/*
 * ZER VRP on IR — Value Range Propagation on basic blocks
 *
 * Tracks {min, max, known_nonzero} per LOCAL id per basic block.
 * Merges at join points (intersect ranges from predecessors).
 * Replaces the checker's manual VarRange tracking.
 *
 * Phase 7 of IR implementation. See docs/IR_Implementation.md Part 6.
 *
 * Status: FOUNDATION — core range tracking framework. Does NOT yet
 * replace the AST VRP. Both coexist during migration.
 *
 * ================================================================
 * STATE OF THIS FILE, MEASURED 2026-08-14 — read before "just wiring it up"
 * ================================================================
 *
 * This file is **DEAD CODE**. It is not in the Makefile and nothing references
 * `vrp_ir` / `vrp_ir_free` anywhere in the tree (`grep -rn vrp_ir --include=*.c
 * --include=*.h --include=Makefile .` matches only this file). It is Phase 0 of
 * docs/unified-oracle-proved-ZER.md, so it is kept deliberately rather than
 * deleted — but a future session should know exactly what it is inheriting:
 *
 * 1. **It still compiles** against the current headers (`gcc -Wall -Wextra
 *    -std=c99 -I. -c vrp_ir.c`, exit 0), so it has not bit-rotted against the IR
 *    API. That is the good news, and it is the only part that was verified to
 *    still hold.
 *
 * 2. **The two functions that answer the actual safety questions are unused even
 *    WITHIN this file** — `ir_range_proves_safe` and `ir_range_proves_nonzero`
 *    are `-Wunused-function`. The pass computes ranges and then discards the
 *    answers; nothing ever asks "is this index in bounds?".
 *
 * 3. **`IRVRPResult` cannot answer the question a consumer needs.** It records
 *    `(local_id, block_id, array_size = range.max + 1)`, conflating "the largest
 *    value this local can hold" with "the size of the array it is safe to index".
 *    The emitter's real query is per-INDEX-NODE ("may I elide the guard at THIS
 *    access?"), which this shape cannot express. Wiring it up therefore means
 *    redesigning the result type, not just calling it.
 *
 * 4. **The merge comment contradicts the merge code — the CODE is right.** The
 *    block comment below says "intersect ranges from predecessors"; the
 *    implementation takes the UNION (min of mins, max of maxes). Union is the
 *    correct, SOUND direction for a join: it widens, so it can only ever ADD a
 *    bounds guard, never remove one. Do not "fix" the code to match the comment —
 *    that would make the analysis unsound. (The comment is left in place with
 *    this note rather than edited, so the discrepancy stays visible to anyone
 *    reading the two side by side.)
 *
 * Consequence: this is a SKELETON, not a working pass held back by a missing
 * call site. Treat items 2 and 3 as the actual Phase 0 work.
 */

#include "ir.h"
#include "checker.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ================================================================
 * Range per LOCAL id
 * ================================================================ */

typedef struct {
    int64_t min;
    int64_t max;
    bool known_nonzero;
    bool valid;             /* false = unknown range */
    bool address_taken;     /* &x taken — invalidated at this point */
} IRRange;

/* Per-block range state: one range per local */
typedef struct {
    IRRange *ranges;        /* array[local_count] */
    int local_count;
} IRRangeState;

static void ir_range_init(IRRangeState *rs, int local_count) {
    rs->local_count = local_count;
    rs->ranges = (IRRange *)calloc(local_count, sizeof(IRRange));
}

static void ir_range_free(IRRangeState *rs) {
    free(rs->ranges);
    rs->ranges = NULL;
}

static IRRangeState ir_range_copy(IRRangeState *src) {
    IRRangeState dst;
    dst.local_count = src->local_count;
    dst.ranges = (IRRange *)malloc(src->local_count * sizeof(IRRange));
    memcpy(dst.ranges, src->ranges, src->local_count * sizeof(IRRange));
    return dst;
}

/* ================================================================
 * Range Operations
 * ================================================================ */

/* Set range for a local */
static void ir_set_range(IRRangeState *rs, int local_id,
                          int64_t min, int64_t max, bool nonzero) {
    if (local_id < 0 || local_id >= rs->local_count) return;
    rs->ranges[local_id].min = min;
    rs->ranges[local_id].max = max;
    rs->ranges[local_id].known_nonzero = nonzero;
    rs->ranges[local_id].valid = true;
}

/* Invalidate range (after &x or untrackable assignment) */
static void ir_invalidate_range(IRRangeState *rs, int local_id) {
    if (local_id < 0 || local_id >= rs->local_count) return;
    rs->ranges[local_id].valid = false;
}

