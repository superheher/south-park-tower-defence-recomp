#!/usr/bin/env python3
"""Disassemble a guest address range from the decrypted PE image (big-endian PPC).

Usage: ppc_dis.py <image.bin> <start_hex> <end_hex> [load_base_hex=0x82000000]
Read-only. Uses capstone PPC. The decrypted XEX image is the *loaded* image, so
RVA == file offset; guest_addr - load_base = file offset.
"""
import sys
from capstone import Cs, CS_ARCH_PPC, CS_MODE_BIG_ENDIAN, CS_MODE_32

img   = open(sys.argv[1], "rb").read()
start = int(sys.argv[2], 16)
end   = int(sys.argv[3], 16)
base  = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x82000000

md = Cs(CS_ARCH_PPC, CS_MODE_BIG_ENDIAN + CS_MODE_32)
md.detail = False

off = start - base
code = img[off: end - base]
for ins in md.disasm(code, start):
    print(f"  0x{ins.address:08X}: {ins.bytes.hex().upper():8}  {ins.mnemonic:8} {ins.op_str}")
