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

/* Label → block mapping entry (BUG-575: stack-first dynamic buffer) */
typedef struct {
    const char *name;
    uint32_t len;
    int block_id;
} IRLabelMap;

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
    /* BUG-590: when >0, the next NODE_BLOCK should NOT fire+pop its own
     * defers — the enclosing construct (loop body, if-branch, switch arm)
     * already does it with the correct semantics (no-pop for loops so
     * break/continue can still fire; scoped fire for branches). Set by
     * for/while/do-while/if/switch handlers before lowering their body;
     * decremented by NODE_BLOCK. */
    int block_defers_managed;

    /* Label → block mapping (for goto/label).
     * Stack-first dynamic pattern (CLAUDE.md rule #7): start with inline
     * 32-slot array; overflow to arena-allocated doubling buffer. A function
     * with thousands of labels is rare but must not silently drop entries. */
    IRLabelMap label_inline[32];
    IRLabelMap *labels;       /* points to label_inline or arena buffer */
    int label_count;
    int label_capacity;

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
    /* BUG-578 fix: obj_local/handle_local used by deprecated builtin IR ops
     * (IR_POOL_ALLOC etc. — collapsed to IR_ASSIGN in Phase 8d). Default to
     * sentinel -1 so BUG-576's tightened ir_validate (>= 0 check) doesn't
     * reject every IR_RETURN / IR_GOTO (they leave these fields unused). */
    inst.obj_local = -1;
    inst.handle_local = -1;
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

/* Find or create label → block mapping.
 * BUG-575: grows via arena-allocated doubling buffer when inline 32-slot
 * overflows. Fixes CLAUDE.md rule #7 violation (silent drop past 128). */
