import struct, zlib

def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    rows = []
    stride = ((w * bpp // 8) + 3) // 4 * 4
    n = abs(h)
    for y in range(n):
        i = off + (n - 1 - y) * stride if h > 0 else off + y * stride
        row = d[i:i + w * (bpp // 8)]
        if bpp == 32:
            row = bytes(c for j, c in enumerate(row) if j % 4 != 3)
        rows.append(row)
    return w, abs(h), rows

def save(rows, w, h, Z, name):
    data = b''
    for y in range(h * Z):
        data += b'\x00'
        row = rows[y // Z]
        for x in range(w * Z):
            bb, gg, rr = row[(x // Z) * 3:(x // Z) * 3 + 3]
            data += bytes((rr, gg, bb))
    def chunk(t, d):
        cc = t + d
        return struct.pack('>I', len(d)) + cc + struct.pack('>I', zlib.crc32(cc))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w * Z, h * Z, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(data, 9)) + chunk(b'IEND', b''))
    open(name, 'wb').write(png)

w, h, rows = read_bmp('build/shot_v41_settings.bmp')
X0, X1, Y0, Y1, Z = 540, 900, 50, 660, 2
sub = [r[X0 * 3:(X1) * 3] for r in rows[Y0:Y1]]
save(sub, X1 - X0, Y1 - Y0, Z, 'build/shot_v41_panel.png')
print('ok')
