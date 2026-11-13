// 秋雨五笔输入法 - 覆盖安装遗留备份文件清理
//
// 覆盖安装时，正在被 explorer / Office 映射的旧 DLL、码表没法直接覆盖，
// 只能先改名成 *.old / *.new / *.pending 再写新文件，并登记“重启后删除”。
// 这些备份文件在占用进程退出前删不掉，如果没人再管就会一直躺在安装目录里
// （用户反馈的“残留 old 文件占空间”）。
//
// 这里提供一套共用的清理逻辑，三个入口都会调用：
//   · 安装程序：安装前、安装后
//   · 输入法 DLL：每次激活（新进程首次加载时清一次）
//   · 设置与演示程序：启动时
// 谁先拿到“文件已不再被占用”的时机，谁就把它删掉。
#pragma once
#include <windows.h>
#include <string>

namespace qy {

// 是否是覆盖安装留下的备份文件（名字里带 .old / .new / .pending）
inline bool IsBackupFileName(const wchar_t* name) {
    if (!name || !*name) return false;
    wchar_t low[MAX_PATH];
    int n = ::lstrlenW(name);
    if (n >= MAX_PATH) n = MAX_PATH - 1;
    for (int i = 0; i < n; ++i) {
        wchar_t c = name[i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        low[i] = c;
    }
    low[n] = 0;
    return ::wcsstr(low, L".old") || ::wcsstr(low, L".new") || ::wcsstr(low, L".pending");
}

// 清理 dir 及其 data 子目录下的备份文件。
// 返回删除成功的个数，left 返回删不掉（仍被占用）的个数。
inline int PurgeBackupFiles(const std::wstring& dir, int* left = nullptr) {
    static const wchar_t* kPats[] = {L"\\*.old*", L"\\*.new*", L"\\*.pending*"};
    int deleted = 0, remain = 0;
    std::wstring dirs[2] = {dir, dir + L"\\data"};
    for (int d = 0; d < 2; ++d) {
        for (auto pat : kPats) {
            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileW((dirs[d] + pat).c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (!IsBackupFileName(fd.cFileName)) continue;
                std::wstring p = dirs[d] + L"\\" + fd.cFileName;
                if (::DeleteFileW(p.c_str())) {
                    ++deleted;
                    continue;
                }
                // 刚被释放的句柄有时还有几毫秒延迟，重试一次
                ::Sleep(60);
                if (::DeleteFileW(p.c_str()))
                    ++deleted;
                else
                    ++remain;
            } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
    }
    if (left) *left = remain;
    return deleted;
}

}  // namespace qy
