# CLAUDE.md

> **This file is loaded into EVERY session, so it holds RULES and POINTERS only.** The long
> form — case studies, per-bug history, full tables — lives in docs read on demand:
>
> | Need | Read |
> |---|---|
> | A safety question answered at N sites: its ONE query, its gate, its history | `docs/multi-site-classes.md` |
> | Process lessons with their evidence (tests, audits, harvesting, debugging, proofs) | `docs/session-playbook.md` |
> | What the code cannot tell you: why, rejected alternatives, cross-file contracts, traps | `docs/compiler-internals.md` (`grep -n '^## '` is its table of contents — read the section, never the whole file) |
> | Dated session / migration / per-bug records — grep only | `docs/compiler-internals-history.md` |
> | Coq / Iris / VST | `docs/proof-internals.md` |
> | What is open NOW (meant to be read whole; gated) | `docs/limitations.md` |
> | A closed entry's discussion — grep by BUG number, never read whole | `docs/limitations-closed.md` |
> | Past bugs and root causes | `BUGS-FIXED.md` |
> | The language as users see it | `docs/reference.md`, `ZER-LANG-LLM.md` (compact) |
>
> An older doc citing `CLAUDE.md "<section title>"` that is not below: the section moved
> verbatim (2026-11-13) — grep the title in `docs/session-playbook.md` or
> `docs/multi-site-classes.md`.
>
> **This file has a byte budget, enforced by `make check` (`tools/audit_docs_budget.sh`).**
> Three kinds of content earn a place: repo facts no model can know, locked decisions, and
> the owner's working preferences — one line each. Generic engineering advice does not.
> Where a gate enforces a rule, the rule shrinks to "gate X exists".
>
> **Adding a lesson: one or two lines HERE, the story in the doc that owns it.** Nothing here
> quotes counts, cell numbers or dated history — those drift. RUN the gate; count from the tree.

## ZER's Goal — Program-Consequence Coverage (Locked Architectural Commitment)

**ZER's safety claim, stated precisely:**

> ZER guarantees 100% program-consequence coverage: every wrong use of a value in ZER source code is caught at the use site. This includes embedded/firmware data — once a value crosses into ZER through a typed boundary (intrinsic return, MMIO read, cinclude function, linker symbol, asm output, source literal), it is program data and ZER verifies every program-level operation on it (bounds, type, qualifier, region, context, lifetime, ownership, escape, provenance). Hardware-consequence — peripheral side effects, datasheet-specific value correctness, silicon behavior — is floor, out of scope for any language, surfaced at the narrowest typed boundary for code review but not verified by the compiler.

### The vocabulary that must not equivocate

The word "consequence" carries two meanings; conflating them produces false claims:

