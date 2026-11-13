// 秋雨五笔输入法 - 演示与设置程序
// 左侧输入区可真实体验五笔/拼音输入与候选窗，右侧进行方案设置
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../core/qy_engine.h"
#include "../core/qy_candui.h"
#include "../core/qy_config.h"
#include "../core/qy_update.h"
#include "../core/qy_cleanup.h"

using namespace qy;

static Engine g_engine;
static CandWindow g_cand;
static Config g_cfg;
static HWND g_main = nullptr, g_input = nullptr, g_panel = nullptr;
static bool g_settingsOnly = false;  // --settings：只显示设置（使用说明占左侧主区）
static bool g_modalOpen = false;     // 正在弹模态提示框（避免定时器/预览逻辑打扰）
static DWORD g_createTick = 0;
static std::wstring g_text;
static std::vector<HWND> g_checks;
static HFONT g_font = nullptr;
static HFONT g_fontUI = nullptr;
static HFONT g_fontTitle = nullptr;
static HWND g_help = nullptr;
// 「帮助 → 查看使用说明」的独立内容窗口（非模态，可滚动、可按 Esc 关闭）
static HWND g_helpPopup = nullptr;
static WNDPROC g_helpPopupEditOld = nullptr;
static const wchar_t* kHelpPopupClass = L"QyHelpPopupWnd";
// 窗口宽度由 900 横向缩小 1/4 到 675（固定边框 8px，客户区约 659）。
// 所有横向坐标统一由 LayoutMain() 按真实客户区宽度计算，别再写死 852 / 868。
static const int kWindowW = 675;
static const int kWindowH = 720;
static const int kClientW = 659;
static RECT g_statusRect{16, 362, 643, 400};  // 状态文字区域，局部刷新用
static RECT g_promptRect{16, 384, 643, 404};
static std::wstring g_status;

// 界面上显示的版本号：读安装目录里的 installed.ver，覆盖安装后立即是新号
// （编译期版本只作兜底：拷到别处单独运行时读不到记录）
static std::wstring ShownVersion() {
    std::wstring v = InstalledVersionOnDisk(ModuleDir(nullptr));
    return v.empty() ? CurrentVersion() : v;
}

// 设置模式下的「使用说明」：改用系统只读 EDIT 控件。
// 以前是自绘卡片：滚动条只是个装饰（拖不动），滚轮全靠整页重绘 40KB 文本，
// 每帧都要 DrawTextW 全量排版，非常卡。EDIT 的滚动是系统原生实现——滚动条
// 可拖动、滚轮/翻页键流畅；拦掉 WM_SETFOCUS 后点击也不会出现插入符。
static std::wstring g_helpText;
static HFONT g_fontHelp = nullptr;
static WNDPROC g_helpOldProc = nullptr;
// 菜单化后右侧平铺控件全部移除，说明卡片铺满整行（右边界在 LayoutMain 里对齐客户区）
static RECT g_helpCard = {16, 84, 643, 596};   // 说明卡片区域

static LRESULT CALLBACK HelpEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SETFOCUS) return 0;  // 不给焦点 → 没有插入符，也不抢窗口快捷键
    return ::CallWindowProcW(g_helpOldProc, h, m, w, l);
}

static std::wstring LoadHelpFile();  // 定义在下面；弹窗缓存没命中时兜底现读

// 「帮助 → 查看使用说明」独立窗口。
// 以前这个菜单项只是把内嵌说明框滚回顶部 —— 内嵌框本来就在屏幕上，点了
// 完全没反应，看起来就像「没有内容」。现在真正弹出一个可滚动的内容窗口。
static LRESULT CALLBACK HelpPopupEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        ::PostMessageW(::GetParent(h), WM_CLOSE, 0, 0);
        return 0;
    }
    return ::CallWindowProcW(g_helpPopupEditOld, h, m, w, l);
}

static LRESULT CALLBACK HelpPopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            RECT rc{};
            ::GetClientRect(h, &rc);
            HWND ed = ::CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL,
                10, 10, rc.right - 20, rc.bottom - 20, h, (HMENU)2001, nullptr, nullptr);
            ::SendMessageW(ed, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
            ::SendMessageW(ed, EM_SETLIMITTEXT, 0x7FFFFFF0, 0);
            std::wstring text = g_helpText;
            if (text.empty()) text = LoadHelpFile();  // 兜底：没缓存就现读一次
            if (text.empty())
                text = L"未能读取“使用说明.txt”。\r\n\r\n"
                       L"请确认输入法已完整安装（安装目录内应有 使用说明.txt），\r\n"
                       L"或访问发布页面查看在线说明：" + std::wstring(QY_SITE_URL);
            ::SetWindowTextW(ed, text.c_str());
            ::SendMessageW(ed, EM_SETSEL, 0, 0);
            g_helpPopupEditOld = (WNDPROC)::SetWindowLongPtrW(ed, GWLP_WNDPROC,
                                                               (LONG_PTR)HelpPopupEditProc);
            ::SetFocus(ed);  // 让滚轮 / PageUp / Home 直接作用于正文
            return 0;
        }
        case WM_SIZE: {
            HWND ed = ::GetDlgItem(h, 2001);
            if (ed) ::MoveWindow(ed, 10, 10, LOWORD(l) - 20, HIWORD(l) - 20, TRUE);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)w;
            ::SetBkColor(dc, RGB(255, 255, 253));
            static HBRUSH br = nullptr;
            if (!br) br = ::CreateSolidBrush(RGB(255, 255, 253));
            return (LRESULT)br;
        }
        case WM_CLOSE:
            ::DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            g_helpPopup = nullptr;
            return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

static void ShowHelpPopup(HWND owner) {
    if (g_helpPopup && ::IsWindow(g_helpPopup)) {
        ::ShowWindow(g_helpPopup, SW_SHOWNORMAL);
        ::SetForegroundWindow(g_helpPopup);
        return;
    }
    const int w = 760, hgt = 560;
    int sw = ::GetSystemMetrics(SM_CXSCREEN), sh = ::GetSystemMetrics(SM_CYSCREEN);
    g_helpPopup = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME, kHelpPopupClass, L"秋雨五笔输入法 - 使用说明",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX, (sw - w) / 2, (sh - hgt) / 2,
        w, hgt, owner, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (g_helpPopup) {
        ::ShowWindow(g_helpPopup, SW_SHOWNORMAL);
        ::UpdateWindow(g_helpPopup);
    }
}

