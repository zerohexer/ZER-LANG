/*
 * ZER-CHECK on IR — Path-sensitive safety analysis on basic blocks
 *
 * This is the IR-based replacement for zercheck.c's AST walking.
 * Uses IRFunc's basic blocks as a real CFG instead of linear AST walk.
 * Handle states tracked per LOCAL id instead of string keys.
 *
 * Phase 6 of IR implementation. See docs/IR_Implementation.md Part 6.
 *
 * Status: FOUNDATION — core handle tracking framework. Does NOT yet
 * replace the AST zercheck. Both coexist during migration.
 */

#include "ir.h"
#include "zercheck.h"
#include "checker.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* Local error reporting — mirrors zercheck.c's zc_error but accessible here */
static void ir_zc_error(ZerCheck *zc, int line, const char *fmt, ...) {
    if (zc->building_summary) return;
    zc->error_count++;
    fprintf(stderr, "%s:%d: zercheck: ", zc->file_name, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* ================================================================
 * IR Handle State — tracked per LOCAL id (not string key)
 * ================================================================ */

typedef enum {
    IR_HS_UNKNOWN = 0,     /* not yet allocated */
    IR_HS_ALIVE,           /* allocated, valid */
    IR_HS_FREED,           /* freed on all paths */
    IR_HS_MAYBE_FREED,     /* freed on some paths */
    IR_HS_TRANSFERRED,     /* ownership transferred (move struct, spawn) */
} IRHandleState;

typedef struct {
    int local_id;          /* which IRLocal this tracks */
    IRHandleState state;
    int alloc_line;        /* where allocated */
    int free_line;         /* where freed */
    int alloc_id;          /* groups aliases — same alloc = same id */
    bool escaped;          /* returned, stored to global, etc. */
} IRHandleInfo;

/* Per-block analysis state */
typedef struct {
    IRHandleInfo *handles;
    int handle_count;
    int handle_capacity;
    bool terminated;       /* block ends with return/unreachable */
} IRPathState;

/* ================================================================
 * Path State Operations
 * ================================================================ */

static void ir_ps_init(IRPathState *ps) {
    ps->handles = NULL;
    ps->handle_count = 0;
    ps->handle_capacity = 0;
    ps->terminated = false;
}

static IRPathState ir_ps_copy(IRPathState *src) {
    IRPathState dst;
    dst.handle_count = src->handle_count;
    dst.handle_capacity = src->handle_count > 0 ? src->handle_count : 4;
    dst.terminated = false;
    dst.handles = (IRHandleInfo *)malloc(dst.handle_capacity * sizeof(IRHandleInfo));
    if (src->handles && src->handle_count > 0)
        memcpy(dst.handles, src->handles, src->handle_count * sizeof(IRHandleInfo));
    return dst;
}

static void ir_ps_free(IRPathState *ps) {
    free(ps->handles);
    ps->handles = NULL;
    ps->handle_count = 0;
}

static IRHandleInfo *ir_find_handle(IRPathState *ps, int local_id) {
    for (int i = 0; i < ps->handle_count; i++)
        if (ps->handles[i].local_id == local_id)
            return &ps->handles[i];
    return NULL;
}

static IRHandleInfo *ir_add_handle(IRPathState *ps, int local_id) {
    IRHandleInfo *existing = ir_find_handle(ps, local_id);
    if (existing) return existing;

    if (ps->handle_count >= ps->handle_capacity) {
        int nc = ps->handle_capacity < 8 ? 8 : ps->handle_capacity * 2;
        IRHandleInfo *nh = (IRHandleInfo *)realloc(ps->handles, nc * sizeof(IRHandleInfo));
        if (!nh) return NULL;
        ps->handles = nh;
        ps->handle_capacity = nc;
    }
    IRHandleInfo *h = &ps->handles[ps->handle_count++];
    memset(h, 0, sizeof(IRHandleInfo));
    h->local_id = local_id;
    h->state = IR_HS_UNKNOWN;
    return h;
}

/* ================================================================
 * State Helpers (same as zercheck.c but on IRHandleState)
 * ================================================================ */

static bool ir_is_invalid(IRHandleInfo *h) {
    return h->state == IR_HS_FREED ||
           h->state == IR_HS_MAYBE_FREED ||
           h->state == IR_HS_TRANSFERRED;
}

static const char *ir_state_name(IRHandleState s) {
    switch (s) {
    case IR_HS_UNKNOWN:     return "unknown";
    case IR_HS_ALIVE:       return "alive";
    case IR_HS_FREED:       return "freed";
    case IR_HS_MAYBE_FREED: return "maybe-freed";
    case IR_HS_TRANSFERRED: return "transferred";
    }
    return "?";
}

/* ================================================================
 * Move struct type detection (Phase B1 — ported from zercheck.c:920-961)
 *
 * A `move struct` has is_move=true on its struct_type. Passing or
 * assigning a move-struct-typed value transfers ownership: source
 * becomes TRANSFERRED, destination takes over as ALIVE.
 *
 * contains_move_struct_field / should_track_move cover the case
 * where a regular struct has a move struct field — the outer struct
 * inherits transfer semantics.
 * ================================================================ */

static bool ir_is_move_struct_type(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    return (eff->kind == TYPE_STRUCT && eff->struct_type.is_move);
}

static bool ir_contains_move_struct_field(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind == TYPE_STRUCT) {
        for (uint32_t i = 0; i < eff->struct_type.field_count; i++) {
            if (ir_is_move_struct_type(eff->struct_type.fields[i].type))
                return true;
        }
    }
    /* Union containing move struct variant */
    if (eff->kind == TYPE_UNION) {
        for (uint32_t i = 0; i < eff->union_type.variant_count; i++) {
            if (ir_is_move_struct_type(eff->union_type.variants[i].type))
                return true;
        }
    }
    return false;
}

static bool ir_should_track_move(Type *t) {
    return t && (ir_is_move_struct_type(t) || ir_contains_move_struct_field(t));
}

/* Allocation ID counter for move struct new-ownership chains.
 * When ownership transfers (e.g., Token b = a), the destination
 * gets a fresh alloc_id representing a new ownership identity —
 * the source's alloc_id goes with its TRANSFERRED state. */
static int _ir_next_alloc_id = 1000000;  /* high base so it doesn't
                                            collide with local_id-based
                                            ids set at allocation */

/* ================================================================
 * CFG Merge — the key advantage over linear AST walk
 *
 * At basic block join points (multiple predecessors), merge handle
 * states from all incoming paths. This is NATURAL with basic blocks.
 * No hack, no block_always_exits check, no 2-pass workaround.
 * ================================================================ */

static IRPathState ir_merge_states(IRPathState *states, int state_count) {
    if (state_count == 0) {
        IRPathState empty;
        ir_ps_init(&empty);
        return empty;
    }
    if (state_count == 1) return ir_ps_copy(&states[0]);

    /* Start from first non-terminated state */
    int first_live = -1;
    for (int i = 0; i < state_count; i++) {
        if (!states[i].terminated) { first_live = i; break; }
    }
    if (first_live < 0) {
        /* All predecessors terminated — this block is unreachable */
        IRPathState result;
        ir_ps_init(&result);
        result.terminated = true;
        return result;
    }

    IRPathState result = ir_ps_copy(&states[first_live]);

    /* Merge each subsequent non-terminated predecessor */
    for (int si = first_live + 1; si < state_count; si++) {
        if (states[si].terminated) continue; /* dead path, skip */

        /* For each handle in result, check if same handle exists in this pred */
        for (int hi = 0; hi < result.handle_count; hi++) {
            IRHandleInfo *rh = &result.handles[hi];
            IRHandleInfo *ph = ir_find_handle(&states[si], rh->local_id);

            if (!ph) continue; /* handle not in this pred — keep result's state */

            /* Merge states: both freed → freed, one freed → maybe_freed, etc. */
            if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_FREED) {
                rh->state = IR_HS_MAYBE_FREED;
                rh->free_line = ph->free_line;
            } else if (rh->state == IR_HS_FREED && ph->state == IR_HS_ALIVE) {
                rh->state = IR_HS_MAYBE_FREED;
            } else if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_TRANSFERRED) {
                rh->state = IR_HS_MAYBE_FREED; /* conservative */
            } else if (rh->state == IR_HS_TRANSFERRED && ph->state == IR_HS_ALIVE) {
                rh->state = IR_HS_MAYBE_FREED;
            }
            /* Both same state → keep. Both freed → keep freed. */
        }

        /* Add handles from pred that aren't in result yet */
        for (int pi = 0; pi < states[si].handle_count; pi++) {
            if (!ir_find_handle(&result, states[si].handles[pi].local_id)) {
                IRHandleInfo *nh = ir_add_handle(&result, states[si].handles[pi].local_id);
                if (nh) *nh = states[si].handles[pi];
            }
        }
    }

    return result;
}

