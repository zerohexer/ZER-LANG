# ZER Unified Compiler — the verification chain and the plan

**Status: DESIGN NOTE (written 2026-09-28). Nothing here is built yet.** It records a decision
discussion so a fresh session does not re-derive it: which tools prove what, why the chosen chain
is RefinedC + CompCert, what CompCert's licence actually restricts, and how ZER becomes one
verified, annotation-free checker using the method already proved on KernelQ's cost-shape checker.

Related: `docs/unified-oracle-proved-ZER.md` (the unification plan, LOCKED direction),
`docs/formal_verification_plan.md`, `docs/proof-internals.md`, CLAUDE.md "The Verification Endgame".

---

## §1 The level map: who proves what

Two different kinds of correctness are involved, and the tools split cleanly between them.

| tool | proves | level |
|---|---|---|
| **RefinedC** | this C program meets its spec (no UB, memory safety, functional claims) | 1: program correctness |
| **VST** | this C program meets its spec | 1: program correctness |
| **CompCert** (`ccomp`) | the assembly behaves like the C program | 2: compiler correctness |

- **Each tool is gap-free for its own claim.** RefinedC's "the program is correct" does not depend
  on any compiler. CompCert's "the binary does what the C says" does not depend on any program proof.
- **VST and RefinedC do the same job.** The only difference in role is the C semantics each proves
  against: VST uses **Clight**, which is CompCert's own semantics; RefinedC uses **Caesium**, its
  own semantics.
- **The merged sentence** "this binary is correct", stated as ONE Coq theorem, needs the program
  proof and the compiler proof to be about the same semantics. VST + CompCert get that for free
  (both Clight). RefinedC + CompCert need one link: "Caesium and CompCert C read this C file the
  same way". That link only matters if the single merged theorem is wanted (for example, a paper
  claiming end to end with no assumption). For "a proved program compiled by a proved compiler",
  RefinedC + CompCert is complete.
- **CompCert's theorem has a precondition:** it applies only if the source has no undefined
  behaviour. A level-1 proof (RefinedC or VST) is what discharges it.

Levels below this (the assembler and linker, CompCert's ISA model vs the real ISA, silicon vs its
spec) are trusted in every system and are printed in the trust ledger, never hidden.

---

## §2 RefinedC vs VST

### 2.1 Trust base

