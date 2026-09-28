# ZER on Pancake — verified compilation without CompCert or GCC

**Status: DESIGN + SPIKE (2026-09-28). Nothing is built beyond the spike.** Adopting this route
**reverses the locked decision "Emit-C via GCC is the permanent architecture"** (CLAUDE.md,
"Architecture Decision: Emit-C Permanently"). That reversal is an owner decision, not something to
drift into; this document records the case for it and what the spike measured.

Related: `docs/zer-unified-compiler.md` (the CompCert / RefinedC / VST chain and the CompCert
licence), `docs/proved-systems-language.md` (the separate proof-first language).

---

## §1 The goal

A ZER that is **correct all the way to machine code**, shippable freely on several ISAs, without
CompCert's licence and without trusting GCC.

- **ZER-core:** a C-like "higher-level assembly". Close to C, not an exact copy. No memory-safety
  checks: full control, like C.
- **ZER-safe:** the same language with ZER's existing safety checker switched on (use-after-free,
  bounds, races, provenance, …). Same source, the checker is the only difference.

Both compile through one verified backend.

## §2 Two kinds of correctness (do not conflate them)

| goal | guarantees | tools |
|---|---|---|
| **verified safety** | the program has no UAF, races, out-of-bounds | ZER's checker, RefinedC, VST |
| **verified compilation** | the binary does exactly what the source says, bugs included | CompCert, CakeML / Pancake, Bedrock2, Jasmin |

ZER-core needs only the second. ZER-safe needs both.

## §3 Routes that do NOT work, and why (settled — do not re-derive)

**The rule: compiling code with CompCert proves the binary matches its source. It never proves the
source is right.**

1. **Build `zerc` with CompCert, keep emitting C to GCC.** `zerc` becomes faithful to `zerc.c`, but
   users' programs are still compiled by GCC (`zerc_main.c` locates a bundled `gcc/bin/gcc` or `gcc`
   on `PATH` and runs `gcc -std=c99 -O2 -fwrapv -fno-strict-aliasing …`; README: "Requires GCC").
   The user's binary is exactly as trusted as today.
2. **Build `zerc` with CompCert and hope its backends come along.** A compiler binary contains only
   its own source's logic. CompCert's backends are CompCert's code; none of it ends up in programs
   CompCert compiles.
3. **Compile GCC's backend with CompCert.** Same rule: GCC's own miscompilation bugs are in its
   source and are faithfully reproduced. GCC is also C++ (CompCert compiles C only), millions of
   lines, and GPL.
4. **Write our own backend** (with or without AI help, "rewritten into the CompCert subset"). A new
   backend is only as trusted as its source; a fast AI rewrite has neither GCC's decades of testing
   nor a proof. Proving it is CompCert-scale work, per ISA.
5. **Bundle `ccomp`.** Shipping CompCert, or users running it commercially, needs an AbsInt licence.

What CompCert trusts is its **description** of each ISA (a formal model), not its **translation**,
which is proved per backend. GCC trusts millions of lines of translation. Some ISA description is
trusted by every verified compiler; the difference is its size.

## §4 Backend candidates

| backend | prover | licence | ISAs | source language | notes |
|---|---|---|---|---|---|
| CompCert | Coq | non-commercial; AbsInt for commercial | x86, ARM, AArch64, PowerPC, RISC-V | full C (C99 subset) | easiest technically; licence |
| **Pancake** (CakeML backend) | HOL4 | **BSD-3** | **x86-64, ARMv8, RISC-V, MIPS-64** (64-bit compiler); arm7, ag32 (32-bit compiler) | small: words, labels, structs; static heap | **chosen candidate**; research language, actively developed |
| Jasmin | Coq | MIT | x86-64; ARMv7 and RISC-V experimental, 32-bit | low-level, built for crypto | more widely used (libjade, Formosa ML-KEM); x86-first in practice |
| Bedrock2 | Coq | MIT | RISC-V only | minimal C-like | one ISA |
| CakeML itself | HOL4 | BSD-3 | as Pancake | ML with a garbage collector | GC conflicts with ZER |
| seL4-style validation of GCC | Isabelle/HOL4 + SMT | your own | whatever you model | real GCC output | years of tooling; SMT in the verdict |

Using Pancake needs **no HOL4**: the compiler's proof was written and checked by the CakeML team.
HOL4 is only needed to prove our own translator in the same logic, prove properties of Pancake
programs, or join ZER's Coq proofs with Pancake's into one theorem (the seL4 vs CompCert logic
mismatch).

## §5 The architecture

```
 BUILD TIME (maintainer, once)             USE TIME (every user)
 ─────────────────────────────             ─────────────────────
 zerc.c ──CompCert──► zerc binary          firmware.zer
                     (faithful to source)       │
                          │                     ▼
                          └──────────────►  zerc: [safety checker, ZER-safe only]
                                                │  lower to Pancake (from the AST)
                                                ▼
                                            firmware.pnk
                                                │  Pancake / CakeML backend (verified)
                                                ▼
                                   x86-64 · ARMv8 · RISC-V · MIPS machine code
```

