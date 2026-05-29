#!/usr/bin/env bash
# detdiff.sh -- determinism-diff correctness gate (CODEGEN-SIZE-PLAN Phase 0).
#
# Drives a FIXED scripted session (gamectl play boot+nav -> Stan's House, then a
# fixed combat dwell so the hot recompiled code runs), captures a semantic
# fingerprint of run.log (detdiff_fp.py), and compares it to a baseline reference
# (the noise mask is derived empirically from K baseline runs). Behaviour-
# equivalence, NOT bit-exact (a realtime MT game isn't deterministic).
#
#   detdiff.sh capture  <out_fp.json> [dwell]   one scripted run -> fingerprint
#   detdiff.sh baseline [N] [dwell]             N baseline runs -> reference.json
#   detdiff.sh gate     [label] [dwell]         one candidate run -> pass/fail verdict
#
# Verdict line (last line of `gate`): DETDIFF status=pass|fail reason=...
#
# NOTE: we do NOT block on `gamectl play` returning -- under a detached (no
# job-control) shell its tail can park in do_wait on the never-exiting game.
# Instead we launch it in the background and poll run.log for the deterministic
# nav milestones, then reclaim the game (which also frees the stuck wrapper).
set -u
ROOT="${REX_GAME_ROOT:-/home/h/src/recomp/rexglue-recomps/south-park-recomp}"
GAMECTL="$ROOT/tools/gamectl.sh"
FP="$ROOT/tools/perf/detdiff_fp.py"
GAME_DIR="${REX_GAME_DIR:-$ROOT/out/build/linux-amd64-release}"
LOG="$GAME_DIR/run.log"
DD="$ROOT/tools/perf/detdiff"; mkdir -p "$DD"
REF="$DD/reference.json"
DWELL_DEFAULT=40

cleanup() { "$GAMECTL" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; }
trap cleanup EXIT

# Drive one fixed scripted session; on success leave a fresh in-level run.log and
# echo "OK". Polls the log rather than waiting on play's return.
run_session() {
  local dwell="$1" try i got_camp
  for try in 1 2 3; do
    "$GAMECTL" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 1
    ( "$GAMECTL" play >/tmp/sp/detdiff_play.log 2>&1 ) &
    # 1) campaign select (camp_diagram) = boot + intro + menu nav done, <=120s
    got_camp=0
    for i in $(seq 1 60); do
      grep -q 'camp_diagram' "$LOG" 2>/dev/null && { got_camp=1; break; }
      sleep 2
    done
    if [ "$got_camp" = 1 ]; then
      # 2) level entry: the A-press sequence enters Stan's House (its .lua probe is
      #    the level-load marker); soft 50s cap so we don't over-fit to one marker.
      for i in $(seq 1 25); do
        grep -q 'Stans_House.lua' "$LOG" 2>/dev/null && break
        sleep 2
      done
      sleep "$dwell"            # 3) fixed in-level combat dwell
      # 4) re-verify the FINAL log is a healthy in-level run. Guards the boot-flake
      #    race where camp_diagram latched on one attempt but a later relaunch reset
      #    run.log to a partial boot, AND catches a candidate that crashed mid-dwell
      #    (a crashing candidate must NOT be captured as OK -- it must retry->FAIL).
      if grep -q 'camp_diagram' "$LOG" 2>/dev/null && \
         [ "$(grep -c 'pacing-diag' "$LOG" 2>/dev/null)" -ge 10 ]; then
        echo "OK"; return 0
      fi
      echo "  [detdiff] attempt $try: lost in-level during dwell (crash/reset) -> retry" >&2
    else
      echo "  [detdiff] attempt $try: no camp_diagram in 120s -> retry" >&2
    fi
  done
  echo "FAIL"; return 1
}

capture() {           # capture <out_fp.json> [dwell]
  local out="$1" dwell="${2:-$DWELL_DEFAULT}" r
  r="$(run_session "$dwell")"
  if [ "$r" != "OK" ]; then
    echo "  [detdiff] SESSION FAILED (no in-level after 2 tries)" >&2
    echo '{"session":"failed"}' > "$out"; return 1
  fi
  cp -f "$LOG" "${out%.json}.run.log"
  python3 "$FP" fp "${out%.json}.run.log" > "$out"
  python3 - "$out" <<'PY'
import json,sys
f=json.load(open(sys.argv[1]))
p=f.get('pacing',{}); r=f.get('reached',{})
print("  [detdiff] captured: markers=%d assets=%d warns=%d errs=%d naninf=%d pipes=%d pacing=%d in_level=%s fps_med=%s"
      % (len(f.get('markers',[])),len(f.get('assets',[])),len(f.get('warnings',[])),
         len(f.get('errors',[])),f.get('naninf',-1),f.get('pipeline_count',-1),
         p.get('count',-1),r.get('in_level'),p.get('fps_med')))
PY
  return 0
}

case "${1:-}" in
  capture)
    capture "${2:?out_fp.json}" "${3:-$DWELL_DEFAULT}" ;;

  baseline)
    N="${2:-3}"; dwell="${3:-$DWELL_DEFAULT}"; fps=()
    echo "## detdiff baseline: $N runs, dwell=${dwell}s"
    for i in $(seq 1 "$N"); do
      echo "== baseline run $i/$N =="
      capture "$DD/baseline_$i.json" "$dwell" && fps+=("$DD/baseline_$i.json")
    done
    [ "${#fps[@]}" -ge 2 ] || { echo "need >=2 good baselines, got ${#fps[@]}"; exit 1; }
    python3 "$FP" buildref "$REF" "${fps[@]}"
    echo "== baseline-vs-baseline self-consistency (each baseline gated vs the reference) =="
    ok=1
    for f in "${fps[@]}"; do
      line="$(python3 "$FP" gate "$REF" "$f")"
      echo "  $(basename "$f"): $line"
      printf '%s' "$line" | grep -q 'status=pass' || ok=0
    done
    [ "$ok" = 1 ] && echo "DETDIFF-SELFCHECK status=pass (reference is self-consistent)" \
                  || echo "DETDIFF-SELFCHECK status=FAIL (reference too tight -- widen mask / more runs)"
    ;;

  gate)
    label="${2:-cand}"; dwell="${3:-$DWELL_DEFAULT}"
    [ -f "$REF" ] || { echo "DETDIFF status=fail reason=no_reference (run 'detdiff.sh baseline' first)"; exit 1; }
    cf="$DD/cand_${label}.json"
    if ! capture "$cf" "$dwell"; then
      echo "DETDIFF status=fail reason=session_failed_no_in_level"; exit 1
    fi
    python3 "$FP" gate "$REF" "$cf" ;;

  *) sed -n '2,15p' "$0"; exit 2 ;;
esac
