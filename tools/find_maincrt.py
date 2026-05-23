#!/usr/bin/env python3
"""Find mainCRTStartup candidates: ROOT functions with a CRT-startup signature.

mainCRTStartup is the kernel's entry on a normal build, so it has NO guest caller
(a root) and looks like (cf. TiP-Recomp's xstart): a prologue saving non-volatiles
(`bl __savegprlr_*`) + a moderate `stwu r1,-N(r1)` frame + several `bl` calls
(security_init_cookie, _initterm x2, main, exit). South Park's XEX entry is a
stub, so its real mainCRTStartup is one of these roots. Read-only over generated/.
"""
import re, glob, os
GEN = r"generated\default"
func_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+|xstart)\)\s*\{')
call_re = re.compile(r'\b(sub_[0-9A-Fa-f]+)\(ctx, base\)')
stwu_re = re.compile(r'//\s*stwu r1,-(\d+)\(r1\)')
save_re = re.compile(r'__savegprlr_(\d+)\(ctx, base\)')
bl_re   = re.compile(r'//\s*bl 0x')

funcs = {}     # name -> dict(frame, save, blcount, calls, lines)
called = set()
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
            text="\n".join(body)
            frame=max([int(x) for x in stwu_re.findall(text)] or [0])
            save=save_re.search(text)
            blcount=len(bl_re.findall(text))
            calls=call_re.findall(text)
            for c in calls: called.add(c)
            funcs[cur]=dict(frame=frame, save=int(save.group(1)) if save else None,
                            bl=blcount, ncalls=len(calls), nlines=len(body))
            cur=None; continue
        body.append(ln)

# roots = defined but never statically called
roots=[n for n in funcs if n not in called and n!="xstart"]
# CRT-startup signature: has savegprlr, moderate frame (0x40..0x800), several bl
cands=[]
for n in roots:
    f=funcs[n]
    if f["save"] is not None and 0x30 <= f["frame"] <= 0x900 and f["bl"]>=4:
        cands.append((f["bl"], n, f))
cands.sort(reverse=True)
print(f"functions={len(funcs)} roots={len(roots)}")
print("=== mainCRTStartup candidates (root + savegprlr + frame 0x30-0x900 + >=4 bl), by bl count ===")
for bl,n,f in cands[:20]:
    print(f"  {n}  frame={f['frame']}(0x{f['frame']:X}) save=r{f['save']} bl={f['bl']} calls={f['ncalls']} lines={f['nlines']}")
