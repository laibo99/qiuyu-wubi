// 秋雨五笔输入法 - 增量更新程序
//
// 全量安装包 9MB 里 4.7MB 是码表，日常改版往往只动两三个组件。这里的做法是：
//   1) 读服务器上的 version.txt，拿到版本号和“增量清单”地址（inc=）
//   2) 清单里逐行写着这一版每个文件的大小与 CRC32
//   3) 把清单和安装目录里的同名文件比对，只下载指纹不一样的那几个
//   4) 校验 CRC 后替换（被占用的 DLL 先改名成 .old，重启后自动清掉）
//
// 所以“更新”只花几百 KB 的流量，不用再下一次完整安装包。
// 写 Program Files 需要管理员权限：启动时先试写，不行就 runas 自己重开。
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../core/qy_common.h"
#include "../core/qy_config.h"
#include "../core/qy_update.h"
#include "../core/qy_cleanup.h"

using namespace qy;

#define WM_UPD_STEP (WM_APP + 1)  // 状态文字变了
#define WM_UPD_PROG (WM_APP + 2)  // 进度变了
#define WM_UPD_DONE (WM_APP + 3)  // 检查/更新结束（wParam: 0 检查完成 1 更新完成 2 失败）

enum { ID_START = 1001, ID_CLOSE = 1002 };

struct Change {
    IncFile f;
    std::wstring target;  // 安装目录里的绝对路径
    std::wstring temp;    // 下载到的临时文件
};

static HWND g_main = nullptr, g_list = nullptr, g_tip = nullptr, g_step = nullptr;
static HWND g_btnStart = nullptr, g_btnClose = nullptr;
static HFONT g_font = nullptr, g_fontTitle = nullptr, g_fontBold = nullptr;
static CRITICAL_SECTION g_cs;

static std::vector<Change> g_changes;
static std::wstring g_installDir;
static std::wstring g_curVer, g_newVer, g_notes;
// installed.ver 是否真的读到了：只有它才是「本地装的是哪一版」的真值。
// 读不到时 g_curVer 只是编译期版本，不能拿来做防降级判断。
static bool g_localVerTrusted = false;
// 本地构建戳（installed.build）。版本号相同、只是构建不同时，靠它判新旧：
// 服务器清单停留在上一次构建，若只按 CRC 比对就会把本机较新的文件判成
// 「待更新」，点更新即回退到旧构建（版本号一样，版本规则看不出来）。
static std::wstring g_localBuild;
static bool g_localBuildTrusted = false;
static std::wstring g_serverBuild;  // 服务器清单里的 build=
// 非空 = 本次服务器清单比本地旧，已禁止任何写入；界面显示原因、按钮禁用。
static std::wstring g_blockReason;
static unsigned long long g_totalBytes = 0;
static std::wstring g_stepText;
static std::wstring g_failText;
static int g_prog = 0;  // 0..1000
static bool g_busy = false;
static bool g_auto = false;  // --auto：检查完直接更新，结束就退出（脚本/自检用）
static int g_exitCode = 0;   // --auto 的进程退出码：0 成功/已最新，2 失败
static bool g_updated = false;  // 本次是否真的写入过新文件（决定要不要结束占用进程）
static volatile LONG g_cancel = 0;

// 简易日志：--auto 模式没有界面，结果写 %TEMP%\qyupd.log
static void LogFile(const wchar_t* fmt, ...) {
    wchar_t buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    ::wvsprintfW(buf, fmt, ap);
    va_end(ap);
    wchar_t dir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"qyupd.log";
    HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char u8[2048]{};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8) - 2, nullptr, nullptr);
    if (n > 0) {
        DWORD wr = 0;
        ::WriteFile(h, u8, (DWORD)(n - 1), &wr, nullptr);  // 去掉结尾的 0
        ::WriteFile(h, "\r\n", 2, &wr, nullptr);
    }
    ::CloseHandle(h);
}

// ---- 线程 → 界面的进度回传 ----
static void PostStep(const std::wstring& s) {
    ::EnterCriticalSection(&g_cs);
    g_stepText = s;
    ::LeaveCriticalSection(&g_cs);
    if (g_main) ::PostMessageW(g_main, WM_UPD_STEP, 0, 0);
}

static void PostProg(int p) {
    ::EnterCriticalSection(&g_cs);
    g_prog = p < 0 ? 0 : (p > 1000 ? 1000 : p);
    ::LeaveCriticalSection(&g_cs);
    if (g_main) ::PostMessageW(g_main, WM_UPD_PROG, 0, 0);
}

static void PostDone(int code, const std::wstring& msg) {
    ::EnterCriticalSection(&g_cs);
    g_failText = msg;
    ::LeaveCriticalSection(&g_cs);
    if (g_main) ::PostMessageW(g_main, WM_UPD_DONE, (WPARAM)code, 0);
}

// ---- 小工具 ----
static std::wstring BaseName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}

static std::wstring DirName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : p.substr(0, s);
}

static void MakeDirRecursive(const std::wstring& d) {
    if (d.empty()) return;
    std::wstring cur;
    for (size_t i = 0; i <= d.size(); ++i) {
        if (i == d.size() || d[i] == L'\\' || d[i] == L'/') {
            if (!cur.empty()) ::CreateDirectoryW(cur.c_str(), nullptr);
        }
        if (i < d.size()) cur.push_back(d[i]);
    }
}

// 注意：wsprintfW 不支持 %f / %llu，浮点与 64 位都要手工换算成 %u
static std::wstring KBText(unsigned long long bytes) {
    wchar_t b[64];
    if (bytes >= 1024ULL * 1024ULL) {
        unsigned long long tenths = (bytes * 10ULL + 524288ULL) / 1048576ULL;
        ::wsprintfW(b, L"%u.%u MB", (unsigned)(tenths / 10ULL), (unsigned)(tenths % 10ULL));
    } else {
        ::wsprintfW(b, L"%u KB", (unsigned)((bytes + 1023ULL) / 1024ULL));
    }
    return b;
}

