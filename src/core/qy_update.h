// 秋雨五笔输入法 - 新版本检测
//
// 更新信息放在一个纯文本 version.txt 里，格式（每行 key=value，顺序随意）：
//
//   version=3.6.0
//   url=https://example.com/qywb/秋雨五笔输入法_Setup.exe
//   notes=修复若干问题
//
// 地址既可以是 http/https 网址，也可以是本地/共享路径（D:\qywb\version.txt）。
// 检测只负责“发现新版本”，下载与安装必须由用户点确认后自己完成，
// 不做静默升级。
#pragma once
#include "qy_common.h"
#include "qy_version.h"
#include <functional>

namespace qy {

struct UpdateInfo {
    std::wstring version;  // 最新版本号，如 3.6.0
    std::wstring url;      // 安装包地址（可空：为空时用 version.txt 的地址）
    std::wstring notes;    // 更新说明（可空，单行）
    std::wstring incUrl;   // 增量清单地址（version.txt 里的 inc=，可空＝不支持增量）
    bool valid() const { return !version.empty(); }
};

// ---- 增量更新 ----
//
// 服务器上放一份清单（inc/manifest.txt），逐行描述“这一版每个文件的内容指纹”：
//
//   version=4.1.0
//   notes=只更新有变动的文件
//   file=qywb64.dll|966144|a1b2c3d4|files/a1b2c3d4.bin|x64
//
// 客户端把每个条目和安装目录里的同名文件做 CRC32 比对，只下载指纹不同的
// 那几个 —— 完整包 9MB 里 4.7MB 是码表，日常改动往往只有几百 KB。
//
// 末尾的 arch 字段（可省，省略 = any）：x64 = 只在 64 位安装包里，
// x86 = 只在 32 位安装包里，any = 两个包都有。同一安装目录路径可以出现
// 两行（如 qydemo.exe 的 x64/x86 二进制），客户端只比对匹配自己架构的行。
struct IncFile {
    std::wstring path;         // 相对安装目录，如 qywb64.dll / data\qywb.dat
    unsigned long long size;   // 字节数
    unsigned long crc;         // CRC32
    std::wstring url;          // 下载地址（已按清单地址解析成绝对地址）
    int arch = 0;              // 0=any 64=x64 32=x86
};

// 本程序自身的架构（x86 的 qyupd 只处理 x86/any 条目）
inline int SelfArch() { return sizeof(void*) >= 8 ? 64 : 32; }

struct IncManifest {
    std::wstring version;
    std::wstring build;  // 构建戳（14 位 YYYYMMDDHHMMSS，可空＝老清单没有这一行）
    // 清单文件自身的时间戳（HTTP 的 Last-Modified / 本地文件的修改时间）。
    // 老清单没写 build= 时用它兜底判「服务器这份清单是什么时候放上去的」。
    std::wstring serverTime;
    std::wstring notes;
    std::vector<IncFile> files;
    bool valid() const { return !files.empty(); }
};

// CRC32（标准多项式，与 zlib.crc32 一致；分块累加时把上次结果传回来）
unsigned long Crc32(unsigned long crc, const void* data, size_t n);
// 计算磁盘文件指纹；失败返回 false
bool Crc32File(const std::wstring& path, unsigned long* crc, unsigned long long* size);

bool ParseIncManifest(const std::string& bytes, IncManifest* out);
// serverTime（可选）返回清单文件自身的时间戳：HTTP 取 Last-Modified，本地取 mtime。
bool FetchIncManifest(const std::wstring& url, IncManifest* out, std::wstring* err,
                      std::wstring* serverTime = nullptr);

// 相对地址按 base 所在目录解析（清单里的 files/xxx.bin 用得到）
std::wstring ResolveRelativeUrl(const std::wstring& base, const std::wstring& rel);

// 下载到本地文件；url 也允许是本地/共享路径（直接复制），便于离线测试。
// progress 返回 false 表示用户取消。失败时 err 给出原因。
using DownloadProgress = std::function<bool(unsigned long long got, unsigned long long total)>;
bool DownloadFile(const std::wstring& url, const std::wstring& localPath,
                  const DownloadProgress& progress, std::wstring* err);

// 当前程序版本（来自 qy_version.h）
inline std::wstring CurrentVersion() { return QY_VERSION_STR; }

// 安装目录里“实际装好的版本”（读 installed.ver）。
//
// 覆盖安装时，旧版 DLL 往往还被 explorer / Office 等进程映射着，本进程
// 运行的仍是旧代码——直接显示编译期版本就会“更新完还显示旧版本号”。
// 关于框、菜单等处一律用这个函数读磁盘上的真实版本；读不到才回退到
// 编译期版本。
std::wstring InstalledVersionOnDisk(const std::wstring& dir);

// 内置的默认更新地址（来自 qy_version.h，可写成目录）
inline std::wstring DefaultUpdateUrl() { return QY_DEFAULT_UPDATE_URL; }

// 版本号比较：a > b 返回正数，a < b 返回负数，相同返回 0。
// 逐段按数字比较（"3.10.0" > "3.9.9"），忽略前缀 v 与多余的后缀。
int CompareVersion(const std::wstring& a, const std::wstring& b);

// 增量更新防降级：本地版本比服务器声明的版本新时返回 true，必须放弃本次更新。
//
// 背景：qyupd 只按 CRC 比对文件，不看版本号。若本地用完整安装包升到了 4.5.3、
// 服务器还挂着 4.5.2 的 version.txt / inc/manifest.txt，旧清单里每个文件的 CRC
// 都与本地不同 → 会被判成「一堆待更新文件」，点开始更新就是把 4.5.3 回退成
// 4.5.2，最后还把 installed.ver 写成旧号。
//
// 规则（发布约定）：
//   · 本地 > 服务器（version.txt 或增量清单任一较旧）→ 阻断，绝不写入；
//   · 本地 == 服务器 → 放行。日常修复默认按 CRC 热修发布、不升版本号，
//     同版本必须仍能按 CRC 修文件，不能一刀切用 >= 拦掉；
//   · localTrusted=false（installed.ver 读不到，只能拿编译期版本兜底）→ 不阻断，
//     避免「旧组件 + 新安装目录」被误判成本地更新而挡住正常升级。
bool IncUpdateWouldDowngrade(const std::wstring& localVer, const std::wstring& versionTxtVer,
                             const std::wstring& manifestVer, bool localTrusted);

// 构建戳比较：a 比 b 新返回正数，旧返回负数，相同/无法比较返回 0。
// 只认 14 位纯数字（YYYYMMDDHHMMSS），其它格式一律视为「不知道」。
int CompareBuildStamp(const std::wstring& a, const std::wstring& b);

// 增量更新防「同版本降级」：服务器清单所属的构建比本机安装的构建旧时返回 true。
//
// 背景：不升版本号发布是常态，同一版本号对应多次构建。服务器上若只更新了
// version.txt、inc/manifest.txt 还是上一次构建的，客户端按 CRC 比对必然列出
// 一批「待更新」文件 —— 点更新就是把本机的新构建回退成旧构建，版本号却一样，
// IncUpdateWouldDowngrade 按版本号根本看不出来。
//
// 规则：
//   · 只在「版本相同」时启用（本地版本更新/更旧由版本号那条规则负责）；
//   · 本地构建**严格新于**服务器清单构建 → 阻断；
//   · 构建相等（同一次构建，只是个别文件被本地改坏）→ 放行，仍可按 CRC 修复；
//   · 任一端构建戳缺失/格式不符 → 不阻断，退化为纯 CRC 比对。
bool IncUpdateIsStaleBuild(const std::wstring& localVer, const std::wstring& manifestVer,
                           const std::wstring& localBuild, const std::wstring& manifestBuild,
                           bool localVerTrusted, bool localBuildTrusted);

// 安装目录里的 installed.build（本地构建戳，由安装程序/更新程序写入）。
// 缺失返回空串；此时调用方可用 installed.ver 的写入时间兜底。
std::wstring InstalledBuildOnDisk(const std::wstring& dir);

// 写入 installed.build（失败返回 false，不影响主流程）
bool WriteInstalledBuild(const std::wstring& dir, const std::wstring& build);

// 解析 version.txt 内容（UTF-8 / GBK 均可，从字节解码）
bool ParseUpdateManifest(const std::string& bytes, UpdateInfo* out);

// 读取更新信息：http/https 网址走 WinINet（带超时），其它按本地文件读。
// 失败时 err 给出简短中文原因。
bool FetchUpdateInfo(const std::wstring& addr, UpdateInfo* out, std::wstring* err);

// 用默认浏览器打开网址（下载页 / 安装包）
void OpenUrl(const std::wstring& url);

}  // namespace qy
