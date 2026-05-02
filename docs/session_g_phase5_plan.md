# Session G Phase 5 — IR-level OrderingState Implementation Plan

**Date**: 2026-05-02 (planning)
**Status**: Design complete. Ready for implementation (estimated 35-50 hrs).
**Predecessors**: Session G Phase 1 (data plumbing) + Phase 2 (per-instruction classification) committed in `359731a`.
**Phase 3 abandoned**: see `docs/asm_preconditions_research.md` "Design gaps identified during failed G3 attempt".

This document is the single source of truth for Phase 5 implementation. Read this BEFORE writing code.

---

## 1. Architecture Overview

System #30 (Atomic Ordering) tracks happens-before relationships across the function CFG. It enforces: every C8 instruction or intrinsic that REQUIRES a barrier subsequently must be followed (along every CFG path to function exit) by a satisfying barrier.

The implementation extends `zercheck_ir.c`'s existing fixed-point CFG analysis with an `IROrderingState` field on `IRPathState`. Joins use set-intersection (most-conservative); pending requirements propagate by union.

Three sources of barriers tracked uniformly:

| Source | Examples | Today's IR shape |
|---|---|---|
| Inline asm with C8 mnemonic | `mfence`, `sfence`, `clwb`, `dmb sy`, `ldar`, `stlr`, `fence rw,rw` | `IR_NOP` with `inst.expr → NODE_ASM` |
| ZER `@atomic_*` intrinsics | `@atomic_load`, `@atomic_store`, `@atomic_cas`, `@atomic_add`, etc. | `IR_ASSIGN` (or `IR_INTRINSIC_DECOMP`) with `inst.expr → NODE_INTRINSIC` |
| ZER `@barrier_*` intrinsics | `@barrier()`, `@barrier_store()`, `@barrier_load()`, `@barrier_acq_rel()` | Same as above |

**Critical simplifying fact**: ZER's `@atomic_*` are all **SEQ_CST** today (verified at `emitter.c:2828-2870`). No ordering parameter is exposed to user. So every `@atomic_*` call produces FULL_MEMORY barrier semantics. No need to track per-call ordering; future work (if ZER adds `.acquire`/`.release` parameters) can refine.

---

## 2. Why this design (over alternatives)

| Alternative | Why rejected |
|---|---|
| Same-asm-block check (Phase G3) | False-positives on canonical multi-block CLWB+SFENCE pattern. Documented in `docs/asm_preconditions_research.md`. |
| Function-scope linear scan in `zercheck.c` | No CFG awareness — errors on `if (cond) { clwb } else { sfence; ... }` (set-intersection at join correctly catches missing barrier on `cond=true` path). Phase G migration also deletes `zercheck.c`. |
| Track in checker.c (AST-level) | AST has no CFG; can't model "this barrier reaches all exits". Same problem as zercheck.c. |
| **CFG-aware OrderingState in zercheck_ir.c** | Fits existing infrastructure. Joins already done correctly (set-intersection). Extends one tracking system; doesn't fight architecture. |

The architecture is identical to the existing handle-state tracking (`IRHandleInfo` + `ir_merge_states`). New state, same machinery.

---

## 3. Data structures

### 3.1 New types in `zercheck_ir.c`

```c
/* Bitmap encoding of barriers seen along this path. Same kinds as
 * ZerBarrierKind (asm_instruction_table.h) but as bitfield positions. */
typedef enum {
    IR_BBIT_NONE              = 0,
    IR_BBIT_FULL_MEMORY       = 1u << 1,
    IR_BBIT_STORE_STORE       = 1u << 2,
    IR_BBIT_LOAD_LOAD         = 1u << 3,
    IR_BBIT_LOAD_STORE        = 1u << 4,
    IR_BBIT_STORE_LOAD        = 1u << 5,
    IR_BBIT_RELEASE           = 1u << 6,
    IR_BBIT_ACQUIRE           = 1u << 7,
    IR_BBIT_ACQUIRE_RELEASE   = 1u << 8,
    IR_BBIT_INSTRUCTION_SYNC  = 1u << 9,
    IR_BBIT_IO_MEMORY         = 1u << 10,
    IR_BBIT_DMA_SYNC          = 1u << 11,
} IRBarrierBit;

/* One pending REQUIRES_AFTER requirement. Cleared when a satisfying
 * barrier is later seen on this path; carried forward at CFG joins. */
typedef struct {
    int origin_line;           /* source line of the requirement (CLWB site) */
    uint8_t needed_kind;       /* ZerBarrierKind expected */
    const char *origin_mnemonic; /* "clwb", "clflushopt" — for error msg */
    size_t origin_mn_len;
    const char *consequence;   /* from .zerdata */
    const char *source;        /* vendor citation */
    /* Dedup key: (origin_line, needed_kind, origin_mnemonic). Two pending
     * requirements with the same key are merged at join (no duplicates). */
} IROrderingPending;

/* Per-path ordering state. Lives inside IRPathState. */
typedef struct {
    uint32_t barriers_seen;        /* bitmap of IRBarrierBit */
    IROrderingPending *pending;    /* dynamic array */
    int pending_count;
    int pending_capacity;
} IROrderingState;
```

