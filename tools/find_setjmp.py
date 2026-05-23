#!/usr/bin/env python3
"""Locate guest setjmp / longjmp in the decrypted PE by their unique signature:
they save/restore the stack pointer (r1) and LR through the jmp_buf pointer (r3) —
something ordinary functions never do.

  setjmp(jmp_buf=r3):  stw r1,X(r3) (+ nonvolatiles) ; ... ; li r3,0 ; blr
  longjmp(jmp_buf=r3): lwz r1,X(r3) ; ... ; mtlr ; mr r3,r4 ; b/blr

Usage: find_setjmp.py <image.bin> [load_base_hex=0x82000000]
Read-only. Needs the .pdata function table (pdata.py) for start addresses, but also
works by scanning the whole code range and snapping to the nearest prior prologue.
Refs: tools/ppc_dis.py, tools/pdata.py
"""
import sys
from capstone import Cs, CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, CS_MODE_32

img  = open(sys.argv[1], "rb").read()
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x82000000

md = Cs(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN + CS_MODE_32)
md.skipdata = True

# Scan the whole guest code range.
START = 0x82000000 - base
END   = min(len(img), 0x82930000 - base)

# Decode the window once into a list for windowed pattern matching.
insns = list(md.disasm(img[START:END], base + START))

def via_r3(i):
    return "(r3)" in i.op_str
def is_nv_store_via_r3(i):   # stw/std/stmw rN,off(r3) of a nonvolatile or SP
    return i.mnemonic in ("stw", "std", "stmw") and via_r3(i)
def is_nv_load_via_r3(i):    # lwz/ld/lmw rN,off(r3)
    return i.mnemonic in ("lwz", "ld", "lmw") and via_r3(i)
def is_li_r3_0(i):
    return i.mnemonic == "li" and i.op_str.replace(" ", "") == "r3,0"
def is_mr_r3_r4(i):
    return i.mnemonic == "mr" and i.op_str.replace(" ", "") == "r3,r4"
def is_mtlr(i):
    return i.mnemonic == "mtlr" or (i.mnemonic == "mtspr" and "lr" in i.op_str)
def is_blr(i):
    return i.mnemonic == "blr"
def saves_sp_via_r3(win):    # stw/std r1,off(r3) present
    return any(w.mnemonic in ("stw", "std") and w.op_str.replace(" ", "").startswith("r1,") and via_r3(w) for w in win)
def restores_sp_via_r3(win):
    return any(w.mnemonic in ("lwz", "ld") and w.op_str.replace(" ", "").startswith("r1,") and via_r3(w) for w in win)

setjmp_hits, longjmp_hits = [], []
for idx in range(len(insns)):
    win = insns[idx: idx + 60]
    n_store = sum(1 for w in win if is_nv_store_via_r3(w))
    n_load  = sum(1 for w in win if is_nv_load_via_r3(w))
    has_stmw = any(w.mnemonic == "stmw" and via_r3(w) for w in win)
    has_lmw  = any(w.mnemonic == "lmw" and via_r3(w) for w in win)
    # setjmp: many nonvolatiles saved via r3 (or stmw r3), saves SP, returns 0
    if (has_stmw or n_store >= 6) and saves_sp_via_r3(win) and any(is_li_r3_0(w) for w in win):
        setjmp_hits.append(insns[idx].address)
    # longjmp: many nonvolatiles restored via r3 (or lmw r3), restores SP, jumps via lr
    if (has_lmw or n_load >= 6) and restores_sp_via_r3(win) and any(is_mtlr(w) for w in win):
        longjmp_hits.append(insns[idx].address)

def snap_start(addr):
    """Walk back to the nearest prologue (mflr / stwu r1) = function start."""
    off = addr - base
    for a in range(addr, addr - 0x400, -4):
        w = img[a - base: a - base + 4]
        if len(w) < 4:
            break
        # mflr r12 (0x7D8802A6) or stwu r1,-X(r1) (0x9421....) or mfspr lr forms
        if w == b"\x7d\x88\x02\xa6" or w[0] == 0x94 and w[1] == 0x21:
            return a
    return addr

print("== setjmp candidates (save SP via r3, return 0) ==")
for a in sorted(set(snap_start(h) for h in setjmp_hits)):
    print(f"  0x{a:08X}")
print("== longjmp candidates (restore SP via r3, jump) ==")
for a in sorted(set(snap_start(h) for h in longjmp_hits)):
    print(f"  0x{a:08X}")
