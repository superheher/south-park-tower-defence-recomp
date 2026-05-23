#!/usr/bin/env python3
"""Locate _initterm-like loops and their callers (mainCRTStartup candidates).

_initterm(begin,end) loops over a function-pointer array calling each non-null
entry. In rexglue output that's a SMALL function with a REX_CALL_INDIRECT_FUNC
inside a backward `goto loc_` loop. Its caller is _cinit/mainCRTStartup (the real
CRT entry that runs C++ static init then main) — the function the recompiled
`xstart` stub fails to reach. Read-only over generated/.
"""
import re, glob, os, collections
GEN = r"generated\default"
func_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+|xstart)\)\s*\{')
label_re = re.compile(r'^loc_([0-9A-Fa-f]+):')
goto_re = re.compile(r'goto loc_([0-9A-Fa-f]+);')
call_re = re.compile(r'\b(sub_[0-9A-Fa-f]+)\(ctx, base\)')

# Pass 1: parse all functions: name -> (lines, labels, gotos, has_indirect, calls)
funcs = {}
callers = collections.defaultdict(set)   # callee -> set of caller names
for path in sorted(glob.glob(os.path.join(GEN, "south_park_td_recomp.*.cpp"))):
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    cur=None; body=[]
    for ln in lines:
        m=func_re.match(ln)
        if m:
            cur=m.group(1); body=[]; continue
        if cur is None: continue
        if ln=="}":
            labels={label_re.match(x).group(1).upper() for x in body if label_re.match(x)}
            gotos=[g.upper() for x in body for g in goto_re.findall(x)]
            has_ind=any("REX_CALL_INDIRECT_FUNC" in x for x in body)
            calls=[c for x in body for c in call_re.findall(x)]
            funcs[cur]=dict(n=len(body),labels=labels,gotos=gotos,ind=has_ind,calls=calls)
            for c in calls: callers[c].add(cur)
            cur=None; continue
        body.append(ln)

# _initterm candidates: small, has indirect call, and a backward goto (loop)
def addr_of(name): return int(name.split("_",1)[1],16)
cands=[]
for name,info in funcs.items():
    if not info["ind"]: continue
    if info["n"] > 60: continue
    a=addr_of(name) if name!="xstart" else 0
    # backward goto = goto target < some defined label after it... approx: any goto whose target
    # label is defined AND appears (loop). Heuristic: has a goto to a label that's <= its own range.
    looped = len(info["gotos"])>0 and len(info["labels"])>0
    if looped:
        cands.append((info["n"], name, len(callers.get(name,())) ))

cands.sort()
print(f"functions parsed: {len(funcs)}")
print(f"=== small (<=60 line) indirect-call loop candidates (likely _initterm/dispatch) ===")
for n,name,ncall in cands[:25]:
    calls_into = funcs[name]["calls"]
    print(f"  {name}  lines={n} callers={ncall}")
    # show its callers (mainCRTStartup candidates)
    for c in sorted(callers.get(name,()))[:4]:
        ci=funcs[c]
        print(f"      <- {c} (lines={ci['n']}, callers={len(callers.get(c,()))}, calls={len(ci['calls'])})")
