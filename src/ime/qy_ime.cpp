// 秋雨五笔输入法 - TSF 文本服务实现
#define INITGUID
#include <initguid.h>
#include <msctf.h>
#include <objbase.h>
#include <shellapi.h>
#include <new>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "qy_ime.h"
#include "../core/qy_config.h"
#include "../core/qy_update.h"
#include "../core/qy_cleanup.h"
#include "../core/qy_version.h"

extern HINSTANCE g_hInst;
static DWORD g_lastShiftToggleTick = 0;  // Win7 轮询切中英的去重时间戳（防止多实例重复切）

namespace qy {

static std::wstring ModuleDirOf(HMODULE h) {
    wchar_t buf[MAX_PATH * 2] = {0};
    ::GetModuleFileNameW(h, buf, MAX_PATH * 2);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

// 对外显示的版本号 = 安装目录里 installed.ver 记录的真实版本。
// 覆盖安装时旧 DLL 可能仍被 explorer / Office 映射着，本进程跑的是旧代码，
// 用编译期版本会显示成旧号（用户反馈“更新完版本没变”就是这个原因）。
static std::wstring DiskVersion() {
    std::wstring v = InstalledVersionOnDisk(ModuleDirOf(g_hInst));
    if (v.empty()) v = CurrentVersion();
    return v;
}

// 磁盘上已是新版本、但当前进程仍运行旧组件时，给出明确提示（不假装已生效）
static std::wstring StaleNote() {
    std::wstring disk = InstalledVersionOnDisk(ModuleDirOf(g_hInst));
    // 只有“磁盘上比本进程新”才说明正在跑旧组件
    if (disk.empty() || CompareVersion(disk, CurrentVersion()) <= 0) return std::wstring();
    return L"\n\n（当前会话仍运行旧版组件 v" + CurrentVersion() +
           L"，重启电脑或重开正在使用的程序后完全生效）";
}

// 覆盖安装留下的 *.old 备份：占用它的进程一退出就该删掉。
// 每个进程只做一次，放在后台线程，绝不阻塞按键。
static DWORD WINAPI PurgeProc(LPVOID p) {
    std::wstring* dir = (std::wstring*)p;
    qy::PurgeBackupFiles(*dir);
    delete dir;
    return 0;
}

static void PurgeBackupsAsync(const std::wstring& dir) {
    static LONG once = 0;
    if (::InterlockedExchange(&once, 1) != 0) return;
    std::wstring* p = new (std::nothrow) std::wstring(dir);
    if (!p) return;
    HANDLE h = ::CreateThread(nullptr, 0, PurgeProc, p, 0, nullptr);
    if (h)
        ::CloseHandle(h);
    else
        delete p;
}

// ---- 系统手写面板 ----
//
// Win10/11 自带“触摸键盘”（TabTip.exe），其中包含手写输入面板。这里不自造
// 手写识别，直接把系统面板叫出来——即“调用系统自带手写”。
//
// 细节：
//   · 32 位输入法跑在 64 位系统上时，必须用 ProgramW6432 去 64 位的
//     Common Files 里找 TabTip.exe；用 CommonProgramFiles 会落到 Program
//     Files (x86) 下，找不到。
//   · Win7 标准版没有触摸键盘组件，退化为“暂不支持”的提示，不静默失败。
//   · 部分系统默认隐藏手写按钮，写一个 HKCU 开关让它在面板里露出来。
static bool FindInkTip(wchar_t* out, size_t cch) {
    wchar_t base[MAX_PATH * 2] = {0};
    DWORD n = ::GetEnvironmentVariableW(L"ProgramW6432", base, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        std::wstring p =
            std::wstring(base) + L"\\Common Files\\Microsoft Shared\\ink\\TabTip.exe";
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ::lstrcpynW(out, p.c_str(), (int)cch);
            return true;
        }
    }
    n = ::GetEnvironmentVariableW(L"CommonProgramFiles", base, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) {
        std::wstring p = std::wstring(base) + L"\\Microsoft Shared\\ink\\TabTip.exe";
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ::lstrcpynW(out, p.c_str(), (int)cch);
            return true;
        }
    }
    return false;
}

static void EnsureHandwritingButton() {
    HKEY k = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\TabletTip\\1.7", 0, nullptr, 0,
                          KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD one = 1;
        ::RegSetValueExW(k, L"EnableHwcButton", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
        ::RegCloseKey(k);
    }
}

// 未文档化但很稳的接口：资源管理器点任务栏“触摸键盘”按钮，走的就是它。
// Toggle() 按当前状态切换显示/隐藏，正好当作“手写输入”的开关。
//   CLSID_UIHostNoLaunch {4CE576FA-83DC-4F88-951C-9D0782B4E376}
//   IID_ITipInvocation   {37C994E7-432B-4834-A2F7-DCE1F13B834B}
struct QyTipInvocation : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Toggle(HWND hwnd) = 0;
};

static bool TipToggleViaCom() {
    static const GUID kClsid = {0x4CE576FA, 0x83DC, 0x4F88,
                                {0x95, 0x1C, 0x9D, 0x07, 0x82, 0xB4, 0xE3, 0x76}};
    static const GUID kIid = {0x37C994E7, 0x432B, 0x4834,
                              {0xA2, 0xF7, 0xDC, 0xE1, 0xF1, 0x3B, 0x83, 0x4B}};
    // 宿主线程可能已被 TSF 初始化成 STA 或 MTA；重复初始化安全，
    // 只有自己真正初始化成功时才配对 CoUninitialize。
    HRESULT ci = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QyTipInvocation* tip = nullptr;
    HRESULT hr = ::CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_HANDLER | CLSCTX_LOCAL_SERVER,
                                    kIid, (void**)&tip);
    bool ok = SUCCEEDED(hr) && tip;
    if (ok) {
        tip->Toggle(::GetDesktopWindow());
        tip->Release();
    }
    if (SUCCEEDED(ci)) ::CoUninitialize();
    return ok;  // 面板进程没起来 → 交给调用方启动
}

static bool TipWindowVisible() {
    HWND h = ::FindWindowW(L"IPTip_Main_Window", nullptr);
    return h && ::IsWindowVisible(h);
}

// show=true 打开，false 收起
static void ToggleSystemHandwriting(bool show, HWND owner) {
    if (!show) {
        if (TipWindowVisible()) TipToggleViaCom();  // 在显示才收；尽力而为
        return;
    }
    if (TipWindowVisible()) return;  // 已经显示，不重复开
    if (TipToggleViaCom()) return;   // 进程在跑但面板收起了 → 显示
    EnsureHandwritingButton();
    wchar_t path[MAX_PATH * 2] = {0};
    if (!FindInkTip(path, MAX_PATH * 2)) ::lstrcpynW(path, L"TabTip.exe", MAX_PATH * 2);
    HINSTANCE r = ::ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        ::MessageBoxW(
            owner,
            L"未能启动系统手写面板。\n\n"
            L"本功能调用 Windows 10/11 自带的“触摸键盘”，其中包含手写输入。\n"
            L"Windows 7 标准版没有安装触摸键盘组件，暂不支持手写。",
            L"秋雨五笔", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    // 有些系统 ShellExecute 后窗口并未真正显示，等它起来再用 COM 顶一次
    ::Sleep(200);
    if (!TipWindowVisible()) TipToggleViaCom();
}

// 极简诊断日志：关键时刻写一行，量很小，记录在 %APPDATA%\QiuYuWubi\trace.log。
//
// 2026-11-04 按用户提供的 Win10 日志做的四项改进：
//   ① 时间戳带完整日期 —— 旧日志只有时分秒，跨天 + 多进程混合后分不清哪天；
//   ② 每个进程首行写 session 头（版本 / 架构 / 宿主 EXE / OS）—— 拿到日志才能
//      判断是哪个构建产生的；旧日志只能靠字符串格式反推版本，很不可靠；
//   ③ 超 256KB 时滚动成 trace.old.log，不再 DeleteFile 整份删掉 —— 排障时日志
//      刚好超限会被清空，用户拷来的往往是残缺记录；
//   ④ 打开时带 FILE_SHARE_WRITE —— 实测一份日志里有 49 个宿主进程并发写，
//      原来只有 FILE_SHARE_READ，别的进程 CreateFile 会失败，日志静默丢行。
void QyTrace(const wchar_t* fmt, ...) {
    wchar_t line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(line, 500, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    line[(n < 500) ? n : 500] = 0;

    const std::wstring dir = UserDir();
    const std::wstring path = dir + L"\\trace.log";
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow > 256u * 1024u) {
        const std::wstring old = dir + L"\\trace.old.log";
        ::MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    const unsigned long pid = (unsigned long)::GetCurrentProcessId();
    wchar_t stamp[96];
    _snwprintf(stamp, 95,
               L"[%04d-%02d-%02d %02d:%02d:%02d.%03d pid=%lu] ", st.wYear, st.wMonth,
               st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, pid);
    stamp[95] = 0;

    std::wstring all;
    static bool s_sessionLogged = false;
    if (!s_sessionLogged) {
        s_sessionLogged = true;
        wchar_t exe[MAX_PATH] = {0};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* slash = ::wcsrchr(exe, L'\\');
        const wchar_t* name = slash ? slash + 1 : exe;
        OSVERSIONINFOW oi{};
        oi.dwOSVersionInfoSize = sizeof(oi);
        // GetVersionExW 从 Win8.1 起受宿主 manifest 影响会固定报 6.2（实测在这台
        // Win10 上写成 os=6.2.9200），排障时会被误导。动态调 ntdll!RtlGetVersion
        // 取真实版本（Win7~Win11 都有）；取不到再回退 GetVersionExW。
        typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
        HMODULE hNt = ::LoadLibraryW(L"ntdll.dll");
        RtlGetVersionFn pRtlGetVersion =
            hNt ? (RtlGetVersionFn)(void*)::GetProcAddress(hNt, "RtlGetVersion") : nullptr;
        if (!pRtlGetVersion || pRtlGetVersion(&oi) != 0) ::GetVersionExW(&oi);
        if (hNt) ::FreeLibrary(hNt);
#if defined(_WIN64) || defined(__x86_64__)
        const wchar_t* arch = L"x64";
#else
        const wchar_t* arch = L"x86";
#endif
        wchar_t hdr[640];
        _snwprintf(hdr, 639,
                   L"[%04d-%02d-%02d %02d:%02d:%02d.%03d pid=%lu] === qywb %s %s host=%s os=%lu.%lu.%lu ===\r\n",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   st.wMilliseconds, pid, QY_VERSION_STR, arch, name,
                   (unsigned long)oi.dwMajorVersion, (unsigned long)oi.dwMinorVersion,
                   (unsigned long)oi.dwBuildNumber);
        hdr[639] = 0;
        all = hdr;
    }
    all += std::wstring(stamp) + line + L"\r\n";

    int cb = ::WideCharToMultiByte(CP_UTF8, 0, all.c_str(), (int)all.size(), nullptr, 0,
                                   nullptr, nullptr);
    if (cb <= 0) return;
    std::string bytes((size_t)cb, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, all.c_str(), (int)all.size(), &bytes[0], cb, nullptr,
                          nullptr);
    HANDLE hf = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    ::WriteFile(hf, bytes.data(), (DWORD)cb, &wrote, nullptr);
    ::CloseHandle(hf);
}

//------------------------------------------------------------------------------
// CEditSession
CEditSession::CEditSession(CTextService* svc, ITfContext* ctx) : _cRef(1), _svc(svc), _ctx(ctx) {
    if (_ctx) _ctx->AddRef();
    if (_svc) _svc->AddRef();
}

CEditSession::~CEditSession() {
    if (_ctx) _ctx->Release();
    if (_svc) _svc->Release();
}

STDMETHODIMP CEditSession::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
        *ppv = (ITfEditSession*)this;
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CEditSession::AddRef() { return InterlockedIncrement(&_cRef); }

STDMETHODIMP_(ULONG) CEditSession::Release() {
    LONG c = InterlockedDecrement(&_cRef);
    if (c == 0) delete this;
    return c;
}

STDMETHODIMP CEditSession::DoEditSession(TfEditCookie ec) { return _svc->Apply(ec, _ctx); }

//------------------------------------------------------------------------------
// CTextService
CTextService::CTextService() : _cRef(1) {}

CTextService::~CTextService() {}

STDMETHODIMP CTextService::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor)) {
        *ppv = (ITfTextInputProcessor*)this;
    } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
        *ppv = (ITfThreadMgrEventSink*)this;
    } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppv = (ITfKeyEventSink*)this;
    } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
        *ppv = (ITfCompositionSink*)this;
    } else if (IsEqualIID(riid, IID_ITfCompartmentEventSink)) {
        *ppv = (ITfCompartmentEventSink*)this;
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) CTextService::AddRef() { return InterlockedIncrement(&_cRef); }

