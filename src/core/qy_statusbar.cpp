// 秋雨五笔输入法 - 输入法状态条实现
//
// 外观要点：
//   · 逻辑坐标固定 172x36，按系统 DPI 放大到物理像素，高分屏不糊
//   · 分层窗口（UpdateLayeredWindow 逐像素透明）：圆角不是硬边裁剪，
//     而是按 4x4 子采样覆盖度算 alpha，边缘与桌面平滑混合（无锯齿）
//   · 卡片式底：竖向渐变 + 1px 圆角描边 + 细微内高光
//   · 中/英 用胶囊色块（渐变），其余文字与图标用统一的深灰
//   · 图标在 4 倍超采样缓冲里绘制，缩小后边缘平滑（无锯齿）
//   · 柔和投影：窗口四周留白，按“到卡片圆角矩形的带符号距离”解析算半透明
//     暗色（不画进缓冲，降采样合成时与卡片做 over 合成），白底上也能看清边界
//   · 鼠标悬停 / 按下有柔和底色反馈
#include "qy_statusbar.h"

#include <cmath>

namespace qy {

static const wchar_t* kClassName = L"QiuYuWubiStatusWnd";

// ---- 逻辑尺寸（96 DPI 基准） ----
static const int kW = 172;
static const int kH = 36;
static const int kRadius = 12;  // 卡片圆角
// 各分区右边界：中/英 · 五/拼 · 全/半 · 图标输入 · 设置
static const int kEdges[6] = {0, 43, 74, 105, 138, kW};

// 阴影留白：窗口在卡片四周各留 kPad 逻辑像素，用来画柔和投影（layered
// 窗口不能用 CS_DROPSHADOW，必须自己按圆角算 alpha，且要把客户区扩出来）。
static const int kPad = 10;
static const int kW2 = kW + 2 * kPad;  // 含阴影留白的窗口逻辑宽
static const int kH2 = kH + 2 * kPad;  // 含阴影留白的窗口逻辑高

static const int kSS = 4;  // 超采样倍数

// 判定“这是拖动而不是点击”的位移阈值（物理像素）。按下后位移超过它就不再
// 触发点击功能；阈值内仍按单击处理，保证小抖动也能正常点按钮。
static const int kDragSlop = 4;

void QyTrace(const wchar_t* fmt, ...);  // 定义在 qy_ime.cpp，排障用

// 自测工具（sbtest2/3/4）单独编译、不链接 qy_ime.cpp，用 QY_SB_NO_TRACE
// 把日志降级成空实现，避免产生未定义符号；输入法 DLL 里走真正的 QyTrace。
#ifdef QY_SB_NO_TRACE
static void SbTrace(const wchar_t*, ...) {}
#else
#define SbTrace QyTrace
#endif

// GHO 精简版 Win10 常关掉主题服务 / 视觉特效，分层窗口的像素会直接落到
// 屏幕 DC 上，旧位置、旧帧不会被桌面自动擦掉 —— 表现为桌面留黑影、F5 也
// 刷不掉。窗口隐藏 / 拖动结束后主动让这块桌面矩形重绘一次，把残影冲掉。
static void RefreshDesktopRect(const RECT& rc) {
    HWND desk = ::GetDesktopWindow();
    if (!desk) return;
    RECT r = rc;
    ::InflateRect(&r, 2, 2);
    ::RedrawWindow(desk, &r, nullptr,
                   RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

// 桌面合成是否健康。Win8+ 的 DWM 恒开，但 GHO 系统可能禁用 Themes 服务；
// 两者任一不成立就按“合成不可靠”处理：不做整体半透明，避免鼠标进出时
// SourceConstantAlpha 在 160/255 之间反复整窗提交。
static bool DesktopCompositionOk() {
    BOOL dwmOn = FALSE;
    typedef HRESULT(WINAPI* DwmIsCompositionEnabledFn)(BOOL*);
    HMODULE d = ::LoadLibraryW(L"dwmapi.dll");
    DwmIsCompositionEnabledFn fn =
        d ? (DwmIsCompositionEnabledFn)(void*)::GetProcAddress(d, "DwmIsCompositionEnabled")
          : nullptr;
    HRESULT hr = fn ? fn(&dwmOn) : E_FAIL;
    if (d) ::FreeLibrary(d);

    BOOL themeOn = FALSE;
    typedef BOOL(WINAPI* IsThemeActiveFn)();
    HMODULE u = ::LoadLibraryW(L"uxtheme.dll");
    IsThemeActiveFn ita =
        u ? (IsThemeActiveFn)(void*)::GetProcAddress(u, "IsThemeActive") : nullptr;
    if (ita) themeOn = ita();
    if (u) ::FreeLibrary(u);

    return SUCCEEDED(hr) && dwmOn && themeOn;
}

static int Clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// 感知亮度：自定义颜色后判断“底深还是底浅”，据此决定文字黑白
static int Luma(COLORREF c) {
    return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
}

static COLORREF RgbLerp(COLORREF a, COLORREF b, int t) {  // t: 0..256
    int r = GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / 256;
    int g = GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / 256;
    int bl = GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / 256;
    return RGB(Clamp255(r), Clamp255(g), Clamp255(bl));
}

static bool IsDarkTheme() {
    HKEY k;
    DWORD v = 1, sz = sizeof(v);
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                        KEY_READ, &k) == ERROR_SUCCESS) {
        ::RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&v, &sz);
        ::RegCloseKey(k);
    }
    return v == 0;
}

// ---- 配色 ----
struct Palette {
    COLORREF cardTop, cardBottom, border, highlight;
    COLORREF pill1, pill2;        // 中：蓝色胶囊渐变
    COLORREF pill1Off, pill2Off;  // 英：灰色胶囊渐变
    COLORREF text, textDim, accent, hover, pressed, divider;
    COLORREF pillText;  // 中/英 胶囊上的字（浅色胶囊要用深字）
};

static Palette MakePalette(bool dark, const StatusSkin& skin) {
    Palette p{};
    if (!dark) {
        p.cardTop = RGB(253, 253, 255);
        p.cardBottom = RGB(240, 243, 248);
        p.border = RGB(216, 221, 230);
        p.highlight = RGB(255, 255, 255);
        p.pill1 = RGB(96, 152, 250);
        p.pill2 = RGB(40, 92, 206);
        p.pill1Off = RGB(178, 184, 196);
        p.pill2Off = RGB(140, 147, 160);
        p.text = RGB(51, 59, 75);
        p.textDim = RGB(112, 120, 136);
        p.accent = RGB(40, 92, 206);
        p.hover = RGB(237, 240, 246);
        p.pressed = RGB(224, 229, 238);
        p.divider = RGB(226, 230, 238);
    } else {
        p.cardTop = RGB(58, 58, 64);
        p.cardBottom = RGB(42, 42, 48);
        p.border = RGB(78, 78, 88);
        p.highlight = RGB(96, 96, 106);
        p.pill1 = RGB(96, 148, 240);
        p.pill2 = RGB(46, 95, 196);
        p.pill1Off = RGB(112, 117, 128);
        p.pill2Off = RGB(84, 89, 100);
        p.text = RGB(232, 234, 240);
        p.textDim = RGB(172, 177, 188);
        p.accent = RGB(130, 172, 248);
        p.hover = RGB(66, 68, 76);
        p.pressed = RGB(78, 80, 90);
        p.divider = RGB(70, 72, 82);
    }
    // ---- DIY 配色 ----
    if (skin.accent) {
        p.accent = skin.accent;
        p.pill1 = RgbLerp(skin.accent, RGB(255, 255, 255), 60);  // 亮端
        p.pill2 = RgbLerp(skin.accent, RGB(0, 0, 0), 60);        // 暗端
    }
    if (skin.card) {
        if (dark) {
            p.cardTop = RgbLerp(skin.card, RGB(0, 0, 0), 130);
            p.cardBottom = RgbLerp(skin.card, RGB(0, 0, 0), 170);
            p.border = RgbLerp(skin.card, RGB(255, 255, 255), 40);
        } else {
            p.cardTop = RgbLerp(skin.card, RGB(255, 255, 255), 40);
            p.cardBottom = RgbLerp(skin.card, RGB(0, 0, 0), 30);
            p.border = RgbLerp(skin.card, RGB(0, 0, 0), 90);
        }
        p.highlight = RGB(255, 255, 255);
    }

    // ---- 自定义颜色的对比度自适应 ----
    // 调色盘是任意颜色：浅色主题下把底色挑成深蓝、深色主题下挑成米白都会出现
    // “深底深字 / 浅底浅字”，什么都看不见。这里按卡片亮度把文字、分隔线、
    // 悬停底色整体翻过来，强调色与底色太接近时也自动提亮或压暗。
    p.pillText = Luma(p.pill2) > 150 ? RGB(32, 38, 50) : RGB(255, 255, 255);
    if (skin.card) {
        const int lc = (Luma(p.cardTop) + Luma(p.cardBottom)) / 2;
        if (lc < 128) {  // 自定义成了深色底 → 用浅色字
            p.text = RGB(236, 238, 244);
            p.textDim = RGB(178, 183, 194);
            p.hover = RgbLerp(p.cardBottom, RGB(255, 255, 255), 34);
            p.pressed = RgbLerp(p.cardBottom, RGB(255, 255, 255), 58);
            p.divider = RgbLerp(p.cardBottom, RGB(255, 255, 255), 26);
            p.highlight = RgbLerp(p.cardTop, RGB(255, 255, 255), 60);
        } else {  // 浅色底 → 用深色字
            p.text = RGB(48, 55, 70);
            p.textDim = RGB(118, 125, 140);
            p.hover = RgbLerp(p.cardBottom, RGB(0, 0, 0), 14);
            p.pressed = RgbLerp(p.cardBottom, RGB(0, 0, 0), 28);
            p.divider = RgbLerp(p.cardBottom, RGB(0, 0, 0), 34);
        }
    }
    if (skin.accent) {
        const int la = Luma(p.accent);
        const int lb = (Luma(p.cardTop) + Luma(p.cardBottom)) / 2;
        if (la - lb > -80 && la - lb < 80) {  // 与底色太接近 → 往反方向拉
            p.accent = (la >= lb) ? RgbLerp(p.accent, RGB(0, 0, 0), 110)
                                  : RgbLerp(p.accent, RGB(255, 255, 255), 110);
        }
    }

    // ---- 经典皮肤（参考微软默认五笔）：纯色平涂、黑色细描边、无渐变 ----
    // 放在 DIY 覆盖之后，这样用户自己选的强调色 / 底色仍然生效，只是不再做
    // 上下渐变，边框统一换成黑色细线；阴影与圆角在 Render() 里按 shape 关掉。
    if (IsClassicSkin(skin.shape)) {
        p.cardBottom = p.cardTop;          // 纯色平涂
        p.highlight = p.cardTop;           // 不画顶部内高光
        p.border = RGB(0, 0, 0);           // 四周黑色细线
        p.divider = dark ? RGB(120, 120, 128) : RGB(150, 150, 158);
        p.pill2 = p.pill1;                 // 胶囊也去渐变，改纯色直角块
        p.pill2Off = p.pill1Off;
        p.pillText = Luma(p.pill1) > 150 ? RGB(20, 20, 24) : RGB(255, 255, 255);
    }
    return p;
}


