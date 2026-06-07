# Option E Execution Plan — Level C Cleanup First

**Status:** Execution checklist. Verified against the actual tree 2026-06-08.
**Purpose:** Make the asm-safety rework (toward Option E) executable by any
session, including a fresh-context one, without re-deriving scope.
**Source of truth for the architecture:** `docs/asm_lang_zer_safe.md` — Level C
deletion list (§9/§10/§11), Level D mechanism (§1.6), Option E layering (§1.7,
READ FIRST). This doc is the *operational* checklist; that doc is the *why*.

---

## 0. The work order (three phases — do NOT conflate)

```
PHASE 1  Level C cleanup       — DELETE per-arch infrastructure. Intrinsics STAY.   <-- this doc
PHASE 2  Level D mechanism     — add @intrinsic_def / @bind (asm_lang §1.6)         (later)
PHASE 3  Option E factoring    — Layer 1 schema/taxonomy; RE-LAYER the 130          (later)
                                 intrinsics: operation->Layer 1, x86 asm->Layer 2 lib
```

**The single most important sequencing fact:** Phase 1 does **NOT** touch the 130
D-Alpha intrinsics. Level C (§9.1) keeps them all. They are x86-heavy and under
Option E they become Layer 2 bindings — but that *re-layering* is Phase 3, after
the mechanism exists. Deleting/moving intrinsics in Phase 1 would strand users
with no replacement. **Phase 1 deletes INFRASTRUCTURE (tables/scripts/categories/
.zerdata), not intrinsics.**

---

## 1. What Phase 1 removes vs keeps (verified against the tree 2026-06-08)

### KEEP (the durable ~600-line core — survives all phases)
- 130 D-Alpha intrinsics (asm_lang §9.1)
- Z-rules Z1–Z8, Z11, Z12 (§9.2) — Z9/Z10/Z13 never shipped, stay unshipped
- Naked-only restriction S1 (§9.3)
- Structured asm syntax: `asm { instructions/inputs/outputs/clobbers/safety }` (§9.4–9.5)
- F7-light LR/SC pairing — hardcoded mnemonics, no table (§9.6)
- **NEW:** hardcoded ~12-entry UB-classics list (§9.7/§11) — replaces the F7-full table dispatch

### DELETE (the per-arch infrastructure — the "sub-extension / alpha stuff")
All present and confirmed on disk 2026-06-08:

| Group | Files |
|---|---|
| Instruction tables | `src/safety/asm_instruction_table.h`, `asm_instruction_table_{x86_64,aarch64,riscv64}.c` |
| Register tables | `src/safety/asm_register_tables.h`, `asm_register_tables_{x86_64,x86_64_avx512f,aarch64,riscv64}.c`, `asm_register_lookup.c` |
| Categories framework | `src/safety/asm_categories.{c,h}` |
| Probe scripts | `scripts/gen_instruction_table.sh`, `scripts/gen_register_tables.sh`, `scripts/candidates_{x86_64,aarch64,riscv64}.txt` |
| Vendored arch data | `arch_data/x86_64.zerdata`, `aarch64.zerdata`, `riscv64.zerdata`, `SCHEMA.md` |
| **Stray VST/obj** (NOT in asm_lang §10 — coupling caught 2026-06-08) | `src/safety/asm_register_tables_x86_64.v`, `asm_register_tables_x86_64_avx512f.v`, `asm_register_lookup.v`, `asm_categories.v` + all matching `.o` |

---

## 2. Commit-by-commit checklist (recommended order: C6 FIRST)

Run `make docker-check` after EVERY commit. The regression net is
`tests/test_asm_matrix.c` (durable Z/S-rules) + `tests/zer/asm_f7_*` (UB classics)
+ `tests/test_cross_arch.sh`. Never push a red commit.

### Commit 1 (was "C6") — checker.c: UB-classics swap + drop table calls  ← DO THIS FIRST
The trickiest piece; validating it first proves the approach before any file
deletion. The files stay on disk this commit (still linked) — only checker.c
changes.

1. In the `NODE_ASM` handler (currently ~checker.c:10720–10890), DELETE:
   - the 3 `zer_asm_register_valid_with_features(...)` register-validation loops
     (inputs/outputs/clobbers, ~10744/10757/10775)
   - the `zer_asm_instruction_info(...)` F4/F7-full table dispatch (~10874) and
     the CPU-feature-gating around it
2. ADD the hardcoded UB-classics list (asm_lang §9.7 / §11) — ~30 lines, frozen:
   ```
   { "bsr",0,REQUIRES_NONZERO,"Intel SDM BSR" }, { "bsf",0,REQUIRES_NONZERO,... },
   { "div",0,REQUIRES_NONZERO,... }, { "idiv",0,REQUIRES_NONZERO|COMPOUND_INTMIN_NEG1,... },
   { "movaps",0,REQUIRES_ALIGN_16,... }, { "movapd",... }, { "movdqa",... },
   { "vmovaps",0,REQUIRES_ALIGN_32,... }, { "vmovapd",... }, { "vmovdqa",... },
   ```
   Each entry dispatches to EXISTING infra: `REQUIRES_NONZERO`→VRP (System #12),
   `REQUIRES_ALIGN_N`→existing alignment, `COMPOUND_INTMIN_NEG1`→signed-overflow.
3. Keep the includes (lines 17–18) for now — remove them in Commit 3/4 when the
   files go. (Or remove now and stub; cleaner to remove with the files.)
4. Register name validation is now GCC's job (emitted C → GCC assembler errors on
   bad register). Document in the commit message.
