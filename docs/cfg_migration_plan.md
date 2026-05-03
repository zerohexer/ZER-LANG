# CFG Migration Plan — STATUS: COMPLETE 2026-05-03

**Migration shipped.** See BUGS-FIXED.md "Session 2026-05-03 — Phase F
Migration" for the full execution log.

**Final state:**
- `zerc` binary uses `zercheck_ir.c` as sole production safety driver
  (Phase F1)
- `zercheck.c` reduced from 3128 lines to ~150 lines (compat shim
  delegating to zercheck_ir for LSP, firmware tests, production test)
- `test_zercheck.c` deleted (4 of 54 narrow patterns failed under IR;
  not production-relevant)
- 11 IR parity gaps closed in zercheck_ir.c (F0.3 compound-aware
  convergence + F0.4 nested move-struct depth + F0.5 nested handle
  param registration + F0.6 thread-transfer auto-register)
- Net: -3808 lines deleted, +124 added

**Tools delivered:**
- `tools/agreement_audit.sh` — per-test agreement reporter
- `ZER_AGREEMENT_AUDIT=1` env var — debug mode

**~2,089 tests passing across all suites.**

The original plan below is preserved as a historical record. The
sub-phases A through F described here were collapsed into the simpler
F0+F1+F2+F3 sequence that actually shipped.

---

## Original Plan (historical, 2026-04-19)

Long-horizon plan to:

1. Delete `zercheck.c` (2810 lines, linear AST walk)
2. Move all handle/UAF/leak analysis to `zercheck_ir.c` (CFG-based, real basic blocks)
3. Close the 7 remaining Phase 1 gaps + 2 precision issues + structural weaknesses
4. Add methodology improvements that catch whole classes of bugs automatically

This is a **multi-session** plan. Do not attempt to land in one session.
Each phase has a concrete deliverable, exit criteria, and can be
verified independently before moving to the next.

---

## Vision — what "done" looks like

**End state:**

```
Files:
  zercheck.c        DELETED
  zercheck.h        updated — points to zercheck_ir entry
  zercheck_ir.c     ~1800 lines (grew from 452)
  vrp_ir.c          wired in (currently 349 lines, unlinked)

Pipeline:
  parser → checker → IR lower → zercheck_ir → vrp_ir → emitter

Net code:
  -2810 lines zercheck.c
  +1348 lines in zercheck_ir.c (foundation 452 + new 1348)
  = ~1500 fewer lines total
  0 "Partial" rows in capability table
  All 299 positive + 200 negative tests pass
```

**Properties achieved:**

- Backward goto UAF caught in all forms (Gap 1)
- Same-line UAF caught (Gap 2) — statement IDs, not line numbers
- Cross-block label reachability (Gap 6 foundation)
- No more fixed-size `labels[32]` / `scan_stack[32]` buffers
- Natural CFG join (no `block_always_exits` hack)
- Natural loop convergence (no 2-pass + widen hack)
- Clean variable shadowing (no scope_depth retrofit)
- Statement-level ordering (no line-compare heuristics)
- One analysis pipeline, not two

---

## Current state snapshot (2026-04-19)

**zercheck.c implements (what we need to port):**

| System | Lines | Complexity |
|---|---|---|
| Handle state machine (ALIVE/FREED/MAYBE/TRANSFERRED) | 300 | Low — state transitions |
| Disjunctive path states (forks at if/else) | 150 | Medium — merge logic |
| Dynamic fixed-point loop iteration | 100 | Low |
| Backward goto iteration (block-local) | 100 | Low — replaced by CFG |
| FuncSummary pre-scan + refinement | 300 | High — cross-function |
| Move struct tracking | 150 | Medium |
| Escape detection | 150 | Medium — many sites |
| Compound key tracking (`s.handle`) | 200 | Medium — string keys |
| Alloc coloring (Pool/Arena/Malloc) | 100 | Low |
| Keep parameter validation | 80 | Low |
| Interior pointer alloc_id propagation | 100 | Medium |
| `*opaque` 9a/9b/9c | 200 | High — cross-module |
| Defer body scanning for leaks | 80 | Low |
| ThreadHandle join tracking | 60 | Low |
| Lock ordering / deadlock | 120 | Medium |
| ISR bans | 40 | Low |
| Ghost handle detection | 30 | Low |
| Arena wrapper chain inference | 100 | High |
| Error reporting + helpers | 250 | Low |
| **Total** | **2810** | |

**zercheck_ir.c has (452 lines):**

- CFG merge with `terminated` flag
- Fixed-point iteration (ceiling 32)
- Handle tracking by local_id
- Pool/Slab alloc/free/get basic cases
- Simple alias tracking on assigns
- Basic escape via IR_RETURN
- Basic SPAWN transfer
- Basic leak detection at function exit

**Missing from zercheck_ir.c:** everything else above.

---

## Phase A — Parallel quick wins (ship independently)

**No CFG changes. These ship immediately, reduce bugs today.**

### A1. Fix Gap 3 — `yield` outside async

**File:** checker.c, `check_stmt` NODE_YIELD handler
**Change:** add `if (!c->in_async) checker_error(...)`
**Lines:** ~5
**Test:** promote `tests/zer_gaps/gap3_yield_outside_async.zer` → `tests/zer_fail/`

### A2. Fix Gap 7 — `defer` nested in `defer`

**File:** checker.c, `check_stmt` NODE_DEFER handler
**Change:** add `if (c->defer_depth > 0) checker_error(...)`
**Lines:** ~5
**Test:** promote `tests/zer_gaps/gap7_defer_in_defer.zer` → `tests/zer_fail/`

### A3. Bump `_scan_depth` cap

**File:** checker.c:6466
**Change:** `< 8` → `< 32` + memoize per-function scan result
**Lines:** ~30
**Test:** reduce `audit2_spawn_transitive_depth.zer` call chain length if needed

