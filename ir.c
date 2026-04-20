/*
 * ZER IR — Construction, validation, and pretty-printing
 *
 * This file implements the IR data structure operations.
 * The lowering pass (AST → IR) is in ir_lower.c.
 * The IR-based C emitter will be in ir_emit.c (future).
 */

#include "ir.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================
 * Construction API
 * ================================================================ */

IRFunc *ir_func_new(Arena *arena, const char *name, uint32_t name_len, Type *ret_type) {
    IRFunc *func = (IRFunc *)arena_alloc(arena, sizeof(IRFunc));
    memset(func, 0, sizeof(IRFunc));
    func->name = name;
    func->name_len = name_len;
    func->return_type = ret_type;

    /* Pre-allocate locals and blocks */
    func->local_capacity = 16;
    func->locals = (IRLocal *)arena_alloc(arena, func->local_capacity * sizeof(IRLocal));
    memset(func->locals, 0, func->local_capacity * sizeof(IRLocal));

    func->block_capacity = 8;
    func->blocks = (IRBlock *)arena_alloc(arena, func->block_capacity * sizeof(IRBlock));
    memset(func->blocks, 0, func->block_capacity * sizeof(IRBlock));

    return func;
}

int ir_add_local(IRFunc *func, Arena *arena,
                 const char *name, uint32_t name_len, Type *type,
                 bool is_param, bool is_capture, bool is_temp, int line) {
    /* Save original name before potential suffix */
    const char *orig_name = name;
    uint32_t orig_name_len = name_len;

    /* Dedup rules (BUG-590 scope-aware):
     *   - Same orig_name + same type + SAME scope_depth → dedup (return existing).
     *     Repeated references to the same var-decl shouldn't create extra locals.
     *   - Same orig_name + different type OR different scope_depth → create NEW
     *     local with a unique suffix. Shadowing across scopes must produce
     *     distinct locals so ir_find_local's scope-aware lookup can pick the
     *     right one after block exits.
     *   - is_temp, is_capture, is_param skip dedup entirely (callers already
     *     build unique names). */
    int cur_depth = func->current_scope;
    /* Dedup rules:
     *   - is_param, is_temp: skip dedup. Params are fresh at entry; temps
     *     already have unique `_zer_tN` names.
     *   - Others (including is_capture): dedup if same orig_name + same type
     *     + same scope_depth. Otherwise create new with suffix.
     *
     * is_capture needs dedup: two if-unwraps in the same outer scope
     * (`if (a) |v| ...; if (b) |v| ...;`) legitimately share a local — C
     * would otherwise see "uint32_t v" declared twice at function top.
     *
     * scope_depth differentiation: outer `Handle h` + inner `Handle h`
     * (shadowing in nested block) no longer collapses. Inner local is
     * suffixed; ir_find_local's scope-aware lookup picks the right one. */
    if (!is_temp && !is_param) {
        for (int i = 0; i < func->local_count; i++) {
            if (func->locals[i].orig_name_len == name_len &&
                memcmp(func->locals[i].orig_name, name, name_len) == 0) {
                bool same_type = (!type || !func->locals[i].type ||
                                  type == func->locals[i].type);
                bool same_scope = (func->locals[i].scope_depth == cur_depth);
                if (same_type && same_scope) return func->locals[i].id;
                /* Different type OR different scope → fall through to create
                 * new suffixed local. Use `_%d` with the count to ensure
                 * uniqueness across suffixed + unsuffixed variants. */
                char buf[64];
                int slen = snprintf(buf, sizeof(buf), "%.*s_%d",
                                    (int)name_len, name, func->local_count);
                if (slen >= (int)sizeof(buf)) slen = (int)sizeof(buf) - 1;
                char *sname = (char *)arena_alloc(arena, slen + 1);
                memcpy(sname, buf, slen + 1);
                name = sname;
                name_len = (uint32_t)slen;
                break;
            }
        }
    }

    /* Grow if needed */
    if (func->local_count >= func->local_capacity) {
        int new_cap = func->local_capacity * 2;
        IRLocal *new_locals = (IRLocal *)arena_alloc(arena, new_cap * sizeof(IRLocal));
        memcpy(new_locals, func->locals, func->local_count * sizeof(IRLocal));
        memset(new_locals + func->local_count, 0,
               (new_cap - func->local_count) * sizeof(IRLocal));
        func->locals = new_locals;
        func->local_capacity = new_cap;
    }

    int id = func->local_count;
    IRLocal *local = &func->locals[func->local_count++];
    local->id = id;
    local->name = name;
    local->name_len = name_len;
    local->orig_name = orig_name;
    local->orig_name_len = orig_name_len;
    local->type = type;
    local->is_param = is_param;
    local->is_capture = is_capture;
    local->is_temp = is_temp;
    local->scope_depth = cur_depth;
    local->source_line = line;
    return id;
}

