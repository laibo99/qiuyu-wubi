// 秋雨五笔输入法 - 安装/卸载程序
// 自包含：载荷以追加方式附在本 EXE 末尾（由 tools/make_payload.py 生成）
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../core/qy_version.h"
#include "../core/qy_cleanup.h"

#pragma pack(push, 1)
struct PkgFooter {
    char magic[8];
    unsigned long long offset;
    unsigned long long length;
};
#pragma pack(pop)

static const char* kMagic = "QYWBPKG1";
static const wchar_t* kAppName = L"秋雨五笔输入法";
static const wchar_t* kVersion = QY_VERSION_STR;
static const wchar_t* kPublisher = L"秋雨";
static const wchar_t* kUninstallKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QiuYuWubi";

// 卸载程序的文件名。早期版本叫 uninst.exe，被 360 的“伪装成卸载程序”规则
// 误报为高危，4.0 起改成产品名 + Uninst，并在安装/卸载时把老的 uninst.exe
// 一并清掉（升级用户的机器上也不会再留着那个名字）。
static const wchar_t* kUninstExe = L"QiuYuWubiUninst.exe";
static const wchar_t* kLegacyUninstExe = L"uninst.exe";

static HWND g_log = nullptr, g_progress = nullptr, g_btnInstall = nullptr;
static HWND g_btnUninstall = nullptr;
static std::wstring g_installDir;
static bool g_elevatedRun = false;
static bool g_uninstallMode = false;  // --uninstall：卸载模式（窗口只保留“卸载”按钮）

static std::wstring g_logPath;

static std::wstring TempFile(const wchar_t* name) {
    wchar_t tmp[MAX_PATH] = {0};
    DWORD n = ::GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(tmp) + name;
}

static void WriteTextFileUtf8(const std::wstring& path, const std::wstring& text) {
    if (path.empty()) return;
    int need = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                     nullptr, nullptr);
    if (need <= 0) return;
    std::string utf8((size_t)need, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], need, nullptr,
                          nullptr);
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
    ::CloseHandle(h);
}

static void DetectResultFile();  // 定义在版本检测之后

// 静默安装（--silent-install）没有界面，日志写到 %TEMP%\qy_setup_last.log，
// 出问题能查、测试也能验证“跳过了多少文件”。
static void LogToFile(const std::wstring& s) {
    if (g_logPath.empty()) {
        g_logPath = TempFile(L"qy_setup_last.log");
        if (g_logPath.empty()) return;
        ::DeleteFileW(g_logPath.c_str());
    }
    std::wstring line = s + L"\r\n";
    int need = ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0,
                                     nullptr, nullptr);
    if (need <= 0) return;
    std::string utf8((size_t)need, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &utf8[0], need, nullptr,
                          nullptr);
    HANDLE h = ::CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    ::WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
    ::CloseHandle(h);
}

static void Log(const std::wstring& s) {
    if (g_log) {
        int len = ::GetWindowTextLengthW(g_log);
        ::SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        std::wstring line = s + L"\r\n";
        ::SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    } else {
        LogToFile(s);
    }
}

static void SetProgress(int percent) {
    if (g_progress) ::SendMessageW(g_progress, PBM_SETPOS, percent, 0);
}

static bool IsAdmin() {
    HANDLE tok = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{};
    DWORD sz = sizeof(elev);
    BOOL ok = ::GetTokenInformation(tok, TokenElevation, &elev, sz, &sz);
    ::CloseHandle(tok);
    return ok && elev.TokenIsElevated;
}

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2] = {0};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf);
}

// 卸载程序是安装程序的副本：双击运行时没有任何命令行参数，
// 只能靠自身文件名判断这是卸载程序，否则会被当成安装程序打开。
// 兼容老版本遗留的 uninst.exe，升级用户的开始菜单快捷方式不会失效。
static bool SelfIsUninstaller() {
    std::wstring p = ExePath();
    size_t slash = p.find_last_of(L"\\/");
    const wchar_t* name = (slash == std::wstring::npos) ? p.c_str() : p.c_str() + slash + 1;
    return ::lstrcmpiW(name, kUninstExe) == 0 || ::lstrcmpiW(name, kLegacyUninstExe) == 0;
}

// 安装包是机器级安装（Program Files + HKLM），开始菜单优先写“所有用户”目录
static std::wstring StartMenuRoot() {
    wchar_t buf[MAX_PATH] = {0};
    if (::SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, 0, buf) == S_OK &&
        buf[0] != 0)
        return std::wstring(buf);
    if (::SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, buf) == S_OK && buf[0] != 0)
        return std::wstring(buf);
    return std::wstring();
}

// 兼容早期版本写到当前用户开始菜单的情况
static void RemovePerUserShortcuts() {
    wchar_t buf[MAX_PATH] = {0};
    if (::SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, buf) != S_OK || !buf[0]) return;
    std::wstring d = std::wstring(buf) + L"\\秋雨五笔输入法";
    ::DeleteFileW((d + L"\\秋雨五笔 设置与演示.lnk").c_str());
    ::DeleteFileW((d + L"\\卸载秋雨五笔.lnk").c_str());
    ::RemoveDirectoryW(d.c_str());
}

static std::wstring DefaultInstallDir() {
    wchar_t buf[MAX_PATH] = {0};
    if (::SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, buf) == S_OK)
        return std::wstring(buf) + L"\\QiuYuWubi";
    return L"C:\\Program Files\\QiuYuWubi";
}

//------------------------------------------------------------------------------
// 载荷读取
struct PkgEntry {
    std::wstring name;
    const BYTE* data;
    DWORD size;
};

static std::vector<BYTE> g_pkgBuf;

static bool LoadPayload(std::vector<PkgEntry>& out) {
    std::wstring exe = ExePath();
    HANDLE h = ::CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    ::GetFileSizeEx(h, &size);
    if (size.QuadPart < 64) {
        ::CloseHandle(h);
        return false;
    }
    PkgFooter foot{};
    ::SetFilePointer(h, (LONG)(size.QuadPart - (LONGLONG)sizeof(PkgFooter)), nullptr, FILE_BEGIN);
    DWORD rd = 0;
    ::ReadFile(h, &foot, sizeof(foot), &rd, nullptr);
    if (rd != sizeof(foot) || memcmp(foot.magic, kMagic, 8) != 0) {
        ::CloseHandle(h);
        return false;
    }
    if (foot.offset + foot.length > (unsigned long long)size.QuadPart) {
        ::CloseHandle(h);
        return false;
    }
    g_pkgBuf.assign((size_t)foot.length, 0);
    ::SetFilePointer(h, (LONG)foot.offset, nullptr, FILE_BEGIN);
    DWORD total = 0;
    while (total < foot.length) {
        DWORD got = 0;
        if (!::ReadFile(h, g_pkgBuf.data() + total, (DWORD)(foot.length - total), &got, nullptr) ||
            got == 0)
            break;
        total += got;
    }
    ::CloseHandle(h);
    if (total != foot.length) return false;

    size_t p = 0;
    while (p + 2 <= g_pkgBuf.size()) {
        WORD nameLen = *(WORD*)(g_pkgBuf.data() + p);
        p += 2;
        if (p + nameLen + 8 > g_pkgBuf.size()) break;
        std::string name((const char*)g_pkgBuf.data() + p, nameLen);
        p += nameLen;
        DWORD fsize = *(DWORD*)(g_pkgBuf.data() + p);
        p += 8;  // size + attr
        if (p + fsize > g_pkgBuf.size()) break;
        PkgEntry e;
        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), (int)name.size(), nullptr, 0);
        e.name.resize(wlen);
        ::MultiByteToWideChar(CP_UTF8, 0, name.c_str(), (int)name.size(), &e.name[0], wlen);
        e.data = g_pkgBuf.data() + p;
        e.size = fsize;
        out.push_back(e);
        p += fsize;
    }
    return !out.empty();
}

// 从安装包内嵌载荷中取出“使用说明.txt”（UTF-8），转为可显示文本（\n -> \r\n）
static std::wstring LoadHelpText() {
    std::vector<PkgEntry> files;
    if (!LoadPayload(files)) return std::wstring();
    for (const auto& f : files) {
        if (f.name != L"使用说明.txt") continue;
        // 说明文件写成 UTF-8 + 单个 BOM（Win7 记事本靠 BOM 才认 UTF-8），
        // 显示前必须剥掉，否则说明框首行会多出一个零宽方框。
        const char* p = (const char*)f.data;
        DWORD n = f.size;
        size_t off = 0;
        while (off + 3 <= (size_t)n && (unsigned char)p[off] == 0xEF &&
               (unsigned char)p[off + 1] == 0xBB && (unsigned char)p[off + 2] == 0xBF)
            off += 3;
        p += off;
        n -= (DWORD)off;
        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, p, (int)n, nullptr, 0);
        if (wlen <= 0) return std::wstring();
        std::wstring w((size_t)wlen, 0);
        ::MultiByteToWideChar(CP_UTF8, 0, p, (int)n, &w[0], wlen);
        // 规范换行：跳过 \r，统一输出 \r\n
        std::wstring out;
        out.reserve(w.size() + 256);
        for (wchar_t c : w) {
            if (c == L'\r') continue;
            if (c == L'\n') out += L"\r\n";
            else out += c;
        }
        return out;
    }
    return std::wstring();
}

