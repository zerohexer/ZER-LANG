# Codebase Audit — 2026-05-24

Comprehensive audit of ZER-LANG compiler with focus on **silent gaps**:
bugs where the compiler accepts unsafe code without warning AND runtime
fails to catch the result.

Auditor: ran 4 parallel deep-dive agents over zercheck_ir.c, ir_lower.c,
checker.c, emitter.c, then verified each claim against the actual code +
reproducer tests + emitted C.

## Severity legend
- **CRITICAL** — silent miscompile in trivial code; baremetal won't catch it
- **HIGH** — silent miscompile in idiomatic code, conditional on context
- **MEDIUM** — silent compile-time discipline gap; runtime trap still fires
- **LOW** — technical debt that turns into a regression class if not addressed

---

## CRITICAL — Bug #1: `global = optional orelse <X>` silently produces no store — **FIXED 2026-05-24**

**Fix landed:** `ir_lower.c:1788-1808` — for IDENT target with `dest_local < 0` (global), now falls through to the synthesized-tmp + new_assign path. Regression test: `tests/zer/audit2026-05-24_global_orelse_literal_FIXED.zer` (4 fallback shapes covered).

Pattern: assigning the result of an `orelse` expression directly to a
**global** identifier produces NO assignment in the emitted C. The IR
shows `bb_ok` and `bb_fail` as empty blocks (only gotos). Every form
fails:
- `g = maybe() orelse 0`   (literal fallback)
- `g = maybe() orelse fn()` (function-call fallback)
- `g = maybe() orelse { return; }` (block fallback)
- `g = maybe() orelse return` (terminating fallback) — **even when the
  Some path is taken, the value is lost**

**Root cause:** `ir_lower.c:1793-1797`. When target is `NODE_IDENT`,
calls `ir_find_local(ctx->func, name, len)`. For a global, this returns
`-1`. Then `lower_orelse_to_dest(ctx, -1, orelse, line)`. Inside
`lower_orelse_to_dest`, both branches (lines 1539 + 1590) are gated on
`if (dest_local >= 0)` — both silently skip the assignment.

**Workarounds (not fixes):**
- Local intermediary: `u32 tmp = ... orelse 0; g = tmp;`
- Struct-field target: `s.x = ... orelse 0` works (field path synthesizes a tmp)
- Pointer-deref target: `*p = ... orelse 0` works

**Fix:** At line 1793-1797, when `dest_local < 0`, fall through to
the "Non-local target — decompose into tmp + synthesized assign" path
at line 1798-1830 (already handles this case correctly for
field/index/pointer-deref targets).

**Baremetal impact:** Completely silent. No segfault, no trap, just
wrong value in global. Catastrophic for boot code initializing global
state from optional sources.

---

## CRITICAL — Bug #2: `defer cleanup(expr orelse value)` silently emits `/* unhandled */0`

**Reproducer:** `audit2026-05-24_defer_orelse_value.zer`

Pattern: any `defer` body containing a NODE_ORELSE expression. The IR
stores `node->defer.body` verbatim into `IR_DEFER_PUSH`. At IR_DEFER_FIRE,
the emitter walks the stored AST through `emit_rewritten_node`, which
has no `case NODE_ORELSE`. Falls through to `default:` at emitter.c:8090
which emits `/* unhandled node 47 */0`. Compiles clean; runtime gets `0`.

**Reproducer output (verified):** `cleanup(/* unhandled node 47 */0);`
in emitted C where the orelse expression should be.

**Root cause:** `ir_lower.c` NODE_DEFER handler does not call
`pre_lower_orelse` on the body. `pre_lower_orelse` only descends into
expression nodes (NODE_BINARY, NODE_UNARY, NODE_CALL, NODE_FIELD, ...).
Statement-shaped children of NODE_DEFER are not visited.

**Fix:** In NODE_DEFER lowering, walk the body via a statement-aware
pre_lower_orelse that descends into NODE_EXPR_STMT, NODE_VAR_DECL.init,
NODE_RETURN.value, NODE_IF.cond, etc.

---

## HIGH — Bug #3: `spawn worker(expr orelse value)` silently emits `/* unhandled */0`

**Reproducer:** `audit2026-05-24_spawn_arg_orelse.zer`

Same root cause class as Bug #2. spawn args are stored verbatim and
emitted at the spawn site through `emit_rewritten_node` (emitter.c
near 8819). Any NODE_ORELSE in a spawn arg silently becomes `0`.

**Emitted C (verified):**
```c
struct _zer_spawn_args_0 *_sa = malloc(...);
_sa->a0 = /* unhandled node 47 */0;   // SHOULD BE: get() or 0
pthread_create(...);
```

**Fix:** `pre_lower_orelse` must descend into `NODE_SPAWN.spawn_stmt.args[]`.

---

## HIGH — Bug #4: `@critical` on x86/Linux emits memory fence, NOT cli/sti

**Reproducer:** `audit2026-05-24_critical_x86_no_cli.zer`

**Site:** `emitter.c:8655-8659` (BEGIN) and 8677-8681 (END), the `#else`
branch (catches `__x86_64__`/`__i386__`).

On Linux userspace, fence is the best you can do — interrupts cannot
be disabled from user mode. But the `#else` is universal: baremetal
kernel mode x86 ALSO gets the fence instead of `cli`/`sti`. An
interrupt firing inside the block silently races with the @critical
body. The compiled code is "valid" — gcc emits a real mfence — but
the semantic is wrong.

**Fix recommendation:**
- Add `__x86_64__` / `__i386__` branch with `pushfq; cli` / `popfq`
- OR introduce `--target-mode=kernel` flag

---

## MEDIUM — Bug #5: C-style cast bypasses bool↔int ban

