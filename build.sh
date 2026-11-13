#!/bin/bash
# 秋雨五笔输入法 - 一键构建脚本
# 依赖：mingw-w64 (msvcrt 变体，Win7 自带 msvcrt.dll，零运行库依赖)、Python 3
#
# 为什么不用 zig：zig 的 windows-gnu 目标写死链接 UCRT（api-ms-win-crt-*.dll），
# Win7 上这套 API Set 不存在，输入法 DLL 加载直接失败。msvcrt 变体的 mingw-w64
# 静态链接 CRT，Win7 SP1 / Win8 / Win10 / Win11 开箱即用。
#
# 产物：
#   dist/秋雨五笔输入法_<ver>_Setup.exe        64 位（Win7 x64 ~ Win11）
#   dist/秋雨五笔输入法_<ver>_x86_Setup.exe    32 位（Win7 x86 SP1 起）
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

GCC64="${GCC64:-$ROOT/../tools/mingw-dl/w64gcc/bin/g++.exe}"
GCC32="${GCC32:-$ROOT/../tools/mingw-dl/w32/mingw32/bin/g++.exe}"
for g in "$GCC64" "$GCC32"; do
    if [ ! -f "$g" ]; then
        echo "[错误] 找不到编译器：$g"
        echo "  下载 mingw-w64 msvcrt 变体并解压到 ../tools/mingw-dl/ 下"
        exit 1
    fi
done

# PE 头固定 6.0（Vista 基线），Win7 完全兼容
PEFLAGS="-Wl,--major-os-version,6 -Wl,--minor-os-version,0 -Wl,--major-subsystem-version,6 -Wl,--minor-subsystem-version,0"

# 构建戳（14 位 YYYYMMDDHHMMSS）。
# 日常修复不升版本号，于是同一个版本号会对应多次构建。服务器清单若停留在
# 较早的构建，客户端只按 CRC 比对就会把本机较新的文件判成「待更新」，点更新
# 即回退成旧构建（版本号一样，版本规则看不出来）。安装目录的 installed.build
# 与清单的 build= 都用这个戳，同版本时据此判新旧。
# 复现某次构建：QY_BUILD_STAMP=20261108120000 bash build.sh
BUILD_STAMP="${QY_BUILD_STAMP:-$(date +%Y%m%d%H%M%S)}"
export QY_BUILD_STAMP="$BUILD_STAMP"

COMMON="-O2 -std=c++17 -fno-exceptions -fno-rtti -w -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -DQY_BUILD_STAMP=L\"$BUILD_STAMP\""
STATIC="-static -static-libgcc -static-libstdc++"
WARN="-Wl,--subsystem,windows -mwindows"

# winlibs 发行包不带 libmsctf.a（TSF 导入库）：从 zig 自带的 def 生成一次
TOOLDIR="$ROOT/../tools"
DEF64="$TOOLDIR/zig-windows-x86_64-0.13.0/lib/libc/mingw/lib-common/msctf.def"
DEF32="$TOOLDIR/zig-windows-x86_64-0.13.0/lib/libc/mingw/lib32/msctf.def"
LIB64="$GCC64"; LIB64="${LIB64%/bin/g++.exe}"
LIB64="$LIB64/x86_64-w64-mingw32/lib/libmsctf.a"
LIB32="$GCC32"; LIB32="${LIB32%/bin/g++.exe}"
LIB32="$LIB32/lib/libmsctf.a"
if [ ! -f "$LIB64" ] && [ -f "$DEF64" ]; then
    "$TOOLDIR/mingw-dl/w64gcc/bin/dlltool.exe" -d "$DEF64" -l "$LIB64" -m i386:x86-64 --as-flags=--64
    echo "生成 $LIB64"
fi
if [ ! -f "$LIB32" ] && [ -f "$DEF32" ]; then
    "$TOOLDIR/mingw-dl/w32/mingw32/bin/dlltool.exe" -d "$DEF32" -l "$LIB32" -m i386
    echo "生成 $LIB32"
