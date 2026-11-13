// 注册诊断工具：LoadLibrary 后直接调用 DllRegisterServer / DllUnregisterServer
#include <windows.h>
#include <stdio.h>
int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: regtool <dll> <reg|unreg>\n"); return 1; }
    wchar_t path[MAX_PATH], mode[16];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path, MAX_PATH);
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, mode, 16);
    HMODULE h = LoadLibraryW(path);
    if (!h) { printf("LoadLibrary failed GLE=%lu\n", GetLastError()); return 2; }
    FARPROC fn = GetProcAddress(h, _wcsicmp(mode, L"reg") == 0 ? "DllRegisterServer"
                                                               : "DllUnregisterServer");
    if (!fn) { printf("export missing\n"); return 3; }
    HRESULT hr = ((HRESULT(WINAPI*)())fn)();
    printf("HRESULT=0x%08lX %s\n", (unsigned long)hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    FreeLibrary(h);
    return SUCCEEDED(hr) ? 0 : 4;
}
