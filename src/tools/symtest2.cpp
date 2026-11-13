// 符号面板“点外面收起”回归测试（v3.4）
// 复现 Win10 上“点状态条图标弹面板，面板不到半秒自己消失”的场景：
//   老代码轮询“刚按下过鼠标”的边沿标志，点状态条图标弹面板的那次点击
//   留下的标志没被消费，保护期一过就把面板误关；新方案用 WH_MOUSE_LL
//   钩子在按下事件里判定，不再有这个问题。
// 本沙箱注入的真实鼠标事件到不了本进程窗口（见 [diag]），所以：
//   · 钩子判定直接喂屏幕坐标（DebugInjectMouse）
//   · 面板内部点击走 SendMessage（等同收到 WM_LBUTTONDOWN）
//   · 真实 SendInput 点击仍会执行，仅作可达性诊断，不作为断言
#include <windows.h>
#include <stdio.h>
#include <string>
#include "../core/qy_symui.h"

static HWND g_host = nullptr;
static std::wstring g_picked;
static std::wstring g_report;
static int g_hostClicks = 0;  // 主人窗口真实收到的左键单击数（诊断用）

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (m == WM_LBUTTONUP) ++g_hostClicks;
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
        Sleep(5);
    }
}

static void Report(const wchar_t* tag, bool ok) {
    g_report += std::wstring(tag) + L"=" + (ok ? L"PASS" : L"FAIL") + L"\r\n";
}

static void RealClickDiag(POINT pt) {
    DWORD tid = 0;
    HANDLE th = CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
            POINT pt = *(POINT*)p;
            SetCursorPos(pt.x, pt.y);
            Sleep(30);
            INPUT in[2]{};
            in[0].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            in[1].type = INPUT_MOUSE;
            in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(2, in, sizeof(INPUT));
            return 0;
        },
        &pt, 0, &tid);
    if (th) {
        WaitForSingleObject(th, 2000);
        CloseHandle(th);
    }
    Sleep(40);
}

int main() {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"QySymTestHost2";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    g_host = CreateWindowExW(WS_EX_TOPMOST, L"QySymTestHost2", L"symtest2 host",
                             WS_OVERLAPPEDWINDOW, 60, 60, 300, 200, nullptr, nullptr, hi,
                             nullptr);
    ShowWindow(g_host, SW_SHOW);
    UpdateWindow(g_host);
    SetForegroundWindow(g_host);
    Pump(300);
    RECT hr{};
    GetWindowRect(g_host, &hr);
    POINT hostCtr{(hr.left + hr.right) / 2, (hr.top + hr.bottom) / 2};

    qy::SymbolPanel p;
    p.Create(hi);
    p.SetOwnerWindow(g_host);
    // 沙箱拿不到前台锁，注入探针替代 GetForegroundWindow，让 CheckForeground
    // 认为主人窗口一直在前台（真实环境里宿主程序本来就在前台）
    p.SetForegroundProbe([]() { return g_host; });
    p.SetPickHandler([](const std::wstring& s) { g_picked = s; });

    // [1] 弹出面板 + 老代码必现误关的场景：面板显示期间发生“面板外”点击。
    //     先用钩子判定点主人窗口（= 点状态条图标本身）：必须放行不收起
    p.Show(POINT{390, 520});  // 面板 (390..794, 322..512)，远离主人窗口
    Pump(100);
    qy::SymbolPanel::DebugInjectMouse(WM_LBUTTONDOWN, hostCtr);
    Pump(500);
    Report(L"[1] 点主人窗口（状态条）面板保持显示", p.visible());

    // [2] 点面板第一个符号（等价收到 WM_LBUTTONDOWN）→ 输出并收起
    RECT pr{};
    GetWindowRect(p.hwnd(), &pr);
    SendMessageW(p.hwnd(), WM_LBUTTONDOWN, 0, MAKELPARAM(31, 68));
    Pump(400);
    Report(L"[2] 点符号输出并收起", !g_picked.empty() && !p.visible());

    // [3] 再弹出，钩子判定点在面板外（非主人窗口）→ 收起
    g_picked.clear();
    p.Show(POINT{120, 520});
    Pump(200);
    Report(L"[3a] 再次弹出显示", p.visible());
    qy::SymbolPanel::DebugInjectMouse(WM_LBUTTONDOWN, POINT{950, 300});
    Pump(400);
    Report(L"[3b] 点面板外收起", !p.visible());

    // [4] 再弹出，静置 1.2 秒 → 不能自己消失（老代码 ~360ms 必现误关）
    p.Show(POINT{120, 520});
    Pump(1200);
    Report(L"[4] 静置保持显示", p.visible());

    // [5] 显示期间点主人窗口 → 钩子放行，面板不被钩子收起
    qy::SymbolPanel::DebugInjectMouse(WM_LBUTTONDOWN, hostCtr);
    Pump(300);
    Report(L"[5] 点主人窗口面板仍在（由主人逻辑决定开关）", p.visible());

    // [6] 右键点面板外同样收起
    qy::SymbolPanel::DebugInjectMouse(WM_RBUTTONDOWN, POINT{950, 300});
    Pump(400);
    Report(L"[6] 右键点面板外收起", !p.visible());

    // 真实注入点击可达性诊断（沙箱里通常为 0，不影响上述判定）
    p.Show(POINT{120, 520});
    Pump(100);
    RealClickDiag(hostCtr);
    Pump(200);
    p.Hide();
    p.Destroy();
    DestroyWindow(g_host);

    {
        wchar_t extra[64];
        wsprintfW(extra, L"\r\n[diag] 主人窗口收到真实点击=%d（0 表示沙箱不投递注入鼠标事件）\r\n",
                  g_hostClicks);
        g_report += extra;
    }
    FILE* f = nullptr;
    _wfopen_s(&f, L"build/sym_test2.txt", L"wb");
    if (f) {
        fwrite(g_report.c_str(), sizeof(wchar_t), g_report.size(), f);
        fclose(f);
    }
    return 0;
}