STDMETHODIMP_(ULONG) CTextService::Release() {
    LONG c = InterlockedDecrement(&_cRef);
    if (c == 0) delete this;
    return c;
}

void CTextService::_LoadConfig() {
    Config cfg;
    cfg.Load();
    _opt = cfg.opt();
    std::wstring p = cfg.Path();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) _cfgTime = fad.ftLastWriteTime;
}

// 把配置里的状态条外观（DIY）应用到状态条
static void ApplyStatusSkin(qy::StatusBar& sb, const qy::Options& o) {
    qy::StatusSkin s;
    s.theme = o.sbTheme;
    s.shape = o.sbSkin;
    s.translucent = o.sbTranslucent;
    s.accent = (COLORREF)o.sbAccent;
    s.card = (COLORREF)o.sbCard;
    sb.SetSkin(s);
}

//------------------------------------------------------------------------------
// 状态条 / 方案切换
void CTextService::_UpdateStatus() {
    StatusInfo si;
    si.chinese = _chinese;
    // 状态条方案图标始终显示用户真实方案，临时拼音（z 引导）不改图标：
    // 临时拼音只在引擎内部一次性把候选按拼音查，输入结束在 Clear() 里复位，
    // 那条路径不经过这里。若图标跟着临时拼音切成「拼」，结束时就没人把它
    // 刷回「五」，会永久停在「拼」。所以这里一律读 _engine.mode()，
    // 方案保存、右键菜单勾选、config.ini 也都读真实 mode()。
    si.mode = _engine.mode();
    si.fullWidth = _opt.fullWidth != 0;
    si.symbols = _sym.visible();
    _status.Update(si);
}

// 把某项开关写回 config.ini，并刷新自身时间戳避免被 _CheckConfigReload 回滚
void CTextService::_SaveOption(int which) {
    Config cfg;
    cfg.Load();
    Options& o = cfg.opt();
    if (which == 0) o.fullWidth = _opt.fullWidth;
    if (which == 1) o.numpadSelect = _opt.numpadSelect;
    if (which == 2) o.statusBar = _opt.statusBar;
    if (which == 3) o.mode = _opt.mode;
    if (which == 4) o.manualFreq = _opt.manualFreq;
    if (which == 5) o.sbTranslucent = _opt.sbTranslucent;
    if (which == 6) o.handwriting = _opt.handwriting;
    if (which == 7) o.sbSkin = _opt.sbSkin;
    if (which == 8) o.layout = _opt.layout;
    cfg.Save();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(cfg.Path().c_str(), GetFileExInfoStandard, &fad))
        _cfgTime = fad.ftLastWriteTime;
}

void CTextService::_SyncStatusVisible() {
    _status.SetEnabled(_opt.statusBar != 0);
    if (_opt.statusBar) {
        // 刚勾选「显示状态条」时先记一次活跃心跳：代理窗口类程序里
        // 前台 PID 与本实例不同，没有这个心跳条会被 250ms 检查立刻收起。
        _status.NoteImeActivity();
        _status.Show();
    } else {
        _status.Hide();
        _sym.Hide();
    }
}

void CTextService::_SaveModeToDisk() {
    Config cfg;
    cfg.Load();
    cfg.opt().mode = ModeToInt(_engine.mode());
    cfg.Save();
    _opt.mode = cfg.opt().mode;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(cfg.Path().c_str(), GetFileExInfoStandard, &fad))
        _cfgTime = fad.ftLastWriteTime;
}

void CTextService::_SetMode(Mode m) {
    if (_engine.mode() == m) return;
    _engine.SetMode(m);
    _HideCandidate();
    if (_pLastContext) _Sync(_pLastContext);
    _SaveModeToDisk();
    _UpdateStatus();
}

void CTextService::_ToggleMode() {
    // 临时拼音期间点方案区=退出临时拼音、退回五笔，而不是把用户方案轮换成拼音
    // （图标此时仍显示真实方案「五」，但底层正处于一次性拼音态，要先清掉）
    if (_engine.tempPinyin()) {
        _engine.CancelTempPinyin();
        _HideCandidate();
        if (_pLastContext) _Sync(_pLastContext);
        _UpdateStatus();
        return;
    }
    _engine.ToggleMode();
    _HideCandidate();
    if (_pLastContext) _Sync(_pLastContext);
    _SaveModeToDisk();
    _UpdateStatus();
}

void CTextService::_ToggleWidth() {
    _opt.fullWidth = _opt.fullWidth ? 0 : 1;
    _engine.SetOptions(_opt);
    _SaveOption(0);
    _UpdateStatus();
}

void CTextService::_ToggleNumpad() {
    _opt.numpadSelect = _opt.numpadSelect ? 0 : 1;
    _SaveOption(1);
    _UpdateStatus();
}

void CTextService::_ToggleChinese() { _SetChinese(!_chinese, L"status-pill"); }