static bool WriteFileFromMem(const std::wstring& path, const BYTE* data, DWORD size) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, data, size, &written, nullptr);
    ::CloseHandle(h);
    return ok && written == size;
}

// 已存在的文件与要写入的内容是否完全一致。
// 码表这类大文件常被运行中的输入法进程内存映射，替换不掉；如果内容本来就
// 相同（例如只升级了 DLL 的版本），直接当成功处理，不再让安装报错中断。
static bool FileContentsEqual(const std::wstring& path, const BYTE* data, DWORD size) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool same = false;
    if (::GetFileSizeEx(h, &sz) && sz.QuadPart == (LONGLONG)size) {
        std::vector<BYTE> buf(size ? size : 1);
        DWORD got = 0;
        if (::ReadFile(h, buf.data(), size, &got, nullptr) && got == size)
            same = memcmp(buf.data(), data, size) == 0;
    }
    ::CloseHandle(h);
    return same;
}

//------------------------------------------------------------------------------
// 本地已安装版本检测
//
// 安装包打开时的第一步是看这台机器上已经装了什么：
//   · 没装过          → 全新安装，写全部文件
//   · 已装旧版本      → 升级安装，只写内容有变化的文件，保留设置与词频
//   · 已装同版本      → 提示已是最新，按钮变成“修复安装”
//   · 已装更新版本    → 提示无需安装，用户坚持才降级
// 这样升级时不再是“每次都全新安装一遍”。
//------------------------------------------------------------------------------
enum InstallKind { kFresh = 0, kUpgrade = 1, kSame = 2, kDowngrade = 3 };

static std::wstring g_localVer;      // 本地已安装版本（空 = 未安装）
static InstallKind g_kind = kFresh;  // 本次安装的性质
// 主输入方案选择（安装界面的单选钮）：
//   0 五笔 / 1 拼音 / 2 五笔拼音混输，对应 config.ini 的 [方案] 默认
// g_schemeFromUI 只有在用户通过界面上点“开始安装”时才置真——
// 静默安装（--silent-install）不带界面，不能偷偷改用户的方案设置
static int g_scheme = 0;
static bool g_schemeFromUI = false;
// 本次安装里有几个文件因被占用走了“改名让位、重启后删除”的路径：
// 说明有程序还加载着旧组件，关闭窗口时应询问用户要不要现在结束它们
static int g_pendingOld = 0;
static bool g_installed = false;  // 安装/修复完成：主按钮变成“关闭”

static const wchar_t* SchemeName(int v) {
    if (v == 1) return L"拼音输入";
    if (v == 2) return L"五笔拼音混输";
    return L"五笔输入";
}




// 逐段按数字比较："3.10.0" > "3.9.9"；忽略前缀 v 与后缀（-beta、空格等）
static int VerCmp(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    for (;;) {
        while (i < a.size() && (a[i] < L'0' || a[i] > L'9')) ++i;
        while (j < b.size() && (b[j] < L'0' || b[j] > L'9')) ++j;
        long va = 0, vb = 0;
        while (i < a.size() && a[i] >= L'0' && a[i] <= L'9') va = va * 10 + (a[i++] - L'0');
        while (j < b.size() && b[j] >= L'0' && b[j] <= L'9') vb = vb * 10 + (b[j++] - L'0');
        if (va != vb) return va > vb ? 1 : -1;
        if (i >= a.size() && j >= b.size()) return 0;
    }
}

// 去掉末尾反斜杠后按大小写不敏感比较两个目录
static bool SameDir(const std::wstring& a, const std::wstring& b) {
    std::wstring x = a, y = b;
    while (!x.empty() && (x.back() == L'\\' || x.back() == L'/')) x.pop_back();
    while (!y.empty() && (y.back() == L'\\' || y.back() == L'/')) y.pop_back();
    return !x.empty() && ::CompareStringOrdinal(x.c_str(), (int)x.size(), y.c_str(), (int)y.size(),
                                                TRUE) == CSTR_EQUAL;
}

// 读本地已安装版本：
//   · 安装目录里的 installed.ver 最直接：它就是“这个目录里装的是哪一版”，
//     每次安装都会重写，不会像注册表那样被系统缓存或残留值干扰
//   · 没有 installed.ver（3.7 及更早）才看注册表 DisplayVersion，
//     且只在 InstallLocation 就是本目录时才认，避免拿别的目录的版本冒充
//   · 目录里有组件却读不到版本号 = 3.7 及更早的安装，按 0.0.0
static std::wstring ReadInstalledVersion(const std::wstring& dir) {
    std::wstring vf = dir + L"\\installed.ver";
    HANDLE h = ::CreateFileW(vf.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char raw[64] = {0};
        DWORD got = 0;
        ::ReadFile(h, raw, sizeof(raw) - 1, &got, nullptr);
        ::CloseHandle(h);
        std::wstring v;
        for (DWORD i = 0; i < got; ++i) {
            char c = raw[i];
            if (c == '\r' || c == '\n' || c == ' ') break;
            if (c >= '0' && c <= '9') v.push_back((wchar_t)c);
            else if (c == '.' && !v.empty()) v.push_back(L'.');
        }
        if (!v.empty()) return v;
    }
    wchar_t buf[64] = {0};
    DWORD sz = sizeof(buf), type = 0;
    HKEY k = nullptr;
    bool regIsThisDir = true;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t regDir[MAX_PATH * 2] = {0};
        DWORD dsz = sizeof(regDir), dtype = 0;
        if (::RegQueryValueExW(k, L"InstallLocation", nullptr, &dtype, (LPBYTE)regDir, &dsz) ==
                ERROR_SUCCESS &&
            regDir[0])
            regIsThisDir = SameDir(dir, regDir);
        LONG r = ::RegQueryValueExW(k, L"DisplayVersion", nullptr, &type, (LPBYTE)buf, &sz);
        ::RegCloseKey(k);
        if (r == ERROR_SUCCESS && type == REG_SZ && buf[0] && regIsThisDir)
            return std::wstring(buf);
    }
    if (::GetFileAttributesW((dir + L"\\qywb64.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
        ::GetFileAttributesW((dir + L"\\qywb32.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        return L"0.0.0";  // 老版本安装，没有版本记录
    return std::wstring();
}

// --detect 的结果写到 %TEMP%\qy_detect.txt（GUI 程序没有控制台）
static void DetectResultFile() {
    const wchar_t* kt = g_kind == kUpgrade      ? L"upgrade"
                       : g_kind == kSame        ? L"same"
                       : g_kind == kDowngrade   ? L"downgrade"
                                                : L"fresh";
    wchar_t line[800];
    ::wsprintfW(line, L"local=%s\r\npkg=%s\r\nkind=%s\r\ndir=%s\r\n",
                g_localVer.empty() ? L"(none)" : g_localVer.c_str(), kVersion, kt,
                g_installDir.c_str());
    WriteTextFileUtf8(TempFile(L"qy_detect.txt"), line);
}

static void DetectLocalVersion() {
    g_localVer = ReadInstalledVersion(g_installDir);
    if (g_localVer.empty()) {
        g_kind = kFresh;
        return;
    }
    int c = VerCmp(g_localVer, kVersion);
    g_kind = (c < 0) ? kUpgrade : (c == 0 ? kSame : kDowngrade);
}

// 路径是否落在某个目录下（忽略大小写；兼容 PendingFileRenameOperations 的
// "*1" / "!" 标记与 "\??\" 设备路径前缀）
static bool EntryUnderDir(const std::wstring& entry, const std::wstring& dir) {
    if (entry.empty()) return false;
    std::wstring s = entry;
    size_t i = 0;
    while (i < s.size() && (s[i] == L'*' || s[i] == L'!' || (s[i] >= L'0' && s[i] <= L'9'))) ++i;
    s.erase(0, i);
    if (s.size() >= 4 && s[0] == L'\\' && s[1] == L'?' && s[2] == L'?' && s[3] == L'\\') s.erase(0, 4);
    if (s.size() < dir.size()) return false;
    return ::CompareStringOrdinal(s.c_str(), (int)dir.size(), dir.c_str(), (int)dir.size(), TRUE) ==
           CSTR_EQUAL;
}

// 清掉 PendingFileRenameOperations 里指向安装目录的待办条目。
//
// 卸载时 qywb64.dll / data\qywb.dat / uninst.exe 正被占用，只能登记成
// “重启后删除”。用户如果没重启就直接重装，这些旧条目会在下次开机时把
// 刚装好的新文件删掉——码表一没，输入法就完全不能打字了。
// 安装成功后目录里的文件已经是最新的，指向它的待办条目全部作废，这里剔除。
static void RemovePendingForDir(const std::wstring& dir) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
                        0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    DWORD type = 0, bytes = 0;
    if (::RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, nullptr, &bytes) !=
            ERROR_SUCCESS ||
        type != REG_MULTI_SZ || bytes < sizeof(wchar_t) * 2) {
        ::RegCloseKey(key);
        return;
    }
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, 0);
    if (::RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, nullptr, (BYTE*)buf.data(),
                           &bytes) != ERROR_SUCCESS) {
        ::RegCloseKey(key);
        return;
    }
    // 值是一串以 \0 分隔、双 \0 结尾的字符串，按“源 / 目标”成对出现；
    // 删除类操作的“目标”是空串，不能当成结束标志（这里按对读取）
    size_t total = bytes / sizeof(wchar_t);
    std::vector<std::wstring> keep;  // 扁平保存：源, 目标, 源, 目标 ...
    int dropped = 0;
    size_t idx = 0;
    while (idx < total) {
        std::wstring src = buf[idx] ? std::wstring(buf.data() + idx) : std::wstring();
        if (src.empty()) break;  // 双 \0 → 结束
        idx += src.size() + 1;
        std::wstring dst;
        if (idx < total) {
            if (buf[idx]) {
                dst = buf.data() + idx;
                idx += dst.size() + 1;
            } else {
                ++idx;  // 空目标：删除
            }
        }
        if (EntryUnderDir(src, dir) || EntryUnderDir(dst, dir)) {
            // 但“重启后删除”的备份文件（*.old / *.new / *.pending）必须留着：
            // 它们正是当时被占用、删不掉的旧组件，靠这条登记在重启后清掉。
            // 早期版本在这里一刀切，把备份的删除项也剔除了，结果 .old 永远
            // 删不掉、越积越多（用户反馈的残留问题）。
            if (dst.empty() && qy::IsBackupFileName(src.c_str())) {
                keep.push_back(src);
                keep.push_back(dst);
                continue;
            }
            dropped += 2;
            continue;
        }
        keep.push_back(src);
        keep.push_back(dst);
    }
    if (dropped > 0) {
        if (keep.empty()) {
            ::RegDeleteValueW(key, L"PendingFileRenameOperations");
        } else {
            std::vector<wchar_t> out;
            for (auto& s : keep) {
                out.insert(out.end(), s.begin(), s.end());
                out.push_back(0);
            }
            out.push_back(0);
            ::RegSetValueExW(key, L"PendingFileRenameOperations", 0, REG_MULTI_SZ, (const BYTE*)out.data(),
                             (DWORD)(out.size() * sizeof(wchar_t)));
        }
        Log(L"  已清理指向安装目录的过期重启删除项");
    }
    ::RegCloseKey(key);
}


