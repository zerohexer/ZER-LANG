# ZER Compiler — Known Limitations

Living document of known compiler limitations, audit findings, and deferred fixes.
Entries removed once fixed.

---

## OPEN — WASM CLI: multi-file imports + macOS terminal zerc (LOW–MEDIUM)

The VS Code extension ships the compiler as WebAssembly (`zer_wasm.c` →
`lsp/zer.wasm`, driven by `lsp/server.js` and `lsp/zerc-cli.js`). As of
2026-06-16 the flag plumbing, `--emit-ir`, and full LSP safety parity are DONE
(`zer_set_target` carries `--target-bits/-arch/-features`, `--no-strict-mmio`,
`--stack-limit`; `zer_emit_ir`; `zer_diagnostics_json` runs `zercheck_ir` and
`server.js` merges its stderr into editor diagnostics). Two gaps remain:

- **Single-file only:** `zer_emit_c`/`zer_diagnostics_json` set
  `import_asts = NULL` and process one `file_node`. Multi-module programs
  (`import`) won't resolve cross-module symbols through the wasm CLI/LSP. Native
  zerc handles modules (topological emit across files). Fix sketch: thread a
  node-side import resolver (read imported `.zer` by path, parse, pass the AST
  array in) — emscripten `NODERAWFS` would let the checker's `fopen`-based
  import loader work directly, or pass `import_asts`/`import_ast_count` built in
  JS. Largest remaining piece; needs replicating zerc_main's module loop.
- **macOS terminal `zerc` dropped:** the wasm VSIX bundles a signed Windows
  `node.exe` for `zerc.cmd` and keeps a native `linux-x64` `zerc`, but no darwin
  CLI build remains. macOS LSP still works (wasm via Electron-node); only the
  macOS *terminal* `zerc` is absent. Fix sketch: bundle a signed darwin node +
  a `zerc` shell shim.
- **Cosmetic:** the CLI prints some checker errors twice (checker records
  certain diagnostics twice); pre-existing, not wasm-specific.

Cross-arch caveat: `--target-arch aarch64|riscv64` configures the *checker*
correctly, but the wasm CLI compiles with the bundled **x86_64** gcc — actual
cross-compilation needs a cross-gcc the bundle doesn't ship.

---

## CLOSED — 6u360k audit (2026-06-09): all 8 gaps fixed (BUG-734..741)

All 8 silent gaps from branch `claude/cool-johnson-6u360k` are closed and
suite-guarded: GAP-5 orelse overwrite leak (BUG-734), GAP-1 @ptrcast concrete
type confusion (BUG-735), GAP-2 --no-strict-mmio runtime alignment trap
(BUG-736), GAP-8 by-value struct param laundering (BUG-737), GAP-7 container
composite type args (BUG-738), GAP-3 alloc_ptr global-alias UAF (BUG-739,
per-function scope — see follow-up below), GAP-4 funcptr double-free
(BUG-740, argument-precise barrier), GAP-6 variable-index array double-free
(BUG-741). Per-gap detail in BUGS-FIXED.md Session 2026-06-09/10 entries.
The `tests/audit_2026-06-09/` reproducer directory is retired — every
reproducer was promoted into `tests/zer_fail/` or `tests/zer_trap/`.

## OPEN — conditional global dangle (MAYBE_FREED at exit) unflagged (LOW, BUG-742 residual)

BUG-742 (2026-06-10) closed cross-function global UAF at the source: a
global definitely FREED at function exit or at a ZER/indirect call site is
a compile error (teaches `g = null;` after the free). DELIBERATELY out of
scope: globals in MAYBE_FREED state at those points — `if (c) {
heap.free_ptr(p); }` then exit leaves the global conditionally dangling,
unflagged. Why: BUG-740/741 widenings produce MAYBE_FREED + escaped on
legitimate hand-off patterns (`g_ptr = p; fp(p);` register-ctx-then-
callback), and flagging MAYBE at exit would reject exactly those. Fix
sketch if ever needed: distinguish widened-by-barrier (escaped at widening
time) from conditionally-freed-by-user (definite IRMC_FREE on one branch)
with a per-entry origin bit, and flag only the latter. Reproducer shape:
conditional free of a global-held pointer without reset on that branch.



---

## PLAN — asm Option E rework (Level C cleanup first)

The asm-safety architecture is slated to move to Option E (three-layer, no-favored-
ISA — `docs/asm_lang_zer_safe.md` §1.7). **Phase 1 = Level C cleanup**: delete the
per-arch infrastructure (~7,000 lines: register/instruction tables, categories
framework, probe scripts, `arch_data/*.zerdata`, stray `.v`), replace the
checker.c F7-full table dispatch with a hardcoded ~12-entry UB-classics list, and
delegate register/instruction/feature validation to GCC. **Intrinsics STAY in
Phase 1** — they get re-layered (operation→Layer 1, x86 asm→Layer 2 lib) only in
Phase 3. Verified file-by-file execution checklist (commit order, regression net,
the `.v`/`check-vst` coupling asm_lang §10 underspecifies):
**`docs/option_e_plan.md`** — fresh-session-executable. `tests/test_asm_matrix.c`
is the regression net for the deletion.

## OPEN — asm S2 instruction-count `\n`-escape bypass (audit rule, not safety)

The S2 rule (checker.c:10379) caps an asm block at 16 instructions for
auditability, counting actual-newline (0x0A) and `;` chars in the instruction
string. ZER's lexer keeps `\n` ESCAPE sequences literal (does not convert to
0x0A), but the emitted C asm string IS expanded by GCC — so
`instructions: "nop\nnop...×17"` passes S2 (counted as 1) yet assembles as 17
real instructions. The S2 "≤16 auditable" guarantee is therefore bypassable via
`\n` escapes.

**Severity: low — S2 is an AUDIT/maintainability rule, not a memory-safety /
program-consequence rule** (its own message: "forces small auditable blocks").
No safety/soundness false negative; the Z-rules, naked-only, qualifier/escape
checks are unaffected. Found by `tests/test_asm_matrix.c` (2026-06-08).

Fix sketch (when convenient): make the S2 counter also count `\n` (and `\t`)
escape pairs in the instruction string, OR normalize asm-string escapes at lex
time so the count matches what GCC assembles. The oracle's `too-many-instructions`
cell uses `;` separators to test S2 as designed in the meantime.

---

## STATUS — soundness oracle suite (read first, 2026-06-07)

Four exhaustive `-Wswitch`-enforced oracles guard the compiler's safety analysis.
A "hole" = a NEG cell that compiled clean (false negative = unsafe program
accepted, the unacceptable class) or a POS cell that was rejected (over-rejection,
acceptable but logged). Each is built + run by `make check`.

| Oracle | File | Cells | Domain | Status |
|---|---|---|---|---|
| Shape | `tests/test_shape_matrix.c` | 25 | temporal (UAF/double-free/leak/move) × type × reach-shape | green |
| Escape | `tests/test_escape_matrix.c` | 35 | local-pointer escape × launder × sink | green |
| Keep | `tests/test_keep_matrix.c` | 21 | non-keep-param persistence × launder × sink (+ keep valve) | green |
| Control-flow | `tests/test_cflow_matrix.c` | 38 | if/loop/switch/break/continue/defer merges × {pool,slab} | green |
| Concurrency | `tests/test_conc_matrix.c` | 15 | data-race / spawn / deadlock / ThreadHandle join | green |
| ISR/atomics/MMIO | `tests/test_hw_matrix.c` | 12 | MMIO range/align/decl, volatile-strip, ISR context + data-race (program-consequence only) | green |
| Async | `tests/test_async_matrix.c` | 10 | yield/await in defer/@critical, spawn-in-async, valid yield/await/defer/state-promotion | green |
| Asm | `tests/test_asm_matrix.c` | 11 | DURABLE asm surface: S1 naked-only, S2/S3/S4, empty-insn, Z8 const-output, Z11 non-keep-ptr+mem-clobber (NOT F4-F7 register tables) | green |

**Pointer-lifetime axis ("universal pointer") is DONE** (2026-06-07): the
compile-time `keep` model (PART 5 of `docs/universal_pointer.md`) — all 5 steps
complete, boundary defaults audited. This session closed 24 real holes (16 escape
+ 6 keep + 1 defer-double + 1 struct-copy). Do NOT re-investigate the pointer
axis for false negatives without a new launder/shape idea — the four oracles are
the standing guard; add a cell if you have a new idea.

**Next frontier = the non-memory domains** (concurrency / ISR / atomics / async /
MMIO). See the dedicated OPEN entry below.

---

## OPEN — next frontier: concurrency / ISR / atomics / async / MMIO oracles

Domains 1 (concurrency) and 2 (ISR/atomics/MMIO) now HAVE oracles (both
green, 0 holes — see the STATUS table). **Domain 3 (async) is the remaining
gap.** The non-memory domains needed their own harness shapes (concurrency is
timing/structural; ISR/privileged ops use the EMIT-ONLY + dead-branch pattern).
Recommended approach for the remainder: **survey first** — write a handful of
adversarial NEG programs, see which leak (compile clean when they should reject),
then build/extend the oracle.

Many rules here ARE accept/reject (oracle-able like the memory matrices); a few
(shared-struct auto-lock *correctness*) are emission-correctness, not accept/
reject, and need an emit-inspection check instead.

**Domain 1 — data-race / spawn / deadlock — DONE (2026-06-07).**
`tests/test_conc_matrix.c`, 15/15, **0 holes found** (regression lock-in — the
spawn/deadlock/join checks are structural bans, built with extensive
Rust-equivalent tests, and held up including the edge cases that found holes in
other domains: transitive non-shared global at call depth, Slab-access from
spawn, and ThreadHandle joined in only one branch). Covered NEG: spawn
non-shared ptr, spawn non-shared global (direct + transitive), deadlock
same-statement, spawn-in-@critical, ThreadHandle not joined (direct +
one-branch). POS: shared auto-lock, scoped spawn+join (incl. join-both-branches),
value args, separate-statement shared access, threadlocal. Residual (add when
convenient): `shared(rw)` concurrent-reader same-statement, Ring/Pool-from-spawn
(only Slab tested), condvar/@barrier/@once interactions, deeper deadlock cycles
(A→B→C ordering).

**Domain 2 — ISR / atomics / MMIO — DONE (2026-06-07).**
`tests/test_hw_matrix.c`, 12/12, **0 holes** (regression lock-in — MMIO/volatile/
ISR checks are mature). PROGRAM-CONSEQUENCE only, per
docs/firmware_safety_extensions.md: tests wrong USES with a structural shadow,
NOT the hardware floor. NEG: @inttoptr no-decl / out-of-range / misaligned,
volatile-strip, slab-in-ISR, spawn-in-ISR, ISR non-volatile shared global, ISR
volatile compound-RMW. POS: @inttoptr in-range+aligned (incl. writing 9601 — the
floor value COMPILES, demonstrating the split), pool-in-ISR, atomic global, ISR
volatile plain assign. EMIT-ONLY harness (interrupt attrs may not compile on
hosted gcc). DELIBERATELY EXCLUDED (floor / Definition B / pending gaps — a NEG
cell for these would be a WRONG expectation): 9601-vs-9600 baud value,
read-clears / W1C / sticky side effects (§16), region-kind hardware correctness,
`@section`/region-kinds/`@reset_handler`/linker-symbol features (not built).
Residual (add when convenient): @atomic non-1/2/4/8 width reject, MMIO
variable-index runtime-trap (belongs in tests/zer_trap, not emit-only).

**Domain 3 — async (yield/await) — DONE (2026-06-07).**
`tests/test_async_matrix.c`, 10/10, **0 holes** (regression lock-in — async bans
are structural). NEG: yield/await in defer, yield/await in @critical,
spawn-in-async. POS: yield, await, defer-without-suspend, local across yield
(state promotion), await-on-shared. **Key finding (corrected a wrong
expectation):** `await g.ready == 1` (await condition reads a shared struct) is
SAFE and correctly COMPILES — each poll locks/reads/unlocks and the lock is
released BETWEEN polls, never held across the suspension. `yield`/`await` are
STATEMENT-ONLY (can't embed in an expression — `g.v + yield` is "undefined
identifier 'yield'"), so a shared lock (held only for its own statement) can
never bracket a separate suspend statement. The "shared access in a statement
containing yield" rule (checker.c:5450) is therefore defensive/forward-compat and
effectively unreachable today — no false negative, the unsafe construction isn't
expressible. (The async analog of the 9601-floor lesson: don't assume a construct
is unsafe; verify. await-on-shared is the floor-equivalent that must COMPILE.)
Residual: if yield/await ever become expressions, revisit the 5450 reachability.

**ALL THREE FRONTIER DOMAINS DONE (2026-06-07).** The seven-oracle suite (shape,
escape, keep, cflow, conc, hw, async) covers the memory-safety axes AND the three
non-memory domains. Net for the frontier: 0 holes found (all regression lock-in)
— the concurrency/ISR/async checks are mature structural bans, in contrast to the
pointer-axis data-flow analyses (24 holes this session). What remains is lower-
value coverage debt (the per-domain "Residual" notes above + the shape-matrix
roadmap items below), not known false negatives.

**Breadth survey (2026-06-07):** direct-case guards CONFIRMED firing —
`spawn worker(&local)` (non-shared ptr → "data race"), `@inttoptr(const)` with no
`mmio` range (→ "requires mmio range"), `yield` in `defer` (→ "corrupts coroutine
state") all reject correctly. So the frontier is NOT wide-open; the oracle work is
finding LAUNDERED / merge / cross-context edge cases (as the memory oracles found
24), not plugging direct holes. Start each domain oracle from its accept/reject
rules above and add launder/merge variants.

**Harness notes:** privileged/hardware ops (ISR bodies, `@cpu_*`, MMIO) can't
`--run` in a hosted container — use EMIT-ONLY (`zerc f.zer -o /tmp/x.c`, exit 0 =
zercheck accepted) for POS, and the dead-branch pattern (`volatile u32 nt = 0;
if (nt == 42) { ...privileged... }`) to compile without executing. Integrity
guard: a NEG rejection must name the relevant safety reason (data race / deadlock
/ ISR / not joined / atomic width), not a parse error. Model the oracle on
`tests/test_cflow_matrix.c` (closest structure: NEG+POS, find_zerc, -Wswitch).

When a domain oracle lands: add it to the STATUS table above, wire into
`make check` (Makefile `check:` deps + run line), document in
`docs/compiler-internals.md`, and trim this entry's covered domain.

---

## OPEN — shape-matrix oracle: remaining coverage roadmap

`tests/test_shape_matrix.c` is the exhaustive memory-safety oracle (option A,
the replacement for the retired dual-run). It currently covers 25 cells:
3 types {pool/Handle, slab/`*T`, move-struct} × 7 reach-shapes {bare, field,
array, fnarg, field-xfn (GAP-A), spawn (GAP-C), deref (BUG-463)} × 4 violations
{uaf, double-free, leak, use-after-move}. Found+fixed BUG-702 (compound leak)
and BUG-703 (move-field over-rejection). These remaining axes are NOT coverage
yet. Ranked by likelihood of finding NEW under-rejections (vs regression
lock-in). NONE are bugs — they're coverage debt: surface where a silent
under-rejection could still hide.

**1. Control-flow / path-sensitivity axis — DONE (core), 2026-06-07.**
`tests/test_cflow_matrix.c` now covers `{pool, slab}` × 19 control-flow
scenarios (if-then/if-both/loop/next-iter/switch-one/switch-all/double-if/
leak-if/leak-loop/nested-if/break/continue/defer-double + the safe POS
counterparts), 38/38. Found+fixed BUG-727 (defer + explicit double-free was a
false negative). Residual control-flow shapes NOT yet enumerated (lower
likelihood, add when convenient): `orelse`-unwrap/fallback paths crossed with
free state; `goto` (forward past a use; backward re-use); deeply-nested (3+)
control flow; move-struct and `*opaque` types under control flow (the cflow
matrix uses pool Handle + slab `*T` only). None are known bugs — coverage debt.

**2. `*opaque`/extern type row — LOW value (lock-in).** A 4th type
(`TY_OPAQUE`: bodyless `?*opaque make()` extern-alloc + `void destroy(*opaque)`
extern-free). Codegen catch: positives can't `--run` (bodyless externs won't
link). Solution worked out: add a per-type `compile_only` flag and assert the
positive with EMIT-ONLY (`zerc f.zer -o /tmp/x.c`, no GCC/link) = exit 0 means
zercheck accepted; negative = `-o /dev/null` still fails at zercheck before
link. Locks in GAP-B (extern-alloc+orelse) and GAP-D (destructor heuristic).
Just audited 2026-06-06, so mostly regression lock-in.

**3. Smaller lock-in cells — LOW value.** wrong-pool (`pool_a` handle freed via
`pool_b` — BUG-471 class); move-array (`consume(arr[0])` move element —
BUG-476 class); alias chains (`h2=h1; free(h1); use(h2)`); return-escape
(`return h` — escape-vs-leak distinction); slab field/array storage (currently
pruned for the `*T`-in-struct non-null concern — verify whether it works).

**4. Different domain → SEPARATE harness.** Concurrency / ISR / atomics / async /
MMIO — promoted to its own actionable entry: see "OPEN — next frontier:
concurrency / ISR / atomics / async / MMIO oracles" above (per-domain
accept/reject rules + survey-first plan + harness notes).