// 是否还能写安装目录（不需要真写成功，能创建临时文件就够）
static bool InstallDirWritable(const std::wstring& dir) {
    std::wstring t = dir + L"\\~qyupd.tmp";
    HANDLE h = ::CreateFileW(t.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

// 以管理员身份重开自己（Program Files 里写文件必须提权）
static bool RelaunchElevated() {
    wchar_t self[MAX_PATH * 2] = {0};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    SHELLEXECUTEINFOW si{};
    si.cbSize = sizeof(si);
    si.lpVerb = L"runas";
    si.lpFile = self;
    si.lpParameters = L"--elevated";
    si.lpDirectory = g_installDir.c_str();
    si.nShow = SW_SHOWNORMAL;
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!::ShellExecuteExW(&si)) return false;
    if (si.hProcess) ::CloseHandle(si.hProcess);
    return true;
}

// ---- 文件替换 ----
// 优先直接覆盖；被占用（DLL 已加载 / 码表被映射）时先改名再写，
// Windows 允许给正在使用的文件改名，这样新文件能立刻就位。
// 改名都失败才安排重启后替换；备份文件后缀用 .qypend，
// 不落进 *.old / *.pending 的清理规则里，否则会被启动清理删掉、重启替换落空。
// 被改名让位的旧文件：这些文件此刻仍被运行中的进程占用着，
// 它们的路径交给 Restart Manager 去找“谁占着”（与 32/64 位无关）。
static std::vector<std::wstring> g_oldMappedFiles;

static bool ApplyOne(const std::wstring& src, const std::wstring& dst, bool* needReboot,
                     std::wstring* err) {
    if (needReboot) *needReboot = false;
    if (::CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;

    std::wstring old = dst + L".old";
    for (int i = 1; i < 200 && ::GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        wchar_t b[32];
        ::wsprintfW(b, L".old%d", i);
        old = dst + b;
    }
    if (::MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (::CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            g_oldMappedFiles.push_back(old);  // 旧文件仍被进程占用着
            ::MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            return true;
        }
        ::MoveFileExW(old.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);  // 还原
    }

    std::wstring pend = dst + L".qypend";
    for (int i = 1; i < 200 && ::GetFileAttributesW(pend.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) {
        wchar_t b[32];
        ::wsprintfW(b, L".qypend%d", i);
        pend = dst + b;
    }
    if (::CopyFileW(src.c_str(), pend.c_str(), FALSE) &&
        ::MoveFileExW(pend.c_str(), dst.c_str(),
                      MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_REPLACE_EXISTING)) {
        if (needReboot) *needReboot = true;
        return true;
    }
    ::DeleteFileW(pend.c_str());
    if (err) *err = L"文件被占用，无法替换（请关闭正在使用输入法的程序后重试）";
    return false;
}

// 已取消的功能保留下来的文件：更新完成后顺手清掉，别一直堆在安装目录里。
// 被旧组件占用时删不掉，就登记到重启后删除，不影响本次更新结果。
static void CleanObsoleteFiles(const std::wstring& dir) {
    static const wchar_t* kObsolete[] = {L"data\\hw_tpl.dat"};
    for (const wchar_t* rel : kObsolete) {
        std::wstring p = dir + L"\\" + rel;
        if (::GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (::DeleteFileW(p.c_str())) {
            LogFile(L"[%s] 已清除遗留文件 %s", dir.c_str(), rel);
        } else {
            ::MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            LogFile(L"[%s] 遗留文件正被占用，已登记重启后删除 %s", dir.c_str(), rel);
        }
    }
}

// 本地构建戳的兜底来源：installed.ver 的写入时间。
//
// 老版本装的目录里没有 installed.build，而 installed.ver 是「这次安装/上次
// 更新时」写的，它的修改时间就近似本机构建时间。注意 FileTime 是 UTC，
// 而 build.sh 写的 build= 是本地时间，必须先转本地时区再格式化，否则会差 8 小时。
static std::wstring BuildStampFromInstalledVerMtime(const std::wstring& dir) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW((dir + L"\\installed.ver").c_str(), GetFileExInfoStandard, &fad))
        return std::wstring();
    FILETIME local{};
    if (!::FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local)) return std::wstring();
    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&local, &st)) return std::wstring();
    wchar_t buf[20] = {0};
    ::wsprintfW(buf, L"%04d%02d%02d%02d%02d%02d", (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    return buf;
}

// 20261108155600 → 2026-11-08 15:56（给人看的提示文案用）
static std::wstring BuildStampText(const std::wstring& s) {
    if (s.size() != 14) return s.empty() ? L"未知" : s;
    return s.substr(0, 4) + L"-" + s.substr(4, 2) + L"-" + s.substr(6, 2) + L" " +
           s.substr(8, 2) + L":" + s.substr(10, 2);
}

static void WriteInstalledVer(const std::wstring& ver) {
    if (ver.empty()) return;
    std::string v;
    for (const wchar_t* q = ver.c_str(); *q; ++q) v.push_back((char)*q);
    v += "\n";
    HANDLE h = ::CreateFileW((g_installDir + L"\\installed.ver").c_str(), GENERIC_WRITE, 0,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    ::WriteFile(h, v.data(), (DWORD)v.size(), &wr, nullptr);
    ::CloseHandle(h);
}

// 注册表里的“显示版本”跟着更新，控制面板里不会还写着旧号
static void UpdateRegistryVersion(const std::wstring& ver) {
    if (ver.empty()) return;
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QiuYuWubi", 0,
                        KEY_WRITE, &k) != ERROR_SUCCESS)
        return;
    ::RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, (const BYTE*)ver.c_str(),
                     (DWORD)((ver.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(k);
}

// ---- 检查：清单 vs 本地 ----
static void DoCheck() {
    PostStep(L"正在读取更新信息…");
    PostProg(40);
    Config cfg;
    cfg.Load();
    std::wstring addr = cfg.opt().updateUrl;
    if (addr.empty()) {
        PostDone(2, L"未配置更新地址。\n\n请在 %APPDATA%\\QiuYuWubi\\config.ini 的\n"
                    L"[更新] 地址 里填写 version.txt 的网址（或本地路径）。");
        return;
    }
    UpdateInfo info;
    std::wstring err;
    if (!FetchUpdateInfo(addr, &info, &err)) {
        PostDone(2, L"检查更新失败：\n" + err);
        return;
    }
    g_notes = info.notes;
    if (info.incUrl.empty()) {
        PostDone(2, L"服务器上还没有增量清单（version.txt 里缺少 inc= 行）。\n\n"
                    L"可以先上传本次生成的 update\\inc 目录，或直接用完整安装包更新。");
        return;
    }
    PostStep(L"正在读取增量清单…");
    PostProg(120);
    IncManifest m;
    std::wstring manifestTime;  // 清单文件自身的发布时间（清单没写 build= 时兜底）
    if (!FetchIncManifest(info.incUrl, &m, &err, &manifestTime)) {
        PostDone(2, L"读取增量清单失败：\n" + err);
        return;
    }
    g_newVer = m.version.empty() ? info.version : m.version;

    // 防降级：服务器还挂着旧版清单时，旧版文件的 CRC 与本地全不同，会被误判成
    // 「一堆待更新文件」，点开始更新就是把新版文件回退，最后还把 installed.ver
    // 写成旧号。必须在比对 CRC 之前拦住。
    // 同版本不拦 —— 修复默认按 CRC 热修发布，本来就不升版本号。
    const std::wstring serverVer = info.version.empty() ? g_newVer : info.version;
    if (IncUpdateWouldDowngrade(g_curVer, info.version, m.version, g_localVerTrusted)) {
        g_changes.clear();
        g_totalBytes = 0;
        g_blockReason = L"本地版本 v" + g_curVer + L" 比服务器 v" + serverVer +
                        L" 更新，已跳过增量更新，避免把文件降级。\n"
                        L"请等服务器上的 version.txt 与 inc/manifest.txt 都更新为同一版本后再检查。";
        LogFile(L"[%s] 防降级：本地 v%s > 服务器 v%s，跳过全部 %d 个清单条目",
                g_installDir.c_str(), g_curVer.c_str(), serverVer.c_str(), (int)m.files.size());
        PostProg(1000);
        PostDone(0, L"");
        return;
    }
    g_blockReason.clear();

    // 同版本的「构建倒退」：不升版本号发布是常态，服务器上若只换了 version.txt、
    // inc/manifest.txt 还停在上一次构建，CRC 必然对不上，于是列出一批待更新文件；
    // 点更新就把本机较新的文件回退成旧构建 —— 版本号一模一样，降级完全无感。
    // 这里用构建戳判方向：本地严格新于清单 → 整份跳过。
    g_serverBuild = m.build.empty() ? manifestTime : m.build;
    if (IncUpdateIsStaleBuild(g_curVer, m.version, g_localBuild, g_serverBuild, g_localVerTrusted,
                              g_localBuildTrusted)) {
        g_changes.clear();
        g_totalBytes = 0;
        const wchar_t* what = m.build.empty() ? L"清单发布时间" : L"构建";
        g_blockReason = L"本机 v" + g_curVer + L" 是 " + BuildStampText(g_localBuild) +
                        L" 的构建，服务器增量清单还是 " + BuildStampText(g_serverBuild) + L"（" +
                        what + L"），已跳过更新，避免把文件回退成旧构建。\n"
                        L"服务器把 inc/manifest.txt 与 inc/files 一起更新后再检查即可。";
        LogFile(L"[%s] 防构建倒退：本地 v%s build=%s > 清单 %s=%s，跳过全部 %d 个条目",
                g_installDir.c_str(), g_curVer.c_str(), g_localBuild.c_str(),
                m.build.empty() ? L"mtime" : L"build", g_serverBuild.c_str(),
                (int)m.files.size());
        PostProg(1000);
        PostDone(0, L"");
        return;
    }

    PostStep(L"正在比对本地文件…");
    PostProg(180);
    g_changes.clear();
    g_totalBytes = 0;
    int same = 0, skippedArch = 0;
    const int selfArch = SelfArch();  // x86 的 qyupd 不碰 x64 条目，反之亦然
    for (auto& f : m.files) {
        if (f.arch != 0 && f.arch != selfArch) {
            ++skippedArch;
            continue;
        }
        std::wstring rel = f.path;
        for (auto& c : rel)
            if (c == L'/') c = L'\\';
        Change ch;
        ch.f = f;
        ch.target = g_installDir + L"\\" + rel;
        unsigned long crc = 0;
        unsigned long long sz = 0;
        bool have = Crc32File(ch.target, &crc, &sz);
        if (have && crc == f.crc && (f.size == 0 || sz == f.size)) {
            ++same;
            continue;
        }
        g_changes.push_back(ch);
        g_totalBytes += f.size;
    }
    PostProg(1000);
    PostDone(0, L"");  // 具体文案由界面按 g_changes 生成
}

// ---- 下载 + 应用 ----
static DWORD WINAPI WorkerProc(LPVOID param) {
    bool checkOnly = (param != nullptr);
    if (checkOnly) {
        DoCheck();
        return 0;
    }

    std::wstring tmpDir = g_installDir + L"\\update_tmp";
    MakeDirRecursive(tmpDir);
    const int n = (int)g_changes.size();
    for (int i = 0; i < n; ++i) {
        if (::InterlockedCompareExchange(&g_cancel, 0, 0) != 0) {
            PostDone(2, L"已取消本次更新。");
            return 0;
        }
        wchar_t head[160];
        ::wsprintfW(head, L"正在下载 %d/%d：%s", i + 1, n, BaseName(g_changes[i].f.path).c_str());
        PostStep(head);
        wchar_t fn[32];
        ::wsprintfW(fn, L"\\%d.tmp", i);
        g_changes[i].temp = tmpDir + fn;
        const int base = i * 900 / n, span = 900 / n;
        std::wstring err;
        bool ok = DownloadFile(g_changes[i].f.url, g_changes[i].temp,
                               [&](unsigned long long got, unsigned long long total) -> bool {
                                   unsigned long long tot = total ? total : g_changes[i].f.size;
                                   int p = tot ? (int)(got * 100ULL / tot) : 100;
                                   PostProg(base + p * span / 100);
                                   return ::InterlockedCompareExchange(&g_cancel, 0, 0) == 0;
                               },
                               &err);
        if (!ok) {
            PostDone(2, L"下载失败：" + BaseName(g_changes[i].f.path) + L"\n" + err);
            return 0;
        }
        unsigned long crc = 0;
        unsigned long long sz = 0;
        if (!Crc32File(g_changes[i].temp, &crc, &sz) || crc != g_changes[i].f.crc) {
            PostDone(2, L"文件校验失败（可能下载不完整）：" + BaseName(g_changes[i].f.path) +
                            L"\n请重试一次。");
            return 0;
        }
    }

    PostStep(L"正在写入程序文件…");
    PostProg(920);
    int reboot = 0;
    for (int i = 0; i < n; ++i) {
        std::wstring err;
        bool needReboot = false;
        if (!ApplyOne(g_changes[i].temp, g_changes[i].target, &needReboot, &err)) {
            PostDone(2, L"写入失败：" + g_changes[i].f.path + L"\n" + err);
            return 0;
        }
        if (needReboot) ++reboot;
        PostProg(920 + (i + 1) * 70 / (n ? n : 1));
    }
    // 绝不把 installed.ver / 注册表写成比本地更旧的版本号（哪怕清单里的文件
    // 全都 CRC 一致，只是做了一次修复性重写）。
    const std::wstring finalVer =
        (g_localVerTrusted && CompareVersion(g_curVer, g_newVer) > 0) ? g_curVer : g_newVer;
    WriteInstalledVer(finalVer);
    // 本机内容从此与清单同源，构建戳跟着清单走（清单没写 build= 则保留原值）。
    if (!g_serverBuild.empty()) WriteInstalledBuild(g_installDir, g_serverBuild);
    UpdateRegistryVersion(finalVer);
    CleanObsoleteFiles(g_installDir);
    g_updated = true;

    for (auto& c : g_changes) ::DeleteFileW(c.temp.c_str());
    ::RemoveDirectoryW(tmpDir.c_str());

    wchar_t msg[512];
    ::wsprintfW(msg,
                L"更新完成：共写入 %u 个文件（%s）。\n\n"
                L"已经打开的程序（Word、浏览器、微信等）还加载着旧组件，\n"
                L"关闭本窗口时会自动结束它们，之后重新打开即可用上最新版本。",
                (unsigned)n, KBText(g_totalBytes).c_str());
    if (reboot) {
        wchar_t extra[160];
        ::wsprintfW(extra, L"\n\n其中 %d 个文件正被占用，已安排在重启后自动替换。", reboot);
        ::lstrcatW(msg, extra);
    }
    PostProg(1000);
    PostDone(1, msg);
    return 0;
}

static void StartWorker(bool checkOnly) {
    if (g_busy) return;
    // 防降级第二道闸：即使界面/自动流程被改坏，也不允许启动「写入」线程。
    if (!checkOnly && !g_blockReason.empty()) return;
    g_busy = true;
    ::InterlockedExchange(&g_cancel, 0);
    ::EnableWindow(g_btnStart, FALSE);
    ::EnableWindow(g_btnClose, TRUE);
    HANDLE th = ::CreateThread(nullptr, 0, WorkerProc, checkOnly ? (LPVOID)1 : nullptr, 0, nullptr);
    if (th)
        ::CloseHandle(th);
    else {
        g_busy = false;
        ::EnableWindow(g_btnStart, TRUE);
    }
}

//------------------------------------------------------------------------------
// 结束“仍加载着旧组件”的程序
//
// 在线更新只是把磁盘上的文件换成新的，已经打开的程序（Word、浏览器、微信…）
// 内存里仍然是旧版本，新功能对它们不生效。以前的提示是“请手动重启这些程序”，
// 现在改成：关闭本更新窗口时自动把它们结束掉。
//
// 做法：Toolhelp 枚举进程 → 枚举每个进程的模块，找出仍映射着 qywb64.dll /
// qywb32.dll 的进程 → 先给它的窗口发 WM_CLOSE（让应用有机会自己保存），
// 1 秒还活着再强制结束。
//
// 安全阀：跳过系统关键进程（csrss/winlogon/服务宿主等）、跳过资源管理器
// explorer.exe（结束它会重启外壳、桌面会闪一下）、跳过本进程。
//------------------------------------------------------------------------------
struct HostProc {
    DWORD pid = 0;
    wchar_t name[MAX_PATH] = {0};
    bool closed = false;  // 已发 WM_CLOSE
    bool forced = false;  // 已强制结束
};

static bool IsSkipProcess(const wchar_t* name) {
    static const wchar_t* kSkip[] = {
        L"csrss.exe",  L"winlogon.exe", L"services.exe",  L"lsass.exe",  L"svchost.exe",
        L"dwm.exe",    L"sihost.exe",   L"spoolsv.exe",  L"smss.exe",   L"wininit.exe",
        L"system",     L"registry",     L"memory compression.exe",
        L"explorer.exe",  // 结束它会重启外壳，桌面会闪
        L"taskmgr.exe", L"searchhost.exe", L"textinputhost.exe",
        L"shellexperiencehost.exe", L"startmenuexperiencehost.exe",
        L"runtimebroker.exe", L"wudfhost.exe"};
    std::wstring n = name ? name : L"";
    for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); ++i) {
        if (::_wcsicmp(n.c_str(), kSkip[i]) == 0) return true;
    }
    return false;
}

