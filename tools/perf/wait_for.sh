#!/usr/bin/env bash
# wait_for.sh <file> <marker> <timeout_s> -- poll <file> until <marker> appears or timeout.
f="${1:?file}"; marker="${2:?marker}"; to="${3:-180}"
end=$(( $(date +%s) + to ))
while [ "$(date +%s)" -lt "$end" ]; do
  if grep -q "$marker" "$f" 2>/dev/null; then echo "FOUND after $(( $(date +%s) - (end - to) ))s"; exit 0; fi
  sleep 2
done
echo "TIMEOUT after ${to}s"; exit 1
