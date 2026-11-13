#!/bin/bash
# 编译回归测试工具（x64，用与主构建相同的 mingw 工具链）
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

GCC64="${GCC64:-$ROOT/../tools/mingw-dl/w64gcc/bin/g++.exe}"
PEFLAGS="-Wl,--major-os-version,6 -Wl,--minor-os-version,0 -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,0"
# QY_SB_NO_TRACE：自测工具不链接 qy_ime.cpp，把状态条里的 QyTrace 降级成空实现
COMMON="-O1 -std=c++17 -fno-exceptions -fno-rtti -w -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DQY_SB_NO_TRACE"
STATIC="-static -static-libgcc -static-libstdc++"
CORE="src/core/qy_engine.cpp src/core/qy_dict.cpp src/core/qy_config.cpp"
SBCORE="src/core/qy_statusbar.cpp src/core/qy_config.cpp"
UPDCORE="src/core/qy_update.cpp src/core/qy_config.cpp"

echo "== 测试工具 =="
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/mixtest.cpp $CORE -o build/mixtest.exe -lole32 -luuid -luser32 -ladvapi32
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/sbtest2.cpp $SBCORE -o build/sbtest2.exe -lole32 -luuid -luser32 -lgdi32 -ladvapi32
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/sbtest3.cpp $SBCORE -o build/sbtest3.exe -lole32 -luuid -luser32 -lgdi32 -ladvapi32
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/sbtest4.cpp $SBCORE -o build/sbtest4.exe -lole32 -luuid -luser32 -lgdi32 -ladvapi32 -lshell32
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/sbtest5.cpp $SBCORE -o build/sbtest5.exe -lole32 -luuid -luser32 -lgdi32 -ladvapi32 -lshell32
# 部分程序（UWP / TextInputHost / 多进程窗口）里状态条自动消失的回归
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/sbtest6.cpp $SBCORE -o build/sbtest6.exe -lole32 -luuid -luser32 -lgdi32 -ladvapi32 -lshell32
# 跨进程“设置窗口”替身（sbtest5 需要它触发真正的设置预览分支）
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/sbfakesettings.cpp -o build/sbfakesettings.exe -luser32 -lgdi32
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/updtest.cpp $UPDCORE -o build/updtest.exe -lwininet -lshell32 -luser32 -ladvapi32
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/upde2e.cpp $UPDCORE -o build/upde2e.exe -lwininet -lshell32 -luser32 -ladvapi32 -lole32 -luuid
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/wordtest2.cpp -o build/wordtest2.exe -lole32 -loleaut32 -luuid -luser32
# GHO 隔间乒乓 / 跨实例中英真值回归（只依赖 qy_common.h 的 inline 实现）
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/cnstatetest.cpp -o build/cnstatetest.exe -ladvapi32 -lshell32 -lole32 -luuid -luser32
# 回车清屏顺序 / 双皮肤接线 / 设置面板菜单化（源码护栏 + 配置真跑）
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/fixtest.cpp src/core/qy_config.cpp -o build/fixtest.exe -ladvapi32 -lshell32 -lole32 -luuid -luser32
echo "完成"
