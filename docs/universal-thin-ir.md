# Universal Thin IR and Route B — verified binaries for any ISA

**Status: DESIGN (written 2026-09-30). Nothing here is built.** This is a pickup document: it records a
long design discussion so a fresh session can continue without re-deriving it. It describes a
**separate product layer**, not part of the ZER language: a small, universal, language-independent
low-level IR ("thin IR"), plus a way to produce **proved-correct binaries for any ISA** by validating
each binary against a formal ISA specification ("route B").

Related documents:

| document | relation |
|---|---|
| `docs/zer-pancake-backend.md` | route A for ZER: ZER → Pancake → CakeML's proved backend |
| `docs/zer-unified-compiler.md` | the RefinedC / VST / CompCert chain and CompCert's licence |
| `docs/proved-systems-language.md` | the separate proof-first language; trust-ledger conventions |
| `docs/asm_lang_zer_safe.md` | the effect table (pre/postconditions per intrinsic), reused here for external calls |

ZER's CLAUDE.md records a locked decision: "Emit-C via GCC is the permanent architecture … never
suggest IR, LLVM, QBE, or native code generation in this project." The thin IR is designed as a
**separate product** fed by ZER through a lowering. Using it for ZER itself would reverse that
decision; that is an owner decision to make consciously, not something to drift into.

---

## §0 The owner's intent

- **A product, not a service.** Adding support for a customer's ISA should be automatable ("add a
  spec"), not a bespoke consulting job each time. "We won't outsource our brain; we sell a product."
- **The strongest guarantee available** for the shipped binary, without owning or verifying the chip
  (no verified-CPU work; the ISA spec is the floor).
- **One universal IR for all ISAs.** No per-ISA machine IR as the architecture; per-ISA extras only as
  optional extensions inside the one IR.