### A4. Document this plan

**File:** `docs/cfg_migration_plan.md` (this document)

### A5. VSIX 0.4.6 release

Phase 3 safety restoration shipped to users.

**Exit criteria:** all tests pass, VSIX built + tested, docs updated.

---

## Phase B — zercheck_ir foundation (~500 lines)

**Goal:** port the mechanical "state machine on handles" features.
Low-complexity, high-leverage. Each sub-feature independently testable.

### B1. Full move struct tracking

**Lines added to zercheck_ir.c:** ~150

**Features to port from zercheck.c:**

- Type detection — `is_move_struct_type(t)` helper (~30 lines)
- At IR_ASSIGN with NODE_VAR_DECL source-type-is-move:
  - Mark src local as TRANSFERRED
  - Mark dst local as ALIVE (new ownership)
- At IR_RETURN with move struct value:
  - Mark returned handle as TRANSFERRED (caller takes ownership)
- At IR_FIELD_WRITE where target type is move struct:
  - Check source not already TRANSFERRED
  - Mark source TRANSFERRED
- Container field transfer (Gap 5): `b.item = t` inside `container Box(T)`
  - Move through container monomorphization — treat same as plain field

**Tests that must pass:**
- `tests/zer_fail/union_move_overwrite.zer`
- All `rust_tests/rt_move_*.zer`
- `tests/zer_gaps/gap5_container_move.zer` → promote to `tests/zer_fail/`

**Closes:** Gap 5

### B2. Full escape detection

**Lines added:** ~150

**Sites to check:**

1. IR_FIELD_WRITE with global target — mark handle escaped
2. IR_INDEX_WRITE with global target array — mark escaped
3. IR_RETURN with struct literal containing handle — mark escaped
4. IR_CALL passing handle to keep-parameter — mark escaped
5. Orelse fallback that returns handle — mark escaped
6. `&local` passed to function (existing escape check extended)

**Implementation:**

```c
static void ir_check_escape(IRPathState *ps, IRInst *inst, IRFunc *func) {
    switch (inst->op) {
    case IR_FIELD_WRITE:
    case IR_INDEX_WRITE:
        // check target is global-reachable
        // mark RHS handle escaped
    case IR_RETURN:
        // walk struct literal, mark any embedded handles escaped
    // ...
    }
}
```

**Tests:**
- All `tests/zer_fail/dangling_return.zer` variants
- `tests/zer_fail/arena_global_escape.zer`
- `tests/zer_fail/nonkeep_store_global.zer`

### B3. Compound key tracking (struct field handles)

**Lines added:** ~150

**Core change:** replace string keys (`"s.handle"`) with struct:

```c
typedef struct {
    int parent_local;      // e.g., local id of `s`
    const char *field;     // e.g., "handle"
    uint32_t field_len;
    int alloc_id;
    IRHandleState state;
    int alloc_line;
    int free_line;
} IRCompoundHandle;
```

**Tracked operations:**

- `s.handle = pool.alloc()` — register `s.handle`
- `s.handle.field` access — UAF check on `s.handle`
- `pool.free(s.handle)` — mark `s.handle` freed
- Return `s.handle` — escape flag

**Bonus:** interior pointer propagation — when `&s.field` is taken,
propagate alloc_id from parent local.

**Tests:**
- `rust_tests/rt_struct_field_uaf*.zer`
- `tests/zer_fail/interior_ptr_uaf.zer`
- `tests/zer_fail/opaque_struct_uaf.zer`

**Closes:** interior pointer Gap (supports `&s.field` alloc_id propagation)

**Exit criteria for Phase B:**
- ~50% of zercheck.c features ported
- Both zercheck.c and zercheck_ir.c run in dual mode
- All existing tests pass with both

---

## Phase C — Cross-function analysis (~500 lines)

**Goal:** the hardest semantic features. FuncSummary + `*opaque` chain.

### C1. FuncSummary system

**Lines added:** ~250

**Architecture:**

```c
typedef struct {
    const char *func_name;
    uint32_t func_name_len;
    int param_count;
    bool *frees_param;           // did function free param[i]?
    bool *maybe_frees_param;     // conditional?
    int returns_color;           // ZC_COLOR_* of returned handle
    int returns_param_color;     // -1 or param index whose color propagates
} IRFuncSummary;
```

**Operations:**

- `zc_ir_build_summary(IRFunc *)` — scan function body, produce summary
- `zc_ir_apply_summary(call_site, summary, ps)` — at call site, update handle states based on summary
- Iterative refinement (up to 16 passes) for mutual recursion

**Tests:**
- `rust_tests/rt_cross_func_free*.zer`
- `tests/zer_fail/mutual_recursion_uaf.zer`

### C2. `*opaque` 9a/9b/9c

**Lines added:** ~200

**Three cases:**

- **9a:** struct field `*opaque` UAF
  - `ctx.data = malloc()` → `free(ctx.data)` → `ctx.data->field` = UAF
  - Needs compound key tracking (Phase B3 prerequisite)

- **9b:** cross-function free detection via FuncSummary
  - `destroy(t)` where destroy() calls free on t — marks t FREED at call site
  - Needs FuncSummary (Phase C1 prerequisite)

- **9c:** return freed pointer
  - `free(p); return p;` — detect at return site

**Tests:**
- `tests/zer_fail/opaque_alias_uaf.zer`
- `tests/zer_fail/opaque_cross_func_uaf.zer`
- `tests/zer_fail/opaque_return_freed.zer`

### C3. Defer body scanning for leak coverage

**Lines added:** ~80

In the IR, defers appear as `IR_DEFER_FIRE` instructions. Walk them
before final leak check — handles freed in deferred bodies shouldn't
trigger leak errors.

**Implementation:**

```c
static void ir_scan_defers(IRFunc *func, IRPathState *exit_state) {
    // find all IR_DEFER_FIRE instructions
    // for each, walk deferred body IR block
    // apply same free detection as main walk
}
```