static int find_label_block(LowerCtx *ctx, const char *name, uint32_t len) {
    for (int i = 0; i < ctx->label_count; i++) {
        if (ctx->labels[i].len == len &&
            memcmp(ctx->labels[i].name, name, len) == 0)
            return ctx->labels[i].block_id;
    }
    /* Create new block for this label */
    int bid = ir_add_block(ctx->func, ctx->arena);
    if (ctx->label_count >= ctx->label_capacity) {
        int new_cap = ctx->label_capacity * 2;
        IRLabelMap *new_labels = (IRLabelMap *)arena_alloc(
            ctx->arena, new_cap * sizeof(IRLabelMap));
        memcpy(new_labels, ctx->labels, ctx->label_count * sizeof(IRLabelMap));
        ctx->labels = new_labels;
        ctx->label_capacity = new_cap;
    }
    ctx->labels[ctx->label_count].name = name;
    ctx->labels[ctx->label_count].len = len;
    ctx->labels[ctx->label_count].block_id = bid;
    ctx->label_count++;
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
static void rewrite_defer_body_idents(LowerCtx *ctx, Node *stmt);
/* Targeted rewrite: walk AST and rewrite every NODE_IDENT matching
 * `from_name` to `to_name`. Used by switch arm lowering to scope captures —
 * arm captures get unique C names so they don't shadow same-named captures
 * from other switches in the function. */
static void rewrite_capture_name(Node *node, const char *from_name, uint32_t from_len,
                                 const char *to_name, uint32_t to_len);
static int lower_expr(LowerCtx *ctx, Node *expr);
static void lower_orelse_to_dest(LowerCtx *ctx, int dest_local, Node *orelse_node, int line);
static void pre_lower_orelse(LowerCtx *ctx, Node **pp, int line);

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
        /* Address-of (&) must preserve the operand's lvalue. Decomposing
         * `arr[0]` to a temp then taking &temp gives a pointer to the copy,
         * not the array element. Passthrough keeps `&arr[0]` intact. */
        if (expr->unary.op == TOK_AMP) goto passthrough;
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
        /* BUG-580 fix: if type unknown (freshly synthesized NODE_FIELD from
         * NODE_SWITCH lowering or similar), infer from object + field name.
         * Without a type, create_temp makes a typeless local that the emitter
         * skips declaring — leading to undefined C identifiers. */
        if (!frt) {
            Type *ot = NULL;
            if (expr->field.object && expr->field.object->kind == NODE_IDENT) {
                int obj_id_pre = ir_find_local(ctx->func,
                    expr->field.object->ident.name,
                    (uint32_t)expr->field.object->ident.name_len);
                if (obj_id_pre >= 0) ot = ctx->func->locals[obj_id_pre].type;
            }
            Type *ot_eff = ot ? type_unwrap_distinct(ot) : NULL;
            /* Unwrap pointer for sw_ptr->_tag / sw_ptr->variant cases */
            if (ot_eff && ot_eff->kind == TYPE_POINTER && ot_eff->pointer.inner)
                ot_eff = type_unwrap_distinct(ot_eff->pointer.inner);
            if (ot_eff && ot_eff->kind == TYPE_OPTIONAL) {
                if (expr->field.field_name_len == 9 &&
                    memcmp(expr->field.field_name, "has_value", 9) == 0) {
                    frt = ty_bool;
                } else if (expr->field.field_name_len == 5 &&
                           memcmp(expr->field.field_name, "value", 5) == 0) {
                    frt = ot_eff->optional.inner;
                }
            } else if (ot_eff && ot_eff->kind == TYPE_UNION) {
                if (expr->field.field_name_len == 4 &&
                    memcmp(expr->field.field_name, "_tag", 4) == 0) {
                    frt = ty_i32;
                } else {
                    /* Look up variant type by name */
                    for (uint32_t ui = 0; ui < ot_eff->union_type.variant_count; ui++) {
                        if (ot_eff->union_type.variants[ui].name_len == expr->field.field_name_len &&
                            memcmp(ot_eff->union_type.variants[ui].name,
                                   expr->field.field_name,
                                   expr->field.field_name_len) == 0) {
                            frt = ot_eff->union_type.variants[ui].type;
                            break;
                        }
                    }
                }
            } else if (ot_eff && ot_eff->kind == TYPE_STRUCT) {
                for (uint32_t fi = 0; fi < ot_eff->struct_type.field_count; fi++) {
                    if (ot_eff->struct_type.fields[fi].name_len == expr->field.field_name_len &&
                        memcmp(ot_eff->struct_type.fields[fi].name,
                               expr->field.field_name,
                               expr->field.field_name_len) == 0) {
                        frt = ot_eff->struct_type.fields[fi].type;
                        break;
                    }
                }
            }
        }
        if (!frt) goto passthrough; /* still no type → can't decompose safely */
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
            if (oe->kind == TYPE_ARRAY) {
                /* Global array passthrough, but first decompose complex index
                 * (orelse, call chains) to a local — emit_rewritten_node can't
                 * handle orelse. Rewrite the index AST node to reference the local. */
                if (expr->index_expr.index &&
                    expr->index_expr.index->kind == NODE_ORELSE) {
                    int idx_id = lower_expr(ctx, expr->index_expr.index);
                    if (idx_id >= 0) {
                        IRLocal *il = &ctx->func->locals[idx_id];
                        Node *new_idx = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(new_idx, 0, sizeof(Node));
                        new_idx->kind = NODE_IDENT;
                        new_idx->loc = expr->loc;
                        new_idx->ident.name = il->name;
                        new_idx->ident.name_len = (size_t)il->name_len;
                        expr->index_expr.index = new_idx;
                    }
                }
                goto passthrough; /* global array index → passthrough */
            }
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

    /* Compound assignment (+=, -=, etc.) with complex value — decompose the
     * value so nested orelse inside the RHS gets properly lowered. Without
     * this, `t += sink(e orelse break)` passes passthrough, emitter emits
     * the call arg via emit_rewritten_node which has no NODE_ORELSE case.
     * Simple `x = Y` is handled at statement level; here we focus on
     * compound ops which need the original target + op preserved. */
    case NODE_ASSIGN: {
        if (expr->assign.op == TOK_EQ) goto passthrough;
        /* Decompose RHS into a local; synthesize `target op= tmp_ident` so the
         * passthrough emitter emits a simple compound assign with no nested
         * orelse visible. */
        int val_local = lower_expr(ctx, expr->assign.value);
        if (val_local < 0) goto passthrough; /* void/array RHS — can't decompose */
        IRLocal *vloc = &ctx->func->locals[val_local];
        Node *tmp_id = (Node *)arena_alloc(ctx->arena, sizeof(Node));
        memset(tmp_id, 0, sizeof(Node));
        tmp_id->kind = NODE_IDENT;
        tmp_id->loc = expr->loc;
        tmp_id->ident.name = vloc->name;
        tmp_id->ident.name_len = (size_t)vloc->name_len;
        Node *new_assign = (Node *)arena_alloc(ctx->arena, sizeof(Node));
        memset(new_assign, 0, sizeof(Node));
        new_assign->kind = NODE_ASSIGN;
        new_assign->loc = expr->loc;
        new_assign->assign.op = expr->assign.op;
        new_assign->assign.target = expr->assign.target;
        new_assign->assign.value = tmp_id;
        /* Emit as void IR_ASSIGN — compound assign result is statement-like. */
        IRInst inst = make_inst(IR_ASSIGN, expr->loc.line);
        inst.expr = new_assign;
        emit_3ac(ctx, inst);
        return -1;
    }

    case NODE_INTRINSIC:
    case NODE_SLICE:
    default:
    passthrough: {
        /* Create temp, emit IR_ASSIGN with expr (passthrough to emit_expr).
         * This is the migration bridge — allows incremental transition. */
        rewrite_idents(ctx, expr);
        /* Any NODE_ORELSE in this expression must be pre-lowered — emit_rewritten_node
         * has no NODE_ORELSE case. Walks the AST and replaces each orelse with a
         * NODE_IDENT referencing a tmp local that gets the orelse result. */
        pre_lower_orelse(ctx, &expr, expr->loc.line);
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

/* Emit IR_DEFER_FIRE for pending defers (fire all, no pop — function/return exit) */
static void emit_defer_fire(LowerCtx *ctx, int line) {
    if (ctx->defer_count > 0) {
        IRInst fire = make_inst(IR_DEFER_FIRE, line);
        emit_inst(ctx, fire);
    }
}

/* Emit scoped IR_DEFER_FIRE: fire defers from top down to base.
 * pop=true: remove them from emitter stack (for end of loop iteration).
 * pop=false: keep on stack (for mid-body break/continue). */
static void emit_defer_fire_scoped(LowerCtx *ctx, int base, bool pop, int line) {
    if (ctx->defer_count > base) {
        IRInst fire = make_inst(IR_DEFER_FIRE, line);
        fire.cond_local = base;
        fire.src2_local = pop ? 0 : 1;
        emit_inst(ctx, fire);
    }
}

/* Emit "pop only" op — doesn't emit defer bodies, just decrements emitter's
 * compile-time defer_stack count. Used at loop exit after all body paths have
 * already emitted their defer bodies (with no-pop). */
static void emit_defer_pop_only(LowerCtx *ctx, int base, int line) {
    if (ctx->defer_count > base) {
        IRInst fire = make_inst(IR_DEFER_FIRE, line);
        fire.cond_local = base;
        fire.src2_local = 2;
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
    case NODE_TYPECAST:
        /* BUG-573: (Type)expr nested in passthrough routes (e.g., NODE_ASSIGN
         * with complex rvalue) must rewrite the inner expr to reach suffixed
         * locals created by scope shadowing. Without this, `d.f = (u32)m`
         * after `?u32 m` is shadowed by `u32 m` emits stale `m` name. */
        rewrite_idents(ctx, expr->typecast.expr);
        break;
    /* NODE_ADDR_OF and NODE_DEREF are both NODE_UNARY — handled above */
    /* NODE_CAST and NODE_SIZEOF are unused (see docs/compiler-internals.md) */
    case NODE_STRUCT_INIT:
        for (int i = 0; i < expr->struct_init.field_count; i++)
            rewrite_idents(ctx, expr->struct_init.fields[i].value);
        break;
    default:
        /* Literals, null, etc. — no idents to rewrite */
        break;
    }
}

/* Walk a statement AST (defer body) and call rewrite_idents on every
 * contained expression. Ensures identifiers inside defer bodies resolve
 * to the correct (scope-suffixed) IR locals. Without this, a defer
 * body declared in an inner scope that references a shadowed name
 * (`defer pool.free(h);` where `h` shadows outer `h`) would lookup by
 * the raw name and potentially resolve to the outer local, causing
 * defer-free tracking to miss the inner handle. */
static void rewrite_defer_body_idents(LowerCtx *ctx, Node *stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < stmt->block.stmt_count; i++)
            rewrite_defer_body_idents(ctx, stmt->block.stmts[i]);
        break;
    case NODE_EXPR_STMT:
        rewrite_idents(ctx, stmt->expr_stmt.expr);
        break;
    case NODE_VAR_DECL:
        rewrite_idents(ctx, stmt->var_decl.init);
        break;
    case NODE_RETURN:
        rewrite_idents(ctx, stmt->ret.expr);
        break;
    case NODE_IF:
        rewrite_idents(ctx, stmt->if_stmt.cond);
        rewrite_defer_body_idents(ctx, stmt->if_stmt.then_body);
        rewrite_defer_body_idents(ctx, stmt->if_stmt.else_body);
        break;
    case NODE_FOR:
        rewrite_defer_body_idents(ctx, stmt->for_stmt.init);
        rewrite_idents(ctx, stmt->for_stmt.cond);
        rewrite_idents(ctx, stmt->for_stmt.step);
        rewrite_defer_body_idents(ctx, stmt->for_stmt.body);
        break;
    case NODE_WHILE:
    case NODE_DO_WHILE:
        rewrite_idents(ctx, stmt->while_stmt.cond);
        rewrite_defer_body_idents(ctx, stmt->while_stmt.body);
        break;
    case NODE_SWITCH:
        rewrite_idents(ctx, stmt->switch_stmt.expr);
        for (int i = 0; i < stmt->switch_stmt.arm_count; i++)
            rewrite_defer_body_idents(ctx, stmt->switch_stmt.arms[i].body);
        break;
    case NODE_CRITICAL:
        rewrite_defer_body_idents(ctx, stmt->critical.body);
        break;
    case NODE_ONCE:
        rewrite_defer_body_idents(ctx, stmt->once.body);
        break;
    case NODE_DEFER:
        /* nested defer — rare, handle transitively */
        rewrite_defer_body_idents(ctx, stmt->defer.body);
        break;
    default:
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

/* ================================================================
 * BUG-594: Shared struct auto-locking for IR path
 *
 * The AST emitter does auto lock/unlock around statements that touch
 * `shared struct` fields (see emit_stmt NODE_BLOCK in emitter.c). The
 * IR path went through emit_ir_inst instead and lost this wrapping —
 * shared access was emitted without any mutex, causing data races
 * under thread contention (rt_sync_send_in_std flaky failure).
 *
 * Fix: detect the shared root of each source statement at lowering
 * time and wrap with IR_LOCK / IR_UNLOCK. The emitter then calls the
 * same emit_shared_lock_mode / emit_shared_unlock helpers the AST
 * path uses.
 *
 * Per-statement (not grouped across consecutive statements): simpler,
 * correct, slightly less efficient than AST's grouping. Optimization
 * for grouping can be added later if profiling shows it matters.
 * ================================================================ */

/* Walk the field/index/deref chain to the root ident, check if that
 * root's type is `shared struct` (directly or via pointer param). */
static Node *find_shared_root_expr(Checker *c, Node *expr);

static Node *find_shared_root_expr(Checker *c, Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == NODE_FIELD) {
        Node *root = expr;
        while (root->kind == NODE_FIELD) root = root->field.object;
        while (root->kind == NODE_INDEX) root = root->index_expr.object;
        while (root->kind == NODE_UNARY && root->unary.op == TOK_STAR)
            root = root->unary.operand;
        if (root->kind == NODE_IDENT) {
            Type *t = checker_get_type(c, root);
            if (t) {
                Type *eff = type_unwrap_distinct(t);
                if (eff->kind == TYPE_STRUCT && eff->struct_type.is_shared) return root;
                if (eff->kind == TYPE_POINTER) {
                    Type *inner = type_unwrap_distinct(eff->pointer.inner);
                    if (inner && inner->kind == TYPE_STRUCT && inner->struct_type.is_shared)
                        return root;
                }
            }
        }
    }
    /* Recurse into sub-expressions */
    Node *found = NULL;
    if (expr->kind == NODE_BINARY) {
        found = find_shared_root_expr(c, expr->binary.left);
        if (!found) found = find_shared_root_expr(c, expr->binary.right);
    } else if (expr->kind == NODE_ASSIGN) {
        found = find_shared_root_expr(c, expr->assign.target);
        if (!found) found = find_shared_root_expr(c, expr->assign.value);
    } else if (expr->kind == NODE_CALL) {
        for (int i = 0; i < expr->call.arg_count && !found; i++)
            found = find_shared_root_expr(c, expr->call.args[i]);
    } else if (expr->kind == NODE_UNARY) {
        found = find_shared_root_expr(c, expr->unary.operand);
    } else if (expr->kind == NODE_INDEX) {
        found = find_shared_root_expr(c, expr->index_expr.object);
    } else if (expr->kind == NODE_ORELSE) {
        found = find_shared_root_expr(c, expr->orelse.expr);
    } else if (expr->kind == NODE_TYPECAST) {
        found = find_shared_root_expr(c, expr->typecast.expr);
    }
    return found;
}

static Node *find_shared_root_in_stmt_ir(Checker *c, Node *stmt) {
    if (!stmt) return NULL;
    switch (stmt->kind) {
    case NODE_EXPR_STMT: return find_shared_root_expr(c, stmt->expr_stmt.expr);
    case NODE_VAR_DECL:  return find_shared_root_expr(c, stmt->var_decl.init);
    case NODE_RETURN:    return find_shared_root_expr(c, stmt->ret.expr);
    /* IF / WHILE / FOR / SWITCH conditions: don't wrap here — the
     * lowering of those constructs decomposes the cond separately.
     * Only wrap the simple statement shapes where the whole stmt is
     * covered by one lock scope. */
    default: return NULL;
    }
}

/* Does this source statement WRITE to a shared field? */
static bool stmt_writes_shared_ir(Node *stmt) {
    if (!stmt) return false;
    if (stmt->kind == NODE_EXPR_STMT && stmt->expr_stmt.expr &&
        stmt->expr_stmt.expr->kind == NODE_ASSIGN)
        return true;
    /* Conservative default for VAR_DECL init: reads shared — the decl
     * itself writes to a local, not the shared field. So read lock. */
    return false;
}

static void emit_shared_lock_if_needed(LowerCtx *ctx, Node *stmt, Node **out_root) {
    Node *root = find_shared_root_in_stmt_ir(ctx->checker, stmt);
    *out_root = root;
    if (!root) return;
    IRInst lock = make_inst(IR_LOCK, stmt->loc.line);
    lock.expr = root;
    lock.src2_local = stmt_writes_shared_ir(stmt) ? 1 : 0;
    emit_inst(ctx, lock);
}

static void emit_shared_unlock_if_needed(LowerCtx *ctx, Node *stmt, Node *root) {
    if (!root) return;
    IRInst unlock = make_inst(IR_UNLOCK, stmt->loc.line);
    unlock.expr = root;
    emit_inst(ctx, unlock);
}

/* Check if an expression contains NODE_ORELSE at the top level */
static Node *find_orelse(Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == NODE_ORELSE) return expr;
    /* BUG-577: `target = <something> orelse break;` — the orelse is the
     * VALUE of an assignment. Without recursing, NODE_EXPR_STMT's
     * find_orelse returns NULL and the orelse isn't lowered, leaving
     * emitter with an unhandled NODE_ORELSE. */
    if (expr->kind == NODE_ASSIGN && expr->assign.op == TOK_EQ)
        return find_orelse(expr->assign.value);
    return NULL;
}

