// 秋雨五笔输入法 - 新版本检测实现
#include "qy_update.h"

#include <wininet.h>
#include <shellapi.h>
#include <stdlib.h>

namespace qy {

//------------------------------------------------------------------------------
// 版本号比较
//------------------------------------------------------------------------------
int CompareVersion(const std::wstring& a, const std::wstring& b) {
    // 去掉前缀 v / V，取到第一个非 数字/点 之前的字串
    auto norm = [](const std::wstring& s) {
        size_t i = 0;
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
        if (i < s.size() && (s[i] == L'v' || s[i] == L'V')) ++i;
        std::wstring out;
        for (; i < s.size(); ++i) {
            wchar_t c = s[i];
            if ((c >= L'0' && c <= L'9') || c == L'.')
                out.push_back(c);
            else
                break;
        }
        return out;
    };
    std::wstring x = norm(a), y = norm(b);
    size_t i = 0, j = 0;
    while (i <= x.size() || j <= y.size()) {
        // 取一段数字
        long long n1 = 0, n2 = 0;
        while (i < x.size() && x[i] >= L'0' && x[i] <= L'9') n1 = n1 * 10 + (x[i++] - L'0');
        while (j < y.size() && y[j] >= L'0' && y[j] <= L'9') n2 = n2 * 10 + (y[j++] - L'0');
        if (n1 != n2) return n1 > n2 ? 1 : -1;
        if (i < x.size() && x[i] == L'.') ++i;
        if (j < y.size() && y[j] == L'.') ++j;
        if (i >= x.size() && j >= y.size()) break;
        // 两边都到结尾（如 3.5 与 3.5.0）→ 视为相同
        bool endX = (i >= x.size()), endY = (j >= y.size());
        if (endX && endY) break;
        if (endX) {
            // 剩下 y 的部分全为 0 则相同
            bool allZero = true;
            for (size_t k = j; k < y.size(); ++k)
                if (y[k] >= L'1' && y[k] <= L'9') allZero = false;
            return allZero ? 0 : -1;
        }
        if (endY) {
            bool allZero = true;
            for (size_t k = i; k < x.size(); ++k)
                if (x[k] >= L'1' && x[k] <= L'9') allZero = false;
            return allZero ? 0 : 1;
        }
    }
    return 0;
}

bool IncUpdateWouldDowngrade(const std::wstring& localVer, const std::wstring& versionTxtVer,
                             const std::wstring& manifestVer, bool localTrusted) {
    // 没有可信的本地版本就不拦：编译期版本号只代表「这个 exe 是哪版编的」，
    // 不代表安装目录里装的是哪一版。
    if (!localTrusted || localVer.empty()) return false;
    const std::wstring* servers[] = {&versionTxtVer, &manifestVer};
    for (const std::wstring* v : servers) {
        if (!v->empty() && CompareVersion(localVer, *v) > 0) return true;
    }
    return false;
}

//------------------------------------------------------------------------------
// 构建戳（14 位 YYYYMMDDHHMMSS）
//
// 只认「正好 14 位纯数字」。其它任何形式（空、带 v 前缀、旧清单里干脆没有）
// 都返回 false，调用方据此退化成纯 CRC 比对 —— 宁可少拦，不可误拦。
//------------------------------------------------------------------------------
static bool ParseBuildStamp(const std::wstring& s, unsigned long long* out) {
    if (!out || s.size() != 14) return false;
    unsigned long long v = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + (unsigned long long)(c - L'0');
    }
    *out = v;
    return true;
}

int CompareBuildStamp(const std::wstring& a, const std::wstring& b) {
    unsigned long long x = 0, y = 0;
    if (!ParseBuildStamp(a, &x) || !ParseBuildStamp(b, &y)) return 0;
    if (x == y) return 0;
    return x > y ? 1 : -1;
}

bool IncUpdateIsStaleBuild(const std::wstring& localVer, const std::wstring& manifestVer,
                           const std::wstring& localBuild, const std::wstring& manifestBuild,
                           bool localVerTrusted, bool localBuildTrusted) {
    // 本地信息不可信就不拦：宁可让用户按 CRC 修，也不要凭空挡住正常更新。
    if (!localVerTrusted || !localBuildTrusted) return false;
    if (localVer.empty() || manifestVer.empty()) return false;
    // 版本号不同时由 IncUpdateWouldDowngrade 负责，这里只管「同版本、不同构建」。
    if (CompareVersion(localVer, manifestVer) != 0) return false;
    // 严格大于才拦：相等说明本机就是这一次构建，个别文件被改坏仍应允许按 CRC 修复。
    return CompareBuildStamp(localBuild, manifestBuild) > 0;
}

