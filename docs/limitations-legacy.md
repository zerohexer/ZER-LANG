# ZER Compiler — Legacy limitation entries (unverified)

**Every entry here is a HYPOTHESIS.** These sections were open or informational when they
were written (all before 2026-09-11) and have not been re-measured since; the compiler has
changed a great deal underneath them. Measured over a sample in 2026-08, four of six such
entries were already closed and one did not reproduce. Several headings below say outright
that their items are closed — they are here, not in the closed archive, because the
2026-11-13 split was mechanical (by heading and date) and they still contain "remaining"
notes nobody has checked.

**Do not implement from an entry here.** Reproduce it on the current compiler first
(`zerc f.zer -o out.c`, read the diagnostic). Then, in the same commit:
- still true → write it as an entry in `docs/limitations.md` with the reproducer, and
  delete it here;
- gone → move it to `docs/limitations-closed.md` with the measurement, and delete it here.

This file should only shrink. Nothing new is ever added to it.

Also kept here: planning notes that were filed in the old limitations file (the max-oracle
gap audit, the oracle roadmaps, the asm Option E plan) — they are plans, not defects.

---



## OPEN — BRANCH `loving-davinci-ii7a90` (2026-09-06): ALL 12 ITEMS CLOSED; 2 refactors + 2 relaxations REMAIN

**STATUS 2026-09-06: items A–K AND both relaxations N / O are CLOSED** (BUG-934..949).
**Only the two architectural refactors L and M remain.**

Four findings were made while closing them that this branch never reported, each with
its own OPEN entry below: a `shared struct` read in an asm operand takes NO LOCK; a
designated initializer does not work at global scope for any field type; BUG-936 left
three defer-body auto-guard siblings (**owned by L, NOT M — corrected 2026-09-06;
see the ORDER section**); and BUG-949, a
use-after-free in the shared-types cache (fixed). **N also proved that the defer half
of the emitter is the blocker it is described as**: the checker relaxation was
correct, but `emit_defer_stmt` had no break/continue arm and TRAPPED, so the emitter
arms had to ship with it — accepting a program the emitter cannot produce is strictly
worse than the over-rejection. A ZER `switch` in a defer body still traps; that one
waits for **L**.

**Read this section ALONE and you can start. Every line below was MEASURED against
main at `9a0e731b` in CHECKER-ONLY mode (`-o out.c`), not read from the branch.**

```
fork point   c8e58304   (the SAME base as all nine other survey branches)
extract      git show origin/claude/loving-davinci-ii7a90:<path>
BUG NUMBERS  its BUG-913..924 COLLIDE with loving-davinci-r3an9y's BUG-913..924
             and with the vigilant-tesla set. RENUMBER ON ADOPTION.
```

**Measured: 55 test files. 18 already rejected by main (skip). 37 accepted — of
those 6 are `zer_trap` (correct: tracked at runtime, not holes) and ~17 are
`tests/zer` positives (correct). THE LIVE HOLES ARE 14, plus 4 non-test items
verified by hand.**

### TAKE FIRST — it extends a class already closed here

**A. ~~The SWITCH ARM is a NINTH value-flow sink.~~ — CLOSED 2026-09-06 as BUG-934, DO NOT REDO.**
All four reject; boundary pinned by `tests/zer/switch_arm_flow_ok.zer`. Original: 4 tests: `switch_arm_literal_oob`,
`_bool_on_int`, `_float_on_int`, `_negative_into_unsigned`. CLAUDE.md's value-flow
row lists EIGHT sinks; the switch arm is not one of them, so every rule that rides
`value_flows_to` — including BUG-927's enum door and BUG-842's negative constant —
is absent there. Measured, all four accepted on main:

    u32 main(){ u8 b = 44;  switch (b) { 300  => {…} default => {} } return 0; }
    u32 main(){ u32 x = 4;  switch (x) { -5   => {…} default => {} } return 0; }
    u32 main(){ u32 x = 4;  switch (x) { true => {…} default => {} } return 0; }
    u32 main(){ u32 x = 4;  switch (x) { 1.5  => {…} default => {} } return 0; }

while the var-decl spelling of the same value (`u8 b = 300;`, `u32 x = -5;`) is
correctly rejected. ONE site, and it closes four tests. Cheapest item in the survey.

### ACCEPT-UNSAFE / MISCOMPILE — all verified live

**B. ~~BUG-917 — the 16-root shared-lock cap drops locks SILENTLY.~~ — CLOSED 2026-09-06 as BUG-935, DO NOT REDO.** *(SharedRootVec, stack-first 16 + heap doubling; the required-emission gate taken too and verified RED pre-fix.)* Original: *(highest
severity in this branch)* The B1 multi-root collector used a fixed 16-slot array
and returned early at the cap. MEASURED on main with 18 roots of ONE `shared(rw)`
type in one statement: **0 diagnostics, 15 `rdlock` calls emitted, `s17`/`s18`
read with NO lock at all.** A silent data race in emitted code. (Probe with 18
DISTINCT shared types instead and the deadlock rule masks it — use one type.)
Their fix: `SharedRootVec` (16 inline + heap doubling) replacing three capped
arrays, and `tools/emit_audit.sh` gains a REQUIRED-fingerprint section asserting
the 17th/18th `rdlock` — a gate for emission that must be PRESENT, which we do not
have. **Take the gate idea as well as the fix.**

**C. ~~BUG-913 — stale VRP in a defer body → an unguarded out-of-bounds store.~~ —
CLOSED 2026-09-06 as BUG-936, PARTIALLY: DO NOT REDO the widening, DO still take M.**
*The widening (ranges live at registration → TOP for the body) is in and closes
`defer_vrp_stale_index` + `_after_loop` (both now trap 133, both verified exit 0
pre-fix). It does NOT close the three siblings — an unprovable index in a defer-body
VAR-DECL INIT, FOR-INIT or WHILE-COND still exits 0, because the auto-guard is emitted
only before an op-kind-GATED subset of instructions and those positions are not in the
gate. **CORRECTED 2026-09-06: those three need refactor L, NOT M** — measured,
`emit_defer_stmt` calls `emit_auto_guards` in only 3 of its 10 arms (EXPR_STMT, RETURN,
IF-cond) and not in WHILE-cond, FOR-init or VAR_DECL-init, which are exactly the three.
They live in the raw-AST defer emitter that L deletes; M's IR op-kind gate never sees a
defer body.* Original:
A defer body is range-checked at the `defer` STATEMENT but RUNS at scope exit,
after every intervening reassignment:

    u32 main(){ u32[4] arr; u32 i = 0; defer { arr[i] = 7; } i = 100; return 0; }

MEASURED: 0 diagnostics, 0 warnings, and the emitted C contains a bare
`arr[i] = 7;` with `i = 100` above it and NO bounds check. Their fix widens every
range live at registration to TOP for the body (keeping `address_taken`; in-body
guards still narrow) and relies on the existing B4 snapshot to restore the
pre-defer ranges afterwards. Five `zer_trap` siblings cover the var-decl-init,
for-init and while-cond positions.

