// 符号面板输出通道验证：在可控的 TSF 宿主（RichEdit）里点符号，看字有没有进文档
//
// 为什么要这个测试：MS Word 里点符号原本走 SendInput（Unicode 按键），
// Word 这种严格宿主不收，表现就是“面板消失了但没上屏”。现在 Office 系列
// 改走 TSF 提交。本测试用 RichEdit 做宿主，通过环境变量
//   QY_FORCE_TSF_SYMBOL=1  → 强制走 TSF 通道（模拟 Office）
//   不设                    → 走 SendInput 通道（微信等宿主）
// 两条通道都必须把符号插进去。
#include <windows.h>
#include <msctf.h>
#include <stdio.h>
#include <string>

static const CLSID kClsid = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                             {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x01}};
static const GUID kProfile = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                              {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x02}};

static std::wstring g_out;
static int g_fail = 0;
static HWND g_host = nullptr;
static HWND g_edit = nullptr;

static void Check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    if (!ok) ++g_fail;
    g_out += std::wstring(ok ? L"PASS " : L"FAIL ") + name +
             (detail.empty() ? L"" : L"  → " + detail) + L"\r\n";
}

// 读/写“输入法开/关”隔间（1=中文 0=英文）。用来模拟“新窗口里系统记着英文状态”
static bool ReadOpenClose(ITfThreadMgr* ptm, int* out) {
    if (!ptm || !out) return false;
    ITfCompartmentMgr* cm = nullptr;
    if (FAILED(ptm->QueryInterface(IID_ITfCompartmentMgr, (void**)&cm)) || !cm) return false;
    bool ok = false;
    ITfCompartment* c = nullptr;
    if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &c)) && c) {
        VARIANT v;
        ::VariantInit(&v);
        if (SUCCEEDED(c->GetValue(&v)) && v.vt == VT_I4) {
            *out = v.lVal;
            ok = true;
        }
        ::VariantClear(&v);
        c->Release();
    }
    cm->Release();
    return ok;
}

