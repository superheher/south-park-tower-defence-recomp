#!/usr/bin/env python3
"""Find code references to target guest addresses in the decrypted XEX image.

PPC forms a 32-bit address with `lis rX, hi` then `addi/ori rX, rX, lo`. This
linearly scans the code section, tracks each register's most-recent `lis` high
half, and reports instruction addresses that complete a reference to any target.
Read-only; reuses the decrypt+decompress from xex_decrypt. For startup RE
(finding _cinit / .CRT$XC / the dispatch-table initializer).
"""
import sys, struct
from Crypto.Cipher import AES
XEX = r"private\default.xex"
RK = bytes([0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91])
def u32(b,o): return struct.unpack_from(">I",b,o)[0]
def u16(b,o): return struct.unpack_from(">H",b,o)[0]
d=open(XEX,"rb").read(); pe=u32(d,8); sec=u32(d,0x10); n=u32(d,0x14)
opt={u32(d,0x18+i*8):u32(d,0x18+i*8+4) for i in range(n)}
load=u32(d,sec+0x110); fk=d[sec+0x150:sec+0x160]
sk=AES.new(RK,AES.MODE_ECB).decrypt(fk)
ff=opt[0x3FF]; enc=u16(d,ff+4); comp=u16(d,ff+6); base=d[pe:]
if enc==1:
    nl=(len(base)//16)*16; base=AES.new(sk,AES.MODE_CBC,b"\0"*16).decrypt(base[:nl])
out=bytearray()
if comp==1:
    p=ff+8; s=0
    for _ in range((u32(d,ff)-8)//8):
        ds=u32(d,p); zs=u32(d,p+4); p+=8; out+=base[s:s+ds]; s+=ds; out+=b"\0"*zs
else: out=bytearray(base)

targets = [int(x,16) for x in sys.argv[1:]] or [0x8260E0F0,0x82003EB0,0x820047B8,0x820048D8,0x82005018]
CODE_LO,CODE_HI = 0x82100000,0x825F0C18
def s16(v): return v-0x10000 if v & 0x8000 else v
lis_hi=[None]*32; lis_at=[0]*32
results={t:[] for t in targets}
rva0 = CODE_LO - load
for off in range(rva0, (CODE_HI-load)-3, 4):
    w=u32(out,off); op=w>>26; va=load+off
    if op==15:           # addis (lis when RA==0)
        rt=(w>>21)&31; ra=(w>>16)&31; simm=w&0xFFFF
        if ra==0: lis_hi[rt]=simm; lis_at[rt]=va
        else: lis_hi[rt]=None
    elif op==14:         # addi rT,rA,SIMM
        rt=(w>>21)&31; ra=(w>>16)&31; simm=w&0xFFFF
        if ra==rt and lis_hi[rt] is not None:
            addr=((lis_hi[rt]<<16)+s16(simm))&0xFFFFFFFF
            if addr in results: results[addr].append((va,"addi",lis_at[rt]))
        lis_hi[rt]=None
    elif op==24:         # ori rA,rS,UIMM
        rs=(w>>21)&31; ra=(w>>16)&31; uimm=w&0xFFFF
        if ra==rs and lis_hi[rs] is not None:
            addr=((lis_hi[rs]<<16)|uimm)&0xFFFFFFFF
            if addr in results: results[addr].append((va,"ori",lis_at[rs]))
        lis_hi[ra]=None
    else:
        # crude: invalidate destination reg of common reg-writing ops
        rt=(w>>21)&31
        if op in (32,33,34,35,36,37,40,41,43,46,7,8,12,13,28,29,21,23): lis_hi[rt]=None

for t in targets:
    print(f"=== xrefs to 0x{t:08X}: {len(results[t])} ===")
    for va,kind,lis in results[t][:8]:
        print(f"  0x{va:08X} ({kind}, lis@0x{lis:08X})")
