#include "emitter.h"
#include "ir.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ================================================================
 * ZER C Emitter — walks typed AST, outputs valid C99
 *
 * Strategy: recursive AST walk. Each emit function handles one
 * node kind, prints C code to the output file.
 * ================================================================ */

/* ---- Helpers ---- */

static void emit_indent(Emitter *e) {
    for (int i = 0; i < e->indent; i++) fprintf(e->out, "    ");
}

static void emit(Emitter *e, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(e->out, fmt, args);
    va_end(args);
}

/* emit a user-defined type name with optional module prefix for namespace mangling.
 * If prefix is set: emits "prefix_name". If NULL: emits "name". */
static void emit_user_name(Emitter *e, const char *prefix, uint32_t prefix_len,
                           const char *name, uint32_t name_len) {
    if (prefix && prefix_len > 0) {
        /* BUG-332: double underscore separator prevents name collisions */
        fprintf(e->out, "%.*s__%.*s", (int)prefix_len, prefix, (int)name_len, name);
    } else {
        fprintf(e->out, "%.*s", (int)name_len, name);
    }
}

/* convenience: emit struct/union/enum name from Type with module prefix */
#define EMIT_STRUCT_NAME(e, t) emit_user_name(e, (t)->struct_type.module_prefix, (t)->struct_type.module_prefix_len, (t)->struct_type.name, (t)->struct_type.name_len)
#define EMIT_UNION_NAME(e, t)  emit_user_name(e, (t)->union_type.module_prefix, (t)->union_type.module_prefix_len, (t)->union_type.name, (t)->union_type.name_len)
#define EMIT_ENUM_NAME(e, t)   emit_user_name(e, (t)->enum_type.module_prefix, (t)->enum_type.module_prefix_len, (t)->enum_type.name, (t)->enum_type.name_len)

/* BUG-218: emit function/global var name with module prefix */
#define EMIT_MANGLED_NAME(e, name, name_len) do { \
    if ((e)->current_module) { \
        fprintf((e)->out, "%.*s__%.*s", (int)(e)->current_module_len, (e)->current_module, (int)(name_len), (name)); \
    } else { \
        fprintf((e)->out, "%.*s", (int)(name_len), (name)); \
    } \
} while(0)

/* null-sentinel check: ?*T and ?FuncPtr both use NULL as none.
 * Also handles TYPE_DISTINCT wrapping pointer/func_ptr (BUG-088 fix). */
static inline bool is_null_sentinel(Type *inner) {
    if (!inner) return false;
    /* BUG-279: unwrap ALL levels of distinct, not just one */
    while (inner->kind == TYPE_DISTINCT) inner = inner->distinct.underlying;
    /* BUG-393: *opaque is _zer_opaque struct, not a pointer — NOT null sentinel */
    if (inner->kind == TYPE_POINTER && inner->pointer.inner &&
        type_unwrap_distinct(inner->pointer.inner)->kind == TYPE_OPAQUE) return false;
    return inner->kind == TYPE_POINTER || inner->kind == TYPE_FUNC_PTR;
}
#define IS_NULL_SENTINEL(inner_kind) \
    ((inner_kind) == TYPE_POINTER || (inner_kind) == TYPE_FUNC_PTR)
/* NOTE: Use is_null_sentinel(type) for full distinct-aware check.
 * IS_NULL_SENTINEL macro kept for backward compat where only kind is available. */

static void emit_type(Emitter *e, Type *t); /* forward decl for optional helpers */

/* ---- Unified optional emission helpers (prevents BUG-042/145/408/409 class) ----
 * ?void has ONE field (has_value). ?T has TWO (value, has_value). ?*T uses null sentinel.
 * These helpers centralize the branching so new optional paths can't get it wrong. */

/* Is this type ?void (optional wrapping void)? ?void has NO .value field. */
static bool is_void_opt(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind != TYPE_OPTIONAL) return false;
    Type *inner = type_unwrap_distinct(eff->optional.inner);
    return inner && inner->kind == TYPE_VOID;
}

/* Emit null check for optional: "!tmp" for null sentinel, "!tmp.has_value" for struct */
static void emit_opt_null_check(Emitter *e, int tmp_id, Type *opt_type) {
    Type *eff = type_unwrap_distinct(opt_type);
    if (is_null_sentinel(eff->optional.inner))
        emit(e, "!_zer_tmp%d", tmp_id);
    else
        emit(e, "!_zer_tmp%d.has_value", tmp_id);
}

/* Emit unwrap for optional: "tmp" for null sentinel, "tmp.value" for struct, "(void)0" for ?void */
static void emit_opt_unwrap(Emitter *e, int tmp_id, Type *opt_type) {
    Type *eff = type_unwrap_distinct(opt_type);
    if (is_null_sentinel(eff->optional.inner))
        emit(e, "_zer_tmp%d", tmp_id);
    else if (is_void_opt(opt_type))
        emit(e, "(void)0");
    else
        emit(e, "_zer_tmp%d.value", tmp_id);
}

/* Emit null literal for optional type: "(T*)0" / "{ 0 }" / "{ 0, 0 }" */
static void emit_opt_null_literal(Emitter *e, Type *opt_type) {
    Type *eff = type_unwrap_distinct(opt_type);
    if (is_null_sentinel(eff->optional.inner)) {
        emit(e, "(");
        emit_type(e, eff->optional.inner);
        emit(e, ")0");
    } else if (is_void_opt(opt_type)) {
        emit(e, "(");
        emit_type(e, opt_type);
        emit(e, "){ 0 }");
    } else {
        emit(e, "(");
        emit_type(e, opt_type);
        emit(e, "){ 0, 0 }");
    }
}

/* forward declaration needed by emit_opt_wrap_value */
static void emit_expr(Emitter *e, Node *node);

/* B4: Emit value wrapped in optional struct: (Type){ val, 1 }.
 * Used for T → ?T wrapping at assignment, var-decl init.
 * opt_type is the target optional type (may be distinct). */
static void emit_opt_wrap_value(Emitter *e, Type *opt_type, Node *value_expr) {
    emit(e, "(");
    emit_type(e, opt_type);
    emit(e, "){ ");
    emit_expr(e, value_expr);
    emit(e, ", 1 }");
}

/* Emit return-null for current function's return type.
 * Handles ?void, ?T struct, ?*T null sentinel, void, and scalar. */
static void emit_return_null(Emitter *e) {
    Type *ret = e->current_func_ret;
    if (!ret || ret->kind == TYPE_VOID) {
        emit(e, "return; ");
        return;
    }
    Type *eff = type_unwrap_distinct(ret);
    if (eff->kind == TYPE_OPTIONAL && !is_null_sentinel(eff->optional.inner)) {
        emit(e, "return ");
        emit_opt_null_literal(e, ret);
        emit(e, "; ");
    } else {
        emit(e, "return 0; ");
    }
}

/* ---- Array size emission helper (BUG-275) ---- */
/* Emit array size — uses sizeof() for target-dependent sizes, numeric for constant */
static void emit_array_size(Emitter *e, Type *arr_type) {
    if (arr_type->array.sizeof_type) {
        emit(e, "sizeof(");
        emit_type(e, arr_type->array.sizeof_type);
        emit(e, ")");
    } else {
        emit(e, "%llu", (unsigned long long)arr_type->array.size);
    }
}

/* ---- Qualifier helpers (RF11) ---- */

/* Walk an expression to its root ident and look up the symbol.
 * Returns the symbol or NULL if not found. Used to detect volatile/const. */
static Symbol *expr_root_symbol(Emitter *e, Node *expr) {
    Node *root = expr;
    while (root) {
        if (root->kind == NODE_FIELD) root = root->field.object;
        else if (root->kind == NODE_INDEX) root = root->index_expr.object;
        else if (root->kind == NODE_SLICE) root = root->slice.object;
        else if (root->kind == NODE_UNARY && root->unary.op == TOK_STAR)
            root = root->unary.operand;
        else break;
    }
    if (root && root->kind == NODE_IDENT) {
        /* try local scope first, then global */
        Symbol *s = scope_lookup(e->checker->current_scope,
            root->ident.name, (uint32_t)root->ident.name_len);
        if (!s) s = scope_lookup(e->checker->global_scope,
            root->ident.name, (uint32_t)root->ident.name_len);
        return s;
    }
    return NULL;
}

/* Check if an expression's root symbol has volatile qualifier. */
static bool expr_is_volatile(Emitter *e, Node *expr) {
    Symbol *s = expr_root_symbol(e, expr);
    if (s && s->is_volatile) return true;
    /* BUG-414: check volatile struct fields. Walk field chain, look up
     * SField.is_volatile for each field access. Handles: dev.regs where
     * dev is non-volatile but regs field is volatile u8[4]. */
    Node *n = expr;
    while (n && n->kind == NODE_FIELD) {
        Type *obj_type = checker_get_type(e->checker, n->field.object);
        if (obj_type) {
            Type *eff = type_unwrap_distinct(obj_type);
            if (eff->kind == TYPE_STRUCT) {
                for (uint32_t i = 0; i < eff->struct_type.field_count; i++) {
                    if (eff->struct_type.fields[i].name_len == (uint32_t)n->field.field_name_len &&
                        memcmp(eff->struct_type.fields[i].name, n->field.field_name,
                               n->field.field_name_len) == 0) {
                        if (eff->struct_type.fields[i].is_volatile) return true;
                        /* also check type-level volatile (slice/pointer) */
                        Type *ft = eff->struct_type.fields[i].type;
                        if (ft && ft->kind == TYPE_SLICE && ft->slice.is_volatile) return true;
                        if (ft && ft->kind == TYPE_POINTER && ft->pointer.is_volatile) return true;
                        break;
                    }
                }
            }
        }
        n = n->field.object;
    }
    return false;
}

/* ---- Type emission ---- */
static void emit_stmt(Emitter *e, Node *node);
static void prescan_async_temps(Emitter *e, Node *node);
static bool is_condvar_type(Emitter *e, uint32_t type_id);
static bool emit_async_orelse_block(Emitter *e, Node *orelse_expr, Node *fallback,
                                     const char *dest_name, size_t dest_len,
                                     Type *dest_type);

/* Refactor 3: unified shared struct ensure-init emission.
 * Emits _zer_mtx_ensure_init[_cv] for a shared struct access.
 * Handles both condvar and non-condvar types, pointer and direct access.
 * All shared lock sites use this — one place to update for new lock patterns. */
static void emit_shared_ensure_init(Emitter *e, Node *root, const char *arrow) {
    Type *rt = checker_get_type(e->checker, root);
    Type *rte = rt ? type_unwrap_distinct(rt) : NULL;
    if (rte && rte->kind == TYPE_POINTER) rte = type_unwrap_distinct(rte->pointer.inner);
    bool needs_cv = rte && rte->kind == TYPE_STRUCT &&
        is_condvar_type(e, rte->struct_type.type_id);
    if (needs_cv) {
        emit(e, "_zer_mtx_ensure_init_cv(&");
        emit_expr(e, root);
        emit(e, "%s_zer_mtx, &", arrow);
        emit_expr(e, root);
        emit(e, "%s_zer_mtx_inited, &", arrow);
        emit_expr(e, root);
        emit(e, "%s_zer_cond)", arrow);
    } else {
        emit(e, "_zer_mtx_ensure_init(&");
        emit_expr(e, root);
        emit(e, "%s_zer_mtx, &", arrow);
        emit_expr(e, root);
        emit(e, "%s_zer_mtx_inited)", arrow);
    }
}
static Type *resolve_type_for_emit(Emitter *e, TypeNode *tn);
static void emit_auto_guards(Emitter *e, Node *node);
static void emit_defers(Emitter *e);

/* Emit the zero value for a type (used by auto-guard return, auto-orelse).
 * void → nothing (caller emits bare return), integer → 0, bool → 0,
 * optional non-pointer → {0}/{0,0}, pointer → NULL */
static void emit_zero_value(Emitter *e, Type *t) {
    if (!t || t->kind == TYPE_VOID) return;
    Type *inner = type_unwrap_distinct(t);
    if (inner->kind == TYPE_OPTIONAL && !is_null_sentinel(inner->optional.inner)) {
        emit_opt_null_literal(e, t);
    } else if (inner->kind == TYPE_POINTER || inner->kind == TYPE_FUNC_PTR ||
               (inner->kind == TYPE_OPTIONAL && is_null_sentinel(inner->optional.inner))) {
        emit(e, "NULL");
    } else if (inner->kind == TYPE_STRUCT || inner->kind == TYPE_UNION) {
        /* BUG-422: struct/union return needs compound literal, not bare 0 */
        emit(e, "(");
        emit_type(e, t);
        emit(e, "){0}");
    } else {
        emit(e, "0");
    }
}

/* Walk expression tree, emit auto-guard if-return statements for unproven NODE_INDEX.
 * Called BEFORE emit_expr for the containing statement. */
static void emit_auto_guards(Emitter *e, Node *node) {
    if (!node) return;
    switch (node->kind) {
    case NODE_INDEX: {
        uint64_t ag_size = checker_auto_guard_size(e->checker, node);
        if (ag_size > 0) {
            emit_indent(e);
            emit(e, "if ((size_t)(");
            emit_expr(e, node->index_expr.index);
            emit(e, ") >= %lluu) ", (unsigned long long)ag_size);
            if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                emit(e, "{\n");
                emit_defers(e);
                emit(e, "return ");
                emit_zero_value(e, e->current_func_ret);
                emit(e, "; }\n");
            } else {
                emit(e, "{\n");
                emit_defers(e);
                emit(e, "return; }\n");
            }
        }
        emit_auto_guards(e, node->index_expr.object);
        emit_auto_guards(e, node->index_expr.index);
        break;
    }
    case NODE_FIELD:
        /* UAF auto-guard: if handle array element may have been freed at dynamic index,
         * emit if (use_idx == freed_idx) { return <zero>; } */
        if (checker_auto_guard_size(e->checker, node) == UINT64_MAX &&
            node->field.object->kind == NODE_INDEX &&
            node->field.object->index_expr.object->kind == NODE_IDENT) {
            const char *aname = node->field.object->index_expr.object->ident.name;
            uint32_t alen = (uint32_t)node->field.object->index_expr.object->ident.name_len;
            Checker *ck = e->checker;
            for (int dfi = 0; dfi < ck->dyn_freed_count; dfi++) {
                struct DynFreed *df = &ck->dyn_freed[dfi];
                if (df->array_name_len == alen &&
                    memcmp(df->array_name, aname, alen) == 0 && !df->all_freed) {
                    emit_indent(e);
                    emit(e, "if ((");
                    emit_expr(e, node->field.object->index_expr.index);
                    emit(e, ") == (");
                    emit_expr(e, df->freed_idx);
                    emit(e, ")) ");
                    if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                        emit(e, "{\n");
                        emit_defers(e);
                        emit(e, "return ");
                        emit_zero_value(e, e->current_func_ret);
                        emit(e, "; }\n");
                    } else {
                        emit(e, "{\n");
                        emit_defers(e);
                        emit(e, "return; }\n");
                    }
                    break;
                }
            }
        }
        emit_auto_guards(e, node->field.object); break;
    case NODE_ASSIGN:
        emit_auto_guards(e, node->assign.target);
        emit_auto_guards(e, node->assign.value); break;
    case NODE_BINARY:
        emit_auto_guards(e, node->binary.left);
        emit_auto_guards(e, node->binary.right); break;
    case NODE_UNARY:
        emit_auto_guards(e, node->unary.operand); break;
    case NODE_CALL:
        emit_auto_guards(e, node->call.callee);
        for (int i = 0; i < node->call.arg_count; i++)
            emit_auto_guards(e, node->call.args[i]);
        break;
    case NODE_ORELSE:
        emit_auto_guards(e, node->orelse.expr);
        if (node->orelse.fallback && !node->orelse.fallback_is_return &&
            !node->orelse.fallback_is_break && !node->orelse.fallback_is_continue)
            emit_auto_guards(e, node->orelse.fallback);
        break;
    case NODE_INTRINSIC:
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            emit_auto_guards(e, node->intrinsic.args[i]);
        break;
    case NODE_TYPECAST:
        emit_auto_guards(e, node->typecast.expr);
        break;
    case NODE_STRUCT_INIT:
        for (int i = 0; i < node->struct_init.field_count; i++)
            emit_auto_guards(e, node->struct_init.fields[i].value);
        break;
    case NODE_SLICE:
        emit_auto_guards(e, node->slice.object);
        emit_auto_guards(e, node->slice.start);
        emit_auto_guards(e, node->slice.end);
        break;
    /* Leaf nodes — no sub-expressions with array indices */
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_IDENT: case NODE_CAST: case NODE_SIZEOF:
    /* Statement/decl nodes — emit_auto_guards only called on expressions */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL: case NODE_VAR_DECL:
    case NODE_BLOCK: case NODE_IF: case NODE_FOR: case NODE_WHILE: case NODE_DO_WHILE:
    case NODE_SWITCH: case NODE_RETURN: case NODE_BREAK:
    case NODE_CONTINUE: case NODE_DEFER: case NODE_GOTO:
    case NODE_LABEL: case NODE_EXPR_STMT: case NODE_ASM:
    case NODE_CRITICAL: case NODE_ONCE: case NODE_SPAWN:
    case NODE_YIELD: case NODE_AWAIT: case NODE_STATIC_ASSERT:
        break;
    }
}

static Node *find_shared_root(Emitter *e, Node *expr); /* forward decl */

/* Find shared struct variable accessed in a statement or expression.
 * Returns the root ident node if any NODE_FIELD chain leads to a shared struct.
 * Used to auto-insert lock/unlock around statements. */
static Node *find_shared_root_in_stmt(Emitter *e, Node *stmt) {
    if (!stmt) return NULL;
    switch (stmt->kind) {
    case NODE_EXPR_STMT: return find_shared_root(e, stmt->expr_stmt.expr);
    case NODE_VAR_DECL: return find_shared_root(e, stmt->var_decl.init);
    case NODE_RETURN: return find_shared_root(e, stmt->ret.expr);
    case NODE_IF: return find_shared_root(e, stmt->if_stmt.cond);
    case NODE_WHILE: case NODE_DO_WHILE: return find_shared_root(e, stmt->while_stmt.cond);
    case NODE_FOR: {
        Node *r = find_shared_root(e, stmt->for_stmt.init);
        if (!r && stmt->for_stmt.cond) r = find_shared_root(e, stmt->for_stmt.cond);
        return r;
    }
    case NODE_SWITCH: return find_shared_root(e, stmt->switch_stmt.expr);
    default: return NULL;
    }
}

static Node *find_shared_root(Emitter *e, Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == NODE_FIELD) {
        /* Walk to root of field chain */
        Node *root = expr;
        while (root->kind == NODE_FIELD) root = root->field.object;
        while (root->kind == NODE_INDEX) root = root->index_expr.object;
        while (root->kind == NODE_UNARY && root->unary.op == TOK_STAR)
            root = root->unary.operand;
        if (root->kind == NODE_IDENT) {
            Type *t = checker_get_type(e->checker, root);
            if (t) {
                Type *eff = type_unwrap_distinct(t);
                /* Direct shared struct */
                if (eff->kind == TYPE_STRUCT && eff->struct_type.is_shared) return root;
                /* Pointer to shared struct */
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
        found = find_shared_root(e, expr->binary.left);
        if (!found) found = find_shared_root(e, expr->binary.right);
    } else if (expr->kind == NODE_ASSIGN) {
        found = find_shared_root(e, expr->assign.target);
        if (!found) found = find_shared_root(e, expr->assign.value);
    } else if (expr->kind == NODE_CALL) {
        for (int i = 0; i < expr->call.arg_count && !found; i++)
            found = find_shared_root(e, expr->call.args[i]);
    } else if (expr->kind == NODE_UNARY) {
        found = find_shared_root(e, expr->unary.operand);
    } else if (expr->kind == NODE_INDEX) {
        found = find_shared_root(e, expr->index_expr.object);
    } else if (expr->kind == NODE_ORELSE) {
        found = find_shared_root(e, expr->orelse.expr);
    } else if (expr->kind == NODE_TYPECAST) {
        found = find_shared_root(e, expr->typecast.expr);
    }
    return found;
}

static bool is_condvar_type(Emitter *e, uint32_t type_id); /* forward decl */
static bool is_async_local(Emitter *e, const char *name, size_t len); /* forward decl */

/* Check if a shared struct type uses condvar (needs mutex instead of spinlock) */
static bool shared_needs_condvar(Emitter *e, Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind == TYPE_POINTER) eff = type_unwrap_distinct(eff->pointer.inner);
    if (eff && eff->kind == TYPE_STRUCT && eff->struct_type.is_shared)
        return is_condvar_type(e, eff->struct_type.type_id);
    return false;
}

/* Check if a shared struct type uses reader-writer lock */
static bool shared_is_rw(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind == TYPE_POINTER) eff = type_unwrap_distinct(eff->pointer.inner);
    if (eff && eff->kind == TYPE_STRUCT) return eff->struct_type.is_shared_rw;
    return false;
}

/* Check if a statement WRITES to a shared struct (vs read-only).
 * Write = assignment target, compound assign, mutating method call.
 * Used to determine rdlock vs wrlock for shared(rw) structs. */
static bool stmt_writes_shared(Node *stmt) {
    if (!stmt) return false;
    switch (stmt->kind) {
    case NODE_EXPR_STMT:
        /* x.field = ...; or x.field += ...; */
        if (stmt->expr_stmt.expr && stmt->expr_stmt.expr->kind == NODE_ASSIGN)
            return true;
        /* Method calls that mutate (push, free, etc.) */
        if (stmt->expr_stmt.expr && stmt->expr_stmt.expr->kind == NODE_CALL)
            return true; /* conservative: any call might mutate */
        return false;
    case NODE_VAR_DECL:
        return false; /* reading into a variable is read-only */
    case NODE_RETURN:
        return false; /* reading for return */
    default:
        return false;
    }
}

/* Emit lock acquire for shared struct variable.
 * For shared(rw) structs, is_write determines rdlock vs wrlock. */
static void emit_shared_lock_mode(Emitter *e, Node *root, bool is_write) {
    Type *rt = checker_get_type(e->checker, root);
    bool is_ptr = (rt && type_unwrap_distinct(rt)->kind == TYPE_POINTER);
    const char *arrow = is_ptr ? "->" : ".";
    emit_indent(e);
    if (shared_is_rw(rt)) {
        if (is_write) {
            emit(e, "pthread_rwlock_wrlock(&");
        } else {
            emit(e, "pthread_rwlock_rdlock(&");
        }
        emit_expr(e, root);
        emit(e, "%s_zer_rwlock);\n", arrow);
    } else {
        /* Refactor 3: unified shared ensure-init + lock */
        emit_shared_ensure_init(e, root, arrow);
        emit(e, ";\n");
        emit_indent(e);
        emit(e, "pthread_mutex_lock(&");
        emit_expr(e, root);
        emit(e, "%s_zer_mtx);\n", arrow);
    }
}

static void emit_shared_lock(Emitter *e, Node *root) {
    emit_shared_lock_mode(e, root, true); /* default: write lock (conservative) */
}

/* Emit lock release for shared struct variable */
static void emit_shared_unlock(Emitter *e, Node *root) {
    Type *rt = checker_get_type(e->checker, root);
    bool is_ptr = (rt && type_unwrap_distinct(rt)->kind == TYPE_POINTER);
    const char *arrow = is_ptr ? "->" : ".";
    emit_indent(e);
    if (shared_is_rw(rt)) {
        emit(e, "pthread_rwlock_unlock(&");
        emit_expr(e, root);
        emit(e, "%s_zer_rwlock);\n", arrow);
    } else {
        /* BUG-473: all shared structs use recursive pthread_mutex */
        emit(e, "pthread_mutex_unlock(&");
        emit_expr(e, root);
        emit(e, "%s_zer_mtx);\n", arrow);
    }
}

/* RF3: resolve TypeNode via checker's typemap (set during resolve_type).
 * Falls back to resolve_type_for_emit if not cached (safety net). */
static Type *resolve_tynode(Emitter *e, TypeNode *tn) {
    if (!tn) return NULL;
    Type *t = checker_get_type(e->checker, (Node *)tn);
    if (t) return t;
    return resolve_type_for_emit(e, tn);  /* fallback for uncached TypeNodes */
}
static void emit_defers(Emitter *e);
static void emit_defers_from(Emitter *e, int base);

/* emit array→slice coercion: wraps array expr in slice compound literal */
static void emit_array_as_slice(Emitter *e, Node *array_expr, Type *array_type, Type *slice_type) {
    emit(e, "((");
    emit_type(e, slice_type);
    emit(e, "){ (");
    if (slice_type && slice_type->slice.is_volatile) emit(e, "volatile ");
    emit_type(e, array_type->array.inner);
    emit(e, "*)");
    emit_expr(e, array_expr);
    emit(e, ", %llu })", (unsigned long long)array_type->array.size);
}

/* emit a C type name for a ZER type */
static void emit_type(Emitter *e, Type *t) {
    if (!t) { emit(e, "void"); return; }

    switch (t->kind) {
    case TYPE_VOID:   emit(e, "void"); break;
    case TYPE_BOOL:   emit(e, "uint8_t"); break;
    case TYPE_U8:     emit(e, "uint8_t"); break;
    case TYPE_U16:    emit(e, "uint16_t"); break;
    case TYPE_U32:    emit(e, "uint32_t"); break;
    case TYPE_U64:    emit(e, "uint64_t"); break;
    case TYPE_USIZE:  emit(e, "size_t"); break;
    case TYPE_I8:     emit(e, "int8_t"); break;
    case TYPE_I16:    emit(e, "int16_t"); break;
    case TYPE_I32:    emit(e, "int32_t"); break;
    case TYPE_I64:    emit(e, "int64_t"); break;
    case TYPE_F32:    emit(e, "float"); break;
    case TYPE_F64:    emit(e, "double"); break;
    case TYPE_OPAQUE: emit(e, "void"); break;

    case TYPE_POINTER:
        if (t->pointer.inner && type_unwrap_distinct(t->pointer.inner)->kind == TYPE_OPAQUE) {
            /* BUG-393: *opaque → _zer_opaque (tagged struct, not void*) */
            if (t->pointer.is_const) emit(e, "const ");
            if (t->pointer.is_volatile) emit(e, "volatile ");
            emit(e, "_zer_opaque");
        } else {
            if (t->pointer.is_const) emit(e, "const ");
            if (t->pointer.is_volatile) emit(e, "volatile ");
            emit_type(e, t->pointer.inner);
            emit(e, "*");
        }
        break;

    case TYPE_OPTIONAL:
        /* ?*T → pointer (null sentinel) */
        if (is_null_sentinel(t->optional.inner)) {
            emit_type(e, t->optional.inner);
            break;
        }
        /* ?T → named optional typedef.
         * Unwrap TYPE_DISTINCT to find the actual type for typedef lookup. */
        Type *opt_inner = type_unwrap_distinct(t->optional.inner);
        switch (opt_inner->kind) {
        case TYPE_VOID:  emit(e, "_zer_opt_void"); break;
        case TYPE_BOOL:  emit(e, "_zer_opt_bool"); break;
        case TYPE_U8:    emit(e, "_zer_opt_u8"); break;
        case TYPE_U16:   emit(e, "_zer_opt_u16"); break;
        case TYPE_U32:   emit(e, "_zer_opt_u32"); break;
        case TYPE_U64:   emit(e, "_zer_opt_u64"); break;
        case TYPE_I8:    emit(e, "_zer_opt_i8"); break;
        case TYPE_I16:   emit(e, "_zer_opt_i16"); break;
        case TYPE_I32:   emit(e, "_zer_opt_i32"); break;
        case TYPE_I64:   emit(e, "_zer_opt_i64"); break;
        case TYPE_USIZE: emit(e, "_zer_opt_usize"); break;
        case TYPE_F32:   emit(e, "_zer_opt_f32"); break;
        case TYPE_F64:   emit(e, "_zer_opt_f64"); break;
        case TYPE_ENUM:
            emit(e, "_zer_opt_i32");  /* enums are int32_t */
            break;
        case TYPE_HANDLE:
            emit(e, "_zer_opt_u64");  /* handles are uint64_t (BUG-390) */
            break;
        case TYPE_POINTER:
            if (opt_inner->pointer.inner &&
                type_unwrap_distinct(opt_inner->pointer.inner)->kind == TYPE_OPAQUE) {
                emit(e, "_zer_opt_opaque");  /* BUG-393: ?*opaque → struct optional */
            } else {
                /* regular ?*T — null sentinel, shouldn't reach here */
                emit_type(e, opt_inner);
            }
            break;
        case TYPE_STRUCT:
            emit(e, "_zer_opt_");
            EMIT_STRUCT_NAME(e, opt_inner);
            break;
        case TYPE_UNION:
            emit(e, "_zer_opt_");
            EMIT_UNION_NAME(e, opt_inner);
            break;
        case TYPE_SLICE: {
            /* ?[]T → _zer_opt_slice_T — unwrap distinct on element type */
            Type *elem = type_unwrap_distinct(opt_inner->slice.inner);
            switch (elem->kind) {
            case TYPE_U8:
            case TYPE_BOOL:  emit(e, "_zer_opt_slice_u8"); break; /* bool = uint8_t */
            case TYPE_U16:   emit(e, "_zer_opt_slice_u16"); break;
            case TYPE_U32:   emit(e, "_zer_opt_slice_u32"); break;
            case TYPE_U64:   emit(e, "_zer_opt_slice_u64"); break;
            case TYPE_I8:    emit(e, "_zer_opt_slice_i8"); break;
            case TYPE_I16:   emit(e, "_zer_opt_slice_i16"); break;
            case TYPE_I32:   emit(e, "_zer_opt_slice_i32"); break;
            case TYPE_I64:   emit(e, "_zer_opt_slice_i64"); break;
            case TYPE_USIZE: emit(e, "_zer_opt_slice_usize"); break;
            case TYPE_F32:   emit(e, "_zer_opt_slice_f32"); break;
            case TYPE_F64:   emit(e, "_zer_opt_slice_f64"); break;
            case TYPE_STRUCT:
                emit(e, "_zer_opt_slice_");
                EMIT_STRUCT_NAME(e, elem);
                break;
            case TYPE_UNION:
                emit(e, "_zer_opt_slice_");
                EMIT_UNION_NAME(e, elem);
                break;
            default:
                emit(e, "struct { ");
                emit_type(e, opt_inner);
                emit(e, " value; uint8_t has_value; }");
                break;
            }
            break;
        }
        default:
            /* fallback: anonymous struct — only for ?FuncPtr (extremely rare) */
            emit(e, "struct { ");
            emit_type(e, opt_inner);
            emit(e, " value; uint8_t has_value; }");
            break;
        }
        break;

    case TYPE_SLICE: {
        /* Unwrap TYPE_DISTINCT for named typedef lookup */
        Type *sl_inner = type_unwrap_distinct(t->slice.inner);
        const char *prefix = t->slice.is_volatile ? "_zer_vslice_" : "_zer_slice_";
        switch (sl_inner->kind) {
        case TYPE_U8:
        case TYPE_BOOL:  emit(e, "%su8", prefix); break; /* bool = uint8_t */
        case TYPE_U16:   emit(e, "%su16", prefix); break;
        case TYPE_U32:   emit(e, "%su32", prefix); break;
        case TYPE_U64:   emit(e, "%su64", prefix); break;
        case TYPE_I8:    emit(e, "%si8", prefix); break;
        case TYPE_I16:   emit(e, "%si16", prefix); break;
        case TYPE_I32:   emit(e, "%si32", prefix); break;
        case TYPE_I64:   emit(e, "%si64", prefix); break;
        case TYPE_USIZE: emit(e, "%susize", prefix); break;
        case TYPE_F32:   emit(e, "%sf32", prefix); break;
        case TYPE_F64:   emit(e, "%sf64", prefix); break;
        case TYPE_STRUCT:
            emit(e, "%s", prefix);
            EMIT_STRUCT_NAME(e, sl_inner);
            break;
        case TYPE_UNION:
            emit(e, "%s", prefix);
            EMIT_UNION_NAME(e, sl_inner);
            break;
        default:
            emit(e, "struct { ");
            if (t->slice.is_volatile) emit(e, "volatile ");
            emit_type(e, sl_inner);
            emit(e, "* ptr; size_t len; }");
            break;
        }
        break;
    }

    case TYPE_ARRAY: {
        /* BUG-297: emit full array type with dimensions for sizeof() context.
         * Walk to base type, then emit all dimensions. */
        Type *base = t;
        while (base->kind == TYPE_ARRAY) base = base->array.inner;
        emit_type(e, base);
        Type *dim = t;
        while (dim->kind == TYPE_ARRAY) {
            if (dim->array.sizeof_type) {
                emit(e, "[sizeof(");
                emit_type(e, dim->array.sizeof_type);
                emit(e, ")]");
            } else {
                emit(e, "[%llu]", (unsigned long long)dim->array.size);
            }
            dim = dim->array.inner;
        }
        break;
    }

    case TYPE_STRUCT:
        /* Async state structs are emitted as typedef, not struct tag */
        if (t->struct_type.name_len >= 11 &&
            memcmp(t->struct_type.name, "_zer_async_", 11) == 0) {
            emit(e, "%.*s", (int)t->struct_type.name_len, t->struct_type.name);
        } else {
            emit(e, "struct ");
            EMIT_STRUCT_NAME(e, t);
        }
        break;

    case TYPE_ENUM:
        emit(e, "int32_t"); /* enums are i32 */
        break;

    case TYPE_UNION:
        emit(e, "struct _union_");
        EMIT_UNION_NAME(e, t);
        break;

    case TYPE_HANDLE:
        emit(e, "uint64_t"); /* BUG-390: Handle = gen(32) << 32 | index(32) */
        break;

    case TYPE_ARENA:
        emit(e, "_zer_arena");
        break;

    case TYPE_BARRIER:
        emit(e, "_zer_barrier");
        break;

    case TYPE_SEMAPHORE:
        emit(e, "_zer_semaphore");
        break;

    case TYPE_SLAB:
        emit(e, "_zer_slab");
        break;

    case TYPE_POOL:
        emit(e, "struct _zer_pool_");
        EMIT_STRUCT_NAME(e, t->pool.elem);
        emit(e, "_%llu", (unsigned long long)t->pool.count);
        break;

    case TYPE_RING:
        emit(e, "struct _zer_ring_%llu", (unsigned long long)t->ring.count);
        break;

    case TYPE_FUNC_PTR:
        emit_type(e, t->func_ptr.ret);
        emit(e, " (*)(");
        for (uint32_t i = 0; i < t->func_ptr.param_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_type(e, t->func_ptr.params[i]);
        }
        emit(e, ")");
        break;

    case TYPE_DISTINCT:
        /* distinct typedef emits as its underlying type */
        emit_type(e, t->distinct.underlying);
        break;
    }
}

/* emit type with variable name (handles arrays and func ptrs) */
static void emit_type_and_name(Emitter *e, Type *t, const char *name, size_t name_len) {
    if (!t) { emit(e, "void %.*s", (int)name_len, name); return; }

    if (t->kind == TYPE_ARRAY) {
        /* collect all array dimensions, emit base type + name + all dims */
        Type *base = t;
        while (base->kind == TYPE_ARRAY) base = base->array.inner;
        /* function pointer array: ret (*name[dim1][dim2])(params) */
        Type *base_eff = type_unwrap_distinct(base);
        if (base_eff->kind == TYPE_FUNC_PTR) {
            emit_type(e, base_eff->func_ptr.ret);
            emit(e, " (*%.*s", (int)name_len, name);
            Type *dim = t;
            while (dim->kind == TYPE_ARRAY) {
                if (dim->array.sizeof_type) {
                    emit(e, "[sizeof(");
                    emit_type(e, dim->array.sizeof_type);
                    emit(e, ")]");
                } else {
                    emit(e, "[%llu]", (unsigned long long)dim->array.size);
                }
                dim = dim->array.inner;
            }
            emit(e, ")(");
            for (uint32_t i = 0; i < base_eff->func_ptr.param_count; i++) {
                if (i > 0) emit(e, ", ");
                emit_type(e, base_eff->func_ptr.params[i]);
            }
            emit(e, ")");
            return;
        }
        emit_type(e, base);
        emit(e, " %.*s", (int)name_len, name);
        Type *dim = t;
        while (dim->kind == TYPE_ARRAY) {
            if (dim->array.sizeof_type) {
                /* BUG-275: target-dependent size — emit sizeof(T) */
                emit(e, "[sizeof(");
                emit_type(e, dim->array.sizeof_type);
                emit(e, ")]");
            } else {
                emit(e, "[%llu]", (unsigned long long)dim->array.size);
            }
            dim = dim->array.inner;
        }
        return;
    }

    /* function pointer: ret (*name)(param1, param2, ...) */
    if (t->kind == TYPE_FUNC_PTR) {
        emit_type(e, t->func_ptr.ret);
        emit(e, " (*%.*s)(", (int)name_len, name);
        for (uint32_t i = 0; i < t->func_ptr.param_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_type(e, t->func_ptr.params[i]);
        }
        emit(e, ")");
        return;
    }

    /* distinct function pointer: unwrap distinct to get func ptr for name placement */
    if (t->kind == TYPE_DISTINCT && type_unwrap_distinct(t)->kind == TYPE_FUNC_PTR) {
        Type *fp = type_unwrap_distinct(t);
        emit_type(e, fp->func_ptr.ret);
        emit(e, " (*%.*s)(", (int)name_len, name);
        for (uint32_t i = 0; i < fp->func_ptr.param_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_type(e, fp->func_ptr.params[i]);
        }
        emit(e, ")");
        return;
    }

    /* A19: distinct wrapping optional wrapping funcptr — unwrap distinct first */
    if (t->kind == TYPE_DISTINCT) {
        Type *dt = type_unwrap_distinct(t);
        if (dt->kind == TYPE_OPTIONAL && type_unwrap_distinct(dt->optional.inner)->kind == TYPE_FUNC_PTR) {
            Type *fp = type_unwrap_distinct(dt->optional.inner);
            emit_type(e, fp->func_ptr.ret);
            emit(e, " (*%.*s)(", (int)name_len, name);
            for (uint32_t i = 0; i < fp->func_ptr.param_count; i++) {
                if (i > 0) emit(e, ", ");
                emit_type(e, fp->func_ptr.params[i]);
            }
            emit(e, ")");
            return;
        }
    }

    /* optional function pointer: ?ret (*name)(params) → ret (*name)(params) (null sentinel) */
    if (t->kind == TYPE_OPTIONAL && t->optional.inner->kind == TYPE_FUNC_PTR) {
        Type *fp = t->optional.inner;
        emit_type(e, fp->func_ptr.ret);
        emit(e, " (*%.*s)(", (int)name_len, name);
        for (uint32_t i = 0; i < fp->func_ptr.param_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_type(e, fp->func_ptr.params[i]);
        }
        emit(e, ")");
        return;
    }

    /* optional distinct function pointer: ?DistinctFuncPtr → null sentinel with name inside (*) */
    if (t->kind == TYPE_OPTIONAL && type_unwrap_distinct(t->optional.inner)->kind == TYPE_FUNC_PTR) {
        Type *fp = type_unwrap_distinct(t->optional.inner);
        emit_type(e, fp->func_ptr.ret);
        emit(e, " (*%.*s)(", (int)name_len, name);
        for (uint32_t i = 0; i < fp->func_ptr.param_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_type(e, fp->func_ptr.params[i]);
        }
        emit(e, ")");
        return;
    }

    emit_type(e, t);
    emit(e, " %.*s", (int)name_len, name);
}

/* ================================================================
 * EXPRESSION EMISSION
 * ================================================================ */

static void emit_expr(Emitter *e, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_INT_LIT:
        if (node->int_lit.value > 0xFFFFFFFF) {
            emit(e, "%lluULL", (unsigned long long)node->int_lit.value);
        } else {
            emit(e, "%llu", (unsigned long long)node->int_lit.value);
        }
        break;

    case NODE_FLOAT_LIT:
        emit(e, "%.17g", node->float_lit.value);
        break;

    case NODE_STRING_LIT:
        /* emit as _zer_slice_u8 compound literal */
        emit(e, "((_zer_slice_u8){ (uint8_t*)\"%.*s\", %zu })",
             (int)node->string_lit.length, node->string_lit.value,
             node->string_lit.length);
        break;

    case NODE_CHAR_LIT:
        if (node->char_lit.value == '\n') emit(e, "'\\n'");
        else if (node->char_lit.value == '\t') emit(e, "'\\t'");
        else if (node->char_lit.value == '\r') emit(e, "'\\r'");
        else if (node->char_lit.value == '\0') emit(e, "'\\0'");
        else if (node->char_lit.value == '\\') emit(e, "'\\\\'");
        else if (node->char_lit.value == '\'') emit(e, "'\\''");
        else emit(e, "'%c'", node->char_lit.value);
        break;

    case NODE_BOOL_LIT:
        emit(e, "%d", node->bool_lit.value ? 1 : 0);
        break;

    case NODE_NULL_LIT:
        emit(e, "0");
        break;

    case NODE_IDENT: {
        /* Async local promotion: emit self->name for promoted locals */
        if (is_async_local(e, node->ident.name, node->ident.name_len)) {
            emit(e, "self->%.*s", (int)node->ident.name_len, node->ident.name);
            break;
        }
        /* BUG-218/222/229/233: module-aware identifier emission.
         * When inside a module body (current_module set), PREFER the mangled key
         * for the current module. This prevents cross-module collision where raw
         * key resolves to wrong module's symbol. */
        bool emitted = false;
        if (e->current_module) {
            /* BUG-233/332: try current module's mangled key FIRST (double underscore) */
            uint32_t mkl = e->current_module_len + 2 + (uint32_t)node->ident.name_len;
            char mk_buf[512];
            char *mk = mk_buf;
            if (mkl >= sizeof(mk_buf)) mk = (char *)arena_alloc(e->arena, mkl + 1);
            memcpy(mk, e->current_module, e->current_module_len);
            mk[e->current_module_len] = '_';
            mk[e->current_module_len + 1] = '_';
            memcpy(mk + e->current_module_len + 2, node->ident.name, node->ident.name_len);
            mk[mkl] = '\0';
            Symbol *ms = scope_lookup(e->checker->global_scope, mk, mkl);
            if (ms && ms->module_prefix) {
                emit(e, "%.*s__%.*s",
                     (int)ms->module_prefix_len, ms->module_prefix,
                     (int)node->ident.name_len, node->ident.name);
                emitted = true;
            }
        }
        if (!emitted) {
            /* Fall back to raw key lookup */
            Symbol *id_sym = scope_lookup(e->checker->global_scope,
                node->ident.name, (uint32_t)node->ident.name_len);
            if (id_sym && id_sym->module_prefix) {
                emit(e, "%.*s__%.*s",
                     (int)id_sym->module_prefix_len, id_sym->module_prefix,
                     (int)node->ident.name_len, node->ident.name);
            } else {
                emit(e, "%.*s", (int)node->ident.name_len, node->ident.name);
            }
        }
        break;
    }

    case NODE_BINARY:
        /* division/modulo: trap on zero divisor (skip if proven safe by range propagation) */
        if ((node->binary.op == TOK_SLASH || node->binary.op == TOK_PERCENT) &&
            checker_is_proven(e->checker, node)) {
            /* proven nonzero divisor — emit plain division, no check */
            emit(e, "(");
            emit_expr(e, node->binary.left);
            emit(e, " %s ", node->binary.op == TOK_SLASH ? "/" : "%");
            emit_expr(e, node->binary.right);
            emit(e, ")");
            break;
        }
        if (node->binary.op == TOK_SLASH || node->binary.op == TOK_PERCENT) {
            int tmp = e->temp_count++;
            Type *div_type = checker_get_type(e->checker,node->binary.left);
            bool is_signed_div = div_type && type_is_signed(div_type);
            emit(e, "({ __typeof__(");
            emit_expr(e, node->binary.right);
            emit(e, ") _zer_dv%d = ", tmp);
            emit_expr(e, node->binary.right);
            emit(e, "; if (_zer_dv%d == 0) ", tmp);
            emit(e, "_zer_trap(\"division by zero\", __FILE__, __LINE__); ");
            /* signed overflow: INT_MIN / -1 traps on x86/ARM */
            if (is_signed_div) {
                emit(e, "if (_zer_dv%d == -1) { __typeof__(", tmp);
                emit_expr(e, node->binary.left);
                emit(e, ") _zer_dd%d = ", tmp);
                emit_expr(e, node->binary.left);
                /* check if dividend is the minimum value for its type */
                int w = type_width(div_type);
                if (w == 8) emit(e, "; if (_zer_dd%d == -128) ", tmp);
                else if (w == 16) emit(e, "; if (_zer_dd%d == -32768) ", tmp);
                else if (w == 32) emit(e, "; if (_zer_dd%d == (-2147483647-1)) ", tmp);
                else emit(e, "; if (_zer_dd%d == (-9223372036854775807LL-1)) ", tmp);
                emit(e, "_zer_trap(\"signed division overflow\", __FILE__, __LINE__); } ");
            }
            emit(e, "(");
            emit_expr(e, node->binary.left);
            emit(e, " %s _zer_dv%d); })",
                 node->binary.op == TOK_SLASH ? "/" : "%", tmp);
            break;
        }
        /* shift operators use safe macros (ZER spec: shift >= width = 0) */
        if (node->binary.op == TOK_LSHIFT || node->binary.op == TOK_RSHIFT) {
            emit(e, "%s(", node->binary.op == TOK_LSHIFT ? "_zer_shl" : "_zer_shr");
            emit_expr(e, node->binary.left);
            emit(e, ", ");
            emit_expr(e, node->binary.right);
            emit(e, ")");
        } else {
            /* check if result type is narrower than int — need cast to prevent
             * C integer promotion from changing wrapping behavior */
            bool needs_narrow_cast = false;
            const char *narrow_cast = "";
            if (node->binary.op == TOK_PLUS || node->binary.op == TOK_MINUS ||
                node->binary.op == TOK_STAR || node->binary.op == TOK_AMP ||
                node->binary.op == TOK_PIPE || node->binary.op == TOK_CARET) {
                Type *res_type = checker_get_type(e->checker,node);
                if (res_type) {
                    switch (res_type->kind) {
                    case TYPE_U8:  narrow_cast = "(uint8_t)"; needs_narrow_cast = true; break;
                    case TYPE_I8:  narrow_cast = "(int8_t)"; needs_narrow_cast = true; break;
                    case TYPE_U16: narrow_cast = "(uint16_t)"; needs_narrow_cast = true; break;
                    case TYPE_I16: narrow_cast = "(int16_t)"; needs_narrow_cast = true; break;
                    default: break;
                    }
                }
            }
            /* BUG-257: optional == null / != null for struct-based optionals.
             * ?*T (null-sentinel) uses plain pointer comparison, but ?u32 etc. are
             * structs — must compare .has_value instead of raw struct == 0. */
            if ((node->binary.op == TOK_EQEQ || node->binary.op == TOK_BANGEQ) &&
                (node->binary.left->kind == NODE_NULL_LIT ||
                 node->binary.right->kind == NODE_NULL_LIT)) {
                Node *opt_node = node->binary.left->kind == NODE_NULL_LIT ?
                    node->binary.right : node->binary.left;
                Type *opt_type = checker_get_type(e->checker, opt_node);
                /* BUG-409: unwrap distinct for optional null comparison */
                Type *opt_eff = opt_type ? type_unwrap_distinct(opt_type) : NULL;
                if (opt_eff && opt_eff->kind == TYPE_OPTIONAL &&
                    !is_null_sentinel(opt_eff->optional.inner)) {
                    /* struct optional: emit .has_value check */
                    if (node->binary.op == TOK_EQEQ) emit(e, "(!");
                    else emit(e, "(");
                    emit_expr(e, opt_node);
                    emit(e, ".has_value)");
                    break;
                }
            }
            /* BUG-485: *opaque comparison — _zer_opaque is ALWAYS a struct
             * (not just when track_cptrs). C can't use == on structs.
             * Compare .ptr fields instead. */
            if ((node->binary.op == TOK_EQEQ || node->binary.op == TOK_BANGEQ) &&
                node->binary.left->kind != NODE_NULL_LIT &&
                node->binary.right->kind != NODE_NULL_LIT) {
                Type *lt = checker_get_type(e->checker, node->binary.left);
                Type *rt = checker_get_type(e->checker, node->binary.right);
                lt = lt ? type_unwrap_distinct(lt) : NULL;
                rt = rt ? type_unwrap_distinct(rt) : NULL;
                /* *opaque = TYPE_POINTER with inner TYPE_OPAQUE */
                bool l_opaque = lt && lt->kind == TYPE_POINTER &&
                    lt->pointer.inner && lt->pointer.inner->kind == TYPE_OPAQUE;
                bool r_opaque = rt && rt->kind == TYPE_POINTER &&
                    rt->pointer.inner && rt->pointer.inner->kind == TYPE_OPAQUE;
                if (l_opaque || r_opaque) {
                    emit(e, "(");
                    emit_expr(e, node->binary.left);
                    if (l_opaque) emit(e, ".ptr");
                    emit(e, " %s ", node->binary.op == TOK_EQEQ ? "==" : "!=");
                    emit_expr(e, node->binary.right);
                    if (r_opaque) emit(e, ".ptr");
                    emit(e, ")");
                    break;
                }
            }
            if (needs_narrow_cast) emit(e, "%s", narrow_cast);
            emit(e, "(");
            emit_expr(e, node->binary.left);
            switch (node->binary.op) {
            case TOK_PLUS:     emit(e, " + "); break;
            case TOK_MINUS:    emit(e, " - "); break;
            case TOK_STAR:     emit(e, " * "); break;
            case TOK_SLASH:    emit(e, " / "); break;
            case TOK_PERCENT:  emit(e, " %% "); break;
            case TOK_EQEQ:    emit(e, " == "); break;
            case TOK_BANGEQ:   emit(e, " != "); break;
            case TOK_LT:       emit(e, " < "); break;
            case TOK_GT:       emit(e, " > "); break;
            case TOK_LTEQ:     emit(e, " <= "); break;
            case TOK_GTEQ:     emit(e, " >= "); break;
            case TOK_AMPAMP:   emit(e, " && "); break;
            case TOK_PIPEPIPE: emit(e, " || "); break;
            case TOK_AMP:      emit(e, " & "); break;
            case TOK_PIPE:     emit(e, " | "); break;
            case TOK_CARET:    emit(e, " ^ "); break;
            default:           emit(e, " ? "); break;
            }
            emit_expr(e, node->binary.right);
            emit(e, ")");
        }
        break;

    case NODE_UNARY:
        /* BUG-215: narrow type unary cast — C promotes u8/u16/i8/i16 to int.
         * ~(u8)0xAA = 0xFFFFFF55 in C, but ZER expects 0x55. Cast result. */
        if (node->unary.op == TOK_TILDE || node->unary.op == TOK_MINUS) {
            Type *res = checker_get_type(e->checker,node);
            if (res) res = type_unwrap_distinct(res);
            if (res && (res->kind == TYPE_U8 || res->kind == TYPE_U16 ||
                        res->kind == TYPE_I8 || res->kind == TYPE_I16)) {
                emit(e, "(");
                emit_type(e, res);
                emit(e, ")(");
                if (node->unary.op == TOK_TILDE) emit(e, "~");
                else emit(e, "-");
                emit_expr(e, node->unary.operand);
                emit(e, ")");
                break;
            }
        }
        switch (node->unary.op) {
        case TOK_MINUS: emit(e, "(-"); break;
        case TOK_BANG:  emit(e, "(!"); break;
        case TOK_TILDE: emit(e, "(~"); break;
        case TOK_STAR:  emit(e, "(*"); break;
        case TOK_AMP:   emit(e, "(&"); break;
        default:        emit(e, "("); break;
        }
        emit_expr(e, node->unary.operand);
        emit(e, ")");
        break;

    case NODE_ASSIGN:
        /* union variant assignment: msg.sensor = val → set tag first */
        if (node->assign.op == TOK_EQ &&
            node->assign.target->kind == NODE_FIELD) {
            Node *obj_node = node->assign.target->field.object;
            Type *obj_type_raw = checker_get_type(e->checker,obj_node);
            Type *obj_type = obj_type_raw ? type_unwrap_distinct(obj_type_raw) : NULL;
            if (obj_type && obj_type->kind == TYPE_UNION) {
                /* find variant index */
                const char *vname = node->assign.target->field.field_name;
                uint32_t vlen = (uint32_t)node->assign.target->field.field_name_len;
                for (uint32_t i = 0; i < obj_type->union_type.variant_count; i++) {
                    SUVariant *v = &obj_type->union_type.variants[i];
                    if (v->name_len == vlen && memcmp(v->name, vname, vlen) == 0) {
                        /* BUG-340: hoist target into pointer temp for single-eval */
                        {
                            int tmp = e->temp_count++;
                            emit(e, "({ __typeof__(");
                            emit_expr(e, obj_node);
                            emit(e, ") *_zer_up%d = &(", tmp);
                            emit_expr(e, obj_node);
                            emit(e, "); _zer_up%d->_tag = %u; _zer_up%d->", tmp, i, tmp);
                            fprintf(e->out, "%.*s", (int)vlen, vname);
                            emit(e, " = ");
                            emit_expr(e, node->assign.value);
                            emit(e, "; })");
                        }
                        goto assign_done;
                    }
                }
            }
        }
        /* BUG-210/216: bit-set assignment: reg[7..0] = 0xFF
         * → ({ auto *_p = &obj; *_p = (*_p & ~mask) | ((val << lo) & mask); })
         * Uses pointer hoist for single-eval of target expression. */
        if (node->assign.op == TOK_EQ &&
            node->assign.target->kind == NODE_SLICE) {
            Type *obj_type = checker_get_type(e->checker,node->assign.target->slice.object);
            if (obj_type && type_is_integer(obj_type)) {
                Node *obj = node->assign.target->slice.object;
                Node *hi_node = node->assign.target->slice.start;  /* high bit */
                Node *lo_node = node->assign.target->slice.end;    /* low bit */
                int btmp = e->temp_count++;
                /* BUG-216: hoist target address for single-eval.
                 * Use *({ auto val = obj; auto *p = &val; ... }) pattern
                 * for simple vars. For indexed/field access, this writes to
                 * a copy which is OK — the outer assignment handles write-back. */
                /* __typeof__ does NOT evaluate its argument.
                 * &(obj) evaluates obj exactly once as an lvalue.
                 * *_p reads/writes through cached pointer — no re-eval. */
                emit(e, "({ __typeof__(");
                emit_expr(e, obj);
                emit(e, ") *_zer_bp%d = &(", btmp);
                emit_expr(e, obj);
                emit(e, "); ");
                /* BUG-316: hoist hi/lo into temps for single evaluation */
                int64_t const_hi = hi_node ? eval_const_expr(hi_node) : CONST_EVAL_FAIL;
                int64_t const_lo = lo_node ? eval_const_expr(lo_node) : CONST_EVAL_FAIL;
                bool bits_const = (const_hi != CONST_EVAL_FAIL && const_lo != CONST_EVAL_FAIL &&
                                   const_hi >= 0 && const_lo >= 0);
                if (!bits_const && hi_node && lo_node) {
                    emit(e, "uint64_t _zer_bh%d = (uint64_t)(", btmp);
                    emit_expr(e, hi_node);
                    emit(e, "); uint64_t _zer_bl%d = (uint64_t)(", btmp);
                    emit_expr(e, lo_node);
                    emit(e, "); ");
                }
                emit(e, "*_zer_bp%d = (*_zer_bp%d", btmp, btmp);
                emit(e, " & ~(");
                /* emit mask: safe for width >= 64 */
                if (hi_node && lo_node) {
                    if (bits_const) {
                        int64_t width = const_hi - const_lo + 1;
                        if (width >= 64) {
                            emit(e, "~(uint64_t)0");
                        } else {
                            emit(e, "((1ull << %lld) - 1)", (long long)width);
                        }
                        emit(e, " << %lld", (long long)const_lo);
                    } else {
                        /* runtime: use hoisted temps */
                        emit(e, "(((_zer_bh%d - _zer_bl%d + 1) >= 64 ? "
                             "~(uint64_t)0 : ((1ull << (_zer_bh%d - _zer_bl%d + 1)) - 1)) "
                             "<< _zer_bl%d)",
                             btmp, btmp, btmp, btmp, btmp);
                    }
                }
                emit(e, ")) | (((uint64_t)(");
                emit_expr(e, node->assign.value);
                emit(e, ") << ");
                if (hi_node && lo_node) {
                    if (bits_const) emit(e, "%lld", (long long)const_lo);
                    else emit(e, "_zer_bl%d", btmp);
                } else {
                    emit(e, "0");
                }
                emit(e, ") & (");
                /* re-emit mask */
                if (hi_node && lo_node) {
                    if (bits_const) {
                        int64_t width = const_hi - const_lo + 1;
                        if (width >= 64) {
                            emit(e, "~(uint64_t)0");
                        } else {
                            emit(e, "((1ull << %lld) - 1)", (long long)width);
                        }
                        emit(e, " << %lld", (long long)const_lo);
                    } else {
                        emit(e, "(((_zer_bh%d - _zer_bl%d + 1) >= 64 ? "
                             "~(uint64_t)0 : ((1ull << (_zer_bh%d - _zer_bl%d + 1)) - 1)) "
                             "<< _zer_bl%d)",
                             btmp, btmp, btmp, btmp, btmp);
                    }
                }
                emit(e, ")); })");
                goto assign_done;
            }
        }
        /* array assignment: x = y → memcpy(x, y, sizeof(x)) — C arrays aren't lvalues
         * BUG-252: hoist target into pointer temp for single evaluation.
         * get_s().arr = local was calling get_s() twice (dest + sizeof). */
        if (node->assign.op == TOK_EQ) {
            Type *tgt_type = checker_get_type(e->checker,node->assign.target);
            if (tgt_type && type_unwrap_distinct(tgt_type)->kind == TYPE_ARRAY) {
                /* BUG-273/320: check if target OR source is volatile — use byte loop */
                bool arr_volatile = expr_is_volatile(e, node->assign.target) ||
                                    expr_is_volatile(e, node->assign.value);
                int tmp = e->temp_count++;
                if (arr_volatile) {
                    emit(e, "({ volatile uint8_t *_zer_vd%d = (volatile uint8_t*)&(", tmp);
                    emit_expr(e, node->assign.target);
                    emit(e, "); const volatile uint8_t *_zer_vs%d = (const volatile uint8_t*)&(", tmp);
                    emit_expr(e, node->assign.value);
                    emit(e, "); for (size_t _i = 0; _i < sizeof(");
                    emit_expr(e, node->assign.target);
                    emit(e, "); _i++) _zer_vd%d[_i] = _zer_vs%d[_i]; })", tmp, tmp);
                } else {
                    emit(e, "({ __typeof__(");
                    emit_expr(e, node->assign.target);
                    emit(e, ") *_zer_ma%d = &(", tmp);
                    emit_expr(e, node->assign.target);
                    /* BUG-306: use memmove for overlap-safe self-assignment */
                    emit(e, "); memmove(_zer_ma%d, ", tmp);
                    emit_expr(e, node->assign.value);
                    emit(e, ", sizeof(*_zer_ma%d)); })", tmp);
                }
                goto assign_done;
            }
        }
        /* compound div/mod: target /= n → check n != 0 first */
        if (node->assign.op == TOK_SLASHEQ || node->assign.op == TOK_PERCENTEQ) {
            int tmp = e->temp_count++;
            emit(e, "({ __typeof__(");
            emit_expr(e, node->assign.value);
            emit(e, ") _zer_dv%d = ", tmp);
            emit_expr(e, node->assign.value);
            emit(e, "; if (_zer_dv%d == 0) ", tmp);
            emit(e, "_zer_trap(\"division by zero\", __FILE__, __LINE__); ");
            emit_expr(e, node->assign.target);
            emit(e, " %s= _zer_dv%d; })",
                 node->assign.op == TOK_SLASHEQ ? "/" : "%", tmp);
            goto assign_done;
        }
        /* compound shift: target <<= n → target = _zer_shl(target, n)
         * If target has side effects, hoist via pointer to avoid double-eval */
        if (node->assign.op == TOK_LSHIFTEQ || node->assign.op == TOK_RSHIFTEQ) {
            /* check if any node in target chain has side effects */
            bool shift_side_effect = false;
            {
                Node *n = node->assign.target;
                while (n) {
                    if (n->kind == NODE_CALL || n->kind == NODE_ASSIGN) {
                        shift_side_effect = true; break;
                    }
                    if (n->kind == NODE_FIELD) n = n->field.object;
                    else if (n->kind == NODE_INDEX) n = n->index_expr.object;
                    else break;
                }
            }
            const char *macro = node->assign.op == TOK_LSHIFTEQ ? "_zer_shl" : "_zer_shr";
            if (shift_side_effect) {
                /* hoist target into pointer: *({ auto *_p = &target; *_p = macro(*_p, n); _p; }) — but simpler: */
                int tmp = e->temp_count++;
                emit(e, "({ __auto_type _zer_sp%d = &(", tmp);
                emit_expr(e, node->assign.target);
                emit(e, "); *_zer_sp%d = %s(*_zer_sp%d, ", tmp, macro, tmp);
                emit_expr(e, node->assign.value);
                emit(e, "); })");
            } else {
                emit_expr(e, node->assign.target);
                emit(e, " = %s(", macro);
                emit_expr(e, node->assign.target);
                emit(e, ", ");
                emit_expr(e, node->assign.value);
                emit(e, ")");
            }
        } else {
        emit_expr(e, node->assign.target);
        switch (node->assign.op) {
        case TOK_EQ:        emit(e, " = "); break;
        case TOK_PLUSEQ:    emit(e, " += "); break;
        case TOK_MINUSEQ:   emit(e, " -= "); break;
        case TOK_STAREQ:    emit(e, " *= "); break;
        case TOK_SLASHEQ:   emit(e, " /= "); break;
        case TOK_PERCENTEQ: emit(e, " %%= "); break;
        case TOK_AMPEQ:     emit(e, " &= "); break;
        case TOK_PIPEEQ:    emit(e, " |= "); break;
        case TOK_CARETEQ:   emit(e, " ^= "); break;
        default:            emit(e, " = "); break;
        }
        /* T → ?T wrap: if target is optional and value isn't, wrap in {value, 1} */
        Type *tgt_type = checker_get_type(e->checker,node->assign.target);
        Type *val_type = checker_get_type(e->checker,node->assign.value);
        /* BUG-409: unwrap distinct for optional assignment checks */
        Type *tgt_eff = tgt_type ? type_unwrap_distinct(tgt_type) : NULL;
        if (node->assign.op == TOK_EQ && tgt_eff && val_type &&
            tgt_eff->kind == TYPE_OPTIONAL &&
            !is_null_sentinel(tgt_eff->optional.inner) &&
            val_type->kind != TYPE_OPTIONAL &&
            node->assign.value->kind != NODE_NULL_LIT) {
            emit_opt_wrap_value(e, tgt_type, node->assign.value);
        } else if (node->assign.op == TOK_EQ && tgt_eff &&
                   tgt_eff->kind == TYPE_OPTIONAL &&
                   !is_null_sentinel(tgt_eff->optional.inner) &&
                   node->assign.value->kind == NODE_NULL_LIT) {
            emit_opt_null_literal(e, tgt_type);
        } else if (node->assign.op == TOK_EQ && tgt_eff && val_type &&
                   tgt_eff->kind == TYPE_SLICE &&
                   type_unwrap_distinct(val_type)->kind == TYPE_ARRAY) {
            /* BUG-419: array→slice coercion in assignment (same as var-decl) */
            emit_array_as_slice(e, node->assign.value, val_type, tgt_type);
        } else {
            emit_expr(e, node->assign.value);
        }
        } /* close else for compound shift */
        assign_done:
        break;

    case NODE_CALL: {
        /* comptime call — emit constant value directly.
         * BUG-388: if target type is optional, wrap in {value, 1}. */
        if (node->call.is_comptime_resolved) {
            /* Comptime struct return — emit as compound literal */
            if (node->call.comptime_struct_init) {
                emit_expr(e, node->call.comptime_struct_init);
                break;
            }
            /* Comptime float return — emit double literal */
            if (node->call.is_comptime_float) {
                emit(e, "%.17g", node->call.comptime_float_value);
                break;
            }
            Type *ct = checker_get_type(e->checker, node);
            /* BUG-506: unwrap distinct for optional wrapping check */
            Type *ct_eff = ct ? type_unwrap_distinct(ct) : NULL;
            if (ct_eff && ct_eff->kind == TYPE_OPTIONAL) {
                emit(e, "(");
                emit_type(e, ct);
                emit(e, "){%lld, 1}", (long long)node->call.comptime_value);
            } else {
                emit(e, "%lld", (long long)node->call.comptime_value);
            }
            break;
        }
        /* intercept builtin method calls: pool.alloc(), pool.get(h), etc. */
        bool handled = false;
        if (node->call.callee->kind == NODE_FIELD) {
            Node *obj_node = node->call.callee->field.object;
            const char *mname = node->call.callee->field.field_name;
            uint32_t mlen = (uint32_t)node->call.callee->field.field_name_len;

            /* check if object is a Pool variable — try checker type first, then global scope */
            if (obj_node->kind == NODE_IDENT) {
                Type *obj_type = checker_get_type(e->checker,obj_node);
                Symbol *sym = NULL;
                if (!obj_type)  {
                    sym = scope_lookup(e->checker->global_scope,
                        obj_node->ident.name, (uint32_t)obj_node->ident.name_len);
                    if (sym) obj_type = sym->type;
                }
                if (obj_type && obj_type->kind == TYPE_POOL) {
                    Type *pool = obj_type;
                    const char *pname = obj_node->ident.name;
                    int plen = (int)obj_node->ident.name_len;

                    if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                        /* pool.alloc() → _zer_pool_alloc(...) wrapped in optional */
                        int tmp = e->temp_count++;
                        emit(e, "({uint8_t _zer_aok%d = 0; uint64_t _zer_ah%d = "
                             "_zer_pool_alloc(%.*s.slots, sizeof(%.*s.slots[0]), "
                             "%.*s.gen, %.*s.used, %llu, &_zer_aok%d); "
                             "(_zer_opt_u64){_zer_ah%d, _zer_aok%d}; })",
                             tmp, tmp,
                             plen, pname, plen, pname,
                             plen, pname, plen, pname,
                             (unsigned long long)pool->pool.count, tmp, tmp, tmp);
                        handled = true;
                    } else if (mlen == 3 && memcmp(mname, "get", 3) == 0) {
                        /* pool.get(h) → return pointer to slot (not deref) */
                        emit(e, "((");
                        emit_type(e, pool->pool.elem);
                        emit(e, "*)_zer_pool_get(%.*s.slots, %.*s.gen, %.*s.used, "
                             "sizeof(%.*s.slots[0]), ",
                             plen, pname, plen, pname, plen, pname, plen, pname);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, ", %llu))", (unsigned long long)pool->pool.count);
                        handled = true;
                    } else if (mlen == 4 && memcmp(mname, "free", 4) == 0) {
                        /* pool.free(h) */
                        emit(e, "_zer_pool_free(%.*s.gen, %.*s.used, ",
                             plen, pname, plen, pname);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, ", %llu)", (unsigned long long)pool->pool.count);
                        handled = true;
                    } else if (mlen == 9 && memcmp(mname, "alloc_ptr", 9) == 0) {
                        /* pool.alloc_ptr() → alloc slot, return pointer (NULL if full) */
                        int tmp = e->temp_count++;
                        emit(e, "({uint8_t _zer_aok%d = 0; uint64_t _zer_ah%d = "
                             "_zer_pool_alloc(%.*s.slots, sizeof(%.*s.slots[0]), "
                             "%.*s.gen, %.*s.used, %llu, &_zer_aok%d); ",
                             tmp, tmp,
                             plen, pname, plen, pname,
                             plen, pname, plen, pname,
                             (unsigned long long)pool->pool.count, tmp);
                        /* ?*T is null sentinel — return pointer or NULL */
                        emit(e, "_zer_aok%d ? (", tmp);
                        emit_type(e, pool->pool.elem);
                        emit(e, "*)_zer_pool_get(%.*s.slots, %.*s.gen, %.*s.used, "
                             "sizeof(%.*s.slots[0]), _zer_ah%d, %llu) : (void*)0; })",
                             plen, pname, plen, pname, plen, pname,
                             plen, pname, tmp,
                             (unsigned long long)pool->pool.count);
                        handled = true;
                    } else if (mlen == 8 && memcmp(mname, "free_ptr", 8) == 0) {
                        /* pool.free_ptr(ptr) → find slot index from pointer, free it */
                        emit(e, "_zer_pool_free(%.*s.gen, %.*s.used, "
                             "((uint64_t)((char*)(", plen, pname, plen, pname);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, ") - (char*)%.*s.slots) / sizeof(%.*s.slots[0])), %llu)",
                             plen, pname, plen, pname,
                             (unsigned long long)pool->pool.count);
                        handled = true;
                    }
                }

                if (!handled && obj_type && obj_type->kind == TYPE_RING) {
                    const char *rname = obj_node->ident.name;
                    int rlen = (int)obj_node->ident.name_len;

                    if (mlen == 4 && memcmp(mname, "push", 4) == 0) {
                        /* ring.push(val) — cast to correct element type */
                        int tmp = e->temp_count++;
                        emit(e, "({");
                        emit_type(e, obj_type->ring.elem);
                        emit(e, " _zer_rpv%d = ", tmp);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, "; _zer_ring_push(%.*s.data, &%.*s.head, &%.*s.tail, "
                             "&%.*s.count, %llu, &_zer_rpv%d, sizeof(_zer_rpv%d)); })",
                             rlen, rname, rlen, rname, rlen, rname, rlen, rname,
                             (unsigned long long)obj_type->ring.count, tmp, tmp);
                        handled = true;
                    } else if (mlen == 3 && memcmp(mname, "pop", 3) == 0) {
                        /* ring.pop() → optional of elem type */
                        int tmp = e->temp_count++;
                        Type *opt_type = type_optional(e->arena, obj_type->ring.elem);
                        emit(e, "({");
                        emit_type(e, opt_type);
                        /* BUG-348: acquire barrier after data read, before tail update */
                        emit(e, " _zer_rp%d = {0}; "
                             "if (%.*s.count > 0) { "
                             "_zer_rp%d.value = %.*s.data[%.*s.tail]; "
                             "__atomic_thread_fence(__ATOMIC_ACQUIRE); "
                             "_zer_rp%d.has_value = 1; "
                             "%.*s.tail = (%.*s.tail + 1) %% %llu; "
                             "%.*s.count--; } "
                             "_zer_rp%d; })",
                             tmp,
                             rlen, rname,
                             tmp, rlen, rname, rlen, rname,
                             tmp,
                             rlen, rname, rlen, rname, (unsigned long long)obj_type->ring.count,
                             rlen, rname,
                             tmp);
                        handled = true;
                    } else if (mlen == 12 && memcmp(mname, "push_checked", 12) == 0) {
                        /* ring.push_checked(val) → ?void (null if full) */
                        int tmp = e->temp_count++;
                        emit(e, "({");
                        emit_type(e, obj_type->ring.elem);
                        emit(e, " _zer_rpv%d = ", tmp);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, "; _zer_opt_void _zer_rpc%d = {0}; "
                             "if (%.*s.count < %llu) { "
                             "_zer_ring_push(%.*s.data, &%.*s.head, &%.*s.tail, "
                             "&%.*s.count, %llu, &_zer_rpv%d, sizeof(_zer_rpv%d)); "
                             "_zer_rpc%d.has_value = 1; } "
                             "_zer_rpc%d; })",
                             tmp,
                             rlen, rname, (unsigned long long)obj_type->ring.count,
                             rlen, rname, rlen, rname, rlen, rname,
                             rlen, rname, (unsigned long long)obj_type->ring.count, tmp, tmp,
                             tmp,
                             tmp);
                        handled = true;
                    }
                }

                /* Slab methods */
                if (!handled && obj_type && obj_type->kind == TYPE_SLAB) {
                    const char *sname = obj_node->ident.name;
                    int slen = (int)obj_node->ident.name_len;

                    if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                        int tmp = e->temp_count++;
                        emit(e, "({uint8_t _zer_aok%d = 0; uint64_t _zer_ah%d = "
                             "_zer_slab_alloc(&%.*s, &_zer_aok%d); "
                             "(_zer_opt_u64){_zer_ah%d, _zer_aok%d}; })",
                             tmp, tmp,
                             slen, sname, tmp, tmp, tmp);
                        handled = true;
                    } else if (mlen == 3 && memcmp(mname, "get", 3) == 0) {
                        emit(e, "((");
                        emit_type(e, obj_type->slab.elem);
                        emit(e, "*)_zer_slab_get(&%.*s, ", slen, sname);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, "))");
                        handled = true;
                    } else if (mlen == 4 && memcmp(mname, "free", 4) == 0) {
                        emit(e, "_zer_slab_free(&%.*s, ", slen, sname);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, ")");
                        handled = true;
                    } else if (mlen == 9 && memcmp(mname, "alloc_ptr", 9) == 0) {
                        /* slab.alloc_ptr() → alloc slot, return pointer (NULL if OOM) */
                        int tmp = e->temp_count++;
                        emit(e, "({uint8_t _zer_aok%d = 0; uint64_t _zer_ah%d = "
                             "_zer_slab_alloc(&%.*s, &_zer_aok%d); ",
                             tmp, tmp, slen, sname, tmp);
                        /* ?*T is null sentinel — return pointer or NULL */
                        emit(e, "_zer_aok%d ? (", tmp);
                        emit_type(e, obj_type->slab.elem);
                        emit(e, "*)_zer_slab_get(&%.*s, _zer_ah%d) : (void*)0; })",
                             slen, sname, tmp);
                        handled = true;
                    } else if (mlen == 8 && memcmp(mname, "free_ptr", 8) == 0) {
                        /* slab.free_ptr(ptr) → find handle from pointer, free it */
                        emit(e, "_zer_slab_free_ptr(&%.*s, ", slen, sname);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, ")");
                        handled = true;
                    }
                }

                /* Arena methods */
                if (!handled && obj_type && obj_type->kind == TYPE_ARENA) {
                    const char *aname = obj_node->ident.name;
                    int alen = (int)obj_node->ident.name_len;

                    if (mlen == 4 && memcmp(mname, "over", 4) == 0) {
                        /* Arena.over(buf) → (_zer_arena){ (uint8_t*)buf, sizeof(buf), 0 }
                         * or for slices: (_zer_arena){ buf.ptr, buf.len, 0 }
                         * BUG-286: hoist arg into temp for single evaluation */
                        if (node->call.arg_count > 0) {
                            int tmp = e->temp_count++;
                            Type *arg_type = checker_get_type(e->checker,node->call.args[0]);
                            Type *arg_eff = arg_type ? type_unwrap_distinct(arg_type) : NULL;
                            if (arg_eff && arg_eff->kind == TYPE_SLICE) {
                                emit(e, "({ __auto_type _zer_ao%d = ", tmp);
                                emit_expr(e, node->call.args[0]);
                                emit(e, "; (_zer_arena){ (uint8_t*)_zer_ao%d.ptr, _zer_ao%d.len, 0 }; })", tmp, tmp);
                            } else {
                                emit(e, "((_zer_arena){ (uint8_t*)");
                                emit_expr(e, node->call.args[0]);
                                emit(e, ", sizeof(");
                                emit_expr(e, node->call.args[0]);
                                emit(e, "), 0 })");
                            }
                        }
                        handled = true;
                    } else if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                        /* arena.alloc(T) → (T*)_zer_arena_alloc(&arena, sizeof(T))
                         * Returns ?*T — null sentinel (NULL = none) */
                        if (node->call.arg_count >= 1 &&
                            node->call.args[0]->kind == NODE_IDENT) {
                            const char *tname = node->call.args[0]->ident.name;
                            int tlen = (int)node->call.args[0]->ident.name_len;
                            /* Look up type to emit correct C name */
                            Symbol *tsym = scope_lookup(e->checker->global_scope,
                                tname, (uint32_t)tlen);
                            if (tsym && tsym->type) {
                                emit(e, "((");
                                emit_type(e, tsym->type);
                                emit(e, "*)_zer_arena_alloc(&%.*s, sizeof(",
                                     alen, aname);
                                emit_type(e, tsym->type);
                                emit(e, "), _Alignof(");
                                emit_type(e, tsym->type);
                                emit(e, ")))");
                            }
                        }
                        handled = true;
                    } else if (mlen == 11 && memcmp(mname, "alloc_slice", 11) == 0) {
                        /* arena.alloc_slice(T, n) → ?[]T
                         * Optional slice: { .value = { .ptr, .len }, .has_value } */
                        if (node->call.arg_count >= 2 &&
                            node->call.args[0]->kind == NODE_IDENT) {
                            const char *tname = node->call.args[0]->ident.name;
                            int tlen = (int)node->call.args[0]->ident.name_len;
                            Symbol *tsym = scope_lookup(e->checker->global_scope,
                                tname, (uint32_t)tlen);
                            if (tsym && tsym->type) {
                                int tmp = e->temp_count++;
                                Type *slice_type = type_slice(e->arena, tsym->type);
                                Type *opt_type = type_optional(e->arena, slice_type);
                                emit(e, "({ size_t _zer_asn%d = (size_t)", tmp);
                                emit_expr(e, node->call.args[1]);
                                /* BUG-266: overflow-safe multiplication for alloc size */
                                emit(e, "; size_t _zer_asz%d; void *_zer_asp%d = "
                                     "__builtin_mul_overflow(sizeof(", tmp, tmp);
                                emit_type(e, tsym->type);
                                emit(e, "), _zer_asn%d, &_zer_asz%d) ? (void*)0 : "
                                     "_zer_arena_alloc(&%.*s, _zer_asz%d, _Alignof(",
                                     tmp, tmp, alen, aname, tmp);
                                emit_type(e, tsym->type);
                                emit(e, ")); ");
                                emit_type(e, opt_type);
                                emit(e, " _zer_asr%d = {0}; ", tmp);
                                emit(e, "if (_zer_asp%d) { _zer_asr%d.value.ptr = (", tmp, tmp);
                                emit_type(e, tsym->type);
                                emit(e, "*)_zer_asp%d; _zer_asr%d.value.len = _zer_asn%d; "
                                     "_zer_asr%d.has_value = 1; } ",
                                     tmp, tmp, tmp, tmp);
                                emit(e, "_zer_asr%d; })", tmp);
                            }
                        }
                        handled = true;
                    } else if ((mlen == 5 && memcmp(mname, "reset", 5) == 0) ||
                               (mlen == 12 && memcmp(mname, "unsafe_reset", 12) == 0)) {
                        /* arena.reset() / arena.unsafe_reset() → reset offset to 0 */
                        emit(e, "(%.*s.offset = 0)", alen, aname);
                        handled = true;
                    }
                }
            }
        }

        /* Task.new() / Task.delete() — auto-Slab sugar */
        if (!handled && node->call.callee->kind == NODE_FIELD) {
            Node *obj_n = node->call.callee->field.object;
            Type *ot = checker_get_type(e->checker, obj_n);
            if (ot) ot = type_unwrap_distinct(ot);
            if (ot && ot->kind == TYPE_STRUCT) {
                const char *mn = node->call.callee->field.field_name;
                uint32_t ml = (uint32_t)node->call.callee->field.field_name_len;
                /* find auto-slab name */
                char asname[128];
                int aslen = snprintf(asname, sizeof(asname), "_zer_auto_slab_%.*s",
                    (int)ot->struct_type.name_len, ot->struct_type.name);
                if (ml == 3 && memcmp(mn, "new", 3) == 0) {
                    /* Task.new() → slab.alloc() */
                    int tmp = e->temp_count++;
                    emit(e, "({uint8_t _zer_aok%d = 0; uint64_t _zer_ah%d = "
                         "_zer_slab_alloc(&%.*s, &_zer_aok%d); "
                         "(_zer_opt_u64){_zer_ah%d, _zer_aok%d}; })",
                         tmp, tmp, aslen, asname, tmp, tmp, tmp);
                    handled = true;
                } else if (ml == 7 && memcmp(mn, "new_ptr", 7) == 0) {
                    /* Task.new_ptr() → slab.alloc_ptr() */
                    int tmp = e->temp_count++;
                    emit(e, "({uint8_t _zer_aok%d = 0; uint64_t _zer_ah%d = "
                         "_zer_slab_alloc(&%.*s, &_zer_aok%d); ",
                         tmp, tmp, aslen, asname, tmp);
                    emit(e, "_zer_aok%d ? (", tmp);
                    emit_type(e, ot);
                    emit(e, "*)_zer_slab_get(&%.*s, _zer_ah%d) : (void*)0; })",
                         aslen, asname, tmp);
                    handled = true;
                } else if (ml == 6 && memcmp(mn, "delete", 6) == 0) {
                    /* Task.delete(h) → slab.free(h) */
                    emit(e, "_zer_slab_free(&%.*s, ", aslen, asname);
                    if (node->call.arg_count > 0)
                        emit_expr(e, node->call.args[0]);
                    emit(e, ")");
                    handled = true;
                } else if (ml == 10 && memcmp(mn, "delete_ptr", 10) == 0) {
                    /* Task.delete_ptr(p) → slab.free_ptr(p) */
                    emit(e, "_zer_slab_free_ptr(&%.*s, ", aslen, asname);
                    if (node->call.arg_count > 0)
                        emit_expr(e, node->call.args[0]);
                    emit(e, ")");
                    handled = true;
                }
            }
        }

        /* ThreadHandle.join() → pthread_join(th, NULL)
         * Check if object is a ThreadHandle by matching against spawn wrapper names */
        if (!handled && node->call.callee->kind == NODE_FIELD) {
            Node *thobj = node->call.callee->field.object;
            const char *thmn = node->call.callee->field.field_name;
            uint32_t thml = (uint32_t)node->call.callee->field.field_name_len;
            if (thobj->kind == NODE_IDENT && thml == 4 && memcmp(thmn, "join", 4) == 0) {
                /* Check if this ident matches any scoped spawn's handle name */
                for (int swi = 0; swi < e->spawn_wrapper_count; swi++) {
                    Node *sn = e->spawn_wrappers[swi].spawn_node;
                    if (sn->spawn_stmt.handle_name &&
                        sn->spawn_stmt.handle_name_len == thobj->ident.name_len &&
                        memcmp(sn->spawn_stmt.handle_name, thobj->ident.name,
                               thobj->ident.name_len) == 0) {
                        emit(e, "pthread_join(%.*s, NULL)",
                             (int)thobj->ident.name_len, thobj->ident.name);
                        handled = true;
                        break;
                    }
                }
            }
        }

        if (!handled) {
            /* normal function call */
            emit_expr(e, node->call.callee);
            emit(e, "(");
            Type *callee_type = checker_get_type(e->checker,node->call.callee);
            for (int i = 0; i < node->call.arg_count; i++) {
                if (i > 0) emit(e, ", ");
                /* unwrap distinct for callee type */
                Type *eff_callee = type_unwrap_distinct(callee_type);
                /* slice→pointer decay: emit .ptr when passing []T to *T */
                Type *arg_type_raw = checker_get_type(e->checker,node->call.args[i]);
                Type *arg_type = arg_type_raw ? type_unwrap_distinct(arg_type_raw) : NULL;
                bool need_decay = arg_type && arg_type->kind == TYPE_SLICE &&
                    eff_callee && eff_callee->kind == TYPE_FUNC_PTR &&
                    (uint32_t)i < eff_callee->func_ptr.param_count &&
                    eff_callee->func_ptr.params[i]->kind == TYPE_POINTER;
                /* array→slice coercion: wrap T[N] in slice compound literal */
                bool need_arr_coerce = arg_type && arg_type->kind == TYPE_ARRAY &&
                    eff_callee && eff_callee->kind == TYPE_FUNC_PTR &&
                    (uint32_t)i < eff_callee->func_ptr.param_count &&
                    eff_callee->func_ptr.params[i]->kind == TYPE_SLICE;
                if (need_arr_coerce) {
                    emit_array_as_slice(e, node->call.args[i], arg_type,
                                        eff_callee->func_ptr.params[i]);
                } else {
                    emit_expr(e, node->call.args[i]);
                    if (need_decay) emit(e, ".ptr");
                }
            }
            emit(e, ")");
        }
        break;
    }

    case NODE_FIELD: {
        /* check if object is an enum type → emit _ZER_EnumName_variant */
        Type *obj_type = checker_get_type(e->checker,node->field.object);
        /* fallback for imported modules: typemap may not have the node */
        if (!obj_type && node->field.object->kind == NODE_IDENT) {
            Symbol *sym = scope_lookup(e->checker->global_scope,
                node->field.object->ident.name,
                (uint32_t)node->field.object->ident.name_len);
            if (sym) obj_type = sym->type;
        }
        /* BUG-410: unwrap distinct for field access dispatch */
        Type *obj_eff = obj_type ? type_unwrap_distinct(obj_type) : NULL;

        /* BUG-501: array.len → emit array size as literal.
         * Fixed arrays in C don't have .len field. range-for desugaring
         * generates collection.len for both slices and arrays. */
        if (obj_eff && obj_eff->kind == TYPE_ARRAY &&
            node->field.field_name_len == 3 &&
            memcmp(node->field.field_name, "len", 3) == 0) {
            emit(e, "%lluU", (unsigned long long)obj_eff->array.size);
            break;
        }

        if (obj_eff && obj_eff->kind == TYPE_ENUM) {
            emit(e, "_ZER_");
            EMIT_ENUM_NAME(e, obj_eff);
            emit(e, "_%.*s",
                 (int)node->field.field_name_len, node->field.field_name);
            break;
        }
        /* Handle auto-deref: h.field → ((T*)_zer_slab_get(&slab, h))->field
         * or ((T*)_zer_pool_get(pool.slots, pool.gen, pool.used, sizeof(pool.slots[0]), h, N))->field */
        if (obj_type && type_unwrap_distinct(obj_type)->kind == TYPE_HANDLE) {
            Type *handle_type = type_unwrap_distinct(obj_type);
            /* find the allocator symbol — first try slab_source on the variable */
            Symbol *alloc_sym = NULL;
            if (node->field.object->kind == NODE_IDENT) {
                Symbol *hsym = scope_lookup(e->checker->current_scope,
                    node->field.object->ident.name,
                    (uint32_t)node->field.object->ident.name_len);
                if (!hsym) hsym = scope_lookup(e->checker->global_scope,
                    node->field.object->ident.name,
                    (uint32_t)node->field.object->ident.name_len);
                if (hsym) alloc_sym = hsym->slab_source;
            }
            /* fallback: find unique allocator for this element type.
             * BUG-416: cross-module Handle auto-deref — also search by
             * struct name match, not just pointer identity, since the
             * slab's elem type may have been resolved from a different
             * scope context than the handle's elem type. */
            if (!alloc_sym) {
                alloc_sym = find_unique_allocator(e->checker->current_scope,
                    handle_type->handle.elem);
                if (!alloc_sym)
                    alloc_sym = find_unique_allocator(e->checker->global_scope,
                        handle_type->handle.elem);
                /* BUG-416 name-based fallback removed — pointer identity works correctly.
                 * The previous session's failure was environment-specific (popen crash). */
            }
            if (alloc_sym && alloc_sym->type) {
                Type *at = alloc_sym->type;
                if (at->kind == TYPE_SLAB) {
                    emit(e, "((");
                    emit_type(e, at->slab.elem);
                    emit(e, "*)_zer_slab_get(&%.*s, ",
                         (int)alloc_sym->name_len, alloc_sym->name);
                    emit_expr(e, node->field.object);
                    emit(e, "))->%.*s",
                         (int)node->field.field_name_len, node->field.field_name);
                } else if (at->kind == TYPE_POOL) {
                    emit(e, "((");
                    emit_type(e, at->pool.elem);
                    emit(e, "*)_zer_pool_get(%.*s.slots, %.*s.gen, %.*s.used, "
                         "sizeof(%.*s.slots[0]), ",
                         (int)alloc_sym->name_len, alloc_sym->name,
                         (int)alloc_sym->name_len, alloc_sym->name,
                         (int)alloc_sym->name_len, alloc_sym->name,
                         (int)alloc_sym->name_len, alloc_sym->name);
                    emit_expr(e, node->field.object);
                    emit(e, ", %llu))->%.*s",
                         (unsigned long long)at->pool.count,
                         (int)node->field.field_name_len, node->field.field_name);
                }
            } else {
                /* shouldn't happen — checker should have caught this */
                emit(e, "/* ERROR: no allocator for handle auto-deref */ 0");
            }
            break;
        }

        /* check if object is a pointer → use -> instead of .
         * BUG-410: unwrap distinct — distinct typedef *T still uses -> */
        emit_expr(e, node->field.object);
        if (obj_eff && obj_eff->kind == TYPE_POINTER) {
            emit(e, "->%.*s", (int)node->field.field_name_len, node->field.field_name);
        } else {
            emit(e, ".%.*s", (int)node->field.field_name_len, node->field.field_name);
        }
        break;
    }

    case NODE_INDEX: {
        /* Auto-guard is emitted at statement level by emit_auto_guards().
         * By the time we reach here, the guard has already been emitted.
         * The normal bounds check still runs as belt-and-suspenders backup. */
        /* Value range propagation: if bounds proven safe, skip check entirely */
        if (checker_is_proven(e->checker, node)) {
            Type *proven_obj_type = checker_get_type(e->checker, node->index_expr.object);
            Type *proven_eff = proven_obj_type ? type_unwrap_distinct(proven_obj_type) : NULL;
            if (proven_eff && proven_eff->kind == TYPE_SLICE) {
                emit_expr(e, node->index_expr.object);
                emit(e, ".ptr[");
                emit_expr(e, node->index_expr.index);
                emit(e, "]");
            } else {
                emit_expr(e, node->index_expr.object);
                emit(e, "[");
                emit_expr(e, node->index_expr.index);
                emit(e, "]");
            }
            break;
        }
        /* Inline bounds check using comma operator:
         *   array:  (_zer_bounds_check(idx, size, ...), arr)[idx]
         *   slice:  (_zer_bounds_check(idx, s.len, ...), s.ptr)[idx]
         * Comma operator preserves lvalue (array decays to pointer).
         * Inline check respects short-circuit (&&/||) and works in
         * if/while/for conditions — fixes both hoisting and missing-check bugs. */
        Type *idx_obj_type_raw = checker_get_type(e->checker,node->index_expr.object);
        /* BUG-410: unwrap distinct for array/slice/pointer index dispatch */
        Type *idx_obj_type = idx_obj_type_raw ? type_unwrap_distinct(idx_obj_type_raw) : NULL;
        /* Check if index or object has side effects — needs single-eval.
         * Simple expressions (ident, literal) can safely double-evaluate. */
        /* detect index expressions with side effects or volatile reads.
         * NODE_CALL, NODE_ASSIGN: obvious side effects.
         * NODE_UNARY(deref): volatile pointer deref must not be double-read.
         * BUG-255: NODE_ORELSE may wrap a NODE_CALL (e.g. get() orelse 0). */
        bool idx_has_side_effects = (node->index_expr.index->kind == NODE_CALL ||
                                      node->index_expr.index->kind == NODE_ASSIGN ||
                                      node->index_expr.index->kind == NODE_UNARY ||
                                      node->index_expr.index->kind == NODE_ORELSE);
        /* check if base object has side effects (e.g. get_slice()[0]) */
        bool obj_has_side_effects = false;
        {
            Node *n = node->index_expr.object;
            while (n) {
                if (n->kind == NODE_CALL || n->kind == NODE_ASSIGN) {
                    obj_has_side_effects = true; break;
                }
                if (n->kind == NODE_FIELD) n = n->field.object;
                else if (n->kind == NODE_INDEX) n = n->index_expr.object;
                else break;
            }
        }
        if (idx_obj_type && idx_obj_type->kind == TYPE_ARRAY &&
            (idx_obj_type->array.size > 0 || idx_obj_type->array.sizeof_type)) {
            if (idx_has_side_effects) {
                /* Single-eval lvalue path: pointer dereference preserves lvalue.
                 * *({ size_t _i = idx; check(_i); &arr[_i]; }) */
                int tmp = e->temp_count++;
                emit(e, "*({ size_t _zer_idx%d = (size_t)(", tmp);
                emit_expr(e, node->index_expr.index);
                emit(e, "); _zer_bounds_check(_zer_idx%d, ", tmp);
                emit_array_size(e, idx_obj_type);
                emit(e, ", __FILE__, __LINE__); &");
                emit_expr(e, node->index_expr.object);
                emit(e, "[_zer_idx%d]; })", tmp);
            } else {
                /* Simple index — comma operator, preserves lvalue */
                emit(e, "(_zer_bounds_check((size_t)(");
                emit_expr(e, node->index_expr.index);
                emit(e, "), ");
                emit_array_size(e, idx_obj_type);
                emit(e, ", __FILE__, __LINE__), ");
                emit_expr(e, node->index_expr.object);
                emit(e, ")[");
                emit_expr(e, node->index_expr.index);
                emit(e, "]");
            }
        } else if (idx_obj_type && idx_obj_type->kind == TYPE_SLICE) {
            if (idx_has_side_effects || obj_has_side_effects) {
                /* hoist both object and index for single-eval.
                 * A18: use __typeof__ to preserve volatile (BUG-319 pattern). */
                int tmp = e->temp_count++;
                emit(e, "*({ __typeof__(");
                emit_expr(e, node->index_expr.object);
                emit(e, ") _zer_obj%d = ", tmp);
                emit_expr(e, node->index_expr.object);
                emit(e, "; size_t _zer_idx%d = (size_t)(", tmp);
                emit_expr(e, node->index_expr.index);
                emit(e, "); _zer_bounds_check(_zer_idx%d, _zer_obj%d.len, __FILE__, __LINE__); &",
                     tmp, tmp);
                emit(e, "_zer_obj%d.ptr[_zer_idx%d]; })", tmp, tmp);
            } else {
                emit(e, "(_zer_bounds_check((size_t)(");
                emit_expr(e, node->index_expr.index);
                emit(e, "), ");
                emit_expr(e, node->index_expr.object);
                emit(e, ".len, __FILE__, __LINE__), ");
                emit_expr(e, node->index_expr.object);
                emit(e, ".ptr)[");
                emit_expr(e, node->index_expr.index);
                emit(e, "]");
            }
        } else {
            emit_expr(e, node->index_expr.object);
            emit(e, "[");
            emit_expr(e, node->index_expr.index);
            emit(e, "]");
        }
        break;
    }

    case NODE_SLICE: {
        /* Bit extraction: reg[high..low] on integer → (reg >> low) & mask
         * Array slicing: buf[start..end] → slice struct */
        Type *obj_type_raw = checker_get_type(e->checker,node->slice.object);
        /* BUG-410: unwrap distinct for slice/array/integer dispatch */
        Type *obj_type = obj_type_raw ? type_unwrap_distinct(obj_type_raw) : NULL;
        if (obj_type && type_is_integer(obj_type) &&
            node->slice.start && node->slice.end) {
            /* bit extraction: expr[high..low] → ((unsigned)expr >> low) & mask
             * Cast to unsigned for signed types (right-shift on signed is impl-defined).
             * Safe mask for both constant and runtime widths. */
            {
                /* determine unsigned cast for signed types */
                bool need_unsigned_cast = type_is_signed(obj_type);
                const char *ucast = "";
                if (need_unsigned_cast) {
                    switch (obj_type->kind) {
                    case TYPE_I8:  ucast = "(uint8_t)"; break;
                    case TYPE_I16: ucast = "(uint16_t)"; break;
                    case TYPE_I32: ucast = "(uint32_t)"; break;
                    case TYPE_I64: ucast = "(uint64_t)"; break;
                    default: break;
                    }
                }
                int64_t high = eval_const_expr(node->slice.start);
                int64_t low = eval_const_expr(node->slice.end);
                int64_t width = (high != CONST_EVAL_FAIL && low != CONST_EVAL_FAIL && high >= 0 && low >= 0) ? high - low + 1 : -1;
                if (width >= 64) {
                    /* constant full-width — just emit the value (mask is all 1s) */
                    emit(e, "%s", ucast);
                    emit_expr(e, node->slice.object);
                } else if (width > 0) {
                    /* constant — safe, precomputed width */
                    emit(e, "((%s", ucast);
                    emit_expr(e, node->slice.object);
                    emit(e, " >> %lld) & ((1ull << %lld) - 1))", (long long)low, (long long)width);
                } else {
                    /* runtime — single-eval: hoist start/end into temps */
                    int tmp = e->temp_count++;
                    emit(e, "({ int _zer_hi%d = (int)(", tmp);
                    emit_expr(e, node->slice.start);
                    emit(e, "); int _zer_lo%d = (int)(", tmp);
                    emit_expr(e, node->slice.end);
                    emit(e, "); int _zer_w%d = _zer_hi%d - _zer_lo%d + 1; ((%s", tmp, tmp, tmp, ucast);
                    emit_expr(e, node->slice.object);
                    emit(e, " >> _zer_lo%d) & ((_zer_w%d >= 64) ? ~(uint64_t)0 : (_zer_w%d <= 0) ? (uint64_t)0 : ((1ull << _zer_w%d) - 1))); })",
                         tmp, tmp, tmp, tmp);
                }
            }
            break;
        }

        /* buf[start..end] → (_zer_slice_T){ &buf[start], end - start }
         * buf[start..]   → (_zer_slice_T){ &buf[start], buf_len - start }
         * buf[..end]     → (_zer_slice_T){ &buf[0], end } */
        /* For simplicity, emit raw pointer + compute length inline */
        Type *elem_type = obj_type ? (obj_type->kind == TYPE_ARRAY ?
            obj_type->array.inner : obj_type->kind == TYPE_SLICE ?
            obj_type->slice.inner : NULL) : NULL;
        /* Unwrap distinct for named typedef lookup */
        Type *eff_elem = type_unwrap_distinct(elem_type);
        /* detect side effects early — if present, skip normal struct literal */
        bool slice_obj_side_effect_early = false;
        if (obj_type && obj_type->kind == TYPE_SLICE) {
            Node *n = node->slice.object;
            while (n) {
                if (n->kind == NODE_CALL || n->kind == NODE_ASSIGN) {
                    slice_obj_side_effect_early = true; break;
                }
                if (n->kind == NODE_FIELD) n = n->field.object;
                else if (n->kind == NODE_INDEX) n = n->index_expr.object;
                else break;
            }
        }
        /* runtime check: start <= end for variable indices */
        bool slice_needs_runtime_check = false;
        if (node->slice.start && node->slice.end && !type_is_integer(obj_type)) {
            int64_t sv = eval_const_expr(node->slice.start);
            int64_t ev = eval_const_expr(node->slice.end);
            if (sv == CONST_EVAL_FAIL || ev == CONST_EVAL_FAIL) slice_needs_runtime_check = true;
        }

        /* Use named _zer_slice_T typedefs for ALL types (BUG-085 fix) */
        bool slice_type_emitted = false;
        if (slice_needs_runtime_check && !slice_obj_side_effect_early) {
            /* skip the normal struct literal — we'll wrap in stmt expr */
        } else if (eff_elem && !slice_obj_side_effect_early) {
            const char *sname = NULL;
            switch (eff_elem->kind) {
            case TYPE_U8:    sname = "_zer_slice_u8"; break;
            case TYPE_U16:   sname = "_zer_slice_u16"; break;
            case TYPE_U32:   sname = "_zer_slice_u32"; break;
            case TYPE_U64:   sname = "_zer_slice_u64"; break;
            case TYPE_I8:    sname = "_zer_slice_i8"; break;
            case TYPE_I16:   sname = "_zer_slice_i16"; break;
            case TYPE_I32:   sname = "_zer_slice_i32"; break;
            case TYPE_I64:   sname = "_zer_slice_i64"; break;
            case TYPE_USIZE: sname = "_zer_slice_usize"; break;
            case TYPE_F32:   sname = "_zer_slice_f32"; break;
            case TYPE_F64:   sname = "_zer_slice_f64"; break;
            case TYPE_BOOL:  sname = "_zer_slice_u8"; break; /* bool = uint8_t */
            default: break;
            }
            if (sname) {
                emit(e, "((%s){ ", sname);
                slice_type_emitted = true;
            } else if (eff_elem->kind == TYPE_STRUCT) {
                emit(e, "((_zer_slice_");
                EMIT_STRUCT_NAME(e, eff_elem);
                emit(e, "){ ");
                slice_type_emitted = true;
            } else if (eff_elem->kind == TYPE_UNION) {
                emit(e, "((_zer_slice_");
                EMIT_UNION_NAME(e, eff_elem);
                emit(e, "){ ");
                slice_type_emitted = true;
            }
        }
        if (!slice_type_emitted && !slice_obj_side_effect_early && !slice_needs_runtime_check) {
            emit(e, "((struct { ");
            if (elem_type) {
                emit_type(e, elem_type);
            } else {
                emit(e, "void");
            }
            emit(e, "* ptr; size_t len; }){ ");
        }
        /* ptr = &obj[start] or &obj.ptr[start] for slices
         * Hoist object if it has side effects (func call in chain) */
        bool obj_is_slice = obj_type && obj_type->kind == TYPE_SLICE;
        bool slice_obj_side_effect = false;
        {
            Node *n = node->slice.object;
            while (n) {
                if (n->kind == NODE_CALL || n->kind == NODE_ASSIGN) {
                    slice_obj_side_effect = true; break;
                }
                if (n->kind == NODE_FIELD) n = n->field.object;
                else if (n->kind == NODE_INDEX) n = n->index_expr.object;
                else break;
            }
        }

        if (slice_obj_side_effect && obj_is_slice) {
            /* hoist entire object into temp, build slice from temp */
            int sl_tmp = e->temp_count++;
            /* A18: __typeof__ preserves volatile */
            emit(e, "({ __typeof__(");
            emit_expr(e, node->slice.object);
            emit(e, ") _zer_so%d = ", sl_tmp);
            emit_expr(e, node->slice.object);
            emit(e, "; ");
            if (slice_type_emitted) { /* re-emit type name for inner struct */ }
            /* rebuild the slice struct from the temp */
            emit(e, "(");
            emit_type(e, type_slice(e->arena, obj_type->slice.inner));
            emit(e, "){ &(_zer_so%d.ptr)[", sl_tmp);
            if (node->slice.start) emit_expr(e, node->slice.start);
            else emit(e, "0");
            emit(e, "], ");
            if (node->slice.end && node->slice.start) {
                emit(e, "("); emit_expr(e, node->slice.end);
                emit(e, ") - ("); emit_expr(e, node->slice.start); emit(e, ")");
            } else if (node->slice.end) {
                emit_expr(e, node->slice.end);
            } else if (node->slice.start) {
                emit(e, "_zer_so%d.len - (", sl_tmp);
                emit_expr(e, node->slice.start); emit(e, ")");
            } else {
                emit(e, "_zer_so%d.len", sl_tmp);
            }
            emit(e, " }; })");
        } else if (slice_needs_runtime_check) {
            /* BUG-262: hoist start/end into temps for single evaluation */
            int sl_tmp = e->temp_count++;
            emit(e, "({ size_t _zer_ss%d = (size_t)(", sl_tmp);
            emit_expr(e, node->slice.start);
            emit(e, "); size_t _zer_se%d = (size_t)(", sl_tmp);
            emit_expr(e, node->slice.end);
            emit(e, "); if (_zer_ss%d > _zer_se%d) _zer_trap(\"slice start > end\", __FILE__, __LINE__); (", sl_tmp, sl_tmp);
            emit_type(e, type_slice(e->arena, obj_type->kind == TYPE_ARRAY ?
                obj_type->array.inner : obj_type->slice.inner));
            emit(e, "){ &(");
            emit_expr(e, node->slice.object);
            if (obj_is_slice) emit(e, ".ptr");
            emit(e, ")[_zer_ss%d], _zer_se%d - _zer_ss%d }; })", sl_tmp, sl_tmp, sl_tmp);
        } else {
            /* normal path — no side effects in object */
            emit(e, "&(");
            emit_expr(e, node->slice.object);
            if (obj_is_slice) emit(e, ".ptr");
            emit(e, ")[");
            if (node->slice.start) {
                emit_expr(e, node->slice.start);
            } else {
                emit(e, "0");
            }
            emit(e, "], ");
            /* len = end - start */
            if (node->slice.end && node->slice.start) {
                emit(e, "(");
                emit_expr(e, node->slice.end);
                emit(e, ") - (");
                emit_expr(e, node->slice.start);
                emit(e, ")");
            } else if (node->slice.end) {
                emit_expr(e, node->slice.end);
            } else if (node->slice.start && obj_type && obj_type->kind == TYPE_ARRAY) {
                emit(e, "%llu - (", (unsigned long long)obj_type->array.size);
                emit_expr(e, node->slice.start);
                emit(e, ")");
            } else if (node->slice.start && obj_is_slice) {
                emit(e, "(");
                emit_expr(e, node->slice.object);
                emit(e, ").len - (");
                emit_expr(e, node->slice.start);
                emit(e, ")");
            } else {
                emit(e, "0 /* unknown len */");
            }
            emit(e, " })");
        }
        break;
    }

    case NODE_ORELSE: {
        /* Detect if the orelse expression is a pointer optional (?*T)
         * by checking the type from the checker's type map.
         * ?*T uses null sentinel → simple ternary
         * ?T uses struct → .has_value/.value */
        Type *orelse_type = checker_get_type(e->checker,node->orelse.expr);
        /* BUG-409: unwrap distinct — distinct typedef ?T is still optional */
        Type *orelse_eff = orelse_type ? type_unwrap_distinct(orelse_type) : NULL;
        bool is_ptr_optional = orelse_eff &&
            orelse_eff->kind == TYPE_OPTIONAL &&
            is_null_sentinel(orelse_eff->optional.inner);

        bool is_void_optional = is_void_opt(orelse_type);

        /* B3 refactor: all orelse paths share the opening temp pattern.
         * Use emit_opt_null_check/emit_opt_unwrap helpers for dispatch. */
        if (node->orelse.fallback_is_return || node->orelse.fallback_is_break ||
            node->orelse.fallback_is_continue) {
            /* orelse return/break/continue — check, emit defers + flow, unwrap */
            int tmp = e->temp_count++;
            emit(e, "({__typeof__(");
            emit_expr(e, node->orelse.expr);
            emit(e, ") _zer_tmp%d = ", tmp);
            emit_expr(e, node->orelse.expr);
            emit(e, "; if (");
            emit_opt_null_check(e, tmp, orelse_type);
            emit(e, ") { ");
            if (node->orelse.fallback_is_return) {
                emit_defers(e);
                emit_return_null(e);
            } else if (node->orelse.fallback_is_break) {
                emit_defers_from(e, e->loop_defer_base);
                emit(e, "break; ");
            } else {
                emit_defers_from(e, e->loop_defer_base);
                emit(e, "continue; ");
            }
            emit(e, "} ");
            emit_opt_unwrap(e, tmp, orelse_type);
            emit(e, "; })");
        } else if (node->orelse.fallback &&
                   node->orelse.fallback->kind == NODE_BLOCK) {
            /* orelse { block } — check, emit block, unwrap */
            int tmp = e->temp_count++;
            emit(e, "({__typeof__(");
            emit_expr(e, node->orelse.expr);
            emit(e, ") _zer_tmp%d = ", tmp);
            emit_expr(e, node->orelse.expr);
            emit(e, "; if (");
            emit_opt_null_check(e, tmp, orelse_type);
            emit(e, ") ");
            emit_stmt(e, node->orelse.fallback);
            emit(e, " ");
            emit_opt_unwrap(e, tmp, orelse_type);
            emit(e, "; })");
        } else {
            /* orelse default_value — ternary */
            int tmp = e->temp_count++;
            emit(e, "({__typeof__(");
            emit_expr(e, node->orelse.expr);
            emit(e, ") _zer_tmp%d = ", tmp);
            emit_expr(e, node->orelse.expr);
            if (is_ptr_optional) {
                emit(e, "; _zer_tmp%d ? _zer_tmp%d : ", tmp, tmp);
            } else if (is_void_optional) {
                emit(e, "; _zer_tmp%d.has_value ? (void)0 : ", tmp);
            } else {
                emit(e, "; _zer_tmp%d.has_value ? _zer_tmp%d.value : ", tmp, tmp);
            }
            if (node->orelse.fallback) {
                emit_expr(e, node->orelse.fallback);
            } else {
                emit(e, "0");
            }
            emit(e, "; })");
        }
        break;
    }

    case NODE_TYPECAST: {
        /* (Type)expr — emit as C cast for primitives.
         * For *opaque round-trips, emit the _zer_opaque unwrap/wrap. */
        Type *tgt = checker_get_type(e->checker, node);
        Type *src = checker_get_type(e->checker, node->typecast.expr);
        Type *tgt_eff = tgt ? type_unwrap_distinct(tgt) : NULL;
        Type *src_eff = src ? type_unwrap_distinct(src) : NULL;

        /* pointer ↔ *opaque: use _zer_opaque wrap/unwrap (same as @ptrcast) */
        if (tgt_eff && src_eff &&
            tgt_eff->kind == TYPE_POINTER && tgt_eff->pointer.inner &&
            type_unwrap_distinct(tgt_eff->pointer.inner)->kind == TYPE_OPAQUE &&
            src_eff->kind == TYPE_POINTER) {
            /* casting TO *opaque — wrap with type_id */
            uint32_t tid = 0;
            if (src_eff->pointer.inner) {
                Type *inner = type_unwrap_distinct(src_eff->pointer.inner);
                if (inner->kind == TYPE_STRUCT) tid = inner->struct_type.type_id;
                else if (inner->kind == TYPE_ENUM) tid = inner->enum_type.type_id;
                else if (inner->kind == TYPE_UNION) tid = inner->union_type.type_id;
            }
            emit(e, "(_zer_opaque){(void*)(");
            emit_expr(e, node->typecast.expr);
            emit(e, "), %u}", (unsigned)tid);
        } else if (tgt_eff && src_eff &&
                   tgt_eff->kind == TYPE_POINTER &&
                   ((src_eff->kind == TYPE_POINTER && src_eff->pointer.inner &&
                     type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_OPAQUE) ||
                    src_eff->kind == TYPE_OPAQUE)) {
            /* casting FROM *opaque — unwrap .ptr with type check */
            uint32_t expected_tid = 0;
            if (tgt_eff->pointer.inner) {
                Type *inner = type_unwrap_distinct(tgt_eff->pointer.inner);
                if (inner->kind == TYPE_STRUCT) expected_tid = inner->struct_type.type_id;
                else if (inner->kind == TYPE_ENUM) expected_tid = inner->enum_type.type_id;
                else if (inner->kind == TYPE_UNION) expected_tid = inner->union_type.type_id;
            }
            if (expected_tid > 0) {
                int tmp = e->temp_count++;
                emit(e, "({ _zer_opaque _zer_pc%d = ", tmp);
                emit_expr(e, node->typecast.expr);
                emit(e, "; if (_zer_pc%d.type_id != %u && _zer_pc%d.type_id != 0) "
                     "_zer_trap(\"type mismatch in cast\", __FILE__, __LINE__); "
                     "(", tmp, (unsigned)expected_tid, tmp);
                emit_type(e, tgt);
                emit(e, ")_zer_pc%d.ptr; })", tmp);
            } else {
                emit(e, "((");
                emit_type(e, tgt);
                emit(e, ")(");
                emit_expr(e, node->typecast.expr);
                emit(e, ").ptr)");
            }
        } else {
            /* Simple C cast for primitives, pointer↔pointer, int↔ptr */
            emit(e, "((");
            emit_type(e, tgt);
            emit(e, ")(");
            emit_expr(e, node->typecast.expr);
            emit(e, "))");
        }
        break;
    }

    case NODE_STRUCT_INIT: {
        /* Designated initializer: emit as C99 compound literal (Type){ .x = 1 }
         * Works in both var-decl init and assignment contexts. */
        Type *si_type = checker_get_type(e->checker, node);
        if (si_type) {
            emit(e, "(");
            emit_type(e, si_type);
            emit(e, ")");
        }
        emit(e, "{ ");
        for (int i = 0; i < node->struct_init.field_count; i++) {
            if (i > 0) emit(e, ", ");
            emit(e, ".%.*s = ", (int)node->struct_init.fields[i].name_len,
                 node->struct_init.fields[i].name);
            emit_expr(e, node->struct_init.fields[i].value);
        }
        emit(e, " }");
        break;
    }

    case NODE_INTRINSIC: {
        const char *name = node->intrinsic.name;
        uint32_t nlen = (uint32_t)node->intrinsic.name_len;

        if (nlen == 4 && memcmp(name, "size", 4) == 0) {
            /* @size(T) → sizeof(T) */
            emit(e, "sizeof(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                emit_type(e, t);
            } else if (node->intrinsic.arg_count > 0 &&
                       node->intrinsic.args[0]->kind == NODE_IDENT) {
                /* named type passed as identifier (e.g. @size(MyStruct)) */
                Symbol *sym = scope_lookup(e->checker->global_scope,
                    node->intrinsic.args[0]->ident.name,
                    (uint32_t)node->intrinsic.args[0]->ident.name_len);
                if (sym && sym->type) emit_type(e, sym->type);
            }
            emit(e, ")");
        } else if (nlen == 6 && memcmp(name, "offset", 6) == 0) {
            /* @offset(T, field) → offsetof(struct _zer_T, field)
             * Parser puts T as type_arg if it's a keyword type,
             * or as args[0] if it's a named type (identifier). */
            emit(e, "offsetof(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                emit_type(e, t);
                emit(e, ", ");
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
            } else if (node->intrinsic.arg_count >= 2) {
                /* args[0] = type name, args[1] = field name */
                emit(e, "struct ");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ", ");
                emit_expr(e, node->intrinsic.args[1]);
            }
            emit(e, ")");
        } else if (nlen == 7 && memcmp(name, "ptrcast", 7) == 0) {
            /* BUG-393: @ptrcast with runtime type tags for *opaque */
            Type *tgt_type = node->intrinsic.type_arg ?
                resolve_tynode(e, node->intrinsic.type_arg) : NULL;
            Type *src_type = (node->intrinsic.arg_count > 0) ?
                checker_get_type(e->checker, node->intrinsic.args[0]) : NULL;
            Type *tgt_eff = tgt_type ? type_unwrap_distinct(tgt_type) : NULL;
            Type *src_eff = src_type ? type_unwrap_distinct(src_type) : NULL;

            /* Level 3+4+5: check alive before any @ptrcast from *opaque */
            bool _ptrcast_track = false;
            if (e->track_cptrs && src_eff &&
                ((src_eff->kind == TYPE_POINTER && src_eff->pointer.inner &&
                  type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_OPAQUE) ||
                 src_eff->kind == TYPE_OPAQUE) &&
                node->intrinsic.arg_count > 0 &&
                node->intrinsic.args[0]->kind == NODE_IDENT) {
                /* BUG-431: ctx is _zer_opaque struct, use .ptr not (void*)ctx */
                emit(e, "(_zer_check_alive(%.*s.ptr, __FILE__, __LINE__), ",
                     (int)node->intrinsic.args[0]->ident.name_len,
                     node->intrinsic.args[0]->ident.name);
                _ptrcast_track = true;
            }

            if (tgt_eff && tgt_eff->kind == TYPE_POINTER &&
                tgt_eff->pointer.inner && type_unwrap_distinct(tgt_eff->pointer.inner)->kind == TYPE_OPAQUE) {
                /* casting TO *opaque — wrap with type_id */
                /* determine source type's ID */
                uint32_t tid = 0;
                if (src_eff && src_eff->kind == TYPE_POINTER && src_eff->pointer.inner) {
                    Type *inner = type_unwrap_distinct(src_eff->pointer.inner);
                    if (inner->kind == TYPE_STRUCT) tid = inner->struct_type.type_id;
                    else if (inner->kind == TYPE_ENUM) tid = inner->enum_type.type_id;
                    else if (inner->kind == TYPE_UNION) tid = inner->union_type.type_id;
                }
                emit(e, "(_zer_opaque){(void*)(");
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
                emit(e, "), %u}", (unsigned)tid);
            } else if (src_eff && src_eff->kind == TYPE_POINTER &&
                       src_eff->pointer.inner &&
                       type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_OPAQUE) {
                /* casting FROM *opaque — check type_id + unwrap .ptr */
                uint32_t expected_tid = 0;
                if (tgt_eff && tgt_eff->kind == TYPE_POINTER && tgt_eff->pointer.inner) {
                    Type *inner = type_unwrap_distinct(tgt_eff->pointer.inner);
                    if (inner->kind == TYPE_STRUCT) expected_tid = inner->struct_type.type_id;
                    else if (inner->kind == TYPE_ENUM) expected_tid = inner->enum_type.type_id;
                    else if (inner->kind == TYPE_UNION) expected_tid = inner->union_type.type_id;
                }
                if (expected_tid > 0) {
                    int tmp = e->temp_count++;
                    emit(e, "({ _zer_opaque _zer_pc%d = ", tmp);
                    if (node->intrinsic.arg_count > 0)
                        emit_expr(e, node->intrinsic.args[0]);
                    emit(e, "; if (_zer_pc%d.type_id != %u && _zer_pc%d.type_id != 0) ",
                         tmp, (unsigned)expected_tid, tmp);
                    emit(e, "_zer_trap(\"@ptrcast type mismatch\", __FILE__, __LINE__); (");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")_zer_pc%d.ptr; })", tmp);
                } else {
                    /* target is primitive pointer or unknown — just unwrap .ptr */
                    emit(e, "(");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")(");
                    if (node->intrinsic.arg_count > 0)
                        emit_expr(e, node->intrinsic.args[0]);
                    emit(e, ").ptr");
                }
            } else {
                /* neither side is *opaque — plain cast */
                emit(e, "(");
                if (tgt_type) emit_type(e, tgt_type);
                emit(e, ")(");
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
                emit(e, ")");
            }
            if (_ptrcast_track) emit(e, ")"); /* close comma expr from check_alive */
        } else if (nlen == 7 && memcmp(name, "bitcast", 7) == 0) {
            /* @bitcast(T, val) → memcpy type punning (valid C99+GCC) */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                int tmp2 = e->temp_count++;
                emit(e, "({__auto_type _zer_bci%d = ", tmp2);
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
                emit(e, "; ");
                emit_type(e, t);
                emit(e, " _zer_bco%d; memcpy(&_zer_bco%d, &_zer_bci%d, sizeof(_zer_bco%d)); _zer_bco%d; })",
                     tmp, tmp, tmp2, tmp, tmp);
            } else {
                emit(e, "0");
            }
        } else if (nlen == 8 && memcmp(name, "truncate", 8) == 0) {
            /* @truncate(val) → (T)(val) */
            emit(e, "(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
        } else if (nlen == 8 && memcmp(name, "saturate", 8) == 0) {
            /* @saturate(T, val) → clamp val to T's min/max range */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                emit(e, "({__auto_type _zer_sat%d = ", tmp);
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
                emit(e, "; ");
                /* clamp: min(max(val, TYPE_MIN), TYPE_MAX) */
                /* for unsigned targets, just clamp to max */
                if (type_is_unsigned(t)) {
                    /* unsigned: clamp to [0, max] — check both bounds for signed source */
                    int w = type_width(t);
                    if (w == 8) emit(e, "_zer_sat%d < 0 ? 0 : _zer_sat%d > 255 ? 255 : (uint8_t)_zer_sat%d", tmp, tmp, tmp);
                    else if (w == 16) emit(e, "_zer_sat%d < 0 ? 0 : _zer_sat%d > 65535 ? 65535 : (uint16_t)_zer_sat%d", tmp, tmp, tmp);
                    else if (w == 32) emit(e, "_zer_sat%d < 0 ? 0 : _zer_sat%d > 4294967295ULL ? 4294967295U : (uint32_t)_zer_sat%d", tmp, tmp, tmp);
                    /* BUG-308: u64 needs upper bound check (f64 can exceed UINT64_MAX) */
                    else emit(e, "_zer_sat%d < 0 ? 0 : _zer_sat%d > 18446744073709551615.0 ? 18446744073709551615ULL : (uint64_t)_zer_sat%d", tmp, tmp, tmp);
                } else {
                    /* signed: clamp to [min, max] for target width */
                    int w = type_width(t);
                    if (w == 8)
                        emit(e, "_zer_sat%d < -128 ? -128 : _zer_sat%d > 127 ? 127 : (int8_t)_zer_sat%d", tmp, tmp, tmp);
                    else if (w == 16)
                        emit(e, "_zer_sat%d < -32768 ? -32768 : _zer_sat%d > 32767 ? 32767 : (int16_t)_zer_sat%d", tmp, tmp, tmp);
                    else if (w == 32)
                        emit(e, "_zer_sat%d < -2147483648LL ? -2147483648LL : _zer_sat%d > 2147483647LL ? 2147483647LL : (int32_t)_zer_sat%d", tmp, tmp, tmp);
                    else
                        emit(e, "(int64_t)_zer_sat%d", tmp);
                }
                emit(e, "; })");
            } else {
                emit(e, "0");
            }
        } else if (nlen == 8 && memcmp(name, "inttoptr", 8) == 0) {
            /* @inttoptr(*T, addr) → (T*)(uintptr_t)(addr)
             * With mmio ranges: variable addresses get runtime range check
             * Auto-discovery removed (2026-04-01) — --no-strict-mmio with no
             * mmio declarations just emits plain cast (like C, programmer's choice) */
            bool need_runtime_check = e->checker->mmio_range_count > 0 &&
                node->intrinsic.arg_count > 0 &&
                node->intrinsic.args[0]->kind != NODE_INT_LIT;
            if (need_runtime_check) {
                int tmp = e->temp_count++;
                emit(e, "({ uintptr_t _zer_ma%d = (uintptr_t)(", tmp);
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "); if (!(");
                for (int ri = 0; ri < e->checker->mmio_range_count; ri++) {
                    if (ri > 0) emit(e, " || ");
                    emit(e, "(_zer_ma%d >= 0x%llxULL && _zer_ma%d <= 0x%llxULL)",
                         tmp, (unsigned long long)e->checker->mmio_ranges[ri][0],
                         tmp, (unsigned long long)e->checker->mmio_ranges[ri][1]);
                }
                emit(e, ")) _zer_trap(\"@inttoptr: address outside mmio range\", __FILE__, __LINE__); ");
                /* BUG-489: runtime alignment check — variable addresses must be
                 * aligned to target type. Constant addresses checked at compile time,
                 * but runtime addresses (0x40000000 + offset) need runtime check. */
                if (node->intrinsic.type_arg) {
                    Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                    Type *inner = t ? type_unwrap_distinct(t) : NULL;
                    if (inner && inner->kind == TYPE_POINTER) inner = inner->pointer.inner;
                    int align = inner ? type_width(inner) / 8 : 0;
                    if (align > 1) {
                        emit(e, "if (_zer_ma%d %% %d != 0) _zer_trap(\"@inttoptr: unaligned address\", __FILE__, __LINE__); ",
                             tmp, align);
                    }
                }
                emit(e, "(");
                if (node->intrinsic.type_arg) {
                    Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                    emit_type(e, t);
                }
                emit(e, ")_zer_ma%d; })", tmp);
            } else {
                emit(e, "(");
                if (node->intrinsic.type_arg) {
                    Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                    emit_type(e, t);
                }
                emit(e, ")(uintptr_t)(");
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
                emit(e, ")");
            }
        } else if (nlen == 8 && memcmp(name, "ptrtoint", 8) == 0) {
            /* @ptrtoint(ptr) → (uintptr_t)(ptr) */
            emit(e, "(uintptr_t)(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
        } else if (nlen == 7 && memcmp(name, "barrier", 7) == 0) {
            emit(e, "__atomic_thread_fence(__ATOMIC_SEQ_CST)");
        } else if (nlen == 13 && memcmp(name, "barrier_store", 13) == 0) {
            emit(e, "__atomic_thread_fence(__ATOMIC_RELEASE)");
        } else if (nlen == 12 && memcmp(name, "barrier_load", 12) == 0) {
            emit(e, "__atomic_thread_fence(__ATOMIC_ACQUIRE)");
        } else if (nlen == 4 && memcmp(name, "trap", 4) == 0) {
            emit(e, "_zer_trap(\"explicit trap\", __FILE__, __LINE__)");
        } else if (nlen == 5 && memcmp(name, "probe", 5) == 0) {
            emit(e, "_zer_probe((uintptr_t)(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, "))");
        } else if (nlen >= 10 && memcmp(name, "atomic_", 7) == 0) {
            /* @atomic_add/sub/or/and/xor/load/store/cas — dual-path emission */
            const char *op = name + 7;
            int oplen = nlen - 7;
            bool is_load = (oplen == 4 && memcmp(op, "load", 4) == 0);
            bool is_store = (oplen == 5 && memcmp(op, "store", 5) == 0);
            bool is_cas = (oplen == 3 && memcmp(op, "cas", 3) == 0);
            /* map op name to GCC __atomic builtin suffix */
            const char *gcc_op = NULL;
            if (oplen == 3 && memcmp(op, "add", 3) == 0) gcc_op = "add";
            if (oplen == 3 && memcmp(op, "sub", 3) == 0) gcc_op = "sub";
            if (oplen == 2 && memcmp(op, "or", 2) == 0) gcc_op = "or";
            if (oplen == 3 && memcmp(op, "and", 3) == 0) gcc_op = "and";
            if (oplen == 3 && memcmp(op, "xor", 3) == 0) gcc_op = "xor";

            if (is_load) {
                emit(e, "__atomic_load_n(");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ", __ATOMIC_SEQ_CST)");
            } else if (is_store) {
                emit(e, "__atomic_store_n(");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ", ");
                emit_expr(e, node->intrinsic.args[1]);
                emit(e, ", __ATOMIC_SEQ_CST)");
            } else if (is_cas) {
                /* BUG-428: __atomic_compare_exchange_n needs &expected as lvalue.
                 * Hoist expected into temp to handle literal args like @atomic_cas(&x, 0, 1). */
                emit(e, "({ __typeof__(*(");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ")) _zer_cas_exp = ");
                emit_expr(e, node->intrinsic.args[1]);
                emit(e, "; __atomic_compare_exchange_n(");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ", &_zer_cas_exp, ");
                emit_expr(e, node->intrinsic.args[2]);
                emit(e, ", 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })");
            } else if (gcc_op) {
                emit(e, "__atomic_fetch_%s(", gcc_op);
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ", ");
                emit_expr(e, node->intrinsic.args[1]);
                emit(e, ", __ATOMIC_SEQ_CST)");
            }
        } else if (nlen == 9 && memcmp(name, "container", 9) == 0) {
            /* @container(*T, ptr, field) → (T*)((char*)(ptr) - offsetof(T, field))
             * BUG-381: propagate volatile from source pointer to result */
            emit(e, "((");
            /* check if source expression is volatile */
            if (node->intrinsic.arg_count > 0 &&
                expr_is_volatile(e, node->intrinsic.args[0])) {
                emit(e, "volatile ");
            }
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")((char*)(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ") - offsetof(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                /* need the struct type without pointer */
                if (t->kind == TYPE_POINTER)
                    emit_type(e, t->pointer.inner);
                else
                    emit_type(e, t);
            }
            emit(e, ", ");
            if (node->intrinsic.arg_count > 1)
                emit_expr(e, node->intrinsic.args[1]);
            emit(e, ")))");
        } else if (nlen == 6 && memcmp(name, "config", 6) == 0) {
            /* @config(key, default) → emit the default value */
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[node->intrinsic.arg_count - 1]);
            else
                emit(e, "0");
        } else if (nlen == 4 && memcmp(name, "cstr", 4) == 0) {
            /* @cstr(buf, slice) → memcpy + null terminate
             * Hoist both args to temps for single-eval */
            int tmp = e->temp_count++;
            Type *buf_type = (node->intrinsic.arg_count > 0) ?
                checker_get_type(e->checker,node->intrinsic.args[0]) : NULL;
            Type *buf_eff = buf_type ? type_unwrap_distinct(buf_type) : NULL;
            bool dest_is_slice = buf_eff && buf_eff->kind == TYPE_SLICE;
            /* BUG-223/RF7: check if destination is volatile — walk field/index chains */
            /* RF11: use shared volatile detection helper */
            /* BUG-384: also check source volatility — memcpy strips volatile reads */
            bool dest_volatile = (node->intrinsic.arg_count > 0) ?
                expr_is_volatile(e, node->intrinsic.args[0]) : false;
            bool src_volatile = (node->intrinsic.arg_count > 1) ?
                expr_is_volatile(e, node->intrinsic.args[1]) : false;
            bool any_volatile = dest_volatile || src_volatile;
            const char *vol = dest_volatile ? "volatile " : "";
            if (dest_is_slice) {
                /* BUG-209: slice destination — hoist as slice, use .ptr */
                emit(e, "({ __auto_type _zer_cd%d = ", tmp);
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "; %suint8_t *_zer_cb%d = (%suint8_t*)_zer_cd%d.ptr", vol, tmp, vol, tmp);
            } else {
                emit(e, "({ %suint8_t *_zer_cb%d = (%suint8_t*)", vol, tmp, vol);
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
            }
            emit(e, "; __auto_type _zer_cs%d = ", tmp);
            if (node->intrinsic.arg_count > 1)
                emit_expr(e, node->intrinsic.args[1]);
            if (buf_eff && buf_eff->kind == TYPE_ARRAY) {
                emit(e, "; if (_zer_cs%d.len + 1 > %llu) { ",
                     tmp, (unsigned long long)buf_eff->array.size);
                /* auto-orelse: return zero value instead of trap. Trap stays as comment. */
                if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                    emit_defers(e);
                    emit(e, "return ");
                    emit_zero_value(e, e->current_func_ret);
                    emit(e, "; } ");
                } else {
                    emit_defers(e);
                    emit(e, "return; } ");
                }
            } else if (dest_is_slice) {
                emit(e, "; if (_zer_cs%d.len + 1 > _zer_cd%d.len) { ", tmp, tmp);
                if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                    emit_defers(e);
                    emit(e, "return ");
                    emit_zero_value(e, e->current_func_ret);
                    emit(e, "; } ");
                } else {
                    emit_defers(e);
                    emit(e, "return; } ");
                }
            } else {
                emit(e, "; ");
            }
            if (any_volatile) {
                /* BUG-223/384: volatile byte-by-byte copy — memcpy strips volatile
                 * on both reads (source) and writes (destination).
                 * Cast source to volatile if source is volatile. */
                const char *src_vol = src_volatile ? "volatile " : "";
                emit(e, "{ %sconst uint8_t *_sv = (%sconst uint8_t*)_zer_cs%d.ptr; ",
                     src_vol, src_vol, tmp);
                emit(e, "for (size_t _i = 0; _i < _zer_cs%d.len; _i++) _zer_cb%d[_i] = _sv[_i]; } ",
                     tmp, tmp);
            } else {
                emit(e, "memcpy(_zer_cb%d, _zer_cs%d.ptr, _zer_cs%d.len); ", tmp, tmp, tmp);
            }
            emit(e, "_zer_cb%d[_zer_cs%d.len] = 0; _zer_cb%d; })", tmp, tmp, tmp);
        } else if (nlen == 4 && memcmp(name, "cast", 4) == 0) {
            /* @cast(T, val) — distinct typedef conversion, same underlying type */
            if (node->intrinsic.type_arg && node->intrinsic.arg_count > 0) {
                emit(e, "((");
                Type *tgt = resolve_tynode(e,node->intrinsic.type_arg);
                if (tgt) emit_type(e, tgt);
                emit(e, ")(");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "))");
            } else {
                emit(e, "0");
            }
        } else if (nlen >= 5 && memcmp(name, "cond_", 5) == 0) {
            /* @cond_wait(shared_var, condition) → pthread_cond_wait loop
             * @cond_signal(shared_var) → pthread_cond_signal
             * @cond_broadcast(shared_var) → pthread_cond_broadcast */
            const char *cop = name + 5;
            int coplen = nlen - 5;
            bool is_wait = (coplen == 4 && memcmp(cop, "wait", 4) == 0);
            bool is_timedwait = (coplen == 9 && memcmp(cop, "timedwait", 9) == 0);
            bool is_signal = (coplen == 6 && memcmp(cop, "signal", 6) == 0);

            if (is_timedwait && node->intrinsic.arg_count >= 3) {
                /* @cond_timedwait(var, cond, timeout_ms) → returns ?void (null=timeout) */
                Type *vt = checker_get_type(e->checker, node->intrinsic.args[0]);
                bool vp = (vt && type_unwrap_distinct(vt)->kind == TYPE_POINTER);
                const char *ar = vp ? "->" : ".";
                int tmp = e->temp_count++;
                emit(e, "({ struct timespec _zer_ts%d; clock_gettime(CLOCK_REALTIME, &_zer_ts%d); ", tmp, tmp);
                emit(e, "{ uint64_t _zer_ms%d = ", tmp);
                emit_expr(e, node->intrinsic.args[2]);
                emit(e, "; _zer_ts%d.tv_sec += _zer_ms%d / 1000; ", tmp, tmp);
                emit(e, "_zer_ts%d.tv_nsec += (_zer_ms%d %% 1000) * 1000000L; ", tmp, tmp);
                emit(e, "if (_zer_ts%d.tv_nsec >= 1000000000L) { _zer_ts%d.tv_sec++; _zer_ts%d.tv_nsec -= 1000000000L; } } ", tmp, tmp, tmp);
                emit(e, "int _zer_twrc%d = 0; ", tmp);
                /* Refactor 3: unified ensure-init */
                emit_shared_ensure_init(e, node->intrinsic.args[0], ar);
                emit(e, "; ");
                emit(e, "pthread_mutex_lock(&");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_mtx); while (!(", ar);
                emit_expr(e, node->intrinsic.args[1]);
                emit(e, ")) { _zer_twrc%d = pthread_cond_timedwait(&", tmp);
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_cond, &", ar);
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_mtx, &_zer_ts%d); if (_zer_twrc%d) break; } ", ar, tmp, tmp);
                emit(e, "pthread_mutex_unlock(&");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_mtx); (_zer_opt_void){ .has_value = (_zer_twrc%d == 0) }; })", ar, tmp);
            } else if (is_wait && node->intrinsic.arg_count >= 2) {
                Type *vt = checker_get_type(e->checker, node->intrinsic.args[0]);
                bool vp = (vt && type_unwrap_distinct(vt)->kind == TYPE_POINTER);
                const char *ar = vp ? "->" : ".";
                /* Refactor 3: unified ensure-init */
                emit(e, "({ ");
                emit_shared_ensure_init(e, node->intrinsic.args[0], ar);
                emit(e, "; pthread_mutex_lock(&");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_mtx); while (!(", ar);
                emit_expr(e, node->intrinsic.args[1]);
                emit(e, ")) { pthread_cond_wait(&");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_cond, &", ar);
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_mtx); } pthread_mutex_unlock(&", ar);
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_mtx); (void)0; })", ar);
            } else if (is_signal && node->intrinsic.arg_count >= 1) {
                Type *vt = checker_get_type(e->checker, node->intrinsic.args[0]);
                bool vp = (vt && type_unwrap_distinct(vt)->kind == TYPE_POINTER);
                const char *ar = vp ? "->" : ".";
                /* Refactor 3: unified ensure-init */
                emit(e, "({ ");
                emit_shared_ensure_init(e, node->intrinsic.args[0], ar);
                emit(e, "; pthread_cond_signal(&");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_cond); (void)0; })", ar);
            } else if (node->intrinsic.arg_count >= 1) {
                /* broadcast */
                Type *vt = checker_get_type(e->checker, node->intrinsic.args[0]);
                bool vp = (vt && type_unwrap_distinct(vt)->kind == TYPE_POINTER);
                const char *ar = vp ? "->" : ".";
                /* Refactor 3: unified ensure-init */
                emit(e, "({ ");
                emit_shared_ensure_init(e, node->intrinsic.args[0], ar);
                emit(e, "; pthread_cond_broadcast(&");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, "%s_zer_cond); (void)0; })", ar);
            } else {
                emit(e, "/* @%.*s — missing args */0", (int)nlen, name);
            }
        } else if (nlen >= 8 && memcmp(name, "barrier_", 8) == 0) {
            const char *bop = name + 8;
            int boplen = nlen - 8;
            if (boplen == 4 && memcmp(bop, "init", 4) == 0 && node->intrinsic.arg_count >= 2) {
                /* @barrier_init(var, count) — &var if direct, var if pointer */
                Type *bt = checker_get_type(e->checker, node->intrinsic.args[0]);
                bool is_ptr = bt && type_unwrap_distinct(bt)->kind == TYPE_POINTER;
                emit(e, "_zer_barrier_init(");
                if (!is_ptr) emit(e, "&");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ", ");
                emit_expr(e, node->intrinsic.args[1]);
                emit(e, ")");
            } else if (boplen == 4 && memcmp(bop, "wait", 4) == 0 && node->intrinsic.arg_count >= 1) {
                Type *bt = checker_get_type(e->checker, node->intrinsic.args[0]);
                bool is_ptr = bt && type_unwrap_distinct(bt)->kind == TYPE_POINTER;
                emit(e, "_zer_barrier_wait(");
                if (!is_ptr) emit(e, "&");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ")");
            } else {
                emit(e, "/* @%.*s — missing args */0", (int)nlen, name);
            }
        } else if (nlen == 11 && memcmp(name, "sem_acquire", 11) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* @sem_acquire(s) → _zer_sem_acquire(&s) or _zer_sem_acquire(s) */
            Type *sat = checker_get_type(e->checker, node->intrinsic.args[0]);
            bool sa_ptr = sat && type_unwrap_distinct(sat)->kind == TYPE_POINTER;
            emit(e, "_zer_sem_acquire(");
            if (!sa_ptr) emit(e, "&");
            emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
        } else if (nlen == 11 && memcmp(name, "sem_release", 11) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* @sem_release(s) → _zer_sem_release(&s) or _zer_sem_release(s) */
            Type *srt = checker_get_type(e->checker, node->intrinsic.args[0]);
            bool sr_ptr = srt && type_unwrap_distinct(srt)->kind == TYPE_POINTER;
            emit(e, "_zer_sem_release(");
            if (!sr_ptr) emit(e, "&");
            emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
        } else {
            emit(e, "/* @%.*s — unknown */0", (int)nlen, name);
        }
        break;
    }

    default:
        emit(e, "/* unhandled expr %s */0", node_kind_name(node->kind));
        break;
    }
}

/* ================================================================
 * STATEMENT EMISSION
 * ================================================================ */

/* Bounds checks are now inline in emit_expr(NODE_INDEX) using the comma
 * operator: (_zer_bounds_check(idx, len, ...), arr)[idx].
 * This respects short-circuit (&&/||) and works in if/while/for conditions.
 * The old statement-level emit_bounds_checks() hoisting has been removed. */

/* emit all accumulated defers in reverse order */
/* emit defers from current count down to 'base' (exclusive) */
static void emit_defers_from(Emitter *e, int base) {
    for (int i = e->defer_stack.count - 1; i >= base; i--) {
        emit_stmt(e, e->defer_stack.stmts[i]);
    }
}

/* emit ALL defers (for return — must fire every scope's defers) */
static void emit_defers(Emitter *e) {
    emit_defers_from(e, 0);
}

static void emit_stmt(Emitter *e, Node *node) {
    if (!node) return;

    /* source mapping: #line directive maps GCC errors/traps to .zer file */
    if (e->source_file && node->loc.line > 0 && node->kind != NODE_BLOCK) {
        emit(e, "#line %d \"%s\"\n", node->loc.line, e->source_file);
    }

    switch (node->kind) {
    case NODE_BLOCK: {
        /* track where this block's defers start in the stack */
        int defer_base = e->defer_stack.count;

        emit(e, "{\n");
        e->indent++;
        for (int i = 0; i < node->block.stmt_count; i++) {
            /* Shared struct auto-locking: group consecutive statements
             * accessing the same shared variable under one lock scope. */
            Node *sr = find_shared_root_in_stmt(e, node->block.stmts[i]);
            if (sr) {
                /* For shared(rw): scan group to determine read vs write lock.
                 * If ANY statement in the group writes → wrlock. All reads → rdlock. */
                bool group_writes = stmt_writes_shared(node->block.stmts[i]);
                const char *sr_name = sr->ident.name;
                uint32_t sr_len = (uint32_t)sr->ident.name_len;
                /* Pre-scan the group for writes */
                for (int j = i + 1; j < node->block.stmt_count; j++) {
                    Node *jsr = find_shared_root_in_stmt(e, node->block.stmts[j]);
                    if (!jsr || jsr->kind != NODE_IDENT ||
                        (uint32_t)jsr->ident.name_len != sr_len ||
                        memcmp(jsr->ident.name, sr_name, sr_len) != 0)
                        break;
                    if (stmt_writes_shared(node->block.stmts[j]))
                        group_writes = true;
                }
                emit_shared_lock_mode(e, sr, group_writes);
                /* Emit this statement + scan ahead for consecutive same-root */
                while (i < node->block.stmt_count) {
                    emit_stmt(e, node->block.stmts[i]);
                    /* Check if NEXT statement also accesses same shared root */
                    if (i + 1 < node->block.stmt_count) {
                        Node *next_sr = find_shared_root_in_stmt(e, node->block.stmts[i + 1]);
                        if (next_sr && next_sr->kind == NODE_IDENT &&
                            (uint32_t)next_sr->ident.name_len == sr_len &&
                            memcmp(next_sr->ident.name, sr_name, sr_len) == 0) {
                            i++;
                            continue; /* same shared root — keep going */
                        }
                    }
                    break;
                }
                emit_shared_unlock(e, sr);
            } else {
                emit_stmt(e, node->block.stmts[i]);
            }
        }
        /* emit only THIS block's defers at block end */
        emit_defers_from(e, defer_base);
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");

        /* pop this block's defers off the stack */
        e->defer_stack.count = defer_base;
        break;
    }

    case NODE_VAR_DECL: {
        /* Async mode: locals are in the state struct — just assign, don't declare */
        if (e->in_async && is_async_local(e, node->var_decl.name, node->var_decl.name_len)) {
            if (node->var_decl.init) {
                emit_auto_guards(e, node->var_decl.init);

                /* BUG-481: orelse { block } in async — split into separate statements
                 * using state struct temp. Block may contain yield — stack temps stale. */
                if (node->var_decl.init->kind == NODE_ORELSE &&
                    node->var_decl.init->orelse.fallback &&
                    node->var_decl.init->orelse.fallback->kind == NODE_BLOCK) {
                    /* Refactor 2: unified async orelse emission */
                    if (emit_async_orelse_block(e, node->var_decl.init->orelse.expr,
                            node->var_decl.init->orelse.fallback,
                            node->var_decl.name, node->var_decl.name_len,
                            checker_get_type(e->checker, node)))
                        break;
                    /* fallthrough to generic emit_expr if no async temp */
                }

                emit_indent(e);
                emit(e, "self->%.*s = ", (int)node->var_decl.name_len, node->var_decl.name);
                emit_expr(e, node->var_decl.init);
                emit(e, ";\n");
            }
            break;
        }
        /* auto-guard: emit bounds guards before var init */
        if (node->var_decl.init) emit_auto_guards(e, node->var_decl.init);
        Type *type = checker_get_type(e->checker,node);
        /* propagate volatile flag from var-decl to pointer type */
        if (node->var_decl.is_volatile && type && type_unwrap_distinct(type)->kind == TYPE_POINTER) {
            Type *tp = type_unwrap_distinct(type);
            Type *vp = type_pointer(e->arena, tp->pointer.inner);
            vp->pointer.is_volatile = true;
            type = vp;
        }

        /* bounds checks now inline in emit_expr(NODE_INDEX) */

        /* Special case: u32 y = x orelse return;
         * → { auto _t = x; if (!_t.has_value) return; }
         *   uint32_t y = _t.value; */
        if (node->var_decl.init && node->var_decl.init->kind == NODE_ORELSE &&
            (node->var_decl.init->orelse.fallback_is_return ||
             node->var_decl.init->orelse.fallback_is_break ||
             node->var_decl.init->orelse.fallback_is_continue)) {
            Type *or_expr_type = checker_get_type(e->checker,node->var_decl.init->orelse.expr);
            /* BUG-409: unwrap distinct for optional dispatch */
            Type *or_eff = or_expr_type ? type_unwrap_distinct(or_expr_type) : NULL;
            bool or_is_ptr = or_eff &&
                or_eff->kind == TYPE_OPTIONAL &&
                is_null_sentinel(or_eff->optional.inner);

            /* BUG-319: use __typeof__ to preserve volatile qualifier */
            int tmp = e->temp_count++;
            emit_indent(e);
            emit(e, "__typeof__(");
            emit_expr(e, node->var_decl.init->orelse.expr);
            emit(e, ") _zer_or%d = ", tmp);
            emit_expr(e, node->var_decl.init->orelse.expr);
            emit(e, ";\n");
            emit_indent(e);
            if (or_is_ptr) {
                emit(e, "if (!_zer_or%d) ", tmp);
            } else {
                emit(e, "if (!_zer_or%d.has_value) ", tmp);
            }
            if (node->var_decl.init->orelse.fallback_is_return) {
                emit(e, "{\n"); emit_defers(e);
                emit_return_null(e);
                emit(e, "}\n");
            } else if (node->var_decl.init->orelse.fallback_is_break) {
                emit(e, "{\n"); emit_defers_from(e, e->loop_defer_base); emit(e, "break; }\n");
            } else {
                emit(e, "{\n"); emit_defers_from(e, e->loop_defer_base); emit(e, "continue; }\n");
            }
            emit_indent(e);
            if (or_is_ptr) {
                emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
                emit(e, " = _zer_or%d;\n", tmp);
            } else if (is_void_opt(type)) {
                /* ?void has no .value — keep as ?void (has_value only) */
                emit(e, "_zer_opt_void %.*s = _zer_or%d;\n",
                     (int)node->var_decl.name_len, node->var_decl.name, tmp);
            } else if (type && type_unwrap_distinct(type)->kind == TYPE_SLICE) {
                /* slice: use __auto_type to avoid anonymous struct incompatibility */
                emit(e, "__auto_type %.*s = _zer_or%d.value;\n",
                     (int)node->var_decl.name_len, node->var_decl.name, tmp);
            } else {
                emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
                emit(e, " = _zer_or%d.value;\n", tmp);
            }
            break;
        }

        /* BUG-481: async orelse { block } — Refactor 2: unified helper */
        if (e->in_async && node->var_decl.init &&
            node->var_decl.init->kind == NODE_ORELSE &&
            node->var_decl.init->orelse.fallback &&
            node->var_decl.init->orelse.fallback->kind == NODE_BLOCK) {
            if (emit_async_orelse_block(e, node->var_decl.init->orelse.expr,
                    node->var_decl.init->orelse.fallback,
                    node->var_decl.name, node->var_decl.name_len, type))
                break;
            /* fallthrough to normal path if no async temp available */
        }

        /* Normal var decl */
        emit_indent(e);
        if (node->var_decl.is_static) emit(e, "static ");
        if (node->var_decl.is_volatile && !(type && type_unwrap_distinct(type)->kind == TYPE_POINTER))
            emit(e, "volatile ");
        emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
        if (node->var_decl.init) {
            /* Optional init: null → {0, 0}, value → {val, 1}
             * But if init is a function call returning ?T, just assign directly.
             * BUG-506: unwrap distinct — distinct typedef ?T is still optional. */
            Type *type_eff_opt = type ? type_unwrap_distinct(type) : NULL;
            if (type_eff_opt && type_eff_opt->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(type_eff_opt->optional.inner)) {
                if (node->var_decl.init->kind == NODE_NULL_LIT) {
                    emit(e, " = ");
                    emit_opt_null_literal(e, type);
                } else if (node->var_decl.init->kind == NODE_CALL ||
                           node->var_decl.init->kind == NODE_ORELSE ||
                           node->var_decl.init->kind == NODE_INTRINSIC) {
                    /* call/orelse/intrinsic might already return ?T — assign directly.
                     * BUG-408: ?void from void function — hoist void call to statement,
                     * then init with { 1 }. Can't put void expression in initializer. */
                    Type *call_ret = checker_get_type(e->checker, node->var_decl.init);
                    if (call_ret && call_ret->kind == TYPE_VOID && is_void_opt(type)) {
                        emit(e, ";\n");
                        emit_indent(e);
                        emit_expr(e, node->var_decl.init);
                        emit(e, ";\n");
                        emit_indent(e);
                        emit(e, "%.*s = (_zer_opt_void){ 1 }",
                             (int)node->var_decl.name_len, node->var_decl.name);
                    } else {
                        emit(e, " = ");
                        emit_expr(e, node->var_decl.init);
                    }
                } else if (node->var_decl.init->kind == NODE_IDENT) {
                    /* check if ident is already ?T or needs wrapping.
                     * BUG-506: unwrap distinct — distinct ?T is still optional. */
                    Type *init_type = checker_get_type(e->checker,node->var_decl.init);
                    if (init_type && type_unwrap_distinct(init_type)->kind == TYPE_OPTIONAL) {
                        emit(e, " = ");
                        emit_expr(e, node->var_decl.init);
                    } else {
                        emit(e, " = ");
                        emit_opt_wrap_value(e, type, node->var_decl.init);
                    }
                } else {
                    /* check if init expression is already ?T (e.g. struct field of ?Handle type).
                     * BUG-506: unwrap distinct. */
                    Type *init_type = checker_get_type(e->checker,node->var_decl.init);
                    if (init_type && type_unwrap_distinct(init_type)->kind == TYPE_OPTIONAL) {
                        emit(e, " = ");
                        emit_expr(e, node->var_decl.init);
                    } else {
                        emit(e, " = ");
                        emit_opt_wrap_value(e, type, node->var_decl.init);
                    }
                }
            } else {
                /* array→slice coercion at var-decl */
                Type *init_type = checker_get_type(e->checker,node->var_decl.init);
                if (type && type_unwrap_distinct(type)->kind == TYPE_SLICE &&
                    init_type && type_unwrap_distinct(init_type)->kind == TYPE_ARRAY) {
                    emit(e, " = ");
                    emit_array_as_slice(e, node->var_decl.init, init_type, type);
                } else if (type && type_unwrap_distinct(type)->kind == TYPE_ARRAY &&
                           init_type && type_unwrap_distinct(init_type)->kind == TYPE_ARRAY) {
                    /* array = array: C arrays aren't assignable, use memcpy.
                     * BUG-278/320: volatile arrays (target OR source) use byte loop */
                    emit(e, " = {0};\n");
                    emit_indent(e);
                    bool vd_init_volatile = node->var_decl.is_volatile ||
                                            expr_is_volatile(e, node->var_decl.init);
                    if (vd_init_volatile) {
                        int tmp = e->temp_count++;
                        emit(e, "{ const volatile uint8_t *_zer_vs%d = (const volatile uint8_t*)&(", tmp);
                        emit_expr(e, node->var_decl.init);
                        emit(e, "); for (size_t _i = 0; _i < sizeof(%.*s); _i++) "
                             "((volatile uint8_t*)%.*s)[_i] = _zer_vs%d[_i]; }",
                             (int)node->var_decl.name_len, node->var_decl.name,
                             (int)node->var_decl.name_len, node->var_decl.name, tmp);
                    } else {
                        emit(e, "memmove(%.*s, ",
                             (int)node->var_decl.name_len, node->var_decl.name);
                        emit_expr(e, node->var_decl.init);
                        emit(e, ", sizeof(%.*s))",
                             (int)node->var_decl.name_len, node->var_decl.name);
                    }
                } else {
                    emit(e, " = ");
                    emit_expr(e, node->var_decl.init);
                }
            }
        } else {
            /* ZER auto-zeroes — unwrap distinct for compound init check */
            Type *eff_local = type_unwrap_distinct(type);
            if (eff_local && (eff_local->kind == TYPE_STRUCT || eff_local->kind == TYPE_ARRAY ||
                         eff_local->kind == TYPE_OPTIONAL || eff_local->kind == TYPE_UNION ||
                         eff_local->kind == TYPE_ARENA || eff_local->kind == TYPE_BARRIER ||
                         eff_local->kind == TYPE_SEMAPHORE || eff_local->kind == TYPE_SLICE)) {
                /* BUG-411: empty struct {0} warns — use {} for zero-field structs */
                if (eff_local->kind == TYPE_SEMAPHORE)
                    emit(e, " = { .count = %u }", (unsigned)eff_local->semaphore.count);
                else if (eff_local->kind == TYPE_STRUCT && eff_local->struct_type.field_count == 0)
                    emit(e, " = {}");
                else
                    emit(e, " = {0}");
            } else {
                emit(e, " = 0");
            }
        }
        emit(e, ";\n");
        break;
    }

    case NODE_IF:
        /* comptime if — only emit the taken branch */
        if (node->if_stmt.is_comptime) {
            int64_t cval = eval_const_expr(node->if_stmt.cond);
            if (cval) {
                if (node->if_stmt.then_body) emit_stmt(e, node->if_stmt.then_body);
            } else {
                if (node->if_stmt.else_body) emit_stmt(e, node->if_stmt.else_body);
            }
            break;
        }
        if (node->if_stmt.capture_name) {
            /* if-unwrap: if (maybe) |val| { ... }
             * |val|  → copy of unwrapped value (immutable)
             * |*val| → pointer to ORIGINAL optional's value (mutable)
             *
             * For |val|:  { auto _tmp = expr; if (_tmp.has_value) { auto val = _tmp.value; ... } }
             * For |*val|: { auto *_ptr = &expr; if (_ptr->has_value) { auto val = &_ptr->value; ... } }
             * ?*T (ptr): { auto _tmp = expr; if (_tmp) { auto val = _tmp; ... } } */
            int tmp = e->temp_count++;
            Type *cond_type = checker_get_type(e->checker,node->if_stmt.cond);
            /* BUG-506: unwrap distinct for null-sentinel detection */
            Type *cond_eff = cond_type ? type_unwrap_distinct(cond_type) : NULL;
            bool is_ptr_opt = cond_eff &&
                cond_eff->kind == TYPE_OPTIONAL &&
                is_null_sentinel(cond_eff->optional.inner);

            /* auto-guard: emit bounds guards before if-unwrap condition */
            emit_auto_guards(e, node->if_stmt.cond);
            emit_indent(e);
            emit(e, "{\n");
            e->indent++;

            if (node->if_stmt.capture_is_ptr && !is_ptr_opt) {
                /* |*val| on struct optional — need pointer to original.
                 * BUG-264: for rvalue expressions (NODE_CALL), hoist into temp
                 * to avoid &(rvalue). For lvalues, use &(expr) directly. */
                bool cond_is_rvalue = (node->if_stmt.cond->kind == NODE_CALL);
                /* BUG-292: preserve volatile on mutable capture pointer */
                bool cond_vol = expr_is_volatile(e, node->if_stmt.cond);
                emit_indent(e);
                if (cond_is_rvalue) {
                    if (cond_vol) emit(e, "volatile ");
                    emit_type(e, cond_type);
                    emit(e, " _zer_uwt%d = ", tmp);
                    emit_expr(e, node->if_stmt.cond);
                    emit(e, ";\n");
                    emit_indent(e);
                    if (cond_vol) emit(e, "volatile ");
                    emit_type(e, cond_type);
                    emit(e, " *_zer_uwp%d = &_zer_uwt%d;\n", tmp, tmp);
                } else {
                    if (cond_vol) emit(e, "volatile ");
                    emit_type(e, cond_type);
                    emit(e, " *_zer_uwp%d = &(", tmp);
                    emit_expr(e, node->if_stmt.cond);
                    emit(e, ");\n");
                }
                emit_indent(e);
                emit(e, "if (_zer_uwp%d->has_value) ", tmp);
                emit(e, "{\n");
                e->indent++;
                emit_indent(e);
                /* BUG-321/322: preserve volatile in capture pointer */
                if (cond_vol) emit(e, "volatile ");
                emit(e, "__typeof__(_zer_uwp%d->value) *%.*s = &_zer_uwp%d->value;\n",
                     tmp,
                     (int)node->if_stmt.capture_name_len,
                     node->if_stmt.capture_name, tmp);
            } else {
                /* |val| or ?*T — use copy.
                 * BUG-267/272: use explicit type to preserve volatile qualifier. */
                emit_indent(e);
                if (cond_type) {
                    /* BUG-272: preserve volatile from source (RF11 helper) */
                    if (expr_is_volatile(e, node->if_stmt.cond)) emit(e, "volatile ");
                    char uwname[32];
                    snprintf(uwname, sizeof(uwname), "_zer_uw%d", tmp);
                    emit_type_and_name(e, cond_type, uwname, strlen(uwname));
                    emit(e, " = ");
                } else {
                    emit(e, "__auto_type _zer_uw%d = ", tmp);
                }
                emit_expr(e, node->if_stmt.cond);
                emit(e, ";\n");
                emit_indent(e);
                if (is_ptr_opt) {
                    emit(e, "if (_zer_uw%d) ", tmp);
                } else {
                    emit(e, "if (_zer_uw%d.has_value) ", tmp);
                }
                emit(e, "{\n");
                e->indent++;
                emit_indent(e);
                if (is_ptr_opt) {
                    /* BUG-322: use __typeof__ to preserve volatile */
                    emit(e, "__typeof__(_zer_uw%d) %.*s = _zer_uw%d;\n",
                         tmp,
                         (int)node->if_stmt.capture_name_len,
                         node->if_stmt.capture_name, tmp);
                } else if (is_void_opt(cond_type)) {
                    /* ?void has no .value field — capture is just a dummy */
                    emit(e, "uint8_t %.*s = 1;\n",
                         (int)node->if_stmt.capture_name_len,
                         node->if_stmt.capture_name);
                } else {
                    /* BUG-322: use __typeof__ to preserve volatile */
                    emit(e, "__typeof__(_zer_uw%d.value) %.*s = _zer_uw%d.value;\n",
                         tmp,
                         (int)node->if_stmt.capture_name_len,
                         node->if_stmt.capture_name, tmp);
                }
            }
            /* emit then body contents (unwrap the block).
             * Track defer scope so defers inside this block fire at block exit,
             * not at function exit (BUG-102 fix). */
            int saved_defer_count = e->defer_stack.count;
            if (node->if_stmt.then_body->kind == NODE_BLOCK) {
                for (int i = 0; i < node->if_stmt.then_body->block.stmt_count; i++) {
                    emit_stmt(e, node->if_stmt.then_body->block.stmts[i]);
                }
            } else {
                emit_stmt(e, node->if_stmt.then_body);
            }
            /* fire defers accumulated inside this block */
            emit_defers_from(e, saved_defer_count);
            e->defer_stack.count = saved_defer_count;
            e->indent--;
            emit_indent(e);
            emit(e, "}\n");
            if (node->if_stmt.else_body) {
                emit_indent(e);
                /* BUG-418: same fix for if-unwrap else path */
                if (node->if_stmt.else_body->kind == NODE_IF && e->source_file) {
                    emit(e, "else\n");
                } else {
                    emit(e, "else ");
                }
                emit_stmt(e, node->if_stmt.else_body);
            }
            e->indent--;
            emit_indent(e);
            emit(e, "}\n");
        } else {
            /* regular if */
            Type *cond_t = checker_get_type(e->checker,node->if_stmt.cond);
            /* BUG-409: unwrap distinct for optional condition check */
            Type *cond_t_eff = cond_t ? type_unwrap_distinct(cond_t) : NULL;
            bool cond_is_struct_opt = cond_t_eff &&
                cond_t_eff->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(cond_t_eff->optional.inner);
            /* auto-guard: emit bounds guards before if condition */
            emit_auto_guards(e, node->if_stmt.cond);
            emit_indent(e);
            emit(e, "if (");
            emit_expr(e, node->if_stmt.cond);
            if (cond_is_struct_opt) emit(e, ".has_value");
            emit(e, ") ");
            emit_stmt(e, node->if_stmt.then_body);
            if (node->if_stmt.else_body) {
                emit_indent(e);
                /* BUG-418: else-if chain — emit_stmt emits #line which must
                 * start at beginning of a line. Without newline after "else",
                 * GCC gets "stray '#' in program" error. */
                if (node->if_stmt.else_body->kind == NODE_IF && e->source_file) {
                    emit(e, "else\n");
                } else {
                    emit(e, "else ");
                }
                emit_stmt(e, node->if_stmt.else_body);
            }
        }
        break;

    case NODE_FOR: {
        int saved_loop_base = e->loop_defer_base;
        e->loop_defer_base = e->defer_stack.count;
        emit_indent(e);
        emit(e, "for (");
        if (node->for_stmt.init) {
            if (node->for_stmt.init->kind == NODE_VAR_DECL) {
                Type *type = checker_get_type(e->checker,node->for_stmt.init);
                emit_type_and_name(e, type,
                    node->for_stmt.init->var_decl.name,
                    node->for_stmt.init->var_decl.name_len);
                if (node->for_stmt.init->var_decl.init) {
                    emit(e, " = ");
                    emit_expr(e, node->for_stmt.init->var_decl.init);
                }
            } else {
                emit_expr(e, node->for_stmt.init);
            }
        }
        emit(e, "; ");
        if (node->for_stmt.cond) emit_expr(e, node->for_stmt.cond);
        emit(e, "; ");
        if (node->for_stmt.step) emit_expr(e, node->for_stmt.step);
        emit(e, ") ");
        emit_stmt(e, node->for_stmt.body);
        e->loop_defer_base = saved_loop_base;
        break;
    }

    case NODE_WHILE: {
        int saved_loop_base = e->loop_defer_base;
        e->loop_defer_base = e->defer_stack.count;
        Type *while_cond_t = checker_get_type(e->checker,node->while_stmt.cond);
        /* BUG-409: unwrap distinct for optional while condition */
        Type *while_eff = while_cond_t ? type_unwrap_distinct(while_cond_t) : NULL;
        bool while_is_struct_opt = while_eff &&
            while_eff->kind == TYPE_OPTIONAL &&
            !is_null_sentinel(while_eff->optional.inner);
        emit_indent(e);
        emit(e, "while (");
        emit_expr(e, node->while_stmt.cond);
        if (while_is_struct_opt) emit(e, ".has_value");
        emit(e, ") ");
        emit_stmt(e, node->while_stmt.body);
        e->loop_defer_base = saved_loop_base;
        break;
    }

    case NODE_DO_WHILE: {
        int saved_loop_base = e->loop_defer_base;
        e->loop_defer_base = e->defer_stack.count;
        emit_indent(e);
        emit(e, "do ");
        emit_stmt(e, node->while_stmt.body);
        emit_indent(e);
        emit(e, "while (");
        emit_expr(e, node->while_stmt.cond);
        emit(e, ");\n");
        e->loop_defer_base = saved_loop_base;
        break;
    }

    case NODE_RETURN: {
        /* BUG-409: unwrap distinct on return type for optional dispatch */
        Type *ret_eff = e->current_func_ret ? type_unwrap_distinct(e->current_func_ret) : NULL;
        /* auto-guard: emit bounds guards before return expression */
        if (node->ret.expr) emit_auto_guards(e, node->ret.expr);
        /* BUG-442: if there are pending defers AND a return expression that
         * could be affected by defer side effects (function calls, field access),
         * hoist the return value into a temp BEFORE firing defers.
         * Otherwise defer free() runs before return value is computed → UAF.
         * Use the function's return type for the temp to preserve ?T wrapping. */
        if (node->ret.expr && e->defer_stack.count > 0 &&
            node->ret.expr->kind != NODE_NULL_LIT &&
            node->ret.expr->kind != NODE_INT_LIT &&
            node->ret.expr->kind != NODE_BOOL_LIT &&
            node->ret.expr->kind != NODE_FLOAT_LIT) {
            int ret_tmp = e->temp_count++;
            emit_indent(e);
            /* emit full return type for the temp */
            if (e->current_func_ret) {
                emit_type(e, e->current_func_ret);
            } else {
                emit(e, "__auto_type");
            }
            emit(e, " _zer_ret%d = ", ret_tmp);
            /* Check if wrapping is needed (?T from non-optional expr) */
            Type *expr_type = checker_get_type(e->checker, node->ret.expr);
            if (ret_eff && ret_eff->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(ret_eff->optional.inner) &&
                expr_type && !type_equals(expr_type, e->current_func_ret)) {
                /* B7: use emit_opt_wrap_value for wrapping */
                if (is_void_opt(e->current_func_ret)) {
                    emit_expr(e, node->ret.expr);
                    emit(e, ";\n");
                    emit_indent(e);
                    emit(e, "_zer_ret%d = (_zer_opt_void){ 1 };\n", ret_tmp);
                } else {
                    emit_opt_wrap_value(e, e->current_func_ret, node->ret.expr);
                    emit(e, ";\n");
                }
            } else {
                emit_expr(e, node->ret.expr);
                emit(e, ";\n");
            }
            emit_defers(e);
            emit_indent(e);
            emit(e, "return _zer_ret%d;\n", ret_tmp);
            break;
        }
        /* emit defers before return (reverse order) */
        emit_defers(e);
        emit_indent(e);
        if (node->ret.expr) {
            /* return null from ?T function → return {0, 0} (or {0} for ?void)
             * BUG-409: unwrap distinct on return type — distinct ?T is still optional */
            if (ret_eff && ret_eff->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(ret_eff->optional.inner) &&
                node->ret.expr->kind == NODE_NULL_LIT) {
                emit(e, "return ");
                emit_opt_null_literal(e, e->current_func_ret);
                emit(e, ";\n");
            }
            /* return value from ?T function → return {value, 1} */
            else if (ret_eff && ret_eff->kind == TYPE_OPTIONAL &&
                     !is_null_sentinel(ret_eff->optional.inner) &&
                     node->ret.expr->kind != NODE_NULL_LIT) {
                /* check if expr already has the optional type (e.g. return ring.pop()) */
                Type *expr_type = checker_get_type(e->checker,node->ret.expr);
                if (expr_type && type_equals(expr_type, e->current_func_ret)) {
                    /* already optional — return directly */
                    emit(e, "return ");
                    emit_expr(e, node->ret.expr);
                    emit(e, ";\n");
                /* B7: use emit_opt_wrap_value for wrapping */
                } else if (is_void_opt(e->current_func_ret)) {
                    emit_expr(e, node->ret.expr);
                    emit(e, ";\n");
                    emit_indent(e);
                    emit(e, "return (_zer_opt_void){ 1 };\n");
                } else {
                    emit(e, "return ");
                    emit_opt_wrap_value(e, e->current_func_ret, node->ret.expr);
                    emit(e, ";\n");
                }
            } else {
                /* array→slice coercion on return */
                Type *expr_type = checker_get_type(e->checker,node->ret.expr);
                if (ret_eff && ret_eff->kind == TYPE_SLICE &&
                    expr_type && type_unwrap_distinct(expr_type)->kind == TYPE_ARRAY) {
                    emit(e, "return ");
                    emit_array_as_slice(e, node->ret.expr, expr_type, e->current_func_ret);
                    emit(e, ";\n");
                } else {
                    emit(e, "return ");
                    emit_expr(e, node->ret.expr);
                    emit(e, ";\n");
                }
            }
        } else {
            /* bare return — for ?void, return {1} (success); for other ?T, return {0,1}
             * For null-sentinel types (?*T, ?FuncPtr), bare return = return NULL (none)
             * BUG-409: unwrap distinct on return type */
            Type *bare_ret = e->current_func_ret ? type_unwrap_distinct(e->current_func_ret) : NULL;
            if (bare_ret && bare_ret->kind == TYPE_OPTIONAL &&
                is_null_sentinel(bare_ret->optional.inner)) {
                /* null-sentinel: bare return = none = NULL */
                emit(e, "return (");
                emit_type(e, bare_ret->optional.inner);
                emit(e, ")0;\n");
            } else if (bare_ret && bare_ret->kind == TYPE_OPTIONAL) {
                emit(e, "return (");
                emit_type(e, e->current_func_ret);
                if (is_void_opt(e->current_func_ret)) {
                    emit(e, "){ 1 };\n");
                } else {
                    emit(e, "){ 0, 1 };\n");
                }
            } else {
                emit(e, "return;\n");
            }
        }
        break;
    }

    case NODE_GOTO:
        emit_defers(e);  /* fire all pending defers before jumping */
        emit_indent(e);
        emit(e, "goto %.*s;\n", (int)node->goto_stmt.label_len, node->goto_stmt.label);
        break;

    case NODE_LABEL:
        emit(e, "%.*s:;\n", (int)node->label_stmt.name_len, node->label_stmt.name);
        break;

    case NODE_BREAK:
        emit_defers_from(e, e->loop_defer_base); /* defers in loop scope only */
        emit_indent(e);
        emit(e, "break;\n");
        break;

    case NODE_CONTINUE:
        emit_defers_from(e, e->loop_defer_base); /* defers in loop scope only */
        emit_indent(e);
        emit(e, "continue;\n");
        break;

    case NODE_EXPR_STMT: {
        /* auto-guard: emit bounds guards before the statement */
        emit_auto_guards(e, node->expr_stmt.expr);

        /* BUG-503: async expr-stmt orelse — Refactor 2: unified helper.
         * dest_name=NULL → result discarded (expr-stmt). */
        if (e->in_async && node->expr_stmt.expr &&
            node->expr_stmt.expr->kind == NODE_ORELSE &&
            node->expr_stmt.expr->orelse.fallback &&
            node->expr_stmt.expr->orelse.fallback->kind == NODE_BLOCK) {
            if (emit_async_orelse_block(e, node->expr_stmt.expr->orelse.expr,
                    node->expr_stmt.expr->orelse.fallback,
                    NULL, 0, NULL))
                break;
        }

        emit_indent(e);
        emit_expr(e, node->expr_stmt.expr);
        emit(e, ";\n");
        /* Level 2: poison-after-free — set pointer to NULL after free() */
        if (node->expr_stmt.expr && node->expr_stmt.expr->kind == NODE_CALL &&
            node->expr_stmt.expr->call.callee &&
            node->expr_stmt.expr->call.callee->kind == NODE_IDENT &&
            node->expr_stmt.expr->call.callee->ident.name_len == 4 &&
            memcmp(node->expr_stmt.expr->call.callee->ident.name, "free", 4) == 0 &&
            node->expr_stmt.expr->call.arg_count == 1 &&
            node->expr_stmt.expr->call.args[0]->kind == NODE_IDENT) {
            emit_indent(e);
            emit(e, "%.*s = (void*)0; /* poison-after-free */\n",
                 (int)node->expr_stmt.expr->call.args[0]->ident.name_len,
                 node->expr_stmt.expr->call.args[0]->ident.name);
        }
        break;
    }

    case NODE_ASM:
        emit_indent(e);
        /* extended asm: raw content includes template + operands + clobbers */
        emit(e, "__asm__ __volatile__(%.*s);\n",
             (int)node->asm_stmt.code_len, node->asm_stmt.code);
        break;

    case NODE_CRITICAL:
        /* @critical { body } — emit interrupt disable/enable wrapper */
        emit_indent(e);
        emit(e, "{ /* @critical */\n");
        e->indent++;
        emit_indent(e);
        emit(e, "#if defined(__ARM_ARCH)\n");
        emit_indent(e);
        emit(e, "uint32_t _zer_primask; __asm__ __volatile__(\"mrs %%0, primask\\n cpsid i\" : \"=r\"(_zer_primask));\n");
        emit_indent(e);
        emit(e, "#elif defined(__AVR__)\n");
        emit_indent(e);
        emit(e, "uint8_t _zer_sreg = SREG; __asm__ __volatile__(\"cli\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__riscv)\n");
        emit_indent(e);
        emit(e, "unsigned long _zer_mstatus; __asm__ __volatile__(\"csrrci %%0, mstatus, 8\" : \"=r\"(_zer_mstatus));\n");
        emit_indent(e);
        emit(e, "#elif (defined(__x86_64__) || defined(__i386__)) && !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)\n");
        emit_indent(e);
        emit(e, "unsigned long _zer_flags; __asm__ __volatile__(\"pushf; pop %%0; cli\" : \"=r\"(_zer_flags));\n");
        emit_indent(e);
        emit(e, "#else\n");
        emit_indent(e);
        emit(e, "/* hosted x86: @critical is a compiler fence (no real interrupt disable) */\n");
        emit_indent(e);
        emit(e, "__atomic_thread_fence(__ATOMIC_SEQ_CST);\n");
        emit_indent(e);
        emit(e, "#endif\n");
        if (node->critical.body) emit_stmt(e, node->critical.body);
        emit_indent(e);
        emit(e, "#if defined(__ARM_ARCH)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"msr primask, %%0\" :: \"r\"(_zer_primask));\n");
        emit_indent(e);
        emit(e, "#elif defined(__AVR__)\n");
        emit_indent(e);
        emit(e, "SREG = _zer_sreg;\n");
        emit_indent(e);
        emit(e, "#elif defined(__riscv)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"csrw mstatus, %%0\" :: \"r\"(_zer_mstatus));\n");
        emit_indent(e);
        emit(e, "#elif (defined(__x86_64__) || defined(__i386__)) && !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"push %%0; popf\" :: \"r\"(_zer_flags));\n");
        emit_indent(e);
        emit(e, "#else\n");
        emit_indent(e);
        emit(e, "__atomic_thread_fence(__ATOMIC_SEQ_CST);\n");
        emit_indent(e);
        emit(e, "#endif\n");
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        break;

    case NODE_ONCE: {
        /* @once { body } — execute exactly once using atomic CAS */
        static int once_id = 0;
        int oid = once_id++;
        emit_indent(e);
        emit(e, "{ /* @once */\n");
        e->indent++;
        emit_indent(e);
        emit(e, "static uint32_t _zer_once_%d = 0;\n", oid);
        emit_indent(e);
        emit(e, "if (!__atomic_exchange_n(&_zer_once_%d, 1, __ATOMIC_ACQ_REL)) {\n", oid);
        e->indent++;
        if (node->once.body) emit_stmt(e, node->once.body);
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        break;
    }

    case NODE_YIELD:
        /* yield — pause coroutine, resume here on next poll */
        if (e->in_async) {
            emit_indent(e);
            emit(e, "self->_zer_state = %d; return 0;\n", e->async_yield_id);
            emit_indent(e);
            emit(e, "case %d:;\n", e->async_yield_id);
            e->async_yield_id++;
        }
        break;

    case NODE_AWAIT:
        /* await expr — yield until condition is true */
        if (e->in_async) {
            emit_indent(e);
            emit(e, "case %d:;\n", e->async_yield_id);
            emit_indent(e);
            emit(e, "if (!(");
            emit_expr(e, node->await_stmt.cond);
            emit(e, ")) { self->_zer_state = %d; return 0; }\n", e->async_yield_id);
            e->async_yield_id++;
        }
        break;

    case NODE_SPAWN: {
        /* spawn func(args); — emit pthread_create using pre-scanned wrapper */
        int sid = -1;
        for (int wi = 0; wi < e->spawn_wrapper_count; wi++) {
            if (e->spawn_wrappers[wi].spawn_node == node) {
                sid = e->spawn_wrappers[wi].id;
                break;
            }
        }
        if (sid < 0) { emit(e, "/* spawn: wrapper not found */\n"); break; }
        int ac = node->spawn_stmt.arg_count;
        bool is_scoped = (node->spawn_stmt.handle_name != NULL);

        /* For scoped spawn, declare pthread_t variable at current scope */
        if (is_scoped) {
            emit_indent(e);
            emit(e, "pthread_t %.*s;\n",
                 (int)node->spawn_stmt.handle_name_len, node->spawn_stmt.handle_name);
        }

        emit_indent(e);
        emit(e, "{ /* spawn %.*s */\n", (int)node->spawn_stmt.func_name_len, node->spawn_stmt.func_name);
        e->indent++;

        if (ac > 0) {
            emit_indent(e);
            emit(e, "struct _zer_spawn_args_%d *_sa = malloc(sizeof(struct _zer_spawn_args_%d));\n", sid, sid);
            for (int i = 0; i < ac; i++) {
                emit_indent(e);
                emit(e, "_sa->a%d = ", i);
                emit_expr(e, node->spawn_stmt.args[i]);
                emit(e, ";\n");
            }
        }

        if (is_scoped) {
            emit_indent(e);
            emit(e, "pthread_create(&%.*s, NULL, _zer_spawn_wrap_%d, ",
                 (int)node->spawn_stmt.handle_name_len, node->spawn_stmt.handle_name, sid);
        } else {
            emit_indent(e);
            emit(e, "pthread_t _zer_th;\n");
            emit_indent(e);
            emit(e, "pthread_create(&_zer_th, NULL, _zer_spawn_wrap_%d, ", sid);
        }
        emit(e, ac > 0 ? "_sa);\n" : "NULL);\n");

        if (!is_scoped) {
            emit_indent(e);
            emit(e, "pthread_detach(_zer_th);\n");
        }

        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        break;
    }

    case NODE_STATIC_ASSERT:
        /* compile-time only — nothing to emit */
        break;

    case NODE_DEFER:
        /* push onto defer stack — will be emitted at block end in reverse */
        /* grow defer stack if needed */
        if (e->defer_stack.count >= e->defer_stack.capacity) {
            int new_cap = e->defer_stack.capacity * 2;
            if (new_cap < 16) new_cap = 16;
            Node **new_stmts = (Node **)malloc(new_cap * sizeof(Node *));
            if (e->defer_stack.stmts) {
                memcpy(new_stmts, e->defer_stack.stmts, e->defer_stack.count * sizeof(Node *));
                free(e->defer_stack.stmts);
            }
            e->defer_stack.stmts = new_stmts;
            e->defer_stack.capacity = new_cap;
        }
        e->defer_stack.stmts[e->defer_stack.count++] = node->defer.body;
        break;

    case NODE_SWITCH: {
        /* ZER switch with => arms → C if/else chain
         * switch (expr) { .a => ..., .b => ..., default => ... }
         * → { auto _sw = expr; if (_sw == a) { ... } else if (_sw == b) { ... } else { ... } }
         *
         * For enum dot syntax: .idle => ... → if (_sw == ENUM_idle) ...
         * For integer values: 0 => ... → if (_sw == 0) ...
         * For default: else { ... }
         */
        int sw_tmp = e->temp_count++;
        Type *sw_type = checker_get_type(e->checker,node->switch_stmt.expr);
        /* BUG-271: unwrap distinct before checking for union switch */
        Type *sw_eff = sw_type ? type_unwrap_distinct(sw_type) : NULL;
        bool is_union_switch = sw_eff && sw_eff->kind == TYPE_UNION;
        /* BUG-274: detect volatile switch expression (RF11 helper) */
        bool sw_volatile = expr_is_volatile(e, node->switch_stmt.expr);
        /* BUG-196b: detect struct-based optional in switch.
         * BUG-505: unwrap distinct + track inner enum type for dot-value emission. */
        bool is_opt_switch = false;
        Type *opt_inner_enum = NULL; /* non-NULL if optional wraps an enum */
        {
            Type *sw_check = sw_type ? type_unwrap_distinct(sw_type) : NULL;
            if (sw_check && sw_check->kind == TYPE_OPTIONAL) {
                Type *inner = sw_check->optional.inner;
                if (!is_null_sentinel(inner)) {
                    is_opt_switch = true;
                    Type *inner_eff = type_unwrap_distinct(inner);
                    if (inner_eff && inner_eff->kind == TYPE_ENUM)
                        opt_inner_enum = inner_eff;
                }
            }
        }

        emit_indent(e);
        emit(e, "{\n");
        e->indent++;
        emit_indent(e);
        if (is_union_switch) {
            /* union switch: need pointer for mutable capture.
             * BUG-268: for lvalue expressions, use direct &(expr) so mutations
             * affect the original. For rvalue (NODE_CALL), hoist into temp. */
            bool sw_is_rvalue = (node->switch_stmt.expr->kind == NODE_CALL);
            if (sw_is_rvalue) {
                /* BUG-352: __typeof__ preserves volatile, __auto_type does not */
                emit(e, "__typeof__(");
                emit_expr(e, node->switch_stmt.expr);
                emit(e, ") _zer_swt%d = ", sw_tmp);
                emit_expr(e, node->switch_stmt.expr);
                emit(e, ";\n");
                emit_indent(e);
                emit(e, "__typeof__(_zer_swt%d) *_zer_swp%d = &_zer_swt%d;\n", sw_tmp, sw_tmp, sw_tmp);
            } else {
                emit(e, "__typeof__(");
                emit_expr(e, node->switch_stmt.expr);
                emit(e, ") *_zer_swp%d = &(", sw_tmp);
                emit_expr(e, node->switch_stmt.expr);
                emit(e, ");\n");
            }
        } else {
            emit(e, "__auto_type _zer_sw%d = ", sw_tmp);
            emit_expr(e, node->switch_stmt.expr);
            emit(e, ";\n");
        }

        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            SwitchArm *arm = &node->switch_stmt.arms[i];
            emit_indent(e);

            if (arm->is_default) {
                if (i > 0) emit(e, "else ");
                emit(e, "/* default */ ");
            } else if (is_union_switch && arm->is_enum_dot) {
                /* union switch: check _tag field via pointer */
                if (i > 0) emit(e, "else ");
                emit(e, "if (");
                for (int j = 0; j < arm->value_count; j++) {
                    if (j > 0) emit(e, " || ");
                    emit(e, "_zer_swp%d->_tag == _ZER_", sw_tmp);
                    EMIT_UNION_NAME(e, sw_eff);
                    emit(e, "_TAG_%.*s",
                         (int)arm->values[j]->ident.name_len, arm->values[j]->ident.name);
                }
                emit(e, ") ");
            } else if (!is_union_switch && arm->is_enum_dot && sw_eff && sw_eff->kind == TYPE_ENUM) {
                /* enum switch: .idle => ... → if (_sw == _ZER_State_idle) */
                if (i > 0) emit(e, "else ");
                emit(e, "if (");
                for (int j = 0; j < arm->value_count; j++) {
                    if (j > 0) emit(e, " || ");
                    emit(e, "_zer_sw%d == _ZER_", sw_tmp);
                    EMIT_ENUM_NAME(e, sw_eff);
                    emit(e, "_%.*s",
                         (int)arm->values[j]->ident.name_len, arm->values[j]->ident.name);
                }
                emit(e, ") ");
            } else if (is_opt_switch) {
                /* optional switch: compare .value (and require .has_value).
                 * BUG-505: for enum dot values, emit _ZER_EnumName_variant
                 * (not bare ident which is undeclared in C). */
                if (i > 0) emit(e, "else ");
                emit(e, "if (");
                for (int j = 0; j < arm->value_count; j++) {
                    if (j > 0) emit(e, " || ");
                    emit(e, "(_zer_sw%d.has_value && _zer_sw%d.value == ", sw_tmp, sw_tmp);
                    if (arm->is_enum_dot && opt_inner_enum) {
                        /* emit fully qualified enum constant */
                        emit(e, "_ZER_");
                        EMIT_ENUM_NAME(e, opt_inner_enum);
                        emit(e, "_%.*s",
                             (int)arm->values[j]->ident.name_len, arm->values[j]->ident.name);
                    } else {
                        emit_expr(e, arm->values[j]);
                    }
                    emit(e, ")");
                }
                emit(e, ") ");
            } else {
                if (i > 0) emit(e, "else ");
                emit(e, "if (");
                for (int j = 0; j < arm->value_count; j++) {
                    if (j > 0) emit(e, " || ");
                    emit(e, "_zer_sw%d == ", sw_tmp);
                    emit_expr(e, arm->values[j]);
                }
                emit(e, ") ");
            }

            /* arm body — handle captures for optional switch */
            if (is_opt_switch && arm->capture_name) {
                emit(e, "{\n");
                e->indent++;
                emit_indent(e);
                /* extract .value from optional */
                if (arm->capture_is_ptr) {
                    Type *inner = sw_type->optional.inner;
                    emit_type(e, inner);
                    emit(e, " *%.*s = &_zer_sw%d.value;\n",
                         (int)arm->capture_name_len, arm->capture_name, sw_tmp);
                } else {
                    emit(e, "__auto_type %.*s = _zer_sw%d.value;\n",
                         (int)arm->capture_name_len, arm->capture_name, sw_tmp);
                }
                int saved_sw_defer = e->defer_stack.count;
                if (arm->body->kind == NODE_BLOCK) {
                    for (int k = 0; k < arm->body->block.stmt_count; k++)
                        emit_stmt(e, arm->body->block.stmts[k]);
                } else {
                    emit_stmt(e, arm->body);
                }
                emit_defers_from(e, saved_sw_defer);
                e->defer_stack.count = saved_sw_defer;
                e->indent--;
                emit_indent(e);
                emit(e, "}\n");
            } else if (is_union_switch && arm->capture_name && arm->value_count > 0) {
                emit(e, "{\n");
                e->indent++;
                emit_indent(e);
                /* extract variant from anonymous union */
                if (arm->capture_is_ptr) {
                    /* mutable capture |*v| — pointer to variant
                     * Can't use __auto_type * (GCC rejects it) — look up variant type */
                    const char *vname = arm->values[0]->ident.name;
                    uint32_t vlen = (uint32_t)arm->values[0]->ident.name_len;
                    Type *vtype = NULL;
                    for (uint32_t vi = 0; vi < sw_eff->union_type.variant_count; vi++) {
                        if (sw_eff->union_type.variants[vi].name_len == vlen &&
                            memcmp(sw_eff->union_type.variants[vi].name, vname, vlen) == 0) {
                            vtype = sw_eff->union_type.variants[vi].type;
                            break;
                        }
                    }
                    /* BUG-274: preserve volatile on variant pointer */
                    if (sw_volatile) emit(e, "volatile ");
                    if (vtype) emit_type(e, vtype);
                    else emit(e, "void");
                    emit(e, " *%.*s = &_zer_swp%d->%.*s;\n",
                         (int)arm->capture_name_len, arm->capture_name,
                         sw_tmp, (int)vlen, vname);
                } else {
                    /* immutable capture |v| — value copy from original */
                    emit(e, "__auto_type %.*s = _zer_swp%d->%.*s;\n",
                         (int)arm->capture_name_len, arm->capture_name,
                         sw_tmp,
                         (int)arm->values[0]->ident.name_len, arm->values[0]->ident.name);
                }
                /* emit body contents — track defer scope (BUG-102 fix) */
                int saved_sw_defer = e->defer_stack.count;
                if (arm->body->kind == NODE_BLOCK) {
                    for (int k = 0; k < arm->body->block.stmt_count; k++)
                        emit_stmt(e, arm->body->block.stmts[k]);
                } else {
                    emit_stmt(e, arm->body);
                }
                emit_defers_from(e, saved_sw_defer);
                e->defer_stack.count = saved_sw_defer;
                e->indent--;
                emit_indent(e);
                emit(e, "}\n");
            } else if (arm->body->kind == NODE_BLOCK) {
                emit_stmt(e, arm->body);
            } else {
                emit(e, "{\n");
                e->indent++;
                emit_stmt(e, arm->body);
                e->indent--;
                emit_indent(e);
                emit(e, "}\n");
            }
        }

        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        break;
    }

    default:
        emit_indent(e);
        emit(e, "/* unhandled stmt %s */\n", node_kind_name(node->kind));
        break;
    }
}

/* ================================================================
 * TYPE RESOLUTION HELPER — resolve TypeNode for emission
 * ================================================================ */

static Type *resolve_type_for_emit(Emitter *e, TypeNode *tn) {
    /* We need the checker's resolve_type, but it's static in checker.c.
     * For now, do a simple direct mapping for basic types.
     * TODO: expose resolve_type or cache resolved types on AST nodes. */
    if (!tn) return NULL;

    switch (tn->kind) {
    case TYNODE_U8:     return ty_u8;
    case TYNODE_U16:    return ty_u16;
    case TYNODE_U32:    return ty_u32;
    case TYNODE_U64:    return ty_u64;
    case TYNODE_I8:     return ty_i8;
    case TYNODE_I16:    return ty_i16;
    case TYNODE_I32:    return ty_i32;
    case TYNODE_I64:    return ty_i64;
    case TYNODE_USIZE:  return ty_usize;
    case TYNODE_F32:    return ty_f32;
    case TYNODE_F64:    return ty_f64;
    case TYNODE_BOOL:   return ty_bool;
    case TYNODE_VOID:   return ty_void;
    case TYNODE_POINTER: {
        Type *inner = resolve_tynode(e,tn->pointer.inner);
        return type_pointer(e->arena, inner);
    }
    case TYNODE_OPTIONAL: {
        Type *inner = resolve_tynode(e,tn->optional.inner);
        return type_optional(e->arena, inner);
    }
    case TYNODE_ARRAY: {
        Type *elem = resolve_tynode(e,tn->array.elem);
        uint32_t size = 0;
        if (tn->array.size_expr) {
            int64_t val = eval_const_expr(tn->array.size_expr);
            if (val > 0) size = (uint32_t)val;
        }
        return type_array(e->arena, elem, size);
    }
    case TYNODE_SLICE: {
        Type *inner = resolve_tynode(e,tn->slice.inner);
        return type_slice(e->arena, inner);
    }
    case TYNODE_HANDLE:
        return type_handle(e->arena, resolve_tynode(e,tn->handle.elem));
    case TYNODE_NAMED: {
        /* look up in checker's global scope */
        Symbol *sym = scope_lookup(e->checker->global_scope,
            tn->named.name, (uint32_t)tn->named.name_len);
        if (sym) return sym->type;
        return ty_void;
    }
    case TYNODE_CONST:
        return resolve_tynode(e,tn->qualified.inner);
    case TYNODE_VOLATILE: {
        Type *inner = resolve_tynode(e,tn->qualified.inner);
        /* propagate volatile to pointer/slice type.
         * ctags audit: unwrap distinct (same fix as A11 in checker). */
        Type *iv = inner ? type_unwrap_distinct(inner) : NULL;
        if (iv && iv->kind == TYPE_POINTER) {
            Type *vp = type_pointer(e->arena, iv->pointer.inner);
            vp->pointer.is_volatile = true;
            if (iv->pointer.is_const) vp->pointer.is_const = true;
            return vp;
        }
        if (iv && iv->kind == TYPE_SLICE) {
            Type *vs = type_volatile_slice(e->arena, iv->slice.inner);
            if (iv->slice.is_const) vs->slice.is_const = true;
            return vs;
        }
        return inner;
    }
    case TYNODE_ARENA:
        return ty_arena;
    case TYNODE_OPAQUE:
        return ty_opaque;
    case TYNODE_POOL: {
        Type *elem = resolve_tynode(e,tn->pool.elem);
        uint32_t count = 0;
        if (tn->pool.count_expr) {
            int64_t val = eval_const_expr(tn->pool.count_expr);
            if (val > 0) count = (uint32_t)val;
        }
        return type_pool(e->arena, elem, count);
    }
    case TYNODE_RING: {
        Type *elem = resolve_tynode(e,tn->ring.elem);
        uint32_t count = 0;
        if (tn->ring.count_expr) {
            int64_t val = eval_const_expr(tn->ring.count_expr);
            if (val > 0) count = (uint32_t)val;
        }
        return type_ring(e->arena, elem, count);
    }
    case TYNODE_FUNC_PTR: {
        Type *ret = resolve_tynode(e,tn->func_ptr.return_type);
        uint32_t pc = (uint32_t)tn->func_ptr.param_count;
        Type **params = NULL;
        if (pc > 0) {
            params = (Type **)arena_alloc(e->arena, pc * sizeof(Type *));
            for (uint32_t i = 0; i < pc; i++)
                params[i] = resolve_tynode(e,tn->func_ptr.param_types[i]);
        }
        return type_func_ptr(e->arena, params, pc, ret);
    }
    /* ctags audit: these 4 TYNODE types were missing — silently returned ty_void */
    case TYNODE_SLAB: {
        Type *elem = resolve_tynode(e,tn->slab.elem);
        return type_slab(e->arena, elem);
    }
    case TYNODE_BARRIER:
        return ty_barrier;
    case TYNODE_SEMAPHORE: {
        uint32_t count = 0;
        if (tn->semaphore.count_expr) {
            int64_t val = eval_const_expr(tn->semaphore.count_expr);
            if (val >= 0) count = (uint32_t)val;
        }
        return type_semaphore(e->arena, count);
    }
    case TYNODE_CONTAINER: {
        /* container instantiation — look up stamped struct from checker */
        Symbol *sym = scope_lookup(e->checker->global_scope,
            tn->container.name, (uint32_t)tn->container.name_len);
        if (sym) return sym->type;
        return ty_void;
    }
    default:
        return ty_void;
    }
}

/* ================================================================
 * TOP-LEVEL DECLARATION EMISSION
 * ================================================================ */

/* Emit all stamped container struct declarations */
static void emit_container_structs(Emitter *e) {
    if (!e->checker || e->checker->container_inst_count == 0) return;
    for (int ci = 0; ci < e->checker->container_inst_count; ci++) {
        Type *st = e->checker->container_instances[ci].stamped_struct;
        if (!st || st->kind != TYPE_STRUCT) continue;
        emit(e, "struct %.*s {\n", (int)st->struct_type.name_len, st->struct_type.name);
        e->indent++;
        for (uint32_t fi = 0; fi < st->struct_type.field_count; fi++) {
            SField *sf = &st->struct_type.fields[fi];
            emit_indent(e);
            emit_type_and_name(e, sf->type, sf->name, sf->name_len);
            emit(e, ";\n");
        }
        e->indent--;
        emit(e, "};\n\n");
    }
}

static void emit_struct_decl(Emitter *e, Node *node) {
    Type *st = checker_get_type(e->checker,node);
    if (node->struct_decl.is_packed) {
        emit(e, "struct __attribute__((packed)) ");
        if (st) EMIT_STRUCT_NAME(e, st);
        else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
        emit(e, " {\n");
    } else {
        emit(e, "struct ");
        if (st) EMIT_STRUCT_NAME(e, st);
        else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
        emit(e, " {\n");
    }
    e->indent++;
    {
    for (int i = 0; i < node->struct_decl.field_count; i++) {
        FieldDecl *f = &node->struct_decl.fields[i];
        Type *ftype = (st && st->kind == TYPE_STRUCT &&
                      (uint32_t)i < st->struct_type.field_count) ?
            st->struct_type.fields[i].type : resolve_tynode(e,f->type);
        emit_indent(e);
        /* check if field has volatile qualifier (TYNODE_VOLATILE wrapper) */
        if (f->type && f->type->kind == TYNODE_VOLATILE &&
            !(ftype && ftype->kind == TYPE_POINTER))
            emit(e, "volatile ");
        emit_type_and_name(e, ftype, f->name, f->name_len);
        emit(e, ";\n");
    }
    }
    /* shared struct: add hidden lock field (+ condvar/rwlock if needed) */
    if (node->struct_decl.is_shared) {
        if (node->struct_decl.is_shared_rw) {
            emit_indent(e); emit(e, "pthread_rwlock_t _zer_rwlock;\n");
        } else {
            Type *st = checker_get_type(e->checker, (Node *)node);
            bool needs_condvar = false;
            if (st && st->kind == TYPE_STRUCT)
                needs_condvar = is_condvar_type(e, st->struct_type.type_id);
            /* BUG-473: all shared structs use recursive pthread_mutex_t.
             * Lazy-init via _zer_mtx_inited flag (auto-zeroed). */
            emit_indent(e); emit(e, "pthread_mutex_t _zer_mtx;\n");
            emit_indent(e); emit(e, "uint8_t _zer_mtx_inited;\n");
            if (needs_condvar) {
                emit_indent(e); emit(e, "pthread_cond_t _zer_cond;\n");
            }
        }
    }
    e->indent--;
    emit(e, "};\n");
    /* emit optional typedef for this struct */
    emit(e, "typedef struct { struct ");
    if (st) EMIT_STRUCT_NAME(e, st);
    else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
    emit(e, " value; uint8_t has_value; } _zer_opt_");
    if (st) EMIT_STRUCT_NAME(e, st);
    else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
    emit(e, ";\n");
    /* emit slice typedef for this struct */
    emit(e, "typedef struct { struct ");
    if (st) EMIT_STRUCT_NAME(e, st);
    else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
    emit(e, "* ptr; size_t len; } _zer_slice_");
    if (st) EMIT_STRUCT_NAME(e, st);
    else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
    emit(e, ";\n");
    /* emit volatile slice typedef for this struct */
    emit(e, "typedef struct { volatile struct ");
    if (st) EMIT_STRUCT_NAME(e, st);
    else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
    emit(e, "* ptr; size_t len; } _zer_vslice_");
    if (st) EMIT_STRUCT_NAME(e, st);
    else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
    emit(e, ";\n");
    /* emit optional-slice typedef for this struct */
    emit(e, "typedef struct { _zer_slice_");
    if (st) EMIT_STRUCT_NAME(e, st);
    else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
    emit(e, " value; uint8_t has_value; } _zer_opt_slice_");
    if (st) EMIT_STRUCT_NAME(e, st);
    else emit(e, "%.*s", (int)node->struct_decl.name_len, node->struct_decl.name);
    emit(e, ";\n\n");
}

/* ================================================================
 * ASYNC FUNCTION EMISSION
 * Transforms async functions into state machine struct + poll function.
 * Uses Duff's device: switch(state) { case 0: ... case N: ... }
 * can jump INTO loops — valid C since 1983.
 * ================================================================ */

/* Refactor 2: unified async orelse block emission (implementation).
 * Forward-declared near top of file. */
static bool emit_async_orelse_block(Emitter *e, Node *orelse_expr, Node *fallback,
                                     const char *dest_name, size_t dest_len,
                                     Type *dest_type) {
    if (!e->in_async) return false;

    int atid = -1;
    for (int ati = 0; ati < e->async_temp_count; ati++) {
        if (e->async_temps[ati].temp_id >= 0) {
            atid = e->async_temps[ati].temp_id;
            e->async_temps[ati].temp_id = -1; /* mark used */
            break;
        }
    }
    if (atid < 0) return false;

    Type *or_expr_type = checker_get_type(e->checker, orelse_expr);
    Type *or_eff = or_expr_type ? type_unwrap_distinct(or_expr_type) : NULL;
    bool or_is_ptr = or_eff && or_eff->kind == TYPE_OPTIONAL &&
        is_null_sentinel(or_eff->optional.inner);

    /* self->_zer_async_tmpN = expr; */
    emit_indent(e);
    emit(e, "self->_zer_async_tmp%d = ", atid);
    emit_expr(e, orelse_expr);
    emit(e, ";\n");

    /* if (!self->_zer_async_tmpN.has_value) { block } */
    emit_indent(e);
    if (or_is_ptr) {
        emit(e, "if (!self->_zer_async_tmp%d) ", atid);
    } else {
        emit(e, "if (!self->_zer_async_tmp%d.has_value) ", atid);
    }
    emit_stmt(e, fallback);
    emit(e, "\n");

    /* self->dest = self->_zer_async_tmpN.value; (skip if dest_name is NULL = discard) */
    if (dest_name) {
        emit_indent(e);
        if (or_is_ptr) {
            emit(e, "self->%.*s = self->_zer_async_tmp%d;\n",
                 (int)dest_len, dest_name, atid);
        } else if (dest_type && is_void_opt(dest_type)) {
            /* ?void — no value to extract */
        } else {
            emit(e, "self->%.*s = self->_zer_async_tmp%d.value;\n",
                 (int)dest_len, dest_name, atid);
        }
    }
    return true;
}

/* BUG-495: helper — register one async orelse temp */
static void register_async_orelse_temp(Emitter *e, Node *orelse_expr) {
    Type *ot = checker_get_type(e->checker, orelse_expr);
    if (!ot) return;
    if (e->async_temp_count >= e->async_temp_capacity) {
        int nc = e->async_temp_capacity < 4 ? 4 : e->async_temp_capacity * 2;
        e->async_temps = realloc(e->async_temps, nc * sizeof(struct AsyncTemp));
        e->async_temp_capacity = nc;
    }
    e->async_temps[e->async_temp_count].type = ot;
    e->async_temps[e->async_temp_count].temp_id = e->async_temp_next_id++;
    e->async_temp_count++;
}

/* BUG-495: scan expression tree for orelse with block fallback.
 * Recurses into ALL expression nodes — NODE_BINARY, NODE_CALL, NODE_ASSIGN,
 * NODE_UNARY, NODE_ORELSE, NODE_INTRINSIC, etc. Finds orelse blocks nested
 * at any depth in expression trees (e.g., 10 + (opt orelse { yield; 42; })). */
static void prescan_expr_for_orelse(Emitter *e, Node *expr) {
    if (!expr) return;
    if (expr->kind == NODE_ORELSE && expr->orelse.fallback &&
        expr->orelse.fallback->kind == NODE_BLOCK) {
        register_async_orelse_temp(e, expr->orelse.expr);
        /* Also recurse into the orelse block's statements */
        prescan_async_temps(e, expr->orelse.fallback);
    }
    /* Recurse into expression children */
    if (expr->kind == NODE_BINARY) {
        prescan_expr_for_orelse(e, expr->binary.left);
        prescan_expr_for_orelse(e, expr->binary.right);
    }
    if (expr->kind == NODE_UNARY) prescan_expr_for_orelse(e, expr->unary.operand);
    if (expr->kind == NODE_ASSIGN) {
        prescan_expr_for_orelse(e, expr->assign.target);
        prescan_expr_for_orelse(e, expr->assign.value);
    }
    if (expr->kind == NODE_CALL) {
        for (int i = 0; i < expr->call.arg_count; i++)
            prescan_expr_for_orelse(e, expr->call.args[i]);
    }
    if (expr->kind == NODE_ORELSE) {
        prescan_expr_for_orelse(e, expr->orelse.expr);
        prescan_expr_for_orelse(e, expr->orelse.fallback);
    }
    if (expr->kind == NODE_INTRINSIC) {
        for (int i = 0; i < expr->intrinsic.arg_count; i++)
            prescan_expr_for_orelse(e, expr->intrinsic.args[i]);
    }
    if (expr->kind == NODE_FIELD) prescan_expr_for_orelse(e, expr->field.object);
    if (expr->kind == NODE_INDEX) {
        prescan_expr_for_orelse(e, expr->index_expr.object);
        prescan_expr_for_orelse(e, expr->index_expr.index);
    }
    if (expr->kind == NODE_TYPECAST) prescan_expr_for_orelse(e, expr->typecast.expr);
}

/* Pre-scan async body for orelse blocks that need promoted temps (BUG-481/495).
 * Recursively finds NODE_ORELSE with block fallback at ANY depth — including
 * inside expression trees (NODE_BINARY, NODE_CALL, etc.). */
static void prescan_async_temps(Emitter *e, Node *node) {
    if (!node) return;
    /* Scan var-decl init expression tree for nested orelse */
    if (node->kind == NODE_VAR_DECL && node->var_decl.init)
        prescan_expr_for_orelse(e, node->var_decl.init);
    /* Scan expr-stmt expression tree */
    if (node->kind == NODE_EXPR_STMT && node->expr_stmt.expr)
        prescan_expr_for_orelse(e, node->expr_stmt.expr);
    /* Scan return expression */
    if (node->kind == NODE_RETURN && node->ret.expr)
        prescan_expr_for_orelse(e, node->ret.expr);
    /* Recurse into statement children */
    if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++)
            prescan_async_temps(e, node->block.stmts[i]);
    }
    if (node->kind == NODE_IF) {
        prescan_expr_for_orelse(e, node->if_stmt.cond);
        prescan_async_temps(e, node->if_stmt.then_body);
        prescan_async_temps(e, node->if_stmt.else_body);
    }
    if (node->kind == NODE_FOR) {
        prescan_async_temps(e, node->for_stmt.init);
        prescan_expr_for_orelse(e, node->for_stmt.cond);
        prescan_expr_for_orelse(e, node->for_stmt.step);
        prescan_async_temps(e, node->for_stmt.body);
    }
    if (node->kind == NODE_WHILE || node->kind == NODE_DO_WHILE) {
        prescan_expr_for_orelse(e, node->while_stmt.cond);
        prescan_async_temps(e, node->while_stmt.body);
    }
    if (node->kind == NODE_SWITCH) {
        prescan_expr_for_orelse(e, node->switch_stmt.expr);
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            prescan_async_temps(e, node->switch_stmt.arms[i].body);
    }
    if (node->kind == NODE_DEFER) prescan_async_temps(e, node->defer.body);
    if (node->kind == NODE_CRITICAL) prescan_async_temps(e, node->critical.body);
    if (node->kind == NODE_ONCE) prescan_async_temps(e, node->once.body);
}

/* BUG-490: helper to add one async local (dedup by name) */
static void add_async_local(Emitter *e, const char *name, size_t name_len) {
    /* dedup — same name already promoted (shadowing or repeated) */
    for (int i = 0; i < e->async_local_count; i++) {
        if (e->async_local_lens[i] == name_len &&
            memcmp(e->async_locals[i], name, name_len) == 0)
            return;
    }
    if (e->async_local_count >= e->async_local_capacity) {
        int nc = e->async_local_capacity < 8 ? 8 : e->async_local_capacity * 2;
        const char **nls = (const char **)arena_alloc(e->arena, nc * sizeof(const char *));
        size_t *nlens = (size_t *)arena_alloc(e->arena, nc * sizeof(size_t));
        if (e->async_locals) {
            memcpy(nls, e->async_locals, e->async_local_count * sizeof(const char *));
            memcpy(nlens, e->async_local_lens, e->async_local_count * sizeof(size_t));
        }
        e->async_locals = nls;
        e->async_local_lens = nlens;
        e->async_local_capacity = nc;
    }
    e->async_locals[e->async_local_count] = name;
    e->async_local_lens[e->async_local_count] = name_len;
    e->async_local_count++;
}

/* BUG-490: collect local variable declarations RECURSIVELY from all blocks.
 * Sub-block locals must be promoted to state struct — they live on the C stack
 * which is destroyed on yield. Same fix as Rust's MIR generator transform. */
static void collect_async_locals(Emitter *e, Node *node) {
    if (!node) return;
    if (node->kind == NODE_VAR_DECL && !node->var_decl.is_static) {
        add_async_local(e, node->var_decl.name, node->var_decl.name_len);
    }
    if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++)
            collect_async_locals(e, node->block.stmts[i]);
    }
    if (node->kind == NODE_IF) {
        /* Async capture promotion: if (opt) |val| introduces implicit local 'val'.
         * Must be promoted to state struct — lives on C stack, invalid after yield. */
        if (node->if_stmt.capture_name) {
            add_async_local(e, node->if_stmt.capture_name,
                            node->if_stmt.capture_name_len);
        }
        collect_async_locals(e, node->if_stmt.then_body);
        collect_async_locals(e, node->if_stmt.else_body);
    }
    if (node->kind == NODE_FOR) {
        collect_async_locals(e, node->for_stmt.init);
        collect_async_locals(e, node->for_stmt.body);
    }
    if (node->kind == NODE_WHILE || node->kind == NODE_DO_WHILE)
        collect_async_locals(e, node->while_stmt.body);
    if (node->kind == NODE_SWITCH) {
        /* Switch arm captures in async deferred to v0.4 (type resolution complex) */
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            collect_async_locals(e, node->switch_stmt.arms[i].body);
    }
    if (node->kind == NODE_DEFER)
        collect_async_locals(e, node->defer.body);
    if (node->kind == NODE_CRITICAL)
        collect_async_locals(e, node->critical.body);
    if (node->kind == NODE_ONCE)
        collect_async_locals(e, node->once.body);
}

/* Check if an ident name is an async-promoted local */
static bool is_async_local(Emitter *e, const char *name, size_t len) {
    if (!e->in_async) return false;
    for (int i = 0; i < e->async_local_count; i++) {
        if (e->async_local_lens[i] == len &&
            memcmp(e->async_locals[i], name, len) == 0)
            return true;
    }
    return false;
}

static void emit_async_func(Emitter *e, Node *node) {
    Node *body = node->func_decl.body;
    if (!body) return;

    /* BUG-482: build module-mangled function name for async struct/init/poll.
     * Without module prefix, two modules with same async function name collide.
     * Uses same module__name pattern as EMIT_MANGLED_NAME. */
    char mname_buf[256];
    int flen;
    if (e->current_module) {
        flen = snprintf(mname_buf, sizeof(mname_buf), "%.*s__%.*s",
            (int)e->current_module_len, e->current_module,
            (int)node->func_decl.name_len, node->func_decl.name);
    } else {
        flen = snprintf(mname_buf, sizeof(mname_buf), "%.*s",
            (int)node->func_decl.name_len, node->func_decl.name);
    }
    const char *fname = mname_buf;

    /* Collect local variables for the state struct */
    e->async_local_count = 0;
    /* BUG-477: also promote function parameters to state struct.
     * Without this, params are undeclared in the poll function after yield. */
    for (int pi = 0; pi < node->func_decl.param_count; pi++) {
        ParamDecl *p = &node->func_decl.params[pi];
        if (e->async_local_count >= e->async_local_capacity) {
            int nc = e->async_local_capacity < 8 ? 8 : e->async_local_capacity * 2;
            const char **nls = (const char **)arena_alloc(e->arena, nc * sizeof(const char *));
            size_t *nlens = (size_t *)arena_alloc(e->arena, nc * sizeof(size_t));
            if (e->async_locals) {
                memcpy(nls, e->async_locals, e->async_local_count * sizeof(const char *));
                memcpy(nlens, e->async_local_lens, e->async_local_count * sizeof(size_t));
            }
            e->async_locals = nls;
            e->async_local_lens = nlens;
            e->async_local_capacity = nc;
        }
        e->async_locals[e->async_local_count] = p->name;
        e->async_local_lens[e->async_local_count] = p->name_len;
        e->async_local_count++;
    }
    collect_async_locals(e, body);

    /* BUG-481: pre-scan for orelse/capture temps that need state struct promotion */
    e->async_temp_count = 0;
    e->async_temp_next_id = 0;
    prescan_async_temps(e, body);

    /* Emit state struct typedef */
    emit(e, "typedef struct {\n");
    emit(e, "    int _zer_state;\n");
    /* Emit function parameters as struct fields (BUG-477) */
    for (int pi = 0; pi < node->func_decl.param_count; pi++) {
        ParamDecl *p = &node->func_decl.params[pi];
        Type *pt = resolve_tynode(e, p->type);
        if (pt) {
            emit(e, "    ");
            emit_type_and_name(e, pt, p->name, p->name_len);
            emit(e, ";\n");
        }
    }
    /* BUG-490: emit ALL collected locals as struct fields.
     * collect_async_locals is now recursive — finds vars in sub-blocks,
     * if/while/for/switch bodies. Use the async_locals list (dedup'd)
     * and resolve types via scope lookup. Skip params (already emitted). */
    {
        /* Collect all var-decl nodes recursively for type resolution */
        struct { Node *node; } var_nodes[256];
        int var_node_count = 0;
        /* Recursive scan — same traversal as collect_async_locals */
        struct { Node *n; } stack[256];
        int sp = 0;
        if (body) stack[sp++].n = body;
        while (sp > 0 && var_node_count < 256) {
            Node *n = stack[--sp].n;
            if (!n) continue;
            if (n->kind == NODE_VAR_DECL && !n->var_decl.is_static) {
                /* Check if this name is in async_locals (dedup'd, skips params) */
                bool is_param = false;
                for (int pi = 0; pi < node->func_decl.param_count; pi++) {
                    if (node->func_decl.params[pi].name_len == n->var_decl.name_len &&
                        memcmp(node->func_decl.params[pi].name, n->var_decl.name,
                               n->var_decl.name_len) == 0) {
                        is_param = true; break;
                    }
                }
                /* Check if already emitted (dedup for same-name in different blocks) */
                bool dup = false;
                for (int vi = 0; vi < var_node_count; vi++) {
                    if (var_nodes[vi].node->var_decl.name_len == n->var_decl.name_len &&
                        memcmp(var_nodes[vi].node->var_decl.name, n->var_decl.name,
                               n->var_decl.name_len) == 0) {
                        dup = true; break;
                    }
                }
                if (!is_param && !dup)
                    var_nodes[var_node_count++].node = n;
            }
            /* Push children — reverse order so left-to-right processing */
            if (n->kind == NODE_BLOCK) {
                for (int i = n->block.stmt_count - 1; i >= 0 && sp < 255; i--)
                    stack[sp++].n = n->block.stmts[i];
            }
            if (n->kind == NODE_IF) {
                if (sp < 254) { stack[sp++].n = n->if_stmt.else_body; stack[sp++].n = n->if_stmt.then_body; }
            }
            if (n->kind == NODE_FOR && sp < 254) { stack[sp++].n = n->for_stmt.body; stack[sp++].n = n->for_stmt.init; }
            if ((n->kind == NODE_WHILE || n->kind == NODE_DO_WHILE) && sp < 255) stack[sp++].n = n->while_stmt.body;
            if (n->kind == NODE_SWITCH) {
                for (int i = n->switch_stmt.arm_count - 1; i >= 0 && sp < 255; i--)
                    stack[sp++].n = n->switch_stmt.arms[i].body;
            }
            if (n->kind == NODE_DEFER && sp < 255) stack[sp++].n = n->defer.body;
            if (n->kind == NODE_CRITICAL && sp < 255) stack[sp++].n = n->critical.body;
            if (n->kind == NODE_ONCE && sp < 255) stack[sp++].n = n->once.body;
        }
        for (int vi = 0; vi < var_node_count; vi++) {
            Node *s = var_nodes[vi].node;
            Type *vt = checker_get_type(e->checker, s);
            if (vt) {
                emit(e, "    ");
                emit_type_and_name(e, vt, s->var_decl.name, s->var_decl.name_len);
                emit(e, ";\n");
            }
        }
    }
    /* Emit capture variables from if-unwrap |val| and switch |val| as struct fields.
     * These are implicit locals — not NODE_VAR_DECL, so the above loop misses them.
     * The capture type is the optional's inner type (value capture) or pointer to it (|*val|). */
    {
        struct { Node *n; } cstack[256];
        int csp = 0;
        if (body) cstack[csp++].n = body;
        while (csp > 0) {
            Node *cn = cstack[--csp].n;
            if (!cn) continue;
            /* If-unwrap capture */
            if (cn->kind == NODE_IF && cn->if_stmt.capture_name) {
                Type *cond_type = checker_get_type(e->checker, cn->if_stmt.cond);
                if (cond_type) {
                    Type *ceff = type_unwrap_distinct(cond_type);
                    Type *inner = NULL;
                    if (ceff && ceff->kind == TYPE_OPTIONAL) {
                        inner = ceff->optional.inner;
                        if (cn->if_stmt.capture_is_ptr && inner) {
                            inner = type_pointer(e->arena, inner);
                        }
                    } else if (ceff && is_null_sentinel(ceff->optional.inner)) {
                        inner = ceff->optional.inner;
                    }
                    if (inner) {
                        /* Dedup against params and var_nodes */
                        bool skip = false;
                        for (int pi = 0; pi < node->func_decl.param_count && !skip; pi++)
                            if (node->func_decl.params[pi].name_len == cn->if_stmt.capture_name_len &&
                                memcmp(node->func_decl.params[pi].name, cn->if_stmt.capture_name,
                                       cn->if_stmt.capture_name_len) == 0) skip = true;
                        if (!skip) {
                            emit(e, "    ");
                            emit_type_and_name(e, inner, cn->if_stmt.capture_name,
                                               cn->if_stmt.capture_name_len);
                            emit(e, ";\n");
                        }
                    }
                }
            }
            /* Switch arm captures: type resolution is complex (depends on union variant).
             * Deferred to v0.4 — switch captures in async are rare and need proper
             * variant type lookup. If-unwrap captures (above) cover the common case. */
            /* Recurse into children */
            if (cn->kind == NODE_BLOCK) {
                for (int i = cn->block.stmt_count - 1; i >= 0 && csp < 255; i--)
                    cstack[csp++].n = cn->block.stmts[i];
            }
            if (cn->kind == NODE_IF && csp < 254) {
                cstack[csp++].n = cn->if_stmt.else_body;
                cstack[csp++].n = cn->if_stmt.then_body;
            }
            if (cn->kind == NODE_FOR && csp < 254) { cstack[csp++].n = cn->for_stmt.body; cstack[csp++].n = cn->for_stmt.init; }
            if ((cn->kind == NODE_WHILE || cn->kind == NODE_DO_WHILE) && csp < 255) cstack[csp++].n = cn->while_stmt.body;
            if (cn->kind == NODE_SWITCH) {
                for (int i = cn->switch_stmt.arm_count - 1; i >= 0 && csp < 255; i--)
                    cstack[csp++].n = cn->switch_stmt.arms[i].body;
            }
            if (cn->kind == NODE_DEFER && csp < 255) cstack[csp++].n = cn->defer.body;
            if (cn->kind == NODE_CRITICAL && csp < 255) cstack[csp++].n = cn->critical.body;
        }
    }
    /* BUG-481: emit promoted orelse/capture temp fields */
    for (int ti = 0; ti < e->async_temp_count; ti++) {
        emit(e, "    ");
        char tname[32];
        int tnlen = snprintf(tname, sizeof(tname), "_zer_async_tmp%d", e->async_temps[ti].temp_id);
        emit_type_and_name(e, e->async_temps[ti].type, tname, tnlen);
        emit(e, ";\n");
    }
    emit(e, "} _zer_async_%.*s;\n\n", flen, fname);

    /* Emit init function — accepts async function params (BUG-477) */
    emit(e, "static inline void _zer_async_%.*s_init(_zer_async_%.*s *self", flen, fname, flen, fname);
    for (int pi = 0; pi < node->func_decl.param_count; pi++) {
        ParamDecl *p = &node->func_decl.params[pi];
        Type *pt = resolve_tynode(e, p->type);
        if (pt) {
            emit(e, ", ");
            emit_type_and_name(e, pt, p->name, p->name_len);
        }
    }
    emit(e, ") {\n");
    emit(e, "    memset(self, 0, sizeof(*self));\n");
    for (int pi = 0; pi < node->func_decl.param_count; pi++) {
        ParamDecl *p = &node->func_decl.params[pi];
        emit(e, "    self->%.*s = %.*s;\n", (int)p->name_len, p->name,
             (int)p->name_len, p->name);
    }
    emit(e, "}\n\n");

    /* Emit poll function */
    emit(e, "static inline int _zer_async_%.*s_poll(_zer_async_%.*s *self) {\n",
         flen, fname, flen, fname);
    emit(e, "    switch (self->_zer_state) { case 0:;\n");

    /* Emit the body with async transformations active */
    e->in_async = true;
    e->async_yield_id = 1; /* state 0 is the entry point */
    e->indent = 1;
    emit_stmt(e, body);
    e->in_async = false;

    emit(e, "    }\n");
    emit(e, "    self->_zer_state = -1;\n");
    emit(e, "    return 1; /* done */\n");
    emit(e, "}\n\n");
}

static void emit_func_decl(Emitter *e, Node *node) {

    /* IR emission path — lower to IR, emit from IR */
    if (e->use_ir && node->func_decl.body) {
        IRFunc *ir = ir_lower_func(e->arena, e->checker, node);
        if (ir) {
            ir->module_prefix = e->current_module;
            ir->module_prefix_len = e->current_module_len;
            if (ir_validate(ir)) {
                emit_func_from_ir(e, ir);
                return;
            }
            /* Validation failed — fall through to AST emission */
            fprintf(stderr, "IR validation failed for '%.*s' — falling back to AST emission\n",
                    (int)node->func_decl.name_len, node->func_decl.name);
        }
    }

    /* Async functions get special emission (AST path) */
    if (node->func_decl.is_async && node->func_decl.body) {
        emit_async_func(e, node);
        return;
    }

    Type *func_type = checker_get_type(e->checker,node);
    Type *ret = (func_type && func_type->kind == TYPE_FUNC_PTR) ?
        func_type->func_ptr.ret : NULL;

    /* section attribute */
    if (node->func_decl.section) {
        emit(e, "__attribute__((section(\"%.*s\"))) ",
             (int)node->func_decl.section_len, node->func_decl.section);
    }
    /* naked attribute */
    if (node->func_decl.is_naked) {
        emit(e, "__attribute__((naked)) ");
    }
    /* static functions */
    if (node->func_decl.is_static) emit(e, "static ");

    emit_type(e, ret);
    emit(e, " ");
    EMIT_MANGLED_NAME(e, node->func_decl.name, node->func_decl.name_len);
    emit(e, "(");

    if (node->func_decl.param_count == 0) {
        emit(e, "void");
    } else {
        for (int i = 0; i < node->func_decl.param_count; i++) {
            if (i > 0) emit(e, ", ");
            ParamDecl *p = &node->func_decl.params[i];
            Type *ptype = (func_type && func_type->kind == TYPE_FUNC_PTR &&
                          (uint32_t)i < func_type->func_ptr.param_count) ?
                func_type->func_ptr.params[i] : resolve_tynode(e,p->type);
            emit_type_and_name(e, ptype, p->name, p->name_len);
        }
    }
    emit(e, ") ");

    if (node->func_decl.body) {
        e->current_func_ret = ret;
        emit_stmt(e, node->func_decl.body);
        e->current_func_ret = NULL;
    } else {
        emit(e, ";\n");
    }
    emit(e, "\n");
}

static void emit_global_var(Emitter *e, Node *node) {
    Type *type = checker_get_type(e->checker,node);
    /* threadlocal */
    if (node->var_decl.is_threadlocal) {
        emit(e, "__thread ");
    }
    /* section attribute */
    if (node->var_decl.section) {
        emit(e, "__attribute__((section(\"%.*s\"))) ",
             (int)node->var_decl.section_len, node->var_decl.section);
    }
    /* propagate volatile flag from var-decl to pointer type */
    if (node->var_decl.is_volatile && type && type_unwrap_distinct(type)->kind == TYPE_POINTER) {
        Type *tp = type_unwrap_distinct(type);
        Type *vp = type_pointer(e->arena, tp->pointer.inner);
        vp->pointer.is_volatile = true;
        type = vp;
    }

    /* Pool(T, N) → use macro for struct layout */
    if (type && type->kind == TYPE_POOL) {
        emit(e, "struct { ");
        emit_type(e, type->pool.elem);
        emit(e, " slots[%llu]; uint32_t gen[%llu]; uint8_t used[%llu]; } ",
             (unsigned long long)type->pool.count, (unsigned long long)type->pool.count, (unsigned long long)type->pool.count);
        emit(e, "%.*s = {0};\n\n",
             (int)node->var_decl.name_len, node->var_decl.name);
        return;
    }

    /* Ring(T, N) → ring struct */
    if (type && type->kind == TYPE_RING) {
        emit(e, "struct { ");
        emit_type(e, type->ring.elem);
        emit(e, " data[%llu]; uint32_t head; uint32_t tail; uint32_t count; } ",
             (unsigned long long)type->ring.count);
        emit(e, "%.*s = {0};\n\n",
             (int)node->var_decl.name_len, node->var_decl.name);
        return;
    }

    /* Arena → _zer_arena */
    if (type && type->kind == TYPE_ARENA) {
        emit(e, "_zer_arena %.*s",
             (int)node->var_decl.name_len, node->var_decl.name);
        if (node->var_decl.init) {
            emit(e, " = ");
            emit_expr(e, node->var_decl.init);
        } else {
            emit(e, " = {0}");
        }
        emit(e, ";\n\n");
        return;
    }

    /* Slab(T) → _zer_slab with slot_size */
    if (type && type->kind == TYPE_SLAB) {
        emit(e, "_zer_slab %.*s = { .slot_size = sizeof(",
             (int)node->var_decl.name_len, node->var_decl.name);
        emit_type(e, type->slab.elem);
        emit(e, ") };\n\n");
        return;
    }

    if (node->var_decl.is_static) emit(e, "static ");
    /* volatile on non-pointer scalars (pointers handled above) */
    if (node->var_decl.is_volatile && !(type && type_unwrap_distinct(type)->kind == TYPE_POINTER))
        emit(e, "volatile ");

    /* BUG-218/222: mangle global var names for imported modules (including static) */
    if (e->current_module) {
        /* RF4: arena-allocated — no fixed buffer limit
         * BUG-332: double underscore __ separator */
        uint32_t mlen = e->current_module_len + 2 + (uint32_t)node->var_decl.name_len;
        char *mangled = (char *)arena_alloc(e->arena, mlen + 1);
        memcpy(mangled, e->current_module, e->current_module_len);
        mangled[e->current_module_len] = '_';
        mangled[e->current_module_len + 1] = '_';
        memcpy(mangled + e->current_module_len + 2, node->var_decl.name, node->var_decl.name_len);
        mangled[mlen] = '\0';
        emit_type_and_name(e, type, mangled, (int)mlen);
    } else {
        emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
    }

    if (node->var_decl.init) {
        /* optional null init needs struct literal, not scalar 0.
         * BUG-506: unwrap distinct — distinct typedef ?T is still optional. */
        Type *gtype_eff = type ? type_unwrap_distinct(type) : NULL;
        if (gtype_eff && gtype_eff->kind == TYPE_OPTIONAL &&
            !is_null_sentinel(gtype_eff->optional.inner) &&
            node->var_decl.init->kind == NODE_NULL_LIT) {
            emit(e, " = ");
            emit_opt_null_literal(e, type);
        } else {
            /* For const globals: try compile-time evaluation first.
             * This avoids GCC statement expression errors from _zer_shl/shr
             * macros which can't be used in global initializers. */
            bool emitted_const = false;
            if (node->var_decl.is_const) {
                int64_t cval = eval_const_expr(node->var_decl.init);
                if (cval != CONST_EVAL_FAIL) {
                    if (cval < 0) {
                        emit(e, " = (%lld)", (long long)cval);
                    } else {
                        emit(e, " = %llu", (unsigned long long)cval);
                    }
                    emitted_const = true;
                }
            }
            if (!emitted_const) {
                emit(e, " = ");
                emit_expr(e, node->var_decl.init);
            }
        }
    } else {
        /* auto-zero — unwrap distinct to check if compound init needed */
        Type *eff_type = type_unwrap_distinct(type);
        if (eff_type && (eff_type->kind == TYPE_STRUCT || eff_type->kind == TYPE_ARRAY ||
                     eff_type->kind == TYPE_OPTIONAL || eff_type->kind == TYPE_UNION ||
                     eff_type->kind == TYPE_BARRIER || eff_type->kind == TYPE_SEMAPHORE ||
                     eff_type->kind == TYPE_SLICE)) {
            if (eff_type->kind == TYPE_SEMAPHORE)
                emit(e, " = { .count = %u }", (unsigned)eff_type->semaphore.count);
            else if (eff_type->kind == TYPE_STRUCT && eff_type->struct_type.field_count == 0)
                emit(e, " = {}");
            else
                emit(e, " = {0}");
        } else {
            emit(e, " = 0");
        }
    }
    emit(e, ";\n\n");
}

/* ================================================================
 * ENTRY POINT
 * ================================================================ */

void emitter_init(Emitter *e, FILE *out, Arena *arena, Checker *checker) {
    memset(e, 0, sizeof(Emitter));
    e->out = out;
    e->arena = arena;
    e->checker = checker;
}

/* ================================================================
 * Spawn wrapper pre-scan + emission
 *
 * pthread_create requires a void*(*)(void*) function pointer.
 * ZER functions have typed params. So we emit a wrapper function at
 * file scope that unpacks the arg struct and calls the real function.
 *
 * Phase 1: pre-scan AST to find all NODE_SPAWN, assign IDs, record them.
 * Phase 2: emit wrapper functions (arg struct typedef + wrapper) at file scope.
 * Phase 3: in NODE_SPAWN emission, reference the wrapper by ID.
 * ================================================================ */

static void prescan_spawn_in_node(Emitter *e, Node *node);

static void prescan_spawn_in_block(Emitter *e, Node *block) {
    if (!block || block->kind != NODE_BLOCK) return;
    for (int i = 0; i < block->block.stmt_count; i++)
        prescan_spawn_in_node(e, block->block.stmts[i]);
}

static void register_condvar_type(Emitter *e, uint32_t type_id) {
    /* Check if already registered */
    for (int i = 0; i < e->condvar_type_count; i++)
        if (e->condvar_type_ids[i] == type_id) return;
    if (e->condvar_type_count >= e->condvar_type_capacity) {
        int nc = e->condvar_type_capacity < 8 ? 8 : e->condvar_type_capacity * 2;
        uint32_t *nids = (uint32_t *)arena_alloc(e->arena, nc * sizeof(uint32_t));
        if (e->condvar_type_ids)
            memcpy(nids, e->condvar_type_ids, e->condvar_type_count * sizeof(uint32_t));
        e->condvar_type_ids = nids;
        e->condvar_type_capacity = nc;
    }
    e->condvar_type_ids[e->condvar_type_count++] = type_id;
}

static bool is_condvar_type(Emitter *e, uint32_t type_id) {
    for (int i = 0; i < e->condvar_type_count; i++)
        if (e->condvar_type_ids[i] == type_id) return true;
    return false;
}

static void prescan_spawn_in_node(Emitter *e, Node *node) {
    if (!node) return;
    switch (node->kind) {
    case NODE_SPAWN: {
        /* Register this spawn for wrapper emission */
        if (e->spawn_wrapper_count >= e->spawn_wrapper_capacity) {
            int nc = e->spawn_wrapper_capacity < 8 ? 8 : e->spawn_wrapper_capacity * 2;
            SpawnWrapper *nw = (SpawnWrapper *)arena_alloc(e->arena, nc * sizeof(SpawnWrapper));
            if (e->spawn_wrappers)
                memcpy(nw, e->spawn_wrappers, e->spawn_wrapper_count * sizeof(SpawnWrapper));
            e->spawn_wrappers = nw;
            e->spawn_wrapper_capacity = nc;
        }
        SpawnWrapper *sw = &e->spawn_wrappers[e->spawn_wrapper_count++];
        sw->id = e->next_spawn_id++;
        sw->spawn_node = node;
        break;
    }
    case NODE_EXPR_STMT:
        /* Detect @cond_wait/@cond_signal/@cond_broadcast usage */
        if (node->expr_stmt.expr && node->expr_stmt.expr->kind == NODE_INTRINSIC) {
            Node *intr = node->expr_stmt.expr;
            if (intr->intrinsic.name_len >= 5 &&
                memcmp(intr->intrinsic.name, "cond_", 5) == 0 &&
                intr->intrinsic.arg_count >= 1) {
                /* First arg is the shared struct variable — get its type */
                Type *stype = checker_get_type(e->checker, intr->intrinsic.args[0]);
                if (stype) {
                    Type *seff = type_unwrap_distinct(stype);
                    if (seff->kind == TYPE_STRUCT && seff->struct_type.is_shared)
                        register_condvar_type(e, seff->struct_type.type_id);
                    if (seff->kind == TYPE_POINTER) {
                        Type *inner = type_unwrap_distinct(seff->pointer.inner);
                        if (inner && inner->kind == TYPE_STRUCT && inner->struct_type.is_shared)
                            register_condvar_type(e, inner->struct_type.type_id);
                    }
                }
            }
        }
        break;
    case NODE_BLOCK: prescan_spawn_in_block(e, node); break;
    case NODE_IF:
        prescan_spawn_in_node(e, node->if_stmt.then_body);
        prescan_spawn_in_node(e, node->if_stmt.else_body);
        break;
    case NODE_FOR: prescan_spawn_in_node(e, node->for_stmt.body); break;
    case NODE_WHILE: case NODE_DO_WHILE: prescan_spawn_in_node(e, node->while_stmt.body); break;
    case NODE_SWITCH:
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            prescan_spawn_in_node(e, node->switch_stmt.arms[i].body);
        break;
    case NODE_FUNC_DECL:
        prescan_spawn_in_node(e, node->func_decl.body);
        break;
    case NODE_INTERRUPT:
        prescan_spawn_in_node(e, node->interrupt.body);
        break;
    case NODE_DEFER:
        prescan_spawn_in_node(e, node->defer.body);
        break;
    case NODE_CRITICAL:
        prescan_spawn_in_node(e, node->critical.body);
        break;
    case NODE_ONCE:
        prescan_spawn_in_node(e, node->once.body);
        break;
    case NODE_YIELD: case NODE_AWAIT: case NODE_STATIC_ASSERT:
        break;
    default: break;
    }
}

static void emit_type(Emitter *e, Type *t); /* forward decl */

static void emit_spawn_wrappers(Emitter *e) {
    if (e->spawn_wrapper_count == 0) return;

    /* Forward-declare target functions so wrappers can call them */
    emit(e, "\n/* ZER spawn target forward declarations */\n");
    for (int wi = 0; wi < e->spawn_wrapper_count; wi++) {
        SpawnWrapper *sw = &e->spawn_wrappers[wi];
        Node *sn = sw->spawn_node;
        Symbol *fsym = scope_lookup(e->checker->global_scope,
            sn->spawn_stmt.func_name, (uint32_t)sn->spawn_stmt.func_name_len);
        if (fsym && fsym->type && fsym->type->kind == TYPE_FUNC_PTR) {
            /* Emit return type + name + params */
            Type *ft = fsym->type;
            emit_type(e, ft->func_ptr.ret);
            emit(e, " %.*s(", (int)sn->spawn_stmt.func_name_len, sn->spawn_stmt.func_name);
            for (uint32_t pi = 0; pi < ft->func_ptr.param_count; pi++) {
                if (pi > 0) emit(e, ", ");
                emit_type(e, ft->func_ptr.params[pi]);
            }
            emit(e, ");\n");
        } else if (fsym && fsym->func_node) {
            /* Use func_node to get the prototype */
            Node *fn = fsym->func_node;
            if (fn->kind == NODE_FUNC_DECL) {
                Type *ret = checker_get_type(e->checker, fn);
                if (ret && ret->kind == TYPE_FUNC_PTR) {
                    emit_type(e, ret->func_ptr.ret);
                    emit(e, " %.*s(", (int)sn->spawn_stmt.func_name_len, sn->spawn_stmt.func_name);
                    for (uint32_t pi = 0; pi < ret->func_ptr.param_count; pi++) {
                        if (pi > 0) emit(e, ", ");
                        emit_type(e, ret->func_ptr.params[pi]);
                    }
                    emit(e, ");\n");
                } else {
                    /* Fallback: just emit void func_name(); */
                    emit(e, "void %.*s();\n",
                         (int)sn->spawn_stmt.func_name_len, sn->spawn_stmt.func_name);
                }
            }
        }
    }

    emit(e, "\n/* ZER spawn thread wrappers */\n");
    for (int wi = 0; wi < e->spawn_wrapper_count; wi++) {
        SpawnWrapper *sw = &e->spawn_wrappers[wi];
        Node *sn = sw->spawn_node;
        int sid = sw->id;
        int ac = sn->spawn_stmt.arg_count;

        if (ac > 0) {
            /* Emit arg struct typedef */
            emit(e, "struct _zer_spawn_args_%d { ", sid);
            for (int i = 0; i < ac; i++) {
                Type *at = checker_get_type(e->checker, sn->spawn_stmt.args[i]);
                if (at) {
                    /* BUG-465: use emit_type_and_name with actual field name.
                     * For function pointers, name must be inside (*name)(params).
                     * Passing NULL + separate name breaks funcptr emission. */
                    char fname[8];
                    int flen = snprintf(fname, sizeof(fname), "a%d", i);
                    emit_type_and_name(e, at, fname, flen);
                    emit(e, "; ");
                }
            }
            emit(e, "};\n");
        }

        /* Emit wrapper function */
        emit(e, "static void *_zer_spawn_wrap_%d(void *_raw) {\n", sid);
        if (ac > 0) {
            emit(e, "    struct _zer_spawn_args_%d *_a = (struct _zer_spawn_args_%d *)_raw;\n", sid, sid);
            emit(e, "    %.*s(", (int)sn->spawn_stmt.func_name_len, sn->spawn_stmt.func_name);
            for (int i = 0; i < ac; i++) {
                if (i > 0) emit(e, ", ");
                emit(e, "_a->a%d", i);
            }
            emit(e, ");\n");
            emit(e, "    free(_a);\n");
        } else {
            emit(e, "    %.*s();\n", (int)sn->spawn_stmt.func_name_len, sn->spawn_stmt.func_name);
        }
        emit(e, "    return NULL;\n");
        emit(e, "}\n");
    }
    emit(e, "\n");
}

/* RF2: unified top-level declaration emitter — used by both emit_file and emit_file_no_preamble.
 * Previously these were two parallel switch statements that had to stay in sync (BUG-086/087 class). */
static void emit_top_level_decl(Emitter *e, Node *decl, Node *file_node, int decl_index) {
    switch (decl->kind) {
    case NODE_STRUCT_DECL:
        emit_struct_decl(e, decl);
        break;

    case NODE_FUNC_DECL:
        /* comptime functions are compile-time only — no C emission */
        if (decl->func_decl.is_comptime) break;
        if (!decl->func_decl.body) {
            /* Forward declaration: check if definition exists later in same file */
            bool has_def = false;
            for (int j = decl_index + 1; j < file_node->file.decl_count; j++) {
                Node *other = file_node->file.decls[j];
                if (other->kind == NODE_FUNC_DECL && other->func_decl.body &&
                    other->func_decl.name_len == decl->func_decl.name_len &&
                    memcmp(other->func_decl.name, decl->func_decl.name,
                           decl->func_decl.name_len) == 0) {
                    has_def = true;
                    break;
                }
            }
            if (has_def) {
                /* Definition exists later — emit prototype so earlier functions
                 * can call it (e.g., mutual recursion). */
                emit_func_decl(e, decl);
                break;
            }
            /* No body in this file — emit C prototype so GCC knows the
             * return type and parameter types. Without prototype, GCC assumes
             * int return which fails for bool/struct/optional types.
             * Skip well-known C stdlib names to avoid conflicting prototypes. */
            {
                const char *n = decl->func_decl.name;
                size_t nl = decl->func_decl.name_len;
                bool is_cstdlib = (nl == 4 && memcmp(n, "puts", 4) == 0) ||
                                  (nl == 6 && memcmp(n, "printf", 6) == 0) ||
                                  (nl == 7 && memcmp(n, "fprintf", 7) == 0) ||
                                  (nl == 6 && memcmp(n, "malloc", 6) == 0) ||
                                  (nl == 6 && memcmp(n, "calloc", 6) == 0) ||
                                  (nl == 7 && memcmp(n, "realloc", 7) == 0) ||
                                  (nl == 6 && memcmp(n, "strdup", 6) == 0) ||
                                  (nl == 7 && memcmp(n, "strndup", 7) == 0) ||
                                  (nl == 4 && memcmp(n, "free", 4) == 0) ||
                                  (nl == 6 && memcmp(n, "memcpy", 6) == 0) ||
                                  (nl == 6 && memcmp(n, "memset", 6) == 0) ||
                                  (nl == 6 && memcmp(n, "strlen", 6) == 0) ||
                                  (nl == 7 && memcmp(n, "putchar", 7) == 0) ||
                                  (nl == 5 && memcmp(n, "fputc", 5) == 0) ||
                                  (nl == 7 && memcmp(n, "memmove", 7) == 0) ||
                                  (nl == 6 && memcmp(n, "memchr", 6) == 0) ||
                                  (nl == 7 && memcmp(n, "bsearch", 7) == 0) ||
                                  (nl == 5 && memcmp(n, "qsort", 5) == 0);
                if (!is_cstdlib) emit_func_decl(e, decl);
            }
            break;
        }
        emit_func_decl(e, decl);
        break;

    case NODE_GLOBAL_VAR:
        emit_global_var(e, decl);
        break;

    case NODE_UNION_DECL: {
        Type *ut = checker_get_type(e->checker, decl);
        /* B8: macro for the 12× repeated union name emission pattern */
        #define EMIT_UNAME() do { \
            if (ut) EMIT_UNION_NAME(e, ut); \
            else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name); \
        } while(0)
        emit(e, "/* tagged union %.*s */\n",
             (int)decl->union_decl.name_len, decl->union_decl.name);
        for (int j = 0; j < decl->union_decl.variant_count; j++) {
            UnionVariant *v = &decl->union_decl.variants[j];
            emit(e, "#define _ZER_");
            EMIT_UNAME();
            emit(e, "_TAG_%.*s %d\n", (int)v->name_len, v->name, j);
        }
        emit(e, "struct _union_"); EMIT_UNAME();
        emit(e, " {\n    int32_t _tag;\n    union {\n");
        for (int j = 0; j < decl->union_decl.variant_count; j++) {
            UnionVariant *v = &decl->union_decl.variants[j];
            Type *vtype = resolve_tynode(e,v->type);
            emit(e, "        ");
            emit_type_and_name(e, vtype, v->name, (int)v->name_len);
            emit(e, ";\n");
        }
        emit(e, "    };\n};\n");
        /* optional/slice/opt-slice typedefs */
        emit(e, "typedef struct { struct _union_"); EMIT_UNAME();
        emit(e, " value; uint8_t has_value; } _zer_opt_"); EMIT_UNAME();
        emit(e, ";\n");
        emit(e, "typedef struct { struct _union_"); EMIT_UNAME();
        emit(e, "* ptr; size_t len; } _zer_slice_"); EMIT_UNAME();
        emit(e, ";\n");
        emit(e, "typedef struct { volatile struct _union_"); EMIT_UNAME();
        emit(e, "* ptr; size_t len; } _zer_vslice_"); EMIT_UNAME();
        emit(e, ";\n");
        emit(e, "typedef struct { _zer_slice_"); EMIT_UNAME();
        emit(e, " value; uint8_t has_value; } _zer_opt_slice_"); EMIT_UNAME();
        emit(e, ";\n\n");
        break;
        #undef EMIT_UNAME
    }

    case NODE_ENUM_DECL: {
        Type *et = checker_get_type(e->checker, decl);
        emit(e, "/* enum %.*s */\n",
             (int)decl->enum_decl.name_len, decl->enum_decl.name);
        int32_t next_val = 0;
        for (int j = 0; j < decl->enum_decl.variant_count; j++) {
            EnumVariant *v = &decl->enum_decl.variants[j];
            int32_t val;
            if (v->value && v->value->kind == NODE_INT_LIT) {
                val = (int32_t)v->value->int_lit.value;
                next_val = val + 1;
            } else if (v->value && v->value->kind == NODE_UNARY &&
                       v->value->unary.op == TOK_MINUS &&
                       v->value->unary.operand->kind == NODE_INT_LIT) {
                val = -(int32_t)v->value->unary.operand->int_lit.value;
                next_val = val + 1;
            } else {
                val = next_val++;
            }
            emit(e, "#define _ZER_");
            if (et) EMIT_ENUM_NAME(e, et);
            else emit(e, "%.*s", (int)decl->enum_decl.name_len, decl->enum_decl.name);
            emit(e, "_%.*s %d\n", (int)v->name_len, v->name, val);
        }
        emit(e, "\n");
        break;
    }

    case NODE_IMPORT:
        emit(e, "/* import %.*s — TODO */\n\n",
             (int)decl->import.module_name_len, decl->import.module_name);
        break;

    case NODE_CINCLUDE:
        /* cinclude "<stdio.h>" → #include <stdio.h>  (system header)
         * cinclude "myheader.h" → #include "myheader.h" (local header) */
        if (decl->cinclude.path_len >= 2 &&
            decl->cinclude.path[0] == '<' &&
            decl->cinclude.path[decl->cinclude.path_len - 1] == '>') {
            emit(e, "#include %.*s\n",
                 (int)decl->cinclude.path_len, decl->cinclude.path);
        } else {
            emit(e, "#include \"%.*s\"\n",
                 (int)decl->cinclude.path_len, decl->cinclude.path);
        }
        break;

    case NODE_INTERRUPT:
        emit(e, "void __attribute__((interrupt)) %.*s_IRQHandler(void) ",
             (int)decl->interrupt.name_len, decl->interrupt.name);
        if (decl->interrupt.body) {
            emit_stmt(e, decl->interrupt.body);
        }
        emit(e, "\n");
        break;

    case NODE_MMIO:
        /* mmio ranges are compile-time only — emit as comment */
        emit(e, "/* mmio 0x%llx..0x%llx */\n",
             (unsigned long long)decl->mmio_decl.range_start,
             (unsigned long long)decl->mmio_decl.range_end);
        break;

    case NODE_TYPEDEF: {
        Type *td_type = checker_get_type(e->checker, decl);
        Type *underlying = td_type;
        if (td_type && td_type->kind == TYPE_DISTINCT)
            underlying = td_type->distinct.underlying;
        emit(e, "typedef ");
        emit_type_and_name(e, underlying,
            decl->typedef_decl.name, decl->typedef_decl.name_len);
        emit(e, ";\n\n");
        break;
    }

    /* Non-top-level nodes — emit_top_level_decl only handles declarations */
    case NODE_VAR_DECL: case NODE_BLOCK: case NODE_IF: case NODE_FOR:
    case NODE_WHILE: case NODE_SWITCH: case NODE_RETURN: case NODE_BREAK:
    case NODE_CONTINUE: case NODE_DEFER: case NODE_GOTO: case NODE_LABEL:
    case NODE_EXPR_STMT: case NODE_ASM: case NODE_CRITICAL: case NODE_ONCE: case NODE_SPAWN:
    case NODE_YIELD: case NODE_AWAIT: case NODE_STATIC_ASSERT:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_IDENT: case NODE_BINARY: case NODE_UNARY: case NODE_ASSIGN:
    case NODE_CALL: case NODE_FIELD: case NODE_INDEX: case NODE_SLICE:
    case NODE_ORELSE: case NODE_INTRINSIC: case NODE_CAST: case NODE_TYPECAST:
    case NODE_SIZEOF: case NODE_STRUCT_INIT: case NODE_CONTAINER_DECL: case NODE_FILE:
        break;
    }
}

/* has_inttoptr removed — auto-discovery removed (2026-04-01 decision).
 * mmio validation now uses startup @probe of declared ranges instead. */

/* Unified file emitter — one flow for both preamble and non-preamble modules.
 * Prevents BUG-472 class: prescan/setup steps can't be forgotten for one path. */
void emit_file_module(Emitter *e, Node *file_node, bool with_preamble) {
    if (!file_node || file_node->kind != NODE_FILE) return;

    if (!with_preamble) {
        /* Non-preamble module: prescan + two-pass emit (same as preamble path) */
        emit(e, "\n/* --- imported module --- */\n\n");

        /* Pre-scan for spawn (same as preamble path) */
        for (int i = 0; i < file_node->file.decl_count; i++)
            prescan_spawn_in_node(e, file_node->file.decls[i]);

        /* Pass 1: struct/enum/union/typedef declarations */
        for (int i = 0; i < file_node->file.decl_count; i++) {
            Node *d = file_node->file.decls[i];
            if (d->kind == NODE_IMPORT || d->kind == NODE_CINCLUDE) continue;
            if (d->kind == NODE_STRUCT_DECL || d->kind == NODE_ENUM_DECL ||
                d->kind == NODE_UNION_DECL || d->kind == NODE_TYPEDEF)
                emit_top_level_decl(e, d, file_node, i);
        }

        /* Emit stamped container struct declarations */
        emit_container_structs(e);

        /* Spawn wrappers (between struct decls and functions) */
        emit_spawn_wrappers(e);

        /* Pass 2: functions, globals */
        for (int i = 0; i < file_node->file.decl_count; i++) {
            Node *d = file_node->file.decls[i];
            if (d->kind == NODE_IMPORT || d->kind == NODE_CINCLUDE) continue;
            if (d->kind != NODE_STRUCT_DECL && d->kind != NODE_ENUM_DECL &&
                d->kind != NODE_UNION_DECL && d->kind != NODE_TYPEDEF &&
                d->kind != NODE_CONTAINER_DECL)
                emit_top_level_decl(e, d, file_node, i);
        }
        return;
    }

    /* C preamble */
    emit(e, "/* Generated by ZER compiler — do not edit */\n");
    emit(e, "/* Compile with: gcc -std=c99 -fwrapv -fno-strict-aliasing */\n");
    emit(e, "#ifndef _POSIX_C_SOURCE\n");
    emit(e, "#define _POSIX_C_SOURCE 200112L\n");
    emit(e, "#define _XOPEN_SOURCE 500\n"); /* BUG-473: PTHREAD_MUTEX_RECURSIVE */
    emit(e, "#endif\n");
    emit(e, "#include <stdint.h>\n");
    emit(e, "#include <stddef.h>\n");
    emit(e, "#include <string.h>\n");
    emit(e, "#include <stdio.h>\n");
    emit(e, "#include <stdlib.h>\n");
    emit(e, "#if defined(__STDC_HOSTED__) && __STDC_HOSTED__\n");
    emit(e, "#include <pthread.h>\n");
    emit(e, "#include <time.h>\n");
    emit(e, "#endif\n");
    emit(e, "\n");

    /* ZER optional type definitions */
    emit(e, "/* ZER optional types */\n");
    emit(e, "typedef struct { uint8_t value; uint8_t has_value; } _zer_opt_bool;\n");
    emit(e, "typedef struct { uint8_t value; uint8_t has_value; } _zer_opt_u8;\n");
    emit(e, "typedef struct { uint16_t value; uint8_t has_value; } _zer_opt_u16;\n");
    emit(e, "typedef struct { uint32_t value; uint8_t has_value; } _zer_opt_u32;\n");
    emit(e, "typedef struct { uint64_t value; uint8_t has_value; } _zer_opt_u64;\n");
    emit(e, "typedef struct { int8_t value; uint8_t has_value; } _zer_opt_i8;\n");
    emit(e, "typedef struct { int16_t value; uint8_t has_value; } _zer_opt_i16;\n");
    emit(e, "typedef struct { int32_t value; uint8_t has_value; } _zer_opt_i32;\n");
    emit(e, "typedef struct { int64_t value; uint8_t has_value; } _zer_opt_i64;\n");
    emit(e, "typedef struct { size_t value; uint8_t has_value; } _zer_opt_usize;\n");
    emit(e, "typedef struct { float value; uint8_t has_value; } _zer_opt_f32;\n");
    emit(e, "typedef struct { double value; uint8_t has_value; } _zer_opt_f64;\n");
    emit(e, "typedef struct { uint8_t has_value; } _zer_opt_void;\n");
    emit(e, "\n");

    /* BUG-393: tagged opaque pointer — runtime provenance checking */
    emit(e, "/* ZER opaque pointer with runtime type tag */\n");
    emit(e, "typedef struct { void *ptr; uint32_t type_id; } _zer_opaque;\n");
    emit(e, "typedef struct { _zer_opaque value; uint8_t has_value; } _zer_opt_opaque;\n");
    emit(e, "\n");

    /* ZER slice types — all primitives */
    emit(e, "/* ZER slice types */\n");
    emit(e, "typedef struct { uint8_t* ptr; size_t len; } _zer_slice_u8;\n");
    emit(e, "typedef struct { uint16_t* ptr; size_t len; } _zer_slice_u16;\n");
    emit(e, "typedef struct { uint32_t* ptr; size_t len; } _zer_slice_u32;\n");
    emit(e, "typedef struct { uint64_t* ptr; size_t len; } _zer_slice_u64;\n");
    emit(e, "typedef struct { int8_t* ptr; size_t len; } _zer_slice_i8;\n");
    emit(e, "typedef struct { int16_t* ptr; size_t len; } _zer_slice_i16;\n");
    emit(e, "typedef struct { int32_t* ptr; size_t len; } _zer_slice_i32;\n");
    emit(e, "typedef struct { int64_t* ptr; size_t len; } _zer_slice_i64;\n");
    emit(e, "typedef struct { size_t* ptr; size_t len; } _zer_slice_usize;\n");
    emit(e, "typedef struct { float* ptr; size_t len; } _zer_slice_f32;\n");
    emit(e, "typedef struct { double* ptr; size_t len; } _zer_slice_f64;\n");
    emit(e, "\n");
    /* ZER volatile slice types — volatile []T for all primitives */
    emit(e, "typedef struct { volatile uint8_t* ptr; size_t len; } _zer_vslice_u8;\n");
    emit(e, "typedef struct { volatile uint16_t* ptr; size_t len; } _zer_vslice_u16;\n");
    emit(e, "typedef struct { volatile uint32_t* ptr; size_t len; } _zer_vslice_u32;\n");
    emit(e, "typedef struct { volatile uint64_t* ptr; size_t len; } _zer_vslice_u64;\n");
    emit(e, "typedef struct { volatile int8_t* ptr; size_t len; } _zer_vslice_i8;\n");
    emit(e, "typedef struct { volatile int16_t* ptr; size_t len; } _zer_vslice_i16;\n");
    emit(e, "typedef struct { volatile int32_t* ptr; size_t len; } _zer_vslice_i32;\n");
    emit(e, "typedef struct { volatile int64_t* ptr; size_t len; } _zer_vslice_i64;\n");
    emit(e, "typedef struct { volatile size_t* ptr; size_t len; } _zer_vslice_usize;\n");
    emit(e, "typedef struct { volatile float* ptr; size_t len; } _zer_vslice_f32;\n");
    emit(e, "typedef struct { volatile double* ptr; size_t len; } _zer_vslice_f64;\n");
    emit(e, "\n");
    /* ZER optional-slice types — ?[]T for all primitives */
    emit(e, "typedef struct { _zer_slice_u8 value; uint8_t has_value; } _zer_opt_slice_u8;\n");
    emit(e, "typedef struct { _zer_slice_u16 value; uint8_t has_value; } _zer_opt_slice_u16;\n");
    emit(e, "typedef struct { _zer_slice_u32 value; uint8_t has_value; } _zer_opt_slice_u32;\n");
    emit(e, "typedef struct { _zer_slice_u64 value; uint8_t has_value; } _zer_opt_slice_u64;\n");
    emit(e, "typedef struct { _zer_slice_i8 value; uint8_t has_value; } _zer_opt_slice_i8;\n");
    emit(e, "typedef struct { _zer_slice_i16 value; uint8_t has_value; } _zer_opt_slice_i16;\n");
    emit(e, "typedef struct { _zer_slice_i32 value; uint8_t has_value; } _zer_opt_slice_i32;\n");
    emit(e, "typedef struct { _zer_slice_i64 value; uint8_t has_value; } _zer_opt_slice_i64;\n");
    emit(e, "typedef struct { _zer_slice_usize value; uint8_t has_value; } _zer_opt_slice_usize;\n");
    emit(e, "typedef struct { _zer_slice_f32 value; uint8_t has_value; } _zer_opt_slice_f32;\n");
    emit(e, "typedef struct { _zer_slice_f64 value; uint8_t has_value; } _zer_opt_slice_f64;\n");
    emit(e, "\n");

    /* ZER runtime: Pool helper macros */
    emit(e, "/* ZER Pool runtime — BUG-390: u64 handles, u32 gen */\n");
    emit(e, "#define _ZER_POOL_DECL(NAME, ELEM_TYPE, CAPACITY) \\\n");
    emit(e, "    struct { \\\n");
    emit(e, "        ELEM_TYPE slots[CAPACITY]; \\\n");
    emit(e, "        uint32_t gen[CAPACITY]; \\\n");
    emit(e, "        uint8_t used[CAPACITY]; \\\n");
    emit(e, "    } NAME = {0}\n");
    emit(e, "\n");
    emit(e, "static inline uint64_t _zer_pool_alloc(void *pool_ptr, size_t slot_size, "
            "uint32_t *gen, uint8_t *used, size_t capacity, uint8_t *ok) {\n");
    emit(e, "    for (uint32_t i = 0; i < capacity; i++) {\n");
    emit(e, "        if (!used[i]) {\n");
    emit(e, "            used[i] = 1;\n");
    emit(e, "            if (gen[i] == 0) gen[i] = 1; /* skip 0: reserved for null handle */\n");
    emit(e, "            *ok = 1;\n");
    emit(e, "            return ((uint64_t)gen[i] << 32) | i;\n");
    emit(e, "        }\n");
    emit(e, "    }\n");
    emit(e, "    *ok = 0;\n");
    emit(e, "    return 0;\n");
    emit(e, "}\n\n");

    /* ZER trap — called on safety violations (use-after-free, bounds, etc.) */
    emit(e, "static void _zer_trap(const char *msg, const char *file, int line) {\n");
    emit(e, "#if defined(__arm__) || defined(__thumb__)\n");
    emit(e, "    (void)msg; (void)file; (void)line;\n");
    emit(e, "    __asm__ volatile(\"bkpt #0\"); for(;;) {}\n");
    emit(e, "#elif defined(__riscv)\n");
    emit(e, "    (void)msg; (void)file; (void)line;\n");
    emit(e, "    __asm__ volatile(\"ebreak\"); for(;;) {}\n");
    emit(e, "#elif defined(__AVR__)\n");
    emit(e, "    (void)msg; (void)file; (void)line;\n");
    emit(e, "    __asm__ volatile(\"break\"); for(;;) {}\n");
    emit(e, "#elif defined(__x86_64__) || defined(__i386__)\n");
    emit(e, "    fprintf(stderr, \"ZER TRAP: %%s at %%s:%%d\\n\", msg, file, line);\n");
    emit(e, "    __asm__ volatile(\"int3\");\n");
    emit(e, "#else\n");
    emit(e, "    fprintf(stderr, \"ZER TRAP: %%s at %%s:%%d\\n\", msg, file, line);\n");
    emit(e, "    abort();\n");
    emit(e, "#endif\n");
    emit(e, "}\n\n");

    /* ZER shared struct auto-locking — BUG-473: use recursive mutex.
     * Recursive mutex handles re-entrant locking when function A calls
     * function B that also auto-locks the same shared struct.
     * Lazy init: first lock call initializes with PTHREAD_MUTEX_RECURSIVE. */
    emit(e, "/* ZER shared struct auto-locking (recursive mutex) */\n");
    /* BUG-483: accept optional condvar pointer — init alongside mutex in CAS winner.
     * Fixes race where condvar init after ensure_init was always false. */
    emit(e, "static inline void _zer_mtx_ensure_init_cv(pthread_mutex_t *mtx, uint8_t *inited, pthread_cond_t *cond) {\n");
    emit(e, "    if (__atomic_load_n(inited, __ATOMIC_ACQUIRE) == 1) return;\n");
    emit(e, "    /* CAS 0→2: winner initializes mutex (+condvar). Losers spin until done (1). */\n");
    emit(e, "    uint8_t expected = 0;\n");
    emit(e, "    if (__atomic_compare_exchange_n(inited, &expected, 2, 0,\n");
    emit(e, "                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {\n");
    emit(e, "        pthread_mutexattr_t attr;\n");
    emit(e, "        pthread_mutexattr_init(&attr);\n");
    emit(e, "        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);\n");
    emit(e, "        pthread_mutex_init(mtx, &attr);\n");
    emit(e, "        pthread_mutexattr_destroy(&attr);\n");
    emit(e, "        if (cond) pthread_cond_init(cond, NULL);\n");
    emit(e, "        __atomic_store_n(inited, 1, __ATOMIC_RELEASE);\n");
    emit(e, "    } else {\n");
    emit(e, "        while (__atomic_load_n(inited, __ATOMIC_ACQUIRE) != 1) {}\n");
    emit(e, "    }\n");
    emit(e, "}\n");
    emit(e, "static inline void _zer_mtx_ensure_init(pthread_mutex_t *mtx, uint8_t *inited) {\n");
    emit(e, "    _zer_mtx_ensure_init_cv(mtx, inited, NULL);\n");
    emit(e, "}\n\n");

    /* ZER thread barrier — portable (mutex + condvar, like Rust) */
    emit(e, "/* ZER thread barrier */\n");
    emit(e, "#if defined(__STDC_HOSTED__) && __STDC_HOSTED__\n");
    emit(e, "typedef struct {\n");
    emit(e, "    pthread_mutex_t mtx;\n");
    emit(e, "    pthread_cond_t cond;\n");
    emit(e, "    uint32_t count;\n");
    emit(e, "    uint32_t target;\n");
    emit(e, "    uint32_t generation;\n");
    emit(e, "} _zer_barrier;\n");
    emit(e, "static inline void _zer_barrier_init(_zer_barrier *b, uint32_t n) {\n");
    emit(e, "    memset(b, 0, sizeof(*b));\n");
    emit(e, "    pthread_mutex_init(&b->mtx, NULL);\n");
    emit(e, "    pthread_cond_init(&b->cond, NULL);\n");
    emit(e, "    b->target = n;\n");
    emit(e, "}\n");
    emit(e, "static inline void _zer_barrier_wait(_zer_barrier *b) {\n");
    emit(e, "    pthread_mutex_lock(&b->mtx);\n");
    emit(e, "    uint32_t gen = b->generation;\n");
    emit(e, "    b->count++;\n");
    emit(e, "    if (b->count >= b->target) {\n");
    emit(e, "        b->count = 0;\n");
    emit(e, "        b->generation++;\n");
    emit(e, "        pthread_cond_broadcast(&b->cond);\n");
    emit(e, "    } else {\n");
    emit(e, "        while (gen == b->generation)\n");
    emit(e, "            pthread_cond_wait(&b->cond, &b->mtx);\n");
    emit(e, "    }\n");
    emit(e, "    pthread_mutex_unlock(&b->mtx);\n");
    emit(e, "}\n");
    /* Semaphore(N) — counting semaphore: shared struct with count + condvar */
    emit(e, "typedef struct {\n");
    emit(e, "    uint32_t count;\n");
    emit(e, "    pthread_mutex_t _zer_mtx;\n");
    emit(e, "    uint8_t _zer_mtx_inited;\n");
    emit(e, "    pthread_cond_t _zer_cond;\n");
    emit(e, "} _zer_semaphore;\n");
    emit(e, "static inline void _zer_sem_acquire(_zer_semaphore *s) {\n");
    emit(e, "    _zer_mtx_ensure_init_cv(&s->_zer_mtx, &s->_zer_mtx_inited, &s->_zer_cond);\n");
    emit(e, "    pthread_mutex_lock(&s->_zer_mtx);\n");
    emit(e, "    while (s->count == 0) pthread_cond_wait(&s->_zer_cond, &s->_zer_mtx);\n");
    emit(e, "    s->count--;\n");
    emit(e, "    pthread_mutex_unlock(&s->_zer_mtx);\n");
    emit(e, "}\n");
    emit(e, "static inline void _zer_sem_release(_zer_semaphore *s) {\n");
    emit(e, "    _zer_mtx_ensure_init_cv(&s->_zer_mtx, &s->_zer_mtx_inited, &s->_zer_cond);\n");
    emit(e, "    pthread_mutex_lock(&s->_zer_mtx);\n");
    emit(e, "    s->count++;\n");
    emit(e, "    pthread_cond_signal(&s->_zer_cond);\n");
    emit(e, "    pthread_mutex_unlock(&s->_zer_mtx);\n");
    emit(e, "}\n");
    emit(e, "#endif\n\n");

    /* Universal fault handler + @probe — uses C standard signal() everywhere.
     * No platform-specific #ifdef. Works on any OS and bare-metal with libc.
     * Dual-mode: during @probe → recover and return null.
     *            during normal code → trap with error message (catches bad MMIO). */
    /* Universal fault handler + @probe.
     * Hosted C (any OS, bare-metal with newlib/picolibc): uses signal() + setjmp().
     * Freestanding C (no libc): fault handler not installed, @probe unavailable.
     * __STDC_HOSTED__ is defined by GCC: 1 = hosted, 0 = freestanding.
     * Guard all signal/setjmp usage so freestanding compiles don't fail. */
    emit(e, "#if defined(__STDC_HOSTED__) && __STDC_HOSTED__ == 1\n");
    emit(e, "#include <setjmp.h>\n");
    emit(e, "#include <signal.h>\n\n");
    emit(e, "/* Universal memory fault handler — catches bad MMIO at runtime */\n");
    emit(e, "static volatile int _zer_in_probe = 0;\n");
    emit(e, "static jmp_buf _zer_probe_jmp;\n\n");
    emit(e, "static void _zer_fault_handler(int sig) {\n");
    emit(e, "    if (_zer_in_probe) {\n");
    emit(e, "        longjmp(_zer_probe_jmp, 1);\n");
    emit(e, "    }\n");
    emit(e, "    (void)sig;\n");
    emit(e, "    _zer_trap(\"memory access fault — invalid MMIO or pointer\", __FILE__, __LINE__);\n");
    emit(e, "}\n\n");
    emit(e, "__attribute__((constructor))\n");
    emit(e, "static void _zer_install_fault_handler(void) {\n");
    emit(e, "    signal(SIGSEGV, _zer_fault_handler);\n");
    emit(e, "#ifdef SIGBUS\n");
    emit(e, "    signal(SIGBUS, _zer_fault_handler);\n");
    emit(e, "#endif\n");
    emit(e, "}\n\n");
    emit(e, "static _zer_opt_u32 _zer_probe(uintptr_t addr) {\n");
    emit(e, "    _zer_in_probe = 1;\n");
    emit(e, "    if (setjmp(_zer_probe_jmp) != 0) {\n");
    emit(e, "        _zer_in_probe = 0;\n");
    emit(e, "        /* re-install handler — signal() resets to SIG_DFL after longjmp on some platforms */\n");
    emit(e, "        signal(SIGSEGV, _zer_fault_handler);\n");
    emit(e, "#ifdef SIGBUS\n");
    emit(e, "        signal(SIGBUS, _zer_fault_handler);\n");
    emit(e, "#endif\n");
    emit(e, "        return (_zer_opt_u32){ 0, 0 };\n");
    emit(e, "    }\n");
    emit(e, "    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;\n");
    emit(e, "    uint32_t val = *p;\n");
    emit(e, "    _zer_in_probe = 0;\n");
    emit(e, "    return (_zer_opt_u32){ val, 1 };\n");
    emit(e, "}\n\n");
    emit(e, "#else /* freestanding — no signal/setjmp, @probe unavailable */\n");
    emit(e, "static _zer_opt_u32 _zer_probe(uintptr_t addr) {\n");
    emit(e, "    /* freestanding: no fault handler, direct read (same as C) */\n");
    emit(e, "    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;\n");
    emit(e, "    uint32_t val = *p;\n");
    emit(e, "    return (_zer_opt_u32){ val, 1 };\n");
    emit(e, "}\n");
    emit(e, "#endif /* __STDC_HOSTED__ */\n\n");

    /* safe shift — ZER spec: shift by >= width returns 0 (not UB like C).
     * Uses GCC statement expression to evaluate b exactly once. */
    emit(e, "#define _zer_shl(a, b) ({ __typeof__(b) _b = (b); "
            "(_b >= (int)(sizeof(a) * 8)) ? (__typeof__(a))0 : (a) << _b; })\n");
    emit(e, "#define _zer_shr(a, b) ({ __typeof__(b) _b = (b); "
            "(_b >= (int)(sizeof(a) * 8)) ? (__typeof__(a))0 : (a) >> _b; })\n\n");

    /* bounds check helper — works in comma expressions (LHS and RHS safe) */
    emit(e, "static inline void _zer_bounds_check(size_t idx, size_t len, "
            "const char *file, int line) {\n");
    emit(e, "    if (idx >= len) _zer_trap(\"array index out of bounds\", file, line);\n");
    emit(e, "}\n\n");

    emit(e, "static inline void *_zer_pool_get(void *slots, uint32_t *gen, uint8_t *used, "
            "size_t slot_size, uint64_t handle, size_t capacity) {\n");
    emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
    emit(e, "    uint32_t h_gen = (uint32_t)(handle >> 32);\n");
    emit(e, "    if (idx >= capacity || !used[idx] || gen[idx] != h_gen) {\n");
    emit(e, "        _zer_trap(\"use-after-free: handle generation mismatch\", __FILE__, __LINE__);\n");
    emit(e, "    }\n");
    emit(e, "    return (char*)slots + idx * slot_size;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void _zer_pool_free(uint32_t *gen, uint8_t *used, "
            "uint64_t handle, size_t capacity) {\n");
    emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
    emit(e, "    if (idx < capacity) {\n");
    emit(e, "        used[idx] = 0;\n");
    emit(e, "        gen[idx]++;\n");
    emit(e, "        if (gen[idx] == 0) gen[idx] = 1; /* skip 0: reserved for null handle */\n");
    emit(e, "    }\n");
    emit(e, "}\n\n");

    /* ZER runtime: Ring helper */
    emit(e, "/* ZER Ring runtime */\n");
    emit(e, "#define _ZER_RING_DECL(NAME, ELEM_TYPE, CAPACITY) \\\n");
    emit(e, "    struct { \\\n");
    emit(e, "        ELEM_TYPE data[CAPACITY]; \\\n");
    emit(e, "        uint32_t head; \\\n");
    emit(e, "        uint32_t tail; \\\n");
    emit(e, "        uint32_t count; \\\n");
    emit(e, "        uint32_t capacity; \\\n");
    emit(e, "    } NAME = { .capacity = CAPACITY }\n");
    emit(e, "\n");

    emit(e, "static inline void _zer_ring_push(void *ring_data, uint32_t *head, uint32_t *tail, "
            "uint32_t *count, size_t capacity, const void *val, size_t elem_size) {\n");
    emit(e, "    memcpy((char*)ring_data + (*head) * elem_size, val, elem_size);\n");
    /* BUG-348: store barrier between data write and head update.
     * Ensures interrupt/other core sees data before updated head. */
    emit(e, "    __atomic_thread_fence(__ATOMIC_RELEASE);\n");
    emit(e, "    *head = (*head + 1) %% capacity;\n");
    emit(e, "    if (*count < capacity) { (*count)++; }\n");
    emit(e, "    else { *tail = (*tail + 1) %% capacity; }\n");
    emit(e, "}\n\n");

    /* ZER runtime: Arena bump allocator */
    emit(e, "/* ZER Arena runtime */\n");
    emit(e, "typedef struct { uint8_t *buf; size_t capacity; size_t offset; } _zer_arena;\n\n");

    emit(e, "static inline void *_zer_arena_alloc(_zer_arena *a, size_t size, size_t align) {\n");
    emit(e, "    size_t off = (a->offset + align - 1) & ~(align - 1);\n");
    emit(e, "    if (off + size > a->capacity) return (void*)0;\n");
    emit(e, "    a->offset = off + size;\n");
    emit(e, "    memset(a->buf + off, 0, size);\n");
    emit(e, "    return a->buf + off;\n");
    emit(e, "}\n\n");

    /* ZER runtime: Slab dynamic allocator — BUG-390: u64 handles, u32 gen */
    emit(e, "/* ZER Slab runtime — dynamic growable pool via mmap/malloc */\n");
    emit(e, "#define _ZER_SLAB_PAGE_SLOTS 64\n");
    emit(e, "typedef struct {\n");
    emit(e, "    char **pages;         /* array of page pointers */\n");
    emit(e, "    uint32_t *gen;        /* flat generation array */\n");
    emit(e, "    uint8_t *used;        /* flat used-slot array */\n");
    emit(e, "    size_t page_count;\n");
    emit(e, "    size_t page_cap;\n");
    emit(e, "    size_t total_slots;\n");
    emit(e, "    size_t slot_size;\n");
    emit(e, "} _zer_slab;\n\n");

    emit(e, "static inline uint64_t _zer_slab_alloc(_zer_slab *s, uint8_t *ok) {\n");
    emit(e, "    /* scan for free slot */\n");
    emit(e, "    for (uint32_t i = 0; i < s->total_slots; i++) {\n");
    emit(e, "        if (!s->used[i]) {\n");
    emit(e, "            s->used[i] = 1;\n");
    emit(e, "            if (s->gen[i] == 0) s->gen[i] = 1; /* zero handle must not match */\n");
    emit(e, "            *ok = 1;\n");
    emit(e, "            return ((uint64_t)s->gen[i] << 32) | i;\n");
    emit(e, "        }\n");
    emit(e, "    }\n");
    emit(e, "    /* grow: add a new page */\n");
    emit(e, "    if (s->page_count >= s->page_cap) {\n");
    emit(e, "        size_t nc = s->page_cap < 4 ? 4 : s->page_cap * 2;\n");
    emit(e, "        char **np = (char**)calloc(nc, sizeof(char*));\n");
    emit(e, "        uint32_t *ng = (uint32_t*)calloc((size_t)nc * _ZER_SLAB_PAGE_SLOTS, sizeof(uint32_t));\n");
    emit(e, "        uint8_t *nu = (uint8_t*)calloc((size_t)nc * _ZER_SLAB_PAGE_SLOTS, sizeof(uint8_t));\n");
    emit(e, "        if (!np || !ng || !nu) { *ok = 0; return 0; }\n");
    emit(e, "        if (s->pages) { memcpy(np, s->pages, s->page_count * sizeof(char*)); free(s->pages); }\n");
    emit(e, "        if (s->gen) { memcpy(ng, s->gen, s->total_slots * sizeof(uint32_t)); free(s->gen); }\n");
    emit(e, "        if (s->used) { memcpy(nu, s->used, s->total_slots * sizeof(uint8_t)); free(s->used); }\n");
    emit(e, "        s->pages = np; s->gen = ng; s->used = nu; s->page_cap = nc;\n");
    emit(e, "    }\n");
    emit(e, "    char *page = (char*)calloc(_ZER_SLAB_PAGE_SLOTS, s->slot_size);\n");
    emit(e, "    if (!page) { *ok = 0; return 0; }\n");
    emit(e, "    s->pages[s->page_count] = page;\n");
    emit(e, "    size_t base = s->total_slots;\n");
    emit(e, "    s->page_count++;\n");
    emit(e, "    s->total_slots += _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "    s->used[base] = 1;\n");
    emit(e, "    if (s->gen[base] == 0) s->gen[base] = 1; /* zero handle must not match */\n");
    emit(e, "    *ok = 1;\n");
    emit(e, "    return ((uint64_t)s->gen[base] << 32) | base;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void *_zer_slab_get(_zer_slab *s, uint64_t handle) {\n");
    emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
    emit(e, "    uint32_t gen = (uint32_t)(handle >> 32);\n");
    emit(e, "    if (idx >= s->total_slots || !s->used[idx] || s->gen[idx] != gen) {\n");
    emit(e, "        _zer_trap(\"slab: use-after-free or invalid handle\", __FILE__, __LINE__);\n");
    emit(e, "    }\n");
    emit(e, "    size_t page = idx / _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "    size_t slot = idx %% _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "    return s->pages[page] + slot * s->slot_size;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void _zer_slab_free(_zer_slab *s, uint64_t handle) {\n");
    emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
    emit(e, "    if (idx < s->total_slots) {\n");
    emit(e, "        s->used[idx] = 0;\n");
    emit(e, "        s->gen[idx]++;\n");
    emit(e, "        if (s->gen[idx] == 0) s->gen[idx] = 1; /* skip 0: reserved for null handle */\n");
    emit(e, "    }\n");
    emit(e, "}\n\n");

    emit(e, "static inline void _zer_slab_free_ptr(_zer_slab *s, void *ptr) {\n");
    emit(e, "    if (!ptr) return;\n");
    emit(e, "    for (size_t i = 0; i < s->total_slots; i++) {\n");
    emit(e, "        size_t page = i / _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "        size_t slot = i %% _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "        if (s->pages[page] + slot * s->slot_size == (char*)ptr) {\n");
    emit(e, "            s->used[i] = 0;\n");
    emit(e, "            s->gen[i]++;\n");
    emit(e, "            if (s->gen[i] == 0) s->gen[i] = 1;\n");
    emit(e, "            return;\n");
    emit(e, "        }\n");
    emit(e, "    }\n");
    emit(e, "    _zer_trap(\"slab: free_ptr with invalid pointer\", __FILE__, __LINE__);\n");
    emit(e, "}\n\n");

    /* Level 3+4+5: *opaque inline header tracking (--track-cptrs) */
    if (e->track_cptrs) {
        emit(e, "\n/* ZER *opaque tracking — inline header per allocation */\n");
        emit(e, "extern void *__real_malloc(size_t);\n");
        emit(e, "extern void __real_free(void *);\n");
        emit(e, "extern void *__real_realloc(void *, size_t);\n");
        emit(e, "static _Atomic uint32_t _zer_alloc_gen = 0;\n\n");

        emit(e, "void *__wrap_malloc(size_t size) {\n");
        emit(e, "    void *raw = __real_malloc(size + 16);\n");
        emit(e, "    if (!raw) return (void*)0;\n");
        emit(e, "    uint32_t *hdr = (uint32_t *)raw;\n");
        emit(e, "    hdr[0] = ++_zer_alloc_gen;\n");
        emit(e, "    hdr[1] = (uint32_t)size;\n");
        emit(e, "    hdr[2] = 0x5A455243u; /* magic ZERC */\n");
        emit(e, "    hdr[3] = 1; /* alive */\n");
        emit(e, "    return (char*)raw + 16;\n");
        emit(e, "}\n\n");

        emit(e, "void __wrap_free(void *ptr) {\n");
        emit(e, "    if (!ptr) return;\n");
        emit(e, "    uint32_t *hdr = (uint32_t *)((char*)ptr - 16);\n");
        emit(e, "    if (hdr[2] != 0x5A455243u) { __real_free(ptr); return; }\n");
        emit(e, "    if (!hdr[3]) _zer_trap(\"double free: tracked pointer\", __FILE__, __LINE__);\n");
        emit(e, "    hdr[3] = 0;\n");
        emit(e, "    __real_free(hdr);\n");
        emit(e, "}\n\n");

        emit(e, "void *__wrap_calloc(size_t n, size_t size) {\n");
        emit(e, "    size_t total = n * size;\n");
        emit(e, "    void *p = __wrap_malloc(total);\n");
        emit(e, "    if (p) memset(p, 0, total);\n");
        emit(e, "    return p;\n");
        emit(e, "}\n\n");

        emit(e, "void *__wrap_realloc(void *ptr, size_t new_size) {\n");
        emit(e, "    if (!ptr) return __wrap_malloc(new_size);\n");
        emit(e, "    if (new_size == 0) { __wrap_free(ptr); return (void*)0; }\n");
        emit(e, "    uint32_t *hdr = (uint32_t *)((char*)ptr - 16);\n");
        emit(e, "    if (hdr[2] != 0x5A455243u) return __real_realloc(ptr, new_size);\n");
        emit(e, "    void *np = __wrap_malloc(new_size);\n");
        emit(e, "    if (!np) return (void*)0;\n");
        emit(e, "    uint32_t old_size = hdr[1];\n");
        emit(e, "    memcpy(np, ptr, old_size < new_size ? old_size : new_size);\n");
        emit(e, "    __wrap_free(ptr);\n");
        emit(e, "    return np;\n");
        emit(e, "}\n\n");

        emit(e, "char *__wrap_strdup(const char *s) {\n");
        emit(e, "    if (!s) return (void*)0;\n");
        emit(e, "    size_t len = strlen(s) + 1;\n");
        emit(e, "    char *p = (char*)__wrap_malloc(len);\n");
        emit(e, "    if (p) memcpy(p, s, len);\n");
        emit(e, "    return p;\n");
        emit(e, "}\n\n");

        emit(e, "char *__wrap_strndup(const char *s, size_t n) {\n");
        emit(e, "    if (!s) return (void*)0;\n");
        emit(e, "    size_t len = strlen(s);\n");
        emit(e, "    if (len > n) len = n;\n");
        emit(e, "    char *p = (char*)__wrap_malloc(len + 1);\n");
        emit(e, "    if (p) { memcpy(p, s, len); p[len] = 0; }\n");
        emit(e, "    return p;\n");
        emit(e, "}\n\n");

        emit(e, "static inline void _zer_check_alive(void *ptr, const char *file, int line) {\n");
        emit(e, "    if (!ptr) return;\n");
        emit(e, "    uint32_t *hdr = (uint32_t *)((char*)ptr - 16);\n");
        emit(e, "    if (hdr[2] != 0x5A455243u) return; /* not tracked */\n");
        emit(e, "    if (!hdr[3]) _zer_trap(\"use-after-free: tracked pointer freed\", file, line);\n");
        emit(e, "}\n\n");

    }

    emit(e, "\n");

    /* MMIO declaration startup validation: @probe each declared mmio range
     * at boot to verify hardware is actually present. Catches wrong datasheet
     * addresses at first power-on instead of hours later in untested code paths.
     * Auto-discovery removed (2026-04-01 decision) — see safety-roadmap.md. */
    /* Emit mmio startup validation only for real hardware ranges.
     * Skip when range covers entire address space (test/development wildcard)
     * or when running on hosted user-space (can't probe physical MMIO addresses).
     * Bare-metal x86 (no __linux__/__APPLE__/_WIN32) gets validation too. */
    {
        int real_ranges = 0;
        for (int i = 0; i < e->checker->mmio_range_count; i++) {
            /* skip wildcard "allow all" range (0x0..0xFFFFFFFFFFFFFFFF) */
            if (e->checker->mmio_ranges[i][0] == 0 &&
                e->checker->mmio_ranges[i][1] >= 0xFFFFFFFF) continue;
            real_ranges++;
        }
        if (real_ranges > 0) {
            emit(e, "/* MMIO startup validation — verify declared ranges have real hardware */\n");
            emit(e, "#if !defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)\n");
            emit(e, "__attribute__((constructor))\n");
            emit(e, "static void _zer_mmio_validate(void) {\n");
            for (int i = 0; i < e->checker->mmio_range_count; i++) {
                if (e->checker->mmio_ranges[i][0] == 0 &&
                    e->checker->mmio_ranges[i][1] >= 0xFFFFFFFF) continue;
                emit(e, "    if (!_zer_probe((uintptr_t)0x%llxu).has_value)\n",
                     (unsigned long long)e->checker->mmio_ranges[i][0]);
                emit(e, "        _zer_trap(\"mmio 0x%llx..0x%llx: no hardware detected\", __FILE__, __LINE__);\n",
                     (unsigned long long)e->checker->mmio_ranges[i][0],
                     (unsigned long long)e->checker->mmio_ranges[i][1]);
            }
            emit(e, "}\n");
            emit(e, "#endif\n\n");
        }
    }

    /* Pre-scan: find all spawn statements and assign IDs for wrapper emission */
    for (int i = 0; i < file_node->file.decl_count; i++)
        prescan_spawn_in_node(e, file_node->file.decls[i]);

    /* Pass 1: emit struct/enum/union/typedef declarations first */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *d = file_node->file.decls[i];
        if (d->kind == NODE_STRUCT_DECL || d->kind == NODE_ENUM_DECL ||
            d->kind == NODE_UNION_DECL || d->kind == NODE_TYPEDEF)
            emit_top_level_decl(e, d, file_node, i);
    }

    /* Emit stamped container struct declarations — after regular structs */
    emit_container_structs(e);

    /* emit auto-Slab globals for Task.new() / Task.delete() — after structs, before functions */
    if (e->checker->auto_slab_count > 0) {
        emit(e, "\n/* ZER auto-Slab globals (Task.new/delete) */\n");
        for (int i = 0; i < e->checker->auto_slab_count; i++) {
            Type *elem = e->checker->auto_slabs[i].elem_type;
            Symbol *sym = e->checker->auto_slabs[i].slab_sym;
            emit(e, "static _zer_slab %.*s = { .slot_size = sizeof(", (int)sym->name_len, sym->name);
            emit_type(e, elem);
            emit(e, ") };\n");
        }
        emit(e, "\n");
    }

    /* Emit spawn wrapper functions — after structs/slabs, before user functions */
    emit_spawn_wrappers(e);

    /* Pass 2: emit everything else (functions, globals, etc.) */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *d = file_node->file.decls[i];
        if (d->kind != NODE_STRUCT_DECL && d->kind != NODE_ENUM_DECL &&
            d->kind != NODE_UNION_DECL && d->kind != NODE_TYPEDEF &&
            d->kind != NODE_CONTAINER_DECL)
            emit_top_level_decl(e, d, file_node, i);
    }
}

/* Backward-compat wrappers — all callers use the unified emit_file_module */
void emit_file(Emitter *e, Node *file_node) {
    emit_file_module(e, file_node, !e->lib_mode);
}

void emit_file_no_preamble(Emitter *e, Node *file_node) {
    emit_file_module(e, file_node, false);
}

/* ================================================================
 * IR-BASED C EMISSION (Phase 5)
 *
 * Emits C code from IRFunc instead of AST. Reuses existing helpers
 * (emit_type, emit_expr, emit_type_and_name) for expressions.
 * Only statement/control-flow emission reads from IR.
 *
 * This is the incremental replacement for emit_stmt/emit_async_func.
 * During migration, both paths coexist. When IR emission is complete,
 * the AST-based emit_stmt path is deleted.
 * ================================================================ */

/* ================================================================
 * IR Local Name Emitter — helper for emitting local variable names
 *
 * All IR instruction emission uses local IDs. This helper emits
 * the C name for a local, with async self-> prefix when needed.
 * ================================================================ */
static void emit_local_name(Emitter *e, IRFunc *func, int local_id) {
    if (local_id < 0 || local_id >= func->local_count) return;
    IRLocal *l = &func->locals[local_id];
    if (func->is_async)
        emit(e, "self->%.*s", (int)l->name_len, l->name);
    else
        emit(e, "%.*s", (int)l->name_len, l->name);
}

/* ================================================================
 * Rewritten AST Node Emitter — emits C from rewritten AST nodes
 *
 * This function handles expressions that lower_expr sends through
 * the passthrough path (IR_ASSIGN{dest, expr}). NODE_IDENTs in the
 * AST have been rewritten to IR local C names by rewrite_idents().
 *
 * DOES NOT CALL emit_expr. Each node type emitted directly.
 * For sub-expressions, calls itself recursively.
 * ================================================================ */
/* Forward declaration for recursive calls */
static void emit_rewritten_node(Emitter *e, Node *node, IRFunc *func);

static void emit_rewritten_node(Emitter *e, Node *node, IRFunc *func) {
    if (!node) return;

    switch (node->kind) {
    case NODE_IDENT:
        /* Rewritten ident — just emit the name (may be IR local or global) */
        if (e->in_async) {
            /* Check if this is an async local */
            for (int i = 0; i < e->async_local_count; i++) {
                if (e->async_local_lens[i] == (size_t)node->ident.name_len &&
                    memcmp(e->async_locals[i], node->ident.name, node->ident.name_len) == 0) {
                    emit(e, "self->%.*s", (int)node->ident.name_len, node->ident.name);
                    return;
                }
            }
        }
        emit(e, "%.*s", (int)node->ident.name_len, node->ident.name);
        return;

    case NODE_INT_LIT:
        if (node->int_lit.value > 0xFFFFFFFF)
            emit(e, "%lluULL", (unsigned long long)node->int_lit.value);
        else
            emit(e, "%llu", (unsigned long long)node->int_lit.value);
        return;
    case NODE_FLOAT_LIT:
        emit(e, "%.17g", node->float_lit.value);
        return;
    case NODE_BOOL_LIT:
        emit(e, "%d", node->bool_lit.value ? 1 : 0);
        return;
    case NODE_CHAR_LIT:
        if (node->char_lit.value >= 32 && node->char_lit.value < 127 &&
            node->char_lit.value != '\'' && node->char_lit.value != '\\')
            emit(e, "'%c'", (char)node->char_lit.value);
        else
            emit(e, "%u", (unsigned)node->char_lit.value);
        return;
    case NODE_NULL_LIT:
        emit(e, "0");
        return;
    case NODE_STRING_LIT:
        emit(e, "(_zer_slice_u8){ (uint8_t*)\"%.*s\", %u }",
             (int)node->string_lit.length, node->string_lit.value,
             (unsigned)node->string_lit.length);
        return;

    case NODE_BINARY: {
        const char *op = "?";
        switch (node->binary.op) {
        case TOK_PLUS: op = "+"; break; case TOK_MINUS: op = "-"; break;
        case TOK_STAR: op = "*"; break; case TOK_SLASH: op = "/"; break;
        case TOK_PERCENT: op = "%"; break;
        case TOK_AMP: op = "&"; break; case TOK_PIPE: op = "|"; break;
        case TOK_CARET: op = "^"; break;
        case TOK_LSHIFT: op = "<<"; break; case TOK_RSHIFT: op = ">>"; break;
        case TOK_EQEQ: op = "=="; break; case TOK_BANGEQ: op = "!="; break;
        case TOK_LT: op = "<"; break; case TOK_GT: op = ">"; break;
        case TOK_LTEQ: op = "<="; break; case TOK_GTEQ: op = ">="; break;
        case TOK_AMPAMP: op = "&&"; break; case TOK_PIPEPIPE: op = "||"; break;
        default: break;
        }
        /* Check for complex operand types → delegate to emit_expr */
        Type *lt = checker_get_type(e->checker, node->binary.left);
        Type *rt = checker_get_type(e->checker, node->binary.right);
        if (lt) {
            Type *le = type_unwrap_distinct(lt);
            /* Optional: compare against null → has_value check */
            if (le->kind == TYPE_OPTIONAL && !is_null_sentinel(le->optional.inner)) {
                if (node->binary.right->kind == NODE_NULL_LIT) {
                    /* opt == null → (!opt.has_value), opt != null → (opt.has_value) */
                    if (node->binary.op == TOK_EQEQ) emit(e, "(!");
                    else emit(e, "(");
                    emit_rewritten_node(e, node->binary.left, func);
                    emit(e, ".has_value)");
                    return;
                }
                /* opt == value — compare .value */
                emit(e, "(");
                emit_rewritten_node(e, node->binary.left, func);
                emit(e, ".value %s ", op);
                emit_rewritten_node(e, node->binary.right, func);
                emit(e, ")");
                return;
            }
            /* Right side optional */
            if (rt) {
                Type *re = type_unwrap_distinct(rt);
                if (re->kind == TYPE_OPTIONAL && !is_null_sentinel(re->optional.inner)) {
                    if (node->binary.left->kind == NODE_NULL_LIT) {
                        if (node->binary.op == TOK_EQEQ) emit(e, "(!");
                        else emit(e, "(");
                        emit_rewritten_node(e, node->binary.right, func);
                        emit(e, ".has_value)");
                        return;
                    }
                    emit(e, "(");
                    emit_rewritten_node(e, node->binary.left, func);
                    emit(e, " %s ", op);
                    emit_rewritten_node(e, node->binary.right, func);
                    emit(e, ".value)");
                    return;
                }
            }
            /* Struct, union can't use == in C — not supported in IR path */
            if (le->kind == TYPE_STRUCT || le->kind == TYPE_UNION) {
                emit(e, "/* struct/union compare unsupported */ 0");
                return;
            }
            if (le->kind == TYPE_POINTER && le->pointer.inner &&
                type_unwrap_distinct(le->pointer.inner)->kind == TYPE_OPAQUE) {
                emit(e, "(");
                emit_rewritten_node(e, node->binary.left, func);
                emit(e, ".ptr %s ", op);
                emit_rewritten_node(e, node->binary.right, func);
                emit(e, ".ptr)");
                return;
            }
        }
        emit(e, "(");
        emit_rewritten_node(e, node->binary.left, func);
        emit(e, " %s ", op);
        emit_rewritten_node(e, node->binary.right, func);
        emit(e, ")");
        return;
    }

    case NODE_UNARY:
        switch (node->unary.op) {
        case TOK_MINUS: emit(e, "-"); break;
        case TOK_BANG: emit(e, "!"); break;
        case TOK_TILDE: emit(e, "~"); break;
        case TOK_STAR: emit(e, "*"); break;
        case TOK_AMP: emit(e, "&"); break;
        default: break;
        }
        emit_rewritten_node(e, node->unary.operand, func);
        return;

    case NODE_FIELD: {
        /* Check object type for accessor: struct uses '.', pointer uses '->' */
        if (node->field.object && node->field.object->kind == NODE_IDENT) {
            Type *ot = checker_get_type(e->checker, node->field.object);
            if (!ot) {
                Symbol *sym = scope_lookup(e->checker->global_scope,
                    node->field.object->ident.name,
                    (uint32_t)node->field.object->ident.name_len);
                if (sym) ot = sym->type;
            }
            /* Fallback: look up in IR locals (rewritten idents use IR local C names) */
            if (!ot && func) {
                for (int li = 0; li < func->local_count; li++) {
                    if (func->locals[li].name_len == (uint32_t)node->field.object->ident.name_len &&
                        memcmp(func->locals[li].name, node->field.object->ident.name,
                               func->locals[li].name_len) == 0) {
                        ot = func->locals[li].type;
                        break;
                    }
                }
            }
            if (ot) {
                Type *ot_eff = type_unwrap_distinct(ot);
                /* Handle auto-deref: h.field → ((T*)_zer_*_get(&slab, h))->field */
                if (ot_eff->kind == TYPE_HANDLE) {
                    Type *elem = ot_eff->handle.elem;
                    /* Find allocator for this handle */
                    Symbol *alloc_sym = find_unique_allocator(e->checker->global_scope, elem);
                    if (alloc_sym) {
                        Type *alloc_type = type_unwrap_distinct(alloc_sym->type);
                        if (alloc_type->kind == TYPE_SLAB) {
                            emit(e, "((");
                            emit_type(e, type_pointer(e->arena, elem));
                            emit(e, ")_zer_slab_get(&%.*s, ",
                                 (int)alloc_sym->name_len, alloc_sym->name);
                            emit_rewritten_node(e, node->field.object, func);
                            emit(e, "))->%.*s",
                                 (int)node->field.field_name_len, node->field.field_name);
                        } else if (alloc_type->kind == TYPE_POOL) {
                            emit(e, "((");
                            emit_type(e, type_pointer(e->arena, elem));
                            emit(e, ")_zer_pool_get(%.*s.slots, %.*s.gen, %.*s.used, "
                                 "sizeof(%.*s.slots[0]), ",
                                 (int)alloc_sym->name_len, alloc_sym->name,
                                 (int)alloc_sym->name_len, alloc_sym->name,
                                 (int)alloc_sym->name_len, alloc_sym->name,
                                 (int)alloc_sym->name_len, alloc_sym->name);
                            emit_rewritten_node(e, node->field.object, func);
                            emit(e, ", %llu))->%.*s",
                                 (unsigned long long)alloc_type->pool.count,
                                 (int)node->field.field_name_len, node->field.field_name);
                        }
                    } else {
                        /* No allocator found — fallback */
                        emit(e, "/* handle auto-deref no alloc */ 0");
                    }
                    return;
                }
                /* Opaque, builtins — simple . field access */
                if (ot_eff->kind == TYPE_OPAQUE ||
                    ot_eff->kind == TYPE_POOL || ot_eff->kind == TYPE_SLAB ||
                    ot_eff->kind == TYPE_RING || ot_eff->kind == TYPE_ARENA) {
                    emit_rewritten_node(e, node->field.object, func);
                    emit(e, ".%.*s", (int)node->field.field_name_len,
                         node->field.field_name);
                    return;
                }
                if (ot_eff->kind == TYPE_ENUM) {
                    /* Emit enum value: _ZER_EnumName_variant */
                    const char *ename = ot_eff->enum_type.name;
                    uint32_t elen = ot_eff->enum_type.name_len;
                    if (ot_eff->enum_type.module_prefix) {
                        emit(e, "_ZER_%.*s__%.*s_%.*s",
                             (int)ot_eff->enum_type.module_prefix_len,
                             ot_eff->enum_type.module_prefix,
                             (int)elen, ename,
                             (int)node->field.field_name_len, node->field.field_name);
                    } else {
                        emit(e, "_ZER_%.*s_%.*s",
                             (int)elen, ename,
                             (int)node->field.field_name_len, node->field.field_name);
                    }
                    return;
                }
                /* Slice .ptr/.len */
                if (ot_eff->kind == TYPE_SLICE) {
                    emit_rewritten_node(e, node->field.object, func);
                    emit(e, ".%.*s", (int)node->field.field_name_len, node->field.field_name);
                    return;
                }
                /* Array .len → emit compile-time size */
                if (ot_eff->kind == TYPE_ARRAY) {
                    if (node->field.field_name_len == 3 &&
                        memcmp(node->field.field_name, "len", 3) == 0) {
                        emit(e, "%uU", (unsigned)ot_eff->array.size);
                        return;
                    }
                }
                /* Pointer → use -> */
                if (ot_eff->kind == TYPE_POINTER) {
                    emit_rewritten_node(e, node->field.object, func);
                    emit(e, "->%.*s", (int)node->field.field_name_len, node->field.field_name);
                    return;
                }
            }
        }
        /* Default: determine accessor from object type (. or ->) */
        {
            Type *obj_type = checker_get_type(e->checker, node->field.object);
            /* NODE_CALL results (e.g. Handle auto-deref get()) are typically pointers */
            if (!obj_type && node->field.object &&
                node->field.object->kind == NODE_CALL) {
                /* Auto-deref get() returns pointer — use -> */
                emit_rewritten_node(e, node->field.object, func);
                emit(e, "->%.*s", (int)node->field.field_name_len, node->field.field_name);
                return;
            }
            /* IR local fallback for object type */
            if (!obj_type && node->field.object && node->field.object->kind == NODE_IDENT && func) {
                for (int li = 0; li < func->local_count; li++) {
                    if (func->locals[li].name_len == (uint32_t)node->field.object->ident.name_len &&
                        memcmp(func->locals[li].name, node->field.object->ident.name,
                               func->locals[li].name_len) == 0) {
                        obj_type = func->locals[li].type;
                        break;
                    }
                }
            }
            const char *acc = ".";
            if (obj_type) {
                Type *oe = type_unwrap_distinct(obj_type);
                if (oe->kind == TYPE_POINTER) acc = "->";
                /* Handle auto-deref: emit get() → -> */
                if (oe->kind == TYPE_HANDLE) {
                    Type *elem = oe->handle.elem;
                    Symbol *alloc_sym = find_unique_allocator(e->checker->global_scope, elem);
                    if (alloc_sym) {
                        Type *alloc_type = type_unwrap_distinct(alloc_sym->type);
                        emit(e, "((");
                        emit_type(e, type_pointer(e->arena, elem));
                        if (alloc_type->kind == TYPE_SLAB) {
                            emit(e, ")_zer_slab_get(&%.*s, ",
                                 (int)alloc_sym->name_len, alloc_sym->name);
                        } else if (alloc_type->kind == TYPE_POOL) {
                            emit(e, ")_zer_pool_get(%.*s.slots, %.*s.gen, %.*s.used, "
                                 "sizeof(%.*s.slots[0]), ",
                                 (int)alloc_sym->name_len, alloc_sym->name,
                                 (int)alloc_sym->name_len, alloc_sym->name,
                                 (int)alloc_sym->name_len, alloc_sym->name,
                                 (int)alloc_sym->name_len, alloc_sym->name);
                        }
                        emit_rewritten_node(e, node->field.object, func);
                        if (alloc_type->kind == TYPE_POOL)
                            emit(e, ", %llu", (unsigned long long)alloc_type->pool.count);
                        emit(e, "))->%.*s",
                             (int)node->field.field_name_len, node->field.field_name);
                        return;
                    }
                }
            }
            emit_rewritten_node(e, node->field.object, func);
            emit(e, "%s%.*s", acc, (int)node->field.field_name_len, node->field.field_name);
        }
        return;
    }

    case NODE_INDEX: {
        /* Index: emit obj[idx] with .ptr for slices.
         * Bounds checks were already inserted by the checker as auto-guard
         * instructions — emitted before this expression via emit_auto_guards. */
        Type *idx_obj_type = checker_get_type(e->checker, node->index_expr.object);
        bool idx_slice = idx_obj_type && type_unwrap_distinct(idx_obj_type)->kind == TYPE_SLICE;
        emit_rewritten_node(e, node->index_expr.object, func);
        if (idx_slice) emit(e, ".ptr");
        emit(e, "[");
        emit_rewritten_node(e, node->index_expr.index, func);
        emit(e, "]");
        return;
    }

    case NODE_ASSIGN: {
        /* Assignments: emit target op= value from rewritten AST.
         * Complex patterns (bit extract, volatile array, shared lock) need emit_expr. */
        /* Check for complex patterns that need emit_expr */
        Type *tgt_type = checker_get_type(e->checker, node->assign.target);
        Type *tgt_eff = tgt_type ? type_unwrap_distinct(tgt_type) : NULL;
        /* Bit extract SET: reg[hi..lo] = val → shift/mask.
         * Replicates emit_expr's BUG-210/216 handler using emit_rewritten_node. */
        if (node->assign.target && node->assign.target->kind == NODE_SLICE) {
            Node *obj = node->assign.target->slice.object;
            Node *hi_node = node->assign.target->slice.start;
            Node *lo_node = node->assign.target->slice.end;
            int btmp = e->temp_count++;
            emit(e, "({ __typeof__(");
            emit_rewritten_node(e, obj, func);
            emit(e, ") *_zer_bp%d = &(", btmp);
            emit_rewritten_node(e, obj, func);
            emit(e, "); ");
            int64_t const_hi = hi_node ? eval_const_expr(hi_node) : CONST_EVAL_FAIL;
            int64_t const_lo = lo_node ? eval_const_expr(lo_node) : CONST_EVAL_FAIL;
            bool bits_const = (const_hi != CONST_EVAL_FAIL && const_lo != CONST_EVAL_FAIL &&
                               const_hi >= 0 && const_lo >= 0);
            if (!bits_const && hi_node && lo_node) {
                emit(e, "uint64_t _zer_bh%d = (uint64_t)(", btmp);
                emit_rewritten_node(e, hi_node, func);
                emit(e, "); uint64_t _zer_bl%d = (uint64_t)(", btmp);
                emit_rewritten_node(e, lo_node, func);
                emit(e, "); ");
            }
            emit(e, "*_zer_bp%d = (*_zer_bp%d & ~(", btmp, btmp);
            if (hi_node && lo_node) {
                if (bits_const) {
                    int64_t width = const_hi - const_lo + 1;
                    if (width >= 64) emit(e, "~(uint64_t)0");
                    else emit(e, "((1ull << %lld) - 1)", (long long)width);
                    emit(e, " << %lld", (long long)const_lo);
                } else {
                    emit(e, "(((_zer_bh%d - _zer_bl%d + 1) >= 64 ? ~(uint64_t)0 : "
                         "((1ull << (_zer_bh%d - _zer_bl%d + 1)) - 1)) << _zer_bl%d)",
                         btmp, btmp, btmp, btmp, btmp);
                }
            }
            emit(e, ")) | (((uint64_t)(");
            emit_rewritten_node(e, node->assign.value, func);
            emit(e, ") << ");
            if (bits_const) emit(e, "%lld", (long long)const_lo);
            else if (lo_node) emit(e, "_zer_bl%d", btmp);
            else emit(e, "0");
            emit(e, ") & (");
            if (hi_node && lo_node) {
                if (bits_const) {
                    int64_t width = const_hi - const_lo + 1;
                    if (width >= 64) emit(e, "~(uint64_t)0");
                    else emit(e, "((1ull << %lld) - 1)", (long long)width);
                    emit(e, " << %lld", (long long)const_lo);
                } else {
                    emit(e, "(((_zer_bh%d - _zer_bl%d + 1) >= 64 ? ~(uint64_t)0 : "
                         "((1ull << (_zer_bh%d - _zer_bl%d + 1)) - 1)) << _zer_bl%d)",
                         btmp, btmp, btmp, btmp, btmp);
                }
            }
            emit(e, ")); })");
            return;
        }
        /* Array assignment — memcpy/byte-loop */
        if (tgt_eff && tgt_eff->kind == TYPE_ARRAY) {
            int tmp = e->temp_count++;
            emit(e, "({ __typeof__(");
            emit_rewritten_node(e, node->assign.target, func);
            emit(e, ") *_zer_ma%d = &(", tmp);
            emit_rewritten_node(e, node->assign.target, func);
            emit(e, "); memmove(_zer_ma%d, ", tmp);
            emit_rewritten_node(e, node->assign.value, func);
            emit(e, ", sizeof(*_zer_ma%d)); })", tmp);
            return;
        }
        /* Shared struct locking — emit lock + assign + unlock */
        if (tgt_eff && tgt_eff->kind == TYPE_STRUCT && tgt_eff->struct_type.is_shared) {
            /* Extract root shared struct for locking.
             * Target is like `shared_var.field` — root is `shared_var`. */
            Node *root = node->assign.target;
            while (root && root->kind == NODE_FIELD) root = root->field.object;
            if (root) {
                emit(e, "({ ");
                emit(e, "_zer_mtx_ensure_init(&(");
                emit_rewritten_node(e, root, func);
                emit(e, "._zer_mtx), &(");
                emit_rewritten_node(e, root, func);
                emit(e, "._zer_mtx_inited)); ");
                emit(e, "pthread_mutex_lock(&(");
                emit_rewritten_node(e, root, func);
                emit(e, "._zer_mtx)); ");
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, " = ");
                emit_rewritten_node(e, node->assign.value, func);
                emit(e, "; ");
                emit(e, "pthread_mutex_unlock(&(");
                emit_rewritten_node(e, root, func);
                emit(e, "._zer_mtx)); })");
            } else {
                /* Fallback for complex targets */
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, " = ");
                emit_rewritten_node(e, node->assign.value, func);
            }
            return;
        }
        /* Simple assignment: target op= value */
        const char *aop = "=";
        switch (node->assign.op) {
        case TOK_EQ: aop = "="; break;
        case TOK_PLUSEQ: aop = "+="; break; case TOK_MINUSEQ: aop = "-="; break;
        case TOK_STAREQ: aop = "*="; break; case TOK_SLASHEQ: aop = "/="; break;
        case TOK_PERCENTEQ: aop = "%="; break;
        case TOK_AMPEQ: aop = "&="; break; case TOK_PIPEEQ: aop = "|="; break;
        case TOK_CARETEQ: aop = "^="; break;
        case TOK_LSHIFTEQ: aop = "<<="; break; case TOK_RSHIFTEQ: aop = ">>="; break;
        default: break;
        }
        /* Optional wrapping on assignment: target = (OptType){ value, 1 } */
        if (node->assign.op == TOK_EQ && tgt_eff &&
            tgt_eff->kind == TYPE_OPTIONAL && !is_null_sentinel(tgt_eff->optional.inner)) {
            Type *val_type = checker_get_type(e->checker, node->assign.value);
            Type *val_eff = val_type ? type_unwrap_distinct(val_type) : NULL;
            if (node->assign.value->kind == NODE_NULL_LIT) {
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, " = ");
                emit_opt_null_literal(e, tgt_eff);
                return;
            }
            if (val_eff && val_eff->kind != TYPE_OPTIONAL) {
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, " = (");
                emit_type(e, tgt_eff);
                emit(e, "){ ");
                emit_rewritten_node(e, node->assign.value, func);
                emit(e, ", 1 }");
                return;
            }
        }
        /* Array → slice coercion on assignment */
        if (node->assign.op == TOK_EQ && tgt_eff && tgt_eff->kind == TYPE_SLICE) {
            Type *val_type = checker_get_type(e->checker, node->assign.value);
            Type *val_eff = val_type ? type_unwrap_distinct(val_type) : NULL;
            if (val_eff && val_eff->kind == TYPE_ARRAY) {
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, " = (");
                emit_type(e, tgt_eff);
                emit(e, "){ ");
                emit_rewritten_node(e, node->assign.value, func);
                emit(e, ", %u }", (unsigned)val_eff->array.size);
                return;
            }
        }
        emit_rewritten_node(e, node->assign.target, func);
        emit(e, " %s ", aop);
        emit_rewritten_node(e, node->assign.value, func);
        return;
    }

    case NODE_CALL: {
        /* Call: callee(args) — rewritten idents, emit directly.
         * Builtins (pool/slab/ring/arena) MUST go through emit_expr
         * for inline C generation. Detect and delegate. */
        if (node->call.is_comptime_resolved) {
            if (node->call.comptime_struct_init) {
                /* Comptime struct return — emit the struct init */
                emit_rewritten_node(e, node->call.comptime_struct_init, func);
                return;
            }
            if (node->call.is_comptime_float)
                emit(e, "%.17g", node->call.comptime_float_value);
            else
                emit(e, "%lld", (long long)node->call.comptime_value);
            return;
        }
        /* Detect builtins + ThreadHandle.join: callee NODE_FIELD */
        if (node->call.callee && node->call.callee->kind == NODE_FIELD &&
            node->call.callee->field.object &&
            node->call.callee->field.object->kind == NODE_IDENT) {
            /* ThreadHandle.join() → pthread_join(th, NULL).
             * Detect: field name "join" + object type is thread handle.
             * Thread handles are emitted as pthread_t — check checker_get_type. */
            if (node->call.callee->field.field_name_len == 4 &&
                memcmp(node->call.callee->field.field_name, "join", 4) == 0) {
                /* ThreadHandle — emit pthread_join directly */
                emit(e, "pthread_join(");
                emit_rewritten_node(e, node->call.callee->field.object, func);
                emit(e, ", NULL)");
                return;
            }
            Type *ot = checker_get_type(e->checker, node->call.callee->field.object);
            if (!ot) {
                Symbol *sym = scope_lookup(e->checker->global_scope,
                    node->call.callee->field.object->ident.name,
                    (uint32_t)node->call.callee->field.object->ident.name_len);
                if (sym) ot = sym->type;
            }
            /* IR locals fallback */
            if (!ot && func) {
                for (int li = 0; li < func->local_count; li++) {
                    if (func->locals[li].name_len == (uint32_t)node->call.callee->field.object->ident.name_len &&
                        memcmp(func->locals[li].name, node->call.callee->field.object->ident.name,
                               func->locals[li].name_len) == 0) {
                        ot = func->locals[li].type;
                        break;
                    }
                }
            }
            if (ot) {
                Type *ot_eff = type_unwrap_distinct(ot);
                if (ot_eff->kind == TYPE_POOL || ot_eff->kind == TYPE_SLAB ||
                    ot_eff->kind == TYPE_RING || ot_eff->kind == TYPE_ARENA ||
                    ot_eff->kind == TYPE_HANDLE || ot_eff->kind == TYPE_STRUCT) {
                    /* Builtin — delegate to emit_expr for inline C */
                    emit_expr(e, node);
                    return;
                }
            }
        }
        emit_rewritten_node(e, node->call.callee, func);
        emit(e, "(");
        for (int i = 0; i < node->call.arg_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_rewritten_node(e, node->call.args[i], func);
        }
        emit(e, ")");
        return;
    }

    case NODE_INTRINSIC: {
        /* Intrinsics — handle each type from rewritten AST */
        const char *name = node->intrinsic.name;
        uint32_t nlen = (uint32_t)node->intrinsic.name_len;
        if (nlen == 4 && memcmp(name, "size", 4) == 0) {
            /* @size(T) → sizeof(CType) — direct type name emission. */
            emit(e, "sizeof(");
            if (node->intrinsic.type_arg) {
                TypeNode *ta = node->intrinsic.type_arg;
                if (ta->kind == TYNODE_NAMED) {
                    /* Named type: look up in scope for C name */
                    Symbol *sym = scope_lookup(e->checker->global_scope,
                        ta->named.name, (uint32_t)ta->named.name_len);
                    fprintf(stderr, "  @size NAMED '%.*s' sym=%p type_kind=%d\n",
                            (int)ta->named.name_len, ta->named.name,
                            (void*)sym,
                            (sym && sym->type) ? type_unwrap_distinct(sym->type)->kind : -1);
                    if (sym && sym->type) {
                        Type *te = type_unwrap_distinct(sym->type);
                        if (te->kind == TYPE_STRUCT) {
                            if (te->struct_type.is_packed)
                                emit(e, "struct __attribute__((packed)) ");
                            else emit(e, "struct ");
                            if (te->struct_type.module_prefix) {
                                emit(e, "%.*s__%.*s",
                                     (int)te->struct_type.module_prefix_len, te->struct_type.module_prefix,
                                     (int)te->struct_type.name_len, te->struct_type.name);
                            } else {
                                emit(e, "%.*s", (int)te->struct_type.name_len, te->struct_type.name);
                            }
                        } else if (te->kind == TYPE_UNION) {
                            emit(e, "struct %.*s", (int)te->union_type.name_len, te->union_type.name);
                        } else {
                            emit_type(e, sym->type);
                        }
                    } else {
                        emit(e, "struct %.*s", (int)ta->named.name_len, ta->named.name);
                    }
                } else {
                    /* Keyword type (u32, i8, etc.) */
                    Type *t = resolve_type_for_emit(e, ta);
                    if (t) emit_type(e, t);
                }
            } else if (node->intrinsic.arg_count > 0 &&
                       node->intrinsic.args[0]->kind == NODE_IDENT) {
                /* @size(TypeName) — type name passed as ident arg */
                const char *tn = node->intrinsic.args[0]->ident.name;
                uint32_t tl = (uint32_t)node->intrinsic.args[0]->ident.name_len;
                Symbol *sym = scope_lookup(e->checker->global_scope, tn, tl);
                if (sym && sym->type) {
                    Type *te = type_unwrap_distinct(sym->type);
                    if (te->kind == TYPE_STRUCT) {
                        if (te->struct_type.is_packed)
                            emit(e, "struct __attribute__((packed)) ");
                        else emit(e, "struct ");
                        emit(e, "%.*s", (int)te->struct_type.name_len, te->struct_type.name);
                    } else if (te->kind == TYPE_UNION) {
                        emit(e, "struct %.*s", (int)te->union_type.name_len, te->union_type.name);
                    } else {
                        emit_type(e, sym->type);
                    }
                } else {
                    emit(e, "struct %.*s", (int)tl, tn);
                }
            } else if (node->intrinsic.arg_count > 0) {
                emit_rewritten_node(e, node->intrinsic.args[0], func);
            }
            emit(e, ")");
            return;
        } else if (0 && nlen == 4 && memcmp(name, "size_DISABLED", 4) == 0) {
            /* Disabled — kept for reference. Direct sizeof emission. */
            emit(e, "sizeof(");
            if (node->intrinsic.type_arg) {
                Type *t = NULL;
                if (node->intrinsic.type_arg->kind == TYNODE_NAMED) {
                    Symbol *sym = scope_lookup(e->checker->global_scope,
                        node->intrinsic.type_arg->named.name,
                        (uint32_t)node->intrinsic.type_arg->named.name_len);
                    if (sym) t = sym->type;
                }
                if (t) {
                    Type *te = type_unwrap_distinct(t);
                    if (te->kind == TYPE_STRUCT) {
                        if (te->struct_type.is_packed) emit(e, "struct __attribute__((packed)) ");
                        else emit(e, "struct ");
                        emit(e, "%.*s", (int)te->struct_type.name_len, te->struct_type.name);
                    } else if (te->kind == TYPE_ENUM) {
                        emit(e, "int32_t"); /* enums are int32_t */
                    } else if (te->kind == TYPE_UNION) {
                        emit(e, "struct "); /* unions emit as struct with _tag */
                        emit(e, "%.*s", (int)te->union_type.name_len, te->union_type.name);
                    } else {
                        emit_type(e, t); /* primitives, pointers, etc. */
                    }
                } else if (node->intrinsic.type_arg->kind == TYNODE_NAMED) {
                    /* Last resort: emit struct prefix + name */
                    emit(e, "struct %.*s",
                         (int)node->intrinsic.type_arg->named.name_len,
                         node->intrinsic.type_arg->named.name);
                } else {
                    /* Primitive type — resolve_tynode handles these */
                    t = resolve_tynode(e, node->intrinsic.type_arg);
                    if (t) emit_type(e, t);
                }
            } else if (node->intrinsic.arg_count > 0) {
                emit_rewritten_node(e, node->intrinsic.args[0], func);
            }
            emit(e, ")");
            return;
        } else if (nlen == 8 && memcmp(name, "truncate", 8) == 0) {
            /* @truncate(T, val) → (T)(val) */
            emit(e, "(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")(");
            if (node->intrinsic.arg_count > 0)
                emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")");
        } else if (nlen == 8 && memcmp(name, "saturate", 8) == 0) {
            /* @saturate(T, val) → clamp to T range */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                emit(e, "({__auto_type _zer_sat%d = ", tmp);
                if (node->intrinsic.arg_count > 0)
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, "; ");
                int w = type_width(t);
                bool is_signed = type_is_signed(t);
                if (is_signed) {
                    int64_t min_v = -(1LL << (w - 1));
                    int64_t max_v = (1LL << (w - 1)) - 1;
                    emit(e, "_zer_sat%d < %lldLL ? (%lldLL) : _zer_sat%d > %lldLL ? (%lldLL) : (",
                         tmp, (long long)min_v, (long long)min_v,
                         tmp, (long long)max_v, (long long)max_v);
                } else {
                    uint64_t max_v = w >= 64 ? UINT64_MAX : ((1ULL << w) - 1);
                    emit(e, "_zer_sat%d > %lluULL ? (%lluULL) : (",
                         tmp, (unsigned long long)max_v, (unsigned long long)max_v);
                }
                emit_type(e, t);
                emit(e, ")_zer_sat%d; })", tmp);
            }
        } else if (nlen == 7 && memcmp(name, "bitcast", 7) == 0) {
            /* @bitcast(T, val) → memcpy type punning */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                int tmp2 = e->temp_count++;
                emit(e, "({__auto_type _zer_bci%d = ", tmp2);
                if (node->intrinsic.arg_count > 0)
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, "; ");
                emit_type(e, t);
                emit(e, " _zer_bco%d; memcpy(&_zer_bco%d, &_zer_bci%d, sizeof(_zer_bco%d)); _zer_bco%d; })",
                     tmp, tmp, tmp2, tmp, tmp);
            }
        } else if (nlen == 4 && memcmp(name, "cast", 4) == 0) {
            /* @cast(T, val) → (T)(val) for distinct typedefs */
            emit(e, "(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")(");
            if (node->intrinsic.arg_count > 0)
                emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")");
        } else if (nlen == 6 && memcmp(name, "offset", 6) == 0) {
            /* @offset(T, field) → offsetof(T, field) */
            emit(e, "offsetof(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                emit_type(e, t);
                emit(e, ", ");
                if (node->intrinsic.arg_count > 0)
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
            } else if (node->intrinsic.arg_count >= 2) {
                /* Named type: args[0] = type name, args[1] = field name */
                emit(e, "struct ");
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ", ");
                emit_rewritten_node(e, node->intrinsic.args[1], func);
            }
            emit(e, ")");
        } else if (nlen == 8 && memcmp(name, "ptrtoint", 8) == 0) {
            /* @ptrtoint(ptr) → (uintptr_t)(ptr) */
            emit(e, "(uintptr_t)(");
            if (node->intrinsic.arg_count > 0)
                emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")");
        } else if (nlen == 4 && memcmp(name, "trap", 4) == 0) {
            emit(e, "_zer_trap(\"trap\", __FILE__, __LINE__)");
        } else if (nlen == 7 && memcmp(name, "ptrcast", 7) == 0) {
            /* @ptrcast(*T, expr) — cast with type_id check */
            Type *tgt_type = node->intrinsic.type_arg ?
                resolve_tynode(e, node->intrinsic.type_arg) : NULL;
            Type *src_type = (node->intrinsic.arg_count > 0) ?
                checker_get_type(e->checker, node->intrinsic.args[0]) : NULL;
            Type *tgt_eff = tgt_type ? type_unwrap_distinct(tgt_type) : NULL;
            Type *src_eff = src_type ? type_unwrap_distinct(src_type) : NULL;

            if (tgt_eff && tgt_eff->kind == TYPE_POINTER && tgt_eff->pointer.inner &&
                type_unwrap_distinct(tgt_eff->pointer.inner)->kind == TYPE_OPAQUE &&
                src_eff && src_eff->kind == TYPE_POINTER) {
                /* To *opaque — wrap with type_id */
                uint32_t tid = 0;
                if (src_eff->pointer.inner) {
                    Type *inner = type_unwrap_distinct(src_eff->pointer.inner);
                    if (inner->kind == TYPE_STRUCT) tid = inner->struct_type.type_id;
                    else if (inner->kind == TYPE_ENUM) tid = inner->enum_type.type_id;
                    else if (inner->kind == TYPE_UNION) tid = inner->union_type.type_id;
                }
                emit(e, "(_zer_opaque){(void*)(");
                if (node->intrinsic.arg_count > 0)
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, "), %u}", (unsigned)tid);
            } else if (tgt_eff && tgt_eff->kind == TYPE_POINTER &&
                       src_eff &&
                       ((src_eff->kind == TYPE_POINTER && src_eff->pointer.inner &&
                         type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_OPAQUE) ||
                        src_eff->kind == TYPE_OPAQUE)) {
                /* From *opaque — unwrap .ptr with type check */
                uint32_t expected_tid = 0;
                if (tgt_eff->pointer.inner) {
                    Type *inner = type_unwrap_distinct(tgt_eff->pointer.inner);
                    if (inner->kind == TYPE_STRUCT) expected_tid = inner->struct_type.type_id;
                    else if (inner->kind == TYPE_ENUM) expected_tid = inner->enum_type.type_id;
                    else if (inner->kind == TYPE_UNION) expected_tid = inner->union_type.type_id;
                }
                if (expected_tid > 0) {
                    int tmp = e->temp_count++;
                    emit(e, "({ _zer_opaque _zer_pc%d = ", tmp);
                    if (node->intrinsic.arg_count > 0)
                        emit_rewritten_node(e, node->intrinsic.args[0], func);
                    emit(e, "; if (_zer_pc%d.type_id != %u && _zer_pc%d.type_id != 0) "
                         "_zer_trap(\"@ptrcast type mismatch\", __FILE__, __LINE__); (",
                         tmp, (unsigned)expected_tid, tmp);
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")_zer_pc%d.ptr; })", tmp);
                } else {
                    emit(e, "((");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")(");
                    if (node->intrinsic.arg_count > 0)
                        emit_rewritten_node(e, node->intrinsic.args[0], func);
                    emit(e, ").ptr)");
                }
            } else {
                /* Neither side is *opaque — plain cast */
                emit(e, "(");
                if (tgt_type) emit_type(e, tgt_type);
                emit(e, ")(");
                if (node->intrinsic.arg_count > 0)
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ")");
            }
        } else if (nlen == 8 && memcmp(name, "inttoptr", 8) == 0) {
            /* @inttoptr(*T, addr) — integer to pointer */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                emit(e, "((");
                emit_type(e, t);
                emit(e, ")(uintptr_t)(");
                if (node->intrinsic.arg_count > 0)
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, "))");
            }
        } else if (nlen == 9 && memcmp(name, "container", 9) == 0) {
            /* @container(*T, ptr, field) → container_of */
            if (node->intrinsic.type_arg && node->intrinsic.arg_count >= 2) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                emit(e, "((");
                emit_type(e, t);
                emit(e, ")((char*)(");
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ") - offsetof(");
                /* Emit the struct type for offsetof */
                if (t) {
                    Type *inner = type_unwrap_distinct(t);
                    if (inner->kind == TYPE_POINTER) inner = type_unwrap_distinct(inner->pointer.inner);
                    emit_type(e, inner);
                }
                emit(e, ", ");
                emit_rewritten_node(e, node->intrinsic.args[1], func);
                emit(e, ")))");
            }
        } else {
            /* Remaining complex intrinsics: @cstr, @barrier*, @atomic_*, @cond_*,
             * @sem_*, @probe, @once — too complex for rewritten emission.
             * Emit as passthrough to emit_expr. */
            emit_expr(e, node);
        }
        return;
    }

    case NODE_SLICE: {
        /* Sub-slice: obj[start..end] → (SliceType){ &obj.ptr[start], end-start } */
        Type *obj_type = checker_get_type(e->checker, node->slice.object);
        bool obj_slice = obj_type && type_unwrap_distinct(obj_type)->kind == TYPE_SLICE;
        if (obj_slice) {
            Type *elem = type_unwrap_distinct(obj_type)->slice.inner;
            emit(e, "(");
            emit_type(e, obj_type);
            emit(e, "){ &(");
            emit_rewritten_node(e, node->slice.object, func);
            emit(e, ".ptr)[");
            if (node->slice.start) emit_rewritten_node(e, node->slice.start, func);
            else emit(e, "0");
            emit(e, "], ");
            if (node->slice.end && node->slice.start) {
                emit(e, "(");
                emit_rewritten_node(e, node->slice.end, func);
                emit(e, ") - (");
                emit_rewritten_node(e, node->slice.start, func);
                emit(e, ")");
            } else if (node->slice.end) {
                emit_rewritten_node(e, node->slice.end, func);
            } else {
                emit_rewritten_node(e, node->slice.object, func);
                emit(e, ".len");
                if (node->slice.start) {
                    emit(e, " - ");
                    emit_rewritten_node(e, node->slice.start, func);
                }
            }
            emit(e, " }");
        } else {
            /* Array sub-slice or complex — delegate to emit_expr */
            emit_expr(e, node);
        }
        return;
    }

    case NODE_TYPECAST: {
        /* Should be handled by IR_CAST, but in case it reaches here */
        emit(e, "((");
        if (node->typecast.target_type) {
            Type *t = resolve_tynode(e, node->typecast.target_type);
            emit_type(e, t);
        }
        emit(e, ")");
        emit_rewritten_node(e, node->typecast.expr, func);
        emit(e, ")");
        return;
    }

    case NODE_STRUCT_INIT: {
        /* Should be handled by IR_STRUCT_INIT_DECOMP, but fallback */
        Type *si_type = checker_get_type(e->checker, node);
        if (si_type) {
            emit(e, "(");
            emit_type(e, si_type);
            emit(e, ")");
        }
        emit(e, "{ ");
        for (int i = 0; i < node->struct_init.field_count; i++) {
            if (i > 0) emit(e, ", ");
            emit(e, ".%.*s = ", (int)node->struct_init.fields[i].name_len,
                 node->struct_init.fields[i].name);
            emit_rewritten_node(e, node->struct_init.fields[i].value, func);
        }
        emit(e, " }");
        return;
    }

    default:
        /* Fallback: emit as C identifier or literal */
        emit(e, "/* unhandled node %d */0", node->kind);
        return;
    }
}

/* Emit one IR instruction as C code */
static void emit_ir_inst(Emitter *e, IRInst *inst, IRFunc *func) {
    switch (inst->op) {

    case IR_ASSIGN: {
        if (inst->dest_local >= 0 && inst->expr) {
            IRLocal *dest = &func->locals[inst->dest_local];

            /* Captures now go through IR_COPY (lowered at if-unwrap site).
             * ?void captures skipped at lowering time. No emit_expr needed. */

            /* ?void from void call now handled at lowering time:
             * void call → IR_ASSIGN(void), then IR_LITERAL(kind=6) for {1}. */

            emit_indent(e);
            if (func->is_async) {
                emit(e, "self->%.*s = ", (int)dest->name_len, dest->name);
            } else {
                emit(e, "%.*s = ", (int)dest->name_len, dest->name);
            }
            /* Type adaptation between source and dest */
            Type *src_type = checker_get_type(e->checker, inst->expr);
            Type *src_eff = src_type ? type_unwrap_distinct(src_type) : NULL;
            Type *dst_type = dest->type;
            Type *dst_eff = dst_type ? type_unwrap_distinct(dst_type) : NULL;

            bool need_unwrap = (src_eff && src_eff->kind == TYPE_OPTIONAL &&
                               dst_eff && dst_eff->kind != TYPE_OPTIONAL &&
                               !is_null_sentinel(src_eff->optional.inner) &&
                               src_eff->optional.inner->kind != TYPE_VOID &&
                               type_equals(type_unwrap_distinct(src_eff->optional.inner), dst_eff));
            bool need_wrap = (dst_eff && dst_eff->kind == TYPE_OPTIONAL &&
                             !is_null_sentinel(dst_eff->optional.inner) &&
                             src_eff && src_eff->kind != TYPE_OPTIONAL);
            bool need_null = (dst_eff && dst_eff->kind == TYPE_OPTIONAL &&
                             !is_null_sentinel(dst_eff->optional.inner) &&
                             inst->expr->kind == NODE_NULL_LIT);

            /* Array→slice coercion */
            bool need_slice = (dst_eff && dst_eff->kind == TYPE_SLICE &&
                              src_eff && src_eff->kind == TYPE_ARRAY);

            /* ?void hoist moved BEFORE "dest = " prefix (above) */
            if (need_null) {
                emit_opt_null_literal(e, dst_eff);
            } else if (need_wrap) {
                /* Inline wrap: (OptType){ value, 1 } — uses emit_rewritten_node */
                emit(e, "(");
                emit_type(e, dst_eff);
                emit(e, "){ ");
                emit_rewritten_node(e, inst->expr, func);
                emit(e, ", 1 }");
            } else if (need_slice) {
                emit_array_as_slice(e, inst->expr, src_type, dst_type);
            } else {
                emit_rewritten_node(e, inst->expr, func);
                if (need_unwrap) emit(e, ".value");
            }
            emit(e, ";\n");
        } else if (inst->expr) {
            /* Assignment to non-local (field, index) or void expr */
            emit_indent(e);
            emit_rewritten_node(e, inst->expr, func);
            emit(e, ";\n");
        }
        break;
    }

    case IR_CALL: {
        /* Function call — emit from local IDs.
         * Simple calls: func_name(local1, local2, ...) from decomposed args.
         * Builtin/complex: delegated to emit_expr on rewritten AST. */
        bool is_builtin = false;
        bool is_comptime = false;

        /* Detect builtins: callee is NODE_FIELD on pool/slab/ring/arena/Task type */
        if (inst->expr && inst->expr->kind == NODE_CALL) {
            Node *callee = inst->expr->call.callee;
            if (callee && callee->kind == NODE_FIELD && callee->field.object &&
                callee->field.object->kind == NODE_IDENT) {
                Type *ot = checker_get_type(e->checker, callee->field.object);
                /* Fallback: look up in global scope (globals may not be in typemap) */
                if (!ot) {
                    Symbol *sym = scope_lookup(e->checker->global_scope,
                        callee->field.object->ident.name,
                        (uint32_t)callee->field.object->ident.name_len);
                    if (sym) ot = sym->type;
                }
                if (ot) {
                    Type *ot_eff = type_unwrap_distinct(ot);
                    if (ot_eff->kind == TYPE_POOL || ot_eff->kind == TYPE_SLAB ||
                        ot_eff->kind == TYPE_RING || ot_eff->kind == TYPE_ARENA ||
                        ot_eff->kind == TYPE_STRUCT /* Task.new */)
                        is_builtin = true;
                }
            }
            /* Detect comptime calls */
            if (inst->expr->call.is_comptime_resolved)
                is_comptime = true;
        }

        emit_indent(e);
        if (inst->dest_local >= 0) {
            emit_local_name(e, func, inst->dest_local);
            emit(e, " = ");
        }

        /* If no decomposed args, must be builtin/comptime (lowering skipped decomposition) */
        if (!inst->call_arg_locals) is_builtin = true;

        if (is_builtin || is_comptime) {
            /* Builtins/comptime — emit_rewritten_node(NODE_CALL) detects
             * builtins and delegates to emit_expr for inline C generation. */
            if (inst->expr) emit_rewritten_node(e, inst->expr, func);
        } else if (inst->call_arg_locals) {
            /* Decomposed call: emit callee(local1, local2, ...) from local IDs.
             * Handle array→slice coercion for args when param expects slice. */
            /* Look up callee's function type for param types */
            Type *callee_ft = NULL;
            if (inst->expr && inst->expr->kind == NODE_CALL && inst->expr->call.callee) {
                Type *ct = checker_get_type(e->checker, inst->expr->call.callee);
                if (ct) {
                    Type *ct_eff = type_unwrap_distinct(ct);
                    if (ct_eff->kind == TYPE_FUNC_PTR) callee_ft = ct_eff;
                }
            }
            /* Emit callee: simple ident or field access (funcptr through struct) */
            if (inst->func_name) {
                emit(e, "%.*s(", (int)inst->func_name_len, inst->func_name);
            } else if (inst->expr && inst->expr->kind == NODE_CALL &&
                       inst->expr->call.callee &&
                       inst->expr->call.callee->kind == NODE_FIELD) {
                /* Struct field callee: obj.method or obj->method */
                Node *callee = inst->expr->call.callee;
                /* Emit object name from local or rewritten ident */
                if (callee->field.object && callee->field.object->kind == NODE_IDENT) {
                    int obj_id = -1;
                    for (int li = 0; li < func->local_count; li++) {
                        if (func->locals[li].name_len == (uint32_t)callee->field.object->ident.name_len &&
                            memcmp(func->locals[li].name, callee->field.object->ident.name,
                                   func->locals[li].name_len) == 0) {
                            obj_id = li; break;
                        }
                    }
                    if (obj_id >= 0) {
                        Type *ot = func->locals[obj_id].type;
                        Type *ot_eff = ot ? type_unwrap_distinct(ot) : NULL;
                        emit_local_name(e, func, obj_id);
                        emit(e, "%s%.*s(",
                             (ot_eff && ot_eff->kind == TYPE_POINTER) ? "->" : ".",
                             (int)callee->field.field_name_len, callee->field.field_name);
                    } else {
                        /* Global/extern funcptr */
                        emit(e, "%.*s.%.*s(",
                             (int)callee->field.object->ident.name_len,
                             callee->field.object->ident.name,
                             (int)callee->field.field_name_len, callee->field.field_name);
                    }
                } else {
                    emit(e, "/* complex callee */(");
                }
            } else if (inst->expr && inst->expr->kind == NODE_CALL &&
                       inst->expr->call.callee &&
                       inst->expr->call.callee->kind == NODE_INDEX) {
                /* Array-indexed funcptr: arr[i](args) */
                Node *idx_callee = inst->expr->call.callee;
                if (idx_callee->index_expr.object->kind == NODE_IDENT) {
                    int arr_id = -1;
                    for (int li = 0; li < func->local_count; li++) {
                        if (func->locals[li].name_len == (uint32_t)idx_callee->index_expr.object->ident.name_len &&
                            memcmp(func->locals[li].name, idx_callee->index_expr.object->ident.name,
                                   func->locals[li].name_len) == 0) {
                            arr_id = li; break;
                        }
                    }
                    if (arr_id >= 0) {
                        emit_local_name(e, func, arr_id);
                    } else {
                        /* Global array */
                        emit(e, "%.*s", (int)idx_callee->index_expr.object->ident.name_len,
                             idx_callee->index_expr.object->ident.name);
                    }
                    emit(e, "[");
                    /* Index expression — find local if decomposed */
                    if (idx_callee->index_expr.index->kind == NODE_IDENT) {
                        int idx_id = -1;
                        for (int li = 0; li < func->local_count; li++) {
                            if (func->locals[li].name_len == (uint32_t)idx_callee->index_expr.index->ident.name_len &&
                                memcmp(func->locals[li].name, idx_callee->index_expr.index->ident.name,
                                       func->locals[li].name_len) == 0) {
                                idx_id = li; break;
                            }
                        }
                        if (idx_id >= 0)
                            emit_local_name(e, func, idx_id);
                        else
                            emit(e, "%.*s", (int)idx_callee->index_expr.index->ident.name_len,
                                 idx_callee->index_expr.index->ident.name);
                    } else {
                        emit(e, "0"); /* non-ident index fallback */
                    }
                    emit(e, "](");
                } else {
                    emit(e, "/* complex index callee */(");
                }
            } else {
                emit(e, "/* unknown callee */(");
            }
            for (int i = 0; i < inst->call_arg_local_count; i++) {
                if (i > 0) emit(e, ", ");
                if (inst->call_arg_locals[i] >= 0) {
                    IRLocal *al = &func->locals[inst->call_arg_locals[i]];
                    Type *at = al->type ? type_unwrap_distinct(al->type) : NULL;
                    /* Check if param expects slice but arg is array → coerce */
                    Type *pt = (callee_ft && (uint32_t)i < callee_ft->func_ptr.param_count) ?
                        type_unwrap_distinct(callee_ft->func_ptr.params[i]) : NULL;
                    if (at && pt && at->kind == TYPE_ARRAY && pt->kind == TYPE_SLICE) {
                        /* Array → slice coercion */
                        emit(e, "(");
                        emit_type(e, pt);
                        emit(e, "){ ");
                        emit_local_name(e, func, inst->call_arg_locals[i]);
                        emit(e, ", %u }", (unsigned)at->array.size);
                    } else if (at && pt && at->kind == TYPE_SLICE && pt->kind == TYPE_POINTER) {
                        /* Slice → pointer coercion (C interop: []T → *T via .ptr) */
                        emit_local_name(e, func, inst->call_arg_locals[i]);
                        emit(e, ".ptr");
                    } else {
                        emit_local_name(e, func, inst->call_arg_locals[i]);
                    }
                } else {
                    emit(e, "0"); /* void/undecomposable arg */
                }
            }
            emit(e, ")");
        }
        emit(e, ";\n");
        break;
    }

    case IR_BRANCH: {
        emit_indent(e);
        emit(e, "if (");
        if (inst->cond_local >= 0) {
            /* Branch on a LOCAL's value — all conditions decomposed at lowering time.
             * Check type: optional struct → .has_value, null-sentinel → as-is. */
            IRLocal *cl = &func->locals[inst->cond_local];
            Type *cond_eff = cl->type ? type_unwrap_distinct(cl->type) : NULL;
            if (cond_eff && cond_eff->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(cond_eff->optional.inner)) {
                emit_local_name(e, func, inst->cond_local);
                emit(e, ".has_value");
            } else {
                emit_local_name(e, func, inst->cond_local);
            }
        } else {
            emit(e, "1"); /* unconditional (shouldn't happen — lowering always sets cond_local) */
        }
        emit(e, ") goto _zer_bb%d; else goto _zer_bb%d;\n",
             inst->true_block, inst->false_block);
        break;
    }

    case IR_GOTO: {
        emit_indent(e);
        emit(e, "goto _zer_bb%d;\n", inst->goto_block);
        break;
    }

    case IR_RETURN: {
        emit_indent(e);

        /* All return expressions decomposed to local ID by lower_expr */
        if (inst->src1_local >= 0) {
            IRLocal *src = &func->locals[inst->src1_local];
            Type *ret = e->current_func_ret;
            Type *ret_eff = ret ? type_unwrap_distinct(ret) : NULL;
            Type *src_eff = src->type ? type_unwrap_distinct(src->type) : NULL;

            bool need_wrap = (ret_eff && ret_eff->kind == TYPE_OPTIONAL &&
                             !is_null_sentinel(ret_eff->optional.inner) &&
                             src_eff && src_eff->kind != TYPE_OPTIONAL);
            bool need_unwrap = (src_eff && src_eff->kind == TYPE_OPTIONAL &&
                               ret_eff && ret_eff->kind != TYPE_OPTIONAL &&
                               !is_null_sentinel(src_eff->optional.inner));

            if (need_wrap) {
                if (is_void_opt(ret_eff)) {
                    emit_local_name(e, func, inst->src1_local);
                    emit(e, ";\n");
                    emit_indent(e);
                    emit(e, "return (_zer_opt_void){ 1 };\n");
                } else {
                    emit(e, "return (");
                    emit_type(e, ret_eff);
                    emit(e, "){ ");
                    emit_local_name(e, func, inst->src1_local);
                    emit(e, ", 1 };\n");
                }
            } else if (need_unwrap) {
                emit(e, "return ");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ".value;\n");
            } else {
                emit(e, "return ");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ";\n");
            }
        } else {
            /* Void or bare return */
            if (func->is_async) {
                emit(e, "self->_zer_state = -1; return 1;\n");
            } else {
                emit_return_null(e);
                emit(e, "\n");
            }
        }
        break;
    }

    case IR_YIELD: {
        if (func->is_async) {
            emit_indent(e);
            emit(e, "self->_zer_state = %d; return 0;\n", e->async_yield_id);
            emit_indent(e);
            emit(e, "case %d:;\n", e->async_yield_id);
            e->async_yield_id++;
            /* After resume, goto the NEXT block (resume point created by start_block).
             * Without this, Duff's device falls through to the sequentially next block
             * which may be the loop exit, not the resume continuation. */
            if (inst->goto_block >= 0) {
                emit_indent(e);
                emit(e, "goto _zer_bb%d;\n", inst->goto_block);
            }
        }
        break;
    }

    case IR_AWAIT: {
        if (func->is_async) {
            emit_indent(e);
            emit(e, "case %d:;\n", e->async_yield_id);
            emit_indent(e);
            emit(e, "if (!(");
            if (inst->cond_local >= 0) {
                emit_local_name(e, func, inst->cond_local);
            } else {
                emit(e, "1"); /* shouldn't happen — lowering always sets cond_local */
            }
            emit(e, ")) { self->_zer_state = %d; return 0; }\n", e->async_yield_id);
            e->async_yield_id++;
            /* Same as yield — goto resume block */
            if (inst->goto_block >= 0) {
                emit_indent(e);
                emit(e, "goto _zer_bb%d;\n", inst->goto_block);
            }
        }
        break;
    }

    case IR_SPAWN: {
        /* Spawn uses the existing AST-based spawn emission (complex wrapper structs).
         * For now, fall through to AST emission via the expression. */
        emit_indent(e);
        emit(e, "/* IR_SPAWN %.*s — TODO: emit from IR */\n",
             (int)inst->func_name_len, inst->func_name);
        break;
    }

    case IR_LOCK: {
        emit_indent(e);
        emit(e, "/* IR_LOCK — TODO */\n");
        break;
    }

    case IR_UNLOCK: {
        emit_indent(e);
        emit(e, "/* IR_UNLOCK — TODO */\n");
        break;
    }

    case IR_POOL_ALLOC: case IR_SLAB_ALLOC: case IR_SLAB_ALLOC_PTR:
    case IR_POOL_FREE: case IR_SLAB_FREE: case IR_SLAB_FREE_PTR:
    case IR_POOL_GET:
    case IR_ARENA_ALLOC: case IR_ARENA_ALLOC_SLICE: case IR_ARENA_RESET:
    case IR_RING_PUSH: case IR_RING_POP: case IR_RING_PUSH_CHECKED: {
        /* Builtin ops no longer created by lowering — all go through IR_ASSIGN.
         * Dead code guard. */
        emit_indent(e);
        emit(e, "/* IR builtin dead — should be IR_ASSIGN */\n");
        break;
    }

    case IR_CRITICAL_BEGIN: {
        emit_indent(e);
        emit(e, "{ /* @critical */\n");
        e->indent++;
        emit_indent(e);
        emit(e, "#if defined(__ARM_ARCH)\n");
        emit_indent(e);
        emit(e, "uint32_t _zer_primask; __asm__ __volatile__(\"mrs %%0, primask\\n cpsid i\" : \"=r\"(_zer_primask));\n");
        emit_indent(e);
        emit(e, "#elif defined(__AVR__)\n");
        emit_indent(e);
        emit(e, "uint8_t _zer_sreg = SREG; __asm__ __volatile__(\"cli\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__riscv)\n");
        emit_indent(e);
        emit(e, "unsigned long _zer_mstatus; __asm__ __volatile__(\"csrrci %%0, mstatus, 8\" : \"=r\"(_zer_mstatus));\n");
        emit_indent(e);
        emit(e, "#else\n");
        emit_indent(e);
        emit(e, "__atomic_thread_fence(__ATOMIC_SEQ_CST);\n");
        emit_indent(e);
        emit(e, "#endif\n");
        break;
    }

    case IR_CRITICAL_END: {
        emit_indent(e);
        emit(e, "#if defined(__ARM_ARCH)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"msr primask, %%0\" :: \"r\"(_zer_primask));\n");
        emit_indent(e);
        emit(e, "#elif defined(__AVR__)\n");
        emit_indent(e);
        emit(e, "SREG = _zer_sreg;\n");
        emit_indent(e);
        emit(e, "#elif defined(__riscv)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"csrw mstatus, %%0\" :: \"r\"(_zer_mstatus));\n");
        emit_indent(e);
        emit(e, "#else\n");
        emit_indent(e);
        emit(e, "__atomic_thread_fence(__ATOMIC_SEQ_CST);\n");
        emit_indent(e);
        emit(e, "#endif\n");
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        break;
    }

    case IR_DEFER_PUSH: {
        /* Push defer body onto emitter's defer stack (same as AST path) */
        if (inst->defer_body) {
            if (e->defer_stack.count >= e->defer_stack.capacity) {
                int new_cap = e->defer_stack.capacity * 2;
                if (new_cap < 16) new_cap = 16;
                Node **new_stmts = (Node **)malloc(new_cap * sizeof(Node *));
                if (e->defer_stack.stmts) {
                    memcpy(new_stmts, e->defer_stack.stmts,
                           e->defer_stack.count * sizeof(Node *));
                    free(e->defer_stack.stmts);
                }
                e->defer_stack.stmts = new_stmts;
                e->defer_stack.capacity = new_cap;
            }
            e->defer_stack.stmts[e->defer_stack.count++] = inst->defer_body;
        }
        break;
    }

    case IR_DEFER_FIRE: {
        /* Fire all pending defers in LIFO order */
        for (int di = e->defer_stack.count - 1; di >= 0; di--) {
            emit_stmt(e, e->defer_stack.stmts[di]);
        }
        break;
    }

    case IR_INTRINSIC: {
        /* IR_INTRINSIC no longer created by lowering — all go through IR_ASSIGN.
         * Dead code guard. */
        emit_indent(e);
        emit(e, "/* IR_INTRINSIC dead — should be IR_ASSIGN */\n");
        break;
    }

    case IR_NOP:
        /* ASM pass-through or no-op */
        if (inst->expr) {
            emit_indent(e);
            emit_stmt(e, inst->expr); /* ASM nodes emit via stmt */
        }
        break;

    /* ================================================================
     * Three-address-code ops — emit from local IDs exclusively, zero emit_ir_value
     * ================================================================ */

    case IR_COPY: {
        if (inst->dest_local >= 0 && inst->src1_local >= 0) {
            IRLocal *dst = &func->locals[inst->dest_local];
            IRLocal *src = &func->locals[inst->src1_local];
            Type *dst_eff = dst->type ? type_unwrap_distinct(dst->type) : NULL;
            Type *src_eff = src->type ? type_unwrap_distinct(src->type) : NULL;

            /* Type adaptation */
            bool need_wrap = (dst_eff && dst_eff->kind == TYPE_OPTIONAL &&
                             !is_null_sentinel(dst_eff->optional.inner) &&
                             src_eff && src_eff->kind != TYPE_OPTIONAL);
            bool need_unwrap = (src_eff && src_eff->kind == TYPE_OPTIONAL &&
                               dst_eff && dst_eff->kind != TYPE_OPTIONAL &&
                               !is_null_sentinel(src_eff->optional.inner) &&
                               src_eff->optional.inner->kind != TYPE_VOID);
            bool need_slice = (dst_eff && dst_eff->kind == TYPE_SLICE &&
                              src_eff && src_eff->kind == TYPE_ARRAY);

            const char *sp = func->is_async ? "self->" : "";
            emit_indent(e);
            emit(e, "%s%.*s = ", sp, (int)dst->name_len, dst->name);

            if (need_slice) {
                emit(e, "(");
                emit_type(e, dst_eff);
                emit(e, "){ %s%.*s, %u };\n",
                     sp, (int)src->name_len, src->name,
                     src_eff ? (unsigned)src_eff->array.size : 0);
            } else if (need_wrap) {
                emit(e, "(");
                emit_type(e, dst_eff);
                emit(e, "){ %s%.*s, 1 };\n",
                     sp, (int)src->name_len, src->name);
            } else if (need_unwrap) {
                emit(e, "%s%.*s.value;\n",
                     sp, (int)src->name_len, src->name);
            } else {
                emit(e, "%s%.*s;\n",
                     sp, (int)src->name_len, src->name);
            }
        }
        break;
    }

    case IR_LITERAL: {
        if (inst->dest_local >= 0) {
            IRLocal *dst = &func->locals[inst->dest_local];
            emit_indent(e);
            if (func->is_async)
                emit(e, "self->%.*s = ", (int)dst->name_len, dst->name);
            else
                emit(e, "%.*s = ", (int)dst->name_len, dst->name);
            switch (inst->literal_kind) {
            case 0: /* int */
                if (inst->literal_int < 0)
                    emit(e, "(%lld)", (long long)inst->literal_int);
                else
                    emit(e, "%lluULL", (unsigned long long)(uint64_t)inst->literal_int);
                break;
            case 1: /* float */
                emit(e, "%.17g", inst->literal_float);
                break;
            case 2: /* string */
                emit(e, "(_zer_slice_u8){ (uint8_t*)\"%.*s\", %u }",
                     (int)inst->literal_str_len, inst->literal_str,
                     inst->literal_str_len);
                break;
            case 3: /* bool */
                emit(e, "%s", inst->literal_int ? "1" : "0");
                break;
            case 4: /* null */
                emit(e, "0");
                break;
            case 5: /* char */
                if (inst->literal_int >= 32 && inst->literal_int < 127 &&
                    inst->literal_int != '\'' && inst->literal_int != '\\')
                    emit(e, "'%c'", (char)inst->literal_int);
                else
                    emit(e, "%d", (int)inst->literal_int);
                break;
            case 6: /* ?void has_value */
                emit(e, "(_zer_opt_void){ %d }", (int)inst->literal_int);
                break;
            }
            emit(e, ";\n");
        }
        break;
    }

    case IR_BINOP: {
        /* Emit: dest = src1 OP src2 — from local IDs */
        if (inst->dest_local >= 0 && inst->src1_local >= 0 && inst->src2_local >= 0) {
            IRLocal *dst = &func->locals[inst->dest_local];
            IRLocal *s1 = &func->locals[inst->src1_local];
            IRLocal *s2 = &func->locals[inst->src2_local];
            const char *sp = func->is_async ? "self->" : "";
            const char *op = "?";
            switch (inst->op_token) {
            case TOK_PLUS: op = "+"; break; case TOK_MINUS: op = "-"; break;
            case TOK_STAR: op = "*"; break; case TOK_SLASH: op = "/"; break;
            case TOK_PERCENT: op = "%"; break;
            case TOK_AMP: op = "&"; break; case TOK_PIPE: op = "|"; break;
            case TOK_CARET: op = "^"; break;
            case TOK_LSHIFT: op = "<<"; break; case TOK_RSHIFT: op = ">>"; break;
            case TOK_EQEQ: op = "=="; break; case TOK_BANGEQ: op = "!="; break;
            case TOK_LT: op = "<"; break; case TOK_GT: op = ">"; break;
            case TOK_LTEQ: op = "<="; break; case TOK_GTEQ: op = ">="; break;
            case TOK_AMPAMP: op = "&&"; break; case TOK_PIPEPIPE: op = "||"; break;
            default: break;
            }
            emit_indent(e);
            emit(e, "%s%.*s = (%s%.*s %s %s%.*s);\n",
                 sp, (int)dst->name_len, dst->name,
                 sp, (int)s1->name_len, s1->name,
                 op,
                 sp, (int)s2->name_len, s2->name);
        }
        break;
    }

    case IR_UNOP: {
        /* Emit: dest = OP src — from local IDs */
        if (inst->dest_local >= 0 && inst->src1_local >= 0) {
            const char *op = "";
            switch (inst->op_token) {
            case TOK_MINUS: op = "-"; break;
            case TOK_BANG: op = "!"; break;
            case TOK_TILDE: op = "~"; break;
            case TOK_STAR: /* deref */
                emit_indent(e);
                emit_local_name(e, func, inst->dest_local);
                emit(e, " = *");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ";\n");
                goto unop_done;
            case TOK_AMP: /* addr-of */
                emit_indent(e);
                emit_local_name(e, func, inst->dest_local);
                emit(e, " = &");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ";\n");
                goto unop_done;
            default: break;
            }
            emit_indent(e);
            emit_local_name(e, func, inst->dest_local);
            emit(e, " = %s", op);
            emit_local_name(e, func, inst->src1_local);
            emit(e, ";\n");
        }
        unop_done:
        break;
    }

    case IR_FIELD_READ: {
        /* Emit: dest = src.field — from local IDs.
         * Complex types (handle auto-deref, opaque, builtins) use emit_expr via
         * the passthrough path (lower_expr creates IR_ASSIGN for those). */
        if (inst->dest_local >= 0 && inst->src1_local >= 0 && inst->field_name) {
            IRLocal *s1 = &func->locals[inst->src1_local];
            Type *st = s1->type ? type_unwrap_distinct(s1->type) : NULL;
            const char *accessor = ".";
            if (st && st->kind == TYPE_POINTER) accessor = "->";
            emit_indent(e);
            emit_local_name(e, func, inst->dest_local);
            emit(e, " = ");
            emit_local_name(e, func, inst->src1_local);
            emit(e, "%s%.*s;\n", accessor,
                 (int)inst->field_name_len, inst->field_name);
        }
        break;
    }

    case IR_INDEX_READ: {
        /* Emit: dest = src[idx] — from local IDs.
         * Bounds checks are in the AST path (emit_expr via IR_ASSIGN passthrough). */
        if (inst->dest_local >= 0 && inst->src1_local >= 0 && inst->src2_local >= 0) {
            IRLocal *s1 = &func->locals[inst->src1_local];
            /* Slice uses .ptr[] */
            Type *st = s1->type ? type_unwrap_distinct(s1->type) : NULL;
            emit_indent(e);
            emit_local_name(e, func, inst->dest_local);
            emit(e, " = ");
            emit_local_name(e, func, inst->src1_local);
            if (st && st->kind == TYPE_SLICE) emit(e, ".ptr");
            emit(e, "[");
            emit_local_name(e, func, inst->src2_local);
            emit(e, "];\n");
        }
        break;
    }

    case IR_FIELD_WRITE:
    case IR_CAST: {
        /* (Type)expr — emit from src_local + cast_type.
         * 3 paths: to *opaque (wrap), from *opaque (unwrap+check), simple C cast. */
        if (inst->dest_local >= 0 && inst->cast_type) {
            Type *tgt = inst->cast_type;
            Type *tgt_eff = type_unwrap_distinct(tgt);
            Type *src_type = (inst->src1_local >= 0) ? func->locals[inst->src1_local].type : NULL;
            Type *src_eff = src_type ? type_unwrap_distinct(src_type) : NULL;

            emit_indent(e);
            emit_local_name(e, func, inst->dest_local);
            emit(e, " = ");

            /* To *opaque: wrap with type_id */
            if (tgt_eff && tgt_eff->kind == TYPE_POINTER && tgt_eff->pointer.inner &&
                type_unwrap_distinct(tgt_eff->pointer.inner)->kind == TYPE_OPAQUE &&
                src_eff && src_eff->kind == TYPE_POINTER) {
                uint32_t tid = 0;
                if (src_eff->pointer.inner) {
                    Type *inner = type_unwrap_distinct(src_eff->pointer.inner);
                    if (inner->kind == TYPE_STRUCT) tid = inner->struct_type.type_id;
                    else if (inner->kind == TYPE_ENUM) tid = inner->enum_type.type_id;
                    else if (inner->kind == TYPE_UNION) tid = inner->union_type.type_id;
                }
                emit(e, "(_zer_opaque){(void*)(");
                emit_local_name(e, func, inst->src1_local);
                emit(e, "), %u}", (unsigned)tid);
            }
            /* From *opaque: unwrap .ptr with type check */
            else if (tgt_eff && tgt_eff->kind == TYPE_POINTER &&
                     src_eff &&
                     ((src_eff->kind == TYPE_POINTER && src_eff->pointer.inner &&
                       type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_OPAQUE) ||
                      src_eff->kind == TYPE_OPAQUE)) {
                uint32_t expected_tid = 0;
                if (tgt_eff->pointer.inner) {
                    Type *inner = type_unwrap_distinct(tgt_eff->pointer.inner);
                    if (inner->kind == TYPE_STRUCT) expected_tid = inner->struct_type.type_id;
                    else if (inner->kind == TYPE_ENUM) expected_tid = inner->enum_type.type_id;
                    else if (inner->kind == TYPE_UNION) expected_tid = inner->union_type.type_id;
                }
                if (expected_tid > 0 && inst->src1_local >= 0) {
                    int tmp = e->temp_count++;
                    emit(e, "({ _zer_opaque _zer_pc%d = ", tmp);
                    emit_local_name(e, func, inst->src1_local);
                    emit(e, "; if (_zer_pc%d.type_id != %u && _zer_pc%d.type_id != 0) "
                         "_zer_trap(\"type mismatch in cast\", __FILE__, __LINE__); (",
                         tmp, (unsigned)expected_tid, tmp);
                    emit_type(e, tgt);
                    emit(e, ")_zer_pc%d.ptr; })", tmp);
                } else if (inst->src1_local >= 0) {
                    emit(e, "((");
                    emit_type(e, tgt);
                    emit(e, ")(");
                    emit_local_name(e, func, inst->src1_local);
                    emit(e, ").ptr)");
                }
            }
            /* Simple C cast */
            else if (inst->src1_local >= 0) {
                emit(e, "((");
                emit_type(e, tgt);
                emit(e, ")");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ")");
            }
            emit(e, ";\n");
        }
        break;
    }

    case IR_STRUCT_INIT_DECOMP: {
        /* { .x = val1, .y = val2 } — emit compound literal from field locals.
         * Field names from inst->expr (NODE_STRUCT_INIT), values from call_arg_locals[]. */
        if (inst->dest_local >= 0 && inst->expr &&
            inst->expr->kind == NODE_STRUCT_INIT && inst->cast_type) {
            emit_indent(e);
            emit_local_name(e, func, inst->dest_local);
            emit(e, " = (");
            emit_type(e, inst->cast_type);
            emit(e, "){ ");
            for (int i = 0; i < inst->expr->struct_init.field_count; i++) {
                if (i > 0) emit(e, ", ");
                emit(e, ".%.*s = ", (int)inst->expr->struct_init.fields[i].name_len,
                     inst->expr->struct_init.fields[i].name);
                if (inst->call_arg_locals && i < inst->call_arg_local_count &&
                    inst->call_arg_locals[i] >= 0) {
                    emit_local_name(e, func, inst->call_arg_locals[i]);
                } else {
                    emit(e, "0"); /* fallback */
                }
            }
            emit(e, " };\n");
        }
        break;
    }

    case IR_INDEX_WRITE:
    case IR_ADDR_OF:
    case IR_DEREF_READ:
    case IR_CALL_DECOMP:
    case IR_INTRINSIC_DECOMP:
    case IR_ORELSE_DECOMP:
    case IR_SLICE_READ: {
        /* Future: emit from local IDs. */
        emit_indent(e);
        emit(e, "/* 3AC op %d — TODO */\n", inst->op);
        break;
    }

    default:
        emit_indent(e);
        emit(e, "/* IR op %d not yet implemented */\n", inst->op);
        break;
    }
}

/* Emit a regular (non-async) function from IR */
static void emit_regular_func_from_ir(Emitter *e, IRFunc *func) {
    /* Emit function signature (from AST node) */
    Node *fn = func->ast_node;
    if (!fn) return;

    /* Emit source mapping */
    if (e->source_file) {
        emit(e, "#line %d \"%s\"\n", fn->loc.line, e->source_file);
    }

    /* Return type + name.
     * func->return_type may be the function TYPE (func_ptr) from typemap.
     * Extract the actual return type from func_ptr.ret. */
    Type *ret = func->return_type;
    if (ret && ret->kind == TYPE_FUNC_PTR) ret = ret->func_ptr.ret;
    if (ret) emit_type(e, ret);
    else emit(e, "void");
    emit(e, " ");

    /* Mangled name */
    if (func->module_prefix) {
        emit(e, "%.*s__%.*s", (int)func->module_prefix_len, func->module_prefix,
             (int)func->name_len, func->name);
    } else {
        emit(e, "%.*s", (int)func->name_len, func->name);
    }

    /* Parameters — use AST types (same resolution as AST emitter).
     * IR local types may be ty_void for complex params (struct, pointer). */
    emit(e, "(");
    Type *func_type = checker_get_type(e->checker, fn);
    if (fn->func_decl.param_count == 0) {
        emit(e, "void");
    } else {
        for (int i = 0; i < fn->func_decl.param_count; i++) {
            if (i > 0) emit(e, ", ");
            ParamDecl *p = &fn->func_decl.params[i];
            Type *ptype = (func_type && func_type->kind == TYPE_FUNC_PTR &&
                          (uint32_t)i < func_type->func_ptr.param_count) ?
                func_type->func_ptr.params[i] : resolve_tynode(e, p->type);
            emit_type_and_name(e, ptype, p->name, p->name_len);
        }
    }
    emit(e, ") {\n");
    e->indent++;
    e->current_func_ret = ret; /* needed for IR_RETURN optional wrapping */
    e->defer_stack.count = 0; /* clear defer stack from previous function */

    /* Declare local variables (skip params — they're parameters) */
    for (int li = 0; li < func->local_count; li++) {
        IRLocal *l = &func->locals[li];
        if (l->is_param || l->is_static) continue;
        if (l->is_capture && l->type && l->type->kind == TYPE_VOID) continue;
        if (!l->type) continue;
        emit_indent(e);
        emit_type_and_name(e, l->type, l->name, l->name_len);
        emit(e, " = {0};\n"); /* auto-zero */
    }

    /* Disable source mapping during IR block emission — #line directives
     * collide with goto labels and statement expressions (BUG-418 class). */
    const char *saved_source = e->source_file;
    e->source_file = NULL;

    /* Emit basic blocks */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];

        /* Label for every block (including bb0 — goto may target entry) */
        emit(e, "_zer_bb%d:;\n", bb->id);

        /* Check if block has a capture that conflicts with another capture
         * of the same name but different type — wrap in C { } scope.
         * Only needed when same name is reused for different optional types. */
        bool has_capture_scope = false;
        if (bb->inst_count > 0 && bb->insts[0].op == IR_ASSIGN &&
            bb->insts[0].dest_local >= 0 &&
            func->locals[bb->insts[0].dest_local].is_capture) {
            /* Check if ANY other capture has the same name but different source */
            int cap_id = bb->insts[0].dest_local;
            IRLocal *cap = &func->locals[cap_id];
            bool name_conflict = false;
            for (int ci = 0; ci < func->local_count; ci++) {
                if (ci == cap_id) continue;
                if (!func->locals[ci].is_capture) continue;
                if (func->locals[ci].name_len == cap->name_len &&
                    memcmp(func->locals[ci].name, cap->name, cap->name_len) == 0) {
                    name_conflict = true; break;
                }
            }
            /* Wait — dedup means there's only ONE local per name.
             * The conflict is between the DECLARED type (first capture) and
             * ACTUAL type (current capture source). Check if source type differs. */
            if (!name_conflict) {
                Type *src = checker_get_type(e->checker, bb->insts[0].expr);
                Type *src_eff = src ? type_unwrap_distinct(src) : NULL;
                if (src_eff && src_eff->kind == TYPE_OPTIONAL) {
                    Type *inner = src_eff->optional.inner;
                    if (inner && !type_equals(type_unwrap_distinct(inner),
                                              type_unwrap_distinct(cap->type))) {
                        name_conflict = true; /* source inner != declared capture type */
                    }
                }
            }
            if (name_conflict) {
                has_capture_scope = true;
                emit_indent(e);
                emit(e, "{\n");
                e->indent++;
            }
        }

        /* Instructions */
        for (int ii = 0; ii < bb->inst_count; ii++) {
            emit_ir_inst(e, &bb->insts[ii], func);
        }

        if (has_capture_scope) {
            e->indent--;
            emit_indent(e);
            emit(e, "}\n");
        }
    }

    e->source_file = saved_source;
    e->current_func_ret = NULL;
    e->indent--;
    emit(e, "}\n\n");
}

/* Emit an async function from IR — state struct + init + poll */
static void emit_async_func_from_ir(Emitter *e, IRFunc *func) {
    Node *fn = func->ast_node;
    if (!fn) return;

    /* Build mangled name */
    char mname[256];
    int flen;
    if (func->module_prefix) {
        flen = snprintf(mname, sizeof(mname), "%.*s__%.*s",
            (int)func->module_prefix_len, func->module_prefix,
            (int)func->name_len, func->name);
    } else {
        flen = snprintf(mname, sizeof(mname), "%.*s",
            (int)func->name_len, func->name);
    }
    if (flen >= (int)sizeof(mname)) flen = (int)sizeof(mname) - 1;

    /* State struct = ALL locals */
    emit(e, "typedef struct {\n");
    emit(e, "    int _zer_state;\n");
    for (int li = 0; li < func->local_count; li++) {
        IRLocal *l = &func->locals[li];
        if (l->is_static) continue;
        if (!l->type) continue;
        emit(e, "    ");
        emit_type_and_name(e, l->type, l->name, l->name_len);
        emit(e, ";\n");
    }
    emit(e, "} _zer_async_%.*s;\n\n", flen, mname);

    /* Init function */
    emit(e, "static inline void _zer_async_%.*s_init(_zer_async_%.*s *self",
         flen, mname, flen, mname);
    for (int li = 0; li < func->local_count; li++) {
        if (!func->locals[li].is_param) continue;
        emit(e, ", ");
        emit_type_and_name(e, func->locals[li].type,
                           func->locals[li].name, func->locals[li].name_len);
    }
    emit(e, ") {\n");
    emit(e, "    memset(self, 0, sizeof(*self));\n");
    for (int li = 0; li < func->local_count; li++) {
        if (!func->locals[li].is_param) continue;
        emit(e, "    self->%.*s = %.*s;\n",
             (int)func->locals[li].name_len, func->locals[li].name,
             (int)func->locals[li].name_len, func->locals[li].name);
    }
    emit(e, "}\n\n");

    /* Poll function = Duff's device */
    emit(e, "static inline int _zer_async_%.*s_poll(_zer_async_%.*s *self) {\n",
         flen, mname, flen, mname);

    /* Emit static locals BEFORE the switch (C static, not in state struct) */
    {
        Node *body = func->ast_node ? func->ast_node->func_decl.body : NULL;
        if (body && body->kind == NODE_BLOCK) {
            for (int si = 0; si < body->block.stmt_count; si++) {
                Node *s = body->block.stmts[si];
                if (s && s->kind == NODE_VAR_DECL && s->var_decl.is_static) {
                    emit(e, "    static ");
                    Type *vt = checker_get_type(e->checker, s);
                    if (vt) emit_type_and_name(e, vt, s->var_decl.name, s->var_decl.name_len);
                    if (s->var_decl.init) {
                        emit(e, " = ");
                        emit_expr(e, s->var_decl.init);
                    }
                    emit(e, ";\n");
                }
            }
        }
    }

    emit(e, "    switch (self->_zer_state) { case 0:;\n");

    e->indent = 1;
    e->async_yield_id = 1;

    /* Set up async context so emit_expr uses self-> for locals.
     * Skip static locals — they're NOT self-> prefixed. */
    bool saved_async = e->in_async;
    e->in_async = true;
    e->async_local_count = 0;
    for (int li = 0; li < func->local_count; li++) {
        if (func->locals[li].is_static) continue;
        add_async_local(e, func->locals[li].name, func->locals[li].name_len);
    }

    /* Disable source mapping during IR blocks */
    const char *saved_source = e->source_file;
    e->source_file = NULL;

    /* Emit basic blocks */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        {
            emit_indent(e);
            emit(e, "_zer_bb%d:;\n", bb->id);
        }
        for (int ii = 0; ii < bb->inst_count; ii++) {
            emit_ir_inst(e, &bb->insts[ii], func);
        }
    }

    e->source_file = saved_source;
    emit(e, "    } self->_zer_state = -1; return 1;\n");
    emit(e, "}\n\n");

    /* Restore async context */
    e->in_async = saved_async;
}

/* Public: emit a function from its IR representation */
void emit_func_from_ir(Emitter *e, void *ir_func_ptr) {
    IRFunc *func = (IRFunc *)ir_func_ptr;
    if (!func) return;
    if (func->is_async) {
        emit_async_func_from_ir(e, func);
    } else {
        emit_regular_func_from_ir(e, func);
    }
}
