#!/usr/bin/env python3
"""Reproducible, read-only STFS (CON/LIVE/PIRS) extractor.

Extracts files from an Xbox 360 STFS package (XBLA games, DLC, title updates,
saves). Read-only: it never writes back to the package. Intended for personal
preservation / interoperability — bring your own dump.

Block math (validated against this title's package):
  data region begins at file offset 0xC000, block size 0x1000 (4 KiB).
  Logical data blocks are interspersed with hash-table blocks:
    - one L0 hash table per 0xAA   (170)      data blocks,
    - one L1 hash table per 0x70E4 (170^2)     data blocks,
    - one L2 hash table per 0x4AF768 (170^3)   data blocks.
  Each table is duplicated (primary+backup) when the package is read-write;
  the multiplier `f` is 1 for read-only packages (volume blockSeparation bit0=1)
  and 2 otherwise.
    backing(b) = b + f*(b//0xAA + b//0x70E4 + b//0x4AF768)
    offset(b)  = 0xC000 + backing(b)*0x1000
Anchors that confirm the math for this package: file table block 0 -> 0xC000;
default.xex start block 31 -> 0x2B000 (== 'XEX2').

Header field offsets (XContent metadata, big-endian unless noted):
  0x000 magic (CON /LIVE/PIRS)         0x344 contentType (u32)
  0x360 titleID (u32)                  0x379 StfsVolumeDescriptor:
    +0x00 size(=0x24)  +0x02 blockSeparation  +0x03 fileTableBlockCount(u16 LE)
    +0x05 fileTableBlockNumber(u24 LE)

File table entry (0x40 bytes):
  0x00 name[0x28]   0x28 flags (bit7=dir, bit6=contiguous, bits0-5=name len)
  0x29 allocBlocks(u24 LE)  0x2F startBlock(u24 LE)  0x32 parentIdx(u16 BE)
  0x34 fileSize(u32 BE)
"""
import os, struct, sys, argparse

BLOCK = 0x1000
DATA_BASE = 0xC000

def u32be(b, o): return struct.unpack_from(">I", b, o)[0]
def u16be(b, o): return struct.unpack_from(">H", b, o)[0]
def u16le(b, o): return struct.unpack_from("<H", b, o)[0]
def u24le(b, o): return b[o] | (b[o+1] << 8) | (b[o+2] << 16)