/* Mark address taken — permanently invalid for this block */
static void ir_mark_address_taken(IRRangeState *rs, int local_id) {
    if (local_id < 0 || local_id >= rs->local_count) return;
    rs->ranges[local_id].address_taken = true;
    rs->ranges[local_id].valid = false;
}

/* Check if range proves an index is in bounds */
static bool ir_range_proves_safe(IRRangeState *rs, int local_id, int64_t array_size) {
    if (local_id < 0 || local_id >= rs->local_count) return false;
    IRRange *r = &rs->ranges[local_id];
    if (!r->valid) return false;
    return r->min >= 0 && r->max < array_size;
}

/* Check if range proves divisor is nonzero */
static bool ir_range_proves_nonzero(IRRangeState *rs, int local_id) {
    if (local_id < 0 || local_id >= rs->local_count) return false;
    IRRange *r = &rs->ranges[local_id];
    if (!r->valid) return false;
    return r->known_nonzero || (r->min > 0) || (r->max < 0);
}

/* ================================================================
 * Merge at Join Points
 *
 * At a block with multiple predecessors, merge ranges:
 * - Both valid + overlap → intersect (wider range)
 * - One valid, one invalid → invalid (conservative)
 * - Both address_taken → address_taken
 * ================================================================ */

static IRRangeState ir_merge_ranges(IRRangeState *states, int state_count, int local_count) {
    IRRangeState result;
    ir_range_init(&result, local_count);

    if (state_count == 0) return result;
    if (state_count == 1) {
        ir_range_free(&result);
        return ir_range_copy(&states[0]);
    }

    for (int li = 0; li < local_count; li++) {
        bool any_valid = false;
        bool all_valid = true;
        bool any_addr = false;
        int64_t merged_min = INT64_MAX;
        int64_t merged_max = INT64_MIN;
        bool merged_nonzero = true; /* all must be nonzero */

        for (int si = 0; si < state_count; si++) {
            IRRange *r = &states[si].ranges[li];
            if (r->address_taken) any_addr = true;
            if (r->valid) {
                any_valid = true;
                if (r->min < merged_min) merged_min = r->min;
                if (r->max > merged_max) merged_max = r->max;
                if (!r->known_nonzero) merged_nonzero = false;
            } else {
                all_valid = false;
            }
        }

        result.ranges[li].address_taken = any_addr;
        if (any_addr) {
            result.ranges[li].valid = false;
        } else if (all_valid && any_valid) {
            /* Widen: take union of ranges (conservative) */
            result.ranges[li].min = merged_min;
            result.ranges[li].max = merged_max;
            result.ranges[li].known_nonzero = merged_nonzero;
            result.ranges[li].valid = true;
        } else {
            result.ranges[li].valid = false;
        }
    }

    return result;
}

/* ================================================================
 * Derive Range from Expression
 *
 * For: %x = expr, derive what range %x has after the assignment.
 * Handles: literals, x % N → [0, N-1], x & MASK → [0, MASK]
 * ================================================================ */

static void ir_derive_range(IRRangeState *rs, int dest_local, Node *expr, IRFunc *func) {
    if (!expr || dest_local < 0) return;
    if (rs->ranges[dest_local].address_taken) return; /* can't narrow */

    switch (expr->kind) {
    case NODE_INT_LIT:
        ir_set_range(rs, dest_local,
                     (int64_t)expr->int_lit.value,
                     (int64_t)expr->int_lit.value,
                     expr->int_lit.value != 0);
        break;

    case NODE_BINARY:
        /* x % N → [0, N-1] */
        if (expr->binary.op == TOK_PERCENT && expr->binary.right &&
            expr->binary.right->kind == NODE_INT_LIT &&
            expr->binary.right->int_lit.value > 0) {
            int64_t n = (int64_t)expr->binary.right->int_lit.value;
            ir_set_range(rs, dest_local, 0, n - 1, false);
        }
        /* x & MASK → [0, MASK] */
        else if (expr->binary.op == TOK_AMP && expr->binary.right &&
                 expr->binary.right->kind == NODE_INT_LIT) {
            /* Bitwise AND: x & MASK → [0, MASK] */
            int64_t mask = (int64_t)expr->binary.right->int_lit.value;
            ir_set_range(rs, dest_local, 0, mask, false);
        }
        else {
            ir_invalidate_range(rs, dest_local); /* unknown binary */
        }
        break;

    case NODE_IDENT: {
        /* Copy range from source variable */
        int src = ir_find_local(func, expr->ident.name, (uint32_t)expr->ident.name_len);
        if (src >= 0 && src < rs->local_count && rs->ranges[src].valid) {
            rs->ranges[dest_local] = rs->ranges[src];
        } else {
            ir_invalidate_range(rs, dest_local);
        }
        break;
    }

    case NODE_UNARY:
        /* &x → mark x as address_taken */
        if (expr->unary.op == TOK_AMP && expr->unary.operand &&
            expr->unary.operand->kind == NODE_IDENT) {
            int target = ir_find_local(func,
                expr->unary.operand->ident.name,
                (uint32_t)expr->unary.operand->ident.name_len);
            ir_mark_address_taken(rs, target);
        }
        ir_invalidate_range(rs, dest_local);
        break;

    default:
        ir_invalidate_range(rs, dest_local);
        break;
    }
}

