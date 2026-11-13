// 秋雨五笔输入法 - 端到端输入测试（带前台保护，避免按键落到其它程序）
#include <windows.h>
#include <msctf.h>
#include <stdio.h>
#include <string>

static const CLSID kClsid = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                             {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x01}};
static const GUID kWubi = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                           {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x02}};
static const GUID kPinyin = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                             {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x03}};

static HWND g_hwnd = nullptr, g_edit = nullptr;
static int g_step = 0, g_idx = 0;
static const char* g_keys = "sm";
static bool g_pinyin = false;

static void SendKey(WORD vk) {
    if (::GetForegroundWindow() != g_hwnd) {
        printf("[中止] 窗口不在前台，不发送按键\n");
        g_step = 9;
        return;
    }
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    ::SendInput(2, in, sizeof(INPUT));
}

static void Dump() {
    int len = ::GetWindowTextLengthW(g_edit);
    std::wstring buf(len + 1, L'\0');
    ::GetWindowTextW(g_edit, &buf[0], len + 1);
    buf.resize(wcslen(buf.c_str()));
    int n = ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), (int)buf.size(), nullptr, 0, nullptr,
                                  nullptr);
    std::string out(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), (int)buf.size(), &out[0], n, nullptr, nullptr);
    printf("[结果] 编辑框内容: \"%s\"\n", out.c_str());
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CREATE) {
        g_edit = ::CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE, 10, 10, 600,
                                   200, h, (HMENU)1, nullptr, nullptr);
        ::SendMessageW(g_edit, WM_SETFONT, (WPARAM)::GetStockObject(DEFAULT_GUI_FONT), TRUE);
        ::SetTimer(h, 1, 700, nullptr);
        return 0;
    }
    if (m == WM_TIMER) {
        if (g_step == 0) {
            ::ShowWindow(h, SW_SHOW);
            ::SetForegroundWindow(h);
            ::BringWindowToTop(h);
            ::SetFocus(g_edit);
            HRESULT hr1 = E_FAIL;
            ITfInputProcessorProfiles* pProf = nullptr;
            if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                             CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                                             (void**)&pProf)) &&
                pProf) {
                hr1 = pProf->ActivateLanguageProfile(kClsid, 0x0804, g_pinyin ? kPinyin : kWubi);
                pProf->Release();
            }
            printf("[激活] ActivateLanguageProfile hr=0x%08lX 前台=%d\n", (unsigned long)hr1,
                   (int)(::GetForegroundWindow() == h));
            g_step = 1;
            return 0;
        }
        if (g_step == 1) {
            size_t n = strlen(g_keys);
            if ((size_t)g_idx < n) {
                SendKey((WORD)g_keys[g_idx]);
                printf("[按键] %c\n", g_keys[g_idx]);
                ++g_idx;
            } else {
                SendKey(VK_SPACE);
                printf("[按键] 空格\n");
                g_step = 2;
                ::KillTimer(h, 1);
            }
            return 0;
        }
        if (g_step == 9) {
            ::KillTimer(h, 1);
            ::PostQuitMessage(2);
        }
        return 0;
    }
    if (m == WM_DESTROY) {
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "pinyin") {
        g_pinyin = true;
        g_keys = "qiuyu";
    }
    if (argc > 2) g_keys = argv[2];

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITfThreadMgr* pTm = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_ITfThreadMgr, (void**)&pTm)) &&
        pTm) {
        TfClientId tid = 0;
        pTm->Activate(&tid);
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QyInputTestHost2";
    ::RegisterClassExW(&wc);
    g_hwnd = ::CreateWindowExW(WS_EX_TOPMOST, L"QyInputTestHost2", L"秋雨五笔 输入测试",
                               WS_OVERLAPPEDWINDOW, 200, 150, 640, 300, nullptr, nullptr,
                               wc.hInstance, nullptr);
    ::ShowWindow(g_hwnd, SW_SHOW);

    MSG msg;
    DWORD start = ::GetTickCount();
    bool dumped = false;
    DWORD t2 = 0;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
        if (g_step == 2 && t2 == 0) t2 = ::GetTickCount();
        if (g_step == 2 && !dumped && ::GetTickCount() - t2 > 700) {
            Dump();
            dumped = true;
            ::PostQuitMessage(0);
        }
        if (::GetTickCount() - start > 15000) break;
    }
    if (pTm) pTm->Release();
    ::CoUninitialize();
    return 0;
}
