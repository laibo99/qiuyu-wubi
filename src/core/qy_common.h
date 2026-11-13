// 秋雨五笔输入法 - 公共定义
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace qy {

// UTF-8 -> UTF-16
inline std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// UTF-16 -> UTF-8
inline std::string WToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// UTF-16 -> GBK（用于旧版程序与文本文件）
inline std::string WToGbk(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = ::WideCharToMultiByte(936, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    ::WideCharToMultiByte(936, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

inline std::wstring GbkToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(936, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(936, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

// 设置窗口（qydemo.exe --settings）是否在前台？
//
// 用途：设置窗口在调状态条的 DIY 配色，用户必须能看着状态条变。
// 所以这种前台不算“切到别的程序了”，状态条要留着做实时预览。
// 判断两条：窗口类名是 QyDemoMain、标题是设置窗口标题；不直接比进程名，
// 是因为 qydemo 演示模式用同一个窗口类。
inline bool IsSettingsWindowForeground(HWND fg) {
    if (!fg) return false;
    for (int i = 0; i < 8 && fg; ++i) {  // 顺着属主链找（提示框的属主才是设置窗口）
        wchar_t cls[64] = {0};
        if (::GetClassNameW(fg, cls, 64) && ::lstrcmpiW(cls, L"QyDemoMain") == 0) {
            wchar_t title[128] = {0};
            ::GetWindowTextW(fg, title, 128);
            // 演示模式标题是“……演示与设置”，也含“设置”二字，所以必须整串比较
            if (::lstrcmpiW(title, L"秋雨五笔输入法 - 设置") == 0) return true;
        }
        fg = ::GetWindow(fg, GW_OWNER);
    }
    return false;
}

inline std::wstring ModuleDir(HMODULE h) {
    wchar_t buf[MAX_PATH * 2] = {0};
    ::GetModuleFileNameW(h, buf, MAX_PATH * 2);
    std::wstring p(buf);
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos);
    return p;
}

inline std::wstring DataDir() {
    // 优先级: 环境变量 QYWB_DATA > 模块目录\data > 模块目录
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD n = ::GetEnvironmentVariableW(L"QYWB_DATA", buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return std::wstring(buf);
    std::wstring dir = ModuleDir(nullptr);
    std::wstring sub = dir + L"\\data\\qywb.dat";
    if (::GetFileAttributesW(sub.c_str()) != INVALID_FILE_ATTRIBUTES) return dir + L"\\data";
    return dir;
}

inline std::wstring UserDir() {    // 允许用环境变量指定用户目录（便于测试与绿色运行）
    wchar_t env[MAX_PATH * 2] = {0};
    DWORD en = ::GetEnvironmentVariableW(L"QYWB_USERDIR", env, MAX_PATH * 2);
    if (en > 0 && en < MAX_PATH * 2) {
        std::wstring d(env);
        ::CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }
    wchar_t buf[MAX_PATH * 2] = {0};
    if (::SHGetFolderPathW(nullptr, 0x001a /*CSIDL_APPDATA*/, nullptr, 0, buf) == S_OK) {
        std::wstring d = std::wstring(buf) + L"\\QiuYuWubi";
        ::CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }
    return ModuleDir(nullptr);
}

// ---- “状态条实时预览”的归属进程 ----
//
// 设置窗口（qydemo --settings）打开时状态条要留着做预览，可屏幕后面可能
// 有好几个程序都加载着输入法、各自有一份状态条实例。谁来显示必须只有一个，
// 否则它们互相 SW_HIDE，画面会闪。
// 约定：设置窗口启动时，把“当时前台窗口所属的进程”记进 preview.pid——
// 那正是用户点的那个状态条所在的进程；只有它能显示，别的实例一律不抢。
inline std::wstring PreviewPidFile() { return UserDir() + L"\\preview.pid"; }

inline DWORD ReadPreviewPid() {
    HANDLE h = ::CreateFileW(PreviewPidFile().c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[32] = {0};
    DWORD got = 0;
    ::ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    ::CloseHandle(h);
    return (DWORD)strtoul(buf, nullptr, 10);
}

inline void WritePreviewPid(DWORD pid) {
    HANDLE h = ::CreateFileW(PreviewPidFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[32];
    wsprintfA(buf, "%lu", (unsigned long)pid);
    DWORD wr = 0;
    ::WriteFile(h, buf, (DWORD)strlen(buf), &wr, nullptr);
    ::CloseHandle(h);
}

inline void ClearPreviewPid() { ::DeleteFileW(PreviewPidFile().c_str()); }

// ---- 跨实例唯一的中/英真值 ----
//
// 每个宿主进程（同一进程里也可能有多个 UI 线程）各持一份 CTextService，
// _chinese 各存各的，从不共享。GHO 精简 Win10 在焦点/上下文切换时会重置
// GUID_COMPARTMENT_KEYBOARD_OPENCLOSE，多个实例同时收到 OnChange，又各自把
// 隔间写回自己的 _chinese → 互相触发 → 无限乒乓：状态条「中/英」无休止乱跳，
// 屏幕上同时存在的几份状态条还会一份显示「中」、一份显示「英」。
//
// 解法：中/英只认一个全局真值，放在会话内命名共享内存中，用 Interlocked
// 原子读写。任何实例切换都写这里；其它实例在 250ms tick 里静默跟上
// （只改自己的 _chinese 和状态条，绝不回写隔间/共享，避免再启一轮广播）。
struct ChineseStateLayout {
    volatile LONG magic;    // 'QYNB' = 已初始化（共享内存初始为全 0）
    volatile LONG chinese;  // 0=未知  1=中文  2=英文
    volatile LONG reserved;
};

// 必须把 CreateFileMapping 的 HANDLE 一直留到 UnmapViewOfFile 之后。
// 踩坑实录：原写法 MapViewOfFile 后立刻 CloseHandle，实测第二次同名
// CreateFileMappingW 的 GetLastError() 是 0（而非 ERROR_ALREADY_EXISTS），
// 说明内核对象已被销毁、创建了一份全新的 → 两个"实例"各写各的，共享完全失效。
struct ChineseState {
    HANDLE map = nullptr;
    ChineseStateLayout* view = nullptr;
};

inline const wchar_t* ChineseStateMapName() { return L"Local\\QiuYuWubiCN_v1"; }

// 用 NULL DACL 建映射：输入法 DLL 跑在各种宿主进程里，完整性级别可能不同
// （GHO 精简系统尤其常见），宽松 DACL 保证都能打开同一份状态。
inline ChineseState OpenChineseStateNamed(const wchar_t* name) {
    ChineseState cs;
    if (!name || !*name) return cs;
    SECURITY_DESCRIPTOR sd;
    if (!::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) return cs;
    if (!::SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE)) return cs;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;
    cs.map = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                                  sizeof(ChineseStateLayout), name);
    if (!cs.map) return cs;
    cs.view = (ChineseStateLayout*)::MapViewOfFile(cs.map, FILE_MAP_ALL_ACCESS, 0, 0,
                                                   sizeof(ChineseStateLayout));
    if (!cs.view) {
        ::CloseHandle(cs.map);
        cs.map = nullptr;
        return cs;
    }
    ::InterlockedCompareExchange(&cs.view->magic, 0x51594E42L, 0);
    return cs;
}

inline ChineseState OpenChineseState() {
    return OpenChineseStateNamed(ChineseStateMapName());
}

inline void CloseChineseState(ChineseState& cs) {
    if (cs.view) ::UnmapViewOfFile(cs.view);
    if (cs.map) ::CloseHandle(cs.map);
    cs.view = nullptr;
    cs.map = nullptr;
}

// 原子读：返回 0/1/2，0 表示还没有任何实例定过状态
inline int SharedChinese(const ChineseState& cs) {
    if (!cs.view) return 0;
    const LONG v = ::InterlockedCompareExchange(&cs.view->chinese, 0, 0);
    return (v == 1 || v == 2) ? (int)v : 0;
}

inline void SetSharedChinese(ChineseState& cs, bool chinese) {
    if (cs.view) ::InterlockedExchange(&cs.view->chinese, chinese ? 1L : 2L);
}

// 状态条位置也做成跨进程唯一真值（与中/英同理）。GHO 精简系统里输入法 DLL 常跑在
// explorer / 桌面这种跨完整性级别的宿主里，那些宿主读不到 %APPDATA% 的配置文件，
// 于是各自记住的位置不同：能读到配置的后台进程把条放在用户拖好的位置（如顶部），
// 读不到的就回落到默认右下角，前台一切换就"上下跳动"。放进同一份 NULL DACL 的命名
// 共享内存，让所有宿主都拿到同一个位置。
struct StatusBarPosLayout {
    volatile LONG magic;  // 'QYSP' = 已初始化（共享内存初始为全 0）
    volatile LONG x;      // 屏幕坐标；-1 = 未设定
    volatile LONG y;
};

inline const wchar_t* StatusBarPosMapName() { return L"Local\\QiuYuWubiSBP_v1"; }

// 复用 ChineseState（就是 map + view 两个句柄），只是映射名与视图布局不同。
// 这里不需要把 magic 当一次性初始化锁：共享内存零初始化，magic 为 0，
// 第一次 SetSharedStatusBarPos 之后才变成 'QYSP'，读取时据此判断有无有效值。
inline ChineseState OpenStatusBarPos() {
    return OpenChineseStateNamed(StatusBarPosMapName());
}

inline bool GetSharedStatusBarPos(const ChineseState& cs, int& x, int& y) {
    if (!cs.view) return false;
    StatusBarPosLayout* p = reinterpret_cast<StatusBarPosLayout*>(cs.view);
    if (p->magic != 0x51595350L) return false;  // 还没有任何宿主定过位置
    const LONG vx = ::InterlockedCompareExchange(&p->x, -1, -1);
    const LONG vy = ::InterlockedCompareExchange(&p->y, -1, -1);
    if (vx < 0 || vy < 0) return false;
    x = (int)vx;
    y = (int)vy;
    return true;
}

inline void SetSharedStatusBarPos(const ChineseState& cs, int x, int y) {
    if (!cs.view) return;
    StatusBarPosLayout* p = reinterpret_cast<StatusBarPosLayout*>(cs.view);
    ::InterlockedExchange(&p->x, (LONG)x);
    ::InterlockedExchange(&p->y, (LONG)y);
    ::InterlockedExchange(&p->magic, 0x51595350L);
}

// 真实系统主版本号。GetVersionExW 从 Win8.1 起受宿主 manifest 影响会谎报 6.2，
// 排障时会被误导；动态调 ntdll!RtlGetVersion 取真实值（Win7~Win11 都有）。
// 结果按进程缓存：每个宿主进程只探测一次。
inline unsigned long RealOsMajorVersion() {
    static const unsigned long major = []() -> unsigned long {
        OSVERSIONINFOW oi{};
        oi.dwOSVersionInfoSize = sizeof(oi);
        typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
        HMODULE hNt = ::LoadLibraryW(L"ntdll.dll");
        RtlGetVersionFn f =
            hNt ? (RtlGetVersionFn)(void*)::GetProcAddress(hNt, "RtlGetVersion") : nullptr;
        const bool ok = f && f(&oi) == 0;
        if (!ok) {
            oi.dwOSVersionInfoSize = sizeof(oi);
            ::GetVersionExW(&oi);
        }
        if (hNt) ::FreeLibrary(hNt);
        return (unsigned long)oi.dwMajorVersion;
    }();
    return major;
}

inline bool IsPreWin10() { return RealOsMajorVersion() < 10; }

// ---- 隔间变化决策（纯函数，供 OnChange 与自测共用） ----
//
// 关键不变量：收到隔间通知时**绝不写回**隔间。写回是乒乓的引擎——每个实例
// 的 _chinese 都可能不同，"纠正"到自己的值就是在给别的实例发新通知。
// 状态条以本实例 _chinese 为准，隔间（任务栏指示）等用户下次主动切换或
// 重新激活时再对齐一次即可。
//
// preWin10（Win7/8）：Ctrl+空格 是系统级输入法热键，整套按键事件根本不会到
// ITfKeyEventSink，PreserveKey 也常被系统占用，**隔间跟随是唯一通路**，所以
// 不能要求「按着 Ctrl」这类按键证据。Win7 的 TSF 不会在焦点切换时乱重置
// 隔间（那是 Win10 才有的噪声），放宽不会造成「点开设置中英自己翻」。
// Win10+ 仍保持严格判据，挡住 GHO 精简系统的隔间重置噪声。
struct CompDecision {
    bool follow;     // 允许跟随这次隔间变化（认为是用户按了 Ctrl+空格）
    bool writeBack;  // 允许把隔间写回 —— 恒为 false
};

inline CompDecision DecideCompartmentChange(bool inited, bool withinActivateGrace,
                                           bool ctrlDown, bool keyJustPressed,
                                           bool ctrlSpaceEnabled, bool withinCooldown,
                                           bool preWin10 = false) {
    CompDecision d{};
    const bool looksLikeHotkey = preWin10 || ctrlDown || keyJustPressed;
    d.follow = inited && !withinActivateGrace && looksLikeHotkey &&
               ctrlSpaceEnabled && !withinCooldown;
    d.writeBack = false;
    return d;
}

}  // namespace qy
