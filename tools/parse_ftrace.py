#!/usr/bin/env python3
"""Parse Xenia's function-trace data (trace_function_data -> ftrace.0).

Per-function blob (little-endian, written by Xenia on x64):
  +0  u32 data_size      (= 48 + instr_count*8)
  +4  u32 start_address  (guest)
  +8  u32 end_address    (guest)
  +12 u32 type
  +16 u64 function_thread_use  (bitmask of thread ids that ran it)
  +24 u64 function_call_count
  +32 u32 caller_history[4]    (recent caller guest addresses)
  +48 u64 instruction_execute_count[instr_count]
Refs: xenia/src/xenia/cpu/function_trace_data.h

Usage: parse_ftrace.py <ftrace.0> [callersof <hex> | calleesof <hex> | entry | summary]
"""
import struct, sys, bisect

data = open(sys.argv[1], "rb").read()
cmd = sys.argv[2] if len(sys.argv) > 2 else "summary"
N = len(data)

funcs = {}       # start -> dict(end, calls, threads, callers[4])
o = 0
parsed = 0
while o + 48 <= N:
    data_size, start, end, typ = struct.unpack_from("<IIII", data, o)
    if not (0x82000000 <= start < 0x82930000 and start <= end < 0x82930000):
        # not a valid blob here; try to resync by scanning forward 4 bytes
        o += 4
        continue
    instr_count = (end - start) // 4 + 1
    expect = 48 + instr_count * 8
    if data_size != expect or data_size <= 0 or o + data_size > N + 8:
        o += 4
        continue
    thr, calls = struct.unpack_from("<QQ", data, o + 16)
    ch = struct.unpack_from("<IIII", data, o + 32)
    funcs[start] = {"end": end, "calls": calls, "threads": thr,
                    "callers": [c for c in ch if c], "off": o}
    parsed += 1
    o += data_size

starts = sorted(funcs)
def containing(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 and a < funcs[starts[i]]["end"] else (starts[i] if i>=0 else None)

if cmd == "summary":
    print(f"parsed {parsed} executed functions; range 0x{starts[0]:08X}..0x{starts[-1]:08X}")
    # functions with the most calls (hot) and a few low-address (CRT) ones
    hot = sorted(funcs.items(), key=lambda kv: -kv[1]["calls"])[:5]
    print("hottest:", " ".join(f"{s:08X}({d['calls']})" for s,d in hot))
elif cmd == "entry":
    # the entry function + neighbors, with caller history
    for a in (0x82449968, 0x824499A0, 0x824499D0):
        cf = containing(a)
        if cf is not None and cf in funcs:
            d = funcs[cf]
            print(f"exec func 0x{cf:08X} (contains 0x{a:08X}) end=0x{d['end']:08X} calls={d['calls']} thr=0x{d['threads']:X} callers={[hex(c) for c in d['callers']]}")
        else:
            print(f"0x{a:08X}: NOT in executed set")
elif cmd == "callersof":
    t = int(sys.argv[3], 16); tf = containing(t) or t
    print(f"callers of 0x{t:08X} (func 0x{tf:08X}): {[hex(c) for c in funcs.get(tf,{}).get('callers',[])]}")
elif cmd == "roots":
    # functions whose caller_history includes a sentinel (0xE0000000 = thread start)
    # or a given caller; shows thread bitmask -> identifies thread entries/continuations
    target = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xE0000000
    rs = [(s, d) for s, d in funcs.items() if any((c & 0xFF000000) == (target & 0xFF000000) and c >= 0xE0000000 for c in d["callers"])] if target >= 0xE0000000 else \
         [(s, d) for s, d in funcs.items() if target in d["callers"]]
    rs.sort()
    print(f"functions with caller ~0x{target:08X} ({len(rs)}):")
    for s, d in rs:
        print(f"  0x{s:08X}  thr=0x{d['threads']:X} calls={d['calls']} callers={[hex(c) for c in d['callers']]}")
elif cmd == "thread":
    # all functions that ran on a given thread bit (e.g. 0x40 = thread 6 main)
    bit = int(sys.argv[3], 16)
    ts = sorted([(s, d) for s, d in funcs.items() if d["threads"] & bit])
    print(f"functions on thread mask 0x{bit:X} ({len(ts)}), sorted by address:")
    for s, d in ts[:60]:
        print(f"  0x{s:08X} calls={d['calls']} callers={[hex(c) for c in d['callers']]}")
elif cmd == "outermost":
    # boot chain order ~ stack depth: a callee called from a HIGHER stack address is
    # OUTER (closer to the entry frame). Rank funcs by their max stack-addr caller.
    def maxstk(d):
        s = [c for c in d["callers"] if 0x70000000 <= c < 0x71000000]
        return max(s) if s else 0
    rs = sorted(((maxstk(d), s, d) for s, d in funcs.items() if maxstk(d)), reverse=True)
    print("functions called from the highest stack frames (outermost boot chain first):")
    for stk, s, d in rs[:25]:
        print(f"  caller_stk=0x{stk:08X} -> func 0x{s:08X} calls={d['calls']} thr=0x{d['threads']:X}")
elif cmd == "initarray":
    # the .rdata init-array slots that called boot functions = __xi/__xc bounds
    slots = sorted(set(c for d in funcs.values() for c in d["callers"] if 0x82000600 <= c < 0x820E8A9C))
    if slots:
        print(f"init-array slots (.rdata) that invoked boot fns: {len(slots)}, range 0x{slots[0]:08X}..0x{slots[-1]:08X}")
        for c in slots: print(f"  0x{c:08X}")
elif cmd == "insns":
    # dump which instructions of a function actually executed (and counts)
    t = int(sys.argv[3], 16); tf = containing(t) or t
    d = funcs.get(tf)
    if not d: print(f"0x{t:08X} not in executed set"); sys.exit()
    base = d["off"] + 48; ic = (d["end"] - tf)//4 + 1
    print(f"func 0x{tf:08X}..0x{d['end']:08X} ({ic} insns); executed instructions:")
    last_run = None
    for i in range(ic):
        cnt = struct.unpack_from("<Q", data, base + i*8)[0]
        if cnt:
            a = tf + i*4
            print(f"  0x{a:08X}  x{cnt}")
            last_run = tf + i*4
    print(f"  -> last executed instruction: 0x{last_run:08X}" if last_run else "  (none ran)")
elif cmd == "calleesof":
    # functions whose caller_history includes the given function's range
    t = int(sys.argv[3], 16); tf = containing(t) or t
    te = funcs.get(tf, {}).get("end", t + 0x200)
    hits = [s for s, d in funcs.items() if any(tf <= c < te for c in d["callers"])]
    print(f"functions called by 0x{tf:08X}..0x{te:08X} ({len(hits)}): " + " ".join(f"{h:08X}" for h in sorted(hits)[:30]))
