# Ghidra preScript (Jython): define functions at every .pdata start before
# auto-analysis, so the analyzer builds an accurate call graph.
# Reads private/pdata_starts.txt (one hex VA per line).
# @category Recomp
import os
cands = [
    r"private\pdata_starts.txt",
    os.path.join(os.getcwd(), "private", "pdata_starts.txt"),
]
sf = next((p for p in cands if os.path.exists(p)), cands[0])
starts = [int(x, 16) for x in open(sf).read().split()]
made = 0
for s in starts:
    a = toAddr(s)
    if getInstructionAt(a) is None:
        try:
            disassemble(a)
        except:
            pass
    if getFunctionAt(a) is None:
        try:
            createFunction(a, None)
            made += 1
        except:
            pass
print("[pre] defined %d new functions from %d .pdata starts" % (made, len(starts)))