fi

IME_SRC="src/ime/qy_ime.cpp src/ime/qy_dll.cpp src/core/qy_engine.cpp src/core/qy_dict.cpp src/core/qy_config.cpp src/core/qy_candui.cpp src/core/qy_statusbar.cpp src/core/qy_symui.cpp src/core/qy_update.cpp"
DEMO_SRC="src/tools/qydemo.cpp src/core/qy_engine.cpp src/core/qy_dict.cpp src/core/qy_config.cpp src/core/qy_candui.cpp src/core/qy_statusbar.cpp src/core/qy_update.cpp"
UPD_SRC="src/tools/qyupd.cpp src/core/qy_config.cpp src/core/qy_update.cpp"

IMELIBS="-lole32 -loleaut32 -luuid -lshlwapi -lshell32 -luser32 -lgdi32 -lmsctf -limm32 -lwininet -ladvapi32"
DEMOLIBS="-lole32 -loleaut32 -lshlwapi -lshell32 -luser32 -lgdi32 -lwininet -lcomdlg32"
UPDLIBS="-lshell32 -luser32 -lgdi32 -lwininet -ladvapi32 -lole32"

mkdir -p build dist build/x86

echo "== 1/9 编译码表 =="
python tools/build_dict.py

echo "== 2/9 编译输入法 DLL（64 位 + 32 位） =="
"$GCC64" -shared $COMMON $PEFLAGS $STATIC $IME_SRC -o build/qywb64.dll $IMELIBS
"$GCC32" -shared $COMMON $PEFLAGS $STATIC $IME_SRC -o build/qywb32.dll $IMELIBS
# 32 位 stdcall 导出带 @N 修饰，注册表期待的导出名不带；就地改写
python tools/fix_exports.py build/qywb32.dll

echo "== 3/9 编译 32 位注册辅助程序（GUI 子系统，避免弹出命令行窗口） =="
"$GCC32" $COMMON $PEFLAGS $STATIC $WARN src/tools/qyreg.cpp -o build/qyreg32.exe -lshell32 -luser32

echo "== 4/9 编译设置/演示程序与增量更新程序（双架构） =="
"$GCC64" $COMMON $PEFLAGS $STATIC $WARN -DQY_SB_NO_TRACE $DEMO_SRC -o build/qydemo.exe $DEMOLIBS
"$GCC32" $COMMON $PEFLAGS $STATIC $WARN -DQY_SB_NO_TRACE $DEMO_SRC -o build/x86/qydemo.exe $DEMOLIBS
"$GCC64" $COMMON $PEFLAGS $STATIC $WARN $UPD_SRC -o build/qyupd.exe $UPDLIBS
"$GCC32" $COMMON $PEFLAGS $STATIC $WARN $UPD_SRC -o build/x86/qyupd.exe $UPDLIBS
"$GCC64" $COMMON $PEFLAGS $STATIC src/tools/mkicon.cpp -o build/mkicon.exe -lgdi32 -luser32
( cd build && ./mkicon.exe qywb.ico )

echo "== 5/9 编译安装程序存根（双架构） =="
"$GCC64" $COMMON $PEFLAGS $STATIC $WARN src/tools/qysetup.cpp -o build/setup_stub.exe -lole32 -loleaut32 -lshlwapi -lshell32 -luser32 -lgdi32 -lcomctl32 -luuid
"$GCC32" $COMMON $PEFLAGS $STATIC $WARN src/tools/qysetup.cpp -o build/x86/setup_stub.exe -lole32 -loleaut32 -lshlwapi -lshell32 -luser32 -lgdi32 -lcomctl32 -luuid

