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

## Round 4 — Comprehensive last sweep (2026-05-02)

User requested round 4. Investigated remaining surfaces.

### Finding #49 [CRITICAL — major architectural gap]: Interrupt handlers NOT analyzed by zercheck_ir

Verified at `emitter.c:4308` (interrupt path) vs `emitter.c:3743` (function path):

```c
/* Function path (line 3743): */
if (e->ir_hook) { e->ir_hook(e->ir_hook_ctx, ir); }
emit_func_from_ir(e, ir);

/* Interrupt path (line 4308): */
IRFunc *ir = ir_lower_interrupt(e->arena, e->checker, decl);
/* NO ir_hook call! */
emit_func_from_ir(e, ir);
```

**zercheck_ir does NOT analyze interrupt handler bodies.** Phase 5 inherits this gap — interrupt handlers won't be checked for ordering.

This is also a pre-existing gap for handle UAF, leak detection, etc. in interrupt handlers via zercheck_ir. zercheck.c (AST) catches some of these via different code paths.

**Impact for Phase 5**:
- Real ISR pattern: `interrupt USART1 { @cache_writeback(buf); ... }` — Phase 5 wouldn't fire even if no SFENCE follows.
- For v1, document as known limitation.
- Future fix: ~5 LOC to add ir_hook call after `ir_lower_interrupt` in emitter.c:4308.

**Resolution**: add the ir_hook call as part of Phase 5's Step 5.4 (asm event tracking). Trivial fix; closes the gap for interrupt handlers as a side-effect of Phase 5.

**Action**: add ~5 LOC ir_hook call to interrupt emission path. Matches function emission pattern.

### Finding #50 [risk — well-bounded]: 207 rust_tests use barriers/atomics, 0 use CLWB

Surveyed all rust_tests/ (784 .zer files):
- 207 use `@atomic_*`, `@barrier_*`, `spawn`, `@cond_*`, `@sem_*` — ALL of these PRODUCE barriers, never REQUIRE.
- 0 use `@cache_writeback`, `@cache_flushopt`, `@nt_store`, or asm `clwb`/`clflushopt`.

**Phase 5 won't false-positive on any of the 207 barrier-using tests.** They all produce barriers; pending requirements never accumulate without subsequent satisfying barrier.

**Consequence**: rust_tests pass cleanly under Phase 5. Risk: minimal.

**Action**: rust_tests are safe; document.

### Finding #51 [confirmed]: Module tests don't exercise barriers

66 test_modules/ files, 0 use `@atomic_*` / `@barrier_*` / `spawn` / asm with C8 mnemonics.

**Phase 5's cross-module barriers_produced inference is UNTESTED at module-test level.** Need to add at least one cross-module Phase 5 test:

```
// test_modules/g5_a.zer
void emit_clwb(*u32 p) {
    asm { instructions: "clwb (%0)" inputs: { "rdi" = p } ... }
}

// test_modules/g5_b.zer (calls g5_a, adds barrier)
import g5_a;
void user(*u32 p) {
    g5_a.emit_clwb(p);
    @barrier_store();
}
```

Phase 5's FuncSummary `barriers_produced` for g5_a.emit_clwb should be empty (it doesn't emit a barrier itself). User's pending CLWB is satisfied by `@barrier_store()`. Should compile clean.

**Action**: add cross-module Phase 5 tests in step 5.8.

### Finding #52 [medium]: LSP doesn't run zercheck_ir at all

Verified `zer_lsp.c:585-586`:
```c
zercheck_init(&zc, &checker, &arena, fname);
zercheck_run(&zc, file_node);  /* AST analyzer only */
```

**LSP uses zercheck.c (AST), not zercheck_ir.** Phase 5 errors won't appear in editor diagnostics until LSP is updated.

After Phase F migration (delete zercheck.c), this becomes a problem — LSP would lose ALL safety check feedback unless wired to zercheck_ir.

**For Phase 5 implementation**: not blocking; LSP wiring is separate (~50 LOC: load IR via emitter shim, run zercheck_ir per function, collect errors, return to LSP frontend).

**Resolution**: defer LSP integration to Phase 6 / Phase G migration. Document.

### Finding #53 [resolved]: Fuzzer doesn't use barriers/atomics

`tests/test_semantic_fuzz.c` has 32 generators, none using `spawn`/`@atomic_*`/`@barrier_*`/`@cache_*`. Fuzzer programs never trigger Phase 5.

**Action**: fuzzer is safe; document.

### Finding #54 [confirmed]: Source line preservation in IR

Verified: `IRInst.source_line` is consistently populated by ir_lower (e.g., `make_inst(op, node->loc.line)`). Phase 5 errors will cite correct source lines.

**Action**: nothing.

### Finding #55 [resolved]: Walker exhaustiveness discipline

Per CLAUDE.md Stage 2 Part B: every safety-critical walker has `-Wswitch` exhaustive enumeration (no default cases). Phase 5's new switches (asm dispatch, intrinsic match, CFG handlers) MUST follow this.

**Action**: Phase 5 implementation must list every NODE_ kind / IROpKind it handles explicitly. No default cases. Pre-commit check via `tools/walker_default_audit.sh` (already exists).

### Finding #56 [estimate]: Phase 5 performance overhead

