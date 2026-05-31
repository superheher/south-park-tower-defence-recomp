#!/usr/bin/env python3
"""Analyze a raw F32LE interleaved PCM dump produced by the SDL audio driver's
`--audio_dump` cvar. The dump is a sequence of fixed 256-sample-per-channel
frames; each frame is either a real (converted) game frame or an all-zero
silence-fill the SDL callback inserts when frames_queued_ is empty (an underrun).

Reports, objectively:
  * level   : peak / RMS / clipping / DC offset (is there real signal? distorted?)
  * underrun: zero-fill frames that interrupt active audio (the crackle source)
  * clicks  : large sample-to-sample discontinuities (glitches)
  * spectrum: spectral centroid / rolloff (music/speech vs broadband noise)

Usage: analyze_dump.py <dump.f32> <channels>
"""
import sys
import numpy as np

SR = 48000
BLOCK = 256  # samples per channel per audio frame (matches channel_samples_)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    path, ch = sys.argv[1], int(sys.argv[2])
    raw = np.fromfile(path, dtype="<f4")
    if raw.size == 0:
        print(f"EMPTY dump: {path} (0 samples) -> audio callback never ran / device never opened")
        sys.exit(1)
    n_full = (raw.size // ch) * ch
    if n_full != raw.size:
        print(f"warn: {raw.size - n_full} trailing samples not a whole frame, truncating")
    x = raw[:n_full].reshape(-1, ch)  # (samples, channels)
    nsamp = x.shape[0]
    dur = nsamp / SR
    print(f"== dump: {path}  channels={ch}  samples/ch={nsamp}  duration={dur:.2f}s ==\n")

    # ---- level ----
    peak = np.max(np.abs(x), axis=0)
    rms = np.sqrt(np.mean(x.astype(np.float64) ** 2, axis=0))
    dc = np.mean(x.astype(np.float64), axis=0)
    clip = np.sum(np.abs(x) > 1.0, axis=0)
    nan = np.sum(~np.isfinite(x))
    print("[level] per channel:")
    for c in range(ch):
        dbfs = 20 * np.log10(rms[c]) if rms[c] > 0 else -np.inf
        print(f"  ch{c}: peak={peak[c]:.4f}  rms={rms[c]:.5f} ({dbfs:6.1f} dBFS)  "
              f"dc={dc[c]:+.5f}  clip={clip[c]}")
    print(f"  total clip(|x|>1.0)={int(clip.sum())}  non-finite(nan/inf)={int(nan)}")
    overall_peak = float(peak.max())
    if overall_peak == 0.0:
        print("\n  *** ENTIRE dump is digital silence: no audio was ever produced. ***")
        sys.exit(0)

    # ---- frame (256-sample block) classification ----
    nblk = nsamp // BLOCK
    blk = x[: nblk * BLOCK].reshape(nblk, BLOCK, ch)
    blk_absmax = np.max(np.abs(blk), axis=(1, 2))      # per-block peak
    silent = blk_absmax == 0.0                          # exact-zero block (silence-fill or digital silence)
    active = ~silent
    n_sil, n_act = int(silent.sum()), int(active.sum())
    print(f"\n[frames] {nblk} blocks of {BLOCK} samp ({BLOCK/SR*1000:.2f} ms each): "
          f"active={n_act} ({100*n_act/nblk:.1f}%)  silent={n_sil} ({100*n_sil/nblk:.1f}%)")

    # ---- underrun: silent runs that sit BETWEEN active audio (gaps), vs leading/trailing/long silence ----
    # find runs of silent blocks
    runs = []
    i = 0
    while i < nblk:
        if silent[i]:
            j = i
            while j < nblk and silent[j]:
                j += 1
            runs.append((i, j - i))  # (start, length)
            i = j
        else:
            i += 1
    first_act = int(np.argmax(active)) if n_act else nblk
    last_act = nblk - 1 - int(np.argmax(active[::-1])) if n_act else -1
    interior = [(s, L) for (s, L) in runs if s > first_act and s + L - 1 < last_act]
    short_gaps = [L for (s, L) in interior if L <= 8]      # <= ~43ms: classic underrun crackle
    long_gaps = [L for (s, L) in interior if L > 8]        # genuine pauses inside content
    gap_blocks = sum(L for (s, L) in interior)
    print(f"\n[underrun] active span = blocks [{first_act}..{last_act}] "
          f"({(last_act-first_act+1)*BLOCK/SR:.2f}s of the {dur:.2f}s clip)")
    print(f"  interior silent gaps: {len(interior)} runs, {gap_blocks} blocks "
          f"({gap_blocks*BLOCK/SR*1000:.0f} ms total dropped)")
    print(f"    short (<=8 blk / <=43ms, crackle-like): {len(short_gaps)} runs"
          + (f"  lens={short_gaps[:30]}" if short_gaps else ""))
    print(f"    long  (>8 blk, genuine pause-like):     {len(long_gaps)} runs"
          + (f"  lens={long_gaps[:20]}" if long_gaps else ""))
    if (last_act - first_act + 1) > 0:
        underrun_rate = len(interior) / ((last_act - first_act + 1) * BLOCK / SR)
        print(f"  interior-gap rate = {underrun_rate:.2f} gaps/s within active audio")

    # ---- clicks: large sample-to-sample jumps (downmixed/output stream) ----
    mono = x.mean(axis=1)
    d = np.abs(np.diff(mono))
    for thr in (0.15, 0.30, 0.50):
        cnt = int(np.sum(d > thr))
        print(f"\n[clicks] |Δsample|>{thr}: {cnt} ({cnt/dur:.1f}/s)" if thr == 0.15
              else f"          |Δsample|>{thr}: {cnt} ({cnt/dur:.1f}/s)")
    print(f"          max |Δ| = {d.max():.4f}")

    # ---- spectrum (active audio only): music/speech vs broadband noise ----
    act_idx = np.where(active)[0]
    if act_idx.size:
        seg = blk[act_idx].reshape(-1, ch).mean(axis=1)
        seg = seg[: (seg.size // 1024) * 1024]
        if seg.size >= 1024:
            S = np.abs(np.fft.rfft(seg.reshape(-1, 1024) * np.hanning(1024), axis=1)).mean(axis=0)
            freqs = np.fft.rfftfreq(1024, 1 / SR)
            S[0] = 0
            if S.sum() > 0:
                centroid = float((freqs * S).sum() / S.sum())
                csum = np.cumsum(S)
                rolloff = float(freqs[np.searchsorted(csum, 0.85 * csum[-1])])
                lo = S[freqs < 500].sum() / S.sum()
                mid = S[(freqs >= 500) & (freqs < 4000)].sum() / S.sum()
                hi = S[freqs >= 4000].sum() / S.sum()
                print(f"\n[spectrum] centroid={centroid:.0f}Hz  85%rolloff={rolloff:.0f}Hz  "
                      f"energy lo<500={lo*100:.0f}% mid={mid*100:.0f}% hi>4k={hi*100:.0f}%")
                print("  (music/speech: centroid ~0.5-3kHz, energy lo/mid-heavy; "
                      "white noise: centroid ~SR/4≈12kHz, flat)")


if __name__ == "__main__":
    main()
