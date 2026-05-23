# south-park-recomp

The native PC port of **South Park: Let's Go Tower Defense Play!** (Xbox 360 /
XBLA, Title ID `58410931`) produced by **static recompilation** of the title's
PowerPC executable, running on the [`rexglue-sdk`](https://github.com/rexglue/rexglue-sdk)
runtime (with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) /
[XenosRecomp](https://github.com/hedge-dev/XenosRecomp) as references/fallback).

Part of the `sotuh-park-xbla-recomp` super-repository. See `../PLAN.md` for the
roadmap and `../knowledge-base/` for analysis.

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