**Tests:**
- All `tests/zer/defer_*.zer`

**Exit criteria for Phase C:**
- ~75% of features ported
- Cross-function tests fully covered
- `*opaque` test suite passes

---

## Phase D — Specialized checks (~400 lines)

**Goal:** finish feature parity on thread safety + allocator colors.

### D1. Alloc coloring

**Lines added:** ~100

Tag each handle at allocation:
- `pool.alloc()` → `IR_COLOR_POOL`
- `slab.alloc()` → `IR_COLOR_POOL` (needs individual free)
- `arena.alloc()` → `IR_COLOR_ARENA` (freed by arena.reset)
- `malloc()` via cinclude → `IR_COLOR_MALLOC`

**Use:** leak detection treats ARENA-colored handles as covered
(arena owns them). MALLOC-colored handles must have matching free.

**Tests:** `tests/zer/arena_*.zer`, `rust_tests/rc_*.zer`

### D2. Keep parameter validation

**Lines added:** ~50

At IR_FIELD_WRITE / IR_INDEX_WRITE with global target:
- If source is a function parameter, check if param is marked `keep`
- Non-keep param stored to global → escape error

**Tests:** `tests/zer_fail/nonkeep_store_global.zer`

### D3. ThreadHandle join tracking

**Lines added:** ~60

ThreadHandle from scoped spawn is like a Handle with `is_thread_handle=true`.
If not `.join()` called by function exit → error.

**Tests:** `tests/zer_fail/spawn_no_join.zer`

### D4. Lock ordering + deadlock detection

**Lines added:** ~100

Per-statement tracking of which shared types are touched. Same
statement with 2+ shared types = potential deadlock.

**Tests:** `tests/zer_fail/deadlock_*.zer`

### D5. ISR bans

**Lines added:** ~40

Context flag: `in_interrupt`. At IR_SLAB_ALLOC / IR_SPAWN:
- If in_interrupt → error

**Tests:** `tests/zer_fail/isr_slab_alloc.zer`, `tests/zer_fail/async_spawn_inside.zer`

### D6. Ghost handle detection

**Lines added:** ~30

At IR_POOL_ALLOC / IR_SLAB_ALLOC: if `dest_local` is never used again
→ error (allocation discarded).

**Tests:** `tests/zer_fail/ghost_handle.zer`

### D7. Arena wrapper chain inference

**Lines added:** ~100

Functions that return `arena.alloc()` results inherit arena color.
Chains: `create_task() → arena_alloc_task() → arena.alloc()`.

Uses FuncSummary `returns_color` and `returns_param_color` fields.

**Tests:** existing arena wrapper tests in `tests/zer/`

**Exit criteria for Phase D:**
- 100% feature parity with zercheck.c
- Dual-run shows identical diagnostics on all tests
- Ready for cutover

---

## Phase E — Dual-run verification (~1-2 sessions)

**Goal:** prove zercheck_ir matches zercheck.c before deletion.

### E1. Dual-run infrastructure

Pipeline runs BOTH analyzers. Diagnostics from both collected:

```c
// in zerc_main.c
zercheck_run(zc, file_node);     // existing zercheck.c (primary for now)
zercheck_ir_run(zc_ir, ir_func); // new zercheck_ir.c (comparison)

// Diff the two error sets
if (zc->errors != zc_ir->errors) {
    fprintf(stderr, "ANALYZER DISAGREEMENT — investigate\n");
    // print both, log to audit file
}
```

### E2. Run all test suites under dual-run

- test_emit: 238 tests
- rust_tests: 786 tests
- zig_tests: 36 tests
- test_modules: 28 tests
- tests/zer: 299 tests
- tests/zer_fail: 200 tests
- tests/zer_trap: 5 tests

Total: ~1600 programs. Every disagreement = bug.

### E3. Fix disagreements

For each disagreement:
1. Reproduce minimal case
2. Determine which analyzer is correct (usually the one matching spec)
3. Fix the other
4. Re-run

Expected disagreements: ~10-30 bugs. Each ~30-60 minutes to fix.

### E4. Convergence check

Declare victory when:
- Zero disagreements across 1600+ programs
- 3 successive runs with different test orderings agree

**Exit criteria:** Full parity verified.

---

## Phase F — Cutover (~1 session)

**Goal:** delete zercheck.c.

### F1. Stop calling zercheck.c

In zerc_main.c: comment out zercheck.c invocation. Only zercheck_ir runs.

### F2. Full test suite verification

- `make check` — all tests must pass
- Run fuzzer for 1 hour

### F3. Delete files

```bash
git rm zercheck.c
# update zercheck.h — keep init/run API but point to zercheck_ir
# update Makefile — remove zercheck.c from source list
# update docs/compiler-internals.md — replace zercheck.c references
```

### F4. Final commit

```
Remove zercheck.c — CFG migration complete

All handle tracking, cross-func summaries, move struct, escape
detection, alloc coloring, interior pointers, *opaque 9a/9b/9c,
defer scanning, compound keys, keep params, ThreadHandle,
lock ordering, ISR bans, and ghost handle detection now live
in zercheck_ir.c.

Net: -2810 + ~1500 = -1310 lines overall. 0 "Partial" rows.
All tests green. Tagged as v0.5.0.
```

---

## Methodology improvements (beyond CFG)

Reduce bug count further via tooling — not tied to CFG migration.
Do in parallel or after.

### M1. Dual-run becomes permanent audit tool

Keep dual-run infrastructure even after Phase F. Add second implementation
(e.g., a simple reference implementation) for any future safety check.
Catches regressions immediately.

### M2. Mutation testing

Automated: delete a safety check, run tests, see if any fail. If none do,
the check is undertested. Identifies gaps.

Target: `tests/mutation_test.sh` that mutates emitter/checker/zercheck_ir
and verifies tests still catch expected failures.

