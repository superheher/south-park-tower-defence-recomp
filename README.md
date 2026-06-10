# south-park-recomp

The native PC port of **South Park: Let's Go Tower Defense Play!** (Xbox 360 /
XBLA, Title ID `58410931`) produced by **static recompilation** of the title's
PowerPC executable, running on the [`rexglue-sdk`](https://github.com/rexglue/rexglue-sdk)
runtime (with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) /
[XenosRecomp](https://github.com/hedge-dev/XenosRecomp) as references/fallback).

**Status: v1.0 (2026-06-10)** — fully playable natively on Linux/Vulkan (boot →
menus → matches → saves, audio + input), mostly-60 fps gameplay; the heaviest
wave peaks still dip (the v2 plan: `docs/ROADMAP-V2.md`). The distilled result
is the granular fix series against upstream rexglue-sdk in
[`patches/`](patches/README.md); `STATUS.md` is the current state;
`docs/HISTORY.md` is the map of this repository's full working history.

> **Bring your own dump.** No copyrighted game code or assets are stored in this
> repository. Everything derived from the game (the extracted `default.xex`,
> recompiled C++, shader caches, assets) is git-ignored and produced locally.

## Layout

```
config/    rexglue codegen config (TOML), jump tables, function overrides
tools/     extraction & asset-prep scripts (STFS -> default.xex)
src/        title-specific host app, kernel/import shims, mid-asm hooks, patches
docs/       build & run instructions, RE notes specific to this title
```

## Quick build (once the XEX is extracted — see `docs/`)

```pwsh
# from rexglue-sdk, with Clang 20+, CMake 3.25+, Ninja on PATH
rexglue init --app_name south_park_td --app_root .
# edit config/south_park_td_config.toml -> point file_path at extracted default.xex
cmake --preset win-amd64-debug
cmake --build --preset win-amd64-debug --target south_park_td_codegen
cmake --build --preset win-amd64-debug
```

For the **Linux build and run** (the v1.0 target) see `docs/RUN-linux.md`;
apply the SDK patch series first (`patches/README.md`). Note the Windows-port
caveats in `docs/ROADMAP-V2.md` §"Windows port checklist".

## Repository history

This repository intentionally keeps its full working history (~520 commits),
research dead ends included — notably the frozen from-scratch track
(`[variant-a]`-prefixed commits, ~370 of them, between the `era/variant-a-start`
and `era/variant-a-frozen` tags). `docs/HISTORY.md` is the reader's map; the
curated change story is the patch series itself.

## License

MIT — see `LICENSE`. It covers this repository's code and documentation only;
no game code or assets are stored here.
