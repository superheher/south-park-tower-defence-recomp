#!/usr/bin/env python3
"""Scan the decrypted image data region for C++ static-initializer arrays.

The MSVC CRT `.CRT$XC*` C++ initializer table is a run of code pointers in
read-only data, bracketed by null sentinels (`.CRT$XCA`/`.CRT$XCZ`). `_cinit`
calls `_initterm(start, end)` over it. Finding it is the key to running the
title's static initialization (the boot blocker). Reuses xex_decrypt's loader.
"""
import sys, struct
sys.path.insert(0, ".")
import importlib.util
spec = importlib.util.spec_from_file_location("xd", __file__.rsplit("\\",1)[0] + "\\xex_decrypt.py")
# Re-decrypt inline instead (xex_decrypt prints on import); duplicate minimal loader:
from Crypto.Cipher import AES
XEX = r"private\default.xex"
RK = bytes([0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91])
def u32be(b,o): return struct.unpack_from(">I",b,o)[0]
def u16be(b,o): return struct.unpack_from(">H",b,o)[0]
d = open(XEX,"rb").read()
pe_off=u32be(d,8); sec=u32be(d,0x10); n=u32be(d,0x14)
opt={u32be(d,0x18+i*8):u32be(d,0x18+i*8+4) for i in range(n)}
image_size=u32be(d,sec+4); load=u32be(d,sec+0x110); fk=d[sec+0x150:sec+0x160]
sk=AES.new(RK,AES.MODE_ECB).decrypt(fk)
ff=opt[0x3FF]; enc=u16be(d,ff+4); comp=u16be(d,ff+6)
base=d[pe_off:]
if enc==1:
    nlen=(len(base)//16)*16; base=AES.new(sk,AES.MODE_CBC,b"\0"*16).decrypt(base[:nlen])
out=bytearray()
if comp==1:
    p=ff+8; src=0; nb=(u32be(d,ff)-8)//8
    for _ in range(nb):
        ds=u32be(d,p); zs=u32be(d,p+4); p+=8
        out+=base[src:src+ds]; src+=ds; out+=b"\0"*zs
else: out=bytearray(base)

CODE_LO, CODE_HI = 0x82100000, 0x825F0C18
def is_ptr(v): return CODE_LO <= v < CODE_HI
# Scan whole image for runs of >=2 consecutive code pointers preceded+followed by a null u32.
print(f"image {len(out)} bytes, load 0x{load:08X}; scanning for null-bracketed code-pointer runs...")
i = 0; found = 0
N = len(out) - 4
while i < N:
    v = u32be(out, i)
    if v == 0 and i+4 <= N:
        j = i + 4; cnt = 0
        while j+4 <= len(out) and is_ptr(u32be(out, j)):
            cnt += 1; j += 4
        if cnt >= 2 and j+4 <= len(out) and u32be(out, j) == 0:
            va = load + i + 4
            ptrs = [u32be(out, i+4+k*4) for k in range(min(cnt,6))]
            print(f"  array @0x{va:08X} (rva 0x{i+4:X}): {cnt} ptrs: " +
                  " ".join(f"0x{p:08X}" for p in ptrs) + ("..." if cnt>6 else ""))
            found += 1
            i = j
            continue
    i += 4
print(f"total null-bracketed code-pointer arrays: {found}")
