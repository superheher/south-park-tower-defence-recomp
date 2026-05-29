#!/usr/bin/env python3
"""detdiff_fp.py -- semantic fingerprint extractor + comparator for the
South Park recomp determinism-diff correctness gate (CODEGEN-SIZE-PLAN Phase 0).

A realtime multithreaded game is NOT bit-deterministic (frame timing, thread
interleave, time-seeded RNG), so we compare *behaviour-equivalence*: the ordered
game-logic progression and the absence of new errors/NaNs/crashes -- not bytes.

A wrong recompiled computation almost always cascades into divergent control
flow (different asset/sound loads, a new warning/assert, a NaN, or a crash),
which this fingerprint catches.

Subcommands:
  fp     <run.log>                      -> print one run's fingerprint as JSON
  buildref <ref.json> <fp1.json> ...    -> aggregate K baseline fps into a reference
  gate   <ref.json> <candidate_fp.json> -> print "DETDIFF status=pass|fail reason=..."

The fingerprint is intentionally broad; buildref derives the noise mask
empirically (required = intersection across baselines, allowed = union), so
run-to-run variation is masked and only divergence *beyond* it is flagged.
"""
import sys, json, re

# ---- normalization ---------------------------------------------------------
TS   = re.compile(r'^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+\]\s*')
SEV  = re.compile(r'^\[(info|warning|error|critical|debug|trace)\]\s*')
SUB  = re.compile(r'^\[([a-z0-9_]+)\]\s*')
TID  = re.compile(r'\[t\d+\]\s*')

def normalize(body):
    """Strip volatile tokens (host addresses, thread ids, pointers) but keep
    semantically meaningful content (asset names, ids, fixed guest handles)."""
    s = TID.sub('', body)
    s = re.sub(r'native=[0-9A-Fa-f]+', 'native=H', s)
    s = re.sub(r'-> thunk at [0-9A-Fa-f]+', '-> thunk at H', s)
    s = re.sub(r'\(object \d+: Instance 0x[0-9a-fA-F]+\)', '', s)
    s = re.sub(r'0x[0-9a-fA-F]+', '0xH', s)
    s = re.sub(r'Instance 0x?[0-9a-fA-F]+', 'Instance H', s)
    return s.strip()

# NaN/Inf as standalone float tokens; \b stops "info"/"infinite" matching.
NANINF = re.compile(r'(?<![\w.])[+-]?(nan|inf)(?:\([a-z]+\))?(?![\w])', re.I)
ASSET  = re.compile(r"(?:served|entry not found for|Loading XEX image:|FAILED: path=)\s*'?([^']+)'?")
PIPE   = re.compile(r'Creating graphics pipeline state with (VS \w+, PS \w+)')
PACING = re.compile(r'\[pacing-diag\]\s*swaps\s*([0-9.]+)/s\s*loading=(true|false)')

def parse_line(line):
    line = line.rstrip('\n')
    m = TS.match(line)
    rest = line[m.end():] if m else line
    sm = SEV.match(rest)
    sev = sm.group(1) if sm else '?'
    rest2 = rest[sm.end():] if sm else rest
    subm = SUB.match(rest2)
    sub = subm.group(1) if subm else '?'
    body = rest2[subm.end():] if subm else rest2
    return sev, sub, body, line

