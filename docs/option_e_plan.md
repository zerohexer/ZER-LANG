# Option E / Effect-Row Composition — Execution Plan (replaces the old Phase-1-only checklist)

**Status:** Execution checklist. Replaces the prior "Level C Cleanup First"
checklist (which covered only Phase 1). Verified against the tree 2026-06-15.
**Purpose:** Make the asm-safety rework EXECUTABLE end-to-end by any session,
including a fresh-context one, without re-deriving scope. The design pivoted from the
old three-phase Option E (Layer 1/2/3 prose-declared bindings) to **Effect-Row
Composition** — the flexibility-weighted winner of the 14-angle adversarial fan-out.
This doc is the *operational* checklist.
**Source of truth for the architecture (the WHY):** `docs/asm_lang_zer_safe.md` —
read END-TO-END before touching asm safety; its decisions are LOCKED. Relevant
sections: §Leaves (Tier A vs Tier B classification), §Composition/ERBT (fold rules),
§Closure (theorem: safe leaves + sound fold ⇒ everything composed is safe), §Witness
(QEMU conformance), §Privileged (demand-driven bring-up), §Implementation-plan (STEP
0–7 — this doc mirrors it commit-by-commit). Cross-references below are to those.

---

## 0. The one classifying test (memorize before reading any step)

Every operation is sorted by ONE structural question (`asm_lang_zer_safe.md` §Leaves
4.1):

> *Is the operation INSIDE C's abstract machine (a value-computation), or does it
> CONFIGURE the machine (privilege, page tables, MSRs, I/O ports, the interrupt
> flag — concepts C has no model for)?*

- **Level 0** — plain ZER arithmetic (`+`, `&`, `<<`). NOT a leaf. Emits ordinary C,
  already fully safe (overflow wraps under `-fwrapv`, over-width shift = 0). No
  asm-safety machinery touches it.
- **Tier A leaf** — value-computation backed by a GCC builtin (`__builtin_add_overflow`,
  `__atomic_*`, `__builtin_clz/ctz/popcount`, `__builtin_bswap`). NO raw asm. Effect
  row DERIVED from the builtin's documented contract (one audited table). GCC selects
  the real instruction and the sub-arch under `-march`. ~95% of ops. Objective test:
  it compiles `-ffreestanding` (clz / add_overflow / bswap do; `__rdmsr` does not).
- **Tier B leaf** — machine-configuration with no portable builtin: CR0/3/4,
  RDMSR/WRMSR, XSETBV, IN/OUT ports, SYSCALL/SYSRET/IRET, CPUID/RDTSC/INVLPG/WBINVD,
  FSGSBASE. ~36 ops, per-ISA. The ONLY place a raw mnemonic appears. Effect row
  WITNESSED (decidable categories, x86, via QEMU) or DECLARED+TAINTED.
- **Composition** — `compose` operations that recombine leaves/ops. NO asm, NO mnemonic
  production in the grammar. Effect row DERIVED by closed FOLD rules over children.

**Two enforcement kinds, never conflated:**
- **COMPILE ERROR** = a structural gate the user cannot bypass without a witness or a
  legal construct. The whole plan maximizes these.
- **NAMED FLOOR** = a category that is author-DECLARED, carries a greppable taint
  marker, and can never read as verified-green. The plan concentrates these into the
  smallest, most centralized surface (the leaf set + fold rules).

---

## 1. The work order (8 steps — ascending effort, each independently shippable)

```
STEP 0  Phase 1 — Level C cleanup (DELETE per-arch tables). Prerequisite.   <-- old doc
STEP 1  S1  Tier-A derive-table in checker.c (builtin -> category bitset)
STEP 2      Wire ordering param through atomic value-ops (fix SEQ_CST hardcode)
STEP 3  S2  Composition + ERBT (NODE_COMPOSED_BIND / NODE_LEAF_BIND, fold, Layer-3 -> DERIVED)
STEP 4      Structural fence (@bind off-allow-list = error; no mnemonic in compose)
STEP 5  S3  Witness tool (tools/asm_witness/ under qemu-system-x86_64)
STEP 6  S4  Taint marker (named floor; propagate to importers)
STEP 7      DRC native CI differential (opt-in, NEVER a gate)
```

