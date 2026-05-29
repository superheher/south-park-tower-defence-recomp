#!/usr/bin/env bash
# Milestone-0 feasibility probe for the AOT+interpreter hybrid.
# Joins per-function PGO execution counts (hotness) with per-function .text sizes,
# then asks: if we keep ONLY the functions covering the hot X% of all execution
# native (recompiled) and hand the cold tail to a shared interpreter (~0 .text each),
# how big is the remaining recompiled .text? If it drops under the 12 MB L3 -> the
# hybrid can reach cache-fit and (per the capacity mechanism) move the floor.
# READ-ONLY: only inspects the PGO profile + the binary's symbol table. No build, no game.
set -u
OUT=/tmp/m0_probe.txt; exec >"$OUT" 2>&1
PROF=/tmp/sp/port.profdata
BIN=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/south_park_td
PROFDATA=$(command -v llvm-profdata || command -v llvm-profdata-21 || command -v llvm-profdata-20)
NM=$(command -v nm)

echo "profdata tool: $PROFDATA"
echo "profile: $PROF ($(stat -c%s "$PROF" 2>/dev/null) B)"
echo "binary:  $BIN"
echo

echo "== dumping per-function counts (llvm-profdata show --all-functions --counts) =="
"$PROFDATA" show --all-functions --counts "$PROF" > /tmp/m0_counts.txt 2>/tmp/m0_counts.err
echo "  counts lines: $(wc -l < /tmp/m0_counts.txt)   (err: $(head -1 /tmp/m0_counts.err))"
echo "  sample (first function block):"; sed -n '1,12p' /tmp/m0_counts.txt | sed 's/^/    /'

echo
echo "== dumping per-symbol .text sizes (nm -S) =="
"$NM" -S --defined-only "$BIN" 2>/dev/null | awk '$3 ~ /^[tT]$/' > /tmp/m0_syms.txt
echo "  text symbols with sizes: $(wc -l < /tmp/m0_syms.txt)"
echo "  sample:"; sed -n '1,5p' /tmp/m0_syms.txt | sed 's/^/    /'

echo
echo "== JOIN + cumulative cold-tail projection =="
python3 - <<'PY'
import re
# 1) sizes: nm -S lines "addr size T name" ; name like sub_82100000 or __imp__sub_82100000 or __imp____savegprlr_29
sizes = {}
for ln in open('/tmp/m0_syms.txt'):
    p = ln.split()
    if len(p) < 4: continue
    try: sz = int(p[1], 16)
    except ValueError: continue
    name = p[3]
    core = name
    if core.startswith('__imp__'): core = core[len('__imp__'):]
    # keep the largest size seen for a core name (body symbol, not a 0-size alias)
    sizes[core] = max(sizes.get(core, 0), sz)

# 2) counts: parse llvm-profdata text. Function header line "<name>:" then "    Function count: N"
counts = {}
cur = None
hdr = re.compile(r'^  (\S.*):\s*$')
cnt = re.compile(r'^\s*Function count:\s*(\d+)')
for ln in open('/tmp/m0_counts.txt'):
    m = hdr.match(ln)
    if m:
        cur = m.group(1).strip()
        continue
    m = cnt.match(ln)
    if m and cur is not None:
        core = cur
        if core.startswith('__imp__'): core = core[len('__imp__'):]
        counts[core] = int(m.group(1))
        cur = None

print(f"  parsed: {len(sizes)} sized text-symbols, {len(counts)} profiled functions")

# 3) join on core name
joined = []  # (count, size, name)
matched_size = 0
for name, sz in sizes.items():
    c = counts.get(name)
    if c is None:    # unprofiled symbol (helper, never-run) -> treat as count 0 (coldest)
        c = 0
    joined.append((c, sz, name))
total_text = sum(sz for _,sz,_ in joined)
total_exec = sum(c for c,_,_ in joined)
matched = sum(1 for c,_,_ in joined if counts.get(_[2] if False else None) is not None)
n_profiled_join = sum(1 for c,s,n in joined if n in counts)
print(f"  joined symbols: {len(joined)}  (of which profiled: {n_profiled_join})")
print(f"  total .text in joined symbols: {total_text/1e6:.2f} MB")
print(f"  total execution count: {total_exec:,}")
print()

# 4) sort coldest-first; cumulative size removed as we interpret the cold tail,
#    and the execution-coverage we'd be LOSING from native (= interpreting).
joined.sort(key=lambda t:(t[0], -t[1]))  # coldest first; within count, biggest first
L3 = 12*1024*1024
cum_removed = 0
cum_exec_interp = 0
print("  cold-tail interpreted -> remaining native .text  (exec% sent to interpreter)")
print("  %funcs |  interp .text removed | remaining native .text | vs L3 | exec% interpreted")
marks = [0.5,0.6,0.7,0.8,0.9,0.95,0.99,0.999,1.0]
import bisect
N = len(joined)
mi = 0
for i,(c,sz,n) in enumerate(joined, start=1):
    cum_removed += sz
    cum_exec_interp += c
    frac = i/N
    if mi < len(marks) and frac >= marks[mi]:
        remaining = total_text - cum_removed
        execpct = 100.0*cum_exec_interp/total_exec if total_exec else 0
        print(f"  {marks[mi]*100:5.1f}% | {cum_removed/1e6:8.2f} MB removed | {remaining/1e6:8.2f} MB native | {remaining/L3:4.2f}x | {execpct:7.4f}% exec interpreted")
        mi += 1

# 5) the DECISIVE framing: keep native only the hot set covering 99.9% / 99.99% of execution;
#    everything else interpreted. Report the resulting native .text.
joined.sort(key=lambda t:(-t[0], -t[1]))  # hottest first
for cover in [0.99, 0.999, 0.9999, 0.99999]:
    need = total_exec*cover
    acc_exec=0; acc_size=0; k=0
    for c,sz,n in joined:
        if acc_exec >= need: break
        acc_exec += c; acc_size += sz; k+=1
    print(f"  HOT-SET covering {cover*100:.3f}% of execution = {k} funcs, native .text = {acc_size/1e6:.2f} MB ({acc_size/L3:.2f}x L3)")
PY
echo "M0_PROBE_DONE"
