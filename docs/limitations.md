# ZER Compiler — Known Limitations

Living document of known compiler limitations, audit findings, and deferred fixes.
Entries removed once fixed.

---

## OPEN — 6u360k audit (2026-06-09): 7 confirmed silent gaps

From branch `claude/cool-johnson-6u360k` (audit-only, reviewed not merged).
All 7 RE-VERIFIED present in current main (commit 7a75a58, with BUG-729..734
landed). Reproducers (NOT auto-run — each compiles clean / runs without trap,
which is the bug): `tests/audit_2026-06-09/*.zer`. The 8th audit gap (GAP-5,
orelse-reassignment overwrite leak) was **closed by BUG-734** this session —
not listed here. When fixing one, move its reproducer into `tests/zer_fail/`
or `tests/zer_trap/`.

- **GAP-1 — `@ptrcast` between unrelated CONCRETE types = silent type confusion (HIGH).**
  `*A pa=&a; *B pb=@ptrcast(*B,pa)` (A,B unrelated structs) compiles clean, no
  runtime trap — reads A's memory as B. Root cause: the provenance check
  (checker.c:~6262) only fires when the SOURCE is `*opaque`; concrete→concrete
  skips it and emits a plain `(B*)pa`. Docs call `@ptrcast` "provenance-tracked"
  and `@pun` DOES trap — discrepancy. Fix: reject concrete→different-concrete
  `@ptrcast` with a hint to `@pun`, OR emit `@pun`'s runtime type_id check.
  Repro: `gap_ptrcast_concrete_unrelated.zer`.

- **GAP-2 — `--no-strict-mmio` strips the RUNTIME range+align check (HIGH).**
  The flag is documented as relaxing COMPILE-TIME strictness, but it also drops
  the emitted `_zer_trap("@inttoptr: address outside mmio range")` and unaligned
  trap (verified: 0 trap-checks in emitted C with the flag). Bare-metal build at
  a runtime-computed address can silently write any peripheral / BusFault on
  Cortex-M0. Fix: `--no-strict-mmio` should relax only compile-time enforcement;
  the runtime range check stays whenever ranges are declared, and the alignment
  check stays unconditionally. Repro: `gap_nostrict_mmio_drops_runtime.zer`.

- **GAP-3 — `alloc_ptr` global-alias UAF silent at BOTH gates (HIGH).**
  `*T p = heap.alloc_ptr() orelse return; g_ptr = p; heap.free_ptr(p);
  *T gp = g_ptr orelse return; gp.value` — confirmed silent (reproducer returns
  the stale value 99, no trap). The global `?*T g_ptr` isn't registered as a
  compound key tracking p's alloc_id (the Handle variant was added, `*T` wasn't),
  and `*T` has no per-slot gen counter (unlike Handle) so no runtime net.
  Contradicts CLAUDE.md "alloc_ptr 100% compile-time safe." Fix: register globals
  storing `alloc_ptr` `*T` as compound keys in zercheck_ir so the later
  unwrap+deref shares p's alloc_id. Repro: `gap_alloc_ptr_global_alias_uaf.zer`.

- **GAP-4 — function-pointer free not tracked → silent double-free (HIGH).**
  Calling `fp(h)` through a funcptr whose target frees `h`, then `heap.free(h)`:
  silent (exit 0). zercheck_ir doesn't propagate the free across the indirect
  call, and `_zer_slab_free` is lenient on already-freed handles. Overlaps the
  existing "Gap 15 reverified" entry. Fix: conservative "any indirect call widens
  ALL ALIVE handles to MAYBE_FREED" barrier (or only handles of the funcptr's
  signature types). Repro: `gap_funcptr_double_free.zer`.

- **GAP-6 — array-element double-free with VARIABLE index (MEDIUM).**
  `heap.free(arr[k]); heap.free(arr[0])` with k==0 (runtime) compiles clean and
  runs silently. `ir_extract_compound_key` only accepts `NODE_INT_LIT` indices,
  so `arr[k]` and `arr[0]` aren't the same compound key. Fix: accept VRP-proven-
  const indices, or widen to "any index" (over-rejects, conservative).
  Repro: `gap_arr_var_index_dfree.zer`.

- **GAP-7 — container monomorphization with composite type args (MEDIUM, UX).**
  `Box(?u32)` / `Box(*u32)` / `Pair(Handle(Item))` emit C struct names like
  `Box_?u32` → GCC syntax error pointing at emitted C, not ZER source. Not a
  safety hole (loud at GCC) but a bad-diagnostic UX gap. Fix: reject non-identifier
  type args at the checker with a clean ZER error (Zig-style), OR a reversible
  name-mangling pass (`?`→`_opt_`, `*`→`_ptr_`, …). Repro: `gap_container_ptr_optional_arg.zer`.

- **GAP-8 — arena escape via struct copy through a value-typed parameter (MEDIUM).**
  `local.ptr = p` (p arena-derived) then `take(local)` (by-value `Container` param)
  then `c = ct` (store to global) slips the static arena-escape check — the
  arena-derived flag doesn't propagate through the field-store→carrier nor through
  the by-value param copy. Runtime catches via malloc-wrap fault on hosted (generic
  "memory access fault", not the arena-escape error); fully silent on bare-metal.
  Distinct from BUG-732 (struct-INIT local escape) and the 2026-06-07 struct-COPY
  work. Fix: (1) propagate `is_arena_derived` from field-store up to the carrier
  struct local; (2) propagate it through value-typed struct params into the callee
  param symbol. Repro: `gap_arena_escape_via_struct_copy.zer`.

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

## Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.

When a `tests/zer_gaps/` reproducer is fixed, move it to
`tests/zer_fail/` so it becomes a permanent regression guard.