int ir_find_local(IRFunc *func, const char *name, uint32_t name_len) {
    /* Search by orig_name (source name before any suffix) OR by C emission
     * name (for already-rewritten idents).
     *
     * BUG-590 scope-aware: skip locals whose scope has already exited
     * (`hidden=true`). Among the visible ones, return the LAST match —
     * later same-scope declarations / suffixed shadows of outer locals
     * naturally win. The `hidden` flag is set on NODE_BLOCK exit.
     *
     * Fallback: if no visible match, try LAST match regardless of hidden —
     * preserves pre-BUG-590 behavior for callers that query names outside
     * normal scope traversal (e.g., emitter passes after lowering). */
    int best = -1;
    int fallback = -1;
    for (int i = 0; i < func->local_count; i++) {
        bool match = false;
        if (func->locals[i].orig_name_len == name_len &&
            memcmp(func->locals[i].orig_name, name, name_len) == 0) match = true;
        if (!match && func->locals[i].name_len == name_len &&
            memcmp(func->locals[i].name, name, name_len) == 0) match = true;
        if (!match) continue;

        fallback = func->locals[i].id;
        if (!func->locals[i].hidden) best = func->locals[i].id;
    }
    return (best >= 0) ? best : fallback;
}

/* Phase E: zercheck-specific lookup that prefers exact-name match over
 * orig_name match. After full-function lowering all locals are hidden,
 * so regular ir_find_local's "prefer non-hidden" degrades to "return
 * last match", which picks the WRONG local for shadow-scope patterns.
 *
 * Example: outer `h` at %2 (name="h"), inner `h` at %7 (name="h_7",
 * orig="h"). Query "h" with regular lookup returns %7 (last match by
 * orig_name). Exact-first returns %2 (only match by exact name "h").
 *
 * Used only by zercheck_ir walkers after lowering. */
int ir_find_local_exact_first(IRFunc *func, const char *name, uint32_t name_len) {
    int exact_match = -1;
    int orig_match = -1;
    for (int i = 0; i < func->local_count; i++) {
        if (func->locals[i].name_len == name_len &&
            func->locals[i].name &&
            memcmp(func->locals[i].name, name, name_len) == 0) {
            exact_match = func->locals[i].id;
        } else if (func->locals[i].orig_name_len == name_len &&
                   func->locals[i].orig_name &&
                   memcmp(func->locals[i].orig_name, name, name_len) == 0) {
            orig_match = func->locals[i].id;
        }
    }
    if (exact_match >= 0) return exact_match;
    return orig_match;
}

int ir_add_block(IRFunc *func, Arena *arena) {
    if (func->block_count >= func->block_capacity) {
        int new_cap = func->block_capacity * 2;
        IRBlock *new_blocks = (IRBlock *)arena_alloc(arena, new_cap * sizeof(IRBlock));
        memcpy(new_blocks, func->blocks, func->block_count * sizeof(IRBlock));
        memset(new_blocks + func->block_count, 0,
               (new_cap - func->block_count) * sizeof(IRBlock));
        func->blocks = new_blocks;
        func->block_capacity = new_cap;
    }

    int id = func->block_count;
    IRBlock *block = &func->blocks[func->block_count++];
    memset(block, 0, sizeof(IRBlock));
    block->id = id;

    /* Pre-allocate instruction array */
    block->inst_capacity = 8;
    block->insts = (IRInst *)arena_alloc(arena, block->inst_capacity * sizeof(IRInst));
    memset(block->insts, 0, block->inst_capacity * sizeof(IRInst));

    return id;
}

void ir_block_add_inst(IRBlock *block, Arena *arena, IRInst inst) {
    if (block->inst_count >= block->inst_capacity) {
        int new_cap = block->inst_capacity * 2;
        IRInst *new_insts = (IRInst *)arena_alloc(arena, new_cap * sizeof(IRInst));
        memcpy(new_insts, block->insts, block->inst_count * sizeof(IRInst));
        memset(new_insts + block->inst_count, 0,
               (new_cap - block->inst_count) * sizeof(IRInst));
        block->insts = new_insts;
        block->inst_capacity = new_cap;
    }
    block->insts[block->inst_count++] = inst;
}

/* ================================================================
 * CFG Analysis
 * ================================================================ */

static void add_pred(IRBlock *block, Arena *arena, int pred_id) {
    /* Dedup */
    for (int i = 0; i < block->pred_count; i++)
        if (block->preds[i] == pred_id) return;

    if (block->pred_count >= block->pred_capacity) {
        int new_cap = block->pred_capacity < 4 ? 4 : block->pred_capacity * 2;
        int *new_preds = (int *)arena_alloc(arena, new_cap * sizeof(int));
        if (block->preds)
            memcpy(new_preds, block->preds, block->pred_count * sizeof(int));
        block->preds = new_preds;
        block->pred_capacity = new_cap;
    }
    block->preds[block->pred_count++] = pred_id;
}