// 32 位进程枚举 64 位进程的模块要用 Module32FirstEx/NextEx，但 MinGW 的
// 头文件和导入库都没有这两个符号（MS SDK 才有），运行时从 kernel32 取。
typedef BOOL(WINAPI* PModule32FirstExW)(HANDLE, LPMODULEENTRY32W, HANDLE);
typedef BOOL(WINAPI* PModule32NextExW)(HANDLE, LPMODULEENTRY32W, HANDLE);
static PModule32FirstExW g_modFirstEx = nullptr;
static PModule32NextExW g_modNextEx = nullptr;

static void EnsureModule32Ex() {
    if (g_modFirstEx) return;
    HMODULE k = ::GetModuleHandleW(L"kernel32.dll");
    if (!k) return;
    g_modFirstEx = (PModule32FirstExW)::GetProcAddress(k, "Module32FirstExW");
    g_modNextEx = (PModule32NextExW)::GetProcAddress(k, "Module32NextExW");
}

// 进程里是否还映射着我们的输入法 DLL；foundPath 回传命中的模块路径（诊断用）
static bool ProcessHoldsOldDll(DWORD pid, const std::wstring& installDir,
                                std::wstring* foundPath = nullptr) {
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        if (foundPath) *foundPath = L"(无法打开进程 err=" + std::to_wstring(::GetLastError()) + L")";
        return false;
    }
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        ::CloseHandle(proc);
        return false;
    }
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    bool okFirst = false;
    if (g_modFirstEx && g_modNextEx)
        okFirst = g_modFirstEx(snap, &me, proc);
    else
        okFirst = ::Module32FirstW(snap, &me);  // 同位数进程走老接口
    if (!okFirst && foundPath)
        *foundPath = L"(模块枚举失败 err=" + std::to_wstring(::GetLastError()) + L")";
    if (okFirst) {
        do {
            std::wstring file = me.szModule ? me.szModule : L"";
            std::wstring path = me.szExePath ? me.szExePath : L"";
            bool isDll = (_wcsicmp(file.c_str(), L"qywb64.dll") == 0 ||
                          _wcsicmp(file.c_str(), L"qywb32.dll") == 0);
            if (!isDll) continue;
            // 只算安装目录里的那份；被改名待重启替换的也算
            if (path.find(installDir) != std::wstring::npos ||
                path.find(L".qypend") != std::wstring::npos) {
                found = true;
                if (foundPath) *foundPath = path;
                break;
            }
            if (foundPath && foundPath->empty()) *foundPath = L"(同名但不在安装目录) " + path;
        } while (g_modFirstEx && g_modNextEx ? g_modNextEx(snap, &me, proc)
                                            : ::Module32NextW(snap, &me));
    }
    ::CloseHandle(snap);
    ::CloseHandle(proc);
    return found;
}