### 3.2 Extension to `IRPathState`

Add ONE field. Existing handle/thread tracking unchanged.

```c
typedef struct {
    IRHandleInfo *handles;
    int handle_count;
    int handle_capacity;
    bool terminated;
    int critical_depth;
    IRThreadTrack *threads;
    int thread_count;
    int thread_capacity;
    IROrderingState ordering;       /* NEW — Session G Phase 5 */
} IRPathState;
```

### 3.3 Operations

```c
static void ir_ord_init(IROrderingState *o);
static IROrderingState ir_ord_copy(const IROrderingState *src);
static void ir_ord_free(IROrderingState *o);

/* PRODUCES: add barrier to seen, discharge pending it satisfies. */
static void ir_ord_produces(IROrderingState *o, uint8_t kind);

/* REQUIRES_AFTER: register pending requirement (if not already satisfied). */
static void ir_ord_requires_after(IROrderingState *o,
    uint8_t kind, int line,
    const char *mnemonic, size_t mn_len,
    const char *consequence, const char *source);

/* CFG join. barriers_seen INTERSECTS, pending UNIONs (deduped). */
static IROrderingState ir_ord_merge(IROrderingState *states, int count);

/* True if any pending in `state` is unsatisfied at function exit. */
static bool ir_ord_has_unsatisfied(const IROrderingState *o);
```

---

## 4. Pure predicate (Level 3 VST extractable)

```c
/* src/safety/ordering_rules.c */
#include "ordering_rules.h"

/* Does barrier of `produced_kind` satisfy `required_kind`?
 *
 * Spec (System #30):
 *   - FULL_MEMORY subsumes any required kind
 *   - ACQUIRE_RELEASE subsumes ACQUIRE, RELEASE, FULL_MEMORY pairings
 *   - INSTRUCTION_SYNC and IO_MEMORY don't subsume STORE_STORE etc.
 *   - Otherwise, exact match required.
 *
 * Codes match ZerBarrierKind in asm_instruction_table.h.
 * Returns 1 if satisfied, 0 if not. */
int zer_barrier_satisfies(int produced_kind, int required_kind) {
    if (produced_kind == 0 || required_kind == 0) return 0;
    if (produced_kind == 1 /* FULL_MEMORY */) return 1;
    if (produced_kind == required_kind) return 1;
    if (produced_kind == 8 /* ACQUIRE_RELEASE */) {
        if (required_kind == 6 /* RELEASE */) return 1;
        if (required_kind == 7 /* ACQUIRE */) return 1;
    }
    return 0;
}

/* Bitmap variant for IROrderingState.barriers_seen. */
int zer_barriers_seen_satisfies(uint32_t bitmap, int required_kind) {
    if (bitmap & (1u << 1) /* FULL_MEMORY */) return 1;
    if (bitmap & (1u << required_kind)) return 1;
    if ((bitmap & (1u << 8) /* ACQUIRE_RELEASE */) &&
        (required_kind == 6 || required_kind == 7)) return 1;
    return 0;
}
```

VST proof: simple destruct on (produced_kind, required_kind) values. ~40 lines of Coq.

---

## 5. Tracking event handlers

### 5.1 Asm events (extend existing IR_NOP/NODE_ASM handler)

Today `zercheck_ir.c:1198-1261` walks structured asm operands for Z1/Z2. Add a sibling block that walks the `instructions` string for ordering effects.

