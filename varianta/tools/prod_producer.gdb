set pagination off
set print frame-arguments none
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
python
import gdb
# Trace the render-work PRODUCER sub_821CC7A0(r3=work item). Read r3 and the work-item's
# draw handler *(item+16) — the field that is null in variant A. Recomp ABI: rdi=&ctx, rsi=base;
# ctx.r3.u32 at host offset 0; guest memory is big-endian.
class Prod(gdb.Breakpoint):
    def __init__(self):
        super().__init__("*0x6f2f20")   # __imp__sub_821CC7A0 entry
        self.n = 0
    def stop(self):
        self.n += 1
        try:
            base = int(gdb.parse_and_eval("$rsi")) & 0xffffffffffffffff
            r3   = int(gdb.parse_and_eval("*(unsigned int*)($rdi)")) & 0xffffffff
            def rd(g):
                g &= 0xffffffff
                v = int(gdb.parse_and_eval("*(unsigned int*)(%d)" % (base + g))) & 0xffffffff
                return ((v>>24)|((v>>8)&0xff00)|((v<<8)&0xff0000)|((v<<24)&0xffffffff)) & 0xffffffff
            if r3:
                print("PROD-ENQ#%d item(r3)=0x%08x vt=*item=0x%08x +4=0x%08x +8=0x%08x +12=0x%08x +16=0x%08x(HANDLER) +20=0x%08x"
                      % (self.n, r3, rd(r3), rd(r3+4), rd(r3+8), rd(r3+12), rd(r3+16), rd(r3+20)))
            else:
                print("PROD-ENQ#%d item(r3)=0 (null arg)" % self.n)
        except Exception as e:
            print("oracle-read err:", e)
        return self.n >= 12
Prod()
print("=== prod producer trace armed (break sub_821CC7A0, read *(item+16)) ===")
end
run
echo \n=== reached 12 producer fires; backtrace of last ===\n
bt 6
quit
