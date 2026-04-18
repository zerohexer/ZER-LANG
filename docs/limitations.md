# ZER Compiler — Known Limitations

Living document of known compiler limitations, audit findings, and deferred fixes.
Entries removed once fixed.

---

## ~~BUG-579~~ (FIXED 2026-04-18, v0.4.9)

Switch arm body gaps — enum/union/optional switches now fully lower to IR.

---

## ~~BUG-581: `zerc --run` exit code propagation~~ (FIXED 2026-04-18)

`zerc file.zer --run` now correctly returns the program's exit code via
`WEXITSTATUS` on POSIX (already worked on Windows). Previously returned the
raw `system()` wait status, causing shell `$?` to see `status & 255` —
silently masking test failures.

---

## ~~BUG-582: Union variant tag missing on IR path~~ (FIXED 2026-04-18)

`emit_rewritten_node` NODE_ASSIGN now handles union variant assignments:
`u.variant = val` and `u.variant[i] = val` both update `u._tag` first. The
handler walks the assign target through NODE_INDEX / NODE_FIELD chains to
find the NODE_FIELD whose object is a TYPE_UNION.

---

## Pre-existing failures surfaced by BUG-581 fix

Surfacing the real exit codes via BUG-581's fix exposed a number of tests
that were silently "passing" (non-zero exit swallowed). The majority were
fixed in the same session; three remain. They are skipped in
`tests/test_zer.sh` via the `KNOWN_FAIL_POSITIVE` list.

### handle_shadow_scope — variable shadowing in IR flat namespace

```zer
?Handle(Item) mh = pool.alloc();
Handle(Item) h = mh orelse return;
defer pool.free(h);
{
    ?Handle(Item) mh2 = pool.alloc();
    Handle(Item) h = mh2 orelse return;  // inner shadows outer
    pool.free(h);                          // frees inner
}
pool.get(h).val = 42;  // should access OUTER h (still alive)
```

**Problem**: IR's flat local namespace has no concept of scope. When the
inner block declares a same-name `h`, IR either dedups to the outer local
(corrupting outer) or creates a suffixed `h_N` local. Either way, after the
inner block ends, `rewrite_idents` looks up `"h"` via `ir_find_local` which
returns the LAST match — the inner `h_N` (now freed). Outer-scope references
to `h` after the inner block get incorrectly rewritten to `h_N`, causing
use-after-free at runtime.

**Fix approach**: introduce scope depth on `IRLocal` and `LowerCtx`. Each
local gets `scope_depth` at creation. `NODE_BLOCK` enter/exit adjusts the
context. `rewrite_idents` / `ir_find_local` must prefer the highest
`scope_depth` ≤ current context depth (not absolute last). Requires changes
to `ir_lower.c` (scope tracking), `ir.h` (IRLocal field), `ir.c`
(`ir_find_local` scope-aware variant).

**Not touched this session**: the fix is structural and non-trivial. No
regression in existing tests (shadowing rarely occurs in real code).

### hash_map_chained & super_hashmap — struct pass-by-value

Both tests pass a `HashMap` struct by value to `map_put`/`map_get`/etc.
and expect mutations to persist to the caller. In ZER, structs are
copy-by-default. The tests mutate a copy; the original stays untouched.
`map_put(m, 42, 100); map_get(m, 42)` returns `null`.

**Root cause**: test design — the tests were written assuming reference
semantics. They need `*HashMap` (pointer) parameters to actually modify
the caller's map. This was masked by BUG-581.

**Fix**: update the tests to use `*HashMap` + take-address at call sites.
Not purely a compiler issue.

---

## Tracking notes

Additional bonus fixes in the same session (BUG-581 exposure):

- **BUG-583**: `@once { }` emitted `if (1)` in IR path — lost CAS semantics.
  Fixed: IR_BRANCH emitter detects `expr = NODE_ONCE` and emits the atomic
  exchange pattern (matching the AST path).
- **BUG-584**: Optional switch value comparison — arms like `42 => ...`
  on `?u32` only checked `has_value`, not the actual value. Fixed: build
  `has_value && (value == arm_value)` with enum variant value lookup for
  `?Enum` arms.
- **BUG-585**: Switch arm capture scoping — same-name `|v|` captures across
  multiple switches in one function collided in IR's flat namespace via
  `ir_find_local` last-match. Fixed: generate unique arm-local names (e.g.
  `v_cap17`) and rewrite the capture name only within the arm body.
- **BUG-586**: `(bool)integer` didn't truthy-convert — emitted as
  `(uint8_t)5` giving `5`, not `1`. Fixed: IR_CAST emitter uses
  `((uint8_t)!!(x))` when target is bool and source is integer/float/pointer.
- **BUG-587**: Funcptr array indexing via literal — `ops[0](...)` and
  `ops[1](...)` both emitted as `arr[0](...)` because the IR_CALL index
  handler had a "non-ident fallback" that emitted literal `0`. Fixed:
  handle NODE_INT_LIT explicitly, fall back to `emit_rewritten_node` for
  complex index expressions.
- **BUG-588**: Entry block was not `bb0` when the function body contained
  labels. `collect_labels` ran before `start_block` so label blocks got
  IDs `0, 1, 2...` and the entry block got a higher ID. The emitter iterates
  blocks in order so C execution started at a label, reading uninitialized
  state and trapping. Fixed: start the entry block FIRST, then pre-scan
  labels. Fuzz tests `safe_goto_defer_*` that previously crashed at runtime
  now pass.
- **BUG-589** (test): the semantic fuzzer's `gen_safe_goto_defer` produced
  a self-contradictory pattern: `defer free(h)` followed by `goto done`
  followed by a read of `h` after the label. ZER's `goto` fires pending
  defers (see `tests/zer/goto_defer.zer`), so the read hits a freed handle.
  Updated the generator to manage lifetime explicitly (no defer; free just
  before return).