class Stfs:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        head = self._read_at(0, 0x1000)
        self.magic = head[0:4]
        if self.magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError(f"not an STFS package (magic={self.magic!r})")
        self.content_type = u32be(head, 0x344)
        self.title_id = u32be(head, 0x360)
        vd = 0x379
        self.vd_size = head[vd]
        self.block_sep = head[vd + 2]
        self.ft_block_count = u16le(head, vd + 3)
        self.ft_block_num = u24le(head, vd + 5)
        # read-only (bit0 set) -> 1 hash copy; read-write -> 2 (primary+backup)
        self.factor = 1 if (self.block_sep & 1) else 2
        self.entries = None

    def _read_at(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def backing(self, b):
        f = self.factor
        return b + f * (b // 0xAA + b // 0x70E4 + b // 0x4AF768)

    def offset(self, b):
        return DATA_BASE + self.backing(b) * BLOCK

    def read_block(self, b):
        return self._read_at(self.offset(b), BLOCK)

    def read_contiguous(self, start, nblocks):
        out = bytearray()
        for i in range(nblocks):
            out += self.read_block(start + i)
        return out

    def parse_file_table(self):
        # The file table is itself stored contiguously starting at ft_block_num.
        raw = self.read_contiguous(self.ft_block_num, self.ft_block_count)
        entries = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            if len(e) < 0x40:
                break
            flags = e[0x28]
            nlen = flags & 0x3F
            if nlen == 0:
                entries.append(None)  # keep index alignment for parent lookup
                continue
            name = e[:nlen].decode("ascii", "replace")
            entries.append({
                "idx": len(entries),
                "name": name,
                "is_dir": bool(flags & 0x80),
                "contiguous": bool(flags & 0x40),
                "alloc_blocks": u24le(e, 0x29),
                "start_block": u24le(e, 0x2F),
                "parent": u16be(e, 0x32),
                "size": u32be(e, 0x34),
            })
        self.entries = entries
        return entries

    def path_of(self, ent):
        parts = [ent["name"]]
        p = ent["parent"]
        seen = set()
        while p != 0xFFFF and p < len(self.entries) and p not in seen:
            seen.add(p)
            par = self.entries[p]
            if not par:
                break
            parts.append(par["name"])
            p = par["parent"]
        return "/".join(reversed(parts))

    def extract_file(self, ent, dest):
        nblocks = (ent["size"] + BLOCK - 1) // BLOCK
        if ent["contiguous"]:
            data = self.read_contiguous(ent["start_block"], nblocks)
        else:
            data = self._follow_chain(ent["start_block"], nblocks)
        data = data[:ent["size"]]
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as o:
            o.write(data)
        return len(data)

    # --- fallback for fragmented files: follow next-block links in L0 hash tables ---
    def _hash_l0_offset(self, block):
        # L0 table for the group containing `block`. It directly precedes the
        # group's data in backing space. backing index of the table:
        group = block // 0xAA
        # backing position of the first data block of the group, minus 1 table
        first = group * 0xAA
        return DATA_BASE + (self.backing(first) - 1) * BLOCK if group else None

    def _next_block(self, block):
        # hash entry is 0x18 bytes; status@0x14, nextBlock u24 BE @0x15
        # NOTE: only used for non-contiguous files (none in this title).
        raise NotImplementedError(
            "fragmented file encountered; chain-following not validated for this package")

    def _follow_chain(self, start, nblocks):
        out = bytearray()
        b = start
        for _ in range(nblocks):
            out += self.read_block(b)
            b = self._next_block(b)
        return out


def main():
    ap = argparse.ArgumentParser(description="Read-only STFS extractor")
    ap.add_argument("package")
    ap.add_argument("-o", "--out", help="output dir (extract all files)")
    ap.add_argument("--only", help="extract only this file name (e.g. default.xex)")
    ap.add_argument("--list", action="store_true", help="list files only")
    args = ap.parse_args()

    s = Stfs(args.package)
    print(f"package      : {os.path.basename(args.package)}")
    print(f"magic        : {s.magic!r}")
    print(f"contentType  : 0x{s.content_type:08X}")
    print(f"titleID      : 0x{s.title_id:08X}")
    print(f"blockSep     : 0x{s.block_sep:02X}  (hash copies f={s.factor})")
    print(f"fileTable    : block {s.ft_block_num}, {s.ft_block_count} blocks")
    entries = [e for e in s.parse_file_table() if e]
    files = [e for e in entries if not e["is_dir"]]
    dirs = [e for e in entries if e["is_dir"]]
    total = sum(e["size"] for e in files)
    print(f"entries      : {len(files)} files, {len(dirs)} dirs, total {total} bytes")

    if args.list or (not args.out and not args.only):
        for e in sorted(entries, key=lambda x: s.path_of(x).lower()):
            tag = "D" if e["is_dir"] else " "
            cz = "" if e["contiguous"] or e["is_dir"] else " [FRAGMENTED]"
            print(f"  {tag} {s.path_of(e):50} {e['size']:>10}{cz}")
        return

    if args.only:
        target = next((e for e in files if e["name"] == args.only), None)
        if not target:
            print(f"!! {args.only} not found"); sys.exit(2)
        dest = args.out if (args.out and not os.path.isdir(args.out)) else \
            os.path.join(args.out or ".", target["name"])
        n = s.extract_file(target, dest)
        with open(dest, "rb") as fh:
            head = fh.read(4)
        print(f"wrote {dest} ({n} bytes); first 4 bytes = {head!r}")
        return

    # extract all
    nfiles = 0
    for e in files:
        rel = s.path_of(e).replace("/", os.sep)
        dest = os.path.join(args.out, rel)
        s.extract_file(e, dest)
        nfiles += 1
    print(f"extracted {nfiles} files to {args.out}")


if __name__ == "__main__":
    main()
