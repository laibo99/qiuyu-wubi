import io

p = r'src/tools/qysetup.cpp'
s = open(p, encoding='utf-8').read()

helper_fn = '''// 32 位 DLL 必须由 32 位进程注册：调用随包安装的 qyreg32.exe 辅助程序
static bool Run32BitHelper(const std::wstring& helper, const std::wstring& dllPath,
                           bool unregister) {
    if (::GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring cmd = L"\\"" + helper + L"\\" " + (unregister ? L"-u " : L"") + L"\\"" + dllPath +
                       L"\\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &si, &pi))
        return false;
    ::WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return code == 0;
}

static bool DoInstall() {'''

s = s.replace('static bool DoInstall() {', helper_fn, 1)

old32 = '''    std::wstring dll32 = g_installDir + L"\\\\qywb32.dll";
    if (::GetFileAttributesW(dll32.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CallDllRegister(dll32, false, &err))
            Log(L"  已注册 32 位输入法组件");
        else
            Log(L"[警告] 32 位注册失败：" + err);
    }'''
new32 = '''    std::wstring dll32 = g_installDir + L"\\\\qywb32.dll";
    if (::GetFileAttributesW(dll32.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (Run32BitHelper(g_installDir + L"\\\\qyreg32.exe", dll32, false))
            Log(L"  已注册 32 位输入法组件");
        else
            Log(L"[警告] 32 位组件注册失败，32 位程序可能无法使用本输入法");
    }'''
assert old32 in s
s = s.replace(old32, new32)

old_un = '''    CallDllRegister(dir + L"\\\\qywb32.dll", true, &err);
    CallDllRegister(dir + L"\\\\qywb64.dll", true, &err);'''
new_un = '''    Run32BitHelper(dir + L"\\\\qyreg32.exe", dir + L"\\\\qywb32.dll", true);
    CallDllRegister(dir + L"\\\\qywb64.dll", true, &err);'''
assert old_un in s
s = s.replace(old_un, new_un)

s = s.replace('''    const wchar_t* names[] = {L"qydemo.exe", L"qywb64.dll", L"qywb32.dll", L"qywb.ico",
                              L"使用说明.txt", L"data\\\\qywb.dat", L"uninst.exe"};''',
              '''    const wchar_t* names[] = {L"qydemo.exe", L"qywb64.dll", L"qywb32.dll", L"qyreg32.exe",
                              L"qywb.ico",   L"使用说明.txt", L"data\\\\qywb.dat", L"uninst.exe"};''')

open(p, 'w', encoding='utf-8').write(s)
print('patched ok')