**The single most important sequencing fact (unchanged from the old doc):** STEP 0
deletes INFRASTRUCTURE (per-arch tables/scripts/categories/.zerdata), NOT the 130
D-Alpha intrinsics. The intrinsics stay; they ARE the shipped Tier-A and Tier-B
leaves already (the 15 `@atomic_*` + 8 bit/bswap are Tier A; D-Alpha-3/4/9..14
privileged batches are Tier B). STEP 0 is a precondition: the new mechanism must be
built on a core with no competing per-arch machinery to reconcile, and reviving the
per-ISA opcode table is exactly the REJECTED DCA/DSC disqualifier
(`asm_lang_zer_safe.md` §Design-exploration).

**Universal commit discipline (applies to EVERY commit in EVERY step):**
> Run `make docker-check` after EVERY commit. The regression net is
> `tests/test_asm_matrix.c` (the durable Z/S-rule oracle) + the `tests/zer/asm_*`
> and `tests/zer_fail/asm_*` cases. **Never push a red commit.**

The asm conformance harness must follow the matrix-oracle methodology of the 8
existing oracles (`tests/test_*_matrix.c` — `test_asm_matrix.c`, `test_hw_matrix.c`,
`test_escape_matrix.c`, `test_cflow_matrix.c`, `test_conc_matrix.c`,
`test_async_matrix.c`, `test_keep_matrix.c`, `test_shape_matrix.c`) — a POS/NEG matrix
with a `-Wswitch`-enforced scenario enum, NOT ad-hoc tests. `test_asm_matrix.c` already
declares itself the regression net for the Level C deletion (header lines 1–22).

The plan rides existing machinery rather than building new dispatch. Verified
2026-06-15: **64 `__builtin_`/`__atomic_` routing sites** in `emitter.c`
(`grep -cE "__builtin_|__atomic_" emitter.c` = 64) and **196 ISA-dispatch sites**
(`grep -cE "target_arch|__x86_64__|__aarch64__|__riscv" emitter.c` = 196). Tier A
reuses the first set; Tier B the second. No new code-emission backbone — the new work
is `checker.c` (derivation, fold, structural gate) plus one out-of-tree witness tool.

---

## STEP 0 — Phase 1: delete the per-arch tables (the prerequisite, UNCHANGED)

The Level C cleanup specified in the prior version of this doc, kept verbatim-in-spirit;
it must ship FIRST. Verified on disk 2026-06-08. Do the `checker.c` swap (Commit 1)
FIRST to validate the approach before any file deletion.

### KEEP (the durable ~600-line core — survives all 8 steps)
- 130 D-Alpha intrinsics (they become the shipped leaves under Effect-Row Composition).
- Z-rules Z1–Z8, Z11, Z12 — keep UAF/move/VRP/provenance/escape/qualifier/MMIO tracking
  ACTIVE through asm operand boundaries (unlike Rust, blind inside `unsafe { asm!() }`).
  Z9/Z10/Z13 never shipped, stay unshipped.
- Naked-only restriction S1; structured asm syntax (`asm { instructions/inputs/outputs/
  clobbers/safety }`, mandatory `safety:` string ≥ 30 chars); F7-light LR/SC pairing
  (hardcoded mnemonics, no table); the hardcoded ~12-entry UB-classics list.

### DELETE (the per-arch infrastructure)
| Group | Files |
|---|---|
| Instruction tables (53/37/30 rows) | `src/safety/asm_instruction_table.h`, `asm_instruction_table_{x86_64,aarch64,riscv64}.c` |
| Register tables | `src/safety/asm_register_tables.h`, `asm_register_tables_{x86_64,x86_64_avx512f,aarch64,riscv64}.c`, `asm_register_lookup.c` |
| Categories framework | `src/safety/asm_categories.{c,h}` |
| Probe scripts | `scripts/gen_instruction_table.sh`, `gen_register_tables.sh`, `candidates_{x86_64,aarch64,riscv64}.txt` |
| Vendored arch data | `arch_data/{x86_64,aarch64,riscv64}.zerdata`, `arch_data/SCHEMA.md` |
| Stray VST `.v`/`.o` | `asm_register_tables_x86_64.v`, `asm_register_tables_x86_64_avx512f.v`, `asm_register_lookup.v`, `asm_categories.v` + matching `.o` |

