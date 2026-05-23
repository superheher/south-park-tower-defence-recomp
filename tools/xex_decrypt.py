#!/usr/bin/env python3
"""Independent XEX2 decrypt + basic-decompress, then dump/decode bytes at an RVA.

Used to cross-check the recompiler's decode of the entry point. Read-only on the
dump; writes nothing. Refs: Xenia xex2.{h,cc}, Free60 XEX.
"""
import struct, sys
from Crypto.Cipher import AES

XEX = sys.argv[1] if len(sys.argv) > 1 else r"private\default.xex"
ADDR = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x824499A0

# Retail XEX2 key
RETAIL_KEY = bytes([0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91])

def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def u16(b, o): return struct.unpack_from(">H", b, o)[0]

data = open(XEX, "rb").read()
assert data[:4] == b"XEX2"
pe_off  = u32(data, 0x08)
sec_off = u32(data, 0x10)
n_opt   = u32(data, 0x14)
opt = {}
for i in range(n_opt):
    k = u32(data, 0x18 + i*8); v = u32(data, 0x18 + i*8 + 4); opt[k] = v

image_size = u32(data, sec_off + 0x04)
load_addr  = u32(data, sec_off + 0x110)
file_key   = data[sec_off + 0x150 : sec_off + 0x150 + 16]
print(f"pe_off=0x{pe_off:X} sec_off=0x{sec_off:X} image_size=0x{image_size:X} load_addr=0x{load_addr:08X}")

# file format info (0x000003FF): enc + comp + basic block list
ff = opt[0x000003FF]
info_size = u32(data, ff)
enc = u16(data, ff + 4)
comp = u16(data, ff + 6)
print(f"file_format: info_size={info_size} enc={enc} comp={comp}")

# session key = AES-ECB-decrypt(file_key) with retail key (CBC IV=0 over 1 block == ECB)
session_key = AES.new(RETAIL_KEY, AES.MODE_ECB).decrypt(file_key)
print(f"session_key={session_key.hex()}")

basefile = data[pe_off:]
if enc == 1:
    n = (len(basefile) // 16) * 16
    basefile = AES.new(session_key, AES.MODE_CBC, b"\x00"*16).decrypt(basefile[:n])

# decompress
out = bytearray()
if comp == 1:  # basic: list of (data_size, zero_size)
    p = ff + 8
    src = 0
    nblocks = (info_size - 8) // 8
    for _ in range(nblocks):
        dsz = u32(data, p); zsz = u32(data, p + 4); p += 8
        out += basefile[src:src+dsz]; src += dsz
        out += b"\x00" * zsz
elif comp == 0:
    out = bytearray(basefile)
else:
    print(f"!! compression {comp} not handled here"); sys.exit(2)

print(f"decompressed image: {len(out)} bytes (expect ~0x{image_size:X})")
print(f"image[0:2] = {out[0:2]!r}  (expect b'MZ' if decode correct)")

# Cross-check entry points: XEX optional header vs embedded PE AddressOfEntryPoint
xex_entry = opt.get(0x00010100, 0)
e_lfanew = struct.unpack_from("<I", out, 0x3C)[0]
pe_sig = out[e_lfanew:e_lfanew+4]
opt_hdr = e_lfanew + 4 + 20
aoe = struct.unpack_from("<I", out, opt_hdr + 0x10)[0]  # AddressOfEntryPoint (RVA, LE)
print(f"\nXEX entry (opt 0x00010100) = 0x{xex_entry:08X}")
print(f"PE sig @0x{e_lfanew:X} = {pe_sig!r}; PE AddressOfEntryPoint RVA=0x{aoe:X} -> 0x{load_addr+aoe:08X}")

rva = ADDR - load_addr
print(f"\n=== words at 0x{ADDR:08X} (rva 0x{rva:X}) ===")
for i in range(16):
    if rva + i*4 + 4 > len(out): break
    w = u32(out, rva + i*4)
    print(f"  0x{ADDR + i*4:08X}: {w:08X}   top6={w>>26}")
