import re, glob, collections
files = glob.glob('/home/h/src/recomp/rexglue-recomps/south-park-recomp/generated/default/south_park_td_recomp.*.cpp')
mn = re.compile(r'^\t// ([a-z][a-z0-9._]+)')
sw = re.compile(r'^\tswitch \(ctx\.r(\d+)\.u32\) \{')
total=0; pats=collections.Counter(); regs=collections.Counter()
for fn in files:
    lines=open(fn).read().splitlines()
    # collect mnemonic per line index (None if not a mnemonic comment)
    for i,l in enumerate(lines):
        m=sw.match(l)
        if not m: continue
        total+=1; regs[int(m.group(1))]+=1
        # walk backwards collecting the last 8 mnemonic-comment lines before this switch
        seq=[]
        j=i-1
        while j>=0 and len(seq)<9:
            mm=mn.match(lines[j])
            if mm: seq.append(mm.group(1))
            j-=1
            if i-j>40: break
        seq=list(reversed(seq))
        pats[' '.join(seq)]+=1
print("total switches:", total)
print("distinct prologue schedules:", len(pats))
print("=== top prologue schedules (count : opcode sequence ending at bctr) ===")
for s,c in pats.most_common(12):
    print(f"{c:4d}  {s}")
print("=== switch-register histogram ===", dict(regs.most_common()))
