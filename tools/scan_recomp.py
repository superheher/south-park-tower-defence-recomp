#!/usr/bin/env python3
"""Static scan of rexglue-generated C++ for compile-blocking control-flow issues.

Splits each south_park_td_recomp.*.cpp into functions (DEFINE_REX_FUNC(name){ ... }),
then within each function finds `goto loc_XXXX;` whose `loc_XXXX:` label is not
defined in that same function (C labels are function-scoped -> compile error
'use of undeclared label'). Cross-references each missing target against the set
of all defined functions to confirm the "branch to another function's entry"
(tail-call) hypothesis. Also tallies REX_FATAL unresolved-call stubs (these
compile but trap at runtime).
"""
import re, sys, glob, os, collections

GEN = sys.argv[1] if len(sys.argv) > 1 else \
    r"generated\default"

func_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+)\)\s*\{')
label_re = re.compile(r'^loc_([0-9A-Fa-f]+):')
goto_re = re.compile(r'goto loc_([0-9A-Fa-f]+);')

files = sorted(glob.glob(os.path.join(GEN, "south_park_td_recomp.*.cpp")))
all_funcs = set()        # sub_XXXX names defined anywhere
per_func = []            # (name, defined_labels, goto_targets)
fatal_count = 0

for path in files:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    cur = None
    labels = set()
    gotos = []
    for ln in lines:
        m = func_re.match(ln)
        if m:
            if cur:
                per_func.append((cur, labels, gotos))
            cur = m.group(1)
            all_funcs.add(cur)
            labels = set()
            gotos = []
            continue
        if cur is None:
            continue
        if ln == "}":            # function end (column-0 brace)
            per_func.append((cur, labels, gotos))
            cur = None
            continue
        lm = label_re.match(ln)
        if lm:
            labels.add(lm.group(1).upper())
        for gm in goto_re.finditer(ln):
            gotos.append(gm.group(1).upper())
        if "REX_FATAL(" in ln and "Unresolved" in ln:
            fatal_count += 1
    if cur:
        per_func.append((cur, labels, gotos))

# Find undeclared-label gotos
missing = collections.Counter()      # target addr -> count of bad gotos
missing_is_func = 0
missing_not_func = 0
funcs_with_missing = set()
for name, labels, gotos in per_func:
    for t in gotos:
        if t not in labels:
            missing[t] += 1
            funcs_with_missing.add(name)
            if ("sub_" + t.lower()) in all_funcs or ("sub_" + t) in {f.upper() for f in all_funcs}:
                pass
# recompute func membership case-insensitively
func_addrs = {f.split("_", 1)[1].upper() for f in all_funcs}
for t in list(missing):
    if t in func_addrs:
        missing_is_func += missing[t]
    else:
        missing_not_func += missing[t]

print(f"generated dir       : {GEN}")
print(f"recomp TUs scanned  : {len(files)}")
print(f"functions defined   : {len(all_funcs)}")
print(f"REX_FATAL unresolved-call stubs (runtime, compile OK): {fatal_count}")
print(f"--- undeclared-label gotos (COMPILE BLOCKERS) ---")
print(f"distinct missing labels : {len(missing)}")
print(f"total bad gotos         : {sum(missing.values())}")
print(f"  target IS a defined function entry (tail-call case): {missing_is_func} gotos")
print(f"  target is NOT a function entry (true missing block): {missing_not_func} gotos")
print(f"functions containing bad gotos: {len(funcs_with_missing)}")
print(f"--- sample missing targets (addr : #gotos : is_func_entry) ---")
for t, c in missing.most_common(20):
    print(f"  loc_{t} : {c} : {'FUNC' if t in func_addrs else 'no'}")