### Commit-by-commit (do C1 first)
- **Commit 1** — in the `checker.c` `NODE_ASM` handler, DELETE the three
  `zer_asm_register_valid_with_features(...)` register-validation loops and the
  `zer_asm_instruction_info(...)` F4/F7-full table dispatch + CPU-feature gating;
  ADD the frozen ~30-line UB-classics list (BSR/BSF/DIV → `REQUIRES_NONZERO`;
  IDIV → `REQUIRES_NONZERO | COMPOUND_INTMIN_NEG1`; MOVAPS/MOVAPD/MOVDQA →
  `REQUIRES_ALIGN_16`; VMOVAPS/VMOVAPD/VMOVDQA → `REQUIRES_ALIGN_32`). Each entry
  dispatches to EXISTING infra (`REQUIRES_NONZERO`→VRP, `REQUIRES_ALIGN_N`→alignment,
  `COMPOUND_INTMIN_NEG1`→signed-overflow). Register-name validation becomes GCC's job
  (emitted C → GCC assembler errors on a bad register). Files stay on disk this
  commit — only `checker.c` changes, proving the approach before any deletion.
- **Commit 2** — `git rm` instruction tables + probe scripts + `arch_data/`.
- **Commit 3** — `git rm` register tables + lookup + stray `.v`/`.o`.
- **Commit 4** — `git rm` `asm_categories.{c,h}` + `.v` + `.o`.
- **Commit 5** — Makefile + `check-vst` wiring: remove the 9 `src/safety/asm_*` files
  from `CORE_SRCS` AND `LIB_SRCS`, remove the `gen-asm-tables:` target, and **remove
  the deleted `asm_*` files from the `check-vst` `clightgen` line** (the one coupling
  that breaks `make check-vst` if missed).
- **Commit 6** — convert tests that referenced deleted infra:
  `tests/zer_fail/asm_aarch64_x86_reg.zer` and `asm_riscv64_x86_reg.zer` now compile
  through ZER and fail at GCC instead — relabel them "rejected by GCC assembler, not
  ZER", do NOT delete (the safety property still holds, at a different layer);
  `tests/zer/asm_avx512_register.zer` / `asm_simd_register.zer` still pass (GCC
  `-mavx512f`); `tests/zer_fail/asm_avx512_no_flag.zer` still fails.

**End state:** ~600 lines durable asm safety (Z1–Z8/Z11/Z12 + F7-light LR/SC +
UB-classics), **zero per-arch tables, zero probe scripts**. This is the disqualifier
removal — Effect-Row Composition must NOT revive these tables.

`make docker-check` after every commit; never push red.

---

## STEP 1 — S1: the Tier-A derive-table in `checker.c` NODE_ASM

**Files touched:** `checker.c` (NODE_ASM handler region). Verified 2026-06-15 the
handler dispatch is `case NODE_ASM:` at `checker.c:10529`, with the operand-walk
helper documented from `checker.c:2596` onward and the body running through the
Z6/Z8/Z11/Z7 wiring to ~`checker.c:10941` (the trailing Z7/MMIO comment ends there).
The architecture text cites the active span as ~10720–10890.

**What is added:** the closed `builtin name → category bitset` derive-table — the
single audited, one-time artifact from which every Tier-A leaf's effect row is
DERIVED, replacing any author-declared categories for the ~95% of ops GCC has a
builtin for. One row per builtin family (`asm_lang_zer_safe.md` §Leaves 4.2 derive-
table shape — illustrative, proposed surface, not yet shipped):