When any axis is added: add the cells, update the "Extending the grid" coverage
list in `docs/compiler-internals.md`, and trim this entry.

---

## ~~Gap 38 — function-return Handle bypassed zercheck_ir~~ (FIXED 2026-05-05)

`zercheck_ir.c` IR_CALL summary path didn't include `TYPE_HANDLE` /
`?TYPE_HANDLE` in its `is_ptr_return` check, so any wrapper function
returning a handle (`?Handle(T) get_handle() { return heap.alloc(); }`)
left the caller's local untracked. Subsequent double-free was silent.

Fixed by extending the check, gated on `summary->returns_color` being
a known allocator (`POOL`/`MALLOC`) so accessor/transfer wrappers like
`pop_free()` from a global queue don't misfire as leaks. The
defer-body scanner was simultaneously extended to consult FuncSummary
`frees_param[i]` so user free-wrappers (`defer device_destroy(h)` →
`pool.free(h)`) propagate FREED through the alias-id chain.

Tests: `tests/zer_fail/gap38_func_return_handle_dfree.zer`,
`tests/zer/gap38_func_return_handle_ok.zer`. See BUGS-FIXED.md
"BUG-661" for the full session entry.

## ~~Runtime preamble unguarded pthread types broke freestanding~~ (FIXED 2026-05-05)

`emitter.c:4564-4583` defined `_zer_mtx_ensure_init_cv` and
`_zer_mtx_ensure_init` referencing `pthread_mutex_t *`,
`pthread_cond_t *`, and `PTHREAD_MUTEX_RECURSIVE` outside any
`__STDC_HOSTED__` guard. `gcc -ffreestanding -D__STDC_HOSTED__=0`
failed with `'pthread_mutex_t' undeclared`. Wrapped the helper
definitions in the same hosted guard already used for the include and
the thread barrier block. See BUGS-FIXED.md "BUG-662".

## ~~`_zer_trap` libc-abort fallback broke freestanding~~ (FIXED 2026-05-05)

`_zer_trap` always emitted `fprintf(stderr,...)` and used `abort()` in
the unknown-arch fallback. Both need libc, so freestanding x86 (kernel
mode, EFI apps) or freestanding non-{ARM,RISC-V,AVR,x86} (MIPS,
PowerPC, SPARC, custom) failed to link. Restructured: the diagnostic
print is gated on `__STDC_HOSTED__`, per-arch trap instructions emit
unconditionally, and the `#else` fallback uses `__builtin_trap()` on
freestanding. See BUGS-FIXED.md "BUG-663".

## ~~`@once` lacks `__STDC_HOSTED__` guard~~ (FIXED 2026-05-02, commit `664b211`)

Wrapped atomic emission in `#if __STDC_HOSTED__` with a non-atomic
fallback for freestanding builds. See BUGS-FIXED.md "Fix #2".

## ~~`@probe` silently succeeds on freestanding~~ (FIXED 2026-05-02, commit `edce2a3`)

Added `--probe-mode={hosted,raw,disabled}` CLI flag with three modes:
hosted (signal-handler default), raw (direct read, no fault recovery),
disabled (compile-error if `@probe` used). See BUGS-FIXED.md "Fix #4".

## ~~`@critical` indirect return via callee~~ (INVESTIGATED 2026-05-02 — not a bug)

**Status:** Audit claim re-verified against actual emission and found
to be incorrect. No fix needed.

**Original claim (from claude/cool-johnson-apebs branch audit):**
calling a function from `@critical` lets the function's `return`
"escape" the `@critical` block without re-enabling interrupts:

```zer
void unlock() { return; }
@critical { unlock(); }   // claimed: interrupts NOT re-enabled
```

**Why the claim is wrong:** A normal function call returns to its
caller. The caller is `@critical { ... }`. Execution returns to the
@critical body, continues to the closing brace, and the closing brace
emits the interrupt-restore code. Verified empirically by inspecting
emitted C: `cpsid i` at @critical entry, `msr primask, ...` at
@critical exit, function calls in between emit standard call/ret —
control flow returns to the @critical body, not to the function that
contains @critical.

A `can_escape` predicate (transitive return/break/continue/goto)
would have to reject EVERY function call from @critical (every
non-trivial function returns), making `@critical` essentially
unusable.

**What IS actually checked today** (sufficient):
1. Direct `return`/`break`/`continue`/`goto` inside `@critical` body —
   per-site rejected (NODE-level checks)
2. `yield`/`await` directly or transitively — `can_yield` propagation
3. `spawn` directly or transitively — `can_spawn` propagation
4. Heap alloc directly or transitively — `can_alloc` propagation
5. Inline asm directly or transitively — `can_*` via FuncSummary

The branch's audit was correct in spirit (transitive checks are good)
but misanalyzed control-flow semantics for plain function returns.

## ~~AST `emit_expr` compound `/=` and `%=` lack signed-overflow trap~~ (FIXED 2026-05-02, commit `b4d10ed`)

Ported the same `INT_MIN/-1` overflow trap pattern from the IR path
to the AST `emit_expr` sibling at emitter.c:1433–1444. Defense in
depth even though function bodies are IR-only since 2026-04-19. See
BUGS-FIXED.md "Fix #3".

## ~~u64 atomic warning fires on 64-bit targets~~ (FIXED 2026-05-02, commit `c4bbbe0`)

Gated the "may require libatomic on 32-bit" warning on
`target_ptr_bits < 64`. The fix uncovered a deeper bug — BUG-652:
`Checker.target_ptr_bits` was never initialized from the global, so
the field was memset-zeroed and any `target_ptr_bits < N` check
silently always-true. See BUGS-FIXED.md "Fix #1".

## OPEN — `naked` attribute silently dropped on IR path

