#!/usr/bin/env python3
"""Parse the Xbox 360 PE .pdata (EXCEPTION) directory = authoritative function table.

Each RUNTIME_FUNCTION (PPC/Xbox360) is 8 bytes, little-endian:
  +0  BeginAddress (RVA, relative to image base)
  +4  packed: FunctionLength:22 | PrologLength:8 | is32:1 | hasException:1  (low->high)
We only rely on BeginAddress + FunctionLength to get [start, start+len*4).

Usage:
  pdata.py <image.bin> list [N]          # first N function starts (default 20)
  pdata.py <image.bin> find <addr_hex>   # which function contains addr; is it a start?
  pdata.py <image.bin> count
Read-only. load base 0x82000000, .pdata at RVA 0xE8C00 size 0x14D80 (auto-read from PE).
"""
import struct, sys

img  = open(sys.argv[1], "rb").read()
cmd  = sys.argv[2] if len(sys.argv) > 2 else "count"
base = 0x82000000

def u16(o): return struct.unpack_from("<H", img, o)[0]
def u32(o): return struct.unpack_from("<I", img, o)[0]

e_lfanew = u32(0x3C)
opt_off  = e_lfanew + 24
dd_off   = opt_off + 0x60
pdata_rva = u32(dd_off + 3*8); pdata_sz = u32(dd_off + 3*8 + 4)
n = pdata_sz // 8

# The EXCEPTION dir RVA can be off by a page vs the reconstructed image (basic
# decompression alignment). Auto-locate the table: first run of >=64 ascending
# big-endian begin addresses in [0x82100000, code_end), 8 bytes apart.
def find_table():
    N = len(img); i = pdata_rva - 0x4000 if pdata_rva > 0x4000 else 0
    while i < min(pdata_rva + 0x4000, N - 8):
        v = struct.unpack_from(">I", img, i)[0]
        if 0x82100000 <= v < 0x826E0000:
            j = i; cnt = 0; prev = 0
            while j < N - 8:
                b = struct.unpack_from(">I", img, j)[0]
                if 0x82000000 <= b < 0x82700000 and b >= prev:
                    cnt += 1; prev = b; j += 8
                else: break
            if cnt > 64: return i, cnt
        i += 4
    return pdata_rva, n
pdata_rva, n = find_table()

funcs = []  # (start_va, length_bytes, prolog_instrs)
def b32(o): return struct.unpack_from(">I", img, o)[0]   # .pdata content is big-endian (guest)
for i in range(n):
    o = pdata_rva + i*8
    begin = b32(o)            # RVA (big-endian)
    packed = b32(o+4)
    flen = (packed & 0x3FFFFF) * 4
    plen = ((packed >> 22) & 0xFF) * 4
    start_va = begin if begin >= base else base + begin
    funcs.append((start_va, flen, plen))
funcs.sort()

# boundaries from consecutive begins (the packed length field decode is format-
# specific; consecutive starts give exact [begin[i], begin[i+1]) spans).
starts = sorted(set(s for s, _, _ in funcs if s >= 0x82100000))
import bisect

if cmd == "count":
    print(f".pdata rva=0x{pdata_rva:X} size=0x{pdata_sz:X} -> {n} RUNTIME_FUNCTION entries")
    print(f"unique starts={len(starts)}  first=0x{starts[0]:08X}  last=0x{starts[-1]:08X}")
elif cmd == "list":
    N = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    for i in range(min(N, len(starts))):
        s = starts[i]; e = starts[i+1] if i+1 < len(starts) else s
        print(f"  0x{s:08X}  span=0x{e-s:X}")
elif cmd == "find":
    a = int(sys.argv[3], 16)
    i = bisect.bisect_right(starts, a) - 1
    print(f"query 0x{a:08X}")
    if i < 0:
        print("  before first function"); sys.exit()
    s = starts[i]; e = starts[i+1] if i+1 < len(starts) else None
    es = f"0x{e:08X}" if e else "(last)"
    print(f"  function start below = 0x{s:08X}, next start = {es}")
    print(f"  is_function_start = {a == s}   offset_into_func = +0x{a-s:X}")
