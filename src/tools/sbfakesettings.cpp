// 极简“设置窗口”替身（独立进程）
//
// 状态条的“设置预览”分支只在“设置窗口是别的进程”时才进入（见
// qy_statusbar.cpp CheckForeground 的 if (pid != GetCurrentProcessId())），
// 所以必须用独立进程复现，不能把窗口建在被测进程里——否则 pid==本进程，
// 设置分支整段跳过，互斥逻辑根本不被验证（旧版 sbtest5 正是栽在这里）。
//
// 窗口类名 QyDemoMain、标题“秋雨五笔输入法 - 设置”——与 IsSettingsWindowForeground
// 的判断完全一致。收到 WM_CLOSE 退出，泵消息（否则被调用方发来的 WM_GETTEXT 会卡死）。
#include <windows.h>
#include <wchar.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        ::DestroyWindow(h);
        return 0;
    }
    if (m == WM_DESTROY) {
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int main() {
    // --plain：普通程序窗口替身（类名 / 标题都不命中 IsSettingsWindowForeground）。
    // sbtest6 用它复现「前台窗口属于别的进程、且那个进程没有输入法实例」。
    const wchar_t* cmd = ::GetCommandLineW();
    const bool plain = cmd && ::wcsstr(cmd, L"--plain") != nullptr;
    const wchar_t* cls = plain ? L"QyPlainHost" : L"QyDemoMain";
    const wchar_t* title = plain ? L"普通程序窗口" : L"秋雨五笔输入法 - 设置";

    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = cls;
    ::RegisterClassExW(&wc);

    HWND w = ::CreateWindowExW(0, cls, title, WS_OVERLAPPEDWINDOW, 100, 100, 320, 200,
                               nullptr, nullptr, hi, nullptr);
    // 不抢前台（SW_SHOWNOACTIVATE），避免干扰被测状态条；隐藏窗口照样能被
    // FindWindowW / GetWindowTextW 找到。
    ::ShowWindow(w, SW_SHOWNOACTIVATE);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}
