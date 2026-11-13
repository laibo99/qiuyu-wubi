// 状态条「代理窗口 / 跨进程前台」自检（2026-11-06）
//
// 复现用户反馈：在部分程序里开启输入法，状态条出现一下就自动消失。
// 根因见 qy_statusbar.cpp CheckForeground：以前只看「前台窗口 PID 是否等于
// 输入法实例所在进程」。UWP / WinUI（ApplicationFrameHost 承载窗口）、
// TextInputHost 承载输入法、Chromium / Electron 多进程窗口、跨完整性级别
// 宿主里，TIP 在 A 进程而前台 HWND 属于 B 进程；B 没有我们的状态条实例，
// A 一收起就再没有路径恢复。
//
// 修复后：目标进程里没有实例接管、且本 TSF 实例仍活跃（心跳由 IME 注入）
// 时保持显示；自检工具不注入心跳则仍走「切到别的程序收起」的老规则。
//
// 本测试用跨进程替身窗口（sbfakesettings.exe --plain，不命中设置预览分支）
// 注入前台，分四段断言：
//   ① 无心跳 → 老规则仍生效，切到别的进程应隐藏（回归保护）
//   ② 重新开启 + 注入心跳 → 代理窗口前台不应再被收起（修复点）
//   ③ 持续心跳 → 一直保持显示，不再自动消失
//   ④ 停止心跳 → 2 秒宽限过后照旧收起（不变成常驻）
//   ⑤ 焦点 PID 明确属于别的进程 + 仍持续注入心跳 → 立刻收起
//      （回归保护：修「切换窗口时状态条没有消失」）
//   ⑥ 焦点 PID 明确属于本进程（UWP 代理窗口）+ 无心跳 → 保持显示
//      （回归保护：不能把上一轮「部分程序自动消失」改回去）
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include "../core/qy_common.h"
#include "../core/qy_statusbar.h"

using namespace qy;

static FILE* g_out = nullptr;
static int g_fail = 0;
static HWND g_plain = nullptr;
// 注入「前台焦点控件所属进程」：0=拿不到（走心跳兜底）
static DWORD g_focusPid = 0;

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    ::wvsprintfW(buf, fmt, ap);
    va_end(ap);
    char u8[1024]{};
    ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), nullptr, nullptr);
    if (g_out) {
        fputs(u8, g_out);
        fputs("\n", g_out);
        fflush(g_out);
    }
}
static void Check(bool ok, const wchar_t* what) {
    Log(L"  [%s] %s", ok ? L"OK" : L"FAIL", what);
    if (!ok) ++g_fail;
}
static bool Visible(const StatusBar& sb) {
    return sb.hwnd() && ::IsWindow(sb.hwnd()) && ::IsWindowVisible(sb.hwnd()) != FALSE;
}
static BOOL CALLBACK CountVisibleStatusProc(HWND h, LPARAM lp) {
    int* n = (int*)lp;
    wchar_t cls[64]{};
    if (::GetClassNameW(h, cls, 64) && ::lstrcmpW(cls, L"QiuYuWubiStatusWnd") == 0 &&
        ::IsWindowVisible(h))
        ++*n;
    return TRUE;
}
static int CountVisibleStatus() {
    int n = 0;
    ::EnumWindows(CountVisibleStatusProc, (LPARAM)&n);
    return n;
}
// 本机可能同时装了输入法：Word / explorer 里的旧实例会按自己的前台判断
// 反复 Show，并用 HideSiblingProc 把本测试的状态条 SW_HIDE。自测要验证的是
// CheckForeground 自身的规则，先把这些外部实例收起来隔离环境。
static BOOL CALLBACK HideForeignProc(HWND h, LPARAM lp) {
    HWND self = (HWND)lp;
    if (h == self) return TRUE;
    wchar_t cls[64]{};
    if (::GetClassNameW(h, cls, 64) && ::lstrcmpW(cls, L"QiuYuWubiStatusWnd") == 0)
        ::ShowWindow(h, SW_HIDE);
    return TRUE;
}
static void HideForeignStatus(HWND self) {
    if (self) ::EnumWindows(HideForeignProc, (LPARAM)self);
}
static void Diag(const wchar_t* tag, const StatusBar& sb) {
    DWORD pid = 0;
    if (g_plain) ::GetWindowThreadProcessId(g_plain, &pid);
    Log(L"  诊断%s: 本条=%d 全系统可见=%d fgpid=%lu mypid=%lu 设置前台=%d preview=%lu",
        tag, Visible(sb) ? 1 : 0, CountVisibleStatus(), pid, ::GetCurrentProcessId(),
        qy::IsSettingsWindowForeground(g_plain) ? 1 : 0, qy::ReadPreviewPid());
}
static void Pump(StatusBar& sb, int rounds, bool heartbeat) {
    MSG msg;
    for (int i = 0; i < rounds; ++i) {
        HideForeignStatus(sb.hwnd());  // 隔离本机其它输入法实例的抢显示
        if (heartbeat) sb.NoteImeActivity();
        ::PostMessageW(sb.hwnd(), WM_TIMER, 1, 0);  // headless 下定时器不一定触发
        for (int k = 0; k < 3; ++k) {
            if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            } else {
                ::Sleep(8);
            }
        }
    }
}
static void WaitPump(int ms) {
    MSG msg;
    DWORD end = ::GetTickCount() + (DWORD)ms;
    while (::GetTickCount() < end) {
        if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        } else {
            ::Sleep(10);
        }
    }
}

