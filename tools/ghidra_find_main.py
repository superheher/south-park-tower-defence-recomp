# Ghidra postScript (Jython): find CRT-startup / main candidates using the
# resolved call graph (Ghidra resolves many indirect/computed calls that a raw
# bl-scan misses). Prints:
#   - the XEX entry (0x824499A0) and what it calls
#   - "roots" (no callers) ranked by out-degree (mainCRTStartup is a high-out root)
#   - functions that reach an _initterm-shaped callee
# @category Recomp
from ghidra.util.task import ConsoleTaskMonitor
fm = currentProgram.getFunctionManager()
mon = ConsoleTaskMonitor()

funcs = list(fm.getFunctions(True))
print("[post] total functions: %d" % len(funcs))

def callees(f):
    try: return set(f.getCalledFunctions(mon))
    except: return set()
def callers(f):
    try: return set(f.getCallingFunctions(mon))
    except: return set()

# index
out_deg = {}
in_deg = {}
for f in funcs:
    out_deg[f] = len(callees(f))
    in_deg[f] = len(callers(f))

ENTRY = 0x824499A0
ef = fm.getFunctionContaining(toAddr(ENTRY))
print("\n=== XEX entry 0x%08X ===" % ENTRY)
if ef:
    print("  containing func: %s @ %s" % (ef.getName(), ef.getEntryPoint()))
    print("  callees: " + ", ".join(str(c.getEntryPoint()) for c in callees(ef)))
    print("  callers: " + ", ".join(str(c.getEntryPoint()) for c in callers(ef)))

# roots = no callers, ranked by out-degree
roots = [f for f in funcs if in_deg[f] == 0]
roots.sort(key=lambda f: -out_deg[f])
print("\n=== top 40 roots (no callers) by out-degree ===")
for f in roots[:40]:
    print("  %s  out=%d  in=%d  %s" % (f.getEntryPoint(), out_deg[f], in_deg[f], f.getName()))

# _initterm-shaped: small function with an indirect/computed call inside a loop.
# Heuristic: <= 0x80 bytes body, has a CALL with no resolved target (indirect) or
# a back-edge. We approximate by: function body small AND it is called with 2 args.
# Simpler signal: list functions whose callers include a root and that themselves
# have an indirect call. Print small funcs that many roots call.
small_called_by_roots = {}
rootset = set(roots)
for f in funcs:
    body = f.getBody().getNumAddresses()
    if body <= 0x120:
        rc = [c for c in callers(f) if c in rootset]
        if rc:
            small_called_by_roots[f] = len(rc)
ranked = sorted(small_called_by_roots.items(), key=lambda kv: -kv[1])
print("\n=== small funcs called by roots (init/_initterm-ish), top 25 ===")
for f, c in ranked[:25]:
    print("  %s  body=0x%X  root_callers=%d" % (f.getEntryPoint(), f.getBody().getNumAddresses(), c))
print("\n[post] done")
