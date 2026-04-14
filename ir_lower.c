/*
 * ZER IR Lowering Pass — AST → IR
 *
 * Converts typed AST (after checker) into flat IR (before zercheck/emitter).
 * See docs/IR_Implementation.md Part 5 for full lowering spec.
 *
 * Key responsibilities:
 * 1. Collect ALL locals (params, var_decls, captures, temps) — flat list
 * 2. Create basic blocks for control flow (if/while/for/switch/goto)
 * 3. Lower builtins to specific IR ops (pool.alloc → IR_POOL_ALLOC)
 * 4. Insert defer fire before every exit (return/break/continue/goto)
 * 5. Handle orelse as branch on has_value
 * 6. Handle if-unwrap capture as explicit local + assign
 */

#include "ir.h"
#include "checker.h"
#include <string.h>
#include <stdlib.h>

/* ================================================================
 * Lowering Context — state maintained during AST → IR translation
 * ================================================================ */
typedef struct {
    IRFunc *func;
    Arena *arena;
    Checker *checker;
    int current_block;        /* block ID where new instructions go */

    /* Loop context (for break/continue) */
    int loop_exit_block;      /* break → goto here (-1 = not in loop) */
    int loop_continue_block;  /* continue → goto here (-1 = not in loop) */
    int loop_defer_base;      /* defer count at loop entry (fire loop-local defers on break/continue) */

    /* Defer tracking */
    int defer_count;          /* number of pending defers */

    /* Label → block mapping (for goto/label) */
    struct { const char *name; uint32_t len; int block_id; } labels[128];
    int label_count;

    /* Temp counter for generated names */
    int temp_count;
} LowerCtx;

/* ---- Helpers ---- */

static void emit_inst(LowerCtx *ctx, IRInst inst) {
    ir_block_add_inst(&ctx->func->blocks[ctx->current_block], ctx->arena, inst);
}

static IRInst make_inst(IROpKind op, int line) {
    IRInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op = op;
    inst.dest_local = -1;
    inst.true_block = -1;
    inst.false_block = -1;
    inst.goto_block = -1;
    inst.source_line = line;
    return inst;
}

/* Start a new basic block, return its ID. Sets current_block. */
static int start_block(LowerCtx *ctx) {
    int id = ir_add_block(ctx->func, ctx->arena);
    ctx->current_block = id;
    return id;
}

/* Ensure current block is terminated. If not, add GOTO to target. */
static void ensure_terminated(LowerCtx *ctx, int target_block) {
    IRBlock *bb = &ctx->func->blocks[ctx->current_block];
    if (bb->inst_count > 0 && ir_block_is_terminated(bb)) return;
    IRInst go = make_inst(IR_GOTO, 0);
    go.goto_block = target_block;
    emit_inst(ctx, go);
}

/* Find or create label → block mapping */
static int find_label_block(LowerCtx *ctx, const char *name, uint32_t len) {
    for (int i = 0; i < ctx->label_count; i++) {
        if (ctx->labels[i].len == len &&
            memcmp(ctx->labels[i].name, name, len) == 0)
            return ctx->labels[i].block_id;
    }
    /* Create new block for this label */
    int bid = ir_add_block(ctx->func, ctx->arena);
    if (ctx->label_count < 128) {
        ctx->labels[ctx->label_count].name = name;
        ctx->labels[ctx->label_count].len = len;
        ctx->labels[ctx->label_count].block_id = bid;
        ctx->label_count++;
    }
    return bid;
}

/* ================================================================
 * Phase 1: Collect ALL locals from function body
 *
 * This is the key advantage of IR — complete local list.
 * Walks entire AST, finds params + var_decls + captures + temps.
 * No enumeration to forget — collects everything recursively.
 * ================================================================ */

