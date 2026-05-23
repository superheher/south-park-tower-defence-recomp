#!/usr/bin/env python3
"""Read-only STFS header recon. Prints structural measurements only (no content)."""
import struct, sys

PKG = sys.argv[1] if len(sys.argv) > 1 else \
    r"..\South Park - Let's Go Tower Defense Play!\58410931\000D0000\A7603793FD5268F4CDA9EC80D1921F0F2F8392D558"

def u32be(b, o): return struct.unpack_from(">I", b, o)[0]
def u16le(b, o): return struct.unpack_from("<H", b, o)[0]
def u24le(b, o): return b[o] | (b[o+1] << 8) | (b[o+2] << 16)
def u24be(b, o): return (b[o] << 16) | (b[o+1] << 8) | b[o+2]

with open(PKG, "rb") as f:
    head = f.read(0x1000)
    f.seek(0xC000); ftblk = f.read(0x140)
    f.seek(0x2B000); xexpk = f.read(0x10)
    f.seek(0, 2); total = f.tell()

print(f"file size           : {total} (0x{total:X})")
print(f"magic               : {head[0:4]!r}")
print(f"contentType @0x344  : 0x{u32be(head,0x344):08X}  (expect 0x000D0000)")
print(f"titleID     @0x360  : 0x{u32be(head,0x360):08X}  (expect 0x58410931)")
print(f"platform    @0x364  : {head[0x364]}")
print(f"execType    @0x365  : {head[0x365]}")
print("--- StfsVolumeDescriptor @0x379 ---")
print(f"size        @0x379  : 0x{head[0x379]:02X}  (expect 0x24)")
print(f"blockSep    @0x37B  : 0x{head[0x37B]:02X}")
print(f"fileTblBlkCnt@0x37C : {u16le(head,0x37C)} (WORD LE)")
print(f"fileTblBlkNum@0x37E : {u24le(head,0x37E)} (INT24 LE)")
print(f"allocBlkCnt @0x395  : {u32be(head,0x395)} (DWORD BE)")
print(f"unallocBlk  @0x399  : {u32be(head,0x399)} (DWORD BE)")
print("--- file table region @0xC000 (first 5 x 0x40 entries: name + key fields) ---")
for i in range(5):
    e = ftblk[i*0x40:(i+1)*0x40]
    if len(e) < 0x40: break
    nlen = e[0x28] & 0x3F
    flags = e[0x28]
    name = e[:nlen].decode("ascii", "replace") if nlen else ""
    blocks = u24le(e, 0x29)
    start = u24le(e, 0x2F)
    parent = struct.unpack_from(">H", e, 0x32)[0]
    fsize = u32be(e, 0x34)
    print(f"  [{i}] name={name!r:24} flags=0x{flags:02X} blocks={blocks} start={start} parent=0x{parent:04X} size={fsize}")
print(f"--- bytes @0x2B000 : {xexpk!r}  (expect b'XEX2...')")