| | RefinedC | VST |
|---|---|---|
| trusted front end | ~6,000 lines of OCaml translating C (via Cerberus's AIL) into Caesium, plus Cerberus | clightgen (CompCert's own C parser) |
| trusted semantics | Caesium, ~1,500 lines of Coq | Clight (CompCert's) |
| program logic trusted? | no: Iris's adequacy theorem reduces it to a closed Coq statement | no: soundness proved against Clight |
| foundational (a Coq proof per program) | yes | yes |
| one theorem down to machine code | no: nothing compiles from Caesium | yes, with CompCert, demonstrated twice |
| escape hatch to watch | `rc::trust_me` is a silent accept (needs an allowlist) | none built in |

(RefinedC figures are the paper's own description of its TCB.)

### 2.2 What each has achieved

- **RefinedC** (PLDI 2021): automated foundational verification of C with refined ownership types;
  handles pointer arithmetic and fine-grained concurrency (spinlocks over atomics). Real-world case
  study: a substantial component of **Google's pKVM** hypervisor (the early allocator). The same
  group's **Islaris** (PLDI 2022) verifies machine code against the official Arm/RISC-V ISA specs,
  also on pKVM, and **RefinedRust** (PLDI 2024) carries the approach to Rust.
- **VST**: **OpenSSL SHA-256 and HMAC** (USENIX Security 2015), described as the first
  machine-checked result combining a program proof, CompCert's compiler proof and a cryptographic
  security proof with no gaps; **mbedTLS HMAC-DRBG**, composed with CompCert end to end.

### 2.3 Ease

| | RefinedC | VST |
|---|---|---|
| automation | high (Lithium, syntax-directed) | low (manual Floyd tactics: `forward`, `entailer!`) |
| pains | positional, syntax-keyed annotations; hard error messages; the gcc 7.5 build pin; a C subset | long, tedious, predictable |
| docs / community | research papers | Appel's *Verifiable C*, larger community |

Measured in KernelQ (LS.31-LS.33): both cost about 5-10x the code in proof on the hard cases.

---

## §3 The decision: RefinedC proves the program, `ccomp` compiles it

**Chosen chain:** ZER source → emitted C → **RefinedC** proves the program → **CompCert** compiles
it instead of GCC.

Why this and not VST:

- VST's only extra is the single merged theorem (§1). The practical result, a proved program
  compiled by a proved compiler, is the same.
- RefinedC's automation saves the manual tactic work that VST requires.

Why `ccomp` instead of GCC, stated as a strict improvement:

| | RefinedC + GCC | RefinedC + `ccomp` |
|---|---|---|
| program proved | yes | yes |
| "the compiler reads the C the way Caesium does" | assumed | assumed |
| compiler can miscompile | **yes** (real GCC miscompilation bugs; aggressive UB-based optimisation) | **no** (proved) |

The "same reading" assumption exists with every compiler; switching to `ccomp` does not add it.
It removes compiler bugs and keeps only the assumption that was already there.

### 3.1 Optional hygiene: the boring fragment

The two semantics can differ only on exotic code: integer↔pointer casts, reading the bytes of a
pointer, comparing pointers into different objects, and edge cases of what counts as UB. Keeping the
C handed to both tools out of those patterns (a syntactic scan is enough) keeps the one assumption
over well-understood code. Ledger line:

```
assumed: Caesium and CompCert C agree on the emitted fragment
         (the fragment excludes int<->ptr casts, pointer byte access, cross-object pointer comparison)
```

### 3.2 Blockers found (measured 2026-09-28)

`ccomp` accepts less than GCC. Against the CompCert user manual (§6.4.1 keywords, §6.5-6.8):

- **Statement expressions `({ ... })` are not listed as supported.** `emitter.c` contains **193**
  string literals that start one (`grep -c '"({' emitter.c`), used for bounds checks with side-effect
  indices and similar. These must be re-emitted without statement expressions before `ccomp` can
  compile ZER output.
- **`typeof` is not a CompCert keyword.** `emitter.c` has **96** lines mentioning `__builtin_`,
  `__attribute__` or `typeof`; each needs checking against the manual's supported list.
- **`asm` needs `-finline-asm`**, and `switch` is restricted to the structured (MISRA) form unless an
  option is set. Variable-length arrays are not supported.
- **Building `zerc` itself with `ccomp`** is untested: try `make CC=ccomp` first.
- `ccomp` is not installed on the laptop yet.

None of this blocks the RefinedC half; it gates only the switch from GCC to `ccomp`.

---

## §4 CompCert's licence: what it actually restricts

Read from CompCert's `LICENSE` and the user manual §2.1 (2026-09-28):

- **Non-commercial only:** most of the compiler. Allowed: "educational, research, or evaluation
  purposes only"; not allowed: use "in connection with any activities which purpose is to procure a
  commercial gain" without a licence from AbsInt. The manual: "commercial uses require purchasing a
  license from AbsInt".
- **Dual-licensed LGPL 2.1+ (commercial use allowed):** `lib/`, `common/`, `cparser/`, `export/`
  (clightgen), and the C/Clight semantics (`Clight.v`, `Csem.v`, `Ctypes.v`, `Cop.v`, `Csyntax.v`, …).
  `flocq/` and `MenhirLib/` are LGPL 3+. **`runtime/` is BSD 3-clause.**
- **Redistribution** of the non-commercial parts must keep the same terms.
- **Compiled output is not mentioned** in the licence, the manual or AbsInt's CompCert page. That is
  silence, not explicit permission.

What that means for ZER:

| case | licence needed? |
|---|---|
| building `zerc` with `ccomp` for open-source, non-commercial ZER, shipping only the `zerc` binary | no (research / personal use; `ccomp` is never shipped) |
| a company **running** the `zerc` binary | no: running a binary is not using CompCert; `runtime/` is BSD |
| a company building its own firmware with ZER → `ccomp` | **yes**: that company runs `ccomp` commercially |
| ZER itself starts earning money (sales, paid support, funded development) | the `ccomp` build step becomes commercial: buy a licence or build releases with GCC |
| VST, clightgen, the Clight semantics | free, commercial included (BSD / LGPL) |

Default stays **emit C → GCC** (CLAUDE.md "Emit-C Permanently"); `ccomp` is a certified-build mode.
If any case turns commercial, get AbsInt to confirm the output point in writing.

---

## §5 Unified ZER by the cost-shape method

The goal: one verified checker, annotation-free, where the programmer narrows ambiguity by writing
code (a guard, a branch, an `orelse`), never by picking a contract the way Rust's lifetimes are
picked.

KernelQ's cost-shape checker (`refinedc/artifacts/LS41_checker_proof/` in the KernelQ repo) has
three properties that ZER's safety checker does not have yet:

| cost-shape has | ZER today |
|---|---|
| **soundness proved**: `check_membership = true` → the bound holds (`graded_run_bounded`) | the Verification Endgame's goal, not built |
| **completeness inside a stated fragment**: every program in the grammar `G` is accepted (`G_is_pterm`) | not attempted: CLAUDE.md says 100% precision is impossible by Rice |
| **over-rejection measured outside it**: pinned witnesses, each classified NECESSARY / OVER / CLOSED / … (`gen_au.py`, `au_expected.tsv`) | holes found by red-team bug hunts |

Rice's theorem rules out completeness for **all** programs, not completeness for a **stated
fragment**. Bringing the middle row to ZER turns "we fix over-rejections as we find them" into
"inside the fragment, refusing a safe program is proved impossible; outside it, every refusal is a
pinned, classified row."

### 5.1 The recipe (per safety class)

1. Define the fragment `G_X` as **its own grammar**, written without reference to the checker
   (otherwise "the fragment" silently means "whatever the checker accepts").
2. Prove **soundness**: accepted → `safe_X` over the core semantics.
3. Prove **completeness in `G_X`**: in `G_X` and safe → accepted.
4. Build the **audit**: witnesses outside `G_X`, each pinned and classified, run in the suite.
5. Refusals print the candidate meanings and the code that would split them.

Rules carried over from the cost-shape work:

- **Never add a check without a theorem that needs it** (rule 2′). A check with no theorem behind
  it is either an over-rejection or unjustified, and reasoning cannot tell which.
- **Audit by term former, not by the checker's arms.** A checker with a catch-all cannot be audited
  from its own code.
- **The link from the model to the binary is the slow part,** not the checker.

### 5.2 Order

Follow `unified-oracle-proved-ZER.md` and CLAUDE.md's sequencing (do not go Coq-first until the
design freezes): the **handles / UAF vertical slice** first (alloc / free / deref + the handle
lattice), then class by class over the same core semantics, then compose into the single
`accepted → safe` theorem.

---

## §6 Proof or refusal (optional)

An optional direction, not a commitment: where the checker can prove an operation safe, emit no
check; where it cannot, refuse, instead of emitting `_zer_bounds_check`, `_zer_trap` or an
auto-guard's silent `if-return`.

- Values from outside the program (MMIO reads, input) cannot be proved; they enter as raw values and
  must be validated in code before use as an index, pointer or divisor. The check is the program's
  own, written and handled, never a crash.
- MMIO interfaces come from per-target register descriptions (address, width, access, reserved
  bits); a read returns a raw value.
- Refusal raises over-rejection, so fragment completeness (§5) comes before removing checks. A
  development mode that keeps the checks is fine as long as the release build is the refusing one.
- ZER measured its bounds checks at about 0-4% overhead, so the gain is guarantees, not speed.

---

## Sources

- RefinedC paper (PLDI 2021): https://plv.mpi-sws.org/refinedc/paper.pdf
- Michael Sammler's publications (RefinedC, RefinedRust, thesis): https://pub.ista.ac.at/~msammler/
- Islaris (PLDI 2022): https://www.cl.cam.ac.uk/~pes20/2022-pldi-islaris.pdf
- Verified correctness and security of OpenSSL HMAC: https://www.cs.princeton.edu/~appel/papers/verified-hmac.pdf
- Verified Correctness and Security of mbedTLS HMAC-DRBG: https://arxiv.org/pdf/1708.08542
- CompCert LICENSE: https://github.com/AbsInt/CompCert/blob/master/LICENSE
- AbsInt CompCert page: https://www.absint.com/compcert/index.htm
- CompCert user manual (local copy read: `~/Downloads/manual.pdf`, §2.1 and §6)
