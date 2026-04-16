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
    inst.cond_local = -1;
    inst.src1_local = -1;
    inst.src2_local = -1;
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

/* collect_locals REMOVED — locals now created on-demand in lower_stmt.
 * This ensures sequential processing order for scope conflict resolution.
 * See ir_add_local: same name + different type → suffixed unique local. */

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

/* Forward declarations */
static void lower_stmt(LowerCtx *ctx, Node *node);
static void rewrite_idents(LowerCtx *ctx, Node *expr);
static int lower_expr(LowerCtx *ctx, Node *expr);
static void lower_orelse_to_dest(LowerCtx *ctx, int dest_local, Node *orelse_node, int line);

/* can_lower_expr removed — lower_expr is now unconditional.
 * ALL expressions get decomposed to local IDs. Complex expressions
 * (calls, intrinsics, builtins, casts, orelse, struct_init) go through
 * the passthrough path in lower_expr which creates IR_ASSIGN{dest,expr}.
 * The emitter handles IR_ASSIGN by calling emit_expr directly.
 * All OTHER IR ops (BRANCH, RETURN, CALL, BINOP, etc.) use local IDs only. */

/* lower_expr decomposes expressions into three-address-code. */

/* ================================================================
 * Three-address-code expression decomposition
 *
 * lower_expr(ctx, node) → returns local_id holding the result.
 * Every sub-expression is decomposed into a temp local + instruction.
 * Variable references use local IDs (ir_find_local), not source names.
 * This is the core of the three-address-code transition.
 * ================================================================ */

/* Create a temp local for intermediate results */
static int create_temp(LowerCtx *ctx, Type *type, int line) {
    char buf[32];
    int tl = snprintf(buf, sizeof(buf), "_zer_t%d", ctx->temp_count++);
    char *name = (char *)arena_alloc(ctx->arena, tl + 1);
    memcpy(name, buf, tl + 1);
    return ir_add_local(ctx->func, ctx->arena,
                        name, (uint32_t)tl, type,
                        false, false, true, line);
}

/* Emit helper: creates instruction, adds to current block */
static void emit_3ac(LowerCtx *ctx, IRInst inst) {
    ir_block_add_inst(&ctx->func->blocks[ctx->current_block], ctx->arena, inst);
}

/* Lower one expression to a local ID.
 * Creates temp locals and emits instructions for each sub-expression.
 * Returns the local ID holding the result, or -1 for void/error. */