def extract(path):
    sev_ct = {'info':0,'warning':0,'error':0,'critical':0,'debug':0,'trace':0,'?':0}
    markers, assets, warns, errs, pipes = set(), set(), set(), set(), set()
    naninf = 0
    pacing_n = 0; pacing_false = 0; fps = []
    reached = {'xex':False,'camp_diagram':False,'stans_house':False,'in_level':False}
    try:
        raw = open(path, errors='replace').read().splitlines()
    except FileNotFoundError:
        return None
    for line in raw:
        if not line.strip():
            continue
        sev, sub, body, full = parse_line(line)
        sev_ct[sev] = sev_ct.get(sev,0)+1
        # NaN/Inf anywhere
        if NANINF.search(body):
            naninf += 1
        # pacing
        pm = PACING.search(body)
        if pm:
            pacing_n += 1
            if pm.group(2) == 'false':
                pacing_false += 1; reached['in_level']=True
            try: fps.append(float(pm.group(1)))
            except: pass
            continue
        # GPU vulkan-loader spam: drop entirely (volatile ordering, no game meaning)
        if 'Vulkan Info' in body:
            continue
        pp = PIPE.search(body)
        if pp:
            pipes.add(pp.group(1))
            continue
        nb = normalize(body)
        # asset paths touched (the "different asset" detector)
        am = ASSET.search(nb)
        if am:
            assets.add(am.group(1))
        # reach markers
        if 'Loading XEX image' in nb: reached['xex']=True
        if 'camp_diagram' in nb: reached['camp_diagram']=True
        if 'Stans_house' in nb or 'Stans_House' in nb: reached['stans_house']=True
        # severity buckets -> sets
        if sev == 'warning':
            warns.add('[%s] %s' % (sub, nb))
        elif sev in ('error','critical'):
            errs.add('[%s] %s' % (sub, nb))
        # markers = non-gpu game-logic progression (sys/krnl/fs/core), deduped SET
        if sub in ('sys','krnl','fs','core') and sev in ('info','warning','error','critical'):
            markers.add('[%s] %s' % (sub, nb))
    fps.sort()
    fp = {
        'lines_total': len(raw),
        'sev': sev_ct,
        'markers': sorted(markers),
        'assets': sorted(assets),
        'warnings': sorted(warns),
        'errors': sorted(errs),
        'naninf': naninf,
        'pipelines': sorted(pipes),
        'pipeline_count': len(pipes),
        'pacing': {'count':pacing_n,'in_level_samples':pacing_false,
                   'fps_min':(fps[0] if fps else 0),
                   'fps_max':(fps[-1] if fps else 0),
                   'fps_med':(fps[len(fps)//2] if fps else 0)},
        'reached': reached,
    }
    return fp

def buildref(fps):
    ms = [set(f['markers']) for f in fps]
    ast= [set(f['assets']) for f in fps]
    wn = [set(f['warnings']) for f in fps]
    pp = [set(f['pipelines']) for f in fps]
    inter = lambda L: sorted(set.intersection(*L)) if L else []
    union = lambda L: sorted(set.union(*L)) if L else []
    ref = {
        'n': len(fps),
        'required_markers': inter(ms),
        'union_markers': union(ms),
        'required_assets': inter(ast),
        'union_assets': union(ast),
        'union_warnings': union(wn),
        'error_bodies': union([set(f['errors']) for f in fps]),
        'naninf_max': max(f['naninf'] for f in fps),
        'pacing_count_min': min(f['pacing']['count'] for f in fps),
        'pipelines_required': inter(pp),
        'pipelines_union': union(pp),
        'reached_required': {k: all(f['reached'][k] for f in fps)
                             for k in fps[0]['reached']},
    }
    return ref

def gate(ref, c):
    fails, notes = [], []
    # 1. new error/critical lines (baseline had a known set, usually empty)
    new_err = set(c['errors']) - set(ref['error_bodies'])
    if new_err:
        fails.append('new_error(%d):%s' % (len(new_err), list(new_err)[:2]))
    # 2. NaN/Inf beyond baseline
    if c['naninf'] > ref['naninf_max']:
        fails.append('naninf=%d>max%d' % (c['naninf'], ref['naninf_max']))
    # 3. missing required milestone markers
    miss_m = set(ref['required_markers']) - set(c['markers'])
    if miss_m:
        fails.append('missing_milestone(%d):%s' % (len(miss_m), list(miss_m)[:2]))
    # 4. new milestone markers never seen in any baseline (extra milestone)
    new_m = set(c['markers']) - set(ref['union_markers'])
    if new_m:
        fails.append('new_milestone(%d):%s' % (len(new_m), list(new_m)[:2]))
    # 5. missing required asset
    miss_a = set(ref['required_assets']) - set(c['assets'])
    if miss_a:
        fails.append('missing_asset(%d):%s' % (len(miss_a), list(miss_a)[:2]))
    # 6. different/new asset
    new_a = set(c['assets']) - set(ref['union_assets'])
    if new_a:
        fails.append('new_asset(%d):%s' % (len(new_a), list(new_a)[:2]))
    # 7. new warning kind (divergence)
    new_w = set(c['warnings']) - set(ref['union_warnings'])
    if new_w:
        fails.append('new_warning(%d):%s' % (len(new_w), list(new_w)[:2]))
    # 8. crash/hang/wrong-state: must reach in-level + sustain pacing
    for k,v in ref['reached_required'].items():
        if v and not c['reached'].get(k):
            fails.append('not_reached:%s' % k)
    if c['pacing']['count'] < max(3, ref['pacing_count_min']*0.5):
        fails.append('pacing_stalled(%d<%.0f)' % (c['pacing']['count'], ref['pacing_count_min']*0.5))
    # pipelines: informational unless a required one vanished
    miss_p = set(ref['pipelines_required']) - set(c['pipelines'])
    if miss_p:
        notes.append('missing_pipelines=%d' % len(miss_p))
    new_p = set(c['pipelines']) - set(ref['pipelines_union'])
    if new_p:
        notes.append('new_pipelines=%d' % len(new_p))
    status = 'fail' if fails else 'pass'
    reason = (';'.join(fails) if fails else 'equivalent') + (' | '+';'.join(notes) if notes else '')
    return status, reason

def main():
    if len(sys.argv) < 2:
        print('usage: detdiff_fp.py {fp|buildref|gate} ...'); sys.exit(2)
    cmd = sys.argv[1]
    if cmd == 'fp':
        fp = extract(sys.argv[2])
        if fp is None: print('{}'); sys.exit(4)
        print(json.dumps(fp, indent=1))
    elif cmd == 'buildref':
        out = sys.argv[2]
        fps = [json.load(open(p)) for p in sys.argv[3:]]
        json.dump(buildref(fps), open(out,'w'), indent=1)
        print('buildref: %d baselines -> %s (%d required, %d union markers)'
              % (len(fps), out, len(buildref(fps)['required_markers']),
                 len(buildref(fps)['union_markers'])))
    elif cmd == 'gate':
        ref = json.load(open(sys.argv[2]))
        c   = json.load(open(sys.argv[3]))
        st, why = gate(ref, c)
        print('DETDIFF status=%s reason=%s' % (st, why))
        sys.exit(0 if st=='pass' else 1)
    else:
        print('unknown cmd', cmd); sys.exit(2)

if __name__ == '__main__':
    main()
