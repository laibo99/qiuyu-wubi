import struct, sys
def exports(path):
    d = open(path,'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    opt = pe+24
    magic = struct.unpack_from('<H', d, opt)[0]
    dd = opt + (112 if magic==0x20b else 96)
    exp_rva, exp_sz = struct.unpack_from('<II', d, dd)
    secs=[]
    for i in range(nsec):
        s = opt + struct.unpack_from('<H', d, pe+20)[0] + i*40
        name = d[s:s+8].rstrip(b'\0').decode()
        vsz, va, rsz, raw = struct.unpack_from('<IIII', d, s+8)
        secs.append((name, va, vsz, raw))
    def r2o(rva):
        for name, va, vsz, raw in secs:
            if va <= rva < va+max(vsz,rsz): return raw + (rva-va)
        return None
    if not exp_rva: return []
    e = r2o(exp_rva)
    nnames = struct.unpack_from('<I', d, e+24)[0]
    names_rva = struct.unpack_from('<I', d, e+32)[0]
    no = r2o(names_rva)
    out=[]
    for i in range(nnames):
        n_rva = struct.unpack_from('<I', d, no+i*4)[0]
        o = r2o(n_rva)
        end = d.index(b'\0', o)
        out.append(d[o:end].decode())
    return out
for p in sys.argv[1:]:
    print(p, '->', exports(p))