static BOOL CALLBACK CloseWindowsProc(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == (DWORD)lp) ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

//------------------------------------------------------------------------------
// Restart Manager：与位数无关地找出“占着某个文件”的进程
//
// Toolhelp 的模块枚举有个坑：32 位进程看不到 64 位进程的模块（MinGW 的
// Module32FirstEx 也不可靠）。Restart Manager 是系统自带的，与位数无关，
// 正好用来补这块：把“被改名让位的旧文件”交给它，它能报出所有占着它的程序。
// MinGW 没带 rstrtmgr.h，这里自己声明需要的部分，运行时从 rstrtmgr.dll 取。
//------------------------------------------------------------------------------
struct RmUniqueProcess {
    DWORD dwProcessId;
    FILETIME ProcessStartTime;
};
#define CCH_RM_MAX_APP_NAME 255
#define CCH_RM_MAX_SVC_NAME 63
struct RmProcessInfo {
    RmUniqueProcess Process;
    wchar_t strAppName[CCH_RM_MAX_APP_NAME + 1];
    wchar_t strServiceShortName[CCH_RM_MAX_SVC_NAME + 1];
    DWORD ApplicationType;
    ULONG AppStatus;
    DWORD TSSessionId;
    BOOL bRestartable;
};
typedef DWORD(WINAPI* PRmStartSession)(DWORD*, DWORD, LPCWSTR);
typedef DWORD(WINAPI* PRmRegisterResources)(DWORD, UINT, LPCWSTR*, UINT, RmUniqueProcess*, UINT,
                                            LPCWSTR*);