static bool MakeDirRecursive(const std::wstring& dir) {
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == L'\\' || i + 1 == dir.size()) {
            ::CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    return ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void CreateShortcut(const std::wstring& lnkPath, const std::wstring& target,
                           const std::wstring& workDir, const std::wstring& icon) {
    IShellLinkW* lnk = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, (void**)&lnk)) ||
        !lnk)
        return;
    lnk->SetPath(target.c_str());
    lnk->SetWorkingDirectory(workDir.c_str());
    lnk->SetDescription(kAppName);
    if (!icon.empty()) lnk->SetIconLocation(icon.c_str(), 0);
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(lnk->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
        pf->Save(lnkPath.c_str(), TRUE);
        pf->Release();
    }
    lnk->Release();
}

// 重启输入法框架进程：否则系统里仍映射着旧版 DLL，新装的组件不会立即生效
static void RestartCtfmon() {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (::Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"ctfmon.exe") == 0) {
                    HANDLE hp = ::OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hp) {
                        ::TerminateProcess(hp, 0);
                        ::CloseHandle(hp);
                    }
                }
            } while (::Process32NextW(snap, &pe));
        }
        ::CloseHandle(snap);
    }
    ::Sleep(300);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmd[] = L"ctfmon.exe";
    if (::CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
    ::Sleep(300);
}

