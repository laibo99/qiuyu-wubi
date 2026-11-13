#include <windows.h>
#include <stdio.h>
#include <string>
static bool SetKV(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& val, long* err) {
    HKEY k=nullptr; DWORD disp=0;
    LSTATUS st = RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, &disp);
    if (st != ERROR_SUCCESS) { *err = st; return false; }
    st = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(), (DWORD)((val.size()+1)*sizeof(wchar_t)));
    RegCloseKey(k);
    *err = st;
    return st == ERROR_SUCCESS;
}
int main(){
    SetConsoleOutputCP(CP_UTF8);
    long e=0;
    printf("HKCU classes: %d err=%ld\n", (int)SetKV(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E01}", nullptr, L"test", &e), e);
    printf("HKCU tip: %d err=%ld\n", (int)SetKV(HKEY_CURRENT_USER, L"Software\\Microsoft\\CTF\\TIP\\{7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E01}\\LanguageProfile\\0x00000804\\{7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E02}", L"Description", L"秋雨五笔", &e), e);
    printf("HKLM classes: %d err=%ld\n", (int)SetKV(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\CTF\\TIP\\{7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E01}", nullptr, L"test", &e), e);
    return 0;
}