- `__builtin_add_overflow` / `sub_overflow` / `mul_overflow` →
  `value_in_value_out`, `produces_carry`, `clobbers_flags`, `no_memory_effect`.
- `__builtin_clz` / `ctz` → `value_in_value_out`, `no_memory_effect`, **`requires_nonzero`**
  (GCC documents these UB at input 0; on x86 BSF/BSR leave the dest untouched).
- `__builtin_popcount` / `parity` / `ffs` → `value_in_value_out`, `no_memory_effect`,
  **NO `requires_nonzero`** (defined at 0 by GCC).
- `__builtin_bswap{16,32,64}` → `value_in_value_out`, `no_memory_effect` (pure).
- `__atomic_load_n` / `store_n` / `fetch_*` / `compare_exchange_n` →
  `reads_mem(w,ord)` / `writes_mem(w,ord)`, `value_in_value_out`.

**Two soundness columns that MUST be derived from the contract, never the author:**
1. **`requires_nonzero` column** — encodes the `clz(0)`/`ctz(0)` UB split. ZER already
   guards this: `@ctz`/`@clz` emit a zero-test returning the type width on input 0 —
   verified at `emitter.c:8324-8331` (the `_zer_bz` temp + `== 0 ? width :
   __builtin_...`); `@popcount`/`@parity`/`@ffs` carry NO guard. The column must match
   this distinction. A wrong column = a re-introduced UB hole.
2. **lock-free-width guard** — 16-byte CAS (`__int128`) is NOT lock-free on baseline
   x86-64 (needs `cmpxchg16b` / `-mcx16`); GCC lowers it to a `libatomic` call that
   does not exist `-ffreestanding` and is not interrupt-safe. The table needs a width
   column; widths 1/2/4/8 derive normally; width 16 (and any width whose `__atomic_*_N`
   is not lock-free on target) is **rejected at the leaf**, not silently shipped as an
   external call. This extends the existing 1/2/4/8 validation + 32-bit libatomic
   warning (CLAUDE.md "Atomic width validation").

**What becomes a COMPILE ERROR in this step:**
- For any builtin-backed op, **author-supplied categories are FORBIDDEN** — the row is
  derived; a `@bind`/`compose` that re-declares a category for a builtin-backed op is
  an error (nothing to declare, so nothing can lie). Tested by `bind_on_builtin_op.zer`
  (STEP 4).
- A 16-byte (non-lock-free-width) atomic CAS is rejected (or tainted) at the leaf.

**The objective Tier-A boundary** is `-ffreestanding` compilability: only builtins on
the freestanding side belong in the derive-table; the rest are Tier B (STEP 5).

