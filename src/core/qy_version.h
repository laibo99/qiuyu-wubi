// 秋雨五笔输入法 - 版本号（唯一来源）
//
// 升版本只改这里：安装程序的显示版本、输入法“关于”框、更新检测的比对
// 都引用本文件的宏，避免各处版本号写死后互相不一致。
#pragma once

#define QY_VERSION_STR L"4.5.3"

// 构建戳（14 位 YYYYMMDDHHMMSS），由 build.sh 每次构建注入：
//     -DQY_BUILD_STAMP=L"20261108155600"
//
// 为什么需要它：日常修复按增量更新发布、**不升版本号**，于是同一个版本号会
// 对应多次构建。服务器上的 inc/manifest.txt 若停留在较早的构建，客户端只按
// CRC 比对就会把「本机更新的文件」判成待更新，点更新即把文件回退到旧构建
// （版本号还一样，靠版本号判不出来）。
//
// 所以清单里带 build=，安装目录里记 installed.build，同版本时用它判新旧。
// 未注入（手写编译）时为 "0"，客户端一律视为「不知道」而不做拦截。
#ifndef QY_BUILD_STAMP
#define QY_BUILD_STAMP L"0"
#endif

// 更新信息（version.txt）的默认地址。
// 支持 http/https 网址，也支持本地或共享目录里的文件（如 D:\qywb\version.txt）。
// 写成目录（以 / 结尾）也可以：会自动补上 version.txt，
// 例如 http://down.sam.gd.cn/downloads/update/ → .../update/version.txt
// 留空则不自动检测；用户仍可在 %APPDATA%\QiuYuWubi\config.ini 的 [更新] 地址 里改。
#define QY_DEFAULT_UPDATE_URL L"http://down.sam.gd.cn/downloads/update/"

// 官方主页：设置窗口「关于 → 访问官方主页」和「关于 秋雨五笔输入法」
// 对话框里的第一行网址。
#define QY_HOME_URL L"http://sam.gd.cn"

// 五笔输入法发布页面（下载）：设置窗口「关于 → 访问发布页面（下载）」
// 和关于对话框里的第二行网址。
// 注意与 QY_DEFAULT_UPDATE_URL 区分：那个以 / 结尾，是服务器上
// version.txt 的基址；这个是给人用浏览器打开的网页，不带尾斜杠。
#define QY_SITE_URL L"http://down.sam.gd.cn/downloads/update"