static bool WriteOpenClose(ITfThreadMgr* ptm, TfClientId tid, int val) {
    if (!ptm) return false;
    ITfCompartmentMgr* cm = nullptr;
    if (FAILED(ptm->QueryInterface(IID_ITfCompartmentMgr, (void**)&cm)) || !cm) return false;
    bool ok = false;
    ITfCompartment* c = nullptr;
    if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &c)) && c) {
        VARIANT v;
        ::VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = val;
        ok = SUCCEEDED(c->SetValue(tid, &v));
        ::VariantClear(&v);
        c->Release();
    }
    cm->Release();
    return ok;
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

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) {
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
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

static void ForceForeground(HWND w) {
    HWND fg = ::GetForegroundWindow();
    DWORD fgTid = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myTid = ::GetCurrentThreadId();
    if (fgTid && fgTid != myTid) ::AttachThreadInput(myTid, fgTid, TRUE);
    ::ShowWindow(w, SW_RESTORE);
    ::BringWindowToTop(w);
    ::SetForegroundWindow(w);
    if (fgTid && fgTid != myTid) ::AttachThreadInput(myTid, fgTid, FALSE);
    Pump(200);
}

static std::wstring EditText() {
    int len = ::GetWindowTextLengthW(g_edit);
    std::wstring s(len + 1, L'\0');
    ::GetWindowTextW(g_edit, &s[0], len + 1);
    s.resize((size_t)len);
    return s;
}

int main() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 变量都放在最前面：下面有 goto out 清理跳转
    RECT sr{}, pr{};
    POINT smiley{}, item{};
    int sw = 0, sh = 0;
    HWND sb = nullptr, panel = nullptr;
    ITfThreadMgr* ptm = nullptr;
    TfClientId tid = 0;

    // ---- 宿主：一个带 RichEdit 的窗口（RichEdit 自带 TSF 文本存储）----
    ::LoadLibraryW(L"Msftedit.dll");
    HINSTANCE hi = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hi;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"QyTsfHostTest";
    ::RegisterClassExW(&wc);
    g_host = ::CreateWindowExW(0, L"QyTsfHostTest", L"符号输出验证", WS_OVERLAPPEDWINDOW, 200, 150,
                               640, 420, nullptr, nullptr, hi, nullptr);
    if (!g_host) {
        Check(L"[0] 创建宿主窗口", false);
        goto out;
    }

    ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                       (void**)&ptm);
    if (ptm) ptm->Activate(&tid);

    // 线程里先有 ThreadMgr，RichEdit 才会把自己接进 TSF
    g_edit = ::CreateWindowExW(0, L"RICHEDIT50W", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                                   ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                               10, 10, 600, 340, g_host, nullptr, hi, nullptr);
    Check(L"[0] 创建 RichEdit 宿主", g_edit != nullptr);
    if (!g_edit) goto out;

    ::ShowWindow(g_host, SW_SHOW);
    ForceForeground(g_host);
    ::SetFocus(g_edit);
    Pump(600);

    // ---- 先模拟“新窗口里系统还记着英文(0)”----
    {
        bool w = WriteOpenClose(ptm, tid, 0);
        int v = -1;
        bool r = ReadOpenClose(ptm, &v);
        Check(L"[0b] 预置系统状态为英文(0)", w && r && v == 0,
              L"write=" + std::to_wstring(w) + L" read=" + std::to_wstring(v));
    }

    // ---- 激活本输入法（等价于宿主切到秋雨五笔）----
    {
        ITfInputProcessorProfileMgr* pm = nullptr;
        ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                           IID_ITfInputProcessorProfileMgr, (void**)&pm);
        HRESULT hr = E_FAIL;
        if (pm) {
            // 0 这个 dwFlags 在部分系统上会返回 E_INVALIDARG，先试 FORPROCESS
            hr = pm->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, kClsid, kProfile,
                                     nullptr, 0x10000000 /* TF_IPPMF_FORPROCESS */);
            if (FAILED(hr))
                hr = pm->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, kClsid, kProfile,
                                         nullptr, 0);
            pm->Release();
        }
        if (FAILED(hr)) {
            // 老接口兜底
            ITfInputProcessorProfiles* pp = nullptr;
            ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                               IID_ITfInputProcessorProfiles, (void**)&pp);
            if (pp) {
                hr = pp->ActivateLanguageProfile(kClsid, 0x0804, kProfile);
                pp->Release();
            }
        }
        Check(L"[1] 激活秋雨五笔（TSF 配置与语言栏）", SUCCEEDED(hr));
        wchar_t b[32];
        ::wsprintfW(b, L"hr=0x%08lx", (unsigned long)hr);
        g_out += std::wstring(L"     ActivateProfile ") + b + L"\r\n";
    }
    Pump(400);
    // 激活配置后重新给一次焦点，TSF 才会把上下文交给本输入法
    ::SetFocus(g_edit);
    Pump(1200);

    {
        int v = -1;
        bool r = ReadOpenClose(ptm, &v);
        Check(L"[1b] 新窗口默认中文：系统状态被改回 1", r && v == 1,
              L"read=" + std::to_wstring(v));
    }

    sb = ::FindWindowW(L"QiuYuWubiStatusWnd", nullptr);
    Check(L"[2] 状态条已出现", sb != nullptr);
    if (!sb) goto out;

    // ---- 点笑脸 → 点第一个符号 ----
    ::GetWindowRect(sb, &sr);
    sw = sr.right - sr.left;
    sh = sr.bottom - sr.top;
    smiley.x = sr.left + (LONG)((105 + 138) * sw / 2 / 172);
    smiley.y = sr.top + sh / 2;
    ClickAt(smiley);
    Pump(400);

    {
        panel = ::FindWindowW(L"QiuYuWubiSymWnd", nullptr);
        Check(L"[3] 点笑脸图标弹出符号面板", panel != nullptr && ::IsWindowVisible(panel));
        if (!panel) goto out;
        ::GetWindowRect(panel, &pr);
        item.x = pr.left + 30;
        item.y = pr.top + 44 + 17;
        ClickAt(item);
        Pump(600);
        // v3.7 起：点符号后面板保持打开，可以连着点几个（用户反馈每次都要重开）
        Check(L"[4] 点符号后面板保持打开", ::IsWindowVisible(panel) == TRUE);
        // 点宿主文档（面板外）才收起
        RECT hr{}, pn{};
        ::GetWindowRect(g_host, &hr);
        ::GetWindowRect(panel, &pn);
        POINT outside{(hr.left + hr.right) / 2, (hr.top + hr.bottom) / 2};
        {
            wchar_t b[256];
            ::wsprintfW(b, L"     宿主=%ld,%ld %ldx%ld 面板=%ld,%ld %ldx%ld 点=%ld,%ld",
                        hr.left, hr.top, hr.right - hr.left, hr.bottom - hr.top, pn.left, pn.top,
                        pn.right - pn.left, pn.bottom - pn.top, outside.x, outside.y);
            g_out += std::wstring(b) + L"\r\n";
        }
        // 沙箱里注入的鼠标事件偶尔会丢一次，最多点三次（真实用户点一下就该收起）
        for (int tryNo = 0; tryNo < 3 && ::IsWindowVisible(panel); ++tryNo) {
            ClickAt(POINT{outside.x + tryNo * 7, outside.y});
            for (int i = 0; i < 5 && ::IsWindowVisible(panel); ++i) Pump(200);
        }
        Check(L"[4b] 点面板外面才收起", ::IsWindowVisible(panel) == FALSE);
    }

    // ---- 符号必须真的进到文档里 ----
    {
        std::wstring t = EditText();
        Check(L"[5] 符号已插入宿主文档", t.find(L"，") != std::wstring::npos, t);
        Check(L"[6] 文档里没有残留编码字母",
              t.find_first_of(L"abcdefghijklmnopqrstuvwxyz") == std::wstring::npos, t);
    }

    // ---- 第二轮：先打一个编码（形成组合），再点符号 ----
    {
        ::SetWindowTextW(g_edit, L"");
        Pump(150);
        // 确保处于中文状态（“中/英”胶囊在状态条最左侧），否则敲 a 只会打出一个普通字母。
        // 注意：v3.8 起新窗口默认就是中文，这里不能再“盲点一下”——那会切成英文
        POINT cn{};
        cn.x = sr.left + (LONG)((0 + 43) * sw / 2 / 172);
        cn.y = sr.top + sh / 2;
        int openNow = -1;
        if (ReadOpenClose(ptm, &openNow) && openNow == 0) {
            ClickAt(cn);
            Pump(400);
        }
        int openChk = -1;
        Check(L"[6b] 打字前处于中文状态", ReadOpenClose(ptm, &openChk) && openChk == 1,
              L"open=" + std::to_wstring(openChk));

        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = 'A';
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        ::SendInput(2, in, sizeof(INPUT));
        Pump(400);

        ClickAt(smiley);
        Pump(400);
        panel = ::FindWindowW(L"QiuYuWubiSymWnd", nullptr);
        Check(L"[7] 第二轮：带编码时面板仍能弹出", panel != nullptr && ::IsWindowVisible(panel));
        if (panel) {
            ::GetWindowRect(panel, &pr);
            item.x = pr.left + 30;
            item.y = pr.top + 44 + 17;
            ClickAt(item);
            Pump(600);
            std::wstring t = EditText();
            Check(L"[8] 带编码点符号：编码被清掉、符号插入", t.find(L"，") != std::wstring::npos, t);
            Check(L"[9] 带编码点符号：没有残留字母",
                  t.find_first_of(L"abc") == std::wstring::npos, t);
        }
    }

out:
    if (g_host) ::DestroyWindow(g_host);
    FILE* f = nullptr;
    _wfopen_s(&f, L"build/symoffice.txt", L"wb");
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
    ::CoUninitialize();
    return g_fail ? 1 : 0;
}
