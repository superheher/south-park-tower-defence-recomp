#!/usr/bin/env python3
"""Find the CRT init-pointer arrays (__xi_a/__xc_a ... function-pointer tables).

In .rdata, the C/C++ static-init tables are contiguous runs of big-endian
pointers into .text ([0x82100000, code_end)). _initterm walks them; mainCRTStartup
passes their bounds. Print candidate runs (start VA, count) so we can xref the
bounds to locate mainCRTStartup.

Usage: find_initarray.py <image.bin> [min_run=4]
Read-only. base 0x82000000; .rdata = [0x82000600, 0x820E8A9C); .text ends ~0x825F0448.
"""
import struct, sys

img = open(sys.argv[1], "rb").read()
min_run = int(sys.argv[2]) if len(sys.argv) > 2 else 4
base = 0x82000000
RDATA_LO, RDATA_HI = 0x600, 0xE8A9C          # rva range of .rdata
TEXT_LO, TEXT_HI   = 0x82100000, 0x825F1000  # valid .text function VAs

def b32(o): return struct.unpack_from(">I", img, o)[0]

runs = []
o = RDATA_LO
while o < RDATA_HI - 4:
    v = b32(o)
    if TEXT_LO <= v < TEXT_HI and (v & 3) == 0:
        start = o; cnt = 0
        while o < RDATA_HI - 4:
            w = b32(o)
            if (TEXT_LO <= w < TEXT_HI and (w & 3) == 0) or w == 0:
                cnt += 1; o += 4
            else:
                break
        # trim trailing zeros from count for reporting
        if cnt >= min_run:
            runs.append((base + start, cnt, [b32(start + k*4) for k in range(min(cnt, 8))]))
    else:
        o += 4

runs.sort(key=lambda r: -r[1])
print(f"found {len(runs)} candidate pointer runs (>= {min_run}); top by length:")
for va, cnt, sample in runs[:25]:
    s = " ".join(f"{x:08X}" for x in sample)
    print(f"  @0x{va:08X}  count={cnt:<4}  first: {s}")
