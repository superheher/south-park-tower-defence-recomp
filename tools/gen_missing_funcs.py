#!/usr/bin/env python3
"""Generate config/sp_functions.toml — functions rexglue's analyzer misses.

rexglue discovers functions from .pdata, direct-call targets, and a built-in
VTableScanner, but it still misses two classes that get reached via a
runtime-computed `ctr` (bctr/bctrl) and therefore fault at runtime with
"Call to invalid or unregistered function at 0x...":

  1. vtable methods in vtables its VTableScanner doesn't recognise, and
  2. computed-jump / adjustor-thunk targets that live *inside* a larger
     .pdata function (mid-function entries; not static .data pointers).

rexglue's RecompilerConfig has a `[functions]` table (addr -> {size,end,name,
parent}); entries are registered as CONFIG entry points and their extent is
discovered automatically. We layer this file into the entrypoint via the
manifest's [entrypoint].includes, so it survives `rexglue -f codegen`.

Class (1) is found here by scanning the decrypted image for vtables = runs of
>=3 consecutive 4-aligned pointers into the code range, minus already-registered
functions (very low false-positive rate). Class (2) cannot be found statically
(computed at runtime); those are listed in KNOWN_COMPUTED as the boot reveals
them. Re-run after each codegen to refresh against the current registered set.

Usage: python tools/gen_missing_funcs.py [image.bin]
  image.bin defaults to a freshly decrypted dump (see tools/xex_decrypt.py).
"""
import struct, glob, re, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = sys.argv[1] if len(sys.argv) > 1 else r"C:\Temp\spdec.bin"
GEN = os.path.join(ROOT, "generated", "default")
OUT = os.path.join(ROOT, "config", "sp_functions.toml")
CODE_LO, CODE_HI = 0x82100000, 0x825F0C18

# Class (2): mid-function computed-jump / adjustor-thunk targets that faulted at
# runtime. Not statically discoverable; append new ones as the boot reveals them.
KNOWN_COMPUTED = [
    0x822E38E0,  # adjustor thunk: addi r3,r3,8; b 0x822E2298 (also sub_822E38E8 branch target)
    0x82250288,  # virtual-dispatch tail inside sub_822501C8
    0x82247E20,  # tiny getter (li r3,50; blr) inside sub_82247D58; static-ptr but belt-and-suspenders
    # Cross-function `b`/`goto` targets rexglue can't resolve to a function entry, so
    # fix_recomp_labels traps them as REX_FATAL. Registering each as a CONFIG function makes
    # the branch a tail-call (runs that tail). Harvested from the generated REX_FATAL traps
    # 2026-05-24 (after the setjmp/longjmp image-EH fix let the boot reach the menu path).
    0x8235278C,  # "Unresolved branch from 0x82352808" target (post-SEH, intro-movie path)
    0x821DA42C, 0x821DA438, 0x821F23EC, 0x822F7598, 0x8231EDF8, 0x823337FC, 0x82333800,
    0x823433F0, 0x82366B08, 0x8242DB70, 0x824313F8, 0x8243254C, 0x8243FFBC, 0x82442E4C,
    0x82445B94, 0x82445BB0, 0x824712D0,
]


def registered_addrs():
    # Include BOTH sub_ (emitted functions) AND __imp__ (import thunks). Import-thunk
    # addresses appear as code pointers in vtables but are resolved by rexglue's import
    # mechanism, NOT as sub_ bodies — registering them as CONFIG functions yields
    # "undefined symbol: sub_8259xxxx" at link. Treat them as already-registered so the
    # vtable scan excludes them (rexglue's own VTableScanner skips isInImportExportRange).
    reg = set()
    for p in glob.glob(os.path.join(GEN, "*_init.cpp")):
        with open(p, encoding="utf-8", errors="replace") as fh:
            for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+),\s*(?:sub_|__imp__)", fh.read()):
                reg.add(int(m.group(1), 16))
    return reg


def vtable_entries(d):
    """Entries of vtable-like runs (>=3 consecutive 4-aligned code pointers)."""
    n = len(d) & ~3
    out = set()
    i = 0
    while i + 4 <= n:
        v = struct.unpack_from(">I", d, i)[0]
        if CODE_LO <= v < CODE_HI and (v & 3) == 0:
            run = []
            j = i
            while j + 4 <= n:
                w = struct.unpack_from(">I", d, j)[0]
                if CODE_LO <= w < CODE_HI and (w & 3) == 0:
                    run.append(w)
                    j += 4
                else:
                    break
            if len(run) >= 3:
                out.update(run)
            i = j
        else:
            i += 4
    return out


def main():
    d = open(IMG, "rb").read()
    reg = registered_addrs()
    vt = vtable_entries(d)
    # Preserve entries already in the config: `reg` is read from the GENERATED code, which
    # includes functions emitted *because* a prior config listed them, so (vt - reg) would
    # silently DROP them on a re-run (705 -> 25 bug). Union the existing config so this
    # script is cumulative + idempotent — the config only grows; extra registrations are
    # harmless ("runs that tail and returns").
    prior = set()
    if os.path.exists(OUT):
        for line in open(OUT, encoding="utf-8"):
            s = line.strip()
            if s.startswith('"0x'):
                try:
                    prior.add(int(s.split('"')[1], 16))
                except (ValueError, IndexError):
                    pass
    # KNOWN_COMPUTED are always emitted: rexglue's base codegen never produces them,
    # and any current registration is only from a hand-patch that a regen will wipe.
    missing = sorted(set(KNOWN_COMPUTED) | prior | (vt - reg))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# AUTO-GENERATED by tools/gen_missing_funcs.py — do not hand-edit.\n")
        fh.write("# Functions rexglue's analyzer misses (vtable methods its VTableScanner\n")
        fh.write("# skips + mid-function computed-jump/adjustor-thunk targets). Layered into\n")
        fh.write("# the entrypoint via [entrypoint].includes. Empty table => size discovered.\n")
        fh.write("[functions]\n")
        for a in missing:
            tag = "  # known computed-jump target" if a in KNOWN_COMPUTED else ""
            fh.write('"0x%08X" = {}%s\n' % (a, tag))
    print("registered:        %d" % len(reg))
    print("vtable-run entries: %d" % len(vt))
    print("KNOWN_COMPUTED:     %d" % len(KNOWN_COMPUTED))
    print("written:           %d functions -> %s" % (len(missing), OUT))


if __name__ == "__main__":
    main()
