# Session G Phase 5 Plan — Re-Audit (2026-05-02, revised)

Re-audit of `docs/session_g_phase5_plan.md`. Goal: find anything wrong, missing, or under-specified before implementation begins. Verified each factual claim against current code.

**User clarification (2026-05-02)**: "We don't use zercheck.c — we fully use zercheck_ir.c."

This means Phase 5 should be planned with zercheck_ir as the sole/primary safety driver. Either as part of Phase 5 work, or via the existing CFG migration plan (Phase F per `docs/cfg_migration_plan.md`, called "Phase G" in CLAUDE.md — same thing, naming inconsistency).

---

## CRITICAL: User intent vs current code state

### Current code state (verified 2026-05-02)

`zercheck.c` IS still active (155K LOC, called at `zerc_main.c:626`):

```c
bool ast_ok = zercheck_run(&zc, main_mod->ast);   // zercheck.c (AST)
if (!ast_ok) return 1;                            // gates compile
/* ... emission, IR hook fires zercheck_ir for each fn ... */
int ast_err = zc.error_count;
int ir_err = zc_ir.error_count;
bool agree = (ast_err==0 && ir_err==0) || (ast_err>0 && ir_err>0);
if (!agree) fprintf(stderr, "DUAL-RUN disagreement");
/* ir_err is NEVER checked for compile failure */
```

`zercheck_ir.c` errors print to stderr but DON'T gate compile. The `ast_ok` from zercheck.c is the sole compile gate.

### User's intent

Per the user message, the project considers zercheck.c deprecated. Phase 5 should be designed assuming zercheck_ir is primary.

### What this means for Phase 5

Either:

**Option A (full cutover)**: Phase G/F migration before Phase 5. Delete zercheck.c, make zercheck_ir primary. ~3 hrs per cfg_migration_plan.md.

**Option B (gate without delete)**: Add `if (zc_ir.error_count > 0) return 1;` in zerc_main.c so IR errors gate compile. Keep zercheck.c as belt-and-suspenders. ~30 min.

**Option C (separate counter)**: Add `phase5_error_count` to ZerCheck so only Phase 5-specific errors gate compile, leaving existing IR errors as informational. ~1 hr.

**Recommendation**: Option B. Reasoning:
- Dual-run has 0 disagreements across 3143 programs (per CLAUDE.md). Making IR errors gate compile won't break existing tests.
- Doesn't require deleting zercheck.c (still battle-tested fallback).
- Aligns with user intent ("fully use zercheck_ir").
- Cheaper than Option A (no file deletion / Makefile changes / etc.).
- More aggressive than Option C (one principle: "if zercheck_ir says no, compile fails").

If user prefers Option A, that's fine too — Phase F per cfg_migration_plan.md is well-documented (~3 hrs). Phase 5 doesn't strictly require it; just needs the gating.

---

## Issues found in the original plan

### Issue #1 [CRITICAL → resolved per user clarification]

**Problem**: As written, Phase 5 errors fire only from zercheck_ir, which doesn't gate compile.

**Resolution**: per user clarification, plan assumes zercheck_ir errors gate compile. Implementation:

```c
/* zerc_main.c, after the dual-run agreement check */
if (zc_ir.error_count > 0 && zc.error_count == 0) {
    /* zercheck_ir caught something AST analyzer missed.
     * Honor the IR finding — fail compile. */
    fprintf(stderr, "error: IR safety check failed (%d errors)\n",
            zc_ir.error_count);
    free(cc.modules);
    arena_free(&cc.arena);
    return 1;
}
```

Or simpler: any IR error fails compile, regardless of AST result:

```c
if (zc_ir.error_count > 0) return 1;
```

Update plan section 7 — zerc_main.c becomes a touched file (+5 LOC).

### Issue #2 [confirmed]: `nlen == 17` typo for @barrier_acq_rel

**Verified at `emitter.c:6307`**:
```c
} else if (nlen == 15 && memcmp(name, "barrier_acq_rel", 15) == 0) {
    emit(e, "__atomic_thread_fence(__ATOMIC_ACQ_REL)");
}
```

Length is **15**, not 17. Plan section 5.2 had typo. Also confirmed `@barrier_acq_rel` IS a real intrinsic implemented in checker.c:5968 + emitter.c:6307.

**Action**: fix typo in plan; intrinsic is real and ready to classify.

### Issue #3 [missing]: IR_LOCK / IR_UNLOCK as barriers

**Verified at `emitter.c:576`**: IR_LOCK emits `pthread_mutex_lock(&...)`.

POSIX guarantees `pthread_mutex_lock`/`unlock` provide acquire/release fence semantics. Conservative classification:
- IR_LOCK → PRODUCES FULL_MEMORY (or ACQUIRE)
- IR_UNLOCK → PRODUCES FULL_MEMORY (or RELEASE)

Recommendation: classify both as FULL_MEMORY for v1. Refinement to ACQUIRE/RELEASE in Phase 6 if needed.

**Verified at `zercheck_ir.c:2734`**: IR_LOCK and IR_UNLOCK currently fall through to no-op in `ir_check_inst`'s exhaustive switch. Adding ordering tracking is straightforward.

### Issue #4 [missing]: IR_YIELD / IR_AWAIT semantics

In ZER's stackless coroutine model (Duff's device per CLAUDE.md), local state is preserved in a state struct across suspend. CPU cache state and pending fences also persist (no kernel context switch for stackless coroutines).

**Decision**: yield/await are NEUTRAL for ordering — pending barriers carry forward across suspend. Don't add or clear barrier state.

**Action**: document in plan; no code change for IR_YIELD/IR_AWAIT.

### Issue #5 [missing]: Dead-code path handling

`IRBlock` has `is_orelse_fallback` and `is_early_exit` flags from `ir.h:207-224`.

zercheck_ir already special-cases these for leak detection. Phase 5 should skip them at the function-exit pending check too — orelse fallback paths return immediately and shouldn't have their pending counted as a function-exit violation.

**Action**: at exit-pending check, skip blocks with `is_orelse_fallback || is_early_exit`. Add to plan section 5.6.

### Issue #6 [resolved]: Convergence proof

Hand-traced for fixed-point convergence:

- `barriers_seen` is a 12-bit bitmap. INTERSECTION at joins is monotonic decreasing in fixed-point. UNION within block is monotonic increasing.
- `pending` is bounded at 32 entries (with `MAX_PENDING` cap). UNION at joins is monotonic increasing. DISCHARGE within block is monotonic decreasing on PRODUCES events.
- Lattice height: `2^12 × 2^32` per block × N blocks. Finite.
- Fixed-point on monotonic function over finite lattice converges in finite steps.
- Worst case: bounded by existing `MAX_ITERATIONS = 32`.

**Critical detail**: REQUIRES_AFTER means strictly after (verified Intel SDM CLWB rule). When adding a pending, do NOT auto-discharge based on PRIOR PRODUCES — only future PRODUCES count.

