# ZER Compiler — Known Limitations

Living document of known compiler limitations, audit findings, and deferred fixes.
Entries removed once fixed.

---

## ~~BUG-579 series — Switch arm body emission gaps~~ (FIXED 2026-04-18, v0.4.9)

**Resolution**: Enum/union/optional switches now fully lower to IR (the same path
as integer switches). Arm bodies go through `lower_stmt`, which handles every
statement kind correctly. See `ir_lower.c` NODE_SWITCH handler and BUGS-FIXED.md
for details.

---

## `zerc --run` exit code propagation

`zerc file.zer --run` returns `system()`'s raw wait status, not the compiled
program's exit code. On POSIX this means shell `$?` gets `status & 255`
(not `WEXITSTATUS(status)`), so a program that exits 3 shows as exit 0 when
invoked via `--run`.

**Impact**: `tests/test_zer.sh` runs positive tests via `--run` and checks
`[ $ret -eq 0 ]`. A compiled program that silently returns the wrong value can
be reported as PASS. Several tests (union_array_variant.zer being the known
example) currently mask a latent issue in union variant tag emission on the
IR path because `--run` returns 0 regardless.

**Workaround**: compile without `--run`, then execute the output directly:
```
./zerc file.zer           # builds file.exe (or file on Unix)
./file                    # real exit code here
```

**Fix**: change `zerc_main.c` line 687 from `return run_ret;` to
`return WEXITSTATUS(run_ret);` (with `#include <sys/wait.h>` for Unix). Need
the equivalent macro on Windows. Verify all test runners still work after the
fix — negative programs that trap (SIGTRAP = exit 133) may now fail in places
where they previously reported 0.

---

## Union variant assignment missing tag update on IR path

`d.quad[0] = 10` on a union does NOT update `d._tag` on the IR path. The AST
path's `emit_expr` NODE_ASSIGN (`emitter.c:1210`) has union-variant detection
that wraps the assignment in `_tag = N; field = value`. `emit_rewritten_node`
NODE_ASSIGN (`emitter.c:6657+`) is missing this handling, so the tag stays at
zero-init value.

**Impact**: Switches on unions that were set via field assignment take the
wrong arm (usually default). `tests/zer/union_array_variant.zer` exposes
this, but is masked by the `--run` exit code bug above.

**Root cause**: When IR lowering was added, the NODE_ASSIGN union-variant
special case in emit_expr was not ported to emit_rewritten_node.

**Fix**: port the union-variant assignment handling from `emit_expr`
(`emitter.c:1212-1239`) to `emit_rewritten_node`
(`emitter.c:6657` NODE_ASSIGN case). Detect `target->kind == NODE_FIELD` on a
TYPE_UNION object, look up the variant index, emit
`({ __typeof__(obj) *_p = &(obj); _p->_tag = N; _p->variant = value; })`.
