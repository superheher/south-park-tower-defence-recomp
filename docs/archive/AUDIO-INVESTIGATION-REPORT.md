# Audio investigation: the output is correct — the "3× / 70% silence" were measurement artifacts

**Date:** 2026-05-31. **Goal:** "fix and optimize audio properly." **One-line:** Rigorous,
measurement-first audit of the SDL audio path. The **audible output is correct** (realtime speed,
normal pitch, continuous, clean, healthy level) — every "broken" signal turned out to be a
measurement artifact. The one *real* finding is an **optimization** opportunity: the SDL get-callback
over-fires **~3×** in the game process (harmless to sound, but wastes audio-chain CPU). Root cause
not isolated; left as a documented lever, not patched (per wrap-up).

Base: kept `.so` **1a3f6076** (exe `848f191c`). No shipped binary change.

## 0. What looked broken, and why each was a red herring
A first pass with the `--audio_dump` cvar (patch 0007) made the audio look badly broken. Each symptom
was chased to ground and **refuted by measurement**:

| Apparent symptom | Measured reality |
|---|---|
| Dump grows at **3.0× realtime** (561 MB in ~160 s) | The SDL get-callback *fires* 3× (140/s vs 47/s). But PipeWire/device consume at **1×** — the 2× excess is discarded inside SDL. Audible output is realtime. |
| Dump is **70 % exact-zero "silence"** | Those are the game's own **quiet frames**: it mixes into a 6-ch container but only populates **ch0/ch1 (FL/FR)**; ch2-5 are always 0. `audio_diag` shows the callback writes **99.9 % real frames, 0.1 % underrun silence-fill** — not dropouts. |
| Speaker **monitor records pure silence** | The test-bench **`Speakers` sink was MUTED + 0 % volume** (host config, not the game). A control tone played to the same sink also recorded — methodology was sound; the sink was just muted. After unmuting, the game's output records full, continuous audio. |
| `WaitMultiple` trylock-storm = audio bug | That's the upstream-Xenia worker contention already addressed by the **despin (patch 0015)**; unrelated to output correctness. |

## 1. The decisive measurement: the actual speaker output is correct
With the sink unmuted, a **same-window cross-check** — bytes written to the dump (pre-SDL) vs. a
`parecord` of the `Speakers.monitor` (post-PipeWire, the real output):

| Stream | Window | Active | Pitch (spectral centroid) | Level | Glitches |
|---|---|---|---|---|---|
| Dump → SDL | 63.0 s of content in 20 s wall (**3×**) | 33 % (ch0/ch1 only) | 2073 Hz | peak 0.59 | — |
| **Monitor (real output)** | 19.1 s in ~20 s wall (**1×, realtime**) | **99.6 %** | **2069 Hz** | peak 0.59, −22 dBFS | 3 gaps / 59 ms over 19 s; **0 clicks >0.3**, max Δ 0.24 |

- **Speed:** monitor = 1× realtime (19.1 s of audio for a 19 s window). Not sped up.
- **Pitch:** monitor centroid **2069 Hz ≈ dump 2073 Hz** (0.2 %): normal pitch, **not chipmunk, not decimated**.
- **Continuity:** 99.6 % active, 3 tiny gaps (≈ pw-top xruns ERR=6/11), essentially smooth.
- **Decode:** a silence-stripped render of the dump is coherent music/speech (centroid ~2.4 kHz).
  The **XMA→PCM decode is correct.**

**Verdict: audio is not audibly broken.** It plays at the right speed, pitch, and level, continuously.

## 2. The one real finding (optimization, not correctness): 3× get-callback over-pull
`audio_diag` (a temporary cvar, see §4) on a live run, corroborated by log timestamps:

- SDL get-callback fires a **steady 140 calls/s** (Δt = 1.43 s / 200 calls), each requesting 4 frames
  (1024 samples, `additional_amount == total_amount == 24576 B`), **99.9 % real**, `SDL_GetAudioStreamQueued`
  **constant** (no backlog → SDL discards the excess; sink-input latency = 0).
- 140 calls/s × 1024 = **2.99×** the realtime 48000.
- Meanwhile PipeWire is **1×**: graph `clock.rate=48000`, `allowed-rates=[48000]`, `quantum=1024`;
  device (Apple-T2 `Speakers`) live hw_params 48000/4-ch S24; `pw-top` shows the `rexglue` node at
  **QUANT 1024 / RATE 48000** (1×).
