set pagination off
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
python
import gdb
st={'armed':False,'n':0}
def be(v):
    v&=0xffffffff
    return ((v>>24)|((v>>8)&0xff00)|((v<<8)&0xff0000)|((v<<24)&0xffffffff))&0xffffffff
class WS(gdb.Breakpoint):
    def __init__(s,addr): super().__init__("*(unsigned int*)0x%x"%addr,gdb.BP_WATCHPOINT,gdb.WP_WRITE); s.addr=addr
    def stop(s):
        st['n']+=1
        v=int(gdb.parse_and_eval("*(unsigned int*)0x%x"%s.addr))&0xffffffff
        f=gdb.newest_frame();names=[]
        for _ in range(4):
            if not f:break
            names.append(f.name() or "?");f=f.older()
        print("vSTATE#%d -> %d  writer: %s"%(st['n'],be(v)," <- ".join(names)),flush=True)
        return st['n']>=24
class Trig(gdb.Breakpoint):
    def __init__(s): super().__init__("__imp__sub_82248010")
    def stop(s):
        if st['armed']:return False
        base=int(gdb.parse_and_eval("$rsi"))&0xffffffffffffffff
        WS(base+0x82657600); st['armed']=True
        print("=== variant-A state watch armed (base 0x%x) ==="%base,flush=True); s.enabled=False
        return False
Trig()
end
run
quit