//------------------------------------------------------------------------------
// 磁盘上的已安装版本（installed.ver）
//------------------------------------------------------------------------------
std::wstring InstalledVersionOnDisk(const std::wstring& dir) {
    if (dir.empty()) return std::wstring();
    std::wstring path = dir + L"\\installed.ver";
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    char raw[64] = {0};
    DWORD got = 0;
    ::ReadFile(h, raw, sizeof(raw) - 1, &got, nullptr);
    ::CloseHandle(h);
    std::wstring v;
    for (DWORD i = 0; i < got; ++i) {
        char c = raw[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') break;
        if (c >= '0' && c <= '9')
            v.push_back((wchar_t)c);
        else if (c == '.' && !v.empty())
            v.push_back(L'.');
    }
    return v;
}

//------------------------------------------------------------------------------
// 磁盘上的构建戳（installed.build）
//
// 安装程序写 = 本次安装包的 QY_BUILD_STAMP；增量更新写完文件后写 = 清单的 build=。
// 老版本装的目录里没有这个文件 → 返回空，由调用方决定怎么兜底。
//------------------------------------------------------------------------------
std::wstring InstalledBuildOnDisk(const std::wstring& dir) {
    if (dir.empty()) return std::wstring();
    std::wstring path = dir + L"\\installed.build";
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    char raw[64] = {0};
    DWORD got = 0;
    ::ReadFile(h, raw, sizeof(raw) - 1, &got, nullptr);
    ::CloseHandle(h);
    std::wstring v;
    for (DWORD i = 0; i < got; ++i) {
        char c = raw[i];
        if (c < '0' || c > '9') break;
        v.push_back((wchar_t)c);
    }
    unsigned long long tmp = 0;
    return ParseBuildStamp(v, &tmp) ? v : std::wstring();
}

bool WriteInstalledBuild(const std::wstring& dir, const std::wstring& build) {
    unsigned long long tmp = 0;
    if (dir.empty() || !ParseBuildStamp(build, &tmp)) return false;
    std::wstring path = dir + L"\\installed.build";
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string bytes(build.begin(), build.end());
    bytes += "\r\n";
    DWORD wrote = 0;
    bool ok = ::WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) != 0 &&
              wrote == (DWORD)bytes.size();
    ::CloseHandle(h);
    return ok;
}

//------------------------------------------------------------------------------
// 字节 → 宽字符（先按严格 UTF-8，失败退回 GBK；记事本另存的 ANSI 文件也能读）
//------------------------------------------------------------------------------
static std::wstring DecodeText(const std::string& bytes) {
    size_t off = 0;
    // 剥掉所有前导 UTF-8 BOM：用户上传时用编辑器再存一次盘常会叠加一层，
    // 只剥一层的话第一行键名前残留 U+FEFF，解析直接失败
    while (off + 3 <= bytes.size() && (unsigned char)bytes[off] == 0xEF &&
           (unsigned char)bytes[off + 1] == 0xBB && (unsigned char)bytes[off + 2] == 0xBF)
        off += 3;
    std::string body = bytes.substr(off);
    if (body.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, body.c_str(), (int)body.size(),
                                  nullptr, 0);
    if (n > 0) {
        std::wstring w(n, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, body.c_str(), (int)body.size(), &w[0], n);
        return w;
    }
    // UTF-16 LE BOM？
    if (body.size() >= 2 && (unsigned char)body[0] == 0xFF && (unsigned char)body[1] == 0xFE)
        return std::wstring((const wchar_t*)(body.data() + 2), (body.size() - 2) / 2);
    return GbkToW(body);
}

static std::wstring TrimW(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n' ||
                     s[b] == 0xFEFF))  // U+FEFF：BOM 剥不干净时的残留
        ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' ||
                     s[e - 1] == L'\n' || s[e - 1] == 0xFEFF))
        --e;
    return s.substr(b, e - b);
}

