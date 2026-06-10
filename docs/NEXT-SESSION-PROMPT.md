# Project parked at v1.0 (2026-06-10) — read STATUS.md first

v1.0 shipped: the game is natively playable on this host, mostly-60 fps;
the maintainer play-verified it and decided to stop here. There is NO active
task. Do not start perf work, measurements, or refactors on your own.

If the maintainer asks to resume (v2): open `docs/ROADMAP-V2.md` — it holds
the full preserved context: the quantified problem (level-2+ peaks 1900–2250
draws/swap → clean 30.0 re-grid = 2× slow-mo; translate ≈8.4 µs/draw), Step 0
(size level-3+ peaks FIRST, protocol in `tools/perf/protocol/`), the two
levers (A: texture-array draw batching, XL; B: guest↔CP pipelining +
swap-at-ring-arrival, deep) with their risks, the Windows-port checklist, and
the unchanged gates (ab_both keep-bar, detdiff, cold-soak, screenshots).

If the maintainer asks for a Windows build: ROADMAP-V2 §"Windows port
checklist" — two mechanical code gaps (NotifyVblank impl is posix-only;
freq_keeper uses pthread_setname_np unguarded), two never-compiled win files
(seh_win.cpp, window_win.cpp), everything else carries over.

Standing rules: no Telegram to the maintainer; no GUI sessions while he may
be at the machine (coordinate or overnight); clean `/dev/shm/xenia_memory_*`
before manual runs; bound every GUI wait; rollback anchors = tag `v1.0` /
`checkpoint-2026-06-10-playable` + sdk branch `checkpoint/2026-06-10-playable`.
