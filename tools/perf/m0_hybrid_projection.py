import re

# ---- 1) per-symbol .text sizes from nm -S (already dumped) ----
sizes = {}
for ln in open('/tmp/m0_syms.txt'):
    p = ln.split()
    if len(p) < 4:
        continue
    try:
        sz = int(p[1], 16)
    except ValueError:
        continue
    name = p[3]
    while name.startswith('__imp__'):
        name = name[len('__imp__'):]
    sizes[name] = max(sizes.get(name, 0), sz)

# ---- 2) per-function execution hotness from llvm-profdata text ----
# Format:
#   Counters:
#     <name>:
#       Hash: 0x..
#       Counters: N
#       Block counts: [a, b, c, ...]
hdr = re.compile(r'^  ([^\s].*):\s*$')          # 2-space indent, name, trailing colon
blk = re.compile(r'^    Block counts:\s*\[(.*)\]\s*$')
counts = {}      # name -> sum of block counts (total block executions ~ instr hotness)
entry  = {}      # name -> entry-block count (call count)
cur = None
for ln in open('/tmp/m0_counts.txt'):
    m = hdr.match(ln)
    if m:
        cur = m.group(1).strip()
        while cur.startswith('__imp__'):
            cur = cur[len('__imp__'):]
        continue
    m = blk.match(ln)
    if m and cur is not None:
        nums = [int(x) for x in m.group(1).split(',') if x.strip().lstrip('-').isdigit()]
        if nums:
            counts[cur] = sum(nums)
            entry[cur]  = nums[0]
        cur = None

print(f"  parsed: {len(sizes)} sized text-symbols, {len(counts)} profiled functions")

# ---- 3) join: keep ONLY recompiled guest funcs (sub_ / __save / __rest) for the .text-shrink math ----
def is_guest(n):
    return n.startswith('sub_') or n.startswith('__save') or n.startswith('__rest')

joined = []
for name, sz in sizes.items():
    if not is_guest(name):
        continue
    c = counts.get(name, 0)        # unprofiled guest fn -> never ran -> coldest
    joined.append((c, sz, name))

total_text = sum(s for _, s, _ in joined)
total_exec = sum(c for c, _, _ in joined)
n_prof = sum(1 for c, _, _ in joined if c > 0)
L3 = 12 * 1024 * 1024
print(f"  guest funcs: {len(joined)}  (executed at least once: {n_prof}, never: {len(joined)-n_prof})")
print(f"  total guest .text: {total_text/1e6:.2f} MB   total block-exec: {total_exec:,}")
print()

# ---- 4) HOT-SET: keep native the hottest funcs covering X% of execution; interpret the rest ----
joined.sort(key=lambda t: (-t[0], -t[1]))   # hottest first
print("  keep-native = hot set covering X% of execution; rest interpreted (~0 .text):")
print("   coverage |  #native funcs | native .text | vs 12MB L3 | #interpreted")
for cover in [0.90, 0.95, 0.99, 0.999, 0.9999, 0.99999, 1.0]:
    need = total_exec * cover
    acc = 0; sz = 0; k = 0
    for c, s, n in joined:
        if acc >= need and c < (joined[0][0]):  # stop once covered (but always include nonzero head)
            break
        if acc >= need:
            break
        acc += c; sz += s; k += 1
    interp = len(joined) - k
    print(f"   {cover*100:8.3f}% | {k:13d} | {sz/1e6:8.2f} MB | {sz/L3:6.2f}x | {interp:12d}")

# ---- 5) inverse view: interpret the coldest N% of FUNCTIONS, how much native .text remains ----
joined.sort(key=lambda t: (t[0], -t[1]))    # coldest first
print()
print("  interpret coldest X% of funcs -> remaining native .text  (exec% handed to interpreter):")
print("   %funcs-interp | native .text left | vs L3 | exec% interpreted | #native left")
N = len(joined)
cumsz = 0; cumc = 0
marks = [0.5,0.6,0.7,0.8,0.9,0.95,0.99]; mi = 0
for i,(c,s,n) in enumerate(joined, start=1):
    cumsz += s; cumc += c
    if mi < len(marks) and i/N >= marks[mi]:
        left = total_text - cumsz
        ep = 100.0*cumc/total_exec if total_exec else 0
        print(f"   {marks[mi]*100:11.1f}% | {left/1e6:9.2f} MB left | {left/L3:4.2f}x | {ep:10.5f}% | {N-i:11d}")
        mi += 1