Per-function cost analysis:
- Asm mnemonic walk: existing for F4 dispatch. Phase 5 adds ~50ns per mnemonic for ordering lookup. Negligible.
- Intrinsic name match: ~15-20 entries, perfect-hash impossible (string comparison). ~100ns per IR_ASSIGN with NODE_INTRINSIC. For typical function: ~1µs total.
- CFG join with ordering: bounded pending merge. ~32 entries × 32 entries = 1024 comparisons. ~10µs per join.
- Fixed-point iteration: linear in blocks × N iterations. For 100 blocks × 32 iter = 3200 block-passes. With ordering merge per join (~5 joins per block), ~5 × 1µs × 3200 = 16ms per function worst case.
- Defer body scan: walks AST per defer per return-block. ~5µs per defer × 5 defers × 5 returns = 125µs per function.
- FuncSummary computation: O(blocks). ~10µs per function.

**Total per-function overhead: ~5-20ms for complex functions.** For a 1000-function compile, ~5-20 seconds added.

This is significant but acceptable. Larger projects might want `--fast-mode` flag to disable Phase 5 in non-CI builds. Defer.

**Action**: profile after implementation. If real users complain, add `--no-ordering-check` flag.

### Finding #57 [confirmed]: Static globals + threadlocal NEUTRAL

Static globals are just memory addresses; their loads/stores have the same ordering semantics as any memory operation. No Phase 5 special handling needed.

`threadlocal` variables emit `__thread` storage class. Per-thread, no synchronization implied. Same ordering as regular memory.

**Action**: nothing.

### Finding #58 [resolved]: IR_RING_PUSH / container method calls

`Ring(T, N)` is a single-threaded circular buffer. No locking; no barriers. IR_RING_PUSH/POP have no ordering implications.

Container methods (e.g., `Stack(u32) s; s.push(5);`) compile to direct field manipulation. No barriers.

**Action**: skip in Phase 5. Document.

### Finding #59 [test plan addition]: Multi-module Phase 5 tests

Add to test_modules/:
- `g5_module_caller.zer` + `g5_module_callee.zer` — cross-module CLWB+barrier
- Verify FuncSummary's barriers_produced propagates correctly across module boundaries

### Finding #60 [resolved]: Error message line accuracy across modules

When CLWB is in module A and Phase 5 fires error in module B's caller (because barriers_produced shows A doesn't satisfy), error should cite:
- The CLWB site (in module A)
- OR the caller site (in module B)
- OR both

Existing FuncSummary errors (e.g., handle UAF across functions) cite the CALL site, not the callee. Phase 5 should follow the same convention: error fires at the CLWB site, not at the eventual function exit.

But wait — Phase 5 fires at function exit when pending unresolved. For cross-module: pending lives in caller, originated from CLWB inside callee. Caller's pending list would have origin_line = callee's CLWB line (a different file).

`ir_zc_error(zc, line, ...)` uses `zc->file_name` (which is the current compilation unit). Cross-module line references would print the caller's file with the callee's line number — wrong.

**Resolution**: extend `IROrderingPending` with `origin_file` field. Use it in error message: `"asm 'clwb' at <origin_file>:<origin_line> requires barrier..."`.

**Action**: add `origin_file` to `IROrderingPending`. ~3 LOC.

---

## Summary of round 4 findings

| # | Severity | Issue | Resolution |
|---|---|---|---|
| **49** | **CRITICAL** | **Interrupt handlers not analyzed by zercheck_ir** | **+5 LOC ir_hook call in interrupt path** |
| 50 | Confirmed | 207 rust_tests safe (PRODUCE-only) | Document |
| 51 | Risk | 0 cross-module barrier tests | Add tests in step 5.8 |
| 52 | Medium | LSP doesn't run zercheck_ir | Defer to Phase 6 |
| 53 | Resolved | Fuzzer doesn't use barriers | Safe |
| 54 | Resolved | Source line preservation correct | OK |
| 55 | Confirmed | Walker exhaustiveness discipline | Honor in Phase 5 |
| 56 | Estimate | ~5-20ms per function overhead | Acceptable; flag if needed |
| 57 | Resolved | Static / threadlocal globals neutral | OK |
| 58 | Resolved | IR_RING / container methods neutral | OK |
| 59 | Test plan | Cross-module test additions | +2 module tests |
| 60 | Spec | Error message line accuracy across modules | +3 LOC origin_file field |

---

## Cumulative effort estimate

| Round | Estimate |
|---|---|
| Original | ~43 hrs |
| Round 1 | ~47 hrs (gating) |
| Round 2 | ~50 hrs (cache_/nt_store classification) |
| Round 3 | ~58 hrs (concurrency intrinsics + defer scanning) |
| **Round 4** | **~60 hrs** (interrupts + multi-module tests + cross-module error format) |

Round 4 added:
- ir_hook for interrupts: +0.5 hr (trivial)
- multi-module test fixtures: +1 hr
- origin_file in pending: +0.5 hr

**Final estimate: ~60 hrs.**

---

## Final implementation order (post round 4)