Critical: avoid duplicating the mnemonic scanner from checker.c. Extract into a helper.

**Step 1: extract mnemonic walker**

New file `src/safety/asm_mnemonic_walk.c`:

```c
/* Walk the asm `instructions` string, calling `cb` for each
 * recognized mnemonic with its instruction-table classification. */

typedef void (*ZerAsmMnemonicCallback)(
    void *ctx,
    const char *mnemonic, size_t mn_len,
    int line,
    const ZerInstructionInfo *info);

void zer_asm_walk_mnemonics(
    const char *s, size_t len,
    ZerArchId arch,
    int source_line,
    void *ctx,
    ZerAsmMnemonicCallback cb)
{
    /* Same scanner as checker.c NODE_ASM dispatch:
     * skip whitespace, detect `.`-directives, read mnemonic
     * token, call zer_asm_instruction_info, invoke cb. */
    /* ~80 lines, almost identical to checker.c lines 9998-10030. */
}
```

Update checker.c to use this helper (keep its existing dispatch logic in the callback). Saves ~80 LOC of duplicated parser.

**Step 2: add ordering callback in zercheck_ir.c**

```c
typedef struct {
    IRPathState *ps;
    ZerCheck *zc;
    bool has_memory_clobber;
} IRAsmOrderingCtx;

static void ir_asm_ordering_cb(void *ctx_p,
    const char *mn, size_t mn_len, int line,
    const ZerInstructionInfo *info)
{
    IRAsmOrderingCtx *ctx = ctx_p;
    if (!info) return;
    if (info->ordering.role == 1 /* PRODUCES */) {
        ir_ord_produces(&ctx->ps->ordering, info->ordering.kind);
    } else if (info->ordering.role == 3 /* REQUIRES_AFTER */) {
        ir_ord_requires_after(&ctx->ps->ordering,
            info->ordering.kind, line,
            mn, mn_len,
            info->consequence, info->source);
    }
    /* role == 2 (REQUIRES_BEFORE): not yet used; reserved for future. */
}

/* In ir_check_inst, IR_NOP/NODE_ASM branch (after Z1/Z2 walk): */
if (asm_node->asm_stmt.instructions) {
    IRAsmOrderingCtx octx = { ps, zc, has_memory_clobber(asm_node) };
    zer_asm_walk_mnemonics(
        asm_node->asm_stmt.instructions,
        asm_node->asm_stmt.instructions_len,
        ctx->checker_arch,
        inst->source_line,
        &octx, ir_asm_ordering_cb);
    if (octx.has_memory_clobber) {
        /* Memory clobber = pessimistic full barrier. Add even if
         * mnemonic walker already classified the asm. Idempotent. */
        ir_ord_produces(&ps->ordering, /* FULL_MEMORY */ 1);
    }
}
```

### 5.2 Atomic / barrier intrinsic events

Atomics today flow through IR as `IR_ASSIGN` with `inst.expr->kind == NODE_INTRINSIC`. Detection at IR_ASSIGN:

```c
/* In ir_check_inst, IR_ASSIGN handler (NEW block, after existing logic): */
static void ir_check_intrinsic_ordering(IRPathState *ps, Node *expr) {
    if (!expr || expr->kind != NODE_INTRINSIC) return;
    const char *name = expr->intrinsic.name;
    int nlen = expr->intrinsic.name_len;
    int line = expr->loc.line;

    /* @atomic_load/store/cas/add/sub/or/and/xor — all SEQ_CST today.
     * Treat as PRODUCES FULL_MEMORY. */
    if (nlen >= 7 && memcmp(name, "atomic_", 7) == 0) {
        ir_ord_produces(&ps->ordering, /* FULL_MEMORY */ 1);
        return;
    }

    /* Explicit barrier intrinsics. */
    if (nlen == 7 && memcmp(name, "barrier", 7) == 0)
        ir_ord_produces(&ps->ordering, 1 /* FULL_MEMORY */);
    else if (nlen == 13 && memcmp(name, "barrier_store", 13) == 0)
        ir_ord_produces(&ps->ordering, 2 /* STORE_STORE */);
    else if (nlen == 12 && memcmp(name, "barrier_load", 12) == 0)
        ir_ord_produces(&ps->ordering, 3 /* LOAD_LOAD */);
    else if (nlen == 17 && memcmp(name, "barrier_acq_rel", 15) == 0)
        ir_ord_produces(&ps->ordering, 8 /* ACQUIRE_RELEASE */);
}
```

