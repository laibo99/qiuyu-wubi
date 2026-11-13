#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""给 PE 文件（EXE/DLL）注入图标 + 版本信息资源。

环境里没有 windres / rc.exe，zig rc 在本机不可用，因此直接构造 .rsrc 节并
追加到 PE：新增节头 -> 更新数据目录[2] -> 写资源目录树。

为什么要有版本信息：360 等安全软件对“没有公司名/版本号、却会写注册表、
会自删除的 exe”非常敏感，会把安装/卸载程序判成高危。补齐 VS_VERSIONINFO
（公司、产品、文件说明、原始文件名、版权）后，它们才像正常软件。

用法：
  python tools/seticon.py <目标.exe> <图标.ico> [组图标ID，默认101] \
      [--ver 4.0.0] [--desc "文件说明"] [--name 内部名] [--dll]
  python tools/seticon.py --verify <目标.exe> ...   # 校验图标/版本资源

可重复执行：已有 .rsrc 节且空间足够时原地覆盖内容；不够且它是最后一个节
就原地扩展，不会叠加第二个 .rsrc 节。
"""
import struct
import sys
import os

RT_ICON = 3
RT_GROUP_ICON = 14
RT_VERSION = 16
LANG_ZH_CN = 0x0804
CODEPAGE_UNICODE = 1200  # 0x04b0


def zero_timestamp(pe):
    """PE 头的 TimeDateStamp 每次链接都会变。

    它会让“一行代码都没改的程序”也算成新文件，增量更新就退化成全量下载；
    统一清零后同一份源码每次构建出完全相同的字节，增量清单才有意义。
    """
    struct.pack_into('<I', pe.d, pe.e_lfanew + 8, 0)
    # 调试目录（IMAGE_DEBUG_DIRECTORY）里还有一份同样会变的时间戳，一起清零
    rva, size = pe.get_dd(6)
    if rva and size:
        for sec in pe.sections():
            span = max(sec['vs'], sec['rs'])
            if sec['va'] <= rva < sec['va'] + span:
                off = sec['ra'] + (rva - sec['va'])
                for k in range(size // 28):
                    struct.pack_into('<I', pe.d, off + k * 28 + 4, 0)
                break
    # 导出目录的 TimeDateStamp（+4）也会被链接器写入当前时间，msvcrt 工具链
    # 生成的 DLL 都带导出表，不清零则每次构建 CRC 都不同。
    # （资源目录与导入描述符的时间戳实测本来就为 0，无需处理）
    rva, size = pe.get_dd(0)
    if rva and size:
        for sec in pe.sections():
            span = max(sec['vs'], sec['rs'])
            if sec['va'] <= rva < sec['va'] + span:
                off = sec['ra'] + (rva - sec['va'])
                struct.pack_into('<I', pe.d, off + 4, 0)
                break


def fix_checksum(pe):
    """重算 PE 校验和（CheckSum @ OptionalHeader+64）。

    链接器算校验和时导出表时间戳还没清零，值会跟着时间漂移；
    时间戳清零后按标准算法重算，校验和就稳定了。
    """
    d = pe.d
    csum_off = pe.opt + 64
    struct.pack_into('<I', d, csum_off, 0)
    n = len(d)
    total = 0
    for i in range(0, n - 1, 2):
        w = d[i] | (d[i + 1] << 8)
        total += w
        total = (total & 0xFFFF) + (total >> 16)
    if n % 2:
        total += d[n - 1]
        total = (total & 0xFFFF) + (total >> 16)
    total = (total + (total >> 16)) & 0xFFFF
    total = (total + n) & 0xFFFFFFFF
    struct.pack_into('<I', d, csum_off, total)


def align_up(v, a):
    return (v + a - 1) // a * a


class PE:
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        self.e_lfanew = struct.unpack_from('<I', d, 0x3c)[0]
        pe = self.e_lfanew
        assert d[pe:pe + 4] == b'PE\0\0', '不是有效的 PE 文件'
        self.nsec = struct.unpack_from('<H', d, pe + 6)[0]
        self.optsz = struct.unpack_from('<H', d, pe + 20)[0]
        self.opt = pe + 24
        self.magic = struct.unpack_from('<H', d, self.opt)[0]
        self.secalign = struct.unpack_from('<I', d, self.opt + 32)[0]
        self.filealign = struct.unpack_from('<I', d, self.opt + 36)[0]
        self.sizeimg = struct.unpack_from('<I', d, self.opt + 56)[0]
        self.sizehdr = struct.unpack_from('<I', d, self.opt + 60)[0]
        self.dd = self.opt + (96 if self.magic == 0x10b else 112)
        self.sec_tbl = self.opt + self.optsz

    def sections(self):
        out = []
        for i in range(self.nsec):
            o = self.sec_tbl + i * 40
            name = bytes(self.d[o:o + 8]).rstrip(b'\0').decode('latin1')
            vs, va, rs, ra = struct.unpack_from('<IIII', self.d, o + 8)
            ch = struct.unpack_from('<I', self.d, o + 36)[0]
            out.append(dict(idx=i, off=o, name=name, vs=vs, va=va, rs=rs, ra=ra, ch=ch))
        return out

    def find(self, name):
        for s in self.sections():
            if s['name'] == name:
                return s
        return None

    def set_dd(self, index, rva, size):
        struct.pack_into('<II', self.d, self.dd + index * 8, rva, size)

    def get_dd(self, index):
        return struct.unpack_from('<II', self.d, self.dd + index * 8)


# ---------------------------------------------------------------------------
# VS_VERSIONINFO
# ---------------------------------------------------------------------------
def _node(key, value, wtype, children, vlen=None):
    """构造一个版本信息块。value 为 None 表示没有 Value 字段。"""
    out = bytearray(struct.pack('<HHH', 0, 0, wtype))
    out += key.encode('utf-16-le') + b'\0\0'
    while len(out) % 4:
        out += b'\0'
    if value is not None:
        out += value
        while len(out) % 4:
            out += b'\0'
    for c in children:
        while len(out) % 4:
            out += b'\0'
        out += c
    if vlen is None:
        vlen = len(value) if value is not None else 0
    struct.pack_into('<HH', out, 0, len(out), vlen)
    return bytes(out)


def _str_node(name, text):
    val = text.encode('utf-16-le') + b'\0\0'
    return _node(name, val, 1, [], vlen=len(text) + 1)


def build_version(ver, desc, internal, original, is_dll=False):
    parts = (ver.split('.') + ['0', '0', '0', '0'])[:4]
    nums = [int(x) if x.isdigit() else 0 for x in parts]
    ms = (nums[0] << 16) | nums[1]
    ls = (nums[2] << 16) | nums[3]
    fv = '.'.join(str(n) for n in nums)
    fixed = struct.pack(
        '<13I',
        0xFEEF04BD, 0x00010000,      # signature, struct version
        ms, ls,                       # file version
        ms, ls,                       # product version
        0x3F, 0,                      # flags mask, flags
        0x40004,                      # VOS_NT_WINDOWS32
        2 if is_dll else 1,           # VFT_DLL / VFT_APP
        0, 0, 0)
    strings = [
        ('CompanyName', '秋雨'),
        ('FileDescription', desc),
        ('FileVersion', fv),
        ('InternalName', internal),
        ('LegalCopyright', 'Copyright (C) 2026 秋雨'),
        ('OriginalFilename', original),
        ('ProductName', '秋雨五笔输入法'),
        ('ProductVersion', ver),
    ]
    table = _node('%04x%04x' % (LANG_ZH_CN, CODEPAGE_UNICODE), None, 1,
                  [_str_node(k, v) for k, v in strings])
    sfi = _node('StringFileInfo', None, 1, [table])
    var = _node('Translation', struct.pack('<HH', LANG_ZH_CN, CODEPAGE_UNICODE), 0, [], vlen=4)
    vfi = _node('VarFileInfo', None, 1, [var])
    return _node('VS_VERSION_INFO', fixed, 0, [sfi, vfi], vlen=len(fixed))


# ---------------------------------------------------------------------------
# 资源节
# ---------------------------------------------------------------------------
def build_rsrc(ico_path, group_id, ver_blob=None):
    """返回 (资源节数据, 数据条目区起始偏移, 叶子条目数)"""
    ico = open(ico_path, 'rb').read()
    res, typ, cnt = struct.unpack_from('<HHH', ico, 0)
    assert res == 0 and typ == 1, '不是有效的 ICO 文件'
    entries = []
    for i in range(cnt):
        w, h, cc, rsv, planes, bpp, size, off = struct.unpack_from('<BBBBHHII', ico, 6 + i * 16)
        entries.append(dict(w=w, h=h, cc=cc, planes=planes, bpp=bpp, size=size,
                            data=ico[off:off + size]))
    n_icon = len(entries)

    grp = struct.pack('<HHH', 0, 1, n_icon)
    for i, e in enumerate(entries):
        grp += struct.pack('<BBBBHHIH', e['w'], e['h'], e['cc'], 0, e['planes'] or 1,
                           e['bpp'], e['size'], i + 1)

    # (type_id, [(id, data), ...])，按类型号升序排列（资源目录要求有序）
    types = [(RT_ICON, [(i + 1, entries[i]['data']) for i in range(n_icon)]),
             (RT_GROUP_ICON, [(group_id, grp)])]
    if ver_blob:
        types.append((RT_VERSION, [(1, ver_blob)]))
    types.sort(key=lambda t: t[0])

    # ---- 布局 ----
    pos = 16 + 8 * len(types)
    lvl2 = []
    for _tid, items in types:
        lvl2.append(pos)
        pos += 16 + 8 * len(items)
    lvl3 = []
    for _tid, items in types:
        row = []
        for _ in items:
            row.append(pos)
            pos += 16 + 8
        lvl3.append(row)
    de_off = pos
    n_leaf = sum(len(items) for _tid, items in types)
    pos += 16 * n_leaf
    pos = align_up(pos, 4)

    blob = {}
    for ti, (_tid, items) in enumerate(types):
        for ii, (_iid, data) in enumerate(items):
            blob[(ti, ii)] = pos
            pos += len(data)
            pos = align_up(pos, 4)
    buf = bytearray(pos)

    de_idx = {}
    cur = de_off
    for ti, (_tid, items) in enumerate(types):
        for ii in range(len(items)):
            de_idx[(ti, ii)] = cur
            cur += 16

    def write_dir(offset, items):
        struct.pack_into('<IIHHHH', buf, offset, 0, 0, 0, 0, 0, len(items))
        for k, (rid, target, isdir) in enumerate(items):
            struct.pack_into('<II', buf, offset + 16 + k * 8, rid & 0x7FFFFFFF,
                             (target | 0x80000000) if isdir else target)

    write_dir(0, [(tid, lvl2[ti], True) for ti, (tid, _i) in enumerate(types)])
    for ti, (_tid, items) in enumerate(types):
        write_dir(lvl2[ti], [(iid, lvl3[ti][ii], True) for ii, (iid, _d) in enumerate(items)])
        for ii, (iid, _d) in enumerate(items):
            write_dir(lvl3[ti][ii], [(LANG_ZH_CN, de_idx[(ti, ii)], False)])

    for ti, (_tid, items) in enumerate(types):
        for ii, (_iid, data) in enumerate(items):
            struct.pack_into('<IIII', buf, de_idx[(ti, ii)], blob[(ti, ii)], len(data), 0, 0)
            buf[blob[(ti, ii)]:blob[(ti, ii)] + len(data)] = data

    return buf, de_off, n_leaf


def apply_rva(buf, data_entries_off, n_leaf, rva):
    """把资源数据条目里的节内偏移改成绝对 RVA。"""
    for k in range(n_leaf):
        o = data_entries_off + k * 16
        off = struct.unpack_from('<I', buf, o)[0]
        struct.pack_into('<I', buf, o, rva + off)


def verify(target):
    """检查 PE 是否带图标 / 版本资源，返回 (ok, 说明)。"""
    pe = PE(open(target, 'rb').read())
    rva, size = pe.get_dd(2)
    if rva == 0 or size == 0:
        return False, '无资源目录'
    sec = pe.find('.rsrc')
    if sec is None:
        return False, '无 .rsrc 节'
    base = sec['ra'] + (rva - sec['va'])

    named, ids = struct.unpack_from('<HH', pe.d, base + 12)
    types = []
    for i in range(named + ids):
        rid, tgt = struct.unpack_from('<II', pe.d, base + 16 + i * 8)
        types.append(rid & 0x7FFFFFFF)
    has_icon = RT_ICON in types and RT_GROUP_ICON in types
    if not has_icon:
        return False, f'缺图标类型（现有类型 {types}）'
    if RT_VERSION not in types:
        return False, f'缺版本信息（现有类型 {types}）'
    return True, f'OK（类型 {sorted(types)}）'


def _write_existing(pe, exist, buf, data_entries_off, n_leaf, target):
    raw_size = align_up(len(buf), pe.filealign)
    secs = pe.sections()
    is_last = exist['idx'] == len(secs) - 1
    if raw_size <= exist['rs']:
        apply_rva(buf, data_entries_off, n_leaf, exist['va'])
        pe.d[exist['ra']:exist['ra'] + len(buf)] = buf
        if raw_size > len(buf):
            pe.d[exist['ra'] + len(buf):exist['ra'] + raw_size] = b'\0' * (raw_size - len(buf))
        struct.pack_into('<II', pe.d, exist['off'] + 8, len(buf), exist['rs'])
        pe.set_dd(2, exist['va'], len(buf))
        zero_timestamp(pe)
        fix_checksum(pe)
        open(target, 'wb').write(pe.d)
        return True
    if not is_last:
        return False
    # 原地扩展最后一个节：虚拟大小 / 原始大小 / 镜像大小一起更新
    apply_rva(buf, data_entries_off, n_leaf, exist['va'])
    if len(pe.d) < exist['ra']:
        pe.d += b'\0' * (exist['ra'] - len(pe.d))
    pe.d[exist['ra']:exist['ra'] + len(buf)] = buf
    if len(pe.d) < exist['ra'] + raw_size:
        pe.d += b'\0' * (exist['ra'] + raw_size - len(pe.d))
    struct.pack_into('<II', pe.d, exist['off'] + 8, len(buf), raw_size)
    pe.set_dd(2, exist['va'], len(buf))
    struct.pack_into('<I', pe.d, pe.opt + 56, align_up(exist['va'] + len(buf), pe.secalign))
    zero_timestamp(pe)
    fix_checksum(pe)
    open(target, 'wb').write(pe.d)
    return True


def main():
    args = [a for a in sys.argv[1:]]

    if args and args[0] == '--verify':
        ok_all = True
        for f in args[1:]:
            ok, msg = verify(f)
            print(f'  [校验] {os.path.basename(f):32s} {"通过" if ok else "失败"} - {msg}')
            if not ok:
                ok_all = False
        return 0 if ok_all else 3

    opts = {'ver': None, 'desc': None, 'name': None, 'orig': None, 'dll': False}
    pos_args = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--ver':
            i += 1
            opts['ver'] = args[i]
        elif a == '--desc':
            i += 1
            opts['desc'] = args[i]
        elif a == '--name':
            i += 1
            opts['name'] = args[i]
        elif a == '--orig':
            i += 1
            opts['orig'] = args[i]
        elif a == '--dll':
            opts['dll'] = True
        else:
            pos_args.append(a)
        i += 1
    if len(pos_args) < 2:
        print(__doc__)
        return 1

    target, ico_path = pos_args[0], pos_args[1]
    group_id = int(pos_args[2]) if len(pos_args) > 2 else 101

    ver_blob = None
    if opts['ver']:
        base = os.path.basename(target)
        internal = opts['name'] or os.path.splitext(base)[0]
        original = opts['orig'] or base
        desc = opts['desc'] or '秋雨五笔输入法'
        ver_blob = build_version(opts['ver'], desc, internal, original, opts['dll'])

    pe = PE(open(target, 'rb').read())
    buf, de_off, n_leaf = build_rsrc(ico_path, group_id, ver_blob)
    raw_size = align_up(len(buf), pe.filealign)

    exist = pe.find('.rsrc')
    if exist is not None:
        if _write_existing(pe, exist, buf, de_off, n_leaf, target):
            note = f'{n_leaf} 项' + ('，含版本信息' if ver_blob else '')
            print(f'  [资源] {os.path.basename(target)}: {note}, {len(buf)/1024:.1f} KB, '
                  f'RVA={exist["va"]:#x}')
            return 0
        print(f'[错误] {os.path.basename(target)} 的 .rsrc 空间不足且不是最后一个节，'
              f'请重新编译后再注入')
        return 4

    secs = pe.sections()
    last = secs[-1]
    if pe.nsec * 40 + pe.sec_tbl + 40 > pe.sizehdr:
        print('[错误] 头部空间不足，无法新增节（该 PE 节太多）')
        return 2

    new_rva = align_up(last['va'] + last['vs'], pe.secalign)
    new_raw = align_up(len(pe.d), pe.filealign)

    apply_rva(buf, de_off, n_leaf, new_rva)

    hdr = pe.sec_tbl + pe.nsec * 40
    struct.pack_into('<8sIIIIIIHHI', pe.d, hdr, b'.rsrc', len(buf), new_rva, raw_size, new_raw,
                     0, 0, 0, 0, 0x40000040)
    struct.pack_into('<H', pe.d, pe.e_lfanew + 6, pe.nsec + 1)
    new_img = align_up(new_rva + len(buf), pe.secalign)
    struct.pack_into('<I', pe.d, pe.opt + 56, new_img)
    pe.set_dd(2, new_rva, len(buf))

    if len(pe.d) < new_raw:
        pe.d += b'\0' * (new_raw - len(pe.d))
    pe.d += bytes(buf)
    if len(pe.d) < new_raw + raw_size:
        pe.d += b'\0' * (new_raw + raw_size - len(pe.d))

    zero_timestamp(pe)
    fix_checksum(pe)
    open(target, 'wb').write(pe.d)
    print(f'  [资源] {os.path.basename(target)}: {n_leaf} 项'
          f'{"" if not ver_blob else "，含版本信息"}, 资源 {len(buf)/1024:.1f} KB, '
          f'RVA={new_rva:#x}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
