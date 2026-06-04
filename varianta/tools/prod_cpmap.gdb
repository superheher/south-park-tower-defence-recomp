set pagination off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
python
import gdb
# Map prod's host command-processor buffer hierarchy: primary ring -> indirect buffers,
# with guest addresses + dword counts, plus draw tallies, bounded to ~3 frames (INTERRUPTs).
st = {"ib":0, "prim":0, "draw":0, "draw_since":0, "int":0}
class IB(gdb.Breakpoint):
    def stop(self):
        try:
            ptr = int(gdb.parse_and_eval("$rsi")) & 0xffffffff
            cnt = int(gdb.parse_and_eval("$rdx")) & 0xffffffff
            st["ib"] += 1
            print("  IB    ptr=0x%08x count=%u dw   (draws in prev buf=%d)" % (ptr, cnt, st["draw_since"]))
            st["draw_since"] = 0
        except Exception as e: print("ib err", e)
        return False
class PRIM(gdb.Breakpoint):
    def stop(self):
        try:
            a = int(gdb.parse_and_eval("$rsi")) & 0xffffffff
            b = int(gdb.parse_and_eval("$rdx")) & 0xffffffff
            st["prim"] += 1
            print("PRIMARY start=%u end=%u" % (a, b))
        except Exception as e: print("prim err", e)
        return False
class DRAW(gdb.Breakpoint):
    def stop(self):
        st["draw"] += 1; st["draw_since"] += 1
        return False
class INT(gdb.Breakpoint):
    def stop(self):
        st["int"] += 1
        print("=== INTERRUPT (frame %d): total IBs=%d prims=%d draws=%d ===" % (st["int"], st["ib"], st["prim"], st["draw"]))
        return st["int"] >= 3
IB("rex::graphics::CommandProcessor::ExecuteIndirectBuffer(unsigned int, unsigned int)")
PRIM("rex::graphics::CommandProcessor::ExecutePrimaryBuffer(unsigned int, unsigned int)")
DRAW("rex::graphics::CommandProcessor::ExecutePacketType3_DRAW_INDX(rex::memory::RingBuffer*, unsigned int, unsigned int)")
INT("rex::graphics::CommandProcessor::ExecutePacketType3_INTERRUPT(rex::memory::RingBuffer*, unsigned int, unsigned int)")
print("=== prod CP buffer-map trace armed ===")
end
run
quit