/* ================================================================
 * Instruction Analysis — process one IR instruction
 * ================================================================ */

static void ir_check_inst(ZerCheck *zc, IRPathState *ps, IRInst *inst, IRFunc *func) {
    (void)zc; /* used for error reporting */

    switch (inst->op) {

    /* Allocation → register handle as ALIVE */
    case IR_POOL_ALLOC:
    case IR_SLAB_ALLOC:
    case IR_SLAB_ALLOC_PTR: {
        if (inst->dest_local >= 0) {
            IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
            if (h) {
                /* Check for overwrite of alive handle (leak) */
                if (h->state == IR_HS_ALIVE) {
                    ir_zc_error(zc, inst->source_line,
                        "handle %%%d overwritten while alive — previous allocation leaked",
                        inst->dest_local);
                }
                h->state = IR_HS_ALIVE;
                h->alloc_line = inst->source_line;
                h->alloc_id = inst->dest_local; /* simple: local_id = alloc_id */
            }
        }
        break;
    }

    /* Free → mark as FREED, check for double-free */
    case IR_POOL_FREE:
    case IR_SLAB_FREE:
    case IR_SLAB_FREE_PTR: {
        int target = inst->handle_local;
        if (target >= 0) {
            IRHandleInfo *h = ir_find_handle(ps, target);
            if (h) {
                if (h->state == IR_HS_FREED) {
                    ir_zc_error(zc, inst->source_line,
                        "double free: %%%d already freed at line %d",
                        target, h->free_line);
                } else if (h->state == IR_HS_MAYBE_FREED) {
                    ir_zc_error(zc, inst->source_line,
                        "freeing %%%d which may already be freed",
                        target);
                } else if (h->state == IR_HS_TRANSFERRED) {
                    ir_zc_error(zc, inst->source_line,
                        "freeing %%%d which was already transferred",
                        target);
                }
                h->state = IR_HS_FREED;
                h->free_line = inst->source_line;

                /* Mark aliases with same alloc_id as FREED */
                for (int i = 0; i < ps->handle_count; i++) {
                    if (ps->handles[i].local_id != target &&
                        ps->handles[i].alloc_id == h->alloc_id) {
                        ps->handles[i].state = IR_HS_FREED;
                        ps->handles[i].free_line = inst->source_line;
                    }
                }
            }
        }
        break;
    }

    /* Get → check handle is ALIVE (UAF check) */
    case IR_POOL_GET: {
        int target = inst->handle_local;
        if (target >= 0) {
            IRHandleInfo *h = ir_find_handle(ps, target);
            if (h && ir_is_invalid(h)) {
                ir_zc_error(zc, inst->source_line,
                    "use after free: %%%d is %s (freed at line %d)",
                    target, ir_state_name(h->state), h->free_line);
            }
        }
        break;
    }

    /* Assign → alias tracking or move transfer.
     * Phase B1: move struct types get TRANSFER semantics (not alias).
     * `Token b = a` transfers ownership: a → TRANSFERRED, b → ALIVE (new id). */
    case IR_ASSIGN: {
        if (inst->dest_local >= 0 && inst->expr) {
            /* If source is an ident that's a tracked handle, create alias */
            if (inst->expr->kind == NODE_IDENT) {
                int src_local = ir_find_local(func,
                    inst->expr->ident.name,
                    (uint32_t)inst->expr->ident.name_len);
                if (src_local >= 0) {
                    IRHandleInfo *src_h = ir_find_handle(ps, src_local);
                    Type *src_type = (src_local < func->local_count)
                        ? func->locals[src_local].type : NULL;
                    bool is_move = ir_should_track_move(src_type);

                    if (is_move) {
                        /* Move transfer: source → TRANSFERRED, dest → new ALIVE */
                        if (src_h && src_h->state == IR_HS_TRANSFERRED) {
                            ir_zc_error(zc, inst->source_line,
                                "use of transferred value (local %%%d) — ownership already moved",
                                src_local);
                        } else if (src_h && ir_is_invalid(src_h)) {
                            ir_zc_error(zc, inst->source_line,
                                "use of %s handle %%%d",
                                ir_state_name(src_h->state), src_local);
                        }
                        /* Ensure source exists as handle so we can mark it */
                        if (!src_h) src_h = ir_add_handle(ps, src_local);
                        if (src_h) {
                            src_h->state = IR_HS_TRANSFERRED;
                            src_h->free_line = inst->source_line;
                        }
                        /* Destination takes new ownership identity */
                        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                        if (dst_h) {
                            dst_h->state = IR_HS_ALIVE;
                            dst_h->alloc_line = inst->source_line;
                            dst_h->alloc_id = _ir_next_alloc_id++;
                        }
                    } else {
                        /* Non-move: regular alias */
                        if (src_h && src_h->state == IR_HS_ALIVE) {
                            IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                            if (dst_h) {
                                dst_h->state = IR_HS_ALIVE;
                                dst_h->alloc_line = src_h->alloc_line;
                                dst_h->alloc_id = src_h->alloc_id;
                            }
                        }
                        /* Check use of invalid handle */
                        if (src_h && ir_is_invalid(src_h)) {
                            ir_zc_error(zc, inst->source_line,
                                "use of %s handle %%%d",
                                ir_state_name(src_h->state), src_local);
                        }
                    }
                }
            }
        }
        break;
    }

    /* Return → check no handles leaked, mark terminated.
     * Phase B1: returning a move struct value transfers ownership
     * to caller. Source is marked TRANSFERRED (not merely escaped) —
     * subsequent use in unreachable code still detectable. */
    case IR_RETURN: {
        if (inst->expr && inst->expr->kind == NODE_IDENT) {
            int ret_local = ir_find_local(func,
                inst->expr->ident.name,
                (uint32_t)inst->expr->ident.name_len);
            if (ret_local >= 0 && ret_local < func->local_count) {
                IRHandleInfo *h = ir_find_handle(ps, ret_local);
                Type *ret_type = func->locals[ret_local].type;
                if (ir_should_track_move(ret_type)) {
                    /* Check using transferred/freed value before return */
                    if (h && ir_is_invalid(h)) {
                        ir_zc_error(zc, inst->source_line,
                            "returning %s value (local %%%d)",
                            ir_state_name(h->state), ret_local);
                    }
                    /* Mark TRANSFERRED — caller owns it now */
                    if (!h) h = ir_add_handle(ps, ret_local);
                    if (h) {
                        h->state = IR_HS_TRANSFERRED;
                        h->free_line = inst->source_line;
                        h->escaped = true;
                    }
                } else {
                    /* Non-move: just mark escaped (existing behavior) */
                    if (h) h->escaped = true;
                }
            }
        }
        ps->terminated = true;
        break;
    }

    /* Spawn → mark args as transferred */
    case IR_SPAWN: {
        /* Arguments passed to spawn transfer ownership */
        for (int i = 0; i < inst->arg_count; i++) {
            if (inst->args && inst->args[i] && inst->args[i]->kind == NODE_IDENT) {
                int arg_local = ir_find_local(func,
                    inst->args[i]->ident.name,
                    (uint32_t)inst->args[i]->ident.name_len);
                if (arg_local >= 0) {
                    IRHandleInfo *h = ir_find_handle(ps, arg_local);
                    if (h) h->state = IR_HS_TRANSFERRED;
                }
            }
        }
        break;
    }

    /* Phase B1: FIELD_WRITE with move struct RHS transfers ownership.
     * Closes Gap 5: `b.item = t` where Box(Tok) b — t transferred to
     * the container field. Subsequent use of t = use-after-transfer.
     * Handles two target shapes:
     *   - src1_local = container local (decomposed path)
     *   - inst->expr  = AST NODE_ASSIGN (passthrough path)
     * Both route through the same logic: find source local, check its
     * type for move-struct, mark TRANSFERRED if so. */
    case IR_FIELD_WRITE: {
        /* Resolve RHS local — could be src2_local (decomposed) or
         * inst->expr's value side (passthrough). */
        int rhs_local = -1;
        if (inst->src2_local >= 0) {
            rhs_local = inst->src2_local;
        } else if (inst->expr && inst->expr->kind == NODE_ASSIGN &&
                   inst->expr->assign.value &&
                   inst->expr->assign.value->kind == NODE_IDENT) {
            rhs_local = ir_find_local(func,
                inst->expr->assign.value->ident.name,
                (uint32_t)inst->expr->assign.value->ident.name_len);
        }
        if (rhs_local >= 0 && rhs_local < func->local_count) {
            Type *rhs_type = func->locals[rhs_local].type;
            if (ir_should_track_move(rhs_type)) {
                IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
                if (rh && ir_is_invalid(rh)) {
                    ir_zc_error(zc, inst->source_line,
                        "use of %s value (local %%%d) in field write",
                        ir_state_name(rh->state), rhs_local);
                }
                if (!rh) rh = ir_add_handle(ps, rhs_local);
                if (rh) {
                    rh->state = IR_HS_TRANSFERRED;
                    rh->free_line = inst->source_line;
                }
            }
        }
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 * Main Analysis — walk CFG in topological order
 * ================================================================ */

bool zercheck_ir(ZerCheck *zc, IRFunc *func) {
    if (!func || func->block_count == 0) return true;

    /* Allocate per-block states */
    IRPathState *block_states = (IRPathState *)calloc(func->block_count, sizeof(IRPathState));
    if (!block_states) return true;

    for (int bi = 0; bi < func->block_count; bi++)
        ir_ps_init(&block_states[bi]);

    /* Process blocks in order (topological for forward edges).
     * For back edges (loops), use fixed-point iteration. */
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 32) {
        changed = false;
        iterations++;

        for (int bi = 0; bi < func->block_count; bi++) {
            IRBlock *bb = &func->blocks[bi];

            /* Merge predecessor states */
            IRPathState merged;
            if (bb->pred_count == 0) {
                ir_ps_init(&merged); /* entry block — empty state */
            } else {
                IRPathState *pred_states = (IRPathState *)calloc(bb->pred_count, sizeof(IRPathState));
                for (int pi = 0; pi < bb->pred_count; pi++) {
                    pred_states[pi] = ir_ps_copy(&block_states[bb->preds[pi]]);
                }
                merged = ir_merge_states(pred_states, bb->pred_count);
                for (int pi = 0; pi < bb->pred_count; pi++)
                    ir_ps_free(&pred_states[pi]);
                free(pred_states);
            }

            /* Process instructions in this block */
            for (int ii = 0; ii < bb->inst_count; ii++) {
                ir_check_inst(zc, &merged, &bb->insts[ii], func);
            }

            /* Check if state changed (for fixed-point convergence) */
            if (merged.handle_count != block_states[bi].handle_count) {
                changed = true;
            } else {
                for (int hi = 0; hi < merged.handle_count; hi++) {
                    IRHandleInfo *mh = &merged.handles[hi];
                    IRHandleInfo *oh = ir_find_handle(&block_states[bi], mh->local_id);
                    if (!oh || oh->state != mh->state) {
                        changed = true;
                        break;
                    }
                }
            }

            /* Update block state */
            ir_ps_free(&block_states[bi]);
            block_states[bi] = merged;
        }
    }

    /* Leak detection: check handles at function exit (last block or return blocks) */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        if (bb->inst_count == 0) continue;
        IRInst *last = &bb->insts[bb->inst_count - 1];
        if (last->op != IR_RETURN) continue;

        IRPathState *ps = &block_states[bi];
        for (int hi = 0; hi < ps->handle_count; hi++) {
            IRHandleInfo *h = &ps->handles[hi];
            if (h->escaped) continue;
            if (h->state == IR_HS_ALIVE) {
                ir_zc_error(zc, last->source_line,
                    "handle %%%d (local '%.*s') allocated at line %d but never freed — "
                    "add defer pool.free() or return the handle",
                    h->local_id,
                    (int)func->locals[h->local_id].name_len,
                    func->locals[h->local_id].name,
                    h->alloc_line);
            } else if (h->state == IR_HS_MAYBE_FREED) {
                ir_zc_error(zc, last->source_line,
                    "handle %%%d may not be freed on all paths",
                    h->local_id);
            }
        }
    }

    /* Cleanup */
    for (int bi = 0; bi < func->block_count; bi++)
        ir_ps_free(&block_states[bi]);
    free(block_states);

    return zc->error_count == 0;
}
