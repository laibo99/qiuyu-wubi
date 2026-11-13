// 秋雨五笔输入法 - TSF 组件自检（不依赖键盘输入）
#include <windows.h>
#include <msctf.h>
#include <stdio.h>

static const CLSID kClsid = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                             {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x01}};

int main() {
    ::SetConsoleOutputCP(CP_UTF8);
    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    printf("CoInitialize: 0x%08lX\n", (unsigned long)hrCo);

    IUnknown* pUnk = nullptr;
    HRESULT hr = ::CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown,
                                    (void**)&pUnk);
    printf("CoCreateInstance(TIP): 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;

    ITfTextInputProcessor* pTip = nullptr;
    hr = pUnk->QueryInterface(IID_ITfTextInputProcessor, (void**)&pTip);
    printf("QueryInterface(ITfTextInputProcessor): 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 2;

    ITfThreadMgr* pTm = nullptr;
    hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                            (void**)&pTm);
    printf("ThreadMgr: 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 3;

    TfClientId tid = 0;
    hr = pTm->Activate(&tid);
    printf("ThreadMgr::Activate: 0x%08lX tid=%u\n", (unsigned long)hr, tid);

    hr = pTip->Activate(pTm, tid);
    printf("TIP::Activate: 0x%08lX\n", (unsigned long)hr);

    // 检查是否已注册为键盘输入法
    ITfInputProcessorProfiles* pProfiles = nullptr;
    hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                            IID_ITfInputProcessorProfiles, (void**)&pProfiles);
    if (SUCCEEDED(hr) && pProfiles) {
        IEnumTfLanguageProfiles* pEnum = nullptr;
        hr = pProfiles->EnumLanguageProfiles(0x0804, &pEnum);
        int n = 0;
        if (SUCCEEDED(hr) && pEnum) {
            TF_LANGUAGEPROFILE lp{};
            ULONG got = 0;
            while (pEnum->Next(1, &lp, &got) == S_OK && got == 1) {
                if (IsEqualCLSID(lp.clsid, kClsid)) {
                    ++n;
                    printf("  已注册语言配置 #%d profile=%08lX active=%d\n", n,
                           (unsigned long)lp.guidProfile.Data1, (int)lp.fActive);
                }
            }
            pEnum->Release();
        }
        printf("语言配置数量: %d\n", n);
        pProfiles->Release();
    }

    hr = pTip->Deactivate();
    printf("TIP::Deactivate: 0x%08lX\n", (unsigned long)hr);
    pTip->Release();
    pTm->Deactivate();
    pTm->Release();
    pUnk->Release();
    ::CoUninitialize();
    printf("自检完成\n");
    return 0;
}
