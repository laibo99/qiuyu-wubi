// 候选窗前台归属测试：
//   1) 本进程在前台时，候选窗必须保持显示（不能误隐藏）
//   2) 前台切到别的进程后，候选窗必须在 ~1 秒内自动隐藏
// 用 AttachThreadInput 拿到前台线程输入队列，保证 SetForegroundWindow 生效
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <vector>
#include "..\core\qy_candui.h"
#include "..\core\qy_engine.h"

using namespace qy;

static HWND g_host = nullptr;
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { ::PostQuitMessage(0); return 0; }
    return ::DefWindowProcW(h, m, w, l);
}

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

static DWORD ForegroundPid() {
    HWND fg = ::GetForegroundWindow();
    DWORD pid = 0;
    if (fg) ::GetWindowThreadProcessId(fg, &pid);
    return pid;
}

// 泵消息 ms 毫秒，让候选窗的 WM_TIMER 有机会执行
static void Pump(int ms) {
    DWORD end = ::GetTickCount() + ms;
    while ((int)(end - ::GetTickCount()) > 0) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(20);
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    fopen_s(&g_log, "build/cand_fg_test.txt", "wb");
    if (!g_log) return 1;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"QyCandFgHost";
    ::RegisterClassExW(&wc);
    g_host = ::CreateWindowExW(WS_EX_TOPMOST, L"QyCandFgHost", L"cand fg host",
                               WS_OVERLAPPEDWINDOW, 100, 100, 400, 300, nullptr, nullptr,
                               hInst, nullptr);
    ::ShowWindow(g_host, SW_SHOW);
    ForceForeground(g_host);
    Pump(400);
    DWORD me = ::GetCurrentProcessId();
    Log("前台窗口 PID=%lu 本进程 PID=%lu", (unsigned long)ForegroundPid(), (unsigned long)me);
    bool selfFg = ForegroundPid() == me;
    Log("本进程是否在前台: %d", (int)selfFg);

    CandWindow cand;
    if (!cand.Create(hInst)) { Log("[FAIL] 候选窗创建失败"); return 2; }

    std::vector<Candidate> items(2);
    items[0].text = L"测试候选一";
    items[0].code = "ceshi";
    items[1].text = L"测试候选二";
    items[1].code = "ceshi";
    CandShow cs;
    cs.items = &items;
    cs.pt = POINT{300, 300};
    cand.Show(cs);
    Log("候选窗 visible=%d", (int)cand.visible());

    // 1) 自己在前台：不应被隐藏
    Pump(1200);
    bool stillShown = cand.visible();
    if (!selfFg) {
        Log("[SKIP] 无法取得前台（环境限制），跳过“本进程前台不隐藏”一项");
        stillShown = true;
    } else {
        Log("[%s] 本进程前台 1.2 秒后仍显示 (visible=%d)", stillShown ? "PASS" : "FAIL",
            (int)stillShown);
    }

    // 2) 启动另一个进程并把前台让给它
    if (!cand.visible()) cand.Show(cs);  // 若上面被跳过时已隐藏，重新显示以便验证
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"build\\fgwin.exe";
    BOOL ok = ::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                               &pi);
    if (!ok) { Log("[FAIL] 无法启动 fgwin.exe err=%lu", ::GetLastError()); return 3; }
    HWND helper = nullptr;
    for (int i = 0; i < 40 && !helper; ++i) {
        ::Sleep(100);
        helper = ::FindWindowW(L"QyFgHelper", nullptr);
    }
    if (!helper) { Log("[FAIL] 未找到辅助窗口"); return 4; }
    ForceForeground(helper);
    Pump(300);
    Log("切换后前台 PID=%lu（辅助进程 PID=%lu）", (unsigned long)ForegroundPid(),
        (unsigned long)pi.dwProcessId);

    Pump(1500);
    bool hidden = !cand.visible();
    Log("[%s] 切到其它进程后候选窗已自动隐藏 (visible=%d)", hidden ? "PASS" : "FAIL",
        (int)cand.visible());

    ::TerminateProcess(pi.hProcess, 0);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    cand.Destroy();
    Log(hidden && stillShown ? "RESULT=PASS" : "RESULT=FAIL");
    fclose(g_log);
    return (hidden && stillShown) ? 0 : 5;
}
