#!/usr/bin/env python3
"""Boundary-independent CRT-startup finder.

(1) Global `bl` scan over .text -> caller map (target -> [call sites]); does not
    depend on function boundaries (which .pdata merges / heuristics over-split).
(2) Pattern-match the canonical MSVC `_initterm(first,last)`: a loop with a bounds
    compare (`cmplw`), an indirect call (`bctrl`), and a `+4` pointer increment.
(3) For each _initterm candidate, print its global callers = mainCRTStartup-ish.

Usage:
  find_crt.py <image.bin> initterm
  find_crt.py <image.bin> callers <addr_hex>
Read-only.
"""
import struct, sys
from capstone import Cs, CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, CS_MODE_32

img = open(sys.argv[1], "rb").read()
cmd = sys.argv[2] if len(sys.argv) > 2 else "initterm"
base = 0x82000000
TEXT_LO, TEXT_HI = 0x82100000, 0x825F1000   # .text VA range

def u16(o): return struct.unpack_from("<H", img, o)[0]
def u32(o): return struct.unpack_from("<I", img, o)[0]
def b32(o): return struct.unpack_from(">I", img, o)[0]

# --- (1) global bl caller map (decode every aligned word in .text) ---
callers = {}   # target -> list of call-site addrs
for va in range(TEXT_LO, TEXT_HI, 4):
    o = va - base
    if o + 4 > len(img): break
    w = b32(o)
    if (w >> 26) == 18:                      # I-form branch (b/bl/ba/bla)
        if w & 1:                            # LK=1 => bl/bla
            li = w & 0x03FFFFFC
            if li & 0x02000000: li -= 0x04000000   # sign-extend 26-bit
            if not (w & 2):                  # AA=0 => relative
                tgt = (va + li) & 0xFFFFFFFF
            else:
                tgt = li & 0xFFFFFFFF
            if TEXT_LO <= tgt < TEXT_HI:
                callers.setdefault(tgt, []).append(va)

# --- .pdata starts (auto-locate, big-endian) ---
e_lfanew = u32(0x3C); opt_off = e_lfanew + 24; dd_off = opt_off + 0x60
pd_rva = u32(dd_off + 3*8); N = len(img)
i = max(0, pd_rva - 0x4000); pd_off = pd_rva; n = 0
while i < min(pd_rva + 0x4000, N - 8):
    v = b32(i)
    if 0x82100000 <= v < 0x826E0000:
        j = i; cnt = 0; prev = 0
        while j < N - 8:
            w = b32(j)
            if 0x82000000 <= w < 0x82700000 and w >= prev: cnt += 1; prev = w; j += 8
            else: break
        if cnt > 64: pd_off, n = i, cnt; break
    i += 4
starts = sorted(set(b32(pd_off + k*8) for k in range(n)))
import bisect
def nxt(s):
    k = bisect.bisect_right(starts, s); return starts[k] if k < len(starts) else s + 0x80

md = Cs(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN + CS_MODE_32); md.detail = True

def is_initterm(s):
    e = min(nxt(s), s + 0x100)
    code = img[s-base:e-base]
    has_bctrl = has_inc4 = has_cmpl = False
    for ins in md.disasm(code, s):
        m = ins.mnemonic
        if m == "bctrl": has_bctrl = True
        elif m == "addi" and ins.op_str.endswith(", 4"): has_inc4 = True
        elif m in ("cmplw", "cmpw"): has_cmpl = True
    return has_bctrl and has_inc4 and has_cmpl

if cmd == "initterm":
    # candidate starts = bl-targets (guaranteed real func starts) U .pdata starts
    cand = sorted(set(callers.keys()) | set(starts))
    hits = [s for s in cand if is_initterm(s)]
    # the interesting ones are those WITH bl callers (real _initterm, not per-TU init thunks)
    called = [s for s in hits if callers.get(s)]
    print(f"_initterm-pattern fns: {len(hits)} total, {len(called)} with bl callers:")
    for s in called:
        cs = callers.get(s, [])
        cs_s = " ".join(f"{c:08X}" for c in cs[:12])
        print(f"  0x{s:08X}  callers[{len(cs)}]: {cs_s}")
elif cmd == "callers":
    a = int(sys.argv[3], 16)
    cs = callers.get(a, [])
    print(f"global bl callers of 0x{a:08X} [{len(cs)}]: " + " ".join(f"{c:08X}" for c in cs))