/* Walk an expression tree; for every NODE_ORELSE found at any depth, lower
 * it to a fresh tmp (via lower_orelse_to_dest) and rewrite the slot to a
 * NODE_IDENT referencing the tmp. After this call, the expression is
 * guaranteed to contain no NODE_ORELSE — passthrough emission via
 * emit_rewritten_node won't hit the unhandled-node default.
 *
 * Takes Node** so it can REPLACE the caller's slot. Used before passthrough
 * in lower_expr (default path) and at other sites where AST survives to
 * emission. Handles terminating fallbacks (break/continue/return) correctly
 * via lower_orelse_to_dest which emits IR gotos. */

/* Targeted rewrite: NODE_IDENT("v") → NODE_IDENT("v_unique") in an AST subtree.
 * Used by switch arm lowering: the arm's capture gets a unique C name
 * (to avoid shadowing same-named captures from other switches in the same
 * function via IR's flat local namespace), and this walker replaces every
 * reference to the bare source name inside the arm body. */
static void rewrite_capture_name(Node *node, const char *from_name, uint32_t from_len,
                                 const char *to_name, uint32_t to_len) {
    if (!node) return;
    switch (node->kind) {
    case NODE_IDENT:
        if (node->ident.name_len == (size_t)from_len &&
            memcmp(node->ident.name, from_name, from_len) == 0) {
            node->ident.name = to_name;
            node->ident.name_len = (size_t)to_len;
        }
        break;
    case NODE_BINARY:
        rewrite_capture_name(node->binary.left, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->binary.right, from_name, from_len, to_name, to_len);
        break;
    case NODE_UNARY:
        rewrite_capture_name(node->unary.operand, from_name, from_len, to_name, to_len);
        break;
    case NODE_CALL:
        rewrite_capture_name(node->call.callee, from_name, from_len, to_name, to_len);
        for (int i = 0; i < node->call.arg_count; i++)
            rewrite_capture_name(node->call.args[i], from_name, from_len, to_name, to_len);
        break;
    case NODE_FIELD:
        rewrite_capture_name(node->field.object, from_name, from_len, to_name, to_len);
        break;
    case NODE_INDEX:
        rewrite_capture_name(node->index_expr.object, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->index_expr.index, from_name, from_len, to_name, to_len);
        break;
    case NODE_SLICE:
        rewrite_capture_name(node->slice.object, from_name, from_len, to_name, to_len);
        if (node->slice.start) rewrite_capture_name(node->slice.start, from_name, from_len, to_name, to_len);
        if (node->slice.end) rewrite_capture_name(node->slice.end, from_name, from_len, to_name, to_len);
        break;
    case NODE_ASSIGN:
        rewrite_capture_name(node->assign.target, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->assign.value, from_name, from_len, to_name, to_len);
        break;
    case NODE_ORELSE:
        rewrite_capture_name(node->orelse.expr, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->orelse.fallback, from_name, from_len, to_name, to_len);
        break;
    case NODE_INTRINSIC:
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            rewrite_capture_name(node->intrinsic.args[i], from_name, from_len, to_name, to_len);
        break;
    case NODE_TYPECAST:
        rewrite_capture_name(node->typecast.expr, from_name, from_len, to_name, to_len);
        break;
    case NODE_STRUCT_INIT:
        for (int i = 0; i < node->struct_init.field_count; i++)
            rewrite_capture_name(node->struct_init.fields[i].value, from_name, from_len, to_name, to_len);
        break;
    /* Statement-level nodes — recurse into their expression/body children */
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            rewrite_capture_name(node->block.stmts[i], from_name, from_len, to_name, to_len);
        break;
    case NODE_EXPR_STMT:
        rewrite_capture_name(node->expr_stmt.expr, from_name, from_len, to_name, to_len);
        break;
    case NODE_VAR_DECL:
        rewrite_capture_name(node->var_decl.init, from_name, from_len, to_name, to_len);
        break;
    case NODE_RETURN:
        rewrite_capture_name(node->ret.expr, from_name, from_len, to_name, to_len);
        break;
    case NODE_IF:
        rewrite_capture_name(node->if_stmt.cond, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->if_stmt.then_body, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->if_stmt.else_body, from_name, from_len, to_name, to_len);
        break;
    case NODE_FOR:
        rewrite_capture_name(node->for_stmt.init, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->for_stmt.cond, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->for_stmt.step, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->for_stmt.body, from_name, from_len, to_name, to_len);
        break;
    case NODE_WHILE:
    case NODE_DO_WHILE:
        rewrite_capture_name(node->while_stmt.cond, from_name, from_len, to_name, to_len);
        rewrite_capture_name(node->while_stmt.body, from_name, from_len, to_name, to_len);
        break;
    case NODE_SWITCH:
        rewrite_capture_name(node->switch_stmt.expr, from_name, from_len, to_name, to_len);
        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            /* Don't recurse into an inner arm if it captures the SAME name —
             * its own capture shadows the outer one for its scope. */
            SwitchArm *ia = &node->switch_stmt.arms[i];
            if (ia->capture_name && ia->capture_name_len == (size_t)from_len &&
                memcmp(ia->capture_name, from_name, from_len) == 0) {
                continue;
            }
            rewrite_capture_name(ia->body, from_name, from_len, to_name, to_len);
        }
        break;
    case NODE_DEFER:
        rewrite_capture_name(node->defer.body, from_name, from_len, to_name, to_len);
        break;
    case NODE_CRITICAL:
        rewrite_capture_name(node->critical.body, from_name, from_len, to_name, to_len);
        break;
    case NODE_ONCE:
        rewrite_capture_name(node->once.body, from_name, from_len, to_name, to_len);
        break;
    case NODE_AWAIT:
        rewrite_capture_name(node->await_stmt.cond, from_name, from_len, to_name, to_len);
        break;
    case NODE_STATIC_ASSERT:
        rewrite_capture_name(node->static_assert_stmt.cond, from_name, from_len, to_name, to_len);
        break;
    /* Leaf / no-expr-children nodes — nothing to rewrite */
    default:
        break;
    }
}