static void collect_locals(LowerCtx *ctx, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_VAR_DECL:
        if (!node->var_decl.is_static) {
            Type *vt = checker_get_type(ctx->checker, node);
            ir_add_local(ctx->func, ctx->arena,
                         node->var_decl.name, (uint32_t)node->var_decl.name_len,
                         vt, false, false, false, node->loc.line);
        }
        /* Also collect from initializer (may contain nested var_decls in blocks) */
        collect_locals(ctx, node->var_decl.init);
        break;

    case NODE_IF:
        /* If-unwrap capture — THE fix for async capture ghost */
        if (node->if_stmt.capture_name) {
            Type *cond_type = checker_get_type(ctx->checker, node->if_stmt.cond);
            Type *cap_type = NULL;
            if (cond_type) {
                Type *eff = type_unwrap_distinct(cond_type);
                if (eff && eff->kind == TYPE_OPTIONAL) {
                    cap_type = eff->optional.inner;
                    if (node->if_stmt.capture_is_ptr && cap_type) {
                        cap_type = type_pointer(ctx->arena, cap_type);
                    }
                }
            }
            if (cap_type) {
                ir_add_local(ctx->func, ctx->arena,
                             node->if_stmt.capture_name,
                             (uint32_t)node->if_stmt.capture_name_len,
                             cap_type, false, true, false, node->loc.line);
            }
        }
        collect_locals(ctx, node->if_stmt.cond);
        collect_locals(ctx, node->if_stmt.then_body);
        collect_locals(ctx, node->if_stmt.else_body);
        break;

    case NODE_SWITCH:
        collect_locals(ctx, node->switch_stmt.expr);
        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            /* Switch arm captures */
            if (node->switch_stmt.arms[i].capture_name) {
                /* For union switch captures, we need the variant type.
                 * Use checker typemap on the arm body for type info. */
                Type *sw_type = checker_get_type(ctx->checker, node->switch_stmt.expr);
                Type *cap_type = NULL;
                if (sw_type) {
                    Type *sw_eff = type_unwrap_distinct(sw_type);
                    if (sw_eff && sw_eff->kind == TYPE_UNION) {
                        /* Find variant type from arm values */
                        for (int vi = 0; vi < node->switch_stmt.arms[i].value_count; vi++) {
                            Node *val = node->switch_stmt.arms[i].values[vi];
                            if (val && val->kind == NODE_FIELD) {
                                /* .variant_name → find field in union */
                                const char *vn = val->field.field_name;
                                uint32_t vl = (uint32_t)val->field.field_name_len;
                                for (uint32_t fi = 0; fi < sw_eff->union_type.variant_count; fi++) {
                                    if (sw_eff->union_type.variants[fi].name_len == vl &&
                                        memcmp(sw_eff->union_type.variants[fi].name, vn, vl) == 0) {
                                        cap_type = sw_eff->union_type.variants[fi].type;
                                        break;
                                    }
                                }
                            }
                        }
                        if (cap_type && node->switch_stmt.arms[i].capture_is_ptr) {
                            cap_type = type_pointer(ctx->arena, cap_type);
                        }
                    } else if (sw_eff && sw_eff->kind == TYPE_OPTIONAL) {
                        cap_type = sw_eff->optional.inner;
                    }
                }
                if (cap_type) {
                    ir_add_local(ctx->func, ctx->arena,
                                 node->switch_stmt.arms[i].capture_name,
                                 (uint32_t)node->switch_stmt.arms[i].capture_name_len,
                                 cap_type, false, true, false, node->loc.line);
                }
            }
            collect_locals(ctx, node->switch_stmt.arms[i].body);
        }
        break;

    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            collect_locals(ctx, node->block.stmts[i]);
        break;
    case NODE_FOR:
        collect_locals(ctx, node->for_stmt.init);
        collect_locals(ctx, node->for_stmt.body);
        break;
    case NODE_WHILE: case NODE_DO_WHILE:
        collect_locals(ctx, node->while_stmt.body);
        break;
    case NODE_DEFER:
        collect_locals(ctx, node->defer.body);
        break;
    case NODE_CRITICAL:
        collect_locals(ctx, node->critical.body);
        break;
    case NODE_ONCE:
        collect_locals(ctx, node->once.body);
        break;
    default:
        break;
    }
}

/* ================================================================
 * Phase 2: Pre-scan for labels (goto targets need blocks created early)
 * ================================================================ */

static void collect_labels(LowerCtx *ctx, Node *node) {
    if (!node) return;
    if (node->kind == NODE_LABEL) {
        find_label_block(ctx, node->label_stmt.name, (uint32_t)node->label_stmt.name_len);
    }
    if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++)
            collect_labels(ctx, node->block.stmts[i]);
    }
    if (node->kind == NODE_IF) {
        collect_labels(ctx, node->if_stmt.then_body);
        collect_labels(ctx, node->if_stmt.else_body);
    }
    if (node->kind == NODE_FOR) {
        collect_labels(ctx, node->for_stmt.body);
    }
    if (node->kind == NODE_WHILE || node->kind == NODE_DO_WHILE) {
        collect_labels(ctx, node->while_stmt.body);
    }
    if (node->kind == NODE_SWITCH) {
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            collect_labels(ctx, node->switch_stmt.arms[i].body);
    }
    if (node->kind == NODE_DEFER) collect_labels(ctx, node->defer.body);
    if (node->kind == NODE_CRITICAL) collect_labels(ctx, node->critical.body);
}

