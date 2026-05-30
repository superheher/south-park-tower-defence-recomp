#!/usr/bin/env bash
# Measure user vs kernel cycles on the guest-sim (Main) thread during combat. If kernel% is tiny,
# CPU security mitigations (which cost only at user<->kernel transitions) have ~no budget on the floor.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
exec >/tmp/uksplit.txt 2>&1
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 2
"$ROOT/tools/gamectl.sh" play >/tmp/uk_play.log 2>&1
PID=$(pgrep -x south_park_td); [ -z "$PID" ] && { echo BOOT_FAILED; exit 0; }
MTID=$(ps -L -o tid,comm -p "$PID" 2>/dev/null | awk '/Main/{print $1}' | head -1)
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
curfps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in $(seq 1 80); do f=$(curfps); [ -n "$f" ] && awk "BEGIN{exit !($f<28)}" && break; sleep 0.5; done
echo "Main_tid=$MTID  heavy fps=$(curfps)"
perf stat -t "$MTID" -e cycles:u,cycles:k,instructions:u,instructions:k,ref-cycles -- sleep 15 2>/tmp/uk_g.txt
cat /tmp/uk_g.txt
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
python3 - <<'PY'
import re
def v(ev):
    for l in open('/tmp/uk_g.txt'):
        m=re.match(r'\s*([\d,]+)\s+'+re.escape(ev),l)
        if m: return float(m.group(1).replace(',',''))
    return 0.0
cu,ck=v('cycles:u'),v('cycles:k'); iu,ik=v('instructions:u'),v('instructions:k')
tot=cu+ck
if tot>0:
    print(f"\n=== Main-thread user/kernel split ===")
    print(f"  user cycles   = {cu/1e9:.2f}B ({100*cu/tot:.1f}%)")
    print(f"  kernel cycles = {ck/1e9:.2f}B ({100*ck/tot:.1f}%)  <-- the entire mitigation budget lives here")
    print(f"  user insn={iu/1e9:.2f}B kernel insn={ik/1e9:.2f}B")
    print(f"  => an upper bound on any mitigations=off gain is ~the kernel% ({100*ck/tot:.1f}%), and only a fraction of THAT is mitigation overhead")
PY
echo UKSPLIT_DONE