| Step | Action | Effort | Tests added |
|---|---|---|---|
| 5.0a | Phase F migration: delete zercheck.c, make zercheck_ir primary | 3 hrs | 0 |
| 5.0b | Add `phase5_error_count` + `summaries_changed` flag | 1.5 hrs | 0 |
| **5.0c** | NEW: Add ir_hook call to interrupt emission path | 0.5 hrs | 0 |
| 5.1 | Extract `asm_mnemonic_walk` helper | 4 hrs | 0 |
| 5.2 | Add `ordering_rules.c` + VST proof | 4 hrs | 0 |
| 5.3 | Add `IROrderingState` plumbing (with `origin_file` field) | 4 hrs | 0 |
| 5.4 | Wire asm event tracking + memory clobber | 6 hrs | 0 |
| 5.4b | Add IR_LOCK, IR_UNLOCK, IR_SPAWN, ThreadHandle.join handlers | 2 hrs | 1 |
| 5.5 | Wire intrinsic event tracking (full classification table) | 9 hrs | 0 |
| 5.6 | Function-exit pending check + update dalpha13 test | 6 hrs | 3 |
| 5.6.5 | Defer body barrier scanning | 5 hrs | 1 |
| 5.7 | CFG join discharge | 4 hrs | 2 |
| 5.8 | FuncSummary `barriers_produced` extension + cross-module tests | 7 hrs | 4 |
| 5.9 | Cross-arch tests | 2 hrs | 2 |
| 5.10 | Documentation + final make check + perf profiling note | 3 hrs | 1 |

**Total: 15 sub-steps. ~60 hrs. 14+ tests.**

---

## After round 4 — what's left?

Items I checked but didn't dig into:
- Specific GCC __atomic intrinsic ordering semantics on each arch
- Full Iris/Coq formal proof for OrderingState soundness (Tier C work)
- Complex integration with `@verified_spec` (Tier C, out of scope)
- Programs > 10000 lines: how does iteration cap behave?
- WASM target (not implemented)
- Fuzzer extension to add CLWB pattern generators (~3 hrs, defer)

If user wants round 5: would investigate the iteration cap behavior on programs near the 32-iter limit, and any remaining ZER intrinsic that uses __asm__ I might have missed.

But honestly, 4 rounds is comprehensive. Round 4 found 1 CRITICAL (interrupt handlers) and 3 medium issues; rounds 1-3 found the architectural blockers. Plan is implementable.

---

## What round 4 changed in the plan

**One new critical finding**: interrupt handlers don't get IR analysis. ~5 LOC fix.

**Two medium findings**: multi-module test gap (need new test fixtures), error message line accuracy across modules (need origin_file field in pending).

**One performance estimate**: ~5-20ms per function. ~5-20 seconds added per 1000-function compile. Acceptable.

**Confirmed safe**: rust_tests (207 files), fuzzer, IR_RING, container methods, static/threadlocal globals. None false-positive under Phase 5.

## Round 5 — Final extreme-thoroughness sweep (2026-05-02)

User asked for round 5. Investigated remaining surfaces. Found one major intrinsic-classification gap and several finer-grained findings.

### Finding #61 [CRITICAL — 40+ MORE intrinsics need classification]

Comprehensive grep across all 145 intrinsics in emitter.c. Found **218 inline `__asm__ __volatile__` emission sites** — far more than the ~10 I'd already classified.

Categorized:

**Already in plan (round 2-3) — keep**:
- `@cache_writeback`, `@cache_flushopt`, `@nt_store` — REQUIRES_AFTER STORE_STORE
- `@atomic_*`, `@barrier_*` — PRODUCES (specific kinds per round 1-2)
- `@cache_*_range`, `@cache_*_line` — self-satisfying

**NEW — need classification (Round 5)**:

| Intrinsic family | Count | Phase 5 classification | Reason |
|---|---|---|---|
| `@tlb_flush_*` (all/global/asid/addr/range) | 5 | PRODUCES FULL_MEMORY | Emit DSB ISH / SFENCE.VMA — strong barrier |
| `@mmu_*` (sync, enable, disable, set_pt, get_pt, etc.) | ~9 | PRODUCES FULL_MEMORY | DSB+ISB combined; CR3 writes are full barriers |
| `@cpu_write_cr0/cr3/cr4/xcr0` | 4 | PRODUCES FULL_MEMORY | CR3 write flushes TLB; CR4 changes paging — serializing |
| `@cpu_save_context/restore_context/save_fpu/restore_fpu/xsave/xrstor/fxsave/fxrstor` | 8 | PRODUCES FULL_MEMORY | All have `: "memory"` clobber |
| `@cpu_iret/syscall/sysret/hypercall/sbi_call/smc_call` | 6 | PRODUCES FULL_MEMORY | Privileged transitions serialize |
| `@cpu_cache_disable/cache_enable/flush_pipeline` | 3 | PRODUCES FULL_MEMORY | CR0 write + WBINVD |
| `@barrier_dma` | 1 | PRODUCES FULL_MEMORY (or DMA_SYNC) | MFENCE / DMB SY / FENCE rw,rw |
| `@cpu_write_msr` | 1 | PRODUCES FULL_MEMORY (conservative) | Some MSRs serialize, some don't |
| `@port_in/out 8/16/32` | 6 | NEUTRAL (or PRODUCES STORE_LOAD) | I/O instructions serialize on x86 |
| `@cpu_disable_int/enable_int` | 2 | NEUTRAL | CLI/STI disable interrupts, not memory order |
| `@cpu_pause/wfe/sev/idle_hint/wait_int/deep_sleep/mwait/monitor_addr/umwait/umonitor` | ~10 | NEUTRAL | Spin/sleep hints, not barriers |
| `@cpu_read_*` (cr0/cr2/cr3/cr4/dr/msr/sp/tp/flags/fsbase/gsbase/etc.) | ~15 | NEUTRAL | Read-only, no barrier |
| `@cpu_id/cpuid/vendor_id/feature_bits/model_id/get_priv_level` | 6 | NEUTRAL | CPUID serializes execution but not memory ordering for our purposes |
| `@cpu_rdrand/rdseed` | 2 | NEUTRAL | Random read, no barrier |
| `@cpu_eoi/breakpoint/endbr/get_pc/sev` | 5 | NEUTRAL | No memory ordering |

