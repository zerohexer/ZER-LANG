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

/* ---- Type emission ---- */

static void emit_type(Emitter *e, Type *t);
static void emit_expr(Emitter *e, Node *node);
static void emit_stmt(Emitter *e, Node *node);
static Type *resolve_type_for_emit(Emitter *e, TypeNode *tn);

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
        emit_type(e, t->pointer.inner);
        emit(e, "*");
        break;

    case TYPE_OPTIONAL:
        /* ?*T → pointer (null sentinel) */
        if (t->optional.inner->kind == TYPE_POINTER) {
            emit_type(e, t->optional.inner);
            break;
        }
        /* ?T → named optional typedef */
        switch (t->optional.inner->kind) {
        case TYPE_VOID:  emit(e, "_zer_opt_void"); break;
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
        default:
            /* fallback: anonymous struct */
            emit(e, "struct { ");
            emit_type(e, t->optional.inner);
            emit(e, " value; uint8_t has_value; }");
            break;
        }
        break;

    case TYPE_SLICE:
        switch (t->slice.inner->kind) {
        case TYPE_U8:  emit(e, "_zer_slice_u8"); break;
        case TYPE_U32: emit(e, "_zer_slice_u32"); break;
        default:
            emit(e, "struct { ");
            emit_type(e, t->slice.inner);
            emit(e, "* ptr; size_t len; }");
            break;
        }
        break;

    case TYPE_ARRAY:
        emit_type(e, t->array.inner);
        break;

    case TYPE_STRUCT:
        emit(e, "struct _zer_%.*s", (int)t->struct_type.name_len, t->struct_type.name);
        break;

    case TYPE_ENUM:
        emit(e, "int32_t"); /* enums are i32 */
        break;

    case TYPE_UNION:
        emit(e, "struct _zer_union_%.*s", (int)t->union_type.name_len, t->union_type.name);
        break;

    case TYPE_HANDLE:
        emit(e, "uint32_t"); /* Handle = gen << 16 | index */
        break;

    case TYPE_ARENA:
        emit(e, "struct _zer_arena");
        break;

    case TYPE_POOL:
        emit(e, "struct _zer_pool_%.*s_%u",
             (int)t->pool.elem->struct_type.name_len,
             t->pool.elem->struct_type.name,
             t->pool.count);
        break;

    case TYPE_RING:
        emit(e, "struct _zer_ring_%u", t->ring.count);
        break;

    case TYPE_FUNC_PTR:
        emit_type(e, t->func_ptr.ret);
        emit(e, " (*");
        /* name filled by caller */
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
        emit_type(e, t->array.inner);
        emit(e, " %.*s[%u]", (int)name_len, name, t->array.size);
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
        emit(e, "%f", node->float_lit.value);
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

    case NODE_IDENT:
        emit(e, "%.*s", (int)node->ident.name_len, node->ident.name);
        break;

    case NODE_BINARY:
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
        case TOK_LSHIFT:   emit(e, " << "); break;
        case TOK_RSHIFT:   emit(e, " >> "); break;
        default:           emit(e, " ? "); break;
        }
        emit_expr(e, node->binary.right);
        emit(e, ")");
        break;

    case NODE_UNARY:
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
            Type *obj_type = checker_get_type(obj_node);
            if (obj_type && obj_type->kind == TYPE_UNION) {
                /* find variant index */
                const char *vname = node->assign.target->field.field_name;
                uint32_t vlen = (uint32_t)node->assign.target->field.field_name_len;
                for (uint32_t i = 0; i < obj_type->union_type.variant_count; i++) {
                    SUVariant *v = &obj_type->union_type.variants[i];
                    if (v->name_len == vlen && memcmp(v->name, vname, vlen) == 0) {
                        /* emit: obj._tag = TAG_variant, obj.variant = val */
                        emit(e, "(");
                        emit_expr(e, obj_node);
                        emit(e, "._tag = %u, ", i);
                        emit_expr(e, node->assign.target);
                        emit(e, " = ");
                        emit_expr(e, node->assign.value);
                        emit(e, ")");
                        goto assign_done;
                    }
                }
            }
        }
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
        case TOK_LSHIFTEQ:  emit(e, " <<= "); break;
        case TOK_RSHIFTEQ:  emit(e, " >>= "); break;
        default:            emit(e, " = "); break;
        }
        emit_expr(e, node->assign.value);
        assign_done:
        break;

    case NODE_CALL: {
        /* intercept builtin method calls: pool.alloc(), pool.get(h), etc. */
        bool handled = false;
        if (node->call.callee->kind == NODE_FIELD) {
            Node *obj_node = node->call.callee->field.object;
            const char *mname = node->call.callee->field.field_name;
            uint32_t mlen = (uint32_t)node->call.callee->field.field_name_len;

            /* check if object is a Pool variable by looking up its type */
            if (obj_node->kind == NODE_IDENT) {
                Symbol *sym = scope_lookup(e->checker->global_scope,
                    obj_node->ident.name, (uint32_t)obj_node->ident.name_len);
                if (sym && sym->type && sym->type->kind == TYPE_POOL) {
                    Type *pool = sym->type;
                    const char *pname = obj_node->ident.name;
                    int plen = (int)obj_node->ident.name_len;

                    if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                        /* pool.alloc() → _zer_pool_alloc(...) wrapped in optional */
                        int tmp = e->temp_count++;
                        emit(e, "({uint8_t _zer_aok%d = 0; uint32_t _zer_ah%d = "
                             "_zer_pool_alloc(%.*s.slots, sizeof(%.*s.slots[0]), "
                             "%.*s.gen, %.*s.used, %u, &_zer_aok%d); "
                             "(_zer_opt_u32){_zer_ah%d, _zer_aok%d}; })",
                             tmp, tmp,
                             plen, pname, plen, pname,
                             plen, pname, plen, pname,
                             pool->pool.count, tmp, tmp, tmp);
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
                        emit(e, ", %u))", pool->pool.count);
                        handled = true;
                    } else if (mlen == 4 && memcmp(mname, "free", 4) == 0) {
                        /* pool.free(h) */
                        emit(e, "_zer_pool_free(%.*s.gen, %.*s.used, ",
                             plen, pname, plen, pname);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, ", %u)", pool->pool.count);
                        handled = true;
                    }
                }

                if (!handled && sym && sym->type && sym->type->kind == TYPE_RING) {
                    const char *rname = obj_node->ident.name;
                    int rlen = (int)obj_node->ident.name_len;

                    if (mlen == 4 && memcmp(mname, "push", 4) == 0) {
                        /* ring.push(val) */
                        int tmp = e->temp_count++;
                        emit(e, "({__auto_type _zer_rpv%d = ", tmp);
                        if (node->call.arg_count > 0)
                            emit_expr(e, node->call.args[0]);
                        emit(e, "; _zer_ring_push(%.*s.data, &%.*s.head, "
                             "&%.*s.count, %u, &_zer_rpv%d, sizeof(_zer_rpv%d)); })",
                             rlen, rname, rlen, rname, rlen, rname,
                             sym->type->ring.count, tmp, tmp);
                        handled = true;
                    } else if (mlen == 3 && memcmp(mname, "pop", 3) == 0) {
                        /* ring.pop() → optional of elem type */
                        int tmp = e->temp_count++;
                        Type *opt_type = type_optional(e->arena, sym->type->ring.elem);
                        emit(e, "({");
                        emit_type(e, opt_type);
                        emit(e, " _zer_rp%d = {0}; "
                             "if (%.*s.count > 0) { "
                             "_zer_rp%d.value = %.*s.data[%.*s.tail]; "
                             "_zer_rp%d.has_value = 1; "
                             "%.*s.tail = (%.*s.tail + 1) %% %u; "
                             "%.*s.count--; } "
                             "_zer_rp%d; })",
                             tmp,
                             rlen, rname,
                             tmp, rlen, rname, rlen, rname,
                             tmp,
                             rlen, rname, rlen, rname, sym->type->ring.count,
                             rlen, rname,
                             tmp);
                        handled = true;
                    }
                }
            }
        }

        if (!handled) {
            /* normal function call */
            emit_expr(e, node->call.callee);
            emit(e, "(");
            for (int i = 0; i < node->call.arg_count; i++) {
                if (i > 0) emit(e, ", ");
                emit_expr(e, node->call.args[i]);
            }
            emit(e, ")");
        }
        break;
    }

    case NODE_FIELD: {
        /* check if object is a pointer → use -> instead of . */
        Type *obj_type = checker_get_type(node->field.object);
        emit_expr(e, node->field.object);
        if (obj_type && obj_type->kind == TYPE_POINTER) {
            emit(e, "->%.*s", (int)node->field.field_name_len, node->field.field_name);
        } else {
            emit(e, ".%.*s", (int)node->field.field_name_len, node->field.field_name);
        }
        break;
    }

    case NODE_INDEX:
        emit_expr(e, node->index_expr.object);
        emit(e, "[");
        emit_expr(e, node->index_expr.index);
        emit(e, "]");
        break;

    case NODE_SLICE: {
        /* Bit extraction: reg[high..low] on integer → (reg >> low) & mask
         * Array slicing: buf[start..end] → slice struct */
        Type *obj_type = checker_get_type(node->slice.object);
        if (obj_type && type_is_integer(obj_type) &&
            node->slice.start && node->slice.end) {
            /* bit extraction: expr[high..low] → (expr >> low) & ((1 << (high-low+1)) - 1) */
            emit(e, "((");
            emit_expr(e, node->slice.object);
            emit(e, " >> ");
            emit_expr(e, node->slice.end);
            emit(e, ") & ((1u << (");
            emit_expr(e, node->slice.start);
            emit(e, " - ");
            emit_expr(e, node->slice.end);
            emit(e, " + 1)) - 1))");
            break;
        }

        /* buf[start..end] → (_zer_slice_T){ &buf[start], end - start }
         * buf[start..]   → (_zer_slice_T){ &buf[start], buf_len - start }
         * buf[..end]     → (_zer_slice_T){ &buf[0], end } */
        /* For simplicity, emit raw pointer + compute length inline */
        bool is_u8_slice = obj_type && (
            (obj_type->kind == TYPE_ARRAY && obj_type->array.inner == ty_u8) ||
            (obj_type->kind == TYPE_SLICE && obj_type->slice.inner == ty_u8));

        if (is_u8_slice) {
            emit(e, "((_zer_slice_u8){ ");
        } else {
            emit(e, "((struct { void* ptr; size_t len; }){ ");
        }
        /* ptr = &obj[start] */
        emit(e, "&(");
        emit_expr(e, node->slice.object);
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
            emit(e, "%u - (", obj_type->array.size);
            emit_expr(e, node->slice.start);
            emit(e, ")");
        } else {
            emit(e, "0 /* unknown len */");
        }
        emit(e, " })");
        break;
    }

    case NODE_ORELSE: {
        /* Detect if the orelse expression is a pointer optional (?*T)
         * by checking the type from the checker's type map.
         * ?*T uses null sentinel → simple ternary
         * ?T uses struct → .has_value/.value */
        Type *orelse_type = checker_get_type(node->orelse.expr);
        bool is_ptr_optional = orelse_type &&
            orelse_type->kind == TYPE_OPTIONAL &&
            orelse_type->optional.inner->kind == TYPE_POINTER;

        if (node->orelse.fallback_is_return || node->orelse.fallback_is_break ||
            node->orelse.fallback_is_continue) {
            int tmp = e->temp_count++;
            emit(e, "({__auto_type _zer_tmp%d = ", tmp);
            emit_expr(e, node->orelse.expr);
            if (is_ptr_optional) {
                emit(e, "; _zer_tmp%d; })", tmp);
            } else {
                emit(e, "; _zer_tmp%d.value; })", tmp);
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
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")");
        } else if (nlen == 6 && memcmp(name, "offset", 6) == 0) {
            /* @offset(T, field) → offsetof(struct _zer_T, field)
             * Parser puts T as type_arg if it's a keyword type,
             * or as args[0] if it's a named type (identifier). */
            emit(e, "offsetof(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
                emit(e, ", ");
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
            } else if (node->intrinsic.arg_count >= 2) {
                /* args[0] = type name, args[1] = field name */
                emit(e, "struct _zer_");
                emit_expr(e, node->intrinsic.args[0]);
                emit(e, ", ");
                emit_expr(e, node->intrinsic.args[1]);
            }
            emit(e, ")");
        } else if (nlen == 7 && memcmp(name, "ptrcast", 7) == 0) {
            /* @ptrcast(*T, expr) → (T*)(expr) */
            emit(e, "(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
        } else if (nlen == 7 && memcmp(name, "bitcast", 7) == 0) {
            /* @bitcast(T, val) → memcpy type punning (valid C99+GCC) */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
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
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
        } else if (nlen == 8 && memcmp(name, "saturate", 8) == 0) {
            /* @saturate(T, val) → clamp val to T's min/max range */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                emit(e, "({__auto_type _zer_sat%d = ", tmp);
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
                emit(e, "; ");
                /* clamp: min(max(val, TYPE_MIN), TYPE_MAX) */
                /* for unsigned targets, just clamp to max */
                if (type_is_unsigned(t)) {
                    int w = type_width(t);
                    if (w == 8) emit(e, "_zer_sat%d > 255 ? 255 : (uint8_t)_zer_sat%d", tmp, tmp);
                    else if (w == 16) emit(e, "_zer_sat%d > 65535 ? 65535 : (uint16_t)_zer_sat%d", tmp, tmp);
                    else emit(e, "(");
                    if (w > 16) { emit_type(e, t); emit(e, ")_zer_sat%d", tmp); }
                } else {
                    /* signed: just cast for now — full clamp needs min/max per type */
                    emit(e, "("); emit_type(e, t); emit(e, ")_zer_sat%d", tmp);
                }
                emit(e, "; })");
            } else {
                emit(e, "0");
            }
        } else if (nlen == 8 && memcmp(name, "inttoptr", 8) == 0) {
            /* @inttoptr(*T, addr) → (T*)(uintptr_t)(addr) */
            emit(e, "(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")(uintptr_t)(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
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
        } else if (nlen == 9 && memcmp(name, "container", 9) == 0) {
            /* @container(*T, ptr, field) → (T*)((char*)(ptr) - offsetof(T, field)) */
            emit(e, "((");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")((char*)(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ") - offsetof(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
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
            /* @cstr(buf, slice) → memcpy + null terminate */
            /* emit inline: ({memcpy(buf, slice.ptr, slice.len); buf[slice.len]=0; buf;}) */
            int tmp = e->temp_count++;
            emit(e, "({__auto_type _zer_cs%d = ", tmp);
            if (node->intrinsic.arg_count > 1)
                emit_expr(e, node->intrinsic.args[1]);
            emit(e, "; memcpy(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ", _zer_cs%d.ptr, _zer_cs%d.len); ", tmp, tmp);
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, "[_zer_cs%d.len] = 0; (uint8_t*)", tmp);
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, "; })");
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

/* emit all accumulated defers in reverse order */
static void emit_defers(Emitter *e) {
    for (int i = e->defer_stack.count - 1; i >= 0; i--) {
        emit_stmt(e, e->defer_stack.stmts[i]);
    }
}

static void emit_stmt(Emitter *e, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_BLOCK: {
        /* save defer state for this block scope */
        DeferStack saved_defers = e->defer_stack;
        e->defer_stack.count = 0;

        emit(e, "{\n");
        e->indent++;
        for (int i = 0; i < node->block.stmt_count; i++) {
            emit_stmt(e, node->block.stmts[i]);
        }
        /* emit accumulated defers in reverse at block end */
        emit_defers(e);
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");

        /* restore parent defer state */
        e->defer_stack = saved_defers;
        break;
    }

    case NODE_VAR_DECL: {
        Type *type = resolve_type_for_emit(e, node->var_decl.type);

        /* Special case: u32 y = x orelse return;
         * → { auto _t = x; if (!_t.has_value) return; }
         *   uint32_t y = _t.value; */
        if (node->var_decl.init && node->var_decl.init->kind == NODE_ORELSE &&
            (node->var_decl.init->orelse.fallback_is_return ||
             node->var_decl.init->orelse.fallback_is_break ||
             node->var_decl.init->orelse.fallback_is_continue)) {
            Type *or_expr_type = checker_get_type(node->var_decl.init->orelse.expr);
            bool or_is_ptr = or_expr_type &&
                or_expr_type->kind == TYPE_OPTIONAL &&
                or_expr_type->optional.inner->kind == TYPE_POINTER;

            int tmp = e->temp_count++;
            emit_indent(e);
            emit(e, "__auto_type _zer_or%d = ", tmp);
            emit_expr(e, node->var_decl.init->orelse.expr);
            emit(e, ";\n");
            emit_indent(e);
            if (or_is_ptr) {
                emit(e, "if (!_zer_or%d) ", tmp);
            } else {
                emit(e, "if (!_zer_or%d.has_value) ", tmp);
            }
            if (node->var_decl.init->orelse.fallback_is_return) {
                if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                    emit(e, "return 0;\n");
                } else {
                    emit(e, "return;\n");
                }
            } else if (node->var_decl.init->orelse.fallback_is_break)
                emit(e, "break;\n");
            else
                emit(e, "continue;\n");
            emit_indent(e);
            emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
            if (or_is_ptr) {
                emit(e, " = _zer_or%d;\n", tmp);
            } else {
                emit(e, " = _zer_or%d.value;\n", tmp);
            }
            break;
        }

        /* Normal var decl */
        emit_indent(e);
        if (node->var_decl.is_static) emit(e, "static ");
        emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
        if (node->var_decl.init) {
            /* Optional init: null → {0, 0}, value → {val, 1}
             * But if init is a function call returning ?T, just assign directly */
            if (type && type->kind == TYPE_OPTIONAL &&
                type->optional.inner->kind != TYPE_POINTER) {
                if (node->var_decl.init->kind == NODE_NULL_LIT) {
                    emit(e, " = { 0, 0 }");
                } else if (node->var_decl.init->kind == NODE_CALL ||
                           node->var_decl.init->kind == NODE_ORELSE ||
                           node->var_decl.init->kind == NODE_IDENT) {
                    /* might already return ?T — assign directly */
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
                emit(e, " = ");
                emit_expr(e, node->var_decl.init);
            }
        } else {
            /* ZER auto-zeroes */
            if (type && (type->kind == TYPE_STRUCT || type->kind == TYPE_ARRAY ||
                         type->kind == TYPE_OPTIONAL || type->kind == TYPE_UNION)) {
                emit(e, " = {0}");
            } else {
                emit(e, " = 0");
            }
        }
        emit(e, ";\n");
        break;
    }

    case NODE_IF:
        if (node->if_stmt.capture_name) {
            /* if-unwrap: if (maybe) |val| { ... }
             * |val|  → copy of unwrapped value (immutable)
             * |*val| → pointer to ORIGINAL optional's value (mutable)
             *
             * For |val|:  { auto _tmp = expr; if (_tmp.has_value) { auto val = _tmp.value; ... } }
             * For |*val|: { auto *_ptr = &expr; if (_ptr->has_value) { auto val = &_ptr->value; ... } }
             * ?*T (ptr): { auto _tmp = expr; if (_tmp) { auto val = _tmp; ... } } */
            int tmp = e->temp_count++;
            Type *cond_type = checker_get_type(node->if_stmt.cond);
            bool is_ptr_opt = cond_type &&
                cond_type->kind == TYPE_OPTIONAL &&
                cond_type->optional.inner->kind == TYPE_POINTER;

            emit_indent(e);
            emit(e, "{\n");
            e->indent++;

            if (node->if_stmt.capture_is_ptr && !is_ptr_opt) {
                /* |*val| on struct optional — need pointer to original */
                emit_indent(e);
                emit_type(e, cond_type);
                emit(e, " *_zer_uwp%d = &(", tmp);
                emit_expr(e, node->if_stmt.cond);
                emit(e, ");\n");
                emit_indent(e);
                emit(e, "if (_zer_uwp%d->has_value) ", tmp);
                emit(e, "{\n");
                e->indent++;
                emit_indent(e);
                emit(e, "__auto_type %.*s = &_zer_uwp%d->value;\n",
                     (int)node->if_stmt.capture_name_len,
                     node->if_stmt.capture_name, tmp);
            } else {
                /* |val| or ?*T — use copy */
                emit_indent(e);
                emit(e, "__auto_type _zer_uw%d = ", tmp);
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
                    emit(e, "__auto_type %.*s = _zer_uw%d;\n",
                         (int)node->if_stmt.capture_name_len,
                         node->if_stmt.capture_name, tmp);
                } else {
                    emit(e, "__auto_type %.*s = _zer_uw%d.value;\n",
                         (int)node->if_stmt.capture_name_len,
                         node->if_stmt.capture_name, tmp);
                }
            }
            /* emit then body contents (unwrap the block) */
            if (node->if_stmt.then_body->kind == NODE_BLOCK) {
                for (int i = 0; i < node->if_stmt.then_body->block.stmt_count; i++) {
                    emit_stmt(e, node->if_stmt.then_body->block.stmts[i]);
                }
            } else {
                emit_stmt(e, node->if_stmt.then_body);
            }
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
            emit_indent(e);
            emit(e, "if (");
            emit_expr(e, node->if_stmt.cond);
            emit(e, ") ");
            emit_stmt(e, node->if_stmt.then_body);
            if (node->if_stmt.else_body) {
                emit_indent(e);
                emit(e, "else ");
                emit_stmt(e, node->if_stmt.else_body);
            }
        }
        break;

    case NODE_FOR:
        emit_indent(e);
        emit(e, "for (");
        if (node->for_stmt.init) {
            if (node->for_stmt.init->kind == NODE_VAR_DECL) {
                Type *type = resolve_type_for_emit(e, node->for_stmt.init->var_decl.type);
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
        break;

    case NODE_WHILE:
        emit_indent(e);
        emit(e, "while (");
        emit_expr(e, node->while_stmt.cond);
        emit(e, ") ");
        emit_stmt(e, node->while_stmt.body);
        break;

    case NODE_RETURN:
        /* emit defers before return (reverse order) */
        emit_defers(e);
        emit_indent(e);
        if (node->ret.expr) {
            /* return null from ?T function → return {0, 0} */
            if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
                e->current_func_ret->optional.inner->kind != TYPE_POINTER &&
                node->ret.expr->kind == NODE_NULL_LIT) {
                emit(e, "return (");
                emit_type(e, e->current_func_ret);
                emit(e, "){ 0, 0 };\n");
            }
            /* return value from ?T function → return {value, 1} */
            else if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL &&
                     e->current_func_ret->optional.inner->kind != TYPE_POINTER &&
                     node->ret.expr->kind != NODE_NULL_LIT) {
                emit(e, "return (");
                emit_type(e, e->current_func_ret);
                emit(e, "){ ");
                emit_expr(e, node->ret.expr);
                emit(e, ", 1 };\n");
            } else {
                emit(e, "return ");
                emit_expr(e, node->ret.expr);
                emit(e, ";\n");
            }
        } else {
            /* bare return — for ?void, return {1} (success) */
            if (e->current_func_ret && e->current_func_ret->kind == TYPE_OPTIONAL) {
                emit(e, "return (");
                emit_type(e, e->current_func_ret);
                emit(e, "){ 0, 1 };\n");
            } else {
                emit(e, "return;\n");
            }
        }
        break;

    case NODE_BREAK:
        emit_indent(e);
        emit(e, "break;\n");
        break;

    case NODE_CONTINUE:
        emit_indent(e);
        emit(e, "continue;\n");
        break;

    case NODE_EXPR_STMT:
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
        if (e->defer_stack.count < MAX_DEFERS) {
            e->defer_stack.stmts[e->defer_stack.count++] = node->defer.body;
        }
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
        Type *sw_type = checker_get_type(node->switch_stmt.expr);
        bool is_union_switch = sw_type && sw_type->kind == TYPE_UNION;

        emit_indent(e);
        emit(e, "{\n");
        e->indent++;
        emit_indent(e);
        emit(e, "__auto_type _zer_sw%d = ", sw_tmp);
        emit_expr(e, node->switch_stmt.expr);
        emit(e, ";\n");

        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            SwitchArm *arm = &node->switch_stmt.arms[i];
            emit_indent(e);

            if (arm->is_default) {
                if (i > 0) emit(e, "else ");
                emit(e, "/* default */ ");
            } else if (is_union_switch && arm->is_enum_dot) {
                /* union switch: check _tag field */
                if (i > 0) emit(e, "else ");
                emit(e, "if (");
                for (int j = 0; j < arm->value_count; j++) {
                    if (j > 0) emit(e, " || ");
                    emit(e, "_zer_sw%d._tag == _ZER_%.*s_TAG_%.*s",
                         sw_tmp,
                         (int)sw_type->union_type.name_len, sw_type->union_type.name,
                         (int)arm->values[j]->ident.name_len, arm->values[j]->ident.name);
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

            /* arm body — handle captures for union switch */
            if (is_union_switch && arm->capture_name && arm->value_count > 0) {
                emit(e, "{\n");
                e->indent++;
                emit_indent(e);
                /* extract variant from anonymous union */
                emit(e, "__auto_type %.*s = _zer_sw%d.%.*s;\n",
                     (int)arm->capture_name_len, arm->capture_name,
                     sw_tmp,
                     (int)arm->values[0]->ident.name_len, arm->values[0]->ident.name);
                /* emit body contents */
                if (arm->body->kind == NODE_BLOCK) {
                    for (int k = 0; k < arm->body->block.stmt_count; k++)
                        emit_stmt(e, arm->body->block.stmts[k]);
                } else {
                    emit_stmt(e, arm->body);
                }
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
        Type *inner = resolve_type_for_emit(e, tn->pointer.inner);
        return type_pointer(e->arena, inner);
    }
    case TYNODE_OPTIONAL: {
        Type *inner = resolve_type_for_emit(e, tn->optional.inner);
        return type_optional(e->arena, inner);
    }
    case TYNODE_ARRAY: {
        Type *elem = resolve_type_for_emit(e, tn->array.elem);
        uint32_t size = 0;
        if (tn->array.size_expr && tn->array.size_expr->kind == NODE_INT_LIT)
            size = (uint32_t)tn->array.size_expr->int_lit.value;
        return type_array(e->arena, elem, size);
    }
    case TYNODE_SLICE: {
        Type *inner = resolve_type_for_emit(e, tn->slice.inner);
        return type_slice(e->arena, inner);
    }
    case TYNODE_HANDLE:
        return type_handle(e->arena, resolve_type_for_emit(e, tn->handle.elem));
    case TYNODE_NAMED: {
        /* look up in checker's global scope */
        Symbol *sym = scope_lookup(e->checker->global_scope,
            tn->named.name, (uint32_t)tn->named.name_len);
        if (sym) return sym->type;
        return ty_void;
    }
    case TYNODE_CONST:
    case TYNODE_VOLATILE:
        return resolve_type_for_emit(e, tn->qualified.inner);
    case TYNODE_ARENA:
        return ty_arena;
    case TYNODE_OPAQUE:
        return ty_opaque;
    case TYNODE_POOL: {
        Type *elem = resolve_type_for_emit(e, tn->pool.elem);
        uint32_t count = 0;
        if (tn->pool.count_expr && tn->pool.count_expr->kind == NODE_INT_LIT)
            count = (uint32_t)tn->pool.count_expr->int_lit.value;
        return type_pool(e->arena, elem, count);
    }
    case TYNODE_RING: {
        Type *elem = resolve_type_for_emit(e, tn->ring.elem);
        uint32_t count = 0;
        if (tn->ring.count_expr && tn->ring.count_expr->kind == NODE_INT_LIT)
            count = (uint32_t)tn->ring.count_expr->int_lit.value;
        return type_ring(e->arena, elem, count);
    }
    case TYNODE_FUNC_PTR: {
        Type *ret = resolve_type_for_emit(e, tn->func_ptr.return_type);
        uint32_t pc = (uint32_t)tn->func_ptr.param_count;
        Type **params = NULL;
        if (pc > 0) {
            params = (Type **)arena_alloc(e->arena, pc * sizeof(Type *));
            for (uint32_t i = 0; i < pc; i++)
                params[i] = resolve_type_for_emit(e, tn->func_ptr.param_types[i]);
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
    if (node->struct_decl.is_packed) {
        emit(e, "struct __attribute__((packed)) _zer_%.*s {\n",
             (int)node->struct_decl.name_len, node->struct_decl.name);
    } else {
        emit(e, "struct _zer_%.*s {\n",
             (int)node->struct_decl.name_len, node->struct_decl.name);
    }
    e->indent++;
    for (int i = 0; i < node->struct_decl.field_count; i++) {
        FieldDecl *f = &node->struct_decl.fields[i];
        Type *ftype = resolve_type_for_emit(e, f->type);
        emit_indent(e);
        emit_type_and_name(e, ftype, f->name, f->name_len);
        emit(e, ";\n");
    }
    e->indent--;
    emit(e, "};\n\n");
}

static void emit_func_decl(Emitter *e, Node *node) {
    Type *ret = resolve_type_for_emit(e, node->func_decl.return_type);

    /* static functions */
    if (node->func_decl.is_static) emit(e, "static ");

    emit_type(e, ret);
    emit(e, " %.*s(", (int)node->func_decl.name_len, node->func_decl.name);

    if (node->func_decl.param_count == 0) {
        emit(e, "void");
    } else {
        for (int i = 0; i < node->func_decl.param_count; i++) {
            if (i > 0) emit(e, ", ");
            ParamDecl *p = &node->func_decl.params[i];
            Type *ptype = resolve_type_for_emit(e, p->type);
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
    Type *type = resolve_type_for_emit(e, node->var_decl.type);

    /* Pool(T, N) → use macro for struct layout */
    if (type && type->kind == TYPE_POOL) {
        emit(e, "struct { ");
        emit_type(e, type->pool.elem);
        emit(e, " slots[%u]; uint16_t gen[%u]; uint8_t used[%u]; } ",
             type->pool.count, type->pool.count, type->pool.count);
        emit(e, "%.*s = {0};\n\n",
             (int)node->var_decl.name_len, node->var_decl.name);
        return;
    }

    /* Ring(T, N) → ring struct */
    if (type && type->kind == TYPE_RING) {
        emit(e, "struct { ");
        emit_type(e, type->ring.elem);
        emit(e, " data[%u]; uint32_t head; uint32_t tail; uint32_t count; } ",
             type->ring.count);
        emit(e, "%.*s = {0};\n\n",
             (int)node->var_decl.name_len, node->var_decl.name);
        return;
    }

    if (node->var_decl.is_static) emit(e, "static ");

    emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);

    if (node->var_decl.init) {
        emit(e, " = ");
        emit_expr(e, node->var_decl.init);
    } else {
        /* auto-zero */
        if (type && (type->kind == TYPE_STRUCT || type->kind == TYPE_ARRAY ||
                     type->kind == TYPE_OPTIONAL || type->kind == TYPE_UNION)) {
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

void emit_file(Emitter *e, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;

    /* C preamble */
    emit(e, "/* Generated by ZER compiler — do not edit */\n");
    emit(e, "#include <stdint.h>\n");
    emit(e, "#include <stddef.h>\n");
    emit(e, "#include <string.h>\n");
    emit(e, "\n");

    /* ZER optional type definitions */
    emit(e, "/* ZER optional types */\n");
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

    /* ZER slice type */
    emit(e, "/* ZER slice types */\n");
    emit(e, "typedef struct { uint8_t* ptr; size_t len; } _zer_slice_u8;\n");
    emit(e, "typedef struct { uint32_t* ptr; size_t len; } _zer_slice_u32;\n");
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
            "uint16_t *gen, uint8_t *used, uint32_t capacity, uint8_t *ok) {\n");
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

    emit(e, "static inline void *_zer_pool_get(void *slots, uint16_t *gen, uint8_t *used, "
            "size_t slot_size, uint32_t handle, uint32_t capacity) {\n");
    emit(e, "    uint32_t idx = handle & 0xFFFF;\n");
    emit(e, "    uint16_t h_gen = (uint16_t)(handle >> 16);\n");
    emit(e, "    if (idx >= capacity || !used[idx] || gen[idx] != h_gen) {\n");
    emit(e, "        /* generation mismatch or invalid — trap */\n");
    emit(e, "        return (void*)0; /* TODO: trap */\n");
    emit(e, "    }\n");
    emit(e, "    return (char*)slots + idx * slot_size;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void _zer_pool_free(uint16_t *gen, uint8_t *used, "
            "uint32_t handle, uint32_t capacity) {\n");
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

    emit(e, "static inline void _zer_ring_push(void *ring_data, uint32_t *head, "
            "uint32_t *count, uint32_t capacity, const void *val, size_t elem_size) {\n");
    emit(e, "    memcpy((char*)ring_data + (*head) * elem_size, val, elem_size);\n");
    emit(e, "    *head = (*head + 1) %% capacity;\n");
    emit(e, "    if (*count < capacity) (*count)++;\n");
    emit(e, "}\n\n");

    emit(e, "\n");

    /* emit declarations */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];

        switch (decl->kind) {
        case NODE_STRUCT_DECL:
            emit_struct_decl(e, decl);
            break;

        case NODE_FUNC_DECL:
            emit_func_decl(e, decl);
            break;

        case NODE_GLOBAL_VAR:
            emit_global_var(e, decl);
            break;

        case NODE_UNION_DECL: {
            /* tagged union → struct with tag + anonymous union */
            emit(e, "/* tagged union %.*s */\n",
                 (int)decl->union_decl.name_len, decl->union_decl.name);
            /* tag constants */
            for (int j = 0; j < decl->union_decl.variant_count; j++) {
                UnionVariant *v = &decl->union_decl.variants[j];
                emit(e, "#define _ZER_%.*s_TAG_%.*s %d\n",
                     (int)decl->union_decl.name_len, decl->union_decl.name,
                     (int)v->name_len, v->name, j);
            }
            emit(e, "struct _zer_union_%.*s {\n",
                 (int)decl->union_decl.name_len, decl->union_decl.name);
            emit(e, "    int32_t _tag;\n");
            emit(e, "    union {\n");
            for (int j = 0; j < decl->union_decl.variant_count; j++) {
                UnionVariant *v = &decl->union_decl.variants[j];
                Type *vtype = resolve_type_for_emit(e, v->type);
                emit(e, "        ");
                emit_type(e, vtype);
                emit(e, " %.*s;\n", (int)v->name_len, v->name);
            }
            emit(e, "    };\n");
            emit(e, "};\n\n");
            break;
        }

        case NODE_ENUM_DECL:
            /* emit as #define constants */
            emit(e, "/* enum %.*s */\n",
                 (int)decl->enum_decl.name_len, decl->enum_decl.name);
            for (int j = 0; j < decl->enum_decl.variant_count; j++) {
                EnumVariant *v = &decl->enum_decl.variants[j];
                emit(e, "#define _ZER_%.*s_%.*s %d\n",
                     (int)decl->enum_decl.name_len, decl->enum_decl.name,
                     (int)v->name_len, v->name, j);
            }
            emit(e, "\n");
            break;

        case NODE_IMPORT:
            emit(e, "/* import %.*s — TODO */\n\n",
                 (int)decl->import.module_name_len, decl->import.module_name);
            break;

        case NODE_INTERRUPT:
            emit(e, "void __attribute__((interrupt)) %.*s_IRQHandler(void) ",
                 (int)decl->interrupt.name_len, decl->interrupt.name);
            if (decl->interrupt.body) {
                emit_stmt(e, decl->interrupt.body);
            }
            emit(e, "\n");
            break;

        case NODE_TYPEDEF:
            /* emit typedef in C */
            {
                Type *underlying = resolve_type_for_emit(e, decl->typedef_decl.type);
                emit(e, "typedef ");
                emit_type(e, underlying);
                emit(e, " %.*s;\n\n",
                     (int)decl->typedef_decl.name_len, decl->typedef_decl.name);
            }
            break;

        default:
            emit(e, "/* unhandled decl %s */\n\n", node_kind_name(decl->kind));
            break;
        }
    }
}
