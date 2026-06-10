# STATUS — single source of truth

> Rewrite this file in place; do NOT let it grow. History lives in git; closed
> investigations live in `docs/archive/` and `varianta/archive/`.
> Last update: 2026-06-10 — **v1.0 SHIPPED, project parked.**

## State: v1.0 (maintainer decision 2026-06-10)

*South Park: Let's Go Tower Defense Play!* runs natively on this Linux host:
boot → menu → match → win → save, audio + input, mostly-60 fps gameplay.
The maintainer play-verified it (levels 1–2, gamepad), accepted the residual
peak slow-mo, and declared **v1.0 — stop here**. No active work. Future work
(v2) is fully specified in `docs/ROADMAP-V2.md` — read it before ANY perf
session; do not re-derive.

- Tags: `v1.0` + `checkpoint-2026-06-10-playable` (this repo), `v1.0` (super
  repo). SDK rollback anchor: branch `checkpoint/2026-06-10-playable`
  (`c38a35e`) in rexglue-sdk = upstream `e8ce24f` + patch series 0001–0019
  (byte-verified == the shipped tree, `9e8c411`).
- **Publication prep 2026-06-10:** one-time filter-repo rewrite — host sudo
  password scrubbed from ALL history (it HAD been pushed to GitHub in the old
  origin/main ⇒ rotate the host password), ~369 frozen-track commits prefixed
  `[variant-a]`; LICENSE (MIT) + `docs/HISTORY.md` + `era/*` tags added.
  Pre-rewrite mirror backup: `/home/h/src/recomp/backups/`. Published
  (force-push, maintainer GO) 2026-06-10 to
  github.com/superheher/south-park-tower-defence-recomp.
- **FROZEN: variant A** (`varianta/`) — research asset, do NOT iterate.
- **No Telegram.** Never message the maintainer via Telegram tools. Blocked →
  record under BLOCKED, finish cleanly, stop. No busy-work.

## Ground truth (root-caused; play-verified 2026-06-10; details: ROADMAP-V2)

- The game is a **60 fps title** (paces to the 60 Hz vblank; sim ticks
  1/frame). It is NOT "a 30 fps game": v1.0 runs 60 the vast majority of play.
- Layer A (CPU-frequency trap = the old first-seconds throttling): **FIXED** —
  EPP=performance persisted (systemd `cpu-epp-performance.service`) +
  `--freq_keeper` in gamectl. Cold-entry 60.0 @2.9 GHz.
- Layer B (chain capacity): **residual, accepted for v1.0** — level-2+ peak
  waves (1900–2250 draws/swap; translate ≈8.4 µs/draw > 16.7 ms budget)
  re-grid to exact 30.0 = 2× slow-mo for tens of seconds; sound/input stay
  normal (own threads). Playtest session: avg 54.2, max 60.0, min 30.0.
- Perception (maintainer asked "is it sped up?"): NO — pre-fix the field
  start ran 2.3–2.5× slow-mo and was learned as "normal"; native pace reads
  as fast. XE_SWAP vsync limiter + 1 tick/frame cap the game at ≤1× native.
- NOT levers: vsync=false (sim runs at render speed = fast-forward); the 24
  closed lever classes (`docs/archive/FLOOR-*`). Do not retry.

## Operational knowledge (still true while parked)

- Run/measure: `/home/h/src/recomp/gamectl.sh play | bench [sec] | boot |
  shot <name> | kill` (symlink to `tools/gamectl.sh` — the only copy).
- Measurement protocol archived: `tools/perf/protocol/` (measure45 natural,
  measure7 cold-soak + README). A/B keep-bar: `tools/perf/ab_both.sh`;
  determinism: `tools/perf/detdiff/`.
- Boot: intermittent first-present deadlock; gamectl bounds + retries it.
- `/dev/shm/xenia_memory_*` leaks 4.5G per launch; gamectl `kill_all`
  reclaims; after manual launches clean by hand or a later boot SIGBUSes.
- Do NOT run GUI sessions when the maintainer may be at the machine;
  coordinate or run overnight.
- Windows port: NOT attempted; concrete checklist (2 mechanical code gaps, 2
  never-compiled win files, what carries over as-is) in ROADMAP-V2 §Windows.

## Next steps

(none active — v1.0 parked. Resuming = v2: open `docs/ROADMAP-V2.md`, start
with its Step 0 — size level-3+ peaks — then pick lever A or B by data.)

## BLOCKED

(nothing)

## Rules for working sessions

- Decision authority: maintainer sets goals/scope; technical forks are the
  agent's call — decide by data, record here, keep going. Stop only on a
  verified result or a hard external blocker.
- This file is the entry point; keep it ≤ ~120 lines; rewrite in place.
- `docs/NEXT-SESSION-PROMPT.md` ≤ 3 KB, OVERWRITTEN each session.
- Per-iteration journaling = git commit messages (+ ≤10-line update here).
  Detailed write-ups only for CLOSED investigations → `docs/archive/`.
- Bound every GUI wait (`timeout` + bail); never idle-wait a flaky boot.
- Verify by running; screenshots to /tmp/sp; never claim success unverified.
- Commit per logical step (author "Claude Code"); patches stay a granular
  byte-verified series (`patches/README.md`); bump the super-repo gitlink.