**~40 new intrinsics to classify in Phase 5 step 5.5** (plus the ~15 already in plan, total ~55 intrinsics to handle).

**Action**: extend Phase 5 step 5.5 classification table to cover all of these. The PRODUCES FULL_MEMORY group is the largest concrete addition (~30 entries). NEUTRAL entries don't need code (default = no event).

This is a **major plan update**. The classification table grows from ~15 entries to ~55 entries. ~150 LOC of `if (memcmp(name, ...) == 0)` chains in zercheck_ir.

**Total additional effort: +6 hrs.** Still doable in Phase 5 step 5.5 estimate (was 9 hrs, now 15 hrs).

### Finding #62 [optimization opportunity]: FuncProps `has_sync` as fast-path

Verified `types.h:217-220`:
```c
struct {
    bool computed;
    bool in_progress;
    bool can_yield;
    bool can_spawn;
    bool can_alloc;
    bool has_sync;  /* body contains @atomic_* or @barrier */
    /* ... */
} props;
```

`has_sync` already tracks "function body has @atomic/@barrier somewhere" via lazy DFS. Wrong semantic for Phase 5 (it's "any path", not "all paths"), so **can't replace FuncSummary.barriers_produced**.

But it's a **fast-path optimization**: if `callee->props.has_sync == false`, callee CERTAINLY produces no barriers. Skip FuncSummary lookup. ~3 LOC, saves time.

**Action**: optimization in step 5.8. Defer.

### Finding #63 [confirmed limitation]: Indirect calls can't resolve barriers_produced

Function pointers (`*(u32) -> u32`) don't have `func_name` at IR_CALL site. FuncSummary lookup fails. Phase 5 conservatively assumes indirect calls produce no barriers.

Real impact: callback-based persistence APIs (passing `pmem_drain` as function pointer) won't have their barrier tracked.

**Resolution**: documented limitation. Same as recursion / extern functions. Future Phase 6+ work.

### Finding #64 [confirmed safe]: --release flag is no-op

Verified `zerc_main.c:224-261`. `release_mode` is parsed but never checked downstream. `--release` doesn't disable Phase 5 (or anything else). Currently a no-op flag.

**Action**: nothing. Future plans for release mode would need explicit Phase 5 opt-out.

### Finding #65 [confirmed]: ZER doesn't support user variadic functions

Only emitter uses `va_list` internally for printf-style formatting. ZER source-level variadic functions don't exist.

**Action**: no Phase 5 handling needed.

### Finding #66 [confirmed]: goto_spaghetti_safe.zer doesn't stress Phase 5

The existing 57-line stress test has no barriers/atomics. Won't exercise the OrderingState lattice.

**Action**: Phase 5 step 5.10 should add a stress test with many barriers + many blocks (e.g., 200-block function with CLWB+SFENCE patterns) to exercise iteration cap.

### Finding #67 [confirmed]: Predicate file template

Verified `src/safety/handle_state.c` template:
- Self-contained (no includes beyond own header)
- Pure (no globals, no side effects)
- Primitive types only
- Early-return cascade pattern
- ~25 LOC for similar complexity

Phase 5's `ordering_rules.c` follows exactly. ~30 LOC for `zer_barrier_satisfies` + `zer_barriers_seen_satisfies`.

**Action**: confirmed feasible. Step 5.2 estimate (~4 hrs) is right.

### Finding #68 [confirmed]: VST proof effort

Verified `proofs/vst/verif_handle_state.v` (188 lines) and `verif_atomic_rules.v` (167 lines) as templates. Phase 5's `verif_ordering_rules.v` ~70-100 lines.

**Action**: confirmed feasible. Step 5.2 includes VST proof in the ~4 hrs.

### Finding #69 [comprehensive safe surface]

Final tally of test files that won't false-positive under Phase 5:
- 538 .zer integration tests, only `dalpha13_linux_scale.zer` breaks (1 file, ~5 LOC fix)
- 200 fuzzer-generated programs — no barriers, safe
- 139 conversion tests — no barriers, safe
- 5 cross-arch tests — no barriers, safe
- 66 module tests — no barriers, safe
- 207 rust_tests using barriers — all PRODUCE-only, safe
- 36 zig_tests — none use barriers, safe
- 8 firmware examples, 1 (concurrency_demo) uses pthread (PRODUCES), no CLWB, safe

**Total: ~1,200 tests, only 1 needs updating.** Excellent blast-radius bounding.

### Finding #70 [comprehensive]: 218 __asm__ sites in emitter.c

Counted: 218 distinct `__asm__ __volatile__` emission sites across 145 intrinsics + asm{} fallback. The ~55 needing Phase 5 classification cover all barrier-relevant cases. The other ~163 are for non-barrier-related ops (read CRn, port I/O, breakpoint, etc.).

**Action**: comprehensive enumeration in Phase 5 step 5.5 table. Ensure walker_audit catches missing cases via discipline.

### Finding #71 [pre-existing infrastructure]: Existing src/safety/ predicate files for inspiration

Verified existing predicate files:
- `arith_rules.c`, `asm_categories.c`, `atomic_rules.c`, `cast_rules.c`, `coerce_rules.c`, `comptime_rules.c`, `concurrency_rules.c`, `container_rules.c`, `context_bans.c`, `escape_rules.c`, `handle_state.c`, `isr_rules.c`, `mmio_rules.c`, `misc_rules.c`, `move_rules.c`, `optional_rules.c`, `provenance_rules.c`, `range_checks.c`, `stack_rules.c`, `type_kind.c`, `variant_rules.c`

20 existing pure-predicate files. Phase 5's `ordering_rules.c` is #21. Architecture is well-established; just follow patterns.

**Action**: no new infrastructure. Slot in alongside existing files.

### Finding #72 [pattern]: zercheck_ir + checker.c BOTH need Phase 5 changes for AST integration

zercheck_ir handles IR-level state machine. But the asm mnemonic walker is currently in checker.c (NODE_ASM dispatch). To use it from zercheck_ir, refactor to shared helper (Phase 5 step 5.1).

For intrinsic detection: the `props.has_sync` AST scanner already exists in checker.c:7088. Phase 5's intrinsic classification could either:
- (A) Add another similar AST scanner in zercheck_ir (duplicates logic)
- (B) Extend checker's existing scan_func_props to compute barriers_produced too
- (C) Walk AST inside zercheck_ir's IR_ASSIGN handler when expr is NODE_INTRINSIC

Option C is cleanest for Phase 5 — zercheck_ir already inspects `inst.expr` for various reasons. Adding intrinsic-name detection there is local to zercheck_ir.

**Action**: confirm Option C in step 5.5. Extension via local detection.

### Finding #73 [confirmed]: Module-mangled names in FuncSummary lookup

Verified `checker.c:7136-7149`: module-qualified calls (`config.func()`) are resolved to mangled names (`module__func`) for FuncProps lookup. Same pattern would work for FuncSummary's barriers_produced.

For Phase 5: when looking up `barriers_produced` of a callee in zercheck_ir IR_CALL handler, use existing module-mangling logic.

**Action**: no special handling needed. Existing infrastructure handles cross-module name resolution.

---

## Summary of round 5 findings

| # | Severity | Issue | Resolution |
|---|---|---|---|
| **61** | **CRITICAL** | **40+ MORE intrinsics need classification** | **+6 hrs in step 5.5** |
| 62 | Optimization | FuncProps has_sync as fast-path | Defer |
| 63 | Limitation | Indirect calls don't resolve barriers | Documented |
| 64 | Confirmed | --release no-op | OK |
| 65 | Confirmed | No user variadics | OK |
| 66 | Test addition | goto_spaghetti doesn't stress Phase 5 | Add real stress test in step 5.10 |
| 67 | Template | handle_state.c template confirmed | OK |
| 68 | Template | VST proof effort confirmed | OK |
| 69 | Comprehensive | Safe surface mapped (~1,200 tests, 1 break) | Excellent |
| 70 | Comprehensive | 218 __asm__ sites; ~55 need Phase 5 | Covered by #61 |
| 71 | Existing | 20 predicate files; ordering_rules is #21 | Pattern established |
| 72 | Architecture | zercheck_ir AST inspection for intrinsics | Option C (local) |
| 73 | Confirmed | Module-mangling already handled | OK |

---

## Cumulative effort estimate

| Round | Estimate |
|---|---|
| Original | ~43 hrs |
| Round 1 | ~47 hrs |
| Round 2 | ~50 hrs |
| Round 3 | ~58 hrs |
| Round 4 | ~60 hrs |
| **Round 5** | **~66 hrs** |

Round 5 added:
- +6 hrs for ~40 additional intrinsic classifications

**Final estimate: ~66 hrs.**

---

## Final implementation order (post round 5)

| Step | Action | Effort |
|---|---|---|
| 5.0a | Phase F migration: delete zercheck.c | 3 hrs |
| 5.0b | Add `phase5_error_count` + `summaries_changed` flag | 1.5 hrs |
| 5.0c | Add ir_hook call to interrupt emission path | 0.5 hrs |
| 5.1 | Extract `asm_mnemonic_walk` helper | 4 hrs |
| 5.2 | Add `ordering_rules.c` + VST proof | 4 hrs |
| 5.3 | Add `IROrderingState` plumbing | 4 hrs |
| 5.4 | Wire asm event tracking + memory clobber | 6 hrs |
| 5.4b | IR_LOCK/UNLOCK/SPAWN/ThreadHandle.join handlers | 2 hrs |
| **5.5** | **Wire intrinsic event tracking — ~55 intrinsic classifications (atomics, barriers, cache, nt_store, cond_*, sem_*, barrier_init/wait, @once, TLB, MMU, write_CR/MSR, context switch, privileged transitions, cache control, barrier_dma)** | **15 hrs** |
| 5.6 | Function-exit pending check + update dalpha13 test | 6 hrs |
| 5.6.5 | Defer body barrier scanning | 5 hrs |
| 5.7 | CFG join discharge | 4 hrs |
| 5.8 | FuncSummary `barriers_produced` extension + cross-module tests | 7 hrs |
| 5.9 | Cross-arch tests | 2 hrs |
| 5.10 | Documentation + final make check + stress test (200-block barrier program) | 3 hrs |

**Total: 15 sub-steps. ~66 hrs. ~14+ tests.**

---

## What's left after round 5?

Items I might verify in round 6:
- Each individual intrinsic in the new ~40-entry classification — manually verify ARM/RISC-V semantics differ from x86
- Specific iteration-cap behavior on a 200-block barrier program (write the stress test, run it)
- Check if Phase 5 needs to interact with zercheck_ir's `urs` (UAF Report Set) for de-duplication

But honestly, after 5 rounds with 73 distinct findings, **the audit is comprehensive**. All architectural blockers found, all major test risks bounded, all classifications enumerated.

**Diminishing returns from here. Implementation should begin.**

---

## What round 5 changed in the plan

**One critical finding**: ~40 more intrinsics need classification beyond what previous rounds identified. The largest single addition to Phase 5 step 5.5 (was ~15 entries, now ~55). +6 hrs.

**Several confirmations**: predicate file templates, VST proof effort, existing infrastructure for module-mangling, safe surface across all 1,200 tests.

**Testing insights**: goto_spaghetti_safe.zer is too small to stress Phase 5; a real 200-block stress test is needed (added to step 5.10).

**Optimization noted**: FuncProps `has_sync` as fast-path before FuncSummary lookup. Defer to post-Phase-5.

---

## Five-round audit cumulative findings

73 distinct findings across 5 rounds:
- **6 CRITICAL** (compile gating, summary loop, intrinsic surface, defer scanning, interrupts, full intrinsic classification)
- **~12 medium** (LSP, multi-module tests, ARM tests, cross-module errors, stress test, etc.)
- **~30 confirmed/resolved** (compound keys, source lines, walker discipline, module mangling, indirect calls, etc.)
- **~15 deferred limitations** (extern atomics, function pointers, infinite loops, recursion pessimism, etc.)
- **~10 risks acknowledged** (iteration cap, performance, false-positive surfaces)

**Architecture remains sound across all 5 rounds.** No findings invalidated the IR-level CFG OrderingState approach. All findings refined the classification table, added missing handlers, or documented limitations.

**The plan is ready to implement.** Total estimate: ~66 hrs across 15 sub-steps, each independently shippable.

---

## Round 6 — Critical evaluation: is the plan SOUND ENOUGH?

User asked the meta-question: "is our plan sound enough or strong enough?"

This isn't another finding-hunting round. It's an honest stress-test of the plan's quality. Five categories:

### 6.1 Is the plan CORRECT?

**Architecture (CFG-aware OrderingState in zercheck_ir)**: ✓ correct.
- Reuses existing fixed-point loop; same shape as handle state.
- Set-intersection at joins is semantically right (most-conservative).
- Union of pending at joins is right (carry forward).
- Discharge-after-merge correctness: hand-traced; works.

**One real correctness gap found in round 6**:

```zer
void persist() {
    defer @cache_writeback(p);    // CLWB at function exit
    // no SFENCE possible after defer fires
    return;
}
```

The plan's defer body scanning (Step 5.6.5) walks defer bodies for **PRODUCES** barriers (Step 5.6.5 adds them to barriers_seen at exit). It does NOT model **REQUIRES_AFTER** inside defer bodies.

Result: CLWB inside defer with no further defer producing SFENCE → Phase 5 would NOT catch this bug. **False negative.**

Worse, LIFO-ordered defers can have correct-looking but actually-broken patterns:
```zer
defer @cache_writeback(p);   // fires SECOND (LIFO)
defer @barrier_store();      // fires FIRST (LIFO)
```
CLWB fires AFTER SFENCE → CLWB has no subsequent barrier → UB. Plan misses it.

**Resolution (corrected — was originally "ban", revised to TRACK per CLAUDE.md design philosophy)**:

CLAUDE.md's ban-decision framework: safety = tracking, not banning. Apply to defer-REQUIRES_AFTER:
1. Hardware/OS constraint? No
2. Emission impossibility? No
3. Needs runtime? No
4. Needs type system? No
5. → TRACK, don't ban.

**Proper TRACK solution: simulate defer LIFO at function exit.**

Algorithm in Step 5.6.5:
1. Collect all `IR_DEFER_PUSH` in function (document order)
2. For each IR_RETURN block:
   - Walk defers in REVERSE order (LIFO simulation)
   - For each defer body, scan AST for ordering events:
     - PRODUCES → add to `barriers_seen`
     - REQUIRES_AFTER → add to `pending`
   - After all defers simulated, check pending against barriers_seen

This correctly handles:

```zer
// Correct: SFENCE fires after CLWB in execution (LIFO)
defer @barrier_store();      // fires LAST
defer @cache_writeback(p);   // fires FIRST
// LIFO sim: CLWB pending → SFENCE discharge → PASS
```

```zer
// Wrong: SFENCE fires BEFORE CLWB (LIFO)
defer @cache_writeback(p);   // fires LAST
defer @barrier_store();      // fires FIRST
// LIFO sim: SFENCE → CLWB pending → ERROR
```

```zer
// Wrong: bare CLWB in defer
defer @cache_writeback(p);
// LIFO sim: CLWB pending → ERROR
```

```zer
// Correct: main-body CLWB, defer SFENCE
asm { instructions: "clwb (%0)" ... }  // main body: pending+=
defer @barrier_store();
// Main body pending → defer SFENCE discharges → PASS
```

All cases correct, no false positives.

**Effort**: ~30 LOC (vs ~10 for the ban approach). Worth it for correctness + no false positives + alignment with CLAUDE.md philosophy.

**Action**: Step 5.6.5 implements LIFO simulation. Walks defers in reverse, applies both PRODUCES and REQUIRES_AFTER events.

**Other correctness checks**:
- Mutual recursion in barriers_produced: pessimistic-and-stable. Correct (no false positive; possible false negative for symmetric patterns — acceptable).
- Goto convergence: lattice is finite, so converges. Same machinery as handle state.
- Indirect calls: pessimistic, no help. Correct.
- Async/yield: state preserved across stackless suspend. Correct.

**Verdict**: 1 correctness gap found in round 6. ~10 LOC fix. Otherwise, plan is correct.

### 6.2 Is the plan COMPLETE?

5 rounds × ~73 findings is comprehensive. But let me try to find what's NOT in the plan:

- **Phase 5 errors during fuzzing**: fuzzer doesn't use barriers, so no fuzz coverage. Risk: Phase 5 has a code path that only executes on real ZER programs. Mitigation: add fuzzer generators for `@cache_writeback`/`@nt_store`/asm CLWB patterns. ~2 hrs. Not in plan.
- **Phase 5 errors during semantic fuzz**: same, semantic fuzzer doesn't generate barriers. Same mitigation.
- **Multi-line `instructions:` strings with multiple C8 mnemonics**: e.g., `"clwb (%0)\nclwb (%1)\nsfence"`. Plan walks each mnemonic; both CLWBs register pending; SFENCE clears both. Should work but untested. Need explicit test.
- **Same-asm-block multi-CLWB with no SFENCE**: `"clwb (%0)\nclwb (%1)"`. Both pending; function exits without barrier. Phase 5 errors. Should work; need test.
- **Rust-style async with stackful coroutines**: ZER doesn't have these. N/A.
- **Multi-thread happens-before across threads**: out of scope per System #30 design (single-hart only).

**One small completeness gap**: defer-with-REQUIRES_AFTER ban. Already covered above.

**One test-coverage gap**: Phase 5 has no fuzzer coverage. Add 1-2 generator functions in `tests/test_semantic_fuzz.c`. ~2 hrs.

**Verdict**: plan is mostly complete. Adding fuzzer coverage + defer-REQUIRES_AFTER ban makes it tight.

### 6.3 Is the plan IMPLEMENTABLE?

**LOC estimates** (round 5 final):
- Step 5.5: ~150 LOC for ~55 intrinsic classifications. Realistic — looking at existing emitter.c emission chains, ~3 LOC per entry.
- Step 5.3: ~80 LOC for IROrderingState + ops. Realistic — handle state ops are ~120 LOC; OrderingState is simpler.
- Step 5.4 + 5.4b: ~100 LOC for asm/intrinsic event dispatch. Realistic.
- Step 5.7: ~60 LOC for CFG join discharge. Realistic — existing handle merge is ~80 LOC.

**Time estimates**:
- Each step has clear deliverables and tests.
- Integration points all in zercheck_ir.c (one file). No cross-cutting concerns beyond FuncSummary extension.
- VST proof template established (~50-100 LOC).
- Each sub-step independently shippable.

**Risk**: Step 5.5 is the largest at 15 hrs. If real implementation is >20 hrs, that's a 25% over-estimate. Acceptable.

**Verdict**: implementable. Realistic estimates. Each sub-step de-risks the next.

### 6.4 Is the plan WORTH IMPLEMENTING NOW?

This is the hardest question.

**Cost**:
- 66 hrs of dev time
- ~5-20s added per 1000-function compile (perf overhead)
- ~150 LOC of intrinsic classification table to maintain
- LSP integration deferred (will need fixing later)
- 1 existing test breaks (dalpha13, ~5 LOC fix)
- Future maintenance burden when ZER adds more intrinsics

**Value**:

Today's user surface = 1 test file (`dalpha13_linux_scale.zer`). Zero real-world ZER programs use `@cache_writeback`/`@cache_flushopt`/`@nt_store` for actual persistent memory. ZER doesn't ship a libpmem equivalent.

Future value: when someone writes ZER persistent-memory code, Phase 5 prevents a class of subtle bugs that would otherwise cause silent data loss on power failure (NVDIMM persistence violations).

**The honest assessment**:

Phase 5 is HIGH cost (66 hrs) for LOW immediate value (~0 real users today). It's an investment in:
- **Safety completeness**: closes the last major gap in System #30.
- **Future-proofing**: when ZER gets persistent-memory libraries, they'll be safe by construction.
- **Architectural cleanliness**: completes the 30-system safety roster ZER claims.
- **Demonstrating the model**: proves @atomic_*/cache_*/etc. integrate cleanly.

**Alternative options** (also valid):

**Option A — Ship full Phase 5 (66 hrs)**:
- Maximum safety guarantee
- Justifies "100% language-safe" claim from CLAUDE.md
- Investment in v1.0 readiness

**Option B — Ship minimum viable Phase 5 (10-15 hrs)**:
- Just classify intrinsics in `.zerdata` (Phase 1+2 already done)
- Skip enforcement entirely
- Document System #30 as "implementation pending"
- Phase 5 becomes 1-paragraph note instead of 66 hrs

**Option C — Defer Phase 5 entirely (0 hrs)**:
- Keep current state: data plumbing exists, enforcement deferred
- Add to "future work" with the design plan
- Revisit when first persistent-memory ZER library is being written

**Option D — Phase 5 lite (20-30 hrs)**:
- Only handle the asm path (skip @cache_*/@nt_*/@cond_*/@sem_*/@once classifications)
- Single-block CLWB→SFENCE check (rejected in round 3 audit, but very narrow scope)
- 80% of safety value, 30% of dev time
- Doesn't cover @cache_writeback (the most likely use)

**My recommendation**: Option C (defer). Reasons:
1. ZER currently has 0 persistent-memory users. Phase 5 catches bugs in code that doesn't exist yet.
2. The plan is well-documented; future implementer has a clear roadmap.
3. 66 hrs is significant; ZER has more impactful work.
4. When real users emerge, the plan is ready to implement.
5. The Phase 1+2 data plumbing (already shipped) provides the foundation.

**Counter-argument (for shipping now)**:
- Once you have users, breaking changes (new compile errors) are harder.
- "100% language-safe" claim needs Phase 5 to be technically true.
- Implementation knowledge is freshest now.

**This decision is YOURS to make.** Plan is sound; implementation is ready when you say go.

### 6.5 What if the plan FAILS?

Failure modes ranked by likelihood:

| Likelihood | Failure mode | Recovery |
|---|---|---|
| HIGH | Step 5.5 (intrinsic classification) takes 25% longer than estimated | Acceptable; ship over multiple sessions |
| MED | Iteration cap (32) hit on real programs | Raise to 64; fail-closed if exceeded |
| MED | Defer body REQUIRES_AFTER bug (round 6 finding) ships and gets reported | Add ban + fix in patch release |
| MED | LSP shows no Phase 5 errors (not wired) | Phase 6 work; document as known |
| LOW | Cross-module FuncSummary mismatch | Existing bug already, not new |
| LOW | Phase F migration (delete zercheck.c) regresses some test | Roll back; debug; re-attempt |
| LOW | Performance overhead causes user complaints | Add `--no-ordering-check` flag |

All failure modes have clear recovery paths. **No catastrophic failure mode identified.**

### 6.6 Round 6 verdict

**Plan is sound. Plan is strong.**

- Architecture is correct (1 small fix for defer-REQUIRES_AFTER, 10 LOC).
- Plan is implementable (15 sub-steps, ~66 hrs, each independently shippable).
- Test coverage is solid (~14 tests + 2 fuzzer generators recommended).
- Failure modes are bounded (all recoverable).

**But the value question is real**: 66 hrs for 1 current user (test file) is a big investment.

**My honest recommendation**: 

Ship Phase 5 as planned IF:
- You want to claim "100% language-safe" rigorously (System #30 must exist)
- You believe persistent-memory ZER libraries will emerge soon
- You have 66 hrs of implementer time and prefer paying it now over later
- You're optimizing for v1.0 readiness

DEFER Phase 5 IF:
- ZER has more impactful work (e.g., LSP, more intrinsics, better tooling)
- You're optimizing for current users (who don't use these patterns)
- You're OK with "Phase 5 is future work" for now
- You want to ship the data plumbing (already done) and document the rest

**Either choice is defensible. The plan stands either way.**

---

## Final cumulative state (across all 6 rounds)

| Metric | Value |
|---|---|
| Audit rounds | 6 |
| Distinct findings | 74 (added 1 in round 6: defer-REQUIRES_AFTER) |
| Critical findings | 6 |
| Final estimate | ~68 hrs (was 66; +2 hrs for defer LIFO simulation, was +1 for ban) |
| Architectural soundness | ✓ confirmed across 6 rounds |
| Implementation readiness | ✓ ready when go-ahead given |
| Real-world user surface | 1 test file (dalpha13) |
| Future user surface | TBD (when persistent-memory libraries emerge) |

**Plan: SOUND. STRONG. READY.**

Decision: ship now (66 hrs) vs defer (0 hrs, plan stays). User's call.

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
