#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""上传服务器前的文本文件编码体检。

为什么需要这个脚本
------------------
version.txt 会被用户用浏览器直接打开。nginx 对 .txt 只回
    Content-Type: text/plain
不带 charset。此时浏览器不看文件内容，按**操作系统默认编码**解码：
简体中文 Windows 的 Chrome/Edge 默认是 GBK，于是 UTF-8 的“秋雨五笔”
被解成“绉嬮洦浜旂瑪” —— 这就是反复出现的“网页打开 version.txt 是乱码”。

带**单个 UTF-8 BOM** 时，浏览器（Chrome/Edge/Firefox/IE）一定按 UTF-8 解码，
因为 BOM 的优先级高于默认编码。客户端 qy_update.cpp 的 DecodeText 会剥掉
所有前导 BOM，所以带 BOM 不影响程序读；而且文件本身已有 BOM 时，用户再用
记事本另存也不会叠出第二层 BOM（历史上就是这样导致过“检查更新失败”）。

规则（不满足即退出码 1，构建/上传前必须通过）
--------------------------------------------
1. dist/version.txt 与 dist/update/version.txt 必须字节完全一致；
2. 必须有且只有一个前导 BOM（EF BB BF），正文里不得再出现 BOM；
3. 正文必须是合法 UTF-8，且含 version= / url= / inc= 三行；
4. version= 必须等于 src/core/qy_version.h 里的 QY_VERSION_STR；
5. dist/index.html 必须声明 charset=utf-8（网页靠 meta 而不是 BOM）。
6. dist/使用说明.txt 同样必须单个 BOM（它会被记事本/浏览器直接打开，
   Win7 记事本不认无 BOM 的 UTF-8；qydemo / qysetup 读取时都会剥掉 BOM）。
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOM = b"\xef\xbb\xbf"

fails = []
notes = []


def fail(msg):
    fails.append(msg)


