# config/ — recompiler configuration (Phase 2)

Holds the per-title codegen configuration consumed by rexglue (and the
equivalent XenonRecomp TOML if the fallback path is used).

After `rexglue init --app_name south_park_td --app_root ..`, the generated
`south_park_td_config.toml` is tuned here:

- `file_path` → `../private/default.xex` (+ patch path if a Title Update exists)
- **switch tables** — from `XenonAnalyse default.xex tables.toml`, reconciled
  with rexglue's scanners; hand-author entries the analyzer misses.
- **register save/restore** addresses (`__savegprlr_14`, `__restvmx_64`, …)
- `longjmp` / `setjmp` addresses (if the title uses them)
- explicit `functions = [{ address, size }]` where boundary analysis fails
- `invalid_instructions` — skip exception-handling data / padding between funcs

Iterate with `--force` to converge past unresolved calls. Keep this file under
version control (it is *configuration*, not extracted game data).

See `knowledge-base/10-toolchain.md` for the documented knobs and the rexglue
workflow, and `../../PLAN.md` Phase 2.
