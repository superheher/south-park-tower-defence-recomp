import re
LOG='/tmp/xr2.log'; CAND='/tmp/sp_switch_validated.toml'; OUT='/tmp/sp_switch_clean.toml'
# labels that the recompiler flagged as out-of-function (any table)
outside=set()
for l in open(LOG):
    m=re.search(r'outside function: ([0-9A-Fa-f]+)', l)
    if m: outside.add(int(m.group(1),16))
print("distinct out-of-function target addresses:", len(outside))
# parse validated TOML
txt=open(CAND).read()
blocks=re.split(r'(?=\[\[switch\]\])', txt)
header=blocks[0]
clean=[]; broken=[]
for b in blocks[1:]:
    if '[[switch]]' not in b: continue
    labs=[]
    if 'labels = [' in b:
        labs=[int(x,16) for x in re.findall(r'0x[0-9A-Fa-f]+', b.split('labels = [')[1].split(']')[0])]
    base=re.search(r'base = (0x[0-9A-Fa-f]+)', b)
    if any(x in outside for x in labs): broken.append((base.group(1) if base else '?', len(labs)))
    else: clean.append(b)
print(f"tables: {len(clean)+len(broken)} | CLEAN (all targets in-bounds): {len(clean)} | boundary-limited: {len(broken)}")
print("boundary-limited bases:", [x[0] for x in broken][:40])
with open(OUT,'w') as f:
    f.write("# XenonAnalyse (South Park extensions), validated vs RexGlue, filtered to in-bounds tables.\n")
    f.write(f"# {len(clean)} tables whose every target lies within its function (cleanly emittable).\n")
    f.write(f"# {len(broken)} detected-but-boundary-limited tables omitted (see NIGHT-LOG: function-boundary problem).\n\n")
    for b in clean: f.write(b if b.endswith('\n') else b+'\n')
print("wrote", OUT)