void CTextService::_OpenSettings() {
    // qydemo 为 GUI 程序（无控制台），以 ShellExecute 启动不会闪出命令行窗口
    std::wstring dir = ModuleDirOf(g_hInst);
    std::wstring exe = dir + L"\\qydemo.exe";
    if (::GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ::MessageBoxW(_status.hwnd(), L"未找到 qydemo.exe，请重新安装秋雨五笔输入法。",
                      L"秋雨五笔", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return;
    }
    ::ShellExecuteW(nullptr, L"open", exe.c_str(), L"--settings", dir.c_str(), SW_SHOWNORMAL);
}

// 点状态条齿轮图标 → 打开/关闭设置窗口（再次点击 = 关闭，形成切换）。
// 用 FindWindow 找“设置窗口”（类名 QyDemoMain + 标题精确匹配），命中就发
// WM_CLOSE 让它正常退出（qydemo 在 WM_DESTROY 里清 preview.pid、还原预览），
// 没命中才走 ShellExecute 启动新的。演示模式窗口标题不同，不会被误关。
void CTextService::_ToggleSettings() {
    HWND hw = ::FindWindowW(L"QyDemoMain", L"秋雨五笔输入法 - 设置");
    if (hw) {
        ::PostMessageW(hw, WM_CLOSE, 0, 0);
        return;
    }
    _OpenSettings();
}

// 点状态条笑脸图标 → 弹出「图标输入」符号面板（贴在状态条正上方）
void CTextService::_ToggleSymbols() {
    if (!_status.hwnd()) return;
    if (!_sym.hwnd()) {
        if (!_sym.Create(_hInst ? _hInst : g_hInst)) return;
        _sym.SetOwnerWindow(_status.hwnd());
    }
    // 记住打开面板时谁在前台：面板不抢焦点，但如果面板自己成了前台
    //（某些宿主会这样），点符号时还得知道该往哪个窗口输出
    HWND fg = ::GetForegroundWindow();
    if (fg && fg != _status.hwnd() && fg != _sym.hwnd()) _symTarget = fg;
    RECT wr{};
    ::GetWindowRect(_status.hwnd(), &wr);
    _sym.Toggle(POINT{wr.left, wr.top});
    _UpdateStatus();
}

void CTextService::_HideSymbols() {
    if (_sym.visible()) {
        _sym.Hide();
        _UpdateStatus();
    }
}

//------------------------------------------------------------------------------
// 新版本检测
//
// 只做“发现新版本”，绝不静默升级：命中新版后弹确认框，用户点“是”才
// 打开下载页，安装包仍要用户自己双击运行，等于双重手动确认。
// 网络读写放在后台线程，避免卡住宿主程序的界面线程；自动检测 12 小时
// 最多一次（时间存在配置里，重启程序也不会重复弹）。
//------------------------------------------------------------------------------
struct UpdateJob {
    std::wstring addr;
    bool manual = false;
};

// 打开增量更新程序：qyupd.exe 自己带界面、自己比对文件，只下载有变化的组件。
// 输入法本体不在这里做 HTTP 下载——宿主进程里做长耗时 I/O 会卡住用户正在
// 打字的窗口，而且提权也做不到。
static bool LaunchUpdaterIn(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::wstring exe = dir + L"\\qyupd.exe";
    if (::GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    HINSTANCE r =
        ::ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

bool CTextService::_LaunchUpdater() { return LaunchUpdaterIn(ModuleDirOf(g_hInst)); }

static DWORD WINAPI UpdateWorkerProc(LPVOID p) {
    UpdateJob* job = (UpdateJob*)p;
    if (!job) return 0;
    UpdateInfo info;
    std::wstring err;
    bool ok = FetchUpdateInfo(job->addr, &info, &err);
    bool manual = job->manual;
    // 用安装目录里的真实版本（installed.ver），不要用编译期版本：覆盖安装后
    // 本进程可能仍是旧组件，拿旧号比较会把「服务器上的旧版」误判成新版本。
    std::wstring cur = DiskVersion();

    if (!ok) {
        if (manual) {
            std::wstring msg = L"检查更新失败：\n" + err +
                               L"\n\n可以到设置里确认“更新地址”，或稍后再试。";
            ::MessageBoxW(nullptr, msg.c_str(), L"秋雨五笔", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        }
    } else if (CompareVersion(info.version, cur) > 0) {
        std::wstring msg = L"发现新版本 v" + info.version + L"（当前 v" + cur + L"）\n\n";
        if (!info.notes.empty()) msg += info.notes + L"\n\n";
        msg += L"是否现在更新？\n"
               L"增量更新只下载有变化的文件（通常几百 KB），不需要再下完整安装包；\n"
               L"设置、词频、自造词都会保留。";
        if (::MessageBoxW(nullptr, msg.c_str(), L"发现新版本",
                          MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST) == IDYES) {
            if (!LaunchUpdaterIn(ModuleDirOf(g_hInst)))
                OpenUrl(info.url.empty() ? job->addr : info.url);
        }
    } else if (manual) {
        std::wstring msg = L"当前已是最新版本（v" + cur + L"）。";
        ::MessageBoxW(nullptr, msg.c_str(), L"秋雨五笔",
                      MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }
    delete job;
    return 0;
}

void CTextService::_CheckUpdate(bool manual) {
    // 用户主动点“检查更新…”：直接叫起增量更新程序，检查、进度、写文件都在那边，
    // 输入法这边不下载任何东西（也不能在宿主进程里提权写 Program Files）
    if (manual && _LaunchUpdater()) return;

    std::wstring addr = _opt.updateUrl;
    if (addr.empty()) {
        if (manual) {
            ::MessageBoxW(
                _status.hwnd(),
                L"未配置更新地址。\n\n请在 %APPDATA%\\QiuYuWubi\\config.ini 的 [更新] 地址 里\n"
                L"填写 version.txt 的网址（或本地路径），再试一次。",
                L"检查更新", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        }
        return;
    }
    // 自动检测节流：12 小时内只查一次
    if (!manual) {
        long long now = (long long)::time(nullptr);
        if (_opt.lastUpdateCheck > 0 && now - _opt.lastUpdateCheck < 12 * 3600) return;
        if (_opt.autoCheckUpdate == 0) return;
    }

    // 先记录检测时间（失败也算查过，避免每次开新程序都重试）
    Config cfg;
    cfg.Load();
    cfg.opt().lastUpdateCheck = (long long)::time(nullptr);
    cfg.Save();
    _opt.lastUpdateCheck = cfg.opt().lastUpdateCheck;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(cfg.Path().c_str(), GetFileExInfoStandard, &fad))
        _cfgTime = fad.ftLastWriteTime;

    UpdateJob* job = new (std::nothrow) UpdateJob();
    if (!job) return;
    job->addr = addr;
    job->manual = manual;
    HANDLE th = ::CreateThread(nullptr, 0, UpdateWorkerProc, job, 0, nullptr);
    if (th) {
        ::CloseHandle(th);
    } else {
        delete job;
    }
}

// Office 系列（Word / Excel / PowerPoint / Outlook …）是“严格”的 TSF 宿主：
// 它不把注入的 Unicode 按键（SendInput 的 VK_PACKET）当文字收下，
// 所以在这些程序里点符号会“面板消失但没上屏”。它们必须走标准 TSF 提交。
// 微信等 Chromium 宿主则相反——用 TSF 改文档容易被它判为异常，
// 那些仍用 SendInput 通道（已经被实测验证过）。
// 自检开关：设 QY_FORCE_TSF_SYMBOL=1 时强制走 TSF 通道，
// 便于在非 Office 宿主里验证这条路径（生产环境不设就没有影响）
static bool ForceTsfForTest() {
    wchar_t b[8] = {0};
    DWORD n = ::GetEnvironmentVariableW(L"QY_FORCE_TSF_SYMBOL", b, 8);
    return n > 0 && b[0] == L'1';
}

static bool IsStrictTsfHost(HWND w) {
    if (!w) return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(w, &pid);
    if (!pid) return false;
    HANDLE hp = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return false;
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = MAX_PATH;
    bool strict = false;
    if (::QueryFullProcessImageNameW(hp, 0, buf, &n)) {
        const wchar_t* slash = ::wcsrchr(buf, L'\\');
        const wchar_t* exe = slash ? slash + 1 : buf;
        static const wchar_t* kExes[] = {
            L"WINWORD.EXE", L"EXCEL.EXE",   L"POWERPNT.EXE", L"OUTLOOK.EXE", L"ONENOTE.EXE",
            L"MSACCESS.EXE", L"MSPUB.EXE",  L"WINPROJ.EXE",  L"VISIO.EXE",
            // WPS 也是 TSF 宿主，同样不认注入的 Unicode 按键
            L"WPS.EXE",     L"ET.EXE",      L"WPP.EXE",      L"WPSSOFT.EXE"};
        for (auto e : kExes) {
            if (::lstrcmpiW(exe, e) == 0) {
                strict = true;
                break;
            }
        }
    }
    ::CloseHandle(hp);
    return strict;
}

// 点击面板里的符号：插入到当前光标处
//
// 两条通道：
//   1) Office 等严格 TSF 宿主：走标准提交流程（和打字上屏同一条路），
//      这样 Word 才能真正收到字符；
//   2) 其它宿主（微信 / Chromium / 记事本 …）：用 Unicode 按键事件
//      （SendInput KEYEVENTF_UNICODE）输出，完全不碰宿主的 COM 对象，
//      这是实测最稳的路径。
// 两条都在鼠标消息之外执行（面板把点击延后投递），不会重入宿主的输入处理。
void CTextService::_InsertSymbol(const std::wstring& s) {
    if (s.empty()) return;

    // 前台若是我们自己的状态条 / 符号面板，用“打开面板时记下的”前台窗口。
    // 状态条和面板本来就跑在宿主进程里，不能用进程号区分，只能比窗口句柄。
    HWND fg = ::GetForegroundWindow();
    if (!fg || fg == _status.hwnd() || fg == _sym.hwnd()) fg = _symTarget;
    if (!fg) {
        if (_engine.hasComp()) {
            _engine.Clear();
            _HideCandidate();
            if (_pLastContext) _Sync(_pLastContext);
        }
        return;
    }

    bool hadComp = _engine.hasComp() || _pComposition != nullptr;
    if (hadComp) {
        _engine.Clear();
        _HideCandidate();
    }

    if (IsStrictTsfHost(fg) || ForceTsfForTest()) {
        bool restored = false;
        HRESULT hr = _CommitSymbolTsf(s, hadComp, &restored);
        QyTrace(L"symbol via TSF hr=0x%08lx hadComp=%d refocus=%d", (unsigned long)hr,
                hadComp ? 1 : 0, restored ? 1 : 0);
        if (SUCCEEDED(hr)) return;
        QyTrace(L"symbol TSF failed -> SendInput fallback (strict host may drop it)");
    }

    if (hadComp && _pLastContext) _Sync(_pLastContext);  // 清组合：与打字同路径
    QyTrace(L"symbol via SendInput len=%d", (int)s.size());
    _SendUnicode(s);
}

// “上下文已断开”——宿主把 TSF 焦点丢掉之后再拿这个 context 做任何事都返回它
static const HRESULT kTfEDisconnected = (HRESULT)0x80040208L;

ITfContext* CTextService::_AcquireFocusContext() {
    if (!_pThreadMgr) return nullptr;
    ITfDocumentMgr* pdim = nullptr;
    ITfContext* pic = nullptr;
    if (SUCCEEDED(_pThreadMgr->GetFocus(&pdim)) && pdim) {
        if (FAILED(pdim->GetTop(&pic)) || !pic) pic = nullptr;
        pdim->Release();
    }
    return pic;
}

// 走 TSF 提交一个符号（含带编码时先抹掉编码）。
//
// MS Office 的坑：点它进程里我们自己的面板窗口，Office 会把 TSF 焦点丢掉
// （ITfThreadMgr::SetFocus(NULL)），此刻 GetFocus 拿到的上下文是“已断开”的，
// RequestEditSession 直接返回 TF_E_DISCONNECTED——表现为“面板关了、字没上”。
// 所以失败时用缓存的 document manager 把焦点接回同一个文档，再重试一次。
HRESULT CTextService::_CommitSymbolTsf(const std::wstring& s, bool hadComp, bool* restored) {
    if (restored) *restored = false;

    // 自检开关：强制先丢掉焦点，模拟 Office 点面板后的状态，验证接回逻辑
    static const bool kSimDrop = [] {
        wchar_t b[8] = {0};
        DWORD n = ::GetEnvironmentVariableW(L"QY_SYMBOL_SIM_DROP", b, 8);
        return n > 0 && b[0] == L'1';
    }();
    if (kSimDrop && _pThreadMgr) {
        _pThreadMgr->SetFocus(nullptr);
        QyTrace(L"symbol sim drop focus");
    }

    ITfContext* pic = _AcquireFocusContext();
    if (!pic && _pLastContext) {
        pic = _pLastContext;
        pic->AddRef();
    }
    if (!pic) return E_FAIL;

    _engine.QueueCommit(s);
    _cancelCompBeforeCommit = hadComp;  // 有未上屏的编码：先抹掉再插符号
    HRESULT hr = _Sync(pic);

    if (FAILED(hr) && _pDocMgr && _pThreadMgr) {
        HRESULT hrf = _pThreadMgr->SetFocus(_pDocMgr);
        // 焦点接回来之后直接从缓存的 document manager 取顶层上下文，
        // 比再问一次 GetFocus 稳（GetFocus 在宿主刚被切走时可能给不出东西）
        ITfContext* pic2 = nullptr;
        HRESULT hrt = _pDocMgr->GetTop(&pic2);
        if (FAILED(hrt) || !pic2) {
            pic2 = _AcquireFocusContext();
            hrt = pic2 ? S_OK : E_FAIL;
        }
        QyTrace(L"symbol refocus hr=0x%08lx gettop=0x%08lx (sync=0x%08lx disconnected=%d same=%d)",
                (unsigned long)hrf, (unsigned long)hrt, (unsigned long)hr,
                hr == kTfEDisconnected ? 1 : 0, pic2 == pic ? 1 : 0);
        if (SUCCEEDED(hrf) && pic2) {
            if (restored) *restored = true;
            // 关键修复：SetFocus 成功就再试一次 TSF 提交，不管 GetTop 给回来的是不是
            // 同一个 context 对象。
            //
            // 旧实现在 same=1（拿回来还是同一个对象）时直接放弃 TSF、改走 SendInput，
            // 理由是“旧上下文被宿主判死”。但 Win10 的 trace.log 实测 14 次 TSF
            // 提交里 13 次失败，且**全部**是 same=1：Office/WPS 点我们自己的面板后
            // 只是把文档焦点丢掉，SetFocus 一接回来，这个 context 立刻就能再开
            // 编辑会话；而这两个严格 TSF 宿主恰恰不收 SendInput 的 VK_PACKET，
            // 于是用户看到的就是「点了符号、面板关了、字没上屏」。
            //
            // 若第一次失败的会话其实跑过 Apply 并消费了队列，这里补回提交；
            // 否则保持原队列，绝不重复入队（否则会插出两个符号）。
            if (!_engine.HasPendingCommit()) _engine.QueueCommit(s);
            HRESULT hr2 = _Sync(pic2);
            QyTrace(L"symbol retry after refocus hr=0x%08lx same=%d", (unsigned long)hr2,
                    pic2 == pic ? 1 : 0);
            if (SUCCEEDED(hr2)) hr = hr2;
            pic2->Release();
        } else if (pic2) {
            pic2->Release();
        }
    }
    pic->Release();

    if (SUCCEEDED(hr)) return hr;
    // 失败就把这次提交撤掉，别让它留到下一次打字时冒出来
    _cancelCompBeforeCommit = false;
    std::wstring drop;
    _engine.TakeCommit(drop);
    return hr;
}

// 兜底通道：直接把字符以 Unicode 键事件发给前台窗口
void CTextService::_SendUnicode(const std::wstring& s) {
    for (wchar_t ch : s) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        ::SendInput(2, in, sizeof(INPUT));
    }
}

void CTextService::_ShowMenu(POINT pt) {
    HMENU m = ::CreatePopupMenu();
    if (!m) return;
    auto flags = [&](bool checked) -> UINT { return MF_STRING | (checked ? MF_CHECKED : 0); };
    ::AppendMenuW(m, flags(_chinese && _engine.mode() == Mode::Wubi), 1, L"五笔输入");
    ::AppendMenuW(m, flags(_chinese && _engine.mode() == Mode::Pinyin), 2, L"拼音输入");
    ::AppendMenuW(m, flags(_chinese && _engine.mode() == Mode::Mixed), 18, L"五笔拼音混输");
    ::AppendMenuW(m, flags(!_chinese), 3, L"英文输入");
    ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(m, flags(_opt.fullWidth != 0), 4, L"全角输出");
    ::AppendMenuW(m, flags(_opt.numpadSelect != 0), 5, L"小键盘数字用于选词");
    ::AppendMenuW(m, flags(_opt.layout != 0), 24, L"竖排候选");
    ::AppendMenuW(m, flags(_sym.visible()), 13, L"图标输入（符号 / 颜文字）");
    ::AppendMenuW(m, flags(_opt.shiftEnglish != 0), 6, L"按住 Shift 直接输入英文");
    ::AppendMenuW(m, flags(_opt.shiftToggle != 0), 14, L"单击 Shift 切换中英文");
    ::AppendMenuW(m, flags(_opt.ctrlSpace != 0), 10, L"Ctrl+空格 切换中英文");
    ::AppendMenuW(m, flags(_opt.manualFreq != 0), 11, L"Ctrl+数字 手动调频（第N个调到首位）");
    ::AppendMenuW(m, MF_STRING | (_engine.promoCount() ? 0 : MF_GRAYED), 12,
                  L"清除手动调频记录");
    // 手写面板是系统级唯一窗口：勾选状态直接按“面板此刻是否可见”判断。
    // 不能用本进程的 _opt.handwriting —— 每个宿主程序里都有一份独立的
    // TextService，A 窗口打开手写后切到 B，B 的内存副本还是旧值，
    // 会出现“面板明明开着、菜单却不勾选”，用户再点一次又走一遍开关流程。
    ::AppendMenuW(m, flags(TipWindowVisible()), 20, L"手写输入（系统手写面板）");
    ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(m, MF_STRING, 7, L"设置…");
    ::AppendMenuW(m, flags(_opt.statusBar != 0), 8, L"显示状态条");
    ::AppendMenuW(m, flags(_opt.sbTranslucent != 0), 19,
                  L"状态条半透明（鼠标靠近恢复实体）");
    // 状态条皮肤：三款互斥，用子菜单 + 勾选标出当前款。切换写回 config.ini，
    // 其它宿主进程的状态条由 _CheckConfigReload() 自动跟上。
    {
        HMENU sm = ::CreatePopupMenu();
        if (sm) {
            const int cur = (_opt.sbSkin >= 0 && _opt.sbSkin <= 2) ? _opt.sbSkin : 0;
            ::AppendMenuW(sm, flags(cur == 0), 21, L"默认皮肤（圆角 · 柔和阴影）");
            ::AppendMenuW(sm, flags(cur == 1), 22, L"经典皮肤（直角 · 黑色细描边 · 无阴影）");
            ::AppendMenuW(sm, flags(cur == 2), 23, L"彩框皮肤（胶囊 · 彩色线框 · 无阴影）");
            ::AppendMenuW(m, MF_POPUP | MF_STRING, (UINT_PTR)sm, L"状态条皮肤");
        }
    }
    ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(m, flags(_opt.autoCheckUpdate != 0), 16, L"自动检测新版本");
    ::AppendMenuW(m, MF_STRING, 15, L"检查更新…");
    {
        std::wstring about = L"关于 秋雨五笔 v" + DiskVersion();
        ::AppendMenuW(m, MF_STRING, 9, about.c_str());
    }

    ::SetForegroundWindow(_status.hwnd());
    int cmd = (int)::TrackPopupMenu(
        m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_NONOTIFY, pt.x, pt.y, 0,
        _status.hwnd(), nullptr);
    ::DestroyMenu(m);
    ::PostMessageW(_status.hwnd(), WM_NULL, 0, 0);
    if (cmd <= 0) return;

    switch (cmd) {
        case 1:
            _SetChinese(true, L"menu");
            _SetMode(Mode::Wubi);
            _UpdateStatus();
            break;
        case 2:
            _SetChinese(true, L"menu");
            _SetMode(Mode::Pinyin);
            _UpdateStatus();
            break;
        case 18:
            _SetChinese(true, L"menu");
            _SetMode(Mode::Mixed);
            _UpdateStatus();
            break;
        case 3:
            _SetChinese(false, L"menu");
            break;
        case 4:
            _ToggleWidth();
            break;
        case 5:
            _ToggleNumpad();
            break;
        case 24: {  // 竖排候选：与设置窗口「选择 → 竖排候选」共用 [界面] 竖排
            _opt.layout = _opt.layout ? 0 : 1;
            _SaveOption(8);
            _engine.SetOptions(_opt);
            // 当前候选窗按旧布局画过，先收起；下次输入时按新布局重显
            _HideCandidate();
            _UpdateStatus();
            break;
        }
        case 6: {
            _opt.shiftEnglish = _opt.shiftEnglish ? 0 : 1;
            Config cfg;
            cfg.Load();
            cfg.opt().shiftEnglish = _opt.shiftEnglish;
            cfg.Save();
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(cfg.Path().c_str(), GetFileExInfoStandard, &fad))
                _cfgTime = fad.ftLastWriteTime;
            break;
        }
        case 7:
            _OpenSettings();
            break;
        case 8:
            _opt.statusBar = _opt.statusBar ? 0 : 1;
            _SaveOption(2);
            _SyncStatusVisible();
            break;
        case 19:  // 状态条半透明 开/关
            _opt.sbTranslucent = _opt.sbTranslucent ? 0 : 1;
            _SaveOption(5);
            ApplyStatusSkin(_status, _opt);
            break;
        case 21:  // 状态条皮肤：默认（shape=0，圆角+阴影）
        case 22:  // 状态条皮肤：经典直角黑细描边（shape=1）
        case 23:  // 状态条皮肤：彩框胶囊彩色线框（shape=2）
            _opt.sbSkin = (cmd == 21) ? 0 : (cmd == 22 ? 1 : 2);
            _SaveOption(7);
            ApplyStatusSkin(_status, _opt);
            break;
        case 10: {
            _opt.ctrlSpace = _opt.ctrlSpace ? 0 : 1;
            Config cfg;
            cfg.Load();
            cfg.opt().ctrlSpace = _opt.ctrlSpace;
            cfg.Save();
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(cfg.Path().c_str(), GetFileExInfoStandard, &fad))
                _cfgTime = fad.ftLastWriteTime;
            _SyncPreservedKey();
            _UpdateStatus();
            break;
        }
        case 14: {
            _opt.shiftToggle = _opt.shiftToggle ? 0 : 1;
            Config cfg;
            cfg.Load();
            cfg.opt().shiftToggle = _opt.shiftToggle;
            cfg.Save();
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(cfg.Path().c_str(), GetFileExInfoStandard, &fad))
                _cfgTime = fad.ftLastWriteTime;
            _UpdateStatus();
            break;
        }
        case 11:
            _opt.manualFreq = _opt.manualFreq ? 0 : 1;
            _SaveOption(4);
            break;
        case 15:  // 检查更新（用户主动，会提示结果）
            _CheckUpdate(true);
            break;
        case 16: {  // 自动检测新版本 开/关
            _opt.autoCheckUpdate = _opt.autoCheckUpdate ? 0 : 1;
            Config cfg;
            cfg.Load();
            cfg.opt().autoCheckUpdate = _opt.autoCheckUpdate;
            cfg.Save();
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(cfg.Path().c_str(), GetFileExInfoStandard, &fad))
                _cfgTime = fad.ftLastWriteTime;
            break;
        }
        case 12:
            if (_engine.promoCount() &&
                ::MessageBoxW(_status.hwnd(),
                              L"确定清除全部手动调频记录吗？\n\n清除后，之前用 Ctrl+数字 调到首位的候选"
                              L"将恢复默认顺序。",
                              L"秋雨五笔", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES) {
                _engine.ClearPromotions();
                ::MessageBoxW(_status.hwnd(), L"手动调频记录已清除。", L"秋雨五笔",
                              MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
            }
            break;
        case 13:
            _ToggleSymbols();
            break;
        case 20: {  // 手写输入 开/关：以系统手写面板的实际可见状态为准
            // 面板显示中 → 本次点击就是“关闭”；未显示 → 打开。
            // 手动关掉面板（点面板自己的关闭钮）后，任何窗口再开菜单看到的
            // 都是不勾选，不会出现“要连点两次才能对上状态”的问题。
            const bool showing = TipWindowVisible();
            _opt.handwriting = showing ? 0 : 1;
            _SaveOption(6);
            ToggleSystemHandwriting(!showing, _status.hwnd());
            break;
        }
        case 9: {
            std::wstring about =
                L"秋雨五笔输入法 v" + DiskVersion() +
                L"\n\n三个方案：五笔输入 / 拼音输入 / 五笔拼音混输（右键菜单里选）\n"
                L"GBK 字符集全量支持\n\n"
                L"状态条：点左侧切中/英，点「五/拼/混」切方案，点「全/半」切全角，"
                L"点笑脸图标打开图标输入（符号 / 颜文字面板），点齿轮打开设置\n"
                L"按住 Shift 可直接输入英文，单击 Shift 切中/英，"
                L"Ctrl+空格 同样切换中/英（三项均可在设置里关闭）\n"
                L"五笔拼音混输是独立方案：忘了五笔码直接打拼音，"
                L"五笔输入、拼音输入两个方案本身不受影响\n"
                L"临时拼音：五笔状态下按 z 进入临时拼音，接着打全拼，"
                L"上屏 / 回车 / Esc 后自动回到五笔（z 开头音节再按一次 z）\n\n"
                L"新版本检测：右键菜单「检查更新…」打开增量更新程序，"
                L"只下载有变化的文件（通常几百 KB），不会自动下载或安装\n\n"
                L"官方主页：" + std::wstring(QY_HOME_URL) +
                L"\n五笔输入法发布页面（下载）：" + std::wstring(QY_SITE_URL) +
                StaleNote();
            ::MessageBoxW(_status.hwnd(), about.c_str(), L"关于", MB_OK | MB_ICONINFORMATION |
                                                                     MB_TOPMOST);
            break;
        }
        default:
            break;
    }
}

void CTextService::_CheckConfigReload() {
    DWORD now = ::GetTickCount();
    // 设置窗口在前台时用 150ms 的快节奏：改配色 / 改开关要能当场在状态条上看见。
    // 平时 700ms 足够（只要别的程序改了配置，打字前就能跟上）。
    const DWORD interval = IsSettingsWindowForeground(::GetForegroundWindow()) ? 150u : 700u;
    if (now - _cfgCheckTick < interval) return;
    _cfgCheckTick = now;
    Config cfg;
    std::wstring p = cfg.Path();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return;
    if (::CompareFileTime(&fad.ftLastWriteTime, &_cfgTime) == 0) return;
    _cfgTime = fad.ftLastWriteTime;
    cfg.Load();
    const int barBefore = _opt.statusBar;
    const int ctrlSpaceBefore = _opt.ctrlSpace;
    const int posXBefore = _opt.statusX, posYBefore = _opt.statusY;
    _opt = cfg.opt();
    _engine.SetOptions(_opt);
    Mode m = ModeFromInt(_opt.mode);
    if (m != _engine.mode()) _engine.SetMode(m);
    ApplyStatusSkin(_status, _opt);
    // 位置也要跟着走：每个后台程序各有一份状态条实例，位置存在自己内存里。
    // 只要有谁拖动过（config 里的 状态条X/Y 变了），其它实例必须同步，
    // 否则显示权换到某个后台进程（切程序、打开设置预览）时，状态条会
    // 瞬间跳回那个进程记住的旧坐标——用户看到的就是“状态条乱跳”
    if ((_opt.statusX != posXBefore || _opt.statusY != posYBefore) &&
        _opt.statusX >= 0 && _opt.statusY >= 0) {
        _status.SetPos(_opt.statusX, _opt.statusY);
        _status.SetSharedPos(_opt.statusX, _opt.statusY);  // 同步给读不到配置的宿主
    }
    // 只有“显示状态条”这个开关本身变化时才动显示状态。
    // 之前每次重载都调 _SyncStatusVisible() → Show()，而 Show() 会把别的进程里
    // 的状态条 SW_HIDE 掉；后台多个实例同时重载时就会互相顶掉，表现为
    // “在设置里改一下配色，状态条反而闪一下没了”
    if (_opt.statusBar != barBefore) _SyncStatusVisible();
    _UpdateStatus();
    // 设置窗口里改了 Ctrl+空格 / Shift 选项后，重新对齐保留键与系统状态
    _SyncPreservedKey();
    // 只有“Ctrl+空格”这个选项本身变了，才需要重新对齐一次中/英；
    // 其它配置变（配色、状态条位置、词库开关…）绝不能顺手动中/英 ——
    // 拖动状态条保存位置、打开设置窗口、改配色都会写 config，以前这里
    // 无条件 _AdoptKeyboardState()，于是“拖一下状态条 / 点一下设置，
    // 中英自己切了”，而且是随机出现。
    if (ctrlSpaceBefore != _opt.ctrlSpace && !_opt.ctrlSpace) _AdoptKeyboardState();
}

STDMETHODIMP CTextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    return _Activate(ptim, tid);
}

HRESULT CTextService::_Activate(ITfThreadMgr* ptim, TfClientId tid) {
    if (!ptim) return E_INVALIDARG;
    _pThreadMgr = ptim;
    _pThreadMgr->AddRef();
    _tid = tid;
    _hInst = g_hInst;

    std::wstring dir = ModuleDirOf(g_hInst);
    if (::GetFileAttributesW((dir + L"\\data\\qywb.dat").c_str()) != INVALID_FILE_ATTRIBUTES)
        _dataDir = dir + L"\\data";
    else
        _dataDir = dir;

    // 顺手清掉覆盖安装遗留的旧文件备份（旧 DLL 一退出就能删）
    PurgeBackupsAsync(dir);

    _LoadConfig();
    _engine.SetOptions(_opt);
    _engine.Init(_dataDir);

    // 方案（五笔 / 拼音）由内部状态决定，不再依赖系统语言配置——
    // 五笔、拼音合并为同一个输入法，通过状态条或菜单切换
    Mode m = ModeFromInt(_opt.mode);
    _engine.SetMode(m);

    if (!_cand.Create(g_hInst)) return E_FAIL;
    _cand.SetClickHandler([this](int idx) {
        if (idx < 0) return;
        if (_engine.SelectOnPage((size_t)idx)) {
            if (_pLastContext) _Sync(_pLastContext);
        }
    });

    // 状态条
    if (_status.Create(g_hInst)) {
        _status.SetClickHandler([this](int region) {
            switch (region) {
                case kCellSymbols:
                    _ToggleSymbols();
                    break;
                case kCellChinese:
                    _HideSymbols();
                    _ToggleChinese();
                    break;
                case kCellMode:
                    _HideSymbols();
                    _ToggleMode();
                    break;
                case kCellWidth:
                    _HideSymbols();
                    _ToggleWidth();
                    break;
                default:
                    _HideSymbols();
                    _ToggleSettings();
                    break;
            }
        });
        _status.SetMenuHandler([this](POINT pt) { _ShowMenu(pt); });
        _status.SetMoveHandler([this](int x, int y) {
            _opt.statusX = x;
            _opt.statusY = y;
            Config cfg;
            cfg.Load();
            cfg.opt().statusX = x;
            cfg.opt().statusY = y;
            cfg.Save();
            // 拖动后把新位置也写进跨进程共享内存（见上方说明），让所有宿主一致
            _status.SetSharedPos(x, y);
            // 自己刚写过盘：记下新时间戳，别让本进程 250ms 后再跑一次配置重载
            //（拖完状态条就“顺手”重载一次，正是各种状态莫名变化的来源）
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (::GetFileAttributesExW(cfg.Path().c_str(), GetFileExInfoStandard, &fad))
                _cfgTime = fad.ftLastWriteTime;
        });
        if (_opt.statusX >= 0 && _opt.statusY >= 0) {
            _status.SetPos(_opt.statusX, _opt.statusY);
            // 把存储位置也写进跨进程共享内存：explorer / 桌面这类读不到配置、
            // 跨完整性级别的宿主（GHO 精简系统常见）也能拿到同一个位置，
            // 否则前台一切换就"上下跳动"。
            _status.SetSharedPos(_opt.statusX, _opt.statusY);
        }
        ApplyStatusSkin(_status, _opt);
        _UpdateStatus();
        _status.SetEnabled(_opt.statusBar != 0);
        if (_opt.statusBar) _status.Show();
        // 状态条自己每 250ms 有一次前台检查，顺带在这里做两件事：
        // ① 静默跟上跨实例唯一的中/英真值（不受 config 时间戳影响，必须每次都跑）；
        // ② 配置热加载：设置窗口改配色/开关后不用等按键，条上立刻能看到结果。
        _status.SetTickHandler([this]() {
            _SyncChineseFromShared();
            _CheckConfigReload();
            // 这里绝不能持续续期活跃心跳：_foreground 默认 true，切到别的
            // 程序时 TSF 未必发 OnSetFocus(FALSE)，250ms 一续期心跳就永不过期，
            // 状态条会永远赖在屏幕上不消失。前台归属现在以「焦点控件所属
            // 进程」即时判定（见 StatusBar::CheckForeground），心跳只在拿不到
            // 焦点 PID 时兜底，且只由真实输入活动注入（按键 / 焦点回来 /
            // 刚勾选显示状态条），2 秒宽限一过自动失效。
            // Win7/8 上 IMM32 兼容层不把单独 Shift 回调给 OnTestKeyDown，shift-tap 走不通；
            // 用 250ms 轮询兜底（见 _PollWin7Shift），Win10 仍走 OnTestKeyDown 原生路径。
            if (IsPreWin10() && _opt.shiftToggle) _PollWin7Shift();
        });
    }

    // 图标输入（符号面板）：点击符号直接上屏，显示状态变化时刷新状态条高亮
    if (_sym.Create(g_hInst)) {
        _sym.SetOwnerWindow(_status.hwnd());
        _sym.SetPickHandler([this](const std::wstring& s) { _InsertSymbol(s); });
        _sym.SetVisibleHandler([this]() { _UpdateStatus(); });
    }

    // 必须在 _InitCompartmentSink() 之前打开：首次激活的 _AdoptKeyboardState()
    // 要优先读这个全局真值，而不是读已被系统/别的实例改过的隔间。
    if (!_sharedState.view) _sharedState = OpenChineseState();

    _InitThreadMgrEventSink();
    _InitKeyEventSink();
    _InitCompartmentSink();  // Ctrl+空格 走系统隔间，必须监听

    // 自动检测新版本：12 小时最多一次，后台线程，发现新版才弹确认框
    _CheckUpdate(false);
    return S_OK;
}

STDMETHODIMP CTextService::Deactivate() {
    _HideCandidate();
    _cand.Destroy();
    _sym.Destroy();
    _status.Hide();
    _status.Destroy();
    if (_pComposition) {
        _pComposition->Release();
        _pComposition = nullptr;
    }
    _UninitKeyEventSink();
    _UninitThreadMgrEventSink();
    _UninitCompartmentSink();
    // 每次切走 / 关闭输入法，都把跨实例唯一真值无条件复位为中文。
    // 用户要求「每次切换回来都必需是中文输入状态」：旧实现带
    // 「新窗口默认中文输入」开关，关掉它的用户切回来仍是英文；而且 Win10 上
    // Win+空格 切走时 TSF 未必调用 Deactivate，光靠这里还不够，另在激活与
    // 前台回来两条路径上强制（见 _InitCompartmentSink / OnSetFocus）。
    // 这里只写共享内存：不写隔间、不发广播，其它实例由 250ms tick 静默跟上，
    // 不触碰「收到 OnChange 绝不写回隔间」这条铁律。
    if (_sharedState.view) {
        const int shared = SharedChinese(_sharedState);
        QyTrace(L"deactivate: force shared chinese -> 1 (was=%d)", shared);
        SetSharedChinese(_sharedState, true);
    }
    _chinese = true;  // 同一实例被 TSF 复用时，别把旧英文带进下一次 Activate
    CloseChineseState(_sharedState);
    _engine.SaveUser();
    if (_pLastContext) {
        _pLastContext->Release();
        _pLastContext = nullptr;
    }
    if (_pDocMgr) {
        _pDocMgr->Release();
        _pDocMgr = nullptr;
    }
    if (_pThreadMgr) {
        _pThreadMgr->Release();
        _pThreadMgr = nullptr;
    }
    return S_OK;
}

BOOL CTextService::_InitThreadMgrEventSink() {
    if (!_pThreadMgr) return FALSE;
    ITfSource* pSource = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSource)) || !pSource) return FALSE;
    HRESULT hr = pSource->AdviseSink(IID_ITfThreadMgrEventSink, (ITfThreadMgrEventSink*)this,
                                     &_dwThreadMgrSinkCookie);
    pSource->Release();
    return SUCCEEDED(hr);
}