Called from both `IR_ASSIGN` and `IR_INTRINSIC_DECOMP` branches.

### 5.3 CFG joins (extend `ir_merge_states`)

Add ordering merge at end of existing function:

```c
/* Existing handle/thread merge above. NEW: ordering merge. */
{
    IROrderingState *ords = malloc(state_count * sizeof(IROrderingState));
    int live_count = 0;
    for (int i = 0; i < state_count; i++) {
        if (states[i].terminated) continue;
        ords[live_count++] = states[i].ordering;
    }
    result.ordering = ir_ord_merge(ords, live_count);
    free(ords);
}
```

`ir_ord_merge` semantics:
- `barriers_seen = AND(all preds)` (set-intersection — most-conservative)
- `pending = UNION(all preds, deduped by (origin_line, needed_kind))` (carry forward)

After UNION, run a discharge pass: for each pending P, check if `barriers_seen` already satisfies. If so, drop P. (This avoids spurious errors when one path satisfied the requirement and barriers_seen still has the bit.)

**Subtle correctness point**: UNION of pending is "any path that hadn't satisfied requires it after merge." Discharge after merge handles the case where both paths produced satisfying barriers but the pending was registered before they did.

### 5.4 Function-exit check

Extend `zercheck_ir`'s post-fixed-point pass (currently lines 2851-2900) to scan for unsatisfied pending at exit blocks:

```c
/* After final pass, scan return-terminated blocks for unsatisfied pending. */
for (int bi = 0; bi < func->block_count; bi++) {
    IRBlock *bb = &func->blocks[bi];
    if (bb->inst_count == 0) continue;
    IRInst *last = &bb->insts[bb->inst_count - 1];
    if (last->op != IR_RETURN) continue;
    IROrderingState *o = &block_states[bi].ordering;
    for (int pi = 0; pi < o->pending_count; pi++) {
        IROrderingPending *p = &o->pending[pi];
        if (zer_barriers_seen_satisfies(o->barriers_seen, p->needed_kind)) continue;
        ir_zc_error(zc, p->origin_line,
            "asm '%.*s' requires barrier subsequently but no satisfying "
            "barrier reaches function exit on this path — consequence: %s "
            "(cite: %s)",
            (int)p->origin_mn_len, p->origin_mnemonic,
            p->consequence ? p->consequence : "ordering UB",
            p->source ? p->source : "no source");
    }
}
```

---

## 6. Inter-procedural: extend FuncProps (System #29)

**Decision: include in Phase 5.** Real persistent-memory libraries do this:

```
asm { instructions: "clwb (%0)" ... }   // in function `pmem_persist`
pmem_drain();                            // separate function emits SFENCE
```

Without inter-procedural, Phase 5 errors on `pmem_persist` even though `pmem_drain` produces FULL_MEMORY. To avoid this false-positive:

### 6.1 Extension to FuncSummary

Add `barriers_produced` bitmap to the existing FuncSummary struct (in `checker.h` or wherever FuncProps lives — locate during impl).

```c
typedef struct {
    /* ... existing FuncProps fields ... */
    uint32_t barriers_produced;  /* IRBarrierBit bitmap */
} FuncSummary;
```

### 6.2 Build-time computation

When zercheck_ir analyzes a function, it already builds the path states. After analysis, compute:

```c
uint32_t produced = 0;
for each exit block bi:
    produced |= block_states[bi].ordering.barriers_seen;
func_sym->summary.barriers_produced = produced;
```

This captures "every barrier kind that's reached on every CFG path to exit."

### 6.3 Use at call sites

In `ir_check_inst` IR_CALL / IR_CALL_DECOMP handler, look up callee's summary:

```c
case IR_CALL: case IR_CALL_DECOMP: {
    /* ... existing call handling ... */
    Symbol *callee = lookup_callee(inst);
    if (callee && callee->summary.barriers_produced != 0) {
        /* Treat call as if it produces these barriers. */
        for (int b = 1; b <= 11; b++) {
            if (callee->summary.barriers_produced & (1u << b)) {
                ir_ord_produces(&ps->ordering, b);
            }
        }
    }
}
```

