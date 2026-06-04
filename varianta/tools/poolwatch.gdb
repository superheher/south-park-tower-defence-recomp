set pagination off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
# Variant-A pool writer-finder: the content draws' vfetch source = slot-0 pool 0xA2000000, which is
# EMPTY (0xFFFFFFFF/poison, never real verts) in variant A. variant A is NOT stripped, so a HW write-watch
# + bt names the writing recompiled fn (sub_XXXXXXXX). At the first VdSwap (g_base set), watch the pool head:
# whatever writes it (the allocator's poison-fill, and ideally a vertex-gen) is reported with a backtrace.
python
import gdb
class Pool(gdb.Breakpoint):
    def __init__(self, addr):
        gdb.Breakpoint.__init__(self, "*(unsigned int*)0x%x" % addr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.n = 0
    def stop(self):
        self.n += 1
        if self.n <= 12:
            gdb.write("=== POOL WRITE #%d (0x%x) ===\n" % (self.n, int(gdb.parse_and_eval("$pc"))))
            try: gdb.execute("bt 6")
            except Exception as e: gdb.write("bt err %s\n" % e)
        return False   # don't stop — keep running
class GetBase(gdb.Breakpoint):
    def stop(self):
        try:
            gb = int(gdb.parse_and_eval("(unsigned long)g_base"))
            gdb.write("WATCHSET g_base=0x%x slot0=0x%x slot1v=0x%x\n" % (gb, gb + 0xA2000000, gb + 0xA01FE0FC + 0x31E6C))
            Pool(gb + 0xA2000000)            # slot-0 target (content vfetch source, off 0)
            Pool(gb + 0xA01FE0FC + 0x31E6C)  # slot-1 region where real screen-coord verts appeared
            self.enabled = False
        except Exception as e:
            gdb.write("getbase err %s\n" % e)
        return False
GetBase("__imp__VdSwap")
print("=== pool-writer watch armed (set on slot-0 pool head at first VdSwap) ===")
end
run
quit