#ifdef QY_SB_DEBUG_DUMP
// 仅自检构建（-DQY_SB_DEBUG_DUMP）才有：把还没提交的分层帧原样落盘，
// 桌面被锁屏 / 远程会话截不到图时，靠它验证配色与文字对比度。
static void DumpFrame(int w, int h, const void* bits) {
    wchar_t path[MAX_PATH]{};
    if (!::GetEnvironmentVariableW(L"QY_SB_DUMP", path, MAX_PATH)) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    const DWORD img = (DWORD)w * (DWORD)h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + img;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = img;
    HANDLE f = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    ::WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
    ::WriteFile(f, &ih, sizeof(ih), &wr, nullptr);
    ::WriteFile(f, bits, img, &wr, nullptr);
    ::CloseHandle(f);
}
#endif

// ---- 基础绘制（均为超采样坐标） ----
static void FillVGradient(HDC dc, int x, int y, int w, int h, COLORREF top, COLORREF bottom) {
    if (h <= 0 || w <= 0) return;
    for (int i = 0; i < h; ++i) {
        int t = (h > 1) ? i * 256 / (h - 1) : 0;
        HBRUSH br = ::CreateSolidBrush(RgbLerp(top, bottom, t));
        RECT r{x, y + i, x + w, y + i + 1};
        ::FillRect(dc, &r, br);
        ::DeleteObject(br);
    }
}

static void FillHGradient(HDC dc, int x, int y, int w, int h, COLORREF left, COLORREF right) {
    if (h <= 0 || w <= 0) return;
    for (int i = 0; i < w; ++i) {
        int t = (w > 1) ? i * 256 / (w - 1) : 0;
        HBRUSH br = ::CreateSolidBrush(RgbLerp(left, right, t));
        RECT r{x + i, y, x + i + 1, y + h};
        ::FillRect(dc, &r, br);
        ::DeleteObject(br);
    }
}

static void FillRound(HDC dc, int x, int y, int w, int h, int radius, COLORREF col) {
    HRGN rgn = ::CreateRoundRectRgn(x, y, x + w + 1, y + h + 1, radius * 2, radius * 2);
    int saved = ::SaveDC(dc);
    ::SelectClipRgn(dc, rgn);
    HBRUSH br = ::CreateSolidBrush(col);
    RECT r{x, y, x + w, y + h};
    ::FillRect(dc, &r, br);
    ::DeleteObject(br);
    ::RestoreDC(dc, saved);
    ::DeleteObject(rgn);
}

static void FillRoundGradient(HDC dc, int x, int y, int w, int h, int radius, COLORREF c1,
                              COLORREF c2, bool vertical) {
    HRGN rgn = ::CreateRoundRectRgn(x, y, x + w + 1, y + h + 1, radius * 2, radius * 2);
    int saved = ::SaveDC(dc);
    ::SelectClipRgn(dc, rgn);
    if (vertical)
        FillVGradient(dc, x, y, w, h, c1, c2);
    else
        FillHGradient(dc, x, y, w, h, c1, c2);
    ::RestoreDC(dc, saved);
    ::DeleteObject(rgn);
}

static void StrokeRound(HDC dc, int x, int y, int w, int h, int radius, int penWidth,
                        COLORREF col) {
    HPEN pen = ::CreatePen(PS_SOLID, penWidth, col);
    HPEN open = (HPEN)::SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::RoundRect(dc, x, y, x + w, y + h, radius * 2, radius * 2);
    ::SelectObject(dc, ob);
    ::SelectObject(dc, open);
    ::DeleteObject(pen);
}

// 经典皮肤：纯色平涂矩形（直角，不依赖 CreateRoundRectRgn(0) 的退化行为）
static void FillRectFlat(HDC dc, int x, int y, int w, int h, COLORREF col) {
    HBRUSH br = ::CreateSolidBrush(col);
    RECT r{x, y, x + w, y + h};
    ::FillRect(dc, &r, br);
    ::DeleteObject(br);
}

// 经典皮肤：四周黑色细线描边。线画在矩形「内侧」，保证降采样后四条边
// 都留在缓冲里（用 NULL_BRUSH 的 Rectangle 会把右/下线画到边界外）。
static void StrokeRectFlat(HDC dc, int x, int y, int w, int h, int penWidth, COLORREF col) {
    HPEN pen = ::CreatePen(PS_SOLID, penWidth, col);
    HPEN open = (HPEN)::SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    const int inset = penWidth / 2;
    ::Rectangle(dc, x + inset, y + inset, x + w - inset, y + h - inset);
    ::SelectObject(dc, ob);
    ::SelectObject(dc, open);
    ::DeleteObject(pen);
}

// ---- 图标 ----

// 图标输入：笑脸（圆脸 + 双眼 + 上扬嘴）
static void IconSmiley(HDC dc, int cx, int cy, int S, COLORREF col) {
    int r = 8 * S;
    int penW = (int)(1.8f * S + 0.5f);
    HPEN pen = ::CreatePen(PS_SOLID, penW, col);
    HPEN open = (HPEN)::SelectObject(dc, pen);
    HBRUSH ob0 = (HBRUSH)::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Ellipse(dc, cx - r, cy - r, cx + r, cy + r);

    // 眼睛
    HBRUSH solid = ::CreateSolidBrush(col);
    HBRUSH ob = (HBRUSH)::SelectObject(dc, solid);
    ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    int er = (int)(1.15f * S + 0.5f);
    int ex = (int)(3.2f * S + 0.5f);
    int ey = (int)(2.6f * S + 0.5f);
    ::Ellipse(dc, cx - ex - er, cy - ey - er, cx - ex + er, cy - ey + er);
    ::Ellipse(dc, cx + ex - er, cy - ey - er, cx + ex + er, cy - ey + er);

    // 嘴：下半圆弧（GDI 弧线默认逆时针，这里显式设为顺时针才走下弧）
    ::SelectObject(dc, pen);
    ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    int mr = (int)(5.2f * S + 0.5f);
    int oldDir = ::SetArcDirection(dc, AD_CLOCKWISE);
    ::Arc(dc, cx - mr, cy - mr, cx + mr, cy + mr, cx + (int)(4.4f * S), cy + (int)(0.3f * S),
          cx - (int)(4.4f * S), cy + (int)(0.3f * S));
    ::SetArcDirection(dc, oldDir);

    ::SelectObject(dc, ob);
    ::SelectObject(dc, ob0);
    ::SelectObject(dc, open);
    ::DeleteObject(solid);
    ::DeleteObject(pen);
}