**Loop convergence**: traced manually for the canonical CLWB-in-loop pattern. Converges correctly. Pending CLWB at last iteration's body end propagates to function exit; if no SFENCE follows the loop, error fires correctly.

**Action**: keep `MAX_ITERATIONS = 32` cap. Fail-closed if exceeded. Document the manual trace.

### Issue #7 [limitation]: Extern functions don't get FuncSummary

A `cinclude`'d C function or extern signature has no FuncSummary entry. Calling it returns `barriers_produced = 0` regardless of what the C function actually does.

```
i32 atomic_thread_fence(...);     // C-declared, in cinclude

void f() {
    asm { instructions: "clwb (%0)" ... }
    atomic_thread_fence(...);     // C call — Phase 5 doesn't recognize as barrier
}
```

Phase 5 fires false-positive error.

**Resolution**: document as known limitation. Users should use ZER's `@atomic_*` / `@barrier_*` intrinsics. Phase 6+ can add `@barrier_extern` annotation for marking extern function declarations as barriers.

**Action**: add to plan section 13 (out-of-scope) and add a positive test that uses ZER intrinsics, NOT extern C calls.

### Issue #8 [missing]: IR_SPAWN as barrier

`pthread_create` provides a memory barrier on the calling thread. IR_SPAWN emits pthread_create.

```
asm { instructions: "clwb (%0)" ... }
spawn worker();   // pthread_create acts as full barrier on caller
```

The CLWB's pending should be discharged by the spawn.

**Verified at `zercheck_ir.c:2545`**: IR_SPAWN handler exists; can extend with ordering effect.

**Action**: classify IR_SPAWN as PRODUCES FULL_MEMORY (calling thread). Add test.

### Issue #9 [resolved]: @critical / IR_CRITICAL_BEGIN/END

`cli` on x86 / `cpsid i` on ARM disable interrupts but are NOT memory barriers.

**Action**: IR_CRITICAL_BEGIN / IR_CRITICAL_END are NEUTRAL for ordering. No tracking change.

### Issue #10 [risk acknowledged]: MAX_ITERATIONS may need raising

Current 32-iteration cap may not suffice for complex programs with both handle and ordering state. If real programs hit it, raise to 64.

**Action**: keep 32 for v1, fail-closed if exceeded. Monitor; raise if needed.

### Issue #11 [verify]: IR_INTRINSIC_DECOMP coverage

ZER lowers some intrinsics inline (NODE_INTRINSIC passthrough → IR_ASSIGN with expr) and some as decomposed (IR_INTRINSIC_DECOMP with arg locals). Phase 5 must handle both.

**Verified by reading code**: both IR_ASSIGN (with expr→NODE_INTRINSIC) and IR_INTRINSIC_DECOMP have access to the AST `intrinsic.name`. The decomposed version stores the call's evaluated args separately but still has the AST node for emission.

**Action**: in Phase 5, handle both at appropriate switch cases. Mention both in plan section 5.5.

### Issue #12 [resolved]: Building summary phase suppresses errors

`zc->building_summary` flag suppresses error output. zercheck_ir already respects this via the existing `ir_zc_error` helper:

```c
static void ir_zc_error(ZerCheck *zc, int line, const char *fmt, ...) {
    if (zc->building_summary) return;
    /* ... */
}
```

Phase 5 errors should use this same helper (or a sibling that also increments a phase5 counter). Errors fire only after `building_summary = false` (final main pass).

**Action**: ensure Phase 5 error helper checks `building_summary` first.

### Issue #13 [spec]: Where exit-pending check runs

Plan section 5.6 says "after final pass." Specific location:

`zercheck_ir.c` flow:
1. Fixed-point loop with `building_summary = true` (errors suppressed) — lines 2778-2845.
2. Final pass with errors enabled — lines 2851-2884.
3. Summary building (frees_param, returns_color, etc.) — lines 2924+.

Phase 5 exit-pending check goes in step 3 region, walking exit blocks of `block_states` and reporting pending. Pseudocode:

```c
/* After step 2 (final pass), before/within step 3 (summary build): */
for (int bi = 0; bi < func->block_count; bi++) {
    IRBlock *bb = &func->blocks[bi];
    if (bb->inst_count == 0) continue;
    if (bb->is_orelse_fallback || bb->is_early_exit) continue;
    IRInst *last = &bb->insts[bb->inst_count - 1];
    if (last->op != IR_RETURN) continue;
    IROrderingState *o = &block_states[bi].ordering;
    for (int pi = 0; pi < o->pending_count; pi++) {
        ir_zc_error(zc, o->pending[pi].origin_line, ...);
    }
}
```

### Issue #14 [UX]: Error message clarity

Plan's error message:
> "asm 'clwb' requires barrier subsequently but no satisfying barrier reaches function exit on this path"

This points at the CLWB site. User sees the error and immediately knows: missing SFENCE downstream. Good enough.

Alternative (cite both lines): more complex, marginal benefit. Skip.

**Action**: plan's error message is fine.

### Issue #15 [done]: FuncSummary extension via existing infrastructure

zercheck_ir.c already builds FuncSummary in step 3 of its analysis (lines 2924+). Adding `barriers_produced` field is a parallel computation:

```c
/* In summary building section, parallel to frees_param computation: */
uint32_t intersected = ~0u;  /* all bits */
bool found_exit = false;
for (int bi = 0; bi < func->block_count; bi++) {
    IRBlock *bb = &func->blocks[bi];
    if (bb->inst_count == 0) continue;
    if (bb->is_orelse_fallback || bb->is_early_exit) continue;
    IRInst *last = &bb->insts[bb->inst_count - 1];
    if (last->op != IR_RETURN) continue;
    intersected &= block_states[bi].ordering.barriers_seen;
    found_exit = true;
}
if (found_exit) {
    summary->barriers_produced = intersected;
}
```

**Action**: add as one block in summary build region. Reuse existing iterative summary loop in zerc_main.c (16-pass max).

### Issue #16 [confirmed]: Recursion handling for FuncSummary

Verified existing iterative summary loop (`zerc_main.c:700`):

```c
for (int pass = 0; pass < 16; pass++) {
    int sc_before = zc_ir.summary_count;
    for (int i = 0; i < zerc_ir_hook_count; i++) {
        zercheck_ir(&zc_ir, zerc_ir_hook_funcs[i]);
    }
    if (pass > 0 && zc_ir.summary_count == sc_before) break;
}
```

**KEY OBSERVATION**: The loop exits when `summary_count` is stable. It does NOT iterate to convergence on summary FIELD values. So if function f1 calls f2 calls f1 (cycle), and barrier production depends on cyclic refinement, the loop may exit prematurely.

For Phase 5: cycles produce pessimistic `barriers_produced = 0` (assume no barriers from cyclic calls). Conservative but correct.

**Action**: document recursive cycles as conservative (no barriers cross recursive calls). Acceptable limitation.

**(Note: this is also a pre-existing limitation for `frees_param` and `returns_color` — not new for Phase 5.)**

### Issue #17 [confirmed]: ARM tests should be cross-arch