This requires DFS topological ordering of analysis (analyze callees before callers). zercheck_ir already does this via FuncSummary lazy computation.

### 6.4 Recursion handling

For recursive cycles, FuncSummary uses pessimistic initial value. For ordering, pessimistic = `barriers_produced = 0` (assume no barriers). This is correct (no guarantee = treat as no production).

---

## 7. Files & LOC estimate

| File | Action | LOC | Notes |
|---|---|---|---|
| `src/safety/asm_mnemonic_walk.c` | NEW | ~100 | Extracted scanner |
| `src/safety/asm_mnemonic_walk.h` | NEW | ~30 | API |
| `src/safety/ordering_rules.c` | NEW | ~50 | Pure predicates |
| `src/safety/ordering_rules.h` | NEW | ~25 | API + bit constants |
| `proofs/vst/verif_ordering_rules.v` | NEW | ~60 | VST proof |
| `zercheck_ir.c` | MODIFY | +250 | OrderingState tracking |
| `checker.c` | MODIFY | -80, +30 | Refactor mnemonic loop to helper |
| `checker.h` | MODIFY | +5 | FuncSummary.barriers_produced field |
| `Makefile` | MODIFY | +3 | New CORE_SRCS entries |
| `tests/zer/asm_g5_*.zer` | NEW (×4) | 4 files | Positive cases |
| `tests/zer_fail/asm_g5_*.zer` | NEW (×3) | 3 files | Negative cases |
| `BUGS-FIXED.md` | UPDATE | ~60 | Session entry |
| `CLAUDE.md` | UPDATE | ~20 | Stage 5 status |
| `docs/asm_preconditions_research.md` | UPDATE | ~40 | Implementation notes |

**Total: ~525 lines new code + ~150 lines modifications + 7 test files + 4 doc updates.**

---

## 8. Test plan

### 8.1 Positive (must compile + run + exit 0)

| File | Pattern | Why it must pass |
|---|---|---|
| `asm_g5_clwb_then_sfence.zer` | CLWB asm block, then SFENCE asm block | Canonical libpmem idiom |
| `asm_g5_clwb_then_atomic.zer` | CLWB asm block, then `@atomic_store(p, v)` | SEQ_CST atomic = FULL_MEMORY satisfies |
| `asm_g5_clwb_then_barrier.zer` | CLWB asm block, then `@barrier_store()` | Explicit ZER barrier intrinsic |
| `asm_g5_clwb_then_mfence.zer` | CLWB asm block, then MFENCE asm block | FULL_MEMORY subsumes STORE_STORE |
| `asm_g5_clwb_in_callee.zer` | CLWB in `f1`, caller calls `f1` then `pmem_drain()` (which emits SFENCE) | Inter-procedural via FuncSummary |
| `asm_g5_acquire_release_pair.zer` | LDAR somewhere, STLR somewhere | ARM acquire/release pairing |
| `asm_g5_mfence_only.zer` | MFENCE alone, no preceding CLWB | No spurious error |

### 8.2 Negative (must fail to compile)