typedef DWORD(WINAPI* PRmGetList)(DWORD, UINT*, UINT*, RmProcessInfo*, LPDWORD);
typedef DWORD(WINAPI* PRmShutdown)(DWORD, DWORD*, LPDWORD);
typedef DWORD(WINAPI* PRmEndSession)(DWORD);

static bool CollectHostsByRm(const std::vector<std::wstring>& files,
                             std::vector<HostProc>& hosts) {
    if (files.empty()) return false;
    HMODULE rm = ::LoadLibraryW(L"rstrtmgr.dll");
    if (!rm) return false;
    PRmStartSession pStart = (PRmStartSession)::GetProcAddress(rm, "RmStartSession");
    PRmRegisterResources pReg = (PRmRegisterResources)::GetProcAddress(rm, "RmRegisterResources");
    PRmGetList pGet = (PRmGetList)::GetProcAddress(rm, "RmGetList");
    PRmEndSession pEnd = (PRmEndSession)::GetProcAddress(rm, "RmEndSession");
    if (!pStart || !pReg || !pGet || !pEnd) {
        ::FreeLibrary(rm);
        return false;
    }
    DWORD session = 0;
    wchar_t key[64]{};
    ::wsprintfW(key, L"QiuYu%lu", ::GetCurrentProcessId());
    if (pStart(&session, 0, key) != 0) {
        ::FreeLibrary(rm);
        return false;
    }
    std::vector<LPCWSTR> arr;
    arr.reserve(files.size());
    for (const auto& f : files) arr.push_back(f.c_str());
    pReg(session, (UINT)arr.size(), arr.data(), 0, nullptr, 0, nullptr);

    UINT needed = 0, count = 0;
    DWORD reasons = 0;
    DWORD rc = pGet(session, &needed, &count, nullptr, &reasons);
    if (rc == 234 /*ERROR_MORE_DATA*/ && needed > 0) {
        std::vector<RmProcessInfo> infos(needed);
        count = needed;
        rc = pGet(session, &needed, &count, infos.data(), &reasons);
        if (rc == 0) {
            for (UINT i = 0; i < count; ++i) {
                DWORD pid = infos[i].Process.dwProcessId;
                if (!pid || pid == ::GetCurrentProcessId()) continue;
                bool dup = false;
                for (const auto& h : hosts)
                    if (h.pid == pid) {
                        dup = true;
                        break;
                    }
                if (dup) continue;
                HostProc hp;
                hp.pid = pid;
                // 应用名比进程名好用（能显示“微信”“Chrome”这类）
                if (infos[i].strAppName[0])
                    ::lstrcpynW(hp.name, infos[i].strAppName, MAX_PATH);
                else
                    ::lstrcpynW(hp.name, L"(未知程序)", MAX_PATH);
                hosts.push_back(hp);
            }
        }
    }
    pEnd(session);
    ::FreeLibrary(rm);
    return true;
}