- **No SMT in the verdict.** Proofs are checked by a small kernel (Coq's); search, if any, only
  suggests.
- **No HOL4 required** for route B. (Route A via Pancake uses HOL4 proofs, but only as a black box.)
- **The IR is not ZER.** It is typeless beyond machine widths and knows nothing about any source
  language.

### Reading order

| you want | read |
|---|---|
| why route B, and how it compares to a proved compiler | §1 |
| what the IR looks like | §2 |
| the whole pipeline in one picture | §3 |
| how optimisation fits | §4 |
| how binaries get proved | §5 |
| where ISA descriptions come from | §6 |
| how ZER and Pancake connect | §7 |
| licensing and the product plan | §8 |
| questions already settled | §9 |

---

## §1 Route A vs route B

### 1.1 The two routes

| | **A: proved compiler** (CompCert, CakeML/Pancake) | **B: validated binaries** (seL4-style translation validation) |
|---|---|---|
| idea | prove the compiler's algorithm once, per target | produce the binary any way you like, then **prove each binary** matches its source |
| scope | every program | each binary that passes validation |
| what is checked | the compiler | **the final binary**, including assembler and linker output |
| per-ISA cost | a backend **plus its proofs** | an ISA spec **plus an unproved code generator** |
| failure mode | none per build | a build can fail to validate (availability, not safety) |
| trusted | proof kernel, ISA model (+ assembler/linker for CompCert) | proof kernel, ISA spec, thin IR semantics, the decoding tooling (unless proved) |

### 1.2 Which is strongest

1. **A + B:** a proved compiler **and** a validated final binary (CompCert + AbsInt's Valex, or
   CakeML/Pancake plus a binary check against a vendor ISA spec).
2. **A with an in-logic bootstrapped compiler** (CakeML/Pancake): the strongest single approach.
3. **A with extraction** (CompCert: trusts Coq extraction, the OCaml compiler, assembler, linker).
4. **B alone:** as strong as A **for each binary it accepts**; stronger on the final bytes; the cost is
   builds that may not validate and a validator whose trusted parts must be stated.

Going below the ISA (a verified CPU such as CakeML's Silver or Bedrock2's Kami) is out of scope: we
do not own the chip. The ISA spec versus real silicon (errata) is the floor for every route.

### 1.3 B does not need A first

B needs A's **engineering** (a code generator) but not A's **proofs**:

| | A | B |
|---|---|---|
| code generator (instruction selection, register allocation, encoding) | ✅ | ✅ (own, simple) |
| proofs that the code generator is correct | ✅ the expensive part | ❌ not needed |
| ISA spec | ✅ | ✅ |
| generic binary matcher | ❌ | ✅ built once |

### 1.4 B is not a riskier guarantee

For any binary it accepts, B's guarantee is the same kind as A's (the binary behaves as the thin IR
says), and it covers the final bytes. The residual items are all stateable: builds that fail to
validate (never shipped), the trusted spec and decoding tooling, and the silicon floor shared with A.

### 1.5 Avionics and certification

- Formal methods are accepted: DO-178C's formal-methods supplement is **DO-333**; binary validation
  tools (AbsInt's Valex) are used in that world.
- The validator is a **verification tool**, so customers need **tool qualification** evidence (what
  it checks, trusted parts, test results). A proved compiler needs the same kind of kit (AbsInt sells
  them for CompCert). This is process work, not a flaw in the technique.
- Tools are not "certified" on their own; **projects** are certified, and tools are **qualified**
  within them.

### 1.6 The ISA landscape (why "any ISA" matters)

| ISA | typical use |
|---|---|
| PowerPC / Power Architecture | traditional flight-critical avionics (NXP QorIQ, MPC5xxx) |
| ARM (Cortex-R, Cortex-A) | growing in avionics and automotive; Zynq UltraScale+ (A53 + R5) |
| x86-64 | mostly non-flight-critical (cabin, displays, mission computers) |
| SPARC (LEON) | space (ESA LEON3/LEON4) |
| RISC-V | emerging, especially space (NOEL-V, PolarFire SoC) |
| legacy (MIL-STD-1750A, 68k), 8/16-bit MCUs (AVR, MSP430, 8051) | long-lived systems |

CompCert covers PowerPC, ARM (32-bit with VFP), AArch64, x86 (32/64), RISC-V (32/64), plus AURIX in
its commercial version. Pancake covers x86-64, ARMv8, RISC-V, MIPS-64 (64-bit compiler) and ARMv7,
ag32 (32-bit compiler). Neither covers everything; route B's point is that a new ISA costs a spec and
a simple generator, not a new proved backend.

---

## §2 The thin IR

### 2.1 Principles

1. **Universal and language-independent.** Nothing about ZER (no structs, enums, `Handle`s, safety
   metadata). Any front end lowers into it.
2. **Values are bits of a width:** `i1`, `i8`, `i16`, `i32`, `i64`. No named types, no aggregates, no
   signedness in the type.
3. **Signedness lives in operations:** `sdiv`/`udiv`, `srem`/`urem`, `slt`/`ult`/`sle`/`ule`,
   `sext`/`zext`, `ashr`/`lshr`. (The Pancake `<` vs `<+` trap made explicit.)
4. **Flat, byte-addressed memory** with explicit width, alignment and a volatile flag on every load and
   store (`load.w32 addr`, `store.w8 addr, v`). Structs and arrays are lowered to offsets before the IR.
5. **Every operation fully defined.** Wrap-around arithmetic, defined results for over-wide shifts, and
   an explicit error state for division by zero (or a precondition the front end must discharge). No
   undefined behaviour inside the IR.
6. **SSA-CFG form:** basic blocks and branches; each value assigned once; unlimited virtual registers.
   CFG matches binaries best (validation); SSA makes optimisation simple.
7. **Calls:** direct and indirect, word arguments; **external calls carry contracts** (effects,
   pre/postconditions), reusing the effect-table idea from `asm_lang_zer_safe.md`.
8. **Target parameters are separate from the IR:** word size, endianness, calling convention and alignment
   rules belong to the target description (§3), not to the IR.

### 2.2 Extension points (instead of per-ISA IRs)

- **Target-specific operations:** a target description can declare extra operations (a DSP
  multiply-accumulate, a bit-field instruction, a special register access) with their meaning taken from
  the ISA's Sail spec. The IR's structure is unchanged; only the operation set grows per target (the
  LLVM "target intrinsic" idea).
- **Optional per-target passes:** plug-in passes running on the same IR for one target only. Two rules:
  they must emit hints (§5), and removing them may only cost speed, never correctness.
- A per-ISA machine IR can still be added later if some ISA truly needs it, but it is not the
  architecture.

### 2.3 Reference IRs to study (not necessarily adopt)

| IR | why |
|---|---|
| CompCert's **Cminor** | thin, nearly typeless, memory "chunks" (width + signedness per access); its semantics is in CompCert's **LGPL** part |
| **WebAssembly** | four value types, fully specified, mechanised semantics in Coq (WasmCert-Coq) and Isabelle |
| Pancake / CakeML's **wordLang** | the words-and-explicit-memory style route A needs |
| **QBE's IL** | a minimal, readable compiler IR |
| LLVM IR | the full-featured reference for SSA design choices |

---

## §3 The pipeline

### 3.1 Target description (data, not code)

Per ISA, one description containing:
- register file (classes, callee/caller-saved, special registers);
- instructions: operand forms, **selection patterns** (thin IR shapes → instructions), costs;
- encodings (instruction → bytes);
- calling convention, stack layout rules, alignment;
- legal widths and operations (what needs legalisation);
- target-specific operations (§2.2).

The GCC analogue is the machine description (`.md` files); the LLVM analogue is TableGen (`.td`).
Long-term goal: derive instruction semantics and selection patterns from the ISA's **Sail spec**, so one
description serves both generation and validation (§6.4).

### 3.2 The diagram

```
                         TARGET DESCRIPTION (per ISA, data)
                         registers · instructions + patterns · encodings
                         calling convention · costs · legal widths · target ops
                                  │ read by every target-aware phase
                                  ▼
front end (ZER, …) ──► lowering into the thin IR
   │
   ▼
┌─────────────────────────── ONE UNIVERSAL THIN IR (SSA-CFG) ─────────────────────────────┐
│ 1. generic optimisations         constant propagation, value numbering (CSE),           │
│    (costs from the description)  dead code, inlining, loop-invariant motion,            │
│                                  simple loop optimisations                              │
│                                  ── checked IR-to-IR (same language both sides) ──      │
│ 2. legalisation                  rewrite what the target lacks, still as thin IR:       │
│                                  64-bit on 32-bit → pairs; no divider → routine;        │
│                                  unaligned access → byte loads                          │
│ 3. instruction selection         IR patterns → target instructions (from description)   │
│                                  the IR now carries target opcodes; same structure      │
│ 4. register allocation           generic (linear scan / graph colouring) over the       │
│                                  register file from the description                     │
│ 5. optional per-target passes    scheduling, peepholes — must emit hints               │
└───────────────────────────────────────────┬─────────────────────────────────────────────┘
                                            │  + HINTS (IR op ↔ instructions, value ↔ register/slot,
                                            │           block ↔ address)
                                            ▼
                                   encoder (from the description) ──► binary
                                            │
                                            ▼
                matcher: binary ≡ thin IR, using the ISA's Sail spec + hints
                ──► a proof checked by the Coq kernel ✔   (or: validation fails, binary not shipped)
```

### 3.3 Lanes

| lane | who produces the binary | validation difficulty | when |
|---|---|---|---|
| **own generator + hints** (default) | our simple, unproved code generator | easy: the matcher follows the hints | every ISA we support |
| **vendor GCC** (optional) | thin IR printed as plain C, compiled by the vendor's GCC | hard: the matcher must reconstruct what GCC did | when a customer needs GCC-level optimisation |
| **Pancake / CakeML** (route A, optional) | thin IR printed as Pancake, CakeML's proved backend | none needed (proved), optionally also validated | ISAs Pancake supports |

The plain-C printer is trivial (one-to-one), and because the final binary is validated, neither the
printer nor the vendor compiler has to be trusted.

---

## §4 Optimisation

### 4.1 How existing compilers split it

**GCC:**
```
front ends ─► GENERIC ─► GIMPLE (SSA): hundreds of machine-independent passes
                         (constant propagation, value numbering, dead code, IPA inlining,
                          loop opts, vectorisation)
          ─► RTL: machine-dependent (combine, scheduling, register allocation IRA/LRA, peephole)
          ─► assembly
```
Targets are machine descriptions (`.md`); a new backend is largely a description.

**LLVM:**
```
front end ─► LLVM IR (SSA): machine-independent pipeline (InstCombine, GVN, LICM, SROA,
             inlining, loop vectoriser), target-aware through cost queries (TargetTransformInfo)
          ─► instruction selection (SelectionDAG / GlobalISel)
          ─► Machine IR: scheduling, register allocation, peephole, branch opts
          ─► MC layer (assembly / object files)
```
Targets are described in TableGen; much of each backend is generated.

**CompCert:**
```
C ─► Clight ─► C#minor ─► Cminor ─► CminorSel (instruction selection, per ISA)
  ─► RTL (constant propagation, CSE, inlining, tail calls, dead code; parameterised by per-ISA
     operations and addressing modes)
  ─► LTL (register allocation, checked by a proved validator) ─► Linear ─► Mach ─► Asm
```
Its manual (§1.4.3): code runs "generally twice as fast as … gcc -O0, and approximately 10% slower
than … gcc -O1", weaker on heavy matrix loops. Its many intermediate languages exist to make the
**proofs** modular; route B validates results instead, so one IR with phases is enough.

### 4.2 Strategy for route B

| level | examples | how it is checked |
|---|---|---|
| machine-independent (thin IR) | constant folding/propagation, DCE, CSE, inlining, LICM | IR-to-IR validation per run, or prove the pass once in Coq (clean-room, from the literature) since it is shared by every ISA |
| machine-dependent (per target) | instruction selection, register allocation, scheduling, peephole | the binary matcher, using hints |

Every optimisation ships with the evidence that lets the checker confirm it: a hint format or a proof.

### 4.3 Aggressiveness versus checkability

| optimisation | validation |
|---|---|
| constant folding, dead code, register allocation, instruction selection | easy (with hints) |
| inlining, scheduling, peephole | moderate: needs good hints |
| loop unrolling, vectorisation, heavy memory reordering | hard |

Start with the easy set (expected roughly between `gcc -O0` and `-O1`, CompCert's neighbourhood). Add
a transformation only together with its hint format or proof. For maximum speed on a given ISA the
vendor-GCC lane remains, with harder and less certain validation.

---

## §5 The matcher (validator)

### 5.1 Shape

For each function: symbolically execute the binary using the ISA's formal spec, execute the thin IR
using its semantics, and show both produce the same observable behaviour (results, memory effects,
external calls). The hints say which binary locations correspond to which IR values and operations,
so the matcher **checks** a correspondence instead of **searching** for one.

### 5.2 Proof-producing, not proved (the LCF / de Bruijn principle)

The matcher can be large and clever. Each successful check **emits a proof** that the **Coq kernel**
checks. A matcher bug can therefore only cause a false rejection, never a false acceptance. Islaris
works this way (per-binary proofs in Coq/Iris). No SMT solver decides anything; if search is ever used,
it only suggests steps that then become kernel-checked proof.

### 5.3 Certifying compilation (why our own generator makes this easy)

With a vendor compiler, the matcher must reverse-engineer register allocation, reordering and inlining.
seL4's translation validation (Sewell, Myreen, Klein, PLDI 2013) handled GCC at `-O1` and most of `-O2`,
using SMT solvers (SONOLAR, Z3) for the matching. With our own generator, the compiler **supplies the
evidence** (hints) and the checker verifies it: no search, no SMT, simple proof generation. This is the
same "tactics, not search" principle as the rest of the design.

### 5.4 Trust ledger for route B

- **Proved per binary:** the binary behaves as the thin IR program says (under the ISA spec).
- **Trusted:** the Coq kernel; the ISA spec (Sail) versus real silicon; the thin IR's semantics; the
  tooling that turns spec + binary into a provable form (Isla, in Islaris's case) unless it is itself
  proved or proof-producing; the source-to-thin-IR lowering (unless proved).
- **Not trusted:** our code generator, the encoder, the assembler/linker, the vendor compiler, the C
  printer. Their output is checked.
- **Failure mode:** a binary that does not validate is not shipped. Availability risk, not safety risk.

---

## §6 ISA specifications

### 6.1 Sail

Sail is the language used for authoritative ISA specifications (RISC-V's official "golden model", an
Armv8/9 model derived from Arm's own ASL, parts of x86 and others). Sail can generate models for
**Coq**, Isabelle and HOL4. **Isla** symbolically executes any Sail spec; **Islaris** (PLDI 2022) uses
Isla to verify machine code against Sail models in Coq/Iris. That makes the binary side of the matcher
**ISA-generic**: supporting a new ISA means supplying its spec.

### 6.2 Legacy ISAs

Legacy ISAs usually have **no** formal spec. Writing one (and validating it against real hardware with
test programs) is the main per-ISA cost: weeks to months of expert work, depending on the ISA. As a
product: ship specs for the common legacy ISAs ourselves (a one-time cost amortised across customers);
offer spec-writing for rare ones.

### 6.3 Where CakeML-style targets fit instead

For conventional 32/64-bit register machines (PowerPC, SPARC, older ARM, MIPS32), route A via a CakeML
fork is also possible: CakeML compiles to a small ISA-neutral assembly (asmLang), and each target
supplies an ISA model (L3-derived, in HOL4), a configuration, an encoder and proofs that CakeML largely
automates (CPP 2017, "Verified compilation of CakeML to multiple machine-code targets"). 8/16-bit and
exotic ISAs fit poorly. This route needs HOL4 skill in-house.

### 6.4 Long-term: one description for both generation and validation

Derive selection patterns and operation semantics from the Sail spec itself, so the same description
drives code generation and validation. Start with a hand-written description (the matcher double-checks
it anyway) and automate later.

---

## §7 Relation to ZER and Pancake

- **ZER feeds the thin IR:** ZER AST / ZER IR → (one lowering) → thin IR. By then ZER's checker has
  already established safety; the thin IR only has to preserve behaviour. The lowering is trusted and
  differentially tested at first, proved later if wanted.
- **One lowering, several outputs:**
  ```
                            ┌──► print as Pancake ──► CakeML/Pancake backend (proved)      route A
  ZER ─► ZER IR ─► THIN IR ─┤
                            ├──► own code generator + hints ──► binary ──► matcher ✔      route B
                            └──► print as plain C ──► vendor GCC ──► binary ──► matcher ✔  route B (optional)
  ```
  All outputs start from the same thin IR, so their behaviours can be compared for free (differential
  testing across lanes).
- **No CakeML dependency for route B.** CakeML/Pancake is optional, for the ISAs it supports.
- **Pancake facts relevant here** (from `docs/zer-pancake-backend.md`): Pancake is a separate language
  that shares CakeML's backend (from wordLang down); it has no GC; its internal `ld32`/`st32` were
  rejected by a front-end bug through v3479 (missing `localise` cases; reported by the owner as #1505
  and fixed by PR #1506 on 2026-09-30); it has no
  division operator; comparisons `<` signed, `<+` unsigned.
- **Intrinsics on these paths:** pure computations (popcount, clz, bswap, carry arithmetic) become thin
  IR operations or routines; MMIO becomes volatile loads/stores; atomics and privileged instructions
  become external calls to per-ISA stubs whose behaviour is specified by ZER's effect table
  (pre/postconditions), trusted and listed by name.

---

## §8 Licensing and the product

### 8.1 Reusing LGPL code (CompCert's LGPL files)

LGPL code **can** be used in a commercial, sold product:
- use and adapt CompCert's LGPL files (`common/`: memory model, values, events, the small-step
  simulation framework; `lib/`; the Cminor and Clight semantics; `cparser/`; `export/`);
- if you distribute them, **those files** (and your changes to them) stay LGPL and their source goes to
  recipients; users must be able to replace the LGPL component;
- **your own files** (thin IR design, matcher, lifters, generator, proofs) stay proprietary;
- those CompCert files are dual-licensed (INRIA non-commercial **or** LGPL): choose LGPL;
- CompCert's **compiler passes and `Asm.v` are not LGPL**: do not copy or port them. Clean-room from
  the papers is fine.

Suggested layout:
```
product/
├── vendor/compcert-lgpl/   ← CompCert LGPL files (+ changes), shipped as source, LGPL
└── src/                    ← validator, matcher, thin IR, generator, proofs: proprietary
```
(Not legal advice; confirm specifics for a commercial release.)

### 8.2 Other licences

- **Pancake/CakeML:** BSD-3. Ship their copyright notice and licence text if you ship `cake` or link
  their code (e.g. `basis_ffi.c`). Released versions stay BSD permanently.
- **GCC-built tools:** building your own tools with GCC is free; the GCC Runtime Library Exception keeps
  your binaries from becoming GPL.
- **CompCert (`ccomp`) itself:** route B does not need it. For the record, AbsInt confirmed in writing
  (2026-09-30) that building the open-source ZER with it is fine and that services around the free ZER
  need no licence; selling a commercial edition built with it needs a project licence (EUR 44,970
  perpetual, per project and target). Details: `docs/zer-unified-compiler.md` §4.1.

### 8.3 Product, not service

- The **matcher, thin IR, generator framework and specs for common ISAs** are the product.
- Per-customer work shrinks to **a target description and, if missing, a Sail spec**; specs for common
  legacy ISAs ship with the product.
- For regulated customers, the product also needs **qualification evidence** for the validator.

### 8.4 Build order

1. The thin IR and its semantics in Coq (study Cminor and WasmCert-Coq first).
2. **RISC-V first:** it has the official Sail golden model and a clean ISA.
3. A simple code generator for RISC-V (unproved) that emits hints.
4. The matcher for RISC-V: follow hints, emit a Coq proof per function.
5. The IR-level optimisations with IR-to-IR checking.
6. A second ISA (ARM, via the Arm-derived Sail model) to prove the design is really ISA-generic.
7. Then a legacy ISA with a customer (PowerPC is the avionics priority).

---

## §9 Settled questions (do not re-derive)

**Q1. Does route B need a proved compiler (route A) first?** No. It needs a code generator, but an
unproved one; correctness comes from validating each binary.

**Q2. Is route B weaker than a proved compiler?** Not for the binaries it accepts: same kind of
guarantee, and it checks the final bytes. Its costs are builds that may not validate, and a trusted
spec/decoding step to state.

**Q3. Is a vendor GCC required?** No. Something must generate the machine code (a Sail spec only says
what instructions do), but it can be our own simple generator. The vendor-GCC lane is optional, for
optimisation.

**Q4. Does route B need CakeML?** No. CakeML/Pancake is optional route A for the ISAs it supports.

**Q5. Should the thin IR be ZER's IR?** No. ZER's IR is large and safety-specific. The thin IR is
separate, universal and language-independent; ZER lowers into it once.

**Q6. Do we need RTL, LTL, Mach etc. like CompCert?** No. Those exist to make proofs modular. With
validation, one IR with phases (optimise, legalise, select, allocate) is enough.

**Q7. Per-ISA machine IRs?** Not as the architecture. Per-target operations and optional per-target
passes live inside the one IR; a machine IR can be added later only if an ISA truly needs it.

**Q8. Where does optimisation happen?** Mostly on the thin IR (generic, costs from the description),
plus instruction selection, register allocation and scheduling per target. Each optimisation carries
its evidence (hints or a proof).

**Q9. Can LGPL code be used in a sold product?** Yes; the LGPL files stay open, our files can stay
closed.

**Q10. Is the validator trusted?** Only if we choose to trust it. The design makes it proof-producing,
so the Coq kernel checks every result and the matcher itself need not be trusted.

**Q11. Should we invent our own Pancake-like programming language for this?** No. Route B needs an
internal validation IR with formal semantics, not a user-facing language (a user language would pull
the design back toward route A).

**Q12. Do we need a verified CPU?** No; we do not own the chip. The ISA spec versus silicon is the floor.

---

## §10 What this does not claim

- No binary is proved unless it validated; some builds will fail to validate.
- The ISA spec is trusted to match the silicon (errata are the floor).
- The thin IR semantics and the spec-decoding tooling are trusted unless proved.
- The source-to-thin-IR lowering is trusted and tested until proved.
- Optimisation is modest at first (around `gcc -O0`…`-O1`); aggressive transformations wait for their
  hint formats.
- No speed or effort numbers are measured yet; everything here is design.

---

## Sources

- Translation validation for a verified OS kernel (seL4, PLDI 2013): https://www.cse.chalmers.se/~myreen/pldi13.pdf
- seL4 graph-refine (BSD-2): https://github.com/seL4/graph-refine
- Islaris (PLDI 2022): https://www.cl.cam.ac.uk/~pes20/2022-pldi-islaris.pdf
- The Trusted Computing Base of the CompCert Verified Compiler: https://arxiv.org/pdf/2201.10280
- CompCert LICENSE (LGPL / non-commercial split): https://github.com/AbsInt/CompCert/blob/master/LICENSE
- AbsInt CompCert (licence types, Valex): https://www.absint.com/compcert/contact.htm
- CompCert user manual (targets §1.4.1, performance §1.4.3; local copy `~/Downloads/manual.pdf`)
- Verified compilation of CakeML to multiple machine-code targets (CPP 2017) and The verified CakeML
  compiler backend (JFP 2019): https://cakeml.org/
- Pancake (PLOS 2023): https://cakeml.org/plos23.pdf
- bedrock2: https://github.com/mit-plv/bedrock2