/* ================================================================
 * Phase 3: Lower statements → IR instructions + basic blocks
 * ================================================================ */

/* Forward declaration */
static void lower_stmt(LowerCtx *ctx, Node *node);

/* Check if a block always exits (return/break/continue/goto)
 * Used by future analysis passes on IR. */
#if 0 /* Reserved for future use */
static bool block_always_exits(Node *node) {
    if (!node) return false;
    if (node->kind == NODE_RETURN || node->kind == NODE_BREAK ||
        node->kind == NODE_CONTINUE || node->kind == NODE_GOTO)
        return true;
    if (node->kind == NODE_BLOCK && node->block.stmt_count > 0)
        return block_always_exits(node->block.stmts[node->block.stmt_count - 1]);
    if (node->kind == NODE_IF && node->if_stmt.then_body && node->if_stmt.else_body)
        return block_always_exits(node->if_stmt.then_body) &&
               block_always_exits(node->if_stmt.else_body);
    return false;
}
#endif

/* Detect if a call is a builtin method (pool.alloc, ring.push, etc.)
 * Returns the IROpKind or IR_NOP if not a builtin */
static IROpKind classify_builtin_call(LowerCtx *ctx, Node *call,
                                       int *out_obj_local, int *out_handle_local) {
    *out_obj_local = -1;
    *out_handle_local = -1;

    if (!call || call->kind != NODE_CALL) return IR_NOP;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_FIELD) return IR_NOP;
    if (!callee->field.object || callee->field.object->kind != NODE_IDENT) return IR_NOP;

    const char *mn = callee->field.field_name;
    uint32_t ml = (uint32_t)callee->field.field_name_len;
    const char *on = callee->field.object->ident.name;
    uint32_t ol = (uint32_t)callee->field.object->ident.name_len;

    /* Look up object type */
    Type *obj_type = checker_get_type(ctx->checker, callee->field.object);
    if (!obj_type) {
        Symbol *sym = scope_lookup(ctx->checker->global_scope, on, ol);
        if (sym) obj_type = sym->type;
    }
    if (!obj_type) return IR_NOP;

    int obj_id = ir_find_local(ctx->func, on, ol);
    *out_obj_local = obj_id;

    Type *ot = type_unwrap_distinct(obj_type);

    /* Pool methods */
    if (ot->kind == TYPE_POOL) {
        if (ml == 5 && memcmp(mn, "alloc", 5) == 0) return IR_POOL_ALLOC;
        if (ml == 4 && memcmp(mn, "free", 4) == 0) {
            if (call->call.arg_count > 0 && call->call.args[0]->kind == NODE_IDENT) {
                *out_handle_local = ir_find_local(ctx->func,
                    call->call.args[0]->ident.name,
                    (uint32_t)call->call.args[0]->ident.name_len);
            }
            return IR_POOL_FREE;
        }
        if (ml == 3 && memcmp(mn, "get", 3) == 0) {
            if (call->call.arg_count > 0 && call->call.args[0]->kind == NODE_IDENT) {
                *out_handle_local = ir_find_local(ctx->func,
                    call->call.args[0]->ident.name,
                    (uint32_t)call->call.args[0]->ident.name_len);
            }
            return IR_POOL_GET;
        }
    }

    /* Slab methods */
    if (ot->kind == TYPE_SLAB) {
        if (ml == 5 && memcmp(mn, "alloc", 5) == 0) return IR_SLAB_ALLOC;
        if (ml == 9 && memcmp(mn, "alloc_ptr", 9) == 0) return IR_SLAB_ALLOC_PTR;
        if (ml == 4 && memcmp(mn, "free", 4) == 0) {
            if (call->call.arg_count > 0 && call->call.args[0]->kind == NODE_IDENT) {
                *out_handle_local = ir_find_local(ctx->func,
                    call->call.args[0]->ident.name,
                    (uint32_t)call->call.args[0]->ident.name_len);
            }
            return IR_SLAB_FREE;
        }
        if (ml == 8 && memcmp(mn, "free_ptr", 8) == 0) {
            if (call->call.arg_count > 0 && call->call.args[0]->kind == NODE_IDENT) {
                *out_handle_local = ir_find_local(ctx->func,
                    call->call.args[0]->ident.name,
                    (uint32_t)call->call.args[0]->ident.name_len);
            }
            return IR_SLAB_FREE_PTR;
        }
    }

    /* Ring methods */
    if (ot->kind == TYPE_RING) {
        if (ml == 4 && memcmp(mn, "push", 4) == 0) return IR_RING_PUSH;
        if (ml == 3 && memcmp(mn, "pop", 3) == 0) return IR_RING_POP;
        if (ml == 12 && memcmp(mn, "push_checked", 12) == 0) return IR_RING_PUSH_CHECKED;
    }

    /* Arena methods */
    if (ot->kind == TYPE_ARENA) {
        if (ml == 5 && memcmp(mn, "alloc", 5) == 0) return IR_ARENA_ALLOC;
        if (ml == 11 && memcmp(mn, "alloc_slice", 11) == 0) return IR_ARENA_ALLOC_SLICE;
        if (ml == 5 && memcmp(mn, "reset", 5) == 0) return IR_ARENA_RESET;
    }

    return IR_NOP; /* Not a builtin — regular method call */
}

