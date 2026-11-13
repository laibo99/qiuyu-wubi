// 模拟真实场景测试设置窗口自动关闭：
// 1) 宿主窗口保持前台 → ShellExecute 打开 qydemo --settings → 它应保持打开
// 2) 宿主把前台切走（激活另一个窗口）→ 设置窗口应在 ~0.5s 内自动关闭
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void Pump(int ms) {
    DWORD t0 = GetTickCount();
    MSG msg;
    while (GetTickCount() - t0 < (DWORD)ms) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

static HWND FindQy() { return ::FindWindowW(L"QyDemoMain", L"秋雨五笔输入法 - 设置"); }

// 抢前台（AttachThreadInput 绕过系统的前台锁）
static void ForceForeground(HWND target) {
    HWND fg = ::GetForegroundWindow();
    DWORD fgThread = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = ::GetCurrentThreadId();
    if (fgThread && fgThread != myThread) ::AttachThreadInput(myThread, fgThread, TRUE);
    ::BringWindowToTop(target);
    ::SetForegroundWindow(target);
    ::SetActiveWindow(target);
    if (fgThread && fgThread != myThread) ::AttachThreadInput(myThread, fgThread, FALSE);
}

int main() {
    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"QySetTestHost";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(WS_EX_TOPMOST, L"QySetTestHost", L"settest host",
                                  WS_OVERLAPPEDWINDOW, 40, 40, 320, 200, nullptr, nullptr, hi,
                                  nullptr);
    ::ShowWindow(host, SW_SHOW);
    ::UpdateWindow(host);
    ::SetForegroundWindow(host);
    Pump(400);

    wchar_t dir[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, dir, MAX_PATH);  // build/settest.exe → 安装目录手工传参
    const wchar_t* qydir = L"C:\\Program Files\\QiuYuWubi";
    std::wstring exe = std::wstring(qydir) + L"\\qydemo.exe";
    ::ShellExecuteW(nullptr, L"open", exe.c_str(), L"--settings", qydir, SW_SHOWNORMAL);

    bool qyWasFg = false;
    {
        DWORD t0 = ::GetTickCount();
        MSG msg;
        while (::GetTickCount() - t0 < 2500) {
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            if (::GetForegroundWindow() == FindQy()) qyWasFg = true;
            ::Sleep(10);
        }
    }
    HWND qy = FindQy();
    printf("[1] host foreground 2.5s -> settings %s (qy was foreground: %s)\n",
           qy ? "ALIVE (OK)" : "CLOSED (BAD)", qyWasFg ? "yes" : "NO");
    fflush(stdout);

    if (qy) {
        // 切前台到另一个程序：直接激活任务栏上的资源管理器（或任意非本进程窗口）
        HWND other = nullptr;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            if (!::IsWindowVisible(h)) return TRUE;
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid == ::GetCurrentProcessId()) return TRUE;
            wchar_t cls[64]{};
            ::GetClassNameW(h, cls, 64);
            if (::wcscmp(cls, L"TXGuiFoundation") == 0) { *(HWND*)lp = h; return FALSE; }
            return TRUE;
        }, (LPARAM)&other);
        if (other) {
            ForceForeground(other);
            Pump(1500);
            wchar_t fgc[64]{};
            ::GetClassNameW(::GetForegroundWindow(), fgc, 64);
            printf("    fg after switch: %ls\n", fgc);
            qy = FindQy();
            printf("[2] switched away -> settings %s\n", qy ? "STILL OPEN (BAD)"
                                                            : "CLOSED (OK)");
            fflush(stdout);
        } else {
            printf("[2] no TXGuiFoundation window to switch to\n");
        }
    }
    if (qy) { ::PostMessageW(qy, WM_CLOSE, 0, 0); Pump(300); }
    ::DestroyWindow(host);
    return 0;
}
