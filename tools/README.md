# tools/ — extraction & recon

Read-only, reproducible helpers used in Phase 1. They never modify the dump and
never emit game content into the repo (outputs go to the git-ignored `private/`).

| Tool | Purpose |
|------|---------|
| `stfs_extract.py` | Extract files from an STFS (CON/LIVE/PIRS) package — XBLA games, DLC, title updates. `--list`, `--only NAME`, or extract all with `-o DIR`. Validated block→offset math (hash-block skipping); follows the contiguous-block fast path. |
| `stfs_recon.py` | Dump STFS header + volume descriptor + first file-table entries (calibration / sanity check). |
| `xex_recon.py` | Parse the plaintext XEX2 header: module flags, base/entry/image size, compression/encryption type, and import libraries + per-library import-record counts. |
| `xex_decrypt.py` | Independent XEX2 **decrypt + basic-decompress** (retail AES key → session key → CBC → basic blocks), then dump/decode 32-bit words at an address. Cross-checks the recompiler's decode and resolves entry-point questions. Needs `pip install pycryptodome`. Usage: `python tools/xex_decrypt.py private/default.xex 0x824499A0`. |
| `scan_recomp.py` / `fix_recomp_labels.py` | Scan / post-codegen fixup of generated C++ (see Phase 2/3 notes). |
| `cdb_*.txt` | cdb command scripts for crash/throw triage (`-cf`). |
| `find_init_arrays.py` | Scan the decrypted image for null-bracketed code-pointer arrays (locates C++ static-initializer `.CRT$XC*` tables / vtables for startup RE). Needs pycryptodome. |
| `find_initterm.py` | Find `_initterm`/`mainCRTStartup` candidates (small indirect-call-loop funcs + their root callers) for startup RE. |
| `find_maincrt.py` | Find mainCRTStartup candidates (root funcs with CRT-startup signature). Note: indirect/vtable calls make 11k+ functions appear as roots, so static heuristics can't pin it — needs dynamic/decompiler analysis. |
| `find_xrefs.py` | Find code references to a guest address via `lis`+`addi/ori` formation (xref analysis for startup RE; note: misses `lis`+load/store-offset patterns). Needs pycryptodome. |
| `pe_inspect.py` | Parse the **decrypted** PE image: machine type, sections, data directories, and TLS directory + callback list. Confirms e.g. "no TLS callbacks". Usage: `python tools/pe_inspect.py private/default_dec.bin`. |
| `pdata.py` | Parse the `.pdata`/EXCEPTION table = **authoritative function-start table** (big-endian; auto-locates the table). `count` / `list N` / `find <addr>` (is an address a function start? which function contains it?). |
| `ppc_dis.py` | Disassemble a guest address range from the decrypted image (capstone, **big-endian PPC**). `python tools/ppc_dis.py private/default_dec.bin 0x824499A0 0x82449B58`. Needs `pip install capstone`. |
| `callgraph.py` | Build a **direct** `bl` call graph over the `.pdata` function list; `roots` / `initterm` / `callers`/`callees`. Caveat: C++ titles are indirect-call-dominated, so this alone can't pin `main` (documented in KB `45`). |
| `find_initarray.py` | Scan `.rdata` for runs of big-endian `.text` pointers (C++ init arrays **and** vtables). |
| `find_crt.py` | **Boundary-independent** global `bl` caller map (scans every aligned word — not limited by function boundaries) + `_initterm`-shape matcher. `callers <addr>` is reliable; `initterm` is noisy (the pattern is generic). |
| `ghidra_pre_funcs.py` / `ghidra_find_main.py` / `ghidra_dump_roots.py` | Ghidra **headless** Jython scripts: define functions from `.pdata`; query the resolved call graph for entry/roots; decompile top roots to spot the CRT entry. Run via `analyzeHeadless ... -loader BinaryLoader -loader-baseAddr 0x82000000 -processor PowerPC:BE:64:default -postScript <script>`. |
| `find_crt_initarray.py` | Find the function forming the C/C++ init-array bounds (`_initterm(&__xc_a,&__xc_z)`) via `lis`+`addi`/`ori` into `.rdata` bounding a `.text`-pointer run. (Caveat: in vtable-heavy titles this matches C++ constructors; see KB `45`.) |
| `find_security_cookie.py` | Find `__security_cookie` (the `.data` global with ~1 writer + many readers) → `__security_init_cookie` → `mainCRTStartup`. (Caveat: matches game *singletons* in singleton-heavy titles; see KB `45`.) |

> Note: the CRT-entry finders (`find_crt*`, `find_security_cookie`, the Ghidra root
> dump) are sound *techniques* but were **defeated** on South Park by its extreme
> C++/vtable/singleton/VMX128 density — `mainCRTStartup` could not be pinned
> headlessly. Documented as a reusable lesson in `knowledge-base/general/45`.

To produce the decrypted image these consume: `python tools/xex_decrypt.py private/default.xex 0x82449968 --save private/default_dec.bin`.

Examples (PowerShell):

```powershell
$pkg = "<dump>\58410931\000D0000\<hash>"
python tools\stfs_extract.py "$pkg" --list                       # inventory
python tools\stfs_extract.py "$pkg" --only default.xex -o private\default.xex
python tools\stfs_extract.py "$pkg" -o private\extracted          # full asset tree
python tools\xex_recon.py private\default.xex                     # XEX recon
```

Phase 1 outputs (this title): `private/default.xex` (8,499,200 B, `XEX2`) and the
full asset tree under `private/extracted/` (1,555 files, 872.7 MiB). The two
`00000002` packages are DLC markers, not a Title Update — see the case study
`knowledge-base/titles/south-park-lgtdp/10-dump-analysis.md`. The algorithms are
generalized in `knowledge-base/general/25-containers-and-extraction.md` and
`general/20-xex-format.md`.
