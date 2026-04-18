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

---

## Remaining known failures (all pre-existing, surfaced by BUG-581)

### 3 rust_tests skipped

- `rt_unsafe_mmio_multi_reg`, `rt_unsafe_mmio_volatile_rw` — access real
  hardware addresses (`0x40020000...`). On a hosted Linux test runner
  these cause SIGSEGV/SIGTRAP. Need a QEMU-like environment or mocked
  MMIO to actually run. Not a compiler bug.

- `gen_comptime_float_001` — comptime `f64` function calls inside binary
  expressions (`SQUARE(5.0)` where SQUARE returns `f64`) resolve to 0
  instead of 25.0. The checker doesn't set `is_comptime_float` +
  `comptime_float_value` for this call pattern. Needs investigation in
  `eval_const_expr_subst` / `find_comptime_return_expr` float paths in
  checker.c. Existing simpler float tests (constant folding inside
  `comptime f64 PI() { return 3.14; }` at top level) still work — this
  is specifically about nested binary expressions with comptime float
  calls.

### 1 zig_tests skipped

- `zt_comptime_float_const` — same underlying issue as
  `gen_comptime_float_001`. Will be fixed together.

---

## Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.