**Regression test to add:** extend the `test_asm_matrix.c` oracle (or a sibling
`test_leaf_matrix.c` following the same methodology) with POS cells (each builtin
family derives the expected bitset) and a NEG cell (16-byte CAS rejected; clz/ctz
require nonzero where VRP can't prove it). `make docker-check` after the commit;
never push red.

---

## STEP 2 — Wire the ordering param through atomic value-ops (fix the SEQ_CST hardcode)

"Category derived from the builtin's contract" is **FALSE today for the `ordering`
category** — the emitter hardcodes `__ATOMIC_SEQ_CST` for every atomic value-op.
Verified sites 2026-06-15:

- **AST path:** `emitter.c:3128` / `:3134` (load/store), `:3146` (CAS — the
  `, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })` form), `:3152` (fetch-*).
- **IR-rewritten path:** `emitter.c:8357` comment literally reads "All SEQ_CST
  ordering for now (Ordering param deferred)", with emissions at `:8364` / `:8368`
  (load/store), `:8375` (CAS), `:8399` (fetch-*).
- **The fence path is ALREADY ordering-parameterized — this is the model to COPY:**
  `emitter.c:3097-3101` — `@barrier` → `__ATOMIC_SEQ_CST`, `@barrier_store` →
  `__ATOMIC_RELEASE`, `@barrier_load` → `__ATOMIC_ACQUIRE` (verified 2026-06-15).

**Files touched:** `emitter.c` (both atomic value-op paths).

**Two acceptable outcomes — pick the cheaper that keeps the derive-table honest:**
- **(a)** Wire an `ordering` parameter through the atomic value-ops in BOTH paths
  (AST `3128–3152`, IR `8364–8399`), mirroring the fence-path precedent, then have the
  STEP 1 derive-table read the actual ordering.
- **(b)** Until (a) ships, the derive-table must **conservatively declare
  `ordering = seq_cst`** — the actually-emitted truth. Over-declaring to the strongest
  ordering is sound; under-declaring would be the unsound direction.

**Hard constraint that bounds this step's ambition — DO NOT re-attempt positional
ordering enforcement.** `memory_barrier` / positional ordering does NOT fold soundly
(STEP 3); ordering is the low-low cell of the observability spectrum (unsound fold ×
unobservable witness) — it stays a NAMED FLOOR (taint, STEP 6), never witnessed. ZER's
own evidence: Session-G Phase 3 in-block ordering enforcement was ABANDONED because it
false-positived the canonical multi-block CLWB+SFENCE libpmem idiom (verified
CLAUDE.md:1296).

**What becomes a COMPILE ERROR:** nothing new — this step is about the HONESTY of the
derived row, not a new gate. **Regression test:** if (a), add a matrix cell asserting
each ordering keyword reaches the emitted `__ATOMIC_*`; if (b), an assertion that the
derive-table declares `seq_cst` for value-ops. The IR-path SEQ_CST audit reminder
(CLAUDE.md "AST→IR Emission Diff Audit") applies — touch BOTH operator paths, test
with values VRP cannot prove. `make docker-check`; never push red.

---

## STEP 3 — S2: composition + ERBT (the flexible layer)

**Files touched:** `ast.h` (two new node kinds + data structs), `parser.c` (the
`compose` / `leaf` grammar — composition body has NO mnemonic production), `checker.c`
(the fold + the ERBT mismatch check + repoint Layer-3 to DERIVED). No `@bind` /
`@intrinsic_def` / allow-list exists in `checker.c` today (verified 2026-06-15 —
`grep -nE "@bind|NODE_COMPOSED_BIND|NODE_LEAF_BIND|allow.list" checker.c` returns
nothing); this step introduces them. Follow the FIVE-place AST-node protocol
(CLAUDE.md "Adding New AST Node Types": lexer, ast, parser, checker, emitter) AND
update every exhaustive `-Wswitch` walker for the new kinds.

**Two new node kinds:**
- `NODE_COMPOSED_BIND` — has **NO `instructions` field**. The composed-binding grammar
  has no mnemonic production: you literally cannot write freeform asm in a `compose`
  body. This is the `@inttoptr`-shaped gate for asm. A composed op recombines
  leaves/ops and other compositions only.
- `NODE_LEAF_BIND` — the ONLY node where a raw mnemonic is permitted, and only behind
  the structural fence of STEP 4.

**The closed FOLD RULES** (the composed op's effect row is DERIVED from its children's
rows — `asm_lang_zer_safe.md` §Composition / §Leaves 4.5):
- `clobbers_register` / `clobbers_flags` → fold by **UNION / OR** (sound: a superset of
  clobbers is always safe to assume). This is the witness sweet spot category.
- `requires_aligned(n)` → fold by **MAX** (sound: the strictest child alignment wins).
- `requires_nonzero` → **OR** (sound: any child that traps on zero makes the composite
  require nonzero).
- positional `memory_barrier` / ordering → **EXCLUDED from the fold vocabulary**
  (max-ordering-of-children is unsound — STEP 2 rationale; ordering stays
  value-intrinsic, lives on `reads_mem`/`writes_mem`/`memory_barrier` as a property of
  THAT op, never folded positionally).

**What becomes a COMPILE ERROR:** an author may state an EXPECTED effect row on a
`NODE_COMPOSED_BIND`; **declared ≠ derived = COMPILE ERROR**. This is the ERBT
(Effect-Row Binding Types) inference check — the checker folds the children, infers the
row, and rejects any author claim that disagrees.

**Repoint Layer-3 from DECLARED to DERIVED:** the static verifier that checks call
sites against effect rows must read the DERIVED row of the composed op, not an
author-declared one. Under the prior Option E, Layer 3 was "verified against a lie" if
a binding mis-declared; under composition a composed op cannot mis-declare (mismatch is
rejected), and a leaf's row is derived (Tier A), witnessed (decidable Tier B), or
tainted (STEP 6). This is the closure theorem made operational: the floor concentrates
into the leaf set, reducing safety from O(bindings) prose checklists to O(leaves) +
O(1) fold proof.

**Regression test to add:** matrix oracle cells — POS: a `compose` of two leaves with
overlapping clobbers derives the UNION; a compose with mixed alignments derives the
MAX; a correct author EXPECTED row is accepted. NEG: an author EXPECTED row that
disagrees with the derived fold is rejected (declared ≠ derived); a `compose` that
tries to declare a positional `memory_barrier` is rejected (excluded from fold
vocabulary). `make docker-check`; never push red.

---

## STEP 4 — Structural fence: `@bind` is a COMPILE ERROR unless on the closed privileged allow-list

**Files touched:** `checker.c` (the allow-list gate on `NODE_LEAF_BIND`), plus the two
negative tests below.

**The gate:** `@bind` (the leaf mechanism that introduces raw asm) is a **COMPILE
ERROR** unless its op is on a closed privileged allow-list — the ~36 Tier-B ops outside
C's abstract machine (CR0/3/4 read/write, RDMSR/WRMSR, XSETBV, IN/OUT ports,
SYSCALL/SYSRET/IRET, CPUID/RDTSC/INVLPG/WBINVD, FSGSBASE, etc.). This is the
**DEMAND-DRIVEN + FAIL-CLOSED** rule (`asm_lang_zer_safe.md` §Privileged): implement a
leaf only when firmware uses that op; an unimplemented privileged op is a compile error,
never a silent hole. The burden tracks YOUR usage (tiny, slow — CR3/MSR/port/IRET/IF
are decades-old, implemented once; FSGSBASE/SMAP/PKE/CET arrive slowly per-generation),
NOT the ISA catalog — so it does NOT trip the per-instruction/per-vendor proactive-
scaling disqualifier the REJECTED DCA/DSC designs hit (they needed the whole opcode
table up front or be fail-open).

