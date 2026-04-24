# ZER Compiler — Known Limitations

Living document of known compiler limitations, audit findings, and deferred fixes.
Entries removed once fixed.

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

## ~~BUG-607 (lexer integer literal overflow silent wrap)~~ (FIXED 2026-04-24)

Integer literals exceeding `UINT64_MAX` silently wrapped due to unchecked
`val *= base` in `parser.c:parse_primary`. Now emit `error: integer
literal exceeds u64 range`. Covers hex (`0x10000000000000000`), decimal
(`18446744073709551616`), and long hex (`0xFFFFFFFFFFFFFFFF_FFFF`).
Regression tests: `tests/zer_fail/int_literal_overflow_{hex,dec}.zer`.

## ~~BUG-608 (defer body nested control-flow scan)~~ (FIXED 2026-04-24)

`defer_scan_all_frees` (zercheck.c) and `ir_defer_scan_frees`
(zercheck_ir.c) only recursed into `NODE_BLOCK`. `defer { if (err) {
free(h); } else { free(h); } }` was not detected as freeing the handle
→ false leak error. Both scanners now recurse into
if/for/while/do-while/switch/critical/once bodies. Regression test:
`tests/zer/defer_nested_if_free.zer`.

## ~~BUG-609 / Gap 6 — goto into if-unwrap/switch-capture arm~~ (FIXED 2026-04-24)

`goto` targeting a label inside a capture arm from outside the arm
compiled clean. Capture variable was auto-zero'd → silent wrong return.
`collect_labels` / `validate_gotos` in `checker.c` now carry an
`ArmStack` (stack-first dynamic array of capture-arm Node IDs).
Goto-vs-label arm paths must share a prefix. Error:
`goto 'X' jumps into if-unwrap/switch-capture arm without binding
the capture`. Moved reproducer from `tests/zer_gaps/` to
`tests/zer_fail/goto_into_capture_arm.zer` (+ switch variant).
Positive test in `tests/zer/goto_inside_capture_arm_ok.zer`.

## ~~BUG-593~~ (FIXED 2026-04-18)

Comptime float functions now dispatch to `eval_comptime_float_expr`
directly when return type is `f32`/`f64`. The integer `eval_comptime_block`
path was doing integer arithmetic on raw double bit-patterns and
returning garbage instead of `CONST_EVAL_FAIL`, short-circuiting past
the float path.

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

| # | Short name | Gap | Runtime fallback | Fix est. |
|---|---|---|---|---|
| 1 | **Cross-block backward goto UAF** | `free(h); goto LABEL; ... LABEL: ... use(h)` — zercheck linear-walk can't track cycles | Slab gen counter traps at runtime (exit 133 / SIGTRAP) | ~300 lines — CFG-based zercheck rewrite |
| 2 | **Same-line UAF suppressed** | `h.free(p); u32 x = p.x;` on same line — `h->free_line < cur_line` check filters | Slab gen counter traps | ~30 lines — replace line-compare with statement-counter |
| 3 | **`yield` outside async silently stripped** | `void go() { yield; }` compiles; emits no-op | None (silent behavior) | ~5 lines — check `!in_async` in NODE_YIELD |
| 4 | **async + shared struct across yield** | `async { s.v = 1; yield; s.v = 2; }` on a `shared struct` — lock held across yield = deadlock per spec, not enforced | Potential deadlock at runtime | medium — AST walker for shared access in async fn |
| 5 | **`container<move struct>` loses move semantics** | `Box(Tok) b; b.item = t; consume(b.item); b.item.k` — move-transfer through container field not tracked | None (use after transfer) | medium — move-tracking through field writes |
| 6 | ~~**`goto` into if-unwrap capture scope**~~ | FIXED 2026-04-24 (BUG-609) — see above | — | — |
| 7 | **`defer` nested in `defer` body** | `defer { defer { ... } }` accepted per spec says banned | Runs inner defer at outer defer fire time | ~5 lines — check `defer_depth > 0` in NODE_DEFER |

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

## Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.

When a `tests/zer_gaps/` reproducer is fixed, move it to
`tests/zer_fail/` so it becomes a permanent regression guard.
