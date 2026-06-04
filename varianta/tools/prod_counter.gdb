set pagination off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
python
import gdb
# Measure prod's pending-submission counter device+0x2b04 at the kick gate sub_821C6C80, and the
# increment/decrement rates (sub_821CCA28 / sub_821CC140). Ground truth for variant A's CPCOMPLETE drain:
# does prod's counter oscillate near 0 (kicks flow), and is inc-rate ~= dec-rate? Recomp ABI rdi=&ctx,
# rsi=base, ctx.r3.u32 at host offset 0; guest mem big-endian.
st = {"kg":0, "kick":0, "defer":0, "dec":0, "inc":0, "cmax":0}
hist = {}
def be32(base, g):
    g &= 0xffffffff
    v = int(gdb.parse_and_eval("*(unsigned int*)(%d)" % (base + g))) & 0xffffffff
    return ((v>>24)|((v>>8)&0xff00)|((v<<8)&0xff0000)|((v<<24)&0xffffffff)) & 0xffffffff
class KG(gdb.Breakpoint):
    def stop(self):
        try:
            base = int(gdb.parse_and_eval("$rsi")) & 0xffffffffffffffff
            r3 = int(gdb.parse_and_eval("*(unsigned int*)($rdi)")) & 0xffffffff
            c = be32(base, r3 + 0x2b04)
            st["kg"] += 1
            if c == 0: st["kick"] += 1
            else: st["defer"] += 1
            if c > st["cmax"]: st["cmax"] = c
            b = c if c < 8 else 8
            hist[b] = hist.get(b, 0) + 1
            if st["kg"] % 500 == 0:
                print("KG=%d kick=%d defer=%d | inc=%d dec=%d | dev=0x%x cmax=%d hist=%s" %
                      (st["kg"], st["kick"], st["defer"], st["inc"], st["dec"], r3, st["cmax"],
                       dict(sorted(hist.items()))))
        except Exception as e:
            print("kg err", e)
        return st["kg"] >= 50000
class DEC(gdb.Breakpoint):
    def stop(self): st["dec"] += 1; return False
class INC(gdb.Breakpoint):
    def stop(self): st["inc"] += 1; return False
KG("__imp__sub_821C6C80")
DEC("__imp__sub_821CC140")
INC("__imp__sub_821CCA28")
print("=== prod counter trace armed (device+0x2b04 oscillation + inc/dec rates) ===")
end
run
quit