void CTextService::_UninitThreadMgrEventSink() {
    if (!_pThreadMgr || _dwThreadMgrSinkCookie == TF_INVALID_COOKIE) return;
    ITfSource* pSource = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, (void**)&pSource)) && pSource) {
        pSource->UnadviseSink(_dwThreadMgrSinkCookie);
        pSource->Release();
    }
    _dwThreadMgrSinkCookie = TF_INVALID_COOKIE;
}

BOOL CTextService::_InitKeyEventSink() {
    if (!_pThreadMgr) return FALSE;
    ITfKeystrokeMgr* pKeyMgr = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeyMgr)) || !pKeyMgr)
        return FALSE;
    HRESULT hr = pKeyMgr->AdviseKeyEventSink(_tid, (ITfKeyEventSink*)this, TRUE);
    pKeyMgr->Release();
    _SyncPreservedKey();
    return SUCCEEDED(hr);
}

// Ctrl+空格 在系统里注册成了“输入法开/关”热键，按下时按键回调根本收不到。
// 用 ITfKeystrokeMgr::PreserveKey 把它声明成本输入法的保留键，
// 由系统把组合直接转给 OnPreservedKey，绕开那条热键。
void CTextService::_SyncPreservedKey() {
    if (!_pThreadMgr) return;
    ITfKeystrokeMgr* pKeyMgr = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeyMgr)) || !pKeyMgr)
        return;
    TF_PRESERVEDKEY pk{};
    pk.uVKey = VK_SPACE;
    pk.uModifiers = TF_MOD_CONTROL;
    if (_opt.ctrlSpace) {
        BOOL have = FALSE;
        if (SUCCEEDED(pKeyMgr->IsPreservedKey(GUID_QYKeyCtrlSpace, &pk, &have)) && have) {
            _preservedKeyOk = true;
        } else {
            const wchar_t* desc = L"秋雨五笔 中英文切换";
            HRESULT hr = pKeyMgr->PreserveKey(_tid, GUID_QYKeyCtrlSpace, &pk, desc,
                                              (ULONG)wcslen(desc));
            _preservedKeyOk = SUCCEEDED(hr);
            // Win7 上系统常已占用 Ctrl+空格，注册会失败；记清楚 hr，实机排障时
            // 一眼能看出该机究竟走的是 preserved key 还是隔间跟随。
            QyTrace(L"preserve Ctrl+Space hr=0x%08lx ok=%d pre10=%d",
                    (unsigned long)hr, _preservedKeyOk ? 1 : 0, IsPreWin10() ? 1 : 0);
        }
    } else {
        pKeyMgr->UnpreserveKey(GUID_QYKeyCtrlSpace, &pk);
        _preservedKeyOk = false;
        QyTrace(L"unpreserve Ctrl+Space");
    }
    pKeyMgr->Release();
}

