#!/usr/bin/env python3
"""Find functions with ZERO references anywhere (kernel-callback-only) — the small
set that includes the XEX entry stub AND mainCRTStartup.

mainCRTStartup is invoked only by the kernel (it's the XEX entry on normal titles),
so its address appears nowhere in the image: no `bl` targets it and no data pointer
points to it. Same for the XEX entry, TLS/exception callbacks, etc. So the set of
.pdata functions that are neither bl-targeted nor data-referenced is small and is
the best brute-force candidate pool for "start the recomp here".

Read-only. Usage: find_zeroref_roots.py <image.bin>
"""
import struct, sys
from capstone import Cs, CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, CS_MODE_32

img = open(sys.argv[1], "rb").read(); base = 0x82000000; N = len(img)
TEXT_LO, TEXT_HI = 0x82100000, 0x825F0C18
def u32(o): return struct.unpack_from("<I", img, o)[0]
def b32(o): return struct.unpack_from(">I", img, o)[0]

# .pdata starts (auto-locate, big-endian)
e=u32(0x3C); dd=e+24+0x60; pd=u32(dd+3*8); i=max(0,pd-0x4000); pdoff=pd; n=0
while i<min(pd+0x4000,N-8):
    v=b32(i)
    if 0x82100000<=v<0x826E0000:
        j=i;c=0;p=0
        while j<N-8:
            w=b32(j)
            if 0x82000000<=w<0x82700000 and w>=p: c+=1;p=w;j+=8
            else: break
        if c>64: pdoff,n=i,c;break
    i+=4
starts=sorted(set(b32(pdoff+k*8) for k in range(n)))
startset=set(starts)

# bl targets (global scan)
bl_targets=set()
for va in range(TEXT_LO, TEXT_HI, 4):
    w=b32(va-base)
    if (w>>26)==18 and (w&1) and not (w&2):
        li=w&0x03FFFFFC
        if li&0x02000000: li-=0x04000000
        t=(va+li)&0xFFFFFFFF
        if TEXT_LO<=t<TEXT_HI: bl_targets.add(t)

# data pointers into .text: scan .rdata + .data (and whole non-.text image) for BE words in startset
data_refs=set()
# scan everything except .text code region for big-endian pointers to function starts
for region_lo, region_hi in [(0x600,0xE9C00),(0x5EE800,0x7EE000)]:  # .rdata-ish, .data-ish (file offsets)
    o=region_lo
    while o < min(region_hi, N-4):
        w=b32(o)
        if w in startset: data_refs.add(w)
        o+=4

MFLR_R12 = 0x7D8802A6  # mflr r12 = standard non-leaf prologue start
zeroref=[s for s in starts if s not in bl_targets and s not in data_refs]
# keep only real function starts (begin with `mflr r12`) -> the CRT entry has this
cands=[s for s in zeroref if b32(s-base)==MFLR_R12]
print(f"{len(starts)} funcs; {len(bl_targets)} bl-targeted; {len(data_refs)} data-referenced")
print(f"zero-ref: {len(zeroref)}; zero-ref AND prologue(mflr r12): {len(cands)}")
out=open("private/entry_candidates.txt","w")
for s in cands:
    out.write("%08X\n"%s)
out.close()
print("wrote private/entry_candidates.txt")
# show the ones in the CRT-ish regions first (early .text + near the XEX entry)
md=Cs(CS_ARCH_PPC,CS_MODE_BIG_ENDIAN+CS_MODE_32); md.skipdata=True
def hint(s):
    ins=list(md.disasm(img[s-base:s-base+12], s))[:3]
    return " ; ".join(f"{x.mnemonic} {x.op_str}" for x in ins)
print("--- candidates in 0x82100000-0x82110000 (early CRT) ---")
for s in cands:
    if 0x82100000<=s<0x82110000: print(f"  0x{s:08X}  {hint(s)}")
print("--- candidates in 0x82440000-0x82460000 (XAPI/CRT near entry) ---")
for s in cands:
    if 0x82440000<=s<0x82460000: print(f"  0x{s:08X}  {hint(s)}")
