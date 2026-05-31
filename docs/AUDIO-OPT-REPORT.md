# The "3× audio over-pull" is 3 concurrent SDL streams each at 1× — not an over-pull. REJECTED with evidence.

**Date:** 2026-05-31. **Goal (from `NEXT-SESSION-AUDIO-OPT-PROMPT.md`):** cut the audio-chain CPU by
making the SDL playback path run at 1× realtime instead of 3×, without regressing the (already-correct)
output — ship it A/B-gated, or reject with evidence.

**Verdict: REJECT.** The task's premise — that *one* SDL get-callback over-fires 3× and drives the
decode chain "3× harder than realtime" — is **false**. The game opens **3 SDL playback streams** (it
registers 3 guest XAudio render-driver clients), and **each stream pulls a correct 1×**. The "3×" is
their *aggregate*; the single-file `--audio_dump` interleaved the three, which is what looked like a 3×
over-pull. Every stream is **already at 1×**, so there is nothing to throttle — rate-limiting any stream
below the device drain rate would simply starve it (underrun). Moreover the **expensive XMA decode
already runs 1×**: only one of the three clients carries audio; the other two are silent. There is no
safe, worthwhile CPU lever. **Nothing shipped; kept `.so` `1a3f6076` (exe `848f191c`) unchanged; SDK
tree restored to canonical.**

---

## 1. The in-game measurement (cvar-gated `audio_diag`, diagnostic-only)
Instrumented `SDLCallback` (per-call timing/req/queued + per-driver real/active/peak) and `Initialize`
(obtained device format) under a temporary `--audio_diag` cvar. In-level (Stan's House), steady state
over a **600 s** dwell:

| metric | value |
|---|---|
| distinct SDL streams (drivers) | **3** |
| device format (each stream) | `freq=48000 ch=6 fmt=F32LE sample_frames=1024`, **no resample** (src==dst) |
| aggregate get-callbacks | **140.6 /s** |
| per call | `req=24576 B` = exactly **one** 1024-frame device buffer; `real=4 sil=0` |
| aggregate real-frame production | **562 /s = 3.00×** the 187.5/s realtime (48000/256) |
| per stream | 140.6/3 = **46.9 calls/s = the quantum rate = 1.0×** |

Every sampled call (across all 3 streams, 80 k+ calls) requests exactly one device buffer and produces
4 real frames — **no stream ever pulls more than 1×.**

## 2. Per-stream breakdown — only **one** of the three clients carries audio
The three clients all use the **same** guest callback `0x82311EE8` with **distinct args**
(`4002D890` / `4002D8E8` / `45F57A30`). Per-stream activity in-level:

| client (idx) | real/s | active % | peak | role |
|---|---|---|---|---|
| **0** | 187.1 (**1.00×**) | **90–94 %** | **0.960** | **the audio** (music + SFX) |
| 1 | 181.3 (0.97×) | 0.1–0.2 % | 0.146 | ~silent |
| 2 | 157.3 (0.84×) | **0.0 %** | **0.000** | silent |

⇒ The **XMA (FFmpeg) decode runs 1×** — only client 0 has real PCM. The two silent clients emit
zero-frames (a cheap `memset`/convert, no real decode). This also **explains the prior report's "70 %
silence / 33 % active"**: it is the *two idle client streams*, not "ch2–5 zeros" — `(90+0+0)/3 ≈ 30 %`
active matches the dump's measured 33 %. The single-file dump simply interleaved 1 active + 2 silent
streams.

## 3. Independent confirmation — the standalone probe reproduces the 3× with **no game**
`tools/audio/sdl_pull_probe_multi.c` opens *N* streams exactly like `SDLAudioDriver::Initialize`
(48000/F32LE/6ch, same 3 hints, same bundled `libSDL3.a`), feeding silence, **no game / no XMA / no
guest**:

| N streams | per-stream | aggregate |
|---|---|---|
| 1 | 46.3 calls/s = **0.987×** | 0.987× |
| **3** | 46.8 calls/s = **0.997×** each | **140.3 calls/s = 2.992×** |

This matches the game's 140.6 calls/s / 3.00× **exactly**. ⇒ the 3× is *purely* N streams × 1× each —
**not** an SDL bug, **not** process-specific. The earlier single-stream probe "failed to reproduce the
3×" only because it opened **one** stream; opening three reproduces it perfectly.

## 4. Mechanism (from source)
- `AudioSystem::RegisterClient` is called **1:1** from the guest export
  `XAudioRegisterRenderDriverClient` (ord `0x1F3`, `xboxkrnl_audio.cpp:54-79`). The **game** calls it 3
  times → 3 clients → 3 `SDLAudioDriver`s → 3 SDL streams. No SDK-side multiplication.
- SDL3's PipeWire backend fires its `process` callback **once per graph quantum per node**
  (`SDL_pipewire.c:1011 output_callback → SDL_PlaybackAudioThreadIterate`). At quantum `1024 / 48000`
  that is 46.875 pulls/s = **1× per node/stream**.
- `SDLCallback` releases the **per-client** semaphore once per real frame consumed, so the guest
  produces at the consume rate = **1× per client** (bounded credit `queued_frames_ = 8`).
- Net: 3 nodes × 47/s × 4 frames = 562 frames/s aggregate, each device node draining a correct 1×;
  PipeWire mixes the three at the device — exactly as XAudio2 mixed voices into one output on the 360.

## 5. Why there is no fix
1. **No over-pull to throttle.** Each stream is already at 1×. The prompt's Step-B rate-limit presupposes
   a stream exceeding the device drain rate; none does. Capping below 1× → underrun (the named risk).
2. **The clients are guest-driven.** The recomp faithfully mirrors 3 `XAudioRegisterRenderDriverClient`
   calls. Dropping or merging clients diverges from the guest's audio engine → a correctness risk against
   the hard constraint "without regressing the audible output."
3. **The expensive work is already 1×.** Only client 0 decodes real XMA. The 2 silent clients cost only a
   cheap silence `memset`/convert + a guest-callback dispatch. Suppressing their semaphore release is
   **unsafe** (it would drop the onset of a sound the instant that voice activates) and **low-ROI** (audio
   is no longer in the hot set after the patch-0015 worker de-spin).
4. **Merging 3 streams → 1** would *not* cut the decode (the guest still produces 3 buses), only the cheap
   SDL conversion + 2 PipeWire nodes, and is a risky behavioral rewrite of `AudioSystem`'s per-client
   model. Out of scope, low payoff.

## 6. Corrections to `AUDIO-INVESTIGATION-REPORT.md`
- **Retract:** "the SDL get-callback over-fires 3× … drives the chain 3× harder than realtime = wasted
  CPU." It does not. Three streams each at 1×; the XMA decode runs 1×.
- The dump's **"70 % silence / 33 % active"** = the **2 idle client streams**, not ch2–5 zeros.
- "The probe does **not** reproduce the 3×" → it **does**, at **N = 3** (the old probe used N = 1).
- These do **not** change the report's correct headline: **the audible output is fine** (1×, normal
  pitch, continuous, clean). Only the *cause* of the "3×" is corrected (3 streams, not 1 over-pulling).