**Two negative tests (both COMPILE ERRORS), in `tests/zer_fail/`:**
- `composed_bind_mnemonic.zer` — a `compose`/`NODE_COMPOSED_BIND` body that tries to
  contain a raw mnemonic. MUST error: the grammar has no mnemonic production at the
  composition level.
- `bind_on_builtin_op.zer` — a `@bind`/`NODE_LEAF_BIND` (or category re-declaration)
  on an op that is builtin-backed (Tier A, in the STEP 1 derive-table). MUST error:
  builtin-backed ops are derived, not bound; raw asm for them is forbidden (GCC already
  has the portable builtin).

These two tests, plus the STEP 3 declared-vs-derived test, are the structural core of
the asm conformance matrix — write them as matrix cells (the `-Wswitch` scenario enum
makes a forgotten case a compile error in the harness itself). `make docker-check`;
never push red.

---

## STEP 5 — S3: the conformance witness tool (`tools/asm_witness/`) under qemu-system-x86_64

**Files touched:** new out-of-tree directory `tools/asm_witness/` (the only out-of-tree
artifact in the plan); `checker.c` (the import/leaf-use gate that consumes
`.zerwitness`); Makefile (a `witness` target). **ENV verified 2026-06-15:**
`/usr/bin/qemu-system-x86_64` and `/usr/bin/qemu-system-i386` are present; there is NO
system QEMU for ARM/RISC-V, NO user-mode QEMU, NO cross-compilers. QEMU emulates
privilege, so privileged ops ARE probeable in system mode by booting a ring0 stub.