/* ================================================================
 * Main VRP Pass — walk CFG, track ranges
 * ================================================================ */

/* Results: which nodes are proven safe (for emitter to skip bounds check) */
typedef struct {
    int local_id;
    int block_id;
    int64_t array_size;     /* proven safe for this size */
} IRProvenSafe;

typedef struct {
    IRProvenSafe *entries;
    int count;
    int capacity;
} IRVRPResult;

IRVRPResult *vrp_ir(IRFunc *func) {
    if (!func || func->block_count == 0 || func->local_count == 0) return NULL;

    int lc = func->local_count;
    IRRangeState *block_ranges = (IRRangeState *)calloc(func->block_count, sizeof(IRRangeState));
    for (int bi = 0; bi < func->block_count; bi++)
        ir_range_init(&block_ranges[bi], lc);

    /* Fixed-point iteration over CFG */
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 32) {
        changed = false;
        iterations++;

        for (int bi = 0; bi < func->block_count; bi++) {
            IRBlock *bb = &func->blocks[bi];

            /* Merge predecessor ranges */
            IRRangeState merged;
            if (bb->pred_count == 0) {
                ir_range_init(&merged, lc);
            } else {
                IRRangeState *pred_rs = (IRRangeState *)calloc(bb->pred_count, sizeof(IRRangeState));
                for (int pi = 0; pi < bb->pred_count; pi++)
                    pred_rs[pi] = ir_range_copy(&block_ranges[bb->preds[pi]]);
                merged = ir_merge_ranges(pred_rs, bb->pred_count, lc);
                for (int pi = 0; pi < bb->pred_count; pi++)
                    ir_range_free(&pred_rs[pi]);
                free(pred_rs);
            }

            /* Process instructions */
            for (int ii = 0; ii < bb->inst_count; ii++) {
                IRInst *inst = &bb->insts[ii];

                switch (inst->op) {
                case IR_ASSIGN:
                    if (inst->dest_local >= 0 && inst->expr) {
                        ir_derive_range(&merged, inst->dest_local, inst->expr, func);
                    }
                    break;

                case IR_CALL:
                    /* Function calls may modify globals — invalidate non-const ranges.
                     * For now: invalidate all address-taken locals. */
                    for (int li = 0; li < lc; li++) {
                        if (merged.ranges[li].address_taken)
                            merged.ranges[li].valid = false;
                    }
                    break;

                default:
                    break;
                }
            }

            /* Check if state changed */
            for (int li = 0; li < lc; li++) {
                if (merged.ranges[li].valid != block_ranges[bi].ranges[li].valid ||
                    (merged.ranges[li].valid &&
                     (merged.ranges[li].min != block_ranges[bi].ranges[li].min ||
                      merged.ranges[li].max != block_ranges[bi].ranges[li].max))) {
                    changed = true;
                    break;
                }
            }

            ir_range_free(&block_ranges[bi]);
            block_ranges[bi] = merged;
        }
    }

    /* Collect results — which locals have proven ranges at which blocks */
    IRVRPResult *result = (IRVRPResult *)calloc(1, sizeof(IRVRPResult));
    result->capacity = 16;
    result->entries = (IRProvenSafe *)calloc(result->capacity, sizeof(IRProvenSafe));

    for (int bi = 0; bi < func->block_count; bi++) {
        for (int li = 0; li < lc; li++) {
            if (block_ranges[bi].ranges[li].valid) {
                if (result->count >= result->capacity) {
                    result->capacity *= 2;
                    result->entries = (IRProvenSafe *)realloc(
                        result->entries, result->capacity * sizeof(IRProvenSafe));
                }
                result->entries[result->count].local_id = li;
                result->entries[result->count].block_id = bi;
                result->entries[result->count].array_size =
                    block_ranges[bi].ranges[li].max + 1; /* max valid index + 1 = array size */
                result->count++;
            }
        }
    }

    /* Cleanup */
    for (int bi = 0; bi < func->block_count; bi++)
        ir_range_free(&block_ranges[bi]);
    free(block_ranges);

    return result;
}

void vrp_ir_free(IRVRPResult *result) {
    if (result) {
        free(result->entries);
        free(result);
    }
}