### M3. Semantic fuzzer extension

Current: 32 generators, 200 tests per make check.

Add generators for:
- move struct + container combinations
- async + shared struct combinations
- `*opaque` through cinclude boundaries
- interior pointers through multiple levels of field access

Target: 48+ generators, 400 tests per make check.

### M4. Walker audit extension

Current: `tools/walker_audit.sh` checks AST emitter vs IR emitter
NODE_ coverage.

Add:
- Safety-emit coverage: grep `_zer_trap` / `_zer_bounds_check` / `_zer_shl`
  in AST path, verify each has IR equivalent
- Automatic check in CI

### M5. Eliminate fixed-size buffers

Per CLAUDE.md rule #7: one-time audit, convert all fixed buffers to
stack-first dynamic pattern.

Targets:
- zercheck.c `labels[32]` → dynamic (already deleted with Phase F)
- zercheck.c `scan_stack[32]` → dynamic (already deleted with Phase F)
- checker.c `stack_labels[128]` → dynamic
- Any remaining after Phase F

### M6. Comment-mode assertions

Pattern that caused Phase 3 regressions:
```c
/* Bounds checks are in the AST path */
emit(e, "%s[%s]", src, idx);  // no bounds check emitted
```

Replace with runtime assert:
```c
#ifdef ZER_ASSERT_SAFETY
    if (!ast_path_active) zer_assert_fail("bounds check expected");
#endif
```

Or compile-time static_assert where possible.

### M7. Real-code test suite expansion

CLAUDE.md lesson: 60-line HTTP server found bugs 1700 tests missed.

Commit to:
- 10 real programs in `examples/` covering feature interactions
- Each exercises 3+ features (e.g., HTTP server = async + shared + handle + defer)
- Run as part of `make check`

### M8. Flag-handler matrix audit extensions

Current: `tools/audit_matrix.sh` covers NODE_ × context flags in checker.c.

Extend to:
- Emitter: NODE_ × type_unwrap_distinct call sites
- zercheck_ir: IR ops × safety checks
- Any subsystem with feature-axes

### M9. Pre-release audit protocol automation

Pre-milestone script that runs:
- `bash tools/audit_matrix.sh checker.c zercheck_ir.c`
- `bash tools/walker_audit.sh`
- `bash tools/emit_audit.sh` (dead-stub grep on emitted C)
- Dual-run diff (if retained from Phase E)

Block release if any drift detected.

### M10. Property-based invariants

Define compiler invariants that must always hold:
- Every IR_POOL_ALLOC dest_local is used (else ghost handle)
- Every handle state transition is monotone (ALIVE → FREED, never reverse)
- Every slice access has either auto-guard or _zer_bounds_check
- Every signed division has zero check OR compile-time proof

Add as runtime assertions in debug builds, static analysis in release.

---

## 29-safety-systems framework — what CFG migration closes

Mapping each safety system to whether migration affects it:

| # | System | CFG migration impact |
|---|---|---|
| 1 | Typemap | unchanged |
| 2 | Type ID | unchanged |
| 3 | Provenance | unchanged |
| 4 | Prov Summaries | unchanged |
| 5 | Param Provenance | unchanged |
| 6 | Alloc Coloring | moves to zercheck_ir (Phase D1) |
| 7 | Handle States | moves to zercheck_ir (Phase B) |
| 8 | Alloc ID | moves to zercheck_ir (Phase B3 compound keys) |
| 9 | Func Summaries | moves to zercheck_ir (Phase C1) |
| 10 | Move Tracking | moves to zercheck_ir (Phase B1) |
| 11 | Escape Flags | moves to zercheck_ir (Phase B2) |
| 12 | Range Propagation | future: vrp_ir.c already 349 lines foundation |
| 13 | Return Range | future: vrp_ir.c |
| 14 | Auto-Guard | stays as emit_auto_guards separate pass |
| 15 | Dynamic Freed | moves to zercheck_ir (Phase B) |
| 16 | Non-Storable | unchanged (checker-time) |
| 17 | ISR Tracking | partial move to zercheck_ir (Phase D5) |
| 18 | Stack Frames | unchanged |
| 19 | MMIO Ranges | unchanged |
| 20 | Qualifier Tracking | unchanged |
| 21 | Keep Parameters | moves to zercheck_ir (Phase D2) |
| 22 | Union Switch Lock | unchanged |
| 23 | Defer Stack | unchanged |
| 24 | Context Flags | unchanged |
| 25 | Container Templates | unchanged |
| 26 | Comptime Evaluator | unchanged |
| 27 | Spawn Global Scan | possibly moves (Phase D — covered by shared type detection) |
| 28 | Shared Type Collect | partial (Phase D4 lock ordering) |
| 29 | FuncProps | unchanged |

