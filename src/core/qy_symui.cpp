// 秋雨五笔输入法 - 图标输入（符号面板）实现
#include "qy_symui.h"

namespace qy {

void QyTrace(const wchar_t* fmt, ...);  // 定义在 qy_ime.cpp，排障用

static const wchar_t* kClassName = L"QiuYuWubiSymWnd";

// 低级鼠标钩子回调里需要的“主人窗口”（状态条），由装钩子的面板写入。
// 同一时刻整个进程最多只有一个面板装着钩子（面板收起即卸载），所以一个
// 文件级静态变量足够。
static HWND g_hookOwner = nullptr;

// ---- 版面常量 ----
static const int kPad = 10;      // 卡片内边距
static const int kTabH = 26;     // 分类标签高度
static const int kTabGap = 8;    // 标签与格子之间的间距
static const int kCellH = 34;    // 单个符号格高度
static const int kWidth = 404;   // 面板宽（固定，切换分类不左右跳）

// ---- 符号表 ----
static const wchar_t* kCat0[] = {  // 常用标点
    L"，", L"。", L"、", L"；", L"：", L"？", L"！", L"…", L"—", L"～",
    L"·",  L"《", L"》", L"「", L"」", L"『", L"』", L"【", L"】", L"〈",
    L"〉", L"（", L"）", L"〔", L"〕", L"“", L"”", L"‘", L"’", L"％",
    L"＃", L"＆", L"＊", L"￥", L"＠", L"×", L"÷", L"±", L"Φ", L"§", L"¶"};

static const wchar_t* kCat1[] = {  // 数学
    L"＋", L"－", L"×", L"÷", L"＝", L"≠", L"≈", L"＜", L"＞", L"≤",
    L"≥", L"±", L"∝", L"∞", L"∑", L"∏", L"√", L"∟", L"∠", L"⌒",
    L"⊙", L"∫", L"∴", L"∵", L"∧", L"∨", L"∩", L"∪", L"∈", L"∶",
    L"°", L"′", L"″", L"‰", L"≡", L"∽", L"⊥", L"∥", L"Φ", L"⌀",
    L"％"};

static const wchar_t* kCat2[] = {  // 序号
    L"①", L"②", L"③", L"④", L"⑤", L"⑥", L"⑦", L"⑧", L"⑨", L"⑩",
    L"⑴", L"⑵", L"⑶", L"⑷", L"⑸", L"⑹", L"⑺", L"⑻", L"⑼", L"⑽",
    L"⒈", L"⒉", L"⒊", L"⒋", L"⒌", L"⒍", L"⒎", L"⒏", L"⒐", L"⒑",
    L"Ⅰ", L"Ⅱ", L"Ⅲ", L"Ⅳ", L"Ⅴ", L"Ⅵ", L"Ⅶ", L"Ⅷ", L"Ⅸ", L"Ⅹ",
    L"㈠", L"㈡", L"㈢", L"㈣", L"㈤", L"㈥", L"㈦", L"㈧", L"㈨", L"㈩"};

static const wchar_t* kCat3[] = {  // 箭头 / 几何
    L"←", L"↑", L"→", L"↓", L"↔", L"↕", L"↖", L"↗", L"↘", L"↙",
    L"▲", L"▼", L"◀", L"▶", L"◆", L"◇", L"○", L"●", L"◎", L"△",
    L"▽", L"□", L"■", L"☆", L"★", L"※", L"◢", L"◣", L"◤", L"◥",
    L"▁", L"▂", L"▃", L"▄", L"▅", L"▆", L"▇", L"█", L"░", L"▒"};

static const wchar_t* kCat4[] = {  // 符号 / 装饰
    L"※", L"§", L"¶", L"©", L"®", L"™", L"№", L"☆", L"★", L"♪",
    L"♫", L"♭", L"♯", L"♠", L"♣", L"♥", L"♦", L"☀", L"☁", L"☂",
    L"☃", L"☎", L"☏", L"☜", L"☞", L"☺", L"☻", L"♀", L"♂", L"☯",
    // 下面十个补满 4 行（都是 GBK 内的符号，GBK 模式下也能正常上屏）
    L"‖", L"∷", L"∮", L"≌", L"≮", L"≯", L"℃", L"¤", L"〓", L"﹢"};

static const wchar_t* kCat5[] = {  // 颜文字（两列，宽格子）
    L"（＾▽＾）", L"（￣▽￣）", L"(^_^)", L"(T_T)", L"(>_<)", L"(￣ー￣)",
    L"（＠＿＠）", L"（￣︶￣）", L"(＝＿＝)", L"（≧▽≦）", L"(⊙＿⊙)", L"（＞﹏＜）"};

struct SymCat {
    const wchar_t* name;
    const wchar_t* const* items;
    int count;
    int cols;
};

// 列数统一取 11：常用分类有 41 个符号，11 列正好排满 4 行（第 4 行留 3 格空），
// 其余分类 40 个也是 4 行；颜文字 3 列 12 个，同样 4 行 —— 所有分类行数一致，
// 切换分类时面板高度不再变化。
static const SymCat kCats[] = {
    {L"常用", kCat0, (int)(sizeof(kCat0) / sizeof(kCat0[0])), 11},
    {L"数学", kCat1, (int)(sizeof(kCat1) / sizeof(kCat1[0])), 11},
    {L"序号", kCat2, (int)(sizeof(kCat2) / sizeof(kCat2[0])), 13},
    {L"箭头", kCat3, (int)(sizeof(kCat3) / sizeof(kCat3[0])), 11},
    {L"符号", kCat4, (int)(sizeof(kCat4) / sizeof(kCat4[0])), 11},
    {L"颜文字", kCat5, (int)(sizeof(kCat5) / sizeof(kCat5[0])), 3},
};
static const int kCatCount = (int)(sizeof(kCats) / sizeof(kCats[0]));

// 网格行数固定：所有分类都排成同样的行数，切换分类时面板高度不变
// （以前按各分类的条数算行数，3~6 行不等，切换时窗口忽高忽低）。
static const int kRows = 4;

// 编译期校验：各分类的条目数不能超过固定行数能容纳的数量（否则会画到面板外）
constexpr int RowsNeeded(int count, int cols) { return (count + cols - 1) / cols; }
static_assert(RowsNeeded((int)(sizeof(kCat0) / sizeof(kCat0[0])), 11) <= kRows, "常用分类超行");
static_assert(RowsNeeded((int)(sizeof(kCat1) / sizeof(kCat1[0])), 11) <= kRows, "数学分类超行");
static_assert(RowsNeeded((int)(sizeof(kCat2) / sizeof(kCat2[0])), 13) <= kRows, "序号分类超行");
static_assert(RowsNeeded((int)(sizeof(kCat3) / sizeof(kCat3[0])), 11) <= kRows, "箭头分类超行");
static_assert(RowsNeeded((int)(sizeof(kCat4) / sizeof(kCat4[0])), 11) <= kRows, "符号分类超行");
static_assert(RowsNeeded((int)(sizeof(kCat5) / sizeof(kCat5[0])), 3) <= kRows, "颜文字分类超行");

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

int SymbolPanel::ColsOf(int cat) const {
    if (cat < 0 || cat >= kCatCount) return 10;
    return kCats[cat].cols;
}

int SymbolPanel::RowsOf(int cat) const {
    // 所有分类都排成 kRows 行（各分类条数已按行数配平），高度恒定
    (void)cat;
    return kRows;
}

int SymbolPanel::PanelH() const {
    return kPad + kTabH + kTabGap + RowsOf(cat_) * kCellH + kPad;
}

bool SymbolPanel::Create(HINSTANCE hInst) {
    if (hwnd_) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    ::RegisterClassExW(&wc);

    fontTab_ = ::CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                             L"Microsoft YaHei");
    fontIcon_ = ::CreateFontW(-20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                              L"Microsoft YaHei");
    fontIconSmall_ = ::CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, L"Microsoft YaHei");

    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClassName,
                              L"图标输入", WS_POPUP, 0, 0, kWidth, PanelH(), nullptr, nullptr,
                              hInst, this);
    return hwnd_ != nullptr;
}

