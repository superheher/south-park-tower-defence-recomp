set pagination off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
# Read-only prod-oracle vertex-pool dump (NO DWARF needed, does NOT modify prod = guardrail-safe).
# variant A: the content draws' vfetch source slot-0 (phys 0x2000000 -> guest 0xA2000000) is FILLER
# (never real verts); the verts sit in slot-1 (guest 0xA01FE0FC). Question: does PROD (which renders the
# menu) populate slot-0 with real verts? Prod base = 0x100000000, physical window 0xA0000000|(p&0x1FFFFFFF),
# so prod host(slot0)=0x1A2000000, host(slot1)=0x1A01FE0FC. Sample at several frames (count INTERRUPTs) to
# pass the movie + reach the menu; dump both pools as floats. Real screen-coord floats at slot0 in prod (vs
# filler in variant A) => variant A's vertex PLACEMENT is the bug; the verts belong at slot0.
python
import gdb
st = {"f": 0}
DUMP_AT = [150, 350, 600, 900, 1200]
def dump(tag):
    gdb.write("=== %s ===\n" % tag)
    for name, g in (("slot0", 0xA2000000), ("slot1", 0xA01FE0FC)):
        host = 0x100000000 + g
        gdb.write("%s guest=0x%x host=0x%x:\n" % (name, g, host))
        try: gdb.execute("x/16fw 0x%x" % host)
        except Exception as e: gdb.write("  err %s\n" % e)
class INT(gdb.Breakpoint):
    def stop(self):
        st["f"] += 1
        if st["f"] in DUMP_AT:
            dump("FRAME %d" % st["f"])
        return st["f"] >= DUMP_AT[-1]
INT("rex::graphics::CommandProcessor::ExecutePacketType3_INTERRUPT(rex::memory::RingBuffer*, unsigned int, unsigned int)")
print("=== prod vtx-pool dump armed ===")
end
run
quit