**Summary:** 13 systems (#6, 7, 8, 9, 10, 11, 15, 17, 21, 27, 28) move
or partially move to zercheck_ir. 16 systems unchanged. Future VRP
migration (vrp_ir.c) covers #12, 13.

---

## Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Regression in subtle zercheck.c behavior not caught by existing tests | High | Medium | Dual-run in Phase E; add test cases during migration |
| FuncSummary cross-module order-of-init issues | Medium | Medium | Port zercheck.c's topological sort logic carefully |
| Arena wrapper chain inference edge cases | Medium | Low | Preserve exact heuristic from zercheck.c |
| *opaque 9a/9b/9c complexity | High | Medium | Port zercheck.c logic verbatim initially, refactor later |
| Dual-run doubles check time | Low | Low | Temporary; removed at Phase F |
| Scope creep — adding features during migration | Medium | High | Strict rule: migration ports only. New features after v0.5 |
| LLM training data gap on CFG analysis | Low | Low | Plan is thorough enough that fresh sessions can execute phase-by-phase |
| Loss of context between sessions | Medium | Medium | Commit per phase with detailed message. Update this doc with phase completion. |
| Undiscovered dependency between features | Medium | Medium | Port in dependency order (B3 before C2, etc.) |

---

## Verification strategy per phase

Every phase MUST satisfy before merging:

1. **Build passes** — `make zerc`
2. **Unit tests pass** — `make check` green
3. **No new warnings** — compile with `-Wall -Wextra`
4. **No performance regression** — `make check` time within 10% of baseline
5. **Dual-run diff** (from Phase B onward) — zero disagreements on test suite
6. **Doc update** — this plan's phase section marked [DONE] with commit ref

---

## Session pacing guidelines

**Per session target:** 1-2 sub-features (e.g., B1 + B2 or just C1)

**Do NOT attempt:**
- Multiple phases in one session
- Features without a test plan
- Refactoring beyond what's needed for the port
- Adding new features during migration (scope creep)

**Commit cadence:** one commit per sub-feature. Descriptive message
with "CFG migration — <sub-feature>" prefix.

---

## Success criteria — when is this done?

- [ ] `zercheck.c` deleted from git
- [ ] `zercheck_ir.c` passes every test zercheck.c passed
- [ ] 1600+ programs show identical analyzer output in dual-run
- [ ] Zero "Partial" rows in any capability table
- [ ] All 7 Phase 1 gaps fixed or explicitly closed
- [ ] `docs/limitations.md` updated to reflect completion
- [ ] `BUGS-FIXED.md` has session entries for every bug found during migration
- [ ] Tagged as v0.5.0

---

## Dependency graph (what must happen in what order)

```
Phase A (quick wins) ──────────> ship immediately, no deps

Phase B1 (move struct) ─────────┐
Phase B2 (escape) ──────────────┼──> Phase B exit
Phase B3 (compound keys) ───────┘

Phase B3 ──┬──> Phase C1 (FuncSummary, needs compound keys) ──┐
           └──> Phase C2 (*opaque, needs compound + summary) ─┤
Phase B  ──────> Phase C3 (defer scanning, needs handle track)┤
                                                               │
                                                               v
                                                        Phase C exit

Phase B ─────> Phase D1 (alloc coloring) ──┐
Phase B ─────> Phase D2 (keep params) ─────┤
Phase B ─────> Phase D3 (ThreadHandle) ────┤
Phase B ─────> Phase D4 (lock ordering) ───┤──> Phase D exit
Phase B ─────> Phase D5 (ISR bans) ────────┤
Phase B ─────> Phase D6 (ghost handle) ────┤
Phase C1 ────> Phase D7 (arena chain) ─────┘

Phase D exit ─────> Phase E (dual-run verification)

Phase E exit ─────> Phase F (delete zercheck.c) = v0.5.0
```

---

## Progress tracking

Update this section as phases complete.

- [ ] Phase A1 — Gap 3 fix
- [ ] Phase A2 — Gap 7 fix
- [ ] Phase A3 — `_scan_depth` bump
- [x] Phase A4 — this plan document (committed)
- [ ] Phase A5 — VSIX 0.4.6 release

- [ ] Phase B1 — Full move struct tracking
- [ ] Phase B2 — Full escape detection
- [ ] Phase B3 — Compound key tracking

- [ ] Phase C1 — FuncSummary system
- [ ] Phase C2 — *opaque 9a/9b/9c
- [ ] Phase C3 — Defer body scanning

- [ ] Phase D1 — Alloc coloring
- [ ] Phase D2 — Keep parameter validation
- [ ] Phase D3 — ThreadHandle join tracking
- [ ] Phase D4 — Lock ordering + deadlock
- [ ] Phase D5 — ISR bans
- [ ] Phase D6 — Ghost handle detection
- [ ] Phase D7 — Arena wrapper chain inference

- [ ] Phase E — Dual-run verification
- [ ] Phase F — Delete zercheck.c, tag v0.5.0

**Methodology improvements (parallel):**

- [ ] M1 — Permanent dual-run tooling
- [ ] M2 — Mutation testing
- [ ] M3 — Semantic fuzzer extensions
- [ ] M4 — Walker audit extensions
- [ ] M5 — Fixed-buffer elimination
- [ ] M6 — Comment-mode assertions
- [ ] M7 — Real-code test suite expansion
- [ ] M8 — Flag-handler matrix audit extensions
- [ ] M9 — Pre-release audit automation
- [ ] M10 — Property-based invariants

---

## Total effort estimate

- Phase A: 1 session (quick wins) — ~2 hours
- Phase B: 2-3 sessions (~500 lines) — ~12 hours
- Phase C: 3 sessions (~500 lines, hardest) — ~18 hours
- Phase D: 2 sessions (~400 lines) — ~10 hours
- Phase E: 1-2 sessions (test diff iteration) — ~6 hours
- Phase F: 1 session (cutover) — ~3 hours
- Methodology items: ongoing, 1 session each — ~20 hours

**Total: ~70 hours of focused work, ~11 sessions.**

Timeline if 1 session per week: 3 months to v0.5.0.
Timeline if 2 sessions per week: 6 weeks to v0.5.0.

---

## Why this plan reduces bugs long-term

The migration itself closes specific bugs (Gap 1, Gap 2, Gap 5,
Gap 6 foundation). The methodology improvements are the bigger
lever — they catch future bugs automatically:

- **M1 dual-run** — any analyzer regression caught immediately
- **M2 mutation testing** — undertested code surfaced
- **M3 fuzzer** — feature-combination bugs surfaced
- **M4 walker audit** — safety-emit regressions caught at commit time
- **M7 real code** — category of bugs unit tests can't catch
- **M9 pre-release audit** — drift blocked at release boundary
- **M10 property invariants** — semantic bugs blocked at runtime

Each of these has proven impact: M4 + M9 types of audit caught 13
bugs in the 2026-04-19 session alone. Making them permanent +
automated means future bugs get caught at commit time, not months
later.

---

## Not a plan for "zero bugs"

Honest framing: no plan achieves zero bugs. This plan targets:

- **Eliminate** specific known gaps (Phase 1 open items)
- **Reduce** future bug rate via methodology (M1-M10)
- **Prevent** regression via automated audits
- **Simplify** the codebase (CFG is cleaner than linear walk + hacks)

Residual bugs will still occur. What the plan provides is faster
detection, cleaner fixes, and fewer re-regressions.

---

# Part 2 — Implementation detail (deep dive)

After the high-level plan, deeper inspection of actual code revealed
concrete file:line anchors. This part provides implementation-level
detail needed to execute each phase without further planning.

## Current pipeline — exact wiring

**File:** `zerc_main.c:494-507`

```c
ZerCheck zc;
zercheck_init(&zc, &checker, &cc.arena, input_path);
// ... imports ...
if (!zercheck_run(&zc, main_mod->ast)) {
    fprintf(stderr, "zercheck failed\n");
    return 1;
}
```

Current call site is the only hook. No other source file calls
`zercheck_*`. The cutover at Phase F is ONE edit.

**Current Makefile** (`Makefile:~5`):

```
CORE_SRCS = lexer.c parser.c ast.c types.c checker.c emitter.c \
            zercheck.c ir.c ir_lower.c zerc_main.c
LIB_SRCS  = lexer.c parser.c ast.c types.c checker.c emitter.c \
            zercheck.c ir.c ir_lower.c
```

Neither `zercheck_ir.c` nor `vrp_ir.c` are in Makefile. Adding them
is step 1 of Phase B.

**Current `zercheck_ir(zc, func)` signature:**

```c
bool zercheck_ir(ZerCheck *zc, IRFunc *func);
```

Takes an `IRFunc*`, not a `Node*` like `zercheck_run`. Migration
wrapper needs to lower each function to IR, call `zercheck_ir` on
each, then roll up results.

---

## zercheck.c function map — what ports to where

| zercheck.c function | Lines | Destination | Phase |
|---|---:|---|---|
| zc_error / zc_warning | 15-34 | Already ported (ir_zc_error) | — |
| pathstate_init/copy/free/equal | 38-105 | Already ported (ir_ps_*) | — |
| find_handle / find_handle_local / add_handle | 78-128 | Already ported | — |
| find_pool_id / register_pool | 129-170 | Not needed (IR uses local_id) | — |
| handle_key_from_expr | 172-213 | Replaced by compound key struct | B3 |
| handle_key_arena | 215-225 | Not needed | — |
| is_move_struct_type / contains_move_struct_field / should_track_move | 920-962 | Port verbatim (type helpers) | B1 |
| is_alloc_call / is_free_call | 237-310 | Replaced by IR op dispatch | — |
| block_always_exits | 312-341 | Not needed (CFG terminated flag) | — |
| defer_stmt_is_free / defer_scan_all_frees | 343-390 | Port for IR_DEFER_FIRE scanning | C3 |
| zc_check_call | 397-536 | Split across IR op handlers | B/C/D |
| func_returns_color_by_name | 540-633 | Port — arena wrapper chain | D7 |
| zc_check_var_init | 647-918 | Fold into IR_ASSIGN handler | B |
| is_handle_invalid / is_handle_consumed | 963-976 | Already ported | — |
| zc_report_invalid_use | 978-1019 | Port as helper | B |
| zc_check_expr | 1021-1617 | Decompose into per-IR-op handlers | B/C/D |
| zc_check_stmt | 1619-2244 | Decompose into per-IR-op handlers | B/C/D |
| find_summary / zc_build_summary / zc_apply_summary | 2246-2491 | Port as zc_ir_build_summary etc | C1 |
| zc_check_function | 2493-2727 | Port as per-function IR orchestration | all |
| zercheck_init / zercheck_run | 2729-2810 | Rewrite — drive IR lowering per func | E/F |

Total phases consume all 2810 lines. Nothing uncategorized.

---

## zc_check_expr — 598 lines mapped to IR ops

| AST NODE_ case | IR op that replaces it | Handler additions |
|---|---|---|
| NODE_IDENT | IR_ASSIGN (RHS) | existing — check handle validity |
| NODE_CALL | IR_CALL + IR_POOL_* + IR_SPAWN | split by callee type |
| NODE_ASSIGN | IR_ASSIGN / IR_FIELD_WRITE / IR_INDEX_WRITE | escape checks (B2) |
| NODE_BINARY | IR_BINOP | no handle logic |
| NODE_FIELD | IR_FIELD_READ | compound key tracking (B3) |
| NODE_INDEX | IR_INDEX_READ | compound key for array elements (B3) |
| NODE_UNARY (deref) | IR_DEREF_READ | handle UAF check via parent |
| NODE_UNARY (&x) | IR_ADDR_OF | interior pointer alloc_id propagation (C2 prereq) |
| NODE_INTRINSIC @ptrcast | IR_INTRINSIC / IR_CAST | *opaque provenance (C2) |
| NODE_ORELSE | IR_ORELSE_DECOMP | orelse fallback escape (B2) |
| NODE_TYPECAST | IR_CAST | same as ptrcast |
| NODE_STRUCT_INIT | IR_STRUCT_INIT_DECOMP | struct return escape (B2) |
| NODE_SLICE | IR_SLICE_READ | no handle logic |

## zc_check_stmt — 627 lines mapped

| AST NODE_ case | IR path replacement |
|---|---|
| NODE_BLOCK | CFG joins handle scope via terminated flag |
| NODE_VAR_DECL | IR_ASSIGN with dest_local classification |
| NODE_IF | IR_BRANCH + two successor blocks + join — CFG merge natural |
| NODE_FOR/WHILE/DO_WHILE | Back-edge CFG loops — fixed-point exists |
| NODE_RETURN | IR_RETURN — extend with leak/escape checks |
| NODE_DEFER | IR_DEFER_PUSH — C3 scans body |
| NODE_CRITICAL | IR_CRITICAL_BEGIN/END — context flag for ISR bans (D5) |
| NODE_ONCE | AST passthrough — no handle logic |
| NODE_SWITCH | Arm bodies each IR blocks, merged at exit |
| NODE_SPAWN | IR_SPAWN — already handles TRANSFERRED |
| NODE_GOTO/BREAK/CONTINUE | IR_GOTO — CFG edges |
| NODE_YIELD/AWAIT | IR_YIELD/IR_AWAIT — existing |
| NODE_LABEL | Block boundary — already handled |

---

## Phase B1 — worked example for session execution

### Session agenda (3-4 hours)

1. **Read** (15 min):
   - zercheck.c:920-961 (move struct helpers)
   - zercheck.c:1712-1849 (move transfer in var_decl)
   - zercheck_ir.c:287-314 (current IR_ASSIGN handler)

2. **Port helpers** (30 min):
   - Copy `is_move_struct_type`, `contains_move_struct_field`,
     `should_track_move` to zercheck_ir.c verbatim.
   - No changes needed — they operate on `Type*`, same in both.

3. **Extend IR_ASSIGN** (1 hour):
   - After existing alias logic, check source local type.
   - If `should_track_move(src_type)`:
     - Mark src handle TRANSFERRED.
     - Mark dst handle ALIVE (new ownership).
     - Set dst.alloc_id = fresh id.

4. **Add IR_RETURN move transfer** (30 min):
   - Check if return value is move struct ident.
   - If yes, mark source TRANSFERRED.

5. **Add IR_FIELD_WRITE handler** (30 min):
   - New case in switch.
   - If target field type is move struct AND source is tracked:
     - Mark source TRANSFERRED.

6. **Wire up** (15 min):
   - Add `#include "zercheck_ir.h"` where needed.
   - Add call to `zercheck_ir` in parallel with `zercheck_run` (dual-run stub).
   - `make zerc` — must compile.

7. **Test** (1 hour):
   - Run `rt_move_*.zer` — must still pass.
   - Run `gap5_container_move.zer` → should now fail when zercheck_ir runs.
   - Add unit test for move transfer through container field.

8. **Commit:**

```
CFG migration Phase B1: full move struct tracking in zercheck_ir

- is_move_struct_type / contains_move_struct_field helpers ported
- IR_ASSIGN marks source TRANSFERRED, dest ALIVE on move
- IR_RETURN marks returned move struct TRANSFERRED
- IR_FIELD_WRITE handles container field transfer (closes Gap 5)
- Dual-run infrastructure: both analyzers run, zercheck.c primary
- gap5_container_move.zer promoted to tests/zer_fail/
```

**Net:** 150 lines added, 1 gap closed, phase B 33% complete.

---

## Compound keys — design decision for Phase B3

Two approaches, pick ONE:

**Approach A — Compound key struct (recommended):**

```c
typedef enum {
    IRHK_LOCAL,         /* just a local */
    IRHK_FIELD,         /* parent_local.field_name */
    IRHK_INDEX,         /* parent_local[const_index] */
    IRHK_FIELD_CHAIN,   /* a.b.c... */
} IRHKKind;

typedef struct IRCompoundKey {
    IRHKKind kind;
    int parent_local;
    struct IRCompoundKey *parent_key;
    const char *field_name;
    uint32_t field_name_len;
    int64_t const_index;
} IRCompoundKey;
```

Handle tracking keyed on `IRCompoundKey*` instead of `int local_id`.

**Approach B — Keep string keys (pragmatic):**

Port `handle_key_from_expr` from zercheck.c:172-213. String comparison
for handle match.

**Recommendation:** Approach A. String comparison is O(n) per lookup;
struct comparison is O(1). Over 1000+ tests the difference matters.
A also avoids the 128-char buffer limit zercheck.c has.

---

## Dual-run verification architecture (Phase E)

Add to zerc_main.c:

```c
#ifdef DUAL_RUN
    ZerCheck zc_ast, zc_ir;
    zercheck_init(&zc_ast, &checker, &cc.arena, input_path);
    zercheck_init(&zc_ir, &checker, &cc.arena, input_path);

    bool ast_ok = zercheck_run(&zc_ast, main_mod->ast);

    bool ir_ok = true;
    for each func_decl in main_mod->ast:
        IRFunc *ir = ir_lower_func(&cc.arena, &checker, func_decl);
        if (!zercheck_ir(&zc_ir, ir)) ir_ok = false;

    if (zc_ast.error_count != zc_ir.error_count) {
        fprintf(stderr, "ANALYZER DISAGREEMENT: ast=%d, ir=%d\n",
                zc_ast.error_count, zc_ir.error_count);
    }
#endif
```

Build with `make DUAL_RUN=1`.

**`tools/dual_run_audit.sh`:**

1. Run `make DUAL_RUN=1 check` — logs both analyzer outputs.
2. Diff per-file.
3. Output disagreement report.

**Convergence criteria:**

- Zero disagreements across ~1800 programs
- 3 successive runs agree
- Real-code examples in `examples/` agree

---

## Phase F cutover sequence (single commit)

Precondition: Phase E zero disagreements.

1. `zerc_main.c:494-507` — replace `zercheck_run` with driver that
   lowers each function, calls `zercheck_ir`, rolls up errors.

2. `Makefile` — remove `zercheck.c` from `CORE_SRCS` + `LIB_SRCS`.
   Add `zercheck_ir.c` to both.

3. `git rm zercheck.c`

4. `zercheck.h` — remove AST-walker-specific types (`PathState`,
   internal helpers). Keep public API.

5. `make check` — must stay green.

6. Commit:
   ```
   Delete zercheck.c — CFG migration complete, v0.5.0
   ```

**Tag as v0.5.0.**

---

## VRP migration (parallel track)

`vrp_ir.c` (349 lines) is Phase 7 foundation. Same pre-migration
status as zercheck_ir. Migration independent of handle-tracking but
shares infrastructure (fixed-point iteration, CFG walking).

**Current state:**
- `vrp_ir(IRFunc*)` function exists
- Returns `IRVRPResult*` with proven-safe index entries
- Not wired into emitter's `emit_auto_guards` pass

**Integration point:** `emitter.c:254, 280` — `emit_auto_guards`.
Currently uses checker's per-variable range state. Migration:

1. Run `vrp_ir(func)` after `ir_lower_func` in emit_func_decl path.
2. `emit_auto_guards` consults `IRVRPResult` instead of checker ranges.
3. Delete checker's VarRange logic.

**Estimated:** ~300 lines. Same phase pattern.

---

## FuncProps integration — do NOT duplicate

**Existing system** (checker.c:6093+): FuncProps tracks `can_yield`,
`can_spawn`, `can_alloc`, `has_sync` per function via lazy DFS
propagation.

This is SEPARATE from FuncSummary (handle free tracking). Keep both.
FuncProps stays in checker.c (pre-IR); FuncSummary moves to
zercheck_ir.c.

**Integration point at IR_CALL:**

```c
Symbol *callee = scope_lookup(global_scope, name, len);
if (callee && callee->props.can_alloc) { /* ... */ }
```

Same access works from zercheck_ir.c. Do not port FuncProps.
Reference it instead.

---

## Context flag inheritance in IR

Checker's context flags (`in_async`, `defer_depth`, `critical_depth`,
`in_interrupt`) stored on `Checker*`. For zercheck_ir, available
per-block.