// 枚举还加载着旧组件的进程
static std::vector<HostProc> FindHostsOfOldComponents(const std::wstring& installDir) {
    std::vector<HostProc> hosts;
    EnsureModule32Ex();
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return hosts;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == ::GetCurrentProcessId()) continue;
            if (IsSkipProcess(pe.szExeFile)) continue;
            std::wstring hitPath;
            if (!ProcessHoldsOldDll(pe.th32ProcessID, installDir, &hitPath)) {
                if (!hitPath.empty())
                    LogFile(L"[诊断] %s (pid %u) 也加载了同名模块：%s", pe.szExeFile,
                            (unsigned)pe.th32ProcessID, hitPath.c_str());
                continue;
            }
            HostProc hp;
            hp.pid = pe.th32ProcessID;
            ::lstrcpynW(hp.name, pe.szExeFile, MAX_PATH);
            hosts.push_back(hp);
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
    // 模块枚举只能看到同位数的进程，再用 Restart Manager 补上“被改名让位的
    // 旧文件”那一路（它与位数无关），两边合并去重
    std::vector<std::wstring> rmFiles = g_oldMappedFiles;
    // 安装目录里还可能留着以前更新改名出来的 .old / .qypend 文件，
    // 它们同样被运行中的程序占着，一并交给 Restart Manager
    WIN32_FIND_DATAW fd{};
    std::wstring pat = installDir + L"\\*";
    HANDLE ff = ::FindFirstFileW(pat.c_str(), &fd);
    if (ff != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName ? fd.cFileName : L"";
            std::wstring lower = name;
            for (auto& c : lower) c = (wchar_t)::towlower(c);
            if (lower.find(L".old") != std::wstring::npos ||
                lower.find(L".qypend") != std::wstring::npos) {
                rmFiles.push_back(installDir + L"\\" + name);
            }
        } while (::FindNextFileW(ff, &fd));
        ::FindClose(ff);
    }
    if (!rmFiles.empty()) CollectHostsByRm(rmFiles, hosts);
    return hosts;
}

// 干跑：只列出占用旧组件的程序，不结束任何东西（--list-hosts）
static int ListHostsOfOldComponents(const std::wstring& installDir) {
    std::vector<HostProc> hosts = FindHostsOfOldComponents(installDir);
    wchar_t buf[256]{};
    ::wsprintfW(buf, L"仍加载旧组件的程序：%u 个", (unsigned)hosts.size());
    LogFile(L"[%s] %s", installDir.c_str(), buf);
    for (const auto& h : hosts) {
        LogFile(L"[%s]   %s (pid %u)", installDir.c_str(), h.name, (unsigned)h.pid);
        wchar_t line[256]{};
        ::wsprintfW(line, L"%s (pid %u)", h.name, (unsigned)h.pid);
        ::wprintf(L"%s\n", line);
    }
    return (int)hosts.size();
}

// 结束占用旧组件的进程；返回被结束的程序数
static int TerminateHostsOfOldComponents(const std::wstring& installDir) {
    std::vector<HostProc> hosts = FindHostsOfOldComponents(installDir);
    if (hosts.empty()) return 0;

    LogFile(L"[%s] 关闭后结束 %u 个仍加载旧组件的程序", installDir.c_str(),
            (unsigned)hosts.size());

    // 第一步：礼貌关闭，让应用有机会自己保存
    for (auto& h : hosts) {
        ::EnumWindows(CloseWindowsProc, (LPARAM)h.pid);
        h.closed = true;
    }
    // 给一点时间（最多 1.2 秒）
    for (int i = 0; i < 12; ++i) {
        ::Sleep(100);
        bool allGone = true;
        for (auto& h : hosts) {
            HANDLE ph = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, h.pid);
            if (ph) {
                DWORD code = 0;
                if (::GetExitCodeProcess(ph, &code) && code == STILL_ACTIVE) allGone = false;
                ::CloseHandle(ph);
            }
        }
        if (allGone) break;
    }
    // 第二步：还活着的强制结束
    for (auto& h : hosts) {
        HANDLE ph = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE,
                                  h.pid);
        if (!ph) continue;
        DWORD code = 0;
        bool alive = ::GetExitCodeProcess(ph, &code) && code == STILL_ACTIVE;
        if (alive) {
            ::TerminateProcess(ph, 0);
            h.forced = true;
            LogFile(L"[%s]   强制结束 %s (pid %u)", installDir.c_str(), h.name, (unsigned)h.pid);
        }
        ::CloseHandle(ph);
    }
    ::Sleep(300);  // 等进程真正退出，文件句柄释放
    return (int)hosts.size();
}

