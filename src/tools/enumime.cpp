// 枚举系统中 0x0804（简体中文）的全部 TSF 输入法配置，确认秋雨五笔已注册并启用
#include <windows.h>
#include <msctf.h>
#include <stdio.h>
#include <string>

static void PrintGuid(const GUID& g, char* out) {
    sprintf(out, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned)g.Data1,
            g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4],
            g.Data4[5], g.Data4[6], g.Data4[7]);
}

int main() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITfInputProcessorProfiles* p = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITfInputProcessorProfiles, (void**)&p);
    if (FAILED(hr) || !p) {
        printf("CoCreateInstance failed 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    LANGID zh = 0x0804;
    IEnumTfLanguageProfiles* pe = nullptr;
    hr = p->EnumLanguageProfiles(zh, &pe);
    if (FAILED(hr) || !pe) {
        printf("EnumLanguageProfiles failed 0x%08lX\n", (unsigned long)hr);
        p->Release();
        ::CoUninitialize();
        return 1;
    }
    TF_LANGUAGEPROFILE prof{};
    ULONG fetched = 0;
    int total = 0, qy = 0;
    while (pe->Next(1, &prof, &fetched) == S_OK && fetched == 1) {
        ++total;
        char c[64], g[64];
        PrintGuid(prof.clsid, c);
        PrintGuid(prof.guidProfile, g);
        bool isQy = prof.clsid.Data1 == 0x7e2f1c4a;
        if (isQy) ++qy;
        printf("[%02d] clsid=%s profile=%s enabled=%d %s\n", total, c, g,
               (int)prof.fActive, isQy ? "<== 秋雨五笔" : "");
        fflush(stdout);
    }
    printf("total=%d qiyu=%d -> %s\n", total, qy, qy > 0 ? "OK" : "MISSING");
    pe->Release();
    p->Release();
    ::CoUninitialize();
    return 0;
}
