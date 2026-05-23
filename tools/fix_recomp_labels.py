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

# --- Fix 2: drop func_mappings entries the runtime can't register ---
# runtime.cpp iterates func_mappings[] until guest==0 and *aborts setup* if any
# entry's address is outside the module range [code_base, code_base+code_size+
# thunk_reserve). rexglue 0.8 emits two kinds of bad entries: (a) the address-0
# sentinel `{ 0x0, sub_0 }` FIRST (sorts first -> the zero-terminated loop stops
# immediately and registers nothing -> "No function registered at <entry>"), and
# (b) ~dozens of out-of-image stub functions (data-as-code / sentinels such as
# 0x8260C92C..0xFFFFFFFF). Drop every func_mappings entry outside the valid code
# range; the array's real terminator `{ 0, nullptr }` is preserved (name != sub_).
def _const(text, name, default):
    m = re.search(r'#define\s+' + name + r'\s+0x([0-9A-Fa-f]+)', text)
    return int(m.group(1), 16) if m else default
entry_re = re.compile(r'^\s*\{\s*0x([0-9A-Fa-f]+),\s*(sub_[0-9A-Fa-f]+|xstart)\s*\},?\s*$')
mappings_dropped = 0
for init_path in glob.glob(os.path.join(GEN, "*_init.cpp")):
    hdr = init_path[:-4] + ".h"
    htext = open(hdr, encoding="utf-8", errors="replace").read() if os.path.exists(hdr) else ""
    code_base = _const(htext, "REX_CODE_BASE", 0x82100000)
    code_size = _const(htext, "REX_CODE_SIZE", 0)
    thunk = _const(htext, "REX_THUNK_RESERVE_SIZE", 0x10000)
    lo, hi = code_base, code_base + code_size + thunk
    with open(init_path, "r", encoding="utf-8", errors="replace") as fh:
        ilines = fh.read().splitlines()
    out = []
    for ln in ilines:
        m = entry_re.match(ln)
        if m:
            addr = int(m.group(1), 16)
            if addr < lo or addr >= hi:
                mappings_dropped += 1
                continue
        out.append(ln)
    if mappings_dropped:
        with open(init_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(out) + "\n")

# --- Fix 3: EH "enter-try" dispatcher returns 0 when its global hook is null ---
# sub_8242EEA0 tail-calls a runtime-installed setjmp-style hook at [0x82902438] when set,
# else FALLS THROUGH leaving r3 = the caller's stale &localbuf (non-zero). That hook is
# image-initialised to 0 and no installer runs before the worker threads that use it, so
# callers (e.g. sub_8226F978, the writer factory's init gate) read the stale non-zero r3
# as "longjmp taken -> SKIP body" and skip critical init callbacks (sub_82277570 sets the
# writer's buffer cursor [+8] and [+68]) -> null-pointer write crash in sub_8227EB58.
# With no EH/longjmp infra installed, the correct degenerate value is 0 ("no jump, run
# body"); inject `r3 = 0` on the null-hook fall-through. Verified (runtime) to unblock the
# boot past the party/session writer init -> reaches GPU SetInterruptCallback.
# NOTE: this MASKS a missing hook installer; revisit if real EH/longjmp paths are needed.
# See knowledge-base/titles/south-park-lgtdp/35-entry-forensics.md.
EH_MARK = "// [recomp-fix:eh-hook-null]"
eh_patched = 0
for path in files:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    out, changed, in_fn, seen_mark = [], False, False, False
    for ln in lines:
        if ln.startswith("DEFINE_REX_FUNC(sub_8242EEA0)"):
            in_fn, seen_mark = True, False
        elif in_fn and EH_MARK in ln:
            seen_mark = True
        elif in_fn and ln == "}":
            if not seen_mark:
                out.append("\tctx.r3.s64 = 0; " + EH_MARK + " null EH hook -> run body")
                eh_patched += 1
                changed = True
            in_fn = False
        out.append(ln)
    if changed:
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(out) + "\n")

