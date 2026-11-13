// 状态条“同进程多实例（多 TSF 上下文）抢显示”自检 —— 修复后版本
//
// 复现用户反馈（GHO Win10 打开设置后状态条自动闪烁 / 双条重叠）：
// 一个进程里挂了多个输入法上下文，每个都有自己的状态条（StatusBar 是每
// CTextService 一个，不是每进程单例）。打开设置后 owner==本进程PID，所有
// 实例都满足该条件、各自 Show() 并 HideSiblingProc 把对方 SW_HIDE，于是每
// 250ms 交替：画面闪烁甚至多条重叠。
//
// 修复后只允许“本进程当前负责显示（g_activeTid 指向的线程）”那一个实例显示，
// 其余一律让位。本测试在同一进程启动两个线程、各持一个 StatusBar（不同线程
// → MyTid 不同，才能触发互斥），把“设置窗口（**跨进程** sbfakesettings.exe）
// 在前台 + preview.pid=本进程”注入进去，长时间采样后断言：
//   1) 同一时刻不会两条都可见（消除双条重叠）；
//   2) 负责显示的实例稳定（翻转次数 == 0 → 不来回抢显示 → 不闪）；
//   3) 始终至少一条可见（预览不丢）。
//
// 关键：设置窗口必须是**别的进程**（sbfakesettings.exe）。若建在本进程，
// CheckForeground 会走“前台属于本程序”分支而不是设置预览分支，互斥逻辑
// 根本不被走到，测试等于没测（旧版 sbtest5 的教训）。
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <process.h>
#include "../core/qy_statusbar.h"

using namespace qy;

static FILE* g_out = nullptr;
static int g_fail = 0;
static HWND g_settings = nullptr;  // 由 main 在启动 sbfakesettings.exe 后填充

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[512]{};
    va_list ap;
    va_start(ap, fmt);
    ::wvsprintfW(buf, fmt, ap);
    va_end(ap);
    char u8[1024]{};
    ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), nullptr, nullptr);
    if (g_out) { fputs(u8, g_out); fputs("\n", g_out); fflush(g_out); }
}
static void Check(bool ok, const wchar_t* what) {
    Log(L"  [%s] %s", ok ? L"OK" : L"FAIL", what);
    if (!ok) ++g_fail;
}

struct Ctx {
    StatusBar* sb = nullptr;
    volatile bool running = true;
};

// 每个工作线程：创建一个状态条，注入“设置窗口在前台”的探针，持续泵消息并
// 主动 PostMessage WM_TIMER 推进前台归属复核（headless 下定时器可能不触发）。
static unsigned __stdcall Worker(void* p) {
    Ctx* c = (Ctx*)p;
    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    c->sb->SetForegroundProbe([]() {
        // 设置窗口句柄在 main 里通过 FindWindowW 取到后写进全局；这里直接返回。
        return g_settings;
    });
    c->sb->Create(hi);
    c->sb->SetEnabled(true);
    c->sb->Show();  // 两个实例初始都显示；互斥逻辑会在首个 tick 收敛到一条
    MSG msg;
    while (c->running) {
        if (c->sb && c->sb->hwnd())
            ::PostMessageW(c->sb->hwnd(), WM_TIMER, 1, 0);  // 强制走 CheckForeground
        if (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        } else {
            ::Sleep(20);
        }
    }
    c->sb->Destroy();
    return 0;
}

int main() {
    g_out = fopen("build/sbtest5.txt", "w");
    if (!g_out) g_out = stdout;

    // 启动**跨进程**设置窗口替身
    wchar_t cwd[MAX_PATH * 2] = {0};
    ::GetCurrentDirectoryW(MAX_PATH * 2, cwd);
    std::wstring fake = std::wstring(cwd) + L"\\build\\sbfakesettings.exe";
    HINSTANCE se = ::ShellExecuteW(nullptr, L"open", fake.c_str(), nullptr, cwd, SW_HIDE);
    Check((size_t)se > 32, L"启动 sbfakesettings.exe（跨进程设置窗口替身）");
    for (int i = 0; i < 100 && !g_settings; ++i) {
        ::Sleep(50);
        g_settings = ::FindWindowW(L"QyDemoMain", L"秋雨五笔输入法 - 设置");
    }
    Check(g_settings != nullptr, L"找到跨进程设置窗口（命中 IsSettingsWindowForeground）");
    if (!g_settings) {
        Log(L"---- %s（失败 %d 项）", g_fail ? L"FAIL" : L"ALL PASS", g_fail);
        if (g_out != stdout) fclose(g_out);
        return 1;
    }

    qy::WritePreviewPid(::GetCurrentProcessId());  // 预览归属 = 本进程（与真实一致）

    StatusBar sb1, sb2;
    Ctx c1{&sb1}, c2{&sb2};
    HANDLE t1 = (HANDLE)::_beginthreadex(nullptr, 0, Worker, &c1, 0, nullptr);
    HANDLE t2 = (HANDLE)::_beginthreadex(nullptr, 0, Worker, &c2, 0, nullptr);
    Check(t1 != nullptr && t2 != nullptr, L"两个工作线程启动");

    // 先让互斥收敛（约 1 秒），再开始采样，排除首拍竞态
    ::Sleep(1000);

    // 采样：每 100ms 看一眼“谁在显示”，统计翻转与异常
    int flips = 0, bothVis = 0, noneVis = 0, samples = 0;
    int prev = -2;  // -2=未初始化 -1=全隐 0=sb1 1=sb2 2=双显
    for (int i = 0; i < 30; ++i) {  // 约 3 秒
        ::Sleep(100);
        bool a = sb1.hwnd() && ::IsWindow(sb1.hwnd()) && ::IsWindowVisible(sb1.hwnd());
        bool b = sb2.hwnd() && ::IsWindow(sb2.hwnd()) && ::IsWindowVisible(sb2.hwnd());
        int id = (a && b) ? 2 : (a ? 0 : (b ? 1 : -1));
        if (id == 2) ++bothVis;
        if (id == -1) ++noneVis;
        if (prev >= 0 && id >= 0 && prev != id) ++flips;
        prev = id;
        ++samples;
    }
    Log(L"采样 %d 次：翻转 %d，双显 %d，全隐 %d", samples, flips, bothVis, noneVis);
    Check(noneVis == 0, L"设置预览时状态条不整体消失（至少一条可见）");
    Check(bothVis == 0, L"修复后不会出现“两条状态条同时可见”（消除双条重叠）");
    Check(flips == 0, L"修复后负责显示的实例稳定（不来回抢显示 → 不闪）");

    c1.running = false; c2.running = false;
    if (t1) ::WaitForSingleObject(t1, 3000);
    if (t2) ::WaitForSingleObject(t2, 3000);
    if (g_settings) ::PostMessageW(g_settings, WM_CLOSE, 0, 0);
    ::Sleep(300);
    qy::ClearPreviewPid();

    Log(L"---- %s（失败 %d 项）", g_fail ? L"FAIL" : L"ALL PASS", g_fail);
    if (g_out != stdout) fclose(g_out);
    return g_fail ? 1 : 0;
}
