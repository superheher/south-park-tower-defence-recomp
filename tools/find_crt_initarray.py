#!/usr/bin/env python3
"""Find mainCRTStartup via the C/C++ init-array bounds it forms.

MSVC CRT startup calls `_initterm(&__xi_a,&__xi_z)` and `_initterm(&__xc_a,&__xc_z)`.
It forms two .rdata addresses A,Z (lis+addi/ori) that bound a small run of .text
function pointers (the per-TU initializers). We scan .text for lis+addi/ori address
formations into .rdata, then report code sites that form a pair (A,Z) close together
where [A,Z) is a run of .text pointers — that site's function is mainCRTStartup.

Read-only. Usage: find_crt_initarray.py <image.bin>
"""
import struct, sys, bisect
from capstone import Cs, CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, CS_MODE_32

img = open(sys.argv[1], "rb").read()
base = 0x82000000
TEXT_LO, TEXT_HI = 0x82100000, 0x825F1000
RDATA_LO, RDATA_HI = 0x82000600, 0x820E8A9C

def b32(o): return struct.unpack_from(">I", img, o)[0]

# scan .text: track last lis per reg; on addi/ori with same reg, emit formed address
md = Cs(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN + CS_MODE_32)
md.detail = False
md.skipdata = True   # don't stop at undecodable (data-in-code / VMX128); keep going
formed = []   # (site_addr, formed_addr)
code = img[TEXT_LO-base : TEXT_HI-base]
lis_hi = {}   # reg -> hi_value (last lis seen for that reg)
for ins in md.disasm(code, TEXT_LO):
    m = ins.mnemonic; ops = ins.op_str
    try:
        if m == "lis":
            r, v = ops.split(",")
            lis_hi[r.strip()] = int(v.strip(), 0) & 0xFFFF
        elif m in ("addi", "addic", "ori"):
            parts = [p.strip() for p in ops.split(",")]
            if len(parts) == 3 and parts[0] == parts[1] and parts[0] in lis_hi:
                hi = lis_hi[parts[0]]
                lo = int(parts[2], 0)
                if m == "ori":
                    addr = ((hi << 16) | (lo & 0xFFFF)) & 0xFFFFFFFF
                else:
                    addr = ((hi << 16) + lo) & 0xFFFFFFFF
                if RDATA_LO <= addr < RDATA_HI:
                    formed.append((ins.address, addr))
                del lis_hi[parts[0]]
    except Exception:
        pass

def is_text_ptr_run(a, z):
    # is [a,z) a run of .text pointers (allowing nulls)?
    if z <= a or (z - a) % 4 or (z - a) > 0x4000: return False
    n = (z - a) // 4
    if n < 1: return False
    good = 0
    for k in range(n):
        v = b32(a - base + k*4)
        if v == 0: continue
        if TEXT_LO <= v < TEXT_HI and (v & 3) == 0: good += 1
        else: return False
    return good >= 1

# group formed addresses by nearby code site (within 0x80 bytes => same function body)
formed.sort()
print("formed .rdata addresses in .text:", len(formed))
hits = []
for i in range(len(formed)):
    si, ai = formed[i]
    for j in range(i+1, len(formed)):
        sj, aj = formed[j]
        if sj - si > 0x100: break          # different function
        a, z = (ai, aj) if ai < aj else (aj, ai)
        if a != z and is_text_ptr_run(a, z):
            hits.append((si, a, z, (z-a)//4))
for site, a, z, n in hits[:40]:
    print(f"  site~0x{site:08X}  init-array [0x{a:08X},0x{z:08X})  {n} entries  first={b32(a-base):08X}")
print(f"total candidate (bounds-forming) sites: {len(hits)}")
