# Variant A — jump-table recovery pipeline

XenonAnalyse (tuned for Sonic Unleashed) detected **0** jump tables in South Park, because SP's
MSVC-360 compiler (a) inserts alignment `nop`s mid-prologue and (b) emits `rlwinm` *before* the
table-base `addi` (Unleashed emits them in the opposite order). That defeats XenonAnalyse's
fixed-opcode-sequence `SearchMask` **and** `ReadTable`'s hardcoded operand offsets.

## Fix (in `varianta/patches/xenonrecomp-sp-instructions.patch`, `XenonAnalyse/main.cpp`)
1. **NOP-tolerant `SearchMask`** — skips interior `nop`s while matching a prologue pattern.
2. **Role-based `ReadTableSP`** — instead of fixed `code+N` offsets, finds `lis`/`addi`/`lbzx|lhzx|lwzx`/
   `rlwinm` by *opcode role*, derives the table TYPE from the load op, then reuses XenonAnalyse's
   per-type address arithmetic. Order- and NOP-independent.
3. **Two SP-ordered patterns** — `spAbsoluteSwitch`, `spShortOffsetSwitch` (the reordered forms).
   The computed / byte-offset SP schedules share Unleashed's opcode order, so the original patterns
   cover them once NOPs are skipped.

→ XenonAnalyse now detects **102** tables.

## Validation (safety net — only correct tables ship)
- `sw_patterns.py` — diagnostic: histograms the distinct jump-table prologue schedules in RexGlue's
  `generated/default/*` (used to discover the NOP/reorder root cause; 15 schedules → 4 table types).
- `validate2.py` — cross-checks each XenonAnalyse candidate against RexGlue's resolved switches:
  STRONG = exact `(r, labels)` match; PLAUSIBLE = every label is a real `loc_XXXX:` jump target in
  RexGlue's output (a misread base would shift labels off all ~98.7k known targets). **0 rejects.**
- `filter_clean.py` — from a recompile log, drops tables whose targets fall outside their function
  boundary (the README "Function Boundary Analysis" limitation). 102 → **93 in-bounds** + 9 deferred.

## Pipeline
```
# 1. (one-time) build the extended XenonAnalyse (patch already applied to the clone)
cmake --build ../../third_party/XenonRecomp/out/build --target XenonAnalyse
# 2. detect candidates
.../XenonAnalyse ../private/extracted/default.xex /tmp/sp_switch_candidate.toml
# 3. validate vs RexGlue  -> /tmp/sp_switch_validated.toml
python3 tools/jumptables/validate2.py
# 4. recompile once, then filter to in-bounds -> /tmp/sp_switch_clean.toml -> sp_switch_tables.toml
python3 tools/jumptables/filter_clean.py
```

## Deferred (function-boundary problem — needs manual function boundaries / analyzer extension)
- 9 boundary-limited jump tables (targets are separate functions, so `goto` can't reach them).
- 161 `// ERROR <addr>` conditional-branch markers (same root cause; present with or without tables).
Both are the XenonRecomp README's acknowledged "no current auto-solution" function-boundary issue.
