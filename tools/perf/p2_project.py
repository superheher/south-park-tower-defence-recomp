#!/usr/bin/env python3
# p2_project.py -- cheap, measured GO/NO-GO projection for P2 selective leaf-inlining,
# BEFORE paying for the codegen change + 15-min build (same discipline as the GPR-as-local NO-GO).
#
# Question: if we inline the eligible tiny straight-line leaves into their direct callsites, what
# FRACTION of the ~157k static intra-recomp direct call/branch SITES disappears (the BTB-capacity
# lever), and how much does .text GROW (the DSB/i-cache counter-pressure, already the dominant
# front-end miss at 77M/1e9i)? If the eliminated-site fraction is small OR the bloat is large, NO-GO.
#
# Reads the generated recomp TUs (the actual emitted call sites + per-function bodies) and the
# medium exe's symbol sizes (x86 bytes per __imp__sub_X) for the bloat estimate. Read-only.
import re, sys, subprocess, glob, os
from collections import defaultdict

GEN = "/home/h/src/recomp/rexglue-recomps/south-park-recomp/generated/default"
EXE = "/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/south_park_td"
THRESH_INSN = int(sys.argv[1]) if len(sys.argv) > 1 else 16   # guest insn count (~4 bytes each -> 64B)

CALL_RE = re.compile(r'\b(sub_[0-9A-Fa-f]+|__imp__\w+|__savegprlr_\d+|__restgprlr_\d+|__save\w+|__rest\w+)\(ctx, base\)')
DEF_RE  = re.compile(r'^DEFINE_REX_FUNC\((\w+)\)\s*\{')
INSN_RE = re.compile(r'^\t// [a-z]')          # disassembly comment lines = guest instructions
INDIRECT_RE = re.compile(r'REX_CALL|ResolveIndirect|\.host\(|PPCFunc|->\s*\(ctx, base\)|\(\*')
LR_RE   = re.compile(r'ctx\.lr')              # any LR touch in a (would-be) leaf -> exclude (mflr/mtlr)
SWITCH_RE = re.compile(r'^\tswitch \(')

# ---- 1. exe symbol sizes: __imp__sub_X -> x86 bytes ----
size = {}
try:
    nm = subprocess.run(["nm", "-S", "--defined-only", EXE], capture_output=True, text=True, timeout=120).stdout
    for line in nm.splitlines():
        p = line.split()
        if len(p) >= 4 and p[3].startswith("__imp__"):
            try: size[p[3][len("__imp__"):]] = int(p[1], 16)
            except ValueError: pass
except Exception as e:
    print("WARN nm failed:", e)

# ---- 2. parse generated TUs: function bodies + every direct call site ----
funcs = {}                      # name -> dict(insns, leaf, uses_lr, indirect, switch)
callsite_count = defaultdict(int)   # callee name -> number of call sites
total_callsites = 0
cur = None
for path in sorted(glob.glob(os.path.join(GEN, "*_recomp.*.cpp"))):
    for line in open(path, encoding="utf-8", errors="replace"):
        m = DEF_RE.match(line)
        if m:
            cur = m.group(1)
            funcs[cur] = dict(insns=0, leaf=True, uses_lr=False, indirect=False, switch=False)
            continue
        if cur is None:
            # calls can also appear at file scope? no. skip.
            pass
        if INSN_RE.match(line):
            if cur: funcs[cur]['insns'] += 1
        if cur and LR_RE.search(line):
            funcs[cur]['uses_lr'] = True
        if cur and INDIRECT_RE.search(line):
            funcs[cur]['indirect'] = True
        if cur and SWITCH_RE.match(line):
            funcs[cur]['switch'] = True
        # every direct call SITE (BL / tail-B / switch-case) -- counts toward branch-site total
        for cm in CALL_RE.finditer(line):
            callee = cm.group(1)
            if callee.startswith("__imp__"): callee = callee[len("__imp__"):]
            callsite_count[callee] += 1
            total_callsites += 1
            if cur: funcs[cur]['leaf'] = False   # this function makes a call -> not a leaf

# ---- 3. eligibility + projection ----
elig = []
for name, f in funcs.items():
    if not f['leaf']: continue
    if f['indirect'] or f['switch'] or f['uses_lr']: continue
    if f['insns'] == 0 or f['insns'] > THRESH_INSN: continue
    elig.append(name)

elig_set = set(elig)
elim_sites = sum(callsite_count[n] for n in elig)
# .text bloat: each inlined callsite adds (x86 body bytes - ~5B call); out-of-line __imp__ copy stays
bloat = 0; known = 0
for n in elig:
    s = size.get(n)
    if s is None: continue
    known += 1
    bloat += callsite_count[n] * max(0, s - 5)

exe_text = 0
try:
    szout = subprocess.run(["size", EXE], capture_output=True, text=True).stdout.splitlines()
    exe_text = int(szout[1].split()[0])
except Exception: pass

print("========== P2 LEAF-INLINE PROJECTION (threshold = %d guest insns / ~%dB) ==========" % (THRESH_INSN, THRESH_INSN*4))
print(f"  total functions parsed         : {len(funcs)}")
print(f"  total direct call SITES        : {total_callsites}   (the BTB-pressure population)")
nleaf = sum(1 for f in funcs.values() if f['leaf'])
print(f"  leaf functions (no calls out)  : {nleaf}")
print(f"  INLINE-ELIGIBLE leaves         : {len(elig)}   (leaf & <= {THRESH_INSN} insn & no lr/indirect/switch)")
print(f"  call sites TO eligible leaves  : {elim_sites}")
print(f"  >> call-site ELIMINATION       : {100.0*elim_sites/total_callsites if total_callsites else 0:5.2f}%  <<  (the BTB win ceiling)")
print(f"  projected .text BLOAT          : {bloat:,} B  (+{100.0*bloat/exe_text if exe_text else 0:.1f}% of {exe_text:,})  [{known}/{len(elig)} sizes known]")
# top eligible leaves by callsite count
top = sorted(elig, key=lambda n: callsite_count[n], reverse=True)[:15]
print("  top eligible leaves by call-site count (name: sites, guest_insns, x86B):")
for n in top:
    print(f"    {n}: {callsite_count[n]} sites, {funcs[n]['insns']} insn, {size.get(n,'?')}B")
# how concentrated: share of elim sites in the top-50
top50 = sorted(elig, key=lambda n: callsite_count[n], reverse=True)[:50]
print(f"  top-50 eligible leaves account for {sum(callsite_count[n] for n in top50)} / {elim_sites} eliminable sites")