// 设置：齿轮（8 齿外轮廓 + 中心孔，路径奇偶填充做镂空）
static void IconGear(HDC dc, int cx, int cy, int S, COLORREF col) {
    const double PI = 3.14159265358979323846;
    POINT pts[64]{};
    int n = 0;
    double rOut = 8.6 * S, rIn = 6.5 * S, hole = 2.9 * S;
    for (int i = 0; i < 8; ++i) {
        double a = i * 45.0 * PI / 180.0;
        double wTop = 13.0 * PI / 180.0;   // 齿顶半角
        double wBase = 21.0 * PI / 180.0;  // 齿根半角
        double seq[4][2] = {
            {a - wBase, rIn}, {a - wTop, rOut}, {a + wTop, rOut}, {a + wBase, rIn}};
        for (int k = 0; k < 4; ++k) {
            pts[n].x = cx + (LONG)std::lround(std::cos(seq[k][0]) * seq[k][1]);
            pts[n].y = cy + (LONG)std::lround(std::sin(seq[k][0]) * seq[k][1]);
            ++n;
        }
    }
    HBRUSH br = ::CreateSolidBrush(col);
    HBRUSH ob = (HBRUSH)::SelectObject(dc, br);
    HPEN op = (HPEN)::SelectObject(dc, ::GetStockObject(NULL_PEN));
    int om = ::SetPolyFillMode(dc, ALTERNATE);

    ::BeginPath(dc);
    ::Polygon(dc, pts, n);
    ::Ellipse(dc, cx - (LONG)hole, cy - (LONG)hole, cx + (LONG)hole + 1, cy + (LONG)hole + 1);
    ::EndPath(dc);
    ::FillPath(dc);

    ::SetPolyFillMode(dc, om);
    ::SelectObject(dc, op);
    ::SelectObject(dc, ob);
    ::DeleteObject(br);
}