void CTextService::_UninitKeyEventSink() {
    if (!_pThreadMgr) return;
    ITfKeystrokeMgr* pKeyMgr = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr, (void**)&pKeyMgr)) && pKeyMgr) {
        pKeyMgr->UnadviseKeyEventSink(_tid);
        pKeyMgr->Release();
    }
}

// 两次"跟随隔间切换"之间的最小间隔（毫秒）。GHO 精简系统上
// GetAsyncKeyState(VK_CONTROL) 可能因键盘钩子 / 远程会话残留恒为真，
// 没有冷却就会把系统重置当热键反复跟随。
static const DWORD kCompFollowCooldown = 300;

//------------------------------------------------------------------------------
// 键盘开/关（中/英）隔间：Ctrl+空格 由系统热键处理，只会改动这个隔间的值，
// 所以保留监听作为兜底（另有 key sink / preserved key 两条路径）。
// 注意：这里只读、必要时跟随，**绝不写回**（见 OnChange 注释）。
BOOL CTextService::_InitCompartmentSink() {
    if (!_pThreadMgr) return FALSE;
    ITfCompartmentMgr* pMgr = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfCompartmentMgr, (void**)&pMgr)) || !pMgr)
        return FALSE;
    bool ok = false;
    if (SUCCEEDED(pMgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &_pOpenClose)) &&
        _pOpenClose) {
        ITfSource* pSource = nullptr;
        if (SUCCEEDED(_pOpenClose->QueryInterface(IID_ITfSource, (void**)&pSource)) && pSource) {
            ok = SUCCEEDED(pSource->AdviseSink(IID_ITfCompartmentEventSink,
                                               (ITfCompartmentEventSink*)this, &_dwCompSinkCookie));
            pSource->Release();
        }
    }
    pMgr->Release();
    _activateTick = ::GetTickCount();
    if (!_chineseInited) {
        // 本程序第一次激活：先按既有逻辑校正一次（登记共享真值 / 对齐隔间），
        // 随后统一强制中文。
        _AdoptKeyboardState();
        // 首次激活即视为中/英已确定：避免「用户从没成功切过→inited 恒 false→
        // Win7 上隔间跟随被 DecideCompartmentChange 的 inited 条件卡死」的死循环
        //（Win7 走隔间跟随这唯一通路，inited 不该成为门槛）。
        _chineseInited = true;
    } else {
        // 再次激活（切窗口、弹/关设置窗、输入法切回来）：先把内存里的旧英文
        // 对齐共享真值，避免它随后又被写回隔间。
        _SyncChineseFromShared();
    }
    // 每次激活一律回到中文（用户要求「每次切换回来都必需是中文输入状态」）。
    // 只在激活路径写隔间，OnChange 回调里仍然绝不写回，乒乓铁律不变；
    // _SetChinese 幂等：已是中文时 _WriteKeyboardOpen 不会重复广播。
    _SetChinese(true, L"activate-force");
    QyTrace(L"comp sink init ok=%d chinese=%d inited=%d os=%lu pre10=%d pk=%d",
            ok ? 1 : 0, _chinese ? 1 : 0, _chineseInited ? 1 : 0,
            RealOsMajorVersion(), IsPreWin10() ? 1 : 0, _preservedKeyOk ? 1 : 0);
    return ok;
}

void CTextService::_UninitCompartmentSink() {
    if (_pOpenClose) {
        if (_dwCompSinkCookie != TF_INVALID_COOKIE) {
            ITfSource* pSource = nullptr;
            if (SUCCEEDED(_pOpenClose->QueryInterface(IID_ITfSource, (void**)&pSource)) && pSource) {
                pSource->UnadviseSink(_dwCompSinkCookie);
                pSource->Release();
            }
            _dwCompSinkCookie = TF_INVALID_COOKIE;
        }
        _pOpenClose->Release();
        _pOpenClose = nullptr;
    }
}

// 读系统“输入法是否开启”：true=中文（开） false=英文（关）
bool CTextService::_ReadKeyboardOpen(int* out) const {
    if (!_pOpenClose || !out) return false;
    VARIANT v;
    ::VariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(_pOpenClose->GetValue(&v))) {
        if (v.vt == VT_I4) {
            *out = v.lVal;
            ok = true;
        } else if (v.vt == VT_BOOL) {
            *out = v.boolVal ? 1 : 0;
            ok = true;
        }
    }
    ::VariantClear(&v);
    return ok;
}