**Reproducer:** `audit2026-05-24_bool_int_cast.zer`

ZER spec: "bool is NOT an integer — no bool↔int coercion". Implicit
coercion is correctly banned. But `(u32)bool_var` and `(bool)int_value`
via C-style cast compile clean.

**Site:** `checker.c:5499-5508` — typecast path accepts `valid = true`
for bool↔int conversions without applying the implicit-coercion bool
guard.

**Impact:** `(bool)42` produces a "boolean" containing non-{0,1} value.
Exhaustive `switch (b) { .true => ...; .false => ...; }` reasoning is
silently defeated. GCC emits no-op (since bool is i8) — the value 42
sits in a bool. Switch default arm or undefined behavior.

---

## MEDIUM — Bug #6: Compound `/=` / `%=` div-by-zero only catches NODE_IDENT divisor

**Reproducer:** `audit2026-05-24_compound_div_field.zer`

**Sites:** Binary form `x = a / b` at checker.c:2593-2617 covers
NODE_IDENT, NODE_FIELD, NODE_CALL. Compound form `x /= b` at
checker.c:3768-3798 has only the NODE_IDENT branch. Field divisor
(`x /= c.d`) and call divisor (`x /= zero_call()`) silently pass.

**Mitigation:** Runtime `_zer_trap` still fires (division by 0 traps).
So this is not memory corruption — but compile-time fail-fast discipline
is lost. Asymmetric to BUG-612 (which DID fix the compound-shift path).

---

## MEDIUM — Bug #7: scoped `spawn` with aliased pointers to same global

**Reproducer:** `audit2026-05-24_spawn_alias_args.zer`

Pattern: `spawn worker(&g, &g)` — both args point to the same global.
ZER bans non-shared pointer to spawn, but doesn't check whether the
two pointer args alias. Scoped spawn (ThreadHandle + join) silently
allows this.

**Site:** checker.c:10454-10493 validates each arg individually but
never compares argument identities.

**Impact:** Callee may assume parameter independence (no aliasing).
With shared-struct args, the auto-lock fires per access, but the function
signature implies `a` and `b` are independent state. With non-shared
scoped spawn, the main thread could also still access &g between spawn
and join — data race.

---

## LOW — Bug #8: TRANSFERRED state never propagates to aliases via alloc_id

**From zercheck_ir audit.**

`ir_propagate_alias_state` is called at 5 sites in zercheck_ir.c — all
with `IR_HS_FREED` (or `new_state` that's never TRANSFERRED). Sites
that set TRANSFERRED but skip alias propagation:
- `zercheck_ir.c:1502` — IR_NOP/NODE_SPAWN arg transfer
- `zercheck_ir.c:1542` — IR_COPY move-struct transfer (gives dst a
  fresh alloc_id — moot for this struct, but FIELD_WRITE doesn't)
- `zercheck_ir.c:2828` — IR_SPAWN arg transfer
- `zercheck_ir.c:2893` — IR_FIELD_WRITE move-struct transfer

**Real-world severity uncertain.** Move struct COPY gives dst a fresh
alloc_id, so most move-struct alias chains don't exist. The path that
matters: passing a `*MoveStruct` through a wrapper and then transferring.
Tests in tests/zer_fail/ cover the bare cases.

**Fix:** Add `ir_propagate_alias_state(ps, h, IR_HS_TRANSFERRED, line)`
at each TRANSFERRED setter.

---

## LOW — Bug #9: Dead-stub `default` cases in emit_rewritten_node

**From emitter audit.**

`emit_rewritten_node` falls back to `/* unhandled node %d */0` for any
NODE_KIND not enumerated (emitter.c:8090). Currently exposed by Bugs
#2 / #3 above (NODE_ORELSE), but ANY new NODE_KIND added to ast.h
without an emit case becomes a silent miscompile.

**Fix:** Convert the fallback to `_zer_trap("emit_rewritten_node: unhandled NODE_X")`
so it aborts at runtime instead of silently emitting `0`. Alternatively,
make the switch exhaustive (GCC `-Wswitch` catches new kinds).

Same pattern at `emitter.c:9677-9688` for unused IR opcodes
(IR_INDEX_WRITE, IR_DEREF_READ, IR_ADDR_OF, ...): all emit
`/* 3AC op N — TODO */`.

---

## Summary table

| # | Severity | Symptom | File:Line | Status |
|---|---|---|---|---|
| 1 | CRITICAL | `g = X orelse Y` global drops the STORE entirely | ir_lower.c:1793-1797 | FIXED |
| 2 | CRITICAL | `defer f(X orelse Y)` body emits `/* unhandled */0` | ir_lower.c NODE_DEFER + emitter.c:8090 | OPEN |
| 3 | HIGH | `spawn f(X orelse Y)` arg emits `/* unhandled */0` | ir_lower.c NODE_SPAWN + emitter.c:8090 | OPEN |
| 4 | HIGH | `@critical` on x86 = fence only, no cli/sti | emitter.c:8655-8681 | OPEN |
| 5 | MEDIUM | C-style cast `(u32)bool` / `(bool)int` allowed | checker.c:5499-5508 | OPEN |
| 6 | MEDIUM | Compound `/=` `%=` with field/call divisor silent | checker.c:3768-3798 | OPEN |
| 7 | MEDIUM | `spawn(&g, &g)` aliased args not flagged | checker.c:10454-10493 | OPEN |
| 8 | LOW | TRANSFERRED state doesn't propagate to aliases | zercheck_ir.c (5 sites) | OPEN |
| 9 | LOW | Dead-stub `default:` cases silently emit `0` | emitter.c:8090, 9677-9688 | OPEN |