// 读取安装目录的使用说明.txt（UTF-8），供设置窗口内的说明面板显示
static std::wstring DecodeHelpBytes(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    DWORD sz = ::GetFileSize(h, nullptr);
    std::string buf((size_t)sz, 0);
    DWORD rd = 0;
    if (sz) ::ReadFile(h, &buf[0], sz, &rd, nullptr);
    ::CloseHandle(h);
    buf.resize(rd);
    // 安装包里的 使用说明.txt 是 UTF-8 + 单个 BOM（见 check_dist_text.py）
    if (buf.size() >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        buf.erase(0, 3);
    int wl = ::MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), nullptr, 0);
    if (wl <= 0) return std::wstring();
    std::wstring w((size_t)wl, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)buf.size(), &w[0], wl);
    std::wstring out;
    out.reserve(w.size() + 256);
    for (wchar_t c : w) {
        if (c == L'\r') continue;
        if (c == L'\n') out += L"\r\n";  // EDIT 控件要 CRLF
        else out += c;
    }
    return out;
}

// 安装后 使用说明.txt 就在模块目录；开发态（build/qydemo.exe）旁边没有，
// 再往源码树的 dist/ 找一次，保证「查看使用说明」在任何运行位置都有内容。
static std::wstring LoadHelpFile() {
    std::wstring dir = ModuleDir(nullptr);
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    static const wchar_t* tails[] = {L"使用说明.txt", L"dist\\使用说明.txt",
                                     L"..\\dist\\使用说明.txt"};
    for (const wchar_t* tail : tails) {
        std::wstring text = DecodeHelpBytes(dir + tail);
        if (!text.empty()) return text;
    }
    return std::wstring();
}

enum {
    ID_BTN_WUBI = 100,
    ID_BTN_PINYIN,
    ID_BTN_SAVE,
    ID_BTN_DEFAULT,
    ID_BTN_UPDATE = 110,
    ID_BTN_SKIN_THEME = 111,
    ID_BTN_SKIN_ACCENT = 112,
    ID_BTN_SKIN_CARD = 113,
    ID_CHK_BASE = 200,

    // ---- 主菜单栏 / 二级菜单（2026-11-06 整理） ----
    // 勾选功能收进「选择」子菜单，点选功能收进「功能」子菜单，
    // 版本显示与两个网址入口收进「关于」子菜单。
    ID_MENU_EDIT_CLEAR = 301,
    ID_MENU_EDIT_SELALL = 302,
    ID_MENU_EDIT_COPY = 303,
    ID_MENU_WIN_TOP = 310,
    ID_MENU_WIN_MIN = 311,
    ID_MENU_WIN_CLOSE = 312,
    ID_MENU_HELP_VIEW = 320,
    ID_MENU_HELP_KEYS = 321,
    ID_MENU_ABOUT_VER = 330,   // 灰显，只显示版本
    ID_MENU_ABOUT_DLG = 331,
    ID_MENU_ABOUT_HOME = 332,    // 访问官方主页 http://sam.gd.cn
    ID_MENU_ABOUT_RELEASE = 333, // 访问发布页面（下载）
    ID_MENU_SKIN = 340,         // 状态条皮肤：默认（圆角·阴影）
    ID_MENU_SKIN_CLASSIC = 343, // 状态条皮肤：经典（直角·黑细描边·无阴影）
    ID_MENU_SKIN_COLOR = 344,   // 状态条皮肤：彩框（胶囊·彩色线框·无阴影）
    ID_MENU_ACCENT_RESET = 341,
    ID_MENU_CARD_RESET = 342,
    ID_MENU_MODE_WUBI = 350,    // 输入方案点选：五笔 86
    ID_MENU_MODE_PINYIN = 351,  // 输入方案点选：拼音
    ID_MENU_MODE_MIXED = 352    // 输入方案点选：五笔拼音混输
};

static const wchar_t* kChkNames[] = {L"四码唯一自动上屏", L"五码自动上屏首选", L"词频调整",
                                     L"中文标点",         L"仅GBK字符集",     L"候选显示编码",
                                     L"竖排候选",         L"显示输入法状态条", L"小键盘数字选词",
                                     L"Ctrl+空格 切换中英文", L"Ctrl+数字 手动调频",
                                     L"单击 Shift 切换中英文", L"按住 Shift 直接输出英文",
                                     L"新窗口默认中文输入",
                                     L"状态条半透明（鼠标靠近恢复实体）"};
static const int kChkCount = 15;

// 15 个勾选项 → Options 字段的唯一映射表。菜单勾选、点击翻转、恢复默认
// 都走它，避免三处各写一份 15 行的平行数组（以前漏一项就错位）。
static int* OptionSlot(Options& o, int i) {
    switch (i) {
        case 0: return &o.autoUnique4;
        case 1: return &o.autoNext5;
        case 2: return &o.freqAdjust;
        case 3: return &o.smartPunct;
        case 4: return &o.gbkOnly;
        case 5: return &o.showCode;
        case 6: return &o.layout;
        case 7: return &o.statusBar;
        case 8: return &o.numpadSelect;
        case 9: return &o.ctrlSpace;
        case 10: return &o.manualFreq;
        case 11: return &o.shiftToggle;
        case 12: return &o.shiftEnglish;
        case 13: return &o.startChinese;
        case 14: return &o.sbTranslucent;
        default: return nullptr;
    }
}

// 菜单栏与各子菜单句柄（挂在窗口上，WM_INITMENUPOPUP 时刷新勾选/文字）
static HMENU g_menuBar = nullptr;
static HMENU g_menuSelect = nullptr;
static HMENU g_menuFunc = nullptr;
static HMENU g_menuAbout = nullptr;
static bool g_topMost = false;

static void SyncOptionsToUI();
static void ApplyOptionsFromUI();
static void SaveModeToConfig();
static void BuildMainMenu(HWND h);
static void RefreshMenuState(HWND h);

// 方案切换后立刻写回配置，正在使用的输入法会自动跟随
static void SaveModeToConfig() {
    g_cfg.opt().mode = ModeToInt(g_engine.mode());
    g_cfg.Save();
}

static void DrawInput(HDC dc, RECT rc) {
    ::FillRect(dc, &rc, (HBRUSH)::GetStockObject(WHITE_BRUSH));
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(20, 20, 20));
    ::SelectObject(dc, g_font);

    int x = 12, y = 12;
    int lineH = (int)(g_cfg.opt().fontSize * 1.6);
    int maxW = rc.right - 24;

    std::wstring all = g_text;
    if (g_engine.hasComp()) all += g_engine.bufferW();
    size_t compStart = g_text.size();

    SIZE sz{};
    for (size_t i = 0; i < all.size(); ++i) {
        wchar_t ch = all[i];
        if (ch == L'\n') {
            x = 12;
            y += lineH;
            continue;
        }
        int w = 0;
        ::GetTextExtentPoint32W(dc, &ch, 1, &sz);
        w = sz.cx;
        if (x + w > maxW) {
            x = 12;
            y += lineH;
        }
        bool composing = g_engine.hasComp() && i >= compStart;
        if (composing) ::SetTextColor(dc, RGB(160, 60, 20));
        ::TextOutW(dc, x, y, &ch, 1);
        if (composing) {
            HPEN pen = ::CreatePen(PS_DOT, 1, RGB(200, 80, 40));
            HPEN old = (HPEN)::SelectObject(dc, pen);
            ::MoveToEx(dc, x, y + lineH - 4, nullptr);
            ::LineTo(dc, x + w, y + lineH - 4);
            ::SelectObject(dc, old);
            ::DeleteObject(pen);
            ::SetTextColor(dc, RGB(20, 20, 20));
        }
        x += w;
    }
    // 光标
    HPEN caret = ::CreatePen(PS_SOLID, 2, RGB(30, 110, 190));
    HPEN old = (HPEN)::SelectObject(dc, caret);
    ::MoveToEx(dc, x + 1, y + 2, nullptr);
    ::LineTo(dc, x + 1, y + lineH - 4);
    ::SelectObject(dc, old);
    ::DeleteObject(caret);
}

