# ZER-Asm Safety — Effect-Row Composition (READ FIRST, supersedes Option E where they conflict)

**Status:** Architectural refinement. Direction LOCKED. Implementation PENDING. This document
refines `docs/asm_lang_zer_safe.md` §1.7 (Option E, locked 2026-06-06); where this design and
Option E conflict, this design supersedes. Nothing here is shipped yet. The step-by-step
execution order lives in `docs/option_e_plan.md` (Phase 1 = table deletion) and in §"IMPLEMENTATION
PLAN" of the longer companion sections of this document. Treat every `@bind` / `compose` / `leaf`
surface syntax below as **(illustrative — proposed surface, not yet shipped)**.

---

## Thesis (one paragraph)

Asm safety in ZER reduces to: a small, audited set of **leaves** (the only place raw-asm
primitives appear, the only gated unit), plus a **closed set of fold rules** that derive a
composed operation's safety properties from its children. Authors recombine leaves into new
operations (`compose`) with NO assembly and NO compiler release; the composed-binding grammar
literally has no mnemonic production, so freeform asm cannot appear in a composition — the same
shape of structural gate ZER already uses for `@inttoptr` (no typed pointer except through that
witness). Safety categories are modeled as composable **effect rows** (ERBT — Effect-Row Binding
Types); the checker folds and infers them. The core theorem: **safe leaves + sound fold ⇒
everything composed above the leaf line is safe.** Composition cannot manufacture a violation,
but it also cannot fix a lying leaf — safety is *relative to* leaf correctness, so the floor
concentrates into the finite leaf set. This collapses asm safety from Option E's O(bindings)
distributed-prose checklists to O(leaves) + an O(1) fold proof — the same closure shape ZER
already uses for memory safety (closure over allocation primitives), type safety (over conversion
intrinsics), and concurrency (over sync primitives).

---

## The whole design in 12 bullets

1. **LEVEL 0** — plain ZER arithmetic (`+`, `&`, `<<`) emits C, GCC compiles it. NOT a leaf and
   not asm; already fully safe (overflow wraps, shift-by-≥width = 0). The lowest primitive everything
   else sits above.
2. **TIER A LEAF** — a GCC builtin (`__builtin_add_overflow`, `__atomic_*`, `__builtin_clz/ctz/popcount`,
   `__builtin_bswap`). NO raw asm. The effect row is DERIVED from the builtin's documented contract
   (one audited table, one time). GCC selects the real instruction and handles sub-architecture via
   `-march`. Structural, portable, zero-per-ISA. Objective Tier-A boundary: **it compiles under
   `-ffreestanding`** (verified: clz / add_overflow / bswap do; `__rdmsr` does not). ~95% of ops by
   count (computational / ALU / atomic / bit).
3. **TIER B LEAF** — raw asm (real mnemonic), the ONLY place raw asm appears. ONLY for ops *outside*
   C's abstract machine for which GCC has no portable builtin: control-state writes (CR0/3/4, RDMSR/
   WRMSR, XSETBV), I/O ports (IN/OUT), privilege transitions (SYSCALL/SYSRET, IRET, ECALL, SVC),
   inspection/cache/TLB (CPUID, RDTSC, INVLPG, WBINVD). ~36 ops, per-ISA. These CONFIGURE the machine
   (privilege, page tables, MSRs, ports, interrupt flag — concepts C has no model for) so no portable
   builtin is possible. Effect row is WITNESSED (where decidable) or DECLARED + TAINTED.
4. **The one test** that splits Tier A from Tier B: *is the operation a value-computation inside C's
   abstract machine, or does it CONFIGURE the machine?* Value-computation → builtin (Tier A);
   machine-configuration → raw-asm leaf (Tier B).
5. **LEAF** — the audited primitive unit. Small and per-ISA. The only construct that may contain
   Tier-B raw asm and the only construct the gate fences. Everything else is composition.
6. **COMPOSITION (the flexible layer)** — authors write `compose` operations that recombine leaves and
   ops. No asm, no core release. The composed-binding grammar has **no mnemonic production** (the
   `@inttoptr`-shaped structural gate). The composed op's effect row is DERIVED by closed FOLD RULES
   over its children. An author MAY state an EXPECTED row; declared-vs-derived mismatch = COMPILE ERROR.
7. **ERBT (Effect-Row Binding Types)** — Option E's categories re-expressed as composable ROWS that the
   checker folds and infers. The type-theoretic backbone of composition; one-time maintenance;
   structural compile-gate.
8. **FOLD SOUNDNESS** — `clobbers_register` / `clobbers_flags` fold by UNION/OR (sound);
   `requires_aligned(n)` folds by MAX (sound). Ordering / `memory_barrier` does NOT fold soundly:
   max-ordering-of-children is UNSOUND. ZER already proved this — Phase 3 in-block ordering enforcement
   was ABANDONED because it false-positived the canonical libpmem CLWB+SFENCE idiom (CLAUDE.md:1295-1296).
   So **positional `memory_barrier` is EXCLUDED from the fold vocabulary; ordering stays value-intrinsic
   only.**
9. **CLOSURE ARGUMENT (the core theorem)** — safe leaves + sound fold ⇒ everything composed above the
   leaf line is safe. Composition safety ALONE is not sufficient: a sound fold faithfully *propagates*
   a lying leaf. Safety is relative to leaf correctness; the floor concentrates into the leaf set.
10. **CONFORMANCE WITNESS** — the toolchain runs per-category probes in QEMU to verify a Tier-B leaf's
    DECLARED row against observed behavior; the witness is bound to `(op, ISA, category-profile,
    asm-hash)`. A Layer-3 import / leaf-use of an unwitnessed or hash-mismatched binding = COMPILE ERROR
    (fail-closed). DECIDABLE categories: `clobbers_register`/`clobbers_flags` (sentinel-fill all regs,
    run, read back — the witness sweet spot, catches the classic under-declared-clobber bug GCC/Rust
    cannot), `changes_privilege` (CPL readback), `requires_nonzero` (zero-trap), `requires_aligned`
    where it actually traps. UNOBSERVABLE: memory ordering (QEMU TCG flattens to TSO — a too-weak fence
    witnesses GREEN, *worse* than author-trust) and alignment on x86 (AC-off → no trap). Env note: only
    `qemu-system-x86_64` 8.2.7 is present (no ARM/RISC-V system QEMU, no user-mode QEMU, no
    cross-compilers); privileged ops are probeable because QEMU emulates privilege in system mode (boot
    a ring0 stub).
11. **TAINT (named floor)** — categories neither derived (Tier A / composition) nor witnessed (decidable
    Tier B) stay author-DECLARED but carry a greppable floor marker that TAINTS the importing function so
    it can never read as verified-green. Covers ordering, non-x86 privileged asm (no QEMU on this host),
    and `provenance_clear_on_output`.
12. **THE TRIANGLE + THE RESIDUAL FLOOR** — flexibility / safety / maintainability: pick any two fully,
    the third degrades. Effect-Row Composition is the *flexibility-weighted* winner (ADOPTED) =
    flexibility + safety, paying with a small leaf-audit and a named floor past the leaf boundary. The
    honest residual floor: (1) silicon errata / microarchitecture — the anchor moves from author's-word
    to GCC-contract (Tier A) + QEMU-TCG-model (Tier B), auditable and shared but neither is the die;
    (2) QEMU/TCG fidelity; (3) ordering + `provenance_clear_on_output` (tainted); (4) non-x86 privileged
    asm on this host (env limit, not architecture — installing system QEMU + cross-gcc promotes
    taint → witness); (5) leaf assertions + fold rules (small, centralized, reviewable without silicon).

---

## Relationship to Option E — this REFINES it, does not replace it

Option E (`docs/asm_lang_zer_safe.md` §1.7) remains the parent architecture. The following Option E
machinery **carries forward unchanged**:

- The **program-consequence vs hardware-consequence** vocabulary (CLAUDE.md "ZER's Goal";
  `asm_lang_zer_safe.md:2225-2243`). ZER owns 100% of program-consequence (every wrong USE of a value
  at the use site); hardware-consequence (does the silicon honor the declared categories) is the named
  floor. This split is exactly what concentrates into the leaf below.
- The **structure/semantics straddle** observation (`asm_lang_zer_safe.md:2216`) — asm is the one ZER
  construct that straddles the program-domain / hardware-domain boundary, which is *why* it uniquely
  needs a binding mechanism.
- The **closed category vocabulary** (`asm_lang_zer_safe.md:2270-2282`: `clobbers_flags`,
  `clobbers_register`, `reads_mem`/`writes_mem(width, ordering)`, `requires_aligned(n)`,
  `requires_nonzero`, `memory_barrier(ordering)`, `produces_carry`/`consumes_carry`,
  `changes_privilege`, `control_flow`, `provenance_clear_on_output`, `value_in_value_out`,
  `no_memory_effect`, …) and the operation taxonomy (`@arith_add_with_carry`, `@load_acquire`,
  `@atomic_cas`, `@cpu_write_cr3`, …). These ARE the effect-row categories ERBT folds.

What Effect-Row Composition **replaces**: Option E's **Layer 2 = "per-ISA bindings, each `@bind`
DECLARING categories, trusted via a library-author + manual checklist."** Option E's static verifier
checks Layer-3 call sites against DECLARED categories, so a wrong declaration produces a Layer-3 result
that is verified-against-a-lie (green, wrong on silicon). Effect-Row Composition replaces that single
trust mechanism with three:

- **DERIVED** — Tier-A leaves and all compositions get their rows computed (builtin contract table /
  fold rules), never declared by hand.
- **WITNESSED** — decidable Tier-B leaf categories are probed in QEMU and gated on the asm-hash.
- **TAINTED** — the irreducible remainder (ordering, non-x86 privileged, `provenance_clear_on_output`)
  stays author-declared but is marked floor and propagates the taint to callers.

So Layer 3 still verifies against rows, but those rows are now (mostly) machine-produced, and the
hand-declared residue is structurally fenced off as floor instead of silently believed.

---

## Reading order for a fresh session

1. **This file** (`00_summary.md`) — orientation, glossary, the 12-bullet design, the Option-E delta.
2. **CLAUDE.md "ZER's Goal"** — the program-consequence / hardware-consequence vocabulary that all asm
   claims must respect, and the closure philosophy ("no path to the unsafe construct except a
   compiler-enforced gate").
3. **`docs/asm_lang_zer_safe.md` §1.7** (the parent Option E) — the three-layer model, the closed
   category vocabulary (§1.7.3 list), the structure/semantics straddle, the SPARK kind-difference.
4. The **leaf / Tier-A / Tier-B** section of this document — what a leaf is, the `-ffreestanding`
   boundary, the ~36 Tier-B ops.
5. The **composition / ERBT / fold** section — the no-mnemonic grammar, the fold rules, why ordering
   is excluded.
6. The **closure argument** section — the theorem and why composition-safety alone is insufficient.
7. The **witness + taint** sections — the QEMU conformance harness, decidable vs unobservable
   categories, the taint marker.
8. **`docs/option_e_plan.md`** + the IMPLEMENTATION PLAN — Phase-1 table deletion through Step-7 DRC CI.

---

## Locked glossary

- **Level 0** — plain ZER arithmetic that emits C and is compiled by GCC. Not a leaf, not asm; the
  lowest already-safe primitive.
- **Tier A leaf** — a leaf backed by a GCC builtin; row DERIVED from the builtin's documented contract;
  portable; no raw asm. Objective test: compiles under `-ffreestanding`.
- **Tier B leaf** — a leaf backed by raw asm (real mnemonic) for a machine-configuring op with no
  portable builtin; per-ISA; row WITNESSED (decidable) or DECLARED + TAINTED.
- **Leaf** — the audited primitive unit; the only construct that may contain Tier-B raw asm; the only
  gated unit. Small, per-ISA, demand-driven, fail-closed.
- **Composition** — a `compose` operation that recombines leaves/ops with no asm and no core release;
  its grammar has no mnemonic production; its row is derived by fold rules.
- **Effect row** — the bundle of safety categories (from Option E's closed vocabulary) attached to an
  op, treated as a composable row that can be folded and inferred.
- **ERBT (Effect-Row Binding Types)** — the type discipline that makes categories composable rows the
  checker folds and infers; the type-theoretic backbone of composition.
- **Fold rule** — a closed, per-category rule for deriving a parent's category value from its children:
  union/OR for clobbers (sound), max for `requires_aligned` (sound); ordering/`memory_barrier` has NO
  sound fold and is excluded.
- **Conformance witness** — a toolchain-produced certificate that a Tier-B leaf's declared row matches
  observed QEMU behavior for decidable categories, bound to `(op, ISA, category-profile, asm-hash)`;
  consumed fail-closed at import.
- **Taint marker** — a greppable floor marker on a category that is neither derived nor witnessed; it
  taints the importing function so it can never read as verified-green.
- **The floor** — hardware-consequence: facts the language cannot verify (does silicon honor the
  declared categories). Surfaced at the narrowest typed boundary (here: the leaf), never silently
  believed.
- **Closure argument** — the theorem that safe leaves + sound fold ⇒ all composed ops are safe;
  safety is relative to leaf correctness, so the floor concentrates into the finite leaf set.
- **The triangle** — flexibility / safety / maintainability; pick any two fully. Effect-Row
  Composition (ADOPTED) takes flexibility + safety.
- **Decidable category** — one whose declared value is observable and deterministic in QEMU
  (clobbers, `changes_privilege`, `requires_nonzero`, trapping alignment); can be witnessed.
- **Unobservable category** — one whose declared value cannot be soundly observed in this environment
  (memory ordering under TCG-TSO; alignment on x86 with AC off); stays tainted, never witnessed.

---

## Verified repo anchors

- `emitter.c:3128`,`:3134`,`:3146`,`:3152` hardcode `__ATOMIC_SEQ_CST` for atomic *value*-ops, and
  `emitter.c:3098` is the *fence* path (the fence path is the one that is ordering-parameterizable).
  So "category derived from the builtin's contract" for ordering is FALSE today and must be WIRED;
  until then the derive-table must conservatively declare `ordering = seq_cst` (the actually-emitted
  truth).
- `checker.c:10529-10941` is the `NODE_ASM` handler region (no `@bind` / `@intrinsic_def` / allow-list
  exists there yet — that is Phase 2/3 work).
- CLAUDE.md:1295-1296 — Session-G Phase 3 in-block ordering enforcement was ABANDONED because it
  false-positived the canonical multi-block CLWB+SFENCE libpmem idiom. This is the evidence that
  positional ordering does not fold soundly.
- `docs/asm_lang_zer_safe.md:2227` / `:2270-2308` — the program-consequence statement and the closed
  category vocabulary + operation taxonomy that ERBT folds.
- `docs/option_e_plan.md` — the Phase-1 deletion plan (remove per-arch instruction/register tables,
  `arch_data/*.zerdata`, `asm_categories.{c,h}`).


## 1. The problem and the floor

This section defines the exact problem the rest of the document solves: the gap
between what an asm binding *declares* and what its asm body *actually does* on
silicon. It locates that gap inside ZER's program-consequence /
hardware-consequence vocabulary, explains why the prior design (Option E) left
the gap to manual diligence (the SPARK-trust model ZER exists to beat), and
derives — from the way an asm instruction *fuses* program-domain structure with
hardware-domain semantics — why asm alone among ZER constructs needs a binding
mechanism, and why that fusion forces the leaf/composition split this document
specifies. A reader with zero prior context should leave able to state the floor
precisely, point at it in the code, and see why the leaf is where it concentrates.

### 1.1 The binding-vs-silicon floor, stated exactly

ZER's asm safety architecture (Option E, `docs/asm_lang_zer_safe.md` §1.7) lets
a value-computing or machine-configuring operation be *declared* once as a
named semantic operation (the taxonomy: `@arith_add_with_carry`,
`@load_acquire`, `@atomic_cas`, `@cpu_write_cr3`, …) and *bound* per-ISA to an
asm body via `@bind` (§1.7.3). Every binding `@bind` declaration must select,
from a **closed category vocabulary** (§1.7.4), the structural effects its asm
body has: `clobbers_flags`, `clobbers_register(name)`, `reads_mem(width,
ordering)`, `writes_mem(width, ordering)`, `requires_aligned(n)`,
`requires_nonzero(operand)`, `memory_barrier(ordering)`, `produces_carry`,
`changes_privilege(from, to)`, `control_flow(...)`,
`provenance_clear_on_output(operand)`, `value_in_value_out`, `no_memory_effect`,
and so on. A downstream caller (Layer 3, firmware) then calls the typed
operation; the static verifier checks the *call site* against the *declared*
categories of the operation it is calling.

> **The floor, in one sentence:** *Does a binding's asm body actually honor the
> categories it declared?*

If a binding declares `clobbers_flags + no_memory_effect` but its asm body
secretly writes memory, ZER cannot detect it. This is stated verbatim in
`docs/asm_lang_zer_safe.md` §1.7.1:

> "If a binding declares `clobbers_flags + no_memory_effect` but the asm secretly
> writes memory, ZER cannot detect it. This is the floor for asm just as
> datasheet-correctness is the floor for MMIO declarations and
> signature-correctness is the floor for cinclude."

The verifier reasons over the **schema**, never over the asm string (§1.7.1) —
deliberate and correct, since it is what makes Layer 3 ISA-portable. But it has a
sharp consequence: a wrong category declaration is a lie the verifier cannot see.
Every Layer 3 call site that compiles green against a lying declaration is
**verified-against-a-lie** — green in the compiler, wrong on silicon. The
verification is sound *relative to* the declaration; the declaration is the floor.

### 1.2 Program-consequence vs hardware-consequence, applied to asm

ZER's load-bearing vocabulary (locked in `CLAUDE.md`, "ZER's Goal", and in
`docs/asm_lang_zer_safe.md` §1.7.1) splits "consequence" into two meanings that
must never be conflated:

- **Program-consequence** — what happens when a value is used wrongly *inside
  ZER source*. The use is in the program, so ZER owns it: caught at 100% at the
  use site. ZER's claim is *100% program-consequence coverage*.
- **Hardware-consequence** — what happens when a *fact* (silicon behavior,
  datasheet value, what an instruction does to the machine) is wrong relative to
  user belief. The fact lives outside the program and never enters it, so ZER
  has nothing to verify. This is the **floor** — out of scope for any language.

Applied to asm (faithful to §1.7.1):

- **Program-consequence (ZER total, 100%):** Every call site of every operation
  — `@arith_add_with_carry(a, b, c_in)`, `@load_acquire(ptr)`, `@barrier_full()`
  — is verified against the operation's declared categories: operand types,
  escape, provenance preservation through outputs, qualifier preservation
  through inputs, clobber accounting, ordering constraints, context permissions.
  Wrong use at the call site = compile error. This holds **regardless of which
  ISA binding the caller compiles against**, because the verifier reasons over
  the schema, not over asm strings.

- **Hardware-consequence (floor, surfaced at the `@bind` declaration site):**
  Whether the asm body inside a `@bind` declaration actually has the categories
  the declaration claims. Same floor pattern as datasheet-correctness for MMIO,
  signature-correctness for cinclude, baud-value-correctness for a peripheral.
  Different boundary *location* (the `@bind` declaration site), same floor
  *shape*.

The phrase §1.7.1 forbids from collapsing:

> "100% program-consequence" must NEVER read as "ZER verifies the asm body
> matches the contract."

It does not. The split is the whole honesty discipline: **owning every wrong
*use* of a value (program-consequence, 100%) does not entail certifying the
*fact* a binding asserts about silicon (hardware-consequence, floor).** This
document is about shrinking and concentrating that floor — not pretending it is
absent.

### 1.3 Why Option E left this floor manual — and why that is the model ZER rejects

Option E names the floor honestly and locates it precisely at the `@bind`
declaration site. But it leaves *discharging* the floor to the library author
plus a manual checklist. §1.7.6 makes this explicit as the design's strength:
because the contract language is the closed category vocabulary, reviewing a
`@bind` declaration is *a finite checklist task* ("for each declared category,
check the asm body") rather than the open-ended proof obligation a SPARK
arbitrary-predicate contract requires. The closed-vocabulary kind-difference
(§1.7.6) is real and is preserved by this document.

But "closed-shaped gap, reviewable by checklist" is still **a gap discharged by
a human looking at asm**. Whether the author actually performed the check, and
performed it correctly, is the author's diligence. Nothing in the toolchain
forces it. A wrong-but-syntactically-valid declaration compiles green and
propagates a lie to every Layer 3 caller.

That is precisely the model ZER's closure philosophy rejects. ZER's existing
guarantees are not "trust the user to run the right check"; they are
**compiler-enforced gates with no path to the unsafe construct except through
the gate**. There is no typed pointer except via `@inttoptr` + `mmio` — the
grammar makes the unsafe construct *unreachable* without a witness
(`checker.c:5601-5608`: no integer-to-pointer cast except through `@inttoptr`
with mandatory `mmio`). "Run a CI test" or "review the asm carefully" is the
**SPARK / MISRA trust-the-user model**: it relies on optional diligence and
fails open when the diligence lapses. The discipline ZER holds itself to:

> Prefer language/toolchain enforcement — *compile error*, or *unusable without
> a witness* — over optional diligence.

Option E's checklist is the best *manual* discharge of the floor (closed-shaped,
not open-shaped — strictly better than SPARK). It is still manual. **This
document's job is to convert as much of that manual discharge as possible into
enforcement** — derive the categories where they are derivable, witness them
where they are observable, and TAINT (a greppable, non-green floor marker) only
the irreducible residue. The floor does not vanish; it shrinks and concentrates.

### 1.4 The structure/semantics straddle — why asm uniquely needs a binding

This is the deepest architectural observation in the asm thread
(`docs/asm_lang_zer_safe.md` §1.7.5), and it is the reason the binding mechanism
exists *for asm and for no other ZER construct*.

**Every other ZER construct sits cleanly on one side of the program-domain /
hardware-domain boundary** (§1.7.5):

- A `u32` value, regardless of origin (hardware read, file, network, literal):
  program-domain at every operation, ZER owns 100%.
- An `mmio` address declaration: a hardware-domain claim *at the declaration*
  ("does this address really name the peripheral?"), then program-domain at
  every downstream use through the typed pointer. The boundary is crossed
  cleanly, *once*, at the declaration.
- A `cinclude` function signature: hardware-domain claim at the signature
  declaration, program-domain at every call site. Same clean cross.
- A linker-symbol extern: same pattern.
- A typed register access: the structure lives in the program (the typed
  pointer), the criterion lives in the datasheet (the address value) — both
  sides exist, but **they live in separate tokens**.

**An asm instruction is the exception. It straddles.** §1.7.5 gives the worked
case: `adc $0, $1` is *one token* that simultaneously:

- **Carries operand structure (program-domain):** the Z-rules apply to its
  inputs and outputs, escape analysis applies to register clobbers, provenance
  clearing applies to outputs, qualifier preservation applies through the
  boundary, ordering constraints apply, context flags apply. (ZER keeps 10 of
  13 Z-rules — Z1-Z8, Z11, Z12 — *active through asm operand boundaries*,
  unlike Rust which goes blind inside `unsafe { asm!() }`; see `CLAUDE.md`,
  "asm safety".)
- **Carries semantic meaning (hardware-domain):** what flags get clobbered, what
  state mutates, what precondition is required for correctness, what the
  instruction actually does to the machine — **and this varies per ISA**. The
  ISA manual is the datasheet for instruction meaning (§1.7.5).

> You cannot separate these by reading the token. The instruction's structural
> connections and its hardware semantics are **fused in one mnemonic.** That
> fusion is unique to asm among ZER constructs.

For an `mmio` address the two sides are *already in two tokens* (the typed
pointer carries the structure; the integer carries the datasheet claim), so the
boundary can be crossed cleanly at one declaration and ZER reasons about the
rest structurally. For an asm instruction there is no second token to factor the
hardware claim into — the meaning is *inside* the mnemonic, and it is per-ISA.

**Why the straddle forces a binding** (§1.7.5, paraphrased and faithful):

1. Asm's semantics are per-ISA hardware facts.
2. Any safety system that wants to reason about asm meaning must import that
   meaning from outside the program text.
3. "Outside the program, per-ISA" is, by definition, a binding (`@bind`).
4. ZER core cannot ship "what `adc` means" without committing to one ISA's
   semantics in the language core — which would make ZER an
   "x86 language wearing a portable costume."

The only factoring that preserves both *"ZER reasons about asm safety
structurally"* and *"ZER does not commit to an ISA"* is:

- **Structure-half stays in core** (universal, ISA-less, frozen): the Z-rules,
  the operand boundaries, the closed category vocabulary, the operation
  taxonomy.
- **Semantics-half imported per-ISA via `@bind`** (user/library-authored;
  hardware-consequence floor surfaced at the binding site).

§1.7.5 is emphatic that this is not a preference among options:

> "It is not 'the cleanest of three options' — it is the unique architecture that
> respects the structure/semantics straddle without either committing to one ISA
> or refusing to reason about asm meaning at all."

Any alternative either (A) commits ZER core to one ISA's semantics (breaking
architecture-agnostic positioning), or (B) refuses to reason about asm meaning
at all (dropping back to "asm is just text we hand to GCC"). The straddle
dictates the shape; the fusion in the token made the choice, not architectural
taste.

### 1.5 Why the straddle forces a leaf + composition split

The straddle says: import semantics per-ISA, reason about structure
universally. This document's refinement reads that mandate as a **partition of
the work**, and the partition is decided by **one test** (developed in detail in
the levels/tiers section of this document):

> *Is the operation inside C's abstract machine (a value-computation), or does it
> CONFIGURE the machine?*

The test sorts operations along the very seam the straddle exposes:

- **Value-computations** (arithmetic, bit ops, atomics, byteswap) are inside
  C's abstract machine. Their *semantics* are already captured by a portable
  GCC builtin (`__builtin_add_overflow`, `__atomic_*`, `__builtin_clz/ctz/
  popcount`, `__builtin_bswap`). The hardware-semantics half is *delegated to
  GCC*, which selects the real instruction and handles sub-architecture via
  `-march`. The category row is then **derived** from the builtin's documented
  contract — one audited table, one time, structural and portable.

- **Machine-configurations** (CR0/3/4 writes, RDMSR/WRMSR, I/O ports,
  SYSCALL/IRET, CPUID/INVLPG/WBINVD) configure privilege, page tables, MSRs,
  I/O ports, the interrupt flag — concepts C's abstract machine has *no model
  for*. GCC has no portable builtin (verified: `__rdmsr` does **not** compile
  `-ffreestanding`, while `clz`/`add_overflow`/`bswap` do). These are the only
  operations whose semantics genuinely must be imported per-ISA as **raw asm**.

This is exactly the leaf/composition split:

- A **LEAF** is the audited primitive unit — the *only* place the per-ISA
  semantics half lives. For value-computations it is a GCC builtin (the binding
  *is* the GCC backend; the category row is derived). For machine-configurations
  it is raw asm whose declared row is WITNESSED where decidable, otherwise
  DECLARED + TAINTED. Leaves are small, per-ISA, and finite (the
  machine-configuration set is ~36 ops).

- **COMPOSITION** is the flexible layer where authors recombine leaves/operations
  with **no asm and no core release**. The composed-binding grammar has *no
  mnemonic production* (you literally cannot write freeform asm in a
  composition — the `@inttoptr`-shaped gate, applied to asm). A composed
  operation's category row is **derived by closed fold rules over its children**
  (clobbers fold by sound UNION/OR; `requires_aligned` by MAX). This is the
  *structure-half reasoned universally* that the straddle promised: composition
  is pure program-domain reasoning, ISA-independent, and a new ISA gets all
  compositions for free once its leaf set exists.

The straddle is why the partition is *forced rather than chosen*: the only part
of an asm program that *cannot* be reasoned about universally is the per-ISA
semantics fused into the mnemonic — so that part, and only that part, is pushed
down into a finite, audited leaf set; everything above the leaf line is
structure, and structure folds.

This is also why **the floor concentrates into the leaf set.** Composition
faithfully propagates whatever the leaf declared — soundly, but lie and all — so
safety is *relative to leaf correctness*. Option E spread the floor across
`O(bindings)` distributed prose checklists; the leaf/composition split reduces it
to `O(leaves) + O(1)` fold proof: a small, centralized set of leaf assertions
plus one soundness argument for the fold. Later sections specify the tiers of
leaves, the derive table, the fold rules and their soundness boundary, the
conformance witness, and the named TAINT floor — all aimed at making the leaf set
as small, as derived, and as witnessed as the physics permits.

### 1.6 Where this lives in the tree today (so the floor is concretely locatable)

The floor is not abstract — it has a code site and a deletion plan:

- The static verifier's asm handler is the `NODE_ASM` handler in `checker.c`
  (~lines 10720-10890). No `@bind` / `@intrinsic_def` / allow-list mechanism
  exists there yet — that is later-phase work (`docs/option_e_plan.md`
  Phases 2/3).
- Phase 1 (`docs/option_e_plan.md`, Level C cleanup) *deletes* the per-arch
  infrastructure that the rejected disassemble-table designs would revive:
  `src/safety/asm_instruction_table_{x86_64,aarch64,riscv64}.c`,
  `asm_register_tables_*.c`, `asm_categories.{c,h}`, and the `arch_data/*.zerdata`.
  These are the "53/37/30-row" tables — the proactive per-ISA opcode tables this
  architecture explicitly does *not* rebuild.
- The ordering category is the hardest part of the floor and the reason
  positional `memory_barrier` is excluded from the fold vocabulary: ZER's own
  Session-G evidence (`CLAUDE.md` ~lines 1294-1296) records that Phase 3 in-block
  ordering enforcement was **ABANDONED** because it false-positived the canonical
  libpmem `CLWB`+`SFENCE` idiom. Ordering does not fold soundly and is not
  observable in the QEMU witness (TCG flattens to TSO), so it stays
  value-intrinsic and TAINTED — the irreducible corner of the floor, treated in
  full later in this document.

The lesson Session-G teaches is the same one that motivates the whole
enforcement-over-diligence stance here: do not ship enforcement that rejects
valid code, and do not ship a green checkmark for a property you cannot actually
observe. The floor must be *named*, not painted over.


## 2. The design-space exploration — why this design and not the others

This section is the audit trail for the architectural choice. A fresh session
reading only this document must know that the adopted design (Effect-Row
Composition) was not the first idea, not the only idea, and not picked by
preference — it survived a deliberate adversarial fan-out against thirteen
competitors, judged on two fixed lenses, with a third priority (flexibility)
added late that reordered the winner. Recording the *losers and their killing
reasons* is the load-bearing part: it stops a future session from re-proposing a
design that was already tried and rejected for a specific, verifiable reason
(most dangerously, the per-ISA opcode-table designs that Phase 1 of
`docs/option_e_plan.md` is *deleting*).

### 2.1 The fan-out method

The exploration was a 14-angle adversarial fan-out run in two waves, roughly 58
agents total. "Adversarial" means each candidate design was developed by an agent
*and then attacked* by the next — the brief was to find the failure mode, not to
advocate. Every candidate was scored on two fixed lenses:

- **Maintainability** — how much recurring human work the design imposes on a
  *solo* author (ZER is solo-authored; this is not incidental, it disqualifies
  whole families of design — see n-version and formal-Sail below). The named
  disqualifier, inherited from the Level C cleanup rationale, is
  **per-instruction / per-vendor / per-ISA-extension PROACTIVE scaling**: any
  design that must enumerate the ISA catalog *ahead of demand* loses on
  maintainability. This is exactly the machinery Phase 1 deletes — the
  `asm_instruction_table_{x86_64,aarch64,riscv64}.c`, the
  `asm_register_tables_*.c`, `asm_categories.{c,h}`, the
  `arch_data/*.zerdata`, and the `gen_*` probe scripts
  (`docs/option_e_plan.md` §1, the DELETE table). A design that *revives* this
  machinery is not a step forward; it un-does the cleanup.

- **Soundness** — does the mechanism *structurally* prevent a wrong asm-safety
  declaration from reading as verified-green, or does it merely make wrongness
  *less likely*? ZER's CLOSURE philosophy (CLAUDE.md: "no path to the unsafe
  construct except a compiler-enforced gate") rejects "run a CI test" /
  "trust-the-user" mechanisms as the *gate*. A design that fails open (an
  unhandled input silently passes) fails the soundness lens outright,
  regardless of how good its happy path is.

The two waves let candidates that survived Wave 1 be re-attacked with the
specific weakness another candidate exposed. The output was a scoreboard, not a
single recommendation — because the ranking *depends on how you weight the
lenses*, which is the crux of §2.2.

Scoreboard (the designs that matter for a fresh session; the disposition column
is the actionable part):

| Design | What it is | Disposition | Why |
|---|---|---|---|
| **Intrinsic-Maximalism** | floor-by-subtraction: builtin dual-path shrinks the floor's *domain*, residue structurally fenced | maint-weighted **winner (8.4)** | safety+maintenance; frozen taxonomy loses flexibility |
| **Effect-Row Composition** | closed-leaf `@bind` (no mnemonic production) ⊕ ERBT + witness/taint | **ADOPTED** (flex-weighted) | safety+flexibility; pays small leaf-audit + named floor |
| **BIND-VIA-BUILTIN** | GCC backend *is* the binding; categories derived from builtin contract | **KEPT** as Tier-A mechanism | maint 9 / enforce 8 — folded into the adopted design |
| **Witnessed Floor / SDCW** | 4-owner decision order: derive → compose → witness → taint | **KEPT** as the verification-ownership skeleton | composes the other surviving pieces in priority order |
| **DCA** (disassemble-classify) | disassemble + classify against opcode table | **REJECTED** | revives deleted table + fail-open + "100×" fiction |
| **DSC** (disassemble-static-check) | DCA + check declarations vs classification | **REJECTED** | same three reasons as DCA |
| **n-version** | N independent impls per-op-per-ISA, cross-checked | **REJECTED** | per-op-per-ISA cost + common-mode under solo |
| **formal-Sail** | discharge leaves vs official ARM/RISC-V Sail models | **REJECTED-for-solo** (maint 3) | per-ISA + from-scratch toolchain + x86 has no model |
| **standalone QEMU-witness** | ring0 stub observes leaf behavior in QEMU | **DEMOTED** | existential-vs-universal gap; decidable x86 predicates only |
| **DRC** (differential reference) | pure-ZER reference + fuzzed differential | **KEPT-as-CI**, not a gate | catches wrong-VALUE bugs but is a test, warns cross-compile |
| **litmus / CWH** | herd7-style ordering probing | **CONFIRMATORY** | probabilistic; confirms ordering is the hardest category |

The two surviving *composite* designs deserve a note because they are not
competitors to the adopted design — they are *inside* it. **BIND-VIA-BUILTIN** is
the Tier-A leaf mechanism: the GCC backend *is* the binding, and the effect row
is derived from the builtin's documented contract (one audited table, one-time).
**Witnessed Floor / SDCW** is the decision-ownership skeleton — the rule that a
category's row is settled by trying, *in this order*, derive (Tier A /
composition) → compose (fold) → witness (decidable Tier-B) → taint (the named
floor for what neither derives nor witnesses). The adopted design is literally
"Composition-Only @bind + ERBT, with BIND-VIA-BUILTIN supplying the leaves and
SDCW supplying the decision order." The fan-out did not pick one winner and
discard the rest; it picked a *vertex* and assembled the surviving pieces that
sit at it.

### 2.2 The two winners and the re-weighting

**Two different designs won, under two different weightings.** This is the most
important fact in the section.

**Maintainability-weighted winner: Intrinsic-Maximalism — 8.4.**
If maintainability is the dominant lens, the winner is *floor-by-subtraction*:
push as many operations as possible onto a GCC builtin dual-path so the floor's
*domain* shrinks (the operation no longer needs raw asm at all, so there is no
declaration to get wrong), and structurally fence the small residue that has no
builtin. Frozen taxonomy: the set of expressible operations is fixed and grows
only by deliberate ZER-team addition (~1–2 ops per decade, matching the
"|Y_intrinsic| ... ~1–2/year additions" / "STABLE at current ~130" framing in
`docs/asm_lang_zer_safe.md` ~lines 1252/1345). This is a *maintainability asset*:
a frozen vocabulary means no per-ISA bring-up, no per-instruction table, no
recurring author work — the disqualifier is structurally avoided because there is
nothing to scale. The companion BIND-VIA-BUILTIN mechanism (the GCC backend *is*
the binding; categories derived from the builtin's documented contract) scored
even higher on the maintainability sub-axis (maint 9 / enforce 8) and is what the
adopted design keeps as its Tier-A leaf mechanism.

**Flexibility-weighted winner (ADOPTED): Effect-Row Composition.**
The re-weighting is this: **flexibility was added as a co-equal third priority.**
The question "can an *author* (not the ZER team) express a *new* operation
without a core release?" is not answered by Intrinsic-Maximalism — its frozen
taxonomy is the very thing that made it cheap. A frozen taxonomy is a
maintainability *asset* and simultaneously a flexibility *liability*: the same
property (the vocabulary cannot grow except by ZER-team action ~1–2/decade) that
eliminates recurring work also means a firmware author who needs an op outside
the taxonomy has *no in-language path* — they wait for a core addition or drop to
raw asm. Once flexibility is weighted equally, that liability dominates, and the
winner flips to **Effect-Row Composition = Composition-Only (closed-leaf `@bind`,
no mnemonic production) ⊕ ERBT (effect-row types) + witness/taint escape.**

Effect-Row Composition pays for its flexibility with a *small leaf-audit* (the
finite Tier-B raw-asm leaf set, ~36 ops, demand-driven) plus a *named floor* past
the leaf boundary (the tainted categories). In exchange, authors write `compose`
operations that recombine existing leaves/ops — no asm, no core release — and the
composed effect row is derived by closed fold rules. New-ISA bring-up is "write
its leaf set once, then *all* compositions work on it for free": flexibility and
per-ISA cost decouple. This is why it wins the flexibility lens while staying
sound (safe leaves + sound fold ⇒ everything composed above the leaf line is
safe).

The frozen taxonomy is the exact pivot, so it is worth stating precisely. Under
Intrinsic-Maximalism a new operation can only enter the language by a ZER-team
addition to the catalog — the same "slow promotion ... handful per several years"
/ "STABLE at current ~130" rate documented in `docs/asm_lang_zer_safe.md`
(~lines 1345/1822). For the *maintainer*, that ceiling is the whole point: a
vocabulary that cannot grow except deliberately is a vocabulary that imposes no
recurring per-ISA work, which is why it dodges the disqualifier. For the *firmware
author*, the identical ceiling is a wall: an op outside the catalog is
inexpressible without either lobbying for a multi-year core addition or dropping
to raw asm (which re-opens the very unsafety the language exists to close). The
two readings are the same fact seen from the two sides of the trade — there is no
tuning that makes a frozen taxonomy both un-growing-for-the-maintainer and
freely-growing-for-the-author. Effect-Row Composition breaks the bind not by
un-freezing the *leaf* set (the leaves stay a small, audited, demand-driven set —
that is where safety concentrates) but by making *composition above the leaves*
the author-extensible surface: authors recombine frozen leaves under closed fold
rules, so expressiveness grows without the catalog growing and without raw asm
re-appearing.

The decision is therefore not "Effect-Row Composition is better than
Intrinsic-Maximalism." It is "under a three-priority weighting
(flexibility + safety + maintainability) where flexibility is co-equal,
Effect-Row Composition is the point on the trade-off surface ZER chose." See the
triangle in §2.4.

### 2.3 Rejected and demoted designs — the scoreboard with killing reasons

Each entry below names the design, its appeal, and the *specific* reason it lost.
The killing reason is what a fresh session needs: it is what stops the design
from being re-proposed.

**REJECTED — DCA (Disassemble-Classify) and DSC (Disassemble-Static-Check).**
Appeal: instead of trusting an author's declared categories, *disassemble* the
emitted/assembled asm and classify each instruction against an opcode table, then
check declarations against the classification. Three killing reasons, any one
fatal:
1. **Revives the deleted per-ISA opcode table** — exactly the
   `asm_instruction_table_*.c` infrastructure Phase 1 deletes
   (`docs/option_e_plan.md` §1). This trips the named disqualifier
   (per-instruction/per-ISA PROACTIVE scaling) head-on; the design *un-does* the
   cleanup it is layered on top of.
2. **Fail-open** — an untabled mnemonic classifies as `NO_CATEGORY`, and a wrong
   declaration against `NO_CATEGORY` passes GREEN. The hole is silent and grows
   with every new instruction the table doesn't cover. This is the
   trust-the-user failure mode ZER's closure philosophy rejects, dressed up as
   automation.
3. **The "100× smaller table" defense was a VERIFIED FICTION.** The argument for
   DCA was that the classification table would be ~100× smaller than a full
   opcode table. The real tables ZER actually built are **53 / 37 / 30 rows**
   (x86_64 / aarch64 / riscv64) — roughly **120 rows total**, not the ~1500-row
   straw man the "100×" claim divided against. The reduction is illusory.
   Additionally, **-O0 compiler glue contaminates the disassembly**: in a
   measured `cmpxchg` case, **8 of 11 instructions** in the disassembled output
   were compiler glue, not the intended operation — so even the classification
   step is unreliable on real output.

**REJECTED — n-version.**
Appeal: implement each op multiple independent times (per-op, per-ISA) and
cross-check the versions; a single author error shows as a disagreement.
Killing reason: **per-op-per-ISA cost** (the disqualifier, multiplied by the
version count) *plus* **common-mode failure under solo authorship** — the same
author writing all N versions makes correlated errors, so the versions agree on
the *same* wrong answer. n-version's soundness premise (independent failure modes)
is false when there is one author. It buys cost without buying the soundness it
exists to provide.

**REJECTED-for-solo — formal-Sail.**
Appeal: the aspirational *ceiling*. Discharge each leaf's declared behavior
against the official ARM / RISC-V Sail formal models — a real proof, not a
probe. Maint 3. Killing reasons: (1) **per-ISA**, against official models that
must be obtained and tracked per architecture; (2) requires a **from-scratch
Sail/OCaml verification toolchain** — infeasible for a solo author; (3) **x86 —
the most asm-heavy ISA — has NO official Sail model**, so the highest-value
target is exactly the one this approach cannot cover. It is the correct *north
star* for what "verified leaf" would ideally mean, and it is recorded as such,
but it is not a shippable gate here. The witness mechanism (§ conformance
witness in the main design) is the pragmatic, decidable substitute for the
decidable categories.

**DEMOTED — standalone QEMU-witness.**
Appeal: boot a ring0 stub under `qemu-system-x86_64` and *observe* a leaf's
behavior, comparing observed effects against its declared categories. Why
demoted rather than rejected: a standalone witness certifies an **EXISTENTIAL**
("these categories were *observed* on x86 in QEMU") that the safety gate would
consume as a **UNIVERSAL** ("the declaration is honored"). That inference is
sound *only for decidable predicates* — categories that are observable and
deterministic (clobbers_register/clobbers_flags via sentinel sweep,
changes_privilege via CPL readback, requires_nonzero via zero-trap). For
unobservable categories (memory ordering: QEMU TCG flattens to TSO, so a
too-weak fence witnesses GREEN — *worse* than author-trust because it manufactures
false confidence; x86 alignment: AC-off means no trap) the existential→universal
leap is unsound. So the witness is **demoted to a subordinate role: decidable x86
predicates only**, never the whole gate. It is also x86-only in this environment
(only `qemu-system-x86_64` 8.2.7 is present; no ARM/RISC-V system QEMU, no
user-mode QEMU, no cross-compilers) — a reason it cannot stand alone as the
verification story.

**REJECTED-as-gate, KEPT-as-CI — DRC (Differential Reference Compilation).**
Appeal: write a pure-ZER reference implementation of each op, run a fuzzed
differential against the asm leaf, and flag any value divergence. DRC catches a
class *no category mechanism can*: **wrong-VALUE** bugs — adc-drops-carry, an
off-by-one in bsr, a CAS with operands swapped. These are correctness bugs inside
a leaf that has *correct categories*; the effect-row machinery is blind to them
by construction (it reasons about effects, not values). Killing reason as a
*gate*: DRC is a **TEST, not a structure** — it is process, not a compile-time
property, and on cross-compilation it can only warn (you cannot run an
ARM/RISC-V binary's differential on an x86 CI host). A warning-not-error that
runs only on the native host is not a gate (it fails ZER's "compile error /
unusable-without-witness" bar). So DRC is **kept as an opt-in x86-native CI
sanity layer** (`make drc-x86`, STEP 7 of the plan), never the gate. Its value is
real and orthogonal — it is the only thing in the whole exploration that catches
wrong-value leaf bugs — which is exactly why it is retained rather than dropped.

**CONFIRMATORY — litmus / CWH (herd7-style concurrent ordering probing).**
Appeal: the *only* path toward verifying the ordering category — run litmus
tests / a concurrency-witness harness to probe whether a fence is strong enough.
Result: it is **explicitly PROBABILISTIC** (litmus testing observes *some*
interleavings, never proves their absence) and **x86-is-TSO hides both
under-declaration and over-declaration** (the strong default memory model masks a
too-weak fence). Its contribution to the exploration is *confirmatory, not
constructive*: it independently confirms that **ordering is the irreducible
hardest category** — no available mechanism (fold, witness, or litmus) makes it
sound on this host. This is the same conclusion ZER reached the hard way in
Session G: Phase 3 in-block ordering enforcement was **ABANDONED** because it
false-positived the canonical libpmem CLWB+SFENCE idiom (CLAUDE.md ~lines
1295–1296; `docs/asm_lang_zer_safe.md` §5.4, the "Session G Phase 3 lesson",
~line 3988+; "don't ship enforcement that rejects valid code patterns"). Ordering
therefore stays **value-intrinsic and TAINTED**, excluded from the fold
vocabulary — litmus/CWH is the evidence that this is not pessimism but the actual
boundary.

### 2.4 The organizing lens — the flexibility / safety / maintainability triangle

The whole exploration resolves to one trade-off triangle. **You can have any two
fully; the third degrades.** This is why two designs "won" — they sit at
different vertices, and the choice is which corner to give up.

```
                         SAFETY
                          /\
                         /  \
                        /    \
   Intrinsic-Maximalism●      ●Effect-Row Composition  (ADOPTED)
   (safety+maintenance,        (safety+flexibility,
    frozen taxonomy =           pays: small leaf-audit
    can't express new ops)      + named floor past leaf)
                       /          \
                      /            \
        MAINTAINABILITY ---------- FLEXIBILITY
                          ●DCA
              (flexibility + low-friction,
               loses SAFETY — fail-open)
```

Reading the three corners:

- **Safety + Maintainability, sacrifice Flexibility → Intrinsic-Maximalism.**
  Frozen taxonomy. Cheap to maintain, structurally safe, but can express only
  the ~130 ops the ZER team has blessed; new ops arrive at ~1–2/decade. The
  maintainability-weighted winner; rejected as the *primary* design only because
  flexibility was raised to co-equal.

- **Safety + Flexibility, sacrifice some Maintainability → Effect-Row
  Composition (ADOPTED).** Authors compose freely without a core release; safety
  holds via safe leaves + sound fold; the cost is a small, *centralized*
  leaf-audit (O(leaves) + O(categories), reviewable without silicon) plus a named
  floor (the tainted categories) past the leaf boundary. Crucially the
  maintainability cost is *bounded and demand-driven*, not the unbounded
  per-ISA-catalog scaling of the disqualifier: new-ISA bring-up is one leaf set,
  then every composition works for free.

- **Flexibility + low-friction, sacrifice Safety → DCA.** Maximum expressiveness,
  minimal author ceremony, but fail-open. This corner is the one ZER's closure
  philosophy forbids — it is the SPARK/trust-the-user model, and it is why DCA is
  rejected rather than merely demoted.

The adopted design's central theorem (developed fully in the main document)
follows directly from picking the safety+flexibility corner: **safe leaves +
sound fold ⇒ everything composed above the leaf line is safe**, with the
important honesty that *composition safety alone is not sufficient* (a sound fold
faithfully propagates a *lying* leaf — safety is relative to leaf correctness).
The floor does not disappear; it **concentrates into the finite leaf set**,
reducing the verification problem from O(bindings) distributed prose checklists
(the Option E status quo) to O(leaves) + O(1)-fold-proof. That concentration —
trading flexibility's cost for a small, centralized, auditable floor instead of
giving up safety (DCA) or flexibility (Intrinsic-Maximalism) — is the whole
reason this vertex was chosen.


## 3. Three levels, two tiers — the "inside C's abstract machine?" test

This section defines the structural partition that the rest of the design rests
on. Every operation a ZER program can express is placed into exactly one of
three levels. The boundary between the two that matter for asm safety — Tier A
vs Tier B — is decided by a single objective test, not by taste. The whole point
is to make the set of operations that require raw-assembly verification as small
as possible, because the residual floor of this design concentrates into that
set (see §"Closure argument" and §"Conformance witness"). Safety here is bought
by **avoiding** assembly, not by cleverly verifying it.

### 3.1 The decisive test

> **Is the operation a value-computation *inside* C's abstract machine, or does
> it CONFIGURE the machine?**

C's abstract machine has a model for: integers, their arithmetic (with
well-defined wraparound under `-fwrapv`), memory cells, loads/stores, bit
operations, and — since C11 — atomics with an ordering parameter. It has **no
model** for: privilege level / current protection ring, page-table base
registers, model-specific registers (MSRs), I/O port space, the interrupt-enable
flag, TLB/cache state as nameable entities, or the CPUID feature space.

- If the operation produces a value (or a memory effect) that C's abstract
  machine can already describe, GCC has — or can have — a portable builtin for
  it. That operation is **Tier A**.
- If the operation changes machine state that C has no concept of, there is no
  portable builtin and never can be (you cannot write a portable `__builtin`
  that "sets the page-table base register" because portable C has no page
  tables). That operation is **Tier B**, and it is the only place raw assembly
  is permitted.

The test is not a heuristic. It coincides with an objective, mechanically
checkable boundary: **does the operation compile under `-ffreestanding`?** A
freestanding GCC has the full builtin/atomic surface but no hosted runtime and
no privileged-instruction intrinsics. Verified facts: `__builtin_clz`,
`__builtin_add_overflow`, and `__builtin_bswap32` compile `-ffreestanding`;
there is no `__builtin_rdmsr` — reading an MSR can only be written as raw asm
(`__asm__ __volatile__("rdmsr" ...)`, which is exactly what the emitter does
today for `@cpu_read_msr` at `emitter.c:7184`). The presence/absence of a
freestanding builtin *is* the Tier-A/Tier-B line made objective.

### 3.2 Level 0 — plain ZER arithmetic (not a leaf)

Level 0 is ordinary ZER expression code: `a + b`, `x & mask`, `v << n`,
comparisons, struct field reads, array indexing. It lowers to plain C and is
handed to GCC. It is the **lowest primitive** in the design and it is **already
fully safe** by ZER's existing guarantees — integer overflow wraps (defined, via
`-fwrapv` and the `#pragma GCC optimize("wrapv")` emitter preamble), shift by
`>=` width yields 0 (the `_zer_shl`/`_zer_shr` wrappers), bounds are checked,
division-by-zero is gated. See CLAUDE.md "Safety Guarantees" table.

Level 0 is **not a leaf** and never appears in the asm-safety verification
machinery at all. There is no effect row to derive, no witness to run, no taint
to propagate. It is mentioned here only to fix the floor of the hierarchy: when
an author composes operations (§"Effect-row composition"), the base case of the
fold can be a Level-0 arithmetic value, which carries the empty/trivial effect
row. Anything expressible at Level 0 should stay at Level 0 — never reach for a
builtin (Tier A) or asm (Tier B) for something `+` already does safely.

### 3.3 Tier A leaf — a GCC builtin (no asm)

A **Tier-A leaf** is an operation backed by a GCC builtin or a C11 atomic
builtin. There is **no raw assembly** anywhere in a Tier-A leaf. The defining
properties:

1. **GCC owns instruction selection.** The leaf emits a builtin call; GCC
   selects the real machine instruction. `@bswap32` emits `__builtin_bswap32`
   (`emitter.c:8294`); `@popcount`/`@ctz`/`@clz`/`@parity`/`@ffs` emit
   `__builtin_popcount`/`ctz`/`clz`/... dispatched on operand width
   (`emitter.c:8290`–`8336`); the alloc-size overflow guard emits
   `__builtin_mul_overflow` (`emitter.c:1948`); the 15 `@atomic_*` intrinsics
   emit `__atomic_load_n` / `__atomic_store_n` / `__atomic_compare_exchange_n` /
   `__atomic_fetch_*` / `__atomic_*_fetch` (`emitter.c:8362`–`8400`).

2. **Sub-architecture is GCC's job via `-march`.** The author does not pick
   between `POPCNT` (SSE4.2) and a software popcount, or between `LZCNT` (BMI1)
   and `BSR`. GCC picks based on `-march`/`-mtune`. This is why the existing
   comment at `emitter.c` reads "All use GCC builtins — auto-port to
   x86-64/ARM64/RISC-V without per-arch work." A Tier-A leaf is portable for
   free: one leaf definition, every ISA GCC supports.

3. **Effect row is DERIVED from the builtin's documented contract.** A Tier-A
   leaf's categories (clobbers_flags, value_in_value_out, reads_mem/writes_mem
   with width and ordering, requires_nonzero, ...) are not author-declared. They
   are looked up in **one audited table** keyed by builtin name. This table is
   written and reviewed **once** (it is `O(1)` maintenance: ~a dozen builtin
   families, not per-binding and not per-ISA). The author of a Tier-A leaf
   **cannot** state categories — declaring them is a compile error, because the
   derive-table is authoritative (Step 1 of the implementation plan,
   `option_e_plan.md` / the S1 derive-table in checker.c).

4. **"Using intrinsics, not emulating."** GCC builtins emit the *real*
   instruction the operation names; they are not a software shim that pretends
   to be hardware. There is exactly **one** genuine emulation exception worth
   naming: **RISC-V add-with-carry**. RISC-V has no architectural carry flag, so
   `__builtin_add_overflow` on RISC-V compiles to a compare-and-branch sequence
   that *computes* the carry rather than reading a flag. That is real emulation
   — and it is the correct, safe behavior (the program-level result is
   identical; only the produces_carry/consumes_carry category is a derived
   property of the builtin contract, not of a physical flag). Every other
   Tier-A op maps to a native instruction on every target that has one.

**Coverage.** Tier A is roughly **95% of operations by count** — the entire
computational / ALU / atomic / bit-manipulation surface. This is the
"floor-by-subtraction" lever: each op that fits Tier A is an op that needs no
raw-asm verification, no per-ISA binding, and no conformance witness. Maximize
Tier A and the thing left to verify shrinks toward nothing.

**The ordering caveat (must be wired, today it is a lie).** "Effect row derived
from the builtin contract" is currently **false for the ordering category** on
atomics. The emitter hardcodes `__ATOMIC_SEQ_CST` for every atomic value-op:
`@atomic_load` → `..., __ATOMIC_SEQ_CST)` at `emitter.c:8364`, store at `:8368`,
cas at `:8375`, the fetch ops at `:8399`; the duplicate site near
`emitter.c:3128`–`3152` does the same; the inline comment at `emitter.c:8357`
says "All SEQ_CST ordering for now (Ordering param deferred)." Only the
**fence** path is ordering-parameterized (`__atomic_thread_fence(__ATOMIC_*)` at
`emitter.c:3098`–`3102`, mirrored at `:6854`). Consequence for this design:
until the ordering parameter is threaded through the atomic value-ops, the
derive-table **must conservatively declare `ordering = seq_cst`** — the
actually-emitted truth — rather than the author-intended ordering. Declaring
anything weaker would make the derived row a lie about the emitted C. (Wiring
this is Step 2 of the plan; ordering also stays value-intrinsic and is excluded
from the fold and never witnessed — see the other sections.)

### 3.4 Tier B leaf — raw asm (the irreducible residue)

A **Tier-B leaf** is an operation that fails the decisive test: it CONFIGURES
the machine, C has no model for it, and GCC therefore has no portable builtin.
It is the **only** place a real mnemonic (raw assembly) is allowed to appear,
and it is the only **gated** unit. Tier-B leaves are per-ISA and small.

The Tier-B residue is roughly **~36 ops**, grouped:

- **Control-state writes:** `CR0`/`CR3`/`CR4` reads/writes (`@cpu_read_cr3` /
  `@cpu_write_cr3` etc.), `RDMSR`/`WRMSR` (`@cpu_read_msr` /`@cpu_write_msr`;
  the emitter already emits the raw `rdmsr` at `emitter.c:7184`), `XSETBV`
  (`@cpu_write_xcr0`).
- **I/O ports:** `IN`/`OUT` byte/word/dword (`@port_in8` … `@port_out32`) — x86
  port space, which C cannot address.
- **Privilege transitions:** `SYSCALL`/`SYSRET`, `IRET`, and the ISA analogues
  `ECALL` (RISC-V), `SVC`/`ERET` (ARM) — `@cpu_syscall` / `@cpu_sysret` /
  `@cpu_iret`.
- **Inspection / cache / TLB:** `CPUID`, `RDTSC`/`RDPMC`, `INVLPG`, `WBINVD`,
  `CLWB`/`CLFLUSHOPT` (`@cpu_cpuid`, `@cpu_read_pmc`, the cache ops, etc.).

These configure privilege, page tables, MSRs, I/O ports, and the interrupt flag
— concepts C has no model for — so no portable builtin is possible, by
construction, not by GCC's oversight.

A Tier-B leaf's effect row cannot be derived from a builtin contract (there is
no builtin). Instead it is, per category:

- **WITNESSED** where the category is decidable on the host emulator — e.g.
  clobbers_register/clobbers_flags via a sentinel sweep, changes_privilege via a
  CPL readback, requires_nonzero via a zero-trap (see §"Conformance witness").
- **DECLARED + TAINTED** where the category is not decidable on this host —
  memory ordering (QEMU TCG flattens to TSO), x86 alignment (AC-off does not
  trap), provenance_clear_on_output, and any privileged op on a non-x86 ISA for
  which this environment has no system QEMU. The declaration stands but carries a
  greppable floor marker that taints the importing function (see §"Taint").

The Tier-B set is maintained **demand-driven** and **fail-closed**: a leaf is
implemented only when firmware actually uses that op; an unimplemented
privileged op is a **compile error**, never a silent hole. The set you *use* is
mostly decades-stable (CR3/MSR/port/IRET/IF), and new privileged ops arrive
slowly and vendor-driven (XSAVE, FSGSBASE, SMAP, PKE, CET each added theirs over
a generation). So the burden tracks *your* usage — tiny and slow — not the ISA
catalog.

### 3.5 Worked classification

| Operation | Level / Tier | Why | Emit / anchor |
|---|---|---|---|
| `a + b`, `x & mask`, `v << n` | **Level 0** | value-computation already in C's machine; already safe | plain C + `-fwrapv` / `_zer_shl` |
| array index `buf[i]` | **Level 0** | in-machine; bounds checked by existing ZER | plain C + `_zer_bounds_check` |
| `@bswap32(x)` | **Tier A** | pure value-computation, freestanding builtin exists | `__builtin_bswap32` (`emitter.c:8294`) |
| `@popcount`/`@ctz`/`@clz`/`@ffs` | **Tier A** | in-machine bit query; GCC picks POPCNT/LZCNT via `-march` | `__builtin_*` (`emitter.c:8290`–`8336`) |
| `__builtin_mul_overflow` (alloc guard) | **Tier A** | overflow-checked arithmetic, in-machine | `emitter.c:1948` |
| `@atomic_add`, `@atomic_cas`, `@atomic_load` | **Tier A** | C11 atomics are part of the abstract machine | `__atomic_*` (`emitter.c:8362`–`8400`) |
| RISC-V add-with-carry | **Tier A (emulated)** | no carry flag on RISC-V → builtin emits compute-the-carry sequence; the one true emulation case | `__builtin_add_overflow` lowering |
| `@cpu_read_cr3` / `@cpu_write_cr3` | **Tier B** | page-table base register — C has no model | raw asm leaf, per-ISA |
| `@cpu_read_msr` (`RDMSR`) | **Tier B** | MSR space; no freestanding builtin | raw `rdmsr` (`emitter.c:7184`) |
| `@port_in8` / `@port_out32` (`IN`/`OUT`) | **Tier B** | x86 I/O port space; C cannot address it | raw asm leaf |
| `@cpu_disable_int` / interrupt flag | **Tier B** | interrupt-enable flag; not in C's machine | raw `cli`/`sti`/`cpsid`/`csrci` leaf |
| `@cpu_syscall` / `@cpu_iret` | **Tier B** | privilege transition | raw `syscall`/`iret`/`ecall`/`eret` leaf |
| `@cpu_cpuid` | **Tier B** | feature-inspection instruction; no portable builtin | raw `cpuid` leaf |

Disambiguation rule when an op looks borderline: apply the freestanding test. If
`gcc -ffreestanding` accepts a builtin for it, it is Tier A; if the only way to
emit it is `__asm__` with a mnemonic, it is Tier B. There is no third option and
no author discretion.

### 3.6 The floor-by-subtraction principle

The strategic claim of this partition: **safety by avoidance, not by
verification.** Every operation pushed into Tier A is an operation that:

- needs no raw-asm verifier (it has no asm),
- needs no per-ISA binding (GCC + `-march` ports it),
- needs no conformance witness (its categories are derived from a one-time
  audited table, not observed),
- and inherits ZER's existing Level-0 value safety through its operands.

Because Tier A absorbs ~95% of operations, the set that needs real
raw-assembly verification — the Tier-B leaves — is small (~36, per-ISA,
demand-driven, fail-closed). The residual floor of the entire asm-safety design
(silicon errata, QEMU/TCG fidelity, ordering, non-x86 privileged asm on this
host) concentrates into that small set and into the one-time
derive-table/fold-rule audit. That concentration — from `O(bindings)` distributed
prose checklists down to `O(leaves) + O(1)` — is the central engineering payoff,
and Tier A is the lever that makes it possible. Maximize Tier A; verify only what
truly cannot live there.

> Illustrative surface (proposed, not yet shipped): a leaf binding might be
> declared `leaf @cpu_read_cr3 -> u64 { asm: "mov %%cr3, %0" ; categories:
> changes_privilege, clobbers_register(...) }` for Tier B, versus a Tier-A op
> which has **no** binding body at all — its categories come from the
> derive-table and any author-stated categories are rejected. Treat all such
> syntax as illustrative (proposed surface, not yet shipped); the load-bearing
> facts are the emit anchors and the freestanding test above.


## 4. Leaves — the audited primitive unit

A **leaf** is the smallest indivisible asm/operation unit that ZER trusts directly,
rather than deriving by composition. It is the only place a Tier-B raw mnemonic may
appear, and it is the only gated/audited unit in the whole design. Everything above
the leaf line (Section 5, composition) is *derived*: a composed op carries no asm and
its effect row is folded from its children by closed rules. Composition can only
faithfully propagate what the leaves declare — so **all safety is relative to leaf
correctness, and the floor concentrates into the leaf set.** The leaf set is small
and per-ISA; the audit is centralized. This is the genuine shrink from Option E's
O(bindings) distributed prose-checklists to O(leaves)+O(1)-fold-proof (the closure
theorem, Section 6).

### 4.1 The one test that classifies a leaf

Every operation is sorted by a single structural question:

> *Is the operation INSIDE C's abstract machine (a value-computation), or does it
> CONFIGURE the machine (privilege, page tables, MSRs, I/O ports, the interrupt
> flag — concepts C has no model for)?*

- **Value-computation** → **Tier A leaf** (a GCC builtin; no raw asm; portable).
- **Machine-configuration** → **Tier B leaf** (raw mnemonic; per-ISA; witnessed or tainted).

Below both tiers sits **Level 0** — plain ZER arithmetic (`+`, `&`, `<<`, `>>`).
Level 0 is *not a leaf*: it emits ordinary C and is already fully safe (overflow
wraps under `-fwrapv`, over-width shift = 0; see CLAUDE.md "No undefined behavior").
It is the lowest primitive but needs no asm-safety machinery at all.

The split is objective, not stylistic. The boundary is **"does it compile under
`-ffreestanding`"**: `__builtin_clz` / `__builtin_add_overflow` / `__builtin_bswap`
DO (Tier A), `RDMSR` has no portable builtin and does NOT (Tier B). Tier A covers
~95% of operations by count (computational / ALU / atomic / bit-query). Tier B is
~36 ops (control-state writes, I/O ports, privilege transitions, inspection /
cache / TLB), per-ISA.

### 4.2 Tier A leaves — derived from a GCC builtin

A Tier-A leaf is *defined by* a GCC builtin. There is **no raw asm and no per-ISA
work**: GCC selects the real instruction (and the right sub-arch instruction under
`-march`) and the emitted C is portable across every backend GCC supports. The leaf's
effect row is **DERIVED** from the builtin's documented C11/GCC contract via **ONE
audited derive-table** — a one-time mapping `builtin-name -> category bitset`, audited
once against the contract, not re-audited per binding.

Tier-A leaves already ship in ZER as the 15 `@atomic_*` and 8 bit/bswap intrinsics
(CLAUDE.md "Atomic Intrinsics", "Bit Query / Byte Swap Intrinsics"). Their emission
is the existing builtin-routing surface in `emitter.c` (the `atomic_*` dispatch at
`emitter.c:8355` onward; the bit-query dispatch at `emitter.c:8303` onward). Under
this design those emission sites stay; what is ADDED is the derive-table that reads
the builtin name and yields the categories, replacing any author-declared categories.

**Derive-table shape (illustrative — proposed surface, not yet shipped):**

```
builtin                       categories (bitset)                        notes
----------------------------- ------------------------------------------ ---------------------------
__builtin_add_overflow        value_in_value_out, produces_carry,        carry-out is the bool result
                              no_memory_effect, clobbers_flags
__builtin_clz / __builtin_ctz value_in_value_out, no_memory_effect,      UB at input 0 -> requires_nonzero
                              requires_nonzero
__builtin_popcount/parity/ffs value_in_value_out, no_memory_effect       defined at 0 -> NO requires_nonzero
__builtin_bswap{16,32,64}     value_in_value_out, no_memory_effect       pure
__atomic_fetch_add (etc.)     reads_mem(w,ord), writes_mem(w,ord),       ordering value-intrinsic (4.5)
                              value_in_value_out
__atomic_compare_exchange_n   reads_mem(w,ord), writes_mem(w,ord),       lock-free-width guard (4.4)
                              value_in_value_out, requires_nonzero(ptr)
__atomic_thread_fence         memory_barrier(ordering)                   fence path, ordering-parameterized
```

Two soundness columns are mandatory in this table and must be derived from the
contract, never from the author:

#### 4.4.a `requires_nonzero` — the `clz(0)` / `ctz(0)` UB column

GCC documents `__builtin_clz`/`__builtin_ctz` as **undefined when the input is 0**
("If x is 0, the result is undefined"). On x86 the underlying BSF/BSR leaves the
destination untouched, leaking garbage; without BMI1 (LZCNT/TZCNT) the UB stands.
ZER already guards this: `@ctz`/`@clz` emit a zero-test that returns the type width
on input 0 — see `emitter.c:8324-8331` (the `_zer_bz` temp + `== 0 ? width :
__builtin_...`). `@popcount`/`@parity`/`@ffs` are defined at 0 by GCC and carry NO
guard (`emitter.c:8332-8336`). The derive-table must encode this distinction as a
`requires_nonzero` column: the same builtin family splits by UB-at-zero, and the
column is exactly what the guard (or the caller's proven-nonzero range, Model 2 VRP)
must satisfy. A wrong column = a re-introduced UB hole, so this column is part of the
one-time audit, not author input.

#### 4.4.b lock-free-width guard — the 16-byte CAS column

`@atomic_cas` on a 16-byte operand emits `__atomic_compare_exchange_16`. On most
targets that is **not** lock-free: GCC lowers it to an external `libatomic` call,
which does not exist under `-ffreestanding` (link error) and is not interrupt-safe.
This is the same hazard documented for atomics generally — "GCC `__atomic` builtins
... On platforms WITHOUT them, GCC calls `libatomic` which may not exist for embedded
targets" (docs/ASM_ZER-LANG.md:53-68) — sharpened to the 16-byte case. The derive
column is a **lock-free-width guard**: widths 1/2/4/8 derive normally; width 16 (and
any width whose `__atomic_*_N` is not lock-free on the target) is rejected at the
leaf, not silently shipped as an external call. ZER already constrains atomic operand
width to 1/2/4/8 bytes and warns on 32-bit libatomic (CLAUDE.md "Atomic width
validation"); the lock-free-width guard makes 16-byte CAS a leaf-level error rather
than a `-ffreestanding` time bomb.

**Forbidding author categories for builtin-backed ops.** Because Tier-A categories
are DERIVED, an author may NOT declare them. A leaf that names a builtin and also
hand-writes a category bitset is a **compile error** (planned test
`bind_on_builtin_op.zer`). The single source of truth is the derive-table; allowing a
parallel author declaration would re-open the "verified against a lie" gap that
Tier-A exists to close.

**Illustrative Tier-A leaf (proposed surface, not yet shipped):**

```
# A leaf is DEFINED by the builtin; categories are derived, not written.
leaf @arith_add_with_carry(u64 a, u64 b) -> (u64 sum, bool carry)
    = builtin __builtin_add_overflow;
    # effect row DERIVED: value_in_value_out, produces_carry,
    #                     clobbers_flags, no_memory_effect
    # author MUST NOT restate categories -> bind_on_builtin_op.zer = compile error
```

### 4.3 Tier B leaves — per-ISA raw asm, witnessed or tainted

A Tier-B leaf is the *only* construct in which a real mnemonic appears. It exists
solely for ops OUTSIDE C's abstract machine that GCC has no portable builtin for:

- **control-state writes** — CR0/CR3/CR4, RDMSR/WRMSR, XSETBV
- **I/O ports** — IN/OUT
- **privilege transitions** — SYSCALL/SYSRET, IRET, ECALL, SVC, SMC, hypercall
- **inspection / cache / TLB** — CPUID, RDTSC, INVLPG, WBINVD, CLWB, CLFLUSHOPT

These map to ZER's existing privileged intrinsic batches D-Alpha-3/4/9..14 (CLAUDE.md
"Interrupt Control", "MSR/CR/XCR0 Access", "Privileged Mode Transitions", etc.). Each
CONFIGURES the machine; no portable builtin can express "switch address space"
(`@cpu_write_cr3`) or "read a model-specific register" (`@cpu_read_msr`).

A Tier-B leaf wraps its mnemonic in the existing **structured asm** form (CLAUDE.md
line 265; docs/ASM_ZER-LANG.md "Extended Inline ASM"): a block with `instructions:`,
`inputs:`, `outputs:`, `clobbers:`, and the **mandatory `safety:` string (>= 30
chars, S4 audit-trail rule)**. The leaf additionally DECLARES its effect row. That
declaration cannot be taken on the author's word: it is either **WITNESSED** (Section
on conformance witness — for decidable categories, on x86, via QEMU) or **TAINTED**
(Section on the named floor — for categories that are neither derivable nor decidably
observable). The classic under-declared-clobber bug — a leaf that touches `rax` but
forgets to list it — is exactly the witness sweet spot: a sentinel-fill of all
registers, run, read-back catches it (something GCC and Rust cannot, because they
trust the author's clobber list).

**Illustrative Tier-B leaf, x86 privileged (proposed surface, not yet shipped):**

```
# RDMSR — no portable builtin; CONFIGURES the machine (reads a model-specific reg).
leaf @cpu_read_msr(u32 msr) -> u64 for arch x86_64
    asm {
        instructions: "rdmsr"
        inputs:   { "ecx" = msr }
        outputs:  { lo = "eax", hi = "edx" }     # result = (hi << 32) | lo
        clobbers: { }
        safety: "RDMSR reads MSR[ecx]; faults #GP if CPL!=0 or MSR unimplemented"
    }
    declares {
        changes_privilege: requires_cpl0,         # WITNESSABLE (CPL readback)
        reads_mem: no,  writes_mem: no,
        clobbers_register: { },                   # WITNESSABLE (sentinel sweep)
        clobbers_flags: false,
    };
    # changes_privilege + clobbers_* -> witnessed under qemu-system-x86_64.
    # No ordering/provenance categories here -> nothing tainted for this leaf.
```

ZER's existing operand-level safety stays ACTIVE across the Tier-B operand boundary
(unlike Rust, which goes blind inside `unsafe { asm!() }`): Z-rules Z1-Z8/Z11/Z12
keep UAF / move / VRP / provenance / escape / qualifier / MMIO tracking live through
asm `inputs:`/`outputs:` (CLAUDE.md line 267-271). Register NAME validity is delegated
to GCC (the assembler errors on a bad name); reserved registers (sp/bp/pc) are
structurally banned as operands (docs/ASM_ZER-LANG.md "Reserved register rejection").
So even a Tier-B leaf is not a total blind spot for value-level program-consequence —
the witness/taint machinery is only about the *effect row* (the machine-config side),
and operand values remain owned by the existing checker.

### 4.4 Sub-architecture is handled AT THE LEAF

Sub-arch divergence does not leak above the leaf line. A single op can have multiple
leaves — one per sub-arch — selected by GCC's target macros at compile time, exactly
as ZER already emits dual-path atomics (docs/ASM_ZER-LANG.md:70-96, "Dual-Path
Emission" gated on `__ARM_FEATURE_LDREX` / `__ARM_ARCH_6M__` / `__riscv_atomic` /
`__AVR__`). The canonical case is Cortex-M0 vs M4: M3/M4/M7 have `ldrex`/`strex`
(native atomics, real effect row), M0/M0+ have neither and must fall back to
interrupt-disable.

**Illustrative per-sub-arch leaf, Cortex-M0 vs M4 (proposed surface, not yet shipped):**

```
# @atomic_cas on ARM: the leaf picks its body by GCC sub-arch macro.
leaf @atomic_cas(*shared u32 p, u32 expected, u32 desired) -> bool for arch arm
    when __ARM_FEATURE_LDREX:           # Cortex-M3/M4/M7 — native
        = builtin __atomic_compare_exchange_n;   # Tier-A derive here
        # effect row DERIVED: reads_mem(4,seq_cst), writes_mem(4,seq_cst),
        #                     value_in_value_out
    when __ARM_ARCH_6M__:               # Cortex-M0/M0+ — no ldrex/strex
        asm {
            instructions: "mrs %0, primask\n cpsid i ... cpsie i"
            safety: "M0 has no LL/SC; CAS emulated under interrupt-disable critical section"
        }
        declares {
            changes_privilege: toggles_interrupt_flag,   # TAINTED (no QEMU here)
            reads_mem: yes, writes_mem: yes,
            clobbers_flags: true,
        };
```

The caller writes `@atomic_cas` once. Which leaf body compiles is decided by the
`#if defined(...)` macro GCC evaluates per target. The M0 leaf's
`toggles_interrupt_flag` is a non-x86 privileged effect that **cannot be witnessed on
this host** (env limit: only `qemu-system-x86_64` is present; no
`qemu-system-aarch64`) — so it stays TAINTED, marking any importer. The M4 leaf is
Tier-A-derived and clean. Both presentations of the same op are correct; sub-arch is
fully absorbed at the leaf and never seen by composition.

### 4.5 Effect rows — what a leaf declares/derives

An **effect row** is the leaf's category set, drawn from Option E Layer-1's CLOSED
category vocabulary (asm_lang_zer_safe.md §1.7): `clobbers_flags`,
`clobbers_register(set)`, `no_memory_effect`, `reads_mem(width,ordering)`,
`writes_mem(width,ordering)`, `requires_aligned(n)`, `requires_nonzero`,
`memory_barrier(ordering)`, `produces_carry` / `consumes_carry`, `changes_privilege`,
`control_flow`, `provenance_clear_on_output`, `value_in_value_out`. The row is the
type-level interface (ERBT — Effect-Row Binding Types, Section 5): the checker folds
child rows into a parent row by closed rules. A leaf's row is the *base case* of that
fold.

**The fold-vocabulary exclusion that the leaf must respect.** `clobbers_register`,
`clobbers_flags` fold by UNION/OR (sound); `requires_aligned` folds by MAX (sound).
**Ordering / `memory_barrier` does NOT fold soundly** — max-ordering-of-children is
unsound. This is ZER's own evidence, not a hypothesis: Session-G Phase 3 in-block
ordering enforcement was ABANDONED because it false-positived the canonical libpmem
CLWB+SFENCE idiom (CLAUDE.md:1294-1296). Consequence at the leaf: a leaf may declare
ordering on its memory categories, but ordering is **value-intrinsic only** — it lives
on `reads_mem`/`writes_mem`/`memory_barrier` as a property of THAT op and is never
folded positionally. Positional `memory_barrier` is excluded from the composition
fold vocabulary entirely. (See the observability spectrum: clobber = sound-fold +
observable-witness = best; ordering = unsound-fold + unobservable = worst, stays
floor.)

**Today's ordering caveat the derive-table must encode.** ZER's atomic value-ops
hardcode `__ATOMIC_SEQ_CST` — `emitter.c:8364` (`__atomic_load_n(..., __ATOMIC_SEQ_CST)`),
`:8368` (store), `:8375` (CAS). The fence path IS ordering-parameterized
(`emitter.c:3098-3102`, the three `__atomic_thread_fence(__ATOMIC_{SEQ_CST,RELEASE,
ACQUIRE})` arms). So "ordering derived from the builtin's contract" is FALSE today for
value-ops. Until the ordering parameter is wired through, the derive-table must
**conservatively declare `ordering = seq_cst`** for those ops — the actually-emitted
truth — rather than a weaker ordering the code does not produce.

### 4.6 New-ISA bring-up: write the leaf set once, compositions follow for free

The leaf line is where the flexibility/maintainability decoupling lives. Bringing up a
new ISA = writing **its leaf set once** (bounded: the ~36 Tier-B ops it actually
needs, plus the Tier-A ops which are mostly free because GCC already retargets the
builtins). Tier-B leaves are **demand-driven and fail-closed**: implement a leaf only
when firmware on that ISA uses that op; an unimplemented privileged op is a compile
error, never a silent hole. The burden tracks YOUR usage (tiny, slow — CR3/MSR/port/
IRET/IF are decades-old, implemented once; vendor extensions like FSGSBASE/SMAP/PKE/
CET arrive slowly, per-generation), not the whole ISA catalog. This deliberately does
NOT trip the rejected "proactive per-instruction/per-vendor whole-ISA table" pattern
(the DCA/DSC disqualifier) — those needed the entire opcode table up front or be
fail-open; demand-driven + fail-closed + one-tiny-leaf-at-a-time does not.

Once a new ISA's leaf set exists, **every composition (Section 5) works on it for
free** — composed ops contain no asm and no per-ISA branches; they fold the new
leaves' rows by the same closed rules. Flexibility (new composed ops) and per-ISA cost
(new leaf sets) are independent axes. This is the concrete payoff of the adopted
Effect-Row Composition design: per-ISA bring-up is a bounded one-time leaf audit, and
the open-ended expressive surface (composition) costs zero per ISA.

### 4.7 Why the leaf is the floor (and what it is NOT)

The leaf is the audited primitive unit precisely because it is where trust is *placed
directly* rather than derived:

- **Tier A** trust anchors in the **GCC/C11 builtin contract** (one audited table,
  shared, auditable without silicon — but neither it nor GCC is the die).
- **Tier B** trust anchors in either a **QEMU/TCG witness** (for decidable categories
  on x86) or an **explicit taint marker** (for ordering, non-x86 privileged asm on
  this host, and `provenance_clear_on_output`).

What the leaf line does NOT claim: it does not verify silicon semantics (errata,
microarchitecture — the named hardware-consequence floor, CLAUDE.md "program- vs
hardware-consequence"), and a sound fold above a *lying* leaf will faithfully
propagate the lie green. Composition safety alone is therefore insufficient; safety is
relative to leaf correctness. That is the honest statement of the design: the floor is
not eliminated, it is **concentrated** into a small, centralized, per-ISA leaf set plus
the one-time derive-table and fold-rule audit — reviewable without a die, and far
smaller than Option E's O(bindings) distributed prose checklists.


## 5. Composition and Effect-Row Types (the flexible layer)

### 5.1 What this layer is, and why it exists

Sections 3-4 establish the **leaf** as the audited primitive unit: a Tier-A leaf is a
GCC builtin (`__builtin_add_overflow`, `__atomic_*`, `__builtin_clz`, `__builtin_bswap`)
whose effect row is *derived* from the builtin's documented contract; a Tier-B leaf is
raw asm for an op outside C's abstract machine (CR3/MSR/port/IRET/CPUID...), whose effect
row is *witnessed* (decidable categories) or *declared+tainted* (the rest). Leaves are
**small** (~36 Tier-B ops, ~95% of ops by count covered by Tier A) and **per-ISA**, and
they are the **only** place raw asm appears.

Leaves alone are not enough for a real firmware codebase. Firmware authors continually
need *new* operations that are not single instructions: a 128-bit add-with-carry chain, a
"disable interrupts, read a control register, restore" critical-region helper, a
masked-load-then-popcount, a CAS-loop fetch-max. Under the rejected Intrinsic-Maximalism
design these would each require a **core release** (extend the frozen taxonomy, ~1-2
ops/decade) — that is the flexibility cost that disqualified it. Under the **Effect-Row
Composition** design (the flexibility-weighted winner, ADOPTED), authors get a third
construct:

> **Composition: an author recombines existing leaves and ops into a new named operation
> with NO asm and NO core release.** The composed operation's effect row is *derived
> mechanically* from its children by closed fold rules. The author writes only ZER-level
> glue (Level-0 arithmetic, control flow, calls to leaves/ops); they never write a
> mnemonic.

This is the layer that delivers the **flexibility** half of the triangle
(flexibility / safety / maintainability — pick two; Composition pays the third with a
small leaf-audit + a named floor past the leaf boundary). New operations cost an author a
local edit, not a language revision. A new ISA's bring-up cost is "write its leaf set
once" (bounded); after that **every existing composition runs on it for free** — flexibility
and per-ISA cost *decouple*.

### 5.2 The structural gate: the composed-binding grammar has no mnemonic production

The single load-bearing structural fact of this layer:

> **You literally cannot write freeform asm in a composition.** The composed-binding
> grammar production for a composition body has **no mnemonic / no instruction string**.
> Raw asm is reachable only through `NODE_LEAF_BIND` (Tier-B), which is itself gated behind
> a closed privileged allow-list (§6 / STEP 4 of the plan).

This is deliberately the same shape as ZER's existing CLOSURE gates: there is *no path*
to a typed raw pointer except `@inttoptr`+mmio (anything else is a compile error); there is
*no path* to raw asm in a composition except dropping down to a leaf (which is allow-listed,
not freeform). The gate is grammatical, not a lint — a composition that tries to embed a
mnemonic does not parse into a `NODE_COMPOSED_BIND` at all. Concretely (planned AST split,
STEP 3):

- `NODE_LEAF_BIND` — **has** an `instructions` field (Tier-B raw asm); subject to the
  privileged allow-list fence; subject to the conformance witness (§4).
- `NODE_COMPOSED_BIND` — **has no** `instructions` field. Its body is a list of child
  calls (other ops / leaves / Level-0 ZER) plus glue. The verifier folds children's
  effect rows; it never parses an asm string here because there is none to parse.

The `@bind` keyword itself is fenced (STEP 4): `@bind` on an op not on the closed
privileged allow-list = **compile error** (test fixtures `composed_bind_mnemonic.zer`,
`bind_on_builtin_op.zer`). Binding a builtin-backed (Tier-A) op by hand is forbidden because
its row is *derived*, not declared — declaring categories for it would let an author state a
lie the deriver would otherwise have gotten right.

### 5.3 Effect rows and the DERIVE-vs-DECLARE direction of trust

An **effect row** is the multiset of categories an operation carries, drawn from the
**closed, ZER-owned category vocabulary** (`docs/asm_lang_zer_safe.md:2270-2282`):

```
clobbers_flags, clobbers_register(name),
reads_mem(width, ordering), writes_mem(width, ordering),
requires_aligned(n), requires_nonzero(operand), requires_in_range(operand, lo, hi),
memory_barrier(acquire | release | seqcst),
produces_carry, consumes_carry,
changes_privilege(from, to),
control_flow(returns | jumps_to | calls),
provenance_clear_on_output(operand),
value_in_value_out, no_memory_effect, no_flag_effect
```

The category set is **closed** (extended only via a ZER release) — this is the kind-difference
from SPARK that the main doc elevates to load-bearing: a binding contract "can be wrong in
*exactly one way*: the declared categories do not match what the asm body actually does"
(`docs/asm_lang_zer_safe.md:2488`). There is no free-form predicate to mis-state.

For compositions the row is **DERIVED** (never trusted from the author):

1. The verifier computes each child's effect row (a leaf's row, or a recursively-derived
   sub-composition's row).
2. It **folds** the children's rows by the closed fold rules of §5.4 into one row for the
   composition.
3. The author MAY write an **EXPECTED** row annotation. The checker compares
   **declared vs derived**. Mismatch = **COMPILE ERROR** (illustrated in §5.6). The
   expected row is a *documentation/assertion convenience that the compiler proves*, never an
   input to the analysis — the derived row is authoritative.

This inverts Option E's trust direction. Option E checked Layer-3 *call sites* against the
binding's **DECLARED** categories, so a wrong declaration meant "Layer-3 verified against a
lie" (green, wrong on silicon). Composition **repoints Layer-3 from DECLARED to DERIVED**
(STEP 3): above the leaf line, nothing is declared — it is computed. The floor therefore
*concentrates into the leaf set* and does not leak per-composition.

### 5.4 Fold rules and fold soundness (the part that must not be wrong)

A fold rule is sound iff the composition's derived category is **at least as strong /
at least as conservative** as the true effect of running the children in sequence. Sound
folds:

| Category | Fold operator | Why sound |
|---|---|---|
| `clobbers_register(name)` | **UNION** of children's clobber sets | If any child clobbers a register, the composition clobbers it. Register write-sets are **structural, not data-dependent** — a sound over-approximation never drops a real clobber. |
| `clobbers_flags` | **OR** | If any child writes flags, the composition writes flags. |
| `requires_aligned(n)` | **MAX** of children's `n` | The strictest child precondition is the composition's precondition; satisfying MAX satisfies all. |
| `control_flow(...)` | **OR** (lattice join of `returns`/`jumps_to`/`calls`) | If any child can jump/call/not-return, the composition can. |
| `produces_carry` / `consumes_carry` | by the carry data-flow of the composition body (intrinsic to the op chain), not a positional fold | Carry is a value flowing through operands, tracked like any value. |
| `value_in_value_out`, `no_memory_effect`, `no_flag_effect` | **AND** (a composition is pure / flag-free / memory-free only if **all** children are) | Removing a guarantee is the conservative direction. |

`clobbers_register` and `clobbers_flags` folding by union/OR is the high-confidence corner
of the **observability spectrum**: sound fold (this section) **times** observable witness
(§4 sentinel-sweep) = the best-achievable category. A category's safety-achievability =
(fold soundness) × (witness observability); clobber scores high on both.

**The category with NO sound fold — EXCLUDED:**

> **`memory_barrier` / ordering does not fold soundly and is therefore EXCLUDED from the
> composition fold vocabulary.** Ordering stays **value-intrinsic only** (a property of a
> single `reads_mem`/`writes_mem`/atomic op's own `ordering` parameter), never a positional
> category the fold combines across children.

The tempting-but-wrong rule is "the composition's barrier = MAX-ordering-of-children" (treat
`seqcst > release/acquire > relaxed` as a lattice and join). It is **unsound**, and ZER has
*its own first-hand evidence* of exactly this failure mode:

> Session G (System #30 atomic ordering): Phases 1-2 done (plumbing + classification in
> vendored tables); **Phase 3 in-block enforcement ABANDONED** (false-positived the
> canonical multi-block CLWB+SFENCE libpmem idiom). Lesson: don't ship enforcement that
> rejects valid code patterns.
> — `CLAUDE.md:1294-1297`

The libpmem persistence idiom is `CLWB <addr>` (cache-line write-back, a *weak*, unordered
flush) followed later, possibly in a *separate block*, by a single `SFENCE` that orders the
whole batch. A positional max-of-children fold sees the `CLWB` as carrying weak/no ordering
and the `SFENCE` as `seqcst`, and either (a) demands a fence the author correctly placed
elsewhere (false positive — exactly what killed Session-G Phase 3), or (b) where used as a
*witness*, certifies a too-weak fence as adequate because QEMU-TCG flattens to TSO and hides
the gap (false confidence — §4). Ordering is therefore the **low-low** corner: unsound fold
× unobservable witness = the **irreducible-hardest** category. It is not folded, not witnessed,
and stays in the named **TAINT** floor (§7). It is the empirical confirmation that ZER's
litmus/concurrency analysis already reached: ordering is the one category no structural
mechanism closes.

Note the asymmetry: `requires_aligned` is excluded from *witnessing on x86* (AC-off ⇒ no
trap) but is **soundly folded** by MAX and remains a real precondition checked at the Level-3
call site; ordering is excluded from *both* fold and witness. The two exclusions have
different causes and different residual status.

### 5.5 ERBT — Effect-Row Binding Types (the type-theoretic backbone)

**ERBT** is the type system in which the above lives. Categories are treated as **rows**
(in the record-/effect-row sense: an unordered, label-keyed bag of category fields). An
operation's type is `(value signature) carrying (effect row)`. ERBT gives the layer three
properties:

1. **Compositionality** — the effect row of a composite is a pure function of its children's
   rows under the §5.4 fold operators. This is what makes "no core release per new op"
   possible: adding a composition adds no axioms, only an application of existing fold rules.
2. **Inference** — the author need not annotate the row; the checker *infers* it bottom-up
   from leaf rows (the EXPECTED annotation, when present, is checked, not required).
3. **Structural compile-gate** — a row mismatch is a **type error at compile time**, not a
   CI finding. This is the CLOSURE-philosophy stance: "run a CI test" is the trust-the-user
   model ZER *rejects*; ERBT makes the wrong composition **fail to compile**.

ERBT is **one-time maintenance**: the fold-rule table (§5.4) and the closed category
vocabulary (§5.3) are defined once and audited once. Adding the thousandth composition
exercises the same table as the first. This is the O(leaves) + O(1)-fold-proof cost shape
that replaces Option E's O(bindings) distributed-prose checklists.

The check the verifier performs at a composition is a **row subtype** test. Let `D` = derived
row, `E` = author-expected row. The composition is well-typed iff `E ⊒ D` *and* `D ⊒ E` in
the relevant direction per category, i.e. the author's stated row must **exactly match** the
derived row component-wise (over-claiming a clobber the body doesn't have is also rejected, so
the EXPECTED annotation stays honest documentation). For *uses* of the composition at Level-3,
the standard subsumption applies: a call site must satisfy every `requires_*` precondition in
the derived row, and must treat every `clobbers_*` / effect as actually occurring.

### 5.6 Illustrative compose surface and a mismatch compile error

*(illustrative — proposed surface, not yet shipped; STEP 3 wires `NODE_COMPOSED_BIND`.
Syntax not final; the load-bearing facts are: no `instructions` field, derived row,
declared-vs-derived check.)*

A 128-bit add built by chaining the `@arith_add_with_carry` op (itself a Tier-A leaf backed
by `__builtin_add_overflow`, row derived from the builtin contract):

```zer
// (illustrative — proposed surface, not yet shipped)
compose add128(a: u128, b: u128) -> { result: u128, carry: u1 } {
    lo = @arith_add_with_carry(a.low,  b.low,  0)      // child 1
    hi = @arith_add_with_carry(a.high, b.high, lo.c_out) // child 2
    return { result: u128_from(hi.result, lo.result), carry: hi.c_out }
    // NOTE: no `instructions:` block exists in this grammar. None can be written.
}
// Derived row (folded over the two children):
//   { value_in_value_out, produces_carry, consumes_carry, clobbers_flags,
//     no_memory_effect }   <- clobbers_flags by OR; carry by the chain's data-flow.
```

A composition that states an EXPECTED row contradicting the derived row — the canonical
compile error this layer produces:

```zer
// (illustrative — proposed surface, not yet shipped)
compose disable_irq_read_msr(idx: u32) -> u64
  expects { no_memory_effect, no_flag_effect, value_in_value_out } {   // <-- author's claim
    @cpu_disable_int()                 // Tier-B leaf: changes_privilege-adjacent, clobbers IF
    v = @cpu_read_msr(idx)             // Tier-B leaf: declared+tainted (ordering/priv)
    @cpu_enable_int()
    return v
}
```

```
error[ERBT-row-mismatch]: composition `disable_irq_read_msr` declared an effect row
        that does not match the row derived from its children
  --> firmware/cpu.zer:14:3
   |
14 |   expects { no_memory_effect, no_flag_effect, value_in_value_out }
   |            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   |
   = derived row (folded over children @cpu_disable_int, @cpu_read_msr, @cpu_enable_int):
       { clobbers_flags(IF), changes_privilege(...), value_in_value_out }   [TAINTED: ordering of @cpu_read_msr]
   = declared `no_flag_effect`   but child @cpu_disable_int folds clobbers_flags  -> contradiction
   = declared `no_memory_effect` is consistent (kept)
   = note: the derived row is authoritative; remove the false guarantees or change the body.
   = note: this composition is TAINTED because it transitively uses @cpu_read_msr, whose
           ordering category is author-declared and unverifiable on this host (see §7).
```

The author cannot suppress this by re-declaring; the only resolutions are to fix the body or
correct the EXPECTED annotation to the truth. There is no flag to demote the error to a
warning — that would reintroduce the trust-the-user model.

### 5.7 Closure: what this layer proves, and what it does not

The composition layer carries exactly **one** theorem and is honest about its one limit:

- **Theorem (composition soundness).** Given (i) safe leaves and (ii) sound fold rules,
  *everything composed above the leaf line is safe*: every derived row is a faithful
  over-approximation of the composite's true effect, so every Level-3 use is checked against
  the truth, not a declaration.
- **The limit (faithful propagation of a lie).** Composition safety **alone is not
  sufficient**. A sound fold *faithfully propagates* a lying leaf — if a Tier-B leaf
  under-declares a clobber, the union-fold dutifully omits it from every composition that uses
  it. **Safety is RELATIVE TO leaf correctness.** This is by design: the floor
  *concentrates* into the finite leaf set (§4 witness for decidable categories; §7 taint for
  the rest) instead of smearing across O(bindings) prose checklists.

This is the same closure shape ZER already relies on elsewhere: memory safety is closure over
the allocation primitives; type safety is closure over the conversion intrinsics; concurrency
safety is closure over the sync primitives. In each, a finite **primitive set** is audited
once and a **propagation rule** ("P holds for all programs") carries it everywhere. Here the
leaves are the primitive set and the §5.4 fold is the propagation rule. The genuine shrink the
main doc claims is real and measurable: O(bindings) distributed prose → O(leaves) +
O(categories) centralized, reviewable-without-silicon audit.

**Cross-references.** Leaf definition and the Tier-A/Tier-B split: §3. Conformance witness for
decidable Tier-B categories: §4. The privileged allow-list that fences `@bind`: §6. The named
TAINT floor for ordering / non-x86 privileged asm / `provenance_clear_on_output`: §7.
Implementation: `option_e_plan.md` Phase 1 (delete per-arch tables) precedes all of this; the
composition machinery is STEP 3 (`NODE_COMPOSED_BIND`, fold OR/MAX, exclude positional
`memory_barrier`, repoint Layer-3 to DERIVED).


## 6. The closure argument and floor concentration

This section states the theorem that makes Effect-Row Composition safe, proves
its shape against ZER's existing closure machinery, and is honest about exactly
what the theorem does and does not buy. A fresh reader should leave knowing
*which artifacts are trusted, how few of them there are, and why everything
above them is safe for free.*

### 6.1 The core theorem

> **Safe leaves + sound fold ⇒ everything composed above the leaf line is safe.**

Unpacked into its two premises and one conclusion:

- **Leaf correctness (premise 1).** Every leaf's declared effect row is the
  truth about that leaf. For a **Tier-A leaf** (a GCC builtin —
  `__builtin_add_overflow`, `__atomic_*`, `__builtin_clz/ctz/popcount`,
  `__builtin_bswap`) the row is *derived* from one audited table mapping the
  builtin's documented contract to a category bitset; correctness reduces to
  "the table faithfully transcribes GCC's documented contract." For a **Tier-B
  leaf** (raw asm for control-state / I/O / privilege / inspection ops —
  ~36 ops, the only place a real mnemonic appears) the row is *witnessed* for
  the decidable categories (clobbers, privilege, nonzero, trapping alignment)
  and *declared+tainted* for the rest; correctness reduces to "the witness
  observed what the row claims" plus "the tainted residue is named, not assumed
  green."

- **Fold soundness (premise 2).** Each fold rule that combines children's rows
  into a composed op's row is sound: the composed row over-approximates the
  union of effects the children can actually have. `clobbers_register` and
  `clobbers_flags` fold by **UNION/OR** (a composition clobbers a register iff
  *some* child clobbers it — sound by construction). `requires_aligned(n)` folds
  by **MAX** (the strictest child constraint dominates — sound). Categories that
  do **not** fold soundly are *excluded from the fold vocabulary entirely*:
  positional `memory_barrier` / ordering is excluded because "max-ordering of
  children" is unsound (see §6.5). The fold vocabulary is therefore a closed,
  audited set of rules, each individually proven monotone over its lattice.

- **Conclusion.** Any operation written with the `compose` form — which has
  **no mnemonic production** in its grammar, so it can only recombine existing
  leaves and Level-0 ZER expressions — has an effect row that is correct by
  the soundness of the fold applied to correct children. By induction over the
  composition tree (leaves at the base, folds at each internal node), the row
  of *any* composed op is correct. The Layer-3 static verifier then checks
  Layer-3 call sites against this **derived** row (Phase 3 repoints Layer 3 from
  the author-DECLARED row to the DERIVED row), so call-site checking inherits
  the correctness.

This is the whole safety story for the flexible layer: authors compose freely,
the checker folds, and no new trusted artifact is created by any composition.

### 6.2 Composition safety alone is INSUFFICIENT — safety is relative to leaf correctness

The theorem is a conditional, and the antecedent matters. **A sound fold
faithfully propagates a lying leaf.** If a Tier-B leaf *declares*
`no_memory_effect` but its raw asm actually writes memory, the union/max fold
will dutifully compute a composed row that *also* omits the memory effect —
correctly, soundly, and wrong on silicon. The fold did its job perfectly; it
propagated a falsehood it was never asked to detect.

Therefore:

- Composition safety is **conditional**, not absolute. The statement is
  "compositions are as correct as their leaves," not "compositions are correct."
- This is *exactly* the failure mode Option E carried (`docs/asm_lang_zer_safe.md`
  §1.7): a binding whose asm does not honor its declared categories yields
  "Layer 3 verified-against-a-lie" — green in the checker, wrong on the die.
  Effect-Row Composition does **not** eliminate that failure mode. It
  **relocates** it.

### 6.3 Floor concentration — the actual contribution

What changes between Option E and Effect-Row Composition is *where the
unverifiable trust lives*, not whether it exists.

- **Option E (dispersed floor).** Every `@bind` is an independent author claim.
  "Does this binding's asm honor its declared categories?" is asked once per
  binding, answered by a manual checklist, and the answer is distributed across
  every binding library. Trust scales as **O(bindings)** prose checklists. Any
  one wrong declaration is a silent hole, and there is no single place to look.

- **Effect-Row Composition (concentrated floor).** Compositions create no new
  trust — their rows are derived. The *only* artifacts that can be wrong are:
  1. each **leaf's** effect row (≈36 Tier-B leaves + one Tier-A derive table),
     and
  2. each **fold rule** (a fixed, small set: union for clobbers, max for
     alignment; ordering deliberately absent).
  Trust scales as **O(leaves) + O(1) fold-proof.** The floor does not disappear;
  it **concentrates** into the leaf set and the fold-rule set, which are small,
  centralized, and reviewable *without a silicon die in hand*.

The complexity reduction is the genuine deliverable. ZER's residual-floor
accounting states it directly (see RESIDUAL FLOOR item 5): the audit surface
shrinks from *O(bindings) distributed prose* to *O(leaves) + O(categories)
centralized audit*. Concretely, ~95% of ops by count are Tier-A (derived from
one table, zero per-ISA work, GCC selects the instruction and handles sub-arch
via `-march`); only the ~36 Tier-B ops carry a per-ISA leaf, and only those
~36 plus the fold rules are the trusted set.

### 6.4 The same shape as ZER's existing closure argument

This is not a new kind of proof. It is ZER's **Closure Principle**
(`docs/primitives-data-races.md` §21.1, lines 1808-1816) applied to asm effect
rows. The principle, stated in the repo:

> "if the set of operations that can violate safety property P is closed under
> a finite set of compiler-visible primitives, and the compiler verifies each
> primitive against P, then P holds for all programs in the language."
> — `docs/primitives-data-races.md:1810-1813`

ZER already discharges this in four other domains; asm effect-correctness is the
fifth instance of the *identical* structure:

| Domain | Finite primitive set ("the leaves") | The fold / closure ("P holds for all programs") | Repo anchor |
|---|---|---|---|
| **Memory safety** | allocation primitives | access mechanism verified per-primitive; ownership intent via primitive choice | `primitives-data-races.md:72` |
| **Type safety** | conversion intrinsics | structural correctness verified per-intrinsic | `primitives-data-races.md:73` |
| **Concurrency** | sync primitives | every race-creating op goes through a finite primitive set | `primitives-data-races.md:828, 1346` |
| **ASM effect-rows (this design)** | leaves: ~36 Tier-B raw-asm ops + 1 Tier-A derive table | sound fold (union/max) over composition trees; positional ordering excluded | this document |

The mapping is exact: **the leaves are the finite primitive set; the fold is
the "P holds for all programs" closure.** In memory safety, closure is taken
over allocation primitives — verify each primitive's access mechanism and
*every* program built from them is memory-safe. In type safety, over conversion
intrinsics. In concurrency, over sync primitives. Here, over asm leaves: verify
each leaf's row and prove each fold rule sound, and *every* composed op has a
correct row. The fold *is* the mechanical step that turns "P holds for each
primitive" into "P holds for all programs," because composition is the only way
to build new ops and the fold preserves correctness across it.

Same closure, same grammar-level enforcement: just as the language as a whole
has "no path to the unsafe construct except a compiler-enforced gate" (no
integer-to-pointer cast except through `@inttoptr`+`mmio` —
`checker.c:5601-5608`), the `compose` form has **no mnemonic production** — you
literally cannot write freeform asm in a composition. Raw asm is reachable
*only* through a `NODE_LEAF_BIND` on a closed privileged allow-list (the
`@inttoptr`-shaped gate for asm). The leaf line is the gate; everything above it
is closed.

### 6.5 What must be true for the theorem to hold (the complete trusted set)

The theorem holds **iff** both of the following, and *nothing else*, are true.
These are the only trusted artifacts; everything else is mechanically derived
from them.

1. **Each leaf's effect row is correct.**
   - *Tier-A leaves:* the derive table faithfully transcribes each GCC builtin's
     documented contract into a category bitset. One audited table, audited once.
     **Caveat with a current anchor:** the derive table must declare the *truth
     that is actually emitted*, not the truth on paper. Today the emitter
     hardcodes `__ATOMIC_SEQ_CST` for atomic value-ops
     (`emitter.c:3128`, `emitter.c:3134`, `emitter.c:3146`, `emitter.c:3152`),
     so until ordering is wired through (Step 2) the derive table must
     conservatively declare `ordering = seq_cst` — the emitted reality — not the
     weaker ordering a programmable builtin could express. The fence path
     *is* already ordering-parameterized (`emitter.c:3098-3102`,
     barrier=SEQ_CST / barrier_store=RELEASE / barrier_load=ACQUIRE), which is
     why ordering-on-fences and ordering-on-value-ops must not be conflated in
     the table.
   - *Tier-B leaves:* for **decidable** categories the witness observed what the
     row claims (clobbers via sentinel sweep, privilege via CPL readback,
     nonzero via zero-trap, alignment where it actually traps — §on conformance
     witness), bound to `(op, ISA, category-profile, asm-hash)`; for
     **undecidable** categories (ordering, provenance, non-x86 privileged on
     this host) the declared value is *tainted*, i.e. carries a greppable floor
     marker and can never read as verified-green.

2. **Each fold rule is sound.**
   - `clobbers_register` / `clobbers_flags`: UNION/OR — sound (over-approximates).
   - `requires_aligned(n)`: MAX — sound (strictest dominates).
   - Ordering / positional `memory_barrier`: **excluded from the fold
     vocabulary**, because "max-ordering-of-children" is **unsound**. ZER has
     direct evidence: Session-G Phase 3 in-block ordering enforcement was
     ABANDONED because it false-positived the canonical multi-block
     CLWB+SFENCE libpmem idiom (`CLAUDE.md:1294-1297`,
     "Lesson: don't ship enforcement that rejects valid code patterns"). Ordering
     stays **value-intrinsic only** — it is never folded positionally, never
     synthesized by composition.

If both hold, the conclusion follows by structural induction over composition
trees. If either fails, the failure is localized: a wrong leaf row is wrong only
in compositions that include that leaf (and is the *named, concentrated* floor),
and a wrong fold rule would be caught by the one-time fold-soundness proof
before it ships.

Nothing else is trusted. In particular: register *name* validity is delegated
to GCC (the assembler errors on a bad name, not the checker); operand *values*
in/out remain under the existing ZER Z-rules (Z1-Z8/Z11 keep
UAF/bounds/qualifier/provenance tracking active *through* asm operands — unlike
Rust, which goes blind inside `unsafe asm`); reserved registers (sp/bp/pc) are
structurally banned as operands. None of these are part of the effect-row trust
set; they are independently closed by other mechanisms.

### 6.6 The author-mismatch gate (closing the leaf line from above)

The theorem says compositions cannot manufacture a *new* lie. The
**declared-vs-derived mismatch check** makes that property load-bearing at
authoring time: an author MAY state an EXPECTED row on a `compose` op, but the
checker computes the DERIVED row by folding the children and compares.

```
// illustrative — proposed surface, not yet shipped
compose @lib::set_and_test(...) expects { clobbers_flags }
// if the folded children also clobber rax, derived = { clobbers_flags,
//   clobbers_register(rax) }; declared ⊊ derived  ⇒  COMPILE ERROR
```

A mismatch is a **compile error**, not a warning. This means an author can never
under-declare a composition into looking safer than its leaves are — the only
way to change a composition's row is to change which leaves it uses, and each
leaf's row is already in the trusted set. The gate thus protects the *boundary*
of the closure: leaves are trusted-and-audited from below, compositions are
derived-and-checked from above, and the two meet exactly at the leaf line.

### 6.7 Honest restatement

The closure argument does **not** make asm safe in an absolute sense — no
language can verify silicon, and ZER does not claim to (program-consequence is
owned 100%; hardware-consequence is floor, `CLAUDE.md:13-14`). What it provides
is a *structural* guarantee with a *named, minimal* exception set:

- **Owned (program-consequence, 100%):** correctness of every composed op's
  effect row, given correct leaves — by sound fold, grammar-enforced (no
  mnemonic above the leaf line), checked at every Layer-3 call site against the
  derived row.
- **Floor (concentrated, not dispersed):** correctness of each leaf row and each
  fold rule — O(leaves) + O(1), centralized, reviewable without silicon. The
  undecidable residue (ordering, provenance_clear_on_output, non-x86 privileged
  asm on this host) is *tainted*: named, greppable, never green-by-default.

That relocation — from O(bindings) distributed prose checklists to O(leaves) +
O(1) fold-proof, with the irreducible remainder explicitly tainted — is the
entire claim of the closure argument. It is the fifth application of the same
Closure Principle ZER already uses for memory, type, and concurrency safety.


## 7. The conformance witness — verifying Tier-B leaves against reality

### 7.1. What the witness is for, in one sentence

Tier-B leaves are the only place raw asm appears (see §LEAVES). A leaf carries a
DECLARED effect row — the set of categories (`clobbers_register`,
`clobbers_flags`, `changes_privilege`, `requires_nonzero`, `requires_aligned(n)`,
`reads_mem/writes_mem`, ordering, ...) the leaf's author asserts about its asm.
Everything above the leaf line is verified *against that declaration*: the fold
rules propagate it through compositions (§COMPOSITION), and Layer-3 call sites are
checked against the DERIVED row. So if a leaf's declaration is a lie — the asm
under-declares its clobbers, or claims a privilege transition it doesn't make —
the entire tower above it is "verified against a lie": green at compile time,
wrong on silicon. This is the FLOOR that Option E (§1.7) named but left to a
library-author checklist.

The conformance witness is the mechanism that closes the gap *for the categories
that are decidable*. It runs per-category probes in QEMU, observes the leaf's
actual behavior, and compares observed-vs-declared. A leaf whose declaration
survives the probes is WITNESSED; a leaf that is unwitnessed or whose asm has
changed since it was witnessed is a COMPILE ERROR at any use site. The witness
does not (and provably cannot) cover every category — that residue stays TAINTED
(§TAINT). The witness is the second half of the closure theorem: safe leaves +
sound fold ⇒ safe tower, and the witness is how "safe leaf" stops being an
author's promise and becomes an observed fact for the decidable categories.

This is the subordinate, decidable-predicates-only role the standalone
QEMU-witness was DEMOTED to in the design fan-out (§THE DESIGN EXPLORATION). It
is not the gate; the structural composition grammar + fold are the gate. The
witness is what makes the *leaf inputs* to that gate trustworthy.

### 7.2. The witness binding — what gets certified

A witness is not "this op is fine." It is bound to a four-tuple so that any
drift in the inputs invalidates it:

```
witness = (op, ISA, category-profile, asm-hash)        (illustrative — proposed surface, not yet shipped)
```

- **op** — the semantic operation name (`@cpu_write_cr3`, `@port_out8`,
  `@cpu_read_msr`, ...). Tier-B ops are the ~36 outside C's abstract machine.
- **ISA** — the target (`x86_64`). The witness is per-ISA because the asm is
  per-ISA; an x86 witness says nothing about an aarch64 leaf for the same op.
- **category-profile** — exactly which categories were probed and what each
  probe concluded. A witness certifies a *set* of category claims, not a blanket
  pass. Categories not in the profile are NOT covered by this witness (they fall
  to taint).
- **asm-hash** — `blake2(asm-text)` of the leaf's mnemonic body. Any edit to the
  asm — even a clobber-list change — changes the hash and invalidates the
  witness. This is what makes "the leaf was witnessed" mean "*this exact asm* was
  witnessed," not "an asm with this name once was."

The witness record (illustrative — proposed surface, not yet shipped) is a
`.zerwitness` file, one record per `(op, ISA)`:

```
op           = @cpu_write_cr3
isa          = x86_64
asm_hash     = blake2(...)
qemu_version = qemu-system-x86_64 8.2.7
profile {
  changes_privilege   = OBSERVED   (CPL readback: probe ran at CPL=0)
  clobbers_register   = OBSERVED   {rax}        (sentinel sweep)
  clobbers_flags      = OBSERVED   none
  ordering            = NOT-PROBED  -> TAINTED
}
```

The `qemu_version` field is in the tuple-by-extension: the witness is only as
good as the emulator that produced it, so the producing QEMU is recorded and
becomes part of the residual floor (§RESIDUAL FLOOR (2): QEMU/TCG fidelity). In
this environment only `qemu-system-x86_64 8.2.7` is present, so every witness
this host can produce carries that string.

### 7.3. Fail-closed at firmware-compile time, no QEMU on that path

The structural precondition is: **use of an unwitnessed or hash-mismatched
Tier-B leaf = COMPILE ERROR.** This is fail-closed by construction — the default
state of a leaf is "not witnessed," and the only way out is a matching witness
record. An author who forgets to witness a new privileged leaf gets a hard error
at the first import, not a silent green (contrast the REJECTED disassemble-table
designs, which were fail-OPEN: an untabled mnemonic produced NO_CATEGORY and the
wrong declaration sailed through green).

Critically, the firmware-compile path does NOT run QEMU. The check at
firmware-compile time is purely:

1. recompute `blake2(asm-text)` of the leaf as it exists in source now,
2. look up the `.zerwitness` record for `(op, ISA)`,
3. error if the record is missing, or if its `asm_hash` ≠ the recomputed hash,
   or if a category the Layer-3 call site relies on is not OBSERVED in the
   profile.

All three are pure-CPU operations (hashing + a table lookup). This is what
**preserves cross-compile**: a firmware build targeting aarch64 on an x86 host,
or any host without the relevant system QEMU, still type-checks and emits C →
GCC normally. QEMU is needed only to *produce* a witness, never to *consume*
one. Witness production is a separate, lazy, opt-in step (`tool/asm_witness/`,
STEP 5 of the plan), re-run only when a leaf's asm changes (hash miss) or a new
leaf is added. The compile path reads the cached artifact; it never boots an
emulator. (Same shape as ZER's other artifact-cached checks: the witness file is
to a Tier-B leaf what a cached `FuncSummary` is to a function — computed once,
consumed cheaply, recomputed on input change.)

### 7.4. DECIDABLE categories and their probe strategies

A category is *decidable* when its honoring/violation is observable and
deterministic in QEMU. For those, the witness is real verification, not faith.
Each probe is generic to the CATEGORY, not the instruction — there are ~a dozen
categories in the closed vocabulary, so there are ~a dozen probe templates, and
every Tier-B leaf reuses them. This is the bounded-by-vocabulary property: probe
count scales with the closed category set, not with the instruction catalog.

**clobbers_register / clobbers_flags — the witness sweet spot.**
Strategy: sentinel-fill every general-purpose register (and the flags register)
with a distinct known value, execute the leaf's asm once, read every register
back. Any register whose value changed but is NOT in the declared
`clobbers_register` set is an UNDER-DECLARED clobber → witness FAIL. This is the
sweet spot because (a) register write-sets are *structural*, not data-dependent
— a single run observes the complete clobber set with near-certainty (an
instruction either writes `rax` or it does not; it doesn't depend on operand
values the way a branch target might), and (b) it catches the exact bug class
that GCC and Rust *cannot*: GCC trusts the programmer's clobber list inside
`asm` and Rust goes blind inside `unsafe { asm!() }`. The sentinel sweep is the
ground truth GCC never checks. (Note the asymmetry with composition: composition
clobbers fold by SOUND union — see §FOLD SOUNDNESS — so the high-soundness fold
meets the high-observability witness here; clobber is the high-high corner of
the OBSERVABILITY SPECTRUM.)

**changes_privilege — CPL readback in a ring0 stub.**
QEMU emulates privilege levels faithfully in *system* mode, so privileged ops
ARE probeable: boot a minimal ring0 stub, read the current privilege level
(CPL), run the leaf, read CPL again (or set up the transition target and observe
the level on the other side). A leaf declaring `changes_privilege` that leaves
CPL unchanged, or one that changes CPL without declaring it, fails. This is why
the env's lack of user-mode QEMU is not fatal for privileged ops: privileged
behavior is exactly what system-mode QEMU models, and the ring0 stub is the
harness for it.

**requires_nonzero — zero-trap.**
Strategy: feed the leaf a zero operand and observe whether it traps (or produces
the divide-error / undefined-result the category claims). A leaf declaring
`requires_nonzero` whose asm silently accepts zero has a wrong declaration. The
probe is a single boundary input.

**requires_aligned(n) — where it actually traps.**
Strategy: feed a deliberately misaligned address and observe a trap. The crucial
qualifier is *where it actually traps*: alignment is only decidable on a target
that raises a fault on misaligned access for that instruction. On x86 with
alignment-check (AC) off — the default — most accesses simply *succeed*
misaligned, so the probe observes no trap and CANNOT distinguish a correctly
`requires_aligned` leaf from a lying one. So `requires_aligned` is decidable on
ISAs/instructions that trap, and UNOBSERVABLE on x86-with-AC-off (§7.5).

### 7.5. UNOBSERVABLE categories — why a green witness can be false confidence

The witness is sound only for predicates QEMU can actually observe. Two
categories are unobservable in this environment, and for them a green witness
would be WORSE than honest author-trust, because it manufactures false
confidence. These stay TAINTED and are NEVER witnessed.

**Memory ordering — QEMU TCG flattens to TSO.**
QEMU's TCG (the dynamic translator used without KVM) does not reproduce weak
memory reordering; it effectively presents a strong (TSO-ish) model. So a leaf
that declares a strong fence and one that declares a too-weak fence both produce
*the same observed behavior* under TCG: green. A too-weak fence — the dangerous
under-declaration — witnesses GREEN. That is strictly worse than leaving ordering
to the author's word, because the green badge tells a reader "verified" when the
emulator was simply incapable of exhibiting the reordering that would expose the
bug. Ordering therefore stays value-intrinsic and TAINTED (this also matches the
fold: ordering does NOT fold soundly — max-ordering-of-children false-positived
the canonical libpmem CLWB+SFENCE idiom, which is why ZER ABANDONED in-block
ordering enforcement; see CLAUDE.md ~line 1295-1296. Ordering is the low-low
corner: unsound fold × unobservable witness — the irreducibly hardest category,
confirmed independently by the litmus/CWH angle in the fan-out.)

**Alignment on x86 — AC off ⇒ no trap.**
As in §7.4: with the alignment-check flag off (the normal state), x86 does not
trap misaligned accesses, so the alignment probe observes nothing. On x86 this
category falls to taint; on an ISA that traps it is decidable.

**provenance_clear_on_output** — not behaviorally observable by a register/CPL probe at all; stays TAINTED.

### 7.6. The existential-vs-universal lesson (why standalone witness was demoted)

This is the load-bearing soundness argument and must not be lost in any summary.

A probe **samples one execution path**: it runs the asm with one (or a few)
chosen inputs, in one emulated machine state, and observes the result. That
establishes an EXISTENTIAL fact — "on *this* run, the leaf did/did not change
CPL / clobber `rbx` / trap on zero."

A category is a **universal contract** — "for *all* executions, this leaf honors
`changes_privilege`." The gate consumes the witness as if it were universal:
Layer-3 verification treats a witnessed category as "declaration honored,
always."

The inference existential ⇒ universal is sound ONLY when the predicate is
deterministic and path-independent — i.e., when one observation generalizes
because the behavior cannot vary across inputs/paths. That holds for the
decidable categories in §7.4: a register write-set is structural (one run is
complete), CPL transition is deterministic, the zero-trap is a fixed boundary.
It does NOT hold for ordering: the reordering you need to observe is precisely
what TCG never produces, so "no reordering observed on this run" does not
generalize to "no reordering ever." A green witness on an unobservable category
asserts a universal from an existential that doesn't license it — false
confidence.

This is *why* the standalone QEMU-witness was DEMOTED from gate to a subordinate
role: as a standalone gate it would certify an existential ("some categories
observed on x86") and let the system read it as the universal ("declaration
honored"), which is sound only for decidable predicates. Restricting the witness
to decidable-x86 predicates is exactly the restriction that makes the
existential⇒universal step legitimate.

### 7.7. Environment reality and the per-category (not per-instruction) bound

**Env:** only `qemu-system-x86_64` (8.2.7) is present on this host — no
ARM/RISC-V system QEMU, no user-mode QEMU, no cross-compilers. Consequences:

- The witness can promote x86 decidable categories from taint to verified.
- Privileged ops are still probeable *because* QEMU emulates privilege in system
  mode (the ring0-stub strategy), even though there is no user-mode QEMU.
- Non-x86 privileged asm cannot be witnessed here at all → it stays TAINTED.
  This is an ENV limit, not an architectural one: installing
  `qemu-system-aarch64` + a cross-gcc would let the same probe templates promote
  those taints to witnesses, opt-in (§RESIDUAL FLOOR (4)).

**Per-category, not per-instruction.** The probes are written once per CATEGORY,
and the category vocabulary is CLOSED (the Layer-1 core). So the witness tool is
~a dozen probe templates total, and a new Tier-B leaf is witnessed by re-running
the templates for the categories it declares — no new probe code per
instruction. This is what keeps the witness inside the maintainability budget
and is the reason it does NOT trip the named disqualifier (per-instruction /
per-vendor / per-ISA-extension PROACTIVE scaling): probe surface tracks the
closed category set, and leaf surface is demand-driven (§PRIVILEGED RESIDUE).

### 7.8. Methodology — follow the existing matrix oracles

The witness harness should follow the methodology already proven by ZER's eight
matrix oracles in `tests/test_*_matrix.c` (`test_asm_matrix.c`,
`test_hw_matrix.c`, `test_conc_matrix.c`, `test_cflow_matrix.c`,
`test_escape_matrix.c`, `test_async_matrix.c`, `test_shape_matrix.c`,
`test_keep_matrix.c`). Those oracles are the template; the witness is the same
shape with QEMU observation substituted for the zercheck verdict. The properties
to carry over, with their anchors:

1. **Bipartite expectation per cell.** Each matrix oracle splits scenarios into
   NEG (must reject for the *right reason*) and POS (must accept). See
   `tests/test_asm_matrix.c:96-120` (the `AMScenario` enum) and
   `:112-121` (`scenario_is_negative`). The witness analog: a leaf whose declared
   category is honored = POS (witness PASS); a deliberately mis-declared leaf
   (under-declared clobber, claimed-but-absent privilege change) = NEG (witness
   must FAIL). A NEG that witnesses GREEN is a false negative — the same hole
   class the oracles guard.

2. **Integrity guard against wrong-reason verdicts.** `test_asm_matrix.c:64-69`
   flags an INVALID-PROBE when a NEG is rejected by a parse error rather than the
   asm-safety check under test; `:42-46` (`has_asm_reason`) requires the
   rejection to cite an asm reason. Witness analog: a witness FAIL must be
   attributable to the *probed category* (e.g., "observed clobber rbx not in
   declared set"), not to a stub-boot crash or a QEMU launch failure. A crash
   that happens to fail the probe is an INVALID-PROBE, not a real FAIL — it must
   be reported separately so it cannot masquerade as verification.

3. **Explicit accounting of failure kinds.** The oracles tally `false_neg`,
   `invalid_probe`, `over_reject` separately (`test_asm_matrix.c:27`, printed at
   `:253-255`). The witness should likewise separate: real category-FAIL,
   invalid-probe (harness/QEMU error), and over-strict-FAIL (a correctly-declared
   leaf the probe wrongly rejects).

4. **`-Wswitch`-enforced exhaustive scenario enum.** The oracles use an enum with
   no `default:` so GCC errors when a scenario is added without a handler
   (`test_asm_matrix.c:95-110`). The witness profile should enumerate the closed
   category set the same way, so adding a category to the Layer-1 vocabulary
   forces a probe decision (decidable → probe template; unobservable → explicit
   taint) rather than silently leaving the new category un-probed.

5. **Emit-only vs run harness distinction.** The matrix oracles run *emit-only*
   (`-o /tmp/x.c`, no GCC) because they judge the *verdict*, not runtime behavior
   (`test_asm_matrix.c:17-18`, `test_hw_matrix.c:14-16`). The witness is the
   opposite end: it MUST run the asm under QEMU, because its whole point is
   observed behavior. The two are complementary — the matrix oracle guards the
   structural verdict at compile time; the witness guards the leaf's observed
   behavior at witness-production time.

### 7.9. How the witness fits the toolchain anchors

For a fresh implementer, the witness touches these real code points:

- **The Tier-B leaves themselves** live as the only raw-asm-bearing `@bind`
  forms (`NODE_LEAF_BIND`, STEP 4 of the plan). The checker's existing
  `NODE_ASM` handler (checker.c ~10720-10890, e.g. the duplicate-register-binding
  checks at `:10720-10738` and the Z-rule context bans at `:10751`+) is the
  machinery that already keeps operand-boundary safety (Z1-Z8/Z11) ACTIVE inside
  a leaf's asm — the witness verifies the *categories*, while these Z-rules keep
  verifying the *operand values* (UAF/bounds/qualifier/provenance) that flow
  through the asm. The witness does not replace them; it adds the missing layer.

- **Ordering is not yet derivable from the builtin** and must stay conservative
  until wired. emitter.c hardcodes `__ATOMIC_SEQ_CST` for atomic *value*-ops
  (e.g. `__atomic_load_n(..., __ATOMIC_SEQ_CST)` at emitter.c:3128 and the
  full block 3117-3152; the @atomic_* routing comment at emitter.c:8355-8359
  states "All SEQ_CST ordering for now"). The fence path *is*
  ordering-parameterized (emitter.c:3098-3102 maps SEQ_CST/RELEASE/ACQUIRE). The
  consequence for the witness: ordering is BOTH unobservable in QEMU AND
  not-yet-derived from the builtin, so it stays doubly TAINTED — the
  derive-table must conservatively declare `ordering = seq_cst` (the
  actually-emitted truth) until the param is wired through the value-ops, and the
  witness never tries to confirm it.

- **No `@bind` / witness consumption exists in checker.c yet** — it is Phase 2/3
  work. The witness producer is `tool/asm_witness/` (STEP 5); the consumer is the
  hash-recompute + lookup added to the leaf-import path. The 8 matrix oracles in
  `tests/test_*_matrix.c` are the regression harness pattern the witness conformance
  tests should imitate (§7.8).

### 7.10. What the witness does NOT do (honest scope)

- It does NOT verify wrong-VALUE bugs (adc-drops-carry, off-by-one bsr,
  CAS-wrong-operand). Those are caught by the differential reference (DRC), kept
  as an opt-in x86-native CI sanity layer, NEVER the gate (§THE DESIGN
  EXPLORATION). The witness checks category *honoring*, not algorithmic
  *correctness*.
- It does NOT make composition or Tier-A leaves safer — those are derived
  (Tier A from the GCC builtin contract) or folded (composition), needing no
  QEMU. The witness exists solely for Tier-B (~36 ops outside C's abstract
  machine).
- It does NOT cover the unobservable categories (ordering, x86 alignment,
  provenance_clear_on_output) — those remain a NAMED, greppable floor (§TAINT),
  honestly marked as never-verified rather than falsely green.
- It does NOT escape the QEMU/TCG fidelity floor: a witness is only as faithful
  as the emulator that produced it, which is why `qemu_version` is recorded and
  the floor is named (§RESIDUAL FLOOR (1)-(2)). The anchor moves from the
  author's word to the QEMU-TCG model — auditable and shared, but not the die.

In sum: the witness converts "the leaf author asserts category C" into "QEMU observed category C honored on x86" for the decidable categories — collapsing the floor into a smaller, centralized, reviewable set — while explicitly refusing to fake verification for the categories an emulator cannot see.


## 8. Clobber and register safety (the strongest case)

Clobber/register safety is the category where ZER's asm-safety design is at its
*strongest* — the exact opposite end of the spectrum from memory ordering (§ on
ordering), which is the *weakest*. Understanding *why* it is the strongest case
is the whole point of this section: it isolates the two structural properties
(fold soundness, witness observability) that make a category cheap to make safe,
and shows that both hold maximally for clobbers and minimally for ordering. The
two ends are independent axes, not a single danger dial — that independence is
the load-bearing observation at the end.

A clobber declaration is a claim: *"executing this asm modifies exactly these
registers/flags and no others."* Get it wrong (under-declare) and the compiler
above the asm assumes a caller-saved register survived the asm when it did not —
a register the surrounding code still needs gets silently corrupted. This is the
canonical asm bug; the existing Option E doc names it as the *invisible-contract
effect bug* and tabulates it as the asm-shaped instance of a general phenomenon
(`docs/asm_lang_zer_safe.md:341-346`): the row reads *"Wrong clobber list (asm) |
'rdx preserved' | Corrupted register downstream."* The same doc also lists the
wrong-clobber-list among the trust-gaps Level D had to confront
(`docs/asm_lang_zer_safe.md:346`).

What makes this category strong is that *every* sub-problem of clobber/register
safety has a structural mechanism — none of it falls to author-trust or to the
tainted floor. There are five distinct sub-problems, each with its own mechanism.

### 8.1 The five sub-problems and their mechanisms

| Sub-problem | Mechanism | Where it lives |
|---|---|---|
| (1) composition clobbers | sound UNION fold over children | ERBT fold rules (S2, proposed) |
| (2) leaf clobbers | QEMU sentinel-sweep witness | conformance witness (S3, proposed) |
| (3) register NAME validity | delegated to GCC assembler | already shipping + `zer_asm_register_valid_with_features` |
| (4) operand VALUES in/out | existing ZER Z-rules (Z1-Z8, Z11) | `checker.c` NODE_ASM, already shipping |
| (5) reserved registers (sp/bp/pc) | structural ban as operand | parse-time, already shipping |

Three of the five (3, 4, 5) are *already implemented today*; only (1) and (2)
are part of the proposed Effect-Row Composition refinement. This is itself
evidence the category is tractable — most of it shipped before the refinement.

### 8.2 Sub-problem (1): composition clobbers fold by SOUND union

When an author writes a `compose` operation that recombines leaves and ops (the
flexible layer — no asm, no core release), the composed op's clobber set is the
*union* of its children's clobber sets, and its flags-clobber is the *OR* of its
children's flags-clobbers. This fold is **sound** in the strict sense: the union
can only ever *over*-approximate the true clobber set, never *under*-approximate
it. If child A clobbers `{rax}` and child B clobbers `{rcx}`, the composition
clobbers `{rax, rcx}` — and that is exactly what executing A then B does. The
caller above is told to assume *more* registers are destroyed than strictly
necessary; the failure mode of an over-approximation is a missed optimization
(a register spilled that needn't be), never a corrupted register. Soundness here
means: **the derived row is always a superset of the true effect.** That is the
safe direction.

Contrast with `requires_aligned(n)`, which folds by MAX (also sound: the
strictest child alignment requirement dominates). Contrast sharply with
`memory_barrier`/ordering, which does **not** fold soundly — taking the
max-ordering-of-children is *unsound* (ZER's own Session-G evidence: Phase 3
in-block ordering enforcement was abandoned because it false-positived the
canonical libpmem CLWB+SFENCE idiom — `CLAUDE.md:1294-1297`). Positional
`memory_barrier` is therefore *excluded* from the fold vocabulary entirely;
ordering stays value-intrinsic only. Clobber's union/OR fold is the textbook
case of a fold that *is* sound, which is precisely why this sub-problem is easy.

The soundness is structural, not empirical: a register written by *either*
child is a register written by the sequence, full stop. No data-dependence, no
ordering subtlety, no weak-memory hazard. The fold is closed and total over the
clobber vocabulary.

### 8.3 Sub-problem (2): leaf clobbers are the WITNESS SWEET SPOT

A Tier-B leaf (raw asm, real mnemonic — the only place freeform asm appears) has
a *declared* clobber row. The conformance witness verifies it. Clobbers are the
single best category for witnessing, for three converging reasons:

1. **Observable.** The toolchain fills *every* register with a distinct sentinel
   value, runs the leaf's asm under `qemu-system-x86_64` (8.2.7 — the only system
   QEMU present in this env), then reads every register back. Any register whose
   sentinel survived was *not* clobbered; any register whose sentinel is gone
   *was* clobbered. This directly observes the true clobber set and compares it
   against the declaration. A declared-but-not-observed register is harmless
   (over-declaration). An *observed-but-not-declared* register is the bug — the
   classic under-declared-clobber — and the sweep catches it.

2. **Deterministic and near-complete in a single run.** Register write-sets are
   *structural*, not data-dependent: an instruction either writes a register or
   it does not, irrespective of operand values. (A multiply writes `rdx:rax`
   whether the inputs are 0 or 2^31.) So one sentinel-fill-and-readback run
   observes essentially the complete clobber set — there is no need to fuzz
   operand values to coax out hidden clobbers, unlike value-correctness bugs
   which *are* data-dependent. This is why clobber witnessing is cheap: O(1)
   runs, not a search.

3. **It catches a bug GCC and Rust structurally CANNOT.** Both GCC inline asm
   and Rust `asm!` *trust the clobber list the author wrote* — they propagate it
   into register allocation without ever checking whether the asm body honors it.
   An under-declared clobber in GCC/Rust is a silent miscompile that surfaces as
   data corruption far from the asm site. ZER's sentinel sweep is the only one of
   the three that *verifies the declaration against observed behavior* — it
   closes the exact hole GCC/Rust leave open. The witness is bound to
   `(op, ISA, category-profile, blake2(asm))`; a leaf-use of an unwitnessed or
   hash-mismatched binding is a **compile error** (fail-closed structural
   precondition), so you cannot use a leaf whose clobber declaration was never
   checked.

The decidable witness categories are exactly the observable+deterministic ones:
clobbers_register/clobbers_flags (this sub-problem — the sweet spot),
`changes_privilege` (CPL readback), `requires_nonzero` (zero-trap), and
`requires_aligned` *where it actually traps*. The UNOBSERVABLE categories —
memory ordering (QEMU TCG flattens to TSO, so a too-weak fence witnesses GREEN,
which is *worse* than author-trust because it manufactures false confidence) and
alignment on x86 (AC-off ⇒ no trap) — stay tainted and are never witnessed.
Clobber sits at the top of the decidable set.

### 8.4 Sub-problem (3): register NAME validity → GCC

Whether `rax` is a real register name, whether `x0` is valid on this target,
whether a sub-extension register exists — all of this is delegated downward.
GCC's assembler errors on a bad register name; ZER does not duplicate the ISA
register catalog (Phase 1 of the plan *deletes* the per-arch register tables —
`asm_register_tables_*.c`). The compiler does a lightweight pre-check today:
`checker.c` NODE_ASM validates each input/output/clobber register name via
`zer_asm_register_valid_with_features(arch, features, name, len)`
(`checker.c:10977-11020`), emitting the *O3 rule* error
*"asm clobber 'X' not recognized for x86_64 — Use a known register name, or
'memory' / 'cc' for side-effect markers."* The pseudo-clobbers `"memory"` and
`"cc"` are special-cased and skipped — they are GCC side-effect markers, not real
registers (`checker.c:11006-11010`). Arch is plumbed from `--target-arch` through
`Checker.target_arch` (`checker.c:10964-10967`); a valid x86 register typed on an
aarch64 build is reported against the *actual* target, not always-x86 (audit fix,
`checker.c:10968-10975`). This pre-check is a UX nicety; GCC remains the
authoritative oracle for register-name validity — *"GCC is the trusted
assembler"* (`docs/asm_lang_zer_safe.md:5849`).

### 8.5 Sub-problem (4): operand VALUES in/out → existing ZER Z-rules

This is the dimension where ZER diverges hardest from Rust. The *names* of
clobbers are a register-allocation concern; the *values* flowing through asm
operands are a memory-safety concern, and ZER keeps its full safety analysis
**active through the asm operand boundary** — unlike Rust, which *goes blind*
inside `unsafe { asm!() }` (the asm doc lists Rust's `unsafe { asm!() }` as a
trust-the-author construct, `docs/asm_lang_zer_safe.md:1493`).

Ten of ZER's thirteen Z-rules are wired into the NODE_ASM handler (Z1-Z8, Z11,
Z12 — `docs/asm_lang_zer_safe.md:4361-4376`). The operand-relevant ones:

| Z-rule | What it tracks through an asm operand |
|---|---|
| Z1 | UAF check at the operand boundary (freed handle passed as operand) |
| Z2 | move-struct transfer at the operand (use-after-move) |
| Z3 | VRP range invalidation on asm output |
| Z4 | provenance type cleared on asm output |
| Z5 | local-derived pointer rejected when asm declares a memory clobber (could store the escaping pointer) |
| Z7 | MMIO range/alignment check on a memory-operand address (via `@inttoptr`) |
| Z8 | qualifier (volatile/const) preservation on asm output |
| Z11 | non-`keep` pointer param + memory clobber rejected |

Two of these key directly off the *clobber list*, tying value-safety to clobber
declarations. When an asm statement declares a `"memory"` clobber, the checker
sets `has_memory_clobber` (`checker.c:10797-10810`) and then:

- **Z5** rejects any `is_local_derived` pointer passed as an input, because a
  memory-clobbering asm body may *store* that pointer somewhere it outlives its
  scope — *"asm body may store ... or remove memory clobber"*
  (`checker.c:10889-10913`).
- **Z11** rejects a non-`keep` pointer *param* under a memory clobber, for the
  same store-escape reason (`checker.c:10792-10855`).

So the memory clobber is not merely accounted for register-allocation-wise — it
*arms* the escape and keep-param analyses. The clobber declaration and the
value-safety analysis are coupled. (Empty clobber entries are also rejected at
parse-validate time: *"asm clobber entry must be non-empty register name string,"*
`checker.c:10603-10607`.)

### 8.6 Sub-problem (5): reserved registers sp/bp/pc structurally banned

The stack pointer, frame pointer, and program counter cannot appear as asm
operands at all. This is a *structural ban*, not a tracked property: an operand
binding to `sp`/`bp`/`pc` (or their per-arch spellings) is rejected outright.
The rule predates Option E — it is item 4 of the original Option-C validation
list: *"Reserved register rejection — reject sp/bp/pc if used as operand (parse
clobber list)"* (`docs/ASM_ZER-LANG.md:229`, `:280`). Banning these as operands
keeps GCC's prologue/epilogue and the compiler's own stack/frame accounting
intact; an asm that needs to touch the stack pointer belongs in a `naked`
function where there is no prologue to corrupt (asm is allowed *only* inside
`naked` functions — the S1 interim guard, `CLAUDE.md:264-268`). Combined with
Z12 (the `scan_frame` stack-depth walker recurses into asm operands —
`docs/asm_lang_zer_safe.md:4376`), the reserved-register ban means asm cannot
silently subvert ZER's stack-overflow accounting.

### 8.7 Illustrative leaf with clobbers and the three checks

A Tier-B leaf is the only place a real mnemonic appears. Its declaration carries
an effect row including clobbers; three independent checks gate it. Syntax below
is **illustrative — proposed surface, not yet shipped**:

```
leaf_bind @cpu_write_cr3 x86_64 {                 // illustrative
    instructions: "mov %0, %%cr3"
    inputs:   { pa: u64 }                          // operand VALUE — Z-rules active
    clobbers: { "memory" }                         // flags/regs touched
    categories: clobbers_register("memory"),
                changes_privilege,                  // witnessable (CPL readback)
                writes_mem(8, seq_cst)              // ordering → TAINTED, never witnessed
    safety: "writes CR3; flushes non-global TLB; CPL=0 required; see SDM Vol 3"
}
```

The three checks that must all pass before any caller can use this leaf:

1. **Consistency (compile-time, structural).** Does every register/clobber NAME
   resolve on this ISA? → `zer_asm_register_valid_with_features` +, ultimately,
   GCC. A typo'd or wrong-arch register name is a compile error (sub-problem 3).

2. **Witness (toolchain, decidable categories).** Does the asm actually clobber
   exactly what `clobbers_register` declares? → sentinel-fill all registers, run
   the asm under `qemu-system-x86_64` (privileged leaves boot a ring0 stub —
   QEMU emulates privilege so CR3/MSR/port ops *are* probeable in system mode),
   read back. An observed-but-undeclared register fails the witness. The witness
   is recorded as `(op, ISA, profile, blake2(asm), qemu-version)`; the importer
   recomputes the hash and a missing/stale witness is a **compile error**
   (sub-problem 2). `changes_privilege` is co-witnessed (CPL readback). The
   `writes_mem(...,seq_cst)` *ordering* sub-claim is **NOT** witnessed — it stays
   tainted (QEMU TCG ⇒ TSO would witness a too-weak fence GREEN).

3. **GCC (assembler).** GCC compiles the emitted C+asm; the assembler is the
   final authority on instruction encoding and register-name validity. ZER never
   re-implements the encoder (emit-C is permanent).

Above the leaf line, a `compose` op that calls `@cpu_write_cr3` plus, say, an
`@invlpg` leaf gets its clobber row by **union fold** (sub-problem 1) — no asm,
no witness re-run for the composition itself, and the derived row is guaranteed a
superset of the true effect. The author may *declare* an expected row;
declared-vs-derived mismatch is a **compile error** (the @inttoptr-shaped gate:
the composed-binding grammar has no mnemonic production, so you literally cannot
smuggle raw asm into a composition).

### 8.8 The observability spectrum — danger and verifiability are independent axes

The reason clobber/register is the strongest case and ordering is the weakest is
captured by one principle:

> A category's **safety-achievability = (fold soundness) × (witness
> observability).** Both factors are HIGH for clobbers and LOW for ordering.

| Category | Fold soundness | Witness observability | Safety-achievability |
|---|---|---|---|
| clobbers_register / clobbers_flags | HIGH (sound union/OR) | HIGH (sentinel sweep, deterministic) | **BEST** |
| requires_aligned | HIGH (sound MAX) | partial (traps only where HW traps) | good |
| changes_privilege | n/a (leaf-only) | HIGH (CPL readback) | good |
| memory ordering | LOW (max-of-children unsound — Session-G) | LOW (TCG flattens to TSO; too-weak fence ⇒ false GREEN) | **WORST (floor)** |

The crucial, non-obvious corollary: **danger and verifiability are independent
axes.** A scary-named, high-privilege operation like `@cpu_write_cr3` (writes the
page-directory base; gets it wrong and the whole address space is gone) is
*observable* — `changes_privilege` reads back via CPL, the clobber sweep observes
the register set — and therefore sits high on the safety-achievability scale.
Meanwhile a boring-named, unprivileged operation like a release-fence in an
ordering claim is *unobservable* on this host and sits on the irreducible floor.

The frightening operation is the *easy* one to verify; the mundane one is the
*hard* one. Intuition that ranks categories by how dangerous they sound is
exactly backwards. ZER ranks them by `fold-soundness × witness-observability`,
which is why clobber/register safety is presented here as the strongest case: it
is the one category where both factors are maxed, the union fold is provably
sound, the sentinel sweep observes the truth deterministically in one run, and
the bug it catches (under-declared clobber) is one that GCC and Rust
structurally cannot.

### 8.9 What is NOT claimed here (honest floor)

Clobber/register safety is *relative to leaf correctness* like everything else in
the closure argument: a sound union fold faithfully propagates a *lying* leaf, so
the floor concentrates into the leaf set, and the conformance witness is what
shrinks that floor for clobbers specifically. The residual floor that clobber
safety does **not** remove: silicon errata (a register the silicon clobbers that
neither the SDM nor QEMU's TCG model reflects — anchored to the QEMU-TCG model,
auditable but not the die), and QEMU/TCG fidelity itself. These are named floors,
not silent holes. Ordering, alignment-on-x86, and `provenance_clear_on_output`
remain tainted — they are *not* clobber problems and are covered in their own
sections. Within its own boundary, clobber/register is the category where the
mechanism is most complete.


## 9. The privileged residue (Tier B) and why it stays bounded

This section pins down the *only* place in the whole design where a real
mnemonic (raw asm) is still allowed to appear: the **Tier B leaf set** — the
privileged / machine-configuration operations. Everything else (Level 0 ZER
arithmetic, Tier A GCC-builtin leaves, and all `compose`d operations above the
leaf line) is either inside C's abstract machine or recombined from leaves by
sound fold rules, and therefore carries *no* mnemonic and *no* per-ISA asm
maintenance. Tier B is the residue. The claim defended here is narrow and
precise: **the residue exists for a structural reason (these ops are outside
C's value-semantics and are inherently single-ISA), it is small and slow-growing,
its growth is demand-driven and fail-closed rather than proactive-whole-ISA, and
the bulk of its declared safety properties are *witnessable* on x86 rather than
blindly trusted.** That is the difference between a bounded floor and an
unbounded one.

### 9.1 What forces an op into Tier B: the abstract-machine test

The single test that splits Tier A from Tier B (introduced in §1 / the
effect-row-composition refinement) is:

> *Is the operation a value-computation inside C's abstract machine, or does it
> CONFIGURE the machine?*

- **Inside the abstract machine** → there is, or could be, a portable GCC
  builtin whose documented contract *is* the effect row. GCC selects the real
  instruction and handles sub-architecture selection via `-march`. These become
  **Tier A leaves** (`__builtin_add_overflow`, `__atomic_*`,
  `__builtin_clz/ctz/popcount`, `__builtin_bswap`, …). No raw asm. ~95% of ops
  by count.

- **Configures the machine** → the operation manipulates state that C *has no
  model for*: the current privilege level, the page-table base, model-specific
  registers, the interrupt flag, I/O port space, the TLB, cache coherency state.
  C's abstract machine is a model of *values and objects in memory*; it has no
  notion of "ring 0", "CR3", "an MSR", or "port 0x60". Because the operation has
  **no value-semantics expressible in C**, GCC cannot offer a portable builtin
  for it — there is nothing for a builtin's contract to *be about*. These become
  **Tier B leaves**, and a Tier B leaf is the *only* construct in ZER's asm
  surface that contains a real mnemonic.

Two independent properties make these ops Tier B, and **both** hold for every
member of the set:

1. **Outside C's abstract machine** — no value-semantics, hence no portable
   builtin is even *possible* (not "GCC happens not to ship one", but "there is
   nothing to model").
2. **Inherently single-ISA** — the *concept* may be cross-architecture (every
   ISA has an "interrupt enable"), but the encoding, operand register discipline,
   and frequently the semantics are per-ISA. x86 `cli` ≠ ARM `cpsid i` ≠ RISC-V
   `csrci sstatus, 8`. There is no neutral spelling.

The objective Tier-A boundary test from §1 — *does it compile under
`-ffreestanding`?* — confirms the split empirically: `__builtin_clz`,
`__builtin_add_overflow`, and `__builtin_bswap*` compile freestanding (Tier A);
`__rdmsr` does **not** (Tier B). The privileged ops fail freestanding precisely
because they are not abstract-machine operations.

### 9.2 The categorization of the ~36 Tier-B ops

ZER already shipped this entire set as the **D-Alpha privileged/CPU
intrinsics** (CLAUDE.md, the "130/130" intrinsic batches; see CLAUDE.md:482).
The Tier-B leaf set is *exactly* the privileged + machine-configuration subset
of those intrinsics — it is not a new catalog to be invented, it is the
already-enumerated privileged residue. Today these are emitted as per-arch
inline asm (CLAUDE.md:347 "Per-arch inline asm emission"); under this design
they become the gated Tier-B leaves. Four categories, with their real D-Alpha
anchors:

**(a) Control-state writes — CR / MSR / XCR0** (D-Alpha-9, CLAUDE.md:363-375):

```
@cpu_read_msr(u32) -> u64    @cpu_write_msr(u32,u64)    # RDMSR / WRMSR
@cpu_read_cr0/cr3/cr4()      @cpu_write_cr0/cr3/cr4(u64)
@cpu_read_xcr0() -> u64      @cpu_write_xcr0(u64)        # XSETBV
```

`@cpu_write_cr3` switches the address space and flushes the non-global TLB
(CLAUDE.md:370) — a page-table reconfiguration with no value-semantics. All are
"All privileged (CPL=0). SIGSEGV in user mode. Non-x86 archs get no-op
fallback." (CLAUDE.md:376). Related control-state ops appear in D-Alpha-13:
`@cpu_read_fsbase/gsbase`, `@cpu_write_fsbase/gsbase` (require CR4.FSGSBASE=1,
CLAUDE.md:418), debug registers `@cpu_read_dr`/`@cpu_write_dr`
(CLAUDE.md:436-438), and `@cpu_read_cr2` (page-fault address, D-Alpha-14,
CLAUDE.md:463).

**(b) I/O ports — IN / OUT** (D-Alpha-13, CLAUDE.md:424-430):

```
@port_in8/in16/in32(u16) -> uN     @port_out8/out16/out32(u16, uN)
```

x86 port I/O is "privileged (CPL <= IOPL)" (CLAUDE.md:424). Port space is a
separate address space C cannot name; there is no builtin and no portable
spelling.

**(c) Privilege transitions — the genuinely "call-like" ops** (D-Alpha-12,
"ALL privileged", CLAUDE.md:404-411):

```
@cpu_syscall()    # user→kernel: syscall / svc #0 / ecall
@cpu_sysret()     # kernel→user: sysretq / eret / sret
@cpu_iret()       # interrupt return: iretq / eret / mret
@cpu_hypercall()  # guest→hypervisor: vmcall / hvc #0 / ecall
@cpu_set_priv_stack(u64)   @cpu_get_priv_level() -> u32
```

These are the ops that *transfer control across a privilege boundary*. Note the
firmware-call siblings in D-Alpha-13: `@cpu_sbi_call` (RISC-V ecall to M-mode)
and `@cpu_smc_call` (ARM TrustZone `smc #0`) (CLAUDE.md:441-442). Each "require
correctly-set system register context (CS/RIP/RFLAGS on x86; ELR/SPSR on ARM;
sepc/sstatus on RISC-V) before the transition" (CLAUDE.md:413-414) — a
precondition C's type system cannot express, which is exactly why these stay
TAINTED/witnessed rather than derived. The interrupt-control ops
(D-Alpha-3, CLAUDE.md:339-345) belong here in spirit — `@cpu_disable_int` /
`@cpu_enable_int` (cli/sti, cpsid/cpsie, csrci/csrsi) flip the interrupt flag,
a piece of machine state C cannot see.

**(d) Inspection / cache / TLB** — CPUID / RDTSC / INVLPG / WBINVD and friends
(D-Alpha-10, -11, -14):

```
@cpu_cpuid(u32,u32) -> u64   @cpu_cpuid_ecx(...)        # CPUID
@cpu_vendor_id/feature_bits/model_id()                  # CPUID leaves 0/1
@cpu_cache_disable()   # CR0.CD=1 + WBINVD  (CLAUDE.md:466)
@cpu_cache_enable()    # CR0.CD=0
@cache_flushopt/@cache_writeback   # CLFLUSHOPT / CLWB  (D-Alpha-13)
@cpu_eoi()             # end-of-interrupt to LAPIC/GICv3
```

Some inspection reads are **non-privileged** (D-Alpha-10 are "All
non-privileged — run in user mode directly", CLAUDE.md:392) — `@cpu_read_sp`,
`@cpu_read_flags`, `@cpu_vendor_id`, etc. They are still Tier B because they
inspect CPU state that has no value-semantics in C and have no portable builtin
(except `@cpu_read_tp`, which is genuinely a builtin — `__builtin_thread_pointer`,
CLAUDE.md:382, so it can ride Tier A). Cache/TLB ops (WBINVD, INVLPG, CLWB)
manipulate coherency state — outside the abstract machine by construction. **CLWB
specifically is the op behind the Session-G ordering false-positive** discussed
below.

The set is ~36 distinct machine-configuration ops once the genuine builtins
(`@cpu_read_tp`) and the abstract-machine-internal ops (atomics, bit ops, bswap
— all Tier A) are subtracted from the 130 D-Alpha intrinsics (CLAUDE.md:482).

### 9.3 Why the residue stays bounded — three structural properties

The fear this section must put to rest is: *"per-ISA raw asm sounds like exactly
the per-instruction/per-vendor/per-ISA-extension maintenance treadmill the whole
design was supposed to escape."* It is not, for three reasons that **must hold
jointly**.

**(1) The USED set is mostly stable, decades-old core.** CR3 switching, MSR
read/write, port I/O, IRET, and the interrupt flag are not moving targets —
they are the same instructions kernels have driven since the 386 / since each
ISA's first protected mode. Implement a leaf for `@cpu_write_cr3` *once* and it
does not change. The privileged *core* is a fixed point, not a growing frontier.

**(2) New privileged ops arrive SLOWLY, and vendor-driven.** New privileged
operations *do* arrive — XSAVE/XRSTOR, FSGSBASE, SMAP, PKE (protection keys),
CET (control-flow integrity, e.g. `@cpu_endbr` → ENDBR64, CLAUDE.md:478-479),
WAITPKG (`@cpu_umwait`/`@cpu_umonitor`, CLAUDE.md:474-476). But they arrive on a
*per-architecture-generation cadence*, gated on silicon shipping, not on a
release schedule ZER controls. This is single-digit ops per architecture per
*generation* — the "~1-2 ops/decade" growth rate cited in §1's triangle for the
frozen-taxonomy axis. The set grows, but at the speed silicon grows, which is
geologically slow compared to software churn.

**(3) DEMAND-DRIVEN + FAIL-CLOSED — the property that actually defeats the
disqualifier.** This is the load-bearing one:

- **Demand-driven**: you write a Tier-B leaf *only when firmware actually uses
  that op*. You never enumerate the whole ISA's privileged opcode space upfront.
  If no firmware in your tree calls `@cpu_write_xcr0`, no leaf for it needs to
  exist. The burden tracks **your usage** (tiny, slow), **not the ISA catalog**
  (huge, vendor-defined).
- **Fail-closed**: an unimplemented privileged op is a **compile error**, never a
  silent hole. There is no "unknown op → assume benign → pass GREEN" path. If
  firmware names an op with no Tier-B leaf, compilation stops; you implement the
  one leaf, witness/taint it, and proceed. Coverage gaps are *loud*, not
  *exploitable*.

The two together mean: **the maintenance burden is bounded by the size of the
privileged surface your firmware actually touches**, and any attempt to use an
unimplemented op is structurally blocked rather than silently mishandled.

### 9.4 Why this does NOT trip the named disqualifier

The design fan-out (§ on the rejected alternatives) named a specific
disqualifier: **proactive per-instruction / per-vendor / per-ISA-extension table
maintenance that must cover the whole ISA or be fail-open.** The rejected
disassemble-classify designs (DCA / DSC) tripped it exactly:

- They needed the **whole-ISA opcode table** *proactively* — every mnemonic had
  to be tabled in advance, because an untabled mnemonic decodes to
  `NO_CATEGORY`, and `NO_CATEGORY` means a wrong declaration passes **GREEN**
  (fail-OPEN). The "100x-smaller table" defense for those designs was a verified
  fiction: the real per-ISA tables ZER deletes in Phase 1 are 53/37/30 rows
  (asm_instruction_table_x86/arm/riscv), not the claimed ~15.

Tier-B leaf maintenance trips *none* of the three clauses of the disqualifier:

| Disqualifier clause | DCA/DSC (rejected) | Tier-B leaves (this design) |
|---|---|---|
| Proactive whole-ISA cover | **Yes** — must table every opcode | **No** — demand-driven, one leaf when used |
| Fail-open on gaps | **Yes** — untabled → NO_CATEGORY → GREEN | **No** — unimplemented → compile error |
| Granularity of growth | per-instruction table rows | one tiny leaf at a time, on first use |

The distinction is *not* "Tier B has no per-ISA work" — it plainly does (a leaf
*is* per-ISA asm). The distinction is that the per-ISA work is **(a) lazy**, **(b)
fail-closed** (gaps are errors, not silent wrong-GREEN), and **(c)
one-leaf-at-a-time** (no obligation to mirror the vendor's opcode catalog).
New-ISA bring-up is therefore *bounded*: write that
ISA's leaf set once (only the ops your firmware uses), and then **every
composition above the leaf line works on the new ISA for free** — flexibility
and per-ISA cost decouple (§1 triangle, "New-ISA bring-up").

### 9.5 The residue is mostly *witnessed*, not blindly trusted

The final reason the residue is acceptable: most of its declared safety
properties are **observable on x86 and verified by the conformance witness**
(see §ConformanceWitness), so "trusting the author's declaration" is the
*exception*, not the rule, within Tier B.

Recall the witness mechanism: the toolchain runs per-category probes in
**qemu-system-x86_64** (the only system QEMU present in this env; version
8.2.7), boots a **ring-0 stub** so that privileged ops *are* executable under
emulation, and binds a `.zerwitness` record to `(op, ISA, category-profile,
blake2(asm), qemu-version)`. A leaf whose declared row is contradicted by
observation, or whose asm hash has drifted, is a **compile error** (fail-closed
precondition).

What is **witnessable** for the privileged residue — the verified majority:

- **`changes_privilege`** — directly observable by **CPL readback**. A privilege
  transition (`@cpu_syscall`, `@cpu_sysret`, `@cpu_iret`) either lands at the
  declared privilege level or it does not; the stub reads CS.RPL / CPL before and
  after. This is the category that dominates Tier B (it is *why* most of these
  ops are privileged), and it is **decidable**. QEMU *emulates* privilege, so
  privileged ops are probeable in system mode — this is the whole reason the
  ring-0 stub exists.
- **`clobbers_register` / `clobbers_flags`** — the witness sweet spot:
  sentinel-fill every GPR + flags, run the leaf, read back, diff. Register
  write-sets are *structural, not data-dependent*, so a single run is
  near-complete. This catches the classic under-declared-clobber bug that GCC
  and Rust inline-asm cannot (they trust the clobber list the author wrote;
  ZER's witness checks it against silicon-emulation reality). MSR/CR writes,
  CPUID (clobbers EAX/EBX/ECX/EDX), and the like all expose their real clobber
  sets here.
- **`requires_nonzero`** (zero-trap probe) and **`requires_aligned` where it
  actually traps** — decidable by feeding the trapping input and observing the
  fault.

What is **NOT witnessable**, and therefore stays TAINTED (the named floor for
the residue):

- **Memory ordering** on the cache/TLB/fence-adjacent ops. QEMU TCG flattens to
  TSO, so a *too-weak* fence witnesses GREEN — *worse* than author-trust, because
  it manufactures false confidence. Ordering is the low-low corner of the
  observability spectrum (unsound fold × unobservable witness) and **stays
  value-intrinsic, declared, and tainted**. This is grounded in ZER's own
  Session-G evidence: Phase 3 in-block ordering enforcement was **ABANDONED**
  because it false-positived the canonical libpmem **CLWB + SFENCE** idiom
  (CLAUDE.md:1294-1296; "Phase 3 in-block enforcement ABANDONED (false-positived
  the canonical multi-block CLWB+SFENCE libpmem idiom)"). The lesson recorded
  there — "don't ship enforcement that rejects valid code patterns"
  (CLAUDE.md:1297) — is *why* positional `memory_barrier` is excluded from the
  fold vocabulary and why ordering is never witnessed.
- **Alignment on x86** — with AC off, misaligned access does not trap, so
  `requires_aligned` cannot be observed on this host for x86.
- **Non-x86 privileged asm** — there is **no ARM or RISC-V system QEMU and no
  cross-compiler in this env**. So ARM/RISC-V Tier-B leaves (e.g.
  `@cpu_iret` → `eret`/`mret`, `@cpu_smc_call`, `@cpu_sbi_call`) cannot be
  witnessed here and stay TAINTED. **This is an environment limit, not an
  architecture limit**: installing `qemu-system-aarch64` + a cross-gcc promotes
  these from taint → witness, opt-in (Residual Floor item 4). Today's D-Alpha
  fallback for these on a non-matching host is already conservative —
  "Non-x86 archs get no-op fallback" (CLAUDE.md:376) — which is fail-safe but
  uninformative, hence the taint marker.

So the honest accounting for Tier B: `changes_privilege` and the clobber
categories — the properties that *make* an op privileged and the properties most
likely to be mis-declared — are **verified by witness on x86**. Only ordering,
x86 alignment, and the *non-x86* leaves on this host fall to the named taint
floor. The residue is **mostly verified, partly tainted, never silently
trusted** (because taint is greppable and propagates to the importing function;
see §Taint).

### 9.6 Where the residue concentrates the floor (closure restatement)

The closure theorem (§ClosureArgument) says: *safe leaves + sound fold ⇒
everything composed above the leaf line is safe; safety is RELATIVE TO leaf
correctness.* Tier B is where that relativity lives. A sound fold faithfully
propagates a **lying leaf** — it cannot detect one. So the entire correctness
budget for machine-configuration safety **concentrates into this ~36-op set**.
That concentration is the win: it reduces the audit surface from O(bindings)
distributed prose checklists (Option E's per-binding "did the author honor the
declared categories?" question, repeated everywhere) to **O(leaves) +
O(1)-fold-proof**, where O(leaves) here is the small, slow-growing, mostly-
witnessed Tier-B set.

The residual floor for Tier B, stated without marketing, is therefore exactly
three things: **(1)** silicon errata / microarchitecture beneath the QEMU-TCG
model (the anchor moves from author's-word to the QEMU-TCG model — auditable and
shared, but not the die); **(2)** the ordering + `provenance_clear_on_output`
categories that are structurally untestable here (tainted); and **(3)** the
non-x86 privileged leaves until ARM/RISC-V QEMU + cross-gcc are installed
(environment-removable, not architectural). Everything else in Tier B is a
compile-error-or-witnessed structural gate.

> **Implementation note (illustrative — proposed surface, not yet shipped).**
> Per STEP 4 / STEP 5 of the plan: a Tier-B leaf is the *only* `@bind` form that
> may carry a real mnemonic (`NODE_LEAF_BIND`), and even then `@bind` is a
> **compile error unless the op is on the closed privileged allow-list** (tests
> `composed_bind_mnemonic.zer`, `bind_on_builtin_op.zer`). A leaf-use of an
> *unwitnessed or hash-mismatched* privileged binding is a compile error
> (`.zerwitness` missing/stale → fail-closed). There is **no checker support for
> `@bind` / `@intrinsic_def` / allow-list today** (it lives in Phase 2/3); today
> these ops are the per-arch-inline-asm D-Alpha intrinsics in the emitter, and
> the NODE_ASM checker handler is checker.c ~10720-10890. The Tier-B
> categorization above is the *target* leaf set, anchored to the already-shipped
> D-Alpha privileged intrinsics.


## 10. Honest scope, fault attribution, and the residual floor

This section states, without marketing, exactly what the Effect-Row Composition
design proves structurally, what it proves by witness, what it leaves
author-declared behind a greppable taint marker, and what is irreducible floor
that no language can close. It carries forward the fault-attribution discipline
that Option E locked in `docs/asm_lang_zer_safe.md` §1.7.31 and reframes it in
terms of the leaf/composition/witness structure this document adds. A fresh
reader must be able to answer, for any single asm-touching guarantee, the
question "is this STRUCTURAL, WITNESSED, TAINTED, or FLOOR?" — and know who owns
the bug when it goes wrong.

The governing vocabulary is the one locked across all of ZER (`CLAUDE.md`,
"ZER's Goal"): **program-consequence** (every wrong USE of a value in ZER source
is caught at the use site — ZER owns this at 100%) versus **hardware-consequence**
(datasheet/silicon facts that never enter the program as values — floor, out of
scope for any language). The asm boundary inherits this split unchanged; the
refinement here is only about WHERE the hardware-consequence floor concentrates
once leaves and folds replace Option E's distributed per-binding prose
checklists.

### 10.1. What is STRUCTURAL (compile-enforced, no diligence required)

Structural means: enforced by the grammar or by a closed, terminating checker
fold — a wrong program does not compile. No CI test, no reviewer attention, no
"trust the author" step participates. This is the CLOSURE register: "no path to
the unsafe construct except a compiler-enforced gate."

1. **Tier-A leaf effect rows are DERIVED, not declared.** A Tier-A leaf is a GCC
   builtin (`__builtin_add_overflow`, `__atomic_*`, `__builtin_clz/ctz/popcount`,
   `__builtin_bswap*`). Its effect row is read out of ONE audited table mapping
   builtin name -> category bitset (the S1 derive-table in `checker.c`). The
   author of a Tier-A op is FORBIDDEN from declaring categories — declaration is
   a compile error, because the derive-table is the single source of truth and
   GCC, not the author, selects the real instruction (and the sub-arch via
   `-march`). This is ~95% of operations by count (computational / ALU / atomic /
   bit). The objective Tier-A boundary is "compiles under `-ffreestanding`":
   verified that `clz`, `add_overflow`, and `bswap` do; `__rdmsr` does not (it is
   not a builtin — it is Tier B).

2. **Composed effect rows are DERIVED by closed fold rules.** A `compose`
   operation (NODE_COMPOSED_BIND) recombines existing leaves/ops. It has **no
   instructions field** — the composed-binding grammar HAS NO mnemonic
   production. The composed op's effect row is computed by folding children:
   `clobbers_register` and `clobbers_flags` by UNION/OR; `requires_aligned(n)` by
   MAX. The author MAY state an EXPECTED row; declared-vs-derived mismatch is a
   COMPILE ERROR. Layer-3 call-site verification is repointed from the author's
   DECLARED row to the DERIVED row.

3. **The no-mnemonic grammar gate.** Raw asm (a real mnemonic) can appear ONLY
   inside a NODE_LEAF_BIND, and a `@bind` is a compile error unless its operation
   is on the closed privileged allow-list (Step 4 structural fence; tripwire
   tests `composed_bind_mnemonic.zer`, `bind_on_builtin_op.zer`). You literally
   cannot write freeform asm in a composition. This is the same shape as ZER's
   existing pointer gate (`checker.c:5601-5608`: no integer-to-pointer cast except
   through `@inttoptr` with mandatory `mmio`) — the unsafe construct exists only
   behind a structural witness.

4. **Operand-value safety stays live through asm.** The existing ZER Z-rules
   (Z1-Z8, Z11, Z12 — wired today in the NODE_ASM handler at `checker.c:10529`)
   keep UAF / bounds / move / VRP / provenance / escape / qualifier / MMIO
   tracking ACTIVE across the asm operand boundary. This is strictly stronger
   than Rust, which goes blind inside `unsafe { asm!() }`. Reserved registers
   (sp/bp/pc) are structurally banned as operands; register NAME validity is
   delegated to GCC (the assembler errors on a bad name).

5. **Fold soundness is the theorem, not a hope.** `clobbers_*` fold by union (a
   superset of clobbers is always a safe over-approximation — sound). The
   CLOSURE ARGUMENT: safe LEAVES + sound FOLD => everything composed above the
   leaf line is safe. This is the SAME closure shape ZER already relies on
   elsewhere (memory safety = closure over allocation primitives; type safety =
   closure over conversion intrinsics; concurrency = closure over sync
   primitives). Composition reduces the audit from O(bindings) distributed prose
   checklists to O(leaves) + O(1) fold-proof.

What structural safety does NOT give you: it faithfully PROPAGATES whatever the
leaf declared. A sound fold over a lying leaf produces a green-but-wrong result.
Therefore composition safety is RELATIVE TO leaf correctness — see §10.5. The
floor does not disappear; it CONCENTRATES into the leaf set.

### 10.2. What is WITNESSED (decidable Tier-B categories on an executable ISA)

Tier-B leaves are the ~36 raw-asm ops OUTSIDE C's abstract machine (control-state
writes — CR0/3/4, RDMSR/WRMSR, XSETBV; I/O ports IN/OUT; privilege transitions
SYSCALL/SYSRET/IRET/ECALL/SVC; inspection/cache/TLB — CPUID, RDTSC, INVLPG,
WBINVD). C has no portable builtin for them because they CONFIGURE the machine
(privilege, page tables, MSRs, ports, interrupt flag — concepts C has no model
for). Their declared effect rows cannot be derived; some can be WITNESSED.

The conformance witness (Step 5, `tool/asm_witness/`) runs per-category probes
in `qemu-system-x86_64` (verified present: version 8.2.7) and compares the
DECLARED effect row against OBSERVED behavior. QEMU emulates privilege, so
privileged ops ARE probeable in system mode by booting a ring-0 stub. The
witness is bound to the tuple `(op, ISA, category-profile, blake2(asm),
qemu-version)` and written to a `.zerwitness` record. A Layer-3 import / leaf-use
of an unwitnessed or hash-mismatched binding is a COMPILE ERROR — the gate is a
structural precondition and FAILS CLOSED. The hash is recomputed at import; a
missing or stale witness is an error, with lazy re-witnessing.

DECIDABLE (observable, deterministic) categories — what the witness actually
certifies:

- **`clobbers_register` / `clobbers_flags`** — the WITNESS SWEET SPOT. Sentinel-
  fill every register, run the op, read back. Register write-sets are STRUCTURAL,
  not data-dependent, so a single run is near-complete. This catches the classic
  under-declared-clobber bug that GCC and Rust CANNOT catch.
- **`changes_privilege`** — CPL readback after the op.
- **`requires_nonzero`** — zero-trap probe.
- **`requires_aligned`** — only where it actually traps.

The observability spectrum: a category's safety-achievability = (fold soundness)
x (witness observability). `clobbers_*` is high-high (sound fold AND observable
witness) — the best case. Note the witness's logical scope: it certifies an
EXISTENTIAL ("these categories were observed honored on x86 under QEMU TCG"). The
gate consumes it as a UNIVERSAL ("the declaration is honored"). That inference is
sound ONLY for decidable predicates — which is exactly why standalone
QEMU-witness was DEMOTED in the design fan-out to this subordinate, decidable-x86
role rather than serving as the general gate.

### 10.3. What is TAINTED (the named floor — author-declared, greppable, never green)

Some categories are neither derivable (Tier A / composition) nor witnessable
(decidable Tier B). They stay author-DECLARED but carry a greppable taint marker
(Step 6 — reuse of the existing trust-boundary marker) that propagates to and
TAINTS every importing function. A tainted symbol can never read as
verified-green. This is the honest middle: not structural, not silent.

What is tainted:

1. **Memory ordering / `memory_barrier(ordering)`.** Ordering does NOT fold
   soundly — max-ordering-of-children is UNSOUND. This is ZER's own Session-G
   evidence: Phase 3 in-block ordering enforcement was ABANDONED because it
   false-positived the canonical libpmem CLWB+SFENCE idiom (`CLAUDE.md`
   lines 1295-1296). So positional `memory_barrier` is EXCLUDED from the fold
   vocabulary entirely; ordering stays value-intrinsic only. Ordering is also
   UNOBSERVABLE under witness: QEMU TCG flattens to TSO, so a too-weak fence
   witnesses GREEN — strictly WORSE than author-trust because it manufactures
   false confidence. Ordering is therefore low-low on the observability spectrum
   (unsound fold AND unobservable witness — the irreducibly hardest category) and
   stays tainted, never witnessed. The litmus/herd7 path is the only conceivable
   ordering probe and is explicitly PROBABILISTIC; x86-is-TSO hides both
   under- and over-declaration. This confirms ordering as the worst case rather
   than relieving it.

   IMPLEMENTATION NOTE (load-bearing): "ordering derived from the builtin" is
   FALSE in the emitter TODAY. Atomic value-ops hardcode `__ATOMIC_SEQ_CST`
   (`emitter.c:3128`, `:3134`, `:3146`, `:3152`); only the fence path
   (`emitter.c:3098-3102`) is ordering-parameterized. Until Step 2 wires the
   ordering parameter through the atomic value-ops, the derive-table MUST
   conservatively declare `ordering = seq_cst` — the actually-emitted truth — so
   the declared row never overstates what GCC emits.

2. **Non-x86 privileged asm on THIS host.** ARM/RISC-V privileged leaves cannot
   be witnessed here: the environment has only `qemu-system-x86_64` 8.2.7 — no
   `qemu-system-aarch64`, no `qemu-system-riscv64`, no user-mode QEMU, no
   cross-compilers. Their effect rows stay DECLARED + TAINTED. This is an
   ENVIRONMENT limit, not an architecture limit — see §10.4 #4: installing
   `qemu-system-aarch64` + a cross-gcc promotes these from taint to witness with
   no design change (opt-in).

3. **`provenance_clear_on_output`.** Not structurally derivable and not
   observable by the probe set; stays declared + tainted.

The taint marker is the design's honesty mechanism: every place the floor is
crossed is greppable, and the floor cannot masquerade as verified.

### 10.4. What is IRREDUCIBLE FLOOR (no language closes it)

These exist by physics or by host configuration, not by any defect in the
design. They are named, not hidden.

1. **Silicon errata / microarchitecture.** Whether the actual die honors the ISA
   manual is the deepest floor — even GCC trusts the ISA manual
   (§1.7.12: "even GCC trusts the ISA manual"). The design's contribution is to
   MOVE the anchor from "the binding author's word" (Option E) to a shared,
   auditable contract: GCC's documented builtin contract (Tier A) and the QEMU
   TCG model (Tier B). Neither is the silicon, but both are inspectable and
   shared rather than per-author prose.

2. **QEMU / TCG fidelity.** The Tier-B witness is only as faithful as TCG's
   model of the ISA. A witness GREEN means "honored under TCG," not "honored on
   the die." This is why the witness scope is limited to decidable predicates and
   why TSO-flattened ordering is excluded (§10.2, §10.3).

3. **Ordering and `provenance_clear_on_output`.** Tainted, per §10.3 — listed
   here because, beyond the taint marker, there is no mechanism (structural or
   witnessed) available in this design that closes them.

4. **Non-x86 privileged asm on this host.** Floor only because of the missing
   QEMU systems and cross-toolchains; promotable to witness by installing them.
   Distinguished from #1-#3 because it is the one floor entry the operator can
   remove without changing the architecture.

5. **Leaf assertions + fold rules — the centralized small audit (the genuine
   shrink).** Every Tier-B leaf's DECLARED-where-undecidable categories, and the
   correctness of the fold rules themselves, must be trusted. But this set is
   SMALL, CENTRALIZED, and reviewable WITHOUT silicon: the fold soundness proof
   is O(1), and the leaf set is per-ISA and demand-driven. This is the
   replacement for Option E's O(bindings) distributed-prose-checklist floor — the
   genuine reduction the whole design buys: O(leaves) + O(categories) centralized
   audit instead of one prose checklist per binding scattered across libraries.

### 10.5. Fault attribution (carried from Option E §1.7.31, refined for leaves)

§1.7.31 partitions every binding-boundary failure into three categories. The
leaf/composition/witness structure preserves the partition and sharpens where
each lands.

- **Category #1 — declared effect row does not match the silicon.** The leaf
  author declared categories the asm does not honor on the target ISA. This is
  the binding/leaf AUTHOR'S fault, REJECTED at the language core. The defense is
  ARCHITECTURAL, not bandwidth-driven: "asm semantics are per-ISA hardware facts;
  the ISA manual is the datasheet for instruction meaning; the core cannot verify
  category-truth-against-silicon without importing per-ISA instruction semantics
  — the unbounded catalog Phase 1 deletes." REFINEMENT: this design narrows
  Category #1's surface. For Tier-A ops it is structurally IMPOSSIBLE (no
  declaration exists to be wrong — §10.1 #1). For decidable Tier-B categories the
  witness CATCHES the classic instance (under-declared clobber — §10.2). What
  remains genuinely Category #1 is the tainted residue (§10.3) and undecidable
  Tier-B declarations.

- **Category #2 — leaf is honest, the core mis-reasons over correct categories.**
  The fold drops a clobber, or the call-site checker fails to enforce a correctly
  derived `requires_aligned`. This is the CORE'S bug, OWNED, must be bug-free.
  The hardware-floor defense does NOT apply: the math is wrong about its OWN
  categories, not about hardware. The commitment is bounded — the category
  vocabulary, the operation taxonomy, the fold rules, and the dispatch paths are
  all FINITE, so verifier correctness is a finite testing surface (the same
  oracle methodology as the 8 matrix oracles in `tests/test_*_matrix.c`).

- **Category #3 — the vocabulary cannot express the leaf honestly.** A missing
  or over-merged category. This is the CORE'S bug, fixed through a slow-cadence
  schema-extension release. By construction extensions are rare: categories
  describe structural behavior KINDS bounded by hardware-software interaction
  physics, not by per-instruction growth.

The load-bearing discipline (§1.7.31): the Category #1 rejection has teeth ONLY
because Categories #2 and #3 are visibly OWNED. Reflexively rejecting a #2 bug as
"#1, your problem" corrodes credibility; accepting a #1 complaint as "we should
add per-instruction semantic verification" reopens the unbounded catalog. Both
drifts are refused. The boundary is the same program-consequence vs
hardware-consequence line ZER draws everywhere.

**The two-responsibility, one-boundary statement, in leaf terms:**

> The core OWNS category-REASONING: given a leaf's correctly-stated effect row,
> the fold and the call-site checker must propagate its implications to every use
> site, every time, without bugs. This is program-domain logic over the closed
> schema; it is bounded by the finite schema and is solo-maintainable.
>
> The core does NOT own category-TRUTH-against-silicon: whether a Tier-B leaf's
> declared-and-undecidable categories actually hold on the die is the
> hardware-consequence floor, surfaced at the leaf and carried by the taint
> marker. For Tier-A and for decidable Tier-B categories, this floor is removed
> (derived or witnessed); for the rest it is named, not verified.

### 10.6. The triangle, and where this design deliberately sits

THE TRIANGLE: flexibility / safety / maintainability — pick any two fully; the
third degrades. The design space was explored by a 14-angle adversarial fan-out
(two waves, ~58 agents, each judged on maintainability and soundness):

- **Intrinsic-Maximalism** — the maintainability-weighted winner (8.4). Buys
  safety + maintenance, SACRIFICES flexibility: a frozen taxonomy growing ~1-2
  ops/decade that cannot express genuinely new ops.
- **Effect-Row Composition (ADOPTED)** — the flexibility-weighted winner. Buys
  flexibility + safety, PAYS with a small leaf-audit plus the named floor past
  the leaf boundary (§10.3-§10.4). Authors write `compose` operations recombining
  leaves with no asm and no core release; new-ISA bring-up is "write its leaf set
  once (bounded), then ALL compositions work on it for free" — flexibility and
  per-ISA cost DECOUPLE.
- **DCA (disassemble-classify)** — buys flexibility + low friction, LOSES safety
  (fail-open). REJECTED: it revives the per-ISA opcode table Phase 1 deletes (the
  named disqualifier) AND fails open (an untabled mnemonic -> NO_CATEGORY -> a
  wrong declaration passes GREEN). The "100x smaller table" defense was a VERIFIED
  FICTION — real tables are 53/37/30 rows (~120 total: the very files Phase 1
  removes — `src/safety/asm_instruction_table_{x86_64,aarch64,riscv64}.c`,
  `asm_register_tables_*.c`, `asm_categories.{c,h}`), not 1500; and `-O0`
  compiler-glue contaminates disassembly (8 of 11 instructions glue in a measured
  cmpxchg case).

The privileged residue is maintainable BECAUSE it is DEMAND-DRIVEN (implement a
leaf only when firmware uses that op — never the whole ISA upfront) and
FAIL-CLOSED (an unimplemented privileged op is a compile error, not a silent
hole). The set you USE is mostly decades-old and stable (CR3/MSR/port/IRET/IF);
new privileged ops arrive SLOWLY (vendor-driven, per-architecture-generation:
XSAVE/FSGSBASE/SMAP/PKE/CET). The burden tracks YOUR usage (tiny, slow), not the
ISA catalog — so it does NOT trip the named disqualifier (per-instruction /
per-vendor / per-ISA-extension PROACTIVE scaling) that sank the disassemble-table
designs, which needed the WHOLE-ISA opcode table proactively or else be
fail-open.

Where this design sits, stated plainly: **flexibility + safety, with bounded
maintenance** — not the maintenance-maximum corner, and explicitly not the
fail-open flexibility corner. The maintenance cost is real but bounded (a small
per-ISA leaf set + an O(1) fold proof + a finite verifier test matrix); the
safety is structural for ~95% of ops, witnessed for the decidable privileged
residue, and the remainder is a NAMED, greppable, never-green floor rather than a
silent gap. That is the honest scope: total program-consequence coverage at the
call site, a concentrated and shrunken hardware-consequence floor at the leaf
line, and no equivocation between the two.


## 11. Implementation plan (lowest-effort-first)

This section is the executable ordering. It assumes the design from the
preceding sections: TIER A leaves = GCC builtins (effect row DERIVED from one
audited builtin→category table), TIER B leaves = raw asm for the ~36 ops
outside C's abstract machine (effect row WITNESSED where decidable, else
DECLARED+TAINTED), COMPOSITION = `compose` operations that recombine
leaves/ops with NO mnemonic production and an effect row folded by closed
OR/MAX rules, and the CLOSURE theorem (safe leaves + sound fold ⇒ everything
above the leaf line is safe). The steps are ordered by ascending effort and by
dependency: each step is independently shippable and leaves the tree green
(`make docker-check`).

The plan rides existing machinery rather than building new dispatch:
**64 `__builtin_`/`__atomic_` routing sites** already exist in `emitter.c`
(verified `grep -cE "__builtin_|__atomic_" emitter.c` = 64) and
**196 ISA-dispatch sites** (`grep -cE "target_arch|__x86_64__|__aarch64__|__riscv" emitter.c` = 196).
Tier A reuses the first set; Tier B reuses the second. No new code-emission
backbone is required — the new work is in `checker.c` (category derivation,
fold, structural gate) plus one out-of-tree witness tool.

A note on what is enforced where, repeated throughout: a **COMPILE ERROR** is a
structural gate the user cannot bypass without a witness or a legal construct; a
**NAMED FLOOR** is a category that is author-DECLARED, carries a greppable taint
marker, and can never read as verified-green. The whole point of the plan is to
maximize the first and concentrate the second into the smallest, most
centralized surface (the leaf set + the fold rules).

---

### STEP 0 — Execute Phase 1 (delete the per-arch tables)

Phase 1 is the Level C cleanup already specified in `docs/option_e_plan.md`
(Commits 1–6, verified there 2026-06-08). It removes the per-ISA infrastructure
that Option E / Effect-Row Composition replaces, and is a precondition: the new
mechanism must be built on a core with no competing per-arch machinery to
reconcile (`option_e_plan.md` §3).

Commit order (the plan recommends doing the checker swap FIRST to validate the
approach before any file deletion):

- **Commit 1** (`option_e_plan.md` §2, "was C6") — in the `checker.c`
  `NODE_ASM` handler, DELETE the three `zer_asm_register_valid_with_features(...)`
  register-validation loops and the `zer_asm_instruction_info(...)` F4/F7-full
  table dispatch + CPU-feature gating; ADD the frozen ~30-line UB-classics list
  (BSR/BSF/DIV/IDIV → `REQUIRES_NONZERO`; MOVAPS/MOVDQA/VMOVAPS → `REQUIRES_ALIGN_{16,32}`;
  IDIV → `COMPOUND_INTMIN_NEG1`). Register-name validation becomes GCC's job
  (the emitted C → GCC assembler errors on a bad register name).
- **Commit 2** — `git rm` the instruction tables
  (`asm_instruction_table.h`, `asm_instruction_table_{x86_64,aarch64,riscv64}.c`
  — 53/37/30 rows), `gen_instruction_table.sh`, `candidates_*.txt`,
  `arch_data/*.zerdata`, `arch_data/SCHEMA.md`.
- **Commit 3** — `git rm` the register tables
  (`asm_register_tables.h`, `asm_register_tables_{x86_64,x86_64_avx512f,aarch64,riscv64}.c`,
  `asm_register_lookup.c`), `gen_register_tables.sh`, and the stray `.v`/`.o`.
- **Commit 4** — `git rm` `asm_categories.{c,h}` + `asm_categories.v` + `.o`.
- **Commit 5** — Makefile + `check-vst` wiring: remove the 9 `src/safety/asm_*`
  files from `CORE_SRCS`/`LIB_SRCS`, the `gen-asm-tables:` target, and the
  deleted files from the `check-vst` `clightgen` line.
- **Commit 6** — convert tests that referenced deleted infrastructure
  (`tests/zer_fail/asm_aarch64_x86_reg.zer`, `asm_riscv64_x86_reg.zer` now fail
  at GCC instead of at ZER — relabel, do not delete; `asm_avx512_register.zer`
  / `asm_simd_register.zer` still validated by GCC `-mavx512f`).

End state (`option_e_plan.md` §3): ~600 lines of durable asm safety
infrastructure (Z-rules Z1–Z8/Z11/Z12, F7-light LR/SC, the UB-classics list),
**zero per-arch tables, zero probe scripts**. This is the disqualifier removal:
Effect-Row Composition must NOT revive the per-ISA opcode table Phase 1 deletes
(that is exactly the REJECTED DCA/DSC failure mode).

---

### STEP 1 — S1: the Tier-A derive-table in `checker.c` NODE_ASM

Add the closed `builtin name → category bitset` table — the single audited,
one-time artifact from which every Tier-A leaf's effect row is DERIVED. The
`NODE_ASM` handler lives at **`checker.c` ~10720–10890** (the duplicate-register
checks, the Z6/Z8/Z11/Z4/Z5 wiring, and the `ASM_OP_ROOT_IDENT` root-walk macro
are all in that span — verified). The derive-table is the logical home for the
"builtin-backed op → categories" lookup that replaces author-declared categories
for the ~95% of ops GCC has a builtin for.

Content of the derive-table (one row per builtin family):
- `__builtin_add_overflow` / `__builtin_sub_overflow` / `__builtin_mul_overflow`
  → `produces_carry`, `value_in_value_out`, `no_memory_effect`.
- `__builtin_clz` / `__builtin_ctz` / `__builtin_popcount` / `__builtin_parity`
  → `value_in_value_out`, `no_memory_effect`; clz/ctz additionally carry the
  **`requires_nonzero` column** (input 0 is undefined for clz/ctz — this is a
  REAL Tier-A floor and must be a derived category, not author-stated).
- `__builtin_bswap{16,32,64}` → `value_in_value_out`, `no_memory_effect`.
- `__atomic_load_n` / `__atomic_store_n` / `__atomic_fetch_*` /
  `__atomic_compare_exchange_n` → `reads_mem`/`writes_mem(width, ordering)`,
  plus the **lock-free-width guard**: 16-byte CAS (`__int128`) is NOT
  lock-free on baseline x86-64 (needs `cmpxchg16b` / `-mcx16`) — the table must
  declare a width column and the checker must reject (or taint) 16-byte atomics
  that would silently route through libatomic locks. The existing atomic routing
  already validates widths 1/2/4/8 (see CLAUDE.md "Atomic width validation"); the
  guard extends that to the 16-byte case.

Two structural rules in this step, both **COMPILE ERRORS**:
1. For any op that is builtin-backed, **author-supplied categories are
   FORBIDDEN** — the row is derived, and a `@bind`/`compose` that re-declares a
   category for a builtin-backed op is an error (the declaration cannot lie
   because there is nothing to declare). This is the `bind_on_builtin_op.zer`
   test in STEP 4.
2. The objective Tier-A boundary is `-ffreestanding` compilability: clz /
   add_overflow / bswap DO compile `-ffreestanding`; `__rdmsr` does NOT. Only
   builtins on the freestanding side belong in the derive-table; the rest are
   Tier B (STEP 5).

---

### STEP 2 — Wire the ordering param through atomic value-ops (fix SEQ_CST hardcoding)

"Category derived from the builtin's contract" is FALSE today for the `ordering`
category: the emitter hardcodes `__ATOMIC_SEQ_CST` for every atomic value-op.
Verified sites in `emitter.c`:
- The AST path: **`emitter.c` ~3128 / ~3134 / ~3146 / ~3152** (load/store/cas/fetch-*).
- The IR-rewritten path: **`emitter.c` ~8358 onward** (the comment at ~8357
  literally says "All SEQ_CST ordering for now (Ordering param deferred)"),
  with the actual `, __ATOMIC_SEQ_CST)` emissions at ~8364 / ~8368 / ~8375 / ~8399.
- The fence path is ALREADY ordering-parameterized: `@barrier` → `SEQ_CST`,
  `@barrier_store` → `__ATOMIC_RELEASE`, `@barrier_load` → `__ATOMIC_ACQUIRE`
  at **`emitter.c` ~3098–3102**. This is the model to copy.

Two acceptable outcomes (pick the cheaper that keeps the derive-table honest):
- **(a)** Wire an `ordering` parameter through the atomic value-ops in BOTH
  emitter paths (AST ~3128–3152 and IR ~8358+), mirroring the fence-path
  precedent, then have the derive-table read the actual ordering.
- **(b)** Until (a) ships, the derive-table must conservatively declare
  `ordering = seq_cst` — the **actually-emitted truth**. A conservative
  over-declaration here is sound (seq_cst is the strongest); under-declaring
  would be the unsound direction.

Critical constraint that bounds this step's ambition: **`memory_barrier` /
positional ordering does NOT fold soundly** and stays out of the composition
fold vocabulary (STEP 3). Ordering is the low-low cell of the observability
spectrum (unsound fold × unobservable witness) — it stays a NAMED FLOOR
(taint, STEP 6), never witnessed. ZER's own Session-G evidence: Phase 3 in-block
ordering enforcement was ABANDONED because it false-positived the canonical
libpmem CLWB+SFENCE idiom (CLAUDE.md **~1295–1296**). Do not re-attempt
positional ordering enforcement here.

---

### STEP 3 — S2: composition (the flexible layer)

Add the composition node and the fold. Two new node kinds:
- `NODE_COMPOSED_BIND` — has NO `instructions` field. The composed-binding
  grammar has no mnemonic production: you literally cannot write freeform asm in
  a `compose` body (this is the `@inttoptr`-shaped gate for asm). A composed op
  recombines leaves/ops and other compositions only.
- `NODE_LEAF_BIND` — the ONLY node where raw asm (a real mnemonic) is permitted,
  and only behind the structural fence of STEP 4.

The composed op's effect row is DERIVED by the closed FOLD RULES over its
children's effect rows:
- `clobbers_register` / `clobbers_flags` → fold by **UNION / OR** (sound:
  superset of clobbers is always safe to assume).
- `requires_aligned(n)` → fold by **MAX** (sound: the strictest child alignment
  is the requirement).
- `requires_nonzero` → OR (sound: any child that traps on zero makes the
  composite require nonzero).
- positional `memory_barrier` / ordering → **EXCLUDED from the fold vocabulary**
  (max-ordering-of-children is unsound — STEP 2 rationale; ordering stays
  value-intrinsic only, never positional, never composed).

Author may state an EXPECTED effect row on a `NODE_COMPOSED_BIND`. **Declared vs
derived mismatch = COMPILE ERROR.** This is the ERBT (Effect-Row Binding Types)
inference check: the checker folds the children, infers the row, and rejects any
author claim that disagrees.

Finally, **repoint Layer-3 from DECLARED to DERIVED**: the static verifier that
checks Layer-3 call sites against effect rows must read the DERIVED row of the
composed op, not an author-declared one. Under Option E (the prior design)
Layer 3 was "verified against a lie" if a binding mis-declared; under
composition, a composed op cannot mis-declare (mismatch is rejected), and a
leaf's declaration is either derived (Tier A), witnessed (decidable Tier B), or
tainted (STEP 6). This is the closure theorem made operational: the floor
concentrates into the leaf set, reducing safety from O(bindings) prose
checklists to O(leaves) + O(1) fold proof.

---

### STEP 4 — Structural fence: `@bind` is a compile error unless on the closed privileged allow-list

`@bind` (the leaf mechanism that introduces raw asm) is a **COMPILE ERROR**
unless its op is on a closed privileged allow-list — the ~36 Tier-B ops outside
C's abstract machine (CR0/3/4 read/write, RDMSR/WRMSR, XSETBV, IN/OUT ports,
SYSCALL/SYSRET/IRET, CPUID/RDTSC/INVLPG/WBINVD, FSGSBASE, etc.). This is the
DEMAND-DRIVEN + FAIL-CLOSED rule: a leaf is implemented only when firmware uses
that op, and an unimplemented privileged op is a compile error, never a silent
hole. The burden tracks YOUR usage (tiny, slow — CR3/MSR/port/IRET/IF are
decades-old) not the ISA catalog, so it does NOT trip the per-instruction /
per-vendor proactive-scaling disqualifier.

Two negative tests (both **COMPILE ERRORS**), in `tests/zer_fail/`:
- `composed_bind_mnemonic.zer` — a `compose`/`NODE_COMPOSED_BIND` body that
  tries to contain a raw mnemonic. Must error: the grammar has no mnemonic
  production at the composition level.
- `bind_on_builtin_op.zer` — a `@bind`/`NODE_LEAF_BIND` (or category
  re-declaration) on an op that is builtin-backed (Tier A, in the STEP 1
  derive-table). Must error: builtin-backed ops are derived, not bound; raw asm
  for them is forbidden (GCC already has the portable builtin).

Follow the matrix-oracle methodology of the 8 existing oracles
(`tests/test_*_matrix.c` — `test_asm_matrix.c`, `test_hw_matrix.c`,
`test_escape_matrix.c`, etc.): the asm conformance harness should be a
positive/negative matrix, not ad-hoc one-off tests.

---

### STEP 5 — S3: the conformance witness tool (`tools/asm_witness/`) under qemu-system-x86_64

Build the out-of-tree witness tool that verifies a Tier-B leaf's DECLARED
effect row against observed behavior. ENV note: only **`qemu-system-x86_64`**
is present (verified `/usr/bin/qemu-system-x86_64`); there is no system QEMU for
ARM/RISC-V, no user-mode QEMU, no cross-compilers. QEMU emulates privilege, so
privileged ops ARE probeable in system mode by booting a ring0 stub.

Per-category probes (DECIDABLE categories only — observable + deterministic):
- `clobbers_register` / `clobbers_flags` — the **witness sweet spot**: fill all
  GPRs (and flags) with sentinels, run the leaf, read back. Register write-sets
  are structural, not data-dependent, so a single run is near-complete. This
  catches the classic under-declared-clobber bug that GCC and Rust CANNOT.
- `changes_privilege` — CPL readback before/after.
- `requires_nonzero` — zero-trap probe.
- `requires_aligned` — only where it actually traps (NOT x86 with AC off — that
  silently succeeds, so it is UNOBSERVABLE on this host).

UNOBSERVABLE categories are NOT witnessed (witnessing them produces FALSE
confidence, worse than honest taint): memory ordering (QEMU TCG flattens to TSO,
so a too-weak fence witnesses GREEN), and x86 alignment with AC off. These stay
TAINTED (STEP 6).

Witness artifact: a `.zerwitness` file binding `(op, ISA, category-profile,
blake2(asm-hash), qemu-version)`. The gate: a Layer-3 import / leaf-use of a
binding recomputes the asm hash; **missing witness or hash mismatch = COMPILE
ERROR** (a structural, FAIL-CLOSED precondition). Re-witnessing is lazy (only
when the hash changes). Note the demotion rationale: standalone QEMU-witness
certifies an EXISTENTIAL ("some categories observed on x86") that the gate
consumes as a UNIVERSAL ("declaration honored") — sound ONLY for the decidable
predicates above, which is why the tool is subordinate and scoped to those.

---

### STEP 6 — S4: the taint marker (the NAMED FLOOR)

Add the greppable taint marker for categories that are neither DERIVED (Tier A /
composition) nor WITNESSED (decidable Tier B). Reuse the existing
trust-boundary / audit-visible marker rather than inventing a new mechanism, and
propagate it to importers: any function importing a tainted binding is itself
tainted and **can never read as verified-green**. This is the honest residual,
not a compile error — the user is permitted to proceed, but the taint is
auditable and propagates.

Taint covers exactly: memory ordering (the irreducible-hardest category —
unsound fold × unobservable witness), non-x86 privileged asm on this host (an
ENV limit, not an architecture limit — installing `qemu-system-aarch64` +
cross-gcc would promote those from taint to witness, opt-in), and
`provenance_clear_on_output`. Everything else resolves to DERIVED or WITNESSED.

The split to keep straight: COMPILE ERROR = the gate (unwitnessed/hash-mismatch
leaf, `@bind` off the allow-list, declared-vs-derived mismatch, raw mnemonic in
a composition, author categories on a builtin-backed op). NAMED FLOOR = taint
(ordering, non-x86 privileged, provenance_clear_on_output).

---

### STEP 7 — Optional: `make drc-x86` native CI differential (NOT a gate)

Add an opt-in differential reference check as a native-x86 CI sanity layer. DRC
runs each leaf against a pure-ZER reference implementation under fuzzed
differential testing, catching **wrong-VALUE** bugs that no category mechanism
can detect (adc-drops-carry, off-by-one bsr, CAS-wrong-operand). It is a TEST
(process, not structure): it can only warn (not error) on cross-compile because
the reference and the leaf must both run on the same host. Keep it as an opt-in
`make drc-x86` CI target; **NEVER make it the gate** — making a probabilistic /
host-bound test a structural precondition would violate the closure model (a
gate must be a compile-time structural property, not a CI run, per ZER's
"run-a-CI-test is the trust-the-user model ZER rejects").

---

### Summary: what each step gates

| Step | Adds | Enforcement |
|---|---|---|
| 0 | Delete per-arch tables (`option_e_plan.md` C1–6) | — (removes the disqualifier) |
| 1 | Tier-A derive-table in `checker.c` ~10720–10890; `requires_nonzero` col; lock-free-width guard | COMPILE ERROR: author categories on builtin-backed op |
| 2 | Wire `ordering` through atomics (`emitter.c` ~3128–3152, ~8358+) or declare `seq_cst` | — (honesty of derived row) |
| 3 | `NODE_COMPOSED_BIND` (no instructions), `NODE_LEAF_BIND`, OR/MAX fold, exclude positional barrier, Layer-3 → DERIVED | COMPILE ERROR: declared ≠ derived row |
| 4 | Structural fence + `composed_bind_mnemonic.zer`, `bind_on_builtin_op.zer` | COMPILE ERROR: `@bind` off allow-list; mnemonic in composition |
| 5 | `tools/asm_witness/` under qemu-system-x86_64; `.zerwitness` = (op, ISA, profile, blake2(asm), qemu-version) | COMPILE ERROR: missing/stale witness |
| 6 | Taint marker (reuse trust-boundary marker), propagate to importers | NAMED FLOOR: ordering, non-x86 privileged, provenance_clear_on_output |
| 7 | `make drc-x86` native differential | CI only — never a gate |

Effort ascends 0→7; each step is independently shippable and green. STEP 0 is
pure deletion (already specified). STEPS 1–4 are `checker.c`-local and ride the
64 builtin + 196 ISA-dispatch sites. STEP 5 is the only out-of-tree artifact.
STEPS 6–7 are markers and an opt-in test.


---

# ===================================================================
# APPENDIX A — Option E and prior asm-safety architecture (PRESERVED)
#
# Everything below is the prior Option E architecture. It is the SUBSTRATE
# the Effect-Row Composition design above refines: the program-consequence
# vs hardware-consequence vocabulary, the three-layer model, the closed
# category vocabulary, the structure/semantics straddle (1.7.5), and the
# fault-attribution model (1.7.31) all CARRY FORWARD. Where the new design
# above conflicts with Option E below, the NEW DESIGN SUPERSEDES.
# ===================================================================

# ZER-Asm Safety — Level C: Defer to GCC, Frozen Core

**Status:** Planning document. Decision finalized 2026-05-12 (Level C).
**Architectural pivot to Level D locked in 2026-05-31 (see Section 1.6).**
**Architectural refinement to Option E — Three-Layer / No-Favored-ISA — locked in 2026-06-06 (see Section 1.7). READ SECTION 1.7 FIRST — it is the current locked architecture; Level D's `@intrinsic_def` mechanism survives but its layering and ISA-reference assumptions are refined.**
Execution pending: Level C cleanup first, then Level D mechanism on top, with Option E's no-favored-ISA factoring applied.
**Date:** 2026-05-05 (drafted), 2026-05-10 (audit), 2026-05-11 (Phase A/B split),
2026-05-12 (Level C decision), 2026-05-12 (2-layer crystallization — see section 1.5),
2026-05-31 (Level D pivot — user-extensible intrinsics, see Section 1.6),
**2026-06-06 (Option E — three-layer, no-favored-ISA, program-consequence vocabulary applied to asm, structure/semantics straddle observation — see Section 1.7).**
**Supersedes:** the extension trajectory of `docs/asm_plan.md` (Session G Phase 5,
Z9/Z10/Z13, per-instruction database growth, register-table maintenance,
CPU feature gating tracking).
**Decision:** **Level C** — drop everything ISA-specific from ZER's compile-time
asm validation. Defer to GCC. Keep only the truly frozen safety layer.

> **POST-DISCUSSION UPDATE (2026-05-12, evening):** A long architectural
> conversation explored richer layers on top of Level C (per-operand
> annotations / "Tier 2", IR region wrappers like `@atomic_sequence`,
> auto-guard emission). All explored. All **DEFERRED indefinitely** in
> favor of the simpler **2-layer model: intent intrinsics (primary safe
> path) + raw asm in naked (escape hatch)**. See **section 1.5** for
> the crystallized final design. Level C content below is unchanged and
> remains the execution baseline; 2-layer simply confirms that nothing
> richer is needed on top of it.

**Scope:** Aggressive cleanup of ZER's asm safety infrastructure. ~7,000 lines
deleted. Compile-time safety preserved via hardcoded well-known UB classics
list (~12 frozen instructions) + Z-rules + naked-only restriction + 130
intrinsics. Register validation, instruction validity, CPU feature gating
all delegated to GCC's assembler.

**Effort to complete:** ~1-2 days, 6 commits.
**Maintenance after:** **TRULY zero.** No probe scripts. No per-arch tables.
No per-ISA-extension sync events. ZER works on every architecture GCC
supports — automatically.

**Architectural certainty:** HIGH.
- Matches Rust and Zig design (they don't validate register names either)
- ZER is a transpiler to C, so any C compiler's validation suffices
- GCC dominates ZER's target audience (embedded/firmware/kernel)
- All deleted features are duplicated by GCC's assembler anyway

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
1.4. [Extended Discussion (2026-05-12 evening) — Full Context Dump](#14-extended-discussion-2026-05-12-evening--full-context-dump) ← **Full conversation context (Tier 2, regions, clobber, Rust comparison, trust gaps, explicit-intent pattern, 99% coverage goal, all deferred ideas)**
1.5. [Final Design — Two-Layer Model (crystallization, 2026-05-12 evening)](#15-final-design--two-layer-model-crystallization-2026-05-12-evening) ← **Level C baseline (still correct)**
1.6. [Level D Pivot — User-Extensible Intrinsics with Explicit Contracts (added 2026-05-31)](#16-level-d-pivot--user-extensible-intrinsics-with-explicit-contracts-added-2026-05-31) ← **Mechanism still current; layering and ISA-reference refined by 1.7**
1.7. [Option E Pivot — Three-Layer Architecture, No Favored ISA, Program-Consequence Vocabulary (added 2026-06-06)](#17-option-e-pivot--three-layer-architecture-no-favored-isa-program-consequence-vocabulary-added-2026-06-06) ← **READ FIRST — CURRENT LOCKED ARCHITECTURE FOR ASM**
2. [The Decision in One Page](#2-the-decision-in-one-page)
3. [The Transpiler Nature of ZER — Why This Works](#3-the-transpiler-nature-of-zer--why-this-works)
4. [GCC Coverage in Embedded — Reality Check](#4-gcc-coverage-in-embedded--reality-check)
5. [Why This Pivot — Bug History and Maintenance Reality](#5-why-this-pivot--bug-history-and-maintenance-reality)
6. [The Architectural Principle Being Applied](#6-the-architectural-principle-being-applied)
7. [The Three Levels Considered](#7-the-three-levels-considered)
8. [Journey of the Discussion](#8-journey-of-the-discussion)
9. [What We're Keeping at Level C](#9-what-were-keeping-at-level-c)
10. [What We're Deleting at Level C](#10-what-were-deleting-at-level-c)
11. [The Hardcoded UB Classics List](#11-the-hardcoded-ub-classics-list)
12. [Integration with Existing Safety Systems](#12-integration-with-existing-safety-systems)
13. [Safety Coverage Matrix — Level A vs B vs C](#13-safety-coverage-matrix--level-a-vs-b-vs-c)
14. [Production-Compiler Precedents](#14-production-compiler-precedents)
15. [Effort Breakdown — 6 Commits](#15-effort-breakdown--6-commits)
16. [Implementation Strategy](#16-implementation-strategy)
17. [Testing Strategy](#17-testing-strategy)
18. [Risks and Mitigations](#18-risks-and-mitigations)
19. [Open Questions Decided](#19-open-questions-decided)
20. [Out of Scope (Explicitly NOT Doing)](#20-out-of-scope-explicitly-not-doing)
21. [The "Smart Language vs Smart Compiler" Principle](#21-the-smart-language-vs-smart-compiler-principle)
22. [Fresh-Session Onboarding Checklist](#22-fresh-session-onboarding-checklist)
23. [Appendix A: Concrete File-by-File Deletion List](#appendix-a-concrete-file-by-file-deletion-list)
24. [Appendix B: The Hardcoded UB Classics — Concrete Contents](#appendix-b-the-hardcoded-ub-classics--concrete-contents)
25. [Appendix C: What ZER's Public Safety Claim Becomes](#appendix-c-what-zers-public-safety-claim-becomes)
26. [Appendix D: Mapping from asm_plan to Level C](#appendix-d-mapping-from-asm_plan-to-level-c)
27. [Appendix E: References](#appendix-e-references)

---

## 1. Executive Summary

ZER asm safety reached a decision point in May 2026. The original direction
(`docs/asm_plan.md`, D-Alpha-7.5 Phase 2) committed to building per-instruction
safety knowledge into the compiler: vendored instruction tables, classification
rules, register-name validation, CPU feature gating, and a CFG-aware
OrderingState pass (Session G).

By 2026-05-02, ~225 hours had been invested and ~80 hours remained (Session G
Phase 5 + Z9/Z10/Z13 + ongoing per-ISA-extension maintenance).

**Through a multi-day architectural discussion (2026-05-12), three increasingly
aggressive cleanup levels were considered:**

- **Level A** (initial proposal): Stop extending. Keep register tables, F7-light,
  F7-full Step 2, CPU feature gating. Add annotations + small implicit table.
  ~3-4 months. Some ongoing GCC-coupled maintenance.

- **Level B**: Drop register tables additionally. Keep CPU feature gating + Z-rules.
  Defer register name validation to GCC. ~3-4 months.

- **Level C** (CHOSEN): Drop register tables AND CPU feature gating AND probe
  scripts AND 8-category framework AND instruction tables. Defer everything
  ISA-specific to GCC. Keep only the frozen core (Z-rules, naked-only,
  intrinsics, ~12 hardcoded UB classics). **~1-2 days execution, truly zero
  maintenance forever.**

**Level C was selected because:**

1. ZER is a transpiler to C — the user's C compiler validates ISA-specific
   things anyway. Register tables and CPU feature gating duplicate work GCC
   already does.

2. ZER targets embedded/firmware/kernel developers — GCC is essentially universal
   in this audience. Deferring to GCC means ZER works on every architecture GCC
   supports, automatically, with zero ZER-side effort.

3. ZER is sole-developer-maintained. Every ongoing-maintenance dependency is a
   real cost. Level A/B keep small maintenance burdens that compound over years.
   Level C eliminates them entirely.

4. Production-compiler precedent — Rust and Zig don't validate register names
   either. They defer to LLVM/GCC's assembler. Level C matches their pattern.

5. "Fit all architecture" is actually STRONGER at Level C. Level A commits to
   3 archs (x86_64, aarch64, riscv64). Level C inherits GCC's full architecture
   matrix (15+ archs including AVR, MSP430, Xtensa, PowerPC, MIPS, etc.).

**The result:**

| Property | Original asm_plan (fully done) | Level C (this plan) |
|---|---|---|
| Compile-time safety coverage | ~99% in scope | ~99% in scope (same) |
| ZER-side maintenance burden | Perpetual (~1-2 events/year) | Zero |
| Architecture coverage | 3 archs maintained | Every arch GCC supports |
| Per-instruction database | ~120 entries growing | None |
| Probe scripts | 2 (register + instruction) | None |
| Register tables | 4 vendored files | None |
| CPU feature enum | ~14 entries managed | None — defer to GCC -m flags |
| Effort remaining | ~80 hours + perpetual | ~1-2 days one-time |
| Code surface | ~7,000 lines of asm-safety infrastructure | ~600 lines of asm-safety code |

**Net effect:** same safety, dramatically less code, ZERO ongoing work,
broader architecture support.

---

## 1.4. Extended Discussion (2026-05-12 evening) — Full Context Dump

This section records the multi-hour architectural conversation that took
place after the Level C decision was made, which explored richer designs
on top of Level C and ultimately rejected them in favor of the 2-layer
crystallization in section 1.5. Captured here so fresh sessions don't
re-litigate the same ground or accidentally revive a deferred idea.

### 1.4.1. Where we started — Level C was already decided

Going into the evening discussion, Level C was already committed:
- Delete instruction tables, register tables, probe scripts, CPU feature enum
- Keep 130 intrinsics, Z-rules, naked-only, hardcoded ~12 UB classics
- Defer everything ISA-specific to GCC
- ~1-2 days execution, zero ongoing maintenance

The discussion was about whether richer safety mechanisms should be added
on top of Level C to push beyond what GCC's assembler validates.

### 1.4.2. The "Tier 2 annotations" exploration

**The idea:** add user-facing per-operand annotations on raw asm operand
bindings to declare semantic preconditions. Example:

```zer
asm {
    instructions: "bsr %1, %0"
    outputs: { "rax" = result }
    inputs:  { "rbx" = mask, requires: nonzero }    // ← Tier 2 annotation
    safety: "..."
}
```

The compiler would dispatch `requires: nonzero` to the existing VRP system.
If VRP can prove `mask` is nonzero at the call site, the asm compiles. If
not, compile error. **No theorem prover, no SMT, no formal proof — just
existing dataflow analyses run through asm operand boundaries.**

**Annotation vocabulary explored:**
1. `requires: nonzero` → VRP check
2. `requires: range(a, b)` → VRP check
3. `requires: aligned(N)` / `align: N` → alignment infrastructure
4. `requires: nonnull` → optional-unwrap
5. `requires: kernel_context` → context flag
6. `opens_state: X` → state machine (user-defined name)
7. `closes_state: X` → state machine
8. `requires_after: X` → state machine

Eight kinds, frozen vocabulary. Compiler-side maintenance: zero growth
beyond initial implementation.

**Why explored:** would let users get compile-time precondition checks for
asm patterns not in the intrinsic catalog (AMX, SVE, future ISA extensions,
custom user operations) without writing a full intrinsic.

**Why DEFERRED:**

1. **Redundancy with intrinsics.** For the common case (~95% of asm),
   intrinsics already encode preconditions. Tier 2 only helps the residual
   ~5% of raw asm in niche cases.
2. **Two-ways-to-express problem.** Same precondition declared two ways:
   either as an intrinsic body or as a Tier 2 annotation. Violates "one
   obvious way to do it."
3. **User must know to add the annotation.** Same meta-trust problem as
   intrinsic selection — if user is "dumb" and doesn't add the annotation,
   no compile-time benefit. Tier 2 doesn't help dumb users.
4. **Caller-side guards are equivalent.** A regular `if (mask == 0) trap;`
   before calling a `naked` function gets elided by VRP if provable —
   same result, no new language surface.
5. **~3-4 weeks of implementation work** for marginal benefit on niche
   cases that ~1-2% of users hit.

**Verdict:** annotations defer to post-v1.0 if real demand emerges from
production use.

### 1.4.3. The "IR region wrappers for asm" exploration

**The idea:** structured block wrappers for multi-block asm patterns:

```zer
@atomic_sequence {
    asm { instructions: "lr.w t0, (a0)" /* ... */ }
    asm { instructions: "addi t0, t0, 1" /* ... */ }
    asm { instructions: "sc.w t1, t0, (a0)" /* ... */ }
}
```

The region carries shared invariants (LR/SC pairing, memory ordering, etc.)
that the compiler tracks across all blocks in the region body. Same pattern
as existing `@critical { }`, `defer { }`, `@once { }`.

**Region keywords proposed:**
- `@atomic_sequence { }` — LR/SC, MONITOR/MWAIT pairings
- `@cache_sync_region { }` — CLWB+SFENCE persistence sequences
- `@dma_buffer_op(buf, len) { }` — DMA setup/transfer/complete with auto-emit cache flush + invalidate
- `@transaction { }` — future transactional memory support

**Why explored:** regions are conceptually consistent with ZER's existing
safety-scope patterns (naked, @critical, defer, @once, shared struct, comptime),
and they cleanly handle multi-block coordination that flat annotations
struggle with.

**Why DEFERRED:**

1. **Composite intrinsics handle the same patterns cleaner.** `@atomic_cas(...)`
   does what `@atomic_sequence { lr.w + ... + sc.w }` does — and the intrinsic
   form is simpler from the user's perspective. The region exposes implementation
   detail (manual LR/SC) that the intrinsic abstracts away.
2. **Adds language surface without unlocking new safety.** Anything a region
   would track, a well-designed composite intrinsic tracks more cleanly.
3. **The cases regions would handle (LR/SC pairing) are already covered.**
   F7-light state machine + composite intrinsics together cover the existing
   real cases.
4. **"User wraps niche asm in a region just to get safety" is a worse UX
   than "user calls an intrinsic that has safety baked in."**
5. **~2-3 weeks of implementation work** for marginal benefit.

**Verdict:** asm-specific regions deferred. General-purpose regions like
`@critical`, `defer`, `@once` stay — they're language features, not asm-safety
features.

### 1.4.4. The "auto-guard emission" exploration

**The idea:** for Tier 2 annotations where VRP can't prove the precondition
at compile time, emit a software runtime check (`if (!cond) _zer_trap(...)`)
before the asm. This would eliminate hardware-dependence: wrong precondition
always produces a deterministic ZER-controlled trap, regardless of CPU.

Example: user declares `requires: aligned(16)` on a MOVAPS operand. If VRP
can't prove the address is 16-byte aligned, ZER emits:

```c
if ((uintptr_t)addr & 15) {
    _zer_trap("asm MOVAPS operand alignment precondition failed");
}
__asm__ __volatile__ ("movaps (%0), %%xmm0" : : "r"(addr));
```

**Why explored:** the hardware trap fallback is hardware-dependent:
- Hosted Linux/BSD/macOS user mode: ~95% reliable (SIGSEGV, SIGFPE, SIGILL)
- Bare-metal Cortex-M0 / no-MMU: hardware doesn't trap on misalignment
- Boot code pre-exception-handler: undefined behavior (no trap path)

Auto-guard would close this gap by emitting software checks ZER controls.

**Why DEFERRED:**

1. **Requires Tier 2 to exist.** Auto-guard is meaningful only if users can
   declare preconditions on raw asm. If Tier 2 is deferred, auto-guard has
   nothing to emit guards for.
2. **Intrinsics already emit their own guards.** `@bit_scan_reverse(mask)` can
   check `mask != 0` internally before emitting `bsr`. Layer 1 already does
   this — auto-guard would be parallel infrastructure for the rare Layer 2
   case.
3. **Performance cost on tight loops.** Auto-emitted runtime checks ~2-4
   instructions per asm block. Acceptable for safety-critical builds,
   noise in hot paths. Without opt-in, defeats the point of inline asm.

**Verdict:** auto-guard deferred along with Tier 2.

### 1.4.5. The clobber gap discussion

A long subthread explored whether ZER can detect missing clobber declarations
in raw asm. The conclusion: **no production language detects this without
either per-instruction database (maintenance hell) or formal proof per
asm block (Vale-tier manual work).**

**The mechanism of the bug:**

```
Wrong precondition (e.g., BSR on zero):
  → asm instruction TRIES to execute
  → CPU exception (#DE/#GP/#AC)
  → SIGFPE/SIGSEGV/SIGBUS
  → LOUD failure, debuggable

Wrong clobber (e.g., user forgot to declare rdx clobbered by DIV):
  → asm instruction DOES execute successfully
  → register rdx silently contaminated with leftover from DIV
  → next ~50 instructions run with garbage in rdx
  → eventually flows into a load address → SIGSEGV
  → BUT debugger points to wrong location, far from cause
  → SILENT failure, hard to debug
```

**Why this is "the effect system gap":**

Languages track *some* effects (allocation, mutation, lifetimes, types). They
don't track *all* effects. Register state is an effect that lives outside
every mainstream language's type system. The contract between user-asm and
GCC is **invisible to the language**.

**Category:** invisible-contract effect bug. The clobber gap is the
asm-shaped instance of a general phenomenon. Other instances in safe code:

| Category instance | Invisible contract | Wrong tool = silent bug |
|---|---|---|
| Wrong clobber list (asm) | "rdx preserved" | Corrupted register downstream |
| Wrong atomic memory ordering (safe Rust) | Happens-before edges | Stale load on weak-memory hardware |
| Wrong Mutex lock order (multi-lock) | Global lock ordering | Deadlock at runtime |
| Wrong drop-order reliance | Field declaration order | Behavioral shift on refactor |
| Wrong Cell vs RefCell vs Mutex choice | "Do I need runtime borrow checks?" | Panic at runtime or perf cost |
| Wrong cinclude C function signature | C ABI | Silent ABI corruption |
| Wrong MMIO range declaration | Hardware address layout | OS faults or wrong device |

In every case: compiler accepts syntactically-correct code; user picked
wrong tool; failure manifests downstream as silent consequence. **Same
shape across domains.**

### 1.4.6. The Rust comparison

A subthread compared Rust's safety story to ZER's. Key findings:

**Both pass the same test at the asm boundary.** Rust's `unsafe { asm!() }`
has zero clobber validation, zero precondition checks beyond operand types.
Same gap as ZER. Same gap as C. Rust's safety claim is **NOT** that asm is
safe — it's that the **safe subset** is checked and **unsafe is bounded**.

**Both implement the "composition" safety model.** Most code is in the safe
subset (genuinely checked). Escape hatches are explicit, isolated, audited.
The boundary between safe and unsafe is the type system's (or in ZER's case,
the 29 tracking systems') job.

**Where ZER catches more by default (the "smart compiler + dumb user" model):**

| Bug class | Rust mechanism | ZER mechanism |
|---|---|---|
| Reference cycle leak | User picks `Weak` for back-references | Handle is index, cycles impossible by construction |
| Lock ordering deadlock | User maintains global lock order manually | Same-statement multi-shared-type access = compile error |
| Uninitialized memory | User picks `MaybeUninit::assume_init` correctly | Everything auto-zeroed at declaration |
| Lifetime annotations | User writes `<'a, 'b>` on every fn | Escape analysis walks field/index chains automatically |
| Atomic ordering | User picks `Acquire`/`Release`/`SeqCst` correctly | Single SeqCst choice — no wrong choice possible |
| Stack overflow | User hopes for the best | `--stack-limit N` + recursion detection via DFS |
| Lock primitive choice | User picks `Mutex` vs `RwLock` vs `Cell` vs `RefCell` | `shared struct` auto-locks; `shared(rw) struct` auto-rwlocks |
| Spawn data race | Auto-traits `Send`/`Sync` (user may `unsafe impl` wrong) | Spawn-target body scanned at compile time |
| Channel size choice | User picks buffer size, wrong = OOM or lockstep | `Ring(T, N)` always bounded; unbounded not available |
| Drop order reliance | Field declaration order matters silently | `defer` makes order explicit; no implicit drop ordering |
| Pin for self-referential | User knows when to pin futures | Async compiles to flat state machine; no Pin needed |
| Bounds proof | User picks `.get()` vs `[]` | VRP proves indices; auto-guard only if unprovable |
| Sign/width conversion | User picks `as` vs `try_into()` | Implicit narrowing rejected; `@truncate`/`@saturate` explicit |

**The architectural insight:**

```
Rust:  Smart user × Medium-smart compiler = safe
       (user encodes invariants in types; borrow checker enforces type discipline)

ZER:   Dumb user × Very-smart compiler = safe
       (compiler infers invariants from dataflow; user writes plain code)
```

Both reach roughly equivalent safety. Rust pushes complexity into the user's
brain (lifetimes, Send/Sync, Pin, MaybeUninit, Cell-family choice, atomic
ordering). ZER pushes complexity into the compiler (29 tracking systems doing
per-function dataflow inference).

**Where Rust catches more (honest):**

The narrow real gap is **API expressiveness**, not safety coverage:
- Rust's signature `fn foo<'a>(x: &'a T) -> &'a U` declares "result tied to
  arg lifetime" at the type level. ZER has no such surface syntax.
- Rust's trait bounds (`T: Hash`) constrain generic APIs. ZER's
  monomorphization defers to per-stamp type-checking.
- Rust's `&mut` aliasing rule prevents some patterns expressible only with
  lifetimes.

**But:** these are API design gaps, not safety gaps. The underlying bugs Rust
would catch via these features, ZER catches via dataflow analysis anyway. The
user-visible difference is "you can't write a library API that statically
refuses aliasing in ZER" — but if your code has the bug, ZER still catches it.

**Net:** ZER catches every bug class Rust catches in the safe subset. The
mechanisms differ. The user experience differs (less expertise required for
ZER). The audit surface and escape-hatch model are equivalent.

### 1.4.7. The trust gap pattern (universal across ZER)

A subthread identified that the clobber gap (user declares, compiler trusts,
wrong declaration causes silent issue) is a **universal pattern** across ZER,
not unique to asm. Every safety feature that bridges to external reality
has the same shape:

| Feature | User declares | Compiler trusts | If wrong, what happens |
|---|---|---|---|
| `mmio 0x4000..0x4FFF;` | Hardware address layout | Address valid | OS fault at runtime |
| `volatile *u32` | Hardware requires unoptimized access | Optimizer leaves accesses | Optimizer removes essential reads silently |
| `shared struct` | Data accessed across threads | Auto-lock generated | Silent race on bypass |
| `threadlocal u32` | Per-thread storage | Each thread has own copy | Lost updates if actually shared |
| `move struct` | Resource has unique ownership | Tracking enforced | Over-restrictive (false positives) |
| `keep` parameter | Pointer may be stored globally | Storage allowed | Escape analysis fires false-positive |
| `@ptrcast(*T, opaque)` | Memory layout is T | Type assertion accepted | Type confusion silent for cross-language |
| `cinclude` C signatures | Function signature matches actual C | Type-checked against decl | ABI silent corruption if mismatch |
| Asm clobber list | Registers modified by asm | Trusted verbatim | Silent register corruption |
| Asm precondition (if Tier 2 existed) | Operand semantic | Dispatched to dataflow | Wrong type of check applied |

**Pattern:** ZER (and every safe language) trusts user declarations about
external reality. The compiler enforces ZER-side consequences of those
declarations. External reality is the user's responsibility.

**Mitigation strategy across ZER (universal):**
1. Multiple overlapping annotations (defense in depth)
2. Runtime/OS/hardware traps for hardware-related cases
3. Audit-ability (all annotations greppable)
4. Documentation conventions (`safety:` strings, header comments)
5. For safety-critical: opt-in formal verification (`@verified_spec` v2.x+)

**No additional core complexity is going to fix this universally** because
fixing it requires either per-item databases (maintenance hell) or formal
proofs (Vale-tier manual work). ZER's design accepts this universal trust
gap and uses the same mitigation strategy across all instances.

### 1.4.8. The hardware-dependent trap reality check

A subthread examined whether the "CPU trap catches wrong precondition" claim
is actually reliable. Conclusion: **conditional on context, not universal.**

```
Hardware trap fallback reliability:

  Linux/BSD/macOS user mode (hosted):       ~95% reliable (loud crash)
  Linux kernel module:                       ~90% reliable
  Modern RTOS (Zephyr, FreeRTOS):           ~85% reliable
  Bare-metal Cortex-M with handlers:        ~70% reliable
  Bare-metal boot code (pre-handler):        ~30% reliable
  Cortex-M0 / AVR / no-MMU MCU:              ~40% reliable
  Inside an exception handler:               unreliable (double-fault risk)
```

Specific cases where hardware does NOT trap:
- Misaligned scalar access on x86 (EFLAGS.AC off by default in user mode)
- Misaligned access on ARM Cortex-A (SCTLR_EL1.A off by default in user mode)
- Misaligned access on Cortex-M0/M0+ (no alignment check exists)
- RDTSC in user mode (works without trap even though it reads privileged time)
- Wrong arithmetic instruction (computes wrong value, no trap)
- Undeclared register clobbers (silent register corruption)
- Wrong FP rounding mode (slightly wrong float)

**Implication:** the public safety claim about wrong-asm-traps-at-runtime
needs to be honest about hardware/OS dependence. ZER's actual guarantee is
at the operand boundary (Z-rules), not for asm body content. Hardware trap
is "usually clean" property of the runtime environment, not a ZER guarantee.

This is the same scope every safe language has for inline asm. Honest.

### 1.4.9. The three tiers of contracts (clarification)

A subthread distinguished three tiers of "contract" mechanisms to clarify
what ZER does and doesn't claim:

**Tier 1: Documentation comments (no enforcement)**
- `// SAFETY: caller must ensure mask != 0` in Rust
- `safety: "..."` string in ZER's asm syntax
- Convention only, zero compiler check

**Tier 2: Annotations checked by existing static analyses (no proofs)**
- User declares a precondition; compiler dispatches to existing dataflow
  analysis (VRP for ranges, alignment infrastructure, optional unwrapping)
- Compile error if analysis can't satisfy the precondition
- **Deterministic, no SMT solver, no theorem prover, no manual proof work**
- ZER already does this for hardcoded UB classics (~12 frozen entries).
- Tier 2 user-facing extension was explored and deferred (see 1.4.2).

**Tier 3: Annotations checked by theorem provers / SMT solvers (formal proofs)**
- SPARK Ada's `Pre`/`Post`/`Global` clauses
- Frama-C ACSL's `\valid`, `\forall`, etc.
- Vale's Coq proof obligations
- Compiler generates proof obligations sent to SMT/theorem prover
- May succeed, fail, or time out; user may need to write proofs manually
- **This is what "formal verification" means in industry**

**ZER's position:**
- Tier 1 always: `safety:` strings on every asm block (≥ 30 chars via S4)
- Tier 2 partial: hardcoded UB classics (the ~12 frozen entries)
- Tier 2 user-facing: explored, **deferred** (see 1.4.2)
- Tier 3: out of core scope; `@verified_spec` opt-in for v2.x+, niche audience

**Critical clarification:** "contracts" in the SPARK/ACSL/Vale sense ARE
proof systems (Tier 3). ZER's hardcoded UB classics dispatch and any future
Tier 2 are NOT proof systems — they're existing dataflow analyses dispatched
through annotation surface. **No mathematical proof is involved.** No SMT
solver is in the toolchain. No Coq is required.

### 1.4.10. The "smart language vs smart compiler" principle (final)

```
                  SAFE DEFAULT FOR EVERYDAY USER
                            ↑
                            |
ZER's design ─────────► Compiler is smart (dataflow infers properties)
                            |
                            |
                            |
                            ↓
                  USER MUST KNOW INVARIANTS
                            ↑
                            |
Rust's design ─────► User encodes invariants in types
                            |
                            ↓
                  REQUIRES SOPHISTICATION
```

Both reach equivalent safety. The dial that differs is **expected user
sophistication.**

ZER's choice: assume the user is a mid-level C programmer who has never
heard of borrow checker. Compiler does the work. Safety guaranteed without
expertise.

Rust's choice: assume the user is a senior systems programmer who reads
the standard library. User provides expertise. Compiler enforces what user
encoded.

**For ZER's audience (embedded/firmware/kernel developers transitioning
from C), the "smart compiler + dumb user" model is the right design.**

### 1.4.11. Industry precedent — final confirmation

The 2-layer model (intrinsics primary + raw asm escape) is the **converged
industry pattern** for production kernel/RTOS development:

- **Hubris RTOS** (Oxide Computer): intrinsics for everything; raw `asm!()`
  in tiny `kernel/boot` crate only. 99% of code never touches asm.
- **Linux kernel**: `readl`/`writel`/`atomic_*`/`barrier` intrinsics
  everywhere; `.S` files only in `arch/<X>/boot/`, `entry/`, `head/`.
- **Zephyr RTOS**: intrinsics in headers; asm in `core/locore.S` and
  `arch/<X>/core/aarch32/cortex_m/exc_exit.S` etc.
- **FreeRTOS**: intrinsics + `portasm.S` per port.
- **STM32 / CMSIS**: `__DMB()`, `__WFI()`, `__NOP()` intrinsic macros; raw
  asm rare and confined to startup files.
- **Rust core::arch**: ~5000 intrinsics covering AVX/SSE/SVE/NEON; `asm!()`
  is the explicit unsafe escape.
- **GCC's own kernel headers** (`<x86intrin.h>`, `<arm_neon.h>`): intrinsics
  as the documented path; `__asm__` as the escape.

**Every production kernel/RTOS converged on this model. ZER's 2-layer
crystallization joins the converged norm.**

### 1.4.12. What "fully safe" means after 2-layer

The honest safety scope claim post-2-layer:

```
ZER guarantees:
  ✓ Memory safety through asm operands (Z-rules)
  ✓ Type safety on operand bindings (existing type system)
  ✓ Concurrency safety through operands (Z6)
  ✓ MMIO range validation (Z7 + mmio decl)
  ✓ Provenance tracking (Z4, Z5)
  ✓ Qualifier preservation (Z8)
  ✓ Move/transfer semantics (Z2)
  ✓ Hardcoded UB classics (~12 frozen, via AST mnemonic detection)
  ✓ LR/SC pairing (F7-light hardcoded state machine)
  ✓ Naked-fn isolation (MISRA Dir 4.3)
  ✓ Intent intrinsic preconditions (via existing dataflow at intrinsic body)
  ✓ Cross-architecture support (every arch GCC supports, ~15+ archs)

ZER does NOT guarantee:
  ✗ Algorithm correctness (Vale-tier territory, opt-in v2.x+)
  ✗ Microarchitectural attacks (hardware vendor problem)
  ✗ Clobber list completeness for raw asm (universal asm limit)
  ✗ cinclude signature correctness (C interop boundary)
  ✗ User-declared facts about external hardware/OS (trust boundary)
  ✗ Hardware-independence of trap behavior on raw asm (context-dependent)

Out of scope (acceptable, matches every safe language):
  ✗ Wrong precondition on niche raw asm instruction (CPU trap usually catches;
     ZER auto-guard would have helped but Tier 2 was deferred)
```

**This is the honest, audit-able safety claim.** No marketing inflation.
Matches Rust, Zig, Linux kernel, Hubris, every production safe language at
this tier.

### 1.4.13. Maintenance picture — final crystallization

```
ZER-side ongoing maintenance after one-time Level C cleanup:

  Intrinsic catalog (~130 today):
    Growth rate: ~1-2 entries / year (real demand only)
    Per-entry cost: ~30-50 lines if using GCC builtin (most cases)
                    ~100-200 lines if needs per-arch inline asm wrapper
    Total catalog code: ~3,000-5,000 lines, bounded forever

  Hardcoded UB classics list (~12 frozen entries):
    Growth rate: ~1 entry / decade (genuinely new well-known UB)
    Per-entry cost: ~3-5 lines in checker.c

  Z-rules infrastructure (Z1-Z8, Z11, Z12):
    Frozen. Zero growth. One-time setup.

  Per-arch register tables:        DELETED. Zero maintenance.
  Per-arch instruction tables:     DELETED. Zero maintenance.
  Probe scripts:                   DELETED. Zero maintenance.
  CPU feature enum:                DELETED. Zero maintenance.
  8-category framework:            DELETED. Zero maintenance.
  Session G ordering plumbing:     DELETED. Zero maintenance.

  Sub-extension support (AVX-512, AMX, SVE, SME, etc.):
    Inherited from GCC automatically. Zero ZER work.

  New architecture support (any arch GCC adds):
    Inherited from GCC automatically. Zero ZER work.

  Tier 2 annotation framework:     NOT BUILT. Deferred.
  IR region wrappers for asm:      NOT BUILT. Deferred.
  Auto-guard emission for Tier 2:  NOT BUILT. Deferred.

TOTAL ZER-side ongoing burden: ~1-2 hours per year, bounded forever.
NOT hellish. NOT continuous. NOT compounding.
```

### 1.4.14. The execution decision

The 6-commit cleanup plan in **section 16** remains correct and proceeds
as originally specified. The 2-layer crystallization in **section 1.5** is
a clarification of what Level C means in practice, NOT a plan revision.

**Optional follow-on (post-Level-C):** if real demand emerges for niche
intrinsics not in the current 130-entry catalog (AMX, SVE specifics,
cache-management wrappers), they can be added incrementally one at a time,
each as a small ~30-50 line addition. Not blocking, not urgent, demand-driven.

**Never blocking:** Tier 2 annotations, IR regions for asm, auto-guard
emission, per-instruction database, additional structural complexity. All
explored, all deferred indefinitely.

### 1.4.15. The "explicit intent via separate intrinsics" pattern

A late-conversation insight crystallized the architectural pattern ZER uses
to handle **sibling operations with identical input types but different
semantic intent.** This is the same shape as the clobber gap (invisible
contract, user must know intent) but the resolution is structural rather
than checked.

**The pattern: don't ship one silent operator; ship distinct intrinsics
with explicit names.**

| Domain | C's silent default (BUG-PRODUCING) | ZER's explicit-intent split |
|---|---|---|
| Width conversion | `(u32)big_u64` silently truncates | `@truncate(u32, val)` — explicit |
| Range conversion | `(i8)big_int` silently wraps | `@saturate(i8, val)` — explicit clamp |
| Bit reinterpretation | `*(u32*)&float_val` silently aliases | `@bitcast(u32, val)` — explicit |
| Integer → pointer | `(int*)addr` silently makes a pointer | `@inttoptr(*u32, addr)` — explicit, MMIO-validated |
| Pointer → integer | `(uintptr_t)ptr` silently | `@ptrtoint(ptr)` — explicit |
| Pointer type cast | `(B*)a_ptr` silent reinterpretation | `@ptrcast(*B, ptr)` — explicit, provenance-checked |
| Container choice | `malloc` for everything | `Pool / Slab / Ring / Arena` — distinct names |
| Atomic ordering | `atomic_load(ptr, ordering)` | **SeqCst only** — family collapsed |
| Lock type | User picks `Mutex` vs `RwLock` vs `Cell` | `shared struct` / `shared(rw) struct` — compiler picks |
| Bit search | One `bsr` mnemonic, UB on zero | `@bit_scan_reverse(mask)` — internal nonzero check |
| Cache ops (future, if added) | One asm `clflushopt` or `clwb` string | `@cache_flushopt` vs `@cache_writeback` — distinct names |
| MSR read/write | Same opcode prefix, different semantic | `@cpu_read_msr` vs `@cpu_write_msr` — distinct sigs |

**The principle:**

```
C model:    ONE operator (cast) picks "truncate" silently
            → user didn't know they were truncating
            → silent value corruption

ZER model:  THREE intrinsics (@truncate/@saturate/@bitcast)
            → user MUST type one
            → typing it IS the intent declaration
            → no silent path exists
            → forgetting = compile error, not silent bug
```

**Why this is a safety mechanism, not just ergonomics:**

The act of typing the intrinsic name **is** the intent declaration. There's
no fallback the compiler picks. The user can't "forget to think about overflow
semantics" because forgetting means the code doesn't compile. The language
literally cannot pick wrong for the user — the user picks, and picking is
logged in the source code as a permanent audit trail.

**Two distinct resolution strategies for sibling families:**

1. **Collapse the family** when there's a single "right default":
   - Atomic ordering → SeqCst only (5 orderings → 1)
   - Lock type → `shared struct` auto-picks (4+ primitives → 1 declaration)
   - Memory layout → auto-zeroing (no `MaybeUninit` family needed)

2. **Split into named siblings** when operations are genuinely different:
   - `@truncate` vs `@saturate` vs `@bitcast` — all valid, different intent
   - `Pool` vs `Slab` vs `Ring` vs `Arena` — different allocation semantics
   - `@cache_flushopt` vs `@cache_writeback` — different cache semantics

**The dumb-user safety floor:**

> User cannot silently pick wrong because the language has no silent path.
> They must type the intent. Once typed, the consequence follows from the
> declared intent.

**This pattern has a name in language design:**
- Zig: *"no hidden control flow, no hidden allocations, no hidden casts"*
- Rust: *"explicit is better than implicit"* (no implicit numeric conversion)
- Ada/SPARK: *"strong typing with explicit conversions"*
- Python's Zen: *"explicit is better than implicit"*
- ZER (implicit via design): **"every conversion is a named intrinsic"**

All converge on the same insight: **silent operators are bug-class generators.**
Named operators with distinct intent are safer because the picker is
mechanically forced to declare which operation they want.

**Relationship to the clobber gap (1.4.5):**

| Gap shape | Clobber (raw asm) | Sibling intrinsic |
|---|---|---|
| Invisible contract | "rdx preserved" | "this is truncate semantics" |
| User must know intent | Yes | Yes |
| Compiler can verify? | No (needs per-instruction DB) | Often yes (type system / VRP) |
| Resolution | None automatic (intrinsics absorb 95%) | Explicit naming forces intent declaration |
| Silent failure? | Yes (register corruption downstream) | No (named operator = audit trail) |

**Key insight:** the intrinsic-sibling case is structurally better than the
clobber case because the explicit name converts the bug from "silent semantic
corruption" into "code review can see which operation was picked." Even when
the compiler can't verify intent, the source code records it.

**Status:** this pattern is already practiced throughout ZER's existing 130
intrinsics + container builtins + conversion intrinsics. Not a new design
addition — a documentation crystallization of the principle already in use.
Future intrinsic additions should follow the same rule: **if two operations
are semantically different, give them different names. Never ship one
operator that silently picks.**

### 1.4.16. The 99% intrinsic coverage goal (zero `.S` files needed)

**Goal stated by the user 2026-05-12 evening:** intrinsics should be expansive
enough that ZER firmware/kernel projects need **zero standalone `.S` assembly
files**. GCC handles all per-arch codegen via intrinsic bodies; raw asm in
`naked` functions covers the irreducible ~1% (boot stubs, vector tables,
hand-tuned hot loops).

**This goal is achievable and matches the converged industry pattern.**
Hubris RTOS (Oxide Computer, Rust-based, in production hardware) hit
exactly this target: 99% of their kernel is intrinsic-based; ~200 LOC of
raw asm in a tiny `boot` crate; no standalone `.S` files in their codebase.

This subsection maps the path from ZER's current ~130-intrinsic catalog
(covering ~95% of typical kernel/firmware needs) to the ~150-170-intrinsic
target (covering ~99%).

#### Current coverage map (what .S files traditionally cover vs ZER today)

| Traditional `.S` use case | ZER current coverage | Status |
|---|---|---|
| Atomic ops, CAS, fetch-add | `@atomic_*` (15 intrinsics, D-Alpha-1) | ✓ Covered |
| Memory barriers | `@barrier_*`, `@barrier_acq_rel` | ✓ Covered |
| Bit ops (popcount, clz, ctz, ffs, parity) | `@popcount`, `@ctz`, `@clz`, `@ffs`, `@parity` (D-Alpha-2) | ✓ Covered |
| Byte swap | `@bswap16/32/64` (D-Alpha-2) | ✓ Covered |
| MSR / CR / XCR0 access | `@cpu_read_msr`, `@cpu_write_cr*`, `@cpu_*_xcr0` (D-Alpha-9) | ✓ Covered |
| Port I/O (in/out) | `@port_in8/16/32`, `@port_out8/16/32` (D-Alpha-13) | ✓ Covered |
| Interrupt enable/disable/wait | `@cpu_disable_int`, `@cpu_enable_int`, `@cpu_wait_int` (D-Alpha-3) | ✓ Covered |
| Interrupt state save/restore | `@cpu_save_int_state`, `@cpu_restore_int_state` (D-Alpha-3) | ✓ Covered |
| Cache management (CLFLUSHOPT/CLWB/MOVNTI) | `@cache_flushopt`, `@cache_writeback`, `@nt_store` (D-Alpha-13) | ✓ Covered |
| CPU feature detection | `@cpu_cpuid`, `@cpu_cpuid_ecx`, `@cpu_vendor_id`, `@cpu_feature_bits` (D-Alpha-10/14) | ✓ Covered |
| Context save/restore (callee-saved) | `@cpu_save_context`, `@cpu_restore_context`, `@cpu_*_fpu` (D-Alpha-4) | ✓ Covered |
| Extended state save/restore (XSAVE) | `@cpu_xsave`, `@cpu_xrstor`, `@cpu_fxsave`, `@cpu_fxrstor` (D-Alpha-13/14) | ✓ Covered |
| Privileged mode transitions | `@cpu_syscall`, `@cpu_sysret`, `@cpu_iret` (D-Alpha-12) | ✓ Covered |
| Hypercalls / firmware calls | `@cpu_hypercall`, `@cpu_sbi_call`, `@cpu_smc_call` (D-Alpha-12/13) | ✓ Covered |
| Debug registers (DR0-DR7) | `@cpu_read_dr`, `@cpu_write_dr` (D-Alpha-13) | ✓ Covered |
| Performance counters | `@cpu_read_pmc` (D-Alpha-13) | ✓ Covered |
| Stack / thread pointer / flags read | `@cpu_read_sp`, `@cpu_read_tp`, `@cpu_read_flags` (D-Alpha-10) | ✓ Covered |
| Power management (sleep, idle, monitor/mwait) | `@cpu_deep_sleep`, `@cpu_idle_hint`, `@cpu_mwait`, `@cpu_umwait` (D-Alpha-11/14) | ✓ Covered |
| Privilege query | `@cpu_get_priv_level` (D-Alpha-12) | ✓ Covered |
| Control-flow integrity | `@cpu_endbr` (CET-IBT) (D-Alpha-14) | ✓ Covered |
| FS/GS segment bases (FSGSBASE) | `@cpu_read_fsbase`, `@cpu_write_fsbase`, GS variants (D-Alpha-13) | ✓ Covered |
| Cache disable/enable (privileged) | `@cpu_cache_disable`, `@cpu_cache_enable` (D-Alpha-14) | ✓ Covered |
| End-of-interrupt | `@cpu_eoi` (D-Alpha-14) | ✓ Covered |
| Page fault address | `@cpu_read_cr2` (D-Alpha-14) | ✓ Covered |
| Legacy FPU init | `@cpu_fpu_init` (D-Alpha-14) | ✓ Covered |
| **Subtotal current intrinsics** | **~130 (D-Alpha-1 through D-Alpha-14)** | **~95% of typical use** |

The 130-intrinsic catalog was deliberately built for kernel/firmware needs
across the 14 D-Alpha batches. Coverage today already exceeds what most
embedded RTOS projects need.

#### The gap — ~15-20 intrinsics to push coverage from ~95% to ~99%

These are remaining `.S`-file use cases that the current catalog doesn't
fully address. Adding them is **optional follow-on work, not blocking
Level C execution**.

| Gap area | Proposed intrinsic | Implementation strategy |
|---|---|---|
| **Boot stack setup** (before C runtime) | `@cpu_set_stack(addr)` | Per-arch inline asm wrapper in intrinsic body |
| **Vector table install** | `@cpu_set_vector_table(addr)` | x86: LIDT; ARM: VTOR write; RISC-V: stvec |
| **Unconditional jump (no return)** | `@cpu_jump_to(addr)` | Per-arch JMP/B/JR; marked `__attribute__((noreturn))` |
| **Full context save** (incl. SP/PC/PSR) | `@cpu_save_full_context(*u8 buf)` | Beyond callee-saved subset (D-Alpha-4 has callee-saved only) |
| **Full context restore** | `@cpu_restore_full_context(*u8 buf)` | Inverse of above |
| **IRQ vector entry prologue** | `@irq_entry()` | Stack alignment + register save per arch ABI |
| **IRQ vector exit epilogue** | `@irq_exit()` | Restore + IRET / RFI / SRET |
| **Atomic max/min via CAS loop** | `@atomic_fetch_max`, `@atomic_fetch_min` | Composite intrinsic (CAS loop) |
| **Aligned vector load/store** | `@vec_load_aligned_128/256/512`, store versions | GCC vector builtins |
| **Saturating arithmetic** | `@sat_add`, `@sat_sub`, `@sat_mul` | GCC `__builtin_*_overflow` + clamp where available |
| **Volatile memcpy/memset** (MMIO) | `@memcpy_volatile`, `@memset_volatile` | Cannot be optimized away — for memory-mapped regions |
| **Cache flush range** | `@cache_flush_range(*u8 addr, usize len)` | Loop wrapper over CLFLUSHOPT / DC CVAC |
| **Cache invalidate range** | `@cache_invalidate_range(*u8 addr, usize len)` | Loop wrapper over CLFLUSH+invalidate / DC IVAC |
| **DMA buffer prep/complete** | `@dma_buffer_prep`, `@dma_buffer_complete` | Cache flush + memory barrier composite |
| **TLB invalidation** | `@tlb_flush_all`, `@tlb_flush_page(addr)` | INVLPG / TLBI / SFENCE.VMA |
| **CPU pause / spin-loop hint** | `@cpu_pause()` | PAUSE / YIELD (spin-loop optimization hint) |
| **Random number** (RDRAND / RDSEED) | `@cpu_rdrand`, `@cpu_rdseed` | x86 hardware RNG with retry loop |
| **Crypto primitives** (if AES-NI present) | `@aes_enc_round`, `@aes_dec_round` (optional) | AES-NI intrinsics already in GCC |
| **SHA primitives** (if SHA-NI present) | `@sha1_msg1/2`, `@sha256_msg1/2` (optional) | SHA-NI intrinsics already in GCC |
| **Carryless multiply** (PCLMULQDQ) | `@clmul_64x64` (optional) | For CRC, GCM mode etc. |

**~15-20 additions** depending on how many crypto helpers are bundled.
After this phase: **~150-170 intrinsics, ~99% coverage of typical kernel/
firmware needs**.

#### The irreducible minimum (~1% raw asm in naked fn, cannot be intrinsics)

Even with maximum intrinsic coverage, three narrow categories will always
need raw asm in `naked` functions. These are the same categories Hubris,
Tock, seL4, Linux, and every production kernel maintain as small,
hand-audited asm:

**Category 1: Boot entry before C runtime exists** (~50-100 LOC per project)
```zer
naked void _start() {
    asm {
        instructions: "
            ldr sp, =_stack_top    // set stack ptr
            ldr r0, =_bss_start    // BSS zeroing
            ldr r1, =_bss_end
        1:  cmp r0, r1
            beq 2f
            str r2, [r0], #4
            b 1b
        2:  bl main                 // jump to C runtime
            b .                     // hang if main returns
        "
        safety: "Boot entry: set SP, zero BSS, call main. Pre-runtime."
    }
}
```
**Why irreducible:** before the boot stub runs, the C runtime doesn't
exist. Cannot call a function until SP is set. Cannot zero BSS until
SP is set. The first ~10 instructions of every embedded system are
fundamentally pre-language. Same for kernel boot, hypervisor entry, etc.

**Category 2: Vendor-specific instructions before ZER catalog catches up**
(~0-50 LOC per project, temporary)
```zer
naked void custom_vendor_op() {
    asm {
        instructions: "tdpbssd %tmm0, %tmm1, %tmm2"
        safety: "Intel AMX matmul step; @amx_tdpbssd intrinsic planned v0.5.1"
    }
}
```
**Why irreducible (temporarily):** ZER's catalog grows ~1-2 entries/year.
Brand-new ISA features (AVX-10.2 in 2027, ARM SME2 in 2028, etc.) may
temporarily need raw asm until intrinsics ship. Migrates to intrinsics
over time.

**Category 3: Hand-tuned hot loops where exact scheduling matters**
(~0-200 LOC per project, rare)
```zer
naked void aes_inner_loop(...) {
    asm {
        instructions: "
            // ~50 lines of hand-scheduled AES rounds
            // Compiler reordering would hurt cache/pipeline
        "
        safety: "Hand-tuned for Skylake µarch; matches OpenSSL reference."
    }
}
```
**Why irreducible:** crypto authors and high-perf inner loops want
*exact* instruction order. Even GCC builtins may reorder. This is rare
(<1% of <1% of code) but legitimate.

**Total irreducible asm per typical project:** ~100-300 LOC across all
three categories. Auditable in one sitting. Same scale as Hubris, Linux's
`arch/<X>/boot/`, Zephyr's `core/locore.S`.

#### Implementation path — phased, demand-driven

```
PHASE 0 (today):
  Status: ~130 intrinsics shipped (D-Alpha-1 through D-Alpha-14)
  Coverage: ~95% of typical kernel/firmware needs
  Raw asm % in typical project: ~3-5%

PHASE 1 (post-Level-C cleanup, ~3-4 weeks, OPTIONAL):
  Action: Add ~15-20 gap intrinsics from table above
  Priority order:
    1. Boot helpers (@cpu_set_stack, @cpu_set_vector_table, @cpu_jump_to)
    2. Full context save/restore (priv levels)
    3. IRQ entry/exit wrappers
    4. Cache range ops
    5. DMA buffer helpers
    6. TLB ops
    7. @cpu_pause spin hint
    8. Vector aligned load/store
    9. Saturating arithmetic
    10. Optional: crypto/SHA/CLMUL (if real demand)

  Implementation: each intrinsic = ~30-100 lines, can ship one at a time
  Coverage after: ~99% of typical needs
  Raw asm % in typical project: ~1%

PHASE 2 (ongoing, demand-driven, ~1-2 entries/year):
  Action: Add intrinsics when real users hit specific gaps
  Source: user issue tracker, vendor docs, new ISA extensions

  Implementation: incremental, never blocking
  Coverage after: bounded growth, asymptotic ~99%+

PHASE 3 (steady state):
  ~150-170 intrinsics in catalog
  ~1% raw asm in typical project (boot + vectors + rare hot loop)
  Zero standalone `.S` files in user projects
  GCC handles all per-arch codegen via intrinsic bodies
  ZER's source files are always `.zer` — no asm file extension needed
```

#### Industry validation — this exact target has been hit before

| System | Intrinsic count | Raw asm in kernel | `.S` files needed |
|---|---|---|---|
| **Hubris RTOS** (Oxide) | ~250 via `core::arch` + custom wrappers | ~200 LOC in `kernel/boot` crate | Zero |
| **Tock OS** (academic + embedded) | ~300 intrinsics + arch-specific | ~150 LOC per port | Zero |
| **seL4** | Similar pattern, Isabelle proofs on top | ~500 LOC verified asm | Zero — even Isabelle proofs treat asm as inline |
| **Zephyr RTOS** | Headers with intrinsics | ~300 LOC in `arch/<X>/core/` | Few (`locore.S`) — could be eliminated |
| **Linux kernel** | Thousands of `readl`/`writel`/etc. intrinsics | Substantial — but spread across thousands of files | Some (e.g., `arch/x86/entry/entry_64.S`) — historically grew, could be modernized |
| **ZER target** | ~150-170 intrinsics | ~100-300 LOC raw asm in naked fns | Zero (goal) |

**ZER's target falls between Hubris and Zephyr in catalog size. Achievable.**
Hubris specifically validated that 99% intrinsic coverage with raw asm
escape works in production embedded hardware (Oxide rack switch firmware).

#### What this means for marketing / public claim

After Phase 1 lands, ZER's public-facing claim about asm safety can include:

> "ZER firmware projects typically have zero standalone `.S` assembly files.
> 99% of CPU-level operations are expressed via the ~150-intrinsic catalog
> (atomic, MSR, port I/O, cache, context, etc.) — each with safety baked in
> at the catalog level. The remaining ~1% (boot stubs, vector tables, the
> rare hand-tuned hot loop) lives in `naked` functions with explicit raw
> asm + `safety:` audit string. GCC handles all per-arch codegen via
> intrinsic bodies. No probe scripts, no per-arch tables, no per-instruction
> database — works on every architecture GCC supports automatically."

This is honest, audit-able, and matches what Hubris/Tock can claim today.

#### Relationship to other sections

- **Section 1.5 (Final Design — Two-Layer Model):** the 99% goal is the
  Layer 1 catalog's stretch target. Layer 2 (raw asm in naked) handles
  the irreducible 1%. Doesn't change the architecture — just expands the
  catalog over time.
- **Section 9 (What We're Keeping):** intrinsic catalog is part of "Keep."
  Phase 1 additions go here.
- **Section 16 (Implementation Strategy):** the 6-commit Level C cleanup
  ships first. Phase 1 catalog expansion is post-Level-C optional follow-on.
- **Section 20 (Out of Scope):** "Algorithm correctness" and
  "Microarchitectural" remain out of scope even at 99% intrinsic coverage.

**Status:** goal documented, path defined, no commitments made beyond
Level C execution. Phase 1 catalog expansion can be deferred indefinitely
or executed incrementally based on user demand.

---

## 1.5. Final Design — Two-Layer Model (crystallization, 2026-05-12 evening)

**This section supersedes any ambiguity in later sections about additional
language complexity on top of Level C.** A multi-day architectural discussion
on 2026-05-12 explored richer designs (Tier 2 operand annotations, IR region
wrappers, auto-guard emission) and **rejected them all** in favor of the
simpler two-layer model below. Level C is the execution baseline; this
section confirms that nothing richer gets layered on top.

### The two layers

```
┌──────────────────────────────────────────────────────────┐
│ LAYER 1 — INTENT INTRINSICS (primary safe path)           │
│                                                            │
│   ~130 today, room for ~20 more if real demand emerges.    │
│                                                            │
│   @bit_scan_reverse, @atomic_*, @cpu_read_msr, @port_*,    │
│   @cache_*, @barrier_*, @cpu_*, @vec_load_aligned, etc.   │
│                                                            │
│   → Compiler-controlled emission. Safety baked in.         │
│   → ~80% wrap GCC builtins (zero per-arch ZER code).       │
│   → ~20% wrap per-arch inline asm (frozen, audited once).  │
│   → User-facing experience: call function, get safety.     │
│   → Covers 95%+ of typical kernel / firmware / embedded.   │
└──────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────┐
│ LAYER 2 — RAW ASM IN NAKED FN (escape hatch, "unsafe")    │
│                                                            │
│   naked fn boot() {                                        │
│       asm {                                                │
│           instructions: "..."                              │
│           safety: "Audit string ≥ 30 chars (S4)"          │
│       }                                                    │
│   }                                                        │
│                                                            │
│   → No Tier 2 annotations. No IR regions. Plain asm.       │
│   → MISRA Dir 4.3 satisfied via mandatory `naked`.         │
│   → User responsibility: clobbers, preconditions, intent.  │
│   → AST mnemonic still auto-applies ~12 UB classics.       │
│   → Z-rules still apply through operand bindings.          │
│   → Covers boot stubs, ctx switch, hand-tuned crypto.      │
└──────────────────────────────────────────────────────────┘
```

That's it. **Two layers. Nothing else.**

### What's EXPLICITLY DEFERRED indefinitely

These ideas were explored in depth during the 2026-05-12 discussion and
**rejected** as adding complexity without commensurate safety gain over the
2-layer model. Re-opening these requires fresh user-visible motivation:

| Deferred idea | Why explored | Why rejected |
|---|---|---|
| **Tier 2 per-operand annotations** (`requires: nonzero`, `align: 16`, etc. on asm operand bindings) | Would let users get compile-time precondition checks on raw asm without writing an intrinsic | Redundant with intrinsics for common cases; rarely useful for niche cases; intent intrinsics + caller-side guards cover the same ground without adding annotation vocabulary; "one obvious way to do it" principle |
| **IR region wrappers for asm safety** (`@atomic_sequence { }`, `@cache_sync_region { }`, `@dma_buffer_op { }`) | Multi-block coordination patterns get clean structural scope | Composite intrinsics (`@atomic_cas`, `@cache_flush_range`, etc.) handle the same patterns with simpler syntax; regions add language surface without unlocking new safety; existing regions like `@critical`/`defer`/`@once` already exist for general scope needs |
| **Auto-guard emission for unprovable Tier 2 preconditions** | Would make wrong-precondition behavior hardware-independent (loud ZER trap instead of CPU-dependent trap) | Only useful if Tier 2 annotations exist (which they don't); intrinsics emit their own internal guards already |
| **State-machine annotations on individual blocks** (`opens_state:` / `closes_state:`) | Would let user-defined multi-block patterns be checked | F7-light hardcoded LR/SC state machine covers the one real case; user-defined patterns either become intrinsics or live in raw asm |
| **Generic asm linter / per-instruction database for clobber completeness** | Would catch missing clobber declarations | Per-instruction database = maintenance hell (the whole reason Level C exists); intrinsics absorb 95%+ of clobber audits at the catalog level instead |
| **`unsafe asm` keyword** (vs current bare `asm`) | Symmetry with Rust | Cosmetic only; `naked` already isolates asm structurally; rename happened in opposite direction (2026-04-25 dropped `unsafe asm` → bare `asm`) |

### What stays from Level C, unchanged

Section 2 ("The Decision in One Page") and section 9 ("What We're Keeping")
are still correct. The 2-layer model is just a more precise framing of what
Level C already says:

- 130 intent intrinsics (catalog) — Layer 1
- Z-rules Z1-Z8 + Z11/Z12 through asm operands — applies to Layer 2
- Naked-only restriction (S1) — defines Layer 2 boundary
- Structured asm syntax (`asm { instructions: ... safety: ... }`) — Layer 2 syntax
- Hardcoded UB classics (~12 frozen) via AST mnemonic detection — applied to Layer 2
- F7-light LR/SC pairing — applies to Layer 2 only (LR/SC also available via composite intrinsic)
- GCC delegation for ISA-level (registers, instructions, CPU features, sub-extensions, future ISAs)

### The honest "what we give up vs Rust"

Rust's `core::arch::*` has ~5000 intrinsics. ZER's catalog has ~130-150. For
truly esoteric ops Rust users find a pre-made intrinsic; ZER users either
wait for a catalog addition or drop to Layer 2 raw asm. **In practice the
~150 catalog covers what kernel / firmware / RTOS developers actually use.**
The 5000-intrinsic gap is mostly SIMD variants used by numerical computing —
not ZER's primary audience.

### Maintenance picture — final

```
ZER-side ongoing maintenance after one-time implementation:

  Intrinsic catalog growth:          ~1-2 entries / year (real demand only)
  Hardcoded UB classics list:         ~1 entry / decade (genuinely new UB class)
  Per-arch register tables:           ZERO — deleted, GCC handles
  Per-arch instruction tables:        ZERO — deleted, GCC handles
  CPU feature enum:                   ZERO — deleted, GCC -m flag passthrough
  Probe scripts:                      ZERO — deleted
  Sub-extension support:              ZERO — GCC inherits automatically
  New architecture support:           ZERO — GCC inherits automatically
  Tier 2 annotation framework:        ZERO — not built (deferred)
  IR region wrappers:                 ZERO — not built (deferred)

TOTAL ZER-side ongoing: ~1-2 hours per year. Bounded. Not hellish.
```

### Industry precedent for this exact model

The 2-layer "intrinsics primary + raw asm escape" model is the **industry
default for production kernel/RTOS development**:

- **Hubris RTOS** (Oxide Computer, Rust-based): intrinsics for everything,
  raw `asm!()` only in tiny `kernel/boot` crate. 99% of code never touches asm.
- **Linux kernel**: `readl`/`writel`/`atomic_*`/`barrier` intrinsics everywhere;
  `.S` files only in `arch/<X>/{boot,entry,head}.S`.
- **Zephyr RTOS**: same pattern, intrinsics in headers, asm in `core/locore.S`.
- **FreeRTOS**: same pattern.
- **STM32 / CMSIS**: `__DMB()`, `__WFI()`, `__NOP()` intrinsic macros; raw asm rare.
- **Rust stdlib**: `core::arch::*` is the intrinsic catalog; `asm!()` is the
  unsafe escape.

**Every production system that takes asm safety seriously uses this exact
2-layer pattern.** ZER's design matches the converged industry norm.

### Conformance — MISRA Dir 4.3 is sufficient

MISRA C:2023 Dir 4.3 (Mandatory): *"Assembly language shall be encapsulated
and isolated."*

ZER's `naked` requirement satisfies this fully. No additional ZER-side
machinery (Tier 2 annotations, region wrappers, contract systems) is
required to claim MISRA conformance for asm encapsulation. **Bare 2-layer
model passes the MISRA test.**

Higher safety-critical standards (DO-178C Level A, ISO 26262 ASIL D, IEC
62304 Class C) require formal proof / exhaustive testing — Vale/SPARK
territory, opt-in via `@verified_spec` v2.x+, out of core ZER scope.

### Execution path remains unchanged

The 6-commit cleanup plan in **section 16** is correct and proceeds as
specified. The 2-layer crystallization above is a clarification, not a
plan revision. Nothing new gets built beyond what section 16 lists.

If intrinsic catalog growth is desired (the optional ~20 new intent
intrinsics for UB-prone operations not currently covered), that's a
**post-Level-C optional follow-on**, ~3-4 weeks, bounded scope, can ship
incrementally one intrinsic at a time. Not blocking.

---

## 1.6. Level D Pivot — User-Extensible Intrinsics with Explicit Contracts (added 2026-05-31)

> **REFINED BY OPTION E (Section 1.7, added 2026-06-06).** Level D's `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18) survive unchanged. The layering described in §1.6.16 ("blessed @core::* with per-ISA dispatch authored by ZER team in core") is refined by Option E: ZER core ships the schema (closed category vocabulary) + the semantic operation taxonomy + the `@intrinsic_def` / `@bind` mechanism, with **zero per-ISA bindings in language core**. Per-ISA bindings (including for operations Level D would have called "blessed @core::*") live in Layer 2 libraries, structurally peer to community libraries. The packaging question of whether ZER team maintains a canonical Layer 2 binding library (e.g., `zer-asm-x86`) is separable from the safety architecture and decidable empirically. See §1.7 for the locked layering, the structure/semantics straddle observation, and the closed-vocabulary kind-difference from SPARK elevated to load-bearing thesis. The program-consequence vs hardware-consequence vocabulary locked in `CLAUDE.md` and `firmware_safety_extensions.md` applies to all of Level D's claims; "wrong contract poisons verifier reasoning about callers" (§1.6.7) is restated as hardware-consequence floor at the `@bind` declaration site, with the closed-vocabulary discipline (§1.7.7) bounding the trust-gap shape.

**This section ADDS to Level C, does not remove it.** Level C remains the
correct execution baseline. Level D is an architectural upgrade on top of
Level C that resolves the one remaining maintenance leak: the ~1-2
intrinsics/year ZER team additions for new ISA features.

**Status:** Architecture LOCKED IN as of 2026-05-31. Implementation pending. **REFINED by Option E as of 2026-06-06 — see §1.7.**

**Pivot summary:** Level C constrains Y (operations) and delegates Z
(ISA-specific details) to GCC, but still leaves Y_intrinsic catalog growth
on ZER team's desk (~1-2 additions/year forever). Level D externalizes the
authorship of Y_intrinsic to users/libraries while keeping the verifier
(X, Z-rules, safety class registry) internal and frozen. ZER team's
ongoing work converges to a fixed point: new safety classes appear only
when fundamentally novel hardware-precondition kinds emerge (~once per
decade), not when new instructions or extensions appear.

> **READ FIRST:** This section captures the full design discussion that
> produced the Level D pivot. The architecture decisions here are LOCKED.
> Future sessions should not re-litigate. Reference sections 1.6.1
> through 1.6.13 for the complete context.

### 1.6.1. The mathematical formalization

Following a long architectural discussion, the asm safety architecture
can be formalized precisely:

```
X = { safety classes }                              finite, bounded set
Y = { all possible ASM operations }                 potentially infinite
Z = X expanded across all (arch, instr, operand)    |Z| = unbounded
```

**X is frozen.** Z-rules Z1-Z8/Z11/Z12 + UB classics (~12) + naked-only
(S1) + operand types + 8 safety categories = ~33 elements. These don't
grow with hardware evolution.

**Z is unbounded.** Every new ISA extension, every new instruction, every
new sub-architecture adds entries to Z. Growth rate: ~1000-1500 entries
over 5 years.

**The X ↔ Z relationship:**
- Every element of Z reduces to membership in some subset of X
- Every safety class in X has potentially infinitely many instances in Z
- Direct enforcement via X alone is incomplete (compressed over per-instruction details)
- Direct enforcement via Z is impossible in practice (|Z| unbounded — maintenance hell)

**The resolution: constrain Y.**

Make Y a controlled subset rather than the full unbounded space:

```
Y_zer = Y_intrinsic ∪ Y_naked

Y_intrinsic: operations expressible through ZER's intrinsic catalog
             class(intrinsic) ⊆ X pre-computed at intrinsic-definition time
             
Y_naked: operations expressible through raw asm in naked functions
         with explicit safety: annotation
         user takes responsibility for safety classes that apply
```

**Coverage equation (Level C):**

```
Coverage(Y_zer) = Coverage(Y_intrinsic) + Coverage(Y_naked) + Delegation(GCC)

Where:
  Coverage(Y_intrinsic) ≈ 99% with complete X enforcement
  Coverage(Y_naked) ≈ 1% with partial X enforcement + user-declared audit
  Delegation(GCC) = all Z-level concerns (register validation, instruction
                    validity, CPU features, sub-extension support)
```

### 1.6.2. The two levers identified

The asm design uses two distinct architectural levers, applied in sequence:

**Lever 1 (Level C): Constrain Y so finite X covers it. Delegate Z to GCC.**

This bounds ZER's burden to |X| + |Y_intrinsic|. |X| is frozen. But
|Y_intrinsic| still grew on ZER team's desk (~130 today, ~1-2/year additions
forever).

**Lever 2 (Level D): Externalize the authorship of Y_intrinsic while
keeping the verifier internal and frozen.**

This is the NEW move. Not "constrain Y" again — it's "constrain Y, then
externalize who authors Y while ZER retains the verification engine."

```
Lever 1 (Level C):
  ZER team owns:    catalog (Y_intrinsic), verifier (X)
  Delegated:        Z (register/instruction validity → GCC)
  Burden:           1-2 intrinsics/year forever
  
Lever 2 (Level D):
  ZER team owns:    verifier (X), mechanism (intrinsic_def syntax)
  Delegated:        Z (→ GCC), catalog authorship (→ user/library ecosystem)
  Burden:           ~0 — new safety class ~once per decade
```

The lever differences matter:
- Lever 1: constrain Y to bounded set
- Lever 2: open Y back to unbounded, but make authorship distributed and contract-explicit

ZER's work converges to a fixed point with Lever 2. That's the actual
freedom Level C was trying to reach but couldn't fully achieve.

### 1.6.3. The intrinsic_def mechanism

User (or library) defines intrinsics in ZER source code:

```
@intrinsic_def @bit_scan_reverse(mask: u32) -> u32 {
    arch x86_64: {
        instructions: "bsr %1, %0"
        inputs:  { "in"  = mask }
        outputs: { "out" = result }
        clobbers: [ "flags" ]
    }
    arch aarch64: {
        instructions: "clz %0, %1"
        inputs:  { "in"  = mask }
        outputs: { "out" = result }
        clobbers: [ ]
    }
    requires:    mask != 0
    safety_class: [ C1_nonzero ]
    effect:       result_in_range(0, 31)
    safety:       "Bit-scan reverse; UB on zero input per Intel SDM Vol 2 BSR"
}
```

User code anywhere in ZER then calls it:

```
u32 idx = @bit_scan_reverse(mask);
// Compiler enforces: mask != 0 via existing VRP infrastructure
// Compiler enforces: declared safety_class membership via existing systems
```

**Compiler responsibilities:**
1. Verify operand types match declared input/output types
2. Verify safety_class assignments dispatch to existing safety infrastructure
3. Verify `requires` clause uses operands correctly (references valid operand names)
4. Verify operand binding consistency (declared names match asm template references)
5. Pass per-arch dispatch to GCC for assembly-time validation

**Author responsibilities (the seam — see Section 1.6.6):**
1. Asm string actually does what the contract claims
2. Clobber list is complete
3. `requires` precondition matches hardware reality
4. `safety_class` assignments are correct for the actual operation
5. Per-arch implementations are semantically equivalent

### 1.6.4. The maintenance picture after Level D

```
ZER-side ongoing maintenance after Level D ships:

  Safety class registry (X):              ~0-1 entries per DECADE
                                          (only for fundamentally novel
                                           hardware-precondition kinds)
  
  Z-rules infrastructure (Z1-Z8, Z11, Z12): FROZEN, no growth
  
  Hardcoded UB classics (~12):            FROZEN, ~1 entry per decade
  
  intrinsic_def syntax/parser:            FROZEN after initial ship
  
  Verification engine:                    FROZEN after initial ship
                                          (uses existing safety systems)
  
  Blessed catalog (@core::*):             STABLE at current ~130
                                          (slow-grow via promotion from
                                           Layer 2, NOT frozen forever)
  
  Per-arch register tables:               DELETED (Level C) — GCC handles
  Per-arch instruction tables:            DELETED (Level C) — GCC handles
  CPU feature enum:                       DELETED (Level C) — GCC handles
  Probe scripts:                          DELETED (Level C) — gone
  
  User-defined intrinsics (@<lib>::*):    UNBOUNDED growth in user space
                                          ZER team NOT involved
```

**The honest fixed-point claim — distinguish X from Layer 1:**

Earlier framing said "Layer 1 FROZEN forever, per-decade fixed point."
That conflated two distinct things. The honest split:

**X (safety class registry) — genuinely per-decade fixed point:**
- New fundamentally novel hardware-precondition kinds only
- Example: CHERI capabilities introduction in early 2020s (one event)
- Example: hypothetical "atomic ordering across heterogeneous memory"
- Frequency: per-decade, possibly per-multi-decade
- This IS the actual fixed point

**Layer 1 (blessed catalog @core::*) — slow-moving, NOT frozen:**
- Starts at ~130 (current state)
- Grows by PROMOTION from Layer 2 when a pattern proves universal
  - Trigger: three or more libraries hand-roll the same pattern against
    the same asm — that's ecosystem signal it should be blessed
- Each promotion: ZER team audits the candidate, then it joins core
- Rate: handful per several years, not zero
- Not a flaw — healthy ecosystem feedback loop
- "Frozen forever" denies this can happen and locks in a claim that
  will be falsified by ZER's own success

**Why "per-year for everything" was also wrong:**

The doc previously stated "~1-2 intrinsics/year on real demand." That
was the Level C figure where ZER team owned all catalog authorship.
Level D externalizes Y_intrinsic authorship, so:

- New instructions → Y growth → user library territory (NOT ZER's work)
- New ISA extensions → Y growth → user library territory (NOT ZER's work)
- New vendor opcodes → Y growth → user library territory (NOT ZER's work)
- New architectures → Y growth → user libraries + GCC delegation (NOT ZER)
- Pattern promotion to Layer 1 → ZER team audit per promotion
  - Rate: handful per several years
- New fundamentally novel safety class → X growth → ZER's work
  - Rate: per-decade

**The two growth rates, separated:**

```
ZER-team ongoing work:
  X (safety classes):    ~per-decade           (true fixed point)
  Layer 1 (blessed):     ~handful per several years (slow promotion)
  Z-rules / UB classics: frozen
  Mechanism / verifier:  frozen after ship
  
Library/ecosystem work:
  Y_user_defined:        unbounded growth in user space (not ZER's work)
```

TOTAL ZER-side burden: a few hours per several years (Layer 1 promotion
audit), ~1-10 hours per decade (new safety class). NOT zero, but bounded
and ecosystem-driven rather than ZER-pushed.

Honest framing: bounded, slow-growth, not perpetual treadmill.
NOT 1-2/year (Level C figure). NOT zero forever (overcorrection).
Actually: slow promotion of Layer 1 (years), plus per-decade X.

### 1.6.5. The bootstrap argument

```
ZER compiler ships ONCE with:
  - Safety class registry X (~33 frozen elements)
  - intrinsic_def syntax (one-time language feature)
  - Verification engine (uses existing safety infrastructure)
  - Operand type checking (already exists)
  
After this ships, ZER compiler is DONE with intrinsic-related work.

The catalog grows in user/library space:
  - Vendor SDKs ship intrinsic libraries for their hardware
  - Domain libraries ship intrinsic_def for their patterns (crypto, DSP, etc.)
  - Project-specific intrinsics live in project code
  - Each contributor audits their own contracts
  
End state:
  Blessed standard library: ~30-50 (minimum) or ~130 (current, frozen)
  User libraries: thousands of domain-specific intrinsics
  ZER team work: minimal, per-decade safety class additions
```

This is "bootstrap ASM" — ASM provides the operations, intrinsic_def
provides the contracts, ZER's existing safety infrastructure provides
the verification. Same pattern as how Rust's standard library ships
`asm!()` macro once, and the crates ecosystem provides every specific
use case (raw-cpuid, x86, cortex-m, etc.).

ZER's version is FIRST-CLASS (not a macro hack) — definitions are
language-level constructs with structured contracts, not text substitution.
Contracts get compiler verification against safety classes.

### 1.6.6. The seam — wrong contract at definition site

**The honest critical analysis: Level D reintroduces Definition B's
failure mode at the intrinsic_def boundary.**

ZER previously rejected Definition B (SPARK-style contracts) in Section 2:

> "If the user's contract is wrong, the compiler verifies a wrong contract
> and produces false confidence. The bug ships anyway, but with a
> 'verified' wrong contract."

intrinsic_def is structurally identical at the definition boundary:
- User declares contract (operands, requires, safety_class, clobbers)
- Compiler verifies code-against-contract
- Wrong contract → silent bug

**This is the seam. It must be named explicitly, not obscured.**

What ZER CAN verify about a user-defined intrinsic_def:

| Property | Verifiable? | How |
|---|---|---|
| Operand types | ✓ Yes | Existing type checker |
| Safety class invocation correctness | ✓ Yes | Pattern check against class definition |
| Operand binding consistency | ✓ Yes | Existing parser |
| `requires` references valid operands | ✓ Yes | Symbol resolution |
| `safety_class` enum membership | ✓ Yes | Lookup in X registry |
| Per-arch dispatch syntactic correctness | Partial | GCC validates at assembly time |
| Whether asm string actually does declared op | ✗ No | Needs per-instruction DB (rejected) |
| Whether clobber list is complete | ✗ No | Needs per-instruction DB |
| Whether `requires` matches actual hardware precondition | ✗ No | Needs hardware spec |
| Whether effect statement is semantically correct | ✗ No | Needs semantic equivalence checker |
| Cross-arch implementations are equivalent | ✗ No | Needs cross-arch semantic checker |

**The seam summary:**

ZER's intrinsic_def verification covers structural consistency (operand
types, safety class invocation, operand binding) but NOT semantic
correctness against silicon (asm-vs-contract, clobber completeness,
hardware-precondition correctness).

**This is the SAME boundary as:**
- Rust's `unsafe impl Send` (user declares Send-ness, compiler trusts)
- Rust's `unsafe { asm!() }` (user declares contract via comments, compiler trusts)
- ZER's `cinclude` (user declares C function signature, compiler trusts)
- ZER's raw `asm { ... }` in naked (user declares via `safety:` string)

Every safe systems language has this boundary somewhere. Level D moves
ZER's boundary from "every asm use site" (current Level C) to "every
intrinsic_def site" (Level D).

### 1.6.7. Why the seam is BETTER than the alternatives

Despite the seam, Level D has real advantages over alternatives:

```
Comparison of contract-authoring locations:

Rust's unsafe { asm!() }:
  Audit point per: every use site
  Contract structure: free-form `// SAFETY:` comments
  Compiler enforcement: minimal — operand types only
  Audit burden: many sites, unstructured

C's asm:
  Audit point per: every use site
  Contract structure: none
  Compiler enforcement: none beyond compilation
  Audit burden: every use site, no help

ZER Level C (current):
  Audit point per: every blessed intrinsic (one-time, ZER team)
  Contract structure: encoded in catalog entry
  Compiler enforcement: complete via catalog (no user input)
  Coverage: limited to catalog (~130 entries)

ZER Level D (NEW):
  Audit point per: every intrinsic_def (one-time, author)
  Contract structure: structured (operand types, requires, safety_class)
  Compiler enforcement: complete against structured contract
  Coverage: unbounded via user libraries
```

**Level D vs Rust unsafe — DIFFERENT TRADE, NOT STRICT IMPROVEMENT.**

Earlier framing claimed Level D was "BETTER than Rust unsafe." That was
overclaim — it stated only the flattering half. Both halves are true:

**Level D ergonomics: BETTER than Rust unsafe**
- Contracts are STRUCTURED (machine-checkable against X, not free-form `// SAFETY:` comments)
- Audit happens ONCE per def, reused many times (not per call site)
- Greppable per category (`grep intrinsic_def` vs `grep unsafe`)

**Level D blast radius when wrong: WORSE than Rust unsafe**
- Rust's `unsafe` cannot lie to the borrow checker about other code —
  the unsafety is contained at the block boundary
- Level D's `intrinsic_def` FEEDS the verifier with structured claims
  (`safety_class:`, operand types, effects) that the verifier uses
  to reason about CALLERS
- A wrong `safety_class` declaration tells the verifier to enforce
  the wrong invariant at every call site
- Worse: a wrong claim about what an intrinsic SATISFIES can cause
  the verifier to certify surrounding code as safe on a false premise
- Blast radius = the verifier's reasoning about callers, not just the
  local intrinsic body
- This is a poisoned input to the trusted verifier, not a contained escape

**Honest framing:** Level D is a DIFFERENT trade-off than Rust unsafe,
not a strict improvement. Better ergonomics and structured auditability;
worse blast radius when a contract is wrong. The choice between them is
about which failure mode is preferable for the audience.

ZER picks Level D because:
1. ZER's audience prefers structured contracts over free-form comments
2. The blast radius is mitigable via the demand/promise asymmetry rule
   (see Section 1.6.17) — user intrinsics can DEMAND obligations freely
   but cannot silently DISCHARGE obligations downstream relies on
3. Greppable per-category auditing matches ZER's existing escape boundaries
   (`cinclude`, `naked fn`, now `intrinsic_def`)

The blast-radius cost is real and named. The mitigation is the
asymmetry rule, not the structured nature of the contracts.

**Level D advantages over Level C alone:**
- Coverage extends to any hardware (not catalog-limited)
- No ZER-team bottleneck for new ISA features
- Library ecosystem can flourish
- ZER team converges to fixed point on X (safety classes)

**Level D trade vs Level C:**
- Soundness claim becomes CONDITIONAL on author contract correctness
- Wrong contract poisons verifier reasoning about callers
  (worse blast radius than the Level C catalog-only approach)
- More compiler complexity (verification engine for intrinsic_def)
- Mitigated by demand/promise asymmetry (Section 1.6.17)

### 1.6.8. The blessed namespace mechanism

To distinguish ZER-audited intrinsics from user-defined ones, Level D
introduces namespace convention:

```
@core::atomic_cas         ← blessed (ZER team audited, frozen)
@core::bit_scan_reverse   ← blessed
@core::cpu_disable_int    ← blessed

@user::custom_op          ← user-defined (author owns contract)
@my_vendor::amx_tdpbssd   ← vendor library
@app::project_specific    ← project-internal

Compiler treats `@core::*` as special:
  - Cannot be redefined by user code
  - Frozen at language version
  - ZER team audited

User namespaces:
  - Defined by module path / package name
  - `@<library>::<intrinsic>` resolves via standard import system
  - Each library author owns their namespace
```

**Implementation:** ~50-100 lines reusing existing module/namespace
infrastructure. No new mechanism needed beyond namespace prefix
convention enforced at intrinsic_def site.

**Why namespace prefix vs other options:**

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Namespace prefix (`@core::*`) | Visual distinction, greppable, reuses module path | Verbose | **CHOSEN** |
| Module path import | Cleaner imports | Requires resolution | Alternative |
| Reserved marker (`@intrinsic_def_blessed`) | Explicit | New mechanism | Heavier |
| Convention only (no language distinction) | Simplest | No enforcement | Too weak |

Recommended: namespace prefix. Already aligns with ZER's existing
module path syntax. No new language mechanism.

### 1.6.9. The conditional soundness claim (4-level breakdown)

Level D's safety story is honestly conditional. ZER's claim splits into
four levels:

```
LEVEL 1 — Pure ZER (no intrinsic_def, no cinclude):
  Unconditionally sound. ZER-team-owned correctness.
  Memory safety, type safety, concurrency safety all guaranteed.
  
LEVEL 2 — ZER + blessed intrinsics (@core::*):
  Unconditionally sound. ZER team has audited these one-time, frozen.
  
LEVEL 3 — ZER + user-defined intrinsics (@<lib>::*):
  Sound CONDITIONAL on author's contract correctness.
  Compiler verifies "code matches declared contract" structurally.
  Compiler does NOT verify "declared contract matches silicon."
  Same conditional boundary as cinclude / Rust unsafe.
  
LEVEL 4 — ZER + cinclude (C interop):
  Sound CONDITIONAL on C code correctness.

Greppable audit boundaries (decentralized auditability):
  grep "@<lib>::"        → audit user-defined intrinsic uses
  grep intrinsic_def     → audit intrinsic definitions
  grep cinclude          → audit C interop boundary points
  grep "naked fn"        → audit raw asm escape points
```

**The honest framing principle:** Each level explicitly names what it
depends on. No hidden assumptions. No "mostly safe" framing.

This is stronger than Rust's claim because:
- intrinsic_def contracts are STRUCTURED (machine-checkable)
- Audit boundaries are GREPPABLE per category
- Conditional dependencies are EXPLICIT per level

It's more honest than languages that obscure the boundary (e.g., claims
of "memory safe" without specifying which code, "race-free" without
specifying which concurrency model).

### 1.6.10. The triage of Level D concerns

In the design discussion, the following concerns were raised about Level D:

| Concern | Whose problem | Resolution |
|---|---|---|
| No mechanical verification asm string matches contract | Design by intention | NOT a gap — author declares; ZER trusts. Same as Definition A philosophy applied at def site instead of use site. |
| Library quality variation | Library author | Library author owns their contracts. Same as every package ecosystem (npm, cargo, pip). ZER provides mechanism; ecosystem provides quality. |
| Versioning / namespace collisions | Library / build system | Module path resolution. Standard package management concern. NOT language concern. |
| Cross-library composition trust transitivity | Library author | Library author publishes contracts; downstream trusts published interface. Same as every package ecosystem. |
| No "blessed library" mechanism in language | ZER | RESOLVED via `@core::*` namespace convention (Section 1.6.8). |
| Soundness claim becomes conditional | ZER (documentation) | RESOLVED via 4-level honest breakdown (Section 1.6.9). |

**Concerns 1, 2, 3, 4 are not ZER's problem — they're ecosystem concerns
that every package-based language handles via convention.**

**Concerns 5 and 6 are ZER's work** — both bounded one-time implementations:
- #5: namespace convention (~50-100 lines parser/checker)
- #6: documentation update (README, reference.md, marketing claim)

### 1.6.11. The mathematical formalization extended

```
After Level D:

Y_zer = Y_blessed ∪ Y_user_defined ∪ Y_naked
        ↑           ↑                  ↑
        frozen      grows in user-space  ~0.1% escape, naked-only

|Y_blessed| = FROZEN at ~130 (current) or trimmed to ~30-50 minimum
|Y_user_defined| = grows with hardware evolution, but in user space
|Y_naked| = bounded ~0.1% (boot stubs, hand-tuned crypto)

class(Y_blessed) ⊆ X         → audited by ZER team (one-time, frozen)
class(Y_user_defined) ⊆ X    → declared by author, structurally verified
                                  by compiler against X
class(Y_naked) ⊆ X           → user-declared via safety annotation

Coverage(Y_zer) = Coverage(Y_blessed) + Coverage(Y_user_defined) +
                  Coverage(Y_naked) + Delegation(GCC)

ZER-team-side maintenance burden:
  |X| growth: ~0-1 per decade
  |Y_blessed| growth: 0 (frozen)
  intrinsic_def mechanism: 0 (one-time ship)
  Verification engine: 0 (one-time ship)
  
Hardware evolution touches:
  |Y_user_defined|: user library territory
  GCC delegation (Z): user's C compiler

ZER team's work converges to a fixed point after Level D ships.
```

### 1.6.12. Comparison to industry practice

```
System              Catalog approach            Maintenance burden
─────────────────────────────────────────────────────────────────────────
Hubris RTOS (Rust)   core::arch + library       Rust-team for core,
                                                  community for crates
                                                  
Linux kernel        readl/writel + arch/*.S    Kernel maintainers
                                                  
Zephyr RTOS         intrinsics in headers      Vendor + community
                                                  
STM32 / CMSIS       __DMB(), __WFI() macros    ARM + ST vendors
                                                  
Rust stdlib         core::arch + asm! macro    Rust team for core,
                                                  crates ecosystem for rest
                                                  
ZER Level C         Built-in catalog (~130)    ZER team (1-2/year)
                                                  
ZER Level D (NEW)   Built-in core + user-def   ZER team for core (frozen),
                                                  community for rest
                                                  Per-decade for ZER team
```

Level D brings ZER into alignment with the converged industry pattern:
**small blessed core + extensible mechanism + community catalog growth.**

This is exactly how every successful systems language handles the
"unbounded catalog" problem. ZER's contribution: STRUCTURED contracts
verified against existing safety infrastructure, not free-form
documentation.

### 1.6.13. Implementation scope for Level D

**What ships with Level D (one-time work):**

```
1. Parser support for intrinsic_def syntax (~200-400 lines)
   - Token recognition for new keyword
   - AST nodes for intrinsic definitions
   - Per-arch dispatch parsing
   - Operand binding parsing
   - Contract clause parsing (requires, safety_class, effect)

2. Verification engine (~500-1000 lines)
   - Operand type validation
   - Safety class invocation dispatch
   - Cross-architecture consistency checks (best-effort)
   - Integration with existing Z-rules

3. Namespace convention (~50-100 lines)
   - @core::* reservation
   - Module path resolution for @<lib>::*
   - Collision detection

4. Documentation (~few hundred lines)
   - 4-level safety claim in docs/reference.md
   - intrinsic_def syntax reference
   - Author's guide for writing safe intrinsic_def
   - Ecosystem conventions

5. Migration (optional)
   - Existing 130 intrinsics become @core::*
   - Verify they're frozen and audited
   - Document the freezing

Total: ~750-1500 lines of compiler code + documentation
Estimated effort: ~4-8 weeks for one developer
```

**What does NOT ship (per Level C decision):**

- No per-instruction database (Z stays in GCC)
- No register validation tables (GCC handles)
- No CPU feature enum (GCC -m flags)
- No probe scripts (deleted in Level C)
- No 8-category framework infrastructure beyond X registry

**Pre-Level-D prerequisite:**

Level C cleanup (Section 16, 6 commits, 1-2 days) should ship FIRST.
This removes the per-instruction database and register tables that
would conflict with the user-extensible approach.

Then Level D adds intrinsic_def on the clean Level C foundation.

### 1.6.14. The architecture locks

> **AUGMENTED BY §1.7.18 (Option E locks).** The 12 locks below remain valid; Option E adds: (13) no per-ISA asm in Layer 1 of language core; (14) closed-vocabulary discipline must stay hard — no free-form predicates in Layer 2; (15) structure/semantics straddle is the architectural forcing argument for the layering; (16) packaging of canonical Layer 2 libraries is separable from safety architecture; (17) program-consequence vs hardware-consequence vocabulary is locked across the asm domain identical to the firmware domain. Read §1.7.18 for the complete locked-decisions list.

**Locked in (do not re-litigate in future sessions):**

1. **The lever is "externalize authorship"** — different from "constrain Y"
2. **Per-decade growth for ZER team applies to X (safety classes)** — Layer 1 is slow-grow via promotion, not frozen forever
3. **The seam IS real** — wrong contract at def site is the failure mode; blast radius is LARGER than Rust unsafe (verifier poisoning)
4. **The mitigation IS greppable trusted-once boundary** — same shape as cinclude, Rust unsafe — PLUS the demand/promise asymmetry rule (lock #11)
5. **The safety claim splits 4 ways** — pure ZER, blessed, user-def, cinclude
6. **Blessed namespace is `@core::*`** — convention enforced at intrinsic_def parsing
7. **Verification engine reuses existing safety infrastructure** — no new Z-rules
8. **|X| stays per-decade fixed point** — ~33 elements, ~0-1 additions per decade
9. **|Y_blessed| is STABLE not frozen** — starts at ~130, grows by promotion from Layer 2 when patterns prove universal (handful per several years)
10. **Library ecosystem owns |Y_user_defined|** — ZER not involved
11. **Demand/promise asymmetry — DIRECTIONAL classification, not syntactic** — see Section 1.6.17. User declarations that cause the verifier to do LESS checking on surrounding code are PROMISES (must be checkable or explicitly taint caller's soundness). Declarations that cause MORE checking are DEMANDS (free, can only over-reject). Classify by effect on verifier workload, not by syntactic slot.
12. **Missing-arch propagation rule** — see Section 1.6.18. Call to intrinsic with no arch-clause for compile target = compile error at call site. Supported-archs is part of the intrinsic's contract; caller's portability set = intersection of supported-archs across all intrinsics transitively used. GCC delegation does NOT cover this.

**Future sessions:** If a fresh session proposes:
- "Let's add another safety class for X" → check if it's genuinely novel; usually no
- "Let's add intrinsic Y to blessed catalog" → check if it qualifies for promotion from Layer 2 (handful per several years rate)
- "Let's verify asm strings against contracts" → that's per-instruction DB, REJECTED
- "Let's audit user libraries" → that's ecosystem, not ZER
- "Let's let user intrinsics claim arbitrary safety_class provides" → VIOLATES lock #11 (promise must be checkable or tainting)
- "Let's silently fallback on missing arch" → VIOLATES lock #12 (must be compile error)

Reference Section 1.6 instead of re-deriving.

### 1.6.15. The complete pivot summary

> **REFINED BY §1.7.16 (Option E comparison matrix).** The "Level D end state" below describes a "Frozen blessed catalog @core::* (~130 or trimmed to ~30-50)" with implicit per-ISA dispatch authored by ZER team in core. Under Option E, the catalog of ~130 intrinsics is refactored: the operation declarations move to Layer 1 (taxonomy in core, no asm bodies); the per-ISA dispatch moves to a separately-maintained Layer 2 binding library (e.g., `zer-asm-x86`). Architectural commitment: ZER core has no per-ISA bindings, ever. Packaging commitment (separable, decidable empirically): ZER team likely maintains one canonical Layer 2 library to avoid universal-constant fragmentation. See §1.7.16 for the complete comparison matrix across Levels C, D, the rejected Option C, and the locked Option E.

```
What ZER had at Level C:
  - Bounded catalog (~130 intrinsics, growing ~1-2/year)
  - Y_naked escape hatch for the 1%
  - GCC delegation for Z
  - Maintenance: ~1-2 intrinsics/year forever

What ZER has at Level D (after pivot):
  - Frozen blessed catalog (@core::*, ~130 or trimmed to ~30-50)
  - Frozen verification engine
  - User-extensible intrinsic_def mechanism (@<lib>::*)
  - Frozen Y_naked for ~0.1% irreducible cases
  - GCC delegation for Z (unchanged)
  - 4-level conditional safety claim
  - Maintenance: ~per-decade safety class additions

The bottom-line architectural lock:
  ZER COMPILER intrinsic work converges to a fixed point.
  Catalog growth happens in user/library space.
  Safety claim is honestly conditional, greppably auditable.
  Hardware evolution doesn't touch ZER team's work.
```

**This is the final architecture for ZER's asm safety.** Locked in
2026-05-31. Level C cleanup ships first, then Level D adds intrinsic_def
on top.

### 1.6.16. The corrected 2-layer architecture (with escape hatch excluded)

> **REFINED BY §1.7.3 (Option E).** The 2-layer model below ("blessed @core::* + user-defined @<lib>::*") is restructured under Option E into a 3-layer model: **Layer 1 (ZER core: schema + operation taxonomy + mechanism, NO per-ISA bindings) + Layer 2 (per-ISA binding libraries, structurally peer, including for operations Level D would have called blessed) + Layer 3 (firmware/application code: import and call)**. The escape hatch (raw asm in naked) remains structurally separate from the layered model. The framing below — "Layer 1 blessed primitives, ZER-team-audited, with internal per-ISA dispatch authored by ZER team" — is the part Option E refines: ZER core does not author per-ISA asm bodies; those live in Layer 2 libraries (potentially ZER-team-maintained as a separate canonical library — a packaging decision, not an architectural one). The conceptual content of "Layer 1 is foundational + Layer 2 is built on top + escape hatch is parallel" carries forward, just at one more level of factoring. Read §1.7.3 for the locked 3-layer model.

Clarification on the architectural layering for Level D — the safety
story is a clean 2-layer model. Raw asm in naked functions is an
ESCAPE HATCH, not a layer.

```
LAYER 1 — Blessed @core::* intrinsics
  ZER-team-audited, FROZEN at ~130 (or trimmed to ~30-50)
  Universal foundational primitives (atomics, barriers, common CPU ops)
  Internal per-ISA dispatch (ZER-team-authored)
  Unconditionally sound

LAYER 2 — User-defined @<lib>::* intrinsics
  Library author-defined, contract-explicit
  Unbounded — grows in user/library space
  Internal per-ISA dispatch (library-author-authored)
  Conditionally sound (depends on author's contract correctness)

[ESCAPE HATCH — not a layer]
  Raw asm in naked functions
  ~0.1% irreducible (boot stubs, hand-tuned crypto)
  Outside the layered scheme — explicit unsafe boundary
  Same conceptual category as cinclude / Rust unsafe blocks
```

**Why raw asm is excluded from layering:**

Raw asm doesn't compose with Layers 1-2:
- It's not a contract-bearing primitive
- It's not in the verification engine's scope
- It's a deliberate escape from the safety story

Treating it as "Layer 3" or "Layer 0" wrongly implies it's part of the
safe architecture. It isn't. It's the explicit-unsafe boundary, same
shape as `cinclude` and Rust's raw `unsafe { asm!() }`.

**The clean 2-layer picture:**

```
ZER's safe ASM architecture:

  LAYER 1: blessed primitives (small, frozen, ZER-audited)
  LAYER 2: user-defined intrinsics (unbounded, library-authored,
                                    contract-verified)
  
  Both layers:
    - Same intrinsic_def mechanism
    - Same verification engine
    - Same call-site experience (just call the intrinsic)
    - Internal per-ISA dispatch (per-intrinsic_def)
    
  Layers differ in:
    - Who authored (ZER team vs library author)
    - Soundness (unconditional vs conditional on contract correctness)
    - Scope (universal foundational vs domain-specific)

[Separately, NOT a layer]
  Raw asm escape hatch for the 0.1% irreducible
```

**Why your framing might tempt 3 layers (and why we don't):**

A natural intuition would be:
- Layer 1: blessed primitives
- Layer 2: user intrinsics building on blessed
- Layer 3: raw asm escape

But this is wrong for two reasons:

1. **User intrinsics don't STRICTLY build on blessed primitives.** Each
   user intrinsic_def is self-contained with its own per-ISA dispatch.
   Library author writes the per-ISA asm directly, not through Layer 1.
   They MAY compose blessed primitives (e.g., calling @core::atomic_cas
   inside their intrinsic body), but this is COMPOSITION not LAYERING-
   FOR-VERIFICATION.

2. **Raw asm is escape, not a layer.** Layers are part of the safety
   story (compose with verification, follow Definition A). Raw asm is
   explicit-unsafe boundary, parallel to cinclude. Calling it Layer 3
   wrongly implies it's verified by the language; it isn't.

The honest model: **2 layers in the safety story + 1 escape hatch
parallel to other unsafe boundaries.**

**Per-ISA dispatch happens INSIDE each intrinsic_def, not BETWEEN layers:**

```
@intrinsic_def @my_lib::aes_enc_round(state: u128, key: u128) -> u128 {
    arch x86_64: {
        instructions: "aesenc %2, %1"     ← per-ISA INSIDE the def
    }
    arch aarch64: {
        instructions: "aese %0.16b, %1.16b; aesmc %0.16b, %0.16b"
    }
    arch riscv64: {
        // not supported — library author can omit
    }
    requires: ...
    safety_class: [ ... ]
}
```

Library author writes per-ISA implementations. Same shape as how ZER
team writes per-ISA for Layer 1 blessed intrinsics. The "for all ISA"
property is internal to each intrinsic_def, not a separate layer.

**Growth pattern under the corrected layering:**

```
Layer 1: FROZEN (no growth — ZER team converged at fixed point)
Layer 2: GROWS in user/library space (no ZER team involvement)
Escape:  BOUNDED at ~0.1% (boot stubs, hand-tuned crypto only)

Per-ISA dispatch: happens INSIDE each intrinsic_def
GCC: handles Z-level concerns (register/instruction/feature validation)
ZER team work: ~per-decade safety class additions to X registry
```

**Cross-reference to other sections:**

- The 4-level conditional soundness claim (Section 1.6.9) refers to
  SOUNDNESS levels (pure ZER, blessed, user-def, cinclude), NOT to
  architectural layers
- The 2-layer architecture here refers to ZER's ACTIVE safety story
  for asm operations (Layer 1 blessed + Layer 2 user-defined)
- The escape hatch (raw asm in naked) is the SAME thing as
  Section 1.5's "Layer 2 — RAW ASM IN NAKED FN" — at Level D it gets
  renamed to "escape hatch" to make its non-layer nature explicit

**Locked terminology going forward:**

| Term | Meaning |
|---|---|
| Layer 1 | Blessed @core::* intrinsics (ZER-audited, frozen) |
| Layer 2 | User-defined @<lib>::* intrinsics (library-authored, conditional) |
| Escape hatch | Raw asm in naked (~0.1% irreducible, explicit unsafe) |
| Soundness Level 1-4 | Conditional soundness claim breakdown (separate concept) |
| Per-ISA dispatch | Internal to each intrinsic_def, not a layer |

When future sessions reference "the layering," they mean the 2-layer
model + escape hatch from this subsection. The 4-level soundness claim
is a SEPARATE axis describing what's conditionally vs unconditionally
sound.

### 1.6.17. The demand/promise asymmetry — DIRECTIONAL classification

**The rule:** Any user declaration that causes the verifier to do LESS
checking on surrounding code is a **PROMISE** and must be either
checkable or explicitly taint the caller's soundness level. Any
declaration that causes the verifier to do MORE checking is a
**DEMAND** and is free (can only over-reject, never under-protect).

**Classify by effect on the verifier's workload, NOT by which syntactic
slot the declaration uses.**

This is the asymmetry that closes the wrong-contract seam (Section 1.6.6).
Without it, a wrong `safety_class` or `effect:` declaration silently
poisons the verifier's reasoning about callers. With it, user intrinsics
have full freedom in the safe direction and are blocked in the unsafe one.

**Why directional, not syntactic:**

A natural but WRONG formulation: "`requires:` = safe demand; `effect:` /
`safety_class:` = unsafe promise." This formulation misses the
load-bearing case — **output-type declarations that look like demands
but act like promises.**

Example of promise-in-demand's-clothing:

```
@intrinsic_def @my_lib::aligned_load(addr: usize) -> *aligned u32 {
    arch x86_64: { ... }
    // No `requires:`, no `effect:`, no `safety_class:` declared
    // BUT: the output type carries `*aligned`
}

Downstream code:
  *aligned u32 p = @my_lib::aligned_load(addr);
  // Compiler sees `*aligned` and SKIPS its own alignment check
  // If aligned_load's asm doesn't actually produce aligned output,
  // downstream code is silently unsafe — verifier did LESS checking
  // based on the declared output type
```

The output type `*aligned u32` is in an operand-type slot (looks like
a demand) but functions as a promise (downstream relaxes its own
checks based on it). Syntactic classification misses this case
entirely.

**The correct directional rule:**

```
For each user declaration in an intrinsic_def, ask:
  "Does this declaration cause the verifier to do LESS checking on
   code that calls or uses this intrinsic?"
  
  YES → it is a PROMISE
  NO  → it is a DEMAND
  
Promises must be EITHER:
  (a) Checkable by the verifier from existing safety infrastructure
      (e.g., compiler can prove the asm produces aligned output)
  (b) Explicitly taint the caller's soundness level
      (caller drops from "blessed-sound" to "user-conditional-sound")

Demands are always free — user can declare as many preconditions /
operand types / safety_class requirements as they want. Worst case
is false rejection (annoying, not unsafe).
```

**Examples classified directionally:**

| Declaration | Verifier effect | Classification |
|---|---|---|
| `requires: mask != 0` | More checking on callers (must prove nonzero) | DEMAND (free) |
| `inputs: { "rax" = x: u32 }` (operand type) | More checking on callers (must pass u32) | DEMAND (free) |
| `safety_class_requires: [C2_alignment]` | More checking on callers | DEMAND (free) |
| `outputs: { "rax" = result: u32 }` (plain type) | No relaxation downstream | DEMAND (free) |
| `outputs: { "rax" = result: *aligned u32 }` | Downstream skips alignment check | **PROMISE** (must be checkable or tainting) |
| `outputs: { "rax" = result: non_null *u8 }` | Downstream skips null check | **PROMISE** (must be checkable or tainting) |
| `effect: result_is_aligned_to(16)` | Downstream skips alignment check | **PROMISE** (must be checkable or tainting) |
| `safety_class_provides: [C2_alignment]` | Downstream skips alignment-class check | **PROMISE** (must be checkable or tainting) |

The same `outputs:` slot can hold a demand (plain `u32`) or a promise
(`*aligned u32`). Slot ≠ classification.

**Tie to keep:**

This is the same sound-but-conservative discipline ZER uses for `keep`:
- `keep` lets the user ADD obligations (parameter can be stored persistently)
- `keep` does NOT let the user DISCHARGE obligations downstream relies on
- Wrong `keep` annotation = false rejection (over-restricting)
- Same direction: free to demand, blocked from silently promising

**Implementation hint:**

The verifier infrastructure already exists for the demand direction —
existing safety systems (VRP, alignment, escape, MMIO, qualifier
preservation) enforce additional checks at use sites. Promise checking
requires either:
- The verifier reasoning about asm semantics (usually impossible)
- An explicit `taint_caller: <soundness_level>` annotation on the
  intrinsic_def declaring that callers drop to a lower soundness level
- A `verify_promise: <existing_check>` hook that runs an existing
  safety check on the intrinsic's output before allowing the promise

Default behavior for unverified promises: REJECTED at intrinsic_def
parse time. User must either prove checkable or explicitly taint.

**Catch the easy mistake:**

If a fresh session writes the rule as "`requires:` is safe, `effect:`
is unsafe," that's the syntactic formulation. It misses the output-type
case. Reject and reformulate directionally.

### 1.6.18. Missing-arch propagation rule

**The rule:** A call to an intrinsic with no arch-clause for the
current compile target is a **compile error at the call site**.
Supported-archs is part of the intrinsic's contract; a caller's
portability set is the intersection of supported-archs across all
intrinsics it transitively uses.

**The hole this closes:**

```
@intrinsic_def @my_lib::aes_enc_round(state: u128, key: u128) -> u128 {
    arch x86_64: { instructions: "aesenc %2, %1" ... }
    arch aarch64: { instructions: "aese ..." ... }
    // riscv64 omitted
}

Compile target: riscv64
User code: u128 result = @my_lib::aes_enc_round(state, key);

Without the rule, three possibilities and the plan didn't pick one:
  (a) compile error "intrinsic not available on this arch"   ← CORRECT
  (b) silent fallback to nothing                              ← UNSOUND
  (c) GCC catches it                                          ← IMPOSSIBLE
```

GCC delegation **cannot cover this case** because:
- GCC validates emitted asm bytes
- If no arch-clause matches, NO asm is emitted for this intrinsic on
  this target
- GCC has nothing to validate — failure is absence-of-emission, not
  presence-of-wrong-emission

**The required behavior:**

```
At intrinsic_def parse time:
  Collect supported_archs = { arch : intrinsic has arch-clause for arch }
  Attach supported_archs to intrinsic's symbol

At call site:
  current_target_arch = compile target
  if current_target_arch not in callee.supported_archs:
    COMPILE ERROR: "intrinsic @<name> not supported on <arch>;
                    supported archs: {arch1, arch2, ...}"

At function/library level:
  supported_archs(fn) = ∩ supported_archs(intrinsic)
                        for all intrinsics fn transitively uses
  This propagates upward — caller portability is bounded by
  intersection of all transitively-called intrinsics' arch support
```

**Why supported-archs propagation matters:**

Without explicit propagation, callers can compile on arch X but fail
silently when retargeted to arch Y because some transitive intrinsic
lacks a Y clause. With propagation, the caller's signature explicitly
encodes its arch portability, and porting to a new arch surfaces all
the missing-arch failures at compile time.

**Implementation:**

```
1. intrinsic_def parser: collect supported_archs from arch-clauses
2. Symbol table: store supported_archs on each intrinsic
3. Call-site checker: verify current_target in callee.supported_archs
   → emit error with directive listing missing arch
4. FuncSummary: propagate supported_archs upward as intersection
5. Library publication: supported_archs is part of public contract
```

Estimated: ~100-200 lines in checker.c + parser support.

**Catch the easy mistake:**

If a fresh session writes "GCC will catch missing-arch via assembly
failure," that's wrong — GCC can't validate absent emission. Need
explicit ZER-side check at the call site. Reject and add the
propagation rule.

---

## 1.7. Option E Pivot — Three-Layer Architecture, No Favored ISA, Program-Consequence Vocabulary (added 2026-06-06)

**This section ADDS to Levels C and D, does not remove them.** Level C is the deletion baseline (Z stays in GCC). Level D's `@intrinsic_def` mechanism is the user-extensibility primitive. Option E is the architectural refinement that:

1. Applies the **program-consequence vs hardware-consequence** vocabulary (locked in `firmware_safety_extensions.md` and `CLAUDE.md`) to the asm domain.
2. Refines the layering from Level D's "blessed-vs-user-defined" split into a cleaner **three-layer** model: **Layer 1 (ZER core: schema + operation taxonomy + mechanism, no ISA)** / **Layer 2 (per-ISA bindings: library territory)** / **Layer 3 (firmware/application: import and call)**.
3. Locks the **no-favored-ISA-in-core** decision: ZER's language core ships the closed category vocabulary + the semantic operation taxonomy + the `@intrinsic_def`/`@bind` mechanism, and ships **zero** per-ISA bindings in core. Per-ISA bindings — including x86 — are user/community library territory.
4. Identifies the **structure/semantics straddle** as the architectural reason asm uniquely requires the binding mechanism while every other ZER construct sits cleanly on one side of the program-domain/hardware-domain boundary.
5. Elevates the **closed verifier-native vocabulary** from "one of four mitigations" to "the load-bearing kind-difference from SPARK," and locks the discipline that the contract language must stay closed (no free-form `requires:` predicates).

**Status:** Architecture LOCKED IN as of 2026-06-06. Mechanical confirmation (write one `@bind` binding by hand against the schema) pending. Implementation pending; the work order is Level C cleanup → Level D mechanism → Option E factoring applied at the layering and ISA-reference points.

> **READ FIRST.** This section is the current locked architecture for ZER's asm safety. It is the lens through which Section 1.6 (Level D) should be read. Where 1.7 and 1.6 disagree on layering or on whether ZER core ships per-ISA bindings, **1.7 supersedes**. 1.6's `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18) carry forward unchanged.

### 1.7.1. The vocabulary locked elsewhere, applied here

The program-consequence vs hardware-consequence vocabulary was locked in `CLAUDE.md` (top of file, "ZER's Goal" section) and `docs/firmware_safety_extensions.md` (§1, §1a, §22). The full locked statement:

> ZER guarantees 100% program-consequence coverage: every wrong use of a value in ZER source code is caught at the use site. This includes embedded/firmware data — once a value crosses into ZER through a typed boundary (intrinsic return, MMIO read, cinclude function, linker symbol, asm output, source literal), it is program data and ZER verifies every program-level operation on it. Hardware-consequence — peripheral side effects, datasheet-specific value correctness, silicon behavior — is floor, out of scope for any language.

The vocabulary discipline that must not equivocate:

- **Program-consequence** — what happens when a value is used wrongly inside ZER source. Caught at 100%. The use is in the program, so ZER owns it.
- **Hardware-consequence** — what happens when a hardware fact is wrong relative to user belief (peripheral doesn't actually clear on read, baud value is wrong-for-this-board, asm instruction doesn't actually have the categories declared). Floor. The fact lives outside the program, never enters it, so ZER has nothing to verify.

Applied to asm:

- **Program-consequence (ZER total, 100%):** Every call site of every intrinsic — `@arith_add_with_carry(a, b, c_in)`, `@load_acquire(ptr)`, `@barrier_full()` — is verified against the operation's declared categories. Operand types, escape, provenance preservation through outputs, qualifier preservation through inputs, clobber accounting, ordering constraints, context permissions. Wrong use at the call site = compile error. This holds regardless of which ISA binding the caller compiles against, because the verifier reasons over the schema, not over asm strings.
- **Hardware-consequence (floor, surfaced at the `@bind` declaration site):** Whether the asm body inside a `@bind` declaration actually has the categories the declaration claims. If a binding declares `clobbers_flags + no_memory_effect` but the asm secretly writes memory, ZER cannot detect it. This is the floor for asm just as datasheet-correctness is the floor for MMIO declarations and signature-correctness is the floor for cinclude. Same floor pattern, different boundary location.

The phrase to never let collapse:

> "100% program-consequence" must never read as "ZER verifies the asm body matches the contract."

It doesn't. ZER verifies that every program-domain operation on values passing through the intrinsic boundary respects the declared categories. The declaration's correctness against silicon is the floor, the same floor that exists for every typed boundary into hardware-domain facts.

### 1.7.2. Why Option E refines Level D's layering

Level D's layering (§1.6.16, "the corrected 2-layer architecture") split intrinsics into:

- **Layer 1:** Blessed `@core::*` intrinsics, ZER-team-audited, frozen, **with internal per-ISA dispatch authored by ZER team**.
- **Layer 2:** User-defined `@<lib>::*` intrinsics, library-authored, conditional, with internal per-ISA dispatch authored by library author.
- **Escape hatch:** Raw asm in naked.

The Option E refinement: the "internal per-ISA dispatch authored by ZER team" part of Layer 1 is the architectural commitment Option E removes. ZER core does NOT author per-ISA asm bodies. ZER core ships the schema (closed category vocabulary) + the operation taxonomy (the set of semantic operations) + the `@intrinsic_def` / `@bind` mechanism, and stops there. Per-ISA bindings live in libraries — including x86, including the operations Level D would have called "blessed."

This is a structural refinement, not a contradiction of Level D. Level D's `@intrinsic_def` mechanism is preserved verbatim. The demand/promise asymmetry rule (§1.6.17) is preserved. The missing-arch propagation rule (§1.6.18) is preserved. The 4-level conditional soundness claim (§1.6.9) is refined: the "blessed" level (Level 2 of the conditional soundness breakdown) is now bound to the schema correctness, not to a ZER-team-shipped per-ISA catalog.

### 1.7.3. The three layers in dependency order

The locked three-layer model:

```
LAYER 1 — ZER core (frozen, shipped once + rare release extensions)
─────────────────────────────────────────────────────────────────
  Lives in: language core, compiler binary
  Authored by: ZER team
  Frequency of change: rare (per-decade for category vocabulary,
                       handful per several years for operation taxonomy)

  Contents:
    - Closed CATEGORY VOCABULARY (the schema)
        clobbers_flags, clobbers_register(name),
        reads_mem(width, ordering), writes_mem(width, ordering),
        requires_aligned(n), requires_nonzero(operand),
        requires_in_range(operand, lo, hi),
        memory_barrier(acquire | release | seqcst),
        produces_carry, consumes_carry,
        changes_privilege(from, to),
        control_flow(returns | jumps_to | calls),
        provenance_clear_on_output(operand),
        value_in_value_out,
        no_memory_effect, no_flag_effect,
        ... [closed set, ZER-owned, extended only via release]

    - SEMANTIC OPERATION TAXONOMY (the named operations)
        @arith_add_wrap(a, b) -> T
        @arith_add_with_carry(a, b, c_in) -> {result, c_out}
        @arith_sub_wrap, @arith_sub_with_borrow,
        @arith_mul_wide, @arith_div_unchecked,
        @bit_and, @bit_or, @bit_xor, @bit_shift_left, @bit_shift_right,
        @bit_rotate_left, @bit_rotate_right,
        @bit_scan_forward, @bit_scan_reverse, @bit_count_leading_zeros,
        @bit_count_trailing_zeros, @bit_population_count,
        @load_aligned(ptr), @load_unaligned(ptr),
        @load_acquire(ptr), @load_relaxed(ptr),
        @store_aligned(ptr, val), @store_unaligned(ptr, val),
        @store_release(ptr, val), @store_relaxed(ptr, val),
        @atomic_cas(ptr, expected, new),
        @atomic_swap, @atomic_fetch_add, @atomic_fetch_sub,
        @atomic_fetch_and, @atomic_fetch_or, @atomic_fetch_xor,
        @barrier_acquire, @barrier_release, @barrier_full,
        @barrier_compiler_only,
        @cpu_disable_int, @cpu_enable_int, @cpu_wait_int, @cpu_pause,
        @cpu_syscall, @cpu_sysret, @cpu_iret,
        @cpu_read_msr(idx), @cpu_write_msr(idx, val),
        @cpu_read_cr(n), @cpu_write_cr(n, val),
        @cpu_cache_invalidate(addr), @cpu_cache_writeback(addr),
        @cpu_dma_start(buf, channel),
        ... [closed-after-design set, ZER-owned, extended via release]

    - MECHANISM
        @intrinsic_def     (declare a new semantic operation in core or library)
        @bind              (provide a per-ISA implementation for a declared operation)
        verifier dispatch  (operate on categories, dispatch to existing Z-rules
                            + existing safety analyses; never on asm strings)

    - DELEGATIONS
        Z-level concerns (register validity, instruction validity, CPU feature
        gating) → GCC, as per Level C
        Hardware-consequence (whether silicon honors the binding's declared
        categories) → engineer territory at the @bind declaration site

  What Layer 1 does NOT contain:
    - No per-ISA asm bodies
    - No "x86 reference" or "ARM reference" or any ISA-specific catalog
    - No free-form predicate language (closed vocabulary only)
    - No user-extensibility of categories or operations
      (extensions go through ZER core release with review)

LAYER 2 — Per-ISA binding library (community / per-vendor / per-project)
───────────────────────────────────────────────────────────────────────
  Lives in: external libraries imported by user firmware
  Authored by: library maintainers — community, vendor, or project-local
  Frequency of change: per (operation, ISA) pair, authored once,
                       frozen-after-review

  Contents:
    - @bind declarations mapping each semantic operation in Layer 1's
      taxonomy to per-ISA asm
    - Per-ISA asm bodies (instructions, operand constraints, clobbers)
    - Category SELECTION (not invention) from Layer 1's vocabulary
    - safety: annotations for review (one declaration ↔ one paragraph
      explaining why the categories match what this asm does on this ISA)

  Example library structure:
    zer-asm-x86            — x86_64 bindings for Layer 1 operations
    zer-asm-arm-cortex-m   — ARM Cortex-M bindings
    zer-asm-arm-cortex-a   — ARM Cortex-A bindings (different memory model)
    zer-asm-riscv          — RISC-V bindings (including no-flag emulation
                              sequences for operations like add_with_carry)
    zer-asm-xtensa-lx7     — Xtensa LX7 bindings
    zer-asm-msp430         — TI MSP430 bindings
    ...

  Each library:
    - Is structurally peer to every other Layer 2 library under the
      Layer 1 schema
    - Has no privileged position
    - Authored by whoever brings up that ISA
    - Reviewed against the closed-checklist (do declared categories
      match what this asm does on this ISA?)
    - Frozen after review for that (operation, ISA) pair

  What Layer 2 does NOT do:
    - Cannot invent new categories (must select from Layer 1's vocabulary)
    - Cannot invent new operations (must bind operations Layer 1 declared)
    - Cannot publish free-form requires: predicates
    - If a binding cannot be expressed in Layer 1's vocabulary, the fix
      is a Layer 1 schema-extension request via ZER core release
      (NEVER a Layer 2 freedom)

LAYER 3 — Firmware / application (the actual program)
──────────────────────────────────────────────────────
  Lives in: user firmware source code
  Authored by: firmware/application engineer (the audience that matters
               for adoption ergonomics)
  Frequency of activity: continuous (this is the regular coding loop)

  What this person does:
    1. Imports a Layer 2 binding library for their target ISA:
         import zer_asm_arm_cortex_m;
    2. Imports Layer 1's operation taxonomy (transitively via Layer 2,
       or explicitly):
         use @core::ops;
    3. Calls operations as typed functions:
         let cur = *GPIOA_ODR;
         let next = @arith_xor(cur, 0x1);
         *GPIOA_ODR = next;
         @barrier_release();
    4. That is the entire workflow.

  What this person does NOT do:
    - Does not write asm
    - Does not author @bind declarations
    - Does not declare or select categories
    - Does not need to read an ISA manual to use operations
    - Does not need to know what asm the binding emits

  Verification at this layer:
    - Every call site is verified against the operation's declared
      categories (Layer 1's contract)
    - Verifier dispatches to existing analyses: Z-rules, escape,
      provenance preservation, qualifier preservation, alignment,
      ordering, context flags
    - Program-consequence: 100% over every call site, locked
```

This is the locked three-layer architecture. The pattern parallels firmware (mmio + @inttoptr in core, per-chip addresses in user space, firmware code that just uses typed pointers), the pattern parallels cinclude (mechanism in core, vendor HAL in user space, firmware code that just calls typed wrappers), and the pattern parallels every other domain in ZER where structural primitives live in core and substrate details live in user space.

### 1.7.4. The locked Layer 1 schema design rules

Layer 1's contents are closed. The discipline:

**Category vocabulary rules:**
1. Categories describe **structural behavior kinds** that exist across ISAs (clobbers a flag register class, reads memory at width W with ordering O, etc.), not ISA-specific instruction properties.
2. Categories are added only via ZER core release with review.
3. Categories support an "N/A on this ISA" disposition — for example, `clobbers_flags` is N/A on RISC-V because no flag register exists. The verifier handles N/A correctly (does not require the binding to declare flag categories on ISAs that lack flags).
4. Categories must be verifier-native: each category dispatches to an existing safety analysis. No category that requires a brand-new analysis.
5. No free-form predicates. If a category-shaped concept cannot be expressed as a selection-from-finite-set, it does not enter the vocabulary.

**Operation taxonomy rules:**
1. Operations are named for what they MEAN, not for any ISA's mnemonic. `@arith_add_with_carry` not `@x86_adc`.
2. Each operation has a fixed category profile that applies to every ISA binding of that operation. If the category profile differs structurally between ISAs (e.g., x86 ADD clobbers flags, ARM plain ADD doesn't), those are different semantic operations (`@arith_add_wrap` vs `@arith_add_with_carry`) — not one operation with per-ISA category variation.
3. Operations are added via ZER core release. The rate is bounded by genuinely-new semantic operation kinds appearing in hardware (per-several-years for vector extensions, novel atomic primitives, new memory ordering modes).
4. Operations are typed at the ZER level (operand types, return types). Per-ISA bindings cannot change the typed signature.
5. Operation taxonomy is closed-after-design. Extensions go through ZER core release.

**Mechanism rules:**
1. `@intrinsic_def` declares a new operation (Layer 1 release) or a new core-blessed wrapper. Used by ZER team and by library authors of new operations they're seeking promotion.
2. `@bind` provides a per-ISA implementation of an existing operation. Used by Layer 2 library authors.
3. Per-ISA dispatch is internal to a `@bind` (one binding per (operation, ISA) pair).
4. Verifier reasons over categories only. Never over asm strings.
5. GCC handles Z-level concerns (register validity, instruction validity, CPU feature gating) as per Level C.

### 1.7.5. The structure/semantics straddle — the architectural reason this factoring is forced

This is the deepest architectural observation in the asm safety thread, and it should be preserved as the load-bearing reason for Option E's shape.

**Every other ZER construct sits cleanly on one side of the program-domain / hardware-domain boundary:**

- A `u32` value, regardless of origin (hardware read, file, network, source literal): program-domain at every operation, ZER owns 100%.
- An `mmio` address declaration: hardware-domain claim at the declaration site (does this address really name the peripheral?), program-domain at every downstream use through the typed pointer. The boundary is crossed cleanly at the declaration.
- A `cinclude` function signature: hardware-domain claim at the signature declaration, program-domain at every call site. Same clean cross.
- A linker symbol extern: same pattern.
- A typed register access: structure in program, criterion in datasheet — both sides exist but they live in separate tokens (the typed pointer vs the address value).

**An asm instruction is the exception. It straddles.**

`adc $0, $1` is one token that simultaneously:
- Carries operand structure (program-domain — Z-rules apply to inputs and outputs, escape analysis applies to register clobbers, provenance clearing applies to outputs, qualifier preservation applies through the boundary, ordering constraints apply, context flags apply)
- Carries semantic meaning (hardware-domain — what flags get clobbered, what state mutates, what precondition is required for correctness, what the instruction actually does to the machine — and this varies per ISA)

You cannot separate these by reading the token. The instruction's structural connections and its hardware semantics are fused in one mnemonic. That fusion is unique to asm among ZER constructs.

**Why this forces Option E specifically:**

1. Asm's semantics are per-ISA hardware facts (the ISA manual is the datasheet for instruction meaning).
2. Any safety system that wants to reason about asm meaning must import that meaning from outside the program text.
3. "Outside the program, per-ISA" is by definition a binding (a `@bind` declaration).
4. ZER core cannot ship "what `adc` means" without committing to an ISA's semantics in the language core — which would make ZER a "portable language" wearing a costume of universality while actually carrying a favored-ISA assumption.
5. The only factoring that preserves both "ZER reasons about asm safety structurally" AND "ZER does not commit to an ISA" is: structure-half stays in core (universal, ISA-less, frozen — Z-rules, operand boundaries, the category vocabulary, the operation taxonomy), semantics-half imported per-ISA via `@bind` (user-authored, hardware-consequence floor surfaced at the binding site).

That is Option E. It is not "the cleanest of three options" — it is the unique architecture that respects the structure/semantics straddle without either committing to one ISA or refusing to reason about asm meaning at all.

**Any alternative either:**
- (A) commits ZER core to one ISA's semantics — breaking architecture-agnostic positioning and making ZER a "<favored-ISA> language" in disguise; or
- (B) refuses to reason about asm meaning at all — abandoning the asm safety claim and dropping back to "asm is just text we hand to GCC."

Option E is what's left after rejecting both. The structure/semantics straddle dictates the shape; the choice was made by the fusion in the token, not by architectural preference.

### 1.7.6. The closed-vocabulary kind-difference from SPARK — load-bearing, not bullet-point

In earlier framings, four "mitigations" were listed for the SPARK-shape trust gap in `@intrinsic_def`:

1. Frozen category vocabulary
2. Per-operation contract, not per-call
3. Audit visibility at use site
4. Per-author trust (library author authors once, users consume)

Three of these are **quantitative** mitigations — they make the SPARK trap smaller (fewer contracts, more visible, authored once). They are real but they are differences of degree. SPARK could in principle adopt all three and still be SPARK.

The first one is the only **kind-difference**, and it is the load-bearing distinction from SPARK. It must be elevated from bullet point to thesis:

> **SPARK contracts are arbitrary predicates; `@intrinsic_def` / `@bind` contracts can only select from a closed, ZER-owned category vocabulary that the verifier already knows how to reason about.**

Why this is a different failure topology, not just a smaller surface:

- A SPARK contract can be wrong in **unbounded ways**, because the predicate language is unbounded. You can assert any logical claim, including ones that are subtly self-inconsistent, vacuously discharged by the prover, or true on the implementation but false on the surrounding program.
- A `@bind` contract can be wrong in **exactly one way**: the declared categories do not match what the asm body actually does on this ISA. It cannot be wrong about anything else, because there is nothing else to declare — the contract language consists only of selections from the closed category vocabulary.

This makes the trust gap **auditable against a fixed checklist** instead of against an open-ended logical claim:

| To review a SPARK contract | To review a `@bind` declaration |
|---|---|
| Understand the predicate language | Skim the closed category vocabulary list |
| Decide whether the predicate is well-formed | (Not a concern — categories are syntactic) |
| Decide whether the predicate is true of the implementation | For each declared category, check the asm body |
| Open-ended reasoning task | Finite checklist |
| Surface grows with predicate complexity | Surface is fixed by the schema |

**This is genuinely stronger than SPARK** — not because the trust gap is absent (it isn't; the other Claudes in the design discussion were correct to push back on any framing that suggested otherwise), but because the gap has a **fixed, finite shape**, so verifying-the-contract-against-the-asm is a mechanical checklist task instead of a proof obligation.

SPARK's gap is open-shaped. ZER's gap is closed-shaped. Same existence; different reviewability.

### 1.7.7. The hard line: closed vocabulary or the kind-difference collapses

The closed-vocabulary discipline is the whole differentiator. The moment Layer 2 is allowed to carry a free-form `requires:` predicate (an arbitrary logical precondition) or a free-form `effect:` clause or any other open-shape declaration, the kind-difference dissolves and ZER becomes "tidier SPARK," not a structurally distinct architecture.

The rules that must hold:

1. **`@bind` declarations select from the Layer 1 category vocabulary.** They do not invent categories.
2. **`@bind` declarations bind operations Layer 1 declared.** They do not invent operations.
3. **No free-form predicate language in Layer 2.** Specifically:
   - `requires:` clauses must select from a finite precondition vocabulary (e.g., `requires_nonzero(operand)`, `requires_aligned(operand, n)`, `requires_in_range(operand, lo, hi)`, `requires_msr_writable(msr_idx)`). Never an arbitrary logical predicate.
   - `effect:` clauses must select from a finite postcondition vocabulary (e.g., `effect_in_range(result, lo, hi)`, `effect_aligned(result, n)`, `effect_provenance_cleared(output)`). Never an arbitrary logical predicate.
4. **If a binding cannot be expressed in the existing schema, that is a Layer 1 schema-extension request via ZER core release.** It is NEVER a Layer 2 freedom to open the vocabulary locally.
5. **Demand/promise asymmetry rule from §1.6.17 still applies.** Promises that cause the verifier to do less checking on callers must either be verifier-checkable or explicitly taint the caller's soundness level. The closed-vocabulary discipline narrows the set of expressible promises but does not relax the asymmetry rule.

The discipline implies a maintenance commitment on ZER team: the schema design must be expressive enough that real-world bindings can be expressed without users feeling the need for free-form predicates. If users keep hitting "the vocabulary is missing X" cases, that is a signal that the schema is incomplete — the fix is to extend the schema (Layer 1 release with review), never to open Layer 2.

**This is the work.** Not "ship x86 reference or not" — that's a packaging decision, separable from the architecture. The real work is making the Layer 1 schema rich enough to handle structurally different machine models (flag-bearing vs flagless ISAs, weak vs TSO memory models, vector vs scalar register classes, CHERI vs non-CHERI capability models) without escape into open-shape predicates. Get this right once and the schema is stable for the long term.

### 1.7.8. The safety vs governance register separation

A critical observation from the design discussion: the question of **"who maintains canonical Layer 2 binding libraries"** is a packaging/governance question, separable from the safety architecture. They live in two distinct registers and must not be conflated.

**Safety register (locked by Option E):**
- Does program-consequence stay 100% under this layering? **Yes.**
- Is binding-correctness floor cleanly named at the `@bind` site? **Yes.**
- Is the closed-vocabulary kind-difference preserved? **Yes.**
- Is the structure/semantics straddle respected? **Yes.**

The safety architecture holds **regardless of who ships the x86 binding library, whether the ZER team blesses one, whether the community owns it, or whether vendor-specific libraries proliferate.**

**Governance / packaging register (open, decidable empirically):**
- Who maintains the canonical x86 binding library?
- Does ZER core's release process include blessing one Layer 2 library per supported ISA?
- Does the community own ISA-bring-up entirely?
- Is there a `zer-asm-x86` repo maintained by the ZER team, structurally peer to community libraries but receiving extra review attention?

These are real ecosystem-coordination questions. They affect onboarding friction, fragmentation risk, default-library competition, and version-skew management. They do NOT affect the safety claim, because Layer 1 (the language core, where safety is established) makes no commitment about which Layer 2 library is canonical.

**Why this separation matters:**

The governance question is reversible. Whether the canonical x86 binding library ships:
- As part of `zer-core` binary,
- As a separately-maintained `zer-asm-x86` repo authored by ZER team,
- Or as a community library that becomes de-facto standard,

these are packaging choices that can change later without breaking any user code, because the user-facing API (the operation taxonomy + categories in Layer 1) is locked at the schema layer.

So you do not have to decide the governance question now to lock the safety architecture. The safety architecture locks at Option E. The governance question is independent and can be decided empirically — by writing one Layer 2 binding for a common operation (the x86 `adc` for `@arith_add_with_carry`, or the RISC-V no-flag carry-emulation sequence) and seeing what packaging model makes sense given the artifact's size, complexity, and review needs.

**The honest framing on packaging:**

A genuine pushback worth recording: under Option E with no x86 reference in core, every single user/library reimplements bindings that are completely identical across all users. `adc` on x86 is `adc` for everyone — it is a universal constant, not a per-user fact. Delegating a universal constant to N users means either fragmentation (N incompatible binding libraries) or de-facto standardization without official governance (one community library becomes load-bearing while ZER team has no formal commitment to it).

This is a real coordination concern. It does not break the safety architecture, but it is a real ecosystem-design question.

The middle-path resolution the design discussion landed on: **ship the Layer 1 schema in core, no per-ISA bindings in language core, AND maintain a small canonical Layer 2 binding library (e.g., `zer-asm-x86`) as a separately-maintained ZER-team-owned repo that is structurally peer to community libraries but receives extra review attention.** This preserves Option E's "no favored ISA in the language" while applying the "universal constant ships once, governed" insight at the library layer instead of the core layer.

This is not C and it is not E-as-stated; it is E's architecture with one canonical Layer 2 library maintained by the same team. The architectural commitment is locked at E. The packaging decision about who maintains x86 is a Layer 2 governance question that can be settled empirically.

### 1.7.9. The full workflow per layer

To make the layering concrete, here is the end-to-end workflow for each layer.

**Layer 3 workflow — firmware/application author (the audience that decides ZER's adoption):**

This person's workflow is **not** extremely different from regular ZER coding. They:

1. Import a per-ISA binding library matching their target:
   ```
   import zer_asm_arm_cortex_m;
   ```

2. Declare their firmware-side things (mmio regions, linker symbols, vector table — using the firmware extension primitives locked in `firmware_safety_extensions.md`):
   ```
   mmio 0x40020000..0x40020FFF;
   volatile *u32 GPIOA_ODR = @inttoptr(volatile *u32, 0x40020014);
   ```

3. Write code calling semantic operations as typed functions:
   ```
   fn toggle_led() {
       let cur = *GPIOA_ODR;                  // volatile read
       let next = @arith_xor(cur, 0x1);       // semantic op
       *GPIOA_ODR = next;                     // volatile write
       @barrier_release();                    // memory barrier
   }
   ```

4. That is the entire workflow.

For this person, asm is **invisible**. They call `@arith_xor(...)` and `@barrier_release()` and the compiler routes through whichever Layer 2 binding library was imported. They never see `eor`, `dmb`, or any other ISA-specific instruction. The cognitive model is identical to calling any other typed function.

**No primitive declarations, no contract authoring, no schema awareness. Same ergonomics as importing any ZER library.**

This is the load-bearing usability property. The architecture is radical in Layer 2, but Layer 3 is ergonomically normal — which is what matters for the audience that decides whether ZER gets adopted.

**Layer 2 workflow — per-ISA binding library author (a smaller audience, but the place where the SPARK-family work lives):**

This person's workflow IS different from writing C inline asm. They:

1. Pick a semantic operation from Layer 1's taxonomy:
   ```
   @arith_xor(a: T, b: T) -> T
       where T in {u8, u16, u32, u64}
       categories: { no_memory_effect, no_flag_effect, value_in_value_out }
   ```

2. Author a `@bind` declaration mapping the operation to per-ISA asm:
   ```
   @bind arith_xor isa: arm_cortex_m {
       asm { instructions: "eor $0, $1, $2"
             inputs: [a, b]
             outputs: [result]
             safety: "ARM EOR: bitwise xor, plain form does not affect
                      flags; categories declared in @arith_xor's
                      Layer 1 entry (no_memory_effect, no_flag_effect,
                      value_in_value_out) all hold on this ISA." }
   }
   ```

3. Select categories from Layer 1's vocabulary. **Never invent.** If a category needed for this binding does not exist in the vocabulary, submit a Layer 1 schema-extension request via ZER core release.

4. Get the `@bind` declaration reviewed against the closed checklist:
   - For each declared category, does the asm body honor it?
   - Are all operand widths and types consistent with the operation's Layer 1 signature?
   - Are clobbers complete (no flag-clobbering asm with `no_flag_effect` declared)?
   - Are ordering constraints honest (no acquire/release mismatches)?
   - Does the safety: annotation explain the binding clearly enough for future review?

5. Once reviewed, the binding is frozen for that (operation, ISA) pair.

This is mechanical to author (selection from a finite menu, not predicate authorship) and mechanical to review (finite checklist against a closed schema, not open proof obligation). It is genuinely different from C-style "write asm, hope it works" — but it is bounded by the schema, which is the whole point of Option E.

**Layer 1 workflow — ZER core / schema author (the smallest audience, the slowest cadence):**

This is design work, not coding work. It happens:

1. **At initial schema design** — enumerate the closed category vocabulary, identifying every category kind needed to express the structural-behavior properties of asm operations across all machine models the architecture will support (flag-bearing, flagless, weak-memory, strong-memory, CHERI, non-CHERI, vector, scalar). Get this right once and most extensions never happen.

2. **At operation taxonomy design** — enumerate the set of semantic operations the language ships in its closed-after-design taxonomy. Bias toward generality: prefer fewer, more general operations over many specialized ones.

3. **At rare schema extensions** — when a Layer 2 library author or community demand identifies a category kind or operation kind the existing schema cannot express, and the extension is genuinely structural (not a workaround for missing operations), go through ZER core release with review and add it.

4. **At per-decade evolutions** — when a fundamentally novel hardware-precondition kind emerges (the CHERI capability model is the canonical example), extend the schema to accommodate it. This is rare by design — the schema should be stable for many years between such extensions.

This is the work the ZER team owns. It is bounded, slow-cadence, and the design discipline is locked: closed vocabulary, no free-form predicates, structural categories only, verifier-native dispatch only.

### 1.7.10. Concrete examples of each operation in Layer 1

To make the schema design intent concrete, here are illustrative shape sketches for several Layer 1 operations and their per-ISA bindings. **These are illustrative, not implementation specifications.** The exact category vocabulary and operation signatures will be locked at schema design time.

**Example 1 — Arithmetic with carry (the canonical ADD/flags case):**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def arith_add_with_carry(a: T, b: T, c_in: u1) -> {result: T, c_out: u1}
    where T in {u8, u16, u32, u64}
    categories: {
        no_memory_effect,
        produces_carry,
        consumes_carry,
        value_in_value_out,
        clobbers_flags    // declared at Layer 1; binding chooses whether
                          // this category applies via the ISA-N/A mechanism
    }
    safety: "Add with carry-in producing carry-out. The clobbers_flags
             category is N/A on ISAs that lack a flag register;
             flagless ISAs must emulate via GPRs while preserving the
             carry-in/carry-out semantics."

// Layer 2 (zer-asm-x86)
@bind arith_add_with_carry isa: x86_64 {
    asm { instructions: "adc $b, $a"
          inputs:  [a, b, c_in => CF]
          outputs: [result => a, c_out => CF]
          clobbers: ["flags"]
          safety:  "x86 ADC: a := a + b + CF, sets CF based on result.
                    All declared categories hold: no memory effect,
                    produces and consumes carry, value_in_value_out,
                    and clobbers_flags is honored (CF/ZF/SF/OF
                    affected as documented in Intel SDM Vol 2 ADC)." }
}

// Layer 2 (zer-asm-arm-cortex-m)
@bind arith_add_with_carry isa: arm_cortex_m {
    asm { instructions: "adcs $0, $1, $2"
          inputs:  [a, b, c_in => C]
          outputs: [result => $0, c_out => C]
          clobbers: ["NZCV"]
          safety:  "ARM ADCS: rd := rn + op2 + C, sets NZCV.
                    All declared categories hold; clobbers_flags
                    honored via NZCV update." }
}

// Layer 2 (zer-asm-riscv)
@bind arith_add_with_carry isa: riscv64 {
    asm { instructions: "add $r, $a, $b\n"
                        "sltu $t1, $r, $a\n"
                        "add $r, $r, $c_in\n"
                        "sltu $t2, $r, $c_in\n"
                        "or $c_out, $t1, $t2"
          inputs:  [a, b, c_in]
          outputs: [result => $r, c_out]
          clobbers: ["$t1", "$t2"]
          isa_disposition: {
              clobbers_flags: N/A  // RISC-V has no flag register
          }
          safety:  "RISC-V emulates carry-add via SLTU sequence.
                    No flag register exists; clobbers_flags marked
                    N/A. Categories produces_carry, consumes_carry,
                    value_in_value_out, no_memory_effect all hold." }
}
```

The same Layer 1 operation has structurally different per-ISA realizations. On flag-bearing ISAs the binding is small and uses the flag register; on flagless ISAs the binding is a multi-instruction emulation sequence. Both bindings honor the same Layer 1 contract. Layer 3 firmware code calls `@arith_add_with_carry(a, b, c)` and the verifier checks every call site against the Layer 1 contract regardless of which binding compiles.

**Example 2 — Acquire load (memory ordering):**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def load_acquire(ptr: *T) -> T
    where T in {u8, u16, u32, u64}
    categories: {
        reads_mem(sizeof(T), acquire),
        requires_aligned(ptr, alignof(T)),
        no_flag_effect,
        value_in_value_out
    }
    safety: "Acquire-ordered load. Reads from ptr with acquire ordering
             semantics — subsequent reads and writes cannot be reordered
             before this load. Requires aligned pointer."

// Layer 2 (zer-asm-x86)
@bind load_acquire isa: x86_64 {
    asm { instructions: "mov ($1), $0"
          inputs:  [ptr]
          outputs: [result => $0]
          clobbers: []
          safety:  "x86 has acquire semantics for ordinary loads under
                    TSO. The MOV instruction satisfies acquire ordering
                    without explicit barrier. Aligned access requirement
                    holds via declared category." }
}

// Layer 2 (zer-asm-arm-cortex-a)
@bind load_acquire isa: arm_cortex_a {
    asm { instructions: "ldar $0, [$1]"
          inputs:  [ptr]
          outputs: [result => $0]
          clobbers: []
          safety:  "ARMv8 LDAR: load-acquire register. Hardware
                    enforces acquire ordering. Aligned access required
                    by ISA and declared category." }
}

// Layer 2 (zer-asm-riscv)
@bind load_acquire isa: riscv64 {
    asm { instructions: "lw $0, ($1)\n"
                        "fence r,rw"
          inputs:  [ptr]
          outputs: [result => $0]
          clobbers: []
          safety:  "RISC-V acquire load: ordinary LW followed by
                    fence r,rw. Acquire ordering: subsequent reads
                    and writes cannot reorder before the load.
                    Aligned access required." }
}
```

**Example 3 — Memory barrier:**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def barrier_full()
    categories: {
        memory_barrier(seqcst),
        no_memory_effect,
        no_flag_effect,
        value_in_value_out
    }
    safety: "Full sequential-consistency memory barrier.
             Prevents any reordering of memory operations across
             this point."

// Layer 2 (zer-asm-x86)
@bind barrier_full isa: x86_64 {
    asm { instructions: "mfence"
          inputs: []
          outputs: []
          clobbers: []
          safety: "x86 MFENCE: full memory barrier, seqcst." }
}

// Layer 2 (zer-asm-arm-cortex-a)
@bind barrier_full isa: arm_cortex_a {
    asm { instructions: "dmb ish"
          inputs: []
          outputs: []
          clobbers: []
          safety: "ARMv8 DMB ISH: data memory barrier, inner shareable
                    domain. Provides seqcst ordering across the
                    inner shareable domain." }
}

// Layer 2 (zer-asm-riscv)
@bind barrier_full isa: riscv64 {
    asm { instructions: "fence rw,rw"
          inputs: []
          outputs: []
          clobbers: []
          safety: "RISC-V FENCE rw,rw: full memory barrier." }
}
```

These examples illustrate the pattern: Layer 1 declares the semantic operation with its category profile; Layer 2 provides per-ISA realizations that honor the contract; Layer 3 firmware code calls the operation without seeing the asm. The verifier's reasoning is over the Layer 1 categories — every call site of `@barrier_full()` is treated as a seqcst memory barrier regardless of which ISA binding compiles, so subsequent operations are correctly ordered.

### 1.7.11. The mechanical confirmation test — write one binding by hand

The whole design verifies under one experiment, which is the artifact-decides discipline applied at the right granularity:

**Write one Layer 2 `@bind` by hand against the schema. Write one Layer 3 call site that uses it. Check the result.**

Specifically:

1. Write the x86 binding for `@arith_add_with_carry` against the schema (the `adc` instruction).
2. Write the RISC-V binding for the same operation (the SLTU-based carry emulation sequence).
3. Write one Layer 3 firmware function that uses `@arith_add_with_carry` — for example, a multi-precision addition routine.
4. Compile it for both targets. Inspect:
   - Does the Layer 3 code know anything about which binding compiles? It should not.
   - Could a firmware author who has never read an x86 manual or a RISC-V manual use `@arith_add_with_carry` correctly by just importing the relevant binding library?
   - Is the x86 `@bind` declaration tiny (one instruction line plus the safety annotation)? It should be.
   - Is the RISC-V `@bind` declaration substantial (a real authored sequence with intermediate registers)? It should be.
   - Does the category vocabulary express both bindings honestly, including the "clobbers_flags N/A on RISC-V" disposition?

If yes to all: the three layers are clean. Layer 2 fully absorbed the per-ISA complexity. Layer 3 ergonomics is normal. The schema is expressive enough.

If Layer 3 leaks — if the firmware author has to understand the binding to use the operation correctly — Layer 2 is not doing its job, or the operation taxonomy is wrong (operations should be self-describing through categories alone). The fix is at Layer 1 (refine the operation signature or category profile), not at Layer 2 (do not add ad-hoc workarounds in bindings).

If the binding cannot be expressed in the existing category vocabulary — if the author keeps reaching for a free-form `requires:` predicate — that is the signal the schema is incomplete. The fix is a Layer 1 schema-extension request through ZER core release. Not a Layer 2 freedom.

**This experiment is the only confirmation that does not drift.** Architecture discussion can ratify the framing; only the artifact (one binding written, one call site compiled) confirms the layering actually delivers what the design claims.

### 1.7.12. The honest scope under Option E — what ZER claims, what it does not

The full claim, stated with the locked vocabulary:

**Program-consequence coverage at 100%.**

Every call site of every Layer 1 operation in Layer 3 firmware code is verified against the operation's declared categories. The verifier dispatches to existing safety analyses — Z-rules (operand boundary checks for asm-produced values), escape analysis (no asm-output value escapes through a memory-clobbering binding to a typed pointer outside its scope), provenance preservation (asm outputs receive provenance_clear and require explicit `@ptrcast` round-trip before re-typing), qualifier preservation (volatile passes through every binding boundary without strip), alignment checks (when categories declare `requires_aligned` the call site is verified or runtime-trapped), ordering enforcement (memory barriers declared by category profile honor the ordering at the verifier level even if the underlying asm uses different instructions per ISA), context propagation (asm-bearing bindings called inside `interrupt`-marked or `@critical`-marked context inherit the context's ban list).

This holds **regardless of which Layer 2 binding library is imported**. The verifier reasons over Layer 1 categories. Layer 2's asm strings are opaque to the verifier; only the declared categories matter.

This holds **regardless of whether the binding's declared categories actually match the asm semantics on this ISA**. That is the hardware-consequence floor, surfaced at the `@bind` site, library-author's responsibility, reviewable as a finite checklist against the closed schema. The program-consequence claim does not depend on binding-correctness; it depends on the schema being honored at every call site, which is structural verification.

**Hardware-consequence (the floor, named):**

What ZER does NOT verify:

- Whether the asm body inside a `@bind` declaration actually has the categories the declaration claims. If a binding declares `clobbers_flags + no_memory_effect` and the asm secretly writes memory, ZER cannot detect it. Library author owns this; reviewer with knowledge of the ISA checks via the finite checklist.
- Whether the asm produces semantically-correct output for the operation. If a binding for `@arith_add_with_carry` actually computes `a XOR b XOR c_in` due to a typo, ZER cannot detect it. The operation's typed signature is honored; the semantic equivalence is not verified.
- Whether the per-ISA implementations of a single Layer 1 operation are semantically equivalent across ISAs. Cross-ISA equivalence is library-author and code-review responsibility. ZER verifies that each binding honors its declared categories on its own ISA; cross-ISA equivalence is the author's contract to the user.
- Whether the ISA's actual hardware honors the declared categories (the silicon matches the ISA manual). This is the deepest floor — even GCC trusts the ISA manual.

These are not gaps. They are the hardware-domain floor that exists for every memory-safe systems language at the asm boundary. The structural property that makes ZER's floor better than alternatives is the closed-vocabulary discipline: the floor has a fixed, finite shape, so review is bounded.

**The architectural distinction from SPARK, stated precisely:**

SPARK and ZER both have a hardware-consequence trust gap at the contract boundary between user declarations and silicon behavior. The gap exists by physics; no language eliminates it. The distinctions:

| Property | SPARK | ZER (Option E) |
|---|---|---|
| Contract language | Arbitrary predicates | Closed category vocabulary |
| Trust gap shape | Open (anything expressible can be wrong) | Closed (only "categories vs asm semantics" can be wrong) |
| Review surface | Open-ended logical task | Finite checklist |
| Contract count per program | Unbounded (per-call) | Bounded (per-operation-per-ISA, frozen-after-review) |
| Audit visibility | Contracts in spec files, sometimes separated from use | Intrinsic name at use site; binding at well-known library location |
| Trust factoring | Contract author per call site | Binding author per (operation, ISA) |
| Hardware floor | Real and per-call | Real and per-(operation, ISA) — smaller, frozen |

ZER is stronger than SPARK on the contract-language axis (closed vs open) and on the bounded-vs-unbounded contract-count axis. ZER is at the same floor as SPARK on the underlying physics (contracts can be wrong about silicon; no language can verify the silicon). Both are honest; ZER's is smaller and more structurally bounded.

### 1.7.13. Cross-reference to firmware safety vocabulary

The same program-consequence vs hardware-consequence vocabulary applies uniformly across ZER's safety domains. The locked statements:

**For firmware (from `firmware_safety_extensions.md` §1):**

> Once a value crosses into ZER through a typed boundary (intrinsic return, MMIO read, cinclude function, linker symbol, asm output, source literal), it is program data and ZER verifies every program-level operation on it. Hardware-consequence — peripheral side effects, datasheet-specific value correctness, silicon behavior — is floor, out of scope for any language.

**For asm (Option E, this section):**

> Every call site of every Layer 1 operation in Layer 3 firmware code is verified against the operation's declared categories. Hardware-consequence — whether the asm body inside a `@bind` declaration actually has the categories declared, whether the ISA semantics match the binding's claims, whether the silicon honors the ISA manual — is floor, out of scope for any language.

The two statements are the same structural commitment applied to different boundaries. Firmware's boundary is at value-entry through typed declarations (mmio, cinclude, linker symbols). Asm's boundary is at the `@bind` declaration site. In both cases, ZER owns program-consequence at 100% and names hardware-consequence as the floor.

The structure/semantics straddle observation (§1.7.5) is the reason asm uniquely requires the binding mechanism while firmware's other primitives do not — but the resulting safety story has the same shape: program-consequence total, hardware-consequence floor at the declaration site.

### 1.7.14. Anti-patterns — drifts to refuse

The session-level drift patterns documented in `firmware_safety_extensions.md` §23 apply to asm work too. Specifically, the following drifts must be refused regardless of how sympathetically they are framed:

**Anti-pattern 1: Collapsing program-consequence and hardware-consequence under one word.**

The word "consequence" must never carry both meanings in the same claim. If you find yourself writing "ZER catches every consequence at 100%," check whether you mean program-consequence (correct, locked) or hardware-consequence (false, floor). The split must be explicit. The discipline from `firmware_safety_extensions.md` §1 applies: program-consequence is caught, hardware-consequence is floor, and "consequence" by itself is ambiguous and forbidden in external claims.

**Anti-pattern 2: Opening Layer 2's contract language.**

The closed-vocabulary kind-difference is the load-bearing distinction from SPARK. The moment Layer 2 is allowed to carry free-form `requires:` predicates, free-form `effect:` clauses, or any other open-shape declarations, the kind-difference dissolves. Refuse this regardless of the use case argument. If a binding needs something the closed vocabulary cannot express, the fix is a Layer 1 schema extension via release, not a Layer 2 escape hatch.

**Anti-pattern 3: Committing ZER core to a favored ISA.**

The "ship x86 reference in core" temptation reappears under variants: "just for bootstrap," "as a baseline implementation," "as the default fallback." Each variant smuggles the favored-ISA commitment back into Layer 1. The structure/semantics straddle (§1.7.5) is the reason this is wrong: ZER core cannot know what an x86 instruction means without committing to x86's semantics, which makes ZER an "x86 language wearing a portable costume." The packaging decision (whether ZER team maintains a canonical x86 Layer 2 library separately) is independent and can be made empirically; the Layer 1 commitment must stay ISA-less.

**Anti-pattern 4: Letting raw asm in naked become a "regular" path.**

Raw asm in naked functions remains the escape hatch for the irreducible ~0.1% (boot stubs, hand-tuned crypto sequences, pre-runtime code). It must not be treated as a Layer 4 or as a normal alternative to Layer 2 bindings. Every operation expressible as a Layer 1 semantic operation must go through Layers 1-2. Raw asm in naked is the bounded floor, marked by mandatory `safety:` annotation, surfaced for review, and structurally separate from the layered safety story.

**Anti-pattern 5: Verifying asm strings against contracts.**

The temptation reappears: "what if we verify the asm body matches the declared categories?" This requires per-instruction semantic knowledge across all ISAs — which is exactly the unbounded catalog Level C rejected. The closed-vocabulary discipline accepts that asm body correctness is the floor; trying to verify it brings back the maintenance hell Levels C and D were designed to avoid.

**Anti-pattern 6: Free-form `requires:` predicates "just for this one case."**

The "just one case" framing always smuggles back the open-vocabulary surface. Refuse. The closed vocabulary is the architecture; opening it for any case opens it structurally. If a real use case demands a precondition the closed vocabulary cannot express, the fix is a Layer 1 schema extension — frozen, reviewed, released. Never a per-binding freedom.

**Anti-pattern 7: Treating "blast radius worse than Rust unsafe" as the differentiator.**

The Level D analysis (§1.6.7) honestly named that wrong contracts in `@intrinsic_def` poison the verifier's reasoning about callers — a worse blast radius than Rust's `unsafe` block. This remains true under Option E. The differentiator from Rust is NOT "smaller blast radius" — it is the **closed-vocabulary checklist-reviewable trust gap** (§1.7.6). Do not let "smaller blast radius" become the public claim; it is false. The honest claim is the closed-vocabulary kind-difference, paired with the demand/promise asymmetry rule from §1.6.17 as the blast-radius mitigation.

**Anti-pattern 8: Inventing time estimates or coverage percentages.**

The drift pattern documented in `firmware_safety_extensions.md` §23 applies here: time estimates ("3 weeks for Gap X") and coverage percentages ("30-40% of asm code surface") generated under pressure to produce concrete planning are not derived from measurement. They feel concrete but they are hallucinations styled as planning. Refuse. The work is real and bounded by the schema design; sequencing matters but durations are speculative.

### 1.7.15. Anti-patterns specific to Layer 2 binding authorship

In addition to the architectural anti-patterns above, Layer 2 binding authors should refuse:

**Layer 2 anti-pattern A: Over-broad categories.**

Declaring `clobbers_flags` on every binding "just to be safe" is over-restriction, which causes false-rejection at call sites that don't actually clobber flags. The demand/promise asymmetry (§1.6.17) means over-restriction is sound but annoying. The discipline: declare the categories the binding actually exhibits, no more.

**Layer 2 anti-pattern B: Under-broad categories.**

The dual error: omitting `clobbers_flags` on a binding that actually does clobber flags. This is under-restriction, which causes silent unsafety at call sites that rely on flag preservation. This is the SPARK-style trust gap exactly at the wrong-contract failure mode. The discipline: cross-check declared categories against the asm body via the closed checklist; review is the mitigation.

**Layer 2 anti-pattern C: Cross-ISA "equivalent enough" bindings.**

When binding the same Layer 1 operation across multiple ISAs, the bindings must be semantically equivalent. "x86 ADC and ARM ADCS are close enough" is not equivalent if their corner cases differ (different flag-update conditions, different operand-size constraints). The discipline: each binding honors its declared Layer 1 contract; if the ISAs cannot deliver equivalent semantics, the binding is invalid and the operation is unbindable on that ISA (mark missing-arch per §1.6.18).

**Layer 2 anti-pattern D: Composing blessed primitives in user libraries to fake `@core::*` status.**

If a user library wraps a Layer 1 `@core::*` operation and re-exports it under a `@<lib>::*` name, the composed library's call site loses the Layer 1's blessed soundness level (it drops to Layer 2 conditional soundness). The discipline: composition is allowed but the soundness level propagates downward, not upward.

**Layer 2 anti-pattern E: Implicit ISA assumptions in operation signatures.**

Operation signatures should not encode ISA-specific assumptions (e.g., register names, word widths beyond the typed `T`). Layer 1's job is to keep the operation taxonomy ISA-neutral; Layer 2 bindings handle the per-ISA realization. If an operation signature contains `register: u64` referring to an x86 register class, that is a Layer 1 design bug, not a Layer 2 binding issue.

### 1.7.16. Comparison with prior architectures (Level C, Level D, Option C)

For completeness, here is how Option E differs from each of the architectures considered earlier in this document.

| Property | Level C (1.5) | Level D (1.6) | Option C (rejected) | **Option E (locked)** |
|---|---|---|---|---|
| Per-instruction database | Deleted (defer to GCC) | Deleted (defer to GCC) | Deleted (defer to GCC) | **Deleted (defer to GCC)** |
| Register tables | Deleted | Deleted | Deleted | **Deleted** |
| CPU feature enum | Deleted | Deleted | Deleted | **Deleted** |
| Catalog of 130 intrinsics in core | Yes (frozen, ZER-team-owned) | Yes (blessed @core::*, ZER-team-authored per-ISA dispatch) | Yes (x86 reference shipped in core) | **No — operation taxonomy lives in core, per-ISA bindings live in Layer 2 libraries** |
| User-extensible intrinsics | No | Yes (@intrinsic_def, @<lib>::*) | Yes (community per-ISA libraries) | **Yes (Layer 2 @bind libraries)** |
| Favored ISA in language core | None explicit (but 130 intrinsics have x86_64 dispatch in core) | None explicit (but @core::* intrinsics have per-ISA dispatch in core) | x86 explicit | **None — Layer 1 is ISA-less by construction** |
| Verifier reasons over | Categories (Z-rules + safety class registry) | Categories (Z-rules + safety class registry + intrinsic_def contracts) | Categories | **Categories (closed vocabulary, no free-form predicates)** |
| Contract language | None at user level | Structured (@intrinsic_def with requires/safety_class/effect) | Structured | **Structured + closed vocabulary discipline (no free-form predicates allowed)** |
| Layering | 2-layer (Layer 1 intrinsics + Layer 2 raw asm in naked) | 2-layer (Layer 1 @core::* + Layer 2 @<lib>::*) + escape hatch | 2-layer with x86 reference + community per-ISA | **3-layer (Layer 1 schema/taxonomy/mechanism + Layer 2 per-ISA bindings + Layer 3 firmware) + escape hatch for ~0.1%** |
| Program-consequence vocabulary | Implicit | Implicit | Implicit | **Explicit and locked — same vocabulary as firmware_safety_extensions.md** |
| Hardware-consequence floor location | Per-intrinsic catalog entry (ZER-team-owned) | Per-intrinsic_def declaration site (ZER team for @core::*, library author for @<lib>::*) | Per-binding declaration site, with x86 reference owned by ZER team | **Per-@bind declaration site, no ISA owned by ZER team in core (governance decision separable)** |
| SPARK comparison | Bounded catalog, not contract-bearing for user code | Contract-bearing for user-defined intrinsics; trust gap surfaced | Contract-bearing; trust gap surfaced with x86 reference as baseline | **Contract-bearing; closed-vocabulary kind-difference elevated to load-bearing distinction** |
| Structure/semantics straddle observation | Implicit | Implicit | Implicit | **Explicit — named as the architectural reason E is forced** |

Option E preserves Level D's `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18). It refines the layering, makes the ISA-less commitment explicit in Layer 1, elevates the closed-vocabulary discipline to load-bearing thesis, and applies the program-consequence vocabulary uniformly.

### 1.7.17. Implementation work order under Option E

The implementation sequence, with dependencies but **no time estimates** (those were drift in earlier drafts):

1. **Level C cleanup first.** Per the existing plan in this document (Section 16), delete per-instruction database, register tables, CPU feature enum, probe scripts. This is the foundation for both Level D and Option E.

2. **Level D mechanism on top of Level C.** Implement `@intrinsic_def` parser + checker + verifier dispatch + namespace convention + demand/promise asymmetry rule + missing-arch propagation rule. Per the work order in §1.6.13. This ships the mechanism Option E uses.

3. **Option E factoring during Level D implementation.** The 130 existing intrinsics, instead of being declared as `@core::*` with per-ISA dispatch in core, are split into:
   - Layer 1 operation declarations (in core, taxonomy only — no per-ISA asm).
   - A separately-maintained Layer 2 binding library (`zer-asm-x86`) authored by ZER team but structurally peer to community libraries. This library provides the per-ISA bindings for the existing intrinsic functionality.
4. **Schema design.** Lock the closed category vocabulary. This is the real design work — see §1.7.7 for the discipline. The vocabulary must be expressive enough to handle structurally different machine models without escape into free-form predicates.

5. **Operation taxonomy design.** Lock the set of semantic operations Layer 1 ships. Factor operations by semantic equivalence across ISAs (e.g., `@arith_add_wrap` vs `@arith_add_with_carry` are separate operations because they have different category profiles even on the same ISA).

6. **Reference Layer 2 libraries.** Author at minimum:
   - `zer-asm-x86` (ZER-team-maintained, separately versioned).
   - A second Layer 2 library for ARM Cortex-M or RISC-V to validate the schema across structurally different machine models.

7. **Mechanical confirmation.** Execute the test in §1.7.11: write one binding by hand, write one Layer 3 call site, compile for both targets, verify Layer 3 stays ergonomically normal and the schema is expressive enough.

8. **Documentation.** Update `docs/reference.md`, `CLAUDE.md`, and `firmware_safety_extensions.md` with cross-references to this section. Ship the public claim using the program-consequence vocabulary.

**No time estimates.** When work lands, replace this sequencing with completion records, not projected dates.

**Dependencies summarized:**

```
Level C cleanup
    └─> Level D mechanism (parser/checker/verifier)
            ├─> Schema design (category vocabulary, operation taxonomy)
            │       └─> Reference Layer 2 libraries
            │               └─> Mechanical confirmation
            │                       └─> Documentation update
            └─> Demand/promise asymmetry rule + missing-arch propagation
```

### 1.7.18. The locked decisions, enumerated

For future-session continuity (so the decisions are not re-litigated), the locked architectural commitments under Option E:

1. **Program-consequence vs hardware-consequence vocabulary is locked.** The word "consequence" must never carry both meanings in external claims. Program-consequence = caught at 100%. Hardware-consequence = floor.

2. **Layer 1 (ZER core) ships the schema + operation taxonomy + mechanism. No per-ISA asm in core.** The ISA-less commitment is structural; refuse "ship x86 reference in core" framings.

3. **Layer 2 (per-ISA binding libraries) carries per-ISA asm bodies.** Authored once per (operation, ISA), reviewed against the closed checklist, frozen-after-review. Structurally peer libraries; whether ZER team maintains a canonical x86 binding library is a separable governance decision.

4. **Layer 3 (firmware/application) imports a Layer 2 library and calls operations as typed functions.** Same ergonomics as any ZER library. Asm is invisible. No upfront declarations.

5. **The structure/semantics straddle is the architectural reason this layering is forced.** Asm uniquely straddles program-domain (structure) and hardware-domain (semantics) within a single token. Any architecture that does not respect this straddle either commits to one ISA or abandons asm safety.

6. **The closed verifier-native vocabulary is the load-bearing kind-difference from SPARK.** Not "smaller surface" (degree) — different failure topology (kind). SPARK contracts open-shaped; ZER contracts closed-shaped; review is bounded by the schema.

7. **The contract language must stay closed.** No free-form `requires:` predicates. No free-form `effect:` clauses. No user-extensible category vocabulary in Layer 2. Extensions go through Layer 1 release with review. The closed-vocabulary discipline is the whole differentiator; opening any escape hatch dissolves the kind-difference.

8. **The packaging question (who maintains canonical Layer 2 libraries) is separable from the safety architecture.** Decide empirically via the artifact, not by reasoning. Reversible decision.

9. **Level D's `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18) survive unchanged.** Option E refines layering and ISA-reference, not the mechanism.

10. **GCC continues to handle Z-level concerns** (register validity, instruction validity, CPU feature gating) per Level C.

11. **Raw asm in naked is the escape hatch for the irreducible ~0.1%** (boot stubs, hand-tuned crypto, pre-runtime code). Structurally outside the three-layer model. Mandatory `safety:` annotation.

12. **Time estimates and coverage percentages are not produced.** Sequencing of work is real; durations are speculative and refused per the discipline locked in `firmware_safety_extensions.md` §23.

13. **The mechanical confirmation test (§1.7.11) is the only confirmation that does not drift.** Architecture review ratifies framings; only the artifact (write one binding) confirms the layering.

These commitments are locked in 2026-06-06. Future sessions reading this section should treat them as architectural facts, not as open design questions.

### 1.7.19. The honest public claim, locked

The defensible public claim for ZER's asm safety, stated with the locked vocabulary:

> ZER's asm safety architecture provides 100% program-consequence coverage: every call site of every semantic operation in ZER source code is verified at compile time against the operation's declared category profile. The verification dispatches to ZER's existing structural analyses (Z-rules, escape analysis, provenance preservation, qualifier preservation, alignment, ordering, context propagation) and reasons only over the closed category vocabulary. This holds for every Layer 3 firmware call site, regardless of which Layer 2 per-ISA binding library compiles, regardless of whether the binding's declared categories actually match the asm semantics on the target ISA. Program-consequence is total over the call-site axis.
>
> Hardware-consequence — whether the asm body inside a `@bind` declaration actually has the categories declared, whether the ISA semantics match the binding's claims, whether the silicon honors the ISA manual — is the floor, surfaced at the `@bind` declaration site, reviewable as a finite checklist because the contract language is closed. The library author owns binding-correctness; the closed-vocabulary discipline ensures the review surface is bounded.
>
> The architecture rests on three structural commitments: (1) ZER core ships the closed category vocabulary + the semantic operation taxonomy + the `@intrinsic_def` / `@bind` mechanism, with **zero per-ISA bindings in language core** — the operation set is ISA-less by construction. (2) Per-ISA bindings live in Layer 2 libraries authored against the closed schema; libraries are structurally peer to each other, with packaging of canonical libraries as a separable governance decision. (3) Layer 3 firmware/application code imports a Layer 2 binding library and calls operations as typed functions, never seeing asm, never touching the schema, with same ergonomics as importing any other ZER library.
>
> The architectural distinction from contract-based verification approaches (SPARK, ACSL, Vale) is the **closed verifier-native vocabulary**: where SPARK's contract language is unbounded predicates that can be wrong in arbitrary ways, ZER's `@bind` declarations select from a closed, ZER-owned category vocabulary that the verifier already knows how to reason about. The trust gap is real and bounded — not absent — but its failure topology is fundamentally different: a fixed, finite shape that allows mechanical checklist review against the schema, rather than open-ended proof obligations against arbitrary predicates. SPARK's gap is open-shaped; ZER's gap is closed-shaped. Same existence; different reviewability.
>
> The structure/semantics straddle observation explains why asm uniquely requires this binding mechanism while every other ZER construct sits cleanly on one side of the program-domain / hardware-domain boundary: an asm instruction simultaneously carries program-domain structure (operand connections, dataflow, clobber accounting) and hardware-domain semantics (instruction meaning, per-ISA). The two are fused in a single token; the language core cannot import the semantics without committing to an ISA. The three-layer model with no favored ISA in core is the unique factoring that respects this straddle without either committing the language to one ISA or abandoning asm meaning verification.
>
> The escape hatch — raw asm in naked functions for the irreducible ~0.1% (boot stubs, hand-tuned crypto, pre-runtime code) — remains structurally outside the three-layer model, marked by mandatory `safety:` annotation, surfaced for review at the `naked fn` boundary. This is the bounded floor that every memory-safe systems language has at the asm boundary; ZER's contribution is the bounded structural shape of the trust gap, not its absence.

This claim survives reviewer attack because:
- "100% program-consequence" is literal and verifiable by inspection of the verifier dispatch (it reasons over categories, never over asm strings).
- "Hardware-consequence floor" is named and bounded by the closed schema.
- The closed-vocabulary distinction from SPARK is structurally checkable (the contract language is not Turing-complete; the predicate set is finite).
- The three-layer architecture is locked by construction; future sessions cannot drift to "ship x86 reference in core" without breaking the locked commitment in §1.7.18 #2.
- The structure/semantics straddle is a load-bearing architectural fact, not a marketing framing.
- No "100% safe" overclaim. No time estimates. No coverage percentages. No invented numbers.

### 1.7.20. Glossary — terms locked for future-session continuity

For continuity across fresh sessions, the following terms are locked with the meanings stated:

| Term | Locked meaning |
|---|---|
| **Program-consequence** | What happens when a value is used wrongly inside ZER source code. Caught at 100% because the use is in the program. See `firmware_safety_extensions.md` §1 and §22 for the parallel firmware statement. |
| **Hardware-consequence** | What happens when a hardware fact is wrong relative to user belief (silicon doesn't honor declared region, asm body doesn't honor declared categories, board's crystal doesn't deliver declared frequency). Floor. The criterion lives outside the program. |
| **Layer 1** | ZER core: closed category vocabulary + semantic operation taxonomy + `@intrinsic_def` / `@bind` mechanism + verifier dispatch. **No per-ISA bindings.** Frozen and extended only via ZER core release with review. |
| **Layer 2** | Per-ISA binding library: `@bind` declarations mapping Layer 1 operations to per-ISA asm bodies, selecting categories from Layer 1's closed vocabulary. Authored once per (operation, ISA), reviewed against the closed checklist, frozen-after-review. Structurally peer to other Layer 2 libraries. |
| **Layer 3** | Firmware / application code: imports a Layer 2 library, declares firmware-side things (mmio, linker symbols, vector table per `firmware_safety_extensions.md`), calls Layer 1 operations as typed functions. Ergonomically normal — same workflow as importing any ZER library. |
| **Escape hatch** | Raw asm in naked functions for the irreducible ~0.1% (boot stubs, hand-tuned crypto, pre-runtime code). Structurally outside Layers 1-3. Mandatory `safety:` annotation. Same conceptual category as cinclude and Rust's `unsafe { asm!() }`. |
| **Closed category vocabulary** | The fixed, finite set of structural-behavior categories that Layer 2 `@bind` declarations select from. ZER-owned, extended only via ZER core release. The load-bearing distinction from SPARK. |
| **Operation taxonomy** | The fixed, finite set of semantic operations Layer 1 declares (`@arith_add_with_carry`, `@load_acquire`, `@barrier_full`, etc.). ZER-owned, extended via ZER core release. Each operation has a fixed category profile that applies across all ISA bindings. |
| **Structure/semantics straddle** | The architectural observation that an asm instruction uniquely carries both program-domain structure (operand connections, dataflow) and hardware-domain semantics (instruction meaning per ISA) within a single token. This straddle is the reason asm requires the binding mechanism while every other ZER construct sits cleanly on one side of the boundary. |
| **Closed-vocabulary kind-difference** | The load-bearing distinction from SPARK: ZER's contract language is closed (selection from finite vocabulary), so the trust gap has a fixed shape and review is a finite checklist. SPARK's contract language is open (arbitrary predicates), so the trust gap has open shape and review is an open-ended proof obligation. Same trust-gap existence; different failure topology. |
| **N/A on this ISA** | The disposition some categories take on ISAs that structurally lack the property (e.g., `clobbers_flags` is N/A on RISC-V because no flag register exists). The schema handles this directly; bindings can mark categories N/A without requiring per-ISA category vocabulary variation. |
| **Demand** | A declaration in a `@bind` that causes the verifier to do MORE checking on callers. Free — over-restriction at worst, never under-protection. See §1.6.17. |
| **Promise** | A declaration in a `@bind` that causes the verifier to do LESS checking on callers. Must be either verifier-checkable or explicitly taint the caller's soundness level. See §1.6.17. |
| **Missing-arch propagation** | The rule that a call to an operation with no Layer 2 binding for the current compile target is a compile error at the call site. Supported-archs is part of the operation's contract; callers' portability is bounded by the intersection of all transitively-called operations' supported-arch sets. See §1.6.18. |
| **Mechanical confirmation test** | Writing one Layer 2 `@bind` by hand against the schema and compiling one Layer 3 call site, to validate the layering actually delivers ergonomic Layer 3 and a closed-vocabulary-expressible Layer 2 binding. See §1.7.11. |

These glossary entries are the canonical reference for future sessions. If a future framing uses any of these terms with a different meaning, treat as drift and re-derive against this section.

### 1.7.21. Closing — the architectural commitments preserved across the asm thread

Across the asm safety thread spanning Levels A, B, C, D, and the Option C-vs-D-vs-E exploration, the following commitments have remained invariant and are preserved under Option E:

1. **Grammar-level closure for the language as a whole.** No in-language `unsafe` keyword. The only escape paths are the explicit cross-language boundaries (cinclude, naked asm, `@bind` for asm), each grep-able and marked. See `CLAUDE.md` "ZER's Goal" section and the Anders et al. 2024 "infinite unsafe impedance" citation.

2. **GCC delegates Z-level concerns.** Register validity, instruction validity, CPU feature gating belong to the user's C compiler. Locked at Level C.

3. **Definition A scoping discipline.** ZER verifies access mechanism structurally; semantic correctness against external substrates (hardware spec, vendor HAL, linker script, ISA manual) is user/library responsibility supplied through declaration sites with audit visibility.

4. **No per-instruction database for semantic verification.** Verifying "this asm does what the contract says" requires per-instruction semantic knowledge across all ISAs — exactly the unbounded catalog Level C rejected. The closed-vocabulary discipline accepts this as the floor.

5. **Sole-developer-sustainable maintenance burden.** ZER team's ongoing work is bounded: schema design once (rare extensions per several years), operation taxonomy design once (rare extensions per several years), mechanism shipped once. No per-ISA catalog growth on ZER team's desk.

6. **Architecture-agnostic positioning.** ZER works on every architecture GCC supports, with no per-ISA commitment in the language core. Per-ISA libraries fill the binding layer; user firmware code is portable across ISAs that have Layer 2 binding coverage for the operations used.

7. **Honest scoping of safety claims.** No "100% safe" without qualifier. No collapsing of program-consequence and hardware-consequence. No invented numbers. The trust gap is named and bounded.

These are the load-bearing commitments. Option E is the architectural shape that preserves all of them while resolving the layering, ISA-reference, and SPARK-distinction questions that remained open through Level D. The work to ship is the implementation work order in §1.7.17, with the mechanical confirmation test in §1.7.11 as the artifact-decides discipline applied at the right granularity.

The architecture is locked. The work is bounded. The discipline is documented.

### 1.7.22. Schema design rationale — the closed category vocabulary in depth

The closed category vocabulary is the load-bearing artifact in Option E. Its design discipline determines whether the architecture stays structurally distinct from SPARK or collapses into "tidier SPARK." This subsection captures the design principles for the category vocabulary so future schema-extension proposals can be evaluated against the locked discipline.

**Design principle 1: Categories describe structural behavior kinds, not instruction properties.**

A category like `clobbers_flags` describes "this operation, when executed, may invalidate the contents of the flag register class." It does not describe "this specific x86 instruction sets ZF based on result." The category is ISA-neutral; the binding's asm body realizes the category on the specific ISA. A category named after a specific instruction (e.g., `is_xmm_movaps`) is a design bug — it would tie Layer 1 to a specific ISA's nomenclature.

The correct shape:
- ✓ `clobbers_flags`, `reads_mem(width, ordering)`, `requires_aligned(operand, n)`
- ✗ `is_x86_cmpxchg`, `arm_load_exclusive`, `riscv_lr_sc_pair`

**Design principle 2: Categories must be verifier-native.**

Each category dispatches to an existing safety analysis. No category that requires a brand-new compiler analysis. The available analyses:

- Type system (operand type checking)
- Z-rules (asm operand boundary checks: Z1 handle state, Z2 move tracking, Z3 VRP, Z4 provenance, Z5 escape, Z6 context, Z7 MMIO, Z8 qualifier, Z11 non-storable, Z12 keep)
- VRP (value range propagation)
- Alignment tracking (in pointer types and access patterns)
- Provenance preservation (across casts, pointer arithmetic, function call boundaries)
- Escape analysis (whether pointers escape locals, asm clobbers, calls)
- Qualifier preservation (volatile, const)
- Context flag propagation (naked, ISR, critical, atomic)
- Bounds checking (slice access, runtime auto-guard)
- Allocator coloring (Pool vs Slab vs Arena vs Handle)

A proposed category that does not dispatch to one of these analyses fails the verifier-native requirement. The fix is either:
- Reformulate the category to dispatch to an existing analysis
- Reject the category (it doesn't belong in Layer 1)
- Add a new safety analysis (per-decade event, very rare, requires substantial design)

**Design principle 3: Categories support "N/A on this ISA" without category vocabulary variation.**

Different machine models have structurally different properties. RISC-V has no flag register. CHERI has capability-tagged pointers. Some ISAs have hardware atomic primitives others lack. The schema must accommodate this without adding per-ISA category vocabulary.

The N/A disposition mechanism:

```
@bind operation_x isa: riscv64 {
    asm { ... }
    isa_disposition: {
        clobbers_flags: N/A    // RISC-V has no flag register
    }
}
```

The verifier handles N/A categories by skipping the check on this binding (the category doesn't apply because the ISA doesn't have the property the category describes). This is structural, not a vocabulary extension.

What N/A is NOT:
- N/A is not "this category doesn't apply because the binding doesn't exhibit it." That would be "category not declared," not N/A. A binding that doesn't clobber flags simply doesn't declare `clobbers_flags`.
- N/A is "this category cannot apply on this ISA because the ISA lacks the property the category describes." It is an ISA-level disposition, not a binding-level choice.

**Design principle 4: Categories must be finite-state, not arbitrary predicates.**

A category like `requires_in_range(operand, lo, hi)` is finite-state — it dispatches to VRP with concrete numeric bounds. A category like `requires_user_specified_predicate(P)` is open-shape — it would dispatch to an arbitrary predicate evaluator. The latter is forbidden by the closed-vocabulary discipline.

This means certain natural-feeling requirements cannot be expressed as categories:
- "This operation requires the input to be a prime number" — open-shape, rejected.
- "This operation requires the input to satisfy `P(x) && Q(x) || R(x)`" — open-shape, rejected.
- "This operation requires the input to be aligned to a non-power-of-two boundary" — depends on what "non-power-of-two boundary" means; if it's a fixed alignment value, expressible; if it's a runtime-computed condition, rejected.

The discipline: if the requirement cannot be expressed as a finite-state category, either find a finite-state proxy that approximates it (over-restriction is sound by the demand asymmetry) or reject the operation as un-binadable via the closed schema. Open-shape escape into predicates is forbidden.

**Design principle 5: Categories compose by intersection, not by union.**

A binding declares a set of categories that the asm exhibits. The verifier treats this as the intersection of all declared properties — every category must hold for the binding to be valid. Removing a category from a binding strictly weakens the contract; adding a category strictly strengthens it.

This composition rule has implications:
- A binding can be made more restrictive (add categories) without breaking callers.
- A binding cannot be made less restrictive (remove categories) without potentially breaking callers that relied on the missing category.
- Cross-binding equivalence requires identical category sets (modulo N/A dispositions for ISAs that lack the property).

**Design principle 6: Categories are versioned.**

Schema extensions (adding new categories) are versioned with the language release. A Layer 2 binding that uses a category introduced in version N requires compiler version >= N. The versioning is mechanical, not a per-binding declaration; it falls out of the schema-release process.

Removing categories from the schema is a breaking change. The discipline: rarely remove. If a category turns out to be misdesigned, prefer deprecation (mark deprecated, encourage migration to a better category, remove in a future major version) over immediate removal.

**Design principle 7: Categories enable safety, not optimization.**

Categories describe safety-relevant properties — what guarantees the verifier can rely on at call sites to maintain program-consequence. They do NOT describe optimization-relevant properties (e.g., "this operation is constant-time," "this operation can be vectorized"). Optimization properties belong to a different concern (the compiler's optimization pipeline) and would dilute the safety-focused vocabulary.

A proposed category that aims to enable an optimization rather than enforce a safety property fails this principle.

### 1.7.23. Operation taxonomy factoring — when to split, when to unify

The operation taxonomy design has its own discipline, distinct from the category vocabulary design. The key question: when should two superficially-similar operations be the same Layer 1 operation, and when should they be split?

**Factoring principle 1: Split by category profile, not by ISA spelling.**

Two operations should be separate Layer 1 entries if their category profiles differ structurally, even if some ISAs use the same instruction for both. Conversely, two operations should be the same Layer 1 entry if their category profiles are identical, even if different ISAs use different instructions.

The ADD/flags example:
- `@arith_add_wrap(a, b) -> T` — categories: `{no_memory_effect, no_flag_effect, value_in_value_out}`
- `@arith_add_with_carry(a, b, c_in) -> {result, c_out}` — categories: `{no_memory_effect, produces_carry, consumes_carry, value_in_value_out, clobbers_flags}` (with N/A on flagless ISAs)

These are different operations because their category profiles differ structurally (`clobbers_flags` and `produces_carry` are in one but not the other). On x86, the `add` instruction binds to `@arith_add_wrap` (no flag setting required by the operation, though x86 always sets them — see factoring principle 3 below) while `adc` binds to `@arith_add_with_carry`. On ARM Cortex-M, `ADD` binds to `@arith_add_wrap` and `ADCS` binds to `@arith_add_with_carry`. Same factoring principle across ISAs.

**Factoring principle 2: Unify by semantic equivalence across ISAs.**

If two operations differ only in ISA-specific spelling but have the same semantic contract, they are the same Layer 1 operation. The Layer 2 bindings handle the per-ISA realization. `@arith_xor(a, b)` is one Layer 1 operation with bindings to `eor` (ARM), `xor` (x86), `xor` (RISC-V), etc. — all the same Layer 1 operation because the semantics (bitwise xor of two operands) are identical and the category profile is identical.

**Factoring principle 3: ISA-specific side effects that the operation does not require are not part of the category profile.**

x86's plain `add` instruction always sets flags. But the operation `@arith_add_wrap(a, b) -> T` does not REQUIRE flags to be set — it requires only the wrap-around add semantic. So `clobbers_flags` is NOT in `@arith_add_wrap`'s category profile, even though the x86 binding will incidentally clobber flags.

The principle: the category profile describes what the operation REQUIRES of the binding, not what every binding incidentally produces. If the x86 binding happens to clobber flags as a side effect of using `add`, that is the x86 binding's responsibility to handle (via the asm operand constraints declaring clobbers), but it is not part of the operation's contract.

This means callers of `@arith_add_wrap` cannot assume flags are clobbered, even on x86. If they need post-operation flag state, they should use `@arith_add_with_carry` or a different operation whose category profile includes flag effects.

**Factoring principle 4: Width parameterization through type generics.**

Operations should be parameterized by operand type rather than having separate entries per width. `@arith_add_wrap<T>(a: T, b: T) -> T where T in {u8, u16, u32, u64}` is one operation parameterized by T, not four separate operations. Each Layer 2 binding can dispatch on T to choose the appropriate instruction (e.g., `add` for 32-bit, `add.w` for 16-bit, etc.).

This keeps the operation taxonomy manageable. Splitting into per-width operations would explode the catalog and obscure the semantic equivalence.

**Factoring principle 5: Memory ordering is part of the operation, not a category modifier.**

`@load_acquire`, `@load_relaxed`, `@store_release`, `@store_relaxed` are separate Layer 1 operations because their memory ordering is fundamental to their contract, and the verifier reasons about ordering at the operation-call level.

It would be tempting to have a single `@load<O>(ptr)` parameterized by ordering, but this creates verifier-dispatch complexity (the verifier would need to extract O from the call site to dispatch ordering-specific analyses). Separate operations are clearer.

**Factoring principle 6: Atomic primitives are separate operations from non-atomic equivalents.**

`@atomic_fetch_add(ptr, val)` is a separate operation from `@arith_add_wrap`. The atomic operation's category profile includes memory-ordering and atomicity categories that the non-atomic version doesn't have. Splitting these is essential for verifier dispatch — the verifier reasons about lock-freedom, ordering, and atomicity only at atomic-operation call sites.

**Factoring principle 7: Reject operations whose semantics cannot be expressed via existing categories.**

If a proposed operation requires a category that doesn't exist in the schema, the operation cannot enter the taxonomy until the schema is extended. The closed-vocabulary discipline applies to both axes: the category vocabulary is closed, and the operation taxonomy can only declare operations expressible in the existing schema.

If a real-world need keeps surfacing operations that the schema can't express, that is the signal for a Layer 1 schema-extension cycle. Both the category vocabulary and the operation taxonomy are extended through ZER core release with review.

### 1.7.24. Worked examples — more operations across machine models

To stress-test the schema design across structurally different machine models, here are additional worked examples covering memory ordering, atomic primitives, vector operations, and CPU-state intrinsics.

**Example 4 — Atomic compare-and-swap:**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def atomic_cas(ptr: *T, expected: T, new: T) -> {result: T, success: bool}
    where T in {u32, u64, usize}
    categories: {
        reads_mem(sizeof(T), seqcst),
        writes_mem(sizeof(T), seqcst),
        atomic_rmw,
        requires_aligned(ptr, alignof(T)),
        no_flag_effect_required,
        value_in_value_out
    }
    safety: "Atomic compare-and-swap with sequential consistency.
             If *ptr == expected, atomically updates *ptr to new
             and returns {result: expected, success: true}.
             Otherwise, returns {result: *ptr, success: false}
             with *ptr unchanged. Requires aligned ptr."

// Layer 2 (zer-asm-x86)
@bind atomic_cas isa: x86_64 {
    asm { instructions: "lock cmpxchg $2, ($0)\n"
                        "sete $3"
          inputs:  [ptr, expected => RAX, new]
          outputs: [result => RAX, success => $3]
          clobbers: ["flags"]
          safety:  "x86 LOCK CMPXCHG: atomic compare-and-swap.
                    Compares *ptr with RAX (expected); if equal,
                    writes new to *ptr and sets ZF. Otherwise loads
                    *ptr into RAX and clears ZF. SETE captures ZF
                    into success operand. Categories all hold;
                    LOCK prefix provides seqcst ordering on x86." }
}

// Layer 2 (zer-asm-arm-cortex-a)
@bind atomic_cas isa: arm_cortex_a {
    asm { instructions: "1: ldaxr $0, [$1]\n"
                        "   cmp $0, $2\n"
                        "   b.ne 2f\n"
                        "   stlxr $w3, $4, [$1]\n"
                        "   cbnz $w3, 1b\n"
                        "   mov $3, #1\n"
                        "   b 3f\n"
                        "2: clrex\n"
                        "   mov $3, #0\n"
                        "3:"
          inputs:  [ptr, expected, new]
          outputs: [result => $0, success => $3]
          clobbers: ["NZCV", "memory"]
          safety:  "ARMv8 LL/SC compare-and-swap via LDAXR/STLXR.
                    Acquire-release ordering for seqcst.
                    Loop on STLXR failure (race retry).
                    CLREX clears monitor on mismatch path.
                    Categories all hold including seqcst ordering." }
}

// Layer 2 (zer-asm-riscv)
@bind atomic_cas isa: riscv64 {
    asm { instructions: "1: lr.d.aq $0, ($1)\n"
                        "   bne $0, $2, 2f\n"
                        "   sc.d.rl $w3, $4, ($1)\n"
                        "   bnez $w3, 1b\n"
                        "   li $3, 1\n"
                        "   j 3f\n"
                        "2: li $3, 0\n"
                        "3:"
          inputs:  [ptr, expected, new]
          outputs: [result => $0, success => $3]
          clobbers: ["memory"]
          safety:  "RISC-V LR/SC compare-and-swap with .aq/.rl
                    for seqcst ordering. Loop on SC failure.
                    Categories all hold; ordering acquired via
                    LR.aq + SC.rl combination." }
}
```

The three bindings have substantially different sizes — x86 is two instructions, ARM and RISC-V are LL/SC loops. All three honor the same Layer 1 category profile. Layer 3 firmware code calling `@atomic_cas(...)` is identical across all three targets.

**Example 5 — CPU privileged-mode intrinsics:**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def cpu_disable_int()
    categories: {
        no_memory_effect,
        changes_context(disable_interrupts),
        requires_privilege(kernel),
        no_flag_effect_required
    }
    safety: "Disable interrupts. Must be called from privileged
             context. Acquires implicit critical-section semantics
             from disable point to corresponding enable."

@intrinsic_def cpu_enable_int()
    categories: {
        no_memory_effect,
        changes_context(enable_interrupts),
        requires_privilege(kernel),
        no_flag_effect_required
    }
    safety: "Enable interrupts. Must be called from privileged
             context. Releases implicit critical-section."

// Layer 2 (zer-asm-x86)
@bind cpu_disable_int isa: x86_64 {
    asm { instructions: "cli"
          inputs: []
          outputs: []
          clobbers: ["flags"]    // CLI clears IF in EFLAGS
          safety: "x86 CLI: clear interrupt flag. Requires CPL <= IOPL.
                   Categories all hold; clobbers flags via EFLAGS update." }
}

@bind cpu_enable_int isa: x86_64 {
    asm { instructions: "sti"
          inputs: []
          outputs: []
          clobbers: ["flags"]
          safety: "x86 STI: set interrupt flag. Requires CPL <= IOPL." }
}

// Layer 2 (zer-asm-arm-cortex-a)
@bind cpu_disable_int isa: arm_cortex_a {
    asm { instructions: "msr daifset, #0x2"
          inputs: []
          outputs: []
          clobbers: []
          safety: "ARMv8 MSR DAIFSET #0x2: set I bit, disabling IRQ.
                   Requires EL >= 1." }
}

@bind cpu_enable_int isa: arm_cortex_a {
    asm { instructions: "msr daifclr, #0x2"
          inputs: []
          outputs: []
          clobbers: []
          safety: "ARMv8 MSR DAIFCLR #0x2: clear I bit, enabling IRQ.
                   Requires EL >= 1." }
}
```

**Example 6 — Vector load (illustrating type parameterization across vector widths):**

```
// Layer 1 (ZER core, frozen)
@intrinsic_def vector_load_aligned(ptr: *V) -> V
    where V in {v128_u8, v128_u16, v128_u32, v256_u8, v256_u16, v256_u32, v512_u8, ...}
    categories: {
        reads_mem(sizeof(V), relaxed),
        requires_aligned(ptr, alignof(V)),
        no_flag_effect,
        value_in_value_out
    }
    safety: "Aligned vector load. Reads sizeof(V) bytes from ptr
             as a vector value. Requires alignment to alignof(V).
             Relaxed ordering — for ordered loads, use vector_load_acquire."

// Layer 2 (zer-asm-x86) — covers SSE/AVX/AVX-512 via type dispatch
@bind vector_load_aligned isa: x86_64 dispatch_on_type {
    V == v128: {
        asm { instructions: "movaps ($1), $0"
              inputs: [ptr]
              outputs: [result => $0]
              clobbers: []
              safety: "x86 MOVAPS: aligned 128-bit load into XMM register.
                       Requires 16-byte alignment." }
    }
    V == v256: {
        asm { instructions: "vmovaps ($1), $0"
              inputs: [ptr]
              outputs: [result => $0]
              clobbers: []
              safety: "x86 VMOVAPS: aligned 256-bit load into YMM.
                       Requires 32-byte alignment; AVX." }
    }
    V == v512: {
        asm { instructions: "vmovaps ($1), $0"
              inputs: [ptr]
              outputs: [result => $0]
              clobbers: []
              safety: "x86 VMOVAPS (EVEX): aligned 512-bit load into ZMM.
                       Requires 64-byte alignment; AVX-512." }
    }
}
```

The `dispatch_on_type` clause in the `@bind` declaration allows one binding declaration to handle multiple type instantiations of the same operation. Each branch within the dispatch independently honors the operation's category profile. This keeps Layer 2 declarations manageable for type-parameterized operations.

These examples are intentionally illustrative and incomplete; the full operation taxonomy will be designed at schema-design time. The point is to validate that the layering accommodates structurally different machine models (flag-bearing vs flagless, weak vs strong memory, LL/SC vs CAS, scalar vs vector) without escape into open-shape predicates.

### 1.7.25. The full coverage matrix under Option E

For comparison with the existing coverage matrix in Section 13, here is the coverage matrix as it stands under Option E.

| Concern | Where verified | Verification mechanism | Floor location |
|---|---|---|---|
| Operand types at asm boundary | Layer 1 verifier | Existing type checker dispatched per category | None |
| Asm-output value escape | Layer 1 verifier | Z5 escape analysis dispatched on category | None |
| Asm-output provenance | Layer 1 verifier | Z4 provenance clearing dispatched on category | None |
| Volatile preservation across asm | Layer 1 verifier | Z8 qualifier preservation dispatched on category | None |
| MMIO range / alignment | Layer 1 verifier | Z7 MMIO dispatched on category | None |
| Memory ordering at use sites | Layer 1 verifier | Ordering categories dispatched to ordering analysis | None |
| Atomicity at use sites | Layer 1 verifier | Atomic categories dispatched to RMW analysis | None |
| Context (ISR / critical / naked) propagation | Layer 1 verifier | Context flags propagated through call graph | None |
| Operand alignment requirement | Layer 1 verifier | Alignment category dispatched to existing alignment tracking | None |
| Operand range requirement | Layer 1 verifier | Range category dispatched to VRP | None |
| Operand non-null requirement | Layer 1 verifier | Non-null category dispatched to nullability tracking | None |
| Clobber accounting | Layer 1 verifier | Z3 / existing clobber tracking | None |
| Cross-binding semantic equivalence | Library author + reviewer | Closed-checklist review of `@bind` against Layer 1 categories | Layer 2 binding-correctness floor |
| Asm body matches declared categories | Library author + reviewer | Closed-checklist review of `@bind` against asm text | Layer 2 binding-correctness floor |
| ISA instruction validity | GCC assembler | GCC's existing instruction validity check | GCC error (not ZER) |
| Register name validity | GCC assembler | GCC's existing register validity check | GCC error (not ZER) |
| CPU feature gating (e.g., AVX-512 only on -mavx512f) | GCC assembler | GCC's existing CPU feature check via -m flags | GCC error (not ZER) |
| ISA spec correctness (silicon matches manual) | None | Outside any language's reach | Hardware floor |
| Cross-ISA binding equivalence | Library author | Author's contract to users | Layer 2 binding-author responsibility |
| Hardware-consequence of binding categories not matching silicon | None | Outside any language's reach | Hardware floor at `@bind` site |

The pattern: Layer 1 covers all program-consequence aspects of asm-bearing code. Layer 2 carries the binding-correctness floor at well-defined declaration sites. GCC handles Z-level concerns. The hardware floor exists at the binding-vs-silicon boundary and is named, bounded, and reviewable as a finite checklist.

### 1.7.26. Journey of the asm-thread architecture decisions

Future sessions reading this document will need to understand how the architecture reached Option E without re-litigating it. Here is the abbreviated journey across the asm thread:

**Phase 1 — Initial direction (`asm_plan.md`, pre-2026-05):** Build per-instruction safety knowledge into the compiler. Vendored instruction tables. Register-name validation. CPU feature gating. CFG-aware OrderingState pass. Y_intrinsic catalog ZER-team-authored and ZER-team-maintained.

**Phase 2 — Levels A/B/C decision (2026-05-12):** Aggressive cleanup. Delete per-instruction database, register tables, CPU feature enum, probe scripts. Defer everything ISA-specific to GCC. Keep frozen core (Z-rules, naked-only, ~12 UB classics) + 130 intrinsics. This is Level C; it shipped as the deletion baseline.

**Phase 3 — Two-layer crystallization (2026-05-12 evening):** Intent intrinsics (Layer 1) + raw asm in naked (Layer 2 / escape hatch). All "richer" designs (per-operand annotations, IR region wrappers, auto-guard emission) deferred indefinitely. The 2-layer model is the resting architecture for Level C.

**Phase 4 — Level D pivot (2026-05-31):** Externalize Y_intrinsic authorship via `@intrinsic_def`. Layer 1 becomes blessed `@core::*` intrinsics; Layer 2 becomes user-defined `@<lib>::*` intrinsics. Mechanism enables unbounded community growth without ZER team involvement. The seam (wrong contract at definition site) is named explicitly; mitigation via demand/promise asymmetry rule (§1.6.17) and missing-arch propagation rule (§1.6.18).

**Phase 5 — Program-consequence vocabulary lockdown (2026-06-05/06):** The "consequence" word collapses program-domain caught-set and hardware-domain floor under one number; this is identified as the load-bearing equivocation. The vocabulary is locked at `firmware_safety_extensions.md` and `CLAUDE.md`: program-consequence (caught at 100%) vs hardware-consequence (floor). The split is then applied to asm.

**Phase 6 — Three-layer / no-favored-ISA refinement (2026-06-06):** The Level D 2-layer model is refined to three layers. The key move is removing per-ISA asm from Layer 1 (language core) entirely. Layer 1 ships the schema + operation taxonomy + mechanism. Layer 2 ships per-ISA bindings (community libraries, structurally peer). Layer 3 is firmware/application code (import and call). This is Option E.

**Phase 7 — Structure/semantics straddle articulation (2026-06-06):** The architectural reason Option E is forced rather than chosen is identified: asm uniquely straddles program-domain (structure: operand connections, dataflow) and hardware-domain (semantics: instruction meaning per ISA) within a single token. Any architecture that does not respect this straddle either commits to one ISA or abandons asm safety. Option E is the unique factoring that respects the straddle.

**Phase 8 — Closed-vocabulary kind-difference elevation (2026-06-06):** The closed verifier-native category vocabulary is elevated from "one of four mitigations" to "the load-bearing kind-difference from SPARK." Trust-gap shape (open vs closed) is identified as a difference in failure topology, not just a smaller surface. The discipline that the contract language must stay closed (no free-form predicates) is locked.

**Phase 9 — Safety vs governance register separation (2026-06-06):** The question "who maintains canonical Layer 2 binding libraries (x86 in particular)" is identified as a packaging/governance question separable from the safety architecture. The safety architecture locks at Option E; the packaging question can be decided empirically via the mechanical confirmation test.

**Phase 10 — Documentation lock (2026-06-06):** This section is written to capture the locked architecture, the vocabulary, the journey, and the discipline so future sessions can re-engage without re-deriving. Cross-references to `firmware_safety_extensions.md`, `CLAUDE.md`, and the existing Section 1.6 (Level D) are added.

The total elapsed wall-time across phases 5-10 was a single intensive design session, with multi-session audit feedback from other Claude instances catching drift patterns at each step. The drift patterns documented in `firmware_safety_extensions.md` §23 apply here too; the locked discipline is the discipline that survives the audit.

### 1.7.27. What other Claude sessions caught — drift patterns specific to this thread

Several drift patterns were caught by other Claude sessions during the asm-architecture discussion. They are recorded here so future sessions can recognize and resist them.

**Drift A — "100% consequence coverage" applied without vocabulary split.**

The temptation appears every time the user articulates the desired safety property as "complete coverage at the use site." Without the program-consequence vs hardware-consequence split, "100% consequence coverage" reads as covering hardware behavior too, which is false. The audit caught this and forced the split into program-consequence (caught) vs hardware-consequence (floor). The discipline: never let "consequence" carry both meanings.

**Drift B — Decision velocity escalation through Options C → D → E.**

The architecture moved from "ship x86 reference in core (Option C)" to "schema only, no operations, no ISA (Option D)" to "schema + operation taxonomy, no ISA (Option E)" across three messages, with the model agreeing at each step. The audit identified this as drift — each option felt cleaner because the user leaned that way and the model generated a confident scaffold for each lean. The mitigation: artifact-confirms-not-reasoning. Write one binding by hand to validate the chosen architecture. The locked answer (Option E) survives this discipline because the structure/semantics straddle observation provides the forcing argument, not just preference.

**Drift C — Firmware analogy applied without checking the user-specific vs universal-constant distinction.**

The firmware analogy ("ZER ships `mmio` primitives, not STM32 chip addresses; therefore ZER ships the operation taxonomy, not x86 bindings") is structurally correct but has a sharp edge: firmware's delegated part (chip addresses) is user-specific; asm's delegated part (x86 `adc` binding) is universal. The audit identified that delegating a universal constant produces either fragmentation or de-facto-standardization-without-governance. The resolution: the safety architecture still locks at Option E (no per-ISA in language core); the governance question (whether ZER team maintains a canonical x86 Layer 2 library) is separable and can be decided at packaging time. See §1.7.8.

**Drift D — "User intention to make it right" framing applied to universal constants.**

The user's intuition "Option E is less burden because user intention makes it right" is correct for parts of Layer 2 that have genuine authorship content (RISC-V carry emulation, the substantive parts of complex bindings). It is misapplied to parts that are universal constants (x86 `adc` is `adc` for every x86 user — there's no intention to express, only a constant to transcribe). The audit caught this and forced the distinction: "user intention to make it right" is correct for substantive bindings, incorrect for universal constants which are better served by a maintained canonical library. The discipline: do not apply "user intention" uniformly to all bindings; honor the user-specific vs universal-constant split at the packaging layer.

**Drift E — Treating SPARK comparison via "smaller surface" instead of "closed vocabulary."**

The temptation to claim "ZER is better than SPARK because the trust gap is smaller (fewer contracts, more visible, per-author)" is real but mis-states the differentiator. Smaller is degree; SPARK could in principle adopt these mitigations and remain SPARK. The kind-difference is the closed verifier-native vocabulary — SPARK's gap is open-shaped (arbitrary predicates), ZER's gap is closed-shaped (finite category selection). Review is finite checklist vs open proof. The audit caught the "smaller surface" framing and forced elevation of the closed-vocabulary distinction to load-bearing thesis.

**Drift F — Free-form `requires:` predicates "just for this one case."**

The temptation to open Layer 2's contract language to express "this one binding's special requirement" is the move that collapses E back into SPARK. The audit caught this preemptively and the discipline is locked: closed vocabulary or the kind-difference dissolves. If a real binding needs something the closed vocabulary cannot express, the fix is a Layer 1 schema extension via release with review — never a Layer 2 freedom.

**Drift G — Two-axis "structural vs datasheet halves" applied to side effects.**

In `firmware_safety_extensions.md` §16, an earlier Claude proposed splitting "side effects" (read-clears, write-1-to-clear) into a "structural half" (in scope for ZER core) and "datasheet half" (floor). The audit caught this as smuggling per-peripheral semantics into ZER core where they don't belong. The corrected classification: side effects are floor for ZER core; vendor HAL types can cast structural shadows in user library territory using existing primitives (move struct). This pattern is preserved in the asm domain: per-instruction semantic verification is per-ISA hardware-floor; trying to verify it brings back the unbounded catalog Level C rejected.

**Drift H — Time estimates and coverage percentages.**

The temptation to produce concrete planning ("3 weeks for schema design, 5 weeks for reference bindings") generates numbers that are not derived from measurement. The audit caught this pattern in `firmware_safety_extensions.md` and the discipline is locked: no time estimates, no coverage percentages. Sequencing of work is real; durations are speculative. The discipline applies to asm work too — Section 1.7.17 lists dependencies without durations.

**Drift I — "Blast radius better than Rust unsafe" claim.**

The Level D analysis honestly named that wrong contracts in `@intrinsic_def` poison the verifier's reasoning about callers — worse blast radius than Rust's `unsafe` block. The temptation to elide this and claim "Level D is better than Rust unsafe" was caught in §1.6.7. The corrected framing: Level D is a DIFFERENT trade-off than Rust unsafe — better ergonomics and structured auditability; worse blast radius when a contract is wrong. The mitigation is the demand/promise asymmetry rule, not the structured nature of the contracts. This survives unchanged under Option E.

These drift patterns are the canonical anti-patterns for asm-architecture sessions. Future sessions should recognize and resist them. The locked discipline is the discipline that survives the audit.

### 1.7.28. Integration with other locked documents

This section's locked commitments integrate with the following documents:

**`CLAUDE.md`** — Top of file, "ZER's Goal" section, contains the locked program-consequence vs hardware-consequence vocabulary that this section applies to asm. Any change to `CLAUDE.md` that modifies the vocabulary requires updating this section to match.

**`docs/firmware_safety_extensions.md`** — The companion document for firmware safety. Its §1 (Executive Summary), §1a (Three-Sentence Locked Statement), §22 (Defensible Public Claim), and §23 (Anti-Patterns) carry the same vocabulary discipline applied to firmware. This section's §1.7.1 (vocabulary), §1.7.5 (structure/semantics straddle), §1.7.6 (closed-vocabulary kind-difference), and §1.7.13 (cross-reference) tie back to firmware_safety_extensions.md.

**`docs/compiler-internals.md`** — Top of file contains the "ZER's Goal — Why This Compiler Exists" section that references both `CLAUDE.md` and `docs/firmware_safety_extensions.md`. Any change to the four-category test for compiler changes (adds coverage / closes gap / neutral / weakens coverage = STOP) applies here too. A compiler change that would weaken Option E's program-consequence coverage on asm-bearing code is a STOP per the locked discipline.

**`docs/primitives-data-races.md`** — Definition A architecture across five safety domains. Option E's "Layer 1 schema + operation taxonomy in core, Layer 2 per-ISA bindings in user space" is Definition A applied to asm: ZER verifies access mechanism structurally (categories at call sites); semantic correctness against external substrates (ISA semantics) is library author responsibility supplied through declaration sites with audit visibility.

**`docs/universal_pointer.md`** — The 4-axis pointer safety decomposition is preserved unchanged under Option E. Pointer-related categories in the asm category vocabulary dispatch to the existing pointer safety machinery.

**`docs/limitations.md`** — The four documented known gaps (naked attribute IR-path drop, intrinsic VRP narrowing missing, bare-metal .bss zeroing build-system contract, ~2% opaque destructor heuristic) bound the "today" claim. Under Option E, the naked attribute IR-path drop (limitations.md:524) directly affects the escape-hatch escape semantics (naked asm bodies); the fix is on the critical path for completing Option E's full coverage.

**`docs/asm_plan.md`** — Superseded by this document at Level C (deletion baseline), Level D (mechanism), and Option E (layering). No content from `asm_plan.md` directly carries forward; this document is the locked architecture.

**`docs/proof-internals.md`** — The Coq+Iris+VST proof infrastructure. Option E's layered architecture changes the proof obligations: the safety theorem for asm operations becomes "for all programs P that use Layer 1 operations through Layer 2 bindings honoring their declared category profiles, program-consequence holds at every call site." This is a stronger and more honest theorem than "for all asm-bearing programs," which has the per-ISA semantic correctness problem.

### 1.7.29. The reading order for fresh sessions

A fresh session encountering this document should read in the following order:

1. **`CLAUDE.md` — "ZER's Goal" section (top of file).** Locks the program-consequence vs hardware-consequence vocabulary. Without this, every subsequent claim reads ambiguously.

2. **`docs/firmware_safety_extensions.md` — §1 (Executive Summary), §1a (Three-Sentence Locked Statement), §22 (Defensible Public Claim).** Applies the vocabulary to firmware. Establishes the boundary discipline.

3. **This section (1.7) — first.** Captures the locked asm architecture under Option E. The "READ FIRST" status is real; everything else in this document is read through Option E's lens.

4. **Section 1.6 (Level D).** Read AFTER 1.7. The `@intrinsic_def` mechanism, demand/promise asymmetry rule (§1.6.17), and missing-arch propagation rule (§1.6.18) are still current and survive Option E. The "blessed @core::* with per-ISA dispatch in core" framing in §1.6.16 is refined by §1.7.3.

5. **Section 1.5 (Two-Layer crystallization).** Read for context. The 2-layer Level C model is what Option E refines, not contradicts.

6. **Section 1.4 (Extended Discussion).** Read for historical context on deferred ideas (Tier 2 annotations, IR region wrappers, auto-guard emission). These remain deferred under Option E.

7. **Sections 2 onward.** Read for implementation details on the Level C deletion plan, file-by-file deletion list, testing strategy, etc. These are execution-level details that survive Option E unchanged.

If a fresh session reads in dependency order (1.4 → 1.5 → 1.6 → 1.7), they will encounter the "blessed catalog in core with per-ISA dispatch" framing of Level D before the Option E refinement, and may build mental model on the Level D framing that they then have to update. Reading 1.7 first is the locked-priority approach.

### 1.7.30. Closing note — the architectural arc

The asm safety thread, taken end-to-end from Levels A/B/C through D through Option E, follows a consistent architectural arc:

- **Constrain Y, delegate Z (Level C):** Bound the catalog ZER team maintains; defer ISA-specific concerns to GCC.
- **Externalize Y authorship (Level D):** Allow user libraries to add to the catalog via `@intrinsic_def` with structured contracts.
- **Remove ISA commitment from Y_blessed (Option E):** Layer 1 ships schema + taxonomy + mechanism, no per-ISA asm. Per-ISA bindings live in Layer 2 libraries. Layer 3 firmware is ergonomically normal.

Each step preserves the discipline established at the prior step. Each step is forced by an architectural fact that becomes visible at that step:
- Level C is forced by the unbounded catalog problem.
- Level D is forced by the sole-developer-maintenance-burden problem.
- Option E is forced by the structure/semantics straddle observation — once you see that asm uniquely straddles the program-domain/hardware-domain boundary, ISA-less Layer 1 with per-ISA Layer 2 bindings is the unique factoring that respects the straddle.

The closure argument from the language as a whole (no in-language `unsafe`, grammar-level rejection of integer-to-pointer fabrication) holds across all steps. The cinclude / naked-asm escape hatches are the bounded cross-substrate boundaries that no language can eliminate. Option E adds the `@bind` declaration as a third bounded boundary, with the trust gap shaped by the closed-vocabulary discipline so that review is a finite checklist.

The final architecture is the one that survives the audits, preserves the closure argument, respects the structure/semantics straddle, names the floor honestly, and ships an ergonomically normal Layer 3 experience for the firmware author. Option E is that architecture. The work to ship it is bounded by the implementation work order in §1.7.17, with the schema design as the real design work and the mechanical confirmation test in §1.7.11 as the artifact-decides discipline.

The architecture is locked. The work is bounded. The discipline is documented. This is the resting place of the asm safety thread, with Option E refining Level D's mechanism into a three-layer no-favored-ISA model that respects the unique structural property of asm and preserves the locked program-consequence vs hardware-consequence vocabulary.

### 1.7.31. Fault attribution at the binding boundary — the architectural defense

This subsection captures the explicit fault-attribution model for failures observed at the Layer 2 binding boundary. It is the architectural payoff of everything above: a sharp, defensible line between complaints that Layer 1 must reject (because they ask Layer 1 to do something architecturally outside its scope) and complaints that Layer 1 must own (because they describe genuine program-domain bugs over Layer 1's own categories). The discipline this section locks is the difference between a solo maintainer surviving the post-adoption complaint flow and a solo maintainer burning out fielding bug reports that the architecture says are not language-level bugs.

**The three failure categories at the binding boundary.**

When something goes wrong with code that uses a Layer 2 binding, the failure falls into exactly one of three categories. Each category has a different correct attribution.

**Category #1 — Binding categories do not match the silicon.**

The Layer 2 author declared a category profile that does not match what the asm body actually does on the target ISA. Example: the binding declares `no_memory_effect` on an instruction that secretly writes memory; the binding declares `no_flag_effect` on an instruction that clobbers flags; the binding declares `produces_carry` correctly but the asm uses a wrong opcode and silently produces zero.

**Attribution: Layer 2 author's fault. Rejected at Layer 1.**

The architectural defense:

> "There is no portable pattern for what an instruction does to hardware. Asm semantics are per-ISA hardware facts. The ISA manual is the datasheet for instruction meaning. Layer 1 cannot verify that a binding's declared categories match the silicon without importing per-ISA instruction semantics into the language core, which is exactly the unbounded catalog Level C rejected. Binding-correctness against silicon is the hardware-consequence floor, surfaced at the `@bind` declaration site, owned by the binding author. The closed-vocabulary discipline ensures the trust gap has a fixed shape so review is a finite checklist — but the floor itself exists and is the binding author's responsibility, not the language's."

This defense is **architectural, not bandwidth-driven**. The rejection is not "I do not have time to maintain ISA semantic verification." The rejection is "that is categorically not Layer 1's domain — it is the hardware-domain floor that exists for every memory-safe systems language at the asm boundary." A defense grounded in the architecture is repeatable, principled, and acceptable to engineers; a defense grounded in maintainer bandwidth is brittle, ad-hoc, and erodes trust.

The load-bearing property: **the program-vs-hardware boundary was drawn first**, in `CLAUDE.md` and `firmware_safety_extensions.md`, before any specific complaint about asm. The Category #1 rejection is the application of that pre-existing boundary to the asm domain, not a special-case carve-out for asm. This is why the rejection has teeth — it follows from the architectural commitments locked elsewhere in ZER, not from a one-off "this case is hard."

**Category #2 — Binding is honest but Layer 1's verifier mis-reasons over correctly-declared categories.**

The Layer 2 author declared a category profile that does match the asm body. But Layer 1's verifier dispatch logic fails to correctly propagate the declared categories to call sites. Example: the binding correctly declares `clobbers_flags`, but the verifier's category-dispatch logic fails to propagate that clobber to a caller that depends on flag preservation; the binding correctly declares `requires_aligned(ptr, 16)`, but the verifier's alignment-checking dispatch fails to enforce it at a call site with an unaligned argument.

**Attribution: Layer 1's bug. Owned. Must be bug-free.**

This is NOT "the math is wrong about hardware." It is "the math is wrong about its own categories." The hardware never entered the failure mode — the failure is pure program-domain logic over Layer 1's own closed schema. The Layer 2 author did everything correctly; the verifier dropped the ball reasoning over the closed vocabulary that Layer 1 owns.

The hardware-floor defense does NOT apply here. Layer 1's claim is "given a binding's correctly-declared categories, every program-domain operation on values flowing through that binding is verified at the call site." If the verifier fails at this claim, the claim is wrong, and Layer 1 owns the bug.

The discipline this implies for Layer 1's implementation: **the verifier dispatch logic over the closed category vocabulary must be bug-free.** This is a real commitment, not a hand-wave. The verifier must be tested against every category in the vocabulary, every dispatch path, every interaction between categories. Tests at the schema level (verifier behaves correctly given each category profile) and at the integration level (binding A combined with binding B in a call sequence behaves correctly) are mandatory.

The good news: **this commitment is bounded by the closed schema.** The category vocabulary is finite. The operation taxonomy is finite. The dispatch paths are finite. Verifier correctness over a finite schema is a finite testing surface, not an open-ended one. The math Layer 1 commits to is bounded by the schema design, which is itself bounded by the closed-vocabulary discipline. Solo maintenance of this surface is feasible because the surface does not grow with hardware evolution — it grows only with rare schema extensions through release.

**Category #3 — Category vocabulary cannot express the instruction honestly.**

The Layer 2 author has an instruction whose actual behavior cannot be honestly expressed by selecting from the existing category vocabulary. Either the relevant category does not exist in the schema, or two categories that should be distinct have merged into one and the author cannot distinguish their binding's behavior, or the schema is incomplete in some other structural way.

**Attribution: Layer 1's bug. Owned. Fixed through schema-extension release.**

This is a schema-completeness issue. The author is being honest (they cannot lie about hardware because the vocabulary does not let them express the truth), and the schema has a gap that prevents honest expression. The fix is a Layer 1 schema-extension cycle: add the missing category (frozen, reviewed, released), or split the merged categories (frozen, reviewed, released), and the author can then re-express the binding honestly.

This is the slow-cadence work the schema design discipline (§1.7.22) accommodates. Schema extensions are rare by construction — categories describe structural behavior kinds that exist across ISAs, and the set of such kinds is bounded by hardware-software interaction physics, not by per-instruction growth. Real-world schema-completeness issues should surface during the initial schema design (rigorous enumeration of category kinds across the supported machine models) and then arrive at low rate (per-several-years cadence) thereafter.

**Why owning #2 and #3 cleanly makes the #1 rejection credible.**

The architectural defense for Category #1 has teeth only if Layer 1 owns Categories #2 and #3 cleanly. If Layer 1 reflexively rejects every complaint with "hardware floor, your problem," engineers learn the defense is reflexive and stop trusting it. Friends who got excited about ZER hit a Category #2 bug, report it correctly, and watch it get waved off as "your binding's problem" — and they conclude the defense was a dodge all along.

The discipline:

- **Category #1 complaints get the architectural rejection, firmly and repeatably.** The rejection is grounded in the program-vs-hardware boundary locked elsewhere in ZER, not in maintainer bandwidth.
- **Category #2 bugs get owned, fixed, and shipped.** No deflection. The verifier dispatch over the closed schema is Layer 1's math, and the math must be correct.
- **Category #3 schema gaps get owned, evaluated, and resolved through release.** Either the gap is real (schema extension shipped) or the proposed binding can be re-expressed using existing categories (author guided through the closed vocabulary).

The credibility of the #1 rejection depends on the visible owning of #2 and #3. If a maintainer reflexively applies the hardware-floor defense to everything, the defense becomes a dodge. If a maintainer owns the bugs that are genuinely Layer 1's and rejects the ones that are not with a principled architectural reason, the rejection is legitimate and engineers accept it.

This is the difference between sustainable solo maintenance and burnout. The architecture provides the structure; the maintainer applies the structure consistently.

**The two-responsibility, one-boundary statement.**

Stated precisely, with the locked vocabulary:

> Layer 1 owns category-reasoning. Given a Layer 2 binding's correctly-declared category profile, Layer 1 must propagate the implications of those categories to every call site, every time, without bugs. This is program-domain logic over Layer 1's own closed schema. The math must be correct; the verifier must be tested; this commitment is bounded by the finite schema and the closed vocabulary, and it is solo-maintainable.
>
> Layer 1 does NOT own category-truth-against-silicon. Given a Layer 2 binding's declared category profile, Layer 1 trusts the declaration as the binding-correctness contract supplied by the library author. Whether the asm body actually has the declared categories on the target ISA is the hardware-consequence floor, surfaced at the `@bind` declaration site, library author's responsibility, reviewable as a finite checklist because the contract language is closed.
>
> The boundary between Layer 1's owned responsibility (category-reasoning) and the binding author's owned responsibility (category-truth-against-silicon) is the same program-consequence vs hardware-consequence boundary that ZER applies uniformly across all safety domains. Applying this boundary to fault attribution at the binding layer: program-domain failures are Layer 1's; hardware-domain failures are the binding author's. Both responsibilities are real; both are bounded; one is solo-maintainable and the other is library-author-owned by design.

**Why this works for solo maintenance.**

The Category #1 rejection is the load-bearing solo-maintainer survival mechanism. Without it, the language core's maintenance burden grows with every ISA extension, every new instruction, every vendor extension — which is exactly the unbounded catalog problem Levels C, D, and Option E were designed to avoid.

The Category #2 ownership is the load-bearing credibility mechanism. Without it, the #1 rejection sounds like a dodge. The bounded nature of the #2 surface (finite schema, finite dispatch paths, finite test matrix) makes #2 ownership feasible for a solo maintainer in a way that ISA-semantic verification (Category #1's territory) never would be.

The Category #3 schema-extension process is the load-bearing schema-evolution mechanism. Without it, the closed-vocabulary discipline becomes brittle (real expressivity gaps cannot be fixed) and the architecture's kind-difference from SPARK dissolves (authors reach for free-form predicates as workarounds, opening the closed vocabulary). The slow-cadence release process keeps the schema closed in practice while allowing genuine extensions to land when needed.

Together, the three responsibilities partition the failure space cleanly. Solo maintenance is feasible because:

- Category #1 is architecturally outside Layer 1's scope (no maintenance burden at all).
- Category #2 is bounded by the closed schema (finite, testable, manageable maintenance burden).
- Category #3 is slow-cadence (extension events per several years, not per release).

The combined ongoing burden is bounded, slow-growing, and architecturally well-defined. Option E is sustainable for one person because the architecture draws the responsibility boundary in the right place — at program-vs-hardware — and the boundary maps cleanly to fault attribution.

**The drift to refuse.**

The drift that breaks the model is reflexive #1-style rejection of #2 or #3 complaints. "My binding declared `clobbers_flags` but the caller is not seeing the clobber" is a #2 complaint, owned by Layer 1. Rejecting it as "your binding's problem, hardware floor" is the wrong attribution and corrodes credibility.

The dual drift is owning #1 complaints as if they were #2 or #3. "My binding declared `no_memory_effect` but the asm actually writes memory and ZER didn't catch the inconsistency" is a #1 complaint, rejected at Layer 1. Accepting it as a Layer 1 bug — "we should add per-instruction semantic verification for this case" — opens the unbounded catalog and breaks the architecture.

Both drifts must be refused. The discipline is the boundary itself: program-domain ↔ Layer 1 owns; hardware-domain ↔ binding author owns. The boundary is the same line ZER draws uniformly; applying it consistently at the binding layer is the architectural commitment.

**One more note on false positives from category clash.**

A practical question raised during the design discussion: what about false positives from category clash — where two operations have superficially similar category profiles and the verifier fails to distinguish them, producing a wrong rejection at a call site?

This is **not** automatically a #1 (binding author's) fault. The correct attribution depends on the root cause:

- If two operations have genuinely identical category profiles and the verifier cannot distinguish them, that may be a **schema-completeness issue (#3)** — the vocabulary is too coarse to express the structural difference, and a schema extension is the fix.
- If two operations have distinct category profiles and the verifier mis-dispatches between them, that is a **verifier bug (#2)** — Layer 1's reasoning over the closed schema has a flaw, owned and fixable.
- Only if the category clash arises from a Layer 2 author declaring categories incorrectly (e.g., copy-pasting from another binding without verifying applicability) is it a binding-author error — and even then, it is closer to a binding-correctness issue than a category-clash issue.

The discipline: when a false positive surfaces from category clash, diagnose the root cause before defending. Reflexively classifying it as the binding author's problem is the same credibility-erosion drift as misattributing #2 as #1.

**The closing on fault attribution.**

The architectural defense Layer 1 provides for Category #1 is the structural reason Option E is sustainable for one person, and its credibility depends on the visible owning of Categories #2 and #3. Stated as the locked discipline:

- Category #1 (binding-vs-silicon mismatch): architecturally outside Layer 1's scope. Rejected with the program-vs-hardware boundary defense. Repeatable, principled, accepted by engineers because it follows from the architecture, not from bandwidth.
- Category #2 (verifier mis-reasoning over correctly-declared categories): Layer 1's bug. Owned, tested, fixed, shipped. Bounded by the closed schema.
- Category #3 (vocabulary incompleteness): Layer 1's bug, schema-extension class. Owned through release process. Slow cadence, bounded by hardware-software interaction physics.

The boundary between #1 and {#2, #3} is the same program-consequence vs hardware-consequence boundary that ZER applies uniformly. The boundary's consistent application at the binding layer is what makes Option E architecturally coherent, ethically defensible to library authors and firmware engineers, and sustainable for a solo maintainer over the long term.

This is the architectural payoff of the entire Option E design. The closed-vocabulary discipline (§1.7.6, §1.7.7), the three-layer structure (§1.7.3), the structure/semantics straddle observation (§1.7.5), the safety-vs-governance separation (§1.7.8), and the program-consequence vs hardware-consequence vocabulary (§1.7.1) together produce a fault-attribution model that is principled, repeatable, and solo-maintainable. The architecture provides the structure; the maintainer applies the structure consistently. That is the model.

---

## 2. The Decision in One Page

**Keep (already shipped, working, truly frozen):**
- All 130 intrinsics from D-Alpha-1 through D-Alpha-14
- Z-rules Z1 through Z8, Z11, Z12 (10 of 13 wired into NODE_ASM)
- Naked-only restriction (S1) — asm only in `naked` functions
- Structured asm syntax (Session A: `asm { instructions: ... safety: ... }`)
- Typed operand bindings (Session B: `inputs:`, `outputs:`, `clobbers:`)
- F7-light LR/SC pairing (hardcoded mnemonic state machine, ~50 lines)
- Hardcoded UB classics dispatch (NEW: ~30 lines, ~12 frozen instructions)
- Operand type validation via existing ZER type system

**Delete (all ISA-coupled maintenance burden):**
- `src/safety/asm_register_tables_*.c` (4 files, 519 lines) — register name tables
- `src/safety/asm_register_lookup.c` (81 lines) — dispatch
- `src/safety/asm_register_tables.h` (116 lines) — schema
- `src/safety/asm_instruction_table_*.c` (3 files, 186 lines) — instruction tables
- `src/safety/asm_instruction_table.h` (188 lines) — schema
- `src/safety/asm_categories.{c,h}` (252 lines) — 8-category framework skeleton
- `scripts/gen_register_tables.sh` (124 lines) — register probe
- `scripts/gen_instruction_table.sh` (373 lines) — instruction probe
- `scripts/candidates_*.txt` (3 files, 468 lines) — register candidate lists
- `arch_data/*.zerdata` (3 files + SCHEMA, 1,532 lines) — per-instruction data
- F7-full Step 2 dispatch code in checker.c (~80 lines, replaced by hardcoded list)
- Session G Phase 1+2 plumbing (`ZerBarrierKind`, `ZerOrderingRole` enums, ~50 lines)
- CPU feature enum + flag mapping in checker.c (~30 lines, replaced by -m flag passthrough)
- Obsolete planning docs in asm_plan.md (Session G Phase 5 plan, re-audit rounds 2-6, ~3,000 lines)

**Defer to GCC:**
- Register name validation per arch (`rax` on ARM = GCC error)
- Instruction validity per arch (`bsr` on ARM = GCC error)
- CPU feature gating (`xmm16` needs `-mavx512f` = GCC error)
- Operand class enforcement (GCC constraint letters)
- Assembly syntax correctness
- Sub-extension support (AMX, SVE, SME, AVX-10, future ISAs)
- New architecture support (any arch GCC adds)

**Public-facing safety claim becomes:**

> "ZER's asm safety: operand-type checking (via existing ZER type system) +
> Z-rules (memory/type/concurrency/MMIO safety through asm operands) +
> 130 intrinsics for common kernel patterns + hardcoded protection for
> well-known UB classics (BSR-on-zero, IDIV-on-zero, MOVAPS-misaligned,
> LR/SC pairing) + naked-fn isolation (MISRA Dir 4.3). Register validation,
> CPU feature gating, instruction validity all handled by your C compiler's
> assembler — works on every architecture GCC supports."

This is honest and accurate. No marketing inflation. No commitment to
perpetual maintenance.

---

## 3. The Transpiler Nature of ZER — Why This Works

ZER is a **transpiler to C**, not a compiler that emits machine code:

```
ZER source → zerc → C source → C compiler → executable
                    ↑                ↑
            (we build this)    (anyone's compiler)
```

This is the same architecture as Nim, Vala, original C++ "Cfront", Haxe.
The user provides their own C compiler. ZER doesn't ship a code generator.

### Why this matters for asm safety

Since the user's C compiler is the actual gateway to machine code, any
ISA-specific validation we do at the ZER level is **duplicating work** the
user's C compiler will do anyway:

| Validation | Where it happens at Level A | Where it happens at Level C |
|---|---|---|
| Register name valid for arch | ZER checker (using vendored tables) AND GCC | GCC only |
| CPU feature flag matches | ZER checker (vendored enum) AND GCC | GCC only (`-m` flags) |
| Instruction valid for arch | ZER checker (vendored tables) AND GCC | GCC only |
| Operand encoding fits | ZER (partial) AND GCC | GCC only |

Both validations produce a compile-time error. The user can't run the broken
code in either case. The only difference is **which tool emits the error
message** — ZER's formatted version, or GCC's.

For a sole developer with no maintenance budget, deferring to GCC is the
right call. The error message difference is cosmetic; the safety difference
is zero.

### Why this works for multiple C compilers

GCC and Clang accept essentially the same C. The C we emit uses GNU C
extensions (`__auto_type`, `__typeof__`, statement expressions, `__atomic_*`,
`__builtin_*`, AT&T inline asm syntax) — all of which Clang also supports.

```bash
# These both work today, no changes needed:
zerc main.zer --emit-c -o main.c
gcc main.c -o main_gcc
clang main.c -o main_clang
```

### What this means for the future

If we ever want to support MSVC, Keil, IAR, or other compilers:
- Add an emit mode: `--emit-target=msvc` produces different C (no statement expressions, `_Interlocked*` instead of `__atomic_*`, etc.)
- Or pre-processing layer that converts GNU C → portable C
- Not planned today; not needed for ZER's audience

**Level C makes this future flexibility easier** — fewer ZER-side dependencies
on GCC-specific things (probe scripts, vendored data) means switching
backends is more contained.

### Concrete consequence for asm safety

**We do not need to be in the ISA-validation business.** Every C compiler
already is. Our job is the safety story (memory/type/concurrency/MMIO);
the C compiler's job is making sure the asm assembles. They don't overlap.

This is the architectural insight that makes Level C correct.

---

## 4. GCC Coverage in Embedded — Reality Check

ZER's target audience is kernel/firmware/embedded developers. Does GCC
actually cover this audience? Yes, overwhelmingly.

### Open-source / hobbyist / maker (100% GCC)

- **Linux kernel** — GCC canonical for 30+ years (Clang support recent, alternative)
- **BSDs** (FreeBSD, OpenBSD, NetBSD) — GCC and Clang
- **STM32 firmware** — `arm-none-eabi-gcc` (Cortex-M0/M0+/M3/M4/M7/M33/M55)
- **ESP32** — `xtensa-esp32-elf-gcc` (Espressif's GCC fork) + RISC-V GCC for newer chips
- **Arduino** — `avr-gcc` + `arm-none-eabi-gcc`
- **Raspberry Pi Pico** — `arm-none-eabi-gcc` (RP2040 Cortex-M0+)
- **Zephyr RTOS** — GCC primary
- **FreeRTOS** — GCC
- **RT-Thread, NuttX, Mbed OS** — GCC
- **OpenWrt routers** — GCC for MIPS, ARM, x86
- **Yocto/Buildroot embedded Linux** — GCC

### Vendor toolchains that ARE GCC underneath

- **Xilinx Vitis** (FPGA soft cores)
- **NXP MCUXpresso**
- **Nordic nRF Connect SDK**
- **Espressif IDF**
- **Renesas e² studio** (some chips)
- **TI MSP430** (msp430-gcc)

### Architectures GCC supports (matters for Level C's "fit all archs")

- x86, x86_64
- ARM (32-bit ARMv4 through ARMv8.x, all Cortex-A/R/M variants)
- AArch64 (ARM64) with SVE/SVE2/SME
- RISC-V (32-bit and 64-bit, all extensions)
- AVR (Arduino, ATtiny, ATmega)
- MSP430 (TI low-power)
- Xtensa (ESP32, Tensilica DSPs)
- PowerPC (32-bit/64-bit, big/little endian)
- MIPS (32-bit/64-bit, big/little endian)
- SPARC, s390x (IBM Z), m68k, SuperH
- ARC, NIOS2, MicroBlaze (FPGA soft cores)
- PIC (via Microchip GCC fork)
- Blackfin, ColdFire (legacy DSPs)

**Level C inherits this entire matrix for free.**

### The exceptions (not ZER's audience)

| Platform | Compiler | ZER target? |
|---|---|---|
| macOS / iOS apps | Clang exclusive | NO |
| Windows kernel drivers | MSVC | NO |
| Apple Silicon bare-metal | Clang | Rare |
| Certified medical/automotive | Keil ARM, IAR (ISO 26262/IEC 61508 cert) | Niche commercial |
| CUDA GPU | nvcc (LLVM-based) | NO |
| AMD ROCm | LLVM | NO |
| Intel oneAPI HPC | Intel compilers (LLVM) | NO |

**For ZER's actual audience: these don't matter.** People writing ZER are
GCC users by definition (open-source/hobbyist/embedded firmware).

### What this means for Level C

When we say "defer to GCC," we mean: ZER targets the universe of programmers
who use GCC, which is essentially the entire embedded/firmware/kernel
open-source ecosystem. The "loss" of not supporting Keil/IAR/MSVC isn't
real because those aren't ZER users.

The "gain" is enormous: ZER works on every architecture GCC supports, with
zero ZER-side maintenance. New architectures show up in GCC; ZER works on
them automatically.

---

## 5. Why This Pivot — Bug History and Maintenance Reality

### 5.1 The bug rate signal

Over the 6-week period from 2026-04-23 to 2026-05-02, the asm-safety work
shipped progressively more infrastructure:

| Date | Item shipped |
|---|---|
| 2026-04-23 | D-Alpha-7.5 Phase 1 (`naked` restriction, S1 rule) |
| 2026-04-25 | Session A (structured asm syntax) |
| 2026-04-25 | Session B (typed operand bindings) |
| 2026-04-26 | F1a category framework |
| 2026-04-26 | F2 build-time probe pipeline (x86_64 register table) |
| 2026-04-26 | F7-minimum (register validation wired) |
| 2026-04-29 | Sub-extension architecture validated 3-arch |
| 2026-04-29 | F4.1+F4.2 x86_64 instruction tables (53 entries) |
| 2026-05-02 | F5 aarch64 instruction tables (37 entries) |
| 2026-05-02 | F6 riscv64 instruction tables (30 entries) |
| 2026-05-02 | F7-light LR/SC pairing |
| 2026-05-02 | F7-full Step 2 constraints (NONZERO/COMPOUND/ALIGNED/BOUNDED) |
| 2026-05-02 | Session G Phase 1+2 (ordering plumbing) |
| 2026-05-02 | Session G Phase 3 attempted, ABANDONED |
| 2026-05-12 | Pivot decision (Level C) |

### 5.2 The maintenance projection

Continuing the original direction would commit to:

| Year | Expected ISA events | Maintenance impact |
|---|---|---|
| 2026 | Session G Phase 5 (CFG OrderingState) | ~30-40 hours |
| 2026 | Z9/Z10/Z13 forward-compat | ~50 hours |
| 2027 | AVX-10.2 release expected | ~100-200 instruction entries |
| 2027 | ARM v10 SVE 3.0 | ~150-200 entries |
| 2027 | New register names per chip rollout | ~20-30 entries |
| 2028 | RISC-V V 1.1 + new Zb*/Zk* extensions | ~100 entries |
| 2028 | Intel AVX-512 successor (probable) | ~200+ entries |
| 2029 | ARM SME 2.0 | ~150 entries |
| 2030+ | Various vendor extensions, new architectures | ~200+ entries/year |

Cumulative over 5 years: **~1,000-1,500 instruction entries to add, vet,
classify, integrate, test.** Plus ongoing per-instruction bug fixes as edge
cases emerge (Session G Phase 3 abandonment is the canonical example —
"we tried, real code broke, had to revert").

**For a sole developer, this is unsustainable.** Even at Level A (keep
register tables), we'd carry ~1-2 GCC-coupled sync events per year forever.
Level C eliminates them all.

### 5.3 The realization

The per-instruction approach commits ZER to **perpetual maintenance of a
domain that is fundamentally not ZER's domain**. Silicon evolution is
Intel/ARM/RISC-V's responsibility. They publish manuals; GCC integrates
the assembler; ZER doesn't need to know what every instruction does.

The asm_plan direction implicitly assumed: "since we want to make asm
safe, we need to know what asm does." This is **wrong**. The correct
inversion: "since we want to make asm safe, we need users to declare
what their asm does (operand structure), and we enforce existing safety
invariants on those operands (UAF, escape, MMIO, etc.). Everything else
is the C compiler's job."

The user owns the instruction manual. The C compiler owns ISA validation.
ZER owns memory/type/concurrency safety. Three clean responsibilities,
no overlap.

### 5.4 The Session G Phase 3 lesson

The Phase 3 attempt was the empirical signal:

**The bug:** Same-block check rejected the canonical Intel libpmem
persistent-memory idiom (CLWB issued in one block, SFENCE in another).
The "correct" check requires CFG-aware analysis (Phase 5), another 30-40
hours of work and a 700+ line addition to `zercheck_ir.c`.

**The deeper lesson:** Even with perfect per-instruction data, the
COMPILER-SIDE analysis to enforce that data is complex and error-prone.
Phase 3 had correct classification data; the analysis logic still got
real code wrong.

**Level C eliminates the entire class of problems** by removing both the
data AND the analysis. GCC handles the ISA-specific stuff. ZER handles
the safety story.

### 5.5 Convergent evidence: Rust didn't do this either

Rust's `asm!()` macro:
- Validates operand types ✓
- Validates register CLASSES (`out("reg") x` = "any GPR") ✓
- Does NOT validate specific register names per arch
- Does NOT validate instruction semantics
- Does NOT do CFG-aware analysis of asm contents

When Rust users write asm requiring instruction-specific preconditions, they
use `// SAFETY:` comments and `unsafe` block boundary. The compiler trusts
human declaration.

Rust made this call after extensive design discussion (RFC 2873). They
explicitly chose NOT to do what ZER's asm_plan was attempting.

**Level C brings ZER into alignment with Rust's pragmatic approach** —
plus a small bonus (~12 hardcoded UB classics protection) that even Rust
doesn't have.

---

## 6. The Architectural Principle Being Applied

### 6.1 The principle (one sentence)

**Compilers should be generic algorithms over annotated languages, not
databases of facts about specific entities.**

### 6.2 Where ZER already follows this

ZER's existing safety architecture rigorously applies this principle:

| Per-item information | Where it lives | Why this works |
|---|---|---|
| Which fields are volatile? | Language: `volatile *u32 reg` | Compiler enforces generically |
| Which params are kept? | Language: `keep *Handler h` | Compiler enforces generically |
| Which addresses are MMIO? | Language: `mmio 0x4000..0x4FFF;` | Compiler enforces generically |
| Which structs auto-lock? | Language: `shared struct C { }` | Compiler enforces generically |
| What's a handle? | Language: `Handle(T) h` | Compiler enforces generically |
| Which structs transfer? | Language: `move struct F { }` | Compiler enforces generically |
| Which functions can yield? | Computed: `Symbol.props.can_yield` | Generic FuncProps analysis |

**None require the compiler to have a database of specific structs,
addresses, drivers, or operations.** The language carries per-item info;
the compiler runs generic algorithms.

### 6.3 Where asm_plan accidentally violated the principle

The original asm_plan direction encoded per-instruction facts in the compiler:

| Per-instruction fact | Encoded as | Maintenance cost |
|---|---|---|
| "BSR has UB on zero" | Constraint in instruction table | Stable but in growing DB |
| "MOVAPS needs 16-byte alignment" | Constraint in instruction table | Stable but in growing DB |
| "LR must precede SC" | State-machine class in instruction table | Stable but in growing DB |
| "MFENCE produces full-memory barrier" | Ordering metadata in instruction table | In growing DB |
| ...× ~4,000 more facts (potential) | Per-instruction entries | Compounds with every ISA extension |
| "Which registers are GPRs vs vec?" | Per-arch register table | Sync with GCC capability |
| "Which CPU features exist?" | Enum, growing | ~2-3/year additions |

**The right answer (Level C):**

- Hardcode the ~12 well-known UB classics directly in checker.c (`bsr`,
  `bsf`, `div`, `idiv`, `movaps`, `movapd`, `movdqa`, `lr.w`, `lr.d`,
  `sc.w`, `sc.d`, `ldxr`, `stxr`) — these are 1980s-1990s classics, frozen.

- Defer register tables to GCC. User's C compiler errors if `rax` used on ARM.

- Defer CPU feature gating to GCC. User passes `-mavx512f` flag; GCC errors
  if AVX-512 instructions used without it.

- Generic safety (Z-rules) applies to operand expressions via existing
  systems (VRP, alignment, escape, MMIO, etc.).

The compiler stays generic. The world can change without the compiler changing.

### 6.4 The general lesson

If you find yourself adding a database to a compiler about specific items
(specific structs, functions, API calls, instructions, addresses), **STOP**.
The right answer is:

1. Identify the property the database encodes
2. Either: design a language-level annotation expressing it (smart language)
3. Or: defer to a more authoritative tool that already validates it (smart delegation)
4. Don't grow the per-item database

Level C applies BOTH inversions to asm safety:
- Smart language: Z-rules, operand types, hardcoded UB classics (small fixed set)
- Smart delegation: register names, CPU features, instruction validity → GCC

---

## 7. The Three Levels Considered

The 2026-05-12 discussion went through three increasingly aggressive cleanup
levels:

### 7.1 Level A — Keep existing infrastructure, add annotations

**Description:** Build on existing Session A/B asm syntax. Keep register
tables, F7-light, F7-full Step 2, CPU feature gating. Add operand metadata
fields, language-level precondition annotations, tiny implicit-precondition
table (~100 entries). Stop extending per-instruction database beyond current
state.

**What it preserves:**
- Per-arch register validation at compile time
- CPU feature gating in ZER's checker
- ZER-formatted error messages
- Implicit table for ~100 well-known UB classics

**Effort:** ~3-4 months one-time + ongoing maintenance (~1-2 events/year
for register tables, ~2-3/year for CPU feature flags).

**Why ultimately rejected:**
- "Low maintenance" isn't zero
- Sole-developer-unfriendly perpetual sync
- Doesn't fully apply the smart-language principle
- Duplicates work the C compiler does anyway

### 7.2 Level B — Drop register tables, keep CPU feature gating

**Description:** Like Level A but ALSO drops the register tables. Keeps
CPU feature flag enum. Specific register validation defers to GCC.

**What it preserves:**
- CPU feature gating in ZER's checker (small enum, ~14 entries)
- Register class validation (via operand annotations like `class: vec`)
- Hardcoded UB classics

**Effort:** ~3-4 months (similar to Level A; register-table work was small fraction).

**Why rejected:**
- Still has ~2-3 CPU feature additions per year (GCC-coupled)
- Half-measure: if we're dropping ISA-specific stuff, drop ALL of it
- Inconsistent: validates CPU features but not specific registers

### 7.3 Level C — Drop everything ISA-specific (CHOSEN)

**Description:** Drop register tables AND CPU feature gating AND probe scripts
AND instruction tables AND 8-category framework. Defer everything ISA-specific
to GCC. Keep only the frozen safety layer.

**What it preserves:**
- Naked-only restriction (S1)
- Z-rules Z1-Z8, Z11, Z12
- 130 intrinsics
- F7-light LR/SC pairing (hardcoded mnemonics, frozen)
- Hardcoded UB classics (~12 instructions, frozen)
- Session A/B asm syntax
- Operand type checking via existing ZER type system

**Effort:** ~1-2 days one-time. **TRULY zero maintenance after.**

**Why selected:**
1. Sole developer wants zero maintenance forever
2. ZER is a transpiler — duplicating GCC's validation is unnecessary
3. "Fit all architecture" is STRONGER at Level C (inherit GCC's full matrix)
4. Production-compiler precedent (Rust/Zig don't validate register names)
5. Smallest possible code surface
6. Cleanest architectural division (ZER does safety; GCC does ISA)

### 7.4 Side-by-side comparison

| Property | Level A | Level B | Level C (CHOSEN) |
|---|---|---|---|
| Effort | ~3-4 months | ~3-4 months | ~1-2 days |
| Code reduction | ~5,600 lines | ~6,700 lines | ~7,000 lines |
| Register table | KEEP (vendored, probed) | DELETE (defer to GCC) | DELETE (defer to GCC) |
| CPU feature enum | KEEP (~14 entries) | KEEP (~14 entries) | DELETE (defer to GCC -m flags) |
| Probe scripts | KEEP (register probe) | KEEP (register probe) | DELETE all |
| Instruction tables | Stop extending | DELETE | DELETE |
| Hardcoded UB classics | ~100 implicit table entries | ~100 implicit table entries | ~12 hardcoded in checker.c |
| Annotations | Add | Add | Not needed |
| F7-light LR/SC | Keep + generalize | Keep + generalize | Keep as-is (hardcoded) |
| F7-full Step 2 | Keep dispatch | Keep dispatch | Refactor to hardcoded |
| Ongoing maintenance | Low but nonzero | Low but nonzero | **TRULY zero** |
| Architecture support | 3 archs maintained | 3 archs (registers via GCC) | Every arch GCC supports |
| ZER error vs GCC error | ZER errors for everything | ZER for features, GCC for registers | GCC for everything ISA-specific |
| Match production compilers | Stricter than Rust | Closer to Rust | Matches Rust (+ classics bonus) |

Level C dominates on every practical axis for ZER's sole-developer
multi-architecture goal.

---

## 8. Journey of the Discussion

The decision was reached through a long architectural discussion across
multiple sessions. The journey matters as much as the conclusion, preserved
here for fresh sessions.

### 8.1 Starting point (2026-05-12 morning)

User asked: "where are we on asm_plan.md?"

Answer at the time: D-Alpha-7.5 Phase 2 in progress. F4-F7 instruction
tables shipped. Session G Phase 3 abandoned. Session G Phase 5 next at
~30-40 hours. Z9/Z10/Z13 forward-compat blocked.

### 8.2 The "make it less complex" framing

> "we did so complex... should we make raw asm unsafe but push everything
> through intrinsics that supported every architecture every machine
> related thing"

**Response:** Pure-intrinsic infeasible (~4,400 instructions × 3 archs ×
growing forever). Hybrid (intrinsics + raw asm + partial validation) is
the right shape — but stop extending the validation side.

### 8.3 The maintenance critique

> "our code shouldn't keep being maintained like today new chip come out
> and stuff need to update spec... but it should be safe cause it is IR
> based, how can we achieve that even with subarch like AVX?"

**Response (key breakthrough):** Operand-kind validation, not instruction
validation. Frozen 10-category enum. AVX-1024 in 2035 still uses these
categories. No per-ISA-extension maintenance.

### 8.4 The "ASM is just abstraction" framing

> "ASM is just abstraction to machine language... using intrinsic, like
> cannot fully but just common, I want full 100% usability, I don't have
> time constraint, what about ASM IR, ASM to machine?"

**Response:** Operand-kind IR (Option 1). Full structured asm AST with
generic safety pipeline. ~8-12 months effort.

### 8.5 The "do we need rewrite?" question

> "I mean that means rewriting whole stuff right if we do that, what if
> we use existing Z-classes since we already did it, just add a complexity
> for these manual stuff"

**Response:** Option 2 (extend current) is viable. Same safety as Option 1,
3x less work. Add operand metadata + annotations + tiny implicit table to
existing Session A/B syntax. Keep Z-rules.

### 8.6 The "are they the same?" verification

> "so both are actually the same solution right, one is just implemented
> in IR level a rewrite whereas the other is whole language enforces to
> ASM... is there any usability risk?"

**Response:** Functionally equivalent for ZER's actual use cases. Different
internal architecture, identical user experience. Decision: Option 2
(extend current design + annotations + tiny implicit table). Estimated
~3-4 months. This became Level A.

### 8.7 The "is this really good?" challenge

> "is this really good refactor?"

**Response:** Honestly evaluated. The plan is good but over-scoped. The 6
helpers split into 3 high-value items and 3 speculative items. Recommended
Phase A (~4 hrs) + defer Phase B. (Note: this was about refactor_ir, but
the same instinct applies to asm work.)

### 8.8 The "Manual is fine" insight

> "these manual we can introduce language complexity like annotations or
> ownership or something else to solve this... meaning it would throw
> error which means its safe"

**Response:** Annotations are compile-time CONTRACTS. Compiler enforces
via existing safety infrastructure. Same safety as auto detection.

### 8.9 The "smart language" principle articulation

> "our asm plan one did this by making compiler smart not by adding
> complexity correct which is not good which became maintenance hell"

**Response:** This is the architectural principle. ZER's 28 of 29 safety
systems follow it. asm_plan was the violation. The fix: smart language
(annotations + frozen primitives) + generic compiler (existing safety
systems) = no per-item database.

### 8.10 The "registry table is must?" question (Level A→B→C inflection)

> "low doesn't mean like good also right... I think the registry table
> still need or no need or that dependent on GCC?"

This was the pivotal question. Re-examining the "LOW maintenance"
classifications honestly:
- Register tables: GCC-dependent, ~1-2 sync events/year
- CPU feature enum: ~2-3 additions/year
- F7-light LR/SC: truly frozen
- Hardcoded UB classics: truly frozen

The user noticed "low" wasn't "zero." If the goal is true zero, drop the
GCC-coupled stuff entirely.

**Response:** Presented Level A vs B vs C. Level A still has nonzero
maintenance. Level B drops register tables but keeps CPU features. Level C
drops everything ISA-specific. Recommended Level C for sole-developer
zero-maintenance goal.

### 8.11 The "GCC covers everything" verification

> "GCC already supported like full everything right cause EVERYTHING is
> ran by GCC whole shit industry, exception is of course LLVM is option
> in software level, but embedded is GCC?"

**Response:** Verified extensively. GCC dominates ZER's audience (embedded/
firmware/kernel open-source). Exceptions (Keil/IAR/MSVC) are commercial
niches that aren't ZER's audience. Level C inherits GCC's full architecture
matrix.

### 8.12 The "transpiler nature" verification

> "even if these diff compiler the way we wrote can easily just switch
> right... GCC next day can just switch to CLang, MSVC we are not making
> our compiler we are transpiling into C correct?"

**Response:** Correct. ZER → C → user's choice of C compiler. GCC ↔ Clang
~95% compatible (Clang accepts GNU C). MSVC needs emitter changes (not
planned, not ZER's audience). Level C makes the transpiler nature explicit:
defer ISA stuff to whatever C compiler the user picks.

### 8.13 The final decision

> "alright let's do C update the asm_lang_zer_safe with everything stuff etc"

**Decision: Level C. Execute the cleanup.** This document is the
comprehensive plan for the execution.

---

## 9. What We're Keeping at Level C

The Level C work preserves the truly frozen safety core. **Nothing here
requires per-ISA-extension maintenance.**

### 9.1 The 130 intrinsics (D-Alpha-1 through D-Alpha-14)

All 130 intrinsics shipped between 2026-04-23 and 2026-04-24 stay exactly
as they are:

- D-Alpha-1: 7 atomic intrinsics (xchg, nand, *_fetch × 5)
- D-Alpha-2: 10 barrier/bit-query/hint intrinsics
- D-Alpha-3: 5 interrupt control intrinsics (privileged)
- D-Alpha-4: 4 context switch intrinsics
- D-Alpha-9: 10 MSR/CR/XCR0 intrinsics
- D-Alpha-10: 10 inspection intrinsics (non-privileged)
- D-Alpha-11: 5 power management intrinsics
- D-Alpha-12: 6 privileged mode transition intrinsics
- D-Alpha-13: 20 Linux-scale x86 essentials
- D-Alpha-14: 12 final misc intrinsics

These cover ~80% of common kernel/firmware asm patterns. They're the
"primary safe path" for users. Fixed set, no per-ISA growth.

### 9.2 Z-rules Z1 through Z8, Z11, Z12 (10 of 13 wired)

Already-shipped Z-rules stay:

| Z-rule | What it does |
|---|---|
| Z1 | UAF check at asm operand boundary |
| Z2 | Move struct transfer at asm operand |
| Z3 | VRP range invalidation on asm output |
| Z4 | Provenance type clear on asm output |
| Z5 | Local-derived pointer rejection with memory clobber |
| Z6 | Asm-in-defer/async ban (forward-compat) |
| Z7 | MMIO range check on asm memory operand (via @inttoptr) |
| Z8 | Qualifier preservation on asm output |
| Z11 | Non-keep pointer param + memory clobber rejected |
| Z12 | scan_frame walker recurses into structured asm operands |

These work. They're tested. They cover the core safety dimensions
(memory/type/concurrency/MMIO/provenance/qualifier) through the asm boundary.

**Z9, Z10, Z13 are NOT shipped and won't be** (deferred indefinitely).

### 9.3 Naked-only restriction (S1)

Phase 1 verified rule: `zer_asm_allowed_in_context(in_naked)` enforces asm
only inside `naked` functions. This is the v1.0 interim guard and the
foundational structural rule.

**Stays. Permanent.** MISRA Dir 4.3 compliant.

### 9.4 Structured asm syntax (Session A)

The `asm { instructions: "..." safety: "..." }` form stays. The mandatory
`safety:` documentation string (≥30 chars) stays. This is the user-facing
entry point.

### 9.5 Typed operand bindings (Session B)

The `inputs: { "reg" = expr }`, `outputs: { "reg" = lvalue }`, `clobbers:
[...]` form stays. Operand expressions go through ZER's normal type
checking, which:
- Validates expression type is integer or pointer (existing)
- Tracks expression through escape analysis (existing)
- Tracks through provenance, qualifier preservation, etc. (existing Z-rules)

What changes at Level C: the register name in the binding key is **no
longer validated against a per-arch table**. GCC's assembler validates it
when the emitted C is compiled. If `"rax"` is used on ARM, GCC errors.

### 9.6 F7-light LR/SC pairing (hardcoded mnemonics)

The hardcoded state-machine for paired atomic operations:

```c
/* In checker.c NODE_ASM handler */
if (target_arch == TARGET_RISCV64) {
    if (mnemonic_is_lrsc_open(mnemonic)) {
        block_state.lrsc_open = true;
    } else if (mnemonic_is_lrsc_close(mnemonic)) {
        if (!block_state.lrsc_open) {
            checker_error(c, line, "sc.w without preceding lr.w");
        }
        block_state.lrsc_open = false;
    }
}
/* Similar for ARM LDXR/STXR, x86 MONITOR/MWAIT */
```

Pairs handled (frozen since 1990s-2000s):
- RISC-V: `lr.w` / `sc.w`, `lr.d` / `sc.d`
- ARM: `ldxr` / `stxr`, `ldaxr` / `stlxr`
- x86: `monitor` / `mwait`, `umonitor` / `umwait`

These mnemonics haven't changed in decades. No maintenance burden.

### 9.7 Hardcoded UB classics (NEW at Level C — replaces F7-full Step 2 table dispatch)

A small frozen list in checker.c (~30 lines):

```c
/* In checker.c NODE_ASM handler */
static const struct { 
    const char *mnemonic; 
    int operand_idx; 
    int constraint;
    const char *citation;
} ub_classics[] = {
    /* Bit-search — UB on zero (Intel SDM Vol 2 BSR/BSF) */
    { "bsr",     0, REQUIRES_NONZERO,            "Intel SDM Vol 2 BSR" },
    { "bsf",     0, REQUIRES_NONZERO,            "Intel SDM Vol 2 BSF" },
    
    /* Division — UB on zero divisor (Intel SDM Vol 2 DIV/IDIV) */
    { "div",     0, REQUIRES_NONZERO,            "Intel SDM Vol 2 DIV" },
    { "idiv",    0, REQUIRES_NONZERO | COMPOUND_INTMIN_NEG1, 
                                                  "Intel SDM Vol 2 IDIV" },
    
    /* Aligned vector loads — SIGSEGV on misalignment */
    { "movaps",  0, REQUIRES_ALIGN_16,           "Intel SDM Vol 2 MOVAPS" },
    { "movapd",  0, REQUIRES_ALIGN_16,           "Intel SDM Vol 2 MOVAPD" },
    { "movdqa",  0, REQUIRES_ALIGN_16,           "Intel SDM Vol 2 MOVDQA" },
    { "vmovaps", 0, REQUIRES_ALIGN_32,           "Intel SDM Vol 2 VMOVAPS" },
    { "vmovapd", 0, REQUIRES_ALIGN_32,           "Intel SDM Vol 2 VMOVAPD" },
    { "vmovdqa", 0, REQUIRES_ALIGN_32,           "Intel SDM Vol 2 VMOVDQA" },
    
    /* End of list — frozen at well-known UB classics */
};
```

Each entry dispatches to existing safety infrastructure:
- `REQUIRES_NONZERO` → existing VRP (System #12)
- `REQUIRES_ALIGN_N` → existing alignment infrastructure
- `COMPOUND_INTMIN_NEG1` → existing signed-overflow handling

**~12 entries. Frozen. Haven't changed semantics since 1980s-1990s.**

No new safety logic. Just a small dispatch table that uses existing systems.

### 9.6 Operand type validation via existing ZER type system

Operand expressions in `inputs:`/`outputs:` are regular ZER expressions.
They go through:
- Type inference (existing)
- Integer/pointer/distinct type checking (existing)
- Escape analysis (existing)
- Provenance tracking (existing)
- Qualifier preservation (existing)

No asm-specific code. Same systems that validate `x + y` validate the
expressions in asm operands.

---

## 10. What We're Deleting at Level C

This section enumerates the actual file/code deletions. Total: ~7,000 lines.

### 10.1 Instruction tables (per-instruction database)

```
src/safety/asm_instruction_table_x86_64.c     # 75 lines
src/safety/asm_instruction_table_aarch64.c    # 59 lines
src/safety/asm_instruction_table_riscv64.c    # 52 lines
src/safety/asm_instruction_table.h             # 188 lines
```

**Total: 374 lines.** The per-instruction safety classification database.

### 10.2 Register tables

```
src/safety/asm_register_tables_x86_64.c        # 121 lines
src/safety/asm_register_tables_x86_64_avx512f.c # 177 lines
src/safety/asm_register_tables_aarch64.c        # 74 lines
src/safety/asm_register_tables_riscv64.c        # 147 lines
src/safety/asm_register_lookup.c                # 81 lines
src/safety/asm_register_tables.h                # 116 lines
```

**Total: 716 lines.** Per-arch register name validation. GCC handles this.

### 10.3 Categories framework

```
src/safety/asm_categories.c                     # 181 lines
src/safety/asm_categories.h                     # 71 lines
```

**Total: 252 lines.** The 8-category skeleton with no enforcement.

### 10.4 Probe scripts

```
scripts/gen_instruction_table.sh                # 373 lines
scripts/gen_register_tables.sh                  # 124 lines
scripts/candidates_x86_64.txt                   # 255 lines
scripts/candidates_aarch64.txt                  # 63 lines
scripts/candidates_riscv64.txt                  # 150 lines
```

**Total: 965 lines.** GCC probe scripts and candidate lists.

### 10.5 Vendored architecture data

```
arch_data/x86_64.zerdata                        # 526 lines
arch_data/aarch64.zerdata                       # 363 lines
arch_data/riscv64.zerdata                       # 323 lines
arch_data/SCHEMA.md                              # 320 lines
```

**Total: 1,532 lines.** Per-instruction classification data.

### 10.6 Checker.c refactoring (code deletion + replacement)

| Code section | Lines deleted | Lines added (replacement) |
|---|---|---|
| F7-full Step 2 table-driven dispatch | ~80 | ~30 (hardcoded UB classics list) |
| Session G Phase 1+2 plumbing (ordering enums, fields) | ~50 | 0 |
| Register validation calls | ~30 | 0 |
| CPU feature enum + flag mapping | ~30 | ~5 (string passthrough to GCC) |
| Instruction table includes/lookups | ~40 | 0 |

**Net: ~230 lines deleted, ~35 lines added = ~195 net deletion.**

### 10.7 Makefile entries

```
# Remove from CORE_SRCS and LIB_SRCS:
src/safety/asm_register_tables_x86_64.c
src/safety/asm_register_tables_x86_64_avx512f.c
src/safety/asm_register_tables_aarch64.c
src/safety/asm_register_tables_riscv64.c
src/safety/asm_register_lookup.c
src/safety/asm_categories.c
src/safety/asm_instruction_table_x86_64.c
src/safety/asm_instruction_table_aarch64.c
src/safety/asm_instruction_table_riscv64.c

# Remove targets:
gen-asm-tables:
    bash scripts/gen_register_tables.sh ...
    bash scripts/gen_instruction_table.sh ...
```

Plus any test invocations referencing the deleted infrastructure.

### 10.8 Obsolete planning docs

In `docs/asm_plan.md`, the following sections become historical-only or
deletable:

| Section | Status |
|---|---|
| "Session G Phase 5 implementation plan" | DELETE — was planning for deferred work |
| "Re-audit round 2 through 6" | DELETE — superseded by Level C |
| "8-category framework details" | KEEP as historical (don't grow) |
| "F4-F7 trajectory tables" | KEEP as historical |
| "Stage 4 status (2026-05-02)" | KEEP as historical — frozen snapshot |

Approximately 3,000 lines of planning content can be deleted (planning
artifacts for work that's now deferred).

### 10.9 Tests that reference deleted infrastructure

| Test file | What to do |
|---|---|
| `tests/zer_fail/asm_aarch64_x86_reg.zer` | Convert: still fails, just via GCC error instead of ZER |
| `tests/zer_fail/asm_riscv64_x86_reg.zer` | Same |
| `tests/zer/asm_avx512_register.zer` | Still works, validation by GCC -mavx512f flag |
| `tests/zer_fail/asm_avx512_no_flag.zer` | Still fails (GCC errors without -mavx512f) |
| `tests/zer/asm_simd_register.zer` | Still works |
| F7-light tests (LR/SC pairing) | Unchanged — F7-light stays |
| F7-full Step 2 tests (BSR/IDIV/MOVAPS) | Unchanged — hardcoded list does same |
| `tests/test_cross_arch.sh` | Adjust to not depend on probe scripts |

### 10.10 Total deletion summary

| Category | Lines |
|---|---|
| Instruction tables + schema | 374 |
| Register tables + lookup + schema | 716 |
| Categories framework | 252 |
| Probe scripts + candidates | 965 |
| Vendored arch data | 1,532 |
| Checker.c refactoring (net) | 195 |
| Obsolete planning docs in asm_plan.md | ~3,000 |
| **Total** | **~7,034 lines** |

After deletion: ZER's asm safety infrastructure is **~600 lines total**:
~500 lines of Z-rules (generic, frozen) + ~50 lines of F7-light (hardcoded
mnemonics) + ~30 lines of hardcoded UB classics (frozen) + Session A/B
parser/AST integration.

---

## 11. The Hardcoded UB Classics List

This is THE concrete addition Level C makes. ~12 frozen entries in
checker.c that catch well-known instruction UB at compile time.

### 11.1 Why hardcoded, not table

Previous design (Level A): ~100-entry implicit-precondition table with
lookup function, schema, citations.

Level C realization: even ~100 entries is overkill. The actual list of
"well-known UB-prone instruction classics" is much smaller. Hardcoding
in checker.c is simpler, faster, and there's no growth pressure
(these instructions are decades-old classics).

### 11.2 The list (concrete, frozen)

**Bit-search (UB on zero operand):**
- `bsr` — Bit Scan Reverse, x86 (1985)
- `bsf` — Bit Scan Forward, x86 (1985)

**Integer division (UB on zero divisor, plus signed overflow):**
- `div` — Unsigned divide, x86 (1978)
- `idiv` — Signed divide, x86 (1978, INT_MIN/-1 overflow)

**Aligned vector loads (SIGSEGV on misalignment):**
- `movaps` — Move Aligned Packed Single, x86 SSE (1999)
- `movapd` — Move Aligned Packed Double, x86 SSE2 (2000)
- `movdqa` — Move Double Quadword Aligned, x86 SSE2 (2000)
- `vmovaps`, `vmovapd`, `vmovdqa` — AVX variants (2011)

**Total: ~10-12 mnemonics.** Frozen since their introduction. No new
similar instructions added in 25+ years.

### 11.3 What's NOT in the list

The list is deliberately small. NOT included:

| Category | Why not |
|---|---|
| LR/SC pairing | Handled by F7-light separately (existing) |
| MONITOR/MWAIT pairing | Handled by F7-light separately (existing) |
| AMX TILECONFIG ordering | Niche, opt-in via comments |
| Privileged instructions (RDMSR/WRMSR/etc.) | User in `naked` + kernel module context already |
| Cache control (CLFLUSH/CLWB) | User-mode safe, no implicit constraint |
| Acquire/release pairing | Niche persistent-memory case, deferred |
| Shift count > width | C compiler handles |
| Most everything else | Either handled by existing systems or GCC catches it |

The list is the **minimal viable set of well-known UB classics** that
provide compile-time protection beyond what GCC catches.

### 11.4 Concrete code (replaces F7-full Step 2 table dispatch)

```c
/* In checker.c, NODE_ASM handler, ~30 lines */

typedef enum {
    UB_REQUIRES_NONZERO     = 1 << 0,
    UB_REQUIRES_ALIGN_16    = 1 << 1,
    UB_REQUIRES_ALIGN_32    = 1 << 2,
    UB_REQUIRES_ALIGN_64    = 1 << 3,
    UB_COMPOUND_INTMIN_NEG1 = 1 << 4,
} UbConstraint;

static const struct {
    const char *mnemonic;
    int operand_idx;
    uint32_t constraint;
    const char *citation;
} ub_classics[] = {
    { "bsr",     0, UB_REQUIRES_NONZERO,                            "Intel SDM Vol 2 BSR" },
    { "bsf",     0, UB_REQUIRES_NONZERO,                            "Intel SDM Vol 2 BSF" },
    { "div",     0, UB_REQUIRES_NONZERO,                            "Intel SDM Vol 2 DIV" },
    { "idiv",    0, UB_REQUIRES_NONZERO | UB_COMPOUND_INTMIN_NEG1,  "Intel SDM Vol 2 IDIV" },
    { "movaps",  0, UB_REQUIRES_ALIGN_16,                           "Intel SDM Vol 2 MOVAPS" },
    { "movapd",  0, UB_REQUIRES_ALIGN_16,                           "Intel SDM Vol 2 MOVAPD" },
    { "movdqa",  0, UB_REQUIRES_ALIGN_16,                           "Intel SDM Vol 2 MOVDQA" },
    { "vmovaps", 0, UB_REQUIRES_ALIGN_32,                           "Intel SDM Vol 2 VMOVAPS (256-bit)" },
    { "vmovapd", 0, UB_REQUIRES_ALIGN_32,                           "Intel SDM Vol 2 VMOVAPD (256-bit)" },
    { "vmovdqa", 0, UB_REQUIRES_ALIGN_32,                           "Intel SDM Vol 2 VMOVDQA (256-bit)" },
};

static void check_ub_classics(Checker *c, AsmNode *asm_node) {
    const char *mnemonic = first_word(asm_node->instructions);
    int mn_len = first_word_len(asm_node->instructions);
    
    for (size_t i = 0; i < ARRAY_SIZE(ub_classics); i++) {
        const char *e_mn = ub_classics[i].mnemonic;
        size_t e_len = strlen(e_mn);
        if (mn_len != (int)e_len) continue;
        if (memcmp(mnemonic, e_mn, e_len) != 0) continue;
        
        /* Found — apply constraints */
        uint32_t c_flags = ub_classics[i].constraint;
        int op_idx = ub_classics[i].operand_idx;
        AsmOperand *op = get_operand(asm_node, op_idx);
        if (!op) return;
        
        if (c_flags & UB_REQUIRES_NONZERO) {
            VarRange range;
            if (find_var_range(c, op->expr, &range)) {
                if (range.min <= 0 && range.max >= 0 && !range.known_nonzero) {
                    checker_error(c, asm_node->line,
                        "asm %s requires operand %d nonzero (%s):\n"
                        "    expression range [%lld, %lld] includes zero",
                        e_mn, op_idx, ub_classics[i].citation,
                        range.min, range.max);
                }
            }
        }
        
        if (c_flags & UB_REQUIRES_ALIGN_16) {
            int actual = expr_alignment(c, op->expr);
            if (actual < 16) {
                checker_error(c, asm_node->line,
                    "asm %s requires operand %d 16-byte aligned (%s):\n"
                    "    expression has alignment %d",
                    e_mn, op_idx, ub_classics[i].citation, actual);
            }
        }
        
        /* Similar for ALIGN_32, ALIGN_64, COMPOUND_INTMIN_NEG1 */
        return;
    }
}
```

**~50 lines including the list. Frozen.**

### 11.5 What this catches

- `naked void f(u32 x) { asm { instructions: "bsr %0, %1" outputs: { "rcx" = result } inputs: { "rax" = x } } }`
  — if `x` not provably nonzero → compile error
- `naked void f(*u8 buf) { asm { instructions: "movaps %0, %1" ... inputs: { "rax" = buf } } }`
  — if `buf` not provably 16-byte aligned → compile error
- `naked void f(i32 x) { asm { instructions: "idiv %0" inputs: { "rax" = x } } }`
  — if `x` not provably nonzero → compile error
- Compound INT_MIN/-1 backstopped at runtime trap (existing infrastructure)

**Same coverage as F7-full Step 2 table-driven dispatch, smaller code.**

---

## 12. Integration with Existing Safety Systems

Level C reuses existing ZER safety infrastructure. No new safety algorithms.

### 12.1 VRP integration (System #12)

The hardcoded UB classics list uses VRP for `REQUIRES_NONZERO`:
- `find_var_range(c, expr, &range)` → returns expression's known range
- Check `range.known_nonzero || range.min > 0 || range.max < 0`
- Error if range includes zero

Same VRP that catches `arr[0]` with `arr` size > 0 catches BSR-on-zero.

### 12.2 Alignment integration

`REQUIRES_ALIGN_N` uses existing alignment infrastructure:
- `expr_alignment(c, expr)` → returns provably known alignment
- Same logic that handles `_Alignas`, `@aligned_alloc`, MMIO alignment

### 12.3 Escape analysis (System #11)

Operand expressions go through escape analysis via Z-rules Z3-Z5, Z11.
Pointer operands tracked the same as regular pointer expressions.

### 12.4 Provenance tracking (Systems #3-#5)

Operand expressions go through provenance via Z-rules Z4-Z5.
Same as regular ZER code.

### 12.5 Handle/move tracking (Systems #7, #10)

Handle and move-struct operands go through Z-rules Z1, Z2.
Same as regular ZER code.

### 12.6 MMIO range checking (System #19)

Memory operand addresses go through existing MMIO infrastructure via Z7.
Same as regular `@inttoptr` validation.

### 12.7 Qualifier preservation (System #20)

Volatile/const tracking through asm operands via Z-rule Z8.
Same as regular qualifier checking.

### 12.8 Summary: zero new safety logic

Level C's work is:
- Delete files
- Refactor F7-full Step 2 dispatch to use hardcoded list (~30 lines)
- Update Makefile

**No new safety algorithms.** Every check uses existing systems.

---

## 13. Safety Coverage Matrix — Level A vs B vs C

Side-by-side comparison.

### 13.1 Memory safety through asm operands

| Property | Level A | Level B | Level C |
|---|---|---|---|
| OOB through operand | ✓ via VRP | ✓ via VRP | ✓ via VRP |
| UAF through operand | ✓ (Z1) | ✓ (Z1) | ✓ (Z1) |
| Use-after-move | ✓ (Z2) | ✓ (Z2) | ✓ (Z2) |
| Escape | ✓ (Z3-Z5) | ✓ (Z3-Z5) | ✓ (Z3-Z5) |
| Provenance | ✓ (Z4-Z5) | ✓ (Z4-Z5) | ✓ (Z4-Z5) |
| Qualifier preservation | ✓ (Z8) | ✓ (Z8) | ✓ (Z8) |
| Keep param violations | ✓ (Z11) | ✓ (Z11) | ✓ (Z11) |
| MMIO range | ✓ via @inttoptr | ✓ via @inttoptr | ✓ via @inttoptr |

**Identical across all levels.** Z-rules unchanged.

### 13.2 Instruction-specific UB

| Property | Level A | Level B | Level C |
|---|---|---|---|
| BSR/BSF on zero | ✓ implicit table | ✓ implicit table | ✓ hardcoded list |
| IDIV on zero | ✓ implicit table | ✓ implicit table | ✓ hardcoded list |
| MOVAPS misaligned | ✓ implicit table | ✓ implicit table | ✓ hardcoded list |
| LR/SC pairing | ✓ F7-light | ✓ F7-light | ✓ F7-light |
| Privileged misuse | ✓ implicit table | ✓ implicit table | Defer to GCC |

Level C drops some niche-but-not-zero coverage (privileged context check)
in exchange for simplicity. The "loss": GCC errors instead of ZER errors.

### 13.3 ISA-specific validation

| Property | Level A | Level B | Level C |
|---|---|---|---|
| Register name valid for arch | ✓ ZER tables | Defer to GCC | Defer to GCC |
| Register width match | ✓ ZER tables | Defer to GCC | Defer to GCC |
| Register class match | ✓ ZER tables | ✓ via annotations | Defer to GCC |
| CPU feature gating | ✓ ZER enum | ✓ ZER enum | Defer to GCC -m flags |
| Instruction valid for arch | ✓ ZER tables | Defer to GCC | Defer to GCC |

**Level C defers ALL ISA-specific to GCC.** Errors still happen at compile
time, just from GCC instead of ZER.

### 13.4 Out-of-scope (all levels)

| Property | All levels |
|---|---|
| Wrong specification (user error) | ✗ |
| Algorithm correctness | ✗ |
| Microarchitectural (Spectre etc.) | ✗ |
| Runtime CPU state | ✗ |

### 13.5 Effort and maintenance

| Property | Level A | Level B | Level C |
|---|---|---|---|
| Effort to ship | ~3-4 months | ~3-4 months | **~1-2 days** |
| Lines deleted | ~5,600 | ~6,700 | **~7,000** |
| Existing code preserved | ~95% | ~85% | ~80% (intrinsics + Z-rules unchanged) |
| Maintenance per ISA extension | Low (probe rerun) | Lower | **Zero** |
| CPU feature additions/year | ~2-3 | ~2-3 | Zero |
| Architecture coverage | 3 archs | 3 archs (registers via GCC) | **All GCC archs (15+)** |
| Sole-developer friendliness | OK | Better | **Best** |

### 13.6 Summary

For ZER's specific situation (sole developer, multi-architecture goal,
transpiler design, GCC audience), **Level C is the clear winner**:
- Same safety coverage as A/B
- Less code
- Zero maintenance
- More architectures supported

---

## 14. Production-Compiler Precedents

### 14.1 Rust's `asm!()` macro

What Rust does:
- Structured operand bindings: `out("reg") result`, `in("reg") input`
- Type checking on bound expressions
- Register CLASS validation (`reg` = any GPR, `reg_byte` = byte reg)
- NO per-arch register NAME validation
- CPU feature flags via `#[cfg(target_feature = "...")]`
- NO per-instruction UB detection
- `unsafe` block boundary
- `// SAFETY:` comment convention

**Level C match:** ZER drops register name validation (same as Rust). ZER
drops per-instruction enum-based feature gating (delegates to GCC -m flags,
similar effect). ZER keeps ~12 hardcoded UB classics (slight safety bonus
over Rust). Naked-only restriction is stricter than Rust's `unsafe`.

### 14.2 Zig's `asm` keyword

What Zig does:
- `asm volatile (template : outputs : inputs : clobbers)` (GCC-style)
- Type checking on bound expressions
- NO per-arch register name validation
- NO per-instruction UB detection

**Level C match:** Essentially the same. Zig defers to LLVM's assembler.
ZER defers to GCC.

### 14.3 seL4 (proven kernel)

What seL4 does:
- Hand-written asm with hand-proven invariants
- Isabelle/HOL formal model of ISA semantics (not in compiler)
- No compile-time asm validation in their build chain

**Level C match:** seL4 is the maximum-safety reference but at proof time,
not compile time. Compile-time asm = "trust hand-proven invariants."
ZER's Level C trust pattern is similar but lighter (compiler enforces what
it can, defers ISA stuff, user takes responsibility for the rest).

### 14.4 GCC's inline asm (the foundation)

What GCC's assembler does (Level C delegates to this):
- Register name validation per target
- Instruction validity per target
- CPU feature gating via `-m` flags
- Operand encoding
- Assembly output
- Cross-toolchain coverage (15+ architectures)

**Level C reality:** This is the validation layer ZER didn't need to
reimplement. GCC does it for every architecture. ZER's transpiler design
makes this delegation natural.

### 14.5 Convergent design

All three production compilers (Rust, Zig, seL4) and the underlying
toolchain (GCC) converge on:
- Validate operand boundary (types, classes)
- Defer instruction validation to assembler
- Provide escape hatch for raw asm
- Optional formal proofs for safety-critical (Vale, seL4)

**Level C brings ZER fully into this convergent design.** Better than A
(which tried to validate per-instruction). Aligned with B (defers register
names) but goes further by also deferring CPU features.

---

## 15. Effort Breakdown — 6 Commits

Level C execution: 6 commits, ~1-2 days.

### Commit 1: Refactor F7-full Step 2 to hardcoded UB classics list (~2-3 hours)

**Goal:** Eliminate dependency on instruction tables for safety checks.

Steps:
1. In `checker.c`, add the hardcoded `ub_classics[]` array (~30 lines)
2. Add `check_ub_classics()` function that dispatches to existing VRP / alignment infrastructure
3. Replace existing F7-full Step 2 dispatch code (which reads instruction tables) with calls to `check_ub_classics()`
4. Verify all F7-full Step 2 tests still pass (BSR-on-zero, IDIV-on-zero, MOVAPS-misaligned, etc.)

**Files touched:** `checker.c` only.
**Lines:** +50 added, ~80 deleted = -30 net.
**Risk:** LOW. Same logic, smaller surface.

### Commit 2: Delete instruction tables and probe script (~1 hour)

**Goal:** Remove the instruction-level database.

Steps:
1. Delete `src/safety/asm_instruction_table_x86_64.c`
2. Delete `src/safety/asm_instruction_table_aarch64.c`
3. Delete `src/safety/asm_instruction_table_riscv64.c`
4. Delete `src/safety/asm_instruction_table.h`
5. Delete `scripts/gen_instruction_table.sh`
6. Delete `arch_data/` directory entirely (4 files: x86_64.zerdata, aarch64.zerdata, riscv64.zerdata, SCHEMA.md)
7. Update Makefile: remove from CORE_SRCS, LIB_SRCS, remove `gen-asm-tables` target
8. Run `make docker-check` — all tests still pass (Commit 1 made checker independent of these)

**Files touched:** Delete 7+ files, modify Makefile.
**Lines:** -1,906 deleted (374 + 1,532).
**Risk:** LOW. Commit 1 already made checker independent.

### Commit 3: Delete register tables and lookup (~2 hours)

**Goal:** Defer register name validation to GCC.

Steps:
1. In `checker.c`, find and remove register name validation calls
2. Add note: "register name validation deferred to GCC; ZER validates operand types and class via existing systems"
3. Delete `src/safety/asm_register_tables_x86_64.c`
4. Delete `src/safety/asm_register_tables_x86_64_avx512f.c`
5. Delete `src/safety/asm_register_tables_aarch64.c`
6. Delete `src/safety/asm_register_tables_riscv64.c`
7. Delete `src/safety/asm_register_lookup.c`
8. Delete `src/safety/asm_register_tables.h`
9. Delete `scripts/gen_register_tables.sh`
10. Delete `scripts/candidates_*.txt` (3 files)
11. Update Makefile
12. Update tests that previously checked register-name errors — they now expect GCC errors at the assembly stage

**Files touched:** Delete 10+ files, modify Makefile, update some tests.
**Lines:** -1,716 deleted (716 + 124 + 468 + 408 misc).
**Risk:** MEDIUM. Test updates needed. Some test assertions change from "ZER error: ..." to "GCC error: ...".

### Commit 4: Delete asm_categories framework and Session G plumbing (~1 hour)

**Goal:** Remove dead-weight infrastructure with no enforcement.

Steps:
1. Delete `src/safety/asm_categories.c`
2. Delete `src/safety/asm_categories.h`
3. In `checker.c`, remove any references to category framework
4. Remove `ZerBarrierKind` enum and `ZerOrderingRole` enum (Session G plumbing)
5. Remove ordering-related fields from any remaining structs
6. Update Makefile
7. Run tests — verify nothing broke (no enforcement was active)

**Files touched:** Delete 2 files, modify `checker.c`, modify Makefile.
**Lines:** -302 deleted (252 + 50).
**Risk:** LOW. No active enforcement to break.

### Commit 5: Remove CPU feature enum, switch to GCC -m flag passthrough (~2 hours)

**Goal:** Defer CPU feature gating to GCC.

Steps:
1. In `checker.c`, find the CPU feature enum (`ZerCpuFeature`) and flag parsing
2. Replace with a simpler `--target-features=foo,bar,baz` flag parser that just collects the names
3. In emitter or zerc_main, pass each as `-mfoo`, `-mbar`, etc. to GCC
4. Remove the per-feature enum and bitmap
5. Remove operand `cpu_feature:` annotation processing (not in Level C scope)
6. Update tests:
   - Tests that used `--target-features=avx512f` to allow `xmm16` register → adjust to verify GCC accepts with the flag and rejects without
7. Documentation update for the simplified flag

**Files touched:** `checker.c`, `zerc_main.c`, possibly some tests.
**Lines:** -30 deleted, +10 added (string passthrough) = -20 net.
**Risk:** LOW-MEDIUM. Test assertions change.

### Commit 6: Clean up obsolete planning docs and update README/CLAUDE.md (~1-2 hours)

**Goal:** Reflect Level C reality in documentation.

Steps:
1. In `docs/asm_plan.md`, delete obsolete planning sections:
   - "Session G Phase 5 implementation plan" subsections
   - "Re-audit round 2-6"
   - Detailed F4-F7 trajectory planning (keep status snapshot)
   - 8-category framework details (keep brief reference)
2. Keep historical context (Sub-Extension Architecture validation, decision log)
3. Update `CLAUDE.md` Stage 4 section to reflect Level C completion
4. Update `docs/asm_lang_zer_safe.md` if any tweaks needed
5. Update `docs/limitations.md` to reflect Level C scope
6. Update `BUGS-FIXED.md` with the cleanup record

**Files touched:** Docs only.
**Lines:** ~3,000 lines deleted from asm_plan.md.
**Risk:** LOW. Documentation only.

### Total

| Commit | Effort | Lines net | Risk |
|---|---|---|---|
| 1. Refactor to hardcoded list | 2-3 hrs | -30 | LOW |
| 2. Delete instruction tables | 1 hr | -1,906 | LOW |
| 3. Delete register tables | 2 hrs | -1,716 | MEDIUM |
| 4. Delete asm_categories + Session G | 1 hr | -302 | LOW |
| 5. CPU feature passthrough | 2 hrs | -20 | LOW-MED |
| 6. Doc cleanup | 1-2 hrs | -3,000 | LOW |
| **Total** | **9-12 hrs (1-2 days)** | **~-7,000** | **LOW** |

After completion: **TRULY zero maintenance** going forward.

---

## 16. Implementation Strategy

### 16.1 Sequencing principle

Each commit must leave the codebase in a working state. Tests pass after
every commit. The sequence is:

1. **First, make checker.c independent of the things we're deleting**
   (Commit 1 — refactor F7-full Step 2 to hardcoded list)
2. **Then delete the now-unused infrastructure** (Commits 2, 3, 4)
3. **Then simplify what remains** (Commit 5 — CPU feature passthrough)
4. **Finally clean up docs** (Commit 6)

This ordering ensures no broken intermediate state.

### 16.2 Per-commit checklist

For each commit:
- [ ] Make changes
- [ ] `make docker-build` succeeds
- [ ] `make docker-check` passes (all tests)
- [ ] `bash tools/walker_audit.sh` passes
- [ ] `bash tools/agreement_audit.sh` shows zero disagreements
- [ ] Commit with descriptive message
- [ ] No push until all 6 commits ready

### 16.3 Atomic by design

If any commit fails (build broken, test regression), revert and investigate
before proceeding. Each commit is independently revertable.

### 16.4 Validation milestones

After Commit 1: F7-full Step 2 tests still pass via hardcoded list
After Commit 2: Build still works without instruction tables
After Commit 3: Build still works without register tables; expected test assertion changes
After Commit 4: Build still works without categories framework
After Commit 5: Build still works with CPU feature passthrough
After Commit 6: Final docs accurate, all tests pass

### 16.5 Push as one batch

After all 6 commits succeed locally:
1. Final `make docker-check` to ensure full suite passes
2. `git push origin main` (push all 6 commits)
3. Verify GitHub CI passes
4. Tag if appropriate

---

## 17. Testing Strategy

### 17.1 Existing test coverage that must keep working

All existing tests must pass after cleanup. Specifically:

**F7-full Step 2 tests** (now driven by hardcoded UB classics list):
- `tests/zer_fail/asm_bsr_zero.zer` — BSR with unprovable nonzero
- `tests/zer_fail/asm_idiv_zero.zer` — IDIV with unprovable nonzero
- `tests/zer_fail/asm_movaps_misalign.zer` — MOVAPS with unprovable alignment
- Positive equivalents that compile clean

**F7-light tests** (unchanged):
- `tests/zer_fail/asm_lrsc_unbalanced.zer`
- `tests/zer_fail/asm_lrsc_orphan_sc.zer`
- Positive equivalents

**Z-rule tests** (unchanged):
- `tests/zer_fail/asm_uaf.zer`
- `tests/zer_fail/asm_move_transfer.zer`
- etc.

### 17.2 Test assertion updates (Commit 3 specifically)

Tests that previously expected ZER errors for register-name violations
now expect GCC errors at the assembly stage. Update:

```bash
# Before (Level A):
$ zerc test.zer
test.zer:5: asm: register 'rax' not valid on aarch64 target

# After (Level C):
$ zerc test.zer  # → emits C
$ aarch64-linux-gnu-gcc test.c
<inline asm>:1: Error: unknown register name 'rax'
```

Test files updated:
- `tests/zer_fail/asm_aarch64_x86_reg.zer` — adjust expected output
- `tests/zer_fail/asm_riscv64_x86_reg.zer` — adjust expected output
- `tests/test_cross_arch.sh` — adjust assertion patterns

### 17.3 Cross-arch test changes (Commit 5)

Tests that use CPU features now rely on GCC's `-m` flag enforcement:

```bash
# Before (Level A):
zerc test.zer --target-features=avx512f  # checker validates

# After (Level C):
zerc test.zer --target-features=avx512f  # passes through to GCC as -mavx512f
# GCC's assembler validates xmm16 usage matches -mavx512f
```

### 17.4 Build verification

- Confirm `make docker-build` works without the deleted files
- Confirm zerc binary still functions
- Confirm all 130 intrinsics still emit correctly
- Confirm Z-rules still fire

### 17.5 Regression suite

Full test suite: ~5,000+ tests including:
- `tests/test_zer.sh` (positive + negative ZER tests)
- `rust_tests/run_tests.sh` (786 Rust equivalents)
- `zig_tests/run_tests.sh` (36 Zig equivalents)
- `test_modules/run_tests.sh` (28 multi-module)
- C unit tests (~95)
- Semantic fuzzer (200/run)
- Cross-arch tests

All must pass after Commit 6.

---

## 18. Risks and Mitigations

### 18.1 Risk: Test assertion changes break CI

**Scenario:** Tests checking "ZER error: register 'rax' invalid on aarch64"
now see "GCC error: unknown register".

**Mitigation:** Update test assertions in Commit 3 to expect GCC errors.
Test framework adjustment, not safety regression.

### 18.2 Risk: F7-light LR/SC pairing breaks due to dependency removal

**Scenario:** F7-light hardcoded mnemonics depend on something we're deleting.

**Mitigation:** Verify F7-light has NO dependencies on instruction tables
(should be self-contained mnemonic-string matching). Verify in Commit 1
that F7-light tests still pass after refactor.

### 18.3 Risk: User loses CPU feature compile-time check

**Scenario:** User types `--target-features=avx512`. ZER passes to GCC as
`-mavx512`. GCC errors "unknown option `-mavx512`" (should have been
`-mavx512f`).

**Mitigation:** This is a typo at runtime; user fixes it. GCC's error is
clear. No safety regression — user can't compile with wrong flags either way.
Alternatively, can keep a small list of valid GCC `-m` flags for hint
generation (~30 lines, frozen). Defer that decision.

### 18.4 Risk: Some legitimate asm pattern breaks

**Scenario:** Existing valid asm test stops compiling for some reason.

**Mitigation:** Each commit tested with full suite. Atomic revertability.
Investigate and fix or revert before proceeding.

### 18.5 Risk: Sub-extension support degrades

**Scenario:** AVX-512 user can't use `xmm16` anymore.

**Mitigation:** They can. They pass `--target-features=avx512f` →
`-mavx512f` → GCC's assembler accepts `xmm16`. Same as before, just GCC
enforces instead of ZER.

### 18.6 Risk: Loss of probe-script automation

**Scenario:** Future ISA extension ships, register tables need updating.

**Mitigation:** At Level C, ZER doesn't track register tables at all.
GCC tracks them. When new ISA extensions ship, GCC adds support; ZER
inherits automatically. This is a feature of Level C, not a risk.

### 18.7 Risk: User-facing error messages change

**Scenario:** Some errors become GCC errors instead of ZER errors. Less
integrated with ZER's error formatting.

**Mitigation:** Document this clearly. Users understand that asm errors
come from GCC. The error message is still actionable. This is also Rust's
model — Rust's `asm!()` errors come partially from LLVM.

---

## 19. Open Questions Decided

### 19.1 Q: Level A, B, or C? → DECIDED: Level C

Per the analysis in section 7. Level C dominates for ZER's specific situation
(sole dev, multi-arch, transpiler design, GCC audience).

### 19.2 Q: Are GCC error messages clear enough? → YES

GCC's assembler error messages are well-established and actionable. The
slight UX delta vs ZER-formatted messages is acceptable for the
maintenance savings.

### 19.3 Q: Will users miss compile-time CPU feature gating in ZER? → NO

Users pass `-mavx512f` flag. GCC validates. Same outcome.

### 19.4 Q: What about Keil/IAR/MSVC users? → NOT ZER'S AUDIENCE

Documented explicitly. Future work can add `--emit-target=` modes if real
demand emerges. Not blocking Level C.

### 19.5 Q: What if a future ISA has well-known UB beyond the ~12 classics? → ADD ENTRY

The hardcoded UB classics list is small but extensible (just add an entry
in checker.c). Maintenance: ~5 lines per addition. Frequency: maybe once
per decade (most ISAs don't ship genuinely new UB classes).

### 19.6 Q: Should Session G Phase 5 ever happen? → NO

Deferred indefinitely. Niche (persistent memory + concurrency). Users
can use `requires:`-style annotations if Level D is ever pursued.

### 19.7 Q: Are we sure register tables aren't needed? → YES

Validated extensively. Rust doesn't have them. Zig doesn't have them.
GCC's assembler validates register names. The only "loss" is error message
formatting, not safety.

---

## 20. Out of Scope (Explicitly NOT Doing)

### 20.1 NOT doing: Full operand-kind IR rewrite

Sketched in earlier sessions as Option 1. ~8-12 months, no user-visible
benefit. Rejected.

### 20.2 NOT doing: Multi-backend support (LLVM, MSVC)

Not ZER's audience. ZER → GNU C (works with GCC and Clang). Future
`--emit-target=` modes possible but not planned.

### 20.3 NOT doing: Direct asm-byte emission

GCC handles assembly. No reinvention.

### 20.4 NOT doing: User-extensible UB classics list

The list is compiler-internal. Users who need specific instruction
preconditions can use `naked` function with `// SAFETY:` comment.

### 20.5 NOT doing: Vale-tier formal proofs

Algorithm correctness is opt-in, separate dimension. Deferred to v2.x+.
Not part of Level C.

### 20.6 NOT doing: Per-instruction operand annotations (Option 2 / Level A)

The annotation-driven approach was an intermediate design (Level A). Level
C simplifies further by deferring to GCC. Annotations are not added.

### 20.7 NOT doing: Session G Phase 5 / Z9-Z13 forward-compat

Deferred indefinitely. Niche, complex, hard to get right (Phase 3 lesson).

### 20.8 NOT doing: Probe-script ecosystem (Capstone/XED integration)

Deleted at Level C. Not coming back.

---

## 21. The "Smart Language vs Smart Compiler" Principle

### 21.1 The principle (one sentence)

**Compilers should be generic algorithms over annotated languages, not
databases of facts about specific entities.**

### 21.2 Level C as full application of the principle

At Level C, ZER's asm safety architecture is:

```
LANGUAGE (smart, frozen vocabulary):
  - naked function attribute
  - structured asm { } syntax
  - typed operand bindings
  - 130 intrinsics (fixed set)
  
COMPILER (generic, frozen algorithms):
  - Z-rules (existing safety systems applied to operands)
  - F7-light state machine (hardcoded LR/SC, ~50 lines)
  - Hardcoded UB classics list (~12 entries, frozen)
  
DELEGATION (defer to other tools):
  - Register name validation → GCC
  - Instruction validity → GCC
  - CPU feature gating → GCC -m flags
  - Sub-extension support → GCC
```

**No per-item databases. No probe scripts. No data files. No category
frameworks. No CPU feature enums. No instruction tables.**

The compiler stays generic. The language is frozen vocabulary. ISA
specifics defer to whoever owns that domain (GCC).

### 21.3 The general rule (reiterated)

If you're tempted to add per-X knowledge to the compiler:

```
1. Identify the property you're encoding
2. Either:
   a. Smart language — design a frozen annotation expressing it
   b. Smart delegation — defer to a more authoritative tool
   c. Hardcoded frozen list — small fixed set of well-known cases
3. Don't grow a per-X database
```

Level C applies (b) for ISA-specific things and (c) for ~12 well-known
UB classics. (a) — language annotations — was Level A's approach,
ultimately not needed because (b) covers the same cases.

---

## 22. Fresh-Session Onboarding Checklist

### 22.1 Read these documents in order

1. **This document end-to-end** (~30 min)
2. `docs/asm_plan.md` warning block + Sub-Extension Architecture section
   (~10 min) — historical context
3. `CLAUDE.md` Stage 4 section — current asm status

### 22.2 Verify current state

```bash
# Confirm 130 intrinsics implemented
grep -E "@cpu_|@atomic_|@barrier|@bswap|@popcount|@ctz|@clz" src/builtins.c | wc -l

# Confirm Z-rules wired (Z1-Z8, Z11, Z12 — 10 of 13)
grep -E "^/\* Z[0-9]+:" checker.c zercheck_ir.c | wc -l

# Confirm tests pass
make docker-check
```

### 22.3 Understand the architectural principle

Read section 21 of this document. The "smart language, generic compiler"
principle is the foundation.

### 22.4 Understand the delegation pattern

At Level C, ZER's asm safety is divided into:
- **What ZER validates:** memory/type/concurrency safety, operand types,
  well-known UB classics, LR/SC pairing
- **What GCC validates:** register names, instruction validity, CPU
  features, assembly syntax, ISA-specific stuff

ZER doesn't try to be smart about ISA-specific things. GCC is. ZER
trusts GCC for that layer.

### 22.5 Execute the cleanup (if not yet done)

Sequence per section 16:
1. Commit 1: Refactor F7-full Step 2 to hardcoded UB classics list
2. Commit 2: Delete instruction tables
3. Commit 3: Delete register tables and lookup
4. Commit 4: Delete asm_categories and Session G plumbing
5. Commit 5: Remove CPU feature enum, switch to GCC -m passthrough
6. Commit 6: Clean up obsolete planning docs

Total: ~1-2 days.

### 22.6 Decision checkpoints (these are LOCKED IN)

Don't revisit these unless explicit user direction:

- Naked-only restriction (S1) is permanent
- GCC is the C compiler dependency
- Per-instruction database not coming back
- Register tables not coming back
- CPU feature enum not coming back
- Probe scripts not coming back
- Smart-language principle applies
- ~12 hardcoded UB classics is the implicit table

### 22.7 Anti-patterns to avoid

- DON'T add per-instruction safety knowledge to ZER
- DON'T add register name validation to ZER
- DON'T add CPU feature enum to ZER
- DON'T extend the hardcoded UB classics list beyond well-known classics
- DON'T add Z14, Z15, etc. (annotation framework was Level A's idea, not used)
- DON'T resurrect Session G Phase 5
- DON'T claim "100% asm safety" — claim what's true (see section 2)

---

## Appendix A: Concrete File-by-File Deletion List

For implementation reference, here's every file affected by Level C.

### A.1 Files to DELETE entirely

```
src/safety/asm_instruction_table_x86_64.c       (75 lines)
src/safety/asm_instruction_table_aarch64.c      (59 lines)
src/safety/asm_instruction_table_riscv64.c      (52 lines)
src/safety/asm_instruction_table.h               (188 lines)
src/safety/asm_register_tables_x86_64.c          (121 lines)
src/safety/asm_register_tables_x86_64_avx512f.c  (177 lines)
src/safety/asm_register_tables_aarch64.c          (74 lines)
src/safety/asm_register_tables_riscv64.c         (147 lines)
src/safety/asm_register_lookup.c                  (81 lines)
src/safety/asm_register_tables.h                 (116 lines)
src/safety/asm_categories.c                      (181 lines)
src/safety/asm_categories.h                       (71 lines)
scripts/gen_instruction_table.sh                 (373 lines)
scripts/gen_register_tables.sh                   (124 lines)
scripts/candidates_x86_64.txt                    (255 lines)
scripts/candidates_aarch64.txt                    (63 lines)
scripts/candidates_riscv64.txt                   (150 lines)
arch_data/x86_64.zerdata                         (526 lines)
arch_data/aarch64.zerdata                        (363 lines)
arch_data/riscv64.zerdata                        (323 lines)
arch_data/SCHEMA.md                              (320 lines)
```

Plus delete `arch_data/` directory entirely after files removed.

**Total: 21 files, ~3,840 lines deleted.**

### A.2 Files to MODIFY

```
checker.c — refactor F7-full Step 2 dispatch, remove register validation
            calls, remove CPU feature enum
zerc_main.c — simplify --target-features= to passthrough
Makefile — remove CORE_SRCS/LIB_SRCS entries for deleted files,
           remove gen-asm-tables target
docs/asm_plan.md — delete obsolete planning sections (~3,000 lines)
CLAUDE.md — update Stage 4 to reflect Level C completion
docs/limitations.md — update asm scope
docs/asm_lang_zer_safe.md — this file, finalize Level C reality
BUGS-FIXED.md — append cleanup record
tests/zer_fail/asm_aarch64_x86_reg.zer — update expected output
tests/zer_fail/asm_riscv64_x86_reg.zer — update expected output
tests/test_cross_arch.sh — adjust assertion patterns
```

### A.3 Files to KEEP unchanged

```
src/safety/handle_state.c, .h                    (Z-rules infrastructure)
src/safety/range_checks.c, .h                    (Z-rules infrastructure)
src/safety/type_kind.c, .h                       (Z-rules infrastructure)
src/safety/coerce_rules.c, .h                    (Z-rules infrastructure)
src/safety/context_bans.c, .h                    (Z-rules infrastructure)
src/safety/escape_rules.c, .h                    (Z-rules infrastructure)
src/safety/provenance_rules.c, .h                (Z-rules infrastructure)
src/safety/mmio_rules.c, .h                      (Z-rules infrastructure)
... (all other Z-rule extracted predicates)
```

These provide the safety primitives Z-rules dispatch to. Unchanged.

```
src/builtins.c                                    (130 intrinsics)
parser.c                                         (Session A/B asm syntax)
ast.c, ast.h                                     (AsmNode, operand bindings)
emitter.c                                        (GCC inline asm emission)
zercheck_ir.c                                    (Z-rules implementation)
```

These provide the asm framework. Unchanged (Level C is just deletion).

---

## Appendix B: The Hardcoded UB Classics — Concrete Contents

Final concrete list. Frozen reference for implementation.

### B.1 Bit-search (UB on zero operand)

| Mnemonic | Arch | Constraint | Citation |
|---|---|---|---|
| `bsr` | x86 | NONZERO on operand 0 | Intel SDM Vol 2 BSR |
| `bsf` | x86 | NONZERO on operand 0 | Intel SDM Vol 2 BSF |

Note: `lzcnt`, `tzcnt`, `popcnt` are well-defined on zero, no constraint.

### B.2 Integer division (UB on zero divisor, signed overflow)

| Mnemonic | Arch | Constraint | Citation |
|---|---|---|---|
| `div` | x86 | NONZERO on operand 0 | Intel SDM Vol 2 DIV |
| `idiv` | x86 | NONZERO on operand 0; INT_MIN/-1 backstop | Intel SDM Vol 2 IDIV |

### B.3 Aligned vector loads (SIGSEGV on misaligned)

| Mnemonic | Arch | Constraint | Citation |
|---|---|---|---|
| `movaps` | x86 SSE | ALIGN_16 on memory operand | Intel SDM Vol 2 MOVAPS |
| `movapd` | x86 SSE2 | ALIGN_16 on memory operand | Intel SDM Vol 2 MOVAPD |
| `movdqa` | x86 SSE2 | ALIGN_16 on memory operand | Intel SDM Vol 2 MOVDQA |
| `vmovaps` | x86 AVX (256-bit) | ALIGN_32 on memory operand | Intel SDM Vol 2 VMOVAPS |
| `vmovapd` | x86 AVX (256-bit) | ALIGN_32 on memory operand | Intel SDM Vol 2 VMOVAPD |
| `vmovdqa` | x86 AVX (256-bit) | ALIGN_32 on memory operand | Intel SDM Vol 2 VMOVDQA |

Note: 512-bit AVX-512 equivalents (`vmovaps zmm0...`) could be added as
ALIGN_64 if real users hit issues. Defer until needed.

### B.4 Total

**10 entries.** Frozen. Could grow to ~12-15 if AVX-512 alignment variants
or ARM/RISC-V well-known UB instructions are added based on user demand.
No automatic growth — manual addition per discovered need.

### B.5 What's NOT in the list and why

**LR/SC pairing** — handled by F7-light separately, not in this table.
**Privileged instructions (RDMSR/WRMSR etc.)** — user in naked function
takes responsibility; GCC handles privilege-level errors.
**Cache management (CLFLUSH/CLWB)** — user-mode safe; no compile-time
constraint needed.
**Atomic primitives (LOCK prefix, etc.)** — well-defined; GCC handles.
**Shift count > width** — C language handles (UB in C, ZER `_zer_shl`
runtime check applies).
**Most x86/ARM/RISC-V instructions** — well-defined, GCC handles, ZER
type system handles operand types.

---

## Appendix C: What ZER's Public Safety Claim Becomes

### C.1 Pre-pivot claim (asm_plan original)

> "ZER's asm safety: 100% language-safe via 13 Z-rules + 8 universal
> precondition categories + System #30 atomic ordering. Per-instruction
> preconditions enforced for ~120 instructions across x86_64, aarch64,
> riscv64. Strict mode catches UB at compile time."

**Problems:**
- "100% language-safe" oversold
- "Per-instruction preconditions" commits to perpetual maintenance
- Specifies 3 archs (limits architecture support)

### C.2 Level A claim (intermediate)

> "ZER's asm safety: operand-type checking + Z-rules + 130 intrinsics +
> operand-level precondition annotations + ~100 well-known UB classics
> auto-protected + per-arch register validation + CPU feature gating
> for x86_64, aarch64, riscv64. Annotations enforced via existing safety
> infrastructure."

**Still has problems:**
- Commits to per-arch maintenance (register tables)
- Commits to CPU feature enum
- Still 3 archs specifically

### C.3 Level C claim (FINAL)

> "ZER's asm safety: operand-type checking via existing ZER type system +
> Z-rules (memory/type/concurrency/MMIO safety through asm operands) +
> 130 intrinsics for common kernel patterns + hardcoded protection for
> well-known UB classics (BSR-on-zero, IDIV-on-zero, MOVAPS-misaligned,
> LR/SC pairing) + naked-fn isolation (MISRA Dir 4.3). Register
> validation, instruction validity, and CPU feature gating delegated to
> the user's C compiler. Works on every architecture GCC supports."

**Why this is better:**
- Honest about what ZER actually does
- Honest about what ZER defers
- "Every architecture GCC supports" is stronger than "3 archs"
- No commitment to per-arch maintenance
- Matches Rust/Zig design with small bonus (UB classics)

### C.4 Comparison with Rust/Zig public claims

**Rust `asm!()` documentation says:**
> "Type-checked operand bindings, register class validation, CPU feature
> via #[cfg]. Unsafe block boundary. Inline asm contents are not validated;
> use // SAFETY: comments."

**Zig `asm` documentation says:**
> "GCC-style inline assembly with typed operand bindings. Operand types
> verified. Assembly content compiled by LLVM."

**ZER Level C claim:**
> "Same operand-type checking as Rust/Zig + 130 intrinsics + ~12 hardcoded
> UB classics + Z-rules extending ZER safety through asm operands + naked-fn
> isolation. Works on every GCC-supported architecture."

**ZER is slightly stronger than Rust/Zig in:**
- Hardcoded UB classics (compile-time BSR-on-zero etc.)
- Z-rules extending safety through asm operands (UAF, escape, MMIO etc.)
- Naked-only restriction (MISRA Dir 4.3, stricter than `unsafe`)

**ZER is equivalent to Rust/Zig in:**
- Operand type checking
- Defer-to-assembler for register/instruction validation

**This is an honest, defensible position.**

---

## Appendix D: Mapping from asm_plan to Level C

Cross-reference: what asm_plan.md item becomes what in Level C.

### D.1 asm_plan items that STAY (already shipped, working)

| asm_plan item | Status in Level C |
|---|---|
| 130 intrinsics | STAY |
| Z1-Z8, Z11, Z12 | STAY |
| Session A asm syntax | STAY |
| Session B typed bindings | STAY |
| Naked-only (S1) | STAY |
| Sub-extension architecture decisions | STAY (locked in) |
| F7-light LR/SC | STAY |

### D.2 asm_plan items that GET DELETED

| asm_plan item | Status in Level C |
|---|---|
| F2 x86_64 register table | DELETED — defer to GCC |
| F5 aarch64 register table | DELETED — defer to GCC |
| F6 riscv64 register table | DELETED — defer to GCC |
| F4 instruction table x86_64 | DELETED |
| F4 instruction table aarch64 | DELETED |
| F4 instruction table riscv64 | DELETED |
| F1a 8-category framework | DELETED |
| F2 build-time-gen pipeline (registers) | DELETED |
| Probe scripts | DELETED |
| CPU feature enum + flag mapping | DELETED |
| C8 ordering metadata | DELETED |
| Session G Phase 1+2 plumbing | DELETED |
| F7-full Step 2 table-driven dispatch | REPLACED with hardcoded list (~30 lines) |

### D.3 asm_plan items that were DEFERRED (now permanently)

| asm_plan item | Status |
|---|---|
| Session G Phase 5 (CFG OrderingState) | DEFERRED indefinitely |
| Z9 / Z10 / Z13 forward-compat | DEFERRED indefinitely |
| Naked-attribute migration | DEFERRED indefinitely |
| @verified_spec Vale-tier | DEFERRED to v2.x+ |
| Per-instruction precondition database | NOT GROWING |
| Universal precondition categories (8) | DELETED (was speculative) |
| System #30 (Atomic Ordering) | DELETED (was speculative) |

### D.4 Concept mapping

| asm_plan concept | Level C equivalent |
|---|---|
| "Make compiler smart about instructions" | "Make compiler delegate to GCC" |
| "Per-arch register tables" | "GCC validates registers" |
| "8 universal precondition categories" | "Hardcoded ~12 UB classics" |
| "Universal precondition framework" | "Existing safety systems (VRP, alignment, etc.)" |
| "Sub-extension classification" | "GCC `-m` flag passthrough" |
| "Build-time gen + vendored" | "No data, no gen" |
| "Strict mode default on" | "Naked-only default on" |
| "100% language-safe" | "Rust-level safety + bonus UB classics" |

---

## Appendix E: References

### E.1 ZER documents

- `docs/ASM_ZER-LANG.md` — original 2026-04-01 asm design (foundation)
- `docs/asm_plan.md` — D-Alpha-7.5 Phase 2 plan (historical, superseded)
- `docs/asm_preconditions_research.md` — research artifact (historical)
- `CLAUDE.md` — Stage 4 asm safety status section (update for Level C)
- `BUGS-FIXED.md` — recent asm-related bug fixes
- `docs/refactor_ir.md` — zercheck_ir helper-layer refactor (separate concern)
- `docs/compiler-internals.md` — implementation patterns
- `docs/limitations.md` — known limitations

### E.2 External references

- Intel Software Developer Manual Vol 2 — instruction-specific UB
- ARM Architecture Reference Manual — ARM instruction semantics
- RISC-V ISA Manual (Privileged + Unprivileged) — RISC-V semantics
- Rust Inline Assembly Reference — Rust's asm story
- Rust RFC 2873 — Rust asm! design rationale
- Zig Inline Assembly — Zig's asm syntax
- GCC Inline Assembly — GCC operand constraints
- GCC Machine-Specific Options — `-m` flag list per architecture

### E.3 Production-compiler precedents

- Rust `core::arch` (intrinsics) + `asm!` (raw asm)
- Zig `@asm` builtin
- seL4 proof-driven approach (different scope)
- GCC inline asm (the foundation Level C builds on)

### E.4 Key decisions (chronological)

- 2026-04-23 D-Alpha-7.5 Phase 1 ships (S1 naked-only)
- 2026-04-25 Session A structured asm syntax
- 2026-04-25 Session B typed operand bindings
- 2026-04-26 F1a category framework (now DELETED at Level C)
- 2026-04-26 F2 register tables (now DELETED at Level C)
- 2026-04-29 Sub-extension architecture validation 3-arch
- 2026-05-02 F4-F7 instruction tables (now DELETED at Level C)
- 2026-05-02 Session G Phase 1+2 plumbing (now DELETED at Level C)
- 2026-05-02 Session G Phase 3 abandoned
- 2026-05-11 Phase A/B split decision in refactor_ir
- **2026-05-12 LEVEL C DECISION — this document**

---

## End of Document

This is a planning document. Implementation follows section 16 (6 commits,
~1-2 days).

When executing:
1. Each commit must leave the codebase in a working state
2. Run `make docker-check` after each commit
3. Don't push until all 6 commits succeed locally
4. Test assertion updates are expected in Commits 3 and 5

The decisions in this document are LOCKED IN. The locked-in items:
- Level C is the chosen direction
- Per-instruction database is not coming back
- Register tables are not coming back
- CPU feature enum is not coming back
- Probe scripts are not coming back
- ~12 hardcoded UB classics is the limit
- GCC is the trusted assembler
- Naked-only restriction is permanent

**The result of Level C: ZER's asm safety is honest, simple, and zero-
maintenance forever. ZER fits every architecture GCC supports. Sole
developer is free of ISA-evolution treadmill. Safety is preserved at
the same level as production compilers (Rust, Zig) with a small bonus
(hardcoded UB classics).**

This is the right architecture for ZER.

---

## Level D Pivot Note (added 2026-05-31)

**This document was updated with Section 1.6 capturing the Level D pivot:
user-extensible intrinsics with explicit contracts via `intrinsic_def`
mechanism + `@core::*` blessed namespace + 4-level conditional safety
claim.**

**Read Section 1.6 for the complete pivot context.** Highlights:

- The new lever is "externalize authorship of Y_intrinsic" (different from
  Level C's "constrain Y")
- ZER team's maintenance burden converges from "~1-2 intrinsics/year" to
  "~per-decade safety class additions"
- The blessed catalog (~130 current) becomes FROZEN; new intrinsics flow
  through user libraries via `@<lib>::*` namespace
- Soundness claim becomes honestly conditional: pure ZER + blessed = sound;
  user-defined = sound conditional on author's contract correctness
- The seam (wrong contract at def site) is the same Definition B failure
  mode at the intrinsic_def boundary — properly named, mitigated by
  greppable trusted-once auditing
- Level C cleanup ships FIRST (Section 16). Level D adds intrinsic_def
  on the clean foundation.
- Estimated implementation: ~750-1500 lines, ~4-8 weeks for one developer

**Level D is LOCKED IN as of 2026-05-31.** Future sessions: reference
Section 1.6 instead of re-deriving the pivot.

---

## Document History

| Date | Change |
|---|---|
| 2026-05-05 (drafted) | Initial Level C planning |
| 2026-05-10 | Audit pass |
| 2026-05-11 | Phase A/B split |
| 2026-05-12 (decision) | Level C chosen over Level A/B |
| 2026-05-12 (evening) | 2-layer model crystallization (Section 1.5) |
| **2026-05-31** | **Level D pivot — user-extensible intrinsics added (Section 1.6)** |