// ---- 生命周期 ----
bool StatusBar::Create(HINSTANCE hInst) {
    if (hwnd_) return true;
    HDC screen = ::GetDC(nullptr);
    dpi_ = ::GetDeviceCaps(screen, LOGPIXELSX);
    ::ReleaseDC(nullptr, screen);
    if (dpi_ < 96) dpi_ = 96;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // 不能用 CS_DROPSHADOW：系统给分层窗口画的阴影是按**窗口矩形**生成的，
    // 完全不理会 UpdateLayeredWindow 的 per-pixel alpha 圆角，于是胶囊四角
    // 外面各留一块直角灰方块（右侧/底部连成 L 形硬边），Win10 上尤其明显。
    // 投影改为自己做：窗口四周留 kPad 空白（见 kW2/kH2），Render 里按“到卡片
    // 圆角矩形的带符号距离”解析算羽化 alpha，与卡片 over 合成（白底上也能看清）。
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClassName;
    ::RegisterClassExW(&wc);

    font_ = ::CreateFontW(-L(14), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                          L"Microsoft YaHei UI");
    fontSmall_ = ::CreateFontW(-L(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                               L"Microsoft YaHei UI");

    hwnd_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                              kClassName, L"秋雨输入法状态", WS_POPUP, 0, 0, L(kW2), L(kH2),
                              nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;
    // 圆角不再用 SetWindowRgn 硬边裁剪（那正是锯齿的来源），
    // 改为 Render() 里逐像素算透明度，交给 UpdateLayeredWindow 合成
    return true;
}

void StatusBar::Destroy() {
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (fontSmall_) {
        ::DeleteObject(fontSmall_);
        fontSmall_ = nullptr;
    }
    if (hwnd_) {
        ::KillTimer(hwnd_, 1);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

int StatusBar::L(int logical) const { return ::MulDiv(logical, dpi_, 96); }

// 跨实例统一位置（进程内静态）：能读到配置的宿主写入这里，读不到的
// （explorer / 桌面等跨完整性级别宿主，GHO 常见）再退回跨进程共享内存。
int StatusBar::s_posX_ = -1;
int StatusBar::s_posY_ = -1;
bool StatusBar::s_posInit_ = false;

void StatusBar::SetSharedPos(int x, int y) {
    s_posX_ = x;
    s_posY_ = y;
    s_posInit_ = true;
    ChineseState pos = OpenStatusBarPos();
    if (pos.view) SetSharedStatusBarPos(pos, x, y);
    CloseChineseState(pos);
}

void StatusBar::Place() {
    if (!hwnd_) return;
    int w = L(kW2), h = L(kH2);
    // 工作区按“状态条想待的位置”就近选显示器。以前固定用主屏工作区，
    // 副屏上记住的位置每次显示都会被拉回主屏——多显示器下就是“一显示就跳”
    POINT p{x_ < 0 ? 0 : x_, y_ < 0 ? 0 : y_};
    RECT wa{};
    HMONITOR mon = ::MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (mon && ::GetMonitorInfoW(mon, &mi))
        wa = mi.rcWork;
    else
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = x_, y = y_;
    if (x < 0 || y < 0) {  // 本实例还没有有效位置
        int sx = s_posX_, sy = s_posY_;
        if (!s_posInit_) {
            // 首次：先去跨进程共享内存取（explorer / 桌面这种读不到配置、
            // 跨完整性级别的宿主也能拿到同一个位置，否则会回落默认右下角，
            // 与别的宿主的存储位置交替 → “上下跳动”）。没取到（冷启动早于发布方）
            // 则不置 s_posInit_，下次显示再试，等发布方写了就能拿到。
            ChineseState pos = OpenStatusBarPos();
            int rx = -1, ry = -1;
            if (GetSharedStatusBarPos(pos, rx, ry)) {
                sx = rx;
                sy = ry;
                s_posX_ = rx;
                s_posY_ = ry;
                s_posInit_ = true;
            }
            CloseChineseState(pos);
        }
        if (sx >= 0 && sy >= 0) {  // 用统一的存储位置
            x = sx;
            y = sy;
        } else {  // 真正的首次默认：贴工作区右下角
            x = wa.right - w - 12;
            y = wa.bottom - h - 12;
        }
    }
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    if (x > wa.right - w) x = wa.right - w;
    if (y > wa.bottom - h) y = wa.bottom - h;
    x_ = x;
    y_ = y;
    RECT before{};
    ::GetWindowRect(hwnd_, &before);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x_, y_, w, h, SWP_NOACTIVATE);
    RECT after{};
    if (::GetWindowRect(hwnd_, &after) &&
        (after.left != before.left || after.top != before.top))
        RefreshDesktopRect(before);  // 挪走后旧位置不留黑影
    forceRender_ = true;
}

void StatusBar::SetPos(int x, int y) {
    x_ = x;
    y_ = y;
    if (visible_) Place();
}

// 显示前先把其它进程里的同类状态条关掉，避免多个实例叠在同一位置
static BOOL CALLBACK HideSiblingProc(HWND h, LPARAM lp) {
    HWND self = (HWND)lp;
    if (h == self) return TRUE;
    wchar_t cls[64]{};
    if (::GetClassNameW(h, cls, 64) && ::wcscmp(cls, kClassName) == 0)
        ::ShowWindow(h, SW_HIDE);
    return TRUE;
}

// 进程内“当前负责显示状态条”的界面线程。
// 同一个程序的多个输入线程各有一份 TextService 实例，只能有一个显示状态条，
// 否则会叠成两个；用最近获得焦点的那个线程作为负责方。
static volatile LONG g_activeTid = 0;
static DWORD MyTid() { return ::GetCurrentThreadId(); }
static DWORD ActiveTid() { return (DWORD)::InterlockedCompareExchange(&g_activeTid, 0, 0); }

// 本进程内是否已经有别的状态条在显示（负责线程的窗口还活着）
static BOOL CALLBACK FindVisibleProc(HWND h, LPARAM lp) {
    bool* found = (bool*)lp;
    wchar_t cls[64]{};
    if (!::GetClassNameW(h, cls, 64) || ::wcscmp(cls, kClassName) != 0) return TRUE;
    if (!::IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid == ::GetCurrentProcessId()) {
        *found = true;
        return FALSE;  // 找到一个就够了
    }
    return TRUE;
}

static bool AnyStatusVisibleInThisProc() {
    bool found = false;
    ::EnumWindows(FindVisibleProc, (LPARAM)&found);
    return found;
}

// 前台线程的「焦点控件」所属进程。0 = 拿不到（无焦点 / 跨会话 / 无权限）。
//
// 顶层前台 HWND 的 PID 不等于「输入法在服务谁」：UWP / WinUI 的窗口由
// ApplicationFrameHost 承载（前台 PID 是宿主 B），可真正接收输入的焦点控件
// 是应用进程的 CoreWindow（PID 是 A，也就是 TIP 所在进程）。TextInputHost、
// 部分 Electron 多进程宿主同理。反过来，用户真的切到另一个程序时，焦点
// 控件也属于那个程序。用 GetGUIThreadInfo(0) 拿前台线程的 hwndFocus 再取
// PID，就能即时区分「代理窗口」和「真的切走」，不用靠会滞留的 TSF 标志。
static DWORD WindowProcessId(HWND h) {
    DWORD pid = 0;
    if (h) ::GetWindowThreadProcessId(h, &pid);
    return pid;
}

static DWORD ForegroundFocusPid() {
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    if (!::GetGUIThreadInfo(0, &gti)) return 0;
    HWND h = gti.hwndFocus ? gti.hwndFocus : gti.hwndActive;
    if (!h) return 0;
    return WindowProcessId(h);
}

// 顶层前台窗口的子窗口链里有没有属于指定进程的窗口。
// UWP / WinUI 的应用窗口（CoreWindow）是 ApplicationFrameHost 顶层窗口的
// 子窗口，却属于应用进程 A（TIP 所在进程）。某些系统/焦点配置下
// GetGUIThreadInfo(0) 只给得出宿主 B 的窗口，用这层复核就能认出代理窗口，
// 不会因为前台 PID 是 B 就把本该显示的条收掉。
struct ChildPidCtx { DWORD pid; bool found; };
static BOOL CALLBACK FindChildPid(HWND h, LPARAM lp) {
    ChildPidCtx* c = (ChildPidCtx*)lp;
    DWORD p = 0;
    ::GetWindowThreadProcessId(h, &p);
    if (p == c->pid) { c->found = true; return FALSE; }
    return TRUE;
}
static bool WindowHasChildInMyProcess(HWND top) {
    if (!top) return false;
    ChildPidCtx c{::GetCurrentProcessId(), false};
    ::EnumChildWindows(top, FindChildPid, (LPARAM)&c);
    return c.found;
}

// 前台窗口所在的进程里是否也加载着输入法（存在我们的状态条窗口）。
//
// 「切到别的程序就收起」隐含一个前提：那个程序里会有另一个输入法实例把
// 状态条显示出来。但不少程序里这个前提不成立：UWP / WinUI 的窗口由
// ApplicationFrameHost.exe 承载、Win10 之后部分输入法由 TextInputHost.exe
// 承载、Chromium / Electron 把窗口放在别的进程、跨完整性级别的宿主等，
// 都会出现「TIP 在 A 进程、前台 HWND 却属于 B 进程」。旧逻辑一律判成
// 「切到别的程序」把 A 的状态条收起，而 B 里根本没有实例接管；一旦收起
// 就再没有任何路径把它显示回来 —— 用户看到的就是「在这些程序里状态条
// 自动消失」。所以隐藏前必须先确认对方进程里确实有实例能接管。
// 注意：EnumWindows 连 SW_HIDE 的顶层窗口也会枚举到，这里问的是「有没有
// 实例」而不是「此刻有没有在显示」，正是我们要的。
struct StatusPidQuery { DWORD pid; bool found; };
static BOOL CALLBACK FindStatusInPid(HWND h, LPARAM lp) {
    StatusPidQuery* q = (StatusPidQuery*)lp;
    wchar_t cls[64]{};
    if (!::GetClassNameW(h, cls, 64) || ::wcscmp(cls, kClassName) != 0) return TRUE;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid == q->pid) {
        q->found = true;
        return FALSE;
    }
    return TRUE;
}
static bool StatusBarExistsInProcess(DWORD pid) {
    if (!pid) return false;
    StatusPidQuery q{pid, false};
    ::EnumWindows(FindStatusInPid, (LPARAM)&q);
    return q.found;
}

// TSF 活跃心跳宽限：代理窗口场景下 250ms 的 tick 会持续续期；用户真的切走
// 后不再续期，最多再留 2 秒就交回「切到别的程序收起」的老规则。
static const DWORD kImeActiveGraceMs = 2000;

void StatusBar::Show() {
    if (!hwnd_) return;
    ::EnumWindows(HideSiblingProc, (LPARAM)hwnd_);
    ::InterlockedExchange(&g_activeTid, (LONG)MyTid());
    dark_ = (skin_.theme == 2) || (skin_.theme == 0 && IsDarkTheme());
    Place();
    forceRender_ = true;  // 重新显示后必须补一帧，分层窗口表面可能已被系统清掉
    SbTrace(L"sb show");
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    visible_ = true;
}

void StatusBar::Hide() {
    if (hwnd_ && visible_) {
        RECT rc{};
        ::GetWindowRect(hwnd_, &rc);
        SbTrace(L"sb hide");
        ::ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        // 若本实例正是“本进程负责显示”的那个，收起时释放负责权，
        // 让同进程别的上下文实例（onwer==本进程时）能接管，避免谁都不显示。
        if (ActiveTid() == MyTid())
            ::InterlockedCompareExchange(&g_activeTid, 0, (LONG)MyTid());
        RefreshDesktopRect(rc);  // 收起后把这块桌面重绘一次，冲掉残影
    }
}

void StatusBar::SetEnabled(bool en) {
    enabled_ = en;
    if (!en) Hide();
}

// 前台归属检查：既负责“切到别的程序就收起”，也负责“切回来自己恢复”。
//
// 判断依据有两条，缺一不可：
//   1) 前台窗口是不是**本进程**的。这里必须用进程而不是线程：
//      记事本、微信、WinUI/UWP 这类应用的编辑区有自己的输入线程，
//      TSF 在那个线程激活输入法，而前台窗口属于另一个线程。
//      早先按线程比较，结果在记事本里一打字就被判成“不是我的”，
//      状态条 250ms 后自己消失了。
//   2) 窗口真实是否可见（IsWindowVisible），而不是内部标志：
//      别的输入法实例显示状态条时会直接把同类窗口 SW_HIDE，
//      此时内部标志还是 true，只看标志就永远恢复不了。
void StatusBar::CheckForeground() {
    if (!hwnd_ || !enabled_) return;
    HWND fg = fgProbe_ ? fgProbe_() : ::GetForegroundWindow();
    if (!fg) return;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);  // 第二参数才是进程 ID
    bool real = ::IsWindowVisible(hwnd_) != FALSE;

#ifdef QY_SB_TRACE
    {
        FILE* f = fopen("build/sb_trace.txt", "a");
        if (f) {
            fprintf(f, "sameProc=%d real=%d probe=%d fgpid=%lu mypid=%lu act=%lu my=%lu\n",
                    (int)(pid == ::GetCurrentProcessId()), (int)real, fgProbe_ ? 1 : 0, pid,
                    ::GetCurrentProcessId(), ActiveTid(), MyTid());
            fclose(f);
        }
    }
#endif

    if (pid != ::GetCurrentProcessId()) {  // 前台是别的程序 → 收起
        // 例外：前台是“设置”窗口（qydemo --settings）。用户正在调状态条的
        // DIY 配色，条必须留在屏幕上做实时预览，否则一进设置就消失，改了什么
        // 都看不见。
        // 谁能显示由 preview.pid 决定（设置窗口启动时记下的“前台进程”）：
        // 后台往往有好几个进程都加载着输入法、各有一份状态条，若每个实例都
        // 自己 Show，Show 里的 HideSiblingProc 会把别人 SW_HIDE 掉，画面会闪。
        if (IsSettingsWindowForeground(fg)) {
            const DWORD owner = ReadPreviewPid();
            // 多实例互斥：设置前台时可能有多个输入法实例（同进程多 TSF 上下文、
            // 或不同进程）都试图显示状态条做预览。GHO 这种一个进程里挂多个输入
            // 上下文的环境尤为常见——它们全都满足 owner==本进程PID，会各自 Show()
            // 并 HideSiblingProc 把对方 SW_HIDE 掉，于是每 250ms 交替：画面闪烁、
            // 甚至两三条状态条重叠。这里只允许“本进程当前负责显示（g_activeTid）
            // 的那个实例”显示，其余一律让位（即便之前可见也收起）。
            const DWORD active = ActiveTid();
            const bool iAmActive = (active == 0 || active == MyTid());
            if (owner == 0) {  // 设置窗口还没登记归属：只让负责方继续显示
                if (iAmActive) {
                    if (!real) Show();
                    visible_ = true;
                } else if (real) {
                    Hide();
                }
                return;
            }
            if (owner == ::GetCurrentProcessId()) {  // 本进程就是预览归属方
                if (iAmActive) {
                    if (!real) Show();               // 从别的程序切回来时恢复
                    visible_ = true;
                } else if (real) {
                    Hide();                          // 同进程里别的实例才是负责方，让位
                }
                return;
            }
            if (real) {  // 非归属进程：让位，不跟设置窗口抢显示
                RECT r{};
                ::GetWindowRect(hwnd_, &r);
                SbTrace(L"sb hide (settings owner elsewhere)");
                ::ShowWindow(hwnd_, SW_HIDE);
                visible_ = false;
                RefreshDesktopRect(r);
            }
            return;
        }
        // 隐藏前必须确认「那边有实例能接管」或「用户确实切走了」。
        //
        // 旧逻辑只看 PID：前台 HWND 不属于本进程就收起。在 UWP / WinUI
        // （ApplicationFrameHost 承载窗口）、TextInputHost 承载输入法、
        // Chromium / Electron 多进程窗口、跨完整性级别宿主里，TIP 在 A
        // 进程而前台 HWND 在 B 进程，B 里根本没有我们的实例；A 一收起就
        // 再没有路径恢复，表现就是「在这些程序里状态条自动消失」。
        //
        // 上一版用「TSF 活跃心跳」保显示，但 tick 里按 _foreground 无条件
        // 续期；_foreground 默认 true、切窗口时 TSF 未必发 OnSetFocus(FALSE)，
        // 心跳就永不过期 → 切到任何没有实例接管的程序，条都再也不收起。
        // 现在以「前台焦点控件所属进程」为即时真值：
        //   ① 明确属于别的进程 → 用户真的切走了，立刻收起（不再被心跳拖住）；
        //   ② 明确属于本进程     → 代理窗口，保持显示；
        //   ③ 拿不到焦点 PID(0) → 退回心跳兜底，宽限一过照旧收起。
        // 心跳只由真实输入活动（按键 / 焦点回到本实例 / 刚开启状态条）注入，
        // 自检工具不注入时 imeActiveTick_ 恒为 0，仍走原来的收起规则。
        const DWORD now = ::GetTickCount();
        // 自检注入了「假前台窗口」时，焦点信息也必须从那个窗口推导，不能掺
        // 真实系统焦点（测试进程自己往往正持有焦点，会判成代理窗口而不收起）。
        const DWORD focusPid = focusPidProbe_ ? focusPidProbe_()
                               : (fgProbe_ ? WindowProcessId(fg) : ForegroundFocusPid());
        const bool fgCanTakeOver = StatusBarExistsInProcess(pid);
        const bool imeStillActive =
            imeActiveTick_ != 0 && (now - imeActiveTick_) < kImeActiveGraceMs;
        // 焦点控件属于本进程，或前台壳窗口里挂着本进程的子窗口（UWP 代理），
        // 都算「输入法仍在服务当前输入」。子窗口树只在换前台 / 满 1 秒时复核；
        // 自检模式不做这层复核，判定完全由注入值决定。
        if (!fgProbe_ && (fg != lastFgHwnd_ || now - lastProxyTick_ > 1000)) {
            lastFgHwnd_ = fg;
            lastProxyTick_ = now;
            lastProxyChild_ = WindowHasChildInMyProcess(fg);
        }
        const bool focusIsMine =
            (focusPid == ::GetCurrentProcessId()) || (!fgProbe_ && lastProxyChild_);
        // focusIsMine 优先（即时且可靠）；焦点信息完全拿不到时才退回心跳兜底。
        const bool keepVisible =
            !fgCanTakeOver && (focusIsMine || (focusPid == 0 && imeStillActive));
        // 250ms 一拍，日志限流到 1 秒一条，别把 trace.log 刷爆
        static DWORD s_lastFgLog = 0;
        if (now - s_lastFgLog > 1000) {
            s_lastFgLog = now;
            SbTrace(L"sb fg: pid=%lu focus=%lu takeover=%d active=%d keep=%d", pid, focusPid,
                    fgCanTakeOver ? 1 : 0, imeStillActive ? 1 : 0, keepVisible ? 1 : 0);
        }
        if (keepVisible) {
            if (real) {
                visible_ = true;  // 本来就在显示，什么都别动
                return;
            }
            // 被 HideSiblingProc 之类误关掉：只要同进程里没有别的线程在显示
            // （g_activeTid 负责方），就由本实例补一帧。跨进程的旧实例由
            // Show() 里的 HideSiblingProc 统一收掉——不能因为「系统里另有
            // 一条状态条」就让位，那条可能属于已经失焦的进程，让位反而让
            // 真正活跃的输入法一直没有条。
            if (!AnyStatusVisibleInThisProc() &&
                (ActiveTid() == 0 || ActiveTid() == MyTid())) {
                SbTrace(L"sb restore (proxy fg pid=%lu)", pid);
                Show();
            }
            return;
        }

        if (real) {
            RECT r{};
            ::GetWindowRect(hwnd_, &r);
            SbTrace(L"sb hide (foreground changed)");
            ::ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
            RefreshDesktopRect(r);
        }
        return;
    }

    // 前台属于本程序
    if (real) {
        visible_ = true;
        ::InterlockedExchange(&g_activeTid, (LONG)MyTid());
        return;
    }

    // 本程序在前台但状态条不可见：看看是不是另一个界面线程已经显示了一份
    DWORD act = ActiveTid();
    if (act != 0 && act != MyTid() && AnyStatusVisibleInThisProc()) return;  // 让它负责

    Show();  // 没人显示 → 由本实例恢复（也顺手接管负责权）
}

