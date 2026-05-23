# Ghidra postScript (Jython): find mainCRTStartup by decompiling root functions.
# mainCRTStartup is a ROOT (kernel-called, no bl callers) that DIRECTLY calls the
# CRT init (_initterm / inline init loops) and then main/WinMain and exit. The CRT
# uses direct calls, so Ghidra resolves these edges even though the rest of the
# game is indirect/vtable. We rank roots by resolved out-degree and decompile the
# top ones; mainCRTStartup is recognizable in the C.
# @category Recomp
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

mon = ConsoleTaskMonitor()
fm = currentProgram.getFunctionManager()
funcs = list(fm.getFunctions(True))
print("[roots] total functions: %d" % len(funcs))

def callers(f):
    try: return set(f.getCallingFunctions(mon))
    except: return set()
def callees(f):
    try: return set(f.getCalledFunctions(mon))
    except: return set()

roots = [f for f in funcs if not callers(f)]
roots = [(f, len(callees(f))) for f in roots]
roots.sort(key=lambda x: -x[1])
print("[roots] %d roots; decompiling top candidates (out-degree 3..20)" % len(roots))

di = DecompInterface()
di.openProgram(currentProgram)

shown = 0
for f, od in roots:
    if od < 3 or od > 20:
        continue
    try:
        res = di.decompileFunction(f, 45, mon)
        c = res.getDecompiledFunction().getC() if res and res.decompileCompleted() else "<decompile failed>"
    except Exception as e:
        c = "<error %s>" % e
    # heuristic flags
    cl = c.lower()
    flags = []
    if "exit" in cl: flags.append("EXIT")
    if "initterm" in cl or "while" in cl: flags.append("LOOP/INITTERM?")
    print("\n================= ROOT %s out=%d %s =================" % (f.getEntryPoint(), od, " ".join(flags)))
    # print first ~30 lines of C
    for ln in c.splitlines()[:32]:
        print(ln)
    shown += 1
    if shown >= 22:
        break
print("\n[roots] done (%d decompiled)" % shown)
