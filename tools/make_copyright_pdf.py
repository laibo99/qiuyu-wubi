# -*- coding: utf-8 -*-
"""生成软件著作权登记所需的鉴别材料 PDF。

产出两份互相独立的 PDF（放在 dist/软著申请材料/）：
  1) 源程序鉴别材料：前 30 页 + 后 30 页，共 60 页，每页 50 行；
  2) 文档鉴别材料：使用说明书全文（不足 60 页时全部提交），每页 >=30 行。

两份都带页眉（左：软件名称 + 版本号；右：右上角页码）。
"""

import os
import sys

from reportlab.lib.pagesizes import A4
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.pdfgen import canvas

SOFT_NAME = "秋雨五笔输入法"
SOFT_VER = "V4.5.3"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "dist", "软著申请材料")

FONT_DIR = "C:/Windows/Fonts"
SONG = "QySong"      # 宋体：正文
HEI = "QyHei"        # 黑体：标题

PW, PH = A4

# 版面
ML, MR = 56.0, 56.0          # 左右页边距
HEAD_Y = PH - 32.0           # 页眉基线
LINE_Y = PH - 44.0           # 页眉分隔线
BODY_TOP = PH - 62.0         # 正文首行基线
TEXT_W = PW - ML - MR        # 正文可用宽度

CODE_SIZE = 9.0
CODE_LEAD = 14.6
CODE_LINES = 50              # 每页行数

DOC_SIZE = 10.5
DOC_LEAD = 20.2
DOC_LINES = 35

# 源程序：按产品代码的逻辑顺序排列（入口 -> 核心 -> 界面 -> 安装/更新）
SRC_FILES = [
    "src/ime/qy_dll.cpp",
    "src/ime/qy_ime.cpp",
    "src/core/qy_engine.cpp",
    "src/core/qy_dict.cpp",
    "src/core/qy_candui.cpp",
    "src/core/qy_symui.cpp",
    "src/core/qy_statusbar.cpp",
    "src/core/qy_config.cpp",
    "src/core/qy_update.cpp",
    "src/tools/qydemo.cpp",
    "src/tools/qysetup.cpp",
    "src/tools/qyupd.cpp",
]

DOC_FILE = "dist/使用说明.txt"

_missing = set()


def register_fonts():
    pdfmetrics.registerFont(TTFont(SONG, os.path.join(FONT_DIR, "simsun.ttc"), subfontIndex=0))
    pdfmetrics.registerFont(TTFont(HEI, os.path.join(FONT_DIR, "simhei.ttf")))


_wcache = {}


def cw(ch, font, size):
    """单字符宽度（带缓存）。"""
    key = (font, size, ch)
    w = _wcache.get(key)
    if w is None:
        w = pdfmetrics.stringWidth(ch, font, size)
        if w <= 0 and not ch.isspace():
            _missing.add(ch)
            w = pdfmetrics.stringWidth("?", font, size)
        _wcache[key] = w
    return w


def wrap(text, font, size, width):
    """按可用宽度折行，中文按字符断行。"""
    out = []
    cur = ""
    curw = 0.0
    for ch in text:
        w = cw(ch, font, size)
        if curw + w > width and cur:
            out.append(cur)
            cur, curw = "", 0.0
        cur += ch
        curw += w
    out.append(cur)
    return out


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    for enc in ("utf-8-sig", "utf-8", "gbk"):
        try:
            return raw.decode(enc).replace("\r\n", "\n").replace("\r", "\n")
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def draw_header(c, page, total, sub=""):
    """页眉：左软件名版本，右上角页码。"""
    c.setFillColorRGB(0.15, 0.15, 0.15)
    c.setFont(SONG, 8.8)
    left = "%s  %s" % (SOFT_NAME, SOFT_VER)
    c.drawString(ML, HEAD_Y, left)
    if sub:
        c.setFont(SONG, 7.6)
        c.setFillColorRGB(0.45, 0.45, 0.45)
        w = pdfmetrics.stringWidth(left, SONG, 8.8)
        c.drawString(ML + w + 8, HEAD_Y, sub)
        c.setFillColorRGB(0.15, 0.15, 0.15)
    c.setFont(SONG, 8.8)
    right = "第 %d 页 / 共 %d 页" % (page, total)
    c.drawRightString(PW - MR, HEAD_Y, right)
    c.setStrokeColorRGB(0.62, 0.62, 0.62)
    c.setLineWidth(0.6)
    c.line(ML, LINE_Y, PW - MR, LINE_Y)


# ---------------------------------------------------------------- 源程序 PDF

def collect_code_lines():
    """把产品源码拼成视觉行序列（Tab 展开、去空行、超宽折行）。

    去掉空行是为了保证每页 50 行全部为有效代码行（软著对每页行数有硬要求），
    每个源文件前插一行路径注释，便于审查时辨认文件边界。
    """
    lines = []
    for rel in SRC_FILES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print("  ! 缺少源文件：%s" % rel)
            continue
        lines.append("/* ==================== %s ==================== */" % rel)
        text = read_text(path)
        for raw in text.split("\n"):
            s = raw.replace("\t", "    ").rstrip()
            if not s.strip():
                continue
            for seg in wrap(s, SONG, CODE_SIZE, TEXT_W):
                lines.append(seg)
    return lines


