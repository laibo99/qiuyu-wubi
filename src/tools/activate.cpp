// 前台激活工具：
//   activate.exe activate <窗口类名>   把指定类名的窗口切到前台
//   activate.exe spawnfg               启动 build\fgwin.exe 并把前台交给它
// 输出 before/after 的前台 PID，便于核对切换是否真实发生
#include <windows.h>
#include <stdio.h>
#include <string>

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

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: activate activate <cls> | spawnfg\n"); return 1; }
    DWORD before = ForegroundPid();
    bool ok = false;
    if (!::strcmp(argv[1], "spawnfg")) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        wchar_t path[MAX_PATH];
        ::GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir = path;
        size_t p = dir.find_last_of(L'\\');
        dir = dir.substr(0, p + 1) + L"fgwin.exe";
        if (!::CreateProcessW(nullptr, &dir[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                              &si, &pi)) {
            printf("spawn failed GLE=%lu\n", ::GetLastError());
            return 2;
        }
        HWND helper = nullptr;
        for (int i = 0; i < 40 && !helper; ++i) {
            ::Sleep(100);
            helper = ::FindWindowW(L"QyFgHelper", nullptr);
        }
        if (!helper) { printf("helper window not found\n"); return 3; }
        ForceForeground(helper);
        ::Sleep(300);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        ok = ForegroundPid() != before;
    } else if (!::strcmp(argv[1], "activate") && argc >= 3) {
        wchar_t cls[128];
        ::MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, cls, 128);
        HWND h = ::FindWindowW(cls, nullptr);
        if (!h) { printf("window not found: %s\n", argv[2]); return 4; }
        ForceForeground(h);
        ::Sleep(300);
        ok = ForegroundPid() != before || ForegroundPid() != 0;
    } else if (!::strcmp(argv[1], "activatepid") && argc >= 3) {
        DWORD want = (DWORD)::strtoul(argv[2], nullptr, 10);
        HWND found = nullptr;
        struct Ctx {
            DWORD want;
            HWND found;
        } ctx{want, nullptr};
        ::EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                Ctx* c = (Ctx*)lp;
                DWORD pid = 0;
                ::GetWindowThreadProcessId(h, &pid);
                if (pid == c->want && ::IsWindowVisible(h) &&
                    ::GetWindow(h, GW_OWNER) == nullptr) {
                    wchar_t t[64]{};
                    ::GetWindowTextW(h, t, 64);
                    if (t[0]) { c->found = h; return FALSE; }
                }
                return TRUE;
            },
            (LPARAM)&ctx);
        if (!ctx.found) { printf("no visible window for pid %lu\n", (unsigned long)want); return 5; }
        ForceForeground(ctx.found);
        ::Sleep(300);
        ok = ForegroundPid() == want;
    }
    printf("before_pid=%lu after_pid=%lu switched=%d\n", (unsigned long)before,
           (unsigned long)ForegroundPid(), (int)ok);
    return 0;
}
