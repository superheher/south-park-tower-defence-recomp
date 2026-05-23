# tools/ — extraction & recon

Read-only, reproducible helpers used in Phase 1. They never modify the dump and
never emit game content into the repo (outputs go to the git-ignored `private/`).

| Tool | Purpose |
|------|---------|
| `stfs_extract.py` | Extract files from an STFS (CON/LIVE/PIRS) package — XBLA games, DLC, title updates. `--list`, `--only NAME`, or extract all with `-o DIR`. Validated block→offset math (hash-block skipping); follows the contiguous-block fast path. |
| `stfs_recon.py` | Dump STFS header + volume descriptor + first file-table entries (calibration / sanity check). |
| `xex_recon.py` | Parse the plaintext XEX2 header: module flags, base/entry/image size, compression/encryption type, and import libraries + per-library import-record counts. |

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
