// MS Word 符号面板端到端验证（v3，用 Word COM 建文档 / 读正文）
//
// 覆盖用户报的两个问题：
//   1) 点符号时面板立刻消失 → 现在点完应该「保持打开」，可以连着点几个；
//   2) 符号没上屏（Office 把 TSF 焦点丢了，RequestEditSession 返回
//      TF_E_DISCONNECTED）→ 现在会自动用 document manager 把焦点接回来重试。
//
// 环境变量 QY_SYMBOL_SIM_DROP=1 时，输入法每次插入符号前会先主动
// SetFocus(NULL) 模拟 Office 丢焦点的状态（Word 由本程序启动，继承该环境变量），
// 用来验证“接回焦点再插入”这条修复路径。
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

static std::wstring g_out;
static int g_fail = 0;
static ULONGLONG g_traceStart = 0;

static void Log(const std::wstring& s) {
    g_out += s + L"\r\n";
}

static void Check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    if (!ok) ++g_fail;
    Log(std::wstring(ok ? L"PASS " : L"FAIL ") + name +
        (detail.empty() ? L"" : L"  → " + detail));
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

// 直接改 config.ini 的 [方案] 默认：0 五笔 / 1 拼音 / 2 混输
// 输入法每 700ms 核对一次配置文件，改完等 1.2s 就会按新方案工作
static void SetScheme(int v) {
    wchar_t ap[MAX_PATH] = {0};
    ::GetEnvironmentVariableW(L"APPDATA", ap, MAX_PATH);
    std::wstring p = std::wstring(ap) + L"\\QiuYuWubi\\config.ini";
    wchar_t val[8];
    ::wsprintfW(val, L"%d", v);
    ::WritePrivateProfileStringW(L"方案", L"默认", val, p.c_str());
}

static DWORD PidOf(HWND h) {
    DWORD pid = 0;
    if (h) ::GetWindowThreadProcessId(h, &pid);
    return pid;
}

// ---------------- Word COM（IDispatch 晚绑定） ----------------
static IDispatch* g_app = nullptr;

static bool DispId(IDispatch* o, const wchar_t* name, DISPID* id) {
    wchar_t* n = (wchar_t*)name;
    return o && SUCCEEDED(o->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, id));
}

static IDispatch* DispGetObj(IDispatch* o, const wchar_t* name) {
    DISPID id;
    if (!DispId(o, name, &id)) return nullptr;
    DISPPARAMS dp{nullptr, nullptr, 0, 0};
    VARIANT v;
    ::VariantInit(&v);
    IDispatch* r = nullptr;
    if (SUCCEEDED(o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &dp, &v,
                            nullptr, nullptr)) &&
        v.vt == VT_DISPATCH) {
        r = v.pdispVal;
    } else {
        ::VariantClear(&v);
    }
    return r;
}

static IDispatch* DispCallObj(IDispatch* o, const wchar_t* name) {
    DISPID id;
    if (!DispId(o, name, &id)) return nullptr;
    DISPPARAMS dp{nullptr, nullptr, 0, 0};
    VARIANT v;
    ::VariantInit(&v);
    IDispatch* r = nullptr;
    if (SUCCEEDED(o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &v, nullptr,
                            nullptr)) &&
        v.vt == VT_DISPATCH) {
        r = v.pdispVal;
    } else {
        ::VariantClear(&v);
    }
    return r;
}

static std::wstring DispStr(IDispatch* o, const wchar_t* name) {
    DISPID id;
    if (!DispId(o, name, &id)) return L"";
    DISPPARAMS dp{nullptr, nullptr, 0, 0};
    VARIANT v;
    ::VariantInit(&v);
    std::wstring r;
    if (SUCCEEDED(o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
                            DISPATCH_PROPERTYGET | DISPATCH_METHOD, &dp, &v, nullptr, nullptr)) &&
        v.vt == VT_BSTR && v.bstrVal)
        r = v.bstrVal;
    ::VariantClear(&v);
    return r;
}

static bool DispPutBool(IDispatch* o, const wchar_t* name, bool val) {
    DISPID id;
    if (!DispId(o, name, &id)) return false;
    VARIANT arg;
    ::VariantInit(&arg);
    arg.vt = VT_BOOL;
    arg.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
    DISPID put = DISPID_PROPERTYPUT;
    DISPPARAMS dp{&arg, &put, 1, 1};
    return SUCCEEDED(o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp,
                               nullptr, nullptr, nullptr));
}