static int lower_expr(LowerCtx *ctx, Node *expr) {
    if (!expr) return -1;

    switch (expr->kind) {

    /* ---- Variable reference: just return the local ID ---- */
    case NODE_IDENT: {
        int id = ir_find_local(ctx->func,
                               expr->ident.name,
                               (uint32_t)expr->ident.name_len);
        if (id >= 0) return id;
        /* Not a local (could be a global, enum value, function name, etc.)
         * Fall through to passthrough — emit_expr handles these. */
        goto passthrough;
    }

    /* ---- Literals: create temp with constant value ---- */
    case NODE_INT_LIT: {
        Type *lt = checker_get_type(ctx->checker, expr);
        if (!lt) lt = ty_u64;
        int tmp = create_temp(ctx, lt, expr->loc.line);
        IRInst inst = make_inst(IR_LITERAL, expr->loc.line);
        inst.dest_local = tmp;
        inst.literal_int = (int64_t)expr->int_lit.value;
        inst.literal_kind = 0; /* int */
        emit_3ac(ctx, inst);
        return tmp;
    }
    case NODE_FLOAT_LIT: {
        Type *lt = checker_get_type(ctx->checker, expr);
        if (!lt) lt = ty_f64;
        int tmp = create_temp(ctx, lt, expr->loc.line);
        IRInst inst = make_inst(IR_LITERAL, expr->loc.line);
        inst.dest_local = tmp;
        inst.literal_float = expr->float_lit.value;
        inst.literal_kind = 1; /* float */
        emit_3ac(ctx, inst);
        return tmp;
    }
    case NODE_BOOL_LIT: {
        int tmp = create_temp(ctx, ty_bool, expr->loc.line);
        IRInst inst = make_inst(IR_LITERAL, expr->loc.line);
        inst.dest_local = tmp;
        inst.literal_int = expr->bool_lit.value ? 1 : 0;
        inst.literal_kind = 3; /* bool */
        emit_3ac(ctx, inst);
        return tmp;
    }
    case NODE_CHAR_LIT: {
        int tmp = create_temp(ctx, ty_u8, expr->loc.line);
        IRInst inst = make_inst(IR_LITERAL, expr->loc.line);
        inst.dest_local = tmp;
        inst.literal_int = (int64_t)expr->char_lit.value;
        inst.literal_kind = 5; /* char */
        emit_3ac(ctx, inst);
        return tmp;
    }
    case NODE_NULL_LIT: {
        Type *lt = checker_get_type(ctx->checker, expr);
        /* null has no inherent type — use pointer placeholder to avoid void temp */
        if (!lt || type_unwrap_distinct(lt)->kind == TYPE_VOID)
            lt = type_pointer(ctx->arena, ty_void);
        int tmp = create_temp(ctx, lt, expr->loc.line);
        IRInst inst = make_inst(IR_LITERAL, expr->loc.line);
        inst.dest_local = tmp;
        inst.literal_kind = 4; /* null */
        emit_3ac(ctx, inst);
        return tmp;
    }
    case NODE_STRING_LIT: {
        Type *lt = checker_get_type(ctx->checker, expr);
        int tmp = create_temp(ctx, lt, expr->loc.line);
        IRInst inst = make_inst(IR_LITERAL, expr->loc.line);
        inst.dest_local = tmp;
        inst.literal_str = expr->string_lit.value;
        inst.literal_str_len = (uint32_t)expr->string_lit.length;
        inst.literal_kind = 2; /* string */
        emit_3ac(ctx, inst);
        return tmp;
    }

    /* ---- Binary operations: decompose both sides ---- */
    case NODE_BINARY: {
        /* Complex operand types need emit_expr (opaque .ptr, struct compare, etc.) */
        Type *lt = checker_get_type(ctx->checker, expr->binary.left);
        Type *rty = checker_get_type(ctx->checker, expr->binary.right);
        if (lt) { Type *le = type_unwrap_distinct(lt);
            if (le->kind == TYPE_OPAQUE || le->kind == TYPE_STRUCT ||
                le->kind == TYPE_OPTIONAL || le->kind == TYPE_UNION)
                goto passthrough;
            if (le->kind == TYPE_POINTER && le->pointer.inner &&
                type_unwrap_distinct(le->pointer.inner)->kind == TYPE_OPAQUE)
                goto passthrough;
        }
        if (rty) { Type *re = type_unwrap_distinct(rty);
            if (re->kind == TYPE_OPAQUE || re->kind == TYPE_STRUCT ||
                re->kind == TYPE_OPTIONAL || re->kind == TYPE_UNION)
                goto passthrough;
        }
        /* Result type void/array → can't store */
        {
            Type *brt = checker_get_type(ctx->checker, expr);
            if (brt) { Type *eff = type_unwrap_distinct(brt);
                if (eff->kind == TYPE_VOID || eff->kind == TYPE_ARRAY) goto passthrough;
            }
        }
        int left = lower_expr(ctx, expr->binary.left);
        int right = lower_expr(ctx, expr->binary.right);
        Type *rt = checker_get_type(ctx->checker, expr);
        if (!rt) rt = ty_i32;
        int tmp = create_temp(ctx, rt, expr->loc.line);
        IRInst inst = make_inst(IR_BINOP, expr->loc.line);
        inst.dest_local = tmp;
        inst.src1_local = left;
        inst.src2_local = right;
        inst.op_token = expr->binary.op;
        inst.expr = expr; /* keep for type info during emission */
        emit_3ac(ctx, inst);
        return tmp;
    }

    /* ---- Unary operations: decompose operand ---- */
    case NODE_UNARY: {
        int operand = lower_expr(ctx, expr->unary.operand);
        Type *rt = checker_get_type(ctx->checker, expr);
        if (!rt) rt = ty_i32;
        int tmp = create_temp(ctx, rt, expr->loc.line);
        IRInst inst = make_inst(IR_UNOP, expr->loc.line);
        inst.dest_local = tmp;
        inst.src1_local = operand;
        inst.op_token = expr->unary.op;
        inst.expr = expr; /* keep for address-of (&) detection */
        emit_3ac(ctx, inst);
        return tmp;
    }

    /* ---- Field access: decompose for simple struct/pointer field read.
     * Complex types (Handle auto-deref, opaque, builtins, slices, arrays,
     * enums) go to passthrough → emit_expr handles the full logic. ---- */
    case NODE_FIELD: {
        /* Non-local objects (enum type, module prefix) → passthrough */
        if (expr->field.object && expr->field.object->kind == NODE_IDENT) {
            int obj_id = ir_find_local(ctx->func,
                expr->field.object->ident.name,
                (uint32_t)expr->field.object->ident.name_len);
            if (obj_id < 0) goto passthrough;
            /* Check object type — complex types need emit_expr */
            Type *ot = ctx->func->locals[obj_id].type;
            if (ot) {
                Type *ot_eff = type_unwrap_distinct(ot);
                if (ot_eff->kind == TYPE_HANDLE || ot_eff->kind == TYPE_OPAQUE ||
                    ot_eff->kind == TYPE_POOL || ot_eff->kind == TYPE_SLAB ||
                    ot_eff->kind == TYPE_RING || ot_eff->kind == TYPE_ARENA ||
                    ot_eff->kind == TYPE_ARRAY || ot_eff->kind == TYPE_SLICE)
                    goto passthrough;
            }
        } else {
            /* Non-ident object (nested field etc.) → passthrough */
            goto passthrough;
        }
        /* Array result → can't store in temp */
        Type *frt = checker_get_type(ctx->checker, expr);
        if (frt) {
            Type *frt_eff = type_unwrap_distinct(frt);
            if (frt_eff->kind == TYPE_ARRAY) goto passthrough;
        }
        int obj = lower_expr(ctx, expr->field.object);
        if (obj < 0) goto passthrough;
        int tmp = create_temp(ctx, frt, expr->loc.line);
        IRInst inst = make_inst(IR_FIELD_READ, expr->loc.line);
        inst.dest_local = tmp;
        inst.src1_local = obj;
        inst.field_name = expr->field.field_name;
        inst.field_name_len = (uint32_t)expr->field.field_name_len;
        inst.expr = expr; /* keep for type info during emission */
        emit_3ac(ctx, inst);
        return tmp;
    }

    /* ---- Index access: decompose object and index ---- */
    case NODE_INDEX: {
        Type *rt = checker_get_type(ctx->checker, expr);
        /* Array-result index (3D array) → can't C-assign */
        if (rt) { Type *rt_eff = type_unwrap_distinct(rt);
            if (rt_eff->kind == TYPE_ARRAY) goto passthrough;
        }
        /* Check if object is an array type (global arrays return -1 from lower_expr) */
        Type *obj_type = checker_get_type(ctx->checker, expr->index_expr.object);
        if (obj_type) { Type *oe = type_unwrap_distinct(obj_type);
            if (oe->kind == TYPE_ARRAY) goto passthrough; /* global array index → passthrough */
        }
        int obj = lower_expr(ctx, expr->index_expr.object);
        if (obj < 0) goto passthrough; /* object couldn't be decomposed → passthrough */
        int idx = lower_expr(ctx, expr->index_expr.index);
        int tmp = create_temp(ctx, rt, expr->loc.line);
        IRInst inst = make_inst(IR_INDEX_READ, expr->loc.line);
        inst.dest_local = tmp;
        inst.src1_local = obj;
        inst.src2_local = idx;
        inst.expr = expr; /* keep for bounds check info */
        emit_3ac(ctx, inst);
        return tmp;
    }

    /* ---- Complex expressions: passthrough to emit_expr during migration ---- */
    /* These keep Node *expr and use emit_expr. They'll be decomposed
     * incrementally as the three-address-code migration continues. */
    /* ---- Typecast: decompose inner expr, create IR_CAST ---- */
    case NODE_TYPECAST: {
        rewrite_idents(ctx, expr->typecast.expr);
        int src = lower_expr(ctx, expr->typecast.expr);
        Type *tgt = checker_get_type(ctx->checker, expr);
        if (!tgt) tgt = ty_i32;
        Type *tgt_eff = type_unwrap_distinct(tgt);
        if (tgt_eff->kind == TYPE_VOID) goto passthrough;
        int tmp = create_temp(ctx, tgt, expr->loc.line);
        IRInst inst = make_inst(IR_CAST, expr->loc.line);
        inst.dest_local = tmp;
        inst.src1_local = src;
        inst.cast_type = tgt;
        inst.expr = expr; /* keep for *opaque type_id logic in emitter */
        emit_3ac(ctx, inst);
        return tmp;
    }

    /* ---- Struct init: decompose field values ---- */
    case NODE_STRUCT_INIT: {
        Type *rt = checker_get_type(ctx->checker, expr);
        if (!rt) goto passthrough;
        Type *rt_eff = type_unwrap_distinct(rt);
        if (rt_eff->kind == TYPE_VOID || rt_eff->kind == TYPE_ARRAY) goto passthrough;
        /* Decompose field value expressions to locals */
        int *field_locals = NULL;
        if (expr->struct_init.field_count > 0) {
            field_locals = (int *)arena_alloc(ctx->arena,
                expr->struct_init.field_count * sizeof(int));
            for (int i = 0; i < expr->struct_init.field_count; i++) {
                rewrite_idents(ctx, expr->struct_init.fields[i].value);
                field_locals[i] = lower_expr(ctx, expr->struct_init.fields[i].value);
            }
        }
        int tmp = create_temp(ctx, rt, expr->loc.line);
        IRInst inst = make_inst(IR_STRUCT_INIT_DECOMP, expr->loc.line);
        inst.dest_local = tmp;
        inst.expr = expr; /* keep for field names + target type */
        inst.call_arg_locals = field_locals;
        inst.call_arg_local_count = expr->struct_init.field_count;
        inst.cast_type = rt; /* target struct type for compound literal */
        emit_3ac(ctx, inst);
        return tmp;
    }

    /* ---- Function calls: decompose args, create IR_CALL with local IDs ---- */
    case NODE_CALL: {
        rewrite_idents(ctx, expr);

        /* Detect builtins — these have type-name args that can't be decomposed.
         * Route builtins through IR_CALL with expr (emitter uses emit_expr). */
        bool call_is_builtin = false;
        bool call_is_comptime = expr->call.is_comptime_resolved;
        if (expr->call.callee && expr->call.callee->kind == NODE_FIELD &&
            expr->call.callee->field.object) {
            Type *ot = checker_get_type(ctx->checker, expr->call.callee->field.object);
            if (!ot && expr->call.callee->field.object->kind == NODE_IDENT) {
                Symbol *s = scope_lookup(ctx->checker->global_scope,
                    expr->call.callee->field.object->ident.name,
                    (uint32_t)expr->call.callee->field.object->ident.name_len);
                if (s) ot = s->type;
            }
            if (ot) {
                Type *ot_eff = type_unwrap_distinct(ot);
                if (ot_eff->kind == TYPE_POOL || ot_eff->kind == TYPE_SLAB ||
                    ot_eff->kind == TYPE_RING || ot_eff->kind == TYPE_ARENA ||
                    ot_eff->kind == TYPE_STRUCT /* Task.new/delete */)
                    call_is_builtin = true;
            }
        }

        /* Decompose arguments to locals (skip for builtins — type-name args) */
        int *arg_locals = NULL;
        int arg_count = expr->call.arg_count;
        if (arg_count > 0 && !call_is_builtin && !call_is_comptime) {
            arg_locals = (int *)arena_alloc(ctx->arena, arg_count * sizeof(int));
            for (int i = 0; i < arg_count; i++) {
                arg_locals[i] = lower_expr(ctx, expr->call.args[i]);
            }
        }
        Type *rt = checker_get_type(ctx->checker, expr);
        if (!rt) rt = ty_i32;
        Type *rt_eff = type_unwrap_distinct(rt);
        /* Void calls don't produce storable value */
        int tmp = -1;
        if (rt_eff->kind != TYPE_VOID && rt_eff->kind != TYPE_ARRAY) {
            tmp = create_temp(ctx, rt, expr->loc.line);
        }
        IRInst inst = make_inst(IR_CALL, expr->loc.line);
        inst.dest_local = tmp;
        inst.call_arg_locals = arg_locals;
        inst.call_arg_local_count = arg_count;
        inst.expr = expr; /* keep for builtin dispatch, module mangling, comptime */
        inst.args = expr->call.args; /* keep for non-decomposable args */
        inst.arg_count = arg_count;
        /* Extract func name for simple calls */
        if (expr->call.callee && expr->call.callee->kind == NODE_IDENT) {
            inst.func_name = expr->call.callee->ident.name;
            inst.func_name_len = (uint32_t)expr->call.callee->ident.name_len;
        }
        emit_3ac(ctx, inst);
        return tmp;
    }

    /* ---- Orelse: lower to IR branches unconditionally ---- */
    case NODE_ORELSE: {
        Type *rt = checker_get_type(ctx->checker, expr);
        if (!rt) rt = ty_i32;
        Type *rt_eff = type_unwrap_distinct(rt);
        if (rt_eff->kind == TYPE_VOID) {
            /* Void orelse (statement-level) — lower as branches, no result */
            lower_orelse_to_dest(ctx, -1, expr, expr->loc.line);
            return -1;
        }
        int tmp = create_temp(ctx, rt, expr->loc.line);
        lower_orelse_to_dest(ctx, tmp, expr, expr->loc.line);
        return tmp;
    }

    case NODE_INTRINSIC:
    case NODE_SLICE:
    default:
    passthrough: {
        /* Create temp, emit IR_ASSIGN with expr (passthrough to emit_expr).
         * This is the migration bridge — allows incremental transition. */
        rewrite_idents(ctx, expr);
        Type *rt = checker_get_type(ctx->checker, expr);
        if (!rt) rt = ty_i32; /* fallback — most expressions have some value type */
        /* Void/array expressions don't produce a storable value */
        Type *rt_eff = type_unwrap_distinct(rt);
        if (rt_eff->kind == TYPE_VOID || rt_eff->kind == TYPE_ARRAY) {
            IRInst inst = make_inst(IR_ASSIGN, expr->loc.line);
            inst.expr = expr;
            emit_3ac(ctx, inst);
            return -1;
        }
        int tmp = create_temp(ctx, rt, expr->loc.line);
        IRInst inst = make_inst(IR_ASSIGN, expr->loc.line);
        inst.dest_local = tmp;
        inst.expr = expr;
        emit_3ac(ctx, inst);
        return tmp;
    }
    }
}

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

