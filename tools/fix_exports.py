#!/usr/bin/env python3
"""把 32 位 DLL 中被 @N 修饰的导出名就地改写为 Windows 期望的未修饰名。
导出名表是若干以 \\0 结尾的字符串，缩短名称并用 \\0 填充不会破坏结构。"""
import sys

PAIRS = [
    (b"DllGetClassObject@12", b"DllGetClassObject"),
    (b"DllCanUnloadNow@0", b"DllCanUnloadNow"),
    (b"DllRegisterServer@0", b"DllRegisterServer"),
    (b"DllUnregisterServer@0", b"DllUnregisterServer"),
]

def patch(path):
    data = bytearray(open(path, "rb").read())
    n = 0
    for old, new in PAIRS:
        start = 0
        while True:
            i = data.find(old, start)
            if i < 0:
                break
            pad = len(old) - len(new)
            data[i:i + len(old)] = new + b"\0" * pad
            start = i + len(old)
            n += 1
    open(path, "wb").write(data)
    print(f"patched {path}: {n} exports")

if __name__ == "__main__":
    patch(sys.argv[1])