**What it does:** verifies a Tier-B leaf's DECLARED effect row against observed
behavior, per-category, for DECIDABLE categories only (observable + deterministic):
- `clobbers_register` / `clobbers_flags` — the **witness sweet spot**: fill all GPRs
  (and flags) with sentinels, run the leaf, read back. Register write-sets are
  structural, not data-dependent, so a single run is near-complete. Catches the classic
  under-declared-clobber bug that GCC and Rust CANNOT (they trust the author's clobber
  list).
- `changes_privilege` — CPL readback before/after.
- `requires_nonzero` — zero-trap probe.
- `requires_aligned` — only where it actually traps (NOT x86 with AC off — that
  silently succeeds, so it is UNOBSERVABLE on this host).

**UNOBSERVABLE categories are NOT witnessed** (witnessing them yields FALSE confidence,
worse than honest taint): memory ordering (QEMU TCG flattens to TSO, so a too-weak
fence witnesses GREEN), and x86 alignment with AC off. These stay TAINTED (STEP 6).

**Witness artifact:** a `.zerwitness` file binding `(op, ISA, category-profile,
blake2(asm-hash), qemu-version)`.

**What becomes a COMPILE ERROR:** a Layer-3 import / leaf-use of a binding recomputes
the asm hash; **missing witness or hash mismatch = COMPILE ERROR** — a structural,
FAIL-CLOSED precondition. Re-witnessing is lazy (only when the hash changes). Note the
demotion rationale (`asm_lang_zer_safe.md` §Witness): standalone QEMU-witness certifies
an EXISTENTIAL ("some categories observed on x86") that the gate consumes as a UNIVERSAL
("declaration honored") — sound ONLY for the decidable predicates above, which is why
the tool is subordinate and scoped to those.

**Regression test to add:** follow the 8-matrix-oracle methodology (`tests/test_*_matrix.c`)
for the conformance harness. POS: a correctly-declared RDMSR leaf witnesses green
(clobbers + changes_privilege observed). NEG: a deliberately under-declared-clobber leaf
(touches `rax`, omits it) FAILS the witness; a leaf with a stale/mismatched asm-hash
fails the import gate. `make docker-check`; never push red.

---

## STEP 6 — S4: the taint marker (the NAMED FLOOR)

**Files touched:** `checker.c` (the marker + importer propagation). **Reuse the existing
trust-boundary / audit-visible marker** rather than inventing a new mechanism — ZER
already has audit-visible-escape machinery (`@pun`-style). Propagate it to importers:
any function importing a tainted binding is itself tainted and **can never read as
verified-green**.

This is the honest residual, NOT a compile error — the user is permitted to proceed,
but the taint is auditable and propagates. Taint covers EXACTLY:
- **memory ordering** — the irreducible-hardest category (unsound fold × unobservable
  witness; the low-low cell).
- **non-x86 privileged asm on this host** — an ENV limit, not an architecture limit:
  installing `qemu-system-aarch64` + cross-gcc would promote those from taint to
  witness (opt-in).
- **`provenance_clear_on_output`** — neither derivable nor decidably observable.

Everything else resolves to DERIVED (Tier A / composition) or WITNESSED (decidable
Tier B).

**The split to keep straight (the whole point of the plan):**
- **COMPILE ERROR = the gate:** unwitnessed/hash-mismatch leaf; `@bind` off the
  allow-list; declared-vs-derived row mismatch; raw mnemonic in a composition; author
  categories on a builtin-backed op.
- **NAMED FLOOR = taint:** ordering, non-x86 privileged asm on this host,
  `provenance_clear_on_output`.

**Regression test to add:** a positive test that compiles (taint is not an error) but a
test harness assertion that the importer carries the greppable marker; a grep-based CI
check that no tainted symbol is reported as verified-green. `make docker-check`; never
push red.

---

## STEP 7 — Optional: DRC native CI differential (NEVER a gate)

**Files touched:** Makefile (`make drc-x86` target), a `tests/drc/` reference directory.
This is an opt-in differential reference check as a native-x86 CI sanity layer. DRC runs
each leaf against a pure-ZER reference implementation under fuzzed differential testing,
catching **wrong-VALUE** bugs that no category mechanism can detect (adc-drops-carry,
off-by-one bsr, CAS-wrong-operand).