# --- Fix 4: emit the analyzer-missed jump-table stub sub_822E38E0 ---
# rexglue's boundary analyzer misses the 8-byte stub at 0x822E38E0
# (addi r3,r3,8 ; b 0x822E2298). It is an indirect-call target (runtime FATAL
# "Call to invalid or unregistered function at 0x822E38E0") AND the direct branch
# target of sub_822E38E8, which rexglue emits as REX_FATAL "Unresolved call ... to
# 0x822E38E0". Emit the stub next to sub_822E38E8, rewrite that branch to a tail call,
# and register the stub in the func table (declared locally in *_init.cpp so init.h is
# untouched -> no full rebuild). Only this single gap exists in this title's codegen.
# See knowledge-base/titles/south-park-lgtdp/35-entry-forensics.md.
STUB_MARK = "[recomp-fix:missing-stub-822E38E0]"
STUB_DEF = ("// " + STUB_MARK + " analyzer-missed jump stub (addi r3,r3,8; b 0x822E2298)\n"
            "DECLARE_REX_FUNC(sub_822E38E0); // local decl keeps symbol extern \"C\" (no init.h edit)\n"
            "DEFINE_REX_FUNC(sub_822E38E0) {\n"
            "\tREX_FUNC_PROLOGUE();\n"
            "\tctx.r3.s64 = ctx.r3.s64 + 8;\n"
            "\tsub_822E2298(ctx, base);\n"
            "\treturn;\n"
            "}\n\n")
STUB_FATAL = 'REX_FATAL("Unresolved call from 0x822E38EC to 0x822E38E0");'
stub_emitted = stub_registered = 0
for path in files:
    txt = open(path, "r", encoding="utf-8", errors="replace").read()
    if STUB_FATAL not in txt or STUB_MARK in txt:
        continue
    out = []
    for ln in txt.splitlines():
        if ln.strip().startswith("// FATAL: unresolved function 0x822E38E0"):
            continue  # drop rexglue's FATAL comment
        if STUB_FATAL in ln:
            indent = ln[:len(ln) - len(ln.lstrip())]
            out.append(indent + "sub_822E38E0(ctx, base); return;")
            continue
        out.append(ln)
    txt = "\n".join(out) + "\n"
    txt = txt.replace("DEFINE_REX_FUNC(sub_822E38E8) {", STUB_DEF + "DEFINE_REX_FUNC(sub_822E38E8) {", 1)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(txt)
    stub_emitted += 1
for init_path in glob.glob(os.path.join(GEN, "*_init.cpp")):
    txt = open(init_path, "r", encoding="utf-8", errors="replace").read()
    if STUB_MARK in txt or "{ 0x822E38E0," in txt:
        continue
    if "PPCFuncMapping PPCFuncMappings[] = {" in txt and "{ 0x822E38D8, sub_822E38D8 }," in txt:
        txt = txt.replace("PPCFuncMapping PPCFuncMappings[] = {",
                          "DECLARE_REX_FUNC(sub_822E38E0); // " + STUB_MARK + "\n\nPPCFuncMapping PPCFuncMappings[] = {", 1)
        txt = txt.replace("\t{ 0x822E38D8, sub_822E38D8 },\n",
                          "\t{ 0x822E38D8, sub_822E38D8 },\n\t{ 0x822E38E0, sub_822E38E0 }, // " + STUB_MARK + "\n", 1)
        with open(init_path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(txt)
        stub_registered += 1

print(f"defined functions      : {len(func_addrs)}")
print(f"EH hook-null fix (sub_8242EEA0): {eh_patched} applied")
print(f"missing-stub 0x822E38E0: emitted={stub_emitted} registered={stub_registered}")
print(f"files changed          : {files_changed}/{len(files)}")
print(f"goto -> tail call      : {total_tail}")
print(f"goto -> REX_FATAL trap : {total_trap}")
print(f"total rewrites         : {total_tail + total_trap}")
print(f"func_mappings out-of-range entries dropped: {mappings_dropped}")