void CTextService::_WriteKeyboardOpen(bool open) {
    if (!_pOpenClose) return;
    // 幂等：隔间已经是目标值就别再 SetValue。TSF 每次 SetValue 都会广播给
    // 其它实例；GHO 精简系统里激活 / 配置轮询很频繁，重复写同一个值等于
    // 白白制造 OnChange 风暴，还会把跨实例乒乓重新点起来。
    int cur = -1;
    if (_ReadKeyboardOpen(&cur) && cur == (open ? 1 : 0)) return;
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = open ? 1 : 0;
    _compGuard = true;  // 自己写的值不回环触发 OnChange
    _pOpenClose->SetValue(_tid, &v);
    _compGuard = false;
    ::VariantClear(&v);
}

// 按系统当前状态校正本实例的 _chinese（只在程序第一次激活 / 关掉 Ctrl+空格 后用）
void CTextService::_AdoptKeyboardState() {
    _chineseInited = true;
    // 全局真值优先：新开的宿主进程不该从"刚被系统 / 别的实例改过"的隔间里
    // 继承一个反向状态 —— 那正是各实例中英不一致、进而互相乒乓的起点。
    const int shared = SharedChinese(_sharedState);
    if (shared != 0) {
        const bool chinese = (shared == 1);
        if (chinese != _chinese) _SetChinese(chinese, L"shared-adopt");
        else _WriteKeyboardOpen(_chinese);  // 幂等：顺手对齐任务栏指示
        return;
    }
    SetSharedChinese(_sharedState, _chinese);  // 首个实例：登记全局真值
    int open = 1;
    if (!_ReadKeyboardOpen(&open)) {
        // 隔间还没有值：我们作为中文输入法，默认置为“开启”，
        // 这样之后 Ctrl+空格 才有 开→关 的变化可切
        _WriteKeyboardOpen(_chinese);
        return;
    }
    bool chinese = open != 0;
    if (!chinese && _opt.startChinese) {
        // 新窗口 / 新程序里，系统这个隔间常常还是“关”（英文）——中文输入法
        // 打开一个新窗口却要按一次 Shift 才能打字，很容易被当成坏了。
        // 默认按中文来，同时把隔间写回“开”，否则任务栏的输入法图标还显示英文。
        QyTrace(L"comp adopt: force chinese (was open=0)");
        _SetChinese(true, L"adopt");
        return;
    }
    if (chinese != _chinese) _SetChinese(chinese, L"adopt");
}

STDMETHODIMP CTextService::OnChange(REFGUID rguid) {
    if (_compGuard) return S_OK;
    if (!IsEqualGUID(rguid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) return S_OK;
    int open = 1;
    if (!_ReadKeyboardOpen(&open)) return S_OK;
    const bool chinese = open != 0;
    if (chinese == _chinese) return S_OK;

    const DWORD nowTick = ::GetTickCount();
    const bool ctrlDown = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    // 按键证据有两个来源，都在 OnTestKeyDown 里记（那个回调不受"吃键"影响）：
    //   ① 400ms 内按过 Ctrl；② 400ms 内看到过 Ctrl+空格 组合。
    const bool keyJustPressed = nowTick - _modKeyTick < 400 ||
                                nowTick - _ctrlSpaceTick < 400;
    const bool withinCooldown = nowTick - _lastCompFollowTick < kCompFollowCooldown;
    // Win7/8 上 Ctrl+空格 是系统级输入法热键，整套按键事件根本到不了
    // ITfKeyEventSink，PreserveKey 也常被系统占用 —— 隔间跟随是唯一通路，
    // 不能要求「按着 Ctrl」这种按键证据。Win7 的 TSF 不会在焦点切换时乱重置
    // 隔间（那是 Win10 才有的噪声），放宽不会造成「点开设置中英自己翻」。
    const bool preWin10 = IsPreWin10();
    const DWORD graceMs = preWin10 ? 150u : 600u;
    // Win7/8 上 Ctrl+空格 是系统级输入法热键，PreserveKey 又常被占用，隔间跟随是
    // 中/英切换的唯一通路；且 Win7 不会像 Win10 那样乱重置隔间，所以不要求
    // inited 当门槛（否则「用户从没成功切过→inited 恒 false→跟随被禁」死循环）。
    const bool initedForFollow = preWin10 ? true : _chineseInited;
    const CompDecision d = DecideCompartmentChange(
        initedForFollow, nowTick - _activateTick < graceMs, ctrlDown, keyJustPressed,
        _opt.ctrlSpace != 0, withinCooldown, preWin10);

    // 事件风暴时限流记日志：GHO 上每秒可能来几百次，逐条 QyTrace 会拖垮宿主
    //（QyTrace 每条都要开文件、查大小、拼时间戳）。
    if (nowTick - _lastCompLogTick >= 1000) {
        _lastCompLogTick = nowTick;
        QyTrace(L"comp OnChange open=%d me=%d ctrl=%d key=%d pre10=%d inited=%d follow=%d writeBack=0",
                open, _chinese ? 1 : 0, ctrlDown ? 1 : 0, keyJustPressed ? 1 : 0,
                preWin10 ? 1 : 0, _chineseInited ? 1 : 0, d.follow ? 1 : 0);
    }

    // ★ 2026-11-05 修掉的"中/英无限循环"根因：这里**绝不写回隔间**。
    // 旧实现在噪声分支里 _WriteKeyboardOpen(_chinese) 把隔间"纠正"回自己的值，
    // 但 _chinese 是每个 CTextService 实例各存一份、从不共享的：点开设置窗口，
    // 焦点切换让系统重置隔间，A 实例写「中」、B 实例写「英」，每次写又给对方
    // 发一次 OnChange → A↔B 无限乒乓，状态条上就是「中」「英」无休止乱跳。
    // _compGuard 只挡本实例自己的写，挡不住别的实例 / 别的 UI 线程。
    // 现在噪声一律不理会；隔间（任务栏指示）错了也无所谓，状态条以 _chinese
    // 为准，等用户下次主动切换或重新激活时由幂等写入对齐一次即可。
    // 代价同前：任务栏的中英按钮不生效，本输入法有胶囊 / Shift / Ctrl+空格 /
    // 右键菜单四种切法。
    if (!d.follow) return S_OK;

    _lastCompFollowTick = nowTick;
    QyTrace(L"comp OnChange follow open=%d", open);
    _SetChinese(chinese, L"compartment");
    return S_OK;
}

// 统一切换中/英：同时把系统隔间状态写成一致，否则下一次 Ctrl+空格 不会产生变化
void CTextService::_SetChinese(bool chinese, const wchar_t* why) {
    // trace.log 里记下“谁改的”：排查“没按键却中英自己切了”时一眼就能定位
    QyTrace(L"set chinese=%d by %s", chinese ? 1 : 0, why ? why : L"?");
    _chinese = chinese;
    _chineseInited = true;  // 用户/程序已经明确要过这个状态，之后别再被系统默认值覆盖
    // 写进跨实例唯一真值：其它宿主进程的状态条会在 250ms tick 里静默跟上，
    // 屏幕上就不会出现一份「中」一份「英」的错乱画面。
    SetSharedChinese(_sharedState, chinese);
    if (!_chinese) {
        _engine.Clear();
        _HideCandidate();
        if (_pLastContext) _Sync(_pLastContext);
    }
    _WriteKeyboardOpen(_chinese);
    _UpdateStatus();
}

// 250ms tick：把本实例的中/英静默对齐到跨实例唯一真值。
// 只改 _chinese + 刷新状态条，**绝不写隔间、绝不写共享**，否则又会变成
// 一次广播，把刚掐断的乒乓回路重新点起来。
void CTextService::_SyncChineseFromShared() {
    if (!_sharedState.view) return;
    const int v = SharedChinese(_sharedState);
    if (v == 0) {  // 还没有全局真值：以本实例为准登记一份
        SetSharedChinese(_sharedState, _chinese);
        return;
    }
    const bool chinese = (v == 1);
    if (chinese == _chinese) return;
    QyTrace(L"chinese sync from shared -> %d (was=%d)", chinese ? 1 : 0, _chinese ? 1 : 0);
    _chinese = chinese;
    _chineseInited = true;
    if (!_chinese) {
        _engine.Clear();
        _HideCandidate();
        if (_pLastContext) _Sync(_pLastContext);
    }
    _UpdateStatus();
}

//------------------------------------------------------------------------------
// Win7/8 专用：250ms 轮询检测 Shift 的按下/抬起边沿，实现「轻按 Shift 切中/英」。
// 背景：Win7 的 IMM32 兼容层不会把「单独的 Shift」这类修饰键回调给 OnTestKeyDown，
// 所以 Win10 那条 shift-tap 路径在 Win7 上收不到事件。这里用 GetAsyncKeyState 轮询
// 而不拦截按键——Shift 打大写/符号完全不受影响，只在「轻按」（down→up <800ms 且期间
// 没用 Shift 做修饰）时切中/英。多实例都通过本进程状态条 tick 调用，用全局去重时间戳
// 避免同一轻按被重复切。
void CTextService::_PollWin7Shift() {
    HWND fg = ::GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) ::GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid != ::GetCurrentProcessId()) { _shiftPollDownTick = 0; return; }  // 非本进程前台，不处理
    const bool shift = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (shift) {
        if (!_shiftPollDownTick) {
            _shiftPollDownTick = ::GetTickCount();
            _shiftPollUsed = false;
        }
    } else if (_shiftPollDownTick) {
        const DWORD dt = ::GetTickCount() - _shiftPollDownTick;
        _shiftPollDownTick = 0;
        if (_opt.shiftToggle && dt < 800 && !_shiftPollUsed && !_engine.hasComp()) {
            const DWORD now = ::GetTickCount();
            if (now - g_lastShiftToggleTick >= 400) {  // 多实例 / 重复触发去重
                g_lastShiftToggleTick = now;
                QyTrace(L"shift tap(win7 poll) -> %s", _chinese ? L"english" : L"chinese");
                _SetChinese(!_chinese, L"shift-tap(win7)");
            }
        }
    }
}

//------------------------------------------------------------------------------
// 编辑会话应用
HRESULT CTextService::Apply(TfEditCookie ec, ITfContext* pic) {
    if (!pic) return E_INVALIDARG;
    std::wstring commit;
    bool hasCommit = _engine.TakeCommit(commit);
    if (hasCommit) {
        if (_cancelCompBeforeCommit) {  // 插入符号前，把未上屏的编码字母抹掉
            _cancelCompBeforeCommit = false;
            _CancelComposition(ec, pic);
        }

        // 首选：把组合范围内的编码原位替换成上屏文本（TSF 标准提交流程）。
        // 之前的做法是“结束组合 + 在光标处另插一次”，在 MS Word 这类严格
        // TSF 宿主里会出大问题：结束组合后 Word 保留组合范围内的编码字母，
        // 而且光标回到组合开头，于是中文插到了编码左边——表现为
        // “测试 imy 秋雨 tof”、光标越打越往左。原位替换则不存在这两个问题。
        bool replaced = false;
        if (!commit.empty() && _pComposition) {
            ITfRange* pRange = nullptr;
            if (SUCCEEDED(_pComposition->GetRange(&pRange)) && pRange) {
                if (SUCCEEDED(pRange->SetText(ec, 0, commit.c_str(),
                                              (LONG)commit.size()))) {
                    _SetAttribute(ec, pic, pRange, true);
                    // 光标直接落到刚上屏的文本末尾，接着打字不会往回跳
                    pRange->Collapse(ec, TF_ANCHOR_END);
                    TF_SELECTION sel{};
                    sel.range = pRange;
                    sel.style.ase = (TfActiveSelEnd)0;  // TS_AE_NONE（zig 头文件缺这个常量）
                    sel.style.fInterimChar = FALSE;
                    pic->SetSelection(ec, 1, &sel);
                    replaced = true;
                }
                pRange->Release();
            }
        }

        _EndComposition(ec, pic);

        if (!replaced && !commit.empty()) {
            // 没有组合（英文直出、设置窗口等场景）：在光标处直接插入
            ITfInsertAtSelection* pias = nullptr;
            if (SUCCEEDED(pic->QueryInterface(IID_ITfInsertAtSelection, (void**)&pias)) &&
                pias) {
                ITfRange* pRange = nullptr;
                if (SUCCEEDED(pias->InsertTextAtSelection(ec, 0, commit.c_str(),
                                                          (ULONG)commit.size(), &pRange)) &&
                    pRange) {
                    _SetAttribute(ec, pic, pRange, true);
                    // 显式把光标放到刚插入文本末尾：部分宿主（Office / 记事本）
                    // 不会自动移动 selection，会留在插入点左端，导致“光标左移”
                    pRange->Collapse(ec, TF_ANCHOR_END);
                    TF_SELECTION sel{};
                    sel.range = pRange;
                    sel.style.ase = (TfActiveSelEnd)0;
                    sel.style.fInterimChar = FALSE;
                    pic->SetSelection(ec, 1, &sel);
                    pRange->Release();
                }
                pias->Release();
            }
        }
    }
    if (_engine.hasComp()) {
        _UpdateComposition(ec, pic);
    } else if (_pComposition && !hasCommit) {
        // 回车清屏 / Esc：编码已被清空且无内容上屏，
        // 必须把组合范围内的字母一并抹掉，否则“abc”会残留在文档里
        _CancelComposition(ec, pic);
    } else {
        _EndComposition(ec, pic);
    }

    _ShowCandidate(pic, ec);
    return S_OK;
}

