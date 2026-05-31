# Next session: finish the audio optimization (kill the 3× get-callback over-pull)

/ goal: cut the audio-chain CPU by making the SDL playback path run at 1× realtime instead of 3×,
WITHOUT regressing the (already-correct) audible output. Ship it A/B-gated, or reject with evidence.

## Read first
- `docs/AUDIO-INVESTIGATION-REPORT.md` — the full prior audit (this is the source of truth).
- memory `sp_audio_correct_3x_artifact`, and `sp_floor_latency_bound` (the patch-0015 despin).
- Do NOT re-litigate whether audio is "broken." It is **not**. Proven: the real `Speakers.monitor`
  output is **1× realtime, normal pitch (centroid 2069≈2073 Hz), 99.6 % active, clean, peak 0.59**.
  The dump's "3× / 70 % silence" and the "silent monitor" were measurement artifacts (over-pull /
  game's quiet ch0-ch1-only 6-ch frames / a MUTED bench sink). Don't chase those again.

## The one real lever (this task)
The SDL get-callback (`SDLAudioDriver::SDLCallback`, `third_party/rexglue-sdk/src/audio/sdl/sdl_audio_driver.cpp`)
fires a steady **~140 calls/s = 3× realtime** in the game process (each requests 4 frames / 1024 samp,
99.9 % real, `SDL_GetAudioStreamQueued` constant). PipeWire/device consume at **1×** (graph 48000-locked,
`pw-top` node 1024/48000), so SDL discards the 2× excess. But because the callback releases the client
semaphore per real frame consumed, the 3× drives **Audio Worker → guest XAudio client callback → XMA
(FFmpeg) decode → 6-ch conversion ~3× harder than realtime = wasted CPU**. Goal: make that chain run 1×.

A faithful **standalone probe** (`tools/audio/sdl_pull_probe.c`, same bundled libSDL3.a 3.5.0, identical
open params + the 3 hints + x11 + game-dir env) pulls at **1×** — it does NOT reproduce the 3×. So the
trigger is something in the **full game process**, not the device/graph/hints/env. Root cause NOT isolated.

## Plan
**Step A — try to isolate the SDL root cause first (a 1-line config fix beats a throttle).** Cheap experiments:
1. Make the probe reproduce it: add `SDL_INIT_VIDEO` (the game inits video+audio; the probe only audio),
   spawn some busy threads, and/or open the stream the exact same order the game does. If the probe then
   pulls 3×, bisect which difference triggers it.
2. Add a per-call wall-clock timestamp log to the callback (the prior `audio_diag` cvar; recipe in the
   report §4) to confirm the 140/s is evenly spaced (it was) vs bursty — evenly-spaced ⇒ SDL believes
   the device needs 3×, so look at `SDL_GetAudioDeviceFormat` obtained `sample_frames` for the GAME
   (add a one-line log in `Initialize`; the probe got 1024 — check the game gets the same).
3. Check SDL3 3.5.0's `SDL_audiocvt.c:1393` get-callback dispatch / `future_buffer` sizing; try a
   different device sample-frames via `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES`, or a newer SDL3.
If a clean root-cause fix falls out, prefer it.

**Step B — robust fallback if the root stays elusive: rate-limit production to the device drain rate.**
The device genuinely needs 1×; cap real-frame consumption + semaphore release to that, so the worker/
guest/XMA run 1×, while keeping the SDL stream adequately buffered so playback never starves. Two designs:
- **High-water gate:** in the real-frame branch, only pop from `frames_queued_` + `semaphore_->Release`
  when `SDL_GetAudioStreamQueued(stream) < target` (target ≈ 2-3 device buffers). On over-pull calls when
  the stream is already full, return early / feed nothing. Risk: if SDL needs data and you feed none →
  underrun. Tune `target` and verify no gaps.
- **Wall-clock credit accumulator:** release the semaphore at exactly `frames_per_sec` using elapsed time,
  independent of how often SDL calls. Over-pull calls drain the already-queued frames; the queue holds ~1×.
Either way the guest is driven at 1×. Keep the change `.so`-only, surgical, well-commented (upstream-Xenia code).

## Verification protocol (must pass ALL before keeping)
1. **Output unchanged (no regression):** record `Speakers.monitor` ~20 s in a level (sink UNMUTED — it
   re-mutes on suspend, run `pactl set-sink-mute <Speakers> 0; pactl set-sink-volume <Speakers> 100%`),
   convert + run `tools/audio/analyze_dump.py <wav→f32> 2`. Require: ~1× realtime, **≥99 % active**,
   centroid ≈ 2.0-2.4 kHz (normal pitch), **0 clicks >0.3**, no new interior gaps. Compare to the report's
   baseline numbers.
2. **CPU actually dropped:** measure the audio threads' CPU before/after (XMA Decoder + Audio Worker +
   the guest-sim cost of the client callback) — e.g. `top -H`/`perf` on the `south_park_td` threads in a
   level. The callback rate should fall from ~140/s toward ~47/s, and frames produced/sec should ≈ 1×.
3. **detdiff gate + render:** `gamectl.sh`-based gate pass + a mid-combat screenshot (render must be
   pixel-correct; audio timing has no path to GPU state, so this should be trivially invariant).

## Build / run mechanics (verified this session)
- Edit `third_party/rexglue-sdk/src/audio/sdl/sdl_audio_driver.cpp` (and/or `.../src/audio/audio_system.cpp`).
- Build: `cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --config Release --target install`
  → emits `third_party/rexglue-sdk/out/install/linux-amd64/lib64/librexruntime.so` (~5 min partial after
  a 1-file edit). Then `cp` that `.so` → `south-park-recomp/out/build/linux-amd64-release/librexruntime.so`
  (the app build dir links the PREBUILT SDK `.so`; editing src without this rebuild+cp does nothing).
- Kept production binary: `.so` **1a3f6076** + exe **848f191c** (mcmodel=medium). Back it up before
  overwriting. SDK tree is currently **canonical** (pristine e8ce24f + `git apply patches/0001..0016`).
- Run: `REX_EXTRA_ARGS="--audio_diag=true --audio_dump=/tmp/sp/a.f32" REX_LOG_LEVEL=info ./gamectl.sh play`
  (the `REX_EXTRA_ARGS`/`REX_LOG_LEVEL` passthrough was added to `gamectl.sh`). `play` nav is flaky
  (intermittent); retry. game = `south_park_td`.
- Probe: `cc -O2 -Ithird_party/rexglue-sdk/out/install/linux-amd64/include tools/audio/sdl_pull_probe.c
  third_party/rexglue-sdk/out/install/linux-amd64/lib64/libSDL3.a -lm -ldl -lpthread -lrt -o /tmp/sp/probe`.

## Gotchas
- `pkill`/`gamectl.sh kill` exits non-zero → **run it ALONE** (it cancels sibling tool-calls in the same batch).
- Each launch leaks a `/dev/shm/xenia_memory_*` (~?GB) — `rm -f /dev/shm/xenia_memory_*` between runs; a full
  /dev/shm → SIGBUS on boot.
- The bench `Speakers` sink re-mutes on suspend — unmute before every monitor recording.
- `audio_diag` and the per-call timing logs are diagnostic-only — gate them on a cvar and DON'T ship them
  in the kept `.so` (revert to canonical for the final build, like last session did).
- If the throttle can't hit 1× without underruns after honest tuning, **reject it** and record the
  evidence — a 3× CPU waste that's harmless to sound is acceptable to leave if no safe fix exists.