void SymbolPanel::Destroy() {
    RemoveHook();
    if (hwnd_) {
        ::KillTimer(hwnd_, 1);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (fontTab_) { ::DeleteObject(fontTab_); fontTab_ = nullptr; }
    if (fontIcon_) { ::DeleteObject(fontIcon_); fontIcon_ = nullptr; }
    if (fontIconSmall_) { ::DeleteObject(fontIconSmall_); fontIconSmall_ = nullptr; }
    visible_ = false;
}

void SymbolPanel::ApplySize() {
    if (!hwnd_) return;
    RECT wa{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = kWidth, h = PanelH();
    int x = anchor_.x;
    int y = anchor_.y - h - 8;  // 默认贴在状态条上方
    if (y < wa.top) y = anchor_.y + 38;
    if (x + w > wa.right) x = wa.right - w;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    if (y + h > wa.bottom) y = wa.bottom - h;
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    HRGN rgn = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, 12, 12);
    ::SetWindowRgn(hwnd_, rgn, TRUE);
}

void SymbolPanel::Show(POINT anchor) {
    if (!hwnd_) return;
    anchor_ = anchor;
    dark_ = IsDarkTheme();
    cat_ = 0;
    hoverItem_ = -1;
    hoverTab_ = -1;
    flashItem_ = -1;
    pendingIdx_ = -1;
    ApplySize();
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    visible_ = true;
    shownTick_ = ::GetTickCount();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    ::UpdateWindow(hwnd_);
    InstallHook();
    if (onVisible_) onVisible_();
}

void SymbolPanel::Hide() {
    if (hwnd_ && visible_) {
        RemoveHook();
        ::ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        if (onVisible_) onVisible_();
    }
}

void SymbolPanel::Toggle(POINT anchor) {
    if (visible_)
        Hide();
    else
        Show(anchor);
}

void SymbolPanel::CheckForeground() {
    if (!hwnd_ || !visible_) return;
    HWND fg = fgProbe_ ? fgProbe_() : ::GetForegroundWindow();
    if (!fg) return;  // 拿不到前台（锁屏/安全桌面/无人值守）不动，与状态条一致
    // 设置窗口（qydemo --settings）在前台：状态条留着做实时预览，符号面板是
    // 它的延伸 UI，同样不能收起。否则“点开设置后点符号图标”面板刚弹出，就被
    // 400ms 的前台检查判成“前台是别的进程”而秒关，符号图标点了等于白点。
    if (IsSettingsWindowForeground(fg)) return;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    if (pid != ::GetCurrentProcessId()) Hide();
}

// ---- “点到面板外面就收起” ----
//
// 面板不抢焦点（WS_EX_NOACTIVATE / MA_NOACTIVATE），收不到 WM_KILLFOCUS，
// 系统也不会把“点在别处”的鼠标消息发给一个不拥有焦点的窗口，所以只能
// 自己想办法感知外部点击。历史方案与废弃原因：
//
//   · GetAsyncKeyState 轮询“刚按下过”的边沿标志（0x0001）：
//     点状态条图标弹面板的那一次点击会在系统里留下一个边沿标志，300ms
//     保护期又在读键之前就返回，标志一直没被消费；保护期一过第一次读到
//     它，而鼠标还停在面板外的状态栏上——面板刚出来约 0.4 秒就被误判成
//     “点了外面”自己消失（Win10 计时精度更粗，几乎每次复现）。
//   · 改轮询“当前是否按着”（0x8000）：快速点击（按下 < 60ms）可能整个
//     落在两次轮询之间，点外面收不起来的概率不小。
//
// 最终方案：面板显示期间装 WH_MOUSE_LL 低级鼠标钩子，在鼠标“按下”事件
// 里拿到屏幕坐标精确判断。回调极短（两次 GetWindowRect + PtInRect），
// 不会触发系统对低级钩子的超时移除；点击不拦截，原样传给系统。
void SymbolPanel::InstallHook() {
    if (hook_) return;
    g_hookOwner = owner_;  // 白名单：状态条上的点击由状态条自己处理
    hook_ = ::SetWindowsHookExW(WH_MOUSE_LL, MouseHook, nullptr, 0);
    if (!hook_) QyTrace(L"sym hook install failed err=%lu", (unsigned long)::GetLastError());
}

void SymbolPanel::RemoveHook() {
    if (hook_) {
        ::UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    g_hookOwner = nullptr;
}

LRESULT CALLBACK SymbolPanel::MouseHook(int code, WPARAM w, LPARAM l) {
    if (code >= 0 && (w == WM_LBUTTONDOWN || w == WM_LBUTTONDBLCLK || w == WM_RBUTTONDOWN ||
                      w == WM_XBUTTONDOWN)) {
        // 同一时刻只有正在显示的那个面板装着钩子（收起时即卸载），窗口全局唯一
        HWND panel = ::FindWindowW(L"QiuYuWubiSymWnd", nullptr);
        if (panel) {
            const MSLLHOOKSTRUCT* hs = (const MSLLHOOKSTRUCT*)l;
            RECT pr{};
            ::GetWindowRect(panel, &pr);
            if (!::PtInRect(&pr, hs->pt)) {
                // 一次物理点击常被报成 LBUTTONDOWN + LBUTTONDBLCLK 两条；不同宿主进程
                // 里各自装着的钩子也可能同时投给同一个面板窗口。trace.log 实测出现过
                // 同一毫秒两条 outside click + 两条 hide，面板会被 Hide 两次。
                // 按「同坐标 + 300ms」去抖，快速连点不同位置不受影响。
                static DWORD s_lastTick = 0;
                static POINT s_lastPt{-10000, -10000};
                const DWORD now = ::GetTickCount();
                const bool sameSpot = hs->pt.x >= s_lastPt.x - 3 &&
                                      hs->pt.x <= s_lastPt.x + 3 &&
                                      hs->pt.y >= s_lastPt.y - 3 &&
                                      hs->pt.y <= s_lastPt.y + 3;
                const bool dup = (now - s_lastTick < 300) && sameSpot;
                s_lastTick = now;
                s_lastPt = hs->pt;
                if (dup) return ::CallNextHookEx(nullptr, code, w, l);

                bool inOwner = false;
                if (g_hookOwner) {
                    RECT orr{};
                    if (::GetWindowRect(g_hookOwner, &orr))
                        inOwner = ::PtInRect(&orr, hs->pt) != FALSE;
                }
                // 点在状态条（白名单）上不收起，也不记日志：实测 43 次 outside click
                // 里有 24 次是这种，全是噪音
                if (!inOwner) {
                    QyTrace(L"sym hook outside click -> hide %ld,%ld panel=%ld,%ld %ldx%ld",
                            hs->pt.x, hs->pt.y, pr.left, pr.top, pr.right - pr.left,
                            pr.bottom - pr.top);
                    // 不能在钩子回调里直接 ShowWindow——此刻宿主正在派发这条
                    // 鼠标事件（微信等宿主对此敏感），投递给面板自己的消息
                    // 循环，回到消息循环再收起
                    ::PostMessageW(panel, WM_APP + 22, 0, 0);
                }
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, w, l);
}

int SymbolPanel::HitTest(POINT c, bool* isTab) const {
    if (isTab) *isTab = false;
    if (c.y >= kPad && c.y < kPad + kTabH) {
        int tabW = (kWidth - 2 * kPad) / kCatCount;
        int idx = (c.x - kPad) / (tabW > 0 ? tabW : 1);
        if (idx < 0) idx = -1;
        if (idx >= kCatCount) idx = -1;
        if (isTab) *isTab = true;
        return idx;
    }
    int cols = ColsOf(cat_);
    int contentW = kWidth - 2 * kPad;
    int cellW = contentW / (cols > 0 ? cols : 1);
    int gridW = cellW * cols;
    int x0 = kPad + (contentW - gridW) / 2;
    int gy = kPad + kTabH + kTabGap;
    if (c.y < gy) return -1;
    int row = (c.y - gy) / kCellH;
    int col = (c.x - x0) / (cellW > 0 ? cellW : 1);
    if (c.x < x0 || col < 0 || col >= cols || row < 0) return -1;
    int idx = row * cols + col;
    if (idx < 0 || idx >= kCats[cat_].count) return -1;
    return idx;
}

LRESULT CALLBACK SymbolPanel::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SymbolPanel* self = (SymbolPanel*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        self = (SymbolPanel*)cs->lpCreateParams;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->hwnd_ = h;
        ::SetTimer(h, 1, 400, nullptr);  // 前台归属
        return 0;
    }
    if (!self) return ::DefWindowProcW(h, m, w, l);

    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (w == 1) { self->CheckForeground(); return 0; }
            if (w == 2) {  // 插入反馈（小高亮）结束
                ::KillTimer(h, 2);
                if (self->flashItem_ >= 0) {
                    self->flashItem_ = -1;
                    ::InvalidateRect(h, nullptr, FALSE);
                }
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(h, &ps);
            self->OnPaint(dc);
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            POINT pt{(LONG)(short)LOWORD(l), (LONG)(short)HIWORD(l)};
            bool isTab = false;
            int idx = self->HitTest(pt, &isTab);
            int ni = isTab ? -1 : idx;
            int nt = isTab ? idx : -1;
            if (ni != self->hoverItem_ || nt != self->hoverTab_) {
                self->hoverItem_ = ni;
                self->hoverTab_ = nt;
                ::InvalidateRect(h, nullptr, FALSE);
            }
            // 手型光标只出现在可点的地方
            ::SetCursor(::LoadCursorW(nullptr, (ni >= 0 || nt >= 0) ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_SETCURSOR: {
            POINT pt{};
            ::GetCursorPos(&pt);
            ::ScreenToClient(h, &pt);
            bool isTab = false;
            int idx = self->HitTest(pt, &isTab);
            ::SetCursor(::LoadCursorW(nullptr, (idx >= 0) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONDOWN: {
            POINT pt{(LONG)(short)LOWORD(l), (LONG)(short)HIWORD(l)};
            bool isTab = false;
            int idx = self->HitTest(pt, &isTab);
            if (isTab && idx >= 0) {
                if (self->cat_ != idx) {
                    self->cat_ = idx;
                    self->hoverItem_ = -1;
                    self->ApplySize();
                    ::InvalidateRect(h, nullptr, TRUE);
                }
            } else if (idx >= 0 && self->onPick_) {
                // 关键：不能在这里直接插入。此刻仍在本窗口的鼠标消息里，
                // 宿主（微信 / Chromium 内核）正在等这条消息返回，
                // 这时候去跑 TSF 编辑会话会重入宿主的输入处理，直接把宿主搞崩。
                // 先把要输出的符号记下来，投递一条自己的消息，等鼠标消息
                // 处理完、回到消息循环时再插入。
                self->pending_ = kCats[self->cat_].items[idx];
                self->pendingIdx_ = idx;
                ::PostMessageW(h, WM_APP + 21, 0, 0);
            }
            return 0;
        }
        case WM_APP + 21: {  // 延后插入：此时已离开鼠标消息
            std::wstring s;
            s.swap(self->pending_);
            if (!s.empty()) {
                // 面板插入后不收起：符号面板是“多点几个”的地方，关掉一次就得
                // 重新点笑脸重开（尤其 Word 里特别麻烦）。收起时机交给：
                // 点面板外面 / 再点一次笑脸 / 开始打字 / 前台换了别的程序。
                auto cb = self->onPick_;  // 拷贝一份，回调里可能销毁面板
                if (cb) cb(s);
                self->flashItem_ = self->pendingIdx_;
                ::SetTimer(h, 2, 130, nullptr);
                ::InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_APP + 22: {  // 钩子判定“点在面板外”后投递过来的收起请求
            // 同一个面板窗口可能收到多条重复的收起请求，只有真的还显示着才记日志、
            // 才 Hide（Hide 自身幂等，但日志不该重复刷）
            if (self->visible()) {
                QyTrace(L"sym hide by outside click");
                self->Hide();
            }
            return 0;
        }
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

void SymbolPanel::OnPaint(HDC dc) {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    HDC mem = ::CreateCompatibleDC(dc);
    HBITMAP bmp = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);
    ::SetBkMode(mem, TRANSPARENT);

    COLORREF bgCol = dark_ ? RGB(44, 46, 50) : RGB(253, 253, 254);
    COLORREF bdCol = dark_ ? RGB(78, 80, 86) : RGB(206, 208, 214);
    COLORREF txtCol = dark_ ? RGB(232, 234, 238) : RGB(36, 40, 46);
    COLORREF dimCol = dark_ ? RGB(150, 155, 165) : RGB(120, 126, 136);
    COLORREF tabSel = dark_ ? RGB(46, 96, 168) : RGB(38, 104, 205);
    COLORREF hovCol = dark_ ? RGB(60, 64, 72) : RGB(228, 236, 248);

    HBRUSH bg = ::CreateSolidBrush(bgCol);
    HPEN bd = ::CreatePen(PS_SOLID, 1, bdCol);
    HBRUSH ob = (HBRUSH)::SelectObject(mem, bg);
    HPEN op = (HPEN)::SelectObject(mem, bd);
    ::RoundRect(mem, 0, 0, rc.right, rc.bottom, 12, 12);
    ::SelectObject(mem, ob);
    ::SelectObject(mem, op);
    ::DeleteObject(bg);
    ::DeleteObject(bd);

    // 分类标签
    int tabW = (rc.right - 2 * kPad) / kCatCount;
    ::SelectObject(mem, fontTab_);
    for (int i = 0; i < kCatCount; ++i) {
        RECT tr{kPad + i * tabW + 2, kPad, kPad + (i + 1) * tabW - 2, kPad + kTabH};
        if (i == cat_) {
            HBRUSH sb = ::CreateSolidBrush(tabSel);
            HBRUSH osb = (HBRUSH)::SelectObject(mem, sb);
            HPEN onp = (HPEN)::SelectObject(mem, ::GetStockObject(NULL_PEN));
            ::RoundRect(mem, tr.left, tr.top, tr.right, tr.bottom, 8, 8);
            ::SelectObject(mem, osb);
            ::SelectObject(mem, onp);
            ::DeleteObject(sb);
            ::SetTextColor(mem, RGB(255, 255, 255));
        } else if (i == hoverTab_) {
            HBRUSH sb = ::CreateSolidBrush(hovCol);
            HBRUSH osb = (HBRUSH)::SelectObject(mem, sb);
            HPEN onp = (HPEN)::SelectObject(mem, ::GetStockObject(NULL_PEN));
            ::RoundRect(mem, tr.left, tr.top, tr.right, tr.bottom, 8, 8);
            ::SelectObject(mem, osb);
            ::SelectObject(mem, onp);
            ::DeleteObject(sb);
            ::SetTextColor(mem, txtCol);
        } else {
            ::SetTextColor(mem, dimCol);
        }
        ::DrawTextW(mem, kCats[i].name, (int)::wcslen(kCats[i].name), &tr,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 符号格子
    int cols = ColsOf(cat_);
    int contentW = rc.right - 2 * kPad;
    int cellW = contentW / (cols > 0 ? cols : 1);
    int gridW = cellW * cols;
    int x0 = kPad + (contentW - gridW) / 2;
    int gy = kPad + kTabH + kTabGap;
    int n = kCats[cat_].count;
    for (int i = 0; i < n; ++i) {
        int row = i / cols, col = i % cols;
        RECT cell{x0 + col * cellW, gy + row * kCellH, x0 + (col + 1) * cellW,
                  gy + (row + 1) * kCellH};
        if (i == hoverItem_ || i == flashItem_) {
            HBRUSH sb = ::CreateSolidBrush(i == flashItem_
                                               ? (dark_ ? RGB(58, 92, 148) : RGB(198, 220, 248))
                                               : hovCol);
            RECT rr = cell;
            ::InflateRect(&rr, -2, -2);
            HBRUSH osb = (HBRUSH)::SelectObject(mem, sb);
            HPEN onp = (HPEN)::SelectObject(mem, ::GetStockObject(NULL_PEN));
            ::RoundRect(mem, rr.left, rr.top, rr.right, rr.bottom, 6, 6);
            ::SelectObject(mem, osb);
            ::SelectObject(mem, onp);
            ::DeleteObject(sb);
        }
        const wchar_t* s = kCats[cat_].items[i];
        int len = (int)::wcslen(s);
        bool one = (len <= 1);
        ::SelectObject(mem, one ? fontIcon_ : fontIconSmall_);
        ::SetTextColor(mem, (i == hoverItem_ || i == flashItem_)
                                ? (dark_ ? RGB(180, 214, 255) : RGB(24, 76, 150))
                                : txtCol);
        ::DrawTextW(mem, s, len, &cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
}

}  // namespace qy
