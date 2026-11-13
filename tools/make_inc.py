#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成增量更新包：只打包“有变化”的文件

输出的 dist/update/inc 直接上传到 version.txt 所在目录的同级 inc 下：

    /downloads/update/version.txt          ← 里写 inc=inc/manifest.txt
    /downloads/update/inc/manifest.txt     ← 本脚本生成
    /downloads/update/inc/files/<crc>.bin  ← 只有变动文件（按内容寻址）

客户端（qyupd.exe）拿清单逐条和安装目录里的文件比对 CRC32，只下载对不上的
那几个，所以日常小改版只需要几百 KB 流量，不用再下 9MB 的完整包。

清单里的文件是“全量列表”（这一版每个文件应有的大小与指纹），
只有内容变了的才会在 files/ 里留一份实体；没变的文件服务器上不需要重传。

用法：
    python tools/make_inc.py                       # 用上次的清单做基线
    python tools/make_inc.py --baseline "C:/Program Files/QiuYuWubi"   # 用已装版本做基线
"""
import os
import re
import sys
import time
import zlib
import shutil
import zipfile

# 与 make_payload.py 保持一致：安装目录里的运行时文件。
# arch：x64 = 只在 64 位安装包里；x86 = 只在 32 位安装包里；any = 两个包都有。
# 同一安装目录路径可以出现两行（如 qydemo.exe 的 x64/x86 二进制），清单里
# 每行带 arch 字段，客户端只比对匹配自己架构（和 any）的条目。
# 注意：x64 条目必须排在同路径 x86 条目之后——旧版（4.2 及以前）客户端不认
# arch 字段，同一文件出现两次时会“全都要”，按清单顺序依次写入，排在最后
# 的会留下。x64 排最后保证老客户端在 64 位机器上最终留下的仍是 x64 二进制。
BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BASE)

_b = os.path.join(ROOT, 'build')
FILES = [
    ('qywb32.dll', os.path.join(_b, 'qywb32.dll'), 'any'),
    ('qywb.ico', os.path.join(_b, 'qywb.ico'), 'any'),
    ('qyreg32.exe', os.path.join(_b, 'qyreg32.exe'), 'any'),
    ('qydemo.exe', os.path.join(_b, 'x86', 'qydemo.exe'), 'x86'),
    ('qydemo.exe', os.path.join(_b, 'qydemo.exe'), 'x64'),
    ('qyupd.exe', os.path.join(_b, 'x86', 'qyupd.exe'), 'x86'),
    ('qyupd.exe', os.path.join(_b, 'qyupd.exe'), 'x64'),
    ('qywb64.dll', os.path.join(_b, 'qywb64.dll'), 'x64'),
    ('data\\qywb.dat', os.path.join(ROOT, 'data', 'qywb.dat'), 'any'),
    ('使用说明.txt', os.path.join(ROOT, 'dist', '使用说明.txt'), 'any'),
]

OUT_DIR = os.path.join(ROOT, 'dist', 'update', 'inc')
MANIFEST = os.path.join(OUT_DIR, 'manifest.txt')


def version():
    path = os.path.join(ROOT, 'src', 'core', 'qy_version.h')
    try:
        text = open(path, encoding='utf-8', errors='ignore').read()
    except OSError:
        return '0.0.0'
    m = re.search(r'QY_VERSION_STR\s+L"([0-9][0-9.]*)"', text)
    return m.group(1) if m else '0.0.0'


def crc32_of(path):
    crc = 0
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def build_stamp_value():
    """构建戳 14 位 YYYYMMDDHHMMSS。

    必须与编译进 qyupd / 安装程序的 QY_BUILD_STAMP 完全一致（build.sh 用同一个
    环境变量传给两边），否则「同版本不同构建」的新旧判定会错位。单独运行脚本
    时没有这个环境变量，退化为当前时间（仅用于本地试验，正式构建请走 build.sh）。
    """
    v = os.environ.get('QY_BUILD_STAMP', '').strip()
    if len(v) == 14 and v.isdigit():
        return v
    return time.strftime('%Y%m%d%H%M%S')


def read_manifest(path):
    """返回 {(相对路径, arch): (size, crc)}；文件不存在则返回空表。
    旧格式（不带 arch 字段）按 any 处理。"""
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path, encoding='utf-8-sig', errors='ignore'):
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, val = line.split('=', 1)
        if key.strip().lower() != 'file':
            continue
        parts = [p.strip() for p in val.split('|')]
        if len(parts) < 3:
            continue
        try:
            arch = parts[4] if len(parts) > 4 and parts[4] else 'any'
            out[(parts[0], arch)] = (int(parts[1]), int(parts[2], 16))
        except ValueError:
            continue
    return out


def read_dir_baseline(d):
    out = {}
    if not os.path.isdir(d):
        print(f'[警告] 基线目录不存在：{d}')
        return out
    for name, _, arch in FILES:
        p = os.path.join(d, name.replace('\\', os.sep))
        if os.path.exists(p):
            out[(name, arch)] = (os.path.getsize(p), crc32_of(p))
    return out


def main():
    baseline_dir = None
    force_include = set()
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == '--baseline' and i + 1 < len(args):
            baseline_dir = args[i + 1]
        if a == '--include' and i + 1 < len(args):
            force_include = {x.strip() for x in args[i + 1].split(',') if x.strip()}
    if baseline_dir:
        prev = read_dir_baseline(baseline_dir)
        print(f'基线目录：{baseline_dir}（{len(prev)} 个文件）')
    else:
        prev = read_manifest(MANIFEST)
        if prev:
            print(f'基线清单：{MANIFEST}（{len(prev)} 个文件）')
        else:
            print('没有基线（首次生成）：所有文件都会进增量包')

    ver = version()
    build_stamp = build_stamp_value()
    os.makedirs(os.path.join(OUT_DIR, 'files'), exist_ok=True)

    entries = []
    changed = []
    total_all = 0
    total_changed = 0
    for name, path, arch in FILES:
        if not os.path.exists(path):
            print(f'[错误] 缺少文件：{path}')
            return 1
        size = os.path.getsize(path)
        crc = crc32_of(path)
        total_all += size
        url = 'files/%08x.bin' % crc
        entries.append((name, size, crc, url, arch))
        old = prev.get((name, arch))
        # --include 用来强制把某些文件塞进增量包：例如“刚发布的基线”被开发
        # 过程覆盖过，但这些文件相对更早的公开版本确实变了，老用户必须有
        if old != (size, crc) or name in force_include:
            changed.append((name, path, size, crc, url))
            total_changed += size

    # 变动文件按内容寻址存一份；同名内容已存在就不重复写
    for name, path, size, crc, url in changed:
        dst = os.path.join(OUT_DIR, url.replace('/', os.sep))
        if not os.path.exists(dst):
            shutil.copyfile(path, dst)

    # 清单里的每个条目都要有对应实体（含未变动的）：用户本地文件损坏时
    # 增量更新应能下载任意条目，而不是 404。内容寻址，只会在首次多占空间
    src_map = {n: p for n, p, _a in FILES}
    for name, size, crc, url, arch in entries:
        dst = os.path.join(OUT_DIR, url.replace('/', os.sep))
        if not os.path.exists(dst):
            src = src_map.get(name)
            if src and os.path.exists(src):
                shutil.copyfile(src, dst)
            else:
                print(f'[警告] 清单条目缺实体：{name}（{url}）')

    # 清掉历史构建遗留的过期 bin：files/ 只保留本清单引用的内容
    files_root = os.path.join(OUT_DIR, 'files')
    if os.path.isdir(files_root):
        ref = {url.replace('/', os.sep) for _n, _s, _c, url, _a in entries}
        for fn in os.listdir(files_root):
            rel = os.path.join('files', fn)
            if rel not in ref:
                os.remove(os.path.join(files_root, fn))
                print(f'  清除过期增量文件 files/{fn}')

    lines = ['# 秋雨五笔输入法 增量更新清单（UTF-8）',
             '# 每行：file=安装目录相对路径|字节数|CRC32(十六进制)|下载地址|架构(x64/x86/any)',
             '# 客户端只比对匹配自身架构（和 any）的条目；同一文件出现两行时',
             '# x64 一行排在后面（老客户端不认架构字段，按顺序写入时留下的是最后一行）',
             f'version={ver}',
             f'build={build_stamp}',
             'notes=只下载有变化的文件']
    for name, size, crc, url, arch in entries:
        lines.append(f'file={name}|{size}|{crc:08x}|{url}|{arch}')
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(MANIFEST, 'w', encoding='utf-8-sig', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')

    # 不再打包“增量包.zip”：客户端的在线更新直接读服务器上的
    # version.txt + inc/manifest.txt + inc/files/*.bin，把 dist/update 整个
    # 目录上传即可，额外打个 zip 只是多一份容易传错的文件。
    # 真要手工离线更新，让用户直接运行安装包即可（安装包本身带全量文件）。

    print(f'  版本 {ver}（构建戳 {build_stamp}）：共 {len(entries)} 个条目（双架构），变动 {len(changed)} 个')
    print(f'  全量 {total_all/1048576:.1f} MB → 增量 {total_changed/1048576:.2f} MB'
          f'（省 {(1 - total_changed/max(total_all,1))*100:.1f}%）')
    for name, path, size, crc, url in changed:
        print(f'    {name:20s} {size/1024:8.1f} KB  {crc:08x}')
    # 顺手把 version.txt 复制进 update 目录：整个 dist/update 就是一个
    # 可以直接上传的镜像（version.txt + inc/），不用再手工摆位置
    src_ver = os.path.join(ROOT, 'dist', 'version.txt')
    if os.path.exists(src_ver):
        shutil.copyfile(src_ver, os.path.join(ROOT, 'dist', 'update', 'version.txt'))

    print(f'  清单：{MANIFEST}')
    print('  上传：把 dist/update 整个目录传到服务器同名目录下（不需要 zip）')
    return 0


if __name__ == '__main__':
    sys.exit(main())
