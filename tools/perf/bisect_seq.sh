#!/usr/bin/env bash
set -u
HERE=/home/h/src/recomp/rexglue-recomps/south-park-recomp/tools/perf
for pair in stub_B97C0:821B97C0 stub_BC3E8:821BC3E8 stub_BC738:821BC738 ; do
  tag="${pair%%:*}"; addr="${pair##*:}"
  echo ">>> $tag (stub $addr)  $(date +%H:%M:%S)"
  bash "$HERE/shot_stub.sh" "$addr" "$tag"
done
echo "ALL_BISECT_DONE"