int main() {
    g_out = fopen("build/sbtest6.txt", "w");
    if (!g_out) g_out = stdout;

    wchar_t cwd[MAX_PATH * 2] = {0};
    ::GetCurrentDirectoryW(MAX_PATH * 2, cwd);
    std::wstring exe = std::wstring(cwd) + L"\\build\\sbfakesettings.exe";
    HINSTANCE se = ::ShellExecuteW(nullptr, L"open", exe.c_str(), L"--plain", cwd, SW_HIDE);
    Check((size_t)se > 32, L"启动跨进程普通窗口替身（--plain）");
    for (int i = 0; i < 100 && !g_plain; ++i) {
        ::Sleep(50);
        g_plain = ::FindWindowW(L"QyPlainHost", L"普通程序窗口");
    }
    Check(g_plain != nullptr, L"找到替身窗口（不命中 IsSettingsWindowForeground）");
    if (!g_plain) {
        Log(L"---- %s（失败 %d 项）", g_fail ? L"FAIL" : L"ALL PASS", g_fail);
        if (g_out != stdout) fclose(g_out);
        return 1;
    }

    StatusBar sb;
    sb.SetForegroundProbe([]() { return g_plain; });
    sb.SetFocusPidProbe([]() { return g_focusPid; });
    Check(sb.Create(::GetModuleHandleW(nullptr)), L"状态条窗口创建成功");
    sb.SetEnabled(true);
    sb.Show();
    Check(Visible(sb), L"初始状态：状态条显示");

    // ① 不注入心跳：自检工具走老规则，切到别的进程仍应隐藏（回归保护）
    Pump(sb, 10, false);
    Diag(L"①", sb);
    Check(!Visible(sb), L"① 无 TSF 心跳时，切到别的进程仍照旧收起");

    // ② 重新开启 + 注入心跳（模拟 IME 的 tick / 按键续期）
    sb.Show();
    Pump(sb, 10, true);
    Diag(L"②", sb);
    Check(Visible(sb), L"② 代理窗口 + TSF 活跃：状态条保持显示");

    // ③ 持续活跃：不再自动消失
    Pump(sb, 30, true);
    Diag(L"③", sb);
    Check(Visible(sb), L"③ 持续 30 拍仍保持显示（不再自动消失）");

    // ④ 停止注入：2 秒宽限过后交回老规则，不变成常驻
    WaitPump(2600);
    Check(!Visible(sb), L"④ 停止心跳 2 秒后照旧收起（非常驻）");

    // ⑤ 真的切到别的程序：焦点控件 PID 明确不是本进程。即便心跳仍在持续
    //    注入（旧版 tick 就是这么干的），也必须立刻收起。
    {
        DWORD plainPid = 0;
        ::GetWindowThreadProcessId(g_plain, &plainPid);
        g_focusPid = plainPid;
        sb.Show();
        Check(Visible(sb), L"⑤ 前置：切换前状态条是显示的");
        Pump(sb, 6, true);
        Diag(L"⑤", sb);
        Check(!Visible(sb), L"⑤ 焦点属于别的进程 + 心跳活跃：立刻收起");
    }

    // ⑥ UWP / TextInputHost 代理窗口：顶层前台 PID 是宿主，但焦点控件属于
    //    本进程；即使没有心跳也不能把条收掉。
    {
        g_focusPid = ::GetCurrentProcessId();
        sb.Show();
        Pump(sb, 10, false);
        Diag(L"⑥", sb);
        Check(Visible(sb), L"⑥ 焦点属于本进程 + 无心跳：代理窗口保持显示");
    }
    g_focusPid = 0;

    sb.Destroy();
    ::PostMessageW(g_plain, WM_CLOSE, 0, 0);
    ::Sleep(200);

    Log(L"---- %s（失败 %d 项）", g_fail ? L"FAIL" : L"ALL PASS", g_fail);
    if (g_out != stdout) fclose(g_out);
    return g_fail ? 1 : 0;
}
