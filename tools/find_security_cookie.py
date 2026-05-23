#!/usr/bin/env python3
"""Find __security_cookie -> __security_init_cookie -> mainCRTStartup.

__security_cookie is a .data global written exactly ONCE (by __security_init_cookie,
called first thing in mainCRTStartup) and read by EVERY /GS-protected function's
prologue/epilogue. So: the .data address with ~1 writer and many readers is the
cookie; its writer function is __security_init_cookie; that function's bl-callers
include mainCRTStartup.

Scans .text (capstone, skipdata) for lis+lwz (read) / lis+stw (write) to .data.
Read-only. Usage: find_security_cookie.py <image.bin>
"""
import struct, sys, bisect
from capstone import Cs, CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, CS_MODE_32

img = open(sys.argv[1], "rb").read()
base = 0x82000000
TEXT_LO, TEXT_HI = 0x82100000, 0x825F1000
DATA_LO, DATA_HI = 0x82600000, 0x82903000

def b32(o): return struct.unpack_from(">I", img, o)[0]

md = Cs(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN + CS_MODE_32); md.detail=False; md.skipdata=True
reads, writes = {}, {}     # data_addr -> [site_addr,...]
read_sites, write_sites = [], []  # (site, data_addr)
lis_hi = {}
for ins in md.disasm(img[TEXT_LO-base:TEXT_HI-base], TEXT_LO):
    m=ins.mnemonic; ops=ins.op_str
    try:
        if m=="lis":
            r,v=ops.split(","); lis_hi[r.strip()]=int(v.strip(),0)&0xFFFF
        elif m in ("lwz","stw"):
            # form: rD, disp(rA)
            d=ops.split(",")
            rd=d[0].strip(); rest=d[1].strip()
            disp=int(rest[:rest.index("(")],0); rA=rest[rest.index("(")+1:-1]
            if rA in lis_hi:
                addr=((lis_hi[rA]<<16)+disp)&0xFFFFFFFF
                if DATA_LO<=addr<DATA_HI:
                    if m=="lwz": reads.setdefault(addr,0); reads[addr]+=1; read_sites.append((ins.address,addr))
                    else: writes.setdefault(addr,0); writes[addr]+=1; write_sites.append((ins.address,addr))
                # base reg consumed for addressing but lis may be reused; clear to avoid stale
                if rd==rA and rd in lis_hi: del lis_hi[rA]
        elif m in ("addi","ori") :
            d=[p.strip() for p in ops.split(",")]
            if len(d)==3 and d[0]==d[1] and d[0] in lis_hi: del lis_hi[d[0]]
    except Exception: pass

# candidate cookie: many readers, very few writers
cands=[]
for a,nr in reads.items():
    nw=writes.get(a,0)
    if nr>=8 and nw<=3 and nw>=1:
        cands.append((nr,nw,a))
cands.sort(reverse=True)
print("data globals with many readers + few writers (cookie candidates):")
for nr,nw,a in cands[:12]:
    ws=[s for s,da in write_sites if da==a]
    print(f"  0x{a:08X}  readers={nr} writers={nw}  write_sites={' '.join('%08X'%w for w in ws[:4])}")