Cross-arch tests live in `tests/test_cross_arch.sh`. ARM-specific positive tests for LDAR/STLR pairing should go there, not in `tests/zer/` (default x86_64).

**Action**: update plan section 8 — ARM tests go to cross-arch suite.

### Issue #18 [confirmed]: Asm scanner handles multi-line strings

The mnemonic walker in checker.c (lines 9998-10030) treats `\n` as instruction separator. Multi-line `asm { instructions: "clwb (%0)\nsfence" }` is properly tokenized.

**Action**: verify `asm_mnemonic_walk` helper preserves this. Add multi-line test.

### Issue #19 [new — found in re-read]: zerc_main.c iterative summary loop convergence subtlety

The summary loop breaks on `summary_count` stability, NOT on summary content stability. This means:

1. First-pass: f1 added with barriers_produced=A. f2 added with barriers_produced=B.
2. If f1 calls f2 and re-analyzing f1 would now produce different barriers_produced (because f2's barriers are now known), the second-pass would update f1.
3. If summary_count is stable (no new functions added) but f1's barriers_produced changes, loop exits.

For Phase 5 to be cross-function correct, summaries must be RE-COMPUTED at every pass even when count is stable.

**Resolution**: change loop condition to also detect summary CONTENT stability:

```c
for (int pass = 0; pass < 16; pass++) {
    int sc_before = zc_ir.summary_count;
    bool any_changed = false;
    /* compute "did any summary's barriers_produced change this pass?" */
    /* ... track via comparing before/after each summary update ... */
    for (int i = 0; i < zerc_ir_hook_count; i++) {
        zercheck_ir(&zc_ir, zerc_ir_hook_funcs[i]);
    }
    if (pass > 0 && !any_changed && zc_ir.summary_count == sc_before) break;
}
```

This requires tracking changes inside zercheck_ir. Less intrusive: just don't break early; run all 16 passes. Wastes time for simple programs but correct.

**Action**: minor change to zerc_main.c for Phase 5 correctness with cross-function ordering. Estimate: +1 hr.

### Issue #20 [new — found in re-read]: Phase 5 errors during summary-build vs main-pass

Currently:
- Summary-build phase: `building_summary=true`, errors suppressed.
- Main pass: `building_summary=false`, errors enabled.

If Phase 5 fires during summary-build (computing barriers_produced of f1 that has unsatisfied pending), the error is suppressed during summary build (correct — we'd otherwise emit the same error 16+ times).

But the FINAL main pass should re-trigger and emit the error once.

**Verification**: existing pattern works correctly because the pending state at function exit is recomputed in each pass. Final pass sees stable pending → fires error once.

**Action**: nothing to change. Existing pattern handles this.

### Issue #21 [new — limitation]: Same-name functions across modules

ZER has module mangling (`module__function` per CLAUDE.md). FuncSummary lookup uses unmangled name. Cross-module function calls might not match the correct summary.

**Verified at `zercheck_ir.c:2284-2289`**:
```c
for (int si = 0; si < zc->summary_count; si++) {
    if (zc->summaries[si].func_name_len == fn_name_len &&
        memcmp(zc->summaries[si].func_name, fn_name, fn_name_len) == 0) {
        summary = &zc->summaries[si]; break;
    }
}
```

Lookup is by `fn_name`. If two modules have a function with the same name, the first match wins. Could mismatch.

**Resolution**: pre-existing issue, not new for Phase 5. Module-mangled names should be used for lookup; same fix applies to all FuncSummary usages.

**Action**: document as pre-existing limitation. Don't fix in Phase 5 (out of scope).

### Issue #22 [new — verified]: dual-run disagreement reporter

Phase 5 errors are unique to zercheck_ir (zercheck.c has no ordering checks). Every Phase 5 error counts as a "disagreement" in the dual-run reporter, which prints to stderr but doesn't fail compile (per Issue #1).

After Issue #1 resolution (IR errors gate compile), the disagreement reporter still prints to stderr — informational only. That's fine.

But: tests run with `make check` may capture stderr. Phase 5 errors might cause spurious "disagreement" output that breaks test snapshots (if any check stderr).

**Verified**: `tests/test_zer.sh` checks exit codes, not stderr content. No stderr-based test breakage expected.

**Action**: nothing to change. The "DUAL-RUN disagreement" message is informational; tests don't depend on it.

### Issue #23 [new — found in re-read]: ZER_DUAL_RUN=0 mode

If user sets `ZER_DUAL_RUN=0`, zercheck_ir doesn't run. Phase 5 errors don't fire. Compile succeeds even with unsafe code.

**Verified at `zerc_main.c:662`**:
```c
const char *dual_env = getenv("ZER_DUAL_RUN");
bool dual_enabled = !(dual_env && dual_env[0] == '0');
```

This is a USER-controlled escape hatch. Document Phase 5 as "active when dual-run enabled (default)."

**Action**: document. Don't disable the escape hatch — users may have legitimate reasons (debugging, profiling).

---

## Updated implementation order (final)

| Step | Action | Effort | Risk | Tests added |
|---|---|---|---|---|
| **5.0** | NEW: Make zercheck_ir errors gate compile in zerc_main.c (Option B from Issue #1). +5 LOC. | 1 hr | Low (dual-run has 0 disagreements) | 0 |
| **5.0b** | NEW: Fix iterative summary loop convergence (Issue #19) — relax `count == sc_before` early exit | 1 hr | Low | 0 |
| 5.1 | Extract `asm_mnemonic_walk` helper; refactor checker.c | 4 hrs | Low | 0 |
| 5.2 | Add `ordering_rules.c` + VST proof | 4 hrs | Low | 0 |
| 5.3 | Add `IROrderingState` plumbing to `IRPathState` | 4 hrs | Low | 0 |
| 5.4 | Wire asm event tracking | 6 hrs | Med | 0 |
| **5.4b** | NEW: Add IR_LOCK, IR_UNLOCK, IR_SPAWN handlers as PRODUCES FULL_MEMORY | 1 hr | Low | 1 (positive: lock+CLWB) |
| 5.5 | Wire intrinsic event tracking (verify both NODE_INTRINSIC and IR_INTRINSIC_DECOMP paths) | 5 hrs | Med | 0 |
| 5.6 | Function-exit pending check (skip dead blocks) | 6 hrs | Med-High | 3 |
| 5.7 | CFG join discharge (set-intersection) | 4 hrs | Med | 2 |
| 5.8 | FuncSummary `barriers_produced` extension | 6 hrs | Med-High | 2 |
| 5.9 | Cross-arch tests (move ARM/RISC-V tests to test_cross_arch.sh) | 3 hrs | Low | 2 |
| 5.10 | Documentation + final make check | 2 hrs | Low | — |

**Updated total: ~47 hrs.** Within original revised range (35-50 hrs).

---

## Summary table of issues

| # | Severity | Issue | Resolution |
|---|---|---|---|
| 1 | **CRITICAL** | zercheck_ir errors don't gate compile | Step 5.0: gate via Option B (5 LOC) |
| 2 | Bug | `nlen == 17` typo for @barrier_acq_rel | Fix to 15; intrinsic exists |
| 3 | Missing | IR_LOCK / IR_UNLOCK as barriers | Add as PRODUCES FULL_MEMORY |
| 4 | Missing | IR_YIELD / IR_AWAIT semantics | Document as neutral |
| 5 | Missing | Dead-code path handling | Skip is_orelse_fallback / is_early_exit |
| 6 | Resolved | Convergence proof | Hand-traced; converges |
| 7 | Limitation | Extern C functions have no summary | Document; recommend ZER intrinsics |
| 8 | Missing | IR_SPAWN as barrier | Add as PRODUCES FULL_MEMORY |
| 9 | Resolved | @critical not a barrier | Skip — not ordering-relevant |
| 10 | Risk | MAX_ITERATIONS may need raising | Keep 32, fail-closed |
| 11 | Verify | IR_INTRINSIC_DECOMP coverage | Verify both paths handled |
| 12 | Resolved | Errors fire during summary building | Existing helper handles |
| 13 | Spec | Where exit-pending check runs | Between fixed-point and summary build |
| 14 | UX | Error message clarity | Existing message is fine |
| 15 | Done | FuncSummary extension | Reuse existing summary-building infrastructure |
| 16 | Confirmed | Recursion = pessimistic | Document; pre-existing |
| 17 | Confirmed | ARM tests cross-arch | Move ARM tests to cross-arch suite |
| 18 | Confirmed | Multi-line asm | Helper preserves \n handling |
| 19 | **NEW Bug** | Summary loop converges on count, not content | Step 5.0b: relax early exit |
| 20 | Resolved | Phase 5 errors timing | Existing pattern correct |
| 21 | Limitation | Cross-module name collision in FuncSummary | Pre-existing; document |
| 22 | Verified | dual-run reporter prints disagreement | Tests don't depend on stderr |
| 23 | Verified | ZER_DUAL_RUN=0 disables Phase 5 | Document as user escape hatch |

---

## Final updated estimate

Original: ~43 hrs.
Re-audit additions: +5 hrs (Steps 5.0, 5.0b, 5.4b, plus extra verification).

**Updated total: ~47 hrs.**

---

## Round 2 — Deep-Read Findings (2026-05-02)

User asked for a deeper audit, "read more codebase to find more findings if uncertain." Read additional sections; found significant gaps.

### Finding #24 [CRITICAL — most important new finding]: ZER intrinsics emit barriers via `__asm__ __volatile__`, bypassing `asm{}` syntax

Verified at `emitter.c:7150-7187` and elsewhere:

- `@cache_flushopt(addr)` emits `__asm__ __volatile__ ("clflushopt (%0)" ... : "memory")` directly in C output.
- `@cache_writeback(addr)` emits `__asm__ __volatile__ ("clwb (%0)" ... : "memory")`.
- `@nt_store(addr, val)` emits `__asm__ __volatile__ ("movnti %1, (%0)" ... : "memory")`.
- `@cache_clean_range`, `@cache_writeback_range`, `@cache_flush_range`, `@cache_flush_line` — emit a loop of clflush/clwb followed by `dsb ish` / `sfence`.
- `@cpu_write_cr3`, `@cpu_write_cr4`, etc. — emit privileged inline asm with `: "memory"` clobber.

**These are AST `NODE_INTRINSIC` nodes, NOT `NODE_ASM`.** My Phase 5 plan walks `node->asm_stmt.instructions` strings — that misses all of these.

**Consequence**: silent miscompile — Phase 5 wouldn't enforce CLWB→SFENCE for the canonical `@cache_writeback(p); ...; @barrier_store();` pattern.

**Resolution — extend Phase 5 step 5.5 (intrinsic event tracking)**:

Classification table (intrinsic name → barrier kind + role):

| Intrinsic | Barrier kind | Role | Reason |
|---|---|---|---|
| `@atomic_*` (load/store/cas/add/sub/or/and/xor/...) | FULL_MEMORY | PRODUCES | SEQ_CST per emitter.c:2828-2870 |
| `@barrier()` | FULL_MEMORY | PRODUCES | __atomic_thread_fence(SEQ_CST) |
| `@barrier_store()` | STORE_STORE | PRODUCES | __atomic_thread_fence(RELEASE) |
| `@barrier_load()` | LOAD_LOAD | PRODUCES | __atomic_thread_fence(ACQUIRE) |
| `@barrier_acq_rel()` | ACQUIRE_RELEASE | PRODUCES | __atomic_thread_fence(ACQ_REL) |
| **`@cache_writeback`** (CLWB) | **STORE_STORE** | **REQUIRES_AFTER** | **NEW — Intel SDM CLWB rule** |
| **`@cache_flushopt`** (CLFLUSHOPT) | **STORE_STORE** | **REQUIRES_AFTER** | **NEW — Intel SDM CLFLUSHOPT rule** |
| **`@nt_store`** (MOVNTI) | **STORE_STORE** | **REQUIRES_AFTER** | **NEW — Intel SDM MOVNTI rule (weakly-ordered)** |
| `@cache_clean_range`, `@cache_writeback_range`, `@cache_flush_range`, `@cache_flush_line` | (none — self-satisfying) | NONE | Emit own dsb/sfence at end |
| All privileged write-CR/MSR intrinsics with `:"memory"` clobber | FULL_MEMORY | PRODUCES | Memory clobber = full barrier (compiler-level) |

This is **the most consequential finding of the audit.** The plan didn't classify `@cache_*`/`@nt_store` at all because I was thinking in terms of `asm{}` blocks. They're a parallel barrier surface.

**Action**: rewrite step 5.5 of the plan to use the table above. ~30 lines of `if (name_matches(...))` chains in zercheck_ir.

### Finding #25 [coverage gap]: NO C unit tests for zercheck_ir

Verified: `test_zercheck.c` calls `zercheck_run` (AST analyzer). No `test_zercheck_ir.c` file. `test_ir_validate.c` tests only structural IR validation, not analysis.

**Consequence**: Phase 5 is tested only at the .zer integration level. No C-level unit tests for OrderingState transitions, satisfies-relation, etc.

**Resolution options**:
- **Option A**: build `test_zercheck_ir.c` as part of Phase 5 (C unit tests for OrderingState).
- **Option B**: skip; rely on .zer integration tests + VST proof of `zer_barrier_satisfies`.

**Recommendation**: Option B for v1. The .zer tests + VST cover the primary semantics. C unit tests are nice-to-have, defer.

### Finding #26 [confirmed]: vrp_ir.c is dead WIP, no Phase 5 conflict

Verified `vrp_ir.c` (349 lines) is NOT in Makefile CORE_SRCS or LIB_SRCS. The file declares its own `IRRangeState` etc. but isn't called from anywhere.

**Action**: noted; doesn't affect Phase 5. Remove from `vrp_ir.c` confusion later.

### Finding #27 [test infra]: stderr suppressed in tests

Verified `tests/test_zer.sh:42` runs tests with `2>/dev/null`. The dual-run "DUAL-RUN disagreement" stderr message doesn't break tests.

**Action**: nothing. Phase 5 stderr is silently ignored in test runs. Errors gating compile via Issue #1 fix is what matters.

### Finding #28 [test infra]: nowarn_check greps for "warning"

Verified `tests/test_zer.sh:171-184`: `nowarn_check` greps stderr for the literal word "warning". Phase 5 errors must NOT use the word "warning" (would false-positive these regression checks if the test happens to use atomics/barriers).

**Action**: error message uses "error" wording, not "warning."

### Finding #29 [test infra]: per-file flag directive supports cross-arch

Verified `tests/test_zer.sh:41`: `// zerc-flags: --target-arch=aarch64` works as first-line directive.

**Action**: Phase 5 ARM/RISC-V tests can stay in `tests/zer/` with the directive instead of moving to cross-arch suite. Simpler than originally planned.

### Finding #30 [resolved]: Iterative summary loop content stability

I previously flagged Issue #19 about summary loop content stability. After re-reading `zercheck_ir.c:3079-3109`:

```c
if (existing) {
    bool changed = false;
    if (existing->param_count == pc) { ... }
    if (existing->returns_color != returns_color_final) changed = true;
    if (existing->returns_param_color != returns_param_color_final) changed = true;
    if (changed) { /* update */ }
}
```

The summary update DOES detect content changes. But the loop in zerc_main.c:705 only checks `summary_count == sc_before` for early exit — it doesn't propagate the per-summary `changed` flag.

For Phase 5: when adding `barriers_produced`, I need to propagate the `changed` flag back to zerc_main loop. Options:
- Add `summaries_changed` flag to `ZerCheck`. Set when any summary's barriers_produced changes. Loop checks both count AND changed flag.
- Or: just run all 16 passes regardless. Wasteful but correct.

**Action**: add `summaries_changed` boolean to ZerCheck. Set in zercheck_ir summary update. Loop in zerc_main checks both. ~5 LOC.

### Finding #31 [coverage]: 45 existing tests use atomic/barrier intrinsics

Found 45 .zer files using `@atomic_*`/`@barrier_*`/`mfence`/etc. Some risky for Phase 5:

- `tests/zer/asm_c8_clwb_classified.zer` — has CLWB (in asm) + SFENCE in next block. Phase 5 must accept this multi-block pattern. ✓ tested already.
- `tests/zer/dalpha13_linux_scale.zer` — uses `@cache_flushopt` and `@cache_writeback` intrinsics. Phase 5 with new intrinsic classification would flag pending CLWB without subsequent SFENCE. **Risk: existing test would fail under Phase 5 classification!**

Let me re-check `dalpha13_linux_scale.zer`:

```zer
void dead_branch_cache_test() {
    u8 data = 0;
    if (never_true == 42) {
        @cache_flushopt(&data);
        @cache_writeback(&data);
    }
}
```

There's NO subsequent SFENCE/MFENCE/@barrier_store. Phase 5 would error on this test.

**This means Phase 5 enforcement would break an existing passing test.**

Options:
- Add `@barrier_store()` to the test (simple fix, makes test correct).
- Make Phase 5 not enforce on dead-branch (but that's complex).
- Add `// zerc-flags: --no-strict-asm-ordering` to opt out per-file.

**Recommendation**: add `@barrier_store()` to the test. The test is "smoke compile of cache intrinsics" — adding a barrier doesn't change its purpose, makes it a valid persistent-memory pattern. ~5-line update.

**Action**: as part of Phase 5 step 5.6 (function-exit check), update `dalpha13_linux_scale.zer` to include barrier or document as known false-positive case.

### Finding #32 [resolved]: ir_hook collection ordering

Verified `emitter.c:3743-3745` and `zerc_main.c:196-205`. Each function's IR is appended to a global array as the emitter encounters it. Order is "topological order in which emitter sees functions" (decided by zerc_main:673-688).

For cross-function barriers_produced lookup: the iterative summary loop (zerc_main.c:697-706) handles dependency ordering. After 16 passes, all summaries (including barriers_produced) should be stable.

**Action**: nothing — existing infrastructure handles ordering.

### Finding #33 [scope clarification]: Phase 5 effort vs delete-zercheck.c

User said "we fully use zercheck_ir." If they want zercheck.c DELETED as part of Phase 5, that's an additional ~3 hrs (per cfg_migration_plan.md Phase F).

**Recommendation**: do Phase F deletion BEFORE Phase 5 implementation. Reasons:
- Cleaner architecture going into Phase 5 work.
- Removes dual-run reporter noise during Phase 5 testing.
- Aligns with user's "fully use" framing.
- Per cfg_migration_plan.md, Phase F is ready to ship — 0 disagreements over 3143 programs.

If preferred to keep zercheck.c as belt-and-suspenders, just do Issue #1's Option B (gate IR errors).

**Open question for user**: do Phase F (delete zercheck.c) or just gate IR errors?

---

## Updated implementation order (final, post-round-2)

| Step | Action | Effort | Tests added |
|---|---|---|---|
| **5.0a** | Phase F migration: delete zercheck.c, make zercheck_ir primary (per cfg_migration_plan.md). Validates 0-disagreement claim. | 3 hrs | 0 |
| **5.0b** | Add `phase5_error_count` (or just gate `error_count` directly per Option B); add `summaries_changed` flag for loop convergence | 1.5 hrs | 0 |
| 5.1 | Extract `asm_mnemonic_walk` helper; refactor checker.c | 4 hrs | 0 |
| 5.2 | Add `ordering_rules.c` + VST proof | 4 hrs | 0 |
| 5.3 | Add `IROrderingState` plumbing to `IRPathState` | 4 hrs | 0 |
| 5.4 | Wire asm event tracking | 6 hrs | 0 |
| **5.4b** | Add IR_LOCK, IR_UNLOCK, IR_SPAWN handlers | 1 hr | 1 (positive: lock+CLWB) |
| **5.5** | Wire intrinsic event tracking — **EXPANDED** to include cache_*, nt_store, plus existing atomic_* and barrier_* | 6 hrs | 0 |
| 5.6 | Function-exit pending check + update `dalpha13_linux_scale.zer` | 6 hrs | 3 |
| 5.7 | CFG join discharge | 4 hrs | 2 |
| 5.8 | FuncSummary `barriers_produced` extension | 6 hrs | 2 |
| 5.9 | Cross-arch tests (use per-file flag directive) | 2 hrs | 2 |
| 5.10 | Documentation + final make check | 2 hrs | — |

**Updated total: ~50 hrs.**

Total estimate evolution:
- Original plan: ~43 hrs
- After re-audit round 1: ~47 hrs
- After re-audit round 2: ~50 hrs

The increase reflects the missed intrinsic surface and the upfront Phase F migration recommendation.

---

## Bottom-line verdict (round 2)

The original plan had:
- **One critical architectural issue (#1)**: resolved by user clarification + Step 5.0 (gate compile on IR errors).
- **One newly-found loop bug (#19)**: convergence early-exit. Fixed by `summaries_changed` flag.
- **One MASSIVE missed surface (#24)**: ZER intrinsics emit barriers via `__asm__ __volatile__` directly, NOT through `asm{}` syntax. Phase 5's intrinsic detection step needs to handle `@cache_writeback`, `@cache_flushopt`, `@nt_store` as REQUIRES_AFTER + many privileged write-* intrinsics as PRODUCES FULL_MEMORY.
- **One existing test breakage (#31)**: `dalpha13_linux_scale.zer` would fail under Phase 5 enforcement — needs minor update.
- **Several smaller issues (#2-#33)**: each documented and resolved or scheduled.

The architecture itself (CFG-aware OrderingState in zercheck_ir, set-intersection at joins, FuncSummary integration with `barriers_produced`) is correct and matches existing patterns.

**Round 2 increases the estimate to ~50 hrs and adds upfront Phase F migration as recommended Step 5.0a.**

The most consequential finding: **intrinsic-name classification table** (Finding #24) is the right way to handle ZER's `@cache_*`/`@nt_*`/etc. intrinsics that emit barriers without going through `asm{}` blocks. Without this, Phase 5 has a major coverage hole for the canonical persistent-memory pattern that ZER actually exposes today.

**Plan is solid after both audit rounds. Step 5.0a (Phase F) + 5.0b (gating + loop) are prerequisites; rest can proceed in original order with expanded Step 5.5 (intrinsic classification table).**

**Round 3 added two more critical pieces (Findings #36 + #37) — full concurrency intrinsic classification AND defer body scanning for barriers. Both are non-optional for Phase 5 to work on real ZER programs. Updated total: ~58 hrs.**

---

## Round 3 — Deep verification (2026-05-02)

User asked for round 3. Investigated all 8 deferred items.

### Finding #34 [resolved]: Compound keys don't apply to OrderingState

Verified: `IRHandleInfo` uses compound keys (path strings shared across path states per arena, line 50-56 of zercheck_ir.c). OrderingState (per Phase 5 design) is whole-program-point state — no per-entity keys, no compound paths.

**Action**: nothing to change. OrderingState lives alongside handles in `IRPathState` without sharing any key infrastructure.

### Finding #35 [resolved]: Memory clobber detection pattern

Verified at `checker.c:9764-9772`. Existing pattern:
```c
bool has_memory_clobber = false;
for (int i = 0; i < node->asm_stmt.clobber_count; i++) {
    AsmOperand *cb = &node->asm_stmt.clobbers[i];
    if (cb->reg_name_len == 6 && memcmp(cb->reg_name, "memory", 6) == 0) {
        has_memory_clobber = true;
        break;
    }
}
```

`AsmOperand` defined at `ast.h:265-270`. Phase 5 reuses this pattern verbatim.

**Action**: copy this pattern into Phase 5's asm event tracking. ~10 LOC.

### Finding #36 [CRITICAL — second-most consequential]: Concurrency intrinsics produce full barriers

Verified emissions:

| Intrinsic | Emits | Phase 5 classification |
|---|---|---|
| `@cond_wait(var, cond)` | `pthread_cond_wait` (which unlocks+waits+relocks the mutex) | PRODUCES FULL_MEMORY |
| `@cond_signal(var)` | `pthread_cond_signal` (with surrounding mutex lock/unlock) | PRODUCES FULL_MEMORY |
| `@cond_broadcast(var)` | `pthread_cond_broadcast` | PRODUCES FULL_MEMORY |
| `@cond_timedwait(var, cond, ms)` | Same as cond_wait but with timeout | PRODUCES FULL_MEMORY |
| `@sem_acquire(s)` | `pthread_mutex_lock + cond_wait + unlock` | PRODUCES FULL_MEMORY |
| `@sem_release(s)` | `pthread_mutex_lock + cond_signal + unlock` | PRODUCES FULL_MEMORY |
| `@barrier_init(var, count)` | `_zer_barrier_init` (pthread_mutex + pthread_cond) | PRODUCES FULL_MEMORY |
| `@barrier_wait(var)` | `_zer_barrier_wait` (lock + cond_wait + unlock) | PRODUCES FULL_MEMORY |
| `@once { body }` | `__atomic_load_n(_, ACQUIRE)` + `__atomic_store_n(_, RELEASE)` | PRODUCES ACQUIRE_RELEASE |
| `ThreadHandle.join()` | `pthread_join(th, NULL)` | PRODUCES FULL_MEMORY |
| `IR_LOCK` (shared struct field write/read) | `pthread_mutex_lock` | PRODUCES FULL_MEMORY |
| `IR_UNLOCK` | `pthread_mutex_unlock` | PRODUCES FULL_MEMORY |
| `IR_SPAWN` | `pthread_create` | PRODUCES FULL_MEMORY |

This is a MASSIVE list. Every one of these operations in real ZER programs would discharge any pending CLWB-style requirement.

**Consequence for Phase 5 tests**: a typical concurrent ZER program has so many full-memory barriers that pending CLWB rarely survives to function exit. Phase 5 mostly fires on:
- Pure single-threaded code with raw asm (no concurrency primitives)
- Concurrent code where CLWB is in a critical section but persistence-relevant store is OUTSIDE the lock

Both are real but narrow. Document.

**Action**: extend Phase 5 classification table with all rows above. ~50 LOC of name matching.

### Finding #37 [CRITICAL — defer body scanning required]

Verified at `zercheck_ir.c:3136-3162` — there's an existing `ir_defer_scan_frees` pattern that walks defer bodies and applies free events to return-block states. **Phase 5 needs the exact parallel for barriers.**

```zer
defer @barrier_store();      // SFENCE runs at exit
asm { instructions: "clwb (%0)" ... }
return;                       // CLWB pending — but defer's SFENCE satisfies it
```

Without defer scanning, Phase 5 would error on this canonical defer-barrier pattern.

**Resolution**: implement `ir_defer_scan_barriers(zc, func, ret_ps, body)` parallel to `ir_defer_scan_frees`. Walks defer body AST for barrier-emitting nodes (NODE_INTRINSIC, NODE_ASM with C8 mnemonics) and updates return-block `barriers_seen`.

**~80-100 LOC.** Significant new code. Add to plan as Step 5.6.5.

### Finding #38 [confirmed]: --lib mode does NOT disable IR analysis

Verified at `zerc_main.c:643-667`. `lib_mode` only affects emitter preamble/runtime. `dual_enabled` is checked separately and only ZER_DUAL_RUN=0 disables IR analysis.

**Action**: Phase 5 fires uniformly across --run, --emit-c, --lib. No mode-specific handling.

### Finding #39 [resolved]: building_summary suppression via ir_zc_error

Verified: `ir_zc_error` at `zercheck_ir.c:25` checks `zc->building_summary` and returns early. Phase 5 errors using the same helper get free suppression during fixed-point iteration.

**Action**: Phase 5 errors call `ir_zc_error` (or a sibling that also increments phase5_error_count). Existing pattern handles suppression.

### Finding #40 [risk]: Iteration cap with ordering state

Lattice size grows from "N handles × 5 states" to "N handles × 5 states + 12 barriers × 32 pending" per block.

Worst case: each iteration changes 1 handle state + 1 barrier bit + 1 pending entry per block = 3 changes per block per iteration. With B blocks, total possible changes = B × (N×5 + 12 + 32). For large B (say 500 blocks), that's potentially 500 × (50 + 44) = 47,000 monotonic state transitions. Well below the 32-iteration cap × 500 blocks = 16,000 iterations × passes.

Wait, that math doesn't quite work — let me redo. Each fixed-point pass updates ALL blocks. The number of passes = max independent state changes along any chain. With finite lattice height per block:
- Handles: 5 states × N handles = 5N transitions
- Barriers: 12 monotonic bits = 12 transitions max per block
- Pending: 32 entries × 12 bits = 384 transitions max per block

Total per-block: 5N + 12 + 384 = 5N + 396 transitions before saturation.

For N=10 handles: 446 transitions per block. With back-edges, 32 passes might be tight but workable.

**Risk**: large programs with many handles AND many barriers might exceed 32 passes.

**Resolution**: monitor in practice. If hit, raise to 64 or 128. Same fail-closed pattern as today.

**Action**: leave at 32. Add Phase 5 stress test (large CFG with many barriers + handles) as part of step 5.10.

### Finding #41 [resolved]: ThreadHandle.join detection

Verified at `emitter.c:1915-1934` — `.join()` on a ThreadHandle is detected by matching against `e->spawn_wrappers` (emitter-local list).

In zercheck_ir, ThreadHandle is tracked as `IRThreadTrack` (line 76). Detection: NODE_CALL with NODE_FIELD callee where field_name="join" AND object name matches a tracked IRThreadTrack.

**Action**: Phase 5 detects `.join()` calls and treats them as PRODUCES FULL_MEMORY. ~15 LOC.

### Finding #42 [resolved]: Async/await/yield are neutral

Verified at `ir_lower.c:2830-2851`. IR_YIELD and IR_AWAIT carry the cond AST but are stackless suspend/resume. State struct preserves locals; CPU/cache state preserves naturally (no kernel context switch).

**Phase 5 decision**: IR_YIELD and IR_AWAIT are NEUTRAL — pending barriers carry forward across suspend.

**Caveat**: real persistent-memory code probably never spans yield boundaries (NVDIMM operations are short critical sections). Document.

**Action**: skip IR_YIELD and IR_AWAIT in OrderingState transitions. Document in plan.

### Finding #43 [risk]: Cyclic call graphs and barriers_produced

Iterative summary loop (zerc_main.c:697-706) currently breaks on `summary_count` stability. If function f1 calls f2 calls f1 (cycle):
- Pass 1: f1 analyzed first, queries f2.summary (doesn't exist). Pessimistic: barriers from f2 = 0. f1's barriers_produced computed.
- Pass 1: f2 analyzed, queries f1.summary (now exists). Gets f1's barriers. f2's barriers_produced computed.
- Pass 1 ends. summary_count stable (both functions have summaries).

If pass 2 would refine f1's barriers (because f2's summary is now better), it doesn't run because count is stable.

**Pre-existing limitation**: same issue exists for handle-related fields (frees_param, returns_color). Phase 5 inherits the same pessimism.

**Action**: pre-existing; documented in Issue #19. Cumulative resolution: `summaries_changed` flag forces additional passes when content changes.

### Finding #44 [confirmed]: Comptime intrinsic calls don't reach IR

Verified: comptime functions are evaluated at compile time and substituted in AST before IR lowering. Comptime calls don't appear as IR_CALL or IR_INTRINSIC.

**Action**: Phase 5 doesn't need to handle comptime specially. They just don't show up.

### Finding #45 [verify]: Top-level `@once { body }` semantics

`@once { body }` with body containing `clwb` and no subsequent SFENCE — is the @once block treated correctly?

Looking at emitter for @once:
```c
emit(e, "    if (__atomic_load_n(inited, __ATOMIC_ACQUIRE) == 1) return;\n");
/* ... CAS to claim init ... */
/* ... body emitted here ... */
emit(e, "        __atomic_store_n(inited, 1, __ATOMIC_RELEASE);\n");
```

The body runs ONCE per program. After body, RELEASE store is emitted. RELEASE is a kind of store-store barrier in C++ semantics.

For Phase 5: `@once { ... clwb (%0) ... }` body — the trailing RELEASE store satisfies STORE_STORE? Per C++ memory model, RELEASE before next dependent store does provide the right semantics. Recommend treating `@once` block as PRODUCES ACQUIRE_RELEASE on entry+exit.

**Action**: classify `@once` block boundaries as PRODUCES ACQUIRE_RELEASE. The body's content is analyzed normally; pending CLWB inside @once would need handling within the block too.

### Finding #46 [test plan]: Phase 5 should exercise concurrency primitives

Add tests:

| Test | Pattern | Phase 5 expectation |
|---|---|---|
| `asm_g5_clwb_then_lock.zer` | CLWB, then access shared struct (IR_LOCK) | LOCK satisfies CLWB |
| `asm_g5_clwb_then_atomic.zer` | CLWB, then @atomic_store | atomic satisfies CLWB |
| `asm_g5_clwb_then_join.zer` | CLWB, then th.join() | join satisfies CLWB |
| `asm_g5_clwb_then_cond_signal.zer` | CLWB, then @cond_signal | signal satisfies CLWB |
| `asm_g5_clwb_in_defer.zer` | `defer @barrier_store(); ... CLWB ...; return` | defer scan finds barrier |
| `asm_g5_async_clwb.zer` | async fn with CLWB, then yield, then SFENCE | pending carries across yield |

### Finding #47 [resolved]: cinclude'd C atomic call NOT treated as barrier

Verified: `cinclude` declarations don't get FuncSummary. `__atomic_thread_fence(...)` called from cinclude'd `<stdatomic.h>` won't be recognized.

**Action**: documented in Finding #7. Users should use ZER intrinsics. Phase 6 can add `@barrier_extern` annotation.

### Finding #48 [edge case]: Function with NO return statement (infinite loops, abort, trap)

What happens to pending if function never returns?

Examples:
```zer
void worker() {
    while (true) {
        asm { instructions: "clwb (%0)" ... }
        @barrier_store();
    }
}
```

The function has no IR_RETURN. Phase 5's exit-block scan looks for IR_RETURN — and finds none. Pending never checked. Function silently passes.

Is that right? Probably. Infinite-loop functions don't return; the runtime never sees the "after" state.

But what about:
```zer
void main_loop() {
    asm { instructions: "clwb (%0)" ... }
    while (true) { ... }  // infinite loop
}
```

The CLWB happens BEFORE the infinite loop. Pending exists. Function never returns → never checked.

In real semantics, this code is fine (the cache line writeback happens, but never observed by anyone external). For ZER's safety claim ("no UB"), we should still error: the writeback's ordering is undefined w.r.t. the loop's stores.

But: detecting this requires distinguishing "function exits" (IR_RETURN) from "control reaches infinite loop." Hard.

**Pragmatic resolution**: scan ALL terminating blocks (not just IR_RETURN). For non-return terminators (IR_GOTO to entry, etc.), check pending too.

Actually simpler: if no IR_RETURN exists and pending was registered somewhere, flag as suspicious.

Even simpler: skip — document as known limitation. Real persistent-memory code does exit functions normally.

**Action**: document. Phase 5 only checks IR_RETURN blocks. Infinite loops are user's responsibility.

---

## Summary of round 3 findings

| # | Severity | Issue | Resolution |
|---|---|---|---|
| 34 | Resolved | Compound keys vs OrderingState | No interaction; OK |
| 35 | Resolved | Memory clobber detection | Pattern verified, ~10 LOC |
| **36** | **CRITICAL** | **All concurrency intrinsics produce barriers** | **+50 LOC classification table** |
| **37** | **CRITICAL** | **Defer body scanning required** | **+80-100 LOC, parallel to ir_defer_scan_frees** |
| 38 | Confirmed | --lib doesn't disable IR analysis | OK |
| 39 | Resolved | building_summary error suppression | Existing helper |
| 40 | Risk | Iteration cap with ordering state | Monitor; raise if hit |
| 41 | Resolved | ThreadHandle.join() detection | ~15 LOC |
| 42 | Resolved | Async/await neutral | Skip in tracking |
| 43 | Confirmed | Cyclic FuncSummary pessimism | Pre-existing |
| 44 | Confirmed | Comptime doesn't reach IR | OK |
| 45 | Verify | @once block boundaries | PRODUCES ACQUIRE_RELEASE |
| 46 | Test plan | New concurrency tests | +6 tests |
| 47 | Resolved | cinclude'd atomics | Documented limitation |
| 48 | Edge | Infinite-loop functions | Documented limitation |

---

## Updated estimate (round 3)

| Stage | Estimate |
|---|---|
| Original plan | ~43 hrs |
| After round 1 | ~47 hrs |
| After round 2 | ~50 hrs |
| **After round 3** | **~58 hrs** |

Round 3 added:
- Step 5.5 expansion: ~50 LOC for full concurrency intrinsic classification (+3 hrs)
- Step 5.6.5 NEW: defer body barrier scanning (+5 hrs)

**Updated total: ~58 hrs.**

---

## Final implementation order (post round 3)

| Step | Action | Effort | Tests added |
|---|---|---|---|
| **5.0a** | Phase F migration: delete zercheck.c, make zercheck_ir primary | 3 hrs | 0 |
| **5.0b** | Add `phase5_error_count` + `summaries_changed` flag | 1.5 hrs | 0 |
| 5.1 | Extract `asm_mnemonic_walk` helper | 4 hrs | 0 |
| 5.2 | Add `ordering_rules.c` + VST proof | 4 hrs | 0 |
| 5.3 | Add `IROrderingState` plumbing | 4 hrs | 0 |
| 5.4 | Wire asm event tracking + memory clobber | 6 hrs | 0 |
| 5.4b | Add IR_LOCK, IR_UNLOCK, IR_SPAWN, ThreadHandle.join handlers | 2 hrs | 1 |
| **5.5** | Wire intrinsic event tracking (FULL classification table — atomics, barriers, cache, nt_store, cond_*, sem_*, barrier_init/wait, @once) | 9 hrs | 0 |
| 5.6 | Function-exit pending check + update dalpha13 test | 6 hrs | 3 |
| **5.6.5** | NEW: Defer body barrier scanning (parallel to ir_defer_scan_frees) | 5 hrs | 1 |
| 5.7 | CFG join discharge | 4 hrs | 2 |
| 5.8 | FuncSummary `barriers_produced` extension | 6 hrs | 2 |
| 5.9 | Cross-arch tests (per-file flag directive) | 2 hrs | 2 |
| 5.10 | Documentation + final make check + stress test for iteration cap | 3 hrs | 1 |

**Total: ~58 hrs. 14 sub-steps. 12+ tests.**

---

## Round 3 — Open questions still unanswered

1. **Should Phase 5 fire on functions that never return?** (Finding #48) — deferred as limitation.

2. **Should @once block boundary be ACQUIRE_RELEASE or just FULL_MEMORY?** — recommend ACQUIRE_RELEASE per emitter pattern. Verify under load.

3. **Should `IR_LOCK` be PRODUCES ACQUIRE rather than FULL_MEMORY?** Pthread_mutex_lock IS strictly acquire; pthread_mutex_unlock IS release. More precise but Phase 6 work; Phase 5 uses FULL_MEMORY for simplicity.

4. **Should the iteration cap auto-raise based on barriers_produced complexity?** — Defer until real programs hit it.

5. **What about @verified_spec ordering claims?** — out of scope per original plan; user-asserted at Tier C.

If user wants round 4 (extreme thoroughness), I'd dig into:
- Module-level test interactions (does dual-run module test runner have anything different?)
- Error message localization (multi-file with cross-module barriers)
- Performance profiling estimate for Phase 5 added overhead

---

## What I did NOT find (sanity check)

Things I checked but found no issue with:

- **Intrinsic name length checks**: `@atomic_*` detection at "atomic_" prefix (length 7) handles all variants.
- **Memory allocation for ordering pending**: `MAX_PENDING = 32` cap prevents unbounded growth.
- **Termination detection**: `bb->insts[last].op == IR_RETURN` correctly identifies exit blocks.
- **Block iteration order**: zercheck_ir uses topological order with fixed-point for back-edges. Same as handle tracking.
- **Compound key for handles**: doesn't apply to ordering (no per-entity keys; ordering is whole-program-point state).
- **Multi-instruction asm with C8**: walker correctly tokenizes each mnemonic.
- **Memory clobber detection**: `node->asm_stmt.clobbers` array exists and is iterable; no helper needed beyond walking it.

---

## Open questions still requiring user input

1. **Option A vs B for Issue #1**: do full Phase F migration (delete zercheck.c) or just gate IR errors? User said "fully use zercheck_ir" — does that mean delete the file?

2. **MAX_PENDING value**: 32 is fine for plan but arbitrary. If real programs exceed, raise. Need feedback from real usage.

3. **Phase 5 errors as warnings or errors?**: plan defaults to errors. User can downgrade via flag if needed. OK with this default?

4. **`@asm_barrier(KIND)` user annotation**: Phase 6 future feature. Useful for marking unclassified asm as a known barrier. Worth adding?

5. **`@barrier_extern` for extern function declarations**: similar — Phase 6+. Marking C function decls as barriers.

These are deferrable. Phase 5 implementation can begin with Option B for #1 and proceed through sub-phases independently.