//------------------------------------------------------------------------------
// 解析 version.txt
//------------------------------------------------------------------------------
bool ParseUpdateManifest(const std::string& bytes, UpdateInfo* out) {
    if (!out) return false;
    std::wstring text = DecodeText(bytes);
    if (text.empty()) return false;
    UpdateInfo info;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, (nl == std::wstring::npos) ? std::wstring::npos
                                                                       : nl - pos);
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        line = TrimW(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        if (line[0] == L'[') continue;  // 允许 [更新] 这类分节标题
        size_t eq = line.find_first_of(L"=:：");
        if (eq == std::wstring::npos) continue;
        std::wstring key = TrimW(line.substr(0, eq));
        std::wstring val = TrimW(line.substr(eq + 1));
        for (auto& c : key)
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (key == L"version" || key == L"最新版本" || key == L"版本" || key == L"ver")
            info.version = val;
        else if (key == L"url" || key == L"地址" || key == L"下载地址" || key == L"setup")
            info.url = val;
        else if (key == L"notes" || key == L"说明" || key == L"更新说明" || key == L"说明文字")
            info.notes = val;
        else if (key == L"inc" || key == L"增量" || key == L"增量清单" || key == L"manifest")
            info.incUrl = val;
    }
    if (!info.valid()) return false;
    *out = info;
    return true;
}

//------------------------------------------------------------------------------
// 读取：本地文件 / http(s)
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// 时间戳：统一成 14 位本地时间 YYYYMMDDHHMMSS
//
// 服务器清单有 build= 时用它（最准）；老清单没有这一行时，退而用清单文件
// 自身的发布时间（HTTP Last-Modified / 本地 mtime），至少能判出「谁更晚」。
// 一律换算成本地时区再格式化：build.sh 的 date 也是本地时间，两边才可比。
//------------------------------------------------------------------------------
static std::wstring StampFromLocalSystemTime(const SYSTEMTIME& st) {
    wchar_t b[20] = {0};
    ::wsprintfW(b, L"%04d%02d%02d%02d%02d%02d", (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    return b;
}

static std::wstring StampFromUtcFileTime(const FILETIME& ft) {
    FILETIME local{};
    if (!::FileTimeToLocalFileTime(&ft, &local)) return std::wstring();
    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&local, &st)) return std::wstring();
    return StampFromLocalSystemTime(st);
}

static std::wstring StampFromFileMtime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return std::wstring();
    return StampFromUtcFileTime(fad.ftLastWriteTime);
}

static bool ReadLocal(const std::wstring& path, std::string* out, std::wstring* err,
                      std::wstring* serverTime = nullptr) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"无法打开更新文件：" + path;
        return false;
    }
    char buf[65536];
    DWORD got = 0;
    std::string data;
    while (data.size() < sizeof(buf) && ::ReadFile(h, buf, sizeof(buf) - (DWORD)data.size(), &got,
                                                   nullptr) && got > 0)
        data.append(buf, got);
    ::CloseHandle(h);
    if (data.empty()) {
        if (err) *err = L"更新文件内容为空：" + path;
        return false;
    }
    *out = data;
    if (serverTime) *serverTime = StampFromFileMtime(path);
    return true;
}

static bool HttpGetWith(const std::wstring& url, DWORD access, std::string* out,
                        std::wstring* err, std::wstring* serverTime = nullptr) {
    HINTERNET ses = ::InternetOpenW(L"QiuYuWubi-Updater", access, nullptr, nullptr, 0);
    if (!ses) {
        if (err) *err = L"无法初始化网络组件";
        return false;
    }
    DWORD tmo = 3000;
    ::InternetSetOptionW(ses, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
    tmo = 6000;
    ::InternetSetOptionW(ses, INTERNET_OPTION_SEND_TIMEOUT, &tmo, sizeof(tmo));
    ::InternetSetOptionW(ses, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo));

    HINTERNET req = ::InternetOpenUrlW(
        ses, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!req) {
        if (err) *err = L"无法连接更新服务器（请检查网络或地址）";
        ::InternetCloseHandle(ses);
        return false;
    }
    DWORD code = 0, len = sizeof(code);
    ::HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr);
    if (code != 0 && code != 200) {
        if (err) {
            wchar_t b[96];
            ::wsprintfW(b, L"更新服务器返回 HTTP %lu", code);
            *err = b;
        }
        ::InternetCloseHandle(req);
        ::InternetCloseHandle(ses);
        return false;
    }
    std::string data;
    char buf[4096];
    DWORD got = 0;
    // 清单文件的发布时间：老清单没有 build= 时靠它判「服务器这份是新是旧」。
    // FLAG_SYSTEMTIME 返回的是 GMT，转本地时区后才能和 build.sh 的本地时间比。
    if (serverTime) {
        SYSTEMTIME st{};
        DWORD sl = sizeof(st);
        if (::HttpQueryInfoW(req, HTTP_QUERY_LAST_MODIFIED | HTTP_QUERY_FLAG_SYSTEMTIME, &st, &sl,
                             nullptr)) {
            FILETIME ft{};
            if (::SystemTimeToFileTime(&st, &ft)) *serverTime = StampFromUtcFileTime(ft);
        }
    }
    while (data.size() < 65536 && ::InternetReadFile(req, buf, sizeof(buf), &got) && got > 0)
        data.append(buf, got);
    ::InternetCloseHandle(req);
    ::InternetCloseHandle(ses);
    if (data.empty()) {
        if (err) *err = L"更新文件内容为空";
        return false;
    }
    *out = data;
    return true;
}

