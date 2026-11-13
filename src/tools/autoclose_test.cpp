// 安装窗口“完成后自动关闭”自检
//
// 真机驱动安装程序界面：点“开始安装” → 勾选确认框并点“确定” → 之后
// 不再碰窗口，等待它自己消失。若超时仍存在，说明自动关闭没生效。
#include <windows.h>
#include <stdio.h>
#include <string>

static std::wstring g_out;
static int g_fail = 0;

static void Check(const wchar_t* name, bool ok, const wchar_t* detail = L"") {
    if (!ok) ++g_fail;
    g_out += std::wstring(ok ? L"PASS " : L"FAIL ") + name;
    if (detail && detail[0]) g_out += std::wstring(L"  → ") + detail;
    g_out += L"\r\n";
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

static HWND FindByClass(const wchar_t* cls, int timeoutMs) {
    DWORD t0 = ::GetTickCount();
    while (::GetTickCount() - t0 < (DWORD)timeoutMs) {
        HWND h = ::FindWindowW(cls, nullptr);
        if (h && ::IsWindowVisible(h)) return h;
        Pump(50);
    }
    return nullptr;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // 命令行用宽字符解析：监控路径含中文，mingw 的 argv 是 ANSI，按 UTF-8 转会变乱码
    std::wstring setup;
    {
        std::wstring cmdline = ::GetCommandLineW();
        std::wstring rest;
        if (!cmdline.empty() && cmdline[0] == L'"') {
            size_t e = cmdline.find(L'"', 1);
            rest = (e == std::wstring::npos) ? L"" : cmdline.substr(e + 1);
        } else {
            size_t sp = cmdline.find(L' ');
            rest = (sp == std::wstring::npos) ? L"" : cmdline.substr(sp + 1);
        }
        while (!rest.empty() && rest[0] == L' ') rest.erase(rest.begin());
        if (!rest.empty() && rest[0] == L'"') {
            size_t e = rest.find(L'"', 1);
            setup = (e == std::wstring::npos) ? rest.substr(1) : rest.substr(1, e - 1);
        } else {
            // msys/bash 常常把引号去掉，rest 里会带上后面的 --uninstall；
            // 按 ".exe" 截一下，别把参数当成文件名（否则 CreateProcess err=2）
            setup = rest;
            size_t ex = setup.find(L".exe");
            if (ex != std::wstring::npos) setup.resize(ex + 4);
        }
    }
    // 第二个参数 --uninstall：走卸载流程（点“卸载”按钮 2002）
    bool uninstallMode = ::wcsstr(::GetCommandLineW(), L"--uninstall") != nullptr;
    const UINT btnId = uninstallMode ? 2002u : 2001u;
    if (setup.empty()) {
        Check(L"[0] 传入安装程序路径", false, L"用法: autoclose_test <setup.exe> [--uninstall]");
    } else {
        std::wstring cmd = L"\"" + setup + L"\" --elevated";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring buf = cmd;
        if (!::CreateProcessW(nullptr, &buf[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                              &pi)) {
            wchar_t eb[64];
            ::wsprintfW(eb, L"CreateProcess 失败 err=%lu", (unsigned long)::GetLastError());
            Check(L"[1] 启动安装程序界面", false, eb);
            goto out;
        }
        ::CloseHandle(pi.hThread);

        HWND w = FindByClass(L"QySetupWnd", 8000);
        Check(L"[1] 安装窗口已打开", w != nullptr);

        bool started = false, confirmed = false, closed = false;
        DWORD closeTick = 0;
        if (w) {
            // 点“开始安装”（按钮 ID 2001）
            // 必须 PostMessage：该消息的处理器里会进入确认框的模态循环，
            // SendMessage 会一直阻塞到确认框关闭，导致我们没法去点它
            ::PostMessageW(w, WM_COMMAND, MAKEWPARAM(btnId, BN_CLICKED), 0);
            started = true;
            HWND dlg = FindByClass(L"QyConfirmWnd", 6000);
            Check(L"[2] 弹出操作前确认框", dlg != nullptr);
            if (dlg) {
                HWND chk = ::GetDlgItem(dlg, 3001);
                if (chk) {
                    ::SendMessageW(chk, BM_CLICK, 0, 0);
                    Pump(120);
                }
                HWND ok = ::GetDlgItem(dlg, IDOK);
                if (ok && ::IsWindowEnabled(ok)) {
                    ::SendMessageW(ok, BM_CLICK, 0, 0);
                    confirmed = true;
                }
            }
            Check(L"[3] 已确认并开始执行", confirmed);

            // 之后完全不碰窗口，等它自己关（安装 + 3 秒计时，给足 90 秒）
            DWORD t0 = ::GetTickCount();
            while (::GetTickCount() - t0 < 90000) {
                if (!::IsWindow(w)) {
                    closed = true;
                    closeTick = ::GetTickCount() - t0;
                    break;
                }
                // 若出现错误提示框，点掉它，避免卡住
                HWND err = ::FindWindowW(L"#32770", L"秋雨五笔输入法");
                if (err) ::SendMessageW(err, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                Pump(100);
            }
            wchar_t d[64];
            ::wsprintfW(d, L"%lu 秒后自行关闭", (unsigned long)((closeTick + 500) / 1000));
            Check(L"[4] 完成后窗口自动关闭（无需手动关）", closed, closed ? d : L"超时仍在");
        }
        if (!closed && pi.hProcess) {
            ::TerminateProcess(pi.hProcess, 0);
        }
        ::WaitForSingleObject(pi.hProcess, 5000);
        ::CloseHandle(pi.hProcess);
        (void)started;
    }

out:
    g_out += g_fail ? L"RESULT: FAIL\r\n" : L"RESULT: ALL PASS\r\n";
    FILE* f = nullptr;
    _wfopen_s(&f, L"build/autoclose.txt", L"wb");
    if (f) {
        int n = ::WideCharToMultiByte(CP_UTF8, 0, g_out.c_str(), (int)g_out.size(), nullptr, 0,
                                     nullptr, nullptr);
        std::string u8(n, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, g_out.c_str(), (int)g_out.size(), &u8[0], n, nullptr,
                              nullptr);
        fwrite(u8.data(), 1, u8.size(), f);
        fclose(f);
    }
    return g_fail ? 1 : 0;
}
