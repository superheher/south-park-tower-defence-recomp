#!/usr/bin/env python3
"""Post-codegen fixup for rexglue 0.8 undeclared-label gotos (re-run after codegen).

rexglue 0.8's analyzer sometimes emits `goto loc_XXXX;` where `loc_XXXX:` is never
defined in that function (C labels are function-scoped -> 'use of undeclared
label' compile error). Two causes (see knowledge-base general/50 + the title's
codegen notes):
  1. branch to ANOTHER function's entry that overlaps the caller's PDATA range
     -> should be a tail call;
  2. branch to an internal address with no emitted block (data-in-code / boundary
     gap) -> no valid local target.

This rewrites each undeclared-label goto, preserving any branch condition:
  - target is a defined function entry  -> `sub_X(ctx, base); return;`  (correct tail call)
  - otherwise                           -> `REX_FATAL(...); return;`     (runtime trap to triage)

Idempotent: only undeclared-label gotos are touched. Safe to run every build.
"""
import re, sys, glob, os, collections

GEN = sys.argv[1] if len(sys.argv) > 1 else \
    r"generated\default"

func_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-Fa-f]+)\)\s*\{')
label_re = re.compile(r'^loc_([0-9A-Fa-f]+):')
goto_cond_re = re.compile(r'^(\s*)if \((.*)\) goto loc_([0-9A-Fa-f]+);\s*$')
goto_uncond_re = re.compile(r'^(\s*)goto loc_([0-9A-Fa-f]+);\s*$')

files = sorted(glob.glob(os.path.join(GEN, "south_park_td_recomp.*.cpp")))

# Pass 1: collect every defined function address (upper-case hex).
func_addrs = set()
for path in files:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = func_re.match(ln)
            if m:
                func_addrs.add(m.group(1).split("_", 1)[1].upper())

# Pass 2: per file, per function, collect defined labels, then rewrite bad gotos.
total_tail, total_trap, files_changed = 0, 0, 0
per_file_stats = collections.Counter()

for path in files:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()

    # Map each line index -> set of labels defined in its enclosing function.
    func_ranges = []  # (start_idx, end_idx_exclusive, labels)
    cur_start = None
    cur_labels = set()
    for i, ln in enumerate(lines):
        if func_re.match(ln):
            cur_start = i
            cur_labels = set()
        elif cur_start is not None and ln == "}":
            func_ranges.append((cur_start, i + 1, cur_labels))
            cur_start = None
        else:
            lm = label_re.match(ln)
            if lm and cur_start is not None:
                cur_labels.add(lm.group(1).upper())

    line_labels = [None] * len(lines)
    for s, e, labels in func_ranges:
        for i in range(s, e):
            line_labels[i] = labels

    changed = False
    for i, ln in enumerate(lines):
        labels = line_labels[i]
        if labels is None:
            continue
        m = goto_cond_re.match(ln)
        cond = None
        if m:
            indent, cond, tgt = m.group(1), m.group(2), m.group(3).upper()
        else:
            m = goto_uncond_re.match(ln)
            if not m:
                continue
            indent, tgt = m.group(1), m.group(2).upper()
        if tgt in labels:
            continue  # declared -> fine
        # undeclared: rewrite
        if tgt in func_addrs:
            action = f"sub_{tgt}(ctx, base); return;"  # tgt already upper-cased
            total_tail += 1
        else:
            action = f'REX_FATAL("rexglue0.8 unmapped branch to 0x{tgt}"); return;'
            total_trap += 1
        if cond is not None:
            lines[i] = f"{indent}if ({cond}) {{ {action} }}"
        else:
            lines[i] = f"{indent}{action}"
        changed = True
        per_file_stats[os.path.basename(path)] += 1

    if changed:
        files_changed += 1
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")

print(f"defined functions      : {len(func_addrs)}")
print(f"files changed          : {files_changed}/{len(files)}")
print(f"goto -> tail call      : {total_tail}")
print(f"goto -> REX_FATAL trap : {total_trap}")
print(f"total rewrites         : {total_tail + total_trap}")