// ---- 界面 ----
static void FillList() {
    ::SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (auto& c : g_changes) {
        std::wstring line = c.f.path;
        while (line.size() < 22) line += L' ';
        line += L"   " + KBText(c.f.size);
        ::SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
}

static void UpdateTipText() {
    std::wstring t;
    if (!g_blockReason.empty()) {
        // 这里不能说「与服务器一致」：服务器确实是旧的，只是我们主动不降级。
        t = g_blockReason;
    } else if (g_changes.empty()) {
        t = L"当前版本 v" + g_curVer + L"，安装目录里的文件已全部是最新（与服务器一致）。";
    } else {
        t = L"当前版本 v" + g_curVer + L"  →  服务器版本 v" + g_newVer;
        if (!g_notes.empty()) t += L"　（" + g_notes + L"）";
        wchar_t b[256];
        ::wsprintfW(b, L"\n只需下载 %d 个有变化的文件，共 %s；未变化的文件不会重复下载。",
                    (int)g_changes.size(), KBText(g_totalBytes).c_str());
        t += b;
    }
    ::SetWindowTextW(g_tip, t.c_str());
}

static void DrawProgress(HDC dc) {
    RECT rc{24, 338, 592, 358};
    HBRUSH track = ::CreateSolidBrush(RGB(226, 230, 238));
    HBRUSH fill = ::CreateSolidBrush(RGB(40, 92, 206));
    HGDIOBJ ob = ::SelectObject(dc, track);
    ::RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
    ::SelectObject(dc, ob);
    int w = rc.right - rc.left;
    int fw = (int)((long long)w * g_prog / 1000);
    if (fw > 2) {
        ::SelectObject(dc, fill);
        ::RoundRect(dc, rc.left, rc.top, rc.left + fw, rc.bottom, 8, 8);
        ::SelectObject(dc, ob);
    }
    ::DeleteObject(track);
    ::DeleteObject(fill);

    wchar_t b[16];
    ::wsprintfW(b, L"%d%%", g_prog / 10);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(120, 128, 142));
    ::SelectObject(dc, g_font);
    ::TextOutW(dc, 600, 337, b, (int)::wcslen(b));
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            g_font = ::CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                   OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, L"Microsoft YaHei");
            g_fontBold = ::CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Microsoft YaHei");
            g_fontTitle = ::CreateFontW(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH, L"Microsoft YaHei");
            HWND t = ::CreateWindowExW(0, L"STATIC", L"增量更新", WS_CHILD | WS_VISIBLE, 24, 16,
                                       300, 34, h, nullptr, nullptr, nullptr);
            ::SendMessageW(t, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);
            g_tip = ::CreateWindowExW(0, L"STATIC", L"正在检查更新…",
                                      WS_CHILD | WS_VISIBLE | SS_LEFT, 24, 58, 592, 84, h,
                                      nullptr, nullptr, nullptr);
            ::SendMessageW(g_tip, WM_SETFONT, (WPARAM)g_font, TRUE);

            g_list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT |
                                           LBS_DISABLENOSCROLL,
                                       24, 150, 592, 150, h, nullptr, nullptr, nullptr);
            ::SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);

            g_step = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 24, 308, 592, 22,
                                       h, nullptr, nullptr, nullptr);
            ::SendMessageW(g_step, WM_SETFONT, (WPARAM)g_fontBold, TRUE);

            g_btnStart = ::CreateWindowExW(0, L"BUTTON", L"开始更新",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                           358, 384, 120, 34, h, (HMENU)ID_START, nullptr,
                                           nullptr);
            g_btnClose = ::CreateWindowExW(0, L"BUTTON", L"关闭",
                                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 496, 384, 120,
                                           34, h, (HMENU)ID_CLOSE, nullptr, nullptr);
            ::SendMessageW(g_btnStart, WM_SETFONT, (WPARAM)g_font, TRUE);
            ::SendMessageW(g_btnClose, WM_SETFONT, (WPARAM)g_font, TRUE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(h, &ps);
            RECT rc;
            ::GetClientRect(h, &rc);
            ::FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
            DrawProgress(dc);
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_UPD_STEP: {
            ::EnterCriticalSection(&g_cs);
            std::wstring s = g_stepText;
            ::LeaveCriticalSection(&g_cs);
            ::SetWindowTextW(g_step, s.c_str());
            return 0;
        }
        case WM_UPD_PROG: {
            ::EnterCriticalSection(&g_cs);
            int p = g_prog;
            ::LeaveCriticalSection(&g_cs);
            g_prog = p;
            RECT r{20, 302, 660, 364};
            ::InvalidateRect(h, &r, FALSE);
            return 0;
        }
        case WM_UPD_DONE: {
            g_busy = false;
            int code = (int)w;
            std::wstring msg;
            ::EnterCriticalSection(&g_cs);
            msg = g_failText;
            ::LeaveCriticalSection(&g_cs);
            if (g_auto) {  // 无人值守：检查完直接更新，结束就退出
                if (code == 0) {
                    if (g_changes.empty()) {
                        if (!g_blockReason.empty())
                            LogFile(L"[%s] 跳过更新：%s", g_installDir.c_str(),
                                    g_blockReason.c_str());
                        else
                            LogFile(L"[%s] 已是最新版本 v%s，无文件需要更新",
                                    g_installDir.c_str(), g_curVer.c_str());
                        ::DestroyWindow(h);
                    } else {
                        LogFile(L"[%s] 需要更新 %d 个文件，共 %u KB", g_installDir.c_str(),
                                (int)g_changes.size(), (unsigned)((g_totalBytes + 1023) / 1024));
                        StartWorker(false);
                    }
                } else if (code == 1) {
                    LogFile(L"[成功] %s", msg.c_str());
                    g_exitCode = 0;
                    ::DestroyWindow(h);
                } else {
                    LogFile(L"[失败] %s", msg.c_str());
                    g_exitCode = 2;
                    ::DestroyWindow(h);
                }
                return 0;
            }
            if (code == 0) {  // 检查完毕
                FillList();
                UpdateTipText();
                if (g_changes.empty()) {
                    ::SetWindowTextW(g_step, g_blockReason.empty()
                                                 ? L"没有需要更新的文件。"
                                                 : L"本地版本更新，已跳过增量更新（避免降级）。");
                    ::SetWindowTextW(g_btnStart, g_blockReason.empty() ? L"已是最新" : L"无需更新");
                    ::EnableWindow(g_btnStart, FALSE);
                } else {
                    wchar_t b[128];
                    ::wsprintfW(b, L"准备就绪：%d 个文件待更新，共 %s。",
                                (int)g_changes.size(), KBText(g_totalBytes).c_str());
                    ::SetWindowTextW(g_step, b);
                    ::SetWindowTextW(g_btnStart, L"开始更新");
                    ::EnableWindow(g_btnStart, TRUE);
                }
            } else if (code == 1) {
                ::SetWindowTextW(g_step, L"更新完成。");
                g_prog = 1000;
                ::InvalidateRect(h, nullptr, FALSE);
                ::EnableWindow(g_btnStart, FALSE);
                ::SetWindowTextW(g_btnStart, L"已完成");
                ::MessageBoxW(h, msg.c_str(), L"增量更新完成", MB_OK | MB_ICONINFORMATION);
            } else {
                ::SetWindowTextW(g_step, L"更新未完成。");
                ::MessageBoxW(h, msg.c_str(), L"增量更新", MB_OK | MB_ICONWARNING);
                ::EnableWindow(g_btnStart, g_changes.empty() ? FALSE : TRUE);
            }
            ::EnableWindow(g_btnClose, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == ID_START) {
                if (g_blockReason.empty() && !g_changes.empty()) StartWorker(false);
            } else if (id == ID_CLOSE) {
                if (g_busy) {
                    if (::MessageBoxW(h, L"更新还没结束，确定要退出吗？", L"秋雨五笔",
                                      MB_YESNO | MB_ICONQUESTION) != IDYES)
                        return 0;
                    ::InterlockedExchange(&g_cancel, 1);
                }
                // 更新过了：把还加载旧组件的程序结束掉，让新版本立刻生效。
                // 不再静默结束——先列出是谁，问用户“现在结束还是稍后重启”。
                if (g_updated && !g_installDir.empty()) {
                    std::vector<HostProc> hosts = FindHostsOfOldComponents(g_installDir);
                    if (!hosts.empty()) {
                        std::wstring names;
                        for (size_t i = 0; i < hosts.size() && i < 6; ++i) {
                            names += L"\n  · ";
                            names += hosts[i].name;
                        }
                        if (hosts.size() > 6) names += L"\n  · …";
                        wchar_t head[128];
                        ::wsprintfW(head, L"有 %u 个程序仍在使用旧版本的输入法组件：",
                                    (unsigned)hosts.size());
                        int r = ::MessageBoxW(
                            h,
                            (std::wstring(head) + names +
                             L"\n\n“是”：现在结束这些程序，新版立刻生效"
                             L"（请先保存它们里未保存的文档）\n"
                             L"“否”：暂不结束，稍后重启电脑时自动完成替换")
                                .c_str(),
                            L"秋雨五笔 更新", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
                        if (r == IDYES) {
                            int n = TerminateHostsOfOldComponents(g_installDir);
                            if (n > 0)
                                ::MessageBoxW(h,
                                              L"已结束仍加载旧组件的程序。\n\n"
                                              L"重新打开它们就会用上最新版本。",
                                              L"秋雨五笔", MB_OK | MB_ICONINFORMATION);
                        } else {
                            LogFile(L"[%s] 用户选择稍后重启，不结束占用程序",
                                    g_installDir.c_str());
                        }
                    }
                }
                ::DestroyWindow(h);
            }
            return 0;
        }
        case WM_CLOSE:
            ::SendMessageW(h, WM_COMMAND, ID_CLOSE, 0);
            return 0;
        case WM_DESTROY:
            if (g_font) ::DeleteObject(g_font);
            if (g_fontBold) ::DeleteObject(g_fontBold);
            if (g_fontTitle) ::DeleteObject(g_fontTitle);
            ::PostQuitMessage(g_exitCode);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    std::wstring dir = ModuleDir(hInst);
    if (::GetFileAttributesW((dir + L"\\qywb64.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 不是从安装目录运行的：按注册表里的安装位置找
        HKEY k = nullptr;
        wchar_t buf[MAX_PATH * 2] = {0};
        DWORD n = sizeof(buf) - sizeof(wchar_t), type = 0;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QiuYuWubi",
                            0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (::RegQueryValueExW(k, L"InstallLocation", nullptr, &type, (LPBYTE)buf, &n) ==
                    ERROR_SUCCESS &&
                type == REG_SZ)
                dir = buf;
            ::RegCloseKey(k);
        }
    }
    if (::GetFileAttributesW((dir + L"\\qywb64.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        ::MessageBoxW(nullptr,
                      L"没有找到秋雨五笔输入法的安装目录。\n请从安装目录运行更新程序，"
                      L"或直接用完整安装包更新。",
                      L"增量更新", MB_OK | MB_ICONWARNING);
        return 1;
    }
    g_installDir = dir;
    g_curVer = InstalledVersionOnDisk(dir);
    g_localVerTrusted = !g_curVer.empty();
    if (!g_localVerTrusted) g_curVer = CurrentVersion();

    // 本地构建戳：优先 installed.build（安装程序/上次更新写入）；老版本装的目录
    // 里没有这个文件，退回用 installed.ver 的写入时间，并顺手补写一份。
    g_localBuild = InstalledBuildOnDisk(dir);
    if (g_localBuild.empty()) {
        g_localBuild = BuildStampFromInstalledVerMtime(dir);
        if (!g_localBuild.empty()) WriteInstalledBuild(dir, g_localBuild);
    }
    g_localBuildTrusted = !g_localBuild.empty();

    // --list-hosts：干跑，列出仍加载旧组件的程序（只读，不需要管理员权限）
    {
        const wchar_t* cmd0 = ::GetCommandLineW();
        if (cmd0 && ::wcsstr(cmd0, L"--list-hosts")) {
            ListHostsOfOldComponents(g_installDir);
            return 0;
        }
    }

    // 写 Program Files 需要管理员权限：普通身份启动时提权重开一次
    if (!InstallDirWritable(dir)) {
        const wchar_t* cmd = ::GetCommandLineW();
        if (!cmd || !::wcsstr(cmd, L"--elevated")) {
            if (RelaunchElevated()) return 0;
            ::MessageBoxW(nullptr,
                          L"没有写入安装目录的权限。\n请右键选择“以管理员身份运行”，"
                          L"或用完整安装包更新。",
                          L"增量更新", MB_OK | MB_ICONWARNING);
            return 1;
        }
    }

    // --kill-hosts：结束仍加载旧组件的程序后退出（供安装程序收尾调用，
    // 安装程序已提权，这里通常直接继承管理员令牌）。返回被结束的数量。
    {
        const wchar_t* cmdk = ::GetCommandLineW();
        if (cmdk && ::wcsstr(cmdk, L"--kill-hosts")) {
            int n = TerminateHostsOfOldComponents(g_installDir);
            LogFile(L"[%s] --kill-hosts：结束 %d 个仍加载旧组件的程序", g_installDir.c_str(), n);
            return n > 0 ? 0 : 3;  // 3 = 没有需要结束的程序
        }
    }

    {
        const wchar_t* cmd = ::GetCommandLineW();
        if (cmd && ::wcsstr(cmd, L"--auto")) {
            g_auto = true;
            LogFile(L"---- 开始自动更新：%s（当前 v%s）", g_installDir.c_str(), g_curVer.c_str());
        }
    }

    // 只允许一个实例
    HWND old = g_auto ? nullptr : ::FindWindowW(L"QyUpdMain", nullptr);
    if (old) {
        ::ShowWindow(old, SW_SHOWNORMAL);
        ::SetForegroundWindow(old);
        return 0;
    }

    ::InitializeCriticalSection(&g_cs);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QyUpdMain";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32,
                                   LR_DEFAULTCOLOR);
    if (!wc.hIcon)
        wc.hIcon = (HICON)::LoadImageW(nullptr, L"qywb.ico", IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
    wc.hIconSm = wc.hIcon;
    ::RegisterClassExW(&wc);

    int w = 656, hgt = 476;
    int sw = ::GetSystemMetrics(SM_CXSCREEN), sh = ::GetSystemMetrics(SM_CYSCREEN);
    g_main = ::CreateWindowExW(0, L"QyUpdMain", L"秋雨五笔输入法 - 增量更新",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               (sw - w) / 2, (sh - hgt) / 2, w, hgt, nullptr, nullptr, hInst,
                               nullptr);
    if (!g_main) return 1;
    if (!g_auto) {
        ::ShowWindow(g_main, SW_SHOW);
        ::UpdateWindow(g_main);
    }
    StartWorker(true);  // 进来就自动检查一次

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::DeleteCriticalSection(&g_cs);
    return g_exitCode;
}