def build_source_pdf(out_path, pages_front=30, pages_back=30):
    lines = collect_code_lines()
    total_pages = pages_front + pages_back
    need = total_pages * CODE_LINES
    if len(lines) < need:
        print("  ! 源程序可视行仅 %d 行，不足 %d 行" % (len(lines), need))
        return None
    picked = lines[: pages_front * CODE_LINES] + lines[-pages_back * CODE_LINES:]

    c = canvas.Canvas(out_path, pagesize=A4)
    c.setTitle("%s %s 源程序" % (SOFT_NAME, SOFT_VER))
    c.setAuthor(SOFT_NAME)
    c.setSubject("软件著作权登记 源程序鉴别材料（前30页+后30页）")

    idx = 0
    for p in range(1, total_pages + 1):
        sub = "源程序（前 30 页）" if p <= pages_front else "源程序（后 30 页）"
        draw_header(c, p, total_pages, sub)
        c.setFillColorRGB(0, 0, 0)
        c.setFont(SONG, CODE_SIZE)
        y = BODY_TOP
        for _ in range(CODE_LINES):
            s = picked[idx] if idx < len(picked) else ""
            idx += 1
            if s:
                c.drawString(ML, y, s)
            y -= CODE_LEAD
        c.showPage()
    c.save()

    # 源码规范化后的比例（供说明用）
    return {"pages": total_pages, "lines_used": len(picked), "lines_total": len(lines)}


# ---------------------------------------------------------------- 文档 PDF

TITLE_PREFIX = ("一、", "二、", "三、", "四、", "五、", "六、", "七、", "八、", "九、")


def build_doc_pdf(out_path):
    text = read_text(os.path.join(ROOT, DOC_FILE))
    raw = text.split("\n")

    # 首行当大标题，等号分隔行丢弃
    title = raw[0].strip() if raw else "%s %s 使用说明书" % (SOFT_NAME, SOFT_VER)
    body = [l for l in raw[1:] if not set(l.strip()) <= {"="} or not l.strip()]

    # 组织成 (类型, 文本) 视觉行：空行、标题、正文
    visual = []
    for line in body:
        s = line.replace("\t", "    ").rstrip()
        if not s.strip():
            if visual and visual[-1][0] != "blank":
                visual.append(("blank", ""))
            continue
        is_head = s.lstrip().startswith(TITLE_PREFIX) and len(s.strip()) <= 20
        if is_head:
            if visual and visual[-1][0] != "blank":
                visual.append(("blank", ""))
            visual.extend(("head", seg) for seg in wrap(s.strip(), HEI, 12.5, TEXT_W))
            visual.append(("blank", ""))
            continue
        # 标题下方的等号行、纯符号装饰行丢弃
        if set(s.strip()) <= set("=-·—"):
            continue
        visual.extend(("body", seg) for seg in wrap(s, SONG, DOC_SIZE, TEXT_W))

    # 去掉尾部空行
    while visual and visual[-1][0] == "blank":
        visual.pop()

    total_pages = max(1, (len(visual) + DOC_LINES - 1) // DOC_LINES)

    c = canvas.Canvas(out_path, pagesize=A4)
    c.setTitle("%s %s 使用说明书" % (SOFT_NAME, SOFT_VER))
    c.setAuthor(SOFT_NAME)
    c.setSubject("软件著作权登记 文档鉴别材料")

    # 第 1 页顶部放文档标题
    idx = 0
    for p in range(1, total_pages + 1):
        draw_header(c, p, total_pages, "文档鉴别材料（使用说明书）")
        y = BODY_TOP
        if p == 1:
            c.setFillColorRGB(0, 0, 0)
            c.setFont(HEI, 16)
            c.drawString(ML, y, title)
            y -= 30
        for _ in range(DOC_LINES):
            if idx >= len(visual):
                break
            kind, s = visual[idx]
            idx += 1
            if kind == "head":
                c.setFont(HEI, 12.5)
                c.setFillColorRGB(0, 0, 0)
                c.drawString(ML, y, s)
            elif kind == "body":
                c.setFont(SONG, DOC_SIZE)
                c.setFillColorRGB(0, 0, 0)
                c.drawString(ML, y, s)
            y -= DOC_LEAD
        c.showPage()
    c.save()
    return {"pages": total_pages, "lines": len(visual)}


def main():
    register_fonts()
    os.makedirs(OUT_DIR, exist_ok=True)

    src_pdf = os.path.join(OUT_DIR, "%s%s_源程序鉴别材料_前30页后30页.pdf" % (SOFT_NAME, SOFT_VER))
    doc_pdf = os.path.join(OUT_DIR, "%s%s_文档鉴别材料_使用说明书.pdf" % (SOFT_NAME, SOFT_VER))

    print("[1/2] 源程序材料 ...")
    a = build_source_pdf(src_pdf)

    print("[2/2] 文档材料 ...")
    b = build_doc_pdf(doc_pdf)

    if _missing:
        print("  ! 字体缺字（已用 ? 替代）：%s" % "".join(sorted(_missing)))
    for path, info in ((src_pdf, a), (doc_pdf, b)):
        if info and os.path.exists(path):
            print("  OK %s  %.1f KB  页数=%d" % (os.path.basename(path),
                                                os.path.getsize(path) / 1024.0, info["pages"]))
    return 0 if (a and b) else 1


if __name__ == "__main__":
    sys.exit(main())
