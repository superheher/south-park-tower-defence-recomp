#!/usr/bin/env bash
# wizard_measure.sh -- gate (correctness) + floor A/B for the compile-level wizard
# levers (march=native, +machine-outliner) vs Phase C. The .text projection already
# showed -2.0% / -8.87%; this answers the only question that matters: does the cut
# move the I-cache-capacity-bound combat FLOOR? detdiff gate is mandatory (ranks above fps).
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME="$ROOT/out/build/linux-amd64-release"
DETDIFF="$ROOT/tools/perf/detdiff.sh"
AB="$ROOT/tools/perf/ab.sh"
GAMECTL="$ROOT/tools/gamectl.sh"
OUT="${OUT:-/tmp/wizard_measure.txt}"; exec >"$OUT" 2>&1
cd "$ROOT" || exit 1

PHASEC=/tmp/sp_phaseC
NATIVE="$ROOT/out/build/linux-amd64-wiz-native/south_park_td"
OUTLINE="$ROOT/out/build/linux-amd64-wiz-outline/south_park_td"

# stable copy of the CURRENT staged Phase C binary (the A/B baseline)
cp -f "$GAME/south_park_td" "$PHASEC"
echo "Phase C binary md5: $(md5sum "$PHASEC" | cut -c1-12)  .text=$(size "$PHASEC"|awk 'NR==2{print $1}')"
echo "native    md5: $(md5sum "$NATIVE" | cut -c1-12)  .text=$(size "$NATIVE"|awk 'NR==2{print $1}')"
echo "outline   md5: $(md5sum "$OUTLINE" | cut -c1-12)  .text=$(size "$OUTLINE"|awk 'NR==2{print $1}')"

gate() {  # gate <label> <binary>  -> echoes pass|fail
  local label="$1" bin="$2"
  "$GAMECTL" kill >/dev/null 2>&1
  cp -f "$bin" "$GAME/south_park_td"
  local v; v="$("$DETDIFF" gate "$label" 40 2>&1 | tee -a "$OUT" | grep -oE 'status=(pass|fail)' | tail -1)"
  echo "$v"
}

echo "============================ GATE: native ============================"
NATIVE_V="$(gate wiz_native "$NATIVE")"
echo ">>> native gate: ${NATIVE_V:-unknown}"
echo "============================ GATE: outline ============================"
OUTLINE_V="$(gate wiz_outline "$OUTLINE")"
echo ">>> outline gate: ${OUTLINE_V:-unknown}"

# restore Phase C as the live baseline before the floor A/B
"$GAMECTL" kill >/dev/null 2>&1
cp -f "$PHASEC" "$GAME/south_park_td"

# build the floor A/B variant list from gate-passing candidates only
ARGS=(base "$PHASEC")
[ "$NATIVE_V"  = "status=pass" ] && ARGS+=(native  "$NATIVE")
[ "$OUTLINE_V" = "status=pass" ] && ARGS+=(outline "$OUTLINE")

echo "============================ FLOOR A/B ============================"
echo "variants in A/B: ${ARGS[*]}"
if [ "${#ARGS[@]}" -ge 4 ]; then
  TARGET="$GAME/south_park_td" "$AB" 90 3 "${ARGS[@]}"
else
  echo "No gate-passing candidate -> skipping floor A/B (both failed correctness)."
fi

# restore Phase C (known-good) at the end no matter what
"$GAMECTL" kill >/dev/null 2>&1
cp -f "$PHASEC" "$GAME/south_park_td"
echo "restored Phase C ($(md5sum "$GAME/south_park_td"|cut -c1-12)) as the live binary."
echo "WIZARD_MEASURE_DONE"
