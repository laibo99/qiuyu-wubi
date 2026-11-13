// 前台窗口辅助：创建一个可见窗口并占住前台，供候选窗归属测试使用
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { ::PostQuitMessage(0); return 0; }
    return ::DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"QyFgHelper";
    ::RegisterClassExW(&wc);
    HWND h = ::CreateWindowExW(WS_EX_TOPMOST, L"QyFgHelper", L"fg helper", WS_OVERLAPPEDWINDOW,
                               200, 200, 300, 200, nullptr, nullptr, hInst, nullptr);
    ::ShowWindow(h, SW_SHOW);
    // AttachThreadInput 保证能真正拿到前台（即使本进程不是前台进程启动）
    {
        FILE* f = nullptr;
        fopen_s(&f, "build/fgwin_log.txt", "wb");
        if (f) {
            HWND f0 = ::GetForegroundWindow();
            DWORD p0 = 0;
            if (f0) ::GetWindowThreadProcessId(f0, &p0);
            fprintf(f, "before fg pid=%lu\n", (unsigned long)p0);
            fclose(f);
        }
    }
    HWND fg = ::GetForegroundWindow();
    DWORD fgThread = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = ::GetCurrentThreadId();
    if (fgThread && fgThread != myThread) ::AttachThreadInput(myThread, fgThread, TRUE);
    ::BringWindowToTop(h);
    ::SetForegroundWindow(h);
    if (fgThread && fgThread != myThread) ::AttachThreadInput(myThread, fgThread, FALSE);
    ::SetTimer(h, 1, 6000, nullptr);
    {
        FILE* f = nullptr;
        fopen_s(&f, "build/fgwin_log.txt", "ab");
        if (f) {
            HWND f1 = ::GetForegroundWindow();
            DWORD p1 = 0;
            if (f1) ::GetWindowThreadProcessId(f1, &p1);
            fprintf(f, "after  fg pid=%lu self=%lu\n", (unsigned long)p1,
                    (unsigned long)::GetCurrentProcessId());
            fclose(f);
        }
    }
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_TIMER) break;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}
