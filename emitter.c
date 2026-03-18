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
        /* ?T → struct { T value; uint8_t has_value; } */
        emit(e, "struct { ");
        emit_type(e, t->optional.inner);
        emit(e, " value; uint8_t has_value; }");
        break;

    case TYPE_SLICE:
        emit(e, "struct { ");
        emit_type(e, t->slice.inner);
        emit(e, "* ptr; size_t len; }");
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
        /* emit as compound literal slice: (struct{uint8_t*ptr;size_t len;}){...} */
        /* for now, emit as C string literal — used in contexts expecting []u8 */
        emit(e, "((struct { uint8_t* ptr; size_t len; }){ (uint8_t*)\"%.*s\", %zu })",
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
        break;

    case NODE_CALL:
        emit_expr(e, node->call.callee);
        emit(e, "(");
        for (int i = 0; i < node->call.arg_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_expr(e, node->call.args[i]);
        }
        emit(e, ")");
        break;

    case NODE_FIELD:
        emit_expr(e, node->field.object);
        emit(e, ".%.*s", (int)node->field.field_name_len, node->field.field_name);
        break;

    case NODE_INDEX:
        emit_expr(e, node->index_expr.object);
        emit(e, "[");
        emit_expr(e, node->index_expr.index);
        emit(e, "]");
        break;

    case NODE_SLICE: {
        /* buf[start..end] → (struct{T*ptr;size_t len;}){obj.ptr+start, end-start}
         * For arrays: (struct{T*ptr;size_t len;}){&arr[start], end-start} */
        emit(e, "/* slice */ 0 /* TODO: slice codegen */");
        break;
    }

    case NODE_ORELSE: {
        /* ?T expr orelse fallback
         *
         * For ?*T (pointer optional — null sentinel):
         *   expr ? expr : fallback
         *
         * For ?T (value optional — struct with has_value):
         *   ({__typeof__(expr) _tmp = expr; _tmp.has_value ? _tmp.value : fallback;})
         *
         * For orelse return/break/continue:
         *   handled in statement context, not expression
         */
        int tmp = e->temp_count++;
        emit(e, "({__auto_type _zer_tmp%d = ", tmp);
        emit_expr(e, node->orelse.expr);
        emit(e, "; _zer_tmp%d.has_value ? _zer_tmp%d.value : ", tmp, tmp);
        if (node->orelse.fallback) {
            emit_expr(e, node->orelse.fallback);
        } else {
            emit(e, "0");
        }
        emit(e, "; })");
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
            /* @offset(T, field) → offsetof(struct _zer_T, field) */
            emit(e, "offsetof(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ", ");
            if (node->intrinsic.arg_count > 0) {
                emit_expr(e, node->intrinsic.args[0]);
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
            /* @bitcast(T, val) → union cast */
            int tmp = e->temp_count++;
            emit(e, "({union{__auto_type _in; ");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, " _out;} _zer_bc%d; _zer_bc%d._in = ", tmp, tmp);
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, "; _zer_bc%d._out;})", tmp);
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
            /* @saturate(val) → clamp to target type range — simplified as cast for now */
            emit(e, "(");
            if (node->intrinsic.type_arg) {
                Type *t = resolve_type_for_emit(e, node->intrinsic.type_arg);
                emit_type(e, t);
            }
            emit(e, ")(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
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
        } else if (nlen == 4 && memcmp(name, "cstr", 4) == 0) {
            /* @cstr(buf, slice) — copy slice to buf + null terminate */
            /* simplified: emit memcpy + null terminator */
            emit(e, "/* @cstr */0");
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

static void emit_stmt(Emitter *e, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_BLOCK:
        emit(e, "{\n");
        e->indent++;
        for (int i = 0; i < node->block.stmt_count; i++) {
            emit_stmt(e, node->block.stmts[i]);
        }
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        break;

    case NODE_VAR_DECL: {
        Type *type = resolve_type_for_emit(e, node->var_decl.type);
        emit_indent(e);
        emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);
        if (node->var_decl.init) {
            emit(e, " = ");
            emit_expr(e, node->var_decl.init);
        } else {
            /* ZER auto-zeroes: emit = {0} for structs/arrays, = 0 for scalars */
            if (type && (type->kind == TYPE_STRUCT || type->kind == TYPE_ARRAY)) {
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
             * → { auto _tmp = maybe; if (_tmp.has_value) { T val = _tmp.value; ... } }
             * For ?*T: if (_tmp != NULL) { *T val = _tmp; ... } */
            int tmp = e->temp_count++;
            emit_indent(e);
            emit(e, "{\n");
            e->indent++;
            emit_indent(e);
            emit(e, "__auto_type _zer_uw%d = ", tmp);
            emit_expr(e, node->if_stmt.cond);
            emit(e, ";\n");
            emit_indent(e);
            emit(e, "if (_zer_uw%d.has_value) ", tmp);
            /* inject capture variable into the then body */
            emit(e, "{\n");
            e->indent++;
            emit_indent(e);
            emit(e, "__auto_type %.*s = _zer_uw%d.value;\n",
                 (int)node->if_stmt.capture_name_len,
                 node->if_stmt.capture_name, tmp);
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
        emit_indent(e);
        if (node->ret.expr) {
            emit(e, "return ");
            emit_expr(e, node->ret.expr);
            emit(e, ";\n");
        } else {
            emit(e, "return;\n");
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
        /* Defer: emit the deferred code at the end of the current block.
         * For now, emit as a comment + the code inline.
         * Full defer requires goto-based cleanup which we'll add per-function. */
        emit_indent(e);
        emit(e, "/* deferred: */ ");
        /* We can't easily defer in C99 without goto. For now, just emit inline
         * with a comment. Real defer codegen needs function-level rewriting. */
        emit_stmt(e, node->defer.body);
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

            /* arm body */
            if (arm->body->kind == NODE_BLOCK) {
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
    case TYNODE_FUNC_PTR:
        /* TODO: function pointer type emission */
        return ty_void;
    default:
        return ty_void;
    }
}

/* ================================================================
 * TOP-LEVEL DECLARATION EMISSION
 * ================================================================ */

static void emit_struct_decl(Emitter *e, Node *node) {
    emit(e, "struct _zer_%.*s {\n",
         (int)node->struct_decl.name_len, node->struct_decl.name);
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
        emit_stmt(e, node->func_decl.body);
    } else {
        emit(e, ";\n");
    }
    emit(e, "\n");
}

static void emit_global_var(Emitter *e, Node *node) {
    Type *type = resolve_type_for_emit(e, node->var_decl.type);

    if (node->var_decl.is_static) emit(e, "static ");

    emit_type_and_name(e, type, node->var_decl.name, node->var_decl.name_len);

    if (node->var_decl.init) {
        emit(e, " = ");
        emit_expr(e, node->var_decl.init);
    } else {
        /* auto-zero */
        if (type && (type->kind == TYPE_STRUCT || type->kind == TYPE_ARRAY)) {
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