void StatusBar::Update(const StatusInfo& s) {
    const bool chineseChanged = s.chinese != info_.chinese;
    bool changed = chineseChanged || s.mode != info_.mode ||
                   s.fullWidth != info_.fullWidth || s.symbols != info_.symbols;
    // 诊断：把"中/英真的翻了"和"只是画面在闪"分开。下次复现时看 trace.log
    // 有没有成对的 sb info chinese=0/1，就能确定是状态逻辑还是重绘问题。
    if (chineseChanged)
        SbTrace(L"sb info chinese=%d (was=%d)", s.chinese ? 1 : 0, info_.chinese ? 1 : 0);
    info_ = s;
    if (visible_ && changed) ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void StatusBar::SetSkin(const StatusSkin& s) {
    // 设置窗口前台时每 150ms 轮询一次配置，以前这里无条件 InvalidateRect，
    // 哪怕配色一个字节没变也要整窗重绘一帧。只在皮肤真的变了才重画。
    const bool changed = s.theme != skin_.theme || s.shape != skin_.shape ||
                         s.translucent != skin_.translucent || s.accent != skin_.accent ||
                         s.card != skin_.card;
    skin_ = s;
    dark_ = (s.theme == 2) || (s.theme == 0 && IsDarkTheme());
    if (changed) {
        forceRender_ = true;
        if (visible_) ::InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

int StatusBar::HitRegion(POINT c) const {
    // 物理 → 卡片逻辑：先减去四周阴影留白 kPad（窗口左上角是阴影外沿，
    // 卡片从 kPad 处开始），再按 DPI 换算。
    int x = ::MulDiv(c.x, 96, dpi_) - kPad;
    int y = ::MulDiv(c.y, 96, dpi_) - kPad;
    // 出界一律不算任何分区。以前横向出界会返回“方案”分区，于是窗口外
    // （或拖动后残留的旧坐标）的点击会被当成“切五笔/拼音”。
    if (y < 0 || y >= kH) return -1;
    for (int i = 0; i < 5; ++i)
        if (x >= kEdges[i] && x < kEdges[i + 1]) return i;
    return -1;
}

LRESULT CALLBACK StatusBar::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    StatusBar* self = (StatusBar*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        self = (StatusBar*)cs->lpCreateParams;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->hwnd_ = h;
        ::SetTimer(h, 1, 250, nullptr);  // 监视前台窗口归属（隐藏/恢复都靠它）
        return 0;
    }
    if (!self) return ::DefWindowProcW(h, m, w, l);

    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (w == 1) {
                self->CheckForeground();
                if (self->onTick_) self->onTick_();  // 配置热加载（设置窗口里改配色即时生效）
                return 0;
            }
            if (w == 2) {  // 所属程序失去激活后的一次快速复核
                ::KillTimer(h, 2);
                self->CheckForeground();
                return 0;
            }
            if (w == 3) {  // 鼠标“离开”的去抖：确认真离开才降半透明
                // 打开设置等前台切换会给无焦点的状态条发“虚假”的
                // WM_MOUSELEAVE；若立刻把 mouseInside_ 清 false，alpha 会在
                // 160/255 之间反复整窗提交（单一闪烁）。这里挂 150ms 再确认，
                // 光标其实没出窗口就保住实体；拿不到光标也保守保持实体。
                ::KillTimer(h, 3);
                if (self->mouseInside_) {
                    RECT wr;
                    ::GetWindowRect(h, &wr);
                    POINT p{};
                    bool ok = ::GetCursorPos(&p) != FALSE;
                    bool inside = ok && ::PtInRect(&wr, p);
                    if (inside) {  // 虚假离开：保持实体，重新武装追踪
                        self->mouseInside_ = true;
                        if (!self->tracking_) {
                            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                            if (::TrackMouseEvent(&tme)) self->tracking_ = true;
                        }
                        SbTrace(L"sb mouseleave ignored (still inside)");
                        return 0;
                    }
                    // 拿不到光标 / 光标确在窗外：真离开 → 降回半透明
                    self->mouseInside_ = false;
                    self->hoverCell_ = -1;
                    SbTrace(L"sb mouseleave -> translucent");
                    ::InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            break;
        case WM_ACTIVATEAPP:
            if (!w) {
                // 本程序被切走：先等 180ms（菜单、提示框等瞬时切换不算），
                // 再确认前台是否还是本线程，不是就隐藏
                ::SetTimer(h, 2, 180, nullptr);
            } else {
                // 重新激活：立即复核，立刻把状态条恢复出来
                self->CheckForeground();
            }
            break;
        case WM_PAINT: {
            // 分层窗口的像素由 Render() 通过 UpdateLayeredWindow 提交，
            // 这里只需把无效区标记清掉
            PAINTSTRUCT ps;
            ::BeginPaint(h, &ps);
            ::EndPaint(h, &ps);
            self->Render();
            return 0;
        }
        case WM_SIZE:
            self->Render();  // DPI 变化等尺寸调整后立即重出图
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: {
            // 状态条整条任意位置都能按住拖动；是否算“拖动”由位移决定，
            // 没超过阈值就还是点击，执行对应分区的功能
            POINT pt{(LONG)(short)LOWORD(l), (LONG)(short)HIWORD(l)};
            RECT wr;
            ::GetWindowRect(h, &wr);
            self->dragOldRect_ = wr;  // 拖动结束后用它刷掉旧位置的残影
            self->dragStart_ = pt;
            self->pressOrigin_.x = wr.left;
            self->pressOrigin_.y = wr.top;
            self->pressCursor_.x = wr.left + pt.x;
            self->pressCursor_.y = wr.top + pt.y;
            self->dragMoved_ = 0;
            self->dragging_ = true;
            self->dragActive_ = false;
            self->pressCell_ = self->HitRegion(pt);
            ::InvalidateRect(h, nullptr, FALSE);
            ::SetCapture(h);
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{(LONG)(short)LOWORD(l), (LONG)(short)HIWORD(l)};
            ::KillTimer(h, 3);  // 取消待定的“离开去抖”：鼠标其实还在 / 回来了
            if (!self->mouseInside_) {
                self->mouseInside_ = true;  // 靠近 → 从半透明恢复实体
                ::InvalidateRect(h, nullptr, FALSE);
            }
            if (!self->dragging_) {
                int cell = self->HitRegion(pt);
                if (cell != self->hoverCell_) {
                    self->hoverCell_ = cell;
                    ::InvalidateRect(h, nullptr, FALSE);
                }
                if (!self->tracking_) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                    if (::TrackMouseEvent(&tme)) self->tracking_ = true;
                }
                return 0;
            }
            // 拖动跟随：用【屏幕坐标】量位移。
            //
            // 客户区坐标不能用：SetWindowPos 挪了窗口，客户区原点跟着走，
            // 光标在客户区里的坐标就被“窗口追上来了”抵消掉 —— 慢速拖动
            // （每次只挪 1~2px）时位移永远攒不到阈值，松手就被当成单击，
            // 于是“慢拖还会触发点击功能，快拖反而正常”。改成
            //   光标屏幕坐标 = 当前窗口左上角 + 客户区坐标
            // 位移 = 光标屏幕坐标 - 按下时光标屏幕坐标
            // 就与窗口怎么动无关，单调累加，慢拖也一定能越过阈值。
            // 目标位置写 pressOrigin_ + 总位移（等价于“光标 - 抓取偏移”），
            // 收敛稳定，不会出现来回抖的反馈回路。
            RECT wr;
            ::GetWindowRect(h, &wr);
            int dx = (wr.left + pt.x) - self->pressCursor_.x;
            int dy = (wr.top + pt.y) - self->pressCursor_.y;
            int d2 = dx * dx + dy * dy;
            if (d2 > self->dragMoved_) self->dragMoved_ = d2;
            // 越过阈值即确认进入拖动状态：清掉悬停/按下高亮、换成移动
            // 光标，之后哪怕又拖回原点，松手也只保存位置、不触发点击，
            // 避免“想拖动结果手一抖切了中英”的误触
            if (!self->dragActive_ && self->dragMoved_ >= kDragSlop * kDragSlop) {
                self->dragActive_ = true;
                self->hoverCell_ = -1;
                self->pressCell_ = -1;
                ::InvalidateRect(h, nullptr, FALSE);
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
            }
            if (self->dragActive_)
                ::SetWindowPos(h, HWND_TOPMOST, self->pressOrigin_.x + dx,
                               self->pressOrigin_.y + dy, 0, 0,
                               SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSELEAVE:
            self->tracking_ = false;
            // 不直接降半透明。打开设置等前台切换会发“虚假”的 WM_MOUSELEAVE，
            // 立刻把 mouseInside_ 清 false 会让 alpha 在 160/255 之间闪。挂个短
            // 定时器（id=3），真离开（光标移出窗口）才降；期间若再来
            // WM_MOUSEMOVE（鼠标其实没动），上方进入分支会取消并保住实体。
            if (self->mouseInside_) ::SetTimer(h, 3, 150, nullptr);
            return 0;
        case WM_LBUTTONUP: {
            const int pressCell = self->pressCell_;  // 按下时落在哪个分区
            self->pressCell_ = -1;
            ::InvalidateRect(h, nullptr, FALSE);
            if (!self->dragging_) return 0;
            self->dragging_ = false;
            ::ReleaseCapture();
            POINT pt{(LONG)(short)LOWORD(l), (LONG)(short)HIWORD(l)};
            // 兜底：松手位置离按下的地方已经超过阈值，就一定不是单击。
            // 极端情况下（消息被丢弃、鼠标移得飞快）可能一个 WM_MOUSEMOVE
            // 都没收到，只按 dragActive_ 判定就会把它错当点击 —— 这里再量一次。
            RECT wr;
            ::GetWindowRect(h, &wr);
            int dx = (wr.left + pt.x) - self->pressCursor_.x;
            int dy = (wr.top + pt.y) - self->pressCursor_.y;
            bool farNow = dx * dx + dy * dy >= kDragSlop * kDragSlop;
            bool wasDrag = self->dragActive_ || farNow;
            self->dragActive_ = false;
            if (!wasDrag) {  // 点击状态：执行分区功能
                // 只有“按下的分区”和“松手的分区”是同一个（且都在条内）才算点击。
                // 手一哆嗦滑到隔壁分区或者滑出条外，宁可什么都不做 ——
                // 否则很容易点设置却切了中英/方案。
                int cell = self->HitRegion(pt);
                if (cell >= 0 && cell == pressCell && self->onClick_) self->onClick_(cell);
            } else {  // 拖动状态：只记住新位置，不触发任何点击
                if (farNow)  // 保证窗口停在光标下（漏帧时补一次）
                    ::SetWindowPos(h, HWND_TOPMOST, self->pressOrigin_.x + dx,
                                   self->pressOrigin_.y + dy, 0, 0,
                                   SWP_NOSIZE | SWP_NOACTIVATE);
                ::GetWindowRect(h, &wr);
                self->x_ = wr.left;
                self->y_ = wr.top;
                if (self->onMove_) self->onMove_(self->x_, self->y_);
                // 拖动只在这里刷一次桌面，不逐帧刷（逐帧刷反而更闪）
                if (wr.left != self->dragOldRect_.left || wr.top != self->dragOldRect_.top)
                    RefreshDesktopRect(self->dragOldRect_);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            // 捕获被别的窗口抢走（弹菜单、被隐藏等）：直接结束本次按下状态。
            // 否则 dragging_ 会一直挂着，之后没按键的鼠标移动也会拖着窗口跑。
            if (self->dragging_) {
                // 已经确认是拖动的，把停下来的位置存住，别让状态条下次显示又跳回去
                if (self->dragActive_) {
                    RECT wr;
                    ::GetWindowRect(h, &wr);
                    self->x_ = wr.left;
                    self->y_ = wr.top;
                    if (self->onMove_) self->onMove_(self->x_, self->y_);
                    if (wr.left != self->dragOldRect_.left || wr.top != self->dragOldRect_.top)
                        RefreshDesktopRect(self->dragOldRect_);
                }
                self->dragging_ = false;
                self->dragActive_ = false;
                self->pressCell_ = -1;
                self->dragMoved_ = 0;
                ::InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_RBUTTONUP: {
            POINT pt;
            ::GetCursorPos(&pt);
            if (self->onMenu_) self->onMenu_(pt);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

// 悬停/按下的柔和底色（文字格与图标格共用；中/英 是实心胶囊，不叠底色）
// classic=true 时高亮块也改成直角，跟经典皮肤的矩形分格保持一致。
static void DrawCellHighlight(HDC dc, int S, int cell, bool pressed, const Palette& pal,
                              bool classic) {
    if (cell <= kCellChinese) return;
    int x0 = kEdges[cell], x1 = kEdges[cell + 1];
    int pad = (cell == kCellSymbols || cell == kCellSettings) ? 3 : 2;
    int h = 28;
    int y = (kH - h) / 2;
    FillRound(dc, (x0 + pad) * S, y * S, (x1 - x0 - pad * 2) * S, h * S,
              classic ? 0 : 9 * S, pressed ? pal.pressed : pal.hover);
}

int StatusBar::ComputeLayeredAlpha(bool compositionOk, bool translucent, bool mouseInside,
                                    bool dragging) {
    // 合成不可靠（GHO 精简系统关掉 Themes/DWM）或本就不透明时恒为实体，
    // 不让 alpha 随鼠标进出在 160/255 之间反复整窗提交（那是“画面乱跳”的根）。
    if (compositionOk && translucent && !mouseInside && !dragging) return 160;
    return 255;
}

void StatusBar::Render() {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    const int pw = rc.right, ph = rc.bottom;
    if (pw <= 0 || ph <= 0) return;

    if (!compositionChecked_) {
        compositionChecked_ = true;
        compositionOk_ = DesktopCompositionOk();
    }

    // 外观签名：内容、悬停、半透明、皮肤、尺寸都没变就直接跳过。
    // 设置窗口前台时配置轮询 150ms 一次，鼠标进出还要切半透明，以前每次
    // InvalidateRect 都做 4x 超采样 + 整窗 UpdateLayeredWindow；在合成不稳的
    // GHO 精简 Win10 上就是画面乱跳、桌面留黑影的根源。
    const DWORD key =
        (info_.chinese ? 1u : 0u) | (((DWORD)info_.mode & 3u) << 2) |
        (info_.fullWidth ? 1u : 0u) << 4 | (info_.symbols ? 1u : 0u) << 5 |
        (mouseInside_ ? 1u : 0u) << 6 | (dragging_ ? 1u : 0u) << 7 |
        (dark_ ? 1u : 0u) << 8 | (compositionOk_ ? 1u : 0u) << 9 |
        (skin_.translucent ? 1u : 0u) << 10 |
        (((DWORD)(hoverCell_ + 1) & 0xFu) << 12) |
        (((DWORD)(pressCell_ + 1) & 0xFu) << 16) | (((DWORD)skin_.theme & 0xFu) << 20) |
        (((DWORD)skin_.shape & 3u) << 24);  // 皮肤形状必须进签名，否则切换不会重绘
    if (!forceRender_ && key == lastRenderKey_ && skin_.accent == lastAccent_ &&
        skin_.card == lastCard_ && pw == lastW_ && ph == lastH_)
        return;
    forceRender_ = false;
    lastRenderKey_ = key;
    lastAccent_ = skin_.accent;
    lastCard_ = skin_.card;
    lastW_ = pw;
    lastH_ = ph;

    const Palette pal = MakePalette(dark_, skin_);
    // 经典皮肤：四角直角、四周黑色细线描边、四周无阴影（参考微软默认五笔）。
    // 默认皮肤（shape=0）完全保持原来的圆角 + 渐变 + 柔和投影，一行不动。
    // 彩框皮肤（shape=2）：沿用默认皮肤的圆角胶囊与渐变，只把外框换成彩色
    // 线条并关掉阴影；线色取「中/英」按键（胶囊）的同一套颜色，中文态彩色、
    // 英文态灰色，且随 DIY 强调色 / 浅深主题自动变化。
    const bool classic = IsClassicSkin(skin_.shape);
    const bool outline = IsOutlineSkin(skin_.shape);
    const COLORREF frameCol = SkinFrameColor(info_.chinese, pal.pill2, pal.pill2Off);
    const int S = kSS;
    // 超采样缓冲仍只画「卡片本身」(kW×kH)。留白/阴影不占缓冲：降采样合成时
    // 把卡片源坐标映射到窗口的 kPad 偏移处即可。早期试过用 SetViewportOrgEx
    // 平移卡片绘图，但 FillRoundGradient 里的 CreateRoundRectRgn/SelectClipRgn
    // 用的是设备坐标、不随 viewport 原点变换 → 裁剪区留在 (0,0)，渐变却画到
    // (kPad,kPad)，卡片右下被削掉、露出 DIB 未初始化的黑像素。切勿再用那招。
    const int bw = kW * S, bh = kH * S;

    // ---- 超采样缓冲：背景 + 图标（不透明内容；DIB section 便于直接读像素） ----
    BITMAPINFO sbi{};
    sbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    sbi.bmiHeader.biWidth = bw;
    sbi.bmiHeader.biHeight = -bh;  // 顶部向下
    sbi.bmiHeader.biPlanes = 1;
    sbi.bmiHeader.biBitCount = 32;
    sbi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = ::CreateCompatibleDC(nullptr);
    VOID* sbits = nullptr;
    HBITMAP sbmp = ::CreateDIBSection(sdc, &sbi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HBITMAP sob = (HBITMAP)::SelectObject(sdc, sbmp);
    ::SetBkMode(sdc, TRANSPARENT);
    // 卡片绘图坐标保持原样 (0,0)-(kW*S,kH*S)，不做任何 DC 变换；
    // 「偏移到窗口中央」在下面的降采样合成里用源坐标映射完成。

    // 卡片底：默认皮肤是竖向渐变 + 圆角；经典皮肤是纯色平涂 + 直角
    if (classic) {
        FillRectFlat(sdc, 0, 0, bw, bh, pal.cardTop);
        // 四周黑色细线条：1 逻辑像素（超采样后 S 像素），画在内侧不越界
        StrokeRectFlat(sdc, 0, 0, bw, bh, S, pal.border);
    } else {
        FillRoundGradient(sdc, 0, 0, bw, bh, kRadius * S, pal.cardTop, pal.cardBottom, true);
        if (outline) {
            // 彩框：2 逻辑像素的圆角线条，颜色与中/英按键一致（中文彩色 / 英文灰）。
            // 比默认皮肤那 1px 淡灰边更醒目，且换主题 / 换配色 / 切中英都会跟着变。
            StrokeRound(sdc, 0, 0, bw - 1, bh - 1, kRadius * S, 2 * S, frameCol);
        } else {
            StrokeRound(sdc, 0, 0, bw - 1, bh - 1, kRadius * S, S, pal.border);
        }
    }
    // 顶部内高光：一条极淡的亮线，让卡片有轻微凸起感。经典皮肤是纯平面，
    // 彩框皮肤要让彩色线条干净地围一圈，都不要这条高光。
    if (!classic && !outline) {
        int ins = 8 * S;
        HPEN hl = ::CreatePen(PS_SOLID, 1, pal.highlight);
        HPEN o = (HPEN)::SelectObject(sdc, hl);
        ::MoveToEx(sdc, ins, 1, nullptr);
        ::LineTo(sdc, bw - ins, 1);
        ::SelectObject(sdc, o);
        ::DeleteObject(hl);
    }

    // 悬停 / 按下反馈
    if (pressCell_ >= 0)
        DrawCellHighlight(sdc, S, pressCell_, true, pal, classic);
    else if (hoverCell_ >= 0)
        DrawCellHighlight(sdc, S, hoverCell_, false, pal, classic);

    // 图标区细分隔线
    {
        HPEN sep = ::CreatePen(PS_SOLID, S, pal.divider);
        HPEN o = (HPEN)::SelectObject(sdc, sep);
        for (int i = 3; i <= 4; ++i) {
            ::MoveToEx(sdc, kEdges[i] * S, 12 * S, nullptr);
            ::LineTo(sdc, kEdges[i] * S, (kH - 12) * S);
        }
        ::SelectObject(sdc, o);
        ::DeleteObject(sep);
    }

    // 中/英 胶囊。默认皮肤是圆头胶囊；经典皮肤改成直角小方块，跟整体矩形一致。
    {
        const int px = 3, py = 4, pwL = 38, phL = kH - 8;
        COLORREF c1 = info_.chinese ? pal.pill1 : pal.pill1Off;
        COLORREF c2 = info_.chinese ? pal.pill2 : pal.pill2Off;
        const int rx = px * S, ry = py * S, rw = pwL * S, rh = phL * S;
        if (classic) {
            FillRectFlat(sdc, rx, ry, rw, rh, c1);
            StrokeRectFlat(sdc, rx, ry, rw, rh, S, pal.border);
        } else {
            FillRoundGradient(sdc, rx, ry, rw, rh, (phL / 2) * S, c1, c2, false);
            StrokeRound(sdc, rx, ry, rw - 1, rh - 1, (phL / 2) * S, S,
                        RgbLerp(c1, RGB(255, 255, 255), 80));
        }
    }

    // 图标：图标输入（笑脸）、设置（齿轮）
    // 中文状态 = 全彩（强调色），英文状态 = 灰
    {
        // 笑脸 / 齿轮：中文状态全彩（强调色），英文状态灰
        COLORREF symCol = info_.chinese ? pal.accent : pal.textDim;
        COLORREF gearCol = info_.chinese ? pal.accent : pal.textDim;
        int cy = (kH / 2) * S;
        IconSmiley(sdc, ((kEdges[3] + kEdges[4]) / 2) * S, cy, S, symCol);
        IconGear(sdc, ((kEdges[4] + kEdges[5]) / 2) * S, cy, S, gearCol);
    }

    // ---- 目标：32bpp 自管理 DIB（预乘 alpha），UpdateLayeredWindow 直接提交 ----
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = pw;
    bi.bmiHeader.biHeight = -ph;  // 顶部向下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC mem = ::CreateCompatibleDC(nullptr);
    VOID* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) ::DeleteObject(bmp);
        ::DeleteDC(mem);
        ::SelectObject(sdc, sob);
        ::DeleteObject(sbmp);
        ::DeleteDC(sdc);
        return;
    }
    HBITMAP mob = (HBITMAP)::SelectObject(mem, bmp);
    ::SetBkMode(mem, TRANSPARENT);  // 文字不得带背景块（默认 OPAQUE 会画出白框）

    // 卡片在目标（窗口）物理坐标中的几何（四周留 kPad 阴影边距）
    const float pad = (float)L(kPad);
    const float cX0 = pad, cY0 = pad;
    const float cX1 = pad + (float)L(kW), cY1 = pad + (float)L(kH);
    // 经典皮肤四角为直角：覆盖度判定与阴影 SDF 都按矩形算（cR=0）。
    const float cR = classic ? 0.0f : (float)L(kRadius);
    // 阴影：以卡片圆角矩形为源，按「到它的带符号距离」向外羽化。卡片边缘处
    // 最深、向外 sBlur 线性衰减到 0，正好铺满 kPad 留白并在窗口边界归零。
    // 早期版本是「阴影矩形内部满值 + 最外 5px 羽化」→ 整个留白变成一圈实心
    // 灰框，正是截图里那个粗灰框的由来，勿再回退。
    const float sBlur = (float)L(kPad);
    const int sColR = 20, sColG = 20, sColB = 28;  // 中性偏冷暗色
    // 经典 / 彩框皮肤四周无阴影：最大值设为 0，shadowA 全程返回 0，
    // 留白区完全透明。默认皮肤（shape=0）仍保留柔和投影，一行不动。
    const int SHADOW_MAX = IsShadowlessSkin(skin_.shape) ? 0 : 64;

    // 卡片覆盖度：圆角矩形解析判定 + 4x4 子采样（边缘抗锯齿）
    auto cov_card = [&](int x, int y) -> int {
        int cov = 0;
        for (int sy = 0; sy < S; ++sy) {
            float fy = y + (sy + 0.5f) / S;
            for (int sx = 0; sx < S; ++sx) {
                float fx = x + (sx + 0.5f) / S;
                if (classic) {
                    // 直角矩形：直接范围判定，不碰 cR=0 的浮点圆边界
                    if (fx >= cX0 && fx < cX1 && fy >= cY0 && fy < cY1) ++cov;
                } else {
                    float cx = fx < cX0 + cR ? cX0 + cR : (fx > cX1 - cR ? cX1 - cR : fx);
                    float cy = fy < cY0 + cR ? cY0 + cR : (fy > cY1 - cR ? cY1 - cR : fy);
                    float dx = fx - cx, dy = fy - cy;
                    if (dx * dx + dy * dy <= cR * cR) ++cov;
                }
            }
        }
        return cov * 255 / (S * S);
    };
    // 阴影不透明度：圆角矩形 SDF（带符号距离）。d<=0 在卡片内（会被卡片
    // 覆盖，给满值也无妨），d>0 时按 d/sBlur 线性衰减到 0。
    auto shadowA = [&](int x, int y) -> int {
        float fx = x + 0.5f, fy = y + 0.5f;
        float cx = (cX0 + cX1) * 0.5f, cy = (cY0 + cY1) * 0.5f;
        float hx = (cX1 - cX0) * 0.5f, hy = (cY1 - cY0) * 0.5f;
        float qx = std::fabs(fx - cx) - hx + cR;
        float qy = std::fabs(fy - cy) - hy + cR;
        float ox = qx > 0.0f ? qx : 0.0f;
        float oy = qy > 0.0f ? qy : 0.0f;
        float mx = qx > qy ? qx : qy;
        float d = std::sqrt(ox * ox + oy * oy) + (mx < 0.0f ? mx : 0.0f) - cR;
        if (d <= 0.0f) return SHADOW_MAX;
        if (d >= sBlur) return 0;
        return (int)(SHADOW_MAX * (1.0f - d / sBlur));
    };

    // ---- 手工 4x4 均匀降采样 + 卡片/阴影 over 合成（预乘 alpha） ----
    // 不用 StretchBlt(HALFTONE)：HALFTONE 在 32bpp DIB 目标上会把颜色
    // 抖动成灰棋盘（实测胶囊渐变、底色全被毁掉），均匀平均既保真又抗锯齿。
    const DWORD* ss = (const DWORD*)sbits;
    DWORD* px32 = (DWORD*)bits;
    const int padPx = L(kPad);
    const int cardW = L(kW), cardH = L(kH);
    for (int y = 0; y < ph; ++y) {
        DWORD* row = px32 + (size_t)y * pw;
        // 目标物理坐标 (x,y) 里卡片从 padPx 起；超采样缓冲只有卡片本身
        // (kW*S × kH*S)，所以源坐标必须减掉 padPx 再按比例映射回去。
        // 直接拿窗口坐标 y*S 去索引缓冲 = 越界 + 错位（黑带/图标错位的根）。
        const int syBase = ::MulDiv(y - padPx, bh, cardH);
        for (int x = 0; x < pw; ++x) {
            int cardA = cov_card(x, y);
            int shA = shadowA(x, y);
            int outA = cardA + shA * (255 - cardA) / 255;
            if (outA <= 0) {
                row[x] = 0;  // 完全透明（预乘 0）
                continue;
            }
            // 4x4 平均卡片颜色（阴影区缓冲为 0，仅在 cardA>0 时取用）
            int sb = 0, sg = 0, sr = 0;
            if (cardA > 0) {
                const int sxBase = ::MulDiv(x - padPx, bw, cardW);
                for (int sy = 0; sy < S; ++sy) {
                    int iy = syBase + sy;
                    if (iy < 0) iy = 0;
                    else if (iy >= bh) iy = bh - 1;
                    const DWORD* srow = ss + (size_t)iy * bw;
                    for (int sx = 0; sx < S; ++sx) {
                        int ix = sxBase + sx;
                        if (ix < 0) ix = 0;
                        else if (ix >= bw) ix = bw - 1;
                        DWORD c = srow[ix];
                        sb += c & 0xFF;
                        sg += (c >> 8) & 0xFF;
                        sr += (c >> 16) & 0xFF;
                    }
                }
                int dv = S * S;
                sb /= dv;
                sg /= dv;
                sr /= dv;
            }
            // 阴影在卡片之下：仅在卡片未覆盖处贡献（over 合成）
            // 注意 sb/sg/sr 分别是蓝/绿/红累加值，别搞错通道顺序。
            int shR = (sColR * shA * (255 - cardA)) / (255 * 255);
            int shG = (sColG * shA * (255 - cardA)) / (255 * 255);
            int shB = (sColB * shA * (255 - cardA)) / (255 * 255);
            int r = sr * cardA / 255 + shR;
            int g = sg * cardA / 255 + shG;
            int b = sb * cardA / 255 + shB;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            row[x] = (DWORD)outA << 24 | (DWORD)r << 16 | (DWORD)g << 8 | (DWORD)b;
        }
    }

    // ---- 文字：按物理分辨率绘制，保证清晰（卡片整体偏移 kPad） ----
    ::SelectObject(mem, font_);
    auto drawText = [&](int cell, const wchar_t* s, COLORREF col) {
        ::SetTextColor(mem, col);
        RECT r{L(kPad) + ::MulDiv(kEdges[cell], L(kW), kW), L(kPad),
               L(kPad) + ::MulDiv(kEdges[cell + 1], L(kW), kW), L(kPad) + L(kH)};
        ::DrawTextW(mem, s, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    };
    drawText(kCellChinese, info_.chinese ? L"中" : L"英", pal.pillText);
    // 五/拼/混、全/半：中文状态用强调色（全彩），英文状态转灰
    COLORREF glyphCol = info_.chinese ? pal.accent : pal.textDim;
    drawText(kCellMode,
             info_.mode == Mode::Pinyin ? L"拼" : (info_.mode == Mode::Mixed ? L"混" : L"五"),
             glyphCol);
    drawText(kCellWidth, info_.fullWidth ? L"全" : L"半", glyphCol);

    // ---- 文字写完后重铺 alpha 通道 ----
    // GDI 的 ClearType 文字混合不认识 alpha 通道，会把途经像素的 alpha
    // 写成 0（颜色对、透明度错）。文字都在卡片内部（覆盖度必为 255），这里
    // 只把卡片内部像素的 alpha 重铺为卡片覆盖度；阴影像素（cov_card==0）保持
    // 降采样阶段算出的合成 alpha，绝不能被清成 0。
    for (int y = 0; y < ph; ++y) {
        DWORD* row = px32 + (size_t)y * pw;
        for (int x = 0; x < pw; ++x) {
            int a = cov_card(x, y);
            if (a > 0) row[x] = (row[x] & 0x00FFFFFFu) | (DWORD)a << 24;
        }
    }

#ifdef QY_SB_DEBUG_DUMP
    DumpFrame(pw, ph, bits);
#endif

    // ---- 提交到分层窗口 ----
    // 半透明（可选）：平时半透明，鼠标靠近（或拖动中）恢复实体。
    // 合成不可靠时（GHO 精简系统）一律保持实体：不让 SourceConstantAlpha
    // 随鼠标进出在 160/255 之间反复整窗提交。
    int alpha = ComputeLayeredAlpha(compositionOk_, skin_.translucent, mouseInside_, dragging_);
    POINT src{0, 0};
    SIZE sz{pw, ph};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)alpha, AC_SRC_ALPHA};
    // hdcDst 传 nullptr，让系统自己管理与窗口关联的合成 DC。以前每次都
    // GetDC(nullptr) 把整张位图直接写到屏幕 DC 上，在没有桌面合成的环境里
    // 旧帧 / 旧位置不会被擦除，桌面就留下刷不掉的黑影。
    BOOL ok = ::UpdateLayeredWindow(hwnd_, nullptr, nullptr, &sz, mem, &src, 0, &bf,
                                    ULW_ALPHA);
    if (!sbLogged_) {
        sbLogged_ = true;
        SbTrace(L"sb render: dwm=%d translucent=%d size=%dx%d theme=%d ulw=%d err=%lu",
                compositionOk_ ? 1 : 0, skin_.translucent ? 1 : 0, pw, ph, skin_.theme,
                ok ? 1 : 0, ok ? 0u : (unsigned long)::GetLastError());
    }

    ::SelectObject(mem, mob);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::SelectObject(sdc, sob);
    ::DeleteObject(sbmp);
    ::DeleteDC(sdc);
}

}  // namespace qy
