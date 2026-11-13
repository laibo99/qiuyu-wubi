// 秋雨五笔输入法 - 端到端输入测试（TSF 宿主）
// 创建编辑框 -> 激活本输入法 -> 模拟按键 -> 读回文本
#include <windows.h>
#include <msctf.h>
#include <stdio.h>
#include <string>

static const CLSID kClsid = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                             {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x01}};
static const GUID kProfileWubi = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                                  {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x02}};

static HWND g_hwnd = nullptr;
static HWND g_edit = nullptr;
static int g_step = 0;
static DWORD g_t2 = 0;
static const char* g_keys = "qiuyu";  // 拼音模式或五笔模式各测一次
static bool g_pinyin = false;

static void SendKey(WORD vk) {
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
    // 转 UTF-8 输出
    int n = ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), (int)buf.size(), nullptr, 0, nullptr,
                                  nullptr);
    std::string out(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), (int)buf.size(), &out[0], n, nullptr, nullptr);
    printf("[结果] 编辑框内容: \"%s\"\n", out.c_str());
    fflush(stdout);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            g_edit = ::CreateWindowExW(0, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                                           ES_AUTOVSCROLL,
                                       10, 10, 600, 200, h, (HMENU)1, nullptr, nullptr);
            ::SendMessageW(g_edit, WM_SETFONT,
                           (WPARAM)::GetStockObject(DEFAULT_GUI_FONT), TRUE);
            ::SetTimer(h, 1, 800, nullptr);
            return 0;
        case WM_TIMER: {
            if (g_step == 0) {
                ::SetForegroundWindow(h);
                ::SetFocus(g_edit);
                HRESULT hr = E_FAIL, hr2 = E_FAIL;
                ITfInputProcessorProfiles* pProf = nullptr;
                if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                                 CLSCTX_INPROC_SERVER,
                                                 IID_ITfInputProcessorProfiles, (void**)&pProf)) &&
                    pProf) {
                    hr2 = pProf->ActivateLanguageProfile(kClsid, 0x0804,
                                                        g_pinyin ? GUID{0x7e2f1c4a, 0x9b3d, 0x4a6e,
                                                                        {0x8f, 0x52, 0x1c, 0x3d, 0x5a,
                                                                         0x7b, 0x9e, 0x03}}
                                                                 : kProfileWubi);
                    pProf->Release();
                }
                printf("[激活] ActivateLanguageProfile hr=0x%08lX\n", (unsigned long)hr2);
                ITfInputProcessorProfileMgr* pMgr = nullptr;
                if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                                 CLSCTX_INPROC_SERVER,
                                                 IID_ITfInputProcessorProfileMgr, (void**)&pMgr)) &&
                    pMgr) {
                    hr = pMgr->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, kClsid,
                                               g_pinyin ? GUID{0x7e2f1c4a, 0x9b3d, 0x4a6e,
                                                               {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b,
                                                                0x9e, 0x03}}
                                                        : kProfileWubi,
                                               nullptr, 0);
                    pMgr->Release();
                }
                printf("[激活] ActivateProfile hr=0x%08lX 前景=%d\n", (unsigned long)hr,
                       (int)(::GetForegroundWindow() == h));
                fflush(stdout);
                g_step = 1;
                return 0;
            }
            if (g_step == 1) {
                // 逐字发送
                static size_t idx = 0;
                if (idx < strlen(g_keys)) {
                    SendKey((WORD)g_keys[idx]);
                    printf("[按键] %c\n", g_keys[idx]);
                    ++idx;
                    if (idx == strlen(g_keys)) Dump();
                    return 0;
                }
                SendKey(VK_SPACE);  // 上屏首选
                g_step = 2;
                g_t2 = ::GetTickCount();
                ::KillTimer(h, 1);
                return 0;
            }
            return 0;
        }
        case WM_TIMER + 100:
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "pinyin") g_pinyin = true;

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
    wc.lpszClassName = L"QyInputTestHost";
    ::RegisterClassExW(&wc);
    g_hwnd = ::CreateWindowExW(0, L"QyInputTestHost", L"秋雨五笔 输入测试", WS_OVERLAPPEDWINDOW,
                               100, 100, 700, 320, nullptr, nullptr, wc.hInstance, nullptr);
    ::ShowWindow(g_hwnd, SW_SHOW);
    ::UpdateWindow(g_hwnd);

    // 读取结果用第二个定时器消息循环
    MSG msg;
    DWORD start = ::GetTickCount();
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
        if (g_step == 2 && ::GetTickCount() - g_t2 > 900) {
            Dump();
            break;
        }
        if (::GetTickCount() - start > 20000) {
            printf("[超时]\n");
            break;
        }
    }
    if (pTm) pTm->Release();
    ::CoUninitialize();
    return 0;
}