/* Emit IR_DEFER_FIRE for pending defers */
static void emit_defer_fire(LowerCtx *ctx, int line) {
    if (ctx->defer_count > 0) {
        IRInst fire = make_inst(IR_DEFER_FIRE, line);
        emit_inst(ctx, fire);
    }
}

/* Lower a single statement */
static void lower_stmt(LowerCtx *ctx, Node *node) {
    if (!node) return;

    /* Check if current block is already terminated — start new block */
    IRBlock *cur = &ctx->func->blocks[ctx->current_block];
    if (cur->inst_count > 0 && ir_block_is_terminated(cur)) {
        start_block(ctx);
    }

    switch (node->kind) {

    /* ---- Block: lower each statement ---- */
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++) {
            lower_stmt(ctx, node->block.stmts[i]);
        }
        break;

    /* ---- Variable declaration: assign to local ---- */
    case NODE_VAR_DECL: {
        if (node->var_decl.is_static) break; /* static handled differently */
        int local_id = ir_find_local(ctx->func,
            node->var_decl.name, (uint32_t)node->var_decl.name_len);
        if (local_id >= 0 && node->var_decl.init) {
            /* Check if init is a builtin call */
            int obj_local, handle_local;
            IROpKind builtin = classify_builtin_call(ctx, node->var_decl.init,
                                                      &obj_local, &handle_local);
            if (builtin != IR_NOP) {
                IRInst inst = make_inst(builtin, node->loc.line);
                inst.dest_local = local_id;
                inst.obj_local = obj_local;
                inst.handle_local = handle_local;
                inst.expr = node->var_decl.init;
                emit_inst(ctx, inst);
            } else {
                IRInst inst = make_inst(IR_ASSIGN, node->loc.line);
                inst.dest_local = local_id;
                inst.expr = node->var_decl.init;
                emit_inst(ctx, inst);
            }
        }
        break;
    }

    /* ---- Expression statement: call or assign ---- */
    case NODE_EXPR_STMT: {
        Node *expr = node->expr_stmt.expr;
        if (!expr) break;

        if (expr->kind == NODE_CALL) {
            int obj_local, handle_local;
            IROpKind builtin = classify_builtin_call(ctx, expr, &obj_local, &handle_local);
            if (builtin != IR_NOP) {
                IRInst inst = make_inst(builtin, node->loc.line);
                inst.obj_local = obj_local;
                inst.handle_local = handle_local;
                inst.expr = expr;
                emit_inst(ctx, inst);
            } else {
                /* Regular function call */
                IRInst inst = make_inst(IR_CALL, node->loc.line);
                inst.expr = expr;
                if (expr->call.callee && expr->call.callee->kind == NODE_IDENT) {
                    inst.func_name = expr->call.callee->ident.name;
                    inst.func_name_len = (uint32_t)expr->call.callee->ident.name_len;
                }
                inst.arg_count = expr->call.arg_count;
                inst.args = expr->call.args;
                emit_inst(ctx, inst);
            }
        } else if (expr->kind == NODE_ASSIGN) {
            IRInst inst = make_inst(IR_ASSIGN, node->loc.line);
            /* If target is an ident, set dest_local */
            if (expr->assign.target && expr->assign.target->kind == NODE_IDENT) {
                inst.dest_local = ir_find_local(ctx->func,
                    expr->assign.target->ident.name,
                    (uint32_t)expr->assign.target->ident.name_len);
            }
            inst.expr = expr;
            emit_inst(ctx, inst);
        } else {
            /* Other expression (rare — orelse as statement, etc.) */
            IRInst inst = make_inst(IR_ASSIGN, node->loc.line);
            inst.expr = expr;
            emit_inst(ctx, inst);
        }
        break;
    }

    /* ---- If/else: branch + basic blocks ---- */
    case NODE_IF: {
        int bb_then = ir_add_block(ctx->func, ctx->arena);
        int bb_else = node->if_stmt.else_body ?
                      ir_add_block(ctx->func, ctx->arena) : -1;
        int bb_join = ir_add_block(ctx->func, ctx->arena);

        /* If-unwrap capture: emit assignment in then-block */
        bool has_capture = (node->if_stmt.capture_name != NULL);

        /* Branch on condition */
        IRInst br = make_inst(IR_BRANCH, node->loc.line);
        br.expr = node->if_stmt.cond;
        br.true_block = bb_then;
        br.false_block = bb_else >= 0 ? bb_else : bb_join;
        emit_inst(ctx, br);

        /* Then block */
        ctx->current_block = bb_then;
        if (has_capture) {
            int cap_id = ir_find_local(ctx->func,
                node->if_stmt.capture_name,
                (uint32_t)node->if_stmt.capture_name_len);
            if (cap_id >= 0) {
                /* capture = condition.value (or condition itself for null-sentinel) */
                IRInst cap = make_inst(IR_ASSIGN, node->loc.line);
                cap.dest_local = cap_id;
                cap.expr = node->if_stmt.cond; /* emitter knows to unwrap */
                emit_inst(ctx, cap);
            }
        }
        lower_stmt(ctx, node->if_stmt.then_body);
        ensure_terminated(ctx, bb_join);

        /* Else block */
        if (bb_else >= 0) {
            ctx->current_block = bb_else;
            lower_stmt(ctx, node->if_stmt.else_body);
            ensure_terminated(ctx, bb_join);
        }

        /* Join block */
        ctx->current_block = bb_join;
        break;
    }

    /* ---- For loop: init → cond → body → step → back to cond ---- */
    case NODE_FOR: {
        /* Lower init in current block */
        lower_stmt(ctx, node->for_stmt.init);

        int bb_cond = ir_add_block(ctx->func, ctx->arena);
        int bb_body = ir_add_block(ctx->func, ctx->arena);
        int bb_step = ir_add_block(ctx->func, ctx->arena);
        int bb_exit = ir_add_block(ctx->func, ctx->arena);

        /* Save loop context */
        int saved_exit = ctx->loop_exit_block;
        int saved_cont = ctx->loop_continue_block;
        int saved_defer = ctx->loop_defer_base;
        ctx->loop_exit_block = bb_exit;
        ctx->loop_continue_block = bb_step;
        ctx->loop_defer_base = ctx->defer_count;

        /* goto cond */
        ensure_terminated(ctx, bb_cond);

        /* Cond block */
        ctx->current_block = bb_cond;
        if (node->for_stmt.cond) {
            IRInst br = make_inst(IR_BRANCH, node->loc.line);
            br.expr = node->for_stmt.cond;
            br.true_block = bb_body;
            br.false_block = bb_exit;
            emit_inst(ctx, br);
        } else {
            /* No condition = infinite loop */
            IRInst go = make_inst(IR_GOTO, node->loc.line);
            go.goto_block = bb_body;
            emit_inst(ctx, go);
        }

        /* Body block */
        ctx->current_block = bb_body;
        lower_stmt(ctx, node->for_stmt.body);
        ensure_terminated(ctx, bb_step);

        /* Step block */
        ctx->current_block = bb_step;
        if (node->for_stmt.step) {
            IRInst step = make_inst(IR_ASSIGN, node->loc.line);
            step.expr = node->for_stmt.step;
            emit_inst(ctx, step);
        }
        ensure_terminated(ctx, bb_cond); /* back edge */

        /* Exit block */
        ctx->current_block = bb_exit;

        /* Restore loop context */
        ctx->loop_exit_block = saved_exit;
        ctx->loop_continue_block = saved_cont;
        ctx->loop_defer_base = saved_defer;
        break;
    }

    /* ---- While loop ---- */
    case NODE_WHILE: {
        int bb_cond = ir_add_block(ctx->func, ctx->arena);
        int bb_body = ir_add_block(ctx->func, ctx->arena);
        int bb_exit = ir_add_block(ctx->func, ctx->arena);

        int saved_exit = ctx->loop_exit_block;
        int saved_cont = ctx->loop_continue_block;
        int saved_defer = ctx->loop_defer_base;
        ctx->loop_exit_block = bb_exit;
        ctx->loop_continue_block = bb_cond;
        ctx->loop_defer_base = ctx->defer_count;

        ensure_terminated(ctx, bb_cond);

        ctx->current_block = bb_cond;
        IRInst br = make_inst(IR_BRANCH, node->loc.line);
        br.expr = node->while_stmt.cond;
        br.true_block = bb_body;
        br.false_block = bb_exit;
        emit_inst(ctx, br);

        ctx->current_block = bb_body;
        lower_stmt(ctx, node->while_stmt.body);
        ensure_terminated(ctx, bb_cond);

        ctx->current_block = bb_exit;

        ctx->loop_exit_block = saved_exit;
        ctx->loop_continue_block = saved_cont;
        ctx->loop_defer_base = saved_defer;
        break;
    }

    /* ---- Do-while loop ---- */
    case NODE_DO_WHILE: {
        int bb_body = ir_add_block(ctx->func, ctx->arena);
        int bb_cond = ir_add_block(ctx->func, ctx->arena);
        int bb_exit = ir_add_block(ctx->func, ctx->arena);

        int saved_exit = ctx->loop_exit_block;
        int saved_cont = ctx->loop_continue_block;
        int saved_defer = ctx->loop_defer_base;
        ctx->loop_exit_block = bb_exit;
        ctx->loop_continue_block = bb_cond;
        ctx->loop_defer_base = ctx->defer_count;

        ensure_terminated(ctx, bb_body);

        /* Body first (execute at least once) */
        ctx->current_block = bb_body;
        lower_stmt(ctx, node->while_stmt.body);
        ensure_terminated(ctx, bb_cond);

        /* Then condition */
        ctx->current_block = bb_cond;
        IRInst br = make_inst(IR_BRANCH, node->loc.line);
        br.expr = node->while_stmt.cond;
        br.true_block = bb_body;
        br.false_block = bb_exit;
        emit_inst(ctx, br);

        ctx->current_block = bb_exit;

        ctx->loop_exit_block = saved_exit;
        ctx->loop_continue_block = saved_cont;
        ctx->loop_defer_base = saved_defer;
        break;
    }

    /* ---- Switch: chain of branches ---- */
    case NODE_SWITCH: {
        int bb_exit = ir_add_block(ctx->func, ctx->arena);

        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            int bb_arm = ir_add_block(ctx->func, ctx->arena);
            int bb_next = (i + 1 < node->switch_stmt.arm_count) ?
                          ir_add_block(ctx->func, ctx->arena) : bb_exit;

            /* Branch: if matches → arm body, else → next check */
            if (node->switch_stmt.arms[i].is_default) {
                /* Default arm — unconditional */
                ensure_terminated(ctx, bb_arm);
            } else {
                IRInst br = make_inst(IR_BRANCH, node->loc.line);
                br.expr = node->switch_stmt.expr; /* emitter handles value comparison */
                br.true_block = bb_arm;
                br.false_block = bb_next;
                emit_inst(ctx, br);
            }

            /* Arm body */
            ctx->current_block = bb_arm;

            /* Switch capture assignment */
            if (node->switch_stmt.arms[i].capture_name) {
                int cap_id = ir_find_local(ctx->func,
                    node->switch_stmt.arms[i].capture_name,
                    (uint32_t)node->switch_stmt.arms[i].capture_name_len);
                if (cap_id >= 0) {
                    IRInst cap = make_inst(IR_ASSIGN, node->loc.line);
                    cap.dest_local = cap_id;
                    cap.expr = node->switch_stmt.expr;
                    emit_inst(ctx, cap);
                }
            }

            lower_stmt(ctx, node->switch_stmt.arms[i].body);
            ensure_terminated(ctx, bb_exit);

            if (!node->switch_stmt.arms[i].is_default) {
                ctx->current_block = bb_next;
            }
        }

        ctx->current_block = bb_exit;
        break;
    }

    /* ---- Return ---- */
    case NODE_RETURN: {
        emit_defer_fire(ctx, node->loc.line);
        IRInst ret = make_inst(IR_RETURN, node->loc.line);
        ret.expr = node->ret.expr;
        emit_inst(ctx, ret);
        break;
    }

    /* ---- Break ---- */
    case NODE_BREAK: {
        if (ctx->loop_exit_block >= 0) {
            /* Fire loop-local defers */
            if (ctx->defer_count > ctx->loop_defer_base) {
                IRInst fire = make_inst(IR_DEFER_FIRE, node->loc.line);
                emit_inst(ctx, fire);
            }
            IRInst go = make_inst(IR_GOTO, node->loc.line);
            go.goto_block = ctx->loop_exit_block;
            emit_inst(ctx, go);
        }
        break;
    }

    /* ---- Continue ---- */
    case NODE_CONTINUE: {
        if (ctx->loop_continue_block >= 0) {
            if (ctx->defer_count > ctx->loop_defer_base) {
                IRInst fire = make_inst(IR_DEFER_FIRE, node->loc.line);
                emit_inst(ctx, fire);
            }
            IRInst go = make_inst(IR_GOTO, node->loc.line);
            go.goto_block = ctx->loop_continue_block;
            emit_inst(ctx, go);
        }
        break;
    }

    /* ---- Goto ---- */
    case NODE_GOTO: {
        emit_defer_fire(ctx, node->loc.line);
        int target = find_label_block(ctx,
            node->goto_stmt.label, (uint32_t)node->goto_stmt.label_len);
        IRInst go = make_inst(IR_GOTO, node->loc.line);
        go.goto_block = target;
        emit_inst(ctx, go);
        break;
    }

    /* ---- Label ---- */
    case NODE_LABEL: {
        int target = find_label_block(ctx,
            node->label_stmt.name, (uint32_t)node->label_stmt.name_len);
        /* End current block, start the label's block */
        ensure_terminated(ctx, target);
        ctx->current_block = target;
        break;
    }

    /* ---- Defer ---- */
    case NODE_DEFER: {
        IRInst push = make_inst(IR_DEFER_PUSH, node->loc.line);
        push.defer_body = node->defer.body;
        emit_inst(ctx, push);
        ctx->defer_count++;
        break;
    }

    /* ---- Yield ---- */
    case NODE_YIELD: {
        IRInst y = make_inst(IR_YIELD, node->loc.line);
        emit_inst(ctx, y);
        /* Yield terminates block — start new block for resume point */
        start_block(ctx);
        break;
    }

    /* ---- Await ---- */
    case NODE_AWAIT: {
        IRInst aw = make_inst(IR_AWAIT, node->loc.line);
        aw.expr = node->await_stmt.cond;
        emit_inst(ctx, aw);
        start_block(ctx);
        break;
    }

    /* ---- Spawn ---- */
    case NODE_SPAWN: {
        IRInst sp = make_inst(IR_SPAWN, node->loc.line);
        sp.func_name = node->spawn_stmt.func_name;
        sp.func_name_len = (uint32_t)node->spawn_stmt.func_name_len;
        sp.arg_count = node->spawn_stmt.arg_count;
        sp.args = node->spawn_stmt.args;
        sp.is_scoped_spawn = (node->spawn_stmt.handle_name != NULL);
        sp.handle_name = node->spawn_stmt.handle_name;
        sp.handle_name_len = (uint32_t)node->spawn_stmt.handle_name_len;
        emit_inst(ctx, sp);
        break;
    }

    /* ---- @critical ---- */
    case NODE_CRITICAL: {
        IRInst begin = make_inst(IR_CRITICAL_BEGIN, node->loc.line);
        emit_inst(ctx, begin);
        lower_stmt(ctx, node->critical.body);
        IRInst end = make_inst(IR_CRITICAL_END, node->loc.line);
        emit_inst(ctx, end);
        break;
    }

    /* ---- @once ---- */
    case NODE_ONCE: {
        /* Lower as: branch on atomic flag → body or skip */
        int bb_body = ir_add_block(ctx->func, ctx->arena);
        int bb_skip = ir_add_block(ctx->func, ctx->arena);
        IRInst br = make_inst(IR_BRANCH, node->loc.line);
        br.expr = NULL; /* emitter handles @once CAS pattern */
        br.true_block = bb_body;
        br.false_block = bb_skip;
        emit_inst(ctx, br);

        ctx->current_block = bb_body;
        lower_stmt(ctx, node->once.body);
        ensure_terminated(ctx, bb_skip);

        ctx->current_block = bb_skip;
        break;
    }

    /* ---- ASM ---- */
    case NODE_ASM: {
        IRInst inst = make_inst(IR_NOP, node->loc.line);
        inst.expr = node; /* pass through — emitter handles raw asm */
        emit_inst(ctx, inst);
        break;
    }

    /* ---- Static assert (compile-time only, no IR) ---- */
    case NODE_STATIC_ASSERT:
        break;

    default:
        break;
    }
}