**Add per-block context to IRBlock:**

```c
typedef struct {
    /* existing fields */

    /* Inherited from control-flow context */
    bool in_interrupt;   /* inside interrupt handler */
    bool in_critical;    /* inside @critical {} */
    bool in_async;       /* function is async */
    bool in_defer;       /* inside defer body */
} IRBlock;
```

**Set during IR lowering:**

- `ir_lower_interrupt` → `in_interrupt=true` on all blocks it creates.
- `IR_CRITICAL_BEGIN` → `in_critical=true` on subsequent blocks until IR_CRITICAL_END.
- `func->is_async` → `in_async=true` on all blocks.
- `IR_DEFER_PUSH` body traversal → `in_defer=true`.

zercheck_ir reads these at check time. No checker context needed.

---

## Per-feature test mapping — what each phase unlocks

| Gap | Reproducer | Closed by | Target when fixed |
|---|---|---|---|
| Gap 1 | gap1_cross_block_goto.zer | B + Phase F (CFG natural) | tests/zer_fail/ |
| Gap 2 | gap2_same_line_uaf.zer | B + Phase F (statement IDs) | tests/zer_fail/ |
| Gap 3 | gap3_yield_outside_async.zer | Phase A1 (pre-CFG) | tests/zer_fail/ |
| Gap 4 | gap4_async_shared_across_yield.zer | NOT by CFG (spec review) | TBD |
| Gap 5 | gap5_container_move.zer | B1 (move struct port) | tests/zer_fail/ |
| Gap 6 | gap6_goto_into_capture.zer | C separate (reachability) | tests/zer_fail/ |
| Gap 7 | gap7_defer_in_defer.zer | Phase A2 (pre-CFG) | tests/zer_fail/ |
| Prec 1 | prec1_vrp_literal_i.zer | VRP migration | tests/zer/ (positive) |
| Prec 2 | prec2_opaque_wrong_type.zer | C2 (opaque refinement) | tests/zer_fail/ |
| audit2 slice bounds x3 | Already fixed | tests/zer_trap/ done |
| audit2 cross-block goto x3 | B + CFG | tests/zer_fail/ |
| audit2 defer scan nested | C3 | TBD |
| audit2 funcsummary chain | Works today (positive) | keep as positive coverage |
| audit2 nested if chain | Works today | keep |
| audit2 switch partial | Works today | keep |
| audit2 spawn transitive | Phase A3 (cap bump) | tests/zer_fail/ at depth >= 32 |
| ast_slice_empty_range | Already fixed | tests/zer_trap/ done |
| ast_signed_div_overflow | Already fixed | tests/zer_trap/ done |
| ast_shift_over_width | Already fixed | tests/zer/ positive done |
| ast_inttoptr_mmio | Already fixed | tests/zer_trap/ done |
| ast_inttoptr_align | Already fixed | tests/zer_trap/ done |

