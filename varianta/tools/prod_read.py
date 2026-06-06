#!/usr/bin/env python3
# prod-oracle helper: read guest globals from a running prod (south_park_td) process.
# Determines the guest->host membase empirically from /proc/PID/maps, then reads
# big-endian guest dwords via /proc/PID/mem.
import sys, struct, re

def find_pid():
    import subprocess
    out = subprocess.run(["pgrep","-x","south_park_td"], capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else None

def read_maps(pid):
    maps=[]
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            m=re.match(r'([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+\s*(.*)', line)
            if m:
                s=int(m.group(1),16); e=int(m.group(2),16)
                maps.append((s,e,m.group(3),int(m.group(4),16),m.group(5).strip()))
    return maps

def covering(maps, host):
    for s,e,perm,off,path in maps:
        if s<=host<e: return (s,e,perm,off,path)
    return None

def read_be32(pid, host):
    with open(f"/proc/{pid}/mem","rb",0) as f:
        f.seek(host)
        b=f.read(4)
    if len(b)<4: return None
    return struct.unpack(">I", b)[0]   # big-endian guest dword

def main():
    pid=find_pid()
    if not pid:
        print("NO prod process (pgrep -x south_park_td empty)"); return 2
    print(f"prod pid={pid}")
    maps=read_maps(pid)
    # guest globals of interest (the advance-gate's gate-clearing registrations)
    guests={
        "subsystem_mgr 0x827FD568": 0x827FD568,
        "subsystem_ptr 0x827FD56C": 0x827FD56C,
        "menu_handler  0x828183A0": 0x828183A0,
    }
    for base in (0x100000000, 0x100010000):
        host=0x82000000+base
        cov=covering(maps, host+0x7FD56C)  # check guest 0x827FD56C
        print(f"membase candidate {base:#x}: guest 0x827FD56C -> host {host+0x7FD56C:#x} -> {'MAPPED '+cov[2]+' '+cov[4] if cov else 'UNMAPPED'}")
    # pick the candidate that maps 0x827FD56C
    chosen=None
    for base in (0x100000000, 0x100010000):
        if covering(maps, base+0x827FD56C): chosen=base; break
    if chosen is None:
        print("Neither membase candidate maps 0x827FD56C — dumping mappings near 0x182xxxxxxx:")
        for s,e,perm,off,path in maps:
            if 0x180000000<=s<0x190000000:
                print(f"  {s:#x}-{e:#x} {perm} off={off:#x} {path}")
        return 3
    print(f"\n=> chosen membase = {chosen:#x}\n")
    for name,g in guests.items():
        host=chosen+g
        cov=covering(maps, host)
        v=read_be32(pid, host) if cov else None
        vs=f"{v:#010x}" if v is not None else "READ-FAIL"
        print(f"  {name}: host={host:#x} {'('+cov[2]+')' if cov else '(UNMAPPED)'} value={vs}")
    return 0

sys.exit(main())
