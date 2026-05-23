# src/ — title-specific host code (Phases 3–5)

Everything hand-written for this port lives here (the *generated* C++ goes to
`../generated/` and is git-ignored). Expected contents as bring-up proceeds:

- **App entry / glue** — `main.cpp` + `south_park_td_app.*` (from `rexglue init`),
  wiring the rexglue runtime, window, VFS mounts (asset root), and main loop.
- **Kernel / XAM import shims** — implementations for `xboxkrnl.exe` / `xam.xex`
  imports the title actually calls that the runtime doesn't already provide.
  Track the backlog in `knowledge-base/50-imports-backlog.md`.
- **Mid-asm hooks** — host callbacks injected at specific guest addresses
  (declared in the config); implement them here, the linker resolves them.
- **Function overrides** — replace whole recompiled functions with host versions
  (weak-alias trick) for engine-specific behaviour (save, timing, asset I/O).
- **Patches** — targeted fixes for engine quirks found during bring-up.

Conventions: keep shims small and well-named; one logical change per commit;
record every new import/hook/quirk in the knowledge base. See `../../PLAN.md`
Phases 3–5.