def rel(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")


def read(path):
    with open(path, "rb") as f:
        return f.read()


def check_version_file(path, label):
    b = read(path)
    if not b.startswith(BOM):
        fail("%s 缺少 UTF-8 BOM —— 浏览器会按 GBK 解码，中文必然乱码" % label)
        return None
    if b.count(BOM) != 1:
        fail("%s 出现 %d 个 BOM，必须恰好 1 个（叠层会导致客户端解析失败）"
             % (label, b.count(BOM)))
    body = b[len(BOM):]
    if BOM in body:
        fail("%s 正文中间还夹着 BOM 字节" % label)
    try:
        text = body.decode("utf-8")
    except UnicodeDecodeError as e:
        fail("%s 正文不是合法 UTF-8：%s" % (label, e))
        return None
    if body[:3] == b"\xef\xbb\xbf" or body.startswith(b"\xff\xfe"):
        fail("%s 正文开头又有一个 BOM" % label)
    kv = {}
    for ln in text.replace("\r\n", "\n").split("\n"):
        ln = ln.strip()
        if not ln or ln[0] in "#;[" or "=" not in ln:
            continue
        k, v = ln.split("=", 1)
        kv[k.strip().lower()] = v.strip()
    for k in ("version", "url", "inc"):
        if k not in kv:
            fail("%s 缺少 %s= 行" % (label, k))
    notes.append("%s: %d 字节，BOM×1，UTF-8，键=%s"
                 % (label, len(b), ",".join(sorted(kv)) if kv else "无"))
    return kv


def main():
    v1 = os.path.join(ROOT, "dist", "version.txt")
    v2 = os.path.join(ROOT, "dist", "update", "version.txt")
    for p in (v1, v2):
        if not os.path.exists(p):
            fail("缺少文件 %s" % rel(p))
    if fails:
        report()
        return 1

    if read(v1) != read(v2):
        fail("dist/version.txt 与 dist/update/version.txt 内容不一致（上传的是 update/ 下的那份）")

    kv = check_version_file(v2, "dist/update/version.txt")
    check_version_file(v1, "dist/version.txt")

    # 版本号必须与源码宏一致
    vh = os.path.join(ROOT, "src", "core", "qy_version.h")
    if os.path.exists(vh):
        m = re.search(r'QY_VERSION_STR\s+L"([^"]+)"',
                      io.open(vh, encoding="utf-8", errors="replace").read())
        if m and kv and "version" in kv and kv["version"] != m.group(1):
            fail("version.txt 里的 version=%s 与 qy_version.h 的 %s 不一致"
                 % (kv["version"], m.group(1)))
        elif m and kv:
            notes.append("版本号一致：%s" % m.group(1))

    # 清单与下载地址是否就位（只提示，不判失败，方便先看编码问题）
    if kv:
        inc = kv.get("inc", "").replace("\\", "/")
        if inc:
            p = os.path.join(ROOT, "dist", "update", inc)
            if not os.path.exists(p):
                fail("清单文件不存在：dist/update/%s（客户端会提示“服务器上还没有增量清单”）" % inc)
            else:
                notes.append("增量清单就位：dist/update/%s" % inc)
        url = kv.get("url", "")
        if url and not re.match(r"^[a-z]+://", url, re.I):
            name = url.replace("\\", "/").split("/")[-1]
            hit = [d for d in ("dist", "dist/update") if os.path.exists(os.path.join(ROOT, d, name))]
            if hit:
                notes.append("安装包就位：%s/%s" % (hit[0], name))
            else:
                fail("version.txt 的 url=%s 在 dist/ 下找不到对应文件" % name)

    # index.html 必须自己声明编码（网页不能依赖 BOM）
    idx = os.path.join(ROOT, "dist", "index.html")
    if os.path.exists(idx):
        head = io.open(idx, encoding="utf-8", errors="replace").read(2048)
        if not re.search(r'charset\s*=\s*["\']?utf-8', head, re.I):
            fail("dist/index.html 缺少 <meta charset=\"utf-8\">，网页可能乱码")
        else:
            notes.append("index.html 已声明 charset=utf-8")
        raw = read(idx)
        if raw.startswith(BOM):
            notes.append("提示：index.html 带 BOM（不影响浏览器，可不处理）")

    # 使用说明.txt：安装到程序目录后被记事本/浏览器打开，Win7 记事本不认
    # 无 BOM 的 UTF-8，同样必须带单个 BOM（读取方 qydemo / qysetup 都会剥掉）
    doc = os.path.join(ROOT, "dist", "使用说明.txt")
    if os.path.exists(doc):
        b = read(doc)
        if not b.startswith(BOM):
            fail("dist/使用说明.txt 缺少 UTF-8 BOM —— Win7 记事本打开会乱码")
        elif b.count(BOM) != 1:
            fail("dist/使用说明.txt 出现 %d 个 BOM，必须恰好 1 个" % b.count(BOM))
        else:
            try:
                b[3:].decode("utf-8")
                notes.append("dist/使用说明.txt: %d 字节，BOM×1，UTF-8" % len(b))
            except UnicodeDecodeError as e:
                fail("dist/使用说明.txt 正文不是合法 UTF-8：%s" % e)

    # 增量清单只给程序读，BOM 有无都行，只做记录
    for p in ("dist/update/inc/manifest.txt",):
        f = os.path.join(ROOT, p)
        if os.path.exists(f):
            b = read(f)
            notes.append("%s: %d 字节，BOM=%d（只给程序读，不要求）" % (p, len(b), b.count(BOM)))

    report()
    return 1 if fails else 0


def report():
    print("== 服务器文本编码体检 ==")
    for n in notes:
        print("  [OK] " + n)
    if not notes:
        print("  （无可检查项）")
    if fails:
        print("\n== 失败项 %d ==" % len(fails))
        for f in fails:
            print("  [FAIL] " + f)
    else:
        print("\n全部检查通过：version.txt 为 UTF-8 + 单个 BOM，浏览器打开不会乱码。")


if __name__ == "__main__":
    sys.exit(main())
