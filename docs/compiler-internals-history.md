# Compiler Internals — history (archive)

**Append-only. Do not read this file whole.** Dated session narratives, migration and
phase records, release history and per-bug stories that used to sit in
`docs/compiler-internals.md`. They explain how the compiler got here; the living file
explains how it is. Look something up by title, BUG number or function name:

    grep -n "BUG-577" docs/compiler-internals-history.md

Text is verbatim. Seeded 2026-11-14 with the sections whose HEADINGS marked them as dated
records (the old file's intro block is included first). New entries go at the END.

---

## (the pre-2026-11-14 intro block of compiler-internals.md)

> **How to use this file.** It is ~1 MB: do not read it end to end. `grep -n '^## '
> docs/compiler-internals.md` is its table of contents; read the section you need.
> Companion documents (split out of `CLAUDE.md` on 2026-11-13 so that the always-loaded file
> stays small): **`docs/multi-site-classes.md`** — the registry of safety questions answered
> at N sites (sites, the ONE query, the gate, the history); **`docs/session-playbook.md`** —
> process lessons with their evidence (tests, audits, harvesting, debugging, proof standards).
> A citation `CLAUDE.md "<title>"` below that `CLAUDE.md` no longer contains resolves to one
> of those two files. Open gaps live in `docs/limitations.md`, never only here.

## Full Codebase Audit + Refactor (2026-04-14)

Deep audit of all 25,757 compiler lines. 15,000 lines read directly, 100% pattern-searched.

### Refactor B1: `track_dyn_freed_index()` (checker.c)
Unified Pool.free/Slab.free DynFreed tracking into single helper. Was exact 20-line copy-paste (caused BUG-471). Both `pool.free` and `slab.free` handlers now call `track_dyn_freed_index(c, node)`.

### BUG-506: 13 missing `type_unwrap_distinct` sites
**Emitter (6 sites):** var-decl optional init (3232), init_type ident/expr (3259, 3272), comptime call (1438), global var null init (4925), if-unwrap condition (3362). All checked `type->kind == TYPE_OPTIONAL` without unwrapping distinct. `distinct typedef ?u32 MaybeId; MaybeId x = null;` failed.

**Checker (7 sites):** cross-module collision (182), `*void`/`[]void` (1108/1128), `??T` nesting (1117), const/volatile propagation (1405-1424), comptime enum resolve (1553), resource assignment (2614), string return mutable slice (7566).

### Buffer over-read fix (5 sites)
`snprintf` return value used unclamped in `memcpy`. Clamp added: `if (sn_len >= sizeof(buf)) sn_len = sizeof(buf) - 1;` at lines 746, 1320, 8521, 8538, 8554.

### Refactor B2: `check_union_switch_mutation()` (checker.c)
Union switch lock check was duplicated between pointer-auto-deref union and direct union field (~50 lines each, identical). Extracted to `check_union_switch_mutation(c, field_object)`. Walks mut_root, checks name match + pointer alias + precise key, emits error. Both sites now: `if (check_union_switch_mutation(c, node->field.object)) { result = ty_void; break; }`. Net -38 lines.

### A7: Spawn string literal safety check (checker.c)
Added string-literal-to-mutable-slice check in NODE_SPAWN arg loop. Same check as regular NODE_CALL (line 3871). Without it, `spawn process("hello")` where `process([]u8 data)` would let spawned thread write to .rodata.

### A16/A17: Fixed arrays → stack-first dynamic
- `LabelInfo labels[128]` in `check_goto_labels` → stack-first with arena overflow
- `FieldDecl fields[128]` in `parse_container_decl` → stack-first with `parser_alloc` overflow (matches `parse_struct_decl` pattern)

### A18: Volatile bounds check temps
`__auto_type` strips volatile. Bounds check slice temps (emitter.c:2028, 2204) now use `__typeof__(expr)` to preserve volatile. Same fix pattern as BUG-319 captures.

### C1/C2: Zig test runner
Created `zig_tests/run_tests.sh` (36 tests: 31 positive, 5 negative). Added to Makefile `check` target. Previously existed but never automated.

### Remaining refactors documented in `docs/ZER_Refactor.md`
**ALL REFACTORS EXECUTED** except B5-B6 and B11 (deferred to v0.4 — intentional structural differences between Pool/Slab, not pure duplication).

### Completed refactors (second batch, same session):
- **B3:** Orelse emission → `emit_opt_null_check`/`emit_opt_unwrap` helpers. 4 blocks consolidated. Net -64 lines.
- **B4:** `emit_opt_wrap_value()` helper — 3 identical `{val, 1}` wrapping sites consolidated.
- **B7:** Return optional wrapping → `emit_opt_wrap_value`.
- **B8:** Union typedef emission → `EMIT_UNAME()` local macro (12 repetitions → 12 calls).
- **B10:** `handle_key_arena()` in zercheck.c — 27 `char key[128]` sites → arena-allocated. Deep expressions no longer silently untracked.
- **A15:** Spawn validation: `is_literal_compatible` + `validate_struct_init` added.
- **A19:** `emit_type_and_name` handles `distinct(?funcptr)`.
- **A20:** Module-qualified call rewrite unwraps distinct.

### ctags-Guided Audit (2026-04-14, second pass)
Used `make tags` (Universal Ctags) to query codebase structure instead of reading source. Found 3 bugs in ~5K tokens:
1. `resolve_type_for_emit` missing TYNODE_SLAB/BARRIER/SEMAPHORE/CONTAINER — fallback returned `ty_void`
2. `resolve_type_for_emit` volatile propagation without `type_unwrap_distinct` (same A11 class)
3. Duplicate `_comptime_global_scope` declaration (lines 1082/1570)

**Methodology:** Query ctags for functions returning `Type*`, cross-reference with TYNODE enum values, grep for missing switch cases. 40x more efficient than brute-force reading.

**For fresh sessions:** Run `make tags` first. Use `grep "function_name" tags` to find locations. Use `grep "pattern" file.c` to find specific code. Never read full files speculatively.

### Flag-Handler Matrix Audit (2026-04-14, automated)
(**Superseded 2026-09-25:** the tool is now BEHAVIOURAL — `bash tools/audit_matrix.sh ./zerc`, one compiled program per construct × context, in `make check`. The grep version below had drifted onto decoy `case` labels.) The original `bash tools/audit_matrix.sh checker.c` cross-referenced control-flow NODE_ handlers against context flags. Found 5 missing checks on first run:

| Node | Missing Flag | Why it's a bug |
|---|---|---|
| YIELD | `defer_depth` | yield in defer corrupts Duff's device state machine |
| YIELD | `critical_depth` | yield in @critical = lock held across suspend = deadlock |
| AWAIT | `defer_depth` | same as yield |
| AWAIT | `critical_depth` | same as yield |
| SPAWN | `in_interrupt` | pthread_create in ISR = unsafe |

**How it works:** Extracts each NODE_ handler body from check_stmt via grep+sed, scans for flag references. Missing reference where safety contract requires one = BUG. Takes <5 seconds, finds bugs that manual reading misses.

**Extend to other subsystems:** Define rows (handlers) and columns (flags/preconditions). Same grep-based cross-reference pattern works for emitter type dispatch, zercheck state checks, any flag×handler matrix.

**Run after adding:** any new NODE_ type, any new context flag, any new control-flow statement. The matrix exposes gaps automatically.

**Note:** The 5 matrix bugs are now FIXED by FuncProps (function summaries). The audit matrix remains useful for scope-exit flag checks (return/break/continue/goto) which FuncProps doesn't cover.

### Function Summaries — FuncProps (2026-04-14, tracking system #29)

Inferred function properties on Symbol, lazily computed via DFS with cycle detection.
Fixes the 5 matrix audit bugs (direct + transitive) and absorbs `has_atomic_or_barrier()`.

**Data structure:** `Symbol.props` — `{computed, in_progress, can_yield, can_spawn, can_alloc, has_sync}`

**Key functions:**
- `scan_func_props(c, node, parent_sym)` — recursive AST walker, sets all bools in one pass, follows callees transitively via `ensure_func_props`
- `ensure_func_props(c, sym)` — lazy compute + DFS cycle detection (`computed` + `in_progress` flags, same pattern as `FuncSharedTypes` deadlock DFS)
- `check_body_effects(c, body, line, ban_yield, msg, ban_spawn, msg, ban_alloc, msg)` — called at @critical, defer, interrupt entry points. Creates temp scan of the restricted body subtree.

**Where checks happen:**
- NODE_CRITICAL handler — bans can_yield, can_spawn (before `check_stmt`)
- NODE_DEFER handler — bans can_yield (before `check_stmt`)
- NODE_INTERRUPT in check_func_body — bans can_spawn, can_alloc (before `check_stmt`)
- NODE_SPAWN handler — bans `c->in_async` (direct flag, not FuncProps — spawn in async is about ownership, not transitivity)
- NODE_SPAWN handler — reads `func_sym->props.has_sync` (replaces `has_atomic_or_barrier()` call)

**Transitive following:** NODE_CALL in scanner → `scope_lookup` callee → `ensure_func_props(callee)` → merge callee props. Also follows module-qualified calls (`module__func` mangled name). Function pointer calls: conservative (assumed no effects).

**Ban decision framework:** Hardware/OS constraint → emission impossibility → needs runtime → needs type system → if none, track. All current bans justified. See CLAUDE.md.

**Design doc:** `docs/FunctionSummaries.md` — full problem statement, rejected approaches (flags, table-driven, effect annotations), architecture, edge cases, testing strategy.

### Why B5-B6 and B11 are deferred (NOT pure duplication):
- **B5-B6:** Pool alloc emits 6 inline args (`slots, sizeof(slots[0]), gen, used, count, &ok`). Slab emits 2 (`&slab, &ok`). A helper needs `is_pool` flag = same line count, worse readability. v0.4 table-driven solves this properly.
- **B11:** Pool uses `pool.elem`/`pool.count`, Slab uses `slab.elem` + ISR ban. Helper needs type flag + element accessor + optional count + ISR flag = more parameters than the code it replaces.

### Full list of unified helpers after this session (28 total)
| # | Helper | File | What |
|---|---|---|---|
| 1 | `vrp_invalidate_for_assign` | checker.c | VRP range invalidation (R1) |
| 2 | `emit_async_orelse_block` | emitter.c | Async orelse emission (R2) |
| 3 | `emit_shared_ensure_init` | emitter.c | Shared struct mutex+condvar init (R3) |
| 4 | `track_dyn_freed_index` | checker.c | Pool/Slab dynamic-index free tracking (B1) |
| 5 | `check_union_switch_mutation` | checker.c | Union switch lock check (B2) |
| 6 | `should_track_move` | zercheck.c | Move struct type detection |
| 7 | `is_handle_invalid` | zercheck.c | Handle use-check (FREED/MAYBE/TRANSFERRED) |
| 8 | `is_handle_consumed` | zercheck.c | Handle merge-check |
| 9 | `zc_report_invalid_use` | zercheck.c | Error message by handle state |
| 10 | `is_void_opt` | emitter.c | ?void type detection |
| 11 | `emit_opt_null_check` | emitter.c | Optional null check emission |
| 12 | `emit_opt_unwrap` | emitter.c | Optional value unwrap emission |
| 13 | `emit_opt_null_literal` | emitter.c | Optional null literal emission |
| 14 | `emit_return_null` | emitter.c | Return null for current function |
| 15 | `type_can_carry_pointer` | checker.c | Escape flag propagation filter |
| 16 | `propagate_escape_flags` | checker.c | Local/arena-derived flag copy |
| 17 | `check_isr_ban` | checker.c | ISR heap allocation ban |
| 18 | `find_or_create_auto_slab` | checker.c | Task.alloc auto-slab |
| 19 | `check_volatile_strip` | checker.c | Cast/intrinsic volatile check |
| 20 | `validate_struct_init` | checker.c | Designated init field validation |
| 21 | `find_handle_local` | zercheck.c | Scope-aware handle registration |
| 22 | `emit_zero_value` | emitter.c | Zero value for any return type |
| 23 | `scan_func_props` | checker.c | Recursive AST walker for function properties (FuncProps) |
| 24 | `ensure_func_props` | checker.c | Lazy DFS compute + cache for FuncProps |
| 25 | `check_body_effects` | checker.c | Context entry point effect checker (@critical/defer/interrupt) |

## Red Team Audit Fixes — Round 12 (2026-04-13, Gemini — 5 claims, 3 real bugs)

### BUG-502: VRP compound assignment range invalidation
Range invalidation was gated by `if (node->assign.op == TOK_EQ)`. Compound ops (`+=`, `-=`, `*=`, etc.) skipped — left stale proven ranges. Fix: ALL assignment ops trigger invalidation. `TOK_EQ` tries to derive new range from value. Compound ops wipe unconditionally.

**VRP architecture after BUG-475/478/479/502:**
1. `&var` → `address_taken=true`, permanently invalid (TOK_AMP handler)
2. Function call → global variable ranges wiped (NODE_CALL handler)
3. Direct assignment `=` → derive new range or wipe (NODE_ASSIGN handler)
4. Compound assignment `+=/-=/*=` → wipe unconditionally (NODE_ASSIGN handler)
5. Comptime calls exempt (pure)

### BUG-503: Async expr-stmt orelse restructured
NODE_EXPR_STMT handler intercepts orelse+block in async mode before `emit_expr`. Uses pre-scanned state struct temp. Separate statements (no GCC statement expression). Result discarded (expr-stmt).

**Async orelse emission summary (3 paths):**
- **Var-decl init** (BUG-481): `self->tmp = expr; if (!tmp.has_value) { block } self->x = tmp.value;`
- **Expr-stmt** (BUG-503): `self->tmp = expr; if (!tmp.has_value) { block }`
- **Expression level** (V46): GCC limitation — can't fix, GCC error is clear

### BUG-504: Condvar intrinsic initialization
All 4 condvar intrinsics (@cond_wait, @cond_timedwait, @cond_signal, @cond_broadcast) now call `_zer_mtx_ensure_init_cv` before `pthread_mutex_lock`. Previously, if the first access to a shared struct was via condvar intrinsic (no prior field access to trigger auto-lock init), the mutex/condvar were uninitialized.

## Red Team Audit Fixes — Round 11 (2026-04-13, Gemini — 5 claims, 4 real bugs)

### BUG-498: Packed struct sync primitive rejection
Semaphore/Barrier/shared struct fields banned inside `packed struct`. `pthread_mutex_t` requires natural alignment — packed offsets cause hard fault on ARM/RISC-V. Added to struct field validation alongside existing Pool/Ring/Slab rejection.

### BUG-499: Async param shadowing rejection
Variable shadowing of function params banned in async functions. Params and locals share state struct (`self->name`) — shadowing overwrites param value. Regular functions unaffected. Check at NODE_VAR_DECL: if `c->in_async` and name matches existing symbol at different line.

### BUG-500: shared(rw) read-only multi-type allowed
Deadlock check now skips when BOTH shared types are `is_shared_rw` AND statement is read-only. `pthread_rwlock_rdlock` allows concurrent readers — no deadlock. Write statements still trigger deadlock check.

### BUG-501: array.len emitter fix
Range-for desugaring generates `collection.len` for loop condition. For TYPE_ARRAY, emitter now emits the compile-time array size as literal (`4U`) instead of invalid C `.len` field access. Checker already handled `array.len` → `ty_usize` — emitter was the missing piece.

**Pattern:** Parser desugaring can generate AST that's semantically valid but emitter-invalid. The checker validates types correctly (array.len → usize), but the emitter must handle the C representation difference (slice = struct with .len, array = raw C array without .len).

## Red Team Audit Fixes — Rounds 9-10 (2026-04-13, Gemini — 10 claims, 5 real bugs)

### BUG-493: Packed struct atomic → compile error
@atomic_* on packed struct fields causes hard fault on ARM/RISC-V (misaligned atomics). Checker walks &field operand to root, checks `is_packed`.

### BUG-494: Move struct eager var-decl registration
Lazy registration (`zc_ensure_move_registered`) found outer handle for inner shadow. Fix: NODE_VAR_DECL eagerly registers move structs at current `scope_depth`. `find_handle` (highest depth) returns inner for inner use. First attempt (shadow logic in `zc_ensure_move_registered`) broke 7 loop tests — declaration-site is the correct location per rule #8.

### BUG-495: Async orelse prescan into expression trees
`prescan_expr_for_orelse` recursively scans NODE_BINARY, NODE_CALL, NODE_ASSIGN, etc. for nested orelse blocks. Registers state struct temp. Expression-level yield in orelse is a GCC limitation ("switch jumps into statement expression") — not fixable, GCC error message is clear. Var-decl level safe via BUG-481 restructured emission.

### BUG-496: Arena value escape + comptime cleanup
Arena VALUE (not pointer) stored in global → dangling buf pointer. Fix: checker checks TYPE_ARENA (and struct fields containing Arena), only rejects LOCAL source → global target. Global arena → global = safe.

Comptime `eval_comptime_block`: 6 early return paths skipped `ct_done` cleanup label. Array bindings leaked. All changed to `goto ct_done`.

### Async architecture summary after rounds 4-10

All async locals now promoted to state struct regardless of scope depth:
- **collect_async_locals**: fully recursive (NODE_BLOCK, NODE_IF, NODE_FOR, NODE_WHILE, NODE_SWITCH, NODE_DEFER, NODE_CRITICAL, NODE_ONCE)
- **prescan_async_temps**: recursive + `prescan_expr_for_orelse` for expression trees
- **State struct fields**: params (BUG-477) + all locals (BUG-490) + orelse temps (BUG-481/495) + static excluded (BUG-486)
- **Emission paths**: var-decl orelse = restructured statements. Expression-level orelse = GCC statement expression (yield = GCC error). Regular locals = `self->name`.

Same approach as Rust's MIR generator transform — promote everything that lives across yield.

## Red Team Audit Fixes — Round 8 (2026-04-13, Gemini — 4 claims, 3 real bugs)

**Summary:** Eighth Gemini audit. 3 real bugs (BUG-490/491/492), 1 false (V39 = per-statement-group locking, same as V12).

### BUG-490: Async sub-block locals — recursive collect_async_locals
`collect_async_locals` was top-level only. Sub-block, if-body, loop-body locals stayed on C stack — stale after yield. Fix: fully recursive scan into all block types. `add_async_local` helper with dedup by name. State struct field emission also recursive (iterative stack traversal). Every local in an async function is now promoted regardless of scope depth.

### BUG-491: Spawn qualifier validation
NODE_SPAWN validated shared vs non-shared but skipped const/volatile qualifier checks. Fix: after pointer safety check, resolve function param types and validate qualifiers. Same checks as NODE_CALL handler (const→mutable = error, volatile→non-volatile = error).

### BUG-492: Dynamic covered_ids (no fixed buffers)
`covered_ids[64]` silently dropped allocations 65+. Fix: stack-first dynamic pattern (`int stack[64]; int *arr = stack; int cap = 64;` with realloc). **Rule added to CLAUDE.md: NEVER use fixed-size buffers for dynamic data.** Same pattern as parser RF9.

## Red Team Audit Fixes — Round 7 (2026-04-12, Gemini — 5 claims, 2 real bugs)

**Summary:** Seventh Gemini audit. 2 real bugs (BUG-488/489), 3 false/design.

### BUG-488: Zercheck scope-aware handle tracking (refactor)
Variable shadowing caused false positive: inner `Handle h` freed → outer `Handle h` falsely FREED. Root cause: flat name matching in `find_handle` with no scope concept.

**Architecture after refactor:**
- `find_handle(ps, name, len)` — source lookup, returns highest `scope_depth` match. For UAF/alias checks. ~20 call sites.
- `find_handle_local(ps, name, len)` — destination registration, returns CURRENT scope match only. For var-decl alloc/alias. ~6 call sites. Returns NULL for outer-scope handles → `add_handle` creates new shadow.
- `scope_depth` on PathState: NODE_BLOCK increments on entry, decrements on exit.
- `scope_depth` on HandleInfo: set by `add_handle` from `ps->scope_depth`.
- Block exit: removes inner-scope handles shadowing outer handles. Propagates state only if same `alloc_id` (aliases of same allocation).
- `pathstate_copy` preserves `scope_depth` for if/else branch copies.

**Pattern:** Mirrors checker's `scope_lookup` (any scope chain) vs `scope_lookup_local` (current scope only). Same two-function separation, same semantics.

**Why multiple patch attempts failed:** The flat array assumption was in `find_handle`, called 25 times. Patching individual call sites (last-match, scope_depth flag on one path) broke other paths. The proper fix was the two-function separation — clean architectural boundary between "reading handle state" and "registering new handles."

### BUG-489: Runtime @inttoptr alignment check
Variable-address `@inttoptr` had range check but no alignment check. Fix: emitter emits `if (_zer_ma0 % align != 0) _zer_trap("unaligned")` after range check. Alignment from target type width.

### Not bugs (V33, V34, V37)
- V33: VRP conservative for compound keys — bounds check NOT eliminated
- V34: Async cancellation — design limitation, accepted for firmware
- V37: Union variant access requires switch — attack blocked

## Red Team Audit Fixes — Round 6 (2026-04-12, Gemini — 4 claims, 3 real bugs)

**Summary:** Sixth Gemini audit. 3 real bugs (BUG-485/486/487), 1 false.

### BUG-485: *opaque comparison — .ptr extraction for track_cptrs
With `--track-cptrs`, `*opaque` is `_zer_opaque` struct (not `void*`). C can't compare structs with `==`/`!=`. Fix: emitter NODE_BINARY detects `TYPE_POINTER(TYPE_OPAQUE)` and emits `.ptr` on both sides. Only fires when `e->track_cptrs` — without it, `*opaque` is plain `void*`.

### BUG-486: Async static locals — skip state struct promotion
`static u32 count` inside async must stay as C `static` in poll function (shared across instances). `collect_async_locals` and struct field emission now check `!is_static`.

### BUG-487: Union move variant — ban assignment to prevent resource leak
`m.k.fd = 42; m.id = 100;` silently overwrites move struct resource. Fix: checker NODE_ASSIGN scans union type for move struct variants → compile error. Same as Rust's enum Drop — must use switch for safe variant transitions.

**Pattern from rounds 5-6:** Feature interactions are the primary bug source. `*opaque` + `==` (emitter vs type representation), `async` + `static` (promotion vs persistence), `union` + `move struct` (variant vs ownership). Each feature works alone; bugs appear at intersections.

### Not a bug (V32): `u8[@size(usize)]` compiles correctly
`@size(usize)` resolves to `sizeof(size_t)` in emitted C. Target-portable.

## Red Team Audit Fixes — Round 5 (2026-04-12, Gemini — 5 claims, 3 real bugs)

**Summary:** Fifth Gemini audit. 3 real bugs (BUG-482/483/484), 2 false.

### BUG-482: Async struct names module-mangled
`_zer_async_init` collides when two modules have `async void init()`. Fix: `emit_async_func_from_ir` builds mangled name `module__funcname` at top, uses throughout all `_zer_async_` emissions. Same pattern as `EMIT_MANGLED_NAME`.

### BUG-483: Condvar init inside CAS winner path
`_zer_mtx_ensure_init` sets `inited=1`, then `if (!inited) condvar_init()` is always false — condvar never initialized. Fix: `_zer_mtx_ensure_init_cv(mtx, inited, cond)` initializes condvar alongside mutex in CAS winner. `_zer_mtx_ensure_init` is wrapper calling `_cv(..., NULL)`. Semaphore acquire/release pass `&s->_zer_cond`.

### BUG-484: Move struct orelse fallback transfer
`Token b = opt orelse a` — `a` not marked HS_TRANSFERRED. Fix: after primary move_src transfer, also check `orelse.fallback` for move struct types. Handles direct ident, array element, struct field. Same type detection as primary path.

### Not bugs (V16, V20)
- V16: Union variant access requires switch + tag check — "corpse read" impossible
- V20: VRP unsigned clamp makes large u64 unprovable — bounds check stays

## Red Team Audit Fixes — Round 4 (2026-04-12, Gemini — 4 claims, 1 real bug)

**Summary:** Fourth Gemini audit. 1 real bug (async orelse stack ghost), 1 doc fix (ABA gen counter), 2 false (auto-lock group + VRP coercion).

### BUG-481: Async yield in orelse — state struct temp promotion (proper fix)
`u32 x = maybe_get() orelse { yield; 42; }` — `_zer_tmp0` is stack local, stale after yield+resume.

**Architecture (same as Rust's MIR generator transform):**
1. `prescan_async_temps(e, body)` — recursive pre-scan finds NODE_ORELSE with block fallback in async body. Records `AsyncTemp` entries with type + temp_id.
2. `emit_async_func_from_ir` adds `_zer_async_tmpN` fields to the state struct typedef.
3. In async var-decl orelse path: split GCC statement expression into separate statements using `self->_zer_async_tmpN`. Temp survives yield.

Non-async code unchanged (efficient GCC statement expression). No language restriction — yield inside orelse blocks now works correctly.

### Not bugs (V12, V15)
- V12: auto-lock group — per-statement locking, unlock before return
- V15: VRP array coercion — slice doesn't alias local variables

## Red Team Audit Fixes — Round 3 (2026-04-12, Gemini — 4 claims, 4 real bugs)

**Summary:** Third Gemini red team audit. All 4 claims were real bugs.

### BUG-477: Async function parameters not in state struct
`async void worker(u32 x) { yield; u32 after = x; }` — `x` undeclared in poll function. `collect_async_locals` only scanned NODE_VAR_DECL. Fix: add params to async_locals list + state struct fields + init function signature. Checker init registration updated to include original params.

### BUG-478: VRP global variable ranges not invalidated after function call
`if (g_idx < 10) { sneaky(); arr[g_idx]; }` where `sneaky()` sets g_idx=100. Fix: after NODE_CALL, scan VRP stack for global variables and wipe their ranges. Skip comptime calls (pure).

### BUG-479: VRP address-taken flag for pointer aliasing
`*u32 p = &idx; if (idx >= 4) return; p[0] = 100; arr[idx]` — guard re-narrowed idx after invalidation. Fix: `address_taken` flag on `struct VarRange`. When `*T p = &var`, flag the aliased variable. `push_var_range` skips narrowing for `address_taken` entries — guards cannot override.

**VRP architecture after BUG-475/478/479:**
Three invalidation triggers:
1. `&var` passed to function call → wipe var's range (BUG-475)
2. Any function call → wipe all global variable ranges (BUG-478)
3. `*T p = &var` in var-decl → mark var as `address_taken`, permanently prevent narrowing (BUG-479)

### BUG-480: Move struct value capture in switch
`switch (m) { .k => |val| { ... } }` where val is move struct — creates copy, two owners. Fix: same check as V13 (if-unwrap): `type_unwrap_distinct(type)->kind == TYPE_STRUCT && is_move` → error "use |*val|". Applied to both union-switch and optional-switch capture paths.

## Red Team Audit Fixes — Round 2 (2026-04-12, Gemini — 7 claims, 3 real bugs)

**Summary:** Second Gemini red team audit. 7 attack vectors tested, 3 real bugs found (BUG-474/475/476), 4 false or already-handled.

### BUG-474: Transitive deadlock depth limit too shallow
`_shared_scan_depth < 4` → `< 8`. Deep call chains (5+ levels) between shared struct accesses now detected. Matches spawn transitive scan depth.

### BUG-475: VRP not invalidated on &variable passed to function call
When `&var` is passed as a function argument, `var`'s value range MUST be wiped — the callee may modify it through the pointer. Fix: NODE_CALL handler in `check_expr` scans args for `NODE_UNARY(TOK_AMP)`, walks to root ident, sets range to `[INT64_MIN, INT64_MAX]`. Without this fix, bounds checks were eliminated based on stale ranges.

**Pattern for future VRP additions:** Any code path where a variable's value could change through an alias (pointer arg, struct field mutation) must invalidate the VRP range. Currently handled: direct assignment, compound assignment, function call with `&var` arg.

### BUG-476: Move struct from array element / struct field not tracked
`Token copy = arr[0]; arr[0].kind` compiled — zercheck only tracked NODE_IDENT sources for move transfer. Extended to use `handle_key_from_expr()` for compound keys. Also detects move struct type through TYPE_ARRAY element type.

### Not Bugs (4 claims)
- V2 (Slot retirement DoS): Gen counter wraps (`gen++; if (gen==0) gen=1`), no retirement. Slots always reuse. Gemini fabricated "permanently retired" mechanism.
- V5 (Async defer bypass): Defer fires at function completion (after all yields), not per-yield. Correct by design — Duff's device places defer block at body end.
- V6 (Zero-handle collision): Gen starts at 1 (`if (gen[i]==0) gen[i]=1`). Auto-zeroed handle gen=0 never matches gen=1. Already fixed.
- V7 (Comptime budget global): Budget resets per top-level call (`if (depth==1) _comptime_ops=0`). Per-call, not per-compilation.

## Red Team Audit Fixes — Round 1 (2026-04-12, Gemini red team — 8 attempts, 5 real bugs)

**Summary:** External AI (Gemini) performed red team audit on ZER-LANG safety guarantees. 8 attack vectors tested: 5 were real bugs (V1,V3,V4,V5,V6), 3 were already caught by existing checks (V2,V7,V8).

### Transitive Deadlock Detection
`collect_shared_types_in_expr` now scans called function bodies transitively (depth 4) for shared type field accesses. Catches: `a.x = helper()` where `helper()` accesses `b.y` — different shared type in same statement = potential AB-BA deadlock. Required adding NODE_RETURN/NODE_EXPR_STMT/NODE_VAR_DECL handling in the expression scanner (callee body statements were being skipped).

### Comptime Global Instruction Budget
`_comptime_ops` counter in `eval_comptime_block`. Incremented per loop iteration (both for and while/do-while). Cap: 1,000,000 total operations per comptime call. Prevents nested loop DoS (10000×10000 = 100M iterations). Resets on depth==1 (top-level call). Returns CONST_EVAL_FAIL on budget exceeded.

### Naked Function Body Validation (V4)
Naked functions (`__attribute__((naked))`) must only contain `asm` and `return` statements. Non-asm code (var-decl, assignment, etc.) uses stack that was never allocated (no prologue). Checker scans body block and errors on first non-asm, non-return statement.

### Thread-Unsafe Slab/Pool/Ring from Spawn (V5)
Pool, Slab, Ring alloc/free/push/pop have non-atomic metadata access. Accessing global Pool/Slab/Ring from spawned thread is a data race. `scan_unsafe_global_access` no longer skips TYPE_POOL/TYPE_SLAB/TYPE_RING — only TYPE_ARENA (bump allocator, single-threaded reset) and TYPE_BARRIER (has own mutex) are safe. Also fixed: NODE_FIELD and callee expression scanning were missing from the spawn global scanner.

### Container Monomorphization Depth Limit (V6)
`_container_depth` static counter in TYNODE_CONTAINER resolution. Cap: 32. Prevents compiler hang/crash from self-referential containers like `container Node(T) { ?*Node(T) next; }`. Also: `subst_typenode()` recursive TypeNode substitution replaces 5 one-level pattern matches — handles T at any nesting depth (`?*Container(T)`, `[]*T`, etc.).

### Move Struct Value Capture Banned (V13)
`if (opt) |k|` where unwrapped type is move struct → compile error. Value capture copies the move struct, creating two owners. Fix: checker detects `should_track_move(unwrapped)` in non-pointer capture path of if-unwrap, forces `|*k|` pointer capture instead.

### Async Shared Struct Access Banned (V14)
Shared struct field access inside async function body → compile error. Lock acquired by auto-locking may be held across yield/await = deadlock. `c->in_async` flag set during async function body check. Same approach as Rust (MutexGuard not Send across .await).

### Already Caught (V2, V7, V8, V12, V15, V16)
- V2: Union mutation via *opaque inside switch arm → "cannot take address of union inside switch arm"
- V7: Union containing move struct → "cannot read union variant directly — must use switch"
- V8: @ptrtoint(&local) returned → "cannot return @ptrtoint of local" (fixed earlier this session)
- V12: Container type-id collision → each stamp gets unique `c->next_type_id++`, @ptrcast catches mismatch
- V15: Comptime @ptrtoint → eval fails (can't resolve pointer addresses at compile time)
- V16: Move struct partial field access → zercheck marks entire struct as HS_TRANSFERRED, any field access errors

### Not Bugs (V9, V11)
- V9: Async defer bypass → NOT a bug, defer fires correctly on async completion (Duff's device handles it)
- V11: Same-type instance deadlock → NOT a real deadlock. Per-statement locking means locks never overlap. Atomicity concern is design limitation (use single shared struct for atomic multi-field ops)

## Session v0.2.1 Final (2026-03-30/31)

525 checker + 233 E2E + 50 zercheck. 4 audit rounds, 4 bugs fixed. Only 2 runtime cases: *opaque from cinclude + INT_MIN/-1. Auto-discovery removed (2026-04-01) — replaced with mmio startup validation via @probe of declared ranges.

## Session v0.2.2 (2026-04-01)

**MMIO redesign:**
- Removed 5-phase auto-discovery (~150 lines, unreliable, chip-specific)
- Added mmio startup validation: @probe declared range starts at boot
- Universal signal() fault handler: replaces all platform-specific handlers. Dual-mode: @probe recovers, normal code traps. Re-installs signal after longjmp (SysV resets to SIG_DFL).
- @probe takes uintptr_t (was uint32_t — broken on 64-bit)
- __STDC_HOSTED__ guard: freestanding compiles don't fail (no signal/setjmp)
- x86 bare-metal included in mmio validation (was ARM/RISC-V/AVR only)
- @inttoptr alignment check: address must match type alignment (u32=4, u16=2, u64=8)
- 4-layer MMIO safety: range + alignment + boot probe + runtime fault handler

**New safety categories (13 → 17):**
- Interrupt safety: shared globals between ISR and main without volatile → error. Compound assign on shared volatile → error (non-atomic race). Pass 4 post-check.
- Stack depth: call graph DFS, frame size estimation, recursion detection → warning. Pass 5 post-check.
- @inttoptr alignment: compile-time check, universal
- Cross-platform @ptrtoint portability: warning when stored in fixed-width type

**zer-convert P0+P1 fixes (108 → 139 tests):**
- volatile qualifier preserved and reordered
- extern/inline/restrict/register/__extension__ stripped
- #if defined(X) → comptime if (X) (expand defined() operator)
- Number suffixes (U/L/UL/ULL) stripped from literals
- MMIO casts → @inttoptr (numeric address detection)
- (uintptr_t)ptr → @ptrtoint (was @truncate)
- uintptr_t/intptr_t added to type_map
- Include guard (#ifndef/#define) detection and stripping
- void ** → **opaque
- Stringify (#), token paste (##), variadic (__VA_ARGS__) macros → auto-extracted to companion .h

559 checker + 238 E2E + 50 zercheck + 139 convert = ~1,700+ tests. All passing.

**ASM implementation (all 5 phases):**
- `@critical { }`: NODE_CRITICAL in parser/checker/emitter. Per-arch interrupt disable/enable via #if defined. Hosted x86 uses __atomic_thread_fence (CLI needs ring 0).
- `@atomic_*`: Checker validates ptr-to-integer first arg. Emitter emits `__atomic_fetch_add` etc. All 8 operations: add/sub/or/and/xor/cas/load/store.
- Extended asm: Parser bypasses lexer — scans raw source for matching `)` (colon not a ZER token). Emits `__asm__ __volatile__(...)` verbatim.
- `naked`: `func_decl.is_naked` flag. Emitter prepends `__attribute__((naked))`.
- `section("name")`: stored on func_decl and var_decl. Emitter prepends `__attribute__((section(...)))`.
- Parser detects `section(...)` and `naked` as contextual keywords at top-level before parse_func_or_var.
- **MISRA Dir 4.3 enforcement:** NODE_ASM in check_stmt checks `c->in_naked`. If false → compile error. `in_naked` flag set when entering naked function body in check_func_body, cleared on exit. Asm only in naked functions — regular functions must use @critical or @atomic_*.

**Session 2026-04-01 audit summary (11 rounds, 15 bugs fixed):**
Comptime recursion depth guard, Pool/Slab zero-handle collision, Pool/Slab ABA wrap-to-1, struct wrapper escape (2 patterns), slab calloc 64-bit overflow, stale range on reassignment, compound /= division guard, orelse identity washing, @cstr local-derived (var-decl + direct arg), slice struct escape, nested orelse recursion, field of local-derived, struct field range invalidation, partial struct mutation flag preservation, Handle type_width=64.

**Additional fixes (audit rounds 4-5):**
- Range propagation stale guard: reassignment directly overwrites VarRange entry (not intersect). `i = get_input()` wipes range.
- Compound /= %= forced division guard: added to NODE_ASSIGN alongside NODE_BINARY.
- Struct wrapper escape: `call_has_local_derived_arg` walks NODE_FIELD chains to call root. Var-decl marks struct results as local-derived.
- Slab calloc overflow: `(size_t)nc * _ZER_SLAB_PAGE_SLOTS` prevents 32-bit wrap.
- Pool/Slab ABA: gen capped at 0xFFFFFFFF, retired slots skipped in alloc.
- Pool/Slab zero-handle: gen starts at 1 (not 0), zero Handle never matches.
- Comptime recursion: 3-layer depth guard in eval_comptime_block/call_subst/const_expr_subst.
- Identity washing via orelse: `call_has_local_derived_arg` checks NODE_ORELSE fallback for &local and local-derived idents.
- @cstr local-derived: var-decl marks local-derived when init is @cstr(local_buf,...). `call_has_local_derived_arg` also checks NODE_INTRINSIC(@cstr) args directly.
- Slice struct escape: BUG-360/383 return checks extended to TYPE_SLICE. `call_has_local_derived_arg` detects local TYPE_ARRAY args (array→slice coercion). Var-decl local-derived marking includes TYPE_SLICE.
- Struct field range invalidation: NODE_ASSIGN uses `build_expr_key` on NODE_FIELD targets to invalidate compound key ranges ("s.x") on reassignment.
- Compound /= %= forced division guard: added to NODE_ASSIGN alongside NODE_BINARY.

- Nested orelse recursion: walks through `o1 orelse o2 orelse &x` chains to any depth.
- Field of local-derived struct: `identity(h.p)` where `h` has `is_local_derived` — walks NODE_FIELD to root NODE_IDENT, checks symbol flag.
- Partial struct mutation: field assignments (`h.val = 42`) no longer clear `is_local_derived` on root. Only whole-variable replacement (`h = ...`) clears flags.
- Handle type_width: TYPE_HANDLE returns 64 (was 0 — broke @size and stack estimates).

**`call_has_local_derived_arg` — the escape analysis walker (checker.c line ~383):**
Central function for detecting local pointer escape through function calls. Checks all args of a NODE_CALL for local-derived sources. **9 cases handled:**
1. `&local` — NODE_UNARY(TOK_AMP) with local root ident
2. Local-derived ident — symbol has `is_local_derived` or `is_arena_derived`
3. Local array — TYPE_ARRAY ident that's not global/static (array→slice coercion)
4. Nested call — NODE_CALL arg returning pointer, recurse
5. Orelse fallback — NODE_ORELSE, check fallback for &local or local-derived ident
6. Nested orelse — recurse through chained NODE_ORELSE fallbacks to any depth
7. @cstr — NODE_INTRINSIC("cstr"), check first arg (buffer) for local root
8. Struct field from call — NODE_FIELD chain walking to NODE_CALL root, recurse
9. Field of local-derived — NODE_FIELD chain to NODE_IDENT root, check `is_local_derived`
If adding a new expression type that can carry a local pointer, ADD A CASE HERE. This is the most-patched function in the checker (9 cases added across audit rounds 5-11).

### Interior Pointer UAF Tracking (zercheck.c, 2026-04-07)

**Problem:** `*u32 p = &b.field; free_ptr(b); p[0]` — field-derived pointer used after parent allocation freed. zercheck tracked `b` as FREED but didn't know `p` was derived from `b`.

**Fix — two changes:**

1. **Interior pointer alias tracking in `zc_check_var_init` and NODE_ASSIGN:**
   When init/value is `NODE_UNARY(TOK_AMP)`, walk the operand through field/index/deref chains to the root ident. If root is a tracked handle, assign the same `alloc_id` to the new variable. Uses the same alloc_id mechanism as handle aliasing (`h2 = h1`).

2. **NODE_INDEX UAF check in `zc_check_expr`:**
   Added `case NODE_INDEX:` — checks if the indexed object (`p` in `p[0]`) is a freed handle. Same pattern as existing NODE_FIELD and NODE_UNARY/TOK_STAR UAF checks. Also recurses into object and index sub-expressions.

**Why NODE_INDEX was missing:** zercheck had UAF checks on NODE_FIELD (`b.x`), NODE_UNARY/deref (`*p`), and NODE_CALL args (`func(p)`), but pointer indexing (`p[0]`) fell through to the default case which doesn't check handle state.

**Design note:** Holding a dangling pointer is not itself UB — only dereferencing it is. So NODE_IDENT doesn't need a UAF check. The three dereference paths (field, deref, index) plus the call-arg path cover all actual memory access.

**Tests:** `interior_ptr_safe.zer` (field ptr used before free — compiles), `interior_ptr_uaf.zer` (field ptr used after free — rejected), `interior_ptr_func.zer` (field ptr passed to function after free — rejected).

**Remaining gap:** `@ptrtoint` + integer math + `@inttoptr` creates a pointer with no link to the original allocation. This is guarded by the `mmio` declaration requirement — `@inttoptr` without mmio ranges is a compile error. The `mmio` requirement is the defense, not pointer tracking.

### C-Style Cast Safety Audit (checker.c, 2026-04-07)

**Problem:** NODE_TYPECAST handler only validated cast direction (int↔int OK, struct→int NO) but didn't apply the safety checks that `@ptrcast` enforces for pointer casts.

**7 bugs found in audit (BUG-445 through BUG-451):**
All same root cause — NODE_TYPECAST was a "dumb cast" that bypassed ZER's safety layer.

**Fix — one unified block in NODE_TYPECAST handler:**
When source and target are both pointers:
1. **Qualifier preservation** — const/volatile cannot be stripped (same as @ptrcast BUG-258/304)
2. **Provenance check** — *opaque with known provenance must match target type (same as @ptrcast BUG-393)
3. **Direct *A→*B rejection** — must go through *opaque round-trip for provenance tracking
4. **Provenance propagation** — `(*opaque)sensor` sets provenance on source symbol, AND var-decl walker now follows NODE_TYPECAST (was only following NODE_INTRINSIC)

When crossing pointer/integer boundary:
5. **int→ptr rejected** — must use `@inttoptr` (mmio range validation)
6. **ptr→int rejected** — must use `@ptrtoint` (portability warning)

**Key principle:** C-style cast is syntax sugar for safe operations. It must NEVER bypass a safety check that `@ptrcast`/`@inttoptr`/`@ptrtoint` enforce. If a cast needs to bypass safety, use the explicit intrinsic.

**Allowed C-style casts:** int↔int (widening/narrowing), int↔float, float↔float, bool↔int, *T↔*opaque (with provenance), distinct↔base (with @cast semantics).

**Provenance propagation for NODE_TYPECAST (BUG-454):**
Two sites needed for C-style cast provenance to work:
1. **NODE_TYPECAST handler** — when casting `*T → *opaque`, walk through `&`/field/index to find root ident, set `provenance_type = source` on its symbol.
2. **Var-decl init handler** — when init is `NODE_TYPECAST` producing `*opaque`, extract source type via `typemap_get(c, init->typecast.expr)` and set as `sym->provenance_type`. Same pattern as @ptrcast's `typemap_get(c, init->intrinsic.args[0])`.

Without (2), `*opaque raw = (*opaque)&a` didn't propagate `*A` provenance to `raw`, so `(*B)raw` wasn't caught. Both `@ptrcast` and C-style cast now use the same provenance mechanism.

**Parser limitation:** `(Celsius)raw` where `Celsius` is a user-defined type — parser can't distinguish from `(variable) * expr`. Use `@cast(Celsius, raw)` for distinct typedefs. Not a safety bug — @cast is the correct syntax for distinct types.

**Tests:** `typecast_safe_complex.zer` (multi-layer safe patterns), `typecast_provenance.zer`, `typecast_volatile_strip.zer`, `typecast_const_strip.zer`, `typecast_direct_ptr.zer`, `typecast_int_to_ptr.zer`, `typecast_ptr_to_int.zer`.

### checker_post_passes Not Called (BUG-453, 2026-04-07)

**Problem:** `zerc_main.c` called `checker_check_bodies` (Pass 2 only), not `checker_check` (all passes). Pass 3 (whole-program provenance), Pass 4 (interrupt safety), Pass 5 (stack depth) never ran in the real compiler — only in unit tests.

**Fix:** Added `checker_post_passes()` function that runs Pass 3+4+5, called from `zerc_main.c` after body checking. Same class of integration bug as zercheck (2026-04-03).

### scan_frame Missing Expression Nodes (BUG-452, 2026-04-07)

**Problem:** `return fibonacci(n-1) + fibonacci(n-2)` — function calls inside NODE_BINARY invisible to stack depth analysis. No recursion warning emitted.

**Fix:** Added NODE_BINARY (recurse left+right), NODE_UNARY (recurse operand), NODE_ORELSE (recurse expr) to `scan_frame`. Now all function calls in any expression position are found.

### Arena Global Escape (BUG-455, 2026-04-07)

**Problem:** Global arena pointer stored in global variable not caught. `Arena scratch;` (global) → `*Cfg c = scratch.alloc(Cfg) orelse return; global_cfg = c;` compiles without error. After `scratch.reset()`, `global_cfg` is dangling.

**Root cause:** `is_arena_derived` flag was only set for LOCAL arena allocs (`!arena_is_global` guard). Global arena allocs were considered "safe" — wrong, because `arena.reset()` invalidates all pointers regardless of arena scope.

**Fix:** Added `is_from_arena` flag on Symbol (types.h). Set for ALL arena allocs (global or local). Assignment-to-global check uses `is_from_arena || is_arena_derived`. Return/keep/call checks still use only `is_arena_derived` (local arenas — global arena pointers CAN be returned from functions safely because global arena outlives function scope).

**Design distinction:**
- `is_arena_derived` = pointer from LOCAL arena → cannot return, cannot pass to keep params, cannot store in global
- `is_from_arena` = pointer from ANY arena → cannot store in global (but CAN return, CAN pass to functions)

Both propagate through aliases, if-unwrap captures, switch captures, orelse unwrap, struct copy.

### Allocation Coloring (zercheck.c, 2026-04-07)

**Problem:** Wrapper functions returning `?*T` from arena triggered false "handle never freed" errors. Chained wrappers (app → driver → hal → arena.alloc) made it worse — each layer hid the arena source.

**Solution — three-layer coloring system:**

1. **`source_color` on HandleInfo** (`ZC_COLOR_UNKNOWN/POOL/ARENA/MALLOC`): set at allocation, propagated through all alias paths (alloc_id copies, struct copies, if-unwrap, interior pointers). Leak check skips `ZC_COLOR_ARENA`.

2. **`func_returns_color_by_name()`**: recursive body scan. Walks return statements — direct `arena.alloc()` → ARENA, `pool.alloc()` → POOL, call to function with known color → inherit transitively. Depth-limited to 8. Cached on `Symbol.returns_color_cached/value`. Handles chained wrappers: `app()` returns `driver()` returns `hal()` returns `arena.alloc()` → all ARENA.

3. **Param color inference** (`Symbol.returns_param_color`): when a function returns a cast of its parameter (e.g., `*Block opaque_to_block(*opaque raw) { return (*Block)raw; }`), the summary records "return inherits param[0]'s color." At call site, the arg's `source_color` is copied to the result. This makes freelist type-punning work: `opaque_to_block(raw)` where `raw` is ARENA-colored → result is ARENA-colored.

**Alias walker updated:** `NODE_TYPECAST` added to the alias source walker (alongside `NODE_INTRINSIC` and `NODE_ORELSE`). `*opaque raw = (*opaque)b` now copies `b`'s alloc_id AND source_color to `raw`.

**Coverage:**
- Direct arena wrapper: 100%
- Chained arena wrapper (any depth up to 8): 100%
- Freelist type-punning through `*opaque` adapter functions: 100%
- Pool/malloc leaks: still caught
- Mixed-source functions (one path arena, one path pool): conservative → tracked

### Exhaustive Switch on NodeKind (RF14, 2026-04-07)

**Problem:** The most common bug class across 12 audit sessions was "AST walker doesn't handle NODE_X" (BUG-433, 434, 435, 437, 452). Each is 2 lines to fix but 10-30 minutes to find. Root cause: `default: break;` in switch statements silently skips new node types.

**Fix:** Converted 5 critical walker functions from `default: break;` to exhaustive case lists covering all 35 NodeKind values. No `default:` means GCC `-Wswitch` (enabled by `-Wall`) warns immediately when a new NODE_ type is added and any walker doesn't handle it.

**Functions converted:**
1. `scan_frame` (checker.c) — stack depth / recursion detection. Also gained recursion into NODE_SWITCH, NODE_DEFER, NODE_CRITICAL, NODE_INTRINSIC, NODE_FIELD, NODE_INDEX, NODE_TYPECAST, NODE_SLICE (all were previously missed via `default: break;`).
2. `collect_labels` (checker.c) — converted from if/else chain to switch.
3. `validate_gotos` (checker.c) — converted from if/else chain to switch.
4. `zc_check_expr` (zercheck.c) — gained NODE_TYPECAST, NODE_SLICE recursion.
5. `zc_check_stmt` (zercheck.c) — all statement nodes explicit.

**Pattern for new walkers:** Always use `switch (node->kind)` with NO `default:`. List every NodeKind explicitly — active cases with logic, inactive cases grouped with `break;`. GCC enforces completeness.

**Also converted (emitter.c):**
6. `emit_auto_guards` — bounds check guard emission
7. `emit_top_level_decl` — top-level declaration emission

**Remaining with intentional `default:`:** `emit_expr` (emits `/* unhandled expr */` diagnostic — only used for top-level constant expressions now), `emit_rewritten_node` (emits `/* unhandled node %d */0` — IR passthrough path), `resolve_type_for_emit` (returns `ty_void` fallback). These are safe — unknown nodes produce visible output in emitted C, not silent skips.

**Total: 7 exhaustive walkers.** GCC `-Wswitch` warns on ALL of them when a new NODE_ type is added.

**Impact:** Prevents ~10-12 bugs per version. The single highest-value refactor for long-term maintainability.

### Semantic Fuzzer (tests/test_semantic_fuzz.c, 2026-04-07)

**Purpose:** Generates random valid ZER programs combining safety-critical patterns, compiles with zerc, runs, and verifies: safe programs must compile AND run (exit 0), unsafe programs must be REJECTED by zerc.

**32 generators (safe + unsafe):**
- Memory: Pool+defer (1-4 handles), arena wrappers (1-4 depth), *opaque roundtrip, interior ptr, Task.alloc/delete, handle alias, Slab alloc_ptr
- Casts: narrowing chain, bool↔int, signed↔unsigned, wrong-type, wrong-provenance, direct *A→*B
- Control: goto+defer, while+break, enum switch, defer+orelse block
- Types: union mutable capture, packed struct, distinct typedef, function pointer callback, nested struct deref, bit extraction, slice subslice, ring buffer
- Unsafe: UAF (handle, interior, alias, goto backward), double free, leak, non-keep param escape, arena global escape

**Architecture:** C program with generator functions (`gen_safe_*`, `gen_unsafe_*`). RNG selects from 32 patterns, generates ZER source, writes to `/tmp/_zer_fuzz.zer`, runs `zerc --run` or `zerc -o /dev/null`. 200 tests per `make check`, deterministic via seed. Auto-detects zerc binary or builds from source.

**Found bugs:** Param color aliasing for `*opaque` adapter functions — `unwrap(*opaque raw)` returning `(*T)raw` was treated as new allocation instead of alias. Fixed: param color inference copies alloc_id from arg to result.

**When adding new features:** Add `gen_safe_<feature>()` + `gen_unsafe_<feature>()` to the fuzzer. Add case to the switch in main. The RNG automatically combines new patterns with existing ones — combinatorial coverage grows with each feature.

**Run:** Part of `make check`. Also: `./test_semantic_fuzz [seed] [count]` for custom runs. Default: seed=42, count=200. Verified with 2,500 tests across 5 seeds — zero failures.

### Param Color Aliasing (zercheck.c, 2026-04-07)

**Problem:** `*Src back = unwrap(raw)` where `unwrap(*opaque r) { return (*Src)r; }` — zercheck treated `back` as a new allocation needing free, even though it's the SAME memory as the arg (just re-cast).

**Fix:** When param color inference detects "function returns cast of param[N]", at the call site: instead of just copying color, create a full ALIAS — copy alloc_id, state, pool_id, free_line, source_color from the arg's HandleInfo. The result shares the same allocation identity as the arg.

**This means:** `defer slab.free_ptr(s); ... *Src back = unwrap((*opaque)s);` — `back` has same alloc_id as `s`, so when `s` is freed by defer, `back` is covered. No false "never freed" error.

**Alias walker updated:** NODE_TYPECAST now followed in the alias source walker (same loop as NODE_INTRINSIC and NODE_ORELSE). `*opaque raw = (*opaque)b` copies alloc_id + source_color from `b` to `raw`.

### shared struct — Auto-Locked Thread-Safe Data (2026-04-07)

**Keyword:** `shared struct Name { fields... }` — adds hidden `uint32_t _zer_lock` field. Every field access auto-locked via spinlock.

**Lexer:** `TOK_SHARED` keyword. **Parser:** `shared struct` prefix (same pattern as `packed struct`). **AST:** `is_shared` on struct_decl. **Types:** `is_shared` on TYPE_STRUCT.

**Emitter — block-level grouping:** NODE_BLOCK emitter scans consecutive statements with `find_shared_root_in_stmt()`. When consecutive statements access the same shared variable, ONE lock scope wraps the group. When a non-matching statement is encountered, the lock is released. Covers NODE_EXPR_STMT, NODE_VAR_DECL, NODE_RETURN, NODE_IF, NODE_WHILE, NODE_FOR, NODE_SWITCH.

**Emitter — lock primitives:** `_zer_lock_acquire` (atomic exchange spinlock), `_zer_lock_release` (atomic store). Emitted in preamble.

**Checker — &shared.field ban:** In NODE_UNARY TOK_AMP handler, if operand is NODE_FIELD on shared struct → compile error "pointer would bypass auto-locking."

**Design doc:** `docs/SHARED_STRUCT_DESIGN.md` — full specification including interactions with defer, goto, Handle, orelse, const, arrays, function parameters.

### spawn — Thread Creation (2026-04-07)

**Contextual keyword:** `spawn` is NOT a reserved keyword (not TOK_SPAWN in lexer). Detected by ident match at statement position in parser. This allows `spawn` as a method name (e.g., `ecs_world.spawn()`).

**Parser:** `spawn func(args);` → NODE_SPAWN with func_name + args array (arena-allocated).

**Checker validation:**
- Function must exist and be is_function
- Pointer args: only `*shared_struct` allowed (auto-locked, safe)
- Non-shared pointer args → compile error "data race"
- Handle args → compile error "pool.get() not thread-safe"
- Value args (u32, bool, struct by value) → OK, copied
- Banned inside @critical → compile error

**Emitter:** Generates pthread_create call with arg struct:
```c
{ struct _zer_spawn_args_N { ... }; ... pthread_create(...); pthread_detach(...); }
```
`#include <pthread.h>` added to preamble (guarded by `__STDC_HOSTED__`).

**NODE_SPAWN in exhaustive switches:** Added to all 7 walkers + scan_frame. CRITICAL: NODE_SPAWN must NOT fall through from leaf nodes (NODE_SIZEOF, NODE_CONTINUE, etc.) — caused segfault crash (accessing spawn_stmt union member on wrong node type).

### HS_TRANSFERRED — Ownership Transfer (zercheck.c, 2026-04-07)

**New HandleState:** `HS_TRANSFERRED` — set when a non-shared pointer is passed to spawn (if the checker somehow allows it via future changes).

**Check sites:** NODE_FIELD, NODE_INDEX, NODE_UNARY/deref, NODE_INTRINSIC/ptrcast — all check `h->state == HS_TRANSFERRED` alongside HS_FREED/HS_MAYBE_FREED. Error: "use after transfer: ownership transferred to thread at line N."

**Note:** Currently, the CHECKER prevents non-shared pointers from reaching spawn, so HS_TRANSFERRED is rarely triggered. It exists as defense-in-depth for future features that might relax the checker constraint.

### Deadlock Detection — Lock Ordering (checker.c, 2026-04-07)

**Pass 7:** `check_lock_ordering()` runs after stack depth analysis. Walks each function body's blocks.

**Algorithm (BUG-464 model, NOT the old ascending-type_id one):** the emitter locks per-statement (lock→op→unlock around each shared access group), releasing between statements, so cross-statement ordering is deadlock-free by construction. The ONLY real deadlock is a SINGLE statement accessing TWO DIFFERENT shared types — the emitter locks one and leaves the other unprotected, and a thread locking them in the opposite order deadlocks. So the check rejects exactly that: `collect_shared_types_in_stmt(stmt)` returns ≥2 distinct shared `type_id`s → compile error "deadlock: single statement accesses both 'A' and 'B' — split into separate statements." (`shared(rw)` read-only statements are exempt — concurrent readers don't deadlock.)

**Helpers:** `check_block_lock_ordering()` iterates a block's statements, runs `collect_shared_types_in_stmt()` on each, and recurses into EVERY body-bearing statement kind (block/if/for/while/do-while/switch-arms/@critical/@once/defer) via a no-default exhaustive switch — a multi-shared statement nested anywhere is caught (2026-06-27: closed switch-arm + do-while holes here; the prior if-chain missed those bodies). `collect_shared_types_in_stmt`/`collect_shared_types_in_expr` (the all-shared-types collector, also exhaustive switches under `-Werror=switch`) and `scan_body_shared_types` (the transitive per-function cache) descend casts/intrinsics/index/slice/orelse/struct-init **and spawn ARGUMENTS** so a shared read hidden in a subexpression can't evade the check.

**SPAWN IS THE ONE EXCEPTION TO "ONE LOCK PER STATEMENT" — and it is per ARGUMENT (verified from emitted C, 2026-09-10).** The emitter marshals each spawn argument under its own lock scope, and labels them:

```c
{ /* spawn w */
    /* shared-read lock for spawn arg 0 */
    pthread_mutex_lock(&a._zer_mtx);   _sa->a0 = a.x;   pthread_mutex_unlock(&a._zer_mtx);
    /* shared-read lock for spawn arg 1 */
    pthread_mutex_lock(&b._zer_mtx);   _sa->a1 = b.y;   pthread_mutex_unlock(&b._zer_mtx);
    pthread_create(&th, NULL, _zer_spawn_wrap_0, (void*)_sa);
}
```

So `spawn w(a.x, b.y)` is TWO correctly-held locks and must stay legal, while `spawn w(a.x + b.y)` is the hazard — one lock, two structs read. **A per-STATEMENT check is therefore wrong for spawn in both directions**: it rejects the safe form and, if you simply omit spawn, misses the unsafe one. `collect_shared_in_spawn_args` (BUG-973) runs the collector per argument and reports the first argument that alone touches two.

**`NODE_SPAWN` was in the collector's "returns 0" list until BUG-973** — an EXPLICIT classification, because the no-default discipline requires every kind to be named, and simply the wrong one: a spawn's arguments are parent-evaluated expressions of that statement. `spawn w(a.x + b.y)` emitted `_sa->a0 = (a.x + b.y);` inside A's lock only. Worth remembering when reading these exhaustive switches: **they prove every kind was CONSIDERED, not that each was considered correctly.** Both spellings need the arm — the bare statement reaches `collect_shared_types_in_stmt`, the scoped `ThreadHandle th = spawn …` arrives as a var-decl initializer and reaches `collect_shared_types_in_expr`. (The legacy `find_shared_type_in_expr/_in_stmt` "first shared type" helpers were dead since BUG-464 and removed 2026-06-27.)

**Two corrections layered on this since (2026-09-06/11) — see "Concurrency rules that changed in the run":** the transitive callee merge runs only for a statement that has a DIRECT shared access (BUG-948 — a statement holding no lock cannot deadlock, and the merge was rejecting `u32 r = touch_both();`), and a `shared(rw)` root RE-ENTERED through a callee is refused (BUG-980 — a `pthread_rwlock` is not recursive; the same-type case was excluded by the `id0 != id1` guard; write re-entry gave silent corruption, read-then-write hung). `lockchk_direct_only` / `lockchk_callee_only` are the two collection modes of the ONE collector.

**Declaration order = lock order:** `type_id` is assigned during `register_decl` in declaration order. Earlier declared struct = lower ID = must be locked first.

**Mathematically proven:** If all code acquires locks in the same total order, deadlock is impossible (Dijkstra, 1965). Same principle as Linux kernel lockdep.

**Post-passes error check:** `zerc_main.c` now checks `checker.error_count` after `checker_post_passes()`. Deadlock errors prevent compilation.

### Scoped Spawn — ThreadHandle + join (2026-04-08)

**Syntax:** `ThreadHandle th = spawn worker(&data);` + `th.join();`

**Key rule:** Scoped spawn (with ThreadHandle capture) allows `*T` (non-shared pointer) args because the thread is guaranteed to be joined before the scope exits. Fire-and-forget spawn still requires `*shared_struct` or value args.

**Parser:** `ThreadHandle` is a contextual ident (12 chars), detected at statement position. Parsed as NODE_SPAWN with `handle_name`/`handle_name_len` fields. `yield` and `await` are also contextual idents (5 chars).

**Checker:** Registers ThreadHandle variable in scope as `u64` with `sym->is_thread_handle = true`. `th.join()` intercepted in builtin method dispatch (checks `is_thread_handle` on symbol). Scoped spawn skips the non-shared pointer error.

**Emitter — proper spawn wrappers:** Pre-scan phase (`prescan_spawn_in_node`) walks entire AST, assigns unique IDs to NODE_SPAWN nodes. Wrapper functions (`_zer_spawn_wrap_N`) emitted at file scope between struct declarations and user functions. Forward declarations emitted for target functions. At NODE_SPAWN emission site, references the wrapper by ID. Scoped spawn emits `pthread_t handle_name;` + `pthread_create` (no detach). Fire-and-forget emits local `pthread_t` + detach.

**zercheck:** ThreadHandle registered as ALIVE with `is_thread_handle = true`. `th.join()` detected in NODE_CALL handler — marks FREED. At function exit, ALIVE ThreadHandle → error "thread not joined: 'th' spawned but never joined — add 'th.join()' before function returns."

**The spawn..join WINDOW is concurrent (BUG-979, 2026-09-11).** The atomic-cell rule used to latch `after_spawn_in_func` for a fire-and-forget spawn and set NOTHING for a scoped one, so `ThreadHandle t = spawn w(); g_ctr = 5; t.join();` raced `w`'s `@atomic_add(&g_ctr, …)` and compiled clean while the fire-and-forget spelling was rejected. It is a REFCOUNT now (`Checker.live_scoped_threads`, `Symbol.th_live`, `unbounded_spawn_in_func`); the window closes at the LAST join and never when a fire-and-forget spawn also ran. What a scoped spawn LENDS is a separate fact (`Symbol.borrow_root_name`, BUG-969; `borrow_root_ambiguous`, BUG-1014).

**Previous spawn UB fixed:** Old emitter cast function pointer `(void*(*)(void*))func_name` — UB for multi-arg functions. New approach: proper wrapper function unpacks arg struct and calls the real function with typed args.

### Condvar — @cond_wait / @cond_signal / @cond_broadcast / @cond_timedwait (2026-04-08)

**Intrinsics:** `@cond_wait(shared_var, condition)`, `@cond_signal(shared_var)`, `@cond_broadcast(shared_var)`, `@cond_timedwait(shared_var, condition, timeout_ms)`.

**Smart lock upgrade:** Pre-scan phase detects which shared struct types are referenced by @cond_* intrinsics. Those types get `pthread_mutex_t _zer_mtx` + `pthread_cond_t _zer_cond` instead of `uint32_t _zer_lock`. Lock/unlock functions (`emit_shared_lock_mode`, `emit_shared_unlock`) check `shared_needs_condvar()` to emit the correct lock type.

**@cond_wait emission:** `({ pthread_mutex_lock(&var._zer_mtx); while (!(condition)) { pthread_cond_wait(&var._zer_cond, &var._zer_mtx); } pthread_mutex_unlock(&var._zer_mtx); (void)0; })`

**@cond_timedwait emission:** Same pattern but uses `clock_gettime` + `pthread_cond_timedwait`. Returns `_zer_opt_void` — `.has_value = 1` if condition met, `.has_value = 0` on timeout. Requires `#include <time.h>` in preamble.

**Checker:** Validates first arg is shared struct variable. @cond_wait requires 2 args, @cond_timedwait requires 3 args (last is integer timeout_ms). @cond_timedwait returns `?void`.

### shared(rw) struct — Reader-Writer Locks (2026-04-08)

**Syntax:** `shared(rw) struct Config { u32 threshold; }` — uses `pthread_rwlock_t` instead of spinlock.

**Parser:** After consuming `TOK_SHARED`, checks for `TOK_LPAREN` + ident "rw" + `TOK_RPAREN`. Sets `is_shared_rw = true` on struct_decl.

**AST/Types:** `bool is_shared_rw` added to both `struct_decl` (AST) and `struct_type` (Type).

**Emitter — auto read/write detection:** Block-level grouping pre-scans the statement group to determine if ANY statement writes. `stmt_writes_shared()` checks for NODE_ASSIGN targets and NODE_CALL (conservative: any call might mutate). If group has writes → `pthread_rwlock_wrlock`. All reads → `pthread_rwlock_rdlock`. Unlock is always `pthread_rwlock_unlock`.

**Preamble:** `#define _POSIX_C_SOURCE 200112L` added before includes for `pthread_rwlock_t` support.

### @once { body } — Thread-Safe Init (2026-04-08)

**Syntax:** `@once { body }` — executes body exactly once, thread-safe.

**Parser:** Detected alongside `@critical` in the `@` + ident pattern. Creates NODE_ONCE with `once.body`.

**Emitter:** `static uint32_t _zer_once_N = 0; if (!__atomic_exchange_n(&_zer_once_N, 1, __ATOMIC_ACQ_REL)) { body }` — atomic CAS ensures only one thread enters.

**NODE_ONCE added to ALL exhaustive walkers:** collect_labels, validate_gotos, contains_break, all_paths_return, scan_frame, find_return_range, emit_auto_guards, prescan_spawn_in_node, emit_top_level_decl, zc_check_expr, zc_check_stmt, block_always_exits, defer free scan.

### @barrier_init / @barrier_wait — Thread Barrier (2026-04-08)

**Intrinsics:** `@barrier_init(var, N)` + `@barrier_wait(var)` — N threads wait until all arrive.

**Implementation:** Portable (mutex + condvar, same as Rust's `std::sync::Barrier`). `_zer_barrier` struct emitted in preamble with `pthread_mutex_t`, `pthread_cond_t`, `count`, `target`, `generation` fields. `_zer_barrier_wait` increments count, broadcasts when target reached, otherwise waits on condvar with generation check for spurious wakeup.

### async/await — Stackless Coroutines (2026-04-08)

**Syntax:** `async void func() { yield; await cond; }` — zero-cost cooperative multitasking.

**Keyword:** `TOK_ASYNC` in lexer. `yield` and `await` are contextual idents (not reserved keywords).

**AST:** `func_decl.is_async` flag. NODE_YIELD (leaf, no operands). NODE_AWAIT with `await_stmt.cond` expression.

**Checker — auto-registration:** When an async function is registered, checker also registers:
- `_zer_async_NAME` — struct type (state machine)
- `_zer_async_NAME_init` — function taking `*_zer_async_NAME`, returns void
- `_zer_async_NAME_poll` — function taking `*_zer_async_NAME`, returns i32
Uses `add_symbol_internal()` to bypass BUG-276 `_zer_` prefix check.

**Emitter — Duff's device transformation:**
1. `collect_async_locals()` — scans body for NODE_VAR_DECL, collects names + lengths
2. Emits `typedef struct { int _zer_state; type field1; type field2; ... } _zer_async_NAME;`
3. Emits `_init` function (memset 0)
4. Emits `_poll` function with `switch (self->_zer_state) { case 0:; body }`
5. NODE_YIELD emits: `self->_zer_state = N; return 0; case N:;`
6. NODE_AWAIT emits: `case N:; if (!(cond)) { self->_zer_state = N; return 0; }`
7. NODE_VAR_DECL in async mode: emits `self->name = init;` (no type declaration)
8. NODE_IDENT in async mode: `is_async_local()` check → emits `self->name`

**Duff's device:** The switch/case labels can jump INTO while/for loops — this is valid C since 1983. Enables yield/await inside loops without special loop handling. The compiler's normal while/for emission works unchanged.

**TYPE_STRUCT for async types:** Emitter checks `_zer_async_` prefix in TYPE_STRUCT emission — skips `struct` keyword (uses typedef name directly).

**Zero overhead:** No heap allocation, no runtime scheduler. Each task is a stack-allocated struct (~4-50 bytes). Poll function is a flat switch — branch predictor handles it efficiently.

### Ring Channel Pointer Warning (2026-04-08)

**Checker:** In Ring.push() and Ring.push_checked() method handling, checks whether the Ring's element type CARRIES a pointer (`type_carries_data_pointer` — a struct with a `?*T` field crossed silently when this tested only the element's bare kind, BUG-986). If so, emits warning: "pushing pointer through Ring channel — pointer may not be valid in receiver context."

### threadlocal Keyword (2026-04-07)

**Keyword:** `TOK_THREADLOCAL` in lexer, recognized for 't' case, 11 chars.

**AST:** `bool is_threadlocal` on var_decl.

**Parser:** `threadlocal` prefix handled before `const` in `parse_declaration`. Sets `is_threadlocal = true`.

**Emitter:** `emit_global_var` emits `__thread` prefix for threadlocal globals.

### Const Global Pre-Evaluation (BUG-461, 2026-04-08)

**Problem:** `const u32 X = 1 << 2;` at global scope fails because `_zer_shl` safety macro uses GCC statement expression `({...})` — invalid in global initializer context.

**Fix:** In `emit_global_var`, when `node->var_decl.is_const`, try `eval_const_expr(node->var_decl.init)` first. If evaluation succeeds (returns != CONST_EVAL_FAIL), emit the pre-computed numeric result directly: `uint32_t X = 4;`. Falls back to `emit_expr()` for non-evaluable expressions.

**Safety:** Compile-time evaluation is SAFER than runtime macro — shift amount verified at compile time, not runtime. All const global initializers with arithmetic (including <<, >>) are now pre-evaluated.

### Rust Test Suite Coverage (2026-04-08)

**400 Rust-equivalent tests** in `rust_tests/`, all passing, integrated into `make check` via `rust_tests/run_tests.sh`.

**Directories fully covered:**
- `tests/ui/threads-sendsync/` — COMPLETE (51/67 translated, 16 not-applicable)

**Directories partially covered:**
- `tests/ui/borrowck/` — 8 patterns (cross-func UAF, interior ptr, alias, autoref)
- `tests/ui/moves/` — 8 patterns (move chain, guard, loop, arc reuse)
- `tests/ui/drop/` — 8 patterns (scope exit, LIFO order, conditional, count, trait object)
- `tests/ui/unsafe/` — 4 patterns (@inttoptr safety, pointer deref, assignability)
- `tests/ui/consts/` — 10 patterns (const fn, binops, array OOB, enum values, comptime if)

**Test detection:** Negative tests auto-detected by "EXPECTED: compile error" in file content. run_tests.sh handles both positive (compile+run+exit 0) and negative (must fail to compile).

**Critical zercheck edge cases verified (15 tests):**
- Cross-function free chain (A→B→C frees)
- MAYBE_FREED in loop, nested if, switch arm
- Interior pointer to nested field chain
- Handle alias tracking (free one var, use other = error)
- *opaque provenance through free call
- defer + return expression evaluation order
- goto fires pending defers
- if-exit-not-MAYBE_FREED (free+return in branch = safe after)
- Double free via alias, ghost handle, handle leak overwrite
- Cross-type slab free, scope escape via struct field return

## BUG-577: Universal orelse pre-lowering (v0.4.6, 2026-04-17)

Triggered by a real ZER program (linked_list.zer) that hung on `current = current.next orelse break;`. UBSan pinpointed null deref. Three fix rounds landed the universal solution.

### The architectural problem

`emit_rewritten_node` (IR path emitter) has NO `NODE_ORELSE` case by design — orelse is supposed to be pre-lowered at the statement level by `lower_orelse_to_dest` (emits IR branches + IR_COPY for unwrap).

The gap: `find_orelse` only detected orelse at the TOP of the expression. Orelse wrapped in NODE_ASSIGN value, buried in call args, binary operands, or index sub-expressions escaped detection. Raw AST reached `emit_rewritten_node` which hit the default `/* unhandled node 47 */0`.

### Three-round fix

**Round 1:** `find_orelse` recurses into `NODE_ASSIGN.assign.value` (TOK_EQ only). Catches `ident = X orelse break;`. Pre-lowers via `lower_orelse_to_dest(target_local, orelse)`.

**Round 2:** for non-local targets (`field/index/deref = X orelse break`), `lower_orelse_to_dest` can't write to them (only IR local IDs). Fix: lower orelse to tmp, synthesize `target = tmp_ident` as a new NODE_ASSIGN, emit via IR_ASSIGN passthrough — emitter's standard NODE_ASSIGN handling does the field/index/deref write.

**Round 3 — `pre_lower_orelse` walker (universal fix):**

```c
/* Takes Node** so it can REPLACE the slot in the parent. */
static void pre_lower_orelse(LowerCtx *ctx, Node **pp, int line);
```

Tree walker that recursively visits expression children. When it finds NODE_ORELSE:
1. Determines result type (from checker or orelse.expr's optional inner).
2. Creates fresh tmp local of that type.
3. Calls `lower_orelse_to_dest(ctx, tmp, orelse, line)` — emits IR branches, IR_GOTO for terminating fallbacks, IR_COPY for value path.
4. Replaces `*pp` with new `NODE_IDENT` referencing the tmp.

After the walk, the expression tree is guaranteed orelse-free. Passthrough emits safely.

Called in `lower_expr`'s `passthrough:` block:
```c
passthrough: {
    rewrite_idents(ctx, expr);
    pre_lower_orelse(ctx, &expr, expr->loc.line);  // NEW
    // ... emit IR_ASSIGN with expr ...
}
```

Recurses into: NODE_BINARY, NODE_UNARY, NODE_CALL (callee + args), NODE_FIELD, NODE_INDEX, NODE_SLICE, NODE_TYPECAST, NODE_ASSIGN, NODE_INTRINSIC, NODE_STRUCT_INIT.

### Compound assign decomposition

Added `lower_expr(NODE_ASSIGN)` for compound ops (`+=`, `-=`, etc.):

```c
case NODE_ASSIGN: {
    if (expr->assign.op == TOK_EQ) goto passthrough;
    int val_local = lower_expr(ctx, expr->assign.value);  // decomposes nested orelse
    // synthesize `target op= tmp_ident` and emit as IR_ASSIGN
}
```

Produces cleaner IR: call runs first, stores result, then `t += tmp` emits simply.

### `emit_rewritten_node` vs `emit_expr` — not the same

Fresh sessions often confuse these. The distinction is the INPUT:

- `emit_expr` sees raw source AST. All node kinds possible. Used only for top-level declarations now.
- `emit_rewritten_node` sees PRE-LOWERED AST — after `rewrite_idents` renamed shadowed locals, after `pre_lower_orelse` replaced orelse with tmp-ident, after compound assign's RHS was decomposed.

IR invariant "zero `emit_expr` calls in IR function body path" verified:
```bash
sed -n '6282,7500p' emitter.c | grep -c "emit_expr("   # 0
sed -n '7520,8800p' emitter.c | grep -c "emit_expr("   # 0
```

`tools/walker_audit.sh` automates this.

### Walker audit tool

`tools/walker_audit.sh` compares AST emit_expr cases vs IR emit_rewritten_node cases. NODE_ORELSE is documented as a known pre-lowered exception. Current output:
```
emit_expr (AST path)        handles 18 node kinds
emit_rewritten_node (IR)    handles 17 node kinds (excluding pre-lowered)
OK — no gaps.
```

Run before releasing. If the tool flags a gap, decide whether to add a case to `emit_rewritten_node` (Option A: for value-only constructs) or pre-lower (Option B: for constructs involving IR control flow).

### Test coverage

Three regression tests in `tests/zer/` guard BUG-577 permanently:

- **`orelse_assign_nonlocal.zer`** — non-local targets (field + index) with orelse break in loop.
- **`orelse_stress.zer`** — 14 orelse positions, each a separate function returning a distinct expected value. Any regression causes main() to exit with a distinct code pinpointing the broken context.
- **`defer_scoped_blocks.zer`** — complementary defer lifecycle coverage inside loop/if/switch arms.

### Pre-lowering pattern as the universal answer for IR path

When a new AST construct can contain an expression (new statement kinds, new expression-wrapping operators):

1. `rewrite_idents` must recurse into it (scope-shadow renaming).
2. `pre_lower_orelse` must recurse into it (orelse decomposition).
3. `emit_auto_guards` must recurse into it (bounds check injection — for NODE_INDEX primarily).
4. Verify via `tools/walker_audit.sh`.

If the new construct involves CFG (breaks, continues, gotos, yields), use Option B (pre-lower to IR). If it's a value-only expression, Option A (add case to emit_rewritten_node) is acceptable.

## BUG-579: Full IR lowering of enum/union/optional switches (v0.4.9, 2026-04-18)

Pre-v0.4.9 enum/union/optional switches used an `IR_NOP` passthrough:
`ir_lower.c` emitted `IR_NOP{expr=NODE_SWITCH}` for these three types,
and `emit_ir_inst` had a mini-`emit_stmt` that walked arm bodies AST-style.
That mini-walker handled ONLY 6 statement kinds (`NODE_EXPR_STMT`,
`NODE_RETURN`, `NODE_BREAK`, `NODE_VAR_DECL`, `NODE_DEFER`, `NODE_IF` with
partial body support). `NODE_FOR`, `NODE_WHILE`, nested `NODE_SWITCH`,
`NODE_CONTINUE`, `NODE_GOTO`, `NODE_CRITICAL`, `NODE_ONCE`, `NODE_SPAWN`,
`NODE_YIELD`, `NODE_AWAIT`, `NODE_ASM`, `NODE_STATIC_ASSERT` in arm bodies
silently emitted as `/* unhandled node N */0;`. `NODE_EXPR_STMT` with
orelse inside also dropped.

Latent for the lifetime of the IR path (no test exercised the gap — first
discovered in audit 2026-04-18).

**Fix**: enum/union/optional switches now follow the integer switch path:
build comparison AST per arm, `lower_expr` it, `IR_BRANCH`, then
`lower_stmt` for arm body. `lower_stmt` handles every statement kind —
free, automatic.

Per-type comparison builder:
- **Enum**: `NODE_BINARY(sw_ref, ==, NODE_INT_LIT(variant.value))`. Resolves
  variant name from `arm->values[vi]` which can be NODE_IDENT (`.west`) or
  NODE_FIELD (`Dir.west`). Look up in `sw_eff->enum_type.variants[i].value`.
- **Union**: `NODE_BINARY(NODE_FIELD(sw_ref, "_tag"), ==, NODE_INT_LIT(index))`
  where sw_ref is a pointer-to-original (`&sw_expr` for lvalue expressions,
  `&hoisted_tmp` for rvalue like NODE_CALL). NODE_FIELD emits as `->_tag`
  because the object type is pointer.
- **Optional**: non-null → `NODE_FIELD(sw_ref, "has_value") && (value == arm)`;
  null → `!has_value`. Null-sentinel optionals (`?*T`) use pointer truthy
  check directly. For `?Enum` arms, resolve variant to its numeric value.

Capture handling:
- Enum: `IR_ASSIGN dest=cap, expr=sw_ref` (IR_COPY with same-type adaptation).
- Optional `|v|`: IR_COPY with src→dst type adaptation (src=?T, dst=T) appends
  `.value`. `|*v|` = BUG-552 path (`&src.value`).
- Union `|v|`: IR_ASSIGN with `cap = NODE_FIELD(sw_ref, variant)` (emitter
  handles array-variants via array-to-array memcpy, see BUG-582 infra).
- Union `|*v|`: `NODE_UNARY(TOK_AMP, NODE_FIELD(sw_ref, variant))` — pointer
  to original, so mutations persist.

**Supporting infra** introduced for BUG-579 and used elsewhere:

1. **`checker_set_type(Checker*, Node*, Type*)`** (public API in `checker.h`).
   Exports `typemap_set` so IR lowering can annotate synthesized AST nodes.
   Without it, `lower_expr` on a freshly-built NODE_BINARY returns `ty_i32`
   fallback, creating wrong-typed locals (pointer-to-union declared as
   int32_t — valid C but crashes).

2. **`lower_expr(NODE_FIELD)` type inference**: when `checker_get_type(field)`
   is NULL, infer from the object's type + field name. Covers `has_value`
   → bool, `value` → optional inner, `_tag` → i32, and named
   struct/union/variant fields. Prevents null-typed temps that the emitter
   silently skips declaring → "_zer_tN undeclared" GCC errors.

3. **`IR_ASSIGN` array-to-array memcpy**: mirror of BUG-548 fix for
   `IR_COPY`. When `dst_eff = TYPE_ARRAY && src_eff = TYPE_ARRAY`, emit
   `memcpy(dst, src, sizeof(dst));` before the `dst = ` prefix. Needed for
   union array-variant captures.

4. **Scoped capture naming** (BUG-585): switch arm captures now get unique
   names (`v_capN` via a counter) and `rewrite_capture_name()` walks the
   arm body to replace references to the source name. Prevents IR's flat
   local namespace from collapsing same-name captures across multiple
   switches in one function via `ir_find_local` last-match.

**Dead code**: the emitter's `IR_NOP` NODE_SWITCH mini-emit_stmt (~500
lines at `emitter.c:8112+`) is no longer reached from `ir_lower.c`. Kept
for now — will remove in a follow-up once stability confirmed.

**Regression tests** (`tests/zer/`):
- `switch_arm_orelse_value.zer` — value fallback orelse in arm
- `switch_arm_for_loop.zer` — for-loop in arm body
- `switch_arm_orelse_break.zer` — terminator fallback in loop+switch
- `switch_arm_nested_switch.zer` — switch inside switch arm
- `switch_arm_while_continue.zer` — while+continue in arm body

## BUG-581 through BUG-589: `--run` exit code + cascaded bugs (2026-04-18)

### BUG-581: `zerc --run` exit code propagation

`zerc_main.c` before this fix returned `system(run_cmd)` raw. On POSIX,
this is the wait status (high bits: signal info; low bits: exit). Shell
`$?` gets `status & 255`, so exit 3 becomes 0. Fix:
```c
#ifndef _WIN32
#include <sys/wait.h>
#endif
...
if (WIFEXITED(run_ret)) return WEXITSTATUS(run_ret);
if (WIFSIGNALED(run_ret)) return 128 + WTERMSIG(run_ret);
```
Windows MSVCRT `system()` already returns the exit code directly, so the
Windows branch is unchanged.

**Impact**: all test runners (`tests/test_zer.sh`,
`rust_tests/run_tests.sh`, `zig_tests/run_tests.sh`, `test_semantic_fuzz`)
trust `$ret -eq 0` as PASS. Before the fix, any compiled program that
returned a non-zero exit (whether "test failed" return or SIGTRAP) was
silently reported as PASS. Surfaced 36 previously-masked failures across
suites when the fix landed:
- 15 in `tests/zer/` — fixed 12, skipped 3 (see `docs/limitations.md`)
- 12 in `rust_tests/` — all skipped (pending investigation)
- 7 in `test_semantic_fuzz/` — fixed 7 (fuzzer generator bug, BUG-589)
- 2 in `zig_tests/` — skipped

Fresh sessions: if a test mysteriously fails now that "used to work", it
likely never actually worked — check git history for when it was added.
BUG-581 is the culprit for ~8 months of hidden test failures.

### BUG-582: Union variant tag update missing on IR path

The AST emitter (`emit_expr` NODE_ASSIGN at `emitter.c:1210`) had special
handling for `u.variant = val` that emitted a statement expression
`({ __typeof__(u) *_p = &(u); _p->_tag = N; _p->variant = val; })` — updates
the `_tag` AND the variant field. When the IR path became default, the
new `emit_rewritten_node` NODE_ASSIGN handler lacked this case.

Fix: port + EXTEND the handler to walk up the assign target through
NODE_INDEX / NODE_FIELD / NODE_UNARY(deref) chains. Not just `u.v = val`
but also `u.v[i] = val` and `u.v.inner = val` now correctly update `_tag`.
The walk: given the target, iterate — at each step check if it's a
NODE_FIELD whose object type is TYPE_UNION; if yes, use that as
`union_field`. Otherwise descend into `.field.object` / `.index_expr.object`
/ `.unary.operand`.

Manifested as: `switch (u)` takes the wrong arm because `_tag` was never
updated. Masked by BUG-581 for union tests (main's exit code was wrong,
but `--run` reported 0).

### BUG-583: `@once { body }` emitted `if (1) goto body`

`NODE_ONCE` lowering emitted an `IR_BRANCH` with `expr=NULL` and
`cond_local=-1`. The emitter fell through to `emit(e, "1")` as an
"unreachable" case — which silently made `@once` always execute its body
(every call, every thread, every time).

Fix: `NODE_ONCE` lowering sets `br.expr = node` (the NODE_ONCE itself) as
a marker. `IR_BRANCH` emitter detects this and emits the atomic CAS
pattern:
```c
{ static uint32_t _zer_once_N = 0;
  if (!__atomic_exchange_n(&_zer_once_N, 1, __ATOMIC_ACQ_REL)) goto body; else goto skip; }
```
Matches the AST path at `emitter.c:3806`.

### BUG-584: Optional switch only checked `has_value`

`switch (?u32 v) { 5 => ..., 42 => ... }` took the FIRST non-null arm
regardless of value. Pre-fix code only emitted `_tmp.has_value` for every
non-null arm. Fix: build `has_value && (value == arm_value)`. For `?Enum`
arms, resolve the arm value name to its variant.value numeric literal.

### BUG-585: Switch arm capture scoping collision

Introduced BY the BUG-579 fix. When multiple switches in one function both
use `|v|` captures, same-typed captures dedup via `ir_add_local` in IR's
flat namespace. Different-typed captures get suffixed names BUT
`rewrite_idents` uses `ir_find_local` which returns LAST match — so any
arm's body referring to `v` gets the LATEST capture's rewritten name,
even if the latest was from a DIFFERENT SWITCH.

Fix: generate unique names per arm (`v_capN` via `ctx->temp_count++`),
and call a new `rewrite_capture_name()` that walks the arm body AST and
replaces references to the source capture name (`v`) with the unique
name (`v_capN`). Respects nested switches that shadow the same name (the
walker detects inner switches whose own captures match and skips their
body).

### BUG-586: `(bool)integer` plain cast (no truthy conversion)

ZER's bool emits as `uint8_t`. A plain C cast `(uint8_t)5` gives `5`, not
`1`. C's native `_Bool` has special conversion (any non-zero → 1). Fix in
IR_CAST emitter: when `dst_eff = TYPE_BOOL` and src is integer/float/
pointer, emit `((uint8_t)!!(x))` instead of `((uint8_t)x)`.

### BUG-587: Funcptr array call with literal index

`ops[0](a, b)` and `ops[1](a, b)` both emitted `ops[0](a, b)` because the
IR_CALL NODE_INDEX-callee path handled only NODE_IDENT indices; anything
else fell through to `emit(e, "0")` ("non-ident index fallback"). Fix:
handle `NODE_INT_LIT` explicitly, fall back to `emit_rewritten_node` for
complex expressions.

### BUG-588: Entry block not `bb0` when function has labels

The most impactful bug in this session. `ir_lower_func` called:
```c
collect_labels(&ctx, func_decl->func_decl.body);  // labels get IDs 0,1,2...
start_block(&ctx);                                 // entry gets higher ID
```
Emitter iterates blocks by ID in order. First block in output is
`_zer_bb0:;` — a random label's code. C execution starts linearly from
there, reading uninitialized state. Any function with a label (even in
nested blocks) silently executed wrong. Manifested as SIGTRAP (UAF on
uninitialized handles) or wrong return values.

Fix: `start_block` FIRST (entry = bb0), THEN `collect_labels` (labels get
IDs ≥ 1). Three-line change. Applied to both `ir_lower_func` and
`ir_lower_interrupt`.

### BUG-589: Semantic fuzzer generator bug (test design)

`gen_safe_goto_defer` in `tests/test_semantic_fuzz.c` generated:
```zer
defer pool.free(h);
pool.get(h).v = 42;
goto done;
done: if (pool.get(h).v != 42) { return 1; }
```
ZER's `goto` fires pending defers (see `tests/zer/goto_defer.zer`). So
at `done:` the handle is freed and the read traps. Masked by BUG-581.

Fix: update the generator to manage lifetime explicitly — no defer, free
before each return.

### Test runner `KNOWN_FAIL` skip infrastructure

Each test runner now has a documented list of pre-existing failures:
- `tests/test_zer.sh`: `KNOWN_FAIL_POSITIVE`
- `rust_tests/run_tests.sh`: `KNOWN_FAIL`
- `zig_tests/run_tests.sh`: `KNOWN_FAIL`

Entries report as `SKIP: name (known pre-existing issue — docs/limitations.md)`.
`make check` treats SKIP as non-fatal so the build stays green while
honestly surfacing the remaining issues.

When fixing a `KNOWN_FAIL` entry, REMOVE it from the list. Don't leave
entries for tests that now pass — otherwise real regressions slip through.

## BUG-590 through BUG-593 (2026-04-18)

Four more bugs surfaced by BUG-581's exit-code fix + one pre-existing
comptime float gap, documented together because they share a root
cause: stale tests became visible only after exit codes were honest.

### BUG-590 group: variable shadowing + NODE_BLOCK defer semantics

IR's flat local namespace had no scope concept. Inner `Handle h`
shadowing outer `Handle h` dedup'd or got a suffixed name, then
`ir_find_local` LAST-MATCH returned the (freed) inner local for
outer references after the block closed → UAF.

**Fix** added to IR:
- `IRLocal.scope_depth` set at creation from `IRFunc.current_scope`
- `IRLocal.hidden` bit marked on locals when NODE_BLOCK exits
- `ir_find_local` prefers non-hidden matches; falls back to LAST
  hidden match (preserves emitter-pass lookups outside normal flow)
- `ir_add_local` creates a suffixed local when name+type exists at
  a different scope_depth
- `NODE_BLOCK` in ir_lower increments/decrements `current_scope`
  and fires scoped defers at block exit — mirror of loops' POP_ONLY
  bb_post pattern so early-exit paths (break/continue/return/
  orelse-return) with earlier block IDs still find defers on the
  emit-time stack
- `LowerCtx.block_defers_managed` — when an outer construct
  (loop/if/switch arm) already manages defers, it sets this flag
  before calling `lower_stmt(body)`, suppressing NODE_BLOCK's own
  fire to avoid double-firing

The cascade effect: once defer-at-block-exit worked, four more
rust_tests started passing as a side effect (rt_defer_order_lifo,
rt_drop_count_3, rt_drop_trait_basic, rt_conc_ring_full_drop).

### BUG-591: `await` condition not re-evaluated on resume

`IR_AWAIT` lowering emitted cond eval instructions into the current
block THEN `IR_AWAIT` (which places `case N:;`). On resume, the
switch-case entry landed BELOW the evaluation → stale cond_local.

**Fix**: IR_AWAIT carries the cond AST on `inst->expr`
(cond_local=-1 as sentinel). Emitter emits `case N:;` followed by
a fresh `emit_rewritten_node(cond)` on every poll.

### BUG-592: signed vs unsigned comparison

`signed_local < 0` evaluated `false` for negative values because
the `0` literal had type `uint32_t` (or `uint64_t`), and C's usual
arithmetic conversion promoted the signed operand to unsigned.

Two-pronged fix in the IR emitter:
1. `IR_LITERAL` emits `(DstType)N` cast instead of `N_ULL`, so the
   temp holds exactly the target's type (signed if target is signed).
2. `IR_BINOP` detects comparison with signed/unsigned mismatch and
   inserts a cast of the unsigned side to the signed side's type.

### BUG-593: comptime float eval short-circuit

`check_call` at `checker.c:4243` ran `eval_comptime_block`
unconditionally. Float args are stored in `ComptimeParam.value` as
int64 bit-patterns (memcpy of double). `eval_comptime_block`
evaluated the body as INTEGER arithmetic: `return x * x` did int
multiply on the raw bits of 5.0, producing garbage, not
`CONST_EVAL_FAIL`. Because the result was non-FAIL, the success
branch ran, setting `is_comptime_resolved=true` with
`comptime_value=<garbage>` — so the float eval path was never reached.

**Fix**: before calling `eval_comptime_block`, check the function's
return type. If `f32`/`f64`, skip integer eval entirely and fall
through to the float branch which uses `eval_comptime_float_expr`.

### The per-declaration `@once` semantic

Two tests in the BUG-590 session had wrong expectations — they
called multiple `@once` blocks and expected them to share state.
ZER's `@once` is per-declaration (matches Rust's `std::sync::Once`):
each `@once { body }` gets its OWN flag. To fire once across
multiple invocations, wrap `@once` in a helper function and call
the helper multiple times.

## The 2026-09-06..13 run — refactors L and M, the r3an9y survey, the five-branch survey (BUG-934..1014): what is durable

Eight days, ~80 numbered fixes, two architectural refactors, three surveys consumed. Per-bug
detail is in BUGS-FIXED.md and the ledger is in limitations.md; the subsections here are the
part a future session needs in its head — the mechanisms, the measured numbers, and the
things that cost a loop to find. Order: the r3an9y run and the slice header (the first
survey), then refactor M (bounds guards as IR branches), then refactor L (the defer body as
IR — read M first, L depends on it), then the fail-open cap map, the concurrency and
forging-door corrections, the emitter facts, and the smaller mechanisms. The harvest-protocol
additions from the five-branch survey are in "Harvesting an audit branch" further down.

### The `loving-davinci-r3an9y` run (2026-09-08..10, BUG-967..974) — what is durable

Nine holes from one survey, closed in three days. The per-bug detail is in BUGS-FIXED.md
and the ledger is in limitations.md; what follows is only the part a future session needs
in its head, plus the mechanisms it must not re-derive.

**Every one was the same shape: ONE semantic question answered at N sites, or answered for
one SPELLING of an operation and not its siblings.** That is the class CLAUDE.md's
multi-site table exists for, and the run is nine more rows of evidence for it.

| bug | the question | how it was under-answered |
|---|---|---|
| 967 | "does this write a view header?" | `.len`/`.ptr` writable — bounds forgeable in one line |
| 968 | "what allocation does this view point into?" | walk stopped at `pool.get(h)`; a SLICE never aliased at all |
| 969 | "what does this spawn argument LEND?" | only a literal `&v` established a borrow |
| 970 | "does this copy a unique resource?" | Arena/Barrier/Semaphore nowhere; Pool/Ring/Slab at ONE site |
| 971 | "is this shared state?" | static locals invisible to BOTH race scans |
| 972 | "can an access through this fault?" | packed rule saw `&p.field`, not `&p.w[0]` nor a slice view |
| 973 | "which mutex is held?" | spawn args excluded from the same-statement rule |
| 974 | "what is the zero of this type?" | `return 0;` for every type, three answers collapsed into one |

#### Four mechanisms worth reusing verbatim

**A BOOLEAN CANNOT ANSWER A "WHICH ONE" QUESTION (BUG-969).** `is_local_derived` says a
pointer points into SOME local — exactly right for the ESCAPE sinks, which ask whether a
value outlives the frame and do not care which local. The scoped-spawn borrow asks a
different question: the race is a parent write to the ROOT (`v = 3`), so the root must be
NAMED. The code had compensated by handling only the one spelling where the name is
visible in the expression. `Symbol.borrow_root_name` carries it, recorded at the
DECLARATION per the BUG-488/494 rule — three sites, each of which already computed the
same walk for `is_local_derived`: the var-decl `&…` path and the `h.p = &v` carrier
assignment (both via `record_borrow_root`), and the slice-of-a-local path (which sets the
field directly, since it already has the root symbol in hand). **When a rule handles exactly one syntactic form,
check whether the fact it needs is even representable in the state it is reading.**

**TEST THE VALUE, NOT THE DESTINATION TYPE (BUG-970).** The unique-resource rule lived at
one site because the same TYPE appears on both sides of a legal construction: `Arena a =
Arena.over(buf)` is FRESH, not a copy. Widening a target-type test would reject the
constructor. Asking about the VALUE (`value_is_existing_resource`: an ident/field/index or
either arm of an `orelse` NAMES something that outlives the binding; a call builds
something new) is what let one predicate serve six sinks through the shared reporter
`reject_unique_resource_copy` — var-decl init, global init, call arg, orelse fallback,
struct-init field, spawn arg — plus the RETURN sink, which is a separate inline check
because its answer differs (see below).

**THE SAME QUESTION CAN HAVE DIFFERENT ANSWERS BY LIFETIME (BUG-970).** The RETURN sink is
deliberately narrower than the other six: returning a LOCAL resource by value is a MOVE
(the idiomatic factory), returning a GLOBAL is a copy. Do not "unify" it.

**TWO SINKS WITH BYTE-IDENTICAL CODE ARE ONE FIX, NOT TWO (BUG-971).**
`scan_unsafe_global_access` and `record_isr_globals` had the same `NODE_VAR_DECL` arm
character for character — confirmed by diffing, not assumed. Both resolve names with
`scope_lookup(c->global_scope, …)`, which is why a `static` LOCAL (one object per function,
shared by every thread AND between ISR and main) was invisible to both. A scan-scoped table
rather than a threaded body root, because the scan has SEVEN body-entry points.

#### Two operations that look like one

**`&packed.field` and a SLICE over a packed array field are DIFFERENT operations (BUG-972).**
No `&` appears in `[*]u32 s = p.w;` at all, so no address-of predicate can reach it. They
need two predicates, and their PRECISION differs on purpose: the slice rule keys on
`type_alignment_bytes(elem)` (a `u8[]` view is safe and is what `packed` exists for), the
address-of rule is deliberately unconditional because relaxing it would be an accept-unsafe
change. That asymmetry is recorded as an OPEN over-rejection in limitations.md.

#### The zero of a valueless return (BUG-974)

`orelse return` is bare by design — "the return value comes from the function's return
type". `emit_return_null`'s fallback was `return 0;` for every non-optional type, which is
three answers in one: correct for integers, INVALID C for aggregates (GCC refused it, so
valid ZER did not build), and a NULL from a non-null type for `*T`/funcptr.

**A construct modelled as a FLAG re-states every rule the real node kind carries — and the
list is easy to leave incomplete.** `orelse.fallback_is_return` reaches no `NODE_RETURN`
handler, which is why the `defer` and `@critical` bans are repeated beside the flag. The
RETURN-TYPE check was the third such rule and was missing; a plain bare `return;` in a `*T`
function had been rejected all along. The same shape applies to `orelse break` / `orelse
continue`.

#### Gates added

`tools/sink_matrix.sh` grew SHAPES **p19–p24** — 100 → 154 declared cells, counted with
`grep -c '^cell ' tools/sink_matrix.sh` at `7bf3fb90~1` and at HEAD (the runtime line
`matrix: 154 ok` agrees). Do not quote a cell count from prose; run the gate. Each was **run against a
build of the commit before its fix** and confirmed to report HOLE; the boundary cells were
confirmed green on BOTH sides. `tests/test_hw_matrix.c` grew the STATIC-LOCAL grid
(SITE x SHAPE, so spawn-vs-ISR disagreement fails the build).

~~**`test_hw_matrix` AUTO-DETECTS `./zerc` from the CWD and IGNORES `argv[1]`.**~~ That was
true for every one of the ten grids until BUG-1010 (2026-09-13): passing an old binary
silently measured the NEW compiler and the grid appeared to pass on a broken build — which
is exactly what it did on the first attempt. Every `tests/test_*_matrix.c` now honours
**`ZER_MATRIX_ZERC=<path>`** (`ZER_MATRIX_ZERC=/tmp/headb/zerc ./test_hw_matrix`); that is
how a new cell is shown to FIRE against a pre-fix build.

### The slice HEADER is read-only (2026-09-08, BUG-967)

`[*]T` bounds safety is ENTIRELY the `{ptr, len}` header — every index emits
`_zer_bounds_check(i, s.len)` — so the header must not be writable. It was:

```zer
u32 main() { u8[4] a; [*]u8 s = a; s.len = 100; s[50] = 1; return 0; }
```

compiled clean, RAN clean, and wrote 46 bytes past a 4-byte array. The check dutifully
validated 50 against the forged 100.

**ONE query, `view_header_field(c, e, &container)`, at TWO sinks** — the NODE_ASSIGN
target and `case TOK_AMP` — because those are the only two ways to write it. Seventeen
spellings were measured live BEFORE implementing and all seventeen fall to that one query
with no case-per-door:

- both fields (`.ptr`, `.len`) and both assign forms (`=`, `+=` / `-=`) — the pair is one
  NODE_ASSIGN differing only in `assign.op`, so it needs one test, not two
- through a bare local, a global, a struct field, a nested struct, an array-of-struct
  element, a pointer auto-deref, a value unwrapped from an `orelse`, a sub-slice, and a
  write inside a defer body — all covered because the query asks the type of the object
  IMMEDIATELY under the field, so nesting is free
- `&s.len` / `&s.ptr`, bound to a local or passed as an argument

**KEY ON THE OBJECT'S TYPE, NEVER ON THE FIELD NAME.** A user struct may perfectly well
have a field called `ptr` or `len`, and this corpus has `JString.len`, `Packet.len` and
`Box.ptr`. A name-keyed rule rejects all of them. `type_dispatch_kind` so a wrapper cannot
walk a slice through the rule. (`distinct [*]u8` turns out not to be expressible — the
grammar requires `distinct typedef` of a named type — but the unwrap is the right habit.)

The ARRAY sibling `a.len = 100` is the same question and was emitting `_zer_t0 = 4U =
100ULL;` — invalid C that only GCC rejected. Loud drift rather than a silent OOB write, so
it is answered by the SAME query rather than a second rule that could drift.

**Why reject rather than track:** corpus cost measured at ONE file, and that file
(`range_for_len_snapshot`) was writing the header only as a convenient way to BUILD a
slice, not because its subject needed it. It is rewritten to build by slicing and to widen
the whole slice mid-loop — the legal route to changing a bound, same property — and the
emitted C was checked to confirm the loop still compares against `_zer_rlen`, read once,
so it still discriminates. Nothing else in the corpus writes a slice header at all.

### Bounds guards are IR BRANCHES — refactor M (2026-09-07, BUG-952..958)

The "Bounds Auto-Guard" sections further down this file describe the ORIGINAL design
(the checker marks an unproven index, the emitter splices `if (idx >= N) return <zero>;`
as C text before the statement). That design still exists, but it is now the FALLBACK.
Read this first; those sections are kept for the mechanism they still own.

**Three pieces, one descent.**

| piece | where | what |
|---|---|---|
| `checker_walk_guard_sites(c, node, fn, ud)` + `ZerGuardSite` | checker.c / checker.h | THE walker. Finds every auto-guarded `NODE_INDEX` (bounds, `array_size`) and every dyn-freed handle-array `NODE_FIELD` (UAF, `freed_idx`) in an expression, in EMISSION ORDER (an access's own guard before its subexpressions, object before index — callers depend on that order; emitted C changes if it moves). Was a private descent inside `emit_auto_guards`, in TWO copies |
| `lower_stmt_guards` -> `lower_one_guard_site` | ir_lower.c | consumer 1: emits the guard as a real IR branch. Called in the block loop **BEFORE `emit_shared_lock_if_needed`** — the guard must precede the statement's lock (BUG-952) and an early return is only legal while no lock is held |
| `emit_auto_guards` -> `emit_one_guard` | emitter.c | consumer 2: the C rendering, for any site consumer 1 declined. `checker_mark_guard_lowered(c, access)` makes `checker_auto_guard_size` return 0 for a node whose branch is in the IR, so the emitter does not guard it a second time |

**DECLINING IS ALWAYS SAFE — that is the whole design.** Anything `lower_one_guard_site`
does not mark keeps the emitter's guard, so every restriction below costs precision and
never soundness, and the migration could land incrementally (stage B 69%, stage C 96%,
stage D five C-level splices left across the corpus). What it declines today, and why:

- the UAF form (`freed_idx` set) — not lowered yet;
- a return type whose zero is not a plain literal (struct / slice / optional — the exit
  block would need `emit_zero_value`); it unwraps `IRFunc.return_type` ONE level first,
  because that field may hold the FUNCTION type rather than the return type (the field's
  own comment says so; it cost a trace to find);
- an index that is not a bare `NODE_IDENT` or `NODE_INT_LIT`. **No AST mutation**: the
  first version rewrote the access to read the guard's temp for single evaluation, and
  that broke the moment the AST was lowered TWICE, which in-process harness paths do
  (`zercheck_run` then `emit_file`; `test_firmware_patterns2` caught it). Same hazard as
  `pre_lower_orelse`, from the other direction;
- statement kinds not in `lower_stmt_guards`'s exhaustive switch. Migrated: EXPR_STMT,
  VAR_DECL init, RETURN, an IF condition, a SWITCH selector (each evaluated EXACTLY ONCE,
  so hoisting before the statement is equivalent), SPAWN arguments (evaluated once, in the
  parent). Deliberately NOT: **loop conditions and `await`**, which are RE-EVALUATED per
  iteration / per poll — a guard hoisted before the statement would be checked once while
  the index changed underneath it. Those keep the emitter's guard, which lands INSIDE the
  loop's condition block (before the `IR_INDEX_READ`, and again — redundantly — before the
  `IR_BINOP` whose `expr` still carries the AST), so it does run per iteration. The switch
  has no `default:`; a new statement kind is a build failure until classified.

**The exit block.** Compare in `u64` — the emitter's guard is `(size_t)(idx) >= N`, and
a signed compare lets a NEGATIVE index through (`for (i32 i = start; …)` with `start =
-2`: `-2 >= 4` is false, no guard, stack OOB — `vrp_for_signed_neg_init_guard` caught
exactly that in the first draft). Then either `IR_TRAP` (a new terminator, BUG-957) when
`critical_depth > 0 || defer_body_depth > 0` — a return there would skip the interrupt
re-enable, or re-fire the defer stack — or: fire defers, `IR_UNLOCK` on
`current_stmt_shared_root` if one is inherited, `IR_RETURN` a zero literal. Using
`IR_RETURN` rather than open-coding the return is what makes this small: the emitter's
IR_RETURN already does the async termination sequence, `void main()` promotion and
array-to-slice coercion. The IR exit is strictly better than the C one — it can RETURN
inside a held lock where the C guard could only `_zer_trap("cannot return without
leaking it")` (`guard_in_shared_lock_traps` became `guard_before_shared_lock_ok`).

**`ir_op_takes_auto_guards` (emitter.c) — the op-kind gate FAILS CLOSED.** It decides
which IR instructions get the emitter's C-level guards for their `expr`. It was an
ALLOWLIST of eight op kinds, inline at the emission loop, in two copies (regular and
async), and it was widened reactively three times, each after a measured miscompile:

| when | added | why |
|---|---|---|
| 2026-05-03/06 | the whole async copy | async silently miscompiled an unproven `arr[i]` |
| 2026-06-30 | `IR_AWAIT`, `IR_NOP` | await conditions, spawn args |
| 2026-09-07 | `IR_LOCK` | BUG-952 — the guard landed INSIDE the lock (`pthread_mutex_lock(&(_zer_bounds_check(i,4,…), arr)[i]._zer_mtx)` trapped before the guard could return) |

It is now ONE exhaustive `switch` shared by both loops: every kind is listed as `true`,
`IR_UNLOCK` alone is `false` (it carries the same indexed shared root as `IR_LOCK` but runs
with the lock HELD, so a guard there could only trap, and the LOCK guard has already
returned early on that path). A new `IROpKind` is a `-Werror=switch` build failure until
classified — verified by adding a fake kind — and `IR_TRAP` was the first real one to hit
it, five switches later. Measured cost of the inversion: 11 extra guards across 587 corpus
files, **0 lost**; every extra one is redundant, not a closed gap (the read above it was
already guarded).

**Two things that cost a turn each, so they are written down:**

- **`lower_expr`'s `NODE_INDEX` arm is NOT the choke point.** An assignment TARGET never
  passes through it — `arr[i] = 7` is an `IR_ASSIGN` passthrough carrying the whole AST.
  Wiring the guard there guarded an unrelated `arr[0]` READ in another function and
  nothing at the reported line, while looking like it worked. The trace, not reasoning,
  found it. That is why the descent is a statement-level walk over the AST.
- **A file-static "already lowered" set never resets between compilations.** The
  harnesses compile many programs per process, arena addresses are reused, and a stale
  node pointer would make the emitter SKIP a guard that was never lowered. The set is a
  `Checker` field (`guard_lowered`), memset clean by `checker_init`.

**A pre-existing over-eagerness, noticed and NOT changed:** `x = f() orelse arr[i]` hoists
the fallback's guard, so the early return can fire even when `f()` succeeded and the
fallback was never evaluated. `emit_auto_guards` already descended `orelse.fallback`; the
IR path inherits it. Recorded rather than quietly changed under cover of a refactor.

**Why M had to precede L.** The C-level guard's early return calls `emit_defers` — the
raw-AST defer path — from an exit that does not exist in the IR at all. While guards lived
outside the IR, splicing defer bodies into the CFG either double-fired them on the guard
path or skipped cleanup there. Three attempts at L in a row hit that wall; each was
reverted clean, and the finding is why the order is M-then-L.

### THE DEFER BODY IS IR NOW — read this before the two sections below (2026-09-08, BUG-959..966)

**A defer body is lowered ONCE at its registration into a detached block range, and
CLONED into the CFG at every fire — for every function that does NOT contain a LABEL.**
`ir_lower.c`: `materialise_defer_body` / `materialise_defers_from`, the template arrays
on `LowerCtx`, and the single query `defers_stay_on_ast(ctx)` (= `label_count > 0`).

Consequences a session touching defer code must know:

- **UPDATE 2026-09-25 (BUG-1298): the template is lowered in EVERY function now.** A
  labelled function still does not SPLICE it (the reasons below stand); instead the
  push carries it (`IRInst.defer_tpl`, an `IRDeferTpl` of detached blocks) and the
  emitter's `emit_defer_body` emits it INLINE at each AST-flagged fire and at every
  C-level early exit (`emit_defers_from`), with fresh `_zer_bb<1<<24+n>` labels, through
  `emit_ir_inst_guarded` — the one per-instruction emitter the regular and async block
  loops also use. `emit_defer_stmt` is only the fallback for a body with no template.
  The "never lower on the label path" rule below no longer applies: nothing REPLAYS the
  AST for emission any more, and zercheck's AST scans look only for frees and uses,
  which `pre_lower_orelse` leaves in place.
- **`zercheck_ir`'s AST defer analysis is the LABEL path only.** At a return it applies
  only the bodies of fires that REACH that return with no work after them
  (`ir_fire_mark_returns`); a fire with work after (the eager fire at a `goto`) has its
  uses AND frees applied in the forward pass. Every expression position gets both raw-AST
  checks (`ir_defer_check_expr` = UAF + wrong-pool).
- **`emit_defer_stmt` and `zercheck_ir`'s AST defer analysis were the LABEL path
  only (superseded for emission by the update above).** Both are gated on `IRInst.defer_fire_emit_ast`, which `make_defer_fire` — the
  ONLY constructor of an `IR_DEFER_FIRE` — sets from that one query. Add a new fire site
  through that constructor or the two consumers will disagree with the lowerer, which is
  precisely how eleven goto/defer tests broke during this work.
- **`IR_DEFER_PUSH` / `IR_DEFER_FIRE` still exist** — `ir_validate` checks their balance
  and `e->defer_stack` still feeds `emit_defers_from` at the remaining C-level guard
  exits. Only body EMISSION moved.
- **Never lower a defer body on the label path.** `pre_lower_orelse` REWRITES nodes, so
  lowering a template the emitter will also replay violates "never lower the same AST
  twice" from inside a single lowering.
- **A guard inside a defer body TRAPS** (`defer_body_depth`), because an early return
  there would re-fire the defer stack. The same counter suppresses a fire emitted from
  inside a body.
- **A fire at an already-terminated position does not materialise.** Splicing a body that
  cannot run makes the ANALYSIS wrong (zercheck walks every block and counts the frees
  again), so this is correctness, not an optimisation.


#### The machinery under it — what a session touching defer lowering must not re-derive

**Cloning is at the IR level, and that is the only reason L was tractable.** The obvious
route — lower the same body AST once per fire site — hits two blockers: lowering MUTATES
the AST (`rewrite_idents` renames in place, `pre_lower_orelse` REPLACES each orelse with an
ident naming a temp created in the first site's blocks — a second lowering finds no
orelse and leaves a dangling reference), and the typemap is keyed by `Node *`, so a cloned
node has no type and `checker_get_type` returns NULL for it. An AST cloner would be a
53-kind walker plus typemap copying. An `IRInst` names other blocks ONLY by integer index
(`true_block` / `false_block` / `goto_block`) and locals only by id, so duplicating a body
is copying a contiguous block range and offsetting three fields.

- **`ir_clone_block_range(func, arena, first, last)` / `ir_append_block_copies(func,
  arena, src, n, src_first)`** (ir.c, BUG-950). The external-source form exists because a
  template is EXTRACTED out of the function (so no skip-flags are needed anywhere, and a
  template's `free()` cannot mask a real leak in zercheck) — the clone copies from the
  saved snapshot, not from `func->blocks`. Allocate every clone FIRST (`ir_add_block`
  reallocs `func->blocks`, so pointers read before the loop dangle). A block reference is
  remapped ONLY when it points INTO the cloned range; one outside it still names a block
  that exists. `make_inst` initialises all three fields to `-1`, so remap on `>= 0` needs no
  op-kind gate — a remap keyed on `!= 0` would have turned `-1` into block 0, the ENTRY
  block, a live target and a plausible-looking corrupt CFG. `label` and `preds` are
  deliberately NOT copied (two blocks with one label = duplicate C goto targets; preds are
  recomputed). Tests in `test_ir_validate.c`, mutation-checked (the three ways it can be
  wrong each fail a different assertion).
- **Lower the template at REGISTRATION, not at the first fire** (BUG-959). The first fire
  can be inside a NESTED scope (`{ Handle h = mh2 orelse return; }` fires the outer defer
  from inside), and re-lowering there resolved `pool.free(h)` to the inner shadowed
  handle. Restoring the scope DEPTH alone was measured and still resolved to the shadow;
  lowering where the defer was written reproduces the context by construction. The
  earlier objection to eager lowering — dead locals in 12 of 60 defer programs — was
  measured BEFORE a consumer existed; with every fire cloning the template its locals are
  used.
- **Resume at the template's EXIT block, not its last index** (BUG-963,
  `defer_tpl_exit[]`). For a body containing a `for`, the exit block is created before the
  post block, so `base + n - 1` lands INSIDE the loop and leaves the real exit
  unterminated — `ir_validate` then reports "IR_DEFER_PUSH has no CFG-reachable
  IR_DEFER_FIRE". Diagnose such shapes by downgrading the validator to a warning and
  dumping the CFG; `--emit-ir` is useless because validation aborts first.
- **`ir_compute_preds` reads a block's FIRST TERMINATOR, not its last instruction**
  (BUG-964). Lowering appends past a terminator — a `return` in a switch arm terminates
  the block and the arm's end still emits `IR_DEFER_FIRE` + `GOTO join`. Reading the last
  instruction gave the join a phantom predecessor; once bodies were real IR, zercheck
  followed it out of a path that had freed a handle into one that freed it again. **Do NOT
  fix this by deleting instructions past a terminator** — that was the first fix, and it
  silently lost the use-after-move report on `return t; u32 k = t.kind;`: unreachable
  instructions are still ANALYSED. `IR_TRAP` had the same phantom fall-through edge and
  now has an explicit no-successor case. `clear_unreachable_blocks` was built for this,
  found to cost the dead-code diagnostics, and removed.
- **Dominators were built and removed.** `ir_compute_dominators` / `ir_dominates` landed as
  the prerequisite for eliding the ARMED gate, then the label-path split made them
  consumer-less; 45 tested lines with no caller were deleted. Do not re-add them without a
  consumer.
- **The bare `orelse return` fallback tags the ENDING block** (BUG-966). Firing defers now
  splices and leaves `current_block` on a fresh continuation, so the `IR_RETURN` no longer
  landed in `bb_fail` and `is_orelse_fallback` missed it; the leak check then read the
  null-optional path as a live allocation. The BLOCK form always tagged the ending block.
- **`defer_body_depth` does three jobs**: guard -> `IR_TRAP` inside a body; suppress a fire
  emitted from inside a body (a defer body has no defers of its own — this is what
  unblocked `break` inside a defer body's own loop); and break the M/L mutual recursion
  (a guard's exit fires defers, materialising a body lowers statements carrying guards —
  ASan reported it as a compiler stack overflow).
- **Every `orelse` form is banned inside a defer body**, which is the only reason lowering
  a template is sound at all: `pre_lower_orelse` is provably a no-op there, so the
  no-behaviour-change checkpoint was possible.

**Still on the AST path: any function containing a LABEL.** Both runtime gates a defer
body can carry — ARMED (a forward `goto` may jump over a registration) and the plt86m
cleanup-label GUARD (the goto fires eagerly and SETS a flag; the fall-through fire runs
`if (!flag)`) — exist only then, and as IR branches they would be VISIBLE to zercheck,
which reads `{freed} U {alive}` as MAYBE_FREED. The GUARD flag genuinely varies (that is
its purpose), so dominance cannot recover it; closing this needs guard-disjointness over a
REASSIGNED flag, which Level B's `ir_local_is_immutable_bool` deliberately refuses. So
`emit_defer_stmt` (BUG-1012 added a LABEL refusal to it — a label in a defer body used to
be a `compiler bug:` line plus a trap) and zercheck's AST defer analysis both stay, gated
on `defer_fire_emit_ast`. The remaining C-level guard exits (`emitter.c` ~647 and the
loop `break`/`continue` paths ~3337) still call `emit_defers` / `emit_defers_from`, which
is why `e->defer_stack` bookkeeping is kept.

**WHAT THE SPLIT ACTUALLY BOUGHT — measured 2026-09-08 by A/B-ing against a build of the
commit right before L, not recalled.** The number that explains the whole class:

| emitter | node kinds handled |
|---|---|
| `emit_defer_stmt` (the defer-body emitter) | **11** — ASM, ASSIGN, BLOCK, BREAK, CONTINUE, EXPR_STMT, FOR, IF, RETURN, VAR_DECL, WHILE |
| `lower_stmt` (every other statement in the language) | **53** |

Its `default:` arm is deliberately loud — it prints `compiler bug: emit_defer_stmt has no
handler for node kind %d` and emits a `_zer_trap` — but loud in a place nobody looks is
still valid ZER turned into a runtime trap. Measured pre-L, each of `switch`, `do-while`
and `@critical` inside a defer body produced that message twice and two traps; post-L,
zero and zero.

The subtler one, and a real DATA RACE: a shared read in a defer-body CONDITION took no
mutex. `defer { if (g.v > 3) { … } }` emitted a bare `if ((g.v > 3))`. BUG-749 (B5)
lock-wrapped the defer body's `NODE_EXPR_STMT` and nothing else, so the ASSIGNMENT form
locked and the CONDITION did not — the partial-coverage shape, occurring inside the
partial emitter. Post-L the body is lowered by the ordinary path and the lock is there.

**NOT a win, though the first draft of this section and of the BUG-959 comment both said
so: the auto-guard.** It was already emitted — refactor M put it there. L only changes it
from C text re-emitted at every fire site into one IR branch inside the template. That
claim had been inherited from `limitations.md`'s L motivation, which was measured BEFORE M
landed, and repeated without re-measuring. It is the reason the baseline-compiler A/B is
now a standing rule in CLAUDE.md.

The two sections below describe the machinery of the LABEL path. It is unchanged and
still load-bearing there.

### capture-on-FIRE + runtime-flag defer emission (2026-06-20 — plt86m defer-goto)

**The defer body-emission model changed: each `IR_DEFER_FIRE` carries its OWN
snapshot of live defer bodies (captured at lowering), and a both-reachable
cleanup label fire is runtime-guarded.** Mandatory before touching defer
lowering/emission.

THE BUG it fixed: the emitter walked basic blocks in block-ID order with ONE
shared mutable `e->defer_stack` (push on `IR_DEFER_PUSH`, emit-from + truncate
on `IR_DEFER_FIRE`). Block-ID order ≠ control-flow order, so two MUTUALLY-
EXCLUSIVE sibling exits of one scope (a goto-path block + a fall-through block,
each with its own FIRE) had the first-emitted FIRE pop the stack and the second
emit NOTHING — silently dropping cleanup on the non-goto path. But that same
`pop=true` was LOAD-BEARING: it also silenced the goto-to-cleanup-label
double-fire (`goto cleanup; cleanup: return` — the goto fires the defers, and
the cleanup return must not re-fire). So the drop bug and the double-fire
prevention were ENTANGLED in the shared stack.

THE FIX (`IRInst.defer_fire_bodies`/`_body_count` + `_guard_flag`/`_guard_below`):
- **capture-on-FIRE.** LowerCtx has a parallel depth-indexed `defer_bodies[]`
  stack (stack-first 32 + arena overflow). NODE_DEFER records the body at its
  depth. `emit_defer_fire`/`_scoped` call `ir_snapshot_defer_bodies` to copy
  `defer_bodies[base..count)` into the fire (arena). The emitter emits that
  snapshot (LIFO), NOT the shared stack — order-independent, fixes the DROP. The
  `defer_stack` push/pop bookkeeping stays for `ir_validate` balance but is dead
  for emission.
- **goto-only cleanup label** (no live fall-through — preceding block terminated
  or the empty dead block after a return): reset `defer_count = 0` so its exit
  fires nothing. No flag.
- **both-reachable cleanup label** (a LIVE fall-through reaches it): RUNTIME
  GUARD. `IRLabelMap.guard_flag_local` is a bool local (lazily via
  `get_label_guard_flag`); the goto, after firing eagerly, sets it
  (`IR_LITERAL` flag=1); the label sets `ctx->active_guard_flag`/`_below =
  defer_count`; `ir_snapshot_defer_bodies` attaches them so the emitter wraps
  each body of original depth < guard_below in `if (!flag) { body }`. Goto path:
  flag=1 → skip (already fired). Fall-through path: flag=0 → fire AT the return,
  AFTER the return expr is evaluated (BUG-442). Defers registered after the
  label (depth ≥ guard_below) stay unguarded.

INVARIANTS for future work:
- Snapshot at LOWERING, emit at EMISSION. Never re-introduce a block-order stack
  replay for body emission.
- Do NOT change the NODE_GOTO `ctx->defer_count = 0` reset — it keeps the linear
  count consistent for the goto path.
- `make_inst` must keep `defer_fire_guard_flag = -1` (0 is a valid local id).
- Three earlier attempts were dead ends (plain capture-on-FIRE → cleanup
  double-fire; +reset → misses dead-code-before-label; +fall-through-edge-fire →
  violates BUG-442 timing). See BUGS-FIXED.md "Session 2026-06-20".
- VERIFY defer changes by DIFFING emitted C per control-flow path (free/mark
  counts), NOT by test exit codes — several defer+goto tests pass despite wrong
  cleanup because they assert return values, not side-effects
  (rt_drop_defer_goto_cleanup, rt_goto_fires_defer, gen_defer_009).

### Bounded walks — the fail-open class, its three remedies, and the cap map (BUG-975..978, 994, 998, 1010)

CLAUDE.md carries the rules; this is the mechanism map and the measurements behind them,
so nobody re-derives which remedy a walk needs.

**The defect.** A depth-limited walk that returns the SAFE answer past its limit does not
lose precision — it ACCEPTS, and nesting past the cap is a supported way to get anything
through the rule. BUG-975 was the purest instance: `eval_const_expr_ex` had `depth > 256`
and incremented properly down unary/binary nodes, but its IDENT arm called the resolver
with NO depth and `resolve_const_ident` restarted the evaluator at 0, so `const u32 A =
A;` HUNG and a two-node mutual cycle SEGFAULTED. The bound existed and structurally could
not fire. **Grep the recursion for a literal `0` passed where a depth belongs.**

**Enumerate, do not fix the reported ones.** The command, and what it turned up:

```
grep -nE "depth *> *[0-9]+\)" checker.c zercheck_ir.c     # then map each to its function + return value
```

BUG-976 found nineteen, every one returning the safe answer; the 17 branch reproducers
covered about half. Re-run it after EVERY branch adoption: `type_carries_forgeable` /
`type_carries_enum_c` arrived from a branch at `depth > 32 -> false` on a construct with no
syntactic limit (an enum 40 structs deep was forgeable through `@pun`), the two factory
walks arrived at `depth > 8 -> false` over AST nesting (a `return cb;` nine ifs deep was
accepted at both sinks up to the parser's 64), and BUG-1010's RMW taint walkers arrived
as if-chains with their own caps.

**Pick the remedy by what the walk PRODUCES:**

| the walk returns | remedy | examples |
|---|---|---|
| a boolean | **FLIP** to the dangerous answer | `arg_is_local_derived`, `call_has_local_derived_arg`, `call_has_nonkeep_derived_arg`, `type_carries_data_pointer/handle/nonshared_pointer/forgeable/enum_c/bool_c`, `expr_mentions_name`, `func_rmw_param_mask`, `func_forwards_param_to_spawn`, `value_is_existing_resource`, `rmw_value_taints_global`, `expr_touches_local_derived` |
| nothing (a scan) | **REPORT**, naming the callee it refused to enter | `scan_unsafe_global_access` (via `_scan_depth_exceeded` + `scan_finding_noun`'s third case), `record_isr_globals` (void — reports directly), both factory walks past `FACTORY_NEST_MAX` (64 = the parser's own bound), `record_isr_funcname_binding` |
| names for a LATER rule | **WIDEN** — it cannot be flipped ("assume every global is an atomic cell" rejects everything) | `record_atomic_plain_in_callee` 8 -> 32 |
| a `Symbol *` the diagnostic NAMES | **REPORT via a `gave_up` flag**, naming the DESTINATION (which the caller always has) | `value_frame_bound_symbol` (BUG-978, `ESCAPE_ORELSE_WALK_MAX` 256) |
| a name the diagnostic PRINTS | **a pointer-compared SENTINEL** with its own sentence | `unique_resource_name` returns `_res_walk_stopped` past 64 (BUG-977b — the BUG-976 flip returned the literal `"resource"` and the diagnostic told the author `'resource' is addressed by NAME … conventionally a global` about a plain struct; the verdict was right and the sentence was fiction) |
| a boolean read by TWO rules of OPPOSITE polarity | **TRI-STATE**; each sink picks its reading of UNKNOWN | `ir_move_verdict_depth` -> `IRMoveVerdict` YES/NO/UNKNOWN (BUG-977): the copy sink reads UNKNOWN as YES (BUG-933's fail-closed), the leak sink as NO (move-tracked locals are EXEMPT from leak checking, so `true` there exempted a 34-deep `Handle`). A THIRD polarity exists and must not change: the BUG-703 site that UNDOES a same-line move-compound materialisation (`grep -n 'undo same-line' zercheck_ir.c`) reads UNKNOWN as YES through `ir_should_track_move` — **a mark and its undo must use the same reading** or the undo fails to undo |

**A generous cap and a conservative answer are not alternatives — you need both.** Flipping
`unique_resource_name` at its cap of 8 made a 34-deep nested struct read as a resource and
MASKED a use-after-move test (a wrong diagnostic is worse than the permissive answer). Raise
the cap past anything real FIRST, then round toward reject.

**Measure the reachable window before choosing the cap.** For the orelse chain (BUG-978)
the hole was reachable at depths 9..95 and no further: a parenthesised chain trips the
parser's "expression nesting too deep (limit 256)" at 84 (measured by bisection), a flat one
trips zercheck's "did not converge within 96 iterations" at 96. **When another limit already
bounds the construct below your cap, the hole is closed by construction** and the report is
a backstop. Struct nesting has NO such limit (300 and 500 deep both tested), which is why
BUG-977 needed the tri-state and not a cap raise. **A report path no program can reach must
still be verified to FIRE** — lower the cap to 3, confirm every branch prints with the
right name, restore.

**A fixed TABLE that silently drops is the same defect.** `_rmw_alias[16]` and
`_static_locals[32]` stopped recording when full; the 17th alias and 33rd static local
were never seen. Both set an overflow flag and their lookups answer conservatively once
set (`_rmw_vtaint` is growable). BUG-935 was the same shape on a 16-slot safety collector.

**A cap that is RESET on a hop is not a cap** (BUG-975). Fixed by threading `depth` through
`ConstIdentResolver` (one call site in ast.h) AND a name stack in `resolve_const_ident`
(`_cident_name[CONST_CHAIN_MAX]`, 64): a CYCLE and a too-long CHAIN need different
diagnostics — the branch's first cut used the bound alone and reported a 70-link chain as
"cyclic", a wrong answer, not a rough one.

**A scan that descends into a callee must not reset the caller's context** (BUG-998). The
spawn race scan enters a bound / returned function from the MIDDLE of the caller's walk and
called `rmw_alias_reset()` so the callee starts clean — which emptied the caller's alias
table, so `volatile *u32 p = &g; fp(); *p += 1;` lost `p -> g`. `RmwScanCtx` /
`rmw_ctx_save` / `rmw_ctx_restore` snapshot the CONTEXT tables around the descent and
leave the FINDING flags alone, so a callee's finding still propagates out.

**The durable end-state for a walk over a TYPE is no cap at all.** The type graph is
finite and acyclic — the checker rejects self-containment ("struct 'A' cannot contain
itself by value") and a by-value field cannot forward-reference, so mutual recursion is
unspellable. A visited-set walk keyed on `Type *` terminates. Every type walk in checker.c
/ zercheck_ir.c (`type_carries_*`, `unique_resource_name`, `ir_move_verdict_depth`, …) is
carrying a bound it does not need; the caps survive only as guards against a malformed
type built during error recovery.

**The enumeration is CLOSED (2026-09-14, BUG-1016).** The 2026-09-13 grep listed eight caps
answering in the ACCEPT direction; all eight are now measured and fixed, FIVE of them live.
The durable output is a set of SHARED BOUNDS at the top of checker.c, so a walk's cap is
tied to the limit that already refuses the program rather than a magic number:

- `ZER_EXPR_WALK_MAX` (= `ZER_EXPR_NESTING_LIMIT` + 8) — a walk over an EXPRESSION tree.
  **`check_expr`'s bound is 1000, NOT the parser's 256** — a left-associative chain
  `g + 1 + 1 + …` parses ITERATIVELY, so a 300-term chain reaches AST depth 300 under the
  parser's per-node counter; `rmw_value_source_global`'s old 256 was below that and lost an
  RMW taint, and `packed_path_aggregate`'s 64 let a 70-deep misaligned `&p.a…w[0]` through.
- `ZER_TYPE_NEST_MAX` (256) — a TYPE's syntactic wrappers (pointer/array/optional). **BY-VALUE
  STRUCT nesting has NO syntactic limit**, so a type walk through struct fields must still
  round toward reject past its cap (BUG-977's tri-state) or use a visited set — the bound
  covers only wrapper depth.
- `ZER_STMT_NEST_MAX` (64) — statement nesting. A walk that counts BOTH the block and the
  statement inside it sees ~2 levels per source level.

The five live: `packed_path_aggregate` (misaligned access at depth 70), the funcptr-field
binding scan (`body_calls_funcptr_field`/`scan_funcptr_field_bindings`, a racy binding 9
calls deep — WIDENED to 32, a call-graph cap not a syntactic one), `for_init_has_loop_jump`
(if-chain, cap 32 fail-open AND missing INDEX/SLICE/FIELD — now a no-`default:` switch),
`compute_max_depth` (stack under-reported past 256 frames, slipping `--stack-limit` — bound
is the frame count, since an acyclic DFS visits each once and a cycle is caught as
`is_recursive`), and `global_init_scan` (availability: a call 200 terms deep reached GCC
instead of the ZER line). The three hardenings (`rmw_*`, `resolve_write_target_global`,
`node_forwards_param_to_spawn`/`tynode_keeps_storage_inline`/`ct_expr_bits`) had no clean
discriminator — the cap was below reachable depth or already the accept direction with a
masking rule in front. Full table + reproducers: limitations.md "CLOSED — the BUG-976
depth-cap enumeration is closed". Zero corpus cost (2621 files, both binaries).

**Two things a fresh session must not re-derive.** (1) The bound for an expression walk is
check_expr's 1000, not the parser's 256 — the parser counts NESTING and a flat chain has
depth-1 nesting but N-term AST depth. (2) `tynode_keeps_storage_inline` and every `type_carries_*`
walk over STRUCT FIELDS is NOT closed by `ZER_TYPE_NEST_MAX` — struct nesting is unbounded;
those round toward reject past the cap (BUG-977), the type-nesting bound only covers the
pointer/array/optional wrapper walks (`packed_step_aggregate`, `resolve_write_target_global`).

### Concurrency rules that changed in the run (BUG-948, 973, 979, 980, 1010, 1013, 1014)

The "Deadlock Detection", "Scoped Spawn" and "Interrupt Safety Analysis" sections lower in
this file describe the base mechanisms; each of these is a correction layered on one of
them, and the base sections carry a one-line pointer here.

**The deadlock check is a three-step probe per statement, not one collection** (BUG-948 +
BUG-980, `check_block_lock_ordering`). (1) `lockchk_direct_only` — collect the types the
statement touches DIRECTLY. A statement with no direct access holds no lock across its
calls (the emitter takes a lock only for a direct access — verified in emitted C), so the
old unconditional transitive merge rejected `u32 r = touch_both();` which cannot deadlock;
the merge now runs only when `ndirect > 0`. (2) the existing two-different-types rule.
(3) `lockchk_callee_only` — the MIRROR of (1): the types reached only THROUGH the
statement's calls — INTERSECTED with the direct set, for `shared(rw)` roots only. A
`pthread_rwlock` is NOT recursive (a plain `shared` mutex is `PTHREAD_MUTEX_RECURSIVE`),
so ONE type is enough to deadlock, and that same-type case is what the `id0 != id1` guard
excluded by construction. A COUNT can never see it: the full pass dedupes the callee's
copy of a type the statement already touches. Measured pre-fix: write-lock re-entry gave
exit 0 and SILENT CORRUPTION (glibc's second `wrlock` returns EDEADLK, the emitted C
ignores it, the callee runs unlocked, its `unlock` releases the CALLER's lock);
read-then-write HANGS. Both modes run the SAME exhaustive collector, so a node kind cannot
go missing in one of them. Two boundaries measured, not assumed: **a bare-name callee is
not necessarily a function** (`fp()` on a funcptr local is a `NODE_IDENT`, and the
transitive lookup would create an empty cache entry and contribute nothing — silently),
and **a bodyless extern is not opaque here** (C cannot name a ZER rwlock; only an UNKNOWN
target — a funcptr call — sets `lockchk_saw_opaque_call` and is refused under a held
`shared(rw)` lock). Read-lock re-entry is refused too (`FuncSharedTypes` records type ids
only, so "the callee only reads" is not available; OPEN in limitations.md).

**BUG-949, found underneath BUG-948:** `find_func_shared_cache` returns a pointer INTO
`func_shared_cache`, which `add_func_shared_cache` REALLOCS (initial capacity 16), and
`scan_body_shared_types` held that pointer across a recursive call that can add an entry.
ASan-confirmed `heap-use-after-free` in `fsc_add_type_id` on the intermediate build; it
had not manifested on main only by allocation luck. Re-look-up after any call that may
grow the cache — the `zercheck_ir.c find-then-add` rule (BUG-616) in the checker.

**Spawn locks per ARGUMENT, not per statement** (BUG-973). The emitter marshals each
argument under its own lock scope and labels them (`/* shared-read lock for spawn arg 0
*/`), so `spawn w(a.x, b.y)` is two correctly-held locks and `spawn w(a.x + b.y)` is the
hazard. `collect_shared_in_spawn_args` runs the collector per argument at BOTH spellings
(the bare statement, and `ThreadHandle th = spawn …`, which arrives as a var-decl init).
`NODE_SPAWN` had been EXPLICITLY listed in the collector's "returns 0" arm — the exhaustive
switch forced the classification to be written down and it was wrong.

**The concurrent window is REFCOUNTED, not latched** (BUG-979). `after_spawn_in_func`
was set by a fire-and-forget spawn and by NOTHING for a scoped one, so the spawn..join
window was exempt from the atomic-cell rule purely by spelling. Now `Checker.
live_scoped_threads` (`++` at the scoped spawn, `--` at a join that is no deeper in
conditional nesting than its spawn — `Symbol.th_live`) and `unbounded_spawn_in_func`
(latched by a fire-and-forget spawn; nothing can join it). The window is open while either
is non-zero; a join on SOME paths leaves it open. Gate: CONCURRENT-WINDOW grid in
`tests/test_conc_matrix.c`.

**The RMW rule is a value TAINT, not a per-statement test** (BUG-1010). `u32 t = g; g = t
+ 1;` is the same lost update as `g += 1` and was accepted at both race sinks. A
NAME -> GLOBAL taint rides the VarRange rails (`RmwTaintEnt`; the ISR sink's per-function
map `Checker.rmw_taints`, the spawn sink's per-scan table `_rmw_vtaint`, growable):
seeded at var-decl and assignment from `rmw_value_source_global` (exhaustive walker, cap
= parser bound because the SOURCE lookup has no conservative value), consumed by
`rmw_value_taints_global` (cap rounds to TRUE) at a write to G whose value mentions a
tainted local; a re-binding from no global CLEARS it. Sits beside BUG-841's
`resolve_write_target_global` (sees through `*p` / `*gp` to the pointee) and
`assign_reads_own_target` (a written-out `g = g + 1`). Gate: RMW FORM grid in
`tests/test_hw_matrix.c` (site x spelling, now with the split-statement and 2-hop forms).

**A `shared struct` field in an ASM OPERAND is refused** (BUG-1013). The per-statement
auto-lock never wraps an asm statement, so `inputs: { "rax" = a.x }` was a bare read
reachable from a spawned thread. Refused by the Ban Decision Framework's hardware branch:
asm is legal only in a `naked` function, which has no frame to take a mutex in. Both
operand lists go through `collect_shared_types_in_expr` (forward-declared for it).

**A carrier lending TWO roots is refused** (BUG-1014). `Symbol.borrow_root_name` holds
ONE name and the last field write won, so `h.p = &v; h.q = &x; spawn w(h); v = 1;` lent
`x` and let the write to `v` race. A second, different root sets
`borrow_root_ambiguous` in `record_borrow_root`; the spawn sink refuses the argument.
Lending every root is the precision fix (OPEN).

### The forging doors, and the question "does the advertised runtime check exist for THESE operands?" (BUG-987/988/989/1004)

The "@pun intrinsic" and "@container Field Validation" sections further down describe the
original checks. Three of them advertised a runtime check that did not exist for the
operands that mattered; the pattern is a checklist question worth asking of any intrinsic
that promises a trap: **for which operand types is the emitted check a compile-time
constant, or absent entirely?**

- **`@pun` (988).** The emitted check is `pn.type_id != TGT && pn.type_id != 0`, and only
  struct / enum / union pointees carry a `type_id`; everything else packs 0, and `!= 0` is
  exactly the escape. `@pun(*P, u64ptr)` where `P { *u32 p; }` turned an integer into a
  working pointer with no `@inttoptr` and no `mmio` (wrote 42 through it); a forged enum
  dispatched to an arm; a forged slice `len` was trusted by every bounds check. Fix:
  refuse only when `pun_type_id_check_can_fire` says the check CANNOT fire — it MIRRORS
  the emitter's rule and must change with it — AND the pointees differ AND the target
  `type_carries_forgeable` (pointer / opaque / slice / funcptr / enum / bool / optional /
  Handle / builtin container / tagged union, recursing arrays and structs; exhaustive, no
  `default:`, cap rounds toward reject). Reinterpreting bits as plain integers forges
  nothing, so `@pun(*u8, structptr)` still compiles. Measured beside it: a struct-to-struct
  pun between DIFFERENT structs always traps (the ids differ statically), so "runtime-
  checked" there means "runtime-refused". The tell: BH-18 #4's commit had already written
  down that the trap is skipped for primitives while closing only the WIDENING half.
- **`@inttoptr` to an enum- or bool-carrying pointee is REJECTED, not guarded**
  (989/1004). `volatile *State reg = @inttoptr(*State, a); switch (*reg)` returned an arm
  on a register holding 200, zero guards emitted — a fourth enum-forging door found by
  challenging the sentence "ZER has no int->enum cast" (it has an int->POINTER cast).
  Guarding would mean firing at every READ through the pointer — a new N-sink surface — so
  the rejection routes users to the guarded `@bitcast(State, *r)` idiom and the guarded
  door set stays at three. `type_carries_enum_c` / `type_carries_bool_c` at the
  `@inttoptr` gate; `@ptrcast` fires when either pointee is enum-or-bool and they differ
  (`@ptrcast(*bool, u8ptr)` minted a `bool` whose load was 2, so `== true` and `== false`
  were both false). The pointer-carrying pointee is deliberately unshipped (OPEN; a
  non-zero corpus cost in `lib/compat.zer`, no measured wrong-dispatch).
- **`@container` had a two-valued domain for a three-valued fact** (987). Provenance
  knew "came from `&outer.field`" and NULL-meaning-unknown-allow; `*Inner ip = &i;` of a
  standalone object is KNOWN-BAD, not unknown, and `@container` subtracted the offset and
  read BEFORE the object (ASan stack-buffer-underflow). `Symbol.is_whole_object_addr` is
  the missing state, mutually exclusive with `container_struct`, written ONLY through
  `set_container_prov_{field,whole,unknown}` so a re-pointed pointer cannot keep a stale
  claim. Second sink found by enumeration: `@container(*Outer, &i, in)` written directly
  skipped EVERY check; both sinks resolve through one `classify_amp_operand` (`&arr[i]` is
  WHOLE for the same reason).

### Emitter facts added in the run (BUG-990/991/993/997/1001/1003/1011)

- **`emit_double_lit` is the ONLY way a `double` reaches emitted C** (991). `%.17g` is
  exact for finite doubles and prints the non-C tokens `inf` / `-inf` / `nan` for the
  other three, so `f64 x = 1e400;` emitted `x = inf;` and GCC blamed the user's `.zer`
  line. The helper renders `__builtin_inf()` / `(-__builtin_inf())` / `__builtin_nan("")`
  (valid in a static initializer and under `-ffreestanding`; NaN tested first, the
  BUG-883 reason). Was five sites. **Gated**: `tools/audit_float_literal.sh` fails the
  build on any float conversion specifier outside the helper — the 10th `make check`
  gate, verified to go RED.
- **`f2i_limits` + `emit_f2i_const`** (990). The BUG-883 float->int saturation guard is a
  GCC statement expression and is illegal at file scope, so a global `u32 g = (u32)1e20;`
  did not build. `emit_f2i_const` is the same saturation as a constant expression, used at
  the AST typecast path when the operand can be re-evaluated for free (no side effects,
  not volatile — a global initializer always qualifies). The four limits are ONE table
  shared with the statement form; two copies of the bounds is the drift shape.
- **A global initialized from a `const` global substitutes that global's OWN initializer**
  (997, `Emitter.global_init_depth > 0` in the NODE_IDENT arm, wrapped by
  `emit_global_var`). `u32 B = SRC;` reached GCC as `uint32_t B = SRC;` — "initializer
  element is not constant" — for every non-integer shape (the old fold covered only an
  integer `eval_const_expr` could evaluate). Correct by construction: that expression
  already passed the file-scope rules for its own declaration. A MUTABLE global is refused
  in the checker at the ZER line.
- **`@bitcast` with an ARRAY on either side** (1000/1001). Target array: refused (the
  declarator was invalid C and the value is not assignable). Source array: BOTH paths copy
  from the array expression directly — `__auto_type b = a` DECAYS to a pointer, so the old
  form copied the bytes of the pointer and `@bitcast(u64, a)` returned `a`'s address (ran
  and returned 1 on main). `grep -c '"bitcast"' emitter.c` = 2.
- **`emit_line_map` re-anchors `#line` PER INSTRUCTION in both IR emitters** (1003). IR
  block emission had `#line` off wholesale (the BUG-418 collision with labels and
  statement expressions), so every runtime trap reported "function's line + offset in the
  generated C" — a 7-line file said line 14. Per-instruction is safe where wholesale was
  not, because that point is always between statements at column 0, never inside `({…})`.
  It runs BEFORE the auto-guards. `tests/test_zer.sh` gained `// expect-trap-at: N` for
  `zer_trap` tests; four are pinned and fail against the pre-fix compiler. **Plausible
  numbers are the ones nobody checks.**
- **A volatile IDENT is admitted to the single-evaluation inline bounds form** (1011). The
  auto-guard `if (i >= N) return; … arr[i]` is two loads for a volatile, and whatever
  changes it between them walks past the guard. The checker leaves a volatile ident
  UNPROVEN and UNGUARDED (`vrp_key_root_is_volatile`; no VRP range is recorded for a
  volatile value), so the emitter's `({ tmp = load; check(tmp); access[tmp]; })` form takes
  it — one load, check and access on the temp, TRAP on OOB. An MMIO POINTER index with a
  volatile has no such form and is refused ("cannot index MMIO pointer with volatile").
- **The IR unknown-intrinsic fallback is LOUD** (993). BUG-767 hardened the AST-path
  fallback and left the IR twin on `/* @name */ 0`; it now emits
  `__zer_intrinsic_<name>_has_no_IR_emitter_handler`. Beside it the AST atomic gate's
  `nlen >= 10` off-by-one (excluded `@atomic_or`) is `>= 9`.

### The certain-OOB loop is a compile ERROR (BUG-992) — what licenses it

`for (u32 i = 0; i < 8; i += 1) { arr4[i] = i; }` was a WARNING, and the auto-guard's
runtime form is a silent early `return` of the function's zero, so the canonical off-by-N
overflow was silent at compile time AND at run time (half the loop never ran, `main`
exited 0). VRP's range `[0,7]` is a MAY-hold fact and cannot license an error — `u32 b =
10; if (c) { b = 2; } arr4[b]` also straddles and is fine. The WILL-hold fact comes from
the loop: constant init, positive constant step, constant bound, and a body that cannot
skip an iteration or touch the counter. `Checker.cert_loop_{name,len,lo,step,last,depth}`
carries it for the INNERMOST counted loop; the access must sit at the loop body's own
`branch_depth` and not in short-circuit RHS position. `loop_body_straight_line` is a
PROOF-OF-DANGER walk — no `default:`, answers NO for anything untaught, so an incomplete
walk can only fail to report. The `while`/`do-while` half (`while_counter_step`)
recognises ONLY increment-LAST: with the increment first the values at the access are
`lo+step, …`, and assuming otherwise is wrong in the ACCEPT direction. `IDX_PARTIAL_OOB`
rewords the warning when the range straddles but no certainty holds.

### Raw-AST argument sites, and the one value-flow query (BUG-942/944; `value_flows_to`)

**Five positions emit a call's arguments from RAW AST instead of decomposed IR locals** —
builtin method args, universal `free(...)`, spawn args, asm operands, and the SUBJECT of an
`orelse` (`lower_orelse_to_dest`). The passthrough exists because a builtin's argument may
be a bare TYPE NAME (`arena.alloc(Task)`), which is not an expression. Two consequences,
both measured live: an `orelse` inside such an argument survived into the emitter (false
leak / `_zer_trap` on valid code at four sites, BUG-942 — fixed by `pre_lower_orelse` at
each, which rewrites ONLY `NODE_ORELSE` subtrees so a type-name argument passes through as
a leaf; the synthesized ident must get `checker_set_type` or the emitter's `free(slice)`
arm cannot see it is a slice); and the raw-AST loop applied NONE of the decomposed path's
four coercions (optional wrap, its null form, array->slice, slice->pointer), so `return
f(5) orelse 0;` did not build where `?u32 r = f(5);` did (BUG-944 — fixed by ROUTING an
ordinary call in an orelse subject through `lower_expr`; builtins keep the passthrough via
the extracted predicate `call_bypasses_arg_lowering`, which both sites now ask).

**`value_flows_to(value, vt, target)` (checker.c) is THE value-flow compatibility query**
(BUG-842, then BUG-934's ninth sink). `const_negative_into_unsigned` / `const_int_into_enum`
refusals, then `type_equals` / `can_implicit_coerce` / `is_literal_compatible`. Nine sinks
call it: struct-init field, assignment, call arg, orelse fallback, var-decl init, switch
arm, return, spawn arg, global init. `report_value_flow_refusal` owns the wording. Do not
re-inline the chain at a new sink; BUG-1002 (an `i64` literal at exactly 64 bits accepted
everywhere) was fixed inside `is_literal_compatible` and reached all nine at once.

### Smaller mechanisms, one line each (so they are findable)

- **`(u3)x` / `(i48)x` parse and WRAP** (945/946). `u3` is an IDENT (only `u8..u64` are
  keywords), so the cast disambiguator sent it to `default:`; it now takes the same
  speculate-and-backtrack arm as `(*`, via the one width predicate `zer_is_intn_type_name` in `ast.h` (checker.c is a thin wrapper). The wrap
  is `emit_intn_mask` at `IR_CAST` — var-decl init / call arg / return funnel through it,
  while assignment already masked after the store (the two-spellings split again).
- **`?u32 g = 5;` at global scope** (943): `emit_global_var`'s initializer branch had one
  optional arm — the null literal — and everything else emitted `_zer_opt_u32 g = 5;`
  (invalid C). Payload folds when it can (file-scope rule) else `emit_opt_wrap_value`.
- **`break`/`continue` whose loop is INSIDE the defer/`@critical`/`@once` block are
  legal** (947): `escaping_block_depth` compares the loop depth at the jump against
  `block_entry_loop_depth` of the INNERMOST block, so the VST-verified
  `zer_break_allowed_in_context` is asked a more accurate question, not a different one.
  `emit_defer_stmt` needed the arms (it had none, and trapped).
- **Factory reach through a SWITCH arm / DO-WHILE body** (994): both factory walks
  (`scan_returned_funcname`, `record_isr_returned_funcname`) were if/else chains one kind
  apart, invisible to both walker audits; exhaustive switches now, `FACTORY_NEST_MAX` 64.
- **`g = idfn(@ptrtoint(&local))`** (995): one query `call_result_is_local_address_int`
  at the var-decl propagation, assignment and return sinks, reusing `ret_param_mask`;
  `classify_return_root` peels `@ptrtoint` as a view; an integer `return s.len` is a
  VALUE (the pointer-vs-scalar refinement) so `len_of(local_slice)` stays accepted;
  `ptr_width_int_type` / `arg_is_local_address_int` decide "address-carrying integer".
- **`mmio_const_addr`** (996) — ONE query (= `eval_const_expr_scoped`) at the four
  `@inttoptr` address sites; plain `eval_const_expr` folds literals and not a `const`
  ident, so every named base address was deferred to a runtime trap on the gate side and
  over-rejected on the bound side.
- **`expr_mentions_name` is ONE exhaustive walker** (999); `expr_mentions_global`
  delegates. The two were if-chains with different coverage; neither listed
  NODE_STRUCT_INIT, so `g = pick({ .a = g }).a + 1` was a plain store at both race sinks.
- **`alloc_id` 0 was both "untracked" and a legal id from local 0** (1006):
  `ir_alloc_id_of_local` (id + 1) at every minting site; a double free used to be
  accepted or rejected by PARAMETER POSITION. `IR_STRUCT_INIT_DECOMP` runs the UAF walkers
  and `ir_mark_transferred` (1007). A multi-view call result (`ir_fill_multiview_set`) is
  registered at the ASSIGNMENT sink too and `ir_merge_states` joins the MAY-alias set as
  UNION, not first-predecessor (1008).
- **`a += f` where `a` is int and `f` float** (1005): the compound form emitted a raw C
  conversion (UB out of range) where `a = f` was refused; both directions refused now.

### Two findings from the doc audit itself (2026-09-13)

1. **FIXED the same day as BUG-1015 — an array index in a LOOP CONDITION was range-checked
   under the PRE-loop range.** All three loop drivers called `check_expr(cond)` BEFORE
   `vrp_invalidate_loop_body_writes(body)` (and, for `for`, before `check_expr(step)`), so
   `u32 i = 0; while (arr[i] > 0) { i += n; }` proved `arr[i]` against `i in [0,0]`, marked
   it proven, and emitted `_zer_t1 = arr[i];` with NO bounds check, NO auto-guard and NO
   warning — ASan `global-buffer-overflow`. BUG-748 had fixed exactly this for the BODY.
   **The rule for the VRP loop drivers, now that both halves have been hit: a loop
   CONDITION is evaluated per iteration under the loop-carried range — widen the body's
   (and step's, and `&i`'s) writes BEFORE checking it, and push the cond-derived narrowing
   (which is for the BODY) AFTER.** The one corpus instance was the CVE-2021-3156 demo's
   own parse loop. Test: `tests/zer/loop_cond_index_guard_bug1015.zer`.
2. **The BUG-976 cap enumeration is not closed** — see the cap map above; OPEN in
   limitations.md.

## 2026-04-19 (continued): V3 routing, Task.alloc/free rename, QEMU MMIO test infrastructure

### V3 target-type routing

`route_alloc_to_ptr_if_needed(call, target)` and
`route_free_to_ptr_if_needed(call)` in checker.c. Called BEFORE
the builtin dispatch so existing matchers see the rewritten method
name. Hooks:

- `check_var_decl` init (line ~6428)
- `check_assign` value (line ~2443)
- TYPE_SLAB builtin dispatch entry (line ~3764)
- TYPE_STRUCT builtin dispatch entry (line ~3946)

Receiver-type guards are critical:
- alloc-routing only applies when receiver is a struct-type ident
  (Task sugar) or TYPE_SLAB value (slab).
- Pool (TYPE_POOL) and Arena (TYPE_ARENA) have no `_ptr` variant.
  Initial version wrongly rewrote `arena.alloc(T)` → `arena.alloc_ptr`
  (doesn't exist) until the guards were added.

free-routing lives INSIDE the TYPE_SLAB / TYPE_STRUCT dispatch
branches, so it naturally skips Pool/Arena.

Orelse unwrapping: the helper walks through `NODE_ORELSE` wrappers
to find the underlying NODE_CALL, so `*T p = Task.alloc() orelse
return;` routes correctly.

### Task.alloc / Task.free rename (no alias)

`new`/`delete` removed entirely, not kept as aliases. ZER has no
constructors or destructors; C++ verbs were a misnomer. All four
allocators now use consistent `alloc`/`free` vocabulary:

```
Pool(T, N):  pool.alloc()    pool.free(h)
Slab(T):     slab.alloc()    slab.free(h)    (+ V3 _ptr routing)
Arena:       arena.alloc(T)                  (bulk reset)
Task sugar:  Task.alloc()    Task.free(h)    (+ V3 _ptr routing)
```

Checker/emitter/zercheck all have `new`/`delete`/`new_ptr`/`delete_ptr`
REMOVED from their name matchers. `alloc`/`free`/`alloc_ptr`/`free_ptr`
detection shared across Slab and Task sugar.

Test migration: 15 test files sed'd, 9 renamed. All `tests/zer/task_new*.zer`
→ `task_alloc*`, `tests/zer_fail/task_delete_*` → `task_free_*`,
`rust_tests/rt_task_new_delete*` → `rt_task_alloc_free*`.

### QEMU MMIO test infrastructure

Previously-skipped tests (rt_unsafe_mmio_multi_reg and
rt_unsafe_mmio_volatile_rw) used STM32 addresses (0x40020000)
unmapped on hosted Linux. Now run under QEMU Cortex-M3 with
Stellaris GPIO_E base (0x40024000) which IS mapped on
`qemu-system-arm -machine lm3s6965evb`. Compiler behavior tested
is identical — only the address changed.

Directory `rust_tests/qemu/`:
- Two `.zer` tests adapted to Stellaris addresses
- `startup.c` — Cortex-M3 reset handler with ARM semihosting exit
  (`SYS_EXIT_EXTENDED 0x20` → qemu-system-arm terminates with
  main's return code)
- `link.ld` — memory map for LM3S6965 (256K flash + 64K SRAM)
- `run_tests.sh` — per-test pipeline: `zerc --lib` →
  `arm-none-eabi-gcc -include stdint.h` → QEMU semihosting.
  Gracefully skips if arm-none-eabi-gcc or qemu-system-arm
  aren't installed.

Dockerfile adds `qemu-system-arm` + `gcc-arm-none-eabi`. Makefile
`check` target calls `rust_tests/qemu/run_tests.sh` as a new
section.

Result: ZERO skipped tests across the entire suite. Every test
runs to a real outcome.

---

## 2026-04-19 late: AST→IR emission safety invariant (BUG-595 through BUG-599)

**MANDATORY invariant for future IR handler changes.**

### The class of bug this section prevents

Commit `010ddea` (2026-04-15, "Phase 8b: local-ID emission") replaced
`emit_expr(inst->expr)` routing with direct local-ID emission in several
IR handlers. In doing so, it silently stripped every runtime safety
wrapper `emit_expr` had been applying. Seven regressions shipped
between IR-default-flip (`82335c3`, 2026-04-17) and audit (2026-04-19).

The compiler was:
- Reading past slice length (silent OOB)
- Writing past slice length (silent buffer overflow)
- Computing `se - ss` as size_t with no check (giant fake slice)
- Dividing INT_MIN by -1 (C undefined behavior)
- Shifting by >= width (C undefined behavior)
- Using arbitrary variable addresses as pointers (no MMIO range check)
- Using misaligned addresses for word-size loads (no alignment check)

Tests stayed green because VRP proves most real-world values safe at
compile time, removing the need for the runtime wrapper entirely for
those cases. Audit tests specifically constructed unprovable values.

### The rule

**Any IR handler that emits C by reading local IDs (not by calling
`emit_expr(inst->expr)`) is responsible for emitting every runtime
safety wrapper that `emit_expr` would have emitted for the same
expression.**

Safety wrappers `emit_expr` applies (search emitter.c for these in
the `< 4000` line range — that's the AST path):
- `_zer_bounds_check(idx, len, ...)` — array/slice indexing
- `_zer_trap("division by zero")` — compile-time guard forces this, still
- `_zer_trap("signed division overflow")` — INT_MIN / -1
- `_zer_shl(a, b)` / `_zer_shr(a, b)` macros — shift safety
- `_zer_trap("slice start > end")` — underflow prevention
- `_zer_trap("@inttoptr: address outside mmio range")`
- `_zer_trap("@inttoptr: unaligned address")`
- `_zer_trap("@ptrcast type mismatch")` — checker catches, backup
- `_zer_trap("type mismatch in cast")` — @cast
- `_zer_trap("explicit trap")` — @trap()
- `_zer_probe((uintptr_t)...)` — @probe()

### Where each safety wrapper currently lives (post-fix)

| Safety check | IR emitter site | Fix commit |
|---|---|---|
| Slice bounds READ | `IR_INDEX_READ` handler + `emit_rewritten_node` NODE_INDEX | 3bdcf85 |
| Slice bounds WRITE | `emit_rewritten_node` NODE_INDEX (comma preserves lvalue) | 3bdcf85 |
| Signed div overflow | `IR_BINOP` TOK_SLASH / TOK_PERCENT path | 3bdcf85 |
| Shift safety | `IR_BINOP` TOK_LSHIFT / TOK_RSHIFT path (routes to `_zer_shl`/`_zer_shr`) | 3bdcf85 |
| Slice `arr[a..b]` range | `emit_rewritten_node` NODE_SLICE (both branches) | 3bdcf85 |
| @inttoptr MMIO range | `emit_rewritten_node` @inttoptr intrinsic | 3bdcf85 |
| @inttoptr alignment | Same site as above | 3bdcf85 |
| Array bounds (unproven index) | `emit_auto_guards` separate pass | pre-IR |

### Verification checklist before committing IR emitter changes

1. **Grep the AST path** for every `_zer_trap` / `_zer_bounds_check` /
   `_zer_shl` / `_zer_probe` call site in `emit_expr` (the `< 4000` line
   range is AST; `> 5000` is mostly IR).

2. **For each AST-path safety emit, find the IR equivalent.**
   - If both exist → safe, no work needed.
   - If AST has it, IR doesn't → **regression**. Port the wrapper.
   - If IR has it, AST doesn't → probably OK (IR is primary now).

3. **Write a test that uses an UNPROVABLE value** to exercise the
   runtime check. Example pattern:
   ```zer
   u32 i = 10;
   for (u32 k = 0; k < 1; k += 1) { i = 10; }  // VRP loses track
   return arr[i];                              // runtime check must fire
   ```
   Without the loop, VRP proves `i = 10` statically and the test
   won't exercise the runtime wrapper.

4. **Compile with `--emit-c`** and grep the output for the expected
   safety call. If missing, the test is PASSING FOR THE WRONG REASON.

### Do not repeat this mistake

When optimizing an IR handler, **if the old version had
`emit_expr(inst->expr)` and the new version emits C directly**, stop
and list every safety wrapper `emit_expr` would have applied. Port
each explicitly. Do not assume the optimizer or the AST fallback
will cover it — there is no fallback; `emit_stmt` was deleted in
commit `449627b` and the AST path for function bodies is dead code.

### Audit trail

- Full methodology: CLAUDE.md "AST→IR Emission Diff Audit" section.
- Individual BUGs: BUG-595 through BUG-599 in BUGS-FIXED.md.
- Reproducer tests: `tests/zer_gaps/audit2_slice_*.zer`,
  `tests/zer_gaps/ast_*.zer`. Verify each traps correctly by
  running `./zerc <file> --emit-c -o /tmp/x.c && gcc ... && /tmp/x`
  and checking for expected trap message.

---

## Phase E — Dual-run verification (IN PROGRESS, 2026-04-20)

The final step before Phase F cutover. Phase E wires `zercheck_ir.c`
into `zerc_main.c` alongside `zercheck.c`, runs BOTH on every compile,
and diffs diagnostic counts. Goal: converge to zero disagreements
across the full test suite (~1110 tests). Current state: 108 remaining
disagreements (down from 257 initial).

### Dual-run wrapper location and contract

**File:** `zerc_main.c`, in the zercheck block (~line 492+).

**Activation:** `ZER_DUAL_RUN=1` env var. `ZER_DUAL_RUN=2` enables
verbose "agree" logging for debugging.

**Design:**

```c
zercheck_run(&zc, main_mod->ast);  // zercheck.c — primary, determines exit code
bool ast_ok = zc.error_count == 0;

if (getenv("ZER_DUAL_RUN")) {
    ZerCheck zc_ir;
    zercheck_init(&zc_ir, ...);

    // Iterative FuncSummary build (16 passes max) — mutual recursion convergence
    zc_ir.building_summary = true;
    for (pass = 0..15) {
        for each func: ir_lower + zercheck_ir(zc_ir, func);
        if summaries stable: break;
    }
    zc_ir.building_summary = false;

    // Main analysis pass
    for each func: zercheck_ir(zc_ir, func);

    // Loose parity: both 0 OR both > 0 agree. Only pure mismatches
    // (0 vs >0 or vice versa) logged as disagreement.
    if ((ast==0) != (ir==0)) log disagreement;
}

if (!ast_ok) exit 1;  // still driven by zercheck.c
```

**Loose parity semantics:** Negative tests may factor errors differently
between analyzers (e.g., zercheck.c emits 3 errors for a goto-cycle,
zercheck_ir emits 1). Both catching the bug counts as agreement. Only
full misses (one catches, the other doesn't) are flagged.

### Critical IR lowering fact — IR_POOL_* is dead

Per `ir_lower.c:84`: "IR_POOL_ALLOC etc. — collapsed to IR_ASSIGN in
Phase 8d". The specialized opcodes (`IR_POOL_ALLOC`, `IR_SLAB_ALLOC`,
`IR_POOL_FREE`, `IR_SLAB_FREE`, `IR_POOL_GET`, `IR_SLAB_ALLOC_PTR`,
`IR_SLAB_FREE_PTR`, `IR_ARENA_ALLOC`, `IR_ARENA_ALLOC_SLICE`) exist in
the enum but are NEVER emitted by the lowering pass.

**What's actually emitted:**

| Source | IR shape |
|---|---|
| `pool.alloc()` / `heap.alloc()` / `Task.alloc()` | `IR_CALL` with dest_local set; `inst->expr` = NODE_CALL (method on struct/pool). OR `IR_ASSIGN` with NODE_CALL when part of `?Handle x = alloc()` |
| `pool.alloc() orelse return` | `IR_ASSIGN` with `inst->expr` = NODE_ORELSE (pre-lowered to IDENT form in many cases) |
| `pool.free(h)` | `IR_CALL` with dest_local = -1, `inst->expr` = NODE_CALL |
| `pool.get(h).field` | `IR_ASSIGN` with NODE_CALL inside field access, or direct IR_CALL |
| `arena.alloc(T)` | `IR_ASSIGN`/`IR_CALL` with `ir_classify_method_call` returning `IRMC_ARENA_ALLOC` |

**Method classification** (`ir_classify_method_call(Node*)` in zercheck_ir.c):

Returns `IRMC_ALLOC` / `IRMC_ALLOC_PTR` / `IRMC_GET` / `IRMC_FREE` /
`IRMC_FREE_PTR` / `IRMC_ARENA_ALLOC` / `IRMC_NONE` based on the call's
NODE_FIELD callee field name. arena.alloc distinguished from pool.alloc
by arg count (arena takes a type arg, pool takes none).

**Lesson for fresh sessions:** do NOT add cases to the specialized
`IR_POOL_ALLOC` / `IR_SLAB_*` handlers in zercheck_ir.c expecting them
to fire. Add detection in the `IR_ASSIGN` method-classification block
or the `IR_CALL` method-classification block.

### Leak detection — alloc_id grouping (mirrors zercheck.c:2631+)

Naive per-block leak detection produces false positives on real-world
patterns (early-return from orelse fallback, alias chains where only
the user variable is freed). The correct approach:

1. **Union coverage phase** — walk all return blocks. For each ALIVE
   handle whose state becomes FREED / TRANSFERRED / escaped in SOME
   return block, add its `alloc_id` to a covered set.

2. **Flag phase** — walk all return blocks again. For each ALIVE handle
   whose `alloc_id` is NOT in the covered set, emit a leak error.
   Dedup across blocks via a reported-ids set (one error per alloc_id).

**Why this works:**

- `?Handle mh = alloc(); Handle h = mh orelse return;` — mh's temp
  (%1, `?Handle`) is ALIVE at the fallback-return block, but the
  success path assigns it to `h` via alias chain, and `free(h)`
  marks alloc_id FREED on the success-return block. Coverage union
  → alloc_id covered → no leak error on fallback block.

- Multiple return paths where some free, some escape — any coverage
  counts.

**Filters applied per-handle before flagging:**

- `h->source_color == ZC_COLOR_ARENA` — arena.reset() frees wholesale
- `ir_should_track_move(lt)` — move struct stack values (exit scope)
- `le->kind == TYPE_OPTIONAL` — nullable temps (fallback = null case)
- `loc->is_temp` — compiler-generated intermediates (user alias tracks)
- `h->path_len > 0` — compound entities (non-allocation)

### Phase E mechanism additions by instruction

**IR_ASSIGN:**

```
if expr is NODE_ORELSE and primary is NODE_IDENT:
    alias dst to src's ident (covers `h = mh orelse return` pattern)
    break

if expr unwrapped is NODE_CALL:
    classify method call → IRMC_ALLOC/ARENA_ALLOC/ALLOC_PTR/GET
    if alloc: register dest ALIVE with POOL/ARENA color; break
    if get: UAF check on arg

(fall through to existing NODE_IDENT alias path)
```

**IR_CALL:**

```
if callee is NODE_FIELD(th, "join") + th is ThreadHandle:
    mark th FREED

if inst->expr is NODE_CALL:
    classify method call → IRMC_*
    if alloc: register dest ALIVE; break
    if free: mark handle FREED, propagate aliases; break
    if get: UAF check; break

(fall through to FuncSummary apply + extern alloc/free heuristic)
```

**IR_COPY** (new handler — was missing):

```
if src has tracked handle:
    alias dst inheriting state + alloc_id + source_color + is_thread_handle
    error if src is invalid (UAF through copy)
```

**IR_NOP:**

```
if inst->expr is NODE_SPAWN:
    D5 interrupt/@critical bans
    transfer args (TRANSFERRED state)
    if scoped + handle_name: register ThreadHandle
```

**IR_CRITICAL_BEGIN / IR_CRITICAL_END:**

```
ps->critical_depth++/-- (preserved across ir_ps_copy)
```

### Phase E 2026-04-20 session — 257 → 2 disagreements (~99.2% reduction)

Late-session sweep landed 11 targeted improvements dropping disagreements
from 108 to 8. Each fix below cites the commit that landed it.

**UAF detection (commit `a6ce3ce`):**
- `ir_check_expr_uaf` generic walker for IR_ASSIGN — catches
  `pool.get(h).id = 5` where expr is NODE_ASSIGN wrapping
  NODE_FIELD(NODE_CALL(pool.get, h), id). Recursively visits every
  identifier use inside any expression.
- `IR_RETURN src1_local` path — ir_lower sets src1_local for simple
  `return ident` keeping expr NULL. Previous handler only checked
  inst->expr, missing `free(p); return p;`.
- Interior pointer tracking in IR_ASSIGN — `*T field_ptr = &b.c`
  shares alloc_id with b.
- IR_INDEX_READ handler + IR_CALL UAF walker for general use-sites.
- **IR_UNOP runs the UAF walker too (BUG-765, 2026-06-22).** The three "dangerous
  reads" — `b.f` / `p[i]` / `*p` — lower to IR_FIELD_READ / IR_INDEX_READ / IR_UNOP
  respectively (sub-exprs are decomposed into their own op). ALL THREE must run
  `ir_check_expr_uaf`, or a deref-after-free of an interior pointer slips through.
  IR_UNOP was in the exhaustive no-op group, so `*p` after free was a SILENT UAF
  (the field/index forms were caught; the deref form wasn't) until the strict
  `*T`-index change forced interior tests onto `*p` and exposed it. **Invariant: any
  new IR op that reads through a value must run the UAF walker — don't drop it into
  the no-op group.** Deref-WRITE (`*p = x`) is already covered by the IR_ASSIGN walker.

**Shadow scoping (commit `51c6f7c`):**
- `ir_find_local_exact_first` — zercheck-specific variant that prefers
  exact C-emission name over orig_name. Regular `ir_find_local`
  degrades to "last match" after full-function lowering (all locals
  hidden), which picks the WRONG local for shadow scopes.
- is_temp skip in 4th overwrite check site (IR_CALL alloc path).

**Move struct propagation (commits `2e8d84c`, `cc4a87d`, `cb9cee4`, `5751a1d`):**
- Function call transfer for move structs — `consume(f)` marks f TRANSFERRED.
- `&movestruct` arg handling — conservatively transfers.
- IR_COPY assignment — `Token b = a` transfers ownership correctly.
- Field-write reset in CFG loops — `while { Msg m; m.code = i; send(m); }`
  resets m to ALIVE on field write, avoiding false UAF from merge.
- Param handle auto-register on pool.free(param) + extern free(param) —
  enables FuncSummary.frees_param to observe FREED at return.

**Alias / compound key (commits `646501e`, `ecfa6d6`, `8c6b442`):**
- source_color + is_thread_handle propagation in IR_ASSIGN alias path.
- Escape detection + compound key registration for NODE_ASSIGN
  passthrough (IR_FIELD_WRITE is dead, moved logic to IR_ASSIGN).
- @ptrcast alias tracking — `raw = @ptrcast(*opaque, s)` shares
  alloc_id. NODE_INTRINSIC args[0] is source (type_arg has the type).

**ThreadHandle (commits `0cf2534`, `2eb2baa`, `61e7e48`):**
- ThreadHandle tracked by name (IRThreadTrack) — no IR local exists
  because emitter owns pthread_t declaration.
- Double-join detection — error if `th.join()` called twice.

**Defer (commit `f2200e8`):**
- `rewrite_defer_body_idents` during IR_DEFER_PUSH — defer bodies in
  inner scopes must resolve shadow-suffixed locals correctly.
- used_locals tracking walks inst->expr AST for passthrough usages.

### Additional late-session fixes (5 more disagreements closed, 8 → 5)

- `cdc4bca` — Cross-function FuncSummary chain via param auto-register:
  destroy_cat(*opaque ctx) { c = @ptrcast; free_ptr(c); } wasn't
  registering ctx as a handle, so alias-propagation to free didn't
  observe ctx FREED at return. Same issue in multi-level chains:
  step_b(r) { step_c(r); } — r is a param, step_c's summary says
  frees_param[0] but the apply path skipped when r had no handle.
  Fix: auto-register with escaped=true so aliases don't flag as leaks,
  summary build still observes FREED state. Fixes ownership_chain.zer,
  rt_drop_struct_object.zer.
- `651fbf3` — Removed TYPE_OPTIONAL leak-detection filter: was masking
  genuine leaks on ?Handle/?*T locals. Two replacement mechanisms:
  1. is_orelse_fallback block skip in leak loop (the block is only
     reached when optional was null, no alloc to leak).
  2. Untrackable-target escape mirror (zercheck.c:1460 pattern):
     `handles[i] = mh` where i is variable → mh escaped (can't track
     through dynamic index). Fixes gen_null_002 + rt_handle_array_alloc.

### Additional late-session fixes (3 more disagreements closed, 5 → 2)

- `b7f52aa` — Move struct from array element: `Token copy = arr[0]`
  where Token is a move struct registers compound key `(arr, "[0]")`
  as TRANSFERRED. Subsequent `arr[0].kind` access triggers UAF via
  compound-key lookup in the UAF walker. Only handles literal indices.
- `eedae4f` — Dead-code-after-return state inheritance: when a block
  has no predecessors (unreachable) and the PREVIOUS block in IR
  order ended with RETURN, inherit the previous block's state. This
  matches AST linear-scan that processes all statements in order,
  catching use-after-move on code written after a return.
- `2bf8619` — Treat pointer-returning calls as allocations: any
  function (not just extern) returning *T / ?*T / *opaque / ?*opaque
  registers its dest as ALIVE. Mirrors zercheck.c:786-808 heuristic.
  Skips if dest already tracked. Catches rt_scope_escape_orelse's
  `*Sensor s = fallback_local()` scope-escape leak.

### Final convergence fixes (2 → 0 disagreements, 100% parity)

Two architectural fixes closed the remaining CFG-vs-linear-scan
semantic gap. Both mirror zercheck.c primitives explicitly:

**1. `is_early_exit` block tag** (commit `572c701`, ir_lower.c + ir.h)

CFG equivalent of zercheck.c's `block_always_exits(node)` (line 312).
When an if-then body's last instruction is RETURN or non-join GOTO,
the body always exits — its effects should NOT contribute to the
canonical fall-through state (matches AST's "take not-taken branch"
post-if behavior).

Implementation:
- New `IRBlock.is_early_exit` flag.
- In NODE_IF lowering: after `lower_stmt(then_body)`, check current
  block's terminator. If RETURN or GOTO-to-non-bb_join, tag all
  blocks in `[then_start_bi, current_block]` as early-exit.
- Same for else-body symmetrically.
- Skip if the if has a capture (if-unwrap) — those have alias-escape
  semantics via union coverage.

Zercheck_ir.c:
- Coverage collection skips is_early_exit blocks.
- Leak check skips is_early_exit blocks (not canonical).

Fixes gen_uaf_003: `if (cond) { free(h); return 0; }` now doesn't
count as coverage. Fall-through `return val` with h ALIVE triggers leak.

**SUPERSEDED 2026-09-23c (BUG-1071).** `is_early_exit` is GONE. It made the leak check SKIP the
early return itself, so `if (k == 1) { return 1; } free(a);` leaked on the k==1 path with no
diagnostic. Every reachable return is now checked on its own state and the null path of an optional
is represented by dropping the optional on the branch's false edge — see "ZER-CHECK Semantics" →
"PER-RETURN, NOT UNION".

**2. Exhaustive enum switch elision** (commit `800aaf6`, ir_lower.c)

When a switch on an enum has no default arm, the checker requires
all variants be covered. The last arm's "condition false" path is
structurally unreachable at runtime. Previously CFG lowering emitted
a BRANCH whose false-target went to bb_exit with pre-switch state,
causing CFG merge at bb_exit to get a spurious unreachable predecessor.

Implementation:
- Detect `is_enum && !has_default` at switch entry.
- For the last arm, emit `IR_GOTO bb_arm` (unconditional) instead of
  `IR_BRANCH cond, bb_arm, bb_exit`.
- bb_exit's predecessors become only the arm-body-ends.

Eliminates false-positive MAYBE_FREED when all arms free a handle.

**3. MAYBE_FREED flagging at canonical return** (zercheck_ir.c)

With fixes #1 and #2 eliminating spurious MAYBE_FREED from CFG merge
conservatism, flagging genuine MAYBE_FREED at return now matches
zercheck.c:2700. Fixes goto_maybe_freed_branch: backward-goto loop
with conditional free produces MAYBE_FREED at fixed-point convergence.

**4. Defer alias propagation** (commit `572c701`, zercheck_ir.c)

Correctness fix: `ir_defer_scan_frees` now calls
`ir_propagate_alias_state` after marking the named handle FREED.
Ensures `?Handle mh = ...; Handle s = mh orelse return; defer free(s)`
marks mh FREED at return (via alloc_id alias propagation).

### 100% parity — what it means

All 1110 dual-run tests produce identical diagnostic counts between
zercheck.c and zercheck_ir.c. The CFG rewrite is correct — independently
reimplementing the analysis on a different IR produces matching output
on every real-world test.

Phase F (delete zercheck.c, tag v0.5.0) is unblocked. zercheck_ir.c
is the proper primary — CFG infrastructure is the foundation for
future analyses (dominator trees, VRP-on-SSA, borrow-checker-lite)
that linear-scan zercheck.c can't easily support.

### Convergence criterion for Phase F entry

- Zero disagreements across 1110 tests (full positive + negative sweep)
- 3 successive runs produce identical output (stability)
- test_modules/ cross-module parity verified

All met by end of 2026-04-20 session. Phase F entry achieved
(see "Phase F — Unconditional dual-run" section below).

---

## Phase F — Unconditional dual-run (2026-04-20, LANDED)

zercheck_ir now runs on EVERY compile alongside zercheck.c. Disagreements
logged as regression signals. zercheck.c still drives compile exit code
(conservative preservation of battle-tested behavior) while the CFG
analyzer accumulates real-world validation.

### The architectural constraint that shaped the design

`ir_lower_func` (ir_lower.c:2775) is NOT idempotent. It calls
`pre_lower_orelse` (ir_lower.c:1239) which destructively rewrites
AST: `NODE_ORELSE` nodes are replaced with `NODE_IDENT` referencing
a temp local created during THIS lowering pass.

```
First ir_lower_func call on `maybe_null() orelse helper(maybe_null() orelse 7)`:
  - Creates temp _zer_or1 for outer orelse
  - Creates temp _zer_t2 + _zer_or3 for inner nested orelse
  - Rewrites the AST so inner orelse becomes NODE_IDENT("_zer_t2")
  - Rewrites outer orelse's fallback to reference _zer_t2

Second ir_lower_func call on the SAME AST:
  - AST already has NODE_IDENT("_zer_t2") where inner orelse used to be
  - pre_lower_orelse doesn't find an orelse there (already replaced)
  - Fresh IRFunc creates new locals; _zer_t2 is NOT among them
  - Emitter walks IR expecting _zer_t2 declared, finds nothing
  - GCC error: "_zer_t2 undeclared"
```

This caused `orelse_stress.zer` and `single_eval_guarantees.zer` to
fail with GCC errors when Phase F initially ran IR analysis BEFORE
emit (double-lowering: once for zercheck, once for emit).

### The proper fix: single-lowering via emitter hook

Architecturally correct observation: **the emitter is the sole
authority that calls ir_lower_func**. Any IR-based analysis should
piggyback on the emitter's lowering, not duplicate it.

Implementation:

1. **`Emitter.ir_hook` (emitter.h)** — optional function pointer:
   ```c
   void (*ir_hook)(void *ctx, void *ir_func);
   void *ir_hook_ctx;
   ```
   Called from `emit_func_decl` after `ir_lower_func` + `ir_validate`,
   before `emit_func_from_ir`. Null by default.

2. **Emitter invocation (emitter.c:~3480)**:
   ```c
   IRFunc *ir = ir_lower_func(e->arena, e->checker, node);
   if (!ir) { abort(); }
   ir->module_prefix = e->current_module;
   ir->module_prefix_len = e->current_module_len;
   if (!ir_validate(ir)) { abort(); }
   if (e->ir_hook) {
       e->ir_hook(e->ir_hook_ctx, ir);  /* ← NEW */
   }
   emit_func_from_ir(e, ir);
   ```

3. **zerc_main registers the hook** (zerc_main.c):
   - Static function `zerc_ir_hook` collects IRFunc pointers in a
     static array (`zerc_ir_hook_funcs`).
   - Hook-collecting is all that happens during emit — no analysis
     yet, because single-pass can't build summaries for forward refs.
   - After emit completes, zerc_main runs iterative FuncSummary build
     + main analysis on the collected array (16 passes, convergence
     check). Uses the SAME IRFunc pointers — no re-lowering.

4. **Result comparison**: `zc.error_count` (AST) vs `zc_ir.error_count`
   (IR). Loose parity (both 0 OR both >0 = agree). Disagreements
   logged to stderr with `VERIFY disagreement` prefix.

### Why iterative summary build after emit?

Functions can reference each other (mutual recursion, forward calls).
When `main()` calls `wrap()`, `wrap`'s FuncSummary must exist for IR
to propagate its effects correctly. The hook collects IR in source
order; if `main` is emitted before `wrap`, `wrap`'s summary isn't
built yet during the hook call.

Post-emit iterative build solves this:
```
Pass 1: analyze all funcs in order, build summaries.
Pass 2: analyze again — some call-sites now see summaries they missed.
...
Pass N: summary_count stable, break. (16-pass ceiling.)
```

Then a final `building_summary=false` pass emits actual errors with
all summaries populated.

### Control knobs

```bash
ZER_DUAL_RUN=0 ./zerc file.zer         # disable zercheck_ir
./zerc file.zer                         # default: both analyzers run
ZER_DUAL_RUN=2 ./zerc file.zer          # verbose: log agreements too
```

### Validation surface (2026-04-20)

| Category | Count | Disagreements |
|---|---|---|
| make check (includes fuzzer, module tests, rust_tests, zig_tests) | 3200+ | 0 |
| Standalone dual-run sweep (tests/zer + rust + zig + zer_fail) | 1115 | 0 |
| test_modules/ multi-module tests | 28 | 0 |
| Semantic fuzzer (10 seeds × 200 programs) | 2000 | 0 |
| **Total unique programs validated** | **~3143** | **0** |

### Why zercheck.c stays (for now)

Phase F does NOT delete zercheck.c. Reasons:

1. **Battle-tested conservatism**: zercheck.c has accumulated 500+ bug
   fixes over many sessions. zercheck_ir has 100% parity on tested
   programs but LESS production exposure. Keeping AST primary means
   zercheck.c's decisions continue gating compiles while the IR gets
   real-world validation.

2. **Regression signal**: when IR and AST disagree (future development
   introduces a bug), the `VERIFY disagreement` log surfaces the
   regression immediately. Deleting zercheck.c would remove this
   oracle.

3. **No functional need to delete**: zercheck.c links into the binary
   as a small cost (~2810 lines). Deleting removes the regression
   oracle without gaining meaningful binary-size reduction.

Phase G (future cutover): flip primary to zercheck_ir after a release
cycle with zero reported disagreements. Then zercheck.c can be deleted
safely with no user-visible behavior change.

### Files touched by Phase F

- `emitter.h`: added `ir_hook` + `ir_hook_ctx` fields to `Emitter`.
- `emitter.c:~3480`: invokes `ir_hook` after `ir_lower_func` + `ir_validate`.
- `zerc_main.c`: replaced the dual-run block with hook collection
  (static `zerc_ir_hook`), post-emit iterative build, disagreement report.
- Commit `3d251b5` — "Phase F: unconditional dual-run via emitter ir_hook".

### Key lesson for fresh sessions

**Never call `ir_lower_func` outside the emitter.** If you need IR-based
analysis, register via `Emitter.ir_hook`. The hook pattern ensures
single-lowering and avoids the nested-orelse AST-mutation trap.

If you're seeing `_zer_tN undeclared` GCC errors after adding a new
analysis, it's almost certainly double-lowering. Check that your
analysis doesn't call `ir_lower_func` on a function the emitter will
later lower again.

---

## Research Phase COMPLETE (2026-04-24) — 8 universal precondition categories

**Fresh sessions: READ THIS before implementing strict mode (D-Alpha-7.5 Phase 2).**

### What was produced

Over 7 research sessions (2026-04-24), classified every UB-bearing instruction across x86-64 + ARM64 + RISC-V + PowerPC + Cortex-M spot-checks into **8 universal precondition categories**:

| # | Name | Maps to ZER system | Extension required? |
|---|---|---|---|
| C1 | Value-range | #12 VRP | None |
| C2 | Alignment | #20 Qualifier | Add alignment facts (~30 hrs) |
| C3 | State machine (C9 merged) | Model 1 | Add exclusive-tx state type (~20 hrs) |
| C4 | CPU feature gate | #24 Context Flags | Add `cpu_features` set (~10 hrs) |
| C5 | Privilege / execution mode | #24 Context Flags | Add `privilege_level` hierarchy (~10 hrs) |
| C6 | Memory addressability | #19 MMIO + minor | Canonical-addr check (~10 hrs) |
| C7 | Provenance / aliasing | #3 Provenance + #11 Escape | None — near-empty in base ISAs |
| C8 | Memory ordering | **NEW System #30** | New system (~80 hrs) |
| ~~C9~~ | ~~Exclusive pairing~~ | MERGED into C3 (Session 3) | — |
| ~~C10~~ | ~~Register dependency~~ | DELETED (Session 7) — collapses to O1/O2/E1/I4 | — |

**Key finding:** framework is universal across 5 different ISA design philosophies. Zero instructions observed that don't fit C1-C8.

### Research artifacts (DO NOT delete / reorganize without reason)

- `docs/asm_preconditions_research.md` — master research doc with per-category findings, ISA citations, System #30 design spec
- `research/asm_generics/C1..C8/` — 24 POC `.zer` files (reject + accept per category), NOT in tests/ (POCs, not regression tests)
- `research/asm_generics/README.md` — structure + migration path

**POCs are intentionally outside tests/** because they use `unsafe asm` structured syntax not yet parsed. Running as tests = vacuous pass (parse error). Migration: when strict mode lands, promote POCs to `tests/zer_fail/` and `tests/zer/`.

### Research phase scope boundary

The research produced DESIGN ONLY — zero compiler code was changed during the 7 research sessions. Test suite stayed 442/442 throughout. Commits visible via:

```
git log --oneline --grep="research:" | head -20
```

### What comes NEXT (for implementation sessions)

Implementation NOT started. Order:

1. **Continue D-Alpha intrinsics** (D-Alpha-10 through D-Alpha-14) — ~52 intrinsics remain
2. **D-Alpha-7.5 Phase 1** — Hardened unsafe asm infrastructure (H1-H7 typed operand syntax) — ~120 hrs
3. **D-Alpha-7.5 Phase 2** — Strict mode implementation (18 structural + 13 Z-rules + 8 categories + new System #30 + per-arch data files) — ~430 hrs
4. **Ship v1.0 Tier A** — 100% language-safe

### System #30 (NEW — Atomic Ordering) — design ONLY

**Only new safety system added by Option C. DO NOT implement until D-Alpha-7.5 Phase 2.**

File placement (decided 2026-04-24):
- **Primary:** `zercheck_ir.c` — CFG-based tracking, extends Model 1 state-machine pattern
- **Supporting:** `checker.c` for AST-level barrier/fence recognition
- **IR ops:** mostly exist from D-Alpha-1/2; may add metadata fields
- **Level 3 VST:** `src/safety/ordering_rules.c` + `proofs/vst/verif_ordering_rules.v` (Phase 3 later)
- **NOT in:** `zercheck.c` (being deleted, CFG migration Phase G)

Design spec + pseudocode: `docs/asm_preconditions_research.md` C8 section.

### Per-arch data file pattern (future .zerdata files)

Final design: per-arch data file maps instruction → category + metadata. Generic checker dispatches to existing ZER safety systems based on category. Example entries in research doc.

Per-new-arch cost with this architecture:
- **Level 1 (manual)** — ~20-25 hrs reading ISA manual + writing `.zerdata` rows
- **Level 2 (LLM-assisted)** — ~1-4 hrs feeding ISA manual PDF to LLM with category prompt, human spot-check
- **Level 3 (Sail extraction)** — NOT pursued for v1.0/v1.x (overkill)

---

## D-Alpha batch progress (2026-04-24 end-of-day)

| Batch | Status | Intrinsics | Commit |
|---|---|---|---|
| D-Alpha-1 | DONE | 7 atomics | 2e152f3 |
| D-Alpha-2 | DONE | 10 barriers + bits | a14d3c0 |
| D-Alpha-3 | DONE | 5 interrupt control | 3e6e2f2 |
| D-Alpha-4 | DONE | 4 context switch | e2f45af |
| D-Alpha-5 | DONE | 10 MMU control | 94f958b |
| D-Alpha-6 | DONE | 11 TLB + cache | 6e7521a |
| D-Alpha-7 | DONE | 8 critical multi-core | ca7e144 |
| D-Alpha-8 | DONE | 4 nice-to-have (read_counter, get_pc, wait_on_address, flush_pipeline) | 4ca14b2 |
| D-Alpha-9 | DONE | 10 MSR/CR/XCR0 (x86 privileged) | a0fe701 |
| D-Alpha-10 | DONE | 10 inspection (read_sp/tp/flags, vendor_id, feature_bits, model_id, core_id, current_mode, cache_line_size, num_cores) | 54e2324 |
| D-Alpha-11 | DONE | 5 power management (reset, deep_sleep, idle_hint, monitor_addr, mwait) | aa3d3fd |
| D-Alpha-12 | DONE | 6 privileged transitions (syscall, sysret, iret, set_priv_stack, get_priv_level, hypercall) | 7de2c36 |
| D-Alpha-13 | DONE | 20 Linux-scale x86 (fsbase/gsbase, port I/O, xsave, debug regs, firmware, persistent memory, nt_store, pmc) | 25e52dd |
| D-Alpha-14 | DONE | 12 final (cpuid, eoi, cr2, cache enable/disable, fxsave/fxrstor, fpu_init, umwait/umonitor, endbr) | 8bc0e9a |
| D-Alpha-7.5 Phase 1 | Pending | Hardened unsafe asm (H1-H7) | — |
| D-Alpha-7.5 Phase 2 | Pending | Strict mode implementation | — |

**130 of 130 intrinsics shipped (100% — ALL 14 D-ALPHA BATCHES COMPLETE).**

### D-Alpha-8/9 patterns discovered

**Test pattern (dead-branch for privileged instructions):**
```zer
volatile u32 never_true;

u64 dead_branch_test() {
    u64 val = 0;
    if (never_true == 42) {
        // privileged instruction here — compiles, never runs in user-mode test
        val = @cpu_read_msr(0x1B);
    }
    return val;
}
```

This pattern was established in D-Alpha-3 (interrupt control) and continues to work for D-Alpha-9 (MSR/CR access). Fresh sessions: use this pattern for any privileged intrinsic.

**ZER does NOT support `(void)` cast syntax.** Don't use `(void)unused_var;` to suppress "unused variable" — use meaningful usage instead (e.g., comparison check that's never true).

**Checker batched validation pattern** (for intrinsics with same signature):
```c
} else if ((nlen == 12 && memcmp(name, "cpu_read_cr0", 12) == 0) ||
           (nlen == 12 && memcmp(name, "cpu_read_cr3", 12) == 0) ||
           (nlen == 12 && memcmp(name, "cpu_read_cr4", 12) == 0) ||
           (nlen == 13 && memcmp(name, "cpu_read_xcr0", 13) == 0)) {
    /* batched 0-arg u64-return validation */
    if (node->intrinsic.arg_count != 0) {
        checker_error(c, node->loc.line, "@%.*s takes no arguments", (int)nlen, name);
    }
    result = ty_u64;
}
```

Each mnemonic needs its own `memcmp` + length check — cheaper than generic solution since we have ~10 instructions per batch typically.

**Emitter pattern for x86-specific ops with no cross-arch equivalent:**
```c
} else if (nlen == 12 && memcmp(name, "cpu_read_cr0", 12) == 0) {
    emit(e, "({ uint64_t _zer_cr = 0;\n"
        "#if defined(__x86_64__)\n"
        "    __asm__ __volatile__ (\"movq %%%%cr0, %%0\" : \"=r\"(_zer_cr));\n"
        "#endif\n"
        "_zer_cr; })");
}
```

Non-x86 archs get the 0-initialized value (no emission inside `#if`). This lets cross-arch kernel code use `comptime if` arch gates without needing per-arch conditional at intrinsic call sites.

**Emit() escape rule reminder** (from D-Alpha-4 gotcha, still applies):
- `%0` (GCC operand ref) → write `%%0` in C fmt string (one layer of escape for `emit()`'s vfprintf)
- `%%rbx` (literal register) → write `%%%%rbx` (two layers — vfprintf then asm parser)

### D-Alpha-10/11/12 patterns (2026-04-24 end-of-day)

**`__builtin_thread_pointer()` for portable thread pointer** (D-Alpha-10):

Rather than per-arch inline asm to read FS:0 (x86) / tpidr_el0 (ARM64) / tp (RISC-V), use GCC's portable builtin. Works on GCC 11+ and Clang. Example in `cpu_read_tp`:
```c
emit(e, "((uint64_t)(uintptr_t)__builtin_thread_pointer())");
```
Single line, cross-arch. Much cleaner than 9 lines of per-arch asm.

**Avoid `sysconf` in emitted preamble** (D-Alpha-10 finding):

`sysconf(_SC_LEVEL1_DCACHE_LINESIZE)` requires `<unistd.h>`. Adding headers to ZER's emitted preamble is avoided — causes cross-platform pain on Windows/bare-metal. Instead, emit compile-time stub:
```c
emit(e, "((uint32_t)64)");  // x86/ARM/RISC-V default cache line
```
Users needing runtime values call POSIX APIs directly.

**Length-matching gotcha in batched checker branches** (D-Alpha-11 bug):

When writing batched same-signature intrinsic validation, the `nlen == N` check AND the `memcmp(..., string, N)` length MUST match the actual string length. Example of the bug I hit:
```c
/* BUG: impossible condition (14 && ... == 13) */
(nlen == 14 && memcmp(name, "cpu_idle_hint", 13) == 0 && nlen == 13)
```
Both must be 13 for "cpu_idle_hint" (13 characters). Always double-check with `wc -c`.

**Mode transition instruction mapping** (D-Alpha-12):

For per-arch asm emission of mode transitions, the pattern is consistent:

| Function | x86-64 | ARM64 | RISC-V |
|---|---|---|---|
| User→kernel trap | `syscall` | `svc #0` | `ecall` |
| Kernel→user return | `sysretq` | `eret` | `sret` |
| Interrupt return | `iretq` | `eret` | `mret` |
| Guest→hypervisor | `vmcall` | `hvc #0` | `ecall` |

Note: ARM64 uses `eret` for BOTH sysret and iret (unified return-from-exception). x86 uses separate `sysretq` vs `iretq`. RISC-V differentiates via `sret` (S-mode) vs `mret` (M-mode).

**Pointer-argument validation pattern** (D-Alpha-11 `cpu_monitor_addr`):

For intrinsics taking pointer arguments, validate via `type_unwrap_distinct` + check `TYPE_POINTER` or `TYPE_ARRAY`:
```c
Type *vt = typemap_get(c, node->intrinsic.args[0]);
if (vt) {
    Type *eff = type_unwrap_distinct(vt);
    if (eff->kind != TYPE_POINTER && eff->kind != TYPE_ARRAY) {
        checker_error(c, node->loc.line, "@%.*s argument must be pointer or array", ...);
    }
}
```
Accepting both `*T` and `T[N]` is idiomatic ZER — arrays auto-coerce to pointers at call sites.

**MSR write pattern for mode-transition setup** (D-Alpha-12 `cpu_set_priv_stack`):

Setting kernel stack pointer uses `wrmsr` with MSR_KERNEL_GS_BASE (0xC0000102) on x86 — target for `swapgs; mov %gs:0, %rsp` convention. On ARM64 uses `msr sp_el0`. On RISC-V `csrw mscratch`. All take u64 argument.

**Unused-var suppression** (ZER doesn't support `(void)cast`):

Instead of `(void)unused_var;` (not valid ZER), use a comparison that's always true:
```zer
if (unused == 0 || unused != 0) { return 0; }
```
Ugly but works. Repeatedly used in D-Alpha-8/9/10/12 tests. If you see this pattern, it's just suppressing unused-var warnings.

### D-Alpha-13/14 patterns (2026-04-24 end-of-day, 130/130 milestone)

**BUG: x86 `and` / `or` with 32-bit imm + 64-bit reg sign-extension** (D-Alpha-14 cpu_cache_enable):

Writing `andq $0xBFFFFFFF, %rax` fails to assemble with "operand type mismatch for `and`". x86-64 AND/OR with 64-bit register requires either:
- 64-bit register source
- Sign-extended 32-bit immediate (so 0xBFFFFFFF sign-extends to 0xFFFFFFFFBFFFFFFF)

The sign-extension behavior is usually NOT what you want when clearing or setting a single bit in a wider register.

**Fix: use BTR (bit test+reset) or BTS (bit test+set) for single-bit CR register modifications.** These take `imm8` bit position:
```c
/* CORRECT — clear CR0.CD (bit 30) */
"btrq $30, %%0\n\t"    /* cleanly clears bit 30 regardless of register width */

/* CORRECT — set CR0.CD */
"btsq $30, %%0\n\t"

/* WRONG — sign-extension issue */
"andq $0xBFFFFFFF, %%0\n\t"   /* clears bits 32-63 too! */
"orq  $0x40000000, %%0\n\t"   /* actually OK but BTS is clearer */
```

Applies to any CR / DR / system register bit manipulation. Remember this when touching emitter asm with immediate values.

**Packed u64 return for multi-register results** (D-Alpha-14 cpu_cpuid pair):

CPUID returns 4 registers (EAX/EBX/ECX/EDX) but our intrinsic return types are single values. Solution: provide TWO intrinsics, each returning a u64 with 2 registers packed:
```c
@cpu_cpuid(leaf, subleaf)      → (EBX << 32) | EAX
@cpu_cpuid_ecx(leaf, subleaf)  → (EDX << 32) | ECX
```

User calls both for full 4-register result. Avoids returning structs (which ZER intrinsics can't do cleanly yet). Same pattern works for any multi-register instruction (RDTSC+TSC_AUX, XGETBV EDX:EAX pairs).

**Runtime-switch dispatch for indexed register access** (D-Alpha-13 cpu_read_dr):

Some x86 instructions encode the target register in the opcode (e.g., `movq %dr0, %rax` vs `movq %dr1, %rax`). Can't use runtime variable. Solution: switch in emitted C:
```c
switch (idx) {
case 0: __asm__ __volatile__ ("movq %%dr0, %0" : "=r"(result)); break;
case 1: __asm__ __volatile__ ("movq %%dr1, %0" : "=r"(result)); break;
/* ... */
case 7: __asm__ __volatile__ ("movq %%dr7, %0" : "=r"(result)); break;
default: break;
}
```

GCC compiles compile-time-constant idx to the single right instruction. Runtime idx gets the full switch. Same pattern works for ARM sysreg access if we ever do a generic `cpu_read_sysreg(u32 encoding)`.

**Port I/O "Nd" constraint** (D-Alpha-13 port_in/out family):

GCC's asm constraint `"Nd"` accepts constant 0-255 OR register DX. Used for x86 port number:
```c
__asm__ __volatile__ ("inb %1, %0" : "=a"(val) : "Nd"(port));
```

Compile-time port: encoded directly in instruction (`inb $0x60, %al`).
Runtime port: placed in DX register automatically (`inb %dx, %al`).

Pattern applies to any x86 instruction with imm8-or-DX operand (outb, inb, outw, inw, outl, inl).

**Final intrinsic count: 130 across all 14 batches.**

Per-batch shipping cost was consistently ~1-3 hours each for 4-20 intrinsics. Total checker.c additions: ~500 lines. Total emitter.c additions: ~2000 lines. All using the proven per-arch `#if defined` dispatch pattern established in D-Alpha-1.

---

# Stage 1 of `docs/4-27-2026-gaps.md` Roadmap — COMPLETE 2026-04-27

Detailed implementation notes for the 12 silent-gap fixes landed as
Stage 1. Each section: WHAT was wrong, WHERE the fix is, HOW the
pattern can be reused, WHY the design choice was made.

## Part B — Mechanical -Wswitch enforcement (IN PROGRESS)

`tools/walker_default_audit.sh` — initial baseline 42 sites, currently
34 remaining (8 closed). All -Wswitch warnings resolved (8 → 0).

### Approach

For each walker that has `switch (...->kind)` / `switch (...->op)` with
a `default:` clause, replace `default: break;` with an explicit list of
every NODE_KIND / TYNODE_KIND / IR_OP_KIND. GCC's -Wswitch then errors
on any new kind added to the enum without updating the walker.

**Pattern for explicit-no-op cases:**
```c
case NODE_BLOCK:
    /* recurse into children */
    break;
case NODE_IF:
    /* recurse into branches */
    break;
/* ... explicit case for every NODE_ kind ... */
case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    /* leaf — explicit no-op */
    break;
```

The full enum list is in `ast.h`. When adding a new NODE_/TYNODE_/IR_OP_,
build will fail with -Wswitch errors at every walker; each error site
needs a deliberate handling decision.

### Walkers converted in Stage 2 Part B

- `zercheck.c:defer_scan_all_frees`
- `zercheck_ir.c:ir_check_expr_uaf`
- `zercheck_ir.c:ir_defer_scan_frees`
- `zercheck_ir.c:used-locals AST walker`
- `ir_lower.c:rewrite_idents`
- `ir_lower.c:rewrite_defer_body_idents`
- `ir_lower.c:rewrite_capture_name`
- `ir_lower.c:find_shared_root_in_stmt_ir`
- `ast.c:node_kind_name`
- `ast.c:print_type` (TYNODE_KIND)
- `ast.c:ast_print`
- `emitter.c:emit_top_level_decl` (added NODE_DO_WHILE)

### Walkers remaining (34 sites)

Many are TYPE_KIND switches (intentional defaults for non-builtin types),
IR_OP_KIND switches (forward-compat for new IR ops), or intrinsic-name
string dispatches (different from NODE_KIND). Review each case-by-case:
- SAFE: explicit reject decision for any future kind not in the targeted
  set (e.g., `ir_receiver_is_builtin_target` rejects all non-Pool/Slab/
  Ring/Arena/Struct types — adding a new TYPE_KIND should require manual
  consideration).
- UNSAFE: silent skip of new NODE_KIND. Convert to enumerated cases.
- NEEDS REWRITE: switch on a string (e.g., intrinsic name) — different
  problem, doesn't benefit from -Wswitch.

Continuation strategy for fresh sessions:
1. Run `bash tools/walker_default_audit.sh` to see current punch list
2. Pick the next site, view it with the surrounding switch
3. Classify (SAFE/UNSAFE/NEEDS_REWRITE) and either convert or annotate
4. Run `make check` after each conversion (regression-test)
5. Update this doc + audit script's INITIAL_COUNT if rebaselining

## Stage 3 — Fixed-buffer cleanup + CI linter (2026-04-28)

Stage 3 closed Gap 35 (HIGH severity, VERIFIED) — `keep_checks[8]` /
`ld_nodes[8]` silently truncated 8-deep orelse chains, bypassing
keep-parameter local-pointer escape check. Plus 3 Tier-4 buffer sites
converted, plus a new CI gate.

### Conversions

| Site | File | Before | After |
|---|---|---|---|
| `keep_checks[8]` | checker.c orelse-chain walker (param check) | Fixed `Node *keep_checks[8]` | Stack-first 8 + arena overflow doubling (parser RF9 pattern) |
| `ld_nodes[8]` | checker.c orelse-chain walker (local-derived check) | Fixed `Node *ld_nodes[8]` | Same as above |
| `build_expr_key stack_buf[512]` | checker.c (compound key path) | Fixed 512-byte buffer, silent truncation | **Two-pass measure-then-fill**: walk once to compute exact length, allocate exactly, fill. No buffer, no retries. |
| `ir_extract_compound_key stack_buf[256]` | zercheck_ir.c | Same as above for IR-side keys | Same two-pass approach (with `ir_measure_key_path` helper) |
| `Node *stack[64]` AST walker | zercheck_ir.c (used-locals tracking) | Fixed 64-slot stack | Stack-first 64 + `ZER_STK_RESERVE(N)` macro that doubles into arena on overflow |

### `tools/audit_fixed_buffers.sh` (NEW)

CI gate that flags new fixed-size buffers in compiler internals.
Compares current declarations against `tools/fixed_buffer_baseline.txt`
(18 known-accepted patterns).

**Critical fix (2026-04-29)**: originally matched on `file:line:content`,
which broke on EVERY edit that shifted lines. Re-architected to match
on `file:content` (line-number-agnostic). Survives any source edit
that doesn't change the buffer declaration itself.

If you add a new fixed buffer:
- Either convert to stack-first dynamic (parser RF9 pattern)
- Or add `file:content` line + justification comment to baseline

The audit runs in `make check`. Fails CI on any new pattern.

### Stage 3 test additions

- `tests/zer_fail/stage3_keep_orelse_chain_n8.zer` — 8-deep orelse
  chain with terminal `&local` must reject (was silently accepted)

Total ZER tests after Stage 3: 513 (up from 512).

---

## F4.1 — Instruction-table pipeline (2026-04-29)

Closes the F1a stub: `zer_asm_category()` was returning 0 for everything
since F1a landed. F4.1 wires real lookup against vendored per-arch
instruction tables generated from `arch_data/<arch>.zerdata` files.

### Pipeline

```
arch_data/x86_64.zerdata        ← MANUAL classification by ZER team
       ↓
scripts/gen_instruction_table.sh  ← bash/awk parser+generator
       ↓
src/safety/asm_instruction_table_x86_64.c  ← VENDORED (committed to git)
       ↓
linked into zerc binary at build time
       ↓
zer_asm_instruction_info(arch, mnemonic, &out_info)
       ↓
checker.c NODE_ASM dispatch on category bitmap
       ↓
Per-category enforcement (today: C4 wired; C1/C2/C3/C5/C8 informational)
```

### Files

- `arch_data/SCHEMA.md` — schema spec (~270 lines)
- `arch_data/x86_64.zerdata` — manually classified instructions
- `scripts/gen_instruction_table.sh` — parser+generator (~210 lines)
- `src/safety/asm_instruction_table.h` — `ZerInstructionEntry` struct
- `src/safety/asm_instruction_table_x86_64.c` — AUTO-GENERATED, vendored
- `src/safety/asm_categories.c` — real lookup (was F1a stub)
- `checker.c` NODE_ASM — F4 dispatch loop with mnemonic parser
- `Makefile` CORE_SRCS / LIB_SRCS — link new vendored .c

### Schema (`arch_data/SCHEMA.md` summary)

Each instruction: INI-style section. Required fields: `category`,
`source`. Optional: `operand_count`, `operand[N].type`,
`operand[N].constraint`, `required_features`, `consequence`, `notes`.

Categories: bitmap (C1-C8), combined with `+` (e.g.,
`category = C2_ALIGNMENT + C5_PRIVILEGE`).

Constraints: `NONZERO`, `ALIGNED(N)`, `BOUNDED(min, max)`,
`COMPOUND(c1, c2, ...)`.

### F4 dispatch behavior

`checker.c` NODE_ASM handler walks the instructions string token-by-token,
extracts mnemonics (alphanumeric + underscore), looks up in
`zer_asm_instruction_info()`, fires per-category check:

- **C4 (CPU feature)** — checks `c->target_features` bitmap against
  `info.feature_bits`. If feature not enabled: error citing Intel SDM +
  consequence.
- **C5 (privilege)** — informational; naked-only restriction (S1) is the
  v1.0 guard. F7-full will add explicit kernel-context modeling.
- **C1/C2/C3/C8** — classification recorded; enforcement deferred to
  F7-full / Stage 5 (operand-binding to existing safety systems).

### F4.1 test additions

- `tests/zer/asm_f4_avx512_with_flag.zer` — VPXORQ + `--target-features=avx512f` compiles
- `tests/zer/asm_f4_bsr_works.zer` — BSR known instruction passes pipeline
- `tests/zer/asm_f4_unknown_insn_passes.zer` — ADD not in table, graceful fall-through
- `tests/zer_fail/asm_f4_avx512_no_flag.zer` — VPXORQ without flag rejected with vendor citation

Sample F4 dispatch error (real output, **production-grade**):

```
asm instruction 'vpxorq' requires CPU feature not enabled in
--target-features (Intel SDM Vol 2C VPXORQ) — consequence:
#UD fault if AVX-512F not enabled at runtime
```

---

## F4.2 — Mass classification (51 entries) + 14 CPU feature flags (2026-04-29)

Expanded `arch_data/x86_64.zerdata` from 13 (gold set) to 51
safety-relevant entries covering all 6 wired categories
(C1, C2, C3, C4, C5, C8).

### Classified instructions (51 total)

- C1 (value range, 4): bsr, bsf, div, idiv
- C2 (alignment-required SIMD, 11): movaps, movapd, movdqa, movntps, movntpd, movntdq, vmovaps, vmovapd, vmovdqa, vmovdqa32, vmovdqa64
- C3 + C5 (state machine + privilege, 2): monitor, mwait
- C4 (CPU feature samples, 10): vpxorq, vpaddq, aesenc, aesdec, aeskeygenassist, sha1rnds4, sha256msg1, tzcnt, lzcnt, popcnt
- C5 (privileged kernel-only, 20): wrmsr, rdmsr, invlpg, invd, wbinvd, invpcid, hlt, swapgs, iretq, sysret, sysretq, lidt, lgdt, lldt, ltr, wrpkru, rdpkru, xsetbv, clac, stac
- C8 (memory ordering, 4): mfence, lfence, sfence, pause (classification only; enforcement is Stage 5 / System #30)

### CPU feature flags wired (`ZerCpuFeature` enum)

Was 1 (AVX512F only). Now 14:

```c
ZER_FEAT_AVX512F  = 1u << 0,
ZER_FEAT_SSE      = 1u << 1,
ZER_FEAT_SSE2     = 1u << 2,
ZER_FEAT_AVX      = 1u << 3,
ZER_FEAT_AVX2     = 1u << 4,
ZER_FEAT_AES      = 1u << 5,
ZER_FEAT_SHA      = 1u << 6,
ZER_FEAT_BMI1     = 1u << 7,
ZER_FEAT_BMI2     = 1u << 8,
ZER_FEAT_LZCNT    = 1u << 9,
ZER_FEAT_POPCNT   = 1u << 10,
ZER_FEAT_INVPCID  = 1u << 11,
ZER_FEAT_PKU      = 1u << 12,
ZER_FEAT_XSAVE    = 1u << 13,
ZER_FEAT_SMAP     = 1u << 14,
```

**Bit values are stable** — once assigned, never renumber. Vendored
.c tables encode them. `scripts/gen_instruction_table.sh::feat_to_int()`
maps feature names → bit values; both must match the enum exactly.

### CLI parser extended (`zerc_main.c`)

`--target-features=` now accepts comma-separated list (`strstr`-based
matching). Each match sets the corresponding bitmap flag AND appends
`-m<feature>` to GCC at emit time.

```bash
zerc --target-features=avx512f,aes,sha,bmi1 main.zer
```

**Baseline default (F4.2)**: x86_64 target defaults to `SSE | SSE2`.
Both are guaranteed by the x86_64 ABI. Without this default, MFENCE
(which requires SSE2 per Intel SDM) would falsely fail the C4 feature
gate. Non-x86 targets reset baseline to 0 in the `--target-arch=`
handler.

### F4.2 test additions

- `tests/zer/asm_f4_aes_with_flag.zer` — AESENC + `--target-features=aes`
- `tests/zer/asm_f4_mfence_compiles.zer` — MFENCE C8 classified, baseline SSE2 satisfies the C4 gate
- `tests/zer/asm_f4_privileged_compiles.zer` — HLT C5 classified, naked-only restriction is the v1.0 guard
- `tests/zer_fail/asm_f4_aes_no_flag.zer` — AESENC rejected without flag, cites Intel SDM Vol 2A

Total ZER tests after F4.2: 525 (up from 521).

### What's NOT yet wired (F7-full / Stage 5 work)

- C1 value-range constraint operand-binding (operand→VRP dispatch)
- C2 alignment constraint operand-binding (operand→qualifier system)
- C3 state-machine LL/SC pairing tracking
- C8 memory-ordering enforcement (System #30 / Session G)

These categories CLASSIFY today (the data is correct and recorded);
they ENFORCE in F7-full and Stage 5.

### Critical "DO NOT do this" warnings for fresh sessions

1. **`.zerdata` is NOT user-extensible at runtime.** It's vendored at
   zerc build time. Adding new instructions = D-Alpha PR batch by ZER
   team, not user-loaded files. The user-extensible runtime registry
   was designed in detail and **rejected** as overengineering — see
   `docs/asm_plan.md` for the rejection rationale.

2. **Don't add a register name to the clobber table to support
   pre-GCC extensions.** That would lie to users (ZER says valid,
   GCC rejects). Instead, **add a compiler-team intrinsic that emits
   raw `.byte` sequences with `"memory"` clobber**. See `docs/asm_plan.md`
   "Adding clobber-gated extensions before GCC catches up" for the
   recipe (~5 hrs per intrinsic).

3. **Don't bump fixed-buffer constants when the audit complains.** Use
   stack-first dynamic with arena overflow (parser RF9 pattern) OR
   use two-pass measure-then-fill (no buffer at all). Bumping constants
   is the wrong fix.

4. **Don't pass `-mavx512f` to non-x86 GCC.** The `--target-arch=`
   handler resets `target_features = 0` when arch is aarch64/riscv64
   to prevent x86 feature flags from being passed to ARM/RISC-V GCC.

5. **Don't build the multi-backend (GCC + Clang) probe pipeline.** It
   was considered (~14 hrs effort) for marginal coverage gain. Defer
   until concrete need; single-backend GCC probe is sufficient for v1.0.

### Final session tally (2026-04-29)

- 525/525 ZER tests pass (was 505 at session start, +20 across all stages)
- 28/28 module tests
- 784/784 Rust translations
- 36/36 Zig translations
- 139/139 conversion tests
- 3/3 cross-arch end-to-end
- All 4 audit gates green: walker IR parity, walker default, fixed-buffer, emit
- Cumulative gaps closed: 27 of 47 (was 19 at session start)

---

## Session 2026-05-02 — F6 + F7-light + 4 limitation closures + branch audit

This session continued the asm safety roadmap and closed 5 of 6 OPEN
limitations. Two layered things happened:

### Branch audit verification (claude/cool-johnson-apebs)

The do-not-merge branch ran a parallel-agent codebase audit and
surfaced 2 real bugs + 1 misframed claim. Per workflow the branch was
not pulled — instead each finding was independently verified against
main, and fixes applied directly here.

**BUG-650** (zercheck_ir.c ir_merge_states bare-only lookups for
compound handles):
- Verified by reading the lookup function definitions: `ir_find_handle`
  at line 184 explicitly filters `path_len == 0` (bare-only),
  `ir_find_compound_handle` at line 194 matches on (local_id, path,
  path_len). Two sources of truth.
- Verified the merge function used the bare-only variant in two
  places (state-merging loop + add-if-missing dedup loop). Compound
  handles (`b.h` registered via `ir_add_compound_handle`) were
  silently skipped in merge.
- Today the bug is masked by AST zercheck.c (legacy) running in
  parallel and catching the case via different logic. After Phase G
  deletes zercheck.c, this becomes a real silent UAF.
- Fix: both call sites now use `ir_find_compound_handle`. Second
  loop uses `ir_alloc_handle_slot` directly (bypassing bare-only
  `ir_add_handle`). Plus 2 new merge cases for MAYBE_FREED↔FREED.

**BUG-651** (section attr lost on IR-path functions):
- Verified by reading `emit_func_decl` and `emit_regular_func_from_ir`.
  The attribute emission at emitter.c:3689-3699 came AFTER the early
  return at line 3681 to the IR path. So functions with bodies took
  the IR path before reaching attribute emission.
- Branch fix duplicated the inline attribute code into the IR path.
  REJECTED that approach. Did helper extraction instead:
  `emit_func_attributes(e, fn)` called from BOTH paths. Single
  source of truth — future attribute additions land in ONE place.
- `naked` deliberately disabled in helper. See docs/limitations.md.

**BUG-650 reproducer**: tests/zer_fail/compound_field_maybe_freed.zer

### Architectural pattern: bare vs compound lookup trap

The compound/bare lookup split in zercheck_ir.c is a structural
fragility waiting to be re-tripped. There are 13 call sites of
`ir_find_handle` (bare-only). Any of them in a state-merging or
dedup context that needs to handle compound entries would be
similarly broken.

**Mitigation pattern (NOT YET IMPLEMENTED):**
- Rename `ir_find_handle` → `ir_find_bare_handle` to make
  bare-only intent explicit at every call site
- Audit all 13 sites for compound-awareness need
- ~3-5 hrs separate refactor work (Stage 6/7 territory)

The branch's BUG-650 fix patches the immediate site but doesn't
prevent the trap from re-emerging elsewhere. Future audits should
look for similar find-then-merge patterns.

### Fixes #1-4 (closed 4 of 6 OPEN limitations)

#### Fix #1 (BUG-652 layered): u64 atomic warning + Checker.target_ptr_bits init

Surface symptom: u64 `@atomic_*` emitted libatomic warning on 64-bit
hosts. Initial fix added `c->target_ptr_bits < 64` gate. Test
verified warning still fired.

Deeper bug found during verification: `c->target_ptr_bits` was
NEVER initialized — `memset`-zeroed in `checker_init`, never
synced from the global `zer_target_ptr_bits`. Any check
`c->target_ptr_bits < N` silently always-true regardless of CLI
flag. Fix in `checker_init`:
```c
c->target_ptr_bits = zer_target_ptr_bits;
```
After both layers, the gate works correctly.

**Lesson for fresh sessions**: when adding a Checker field, ALSO add
its initialization in `checker_init` from the appropriate source.
The struct is memset-zeroed; fields without explicit init silently
have value 0.

#### Fix #2: @once `__STDC_HOSTED__` guard

`@once` emitted `__atomic_exchange_n` unconditionally. On
freestanding without libatomic linkage, link fail or non-atomic
fallback (racy across cores).

Fix at emitter.c:8327 IR_BRANCH NODE_ONCE handler — wrap atomic
emission in `#if __STDC_HOSTED__`. Else-branch: non-atomic flag
check (single-core safe; multi-core bare-metal users provide own
synchronization).

The legacy AST emit_stmt path for NODE_ONCE was already deleted, so
only the IR path needed the fix.

#### Fix #3: AST emit_expr compound /=/%= INT_MIN/-1 trap

Defense in depth. The IR path's emit_rewritten_node had the full
guard set (divzero + INT_MIN/-1 signed overflow); the AST sibling
only had divzero. Function bodies are IR-only since 2026-04-19, but
other emission contexts (statement-expressions, top-level
initializers, comptime emission) still go through emit_expr.

Ported the IR pattern to AST. Same generated C structure: divzero
trap → if signed AND divisor==-1 then INT_MIN trap → execute. Per-
width INT_MIN literal selection (i8/-128, i16/-32768, i32/(-INT_MAX-1),
i64/(-INT_MAX-1LL)).

#### Fix #4: `@probe` --probe-mode= flag (Option C)

`@probe(addr)` had a single hardcoded behavior: hosted used signal
handler + setjmp recovery, freestanding did direct read with garbage
on fault. Silent gap on bare-metal — programs relying on null-on-
fault got apparent success with garbage instead.

Three modes via CLI flag:
- `--probe-mode=hosted` (default): current behavior, `#if __STDC_HOSTED__` dispatches
- `--probe-mode=raw`: direct read only, no signal handler emitted (embedded path)
- `--probe-mode=disabled`: compile error on any @probe usage (safety-critical)

Plumbing: zerc_main.c parses the flag, Checker has `probe_mode` field
(checked at NODE_INTRINSIC for "probe"), Emitter has `probe_mode`
field (routes emission).

Test runner enhancement: `tests/test_zer.sh` extended `// zerc-flags:`
directive parsing to the negative branch (was positive-only).
Required for `probe_disabled_mode.zer` where the directive supplies
the flag that triggers the rejection.

### Fix #5 INVESTIGATION RESULT — `@critical` indirect return is NOT a bug

The branch's audit claimed:
```zer
void unlock() { return; }
@critical { unlock(); }   // claimed: interrupts NOT re-enabled
```

Empirically verified the claim against emitted C:
- Entry: `cpsid i` (ARM) / `cli` (AVR) / `csrrci` (RISC-V)
- Body: `unlock()` called via standard C call/ret — returns to
  @critical body
- Exit: `msr primask, _zer_primask` etc. — restores state

A normal function call returns to its caller. The caller IS
`@critical { ... }`. Execution returns to @critical body, hits
closing brace, interrupts re-enabled. **No escape.**

A `can_escape` predicate as proposed would reject EVERY function
call from @critical (every non-trivial function returns) — over-
restriction masquerading as safety.

I started implementing it before realizing the claim was wrong.
Reverted: types.h field, scan_func_props NODE_RETURN handler,
transitive propagation. Documented in docs/limitations.md so future
sessions don't re-implement.

**Lesson for fresh sessions:** when an audit claim seems too broad,
verify with empirical test BEFORE implementing. Branch claims are
67% accurate (2 real bugs + 1 misframed) — not a free pass.

### F6: riscv64 instruction-level safety classification

Same recipe as F4/F5 — `arch_data/riscv64.zerdata` (30 entries) +
generator + dispatch + tests. RISC-V semantic specifics captured:

- DIV/DIVU/REM/REMU don't trap on zero (return -1/dividend) — no C1
  entries needed
- LR/SC pairs are explicit C2+C3 (alignment + state machine)
- AMO* require natural alignment + A extension (C2+C4)
- Privileged: MRET, SRET, WFI, ECALL, EBREAK, SFENCE.VMA, CSRRW/RS/RC (C5)
- Memory ordering: FENCE, FENCE.I, FENCE.TSO (C8)

#### BUG-653: gen_instruction_table.sh dot-mnemonic regex

Section header regex was `/^\[[a-zA-Z][a-zA-Z0-9_]*\]/`. RISC-V
mnemonics like `[lr.w]`, `[fence.i]`, `[amoadd.w]`, `[c.add]`
were silently skipped. Initial F6 generation produced 11 of 30
expected entries.

Fix: extend to `[a-zA-Z0-9_.]` (allow dots in section headers).

#### BUG-654: checker NODE_ASM mnemonic parser stopped at dots

After F6 generation, the F4 dispatch couldn't find any RISC-V
dot-mnemonic in the table. Parser at checker.c:9989-9993 only
matched `[_a-zA-Z0-9]`. `fence.i` was tokenized as `fence`.

Fix: extend parser to include dot, with defensive trailing-dot trim
to avoid matching periods in pseudo-comments.

### F7-light: C3 state machine — LR/SC pairing enforcement

First piece of F7 enforcement landing. Today F4 dispatch only fired
for C4 (CPU feature). F7-light adds C3 LL/SC pairing.

**Implementation**:
- Hardcoded LL/SC mnemonic classification per arch via macros
  (`ZER_ASM_IS_LL`, `ZER_ASM_IS_SC`):
  - x86_64: monitor (LL) ↔ mwait (SC)
  - aarch64: ldxr/ldaxr (LL) ↔ stxr/stlxr (SC)
  - riscv64: lr.w/lr.d (LL) ↔ sc.w/sc.d (SC)
- Per-asm-block `ll_pending` flag tracked through mnemonic-iteration loop
- End-of-block check fires if `ll_pending` still true
- Errors cite the offending instruction + the consequence from the
  instruction table when available

**Why hardcoded macros vs schema sub-categories**: the LL/SC
partition is small and stable per ISA. Encoding sub-categories in
`.zerdata` would add schema/generator complexity for marginal benefit.
The macros sit at the dispatch site where readers understand the
context.

**Catches**:
- SC without LL → SC's success/failure is undefined per ISA spec
- LL without matching SC → reservation leak; subsequent SC may fail/succeed unexpectedly
- Nested LL → most archs only support one outstanding reservation

### What F7-light covers vs the full F7 plan

| Category | Status after F7-light |
|---|---|
| C1 (value range) | Classified, NOT enforced — needs schema extension + operand-binding (~30 hrs) |
| C2 (alignment) | Classified, NOT enforced — same |
| C3 (state machine) | **ENFORCED via F7-light** for LR/SC pairing across all 3 archs |
| C4 (CPU feature) | Enforced since F4.1 |
| C5 (privilege) | Covered by S1 naked-only restriction (v1.0 stand-in) |
| C6 (memory addr) | Covered by existing @inttoptr range check |
| C8 (memory order) | Stage 5 / System #30 territory (~80 hrs) |

**For C1/C2 enforcement**: the .zerdata schema HAS `operand[N].constraint`
fields, but the generator currently does NOT emit them in the
vendored .c. Extending requires:
1. Schema parser extension (currently strips constraints)
2. Generator emits constraint table per instruction
3. ZerInstructionEntry struct extends with operand_constraints array
4. Dispatch extracts asm-block operand bindings
5. Wire to existing safety systems (VRP for C1, qualifier for C2)

That's substantially more scope than F7-light. Future session.

### Don't re-implement these (fresh-session warnings)

1. `@critical { f(); }` is NOT a transitive escape — DON'T add a
   `can_escape` FuncSummary predicate. Investigation result locked in.

2. RISC-V `DIV`/`DIVU` don't trap on zero — DON'T add C1 entries
   for them. Spec-correct behavior is "return -1/dividend".

3. RISC-V mnemonic dots are LOAD-BEARING — `scripts/gen_instruction_table.sh`
   AND `checker.c` NODE_ASM parser must accept them. Don't drop.

4. `Checker.target_ptr_bits` MUST be initialized from
   `zer_target_ptr_bits` global at `checker_init`. Field is
   memset-zeroed otherwise.

5. F4 dispatch's per-arch handling for `Checker.target_arch` keys on
   the integer ID (1=x86_64, 2=aarch64, 3=riscv64). The non-x86 archs
   reset `target_features` to 0 in zerc_main.c `--target-arch=`
   handler — don't pass x86 feature flags to ARM/RISC-V GCC.

6. `emit_func_attributes` helper is the single source of truth for
   function-level GCC attributes (section, static, naked-deferred).
   Both AST proto path AND IR body path call it. DON'T duplicate
   inline emission anywhere.

### Final session tally (2026-05-02 extended)

- 533/533 ZER tests pass (was 525 at session start, +8 across all fixes)
- 28/28 module tests
- 784/784 Rust translations
- 36/36 Zig translations
- 139/139 conversion tests
- 5/5 cross-arch end-to-end (was 3, +2 for aarch64 dmb + riscv64 fence.i)
- All 4 audit gates green
- OPEN limitations: 6 → 1 (just `naked` deferred)
- Stage 4: F4-F6 instruction tables complete (3-arch parity); F7-light
  C3 LL/SC enforcement live; F7-full C1/C2 deferred (~30 hrs); Stage 5
  / System #30 pending (~80 hrs); naked migration deferred (~20 hrs
  after S1 relaxation)

---

## Stage 4 final status (moved from CLAUDE.md 2026-06-10)

Consolidated sub-stage status table + dispatch list as of the Option E
pivot. The per-session detail is in the D-Alpha-7.5 Session A..F sections
above; this is the summary view CLAUDE.md used to carry.


**PIVOT NOTICE:** The "make compiler smart about specific instructions"
direction (Session G Phase 5, Z9/Z10/Z13, more per-instruction DB
growth) is **DEFERRED indefinitely**. New direction: annotation-driven
asm safety per `docs/asm_lang_zer_safe.md` (the "smart language,
generic compiler" approach).

**What stays from asm_plan (already shipped):**
- 130 intrinsics, Z1-Z8/Z11/Z12, F2/F4/F5/F6 register tables, F7-light
  LR/SC, F7-full Step 2 constraints, C8 ordering metadata, Session A/B
  syntax, naked-only restriction.

**What's new (~3-4 months work in v1.x):**
- Operand metadata, precondition annotations, ~100-entry implicit
  table, state-machine annotation generalization. Reuses existing
  Z-rules and safety infrastructure. Builds on Session A/B syntax.

**Result:** Same safety as full asm_plan with ~3x less work and zero
ongoing maintenance. Per-instruction DB frozen at ~120 entries (no
growth per ISA extension).

**For fresh sessions:** Read `docs/asm_lang_zer_safe.md` end-to-end
before touching asm safety. The historical context below is preserved
but the direction has changed.

**As of 2026-05-02 (historical pre-pivot status):**

| Sub-stage | Status | What was done |
|---|---|---|
| Sub-extension architecture | VALIDATED 2026-04-29 | 3-arch end-to-end empirical proof (x86_64 + aarch64 + riscv64). All 4 design alternatives evaluated and rejected. |
| F4.1 + F4.2 (x86_64 instructions) | DONE 2026-04-29 | 51 entries across C1-C5 + C8. 14 CPU feature flags wired. Pipeline + dispatch + tests. |
| F5 (aarch64 instructions) | DONE 2026-04-29 | 31 entries across C3-C5 + C8. ARM-specific semantics captured. |
| F6 (riscv64 instructions) | DONE 2026-05-02 | 30 entries across C2-C5 + C8. RISC-V LR/SC, AMO, FENCE families. **3-arch instruction-table parity COMPLETE.** |
| F7-light C3 (LR/SC pairing) | DONE 2026-05-02 | Per-block state machine catches SC-without-LR + LR-without-matching-SC across all 3 archs. Real UB caught. |
| F7-full Step 1 (constraint plumbing) | DONE 2026-05-02 | Schema/generator/struct extended; all 3 arch tables vendored with per-operand constraints. |
| F7-full Step 2a (NONZERO via VRP) | DONE 2026-05-02 | BSR/IDIV/etc. unprovable nonzero rejected with vendor citation. |
| F7-full Step 2b (COMPOUND via VRP) | DONE 2026-05-02 | NONZERO check on divisor; INT_MIN/-1 backstopped at runtime. |
| F7-full Step 2c (ALIGNED via heuristic Pass B) | DONE 2026-05-02 | MOVAPS misaligned constants rejected. Heuristic walks all bindings (positional binding fails when register operands are clobbers). |
| F7-full Step 2d (BOUNDED via VRP) | DONE 2026-05-02 | Wiring active; no .zerdata entries use BOUNDED yet. |
| C8 instruction classification | DONE 2026-05-02 | x86 CLWB/CLFLUSHOPT (53 entries), ARM LDAR/STLR/LDARB/LDARH/STLRB/STLRH (37 entries). Persistent-memory + acquire/release primitives ready for Session G enforcement. |
| Session G Phase 1 (ordering plumbing) | DONE 2026-05-02 | `ZerBarrierKind` + `ZerOrderingRole` enums; ordering field on instruction entry; generator + 3 vendored tables. |
| Session G Phase 2 (per-instruction classification) | DONE 2026-05-02 | x86/ARM/RISC-V C8 entries declare ordering.barrier_kind + role (MFENCE→PRODUCES FullMemory; CLWB→REQUIRES_AFTER StoreStore; LDAR→PRODUCES Acquire; etc.). |
| Session G Phase 3 (in-block enforcement) | ABANDONED 2026-05-02 | Same-block check rejected canonical multi-block libpmem idiom; reverted. See docs/asm_preconditions_research.md "Design gaps identified during failed G3 attempt" for the 5 reasons why intermediate phases were dropped. |
| Session G IR-level enforcement | NEXT (~30-40 hrs) | Smallest design that doesn't false-positive: full CFG-aware OrderingState in `zercheck_ir.c`. Tracks barriers from BOTH asm blocks AND `@atomic_*` intrinsics. Joins use set-intersection. See docs/asm_preconditions_research.md "Implementation sketch". |
| C5 (privilege) | Covered by S1 | Naked-only restriction is the v1.0 stand-in. F7-full would add explicit kernel-context model. |
| C6 (memory addr) | Covered by @inttoptr | Existing MMIO range check fires at every address derivation. |
| Z9/Z10/Z13 forward-compat | Blocked on S1 relaxation | Included in F7-full estimate. |
| naked attribute migration | DEFERRED | Requires asm-test rewrite (~20 hrs after S1 relaxation). See docs/limitations.md. |

**Current F4/F7 dispatch fires (in checker.c NODE_ASM):**
- C1 (value range) — Step 2a/2b NONZERO/COMPOUND via VRP
- C2 (alignment) — Step 2c ALIGNED via heuristic Pass B
- C3 (state machine) — F7-light LR/SC pairing across same asm block
- C4 (CPU feature) — checked against `--target-features=` bitmap
- C5 (privilege) — covered by S1 naked-only restriction
- BOUNDED — Step 2d wiring ready (no instructions use it yet)

**C8 enforcement (acquire/release pairing, CLWB→SFENCE) deferred to Session G.**
Classification is in vendored tables; OrderingState tracking pending Stage 5.

**`.zerdata` is compiler-internal, vendored, NOT user-extensible.**
Design locked 2026-04-29.

**CLI flags (Stage 4-related):**
- `--target-arch={x86_64,aarch64,riscv64}` — picks register/instruction tables + cross-gcc
- `--target-features=avx512f,sse,sse2,avx,avx2,aes,sha,bmi1,bmi2,lzcnt,popcnt,invpcid,pku,xsave,smap` — comma-separated; sets ZerCpuFeature bitmap + appends `-m<flag>` to GCC. Baseline x86_64: SSE | SSE2.
- `--probe-mode={hosted,raw,disabled}` — `@probe` behavior (Fix #4 2026-05-02)

**Mnemonic parser accepts dots** (RISC-V `lr.w`, `fence.i`, etc.) —
both `scripts/gen_instruction_table.sh` section-header regex and
`checker.c` NODE_ASM token reader. Don't drop this — RISC-V tables
won't parse without it. Fixed BUG-653 + BUG-654 on 2026-05-02.

**`Checker.target_ptr_bits` MUST be initialized in `checker_init`**
from the global `zer_target_ptr_bits`. Pre-fix the field was
memset-zeroed and never synced — any check `c->target_ptr_bits < N`
silently always-true. See BUG-652 on 2026-05-02.

**For fresh sessions touching asm safety:**
- Read `docs/asm_plan.md` "Sub-Extension Architecture — Validated 2026-04-29" section
- The architecture decisions there are LOCKED IN — don't re-litigate
- F7-full Step 2 is COMPLETE — all 4 constraint kinds (NONZERO/COMPOUND/ALIGNED/BOUNDED)
  enforced. Pass A (positional, GCC %N convention) handles NONZERO/COMPOUND/BOUNDED.
  Pass B (heuristic walk all bindings) handles ALIGNED.
- F7-light LL/SC pairing implementation pattern (hardcoded LL/SC
  mnemonic macros per arch) is the template for future state-machine
  enforcement — see `checker.c` NODE_ASM dispatch
- @critical "transitive escape via callee" was investigated 2026-05-02
  and found to be NOT a bug — don't re-implement `can_escape`. See
  docs/limitations.md.
- **Session G (System #30 / atomic ordering) is the next major piece.**
  Phase 1 (data plumbing) + Phase 2 (per-instruction classification)
  DONE 2026-05-02. Phase 3 (in-block enforcement) was attempted and
  ABANDONED — same-block check creates false positives on canonical
  multi-block CLWB+SFENCE idiom. The honest path is IR-level
  enforcement (Phase 5 of original plan, now the next phase) —
  see `docs/asm_preconditions_research.md` "Design gaps identified
  during failed G3 attempt" for why intermediate phases are
  bypassed. Implementation: ~30-40 hrs CFG-aware OrderingState in
  `zercheck_ir.c` that tracks barriers from BOTH asm blocks AND
  `@atomic_*` intrinsics. Lesson: don't ship enforcement that
  rejects valid code patterns; either skip the check or wait until
  the analysis is correct.


## CFG migration record (moved from CLAUDE.md 2026-06-10)


**Phase F MIGRATION COMPLETE.** zercheck_ir.c is the SOLE production
safety analyzer. zercheck.c is now a 150-line shim delegating to
zercheck_ir for backward-compat (LSP, firmware tests, production test).
The original 3128-line AST analyzer is DELETED.

**Final state (post-F1+F2+F3):**
- `zercheck_ir.c` (~3800 lines) — production CFG-based safety analyzer
- `zercheck.c` (~150 lines) — backward-compat shim:
  `zercheck_run` → lower each fn to IR → `zercheck_ir` → return error_count
- `zerc` binary uses zercheck_ir directly (skips shim)
- `zer_lsp.c`, `test_firmware_patterns*.c`, `test_production.c` — call
  `zercheck_run` (the shim)
- `test_zercheck.c` — DELETED (was unit tests for the 3128-line analyzer;
  4 of 54 narrow patterns originally failed under IR — all 4 closed in
  Phase F3.1 + F3.2, see BUGS-FIXED.md 2026-05-03 / 2026-05-04)

**For fresh sessions:**
- DO NOT add new safety analysis to `zercheck.c` — it's a shim.
- ALL new safety analysis goes in `zercheck_ir.c`.
- DO NOT call `ir_lower_func` outside the emitter or the shim's
  `collect_funcs_from_file`. AST mutation issue (see below).
- The shim's `collect_funcs_from_file` lowers each function once per call.
  Callers (LSP, test harnesses) MUST produce a fresh AST per call —
  re-using the same `file_node` will corrupt due to `pre_lower_orelse`'s
  destructive AST rewrite. Current callers re-parse on every invocation.

**Phase F0 closed 11 IR parity gaps (2026-05-03 session):**

| Sub-phase | Gap | Fix |
|---|---|---|
| F0.1 | Coarse `ast_err==0 && ir_err==0 \|\| both>0` agreement check missed real disagreements | Per-test agreement reporter (`tools/agreement_audit.sh`) + `AGREEMENT_FAIL` machine-parseable line classifying each disagreement |
| F0.2 | No way to verify "what fails when IR is sole driver" | `ZER_AGREEMENT_AUDIT=1` env var (audit mode for the reporter) — runs both analyzers without bailing on AST failure |
| F0.3 | 6 false positives — fixed-point convergence used `ir_find_handle` (bare-only); compound handles caused infinite "changed" loops | Use `ir_find_compound_handle` in convergence check (zercheck_ir.c:2839) |
| F0.4 | `ir_contains_move_struct_field` checked one level deep; 2-level nested move struct undetected | Recursive depth-limited (32) check `ir_contains_move_struct_field_depth` |
| F0.5 | Function params of nested-struct types had inner `Handle` fields silently untracked | `ir_register_nested_handles` walks struct/union recursively, registers compound handles on entry block AND in post-fixed-point final pass (both sites needed!) |
| F0.6 | Spawn arg of move-struct type wasn't auto-registered before being marked TRANSFERRED | Auto-register as ALIVE first when arg is move-struct typed |
| F0.7 | Audit flagged `test_modules/opaque_layer1.zer`/`resource.zer` as IR false negatives — investigation showed AST was wrong (false positive on standalone library modules); IR was correct | No code change |

**Control knobs (post-F1):**
- `ZER_AGREEMENT_AUDIT=1` — debug mode: re-enable AST analysis
  alongside IR for `tools/agreement_audit.sh` measurement. Production
  compiles should NEVER set this.
- `ZER_DUAL_RUN`/`ZER_DUAL_RUN=0`/`=2` — REMOVED in F1.
- `ZER_IR_ONLY=1` — REMOVED in F1 (now the default behavior).

**Tools added in F0:**
- `tools/agreement_audit.sh` — runs all test suites with audit mode on,
  collects per-test disagreements into `ir_false_positive` /
  `ir_false_negative` / `ir_count_diff` classes. After F1+F2+F3 this
  tool serves as historical record + can detect any future divergence
  if zercheck.c logic ever drifts (it shouldn't, since it's now a shim).

**THE critical architectural constraint (still applies):**

`ir_lower_func` **mutates the AST** (`pre_lower_orelse` at ir_lower.c:1239
replaces `NODE_ORELSE` with `NODE_IDENT` referencing a temp local). Calling
`ir_lower_func` TWICE on the same function corrupts emission. The shim's
`collect_funcs_from_file` calls `ir_lower_func` ONCE per shim invocation;
callers (LSP, test harnesses) re-parse to get fresh AST per call.

Production `zerc` binary uses the emitter's single-lowering hook: zerc_main
registers `zerc_ir_hook` that collects each IRFunc as the emitter lowers
it. After emit completes, zerc_main runs iterative FuncSummary build +
main analysis on the collected IRFuncs — NOT re-lowering, just analyzing.

**For fresh sessions touching analyzers:**
- ALL new safety code goes in `zercheck_ir.c` (the production analyzer).
- Adding new IR-based analyses: register via `Emitter.ir_hook` for
  production path, or extend the shim for backward-compat callers.
- The 4 narrow patterns test_zercheck.c covered (pool_a-vs-pool_b alias,
  direct overwrite leak, free-then-realloc loop, struct copy alias UAF)
  ARE NOW IN zercheck_ir as of Phase F3.1 + F3.2 (2026-05-03/04). The
  fixes added: IR_COPY overwrite detection, struct-copy alloc_id
  propagation, `pool_name` field on `IRHandleInfo` + wrong-pool walker,
  and `ALIVE+MAYBE_FREED → MAYBE_FREED` merge case for monotonic lattice
  convergence. Tests in `tests/zer_fail/wrong_pool_*.zer` and
  `tests/zer/free_realloc_loop.zer`.

**Critical IR lowering fact (Phase 8d, per ir_lower.c:84):**
`IR_POOL_ALLOC` / `IR_SLAB_ALLOC` / `IR_POOL_FREE` / etc. enum values exist
but are **NEVER emitted by ir_lower.c**. Pool/Slab/Task method calls flow
through generic `IR_ASSIGN` (with NODE_ORELSE wrapping NODE_CALL, or direct
NODE_CALL) and `IR_CALL` (with `inst->expr` holding the call). Specialized
handlers for those opcodes in `zercheck_ir.c` are dead code. Method detection
lives in `IR_ASSIGN` and `IR_CALL` via `ir_classify_method_call(Node*)`.

**Do NOT try to "fix" the absence of IR_POOL_ALLOC emission** — this is
intentional architecture. Add detection in IR_ASSIGN/IR_CALL method-call
paths instead.

<!-- Sections moved to docs/compiler-internals.md 2026-04-19:
       - IR Path Validation (full history)
       - IR Path Architectural Invariants (22 items)
       - Scoped Defer Emission pattern
       - IR lowering patterns that must passthrough
       - Walker missing node kind class
       - emit_rewritten_node vs emit_expr
       - Pre-lowering architecture
       - Test Harness Architecture
     Total: ~145 lines moved, preserving all content. -->

<!-- The orphan IR sub-sections that lived here (lines ~1137-1278) were
     moved verbatim to docs/compiler-internals.md in the same commit. -->



## Release history v0.2–v0.4 (moved from CLAUDE.md 2026-06-10)

- **v0.2 (RELEASED):** Slab(T), volatile slices, stdlib (str/fmt/io), bundled GCC, zer-convert Phase 1+2
- **v0.2.1:** comptime functions + comptime if, 4-layer MMIO safety, @ptrcast/@container provenance, safe intrinsics, zer-convert P0+P1, value range propagation, bounds auto-guard, forced division guard, zercheck 1-4, 415+ bug fixes, 1,700+ tests
- **v0.2.2:** FULL CONCURRENCY: shared struct (auto-locking), shared(rw) (rwlock), spawn (fire-and-forget + scoped ThreadHandle+join), deadlock detection (compile-time lock ordering), condvar (@cond_wait/signal/broadcast/timedwait), threadlocal, @once, @barrier_init/wait, async/await (stackless coroutines via Duff's device), Ring channel pointer safety, allocation coloring, semantic fuzzer (32 generators), 461+ bug fixes, 3,200+ tests (incl. 400 Rust-equivalent safety/concurrency tests)
- **v0.3.0 (CURRENT):** `move struct`, `Barrier` keyword type, comptime locals/loops/switch/arrays/struct-return/float/enum, `static_assert`, range-based `for (T item in slice)`, `do-while`, designated initializers + compound literals, `container` keyword (monomorphization), `--stack-limit N`, spawn global data race detection (error/warning), 786 Rust tests + 36 Zig tests + 195 ZER integration + 68 ZER negative (0 failures), flag-handler matrix audit tool (`tools/audit_matrix.sh`) found 5 missing checker validations, ctags-guided audit found 3 emitter bugs in ~5K tokens, red team audit: 42/81 Gemini attacks fixed (12 rounds) + 2 codebase analysis finds + full 25K-line audit, `Semaphore(N)` builtin, BUG-462 through BUG-506 (46 bugs fixed), systematic refactoring (25 unified helpers — R1-R3 + B1 `track_dyn_freed_index` + B2 `check_union_switch_mutation` + B4 `emit_opt_wrap_value` + B10 `handle_key_arena` + 18 prior), full codebase audit (25,757 lines): 15 distinct unwrap fixes (BUG-506 + A15/A19/A20), 5 buffer over-read fixes, 2 fixed arrays → dynamic, 2 volatile temp fixes, spawn string+struct validation, orelse emission consolidated (B3), return wrapping consolidated (B7), union typedef macro (B8), zercheck 27 arena keys (B10), zig test runner (36 tests automated), refactor plan in `docs/ZER_Refactor.md`, CFG-aware zercheck with scope-aware handle tracking (`find_handle` vs `find_handle_local`), recursive mutex with CAS lazy init, unified `emit_file_module`, VRP 100% via address_taken at TOK_AMP + compound assign invalidation, deadlock call graph DFS, async state struct temp promotion (Rust MIR-equivalent), `*opaque` comparison `.ptr` extraction, runtime MMIO alignment check, C interop safety model (`cinclude` + `*opaque` + `shared struct`), 510+ bug fixes, 4,000+ tests, FuncProps function summaries (tracking system #29 — transitive context safety via lazy DFS on Symbol)
- **v0.4:** MIR-inspired IR (flat locals, basic blocks) — replaces 29 AST walkers with one lowering pass. zercheck on CFG, VRP on SSA-like locals, async complete by construction. Still emits C → GCC. See `docs/IR_Implementation.md`
- **v1.0:** self-hosting proof (zerc.zer compiles itself identically)

---

## 2026-09-22 session — assignment-as-value lowering, the RMW summary walk, orelse blocks in six walkers, the keep trace peel, RMW reach, the comptime folder (BUG-1041..1048)

**`LowerCtx.assign_stmt_pos` — the ONE place a NODE_ASSIGN's result is discarded
(BUG-1041).** `lower_expr`'s NODE_ASSIGN arm serves both `x += 1;` (statement, via the
NODE_EXPR_STMT arm) and `(x += 1) > 3` (value). The flag is set by the expression-statement
arm around its single `lower_expr` call and read-and-cleared at the top of the assign arm.
Compound in VALUE position: the synthesized `target op= tmp` node gets a typemap entry
(`checker_set_type`, the assignment's type else the target's) and goes through the
passthrough WITH a dest temp — `_zer_tN = (target op= tmp)` — which is why the uN / union /
shared statement-expression emissions need nothing new: their value is the store. Compound
in STATEMENT position still emits the void IR_ASSIGN (no dead temps). The for STEP is a
direct passthrough and never reaches the arm. If you add another statement-level caller of
`lower_expr` on a NODE_ASSIGN, set the flag around it or every `x += 1` there grows a temp.

**`rmw_scan_body` is exhaustive (BUG-1043).** The main-side "does this function RMW through
param n?" summary (`func_rmw_param_mask`, BUG-801) descends every child now. The RMW FORM
grid in `tests/test_hw_matrix.c` has a third site, `RSITE_MAIN` (helper called from main,
ISR stores plainly), because the spawn and ISR sinks are exhaustive body scans and could
not show a summary-only hole. `rmw/spawn/param in @once` is the grid's one POSITIVE cell:
`scan_unsafe_global_access` keeps NODE_ONCE a leaf on purpose (a once-body has one writer
and publishes with release) and the cell pins that decision; at the ISR and MAIN sites the
same body is rejected, because an interrupt orders nothing against a once-body.

**RMW REACH — one fact, one barrier, three sinks (BUG-1046).** The "which global does
this call ARGUMENT designate?" question is answered by `rmw_arg_target_global` (scans:
`&g`, alias-table name) and `rmw_arg_target_global_main` (main: `&g`, carrier-table name,
pointer local's `&g` init hop — never a global pointer itself). The CARRIER fact —
`h.p = &g` / `H h = { .p = &g }` / `q = &g` binds the local's ROOT name to `g` — is
recorded by `rmw_bind_carrier_scan` / `rmw_bind_carrier_scan_name` into `_rmw_alias`
(update in place: `rmw_alias_lookup` returns the FIRST match) and by
`rmw_bind_carrier_main` / the var-decl seed into `Checker.rmw_ptr_carriers` (per
function, reset beside `rmw_taints`; a bare-ident target rebinds or clears, a projection
target only adds). `carrier_value_global` is the one walk over the VALUE (`&g` through
launders, an IDENT that is itself an alias / carrier — a pointer or carrier COPY — a
struct literal's fields, both orelse arms; its `main_side` flag picks which argument
resolver an IDENT goes through). A callee the analysis cannot
name — `callee_is_opaque_funcptr`: an IDENT that is not a global function nor
`alloc`/`free`, or a FIELD/INDEX typed as a funcptr — makes every global handed by
pointer a MAY-RMW: `track_isr_global_opaque` (ISR + main, `IsrGlobal.opaque_in_*`, its
own branch in `check_interrupt_safety`) and `_rmw_flagged_opaque` (spawn, its own
sentence in the spawn report). At the main sink the FIELD/INDEX callee form lives after
`normal_call:` because the callee is typed only there. When you add a vehicle by which a
pointer can reach a helper, teach `carrier_value_global` (a value shape) or the two
`rmw_arg_target_global*` twins (an argument shape) — not a sink.

**`keep_arg_caller_root` peels through `unwrap_ptr_launder` (BUG-1045).** It had its own
"last argument is the pointer" rule, wrong for `@container` (last arg = field name). Any
future trace that needs "the pointer an intrinsic carries" asks the shared peeler. Its
orelse arm follows BOTH arms (a join), recursing on each; a non-expression fallback is -1;
its NODE_CALL arm follows the arguments the callee's `ret_param_mask` names (all of them
when `ret_summary_complete` is false); its NODE_STRUCT_INIT arm traces every field value.

**The comptime interpreter fails closed (BUG-1048).** `eval_comptime_block` signals through
`ComptimeCtx.failed` / `brk` / `cont`: `CT_FAIL()` for every failure (a nested body's
failure is the caller's failure — the old "CONST_EVAL_FAIL from a nested body means no
return here" conflation is gone), `CT_AFTER_BODY` / `CT_AFTER_LOOP_BODY` after every
nested evaluation, `CT_UNSUPPORTED(what, line)` for a statement it does not model (the
kind list is a no-`default:` switch; `comptime_report_unsupported` appends the reason to
the "could not be evaluated" error). `break` / `continue` are real. If you teach the
interpreter a new statement kind, remove it from the unsupported switch AND make sure every
failure inside it goes through `CT_FAIL`.

**Compound division guard reports a folded-zero divisor (BUG-1042)** with the same
"division by zero" as the binary site; `ct_apply_assign_op` fails (never folds 0) on `/= 0`,
`%= 0` and `INT64_MIN / -1`.

## 2026-09-21 session — whole-program post passes, the for-loop lower bound, and module container globals (BUG-1034..1040)

**Post passes are whole-program now (BUG-1037).** `checker_post_passes_files(Checker *,
const CheckerFile *files, int n)` (checker.h) is the entry point; `zerc_main.c` builds the
array from the modules in topological order (dependencies first, main LAST) with each
module's `ast / file_name / source / module / module_len`. Inside:

- per-file passes run under `ZER_ENTER_FILE(i)` (switches `c->file_name`, `c->source`,
  `c->current_module`) so a diagnostic names the right file and echoes the right line:
  `check_call_provenance` (every decl of every file) and `check_lock_ordering`;
- whole-program passes run once: `check_interrupt_safety` / `check_atomic_cell_safety`
  (global state, unchanged), `check_stack_depth_files` (frames from EVERY file; ISR
  detection via `files_declare_isr` across all of them; frames are keyed by NAME, so two
  modules' same-named functions share one frame whose size is the SUM — conservative), and
  `check_builtin_init_files` (ONE `ZerInitSet` over every file — an arena declared and
  allocated from in a module and backed in main is initialised; each entry remembers its
  declaring file for the diagnostic).
- `checker_post_passes(c, file)` is the one-file wrapper, kept for `checker_check` (the
  C unit tests / LSP path).

Add a new post pass INSIDE `checker_post_passes_files`, never as a second call in the driver.

**The for-loop lower bound (BUG-1034).** In `case NODE_FOR`, the counter's `[init,
bound-1]` push is now gated: `lo_sound` = `!ast_name_mutated_or_addrd(body, counter)` and
the step is either not a writer of the counter or a non-negative constant increment
(`i += C`, `i = i + C`, `i = C + i`, `eval_const_expr(C) >= 0`). Otherwise the seed is
`INT64_MIN` (unsigned counters clamp to 0 in `push_var_range`, so only a signed counter that
is really lowered loses precision). `ast_name_mutated_or_addrd` (zercheck_ir.c, the Level-B
guard-stability walker) is exported through checker.h for this — it is the one exhaustive
no-default "is this name written or address-taken anywhere in here?" walk in the tree; do
not write another.

**`find_return_range` reaches every expression position (BUG-1035).**
`scan_expr_orelse_returns` is now called on the if / while / do-while condition, the for
init (through `find_return_range`, since it may be a statement) / cond / step, the switch
subject, the await condition and each spawn argument, and it descends `NODE_ASSIGN`. The
rule that keeps it sound is unchanged: a buried return with no derivable range makes the
whole summary give up.

**FuncProps sees the universal builtins (BUG-1036).** `scan_func_props`'s NODE_CALL arm
recognises `alloc` / `free` with a bare NODE_IDENT callee and no function Symbol — the same
by-name recognition `scan_frame` uses — as `can_alloc` + `has_direct_alloc`. `alloc(T)` on
a struct was already caught because the checker rewrites it to the `T.alloc_ptr()` method
form; `alloc(T, n)` and `free(slice)` are not rewritten.

**`@size` operand (BUG-1038).** The checker resolves a `uN`/`iN` ident operand as the type
it names (`zer_is_intn_type_name` → a synthesised `TYNODE_NAMED` → `resolve_type`) and
`checker_set_type`s it on the ident; both emitter `@size` arms (AST ~3690, IR ~8340) read
`checker_get_type(args[0])` FIRST and `emit_type` it — a variable's size is its type's.
The old name-based `struct <name>` fallback is still there for anything the checker did not
type.

**Address-int arithmetic at the assignment sink (BUG-1039).** The BUG-995 arm in
`NODE_ASSIGN` now has an `else if` for a non-ident, non-call root: `expr_touches_local_derived`
on the whole value (the same walker the var-decl sink uses to taint `usize b = a + 1`).

**Module container globals (BUG-1040).** `emit_module_global_name(e, name, len)` spells
`mod__name` inside a module and is used by the Pool / Ring / Arena / Slab declaration arms
of `emit_global_var_inner`. `emit_builtin_inline` (the IR builtin-method emitter) re-spells
its receiver `on/ol` by the ident emitter's rule (a local of `func` keeps its name; a global
with `module_prefix` gets `current_module` inside a module or its own prefix from main; an
unknown name inside a module gets `current_module`). `emit_alloc_sym_cname(e, sym)` spells
an allocator found by `find_unique_allocator` at the three Handle auto-deref sites — the
lookup can return either of BUG-233's two Symbols (raw or mangled key), so the prefix is
added only when the name does not already carry it. The arena `alloc` / `alloc_slice`
element is spelled with `emit_type` (module-prefixed), not a hand-rolled `struct <name>`.
Residuals: limitations.md "AST-path container-method receivers" and "same NON-static global
name in two modules".

## 2026-09-23 session — shared queries introduced (BUG-1049..1059, 1110..1115)

Each is ONE answer to a question that used to be answered at several sites. Call it; do not
re-derive the question at a new site.

| query / helper | file | question it answers | replaced |
|---|---|---|---|
| bare-ident arm of `ir_extract_compound_key` | zercheck_ir.c | "which tracking entry names this bare GLOBAL?" — `(IR_GLOBAL_ROOT_ID, "g")`, the key BUG-739's store arm already used | a key only one arm could produce (BUG-1049) |
| `ir_report_dangling_global` | zercheck_ir.c | the dangling-global sentence at the exit AND call boundaries, carrier-aware (a non-optional global cannot take `= null`) | two literal copies |
| `ir_local_desc` + `ir_zc_error_for` | zercheck_ir.c | how to NAME an IR local in a diagnostic; drop a temp-only duplicate on a line already reported | 33 `local %%%d` prints (BUG-1050) |
| `make_local_ident` | ir_lower.c | an AST identifier for an IR local, carrying the local's TYPE | 7 hand-built idents, some untyped (BUG-1051) |
| `field.handle_alloc` (set by the checker) | ast.h / checker.c / emitter.c | which allocator a Handle auto-deref `h.f` uses | three emitter re-searches that gave up on 2 allocators (BUG-1053) |
| `type_is_null_sentinel` | types.c | `?*T`/`?funcptr` are NULL-sentinel | emitter-private `is_null_sentinel` |
| `ast_name_bind_count` | zercheck_ir.c | how many times is NAME bound (var-decl / if-capture / switch-capture) in a body | nothing — `ast_name_mutated_or_addrd` only asked "is the binding written" (BUG-1055) |
| `mmio_inttoptr_bound` | checker.c | elements between a constant `@inttoptr` address and its range end, struct-aware | three copies using `type_width` (0 for a struct) (BUG-1056) |
| `global_name_never_mutated` (cached on `Symbol.never_mutated_cache`) | checker.c | is a GLOBAL ever assigned / address-taken in ANY registered body (`Checker.reg_files`)? Ask it before trusting a global's DECLARATION initializer | stale MMIO bound; funcptr stack resolution (BUG-1056, 1059e) |
| `reject_array_view_hazards` (= packed + `reject_array_view_qualifier_drop`) | checker.c | may this array-valued expression become this slice? (misaligned / dropped volatile / dropped const) — call it at EVERY value-flow sink | three ident-only checks; the assign sink never called the packed one (BUG-1057) |
| `emit_inttoptr` | emitter.c | the one emission of `@inttoptr` for BOTH dispatch paths | two identical copies with the same truncation/wrap bugs (BUG-1058) |
| `track_isr_pointee_of` | checker.c | naming a global pointer with an `&g` initializer reaches `g` (ISR sharing) | — (BUG-1059a) |
| `comptime_cond_value` | checker.c | fold a compile-time CONDITION (bools included) for `comptime if` / `static_assert` | ad-hoc ident lookup + integer fold (BUG-1110) |
| `emit_func_decl_head/params/tail` | emitter.c | the C declarator of a function, including one RETURNING a funcptr | two signature emitters, one wrong (BUG-1111) |

Two traps met this session worth remembering:

- **A walk that runs OUTSIDE the checker's own nesting must re-establish the context it
  reads.** `record_isr_globals` is a post pass; `track_isr_global_ex` consulted
  `critical_depth`, which was 0 there. The fix pushes/pops `critical_depth` at the walk's
  `NODE_CRITICAL` arm. Any new context-sensitive predicate called from a post-pass walker has
  the same hazard.
- **A size/alignment walk that recomputes a child to learn its alignment is exponential in
  depth.** `compute_type_size` did, and a 40-deep nest took 17 s (then 34 s once a second
  caller appeared). Ask `type_alignment_bytes` — linear — for alignment, and remember a ZER
  union carries an `int32_t _tag` (alignment >= 4).

Parallel-work note: this session ran fix-agents in `.claude/worktrees/` under the repo. Add
`.claude/` to `.git/info/exclude` before `git add -A`, and keep scratch tooling in a PRIVATE
subdirectory — one agent overwrote a shared `corpus.sh`, and every concurrent
`tests/test_*_matrix` run shares fixed `/tmp/_zer_*` paths (limitations.md).

## 2026-09-23g session — the emitter entry wrappers, the load guard, and five shared queries (BUG-1152..1175)

**`emit_expr` / `emit_rewritten_node` are now thin WRAPPERS** over `emit_expr_impl` /
`emit_rewritten_node_impl`. Every recursive emission passes through the wrapper, which is the
one place three cross-cutting facts are applied:
1. **The load guard** (`nn_guard_wanted` → `load_guard_kind`): a FIELD / INDEX / `*pp` whose
   type is a non-optional `*T` (BUG-1152) or a no-zero-variant enum (BUG-1175) is emitted as
   `({ __auto_type _zer_nnN = LOAD; if (!_zer_nnN) _zer_trap(...); _zer_nnN; })`. The
   wrapper sets `Emitter.nn_bypass = node` before recursing so the node's own emission is not
   wrapped twice (a node emitted twice inside `__typeof__` stays unwrapped — typeof does not
   evaluate). `__auto_type` drops only the top-level qualifier of the pointer VALUE.
2. **Lvalue exemption**: `nn_lvalue_of(node)` names the assignment target / `&` operand of the
   node being emitted; the wrapper stores it in `Emitter.nn_lvalue` for the duration, so the
   same Node is never guarded as a load. Save/restore makes nesting safe.
3. **The union reset prefix** (`union_partial_write_target` + `emit_union_reset_prefix`,
   BUG-1161): a compound op or partial write into a union variant is emitted as
   `((reset-if-variant-changes, 0), <assignment>)`.
Skipped at file scope (`global_init_depth > 0`): a statement expression is illegal there.
The IR's own decomposed loads (IR_FIELD_READ / IR_INDEX_READ / IR_UNOP deref) are statements,
so they get `emit_nonnull_local_check` on the SAME C line as the load (the trap's `__LINE__`
must be the load's `#line`, which BUG-1003 made per-instruction).

**`emit_intn_store`** (BUG-1162..1164) is the one uN/iN store path for both emitters: compute
and mask in a carrier temp, store ONCE (a volatile register sees one store), set the union
tag when the target is a variant, and leave a bit-slice target to its own SET case.

**`opaque_type_id` / `opaque_type_id_nominal`** (BUG-1166) replace seven copies of the
struct/enum/union type-id triple. The wrap/unwrap sites use the full one (scalars get reserved
high-bit ids so a `*u32` wrapped in a callee cannot be unwrapped as `*Big`); `@pun` uses the
nominal one, mirrored by the checker's `pun_type_id_check_can_fire`.

**Checker shared queries added:** `container_prov_of_value` (the @container provenance of a
value, following a single-param return summary — BUG-1167); `value_reaches_threadlocal`
(BUG-1170, reads the recorded `borrow_root_name`, which deliberately keeps global roots);
`record_borrow_roots_from_struct_init` (BUG-1169); `borrow_alias_reaches_lent_root` +
`target_path_derefs` (a write/read THROUGH a pointer bound to a lent local); the struct-literal
field gate in `struct_init_has_local_derived` (BUG-1158); `reject_packed_view_in_literal`
(BUG-1171, called from `type_struct_literal`, so every literal sink gets it).

**zercheck_ir:** `FuncSummary.resets_arena` + `ZerCheck.cur_resets_arena` (BUG-1172; the flag
is set while analysing a function and stored into its summary, so the iterative summary build
closes it transitively); `ir_check_returned_compounds` (BUG-1173); the out-param
FREED-at-exit rule restricted to allocations made in the function (a field the CALLER put
there is a hand-off reported by `frees_param_field` at the call site — the reference gate
caught the unrestricted first draft on `kv_table_free`). Return summaries and the join check
no longer skip orelse-fallback blocks (BUG-1155). `IRLocal.snapshot_of_plus1` marks the
return-value snapshot temp (BUG-1154) so diagnostics name the local it copies.

---

## Notes moved from the limitations ledger when its legacy file was retired (2026-11-14)

Fourteen sections of `docs/limitations-legacy.md` were not limitations: eight are hand-off
notes of the 2026-08 branch survey, five are oracle planning notes from 2026-06 (the plan
of record is `docs/unified-oracle-proved-ZER.md`; the grids these notes plan — concurrency,
hardware, async, asm — exist now, see CLAUDE.md "What a green `make check` covers"), one is
a tracking stub. Moved verbatim; each heading carries `[MOVED 2026-11-14]`. Dated history:
measure before relying on any of it.

---

## [MOVED 2026-11-14] Where main stands

`make check` exit 0 — **1391 .zer**, modules 30/30, 200 fuzz, 139 convert, all ten matrices, all EIGHT
audit gates (`audit_reference_examples.sh` joined with BUG-900), sink matrix **88 cells / 0 mismatch**.

Landed this session, newest first:

| sha | what |
|---|---|
| `7c873edd` | BUG-884..888 — the call-RESULT view class (4 parts) + my container fix REPLACED + BUG-869/870/871 |
| `c0180e3f` | BUG-883 — float -> int is DEFINED as SATURATING |
| `ae033cd0` | BUG-857/858 — container SEGFAULT (superseded, see below) + recycled-slot auto-zero |
| earlier | the 45-row harvest 1, BUG-815..856 |
---

## [MOVED 2026-11-14] DECIDED — do not re-open

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
---

## [MOVED 2026-11-14] THE CHEAP PATH: cherry-pick, do not re-derive — this is how tracker 3 was closed

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
---

## [MOVED 2026-11-14] Verification that must not be skipped

1. **Reproduce on main FIRST.** Read the DIAGNOSTIC, never the exit code — `-o out.c`
   isolates the checker's verdict from GCC's, and the echoed source line contains the
   offending text, which defeats a loose grep.
2. **Build a pre-fix compiler and prove the test flips**:
   `git archive <prev-sha> | tar -x -C $S && (cd $S && rm -f *.o src/safety/*.o && make zerc)`
3. **A new gate must be shown to FIRE**, not merely pass. `bash tools/sink_matrix.sh $S/zerc`
   is the pattern — p18 reported 7 holes + 2 over-rejects pre-fix, 0 after.
4. `MAKE_CHECK_EXIT` must be echoed. Make aborts at the first failing audit, so a later
   gate silently never runs.
---

## [MOVED 2026-11-14] Corrections I made to MY OWN work — do not repeat these

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
---

## [MOVED 2026-11-14] Two gates that were themselves broken — the general lesson

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
---

## [MOVED 2026-11-14] The two closure probes are MEASUREMENT SCRIPTS, not gates — do not count them as coverage

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
---

## [MOVED 2026-11-14] Recommended order — NOTHING REMAINS (kept as the record of what was done)

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

## [MOVED 2026-11-14] OPEN — MAX-ORACLE GAP AUDIT (2026-06-23) — the master map: which safety classes are not-sound / not-flexible / coarse-or-no-oracle

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

## [MOVED 2026-11-14] PLAN — asm Option E rework (Level C cleanup first)

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
---

## [MOVED 2026-11-14] STATUS — soundness oracle suite (read first, 2026-06-07)

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

## [MOVED 2026-11-14] OPEN — next frontier: concurrency / ISR / atomics / async / MMIO oracles

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

## [MOVED 2026-11-14] OPEN — shape-matrix oracle: remaining coverage roadmap

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

## [MOVED 2026-11-14] Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.

When a `tests/zer_gaps/` reproducer is fixed, move it to
`tests/zer_fail/` so it becomes a permanent regression guard.