## 7. Output correctness — unchanged and re-confirmed
`.so` unchanged (`1a3f6076`). The active stream (client 0) is continuous real audio: 90–94 % active,
peak 0.960 (pre-device), 1× realtime — corroborating the report's `Speakers.monitor` baseline (1×
realtime, 99.6 % active, centroid 2069 Hz, peak 0.59, 0 clicks > 0.3). Fresh confirmatory
`Speakers.monitor` capture on the restored kept `.so`:

`parecord Speakers.monitor` (18 s, in-combat, stereo F32 → `analyze_dump.py 2`):

| metric | value | bar |
|---|---|---|
| speed | **16.9 s audio / ~18 s wall ≈ 1× realtime** | 1× |
| active | **99.9 %** (3164/3168 blocks) | ≥99 % ✓ |
| interior gaps | **0 runs, 0 ms** | none ✓ |
| clicks > 0.3 | **0** (only 6 > 0.15, max Δ 0.174) | 0 ✓ |
| spectral centroid | **2538 Hz**, 85 % rolloff 5156 Hz, energy lo 37 %/mid 44 %/hi 19 % | normal music/speech ✓ |
| level | peak 0.41, RMS −21.3 dBFS, 0 clip, 0 nan/inf | clean ✓ |

(Centroid 2538 Hz vs the report's 2069 Hz is content, not pitch — this capture was mid-combat, SFX-heavy;
both sit in the 0.5–3 kHz music/speech band, lo/mid-weighted, i.e. **normal pitch, not chipmunk/decimated**.)
Since nothing was changed, there is **no regression** by construction; the recording validates the `.so`
restore and the measurement chain.

## 8. Artifacts / state
- `tools/audio/sdl_pull_probe_multi.c` — N-stream pull-rate probe (the decisive, game-free reproduction).
- `audio_diag` cvar recipe (per-driver real/active/peak + obtained device format + `RegisterClient`
  callback identity) — **diagnostic only, reverted** from the tree.
- **SDK tree restored to canonical:** `audio_system.cpp` → pristine `e8ce24f`; `sdl_audio_driver.cpp` →
  pristine + patch `0007` (audio_dump). Canonical rebuild is clean (0 `audio-diag` strings). Kept `.so`
  `1a3f6076` restored to the game dir (exe `848f191c`).

## 9. Bottom line
The audio chain runs at **exactly the rate the guest's three render-driver clients require** — 1× each,
one of them active. There is **no 3× over-pull** and **no safe CPU lever**. Audio is correct end-to-end.
**The "3× audio CPU" lever is CLOSED.**
