// 图标输入面板自检：创建面板 → 点“数学”分类 → 点第一个符号 → 记录选中的符号
#include <windows.h>
#include <stdio.h>
#include "../core/qy_symui.h"

static HWND g_host = nullptr;
static std::wstring g_picked;

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

int main() {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"QySymTestHost";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    g_host = CreateWindowExW(WS_EX_TOPMOST, L"QySymTestHost", L"symtest host",
                             WS_OVERLAPPEDWINDOW, 60, 60, 300, 200, nullptr, nullptr, hi,
                             nullptr);
    ShowWindow(g_host, SW_SHOW);
    UpdateWindow(g_host);
    SetForegroundWindow(g_host);
    Pump(300);

    qy::SymbolPanel p;
    if (!p.Create(hi)) {
        FILE* f = nullptr;
        _wfopen_s(&f, L"build/sym_test.txt", L"wb");
        if (f) { fputs("create FAILED", f); fclose(f); }
        return 1;
    }
    p.SetPickHandler([](const std::wstring& s) { g_picked = s; });
    p.Show(POINT{120, 520});
    Pump(2000);  // 此时外部截图

    // 点“数学”分类（tab 宽 64，第 2 个）
    POINT tb{10 + 64 + 32, 22};
    SendMessageW(p.hwnd(), WM_LBUTTONDOWN, 0, MAKELPARAM(tb.x, tb.y));
    Pump(2000);  // 外部截图

    // 点第一个符号
    POINT c0{31, 44 + 17};
    SendMessageW(p.hwnd(), WM_LBUTTONDOWN, 0, MAKELPARAM(c0.x, c0.y));
    Pump(300);

    // 点“颜文字”分类（第 6 个）
    POINT tb5{10 + 5 * 64 + 32, 22};
    SendMessageW(p.hwnd(), WM_LBUTTONDOWN, 0, MAKELPARAM(tb5.x, tb5.y));
    Pump(2200);  // 外部截图

    RECT r{};
    GetWindowRect(p.hwnd(), &r);
    wchar_t buf[512];
    wsprintfW(buf, L"picked=[%s]\r\npanel rect=%ld,%ld %ldx%ld\r\nvisible=%d\r\n",
              g_picked.c_str(), r.left, r.top, r.right - r.left, r.bottom - r.top,
              (int)p.visible());
    FILE* f = nullptr;
    _wfopen_s(&f, L"build/sym_test.txt", L"wb");
    if (f) { fwrite(buf, sizeof(wchar_t), wcslen(buf), f); fclose(f); }

    p.Destroy();
    DestroyWindow(g_host);
    return 0;
}
