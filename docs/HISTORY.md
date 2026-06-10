# Repository history — a reader's map

This repository keeps its FULL working history (~520 commits), research dead
ends included. It is a working journal, not a curated changelog — you are not
meant to read it linearly. Use this map instead.

## Where the actual result lives

- **`patches/0001–0019` + `patches/README.md`** — the distilled outcome of the
  whole project: every fix against upstream
  [rexglue-sdk](https://github.com/rexglue/rexglue-sdk) `e8ce24f`, granular,
  individually explained, byte-verified to reproduce the shipping tree.
- The port skeleton (`src/`, `launcher/`, `config/`, CMake) and the harness
  (`tools/gamectl.sh`, `tools/perf/`).
- Current state and the v2 plan: `STATUS.md`, `docs/ROADMAP-V2.md`.

## Eras (marked with `era/*` tags)

1. **Windows bring-up → Linux/Vulkan port** — SEH unwinding, threading,
   audio/input, boot present/fence deadlocks (patches 0001–0011).
2. **The performance campaign** — 24 levers tried and closed (PGO, BOLT, ICF,
   code models, spin removal, …). Closed investigations are preserved in
   `docs/archive/FLOOR-*`; the keepers became patches 0012–0016.
3. **`[variant-a]` commits (~370, tag `era/variant-a-frozen`)** — a parallel
   from-scratch research track (own XenonRecomp-based runtime + renderer).
   Frozen as a research asset (`varianta/README.md`); NOT part of the shipping
   build. Kept for provenance; its working logs are mixed Russian/English.
4. **Root-cause + v1.0 (2026-06, tag `v1.0`)** — the two-layer in-match lag
   diagnosis (host CPU-frequency trap + serial translate capacity), patches
   0017–0019, maintainer-verified playability.

## What is deliberately NOT here

`private/` (game packages, savegames) and `generated/` (recompiled sources)
have never been committed — bring your own legally-obtained copy of the game.
Host-specific scripts under `tools/perf/` reference the original development
bench paths; they are measurement provenance, not portable tooling.
