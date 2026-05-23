#!/usr/bin/env python3
"""Build a direct-call graph over the authoritative .pdata function list and find
CRT-startup candidates.

For each .pdata function we disassemble its bytes (capstone, BE PPC) and collect
`bl`/`b` targets that land on another function start. Then:
  - callers[f]  = functions that bl into f
  - callees[f]  = function starts f directly calls
A root = a function with no direct callers (kernel-/pointer-called). mainCRTStartup
is a root that calls a moderate number of functions and (transitively) reaches a
tiny "_initterm" loop (indirect call inside a loop over a pointer array).

Usage:
  callgraph.py <image.bin> roots [N]      # roots sorted by out-degree
  callgraph.py <image.bin> initterm       # find _initterm-shaped funcs + their callers
  callgraph.py <image.bin> callers <addr>
  callgraph.py <image.bin> callees <addr>
Read-only.
"""
import struct, sys
from capstone import Cs, CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, CS_MODE_32

img = open(sys.argv[1], "rb").read()
cmd = sys.argv[2] if len(sys.argv) > 2 else "roots"
base = 0x82000000

def u16(o): return struct.unpack_from("<H", img, o)[0]
def u32(o): return struct.unpack_from("<I", img, o)[0]
def b32(o): return struct.unpack_from(">I", img, o)[0]

# locate .pdata table (auto, as in pdata.py)
e_lfanew = u32(0x3C); opt_off = e_lfanew + 24; dd_off = opt_off + 0x60
pd_rva = u32(dd_off + 3*8); pd_sz = u32(dd_off + 3*8 + 4); N = len(img)
def find_table():
    i = max(0, pd_rva - 0x4000)
    while i < min(pd_rva + 0x4000, N - 8):
        v = b32(i)
        if 0x82100000 <= v < 0x826E0000:
            j = i; cnt = 0; prev = 0
            while j < N - 8:
                w = b32(j)
                if 0x82000000 <= w < 0x82700000 and w >= prev: cnt += 1; prev = w; j += 8
                else: break
            if cnt > 64: return i, cnt
        i += 4
    return pd_rva, pd_sz // 8
pd_off, n = find_table()
starts = sorted(set(b32(pd_off + i*8) for i in range(n)))
startset = set(starts)
import bisect
def func_end(s):
    i = bisect.bisect_right(starts, s); return starts[i] if i < len(starts) else s + 0x40

md = Cs(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN + CS_MODE_32); md.detail = False
TEXT_LO, TEXT_HI = 0x82100000, 0x825F1000

callers = {s: set() for s in starts}   # f -> who calls f
callees = {s: set() for s in starts}   # f -> what f calls
initterm_like = []

def disasm_func(s):
    e = func_end(s)
    off = s - base
    if off < 0 or off >= len(img): return []
    return list(md.disasm(img[off:e-base], s))

for s in starts:
    ins_list = disasm_func(s)
    has_loop_indcall = False
    seen_bctrl = False
    n_ins = len(ins_list)
    for ins in ins_list:
        m = ins.mnemonic
        if m in ("bl",) :
            try: tgt = int(ins.op_str.split(",")[-1].strip(), 16)
            except: continue
            if tgt in startset:
                callees[s].add(tgt); callers[tgt].add(s)
        elif m == "bctrl":
            seen_bctrl = True
    # _initterm shape: small function, has bctrl, and a backward branch (loop)
    if seen_bctrl and 6 <= n_ins <= 24:
        for ins in ins_list:
            if ins.mnemonic.startswith("b") and ins.mnemonic not in ("bl","bctrl","blr","bctr"):
                try: t = int(ins.op_str.split(",")[-1].strip(), 16)
                except: continue
                if s <= t < ins.address:   # backward branch within func => loop
                    initterm_like.append((s, n_ins)); break

if cmd == "roots":
    K = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    roots = [s for s in starts if not callers[s]]
    roots.sort(key=lambda s: -len(callees[s]))
    print(f"{len(starts)} funcs; {len(roots)} roots (no direct callers). Top by out-degree:")
    for s in roots[:K]:
        print(f"  0x{s:08X}  calls={len(callees[s]):<3}  span=0x{func_end(s)-s:X}")
elif cmd == "initterm":
    print(f"{len(initterm_like)} _initterm-shaped funcs (small, bctrl-in-loop):")
    for s, ni in initterm_like:
        cs = sorted(callers[s])
        cs_s = " ".join(f"{c:08X}" for c in cs[:8])
        print(f"  0x{s:08X} ins={ni} callers[{len(cs)}]: {cs_s}")
elif cmd == "callers":
    a = int(sys.argv[3], 16)
    print(f"callers of 0x{a:08X}: " + " ".join(f"{c:08X}" for c in sorted(callers.get(a, []))))
elif cmd == "callees":
    a = int(sys.argv[3], 16)
    print(f"callees of 0x{a:08X}: " + " ".join(f"{c:08X}" for c in sorted(callees.get(a, []))))