/* ================================================================
 * Expression ident rewriting — three-address-code foundation
 *
 * When ir_add_local creates a suffixed local (same name, different type),
 * expression trees still reference the original source name. This function
 * walks an expression tree and rewrites NODE_IDENTs to use the IR local's
 * actual C name (which may be suffixed). Safe because IR lowering is the
 * last consumer of these AST nodes.
 * ================================================================ */

static void rewrite_idents(LowerCtx *ctx, Node *expr) {
    if (!expr) return;

    switch (expr->kind) {
    case NODE_IDENT: {
        int id = ir_find_local(ctx->func, expr->ident.name,
                               (uint32_t)expr->ident.name_len);
        if (id >= 0) {
            IRLocal *l = &ctx->func->locals[id];
            /* If C name differs from source name, rewrite the AST node */
            if (l->name_len != (uint32_t)expr->ident.name_len ||
                memcmp(l->name, expr->ident.name, l->name_len) != 0) {
                expr->ident.name = l->name;
                expr->ident.name_len = (int)l->name_len;
            }
        }
        break;
    }
    case NODE_BINARY:
        rewrite_idents(ctx, expr->binary.left);
        rewrite_idents(ctx, expr->binary.right);
        break;
    case NODE_UNARY:
        rewrite_idents(ctx, expr->unary.operand);
        break;
    case NODE_CALL:
        rewrite_idents(ctx, expr->call.callee);
        for (int i = 0; i < expr->call.arg_count; i++)
            rewrite_idents(ctx, expr->call.args[i]);
        break;
    case NODE_FIELD:
        rewrite_idents(ctx, expr->field.object);
        break;
    case NODE_INDEX:
        rewrite_idents(ctx, expr->index_expr.object);
        rewrite_idents(ctx, expr->index_expr.index);
        break;
    case NODE_ASSIGN:
        rewrite_idents(ctx, expr->assign.target);
        rewrite_idents(ctx, expr->assign.value);
        break;
    case NODE_ORELSE:
        rewrite_idents(ctx, expr->orelse.expr);
        rewrite_idents(ctx, expr->orelse.fallback);
        break;
    case NODE_INTRINSIC:
        for (int i = 0; i < expr->intrinsic.arg_count; i++)
            rewrite_idents(ctx, expr->intrinsic.args[i]);
        break;
    case NODE_SLICE:
        rewrite_idents(ctx, expr->slice.object);
        rewrite_idents(ctx, expr->slice.start);
        rewrite_idents(ctx, expr->slice.end);
        break;
    /* NODE_ADDR_OF and NODE_DEREF are both NODE_UNARY — handled above */
    case NODE_STRUCT_INIT:
        for (int i = 0; i < expr->struct_init.field_count; i++)
            rewrite_idents(ctx, expr->struct_init.fields[i].value);
        break;
    default:
        /* Literals, null, etc. — no idents to rewrite */
        break;
    }
}