// 先用系统代理设置（企业网 / 代理客户端），失败再直连一次：
// 有些机器装了代理软件（PAC 指向本地端口，但服务没起来），
// 走代理会直接“无法连接”，直连却能通（本机自测、局域网服务器尤其常见）。
static bool HttpGet(const std::wstring& url, std::string* out, std::wstring* err,
                    std::wstring* serverTime = nullptr) {
    std::wstring e1;
    if (HttpGetWith(url, INTERNET_OPEN_TYPE_PRECONFIG, out, &e1, serverTime)) return true;
    std::wstring e2;
    if (HttpGetWith(url, INTERNET_OPEN_TYPE_DIRECT, out, &e2, serverTime)) return true;
    if (err) *err = e1.empty() ? e2 : e1;
    return false;
}

// 更新地址可以写成“目录”（如 http://host/downloads/update/）：
// 自动补上清单文件名 version.txt。
// 判据：以 / 或 \ 结尾，或者 http 地址最后一段里没有“.”（没有扩展名）。
static std::wstring NormalizeManifestUrl(const std::wstring& in) {
    std::wstring a = in;
    if (a.empty()) return a;
    wchar_t back = a.back();
    if (back == L'/' || back == L'\\') return a + L"version.txt";
    bool isHttp = (a.size() > 7 && _wcsnicmp(a.c_str(), L"http://", 7) == 0) ||
                  (a.size() > 8 && _wcsnicmp(a.c_str(), L"https://", 8) == 0);
    if (isHttp) {
        size_t q = a.find_first_of(L"?#");
        std::wstring path = (q == std::wstring::npos) ? a : a.substr(0, q);
        size_t slash = path.find_last_of(L'/');
        if (slash != std::wstring::npos) {
            std::wstring seg = path.substr(slash + 1);
            if (seg.find(L'.') == std::wstring::npos)  // 末尾是目录名，不是文件
                return path + L"/version.txt";
        }
    }
    return a;
}

// 清单里的 url= 允许写相对路径（如 秋雨五笔输入法_Setup.exe），
// 按清单所在目录解析
static std::wstring ResolveDownloadUrl(const std::wstring& manifestUrl, const std::wstring& dl) {
    if (dl.empty()) return dl;
    bool absolute = dl.find(L"://") != std::wstring::npos ||
                    (dl.size() > 1 && dl[1] == L':') || dl[0] == L'\\';
    if (absolute) return dl;
    size_t p = manifestUrl.find_last_of(L"/\\");
    if (p == std::wstring::npos) return dl;
    std::wstring dir = manifestUrl.substr(0, p + 1);
    std::wstring rel = dl;
    while (!rel.empty() && (rel[0] == L'/' || rel[0] == L'\\')) rel.erase(0, 1);
    return dir + rel;
}

