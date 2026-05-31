import re, glob
GEN='/home/h/src/recomp/rexglue-recomps/south-park-recomp/generated/default'
CAND='/tmp/sp_switch_candidate.toml'
OUT='/tmp/sp_switch_validated.toml'
files=glob.glob(GEN+'/south_park_td_recomp.*.cpp')

swre=re.compile(r'switch \(ctx\.r(\d+)\.u32\) \{')
casere=re.compile(r'^\s*case (\d+):')
gotore=re.compile(r'goto loc_([0-9A-Fa-f]+);')
locdef=re.compile(r'^loc_([0-9A-Fa-f]+):')

rexset={}    # (r, tuple(labels)) -> count
seen=0
locs=set()   # all loc_ label DEFINITIONS (valid jump targets)
for fn in files:
    lines=open(fn).read().splitlines()
    for l in lines:
        m=locdef.match(l)
        if m: locs.add(int(m.group(1),16))
    i=0
    while i<len(lines):
        m=swre.search(lines[i])
        if m:
            seen+=1; r=int(m.group(1)); labels={}; j=i+1
            while j<len(lines) and not re.match(r'^\t\}',lines[j]):
                cm=casere.match(lines[j])
                if cm:
                    k=int(cm.group(1)); g=None
                    for t in range(j+1,min(j+4,len(lines))):
                        gm=gotore.search(lines[t])
                        if gm: g=int(gm.group(1),16); break
                        if casere.match(lines[t]): break
                    if g is not None: labels[k]=g
                j+=1
            if labels:
                mk=max(labels); lab=tuple(labels.get(x,0) for x in range(mk+1))
                rexset[(r,lab)]=rexset.get((r,lab),0)+1
            i=j; continue
        i+=1
print(f"RexGlue: {seen} switch stmts seen, {sum(rexset.values())} parsed w/ labels, {len(locs)} loc targets")

# candidates
txt=open(CAND).read()
blocks=re.split(r'(?=\[\[switch\]\])', txt)
cands=[]
for b in blocks[1:]:
    bm=re.search(r'base = (0x[0-9A-Fa-f]+)', b); rm=re.search(r'r = (\d+)', b)
    labs=[]
    if 'labels = [' in b:
        labs=[int(x,16) for x in re.findall(r'0x[0-9A-Fa-f]+', b.split('labels = [')[1].split(']')[0])]
    if bm and rm: cands.append((int(bm.group(1),16),int(rm.group(1)),tuple(labs),b))

strong=[]; plausible=[]; reject=[]; used={}
for base,r,lab,b in cands:
    key=(r,lab)
    if rexset.get(key,0)-used.get(key,0)>0:
        used[key]=used.get(key,0)+1; strong.append((base,r,lab,b))
    elif lab and all(x in locs for x in lab):
        plausible.append((base,r,lab,b))
    else:
        reject.append((base,r,lab))
print(f"candidates: {len(cands)} | STRONG(exact RexGlue match): {len(strong)} | PLAUSIBLE(all labels are loc targets): {len(plausible)} | REJECT: {len(reject)}")
for base,r,lab in reject:
    bad=[hex(x) for x in lab if x not in locs][:3]
    print(f"  REJECT base=0x{base:X} r={r} n={len(lab)} non-loc-labels={bad}")

with open(OUT,'w') as f:
    f.write("# XenonAnalyse (South Park extensions), validated vs RexGlue.\n")
    f.write(f"# {len(strong)} exact-match + {len(plausible)} all-labels-are-RexGlue-jump-targets. {len(reject)} rejected.\n\n")
    for base,r,lab,b in strong+plausible:
        f.write(b if b.endswith('\n') else b+'\n')
print(f"wrote {OUT}: {len(strong)+len(plausible)} tables")
