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
| ~~3~~ | ~~**`yield` outside async silently stripped**~~ | FIXED 2026-04-22 — `check_stmt` rejects `yield`/`await` when `!c->in_async`. Regression: `tests/zer_fail/{yield,await}_outside_async.zer` | — | — |
| 4 | **async + shared struct across yield** | `async { s.v = 1; yield; s.v = 2; }` on a `shared struct` — lock held across yield = deadlock per spec, not enforced | Potential deadlock at runtime | medium — AST walker for shared access in async fn |
| 5 | **`container<move struct>` loses move semantics** | `Box(Tok) b; b.item = t; consume(b.item); b.item.k` — move-transfer through container field not tracked | None (use after transfer) | medium — move-tracking through field writes |
| 6 | **`goto` into if-unwrap capture scope** | `goto inside; if (m) \|v\| { inside: return v; }` — captured `v` uninitialized at goto target | Auto-zero (returns 0 silently) | medium — label reachability analysis |
| ~~7~~ | ~~**`defer` nested in `defer` body**~~ | FIXED 2026-04-22 — `check_stmt` rejects `defer` when `c->defer_depth > 0`. Regression: `tests/zer_fail/defer_nested_in_defer.zer` | — | — |

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

## Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.

When a `tests/zer_gaps/` reproducer is fixed, move it to
`tests/zer_fail/` so it becomes a permanent regression guard.