// 把删不掉的备份文件登记成“重启后删除”。
// 必须做：万一它们之前那条登记被旧版本安装程序误删了（3.x 的 bug），
// 文件就会永远躺在安装目录里，既删不掉也没人再管。
// 同一个文件只登记一次：每次安装都补一条会让 PendingFileRenameOperations
// 无限膨胀（重复条目无害，但没必要）。
static void ScheduleDeleteBackups(const std::wstring& dir) {
    static const wchar_t* kPats[] = {L"\\*.old*", L"\\*.new*", L"\\*.pending*"};
    // 先读现有登记，用于去重
    std::vector<std::wstring> existing;
    HKEY key = nullptr;
    bool canWrite = false;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
                        0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        canWrite = true;
        DWORD type = 0, bytes = 0;
        if (::RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, &type, nullptr,
                               &bytes) == ERROR_SUCCESS &&
            type == REG_MULTI_SZ && bytes >= sizeof(wchar_t) * 2) {
            std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, 0);
            if (::RegQueryValueExW(key, L"PendingFileRenameOperations", nullptr, nullptr,
                                   (BYTE*)buf.data(), &bytes) == ERROR_SUCCESS) {
                size_t total = bytes / sizeof(wchar_t), idx = 0;
                while (idx < total) {
                    std::wstring s = buf[idx] ? std::wstring(buf.data() + idx) : std::wstring();
                    if (s.empty()) break;  // 双 \0 结束
                    existing.push_back(s);
                    idx += s.size() + 1;
                    // 跳过成对出现的“目标”（删除操作的目标是空串）
                    if (idx < total) {
                        std::wstring dst = buf[idx] ? std::wstring(buf.data() + idx)
                                                    : std::wstring();
                        idx += dst.size() + 1;
                    }
                }
            }
        }
    }
    auto stripPrefix = [](const std::wstring& s) {
        std::wstring t = s;
        size_t i = 0;
        while (i < t.size() && (t[i] == L'*' || t[i] == L'!' || (t[i] >= L'0' && t[i] <= L'9')))
            ++i;
        t.erase(0, i);
        if (t.size() >= 4 && t[0] == L'\\' && t[1] == L'?' && t[2] == L'?' && t[3] == L'\\')
            t.erase(0, 4);
        for (auto& c : t) c = (wchar_t)::towlower(c);
        return t;
    };

    std::vector<std::wstring> add;
    std::wstring dirs[2] = {dir, dir + L"\\data"};
    for (int d = 0; d < 2; ++d) {
        for (auto pat : kPats) {
            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileW((dirs[d] + pat).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!qy::IsBackupFileName(fd.cFileName)) continue;
                std::wstring p = dirs[d] + L"\\" + fd.cFileName;
                if (::DeleteFileW(p.c_str())) continue;
                std::wstring low = stripPrefix(p);
                bool dup = false;
                for (auto& e : existing)
                    if (stripPrefix(e) == low) { dup = true; break; }
                if (!dup) {
                    existing.push_back(p);
                    add.push_back(p);
                }
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
    }
    if (canWrite && !add.empty()) {
        for (auto& p : add) ::MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    if (key) ::RegCloseKey(key);
}

// 删除覆盖安装时被占用而改名备份的 *.old / *.new / *.pending（含 data 子目录）。
// 返回仍删不掉的个数：这些文件还锁在 explorer / Office 里，只能靠重启后删除，
// 调用方据此在界面上如实说明，不能假装已经清干净。
static int CleanOldFiles(const std::wstring& dir) {
    int left = 0;
    int n = qy::PurgeBackupFiles(dir, &left);
    if (n > 0) {
        wchar_t b[128];
        ::wsprintfW(b, L"  已清理 %d 个覆盖安装遗留的旧文件", n);
        Log(b);
    }
    if (left > 0) {
        // 删不掉的必须登记重启后删除，否则会永久残留
        ScheduleDeleteBackups(dir);
    }
    return left;
}

// 给“被占用的旧文件”找一个可用的备份名。
// 登记过“重启后删除”的文件会一直处于 delete-pending，既删不掉也不能被覆盖，
// 所以固定用 .old / .pending 这样的名字会让下一次覆盖安装永远失败（实测卡过）。
static std::wstring UniqueSibling(const std::wstring& path, const wchar_t* tag) {
    for (int i = 0; i < 99; ++i) {
        std::wstring cand = path + tag + (i ? std::to_wstring(i) : std::wstring());
        if (::GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) return cand;
    }
    wchar_t buf[24];
    ::wsprintfW(buf, L"%s%lu", tag, (unsigned long)::GetTickCount());
    return path + buf;
}

//------------------------------------------------------------------------------
static bool CallDllRegister(const std::wstring& dllPath, bool unregister, std::wstring* err) {
    HMODULE h = ::LoadLibraryW(dllPath.c_str());
    if (!h) {
        if (err) *err = L"无法载入 " + dllPath;
        return false;
    }
    typedef HRESULT(WINAPI * PFN)();
    PFN fn = (PFN)::GetProcAddress(h, unregister ? "DllUnregisterServer" : "DllRegisterServer");
    if (!fn) {
        ::FreeLibrary(h);
        if (err) *err = L"缺少注册导出函数";
        return false;
    }
    HRESULT hr = fn();
    ::FreeLibrary(h);
    if (FAILED(hr)) {
        if (err) {
            wchar_t buf[128];
            wsprintfW(buf, L"注册返回 0x%08lX", (unsigned long)hr);
            *err = buf;
        }
        return false;
    }
    return true;
}

// 32 位 DLL 必须由 32 位进程注册：调用随包安装的 qyreg32.exe 辅助程序
static bool Run32BitHelper(const std::wstring& helper, const std::wstring& dllPath,
                           bool unregister) {
    if (::GetFileAttributesW(helper.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring cmd = L"\"" + helper + L"\" " + (unregister ? L"-u " : L"") + L"\"" + dllPath +
                       L"\"";
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

static bool DoInstall() {
    // 静默安装不走 WM_CREATE，这里统一再检测一次；界面模式重复检测也无副作用
    DetectLocalVersion();
    std::vector<PkgEntry> files;
    if (!LoadPayload(files) || files.empty()) {
        Log(L"[错误] 安装包损坏（未找到内嵌文件）");
        return false;
    }
    switch (g_kind) {
        case kUpgrade:
            Log(L"检测到已安装版本 " + g_localVer + L"，升级到 " + kVersion);
            Log(L"  升级方式：只写入有变化的文件，保留已有设置与词频");
            break;
        case kSame:
            Log(L"已安装相同版本 " + g_localVer + L"，执行修复安装（覆盖程序文件）");
            break;
        case kDowngrade:
            Log(L"[注意] 已安装版本 " + g_localVer + L" 比本安装包（" + kVersion + L"）更新");
            Log(L"  仍按要求安装较低版本，程序文件将被替换");
            break;
        default:
            Log(L"未检测到已安装版本，开始全新安装 " + std::wstring(kVersion));
            break;
    }
    Log(L"安装位置：" + g_installDir);
    // 清理上次覆盖安装遗留的 .old 备份（当时被占用的旧组件）
    CleanOldFiles(g_installDir);
    // 老版本（3.x）的卸载程序叫 uninst.exe，被 360 误报为高危，这里顺手删掉
    {
        std::wstring legacy = g_installDir + L"\\" + kLegacyUninstExe;
        if (::GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (::DeleteFileW(legacy.c_str()))
                Log(L"  已移除旧版卸载程序 uninst.exe（改名避免被安全软件误报）");
            else
                ::MoveFileExW(legacy.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    // 内置手写功能已取消，装新版时顺手清掉早先版本留下的识别模板（约 1.5 MB）
    {
        std::wstring tpl = g_installDir + L"\\data\\hw_tpl.dat";
        if (::GetFileAttributesW(tpl.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (::DeleteFileW(tpl.c_str()))
                Log(L"  已清除遗留的内置手写模板 data\\hw_tpl.dat");
            else
                ::MoveFileExW(tpl.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    }
    // 卸掉“上次卸载留下的、指向本目录的重启后删除”条目，否则重装后一重启
    // 新写入的码表/DLL 就被删了
    RemovePendingForDir(g_installDir);
    if (!MakeDirRecursive(g_installDir)) {
        Log(L"[错误] 无法创建安装目录");
        return false;
    }
    // 修复安装（同版本）才整包重写；升级只写内容有变化的文件。
    // 这就是“只更新新版本内容”：没改的组件不去动，既快，也不会去碰
    // 正在被 explorer / 微信 / Office 加载的 DLL（写入失败的最大来源）。
    const bool forceWrite = (g_kind == kSame);
    int i = 0;
    int failed = 0;
    int wrote = 0, skipped = 0;
    for (auto& f : files) {
        std::wstring rel = f.name;
        for (auto& ch : rel)
            if (ch == L'/') ch = L'\\';
        std::wstring path = g_installDir + L"\\" + rel;
        size_t pos = path.find_last_of(L'\\');
        if (pos != std::wstring::npos) MakeDirRecursive(path.substr(0, pos));

        // 内容完全一致 → 这个文件不需要更新，跳过
        if (!forceWrite && FileContentsEqual(path, f.data, f.size)) {
            ++skipped;
            SetProgress(10 + (int)(60.0 * (++i) / (double)files.size()));
            continue;
        }
        bool okWrite = WriteFileFromMem(path, f.data, f.size);
        if (!okWrite && ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // 文件被输入法进程占用（explorer / 微信 / Office 都加载着这个 DLL）：
            // Windows 允许给运行中的 DLL 改名，先把旧文件挪走再写入。
            // 注意必须用“空闲的”备份名：上一次安装登记过“重启后删除”的
            // qywb64.dll.old 会长期处于 delete-pending 状态，既删不掉也覆盖不了，
            // 用固定名字会让之后每一次覆盖安装都卡在这里失败。
            std::wstring old = UniqueSibling(path, L".old");
            if (::MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                ::MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                okWrite = WriteFileFromMem(path, f.data, f.size);
                if (okWrite) {
                    Log(L"  已替换（旧文件重启后自动删除）：" + rel);
                    ++g_pendingOld;
                }
            }
        }
        if (!okWrite) {
            // 仍失败（如码表被内存映射占用）：写到 .new，随后立即尝试原地替换
            // （引擎打开码表带 FILE_SHARE_DELETE，改名通常可以成功）；
            // 替换不了才登记重启后替换
            std::wstring tmp = UniqueSibling(path, L".new");
            if (WriteFileFromMem(tmp, f.data, f.size)) {
                std::wstring pend = UniqueSibling(path, L".pending");
                ::MoveFileExW(path.c_str(), pend.c_str(), MOVEFILE_REPLACE_EXISTING);
                if (::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    ::MoveFileExW(pend.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                    okWrite = true;
                    Log(L"  已替换（旧文件重启后自动删除）：" + rel);
                    ++g_pendingOld;
                } else {
                    ::MoveFileExW(pend.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
                    ::MoveFileExW(tmp.c_str(), path.c_str(),
                                  MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_REPLACE_EXISTING);
                    Log(L"  [提示] 文件被占用，已安排重启后自动更新：" + rel);
                    ++g_pendingOld;
                }
            }
            if (!okWrite) {
                // 仍失败（如码表被内存映射占用）：内容完全一致就视为已安装成功
                if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
                    FileContentsEqual(path, f.data, f.size)) {
                    Log(L"  内容未变化，跳过被占用文件：" + rel);
                    okWrite = true;
                }
            }
            if (!okWrite) {
                ++failed;
            }
        }
        if (okWrite) {
            ++wrote;
            if (::GetFileAttributesW((path + L".old").c_str()) == INVALID_FILE_ATTRIBUTES)
                Log(L"  已写入 " + f.name);
        }
        SetProgress(10 + (int)(60.0 * (++i) / (double)files.size()));
    }
    {
        wchar_t sb[160];
        ::wsprintfW(sb, L"  文件处理完成：写入 %d 个，跳过 %d 个（内容未变化）", wrote, skipped);
        Log(sb);
    }
    if (failed) {
        Log(L"[错误] 有文件写入失败，安装中止");
        return false;
    }
    // 复制自身，供卸载使用。文件名带产品名而不是笼统的 uninst.exe：
    // 360 等安全软件对 "uninst.exe" 有“伪装成卸载程序”的启发式规则，会直接
    // 判高危；配上版本信息资源（见 tools/seticon.py）才是正常软件的样子。
    {
        std::wstring dst = g_installDir + L"\\" + kUninstExe;
        if (!::CopyFileW(ExePath().c_str(), dst.c_str(), FALSE))
            Log(L"[警告] 卸载程序写入失败，可在“设置-应用”里重新运行安装包卸载");
    }

    // 注册 64 位 / 32 位 文本服务
    std::wstring err;
    std::wstring dll64 = g_installDir + L"\\qywb64.dll";
    if (::GetFileAttributesW(dll64.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CallDllRegister(dll64, false, &err))
            Log(L"  已注册 64 位输入法组件");
        else
            Log(L"[警告] 64 位注册失败：" + err);
    }
    std::wstring dll32 = g_installDir + L"\\qywb32.dll";
    if (::GetFileAttributesW(dll32.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (Run32BitHelper(g_installDir + L"\\qyreg32.exe", dll32, false))
            Log(L"  已注册 32 位输入法组件");
        else
            Log(L"[警告] 32 位组件注册失败，32 位程序可能无法使用本输入法");
    }
    SetProgress(80);

    // 记录的版本号必须和“磁盘上真正装好的东西”同步：
    // installed.ver 先写，注册表随后跟着写，两边永远是同一版，
    // 不会再出现“更新完显示的还是旧版本”。
    {
        std::string v;
        for (const wchar_t* q = kVersion; *q; ++q) v.push_back((char)*q);
        v += "\n";
        if (!WriteFileFromMem(g_installDir + L"\\installed.ver", (const BYTE*)v.data(),
                              (DWORD)v.size()))
            Log(L"[警告] 版本记录写入失败（下次打开安装包可能重新判断版本）");
    }

    // 构建戳：同版本号可能对应多次构建（日常修复不升版本号），更新程序靠它
    // 判断「本机这份是哪次构建」，服务器清单若更旧就不会再往回覆盖。
    // 只有 build.sh 注入过（14 位 YYYYMMDDHHMMSS）才写，手写编译的 "0" 不写。
    {
        std::string b;
        for (const wchar_t* q = QY_BUILD_STAMP; *q; ++q) b.push_back((char)*q);
        bool okStamp = (b.size() == 14);
        for (char c : b)
            if (c < '0' || c > '9') okStamp = false;
        if (okStamp) {
            b += "\r\n";
            if (!WriteFileFromMem(g_installDir + L"\\installed.build", (const BYTE*)b.data(),
                                  (DWORD)b.size()))
                Log(L"[警告] 构建戳写入失败（更新程序将用 installed.ver 的时间兜底）");
        }
    }

    // 卸载信息
    HKEY k = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                          nullptr) == ERROR_SUCCESS) {
        auto setStr = [&](const wchar_t* name, const std::wstring& val) {
            ::RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(),
                             (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        };
        // 卸载命令必须带引号：Program Files 路径含空格，不加引号时
        // “设置-应用-卸载”会把路径截断成 C:\Program 而执行失败
        std::wstring uninstCmd = L"\"" + g_installDir + L"\\" + kUninstExe + L"\" --uninstall";
        setStr(L"DisplayName", kAppName);
        setStr(L"DisplayVersion", kVersion);
        setStr(L"Publisher", kPublisher);
        setStr(L"InstallLocation", g_installDir);
        setStr(L"DisplayIcon", g_installDir + L"\\qydemo.exe");
        setStr(L"UninstallString", uninstCmd);
        setStr(L"QuietUninstallString", uninstCmd + L" --silent-install");
        setStr(L"URLInfoAbout", L"http://down.sam.gd.cn/");
        wchar_t date[16] = {0};
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        ::wsprintfW(date, L"%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
        setStr(L"InstallDate", date);
        DWORD one = 1;
        ::RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        ::RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        // EstimatedSize 以 KB 计：按安装目录实际占用填，任务管理器/设置里
        // 显示的大小才不会是个拍脑袋的数字
        DWORD sizeKb = 9000;
        {
            wchar_t buf[MAX_PATH * 2] = {0};
            ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
            std::wstring p = buf;
            size_t sl = p.find_last_of(L'\\');
            if (sl != std::wstring::npos) p.resize(sl);
            WIN32_FIND_DATAW fd{};
            HANDLE hf = ::FindFirstFileW((g_installDir + L"\\*").c_str(), &fd);
            unsigned long long total = 0;
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    total += ((unsigned long long)fd.nFileSizeHigh << 32) + fd.nFileSizeLow;
                } while (::FindNextFileW(hf, &fd));
                ::FindClose(hf);
            }
            if (total > 0) sizeKb = (DWORD)(total / 1024 + 1);
        }
        ::RegSetValueExW(k, L"EstimatedSize", 0, REG_DWORD, (const BYTE*)&sizeKb, sizeof(sizeKb));
        ::RegCloseKey(k);
        // 刷新“设置-应用”里的列表，版本号立即变成新装的这一版
        ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    } else {
        Log(L"[警告] 写入卸载信息失败，可在“设置-应用”里看不到本程序");
    }

    // 开始菜单快捷方式（所有用户）
    std::wstring smRoot = StartMenuRoot();
    if (!smRoot.empty()) {
        std::wstring dir = smRoot + L"\\秋雨五笔输入法";
        MakeDirRecursive(dir);
        CreateShortcut(dir + L"\\秋雨五笔 设置与演示.lnk", g_installDir + L"\\qydemo.exe",
                       g_installDir, g_installDir + L"\\qywb.ico");
        CreateShortcut(dir + L"\\卸载秋雨五笔.lnk", g_installDir + L"\\" + kUninstExe, g_installDir,
                       g_installDir + L"\\qywb.ico");
        RemovePerUserShortcuts();
        Log(L"  已创建开始菜单快捷方式");
    }
    SetProgress(100);
    // 用户配置：只在“还没有这一项”时补默认值（新装默认显示状态条）。
    // 升级时绝不覆盖用户已经改过的设置——这也是“只更新程序内容、
    // 不重做一遍安装”的一部分。
    {
        wchar_t ad[MAX_PATH * 2] = {0};
        if (::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, ad) == S_OK && ad[0] != 0) {
            std::wstring dir = std::wstring(ad) + L"\\QiuYuWubi";
            ::CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring ini = dir + L"\\config.ini";
            wchar_t cur[16] = {0};
            ::GetPrivateProfileStringW(L"界面", L"状态条", L"", cur, 16, ini.c_str());
            if (cur[0] == 0) {
                ::WritePrivateProfileStringW(L"界面", L"状态条", L"1", ini.c_str());
                Log(L"  已默认勾选“显示输入法状态条”");
            } else {
                Log(L"  保留您原有的界面设置（升级不重置）");
            }
            // 主输入方案：用户在安装界面上选了什么就写什么（包括升级时改主意）；
            // 静默安装没有界面，不动这项，尊重用户现有配置
            if (g_schemeFromUI) {
                wchar_t v[8];
                ::wsprintfW(v, L"%d", g_scheme);
                ::WritePrivateProfileStringW(L"方案", L"默认", v, ini.c_str());
                Log(std::wstring(L"  默认输入方案已设为：") + SchemeName(g_scheme));
            }
        }
    }
    // 让系统卸载旧版 DLL 并立即加载新版：否则状态条等改动要重启电脑才出现
    RestartCtfmon();
    int leftovers = CleanOldFiles(g_installDir);
    Log(L"  已重启输入法框架，新版本立即生效");
    if (leftovers > 0) {
        wchar_t b[220];
        ::wsprintfW(b,
                    L"  仍有 %d 个旧文件正被其它程序占用，已登记重启后自动删除"
                    L"（重启一次即可彻底清空）。",
                    leftovers);
        Log(b);
    }
    if (g_kind == kUpgrade)
        Log(L"更新完成，当前版本 " + std::wstring(kVersion));
    else if (g_kind == kSame)
        Log(L"修复安装完成，当前版本 " + std::wstring(kVersion));
    else
        Log(L"安装完成，当前版本 " + std::wstring(kVersion));
    Log(L"使用方法：按 Win+空格 切到“秋雨五笔”，右下角即为状态条。");
    return true;
}

static bool DoUninstall() {
    std::wstring dir = g_installDir;
    Log(L"开始卸载：" + dir);
    std::wstring err;
    Run32BitHelper(dir + L"\\qyreg32.exe", dir + L"\\qywb32.dll", true);
    CallDllRegister(dir + L"\\qywb64.dll", true, &err);
    Log(L"  已注销输入法组件");
    ::RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);
    std::wstring smRoot = StartMenuRoot();
    if (!smRoot.empty()) {
        std::wstring d = smRoot + L"\\秋雨五笔输入法";
        ::DeleteFileW((d + L"\\秋雨五笔 设置与演示.lnk").c_str());
        ::DeleteFileW((d + L"\\卸载秋雨五笔.lnk").c_str());
        ::RemoveDirectoryW(d.c_str());
    }
    RemovePerUserShortcuts();
    const wchar_t* names[] = {L"qydemo.exe",   L"qywb64.dll",     L"qywb32.dll",
                              L"qyreg32.exe", L"qyupd.exe",      L"qywb.ico",       L"使用说明.txt",
                              L"installed.ver", L"installed.build", L"data\\qywb.dat",
                              kLegacyUninstExe, kUninstExe};
    // 延迟删除自身
    for (auto n : names) {
        std::wstring p = dir + L"\\" + n;
        if (::DeleteFileW(p.c_str())) continue;
        // 当前运行的卸载程序是“自己”，下面会挪到临时目录再删，不在这里登记
        if (::lstrcmpiW(n, kUninstExe) == 0 && _wcsicmp(ExePath().c_str(), p.c_str()) == 0)
            continue;
        // 删不掉（DLL 已加载、码表被映射）才登记重启后删除。
        // 不能无条件登记：用户可能马上重装，重启时会把新装的文件删掉
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            ::MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    // 覆盖安装时留下的 *.old / *.new / *.pending 备份也要清掉（含 data 子目录），
    // 否则卸载后目录还剩几个旧文件，看着像没卸干净
    {
        int left = 0;
        int n = qy::PurgeBackupFiles(dir, &left);
        if (n > 0) {
            wchar_t b[96];
            ::wsprintfW(b, L"  已清理 %d 个旧文件备份", n);
            Log(b);
        }
        if (left > 0) {
            // 还锁在别的进程里：登记重启后删除，否则会永久残留
            ScheduleDeleteBackups(dir);
            Log(L"  [提示] 部分旧文件备份正被占用，已安排重启后删除");
        }
    }
    // data 目录：码表可能正被运行中的输入法映射着，删不掉就登记重启后删
    {
        std::wstring datadir = dir + L"\\data";
        if (!::RemoveDirectoryW(datadir.c_str()) &&
            ::GetFileAttributesW(datadir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring dat = datadir + L"\\qywb.dat";
            if (::GetFileAttributesW(dat.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ::MoveFileExW(dat.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
            ::RemoveDirectoryW(datadir.c_str());
        }
    }
    ::RemoveDirectoryW(dir.c_str());

    // 自身无法在运行中直接删除，但可以“改名”——先挪到临时目录，目录就能删干净。
    // 只有“卸载程序本身就在安装目录里”（正常情况：uninst.exe）才这么做；
    // 用户直接拿安装包执行卸载时，安装包在别的目录，绝不能把人家的文件挪走
    {
        std::wstring self = ExePath();
        std::wstring prefix = dir;
        if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
        bool selfInDir = _wcsnicmp(self.c_str(), prefix.c_str(), prefix.size()) == 0;
        wchar_t tmp[MAX_PATH] = {0};
        if (::GetTempPathW(MAX_PATH, tmp) > 0) {
            wchar_t dst[MAX_PATH] = {0};
            ::wsprintfW(dst, L"%sqywb_uninst_%lu.tmp", tmp, (unsigned long)::GetCurrentProcessId());
            if (selfInDir && ::MoveFileExW(self.c_str(), dst, MOVEFILE_REPLACE_EXISTING)) {
                // 已挪走，原目录此刻为空，直接删除；临时文件登记重启后清理
                ::RemoveDirectoryW(dir.c_str());
                ::MoveFileExW(dst, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
        }
        if (selfInDir && ::GetFileAttributesW(self.c_str()) != INVALID_FILE_ATTRIBUTES)
            ::MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        ::RemoveDirectoryW(dir.c_str());
    }

    RestartCtfmon();
    SetProgress(100);
    if (::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // 覆盖安装改名留下的 .old/.pending 若已被登记“重启后删除”，现在删不掉，
        // 目录也就删不掉：把目录本身也登记一次，重启后连壳一起清干净
        ::MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        Log(L"  [提示] 部分文件正被运行中的程序占用，已安排重启后自动删除");
    }
    Log(L"卸载完成。");
    return true;
}

//------------------------------------------------------------------------------
// 安装 / 卸载前的确认框：必须勾选“我已确认……”才能继续
//------------------------------------------------------------------------------
struct ConfirmState {
    bool confirmed = false;
    HWND chk = nullptr;
    HWND ok = nullptr;
};

static const int kChkId = 3001;

static LRESULT CALLBACK ConfirmProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ConfirmState* st = (ConfirmState*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == kChkId && st) {
                bool checked = ::SendMessageW(st->chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                ::EnableWindow(st->ok, checked ? TRUE : FALSE);
                return 0;
            }
            if (id == IDOK && st) {
                if (::SendMessageW(st->chk, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    st->confirmed = true;
                    ::DestroyWindow(h);
                }
                return 0;
            }
            if (id == IDCANCEL) {
                ::DestroyWindow(h);
                return 0;
            }
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

static bool ConfirmWithCheck(HWND owner, const wchar_t* heading, const wchar_t* statement,
                             const wchar_t* checkText) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ConfirmProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"QyConfirmWnd";
        ::RegisterClassExW(&wc);
        reg = true;
    }

    RECT cr{0, 0, 520, 236};
    ::AdjustWindowRect(&cr, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE);
    int w = cr.right - cr.left, hgt = cr.bottom - cr.top;
    RECT orc{0, 0, 0, 0};
    if (owner) ::GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - w) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - hgt) / 2;

    HWND h = ::CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"QyConfirmWnd", heading,
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, hgt, owner, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
    if (!h) return false;

    ConfirmState st;
    ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    HFONT f = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
    HFONT ft = ::CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, GB2312_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");

    HWND t = ::CreateWindowExW(0, L"STATIC", heading, WS_CHILD | WS_VISIBLE, 20, 14, 480, 30, h,
                               nullptr, nullptr, nullptr);
    ::SendMessageW(t, WM_SETFONT, (WPARAM)(ft ? ft : f), TRUE);

    HWND s = ::CreateWindowExW(0, L"STATIC", statement, WS_CHILD | WS_VISIBLE, 20, 54, 480, 48, h,
                               nullptr, nullptr, nullptr);
    ::SendMessageW(s, WM_SETFONT, (WPARAM)f, TRUE);

    st.chk = ::CreateWindowExW(0, L"BUTTON", checkText,
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 20, 112, 480,
                               26, h, (HMENU)(INT_PTR)kChkId, nullptr, nullptr);
    ::SendMessageW(st.chk, WM_SETFONT, (WPARAM)f, TRUE);

    st.ok = ::CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 300,
                              168, 90, 30, h, (HMENU)(INT_PTR)IDOK, nullptr, nullptr);
    ::SendMessageW(st.ok, WM_SETFONT, (WPARAM)f, TRUE);
    ::EnableWindow(st.ok, FALSE);  // 未勾选不可继续

    HWND cancel = ::CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 400, 168, 90, 30,
                                    h, (HMENU)(INT_PTR)IDCANCEL, nullptr, nullptr);
    ::SendMessageW(cancel, WM_SETFONT, (WPARAM)f, TRUE);

    if (owner) ::EnableWindow(owner, FALSE);
    ::ShowWindow(h, SW_SHOW);
    ::SetForegroundWindow(h);
    ::SetFocus(st.chk);

    MSG msg;
    while (::IsWindow(h) && ::GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                ::DestroyWindow(h);
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                if (::SendMessageW(st.chk, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    st.confirmed = true;
                    ::DestroyWindow(h);
                }
                continue;
            }
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (owner) {
        ::EnableWindow(owner, TRUE);
        ::SetForegroundWindow(owner);
    }
    if (ft) ::DeleteObject(ft);
    return st.confirmed;
}

//------------------------------------------------------------------------------
// 安装界面的文案：随“本地已装什么”变化
//------------------------------------------------------------------------------
static HWND g_status = nullptr;  // 版本检测结果那一行文字


static std::wstring WindowCaption() {
    std::wstring v = kVersion;
    if (g_uninstallMode) return L"秋雨五笔输入法 卸载程序";
    if (g_kind == kUpgrade) return L"秋雨五笔输入法 更新程序 " + g_localVer + L" → " + v;
    if (g_kind == kSame) return L"秋雨五笔输入法 安装程序（已是最新 " + v + L"）";
    if (g_kind == kDowngrade) return L"秋雨五笔输入法 安装程序（已装 " + g_localVer + L"）";
    return L"秋雨五笔输入法 安装程序 " + v;
}

static const wchar_t* InstallButtonText() {
    switch (g_kind) {
        case kUpgrade:
            return L"立即更新";
        case kSame:
            return L"修复安装";
        case kDowngrade:
            return L"仍然安装";
        default:
            return L"开始安装";
    }
}

static std::wstring VersionSummary() {
    wchar_t b[640];
    switch (g_kind) {
        case kUpgrade:
            ::wsprintfW(b,
                        L"检测到已安装版本 %s，本次升级到 %s：只更新有变化的文件，"
                        L"保留您的设置与词频记录。",
                        g_localVer.c_str(), kVersion);
            break;
        case kSame:
            ::wsprintfW(b,
                        L"已安装相同版本 %s，无需更新。点“修复安装”可把程序文件重新覆盖一遍"
                        L"（设置与词频仍保留）。",
                        g_localVer.c_str());
            break;
        case kDowngrade:
            ::wsprintfW(b,
                        L"已安装版本 %s 比本安装包（%s）更新，一般无需安装。"
                        L"如确需降级，建议先卸载。",
                        g_localVer.c_str(), kVersion);
            break;
        default:
            ::wsprintfW(b, L"未检测到已安装的秋雨五笔，本次为全新安装 %s。", kVersion);
            break;
    }
    return std::wstring(b);
}

// 检测结果变化后刷新标题、状态行、按钮文字（用户改了安装路径时会重新检测）
static void UpdateVersionUI(HWND h) {
    if (g_status) ::SetWindowTextW(g_status, VersionSummary().c_str());
    if (g_btnInstall) ::SetWindowTextW(g_btnInstall, InstallButtonText());
    if (h) ::SetWindowTextW(h, WindowCaption().c_str());
}

// 安装/更新前的确认框：文案按检测结果区分，同一版本时明确告诉用户“已是最新”
static bool ConfirmInstall(HWND owner) {
    std::wstring title, body, check;
    switch (g_kind) {
        case kUpgrade:
            title = L"更新秋雨五笔输入法到 " + std::wstring(kVersion);
            body = L"将从 " + g_localVer + L" 升级到 " + kVersion + L"。\n"
                   L"只更新有变化的程序文件，设置与词频都会保留。";
            check = L"我已确认阅读完使用说明，开始更新";
            break;
        case kSame:
            title = L"修复安装 秋雨五笔输入法";
            body = std::wstring(L"已安装相同版本 ") + kVersion + L"，无需更新。\n"
                   L"继续将覆盖程序文件，用于修复损坏或丢失。";
            check = L"我已确认阅读完使用说明，开始修复安装";
            break;
        case kDowngrade:
            title = L"安装较低版本 秋雨五笔输入法";
            body = L"已安装版本 " + g_localVer + L" 比本安装包（" + kVersion + L"）更新。\n"
                   L"继续将替换为较低版本，建议先卸载。";
            check = L"我已了解这是降级安装，继续";
            break;
        default:
            title = L"安装秋雨五笔输入法";
            body = std::wstring(L"安装会注册输入法组件，并重启输入法服务（ctfmon）。\n"
                                L"完成后即可按 Win+空格 切换使用。");
            check = L"我已确认阅读完使用说明，开始安装秋雨输入法";
            break;
    }
    return ConfirmWithCheck(owner, title.c_str(), body.c_str(), check.c_str());
}

//------------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            // 安装包一打开就先看本地装了什么：决定是全新安装、升级还是修复
            DetectLocalVersion();
            HFONT f = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
            // 标题字体
            HFONT ft = ::CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, GB2312_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");
            // 说明区字体：微软雅黑，回退由系统处理
            HFONT fy = ::CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, GB2312_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"微软雅黑");

            const wchar_t* headText =
                g_uninstallMode ? L"秋雨五笔输入法 卸载程序"
                                : (g_kind == kUpgrade ? L"秋雨五笔输入法 更新程序"
                                                       : L"秋雨五笔输入法 安装程序");
            HWND t = ::CreateWindowExW(0, L"STATIC", headText, WS_CHILD | WS_VISIBLE | SS_CENTER, 20,
                                       12, 700, 34, h, nullptr, nullptr, nullptr);
            ::SendMessageW(t, WM_SETFONT, (WPARAM)(ft ? ft : f), TRUE);

            HWND lbl = ::CreateWindowExW(0, L"STATIC",
                                         g_uninstallMode ? L"安装位置（将删除）：" : L"安装位置：",
                                         WS_CHILD | WS_VISIBLE, 20, 56, 130, 22, h, nullptr,
                                         nullptr, nullptr);
            ::SendMessageW(lbl, WM_SETFONT, (WPARAM)f, TRUE);
            HWND ed = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_installDir.c_str(),
                                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
                                            (g_uninstallMode ? ES_READONLY : 0),
                                        160, 54, 560, 24, h, (HMENU)1001, nullptr, nullptr);
            ::SendMessageW(ed, WM_SETFONT, (WPARAM)f, TRUE);

            // ---- 中部：使用说明（内容取自安装包内嵌的使用说明.txt） ----
            HWND ht = ::CreateWindowExW(0, L"STATIC", L"使用说明", WS_CHILD | WS_VISIBLE, 20, 88,
                                        200, 22, h, nullptr, nullptr, nullptr);
            ::SendMessageW(ht, WM_SETFONT, (WPARAM)f, TRUE);
            HWND help = ::CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                20, 112, 700, 234, h, (HMENU)1003, nullptr, nullptr);
            ::SendMessageW(help, WM_SETFONT, (WPARAM)(fy ? fy : f), TRUE);
            ::SendMessageW(help, EM_SETLIMITTEXT, 0x7FFFFFF0, 0);
            std::wstring helpText = LoadHelpText();
            if (helpText.empty())
                helpText =
                    L"（未能读取内嵌说明文件，安装完成后可在安装目录打开“使用说明.txt”）";
            ::SetWindowTextW(help, helpText.c_str());

            // ---- 主输入方案：安装时就选好习惯的方案（写进 config.ini） ----
            if (!g_uninstallMode) {
                HWND sl = ::CreateWindowExW(0, L"STATIC", L"主输入方案：", WS_CHILD | WS_VISIBLE,
                                            20, 350, 96, 22, h, nullptr, nullptr, nullptr);
                ::SendMessageW(sl, WM_SETFONT, (WPARAM)(fy ? fy : f), TRUE);
                struct {
                    int id;
                    const wchar_t* text;
                } radios[] = {
                    {3001, L"五笔输入（默认）"},
                    {3002, L"拼音输入"},
                    {3003, L"五笔拼音混输"},
                };
                // 已装过的机器：按用户当前方案预选，全新安装默认五笔
                int pre = g_scheme;
                {
                    wchar_t ad[MAX_PATH * 2] = {0};
                    if (::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, ad) == S_OK &&
                        ad[0]) {
                        std::wstring ini = std::wstring(ad) + L"\\QiuYuWubi\\config.ini";
                        wchar_t cur[16] = {0};
                        ::GetPrivateProfileStringW(L"方案", L"默认", L"", cur, 16, ini.c_str());
                        if (cur[0] == L'1' || cur[0] == L'2') pre = cur[0] - L'0';
                    }
                }
                g_scheme = pre;
                for (int i = 0; i < 3; ++i) {
                    HWND rb = ::CreateWindowExW(
                        0, L"BUTTON", radios[i].text,
                        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0),
                        120 + i * 200, 349, 198, 24, h, (HMENU)(INT_PTR)radios[i].id, nullptr,
                        nullptr);
                    ::SendMessageW(rb, WM_SETFONT, (WPARAM)(fy ? fy : f), TRUE);
                }
                ::CheckRadioButton(h, 3001, 3003, 3001 + g_scheme);
            }

            // ---- 版本检测提示：已装版本 / 本次是升级还是修复 ----
            if (!g_uninstallMode) {
                g_status = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 380,
                                             700, 26, h, (HMENU)1004, nullptr, nullptr);
                ::SendMessageW(g_status, WM_SETFONT, (WPARAM)(fy ? fy : f), TRUE);
            }

            // ---- 底部：安装过程日志 / 进度 / 按钮 ----
            HWND lt = ::CreateWindowExW(0, L"STATIC",
                                        g_uninstallMode ? L"卸载过程" : L"安装过程", WS_CHILD |
                                            WS_VISIBLE,
                                        20, 416, 200, 22, h, nullptr, nullptr, nullptr);
            ::SendMessageW(lt, WM_SETFONT, (WPARAM)f, TRUE);
            g_log = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                          WS_VSCROLL,
                                      20, 440, 700, 82, h, (HMENU)1002, nullptr, nullptr);
            ::SendMessageW(g_log, WM_SETFONT, (WPARAM)f, TRUE);

            g_progress = ::CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 20, 532,
                                           700, 16, h, nullptr, nullptr, nullptr);

            // 安装窗口只有“开始安装”，卸载窗口只有“卸载”，不再互相混排
            if (g_uninstallMode) {
                g_btnUninstall = ::CreateWindowExW(0, L"BUTTON", L"卸载",
                                                   WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 328,
                                                   560, 100, 32, h, (HMENU)2002, nullptr, nullptr);
                ::SendMessageW(g_btnUninstall, WM_SETFONT, (WPARAM)f, TRUE);
            } else {
                g_btnInstall = ::CreateWindowExW(0, L"BUTTON", InstallButtonText(),
                                                 WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 328, 560,
                                                 100, 32, h, (HMENU)2001, nullptr, nullptr);
                ::SendMessageW(g_btnInstall, WM_SETFONT, (WPARAM)f, TRUE);
            }
            UpdateVersionUI(h);  // 标题、版本提示、按钮文字按检测结果刷新
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == 2001) {
                // 安装完成后主按钮变成“关闭”：点了走 WM_CLOSE（可能询问结束占用进程）
                if (g_installed) {
                    ::PostMessageW(h, WM_CLOSE, 0, 0);
                    return 0;
                }
                // 用户点了安装：把单选钮里的方案记下来，安装完写进 config.ini
                if (::IsDlgButtonChecked(h, 3002) == BST_CHECKED)
                    g_scheme = 1;
                else if (::IsDlgButtonChecked(h, 3003) == BST_CHECKED)
                    g_scheme = 2;
                else
                    g_scheme = 0;
                g_schemeFromUI = true;
                wchar_t buf[MAX_PATH * 2] = {0};
                ::GetWindowTextW(::GetDlgItem(h, 1001), buf, MAX_PATH * 2);
                if (buf[0]) g_installDir = buf;
                // 用户可能改了安装路径：按新路径再检测一次版本
                DetectLocalVersion();
                UpdateVersionUI(h);
                // 安装/更新前确认：文案随“本地已装什么”变化，仍需勾选才能继续
                if (!ConfirmInstall(h)) return 0;
                ::EnableWindow(g_btnInstall, FALSE);
                bool ok = DoInstall();
                if (ok) {
                    g_installed = true;
                    // 成功：结果写进过程日志；窗口不再自动关闭——若有程序还
                    // 加载着旧组件，用户点“关闭”时再决定是否立即结束它们
                    Log(g_kind == kUpgrade ? L"更新成功！" : g_kind == kSame ? L"修复安装成功！" : L"安装成功！");
                    Log(L"请按 Win+空格 切换到“秋雨五笔”；若列表里没有，可到 设置→时间和语言→"
                        L"语言→中文(简体)→选项 里添加键盘。");
                    if (g_pendingOld > 0) {
                        wchar_t b[160];
                        ::wsprintfW(b, L"有 %d 个文件曾被占用、旧副本将在重启电脑后自动删除。",
                                    g_pendingOld);
                        Log(b);
                        Log(L"点“关闭”时可选择立即结束仍在使用旧组件的程序，让新版立刻生效。");
                    }
                    if (g_btnInstall) {
                        ::EnableWindow(g_btnInstall, TRUE);
                        ::SetWindowTextW(g_btnInstall, L"关闭");
                    }
                } else {
                    ::EnableWindow(g_btnInstall, TRUE);
                    ::MessageBoxW(h, L"安装未完成，请查看下方“安装过程”日志中的错误说明。", kAppName,
                                  MB_OK | MB_ICONERROR);
                }
            } else if (id == 2002) {
                // 卸载前确认：必须勾选“我已确认不再使用秋雨输入法”才能继续
                if (!ConfirmWithCheck(h, L"卸载秋雨五笔输入法",
                                      L"卸载会移除安装目录与输入法注册信息。\n"
                                      L"您的个人设置与词频记录会被保留。",
                                      L"我已确认不再使用秋雨输入法，开始卸载"))
                    return 0;
                if (g_btnUninstall) ::EnableWindow(g_btnUninstall, FALSE);
                bool ok = DoUninstall();
                if (ok) {
                    Log(L"窗口将在 2 秒后自动关闭。");
                    ::SetTimer(h, 3, 2000, nullptr);  // 卸载完成：自动关闭
                } else {
                    if (g_btnUninstall) ::EnableWindow(g_btnUninstall, TRUE);
                    ::MessageBoxW(h, L"卸载未完成，请查看下方“卸载过程”日志。", kAppName,
                                  MB_OK | MB_ICONERROR);
                }
            }
            return 0;
        }
        case WM_TIMER:
            // 3 号定时器 = 安装/卸载完成后的自动关闭
            if (w == 3) {
                ::KillTimer(h, 3);
                ::PostMessageW(h, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_CLOSE: {
            // 安装/修复完成且本次有文件因被占用而延迟替换：关闭前问一下，
            // 由用户决定“现在结束占用程序”还是“稍后重启再完成”
            if (g_installed && !g_uninstallMode && g_pendingOld > 0) {
                wchar_t msg[640];
                ::wsprintfW(msg,
                            L"有 %d 个程序仍在使用旧版本的输入法组件"
                            L"（旧文件已登记，重启电脑后自动删除）。\n\n"
                            L"“是”：现在结束这些程序，新版立刻生效"
                            L"（请先保存它们里未保存的文档）\n"
                            L"“否”：暂不结束，稍后重启电脑时自动完成替换",
                            g_pendingOld);
                if (::MessageBoxW(h, msg, kAppName,
                                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    // 结束进程交给安装目录里的 qyupd.exe --kill-hosts（复用更新
                    // 程序的进程检测：模块扫描 + Restart Manager，跳过关键进程）
                    std::wstring upd = g_installDir + L"\\qyupd.exe";
                    SHELLEXECUTEINFOW se{sizeof(se)};
                    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
                    se.lpVerb = L"open";
                    se.lpFile = upd.c_str();
                    se.lpParameters = L"--kill-hosts";
                    se.lpDirectory = g_installDir.c_str();
                    se.nShow = SW_HIDE;
                    if (::GetFileAttributesW(upd.c_str()) != INVALID_FILE_ATTRIBUTES &&
                        ::ShellExecuteExW(&se) && se.hProcess) {
                        ::WaitForSingleObject(se.hProcess, 15000);
                        ::CloseHandle(se.hProcess);
                        Log(L"  已结束仍加载旧组件的程序，重新打开它们即是新版。");
                    } else {
                        Log(L"  [提示] 未能执行结束操作，可稍后重启电脑自动完成替换。");
                    }
                } else {
                    Log(L"  已选择稍后处理：重启电脑后会自动完成替换。");
                }
            }
            ::DestroyWindow(h);
            return 0;
        }
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // 命令行
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    bool uninstall = false;
    bool silent = false;
    bool detect = false;
    std::wstring customDir;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--uninstall" || a == L"-u") uninstall = true;
        if (a == L"--elevated") g_elevatedRun = true;
        // 只做本地版本检测并输出结果，不安装（排查问题 / 自动化测试用）
        if (a == L"--detect") detect = true;
        if (a == L"--silent-install") {
            g_elevatedRun = true;
            silent = true;
        }
        if (a == L"--dir" && i + 1 < argc) customDir = argv[i + 1];
    }
    if (argv) ::LocalFree(argv);

    // 双击 uninst.exe（包括开始菜单里的「卸载秋雨五笔」快捷方式）时没有参数，
    // 按文件名兜底识别为卸载程序，避免弹出安装窗口
    if (!uninstall && SelfIsUninstaller()) uninstall = true;

    g_installDir = customDir.empty() ? DefaultInstallDir() : customDir;
    // 从注册表读取已安装路径；但用户显式 --dir 指定了目录时以用户为准
    // （否则“装到别的目录”永远被旧路径覆盖，版本检测也会认错目录）
    if (customDir.empty()) {
        HKEY k = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            wchar_t buf[MAX_PATH * 2] = {0};
            DWORD sz = sizeof(buf), type = 0;
            if (::RegQueryValueExW(k, L"InstallLocation", nullptr, &type, (LPBYTE)buf, &sz) ==
                    ERROR_SUCCESS &&
                buf[0])
                g_installDir = buf;
            ::RegCloseKey(k);
        }
    }

    if (detect) {
        DetectLocalVersion();
        DetectResultFile();
        return 0;
    }

    if (!IsAdmin()) {
        std::wstring self = ExePath();
        std::wstring args = uninstall ? L"--uninstall --elevated" : L"--elevated";
        if (silent) args += L" --silent-install";
        if (!customDir.empty()) args += L" --dir \"" + customDir + L"\"";
        HINSTANCE r = ::ShellExecuteW(nullptr, L"runas", self.c_str(), args.c_str(), nullptr,
                                      SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) {
            ::MessageBoxW(nullptr, L"需要管理员权限才能安装输入法。", kAppName, MB_ICONERROR);
            return 1;
        }
        return 0;
    }

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ::InitCommonControls();

    if (silent) {
        bool ok = uninstall ? DoUninstall() : DoInstall();
        ::CoUninitialize();
        return ok ? 0 : 3;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QySetupWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    // 图标：优先取内嵌资源（ID 101），否则退回同目录 qywb.ico
    wc.hIcon = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32,
                                   LR_DEFAULTCOLOR);
    if (!wc.hIcon)
        wc.hIcon =
            (HICON)::LoadImageW(nullptr, L"qywb.ico", IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    wc.hIconSm = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16,
                                     LR_DEFAULTCOLOR);
    ::RegisterClassExW(&wc);

    g_uninstallMode = uninstall;

    HWND h = ::CreateWindowExW(
        0, L"QySetupWnd",
        uninstall ? L"秋雨五笔输入法 卸载程序" : L"秋雨五笔输入法 安装程序",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 280, 100, 756, 660, nullptr,
        nullptr, hInst, nullptr);
    if (!h) return 1;
    ::ShowWindow(h, SW_SHOW);

    if (uninstall) {
        // 卸载模式：窗口只有“卸载”一个按钮，打开后直接进入确认流程
        ::PostMessageW(h, WM_COMMAND, 2002, 0);
    }

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::CoUninitialize();
    return 0;
}