static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND:
            return 1;  // 双缓冲已覆盖整个客户区
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(h, &ps);
            RECT rc;
            ::GetClientRect(h, &rc);
            HDC mem = ::CreateCompatibleDC(dc);
            HBITMAP bmp = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HBITMAP ob = (HBITMAP)::SelectObject(mem, bmp);
            DrawInput(mem, rc);
            ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            ::SelectObject(mem, ob);
            ::DeleteObject(bmp);
            ::DeleteDC(mem);
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            ::InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            ::SetFocus(h);
            return 0;
        case WM_KEYDOWN: {
            bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool handled = false;
            std::wstring commit;
            if (w >= 'A' && w <= 'Z') {
                handled = g_engine.OnLetter((char)(w - 'A' + 'a'));
            } else if (w >= '1' && w <= '9' && g_engine.hasComp()) {
                handled = g_engine.OnDigit((int)(w - '0'));
            } else if (w == VK_SPACE) {
                if (g_engine.hasComp()) {
                    handled = g_engine.OnSpace();
                } else {
                    g_text += L' ';
                    handled = true;
                }
            } else if (w == VK_BACK) {
                if (g_engine.hasComp()) {
                    handled = g_engine.OnBackspace();
                } else if (!g_text.empty()) {
                    g_text.pop_back();
                    handled = true;
                }
            } else if (w == VK_RETURN) {
                if (g_engine.hasComp()) {
                    handled = g_engine.OnEnter();
                } else {
                    g_text += L'\n';
                    handled = true;
                }
            } else if (w == VK_ESCAPE) {
                handled = g_engine.OnEscape();
            } else if (w == VK_NEXT || w == VK_DOWN) {
                handled = g_engine.OnPageKey(true);
            } else if (w == VK_PRIOR || w == VK_UP) {
                handled = g_engine.OnPageKey(false);
            } else if (w == VK_TAB) {
                g_engine.ToggleMode();
                SaveModeToConfig();
                handled = true;
            } else {
                // 标点
                wchar_t ch = 0;
                switch (w) {
                    case VK_OEM_1: ch = shift ? L':' : L';'; break;
                    case VK_OEM_PLUS: ch = shift ? L'+' : L'='; break;
                    case VK_OEM_COMMA: ch = shift ? L'<' : L','; break;
                    case VK_OEM_MINUS: ch = shift ? L'_' : L'-'; break;
                    case VK_OEM_PERIOD: ch = shift ? L'>' : L'.'; break;
                    case VK_OEM_2: ch = shift ? L'?' : L'/'; break;
                    case VK_OEM_3: ch = shift ? L'~' : L'`'; break;
                    case VK_OEM_4: ch = shift ? L'{' : L'['; break;
                    case VK_OEM_5: ch = shift ? L'|' : L'\\'; break;
                    case VK_OEM_6: ch = shift ? L'}' : L']'; break;
                    case VK_OEM_7: ch = shift ? L'"' : L'\''; break;
                    default: break;
                }
                if (ch) {
                    g_text += g_engine.punct(ch, shift);
                    handled = true;
                }
            }
            while (g_engine.TakeCommit(commit)) g_text += commit;
            if (!handled && w == VK_TAB) handled = true;
            if (handled) {
                ::InvalidateRect(h, nullptr, FALSE);
                // 候选窗
                if (g_engine.hasComp() && !g_engine.cands().empty()) {
                    POINT pt;
                    pt.x = 0;
                    pt.y = 0;
                    ::ClientToScreen(h, &pt);
                    CandShow cs;
                    cs.items = &g_engine.cands();
                    cs.page = g_engine.pageNo();
                    cs.pageSize = (size_t)g_cfg.opt().pageSize;
                    cs.pt = POINT{pt.x + 20, pt.y + (LONG)(g_cfg.opt().fontSize * 2.4)};
                    cs.fontSize = g_cfg.opt().fontSize;
                    cs.showCode = g_cfg.opt().showCode != 0;
                    cs.horizontal = g_cfg.opt().layout == 0;
                    cs.showTip = g_cfg.opt().showCandTip != 0;
                    g_cand.Show(cs);
                } else {
                    g_cand.Hide();
                }
            }
            return 0;
        }
        case WM_CHAR:
            return 0;  // 已由 WM_KEYDOWN 处理
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

// ---- 状态条 DIY：主题 / 强调色 / 底色 循环切换 ----
// 预设色板（循环顺序）；0 表示“默认”（跟主题走）
static const COLORREF kAccentPresets[] = {
    0, RGB(40, 92, 206),  RGB(34, 160, 94), RGB(140, 90, 220),
    RGB(235, 130, 50), RGB(225, 90, 140), RGB(20, 150, 170), RGB(210, 70, 70)};
static const wchar_t* kAccentNames[] = {L"默认蓝", L"蓝",   L"绿",   L"紫",
                                        L"橙",    L"粉",   L"青",   L"红"};
static const COLORREF kCardPresets[] = {0,           RGB(250, 250, 252), RGB(247, 240, 222),
                                        RGB(232, 244, 230), RGB(230, 240, 250),
                                        RGB(248, 232, 238), RGB(238, 238, 242)};
static const wchar_t* kCardNames[] = {L"默认", L"白", L"米黄", L"浅绿", L"浅蓝", L"浅粉",
                                      L"浅灰"};
static const wchar_t* kThemeNames[] = {L"跟随系统", L"浅色", L"深色"};

static int FindPreset(const COLORREF* arr, int n, COLORREF v) {
    for (int i = 0; i < n; ++i)
        if (arr[i] == v) return i;
    return 0;
}

