// 状态条前台归属自检（用可注入的“前台窗口”探针，避免依赖真实前台切换）
// 1) 前台=本线程窗口 → 状态条显示
// 2) 前台=别的进程窗口（模拟切到别的程序）→ 状态条隐藏
// 2b) 前台=本进程另一个线程的窗口（记事本/WinUI 的输入线程场景）→ 仍应显示
// 3) 前台切回本线程 → 状态条自动恢复（本次修复重点）
// 4) 状态条被外部 ShowWindow(SW_HIDE)（模拟被别的输入法实例隐藏）→ 切走再切回仍能恢复
// 5) SetEnabled(false) 后不再自动弹出
#include <windows.h>
#include <stdio.h>
#include <string>
#include "../core/qy_statusbar.h"

using namespace qy;

static FILE* g_out = nullptr;
static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);
    char u8[1024]{};
    ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), nullptr, nullptr);
    if (g_out) {
        fputs(u8, g_out);
        fputs("\n", g_out);
        fflush(g_out);
    }
}

static void Pump(int ms) {
    DWORD t0 = ::GetTickCount();
    MSG msg;
    while (::GetTickCount() - t0 < (DWORD)ms) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(10);
    }
}

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        ::DestroyWindow(h);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

struct OtherCtx {
    HWND hwnd = nullptr;
    HANDLE ready = nullptr;
};

static DWORD WINAPI OtherThread(LPVOID p) {
    OtherCtx* c = (OtherCtx*)p;
    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"QySbOtherHost";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    ::RegisterClassExW(&wc);
    c->hwnd = ::CreateWindowExW(0, L"QySbOtherHost", L"other app", WS_OVERLAPPEDWINDOW, 600, 120,
                                320, 200, nullptr, nullptr, hi, nullptr);
    ::ShowWindow(c->hwnd, SW_SHOW);
    ::SetEvent(c->ready);
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}

static HWND g_probe = nullptr;
static int g_fail = 0;

static void Check(const wchar_t* name, bool expectVisible, bool actualVisible) {
    bool ok = (expectVisible == actualVisible);
    if (!ok) ++g_fail;
    Log(L"%s %s: expected=%s actual=%s", ok ? L"PASS" : L"FAIL", name,
        expectVisible ? L"shown" : L"hidden", actualVisible ? L"shown" : L"hidden");
}

int main() {
    g_out = fopen("build/sbtest3.txt", "wb");
    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"QySbTestHost";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(0, L"QySbTestHost", L"sb host", WS_OVERLAPPEDWINDOW, 40, 40, 320,
                                  200, nullptr, nullptr, hi, nullptr);
    ::ShowWindow(host, SW_SHOW);
    ::UpdateWindow(host);

    OtherCtx oc;
    oc.ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::CreateThread(nullptr, 0, OtherThread, &oc, 0, nullptr);
    ::WaitForSingleObject(oc.ready, 3000);

    StatusBar sb;
    bool ok = sb.Create(hi);
    Log(L"create=%d hwnd=%p host=%p", (int)ok, (void*)sb.hwnd(), (void*)host);
    if (!ok) return 1;
    HWND sh = sb.hwnd();
    sb.Update(StatusInfo{});
    sb.SetForegroundProbe([]() { return g_probe; });
    g_probe = host;  // 本线程窗口在前台
    sb.SetEnabled(true);
    sb.Show();
    Pump(500);
    Check(L"[1] 前台=本线程", true, ::IsWindowVisible(sh) != FALSE);

    // 别的进程窗口：任务栏属于 explorer，用它模拟“切到其它程序”
    HWND otherProc = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!otherProc) otherProc = ::GetShellWindow();
    Log(L"other proc window=%p selfpid=%lu", (void*)otherProc, ::GetCurrentProcessId());
    g_probe = otherProc;
    Pump(600);
    Check(L"[2] 切到其它程序（别的进程）", false, ::IsWindowVisible(sh) != FALSE);

    g_probe = host;  // 切回来
    Pump(600);
    Check(L"[3] 切回本程序（自动恢复）", true, ::IsWindowVisible(sh) != FALSE);

    // 关键回归：前台窗口属于本进程但不在本线程 —— 记事本、WinUI/UWP 就是这种
    // 结构（输入法在编辑区自己的线程里激活）。这种必须仍然显示，否则一打字就消失。
    g_probe = oc.hwnd;
    Pump(600);
    Check(L"[3b] 本进程另一线程窗口在前台（记事本场景）", true,
          ::IsWindowVisible(sh) != FALSE);
    g_probe = host;
    Pump(400);

    // 4) 被别的输入法实例直接隐藏（内部标志仍认为已显示）
    ::ShowWindow(sh, SW_HIDE);
    Log(L"[4a] 外部隐藏后: real=%d", (int)::IsWindowVisible(sh));
    g_probe = otherProc;
    Pump(500);
    Check(L"[4b] 外部隐藏后切走", false, ::IsWindowVisible(sh) != FALSE);
    g_probe = host;
    Pump(600);
    Check(L"[4c] 外部隐藏后切回（应恢复）", true, ::IsWindowVisible(sh) != FALSE);

    // 5) 关闭开关后不再自动弹出
    sb.SetEnabled(false);
    ::ShowWindow(sh, SW_HIDE);
    Pump(700);
    Check(L"[5] 关闭状态条开关", false, ::IsWindowVisible(sh) != FALSE);

    Log(L"RESULT: %s (%d fail)", g_fail == 0 ? L"ALL PASS" : L"HAS FAILURE", g_fail);
    sb.Destroy();
    ::PostMessageW(host, WM_CLOSE, 0, 0);
    ::PostMessageW(oc.hwnd, WM_CLOSE, 0, 0);
    Pump(200);
    if (g_out) fclose(g_out);
    return g_fail == 0 ? 0 : 2;
}
