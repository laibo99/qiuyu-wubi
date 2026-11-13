# -*- coding: utf-8 -*-
"""一次性清理 dist/update/inc/files：只保留当前清单引用的 bin。
被引用但本地缺失的（未变动文件）从安装目录补齐（先校验指纹）。"""
import os, zlib, io, shutil

man = 'dist/update/inc/manifest.txt'
files_dir = 'dist/update/inc/files'
inst = r'C:\Program Files\QiuYuWubi'
BSLASH = chr(92)  # 反斜杠，避免脚本转义问题


def crc(p):
    c = 0
    with open(p, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            c = zlib.crc32(b, c)
    return c & 0xFFFFFFFF


refs = {}  # bin名 -> (name, size, crc)
for line in io.open(man, encoding='utf-8-sig'):
    line = line.strip()
    if not line.startswith('file='):
        continue
    parts = line[5:].split('|')
    name, size, c, url = parts[0], int(parts[1]), parts[2], parts[3]
    refs[os.path.basename(url)] = (name, size, int(c, 16))

removed = 0
for bin in sorted(os.listdir(files_dir)):
    p = os.path.join(files_dir, bin)
    if bin in refs:
        continue
    os.remove(p)
    removed += 1

# 补齐被引用但本地缺失的 bin（从未变动的文件，从安装目录取，先校验指纹）
fixed = []
for bin, (name, size, c) in refs.items():
    p = os.path.join(files_dir, bin)
    if os.path.exists(p):
        continue
    src = os.path.join(inst, name.replace(BSLASH, os.sep))
    a = (os.path.getsize(src), crc(src))
    assert a == (size, c), '指纹不符 %s: %r != %r' % (name, a, (size, c))
    shutil.copyfile(src, p)
    fixed.append('%s <- %s (%d B)' % (bin, name, size))

print('删除过期 %d 个，补齐 %d 个：' % (removed, len(fixed)))
for m in fixed:
    print('  +', m)
total = sum(os.path.getsize(os.path.join(files_dir, f)) for f in os.listdir(files_dir))
print('目录现状：%d 个文件，共 %.2f MB' % (len(os.listdir(files_dir)), total / 1048576))
