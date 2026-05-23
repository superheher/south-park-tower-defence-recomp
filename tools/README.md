# tools/ — extraction & asset prep (Phase 1)

Reproducible tooling to turn the **dump** into recompiler inputs. Outputs go to
`../private/` and the asset content root (both git-ignored).

Deliverables of this stage:

1. `private/default.xex` — extracted from the main LIVE/STFS package
   `58410931/000D0000/...`. Verify the first 4 bytes are `XEX2`.
2. The **asset tree** the loader reads (for the runtime VFS to mount later).
3. Classification of the two `00000002` marketplace packages — is either a
   **Title Update** (`*.xexp`)? Keep it if so (feeds the recompiler patch input).

STFS extraction notes (for a purpose-written reader):

- Magic `LIVE` at offset 0; data region starts at `0xC000`; block size `0x1000`.
- Hash blocks interleave the data: a level-0 hash block precedes every `0xAA`
  (170) data blocks (read-only vs read-write affects backup-hash spacing).
- The volume descriptor gives the file-table block; file-table entries are
  `0x40` bytes (name, flags, block count, starting block, size).

A third-party extractor (wxPirs / Velocity / a QuickBMS STFS script) is an
acceptable bootstrap, but a small committed Python/CLI extractor here is
preferred for reproducibility. Record XEX recon results in
`../../knowledge-base/20-dump-analysis.md`.