- **Program-consequence** — what happens when a value is used wrongly inside ZER source. Caught at 100%. The use is in the program, so ZER owns it.
- **Hardware-consequence** — what happens when a hardware fact is wrong relative to user belief (peripheral doesn't actually clear on read, baud value is wrong-for-this-board). Floor. The fact lives in the datasheet, never enters the program, so ZER has nothing to verify.

Any external claim must use the split. Never let one word "consequence" cover both meanings — that's the equivocation that produced earlier false "100% safe" framings.

### The boundary

ZER's boundary is **provenance of facts, not provenance of values.** Once a value crosses into ZER it is program data regardless of where it came from (hardware register, file, network, literal). Once a fact lives in the datasheet rather than the source, it is hardware-domain regardless of how closely the program relates to it. ZER is 100% over program-domain (operations on values in the program). ZER is silent on hardware-domain (silicon behavior, datasheet facts).

### Why ZER exists

ZER exists because escape hatches (Rust's `unsafe`, contract-based verification that accepts wrong user claims) compromise the safety theorem. ZER's theorem holds for all programs because:

- **No in-language `unsafe` keyword.** The grammar enforces that values must enter through typed boundaries (checker.c:5601-5608 — no integer-to-pointer cast except through `@inttoptr` with mandatory `mmio`).
- **Closure argument is grammar-enforced**, not "modulo `unsafe`." This is the position Anders et al. 2024 identified as "infinite unsafe impedance" — hypothetically possible but absent from production languages. ZER occupies it.
- **No contract-based verification of hardware properties** (Definition B). User contracts about hardware can be wrong; ZER refuses to accept claims it cannot verify against reality. Instead: ZER provides explicit-intent primitives where the user's claim is visible at the use site (audit visibility) rather than hidden in contract specifications.
- **Definition A scoping discipline.** ZER verifies access mechanism structurally; semantic correctness against external substrates (hardware spec, vendor HAL, linker script) is user responsibility supplied through primitive choice. The trust gap is bounded by frozen catalog (~33 safety classes, ~12 firmware structural relationships) — not unbounded per-call contracts.

### Document map for the safety claim

- **`docs/primitives-data-races.md`** — Definition A architecture across five safety domains (memory, type, ASM, concurrency, I/O), the closure argument, anti-patterns, the drift pattern documented in §22-23
- **`docs/asm_lang_zer_safe.md`** — Level C (delegate ISA-specific to GCC) and Level D (user-extensible intrinsics with finite verifier)
- **`docs/universal_pointer.md`** — 4-axis pointer safety with 8-desiderata conservation analysis
- **`docs/firmware_safety_extensions.md`** — program-consequence vs hardware-consequence vocabulary, finite firmware shadow set, four firmware extension gaps (`@section`, region kinds as type discipline, vector table convention, linker symbols), explicit anti-patterns including the drift caught in earlier drafts
- **`docs/proof-internals.md`** — Coq+Iris+VST proof infrastructure (author review pending per proof-internals.md disclosure)
- **`docs/limitations.md`** — documented gaps that bound the "today" claim below the target (naked attribute IR-path drop, intrinsic VRP narrowing missing, bare-metal `.bss` zeroing build-system contract, ~2% opaque destructor heuristic). None permit silent program-consequence violations.

### What ZER explicitly is NOT

- Not "100% safe" without qualifier (that's the equivocation trap)
- Not "memory safe with footnotes" (that's Rust's `unsafe` problem)
- Not contract-based verification (that's SPARK; user contracts can be wrong about hardware)
- Not a verified compiler for hardware semantics (no language is; the floor exists by physics)
- Not "production-ready for any specific board" (requires vendor HAL + BSP work via cinclude)
- Not a replacement for SPARK in safety-critical certified domains (different audience, different trade-off)

### What ZER IS

- Memory-safe systems language with grammar-level closure (no in-language `unsafe`)
- 100% program-consequence coverage at the use site for every value in ZER source
- Unified safety architecture across five domains via Definition A
- Audit-visible primitive selection (user's hardware claim visible at use site, not hidden in contract spec)
- Architecture-agnostic (delegates ISA to GCC, vendor-specific to user libraries via cinclude)
- Currently shipping (publicly available, VS Code extension, GitHub repo)

### The Verification Endgame — 100% sound checker via core λZER (LOCKED DIRECTION)

Full text: `docs/session-playbook.md` "The Verification Endgame". The plan is WRITTEN:
**read `docs/unified-oracle-proved-ZER.md` before any work on unification, oracles or the
per-class safety architecture. Do NOT re-derive it.**

- **Goal:** a machine-checked 100%-SOUND checker — `accepted(p) → ∀ reachable c, ¬ bad(c)` —
  for all programs, over the REAL checker. Soundness is Rice-immune. 100% PRECISION is
  impossible and is NOT a goal: an over-rejected program never runs, so it is harmless.
  ZER is annotation-free, so the theorem certifies the auto-inference itself.
- **Method:** core λZER (~15-20 semantic primitives) with TWO judgments — a type system
  (statics; lives in `checker.c` on the AST) and an operational semantics (dynamics;
  IR-shaped) — plus a VERIFIED desugaring and a forward simulation. A missing abstract
  state is a diagram that will not close = a stuck proof. Not 53 per-node semantics.
  Never say "core = IR" (that drops the type-system half).
- **Scope:** language level. Floors, not failures: `cinclude` / C-FFI, hardware. Separate
  axis: emitter + GCC — a sound CHECKER is not a verified compiler to asm.
- **Sequencing:** not Coq-first while the C checker still moves. Core semantics + a UAF
  vertical slice first, then class by class, then one `accepted → safe` theorem.
  Oracle-driven always: spec against the SEMANTICS, never against current C.
- **Current gap:** the per-class oracles prove each transfer sound over the states LISTED,
  not that the listed states COVER the semantics.

## Critical gotchas (each cost at least one cycle — long form: `docs/session-playbook.md`)

**Build / tooling**
- **Stale objects.** The Makefile has ZERO header dependencies. After touching ANY `.h` — or
  whenever a `make zerc` build rejects trivial programs or segfaults on a test that just
  passed — `rm -f *.o src/safety/*.o` before `make zerc`. Sanitizers cannot see this class;
  `objdump` can.
- **`make check` reports failure ONLY in its exit code.** Always record `MAKE_CHECK_EXIT=$?`.
  Make stops at the first failing step, so every later gate silently never runs. Verify each
  gate by ITS OWN verdict line (table below) — never `grep 'OK — no'`, which matches an
  earlier gate. On a Windows checkout normalise CRLF first
  (`find . -name "*.sh" -exec sed -i 's/\r$//' {} +`) or the run dies before the audits.
- `make check` takes 5-25 minutes: run it in the background and wait on a status file; do not
  poll by hand (`pgrep -x make`, not `pgrep -f`). `grep -c error build.log` lies
  (`-Werror=switch` is on the command line); use `grep -cE '^[^ ]*\.c:[0-9]+:[0-9]+: error:'`.
- **`zerc f.zer -o out.c` is the checker-only probe** (a `.c` suffix means emit only; exit 1
  on a checker error). Any other `-o` path — `/dev/null` included — runs GCC, which rejects
  for reasons of its own (every `interrupt` block on hosted x86-64) and builds the exe NEXT
  TO THE SOURCE. Read the diagnostic, never the exit code.
- **Windows host (the dev machine).** Never execute compiled binaries natively and never use
  a writable bind mount (unsigned `.exe` = Defender Wacatac). Use `make docker-check`, or the
  `:ro` mount + tar-into-container pattern (playbook "Ad-hoc Docker verify"; VST and lattice
  variants beside it). An agent that compiles must be told the same; after any agent run,
  `git status` + `git diff` — a "design only" agent may still edit files.
- `test_modules/run_tests.sh` uses a relative `../zerc`: run it FROM `test_modules/`.
- `audit_type_dispatch.sh` matches inside comments: never write the literal raw kind
  comparison in a comment; in code use `type_dispatch_kind(t) == TYPE_X`.
- Coq comments NEST: a C example like `(*opaque)` inside a `(* … *)` comment opens another.

**Compiler structure**
- **Two emitter dispatch paths.** Every intrinsic / coercion / safety wrapper needs a handler
  in BOTH the AST path and the IR-rewritten path of `emitter.c` (`grep -n '"name"' emitter.c`
  must show two hits), or the IR path emits a placeholder that segfaults at run time.
- **`type_name()` rotates TWO static buffers** (types.c): a third call in one `printf` /
  `checker_error` overwrites the first. Copy each result into a local buffer.
- **Entry points.** The compile path is `zerc_main.c` → `checker_check_bodies` +
  `check_keep_inference` + `checker_post_passes_files`; `checker_check` is the LSP / unit-test
  path. Wire a new deferred pass in BOTH. A whole-program pass goes INSIDE
  `checker_post_passes_files`, which is fed every module — never a second call in the driver.
- **Never hold two different shared-struct locks at once**: per-statement locking is what
  makes lock order deadlock-free by construction.
- **An AST node rewritten in place by `ir_lower` must survive a second lowering** (record it
  through `hoist_undo` / `hoist_restore`); the shim and `test_firmware` lower twice — run
  `test_firmware` after touching `ir_lower.c`.
- **A terminator is the last instruction of its block** (`ir_add_inst_checked`; exit
  consumers ask `ir_block_is_live_return`). A `defer` push without a reachable fire is a
  hard `ir_validate` error. A new IR op needs its `ir_validate` case.
- **A ternary between two FORMAT STRINGS switches `-Wformat` off**: use two calls.
- **An exhaustive switch proves every kind was CONSIDERED, not considered correctly.** When
  you put a kind in a no-op arm, write WHY it is a no-op.
- Layout-fragile Heisenbug (vanishes under prints / ASan / `-O0`): env-gate code paths on the
  SAME binary, `gdb` break + `bt`, `objdump` the emitted code (playbook, same title).

**Escape / keep analysis** (map: compiler-internals.md "Escape & keep analysis")
- The policy for every call-result sink is ONE function, `call_result_escapes`; "which non-keep
  params may this value carry?" is ONE query, `keep_value_roots`. Call them; never re-inline.
- **Forming a reference aliases; reading a value does not.** A widened alias / escape rule
  must be rooted at `&expr` or a slice; a bare `h.field` / `s[0]` reads a value out. Reading a
  POINTER-carrying field yields a reference, a SCALAR field does not — gate every
  field-reading alias arm on the field type (`test_modules/move_user.zer` broke twice on this).
- Returning a view of a slice / pointer PARAMETER is allowed (the call site rejects a caller
  that lets a local escape through it); returning a view of a LOCAL is not.

## MULTI-SITE SAFETY — the #1 recurring bug class

The SAME semantic question is answered independently at N sites; a fix at one leaves the
siblings broken, and a NEW site / node kind / value shape is silently missed. Every hole of
this kind was red-team-found, never proof-found. **Before editing any analysis with this
shape: (1) ENUMERATE the sibling set, (2) fix / verify EVERY member — a green test at one
site proves nothing about the others, (3) add the new shape's cell to the class's gate IN
THE SAME COMMIT.** The durable end-state is ONE query + a gate, never N call sites.

- **Choosing the gate.** Sites are a `switch` on an enum → `-Werror=switch`, no `default:`
  (an if-chain over node kinds is invisible to both walker audits: convert it, do not
  extend it). Scattered sinks → an audit-script baseline (`file:content`). "Do the listed
  forms COVER the semantics?" → a Coq max-oracle.
- **A gate that has only ever printed OK is a script, not a net.** Break the compiler on
  purpose (or run the grid against a pre-fix build: `ZER_MATRIX_ZERC=<path> ./test_x_matrix`)
  and watch it go RED — this applies to the gate's SCOPE as much as to its assertion.
- **A stale gate is worse than none.** Remove the cell / baseline row when a site is unified
  away; add one when a shape appears.
- An EXEMPTION (`volatile`, `@once`, "scoped spawn is joined") is a multi-site class too:
  grep every site granting it, and check each qualifier of its written rationale is TESTED.

**The index** — class → the ONE query → gate — is the first screen of
`docs/multi-site-classes.md`. **Read it before editing any safety analysis**, then the full row
of the class you are touching. A new class gets its index line and its row there in the
same commit as the code.

## What a green `make check` covers

`MAKE_CHECK_EXIT=0` AND each verdict line present — grep the SPECIFIC line:

| gate | its verdict line |
|---|---|
| `walker_audit.sh` | `OK — no gaps. IR emitter covers every node kind the AST emitter does.` |
| `walker_default_audit.sh` | `OK — no default: clauses remain in node-kind / op-kind switches.` |
| `audit_walker_fields.sh` | `OK — no new walker field-coverage gaps` |
| `audit_fixed_buffers.sh` | `OK — no new fixed-size buffer declarations.` |
| `audit_type_dispatch.sh` | `OK — no new raw type-dispatch sites.` |
| `audit_carrier_dispatch.sh` | `OK — no new hand-rolled carrier dispatches.` |
| `audit_esc_restore.sh` | `OK — every realloc'd Checker buffer is kept by esc_restore` |
| `audit_limitations.sh` | `OK — limitations.md holds open entries only` |
| `audit_docs_budget.sh` | `OK — every budgeted document is within its size budget.` |
| `emit_audit.sh` | `OK — no dead-stub markers in emitted C` |
| `sink_matrix.sh` | `SINK MATRIX CLEAN` |
| `audit_reference_examples.sh` | `OK — every non-baselined reference.md example builds.` |
| `audit_float_literal.sh` | `OK — every emitted float literal goes through emit_double_lit.` |
| `audit_matrix.sh` | `OK — every construct x context verdict holds.` |

Plus the axis-crossed grids `tests/test_*_matrix.c` (shape, escape, keep, cflow, conc,
view-alias, hw, async, asm, defer-goto, vrp-position, vrp-fact), each printing
`=== <name>-matrix: N/N cells correct ===`, and `tests/test_lib.sh` (the standard library
in `lib/` builds and runs). A grid may assert a RUNTIME VALUE, not just accept / reject —
use that when the defect is a silent miscompile. Grids are safe to run concurrently (each
uses `ZT()` from `tests/zer_tmp.h`; a new grid must too).

**NOT in `make check` — run by hand:** `tools/ubsan_sweep.sh`, `tools/ub_sweep.sh`,
`tools/grammar_closure_probe.sh`, `tools/qualifier_closure_probe.sh`,
`tools/cross_target_sweep.sh`, the firmware examples. **Nothing in `make check` compiles
the emitted C for any target but the host**: after touching per-arch emission (`@critical`,
`@cpu_*`, barriers, the trap) run `bash tools/cross_target_sweep.sh`.

## ZER Language — Complete Quick Reference

ZER is memory-safe C. Same syntax, same mental model — but the compiler prevents buffer overflows, use-after-free, null dereferences, and silent memory corruption. Compiles to C, then GCC handles backends.

### Primitive Types
```
u8  u16  u32  u64        unsigned integers
i8  i16  i32  i64        signed integers
uN  iN                   arbitrary-width integers (u21, i48, u3 … N=1..128; native, carrier+mask, wraps at 2^N)
usize                    pointer-width unsigned (width auto-detected from the target GCC; --target-bits overrides)
f32  f64                 floating point
bool                     true / false (NOT an integer — no bool↔int coercion)
void                     return type only, cannot declare void variables
```

### Compound Types
```
u8[256] buf;             fixed array — NOTE: size AFTER type, BEFORE name
[*]u8 data;              dynamic pointer to many — {ptr, len}, bounds checked (replaces []T)
[]u8 data;               DEPRECATED — same as [*]u8, warns "use [*]T instead"
*Task ptr;               pointer — guaranteed non-null
?*Task maybe;            optional pointer — might be null (null sentinel, zero overhead)
?u32 result;             optional value — struct { u32 value; u8 has_value; }
?bool flag;              optional bool — struct { u8 value; u8 has_value; }
?void status;            optional void — struct { u8 has_value; } (NO value field!)
*opaque raw;             type-erased pointer (C's void*)
```

### Builtin Container Types
```
Pool(Task, 8) tasks;     fixed-slot allocator — compile-time count, ALWAYS global
Slab(Task) tasks;         dynamic slab allocator — grows on demand, ALWAYS global
Ring(u8, 256) rx_buf;    circular buffer — ALWAYS global
Arena scratch;            bump allocator — (over, alloc, alloc_slice, reset)
Barrier b;               thread sync point — (@barrier_init, @barrier_wait)
Handle(Task) h;          u64: index(32) + generation(32), not a pointer
Semaphore(3) slots;      counting semaphore — @sem_acquire/@sem_release, thread-safe
```

### User-Defined Containers (Monomorphization)
```
// Define parameterized struct template — NOT generics, just type stamping
container Stack(T) {
    T[64] data;
    u32 top;
}

Stack(u32) s;                // stamps concrete struct Stack_u32
Stack(Point) ps;             // stamps separate struct Stack_Point

// Functions are regular free functions, NOT methods (no `this`)
void stack_push(*Stack(u32) s, u32 val) { s.data[s.top] = val; s.top += 1; }

// T substitution works in: T, *T, ?T, []T, T[N] field types
container Wrapper(T) { *T ptr; ?T maybe; T[4] arr; }
```

### Designated Initializers
```
Point p = { .x = 10, .y = 20 };     // var-decl init
p = { .x = 100, .y = 200 };         // assignment (C99 compound literal)
Config c = { .baud = 9600 };         // partial — unmentioned fields auto-zero
```

### CRITICAL Syntax Differences from C

**These cause the most wasted turns in fresh sessions:**

1. **Braces ALWAYS required** for if/else/while/for bodies. No braceless one-liners.
   ```
   if (x > 5) { return 1; }      // OK
   if (x > 5) return 1;           // PARSE ERROR — "expected '{'"
   ```

2. **C-style casts ARE supported** for type conversion. `@saturate`, `@bitcast`, and `@pun` remain for the rare cases.
   ```
   (u32)small                     // OK — widening (or narrowing truncate)
   (f32)count                     // OK — int to float value convert
   (*opaque)sensor                // OK — *T to *opaque (sets provenance)
   (*Motor)ctx                    // OK — *opaque to *T (provenance checked)
   (*Task)t                       // OK — identity cast where t is already *Task (elided, zero overhead)
   (*u32)int_val                  // ERROR — use @inttoptr (mmio safety)
   (u32)ptr                       // ERROR — use @ptrtoint
   (*B)a_ptr                      // ERROR — types differ. Use @pun(*B, src) for explicit pun
   (*u32)volatile_ptr             // ERROR — cannot strip volatile
   (*u32)const_ptr                // ERROR — cannot strip const
   @pun(*Motor, sensor_ptr)       // OK — explicit type-punning with runtime type_id check
   @saturate(i8, big)             // OK — clamp to [-128, 127] (explicit only)
   @bitcast(u32, float_val)       // OK — reinterpret bits (explicit only)
   ```

   **Pointer-cast rule (LOCKED — added 2026-06-06, see BUGS-FIXED.md session
   2026-06-06):** Direct `(*T)src` where types match → elide, zero overhead.
   Direct `(*U)src` where types differ → compile error pointing to `@pun`.
   `@pun(*T, src)` → audit-visible escape, inlines *opaque round-trip with
   runtime type_id check that traps on mismatch. For byte parsing use `[*]u8`
   slices, not pointer casts.

3. **Variable declarations: type before name, no `struct` keyword in usage.**
   ```
   struct Task { u32 id; }        // declaration uses 'struct' keyword
   Task t;                        // usage: just the name, NO 'struct' prefix
   *Task ptr = &t;                // pointer to Task
   ```

4. **Array declaration: size between type and name.**
   ```
   u8[256] buf;                   // ZER: type[size] name
   // NOT: u8 buf[256];           // C style — won't work
   ```

5. **Enum values: dot syntax, not bare names.**
   ```
   enum State { idle, running, done }
   State s = State.idle;          // qualified access
   switch (s) {
       .idle => { ... }           // dot-prefixed in switch arms
       .running => { ... }
       .done => { ... }
   }
   ```

6. **Switch uses `=>` arrows, no fallthrough, no `case` keyword.**
   ```
   switch (x) {
       .a => { ... }              // enum arm
       .b, .c => { ... }          // multi-value arm
       default => { ... }         // default arm (required for int switches)
   }
   ```

7. **Optional unwrapping: `orelse` and `if |capture|`.**
   ```
   u32 val = maybe_func() orelse 0;           // default value
   u32 val = maybe_func() orelse return;       // propagate failure
   u32 val = maybe_func() orelse { cleanup(); return; }  // block fallback

   if (maybe_val) |v| { use(v); }             // if-unwrap with capture
   if (maybe_val) |*v| { v.field = 5; }       // mutable capture (pointer)
   ```

8. **No `++`/`--` operators.** Use `+= 1` / `-= 1`.

9. **No `malloc`/`free`.** Use Pool, Slab, Ring, or Arena builtins.

10. **String literals are `[*]u8` (slices), not `char*`.** `[*]T` is the preferred syntax — reads as "pointer to many." `[]T` still works but emits a deprecation warning ("use [*]T instead"). Use `[*]T` in all new code.
    ```
    const [*]u8 msg = "Hello";    // slice with .ptr and .len — a literal is READ-ONLY, so the slice must be const
    ```

11. **`else if` is supported.** Both forms work:
    ```
    if (a) { ... } else if (b) { ... }       // OK — parsed as nested if
    if (a) { ... } else { if (b) { ... } }   // OK — same result
    ```

12. **`orelse return` is bare — no value.** Return value comes from function's return type.
    ```
    *Task t = pool.alloc(Task) orelse return;    // OK — bare return
    *Task t = pool.alloc(Task) orelse return 1;  // PARSE ERROR
    ```

13. **`arena.alloc(T)` / `alloc(T)` / `@size(T)` take a type name, a primitive keyword,
    or a container stamp** (BUG-1556 — the last two used to be parse errors).
    ```
    arena.alloc(Task)           // OK
    arena.alloc(u32)            // OK — ?*u32
    arena.alloc_slice(u8, n)    // OK — ?[*]u8
    alloc(Box(u32))             // OK — a container stamp
    ```

14. **For loops use `i += 1`, compound init, and C-style structure.**
    ```
    for (u32 i = 0; i < 10; i += 1) { ... }   // OK
    for (u32 i = 0; i < 10; i++) { ... }       // PARSE ERROR — no ++
    ```

15. **`goto` and labels are supported.** Forward and backward jumps. Safe because auto-zero + defer neutralize goto's traditional dangers. Banned inside `defer` and `@critical` blocks.
    ```
    goto cleanup;                              // forward jump
    // ...
    cleanup:
        free_resources();
        return 1;
    ```

16. **Inline asm is bare `asm(...)`, only inside a `naked` function.** `asm("nop");` is the
    operand-free form; operands go in the structured form
    `asm { instructions: "..."  inputs: { "rax" = expr }  outputs: { ... }  clobbers: { ... }
    safety: "..." }` whose `safety:` string (>= 30 chars) is mandatory. The GCC extended
    form with colons is a compile error. See "`asm`" below.

17. **Variadic `...` — bodyless extern declarations ONLY (2026-06-16).** For
    C-interop so `printf` et al. work:
    ```
    i32 printf(const *u8 fmt, ...);   // OK — bodyless extern
    printf("x=%d\n", 7);               // OK — call site needs >= fixed params
    void f(...);                        // ERROR — needs >= 1 named param first
    void g(*u8 s, ...) { }              // ERROR — variadic with a body is banned
    ```
    A ZER function with a body cannot be variadic (it would read untyped,
    unverified args — the unchecked-boundary ban). `...` must be the final
    parameter; ≥1 named param required before it.

### Intrinsics (@ builtins)
```
@size(T)                 sizeof — returns usize
@truncate(T, val)        keep low bits (big→small). REJECTED on a FLOAT operand — a float has no
                         low bits; use `(T)x` (saturates) or `@saturate(T, x)`. Deliberate
                         divergence from two audit branches, which made it saturate: that gives
                         one primitive two unrelated meanings by operand type, and is redundant.
@saturate(T, val)        clamp to min/max of T
@bitcast(T, val)         reinterpret bits (same width required, qualifier-checked)
@cast(T, val)            distinct typedef conversion only (qualifier-checked)
@inttoptr(*T, addr)      integer to pointer (mmio-range-validated if ranges declared)
@ptrtoint(ptr)           pointer to integer (usize)
@ptrcast(*T, ptr)        pointer type cast (provenance-tracked, qualifier-checked)
@pun(*T, src)            explicit type-pun (runtime type_id check, qualifier-checked, audit-visible escape for raw casts)
@barrier()               full memory barrier (seq_cst)
@barrier_store()         store barrier (release)
@barrier_load()          load barrier (acquire)
@barrier_acq_rel()       acquire+release fence            [D-Alpha-2]
@offset(T, field)        offsetof
@container(*T, ptr, f)   container_of (field-validated, provenance-tracked)
@trap()                  crash intentionally
@probe(addr)             safe MMIO read — returns ?u32, null if address faults
@try_enum(E, val)        CHECKED int->enum — returns ?E, null if not a declared variant
                         (@bitcast is the unchecked-but-trapping route; a bare int is REJECTED)
@unreachable()           GCC unreachable hint (UB if reached)  [D-Alpha-2]
@expect(val, expected)   branch prediction hint                [D-Alpha-2]
```

### Atomic Intrinsics (15, all SEQ_CST ordering)
```
# Load / store / cas:
@atomic_load(*T) -> T
@atomic_store(*T, val)
@atomic_cas(*T, expected, desired) -> bool

# Fetch-old (returns value BEFORE op):
@atomic_add(*T, val) -> T          # existing
@atomic_sub(*T, val) -> T
@atomic_or(*T, val) -> T
@atomic_and(*T, val) -> T
@atomic_xor(*T, val) -> T
@atomic_nand(*T, val) -> T         [D-Alpha-1]
@atomic_xchg(*T, val) -> T         [D-Alpha-1] swap, returns old

# Fetch-new (returns value AFTER op):
@atomic_add_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_sub_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_or_fetch(*T, val) -> T     [D-Alpha-1]
@atomic_and_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_xor_fetch(*T, val) -> T    [D-Alpha-1]
```
The first argument is the address of an INTEGER global or struct field of width 1/2/4/8
bytes; it need not be in a `shared struct`. The safety net is the ATOMIC-CELL rule: once a
global is touched atomically in a concurrent context, every other access to it must be
atomic too. All orderings are SEQ_CST.

### Bit Query / Byte Swap Intrinsics (D-Alpha-2)
```
@bswap16(u16) -> u16     byte swap
@bswap32(u32) -> u32
@bswap64(u64) -> u64
@popcount(x) -> u32      count 1 bits  (dispatches on u32 vs u64 width)
@ctz(x) -> u32           count trailing zeros
@clz(x) -> u32           count leading zeros
@parity(x) -> u32        0=even / 1=odd parity
@ffs(x) -> u32           find first set bit, 1-indexed (0 if input is 0)
```
All use GCC builtins — auto-port to x86-64/ARM64/RISC-V without per-arch work.

### CPU / privileged intrinsics, carry primitives, uN / iN
130 intrinsics in all — the catalog is in `docs/reference.md`. `@cpu_*`, `@port_*` and
friends fault in user mode: test them in a DEAD BRANCH so they compile without executing
(`volatile u32 never = 0; if (never == 42) { @cpu_disable_int(); }`). `@addc` / `@subb` /
`@mulw` return `AddCarry64` / `SubBorrow64` / `MulWide64`. `uN` / `iN` (N = 1..128) wrap at
2^N (`emit_intn_mask`); single-bit access is `reg[5..5]`.

### `asm`
Bare `asm(...)`, only inside a `naked` function; the structured form's `safety:` string
(≥ 30 chars) is mandatory; max 16 instructions, no labels. Operand-boundary tracking (UAF,
move, VRP, provenance, escape, qualifier, MMIO) stays active through asm operands. Z1 / Z2
live in `zercheck_ir.c`, Z3..Z13 in `checker.c`. **Direction is LOCKED (Option E): read
`docs/asm_lang_zer_safe.md` + `docs/option_e_plan.md` before touching asm safety; do not grow
per-instruction tables.** Mnemonics may contain dots (`lr.w`).

### mmio Declaration (MANDATORY for @inttoptr)
```
mmio 0x40020000..0x40020FFF;   // declare valid MMIO address range
mmio 0x40011000..0x4001103F;   // multiple ranges allowed
// @inttoptr with constant address outside ranges → compile error
// @inttoptr with variable address → runtime range check + trap
// No mmio declarations + @inttoptr → compile error (strict by default)
// --no-strict-mmio flag: allow @inttoptr without mmio declarations
// For tests: mmio 0x0..0xFFFFFFFFFFFFFFFF; (allow all addresses)
```

### Provenance Tracking (@ptrcast + @container)
```
// @ptrcast tracks original type through *opaque round-trips:
*opaque ctx = @ptrcast(*opaque, &sensor);  // provenance = *Sensor
*Sensor s = @ptrcast(*Sensor, ctx);        // OK — matches provenance
*Motor m = @ptrcast(*Motor, ctx);          // COMPILE ERROR — wrong type

// @container validates field existence + tracks &struct.field origin:
*ListHead ptr = &dev.list;                  // provenance = (Device, list)
*Device d = @container(*Device, ptr, list); // OK — proven
*Other o = @container(*Other, ptr, list);   // COMPILE ERROR — wrong struct

// Unknown provenance (params, cinclude) → allowed (can't prove wrong)
// Propagates through aliases and clears+re-derives on assignment
```

### Function pointers and field access
```
u32 (*fn)(u32, u32) = add;        // 2A, C-style: `*` INSIDE the parens
*(u32, u32) -> u32 fn = add;      // 2C, ZER-native: `*` BEFORE the parens, name last
*(u32) -> u32 [4] ops;            // 2C needs no typedef for arrays or return types
?*(u32) -> u32 cb = null;         // nullable funcptr;   *(u32) -> ?u32 f;   returns an optional
typedef ?u32 (*H)(u32);           // 2A typedef: the `?` belongs to the RETURN type
?void (*on_event)(u32) = null;    // 2A anywhere else: the `?` makes the POINTER nullable
```
Both spellings are one `TYPE_FUNC_PTR`. A local funcptr needs an initializer. `.` is the only
field-access operator — value, pointer (auto-deref) and Handle (auto-lookup) alike; `->`
appears only as the return-type arrow of 2C.

### Defer
```
void f() {
    *u8 buf = alloc();
    defer free(buf);                     // runs at scope exit, in reverse order
    if (error) { return; }               // defer fires on ALL return paths
}
```

### Comptime (replaces `#define` and `#ifdef`)
```
comptime u32 BIT(u32 n) { return 1 << n; }        // arguments must be compile-time constants
u32 mask = BIT(3);
comptime if (HAS_FEATURE()) { ... } else { ... }  // only the taken branch is checked and emitted
```
Nested calls and `const` idents as arguments work; recursion depth 16; a global instruction
budget stops runaway loops.

### Thread Safety
```
// shared struct — auto-locked, no annotations needed:
shared struct Counter { u32 value; u32 total; }
Counter g;
g.value = 42;              // auto: lock → write → unlock
g.total = g.value + 1;     // SEPARATE lock scope — locking is PER-STATEMENT, NOT
                           // grouped (current_stmt_shared_root, ir_lower.c). Two
                           // statements = two lock/unlock pairs; the lock is
                           // released between them. This is what makes cross-
                           // statement lock ordering deadlock-free; it also means
                           // multi-statement check-then-act is NOT atomic (see the
                           // `locked g {}` design discussion in
                           // docs/primitives-data-races.md §24).

// spawn — fire-and-forget thread creation:
spawn worker(&g);           // OK — *shared struct, auto-locked
spawn process(&data);       // ERROR — non-shared pointer, data race
spawn handler(42, true);    // OK — value args copied, no sharing

// scoped spawn — ThreadHandle + join (allows *T args):
ThreadHandle th = spawn compute(&work);  // OK — *T allowed, will be joined
th.join();                  // MUST call before function returns
// zercheck: ThreadHandle not joined = compile error

// condvar — thread synchronization:
@cond_wait(shared_var, shared_var.count > 0);  // wait for condition
@cond_signal(shared_var);   // wake one waiting thread
@cond_broadcast(shared_var); // wake all waiting threads
// shared structs using condvar auto-upgrade to pthread_mutex_t

// threadlocal — per-thread storage:
threadlocal u32 counter;    // each thread has its own copy (__thread)

// Lock ordering — deadlock prevention (BUG-464 redesign):
shared struct A { u32 x; }
shared struct B { u32 y; }
A a; B b;
a.x = 1; b.y = 2;          // OK — separate statements, no nested locks
b.y = 2; a.x = 1;          // OK — separate statements, locks released between
a.x = b.y;                 // ERROR — same statement accesses both A and B
```


> **Concurrency safety status — read before claiming anything.** Do NOT claim ZER is fully
> data-race-free. The known cross-thread holes are closed and gated (`tests/test_conc_matrix.c`,
> `tests/test_hw_matrix.c`); the open ones — and exactly what is and is not PROVEN — are in
> `docs/limitations.md` (section B / B2). Floors: `cinclude` code; liveness. Design + Rust
> mapping: `docs/primitives-data-races.md` §24. Proofs: `proofs/operational/lambda_zer_concurrency/`.
> History of the audit: playbook "CONCURRENCY MEMORY-SAFETY STATUS".

### Move Struct — Ownership Transfer
```
// move struct: passing or assigning transfers ownership, original invalid
move struct FileHandle { i32 fd; }
FileHandle f;
f.fd = 42;
consume(f);          // ownership transferred
f.fd;                // COMPILE ERROR — use after move

// Assignment also transfers:
move struct Token { u32 kind; }
Token a;
a.kind = 1;
Token b = a;         // ownership transfers to b
a.kind;              // COMPILE ERROR — use after move
```

ZER is copy-by-default (opposite of Rust). `move struct` opts IN to ownership tracking.
Only for types representing unique resources (file descriptors, hardware handles, one-shot tokens).
Uses zercheck's existing HS_TRANSFERRED state — zero new infrastructure.

### Hardware Support
```
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);  // MMIO register
u32 bits = reg[9..8];                               // bit extraction
interrupt USART1 { handle_rx(); }                    // interrupt handler
packed struct Packet { u8 id; u16 val; u8 crc; }    // unaligned struct
```

### What ZER Does NOT Have
- No classes, inheritance, templates, generics (use `container` keyword for type-stamped containers)
- No exceptions, try/catch
- No garbage collector
- Heap allocation IS `alloc(T)` / `alloc(T,n)` / `free(x)` — a typed, zercheck-tracked malloc-equivalent (see docs/universal_alloc.md). What ZER does NOT have: a RAW untyped `void*` malloc. Pool/Slab/Ring/Arena/Handle remain as explicit-control allocators.
- No implicit narrowing or sign conversion
- No undefined behavior (overflow wraps, shift by >=width = 0)
- No `++`/`--`, no comma operator
- No C-style cast between two DIFFERENT pointer types (`@pun` is the audit-visible route); value casts `(T)x` ARE supported and explicit
- No preprocessor (#define → `comptime` functions, #ifdef → `comptime if`)
- No header files (use `import`)
- No preprocessor (#define, #ifdef)

### Feature Judgement — What NOT to Add

When considering new features, apply the **primitives test**: if the use case can be solved with existing language primitives, it's a library concern, NOT a language feature. ZER's concurrency is complete at the language level:

- **Park/unpark** — DON'T ADD. Condvar already covers thread wake/sleep. Two mechanisms for the same thing = complexity.
- **Future/Pin** — DON'T ADD. ZER async compiles to concrete state machines (Duff's device). No trait abstraction needed — each async function IS its own type. Pin exists in Rust only because borrow checker can't handle self-referential structs.
- **Arc (ref counting)** — DON'T ADD. Pool/Slab/Handle covers allocation. Ref counting adds runtime overhead and cycle leak risk.
- **Async runtime** — DON'T ADD as language feature. User writes the poll loop (bare-metal) or wraps with epoll (Linux). Library concern, not language.
- **Unbounded channel** — DON'T ADD. `Ring(T, N)` is bounded by design — prevents OOM on embedded. Unbounded is a library-level choice.
- **Generics** — DON'T ADD. `container` keyword covers user-defined containers via monomorphization (type stamping). No type constraints, no SFINAE, no template metaprogramming — just text substitution + type checking on the result.
- **Whole-program analysis** — BANNED from architecture. zercheck is per-file with summaries. Heuristics cover callbacks. See design decisions.
- **Partial moves** — DON'T ADD. Copy field value before consuming whole struct. Adds 200 lines of per-field tracking for a one-line workaround.

### Safety guarantees
The mechanism behind each class (bounds, UAF, null, races, …) is tabulated in the playbook,
"Safety Guarantees". Two things to hold on to here: never write "100% safe" without the
program-consequence qualifier, and never claim "data-race-free" — `docs/limitations.md`
says what is open and what is proven.

### Pitfalls when WRITING ZER (stdlib lessons)
- `orelse return` is bare (no value); for a `bool` function restructure with an if-unwrap.
- A string literal is read-only: bind it to `const [*]u8`. `.ptr` of a const slice is
  `const *u8` — cast to `const *opaque`.
- `cinclude` only emits the `#include`: declare every C function you call as a ZER signature.
  C MACROS (`stderr`) are not visible — wrap them in a C helper.
- The `_zer_` prefix is reserved for the compiler.
- Returning a sub-slice / `&elem` of a slice or pointer PARAM is allowed; of a LOCAL is not.
- `pool.get(h)` is non-storable (use it inline, or `h.field`); a `const Handle` is a const
  KEY — the data stays writable. After freeing a pointer held in a global, reset the global.

## Project map

Pipeline: `lexer.c` → `parser.c` → `ast.c` → `checker.c` (types, escape / keep, VRP,
concurrency, ISR) → `ir_lower.c` → `zercheck_ir.c` (handles, moves, threads on the CFG) →
`emitter.c` → GCC. Also `types.c`, `ir.c` (IR + `ir_validate`), `vrp_ir.c`, `src/safety/*.c`
(pure predicates: linked into zerc AND VST-verified from the same file), `zerc_main.c`
(driver), `zer_lsp.c`, `tools/zer-convert.c` / `zer-upgrade.c` (C → ZER).
- **`zercheck_ir.c` is the sole safety analyzer; `zercheck.c` is a shim — never add safety
  code there.** `IR_POOL_ALLOC` and friends are never emitted by design (method detection is
  `ir_classify_method_call_ex`).
- **Emit-C via GCC is permanent.** Never suggest LLVM, QBE or a native backend. The compiler
  is developed in C; self-hosting (`zerc.zer` compiles itself identically) is the v1.0
  proof, not the development path.
- Function bodies are IR-only. Before touching `ir_lower.c`, `ir.c` or an `IR_*` handler in
  `emitter.c`, read compiler-internals.md "IR Path Validation" (the invariants each came
  from a miscompile).
- Flags: `--run`, `--emit-c`, `--emit-ir`, `--lib`, `--no-strict-mmio`, `--target-bits N`,
  `--gcc PATH`, `--stack-limit N`, `--target-arch=`, `--target-features=`. Emitted C needs
  `-fwrapv` and GCC extensions. User type names emit as-is; internal names are `_zer_`.
- Navigate with `make tags` + grep; never read a source file end to end.

## Building & testing

`make zerc` · `make check` (everything) · `make docker-check` (the Windows host) ·
`make check-proofs` (Coq / Iris + the admit gate) · `make check-vst` (VST on `src/safety/*.c`).

| Directory | Expectation | Runner |
|---|---|---|
| `tests/zer/` | compiles, runs, exits 0 | `tests/test_zer.sh` |
| `tests/zer_fail/` | fails to compile FOR THE STATED REASON | same |
| `tests/zer_trap/` | compiles clean, traps at run time (`// expect-trap`, `// expect-trap-at: N`) | same |
| `tests/zer_gaps/` | known gaps, expectation INVERTED (compiling IS the gap; a closed gap fails loudly; a masked one needs `// gap-masked-by:`) | same |
| `test_modules/` | multi-file (`_negative` suffix for negatives) | `test_modules/run_tests.sh` |
| `rust_tests/`, `zig_tests/` | translations ONLY; contain negatives (`reject_*`, or `// EXPECTED: compile error`) | their own |
| `test_*.c`, `tests/test_*_matrix.c` | C unit tests; axis-crossed grids | `make check` |

Count tests from the tree (`ls tests/zer/*.zer | wc -l`), never from a document.

### Test rules
- **Every negative states WHY: `// expect-error: <substring>` in the first 5 lines** — the
  reason the rule is SUPPOSED to give, never a paste of today's output. Many independent
  rules reject the same file, so a non-zero exit proves nothing; this gets WORSE as the
  compiler gets better.
- A test that needs compiler flags says so on its first line: `// zerc-flags: …`.
- Every test directory is executed by a runner; wire a new one in the same commit. An
  unexecuted test decays to zero value, then to negative value once it is believed.
- Every spec rule has a positive AND a negative test; every safety feature has a
  `tests/zer_fail/` file through the full `zerc` pipeline (unit tests alone once hid that
  zercheck was never called from the compiler).
- Bug found → write the reproducer BEFORE the fix and show it FAILS on a pre-fix build.
  Keep a baseline compiler for the whole session
  (`git archive HEAD | tar -x -C /tmp/headb && make -C /tmp/headb zerc`) and A/B every
  "X was broken, my change fixes it" against it. Check WHICH binary each row ran, and grep
  for the diagnostic that is actually printed (`compiler bug:` is not `error:`).
- Use shapes the analysis cannot trivially prove: a FIXED ARRAY for VRP (a slice keeps its
  runtime check and masks the bug); values laundered through a loop for runtime wrappers.
- Hosted x86-64 limits: an `interrupt` block cannot be in a runnable positive (GCC refuses
  it) — verify compile-only; an MMIO positive must never touch the address (use an
  out-of-range index so the guard returns).
- New feature — test the interactions: both `orelse return` and `orelse { return; }`,
  `defer`, loops, `switch`, const / volatile, `*opaque`, if-unwrap capture, `@critical`,
  ISR, BOTH the assign and the var-decl spelling, all value-flow sites (init, assign, call
  arg, return). Then write one REAL 10-60 line program: real programs found bugs that
  1,700 unit tests missed.
- A new diagnostic that fires on a corpus file which is not your own new negative is either
  a real bug or a test that was passing for the wrong reason — check which, every time.
  Measure the corpus cost of any tightening by COMPILING the corpus with the rule and
  grepping for your diagnostic; a source grep sees only syntax, and a truncated list is not
  evidence.

## How to work here

**Preferences (the owner's — hold them even under pushback):** the proper fix, not the
pragmatic one — at the root (declaration, data structure, shared helper), never where the bug
shows; track, don't ban (ban only for a hardware / OS constraint, an emission impossibility,
or something needing a runtime or type system ZER lacks); an over-rejection is acceptable
only when the rewrite it forces is a teachable discipline; soundness is a hard wall,
precision a gradient.

**Before code.** Read the function, its callers and its data flow first. Grep
`BUGS-FIXED.md` and `docs/limitations.md` for the area. A documented open hole is a
HYPOTHESIS — reproduce it on the current compiler before anything else.

**Rules that keep paying for themselves** (the evidence for each is in the playbook)
1. Extract the helper FIRST — the same fix at 4+ sites means a missing abstraction.
2. A desugaring clones AST nodes; never share a mutable node.
3. **Rule #7 — no fixed-size buffer for dynamic data** (stack-first + arena / realloc
   overflow; `audit_fixed_buffers.sh`). A table that silently drops is a hole.
4. Scope-sensitive state is written at DECLARATION sites and only read at use sites.
5. Handle the edge or reject it at compile time — never silently wrong.
6. **A bounded walk rounds toward REJECT**, and what "reject" means depends on the caller's
   polarity (flip a boolean; make a silent walk report; go tri-state when two callers
   disagree). A walk over TYPES needs no cap.
7. **A relaxation (reject → accept) is the one change class where a bug ships a
   use-after-free or a race.** Gate it with a complete no-`default:` AST walk; land the
   tracking with no behaviour change first; write the rule's hypotheses down as a model —
   which edges LEAVE the region, which ENTER the code after it (compiler-internals.md
   "Sound relaxation").
8. **Verify that an edit LANDED and RUNS.** A hook can compile and never fire; an insert can
   land in the wrong function, or above the line that computes what it reads. One trace
   line proving the predicate is REACHED settles it.
9. **Measure, don't argue.** A/B every "this was broken, my change fixes it" against a
   baseline compiler; get an over-rejection's corpus cost by COMPILING the corpus; confirm a
   suspected race with ThreadSanitizer before designing the rule.

**Debugging.** Commit or stash first. Minimal repro → ONE targeted print → confirm the root
cause → fix. Three cycles without a confirmed cause, or a fix past ~10 lines: revert and
restate. A compiler segfault: rebuild with `-fsanitize=address -g -O0`.

**Checklists.** New builtin method → checker + emitter (both paths) + zercheck_ir. New AST
node → every exhaustive walker (`-Werror=switch` lists them). New coercion →
`value_flows_to`. New emission path → every safety wrapper ported. New safety rule → its
gate cell, a negative with `expect-error`, its row in `docs/multi-site-classes.md`.

**Docs go in the SAME commit, after `make check` is green.**
- a fix → `BUGS-FIXED.md` (symptom, root cause, fix, test);
- an open gap, deferred fix, over-rejection or residual → an entry in `docs/limitations.md`
  — a gap recorded only in a commit message or a code comment is lost. That file holds OPEN
  entries only (`audit_limitations.sh` fails the build otherwise): when a fix closes one,
  MOVE its text to the end of `docs/limitations-closed.md`, and write whatever the fix left
  open as its own entry tagged `[residual of: … — closed by BUG-n]`. Never strike an entry
  through and leave it;
- durable architecture → `docs/compiler-internals.md`; a language change → `docs/reference.md`;
- a class or gate → its row in `docs/multi-site-classes.md` + ONE index line here;
- a process lesson → `docs/session-playbook.md` + at most two lines here.

- `docs/compiler-internals.md` has a byte budget (`audit_docs_budget.sh`): to add a section,
  move one that became history to the end of `docs/compiler-internals-history.md`.

**Harvesting a `claude/*` audit branch**: protocol in compiler-internals.md, same title — the
fork point is the triage; a clean apply is not evidence of no conflict.

## Pushback discipline — capitulation is not helpfulness

When the user disagrees, re-derive the answer from the substrate (code, docs, a
measurement), independently of which way they lean.
1. **User right** → change position AND name the specific technical error in your earlier
   reasoning. "You're right, I overstated" is not a reason.
2. **User wrong** → hold the position and show the specific thing they are missing. Their
   confidence is not evidence.
3. **A genuine judgment call** → say so and lay out both sides, without picking the side
   they leaned to.
4. Never open with "you're right" before re-checking. A flip within one message of pushback
   is drift, not reasoning.

The test: could you defend the new position against equal pushback from the opposite
direction? If not, it was capitulation. The inverse holds too — do not re-litigate settled
questions (see "Confirmed NOT bugs"). Full text: playbook, same title.

## Proof work — read `docs/proof-internals.md` first

- **Three levels — keep them apart in every claim.** L1: a Coq / Iris oracle (the abstract
  rule is sound). L2: tests (the compiler rejects known violations). L3: VST on
  `src/safety/*.c` (the C that is linked into zerc matches the spec for every input).
- **THE ONE RULE: write specs against the L1 ORACLE, never against current C.** A
  code-driven spec is a tautology that freezes bugs into CI. No oracle → write it first.
- **The max-oracle standard.** Every distinguishable state AND every operational
  PRECONDITION the decision relies on is a variable of the model — a soundness hole found by
  red team rather than by a failing proof means the model lacked that variable (Level B's
  guard stability; BUG-1651's edges leaving the window). Merge = JOIN with the unsafe class
  as the default. Per oracle: T1 soundness, T3 a precision witness, T4 no-under-rejection.
  Zero admits, listed in `_CoqProject` AND in the `make check-proofs` admit gate. Name the
  honest LIMIT.
- 100% precision is impossible (Rice). Pay down only the PAYABLE over-rejection (relational
  domains, reduced product), each increment proven; never chase the forced tail.
- VST-friendly C: a flat cascade of early-return ifs, no nesting, no compound conditions.
  `src/safety/*.v` is clightgen output — never hand-edit it.
- **Say what is NOT proven.** A proof file that was written but never compiled is not a
  proof (`docs/limitations.md` B2 lists any such file).

## Safety architecture — 4 models (tables: `docs/safety-model.md`)

A new safety feature belongs to one of:
1. **State machine** — an entity with a lifecycle (handle states, moves, thread handles).
2. **Program-point property** — a fact about a value that changes at assignments and
   branches (ranges, provenance, escape flags, context flags).
3. **Function summary** — a per-function fact read at call sites (FuncProps, free / store
   summaries, return range, stack frames, spawn and shared-type scans).
4. **Static annotation** — declared once, checked at uses (qualifiers, MMIO ranges, keep).

Fits none → it is a combination of these; otherwise stop and document why.

## Diff-Based Post-Release Audit / AST→IR Emission Diff Audit

After a large structural change, green tests are not sufficient (protocols:
compiler-internals.md, same titles). The standing rule: an IR handler that replaces
`emit_expr(...)` with direct emission silently STRIPS the safety wrappers `emit_expr`
applied, and VRP hides the loss from tests. Before committing an emitter refactor:

    grep -nE "_zer_trap|_zer_bounds_check|_zer_shl|_zer_shr|_zer_probe|emit_intn_mask" emitter.c

Every AST-region match needs an IR-path equivalent. Audit BOTH operator forms (binary and
compound assignment are different cases). A region-presence gate does NOT discriminate;
behavioural coverage across emission forms does (`tests/zer/uN_width_wrap_all_forms.zer`).

## Intentional — not bugs

- MAYBE_FREED conservatism: a handle freed on one branch is unusable after the join unless
  the guards are provably disjoint. Add `return;` after the free, or restructure.
- No pointer arithmetic; `[*]T → *T` coercion only for C externs; a plain `*T` is not
  indexable; `@inttoptr(*T, 0)` is allowed (MMIO at address 0).
- `*opaque` from C has type id 0 (unknown) and passes the cast check: C is outside the boundary.
- Handles passed to a funcptr call are consume-maybe; a global left MAYBE_FREED at exit is
  deliberately not flagged.
- Pool (fixed, ISR-safe), Slab (growable, not ISR-safe) and Arena (bump, bulk reset) are
  three things on purpose. `Task.alloc()` uses one implicit auto-slab per struct type.
- Emitted struct initialisers are always DESIGNATED, never positional.

## Confirmed NOT bugs

Eight findings of the 2026-04-14 audit that were investigated and are correct by design
(distinct types block implicit conversion; `type_equals` is nominal; …) are listed in the
playbook under "Confirmed NOT Bugs". Check that list before re-opening one.

## Git Rules

- NEVER add Co-Authored-By or any Claude/AI attribution to commits
- All commits must be under zerohexer@gmail.com only
- No other authors, no co-authors, no AI mentions in commit messages