static void pre_lower_orelse(LowerCtx *ctx, Node **pp, int line) {
    if (!pp || !*pp) return;
    Node *n = *pp;
    if (n->kind == NODE_ORELSE) {
        /* Determine result type. For non-void orelse, lower to tmp. */
        Type *rt = checker_get_type(ctx->checker, n);
        if (!rt) {
            Type *it = checker_get_type(ctx->checker, n->orelse.expr);
            if (it) {
                Type *ie = type_unwrap_distinct(it);
                if (ie && ie->kind == TYPE_OPTIONAL) rt = ie->optional.inner;
            }
            if (!rt) rt = ty_i32;
        }
        Type *rt_eff = type_unwrap_distinct(rt);
        if (rt_eff->kind == TYPE_VOID) {
            /* Void orelse at expression position — shouldn't normally happen,
             * but lower for side effects and replace with 0 constant. */
            lower_orelse_to_dest(ctx, -1, n, line);
            Node *lit = (Node *)arena_alloc(ctx->arena, sizeof(Node));
            memset(lit, 0, sizeof(Node));
            lit->kind = NODE_INT_LIT;
            lit->loc = n->loc;
            *pp = lit;
            return;
        }
        int tmp = create_temp(ctx, rt, line);
        lower_orelse_to_dest(ctx, tmp, n, line);
        IRLocal *tloc = &ctx->func->locals[tmp];
        Node *id = (Node *)arena_alloc(ctx->arena, sizeof(Node));
        memset(id, 0, sizeof(Node));
        id->kind = NODE_IDENT;
        id->loc = n->loc;
        id->ident.name = tloc->name;
        id->ident.name_len = (size_t)tloc->name_len;
        *pp = id;
        return;
    }
    /* Recurse into children containing expressions */
    switch (n->kind) {
    case NODE_BINARY:
        pre_lower_orelse(ctx, &n->binary.left, line);
        pre_lower_orelse(ctx, &n->binary.right, line);
        break;
    case NODE_UNARY:
        pre_lower_orelse(ctx, &n->unary.operand, line);
        break;
    case NODE_CALL:
        pre_lower_orelse(ctx, &n->call.callee, line);
        for (int i = 0; i < n->call.arg_count; i++)
            pre_lower_orelse(ctx, &n->call.args[i], line);
        break;
    case NODE_FIELD:
        pre_lower_orelse(ctx, &n->field.object, line);
        break;
    case NODE_INDEX:
        pre_lower_orelse(ctx, &n->index_expr.object, line);
        pre_lower_orelse(ctx, &n->index_expr.index, line);
        break;
    case NODE_SLICE:
        pre_lower_orelse(ctx, &n->slice.object, line);
        if (n->slice.start) pre_lower_orelse(ctx, &n->slice.start, line);
        if (n->slice.end) pre_lower_orelse(ctx, &n->slice.end, line);
        break;
    case NODE_TYPECAST:
        pre_lower_orelse(ctx, &n->typecast.expr, line);
        break;
    case NODE_ASSIGN:
        pre_lower_orelse(ctx, &n->assign.target, line);
        pre_lower_orelse(ctx, &n->assign.value, line);
        break;
    case NODE_INTRINSIC:
        for (int i = 0; i < n->intrinsic.arg_count; i++)
            pre_lower_orelse(ctx, &n->intrinsic.args[i], line);
        break;
    case NODE_STRUCT_INIT:
        for (int i = 0; i < n->struct_init.field_count; i++)
            pre_lower_orelse(ctx, &n->struct_init.fields[i].value, line);
        break;
    default:
        break;
    }
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

    /* Emit: tmp = expr.
     * Phase 8d: builtins (pool/slab/ring/arena) emit via IR_ASSIGN passthrough.
     * emit_rewritten_node detects builtins and calls emit_builtin_inline.
     * inner may contain nested orelse (e.g., `(A orelse B) orelse C` — outer's
     * inner is another orelse). pre_lower_orelse decomposes before emission. */
    Node *inner = orelse_node->orelse.expr;
    rewrite_idents(ctx, inner);
    pre_lower_orelse(ctx, &inner, line);
    IRInst assign_tmp = make_inst(IR_ASSIGN, line);
    assign_tmp.dest_local = tmp_id;
    assign_tmp.expr = inner;
    emit_inst(ctx, assign_tmp);

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
    /* Phase E: tag fallback-return/break/continue blocks so zercheck_ir
     * leak detection can skip them. The fallback is only reached when
     * the optional was null — nothing was allocated to leak. */
    if (orelse_node->orelse.fallback_is_return ||
        orelse_node->orelse.fallback_is_break ||
        orelse_node->orelse.fallback_is_continue) {
        ctx->func->blocks[bb_fail].is_orelse_fallback = true;
    }
    if (orelse_node->orelse.fallback_is_return) {
        emit_defer_fire(ctx, line);
        IRInst ret = make_inst(IR_RETURN, line);
        emit_inst(ctx, ret);
    } else if (orelse_node->orelse.fallback_is_break && ctx->loop_exit_block >= 0) {
        /* Fire loop-scoped defers (emit, don't pop — other paths still need them) */
        emit_defer_fire_scoped(ctx, ctx->loop_defer_base, false, line);
        IRInst go = make_inst(IR_GOTO, line);
        go.goto_block = ctx->loop_exit_block;
        emit_inst(ctx, go);
    } else if (orelse_node->orelse.fallback_is_continue && ctx->loop_continue_block >= 0) {
        emit_defer_fire_scoped(ctx, ctx->loop_defer_base, false, line);
        IRInst go = make_inst(IR_GOTO, line);
        go.goto_block = ctx->loop_continue_block;
        emit_inst(ctx, go);
    } else if (orelse_node->orelse.fallback) {
        Node *fb = orelse_node->orelse.fallback;
        /* Value expression fallback — assign to dest_local.
         * Block fallback — lower statements (may terminate via break/return/goto).
         * Nested orelse (e.g., `a() orelse b() orelse 0`) — recurse to lower the inner. */
        if (fb->kind == NODE_ORELSE) {
            lower_orelse_to_dest(ctx, dest_local, fb, line);
        } else if (fb->kind != NODE_BLOCK) {
            /* Value fallback: dest_local = fallback_value;
             * The fallback expression may contain nested orelse (e.g.,
             * `A orelse bar(B orelse 7)`). Pre-lower any orelse inside
             * before handing the AST to the passthrough emitter. */
            if (dest_local >= 0) {
                pre_lower_orelse(ctx, &fb, line);
                IRInst assign = make_inst(IR_ASSIGN, line);
                assign.dest_local = dest_local;
                assign.expr = fb;
                emit_inst(ctx, assign);
            }
        } else {
            lower_stmt(ctx, fb);
        }
        /* Check if fallback terminated; if not, goto join */
        IRBlock *fb_blk = &ctx->func->blocks[ctx->current_block];
        if (fb_blk->inst_count == 0 || !ir_block_is_terminated(fb_blk)) {
            if (bb_join >= 0) {
                IRInst go = make_inst(IR_GOTO, line);
                go.goto_block = bb_join;
                emit_inst(ctx, go);
            }
        } else if (fb_blk->inst_count > 0) {
            /* Phase E: block fallback terminated with RETURN/BREAK/CONTINUE
             * /GOTO. Tag the fail block so zercheck_ir skips leak/join
             * checks — this path is the null-optional exit, allocation
             * didn't succeed. Tag the ENDING block (might be different
             * from bb_fail if the block lowered into multiple blocks). */
            IRInst *fb_last = &fb_blk->insts[fb_blk->inst_count - 1];
            if (fb_last->op == IR_RETURN || fb_last->op == IR_GOTO) {
                ctx->func->blocks[bb_fail].is_orelse_fallback = true;
                /* Also tag the ending block if it's different from bb_fail
                 * (block lowering may have split into sub-blocks) */
                if (ctx->current_block != bb_fail) {
                    ctx->func->blocks[ctx->current_block].is_orelse_fallback = true;
                }
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

    /* === Ok block: unwrap value from tmp (NOT re-emit inner expr — side effects!) ===
     * IR_COPY src=tmp, dst=dest_local: emitter detects src=optional+dst=non-optional
     * and appends .value for unwrap. */
    ctx->current_block = bb_ok;
    if (dest_local >= 0) {
        IRInst unwrap = make_inst(IR_COPY, line);
        unwrap.dest_local = dest_local;
        unwrap.src1_local = tmp_id;
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

    /* ---- Block: lower each statement. BUG-590: track scope for
     * variable shadowing. Inner locals get scope_depth = outer+1 (for
     * dedup decisions) and are created during the block. On exit, mark
     * all locals created within this block as `hidden` so subsequent
     * ir_find_local lookups skip them — after inner `Handle h` goes out
     * of scope, outer references to `h` resolve to the outer local.
     *
     * Note: hidden locals still exist in func->locals and get C
     * declarations at function top — they just aren't findable by name. */
    case NODE_BLOCK: {
        int saved_local_count = ctx->func->local_count;
        int saved_defer = ctx->defer_count;
        /* If the enclosing construct (loop/if/switch arm) already manages
         * defers for this body, skip our own fire — their semantics differ
         * (no-pop for loops so break/continue paths still have defers
         * available; scoped fire for branches). See BUG-590 note. */
        bool managed_by_enclosing = (ctx->block_defers_managed > 0);
        if (managed_by_enclosing) ctx->block_defers_managed--;
        ctx->func->current_scope++;
        for (int i = 0; i < node->block.stmt_count; i++) {
            Node *shared_root;
            emit_shared_lock_if_needed(ctx, node->block.stmts[i], &shared_root);
            lower_stmt(ctx, node->block.stmts[i]);
            emit_shared_unlock_if_needed(ctx, node->block.stmts[i], shared_root);
        }
        /* Fire defers pushed inside THIS block at block exit.
         *
         * Same ordering problem as loops (BUG-544): if we fire+POP here and
         * an early-exit path (e.g., `orelse return` with an earlier block
         * ID) hasn't been emitted yet, its DEFER_FIRE finds the emit-time
         * stack empty → silent miscompile. Solution mirrors the loop POP_ONLY
         * trick: fire with no-pop in the reachable path, then create a
         * bb_post block that's emitted LAST (highest block ID) with POP_ONLY
         * to clear the emit-time stack after all early-exit paths have
         * already read their defers.
         *
         * Skipped when the enclosing construct manages defers itself. */
        if (!managed_by_enclosing && ctx->defer_count > saved_defer) {
            emit_defer_fire_scoped(ctx, saved_defer, false, node->loc.line);
            int bb_post = ir_add_block(ctx->func, ctx->arena);
            ensure_terminated(ctx, bb_post);
            ctx->current_block = bb_post;
            emit_defer_pop_only(ctx, saved_defer, node->loc.line);
            ctx->defer_count = saved_defer;
        }
        ctx->func->current_scope--;
        /* Mark locals declared inside this block as out-of-scope. Skip
         * temps (they're compiler-generated, shouldn't shadow user names).
         * Skip captures (if-unwrap / switch captures are attached to the
         * enclosing construct, not the block; keeping them visible lets
         * the then-body or arm body reference them after block re-entry
         * if needed). */
        for (int li = saved_local_count; li < ctx->func->local_count; li++) {
            if (ctx->func->locals[li].is_temp) continue;
            if (ctx->func->locals[li].is_capture) continue;
            ctx->func->locals[li].hidden = true;
        }
        break;
    }

    /* ---- Variable declaration: assign to local ---- */
    case NODE_VAR_DECL: {
        /* Static locals: declare as `static T name = {0};`. Init only at declaration
         * (C semantics — runs once). Skip lowering init expression — user must use a
         * compile-time constant (enforced by checker). */
        if (node->var_decl.is_static) {
            Type *vt = checker_get_type(ctx->checker, node);
            int sid = ir_add_local(ctx->func, ctx->arena,
                node->var_decl.name, (uint32_t)node->var_decl.name_len,
                vt, false, false, false, node->loc.line);
            if (sid >= 0) ctx->func->locals[sid].is_static = true;
            break;
        }
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
                /* Array→slice coercion: lower_expr can't return a local for
                 * array-typed exprs (C can't assign arrays). Emit IR_ASSIGN
                 * passthrough — emitter's need_slice path handles coercion. */
                Node *init = node->var_decl.init;
                Type *init_type = checker_get_type(ctx->checker, init);
                Type *init_eff = init_type ? type_unwrap_distinct(init_type) : NULL;
                Type *vt_unwrap = vt ? type_unwrap_distinct(vt) : NULL;
                if (init_eff && init_eff->kind == TYPE_ARRAY &&
                    vt_unwrap && vt_unwrap->kind == TYPE_SLICE) {
                    rewrite_idents(ctx, init);
                    pre_lower_orelse(ctx, &init, node->loc.line);
                    IRInst inst = make_inst(IR_ASSIGN, node->loc.line);
                    inst.dest_local = local_id;
                    inst.expr = init;
                    emit_inst(ctx, inst);
                    break;
                }
                /* Unified: lower_expr decomposes all inits.
                 * Simple expressions → local ID + IR_COPY.
                 * Complex expressions (calls, builtins, intrinsics, casts,
                 * orelse, struct_init) → IR_ASSIGN passthrough via lower_expr. */
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

        /* Orelse lowering — ALL orelse patterns, not just terminating ones.
         *
         * emit_rewritten_node has no NODE_ORELSE case (pre-lowered by design).
         * Any NODE_ORELSE that survives to emission becomes an unhandled-node
         * default emission (wrong value). Ensure every orelse at statement
         * level is pre-lowered here.
         *
         * Case A: `target = X orelse Y;` — NODE_ASSIGN wrapping NODE_ORELSE
         *   target is NODE_IDENT   : route orelse result to target's local
         *   target is field/index  : lower to tmp, synth `target = tmp_ident`
         * Case B: `X orelse Y;` — bare orelse expression statement
         *   lower with dest=-1 (void; side effects only, for orelse return etc.)
         * Case C: orelse buried deeper (inside call arg, binary, etc.)
         *   lower_expr(NODE_ORELSE) handles this naturally — no special case.
         *
         * Works uniformly for value fallbacks, block fallbacks, and terminating
         * fallbacks (return/break/continue). Fixes BUG-577 and BUG-577-v2. */
        {
            Node *orelse = find_orelse(expr);
            if (orelse) {
                if (expr->kind == NODE_ASSIGN && expr->assign.op == TOK_EQ &&
                    expr->assign.target && expr->assign.value == orelse) {
                    Node *tgt = expr->assign.target;
                    if (tgt->kind == NODE_IDENT) {
                        int dest_local = ir_find_local(ctx->func,
                            tgt->ident.name,
                            (uint32_t)tgt->ident.name_len);
                        lower_orelse_to_dest(ctx, dest_local, orelse, node->loc.line);
                    } else {
                        /* Non-local target — decompose into tmp + synthesized assign. */
                        Type *rt = checker_get_type(ctx->checker, orelse);
                        if (!rt) {
                            Type *ot = checker_get_type(ctx->checker, orelse->orelse.expr);
                            if (ot) {
                                Type *oe = type_unwrap_distinct(ot);
                                if (oe && oe->kind == TYPE_OPTIONAL) rt = oe->optional.inner;
                            }
                            if (!rt) rt = ty_i32;
                        }
                        int tmp = create_temp(ctx, rt, node->loc.line);
                        lower_orelse_to_dest(ctx, tmp, orelse, node->loc.line);
                        IRLocal *tloc = &ctx->func->locals[tmp];
                        Node *tmp_id = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(tmp_id, 0, sizeof(Node));
                        tmp_id->kind = NODE_IDENT;
                        tmp_id->loc = node->loc;
                        tmp_id->ident.name = tloc->name;
                        tmp_id->ident.name_len = (size_t)tloc->name_len;
                        Node *new_assign = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(new_assign, 0, sizeof(Node));
                        new_assign->kind = NODE_ASSIGN;
                        new_assign->loc = node->loc;
                        new_assign->assign.op = TOK_EQ;
                        new_assign->assign.target = tgt;
                        new_assign->assign.value = tmp_id;
                        /* Target may contain orelse in an index/field path —
                         * pre-lower before emission. */
                        pre_lower_orelse(ctx, &new_assign->assign.target, node->loc.line);
                        IRInst inst = make_inst(IR_ASSIGN, node->loc.line);
                        inst.expr = new_assign;
                        emit_inst(ctx, inst);
                    }
                    break;
                }
                if (expr == orelse) {
                    /* Bare orelse expression statement — side effects only. */
                    lower_orelse_to_dest(ctx, -1, orelse, node->loc.line);
                    break;
                }
                /* Otherwise: orelse buried in a sub-expression (NODE_CALL arg,
                 * NODE_BINARY operand, etc.). lower_expr decomposes those via
                 * its NODE_ORELSE case — no pre-lowering needed here. */
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
        /* Save defer count so if-scoped defers fire at block exit */
        int then_defer_base = ctx->defer_count;
        ctx->block_defers_managed++;  /* if-body block: we manage */
        int then_start_bi = bb_then;
        int then_start_block_count = ctx->func->block_count;
        lower_stmt(ctx, node->if_stmt.then_body);
        emit_defer_fire_scoped(ctx, then_defer_base, true, node->loc.line);
        ctx->defer_count = then_defer_base;

        /* Phase E: detect if the then-body "always exits" — its final
         * block ends with RETURN/BREAK/CONTINUE (not a normal fall-
         * through to bb_join). If so, tag all blocks in the then-body
         * range [then_start_bi, ctx->current_block] as is_early_exit.
         * This mirrors zercheck.c's block_always_exits semantic: these
         * blocks shouldn't provide leak-coverage to non-early-exit
         * returns — they're conditional bypass paths that don't
         * contribute to the canonical function-exit state.
         *
         * Check BEFORE ensure_terminated (which would add GOTO to
         * bb_join for non-terminated blocks).
         *
         * Side note: we don't rely on was_terminated because the
         * then-body may have created intermediate blocks via nested
         * control flow; only the current block's terminator matters. */
        bool then_always_exits = false;
        {
            IRBlock *cb = &ctx->func->blocks[ctx->current_block];
            if (cb->inst_count > 0) {
                IRInst *last = &cb->insts[cb->inst_count - 1];
                /* RETURN always exits. GOTO to bb_join is NOT an exit
                 * (that's normal fall-through). GOTO to loop_exit/
                 * loop_continue/label is an exit. BREAK/CONTINUE via
                 * GOTO have goto_block != bb_join. */
                if (last->op == IR_RETURN) {
                    then_always_exits = true;
                } else if (last->op == IR_GOTO &&
                           last->goto_block != bb_join) {
                    then_always_exits = true;
                }
            }
        }
        /* Only tag if-without-capture (regular if). If-unwrap (with
         * capture) has special alias semantics: when the capture is
         * freed in the early-exit body, the original allocation's
         * alloc_id should be considered "escaped" on the fall-through
         * path too. Excluding the early-exit from coverage would
         * incorrectly flag such patterns as leaks. Union coverage
         * correctly handles if-unwrap early-exits via alias
         * propagation to the canonical return's state. */
        if (then_always_exits && !has_capture) {
            /* Tag blocks created during then-body lowering plus bb_then.
             * Skip bb_join — that's the canonical fall-through target. */
            for (int bi = then_start_bi; bi < ctx->func->block_count; bi++) {
                if (bi == bb_join) continue;
                if (!ctx->func->blocks[bi].is_early_exit)
                    ctx->func->blocks[bi].is_early_exit = true;
            }
            (void)then_start_block_count;
        }

        ensure_terminated(ctx, bb_join);

        /* Else block */
        int else_start_bi = -1;
        if (bb_else >= 0) {
            ctx->current_block = bb_else;
            else_start_bi = bb_else;
            int else_defer_base = ctx->defer_count;
            ctx->block_defers_managed++;  /* else-body block: we manage */
            lower_stmt(ctx, node->if_stmt.else_body);
            emit_defer_fire_scoped(ctx, else_defer_base, true, node->loc.line);
            ctx->defer_count = else_defer_base;

            /* Phase E: same tagging for else-body if it always exits. */
            bool else_always_exits = false;
            IRBlock *cb = &ctx->func->blocks[ctx->current_block];
            if (cb->inst_count > 0) {
                IRInst *last = &cb->insts[cb->inst_count - 1];
                if (last->op == IR_RETURN) {
                    else_always_exits = true;
                } else if (last->op == IR_GOTO &&
                           last->goto_block != bb_join) {
                    else_always_exits = true;
                }
            }
            if (else_always_exits) {
                for (int bi = else_start_bi; bi < ctx->func->block_count; bi++) {
                    /* Don't tag bb_join if we accidentally reach it */
                    if (bi == bb_join) continue;
                    if (!ctx->func->blocks[bi].is_early_exit)
                        ctx->func->blocks[bi].is_early_exit = true;
                }
            }

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
        ctx->block_defers_managed++;  /* for-body block: we manage (no-pop semantics) */
        lower_stmt(ctx, node->for_stmt.body);
        /* End of body: emit defer bodies inline (for normal iteration path),
         * but DO NOT pop. Divergent paths (break/continue/orelse-continue)
         * also emit their own defer bodies with no-pop.
         * Pop happens once in the POST block — created AFTER all body blocks
         * so it's emitted LAST. */
        emit_defer_fire_scoped(ctx, ctx->loop_defer_base, false, node->loc.line);
        ensure_terminated(ctx, bb_step);

        /* Step block */
        ctx->current_block = bb_step;
        if (node->for_stmt.step) {
            rewrite_idents(ctx, node->for_stmt.step);
            /* Step may contain orelse: `for (..; ..; x = next() orelse 0)` */
            pre_lower_orelse(ctx, &node->for_stmt.step, node->loc.line);
            IRInst step = make_inst(IR_ASSIGN, node->loc.line);
            step.expr = node->for_stmt.step;
            emit_inst(ctx, step);
        }
        ensure_terminated(ctx, bb_cond); /* back edge */

        /* Create post-exit block AFTER body (including orelse-created blocks).
         * Block IDs are monotonic — this block is emitted LAST of loop blocks.
         * POP_ONLY here ensures all body/continue/break fire sites already
         * emitted their defer bodies before we clear the compile-time stack. */
        int bb_post = -1;
        if (ctx->defer_count > ctx->loop_defer_base) {
            bb_post = ir_add_block(ctx->func, ctx->arena);
        }
        ctx->current_block = bb_exit;
        if (bb_post >= 0) {
            IRInst go = make_inst(IR_GOTO, node->loc.line);
            go.goto_block = bb_post;
            emit_inst(ctx, go);
            ctx->current_block = bb_post;
            emit_defer_pop_only(ctx, ctx->loop_defer_base, node->loc.line);
        }
        ctx->defer_count = ctx->loop_defer_base;

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
        ctx->block_defers_managed++;  /* while-body block: we manage */
        lower_stmt(ctx, node->while_stmt.body);
        emit_defer_fire_scoped(ctx, ctx->loop_defer_base, false, node->loc.line);
        ensure_terminated(ctx, bb_cond);

        /* Post-exit block for POP_ONLY — created AFTER all body blocks */
        {
            int bb_post = -1;
            if (ctx->defer_count > ctx->loop_defer_base) {
                bb_post = ir_add_block(ctx->func, ctx->arena);
            }
            ctx->current_block = bb_exit;
            if (bb_post >= 0) {
                IRInst go = make_inst(IR_GOTO, node->loc.line);
                go.goto_block = bb_post;
                emit_inst(ctx, go);
                ctx->current_block = bb_post;
                emit_defer_pop_only(ctx, ctx->loop_defer_base, node->loc.line);
            }
            ctx->defer_count = ctx->loop_defer_base;
        }

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
        ctx->block_defers_managed++;  /* do-while body: we manage */
        lower_stmt(ctx, node->while_stmt.body);
        emit_defer_fire_scoped(ctx, ctx->loop_defer_base, false, node->loc.line);
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

        /* Post-exit block for POP_ONLY */
        {
            int bb_post = -1;
            if (ctx->defer_count > ctx->loop_defer_base) {
                bb_post = ir_add_block(ctx->func, ctx->arena);
            }
            ctx->current_block = bb_exit;
            if (bb_post >= 0) {
                IRInst go = make_inst(IR_GOTO, node->loc.line);
                go.goto_block = bb_post;
                emit_inst(ctx, go);
                ctx->current_block = bb_post;
                emit_defer_pop_only(ctx, ctx->loop_defer_base, node->loc.line);
            }
            ctx->defer_count = ctx->loop_defer_base;
        }

        ctx->loop_exit_block = saved_exit;
        ctx->loop_continue_block = saved_cont;
        ctx->loop_defer_base = saved_defer;
        break;
    }

    /* ---- Switch: chain of branches ---- */
    case NODE_SWITCH: {
        /* Full IR lowering for ALL switch types (integer, enum, union, optional).
         * BUG-579: Previously enum/union/optional went through IR_NOP passthrough
         * where a mini-emit_stmt in the emitter handled arm bodies — that path
         * silently dropped NODE_FOR/NODE_WHILE/nested NODE_SWITCH/NODE_ORELSE
         * and NODE_BREAK/NODE_CONTINUE (target was wrong).
         *
         * Strategy: hoist switch expression into a local (pointer for unions,
         * value for others), build per-arm comparison AST referencing that local,
         * lower via IR_BRANCH, and lower arm bodies via lower_stmt (which handles
         * every statement kind correctly). */
        Type *sw_type = checker_get_type(ctx->checker, node->switch_stmt.expr);
        Type *sw_eff = sw_type ? type_unwrap_distinct(sw_type) : NULL;
        bool is_enum = sw_eff && sw_eff->kind == TYPE_ENUM;
        bool is_union = sw_eff && sw_eff->kind == TYPE_UNION;
        bool is_optional = sw_eff && sw_eff->kind == TYPE_OPTIONAL;
        /* Null-sentinel optional: ?*T or ?FuncPtr. Inner unwrap distinct. */
        bool is_null_sent_opt = false;
        if (is_optional && sw_eff->optional.inner) {
            Type *inner_eff = type_unwrap_distinct(sw_eff->optional.inner);
            is_null_sent_opt = inner_eff && (inner_eff->kind == TYPE_POINTER ||
                                             inner_eff->kind == TYPE_FUNC_PTR);
        }
        bool is_void_opt = is_optional && sw_eff->optional.inner &&
                           type_unwrap_distinct(sw_eff->optional.inner)->kind == TYPE_VOID;

        rewrite_idents(ctx, node->switch_stmt.expr);

        /* Hoist switch expression.
         *   sw_ref = AST reference used in comparisons/captures.
         *   For unions: sw_ref is NODE_IDENT to a pointer local (→ &sw_expr),
         *   so NODE_FIELD(sw_ref, name) emits as sw_ref->name and mutable
         *   captures &sw_ref->variant persist to the original.
         *   For everything else: sw_ref is NODE_IDENT to a value local. */
        Node *sw_ref = NULL;
        if (is_union) {
            Node *sw_expr = node->switch_stmt.expr;
            bool is_lvalue = (sw_expr->kind == NODE_IDENT ||
                              sw_expr->kind == NODE_FIELD ||
                              sw_expr->kind == NODE_INDEX ||
                              (sw_expr->kind == NODE_UNARY && sw_expr->unary.op == TOK_STAR));
            Node *addr_expr;
            if (is_lvalue) {
                /* Build &sw_expr — TOK_AMP passthrough in lower_expr preserves lvalue */
                addr_expr = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                memset(addr_expr, 0, sizeof(Node));
                addr_expr->kind = NODE_UNARY;
                addr_expr->loc = node->loc;
                addr_expr->unary.op = TOK_AMP;
                addr_expr->unary.operand = sw_expr;
            } else {
                /* Rvalue (call, binary, etc.): copy into tmp first, then &tmp */
                int val_local = lower_expr(ctx, sw_expr);
                if (val_local < 0) break; /* void/array — shouldn't happen for union */
                IRLocal *vl = &ctx->func->locals[val_local];
                Node *ident = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                memset(ident, 0, sizeof(Node));
                ident->kind = NODE_IDENT;
                ident->loc = node->loc;
                ident->ident.name = vl->name;
                ident->ident.name_len = (size_t)vl->name_len;
                /* Set type on synthesized ident so lower_expr and type inference
                 * see the correct union type (not ty_i32 fallback). */
                checker_set_type(ctx->checker, ident, sw_type);
                addr_expr = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                memset(addr_expr, 0, sizeof(Node));
                addr_expr->kind = NODE_UNARY;
                addr_expr->loc = node->loc;
                addr_expr->unary.op = TOK_AMP;
                addr_expr->unary.operand = ident;
            }
            /* Annotate addr_expr with pointer-to-union type so lower_expr
             * creates a properly-typed temp (not ty_i32 fallback). */
            Type *ptr_to_union = type_pointer(ctx->arena, sw_type);
            checker_set_type(ctx->checker, addr_expr, ptr_to_union);
            int ptr_local = lower_expr(ctx, addr_expr);
            if (ptr_local < 0) break;
            IRLocal *pl = &ctx->func->locals[ptr_local];
            sw_ref = (Node *)arena_alloc(ctx->arena, sizeof(Node));
            memset(sw_ref, 0, sizeof(Node));
            sw_ref->kind = NODE_IDENT;
            sw_ref->loc = node->loc;
            sw_ref->ident.name = pl->name;
            sw_ref->ident.name_len = (size_t)pl->name_len;
        } else {
            int val_local = lower_expr(ctx, node->switch_stmt.expr);
            if (val_local < 0) break;
            IRLocal *vl = &ctx->func->locals[val_local];
            sw_ref = (Node *)arena_alloc(ctx->arena, sizeof(Node));
            memset(sw_ref, 0, sizeof(Node));
            sw_ref->kind = NODE_IDENT;
            sw_ref->loc = node->loc;
            sw_ref->ident.name = vl->name;
            sw_ref->ident.name_len = (size_t)vl->name_len;
        }

        int bb_exit = ir_add_block(ctx->func, ctx->arena);

        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            SwitchArm *arm = &node->switch_stmt.arms[i];
            int bb_arm = ir_add_block(ctx->func, ctx->arena);
            int bb_next = (i + 1 < node->switch_stmt.arm_count) ?
                          ir_add_block(ctx->func, ctx->arena) : bb_exit;

            if (arm->is_default) {
                ensure_terminated(ctx, bb_arm);
            } else {
                /* Build per-arm comparison: OR of equality checks */
                Node *cmp = NULL;
                for (int vi = 0; vi < arm->value_count; vi++) {
                    Node *eq;
                    if (is_enum) {
                        /* Extract variant name: arm values can be
                         *  - NODE_IDENT("west") for `.west` dot syntax
                         *  - NODE_FIELD(Dir, "west") for `Dir.west` qualified syntax */
                        const char *vname = NULL;
                        size_t vlen = 0;
                        if (arm->values[vi]->kind == NODE_IDENT) {
                            vname = arm->values[vi]->ident.name;
                            vlen = arm->values[vi]->ident.name_len;
                        } else if (arm->values[vi]->kind == NODE_FIELD) {
                            vname = arm->values[vi]->field.field_name;
                            vlen = arm->values[vi]->field.field_name_len;
                        }
                        /* Look up variant numeric value */
                        int64_t variant_value = 0;
                        if (vname) {
                            for (uint32_t ei = 0; ei < sw_eff->enum_type.variant_count; ei++) {
                                if (sw_eff->enum_type.variants[ei].name_len == vlen &&
                                    memcmp(sw_eff->enum_type.variants[ei].name, vname, vlen) == 0) {
                                    variant_value = (int64_t)sw_eff->enum_type.variants[ei].value;
                                    break;
                                }
                            }
                        }
                        Node *lit = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(lit, 0, sizeof(Node));
                        lit->kind = NODE_INT_LIT;
                        lit->loc = node->loc;
                        lit->int_lit.value = (uint64_t)variant_value;
                        eq = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(eq, 0, sizeof(Node));
                        eq->kind = NODE_BINARY;
                        eq->loc = node->loc;
                        eq->binary.op = TOK_EQEQ;
                        eq->binary.left = sw_ref;
                        eq->binary.right = lit;
                    } else if (is_union) {
                        /* Extract variant name (either NODE_IDENT or NODE_FIELD) */
                        const char *vname = NULL;
                        size_t vlen = 0;
                        if (arm->values[vi]->kind == NODE_IDENT) {
                            vname = arm->values[vi]->ident.name;
                            vlen = arm->values[vi]->ident.name_len;
                        } else if (arm->values[vi]->kind == NODE_FIELD) {
                            vname = arm->values[vi]->field.field_name;
                            vlen = arm->values[vi]->field.field_name_len;
                        }
                        /* Compare sw_ref->_tag == variant_index */
                        int variant_index = 0;
                        if (vname) {
                            for (uint32_t ui = 0; ui < sw_eff->union_type.variant_count; ui++) {
                                if (sw_eff->union_type.variants[ui].name_len == vlen &&
                                    memcmp(sw_eff->union_type.variants[ui].name, vname, vlen) == 0) {
                                    variant_index = (int)ui;
                                    break;
                                }
                            }
                        }
                        Node *tag_field = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(tag_field, 0, sizeof(Node));
                        tag_field->kind = NODE_FIELD;
                        tag_field->loc = node->loc;
                        tag_field->field.object = sw_ref;
                        tag_field->field.field_name = "_tag";
                        tag_field->field.field_name_len = 4;
                        Node *lit = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(lit, 0, sizeof(Node));
                        lit->kind = NODE_INT_LIT;
                        lit->loc = node->loc;
                        lit->int_lit.value = (uint64_t)variant_index;
                        eq = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(eq, 0, sizeof(Node));
                        eq->kind = NODE_BINARY;
                        eq->loc = node->loc;
                        eq->binary.op = TOK_EQEQ;
                        eq->binary.left = tag_field;
                        eq->binary.right = lit;
                    } else if (is_optional) {
                        bool is_null = (arm->values[vi]->kind == NODE_NULL_LIT);
                        if (is_null_sent_opt) {
                            /* Raw pointer (?*T) — null arm = !ptr, non-null = ptr.
                             * Only null/non-null matching is meaningful here; arm
                             * values are all NULL or pointer-valued (the user
                             * doesn't write literal pointer addresses as arm
                             * values). */
                            if (is_null) {
                                eq = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                                memset(eq, 0, sizeof(Node));
                                eq->kind = NODE_UNARY;
                                eq->loc = node->loc;
                                eq->unary.op = TOK_BANG;
                                eq->unary.operand = sw_ref;
                            } else {
                                eq = sw_ref;
                            }
                        } else {
                            /* Struct optional — null arm: !has_value.
                             * Non-null arm: has_value AND value == arm_value.
                             * Value comparison matters for `?u32` with `5 => ...`
                             * and `?Color` with `.red => ...`. */
                            Node *hv_field = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                            memset(hv_field, 0, sizeof(Node));
                            hv_field->kind = NODE_FIELD;
                            hv_field->loc = node->loc;
                            hv_field->field.object = sw_ref;
                            hv_field->field.field_name = "has_value";
                            hv_field->field.field_name_len = 9;
                            if (is_null) {
                                eq = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                                memset(eq, 0, sizeof(Node));
                                eq->kind = NODE_UNARY;
                                eq->loc = node->loc;
                                eq->unary.op = TOK_BANG;
                                eq->unary.operand = hv_field;
                            } else {
                                /* Build: has_value && (value == arm_value).
                                 * For enum inner type, resolve arm value to
                                 * variant.value literal. */
                                Node *val_field = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                                memset(val_field, 0, sizeof(Node));
                                val_field->kind = NODE_FIELD;
                                val_field->loc = node->loc;
                                val_field->field.object = sw_ref;
                                val_field->field.field_name = "value";
                                val_field->field.field_name_len = 5;

                                /* Determine RHS of value comparison */
                                Node *rhs = arm->values[vi];
                                Type *inner_eff = type_unwrap_distinct(sw_eff->optional.inner);
                                if (inner_eff && inner_eff->kind == TYPE_ENUM) {
                                    /* Resolve variant name → numeric value */
                                    const char *vn = NULL;
                                    size_t vl = 0;
                                    if (rhs->kind == NODE_IDENT) {
                                        vn = rhs->ident.name;
                                        vl = rhs->ident.name_len;
                                    } else if (rhs->kind == NODE_FIELD) {
                                        vn = rhs->field.field_name;
                                        vl = rhs->field.field_name_len;
                                    }
                                    int64_t vv = 0;
                                    if (vn) {
                                        for (uint32_t ei = 0; ei < inner_eff->enum_type.variant_count; ei++) {
                                            if (inner_eff->enum_type.variants[ei].name_len == vl &&
                                                memcmp(inner_eff->enum_type.variants[ei].name, vn, vl) == 0) {
                                                vv = (int64_t)inner_eff->enum_type.variants[ei].value;
                                                break;
                                            }
                                        }
                                    }
                                    Node *lit = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                                    memset(lit, 0, sizeof(Node));
                                    lit->kind = NODE_INT_LIT;
                                    lit->loc = node->loc;
                                    lit->int_lit.value = (uint64_t)vv;
                                    rhs = lit;
                                }

                                Node *val_eq = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                                memset(val_eq, 0, sizeof(Node));
                                val_eq->kind = NODE_BINARY;
                                val_eq->loc = node->loc;
                                val_eq->binary.op = TOK_EQEQ;
                                val_eq->binary.left = val_field;
                                val_eq->binary.right = rhs;

                                Node *and_node = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                                memset(and_node, 0, sizeof(Node));
                                and_node->kind = NODE_BINARY;
                                and_node->loc = node->loc;
                                and_node->binary.op = TOK_AMPAMP;
                                and_node->binary.left = hv_field;
                                and_node->binary.right = val_eq;
                                eq = and_node;
                            }
                        }
                    } else {
                        /* Integer / other — direct equality */
                        eq = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(eq, 0, sizeof(Node));
                        eq->kind = NODE_BINARY;
                        eq->loc = node->loc;
                        eq->binary.op = TOK_EQEQ;
                        eq->binary.left = sw_ref;
                        eq->binary.right = arm->values[vi];
                    }

                    if (!cmp) {
                        cmp = eq;
                    } else {
                        Node *or_node = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                        memset(or_node, 0, sizeof(Node));
                        or_node->kind = NODE_BINARY;
                        or_node->loc = node->loc;
                        or_node->binary.op = TOK_PIPEPIPE;
                        or_node->binary.left = cmp;
                        or_node->binary.right = eq;
                        cmp = or_node;
                    }
                }
                IRInst br = make_inst(IR_BRANCH, node->loc.line);
                br.cond_local = lower_expr(ctx, cmp);
                br.true_block = bb_arm;
                br.false_block = bb_next;
                emit_inst(ctx, br);
            }

            /* Arm body */
            ctx->current_block = bb_arm;

            /* Capture */
            if (arm->capture_name && !is_void_opt) {
                /* Determine capture type and source expression */
                Type *cap_type = NULL;
                Node *cap_src = NULL;
                bool cap_is_ptr = arm->capture_is_ptr;

                if (is_enum) {
                    cap_type = sw_eff;
                    cap_src = sw_ref;
                } else if (is_optional) {
                    Type *inner = sw_eff->optional.inner;
                    if (cap_is_ptr) {
                        cap_type = type_pointer(ctx->arena, inner);
                    } else {
                        cap_type = inner;
                    }
                    if (is_null_sent_opt) {
                        /* ?*T : sw_ref already pointer-valued */
                        cap_src = sw_ref;
                    } else {
                        /* Struct optional: sw_ref.value for |v|,
                         * &sw_ref.value for |*v| — BUG-552 IR_COPY handles */
                        cap_src = sw_ref;
                    }
                } else if (is_union) {
                    /* Find variant type by name (arm->values[0]) — supports both
                     * NODE_IDENT and NODE_FIELD (.variant vs Union.variant) */
                    const char *vname = NULL;
                    size_t vlen = 0;
                    if (arm->value_count > 0) {
                        if (arm->values[0]->kind == NODE_IDENT) {
                            vname = arm->values[0]->ident.name;
                            vlen = arm->values[0]->ident.name_len;
                        } else if (arm->values[0]->kind == NODE_FIELD) {
                            vname = arm->values[0]->field.field_name;
                            vlen = arm->values[0]->field.field_name_len;
                        }
                    }
                    if (vname) {
                        for (uint32_t ui = 0; ui < sw_eff->union_type.variant_count; ui++) {
                            if (sw_eff->union_type.variants[ui].name_len == vlen &&
                                memcmp(sw_eff->union_type.variants[ui].name, vname, vlen) == 0) {
                                Type *vt = sw_eff->union_type.variants[ui].type;
                                cap_type = cap_is_ptr ? type_pointer(ctx->arena, vt) : vt;
                                /* sw_ref->variant_name via NODE_FIELD. Annotate
                                 * with variant type so IR_ASSIGN emitter can
                                 * detect array-to-array and emit memcpy. */
                                Node *field = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                                memset(field, 0, sizeof(Node));
                                field->kind = NODE_FIELD;
                                field->loc = node->loc;
                                field->field.object = sw_ref;
                                field->field.field_name = vname;
                                field->field.field_name_len = vlen;
                                checker_set_type(ctx->checker, field, vt);
                                if (cap_is_ptr) {
                                    /* &sw_ref->variant */
                                    Node *addr = (Node *)arena_alloc(ctx->arena, sizeof(Node));
                                    memset(addr, 0, sizeof(Node));
                                    addr->kind = NODE_UNARY;
                                    addr->loc = node->loc;
                                    addr->unary.op = TOK_AMP;
                                    addr->unary.operand = field;
                                    checker_set_type(ctx->checker, addr, cap_type);
                                    cap_src = addr;
                                } else {
                                    cap_src = field;
                                }
                                break;
                            }
                        }
                    }
                }

                if (cap_type && cap_src) {
                    /* Build a UNIQUE capture name per arm so this capture doesn't
                     * shadow same-named captures in other switches. The IR has a
                     * flat local namespace (no scopes), so two arms both using |v|
                     * across different switches would both map to the LAST "v"
                     * local via ir_find_local — wrong. Give each arm's capture a
                     * unique suffix (e.g., "v_sw3_arm0") and rewrite references
                     * to the source name in THIS arm's body before lowering it. */
                    char uniq_buf[96];
                    int ulen = snprintf(uniq_buf, sizeof(uniq_buf),
                        "%.*s_cap%d",
                        (int)arm->capture_name_len, arm->capture_name,
                        ctx->temp_count++);
                    if (ulen < 0) ulen = 0;
                    if (ulen >= (int)sizeof(uniq_buf)) ulen = (int)sizeof(uniq_buf) - 1;
                    char *uniq_name = (char *)arena_alloc(ctx->arena, ulen + 1);
                    memcpy(uniq_name, uniq_buf, ulen);
                    uniq_name[ulen] = '\0';

                    int cap_id = ir_add_local(ctx->func, ctx->arena,
                        uniq_name, (uint32_t)ulen,
                        cap_type, false, true, false, node->loc.line);

                    /* Rewrite references to the source capture name in arm body */
                    if (cap_id >= 0 && arm->body) {
                        rewrite_capture_name(arm->body,
                            arm->capture_name, (uint32_t)arm->capture_name_len,
                            uniq_name, (uint32_t)ulen);
                    }

                    if (cap_id >= 0) {
                        /* For optional value captures: IR_COPY with type adaptation
                         * handles .value unwrap (src ?T → dst T). For |*v| optional:
                         * IR_COPY with dst=*T + src=?T emits &src.value (BUG-552).
                         * For union/enum: emit normal IR_ASSIGN or IR_COPY. */
                        if (is_optional) {
                            /* IR_COPY src=sw_ref_local, dest=cap — type adaptation */
                            int src_local = ir_find_local(ctx->func,
                                sw_ref->ident.name, (uint32_t)sw_ref->ident.name_len);
                            if (src_local >= 0) {
                                IRInst cop = make_inst(IR_COPY, node->loc.line);
                                cop.dest_local = cap_id;
                                cop.src1_local = src_local;
                                emit_inst(ctx, cop);
                            } else {
                                /* Fallback: passthrough IR_ASSIGN with cap_src */
                                IRInst cap = make_inst(IR_ASSIGN, node->loc.line);
                                cap.dest_local = cap_id;
                                cap.expr = cap_src;
                                emit_inst(ctx, cap);
                            }
                        } else {
                            /* Enum/union: emit the cap_src AST directly */
                            IRInst cap = make_inst(IR_ASSIGN, node->loc.line);
                            cap.dest_local = cap_id;
                            cap.expr = cap_src;
                            emit_inst(ctx, cap);
                        }
                    }
                }
            }

            int arm_defer_base = ctx->defer_count;
            ctx->block_defers_managed++;  /* switch arm body: we manage */
            lower_stmt(ctx, arm->body);
            emit_defer_fire_scoped(ctx, arm_defer_base, true, node->loc.line);
            ctx->defer_count = arm_defer_base;
            ensure_terminated(ctx, bb_exit);

            if (!arm->is_default) {
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
            /* Keep expr for cases lower_expr returns -1 (void/array passthrough).
             * Emitter's IR_RETURN checks expr for array→slice coercion. */
            if (ret.src1_local < 0) ret.expr = ret_expr;
        }
        emit_defer_fire(ctx, node->loc.line);
        emit_inst(ctx, ret);
        break;
    }

    /* ---- Break ---- */
    case NODE_BREAK: {
        if (ctx->loop_exit_block >= 0) {
            /* Fire loop-scoped defers (emit bodies, DO NOT pop — other paths may need them) */
            emit_defer_fire_scoped(ctx, ctx->loop_defer_base, false, node->loc.line);
            IRInst go = make_inst(IR_GOTO, node->loc.line);
            go.goto_block = ctx->loop_exit_block;
            emit_inst(ctx, go);
        }
        break;
    }

    /* ---- Continue ---- */
    case NODE_CONTINUE: {
        if (ctx->loop_continue_block >= 0) {
            emit_defer_fire_scoped(ctx, ctx->loop_defer_base, false, node->loc.line);
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
        /* Rewrite idents in the defer body so references to scope-
         * shadowed names (`h` → `h_17`) resolve correctly when the
         * defer body is consumed later (emitter or zercheck_ir). */
        rewrite_defer_body_idents(ctx, node->defer.body);
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
        /* BUG-591: await condition must be RE-EVALUATED on every poll.
         * Previously we lowered the cond to a local in the current block,
         * then emitted IR_AWAIT which placed `case N:;` AFTER the eval.
         * On resume (state=N), the switch jumped past the eval so the
         * stale cond_local value was used.
         *
         * Fix: store the cond AST on IR_AWAIT and let the emitter emit
         * `case N:;` followed by a FRESH evaluation of the cond expression
         * via emit_rewritten_node. The cond value is computed inline in
         * the generated C — no IR local, so each poll re-reads globals /
         * fields and correctly re-evaluates. */
        int resume_bb = ir_add_block(ctx->func, ctx->arena);
        IRInst aw = make_inst(IR_AWAIT, node->loc.line);
        rewrite_idents(ctx, node->await_stmt.cond);
        aw.cond_local = -1;                     /* don't use pre-computed local */
        aw.expr = node->await_stmt.cond;        /* emitter re-evaluates fresh */
        aw.goto_block = resume_bb;
        emit_inst(ctx, aw);
        ctx->current_block = resume_bb;
        break;
    }

    /* ---- Spawn ---- */
    case NODE_SPAWN: {
        /* Spawn uses complex wrapper structs + pthread_create.
         * Pass through as AST node for emit_stmt to handle.
         *
         * NOTE: For scoped spawn (ThreadHandle th = spawn ...), no IR local
         * is created — the emitter's NODE_SPAWN passthrough emits the
         * pthread_t declaration directly. zercheck_ir tracks ThreadHandle
         * join status by name via IR_NOP(NODE_SPAWN) handler, not by IR
         * local id. */
        IRInst sp = make_inst(IR_NOP, node->loc.line);
        sp.expr = node; /* emit_stmt handles NODE_SPAWN */
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
        /* Lower as: branch on atomic flag → body or skip.
         * The IR_BRANCH emitter detects expr=NODE_ONCE and emits a CAS
         * exchange gate instead of a plain condition. */
        int bb_body = ir_add_block(ctx->func, ctx->arena);
        int bb_skip = ir_add_block(ctx->func, ctx->arena);
        IRInst br = make_inst(IR_BRANCH, node->loc.line);
        br.expr = node; /* NODE_ONCE marker for emitter's CAS pattern */
        br.cond_local = -1;
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
    ctx.labels = ctx.label_inline;
    ctx.label_capacity = (int)(sizeof(ctx.label_inline) / sizeof(ctx.label_inline[0]));

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

    /* Phase 2: Create entry block FIRST so it's block 0.
     * BUG-588: previously collect_labels ran before start_block, so if the
     * function body had a label (e.g., goto target), the label's block got
     * ID 0 and the entry block got a higher ID. The emitter emits blocks
     * in ID order, so C execution would start at the LABEL's code instead
     * of the entry — reading uninitialized state. Masked by --run exit
     * code bug (BUG-581); surfaced by that fix as runtime SIGTRAP. */
    start_block(&ctx);

    /* Phase 3: Pre-scan for labels (now get IDs >= 1) and lower body */
    collect_labels(&ctx, func_decl->func_decl.body);
    lower_stmt(&ctx, func_decl->func_decl.body);

    /* Ensure CURRENT block is terminated (not last block — yield may
     * create blocks after the current one, making last != current).
     * Fire pending defers before the implicit return (same as explicit NODE_RETURN). */
    IRBlock *cur = &func->blocks[ctx.current_block];
    if (cur->inst_count == 0 || !ir_block_is_terminated(cur)) {
        emit_defer_fire(&ctx, func_decl->loc.line);
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
    ctx.labels = ctx.label_inline;
    ctx.label_capacity = (int)(sizeof(ctx.label_inline) / sizeof(ctx.label_inline[0]));

    /* Locals created on-demand during lower_stmt. Entry block MUST be first
     * (see BUG-588 note above for the function variant). */
    start_block(&ctx);
    collect_labels(&ctx, interrupt->interrupt.body);
    lower_stmt(&ctx, interrupt->interrupt.body);

    IRBlock *cur = &func->blocks[ctx.current_block];
    if (cur->inst_count == 0 || !ir_block_is_terminated(cur)) {
        IRInst ret = make_inst(IR_RETURN, interrupt->loc.line);
        emit_inst(&ctx, ret);
    }

    ir_compute_preds(func, ctx.arena);
    return func;
}
