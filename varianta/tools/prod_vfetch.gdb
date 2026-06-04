# ⚠⚠ BLOCKED (2026-06-04): the prod binary has NO DWARF (readelf -S shows zero .debug_* sections — only the
# .symtab function names that let breakpoints resolve). So gdb has NO TYPE INFO: `this`, the
# `rex::graphics::CommandProcessor*` cast, and `ptype` ALL fail ("No symbol table is loaded"). The typed
# register-file read below CANNOT work on this binary. To finish the prod-oracle: (a) REBUILD prod with debug
# info (cmake -DCMAKE_BUILD_TYPE=Debug, or -g), or (b) compute CommandProcessor::register_file_ + RegisterFile
# ::values raw offsets FROM SOURCE (rexglue-sdk command_processor.h + register_file.h class layout — error-prone:
# base classes/vtable/padding) and read *(uint32_t*)($rdi + reg_off + values_off + 0x4800*4). The breakpoint +
# progc-filter scaffold is correct; only the member access needs (a) or (b). ⇒ The tractable NO-PROD resolver
# is instead a VARIANT-A hardware watchpoint on the vertex region (variant A is NOT stripped: bt gives the
# sub_XXXXXXXX vertex-gen writer) to find why the content vfetch source (slot 0 = 0xA2000000) is empty.
set pagination off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
python
import gdb
# Prod-oracle vertex-fetch probe: at each DRAW that uses a MENU CONTENT shader (SQ_PROGRAM_CNTL
# 0x10010001 / 0x10110101 — the prim-5/prim-13 UI shaders RE'd in variant A), read prod's fetch-constant
# slot 0 (the vfetch source). Variant A's slot 0 = 0x02000000 (type 0/2) pointing at a POISON pool; if
# prod's slot 0 is a VALID kVertex constant (type 3, real addr) with real verts, the vertex data IS set
# up in prod and variant A's poison is a variant-A vertex-lifecycle gap.
st = {"d": 0}
class DRAW(gdb.Breakpoint):
    def stop(self):
        try:
            v = gdb.parse_and_eval("this->register_file_->values")
            progc = int(v[0x2180]) & 0xffffffff
            st["seen"] = st.get("seen", 0) + 1
            if progc not in (0x10010001, 0x10110101):
                return False
            d0 = int(v[0x4800]) & 0xffffffff
            d1 = int(v[0x4801]) & 0xffffffff
            surf = int(v[0x2000]) & 0xffffffff
            base = d0 & 0xfffffffc
            st["d"] += 1
            print("MENUDRAW #%d (of %d draws) progc=0x%08x | SLOT0 d0=0x%08x d1=0x%08x type=%d base=0x%x | surf=0x%08x"
                  % (st["d"], st["seen"], progc, d0, d1, d0 & 3, base, surf))
            try:
                hp = gdb.parse_and_eval("(float*)this->memory_->TranslatePhysical(%d)" % base)
                print("   verts@base: %g %g %g %g %g %g %g %g" % tuple(float(hp[i]) for i in range(8)))
            except Exception as e2:
                print("   (vert read failed: %s)" % e2)
            return st["d"] >= 8
        except Exception as e:
            st["err"] = st.get("err", 0) + 1
            if st["err"] <= 3: print("err", e)
            return False
DRAW("rex::graphics::CommandProcessor::ExecutePacketType3Draw(rex::memory::RingBuffer*, unsigned int, char const*, unsigned int, unsigned int)")
print("=== prod vfetch probe armed (menu content draws) ===")
end
run
quit