/* ================================================================
 * Orelse lowering — proper branch pattern, not expression hack
 *
 * val = expr orelse return;  →
 *   %tmp = expr
 *   BRANCH %tmp.has_value → bb_ok, bb_fail
 *   bb_fail: DEFER_FIRE; RETURN
 *   bb_ok: %val = %tmp  (emitter unwraps .value)
 * ================================================================ */

/* Check if an expression contains NODE_ORELSE at the top level */
static Node *find_orelse(Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == NODE_ORELSE) return expr;
    return NULL;
}

/* Lower: dest_local = orelse_expr
 * Creates temp, branches, assigns unwrapped value on ok path.
 * Three patterns:
 *   val = expr orelse return;        → fail: return
 *   val = expr orelse { block };     → fail: block (may terminate)
 *   val = expr orelse default_value; → fail: dest = default, join
 */
static void lower_orelse_to_dest(LowerCtx *ctx, int dest_local, Node *orelse_node, int line) {
    /* Create temp for the optional result */
    char tmp_buf[32];
    int tn = ctx->temp_count++;
    int tl = snprintf(tmp_buf, sizeof(tmp_buf), "_zer_or%d", tn);
    /* Arena-allocate the name — stack buffer would dangle after this function returns */
    char *tmp_name = (char *)arena_alloc(ctx->arena, tl + 1);
    memcpy(tmp_name, tmp_buf, tl + 1);
    Type *opt_type = checker_get_type(ctx->checker, orelse_node->orelse.expr);
    int tmp_id = ir_add_local(ctx->func, ctx->arena,
        tmp_name, (uint32_t)tl, opt_type, false, false, true, line);

    /* Emit: tmp = expr */
    Node *inner = orelse_node->orelse.expr;
    rewrite_idents(ctx, inner);
    int obj_local, handle_local;
    IROpKind builtin = classify_builtin_call(ctx, inner, &obj_local, &handle_local);
    if (builtin != IR_NOP) {
        IRInst alloc = make_inst(builtin, line);
        alloc.dest_local = tmp_id;
        alloc.obj_local = obj_local;
        alloc.handle_local = handle_local;
        alloc.expr = inner;
        emit_inst(ctx, alloc);
    } else {
        IRInst assign_tmp = make_inst(IR_ASSIGN, line);
        assign_tmp.dest_local = tmp_id;
        assign_tmp.expr = inner;
        emit_inst(ctx, assign_tmp);
    }

    /* Determine if fallback always terminates (return/break/continue/goto) */
    bool fallback_terminates = orelse_node->orelse.fallback_is_return ||
                               orelse_node->orelse.fallback_is_break ||
                               orelse_node->orelse.fallback_is_continue;

    /* Create blocks */
    int bb_ok = ir_add_block(ctx->func, ctx->arena);
    int bb_fail = ir_add_block(ctx->func, ctx->arena);
    int bb_join = -1;
    if (!fallback_terminates) {
        bb_join = ir_add_block(ctx->func, ctx->arena);
    }

    /* Branch on has_value */
    IRInst br = make_inst(IR_BRANCH, line);
    br.cond_local = tmp_id;
    br.true_block = bb_ok;
    br.false_block = bb_fail;
    emit_inst(ctx, br);

    /* === Fail block === */
    ctx->current_block = bb_fail;
    if (orelse_node->orelse.fallback_is_return) {
        emit_defer_fire(ctx, line);
        IRInst ret = make_inst(IR_RETURN, line);
        emit_inst(ctx, ret);
    } else if (orelse_node->orelse.fallback_is_break && ctx->loop_exit_block >= 0) {
        IRInst go = make_inst(IR_GOTO, line);
        go.goto_block = ctx->loop_exit_block;
        emit_inst(ctx, go);
    } else if (orelse_node->orelse.fallback_is_continue && ctx->loop_continue_block >= 0) {
        IRInst go = make_inst(IR_GOTO, line);
        go.goto_block = ctx->loop_continue_block;
        emit_inst(ctx, go);
    } else if (orelse_node->orelse.fallback) {
        /* Block or default value fallback.
         * Lower the fallback as a statement — it may contain break/return/goto
         * which terminates the block. If it doesn't terminate, it's a default value. */
        lower_stmt(ctx, orelse_node->orelse.fallback);
        /* Check if the fallback actually terminated (break, return, etc.) */
        IRBlock *fb = &ctx->func->blocks[ctx->current_block];
        if (fb->inst_count == 0 || !ir_block_is_terminated(fb)) {
            /* Fallback didn't terminate — it's a default value expression.
             * But we already lowered it as statements. Need to assign result.
             * For simple default values (literals), the lowered stmt is an
             * IR_ASSIGN. For blocks with break/return, it terminated above. */
            if (bb_join >= 0) {
                IRInst go = make_inst(IR_GOTO, line);
                go.goto_block = bb_join;
                emit_inst(ctx, go);
            }
        }
    }
    /* If fail block still not terminated (bare orelse with no fallback = shouldn't happen) */
    {
        IRBlock *fb = &ctx->func->blocks[bb_fail];
        if (fb->inst_count == 0 || !ir_block_is_terminated(fb)) {
            if (bb_join >= 0) {
                IRInst go = make_inst(IR_GOTO, line);
                go.goto_block = bb_join;
                emit_inst(ctx, go);
            }
        }
    }

    /* === Ok block: unwrap value === */
    ctx->current_block = bb_ok;
    if (dest_local >= 0) {
        IRInst unwrap = make_inst(IR_ASSIGN, line);
        unwrap.dest_local = dest_local;
        unwrap.expr = orelse_node->orelse.expr; /* emitter appends .value */
        emit_inst(ctx, unwrap);
    }
    if (fallback_terminates) {
        /* No join needed — fail path exits, ok path continues */
    } else {
        /* Join block after both paths */
        IRInst go = make_inst(IR_GOTO, line);
        go.goto_block = bb_join;
        emit_inst(ctx, go);
        ctx->current_block = bb_join;
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
        /* On-demand local creation — creates at the point encountered during
         * sequential lowering. For scope conflicts (same name, different type),
         * ir_add_local creates a suffixed local. */
        Type *vt = checker_get_type(ctx->checker, node);
        int local_id = ir_add_local(ctx->func, ctx->arena,
            node->var_decl.name, (uint32_t)node->var_decl.name_len,
            vt, false, false, false, node->loc.line);
        if (local_id >= 0 && node->var_decl.init) {
            /* Rewrite idents in init expression to use correct local names */
            rewrite_idents(ctx, node->var_decl.init);
            /* Orelse lowered to IR branches when GCC stmt expr won't work:
             * - Async (Duff's case can't go inside ({...}))
             * - Inside loop (IR uses gotos — C break/continue won't work)
             * Full orelse→IR (Rust-style) deferred: lower_orelse_to_dest default-value
             * join path needs fixing for complex expression patterns. */
            Node *orelse = find_orelse(node->var_decl.init);
            bool need_ir_orelse = orelse && (ctx->func->is_async ||
                ctx->loop_exit_block >= 0 ||
                orelse->orelse.fallback_is_break || orelse->orelse.fallback_is_continue);
            if (need_ir_orelse) {
                lower_orelse_to_dest(ctx, local_id, orelse, node->loc.line);
            } else {
                /* Unified: lower_expr decomposes all inits.
                 * Simple expressions → local ID + IR_COPY.
                 * Complex expressions (calls, builtins, intrinsics, casts,
                 * orelse, struct_init) → IR_ASSIGN passthrough via lower_expr. */
                Node *init = node->var_decl.init;
                int src = lower_expr(ctx, init);
                if (src >= 0 && src != local_id) {
                    IRInst inst = make_inst(IR_COPY, node->loc.line);
                    inst.dest_local = local_id;
                    inst.src1_local = src;
                    emit_inst(ctx, inst);
                } else if (src < 0) {
                    /* lower_expr returned -1 (void expression).
                     * The call was already emitted as a side-effect IR_ASSIGN.
                     * If dest is ?void, assign {has_value=1} (BUG-408 pattern). */
                    Type *vt_eff = vt ? type_unwrap_distinct(vt) : NULL;
                    if (vt_eff && vt_eff->kind == TYPE_OPTIONAL &&
                        vt_eff->optional.inner &&
                        type_unwrap_distinct(vt_eff->optional.inner)->kind == TYPE_VOID) {
                        /* dest = (_zer_opt_void){1} via IR_LITERAL */
                        IRInst lit = make_inst(IR_LITERAL, node->loc.line);
                        lit.dest_local = local_id;
                        lit.literal_int = 1;
                        lit.literal_kind = 6; /* ?void has_value=1 */
                        emit_inst(ctx, lit);
                    }
                }
            }
        }
        break;
    }

    /* ---- Expression statement: call or assign ---- */
    case NODE_EXPR_STMT: {
        Node *expr = node->expr_stmt.expr;
        if (!expr) break;

        /* Rewrite idents in expression to use correct local names */
        rewrite_idents(ctx, expr);

        /* Orelse to IR branches when async or inside loop */
        {
            Node *orelse = find_orelse(expr);
            bool need_ir = orelse && (ctx->func->is_async ||
                ctx->loop_exit_block >= 0 ||
                orelse->orelse.fallback_is_break || orelse->orelse.fallback_is_continue);
            if (need_ir) {
                lower_orelse_to_dest(ctx, -1, orelse, node->loc.line);
                break;
            }
        }

        /* Unified: route ALL expressions through lower_expr.
         * Calls → IR_CALL, assignments → IR_ASSIGN passthrough,
         * everything else → decomposed or passthrough. */
        lower_expr(ctx, expr);
        break;
    }

    /* ---- If/else: branch + basic blocks ---- */
    case NODE_IF: {
        /* comptime if: evaluate condition, only lower the taken branch */
        if (node->if_stmt.is_comptime) {
            int64_t cval = eval_const_expr(node->if_stmt.cond);
            if (cval) {
                if (node->if_stmt.then_body) lower_stmt(ctx, node->if_stmt.then_body);
            } else {
                if (node->if_stmt.else_body) lower_stmt(ctx, node->if_stmt.else_body);
            }
            break;
        }
        int bb_then = ir_add_block(ctx->func, ctx->arena);
        int bb_else = node->if_stmt.else_body ?
                      ir_add_block(ctx->func, ctx->arena) : -1;
        int bb_join = ir_add_block(ctx->func, ctx->arena);

        /* If-unwrap capture: create local on-demand */
        bool has_capture = (node->if_stmt.capture_name != NULL);
        if (has_capture) {
            Type *cond_type = checker_get_type(ctx->checker, node->if_stmt.cond);
            Type *cap_type = NULL;
            if (cond_type) {
                Type *eff = type_unwrap_distinct(cond_type);
                if (eff && eff->kind == TYPE_OPTIONAL) {
                    cap_type = eff->optional.inner;
                    if (node->if_stmt.capture_is_ptr && cap_type)
                        cap_type = type_pointer(ctx->arena, cap_type);
                }
            }
            if (cap_type && cap_type->kind != TYPE_VOID) {
                ir_add_local(ctx->func, ctx->arena,
                    node->if_stmt.capture_name,
                    (uint32_t)node->if_stmt.capture_name_len,
                    cap_type, false, true, false, node->loc.line);
            }
        }

        /* ALWAYS rewrite idents (captures may reference the condition expr).
         * Then decompose if safe. */
        rewrite_idents(ctx, node->if_stmt.cond);
        IRInst br = make_inst(IR_BRANCH, node->loc.line);
        br.cond_local = lower_expr(ctx, node->if_stmt.cond);
        br.true_block = bb_then;
        br.false_block = bb_else >= 0 ? bb_else : bb_join;
        emit_inst(ctx, br);

        /* Then block */
        ctx->current_block = bb_then;
        if (has_capture) {
            int cap_id = ir_find_local(ctx->func,
                node->if_stmt.capture_name,
                (uint32_t)node->if_stmt.capture_name_len);
            if (cap_id >= 0 && br.cond_local >= 0) {
                /* Skip ?void captures — no value to unwrap */
                Type *cond_t = ctx->func->locals[br.cond_local].type;
                Type *cond_eff = cond_t ? type_unwrap_distinct(cond_t) : NULL;
                bool is_void_cap = (cond_eff && cond_eff->kind == TYPE_OPTIONAL &&
                    cond_eff->optional.inner &&
                    type_unwrap_distinct(cond_eff->optional.inner)->kind == TYPE_VOID);
                if (!is_void_cap) {
                    /* capture = condition (IR_COPY handles unwrap via type adaptation) */
                    IRInst cap = make_inst(IR_COPY, node->loc.line);
                    cap.dest_local = cap_id;
                    cap.src1_local = br.cond_local;
                    emit_inst(ctx, cap);
                }
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
            rewrite_idents(ctx, node->for_stmt.cond);
            IRInst br = make_inst(IR_BRANCH, node->loc.line);
            br.cond_local = lower_expr(ctx, node->for_stmt.cond);
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
            rewrite_idents(ctx, node->for_stmt.step);
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
        {
        rewrite_idents(ctx, node->while_stmt.cond);
        IRInst br = make_inst(IR_BRANCH, node->loc.line);
        br.cond_local = lower_expr(ctx, node->while_stmt.cond);
        br.true_block = bb_body;
        br.false_block = bb_exit;
        emit_inst(ctx, br);
        }

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
        {
        rewrite_idents(ctx, node->while_stmt.cond);
        IRInst br = make_inst(IR_BRANCH, node->loc.line);
        br.cond_local = lower_expr(ctx, node->while_stmt.cond);
        br.true_block = bb_body;
        br.false_block = bb_exit;
        emit_inst(ctx, br);
        }

        ctx->current_block = bb_exit;

        ctx->loop_exit_block = saved_exit;
        ctx->loop_continue_block = saved_cont;
        ctx->loop_defer_base = saved_defer;
        break;
    }

    /* ---- Switch: chain of branches ---- */
    case NODE_SWITCH: {
        /* Union/optional/enum switch: complex tag/has_value dispatch.
         * Pass through as AST node — emitter handles via emit_stmt.
         * This is the ONLY remaining emit_stmt in the IR function body path.
         * Eliminating requires: lower tag comparison to IR_BRANCH, union
         * capture to IR_ASSIGN, arm bodies already lowered. ~150 lines. */
        Type *sw_type = checker_get_type(ctx->checker, node->switch_stmt.expr);
        Type *sw_eff = sw_type ? type_unwrap_distinct(sw_type) : NULL;
        if (sw_eff && (sw_eff->kind == TYPE_UNION || sw_eff->kind == TYPE_OPTIONAL ||
                       sw_eff->kind == TYPE_ENUM)) {
            IRInst pass = make_inst(IR_NOP, node->loc.line);
            pass.expr = node;
            emit_inst(ctx, pass);
            break;
        }

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

            /* Switch capture: create local on-demand + assignment */
            if (node->switch_stmt.arms[i].capture_name) {
                /* Create capture local if not already present */
                Type *cap_type = checker_get_type(ctx->checker, node->switch_stmt.expr);
                if (cap_type) {
                    ir_add_local(ctx->func, ctx->arena,
                        node->switch_stmt.arms[i].capture_name,
                        (uint32_t)node->switch_stmt.arms[i].capture_name_len,
                        cap_type, false, true, false, node->loc.line);
                }
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
        IRInst ret = make_inst(IR_RETURN, node->loc.line);
        /* Evaluate return expression BEFORE firing defers (BUG-442).
         * Defers may free handles used in the return expression. */
        Node *ret_expr = node->ret.expr;
        if (ret_expr) {
            rewrite_idents(ctx, ret_expr);
            ret.src1_local = lower_expr(ctx, ret_expr);
        }
        emit_defer_fire(ctx, node->loc.line);
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
        /* Create resume block FIRST so we know its ID */
        int resume_bb = ir_add_block(ctx->func, ctx->arena);
        IRInst y = make_inst(IR_YIELD, node->loc.line);
        y.goto_block = resume_bb; /* emitter uses this for goto after case label */
        emit_inst(ctx, y);
        ctx->current_block = resume_bb;
        break;
    }

    /* ---- Await ---- */
    case NODE_AWAIT: {
        int resume_bb = ir_add_block(ctx->func, ctx->arena);
        IRInst aw = make_inst(IR_AWAIT, node->loc.line);
        rewrite_idents(ctx, node->await_stmt.cond);
        aw.cond_local = lower_expr(ctx, node->await_stmt.cond);
        aw.goto_block = resume_bb;
        emit_inst(ctx, aw);
        ctx->current_block = resume_bb;
        break;
    }

    /* ---- Spawn ---- */
    case NODE_SPAWN: {
        /* Spawn uses complex wrapper structs + pthread_create.
         * Pass through as AST node for emit_stmt to handle. */
        IRInst sp = make_inst(IR_NOP, node->loc.line);
        sp.expr = node; /* emit_stmt handles NODE_SPAWN */
        emit_inst(ctx, sp);
        /* For scoped spawn, assign ThreadHandle */
        if (node->spawn_stmt.handle_name) {
            int th_id = ir_find_local(ctx->func,
                node->spawn_stmt.handle_name,
                (uint32_t)node->spawn_stmt.handle_name_len);
            if (th_id >= 0) {
                /* emit_stmt already assigned the pthread_t */
            }
        }
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
                /* Complex type — use checker's func_type for accurate param types.
                 * The TypeNode switch above only handles primitives.
                 * Pointer, struct, optional, etc. need checker resolution. */
                Type *func_type = checker_get_type(checker, func_decl);
                if (func_type && func_type->kind == TYPE_FUNC_PTR &&
                    (uint32_t)i < func_type->func_ptr.param_count) {
                    pt = func_type->func_ptr.params[i];
                } else {
                    /* Fallback: look up in scope */
                    Symbol *psym = scope_lookup(checker->global_scope,
                        p->name, (uint32_t)p->name_len);
                    if (psym && psym->type) pt = psym->type;
                }
                break;
            }
            }
        }
        if (!pt) pt = ty_void; /* fallback */
        ir_add_local(func, arena, p->name, (uint32_t)p->name_len,
                     pt, true, false, false, p->loc.line);
    }

    /* Phase 1b: Locals created on-demand during lower_stmt.
     * This ensures sequential processing order — scope-conflicting locals
     * (same name, different type) are created at the right time.
     * With upfront collect_locals, both would exist before any lowering,
     * making ir_find_local (last-match) return the wrong one. */

    /* Phase 2: Pre-scan for labels */
    collect_labels(&ctx, func_decl->func_decl.body);

    /* Phase 3: Create entry block and lower body */
    start_block(&ctx);
    lower_stmt(&ctx, func_decl->func_decl.body);

    /* Ensure CURRENT block is terminated (not last block — yield may
     * create blocks after the current one, making last != current). */
    IRBlock *cur = &func->blocks[ctx.current_block];
    if (cur->inst_count == 0 || !ir_block_is_terminated(cur)) {
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

    /* Locals created on-demand during lower_stmt */
    collect_labels(&ctx, interrupt->interrupt.body);

    start_block(&ctx);
    lower_stmt(&ctx, interrupt->interrupt.body);

    IRBlock *cur = &func->blocks[ctx.current_block];
    if (cur->inst_count == 0 || !ir_block_is_terminated(cur)) {
        IRInst ret = make_inst(IR_RETURN, interrupt->loc.line);
        emit_inst(&ctx, ret);
    }

    ir_compute_preds(func, ctx.arena);
    return func;
}