// 取消组合：清空组合范围内的全部文本，再结束组合
HRESULT CTextService::_CancelComposition(TfEditCookie ec, ITfContext* pic) {
    if (!_pComposition) return S_OK;
    ITfRange* pRange = nullptr;
    if (SUCCEEDED(_pComposition->GetRange(&pRange)) && pRange) {
        // ★ 顺序不能反（2026-11-06 修复「回车清屏全局失效」）：
        //   SetText 必须用「尚未折叠」的完整组合范围，才会把编码字母删掉。
        //   以前先 Collapse(TF_ANCHOR_END) 把 range 折成一个空插入点，再
        //   SetText(L"") 等于往空范围写空串 —— 一个字符都没删，结束组合后
        //   "abc" 就当成正式文本留在文档里，用户看到的就是回车清屏 / Esc
        //   完全不起作用。所有宿主都走这条路径，所以是「全局失效」。
        //
        // 光标的修复不能靠提前 Collapse：TSF 的 SetText 会把 range 自动收缩到
        // 写入文本的末尾（这里写空串 → 收缩到组合起点）。所以先删除，再在
        // 收缩后的 range 上折叠到末尾并显式 SetSelection，既删掉了字母，
        // 又保住了「光标不往左跑」的老修复（Office / 记事本点符号时明显）。
        pRange->SetText(ec, 0, L"", 0);
        pRange->Collapse(ec, TF_ANCHOR_END);
        TF_SELECTION sel{};
        sel.range = pRange;
        sel.style.ase = (TfActiveSelEnd)0;  // TS_AE_NONE（zig 头文件缺该常量）
        sel.style.fInterimChar = FALSE;
        pic->SetSelection(ec, 1, &sel);
        pRange->Release();
    }
    return _EndComposition(ec, pic);
}

HRESULT CTextService::_UpdateComposition(TfEditCookie ec, ITfContext* pic) {
    const std::wstring& text = _engine.bufferW();
    if (text.empty()) return S_OK;

    if (!_pComposition) {
        ITfInsertAtSelection* pias = nullptr;
        if (FAILED(pic->QueryInterface(IID_ITfInsertAtSelection, (void**)&pias)) || !pias)
            return E_FAIL;
        ITfRange* pRange = nullptr;
        HRESULT hr = pias->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &pRange);
        pias->Release();
        if (FAILED(hr) || !pRange) return FAILED(hr) ? hr : E_FAIL;

        ITfContextComposition* pcc = nullptr;
        if (FAILED(pic->QueryInterface(IID_ITfContextComposition, (void**)&pcc)) || !pcc) {
            pRange->Release();
            return E_FAIL;
        }
        hr = pcc->StartComposition(ec, pRange, (ITfCompositionSink*)this, &_pComposition);
        pcc->Release();
        pRange->Release();
        if (FAILED(hr) || !_pComposition) return FAILED(hr) ? hr : E_FAIL;
    }

    ITfRange* pRange = nullptr;
    if (FAILED(_pComposition->GetRange(&pRange)) || !pRange) return E_FAIL;
    HRESULT hr = pRange->SetText(ec, 0, text.c_str(), (ULONG)text.size());
    if (SUCCEEDED(hr)) _SetAttribute(ec, pic, pRange, false);
    pRange->Release();
    return hr;
}

HRESULT CTextService::_EndComposition(TfEditCookie ec, ITfContext* pic) {
    if (!_pComposition) return S_OK;
    _endingComposition = true;
    ITfRange* pRange = nullptr;
    if (SUCCEEDED(_pComposition->GetRange(&pRange)) && pRange) {
        _SetAttribute(ec, pic, pRange, true);
        pRange->Release();
    }
    _pComposition->EndComposition(ec);
    _pComposition->Release();
    _pComposition = nullptr;
    _endingComposition = false;
    return S_OK;
}

HRESULT CTextService::_SetAttribute(TfEditCookie ec, ITfContext* pic, ITfRange* pRange, bool clear) {
    ITfProperty* pProp = nullptr;
    if (FAILED(pic->GetProperty(GUID_PROP_ATTRIBUTE, &pProp)) || !pProp) return E_FAIL;
    HRESULT hr = S_OK;
    if (clear) hr = pProp->Clear(ec, pRange);
    pProp->Release();
    return hr;
}

HRESULT CTextService::_Sync(ITfContext* pic) {
    if (!pic) return E_FAIL;
    CEditSession* pSession = new (std::nothrow) CEditSession(this, pic);
    if (!pSession) return E_OUTOFMEMORY;
    HRESULT hrSession = S_OK;
    HRESULT hr = pic->RequestEditSession(_tid, pSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    pSession->Release();
    if (FAILED(hr)) return hr;
    return hrSession;
}

void CTextService::_HideCandidate() { _cand.Hide(); }

void CTextService::_ShowCandidate(ITfContext* pic, TfEditCookie ec) {
    if (!_engine.hasComp() || _engine.cands().empty()) {
        _HideCandidate();
        return;
    }
    POINT pt{0, 0};
    ITfContextView* pView = nullptr;
    if (SUCCEEDED(pic->GetActiveView(&pView)) && pView) {
        ITfRange* pRange = nullptr;
        if (_pComposition) _pComposition->GetRange(&pRange);
        if (!pRange) {
            TF_SELECTION sel{};
            ULONG fetched = 0;
            if (SUCCEEDED(pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) &&
                fetched == 1)
                pRange = sel.range;
        }
        if (pRange) {
            RECT rc{0, 0, 0, 0};
            BOOL clipped = FALSE;
            if (SUCCEEDED(pView->GetTextExt(ec, pRange, &rc, &clipped))) {
                pt.x = rc.left;
                pt.y = rc.bottom + 2;
            }
            pRange->Release();
        }
        pView->Release();
    }
    if (pt.x == 0 && pt.y == 0) {
        POINT cur;
        if (::GetCursorPos(&cur)) {
            pt.x = cur.x;
            pt.y = cur.y + 8;
        }
    }
    CandShow cs;
    cs.items = &_engine.cands();
    cs.page = _engine.pageNo();
    cs.pageSize = (size_t)_opt.pageSize;
    cs.pt = pt;
    cs.fontSize = _opt.fontSize;
    cs.showCode = _opt.showCode != 0;
    cs.horizontal = _opt.layout == 0;
    cs.showTip = _opt.showCandTip != 0;
    cs.sel = _sel;
    _cand.Show(cs);
}

void CTextService::_CommitNow(const std::wstring& text) {
    _engine.QueueCommit(text);
}

//------------------------------------------------------------------------------
// 线程管理器事件
STDMETHODIMP CTextService::OnInitDocumentMgr(ITfDocumentMgr* pdim) { (void)pdim; return S_OK; }

STDMETHODIMP CTextService::OnUninitDocumentMgr(ITfDocumentMgr* pdim) {
    // 文档关掉了：把缓存的那份引用放掉，别拖着宿主的内存
    if (pdim && pdim == _pDocMgr) {
        _pDocMgr->Release();
        _pDocMgr = nullptr;
    }
    return S_OK;
}

STDMETHODIMP CTextService::OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) {
    (void)pdimPrevFocus;
    if (!pdimFocus) {
        // 失焦：结束会话状态，组合由 TSF 自行终止（OnCompositionTerminated 回收）
        _engine.Clear();
        _HideCandidate();
        // 图标输入面板不在这里收起：面板是不抢焦点的独立窗口，宿主失焦
        // （Office 点我们自己面板、切到别的窗口）时把面板关掉，用户就只能
        // 反复重开。真正的“该收起”由面板自己的前台归属检查判断。
        _engine.SaveUser();
        // 必须把上下文引用放掉：宿主失焦后会销毁这个 context，
        // 抱着不放的指针一旦被再次使用（插入符号、切中英）就会崩溃
        if (_pLastContext) {
            _pLastContext->Release();
            _pLastContext = nullptr;
        }
        if (_pComposition) {
            _pComposition->Release();
            _pComposition = nullptr;
        }
        return S_OK;
    }
    if (_pComposition) {
        _pComposition->Release();
        _pComposition = nullptr;
    }
    ITfContext* pic = nullptr;
    if (SUCCEEDED(pdimFocus->GetTop(&pic)) && pic) {
        if (_pLastContext) _pLastContext->Release();
        _pLastContext = pic;
    }
    // 记住这个 document manager：焦点被宿主丢掉后 SetFocus 回它即可恢复
    pdimFocus->AddRef();
    if (_pDocMgr) _pDocMgr->Release();
    _pDocMgr = pdimFocus;
    return S_OK;
}

STDMETHODIMP CTextService::OnPushContext(ITfContext* pic) {
    if (pic) {
        pic->AddRef();
        if (_pLastContext) _pLastContext->Release();
        _pLastContext = pic;
    }
    return S_OK;
}

STDMETHODIMP CTextService::OnPopContext(ITfContext* pic) { (void)pic; return S_OK; }

//------------------------------------------------------------------------------
// 键盘事件
STDMETHODIMP CTextService::OnSetFocus(BOOL fForeground) {
    const bool wasForeground = _foreground;
    _foreground = fForeground != FALSE;
    // 从后台回到前台＝输入法切回来了：无条件中文。
    // Win10 上 Win+空格 切走既可能不调 Deactivate、也可能不重新 Activate，
    // 只靠那两条路径会漏；这里按「false → true」的边沿补一刀。
    // 同一前台内的连续 TRUE（UWP / WinUI 编辑区焦点抖动）不重复写。
    if (_foreground && !wasForeground) _SetChinese(true, L"foreground-return");
    if (_foreground) _status.NoteImeActivity();  // 活跃心跳：代理窗口场景保显示
    if (!_foreground) {
        _HideCandidate();
        // 切走窗口＝本次输入结束：只按了 z、还没打字母时把临时拼音撤掉，
        // 避免切回来状态条还显示「拼」。已有半截拼音的组合交给
        // OnCompositionTerminated 里的 _engine.Clear() 统一复位并清掉编码字母。
        if (_engine.tempPinyin() && !_engine.hasComp()) {
            _engine.CancelTempPinyin();
            _UpdateStatus();
        }
        // 状态条和符号面板都不在这里收起：WinUI / UWP / 记事本这类应用的编辑区
        // 会在多个输入线程之间反复切换焦点，一失焦就隐藏会让状态条在打字时
        // 忽隐忽现；MS Office 更是会在点到我们自己的面板窗口时立刻报“失焦”，
        // 那样面板刚点开就消失。统一交给窗口自己的前台归属检查（250/400ms 一次）。
    } else if (_opt.statusBar) {
        _status.Show();
        _UpdateStatus();
    }
    return S_OK;
}

wchar_t CTextService::_VkToChar(WPARAM vk, bool shift) const {
    switch (vk) {
        case VK_OEM_1: return shift ? L':' : L';';
        case VK_OEM_PLUS: return shift ? L'+' : L'=';
        case VK_OEM_COMMA: return shift ? L'<' : L',';
        case VK_OEM_MINUS: return shift ? L'_' : L'-';
        case VK_OEM_PERIOD: return shift ? L'>' : L'.';
        case VK_OEM_2: return shift ? L'?' : L'/';
        case VK_OEM_3: return shift ? L'~' : L'`';
        case VK_OEM_4: return shift ? L'{' : L'[';
        case VK_OEM_5: return shift ? L'|' : L'\\';
        case VK_OEM_6: return shift ? L'}' : L']';
        case VK_OEM_7: return shift ? L'"' : L'\'';
        case '1': return L'!';
        case '2': return L'@';
        case '3': return L'#';
        case '4': return L'$';
        case '5': return L'%';
        case '6': return L'^';
        case '7': return L'&';
        case '8': return L'*';
        case '9': return L'(';
        case '0': return L')';
        default: return 0;
    }
}