5. Verify: `test_asm_matrix` green; `asm_f7_*` (BSR/IDIV/MOVAPS pos+neg) green;
   `asm_simd_register`/`asm_avx512_register` now validated by GCC `-mavx512f`.

### Commit 2 — delete instruction tables + probe scripts + arch_data
- `git rm` the 3 `asm_instruction_table_*.c` + `.h`, `gen_instruction_table.sh`,
  `candidates_*.txt`, `arch_data/*.zerdata`, `arch_data/SCHEMA.md`.
- Remove the `asm_instruction_table.h` include from checker.c (if not done in C1).

### Commit 3 — delete register tables + lookup (+ AVX-512 + .v)
- `git rm` the 4 `asm_register_tables_*.c`, `asm_register_lookup.c`,
  `asm_register_tables.h`, `gen_register_tables.sh`.
- `git rm` the stray `.v` (`asm_register_tables_x86_64.v`,
  `asm_register_tables_x86_64_avx512f.v`, `asm_register_lookup.v`) + `.o`.
- Remove the `asm_register_tables.h` include from checker.c.

### Commit 4 — delete categories framework
- `git rm` `asm_categories.{c,h}` + `asm_categories.v` + `.o`.

### Commit 5 — Makefile + check-vst wiring
- Remove the 9 `src/safety/asm_*` files from `CORE_SRCS` AND `LIB_SRCS` (lines 8, 15).
- Remove the `gen-asm-tables:` target (~line 237) and its body.
- **Remove the deleted `asm_*` files from the `check-vst` `clightgen` line (~line 382)**
  — this coupling is the one asm_lang §10.7 only half-mentions. Without it,
  `make check-vst` breaks looking for deleted `.c`.

### Commit 6 — convert tests that referenced deleted infrastructure (§10.9)
- `tests/zer_fail/asm_aarch64_x86_reg.zer`, `asm_riscv64_x86_reg.zer`: these
  previously failed at ZER (register table). They now compile through ZER and
  fail at GCC. **Convert** — either move to a GCC-reject harness or relabel as
  "rejected by GCC assembler, not ZER". Do NOT just delete (the safety property
  still holds, just at a different layer).
- `tests/zer/asm_avx512_register.zer`, `asm_simd_register.zer`: still pass
  (GCC validates with `-mavx512f`). Verify.
- `tests/zer_fail/asm_avx512_no_flag.zer`: still fails (GCC errors without flag).
- `tests/test_cross_arch.sh`: drop any dependency on the probe scripts.

### Commit 7 (optional, docs) — prune asm_plan.md stale planning (§10.8)
- DELETE: "Session G Phase 5 implementation plan", "Re-audit round 2–6".
- KEEP as historical (do not grow): 8-category details, F4-F7 trajectory, Stage 4
  snapshot.
- ~3,000 lines of deferred-work planning removable. Low priority; pure docs.

---

## 3. End state after Phase 1

ZER asm safety infrastructure ≈ **600 lines**: Z-rules (~500, generic/frozen) +
F7-light LR/SC (~50, hardcoded mnemonics) + UB classics (~30, frozen) + Session
A/B parser/AST. **Zero per-arch tables, zero probe scripts, zero per-ISA-extension
maintenance.** Register/instruction/feature validation delegated to GCC. The 130
intrinsics and all 10 wired Z-rules unchanged.

This is also the cleanest base for Phase 2/3: the `@intrinsic_def`/`@bind`
mechanism and the Layer 1 schema get built on a core with no competing per-arch
machinery to reconcile.

---

## 4. Regression net (what proves each commit safe)

- `tests/test_asm_matrix.c` (built 2026-06-08) — the DURABLE surface: S1 naked-only,
  S2/S3/S4, empty-insn, Z8 const-output, Z11 non-keep-ptr+mem-clobber. If a deletion
  drops one of these, a NEG cell flips to false-negative.
- `tests/zer/asm_f7_*` — UB-classics behavior (BSR-on-zero, IDIV, MOVAPS-misaligned)
  the hardcoded list must reproduce.
- `tests/test_cross_arch.sh` — cross-arch compile.
- Full `make docker-check` after each commit.

Known oracle gap (acceptable): `test_asm_matrix` does NOT cover the UB-classics
themselves (the `asm_f7_*` tests do). If desired, add UB-classics cells to the
oracle after Commit 1 so the regression net is single-file.

---

## 5. Deferred to later planning rounds (NOT Phase 1)

- **Phase 2 (Level D):** `@intrinsic_def` / `@bind` mechanism — asm_lang §1.6.
  Demand/promise asymmetry rule (§1.6.17), missing-arch propagation (§1.6.18).
- **Phase 3 (Option E):** Layer 1 closed category vocabulary + operation taxonomy
  (§1.7.3), verifier-dispatch-over-categories, then RE-LAYER the 130 intrinsics
  (operation decl → Layer 1; x86 asm body → a `zer-asm-x86` Layer 2 library). The
  cross-ISA semantic intrinsics (atomics, bit ops, barriers) map to Layer 1
  taxonomy; the x86-privileged ones (@cpu_read_msr, @port_*, @cpu_iret, CR/MSR/
  XCR0) become Layer 2 x86 bindings. **This is where "intrinsics we don't want in
  core anymore" actually get moved — Phase 3, with a replacement in place, not now.**
- The hard Phase 3 work is the Layer 1 SCHEMA (asm_lang §1.7.7 "This is the work"):
  rich enough for flag-bearing vs flagless, weak vs TSO, vector, CHERI — without
  free-form predicates.
