// MS Word 端到端验证：点状态条笑脸图标 → 点面板第一个符号 → 全选复制读剪贴板
//
// 只验证“在 Word 里点符号能不能真的上屏”。Word 必须已经切到秋雨五笔
//（状态条窗口出现在 Word 进程里），否则直接报“未激活”退出。
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

static std::wstring g_out;
static int g_fail = 0;

static void Log(const std::wstring& s) {
    g_out += s + L"\r\n";
}

static void Check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    if (!ok) ++g_fail;
    Log(std::wstring(ok ? L"PASS " : L"FAIL ") + name + (detail.empty() ? L"" : L"  → " + detail));
}

static void Pump(int ms) {
    DWORD t0 = ::GetTickCount();
    MSG msg;
    while (::GetTickCount() - t0 < (DWORD)ms) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(5);
    }
}

static DWORD PidOf(HWND h) {
    DWORD pid = 0;
    if (h) ::GetWindowThreadProcessId(h, &pid);
    return pid;
}

// 按类名找出属于指定进程的第一个可见窗口
struct FindCtx {
    const wchar_t* cls;
    DWORD pid;
    HWND found;
};
static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    auto* c = (FindCtx*)lp;
    wchar_t buf[128] = {0};
    ::GetClassNameW(h, buf, 128);
    if (::lstrcmpW(buf, c->cls) == 0 && PidOf(h) == c->pid) {
        c->found = h;
        return FALSE;
    }
    return TRUE;
}
static HWND FindInPid(const wchar_t* cls, DWORD pid) {
    FindCtx c{cls, pid, nullptr};
    ::EnumWindows(EnumProc, (LPARAM)&c);
    return c.found;
}

// 抢前台（沙箱里 SetForegroundWindow 常常被系统拒绝，用 AttachThreadInput 兜底）
static void ForceForeground(HWND w) {
    HWND fg = ::GetForegroundWindow();
    DWORD fgTid = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = ::GetCurrentThreadId();
    if (fgTid && fgTid != myTid) ::AttachThreadInput(myTid, fgTid, TRUE);
    ::ShowWindow(w, SW_RESTORE);
    ::BringWindowToTop(w);
    ::SetForegroundWindow(w);
    ::SetActiveWindow(w);
    if (fgTid && fgTid != myTid) ::AttachThreadInput(myTid, fgTid, FALSE);
    Pump(200);
    HWND now = ::GetForegroundWindow();
    wchar_t cls[128] = {0};
    if (now) ::GetClassNameW(now, cls, 128);
    Log(L"     当前前台窗口类=" + std::wstring(cls) + L"，目标是否在前台=" +
        ((now == w) ? L"是" : L"否"));
}

static void ClickAt(POINT pt) {
    ::SetCursorPos(pt.x, pt.y);
    Pump(40);
    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ::SendInput(2, in, sizeof(INPUT));
    Pump(60);
}

static void KeyCombo(WORD vk, bool ctrl) {
    INPUT in[4]{};
    int n = 0;
    if (ctrl) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = VK_CONTROL;
        ++n;
    }
    in[n].type = INPUT_KEYBOARD;
    in[n].ki.wVk = vk;
    ++n;
    in[n] = in[n - 1];
    in[n].ki.dwFlags = KEYEVENTF_KEYUP;
    ++n;
    if (ctrl) {
        in[n] = in[0];
        in[n].ki.dwFlags = KEYEVENTF_KEYUP;
        ++n;
    }
    ::SendInput(n, in, sizeof(INPUT));
    Pump(80);
}

// Win+空格：切换系统输入法（循环逐个试，直到切到秋雨五笔）
static void KeyWinSpace() {
    INPUT in[4]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_SPACE;
    in[2] = in[1];
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3] = in[0];
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(4, in, sizeof(INPUT));
    Pump(120);
}

static std::wstring ReadClipboard() {
    for (int i = 0; i < 20; ++i) {
        if (::OpenClipboard(nullptr)) {
            std::wstring out;
            HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
            if (h) {
                const wchar_t* p = (const wchar_t*)::GlobalLock(h);
                if (p) {
                    out = p;
                    ::GlobalUnlock(h);
                }
            }
            ::CloseClipboard();
            return out;
        }
        ::Sleep(30);
    }
    return std::wstring();
}