See full entry near the bottom of this file ("`naked` attribute
silently dropped on IR path (deferred 2026-05-02)") — kept in original
location to preserve the more detailed analysis added in the
2026-05-02 fix session.

## ~~Codebase audit 2026-05-07 — 5 silent gaps closed~~ (FIXED 2026-05-07)

Full bug-hunt session. See BUGS-FIXED.md "Session 2026-05-07" for
per-bug detail.

- BUG-661 — `return s.len` (scalar field of local-derived slice)
  rejected as a pointer escape. Fixed: gate the field-walk escape
  check on `type_can_carry_pointer(ret_type)`.
- BUG-662 — `@critical { wrapper(); }` where wrapper transitively
  calls slab.alloc(): silently allowed. Fixed: enable ban_alloc=true
  on @critical's check_body_effects.
- BUG-663 — `slab.free()` / `slab.free_ptr()` / `Task.free()` /
  `Task.free_ptr()` inside @critical or interrupt handlers:
  silently allowed. Fixed: per-site check_isr_ban at the four free
  handlers.
- BUG-664 — Pool/Ring/Arena `.alloc()` flagged as heap by
  scan_func_props (false positive on transitive path through a
  pool wrapper). Fixed: gate can_alloc on receiver TYPE_SLAB or
  TYPE_STRUCT (Task auto-slab); Pool/Ring/Arena types excluded.
  Same change extends detection to free/free_ptr methods, closing
  the transitive variant of BUG-663.
- BUG-665 — `return g.v;` where `g` is `shared struct` leaked the
  auto-mutex past return. Multi-threaded programs deadlock on the
  next acquire. Fixed: ir_lower NODE_RETURN / NODE_BREAK /
  NODE_CONTINUE / NODE_GOTO emit IR_UNLOCK before the exit when an
  enclosing block holds a shared lock.

Cumulative fix size ~70 LOC across checker.c + ir_lower.c. Seven
regression tests added (3 positive + 4 negative). Full test suite
green (1,400+ tests across tests/zer, test_modules, rust_tests,
zig_tests).

Single-level tracking on the BUG-665 fix — nested cases where an
ancestor block holds a different shared root still leak the
outer lock. Tracked as a follow-up; the simple `return
shared.field;` form is now safe.

## ~~Gap 38 — function-return Handle bypasses zercheck_ir tracking~~ (FIXED 2026-05-16)

**Status:** zercheck_ir now treats `Handle(T)` / `?Handle(T)` returns
the same way it treats pointer returns — registers the dest local as
fresh ALIVE with `escaped=true`. The escape flag suppresses the
leak-at-exit check (caller commonly passes the handle to a destructor
via `defer device_destroy(h)`, where the destructor lives in another
module and its FuncSummary may not propagate) while still preserving
FREED-state transitions for double-free / UAF detection.

Two patterns now caught:

```zer
?Handle(Task) get_handle() { return heap.alloc(); }

?Handle(Task) mh = get_handle();
Handle(Task) a = mh orelse return;
heap.free(a);
heap.free(a);   // detected: double-free
```

```zer
Handle(Task) h = get_handle() orelse return;
heap.free(h);
heap.free(h);   // detected: double-free (orelse-IR_ASSIGN variant)
```

Two reproducers landed in `tests/zer_fail/`:
- `gap38_fn_return_handle_double_free.zer` (IR_CALL site fix)
- `gap38_handle_orelse_return_double_free.zer` (IR_ASSIGN orelse-decomp site fix)

Implementation: `zercheck_ir.c` — added TYPE_HANDLE to the existing
pointer-return detection in two branches (FuncSummary present and
not-present) of the IR_CALL handler, plus a new IRMC_NONE Handle
branch in the IR_ASSIGN handler for orelse-wrapped calls. Both sites
set `h->escaped = true` after registering.

## ~~Gap 27 — `@cstr` to raw `*u8` destination — no bounds check~~ (FIXED 2026-05-16)

**Status:** checker now rejects `*u8` (non-volatile, non-const) as
`@cstr` destination. Pattern that silently overflowed before:

```zer
u8[4] buf;
*u8 p = buf[0..].ptr;
@cstr(p, "Hello, world this is too long");  // 29-byte write to 4-byte buf
```

Fix: error with hint to use slice (`[*]u8`) or fixed array (`u8[N]`)
destination. Volatile pointers (MMIO registers) are still permitted —
size is hardware-fixed there. Reproducer: `tests/zer_fail/gap27_cstr_raw_ptr_dest.zer`.

Implementation: `checker.c` `@cstr` intrinsic handling — added
TYPE_POINTER rejection that excludes `is_volatile` (so MMIO writes
still compile) and `is_const` (already rejected earlier with a
different message).

## ~~Gap 10 — `@critical` on bare-metal x86 only emits memory fence~~ (FIXED 2026-05-16)

**Status:** `IR_CRITICAL_BEGIN` / `IR_CRITICAL_END` now emit an
`#elif (defined(__x86_64__) || defined(__i386__)) && (!defined(__STDC_HOSTED__) || __STDC_HOSTED__ == 0)`
branch that saves EFLAGS and runs `cli` (begin) / `popf` (end). On
hosted x86 user-mode the fence-only fallback is preserved — `cli`
SIGSEGV's at CPL > 0, and user code can't legitimately disable
interrupts there anyway.

Verified by disassembly of a freestanding-compiled @critical test:

```
0000000000000000 <main>:
   0: endbr64
   4: pushf
   5: pop    %rax
   6: cli                    ; <-- actual interrupt disable
   7: addl   $0x1,0x0(%rip)  ; critical body
   e: push   %rax
   f: popf                   ; <-- EFLAGS restore
  10: mov    0x0(%rip),%eax
  16: ret
```

The arch cascade order in `emitter.c` IR_CRITICAL_BEGIN/END is now:
`__ARM_ARCH` → `__AVR__` → `__riscv` → bare-metal x86 → fence
fallback. Bare-metal x86 takes precedence over the generic fence.

---

## ~~Pool/Slab.free of auto-zero Handle corrupts slot 0~~ (FIXED 2026-05-21, audit session)

**Symptom (pre-fix):** declaring `Handle(Item) h;` without initializer
auto-zeroes to `h_gen == 0`, `idx == 0`. Calling `pool.free(h)` on this
"null" Handle bumped `pool.gen[0]` and cleared `pool.used[0]` —
silently invalidating any legitimate handle that the pool had issued
for slot 0 (which has `h_gen >= 1`). Subsequent `pool.get(legit_h)`
would trap on gen mismatch, but the trap message blamed the legitimate
handle instead of the actual cause (the null-handle free).

**Root cause:** `_zer_pool_free` and `_zer_slab_free` lacked the
`h_gen == 0` short-circuit. Compile-time `zercheck_ir` did not flag
the use of an uninitialized Handle either, because UNKNOWN-state
handles fall through the IR_POOL_FREE handler (which only checks
ALIVE/FREED/MAYBE_FREED/TRANSFERRED transitions). Both compile-time
miss + runtime corruption = SILENT GAP class — caught only when a
legitimate handle later trapped on gen mismatch.

**Fix:** added `if (h_gen == 0) return;` no-op at top of
`_zer_pool_free` and `_zer_slab_free`. Auto-zero Handle free is now
silently ignored (the typical "null handle" pattern), preserving pool
state integrity. Regression tests:
- `tests/zer/null_handle_pool_free_noop.zer`
- `tests/zer/null_handle_slab_free_noop.zer`

**Stronger validation deferred.** Ideally `_zer_pool_free` would also
trap on `gen[idx] != h_gen` (matching `_zer_pool_get`'s discipline),
catching wrong-pool / stale-handle frees that compile-time
`zercheck_ir` misses (e.g., cross-function calls without a
pool-aware FuncSummary). Attempted in this session but reverted: it
trips the **defer-fires-twice-on-goto-to-same-scope** emitter bug
described below.

## OPEN — Defer fires twice when goto target is in same defer scope

**Symptom:** `defer free(h); ... goto cleanup; ... cleanup: return 0;`
fires the defer body once at the goto site AND again at the return
site. The defer body executes twice for a single dynamic execution.

**Why it's hidden today:** runtime `_zer_pool_free` is intentionally
lenient (just bumps gen + clears used; no validation on stale h_gen).
Double-fire of `pool.free(h)` silently bumps gen twice. The
`rust_tests/rt_goto_fires_defer.zer` test relies on this — its
check `freed_count != 1` runs at `cleanup:` BEFORE the second fire,
so the test passes.

**Why it matters:** any future runtime hardening of `_zer_pool_free`
(generation validation, wrong-pool detection) traps on the second
fire. The compile-time guarantee `zercheck_ir` reports for handle
states becomes inconsistent with runtime behavior. Also: for user
defers with non-idempotent side effects (file close, lock release),
double-fire is a real bug.

**Root cause:** `ir_lower.c` NODE_GOTO emits `emit_defer_fire(ctx)`
which generates `IR_DEFER_FIRE` with mode "fire all, no pop". The
function-exit defer fire then emits the same set again. The emitter
needs to track which defer entries have already been fired on the
current dynamic path, OR not fire defers when the goto target is
inside the same defer scope (let the natural function exit fire them).

**Fix estimate:** ~50-80 lines in `ir_lower.c` to compute "is goto
target inside current defer scope" and skip the fire if so. Requires
walking from goto site to label site through the lexical block
structure. Alternative: a runtime per-defer "fired" flag — simpler
but adds per-defer state.

**Workaround today:** users SHOULD NOT rely on goto-fires-defer for
non-idempotent cleanup. Either avoid goto-to-same-scope-label OR use
explicit cleanup at the label.

## ~~4 narrow zercheck patterns not in zercheck_ir~~ (FIXED 2026-05-04, Phase F3.2)

**Originally discovered:** 2026-05-03 Phase F3 audit when `test_zercheck.c`
was deleted. **All four patterns now caught by zercheck_ir.**

Pattern 2 (direct overwrite leak) and Pattern 4 (struct copy alias UAF)
were fixed in commit `ce1d82a` (Phase F3.1, 2026-05-03):
- IR_COPY handler detects overwrite-while-alive on non-temp dest
- Compound handles propagate through struct value copies (two-pass
  collect + replicate, survives ir_add_compound_handle realloc)

Pattern 1 (wrong pool detection) and Pattern 3 (free-then-realloc loop
FALSE POSITIVE) were fixed in this session (2026-05-04):

**Pattern 1**: added `pool_name`/`pool_name_len` fields to `IRHandleInfo`,
captured at alloc sites via `ir_extract_pool_name`, propagated through
COPY and orelse-ident alias paths. New `ir_check_expr_wrong_pool` walker
mirrors `ir_check_expr_uaf` recursion shape and flags GET/FREE calls
where receiver name differs from the handle's recorded pool. Catches
both `pool_b.get(h).id` (NODE_FIELD wrapping) and bare-statement
`pool_b.free(h)`. ~120 LOC.

**Pattern 3 root cause**: `ir_merge_states` was missing the
`ALIVE + MAYBE_FREED → MAYBE_FREED` case. The lattice was non-monotonic:
when `first_live` had ALIVE and a later pred had MAYBE_FREED, the join
fell through and kept ALIVE. Result: state oscillated ALIVE↔MAYBE_FREED
across loop iterations and the fixed point never converged. Fix added
the missing case (and the symmetric TRANSFERRED + MAYBE_FREED). ~6 LOC.

**Tests** (added 2026-05-04):
- `tests/zer_fail/wrong_pool_get.zer` — must reject pool_b.get(h)
- `tests/zer_fail/wrong_pool_free.zer` — must reject pool_b.free(h)
- `tests/zer/free_realloc_loop.zer` — must compile + run cleanly

---

## OPEN — `naked` is a silent marker, not real-naked

**Discovered:** 2026-05-01 audit (BUG-651 sibling).

Functions declared `naked` compile cleanly but the IR emission path
does NOT emit `__attribute__((naked))`. GCC therefore inserts a
normal prologue/epilogue around the user's hand-written asm. The
existing `tests/zer/asm_*.zer` suite implicitly relies on this — none
of those asm bodies include explicit `ret` instructions.

Restoring real naked semantics requires:

1. Emit `__attribute__((naked))` from `emit_regular_func_from_ir`.
2. Update every existing asm test to include `ret`/`bx lr`/`ret` in
   the asm body.
3. Add a checker error for `return expr;` inside naked (ABI broken
   when there's no prologue to set the return register).
4. Document the migration in CLAUDE.md and reference.md.

This is a user-visible breaking change. Until then, `naked` enforces
the asm-only body restriction (BUG V4 fix from 2026-04-12) but does
NOT actually elide the prologue. The false-naked semantics are sound:
regular C prologue + asm body + regular C epilogue is well-defined;
users who expected true naked would fail to assemble due to the
missing `ret` they didn't write.

## OPEN — `@once` lacks `__STDC_HOSTED__` guard

**Discovered:** 2026-05-01 audit.

Emitter unconditionally produces `__atomic_exchange_n(&_zer_once_N, 1,
__ATOMIC_ACQ_REL)` for `@once` blocks (emitter.c:8313). GCC implements
`__atomic_*` via libatomic on targets without lock-free CAS for the
relevant width — freestanding/baremetal builds without libatomic
linkage will fail to link or fall back to a non-atomic implementation
that's racy across cores. Should guard with `__STDC_HOSTED__` or emit
a target-specific intrinsic where available.

## OPEN — `@probe` silently succeeds on freestanding

**Discovered:** 2026-05-01 audit.

`@probe(addr)` returns `?u32`. Hosted builds catch SIGSEGV and return
null on fault. Freestanding builds (no signal handler) return
`{ .has_value = 1, .value = <whatever the load returned> }` — silent
garbage on bad MMIO addresses. Either disable `@probe` on freestanding
or add a target-specific fault handler hook.

## OPEN — `@critical` indirect return via callee

**Discovered:** 2026-05-01 audit. checker.c around lines 8983 and 10016.

Direct `return`/`break`/`continue`/`goto` inside `@critical` is rejected.
Calling a function whose body returns from the caller's perspective
escapes `@critical` without re-enabling interrupts:

```zer
void unlock() { return; }   // legal
@critical { unlock(); }     // interrupts NOT re-enabled
```

Catching this requires call-graph analysis or function summaries
(`can_escape` predicate), similar to the existing transitive deadlock
detection. Tracked in the audit roadmap; defer until concurrency
work resumes.

## OPEN — AST `emit_expr` compound `/=` and `%=` lack signed-overflow trap

**Discovered:** 2026-05-01 audit. emitter.c:1433–1444.

BUG-612 fixed `INT_MIN / -1` trap emission for the IR path
(`emit_rewritten_node` at emitter.c:5787–5808). The AST sibling at
1433–1444 only emits the divzero trap. Reachability through user
function bodies is limited (function bodies are IR-only since
2026-04-19), but other emission contexts (some statement-expression
fallbacks) still go through `emit_expr`. Apply the same `INT_MIN`
guard pattern as the IR path.

## OPEN — u64 atomic warning fires on 64-bit targets

**Discovered:** 2026-05-01 audit. checker.c around lines 6601, 6637.

The "may require libatomic on 32-bit" warning is emitted for every
`@atomic_*` on a u64 operand, regardless of `--target-bits`. False
positive on 64-bit hosts. Gate the warning on `target_bits < 64`.

---

## ~~BUG-579~~ (FIXED 2026-04-18, v0.4.9)

Switch arm body gaps — enum/union/optional switches now fully lower to IR.

## ~~BUG-581~~ (FIXED 2026-04-18)

`zerc --run` now propagates exit codes via `WEXITSTATUS` on POSIX.

## ~~BUG-582~~ (FIXED 2026-04-18)

Union variant tag update is emitted on the IR path for all target chain
shapes (`u.v = x`, `u.v[i] = x`, nested fields).

## ~~BUG-590 group — per-block defer firing, variable shadowing, capture scoping~~ (FIXED 2026-04-18)

`IRLocal.scope_depth` + `IRLocal.hidden` + scope-aware `ir_find_local`
handle variable shadowing correctly. `NODE_BLOCK` fires+pops its own defers
at block exit using the same POP_ONLY bb_post trick as loops, so
early-exit paths (return/break/continue/orelse-return) that emit earlier
blocks still find the defer bodies on the emit-time stack. When the
enclosing construct manages defers itself (loop, if-branch, switch arm),
`block_defers_managed` suppresses the block's own fire to avoid duplicates.

## ~~BUG-591~~ (FIXED 2026-04-18)

`await` condition is now re-evaluated on every poll. The IR_AWAIT emitter
emits `case N:;` followed by a fresh evaluation of the AST cond via
`emit_rewritten_node`, instead of reusing a stale pre-computed local.

## ~~BUG-592~~ (FIXED 2026-04-18)

Signed/unsigned comparison in IR_BINOP: when one side is signed and the
other unsigned, cast the unsigned side to the signed type before
emitting the operator. Without this, `signed_local < 0ULL` evaluated to
`false` because C promoted the signed operand to unsigned first. Also
IR_LITERAL now emits `(CType)N` cast to match the local's type instead
of always-ULL suffix.

## ~~BUG-593~~ (FIXED 2026-04-18)

Comptime float functions now dispatch to `eval_comptime_float_expr`
directly when return type is `f32`/`f64`. The integer `eval_comptime_block`
path was doing integer arithmetic on raw double bit-patterns and
returning garbage instead of `CONST_EVAL_FAIL`, short-circuiting past
the float path.

---

## ~~Silent gaps — 6 closed 2026-06-03~~ (FIXED)

Targeted audit found and closed six distinct silent gaps:

1. **`arena.alloc_slice` not classified as IRMC_ARENA_ALLOC** — slice
   from `ar.alloc_slice(T, n)` was not tagged `ZC_COLOR_ARENA`, so
   `arena.reset()` did not flag it. Post-reset access compiled clean.
   Fixed in `zercheck_ir.c`.
2. **Shared-struct `return field` leaked the mutex** — IR_UNLOCK emitted
   after IR_RETURN became dead code. Cross-thread access deadlocked.
   Fixed in `ir_lower.c` NODE_BLOCK loop.
3. **For-init read of shared struct without lock** — `for (u32 i = g.v;
   ...)` raced the field. Fixed in `ir_lower.c` NODE_FOR handler.
4. **For-step write to shared struct without lock** — same shape as #3,
   step expression side. Fixed in `ir_lower.c` NODE_FOR handler.
5. **Shared struct passed by value** — embedded mutex copied; function
   locked its own copy → zero synchronization with caller. Fixed in
   `checker.c` call-arg validation.
6. **`@ptrcast` launders arena pointer escape** — the arena escape check
   only matched direct NODE_IDENT values; `g = @ptrcast(*u32, arena_ptr);`
   bypassed it. Fixed in `checker.c` NODE_ASSIGN handler by unwrapping
   `@ptrcast`/`@cast` before the ident check.

See `BUGS-FIXED.md` "Session 2026-06-03" for full root-cause + fix
narrative per gap. Tests in `tests/zer/shared_return_no_deadlock.zer`,
`tests/zer/shared_for_init_locked.zer`,
`tests/zer_fail/arena_alloc_slice_uaf.zer`,
`tests/zer_fail/shared_struct_by_value.zer`,
`tests/zer_fail/arena_ptrcast_escape.zer`.

---

## Remaining known failures

### No skipped tests

All tests run. The 2 mmio hardware tests moved to `rust_tests/qemu/` and
run under QEMU Cortex-M3 (see `docs/compiler-internals.md` "QEMU MMIO
test infrastructure").

---

## Phase 1 audit findings (2026-04-19 — 52 adversarial tests, 24 safety systems)

Full systematic audit of the 29-system framework. 52 adversarial `.zer`
programs were written, one or more per system, each attempting to
trigger a safety-system violation. Expected: every test compile-errors.
Observed: 7 real gaps + 1 silent miscompilation (fixed) + 7 over-pessimistic
spec claims (corrected — tests revealed the claims were weaker than
the implementation actually guarantees).

Reproducers live in `tests/zer_gaps/` (committed as documentation of
current behavior — NOT in the `make check` run, since they pass when
they should fail). When a gap is fixed, move the reproducer to
`tests/zer_fail/` as a regression test.

### Real safety/correctness gaps

**Status update (2026-05-21 audit):** all 7 originally-listed gaps from
the 2026-04-19 audit are NOW CAUGHT by the production compiler.
Re-verified empirically — see status column. Phase F migration to IR-CFG
analyzer + multiple bug fixes since then closed them. Left in place as
history; can be removed once a follow-up audit re-confirms.

| # | Short name | Gap | Status 2026-05-21 |
|---|---|---|---|
| 1 | **Cross-block backward goto UAF** | `free(h); goto LABEL; ... LABEL: ... use(h)` | **CAUGHT** — IR fixpoint convergence widens through backward edges; reports `use after free: 'mh' is maybe-freed` |
| 2 | **Same-line UAF suppressed** | `h.free(p); u32 x = p.x;` on same line | **CAUGHT** — zercheck_ir reports `use after free: 'h' is freed (freed at line 7)` |
| 3 | **`yield` outside async silently stripped** | `void go() { yield; }` compiles; emits no-op | **CAUGHT** — `error: 'yield' only allowed inside async function` |
| 4 | **async + shared struct across yield** | `shared struct` access across `yield` = deadlock | **CAUGHT** — checker.c:5034-5038 rejects shared struct access in yield statement |
| 5 | **`container<move struct>` loses move semantics** | `Box(Tok) b; b.item = t; consume(b.item); b.item.k` | **CAUGHT** — zercheck_ir reports `use after free: 'b' is transferred` (move tracking through field writes works for the documented pattern) |
| 6 | **`goto` into if-unwrap capture scope** | `goto inside; if (m) \|v\| { inside: ... }` | **CAUGHT** — `error: goto 'inside' jumps into if-unwrap/switch-capture arm without binding the capture` |
| 7 | **`defer` nested in `defer` body** | `defer { defer { ... } }` | **CAUGHT** — `error: 'defer' cannot be nested inside another 'defer' body` |

### Precision issues (not safety)

- **VRP doesn't propagate `u32 i = literal_value`** (`tests/zer_gaps/s12_range_oob.zer`) — direct `arr[10]` is rejected at compile time, but `u32 i = 10; arr[i];` only triggers auto-guard warning. Safe via auto-guard emission. VRP improvement opportunity.
- **`*opaque` cast to wrong type inside same function** (`tests/zer_gaps/s5_param_prov.zer`) — param used as both `*A` and `*B`. Compile accepts; runtime type_id check traps on the wrong-type cast. Same class as Gap 1 — compile blind, runtime catches.

### Fixed this session

- **Comptime loop truncation** — silent miscompilation where 10k iter cap
  stopped loops without error, returning truncated values. See BUG
  entry above / `tests/zer_fail/comptime_loop_truncation.zer`.
- **Mutual recursion handle tracking** (Gap 2 from earlier spec) —
  fixed via iterative FuncSummary refinement. See
  `tests/zer_fail/mutual_recursion_uaf.zer`.

### Spec corrections (claims were stronger than needed)

Systematic adversarial testing found several EDGE CASES in the safety
spec that were pessimistic — the implementation actually handles them:

- **Pass-by-value move transfer** is caught (spec said "not a transfer").
- **Mutual recursion with `% N` return range** is propagated (spec said cleared).
- **Simple 2D array UAF** is caught (spec said "not covered").
- **Defer fires after return expression** — using handle in return expr before defer free is legal (test was wrong, not a gap).
- **Semaphore release without acquire** is legal (initial-count pattern).
- **Per-statement shared(rw) locking** is correct (test was wrong).
- **goto skipping a `defer`** is correct (defer never pushed = no fire, semantically fine).

### Empirical coverage

- All 4 safety models covered.
- All 24 safety-critical systems tested at least once.
- Infrastructure systems 1 (Typemap) and 2 (Type ID) not adversarially
  tested — they're self-validating (broken Typemap = no test compiles;
  broken Type ID = cross-module cast mismatches). Existing 1400+
  passing tests indirectly validate them.

---

## Phase 2 audit findings (2026-04-19 — code-inspection targeted tests)

After the phase 1 behavioral audit, I read zercheck.c/checker.c/emitter.c
looking for structural weaknesses (fixed buffers, depth caps, TODO
markers) and wrote targeted tests for each candidate. Reproducers in
`tests/zer_gaps/audit2_*.zer`.

### ~~Severe — `[*]T` slice bounds check missing on IR path~~ (FIXED 2026-04-19, commit 3bdcf85)

Fixed as part of the Phase 3 sweep — `IR_INDEX_READ` + `emit_rewritten_node`
NODE_INDEX now emit `_zer_bounds_check` wrapper for slices. Both
READ and WRITE covered (comma operator preserves lvalue).
Retained below as audit history.

### Severe — `[*]T` slice bounds check missing on IR path (REGRESSION)

**Reproducers:** `audit2_slice_oob.zer`, `audit2_slice_star_oob.zer`.

`emitter.c:7498` `IR_INDEX_READ` handler emits raw `src.ptr[idx]` for
TYPE_SLICE sources with NO `_zer_bounds_check` call. The comment claims
"Bounds checks are in the AST path (emit_expr via IR_ASSIGN
passthrough)" — but function bodies have been IR-only since 2026-04-19,
so the AST TYPE_SLICE branch at `emitter.c:2045-2067` is never reached.

**Verified across three entry points:**
- stack array coerced to `[*]T` via `arr[0..]`
- arena-allocated slice from `ar.alloc_slice(T, n)`
- function parameter `[*]T s`

All emit `s.ptr[idx]` unchecked. Runtime silently reads stale/OOB
memory, exit 0.

**CLAUDE.md currently claims:**
> "[*]u8 data; dynamic pointer to many — {ptr, len}, bounds checked"

That claim is CURRENTLY FALSE for any slice indexing after IR migration.

**WRITE path also broken.** Verified `s[i] = 99` emits `s.ptr[i] = 99`
with no bounds check. `IR_INDEX_WRITE` handler at `emitter.c:7626` is
a stub (`TODO` comment). Slice element assignment is currently an
uncontained buffer overflow primitive.

**Regression timeline (confirmed via git archaeology):**
- Commit `010ddea` (2026-04-15, "Phase 8b: local-ID emission for
  BINOP/UNOP/FIELD_READ/INDEX_READ") replaced `emit_expr(inst->expr)`
  with direct local-ID emission. The AST emitter had the bounds
  check at `emitter.c:2045-2067` TYPE_SLICE branch; direct emission
  strips it.
- Commit `82335c3` (2026-04-17, "flip use_ir default") made IR path
  default. Regression became effective.
- Tests didn't catch it because VRP proves most real-world slice
  indexes safe, eliminating the need for runtime check.

**Fix:** ~15-20 lines in `IR_INDEX_READ` emitter — port the TYPE_SLICE
branch from AST `emit_expr`. Same needed for `IR_INDEX_WRITE` (which
is currently only a stub).
**Priority 0.** Highest-impact safety gap in the codebase.

### Major — backward goto cross-block (Gap 1 root cause confirmed)

**Reproducers:** `audit2_cross_block_goto.zer` (+ `_handle` variant
that traps at runtime proving the class), `audit2_goto_across_scope.zer`,
`audit2_labels_32_overflow.zer`.

`zercheck.c:1636` collects labels into block-local `labels[32]`.
`zercheck.c:1668` backward-goto iteration keyed on that array. Two
failure modes:

1. **Cross-block:** goto inside inner block targets label in outer
   block. Inner block's `labels[]` doesn't contain the outer label, so
   `label_idx = -1`, iteration doesn't fire, UAF across the cycle
   missed.
2. **Buffer overflow:** `labels[32]` is a fixed-size stack buffer.
   A block with 33+ labels silently drops the rest (CLAUDE.md rule
   #7 violation — stack-first dynamic pattern not applied here).
   Backward goto to a label past index 32 = label not found =
   no iteration.

Same root cause fix subsumes all three: replace block-local label
collection with CFG analysis, OR at minimum use stack-first dynamic
array for labels[]. The full CFG fix is ~300 lines.

### Moderate — `_scan_depth < 8` spawn transitive data race detection

**Reproducer:** `audit2_spawn_transitive_depth.zer`.

`checker.c:6466` caps transitive call-chain scanning at 8 levels. A
10-level chain `spawn entry() → f1 → f2 → ... → f10 → g = g + 1` does
not detect the non-shared global touched at the end of the chain.
CLAUDE.md already documents "Transitive through callees (8 levels)"
as the design limit, but 8 is low for real call graphs.

**No runtime fallback** — data races silently occur.

**Fix:** raise cap to 16-32, or memoize per-function scan result to
avoid re-analysis. The cap exists to prevent infinite recursion on
recursive call chains.

### Confirmed NOT-gaps (positive coverage — keep as regression tests)

- `audit2_funcsummary_chain.zer` — 6-level free chain propagates via
  FuncSummary iterative refinement.
- `audit2_nested_if_chain.zer` — 5-level else-if chain with handle
  freed in 4/5 arms correctly flagged MAYBE_FREED.
- `audit2_switch_partial_transfer.zer` — 5-arm switch with 3 freeing,
  2 not → MAYBE_FREED correctly emitted.

### Behavior to investigate further

- `audit2_defer_scan_nested.zer` — 32 levels of nested `if (c) {...}`
  with defer at innermost compiles clean. Unclear whether
  `scan_stack[32]` overflow was hit and zercheck still found the defer
  via direct walk, or whether depth wasn't actually > 31. Requires
  instrumentation to confirm.

### ~~AST→IR emission audit — 6 more runtime-check regressions~~ (FIXED 2026-04-19, commit 3bdcf85)

**All 7 Phase 3 regressions FIXED.** Full details in `BUGS-FIXED.md`
under BUG-595 through BUG-599. Test suite green: 290/290 ZER
integration, 139/139 convert, 200/200 negative, all subsuites
unchanged. The section below is retained as audit history and
methodology reference.

### AST→IR emission audit — 6 more runtime-check regressions found

After confirming the slice bounds check regression, I ran a systematic
AST→IR diff audit: grep every `_zer_trap` / `_zer_bounds_check` /
`_zer_shl` / `_zer_probe` call-site in `emit_expr` (AST path, now
mostly dead for function bodies), then wrote one reproducer test per
mechanism. Reproducers in `tests/zer_gaps/ast_*.zer`.

**All regressions in same window** — commits `010ddea` (2026-04-15,
Phase 8b local-ID emission) through `82335c3` (2026-04-17, IR default).

| # | Mechanism | AST path | IR path | Reproducer |
|---|---|---|---|---|
| 1 | Slice bounds check (READ) | `_zer_bounds_check` emitter.c:2045 | raw `s.ptr[i]` emitter.c:7498 | `audit2_slice_oob.zer` |
| 2 | Slice bounds check (WRITE) | same as READ | `IR_INDEX_WRITE` stub `/* TODO */` emitter.c:7626 | same |
| 3 | Slice range `arr[a..b]` (a > b) | `_zer_trap("slice start > end")` emitter.c:2258 | raw `se - ss` (underflow) | `ast_slice_empty_range.zer` |
| 4 | Signed division overflow (INT_MIN/-1) | `_zer_trap("signed division overflow")` emitter.c:1068 | raw `a / b` (C UB) | `ast_signed_div_overflow.zer` |
| 5 | Shift over width (`x << n` where n ≥ width) | `_zer_shl` macro (clamps to 0) | raw `x << n` (C UB) | `ast_shift_over_width.zer` |
| 6 | @inttoptr mmio range (variable address) | `_zer_trap("outside mmio range")` emitter.c:2650 | raw cast, no check | `ast_inttoptr_mmio.zer` |
| 7 | @inttoptr alignment (variable address) | `_zer_trap("unaligned address")` emitter.c:2660 | raw cast, no check | `ast_inttoptr_align.zer` |

**NOT regressions** (still protected):
- Division by zero — checker forces compile-time guard (can't reach without explicit `if (b==0)`)
- @ptrcast type mismatch — checker catches at compile time via provenance
- Compile-time-provable array OOB (`arr[10]` on `u32[4]`) — checker error
- Array runtime OOB with variable index — `emit_auto_guards` separate pass still works
- @trap / @probe — emitted correctly through IR (verified)
- Handle gen check — `_zer_slab_get` runtime always called, independent of emit path

**Root cause for all 7:** commit `010ddea` replaced `emit_expr(inst->expr)`
routing with direct local-ID emission in IR handlers. Every safety-emit
that `emit_expr` wrapped around expressions was stripped. Arrays
survived because `emit_auto_guards` runs as a separate pass before IR
lowering. Slices and the other mechanisms have no separate-pass
equivalent.

**Impact:** Currently shipping v0.4.5 produces binaries with:
- Unchecked buffer overflows on any `[*]T` indexing
- Silent integer UB on shifts and signed division
- No MMIO range or alignment safety on variable-address @inttoptr

**Fix is localized:** port each safety-emit from `emit_expr` into the
corresponding IR emitter handler. Estimated:
- IR_INDEX_READ/WRITE: ~30 lines
- IR_BINOP (shift, signed div): ~20 lines
- IR_CAST / @inttoptr handling: ~30 lines
- Slice `[a..b]` range check: ~10 lines

Total ~90 lines in emitter.c. No checker or IR data structure changes.
Would graduate the compiler from "unsafe in ways CLAUDE.md claims it
isn't" back to its pre-IR-migration safety level.

### Doc accuracy issue

CLAUDE.md states `alloc_ptr/free_ptr` is "100% compile-time safe for
pure ZER code." That is aspirational — zercheck has known gaps, and
**unlike Handle, `*T` from `alloc_ptr` has NO runtime generation
check**. Post-free pointer deref reads stale slab memory silently
(verified: Handle variant of `audit2_cross_block_goto` traps via gen
check; `*T` variant returns 0). Update doc to state: "compile-time
only for pure ZER; prefer Handle when runtime fallback is desired."

Also: only `*T` has `alloc_ptr/free_ptr`. `[*]T` has no equivalent —
it must come from `arena.alloc_slice` (whole-arena-reset semantics)
or from `arr[0..]` coercion (stack). CLAUDE.md should make this
explicit.

---

## Bare-metal: `.bss` zeroing requirement (Gap 13, 2026-04-27)

ZER's "everything auto-zeroed" guarantee depends on the C runtime startup
zeroing the `.bss` section before `main()` runs. On hosted targets
(Linux/Win/macOS), the C runtime (`crt0`/`crt1`) handles this automatically.

**On bare-metal targets (Cortex-M, RISC-V, custom kernels), the user-supplied
linker script + startup assembly MUST zero `.bss` before calling main.**

Without this, uninitialized globals hold whatever random values were in RAM
at boot — silently breaking ZER's auto-zero guarantee.

Standard pattern in startup `.S` files:
```asm
ldr r0, =__bss_start
ldr r1, =__bss_end
mov r2, #0
bss_zero:
    cmp r0, r1
    beq bss_done
    str r2, [r0], #4
    b bss_zero
bss_done:
```

See ARM Cortex-M reference startup code, ESP-IDF's `cpu_start.c`, or
Linux kernel's `head.S`. ZER cannot enforce this from inside its emitted
code — it's a build-system / startup contract.

If your target's startup does NOT zero `.bss`, ZER's safety guarantees
on global variable initial state are void. Verify your linker script
includes the `.bss` zeroing loop before claiming bare-metal correctness.

---

## *opaque ghost handle wrap pattern (Gap 4, 2026-04-27)

`*opaque` pointers crossing the C-interop boundary (via `cinclude`-declared
functions returning/taking `*opaque`) have heuristic-only lifetime tracking
in zercheck. Coverage is ~98% for typical patterns but the residual ~2%
requires the **wrap pattern** + `--track-cptrs` flag for full safety.

### When the heuristic suffices

zercheck recognizes the following patterns automatically:
- `void destroy(*opaque)` — bodyless void → assumed-free heuristic
- `int xyz_close(*opaque)` — bodyless non-void with destructor-name in
  the function name (free, destroy, close, release, delete, dispose,
  drop, cleanup, deinit, fini, shutdown, term) → assumed-free
  heuristic (Gap 17 fix, 2026-04-27)
- ZER wrapper functions where the body is visible — full FuncSummary
  tracking applies

### When the wrap pattern is required

If the C library has an idiosyncratic destructor name (no recognized
substring) AND the function body is invisible (cinclude only), the
heuristic doesn't fire. Solution: write a thin ZER wrapper.

```zer
// thin wrapper makes intent explicit and gives zercheck a body to scan
void mylib_dispose(*opaque h) {
    @ptrcast(*MyType, h);  /* validate type via provenance */
    mylib_xyz_terminate_session(h);  /* obscure C name */
}
```

### `--track-cptrs` runtime backup

For the residual 2% (anonymous casts, dynamic dispatch through C function
pointers), enable `--track-cptrs`. This compiles in:
- `__wrap_malloc`/`__wrap_free` linker wrap
- Inline allocation header on every `*opaque` pointer
- Runtime UAF detection at every `@ptrcast` deref

Cost: ~1ns per `@ptrcast` deref + extra header bytes per allocation. NOT
applicable to bare-metal (requires libc malloc).

### Coverage summary

| Pattern | Compile-time | Runtime backup | Total |
|---|---|---|---|
| Pure ZER (no `*opaque`) | 100% via Handle states | not needed | 100% |
| `*opaque` with wrap functions | ~99% | not needed | ~99% |
| `*opaque` raw cinclude + dtor-name fn | ~98% (heuristic) | not needed | ~98% |
| `*opaque` raw cinclude + opaque-name fn | ~80% (alias only) | `--track-cptrs` | ~99% |
| `*opaque` through dynamic funcptrs | ~50% (no track) | `--track-cptrs` | ~95% |

Recommendation: write ZER wrappers around C libraries. The wrap pattern
is documented architecture and pays back as runtime overhead saved +
clearer code intent.

---

## `naked` attribute silently dropped on IR path (deferred 2026-05-02)

**Status:** known regression from IR migration; not fixed because fixing
breaks every existing `tests/zer/asm_*.zer` test.

**Symptom:** ZER source declaring `naked void f() { asm { ... } }`
emits C without `__attribute__((naked))`. GCC therefore generates a
normal prologue + epilogue around the asm body. The asm appears to
"work" because the implicit prologue/epilogue saves/restores callee-
saved registers and ensures `ret` happens via the epilogue.

**Why this is a real loss of safety semantics:**

A genuinely naked function has no compiler-generated prologue/epilogue
— the user controls every byte. This matters for:

- Interrupt handlers using `iret` directly (no implicit `ret`)
- Boot/reset handlers that haven't set up the stack yet
- Context switch primitives that save callee-saved registers themselves
- Code that needs exact frame layout (no surprise spills)

With the implicit prologue, these scenarios silently malfunction (or
leak/corrupt registers) but the asm "compiles".

**Why it's deferred:**

Existing tests/zer/asm_*.zer (10+ files) all rely on the implicit
prologue/epilogue. Their asm bodies omit explicit `ret` instructions.
Re-enabling `__attribute__((naked))` would SIGILL each of them at
runtime (function falls off the end without `ret`).

Restoring true naked semantics requires:
1. Updating every existing asm test to include explicit `ret` /
   `iret` / `eret` per architecture
2. Adding a checker rule that bans `return expr;` inside naked
   functions (only `return;` or no return at all permitted)
3. Auditing user-facing docs and bumping any "naked" examples
4. A user-visible breaking change announcement

That's a separate migration effort, not in scope for the bug-fix
sessions. The current state (silent attribute drop) is documented
here so fresh sessions know it's INTENTIONAL deferral rather than
oversight.

**Workaround for users TODAY:** if you actually need true naked
semantics, write the function in C and link via `cinclude`. This is
the pattern used in real projects shipping production firmware.

**Tracking:** when the asm-test migration lands, re-enable the
attribute in `emit_func_attributes` (emitter.c) and remove this entry.

---

## OPEN — defer body uses a handle the function body then frees → silent UAF (2026-06-15)

**Symptom:** a deferred call *uses* a handle (`defer use_item(h);`), the
function body then transitions that handle out of ALIVE (`gp.free(h)`,
`slab.free(h)`, move-consume) before returning; at scope exit the defer
fires on the stale handle. **No diagnostic.** Runtime: pool/slab gen
mismatch traps; move-struct is silent UB.

Reproducer (compiles cleanly when it shouldn't — confirmed on 2026-06-15):
```zer
struct Item { u32 id; }
Pool(Item, 4) gp;
void use_item(Handle(Item) h) { gp.get(h).id = 5; }
void run() {
    Handle(Item) h = gp.alloc() orelse return;
    defer use_item(h);    // scheduled use at scope exit
    gp.free(h);           // h now FREED in path state
    return;               // defer fires use_item(h) on a FREED handle
}
```
The non-defer form `gp.free(h); use_item(h);` IS correctly rejected — only
the defer-scheduled use slips through.

**Root cause:** `zercheck_ir.c` `ir_defer_scan_frees` (~line 1351) walks
each defer body ONLY for free calls (so `defer gp.free(h)` folds into the
exit state correctly). It does NOT scan defer bodies for non-free *uses*
of a handle. The main-body walker sees `gp.free(h); return;` and signs off;
the deferred `use_item(h)` is never checked against the post-body state.

**Why the obvious fix isn't trivial:** defers fire LIFO and a single defer
body can legitimately use-then-free — `defer { use_item(h); gp.free(h); }`
is the canonical safe cleanup and must stay accepted. A naive "scan all
uses then apply all frees" over-rejects it.

**Fix sketch (deferred — net-new analysis pass):** at function exit, walk
defers in LIFO order; for each defer body walk its statements top-down
against a snapshot of the path state — a non-free USE checks the snapshot
(emit use-after-free/use-after-move if FREED/TRANSFERRED), a FREE applies
to the snapshot, then advance. Fold the snapshot into the return state.
Originally found in the 2026-06-07 audit (cool-johnson-InoCW); reproducer
`tests/zer_gaps/audit_2026-06-07_defer_use_after_body_free.zer`.

---

## OPEN — 2026-06-18 multi-agent bug-hunt (BH-18): index + reproduction harness

A 12-finder adversarial bug hunt against current HEAD (`cc374ab`) found and
triple-verified (finder → independent verifier → maintainer re-run in a
container) **14 distinct compiler bugs**. All compile **clean** unless noted.
Six are memory-unsafe soundness holes. Each entry below (BH-18 #1..#14) is
self-contained: minimal reproducer, exact observed/expected, the asymmetric
control that proves it is a gap (not a design choice), root cause, and a fix
sketch. A fresh session can reproduce every one with only the steps here.

**How to reproduce (any fresh session):**
```
# build the compiler (native or in Docker; Docker avoids Windows AV):
gcc -O1 -w -I. -o zerc lexer.c parser.c ast.c types.c checker.c emitter.c \
    zercheck.c ir.c ir_lower.c zercheck_ir.c vrp_ir.c zerc_main.c src/safety/*.c
# OR: make zerc

zerc x.zer --run                 # compile+run; prints "zerc: running x" then process exit = main()'s return
zerc x.zer -o x.exe              # compile only (clean compile = no 'error:' line, exit 0)
zerc x.zer --emit-c -o x.c       # inspect the emitted C (proves dropped guards / placeholders)
# To PROVE an out-of-bounds access is real (#2,#4,#5), ASan the emitted C:
zerc x.zer --emit-c -o x.c && gcc -fsanitize=address -g -O0 x.c -o x_asan && ./x_asan
```
A "soundness hole" = clean compile + memory-unsafe/guarantee-violating at
runtime (the crown-jewel class — ZER claims 100% program-consequence
coverage). "miscompile" = clean compile + wrong runtime result. Severity tags:
🔴 critical (memory-unsafe), 🟠 high (race / double-free), 🟡 medium
(miscompile), 🟢 low (false-reject / diagnostic).

| # | Bug | Class | Root-cause area |
|---|---|---|---|
| 1 | move-struct pointer-alias defeats ownership/free tracking (heap UAF + use-after-move + double-consume) | 🔴 soundness | zercheck_ir move/alias |
| 2 | VRP range-narrow scope leak → OOB write | 🔴 soundness | checker.c if-VRP |
| 3 | `@bitcast` forges integer↔pointer | 🔴 soundness | checker `@bitcast` |
| 4 | `@pun(*Struct,*primitive)` skips type_id trap → OOB read | 🔴 soundness | emitter `@pun` guard |
| 5 | fixed-array bare-call index drops bounds check | 🔴 soundness | emitter index single-eval |
| 6 | `if (opt) \|*v\|` capture escapes to a global | 🔴 soundness | capture desugar + escape prov |
| 7 | shared-struct multi-access via cast/intrinsic/index/orelse subexpr evades lock check → race | 🟠 race | checker `collect_shared_types_in_expr` |
| 8 | `spawn` data-race scan blind to funcptr indirection → race | 🟠 race | checker `scan_unsafe_global_access` |
| 9 | shared-struct read in `await` condition unlocked → race | 🟠 race | checker `NODE_AWAIT` handler |
| 10 | value-returning `async` never finalizes state machine | 🟡 miscompile | emitter `IR_RETURN` async |
| 11 | bit-query/byte-swap intrinsics emit `0` in global initializers | 🟡 miscompile | emitter AST `NODE_INTRINSIC` |
| 12 | `defer` + backward `goto` fires wrong count (folds into known #5) | 🟡 miscompile | ir_lower defer/back-edge |
| 13 | nested inline designated initializer false-reject | 🟢 false-reject | checker `validate_struct_init` |
| 14 | conversion-intrinsic arity not validated (missing/excess args) | 🟢 diagnostic | checker intrinsic arg check |

---

## OPEN — BH-18 #1 — move-struct pointer alias defeats ownership/free tracking (🔴 soundness)

**Symptom:** a `*T` pointer alias taken **before** a `move struct` is consumed
(or before its owned pointer field is freed) is never linked to the source's
`HS_TRANSFERRED`/`FREED` state. Three escalating, clean-compiling manifestations
— the worst is a genuine **heap use-after-free with slab slot reuse**.

**1a — heap UAF + slot reuse (memory corruption):**
```zer
struct Task { u32 id; }
Slab(Task) pool;
move struct Owner { *Task p; }
void release(Owner o) { pool.free_ptr(o.p); }
u32 main() {
    Owner o;
    o.p = pool.alloc_ptr() orelse return;
    o.p.id = 100;
    *Task alias = o.p;          // raw-ptr alias copied out before the move
    release(o);                 // moves o AND frees the slab slot (cross-function)
    *Task fresh = pool.alloc_ptr() orelse return;  // reuses the just-freed slot
    fresh.id = 222;
    u32 corrupted = alias.id;   // reads the REUSED slot -> 222, no trap
    pool.free_ptr(fresh);
    return corrupted;
}
```
Observed: clean compile, `EXIT=222` (reads memory now owned by `fresh`).
`@ptrtoint(alias) == @ptrtoint(fresh)` proves they share one slot. The
`alloc_ptr` raw-pointer path has **no** runtime generation backstop (emitted C
is a bare `alias->id` deref), so it corrupts **silently** — the Handle-field
variant at least traps at runtime (slab gen check, EXIT=133).

**1b — use-after-move stale read** (stack; logical violation, no corruption):
```zer
move struct Tok { u32 kind; }
u32 main() { Tok a; a.kind = 11; *Tok p = &a; Tok b = a; return p.kind; }
```
Observed: clean compile, `EXIT=11` — reads the moved-from value through `p`.

**1c — double-consume / double-close** (re-consumes a unique resource):
```zer
move struct FileHandle { i32 fd; }
i32 g_closes;
void close_file(FileHandle f) { g_closes += 1; }
u32 main() {
    FileHandle f; f.fd = 3;
    *FileHandle alias = &f;
    close_file(f);                  // first consume
    FileHandle reborn = *alias;      // resurrect moved-from f via the alias
    close_file(reborn);             // second consume of the SAME fd
    return (u32)g_closes;            // EXIT=2 -> double-close
}
```

**Control (proves it is a gap, not design):** every NON-alias form IS caught.
Direct `*Task a = pool.alloc_ptr()...; *Task alias = a; release(a); alias.id`
→ `zercheck: use after free`. Direct `Tok b = a; a.kind` → `use after move:
'a' ... transferred`. Direct `close_file(f); close_file(f)` → `use after
move`. Only routing the post-move read/free through a `*T` alias taken before
the transfer escapes both nets.

**Root cause:** move-transfer marks the source variable `HS_TRANSFERRED`, but
(a) it does not propagate that state to a pre-existing pointer alias
(`*T p = &x`), and (b) for a move-struct *field* the transfer clears/overrides
the owned field allocation's tracking instead of linking the field's `alloc_id`
to its aliases before transfer. ZER already does the equivalent
alias-propagation for Pool/Slab interior pointers (shared `alloc_id`, BUG-488/494)
and the design note in `docs/refactor_ir.md` (~1036-1045) explicitly states
"TRANSFERRED does NOT propagate to aliases" under the assumption "source's
alloc_id has no aliases at the time of transfer" — taking `&f`/`alias = o.p`
before the move **violates that precondition**.

**Fix sketch (track, don't ban):** when `&x` is taken of a move-tracked
variable (or a raw-ptr copy of a move-struct's owned field is made), register
the alias to share `x`'s move/free-state key (declaration-site aliasing, same
pattern as handle `alloc_id`). Then `HS_TRANSFERRED`/`FREED` flows to the alias
and the use/free through it is gated. Suggested tripwire: `tests/zer_fail/`
three reproducers (1a/1b/1c) must each produce a use-after-free/use-after-move
error.

**Distinctness:** NOT BUG-740 (funcptr consume-maybe — caught here), NOT
BUG-742 (conditional global dangle), NOT defer item #9 (no defer, no Handle).

---

## OPEN — BH-18 #2 — VRP range-narrowing scope leak → unchecked OOB write (🔴 soundness)

**Symptom:** a recognized bounds guard (`if (idx >= N) { return; }`) **nested
inside a non-comparison `if` (e.g. `if (b)`)** leaks its range narrowing
(`idx <= N-1`) out to the unconditional path. The compiler then "proves" the
later array index safe and emits it with **no** `_zer_bounds_check` and **no**
auto-guard and **no** warning — but the guard only ran on the `b == true` path.

**Reproducer:**
```zer
u32 main() {
    u32[4] buf;
    buf[0] = 0;
    u32 idx = 0;
    for (u32 k = 0; k < 5; k += 1) { idx += 1; }   // idx == 5 (laundered past VRP)
    bool b = false;
    if (b) {
        if (idx >= 4) { return 0; }   // guard runs ONLY when b is true
    }
    buf[idx] = 2989;                  // idx==5 -> OOB write, NO guard emitted
    return buf[0];
}
```
Observed: clean compile (no warning), `--run` EXIT=0 (silent stack corruption).
ASan on the emitted C: `AddressSanitizer: stack-buffer-overflow ... WRITE of
size 4 ... [32,48) 'buf' <== Memory access at offset 52 overflows this
variable`. Expected: auto-guard or `_zer_bounds_check` (exactly what the
plain `buf[idx]` path emits when idx is unprovable), or a "not proven in range"
warning.

**Control (proves it is a scope leak, not guard-trust):**
- baseline (no inner guard) → "index not proven in range — auto-guard inserted", ASan clean.
- outer **comparison** `if (mode == 1) { if (idx>=4) return; }` → guard emitted, ASan clean (this branch saves/restores `saved_range_count`).
- **unconditional** `if (idx>=4) return;` at top level → no guard emitted but genuinely SOUND (ASan clean) — proves ZER does NOT blanket-trust guards; only the nested-non-comparison scope leak is wrong.
- cross-function `pick(5,false)` returning the guarded value → `find_return_range` derives a bogus `[0,3]` and the caller's index is unchecked too (ASan overflow).

**Root cause:** in the checker if-statement VRP handler, the
`/* non-comparison condition — no range narrowing */` branch calls
`check_stmt(then_body)` **without** saving/restoring `c->var_range_count`
around it. The nested guard's inverse-range push (`idx.max = 3`, intended to
"stay valid after the if") therefore persists past the `if (b)` body and is
treated as unconditionally valid.

**Fix sketch:** save/restore `var_range_count` (or scope the pushed ranges)
around the non-comparison branch exactly as the comparison branch already does
with `saved_range_count`. Tripwire: `tests/zer_trap/` — the reproducer must
trap (or `tests/zer/` with the auto-guard warning), never run clean.

---

## OPEN — BH-18 #3 — `@bitcast` forges integer↔pointer, bypassing the mmio/inttoptr gate (🔴 soundness)

**Symptom:** `@bitcast(*T, intval)` (and `@bitcast(uN, *T)`) reinterprets an
arbitrary integer as a pointer (and back) with a **clean compile** — no mmio
range check, no alignment check, no `@inttoptr`/`@ptrtoint` gate. On 64-bit a
pointer and `u64` are both 8 bytes, so `@bitcast`'s same-width check passes and
int↔ptr reinterpretation is permitted. Round-tripping through `u64` also
synthesizes the pointer arithmetic ZER explicitly bans.

**Reproducer (silent write through a forged pointer):**
```zer
u32 main() {
    u32 real = 12345;
    u64 raw = @bitcast(u64, &real);
    *u32 p = @bitcast(*u32, raw);
    p[0] = 99;            // writes through a forged pointer, no gate
    return real;          // EXIT=99
}
```
Observed: clean compile, `EXIT=99`. (Forged offset variant: bitcast `&arr[0]`
to u64, `+= 8`, bitcast back, deref → reads `arr[2]`, the banned `ptr+N`.)

**Control:** the identical conversion via the intended primitive is rejected —
`*u32 p = @inttoptr(*u32, addr);` → `error: @inttoptr requires mmio range
declarations`. And direct `*u32 q = p + 2;` → `error: arithmetic requires
numeric types`. So all three guards (`@inttoptr` mmio, `@ptrtoint`, no-ptr-math)
are enforced and `@bitcast` circumvents all three.

**Why it matters:** ZER claims grammar-level closure — "no in-language unsafe",
every value entering a pointer must cross a typed, mmio-validated boundary.
`@bitcast` is an unguarded escape hatch for the entire mechanism. (Note: the
runtime "ZER TRAP" some inputs hit is just the OS SIGSEGV handler for an
unmapped address — a mapped/in-range forged address reads/writes silently, as
EXIT=99 shows.)

**Root cause:** the `@bitcast` checker handler allows the cast whenever
`src`/`dst` widths match; it does not reject the case where exactly one of
`{src, dst}` is a pointer type.

**Fix sketch:** in the `@bitcast` checker, reject when exactly one operand is a
pointer (point users at `@inttoptr`/`@ptrtoint`, mirroring the existing C-style
`(*T)int` → "use @inttoptr" diagnostic). Pointer↔pointer and scalar↔scalar bit
reinterpretation stay allowed. Tripwire: `tests/zer_fail/bitcast_int_ptr.zer`.

---

## OPEN — BH-18 #4 — `@pun(*Struct, *primitive)` silently skips its runtime type_id trap → OOB read (🔴 soundness)

**Symptom:** `@pun`'s documented guarantee is "runtime type_id check that traps
on mismatch." A fully-typed in-ZER pointer to a **primitive** (`*u32`, `*u8`,
a slice `.ptr`, an `@inttoptr` result) packs `type_id == 0`, and the emitted
guard `if (type_id != TARGET && type_id != 0) trap;` short-circuits to false —
so the trap is skipped even when the sizes plainly mismatch.

**Reproducer:**
```zer
struct Big { u64 a; u64 b; }
u32 main() {
    u32 small = 7;
    *u32 sp = &small;
    *Big bp = @pun(*Big, sp);   // 4-byte target punned to 16-byte struct, no trap
    return (u32)bp.b;           // reads offset 8, past the 4-byte 'small'
}
```
Observed: clean compile, runtime garbage; ASan: `stack-buffer-overflow ... READ
of size 8 ... underflows this variable`. The emitted guard is
`(_zer_opaque){(void*)(sp), 0}; if (0 != 1 && 0 != 0) _zer_trap(...)` →
`(true && false)` → trap skipped.

**Control (proves the bypass is type_id==0-specific):** `@pun` between two
**struct** types (both type_ids nonzero) correctly traps:
`ZER TRAP: @pun type mismatch` / EXIT=133. And `@ptrcast(*Big, *u32)` is
correctly compile-rejected ("type confusion — use @pun"). Only `@pun` with a
primitive/slice source slips through.

**Root cause:** the `type_id == 0` escape clause in the `@pun` runtime guard was
intended for genuinely-unknown-provenance pointers (cinclude/extern `*opaque`),
but in-ZER primitive/slice-element pointers also carry `type_id == 0`.

**Distinctness:** This is the WORKS-AS-DESIGNED note "type_id=0 (cinclude)
skipping the **@ptrcast** check" applied to the **wrong** intrinsic and the
wrong source — it is `@pun` (not `@ptrcast`, which here compile-rejects) and an
**in-program** primitive pointer (not a cinclude `*opaque`).

**Fix sketch:** in the `@pun` lowering, do not apply the `!= 0` escape when the
source is a fully-typed in-ZER primitive/slice pointer; OR add a compile-time
size-widening check at the `@pun` site (the 4-vs-16-byte mismatch is statically
known). Tripwire: `tests/zer_fail/pun_primitive_to_struct.zer`.

---

## OPEN — BH-18 #5 — fixed-array index that is a bare function call drops the bounds check (🔴 soundness)

**Symptom:** indexing a fixed-size array by a **bare function call**
(`a[idx()]`) emits a raw C subscript with **neither** the auto-guard (used for
simple variable indices) **nor** the `_zer_bounds_check` wrapper (used for
arithmetic indices and slices). This is the documented BUG-595..612
emission-diff class: a single-eval path for the side-effecting index never got
the bounds wrapper that `emit_expr` applies elsewhere.

**Reproducer:**
```zer
u32 g = 100;
u32 idx() { return g; }
u32 main() {
    u32[4] a;
    a[idx()] = 999;   // idx()==100 -> OOB write into a u32[4], no trap
    return 5;
}
```
Observed: clean compile (no warning), `EXIT=5` (no trap). Emitted main body:
`_zer_t0 = a[idx()] = 999;` — bare subscript. Also reproduces for read
(`v = a[idx()]`) and compound (`a[idx()] <<= 1` — the `_zer_shl` shift guard is
kept but the bounds check is still dropped).

**Control (proves VRP is NOT proving it safe, the check was dropped):** the
SAME OOB value through any other index shape traps or auto-guards —
`a[idx()+0]` → `ZER TRAP: array index out of bounds` (EXIT=133); slice
`s[idx()]` (`[*]u32 s = a;`) → same trap; simple `u32 i = g; a[i]` → "not
proven in range — auto-guard inserted". Wrapping the call in trivial arithmetic
re-routes it through the guarded complex-expression path.

**Root cause:** the emitter path that single-evaluates a side-effecting index
for a **fixed array** (so `idx()` is called once) omits the bounds wrapper.
Slices go through a different, correctly-guarded path.

**Fix sketch:** port the `_zer_bounds_check` / auto-guard emission to the
fixed-array side-effecting-index single-eval path (mirror the slice path / the
`a[idx()+0]` complex-expression path). Run the BH audit grep before committing:
`grep -nE "_zer_trap|_zer_bounds_check|_zer_shl" emitter.c` — every AST-region
match needs an IR-path equivalent. Tripwire: `tests/zer_trap/array_call_index_oob.zer`.

---

## OPEN — BH-18 #6 — `if (opt) |*v|` mutable capture escapes a pointer-to-local to a global (🔴 soundness)

**Symptom:** `if (opt) |*v| { ... }` binds `v = &m.value` — a pointer **into**
the local optional `m`. Storing `v` into a global is a dangling-pointer escape,
but the capture-desugared `v` does not carry local-derived provenance, so the
escape check is bypassed. After the function returns, the global points at dead
stack.

**Reproducer (scalar):**
```zer
?*u32 g = null;
void stash() {
    ?u32 m = 5;
    if (m) |*v| { g = v; }      // pointer-to-local m escapes to global g
}
u32 main() { stash(); if (g) |gv| { return gv[0]; } return 0; }
```
Observed: clean compile (only a benign bounds-check warning), runtime returns
dead-stack garbage (varies; inserting a clobber between `stash()` and the read
changes the value — proving it reads a reclaimed frame, not the stored 5).
The struct variant (`?P` with `P { u32 x; }`) reproduces identically.

**Control:** the syntactically-direct form IS rejected —
`void stash(){ ?u32 m=5; g=&m.value; }` → `error: cannot store pointer to local
'm' in static/global variable 'g'`. The escape analysis exists and fires for
`&m.value` written by hand; it only misses the same address synthesized by the
`|*v|` capture binding.

**Root cause:** the `|*v|` capture desugars to a synthesized
`v = &m.value` whose result does not inherit the `is_local_derived` escape
provenance, so System-11 scope-escape analysis treats `g = v` as a normal
global store.

**Fix sketch:** mark the capture binding `v` from `|*v|` as local-derived
(pointer into the local optional's storage), so the existing escape check fires
on `g = v` exactly as it does for `g = &m.value`. Tripwire:
`tests/zer_fail/capture_ptr_escape_global.zer` (scalar + struct).

**Distinctness:** NOT BUG-742 (that is a freed-heap MAYBE_FREED-at-exit case);
this is an unconditional stack-local-address escape, caught in every direct
form, missed only through `|*v|` capture desugaring.

---

## OPEN — BH-18 #7 — shared-struct multi-access in one statement evades the deadlock/lock check via a cast/intrinsic/index/orelse subexpression → data race (🟠 race)

**Symptom:** reading a shared-struct field inside a `(T)cast`, `@intrinsic(...)`,
array index, or `orelse` subexpression — while assigning another shared
struct's field in the same statement — compiles clean. The shared-type
collector does not recurse into those node kinds, so it sees only ONE shared
type and stays silent; the emitter (lock-per-statement) then locks one struct
and reads the other **unlocked**.

**Reproducer:**
```zer
shared struct A { u32 x; }
shared struct B { u32 y; }
A a; B b;
void f(*A pa, *B pb) { pa.x = (u32)pb.y; }   // reads B's field under only A's lock
u32 main() { return 0; }
```
Emitted `f()`:
```c
void f(struct A* pa, struct B* pb) {
    pthread_mutex_lock(&pa->_zer_mtx);
    _zer_t0 = pa->x = ((uint32_t)pb->y);   // <-- reads pb->y of struct B with NO B lock
    pthread_mutex_unlock(&pa->_zer_mtx);
}
```
ThreadSanitizer confirms a real read/write race on `b` (the two threads hold
different mutexes M0=A, M1=B). Also reproduces via `@bitcast(u32, pb.y)` and
`pb.arr[pb.idx]`.

**Control:** the plain binary form `pa.x = pb.y;` IS rejected —
`error: deadlock: single statement accesses both 'A' (order 1) and 'B'
(order 2)`. Only wrapping one access in a cast/intrinsic/index/orelse evades it.

**Root cause:** `collect_shared_types_in_expr` (checker.c, ~14597) recurses
into `NODE_BINARY`/`NODE_ASSIGN`/`NODE_UNARY`/CALL-args but NOT into
`NODE_CAST`/`NODE_TYPECAST`/`NODE_INTRINSIC`/`NODE_INDEX` (the index
sub-expression)/`NODE_ORELSE`.

**Fix sketch:** add those node kinds to the collector's recursion so the
deadlock/multi-lock check sees both shared types (then the binary-form error
fires for the cast form too). Tripwire: `tests/zer_fail/shared_cast_subexpr.zer`.

---

## OPEN — BH-18 #8 — `spawn` data-race scan is blind to function-pointer indirection → data race (🟠 race)

**Symptom:** the spawn non-shared-global scan follows only **direct** calls. A
call through a function pointer (a `*()` param `cb()`, or a local
`*() fp = do_increment; fp()`) is invisible, so a non-shared global mutated in
the indirectly-reached callee is never flagged.

**Reproducer:**
```zer
u32 g_counter;
void do_increment() { g_counter = g_counter + 1; }
void run_n(*() cb, u32 n) { for (u32 i = 0; i < n; i += 1) { cb(); } }
void worker() { run_n(do_increment, 500000); }
u32 main() { spawn worker(); spawn worker(); return 0; }
```
Observed: clean compile (only a "stack depth not verifiable" warning), no
data-race error. TSan confirms a read+write race on `g_counter`.

**Control:** the direct-call form `void worker() { do_increment(); }` IS
rejected — `error: spawn target 'worker' accesses non-shared global
'g_counter' — data race`. Direct calls are even transitive (multi-level), so
the indirect miss is a genuine escape, not a depth limit. Contradicts the
limitations.md "spawn non-shared global (direct + transitive) — 0 holes"
claim, which tested direct-call depth only.

**Root cause:** `scan_unsafe_global_access` (checker.c, ~8491) only descends
`NODE_CALL` with a `NODE_IDENT` callee resolvable to a function symbol; it skips
funcptr call sites entirely instead of treating an unresolvable indirect call
conservatively.

**Fix sketch:** apply the BUG-740 argument-precise-barrier discipline — an
unresolvable indirect call inside a spawn target should widen to a possible-race
(error/conservative), not be silently skipped. (A trivial intraprocedural
resolution would even catch the `fp = do_increment; fp()` local case.) Tripwire:
`tests/zer_fail/spawn_funcptr_global_race.zer`.

---

## OPEN — BH-18 #9 — shared-struct access in an `await` condition is not locked (D02 false-negative) → data race (🟠 race)

**Symptom:** accessing a `shared struct` field in an `await` condition compiles
clean and emits an **unlocked** read, violating both the D02 "no shared access
in a yield/await statement" ban and the "shared struct = auto-locked" guarantee.

**Reproducer:**
```zer
shared struct Flag { u32 ready; u32 data; }
Flag g;
async void waiter() {
    await g.ready > 0;     // shared read in await cond -> D02 should reject, doesn't
    g.data = g.ready;      // (this one IS properly mutex-wrapped)
}
```
Emitted poll: `case 1:; if (!((g.ready > 0))) { self->_zer_state = 1; return 0; }`
— `g.ready` read with **no** `pthread_mutex_lock(&g._zer_mtx)`, while every
other access to `g` in the program is mutex-wrapped. The await condition is
re-evaluated on every poll while suspended; a concurrent `spawn setter()` (whose
write IS locked) races it. Pointer form `*Flag p = &g; await p.ready > 0;` also
slips through.

**Root cause:** the D02 ban (checker.c, ~5722) is gated on
`c->in_async_yield_stmt`, which is set only for `NODE_EXPR_STMT` and
`NODE_VAR_DECL` whose expression contains yield (checker.c, ~8677-8681). A bare
`await cond;` is a `NODE_AWAIT` statement (checker.c, ~11717) — neither node
kind — so the flag is never set. Because `yield`/`await` are statements (not
expressions), the await condition is the only realistic way to have a shared
access inside a suspending statement, so the unguarded path is exactly the
reachable one.

**Fix sketch:** one-line parity — set `c->in_async_yield_stmt` around the
`check_expr` of the `await` condition in the `NODE_AWAIT` handler (or detect
`NODE_AWAIT` in `check_stmt` like the other two node kinds). Tripwire:
`tests/zer_fail/await_shared_unlocked.zer`.

---

## OPEN — BH-18 #10 — value-returning `async` never finalizes its state machine (🟡 miscompile)

**Symptom:** `async u32` / `async ?u32` (any `return <value>;` in an async body)
never sets `self->_zer_state = -1` on completion and returns the user value
instead of the poll done-flag. Result: (1) the coroutine tail **re-executes on
every subsequent poll** (re-runs side effects), and (2) `while(poll()==0)`
breaks because the user value is indistinguishable from the "not done" flag.

**Reproducer:**
```zer
u32 side;
async u32 compute() { yield; side += 1000; return 42; }
u32 main() {
    side = 0;
    _zer_async_compute task;
    _zer_async_compute_init(&task);
    _zer_async_compute_poll(&task);   // poll 1: yield
    _zer_async_compute_poll(&task);   // poll 2: completes, side += 1000
    _zer_async_compute_poll(&task);   // poll 3: should be no-op...
    _zer_async_compute_poll(&task);   // poll 4: ...but re-runs the tail
    return side / 1000;               // EXIT=3 (tail ran 3x); expected 1
}
```
Observed: `EXIT=3`. Emitted completion block: `... side += ...; self->_zer_t1 =
(uint32_t)42; return self->_zer_t1;` — no `self->_zer_state = -1`. With
`return 0;` the canonical `while(poll()==0)` loop infinite-loops.

**Control:** `async void` (same body) correctly emits `self->_zer_state = -1;
return 1;` and is idempotent → `EXIT=1`.

**Root cause:** emitter `IR_RETURN` handler (emitter.c, ~9413): the `is_async`
finalization (`self->_zer_state = -1; return 1;`) lives only in the void/bare
return branch (~9466-9468). The value-return branch (~9456-9463) never checks
`func->is_async`. BUG-509 fixed the void path only; the value path was never
fixed. The checker accepts `async <non-void>` without rejection.

**Fix sketch:** in the value-return branch, when `func->is_async`, emit
`self->_zer_state = -1;` before returning and reconcile the poll protocol (the
poll signature is `int` done-flag; a value-returning async needs a separate
value-retrieval mechanism, not a return-value overload). Tripwire:
`tests/zer/async_value_return_idempotent.zer` (or reject `async <non-void>`
until a real value-retrieval API exists).

---

## OPEN — BH-18 #11 — bit-query/byte-swap intrinsics emit `0` in global initializers (🟡 miscompile)

**Symptom:** `@popcount`/`@ctz`/`@clz`/`@ffs`/`@parity`/`@bswap16`/`@bswap32`/
`@bswap64` used in a **global variable initializer** silently emit
`/* @X — unknown */0`. Clean compile, wrong value (and wrong control flow when
the global feeds a comparison).

**Reproducer:**
```zer
u32 g = @popcount(255);   // emitted: uint32_t g = /* @popcount — unknown */0;
u32 main() { return g; }   // EXIT=0 ; expected 8
```
Observed: `g == 0`. `@bswap32(1)` global → 0 (expected 16777216), and an
`if (g == 16777216)` then wrongly takes the false branch.

**Control:** the SAME intrinsic in a **function body** is correct
(`u32 x = @popcount(255);` → 8). And `@truncate`/`@bitcast`/`@size` ARE handled
in the global/AST path — so it is specifically these 9 intrinsics missing from
that path, not a general "no intrinsics in globals" limit.

**Root cause:** the documented two-emitter-path gotcha. The IR-rewritten path
(emitter.c, ~6561) handles popcount/ctz/clz/ffs/parity/bswap*, but the AST path
(`NODE_INTRINSIC`, emitter.c, ~2765 — used for global initializers) does not,
and falls through to the `/* @%.*s — unknown */0` placeholder. The checker
accepts the global because these return `u32` (type-checks fine).

**Fix sketch:** add handlers for the 9 bit-query/byte-swap intrinsics to the AST
`NODE_INTRINSIC` emitter path (mirror the IR path / the existing
`@truncate`/`@bitcast` AST handlers). Verify both paths with
`grep -n '"popcount"' emitter.c` returning TWO hits. Tripwire:
`tests/zer/bitquery_global_init.zer`.

---

## OPEN — BH-18 #12 — `defer` + backward `goto` fires the wrong count (🟡 miscompile; folds into known item "defer fires twice")

**Symptom:** a function-scope `defer` is lowered onto the backward-`goto`
**back-edge** block instead of the real exit paths, so it fires once **per
back-edge traversal** — N times for N traversals, **0** times when the back-edge
is never taken (silent skipped cleanup / leak), and never at the true return.

**Reproducer:**
```zer
u32 counter;
void inc() { counter += 1; }
void run() {
    u32 i = 0;
    defer inc();              // registered once, before the label
    loop:
    i += 1;
    if (i < 3) { goto loop; }  // 2 back-edges
}
u32 main() { run(); return counter; }   // EXIT=2 ; expected 1
```
Observed: `EXIT=2`. Parametric: bound `i<1`→0 fires (cleanup skipped), `i<2`→1,
`i<5`→4. With `defer pool.free_ptr(h)` this becomes a clean-compiling
double-free; with the back-edge never taken it becomes a leak.

**Control:** the structured-loop analog (`while (i<3) { i += 1; }`, same defer)
correctly fires once → `EXIT=1`.

**Root cause:** ir_lower defer lowering attaches the function-scope defer to the
same-scope `goto` back-edge block rather than to all real exit paths.

**Distinctness / honesty note:** this is the **same defect** as the existing
OPEN item "defer fires twice when a goto target sits in the same defer scope" —
this entry is a sharper characterization (parametric fire count, plus the
0-fire leak and the double-free escalation), not a wholly new bug. Track it as
an escalation of that item, not a separate fix. Tripwire (shared with that
item): the goto-loop defer must fire exactly once.

---

## OPEN — BH-18 #13 — nested inline designated initializer rejected ("got void") (🟢 false-reject)

**Symptom:** a designated initializer whose field value is itself an inline
brace literal is rejected — the inner literal is typed `void` because the
field's expected type isn't threaded into it. Reproduces in all three
value-flow sites (var-decl init, assignment, return).

**Reproducer:**
```zer
struct Inner { u32 x; u32 y; }
struct Outer { Inner pos; u32 id; }
u32 main() {
    Outer o = { .pos = { .x = 3, .y = 4 }, .id = 9 };   // error: field '.pos' expects 'Inner', got 'void'
    return o.pos.x + o.pos.y + o.id;
}
```
Observed: `error: field '.pos' expects 'Inner', got 'void'`.

**Control (proves the data model + emitter are fine):** hoisting the inner
literal to a named var compiles and runs — `Inner i = { .x = 3, .y = 4 };
Outer o = { .pos = i, .id = 9 };` → `EXIT=16`.

**Root cause:** `validate_struct_init` (checker.c, ~1194-1229) calls
`checker_get_type(df->value)` on each field value but does not recurse when
`df->value` is itself a `NODE_STRUCT_INIT`, so the inner literal never gets
validated against the field's declared type and types as `void`.

**Fix sketch:** in `validate_struct_init`, when `df->value->kind ==
NODE_STRUCT_INIT`, recurse `validate_struct_init(c, df->value, field_type,
line)` instead of comparing `checker_get_type` against the field type. (The
array-field variant `{ .data = { 10, 20, 30 } }` is a SEPARATE missing feature
— ZER has no bare-aggregate array-literal syntax at all; don't conflate.)
Tripwire: `tests/zer/nested_designated_init.zer`.

---

## OPEN — BH-18 #14 — conversion-intrinsic arity is not validated (🟢 diagnostic)

**Symptom:** the conversion/layout intrinsic family (`@truncate`, `@bitcast`,
`@saturate`, `@cast`, `@inttoptr`, `@ptrcast`, `@size`) does not validate
argument count: a **missing value operand** passes the checker and emits invalid
C (GCC then errors on a non-existent source line), and **excess trailing args**
are silently dropped.

**Reproducers:**
```zer
u32 main() { u8 x = @truncate(u8, 5, 6, 7); return (u32)x; }   // EXIT=5 — args 6,7 silently dropped
```
```zer
u32 main() { u8 x = @truncate(u8); return 0; }   // checker accepts; emits (uint8_t)(); GCC: "expected expression before ')'"
```
Observed: extra-args case compiles clean (`(uint8_t)(5)` emitted, EXIT=5);
missing-operand case passes the ZER checker (`--emit-c` exits 0) and only GCC
complains, mis-attributed to the wrong line.

**Control:** a normal user function enforces arity —
`add(1,2,3)` for a 2-param `add` → `error: expected 2 arguments, got 3`. And
`@atomic_load()` gives a clean checker error `@atomic_load requires 1
argument`. So the front-end has the mechanism; the conversion-intrinsic family
just doesn't apply it.

**Why it's diagnostic, not a soundness hole:** these programs never produce a
running binary, so there's no memory unsafety — the harm is silently-masked
typos (extra args) and a wrong-line/wrong-stage error (missing arg).

**Fix sketch:** add an exact-arity check to the conversion/layout intrinsic
checker handlers (reject too-few AND too-many), matching `@atomic_load`'s
"requires N argument" pattern. Tripwire: `tests/zer_fail/intrinsic_arity.zer`.

---

## CLOSED — keep transitivity through a function-POINTER forward (2026-06-19)

Initially left open, then **closed the same day** (option B). `keep_edge_propagates`
now worst-cases EVERY pointer param of a funcptr call (it just calls
`keep_edge_callee_keeps`), so forwarding a param to ANY funcptr — direct funcptr
param, global funcptr, or stored callback — infers keep on the forwarded param.
A stack pointer therefore can never reach a retaining callback via a forwarder;
`invoke(&local, retaining_cb)` is rejected. This makes a *forward through* a
funcptr consistent with a *direct* funcptr call (`fn(&local)` was already
rejected), closing the funcptr-forwarding hole to **100% soundness** for the
keep/escape property.

**Cost (measured, tiny):** a read-only callback called with a STACK-local context
is now also rejected (`compute(&local_ctx, reader)`) — use a long-lived context.
This restricts exactly ONE pattern across the whole suite
(`rust_tests/rt_opaque_provenance_chain.zer`, updated to a global context). The
precise alternative (resolve the concrete callback's inferred keep per call site
via a forwarding summary — preserves the read-only-stack-local idiom) was judged
not worth ~150 lines to save one rare pattern; revisit if it bites in practice.

---

## CLOSED — "semantic-fuzzer flake / expr-nesting-too-deep" was a STALE corrupt `.o`, not a code bug (2026-06-19)

**This corrects a misdiagnosis.** A prior note here blamed an "uninitialized-read
UB" for the `make zerc` build spuriously rejecting trivial programs with
`error: expression nesting too deep (limit 1000)` (semantic fuzzer ~165/200).
That was WRONG — the compiler source is fine.

**Real root cause:** a stale, MISCOMPILED `src/safety/comptime_rules.o` left in
the working tree (dated 2026-06-06, gitignored). In that object,
`zer_expr_nesting_valid` read its argument from register `%ecx` instead of
`%edi` (wrong calling convention), so it received stale garbage instead of the
nesting depth and rejected ~80% of programs. The `.c` source is OLDER than the
bad `.o`, so `make` saw the object as up-to-date and never rebuilt it. The
corrupt object almost certainly came from an OOM-interrupted / corrupted build
on 2026-06-06.

**Why it looked layout/`vrp_ir`-dependent:** any build path that RECOMPILES
`comptime_rules.c` — a single-`gcc` invocation, the `vrp_ir`-linked dev build, a
fresh `git archive` checkout, normal CI — produced a correct object (reads
`%edi`) → 200/200. Only builds that REUSED the stale object failed. "Baseline
fails identically" was true precisely because baseline reused the SAME stale
object.

**Fix (done):** (1) `rm -f *.o src/safety/*.o` clears the corrupt object;
(2) the Makefile `clean:` target now removes `src/safety/*.o` (it previously
removed only top-level `*.o`, which let the bad object survive every
`make clean`). Verified: fresh `make zerc` → `zer_expr_nesting_valid` reads
`%edi`, semantic fuzzer 200/200, full `make check` green.

**Lesson for future sessions:** if a `make`-built `zerc` spuriously rejects
trivial programs but a `git archive` / single-`gcc` build of the SAME source
passes, suspect a stale `src/safety/*.o` FIRST — `objdump -d src/safety/<x>.o`
and check which register the function reads its argument from. Don't chase a
Heisenbug in the source.

---

## CLOSED — 2026-06-17 audit (from plt86m branch): ALL 9 gaps FIXED

Nine gaps documented on branch `claude/cool-johnson-plt86m`, INDEPENDENTLY
re-verified real (each reproduced on baseline). **ALL 9 are now FIXED**
(2026-06-19d/e + 2026-06-20). Each has a `tests/zer_fail/` tripwire (and a
`tests/zer/` positive guard where relevant); see BUGS-FIXED.md.

**FIXED 2026-06-20 — `defer_goto_fallthrough_drops` (was HIGH miscompile):** a
defer scope with both a `goto` exit and a sibling fall-through exit dropped the
defer body on the fall-through path (shared `defer_stack` consumed by the
goto-path fire in block-ID order). Fixed via **capture-on-FIRE + a per-label
runtime "fired" flag** (ir.h + ir_lower.c + emitter.c). Each `IR_DEFER_FIRE`
carries its own live-defer snapshot (order-independent emission, fixes the
drop). A goto-only cleanup label resets defer_count; a both-reachable cleanup
label installs a runtime guard — the goto sets a bool flag and the label's
return-fire emits each goto-fired body as `if (!flag) { body }`, so the goto
path skips (fired eagerly) and the fall-through path fires AT the return after
eval (BUG-442 preserved). Three earlier shapes (plain capture-on-FIRE; +reset;
+fall-through-edge-fire) were each insufficient and are documented in
BUGS-FIXED.md "Session 2026-06-20" so they are not re-tried. Tests:
`tests/zer/defer_goto_{fallthrough_fires,both_reachable,return_reads_deferred}.zer`.
Architecture: docs/compiler-internals.md "capture-on-FIRE + runtime-flag defer
emission". (The earlier "needs CFG defer-liveness dataflow" pessimism was wrong
— a per-label runtime flag suffices.)

**FIXED 2026-06-19e (5 gaps):**
- `container_const_strip` — reject a `TYNODE_CONST`/`TYNODE_VOLATILE` container
  type arg (checker.c TYNODE_CONTAINER); the qualifier was dropped at
  monomorphization. `tests/zer_fail/container_const_type_arg.zer`.
- `mmio_range_ignores_size` — the @inttoptr range gate now requires the whole
  span `[addr, addr+sizeof(T)-1]` to fit (checker.c constant path + both emitter
  variable-address runtime traps). `tests/zer_fail/inttoptr_size_past_range.zer`
  + `tests/zer/inttoptr_u8_at_range_end.zer`.
- `var_index_move_array` — a variable-index move from a move-struct array is now a
  hard error (zercheck_ir.c). `tests/zer_fail/move_array_var_index.zer`.
- `defer_use_after_alloc_ptr`, `defer_use_after_move` — new `ir_defer_scan_uses`
  walker checks defer-body USES against the pristine exit state.
  `tests/zer_fail/defer_use_after_{alloc_ptr,move}.zer` +
  `tests/zer/defer_free_pattern_ok.zer`.
- x9-11 completeness — a non-constant bit-query intrinsic arg in a global
  initializer is now a clean ZER error (checker.c NODE_GLOBAL_VAR init check,
  NODE_FIELD-guarded for enum constants). `tests/zer_fail/global_init_nonconst_intrinsic.zer`.

**FIXED 2026-06-19d (Theme B, 3 distinct-const sites):** var-decl-init,
assignment, and call-arg const guards now use `type_dispatch_kind` +
`type_unwrap_distinct` (+ a symbol-level check, since `const MyPtr` stores
`const` on the SYMBOL). `tests/zer_fail/distinct_const_{var_decl,param,slice_param}_launder.zer`.

Note `container_const_strip` was tagged "KNOWN/WAD-maybe" in the original audit;
on verification the plain-const variant IS rejected while the container variant
silently dropped const — a real asymmetry, so it was fixed (reject), not waived.

---

## OPEN — Concurrency memory-safety: 3-sweep audit (~25 holes), four-axis closure (IMPLEMENTATION IN PROGRESS, 2026-06-21)

**Scope of this entry:** ZER's concurrency PRIMITIVES are all implemented
(shared/spawn/atomics/Semaphore/Barrier/condvar/Ring/async/move). This entry is
the standing ledger of the **memory-safety gaps** in the concurrency model —
verified data races + cross-thread use-after-free that compile clean. Full
design + Rust mapping + closure: `docs/primitives-data-races.md` §24. Per-hole
file:line detail: workflow task outputs `wpbbu8v47` / `wwt4c31zh` / `wgvm1bid5`.
**Do NOT yet claim ZER is data-race-safe as shipped** — it is *designed* to be
(auto-inferred Rust-equivalent safety); implementation has BEGUN (phase 2) but is
not complete.

**IMPLEMENTATION PROGRESS (phase 2, session 2026-06-21b) — 9 holes CLOSED
(BUG-743..751), each verified by the full ZER suite + C unit tests (every fix
below has a negative test in `tests/zer_fail/` that runs in `make check`, which
IS the regression gate; full `make check` GREEN — ZER 774, Rust 784, Zig 36,
modules 139, all audits OK):**
- **[FIXED BUG-743, Axis C]** `ir_merge_states` now merges `threads[]` (union by
  name, `joined` = AND over preds) + the convergence check compares thread state
  — the false-green scoped-spawn stack-UAF is closed. (The concrete soundness bug
  the audit flagged as "most actionable.")
- **[FIXED BUG-744, Axis A1]** spawn-arg dispatch is exhaustive over
  pointer/slice/opaque (`[*]T` over stack / `(*opaque)&local` now caught).
- **[FIXED BUG-745, Axis C2]** fire-and-forget spawn of a pointer to a STACK
  local (incl. `*shared T`) now rejected (lifetime arm).
- **[FIXED BUG-746, Axis A3]** volatile compound-RMW in a spawn target now flags
  (wired `zer_volatile_compound_valid`); simple volatile store still allowed.
- **[FIXED BUG-747, Axis A4]** Arena removed from the spawn scanner's
  safe-exclusion (concurrent `arena.alloc()` races).
- **[FIXED BUG-748, Axis D2]** `@probe` `_zer_in_probe`/`_zer_probe_jmp` now
  `__thread` (no cross-thread longjmp corruption).
- **[FIXED BUG-749, Axis B5]** deferred shared-struct access (`defer g.x = v;`)
  now lock-wrapped in the emitter.
- **[FIXED BUG-750, Axis A6/#5]** the interior-extraction ban (`&shared.field`)
  now also covers pointer-rooted (`*Counter c; &c.value`) and array-element
  (`&shared.arr[i]`) bypasses; `&whole_struct` still allowed. Also: the
  positive-test runner now has a 30s `timeout` so a deadlocking auto-lock fails
  red (exit 124) instead of hanging CI — the visibility mechanism for the B
  lock-scope redesign.
- **[FIXED BUG-751, Axis C scoped-borrow]** a parent WRITE to a non-shared local
  lent via `&x` to a scoped spawn, between `spawn` and `th.join()`, now errors
  (the thread has exclusive `&mut`-style access). Write-only + linear
  approximation; READ-during-borrow and cross-block remain as a tighter CFG
  version.
- **[FIXED BUG-752, Axis A6-full, #7 — atomic-cell inclusion model, slices 1/2/4
  DONE]** a scalar global used with `@atomic_*` is an atomic cell; in a
  fire-and-forget concurrent context, a plain WRITE (slice 1), a plain READ
  (slice 2), and an address-launder `&g` for non-atomic use (slice 4) are all
  flagged — the taint is non-strippable via `&` (only the `@atomic_*` target arg0
  is blessed). **Concurrency-aware (gated on fire-and-forget after-spawn), NOT
  strict-always** — strict false-positived 21 safe pre-spawn `g=0` inits; the
  gate keeps pre-spawn init + single-threaded + post-scoped-join access legal
  (matches "shared = reachable by ≥2 threads"). **LEARNING:** the inclusion model
  is NOT strictly simpler than the exclusion-list — it ALSO needs concurrency
  context. **Remaining [OPEN, narrow]:** struct-field atomics `@atomic_*(&s.f)` on
  a plain (non-shared) global struct (slice 3) — needs a parallel
  (struct-symbol, field) compound-key list (the scalar machinery keys on the
  Symbol, which has no per-field flag); uncommon + mostly logic race.
  **UPDATE: slice 3 DONE** — struct-field atomic cells now tracked field-precise
  (`Checker.atomic_fields`, write side). **A6-full atomic-cell taint COMPLETE**
  (scalar write/read/launder + struct-field, all concurrency-aware). The
  remaining exclusion-list entries (const/shared-struct/threadlocal/atomic/
  Barrier/Semaphore) are the genuinely-safe SYNCHRONIZED categories, not holes.
  Micro-residuals [OPEN, very narrow]: struct-field plain READS + `&s.f` launder
  (even rarer than the scalar equivalents; same machinery extended by field).

The remaining OPEN holes (B1–B4 lock-scope redesign, A5 threadlocal-escape, A6
shared-scalar representation incl. #7 atomic-cell uniformity, and the
scoped-borrow READ/CFG residue) are the deadlock-sensitive / type-system-extension
/ subsystem-scale pieces; each is annotated `[OPEN]` below. **D1 (cinclude
thread-capture) is RECLASSIFIED as a named FLOOR, not a hole** (C-domain behavior,
out of ZER's scope; safe path already exists via long-lived data — see Axis D).
**Risk classification (confirmed 2026-06-21b):** a botched lock-scope redesign's
worst NEW failure is a DEADLOCK = a hang = the liveness floor (NOT a memory-safety
violation; out of scope for ZER *and* Rust), now made VISIBLE by the runner
timeout; the shared-scalar extension's risk is a FALSE-POSITIVE = a visible
compile error on a positive test. Neither is a new memory-safety hole, so both are
safe to iterate on ("loop till green"); the under-lock botch direction cannot make
memory safety worse than the already-open hole.

Three adversarial, code-grounded sweeps found **~25 holes**; the find-rate did NOT
decay (9 → 11 → ~10 new), proving they are generated by a few architectural roots,
not scattered bugs. All map to **four axes**:

**Axis A — exclusion-list reachability scanner** (`scan_unsafe_global_access`
checker.c + spawn-arg handler). Every exclusion / forgotten type-kind is a hole:
- **[FIXED BUG-746]** `volatile` whitelisted → thread `volatile` compound-RMW
  race. Now: `NODE_ASSIGN` case wires `zer_volatile_compound_valid`; volatile
  compound-RMW in a spawn target flags, simple volatile store still allowed.
  "or volatile" removed from the spawn fix suggestion.
- **[FIXED BUG-747]** `Arena` whitelisted → concurrent `arena.alloc()` races.
  Now removed from the safe-exclusion (Barrier/Semaphore kept — internally synced).
- **[OPEN — A5]** `threadlocal` whitelisted (CORRECT for direct access — a
  spawned thread reads its own TLS copy) BUT `&threadlocal` untainted → publishing
  `&tl` to a shared/global carrier = cross-thread wrong-TLS/UAF. This is an
  escape/taint SINK, not the scanner; fold into the A6 taint work (mark `&tl`
  tl-derived, reject store to a global/shared carrier).
- **[FIXED BUG-744]** `TYPE_SLICE` + `TYPE_OPAQUE` spawn args were uncased → now
  the spawn-arg dispatch is exhaustive over pointer/slice/opaque; the
  `spawn_arg_is_stack_derived` helper unwraps casts + `@ptrcast/@bitcast/@cast/@pun`
  (so `(*opaque)&local` and `[*]T` over a stack array are caught).
- Remaining fix direction: carrier-or-tainted *inclusion* model (the full A6
  shared-scalar taint) replaces the exclusion list entirely; a `-Wswitch`-style
  gate on the spawn-arg dispatch.

**Axis B — single-root auto-lock incompleteness** (per-statement
`current_stmt_shared_root`, ir_lower.c). Locks only the first shared root; bypassed at:
- **[OPEN — B1]** `shared(rw)` multi-read (`x = ga.v + gb.v`) → only `ga` locked
  (BUG-500 removed the deadlock-skip for read locks). Read locks COMPOSE
  (deadlock-free), so the fix — lock ALL read roots in canonical order — is safe;
  but it requires making `current_stmt_shared_root` a SET (lock-scope redesign).
- **[OPEN — B2]** union-switch on a shared field: lock released before arm bodies;
  the arm capture is a live raw pointer into the shared bytes.
- **[OPEN — B3]** `@cond_wait` predicate: a 2nd shared read in the predicate gets
  no lock (`collect_shared_types_in_expr` has no `NODE_INTRINSIC` case).
- **[OPEN — B4]** `@once` loser doesn't wait for the winner → reads
  half-constructed published state. Fix: a 3-state flag (0 untouched / 1 in-progress
  / 2 done): winner CAS 0→1, runs body, stores 2; loser spins until 2. Needs CFG
  surgery (inject the done-store at body-block end + a spin in the loser path).
- **[FIXED BUG-749 — B5]** defer-body shared access: `emit_defer_stmt`'s
  `NODE_EXPR_STMT` now lock-wraps the deferred shared access
  (`emit_defer_shared_root` + `emit_shared_lock_mode`/`unlock`, recursive mutex).
  Narrow residue: shared access inside an `if`/`for`/`while` *condition* within a
  defer body is not yet wrapped (rare).
- Remaining-fix direction: one lock-scope-walker redesign covering all roots +
  switch-arm/cond bodies + the `@once` loser-wait. Deadlock-sensitive (multi-root
  WRITE locks must keep the same-statement-multi-shared-type ban; READ locks
  compose).

**Axis C — per-function CFG lattice (lifetime/temporal).**
- **[FIXED BUG-743]** `ir_merge_states` now merges `threads[]` (union by name,
  `joined` = AND over preds that contain it) and the convergence check compares
  per-thread join state — the false-green scoped-spawn stack-UAF (a `ThreadHandle`
  created on a non-`first_live` predecessor, silently dropped at the merge) is
  closed. Tests: `tests/zer_fail/spawn_branch_no_join.zer` (+ 2 positives).
- **[FIXED BUG-745]** stack-local pointer (incl. `*shared T`) to a fire-and-forget
  spawn now rejected (the lifetime arm — `spawn_arg_is_stack_derived`).
- **[OPEN]** remaining lifetime/temporal holes: free-after-publish across threads;
  detached grandchild outliving a scoped join; block-scoped Barrier/Semaphore
  outlived by a function-scoped join; async task struct raced by concurrent polls;
  block-scoped Barrier/Semaphore outlived by a function-scoped join. Fix
  direction: block-scope carrier lifetime tracking + publication lifetime-arm +
  Handle-gen runtime trap for freeable carrier payloads.
- **[FIXED BUG-751]** scoped-borrow exclusivity (the C3-investigation residual):
  a parent WRITE to a non-shared local lent via `&x` to a scoped spawn, between
  `spawn` and `th.join()`, now errors (the thread has exclusive `&mut`-style
  access). Symbol `is_borrowed_by_thread`/`th_borrows_name`; write-only + linear
  (statement-order) approximation. **[OPEN residue]** parent READ during the
  borrow window (a tighter case) and cross-block / aliased-pointer writes — a CFG
  version in zercheck_ir (borrow set merged like `threads[]`) would be exact.
- **General class warning (still OPEN):** a systematic "merged vs first_live-only"
  audit of EVERY `IRPathState` field — the Axis-C bug class is "a second lattice
  family not added to the merge". `threads[]` is now merged; verify no other field
  (e.g. future per-path lifetime tags) repeats the omission.

**Axis D — boundary/runtime concurrency-capture.** Concurrency entering with no
visible ZER node:
- **[FLOOR — D1, RECLASSIFIED 2026-06-21b — NOT a hole, NOT in scope]** FFI/cinclude:
  a ZER ptr/funcptr handed to a bodyless extern that `pthread_create`s internally.
  This is **C-domain behavior**, outside ZER's safety boundary by the same logic
  that makes ZER silent on *any* C-internal behavior (a `cinclude`d C function that
  double-frees, stashes, or over-writes your pointer is equally invisible —
  "C code is outside ZER's safety boundary", CLAUDE.md). It is NOT a
  program-consequence leak: the program-consequence claim is scoped to uses **in
  ZER source**; a C lib threading your pointer is a use **in C source**. It belongs
  with **deadlock and hardware-consequence as a named FLOOR**, not an OPEN hole. The
  earlier audit framing ("the one place the program-consequence claim leaks") was
  wrong — it was never *in* the claim.
  - **A VERIFICATION would be the contract-trap CLAUDE.md rejects:** trusting a
    `captures` annotation and claiming safety = accepting an unverifiable claim
    about C behavior = manufacturing FALSE safety. Do NOT build it as a closure.
  - **The safe path already exists today, no annotation needed (document this as
    the recipe):** the only hazard is *lifetime* (the C thread outlives the data),
    and ZER already lets you express data that outlives the thread — hand a
    capturing extern a **global**, a **global instance of a `shared struct`**, or
    **Pool/Slab-allocated** data (never `&stack_local`, the same discipline as
    "can't return `&local`"). That eliminates the cross-thread UAF with existing
    primitives. The remaining *mutual-exclusion* half (the C thread and ZER both
    touching the data) follows the **C library's own threading contract** — ZER's
    auto-lock does NOT reach into the C thread (C never acquires `_zer_mtx`); use
    `@atomic_*`/`*opaque` or the lib's locking, same as any FFI. This mirrors how
    ZER treats hardware: it hands you safe building blocks (lifetimes you control,
    `mmio` ranges), you apply them at the boundary.
  - **Optional future polish (NOT a closure):** a `captures`/`threads` marker on a
    cinclude param could be added purely as **audit visibility** (the asm
    `safety:`-string style) — it would let ZER enforce the in-scope *lifetime*
    consequence ("if you declare this param captured, the ZER pointer must be
    long-lived"). But it can't be inferred (no ZER body), doesn't protect the
    unaware user, and must NEVER be sold as "verified". Deferred unless users ask.
- **[FIXED BUG-748 — D2]** the compiler-emitted `@probe` runtime
  `_zer_in_probe`/`_zer_probe_jmp` are now `__thread` (were process-global statics
  raced by threads → cross-thread longjmp corruption). **[OPEN]** residue: other
  emitter-runtime statics (`@once` state — see B4, async task struct,
  shared-lazy-init CAS) want the same `__thread`-vs-`static` audit; a ZER fn
  installed as a POSIX signal handler via cinclude self-deadlocks its non-recursive
  mutex (the shared mutex is recursive, but a signal-handler re-entry on the SAME
  thread mid-critical-section is a separate hazard).

**The closure (ends all four):** put the invariant on the DATA — an inferred,
non-strippable `shared` taint (Model 4 extension of the `volatile` machinery)
propagated through `&`/casts/slices; auto-lock covers every sub-statement; the CFG
lattice merges every tracked-state family; the cinclude boundary is the named
FLOOR (D1 — safe via long-lived data, not a verification target); all frozen by a
CI audit gate so it cannot regress. `shared` on
scalars/pointers is INFERRED (keep/escape/provenance family) — the dumb user never
writes it; only the `shared struct` keyword stays, and it is demanded by an error.
This is the auto-inferred equivalent of Rust's `Send`/`Sync`+`'static` (Rust is the
existence proof it's achievable).

**Out of scope (named floor):** deadlock/livelock — undecidable, same boundary Rust
holds (a Rust `Mutex` AB-BA deadlock compiles fine); **D1 cinclude thread-capture**
— C-domain behavior, outside ZER's safety boundary (the safe path exists today:
hand capturing externs long-lived data — global / global `shared struct` instance /
Pool/Slab — never `&stack_local`; cross-C-thread mutual exclusion follows the C
lib's contract). ZER's per-statement auto-lock
already kills the lock-ordering deadlock class by construction.

**Status (2026-06-21b):** implementation phase 2 BEGUN — **7 of the ~25 holes
CLOSED** (Axis C `ir_merge_states` + A1 + C2 + A3 + A4 + D2 + B5; BUG-743..749),
each verified by the full ZER suite (769/769) + C unit tests, each with a
regression negative test in `tests/zer_fail/` that runs in `make check` (the gate).
The CLOSED set covers the reachability holes that need no representation change
(volatile-RMW, Arena, slice/opaque dispatch), the spawn lifetime arm, the runtime
race (`@probe`), and the most-common lock-completeness case (defer). REMAINING
(each annotated `[OPEN]` above) is the **subsystem-scale** core (like `keep` / the
IR migration): **B1–B4** the deadlock-sensitive lock-scope-walker redesign
(multi-root / union-switch / cond-predicate / `@once` loser-wait); **A5**
threadlocal `&`-escape taint; **A6** the `shared`-as-scalar/pointer qualifier
representation (the recurring blocker that subsumes A5 + the carrier-or-tainted
inclusion model); and the scoped-borrow READ/CFG residue (write-path FIXED in
BUG-751). **D1 is now a FLOOR, not a remaining build** (C-domain; safe path
exists). And the still-unprobed residue (FFI callback tables; other
emitter-runtime statics;
cross-module spawn/extern; `NODE_STRUCT_INIT` global read in a spawn body). These
each carry real risk (deadlock for the lock redesign, false-positives for the
type-system extension) and were deliberately NOT shipped half-verified — the next
session continues with the lock-scope-walker redesign (B1–B4) and the
`shared`-scalar representation (A6).

---

## Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.

When a `tests/zer_gaps/` reproducer is fixed, move it to
`tests/zer_fail/` so it becomes a permanent regression guard.