/* ================================================================
 * Entry Point: lower one function
 * ================================================================ */

IRFunc *ir_lower_func(Arena *arena, void *checker_ptr, Node *func_decl) {
    Checker *checker = (Checker *)checker_ptr;
    if (!func_decl || func_decl->kind != NODE_FUNC_DECL || !func_decl->func_decl.body)
        return NULL;

    /* Resolve return type */
    Type *ret_type = checker_get_type(checker, func_decl);
    /* ret_type from typemap might be the function type, not return type.
     * For now, pass NULL — emitter reads from AST. */

    IRFunc *func = ir_func_new(arena,
        func_decl->func_decl.name, (uint32_t)func_decl->func_decl.name_len,
        ret_type);

    func->is_async = func_decl->func_decl.is_async;
    func->is_naked = func_decl->func_decl.is_naked;
    func->ast_node = func_decl;

    /* Initialize lowering context */
    LowerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = func;
    ctx.arena = arena;
    ctx.checker = checker;
    ctx.loop_exit_block = -1;
    ctx.loop_continue_block = -1;

    /* Phase 1: Collect params as locals.
     * Param types resolved from TypeNode using basic primitive mapping.
     * Complex types (struct, pointer) fall back to ty_void — emitter reads AST. */
    for (int i = 0; i < func_decl->func_decl.param_count; i++) {
        ParamDecl *p = &func_decl->func_decl.params[i];
        Type *pt = NULL;
        /* Try to resolve type from TypeNode */
        if (p->type) {
            switch (p->type->kind) {
            case TYNODE_U8:  pt = ty_u8;  break;
            case TYNODE_U16: pt = ty_u16; break;
            case TYNODE_U32: pt = ty_u32; break;
            case TYNODE_U64: pt = ty_u64; break;
            case TYNODE_I8:  pt = ty_i8;  break;
            case TYNODE_I16: pt = ty_i16; break;
            case TYNODE_I32: pt = ty_i32; break;
            case TYNODE_I64: pt = ty_i64; break;
            case TYNODE_USIZE: pt = ty_usize; break;
            case TYNODE_F32: pt = ty_f32; break;
            case TYNODE_F64: pt = ty_f64; break;
            case TYNODE_BOOL: pt = ty_bool; break;
            case TYNODE_VOID: pt = ty_void; break;
            default: {
                /* Complex type — look up in global scope by param name.
                 * This works because checker registered all symbols. */
                Symbol *psym = scope_lookup(checker->global_scope,
                    p->name, (uint32_t)p->name_len);
                if (psym && psym->type) pt = psym->type;
                break;
            }
            }
        }
        if (!pt) pt = ty_void; /* fallback */
        ir_add_local(func, arena, p->name, (uint32_t)p->name_len,
                     pt, true, false, false, p->loc.line);
    }

    /* Phase 1b: Collect all var_decls + captures recursively */
    collect_locals(&ctx, func_decl->func_decl.body);

    /* Phase 2: Pre-scan for labels */
    collect_labels(&ctx, func_decl->func_decl.body);

    /* Phase 3: Create entry block and lower body */
    start_block(&ctx);
    lower_stmt(&ctx, func_decl->func_decl.body);

    /* Ensure last block is terminated */
    IRBlock *last = &func->blocks[func->block_count - 1];
    if (last->inst_count == 0 || !ir_block_is_terminated(last)) {
        /* Add implicit return */
        IRInst ret = make_inst(IR_RETURN, func_decl->loc.line);
        emit_inst(&ctx, ret);
    }

    /* Compute CFG predecessors */
    ir_compute_preds(func, arena);

    return func;
}