echo "== 6/9 注入图标/版本信息资源并打包安装程序 =="
# 环境无 windres/rc.exe，由 seticon.py 直接构造 .rsrc 节（RT_ICON + RT_GROUP_ICON + RT_VERSION）
# 版本信息是“通过安全软件安检”的必要条件：没有公司名/版本号的 exe 更容易被
# 360 之类判成高危，尤其是安装/卸载程序。
# 注意：任何重新编译过的 EXE/DLL 都必须在这一步重新注入，否则图标会丢失
VER="$(sed -n 's/.*QY_VERSION_STR L"\([0-9][0-9.]*\)".*/\1/p' src/core/qy_version.h | head -1)"
if [ -z "$VER" ]; then
    echo "[错误] 无法从 src/core/qy_version.h 读取版本号"
    exit 1
fi
echo "版本号：$VER　构建戳：$BUILD_STAMP"
SHORT_VER="$(echo "$VER" | cut -d. -f1,2)"
SETUP_NAME="秋雨五笔输入法_${SHORT_VER}_Setup.exe"
SETUP_X86="秋雨五笔输入法_${SHORT_VER}_x86_Setup.exe"
echo "安装包：$SETUP_NAME / $SETUP_X86"

inject() {
    local f="$1"; local desc="$2"; local gid="$3"; local extra="$4"
    if [ -n "$gid" ]; then
        python tools/seticon.py "$f" build/qywb.ico "$gid" --ver "$VER" --desc "$desc" $extra
    else
        python tools/seticon.py "$f" build/qywb.ico --ver "$VER" --desc "$desc" $extra
    fi
}
inject build/qydemo.exe "设置与演示程序"
inject build/x86/qydemo.exe "设置与演示程序"
inject build/qyupd.exe "增量更新程序"
inject build/x86/qyupd.exe "增量更新程序"
inject build/qyreg32.exe "输入法注册辅助程序"
inject build/qywb64.dll "输入法组件（64 位）" 100 --dll
inject build/qywb32.dll "输入法组件（32 位）" 100 --dll
inject build/setup_stub.exe "安装与卸载程序" "" "--name qywb_setup --orig "$SETUP_NAME""
inject build/x86/setup_stub.exe "安装与卸载程序" "" "--name qywb_setup --orig "$SETUP_X86""

echo "== 7/9 打包载荷（64 位含双 DLL；32 位只含 32 位组件） =="
python tools/make_payload.py --arch x64
python tools/make_payload.py --arch x86

echo "== 8/9 校验图标 / 版本资源（缺失即构建失败） =="
python tools/seticon.py --verify build/qydemo.exe build/x86/qydemo.exe build/qyupd.exe build/x86/qyupd.exe \
    build/qyreg32.exe build/qywb64.dll build/qywb32.dll build/setup_stub.exe build/x86/setup_stub.exe \
    "dist/$SETUP_NAME" "dist/$SETUP_X86"

echo "== 9/9 生成增量更新包（只含变动文件，双架构） =="
# 增量基线优先用“本机已安装的版本”（= 最近一次发布到用户手里的版本），
# 生成出来的增量包就是“老用户 → 新版”需要的文件；本机没装（CI 环境）
# 才退回用上次的清单做基线。也可用 QY_BASELINE 显式指定基线目录。
BASELINE="${QY_BASELINE:-C:/Program Files/QiuYuWubi}"
if [ -d "$BASELINE" ]; then
    python tools/make_inc.py --baseline "$BASELINE"
else
    python tools/make_inc.py
fi

echo "== 上传前体检：服务器文本编码 =="
# version.txt 会被浏览器直接打开，nginx 只给 text/plain 不带 charset，
# 无 BOM 的 UTF-8 在简体中文 Windows 上会被按 GBK 解码成乱码。
# 这里只提示不中断（升版本时 version.txt 可能还没改），上传前必须清零。
python tools/check_dist_text.py ||
    echo "！！以上问题会导致浏览器打开 version.txt 乱码 / 客户端检查更新失败，上传前务必修好"

echo
echo "构建完成："
ls -lh dist/