bool FetchUpdateInfo(const std::wstring& addr, UpdateInfo* out, std::wstring* err) {
    if (!out) return false;
    std::wstring a = TrimW(addr);
    if (a.empty()) {
        if (err) *err = L"未配置更新地址";
        return false;
    }
    std::wstring manifestUrl = NormalizeManifestUrl(a);
    std::string bytes;
    std::wstring e;
    bool isHttp = (a.size() > 7 && (_wcsnicmp(a.c_str(), L"http://", 7) == 0)) ||
                  (a.size() > 8 && (_wcsnicmp(a.c_str(), L"https://", 8) == 0));
    bool ok = isHttp ? HttpGet(manifestUrl, &bytes, &e) : ReadLocal(manifestUrl, &bytes, &e);
    if (!ok) {
        if (err) *err = e;
        return false;
    }
    if (!ParseUpdateManifest(bytes, out)) {
        if (err) *err = L"更新信息格式不正确（应包含 version= 与 url=）";
        return false;
    }
    out->url = ResolveDownloadUrl(manifestUrl, out->url);
    if (!out->incUrl.empty()) out->incUrl = ResolveDownloadUrl(manifestUrl, out->incUrl);
    return true;
}

//------------------------------------------------------------------------------
// 增量更新：CRC32 / 清单 / 只下载变动文件
//------------------------------------------------------------------------------
unsigned long Crc32(unsigned long crc, const void* data, size_t n) {
    static unsigned long table[256];
    static bool init = false;
    if (!init) {
        for (unsigned long i = 0; i < 256; ++i) {
            unsigned long c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    const unsigned char* p = (const unsigned char*)data;
    crc ^= 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

bool Crc32File(const std::wstring& path, unsigned long* crc, unsigned long long* size) {
    // 共享读 + 允许删除：正在被进程加载的 DLL 也能算指纹（不会因此更新失败）
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    static unsigned char buf[65536];
    unsigned long c = 0;
    unsigned long long total = 0;
    DWORD got = 0;
    while (::ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) {
        c = Crc32(c, buf, got);
        total += got;
    }
    ::CloseHandle(h);
    if (crc) *crc = c;
    if (size) *size = total;
    return true;
}

std::wstring ResolveRelativeUrl(const std::wstring& base, const std::wstring& rel) {
    return ResolveDownloadUrl(base, rel);
}

bool ParseIncManifest(const std::string& bytes, IncManifest* out) {
    if (!out) return false;
    std::wstring text = DecodeText(bytes);
    if (text.empty()) return false;
    IncManifest m;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line =
            text.substr(pos, (nl == std::wstring::npos) ? std::wstring::npos : nl - pos);
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;
        line = TrimW(line);
        if (line.empty() || line[0] == L'#' || line[0] == L';' || line[0] == L'[') continue;
        size_t eq = line.find_first_of(L"=:：");
        if (eq == std::wstring::npos) continue;
        std::wstring key = TrimW(line.substr(0, eq));
        std::wstring val = TrimW(line.substr(eq + 1));
        for (auto& c : key)
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (key == L"version" || key == L"版本")
            m.version = val;
        else if (key == L"build" || key == L"构建")
            m.build = val;
        else if (key == L"notes" || key == L"说明")
            m.notes = val;
        else if (key == L"file" || key == L"文件") {
            // 路径|字节数|CRC32(十六进制)|相对地址
            std::vector<std::wstring> f;
            size_t s = 0;
            while (s <= val.size()) {
                size_t b = val.find(L'|', s);
                f.push_back(TrimW(val.substr(s, (b == std::wstring::npos) ? std::wstring::npos
                                                                         : b - s)));
                if (b == std::wstring::npos) break;
                s = b + 1;
            }
            if (f.size() < 2 || f[0].empty()) continue;
            IncFile e{};
            e.path = f[0];
            e.size = _wcstoui64(f[1].c_str(), nullptr, 10);
            e.crc = (f.size() > 2 && !f[2].empty()) ? (unsigned long)_wcstoui64(f[2].c_str(),
                                                                               nullptr, 16)
                                                    : 0;
            e.url = (f.size() > 3) ? f[3] : std::wstring();
            // 第 5 段：架构（x64 / x86 / 64 / 32，缺省 any）
            if (f.size() > 4) {
                std::wstring a;
                for (wchar_t c : f[4]) {
                    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
                    a.push_back(c);
                }
                if (a == L"x64" || a == L"64" || a == L"amd64")
                    e.arch = 64;
                else if (a == L"x86" || a == L"32" || a == L"i386")
                    e.arch = 32;
            }
            m.files.push_back(e);
        }
    }
    if (!m.valid()) return false;
    *out = m;
    return true;
}

bool FetchIncManifest(const std::wstring& url, IncManifest* out, std::wstring* err,
                      std::wstring* serverTime) {
    if (!out) return false;
    if (url.empty()) {
        if (err) *err = L"未提供增量清单地址";
        return false;
    }
    bool isHttp = url.find(L"://") != std::wstring::npos;
    std::string bytes;
    std::wstring e;
    std::wstring stamp;
    if (!(isHttp ? HttpGet(url, &bytes, &e, &stamp) : ReadLocal(url, &bytes, &e, &stamp))) {
        if (err) *err = e;
        return false;
    }
    if (serverTime) *serverTime = stamp;
    if (!ParseIncManifest(bytes, out)) {
        if (err) *err = L"增量清单格式不正确";
        return false;
    }
    for (auto& f : out->files)
        if (!f.url.empty()) f.url = ResolveDownloadUrl(url, f.url);
    return true;
}

static bool DownloadHttp(const std::wstring& url, const std::wstring& localPath,
                         const DownloadProgress& progress, DWORD access, std::wstring* err) {
    if (url.empty()) {
        if (err) *err = L"下载地址为空";
        return false;
    }
    HINTERNET ses = ::InternetOpenW(L"QiuYuWubi-Updater", access, nullptr, nullptr, 0);
    if (!ses) {
        if (err) *err = L"无法初始化网络组件";
        return false;
    }
    DWORD tmo = 8000;
    ::InternetSetOptionW(ses, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
    tmo = 20000;
    ::InternetSetOptionW(ses, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo));
    HINTERNET req = ::InternetOpenUrlW(
        ses, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!req) {
        if (err) *err = L"无法连接更新服务器";
        ::InternetCloseHandle(ses);
        return false;
    }
    DWORD code = 0, len = sizeof(code);
    ::HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr);
    if (code != 0 && code != 200) {
        if (err) {
            wchar_t b[96];
            ::wsprintfW(b, L"更新服务器返回 HTTP %lu", code);
            *err = b;
        }
        ::InternetCloseHandle(req);
        ::InternetCloseHandle(ses);
        return false;
    }
    unsigned long long total = 0;
    {
        // 用 32 位长度字段：单个组件不会超过 4GB，且各版本 SDK 都定义了这个标志
        DWORD cl = 0, n = sizeof(cl);
        if (::HttpQueryInfoW(req, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &cl, &n,
                             nullptr))
            total = (unsigned long long)cl;
    }
    HANDLE h = ::CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"无法写入临时文件：" + localPath;
        ::InternetCloseHandle(req);
        ::InternetCloseHandle(ses);
        return false;
    }
    static unsigned char buf[65536];
    DWORD got = 0;
    unsigned long long done = 0;
    bool ok = true;
    while (::InternetReadFile(req, buf, sizeof(buf), &got) && got > 0) {
        DWORD wr = 0;
        if (!::WriteFile(h, buf, got, &wr, nullptr) || wr != got) {
            ok = false;
            if (err) *err = L"写入磁盘失败（空间不足或被安全软件拦截）";
            break;
        }
        done += got;
        if (progress && !progress(done, total ? total : done)) {
            ok = false;
            if (err) *err = L"已取消";
            break;
        }
    }
    ::CloseHandle(h);
    ::InternetCloseHandle(req);
    ::InternetCloseHandle(ses);
    return ok;
}

bool DownloadFile(const std::wstring& url, const std::wstring& localPath,
                  const DownloadProgress& progress, std::wstring* err) {
    bool isHttp = url.find(L"://") != std::wstring::npos;
    if (!isHttp) {  // 本地 / 共享路径：直接复制（离线更新或自测用）
        if (!::CopyFileW(url.c_str(), localPath.c_str(), FALSE)) {
            if (err) *err = L"复制失败：" + url;
            return false;
        }
        if (progress) progress(1, 1);
        return true;
    }
    std::wstring e1;
    if (DownloadHttp(url, localPath, progress, INTERNET_OPEN_TYPE_PRECONFIG, &e1)) return true;
    std::wstring e2;
    if (DownloadHttp(url, localPath, progress, INTERNET_OPEN_TYPE_DIRECT, &e2)) return true;
    if (err) *err = e1.empty() ? e2 : e1;
    return false;
}

void OpenUrl(const std::wstring& url) {
    if (url.empty()) return;
    ::ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace qy
