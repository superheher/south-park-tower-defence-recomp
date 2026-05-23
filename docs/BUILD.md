# Building (host setup) — Phase 0

Prerequisites on this Windows 11 host:

| Tool | Version | Note |
|------|---------|------|
| Clang | **20+** (clang-cl / VS2022 "C++ Clang tools") | **Required** — recomp output needs Clang. Not yet installed on host. |
| CMake | 3.25+ | present |
| Ninja | any recent | present |
| PowerShell 7 (`pwsh`) | 7.x | for the rexglue **PSReX** lifecycle |
| Python | 3.10+ | extraction/asset tooling |
| Vulkan SDK | optional | only for the Vulkan backend; D3D12 is primary |

Steps:

```pwsh
# from the super-repo root
git submodule update --init --recursive

# build + install the SDK
cd third_party/rexglue-sdk
cmake --preset win-amd64
cmake --build --preset win-amd64 --target install
# optional: Import-Module ./scripts/PSReX; Invoke-ReXSetup
```

Then proceed to Phase 1 (extraction) — see `../tools/README.md` — and Phase 2
(`rexglue init`) — see `../config/README.md`. Full roadmap: `../../PLAN.md`.
A `RUN.md` will be written once the build is runnable (Phase 6).
