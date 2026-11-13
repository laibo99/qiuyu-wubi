// 秋雨五笔输入法 - 注册/卸载工具（替代 regsvr32，便于受控调用）
#include <windows.h>
#include <stdio.h>
#include <string>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

int main() {
    ::SetConsoleOutputCP(CP_UTF8);
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) argc = 0;
    std::wstring dll;
    bool unreg = false;
    bool perUser = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"/u" || a == L"-u")
            unreg = true;
        else if (a == L"/user")
            perUser = true;
        else
            dll = a;
    }
    if (dll.empty()) {
        wchar_t self[MAX_PATH * 2];
        ::GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
        std::wstring p(self);
        p = p.substr(0, p.find_last_of(L"\\/"));
        dll = p + L"\\qywb64.dll";
    }
    HMODULE h = ::LoadLibraryW(dll.c_str());
    if (!h) {
        printf("载入失败 %lu: %ls\n", ::GetLastError(), dll.c_str());
        return 1;
    }
    HRESULT hr = E_FAIL;
    if (unreg) {
        typedef HRESULT(WINAPI * PFN)();
        PFN fn = (PFN)::GetProcAddress(h, "DllUnregisterServer");
        if (fn) hr = fn();
        printf("卸载: 0x%08lX\n", (unsigned long)hr);
    } else {
        typedef HRESULT(WINAPI * PFN)();
        PFN fn = (PFN)::GetProcAddress(h, "DllRegisterServer");
        if (fn) hr = fn();
        printf("注册: 0x%08lX\n", (unsigned long)hr);
    }
    ::FreeLibrary(h);
    LocalFree(argv);
    return SUCCEEDED(hr) ? 0 : 2;
}
