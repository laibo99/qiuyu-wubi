#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""秋雨五笔 - 码表编译器
把 rime 五笔86 码表 (wubi86.dict.yaml) + 汉字拼音数据 (pinyin.txt)
编译成引擎可直接 mmap 载入的二进制 qywb.dat

输出格式 (little-endian):
  header 64B:
    char    magic[8]   = "QYWBDIC1"
    u32     version
    u32     count
    u32     wcharOff, wcharLen   (UTF-16 code units)
    u32     charOff,  charLen    (ansi bytes: 拼音音节, 用 '\'' 分隔)
    u32     codeOff,  codeLen    (ansi bytes: 五笔编码字母)
    u32     codeIdxOff, pyIdxOff, abbrIdxOff, textIdxOff
  rec[count] 20B:
    u32 textOff; u32 pyOff; u32 codeOff; u16 textLen; u8 codeLen; u8 sylCount; u32 weight
  索引: u32 数组
"""
import os
import re
import sys
import struct
import unicodedata
from collections import defaultdict

BASE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(BASE, '..', 'data')
OUT = os.path.join(BASE, '..', 'data', 'qywb.dat')

CODE_RE = re.compile(r'^[a-y]{1,4}$')


def strip_tone(py: str) -> str:
    """qiū -> qiu ; lǜ -> lv"""
    py = py.strip().lower()
    out = []
    for ch in py:
        if ch in 'üǖǘǚǜ':
            out.append('v')
            continue
        d = unicodedata.normalize('NFD', ch)
        d = ''.join(c for c in d if unicodedata.category(c) != 'Mn')
        if d in ('ü',):
            d = 'v'
        out.append(d)
    s = ''.join(out)
    s = s.replace('ü', 'v').replace('ê', 'e')
    return s if re.fullmatch(r'[a-z]{1,6}', s) else ''


def load_pinyin():
    table = {}
    path = os.path.join(DATA, 'pinyin.txt')
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            m = re.match(r'^U\+([0-9A-Fa-f]+):\s*([^#]+)#', line)
            if not m:
                continue
            cp = int(m.group(1), 16)
            if not (0x3400 <= cp <= 0x9FFF or 0x20000 <= cp <= 0x2FA1F or cp == 0x3007):
                continue
            ch = chr(cp)
            if ch in table:
                continue
            readings = [strip_tone(x) for x in m.group(2).split(',')]
            readings = [r for r in readings if r]
            if readings:
                # 首选读音：优先非生僻(排除少见读音顺序已由数据给出)
                table[ch] = readings
    return table


def load_wubi():
    """返回 [(text, code, weight)]，含简码与全码"""
    path = os.path.join(DATA, 'wubi86.dict.yaml')
    entries = []
    started = False
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('...'):
                started = True
                continue
            if not started:
                continue
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) < 3:
                continue
            text, code, weight = parts[0], parts[1], parts[2]
            if not CODE_RE.match(code):
                continue
            if not text or len(text) > 8:
                continue
            if any(not ('\u3400' <= c <= '\u9fff' or c in '〇〇') for c in text):
                # 允许少量非汉字（如字母词）跳过全部非汉字词
                continue
            try:
                w = int(weight)
            except ValueError:
                w = 1000
            entries.append((text, code, w))
            if len(parts) >= 4:
                stem = parts[3].strip()
                if CODE_RE.match(stem) and stem != code:
                    entries.append((text, stem, max(1, w // 3)))
    return entries


def load_networds():
    """自定义词条（网络用语 / APP 名称 / 网络高频词）

    文件 data/networds.txt 由 tools/gen_networds.py 生成，格式：
        词 <TAB> 编码 <TAB> 权重 <TAB> 说明
    这些词条会同时进入五笔索引与拼音索引：既能用自定义码（如 tmwy→微信），
    也能用拼音/简拼（如 wx→微信、yyds→永远的神）。
    """
    path = os.path.join(DATA, 'networds.txt')
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) < 3:
                continue
            text, code = parts[0].strip(), parts[1].strip()
            if not text or not re.match(r'^[a-z]{1,4}$', code):
                continue
            if len(text) > 8:
                continue
            if any(not ('\u3400' <= c <= '\u9fff' or c in '〇〇') for c in text):
                continue
            try:
                w = int(parts[2])
            except ValueError:
                w = 1000
            out.append((text, code, w))
    return out


def main():
    pinyin = load_pinyin()
    raw = load_wubi()
    custom = load_networds()
    print(f'拼音表 {len(pinyin)} 字, 五笔原始条目 {len(raw)}, 自定义词条 {len(custom)}')

    # 去重: (text, code) 取最大权重；stem 条目单独标记
    # （stem = 码表第 4 列的编码词根，如“国 l ... lg”——它是给“l”字的编码提示，
    #   不能压过真实二级简码，否则打 lg 会先出“国”而不是“车”）
    best = {}
    stems = set()
    for text, code, w in raw:
        k = (text, code)
        if k not in best or best[k] < w:
            best[k] = w

    # 自定义词条并入（权重很高，同码时排最前）
    for text, code, w in custom:
        k = (text, code)
        if k not in best or best[k] < w:
            best[k] = w
    print(f'并入自定义词条 {len(custom)} 条，总条目 {len(best)}')

    # 重新扫描一遍，把 stem 对 (text,code) 记下来
    started2 = False
    with open(os.path.join(DATA, 'wubi86.dict.yaml'), encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('...'):
                started2 = True
                continue
            if not started2 or not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) >= 4:
                stem = parts[3].strip()
                if CODE_RE.match(stem) and stem != parts[1]:
                    stems.add((parts[0], stem))

    # 撞码处理：一级简码字往往带一个“词根提示码”（如 一 g ... gg、上 h ... hh、
    # 国 l ... lg），而这个两码其实是别的字的二级简码（五 GG、止 HH、车 LG）。
    # 若保留提示码，同码下它会压过真正的二级简码，导致打 gg 先出“一”。
    # 规则：该码已有真实条目时，直接丢弃提示码；只有该码没有真实条目时才保留。
    max_non_stem = {}
    for (text, code), w in best.items():
        if (text, code) not in stems:
            if max_non_stem.get(code, 0) < w:
                max_non_stem[code] = w
    dropped = 0
    for k in list(best.keys()):
        if k in stems and k[1] in max_non_stem:
            del best[k]
            dropped += 1
    if dropped:
        print(f'丢弃撞码的词根提示条目 {dropped} 条（同码已有真实简码）')

    # 生成记录
    text_arena = []
    char_arena = bytearray()
    code_arena = bytearray()
    text_off = {}
    recs = []

    # 按 text 分组，选取"主记录"用于拼音索引（权重最高者）
    per_text = defaultdict(list)

    def intern_text(t):
        if t not in text_off:
            text_off[t] = len(text_arena)
            text_arena.extend(t)
        return text_off[t]

    for (text, code), w in best.items():
        py_syl = []
        ok = True
        for ch in text:
            rs = pinyin.get(ch)
            if not rs:
                ok = False
                break
            py_syl.append(rs[0])
        py = "'".join(py_syl) if ok and py_syl else ''
        py_off = len(char_arena)
        if py:
            # 末尾追加分隔符，便于引擎按音节数扫描边界
            char_arena.extend((py + "'").encode('ascii'))
        code_off = len(code_arena)
        code_arena.extend(code.encode('ascii'))
        toff = intern_text(text)
        rec = [toff, py_off, code_off, len(text), len(code), len(py_syl) if py else 0, min(w, 0xFFFFFFFF)]
        idx = len(recs)
        recs.append((rec, text, code, py, w))
        per_text[text].append(idx)

    print(f'记录数 {len(recs)}, 文本 arena {len(text_arena)}, 拼音 arena {len(char_arena)}')

    # 索引
    def compact(py):
        return py.replace("'", '')

    code_idx = sorted(range(len(recs)), key=lambda i: (recs[i][2], -recs[i][4]))
    py_idx = sorted(range(len(recs)), key=lambda i: (compact(recs[i][3]), -recs[i][4]))
    # 拼音索引去重：同 text 只保留权重最高的一条
    seen = set()
    py_idx_dedup = []
    for i in py_idx:
        t = recs[i][1]
        if not recs[i][3]:
            continue
        if t in seen:
            continue
        seen.add(t)
        py_idx_dedup.append(i)
    abbr_idx = sorted(py_idx_dedup,
                      key=lambda i: (''.join(s[0] for s in recs[i][3].split("'")), -recs[i][4]))
    text_idx = sorted(range(len(recs)), key=lambda i: (recs[i][1], -recs[i][4]))

    # 组装
    HEADER = 72
    rec_size = 20
    rec_off = HEADER
    wchar_off = rec_off + rec_size * len(recs)
    wchar_bytes = len(text_arena) * 2
    char_off = wchar_off + wchar_bytes
    char_bytes = len(char_arena)
    code_off = char_off + char_bytes
    code_bytes = len(code_arena)
    code_idx_off = code_off + code_bytes
    py_idx_off = code_idx_off + 4 * len(code_idx)
    abbr_idx_off = py_idx_off + 4 * len(py_idx_dedup)
    text_idx_off = abbr_idx_off + 4 * len(abbr_idx)
    total = text_idx_off + 4 * len(text_idx)

    buf = bytearray(total)
    buf[0:8] = b'QYWBDIC1'
    struct.pack_into('<16I', buf, 8, 1, len(recs), wchar_off, len(text_arena),
                     char_off, char_bytes, code_off, code_bytes,
                     code_idx_off, py_idx_off, len(py_idx_dedup),
                     abbr_idx_off, len(abbr_idx), text_idx_off, len(text_idx), 0)

    p = rec_off
    for rec, _, _, _, _ in recs:
        struct.pack_into('<IIIHBBI', buf, p, rec[0], rec[1], rec[2], rec[3], rec[4], rec[5], rec[6])
        p += rec_size

    struct.pack_into(f'<{len(text_arena)}H', buf, wchar_off, *[ord(c) for c in text_arena])
    buf[char_off:char_off + char_bytes] = char_arena
    buf[code_off:code_off + code_bytes] = code_arena
    for off, arr in ((code_idx_off, code_idx), (py_idx_off, py_idx_dedup),
                     (abbr_idx_off, abbr_idx), (text_idx_off, text_idx)):
        struct.pack_into(f'<{len(arr)}I', buf, off, *arr)

    with open(OUT, 'wb') as f:
        f.write(buf)
    print(f'写出 {OUT}  {total/1048576:.2f} MB')
    print(f'五笔索引 {len(code_idx)}, 拼音索引 {len(py_idx_dedup)}, 简拼索引 {len(abbr_idx)}')

    # 抽样自检
    def lookup(code):
        hits = [(recs[i][1], recs[i][2], recs[i][4]) for i in code_idx if recs[i][2] == code]
        return hits[:5]
    for c in ('q', 'aaa', 'qiuyu'):
        pass
    print('样例 aaa ->', lookup('aaa'))
    print('样例 gkld ->', lookup('gkld'))


if __name__ == '__main__':
    main()