| File | Pattern | Expected error |
|---|---|---|
| `asm_g5_clwb_no_sfence.zer` | CLWB asm block, function returns | "requires barrier subsequently" |
| `asm_g5_clwb_lfence_only.zer` | CLWB then LFENCE (LOAD_LOAD doesn't satisfy STORE_STORE) | Same |
| `asm_g5_clwb_branch_one_only.zer` | `if (cond) { clwb; sfence } else { clwb; }` — second branch lacks SFENCE | Same — set-intersection at exit catches this |
| `asm_g5_clwb_callee_no_barrier.zer` | CLWB in caller, calls function that does no barriers, returns | Same — FuncSummary shows callee produces nothing |

### 8.3 Cross-arch tests (cross-arch test runner)

| File | Arch | Pattern |
|---|---|---|
| `asm_g5_arm_ldar_stlr.zer` | aarch64 | LDAR/STLR pair |
| `asm_g5_riscv_fence_i.zer` | riscv64 | FENCE.I after self-modifying code (positive — has FENCE.I) |

---

## 9. Edge cases and risks

### 9.1 Memory clobber as full barrier

ZER's asm with `clobbers: ["memory"]` is conservatively treated as FULL_MEMORY. Real GCC inline asm with memory clobber generates a `__asm__ __volatile__` that prevents reordering across it but doesn't always emit a hardware fence. ZER's SAFETY claim is at the LANGUAGE level (compiler reordering); the hardware barrier is the user's responsibility unless they used a known barrier mnemonic.

**Decision**: classify memory-clobber as PRODUCES FULL_MEMORY. Compiler-level safety only. Matches existing `__atomic_thread_fence(__ATOMIC_SEQ_CST)` semantics.

### 9.2 Pending bound (memory exhaustion)

A pathological function with 1000+ CLWB and no SFENCE would have 1000+ pending. Cap at 32; on overflow, drop new pending and log a warning. Safe-by-design (can't OOM the analyzer).

### 9.3 Convergence

The lattice is finite: `barriers_seen` is a 12-bit bitmap (4096 values), pending is bounded at 32. Worst-case fixed-point convergence: O(blocks × 4096 × 32) iterations. zercheck_ir's existing 32-iteration cap should hold; if it doesn't, fail-closed (error: "ordering analysis didn't converge — refusing to compile").

### 9.4 SEQ_CST atomic on x86 doesn't always emit MFENCE

GCC's `__atomic_store_n(ptr, v, __ATOMIC_SEQ_CST)` may emit just `MOV` on x86 (TSO model). Whether this still constitutes FULL_MEMORY depends on the C++ memory model definition. ZER's safety claim is at the language level: SEQ_CST = FULL_MEMORY. If the user wants hardware-level SFENCE, they use `asm("sfence")`.

### 9.5 Multi-arch concurrent compile

The arch is per-compile (`--target-arch=`). Phase 5 reads `c->target_arch`. No multi-arch.

### 9.6 zercheck.c (AST) doesn't have these checks

Phase 5 fires from zercheck_ir only. Today (Phase F) zercheck_ir runs unconditionally; its errors ARE compile errors. So Phase 5 fires correctly without needing zercheck.c support. After Phase G migration, zercheck.c is deleted; Phase 5 remains.

### 9.7 Dual-run regression

Adding new error class to zercheck_ir may cause new "regression" disagreements vs zercheck.c (which doesn't have the check). zercheck.c will see no error; zercheck_ir will see one. The dual-run reporter logs disagreement but doesn't fail compile (zercheck_ir's error already fails compile via shared error counter). Need to verify the disagreement logger doesn't double-report.

---

## 10. Implementation order (sub-phases)

Each step independently shippable. Test count grows monotonically.

| Step | Scope | Effort | Tests added | Risk |
|---|---|---|---|---|
| **5.1** | Extract `asm_mnemonic_walk` helper; refactor checker.c to use it; verify 538/538 still passes | 4 hrs | 0 | Low (pure refactor) |
| **5.2** | Add `ordering_rules.c` + VST proof | 4 hrs | 0 | Low (pure predicate) |
| **5.3** | Add `IROrderingState` to `IRPathState`; init/copy/free/merge plumbing; verify still passes | 4 hrs | 0 | Low (state plumbing) |
| **5.4** | Wire asm event tracking (5.1 callback); positive test for "asm with C8 mnemonic doesn't break compile" | 6 hrs | 1 positive | Med |
| **5.5** | Wire intrinsic event tracking; positive test for "@atomic_/`@barrier_*` doesn't break compile" | 4 hrs | 1 positive | Low |
| **5.6** | Add function-exit pending check; FIRST negative test (CLWB at end of function with no SFENCE) | 6 hrs | 1 negative + 2 positive | Med-High |
| **5.7** | Add CFG join discharge logic; add branch-asymmetric test (CLWB on one branch, SFENCE only on other → error) | 4 hrs | 1 negative + 1 positive | Med |
| **5.8** | Extend FuncSummary with `barriers_produced`; cross-function positive test | 6 hrs | 1 positive + 1 negative | Med-High |
| **5.9** | Cross-arch tests (aarch64 LDAR/STLR, riscv64 FENCE patterns) | 3 hrs | 2 positive | Low |
| **5.10** | Documentation (BUGS-FIXED.md, CLAUDE.md, design doc) + final make check | 2 hrs | — | Low |

**Total: 43 hrs (matches the ~35-50 hr revised estimate).**

---

## 11. Open questions for fresh sessions

1. **Should @atomic_* with relaxed/acquire/release variants be added to ZER first?**
   - Today all are SEQ_CST. Adding `.acquire`/`.release` parameters is independent work. Phase 5 doesn't need it.
   - If added later, only the intrinsic detection logic in 5.5 needs updating to read the ordering parameter.

2. **Should we add `@asm_barrier(KIND)` annotation for unclassified asm?**
   - User-asserted barrier marker for asm not in our table. e.g., `@asm_barrier(STORE_STORE)` after a custom asm block.
   - Defer to Phase 6 — first prove the architecture works on classified instructions.

3. **Should ARM DMB SY/ISH/OSH be distinguished?**
   - Today all DMB variants flatten to FULL_MEMORY. Real ARM code uses scoped variants (DMB ISHST) for performance.
   - Phase 5 design supports finer granularity: read DMB's IMMEDIATE operand, classify as ZER_BARRIER_STORE_STORE if `ST` modifier, etc.
   - Defer — flat FULL_MEMORY is correct (over-conservative). Refinement when users actually need it.

4. **Should `--relax-asm` disable Phase 5?**
   - Yes. `--relax-asm` already exists per CLAUDE.md; it should disable all Session G enforcement when set.
   - Also add per-block `@relax_check(SESSION_G_ORDERING)` for fine-grained opt-out.

5. **Phase 5 error wording — error vs warning?**
   - Recommendation: ERROR by default. User can downgrade via `--warn-only-asm-ordering=true` if needed.
   - Reasoning: ZER's "fail closed" philosophy. SFENCE missing is a real correctness issue for NVDIMM.

6. **Should @atomic_thread_fence be exposed?**
   - Currently @barrier* maps to __atomic_thread_fence. They ARE the explicit thread fence.
   - No change needed.

---

## 12. Success criteria

1. **All existing tests still pass**: 538/538 ZER + 200 fuzz + 139 conversion + 5 cross-arch.
2. **New tests added**: 7 positive + 4 negative + 2 cross-arch.
3. **Real safety value**: at least one negative test catches a real silent UB pattern (CLWB without subsequent SFENCE on at least one CFG path).
4. **No false positives**: positive tests cover multi-block, cross-function, branch-asymmetric, and inter-procedural cases.
5. **Level 3 VST**: `verif_ordering_rules.v` is admit-free.
6. **Convergence**: existing 32-iteration cap holds. If not, fail-closed error.
7. **Architecture clean**: asm scanner deduplicated between checker.c and zercheck_ir.c via `asm_mnemonic_walk` helper.

---

## 13. Out of scope (Phase 6+)

- `.aq`/`.rl` modifier parsing for RISC-V atomics (`lr.w.aq`, `sc.w.rl`, `amoadd.w.aq.rl`)
- Fine-grained ARM DMB variant tracking (SY/ISH/OSH × LD/ST)
- @asm_barrier user annotation
- @relax_check directive
- Self-modifying code FENCE.I check (ZER doesn't expose self-modifying code today)
- Cross-thread happens-before (ZER's `shared struct` already provides lock-based ordering)
- @verified_spec algorithm-correctness proofs

---

## 14. Why this is the right shape

- **Reuses existing infrastructure** — `IRPathState`, `ir_merge_states`, fixed-point loop. One new field, three new helpers.
- **One source of truth** for asm mnemonic classification — `asm_mnemonic_walk` callable from both checker.c and zercheck_ir.c.
- **Matches the existing safety model** — Models 1-4 from CLAUDE.md. New State Machine on whole-program-point (per the design doc — same shape as the 7 existing CFG-traversed state machines, distinct because state is global not per-entity).
- **Inter-procedural via FuncSummary** — extends an existing system (System #29), not new infrastructure.
- **Level 3 VST extractable** — pure predicate file, follows established pattern.
- **Fail-closed on convergence** — same pattern as existing zercheck_ir bounded iteration.
- **Honest about scope** — out-of-scope items listed; Phase 6 is identified.
- **Test coverage proven** — 11 tests cover positive, negative, cross-block, cross-function, cross-arch, branch-asymmetric.

This is the smallest design that doesn't false-positive on any canonical pattern. Bigger designs (whole-program data-flow, dependent-store tracking) are deferred to Phase 7+ if real ZER programs prove they're needed.
