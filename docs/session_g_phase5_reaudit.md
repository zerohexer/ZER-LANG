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

## Bottom-line verdict

The original plan had:
- **One critical architectural issue (#1)**: resolved by user clarification + Step 5.0 (5 LOC change).
- **One newly-found bug (#19)**: convergence loop early-exit. Fixed by Step 5.0b (~10 LOC).
- **Several smaller issues (#2-#23)**: each documented and resolved or scheduled.

The architecture itself (CFG-aware OrderingState in zercheck_ir, set-intersection at joins, FuncSummary integration with `barriers_produced`) is correct and matches existing patterns.

**Plan is solid after re-audit. Step 5.0 + 5.0b are prerequisites; rest can proceed in original order.**

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
