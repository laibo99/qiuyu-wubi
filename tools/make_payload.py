#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把运行时文件打包进安装程序：setup_stub.exe + 载荷 + 尾部索引"""
import os
import struct
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BASE)

STUB = {
    'x64': os.path.join(ROOT, 'build', 'setup_stub.exe'),
    'x86': os.path.join(ROOT, 'build', 'x86', 'setup_stub.exe'),
}


def short_version():
    """从 qy_version.h 取版本号，文件名只用主.次（4.0.0 → 4.0）。

    安装包文件名带版本号，用户下载目录里多个版本并存时不会分不清哪个是最新的。
    """
    import re
    path = os.path.join(ROOT, 'src', 'core', 'qy_version.h')
    try:
        text = open(path, encoding='utf-8', errors='ignore').read()
    except OSError:
        return '0.0'
    m = re.search(r'QY_VERSION_STR\s+L"([0-9][0-9.]*)"', text)
    if not m:
        return '0.0'
    return '.'.join(m.group(1).split('.')[:2])


def out_name(arch):
    v = short_version()
    # 64 位是主包，文件名不带架构；32 位包加 _x86 后缀
    return os.path.join(ROOT, 'dist', '秋雨五笔输入法_%s%s_Setup.exe'
                        % (v, '' if arch == 'x64' else '_x86'))


# 两个架构的载荷。安装目录里的文件名完全一致（qydemo.exe / qyupd.exe 按
# 安装包架构不同而内容不同），qywb64.dll 只在 64 位包里，32 位系统没有
# 64 位 DLL 的存在意义。
def files_for(arch):
    b = os.path.join(ROOT, 'build')
    files = [
        ('qywb32.dll', os.path.join(b, 'qywb32.dll')),
        ('qywb.ico', os.path.join(b, 'qywb.ico')),
        ('qyreg32.exe', os.path.join(b, 'qyreg32.exe')),
        ('data\\qywb.dat', os.path.join(ROOT, 'data', 'qywb.dat')),
        ('使用说明.txt', os.path.join(ROOT, 'dist', '使用说明.txt')),
    ]
    if arch == 'x64':
        files.insert(0, ('qywb64.dll', os.path.join(b, 'qywb64.dll')))
    files.append(('qydemo.exe', os.path.join(b, 'qydemo.exe' if arch == 'x64'
                                             else os.path.join('x86', 'qydemo.exe'))))
    files.append(('qyupd.exe', os.path.join(b, 'qyupd.exe' if arch == 'x64'
                                            else os.path.join('x86', 'qyupd.exe'))))
    return files


def main():
    arch = 'x64'
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == '--arch' and i + 1 < len(args):
            arch = args[i + 1]
    if arch not in ('x64', 'x86'):
        print('--arch 只支持 x64 / x86')
        return 1
    STUB_PATH = STUB[arch]
    OUT = out_name(arch)
    FILES = files_for(arch)
    if not os.path.exists(STUB_PATH):
        print(f'缺少 {STUB_PATH}，请先编译')
        return 1
    payload = bytearray()
    for name, path in FILES:
        if not os.path.exists(path):
            print(f'缺少文件: {path}')
            return 1
        data = open(path, 'rb').read()
        nb = name.encode('utf-8')
        payload += struct.pack('<H', len(nb))
        payload += nb
        payload += struct.pack('<II', len(data), 0)
        payload += data
        print(f'  打包 {name:16s} {len(data)/1024:8.1f} KB')

    stub = open(STUB_PATH, 'rb').read()
    offset = len(stub)
    out = stub + bytes(payload) + struct.pack('<8sQQ', b'QYWBPKG1', offset, len(payload))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, 'wb').write(out)
    print(f'生成安装程序({arch}): {OUT}  ({len(out)/1048576:.2f} MB)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
