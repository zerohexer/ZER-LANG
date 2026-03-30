#include "emitter.h"
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
    return inner->kind == TYPE_POINTER || inner->kind == TYPE_FUNC_PTR;
}
#define IS_NULL_SENTINEL(inner_kind) \
    ((inner_kind) == TYPE_POINTER || (inner_kind) == TYPE_FUNC_PTR)
/* NOTE: Use is_null_sentinel(type) for full distinct-aware check.
 * IS_NULL_SENTINEL macro kept for backward compat where only kind is available. */

/* ---- Array size emission helper (BUG-275) ---- */
/* Emit array size — uses sizeof() for target-dependent sizes, numeric for constant */
static void emit_type(Emitter *e, Type *t); /* forward decl */
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
    return s && s->is_volatile;
}

/* ---- Type emission ---- */

static void emit_expr(Emitter *e, Node *node);
static void emit_stmt(Emitter *e, Node *node);
static Type *resolve_type_for_emit(Emitter *e, TypeNode *tn);

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
        if (t->pointer.is_const) emit(e, "const ");
        if (t->pointer.is_volatile) emit(e, "volatile ");
        emit_type(e, t->pointer.inner);
        emit(e, "*");
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
            emit(e, "_zer_opt_u32");  /* handles are uint32_t */
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
        emit(e, "struct ");
        EMIT_STRUCT_NAME(e, t);
        break;

    case TYPE_ENUM:
        emit(e, "int32_t"); /* enums are i32 */
        break;

    case TYPE_UNION:
        emit(e, "struct _union_");
        EMIT_UNION_NAME(e, t);
        break;

    case TYPE_HANDLE:
        emit(e, "uint32_t"); /* Handle = gen << 16 | index */
        break;

    case TYPE_ARENA:
        emit(e, "_zer_arena");
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
    if (t->kind == TYPE_DISTINCT && t->distinct.underlying->kind == TYPE_FUNC_PTR) {
        Type *fp = t->distinct.underlying;
        emit_type(e, fp->func_ptr.ret);
        emit(e, " (*%.*s)(", (int)name_len, name);
        for (uint32_t i = 0; i < fp->func_ptr.param_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_type(e, fp->func_ptr.params[i]);
        }
        emit(e, ")");
        return;
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
    if (t->kind == TYPE_OPTIONAL && t->optional.inner->kind == TYPE_DISTINCT &&
        t->optional.inner->distinct.underlying->kind == TYPE_FUNC_PTR) {
        Type *fp = t->optional.inner->distinct.underlying;
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
        /* division/modulo: trap on zero divisor */
        if (node->binary.op == TOK_SLASH || node->binary.op == TOK_PERCENT) {
            int tmp = e->temp_count++;
            Type *div_type = checker_get_type(e->checker,node->binary.left);
            bool is_signed_div = div_type && type_is_signed(div_type);
            emit(e, "({ __auto_type _zer_dv%d = ", tmp);
            emit_expr(e, node->binary.right);
            emit(e, "; if (_zer_dv%d == 0) ", tmp);
            emit(e, "_zer_trap(\"division by zero\", __FILE__, __LINE__); ");
            /* signed overflow: INT_MIN / -1 traps on x86/ARM */
            if (is_signed_div) {
                emit(e, "if (_zer_dv%d == -1) { __auto_type _zer_dd%d = ", tmp, tmp);
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
                if (opt_type && opt_type->kind == TYPE_OPTIONAL &&
                    !is_null_sentinel(opt_type->optional.inner)) {
                    /* struct optional: emit .has_value check */
                    if (node->binary.op == TOK_EQEQ) emit(e, "(!");
                    else emit(e, "(");
                    emit_expr(e, opt_node);
                    emit(e, ".has_value)");
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
            Type *obj_type = checker_get_type(e->checker,obj_node);
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
            if (tgt_type && tgt_type->kind == TYPE_ARRAY) {
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
            emit(e, "({ __auto_type _zer_dv%d = ", tmp);
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
        if (node->assign.op == TOK_EQ && tgt_type && val_type &&
            tgt_type->kind == TYPE_OPTIONAL &&
            !is_null_sentinel(tgt_type->optional.inner) &&
            val_type->kind != TYPE_OPTIONAL &&
            node->assign.value->kind != NODE_NULL_LIT) {
            emit(e, "(");
            emit_type(e, tgt_type);
            emit(e, "){ ");
            emit_expr(e, node->assign.value);
            emit(e, ", 1 }");
        } else if (node->assign.op == TOK_EQ && tgt_type &&
                   tgt_type->kind == TYPE_OPTIONAL &&
                   !is_null_sentinel(tgt_type->optional.inner) &&
                   node->assign.value->kind == NODE_NULL_LIT) {
            emit(e, "(");
            emit_type(e, tgt_type);
            if (tgt_type->optional.inner->kind == TYPE_VOID)
                emit(e, "){ 0 }");
            else
                emit(e, "){ {0} }");
        } else {
            emit_expr(e, node->assign.value);
        }
        } /* close else for compound shift */
        assign_done:
        break;

    case NODE_CALL: {
        /* comptime call — emit constant value directly */
        if (node->call.is_comptime_resolved) {
            emit(e, "%lld", (long long)node->call.comptime_value);
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
                        emit(e, "({uint8_t _zer_aok%d = 0; uint32_t _zer_ah%d = "
                             "_zer_pool_alloc(%.*s.slots, sizeof(%.*s.slots[0]), "
                             "%.*s.gen, %.*s.used, %llu, &_zer_aok%d); "
                             "(_zer_opt_u32){_zer_ah%d, _zer_aok%d}; })",
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
                        emit(e, "({uint8_t _zer_aok%d = 0; uint32_t _zer_ah%d = "
                             "_zer_slab_alloc(&%.*s, &_zer_aok%d); "
                             "(_zer_opt_u32){_zer_ah%d, _zer_aok%d}; })",
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
                            if (arg_type && arg_type->kind == TYPE_SLICE) {
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
                Type *arg_type = checker_get_type(e->checker,node->call.args[i]);
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
        if (obj_type && obj_type->kind == TYPE_ENUM) {
            emit(e, "_ZER_");
            EMIT_ENUM_NAME(e, obj_type);
            emit(e, "_%.*s",
                 (int)node->field.field_name_len, node->field.field_name);
            break;
        }
        /* check if object is a pointer → use -> instead of . */
        emit_expr(e, node->field.object);
        if (obj_type && obj_type->kind == TYPE_POINTER) {
            emit(e, "->%.*s", (int)node->field.field_name_len, node->field.field_name);
        } else {
            emit(e, ".%.*s", (int)node->field.field_name_len, node->field.field_name);
        }
        break;
    }

    case NODE_INDEX: {
        /* Inline bounds check using comma operator:
         *   array:  (_zer_bounds_check(idx, size, ...), arr)[idx]
         *   slice:  (_zer_bounds_check(idx, s.len, ...), s.ptr)[idx]
         * Comma operator preserves lvalue (array decays to pointer).
         * Inline check respects short-circuit (&&/||) and works in
         * if/while/for conditions — fixes both hoisting and missing-check bugs. */
        Type *idx_obj_type = checker_get_type(e->checker,node->index_expr.object);
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
                /* hoist both object and index for single-eval */
                int tmp = e->temp_count++;
                emit(e, "*({ __auto_type _zer_obj%d = ", tmp);
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
        Type *obj_type = checker_get_type(e->checker,node->slice.object);
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
            emit(e, "({ __auto_type _zer_so%d = ", sl_tmp);
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
        bool is_ptr_optional = orelse_type &&
            orelse_type->kind == TYPE_OPTIONAL &&
            is_null_sentinel(orelse_type->optional.inner);

        bool is_void_optional = orelse_type &&
            orelse_type->kind == TYPE_OPTIONAL &&
            orelse_type->optional.inner->kind == TYPE_VOID;

        if (node->orelse.fallback_is_return || node->orelse.fallback_is_break ||
            node->orelse.fallback_is_continue) {
            if (is_void_optional) {
                /* ?void orelse return/break/continue — no .value to extract,
                 * just check has_value and branch */
                int tmp = e->temp_count++;
                /* BUG-289: use __typeof__ to preserve volatile in orelse temp */
                emit(e, "({__typeof__(");
                emit_expr(e, node->orelse.expr);
                emit(e, ") _zer_tmp%d = ", tmp);
                emit_expr(e, node->orelse.expr);
                emit(e, "; if (!_zer_tmp%d.has_value) { ", tmp);
                /* emit defers before return/break/continue */
                if (node->orelse.fallback_is_return) {
                    emit_defers(e);
                    if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
                        !is_null_sentinel(e->current_func_ret->optional.inner)) {
                        emit(e, "return (");
                        emit_type(e, e->current_func_ret);
                        if (e->current_func_ret->optional.inner->kind == TYPE_VOID)
                            emit(e, "){ 0 }; ");
                        else
                            emit(e, "){ {0} }; ");
                    } else if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                        emit(e, "return 0; ");
                    } else {
                        emit(e, "return; ");
                    }
                } else if (node->orelse.fallback_is_break) {
                    emit_defers_from(e, e->loop_defer_base);
                    emit(e, "break; ");
                } else {
                    emit_defers_from(e, e->loop_defer_base);
                    emit(e, "continue; ");
                }
                emit(e, "} (void)0; })");
            } else {
                /* ?T (non-void, non-pointer) orelse return/break/continue */
                int tmp = e->temp_count++;
                /* BUG-289: use __typeof__ to preserve volatile in orelse temp */
                emit(e, "({__typeof__(");
                emit_expr(e, node->orelse.expr);
                emit(e, ") _zer_tmp%d = ", tmp);
                emit_expr(e, node->orelse.expr);
                if (is_ptr_optional) {
                    emit(e, "; if (!_zer_tmp%d) { ", tmp);
                } else {
                    emit(e, "; if (!_zer_tmp%d.has_value) { ", tmp);
                }
                /* emit defers before return/break/continue */
                if (node->orelse.fallback_is_return) {
                    emit_defers(e);
                    if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
                        !is_null_sentinel(e->current_func_ret->optional.inner)) {
                        emit(e, "return (");
                        emit_type(e, e->current_func_ret);
                        if (e->current_func_ret->optional.inner->kind == TYPE_VOID)
                            emit(e, "){ 0 }; ");
                        else
                            emit(e, "){ {0} }; ");
                    } else if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                        emit(e, "return 0; ");
                    } else {
                        emit(e, "return; ");
                    }
                } else if (node->orelse.fallback_is_break) {
                    emit_defers_from(e, e->loop_defer_base);
                    emit(e, "break; ");
                } else {
                    emit_defers_from(e, e->loop_defer_base);
                    emit(e, "continue; ");
                }
                if (is_ptr_optional) {
                    emit(e, "} _zer_tmp%d; })", tmp);
                } else {
                    emit(e, "} _zer_tmp%d.value; })", tmp);
                }
            }
        } else if (node->orelse.fallback &&
                   node->orelse.fallback->kind == NODE_BLOCK) {
            /* orelse { block } — statement-only form
             * → { auto _t = expr; if (!_t.has_value) { block } } */
            int tmp = e->temp_count++;
            emit(e, "({__auto_type _zer_tmp%d = ", tmp);
            emit_expr(e, node->orelse.expr);
            emit(e, "; ");
            if (is_ptr_optional) {
                emit(e, "if (!_zer_tmp%d) ", tmp);
            } else {
                emit(e, "if (!_zer_tmp%d.has_value) ", tmp);
            }
            emit_stmt(e, node->orelse.fallback);
            emit(e, " 0; })");
        } else {
            int tmp = e->temp_count++;
            emit(e, "({__auto_type _zer_tmp%d = ", tmp);
            emit_expr(e, node->orelse.expr);
            if (is_ptr_optional) {
                emit(e, "; _zer_tmp%d ? _zer_tmp%d : ", tmp, tmp);
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
            /* @ptrcast(*T, expr) → (T*)(expr) */
            emit(e, "(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
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
             * With mmio ranges: variable addresses get runtime range check */
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
                emit(e, ")) _zer_trap(\"@inttoptr: address outside mmio range\", __FILE__, __LINE__); (");
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
            bool dest_is_slice = buf_type && buf_type->kind == TYPE_SLICE;
            /* BUG-223/RF7: check if destination is volatile — walk field/index chains */
            /* RF11: use shared volatile detection helper */
            bool dest_volatile = (node->intrinsic.arg_count > 0) ?
                expr_is_volatile(e, node->intrinsic.args[0]) : false;
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
            if (buf_type && buf_type->kind == TYPE_ARRAY) {
                emit(e, "; if (_zer_cs%d.len + 1 > %llu) ",
                     tmp, (unsigned long long)buf_type->array.size);
                emit(e, "_zer_trap(\"@cstr buffer overflow\", __FILE__, __LINE__); ");
            } else if (dest_is_slice) {
                emit(e, "; if (_zer_cs%d.len + 1 > _zer_cd%d.len) ", tmp, tmp);
                emit(e, "_zer_trap(\"@cstr buffer overflow\", __FILE__, __LINE__); ");
            } else {
                emit(e, "; ");
            }
            if (dest_volatile) {
                /* volatile: byte-by-byte copy (memcpy discards volatile qualifier) */
                emit(e, "for (size_t _i = 0; _i < _zer_cs%d.len; _i++) _zer_cb%d[_i] = _zer_cs%d.ptr[_i]; ",
                     tmp, tmp, tmp);
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
            emit_stmt(e, node->block.stmts[i]);
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
        Type *type = checker_get_type(e->checker,node);
        /* propagate volatile flag from var-decl to pointer type */
        if (node->var_decl.is_volatile && type && type->kind == TYPE_POINTER) {
            Type *vp = type_pointer(e->arena, type->pointer.inner);
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
            bool or_is_ptr = or_expr_type &&
                or_expr_type->kind == TYPE_OPTIONAL &&
                is_null_sentinel(or_expr_type->optional.inner);

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
                emit(e, "{ "); emit_defers(e);
                if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
                    !is_null_sentinel(e->current_func_ret->optional.inner)) {
                    /* ?T function: return null optional */
                    emit(e, "return (");
                    emit_type(e, e->current_func_ret);
                    if (e->current_func_ret->optional.inner->kind == TYPE_VOID)
                        emit(e, "){ 0 }; }\n");
                    else
                        emit(e, "){ {0} }; }\n");
                } else if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                    emit(e, "return 0; }\n");
                } else {
                    emit(e, "return; }\n");
                }
            } else if (node->var_decl.init->orelse.fallback_is_break) {
                emit(e, "{ "); emit_defers_from(e, e->loop_defer_base); emit(e, "break; }\n");
            } else {
                emit(e, "{ "); emit_defers_from(e, e->loop_defer_base); emit(e, "continue; }\n");
            }
            emit_indent(e);
            if (or_is_ptr) {
                emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
                emit(e, " = _zer_or%d;\n", tmp);
            } else if (type && type->kind == TYPE_OPTIONAL &&
                       type->optional.inner->kind == TYPE_VOID) {
                /* ?void has no .value — keep as ?void (has_value only) */
                emit(e, "_zer_opt_void %.*s = _zer_or%d;\n",
                     (int)node->var_decl.name_len, node->var_decl.name, tmp);
            } else if (type && type->kind == TYPE_SLICE) {
                /* slice: use __auto_type to avoid anonymous struct incompatibility */
                emit(e, "__auto_type %.*s = _zer_or%d.value;\n",
                     (int)node->var_decl.name_len, node->var_decl.name, tmp);
            } else {
                emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
                emit(e, " = _zer_or%d.value;\n", tmp);
            }
            break;
        }

        /* Normal var decl */
        emit_indent(e);
        if (node->var_decl.is_static) emit(e, "static ");
        if (node->var_decl.is_volatile && !(type && type->kind == TYPE_POINTER))
            emit(e, "volatile ");
        emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
        if (node->var_decl.init) {
            /* Optional init: null → {0, 0}, value → {val, 1}
             * But if init is a function call returning ?T, just assign directly */
            if (type && type->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(type->optional.inner)) {
                if (node->var_decl.init->kind == NODE_NULL_LIT) {
                    if (type->optional.inner->kind == TYPE_VOID)
                        emit(e, " = { 0 }");
                    else
                        emit(e, " = {0}");
                } else if (node->var_decl.init->kind == NODE_CALL ||
                           node->var_decl.init->kind == NODE_ORELSE) {
                    /* call/orelse might already return ?T — assign directly */
                    emit(e, " = ");
                    emit_expr(e, node->var_decl.init);
                } else if (node->var_decl.init->kind == NODE_IDENT) {
                    /* check if ident is already ?T or needs wrapping */
                    Type *init_type = checker_get_type(e->checker,node->var_decl.init);
                    if (init_type && init_type->kind == TYPE_OPTIONAL) {
                        emit(e, " = ");
                        emit_expr(e, node->var_decl.init);
                    } else {
                        emit(e, " = (");
                        emit_type(e, type);
                        emit(e, "){ ");
                        emit_expr(e, node->var_decl.init);
                        emit(e, ", 1 }");
                    }
                } else {
                    emit(e, " = (");
                    emit_type(e, type);
                    emit(e, "){ ");
                    emit_expr(e, node->var_decl.init);
                    emit(e, ", 1 }");
                }
            } else {
                /* array→slice coercion at var-decl */
                Type *init_type = checker_get_type(e->checker,node->var_decl.init);
                if (type && type->kind == TYPE_SLICE &&
                    init_type && init_type->kind == TYPE_ARRAY) {
                    emit(e, " = ");
                    emit_array_as_slice(e, node->var_decl.init, init_type, type);
                } else if (type && type->kind == TYPE_ARRAY &&
                           init_type && init_type->kind == TYPE_ARRAY) {
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
                         eff_local->kind == TYPE_ARENA || eff_local->kind == TYPE_SLICE)) {
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
            bool is_ptr_opt = cond_type &&
                cond_type->kind == TYPE_OPTIONAL &&
                is_null_sentinel(cond_type->optional.inner);

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
                } else if (cond_type && cond_type->kind == TYPE_OPTIONAL &&
                           cond_type->optional.inner->kind == TYPE_VOID) {
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
                emit(e, "else ");
                emit_stmt(e, node->if_stmt.else_body);
            }
            e->indent--;
            emit_indent(e);
            emit(e, "}\n");
        } else {
            /* regular if */
            Type *cond_t = checker_get_type(e->checker,node->if_stmt.cond);
            bool cond_is_struct_opt = cond_t &&
                cond_t->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(cond_t->optional.inner);
            emit_indent(e);
            emit(e, "if (");
            emit_expr(e, node->if_stmt.cond);
            if (cond_is_struct_opt) emit(e, ".has_value");
            emit(e, ") ");
            emit_stmt(e, node->if_stmt.then_body);
            if (node->if_stmt.else_body) {
                emit_indent(e);
                emit(e, "else ");
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
        bool while_is_struct_opt = while_cond_t &&
            while_cond_t->kind == TYPE_OPTIONAL &&
            !is_null_sentinel(while_cond_t->optional.inner);
        emit_indent(e);
        emit(e, "while (");
        emit_expr(e, node->while_stmt.cond);
        if (while_is_struct_opt) emit(e, ".has_value");
        emit(e, ") ");
        emit_stmt(e, node->while_stmt.body);
        e->loop_defer_base = saved_loop_base;
        break;
    }

    case NODE_RETURN:
        /* bounds checks now inline in emit_expr(NODE_INDEX) */
        /* emit defers before return (reverse order) */
        emit_defers(e);
        emit_indent(e);
        if (node->ret.expr) {
            /* return null from ?T function → return {0, 0} (or {0} for ?void) */
            if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(e->current_func_ret->optional.inner) &&
                node->ret.expr->kind == NODE_NULL_LIT) {
                emit(e, "return (");
                emit_type(e, e->current_func_ret);
                if (e->current_func_ret->optional.inner->kind == TYPE_VOID) {
                    emit(e, "){ 0 };\n");
                } else {
                    emit(e, "){ {0} };\n");
                }
            }
            /* return value from ?T function → return {value, 1} */
            else if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
                     !is_null_sentinel(e->current_func_ret->optional.inner) &&
                     node->ret.expr->kind != NODE_NULL_LIT) {
                /* check if expr already has the optional type (e.g. return ring.pop()) */
                Type *expr_type = checker_get_type(e->checker,node->ret.expr);
                if (expr_type && type_equals(expr_type, e->current_func_ret)) {
                    /* already optional — return directly */
                    emit(e, "return ");
                    emit_expr(e, node->ret.expr);
                    emit(e, ";\n");
                } else if (e->current_func_ret->optional.inner->kind == TYPE_VOID) {
                    /* ?void: emit void expr as statement, then return {1} */
                    emit_expr(e, node->ret.expr);
                    emit(e, ";\n");
                    emit_indent(e);
                    emit(e, "return (_zer_opt_void){ 1 };\n");
                } else {
                    emit(e, "return (");
                    emit_type(e, e->current_func_ret);
                    emit(e, "){ ");
                    emit_expr(e, node->ret.expr);
                    emit(e, ", 1 };\n");
                }
            } else {
                /* array→slice coercion on return */
                Type *expr_type = checker_get_type(e->checker,node->ret.expr);
                if (e->current_func_ret && e->current_func_ret->kind == TYPE_SLICE &&
                    expr_type && expr_type->kind == TYPE_ARRAY) {
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
             * For null-sentinel types (?*T, ?FuncPtr), bare return = return NULL (none) */
            if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
                is_null_sentinel(e->current_func_ret->optional.inner)) {
                /* null-sentinel: bare return = none = NULL */
                emit(e, "return (");
                emit_type(e, e->current_func_ret->optional.inner);
                emit(e, ")0;\n");
            } else if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL) {
                emit(e, "return (");
                emit_type(e, e->current_func_ret);
                if (e->current_func_ret->optional.inner->kind == TYPE_VOID) {
                    emit(e, "){ 1 };\n");
                } else {
                    emit(e, "){ 0, 1 };\n");
                }
            } else {
                emit(e, "return;\n");
            }
        }
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

    case NODE_EXPR_STMT:
        /* bounds checks now inline in emit_expr(NODE_INDEX) */
        emit_indent(e);
        emit_expr(e, node->expr_stmt.expr);
        emit(e, ";\n");
        break;

    case NODE_ASM:
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"%.*s\");\n",
             (int)node->asm_stmt.code_len, node->asm_stmt.code);
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
        /* BUG-196b: detect struct-based optional in switch */
        bool is_opt_switch = false;
        if (sw_type && sw_type->kind == TYPE_OPTIONAL) {
            Type *inner = sw_type->optional.inner;
            if (!is_null_sentinel(inner)) {
                is_opt_switch = true;
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
                /* optional switch: compare .value (and require .has_value) */
                if (i > 0) emit(e, "else ");
                emit(e, "if (");
                for (int j = 0; j < arm->value_count; j++) {
                    if (j > 0) emit(e, " || ");
                    emit(e, "(_zer_sw%d.has_value && _zer_sw%d.value == ", sw_tmp, sw_tmp);
                    emit_expr(e, arm->values[j]);
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
        /* propagate volatile to pointer type */
        if (inner && inner->kind == TYPE_POINTER) {
            Type *vp = type_pointer(e->arena, inner->pointer.inner);
            vp->pointer.is_volatile = true;
            return vp;
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
    default:
        return ty_void;
    }
}

/* ================================================================
 * TOP-LEVEL DECLARATION EMISSION
 * ================================================================ */

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

static void emit_func_decl(Emitter *e, Node *node) {

    Type *func_type = checker_get_type(e->checker,node);
    Type *ret = (func_type && func_type->kind == TYPE_FUNC_PTR) ?
        func_type->func_ptr.ret : NULL;

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
    /* propagate volatile flag from var-decl to pointer type */
    if (node->var_decl.is_volatile && type && type->kind == TYPE_POINTER) {
        Type *vp = type_pointer(e->arena, type->pointer.inner);
        vp->pointer.is_volatile = true;
        type = vp;
    }

    /* Pool(T, N) → use macro for struct layout */
    if (type && type->kind == TYPE_POOL) {
        emit(e, "struct { ");
        emit_type(e, type->pool.elem);
        emit(e, " slots[%llu]; uint16_t gen[%llu]; uint8_t used[%llu]; } ",
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
    if (node->var_decl.is_volatile && !(type && type->kind == TYPE_POINTER))
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
        /* optional null init needs struct literal, not scalar 0 */
        if (type && type->kind == TYPE_OPTIONAL &&
            !is_null_sentinel(type->optional.inner) &&
            node->var_decl.init->kind == NODE_NULL_LIT) {
            if (type->optional.inner->kind == TYPE_VOID)
                emit(e, " = { 0 }");
            else
                emit(e, " = {0}");
        } else {
            emit(e, " = ");
            emit_expr(e, node->var_decl.init);
        }
    } else {
        /* auto-zero — unwrap distinct to check if compound init needed */
        Type *eff_type = type_unwrap_distinct(type);
        if (eff_type && (eff_type->kind == TYPE_STRUCT || eff_type->kind == TYPE_ARRAY ||
                     eff_type->kind == TYPE_OPTIONAL || eff_type->kind == TYPE_UNION ||
                     eff_type->kind == TYPE_SLICE)) {
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
            if (!has_def) break;
        }
        emit_func_decl(e, decl);
        break;

    case NODE_GLOBAL_VAR:
        emit_global_var(e, decl);
        break;

    case NODE_UNION_DECL: {
        Type *ut = checker_get_type(e->checker, decl);
        emit(e, "/* tagged union %.*s */\n",
             (int)decl->union_decl.name_len, decl->union_decl.name);
        for (int j = 0; j < decl->union_decl.variant_count; j++) {
            UnionVariant *v = &decl->union_decl.variants[j];
            emit(e, "#define _ZER_");
            if (ut) EMIT_UNION_NAME(e, ut);
            else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
            emit(e, "_TAG_%.*s %d\n", (int)v->name_len, v->name, j);
        }
        emit(e, "struct _union_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, " {\n    int32_t _tag;\n    union {\n");
        for (int j = 0; j < decl->union_decl.variant_count; j++) {
            UnionVariant *v = &decl->union_decl.variants[j];
            Type *vtype = resolve_tynode(e,v->type);
            emit(e, "        ");
            emit_type(e, vtype);
            emit(e, " %.*s;\n", (int)v->name_len, v->name);
        }
        emit(e, "    };\n};\n");
        /* optional/slice/opt-slice typedefs */
        emit(e, "typedef struct { struct _union_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, " value; uint8_t has_value; } _zer_opt_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, ";\n");
        emit(e, "typedef struct { struct _union_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, "* ptr; size_t len; } _zer_slice_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, ";\n");
        emit(e, "typedef struct { volatile struct _union_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, "* ptr; size_t len; } _zer_vslice_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, ";\n");
        emit(e, "typedef struct { _zer_slice_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, " value; uint8_t has_value; } _zer_opt_slice_");
        if (ut) EMIT_UNION_NAME(e, ut);
        else emit(e, "%.*s", (int)decl->union_decl.name_len, decl->union_decl.name);
        emit(e, ";\n\n");
        break;
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
        emit(e, "#include \"%.*s\"\n",
             (int)decl->cinclude.path_len, decl->cinclude.path);
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

    default:
        break;
    }
}

void emit_file(Emitter *e, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;

    /* C preamble */
    emit(e, "/* Generated by ZER compiler — do not edit */\n");
    emit(e, "/* Compile with: gcc -std=c99 -fwrapv -fno-strict-aliasing */\n");
    emit(e, "#include <stdint.h>\n");
    emit(e, "#include <stddef.h>\n");
    emit(e, "#include <string.h>\n");
    emit(e, "#include <stdio.h>\n");
    emit(e, "#include <stdlib.h>\n");
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
    emit(e, "/* ZER Pool runtime */\n");
    emit(e, "#define _ZER_POOL_DECL(NAME, ELEM_TYPE, CAPACITY) \\\n");
    emit(e, "    struct { \\\n");
    emit(e, "        ELEM_TYPE slots[CAPACITY]; \\\n");
    emit(e, "        uint16_t gen[CAPACITY]; \\\n");
    emit(e, "        uint8_t used[CAPACITY]; \\\n");
    emit(e, "    } NAME = {0}\n");
    emit(e, "\n");
    emit(e, "static inline uint32_t _zer_pool_alloc(void *pool_ptr, size_t slot_size, "
            "uint16_t *gen, uint8_t *used, size_t capacity, uint8_t *ok) {\n");
    emit(e, "    for (uint32_t i = 0; i < capacity; i++) {\n");
    emit(e, "        if (!used[i]) {\n");
    emit(e, "            used[i] = 1;\n");
    emit(e, "            *ok = 1;\n");
    emit(e, "            return (uint32_t)((gen[i] << 16) | i);\n");
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

    emit(e, "static inline void *_zer_pool_get(void *slots, uint16_t *gen, uint8_t *used, "
            "size_t slot_size, uint32_t handle, size_t capacity) {\n");
    emit(e, "    uint32_t idx = handle & 0xFFFF;\n");
    emit(e, "    uint16_t h_gen = (uint16_t)(handle >> 16);\n");
    emit(e, "    if (idx >= capacity || !used[idx] || gen[idx] != h_gen) {\n");
    emit(e, "        _zer_trap(\"use-after-free: handle generation mismatch\", __FILE__, __LINE__);\n");
    emit(e, "    }\n");
    emit(e, "    return (char*)slots + idx * slot_size;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void _zer_pool_free(uint16_t *gen, uint8_t *used, "
            "uint32_t handle, size_t capacity) {\n");
    emit(e, "    uint32_t idx = handle & 0xFFFF;\n");
    emit(e, "    if (idx < capacity) {\n");
    emit(e, "        used[idx] = 0;\n");
    emit(e, "        gen[idx]++;\n");
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

    /* ZER runtime: Slab dynamic allocator */
    emit(e, "/* ZER Slab runtime — dynamic growable pool via mmap/malloc */\n");
    emit(e, "#define _ZER_SLAB_PAGE_SLOTS 64\n");
    emit(e, "typedef struct {\n");
    emit(e, "    char **pages;         /* array of page pointers */\n");
    emit(e, "    uint16_t *gen;        /* flat generation array */\n");
    emit(e, "    uint8_t *used;        /* flat used-slot array */\n");
    emit(e, "    uint32_t page_count;\n");
    emit(e, "    uint32_t page_cap;\n");
    emit(e, "    uint32_t total_slots;\n");
    emit(e, "    size_t slot_size;\n");
    emit(e, "} _zer_slab;\n\n");

    emit(e, "static inline uint32_t _zer_slab_alloc(_zer_slab *s, uint8_t *ok) {\n");
    emit(e, "    /* scan for free slot */\n");
    emit(e, "    for (uint32_t i = 0; i < s->total_slots; i++) {\n");
    emit(e, "        if (!s->used[i]) {\n");
    emit(e, "            s->used[i] = 1;\n");
    emit(e, "            *ok = 1;\n");
    emit(e, "            return (uint32_t)((s->gen[i] << 16) | i);\n");
    emit(e, "        }\n");
    emit(e, "    }\n");
    emit(e, "    /* grow: add a new page */\n");
    emit(e, "    if (s->page_count >= s->page_cap) {\n");
    emit(e, "        uint32_t nc = s->page_cap < 4 ? 4 : s->page_cap * 2;\n");
    emit(e, "        char **np = (char**)calloc(nc, sizeof(char*));\n");
    emit(e, "        uint16_t *ng = (uint16_t*)calloc(nc * _ZER_SLAB_PAGE_SLOTS, sizeof(uint16_t));\n");
    emit(e, "        uint8_t *nu = (uint8_t*)calloc(nc * _ZER_SLAB_PAGE_SLOTS, sizeof(uint8_t));\n");
    emit(e, "        if (!np || !ng || !nu) { *ok = 0; return 0; }\n");
    emit(e, "        if (s->pages) { memcpy(np, s->pages, s->page_count * sizeof(char*)); free(s->pages); }\n");
    emit(e, "        if (s->gen) { memcpy(ng, s->gen, s->total_slots * sizeof(uint16_t)); free(s->gen); }\n");
    emit(e, "        if (s->used) { memcpy(nu, s->used, s->total_slots * sizeof(uint8_t)); free(s->used); }\n");
    emit(e, "        s->pages = np; s->gen = ng; s->used = nu; s->page_cap = nc;\n");
    emit(e, "    }\n");
    emit(e, "    char *page = (char*)calloc(_ZER_SLAB_PAGE_SLOTS, s->slot_size);\n");
    emit(e, "    if (!page) { *ok = 0; return 0; }\n");
    emit(e, "    s->pages[s->page_count] = page;\n");
    emit(e, "    uint32_t base = s->total_slots;\n");
    emit(e, "    s->page_count++;\n");
    emit(e, "    s->total_slots += _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "    s->used[base] = 1;\n");
    emit(e, "    *ok = 1;\n");
    emit(e, "    return (uint32_t)((s->gen[base] << 16) | base);\n");
    emit(e, "}\n\n");

    emit(e, "static inline void *_zer_slab_get(_zer_slab *s, uint32_t handle) {\n");
    emit(e, "    uint32_t idx = handle & 0xFFFF;\n");
    emit(e, "    uint16_t gen = (uint16_t)(handle >> 16);\n");
    emit(e, "    if (idx >= s->total_slots || !s->used[idx] || s->gen[idx] != gen) {\n");
    emit(e, "        _zer_trap(\"slab: use-after-free or invalid handle\", __FILE__, __LINE__);\n");
    emit(e, "    }\n");
    emit(e, "    uint32_t page = idx / _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "    uint32_t slot = idx %% _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "    return s->pages[page] + slot * s->slot_size;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void _zer_slab_free(_zer_slab *s, uint32_t handle) {\n");
    emit(e, "    uint32_t idx = handle & 0xFFFF;\n");
    emit(e, "    if (idx < s->total_slots) {\n");
    emit(e, "        s->used[idx] = 0;\n");
    emit(e, "        s->gen[idx]++;\n");
    emit(e, "    }\n");
    emit(e, "}\n\n");

    emit(e, "\n");

    /* emit declarations — uses unified emit_top_level_decl (RF2) */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        emit_top_level_decl(e, file_node->file.decls[i], file_node, i);
    }
}

/* RF2: unified — uses same emit_top_level_decl as emit_file */
void emit_file_no_preamble(Emitter *e, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;
    emit(e, "\n/* --- imported module --- */\n\n");
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_IMPORT || decl->kind == NODE_CINCLUDE) continue;
        emit_top_level_decl(e, decl, file_node, i);
    }
}
