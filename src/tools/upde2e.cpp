// 自动检测新版本的端到端自检
//
// 直接在测试进程里加载已注册的输入法组件并调用 ITfTextInputProcessor::Activate，
// 这等价于宿主程序激活输入法，会自动触发一次“启动时检测新版本”。
//
//   [A] version.txt 里的版本比当前新 → 必须弹出“发现新版本”确认框（不自动升级）
//   [B] version.txt 与当前版本相同   → 不得弹框打扰用户
//   [C] 两次检测都会把时间写回配置的 [更新] 上次检测
#include <windows.h>
#include <msctf.h>
#include <shlobj.h>
#include <stdio.h>
#include <string>
#include "../core/qy_update.h"

static const CLSID kClsid = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                             {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x01}};

static std::wstring g_out;
static int g_fail = 0;

static void Check(const wchar_t* name, bool ok, const wchar_t* detail = L"") {
    if (!ok) ++g_fail;
    g_out += std::wstring(ok ? L"PASS " : L"FAIL ") + name;
    if (detail && detail[0]) g_out += std::wstring(L"  → ") + detail;
    g_out += L"\r\n";
}

static std::wstring CfgPath() {
    wchar_t b[MAX_PATH * 2] = {0};
    ::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, b);
    std::wstring d = std::wstring(b) + L"\\QiuYuWubi";
    ::CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\config.ini";
}

static void SetIni(const wchar_t* sec, const wchar_t* key, const std::wstring& val) {
    ::WritePrivateProfileStringW(sec, key, val.c_str(), CfgPath().c_str());
}

static std::wstring ReadIni(const wchar_t* sec, const wchar_t* key) {
    wchar_t b[512] = {0};
    ::GetPrivateProfileStringW(sec, key, L"", b, 512, CfgPath().c_str());
    return b;
}

static bool WriteFileText(const std::wstring& path, const std::string& body) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    ::WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
    ::CloseHandle(h);
    return w == body.size();
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

// 找“发现新版本”确认框；找到就点“否”（代表用户暂不更新），返回是否出现过
static bool WaitNewerDialog(int ms, bool* dismissed) {
    DWORD t0 = ::GetTickCount();
    *dismissed = false;
    while (::GetTickCount() - t0 < (DWORD)ms) {
        HWND dlg = ::FindWindowW(L"#32770", L"发现新版本");
        if (dlg) {
            // 点“否”：MessageBox 的否按钮 ID = IDNO
            ::SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
            Pump(150);
            *dismissed = (::FindWindowW(L"#32770", L"发现新版本") == nullptr);
            return true;
        }
        Pump(50);
    }
    return false;
}

int main() {
    std::wstring dir = CfgPath();
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    std::wstring manifest = dir + L"\\update_test.txt";

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITfThreadMgr* ptm = nullptr;
    TfClientId tid = 0;
    HRESULT hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITfThreadMgr, (void**)&ptm);
    if (FAILED(hr) || !ptm) {
        Check(L"[0] 创建线程管理器", false);
        goto out;
    }
    ptm->Activate(&tid);
    {
        // ---------------- [A] 有新版本 ----------------
        bool okFile = WriteFileText(manifest,
                                    "version=9.9.9\n"
                                    "url=https://example.com/qywb/setup.exe\n"
                                    "notes=端到端测试\n");
        Check(L"[A0] 写入测试 version.txt", okFile);
        SetIni(L"更新", L"地址", manifest);
        SetIni(L"更新", L"自动检测", L"1");
        SetIni(L"更新", L"上次检测", L"0");

        ITfTextInputProcessor* pTip = nullptr;
        HRESULT hr2 =
            ::CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, IID_ITfTextInputProcessor,
                               (void**)&pTip);
        Check(L"[A1] 加载输入法组件", SUCCEEDED(hr2) && pTip != nullptr);

        bool dialogSeen = false, dismissed = false;
        if (SUCCEEDED(hr2) && pTip) {
            pTip->Activate(ptm, tid);
            dialogSeen = WaitNewerDialog(12000, &dismissed);
        }
        Check(L"[A2] 发现新版本时弹出确认框（不后台静默升级）", dialogSeen);
        Check(L"[A3] 确认框可点“否”取消更新", dismissed);
        {
            std::wstring t = ReadIni(L"更新", L"上次检测");
            long long v = _wtoi64(t.c_str());
            // 必须是合理的 unix 时间（2020 年以后），防止把“ld”这类脏值当成有效时间
            Check(L"[A4] 检测时间已按数字写回配置", v > 1600000000LL, t.c_str());
        }
        if (pTip) {
            pTip->Deactivate();
            pTip->Release();
        }

        // ---------------- [B] 已是最新 ----------------
        WriteFileText(manifest,
                      "version=3.5.0\n"
                      "url=https://example.com/qywb/setup.exe\n");
        SetIni(L"更新", L"上次检测", L"0");
        Pump(200);
        ITfTextInputProcessor* pTip2 = nullptr;
        HRESULT hr3 =
            ::CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, IID_ITfTextInputProcessor,
                               (void**)&pTip2);
        bool dialogSeen2 = false, dismissed2 = false;
        if (SUCCEEDED(hr3) && pTip2) {
            pTip2->Activate(ptm, tid);
            dialogSeen2 = WaitNewerDialog(6000, &dismissed2);
        }
        Check(L"[B1] 已是最新时不弹框打扰", !dialogSeen2);
        if (pTip2) {
            pTip2->Deactivate();
            pTip2->Release();
        }

        // 清理：测试用的更新地址；顺带恢复“上次检测”为 0，便于用户真机首次检查
        SetIni(L"更新", L"地址", L"");
        SetIni(L"更新", L"上次检测", L"0");
        ::DeleteFileW(manifest.c_str());
    }

out:
    if (ptm) {
        ptm->Deactivate();
        ptm->Release();
    }
    ::CoUninitialize();

    g_out += g_fail ? L"RESULT: FAIL\r\n" : L"RESULT: ALL PASS\r\n";
    FILE* f = nullptr;
    _wfopen_s(&f, L"build/upde2e.txt", L"wb");
    if (f) {
        std::string u8 = qy::WToUtf8(g_out);
        fwrite(u8.data(), 1, u8.size(), f);
        fclose(f);
    }
    return g_fail ? 1 : 0;
}
