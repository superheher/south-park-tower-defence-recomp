#!/usr/bin/env python3
"""Inspect the decrypted XEX PE image: sections, data directories, TLS callbacks.

The decrypted XEX basefile is the *loaded* image (RVA == file offset). Xbox 360
PEs are little-endian in their PE structures (the headers), big-endian for guest
data. Entry/CRT discovery: locates the TLS directory + callback list and prints
section bounds so we can find the C++ static-init array (.CRT / .rdata).

Usage: pe_inspect.py <image.bin> [load_base=0x82000000]
Read-only.
"""
import struct, sys

img  = open(sys.argv[1], "rb").read()
base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x82000000

def u16(o): return struct.unpack_from("<H", img, o)[0]
def u32(o): return struct.unpack_from("<I", img, o)[0]

e_lfanew = u32(0x3C)
assert img[e_lfanew:e_lfanew+4] == b"PE\x00\x00", "not a PE"
machine   = u16(e_lfanew + 4)
nsec      = u16(e_lfanew + 6)
opt_off   = e_lfanew + 24
opt_magic = u16(opt_off)
image_base = u32(opt_off + 0x1C)            # PE32 ImageBase
aoe       = u32(opt_off + 0x10)             # AddressOfEntryPoint (RVA)
ndd       = u32(opt_off + 0x5C)             # NumberOfRvaAndSizes
dd_off    = opt_off + 0x60                  # DataDirectory[]
print(f"machine=0x{machine:04X} sections={nsec} opt_magic=0x{opt_magic:04X}")
print(f"ImageBase=0x{image_base:08X} AddressOfEntryPoint RVA=0x{aoe:X} -> 0x{image_base+aoe:08X}")
print(f"NumberOfRvaAndSizes={ndd}")

DD = ["EXPORT","IMPORT","RESOURCE","EXCEPTION","SECURITY","BASERELOC","DEBUG",
      "ARCH","GLOBALPTR","TLS","LOAD_CONFIG","BOUND_IMPORT","IAT","DELAY_IMPORT","CLR","RES"]
print("--- data directories ---")
tls_rva = tls_sz = 0
for i in range(min(ndd, len(DD))):
    rva = u32(dd_off + i*8); sz = u32(dd_off + i*8 + 4)
    if rva or sz:
        print(f"  [{i:2}] {DD[i]:12} rva=0x{rva:08X} size=0x{sz:X}")
    if DD[i] == "TLS": tls_rva, tls_sz = rva, sz

# sections
sec_off = opt_off + u16(e_lfanew + 20)   # SizeOfOptionalHeader
print("--- sections ---")
secs = []
for i in range(nsec):
    o = sec_off + i*40
    name = img[o:o+8].rstrip(b"\x00").decode("latin1")
    vsz = u32(o+8); vaddr = u32(o+12); rsz = u32(o+16); raddr = u32(o+20)
    secs.append((name, vaddr, vsz, raddr, rsz))
    print(f"  {name:8} VA=0x{base+vaddr:08X} vsz=0x{vsz:06X}  raw@0x{raddr:06X} rsz=0x{rsz:06X}")

# TLS directory (PE32): StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex,
# AddressOfCallBacks, SizeOfZeroFill, Characteristics  (all VA, 32-bit)
if tls_rva:
    t = tls_rva
    start_va = u32(t); end_va = u32(t+4); idx_va = u32(t+8); cb_va = u32(t+12)
    zfill = u32(t+16); chars = u32(t+20)
    print(f"--- TLS @rva 0x{tls_rva:X} ---")
    print(f"  raw data   : 0x{start_va:08X}..0x{end_va:08X}  zerofill=0x{zfill:X}")
    print(f"  index VA   : 0x{idx_va:08X}")
    print(f"  callbacks  : VA 0x{cb_va:08X}")
    if cb_va:
        co = cb_va - base
        print("  callback list:")
        for k in range(16):
            fn = u32(co + k*4)
            if fn == 0: break
            print(f"    [{k}] 0x{fn:08X}")
    else:
        print("  (no callback array)")
else:
    print("--- no TLS directory ---")
