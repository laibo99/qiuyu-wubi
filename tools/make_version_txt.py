#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成服务器用的 version.txt（自动带 UTF-8 BOM，双份同步）。

为什么要有这个脚本
------------------
手写 version.txt 最大的坑是编码：用普通 `open(..., 'w', encoding='utf-8')`
写出来是**无 BOM** 的 UTF-8，浏览器打开就是乱码（nginx 不带 charset，
浏览器按 GBK 猜）。这个脚本固定写成“单个 BOM + UTF-8 正文”，
并同时更新 dist/version.txt 与 dist/update/version.txt 两份，
最后自动跑一遍 tools/check_dist_text.py。

用法：
    python tools/make_version_txt.py --url 秋雨五笔输入法_4.5_Setup.exe \
        --notes "v4.5：xxx" [--version 4.5.0] [--inc inc/manifest.txt]

不带 --version 时从 src/core/qy_version.h 的 QY_VERSION_STR 读取。
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOM = b"\xef\xbb\xbf"


def header_version():
    p = os.path.join(ROOT, "src", "core", "qy_version.h")
    with open(p, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    m = re.search(r'QY_VERSION_STR\s+L"([^"]+)"', text)
    if not m:
        sys.exit("读不到 qy_version.h 里的 QY_VERSION_STR")
    return m.group(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=None, help="默认取 qy_version.h")
    ap.add_argument("--url", required=True, help="安装包文件名（相对 update/ 目录）")
    ap.add_argument("--inc", default="inc/manifest.txt")
    ap.add_argument("--notes", default="")
    a = ap.parse_args()

    ver = a.version or header_version()
    for label, val in (("version", ver), ("url", a.url), ("inc", a.inc)):
        if "\n" in val or "\r" in val:
            sys.exit("%s 里不能有换行" % label)

    lines = ["version=" + ver, "url=" + a.url]
    if a.inc:
        lines.append("inc=" + a.inc)
    if a.notes:
        lines.append("notes=" + a.notes)
    body = ("\n".join(lines) + "\n").encode("utf-8")

    out = BOM + body
    targets = [os.path.join(ROOT, "dist", "version.txt"),
               os.path.join(ROOT, "dist", "update", "version.txt")]
    for t in targets:
        os.makedirs(os.path.dirname(t), exist_ok=True)
        with open(t, "wb") as f:
            f.write(out)
    print("已写入 %d 字节（UTF-8 + 单 BOM）到：" % len(out))
    for t in targets:
        print("  " + os.path.relpath(t, ROOT).replace("\\", "/"))

    rc = subprocess.call([sys.executable, os.path.join(ROOT, "tools", "check_dist_text.py")])
    return rc


if __name__ == "__main__":
    sys.exit(main())
