// 读取秋雨五笔的注册信息，确认安装后的注册状态
#include <windows.h>
#include <stdio.h>
#include <string>

static std::wstring ReadStr(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return L"<key missing>";
    wchar_t buf[MAX_PATH * 2]{};
    DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    std::wstring out;
    if (::RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS &&
        type == REG_SZ)
        out = buf;
    else
        out = L"<value missing>";
    ::RegCloseKey(k);
    return out;
}

static bool KeyExists(HKEY root, const wchar_t* sub) {
    HKEY k = nullptr;
    bool ok = ::RegOpenKeyExW(root, sub, 0, KEY_READ, &k) == ERROR_SUCCESS;
    if (k) ::RegCloseKey(k);
    return ok;
}

static void Print(const char* tag, const std::wstring& s) {
    char u8[MAX_PATH * 4]{};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u8, sizeof(u8) - 1,
                                  nullptr, nullptr);
    u8[n] = 0;
    printf("[%s] %s\n", tag, u8);
    fflush(stdout);
}

int main() {
    Print("InprocServer32(64)",
          ReadStr(HKEY_LOCAL_MACHINE,
                  L"SOFTWARE\\Classes\\CLSID\\{7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E01}"
                  L"\\InprocServer32",
                  nullptr));
    Print("InprocServer32(WOW64)",
          ReadStr(HKEY_LOCAL_MACHINE,
                  L"SOFTWARE\\Classes\\WOW6432Node\\CLSID\\{7E2F1C4A-9B3D-4A6E-8F52-"
                  L"1C3D5A7B9E01}\\InprocServer32",
                  nullptr));
    Print("TIP key exists",
          KeyExists(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\CTF\\TIP\\{7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E01}"
                    L"\\LanguageProfile\\0x00000804\\{7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E02}")
              ? L"yes"
              : L"NO");
    {  // Enable 是 DWORD
        HKEY k = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"SOFTWARE\\Microsoft\\CTF\\TIP\\{7E2F1C4A-9B3D-4A6E-8F52-"
                            L"1C3D5A7B9E01}\\LanguageProfile\\0x00000804\\{7E2F1C4A-9B3D-"
                            L"4A6E-8F52-1C3D5A7B9E02}",
                            0, KEY_READ, &k) == ERROR_SUCCESS) {
            DWORD v = 0, sz = sizeof(v), t = 0;
            if (::RegQueryValueExW(k, L"Enable", nullptr, &t, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
                Print("profile Enable", t == REG_DWORD ? (v ? L"1" : L"0") : L"<type?>");
            else
                Print("profile Enable", L"<missing>");
            ::RegCloseKey(k);
        } else
            Print("profile Enable", L"<key missing>");
    }
    // 枚举 HKCU 配置文件键的全部值（"0804:{CLSID}{GUID}" 是命名值）
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Control Panel\\International\\User Profile\\zh-Hans-CN", 0,
                        KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD idx = 0, namesz = 64, type = 0;
        wchar_t name[64]{};
        BYTE data[512]{};
        DWORD dsz = sizeof(data) - 2;
        while (::RegEnumValueW(k, idx++, name, &namesz, nullptr, &type, data, &dsz) ==
               ERROR_SUCCESS) {
            std::wstring line = std::wstring(name) + L" = " +
                                (type == REG_SZ ? (wchar_t*)data : L"<binary>");
            Print("HKCU value", line);
            namesz = 64;
            dsz = sizeof(data) - 2;
            data[0] = data[1] = 0;
        }
        ::RegCloseKey(k);
    } else {
        Print("HKCU value", L"<key missing>");
    }
    // 父键（User Profile）下也可能有 0804:{...}
    HKEY k2 = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\International\\User Profile", 0,
                        KEY_READ, &k2) == ERROR_SUCCESS) {
        DWORD idx = 0, namesz = 64, type = 0;
        wchar_t name[64]{};
        BYTE data[512]{};
        DWORD dsz = sizeof(data) - 2;
        while (::RegEnumValueW(k2, idx++, name, &namesz, nullptr, &type, data, &dsz) ==
               ERROR_SUCCESS) {
            std::wstring line = std::wstring(name) + L" = " +
                                (type == REG_SZ ? (wchar_t*)data : L"<binary>");
            Print("HKCU parent", line);
            namesz = 64;
            dsz = sizeof(data) - 2;
            data[0] = data[1] = 0;
        }
        ::RegCloseKey(k2);
    }
    return 0;
}