void ir_compute_preds(IRFunc *func, Arena *arena) {
    /* Clear existing preds */
    for (int bi = 0; bi < func->block_count; bi++) {
        func->blocks[bi].pred_count = 0;
    }

    /* Walk all blocks, add edges from terminators */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *block = &func->blocks[bi];
        if (block->inst_count == 0) continue;

        IRInst *last = &block->insts[block->inst_count - 1];
        switch (last->op) {
        case IR_BRANCH:
            if (last->true_block >= 0 && last->true_block < func->block_count)
                add_pred(&func->blocks[last->true_block], arena, bi);
            if (last->false_block >= 0 && last->false_block < func->block_count)
                add_pred(&func->blocks[last->false_block], arena, bi);
            break;
        case IR_GOTO:
            if (last->goto_block >= 0 && last->goto_block < func->block_count)
                add_pred(&func->blocks[last->goto_block], arena, bi);
            break;
        case IR_YIELD:
            /* Yield → next block is resume point (implicit edge) */
            if (bi + 1 < func->block_count)
                add_pred(&func->blocks[bi + 1], arena, bi);
            break;
        case IR_RETURN:
            /* No successor */
            break;
        default:
            /* Non-terminator last instruction — implicit fallthrough to next block */
            if (bi + 1 < func->block_count)
                add_pred(&func->blocks[bi + 1], arena, bi);
            break;
        }
    }
}

bool ir_block_is_terminated(IRBlock *block) {
    if (block->inst_count == 0) return false;
    IROpKind op = block->insts[block->inst_count - 1].op;
    return op == IR_BRANCH || op == IR_GOTO || op == IR_RETURN || op == IR_YIELD;
}

/* ================================================================
 * Validation
 * ================================================================ */

/* Returns true if block `from` can reach any block in `target_set` via
 * CFG edges. Used by phase 2 defer balance check.
 *
 * has_fire_in_block: for each block bi, true if bi contains an emit-bodies
 *   IR_DEFER_FIRE anywhere (conservative — doesn't track instr position
 *   within a block). Callers that need "fire AFTER instr position in the
 *   same block as push" check that separately before calling this. */
static bool cfg_reaches_fire(IRFunc *func, int from, const bool *has_fire_in_block,
                             bool *visited) {
    if (from < 0 || from >= func->block_count) return false;
    if (visited[from]) return false;
    visited[from] = true;
    if (has_fire_in_block[from]) return true;
    IRBlock *block = &func->blocks[from];
    /* Successors via block terminator (same logic as dfs_reachable). */
    if (block->inst_count == 0) {
        if (from + 1 < func->block_count)
            return cfg_reaches_fire(func, from + 1, has_fire_in_block, visited);
        return false;
    }
    IRInst *last = &block->insts[block->inst_count - 1];
    switch (last->op) {
    case IR_BRANCH:
        if (cfg_reaches_fire(func, last->true_block, has_fire_in_block, visited)) return true;
        return cfg_reaches_fire(func, last->false_block, has_fire_in_block, visited);
    case IR_GOTO:
        return cfg_reaches_fire(func, last->goto_block, has_fire_in_block, visited);
    case IR_RETURN:
        return false;
    case IR_YIELD:
    case IR_AWAIT:
        if (from + 1 < func->block_count)
            return cfg_reaches_fire(func, from + 1, has_fire_in_block, visited);
        return false;
    default:
        if (from + 1 < func->block_count)
            return cfg_reaches_fire(func, from + 1, has_fire_in_block, visited);
        return false;
    }
}

/* Depth-first reachability walk from bb0. Fills reachable[] with true for
 * every block reachable via BRANCH/GOTO/implicit-fallthrough edges. */
static void dfs_reachable(IRFunc *func, int bi, bool *reachable) {
    if (bi < 0 || bi >= func->block_count) return;
    if (reachable[bi]) return;
    reachable[bi] = true;
    IRBlock *block = &func->blocks[bi];
    /* Determine successors. */
    if (block->inst_count == 0) {
        /* Empty block = implicit fallthrough to next block. */
        if (bi + 1 < func->block_count) dfs_reachable(func, bi + 1, reachable);
        return;
    }
    IRInst *last = &block->insts[block->inst_count - 1];
    switch (last->op) {
    case IR_BRANCH:
        dfs_reachable(func, last->true_block, reachable);
        dfs_reachable(func, last->false_block, reachable);
        break;
    case IR_GOTO:
        dfs_reachable(func, last->goto_block, reachable);
        break;
    case IR_RETURN:
        /* no successors */
        break;
    case IR_YIELD:
    case IR_AWAIT:
        /* async resume falls through to next block */
        if (bi + 1 < func->block_count) dfs_reachable(func, bi + 1, reachable);
        break;
    default:
        /* Non-terminator last instruction = implicit fallthrough. */
        if (bi + 1 < func->block_count) dfs_reachable(func, bi + 1, reachable);
        break;
    }
}