static bool DispPutStr(IDispatch* o, const wchar_t* name, const std::wstring& s) {
    DISPID id;
    if (!DispId(o, name, &id)) return false;
    VARIANT arg;
    ::VariantInit(&arg);
    arg.vt = VT_BSTR;
    arg.bstrVal = ::SysAllocString(s.c_str());
    DISPID put = DISPID_PROPERTYPUT;
    DISPPARAMS dp{&arg, &put, 1, 1};
    bool ok = SUCCEEDED(o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &dp,
                                  nullptr, nullptr, nullptr));
    ::VariantClear(&arg);
    return ok;
}

static int DispGetInt(IDispatch* o, const wchar_t* name) {
    DISPID id;
    if (!DispId(o, name, &id)) return 0;
    DISPPARAMS dp{nullptr, nullptr, 0, 0};
    VARIANT v;
    ::VariantInit(&v);
    int r = 0;
    if (SUCCEEDED(o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &dp, &v,
                            nullptr, nullptr)) &&
        v.vt == VT_I4)
        r = v.lVal;
    ::VariantClear(&v);
    return r;
}

static bool DispCallInt(IDispatch* o, const wchar_t* name, int arg) {
    DISPID id;
    if (!DispId(o, name, &id)) return false;
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = arg;
    DISPPARAMS dp{&v, nullptr, 1, 0};
    return SUCCEEDED(o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, nullptr,
                               nullptr, nullptr));
}

static void DispCall(IDispatch* o, const wchar_t* name) {
    DISPID id;
    if (!DispId(o, name, &id)) return;
    DISPPARAMS dp{nullptr, nullptr, 0, 0};
    o->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, nullptr, nullptr, nullptr);
}

static IDispatch* WordApp() {
    if (g_app) return g_app;
    CLSID clsid;
    if (FAILED(::CLSIDFromProgID(L"Word.Application", &clsid))) return nullptr;
    IDispatch* p = nullptr;
    if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch,
                                  (void**)&p)))
        return nullptr;
    g_app = p;
    return p;
}

static std::wstring DocText(IDispatch* doc) {
    IDispatch* content = DispGetObj(doc, L"Content");
    if (!content) return L"";
    std::wstring t = DispStr(content, L"Text");
    content->Release();
    return t;
}

// ---------------- 窗口工具 ----------------
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

struct FindChildCtx {
    const wchar_t* cls;
    HWND found;
};
static BOOL CALLBACK EnumChildProc(HWND h, LPARAM lp) {
    auto* c = (FindChildCtx*)lp;
    wchar_t buf[128] = {0};
    ::GetClassNameW(h, buf, 128);
    if (::lstrcmpW(buf, c->cls) == 0) {
        c->found = h;
        return FALSE;
    }
    return TRUE;
}
static HWND FindChild(HWND parent, const wchar_t* cls) {
    FindChildCtx c{cls, nullptr};
    ::EnumChildWindows(parent, EnumChildProc, (LPARAM)&c);
    return c.found;
}

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
    Pump(150);
}

static std::wstring Trim(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\a' || c == L'\x0b') continue;
        r.push_back(c);
    }
    return r;
}

static std::wstring AppDataDir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = ::GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    return n ? std::wstring(buf) : std::wstring(L"C:\\Users\\Public");
}

static std::wstring TracePath() {
    return AppDataDir() + L"\\QiuYuWubi\\trace.log";
}

static void DumpNewTrace() {
    HANDLE h = ::CreateFileW(TracePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    if ((ULONGLONG)sz.QuadPart > g_traceStart) {
        DWORD n = (DWORD)((ULONGLONG)sz.QuadPart - g_traceStart);
        ::SetFilePointer(h, (LONG)g_traceStart, nullptr, FILE_BEGIN);
        if (n > 8000) n = 8000;
        std::string buf(n, 0);
        DWORD got = 0;
        ::ReadFile(h, &buf[0], n, &got, nullptr);
        buf.resize(got);
        int wn = ::MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), (int)buf.size(), nullptr, 0);
        std::wstring w;
        w.resize(wn > 0 ? wn : 0);
        if (wn > 0) ::MultiByteToWideChar(CP_UTF8, 0, buf.c_str(), (int)buf.size(), &w[0], wn);
        Log(L"--- 本次运行新增 trace ---");
        Log(Trim(w));
        Log(L"--- trace 结束 ---");
    }
    ::CloseHandle(h);
}