**D. ~~BUG-914 — Level B keeps ONE free site, so a SECOND disjoint free is unguarded.~~ — CLOSED 2026-09-06 as BUG-937, DO NOT REDO.** *(IR_FREE_BLOCK_MULTI saturation at the tag, the merge JOIN, and the alias group. All 3 branch negatives STATED, all 12 existing guarded_* negatives unchanged, Level B's two recoveries pinned by tests/zer/guarded_level_b_ok.zer. Cost: a disjoint use with 3+ complementary frees now over-rejects.)* Original:
Directly a hole in the guarded-refinement relaxation:

    if (c) { free(h); }
    if (!c) { if (d) { free(h); } }
    if (!c) { if (d) { g = h.v; } }      // accepted — the exact path of the 2nd free

`free_block` held the FIRST site, so a use under `!c && d` was judged disjoint from
it. Their fix makes the slot a JOIN over free sites saturating to
`IR_FREE_BLOCK_MULTI`, with one sink `ir_note_free_site` at the three free sites,
the merge joining sites for FREED and MAYBE_FREED, and alias propagation
saturating. They record the precision cost (a third complementary free) as a
known over-rejection. Related tests: `guarded_multi_free_alias_uaf`,
`guarded_freed_both_arms_then_use`.

**E. ~~BUG-915 — `&freed.field` was skipped as a capture.~~ — CLOSED 2026-09-06 as BUG-938, DO NOT REDO.** *(`ir_addr_of_deref_target`: the skip stays for VALUE navigation and lifts exactly when a step auto-derefs. **Also closed the sibling the branch missed** — `ir_check_expr_wrong_pool` had the identical skip, so `wr(&pb.get(h).id)` escaped the wrong-pool rule; found by enumerating sibling sites.)* Original: `free(h); wr(&h.v);`
compiles clean on main. The UAF walker skipped EVERY `&` operand. Forming `&h.v`
dereferences a freed pointer to produce an interior pointer into the freed
allocation. NOTE the interaction with our own rule *"FORMING a reference aliases;
READING a value does not"* — the fix must check the auto-deref CROSSINGS on the
operand chain, not treat `&` as opaque. Sibling: `uaf_addr_of_index_through_freed_slice`.

**F. ~~BUG-922 — a MISCOMPILE, and it is the TWO-SPELLINGS class again.~~ —
CLOSED 2026-09-06 as BUG-939, PARTIALLY.** *Retype added at the four missing sinks
(one call ahead of the assign op-chain covers plain AND compound), a shared
`emit_int_literal` keyed on the resolved type for BOTH AST emitters, and the
file-scope fold un-gated from `is_const` (a non-const global emitted `_zer_shl`, a
GCC statement expression, which is **illegal at file scope** — a valid ZER program
that could not be BUILT).*
**STILL OPEN — the BINARY OPERAND position.** `v != (1 << 40)` still folds at the
default width, because `is_literal_compatible` accepts only a lone `NODE_INT_LIT`
and never a pure-literal TREE, so the binary retype at checker.c never fires for
one. **That is the same predicate as item K (BUG-921), so the two must be done
together** — widening `is_literal_compatible` to a pure-literal tree whose
WIDTH-EXACT fold fits closes both the over-rejection AND this residual. The branch's
caveat applies: every literal AND intermediate must fit, because `/`, `%` and shifts
do not commute with the wrap. Original:

    i64 v = -(1 << 40);      // var-decl  -> correct
    i64 a;  a = -(1 << 40);  // assign    -> DIFFERENT VALUE

MEASURED: the two spellings **disagree at runtime**. The post-acceptance retype ran
at four of the eight value-flow sinks; assignment, orelse fallback, spawn arg and
global init evaluated the tree in u32, and both AST literal emitters wrote a bare C
`int`. Their fix: one `retype_accepted_const` at all sinks + compound assign +
binary operand, one `emit_int_literal` that casts >32-bit literals, and a
file-scope fold for negative trees. **This is the same shape as BUG-933 — see the
two-spellings row in CLAUDE.md.**

**G. ~~BUG-916 — spawn arguments emitted from the AST by name without
`rewrite_idents`,~~ — CLOSED 2026-09-06 as BUG-941, DO NOT REDO.** *(The non-descent was ALREADY in `walker_field_baseline.txt`, so the audit had been silenced on it; the stale row is removed. `asm` operands are the structurally-excluded sibling — asm is legal only in a `naked` function, which cannot declare a local to shadow.)* Original: so an argument naming a SHADOWED inner local bound to the wrong
variable.

**H. ~~BUG-918 — `orelse` inside a builtin method's args survives lowering.~~ —
CLOSED 2026-09-06 as BUG-942, DO NOT REDO.** *(The survey named ONE site; enumerating
the raw-AST argument positions found **FOUR** — builtin method arg, universal
`free(slice)`, spawn arg, and **asm operand**, the last found only by the enumeration.
All four hoist via the one shared `pre_lower_orelse`. Two further findings: the hoisted
identifier carried NO TYPE, so the emitter's `free(slice)` gate failed and wrote
`free(tmp)` instead of `free((void*)tmp.ptr)` — fixed at the declaration site with
`checker_set_type`; and the **VALUE** fallback was broken too, not just the loud
control-flow one. The emitter's "compiler bug … in a spawn argument" message named
spawn at all four sites and claimed the checker rejected the case — both false, both
corrected.)* Original: MEASURED: `heap.free_ptr(mh orelse return)` produces a FALSE LEAK on main
(`handle 'mh' allocated … but never freed`) because zercheck cannot key the free's
argument; the branch also reports the emitter falling through to a "compiler bug …
in a spawn argument" message plus a runtime trap. Their fix hoists every `orelse`
in a builtin's args to a branch + temp.

**I. ~~BUG-923 — `(u3)x` / `(i48)x` do not PARSE~~ — CLOSED 2026-09-06 as
BUG-945 + BUG-946, DO NOT REDO.** *(Parse: the uN/iN spelling now takes the SAME
speculate-and-backtrack arm as `(*`; corpus cost measured ZERO — all 313
parenthesized int-type spellings in the tree are keyword widths that never reach it.
The width rule moved to `ast.h` as `zer_is_intn_type_name` with `checker.c` a thin
wrapper, so parser and checker ask ONE question. Wrap: measuring first showed the
survey's expectation was wrong — it is not three cast emitters needing a new
`emit_intn_cast_wrap_open/close`, it is ONE site (`IR_CAST`) needing the EXISTING
`emit_intn_mask` that IR_BINOP and IR_UNOP already use. Var-decl / call-arg / return
were wrong (44 instead of 4); plain and global assignment were already right — the
two-spellings split. The test discriminates both halves, verified against a build of
the intermediate state, not assumed.)* Original: (uN/iN are IDENTs, never a cast
start). MEASURED: `error: expected ';' after variable declaration`. Their fix moves
`intn_type_name` to `ast.h` and makes the cast speculative like `(*`. They also
found that once parseable, all three cast emitters kept the carrier's high bits
(`(u3)300 == 44`), needing `emit_intn_cast_wrap_open/close` at all three sites —
i.e. the uN width-wrap multi-site class AGAIN.

**J. ~~BUG-924 — a value-optional global initialised with its bare payload emits
INVALID C.~~ — CLOSED 2026-09-06 as BUG-943, DO NOT REDO.** *(The survey named one
shape; the family is four — `?u32 = 5`, `?bool = true`, `?E = E.b`, `?i64 = -7` all
emitted invalid C, and only `= null` worked because only `= null` had an arm. The
payload is const-folded when it folds — the BUG-939 file-scope statement-expression
rule — and otherwise goes through `emit_opt_wrap_value`, the shared T -> ?T query.
**Measuring it also found BUG-944**, the FIFTH raw-AST argument site: the SUBJECT of an
`orelse` is a raw passthrough, and the emitter's raw-AST argument loop applies NO
coercion while the decomposed path applies four — so `f(5) orelse 0` failed to compile
where `?u32 r = f(5);` built. Fixed by ROUTING an ordinary call through `lower_expr`
rather than duplicating the coercions.)* Original: MEASURED: `?u32 g = 5;` is accepted by the checker and emits
`_zer_opt_u32 g = 5;` → `gcc: error: invalid initializer`. A valid ZER program that
cannot be built.

**K. ~~BUG-921 — OVER-REJECTION: a pure integer-literal EXPRESSION is typed by its
spelling.~~ — CLOSED 2026-09-06 as BUG-940, DO NOT REDO.** *(Ring-homomorphism rule: `+ - * << & | ^` and unary `-` commute with the wrap, `/ % >> ~` do not. Leaf-fits + final-fits. **uN/iN excluded** to honour the `global_uN_arith_narrow` tripwire. Also closes BUG-939's binary-operand residual — same predicate. NOTE the ordering: doing this BEFORE 939 would have been harmful.)* Original: MEASURED: `i8 x = -5 - 1;` and `u16 y = 100 + 27;` are rejected while
the identical VALUE as a lone literal (`i8 x = -6;`) is accepted. Their fix accepts
any pure-literal tree whose WIDTH-EXACT fold fits — every literal AND intermediate
must fit, because `/`, `%` and shifts do not commute with the wrap — and looks
through one optional level.

### TWO REFACTORS — architectural, and one closes a recorded item

**L. Lower DEFER BODIES into the IR at every fire site; delete the raw-AST defer
emitter.** *(CROSS-BRANCH COMPARISON DONE 2026-09-06 — this SUPERSEDES `o51x9p`'s
answer to the same defect; see the corrected CLASS 8. o51x9p BANS the constructs
in +615 lines, ii7a90 SUPPORTS them at net −386, and the Ban Decision Framework
picks ii7a90 because the ban's only possible justification — emission
impossibility — is disproved by ii7a90 emitting them.)* This closes the recorded NON-TEST finding *"`emit_defer_stmt` is the
last raw-AST STATEMENT emitter (`o51x9p`, ARCHITECTURAL)"*. A defer body used to
travel as raw AST, emitted by a SECOND statement emitter and analysed by a SECOND
handle analyzer — so every safety rule had to be re-implemented for defer bodies
and several were not. The branch lists six measured consequences, incl. a
MAYBE_FREED silently promoted to FREED (double free), shared reads in a defer-body
condition taking NO mutex, and `switch`/`do-while`/`@critical` inside a defer body
trapping at runtime on VALID code. Deletes ~733 lines from `zercheck_ir.c` plus the
emitter's whole defer stack. **Large; adopt deliberately, not casually.**

**M. Lower bounds/UAF AUTO-GUARDS into the IR as branches; new `IR_TRAP`
terminator.** The emitter synthesised the guard in C before an op-kind-GATED subset
of instructions, so every op kind missing from that gate was a silent OOB and the
guard was invisible to `zercheck_ir`. Moves it to the single choke point every
instruction passes through. Also fixes an ordering bug: the lock of an indexed
shared root (`arr[i].v = 1`) now guards BEFORE the lock instead of trapping inside
a held lock (133 → clean early return).

### TWO RELAXATIONS — both measured, both with the reason NARROWER than the code

**N. ~~`break`/`continue` targeting a loop nested INSIDE a defer body / `@critical`
/ `@once`~~ — CLOSED 2026-09-06 as BUG-947, DO NOT REDO.** *(`escaping_block_depth`
returns the depth the jump would ESCAPE, so the VST-verified predicates in
`src/safety/context_bans.c` are UNCHANGED — asked a more accurate question, not a
different one. The test is a COMPARISON against the loop depth recorded at the
innermost block's entry, which is what makes the interleaved case
`defer { for(){ @critical { break; } } }` stay rejected. **The relaxation also
exposed an EMISSION gap that had to be closed in the same commit**: `@critical` and
`@once` emitted fine but `emit_defer_stmt` had no break/continue arm and trapped —
accepting a program the emitter cannot produce would be strictly worse than the
over-rejection. A ZER `switch` in a defer body still traps; pre-existing, refactor
**L**.)* Original: leaves only that loop, never the body — the ban keyed on "inside a
defer/@critical/@once" alone. A break of a loop that ENCLOSES the block stays
rejected.

**O. ~~The same-statement deadlock rule merged a callee's TRANSITIVE shared types
into every calling statement~~ — CLOSED 2026-09-06 as BUG-948 (+ BUG-949),
DO NOT REDO.** *(Both halves of the model verified in the EMITTED C, not argued:
`u32 r = a.x + helper();` emits lock / read / CALL / unlock, so with a direct access
the callee's locks genuinely nest; `u32 r = helper();` emits no lock in the caller at
all. The positive test is empirical — two threads, opposite orders, 200 iterations.
**The first draft was a HOLE**: gating with `continue` also skipped the RECURSION into
nested bodies, so `do { a.x = b.y; } while (k);` compiled; three existing negatives
caught it. Gate the CHECK, never the walk. **And it shook loose BUG-949**, a
use-after-free: `find_func_shared_cache` returned a pointer INTO a realloc-grown array
while `scan_body_shared_types` held it across a recursive call that can add an entry —
ASan-confirmed on the intermediate build, boundary exactly at the initial capacity
16 -> 17. Fixed by making entries individually allocated so they never move.)* Original:
so `u32 r = f();` was rejected when `f` touches two
shared structs sequentially. A statement with no DIRECT shared access takes no
lock, so nothing can nest around the call. `g.v = f();` stays rejected.

### M — STATUS 2026-09-07: 95% DONE, and the remainder is PRINCIPLED

BUG-952 / 953 / 955 / 956 / 957 / 958 landed. **119 of 124 bounds guards are IR
branches**; five remain as C splices and `emit_auto_guards` therefore still exists.

**Do not try to delete it by hoisting the rest.** The five are declined for reasons:

- `await g_arr[i] != 0` and `while (arr[i] > 0)` are RE-EVALUATED — per poll and per
  iteration. A guard hoisted before the statement is checked once while the index
  changes underneath it, which is sound only by accident. Migrating these means
  emitting into the loop's condition block and the await's poll state, which is a
  different job from hoisting.
- An OPTIONAL or STRUCT return type has no literal zero — `?u32` is `{0,0}` — so the
  exit block cannot build its return value without the emitter's `emit_zero_value`.

**What L needs from M is already true.** L's blocker was that the auto-guard early
return called `emit_defers` — the raw-AST path — from a C exit that did not exist in
the IR. For the 95% that are now IR branches, the exit fires defers through
`emit_defer_fire` like any other IR return. The remaining five still take the emitter's
path, so L's stage 3 (deleting `emit_defer_stmt`) still has to account for them — but
they are now a SHORT, ENUMERATED list rather than every guarded access in the program.

Also recorded: `x = f() orelse arr[i]` hoists the fallback's guard, so an early return
can fire even when `f()` succeeded and the fallback was never evaluated. Pre-existing —
`emit_auto_guards` already descended into `orelse.fallback` before the instruction — and
left alone rather than quietly changed.

### M — THE ARCHITECTURAL HALF: where the guard must be lowered (MEASURED 2026-09-07, do not re-derive)

M's two contained halves are CLOSED — the ordering bug (BUG-952) and the fail-closed
gate (BUG-953). What remains is lowering the guard itself into the IR as a branch, and
a vertical slice of that was BUILT, measured, and reverted. The tree is unchanged; this
is what it established.

**The obvious wiring point does not work.** `lower_expr`'s `NODE_INDEX` arm looks like
the choke point, and it is not: an assignment TARGET never passes through it. Measured
with a trace on `arr[i] = 7` — the arm fired only for the unrelated `arr[0]` READ in
another function, at a different source line, while the guarded access at line 4 never
reached it at all. An `ASSIGN` goes to passthrough carrying its whole AST, so the target
index node is never individually lowered. Assignment targets are where nearly every
auto-guard lives, so wiring there guards almost nothing.

**The guard must therefore be lowered by a STATEMENT-LEVEL AST WALK**, mirroring what
`emit_auto_guards` already does in the emitter — descend the statement's expression tree,
find each index whose `checker_auto_guard_size` is non-zero, and emit the branch before
the statement's instructions. Note this must come before the statement's LOCK, which is
what BUG-952 established.

**Do not simply copy `emit_auto_guards` into `ir_lower.c`.** That would be a second
implementation of "find the guardable indexes", the multi-site shape this codebase keeps
paying for. The descent wants to become ONE walker both sides call — a visitor that
yields index nodes — with the emitter's version emitting C and the lowering version
emitting IR, until the emitter's is deleted.

**What the slice DID prove works:**

- The IR early-exit block is straightforward and STRICTLY BETTER than the C one: fire
  defers, release `current_stmt_shared_root`, `IR_RETURN` a zero literal. Releasing the
  lock is exactly what the C-level guard cannot do, and the reason it degrades to a trap
  inside one ("cannot return without leaking it").
- Using `IR_RETURN` sidesteps the emitter's three special cases for free — the async
  termination sequence, `void main()` promotion, and the defer-body form — because
  `IR_RETURN`'s own emitter already handles them.
- Single evaluation is available: lower the index once and rewrite the AST to name the
  local, the trick the `NODE_INDEX` arm already uses for an `orelse` index.
- A `checker_mark_guard_lowered` marker lets `checker_auto_guard_size` return 0 for a
  node whose guard is now in the IR, so the two paths can coexist during migration
  rather than the flip being all-or-nothing. That is what makes this refactor
  incrementally landable, unlike L's stage 2.
- Restricting the first cut to void/integer/bool return types is safe: anything else
  declines and the emitter's guard still fires. Declining is always safe.

### L — FEASIBILITY, MEASURED 2026-09-06. Read before implementing; the obvious route is the WRONG one.

Design work done, implementation NOT started. Two prerequisites were discovered by
measurement, and they rule out the route anyone would try first.

**A defer body really does reach MORE THAN ONE fire site.** Measured — one `defer` with
an early `return` emits **2 copies** of the body in one function:

```zer
void f(u32 k){ defer { g += 1; } if (k > 0) { return; } g += 10; }
```

So "lower the body at each fire site" means lowering the SAME AST N times.

**PREREQUISITE 1 — lowering MUTATES the AST, so per-site lowering needs a clone.**
`rewrite_idents` renames in place and `pre_lower_orelse` REPLACES each orelse with an
identifier naming a temp. The second lowering therefore finds no orelse and emits no
branch, while the identifier still refers to a temp created in the FIRST fire site's
blocks — a dangling reference, i.e. a miscompile. This is the hazard CLAUDE.md already
records as *"never call ir_lower_func twice on the same AST"*. **No AST cloner exists in
the tree** (`grep` for clone_node / ast_clone / deep_copy / copy_node: nothing).

**PREREQUISITE 2 — the typemap is keyed by the Node POINTER.** `typemap_set` hashes
`(uintptr_t)node >> 3`, so a cloned node has NO type entry and `checker_get_type`
returns NULL for it — which emission depends on everywhere. An AST cloner would have to
copy typemap entries for every node it clones, so it needs the Checker, not just an
Arena.

Together those make the AST-clone route a 53-kind exhaustive walker PLUS typemap
copying — large, and exactly the kind of walker where a dropped child field is silent.

**THE BETTER ROUTE: clone at the IR level, not the AST level.** Measured from `ir.h`:
an `IRInst` refers to other blocks ONLY by integer index (`true_block`, `false_block`,
`goto_block`), locals only by id, and blocks live in a flat `func->blocks[]` array
appended by `ir_add_block`. So duplicating a body means copying a contiguous BLOCK RANGE
and adding an offset to those three fields. Locals are shared — same function, no
remap needed.

That route touches neither the AST nor the typemap, so BOTH prerequisites disappear, and
it is tens of lines instead of a 53-kind walker.

**STAGE 2 LANDED 2026-09-08 (fourth attempt).** Defer bodies are lowered into the IR
and spliced at each fire, for every function that does not contain a LABEL. The three
earlier attempts are kept below because their MEASUREMENTS are the reason this one
worked; the plans they proposed are superseded by the one boundary described here.

**WHAT LANDED — the shape.**

- **A template per defer, lowered at REGISTRATION and EXTRACTED.** `lower_stmt` on the
  body into a fresh range, then copy the blocks aside and truncate `block_count` back,
  leaving the function exactly as it was. Every fire clones the template with
  `ir_append_block_copies` (BUG-950's cloner, generalised to an external source so a
  snapshot survives the live array growing).
  At registration rather than at the first fire, for CORRECTNESS, not tidiness:
  lowering re-resolves identifiers and the first fire can be in a NESTED scope, where
  `pool.free(h)` resolves to an inner shadowed handle. Restoring the scope DEPTH alone
  did not reproduce the registration context — measured, it still resolved to the
  shadow. Lowering where the defer was WRITTEN does, by construction.
- **`defer_tpl_exit` — resume at the template's EXIT block, not its last INDEX.** For a
  `for` loop the exit block is created before the post block, so index-based resumption
  lands inside the loop and leaves the real exit unterminated. Lowering leaves
  `current_block` AT the exit; that is what gets recorded.
- **ONE query decides the path: `defers_stay_on_ast(ctx)` = `label_count > 0`,** and
  `make_defer_fire` is the only constructor of an `IR_DEFER_FIRE`, so the flag cannot
  be forgotten at a new fire site. It was: three sites answered independently and two
  answered "no" by omission, so the bodies existed on NEITHER path and every handle a
  defer freed was reported leaked — 11 goto/defer tests. One query, one constructor.

**WHY `label_count > 0` IS THE BOUNDARY — this supersedes items 1 and 2 of the third
attempt's plan.** A defer body carries two runtime gates, and BOTH exist only in a
function containing a label:

- the **ARMED** flag (a forward `goto` can jump over a registration), and
- the plt86m cleanup-label **GUARD** (a `goto` fires the body eagerly and SETS a flag;
  the later fall-through fire runs it only `if (!flag)`).

As raw AST they were INVISIBLE, so zercheck applied the body's frees unconditionally.
As IR branches they are visible and the merge of {freed} and {alive} is MAYBE_FREED.
The plan was to elide ARMED by DOMINANCE and split GUARD onto the AST path — two
mechanisms. But the guard flag genuinely VARIES (that is its purpose), so dominance
cannot help it, and it is only ever created when a label exists. Declining WHOLESALE for
those functions therefore covers both, with no new analysis: functions without a label —
the overwhelming majority, and where the safety wins are — get the IR treatment;
functions with one keep exactly the behaviour they had, and a materialised body never
carries a gate at all.

**Consequence: `ir_compute_dominators` (BUG-960) had NO consumer and was REMOVED
(2026-09-08, the commit after L).** It was built as the prerequisite for the elision this
boundary makes unnecessary, and a tested CFG utility with zero callers is still dead code
in the compiler core. It was 45 lines plus four tests — the classic iterative bitset
fixpoint over `func->blocks` on top of `ir_compute_preds` — so rebuilding it when a
consumer appears is cheap; keeping it on speculation is not the trade this project makes.
Its history is at `f60a803d` if it is wanted verbatim.

**WHAT IT BUYS — measured, not argued.**

- **ACCEPT-UNSAFE CLOSED: a wrong-pool free inside a defer body.** `*Task t =
  alloc(Task); defer heap.free_ptr(t);` frees an auto-Slab allocation on a DIFFERENT,
  explicitly declared Slab. Rejected everywhere else, silently accepted inside a defer,
  because the body was raw AST the CFG walk never saw. Measured with NO nesting at all,
  so it was never a depth or scan-limit story. Pinned by
  `tests/zer_fail/defer_body_wrong_pool.zer`.
- **A MISSED LEAK CLOSED: a `defer` registered inside a conditional block.** The
  fall-through path allocates and never frees; verified in the EMITTED C that the
  false-branch block returns with no free call. Pinned by
  `tests/zer_fail/defer_in_branch_fallthrough_leak.zer`. It closed as a second-order
  effect of BUG-966 (below) and was briefly recorded as a `zer_gaps` entry before the
  same day's fix closed it.
- **A DATA RACE CLOSED: a shared read in a defer-body CONDITION took no mutex.**
  `defer { if (g.v > 3) { … } }` emitted a bare `if ((g.v > 3))`. BUG-749 (B5) had
  covered `NODE_EXPR_STMT` only, so the assignment form locked and the condition did
  not — the partial-coverage shape, inside the partial emitter.
- **`switch`, `do-while` and `@critical` in a defer body were VALID CODE TURNED INTO A
  TRAP.** Each printed `compiler bug: emit_defer_stmt has no handler for node kind N`
  and substituted a `_zer_trap`. That helper covers ELEVEN node kinds; `lower_stmt`
  covers 53, which is the whole bug class in one ratio.
- **NOT a win, though an earlier draft here claimed it: the auto-guard.** It was
  already emitted — refactor M put it there. L only changes it from C text re-emitted
  per fire site into one IR branch inside the template. Corrected 2026-09-08 by A/B-ing
  against a build of the pre-L commit; the claim had been inherited from this section's
  own pre-M motivation text and repeated without re-measuring.

**FOUR BUGS FOUND ON THE WAY — each was a defect in EXISTING code that only a real CFG
in the defer body could expose.**

1. **BUG-964 — `ir_compute_preds` read a block's LAST instruction, not its FIRST
   TERMINATOR.** Lowering appends past a terminator: a `return` in a switch arm
   terminates the block, and the arm's end still emits its `IR_DEFER_FIRE` and a GOTO
   to the join. That dead GOTO gave the join a predecessor it does not have, and once
   bodies were real IR, zercheck followed the phantom edge out of a path that had freed
   a handle into one that frees it again — a false double free
   (`rt_drop_enum_variant_cleanup`). **Fixed in the EDGE computation.** The first
   attempt deleted the trailing instructions instead, which passed that test and
   silently LOST a diagnostic: they are unreachable but still ANALYSED, and deleting
   them dropped the use-after-move report on `return t; u32 k = t.kind;`
   (`rt_move_struct_return_then_use`). Correct edges, every instruction intact.
   Same read-the-last-instruction bug also gave `IR_TRAP` — a terminator since BUG-957
   — a phantom FALL-THROUGH edge; it now has an explicit no-successor case.
2. **BUG-966 — the BARE `orelse return` fallback tagged only `bb_fail`.** Firing the
   defers now SPLICES, leaving `current_block` on a fresh continuation, so the
   `IR_RETURN` no longer landed in the tagged block and `is_orelse_fallback` missed it.
   The leak check then read the null-optional path as a live allocation. The BLOCK-form
   fallback has always tagged "the ENDING block if lowering split into sub-blocks"; the
   bare form did not, because before the splice it never split. Same question, two
   sites, one updated — and the fix is what closed the conditional-defer leak above.
3. **A fire at an ALREADY-TERMINATED position must not materialise.** The canonical
   case is a block-exit fire following a `return`: the raw-AST emitter emitted it too
   and it was never reached. Splicing there appends unreachable blocks, and zercheck
   walks every block, so the frees counted TWICE — false double-free across 38 fuzzer
   programs. Not an optimisation: emitting a body that cannot run is what makes the
   analysis wrong.
4. **MUTUAL RECURSION between M and L, found by ASan as a compiler stack overflow.** A
   bounds guard's early exit fires defers (M); materialising a body lowers statements
   carrying guards (L). Fixed with `defer_body_depth`: a guard inside a defer body
   emits `IR_TRAP` instead of returning — independently the SEMANTICALLY correct answer,
   since an early return there would re-fire the defer stack, which is exactly why the
   emitter's `guard_traps` exists. The same counter suppresses a fire emitted from
   inside a body (a `break` in the body's own loop reached `emit_defer_fire_scoped`,
   baking a stray fire into the template that then cloned to every site and left
   `IR_DEFER_PUSH` without a well-formed reachable fire — the one shape the third
   attempt could not land).

**KNOWN COST.** `tests/zer_fail/cinterop_defer_double_close.zer` lost the word
"deferred" from its diagnostic: the reason is unchanged and it is still rejected as a
double free, but the message now comes from the CFG rather than from zercheck's AST
scan of defer bodies, which knew the free came from a defer. The `expect-error`
directive was relaxed to `double free:` — the reason the rule is supposed to give, not
phrasing the mechanism no longer produces. Restoring the specificity means carrying a
"came from a defer template" bit on the spliced instructions.

**STILL ON THE AST PATH — the remaining work.** Functions containing a LABEL. Their
bodies are neither lowered nor spliced (`pre_lower_orelse` REWRITES nodes, so lowering
a template the emitter will also replay would violate "never lower the same AST twice"
from inside one lowering). `emit_defer_stmt` and `zercheck_ir`'s AST defer analysis
therefore both STAY, gated on `defer_fire_emit_ast`, and cannot be deleted yet. Closing
that needs the cleanup-label guard's correlation to survive as IR — guard-disjointness
over a REASSIGNED flag, which Level B's `ir_local_is_immutable_bool` deliberately
refuses — so it is a real piece of analysis, not a cleanup.

---

**HISTORY — the three attempts, kept for their MEASUREMENTS. Their PLANS are
superseded by the boundary above; do not implement from them.**

**STAGE 2 WAS BUILT AND MEASURED 2026-09-07, then reverted — do not re-derive any of
this.** The splice works; it is the emitter that is not ready. What was proven, on a
working build:

- **Lazy materialisation, not eager.** Lowering each body at REGISTRATION into a
  detached range and extracting it works, but leaves the body's temps as locals no
  fire uses — every defer-using function grew dead declarations, and 12 of 60 defer
  programs changed their emitted C for no benefit (for an async function locals ARE
  the state struct). Lowering at the FIRST fire and cloning at later ones has no such
  cost: the first copy is live.
- **The CFG splice is correct.** Measured by making the body fire twice on purpose:
  `defer { g += 1; }` went 11 -> 12, a two-fire-site function 12 -> 14, and a body
  containing `for` + `if` + `break` went 106 -> 112 — exactly two extra runs, so the
  CLONE's internal branches remapped correctly.
- **The two runtime gates express fine as IR branches** — guard (skip when SET) outside,
  armed (run when SET) inside, matching the emitter's nesting.
- **Instruction arrays are arena-allocated**, so a snapshot's pointers stay valid; the
  implementation deep-copies anyway rather than depend on that.
- **The whole defer corpus ran with the splice on: 0 crashes, 0 hangs.** The 20
  failures were all `zercheck failed` — the body counted TWICE, once through the new
  CFG and once through zercheck's AST scan. That is the expected transition state and
  confirms where the remaining work is: `zercheck_ir`'s AST defer analysis is
  concentrated in four places (the C3 pass at ~7601, the IR_DEFER_FIRE handler at
  ~6352, `ir_defer_instance_index` at ~2657, and the op skip at ~2633).
- **IR_DEFER_PUSH / IR_DEFER_FIRE must STAY** for `ir_validate`'s push/fire balance
  check; only body EMISSION moves out of the emitter.
- **Sound to lower a defer body at all** because every `orelse` form is banned inside
  one — value fallback, var-decl, return, break, continue, all measured — so
  `pre_lower_orelse` is a no-op there and cannot mutate the AST out from under a later
  fire. (Those bans exist only because the raw-AST emitter cannot express a branch;
  after L they can be relaxed.)

**STAGE 2, THIRD ATTEMPT (2026-09-08) — 5 failures down to 1, and FOUR of the five
blockers are SOLVED with the fixes written out below. Reverted again; `ir_compute_dominators`
LANDED (BUG-960) as the prerequisite it is.** Re-apply from here, not from scratch.

**SOLVED — apply these, they are measured:**

1. **The ARMED gate.** Elide it where the block that SET the flag DOMINATES the gate.
   The flag is set immediately after `IR_DEFER_PUSH` and never cleared, so there it is
   provably 1 and the branch need not exist. Where it does not dominate — the goto-skip
   shape the flag exists for — the branch stays and the conservative answer is right.
   Needs `armed_setter_block` on the branch, `defer_armed_block[di]` recorded at
   registration, and a post-lowering pass after `ir_compute_preds` (NOT during lowering:
   a later goto can add an edge that destroys a dominance that held at the time).
   **Fixes `goto_defer`.**
2. **The cleanup-label GUARD.** Do NOT lower a guarded body — keep it on the raw-AST
   path, and have the emitter emit exactly those (same `guarded` condition, from the
   same fields, so the two cannot disagree). Unlike the armed flag this one genuinely
   varies, so dominance cannot help; recovering it would need guard-disjointness over a
   REASSIGNED flag, which Level B's `ir_local_is_immutable_bool` deliberately refuses.
   zercheck must then still scan those bodies from the AST — and WITHOUT the
   `ir_fire_has_work_after` gate, since Phase C3 no longer runs to cover exit fires.
   **Fixes `defer_goto_handle_leak_regression`.**
3. **SCOPE.** Lower the template at REGISTRATION and EXTRACT it (copy the blocks aside,
   truncate `block_count`), not at the first fire. Lowering re-resolves identifiers, and
   the first fire can be in a NESTED scope — `{ Handle h = mh2 orelse return; }` fires
   the outer defer from the inner block — where `pool.free(h)` resolves to the INNER
   shadowed handle. Restoring the scope DEPTH alone does not reproduce the registration
   context; measured, it still resolved to the shadow. **Fixes `handle_shadow_scope`.**
   The earlier objection to eager lowering (dead locals) was measured BEFORE a consumer
   existed — once every fire clones the template, its locals are used.
4. **A defer body must not FIRE defers.** Guard `materialise_defers_from` on
   `defer_body_depth > 0`.

**~~STILL OPEN — ONE shape.~~ CLOSED 2026-09-08 in the fourth attempt** — verified by
running the exact program below, exit 0; pinned by `tests/zer/defer_body_loop_break.zer`.
The cause was not the break's CFG edge as guessed here: a `break` in the body's own loop
reached `emit_defer_fire_scoped`, which baked a stray `IR_DEFER_FIRE` INTO the template,
and every fire then cloned it — leaving an `IR_DEFER_PUSH` whose only fires were inside
copies of a body. Suppressing the SPLICE was not enough; the INSTRUCTION is the half that
breaks validation. `defer_body_depth > 0` now returns from all three fire emitters. Text
kept for the diagnosis method, which was sound. The original claim:

A `break` inside a defer body's own loop makes `ir_validate` abort:
*"IR_DEFER_PUSH has no CFG-reachable IR_DEFER_FIRE"*. Minimal:

```zer
void f(){ defer { for (u32 i = 0; i < 5; i += 1) { if (i == 2) { break; } g += 1; } } }
```

Note this shape only became legal in BUG-947 (this session), so it is new ground. The
body without the `break` validates and runs correctly, so it is the break's CFG edge —
most likely its target leaving the extracted template range, or the reachability DFS
losing the PUSH-to-FIRE path once the template is truncated out. Diagnose by dumping the
CFG with the validator temporarily downgraded to a warning; `--emit-ir` is useless here
because validation aborts first.

**STAGE 2, SECOND ATTEMPT (2026-09-07, after M landed) — got MUCH further, one blocker
left, and it is a DESIGN question rather than a bug.** Reverted again; tree unchanged.

Failures went **38 -> 7 -> 5** as three genuine bugs were found and fixed. Those fixes
are the part worth keeping:

1. **Materialising at an UNREACHABLE fire.** A block-exit fire that follows a `return`
   is dead — the raw-AST emitter also emitted it and it was simply never reached. But
   zercheck walks every block, so splicing the body there counted its frees a SECOND
   time: a false "double free" plus "use after free" on
   `defer { p.free(h); } … return 0;`, 38 fuzzer programs. **Fix: skip materialisation
   when the current block is already terminated.** Not an optimisation — emitting a
   body that cannot run is what makes the analysis wrong.
2. **MUTUAL RECURSION between M and L, found by ASan as a compiler stack overflow.**
   A bounds guard's early exit fires defers (M), and materialising a defer body lowers
   statements that themselves carry guards (L) — `lower_stmt -> materialise -> lower_stmt`
   forever. **Fix: a `defer_body_depth` counter; a guard inside a defer body emits
   IR_TRAP instead of a return.** That is also the SEMANTICALLY correct answer
   independently — an early return inside a defer body would re-fire the defer stack and
   skip the rest of the function's cleanup, which is exactly why the emitter's
   `guard_traps` exists.
3. **zercheck's AST defer analysis double-counts** once bodies are real CFG. Disable
   the `IR_DEFER_FIRE` handler's `ir_defer_scan_frees` call and the Phase C3 pass;
   `ir_defer_scan_uses` / `_frees` / `ir_defer_instance_id` / `ir_fire_has_work_after`
   then become dead and should be deleted with them.

**THE REMAINING BLOCKER — the runtime GATES become CFG merges.** A defer body is
wrapped in two gates: ARMED (`this registration never ran, skip it`) and the
cleanup-label GUARD. As raw AST they were invisible to zercheck, which applied the
body's frees unconditionally from the AST at each return block. As IR branches they are
VISIBLE, so the analysis now sees a path where the body is skipped, and the merge of
{freed} and {alive} is MAYBE_FREED:

    tests/zer/goto_defer.zer -> "handle 'c' may not be freed on all paths"

Three of the five remaining failures are this, in `goto` / shadowing shapes
(`goto_defer`, `handle_shadow_scope`, `defer_goto_handle_leak_regression`).

**The `is_orelse_fallback` exemption does NOT solve it** — that mechanism skips a whole
RETURN block, and the armed-skip path REJOINS and shares the same return block, so no
block-level flag reaches it. Checked before assuming.

**What would:** elide the armed branch where the flag provably dominates the fire. The
flag is set immediately after `IR_DEFER_PUSH` in the same block, and in `goto_defer`
that block dominates the fire, so the flag is provably 1 — the branch need not exist.
The flag is currently created for EVERY defer in a function containing ANY label,
deliberately over-broad so soundness does not depend on the partial
`find_goto_to_label` walk. Keeping that for EMISSION while eliding the IR BRANCH under
dominance is sound and is the shape of the fix. It needs a dominator computation over
`func->blocks`, which does not exist yet (`ir_compute_preds` does, so it is a short step).

**One test result worth keeping either way:** `audit2_defer_scan_nested` in
`tests/zer_gaps/` reported *"no longer exhibits its gap"* — the inverted-expectation
harness saying the splice CLOSED a recorded gap. That is the safety win L is for,
observed.

**Staging (as planned; steps 1 and 2 SHIPPED, step 3 is what remains):**

1. ~~IR block-range cloner (`ir.c`)~~ — SHIPPED, BUG-950.
2. ~~Lower each defer body ONCE into a detached block range at `NODE_DEFER`; at each
   fire site splice in a clone. The change point is the THREE functions
   `emit_defer_fire`, `emit_defer_fire_scoped`, `emit_defer_pop_only`~~ — SHIPPED for
   every function WITHOUT a label, and the three functions were indeed the whole change
   point.
3. Delete `emit_defer_stmt` (214 lines) and prune `zercheck_ir.c`'s second defer
   analyzer (~733 lines). **STILL BLOCKED** — both are live for label-containing
   functions; see "STILL ON THE AST PATH" above.

The existing machinery this had to keep, and did: the capture-on-FIRE snapshot
(`ir_snapshot_defer_bodies`, the plt86m defer-goto gap), the per-defer ARMED flag (F2 —
a forward `goto` can jump over a registration), and the both-reachable-cleanup-label
guard (`defer_fire_guard_flag`). Each exists because a real bug got through; none is
optional. All three are exactly the machinery of the LABEL path, which is why declining
wholesale for those functions preserves them untouched.

### ORDER OF L AND M — MEASURED 2026-09-06, do not re-derive

**M FIRST, THEN L** — corrected 2026-09-07; see the dependency note at the end of this
section, which is the authoritative line. (This section first said L-before-M, itself a
correction of an even earlier size-based ordering. The severity comparison below stands
— L fixes more, and worse — but M is the PREREQUISITE, so it goes first.) Every line below was
measured against main at `b925a871` by INSPECTING EMITTED C, never by exit code — the
auto-guard is a silent `if (...) return;`, so exit 0 can mean "the guard fired". Reading
exit codes gives the wrong answer here, and it gave me one twice during this check.

**L's live symptom set — all three confirmed:**

| symptom | evidence |
|---|---|
| unlocked shared read | `defer { if (s.v > 0) {…} }` emits `if ((s.v > 0))` with NO `pthread_mutex_lock`; the identical read in an ordinary statement locks |
| **false assurance + silent OOB** | `defer { u32 v = arr[i]; }` warns *"auto-guard inserted"* and emits **ZERO** guards. Same for a defer-body FOR-init and WHILE-cond |
| trap on VALID code | `switch`, `do-while` and `@critical` in a defer body each emit `_zer_trap("compiler bug: unsupported stmt kind in defer")` |

One root cause: `emit_defer_stmt` (emitter.c, 214 lines) is a SECOND statement emitter
with arms for BLOCK / EXPR_STMT / RETURN / ASM / IF / WHILE / FOR / VAR_DECL / BREAK /
CONTINUE, and it calls `emit_auto_guards` in only THREE of them.

**M's live symptom set: ONE, and it is not a soundness hole.** An indexed shared root
emits the lock first and then traps inside it:

```c
pthread_mutex_lock(&(_zer_bounds_check((size_t)(i), 4, …), arr)[i]._zer_mtx);
if ((size_t)(i) >= 4u) { _zer_trap("out-of-bounds access inside a held lock …"); }
```

so a program that should take the guard's clean early return exits 133 instead. Safe,
just worse. **CLOSED 2026-09-07 as BUG-952** — `IR_LOCK` was simply missing from the
gate; adding it emits the check before the lock (133 -> 0). The rest of M — replacing
the hand-maintained allowlist with a single choke point — is still open, and is the
PREREQUISITE for L. Everything else about M's premise is TRUE but currently LATENT: the gate is
a hand-maintained allowlist of 8 op kinds in TWO copies (regular + async), widened
reactively at least twice — but **25 op kinds sit outside it and none could be made to
drop a guard**, because lowering funnels conditions through gated ops first. Probed
clean: `if` / `while` conditions, for-init, IR_BINOP, `@critical`, `@once`, an async
body, and an async `await` condition.

**Dependency — CORRECTED 2026-09-07 by building the splice and hitting the wall. The
order is M FIRST, THEN L.** The earlier conclusion here ("no dependency in the other
direction") was wrong, and it was wrong because it was reasoned rather than built.

L fixes the three unguarded defer positions and M does not — that half still holds. But
**L cannot COMPLETE without M**, for a reason only visible from the emitter side:

`emitter.c:540` is the auto-guard early return, and it calls `emit_defers(e)` — the
RAW-AST path — because an out-of-bounds index must run the pending cleanup before it
returns. That exit is synthesised as C by the emitter and does not exist in the IR at
all. So while auto-guards live outside the IR:

- if defer bodies are spliced into the CFG *and* still emitted at that exit, they
  DOUBLE-FIRE on the guard path;
- if they are spliced and NOT emitted there, cleanup is SKIPPED on the guard path —
  a leak, which is worse.

Either way `emit_defer_stmt` cannot be deleted. **M is exactly what removes the
obstacle**: lowering auto-guards into the IR as branches makes every early exit an IR
edge, after which L's splice covers all of them and the second emitter can die. (The
same tension is already visible in that site's own code: inside a defer body or a held
lock the auto-guard TRAPS instead of returning, with the comment *"cannot return without
leaking it"*.)

### SUGGESTED ORDER

```
1. A  ~~switch arm as the 9th value-flow sink~~  CLOSED (BUG-934)
2. B  ~~the 16-root lock cap~~               CLOSED (BUG-935), gate taken
3. C  ~~stale VRP in defer bodies~~          CLOSED (BUG-936); 3 siblings need M
4. D  ~~Level B multi-free~~                 CLOSED (BUG-937)
5. E  ~~&freed.field~~                       CLOSED (BUG-938) + its sibling
6. F  ~~const-expression MISCOMPILE~~       CLOSED (BUG-939); binary-operand
      residual folds into K (BUG-921) — same predicate, do them together
7. ~~J/I/K/H/G~~  ALL CLOSED (BUG-940/941/942/943+944/945+946)
8. L/M  the two refactors -- deliberately, and only after 1-7 are stable
```

---

## OPEN — BRANCH SURVEY 2026-08-20: 11 `vigilant-tesla-*` branches, ~100 live holes NOT yet fixed

**Read this section ALONE and you can start work. Nothing here needs re-deriving.**
Every finding was MEASURED against `main` at `c8e58304` using the branch's OWN test
file (a reconstruction can CONFIRM a hole but never REFUTE one). Reproducers are
inline so you do not need to fetch the branches to begin.

### Branches surveyed, and where each forked

| branch | ahead | behind | forked at | date |
|---|---|---|---|---|
| `vigilant-tesla-v7pucv` | 1 | 0 | c8e58304 | 2026-09-03 |
| `vigilant-tesla-pstdqk` | 1 | 0 | c8e58304 | 2026-09-02 |
| `vigilant-tesla-yzhu1s` | 4 | 0 | c8e58304 | 2026-09-01 |
| `vigilant-tesla-fhf8rn` | 2 | 0 | c8e58304 | 2026-08-31 |
| `vigilant-tesla-lzmkhn` | 8 | 0 | c8e58304 | 2026-08-30 |
| `vigilant-tesla-o51x9p` | 2 | 0 | c8e58304 | 2026-08-29 |
| `vigilant-tesla-1zukjq` | 6 | 0 | c8e58304 | 2026-08-28 |
| `vigilant-tesla-ef9cao` | 7 | 0 | c8e58304 | 2026-08-27 |
| `vigilant-tesla-osp1a7` | 7 | 17 | ae033cd0 | 2026-08-26 |
| `vigilant-tesla-as71kk` | 16 | 18 | 9f8cda08 | 2026-08-25 |
| `vigilant-tesla-r1piyr` | 1 | 19 | 1c4c64d2 | 2026-08-24 |

Eight forked from CURRENT HEAD, so they audit the live compiler and their findings
are live. The last three are largely CONSUMED (osp1a7's BUG-909..912 landed; as71kk
BUG-857..881; r1piyr BUG-857..864) — only the residuals listed below survive.

**BUG NUMBERS COLLIDE.** All eight current-HEAD branches independently numbered from
BUG-913. Never trust a branch's BUG-NNN; renumber on adoption.

Fetch a file without checking out:
    git show origin/claude/vigilant-tesla-<id>:tests/zer_fail/<name>.zer

### THREE HARNESS TRAPS THAT WILL WASTE YOUR TIME (all hit during this survey)

1. **A `zer_fail` test that produces NO checker diagnostic is not automatically a
   hole.** ZER answers several classes by TRACKING (a runtime guard), not by
   rejecting — enum forging is the big one. `zerc f.zer -o out.c` shows nothing and
   the program still TRAPS at runtime, correctly. Discriminate by running:
   `zerc f.zer --run; echo $?` — **133 = SIGTRAP = handled, not a hole.**
   This would have produced ~18 false holes in this survey.
2. **`$?` is clobbered by command substitution.** `printf "%s %s" "$(basename $f)" "$?"`
   reports the exit of `basename`, not of the program. Capture `rc=$?` on the line
   IMMEDIATELY after the command. This made all four enum trap pins look broken when
   the real runner shows them PASS at exit 133.
3. **Module tests extracted flat cannot resolve their imports.** ~12 `xmod_*` /
   `contuser` / `spawnimp_user` files "fail" with `cannot open module` — that is the
   extraction, not an over-rejection. Keep companions in the same directory.

4. **A TIMED-OUT probe looks exactly like "accepted with no diagnostics".** A probe
   that wraps `zerc` in `timeout N` and then greps stdout for diagnostics cannot tell
   a HANG from a clean accept — both produce empty output. This survey classified
   `const u32 A = A;` as "accepted" when it actually hangs the compiler forever.
   Always capture the exit code separately and treat 124 as its own verdict.

Also: `tests/zer_trap/` files may carry `// zerc-flags:` on the first 5 lines and the
runner passes them; a probe that ignores them measures a different program.

---

### CLASS 1 — ENUM FORGING: the door set is NOT closed (HIGH, accept-unsafe)

**CLAUDE.md currently states the doors are "EXACTLY THREE and the set is closed
(@bitcast, @truncate, @saturate)". MEASURED FALSE 2026-08-20.** That sentence must be
corrected when this class is fixed.

**CONFIRMED HANDLED (do NOT re-fix — these TRAP at 133):** `@bitcast`, `@truncate`,
`@saturate`, `@pun` to a funcptr, `@inttoptr` mint. Main's four
`tests/zer_trap/*_enum_forged_*.zer` pins all pass.

**LIVE — a plain LITERAL is the fourth door, and it was never counted.** Branch
`fhf8rn`. Ten sinks, each its own test file, all reach an impossible switch arm
(the programs exit 99, their own "impossible" signal):

    enum Color { red, green, blue }
    u32 main() { Color c = 99; return (u32)c; }        // enum_forge_lit_vardecl

Sinks: `_vardecl`, `_assign`, `_callarg`, `_return`, `_structinit`, `_arrayelem`,
`_globalinit`, `_orelse`, `_spawnarg`, `_negative`.

**LIVE — enum ARITHMETIC produces non-variants.** Branch `fhf8rn`, 5 forms
(`enum_arith_binary`, `_bitwise`, `_compound`, `_unary_minus`, `_unary_not`):

    enum Color { red, green, blue }
    u32 main() { Color c = Color.blue; Color d = c + c; return (u32)d; }

**LIVE — `@ptrcast` is a door** (`fhf8rn/enum_mint_ptrcast`, exits 99).
**LIVE — `@pun` to enum / pointer / slice, `@inttoptr` to an enum target**
(`ef9cao`: `pun_forge_enum`, `pun_forge_pointer`, `pun_forge_slice`,
`inttoptr_enum_target`, `inttoptr_enum_in_struct`).

**Best source: `fhf8rn`** (literal sinks + arithmetic) **plus `ef9cao`** (the
`@pun`/`@inttoptr` doors). Fix shape: the variant guard already exists and already
recurses struct fields / optional payloads / array elements — it needs to be wired at
the literal-assignment sites and at these conversions, i.e. ONE predicate at the
value-flow sites, not another per-door patch.

---

### ~~CLASS 2 — CAST/LAUNDER AT THE **HEAP / UAF** SINK~~ — **CLOSED 2026-09-04 as BUG-931, DO NOT REDO**

> **All eleven recorded forms (2a–2g) now reject; measured.** The cause was NOT a missing
> peel but **EIGHT separate hand-rolled peelers**, each knowing a different subset of the
> carriers — so every carrier was rejected at some sinks and silently accepted at others,
> which is why no single-sink test found it. `checker.c` already had a complete peeler
> (`unwrap_ptr_launder`); five sinks simply never called it, and `zercheck_ir.c` had three
> partial ones of its own plus an alias arm hardcoded to `"ptrcast"`.
>
> **Landed:** `zercheck_ir.c`'s three peelers collapse into one `ir_peel_launder`
> (allowlist-based, so a VALUE-producing intrinsic like `@ptrtoint` is NOT peeled); the
> five `checker.c` sites call `unwrap_ptr_launder`; one new shape at the assign-value sink
> (a launder can yield the ARRAY ITSELF, gated on a POINTER destination — between two
> arrays it is a value copy, see trap below).
>
> **Also closed an OVER-REJECTION:** freeing through a laundered name was reported as a
> leak of the original. `tests/zer/cast_launder_ok.zer` (the branches' own boundary
> positive) failed pre-fix and passes now.
>
> **Gate:** `tools/sink_matrix.sh` **p15b — carrier x sink**, 12 cells (88 -> 100),
> verified to fire (9 holes + 1 over-rejection against a pre-fix build). Nine negatives in
> `tests/zer_fail/`, each with `// expect-error:` and each verified accepted pre-fix.
>
> **Trap paid for here:** the first version of the array-decay shape rejected
> `volatile u8[4] hw; hw = src;` — a whole-array VALUE COPY, not a pointer store. Caught by
> `test_emit.c`'s BUG-273 case. Any future widening of that shape must keep the
> pointer-destination gate.

<details><summary>original entry (kept for the reproducers)</summary>

### CLASS 2 — CAST/LAUNDER AT THE **HEAP / UAF** SINK (CRITICAL, accept-unsafe)

**This is NOT the class closed by BUG-791.** BUG-791 peeled launders at the STACK
ESCAPE sinks (`unwrap_ptr_launder` + the store-to-global arm + `ir_extract_compound_key`).
The sink still open is the **allocation-identity / handle-tracking** one in
`zercheck_ir.c`: a laundered pointer assigned to a GLOBAL loses its alloc identity, so
the free is never linked to the later use.

**2a. UAF through a cast-laundered GLOBAL** — `lzmkhn/cast_launder_global_dangle`.
Measured: **0 diagnostics.**

    struct N { u32 v; }
    ?*N g;
    u32 main() {
        *N n = alloc(N) orelse { return 1; };
        n.v = 7;
        g = (*N)n;          // <-- identity lost here
        free(n);
        *N r = g orelse { return 2; };
        return r.v;         // use-after-free
    }

**2b. UAF through `@cast` to a distinct typedef** — `1zukjq/cast_launder_uaf`:

    distinct typedef [*]u8 Buf;
    u32 res;
    void f() {
        [*]u8 b = alloc(u8, 4) orelse return;
        Buf c = @cast(Buf, b);
        free(b);
        res = (u32)c[0];    // UAF
    }

**2c. DOUBLE FREE through the same distinct-typedef launder** —
`1zukjq/cast_launder_double_free`: `free(c); free(b);` where `c = @cast(Buf, b)`.

**2d. Cast defeats KEEP INFERENCE** — `lzmkhn/cast_launder_keep_inference`:

    ?*u32 g_p;
    *u32 idfn(*u32 p) { return p; }
    void stash(*u32 p) { g_p = idfn((*u32)p); }
    u32 main() { u32 loc = 5; stash(&loc); return 0; }

**2e. STRUCT LITERAL is an unpeeled launder** — `lzmkhn/cast_launder_struct_literal_escape`
and `fhf8rn/local_launder_structlit_ccast`:

    struct H { ?*u32 p; }
    H g;
    void c() { u32 loc = 5; g = { .p = (*u32)(&loc) }; }

**2f. ARENA-derived pointer laundered to a global** — branch `fhf8rn`, 4 forms.
`is_from_arena` is defeated by a C-cast, by `@pun`, by a struct literal, and by a
two-hop C-cast:

    struct N { u32 v; }
    ?*N g;
    void stash() {
        u8[256] bk;
        Arena ar = Arena.over(bk);
        *N a = ar.alloc(N) orelse return;
        g = (*N)a;                      // arena_launder_ccast_global
        // g = @pun(*N, a);             // arena_launder_pun_global
        // gb = { .p = @pun(*N, a) };   // arena_launder_structlit
        // *N b = (*N)a; g = b;         // arena_launder_twohop_ccast
    }

**2g. `@cstr` and `@container` as launders** — branch `ef9cao`:

    ?*u8 gp;
    void publish() {
        u8[8] buf;
        const [*]u8 s = "hi";
        gp = @cstr(buf, s);             // launder_cstr_global_escape — buf is a LOCAL
    }

    // launder_container_struct_literal: @container inside a struct literal
    Dev local; *LH lp = &local.lh;
    W w = { .d = @container(*Dev, lp, lh) };

**Best source: `lzmkhn`** (4 forms incl. keep-inference) **+ `fhf8rn`** (arena, 4
forms) **+ `1zukjq`** (distinct-typedef UAF + double-free) **+ `ef9cao`**
(`@cstr`/`@container`).

**Fix shape — do NOT patch these one at a time.** The peel must reach (i) the
allocation-identity path in `zercheck_ir.c` for a store to a GLOBAL, and (ii) the
STRUCT LITERAL as a launder carrier at every escape sink. `fhf8rn`'s own
limitations.md records "CLOSED 2026-08-31 (BUG-920) — the hand-rolled peel sites WERE
live holes", which is the same conclusion from the other side.

---

</details>

### CLASS 3 — LOOP COUNTER PAST END / OFF BY ONE (HIGH, silent OOB) — `v7pucv`

7 tests: `loop_counter_off_by_one`, `_past_end`, `_past_end_field`,
`_step_overshoot`, `_while_past_end`, `loop_counter_dowhile_past_end`,
`dowhile_counter_past_end_bug748`.

    u32 main() {
        u32[4] arr;
        for (u32 i = 0; i <= 4; i += 1) { arr[i] = i; }   // <= is one past the end
        return 0;
    }

Companion POSITIVES that must keep compiling (currently OVER-REJECTED, see below):
`loop_counter_bounds_ok`, `vrp_empty_range_zero_trip_ok`.

---

### ~~CLASS 4 — THE **ASSIGN FORM** IS NOT TRACKED~~ — **CLOSED 2026-09-05 as BUG-933, DO NOT REDO**

> All 11 reproducers reject; each verified accepted on the pre-fix build. FIVE distinct
> causes, not one: (1) `ir_unwrap_alloc_expr` does not peel `NODE_ASSIGN` and the assign
> target is never any instruction's `dest_local`, so the allocation was never registered;
> (2) the move-on-assign arm required a FIELD/INDEX target, missing the bare `b = a;`;
> (3) a move SOURCE that is a projection never resolved; (4)
> `ir_contains_move_struct_field_depth` did not recurse into **ARRAY** (the carrier class);
> (5) that walk returned **false** past its depth guard, so a 34-deep nest defeated move
> tracking by being deep — **exceeding a guard must be conservative**.
>
> Registration is now shared (`ir_register_alloc_result`) between the var-decl and assign
> paths so the two spellings cannot drift apart again.
>
> Tests: the branch's 11 negatives verbatim + `tests/zer/assign_form_ok.zer` (boundary
> positive: assign-form alloc, move-then-use-DEST, move out of an array, and a plain
> non-move array copy must all still run).

<details><summary>original entry (kept for the reproducers)</summary>

### CLASS 4 — THE **ASSIGN FORM** IS NOT TRACKED (HIGH, accept-unsafe) — `v7pucv`

Allocation and move tracking key on the VAR-DECL spelling; the ASSIGNMENT spelling of
the same operation is invisible. 9 tests.

    struct Device { u32 id; }
    Pool(Device, 8) pool_a;
    void run() {
        ?Handle(Device) mh;
        mh = pool_a.alloc();        // ASSIGN, not var-decl  <-- not registered
        Handle(Device) h = mh orelse return;
        pool_a.free(h);
        u32 v = pool_a.get(h).id;   // UAF, accepted
    }

Forms: `alloc_assign_form_uaf`, `_double_free`, `_leak`, `_ptr_uaf`, `_branch_uaf`,
`_universal_uaf`; and for `move struct`: `move_assign_form_ident`, `_field`, `_index`:

    move struct Token { u32 id; }
    u32 main() { Token a; a.id = 1; Token b; b = a; return a.id; }  // use-after-move

Also `v7pucv`: `move_struct_deep_nesting_uam`, `wrong_pool_across_branch`.

---

</details>

### CLASS 5 — ATOMIC CELL vs SCOPED SPAWN (HIGH, silent race) — `v7pucv`

The atomic-cell rule does not cover the window between `spawn` and `.join()`:

    u32 g_ctr;
    void worker() { u32 v = @atomic_add(&g_ctr, 1); }
    u32 main() {
        ThreadHandle t = spawn worker();
        g_ctr = 5;              // plain write races the atomic — accepted
        t.join();
        return 0;
    }

3 tests: `atomic_cell_scoped_spawn_window`, `_two_threads`, `_via_helper`.

---

### CLASS 6 — RMW SPLIT OVER TWO STATEMENTS (MEDIUM, bare-metal) — `1zukjq`

**Direct residual of BUG-792.** That fix made the rule catch the written-out
single-statement form (`g = g + 1`); splitting the same operation over two statements
still evades it, because the rule is per-STATEMENT:

    volatile u32 g;
    interrupt TIM2 { g = 7; }
    u32 main() {
        u32 t = g;
        g = t + 1;      // same RMW, two statements — accepted
        return g;
    }

---

### CLASS 7 — VIEW / STRUCT-INIT FIELD (HIGH, accept-unsafe) — `1zukjq`

    // multiview_assign_uaf — a reassigned slice view loses which allocation it names
    [*]u8 pick([*]u8 a, [*]u8 b, bool fl) { if (fl) { return b[0..1]; } return a[0..1]; }
    void g(bool fl) {
        [*]u8 x = alloc(u8, 4) orelse return;
        [*]u8 y = alloc(u8, 4) orelse return;
        [*]u8 h = x;  h = pick(x, y, fl);
        free(y);  res = (u32)h[0];  free(x);
    }

    // struct_init_field_uaf — a freed pointer stored into a struct literal, then read
    *Item t = pool.alloc_ptr() orelse return;
    pool.free_ptr(t);
    Holder h = { .t = t };
    res = h.t.id;

    // param_local0_double_free — unwrap a by-value param's handle field, free both
    void f(Holder hd) { Handle(Item) k = hd.h; pool.free(k); pool.free(hd.h); }

Also `multiview_branch_join_uaf`, `struct_init_field_move`.

---

### CLASS 8 — `defer` BODY: **TWO BRANCHES DISAGREE. TAKE `ii7a90`, NOT `o51x9p`.**

> **CORRECTED 2026-09-06 by a cross-branch comparison. The heading below —
> "restrictions not enforced" — ENCODES o51x9p's ANSWER, and that answer is the
> WRONG ONE by ZER's own Ban Decision Framework. Do not implement it.**
>
> Both branches found the same defect and fixed it in OPPOSITE directions:
>
> | | `o51x9p` | `loving-davinci-ii7a90` |
> |---|---|---|
> | verdict | **BAN** switch/`@critical`/`@once`/spawn/label in a defer body | **SUPPORT** them — lower the body into the IR |
> | tests | 5 `zer_fail` + 1 positive | 6 `zer` POSITIVES |
> | size | +615 lines, mostly `checker.c` | net **−386** (+1448 / −1834) |
>
> **What main actually does (measured 2026-09-06):** `defer { switch (k) {…} }`
> compiles with **0 diagnostics**, emits a `compiler bug: unsupported stmt kind in
> defer` marker, and **traps at runtime (exit 133) on valid code.** So this is not
> an accept-unsafe hole and not a missing ban — it is a BROKEN EMISSION.
>
> **Why ii7a90 wins.** The Ban Decision Framework permits a ban only for a hardware
> constraint, an EMISSION IMPOSSIBILITY, a missing runtime, or a missing type
> system. o51x9p's ban implicitly claims the second — and ii7a90 **disproves it by
> emitting the construct correctly**. None of the other three applies, so rule 5
> governs: *track, don't ban*. CLAUDE.md also says outright: *"If the user pushes
> back on a ban, they're usually right — look for a tracking solution."*
>
> Adopting o51x9p here would have turned a fixable emitter gap into a permanent
> language restriction, and cost 615 lines to do it.
>
> **Keep from o51x9p:** nothing on this class. Its 5 negatives assert the ban and
> must NOT be added — they would pin the wrong behaviour.

<details><summary>original entry (o51x9p's framing — kept for its reproducers only)</summary>

### CLASS 8 — `defer` BODY RESTRICTIONS NOT ENFORCED (MEDIUM) — `o51x9p`

5 tests. `spawn` inside a defer body is accepted:

    void w(u32 v) { u32 t = v; }
    u32 main() { defer { spawn w(1); } return 0; }

Forms: `defer_body_spawn`, `_critical`, `_label`, `_once`, `_switch`.

---

</details>

### CLASS 9 — GLOBAL INITIALIZERS: a self-cycle **HANGS THE COMPILER** (HIGH, DoS) — `o51x9p`

**Upgraded from MEDIUM after re-measurement 2026-08-20.** This does not merely
compile — the compiler LOOPS FOREVER in constant evaluation. Measured `exit=124` at a
15-second timeout, on both the hand-written form and the branch's own file:

    const u32 A = A;            // global_init_cycle_self -- COMPILER HANGS
    u32 main() { return A; }

A hang is worse than an accept: it cannot be caught by any negative test that has a
timeout, and it takes `make check` down with it (`tests/zer_trap` already treats 124
as an explicit FAIL for exactly this reason). Fix needs a visited-set / depth bound in
the global-initializer constant folder, reported as a diagnostic.

4 tests: `global_init_cycle_self`, `_cycle_mutual`, `_chain_too_deep`,
`_from_mutable`. Check whether `_cycle_mutual` hangs too before assuming it only
accepts.

---

### CLASS 10 — `@ptrtoint(&local)` LAUNDERED THROUGH A CALL (MEDIUM) — `o51x9p`

    usize g = 0;
    usize idfn(usize x) { return x; }
    u32 main() { u32 local = 5; g = idfn(@ptrtoint(&local)); return 0; }

3 tests: `_via_call_global`, `_via_call_alias`, `_via_call_return`.
CLAUDE.md lists `@ptrtoint(&local)` direct AND indirect escape as covered — the
CALL-laundered form is not.

---

### CLASS 11 — MMIO ADDRESS VIA A `const` IDENT (MEDIUM, bare-metal) — `o51x9p`

The range and alignment checks fold a literal but not a `const` identifier:

    mmio 0x4000_0000..0x4000_0FFF;
    const u32 ADDR = 0x40005000;                  // outside the declared range
    u32 main() { volatile *u32 r = @inttoptr(*u32, ADDR); return 0; }

2 tests: `mmio_const_ident_oob_addr`, `mmio_const_ident_misaligned`.
Companion positive `mmio_const_ident_base` is currently OVER-REJECTED (below).

---

### CLASS 12 — FUNCPTR FACTORY THROUGH switch / do-while (MEDIUM) — `pstdqk`

Extends the funcptr REACH class. `scan_returned_funcname` does not descend a `switch`
arm or a `do-while` body:

    u32 g;
    void cb() { g += 1; }
    void nop() { }
    *() -> void mk(u32 k) {
        switch (k) { 0 => { return cb; } default => { } }
        return nop;
    }
    void worker() { *() -> void fp = mk(0); fp(); }
    u32 main() { spawn worker(); return 0; }

3 tests: `spawn_race_factory_switch`, `spawn_race_factory_dowhile`,
`isr_race_factory_switch`. **Both sinks — add the ISR mirror in the same commit.**

---

### CLASS 13 — `@container` WHOLE-OBJECT / ARRAY ELEMENT (MEDIUM) — `ef9cao`

    struct Inner { u32 a; }
    struct Outer { u64 pad; Inner in; }
    u32 main() {
        Inner i; i.a = 3;
        *Inner ip = &i;                            // NOT a field of an Outer
        *Outer o = @container(*Outer, ip, in);     // accepted — reads outside `i`
        return (u32)o.pad;
    }

5 tests: `container_whole_object`, `_alias`, `_direct`, `_global`,
`container_array_element`.

---

### CLASS 14 — CLI ARGUMENT VALIDATION (LOW, tooling) — `as71kk`

`cli_unknown_option`, `cli_bad_target_arch`, `cli_bad_stack_limit`. Unknown and
malformed arguments are silently ignored. **NOTE: also found independently on branch
`hefb6x` in the previous survey and still not landed** — two branches, one finding.

---

### CLASS 15-18 — smaller, each one branch

- **i64 literal range** (`lzmkhn`): `i64_literal_above_max`, `_below_min`,
  `_over_range_sinks`.
- **compound float <-> int** (`fhf8rn`): `compound_float_into_int`,
  `compound_int_into_float`.
- **negative const into unsigned** (`osp1a7`): `neg_const_into_unsigned_assign`,
  `_tilde`. NB: distinct from the ALLOWED unsigned ops `~0` / `0 - 1`; see the
  corpus-cost rule in CLAUDE.md before widening.
- **misc** (`yzhu1s`): `bitcast_bare_array_source` (`stack_limit_root_entry` is CLOSED as BUG-1296).
- **bool as a forged-tag carrier** (`fhf8rn/bool_mint_ptrcast`).

---

### NON-TEST FINDINGS (no reproducer; from the branches' limitations.md)

- ~~**`ir_merge_states` merges only 4-5 of ~17-18 `IRHandleInfo` fields.**~~
  **MEASURED 2026-09-05 (BUG-933) — mostly REFUTED as a safety claim; ONE field was real.
  Do not re-derive.** The count is right; the conclusion was not. Of the 13 unmerged
  fields: **7 are set at handle CREATION** (`alloc_id`, `alloc_line`, `source_color`,
  `is_move_local`, `is_thread_handle`, the view set, and `pool_name` in the single-pool
  case) so they are IDENTICAL across predecessors — or the handle exists in only one pred,
  where the merge copies the whole struct (`*nh = *src`). **`escaped` is COMPENSATED, not
  merged**: the exit pass scans EVERY block's own state and builds `covered_ids` keyed on
  `alloc_id`, so an escape recorded in any block covers the allocation (probed across
  if-body / else / loop-back-edge — no false leak). **`freed_defer_id` and
  `defer_double_reported` are diagnostic dedup** and cannot change a verdict.
  **The ONE real hole was `pool_name`** (`wrong_pool_across_branch`: allocate from
  `pool_a` on one arm and `pool_b` on the other, then `pool_a.free(h)`) — and it was
  **NOT REACHABLE until CLASS 4 was fixed**, because with neither branch registering an
  allocation the predecessors never disagreed about anything. Now merged with a
  `_ir_pool_mixed` sentinel set only when both preds name a pool and the names differ.
  **Lesson worth keeping: a field-coverage count is not a hole count — ask which fields
  can DIFFER across predecessors, and whether a later pass re-derives the fact.**
- **`IRPathState.critical_depth` is not merged at all** (`pstdqk` lead B).
- **`ir_ps_copy` forces `terminated = false`, making three merge branches dead**
  (`pstdqk` lead C).
- **The analyzer READ FREED MEMORY to decide a safety verdict** (`lzmkhn`
  BUG-920/921). A use-after-free INSIDE the compiler affecting a safety answer. That
  branch also ships `tools/compiler_asan_sweep.sh` — take the tool.
- **The grammar-closure probe printed OK on a compiler that HAD a breach**
  (`ef9cao`). A gate that was lying; same class as the two gate defects in
  CLAUDE.md's "A GATE CAN ITSELF BE BROKEN".
- **`--track-cptrs`'s use-after-free half has NO call site, and the obvious fix is
  UNSOUND** (`fhf8rn` BUG-921).
- **`emit_defer_stmt` is the last raw-AST STATEMENT emitter** (`o51x9p`,
  ARCHITECTURAL).
- **`tests/zer_gaps/` triaged 23 -> 10 plus the harness oracle that let them rot**
  (`lzmkhn`) — take this wholesale.
- **reference.md**: 34 more examples compile standalone; an audit-harness defect
  manufactured five of the failures (`lzmkhn`). Baseline down to 45.
- `pstdqk` lead E (`ir_lower_interrupt` + defers) is **REFUTED, measured** — do not
  re-probe. Lead G was CONFIRMED and fixed on that branch (BUG-917).

---

### OVER-REJECTIONS found by the survey (positives main refuses)

| test | branch |
|---|---|
| `loop_counter_bounds_ok`, `vrp_empty_range_zero_trip_ok` | v7pucv |
| `comptime_width_conversions`, `return_literal_is_static_ok` | lzmkhn |
| `mmio_const_ident_base` | o51x9p |
| `funcptr_global_registry_ok` | osp1a7 |
| `cast_launder_ok` | 1zukjq |

NOT over-rejections: ~12 `xmod_*` / `contuser` / `spawnimp_user` module tests — see
harness trap 3.

**MEASURED 2026-09-04 against `6f8da82c` — the seven-item over-rejection list, triaged.**
Do not re-derive; each was run, not read.

| test | branch | status |
|---|---|---|
| `cast_launder_ok` | `1zukjq` | **CLOSED** by BUG-931 — the missing alias made a free look like a leak |
| `loop_counter_bounds_ok` | `v7pucv` | **CLOSED** by BUG-932 |
| `vrp_empty_range_zero_trip_ok` | `v7pucv` | **CLOSED** by BUG-932 — same single cause, a zero-trip loop |
| `funcptr_global_registry_ok` | `osp1a7` | **DECIDED — DO NOT "FIX".** Their whole-file rule was measured to ACCEPT an unguarded indirect call through null (no guard in the emitted C). Main's rule is the sound one and the restructure is teachable: `?u32 (*g)(u32,u32) = null;` + `if (g) \|f\|` compiles and runs. Soundness is the hard wall; over-rejection is the soft gradient |
| `mmio_const_ident_base` | `o51x9p` | live — `@inttoptr(*u32, UART)` with a `const` ident base derives no bound. Same shape as the documented `const u32 N; x % N` division case: resolve the const's init. Contained |
| `comptime_width_conversions` | `lzmkhn` | live — the comptime evaluator cannot evaluate a C-style cast in a comptime body. Contained, evaluator-local |
| `return_literal_is_static_ok` | `lzmkhn` | live — `return pick(b)` where the callee returns a LITERAL, not a view of the local. Genuine PRECISION work on the return summary (`ret_param_mask` / `call_result_static_given_args`), not a rule bug |

**The triage that matters:** two were BUGS IN A RULE (one of them mine, shipped green because
no corpus test had that shape) and were obligatory to fix; one is a DECISION already taken on
soundness grounds; two are contained gaps; ONE is real precision work. Only that last row is
a "relaxation project", and per the MAX-ORACLE standard it ranks below every remaining
accept-unsafe class.

---

### SUGGESTED ORDER (highest collapse ratio first)

1. **Enum literal door** — 10 sinks close with one predicate at the value-flow sites.
   Correct CLAUDE.md's "EXACTLY THREE / set is closed" sentence in the same commit.
2. **Cast/launder at the HEAP-UAF sink** — ~15 tests across 4 branches, one peel
   reaching the allocation-identity path + struct-literal carrier.
3. ~~**`ir_merge_states` field coverage**~~ — MEASURED, mostly refuted; the one
   real field (`pool_name`) is CLOSED with BUG-933. See the NON-TEST FINDINGS note.
4. ~~**The ASSIGN form** for alloc/move~~ — CLOSED (BUG-933); it was 11 tests and
   FIVE causes, not one registration site.
5. **The global-init self-cycle HANG** — small fix, but it is a compiler DoS and
   any negative test for it will time out `make check` until it is fixed.
6. Everything else, per class above.

## Where main stands

`make check` exit 0 — **1391 .zer**, modules 30/30, 200 fuzz, 139 convert, all ten matrices, all EIGHT
audit gates (`audit_reference_examples.sh` joined with BUG-900), sink matrix **88 cells / 0 mismatch**.

Landed this session, newest first:

| sha | what |
|---|---|
| `7c873edd` | BUG-884..888 — the call-RESULT view class (4 parts) + my container fix REPLACED + BUG-869/870/871 |
| `c0180e3f` | BUG-883 — float -> int is DEFINED as SATURATING |
| `ae033cd0` | BUG-857/858 — container SEGFAULT (superseded, see below) + recycled-slot auto-zero |
| earlier | the 45-row harvest 1, BUG-815..856 |

## DECIDED — do not re-open

**float -> int out of range SATURATES, NaN -> 0.** Owner's call, matching Rust's `as`.
The reasoning is MEMORY vs ARITHMETIC: memory violations halt (slice OOB, misaligned
`@inttoptr`, bad `@pun`); arithmetic gets a defined value, which is already why integer
overflow WRAPS. Two sub-decisions came with it and are also settled:

- the compile-time **literal** case is a WARNING, not an error — rejecting `(u32)(-1.5)`
  while the same value through a variable is defined was "two spellings disagree";
- **`@truncate` on a float STAYS REJECTED**, a deliberate divergence from `29fiao` and
  `qa249l`. `@truncate` means "keep the low bits" and a float has none, and it is
  redundant — verified: `(u32)x` saturates and `@saturate(u32,x)` is named for it, both
  giving 4294967295 for 1e20 and 0 for -1.5. Their `float_to_int_saturates` /
  `float_to_int_total` are therefore NOT taken verbatim;
  `tests/zer/float_to_int_saturates.zer` covers the same semantics.

## THE CHEAP PATH: cherry-pick, do not re-derive — this is how tracker 3 was closed

`origin/claude/vigilant-tesla-as71kk` forked at `9f8cda08` (one behind main) and carried
better implementations than the catalog entries described. **It cherry-picked.** Every
commit is now TAKEN; the table is the record of what each cost.

The lesson for the next branch: **read the fork point first.** A branch that forked at or
near main is auditing the CURRENT compiler and its findings are live; one that forked
earlier will overlap heavily with whatever landed since, and its BUG numbers will collide.
Of the nine branches, only `r1piyr` and `as71kk` forked near main, and they produced
almost everything that survived.

| commit | rows | files | conflicts with main? |
|---|---|---|---|
| ~~`ccdd49ee`~~ TAKEN | **A2** @cstr, **A3** sign/`value_flows_to`, **A4** enum siblings (861 = already on main) | checker.c emitter.c | ONE conflict in checker.c, resolved to HEAD (purely additive on both sides). Its 861 `memset` had to be DELETED — main's BUG-858 already emits it, so the merge doubled it in both allocators. It also carried SEVEN tracked ELF binaries; dropped. |
| ~~`dda5b33b`~~ TAKEN | **A5** `?T` compare, **A6** non-null drift, **A7** inline-array free | checker.c test_emit.c | ONE conflict in checker.c, resolved to HEAD. It is what finally USES `nonnull_zero_hole` / `is_null_sentinel_ck`, which had arrived dead with `a22de320`. |
| ~~`6af2497e`~~ TAKEN | **A8** indirect-call keep carrier | checker.c | none |
| ~~`1cb1f93e`~~ TAKEN | **A14** `lower_expr`, dead code, warnings 32 -> 0 | many | NONE. Measured on main: 28 warnings before, 0 after. |
| ~~`a1919aa5`~~ TAKEN | **A9** six intrinsics storing through const | checker.c | none |
| ~~`0a0ec95e`~~ TAKEN | **A15** short-circuit, **A16** const slice, **A17** const array size | checker.c checker.h | none. Also fixes `audit_walker_fields.sh` membership (prose -> structural), baseline 758 -> 684. |
| ~~`e937a1d8`~~ TAKEN | **A18** funcptr arrays | checker.c emitter.c parser.c parser.h | none. Brings `audit_reference_examples.sh` and WIRES it into `make check` — verified non-vacuous (RED on the pre-fix compiler). |
| ~~`0828d1f7`~~ TAKEN | **A19** bit extraction through a pointer | checker.c | none |
| ~~`15b4fe49`~~ TAKEN | **A13** `@trap` arity + `ubsan_sweep.sh` | checker.c | none |
| ~~`38c27c18` `0ff11690` `d2cd8394`~~ TAKEN | tooling: `ub_sweep.sh`, the rc_cond_004 flake, structural `.gitignore` | — | `d2cd8394` conflicts modify/delete on the binaries it exists to remove; resolved by deleting. The flake did NOT reproduce here (16/16 on a pre-fix build) — the race is real by construction, not by measurement on this box. |

Recipe that worked twice today:

```
git cherry-pick --no-commit <sha>
# resolve any conflict by taking THEIRS unless main's version is strictly better
rm -f *.o src/safety/*.o && make zerc          # ALWAYS after a .h edit
# run the branch's own tests, then make check
```

`f63bddcb` (the view class) applied with **zero** conflicts because it touches only
`zercheck.h` / `zercheck_ir.c`. `a22de320` conflicted twice in `checker.c` and both
resolved to "theirs".

## Verification that must not be skipped

1. **Reproduce on main FIRST.** Read the DIAGNOSTIC, never the exit code — `-o out.c`
   isolates the checker's verdict from GCC's, and the echoed source line contains the
   offending text, which defeats a loose grep.
2. **Build a pre-fix compiler and prove the test flips**:
   `git archive <prev-sha> | tar -x -C $S && (cd $S && rm -f *.o src/safety/*.o && make zerc)`
3. **A new gate must be shown to FIRE**, not merely pass. `bash tools/sink_matrix.sh $S/zerc`
   is the pattern — p18 reported 7 holes + 2 over-rejects pre-fix, 0 after.
4. `MAKE_CHECK_EXIT` must be echoed. Make aborts at the first failing audit, so a later
   gate silently never runs.

## Corrections I made to MY OWN work — do not repeat these

- **O3 / H14 (`&&` narrowing).** I closed it as "precision only" after probing an
  UNPROVABLE index, which correctly guards. With a PROVABLE index the `&&` spelling is
  HARD-REJECTED while the nested-if spelling compiles. My own tracker had predicted this
  ("becomes required if the always-OOB verdict is ever promoted") and I never checked
  that BUG-796 had already promoted it. Still open as **A15**.
- **BUG-857 (container cycle).** My fix stopped the SEGFAULT and traded it for an
  OVER-REJECTION of a pointer-broken cycle. Replaced by as71kk's BUG-868 in `7c873edd`.
  The rule: a cycle is infinite only if EVERY edge is by-value.
- **The view class, partial.** I wrote the arg-FORM half and reverted it. Correct call:
  the ARITY part turns `returns_param_color` into a MASK precisely because the BLOCK-TAG
  bug lets the int name the WRONG param, so widening the consumer alone makes the wrong
  answer apply more often. Landed whole in `7c873edd`.
- **I briefly blamed my own change** for three view-class boundary over-rejections that
  predated it. Check before attributing.

## Two gates that were themselves broken — the general lesson

`audit_walker_fields.sh` (which I installed) decided membership on **prose**, counting
the function's own name inside comments, so editing a comment moved a walker in or out of
the audited set. as71kk fixes it; baseline 758 -> 684.

as71kk's qualifier probe's **first oracle was vacuous** — `-Wdiscarded-qualifiers` is
silenced by the explicit cast ZER's emitter writes, so it printed OK against a
deliberately broken compiler. And its `ubsan_sweep.sh` needed `float-cast-overflow` named
explicitly, because it is NOT in GCC's `-fsanitize=undefined` — found by breaking the
BUG-845 guard and watching the sweep report CLEAN on the exact class it exists to find.

**A gate that has only ever printed OK is a script, not a net.** Both of these were
caught by deliberately breaking the compiler and checking the gate went red.

## The two closure probes are MEASUREMENT SCRIPTS, not gates — do not count them as coverage

`tools/grammar_closure_probe.sh` and `tools/qualifier_closure_probe.sh` landed with
BUG-896 and are **not wired into `make check`** — neither on main nor on `as71kk`, which
wires only `audit_reference_examples.sh`. They are slow full-intrinsic sweeps meant to be
run by hand when the intrinsic set or a qualifier rule changes.

Recording this because CLAUDE.md's own rule is that an un-executed test artifact decays to
zero value and then to NEGATIVE value once someone believes it covers something. A future
session seeing `qualifier_closure_probe.sh` in `tools/` must not conclude qualifier
closure is gated. It is measured, once, by whoever runs it.

`tools/qualifier_closure_baseline.txt` is the useful half: 25 justified strips, each
established by READING the emitted C (the pointer's value goes to an `"r"` asm operand
with a `"memory"` clobber, never a dereference), with the discriminating question written
down for the next reader. The CONST half is not baselined — those six became BUG-896.

## Recommended order — NOTHING REMAINS (kept as the record of what was done)

1. ~~**A3**, **A2**, **A4**~~ — DONE 2026-08-26 (BUG-889..891, `ccdd49ee`). `value_flows_to`
   is in; the eight negative-constant sinks each have their own test, which is the only way
   to prove one query really replaced all eight.
2. ~~**A5**, **A6**, **A7**, **A8**, **A9**~~ — ALL DONE 2026-08-26 (BUG-892..896,
   `dda5b33b` / `6af2497e` / `a1919aa5`). **Every accept-unsafe row in tracker 3 is closed.**
3. ~~**A15..A19**~~ — ALL DONE 2026-08-26 (BUG-897..901, `0a0ec95e` / `e937a1d8` /
   `0828d1f7`, no conflicts). A15 was my mis-closed row.
4. ~~**A13**, **A14**~~ — DONE 2026-08-26 (BUG-902/903, `15b4fe49` / `1cb1f93e`).
5. ~~Tooling~~ — ALL DONE 2026-08-26. `ubsan_sweep.sh` + `ub_sweep.sh` (measurement
   scripts, see the note above); the walker-field membership fix (758 -> 684, rode with
   BUG-897); the structural `.gitignore` (1084 binaries untracked, all verified ELF); the
   rc_cond_004 flake.

**HARVEST TRACKER 3 IS CLOSED — all 20 rows.** `as71kk` is fully consumed; nothing
remains to cherry-pick from it.

Still parked, deliberately, with reasons recorded in their own entries below: the comptime
array-element width, the extern `*opaque` type_id (both candidate fixes risk an
under-rejection), and `29fiao`'s BUG-852 (measured at 34 correct corpus files, so the
blanket rule was NOT shipped).

---

## OPEN — the four 2026-08-11 residuals, all measured 2026-08-16

Two CLOSED, one CONFIRMED LIVE with a precise narrowing, one confirmed live and awaiting a
POLICY decision. **Every probe below was routed around its masking rule** — the first pass
scored two of them "closed" on masked evidence.

### ~~ISR non-atomic RMW laundered through a pointer parameter~~ — **CLOSED (measured)**

`interrupt { rmw(&g); }` with `void rmw(*C p){ p.v += 1; }` is rejected on both routes:
with a plain param the QUALIFIER gate fires (*"cannot pass volatile pointer to non-volatile
param"*); with a `volatile *C` param the ISR rule fires (*"volatile global 'g' is shared
between interrupt and main"*). The hazard has no unblocked path.
**PROBE WARNING:** the obvious reproducer uses `p[0] += 1`, which is rejected by the
single-pointer-indexing rule and masks both. Use a struct field (`p.v += 1`).

### ~~`free`-of-a-param-field through an UNWRAP-TO-LOCAL~~ — **CLOSED 2026-08-16 (BUG-785)**

`lp = h.b` now registers an alias with the compound `(param, ".b")`, so a free propagates in
BOTH directions and the cross-function `frees_param_field` summary sees it. The existing
FIELD_READ alias arm linked the dest to the BASE object's handle; a by-value struct param
carries no allocation identity on the base — it lives on the COMPOUND — so the arm silently
did nothing. Added a fallback that mints a shared `alloc_id` on the compound and aliases the
unwrapped local into that group. Same POINTER-CARRYING gate as the existing arm, which is
what keeps `test_modules/move_user` green (verified: 0 errors).
Tests: `tests/zer_fail/frees_param_field_unwrap{,_reverse}.zer` +
`tests/zer/field_read_scalar_no_alias_ok.zer` (the scalar canary).

### (superseded) CONFIRMED LIVE — `free`-of-a-param-field through an UNWRAP-TO-LOCAL

The entry marked *"CLOSED 2026-08-08"* IS incomplete, as `2hg2v4` reported. Narrowed:

| shape | verdict |
|---|---|
| `void fb(H h){ free(h.b); }` — direct | **caught** |
| `void fb(H h){ [*]B lp = h.b; free(lp); }` | **LIVE** |
| `void fb(H h){ [*]B a = h.b; [*]B c = a; free(c); }` | **LIVE** |

**Root cause, measured — it is WIDER than the summary.** The summary scan
(`zercheck_ir.c` ~6220) looks for a compound handle rooted at the param
(`ch->local_id == plocal && ch->path_len > 0`). An unwrap-to-local frees a DIFFERENT local,
so the compound is never marked. But the alias is missing INTRA-function too, in BOTH
directions: `free(h.b); use(lp)` is also accepted. So `lp = h.b` registers no alias with the
compound at all — fixing only the summary scan would leave the intra-function half open.

**Fix shape:** register a var-decl/COPY whose source is a POINTER-CARRYING field of a
by-value struct param as an alias of the compound `(plocal, ".field")`, so the existing
alias-group propagation carries frees both ways. **DANGER — this is the exact class that
broke `test_modules/move_user` TWICE** (see CLAUDE.md "The pointer-vs-scalar refinement"):
the arm MUST be gated on the field being pointer-carrying, or a scalar field read
(`u32 v = t.id;`) inherits the parent's transferred/freed state.
**PROBE WARNING:** two of the three shapes are MASKED by the LEAK rule
(*"handle %0 (local 'h') allocated ... never freed"*) — read the reason, not the exit code.
The unmasked discriminator is `free(h.b); use(lp);`.

### ~~`&packed_field` forms a possibly-misaligned `*T`~~ — **CLOSED 2026-08-16 (BUG-786)**

**It was never a policy question — ZER had already decided it.** `tests/zer_fail/atomic_packed_field.zer`
(BUG-493) rejects `@atomic_add(&r.counter)` on a packed field precisely because it is misaligned
on ARM/RISC-V. The same question simply had no answer at the other sinks, which is the
one-question-many-sinks pattern, not an open design choice.

Resolved by TRACKING, per the Ban Decision Framework (rule 5: none of the four ban conditions
applies, so track). New `Symbol.is_packed_derived` — a Model-4 static annotation on the same
rails as `volatile`/`const` — set when a var-decl binds `&packed.field`, and consumed at the
DEREF sink. Index was already covered by the `*T`-indexing rule; atomics stay on BUG-493.

Measured: the emitter drops the packed-ness entirely (`uint32_t* _zer_t0 = &p.b;`), so nothing
downstream could recover it. Corpus cost ONE file — the gap reproducer, now promoted to
`tests/zer_fail/packed_field_addr_deref.zer`. Direct access `p.field` is unaffected and is what
the diagnostic points users to; pinned by `tests/zer/packed_field_direct_ok.zer`.

### (superseded) CONFIRMED LIVE — `&packed_field`

`packed struct P { u8 a; u32 b; } ... *u32 q = &p.b;` compiles with no diagnostic. The
non-packed control behaves identically, which is the point: the compiler cannot distinguish
them at the `&` site today.
**PROBE WARNING:** `return q[0]` masks this via the single-pointer-indexing rule; use `*q`.
**Still an owner decision** — ban `&` on a packed field, require an explicit unaligned-load
intrinsic, or accept with a diagnostic. Not something to settle by patching.

### deref-launder residual (LOW, latent)

The escape peel takes an intrinsic's LAST argument, so a hypothetical single-arg
pointer-taking intrinsic with a SCALAR result would mis-mark its result local-derived. The
one live instance (`@atomic_load(&local)`) is now unreachable — BUG-784 rejects atomics on a
stack local first. `@ptrtoint` and `@probe` were checked and are unaffected. Fixing it would
be a RELAXATION (the dangerous direction) for no currently-reachable gain.

---

## OPEN — concurrency sweep 2026-08-10: NO holes found; two doc/impl mismatches corrected

Systematic probe of the concurrency surface after BUG-783. **No accept-unsafe hole found.**
Recorded so a fresh session does not re-run it blind, and because the sweep's own METHOD
failed twice before it produced a trustworthy answer.

### Probe-harness failures that produced FALSE "holes" (read before writing a sweep)

1. **The detector missed `zercheck:` diagnostics.** The helper grepped `': error'`, which
   matches neither `file:3: zercheck: ...` nor a line starting `error:`. ThreadHandle-not-joined
   was reported as an accepted hole when it is caught. **Grep `-E 'error|zercheck:'`.**
2. **Three probes did not exercise their rule.** `volatile u64` from a spawn needs
   `--target-bits 32` (on a 64-bit target a u64 IS single-word, so acceptance is correct);
   the Ring pointer-element probe used a struct VALUE element; the async probe used a shape
   the rule deliberately allows. Each looked like a hole and was a malformed probe.

### Verified CLOSED by this sweep (each caught, correct reason)

ThreadHandle never joined; ISR non-atomic RMW via a pointer param; scoped-borrow write between
spawn/join; scoped-borrow CROSS-BLOCK; `threadlocal &` to a spawn; Handle nested in an optional
struct to a spawn; same-statement two-shared-type deadlock; spawn inside `@critical`; global
Pool from a spawn body; `slab.alloc` in an ISR; `@cond_wait` on a foreign shared struct;
`volatile u64` from a spawn at `--target-bits 32`; move-struct through a spawn arg; Ring push of
a local-derived pointer; shared read in an `await` condition; shared read in a `yield` statement.

### Doc/impl mismatch #1 — CORRECTED (CLAUDE.md)

CLAUDE.md's safety table said *"Shared struct field access inside async function -> compile
error"*. **That overstates the rule.** The real D02 ban is *shared access in a statement
CONTAINING yield/await* (checker.c ~7496, gated on `in_async_yield_stmt`), because locking is
PER-STATEMENT: `x = g.v; yield;` releases the lock before the suspend and is correctly
ACCEPTED. The row now states the real rule. Left as-is this would have sent a session hunting
a non-bug — or "fixing" a correct acceptance into an over-rejection.

### ~~Doc/impl mismatch #2~~ — **RESOLVED 2026-08-10: the DOCS were wrong, the compiler is right**

Asked to "enforce the docs", measuring first showed enforcement is **impossible and would be
wrong**:
- **`*shared T` is NOT CONSTRUCTIBLE.** `shared` is a struct-only qualifier (`shared u32 g;`
  -> *"expected 'struct' after 'shared'"*) and you cannot take the address of a shared
  struct's interior (*"cannot take address of a shared struct's interior"*). There is no way
  to build the documented operand, so enforcing it would reject EVERY `@atomic_*` call with
  no legal alternative.
- **Corpus: 46 of 48** atomic-using test files operate on a plain global, including the whole
  `rt_conc_atomic_*` Rust-translation set and the atomic-cell tests BUG-769/771 depend on.
- **It inverts the feature.** `shared struct` is a MUTEX; requiring one to perform an atomic
  operation means taking a lock to do lock-free programming.
- **The plain-global form is already sound**: a real atomic instruction is emitted, and the
  ATOMIC CELL rule rejects any MIXED plain+atomic access (that is exactly what BUG-769/771
  were), so all-atomic access to a plain global has no race.

**Everything ELSE in that doc sentence IS enforced** — verified: width must be 1/2/4/8 bytes
(`u128` and a non-native `u24` both rejected with that exact message) and the operand must be
an integer (`f32` and a struct both rejected). Only the `*shared T` phrase was wrong.

Docs corrected in `docs/reference.md` and `CLAUDE.md` to state the real contract. Pinned by
`tests/zer_fail/atomic_width_u128.zer` and `tests/zer/atomic_plain_global_contract_ok.zer`
(1/2/4/8-byte globals + a struct field + load/store round-trip).

### (superseded) original entry

`@atomic_load(&g)` on a **plain non-shared** global compiles clean. The docs describe atomics
as requiring a `*shared T` operand. Probed: this is **not a race** — the atomic-cell rule
(BUG-769/771) catches any MIXED plain+atomic access, so a purely-atomic access to a plain
global is sound, and the emitted code is a real atomic instruction. So the question is
whether to (a) enforce the documented `*shared T` requirement (a tightening; would reject
working code), or (b) relax the docs to match. **Not a safety issue either way** — recorded so
it is not re-probed as a suspected hole.

---

## OPEN — residuals after the 2026-08-10 survey (measured; reproducers in `tests/zer_gaps/`)

The 9 survey fixes + the 8th funcptr REACH form all landed. These are what the three
branches documented but did NOT fix, each **re-verified live on main 2026-08-10**.

### ~~HIGH ACCEPT-UNSAFE — pointer-deref var-decl alias (`*T k = *pp;`)~~ — **CLOSED 2026-08-10 (BUG-781)**

Closed by REJECTING the form, not by tracking it — the Level-A stance: when the analyzer
cannot prove which allocation an alias refers to, reject. Resolving `*pp` needs points-to
over a pointer-to-pointer, which the per-file + summaries model deliberately lacks, so
there is nothing to prove it WITH. **Cost measured before shipping: ZERO instances in the
whole corpus** (tests/zer, zer_fail, test_modules, rust_tests, zig_tests) — every
deref-init var-decl there is a SCALAR or struct-VALUE copy, which the predicate does not
match because it requires a POINTER-typed result. Restructure is one line and teachable
(`*T k = p;`), which is the bar CLAUDE.md sets for an acceptable false positive.
Tests: `tests/zer_fail/deref_alias_uaf_double_free.zer` + `tests/zer/deref_scalar_ok.zer`.

**Enumerating first found TWO more live forms the branch had not recorded** — deref into a
struct FIELD (`h.p = *pp;`) and deref RETURNED through a function (`return *pp;`). Both
still ACCEPTED; the var-decl sink is closed, those two sinks are not. Same predicate
(`deref_ptr_launder`) applies — wire it at the assignment and return sinks. Kept OPEN below
rather than claimed closed, because a partial fix that promotes the gap file is exactly the
false-confidence failure this ledger exists to prevent.

### ~~HIGH ACCEPT-UNSAFE (RESIDUAL) — deref-launder at the FIELD-STORE and RETURN sinks~~ — **CLOSED 2026-08-10 (BUG-782)**

`h.p = *pp;` and `*Node leak(**Node pp) { return *pp; }` now reject via the same
`deref_ptr_launder` predicate. **All three sinks of this class are closed** (var-decl,
field-store/assign, return). Tests: `tests/zer_fail/deref_alias_{uaf_double_free,field_store,return}.zer`;
`tests/zer/deref_scalar_ok.zer` pins the over-rejection boundary across all three sinks
(scalar var-decl, scalar assign, struct-VALUE copy, scalar return).

### ~~(superseded — original entry text follows)~~ pointer-deref var-decl alias

`tests/zer_gaps/deref_alias_uaf_double_free.zer`. **Measured live: the program RUNS and
returns 7** — it reads `n.v` after `free(k)` (stale value) and then double-frees. Heap facet
of HOLE-A4.

**Narrowed precisely 2026-08-10 — the gap is ONE form, not the whole class:**

| shape | verdict on main |
|---|---|
| `*Node k = n; free(k); n.v` — direct alias | **caught** |
| `*Node k = n; k = *pp;` — ASSIGN with a deref RHS | **caught** (k already had a handle) |
| `*Node k = *pp;` — VAR-DECL with a deref init | **ACCEPTED — the hole** |
| `... free(n); k.v` — same, use through k | **ACCEPTED** |

**Why the obvious patch is wrong.** In the IR the deref lowers to `%8 = UNOP <expr>` and the
var-decl to `%7 = COPY` from it; the UNOP temp carries no handle, so the copy inherits
nothing. Making the COPY inherit requires resolving `*pp` -> `pp` -> its init `&n` -> `n`'s
handle. That is a POINTS-TO question over a pointer-to-pointer, which the per-file +
summaries model deliberately does not have (whole-program analysis is banned from the
architecture). The enabling step is `&n` on a local that holds a tracked allocation.

**Two sound directions, both needing design rather than a patch:**
1. *Syntactic resolution* (cheap, narrow): if a var-decl init is `*X` and `X`'s own
   var-decl init was `&Y` (a bare ident, not reassigned anywhere — reuse
   `ast_name_mutated_or_addrd` as the stability gate), alias the new local to `Y`'s handle.
   Same shape as the funcptr local-binding resolution in `00dc785a`. Covers the reproducer;
   does NOT cover a reassigned or computed `pp`.
2. *Conservative barrier* (broad, needs care): taking `&n` where `n` holds a tracked
   allocation means aliases exist that the analyzer cannot follow — apply the
   argument-precise barrier principle and widen `n` to MAYBE_FREED on any free through an
   unresolved deref. Touches the CFG fixpoint; measure over-rejection first.

### MEDIUM silent bare-metal — non-atomic RMW in an ISR laundered through a pointer parameter

Sibling of BUG-774: the ISR compound-RMW check is intra-body, so `interrupt { rmw(&g); }`
with `void rmw(*u32 p) { p[0] += 1; }` is unchecked. The transitive machinery from BUG-774
(`record_isr_globals` following calls) exists; what is missing is propagating the
*compound-assign* fact through a pointer PARAM rather than a global name.

### MEDIUM (POLICY, not a bug) — `&packed_field` forms a misaligned `*T`

`tests/zer_gaps/packed_field_addr_misaligned.zer`. Confirmed + UBSan-reproduced.
**Deferred pending an owner decision**: whether ZER permits forming a possibly-misaligned
pointer at all. Options: ban `&` on a packed field, require an explicit unaligned-load
intrinsic, or accept with a diagnostic. Not something to settle by patching.

### LOW bare-metal — `volatile` lost through a `@ptrtoint -> @inttoptr` round-trip

`tests/zer_gaps/volatile_stripped_ptrtoint_roundtrip.zer`. Audit-visible (both intrinsics
are explicit at the use site), so it is a qualifier-propagation gap, not a silent one.

### Cross-references (tracked elsewhere, do NOT duplicate)

- **frees-param-field via UNWRAP-TO-LOCAL** (`2hg2v4`) — extends the frees-param-field entry
  marked "CLOSED 2026-08-08"; **that closure is INCOMPLETE.** Re-probe before trusting it.
- **comptime / const integer fold ignores per-operation width wrapping** (`2hg2v4`) —
  value-correctness, explicitly probed and found NOT a safety hole (checker and emitter read
  the same folded value). Deferred: the fix threads a destination width through the const
  evaluator.
- **`naked` attribute dropped on the IR path**, **`vrp_ir.c` dead code** (Phase 0 of
  `docs/unified-oracle-proved-ZER.md`), **ISR funcptr FIELD call not bound in a same-scope
  struct init** (the residual after BUG-774) — all already have entries.

---

## OPEN — branch survey 2026-08-10: 9 verified fixes to implement (`p0w5lj` / `2hg2v4` / `l3vn1i`) + a LIVE `make check` breakage

**READ THIS FIRST — `make check` IS CURRENTLY BROKEN AND HAS BEEN SINCE `65af3864`.**
`MAKE_CHECK_EXIT=2`. Make aborts at the walker-default-clause audit, so **five gates never
run**: fixed-buffer, type-dispatch, carrier-dispatch, emit-audit, and the sink matrix.
Cause: the G3 fix (`65af3864`) shipped `record_atomic_plain_in_callee` with a
`default: return;` in a `switch (node->kind)`, which CLAUDE.md forbids. **Fix P3 below closes
both the coverage hole and the build breakage — land it FIRST or nothing else can be verified.**

**HOW IT WAS MISSED, so nobody repeats it.** The verification grep was `grep -E 'OK — no'`,
which matches **"OK — no gaps. IR emitter covers every node kind…"** — a DIFFERENT audit that
runs EARLIER. That line was present, so four commits (`65af3864`, `88c012ac`, `dfccd3f5`,
`d995dbac`, `36404262`) were reported "make check exit 0, all gates" when make had actually
aborted. CLAUDE.md already warns *"exit=2 with those lines ABSENT means an earlier step
aborted make before the audits"* — the trap is that a DIFFERENT audit's OK line satisfies a
loose grep. **Grep for the SPECIFIC line (`no default: clauses`), and always echo the real
`MAKE_CHECK_EXIT`.**

---

**Survey scope.** Three `claude/gifted-noether-*` branches, one squashed commit each, forked
from recent main (`65af3864` / `ac97e11a` / `6f6d9186`). **Every fix below was independently
reproduced against current main before being listed** — mostly by running the branch's OWN
negative test with main's `zerc`. Nothing is repeated on a branch's word alone. NOT merged or
pulled; implement from these descriptions.

### The 9 fixes to take

| # | fix | source | how verified on main |
|---|---|---|---|
| **P3** | G3 walker `default:` skips 9 child-carriers **+ unbreaks `make check`** | `p0w5lj` | `atomic_cell_plain_via_switch_arm.zer` ACCEPTED |
| **L1** | `@container` launders a stack pointer past escape sinks | **`l3vn1i`** | `container_var_escape_global.zer` ACCEPTED |
| **L2** | `find_return_range` misses a return buried in a NESTED orelse | `l3vn1i` | 3 `zer_trap` files exit 0 (guard elided) |
| **L3** | ISR dispatch through a funcptr never scanned (3 idioms) | `l3vn1i` | 3 negatives ACCEPTED |
| **P1** | `return { .p = &local }` struct-literal escape at the RETURN sink | `p0w5lj` | `return_struct_init_local_escape.zer` ACCEPTED |
| **P2** | ISR body never resets the VarRange map -> elided bounds guard | `p0w5lj` | emit-C only (invisible on hosted) |
| **H1** | funcptr ARRAY-ELEMENT field callback `o.fns[0]()` spawn race | `2hg2v4` | `spawn_funcptr_array_field_race.zer` ACCEPTED |
| **H3** | uN/iN SHIFT result not width-wrapped on the AST-passthrough store path | `2hg2v4` | branch's `uN_width_wrap_all_forms.zer` exits 40 |
| **H4** | `@bitcast` / `@truncate`-to-uN in a global initializer emit illegal C | `2hg2v4` | both negatives ACCEPTED |

---

**P3 — the G3 atomic-cell walker shipped with a `default:` (`p0w5lj`). CRITICAL + build-breaking.**
`record_atomic_plain_in_callee` (checker.c, added `65af3864`) used `default: return;`, silently
skipping **switch / defer / orelse / do-while / @critical / @once / spawn / slice / struct-init**
child-carriers. A plain atomic-cell access inside any of those, in a helper called after a
fire-and-forget spawn, is not recorded and the race slips. Fix: enumerate the switch (descend the
child-carriers, explicit no-op leaves/decls, NO `default:`). This is a partial walk by intent, so
the no-op cases must be listed explicitly — that is exactly what the audit demands.
Test: `tests/zer_fail/atomic_cell_plain_via_switch_arm.zer`.

**L1 — `@container` launders a stack pointer past the escape sinks. CRITICAL (ASan stack-UAF).**
**DUPLICATE — `2hg2v4` also fixes this (its BUG-769, 3 sinks). TAKE `l3vn1i`'s:** it covers
**4** sinks (store-global, struct-field-of-global, return, keep), routes all **six** escape peels
through the shared `unwrap_ptr_launder`, peels launder intrinsics at the top of
`arg_is_local_derived`, **and fixes a latent sibling** — `unwrap_ptr_launder` peeled
`@cstr(buf, str)` to the string literal instead of its `args[0]` buffer.
Root cause: `@container(*T, ptr, field)`'s pointer is **args[0]**, but the sinks hand-rolled a
LAST-arg intrinsic unwrap (last arg = the field NAME), so `is_local_derived` was never consulted.
**PROBE WARNING — one of the branch's own negatives is MASKED:** `container_var_escape_return.zer`
IS rejected on main, but by the LEAK rule (`handle %0 (local 'd') allocated…`), not the escape
rule. Reading the exit code alone scores this "already fixed". Check the REASON.

**L2 — a return buried in a NESTED orelse fallback is dropped from the return range. CRITICAL
(ASan global-buffer-overflow).** The earlier B9 fix only saw a TOP-LEVEL orelse. These three
shapes drop the buried return, so the caller elides `arr[callee()]`'s guard:
`(mb() orelse { return 9; }) + b`, the same inside a call arg, and inside a return-expr wrapper.
Fix: `scan_expr_orelse_returns`, a recursive expr walker wired into the `NODE_RETURN` handler and
the general block. Conservative — it can only ADD guards.
Tests: `tests/zer_trap/vrp_buried_orelse_{binary,callarg,return}.zer` (all exit 0 on main = guard
elided = the bug).

**L3 — ISR dispatch through a function pointer is never scanned. HIGH (silent bare-metal race).**
The ISR sibling of the spawn funcptr family (`5ed17c2f`, `00dc785a`, `ac97e11a`) — the pattern
predicted and confirmed: spawn side fixed three times, ISR side never. A non-volatile global
touched only through an ISR-dispatched funcptr is invisible, and GCC `-O2` is free to hoist/tear
it. Three idioms now followed from `record_isr_globals` to the target body: a **global funcptr
variable**, a **funcptr field in a struct init**, and a **function name passed as an arg**.
Tests: `tests/zer_fail/isr_funcptr_{global_var,struct_field,arg}.zer` — all three ACCEPTED on main.

**P1 — `return { .p = &local }` dangles. HIGH.** A struct/union **literal** returned BY VALUE
carrying a pointer/slice to a frame-local. The var-decl and assign-to-global sinks already ran
`struct_init_has_local_derived`; the RETURN sink was the missing sibling (the multi-site class
again). Gate on `type_carries_data_pointer`. `p0w5lj` also adds sink-matrix shape **p14**
(3 reject + 2 safe cells) — take the matrix cells with the fix.
Tests: `return_struct_init_local_escape.zer` + two positives (`_global_ptr_ok`, `_param_ptr_ok`).

**P2 — an ISR body never resets the name-keyed VarRange map. HIGH (silent bare-metal OOB).**
Only `func_decl` reset it (per SS-C#15). A narrowed range from the preceding declaration leaks
into the `interrupt {}` body, marks a fixed-array index "proven", and elides the bounds
auto-guard -> stack OOB. One-line mirror of the `func_decl` reset.
**TEST-AUTHORING NOTE:** invisible on hosted x86 and an `interrupt` block cannot appear in a
runnable `tests/zer/` positive (GCC refuses ISRs on hosted x86-64). Verify **emit-C-only**
(`zerc f.zer -o f.c`) and say so in the commit.

**H1 — funcptr ARRAY-ELEMENT field callback evades the spawn race scan. HIGH.**
`o.fns[0]()` bound in the CALLER was accepted while the scalar sibling `o.cb()` was rejected:
`body_calls_funcptr_field` / `scan_funcptr_field_bindings` matched only a bare `NODE_FIELD`
callee/target, never `NODE_INDEX`-over-`FIELD`. Fix: a recursive `funcptr_field_access()` helper
used at BOTH sites. **This is a 7th form of the funcptr REACH class** — add its cell to the REACH
grid in `tests/test_conc_matrix.c` in the same commit, or it is invisible.

**H3 — uN/iN SHIFT result is not width-wrapped on the AST-passthrough store path. HIGH
(silent miscompile).** `u3 3<<2` gives 12, not 4; iN keeps the wrong sign. The G3 self-mask
EXCLUDED shifts, and `_zer_shl`/`_zer_shr` only guard shift-by->=carrier-width — they never mask
to N. var-decl / return / call-arg were safe (IR_BINOP + `emit_intn_mask`); **plain assign,
global store, struct-field, array-elem, and shift-feeding-div/mod/shr were not.** Fix: wrap via
`emit_intn_mask_lv`, mirroring the IR path. Take the branch's EXTENDED
`tests/zer/uN_width_wrap_all_forms.zer` (main already has the file; the branch appends the shift
forms — it exits **40** on main).

**H4 — `@bitcast` and `@truncate`-to-uN in a global initializer emit illegal C. MEDIUM (LOUD).**
The checker accepts them but the emitter produces a statement-expression, which is illegal at
file scope -> a GCC error the user cannot act on. Add both to the G7 global-init reject list so
the diagnostic comes from the checker. A native `@truncate` global still compiles.

### Suggested order

**P3 FIRST** — until it lands, `make check` aborts before five gates and the sink matrix, so no
subsequent fix can be properly verified. Then **L1 -> L2 -> L3** (largest safety surface, all
CRITICAL/HIGH), then **P1 -> P2**, then **H1 -> H3 -> H4**.

### Also on these branches: documented-but-NOT-fixed (verify before chasing)

From `p0w5lj` (5 reproducers added to `tests/zer_gaps/` with tripwires):
- **HIGH** cross-thread race via a **CALLER-supplied funcptr forwarded to `spawn`**
  (`spawn_funcptr_forwarded_param_race.zer`) — an 8th REACH form; the target's funcptr comes in
  as a PARAMETER, so no binding is visible in the file.
- **HIGH** pointer-deref var-decl alias (`*T k = *pp;`) untracked -> UAF + double-free. This is
  the HOLE-A4 heap facet, also confirmed independently by `2hg2v4`.
- **MEDIUM** non-atomic RMW in an ISR laundered through a pointer parameter.
- **MEDIUM** `&packed_field` misalignment (already tracked here; policy decision pending).
- **LOW** `volatile` lost through a `@ptrtoint -> @inttoptr` round-trip (audit-visible).

From `2hg2v4`:
- **CONFIRMED silent miscompile** — comptime/const integer fold ignores per-operation width
  wrapping. Computes in host `int64` and masks only the final result. Explicitly probed and found
  NOT a safety hole (checker and emitter read the same folded value), but a VALUE-correctness bug.
  Deferred because the correct fix threads a destination width through the const evaluator.
- **CONFIRMED accept-unsafe** — frees-param-field via UNWRAP-TO-LOCAL, extending the
  "CLOSED 2026-08-08" frees-param-field entry. **That earlier closure is therefore incomplete.**
- **LOW over-rejection** — a function returning a raw `*T` view of a global flagged as a leaked handle.
- **SUSPICION (not confirmed)** — `@atomic_load(&g)` on a plain non-shared global compiles clean.

From `l3vn1i`:
- ~~ISR funcptr FIELD *call* / factory-return residual after L3~~ — **CLOSED 2026-08-10
  (BUG-783).** All NINE reach forms are now covered at the ISR sink. Measuring first
  corrected the premise: seven were ALREADY caught (`record_isr_globals` descends var-decl
  inits, assignments and struct-init fields generically), and only the two FACTORY-return
  forms were live. Gated by a 9-cell ISR sub-grid in `tests/test_conc_matrix.c`
  (conc-matrix 84 cells, verified firing at 82/84 pre-fix).
- `naked` attribute silently dropped on the IR path (confirmed still live).
- `vrp_ir.c` DEAD CODE (349 lines) — cross-ref: this is Phase 0 of
  `docs/unified-oracle-proved-ZER.md`.

---

## OPEN — `Pool`/`Slab`/`Ring` as a container FIELD reports "undefined type 'T'" (LOW, message quality)

**Symptom.** `container C(T) { Pool(T,4) v; }` is rejected with `error: undefined type 'T'`.

**Status — the rejection is CORRECT, the message is not.** The emitter cannot stamp their
inline storage for a monomorphized container, so accepting them would produce a broken
emission rather than a working feature. But they are rejected because `subst_typenode` leaves
them unsubstituted, not by a deliberate check — so the diagnostic blames the type parameter
instead of naming the real restriction.

`Handle(T)` was in the same leaf group and was FIXED 2026-08-06: a Handle is a `u64`
(index + generation), so the stamped struct needs no per-T layout. That is exactly the
property Pool/Slab/Ring lack.

**Fix sketch.** Either add the emitter support, or add an explicit check in the container-field
validator that names the restriction ("Pool/Slab/Ring cannot be a container field — declare the
allocator as a global and store `Handle(T)`"). The second is a few lines and is what a user
actually needs. Do NOT simply add substitution in `subst_typenode` — the comment there records
why, and doing so would swap a clear type error for a broken emission.

**Documented for users** in `docs/reference.md` (container section), so the restriction is at
least discoverable while the message is poor.

---

## OPEN — scoped-borrow: a join on EVERY branch arm is over-rejected (LOW, precision)

**Symptom.** Joining on every arm of a branch and then using the borrowed local is
safe in reality, but rejected:

```zer
ThreadHandle th = spawn worker(&work);
if (f == 3) { th.join(); } else { th.join(); }
return work.x;              // rejected: "borrowed by a scoped spawn"
```

**Cause — deliberate.** The 2026-08-03 cross-block fix guards borrow release on
`Checker.branch_depth <= Symbol.th_spawn_branch_depth`. A join nested deeper than
the spawn is path-conditional, so the borrow survives the branch. That is what
closes the accept-unsafe race; the price is that an all-arms join is not
recognised as unconditional.

**Workaround (one line).** Hoist the join out of the branch — `if (f == 3) { ... }
th.join();`. The compiler error names the borrow, and this is the idiomatic shape
anyway.

**Why it was not fixed at the same time.** Recognising "joined on every arm"
requires all-paths analysis of the branch, i.e. the borrow set must become a
per-block lattice with a JOIN at merges (the shape CLAUDE.md's VRP-JOIN row
describes). That is a real refinement, and it is a RELAXATION — the change class
where a bug ships a race rather than an over-rejection. It should follow the
documented accept-unsafe discipline: build the exhaustive branch x join-position
grid FIRST, verify it fires against the pre-fix build, then relax. The
over-rejection is safe to live with meanwhile.

**Tripwire.** `tests/zer/scoped_borrow_branch_shapes.zer` pins the four shapes
that must keep compiling, so the guard cannot broaden unnoticed.

---

## OPEN — findings carried forward from the 2026-07-21→31 branch wave (documented, NOT fixed on any branch)

These are the audit findings the eleven branches recorded but did **not** fix. Deduped against the rest
of this file — items already tracked here (cross-block scoped-borrow, the `?T` optional-unwrap class-kill,
`vrp_ir.c` dead code) are noted as cross-references rather than repeated.

### ~~CRITICAL ACCEPT-UNSAFE — cross-function free of a FIELD of a by-value struct/union param~~ (`rdh99l` `9a40ead4`) — **CLOSED, measured 2026-08-08**
**Does NOT reproduce on main.** The reproducer below is rejected with `double free: local %0 already
freed at line 7` + `use after free`. Siblings probed the same day and ALSO rejected: 2-hop
(`outer(h) -> inner(h) -> free(h.f)`), free-then-USE (not double-free), and a NESTED field
(`free(h.in.f)`). Four over-rejection controls still compile (single correct free; read-only callee;
two independent allocs; plain by-value param). The union form is masked by the stronger
union-variant-read rule, so it is not separately testable. Closed by the by-value-struct-field
carrier work of 2026-08-06 (`5748e904`, `ir_arg_view_handle` + the (c3)/(c4) return-summary arms).
Pure ZER (no cinclude / `*opaque` / asm). Compiles clean; runtime glibc double-free abort or observable UAF.
```zer
struct B { u32 x; } struct H { [*]B buckets; }
void fb(H h) { free(h.buckets); } // frees a FIELD of a by-value struct param
u32 main() { H h; h.buckets = alloc(B, 4) orelse { return 0; };
             fb(h); free(h.buckets); return 0; } // analyzer thinks THIS is the first free
```
**Root cause.** The FuncSummary free-of-param scan (`zercheck_ir.c` ~5352) gates on the param's resolved
type being POINTER/HANDLE/OPAQUE/SLICE, so a by-value **STRUCT/UNION** param is dropped entirely; and even
for the accepted kinds it inspects only the BARE param handle (`ir_find_handle`, `path_len == 0`), never a
compound FIELD handle. There is no `frees_param_field` signal, so a callee freeing `h.buckets` records
nothing and the call site never widens `arg.field` to FREED. Distinct from BUG-737 (by-value field
STORE-to-global escape), P9 (by-value field launder), §A #6 (intra-function compound copy) and §A #2
(slice/pointer PARAM free).
WARNING: **Doc correction:** the existing negative `tests/zer_fail/alloc_byval_field_slice_uaf.zer` passes for the
WRONG reason (rejected as a LEAK, not because the field-free is tracked) — the moment a caller-side free
satisfies the leak check, the double-free/UAF passes silently. The 2026-07-15 tracker's §A #2 claim that
`fb(H h){free(h.buckets)}` "was recorded" is **inaccurate**.
**Fix sketch (attempted 2026-07-26, REVERTED — not a safe single-session change):** a coarse
`bool *frees_param_field` on FuncSummary (definite/all-path field free of an aggregate param), detected by
looking for a FREED compound handle rooted at the param local in every return block.

### ~~CRITICAL ACCEPT-UNSAFE — block-scoped `defer free/consume` UAF~~ (`i0txin` `84097263`) — **CLOSED, measured 2026-08-08**
**Does NOT reproduce on main.** The reproducer below is rejected: `use after free: local %0 is freed
(freed at line 5)` — the SAME diagnostic and line as the non-defer control, i.e. the defer-specific
gap the entry describes is gone. Closed by the defer instance-id work (`ir_defer_instance_id` /
`ir_fire_has_work_after` in `zercheck_ir.c`), which is the `freed_defer_id` shape §F1 called for.
A `defer free(p)` (or `defer consume(m)`) inside a NESTED block frees/moves at BLOCK EXIT (the intended ZER
semantics), and the emitter emits the free there — but a USE after the block and before the function returns
is silently accepted.
```zer
{ defer free(p); p.val = 222; } // p freed HERE
?*Node mq = alloc(Node); // reuses the freed slot
u32 r = p.val; // UAF read — ACCEPTED; reads 999
```
**Root cause (emitter/zercheck semantic mismatch).** `ir_lower.c` emits a block-scoped `IR_DEFER_FIRE` at
each enclosing block exit and the emitter frees there (correct). But `zercheck_ir.c`'s forward pass (~4955)
treats `IR_DEFER_FIRE` as a NO-OP, and Phase C3 (~5658-5718) applies defer bodies' frees ONLY at `IR_RETURN`
blocks (function-exit / Go semantics) — so the analyzer only "knows" the free at function exit and misses the
mid-function use. A non-defer `free(p)` in the same inner block IS caught, so the gap is defer-specific;
function-level `defer free(p); return p.val;` remains CORRECT (fires after the use).
WARNING: **The trap in the obvious fix:** applying the fired body's free at the `IR_DEFER_FIRE` point closes the
UAF, but `ir_defer_scan_frees` (~1987) has a double-free detector guarded on `free_line != defer_line`, and
Phase C3 ALSO re-applies every defer body at each return block — the two differing lines then trip a FALSE
double-free. A correct fix must coordinate the two application points (either make the forward pass own ALL
`IR_DEFER_FIRE` and have C3 skip already-processed fires, or tag each defer block-scoped vs function-scoped).
DEPENDENCY: Interacts with **§F1** (`freed_defer_id` replaces exactly that line-based guard) — land §F1 first and this
fix gets easier.

### ~~CRITICAL/MEDIUM goto-defer double-fire → double-FREE on the SUCCESS path~~ (`3o10j6` `d1a7d9cc`) — **REPRODUCED with the branch's exact file, then FIXED 2026-08-10**

**Yesterday's "CLOSED, both variants" verdict was WRONG — it came from a hand-written
reconstruction.** Second instance of that failure mode in two days (see G3). The reconstruction put
the loop in `main` with an inline check; the real file has the label followed by `return;` in a
SEPARATE function, and that is what triggers it. The verbatim file
(`origin/claude/gifted-noether-3o10j6:tests/zer_gaps/gap_goto_out_of_loop_defer_double_fire.zer`)
gave `loop_d == 4` for a 3-iteration loop on the FIRST run.

**The defect.** `NODE_LABEL` (ir_lower.c) raised `defer_count` back to the goto's fired-count. A goto
inside a loop body records a fired-count that INCLUDES the loop-scoped defer — but on the
fall-through path that defer has already fired AND popped at every iteration exit. Raising re-armed
it for one extra fire at the label, so a 3-iteration loop fired its defer **4 times, on the path
where the goto was NEVER TAKEN** (the success path). With `defer free(x)` that is a double free.

**The fix is a REMOVAL, and the evidence for it is that code and rationale had diverged.** The
raise's own comment justified it as *"defers registered AFTER what the goto fired ALSO need to
fire"* — but those are already inside `ctx->defer_count` and never needed a raise. The raise could
only ever take effect when the goto had fired MORE defers than are currently live, which is exactly
the popped-scope case. `ctx->defer_count` at the label IS the live set on that path. `restore_count`
is still computed and still positions the guard flag; only the resurrecting assignment is gone.

**Checked before removing, not after:** the raise was introduced by `6c368761` together with two
regression tests (`defer_goto_fallthrough_zero_fire.zer`, `defer_goto_handle_leak_regression.zer`) —
**both still pass without it**, as does the whole suite and all 12 defer+goto tests.

**The 34 existing defer-goto matrix cells did NOT discriminate** — they measure an acquire/release
BALANCE, which is invariant to an extra fire that also acquires, so they passed both before and
after. Verified against a pre-fix build. Added a **fire-count sub-grid** (`LoopGoto` axis, 2 cells,
matrix now 36) that does: **35/36 pre-fix (fires 4, want 3) → 36/36 after.** Regression test
`tests/zer/defer_goto_loop_fire_count.zer` covers both arms (goto never taken = 3, taken at i==1 = 2)
and was verified to FAIL pre-fix.


### ~~HIGH spawn target reaches a non-shared global through a funcptr FIELD callback~~ (`02nq43` `7aac453a`) — **CLOSED, measured 2026-08-08**
**Does NOT reproduce on main** — rejected with an exact diagnostic: *"spawn target 'worker' calls through
a function-pointer field whose bound function accesses non-shared global 'g_ctr'"*, both when the carrier
is read as a global and when it is passed as a spawn arg. Closed by `5ed17c2f`
(`scan_funcptr_field_bindings`). **PROBE WARNING — the entry's own reproducer is MASKED twice:**
`spawn worker(&gw)` on a non-shared `W` is rejected by the non-shared-pointer-to-spawn rule, and reading
a non-shared global `gw` inside the target is rejected by the global-access rule. The carrier must be a
`shared struct` for the funcptr-field rule to be the one under test.
```zer
void worker(*W w) { w.cb(); } // call through a funcptr FIELD
u32 main() { gw.cb = do_inc; spawn worker(&gw); return 0; } // ACCEPTED — g_ctr races
```
The DIRECT form (`worker` writing `g_ctr` itself) IS correctly rejected. This is the register-in-setup /
invoke-in-worker RTOS pattern, so the binding `gw.cb = do_inc` lives in `main`, OUTSIDE the scanned `worker`
body; a funcptr FIELD callee (`NODE_FIELD`) is not resolved. Same "check fires lexically but misses the access
reached through a funcptr the sink doesn't match" shape as the closed SPAWN-FP / ISR-TRANS holes (#11/#12),
one sink further out. **Sibling of §D8** (which closes the funcptr-ARG facet) — take §D8 first, then decide
whether the FIELD facet warrants the same treatment.

### ~~HIGH spawn target reaches a non-shared global through a RETURNED funcptr~~ (`rvek5f` `00f3c2af` HOLE-D) — **FIXED 2026-08-08** (LOCAL half was `00dc785a`)
Third and last form of one question. `scan_funcname_binding` required a `NODE_IDENT`, so a funcptr
obtained from a FACTORY CALL (`*() fp = get_fp(); fp();`) resolved to nothing and the callback was
never scanned. New `scan_returned_funcname` walks the callee's `return <global function name>` sites
and scans each returned body through the existing global-access scan. Flagging on ANY returned name
is the sound direction — if a racing function CAN be returned, the race is reachable. Depth-guarded
by the shared `_scan_global_depth`; a computed or param return resolves to nothing (no new rejection).
Tests: `tests/zer_fail/spawn_funcptr_returned_race.zer` (verified ACCEPTED pre-fix) +
`tests/zer/spawn_funcptr_returned_safe_ok.zer` (four over-rejection controls: no-op, threadlocal,
`@atomic_*`, and a factory used outside `spawn`).

**A FIFTH form was found by enumerating for the gate, not by any report:** a factory returning
ANOTHER factory's result (`get_a(){ return get_b(); }`) — the return expr is a `NODE_CALL`, so the
new resolver bailed exactly as the old one had. Closed by recursing through `scan_funcname_binding`
(bounded by `_scan_global_depth`; a self-recursive factory terminates). Four sequential fixes had
not exhausted this class. The whole REACH axis is now gated by the funcptr-reach grid in
`tests/test_conc_matrix.c` (6 reach forms x 4 payloads, verified firing: 65/67 with 2 false
negatives pre-fix, 67/67 after).
```zer
*() get_fp() { return touch; }
void worker() { *() fp = get_fp(); fp(); } // indirect call — scanner gives up
```
Genuine unsynchronized race; compiles clean with only a stack-depth *warning*. `scan_unsafe_global_access`
(checker.c ~10203-10264) follows only calls whose callee is a named global function (plus function-NAMES
passed as args, BH-18 #8). A call through a funcptr obtained from a return value / local is unresolvable →
the scanner gives up and the data-race rule is bypassed. Direct `fp = touch; fp()`, a global funcptr, and a
struct-field funcptr are all caught (the funcptr STORAGE is itself a flagged non-shared global) — only the
returned/local funcptr leaks.
**Fix sketch:** a spawn target (transitively) calling through an UNRESOLVABLE funcptr cannot be proven free of
non-shared-global access → conservatively REJECT (the can't-prove-safe ⇒ reject barrier discipline). NOT done
on the branch because it risks broadly over-rejecting legitimate callback-in-thread patterns; the
safe/ergonomic boundary is a design call.
Together with the funcptr-FIELD item above and §D8, this completes the spawn-funcptr facet map:
**ARG = fixed (§D8), FIELD = open, RETURNED/local = open, direct/global/struct-storage = already caught.**

### ~~HIGH G3 — atomic-cell plain access not transitive through a helper~~ (`3o10j6` + `38z6wi` + `fxvnsu` G3) — **REPRODUCED with the branch's exact file, then FIXED 2026-08-09**

**The hand-written reconstruction was WRONG, and the "did not reproduce in 5 shapes" verdict it
produced was wrong with it.** The probes used a helper taking a POINTER (`helper(*C p){ p.v += 1; }`);
the real shape is a helper writing the global BY NAME (`poke(){ g_ctr = 5; }`). Pulling the verbatim
file from `origin/claude/gifted-noether-fxvnsu:tests/zer_gaps/gap_atomic_cell_plain_access_via_helper.zer`
reproduced it immediately. **This is the concrete cost of a missing reproducer file** — exactly the
risk flagged when four G-probes had to be measured with reconstructions. Reconstruct only when you
must, say so, and go get the original before trusting a NEGATIVE result.

**The defect.** "Is this global an atomic cell?" is whole-program, but "flag the plain access" fired
only for accesses LEXICALLY inside the spawning function (gated on the per-function
`c->after_spawn_in_func`). So `spawn worker(); g_ctr = 5;` was rejected while `spawn worker(); poke();`
with `poke(){ g_ctr = 5; }` was accepted — **moving a statement into a helper changed whether it
raced.** TSan-confirmed on the branch.

**The fix** mirrors the ISR-TRANS fix (#12) — the same per-function to transitive shape. New
`record_atomic_plain_in_callee` (checker.c) descends a callee's body at a `NODE_CALL` made while a
fire-and-forget spawn is live and records its plain global accesses; the existing post-check
`check_atomic_cell_safety` still makes the decision, so a global that never becomes an atomic cell is
unaffected and this adds no rejection on its own. `@atomic_*` arg0 is skipped (the blessed atomic
access), mirroring `in_atomic_intrinsic_arg` on the direct path. Depth-8 bounded; 2-hop verified;
plain READS caught as well as writes.

**Over-rejection boundary pinned** by `tests/zer/atomic_cell_helper_safe_ok.zer`: a helper that
synchronizes with `@atomic_*`, one touching an unrelated global, one where no spawn exists, and one
called BEFORE the spawn all still compile. Negative =
`tests/zer_fail/atomic_cell_plain_via_helper.zer`, kept **byte-identical to the branch file** so it
pins the finding rather than a reconstruction; verified ACCEPTED on a pre-fix build.
Probed 2026-08-08: `&global` handed to a helper, the same two-hop (`worker -> mid -> helper`), and a
plain struct write through a helper with the cell also touched atomically — **all three REJECTED**
("spawn target accesses non-shared global"). The one shape that was accepted had BOTH sides atomic
(`@atomic_add` + `@atomic_store`), which is correct acceptance, not a hole. Either the transitive scan
now covers this, or the live shape is narrower than described. **Do not reopen without a reproducer that
is verified to compile clean on main** — and check the rejection REASON, since the plain-global rule
masks this one easily.
Already tracked as **G3** in the 2026-07-19 §G section — re-verified still open on both branches
(TSan-confirmed). Recorded here only so the wave's coverage map is complete; do not duplicate the entry.
The unified root with the now-fixed §D6/§D7 borrow holes: the scoped-borrow / plain-access analysis is
intra-name and intra-function and does not treat `&x` handed to a helper as an access. Making it
inter-procedural (a summary "does this callee access/borrow its pointer arg?") is subsystem-scale.

### ~~HIGH — variable-index bit-slice WRITE: unclamped position shift is C UB~~ (`n0odo5` `17ec74ca`) — **FIXED `ec64526e` 2026-08-06** (`emit_bitslice_runtime_mask`, both emitter paths; test uses a `volatile` position so it discriminates at `-O2`)
`reg[hi..lo] = v` with a RUNTIME `lo` lowers to `(uint64_t)(v) << _zer_bl` and `mask << _zer_bl`
(emitter.c ~1708-1743 AST path + the IR mirror). When `lo >= 64` the shift count is out of range → C UB
(x86 masks mod 64, other arches differ), violating ZER's stated "shift by ≥ width = 0 (defined)" guarantee.
The WIDTH computation is already clamped; the POSITION shift is not. The companion bit-slice READ path was
fixed for exactly this (audit #18, commit `c9e4abca`); the WRITE path was not.
**Severity LOW** — the result is stored back through `*_zer_bp` typed to the carrier width, so the store
truncates: a wrong VALUE / UB, **not** an out-of-bounds memory write.

### LOW LOW — value-returning `async` has no result-retrieval API (`7fxhb3` `31796ef8`, 2026-07-28)
`async u32 compute() { … return 42; }` compiles clean and the state machine correctly finalizes (BH-18 #10,
fixed 2026-06-26). But the returned value is stored in an internal temp (`self->_zer_t0`) with **no
caller-accessible accessor** — a user who writes `async <non-void>` can never read the result. Neither
rejected nor retrievable = a silent footgun. Resolution is either a real retrieval mechanism (a stable
`.result` field + `_zer_async_NAME_result(&task)`, distinct from the `int` poll done-flag) or REJECT
`async <non-void>` until such an API exists. Not memory-unsafe (there is no valid usage today).

### ~~LOW Over-rejection — container field of `Handle(T)` drops T substitution~~ (`i0txin` `84097263`) — **`Handle(T)` FIXED `9ea6c864` 2026-08-06**; `Pool`/`Ring`/`Slab` stay rejected (emitter cannot stamp inline storage) — message quality tracked as its own OPEN entry
`subst_typenode` (checker.c ~2158-2219) treats `TYNODE_HANDLE`/`POOL`/`RING`/`SLAB` as LEAVES and does not
recurse into their `.elem`, so `container W(T) { Handle(T) h; }` fails to substitute and emits a LOUD
`error: undefined type 'T'` at the template line. Fails loudly — no silent miscompile. The documented
`T`/`*T`/`?T`/`[*]T`/`T[N]` shapes substitute correctly. **Fix:** recurse into `.elem`/`.inner` for those
four TypeNode kinds.

### ~~LOW Over-rejection — struct-by-value return carrying a PARAM-view field~~ (`rvek5f` `00f3c2af`) — **FIXED `5748e904` 2026-08-06** (view-alias family form 5; gated by `tests/test_view_alias_matrix.c`)
`struct Sl { [*]u32 s; } Sl mk([*]u32 p) { Sl r; r.s = p[0..2]; return r; }` is rejected
("return pointer to local 'r'") though it is safe — the struct copy carries a CALLER-memory view out. The
bare-param-view relaxation (BUG-764) covers `return p[0..2];` but not a param view wrapped in a
returned-by-value struct. Errs conservative → no soundness threat.

### LOW Doc mismatch — bit-extraction on a `volatile *u32` MMIO register is over-rejected (`rvek5f` `00f3c2af`)
CLAUDE.md's "Hardware Support" quick reference shows
`volatile *u32 reg = @inttoptr(...); u32 bits = reg[9..8];`, but `reg[hi..lo]` on a POINTER parses as a slice
range → `error: slice start (9) is greater than end (8)`. Bit-extraction only works on a scalar VALUE.
Verified workaround: `u32 v = *reg; u32 bits = v[9..8];`. Either fix the docs to show the deref form, or make
`reg[hi..lo]` on a volatile scalar pointer auto-deref for bit-extraction.

### Cross-references (already tracked elsewhere in this file — do NOT duplicate)
- **HOLE-C / cross-block scoped-borrow, join-in-branch** (`rvek5f` `00f3c2af`) — **FIXED 2026-08-03.**
  rvek5f's trigger was exact: `th.join()` inside a branch cleared the per-Symbol `is_borrowed_by_thread`
  flag in AST-walk order, so an unguarded access on the OTHER (un-joined) path was not flagged.
  **Its proposed fix location was wrong** — the borrow lives in `checker.c` as a linear statement-order
  approximation, NOT in the IR analyzer, so a "zercheck_ir borrow-set CFG merge" would have edited a
  subsystem that never held the state. The actual fix is `Checker.branch_depth` +
  `Symbol.th_spawn_branch_depth`: a join releases the borrow only when it is no deeper in
  runtime-conditional nesting than the spawn. Regression test
  `tests/zer_fail/scoped_borrow_crossblock_early_return.zer` (verified to COMPILE pre-fix); boundary
  positives in `tests/zer/scoped_borrow_branch_shapes.zer`. The residual precision cost (a join on EVERY
  arm is now over-rejected) is its own OPEN entry above.
- **`?T` optional-wrapper class-kill — BUILT 2026-08-01.** Was the OPEN "optional-unwrap" class-kill.
  Closed as ONE gate for the whole wrapper family, not a `?T`-specific one: the wrapper set that hides
  an inner kind is FINITE (`?T`, `distinct T`, array-of, by-value struct/union CARRYING a pointer) and
  the 2026-08-01 sweep hit THREE of the four (C2/D2 optional, C7/D1 struct-carrier, C3/C4
  slice+distinct) — an optional-only gate would have left the struct-carrier shape open.
  - **Gate A (author-time)** `tools/audit_carrier_dispatch.sh` + `carrier_dispatch_baseline.txt`:
    freezes the 33 hand-rolled carrier disjunctions in checker.c/zercheck_ir.c and FAILS on a new one,
    forcing either a carrier predicate (`type_carries_data_pointer` / `type_can_carry_pointer` /
    `escape_type_carries_ref`) or a justified baseline row. Wired into `make check` (gate 6 of 6).
    Verified to fire by injecting a violation.
  - **Gate B (exhaustive)** `LD_OPTWRAP` axis in `tests/test_escape_matrix.c`: the escaping value is
    bound through an optional carrier before reaching each sink. `-Wswitch` on the enum forces every
    generator/name/validity site to handle it, so the grid cannot silently shrink. 35 -> 43 cells,
    all reject, 0 false negatives.
  - **NOT a blanket accessor** (the distinct playbook does not transfer): unwrapping `?T` is correct at
    a safety sink but WRONG at the ~93 emitter sites that dispatch on TYPE_OPTIONAL to decide
    `.has_value` / null-sentinel emission. Hence a linter that forces a per-site choice, not a rewrite.
  - Residual: the 33 baselined rows are "known, untriaged" — Gate B answers whether each handles every
    wrapper. Prefer converting a row to a predicate and DELETING it over leaving it frozen.
- **`vrp_ir.c` is fully dead code** (`rvek5f` `00f3c2af`) — already noted here; rvek5f re-confirms `vrp_ir()`
  / `IRVRPResult` have ZERO callers and are not in the Makefile, while all load-bearing bounds/division VRP
  is the AST VRP in checker.c. Either wire it (the tracked "wire the orphaned vrp_ir.c" direction) or delete
  it to stop implying live coverage.

---

## OPEN — BUGS: fixes available on four `claude/*` branches (fxvnsu / yd5ajq / 0h7oz9 / c4c09l), verified NOT in main (2026-07-19)

**What this is.** A SECOND set of `claude/*` audit branches, created AFTER the 41-fix tracker
above closed. Cross-referenced 2026-07-19 against current main (`5fe952f3`): **22 distinct fixes
are NOT in main** (each verified by grepping the fix's distinctive marker AND reading the main
code region) + **5 documented-but-unfixed gaps** (fxvnsu, reproducers already in
`tests/zer_gaps/`). Two of the 24 fixes ARE already in main (M1/M2 — do NOT re-open). Same
consumption rules as the 41-tracker: apply the PROPER version per bug, cherry-pick/rebase onto
HEAD, re-verify (build + neg/pos + FOREGROUND `make check` + sink matrix for escape/free).
`git show <sha>` to inspect. Severity: ~13 memory-safety (UAF/OOB/race incl. an analyzer
heap-UAF), ~9 miscompiles.

**Root-cause pattern (why these exist — so a fresh session sees the shape, not 22 unrelated
bugs).** Almost all are the MULTI-SITE / PER-NODE-KIND patchwork class (CLAUDE.md "MULTI-SITE
SAFETY IS THE #1 RECURRING BUG CLASS"): the same question answered independently at N
sites/node-kinds, a new form silently missed. The clusters: (a) VRP-JOIN wired per node-kind —
only NODE_IF landed via §C #13, so switch/for/goto/do-while still leak (§A #1–#4); (b) the `?T`
optional wrapper hiding the inner kind at N escape/coercion sites — NO enforced unwrap gate (the
OPEN "optional-unwrap" class-kill), producing §B #7–#9 + §D #14–#16; (c) the two emitter dispatch
paths (AST ~3xxx + IR ~7xxx) — §D; (d) uN/iN mask not threaded through every value op — §D #13/#17.
Coverage was audit-found, not proof-found. The durable end-states are the class-kills
(one-query-plus-a-gate) noted in CLAUDE.md.

### Source branches (fork base → commits)
| Branch | Base | Commits (short) |
|---|---|---|
| gifted-noether-fxvnsu | 5fe952f3 (current HEAD) | 9fea9990 |
| gifted-noether-yd5ajq | 260a80d9 | c6b72dc0, a44bbfd0, 11621483 |
| gifted-noether-0h7oz9 | 065679bd | 65fea9a9, d9bfb368, ea8264f5, 395ea87e, c9e4abca, 5f783476, 12684dc4 |
| gifted-noether-c4c09l | 3cc45d1d | c8badf4a, 6e72b400 |

### Already in main — do NOT re-open (2)
- **M1 — VRP-1 NODE_IF conditional-narrow leak** (`0h7oz9` `395ea87e`). CLOSED in main via **§C #13**
  (`vrp_snap_take/restore/join`, checker.c ~11128). 0h7oz9's `vrp_snapshot`/`vrp_join_after_region`
  is a REDUNDANT different impl — skip it (its `vrp_conditional_narrow_no_leak.zer` duplicates main's
  coverage).
- **M2 — §B#13 spawn by-value STRUCT/UNION carrying stack ptr** (`0h7oz9` `5f783476`). CLOSED in main
  (checker.c ~13598, the `else if (!is_scoped && (STRUCT||UNION) && spawn_arg_is_stack_derived)` arm).
  0h7oz9 additionally lists by-value **ARRAY** + a `type_carries_data_pointer` precision gate — marginal
  & likely moot (by-value arrays coerce to slices at the call site). Core closed; ARRAY extension is
  low-value.

### A. VRP range-JOIN class → silent OOB — ALL 4 DONE (landed 2026-07-19, make check 990/0)
**DONE 2026-07-19:** all four siblings (#1 switch, #2 for-body, #3 goto/label, #4 do-while)
applied to `checker.c` + regression tests (3 in `tests/zer_trap/` compile-clean + TRAP=133, the
do-while positive in `tests/zer/`). make check 990/0, all audits + sink matrix clean.
The branch-merge JOIN is re-implemented per node-kind. Main has NODE_IF (§C #13) + while/do-while
body-writes (`vrp_invalidate_loop_body_writes`, BUG-748). These four kinds still leak a narrowed range
past the join → a later `arr[idx]` proven in-bounds on a path where idx is wild → bounds guard elided →
silent OOB (read AND write; ASan-confirmed on the branches).

- **#1 — switch-arm range narrow leaks past the join** (`yd5ajq` `11621483` VRP#3). `switch(sel){ 7 =>
  {idx = wild%4;} default => {} }` mutates the VarRange in place; NODE_SWITCH has no
  snapshot/restore/union-join → [0,3] leaks to `arr[idx]` on the default path. Fix: mirror the if-handler
  (snapshot pre-switch, `vrp_snap_restore` before each arm, accumulate the UNION seeded with pre-switch
  state, restore the union after). Test `tests/zer_trap/vrp_switch_arm_range_leak.zer`. **In-main: NOT
  present** — main's NODE_SWITCH handler (checker.c ~11491) has ZERO `vrp_snap`/`var_range` save/restore.
- **#2 — for-body other-var range leak** (`yd5ajq` `11621483` VRP#4). `u32 j=0; for(i..){ arr[j]=..;
  j+=1; }` — the for-handler's `check_expr(step)` re-widens only the LOOP var, so `j` is seen as its
  pre-loop [0,0] in the body. Fix: add the `vrp_invalidate_loop_body_writes` pre-pass (the while/do-while
  handler already has it) before the loop-var range push. Test `tests/zer_trap/vrp_for_body_range_leak.zer`.
  **In-main: NOT present** — main's for-handler (checker.c ~11370) never calls
  `vrp_invalidate_loop_body_writes`.
- **#3 — goto/label ignored by single-pass VRP** (`yd5ajq` `a44bbfd0` VRP#5). `idx=wild%256; goto skip;
  narrow: idx=wild%4; skip: arr[idx]` — the source-order walk narrows at `narrow:` though `goto skip`
  bypasses it at runtime (also a backward-goto loop). Fix: NODE_LABEL handler widens EVERY tracked VRP
  range (a goto can enter carrying any value → sound over-approx of the join over all incoming edges).
  Test `tests/zer_trap/vrp_goto_skip_narrow_leak.zer`. **In-main: NOT present** — main's NODE_LABEL
  (checker.c ~12441) is just `break` ("labels are just markers").
- **#4 — do-while applies bottom-condition range to the first (unguarded) body iteration** (`fxvnsu`
  `9fea9990` BUG-D). `u32 i=seed(); do { s+=buf[i]; i+=1; } while (i<4);` with unprovable entry i=10 elides
  the guard. while/do-while SHARE the cond-narrow (`push_var_range` from the cond) — sound for while (cond
  checked first), unsound for do-while (body runs first). Fix: gate the cond-derived narrowing to
  `node->kind != NODE_DO_WHILE` (`vrp_invalidate_loop_body_writes` already widened body-written vars, so
  the loop var stays full-range → emitter inserts the runtime guard). Test
  `tests/zer/dowhile_bounds_first_iter.zer`. Distinct from §C #14 (do-while `find_return_range`).
  **In-main: NOT present** — main narrows for BOTH kinds (checker.c ~11464, no `!= NODE_DO_WHILE` gate).

### B. Escape / dangling-pointer class → accept-unsafe UAF — ALL 5 DONE (landed 2026-07-19)
**DONE: #7/#8/#9 DONE (make check 994/0):** the c4c09l `?[*]T`/`?*T` optional-carrier sub-cluster —
keep-registration + persist sink now taint TYPE_OPTIONAL carriers (#7); the return-dangling (#8)
and array-store-to-global (#9) escape checks now accept optional-of-slice (via `type_dispatch_kind`,
audit-clean). 4 negatives in `tests/zer_fail/` reject; ground-truth-probed bug-present-on-main first.
**DONE: #5/#6 DONE (make check 996/0, sink matrix 32→41 CLEAN):** yd5ajq intrinsic-launder (#5, new
`unwrap_ptr_launder` helper at the assign + orelse-fallback taint sinks — `@ptrcast(&local)` is
NODE_INTRINSIC, not NODE_UNARY) + struct-element-copy (#6, store-to-global descend gate widened to
`escape_type_carries_ref(vt) || type_can_carry_pointer(vt)`). Tests
`tests/zer_fail/escape_{intrinsic_field_store_global,array_elem_struct_copy_global}.zer` +
sink-matrix cells p11/p12 (+ 3 safe baselines); the 5 `rust_tests/rt_opaque_*` were converted to the
long-lived-context idiom (they had relied on the #5 loophole). Whole class closed.
Same escape class as CLAUDE.md's per-sink patchwork note, at 5 sinks main's taint missed. All
ASan-confirmed `stack-use-after-return`/`stack-buffer-overflow` on the branches. #7/#8/#9 are the
`?[*]T`/`?*T` optional-carrier sub-cluster (the OPEN "optional-unwrap" class-kill).

- **#5 — intrinsic-wrapped `&local` into a struct field escapes un-tainted** (`yd5ajq` `11621483`
  Escape#1). `Box b; b.p = @ptrcast(*u32,&local); g_box = b;` compiles → `g_box.p` dangles. The assign-sink
  taint marking the container root `is_local_derived` matched only a BARE `&local` (NODE_UNARY);
  `@ptrcast`/`@pun`/`@bitcast`/`@cast`/`@container`-wrapped `&local` is NODE_INTRINSIC → taint never fired.
  Fix: `unwrap_ptr_launder` helper (unwrap launder intrinsics → underlying pointer; @container→args[0],
  else last arg) at the assign-taint site + its orelse-fallback. Sink cell `p11`. Test
  `tests/zer_fail/escape_intrinsic_field_store_global.zer`. **In-main: NOT present** — `unwrap_ptr_launder`
  = 0 hits.
- **#6 — struct-copy of a local-derived array/struct ELEMENT** (`yd5ajq` `11621483` Escape#2). `Box[2]
  arr; arr[0].p=&local; g=arr[0];` compiles. The store tainted the array root, but the store-to-global sink
  gated its descend-to-root on `escape_type_carries_ref(vt)` (pointer/slice/opt-of-those only) — `arr[0]`
  has value-type `Box` (a by-value struct transitively carrying a pointer), so descend was skipped. Fix:
  widen the gate to `escape_type_carries_ref(vt) || type_can_carry_pointer(vt)` (the root
  `is_local_derived` check still gates; pointerless struct/scalar copies stay accepted). Sink cell `p12`.
  Test `tests/zer_fail/escape_array_elem_struct_copy_global.zer`. **In-main: NOT present** — main's gate is
  bare `escape_type_carries_ref(vt)` (checker.c ~4559).
- **#7 — keep inference blind to `?[*]T`/`?*T` optional-carrier param** (`c4c09l` `c8badf4a`). `?[*]u8 g;
  stash(?[*]u8 s){ g=s; }` did NOT infer keep on `s` (the plain `[*]u8` param did), so
  `stash(local_array)`/`stash(&local)` compiled → dangling global slice. Two per-sink lists omitted
  TYPE_OPTIONAL: param-registration + the keep-persist sink. Fix: add an optional-carrier branch at
  registration (`pk == TYPE_OPTIONAL && type_carries_data_pointer(ptype,0)` — the helper recurses the
  optional inner, so `?u32`/`?bool` stay untainted) and accept TYPE_OPTIONAL at the persist sink. Tests
  `tests/zer_fail/keep_optional_{slice,ptr}_param.zer`. **In-main: NOT present** — main's keep-registration
  (checker.c ~14899) lists POINTER/OPAQUE/SLICE, no TYPE_OPTIONAL branch.
- **#8 — `?[*]T` RETURN of a local array bypasses the escape check** (`c4c09l` `6e72b400` #4). `?[*]u8
  mk(){ u8[5] buf; return buf; }` compiles a dangling slice (the non-optional `[*]u8` form is rejected).
  The return-dangling check gated on `current_func_ret->kind == TYPE_SLICE`; the `?[*]T` (TYPE_OPTIONAL)
  wrapper hid it. Fix: unwrap the optional — a return target is slice-like if slice OR optional-of-slice.
  Test `tests/zer_fail/return_local_array_optslice.zer`. **In-main: NOT present** — main line ~12029 gates
  on bare `c->current_func_ret->kind == TYPE_SLICE`.
- **#9 — `?[*]T` global STORE of a local array bypasses the escape check** (`c4c09l` `6e72b400` #5).
  `?[*]u8 g; g = buf;` (local buf) compiles a dangling global pointer. Same root cause — the
  array→slice-into-global check gated on `target->kind == TYPE_SLICE`. Fix: widen to optional-of-slice
  targets. Test `tests/zer_fail/store_local_array_optslice_global.zer`. **In-main: NOT present** — main
  line ~5006 gates on `type_unwrap_distinct(target)->kind == TYPE_SLICE` (unwraps distinct, NOT optional).

### C. Concurrency / ISR (3)
**DONE: #10 DONE (landed 2026-07-19, make check 1002/0):** the B1 extra-lock emitter now CAPTURES the
locked roots into a caller array and the paired unlock REPLAYS exactly that set (reverse order)
instead of re-deriving — balanced across the destructive orelse rewrite — so the `!find_orelse` gate
is removed and `x = ga.maybe orelse gb.plain` locks gb too. Regression
`tests/zer/conc_orelse_multiroot_lock.zer`; the two `[16]` scratch arrays baselined.
**DONE: #11/#12 DONE (landed 2026-07-19, make check 1006/0):** #11 SPAWN-FP — `scan_unsafe_global_access`
now descends into a function-name binding (var-decl init / assignment / struct-init field) via
`scan_funcname_binding`, so a global RMW laundered through a local funcptr (`*() fp = do_inc; fp();`)
is no longer race-blind (shared file-scope depth budget). #12 ISR-TRANS — new `record_isr_globals`
walks the `interrupt {}` body and follows direct calls into global-function bodies (depth-guarded),
recording every global read + compound-assign target, so the "shared ISR/main → must be volatile" +
"volatile compound RMW → non-atomic" checks are transitive through helpers. Tests
`tests/zer_fail/{isr_transitive_rmw,isr_transitive_volatile,spawn_funcptr_local_race}.zer` +
`tests/zer/spawn_funcptr_shared_ok.zer`; `test_modules/hal.zer` updated to the safe pattern (it had
relied on the hole). **Concurrency/ISR class C COMPLETE.**
- **#10 — same-type two-instance shared read in an `orelse` under-locked → data race** (`yd5ajq`
  `c6b72dc0`, TSan-confirmed). `x = ga.maybe orelse gb.plain` where `ga`,`gb` are two instances of the
  SAME `shared struct S` locked only `ga` → `gb.plain` read unlocked. Two gaps: (1) the same-statement
  deadlock check dedups shared roots by `struct_type.type_id`, collapsing the same-type pair (a
  DIFFERENT-type pair IS caught); (2) the B1 extra-lock emitter was gated `if (se && !find_orelse(se))`
  (skipped for orelse, because unlock re-derives the root set and lowering destructively rewrites the
  NODE_ORELSE between lock/unlock). Fix: `emit_shared_lock_if_needed` CAPTURES the extra locked roots into
  a caller array; `emit_shared_unlock_if_needed` REPLAYS exactly that set (reverse order) instead of
  re-deriving → balanced across the rewrite → the `!find_orelse` gate is removed. Threaded through all 3
  lock/unlock call sites. Test `tests/zer/conc_orelse_multiroot_lock.zer`. **In-main: NOT present** —
  ir_lower.c ~1544/1562 still `if (se && !find_orelse(se))`.
- **#11 — spawn race scan blind to a global laundered via a local funcptr** (`0h7oz9` `ea8264f5`
  SPAWN-FP). `*() fp = do_inc; fp();` (or a funcptr struct field) — `scan_unsafe_global_access` followed a
  call transitively only when the callee resolved to a GLOBAL function, so a local funcptr callee's
  non-shared global RMW raced clean (reproducible lost update). Fix: `scan_funcname_binding` descends into
  a function-name binding (var-decl init / assignment / struct-init field), mirroring the existing
  function-name-as-arg descent; shared file-scope depth budget. Tests
  `tests/zer_fail/spawn_funcptr_local_race.zer` + `tests/zer/spawn_funcptr_shared_ok.zer`. **In-main: NOT
  present** — `scan_funcname_binding` = 0 hits.
- **#12 — ISR global-access checks not transitive through callees** (`0h7oz9` `ea8264f5` ISR-TRANS,
  silent bare-metal). The "shared ISR/main → must be volatile" + "volatile compound RMW → non-atomic"
  checks saw only globals lexically inside the `interrupt {}` body; an ISR touching a global through a
  helper bypassed both (non-volatile flag hoisted → hang; helper `counter += 1` torn RMW). Fix:
  `record_isr_globals` walks the interrupt body (in_interrupt), follows direct calls into global-function
  bodies (depth-guarded), records every global read + compound-assign target as an ISR access.
  (`test_modules/hal.zer` relied on the hole — updated to volatile + explicit read/write.) Tests
  `tests/zer_fail/isr_transitive_{volatile,rmw}.zer`. **In-main: NOT present** — `record_isr_globals` = 0
  hits.

### ~~D. Emitter miscompiles (7)~~ — **ALL 7 DONE** (#13/#17/#18/#19 landed 2026-07-19; #14/#15/#16 measured CLOSED 2026-08-09)
The array->`?[*]T` coercion family (#14 struct-init field, #15 var-decl/assign/call-arg/return, #16 global
array returned as `?[*]T`) was measured on main 2026-08-09: **all four value-flow sites carry `.len`
correctly** (`v.len == 4` at var-decl, call-arg, global return, and struct-init field; each run exits 0).
**DONE: #13/#17/#18/#19 DONE (landed 2026-07-19, make check 1000/0, all matrices + sink 41 CLEAN):**
#13 `@saturate(uN)` unsigned odd-width now clamps via `(1ULL<<w)-1` + carrier cast (both emitter
paths); #17 `@bitcast(uN/iN)` now masks/sign-extends the punned carrier via `emit_intn_mask_lv`
(both paths); #18 variable-index bit-extract now guards the POSITION shift on `type_width` (both
paths); #19 `volatile` scalar/aggregate locals now emit the qualifier (new `IRLocal.is_volatile`,
set in ir_lower, emitted in emit_regular_func_from_ir). 4 positive tests in `tests/zer/` compile +
run exit 0. **DONE: #15/#16 DONE + #14 mostly done (landed 2026-07-19, make check 1011/0, all audits +
sink 41 CLEAN):** the array→optional-slice coercion is now applied at every value-flow site —
`emit_opt_wrap_value` + two helpers (`struct_field_type_by_name`, `aggregate_slice_coerce_target`) +
AST/IR struct-init + IR var-decl `need_wrap` + IR assign-expression opt-wrap + **call-arg** opt-wrap +
the **#16 array→`?[*]T` return** branch (was falling through to a bare `return;` → caller saw None) — all
audit-clean via `type_dispatch_kind` (no baseline coupling). Verified: `optional_slice_coerce.zer`
(var-decl / assignment / call-arg / param-slice-return, exit 0) + `optional_slice_return_global.zer`
(global-array `?[*]T` return, exit 0). **DONE: #14 struct-init field COMPLETE (landed 2026-07-20, make
check 1012/0):** the missing site was the **IR_STRUCT_INIT_DECOMP** emitter (the var-decl struct-init
`Buf b = { .data = a }` / `W w = { .maybe = a }` path — distinct from the emit_rewritten_node
NODE_STRUCT_INIT path used by the assignment form). The DECOMP emitter has the field-value type
directly as `func->locals[vloc].type` (no node-typemap dependency), so the coercion builds the
`{ptr,len}` slice from the local name + `array.size` in both the optional-field-wrap and plain-slice-
field branches. Test `tests/zer/optional_slice_struct_field.zer` (var-decl + assignment, both field
kinds, exit 0). **The `?[*]T`/array→slice coercion class (#14/#15/#16) is now fully closed.**
  **DONE: BONUS — param-array-return relaxation SHIPPED (2026-07-20, make check 1014/0, sink matrix 44
  CLEAN):** returning a `u8[N]` array PARAM as a slice (`[*]u8 f(u8[N] a){ return a; }` / `?[*]u8`) is
  now ACCEPTED — array params are by-reference (they decay to a pointer into the CALLER's array,
  empirically verified), so the returned slice views caller memory, sound exactly like the BUG-764
  slice/pointer-param view class. The return-escape check (checker.c ~12287) now skips its error when
  the root ident is a param; the emitter (src1_local return path, both the `?[*]T`-wrap and plain
  `[*]T` branches) builds the `{ptr,len}` slice from the local name + `array.size`. **Accept-unsafe
  VERIFIED SAFE:** `classify_return_root` already records the param index in `ret_param_mask`, so the
  CALL SITE rejects a caller that passes a LOCAL and lets the result escape (`g = f(local)` ->
  "local-derived pointer argument ... may escape"). Tests `tests/zer/return_param_array_ok.zer` (local
  use, exit 0) + `tests/zer_fail/return_param_array_escape.zer` (escape rejected) + sink-matrix cells
  p13 (reject escape) + safe_return_param_arr_{opt,slice} (accept). A LOCAL array return still dangles
  -> still rejected.
  **Prior investigation 2026-07-19 (first attempt reverted — do NOT half-apply):** this is a **7-site emitter
  patchwork** — `emit_opt_wrap_value` coercion + two helpers (`struct_field_type_by_name`,
  `aggregate_slice_coerce_target`) + AST struct-init + IR struct-init (both wrap/non-wrap branches) +
  IR var-decl `need_wrap` + IR **assign** `need_wrap` + IR **return** `need_wrap` (the `emit_local_name`
  path — structurally different, harder) + **call-arg**. 5 of these were applied audit-clean (via
  `type_dispatch_kind`, no baseline coupling — fxvnsu's version instead baselined 14 raw `->kind` reads)
  and BUILD-GREEN, but the 3 tests still failed: `optional_slice_coerce` (run≠0 — assign/call-arg site)
  and `optional_slice_return_global` (run≠0 — the return `need_wrap` site) need the remaining sites.
  **CHECKER INTERACTION (the real blocker):** the fxvnsu `optional_slice_from_array` test does
  `?[*]u8 get(u8[4] a){ return a; }` (a is a PARAM array) — my just-landed **#8** fix (return an
  array as `?[*]T` now hits the escape check, consistent with the pre-existing NON-optional check)
  **rejects it**, where before #8 the optional wrapper hid it. Before finishing #14-16, DECIDE: is
  `return param_array` as a slice safe? If ZER array params decay to a caller-memory pointer (like a
  slice/pointer param — BUG-764 allows returning views of those), then #8 should be RELAXED for param
  arrays and the test compiles; if array params are by-value copies, #8 correctly rejects and the test
  must source from a global. Resolve that first, then apply the remaining 3 emitter sites + bring the 3
  tests (`optional_slice_{from_array,coerce,return_global}.zer`). Silent-miscompile severity (wrong
  `.len`), NOT memory-safety — lowest priority of the 22.
- **#13 — `@saturate(uN, v)` for a non-native UNSIGNED width never clamps** (`fxvnsu` `9fea9990` BUG-A).
  `@saturate(u7, 200)` returns 200 not 127. The unsigned emit path (BOTH emitter paths) hardcoded the max
  on a `{8,16,32,else→u64}` switch → every odd width fell to the u64 branch (never clamps), result cast to
  the carrier with no N-bit mask (the signed path already computed min/max from the width → correct). Fix:
  compute the unsigned max as `(1ULL<<w)-1` for `w<64` (keep the `.0` double form for `w>=64`), cast via
  `emit_type`. Test `tests/zer/saturate_unsigned_odd_width.zer`. **In-main: NOT present** — main emitter.c
  ~7186 hardcoded `{8,16,32,else}` switch.
- **#14 — fixed array into an optional-slice / slice AGGREGATE field not coerced (`.len` dropped)**
  (`fxvnsu` `9fea9990` BUG-B). `?[*]u8 s=a; return a; W w={.maybe=a}; Buf b={.data=a};` + the assignment
  form emitted the bare array ident; C brace-flattening dropped it into `.value.ptr`/`.len` (optional-slice:
  swallowed `has_value` into `.value.len` → a PRESENT optional built EMPTY; plain slice: `.len=0` → false
  OOB trap). Scalar/var-decl/call-arg paths coerced; the AGGREGATE-construction paths (optional-wrap,
  return-into-optional, struct-init field — across AST + IR + IR_STRUCT_INIT_DECOMP) did not. Fix: shared
  `struct_field_type_by_name` + `aggregate_slice_coerce_target` helpers; `emit_opt_wrap_value` coerces its
  `?slice` inner; all six aggregate sites coerce array→slice before the wrap. Test
  `tests/zer/optional_slice_from_array.zer`. **SUPERSET of #15/#16** — fxvnsu is on current HEAD, so this is
  the canonical base. **In-main: NOT present** — `aggregate_slice_coerce_target` absent; `need_wrap`
  (emitter.c ~9799) emits `(opt){<array>,1}` → `.len=0`.
- **#15 — array→`?[*]T` coercion dropped `.len` at var-decl/assign/call-arg/return** (`c4c09l` `6e72b400`
  #3). SAME CLASS as #14, narrower/earlier. Fixed the four value-flow sites to build `{ptr,len}` inside the
  optional when `optional.inner` is a slice and the source is an array. Tests
  `tests/zer/optional_slice_coerce.zer` + `optional_slice_return_global.zer`. (c4c09l covers the call-arg
  site; #14 covers struct-init-field — merge as ONE class, base on #14.) **In-main: NOT present** (same
  evidence as #14).
- **#16 — global array returned as `?[*]T` emits a bare `return`** (`c4c09l` `6e72b400` #6). The
  array→slice-return coercion gated on `ret_eff->kind == TYPE_SLICE` → an optional return fell to the void
  branch → caller saw None. Fixed alongside #15. Test `optional_slice_return_global.zer`. **In-main: NOT
  present** (same class).
- **#17 — `@bitcast(uN/iN, x)` never masked/sign-extended the punned carrier** (`c4c09l` `6e72b400` #1).
  `@bitcast(u5, i5(-1))` yielded 255 not 31; `@bitcast(i5, u5(31))` yielded 31 not -1. The memcpy-pun into a
  carrier temp omitted the non-native uN/iN mask `@truncate` has. Fix: apply `emit_intn_mask_lv` to
  `_zer_bco` when `type_is_nonnative_intn(t)` on BOTH dispatch paths. Test `tests/zer/bitcast_intn.zer`.
  **In-main: NOT present** — main emitter.c ~3204/7208 emit `_zer_bco` with no mask.
- **#18 — variable-index bit-extract emitted an unguarded POSITION shift (UB)** (`0h7oz9` `c9e4abca`
  BITSHIFT). `reg[hi..lo]` with runtime `lo` lowered to raw `reg >> _zer_lo` — C UB when `lo >= width`
  (GCC folds it differently at -O0 vs -O2 and on ARM/RISC-V), violating ZER's "shift by ≥ width = 0". The
  F8 fix guarded the extract-width MASK but not the shift. Fix: guard the shift on the object's `type_width`
  in both emit paths. Test `tests/zer/bitslice_read_runtime_shift_guard.zer`. **In-main: NOT present** —
  main emitter.c ~2565 emits raw `>> _zer_lo` (only the mask `& ((_zer_w>=64)?...)` is guarded).
- **#19 — volatile qualifier dropped on scalar/aggregate locals** (`0h7oz9` `d9bfb368` VOL-1, silent
  bare-metal). `volatile u32 d;` delay/timing loop silently optimized away — scalar/aggregate locals carry
  no volatile in their Type (only slice/pointer do), so the emitter emitted no qualifier. Fix: add an
  `is_volatile` IR-local flag (ir.h), set in ir_lower.c, emit the qualifier in emitter.c. Test
  `tests/zer/volatile_scalar_local.zer`. **In-main: NOT present** — `is_volatile` absent from ir.h.

### E. Checker miscompiles (2) — DONE: BOTH DONE (landed 2026-07-19, make check 1009/0)
**DONE: #20/#21 DONE:** #20 LIT-1 — `retype_const_int_to_target` retypes a PURE integer-literal expr to
the destination integer width (`is_pure_int_literal_expr` / `int_retype_target`) at var-decl init,
return, call-arg, struct designated-init field, AND binary-operand promotion, so `i64 a=-3` /
`u64=1<<40` compute in the target width (test `tests/zer/const_int_target_width.zer`). #21 orelse-block
— a non-diverging `orelse { block }` in value position now types `void` (via `orelse_block_diverges`,
an if-chain outside the -Wswitch audit) so the existing "cannot initialize with void" check rejects it,
while a diverging block keeps the unwrapped type and a bare-statement `f() orelse {…}` stays legal
(tests `tests/zer/orelse_block_diverge.zer` + `tests/zer_fail/orelse_block_value_nondiverge.zer`).
Checker-miscompile class E COMPLETE.
- **#20 — negative / u32-overflowing integer literal into a 64-bit type computed in u32** (`0h7oz9`
  `65fea9a9` LIT-1). `i64 a=-3` = 4294967293; `u64 x=100000*100000` wraps at 2^32; `u64 s=1<<40` truncated.
  Literals lowered in u32/u64 by value width; the assignment path already const-folds to the target width,
  but the other value-flow sites lowered u32-typed temps. Fix: `retype_const_int_to_target` retypes a PURE
  integer-literal expr (`is_pure_int_literal_expr`) to the destination integer width (`int_retype_target`,
  unwraps `?integer`) at var-decl init, return, call-arg, struct designated-init field, AND binary-operand
  promotion. Narrow targets still wrap; non-fitting constants still rejected. Test
  `tests/zer/const_int_target_width.zer`. **In-main: NOT present** — `retype_const_int_to_target` = 0 hits.
- **#21 — non-diverging `orelse { block }` in value position silently yields 0** (`c4c09l` `6e72b400`
  #2). `u32 x = f() orelse { g=1; };` (block falls through) compiles and sets x=0 — the checker typed the
  orelse `unwrapped` unconditionally (stale "GCC rejects value-position stmt-exprs" assumption the IR
  auto-zeroed temp defeats). Fix: `orelse_block_diverges()` helper (tail return/break/continue/goto,
  nested-if both-arms-diverge, `orelse return/...` tail); a FALL-THROUGH block types the orelse `void`
  (existing "cannot initialize with void" rejects value use); a bare-statement `f() orelse {...}` stays
  legal. Tests `tests/zer/orelse_block_diverge.zer` + `tests/zer_fail/orelse_block_value_nondiverge.zer`.
  **In-main: NOT present** — `orelse_block_diverges` = 0 hits.

### F. Analyzer heap-UAF (1 — a crash / stale safety decision in zercheck itself) — DONE (2026-07-19)
**DONE: #22 DONE (make check 1002/0):** the IR_ASSIGN non-move alias branch's invalid-use check is now
`else if` (mutually exclusive with the ALIVE-add branch that reallocs `ps->handles`), so the
dangling `src_h` is never re-read. Regression `tests/zer/handle_alias_realloc_uaf.zer` (10 straight-line
?Handle/orelse alias pairs to cross the capacity-8 realloc; compiles + runs exit 0 — the true guard is
an ASan build).
- **#22 — heap-use-after-free reading `src_h` after `ir_add_handle` realloc** (`0h7oz9` `12684dc4`). The
  non-move alias branch of `ir_check_inst` (IR_ASSIGN) captured `src_h` as a raw pointer into `ps->handles`,
  then called `ir_add_handle(dest_local)` which can realloc that array; the subsequent invalid-handle check
  dereferenced the dangling `src_h` (ASan crash on valid input; a stale safety DECISION on the shipped
  build, where the freed region is still mapped). The alive-add branch and the invalid-use check are
  mutually exclusive (ALIVE ⟹ not invalid). Fix: convert the second `if (src_h && ir_is_invalid(src_h))`
  to `else if`. Repro: two `?Handle`/orelse + array-store pairs push handle_count past capacity 8 at the
  aliasing assignment. (No dedicated test file — repro in the commit message.) **In-main: NOT present** —
  main zercheck_ir.c ~3819 still a plain `if` right after the reallocating `ir_add_handle` at ~3813.

### ~~G. Documented-but-unfixed gaps — `fxvnsu` `9fea9990`~~ — **4 of 5 CLOSED, measured 2026-08-09; G3 UNCERTAIN**

**MEASURED, not assumed** — each probed on main and the REJECTION REASON read:
- **G1 goto-skips-defer — CLOSED.** A `defer` registered after the goto source does not fire at the
  label (`lock == 1`, correct). The sibling goto-defer double-fire (`3o10j6`) was NOT closed —
  that reconstruction was wrong; it is fixed 2026-08-10, see its own entry.
- **G2 helper launders `&local` past a scoped borrow — CLOSED.** *"cannot pass '&d' to a call while
  it is borrowed by a scoped spawn"* — the exact rule, not a mask. Direct-write control also
  rejected; the no-access-between-spawn-and-join control still COMPILES (no over-rejection).
- **G4 `threadlocal &`-escape to a SCOPED spawn — CLOSED.** *"cannot pass '&tl' (threadlocal) to a
  scoped spawn — each thread has its own copy"*. The scoped path A5/BUG-757 left uncovered is covered.
- **G5 heap ptr into a struct-global FIELD — CLOSED.** *"global 'g.p' left dangling at function
  exit"* — the `.field` projection sink is reached; the bare-ident control is also caught.
- **G3 atomic-cell transitivity — CLOSED 2026-08-09.** Reproduced with the branch's VERBATIM file
  after a hand-written reconstruction had wrongly reported "does not reproduce"; see its own entry above.

**The reproducer files this heading claims are "in `tests/zer_gaps/`" DO NOT EXIST** (`gap_goto_skips_defer.zer`,
`gap_scoped_borrow_via_helper.zer`, `gap_threadlocal_amp_escape_scoped_spawn.zer`,
`gap_struct_global_field_dangle.zer` — none present; that directory holds an unrelated 24). Measured with
hand-written equivalents from the descriptions. **If someone still has `fxvnsu`, re-probe with its exact
files before trusting these four closures.** The same claim was false for the `3o10j6` goto-defer entry.
- **G1 — forward `goto` fires a defer it textually skipped** (MEDIUM miscompile). `gap_goto_skips_defer.zer`.
  The acquire / `goto done` / `defer release()` idiom underflows a lock counter (release fires on the error
  path where acquire never ran). Root cause: `ir_lower.c` fires function-scope defers as a static set at
  every exit/goto-target; only defers registered AFTER the goto source are runtime-skipped
  (`defer_count_at_def` handles back-edges; the "goto BEFORE the defer, label AFTER" shape is unhandled).
  Proper fix: per-defer runtime "armed" flags (like the plt86m `defer_fire_guard_flag`); sound interim:
  reject a forward goto that skips a defer registration. NOT the documented back-edge/bh18_12 case.
- **G2 — scoped-borrow exclusivity evaded by a helper laundering `&local`** (HIGH race, TSan-confirmed).
  `gap_scoped_borrow_via_helper.zer`. A stack local borrowed by a scoped `spawn` is exclusive until
  `.join()`; the DIRECT `local.v=7;` between spawn/join IS rejected but `poke(&local)` is not — the borrow
  check is intra-name and does not treat `&local` handed to a helper as an access. Same-block (distinct from
  the known-open cross-block case).
- **G3 — atomic-cell plain-access check not transitive through a helper** (HIGH race, TSan-confirmed).
  `gap_atomic_cell_plain_access_via_helper.zer`. "is atomic cell?" is whole-program, but "flag the plain
  access" fires only for accesses lexically inside a spawning/spawned function; move the plain write into
  `poke(){ g_ctr=5; }` and it is missed (the same plain write directly in `main` IS rejected).
- **G4 — `threadlocal &`-escape to a SCOPED spawn establishes no borrow** (HIGH race, TSan-confirmed).
  `gap_threadlocal_amp_escape_scoped_spawn.zer`. Passing `&tls` to a scoped spawn registers no borrow
  (unlike a stack local) → main's concurrent write between spawn/join is unflagged → the thread writes
  MAIN's TLS slot. A5 (BUG-757) closed threadlocal `&`-escape for fire-and-forget; the scoped-spawn path
  was not covered.
- **G5 — heap pointer stored into a struct-global FIELD dangles unflagged** (CRITICAL UAF).
  `gap_struct_global_field_dangle.zer`. `g.p = n; free(n);` (g a struct global) leaves `g.p` dangling, but
  the "global left dangling at exit" check (GAP-3/BUG-739, zercheck_ir.c ~3231) matches only a BARE global
  ident store (`g = n`, which IS caught) — the `.field` projection sink is missed (per-sink patchwork; cf.
  the P9 by-value-field-launder fix that descended the projection to the root). Cross-thread amplification:
  storing into a `shared struct` field + reading from a worker = silent cross-thread UAF on a reused slab
  slot (observably reproduced, exit=999). CAUTION for the fixer: extends the BUG-742 global-dangle
  conservatism — the maintainer deliberately does NOT flag MAYBE_FREED globals at exit (avoids noising the
  legit register-ctx-then-callback pattern); a field-projection fix must flag only a DEFINITELY-freed
  target, mirroring the bare-ident register/alias/exit machinery for the compound `(IR_GLOBAL_ROOT_ID,
  name.field)` key.

**Test-only (not a bug):** `fxvnsu` also rewrites the flaky `rust_tests/rc_cond_004` (a Rust-Mutex→ZER
translation assuming cross-statement atomicity ZER's per-statement locking doesn't provide → ~12%
lost-update failures breaking the `make check` gate; rewritten to accumulate via atomic compound-assign).
Main still has the flaky version.

---

## OPEN — type-erasure / safe `void*` (generic `*opaque` container over-rejected) (2026-07-16) (over-rejection, NOT a soundness hole)

**Symptom:** a GLib-`GHashTable`-style generic container that stores `*opaque` VALUES and returns
them (`?*opaque map_get(...)`) is rejected by zercheck — the `?*opaque` return trips the
allocation/ghost-handle heuristic (*"result of alloc() … never used"*), and the borrowed value
looks like an owned handle that must be freed. So the GLib `void*` pattern fights the safety layer.

**Root cause:** `*opaque` owned-ness is **type-driven** ("any `*opaque` = an owned allocation"),
whereas every OTHER ZER pointer (`*T`/`[*]T`) derives owned-ness from **provenance** (`alloc_id`).
An erased borrow (`(*opaque)&global`) has no `alloc_id` but is still treated as owned.

**Why the obvious workaround is fine for now:** `usize` value tokens (ZER's `gpointer` analog) work
TODAY (VERIFIED), and `container(K,V)` monomorphization / a tagged union cover compile-time-known
and closed-set cases. Only the SAFE re-type of an open-runtime erased value is missing.

**The fix (Path A, IN PROGRESS) — full design + reasoning + how-we-got-here + the C-cast sugar +
Rust comparison + implementation plan + test matrix live in `docs/universal_pointer.md` PART 6
/ §36.** One-line sketch: make `*opaque` owned-ness `alloc_id`/provenance-driven (not type-driven),
so the erased pointer becomes the fifth citizen of the pointer family (`{ptr, type_id}` = the
`[*]T` of erasure); re-type via the `type_id` trap; sugar is the C-cast `(*T)erased` (already works
+ already checked — verified). RELAXATION (bug = shipped UAF), so oracle-first.

**STATUS (2026-07-16):**
- **Level-1 oracle DONE** — `proofs/operational/lambda_zer_escape/erased_ownership_lattice.v`
  (0 admits, in `make check-proofs`), certifying the origin lattice + AOParam/AOBorrow split + both
  accept-unsafe traps as witnesses.
- **Step-2 Increment 0 SHIPPED (sound, inert)** — the `FuncSummary.ret_is_borrow` signal +
  exhaustive `ir_expr_has_ptrish_call` walker; computed + verified, NOT consumed (zero behavior
  change).
- **Step-2 Increment 1 (the consumer) SHIPPED (2026-07-16) — the content-borrow over-rejection is
  FIXED.** Two FuncSummary signals + a leak-suppress: `ret_is_borrow` (no allocation-capable call
  in the body) AND `ret_is_content` (every return is a value-read of a field/element or null — NOT
  a param-VIEW). Only `ret_is_borrow && ret_is_content` marks the result `escaped=true` (suppress
  the false leak) while keeping it REGISTERED (double-free/UAF intact). This IS the oracle's
  AOBorrow (suppress) vs AOParam (keep). `map_get`'s `*opaque` container now compiles
  (`erased_map_get_ok.zer`); `rt_drop_conflict_uaf` + `erased_wrapper_double_free` still reject.
  Full `make check` GREEN. **A naive `ret_is_borrow`-only gate was accept-unsafe** (broke the
  interior-pointer UAF); `ret_is_content` is load-bearing — see §36.17 + the zercheck_ir.c
  consumer comment. **Residual (pre-existing, NOT a regression):** a param-VIEW return
  (`return &s.field`) still over-rejects with a leak false-positive — deliberately kept because it
  incidentally catches the through-call interior-pointer UAF (which zercheck does not otherwise
  catch). Fixing that properly (infer `returns_param_color` for `&param.field` so the caller
  aliases the result to the arg → a real UAF, then leak-suppress the view too) is a separate future
  refinement.

## OPEN — native `uN`/`iN` follow-ups (2026-07-09) (none a soundness hole; polish only)

Native arbitrary-width integers `uN`/`iN` shipped (BUGS-FIXED.md 2026-07-09,
commits e7ea2bcb/d91d0742/80183261; `make check` GREEN). The type-kind
predicates are VST-verified for `TYPE_UINT`/`TYPE_SINT` (Level-3 restored,
`verif_type_kind.v`, verified via `make check-vst` 2026-07-10). Width-masking is
complete: `emit_intn_mask` runs on `IR_BINOP` (arithmetic + shift) and `IR_UNOP`
(negation/complement); `emit_intn_mask_lv` masks assignment + compound-assign stores
(`=`/`+=`/`-=`/`*=`/`&=`/`|=`/`^=`/`<<=`, single-eval on side-effecting targets); `@truncate`
masks inline+stored; bit-slice READ uses a 64-bit guarded mask (all 2026-07-13, merged from
k7l625/jfrmer branches); global-scope arithmetic needs no mask (verified safe by rejection
2026-07-12 — see BUGS-FIXED.md). Remaining edges (all polish/deferred):

- **VRP mask-elision (LOW — performance).** `emit_intn_mask` always emits the
  `& (2^N-1)` after `uN` arithmetic. Sound but not minimal: where VRP can prove
  the result already fits N bits (bounded counters, constants), the mask should
  be elided → single machine op. Also the invariant-preserving ops (`& | ^ >>`)
  never need the mask and could skip it unconditionally. Not built. (Explicitly
  deferred 2026-07-10 — a Rice-bounded precision optimization on the least-
  important axis, not a correctness matter; see BUGS-FIXED.md.)
- **Single-bit `reg[5]` shorthand — an ARCHITECT DECISION, not a mechanical fix.**
  A bare single index on a scalar integer (`reg[5]`) errors "cannot index type";
  use the range form `reg[5..5]` (works today, read + write). Making `reg[5]` a
  bit-access is a 2-line rewrite (`NODE_INDEX` → `NODE_SLICE[N..N]` in the
  checker), and it works — BUT it conflicts with an INTENTIONAL safety design:
  ZER deliberately rejects indexing a scalar integer (`u32_var[i]`) to catch
  "I thought this was an array" bugs (C unit test "index u32 rejected"). Bit
  access uses the explicit range syntax on purpose. Flipping `scalar[5]` from an
  error to a silent bit-access removes that guard — a safety tradeoff to decide
  deliberately, not slip in. (Attempted + reverted 2026-07-09: the rewrite broke
  that test; `reg[5..5]` remains the way unless the design is changed on purpose.)
- **`>64`-bit `uN` uses emulated multi-word arithmetic** (carrier is `__int128`
  ≤128; the `emit_intn_mask` __int128 branch). For hand-tuned big-int use the
  `@addc`/`@subb`/`@mulw` carry primitives + a limb struct (library).

---

## OPEN — findings surfaced during the universal-`alloc` build (2026-07-08) (mostly LOW/MEDIUM, none an active soundness hole)

Bugs found while building `alloc`/`free` (docs/universal_alloc.md) but out of that
scope, so NOT fixed. Full write-ups (repro + root cause) in
**docs/universal_alloc.md §11**. Triage:

- **MEDIUM — bare `orelse return;` inside a `?T`-returning function yields a wrong
  `None`.** `?u32 f(){ *E e = slot orelse return; return e.value; }` with `slot`
  null: the caller sees `f()` as HAVING a value. Only the BARE form in a `?T`
  function; the block form `orelse { …; return null; }` and explicit `return
  null;` are fine. A correctness bug (wrong runtime behavior), narrow.
- **MEDIUM — `subst_typenode`'s `TYNODE_HANDLE` case does not recurse into
  `handle.elem`.** Any `container` field shaped `Handle(T)`/`?Handle(T)` fails
  with "undefined type 'T'" (breaks self-referential `container Chained(T){
  ?Handle(Chained(T)) next; }` and more). Separate from the depth-32 recursion
  guard. Would need `subst_typenode` to recurse HANDLE like POINTER/OPTIONAL do.
- **LOW — global `Arena` in-place `garena.over(buf)` does not initialize** → a
  later `garena.alloc_slice(...)` returns `None` at runtime. Only `Arena x =
  Arena.over(buf)` (capture the return) works.
- **LOW — `global = arena.alloc_slice(...)` (direct form) compiles** when the
  temp-var form is rejected: the escape rejection (checker.c ~4694) only fires on
  a bare `NODE_IDENT` value; the direct call/orelse form only taints
  (checker.c ~4645). A false-negative escape gap (masked at runtime by the global-
  arena init bug above).
- **LOW — `[*]?*T` slice element emits broken C** ("incompatible types … struct
  anonymous" from GCC). Workaround: a named wrapper struct (`struct Bucket { ?*T
  head; }`). An anonymous-struct-in-slice-typedef emitter gap.
- **LOW — a named `const` is not accepted as an array size** (`?*E[N]` with a
  const N → "array size must be a compile-time constant"); only a literal or a
  `comptime` function call is. Cosmetic wart.

**Also OPEN — the pointer-return relaxation covers var-decl only.** `*T p = &r[i];
return p;` (where r is a param/heap slice) is accepted; the separate ASSIGNMENT
form `p = &r[i]` (p declared earlier, assigned later) is a different escape sink
(checker.c ~4138 area) and may still over-reject. Extend by mirroring the same
`root_is_ref && !is_local_derived` guard there. See BUGS-FIXED.md 2026-07-08.

---

## OPEN — MAX-ORACLE GAP AUDIT (2026-06-23) — the master map: which safety classes are not-sound / not-flexible / coarse-or-no-oracle

> **THE PLAN THAT CONSUMES THIS AUDIT: `docs/unified-oracle-proved-ZER.md` (2026-08-10).**
> This entry is the per-class GAP MAP; that document is what to DO about it — the unified
> Level A product (provenance x liveness x ownership x bounds x capability, componentwise
> join, conjunction at every use site), with Level B relaxations underneath. Priority order
> there is driven by a measured finding: **every defect found 2026-08-08..10 was in a class
> with NO oracle, zero exceptions**, while handle/concurrency/escape/move/opaque produced
> none. Phase 0 is wiring the orphaned `vrp_ir.c` (349 lines, 0 Makefile refs, 0 symbols in
> the built binary — the sound CFG bounds analysis is not compiled).


Audit of EVERY safety class against the MAX-ORACLE STANDARD (CLAUDE.md): a class is
"at maximum" iff it is (a) SOUND (zero under-rejection — never accepts unsafe), (b)
FLEXIBLE (minimal over-rejection), AND (c) backed by a MAX oracle (a Coq/Iris Level-1
spec whose finite-state set is COMPLETE and whose abstraction is the richest sound one,
not a flat/coarse one). This is the INDEX; per-hole detail lives in the linked entries
below. Verdict tally: ~14 live under-rejections (6 are CRITICAL memory-corruption), ~4 real
over-rejections, and MOST classes are coarse-oracle or no-oracle — only 3 are genuinely
AT-MAX. Two clusters (type/provenance fully audited 2026-06-23; the other five audited
from this ledger after the parallel workflow rate-limited).

### NOT-SOUND — under-rejects (accepts unsafe). The urgent tier (close before precision work).

**Memory-corruption (CRITICAL UAF/OOB):**
- **`@bitcast` int↔ptr forge** (#3) — **[FIXED 2026-06-23 — wired the verified
  `zer_bitcast_operand_valid`; see the FIXED entry below]**. Was: `@bitcast(*T, intval)`
  reinterprets an integer as a pointer with a clean compile: on 64-bit a ptr and u64 are
  both 8 bytes so `zer_bitcast_width_valid` passes, and the handler (checker.c:7230-7270)
  calls ONLY the width + const/volatile-strip checks — never an int-vs-ptr operand check.
  A grammar-level breach of the "no in-language unsafe" closure (synthesizes the banned
  `ptr+N`). The fix predicate ALREADY EXISTS and is VST-verified —
  `zer_bitcast_operand_valid(is_primitive)` returns 0 for a pointer operand
  (src/safety/cast_rules.c:31, proofs/vst/verif_cast_rules.v) — but is NEVER CALLED from
  checker.c. Control: `(*T)int`→"use @inttoptr" (checker.c:6840), `(u32)ptr`→"use
  @ptrtoint" (6848) — every other path gates int↔ptr; only `@bitcast` bypasses all three.
  **Fix = one call site** (wire the proven predicate; reject when exactly one of {src,dst}
  is a pointer) + tripwire. The cleanest fix in this whole audit.
- **`@pun` `type_id==0` short-circuit** (#4) — **[FIXED 2026-06-23 — compile-time
  widening reject; see the FIXED entry below]**. Was: the emitted guard is
  `if (type_id != TGT && type_id != 0) trap` (emitter.c:2723/2870/2951/2972/6860/6921;
  comment at 2908 admits "type_id == 0 sentinel matches anything"). An in-ZER pointer to
  a PRIMITIVE (`*u32`/`*u8`/slice `.ptr`/`@inttoptr` result) packs `type_id==0`, so
  `(0 != TGT && 0 != 0)` = false → trap SKIPPED even for a statically-known size mismatch
  (`*u32 sp=&small; *Big bp=@pun(*Big,sp); bp.b` reads 8 past a 4-byte object). The opaque
  oracle (lambda_zer_opaque J04) models TRACKED provenance and never sanctions extending
  the unknown-tag(0) escape to a fully-typed in-program pointer. **Fix:** don't grant the
  `!=0` escape to a fully-typed in-ZER primitive/slice pointer (the 0-escape is only for
  genuinely-unknown cinclude `*opaque`), or add the compile-time size-widening check.
- **move-struct alias** (#1, line ~1551) — an alias taken BEFORE the move-transfer isn't
  registered in the source's state group, so TRANSFERRED doesn't propagate → free/move
  tracking defeated.
- **VRP scope-leak OOB** (#2, line ~1636) — a branch-local range narrowing leaks past a
  control-flow join (flat AST `var_range_count` not saved/restored on the non-comparison
  branch) → the compiler proves `buf[idx]` safe and emits NO bounds check on a path where
  `idx` is OOB. ROOT: the sound CFG-VRP `vrp_ir.c` is orphaned (absent from the Makefile,
  not even compiled); production runs the unsound flat pass. Oracle now exists
  (lambda_zer_bounds/bounds_lattice.v `elide_on_join_sound`); fix = wire `vrp_ir.c`.
- **fixed-array bare-call index** (#5, line ~1786) — `arr[f()]` on a fixed array drops the
  bounds check on the bare-call single-eval emission path.
- **`|*v|` capture escape** (#6, line ~1829) — the `if(opt)|*v|` capture binds `v=&m.value`
  (into a local) but the desugaring doesn't inherit `is_local_derived`, so `g=v` is treated
  as a normal global store → dangling global. The direct `g=&m.value` IS rejected; only the
  capture-synthesized address slips through. Oracle now exists
  (lambda_zer_capture/capture_lattice.v — `capture_preserves_escape` + `buggy_reset_unsound`
  witness the bug); fix = capture inherits the matched value's region.
- **defer-body UAF** (line ~1459) — a defer body uses a handle the function body then frees.
- **P9 by-value struct field-launder** (#P9) — **[FIXED 2026-06-24 — see BUGS-FIXED.md]**.
  Was: `void stash(Holder h){ g = h.p; }` + `stash({.p=&local})` COMPILED — a pointer field
  of a by-value struct PARAM stored to a global laundered a local-derived pointer (the direct
  `g = h.p` with a local `h` was already caught; only the through-a-param launder slipped).
  Found by the empirical probe sweep. Fixed: the keep-2a sink (checker.c ~4209) now descends
  the `param.field` projection to the root param and infers keep (theorem param_lattice.v T5
  projection_preserves_escape / buggy_projection_unsound). This was a form→state coverage
  gap, NOT a missing finite state — the per-sink-patchwork class the codebase warns about.

**Data-race (HIGH concurrency):** shared-struct multi-access hidden in a cast/intrinsic/
index/orelse SUBEXPRESSION evades the same-statement deadlock/lock check (#7, ~1881); the
`spawn` data-race scan is blind to function-pointer indirection (#8, ~1925); shared access
in an `await` CONDITION is not locked (D02 false-negative, #9, ~1963). The **cross-block
scoped-borrow** hole (spawn + access in different CFG blocks) is **FIXED 2026-08-03** via
`Checker.branch_depth` / `Symbol.th_spawn_branch_depth` — not the zercheck_ir borrow-set
merge previously sketched here, which named the wrong subsystem.

**Miscompile (MEDIUM — unsound OUTPUT, not a UAF):** value-returning `async` never finalizes
its state machine (#10, ~2001); bit-query/byte-swap intrinsics emit `0` in global
initializers (#11, ~2046); `defer` + backward `goto` fires the wrong defer count (#12,
~2080 / the "defer fires twice" entry ~799); compound `/=`/`%=` lack the signed-overflow
trap on the AST emit path (~938).

### NOT-FLEXIBLE — over-rejects (rejects correct code). Coarse abstractions.

- **Escape disjunctive return** — **NARROWER THAN PREVIOUSLY STATED (empirically corrected
  2026-06-24).** The common disjunctive shapes COMPILE: `pick(c){if c return &g1; return &g2}`
  (both static) and `either(p,c){if c return p; return &g}` called with a global arg both
  compile (Stages 1-2 handle them). The flat `ret_param_mask`'s only real loss vs the JOIN is
  using the surviving `ARParam` fact for DOWNSTREAM precision, not the escape verdict itself
  — for the escape decision, "summary incomplete → UNKNOWN → reject" and "summary contains
  ARLocal → reject" coincide, so a function with a genuinely-unclassifiable return path is
  rejected either way (and is usually unsafe to escape anyway). The RICH oracle
  (lambda_zer_escape/join_lattice.v) is still worth implementing for downstream precision +
  the relational tail, but the "EVERY call maximally conservative" framing was an over-claim.
- **Aliased mutation** — **DOES NOT REPRODUCE (empirically corrected 2026-06-24).** ZER has
  NO aliasing-XOR-mutability rule, so two live interior pointers into one array mutated
  through both (`*u32 p=&a[0]; *u32 q=&a[5]; *p=1; *q=2;`) COMPILES — ZER already accepts
  what Rust rejects here; there is nothing to "unblock". The disjoint oracle
  (lambda_zer_disjoint/disjoint_lattice.v) is NOT needed to accept aliased mutation. The
  genuine residual is narrower: the `alloc_id` **fate-sharing** false-positive (freeing one
  slice-half false-flags the other; mixing literal/variable index frees, BUG-741) — that is
  the only real over-rejection in this area, and it needs the relational layer.
- **Nested inline designated initializer** (#13) — **[FIXED 2026-06-24 — see BUGS-FIXED.md]**.
  Was: `Outer o = { .inner = { .x = 1 }, .y = 2 };` rejected with "field '.inner' … got
  'void'" (the inner `{ .x = 1 }` has no standalone type). Fixed: `validate_struct_init`
  (checker.c ~1441) now recurses on a `NODE_STRUCT_INIT` field value, validating it against
  the field type (which it inherits as context). 2/3-level nests compile+run; inner
  field-name/type errors still rejected.
- **MAYBE_FREED path-correlation — FIXED (2026-06-27): Level B guarded refinement SHIPPED.**
  `if(c){free(h)} if(!c){use(h)}` — and the matching double-free under `!c` + the leak check
  when freed under both `c` and `!c` — now COMPILE: freeing under one guard and using/freeing
  under the DISJOINT complement is recovered, gated on PROVABLE guard disjointness (else the
  Level-A MAYBE_FREED conservatism stands). Implementation (zercheck_ir.c): per-block
  immutable-bool guard sets (`ir_compute_block_guards`) + per-handle `free_block` /
  `freed_all_paths`; the use/double-free/leak sites relax via `ir_use_guard_disjoint` /
  `ir_free_completes_coverage`. SOUNDNESS GATE: `ir_local_is_immutable_bool` via a no-default
  exhaustive AST walk (`ast_name_mutated_or_addrd`) rejecting any reassigned/address-taken
  condition — two accept-unsafe holes (reassigned param; `&c` in a call arg) were found+closed
  during the build. Certified by handle_flow_lattice.v Level B. Tests:
  `tests/zer/guarded_maybe_freed_disjoint.zer` + 6 `tests/zer_fail/guarded_*`. Full detail:
  BUGS-FIXED.md 2026-06-27.
  - **DEFERRED — full state-TRUTHFULNESS (2026-06-28).** The shipped design is
    DECISION-layer (the `MAYBE_FREED` state stays coarse; the use/double-free/leak
    sites consult guard side-channels to get the right OUTCOME). The truer
    STATE-layer version (refine `MAYBE_FREED → ALIVE` in the lattice so every
    consumer sees the truth) was attempted and reverted — it regressed the leak on
    a nested `if` (the refinement bypasses the `freed_all_paths` set-path; the
    leak's "freed on ALL paths" is a coverage fact that can't be per-block state).
    Outcomes are already correct; state-truthfulness needs a per-handle
    free-guard-SET + a more precise (dominator/per-edge) guard computation. See
    `docs/compiler-internals.md` "Sound relaxation" (the reverted-attempt note).
- Every flat-lattice class carries residual over-rejection by construction (see below).

### ORACLE COVERAGE — the theorem layer, by class (the (c) criterion)

- **AT-MAX (sound + MAX-oracle-backed):** **CORRECTED (2026-06-24, maximality workflow
  overturned the original trio).** The verified at-max set is `move` (lambda_zer_move),
  `qualifier` and `volatile` (the property admits no richer sound abstraction). `@ptrcast`/
  `@container` provenance (lambda_zer_opaque) is SUFFICIENCY-only for the modeled fragment
  (PtrNull / type_id=0 / structured `@container` provenance unmodeled). MMIO is COARSE (see
  below). **Handle is NOT at-max:** its operational track leaves soundness obligations
  unproved (excluded from the gate) and the gated Iris track is flat 2-state with placeholder
  lemmas — BUT the abstract DOMAIN is now certified by
  `lambda_zer_handle/handle_flow_lattice.v` (the 4-state flow lattice + JOIN merge, Level A;
  the guarded MAYBE_FREED refinement, Level B). So handle's domain went uncertified → certified
  (2026-06-24); the proof-track admits/placeholders remain a separate cleanup.
- **COARSE oracle (exists but FLAT — precision left on the table):** escape (shipped
  `param_lattice.v` is flat; the rich `join_lattice.v` is spec-only), bounds (interval, not
  relational `i<j`), qualifier / capture / volatile (the 4 added 2026-06-23 are flat by
  design), optional/null (typing.v Section N is flat — certifies only `?T`-vs-`*T`
  type-discipline, NOT the flow-sensitive null-state lattice; rows N04/N06/N07/N08 unproven,
  typing.v:1324).
- **NO oracle (finite-state set UNCERTIFIED — discovered by red team):** distinct-typedef
  unwrap invariant (the #1 historical bug class BUG-409/GAP-F, guarded only by the
  line-frozen `tools/audit_type_dispatch.sh` linter, not an oracle), division-by-zero,
  integer-overflow-wrap, shift safety, ISR safety, naked/asm (Z9/Z10/Z13 never shipped),
  stack-overflow/recursion, `@critical` control-flow, and the intrinsic-miscompile classes
  (#10/#11/#12). The "next frontier" oracle backlog (concurrency/ISR/atomics/async/MMIO
  decision oracles) is line ~393.

### NAMED FLOORS — NOT gaps (out of scope, do not chase)

cinclude/FFI `type_id=0` (extern `*opaque`, C-domain), liveness/deadlock-livelock (out of
scope for ZER *and* Rust), hardware-consequence (datasheet/silicon correctness — physics
floor).

### PRIORITY (fixed by the "never allow unsafe" hard constraint — sound before precision)

1. **The two proof-backed near-free CRITICAL wins:** `@bitcast` #3 (wire the already-VST-verified
   `zer_bitcast_operand_valid`, one call site + tripwire) and `@pun` #4 (the size guard).
2. The stateful CRITICAL holes: #1 (decl-site alias), #2 (wire `vrp_ir.c`), #5 (emitter path),
   #6 (capture inherits region — oracle ready).
3. The HIGH races (#7/#8/#9) + the cross-block scoped-borrow.
4. The MEDIUM miscompiles.
5. THEN precision: implement `join_lattice.v` (disjunctive return) and `disjoint_lattice.v`
   (aliased mutation), and write the missing/richer oracles (relational bounds, the null
   flow-state lattice, distinct-unwrap, the no-oracle classes).

---

## OPEN — WASM CLI: multi-file imports + macOS terminal zerc (LOW–MEDIUM)

> **NOTE 2026-08-10 — the original Defender motivation no longer reproduces for the
> maintainer.** The WASM bridge exists because unsigned mingw PEs tripped Defender
> `Wacatac.B!ml` (CLAUDE.md "VS Code Extension (VSIX) Build"); the shipping VSIX is now
> reported to install and run with no scan. That is a single-machine observation, not a
> general guarantee — do NOT rip out the WASM bridge on the strength of it. **The two
> gaps below are FUNCTIONAL and independent of the scanning question:** multi-file
> `import` genuinely does not resolve through the wasm CLI/LSP regardless of why the
> wasm path exists. This entry stays canon.


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

## OPEN — `naked` attribute silently dropped on IR path

See full entry near the bottom of this file ("`naked` attribute
silently dropped on IR path (deferred 2026-05-02)") — kept in original
location to preserve the more detailed analysis added in the
2026-05-02 fix session.

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
    @ptrcast(*MyType, h); /* validate type via provenance */
    mylib_xyz_terminate_session(h); /* obscure C name */
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

## OPEN — 2026-06-18 multi-agent bug-hunt (BH-18): index + reproduction harness

A 12-finder adversarial bug hunt against current HEAD (`cc374ab`) found and
triple-verified (finder → independent verifier → maintainer re-run in a
container) **14 distinct compiler bugs**. All compile **clean** unless noted.
Six are memory-unsafe soundness holes. Each entry below (BH-18 #1..#14) is
self-contained: minimal reproducer, exact observed/expected, the asymmetric
control that proves it is a gap (not a design choice), root cause, and a fix
sketch. A fresh session can reproduce every one with only the steps here.

> **WARNING: This index PREDATES the 41-fix merge-back (completed 2026-07-15).** Several
> BH-18 rows have since been CLOSED on main and must NOT be treated as still-open —
> cross-check the "## DONE … TASK TRACKER COMPLETE" section at the top of this
> file + `git log` before reproducing any row. Verified closed on main by the
> merge-back: **#2 (VRP range-narrow scope leak → OOB) = §C #13** (`vrp_snap_*` /
> `vrp_join_assign_range`, tests `vrp_{branch_assign,loop_assign}_guard_ok`);
> **#1 (move-struct pointer-alias) = §A #7 + the FIXED-2026-07-01 subsections below**;
> **#7 (shared multi-access via cast/intrinsic/index/orelse subexpr)** was closed
> independently 2026-06-27 (`collect_shared_types_in_expr` no-default switch under
> `-Werror=switch`, see CLAUDE.md). Remaining rows MAY overlap fixes merged in this
> window (e.g. #5, #8, #9) — verify each against the tracker + `git log` before
> treating it as open; do not assume open OR closed without checking.

**How to reproduce (any fresh session):**
```
# build the compiler (native or in Docker; Docker avoids Windows AV):
gcc -O1 -w -I. -o zerc lexer.c parser.c ast.c types.c checker.c emitter.c \
    zercheck.c ir.c ir_lower.c zercheck_ir.c vrp_ir.c zerc_main.c src/safety/*.c
# OR: make zerc

zerc x.zer --run # compile+run; prints "zerc: running x" then process exit = main()'s return
zerc x.zer -o x.exe # compile only (clean compile = no 'error:' line, exit 0)
zerc x.zer --emit-c -o x.c # inspect the emitted C (proves dropped guards / placeholders)
# To PROVE an out-of-bounds access is real (#2,#4,#5), ASan the emitted C:
zerc x.zer --emit-c -o x.c && gcc -fsanitize=address -g -O0 x.c -o x_asan && ./x_asan
```
A "soundness hole" = clean compile + memory-unsafe/guarantee-violating at
runtime (the crown-jewel class — ZER claims 100% program-consequence
coverage). "miscompile" = clean compile + wrong runtime result. Severity tags:
CRITICAL critical (memory-unsafe), HIGH high (race / double-free), MEDIUM medium
(miscompile), LOW low (false-reject / diagnostic).

| # | Bug | Class | Root-cause area |
|---|---|---|---|
| 1 | move-struct pointer-alias defeats ownership/free tracking (heap UAF + use-after-move + double-consume) | CRITICAL soundness | zercheck_ir move/alias |
| 2 | VRP range-narrow scope leak → OOB write | CRITICAL soundness | checker.c if-VRP |
| 3 | `@bitcast` forges integer↔pointer | CRITICAL soundness | checker `@bitcast` |
| 4 | `@pun(*Struct,*primitive)` skips type_id trap → OOB read | CRITICAL soundness | emitter `@pun` guard |
| 5 | fixed-array bare-call index drops bounds check | CRITICAL soundness | emitter index single-eval |
| 6 | `if (opt) \|*v\|` capture escapes to a global | CRITICAL soundness | capture desugar + escape prov |
| 7 | shared-struct multi-access via cast/intrinsic/index/orelse subexpr evades lock check → race | HIGH race | checker `collect_shared_types_in_expr` |
| 8 | `spawn` data-race scan blind to funcptr indirection → race | HIGH race | checker `scan_unsafe_global_access` |
| 9 | shared-struct read in `await` condition unlocked → race | HIGH race | checker `NODE_AWAIT` handler |
| 10 | value-returning `async` never finalizes state machine | MEDIUM miscompile | emitter `IR_RETURN` async |
| 11 | bit-query/byte-swap intrinsics emit `0` in global initializers | MEDIUM miscompile | emitter AST `NODE_INTRINSIC` |
| 12 | `defer` + backward `goto` fires wrong count (folds into known #5) | MEDIUM miscompile | ir_lower defer/back-edge |
| 13 | nested inline designated initializer false-reject | LOW false-reject | checker `validate_struct_init` |
| 14 | conversion-intrinsic arity not validated (missing/excess args) | LOW diagnostic | checker intrinsic arg check |

---

## OPEN — Concurrency memory-safety: all ~25 audited holes CLOSED + named floors (2026-06-22, updated 2026-08-08)

> **STATUS UPDATE 2026-08-08.** The "single remaining hole" named throughout this entry — the
> **cross-block scoped-borrow** case — was **CLOSED 2026-08-03**. The fix was NOT the
> subsystem-scale `zercheck_ir` borrow-set merge sketched below: the borrow is a linear
> statement-order approximation in `checker.c` (`Symbol.is_borrowed_by_thread`), not IR state,
> so an IR merge would have edited a component that never held it. Real defect: a `th.join()`
> nested in a branch cleared the borrow for code AFTER the branch, on paths that never joined.
> Fix = `Checker.branch_depth` + `Symbol.th_spawn_branch_depth` (~40 lines). Residual is
> PRECISION only (a join on every arm is over-rejected) — tracked as its own OPEN entry.
> **Text below is preserved as the historical audit record; read the sketch as superseded.**

**Scope of this entry:** ZER's concurrency PRIMITIVES are all implemented
(shared/spawn/atomics/Semaphore/Barrier/condvar/Ring/async/move). This entry is
the standing ledger of the **memory-safety gaps** in the concurrency model —
verified data races + cross-thread use-after-free that compile clean. Full
design + Rust mapping + closure: `docs/primitives-data-races.md` §24. Per-hole
file:line detail: workflow task outputs `wpbbu8v47` / `wwt4c31zh` / `wgvm1bid5`.
**Do NOT yet claim ZER is FULLY data-race-safe as shipped** — but the open surface
is now ONE hole, not ~25. ~24 of the ~25 audited holes are CLOSED + regression-tested
(BUG-743..759: Axis B complete, A6-full atomic-cell taint complete, A5 threadlocal
`&`-escape fixed, Axis C `threads[]` merge + scoped-borrow same-block read/write
fixed). The single remaining memory-safety hole is the **cross-block scoped-borrow
case** (spawn and access in different CFG blocks; same-block is covered) — detailed
below. Everything else is named FLOORS, out of scope for ZER *and* Rust: D1 cinclude
thread-capture (C-domain, safe path exists) and liveness (deadlock/livelock). So:
very close, not 100% — one subsystem-scale fix (a zercheck_ir borrow-set merge) away.

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
- **[FIXED BUG-751 + BUG-759, Axis C scoped-borrow]** a parent WRITE (751) or READ
  (759) of a non-shared local lent via `&x` to a scoped spawn, between `spawn` and
  `th.join()`, now errors (the thread has exclusive `&mut`-style access until join).
  Both linear (same-block); the borrow flag is set at the spawn and cleared at join.
  **Remaining [OPEN]: cross-block** (spawn and access in different CFG blocks) — the
  proper fix is a borrow-set merge in zercheck_ir (like the `threads[]` merge),
  subsystem-scale and lower-value now that same-block read+write are both covered.
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
  Micro-residuals **[FIXED BUG-758]**: struct-field plain READS + `&s.f` launder
  now tracked (read hook at NODE_FIELD, launder hook at TOK_AMP, mirroring the
  scalar slices 2/4, gated on the fire-and-forget after-spawn context). **A6-full
  is now COMPLETE end to end** — scalar + struct-field, write/read/launder.

**AXIS B IS COMPLETE (2026-06-22): B1 multi-root (BUG-753), B2 union copy-out
(BUG-754), B3 cond_wait foreign-shared reject (BUG-755), B4 @once loser-wait
(BUG-756), B5 defer lock (BUG-749).** A6-full atomic-cell taint complete (BUG-752 +
758). **A5 threadlocal `&`-escape FIXED (BUG-757).** Scoped-borrow read+write are
both FIXED (BUG-751 + 759). The remaining OPEN hole is now ONLY the **scoped-borrow
CROSS-BLOCK case** (spawn and access in different CFG blocks — same-block read+write
are covered; the proper fix is a borrow-set merge in zercheck_ir, subsystem-scale).
**D1 (cinclude thread-capture) is RECLASSIFIED as a named FLOOR, not a hole**
(C-domain behavior, out of ZER's scope; safe path already exists via long-lived
data — see Axis D).
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
- **[FIXED BUG-757 — A5]** `threadlocal` whitelisted for direct access (CORRECT — a
  spawned thread reads its own TLS copy), but `&threadlocal` published to a
  non-threadlocal global/static was a cross-thread wrong-TLS/UAF. Now rejected at
  the escape-sink assignment check (checker.c) — an `else if` after the `&local`
  branch, since a threadlocal is global-scope (`val_is_global`) so the `&local`
  check skipped it. Storing `&tl` into ANOTHER threadlocal is within-thread →
  allowed.
- **[FIXED BUG-744]** `TYPE_SLICE` + `TYPE_OPAQUE` spawn args were uncased → now
  the spawn-arg dispatch is exhaustive over pointer/slice/opaque; the
  `spawn_arg_is_stack_derived` helper unwraps casts + `@ptrcast/@bitcast/@cast/@pun`
  (so `(*opaque)&local` and `[*]T` over a stack array are caught).
- Remaining fix direction: carrier-or-tainted *inclusion* model (the full A6
  shared-scalar taint) replaces the exclusion list entirely; a `-Wswitch`-style
  gate on the spawn-arg dispatch.

**Axis B — single-root auto-lock incompleteness** (per-statement
`current_stmt_shared_root`, ir_lower.c) — **ALL FIVE SUB-ITEMS NOW CLOSED
(2026-06-22)**. Was: locks only the first shared root; bypassed at:
- **[FIXED BUG-753 — B1]** `shared(rw)` multi-read (`x = ga.v + gb.v`) now locks
  ALL distinct shared roots, not just the first (`find_all_shared_roots_expr`;
  extras as read locks — deadlock-free since read locks compose and the
  multi-WRITE case is rejected by the deadlock check; lock/unlock re-derive the
  set, so no `current_stmt_shared_root`-set change was needed). Non-`orelse`
  statements only (narrow residual). Verified in emitted C.
- **[FIXED BUG-754 — B2]** union-switch on a shared struct field: the lvalue path
  built `sw_ref = &g.union` (a raw alias into the shared bytes) and the discriminant
  + capture reads happened AFTER the lock released — even the `|x|` VALUE capture was
  a cross-thread torn read / type confusion. FIX (copy-out, ir_lower.c): when the
  switch root is shared (`find_shared_root_expr`, covers `shared(rw)` too) take the
  RVALUE path, which copies the whole union into a LOCAL *under* the switch-expr lock;
  every subsequent tag/capture read is then of a private snapshot. The `|*x|` mutable
  capture of a shared union is REJECTED at the checker (it would alias the throwaway
  copy → lost mutation; mirrors the A6/#5 interior-extraction ban). No nested lock
  (lock scope unchanged), no new IR. Tests: `tests/zer/shared_union_switch_copyout.zer`,
  `tests/zer_fail/shared_union_switch_ptr_capture.zer`.
- **[FIXED BUG-755 — B3]** `@cond_wait`/`@cond_timedwait` predicate reading a shared
  struct OTHER than the cond struct: the predicate is re-evaluated under ONLY the cond
  mutex (pthread_cond_wait releases only that one lock), so a foreign shared read is an
  unsynchronized cross-thread race. FIX (checker-only reject, `cond_pred_foreign_shared`):
  reject a predicate that reads any shared root whose ROOT IDENT differs from the cond
  var's — instance-precise, so a 2nd INSTANCE of the SAME shared type is also caught,
  while the legit pointer-param `b`/`b.field` case passes. Locking the 2nd struct inside
  the cond mutex is not an option (AB-BA deadlock + cond_wait sleeps holding the extra
  lock); the textbook rule is "a condvar predicate reads only the cond mutex's own
  state." Over-rejects nothing (all 30 existing condvar predicates read only their cond
  struct). Tests: `tests/zer_fail/cond_wait_foreign_shared.zer`,
  `tests/zer_fail/cond_wait_foreign_same_type.zer` (instance-precise),
  `tests/zer/cond_wait_same_struct_multifield.zer` (the prescribed safe restructure).
- **[FIXED BUG-756 — B4]** `@once` loser now WAITS for the winner. 3-state flag
  (0/1/2): winner CAS 0→1 → body → store 2 (RELEASE) at the join block; loser spins
  on `!= 2` (ACQUIRE) → skip. The ACQUIRE/RELEASE pairing means a loser never reads
  the half-constructed published state. **The "BLOCKER" was illusory** — naming the
  function-scope flag `_zer_once_<bb_skip_id>` (the @once's `false_block` id, unique
  per @once, available as `inst->false_block` at the branch AND `bb->id` at the join)
  makes it reachable from both emission sites with ZERO IR change. Control flow
  (return/break/continue/goto) that exits a @once body is now banned (`Checker.in_once`)
  — an early exit would skip the done-publish and hang losers. Tests:
  `tests/zer/once_loser_wait.zer`, `tests/zer_fail/once_control_flow.zer`. Freestanding
  path unchanged (single-core, loser does not wait).
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
The CLOSED set covers the reachability holes (volatile-RMW, Arena, slice/opaque
dispatch), the spawn lifetime arm, the runtime race (`@probe`), **the A6-full
atomic-cell inclusion taint (BUG-752)**, and **the ENTIRE Axis B lock-completeness
family — B1 multi-root (BUG-753), B2 union copy-out (BUG-754), B3 cond_wait
foreign-shared reject (BUG-755), B4 @once loser-wait (BUG-756), B5 defer (BUG-749)**.
The B1–B4 "deadlock-sensitive lock-scope-walker redesign" turned out NOT to need a
single global redesign: each was closed in place without ever holding two shared
locks at once (B1 read-locks-compose, B2 copy-out-under-the-one-lock, B3 reject not
lock, B4 a private once-flag not a struct mutex). Also FIXED: **A5** threadlocal
`&`-escape (BUG-757), the **A6 micro-residuals** (BUG-758, struct-field reads +
`&s.f` launder), and the **scoped-borrow read-side** (BUG-759, same-block).
REMAINING (annotated `[OPEN]` above) is now ONLY the **scoped-borrow CROSS-BLOCK**
case (a zercheck_ir borrow-set merge — subsystem-scale, lower-value). **D1 is a
FLOOR, not a remaining build**
(C-domain; safe path exists). And the still-unprobed residue (FFI callback tables;
other emitter-runtime statics; cross-module spawn/extern; `NODE_STRUCT_INIT` global
read in a spawn body).

---

### VERIFIED STILL OPEN 2026-08-01 — reproduced, with the exact mechanism

Probed against the 2026-07-21 `zerc` (i.e. AFTER the 12-branch merge-back). The entry
above is **accurate, not stale** — the cross-block scoped-borrow hole is live. Three
shapes, all compiled clean or correctly rejected as marked:

| Shape | Result |
|---|---|
| `join` in ONE arm of an `if`, access after the merge | **ACCEPTED — real data race** |
| `join` inside a `while` body, access after the loop | **ACCEPTED — real data race** |
| `join` on BOTH arms, access after (legal) | ACCEPTED — correct; this is the CONTROL |

```zer
struct Work { u32 x; }
void worker(*Work w) { w.x = 1; }
u32 flag_in; // NOTE: condition must NOT mention `work` (see gotcha)
u32 main() {
    Work work;
    ThreadHandle th = spawn worker(&work);
    if (flag_in == 99) {
        th.join(); // borrow cleared on ONE path only
    }
    work.x = 2; // other path: the thread is STILL running -> RACE
    th.join();
    return 0;
}
```

**Root cause — the borrow was never migrated to the CFG.** Handles moved to
`zercheck_ir.c` with a real `ir_merge_states`; the scoped-spawn borrow did not. It is
still a single boolean on the `Symbol`, mutated by a linear AST walk:

| Site | Code |
|---|---|
| `checker.c:14087` | `vs->is_borrowed_by_thread = true;` — set at `spawn` |
| `checker.c:5614` | `bv->is_borrowed_by_thread = false;` — cleared at `join`, **unconditionally** |
| `checker.c:3558` | read check |
| `checker.c:3970` | write check |

Walking into an `if` body clears the flag; walking out does not restore it. Everything
after the merge therefore sees "not borrowed", including the path on which the thread is
still live. There is no `preds[]`, so the state cannot be path-dependent — this is the
AST-cannot-represent-a-merge problem, still resident in the concurrency layer.

Note the `borrow` identifiers in `zercheck_ir.c` are `ret_is_borrow` — return-value
analysis for leak suppression, an unrelated concern. Grepping "borrow" there does **not**
find this.

**GOTCHA for whoever writes the regression test.** A naive test accidentally PASSES.
If the branch condition mentions the borrowed variable (`if (work.x == 0)`), the read
*inside the condition* trips the checker at `checker.c:3558` before the interesting
statement is reached, and the test looks like it rejects correctly. **The condition must
reference an unrelated variable** for the hole to surface. Three of four first-attempt
probes were masked this way.

**Fix (unchanged from the entry above): a borrow-set merge in `zercheck_ir.c`.** Move
the borrow from a `Symbol` boolean to a per-block set, union at joins, same shape as the
`threads[]` merge that fixed the false-green scoped-spawn stack-UAF. Conservative
direction at a join is UNION (still-borrowed wins), matching every other class's
fail-closed discipline.

**Why the obvious fix is wrong:** "never clear the flag" makes the CONTROL case above
(join on both arms, then access) a false positive, rejecting correct code. The
merge must be path-aware, not merely sticky.

**Tripwire tests to add (none exist today):**

```
tests/zer_fail/borrow_join_one_arm.zer must FAIL to compile — currently passes
tests/zer_fail/borrow_join_in_loop.zer must FAIL to compile — currently passes
tests/zer/borrow_join_both_arms.zer must COMPILE — pins against over-correction
```

The third is not optional: without it the sticky-flag "fix" would ship and nothing
would catch the over-rejection.

### RE-VERIFIED 2026-08-03 (HEAD `30a79744`, clean rebuild) — STILL OPEN, and the reproduction above is now MASKED

Re-ran the recorded shapes against a clean rebuild of current main. **The hole is still
live, and the recorded reproduction no longer exhibits it** — anyone re-running the old
repro will wrongly conclude this entry is fixed.

MEASURED per shape on `30a79744` (a first pass said "all three rejected for the wrong
reason"; that was too strong — one is now rejected CORRECTLY, which is a real gain from
the 2026-08-02/03 scoped-borrow work and should not be undone):

| Shape | Result |
|---|---|
| write in a branch, condition references an UNRELATED variable | REJECTED by the **borrow** rule — correct, and newly so |
| conditional join, no early return | REJECTED by the **ThreadHandle leak** rule — masked |
| conditional join **+ early return** | **ACCEPTED — this is the live hole** |

What actually happens now: a *conditionally* joined ThreadHandle trips the
join-on-all-paths LEAK check —

```
zercheck: ThreadHandle 'th' not joined before function exit — add th.join() or detach explicitly
```

— which fires *before* the borrow check is ever consulted. So the leak rule masks the
race rule. This is a SECOND instance of this entry's own test-design gotcha (the first
was: a branch condition that mentions the borrowed variable trips the read check first).
**The lesson generalizes: on this checker, a negative test proves nothing until you read
the actual diagnostic — several rules can reject the same program, and only one of them
is the one under test.**

**CORRECTED reproduction — satisfies the join discipline and still races:**

```zer
struct Work { u32 x; }
u32 flag_in = 0;
u32 worker(*Work w) { w.x = 1; return 0; }
u32 main() {
    Work work;
    ThreadHandle th = spawn worker(&work);
    if (flag_in == 99) { th.join(); return 0; }   // path A: joined, then exits
    work.x = 2;          // path B: thread STILL RUNNING -> REAL DATA RACE
    th.join();
    return 0;
}
```

`ACCEPTED` — compiles clean. The race is visible in the emitted C:

```c
462:    pthread_join(th, NULL);      /* path A, inside the if */
466:    _zer_t4 = work.x = 2;        /* path B, thread still running */
467:    pthread_join(th, NULL);
```

The early `return` is what makes it legal to the leak rule (path A is joined and exits;
path B joins at the end) while leaving the borrow live across the merge on path B.

**Controls that still behave correctly** (so the fix must preserve both):
- same-block write while borrowed -> REJECTED with the right message
  (`cannot write to 'work' while it is borrowed by a scoped spawn`)
- join on every path *before* the access -> ACCEPTED

**Revised tripwire set** (supersedes the three above — the first two would now pass for
the wrong reason and must be rewritten to this shape):

```
tests/zer_fail/borrow_join_one_arm_return.zer   must FAIL — currently ACCEPTED
tests/zer/borrow_join_all_paths.zer             must COMPILE — pins against over-correction
tests/zer_fail/borrow_same_block.zer            must FAIL — pins the rule still fires at all
```

The third is new and matters: it is the only one that would catch the borrow check being
disabled outright while the leak check silently absorbs the negative tests.

**Consequence for the soundness theorem:** this class currently fails OPEN at a CFG
join. An analysis that fails open cannot discharge a forward-simulation obligation —
the abstract state does not over-approximate the concrete one — so `checker_sound` is
not merely unproven for scoped borrows, it would be **false**. Same standing as the
provenance-at-indirect-call hole recorded elsewhere: these are prerequisites for the
Lean port, not independent bugs.

## Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.

When a `tests/zer_gaps/` reproducer is fixed, move it to
`tests/zer_fail/` so it becomes a permanent regression guard.