// #RRGGBB：按钮上直接显示当前值，用户一眼就知道现在是什么颜色
static std::wstring HexColor(COLORREF c) {
    wchar_t b[16];
    ::wsprintfW(b, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

static std::wstring SkinBtnText(int which) {
    const Options& o = g_cfg.opt();
    if (which == 0) {
        int t = o.sbTheme;
        if (t < 0 || t > 2) t = 0;
        return std::wstring(L"状态条主题：") + kThemeNames[t];
    }
    COLORREF c = (COLORREF)(which == 1 ? o.sbAccent : o.sbCard);
    const wchar_t* head = (which == 1) ? L"状态条配色：" : L"状态条底色：";
    if (!c) return std::wstring(head) + L"默认";
    // 恰好落在预设色上时显示中文名，其余显示十六进制
    if (which == 1) {
        int i = FindPreset(kAccentPresets, 8, c);
        if (i > 0) return std::wstring(head) + kAccentNames[i];
    } else {
        int i = FindPreset(kCardPresets, 7, c);
        if (i > 0) return std::wstring(head) + kCardNames[i];
    }
    return std::wstring(head) + HexColor(c);
}

// 调色盘：用系统颜色对话框让用户自定任意颜色。
// 预设色塞进“自定义颜色”格子，喜欢现成颜色的照样一点即用。
static bool PickColor(HWND owner, COLORREF init, COLORREF* out) {
    static COLORREF cust[16] = {0};
    static bool seeded = false;
    if (!seeded) {
        seeded = true;
        for (int i = 0; i < 8; ++i)
            cust[i] = kAccentPresets[i] ? kAccentPresets[i] : RGB(40, 92, 206);
        for (int i = 0; i < 7; ++i)
            cust[8 + i] = kCardPresets[i] ? kCardPresets[i] : RGB(250, 250, 252);
        cust[15] = RGB(255, 255, 255);
    }
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = init;
    cc.lpCustColors = cust;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!::ChooseColorW(&cc)) return false;
    *out = cc.rgbResult;
    return true;
}

static void RefreshMenuState(HWND h);

// 恢复某个 DIY 颜色为「跟随主题」
static void ResetSkinColor(HWND h, int which) {
    Options& o = g_cfg.opt();
    if (which == 1)
        o.sbAccent = 0;
    else
        o.sbCard = 0;
    g_cfg.Save();
    RefreshMenuState(h);
}

// ---- 主菜单栏（2026-11-06 整理设置面板）----
// 以前 15 个勾选框 + 7 个点选按钮平铺在右侧，占掉大半窗口。现在全部收进
// 二级菜单：勾选 → 「选择」，点选 → 「功能」，版本 → 「关于」。
// 菜单状态在每次展开时（WM_INITMENUPOPUP）按 config.ini 重新刷新，这样
// 输入法那边的右键菜单改了开关，这边展开也能立刻看到一致的状态。
static void BuildMainMenu(HWND h) {
    HMENU bar = ::CreateMenu();

    HMENU ed = ::CreatePopupMenu();
    ::AppendMenuW(ed, MF_STRING, ID_MENU_EDIT_CLEAR, L"清空演示输入");
    ::AppendMenuW(ed, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(ed, MF_STRING, ID_MENU_EDIT_SELALL, L"全选说明文本");
    ::AppendMenuW(ed, MF_STRING, ID_MENU_EDIT_COPY, L"复制说明文本");
    ::AppendMenuW(bar, MF_POPUP | MF_STRING, (UINT_PTR)ed, L"编辑(&E)");

    g_menuSelect = ::CreatePopupMenu();
    for (int i = 0; i < kChkCount; ++i)
        ::AppendMenuW(g_menuSelect, MF_STRING, ID_CHK_BASE + i, kChkNames[i]);
    ::AppendMenuW(bar, MF_POPUP | MF_STRING, (UINT_PTR)g_menuSelect, L"选择(&S)");

    HMENU wn = ::CreatePopupMenu();
    ::AppendMenuW(wn, MF_STRING, ID_MENU_WIN_TOP, L"窗口总在最前");
    ::AppendMenuW(wn, MF_STRING, ID_MENU_WIN_MIN, L"最小化");
    ::AppendMenuW(wn, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(wn, MF_STRING, ID_MENU_WIN_CLOSE, L"关闭(&C)");
    ::AppendMenuW(bar, MF_POPUP | MF_STRING, (UINT_PTR)wn, L"窗口(&W)");

    g_menuFunc = ::CreatePopupMenu();
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_MENU_MODE_WUBI, L"输入方案：五笔 86");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_MENU_MODE_PINYIN, L"输入方案：拼音");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_MENU_MODE_MIXED, L"输入方案：五笔拼音混输");
    ::AppendMenuW(g_menuFunc, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_BTN_SKIN_THEME, L"状态条主题");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_BTN_SKIN_ACCENT, L"状态条配色");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_BTN_SKIN_CARD, L"状态条底色");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_MENU_ACCENT_RESET, L"恢复默认配色");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_MENU_CARD_RESET, L"恢复默认底色");
    ::AppendMenuW(g_menuFunc, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_MENU_SKIN, L"皮肤：默认（圆角·阴影）");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_MENU_SKIN_CLASSIC, L"皮肤：经典（直角·黑细描边）");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_MENU_SKIN_COLOR,
                  L"皮肤：彩框（胶囊·彩色线框·无阴影）");
    ::AppendMenuW(g_menuFunc, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_BTN_SAVE, L"保存设置");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_BTN_DEFAULT, L"恢复默认设置");
    ::AppendMenuW(g_menuFunc, MF_STRING, ID_BTN_UPDATE, L"检查更新…");
    ::AppendMenuW(bar, MF_POPUP | MF_STRING, (UINT_PTR)g_menuFunc, L"功能(&F)");

    HMENU hp = ::CreatePopupMenu();
    ::AppendMenuW(hp, MF_STRING, ID_MENU_HELP_VIEW, L"查看使用说明");
    ::AppendMenuW(hp, MF_STRING, ID_MENU_HELP_KEYS, L"快捷键一览");
    ::AppendMenuW(bar, MF_POPUP | MF_STRING, (UINT_PTR)hp, L"帮助(&H)");

    g_menuAbout = ::CreatePopupMenu();
    ::AppendMenuW(g_menuAbout, MF_STRING | MF_GRAYED, ID_MENU_ABOUT_VER, L"版本");
    ::AppendMenuW(g_menuAbout, MF_STRING, ID_MENU_ABOUT_DLG, L"关于 秋雨五笔输入法…");
    ::AppendMenuW(g_menuAbout, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(g_menuAbout, MF_STRING, ID_MENU_ABOUT_HOME, L"访问官方主页");
    ::AppendMenuW(g_menuAbout, MF_STRING, ID_MENU_ABOUT_RELEASE, L"访问发布页面（下载）");
    ::AppendMenuW(bar, MF_POPUP | MF_STRING, (UINT_PTR)g_menuAbout, L"关于(&A)");

    g_menuBar = bar;
    ::SetMenu(h, bar);
    RefreshMenuState(h);
}

