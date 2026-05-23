#!/usr/bin/env python3
"""Read-only XEX2 recon. Parses the (plaintext) XEX header + optional headers.

The XEX *headers* (security info, optional headers, import libraries) are stored
in the clear; only the basefile (PE image) is compressed/encrypted. So we can
report compression/encryption type, image base, entry point, and the import
library names + per-library import counts WITHOUT decryption. The exact resolved
ordinal list and .pdata/section layout live inside the (de)compressed PE and are
left to the recompiler (rexglue/XenonRecomp) to enumerate.

Refs: Xenia xex2.h / xex_module.cc; Free60 XEX page.
"""
import struct, sys

def u16be(b, o): return struct.unpack_from(">H", b, o)[0]
def u32be(b, o): return struct.unpack_from(">I", b, o)[0]

# Known optional header keys (id<<8 | sizeclass). 0xFF low byte => value is an offset.
OPT_KEYS = {
    0x000002FF: "RESOURCE_INFO",
    0x000003FF: "FILE_FORMAT_INFO",
    0x000005FF: "DELTA_PATCH_DESCRIPTOR",
    0x000080FF: "BOUNDING_PATH",
    0x00010001: "ORIGINAL_BASE_ADDRESS",
    0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE_ADDRESS",
    0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP",
    0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES",
    0x00020104: "TLS_INFO",
    0x00020200: "DEFAULT_STACK_SIZE",
    0x00020301: "DEFAULT_FILESYSTEM_CACHE_SIZE",
    0x00020401: "DEFAULT_HEAP_SIZE",
    0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO",
    0x00040201: "TITLE_WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS",
    0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO",
    0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALTERNATE_TITLE_IDS",
    0x00040801: "ADDITIONAL_TITLE_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}
COMPRESSION = {0: "none", 1: "basic", 2: "normal(LZX)", 3: "delta"}
ENCRYPTION = {0: "none", 1: "normal(AES-128)"}
MODULE_FLAGS = {
    0x01: "TITLE", 0x02: "EXPORTS_TO_TITLE", 0x04: "SYSTEM_DEBUGGER",
    0x08: "DLL_MODULE", 0x10: "MODULE_PATCH", 0x20: "PATCH_FULL",
    0x40: "PATCH_DELTA", 0x80: "USER_MODE",
}

def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"XEX2", f"not XEX2: {data[:4]!r}"
    module_flags = u32be(data, 0x04)
    pe_offset = u32be(data, 0x08)
    sec_offset = u32be(data, 0x10)
    opt_count = u32be(data, 0x14)

    print(f"file size       : {len(data)} (0x{len(data):X})")
    print(f"magic           : XEX2")
    flags = [n for bit, n in MODULE_FLAGS.items() if module_flags & bit]
    print(f"module flags    : 0x{module_flags:08X}  {' | '.join(flags)}")
    print(f"PE data offset  : 0x{pe_offset:X}")
    print(f"security offset : 0x{sec_offset:X}")
    print(f"opt hdr count   : {opt_count}")

    opt = {}
    base = 0x18
    for i in range(opt_count):
        key = u32be(data, base + i * 8)
        val = u32be(data, base + i * 8 + 4)
        opt[key] = val
    print("--- optional headers ---")
    for key, val in opt.items():
        name = OPT_KEYS.get(key, f"UNKNOWN_0x{key:08X}")
        print(f"  {name:30} key=0x{key:08X} val=0x{val:08X}")

    # entry point / image base (inline values)
    if 0x00010100 in opt:
        print(f"entry point     : 0x{opt[0x00010100]:08X}")
    if 0x00010201 in opt:
        print(f"image base      : 0x{opt[0x00010201]:08X}")
    if 0x00010001 in opt:
        print(f"original base   : 0x{opt[0x00010001]:08X}")
    if 0x00020200 in opt:
        print(f"default stack   : 0x{opt[0x00020200]:08X}")
    if 0x00020401 in opt:
        print(f"default heap    : 0x{opt[0x00020401]:08X}")

    # file format info (compression + encryption)
    if 0x000003FF in opt:
        off = opt[0x000003FF]
        info_size = u32be(data, off)
        enc = u16be(data, off + 4)
        comp = u16be(data, off + 6)
        print(f"--- file format info @0x{off:X} (size {info_size}) ---")
        print(f"  encryption    : {enc} ({ENCRYPTION.get(enc, '?')})")
        print(f"  compression   : {comp} ({COMPRESSION.get(comp, '?')})")

    # security info: image base (load addr) + image size
    try:
        sec_size = u32be(data, sec_offset)
        image_size = u32be(data, sec_offset + 4)
        # load address is at sec_offset + 0x108 + 0x38 in many layouts; report a few
        print(f"--- security info @0x{sec_offset:X} ---")
        print(f"  header size   : 0x{sec_size:X}")
        print(f"  image size    : 0x{image_size:X} ({image_size} bytes)")
        load_addr = u32be(data, sec_offset + 0x108 + 0x38) if sec_offset + 0x150 < len(data) else 0
        print(f"  load addr(?)  : 0x{load_addr:08X}")
    except Exception as e:
        print(f"  (security info parse skipped: {e})")

    # import libraries: names + per-library import counts
    if 0x000103FF in opt:
        off = opt[0x000103FF]
        total_size = u32be(data, off)
        string_table_size = u32be(data, off + 4)
        lib_count = u32be(data, off + 8)
        str_base = off + 12
        strings = data[str_base:str_base + string_table_size]
        names = [s.decode("ascii", "replace") for s in strings.split(b"\x00") if s]
        print(f"--- import libraries @0x{off:X} (size {total_size}) ---")
        print(f"  string table  : {string_table_size} bytes, lib_count={lib_count}")
        print(f"  names         : {names}")
        # walk library descriptors after the string table
        p = str_base + string_table_size
        for li in range(lib_count):
            if p + 0x24 > len(data):
                break
            desc_size = u32be(data, p)
            name_index = u16be(data, p + 0x20 + 0x14 - 0x14 + 0x1C)  # placeholder
            # proper fields:
            # 0x00 size; 0x04 next_import_digest[0x14]; 0x18 id; 0x1C version;
            # 0x20 version_min; 0x24 name_index(u16); 0x26 count(u16); 0x28 records[count]
            vid = u32be(data, p + 0x18)
            version = u32be(data, p + 0x1C)
            version_min = u32be(data, p + 0x20)
            name_index = u16be(data, p + 0x24)
            count = u16be(data, p + 0x26)
            nm = names[name_index] if name_index < len(names) else f"#{name_index}"
            print(f"  lib[{li}] {nm:16} import_records={count:4}  ver=0x{version:08X} min=0x{version_min:08X} (desc_size={desc_size})")
            p += desc_size

if __name__ == "__main__":
    main()
