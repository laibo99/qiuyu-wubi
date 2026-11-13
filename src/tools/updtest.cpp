// 新版本检测自检：版本号比较 / version.txt 解析（UTF-8 与 GBK）/ 本地地址读取
#include <windows.h>
#include <stdio.h>
#include <string>
#include "../core/qy_update.h"

using namespace qy;

static std::wstring g_out;
static int g_fail = 0;

static void Check(const wchar_t* name, bool ok, const wchar_t* detail = L"") {
    if (!ok) ++g_fail;
    g_out += std::wstring(ok ? L"PASS " : L"FAIL ") + name;
    if (detail && detail[0]) g_out += std::wstring(L"  → ") + detail;
    g_out += L"\r\n";
}

int main() {
    // ---- 版本号比较 ----
    Check(L"[1] 3.6.0 > 3.5.0", CompareVersion(L"3.6.0", L"3.5.0") > 0);
    Check(L"[2] 3.5.0 == 3.5", CompareVersion(L"3.5.0", L"3.5") == 0);
    Check(L"[3] 3.10.0 > 3.9.9（逐段数字，不是字符串比较）",
          CompareVersion(L"3.10.0", L"3.9.9") > 0);
    Check(L"[4] v3.6.0 前缀可识别", CompareVersion(L"v3.6.0", L"3.5.0") > 0);
    Check(L"[5] 3.5.0 < 3.5.1", CompareVersion(L"3.5.0", L"3.5.1") < 0);
    Check(L"[6] 空版本号按 0 处理不会崩", CompareVersion(L"", L"3.5.0") < 0);

    // ---- 当前版本取自 qy_version.h（格式必须是 x.y.z，内容随版本走）----
    {
        const std::wstring v = CurrentVersion();
        int dots = 0;
        bool digits = !v.empty();
        for (wchar_t c : v) {
            if (c == L'.') ++dots;
            else if (c < L'0' || c > L'9') digits = false;
        }
        Check(L"[7] 当前版本是 x.y.z 格式", digits && dots == 2, v.c_str());
    }

    // ---- 解析：UTF-8 ----
    {
        std::string utf8 =
            "version=3.9.1\n"
            "url=https://example.com/qywb/setup.exe\n"
            "notes=修复若干问题\n";
        UpdateInfo info;
        bool ok = ParseUpdateManifest(utf8, &info);
        Check(L"[8] UTF-8 清单解析", ok && info.version == L"3.9.1" &&
                                         info.url == L"https://example.com/qywb/setup.exe" &&
                                         info.notes == L"修复若干问题",
              info.version.c_str());
    }

    // ---- 解析：GBK（记事本另存的 ANSI）与中文键名 ----
    {
        std::string gbk = "版本=3.9.2\n地址=D:\\qywb\\setup.exe\n说明=测试中文\n";
        std::string gbkBytes = WToGbk(Utf8ToW(gbk));
        UpdateInfo info;
        bool ok = ParseUpdateManifest(gbkBytes, &info);
        Check(L"[9] GBK 清单 + 中文键名", ok && info.version == L"3.9.2" &&
                                              info.url == L"D:\\qywb\\setup.exe",
              info.version.c_str());
    }

    // ---- 解析：带注释、空行、分节标题 ----
    {
        std::string txt = "\xEF\xBB\xBF# 注释\r\n[更新]\r\n\r\nversion = 4.0\r\nnotes=a=b\r\n";
        UpdateInfo info;
        bool ok = ParseUpdateManifest(txt, &info);
        Check(L"[10] BOM/注释/分节/空格兼容", ok && info.version == L"4.0" &&
                                                info.notes == L"a=b");
    }

    // ---- 解析：叠加双 BOM（用户上传时编辑器再存盘会多一层）----
    {
        std::string txt = "\xEF\xBB\xBF\xEF\xBB\xBFversion=4.3.1\r\nurl=setup.exe\r\n";
        UpdateInfo info;
        bool ok = ParseUpdateManifest(txt, &info);
        Check(L"[19] 双重 BOM 兼容", ok && info.version == L"4.3.1" &&
                                        info.url == L"setup.exe",
              info.version.c_str());
    }

    // ---- 解析：单个 BOM + 中文说明（= 服务器上 version.txt 的正式形态）----
    // 为什么非要带 BOM：version.txt 由浏览器直接打开时，nginx 只给
    // Content-Type: text/plain（没有 charset），浏览器就按本地默认编码
    // （简体中文 Windows 下是 GBK）解码，UTF-8 无 BOM 必然显示乱码。
    // 带单个 BOM 浏览器才会强制走 UTF-8；解析器必须照样认。
    {
        std::string txt =
            "\xEF\xBB\xBFversion=4.4.0\n"
            "url=cqwb.exe\n"
            "inc=inc/manifest.txt\n"
            "notes=v4.4.0：右键菜单新增“手写输入”\n";
        UpdateInfo info;
        bool ok = ParseUpdateManifest(txt, &info);
        Check(L"[20] 单 BOM + 中文说明（发布形态）",
              ok && info.version == L"4.4.0" && info.url == L"cqwb.exe" &&
                  info.incUrl == L"inc/manifest.txt" &&
                  info.notes == L"v4.4.0：右键菜单新增“手写输入”",
              info.version.c_str());
    }

    // ---- 增量清单：架构字段（x64 / x86 / 缺省=any）----
    {
        std::string txt =
            "version=4.3.0\n"
            "file=qywb32.dll|100|aabbccdd|files/aabbccdd.bin|any\n"
            "file=qydemo.exe|200|11223344|files/11223344.bin|x86\n"
            "file=qydemo.exe|300|55667788|files/55667788.bin|x64\n"
            "file=old.txt|400|99887766|files/99887766.bin\n";
        IncManifest m;
        bool ok = ParseIncManifest(txt, &m);
        const IncFile* x86 = nullptr;
        const IncFile* x64 = nullptr;
        const IncFile* any1 = nullptr;
        const IncFile* noarch = nullptr;
        for (auto& f : m.files) {
            if (f.path == L"qydemo.exe" && f.arch == 32 && !x86) x86 = &f;
            if (f.path == L"qydemo.exe" && f.arch == 64 && !x64) x64 = &f;
            if (f.path == L"qywb32.dll" && f.arch == 0 && !any1) any1 = &f;
            if (f.path == L"old.txt" && f.arch == 0 && !noarch) noarch = &f;
        }
        Check(L"[18] 增量清单架构字段解析（x64/x86/缺省any）",
              ok && x86 && x64 && any1 && noarch && m.files.size() == 4);
    }

    // ---- 读取本地文件真实路径 ----
    {
        wchar_t tmp[MAX_PATH] = {0};
        ::GetTempPathW(MAX_PATH, tmp);
        std::wstring p = std::wstring(tmp) + L"qywb_version_test.txt";
        HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            std::string body =
                "version=9.9.9\n"
                "url=https://example.com/x.exe\n";
            DWORD w = 0;
            ::WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
            ::CloseHandle(h);
            UpdateInfo info;
            std::wstring err;
            bool ok = FetchUpdateInfo(p, &info, &err);
            Check(L"[11] 从本地文件读取更新信息",
                  ok && info.version == L"9.9.9" && info.url == L"https://example.com/x.exe",
                  ok ? L"" : err.c_str());
            ::DeleteFileW(p.c_str());
        } else {
            Check(L"[11] 从本地文件读取更新信息", false, L"无法创建临时文件");
        }
    }

    // ---- 不存在的路径要给出错误而不是崩溃 ----
    {
        UpdateInfo info;
        std::wstring err;
        bool ok = FetchUpdateInfo(L"Z:\\definitely\\missing\\version.txt", &info, &err);
        Check(L"[12] 文件不存在 → 返回失败并带原因", !ok && !err.empty(), err.c_str());
    }

    // ---- 空地址 ----
    {
        UpdateInfo info;
        std::wstring err;
        bool ok = FetchUpdateInfo(L"", &info, &err);
        Check(L"[13] 空地址 → 提示未配置", !ok && err.find(L"未配置") != std::wstring::npos,
              err.c_str());
    }

    // ---- 地址写成“目录”：自动补 version.txt，且清单里的相对下载地址按目录解析 ----
    {
        wchar_t tmp[MAX_PATH] = {0};
        ::GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"qywb_upd_dir";
        ::CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring manifest = dir + L"\\version.txt";
        HANDLE h = ::CreateFileW(manifest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        bool okCreate = false;
        if (h != INVALID_HANDLE_VALUE) {
            std::string body = "version=9.9.9\nurl=秋雨五笔输入法_Setup.exe\n";
            DWORD w = 0;
            okCreate = ::WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr) && w == body.size();
            ::CloseHandle(h);
        }
        UpdateInfo info;
        std::wstring err;
        // 传目录（末尾带反斜杠）→ 应自动找目录下的 version.txt
        bool ok = okCreate && FetchUpdateInfo(dir + L"\\", &info, &err);
        Check(L"[14] 目录地址自动补 version.txt", ok && info.version == L"9.9.9",
              ok ? info.version.c_str() : err.c_str());
        Check(L"[15] 清单里的相对地址按目录解析", ok && info.url == dir + L"\\秋雨五笔输入法_Setup.exe",
              info.url.c_str());
        ::DeleteFileW(manifest.c_str());
        ::RemoveDirectoryW(dir.c_str());
    }

    // ---- 目录地址不存在时，报错信息里指向补全后的 version.txt ----
    {
        UpdateInfo info;
        std::wstring err;
        bool ok = FetchUpdateInfo(L"Z:\\definitely\\missing\\", &info, &err);
        Check(L"[16] 目录不存在 → 失败并带上 version.txt",
              !ok && err.find(L"version.txt") != std::wstring::npos, err.c_str());
    }

    // ---- 内置默认地址 ----
    {
        std::wstring u = DefaultUpdateUrl();
        Check(L"[17] 内置默认更新地址已配置", !u.empty() && u.find(L"http") != std::wstring::npos,
              u.c_str());
    }

    // ---- 增量更新防降级（本地 4.5.3 / 服务器 4.5.2 的真实事故）----
    // qyupd 只按 CRC 比对，服务器挂着旧清单时会把新版文件全判成「待更新」，
    // 点开始更新就是降级。判定必须是「严格大于」，同版本仍要放行 CRC 热修。
    Check(L"[21] 本地比服务器 version.txt 新 → 阻断",
          IncUpdateWouldDowngrade(L"4.5.3", L"4.5.2", L"4.5.2", true));
    Check(L"[22] version.txt 已更新但增量清单还是旧的 → 同样阻断",
          IncUpdateWouldDowngrade(L"4.5.3", L"4.5.3", L"4.5.2", true));
    Check(L"[23] 本地与服务器同版本 → 放行（修复按 CRC 热修，不升版本号）",
          !IncUpdateWouldDowngrade(L"4.5.3", L"4.5.3", L"4.5.3", true));
    Check(L"[24] 本地比服务器旧 → 放行（正常升级）",
          !IncUpdateWouldDowngrade(L"4.5.2", L"4.5.3", L"4.5.3", true));
    Check(L"[25] installed.ver 读不到（编译期版本兜底）→ 不阻断",
          !IncUpdateWouldDowngrade(L"4.5.3", L"4.5.2", L"4.5.2", false));
    Check(L"[26] 服务器两个版本号都缺失 → 保持按 CRC 比对",
          !IncUpdateWouldDowngrade(L"4.5.3", L"", L"", true));
    Check(L"[27] 4.5.10 > 4.5.9 按数字段比较（不是字符串比较）",
          IncUpdateWouldDowngrade(L"4.5.10", L"4.5.9", L"4.5.9", true) &&
              !IncUpdateWouldDowngrade(L"4.5.9", L"4.5.10", L"4.5.10", true));

    // ---- 同版本不同构建（真实事故：本机装了 15:05 的 4.5.3，服务器清单还是
    //      10:22 的 4.5.3）----
    // 版本号规则此时完全失效（同版本一律放行），只能靠构建戳判方向。
    Check(L"[28] 同版本、本地构建更新 → 阻断（旧清单会把新文件覆盖回去）",
          IncUpdateIsStaleBuild(L"4.5.3", L"4.5.3", L"20261108150500", L"20261108102200", true,
                                true));
    Check(L"[29] 同版本、本地构建更旧 → 放行（服务器有新构建的热修）",
          !IncUpdateIsStaleBuild(L"4.5.3", L"4.5.3", L"20261108102200", L"20261108150500", true,
                                 true));
    Check(L"[30] 同版本、构建相同 → 放行（个别文件损坏仍要能按 CRC 修）",
          !IncUpdateIsStaleBuild(L"4.5.3", L"4.5.3", L"20261108150500", L"20261108150500", true,
                                 true));
    Check(L"[31] 版本不同 → 构建判定不参与（交给版本号规则）",
          !IncUpdateIsStaleBuild(L"4.5.4", L"4.5.3", L"20261108150500", L"20261108102200", true,
                                 true));
    Check(L"[32] installed.build 缺失（老版本装的）→ 不阻断",
          !IncUpdateIsStaleBuild(L"4.5.3", L"4.5.3", L"", L"20261108102200", true, false));
    Check(L"[33] 构建戳格式非法 → 不阻断（宁可少拦，不可误拦）",
          !IncUpdateIsStaleBuild(L"4.5.3", L"4.5.3", L"2026-11-08", L"20261108102200", true,
                                 true) &&
              !IncUpdateIsStaleBuild(L"4.5.3", L"4.5.3", L"20261108150500", L"", true, true));
    Check(L"[34] 构建戳按数值比较（跨日/跨月）",
          CompareBuildStamp(L"20261201000000", L"20261130235959") > 0 &&
              CompareBuildStamp(L"20261101000000", L"20261102000000") < 0 &&
              CompareBuildStamp(L"20261108150500", L"20261108150500") == 0);

    // ---- installed.build 落盘往返 ----
    {
        wchar_t tmp[MAX_PATH] = {0};
        ::GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"qyupd_build_test";
        ::CreateDirectoryW(dir.c_str(), nullptr);
        bool okWrite = WriteInstalledBuild(dir, L"20261108150500");
        std::wstring back = InstalledBuildOnDisk(dir);
        Check(L"[35] installed.build 写入/读取往返",
              okWrite && back == L"20261108150500", back.c_str());
        Check(L"[36] 非法构建戳拒绝写盘（不产生垃圾文件）",
              !WriteInstalledBuild(dir, L"abc") && !WriteInstalledBuild(dir, L""));
        ::DeleteFileW((dir + L"\\installed.build").c_str());
        ::RemoveDirectoryW(dir.c_str());
    }

    g_out += g_fail ? L"RESULT: FAIL\r\n" : L"RESULT: ALL PASS\r\n";
    FILE* f = nullptr;
    _wfopen_s(&f, L"build/updtest.txt", L"wb");
    if (f) {
        std::string u8 = WToUtf8(g_out);
        fwrite(u8.data(), 1, u8.size(), f);
        fclose(f);
    }
    return g_fail ? 1 : 0;
}