// 每次展开菜单前刷新：勾选状态、DIY 当前值、皮肤互斥勾选、版本文字、置顶勾选
static void RefreshMenuState(HWND h) {
    (void)h;
    Options o = g_cfg.opt();
    if (g_menuSelect) {
        for (int i = 0; i < kChkCount; ++i) {
            int* p = OptionSlot(o, i);
            ::CheckMenuItem(g_menuSelect, ID_CHK_BASE + i,
                            MF_BYCOMMAND | ((p && *p) ? MF_CHECKED : MF_UNCHECKED));
        }
    }
    if (g_menuFunc) {
        // 动态文字：把当前值直接写在菜单项上，点一下就能看到下一档是什么
        ::ModifyMenuW(g_menuFunc, ID_BTN_SKIN_THEME, MF_BYCOMMAND | MF_STRING,
                      ID_BTN_SKIN_THEME, SkinBtnText(0).c_str());
        ::ModifyMenuW(g_menuFunc, ID_BTN_SKIN_ACCENT, MF_BYCOMMAND | MF_STRING,
                      ID_BTN_SKIN_ACCENT, SkinBtnText(1).c_str());
        ::ModifyMenuW(g_menuFunc, ID_BTN_SKIN_CARD, MF_BYCOMMAND | MF_STRING,
                      ID_BTN_SKIN_CARD, SkinBtnText(2).c_str());
        ::CheckMenuItem(g_menuFunc, ID_MENU_SKIN,
                        MF_BYCOMMAND | (o.sbSkin == 0 ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(g_menuFunc, ID_MENU_SKIN_CLASSIC,
                        MF_BYCOMMAND | (o.sbSkin == 1 ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(g_menuFunc, ID_MENU_SKIN_COLOR,
                        MF_BYCOMMAND | (o.sbSkin == 2 ? MF_CHECKED : MF_UNCHECKED));
        const int m = ModeToInt(g_engine.mode());
        ::CheckMenuItem(g_menuFunc, ID_MENU_MODE_WUBI,
                        MF_BYCOMMAND | (m == 0 ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(g_menuFunc, ID_MENU_MODE_PINYIN,
                        MF_BYCOMMAND | (m == 1 ? MF_CHECKED : MF_UNCHECKED));
        ::CheckMenuItem(g_menuFunc, ID_MENU_MODE_MIXED,
                        MF_BYCOMMAND | (m == 2 ? MF_CHECKED : MF_UNCHECKED));
    }
    if (g_menuAbout) {
        std::wstring ver = L"版本  v" + ShownVersion();
        ::ModifyMenuW(g_menuAbout, ID_MENU_ABOUT_VER,
                      MF_BYCOMMAND | MF_STRING | MF_GRAYED, ID_MENU_ABOUT_VER, ver.c_str());
    }
    if (g_menuBar) {
        HMENU wn = ::GetSubMenu(g_menuBar, 2);
        if (wn)
            ::CheckMenuItem(wn, ID_MENU_WIN_TOP,
                            MF_BYCOMMAND | (g_topMost ? MF_CHECKED : MF_UNCHECKED));
    }
}

static void SyncOptionsToUI() { RefreshMenuState(g_main); }

static void ApplyOptionsFromUI() {
    g_engine.SetOptions(g_cfg.opt());
    if (g_input) ::InvalidateRect(g_input, nullptr, FALSE);
}

// 菜单里的每一次点选都走这里：落盘 -> 同步演示引擎 -> 刷新菜单勾选/文字。
// 菜单化以后不再有「先改控件、点保存才生效」的两段式，改完立刻生效。
static void CommitOptions(HWND h) {
    g_cfg.Save();
    ApplyOptionsFromUI();
    RefreshMenuState(h);
}

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return ::DefWindowProcW(h, m, w, l);
}

// ---- 设置模式的说明卡片底（圆角卡片由父窗口画，正文是 EDIT 子控件） ----

// 按真实客户区宽度排布子控件：窗口横向缩小 1/4 后不留下溢出/空白，
// 以后改宽度或系统 DPI 变化也只需动 kWindowW 一个常量。
static void LayoutMain(HWND h) {
    RECT rc{};
    if (!::GetClientRect(h, &rc) || rc.right <= 64) return;
    const int w = rc.right;
    g_helpCard.right = w - 16;
    g_statusRect.right = w - 16;
    g_promptRect.right = w - 16;
    if (g_input) ::MoveWindow(g_input, 16, 56, w - 32, 300, TRUE);
    if (g_help) {
        if (g_settingsOnly)
            ::MoveWindow(g_help, g_helpCard.left + 8, g_helpCard.top + 8,
                         g_helpCard.right - g_helpCard.left - 16,
                         g_helpCard.bottom - g_helpCard.top - 16, TRUE);
        else
            ::MoveWindow(g_help, 16, 454, w - 32, 180, TRUE);
    }
    if (g_settingsOnly) ::InvalidateRect(h, nullptr, FALSE);  // 卡片底跟着重画
}

static void DrawHelpCardBg(HDC dc) {
    // 卡片底：暖白圆角 + 细边框，和窗口灰底区分但不刺眼
    HBRUSH bg = ::CreateSolidBrush(RGB(255, 255, 253));
    HPEN bd = ::CreatePen(PS_SOLID, 1, RGB(212, 214, 220));
    HBRUSH ob = (HBRUSH)::SelectObject(dc, bg);
    HPEN op = (HPEN)::SelectObject(dc, bd);
    ::RoundRect(dc, g_helpCard.left, g_helpCard.top, g_helpCard.right, g_helpCard.bottom, 10, 10);
    ::SelectObject(dc, ob);
    ::SelectObject(dc, op);
    ::DeleteObject(bg);
    ::DeleteObject(bd);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            g_fontTitle = ::CreateFontW(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Microsoft YaHei");
            g_font = ::CreateFontW(-22, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, L"Microsoft YaHei");
            g_fontUI = ::CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                     OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Microsoft YaHei");
            // 演示输入框只在演示模式创建；设置模式下这块区域是“使用说明”卡片。
            // 菜单化以后右侧不再有平铺控件，输入区放宽到整行。
            if (!g_settingsOnly) {
                g_input = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"QyDemoInput", L"",
                                            WS_CHILD | WS_VISIBLE, 16, 56, kClientW - 32, 300,
                                            h, nullptr, nullptr, nullptr);
            }
            // 勾选 / 点选控件已全部收进主菜单栏的二级菜单（BuildMainMenu），
            // 窗口里只留说明卡片与演示输入区，不再有按钮抢占焦点。

            HFONT fy = ::CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, GB2312_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");

            std::wstring help = LoadHelpFile();
            if (help.empty())
                help = L"未能读取“使用说明.txt”。\n\n"
                       L"常用操作：\n"
                       L"  字母 = 输入编码    空格 = 上屏首选    数字 = 选候选\n"
                       L"  回车 = 清屏（清空编码与候选，不输出字符）\n"
                       L"  Tab  = 切换五笔 / 拼音\n"
                       L"  混输 = 忘了五笔码可直接打拼音（如 nihao），可在右侧关闭\n"
                       L"  Shift 单击 = 中英切换，Shift 按住打字母 = 直接输出英文\n"
                       L"  Esc  = 取消本次输入\n\n"
                       L"状态条：中/英 · 五/拼 · 全/半 · 小键盘 · 设置";
            // 两种模式都缓存全文：「帮助 → 查看使用说明」的弹出窗口要用
            g_helpText = help;

            if (g_settingsOnly) {
                // 纯设置模式：说明正文用系统只读 EDIT——滚动条可拖动、滚轮流畅；
                // 拦掉 WM_SETFOCUS 后点击不出现插入符（此前“白框”问题的本源）
                HWND ht = ::CreateWindowExW(0, L"STATIC", L"使用说明", WS_CHILD | WS_VISIBLE,
                                            16, 58, 200, 22, h, nullptr, nullptr, nullptr);
                ::SendMessageW(ht, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
                g_fontHelp = fy;
                g_help = ::CreateWindowExW(
                    0, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                        ES_AUTOVSCROLL,
                    g_helpCard.left + 8, g_helpCard.top + 8,
                    g_helpCard.right - g_helpCard.left - 16,
                    g_helpCard.bottom - g_helpCard.top - 16, h, (HMENU)1005, nullptr, nullptr);
                ::SendMessageW(g_help, WM_SETFONT, (WPARAM)(fy ? fy : g_fontUI), TRUE);
                ::SendMessageW(g_help, EM_SETLIMITTEXT, 0x7FFFFFF0, 0);
                ::SetWindowTextW(g_help, help.c_str());
                g_helpOldProc = (WNDPROC)::SetWindowLongPtrW(g_help, GWLP_WNDPROC,
                                                             (LONG_PTR)HelpEditProc);
            } else {
                // 演示模式：说明放在演示区与状态行下方
                HWND ht = ::CreateWindowExW(0, L"STATIC", L"使用说明", WS_CHILD | WS_VISIBLE, 16,
                                            430, 200, 22, h, nullptr, nullptr, nullptr);
                ::SendMessageW(ht, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
                g_help = ::CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                        ES_AUTOVSCROLL,
                    16, 454, kClientW - 32, 180, h, (HMENU)1004, nullptr, nullptr);
                ::SendMessageW(g_help, WM_SETFONT, (WPARAM)(fy ? fy : g_fontUI), TRUE);
                ::SendMessageW(g_help, EM_SETLIMITTEXT, 0x7FFFFFF0, 0);
                ::SetWindowTextW(g_help, help.c_str());
            }

            BuildMainMenu(h);
            LayoutMain(h);  // 子控件按 675 宽的客户区排布
            SyncOptionsToUI();
            g_createTick = ::GetTickCount();
            ::SetTimer(h, 1, 250, nullptr);
            return 0;
        }
        case WM_SIZE:
            LayoutMain(h);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // 全部由 WM_PAINT 双缓冲绘制，避免闪烁
        case WM_TIMER: {
            // 设置窗口是“常驻”的：切到别的程序、点一下空白处都不再自动关闭。
            // 只有用户点右上角关闭（或点「完成」）才收起——以前为了不留“孤儿
            // 窗口”做过失焦自动关闭，结果用户切到 Word 里试一下效果，回来设置
            // 就没了，反复打开很烦。
            wchar_t buf[256];
            wsprintfW(buf, L"当前方案：%s    候选数：%u    编码：%S",
                      g_engine.mode() == Mode::Pinyin
                          ? L"拼音"
                          : (g_engine.mode() == Mode::Mixed ? L"五笔拼音混输" : L"五笔 86"),
                      (unsigned)g_engine.cands().size(), g_engine.buffer().c_str());
            if (g_status != buf) {
                g_status = buf;
                // 只刷新状态文字那一条，不重画整窗
                if (!g_settingsOnly) ::InvalidateRect(h, &g_statusRect, FALSE);
            }
            return 0;
        }
        case WM_INITMENUPOPUP: {
            // 输入法那边的右键菜单可能刚改过开关（config.ini 是跨进程真值）。
            // 每次展开菜单前重读一次，勾选状态 / 皮肤 / 版本都与输入法保持一致。
            g_cfg.Load();
            ApplyOptionsFromUI();
            RefreshMenuState(h);
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(w);
            // 「选择」子菜单：15 个勾选项，统一走 OptionSlot 唯一映射
            if (id >= ID_CHK_BASE && id < ID_CHK_BASE + kChkCount) {
                int* slot = OptionSlot(g_cfg.opt(), id - ID_CHK_BASE);
                if (slot) {
                    *slot = *slot ? 0 : 1;
                    CommitOptions(h);
                }
                return 0;
            }
            switch (id) {
                case ID_MENU_MODE_WUBI:
                case ID_MENU_MODE_PINYIN:
                case ID_MENU_MODE_MIXED:
                    g_engine.SetMode(id == ID_MENU_MODE_WUBI
                                         ? Mode::Wubi
                                         : (id == ID_MENU_MODE_PINYIN ? Mode::Pinyin
                                                                      : Mode::Mixed));
                    SaveModeToConfig();
                    g_cand.Hide();
                    if (g_input) ::InvalidateRect(g_input, nullptr, FALSE);
                    RefreshMenuState(h);
                    break;
                case ID_BTN_SAVE:
                    CommitOptions(h);
                    g_modalOpen = true;
                    ::MessageBoxW(h, L"设置已保存到 %APPDATA%\\QiuYuWubi\\config.ini", L"秋雨五笔",
                                  MB_OK | MB_ICONINFORMATION);
                    g_modalOpen = false;
                    break;
                case ID_BTN_SKIN_THEME:
                    g_cfg.opt().sbTheme = (g_cfg.opt().sbTheme + 1) % 3;
                    CommitOptions(h);
                    break;
                case ID_BTN_SKIN_ACCENT:
                case ID_BTN_SKIN_CARD: {
                    const bool accent = (id == ID_BTN_SKIN_ACCENT);
                    Options& o = g_cfg.opt();
                    COLORREF cur = (COLORREF)(accent ? o.sbAccent : o.sbCard);
                    if (!cur) cur = accent ? RGB(40, 92, 206) : RGB(250, 250, 252);
                    COLORREF picked = cur;
                    g_modalOpen = true;
                    const bool ok = PickColor(h, cur, &picked);
                    g_modalOpen = false;
                    if (ok) {
                        if (accent)
                            o.sbAccent = (LONG)picked;
                        else
                            o.sbCard = (LONG)picked;
                        CommitOptions(h);
                    }
                    break;
                }
                case ID_MENU_ACCENT_RESET:
                    ResetSkinColor(h, 1);
                    CommitOptions(h);
                    break;
                case ID_MENU_CARD_RESET:
                    ResetSkinColor(h, 2);
                    CommitOptions(h);
                    break;
                case ID_MENU_SKIN:
                case ID_MENU_SKIN_CLASSIC:
                case ID_MENU_SKIN_COLOR:
                    // 0=默认（圆角+阴影） 1=经典（直角+黑细描边）
                    // 2=彩框（胶囊+彩色线框，线色随中英/配色变化）
                    g_cfg.opt().sbSkin = (id == ID_MENU_SKIN) ? 0
                                         : (id == ID_MENU_SKIN_CLASSIC ? 1 : 2);
                    CommitOptions(h);
                    break;
                case ID_BTN_DEFAULT: {
                    Options def;
                    g_cfg.opt() = def;
                    CommitOptions(h);
                    if (g_input) ::InvalidateRect(g_input, nullptr, FALSE);
                    break;
                }
                case ID_BTN_UPDATE: {
                    std::wstring dir = ModuleDir(nullptr);
                    std::wstring exe = dir + L"\\qyupd.exe";
                    if (::GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        ::ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(),
                                        SW_SHOWNORMAL);
                        break;
                    }
                    std::wstring addr = g_cfg.opt().updateUrl;
                    if (addr.empty()) {
                        g_modalOpen = true;
                        ::MessageBoxW(h,
                                      L"未配置更新地址。\n\n请在 %APPDATA%\\QiuYuWubi\\config.ini 的\n"
                                      L"[更新] 地址 里填写 version.txt 的网址（或本地路径）。",
                                      L"检查更新", MB_OK | MB_ICONINFORMATION);
                        g_modalOpen = false;
                        break;
                    }
                    g_modalOpen = true;
                    qy::UpdateInfo info;
                    std::wstring err;
                    if (!qy::FetchUpdateInfo(addr, &info, &err)) {
                        std::wstring msg = L"检查更新失败：\n" + err;
                        ::MessageBoxW(h, msg.c_str(), L"检查更新", MB_OK | MB_ICONWARNING);
                    } else if (qy::CompareVersion(info.version, ShownVersion()) > 0) {
                        std::wstring msg = L"发现新版本 v" + info.version + L"（当前 v" +
                                           ShownVersion() + L"）\n\n";
                        if (!info.notes.empty()) msg += info.notes + L"\n\n";
                        msg += L"是否现在下载？下载后双击安装包即可覆盖安装。";
                        if (::MessageBoxW(h, msg.c_str(), L"发现新版本",
                                          MB_YESNO | MB_ICONINFORMATION) == IDYES)
                            qy::OpenUrl(info.url.empty() ? addr : info.url);
                    } else {
                        std::wstring msg =
                            L"当前已是最新版本（v" + ShownVersion() + L"）。";
                        ::MessageBoxW(h, msg.c_str(), L"检查更新", MB_OK | MB_ICONINFORMATION);
                    }
                    g_modalOpen = false;
                    break;
                }
                case ID_MENU_EDIT_CLEAR:
                    g_text.clear();
                    g_engine.Clear();
                    g_cand.Hide();
                    if (g_input) ::InvalidateRect(g_input, nullptr, FALSE);
                    break;
                case ID_MENU_EDIT_SELALL:
                    if (g_help) ::SendMessageW(g_help, EM_SETSEL, 0, -1);
                    break;
                case ID_MENU_EDIT_COPY:
                    if (g_help) {
                        ::SendMessageW(g_help, EM_SETSEL, 0, -1);
                        ::SendMessageW(g_help, WM_COPY, 0, 0);
                    }
                    break;
                case ID_MENU_WIN_TOP:
                    g_topMost = !g_topMost;
                    ::SetWindowPos(h, g_topMost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE);
                    RefreshMenuState(h);
                    break;
                case ID_MENU_WIN_MIN:
                    ::ShowWindow(h, SW_MINIMIZE);
                    break;
                case ID_MENU_WIN_CLOSE:
                    ::PostMessageW(h, WM_CLOSE, 0, 0);
                    break;
                case ID_MENU_HELP_VIEW:
                    ShowHelpPopup(h);
                    break;
                case ID_MENU_HELP_KEYS:
                    g_modalOpen = true;
                    ::MessageBoxW(h,
                                  L"字母 = 输入编码    空格 = 上屏首选    数字 = 选候选\r\n"
                                  L"回车 = 清屏（清空编码与候选，不输出字符）\r\n"
                                  L"Esc  = 取消本次输入    Tab = 切换五笔 / 拼音 / 混输\r\n"
                                  L"Ctrl+空格 / 单击 Shift = 中英切换\r\n"
                                  L"按住 Shift 打字母 = 直接输出英文\r\n"
                                  L"Ctrl+数字 = 手动调频（把第 N 个候选调到首位）",
                                  L"快捷键一览", MB_OK | MB_ICONINFORMATION);
                    g_modalOpen = false;
                    break;
                case ID_MENU_ABOUT_DLG: {
                    std::wstring msg = L"秋雨五笔输入法  v" + ShownVersion() +
                                       L"\r\n\r\n"
                                       L"五笔 86 / 拼音 / 五笔拼音混输，支持 GBK、Windows 7～11。\r\n"
                                       L"官方主页：" + std::wstring(QY_HOME_URL) +
                                       L"\r\n五笔输入法发布页面（下载）：" +
                                       std::wstring(QY_SITE_URL);
                    g_modalOpen = true;
                    ::MessageBoxW(h, msg.c_str(), L"关于 秋雨五笔输入法",
                                  MB_OK | MB_ICONINFORMATION);
                    g_modalOpen = false;
                    break;
                }
                case ID_MENU_ABOUT_HOME:
                    qy::OpenUrl(QY_HOME_URL);
                    break;
                case ID_MENU_ABOUT_RELEASE:
                    qy::OpenUrl(QY_SITE_URL);
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(h, &ps);
            RECT rc;
            ::GetClientRect(h, &rc);

            // 双缓冲：先画到内存 DC，再一次性贴到屏幕，消除闪烁
            HDC mem = ::CreateCompatibleDC(dc);
            HBITMAP bmp = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HBITMAP oldBmp = (HBITMAP)::SelectObject(mem, bmp);

            ::FillRect(mem, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
            ::SetBkMode(mem, TRANSPARENT);

            ::SelectObject(mem, g_fontTitle ? g_fontTitle : g_fontUI);
            ::SetTextColor(mem, RGB(20, 30, 50));
            {
                const wchar_t* title = g_settingsOnly ? L"秋雨五笔输入法 · 设置"
                                                      : L"秋雨五笔输入法 · 演示与设置";
                ::TextOutW(mem, 16, 14, title, (int)::wcslen(title));
                // 版本号已移到「关于 -> 版本」（灰显项），窗口右上角不再自绘
            }

            if (g_settingsOnly) {
                DrawHelpCardBg(mem);  // 卡片底；正文是 EDIT 子控件，自己会画
            } else {
                ::SelectObject(mem, g_fontUI);
                ::SetTextColor(mem, RGB(90, 90, 95));
                if (!g_status.empty())
                    ::TextOutW(mem, 16, 366, g_status.c_str(), (int)g_status.size());
                ::TextOutW(mem, 16, 386,
                           L"提示：字母=编码 空格=上屏 数字=选词 回车=清屏 Tab=切换方案", 34);
            }

            ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            ::SelectObject(mem, oldBmp);
            ::DeleteObject(bmp);
            ::DeleteDC(mem);
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (g_settingsOnly && g_help) {
                // 说明框被拦了焦点，滚轮消息转给它；这样无论系统设置是
                // “悬停滚动”还是“焦点窗口滚动”，说明都能滚
                ::SendMessageW(g_help, WM_MOUSEWHEEL, w, l);
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            // 只读 EDIT 发的是 CTLCOLORSTATIC：给说明框配卡片同款暖白底
            if (g_settingsOnly && (HWND)l == g_help) {
                HDC dc = (HDC)w;
                ::SetBkColor(dc, RGB(255, 255, 253));
                static HBRUSH helpBr = nullptr;
                if (!helpBr) helpBr = ::CreateSolidBrush(RGB(255, 255, 253));
                return (LRESULT)helpBr;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            // 说明区自绘，点击只需确保窗口有焦点，不再有任何控件抢焦点画白框
            ::SetFocus(h);
            return 0;
        }
        case WM_DESTROY:
            if (g_helpPopup && ::IsWindow(g_helpPopup)) ::DestroyWindow(g_helpPopup);
            if (g_menuBar) {
                ::DestroyMenu(g_menuBar);
                g_menuBar = nullptr;
            }
            g_menuSelect = nullptr;
            g_menuFunc = nullptr;
            g_menuAbout = nullptr;
            g_cand.Hide();
            if (g_settingsOnly) qy::ClearPreviewPid();  // 预览归属随设置窗口一起结束
            if (g_font) ::DeleteObject(g_font);
            if (g_fontUI) ::DeleteObject(g_fontUI);
            if (g_fontTitle) ::DeleteObject(g_fontTitle);
            if (g_fontHelp) ::DeleteObject(g_fontHelp);
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_cfg.Load();
    g_engine.SetOptions(g_cfg.opt());

    // 由输入法状态条“设置…”启动时带 --settings，窗口标题改为“设置”并把焦点
    // 放在选项区，避免直接落到演示输入框里
    {
        const wchar_t* cmd = ::GetCommandLineW();
        if (cmd && wcsstr(cmd, L"--settings")) g_settingsOnly = true;
    }

    // 设置窗口只保留一个：重复点状态条设置图标时，把已经开着的那个提到前面
    if (g_settingsOnly) {
        HWND old = ::FindWindowW(L"QyDemoMain", L"秋雨五笔输入法 - 设置");
        if (old) {
            ::ShowWindow(old, SW_SHOWNORMAL);
            ::SetForegroundWindow(old);
            return 0;
        }
    }

    std::wstring dir = ModuleDir(hInst);
    // 设置窗口打开时状态条要留着做实时预览：把“打开设置时谁在前台”记下来，
    // 只有那个进程的状态条会被允许显示（见 qy_common.h 的 preview.pid 说明）
    if (g_settingsOnly) {
        HWND fg = ::GetForegroundWindow();
        DWORD pid = 0;
        if (fg) ::GetWindowThreadProcessId(fg, &pid);
        if (pid && pid != ::GetCurrentProcessId()) qy::WritePreviewPid(pid);
    }
    // 打开设置/演示程序时顺便清掉覆盖安装遗留的旧文件备份
    qy::PurgeBackupFiles(dir);
    if (::GetFileAttributesW((dir + L"\\data\\qywb.dat").c_str()) != INVALID_FILE_ATTRIBUTES)
        dir += L"\\data";
    if (!g_engine.Init(dir)) {
        ::MessageBoxW(nullptr, L"码表 data\\qywb.dat 载入失败", L"秋雨五笔", MB_ICONERROR);
        return 1;
    }
    g_engine.SetMode(ModeFromInt(g_cfg.opt().mode));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QyDemoMain";
    // 关键：类背景刷必须与自绘底色一致。否则点击/激活窗口引发擦除时
    // 会先露出未定义背景（白屏），这就是“点击窗口闪空白”的原因
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    // 图标：优先取内嵌资源（ID 101），否则退回同目录 qywb.ico
    wc.hIcon = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32,
                                   LR_DEFAULTCOLOR);
    if (!wc.hIcon)
        wc.hIcon =
            (HICON)::LoadImageW(nullptr, L"qywb.ico", IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    wc.hIconSm = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16,
                                     LR_DEFAULTCOLOR);
    ::RegisterClassExW(&wc);

    WNDCLASSEXW wc2 = wc;
    wc2.lpfnWndProc = InputProc;
    wc2.lpszClassName = L"QyDemoInput";
    wc2.hbrBackground = (HBRUSH)::GetStockObject(WHITE_BRUSH);  // 输入区底色就是白
    ::RegisterClassExW(&wc2);

    WNDCLASSEXW wc3 = wc;
    wc3.lpfnWndProc = HelpPopupProc;
    wc3.lpszClassName = kHelpPopupClass;
    ::RegisterClassExW(&wc3);

    g_main = ::CreateWindowExW(
        0, L"QyDemoMain",
        g_settingsOnly ? L"秋雨五笔输入法 - 设置" : L"秋雨五笔输入法 - 演示与设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, 200, 100,
        kWindowW, kWindowH, nullptr,
        nullptr, hInst, nullptr);
    if (g_settingsOnly) {
        // 屏幕居中显示，更像一个设置对话框；先定位再显示，避免位置跳动
        int sw = ::GetSystemMetrics(SM_CXSCREEN), sh = ::GetSystemMetrics(SM_CYSCREEN);
        ::SetWindowPos(g_main, HWND_TOP, (sw - kWindowW) / 2, (sh - kWindowH) / 2, 0, 0,
                       SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // 显示后立刻同步完成一次完整绘制：窗口出现的同一帧就是最终内容，
    // 不存在“先空白后内容”的中间态
    ::ShowWindow(g_main, SW_SHOW);
    ::UpdateWindow(g_main);
    if (!g_settingsOnly) {
        ::SetFocus(g_input);
    }

    g_cand.Create(hInst);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        if (!g_settingsOnly && msg.message == WM_KEYDOWN && ::GetFocus() != g_input) {
            ::SetFocus(g_input);
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    g_cand.Destroy();
    return 0;
}