bool CTextService::_TestEaten(WPARAM vk, LPARAM lp) {
    (void)lp;
    const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    // Ctrl+空格：切换中英文（与 Shift 单击等效，可在设置中关闭）
    if (_opt.ctrlSpace && ctrl && !alt && vk == VK_SPACE) return true;
    // Ctrl+数字：手动调频，把第 N 个候选调到第一位（有候选时才有意义）
    if (_opt.manualFreq && ctrl && !alt && vk >= '1' && vk <= '9' && _engine.hasComp())
        return true;
    if (ctrl || alt) return false;
    // Shift 轻按切中/英需要收到 keydown + keyup；TSF 只把「测试阶段判为吃」的键
    // 交给 OnKeyDown/OnKeyUp，这里恒 false 会让整条 shift-tap 路径变成死代码。
    // 只声明「有兴趣」：OnKeyDown 的 Shift 分支把 pfEaten 留 FALSE，键照常传给
    // 应用，不影响 Shift 做修饰键。设置里关掉「Shift 单击切换」就完全不吃。
    if (vk == VK_SHIFT) return !IsPreWin10() && _opt.shiftToggle != 0;  // Win7 交给轮询，避免双触发/残留

    if (_engine.hasComp()) {
        switch (vk) {
            case VK_BACK:
            case VK_SPACE:
            case VK_RETURN:
            case VK_ESCAPE:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_UP:
            case VK_DOWN:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_OEM_MINUS:
            case VK_OEM_PLUS:
            case VK_OEM_COMMA:
            case VK_OEM_PERIOD:
            case VK_OEM_4:
            case VK_OEM_6:
            case VK_TAB:
                return true;
            default:
                break;
        }
        if (vk >= '0' && vk <= '9') return true;
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return _opt.numpadSelect != 0;
        if (vk >= 'A' && vk <= 'Z') return true;
        return false;
    }

    if (!_chinese) return false;

    // 按 z 进入临时拼音、还没打字母时组合是空的，Esc / 回车 / 退格
    // 平时不会被吃，OnKeyDown 收不到 → 临时状态会一直挂着。这里补上让它们能退出。
    // 空格不在此列：它照常输出半角/全角空格，临时状态保留到明确结束。
    if (_engine.tempPinyin() && !_engine.hasComp() &&
        (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_BACK))
        return true;

    // 按住 Shift 不放直接输入英文：不吃键，交给目标程序，得到大写字母
    if (_opt.shiftEnglish && vk >= 'A' && vk <= 'Z' &&
        (::GetKeyState(VK_SHIFT) & 0x8000) != 0)
        return false;

    if (vk >= 'A' && vk <= 'Z') return true;
    switch (vk) {
        case VK_OEM_1:
        case VK_OEM_PLUS:
        case VK_OEM_COMMA:
        case VK_OEM_MINUS:
        case VK_OEM_PERIOD:
        case VK_OEM_2:
        case VK_OEM_3:
        case VK_OEM_4:
        case VK_OEM_5:
        case VK_OEM_6:
        case VK_OEM_7:
            return true;
        default:
            break;
    }
    if (vk >= '0' && vk <= '9') {
        const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        return shift;  // Shift+数字 转为全角符号
    }
    return false;
}

STDMETHODIMP CTextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                         BOOL* pfEaten) {
    (void)pic;
    if (!pfEaten) return E_INVALIDARG;
    // 有键送到本实例，就是「输入法仍在使用」最硬的证据。代理窗口类程序
    // 的前台 PID 与本实例不同，靠这个心跳保住状态条不自动消失。
    _status.NoteImeActivity();
    // Win7 专用：轮询法检测 Shift 轻按。若 Shift 正按住且本次是普通可输出字符，
    // 说明用户在用 Shift 做修饰（打大写/符号），标记「已用」避免被当成轻按切换。
    if (IsPreWin10() && _shiftPollDownTick &&
        (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        if ((wParam >= 'A' && wParam <= 'Z') || (wParam >= '0' && wParam <= '9') ||
            (wParam >= VK_OEM_1 && wParam <= VK_OEM_102)) {
            _shiftPollUsed = true;
        }
    }
    // 这个回调对**每一个** keydown 都会来，不受"是否被吃"影响。隔间跟随需要的
    // 按键证据必须在这儿记：OnKeyDown 只收被吃的键，而 VK_CONTROL 永远不会被吃
    // （_TestEaten 里 ctrl 组合直接 return false），写在那儿等于永远不更新。
    if (wParam == VK_CONTROL) _modKeyTick = ::GetTickCount();
    if (wParam == VK_SPACE && _opt.ctrlSpace &&
        (::GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (::GetKeyState(VK_MENU) & 0x8000) == 0)
        _ctrlSpaceTick = ::GetTickCount();
    *pfEaten = _TestEaten(wParam, lParam) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                       BOOL* pfEaten) {
    (void)pic;
    (void)lParam;
    // TSF 只在 pfEaten=TRUE 时才回调 OnKeyUp；原来恒 FALSE，shift-tap 永远
    // 收不到抬键，整条路径是死代码。OnKeyUp 里 pfEaten 仍是 FALSE，应用照常
    // 收到 Shift 抬键，不影响它做修饰键。
    if (pfEaten)
        *pfEaten = (_opt.shiftToggle && wParam == VK_SHIFT && _shiftDown) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    if (!pfEaten) return E_INVALIDARG;
    *pfEaten = FALSE;
    if (!pic) return S_OK;

    // 按键证据统一在 OnTestKeyDown 里记（见那里注释），这里不再重复。

    // 其他窗口改了设置（方案/状态条等）时同步过来
    _CheckConfigReload();

    // 一开始打字就收起图标输入面板，避免它滞留在屏幕上
    if (_sym.visible()) _HideSymbols();

    if (wParam == VK_SHIFT) {
        // 故意不设 *pfEaten：只借 TSF 的回调记下按下时刻，键本身继续交给应用，
        // 这样「Shift+点击 / Shift 做修饰」等行为完全不受影响。
        _shiftDown = true;
        _shiftUsedAsEnglish = false;
        _shiftTick = ::GetTickCount();
        return S_OK;
    }

    const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // 中文状态下按住 Shift 打字母 → 直接出英文（由程序处理，输入法不拦）
    if (_chinese && !_engine.hasComp() && _opt.shiftEnglish && shift && wParam >= 'A' &&
        wParam <= 'Z') {
        _shiftUsedAsEnglish = true;
        return S_OK;
    }

    if (!_TestEaten(wParam, lParam)) return S_OK;
    *pfEaten = TRUE;

    if (_pLastContext != pic) {
        if (_pLastContext) _pLastContext->Release();
        _pLastContext = pic;
        pic->AddRef();
    }

    // Ctrl+空格 切中英文（能走到这里的系统上，热键没被系统吞掉）
    if (wParam == VK_SPACE && _opt.ctrlSpace &&
        (::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        QyTrace(L"key sink Ctrl+Space");
        _SetChinese(!_chinese, L"ctrl+space(key)");
        if (_pLastContext) _Sync(_pLastContext);
        return S_OK;
    }

    // Ctrl+数字 手动调频：把当前页第 N 个候选提到首位并记住（写入 promote.dat）
    if (_opt.manualFreq && wParam >= '1' && wParam <= '9' &&
        (::GetKeyState(VK_CONTROL) & 0x8000) != 0 && _engine.hasComp()) {
        size_t n = (size_t)(wParam - '1');
        if (_engine.PromoteOnPage(n)) {
            _sel = 0;
            if (_pLastContext) _Sync(_pLastContext);
        }
        return S_OK;
    }

    if (_engine.hasComp()) {
        if (wParam >= 'A' && wParam <= 'Z') {
            _engine.OnLetter((char)(wParam - 'A' + 'a'));
        } else if (wParam >= '1' && wParam <= '9') {
            _engine.OnDigit((int)(wParam - '0'));
        } else if (wParam >= VK_NUMPAD1 && wParam <= VK_NUMPAD9) {
            _engine.OnDigit((int)(wParam - VK_NUMPAD0));
        } else {
            switch (wParam) {
                case VK_SPACE: _engine.OnSpace(); break;
                case VK_BACK: _engine.OnBackspace(); break;
                case VK_ESCAPE: _engine.OnEscape(); break;
                case VK_RETURN: _engine.OnEnter(); _HideCandidate(); break;
                case VK_TAB: _ToggleMode(); break;
                case VK_NEXT:
                case VK_DOWN:
                case VK_RIGHT:
                case VK_OEM_PLUS:
                case VK_OEM_6:
                    _engine.OnPageKey(true);
                    break;
                case VK_PRIOR:
                case VK_UP:
                case VK_LEFT:
                case VK_OEM_MINUS:
                case VK_OEM_4:
                    _engine.OnPageKey(false);
                    break;
                default: break;
            }
        }
    } else {
        if (wParam >= 'A' && wParam <= 'Z') {
            // 五笔方案、编码为空、且尚未进入临时拼音时，z 是「临时拼音」引导键：
            // 它本身不进编码缓冲，接着打全拼即可；要拼 z 开头的音节（zhong / zai）
            // 就再按一次 z，那时它已是普通拼音字母。拼音 / 混输方案里 z 仍是普通字母。
            if (wParam == 'Z' && _engine.CanTempPinyin()) {
                _engine.BeginTempPinyin();
                QyTrace(L"temp pinyin: begin (z as lead key)");
                _UpdateStatus();
                _Sync(pic);
                return S_OK;
            }
            _engine.OnLetter((char)(wParam - 'A' + 'a'));
        } else {
            // 组合为空时按 Esc / 回车 / 退格：放弃本次临时拼音，状态条还原成「五」
            if (_engine.tempPinyin() &&
                (wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_BACK)) {
                _engine.CancelTempPinyin();
                QyTrace(L"temp pinyin: cancel");
                _UpdateStatus();
                _Sync(pic);
                return S_OK;
            }
            wchar_t ch = _VkToChar(wParam, shift);
            if (ch) {
                if (wParam >= '0' && wParam <= '9')
                    _CommitNow(_engine.punct(ch, shift));
                else
                    _CommitNow(_engine.punct(ch, shift));
            }
        }
    }
    _sel = 0;
    _Sync(pic);
    return S_OK;
}

STDMETHODIMP CTextService::OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
    (void)lParam;
    if (pfEaten) *pfEaten = FALSE;
    if (wParam == VK_SHIFT) {
        if (_shiftDown) {
            DWORD dt = ::GetTickCount() - _shiftTick;
            _shiftDown = false;
            // 单击 Shift 切中/英（可选功能，设置窗口里可关闭）
            if (_opt.shiftToggle && dt < 800 && !_engine.hasComp() && !_shiftUsedAsEnglish) {
                bool toEnglish = _chinese;
                QyTrace(L"shift tap -> %s", toEnglish ? L"english" : L"chinese");
                _SetChinese(!_chinese, L"shift-tap");
                if (toEnglish && pic) _Sync(pic);
            }
        }
    } else {
        _shiftDown = false;
    }
    _shiftUsedAsEnglish = false;
    return S_OK;
}

STDMETHODIMP CTextService::OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) {
    if (pfEaten) *pfEaten = FALSE;
    if (!IsEqualGUID(rguid, GUID_QYKeyCtrlSpace)) return S_OK;
    if (!_opt.ctrlSpace) return S_OK;
    QyTrace(L"preserved key fired -> toggle");
    if (pic) {
        if (_pLastContext != pic) {
            if (_pLastContext) _pLastContext->Release();
            _pLastContext = pic;
            pic->AddRef();
        }
    }
    _SetChinese(!_chinese, L"ctrl+space(preserved)");
    if (pic) _Sync(pic);
    if (pfEaten) *pfEaten = TRUE;
    return S_OK;
}

STDMETHODIMP CTextService::OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) {
    if (_pComposition && _pComposition == pComposition) {
        _pComposition->Release();
        _pComposition = nullptr;
    }
    if (!_endingComposition) {
        // 宿主主动终止组合（MS Word 在提交后、焦点或格式变化时都会这样）：
        // 组合一终止，里面还没上屏的编码字母就会“定稿”留在文档里，
        // 必须顺手抹掉，否则 Word 里会看到打了一半的编码
        if (pComposition) {
            ITfRange* pRange = nullptr;
            if (SUCCEEDED(pComposition->GetRange(&pRange)) && pRange) {
                pRange->SetText(ecWrite, 0, L"", 0);
                pRange->Release();
            }
        }
        _engine.Clear();
        _HideCandidate();
    }
    return S_OK;
}

}  // namespace qy