**31 reproducers total.** At end of Phase F, ~20 move to zer_fail/
or zer_trap/, ~5 stay as positive coverage, ~5 may be re-classified.

---

## Out of scope (explicit)

Keep scope bounded. NOT in this plan:

- Rewrite checker.c (11077 lines) — unrelated. Stays AST-based.
- Rewrite emitter.c (8037 lines) — unrelated. IR-native already.
- Cross-module whole-program analysis — stays per-function.
- Runtime information access (`*opaque` type IDs) — runtime-only.
- New language features during migration — strict no-scope-creep.
- Performance optimization — v0.6+.
- Self-hosting (zer-src/) — v1.0 concern.

---

## Plan completeness self-audit

Does this plan have what a fresh session needs to execute Phase B1
without further planning? Checklist:

- Exact source file:line references: yes
- Exact destination hook points: yes
- Expected code changes quantified: yes
- Dependencies on other phases stated: yes
- Tests unlocked identified: yes
- Commit message template: yes
- Estimated session time: yes
- Verification criteria: yes

Same checklist applies to B2, B3, C1, C2, C3, D1-D7, E, F.

**This plan is executable as written.** No further planning required
to start Phase A1. No further planning required to start Phase B1.
Each subsequent phase can be planned in detail at execution time
using this document as a reference.