static void RunTest(const std::wstring& exe) {
    if (::GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Check(L"[0] 找到 WINWORD.EXE", false, exe);
        return;
    }

    // 已经开着就用现成的，避免多开一个进程
    HWND word = ::FindWindowW(L"OpusApp", nullptr);
    if (!word) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + exe + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                              &pi)) {
            Check(L"[0] 启动 Word", false, L"CreateProcess 失败");
            return;
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        for (int i = 0; i < 120 && !word; ++i) {  // 最多等 24 秒
            Pump(200);
            word = ::FindWindowW(L"OpusApp", nullptr);
        }
    }
    Check(L"[0] 找到 Word 主窗口", word != nullptr);
    if (!word) return;
    const DWORD wp = PidOf(word);

    ForceForeground(word);
    Pump(400);

    // 确保有打开的文档（启动画面时 Ctrl+N 新建）
    KeyCombo('N', true);
    Pump(1500);

    // Word 里必须已经切到秋雨五笔：状态条窗口出现在 Word 进程里
    HWND sb = nullptr;
    int tries = 0;
    for (int i = 0; i < 40 && !sb; ++i) {
        sb = FindInPid(L"QiuYuWubiStatusWnd", wp);
        if (sb) break;
        if (i % 4 == 3) {  // 每等 0.8 秒切一次输入法
            KeyWinSpace();
            ++tries;
        }
        Pump(200);
    }
    Log(L"     切换输入法次数=" + std::to_wstring(tries));
    Check(L"[1] Word 中已激活秋雨五笔（找到状态条窗口）", sb != nullptr,
          sb ? L"" : L"请先在 Word 里切到「秋雨五笔」再跑本测试");
    if (!sb) return;

    RECT sr{};
    ::GetWindowRect(sb, &sr);
    const int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    Log(L"     状态条 rect=" + std::to_wstring(sr.left) + L"," + std::to_wstring(sr.top) + L" " +
        std::to_wstring(sw) + L"x" + std::to_wstring(sh));

    // 笑脸图标格：逻辑坐标 105..138 / 172，纵向居中
    POINT smiley{};
    smiley.x = sr.left + (LONG)((105 + 138) * sw / 2 / 172);
    smiley.y = sr.top + sh / 2;
    ClickAt(smiley);
    Pump(400);

    HWND panel = FindInPid(L"QiuYuWubiSymWnd", wp);
    Check(L"[2] 点笑脸图标弹出符号面板", panel != nullptr && ::IsWindowVisible(panel));
    if (!panel) return;

    RECT pr{};
    ::GetWindowRect(panel, &pr);
    Log(L"     面板 rect=" + std::to_wstring(pr.left) + L"," + std::to_wstring(pr.top) + L" " +
        std::to_wstring(pr.right - pr.left) + L"x" + std::to_wstring(pr.bottom - pr.top));

    // 第一个符号格（面板内左上，跳过 10px 边距 + 26px 标签 + 8px 间距）
    POINT item{};
    item.x = pr.left + 30;
    item.y = pr.top + 44 + 17;
    ClickAt(item);
    Pump(600);

    Check(L"[3] 点符号后面板自动收起", !::IsWindowVisible(panel));

    // 全选复制：文档里应该只有刚插入的那个符号
    KeyCombo('A', true);
    KeyCombo('C', true);
    Pump(300);
    std::wstring body = ReadClipboard();
    while (!body.empty() && (body.back() == L'\r' || body.back() == L'\n')) body.pop_back();
    Check(L"[4] 符号已插入 Word 文档", !body.empty(),
          body.empty() ? L"剪贴板为空（可能没插进去）" : std::wstring(L"内容=") + body);
    Check(L"[5] 插入内容不是编码字母",
          body.find_first_of(L"abcdefghijklmnopqrstuvwxyz") == std::wstring::npos, body);
}

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring exe = L"C:////Program Files////Microsoft Office////root////Office16////WINWORD.EXE";
    if (argc > 1) {
        int n = ::MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        exe.resize(n ? n - 1 : 0);
        if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &exe[0], n);
    }
    RunTest(exe);

    FILE* f = nullptr;
    _wfopen_s(&f, L"build/wordtest.txt", L"wb");
    if (f) {
        std::string u8;
        int n = ::WideCharToMultiByte(CP_UTF8, 0, g_out.c_str(), (int)g_out.size(), nullptr, 0,
                                      nullptr, nullptr);
        u8.resize(n);
        ::WideCharToMultiByte(CP_UTF8, 0, g_out.c_str(), (int)g_out.size(), &u8[0], n, nullptr,
                              nullptr);
        fwrite(u8.data(), 1, u8.size(), f);
        fclose(f);
    }
    CoUninitialize();
    return g_fail ? 1 : 0;
}
