// 对照实验：用同样的方式激活不同 TIP，判断 ActivateLanguageProfile 失败是否环境导致
#include <windows.h>
#include <msctf.h>
#include <stdio.h>

static bool ParseGuid(const char* s, GUID* g) {
    unsigned long d1;
    unsigned int d2, d3;
    unsigned int b[8];
    if (sscanf(s, "{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2],
               &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    g->Data1 = d1;
    g->Data2 = (unsigned short)d2;
    g->Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; ++i) g->Data4[i] = (unsigned char)b[i];
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("用法: acttest <CLSID> <PROFILE-GUID> [langid-hex]\n");
        return 2;
    }
    GUID clsid{}, prof{};
    if (!ParseGuid(argv[1], &clsid) || !ParseGuid(argv[2], &prof)) {
        printf("GUID 解析失败\n");
        return 2;
    }
    LANGID lang = (argc > 3) ? (LANGID)strtoul(argv[3], nullptr, 16) : 0x0804;

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITfThreadMgr* pTm = nullptr;
    TfClientId tid = 0;
    HRESULT hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITfThreadMgr, (void**)&pTm);
    printf("ThreadMgr CoCreate  = 0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && pTm) {
        hr = pTm->Activate(&tid);
        printf("ThreadMgr Activate  = 0x%08lX tid=%u\n", (unsigned long)hr, tid);
    }

    ITfInputProcessorProfileMgr* pMgr = nullptr;
    hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                            IID_ITfInputProcessorProfileMgr, (void**)&pMgr);
    printf("ProfileMgr CoCreate = 0x%08lX\n", (unsigned long)hr);
    if (SUCCEEDED(hr) && pMgr) {
        hr = pMgr->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, lang, clsid, prof, nullptr,
                                   0);
        printf("ProfileMgr ActivateProfile = 0x%08lX\n", (unsigned long)hr);

        TF_INPUTPROCESSORPROFILE cur{};
        cur.dwProfileType = TF_PROFILETYPE_INPUTPROCESSOR;
        hr = pMgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &cur);
        printf("GetActiveProfile   = 0x%08lX clsid=%08lX-%04X-%04X langid=0x%04X\n",
               (unsigned long)hr, (unsigned long)cur.clsid.Data1, cur.clsid.Data2, cur.clsid.Data3,
               cur.langid);
        pMgr->Release();
    }

    ITfInputProcessorProfiles* pProf = nullptr;
    hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                            IID_ITfInputProcessorProfiles, (void**)&pProf);
    if (SUCCEEDED(hr) && pProf) {
        hr = pProf->ActivateLanguageProfile(clsid, lang, prof);
        printf("ActivateLanguageProfile    = 0x%08lX  (%s)\n", (unsigned long)hr,
               SUCCEEDED(hr) ? "成功" : "失败");
        pProf->Release();
    }
    if (pTm) {
        pTm->Deactivate();
        pTm->Release();
    }
    ::CoUninitialize();
    return 0;
}