/* Also handle interrupt bodies */
IRFunc *ir_lower_interrupt(Arena *arena, void *checker_ptr, Node *interrupt) {
    Checker *checker = (Checker *)checker_ptr;
    if (!interrupt || interrupt->kind != NODE_INTERRUPT || !interrupt->interrupt.body)
        return NULL;

    IRFunc *func = ir_func_new(arena,
        interrupt->interrupt.name, (uint32_t)interrupt->interrupt.name_len,
        NULL /* void return */);

    func->is_interrupt = true;
    func->ast_node = interrupt;

    LowerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = func;
    ctx.arena = arena;
    ctx.checker = checker;
    ctx.loop_exit_block = -1;
    ctx.loop_continue_block = -1;

    collect_locals(&ctx, interrupt->interrupt.body);
    collect_labels(&ctx, interrupt->interrupt.body);

    start_block(&ctx);
    lower_stmt(&ctx, interrupt->interrupt.body);

    IRBlock *last = &func->blocks[func->block_count - 1];
    if (last->inst_count == 0 || !ir_block_is_terminated(last)) {
        IRInst ret = make_inst(IR_RETURN, interrupt->loc.line);
        emit_inst(&ctx, ret);
    }

    ir_compute_preds(func, ctx.arena);
    return func;
}
