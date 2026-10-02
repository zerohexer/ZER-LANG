# ZER on Pancake — verified compilation without CompCert or GCC

**Status: DESIGN + SPIKES (2026-09-28; concurrency 2026-10-02, §12). Nothing is built beyond the spikes.** Adopting this route
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

**Using this route needs no HOL4 from us.** We only run the CakeML/Pancake compiler (`cake
--pancake`), exactly as in the spike. Its proofs were written and checked by the CakeML team; we
inherit them by using their released compiler. HOL4 would only be needed for optional future work
(proving our translator in the same logic, or proving properties of Pancake programs).

**The compiler binary itself is proved, not just the compiler algorithm.** CakeML is bootstrapped
"in the logic": HOL4 evaluates the compiler on its own source and produces the machine code of the
compiler together with a theorem that this machine code is correct. So the `cake` binary from a
release is not "a verified compiler built by an unverified toolchain". Compare CompCert, whose `ccomp`
binary exists only through Coq's extraction to OCaml and the OCaml compiler, two extra trusted steps.
(Source: the CakeML JFP paper, section 11 "Compiler Bootstrapping".) Pancake is integrated into the
CakeML compiler, so the same released binary compiles Pancake (`--pancake`).

**The assembler barely translates.** CakeML's `.S` output is mostly raw hex bytes of machine code plus
a small wrapper (per the release's `how-to.md`), so the assembler and linker mainly place bytes and
resolve the wrapper's symbols.

Trust, piece by piece:

- **Proved:** the Pancake backend (Pancake AST → machine code), by the CakeML team in HOL4.
- **Proved:** the `cake` compiler binary itself (in-logic bootstrap).
- **Faithful:** the `zerc` binary (built by CompCert).
- **Trusted:** the ZER → Pancake translation (until proved: the canonical-form invariant of §8 is
  the first thing to prove); the ISA models; assembler/linker; silicon. For ZER-safe, also the
  safety checker (until ZER's verification endgame lands).
- **Trusted, small:** C code reached through Pancake FFI (traps, any remaining helpers), and the
  C glue linked with the output (`basis_ffi.c`, our FFI file), compiled by an ordinary C compiler.
- **Trusted, the logic itself:** HOL4's kernel (and the platform it runs on). In-logic bootstrapping
  does not defend against a compromised proof checker ("trusting trust"); it shrinks the trust to one
  small kernel. Verified checkers exist in this ecosystem: the CakeML release ships `candle_boot.ml`,
  Candle, a HOL Light theorem prover verified and compiled with CakeML.
- **Trusted:** the ISA models the backend is proved against; assembler and linker (see above).

In one line: **the remaining trusted pieces are our ZER → Pancake translator, HOL4's kernel, the ISA
models, and the assembler and linker.** Everything between Pancake source and machine code, including
the compiler binary that does the work, is proved.

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
| `u32` | native | words + mask in the spike (internal `ld32`/`st32` were broken in v3479; fixed upstream since, §9) | native `u8`/`u32`/`u64` |
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
3. `ld32`/`st32`: fixed upstream on 2026-09-30 (§9); usable from the first CakeML release that
   includes PR #1506. This becomes the preferred form once that release is pinned.

Use (1) for ordinary data and (2) where layout is observable. MMIO always uses the shared-memory
`!ld32`/`!st32`, which work.

## §9 Pancake's internal `ld32`/`st32` (FIXED UPSTREAM 2026-09-30)

**Status: fixed.** The owner reported it as CakeML issue **#1505**; it was closed by **PR #1506,
"Add missing localise clauses for 32-bit ops"**, merged 2026-09-30. The fix is the two equations
proposed in the report (`Load32` in `localise_exp`, `Store32` in `localise_prog`), and the same PR
changed the NEWS tag from "Feature disabled: `32bit`" to "Feature **enabled**: `32bit`". Releases
up to and including v3479 still have the bug; use the first release that contains #1506. The
record below is kept as the diagnosis.

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
- **Reported upstream** by the owner as issue #1505, with the two-line suggested fix and a follow-up
  comment about the NEWS tag; **fixed by PR #1506** as proposed (see the status note above).
- **Was not a blocker for ZER** even before the fix: values use §8's canonical form and MMIO uses the
  shared-memory ops. With the fix, internal-memory `u32` fields can use `ld32`/`st32` directly, so the
  word-per-field or byte-wise workaround is only needed on releases without #1506.

## §10 Licences

- **Pancake / CakeML:** BSD-3; ship freely, commercially too.
- **CompCert:** running `ccomp` is non-commercial only (AbsInt licence otherwise); compiled output is
  not mentioned; `runtime/` is BSD; the C/Clight semantics, `lib/`, `common/`, `cparser/`, `export/`
  are LGPL. Building `zerc` with `ccomp` is fine while ZER is non-commercial: **confirmed in writing by
  AbsInt on 2026-09-30**, along with "services around the free ZER need no licence" and "selling a
  commercial version requires a project licence, EUR 44,970 perpetual per project and target"
  (`docs/zer-unified-compiler.md` §4.1). Self-hosting through Pancake, or building with GCC, avoids
  that cost for a commercial edition.
- **GCC:** GPL; bundling it (as ZER does today) is fine.

## §11 Next steps

1. Owner decision: adopt the Pancake route (reversing "emit C permanently"), or keep GCC as default
   and treat Pancake as an optional backend.
2. A real translator spike from ZER's **AST** for a small subset (integers, arrays, structs, `if`,
   `while`, calls), with the §8 masking generated from types.
3. Differential test harness: GCC path vs Pancake path on the existing ZER test programs.
4. Runtime routines: division/modulo, traps.
5. `make CC=ccomp` for `zerc` itself (untested; ZER's own source must fit CompCert's C subset).
6. Pin the first CakeML release containing PR #1506 and switch `u32` memory fields to `ld32`/`st32`.
7. Concurrency (`spawn`, `shared struct`): after items 2 to 4, in the order of §12.10.

## §12 Concurrency on the Pancake path (spike 2026-10-02)

**Status: SPIKE ONLY.** A hand-written Pancake component and a C harness, run on x86-64. No ZER
code was involved: nothing here is emitted by `zerc` yet.

### §12.1 The starting fact: verified compilers prove sequential code

| compiler | what its theorem covers |
|---|---|
| CompCert | sequential C. pthreads can be called as external functions, but the theorem says nothing about threads sharing memory |
| CakeML / Pancake | sequential programs, plus memory declared as shared with the outside (§12.3) |
| Bedrock2, Jasmin | sequential |

Proved compilation of shared-memory concurrency exists only as research (CompCertTSO, the concurrent
CompCert work). A Pancake program has no threads, no atomics and no memory-ordering model. This is
not a gap relative to any competitor.

### §12.2 Terms (they were conflated in the design discussion)

| question | options |
|---|---|
| do things run at the same instant? | one core, interleaved (**concurrency**) / several cores (**parallelism**) |
| what do they share? | everything in one address space (**threads**) / nothing except declared regions (**components**) |

"Single-threaded" below describes what each compiled component may **assume** (nobody else touches
its private memory). It does not describe how the whole program runs: the spike ran two components
truly in parallel on two cores.

### §12.3 Three ways to get concurrency

1. **Components (the seL4 pattern; recommended).** Each component is a single-threaded piece of
   code, compiled separately. Something outside (an OS, a small kernel, or a thread-start stub)
   runs them. They communicate through memory regions **outside** every component, read and written
   with Pancake's shared-memory operations (`!ldw`, `!stw`, `!ld32`, `!st32`, `!ld8`, `!st8`), whose
   semantics already says the memory can change between accesses. Each component's proof stays
   honest.
2. **Threads through FFI stubs.** Thread start, lock, unlock and atomics are FFI calls to small
   trusted stubs. This is option 1 inside one process; it is what the spike tested.
3. **Stay on GCC.** `spawn`, `shared struct` and atomics keep working on the C path, with ZER's
   concurrency checks and no proved backend.

**Interrupts** on single-core bare metal are also concurrency. Shape: a tiny trusted stub records
the event in a shared location; the main loop reads it with a shared-memory load. That is option 1.

```
                 process
   +------------------------------------------+
   |  thread 1 (core A)    thread 2 (core B)  |   real OS threads, simultaneous
   |  +------------+       +------------+     |
   |  | instance a |       | instance b |     |   each single-threaded inside,
   |  | own memory |       | own memory |     |   each with its own heap + stack
   |  +-----+------+       +------+-----+     |
   |        +------ lock ---------+           |   FFI stubs (trusted)
   |            shared counter                |   outside both: !ldw / !stw only
   +------------------------------------------+
```

### §12.4 How entry into compiled Pancake code works (read from the generated `.S`)

With `--main_return=true`, `cml_main` runs Pancake's `main` and returns; each `export fun` then
becomes a C-callable symbol. The generated file keeps **one** set of state in ordinary globals:

| symbol | role |
|---|---|
| `cml_heap`, `cml_stack`, `cml_stackend` | the memory region; set by the caller before `cml_main` |
| `ret_base`, `ret_stack`, `ret_stackend` | saved by `cml_return`, reloaded into `r14`/`r12`/`r13` by `cake_enter` on every exported call |
| `can_enter` | re-entry flag: `cake_enter` tests it, jumps to `cake_err3` (`cml_err(3)`) if 0, else sets it to 0; `cake_return` sets it back to 1 |

Consequences:

- **One compiled image = one instance.** Two threads inside it would share one Pancake stack.
- **The re-entry guard is a plain test-then-set, not atomic.** It stopped the bad case 20 of 20 times
  in the spike, but two threads can pass it together. Treat it as a debugging aid, never as the
  mechanism.
- **Several instances need several copies** of those globals, i.e. the object linked more than once
  under different symbol names (§12.5).

### §12.5 The spike

Tooling: CakeML `v3479` (`cake-x64-64`), host GCC 7.5, x86-64 Linux, 20 cores. One Pancake
component, compiled once, linked **twice** into one process (symbols renamed with `objcopy`), driven
by two pthreads.

| set-up | result |
|---|---|
| **two instances, one per thread; shared counter under an FFI lock** | **correct 10/10** (6,000,000 of 6,000,000 increments); each instance's private count intact (3,000,000 each) |
| two instances, shared counter **without** the lock | wrong 10/10 (e.g. 3,746,453 of 6,000,000): a real data race. Compiles with no warning |
| one instance, two threads, every call serialised by one outer lock (a monitor) | correct 10/10; no parallelism inside the instance, and its private state is one copy seen by both threads |
| one instance, two threads, no serialisation | stopped 20/20 by the re-entry guard (`cml_err(3)`) |
| private work only: two instances in parallel vs one after the other | 1.88x to 1.99x faster, identical results |

The same source compiles for `--target=arm8` and `--target=riscv` with the same entry guard; those
were **not run**.

Findings:

1. **Two instances of one component can live in one process.** Not known before the spike.
2. **Pancake does not prevent data races on shared memory.** The enforcement must come from above:
   ZER's `shared struct` rule.
3. **Parallelism is real**, not simulated.

Build commands:

```sh
cake --pancake --main_return=true < worker.pnk > worker.S
gcc -c -o worker.o worker.S
for p in a_ b_; do
  nm worker.o | awk -v p=$p '$2 ~ /[TDBR]/ {print $3, p $3}' > syms_$p.txt   # defined globals only
  objcopy --redefine-syms=syms_$p.txt worker.o worker_$p.o
done
gcc -O2 -pthread -o conc harness.c worker_a_.o worker_b_.o
./conc two-instances-locked 3000000     # also: two-instances-racy, one-instance-monitor,
                                        #       one-instance-raw, speed
```

The renaming covers `cml_main`, `cml_heap`, `cml_stack`, `cml_stackend`, the four buffer/text
markers and the exported functions. The file-local labels (`ret_base`, `can_enter`, ...) are already
per-object. Undefined references (`cml_err`, `cml_clear`, `ffilock`, `ffiunlock`) stay shared.

`worker.pnk`:

```
// One ZER "thread body" as a Pancake component.
//   private state : word at @base (this instance's own memory)
//   shared state  : a counter OUTSIDE Pancake memory, reached only with !ldw / !stw
//   locking       : FFI stubs (trusted), like a ZER shared struct's auto-lock

// increments the shared counter n times under the lock; returns private count
export fun work_locked(1 shared, 1 n) {
  var i = 0;
  var v = 0;
  while i <+ n {
    @lock(0, 0, 0, 0);
    !ldw v, shared;
    v = v + 1;
    !stw shared, v;
    @unlock(0, 0, 0, 0);
    var p = lds 1 @base;
    st @base, p + 1;
    i = i + 1;
  }
  var r = lds 1 @base;
  return r;
}

// same, but WITHOUT the lock: a data race on the shared counter
export fun work_racy(1 shared, 1 n) {
  var i = 0;
  var v = 0;
  while i <+ n {
    !ldw v, shared;
    v = v + 1;
    !stw shared, v;
    var p = lds 1 @base;
    st @base, p + 1;
    i = i + 1;
  }
  var r = lds 1 @base;
  return r;
}

// pure private computation (for the parallel-speed measurement)
export fun spin(1 n) {
  var i = 0;
  var acc = 0;
  while i <+ n {
    acc = (acc * 31 + i) & 4294967295;
    st @base + 8, acc;
    i = i + 1;
  }
  return acc;
}

export fun reset() {
  st @base, 0;
  return 0;
}

fun main() {
  st @base, 0;
  return 0;
}
```

`harness.c`:

```c
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* two separately-linked copies of the same compiled Pancake component */
#define INST(p) \
  extern void p##cml_main(void); extern void *p##cml_heap, *p##cml_stack, *p##cml_stackend; \
  extern long p##work_locked(long, long), p##work_racy(long, long), p##spin(long), p##reset(void);
INST(a_) INST(b_)

/* runtime hooks the generated code expects */
void cml_exit(int c) { fprintf(stderr, "  [pancake runtime: cml_exit(%d)]\n", c); exit(40 + c); }
void cml_err(int c)  { fprintf(stderr, "  [pancake runtime: cml_err(%d)%s]\n", c,
                               c == 3 ? " = re-entered while already running" : ""); exit(40 + c); }
void cml_clear(void) {}

/* trusted FFI stubs: the lock a ZER shared struct would carry */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
void ffilock(unsigned char *c, long cl, unsigned char *a, long al)   { pthread_mutex_lock(&mu); }
void ffiunlock(unsigned char *c, long cl, unsigned char *a, long al) { pthread_mutex_unlock(&mu); }

static void boot(void **heap, void **stack, void **stackend, void (*m)(void)) {
  size_t h = 1 << 20, s = 1 << 20;
  char *p = malloc(h + s);
  *heap = p; *stack = p + h; *stackend = p + h + s;
  m();                                   /* runs Pancake main(), which returns */
}

static volatile long shared_counter;
static long N;
static pthread_mutex_t entry = PTHREAD_MUTEX_INITIALIZER;
typedef long (*work_fn)(long, long);
struct job { work_fn f; int serialise; long priv; };

static void *run(void *v) {
  struct job *j = v;
  if (j->serialise) {                    /* monitor: one thread inside the image at a time */
    long r = 0;
    for (long i = 0; i < N; i++) { pthread_mutex_lock(&entry); r = j->f((long)&shared_counter, 1); pthread_mutex_unlock(&entry); }
    j->priv = r;
  } else j->priv = j->f((long)&shared_counter, N);
  return 0;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *run_spin_a(void *v) { *(long *)v = a_spin(N); return 0; }
static void *run_spin_b(void *v) { *(long *)v = b_spin(N); return 0; }

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "";
  N = argc > 2 ? atol(argv[2]) : 200000;
  boot(&a_cml_heap, &a_cml_stack, &a_cml_stackend, a_cml_main);
  boot(&b_cml_heap, &b_cml_stack, &b_cml_stackend, b_cml_main);
  pthread_t t1, t2;
  struct job j1 = {0}, j2 = {0};

  if (!strcmp(mode, "two-instances-locked"))      { j1.f = a_work_locked; j2.f = b_work_locked; }
  else if (!strcmp(mode, "two-instances-racy"))   { j1.f = a_work_racy;   j2.f = b_work_racy; }
  else if (!strcmp(mode, "one-instance-raw"))     { j1.f = a_work_locked; j2.f = a_work_locked; }
  else if (!strcmp(mode, "one-instance-monitor")) { j1.f = a_work_racy;   j2.f = a_work_racy; j1.serialise = j2.serialise = 1; }
  else if (!strcmp(mode, "speed")) {
    long r1, r2, r3, r4; double t0 = now();
    r1 = a_spin(N); r2 = b_spin(N);
    double seq = now() - t0; t0 = now();
    pthread_create(&t1, 0, run_spin_a, &r3); pthread_create(&t2, 0, run_spin_b, &r4);
    pthread_join(t1, 0); pthread_join(t2, 0);
    double par = now() - t0;
    printf("sequential %.3fs  parallel %.3fs  speedup %.2fx  results %s\n", seq, par, seq / par,
           (r1 == r3 && r2 == r4 && r1 == r2) ? "identical" : "DIFFER");
    return 0;
  } else { fprintf(stderr, "mode?\n"); return 2; }

  pthread_create(&t1, 0, run, &j1); pthread_create(&t2, 0, run, &j2);
  pthread_join(t1, 0); pthread_join(t2, 0);
  long want = 2 * N;
  printf("shared=%ld (want %ld) %s | private: t1=%ld t2=%ld\n", shared_counter, want,
         shared_counter == want ? "OK" : "LOST UPDATES", j1.priv, j2.priv);
  return shared_counter == want ? 0 : 1;
}
```

### §12.6 Mapping ZER onto it

| ZER | Pancake path |
|---|---|
| `spawn f(...)` | start a thread on its own instance of the component holding `f` (trusted stub) |
| data not marked `shared` | the instance's own memory; Pancake's sequential theorem applies |
| `shared struct`, atomics, `Ring` | memory **outside** every instance, reached only with shared-memory operations; lock/unlock and atomic operations as FFI stubs |

Why ZER is a good fit: Pancake's theorem assumes nobody else changes the program's ordinary memory.
For hand-written multi-threaded Pancake that is an unchecked hope. ZER's checker already enforces
it at source level (non-shared pointers and Handles cannot be passed to `spawn`; a spawn target's
body is scanned for unsynchronised global access; CLAUDE.md "Thread data race"). So ZER can state
**why** the sequential proof applies per thread.

Open design points (not solved by the spike):

- **Globals.** A ZER global is one object for the whole process; with one instance per thread, a
  global placed in instance memory would silently become one copy per thread. Every global reachable
  from more than one thread body must live in the outside region.
- **Scoped spawn.** ZER allows a non-shared pointer into a `spawn` when the thread is joined in scope
  (`ThreadHandle` + join). On this path that pointer would reach into another instance's private
  memory. Either such data moves to the outside region, or the form is refused on this path.
- **Code size.** Each instance is a full copy of the component's code.

### §12.7 Who owns the lock

| | who decides where to lock | what the lock is |
|---|---|---|
| ZER on GCC (today) | ZER's emitter, per statement touching a `shared struct` | `pthread_mutex_lock`/`unlock` written into the emitted C (`emitter.c`, the `_zer_mtx` field; recursive mutex, BUG-473) |
| ZER on Pancake (design) | the same rule, in the Pancake emitter | an FFI call to a stub: pthread on a hosted system; interrupt disable/enable on single-core bare metal; a spinlock on an atomic instruction on multicore bare metal |

The placement is ZER's on both paths. The primitive is borrowed on both paths.

Trust ledger for the concurrent path (additions to §6):

| item | status |
|---|---|
| each instance's code between FFI calls | **proved** (Pancake backend) |
| lock, unlock, thread-start and atomic stubs, and the platform lock under them | trusted; specified by the effect table (`docs/asm_lang_zer_safe.md`) |
| the emitter placing lock calls and choosing instance vs outside memory | trusted, tested (differential against the GCC path) |
| "no instance touches another's private memory" | from ZER's checker (not yet proved) plus the stubs |
| the `objcopy` renaming step | trusted build step; a workaround, not a CakeML feature |
| weak memory ordering on multicore | not modelled anywhere; the stubs must use the right barriers |

### §12.8 Intrinsics on the Pancake path

ZER's intrinsics fall into four groups. The lowering is chosen by the emitter; source code is the
same on both paths.

| group | examples | Pancake path | status |
|---|---|---|---|
| 1. pure computations | `@popcount`, `@clz`, `@ctz`, `@bswap*`, `@addc`/`@subb`, `@mulw`, `@truncate`, `@saturate` | Pancake functions (`inline fun`): bit tricks, loops, wide multiply in halves | proved; slower than one machine instruction |
| 2. MMIO | `volatile` register access via `@inttoptr` | shared-memory operations | proved, with device behaviour as the floor |
| 3. atomics and barriers | `@atomic_*`, `@barrier*`, `@cond_*` | not expressible; FFI stubs per ISA | trusted |
| 4. privileged / CPU-specific | `@cpu_disable_int`, MSR/CR access, port I/O, `@probe` | not expressible; FFI stubs per ISA | trusted |
| hints | `@expect`, `@unreachable` | dropped, or a trap | n/a |

For groups 3 and 4 the **effect table** (pre/postconditions per intrinsic, per-ISA entries) is the
specification of the stubs. Pancake models an FFI call as an interaction with an external oracle, so
its theorem holds relative to what the stubs do; the effect table is where that is written down.
The effect-row fold rules and QEMU conformance witnesses stay optional (GCC-path raw asm only).

### §12.9 Limits of the spike

- Run on x86-64 only. ARMv8 and RISC-V compile; not executed.
- A plain mutex only. Atomics, condition variables, `shared(rw)` and `Ring` were not tested.
- A test, not a proof: Pancake's theorem covers each instance alone.
- `objcopy` renaming is a workaround in the trusted build.
- Thread start was `pthread_create` in the C harness; there is no `spawn` stub yet.

### §12.10 Build order for concurrency

1. The single-threaded Pancake emitter (§11 items 2 to 4). Nothing below can start before it.
2. `shared struct` lowering: outside-region placement, `!ldw`/`!stw` access, lock calls by the C
   emitter's existing per-statement rule.
3. `spawn` lowering: one instance per thread body, plus the start stub; settle the globals and
   scoped-spawn points of §12.6.
4. The stubs per platform, each with its effect-table entry.
5. The multi-instance build step. Worth raising upstream: a supported way to produce several
   instances (a symbol-prefix option) would remove the `objcopy` workaround.
6. Run the spike on ARMv8 and RISC-V under an emulator.

## References (for a future paper's related work)

From the CakeML publications page (numbering is that page's), grouped by what each supports here.

**Primary: Pancake itself** (not on that page's list)

- Johannes Åman Pohjola et al. *Pancake: Verified Systems Programming Made Sweeter.* PLOS 2023.
  https://cakeml.org/plos23.pdf — the language, its verified compiler, the seL4 driver case study and
  the performance numbers quoted in §7-§9.
- *Verifying Device Drivers with Pancake.* arXiv 2025. https://arxiv.org/pdf/2501.08249

**Precedent: other languages compiled through CakeML's verified backend** (the ZER → Pancake pattern;
each proved its own front-end translation, so these are the models if the translator is ever proved)

- [3] Nezamabadi, Myreen, Tan. *Verified VCG and verified compiler for Dafny.* CPP 2026.
- [4] Lasnier, Yallop, Myreen. *Brack: A verified compiler for Scheme via CakeML.* CPP 2026.
- [40] Hupel, Nipkow. *A verified compiler from Isabelle/HOL to CakeML.* ESOP 2018.
- [20] Åman Pohjola, Gómez-Londoño, Shaker, Norrish. *Kalas: A verified, end-to-end compiler for a
  choreographic language.* ITP 2022.

**Claims made in this document**

- [30] Tan, Myreen, Kumar, Fox, Owens, Norrish. *The verified CakeML compiler backend.* JFP 29, 2019.
  Canonical backend reference; section 11, "Compiler Bootstrapping", is the in-logic bootstrap (§6).
- [47] Same authors. *A new verified compiler backend for CakeML.* ICFP 2016 (conference version of [30]).
- [45] Fox, Myreen, Tan, Kumar. *Verified compilation of CakeML to multiple machine-code targets.*
  CPP 2017. The multi-ISA claim (§4).
- [38] Kumar, Mullen, Tatlock, Myreen. *Software verification with ITPs should use binary code
  extraction to reduce the TCB.* ITP 2018. Why in-logic compilation beats extraction + an OCaml
  compiler (the `ccomp` comparison, §6).
- [18] Kanabar, Fox, Myreen. *Taming an authoritative Armv8 ISA specification: L3 validation and
  CakeML compiler verification.* ITP 2022. Reducing trust in the ISA model (§6).
- [31] Lööw, Kumar, Tan, Myreen, Norrish, Abrahamsson, Fox. *Verified compilation on a verified
  processor.* PLDI 2019. The chain extended to a verified CPU (Silver).
- [25] Gómez-Londoño, Åman Pohjola, Syeda, Myreen, Tan. *Do you have space for dessert? A verified
  space cost semantics for CakeML programs.* OOPSLA 2020. The out-of-memory caveat in correctness
  theorems.
- [21] Myreen. *A minimalistic verified bootstrapped compiler (proof pearl).* CPP 2021. Inspiration
  for `zerc` compiling itself through the proved backend.
- [19] Becker, Rabe, Darulova, Myreen, Tatlock, Kumar, Tan, Fox. *Verified compilation and
  optimization of floating-point programs in CakeML.* ECOOP 2022. CakeML handles floats; Pancake does
  not yet (the float gap in §7 / Next steps).
- [17] Abrahamsson, Myreen, Kumar, Sewell. *Candle: A verified implementation of HOL Light.* ITP 2022;
  [5] extended version, JAR 2025. The verified prover shipped in the release (trusting trust, §6).
- [24] Myreen. *The CakeML project's quest for ever stronger correctness theorems.* ITP 2021. Overview.
- [55] Kumar, Myreen, Norrish, Owens. *CakeML: A verified implementation of ML.* POPL 2014. The
  project's canonical reference.

**Related approaches (compared against in this document)**

- Sewell, Myreen, Klein. *Translation validation for a verified OS kernel.* PLDI 2013 (seL4's GCC
  output validation).
- O'Connor et al. *COGENT: Certified Compilation for a Functional Systems Language.*
  https://arxiv.org/pdf/1601.05520 (certifying compilation: a proof per program).
- Leroy et al. CompCert (C → assembly, Coq); Monniaux, Boldo. *The Trusted Computing Base of the
  CompCert Verified Compiler.* https://arxiv.org/pdf/2201.10280

## Sources

- Pancake: Verified Systems Programming Made Sweeter (PLOS 2023): https://cakeml.org/plos23.pdf
- Verifying Device Drivers with Pancake: https://arxiv.org/pdf/2501.08249
- CakeML: https://github.com/CakeML/cakeml (BSD-3), releases: https://github.com/CakeML/cakeml/releases
- CakeML issue #1131 and PR #1165 (32-bit internal load/store): https://github.com/CakeML/cakeml/issues/1131, https://github.com/CakeML/cakeml/pull/1165
- Jasmin: https://github.com/jasmin-lang/jasmin (MIT); The Jasmin Compiler Preserves Cryptographic Security: https://arxiv.org/pdf/2511.11292
- bedrock2: https://github.com/mit-plv/bedrock2
- CompCert licence: https://github.com/AbsInt/CompCert/blob/master/LICENSE
- seL4 translation validation (PLDI 2013): https://www.cse.chalmers.se/~myreen/pldi13.pdf
