// 状态条“设置窗口预览”自检
//
// 场景（用户反馈）：在设置里调状态条配色时，一点设置状态条就消失，
// 所以看不到改动效果。修好后应当满足：
//   1) 设置窗口在前台 → 状态条继续显示（可实时预览）
//   2) 切到别的程序（非设置窗口）→ 状态条照常收起
//   3) 再切回设置窗口 → 状态条自动恢复（否则预览一次就没了）
//   4) 非“归属进程”的实例在设置窗口前台时不许抢显示（避免多进程互顶闪屏）
//
// 前台窗口用可注入探针模拟，避免依赖真实的窗口激活时序。
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>
#include "../core/qy_statusbar.h"

using namespace qy;

static FILE* g_out = nullptr;
static int g_fail = 0;

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

static void Check(bool ok, const wchar_t* what) {
    Log(L"  [%s] %s", ok ? L"OK" : L"FAIL", what);
    if (!ok) ++g_fail;
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

int main() {
    g_out = fopen("build/sbtest4.txt", "w");
    if (!g_out) g_out = stdout;

    // 宿主窗口：让“本进程”有个前台窗口可当预览归属
    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hi;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QySbTest4Host";
    ::RegisterClassExW(&wc);
    HWND host = ::CreateWindowExW(0, L"QySbTest4Host", L"宿主", WS_OVERLAPPEDWINDOW, 40, 40, 400,
                                  200, nullptr, nullptr, hi, nullptr);
    ::ShowWindow(host, SW_SHOWNORMAL);
    ::SetForegroundWindow(host);
    Pump(120);

    // 启动设置窗口（真实进程，用于判断“设置窗口在前台”）
    wchar_t cwd[MAX_PATH * 2] = {0};
    ::GetCurrentDirectoryW(MAX_PATH * 2, cwd);
    std::wstring demo = std::wstring(cwd) + L"\\build\\t4\\qydemo.exe";
    std::wstring demoDir = std::wstring(cwd) + L"\\build\\t4";
    HINSTANCE se = ::ShellExecuteW(nullptr, L"open", demo.c_str(), L"--settings", demoDir.c_str(),
                                   SW_SHOWNORMAL);
    Log(L"ShellExecute -> %p (%ls)", (void*)se, demo.c_str());
    HWND settings = nullptr;
    for (int i = 0; i < 100 && !settings; ++i) {
        Pump(50);
        settings = ::FindWindowW(L"QyDemoMain", L"秋雨五笔输入法 - 设置");
    }
    Log(L"设置窗口：%p", (void*)settings);
    Check(settings != nullptr, L"qydemo --settings 能启动");

    // 归属进程：qydemo 启动时写 preview.pid；这里读出来（不稳定时改成自己）。
    // 注意：qydemo 写的是“启动瞬间前台窗口的 pid”。无人值守 / 锁屏会话里
    // GetForegroundWindow() 返回 0，qydemo 没东西可写，这属于环境限制，
    // 不算产品缺陷——记成警告，后面用本进程 pid 继续验证预览逻辑。
    DWORD owner = ReadPreviewPid();
    Log(L"preview.pid = %lu（本进程 %lu）", (unsigned long)owner,
        (unsigned long)::GetCurrentProcessId());
    {
        HWND fg0 = ::GetForegroundWindow();
        DWORD fgPid0 = 0;
        if (fg0) ::GetWindowThreadProcessId(fg0, &fgPid0);
        if (owner == 0 && fgPid0 == 0)
            Log(L"  [跳过] 会话没有前台窗口（无人值守环境），无法验证 preview.pid 登记");
        else
            Check(owner != 0, L"设置窗口登记了 preview.pid（预览归属）");
    }
    if (owner != ::GetCurrentProcessId()) {
        WritePreviewPid(::GetCurrentProcessId());
        Log(L"  （测试环境前台不是本进程，已改写为本进程以便验证逻辑）");
    }

    StatusBar sb;
    HWND fg = settings;
    sb.SetForegroundProbe([&]() { return fg; });
    sb.Create(hi);
    sb.SetEnabled(true);
    sb.Show();
    Pump(400);
    Check(sb.visible() && ::IsWindowVisible(sb.hwnd()), L"初始状态：状态条显示");

    // 1) 前台=设置窗口 → 保持显示
    fg = settings;
    Pump(700);
    Check(sb.visible() && ::IsWindowVisible(sb.hwnd()), L"设置窗口在前台时状态条保持显示");

    // 1b) 再等几拍，确认不会被“前台不是本进程”的规则收走
    Pump(900);
    Check(::IsWindowVisible(sb.hwnd()), L"持续 1.6 秒仍保持显示（不闪退）");

    // 2) 前台=别的程序的窗口 → 收起
    HWND tray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        fg = tray;
        Pump(700);
        Check(!::IsWindowVisible(sb.hwnd()), L"切到别的程序 → 状态条收起");
    } else {
        Log(L"  [跳过] 找不到任务栏窗口，无法模拟其它程序");
    }

    // 3) 切回设置窗口 → 恢复显示
    fg = settings;
    Pump(700);
    Check(::IsWindowVisible(sb.hwnd()), L"切回设置窗口 → 状态条自动恢复（继续预览）");

    // 4) 归属进程不是自己 → 不许抢显示
    WritePreviewPid(::GetCurrentProcessId() + 4);  // 一个不存在的进程
    Pump(700);
    Check(!::IsWindowVisible(sb.hwnd()), L"非归属进程的实例不抢显示（避免多进程互顶）");
    WritePreviewPid(::GetCurrentProcessId());

    // 5) 设置窗口关掉（清掉归属）→ 回到普通规则（前台不是本进程就收起）
    ClearPreviewPid();
    if (settings) ::PostMessageW(settings, WM_CLOSE, 0, 0);
    Pump(800);
    fg = (tray && ::IsWindow(tray)) ? tray : nullptr;
    if (!fg) {
        Log(L"  [跳过] 没有可用的“其它程序”窗口");
    } else {
        Pump(700);
        Check(!::IsWindowVisible(sb.hwnd()), L"预览结束后回到普通规则（不在前台就收起）");
    }

    sb.Destroy();
    ::DestroyWindow(host);
    Log(L"---- %s（失败 %d 项）", g_fail ? L"FAIL" : L"ALL PASS", g_fail);
    if (g_out != stdout) fclose(g_out);
    return g_fail ? 1 : 0;
}