int main(int argc, char** argv) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        HANDLE h = ::CreateFileW(TracePath().c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                 nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            ::GetFileSizeEx(h, &sz);
            g_traceStart = (ULONGLONG)sz.QuadPart;
            ::CloseHandle(h);
        }
    }
    {
        wchar_t b[8] = {0};
        DWORD n = ::GetEnvironmentVariableW(L"QY_SYMBOL_SIM_DROP", b, 8);
        Log(std::wstring(L"模式：QY_SYMBOL_SIM_DROP=") +
            ((n > 0 && b[0] == L'1') ? L"1（模拟 Office 丢焦点）" : L"0（正常）"));
    }

    std::wstring exe = L"C:\\Program Files\\Microsoft Office\\root\\Office16\\WINWORD.EXE";
    if (argc > 1) {
        int n = ::MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
        exe.resize(n ? n - 1 : 0);
        if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &exe[0], n);
    }

    IDispatch* app = WordApp();
    IDispatch* doc = nullptr;
    Check(L"[0a] 连上 Word（COM）", app != nullptr);
    if (!app) goto done;
    DispPutBool(app, L"Visible", true);
    Pump(600);

    {
        IDispatch* docs = DispGetObj(app, L"Documents");
        Check(L"[0b] 拿到 Documents", docs != nullptr);
        if (!docs) goto done;
        doc = DispCallObj(docs, L"Add");
        docs->Release();
        Check(L"[0c] 新建文档", doc != nullptr);
        if (!doc) goto done;
        IDispatch* content = DispGetObj(doc, L"Content");
        if (content) {
            DispPutStr(content, L"Text", L"");
            content->Release();
        }
        Pump(800);
    }

    if (true) {
        // 用 COM 的 ActiveWindow.Hwnd 精确定位“这份文档”所在的窗口：
        // 机器上若还残留着别的 Word 进程，FindWindow(OpusApp) 会认错窗口
        HWND wwg = nullptr;
        {
            IDispatch* win = DispGetObj(app, L"ActiveWindow");
            if (win) {
                wwg = (HWND)(INT_PTR)DispGetInt(win, L"Hwnd");
                win->Release();
            }
        }
        HWND word = wwg ? (HWND)::GetAncestor(wwg, GA_ROOT) : ::FindWindowW(L"OpusApp", nullptr);
        if (!wwg) wwg = word ? FindChild(word, L"_WwG") : nullptr;
        Check(L"[1a] 找到 Word 主窗口", word != nullptr);
        if (!word) goto docdone;
        const DWORD wp = PidOf(word);
        ForceForeground(word);
        Pump(400);
        Log(std::wstring(L"     文档视图窗口 _WwG=") + (wwg ? L"找到" : L"未找到"));
        if (wwg) {
            RECT r{};
            ::GetWindowRect(wwg, &r);
            ClickAt(POINT{(r.left + r.right) / 2, r.top + (r.bottom - r.top) / 2});
            Pump(300);
        }

        HWND sb = nullptr;
        for (int i = 0; i < 40 && !sb; ++i) {
            sb = FindInPid(L"QiuYuWubiStatusWnd", wp);
            if (sb) break;
            if (i % 4 == 3) KeyWinSpace();
            Pump(200);
        }
        Check(L"[1b] Word 中已激活秋雨五笔（找到状态条窗口）", sb != nullptr,
              sb ? L"" : L"请先在 Word 里切到「秋雨五笔」再跑本测试");
        if (!sb) goto docdone;

        RECT sr{};
        ::GetWindowRect(sb, &sr);
        const int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
        Log(L"     状态条 rect=" + std::to_wstring(sr.left) + L"," + std::to_wstring(sr.top) +
            L" " + std::to_wstring(sw) + L"x" + std::to_wstring(sh));

        POINT smiley{};
        smiley.x = sr.left + (LONG)((105 + 138) * sw / 2 / 172);
        smiley.y = sr.top + sh / 2;
        {
            // 诊断：点击前状态条是否真的在屏幕上、前台是不是 Word
            DWORD fgPid = 0;
            HWND fg = ::GetForegroundWindow();
            if (fg) ::GetWindowThreadProcessId(fg, &fgPid);
            Log(L"     点击前: bar visible=" + std::to_wstring((int)::IsWindowVisible(sb)) +
                L" fg pid=" + std::to_wstring((unsigned long)fgPid) +
                L" word pid=" + std::to_wstring((unsigned long)wp));
        }
        ClickAt(smiley);
        Pump(500);
        {
            HWND panel2 = FindInPid(L"QiuYuWubiSymWnd", wp);
            Log(L"     点击后: panel=" + std::to_wstring((size_t)(void*)panel2) +
                L" visible=" +
                std::to_wstring(panel2 ? (int)::IsWindowVisible(panel2) : -1));
        }

        HWND panel = FindInPid(L"QiuYuWubiSymWnd", wp);
        Check(L"[2] 点笑脸弹出符号面板", panel != nullptr && ::IsWindowVisible(panel));
        if (!panel || !::IsWindowVisible(panel)) goto docdone;

        RECT pr{};
        ::GetWindowRect(panel, &pr);
        Log(L"     面板 rect=" + std::to_wstring(pr.left) + L"," + std::to_wstring(pr.top) + L" " +
            std::to_wstring(pr.right - pr.left) + L"x" + std::to_wstring(pr.bottom - pr.top));

        // 版式：kPad=10，标签 26 + 间距 8 → 格子区 y=44，格高 34，10 列，格宽 38，x0=12
        auto cellPt = [&](int idx) {
            int col = idx % 10, row = idx / 10;
            return POINT{pr.left + 12 + col * 38 + 19, pr.top + 44 + row * 34 + 17};
        };

        ClickAt(cellPt(0));  // 「，」
        Pump(700);
        std::wstring t1 = DocText(doc);
        Check(L"[3] 第一个符号已上屏", Trim(t1) == L"，", L"doc=【" + Trim(t1) + L"】");
        Check(L"[4] 插入后面板仍然打开（不用再点一次笑脸）",
              ::IsWindowVisible(panel) == TRUE);

        ClickAt(cellPt(1));  // 「。」
        Pump(600);
        std::wstring t2 = DocText(doc);
        Check(L"[5] 连点第二个符号也上屏", Trim(t2) == L"，。", L"doc=【" + Trim(t2) + L"】");
        Check(L"[6] 面板依旧保持打开", ::IsWindowVisible(panel) == TRUE);

        // 点文档正文（面板外）→ 面板收起
        POINT body{};
        body.x = (sr.left + pr.left) / 2;
        body.y = pr.top - 140;
        if (wwg) {
            RECT r{};
            ::GetWindowRect(wwg, &r);
            body.x = (r.left + r.right) / 2;
            body.y = r.top + (r.bottom - r.top) / 3;
        }
        // 沙箱里偶尔会丢一次鼠标事件，最多点 3 次
        for (int i = 0; i < 3 && ::IsWindowVisible(panel); ++i) {
            ClickAt(body);
            Pump(700);
        }
        Check(L"[7] 点面板外面后自动收起", ::IsWindowVisible(panel) == FALSE);

        ClickAt(smiley);
        Pump(600);
        bool open2 = panel && ::IsWindowVisible(panel);
        if (!open2) {
            Log(L"     第一次点击未弹出，再点一次");
            ClickAt(smiley);
            Pump(600);
            open2 = panel && ::IsWindowVisible(panel);
        }
        Check(L"[8] 再点笑脸能重新弹出", open2);
        if (open2) {
            ClickAt(cellPt(2));  // 「、」
            Pump(600);
            std::wstring t3 = DocText(doc);
            Check(L"[9] 重新打开后插入正常", Trim(t3) == L"，。、", L"doc=【" + Trim(t3) + L"】");
            ClickAt(body);  // 收起，进入下一轮
            Pump(400);
        }

        // ---- 带编码时点符号：编码字母不能留在文档里 ----
        {
            IDispatch* content = DispGetObj(doc, L"Content");
            if (content) {
                DispPutStr(content, L"Text", L"");
                content->Release();
            }
            Pump(300);
            if (wwg) {
                RECT r{};
                ::GetWindowRect(wwg, &r);
                ClickAt(POINT{(r.left + r.right) / 2, r.top + (r.bottom - r.top) / 2});
                Pump(300);
            }
            // 敲两个字母形成编码（此时应该是组合状态）
            INPUT in[4]{};
            in[0].type = INPUT_KEYBOARD;
            in[0].ki.wVk = 'W';
            in[1] = in[0];
            in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            in[2].type = INPUT_KEYBOARD;
            in[2].ki.wVk = 'Q';
            in[3] = in[2];
            in[3].ki.dwFlags = KEYEVENTF_KEYUP;
            ::SendInput(4, in, sizeof(INPUT));
            Pump(400);
            ClickAt(smiley);
            Pump(500);
            if (panel && ::IsWindowVisible(panel)) {
                ClickAt(cellPt(0));
                Pump(700);
                std::wstring t4 = DocText(doc);
                Check(L"[10] 带编码点符号：文档只剩符号，没有残留字母",
                      Trim(t4) == L"，", L"doc=【" + Trim(t4) + L"】");
            } else {
                Check(L"[10] 带编码点符号：面板能弹出", false);
            }
        }

        // ---- 五笔拼音混输：在 Word 里直接打拼音（nihao + 空格）----
        {
            IDispatch* content = DispGetObj(doc, L"Content");
            if (content) {
                DispPutStr(content, L"Text", L"");
                content->Release();
            }
            Pump(300);
            if (wwg) {
                RECT r{};
                ::GetWindowRect(wwg, &r);
                ClickAt(POINT{(r.left + r.right) / 2, r.top + (r.bottom - r.top) / 2});
                Pump(300);
            }
            const char* keys = "NIHAO";  // 逐键：n i h a o
            for (int i = 0; keys[i]; ++i) {
                INPUT in[2]{};
                in[0].type = INPUT_KEYBOARD;
                in[0].ki.wVk = keys[i];
                in[1] = in[0];
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                ::SendInput(2, in, sizeof(INPUT));
                Pump(120);
            }
            Pump(400);
            INPUT sp[2]{};
            sp[0].type = INPUT_KEYBOARD;
            sp[0].ki.wVk = VK_SPACE;
            sp[1] = sp[0];
            sp[1].ki.dwFlags = KEYEVENTF_KEYUP;
            ::SendInput(2, sp, sizeof(INPUT));
            Pump(600);
            std::wstring t5 = DocText(doc);
            Check(L"[11] 混输：Word 里打 nihao 能出“你好”", Trim(t5) == L"你好",
                  L"doc=【" + Trim(t5) + L"】");
        }

        // ---- 纯五笔方案：同样的按键不该冒出拼音（三个方案互不干扰）----
        {
            SetScheme(0);
            Pump(1300);  // 等输入法读到新方案
            IDispatch* content = DispGetObj(doc, L"Content");
            if (content) {
                DispPutStr(content, L"Text", L"");
                content->Release();
            }
            Pump(300);
            if (wwg) {
                RECT r{};
                ::GetWindowRect(wwg, &r);
                ClickAt(POINT{(r.left + r.right) / 2, r.top + (r.bottom - r.top) / 2});
                Pump(300);
            }
            const char* keys = "NIHAO";
            for (int i = 0; keys[i]; ++i) {
                INPUT in[2]{};
                in[0].type = INPUT_KEYBOARD;
                in[0].ki.wVk = keys[i];
                in[1] = in[0];
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                ::SendInput(2, in, sizeof(INPUT));
                Pump(120);
            }
            Pump(300);
            INPUT sp[2]{};
            sp[0].type = INPUT_KEYBOARD;
            sp[0].ki.wVk = VK_SPACE;
            sp[1] = sp[0];
            sp[1].ki.dwFlags = KEYEVENTF_KEYUP;
            ::SendInput(2, sp, sizeof(INPUT));
            Pump(600);
            std::wstring t6 = DocText(doc);
            Check(L"[12] 纯五笔方案：打 nihao 不出“你好”（方案互不干扰）",
                  t6.find(L"你好") == std::wstring::npos,
                  L"doc=【" + Trim(t6) + L"】");
            SetScheme(2);  // 恢复混输方案，后面手工测试仍是混输
            Pump(1200);
        }
    }

docdone:
    if (doc) {
        DispCallInt(doc, L"Close", 0);  // 不保存，别给下次测试留下文档
        doc->Release();
    }
    if (app) DispCall(app, L"Quit");  // 退出本次 Word，避免残留进程干扰下次测试
done:
    DumpNewTrace();
    FILE* f = nullptr;
    _wfopen_s(&f, L"build/wordtest2.txt", L"wb");
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
    if (g_app) g_app->Release();
    ::CoUninitialize();
    return g_fail ? 1 : 0;
}