- CompCert builds `zerc` itself, once (non-commercial use: fine while ZER is non-commercial; if ZER
  earns money, build releases with GCC instead). It is never shipped and never touches users'
  programs.
- Users need no GCC, no CompCert, no licence: the Pancake backend (BSD) can be shipped.

## §6 Trust ledger for this route

- **Proved:** the Pancake backend (Pancake AST → machine code), by the CakeML team in HOL4.
- **Faithful:** the `zerc` binary (built by CompCert).
- **Trusted:** the ZER → Pancake translation (until proved: the canonical-form invariant of §8 is
  the first thing to prove); the ISA models; assembler/linker; silicon. For ZER-safe, also the
  safety checker (until ZER's verification endgame lands).
- **Trusted, small:** C code reached through Pancake FFI (traps, any remaining helpers).

## §7 The spike (2026-09-28)

**Program:** the hardest representative runtime piece of ZER: `Pool(Acct, 4)` + `Handle(Acct)`
(`u64` handle = `gen << 32 | idx`, generation check on `get`, generation bump on `free`, gen 0
reserved for the null handle). Allocate, write, read, free, re-allocate (same slot, new
generation), read, free; result 105; then a stale-handle read that must trap.

ZER itself refuses the stale read at compile time
(`zercheck: use after free: 'a' is freed (freed at line 10)`); the runtime generation check is the
second line of defence, and that is what the translations reproduce.

| | ZER → C → GCC | ZER → Pancake (hand) | ZER → Jasmin (hand) |
|---|---|---|---|
| x86-64 result | 105 | **105, then UAF trap, exit 3** | **105, then UAF trap, exit 3** |
| other ISAs, same source | all GCC targets | **ARMv8, RISC-V, MIPS compiled unchanged** (not run: no emulator) | ARM-M4 / RISC-V: experimental, 32-bit, `u64` handle rejected ("too many large parameters") |
| `u32` | native | words + mask (internal `ld32`/`st32` disabled, §9) | native `u8`/`u32`/`u64` |
| mutable global state | static struct | static heap (`@base`) | globals read-only: pool in caller-owned memory |
| trap | `_zer_trap` | FFI call to C | no C calls: returned status code |
| helpers | calls | calls | no general calling convention: two live handles to one helper conflict in register allocation; inline or spill to `stack` |
| MMIO | `volatile` | `!ld32`/`!st32`/`!st16`/`!ld8` work (shared memory, since March 2024) | not tested |

Findings:

1. **Neither Pancake nor Jasmin has `goto`.** ZER's emitted C is basic blocks + `goto` (from the IR).
   A translator must lower from ZER's **AST** (structured, and still typed) or add a pass that
   restructures the IR's control flow.
2. **Pancake has no division operator** (`/` is a parse error in v3304, v3400, v3479): division and
   modulo need a runtime routine (shift-subtract) or FFI.
3. **Pancake comparisons: `<` is signed, `<+` is unsigned.** `>>>` is the logical shift, `>>` the
   arithmetic one (measured).
4. **Pancake's 2023 "MMIO only through C FFI" limitation is out of date:** shared-memory 8/16/32/word
   loads and stores exist and compile with local variables.
5. **Jasmin is x86-64-first**; its other targets are experimental and 32-bit.

Verdict: **Pancake fits ZER's systems-on-several-ISAs goal better**; Jasmin fits exact types and Coq
but is effectively x86-only today.

Spike tooling: CakeML release `v3479` (`cake-x64-64`, built with `make cake`); `cake --pancake
[--target=x64|arm8|riscv|mips] < prog.pnk > prog.S`; link with `basis_ffi.c` + our FFI file + `-lm`.
Pancake FFI convention: `@name(c, clen, a, alen)` calls C `ffiname(unsigned char *c, long clen,
unsigned char *a, long alen)`. Jasmin `2026.03.2` via `opam install jasmin` in a 2 GB-capped
container; sized memory access is `[:u32 addr]`.

## §8 Lowering spec: integers

**Every ZER integer lives in a 64-bit word, always in canonical form.**

| ZER type | canonical form | restore after an overflowing operation |
|---|---|---|
| `u8` / `u16` / `u32` | zero-extended (value < 2⁸ / 2¹⁶ / 2³²) | `& 0xFF` / `& 0xFFFF` / `& 0xFFFFFFFF` |
| `i8` / `i16` / `i32` | sign-extended | `(x << 56) >> 56`, `(x << 48) >> 48`, `(x << 32) >> 32` (`>>` arithmetic) |
| `u64` / `i64` | the word | none |

| operation | restore? |
|---|---|
| `+`, `-`, `*`, `<<`, `~`, negation | **yes** |
| `&`, `\|`, `^` | no |
| unsigned `>>` | no; use `>>>` |
| signed `>>` | no; use `>>` |
| `/`, `%` | runtime routine |
| comparisons | unsigned types use `<+`, `>=+`, …; signed use `<`, `>=`. A wrong choice is a silent bug |

This is the standard technique for 32-bit integers on 64-bit machines (64-bit RISC-V's `addw`
family, JVMs, WebAssembly engines). It is correct if applied systematically; the risk is a **missed
mask, which fails silently** (garbage in the high bits surfaces later in a comparison or division).

- Generate masks **from the type, in one place** in the translator; never per case by hand (the
  catch-all lesson).
- **Differential testing:** run the same ZER program through ZER → C → GCC and ZER → Pancake on
  many inputs and compare outputs.
- **First thing to prove** about the translator: "canonical form is preserved by every operation".
- **Eager masking first** (after every overflowing operation; simplest to trust). **Lazy masking**
  (only where high bits become observable: comparisons, division, right shift, store, call, return)
  is a later optimisation.
- On Pancake's 32-bit targets (`arm7`) a word is 32 bits: `u32` needs no mask.

**Memory layout** for `u32` fields in internal memory:

1. one word per field: simple, fast, uses more memory;
2. four byte accesses with shifts: exact ZER/C layout, slower; required where layout is observable
   (packed structs, FFI buffers shared with C);
3. `ld32`/`st32` if Pancake re-enables them (§9).

Use (1) for ordinary data and (2) where layout is observable. MMIO always uses the shared-memory
`!ld32`/`!st32`, which work.

## §9 Pancake's internal `ld32`/`st32`

- **Symptom:** any internal `ld32`/`st32` whose operands mention a local variable or parameter fails
  with a spurious "variable … is not in scope" error (v3304, v3400, v3479; 64-bit and 32-bit
  compilers; source `master` at 31b72d0003). Constant operands compile. A **global** variable works.
- **Root cause (read in source):** in `pancake/parser/panPtreeConversionScript.sml`,
  `localise_exp` / `localise_prog` (which mark variables local or global) have no `Load32` /
  `Store32` cases and fall into their catch-alls. They are the only constructors with
  sub-expressions missing. The static checker's own `Store32` handling mirrors `StoreByte` and is
  fine.
- **Maintainer intent:** `pancake/NEWS.md` tags the feature `` <sub>Feature disabled: `32bit`</sub> ``
  (retrofitted by Pancake's lead author in 83e60c30, 2026-08-25), and the compiler's feature list is
  built from those tags. So the maintainers consider it disabled; the defect is at least the
  misleading error.
- **It is a rejection, not a miscompilation:** a verified compiler promises correct code for what it
  accepts, not that it accepts everything. The code Pancake did produce in the spike is covered by
  its proof. A fix is front-end only and needs no new compiler proof (the backend `Load32`/`Store32`
  support was proved in PR #1165); maintainers only rebuild.
- **Reported upstream** by the owner (issue text in `~/Downloads/cakeml-issue-ld32-st32.md`, follow-up
  comment in `~/Downloads/cakeml-issue-comment.md`), with the two-line suggested fix.
- **Not a blocker for ZER:** values use §8's canonical form; MMIO uses the working shared-memory ops;
  only internal-memory `u32` fields pay a memory or speed cost.

## §10 Licences

- **Pancake / CakeML:** BSD-3; ship freely, commercially too.
- **CompCert:** running `ccomp` is non-commercial only (AbsInt licence otherwise); compiled output is
  not mentioned; `runtime/` is BSD; the C/Clight semantics, `lib/`, `common/`, `cparser/`, `export/`
  are LGPL. Building `zerc` with `ccomp` is fine while ZER is non-commercial.
- **GCC:** GPL; bundling it (as ZER does today) is fine.

## §11 Next steps

1. Owner decision: adopt the Pancake route (reversing "emit C permanently"), or keep GCC as default
   and treat Pancake as an optional backend.
2. A real translator spike from ZER's **AST** for a small subset (integers, arrays, structs, `if`,
   `while`, calls), with the §8 masking generated from types.
3. Differential test harness: GCC path vs Pancake path on the existing ZER test programs.
4. Runtime routines: division/modulo, traps.
5. `make CC=ccomp` for `zerc` itself (untested; ZER's own source must fit CompCert's C subset).
6. Watch the upstream `ld32`/`st32` issue.

## Sources

- Pancake: Verified Systems Programming Made Sweeter (PLOS 2023): https://cakeml.org/plos23.pdf
- Verifying Device Drivers with Pancake: https://arxiv.org/pdf/2501.08249
- CakeML: https://github.com/CakeML/cakeml (BSD-3), releases: https://github.com/CakeML/cakeml/releases
- CakeML issue #1131 and PR #1165 (32-bit internal load/store): https://github.com/CakeML/cakeml/issues/1131, https://github.com/CakeML/cakeml/pull/1165
- Jasmin: https://github.com/jasmin-lang/jasmin (MIT); The Jasmin Compiler Preserves Cryptographic Security: https://arxiv.org/pdf/2511.11292
- bedrock2: https://github.com/mit-plv/bedrock2
- CompCert licence: https://github.com/AbsInt/CompCert/blob/master/LICENSE
- seL4 translation validation (PLDI 2013): https://www.cse.chalmers.se/~myreen/pldi13.pdf