**It is a TEST (process, not structure):** it can only WARN (not error) on cross-compile
because the reference and the leaf must both run on the same host. Keep it opt-in;
**NEVER make it the gate.** Making a probabilistic / host-bound test a structural
precondition would violate the closure model — a gate must be a compile-time structural
property, not a CI run. "Run a CI test" is precisely the SPARK/MISRA trust-the-user model
ZER REJECTS (CLAUDE.md "No contract-based verification"; `asm_lang_zer_safe.md`
§Design-exploration "REJECTED-as-gate / kept-as-CI: DRC"). Same for litmus/CWH
ordering probing — explicitly PROBABILISTIC, x86-is-TSO hides under/over-declaration —
which confirms ordering is the irreducible-hardest category, not a gate candidate.

`make docker-check` still green; never push red.

---

## Summary: what each step adds and gates

| Step | Adds | Enforcement |
|---|---|---|
| 0 | Delete per-arch tables (C1–C6) | — (removes the DCA/DSC disqualifier) |
| 1 | Tier-A derive-table in `checker.c` NODE_ASM (~`checker.c:10529`+); `requires_nonzero` col; lock-free-width guard | COMPILE ERROR: author categories on builtin-backed op; non-lock-free-width CAS |
| 2 | Wire `ordering` through atomics (`emitter.c:3128–3152`, `:8364–8399`) or declare `seq_cst` | — (honesty of the derived row; no positional ordering enforcement) |
| 3 | `NODE_COMPOSED_BIND` (no instructions), `NODE_LEAF_BIND`, OR/MAX fold, exclude positional barrier, Layer-3 → DERIVED | COMPILE ERROR: declared ≠ derived row |
| 4 | Structural fence + `composed_bind_mnemonic.zer`, `bind_on_builtin_op.zer` | COMPILE ERROR: `@bind` off allow-list; mnemonic in composition |
| 5 | `tools/asm_witness/` under qemu-system-x86_64; `.zerwitness` = (op, ISA, profile, blake2(asm), qemu-version) | COMPILE ERROR: missing/stale witness |
| 6 | Taint marker (reuse trust-boundary marker), propagate to importers | NAMED FLOOR: ordering, non-x86 privileged, provenance_clear_on_output |
| 7 | `make drc-x86` native differential | CI only — NEVER a gate |

**Effort ascends 0→7; each step is independently shippable and green.** STEP 0 is pure
deletion (already specified). STEPS 1–4 are `checker.c`-local (plus `emitter.c` for
STEP 2 ordering, `ast.h`/`parser.c` for STEP 3 nodes) and ride the 64 builtin + 196
ISA-dispatch sites. STEP 5 is the only out-of-tree artifact. STEPS 6–7 are markers and
an opt-in test.

---

## Deferred / out of scope (the honest residual floor)

Per `asm_lang_zer_safe.md` §Scope-floor, these are NOT closed by any step and are the
named floor:
1. **silicon errata / microarchitecture** — anchor moves from author's-word to
   GCC-contract (Tier A) + QEMU-TCG-model (Tier B): auditable/shared but neither is the
   die.
2. **QEMU/TCG fidelity** — the witness is only as good as the emulator.
3. **ordering + `provenance_clear_on_output`** — tainted (STEP 6).
4. **non-x86 privileged asm on this host** — ENV limit; installing
   `qemu-system-aarch64` + cross-gcc promotes taint → witness (opt-in).
5. **leaf assertions + fold-rules** — small, centralized, reviewable-without-silicon —
   the genuine shrink (O(bindings) distributed prose → O(leaves) + O(categories)
   centralized audit).

Record any deferred sub-task or discovered gap as a `## OPEN — …` entry in
`docs/limitations.md` in the SAME commit (CLAUDE.md "GAPS / LIMITATIONS / TECH DEBT").


---

# APPENDIX — original Phase-1-only checklist (PRESERVED; Step 0 above incorporates it)

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