- A **standalone probe** (`tools/audio/sdl_pull_probe.c`) — same bundled SDL3 (3.5.0 static), identical
  open params (48000/F32LE/6-ch), the same three SDL hints, run from the game dir under
  `SDL_VIDEODRIVER=x11` — pulls at **0.98× (47 calls/s, 1024 buffer)**. **It does NOT reproduce the 3×.**

⇒ The 3× is **specific to the full game process** (not the device, the PipeWire graph, the SDL hints,
or the env — all controlled for). Because the SDL callback releases the client semaphore per real frame
consumed, the 3× drives the **Audio Worker → guest XAudio client callback → XMA (FFmpeg) decode →
6-ch conversion** ~3× harder than realtime — **wasted CPU** on a perf-floor-bound port — while the
device still plays a correct 1× stream. Root cause inside SDL3's get-callback dispatch
(`thirdparty/sdl3/src/audio/SDL_audiocvt.c:1393`) was **not isolated**; deferred, not patched.

A safe fix would cap real-frame production to the device drain rate (e.g. gate the
`frames_queued_` pop + semaphore `Release` on `SDL_GetAudioStreamQueued < target` so the worker/guest/XMA
run 1×), but it risks underruns and needs its own A/B — out of scope for this wrap-up.

## 3. Incidental fix: SDK working-tree drift repaired
The `third_party/rexglue-sdk` working tree had **drifted** from the canonical patch series: `threading_posix.cpp`
was missing the **0015 audio-worker despin** (`g_wait_notify_cv`) *and* the **0012** `VblankBackoffWait`/
`NotifyVblank` exports, even though the shipped `.so` (1a3f6076) contains them. Rebuilt the tree to
**canonical (pristine `e8ce24f` + `git apply` 0001-0016, all 16 clean)**; the despin/vblank exports are
restored. Kept `.so` unchanged (restored 1a3f6076 to the game dir).

## 4. Artifacts / reusable tooling
- `tools/audio/sdl_pull_probe.c` — standalone SDL3 pull-rate probe (built `cc -Iout/install/.../include
  sdl_pull_probe.c out/install/.../lib64/libSDL3.a -lm -ldl -lpthread -lrt`). Prints obtained device
  format + measured pull ratio. The clean way to test the device/SDL path without the game.
- `tools/audio/analyze_dump.py` — analyzes a raw F32LE dump (or a WAV→f32 monitor capture): level,
  per-channel peaks/RMS, exact-zero frame %, interior-gap (underrun) runs, click rate, spectral centroid.
- `gamectl.sh` — added a `REX_EXTRA_ARGS` / `REX_LOG_LEVEL` passthrough to `launch_detached` so audio
  cvars can be set on a `play` run.
- **`audio_diag` cvar** (temporary; reverted from the tree to keep it canonical). Recoverable diff —
  add to `src/audio/sdl/sdl_audio_driver.cpp`:
  ```
  REXCVAR_DEFINE_BOOL(audio_diag, false, "Audio", "Log SDL audio get-callback request pattern");
  ```
  In `SDLCallback`: capture `req_at_entry = additional_amount` before the loop; count real/silence
  frames in each branch; after the loop, when `REXCVAR_GET(audio_diag)`, accumulate into file-static
  counters and `REXAPU_INFO` (first 20 calls + every 200th): `additional_amount`, `total_amount`,
  per-call real/silence, cumulative real-% , cumulative requested MB, and
  `SDL_GetAudioStreamQueued`/`SDL_GetAudioStreamAvailable`.
- Host note: the bench `Speakers` sink was muted/0 %; left **unmuted at 100 %** after the audit.

## 5. Bottom line
"Fix": **nothing to fix** — the audio path is correct end-to-end (decode → SDL → PipeWire → device).
"Optimize": one real lever identified — the **3× get-callback over-pull** that triples the audio-chain
CPU. It is harmless to the sound and its root cause is SDL-internal/process-specific (unsolved), so it
is **documented here rather than patched**. The reusable probe + analyzer + `audio_diag` recipe make a
future, A/B-gated attempt straightforward.