bool ir_validate(IRFunc *func) {
    bool valid = true;

    if (func->block_count == 0) {
        fprintf(stderr, "IR VALIDATION ERROR: function '%.*s' has no basic blocks\n",
                (int)func->name_len, func->name);
        return false;
    }

    /* Check each block */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *block = &func->blocks[bi];

        /* Empty blocks are OK — they can be join points that just fall through.
         * The emitter handles them (emit label, continue to next block). */
        if (block->inst_count == 0) {
            continue; /* skip further checks for empty blocks */
        }

        /* Last instruction must be a terminator (except last block which may fallthrough) */
        if (bi < func->block_count - 1 && !ir_block_is_terminated(block)) {
            /* Non-terminated non-last block — could be implicit fallthrough, which is OK */
        }

        /* Validate branch targets */
        for (int ii = 0; ii < block->inst_count; ii++) {
            IRInst *inst = &block->insts[ii];

            if (inst->op == IR_BRANCH) {
                if (inst->true_block < 0 || inst->true_block >= func->block_count) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d BRANCH true_block=%d out of range in '%.*s'\n",
                            bi, inst->true_block, (int)func->name_len, func->name);
                    valid = false;
                }
                if (inst->false_block < 0 || inst->false_block >= func->block_count) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d BRANCH false_block=%d out of range in '%.*s'\n",
                            bi, inst->false_block, (int)func->name_len, func->name);
                    valid = false;
                }
            }
            if (inst->op == IR_GOTO) {
                if (inst->goto_block < 0 || inst->goto_block >= func->block_count) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d GOTO target=%d out of range in '%.*s'\n",
                            bi, inst->goto_block, (int)func->name_len, func->name);
                    valid = false;
                }
            }

            /* Validate local references.
             * Note: obj_local/handle_local use 0 as "unset" (memset sentinel),
             * dest_local/cond_local/src*_local use -1. Both forms are OK as
             * long as the out-of-range check doesn't accept negative values. */
            if (inst->dest_local >= 0 && inst->dest_local >= func->local_count) {
                fprintf(stderr, "IR VALIDATION ERROR: bb%d inst %d references local %d but only %d locals in '%.*s'\n",
                        bi, ii, inst->dest_local, func->local_count,
                        (int)func->name_len, func->name);
                valid = false;
            }
            if (inst->obj_local >= 0 && inst->obj_local >= func->local_count) {
                fprintf(stderr, "IR VALIDATION ERROR: bb%d inst %d obj_local %d out of range in '%.*s'\n",
                        bi, ii, inst->obj_local, (int)func->name_len, func->name);
                valid = false;
            }
            if (inst->handle_local >= 0 && inst->handle_local >= func->local_count) {
                fprintf(stderr, "IR VALIDATION ERROR: bb%d inst %d handle_local %d out of range in '%.*s'\n",
                        bi, ii, inst->handle_local, (int)func->name_len, func->name);
                valid = false;
            }
            if (inst->cond_local >= 0 && inst->cond_local >= func->local_count) {
                /* For IR_DEFER_FIRE, cond_local is a defer-stack base INDEX
                 * (0..defer_count), not a local id. Skip the local-range
                 * check for this op. (src2_local has the same exception a
                 * few lines down.) */
                if (inst->op != IR_DEFER_FIRE) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d inst %d cond_local %d out of range in '%.*s'\n",
                            bi, ii, inst->cond_local, (int)func->name_len, func->name);
                    valid = false;
                }
            }
            if (inst->src1_local >= 0 && inst->src1_local >= func->local_count) {
                fprintf(stderr, "IR VALIDATION ERROR: bb%d inst %d src1_local %d out of range in '%.*s'\n",
                        bi, ii, inst->src1_local, (int)func->name_len, func->name);
                valid = false;
            }
            if (inst->src2_local >= 0 && inst->src2_local >= func->local_count) {
                /* Skip ops that use src2_local as a FLAG, not a local id:
                 *   IR_DEFER_FIRE: 0/1/2 (pop mode)
                 *   IR_LOCK: 0/1 (read vs write lock — BUG-594)
                 * All other ops treat src2_local as a local-id reference. */
                if (inst->op != IR_DEFER_FIRE && inst->op != IR_LOCK) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d inst %d src2_local %d out of range in '%.*s'\n",
                            bi, ii, inst->src2_local, (int)func->name_len, func->name);
                    valid = false;
                }
            }
        }
    }

    /* Check for duplicate local IDs */
    for (int i = 0; i < func->local_count; i++) {
        for (int j = i + 1; j < func->local_count; j++) {
            if (func->locals[i].id == func->locals[j].id) {
                fprintf(stderr, "IR VALIDATION ERROR: duplicate local id %d in '%.*s'\n",
                        func->locals[i].id, (int)func->name_len, func->name);
                valid = false;
            }
        }
    }

    /* ================================================================
     * Phase 1 hardening (2026-04-20):
     *   (a) Per-op field invariants — malformed instruction detection
     *   (b) Reachability (diagnostic only, opt-in via env var) — cannot
     *       be promoted to error because lowerer correctly emits IR for
     *       syntactically-reachable-but-semantically-dead source code
     *       (e.g. `goto done; x=0; done:`); no static way to distinguish
     *       that from a genuine forgotten-edge lowerer bug.
     *
     * Items dropped from the original plan after code inspection:
     *   - Missing terminator on non-last block: already supported as
     *     "implicit fallthrough" by ir_compute_preds / dfs_reachable.
     *     Not a bug pattern.
     *   - Missing terminator on last non-void block: caught by GCC's
     *     -Wreturn-type (we already enable -Wall -Wextra). Redundant.
     *   - Locals-used-while-hidden: `hidden` is a lookup-time flag for
     *     name resolution during lowering, not a runtime-reachability
     *     property. Post-lowering, hidden locals can legitimately be
     *     referenced from earlier-emitted instructions. Not a check.
     *
     * Items deferred to phase 2:
     *   - Defer push/fire balance (per-CFG-path depth tracking)
     *   - Use-before-define (needs dominator analysis for non-SSA IR)
     * ================================================================ */

    /* Audit addition (2026-04-20): NULL type on local.
     *
     * Considered but rejected: "dead code after terminator" as a validator
     * check. The lowerer emits legitimate patterns like
     *   RETURN; DEFER_FIRE; GOTO bb_post
     * where the DEFER_FIRE and GOTO are bb_post scope-cleanup emissions
     * that become dead C code which GCC strips. Not a safety hole, just
     * redundant IR. Promoting to error would block a correct pattern. */

    /* Every local must have a type. NULL type means the lowerer
     * failed to call resolve_type or similar. Downstream (emitter,
     * analyzer) will crash or emit wrong C. */
    for (int li = 0; li < func->local_count; li++) {
        IRLocal *l = &func->locals[li];
        if (!l->type) {
            fprintf(stderr, "IR VALIDATION ERROR: local %%%d (%.*s) has NULL type "
                    "in '%.*s' (lowerer forgot resolve_type)\n",
                    l->id, (int)l->name_len, l->name ? l->name : "?",
                    (int)func->name_len, func->name);
            valid = false;
        }
    }

    /* (a) Per-op field invariants — catches lowerer building a
     * malformed instruction (field forgotten, wrong local type, etc.).
     * Run after the range checks above so we know indices are sane. */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *block = &func->blocks[bi];
        for (int ii = 0; ii < block->inst_count; ii++) {
            IRInst *inst = &block->insts[ii];
            switch (inst->op) {
            case IR_BRANCH:
                /* Must have a condition: either cond_local OR an expr tree. */
                if (inst->cond_local < 0 && !inst->expr) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d BRANCH has neither "
                            "cond_local nor expr in '%.*s'\n",
                            bi, (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_BINOP:
                /* Binary op needs dest + two sources. */
                if (inst->dest_local < 0 || inst->src1_local < 0 || inst->src2_local < 0) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d BINOP missing operand "
                            "(dest=%d src1=%d src2=%d) in '%.*s'\n",
                            bi, inst->dest_local, inst->src1_local, inst->src2_local,
                            (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_UNOP:
                if (inst->dest_local < 0 || inst->src1_local < 0) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d UNOP missing operand "
                            "(dest=%d src1=%d) in '%.*s'\n",
                            bi, inst->dest_local, inst->src1_local,
                            (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_COPY:
                if (inst->dest_local < 0 || inst->src1_local < 0) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d COPY missing operand "
                            "(dest=%d src1=%d) in '%.*s'\n",
                            bi, inst->dest_local, inst->src1_local,
                            (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_LITERAL:
                /* Literals must write somewhere. */
                if (inst->dest_local < 0) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d LITERAL has no "
                            "dest_local in '%.*s'\n",
                            bi, (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_FIELD_READ:
                if (inst->dest_local < 0 || inst->src1_local < 0 || !inst->field_name) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d FIELD_READ malformed "
                            "(dest=%d src1=%d field=%s) in '%.*s'\n",
                            bi, inst->dest_local, inst->src1_local,
                            inst->field_name ? "set" : "NULL",
                            (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_FIELD_WRITE:
                if (inst->src1_local < 0 || inst->src2_local < 0 || !inst->field_name) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d FIELD_WRITE malformed "
                            "(src1=%d src2=%d field=%s) in '%.*s'\n",
                            bi, inst->src1_local, inst->src2_local,
                            inst->field_name ? "set" : "NULL",
                            (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_INDEX_READ:
                if (inst->dest_local < 0 || inst->src1_local < 0 || inst->src2_local < 0) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d INDEX_READ missing operand "
                            "(dest=%d src1=%d src2=%d) in '%.*s'\n",
                            bi, inst->dest_local, inst->src1_local, inst->src2_local,
                            (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_ADDR_OF:
            case IR_DEREF_READ:
                if (inst->dest_local < 0 || inst->src1_local < 0) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d %s missing operand "
                            "(dest=%d src1=%d) in '%.*s'\n",
                            bi, inst->op == IR_ADDR_OF ? "ADDR_OF" : "DEREF_READ",
                            inst->dest_local, inst->src1_local,
                            (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_CAST:
                if (inst->dest_local < 0 || inst->src1_local < 0 || !inst->cast_type) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d CAST malformed "
                            "(dest=%d src1=%d type=%s) in '%.*s'\n",
                            bi, inst->dest_local, inst->src1_local,
                            inst->cast_type ? "set" : "NULL",
                            (int)func->name_len, func->name);
                    valid = false;
                }
                break;
            case IR_CALL_DECOMP:
                /* Decomposed call must have consistent arg array. */
                if (inst->call_arg_local_count > 0 && !inst->call_arg_locals) {
                    fprintf(stderr, "IR VALIDATION ERROR: bb%d CALL_DECOMP has "
                            "arg_count=%d but call_arg_locals is NULL in '%.*s'\n",
                            bi, inst->call_arg_local_count,
                            (int)func->name_len, func->name);
                    valid = false;
                }
                /* Each arg local must be in range. */
                for (int ai = 0; ai < inst->call_arg_local_count; ai++) {
                    int a = inst->call_arg_locals[ai];
                    if (a < 0 || a >= func->local_count) {
                        fprintf(stderr, "IR VALIDATION ERROR: bb%d CALL_DECOMP arg[%d]=%d "
                                "out of range in '%.*s'\n",
                                bi, ai, a, (int)func->name_len, func->name);
                        valid = false;
                    }
                }
                break;
            default:
                break;
            }
        }
    }

    /* ================================================================
     * Phase 2 hardening — defer balance.
     *
     * For every IR_DEFER_PUSH, at least one IR_DEFER_FIRE with
     * emit_bodies=true (src2_local != 2) must be CFG-reachable from
     * the push. Otherwise the pushed defer body never emits anywhere,
     * silently dropping the user's `defer` statement.
     *
     * Note: emit-bodies fire can be within the same block (after the
     * push) or in a downstream CFG-reachable block. We can't prove the
     * exact stack-balancing (depth tracking requires path-sensitive
     * simulation; stack mode varies), but "at least one fire reaches"
     * is necessary: without it the defer body is statically dead.
     *
     * This is a true safety check — a missed defer is a leak or an
     * unreleased lock at runtime. Logged as ERROR, aborts compilation. */
    {
        /* Pre-compute which blocks contain ≥1 emit-bodies FIRE. */
        bool *fire_in_block = (bool *)calloc(func->block_count, sizeof(bool));
        if (fire_in_block) {
            for (int bi = 0; bi < func->block_count; bi++) {
                IRBlock *block = &func->blocks[bi];
                for (int ii = 0; ii < block->inst_count; ii++) {
                    IRInst *inst = &block->insts[ii];
                    if (inst->op == IR_DEFER_FIRE && inst->src2_local != 2) {
                        fire_in_block[bi] = true;
                        break;
                    }
                }
            }
            /* For every PUSH, verify a reachable FIRE exists. */
            bool *visited = (bool *)calloc(func->block_count, sizeof(bool));
            if (visited) {
                for (int bi = 0; bi < func->block_count; bi++) {
                    IRBlock *block = &func->blocks[bi];
                    for (int ii = 0; ii < block->inst_count; ii++) {
                        IRInst *inst = &block->insts[ii];
                        if (inst->op != IR_DEFER_PUSH) continue;
                        /* Same-block fire AFTER this push counts. */
                        bool found = false;
                        for (int jj = ii + 1; jj < block->inst_count; jj++) {
                            IRInst *j = &block->insts[jj];
                            if (j->op == IR_DEFER_FIRE && j->src2_local != 2) {
                                found = true;
                                break;
                            }
                        }
                        if (found) continue;
                        /* Otherwise do CFG-reachability from successors. */
                        memset(visited, 0, func->block_count * sizeof(bool));
                        visited[bi] = true; /* don't revisit the push's block */
                        bool reached = false;
                        /* Compute successors of bi and DFS from them. */
                        if (block->inst_count > 0) {
                            IRInst *last = &block->insts[block->inst_count - 1];
                            switch (last->op) {
                            case IR_BRANCH:
                                reached = cfg_reaches_fire(func, last->true_block, fire_in_block, visited)
                                       || cfg_reaches_fire(func, last->false_block, fire_in_block, visited);
                                break;
                            case IR_GOTO:
                                reached = cfg_reaches_fire(func, last->goto_block, fire_in_block, visited);
                                break;
                            case IR_RETURN:
                                reached = false;
                                break;
                            default:
                                if (bi + 1 < func->block_count)
                                    reached = cfg_reaches_fire(func, bi + 1, fire_in_block, visited);
                                break;
                            }
                        }
                        if (!reached) {
                            fprintf(stderr, "IR VALIDATION ERROR: bb%d inst %d IR_DEFER_PUSH "
                                    "has no CFG-reachable IR_DEFER_FIRE in '%.*s' "
                                    "(defer body would never execute)\n",
                                    bi, ii, (int)func->name_len, func->name);
                            valid = false;
                        }
                    }
                }
                free(visited);
            }
            free(fire_in_block);
        }
    }

    /* (b) Block reachability — DFS from bb0.
     *
     * DIAGNOSTIC ONLY. Unreachable blocks have two sources we can't
     * statically distinguish:
     *   (1) Lowerer forgot an edge — real bug.
     *   (2) Source code is syntactically reachable but semantically
     *       dead — e.g. `goto done; x = 0; done:` where the assign
     *       is unreachable but the lowerer correctly emits it (we
     *       don't do DCE at IR level; GCC does).
     * Both produce identical IR. Cannot be promoted to error without
     * lowerer-level "this block was source code between goto/label"
     * annotations. Stays warning-only even in strict mode.
     *
     * Enabled by ZER_IR_WARN_UNREACHABLE=1 (off by default — too noisy
     * for normal builds, useful for lowerer refactor sessions). */
    if (getenv("ZER_IR_WARN_UNREACHABLE")) {
        bool *reachable = (bool *)calloc(func->block_count, sizeof(bool));
        if (reachable) {
            dfs_reachable(func, 0, reachable);
            for (int bi = 0; bi < func->block_count; bi++) {
                if (!reachable[bi]) {
                    IRBlock *block = &func->blocks[bi];
                    fprintf(stderr, "IR VALIDATION NOTE: bb%d unreachable from entry "
                            "in '%.*s' (%d instructions — lowerer dead-code emission "
                            "OR forgotten edge; GCC will DCE)\n",
                            bi, (int)func->name_len, func->name, block->inst_count);
                }
            }
            free(reachable);
        }
    }

    return valid;
}

/* ================================================================
 * Pretty-Printer
 * ================================================================ */

static const char *ir_op_name(IROpKind op) {
    switch (op) {
    case IR_ASSIGN:           return "ASSIGN";
    case IR_CALL:             return "CALL";
    case IR_BRANCH:           return "BRANCH";
    case IR_GOTO:             return "GOTO";
    case IR_RETURN:           return "RETURN";
    case IR_YIELD:            return "YIELD";
    case IR_AWAIT:            return "AWAIT";
    case IR_SPAWN:            return "SPAWN";
    case IR_LOCK:             return "LOCK";
    case IR_UNLOCK:           return "UNLOCK";
    case IR_POOL_ALLOC:       return "POOL_ALLOC";
    case IR_POOL_FREE:        return "POOL_FREE";
    case IR_POOL_GET:         return "POOL_GET";
    case IR_SLAB_ALLOC:       return "SLAB_ALLOC";
    case IR_SLAB_FREE:        return "SLAB_FREE";
    case IR_SLAB_FREE_PTR:    return "SLAB_FREE_PTR";
    case IR_SLAB_ALLOC_PTR:   return "SLAB_ALLOC_PTR";
    case IR_ARENA_ALLOC:      return "ARENA_ALLOC";
    case IR_ARENA_ALLOC_SLICE:return "ARENA_ALLOC_SLICE";
    case IR_ARENA_RESET:      return "ARENA_RESET";
    case IR_RING_PUSH:        return "RING_PUSH";
    case IR_RING_POP:         return "RING_POP";
    case IR_RING_PUSH_CHECKED:return "RING_PUSH_CHECKED";
    case IR_CRITICAL_BEGIN:   return "CRITICAL_BEGIN";
    case IR_CRITICAL_END:     return "CRITICAL_END";
    case IR_DEFER_PUSH:       return "DEFER_PUSH";
    case IR_DEFER_FIRE:       return "DEFER_FIRE";
    case IR_INTRINSIC:        return "INTRINSIC";
    case IR_COPY:             return "COPY";
    case IR_BINOP:            return "BINOP";
    case IR_UNOP:             return "UNOP";
    case IR_FIELD_READ:       return "FIELD_READ";
    case IR_FIELD_WRITE:      return "FIELD_WRITE";
    case IR_INDEX_READ:       return "INDEX_READ";
    case IR_INDEX_WRITE:      return "INDEX_WRITE";
    case IR_LITERAL:          return "LITERAL";
    case IR_ADDR_OF:          return "ADDR_OF";
    case IR_DEREF_READ:       return "DEREF_READ";
    case IR_CALL_DECOMP:      return "CALL_DECOMP";
    case IR_CAST:             return "CAST";
    case IR_INTRINSIC_DECOMP: return "INTRINSIC_DECOMP";
    case IR_ORELSE_DECOMP:    return "ORELSE_DECOMP";
    case IR_SLICE_READ:       return "SLICE_READ";
    case IR_STRUCT_INIT_DECOMP: return "STRUCT_INIT_DECOMP";
    case IR_NOP:              return "NOP";
    }
    return "???";
}

void ir_print(FILE *out, IRFunc *func) {
    /* Function header */
    fprintf(out, "FUNC %.*s(", (int)func->name_len, func->name);
    bool first = true;
    for (int i = 0; i < func->local_count; i++) {
        if (!func->locals[i].is_param) continue;
        if (!first) fprintf(out, ", ");
        first = false;
        fprintf(out, "%%%d:%.*s", func->locals[i].id,
                (int)func->locals[i].name_len, func->locals[i].name);
        if (func->locals[i].type)
            fprintf(out, " : %s", type_name(func->locals[i].type));
    }
    fprintf(out, ")");
    if (func->return_type)
        fprintf(out, " -> %s", type_name(func->return_type));
    if (func->is_async) fprintf(out, " [async]");
    if (func->is_interrupt) fprintf(out, " [interrupt]");
    fprintf(out, "\n");

    /* Locals */
    fprintf(out, "  LOCALS:\n");
    for (int i = 0; i < func->local_count; i++) {
        IRLocal *l = &func->locals[i];
        fprintf(out, "    %%%d: %.*s", l->id, (int)l->name_len, l->name);
        if (l->type) fprintf(out, " : %s", type_name(l->type));
        if (l->is_param)   fprintf(out, " [param]");
        if (l->is_capture) fprintf(out, " [capture]");
        if (l->is_temp)    fprintf(out, " [temp]");
        if (l->is_static)  fprintf(out, " [static]");
        fprintf(out, " (line %d)\n", l->source_line);
    }
    fprintf(out, "\n");

    /* Basic blocks */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        fprintf(out, "  bb%d:", bb->id);
        if (bb->label) fprintf(out, " (%.*s)", (int)bb->label_len, bb->label);

        /* Print predecessors */
        if (bb->pred_count > 0) {
            fprintf(out, "  ; preds:");
            for (int pi = 0; pi < bb->pred_count; pi++)
                fprintf(out, " bb%d", bb->preds[pi]);
        }
        fprintf(out, "\n");

        /* Print instructions */
        for (int ii = 0; ii < bb->inst_count; ii++) {
            IRInst *inst = &bb->insts[ii];
            fprintf(out, "    ");

            /* Destination */
            if (inst->dest_local >= 0)
                fprintf(out, "%%%d = ", inst->dest_local);

            /* Operation */
            fprintf(out, "%s", ir_op_name(inst->op));

            /* Operands */
            switch (inst->op) {
            case IR_BRANCH:
                fprintf(out, " -> bb%d, bb%d", inst->true_block, inst->false_block);
                break;
            case IR_GOTO:
                fprintf(out, " -> bb%d", inst->goto_block);
                break;
            case IR_CALL:
            case IR_SPAWN:
                if (inst->func_name)
                    fprintf(out, " %.*s", (int)inst->func_name_len, inst->func_name);
                fprintf(out, "(%d args)", inst->arg_count);
                if (inst->op == IR_SPAWN && inst->is_scoped_spawn)
                    fprintf(out, " [scoped]");
                break;
            case IR_POOL_ALLOC: case IR_SLAB_ALLOC: case IR_SLAB_ALLOC_PTR:
                fprintf(out, " obj=%%%d", inst->obj_local);
                break;
            case IR_POOL_FREE: case IR_SLAB_FREE: case IR_SLAB_FREE_PTR:
                fprintf(out, " obj=%%%d handle=%%%d", inst->obj_local, inst->handle_local);
                break;
            case IR_POOL_GET:
                fprintf(out, " obj=%%%d handle=%%%d", inst->obj_local, inst->handle_local);
                break;
            case IR_LOCK: case IR_UNLOCK:
                fprintf(out, " obj=%%%d", inst->obj_local);
                break;
            case IR_INTRINSIC:
                if (inst->intrinsic_name)
                    fprintf(out, " @%.*s", (int)inst->intrinsic_name_len, inst->intrinsic_name);
                break;
            case IR_DEFER_PUSH:
                fprintf(out, " <body>");
                break;
            case IR_RETURN:
                if (inst->expr) fprintf(out, " <expr>");
                break;
            default:
                if (inst->expr) fprintf(out, " <expr>");
                break;
            }

            fprintf(out, "  ; line %d\n", inst->source_line);
        }
        fprintf(out, "\n");
    }
}
