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

/* BUG-991: render a `double` as a VALID C floating constant.
 *
 * `%.17g` is round-trip exact for every FINITE double, and was used verbatim at
 * FIVE emission sites. It is wrong for the three non-finite values: glibc prints
 * `inf` / `-inf` / `nan`, none of which is a C token. So a ZER program containing
 * a float literal that overflows the double range —
 *
 *     f64 x = 1e400;          // strtod -> +inf
 *
 * emitted `_zer_t0 = inf;`, and the user got GCC's *"'inf' undeclared"* pointing
 * at their own `.zer` line via `#line`. No ZER diagnostic ever names the real
 * problem, and the failure is attributed to the C layer the user never wrote.
 *
 * `__builtin_inf()` / `__builtin_nan("")` are GCC constants: usable in a STATIC
 * INITIALIZER (so the global-scope site works), available under `-ffreestanding`
 * (so bare-metal works), and exact. ZER emits GCC-only C already (statement
 * expressions, `__auto_type`, `__attribute__`), so this adds no new dependency.
 *
 * ONE helper, not five spellings. A new float-emitting site must call this, never
 * `%.17g`; `tools/audit_float_literal.sh` fails the build on a raw one. */
/* BUG-1318: `%.17g` prints an INTEGRAL double without a decimal point — `2.0` as
 * `2`, `-0.0` as `-0` — and C reads that token as an INT. The var-decl form hid it
 * (the value lands in a typed temp); every expression position computed in int:
 * `a = 1.0 / 2.0;` was `a = 1 / 2` = 0.0, `100000.0 * 100000.0` an int multiply,
 * `@bitcast(u64, 2.0)` copied 8 bytes out of a 4-byte int (ASan), and `-0.0` lost
 * its sign. An integral value now carries `.0`. `f32` = the literal's checked type
 * is f32: it is printed as the FLOAT value with an `f` suffix, so an expression
 * such as `c = a * 0.1` computes in f32 like the var-decl form does (the typed
 * temp converts the double literal to float — the same value printed here). */
static void emit_double_lit(Emitter *e, double v, bool f32) {
    /* NaN first: every comparison against NaN is false, so an `isnan`-last
     * ordering would fall through to `%.17g` and print `nan`. Same reason the
     * float->int saturation guard tests NaN first (BUG-883). */
    if (v != v) { emit(e, f32 ? "__builtin_nanf(\"\")" : "__builtin_nan(\"\")"); return; }
    if (f32) {
        float fv = (float)v;
        if (fv != fv || fv > 3.40282347e38f || fv < -3.40282347e38f) {
            emit(e, fv < 0 ? "(-__builtin_inff())" : "__builtin_inff()");
            return;
        }
        emit(e, "%.9g", (double)fv);
        if (fv < 1e9f && fv > -1e9f && fv == (float)(long long)fv) emit(e, ".0");
        emit(e, "f");
        return;
    }
    if (v > 1.7976931348623157e308) { emit(e, "__builtin_inf()"); return; }
    if (v < -1.7976931348623157e308) { emit(e, "(-__builtin_inf())"); return; }
    emit(e, "%.17g", v);
    /* %.17g uses an exponent (a floating token) from 1e17 up */
    if (v < 1e17 && v > -1e17 && v == (double)(long long)v) emit(e, ".0");
}
/* The literal's checked type decides its C spelling. */
static bool emit_type_is_f32(Type *t) {
    t = t ? type_unwrap_distinct(t) : NULL;
    return t && type_dispatch_kind(t) == TYPE_F32;
}

/* BUG-1323: the operand of a bit-query intrinsic (@popcount/@ctz/@clz/@parity/
 * @ffs), ZERO-extended to the width it is counted at — 32 for up to 32 bits, else
 * 64 (reference.md: "only TWO widths"). The operand used to reach the builtin
 * as-is: a signed narrow value SIGN-extended (`@popcount(i8 -1)` was 32, not 8;
 * `i5 -1` also 32), and the zero result was the operand's OWN width (`@clz(u8 0)`
 * was 8 while `@clz(u8 1)` was 31 — not even monotonic). */
static int bitq_count_width(int w) { return w > 32 ? 64 : 32; }
static void bitq_operand_open(Emitter *e, int w) {
    emit(e, w > 32 ? "((uint64_t)(" : "((uint32_t)(");
}
static void bitq_operand_close(Emitter *e, int w) {
    if (w <= 0 || w == 32 || w >= 64) { emit(e, "))"); return; }
    emit(e, ") & 0x%llxULL)", (unsigned long long)((1ULL << w) - 1ULL));
}

/* BUG-1321: the body of a ZER string literal as C string-literal text. ZER's
 * escapes are fixed-width — `\xHH` is exactly two hex digits and `\0` is one NUL
 * byte — while C's are greedy: `\x` eats EVERY following hex digit and `\0`
 * starts an OCTAL escape of up to three digits. Copied verbatim, `"\x41b"` (two
 * bytes in ZER) was one out-of-range byte in C and `"\01"` (NUL, '1') was the
 * single byte 0x01 — a wrong `.len` and wrong bytes, silently. A hex escape is
 * closed with `""` (C literal concatenation) when a hex digit follows, and `\0`
 * is spelled `\000` (three octal digits end the escape). Every other character
 * and escape is copied as written. */
static int zer_is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static void emit_c_string_text(Emitter *e, const char *t, int n) {
    for (int i = 0; i < n; i++) {
        if (t[i] != '\\' || i + 1 >= n) { fputc(t[i], e->out); continue; }
        char esc = t[i + 1];
        if (esc == 'x') {
            /* \xHH — the lexer guarantees two hex digits */
            fputc('\\', e->out); fputc('x', e->out);
            if (i + 2 < n) fputc(t[i + 2], e->out);
            if (i + 3 < n) fputc(t[i + 3], e->out);
            i += 3;
            if (i + 1 < n && zer_is_hex(t[i + 1])) fputs("\"\"", e->out);
            continue;
        }
        if (esc == '0') { fputs("\\000", e->out); i += 1; continue; }
        fputc('\\', e->out); fputc(esc, e->out);
        i += 1;
    }
}
static void emit_zer_string_slice(Emitter *e, const char *t, int n, bool paren) {
    emit(e, paren ? "((_zer_slice_u8){ (uint8_t*)\"" : "(_zer_slice_u8){ (uint8_t*)\"");
    emit_c_string_text(e, t, n);
    emit(e, "\", sizeof(\"");
    emit_c_string_text(e, t, n);
    emit(e, paren ? "\") - 1 })" : "\") - 1 }");
}

/* BUG-1320: a character literal is a u8 (BUG-1191). The AST field is a (signed)
 * `char`, so '\xff' read as -1: one emitter printed `(unsigned)-1` = 4294967295
 * (`t['\xff'] = 7` trapped as out of bounds, `c == '\xff'` was false for c = 255)
 * and the other printed the raw byte inside quotes, a negative `int` in C. ONE
 * spelling for both paths: printable ASCII as itself, everything else as its
 * unsigned value. */
static void emit_char_lit(Emitter *e, Node *node) {
    unsigned v = (unsigned)(uint8_t)node->char_lit.value;
    if (v >= 32 && v < 127 && v != '\'' && v != '\\') emit(e, "'%c'", (char)v);
    else emit(e, "%uU", v);
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
    return type_is_null_sentinel(inner);   /* BUG-1054: one definition, types.c */
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

static void emit_array_as_slice(Emitter *e, Node *array_expr, Type *array_type, Type *slice_type);

/* B4: Emit value wrapped in optional struct: (Type){ val, 1 }.
 * Used for T → ?T wrapping at assignment, var-decl init.
 * opt_type is the target optional type (may be distinct). */
static void emit_opt_wrap_value(Emitter *e, Type *opt_type, Node *value_expr) {
    emit(e, "(");
    emit_type(e, opt_type);
    emit(e, "){ ");
    /* #14 (B): an array value into an optional-SLICE (?[*]T) must be coerced to a
     * {ptr,len} slice literal first — a bare array flattens into .value.ptr /
     * .value.len and defaults .has_value to 0 (a present optional built empty). */
    Type *ow_ot = opt_type ? type_unwrap_distinct(opt_type) : NULL;
    Type *ow_inner = (ow_ot && type_dispatch_kind(ow_ot) == TYPE_OPTIONAL)
                     ? ow_ot->optional.inner : NULL;
    Type *ow_vt = value_expr ? checker_get_type(e->checker, value_expr) : NULL;
    if (ow_inner && type_dispatch_kind(ow_inner) == TYPE_SLICE &&
        ow_vt && type_dispatch_kind(ow_vt) == TYPE_ARRAY)
        emit_array_as_slice(e, value_expr, type_unwrap_distinct(ow_vt),
                            type_unwrap_distinct(ow_inner));
    else
        emit_expr(e, value_expr);
    emit(e, ", 1 }");
}

/* F21 (audit 2026-07-06): if struct field `fname` of `si_type` is a value-
 * optional (?u32/?bool/… — a struct optional, NOT a ?*T null-sentinel) and the
 * initializer value's type `val_type` is NOT already optional, return the field's
 * optional Type so the caller wraps the scalar as {val,1}. Otherwise NULL. Without
 * the wrap, C brace-elision drops the scalar into `.value`, leaving `.has_value=0`
 * (so `Cfg c = { .baud = 9600 }; c.baud orelse d` always took the fallback). */
static Type *struct_init_opt_wrap_type(Type *si_type, const char *fname,
                                       uint32_t fname_len, Type *val_type) {
    Type *si_eff = si_type ? type_unwrap_distinct(si_type) : NULL;
    if (!si_eff || si_eff->kind != TYPE_STRUCT) return NULL;
    Type *ftype = NULL;
    for (uint32_t j = 0; j < si_eff->struct_type.field_count; j++) {
        if (si_eff->struct_type.fields[j].name_len == fname_len &&
            memcmp(si_eff->struct_type.fields[j].name, fname, fname_len) == 0) {
            ftype = si_eff->struct_type.fields[j].type;
            break;
        }
    }
    if (!ftype) return NULL;
    Type *ft_eff = type_unwrap_distinct(ftype);
    if (!ft_eff || ft_eff->kind != TYPE_OPTIONAL) return NULL;
    if (is_null_sentinel(ft_eff->optional.inner)) return NULL;
    if (val_type && type_dispatch_kind(val_type) == TYPE_OPTIONAL) return NULL;
    return ftype;
}

/* #14/#15: look up a struct field's declared Type by name (NULL if not found /
 * not a struct). Shared by the struct-init array→slice coercion. */
static Type *struct_field_type_by_name(Type *si_type, const char *fname,
                                       uint32_t fname_len) {
    Type *si_eff = si_type ? type_unwrap_distinct(si_type) : NULL;
    if (!si_eff || type_dispatch_kind(si_eff) != TYPE_STRUCT) return NULL;
    for (uint32_t j = 0; j < si_eff->struct_type.field_count; j++) {
        if (si_eff->struct_type.fields[j].name_len == fname_len &&
            memcmp(si_eff->struct_type.fields[j].name, fname, fname_len) == 0)
            return si_eff->struct_type.fields[j].type;
    }
    return NULL;
}

/* #14/#15: if `field_type` is a slice (or an optional-of-slice) and `val_type` is
 * a fixed array, return the effective SLICE type to coerce to; else NULL. The
 * bare-array-into-slice-slot bug: an aggregate initializer emitted the raw array
 * identifier, which C brace-flattens into `.ptr`/`.len`. */
static Type *aggregate_slice_coerce_target(Type *field_type, Type *val_type) {
    Type *ft = field_type ? type_unwrap_distinct(field_type) : NULL;
    Type *vt = val_type ? type_unwrap_distinct(val_type) : NULL;
    if (!vt || type_dispatch_kind(vt) != TYPE_ARRAY) return NULL;
    if (!ft) return NULL;
    if (type_dispatch_kind(ft) == TYPE_SLICE) return ft;
    if (type_dispatch_kind(ft) == TYPE_OPTIONAL && ft->optional.inner) {
        Type *inner = type_unwrap_distinct(ft->optional.inner);
        if (inner && type_dispatch_kind(inner) == TYPE_SLICE) return inner;
    }
    return NULL;
}

/* Emit return-null for current function's return type.
 * Handles ?void, ?T struct, ?*T null sentinel, void, and scalar. */
static void emit_return_null(Emitter *e) {
    Type *ret = e->current_func_ret;
    if (!ret || ret->kind == TYPE_VOID) {
        /* BUG fix: for void main() auto-promoted to int main(), emit
         * `return 0;` so the promoted signature is consistent. */
        if (e->current_main_promoted) {
            emit(e, "return 0; ");
        } else {
            emit(e, "return; ");
        }
        return;
    }
    Type *eff = type_unwrap_distinct(ret);
    if (eff->kind == TYPE_OPTIONAL && !is_null_sentinel(eff->optional.inner)) {
        emit(e, "return ");
        emit_opt_null_literal(e, ret);
        emit(e, "; ");
    } else if (type_dispatch_kind(eff) == TYPE_SLICE ||
               type_dispatch_kind(eff) == TYPE_STRUCT ||
               type_dispatch_kind(eff) == TYPE_UNION ||
               type_dispatch_kind(eff) == TYPE_ARRAY) {
        /* BUG-974: `return 0;` is not a value of an aggregate type — GCC refused it
         * ("incompatible types when returning type 'int'"), so a bare `orelse return`
         * in a function returning a SLICE or a STRUCT failed to compile at all. Valid
         * ZER, rejected by the C compiler rather than by ZER, which is the worst place
         * for a diagnostic to come from.
         *
         * The zero of these types EXISTS and is well defined — an empty slice, a zeroed
         * struct — so emit it as a compound literal. That is the same "the return value
         * comes from the function's return type" rule the integer case already follows;
         * only the SPELLING of zero differs. Contrast the non-null pointer and funcptr
         * cases, which have no zero at all and are rejected in the checker. */
        emit(e, "return (");
        emit_type(e, eff);
        emit(e, "){0}; ");
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
            /* BUG-749 (2026-06-18): pointer-to-struct auto-deref (`ptr.field`
             * for `*S ptr`) must also be scanned. Pre-fix expr_is_volatile
             * matched only direct struct values, so a volatile field reached
             * via `reg.status` for `*MMIO reg` returned false, and any
             * duplicating emitter (bounds-check + index) re-read the
             * volatile location twice — silent volatile-semantics violation
             * (program-consequence: a volatile-qualified read must be a
             * single C-level load; hardware: read-clear/sequence-counter/
             * FIFO registers misbehave). Unwrap pointer/optional here. */
            if (eff && eff->kind == TYPE_POINTER) eff = type_unwrap_distinct(eff->pointer.inner);
            if (eff && eff->kind == TYPE_OPTIONAL) eff = type_unwrap_distinct(eff->optional.inner);
            if (eff && eff->kind == TYPE_POINTER) eff = type_unwrap_distinct(eff->pointer.inner);
            if (eff && eff->kind == TYPE_STRUCT) {
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

/* A field-access OBJECT needs C parentheses when its emitted form binds LOOSER
 * than the postfix `.`/`->` accessor. Without them valid ZER `(*p).field`
 * mis-emits as `*p.field` = C `*(p.field)` — uncompilable, or (with a pointer
 * field) a silent wrong access. Same for `((T)x).field` and
 * `(a orelse b).field`. Found in the 2026-07-31 audit; verified on main by
 * tests/zer/explicit_deref_field.zer (gcc: "'p' is a pointer; did you mean
 * to use '->'?").
 *
 * Prefix-unary (`*` `&` `-` `!` `~`), casts, binary operators, orelse and
 * assignment all bind looser than the postfix accessor; primaries and postfix
 * forms (ident/field/index/call/literals) do not. Over-parenthesizing is
 * always harmless in C, so when a kind is arguable the safe answer is `true`.
 *
 * NO `default:` — walker_default_audit.sh + -Werror=switch make a NEW NodeKind
 * a hard build failure here, forcing an explicit decision instead of silently
 * defaulting to "no parens" (which is the unsafe direction). */
static bool field_obj_needs_parens(Node *obj) {
    if (!obj) return false;
    switch (obj->kind) {
    /* Bind LOOSER than postfix `.`/`->` -> parenthesize.
     * NODE_ASSIGN is here (the branch put it in the tight set): assignment is a
     * real ZER expression (`y = (x = a && b)`) and binds looser than `.`, so if
     * `(x = y).f` is ever constructible it needs the parens. Harmless if not. */
    case NODE_UNARY: case NODE_BINARY: case NODE_TYPECAST:
    case NODE_CAST: case NODE_ORELSE: case NODE_ASSIGN:
        return true;
    /* Primaries / postfix / statements: bind at least as tightly as the
     * accessor, or can never appear as a field object. */
    case NODE_IDENT: case NODE_FIELD: case NODE_INDEX: case NODE_CALL:
    case NODE_INTRINSIC: case NODE_SLICE: case NODE_STRUCT_INIT:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_SIZEOF:
    case NODE_BLOCK: case NODE_IF: case NODE_FOR: case NODE_WHILE:
    case NODE_DO_WHILE: case NODE_SWITCH: case NODE_RETURN: case NODE_BREAK:
    case NODE_CONTINUE: case NODE_GOTO: case NODE_LABEL: case NODE_EXPR_STMT:
    case NODE_DEFER: case NODE_CRITICAL: case NODE_ONCE: case NODE_AWAIT:
    case NODE_YIELD: case NODE_SPAWN: case NODE_VAR_DECL: case NODE_ASM:
    case NODE_STATIC_ASSERT: case NODE_FILE: case NODE_FUNC_DECL:
    case NODE_STRUCT_DECL: case NODE_ENUM_DECL: case NODE_UNION_DECL:
    case NODE_TYPEDEF: case NODE_IMPORT: case NODE_CINCLUDE:
    case NODE_INTERRUPT: case NODE_MMIO: case NODE_GLOBAL_VAR:
    case NODE_CONTAINER_DECL:
        return false;
    }
    return false;
}


/* Conservatively check if evaluating `n` may have observable side effects.
 * Used to decide whether to hoist a target/index into a temp before
 * duplicating it in the emitted C code.
 *
 * Returns true for any subexpression that is a NODE_CALL, NODE_ASSIGN,
 * NODE_ORELSE (may wrap a call), NODE_INTRINSIC (conservatively all),
 * or a NODE_UNARY deref of a non-trivial expression (volatile reads
 * must single-eval). Walks into NODE_FIELD/NODE_INDEX/NODE_SLICE
 * subexpressions so things like `arr[fn()] <<= n` and
 * `s.field[fn()] = v` are correctly classified.
 *
 * Pre-fix, several emission sites used a partial walker that descended
 * into NODE_INDEX.object but not NODE_INDEX.index, so `arr[fn()] <<= n`
 * silently evaluated `fn()` twice (BUG: indexed compound side-effect).
 */

static bool expr_has_side_effects(Node *n) {
    if (!n) return false;
    switch (n->kind) {
    /* Direct side effects. */
    case NODE_CALL:
    case NODE_ASSIGN:
    case NODE_ORELSE:
    case NODE_INTRINSIC:
        return true;
    case NODE_UNARY:
        /* Volatile deref must single-eval; conservatively flag any deref. */
        if (n->unary.op == TOK_STAR) return true;
        return expr_has_side_effects(n->unary.operand);
    case NODE_FIELD:
        return expr_has_side_effects(n->field.object);
    case NODE_INDEX:
        return expr_has_side_effects(n->index_expr.object) ||
               expr_has_side_effects(n->index_expr.index);
    case NODE_SLICE:
        return expr_has_side_effects(n->slice.object) ||
               expr_has_side_effects(n->slice.start) ||
               expr_has_side_effects(n->slice.end);
    case NODE_BINARY:
        return expr_has_side_effects(n->binary.left) ||
               expr_has_side_effects(n->binary.right);
    case NODE_TYPECAST:
        return expr_has_side_effects(n->typecast.expr);
    case NODE_STRUCT_INIT:
        for (int i = 0; i < n->struct_init.field_count; i++) {
            if (expr_has_side_effects(n->struct_init.fields[i].value)) return true;
        }
        return false;
    /* Leaf expressions — side-effect free. */
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_IDENT: case NODE_CAST: case NODE_SIZEOF:
        return false;
    /* Statement/decl nodes — caller should only pass expressions, but
     * be safe: a stray declaration node has no observable subexpression
     * side effects from our perspective (we don't lift values out of it). */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
    case NODE_VAR_DECL: case NODE_BLOCK: case NODE_IF: case NODE_FOR:
    case NODE_WHILE: case NODE_SWITCH: case NODE_RETURN: case NODE_BREAK:
    case NODE_CONTINUE: case NODE_DEFER: case NODE_GOTO: case NODE_LABEL:
    case NODE_EXPR_STMT: case NODE_ASM: case NODE_CRITICAL:
    case NODE_ONCE: case NODE_SPAWN: case NODE_YIELD: case NODE_AWAIT:
    case NODE_DO_WHILE: case NODE_STATIC_ASSERT:
        return false;
    }
    return false;
}

/* ---- Type emission ---- */
static void prescan_async_temps(Emitter *e, Node *node);
static bool is_condvar_type(Emitter *e, uint32_t type_id);

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

/* BUG-953 (refactor M, the structural half): does this op kind's `expr` get
 * auto-guards emitted before it?
 *
 * This used to be an ALLOWLIST of eight op kinds written inline at the emission
 * loop — in TWO copies, regular and async. An allowlist FAILS OPEN: an op kind
 * nobody added silently got no guard, while the checker had already printed
 * "auto-guard inserted". It was widened reactively three times, each after a
 * measured miscompile — async emission (2026-05-03/06), IR_AWAIT and IR_NOP
 * (2026-06-30), and IR_LOCK (BUG-952).
 *
 * Inverted, it FAILS CLOSED: an op kind nobody classified gets guarded, and the
 * worst case is a redundant check that is dead on the safe path — never a missing
 * one. That is the same conservative-default rule the rest of this compiler uses:
 * an unclassifiable form must round toward the SAFE answer.
 *
 * A no-default switch is the mechanism, not an if-chain, so a NEW IROpKind is a
 * BUILD FAILURE under -Werror=switch until someone classifies it, rather than
 * silently inheriting a default. That is the strongest of the mechanisms
 * CLAUDE.md lists, and it is free here because the sites are an enum. */
static bool ir_op_takes_auto_guards(IROpKind op) {
    switch (op) {
    /* EXCLUDED — guarding here would be wrong, not merely redundant.
     *
     * IR_UNLOCK carries the same indexed shared root as IR_LOCK, but it runs with
     * the lock HELD. emit_safety_early_return cannot return there (it would leak
     * the mutex) so it would emit a trap — and the LOCK guard has already taken a
     * clean early return on that path, so the check is both unreachable and
     * misleading. IR_LOCK is guarded; its partner must not be. */
    case IR_UNLOCK:
        return false;

    /* Everything else is guarded when it carries an expr. Listing each kind rather
     * than writing `default: return true` is the whole point: the compiler now
     * refuses to build when a kind is added and nobody has decided. */
    case IR_ASSIGN: case IR_CALL: case IR_BRANCH: case IR_GOTO:
    case IR_RETURN: case IR_YIELD: case IR_AWAIT: case IR_SPAWN:
    case IR_LOCK:
    case IR_POOL_ALLOC: case IR_POOL_FREE: case IR_POOL_GET:
    case IR_SLAB_ALLOC: case IR_SLAB_FREE: case IR_SLAB_FREE_PTR:
    case IR_SLAB_ALLOC_PTR:
    case IR_ARENA_ALLOC: case IR_ARENA_ALLOC_SLICE: case IR_ARENA_RESET:
    case IR_RING_PUSH: case IR_RING_POP: case IR_RING_PUSH_CHECKED:
    case IR_CRITICAL_BEGIN: case IR_CRITICAL_END:
    case IR_DEFER_PUSH: case IR_DEFER_FIRE:
    case IR_LITERAL: case IR_BINOP: case IR_UNOP: case IR_COPY:
    case IR_CAST: case IR_ADDR_OF: case IR_DEREF_READ:
    case IR_FIELD_READ: case IR_FIELD_WRITE:
    case IR_INDEX_READ: case IR_INDEX_WRITE: case IR_SLICE_READ:
    case IR_INTRINSIC: case IR_INTRINSIC_DECOMP:
    case IR_CALL_DECOMP: case IR_STRUCT_INIT_DECOMP: case IR_ORELSE_DECOMP:
    case IR_NOP:
    /* IR_TRAP never carries an expr, so this is moot — but the fail-closed
     * default is `true`, and stating it keeps the rule uniform. */
    case IR_TRAP:
        return true;
    }
    return true;   /* unreachable; conservative if a cast smuggles a bad value in */
}
static void emit_local_name(Emitter *e, IRFunc *func, int local_id);
static void emit_unreachable(Emitter *e, const char *what, Node *n);
static void emit_module_global_name(Emitter *e, const char *name, uint32_t len);   /* BUG-1040 */
static void emit_alloc_sym_cname(Emitter *e, Symbol *sym);                         /* BUG-1040 */
static void emit_rewritten_node(Emitter *e, Node *node, IRFunc *func);
static void emit_inttoptr(Emitter *e, Node *node, IRFunc *func);   /* BUG-1058 */

/* BUG-1019: the IR_CALL callee text, factored out of the four inline branches
 * that used to emit it together with the opening `(`. Emits ONLY the callee —
 * no paren — so the caller can either open the arg list directly or wrap the
 * callee in the null-funcptr guard. Four shapes: a direct/mangled name, a
 * struct-field funcptr, an array-element funcptr, and the unreachable
 * fallback. */
static void emit_ir_call_callee(Emitter *e, IRInst *inst, IRFunc *func) {
    /* Emit callee: simple ident or field access (funcptr through struct) */
    if (inst->func_name) {
        /* Check for cross-module function needing mangled name */
        Symbol *fsym = scope_lookup(e->checker->global_scope,
            inst->func_name, inst->func_name_len);
        if (fsym && fsym->is_function && fsym->module_prefix) {
            emit(e, "%.*s__%.*s",
                 (int)fsym->module_prefix_len, fsym->module_prefix,
                 (int)inst->func_name_len, inst->func_name);
        } else {
            emit(e, "%.*s", (int)inst->func_name_len, inst->func_name);
        }
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
                emit(e, "%s%.*s",
                     (ot_eff && ot_eff->kind == TYPE_POINTER) ? "->" : ".",
                     (int)callee->field.field_name_len, callee->field.field_name);
            } else {
                /* Global/extern funcptr */
                emit(e, "%.*s.%.*s",
                     (int)callee->field.object->ident.name_len,
                     callee->field.object->ident.name,
                     (int)callee->field.field_name_len, callee->field.field_name);
            }
        } else {
            emit_unreachable(e, "this call target", inst->expr);   /* BUG-851 */
        }
    } else if (inst->expr && inst->expr->kind == NODE_CALL &&
               inst->expr->call.callee &&
               inst->expr->call.callee->kind == NODE_INDEX) {
        /* Indexed funcptr callee: arr[i](args) / slice[i](args).
         * BUG-1027b: this used to be a hand-rolled `name[index]` spelling
         * that never went through the NODE_INDEX emitter, so a SLICE
         * callee lost its `.ptr` and its `_zer_bounds_check` (GCC then
         * refused `s[i](..)`; a fixed array is guarded by the IR branch
         * the checker's auto-guard lowers to, so that form was safe).
         * Route the callee through the one index emitter instead. The
         * BUG-1019 null-funcptr guard wraps this callee text in
         * `__typeof__(...)` + one initialiser, so the bounds check inside
         * the index emitter still evaluates exactly once. */
        emit_rewritten_node(e, inst->expr->call.callee, func);
    } else {
        emit_unreachable(e, "this callee expression", inst->expr);   /* BUG-851 */
    }
}

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

/* Emit `{ defers; return [value]; }` for early-exit safety guards
 * (auto-guard NODE_INDEX, UAF auto-guard NODE_FIELD, @cstr overflow auto-orelse).
 *
 * Async-aware: in async function bodies (Duff's-device poll loops), a bare
 * C `return;` would suspend the coroutine without signalling completion —
 * subsequent polls would re-enter at state 0 and re-run the prologue,
 * silently looping. Emit the same termination sequence IR_RETURN uses for
 * async (`self->_zer_state = -1; return 1;`) so the auto-guard early-out
 * marks the coroutine done.
 *
 * Main-promotion-aware: `void main()` is auto-promoted to `int main(void)`.
 * A bare `return;` in C `int main(void)` makes the exit code UB — eax holds
 * whatever happened to be there. Observed exit=208 on gcc -O2 vs exit=0 on
 * -O0 for the same source. Emit `return 0;` when `current_main_promoted`.
 *
 * `with_braces` controls whether the leading `{` / trailing `}` is emitted —
 * NODE_INDEX/NODE_FIELD callers want explicit braces+newline (statement form);
 * @cstr inline statement-expression already opened the brace via "if (...) { ". */
static void emit_safety_early_return(Emitter *e, bool with_braces) {
    /* §C #16: inside a defer body an early-return would re-fire the defer stack
     * and skip the remaining function cleanup, so a bounds/UAF auto-guard traps
     * instead (aborts safely before the out-of-bounds access). See guard_traps. */
    /* BUG-835: the guard's runtime form is `if (idx >= N) { <defers>; return X; }`.
     * guard_traps already covered a DEFER body; nothing covered the two OTHER scopes
     * a `return` must never leave, so the compiler emitted, verbatim:
     *
     *   __asm__("mrs %0, primask\n cpsid i" ...);   // interrupts OFF
     *   if ((size_t)(i) >= 4u) { return 0; }        // <-- leaves them OFF forever
     *   __asm__("msr primask, %0" ...);             // never reached
     *
     *   pthread_mutex_lock(&g._zer_mtx);
     *   if ((size_t)(i) >= 4u) { return; }          // <-- mutex never released
     *   pthread_mutex_unlock(&g._zer_mtx);
     *
     * ZER hard-errors a USER-written `return` inside @critical for exactly this
     * reason; the compiler was emitting the construct it bans. MEASURED: the lock
     * form HANGS FOREVER (the join waits on a worker that can never take the mutex);
     * the @critical form returned 0 silently, because hosted x86-64 degrades the
     * critical section to a compiler fence — on bare metal it leaves interrupts off
     * with no fault to notice it by. That asymmetry is why no existing test caught
     * either half. Trapping is the same trade-off already accepted for defer bodies,
     * and consistent with slices, which already TRAP on an out-of-range index. */
    /* BUG-1222: an early return must RETURN SOMETHING, and for a `*T` function
     * (non-null) or an enum without a 0 variant the zero it used to return is not
     * a value of the type — a NULL non-null pointer handed to the caller (on bare
     * metal a silent access near address 0), or a non-variant enum that the
     * exhaustive switch's last-arm elision turns into a wrong arm. Trap, exactly
     * like the lock / @critical / defer scopes below. */
    bool ret_has_no_zero = e->current_func_ret &&
        checker_type_has_no_zero_value(e->current_func_ret);
    if (ret_has_no_zero) {
        if (with_braces) emit(e, "{ ");
        emit(e, "_zer_trap(\"out-of-bounds access in a function whose return type has "
                "no zero value (non-null pointer / enum without a 0 variant) — it cannot "
                "return early\", __FILE__, __LINE__);");
        if (with_braces) emit(e, " }\n"); else emit(e, " ");
        return;
    }
    if (e->guard_traps || e->noreturn_scope_depth > 0) {
        if (with_braces) emit(e, "{ ");
        emit(e, "_zer_trap(\"out-of-bounds access inside a held lock, @critical block, @once body, semaphore hold "
                "or defer cleanup — cannot return without leaking it\", __FILE__, __LINE__);");
        if (with_braces) emit(e, " }\n"); else emit(e, " ");
        return;
    }
    if (with_braces) emit(e, "{\n");
    emit_defers(e);
    if (e->in_async) {
        if (with_braces) emit_indent(e);
        emit(e, "self->_zer_state = -1; return 1;");
    } else if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
        emit(e, "return ");
        emit_zero_value(e, e->current_func_ret);
        emit(e, ";");
    } else if (e->current_main_promoted) {
        emit(e, "return 0;");
    } else {
        emit(e, "return;");
    }
    if (with_braces) emit(e, " }\n");
    else emit(e, " ");
}


/* Walk expression tree, emit auto-guard if-return statements for unproven NODE_INDEX.
 * Called BEFORE emit_expr for the containing statement. */
/* BUG-955 (refactor M): the DESCENT moved to checker_walk_guard_sites, so the
 * emitter and the IR lowering find guard sites with ONE walker instead of each
 * carrying a copy. This is now only the C rendering of a site.
 *
 * Byte-for-byte the same output as the inline version it replaces — the site order
 * (an access's own guard before its subexpressions, object before index) is part of
 * the walker's contract for exactly that reason, and the whole 587-file corpus was
 * diffed to confirm nothing moved. */
static void emit_one_guard(void *ud, const ZerGuardSite *site) {
    Emitter *e = (Emitter *)ud;
    emit_indent(e);
    if (site->freed_idx) {
        emit(e, "if ((");
        emit_expr(e, site->index_expr);
        emit(e, ") == (");
        emit_expr(e, site->freed_idx);
        emit(e, ")) ");
    } else {
        emit(e, "if ((size_t)(");
        emit_expr(e, site->index_expr);
        emit(e, ") >= %lluu) ", (unsigned long long)site->array_size);
    }
    emit_safety_early_return(e, true);
}

static void emit_auto_guards(Emitter *e, Node *node) {
    checker_walk_guard_sites(e->checker, node, emit_one_guard, e);
}

static Node *find_shared_root(Emitter *e, Node *expr); /* forward decl */


static Node *find_shared_root(Emitter *e, Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == NODE_FIELD) {
        /* Walk to root of field chain */
        Node *root = expr_root_ident(expr);  /* BUG-817 */
        if (root) {
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


/* Check if a shared struct type uses reader-writer lock */
static bool shared_is_rw(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind == TYPE_POINTER) eff = type_unwrap_distinct(eff->pointer.inner);
    if (eff && eff->kind == TYPE_STRUCT) return eff->struct_type.is_shared_rw;
    return false;
}


/* Emit lock acquire for shared struct variable.
 * For shared(rw) structs, is_write determines rdlock vs wrlock. */
static void emit_shared_lock_mode(Emitter *e, Node *root, bool is_write) {
    e->noreturn_scope_depth++;   /* BUG-835: counted HERE so all five call sites are
                                  * covered by construction, not by remembering. */
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
    if (e->noreturn_scope_depth > 0) e->noreturn_scope_depth--;   /* BUG-835 */
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

static void emit_type_and_name(Emitter *e, Type *t, const char *name, size_t len);
/* BUG-1111: ONE function-declarator emitter for the prototype path and the IR
 * definition path. A function RETURNING a function pointer needs C's nested
 * declarator `RET (*name(params))(fp_args)`; only the definition path knew, so a
 * bodyless prototype `*(u32, u32) -> u32 select_op(u32 kind);` emitted
 * `uint32_t (*)(uint32_t, uint32_t) select_op(uint32_t kind);`, which GCC
 * refuses. Split as head / params / tail so each caller keeps its own name
 * spelling (the IR path mangles from IRFunc, the prototype from the AST). */
static bool func_ret_is_funcptr(Type *ret, bool main_promote) {
    return !main_promote && ret && type_dispatch_kind(ret) == TYPE_FUNC_PTR;
}
static void emit_func_decl_head(Emitter *e, Type *ret, bool main_promote) {
    if (main_promote) {
        emit(e, "int ");
    } else if (func_ret_is_funcptr(ret, main_promote)) {
        emit_type(e, type_unwrap_distinct(ret)->func_ptr.ret);
        emit(e, " (*");
    } else if (ret) {
        emit_type(e, ret);
        emit(e, " ");
    } else {
        emit(e, "void ");
    }
}
static void emit_func_decl_params(Emitter *e, Node *fn, Type *func_type) {
    emit(e, "(");
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
        if (fn->func_decl.is_variadic) emit(e, ", ...");
    }
    emit(e, ")");
}
static void emit_func_decl_tail(Emitter *e, Type *ret, bool main_promote) {
    if (!func_ret_is_funcptr(ret, main_promote)) return;
    Type *r = type_unwrap_distinct(ret);
    emit(e, ")(");
    if (r->func_ptr.param_count == 0) emit(e, "void");
    for (uint32_t i = 0; i < r->func_ptr.param_count; i++) {
        if (i > 0) emit(e, ", ");
        emit_type(e, r->func_ptr.params[i]);
    }
    emit(e, ")");
}

static void emit_defers(Emitter *e);
static void emit_defers_from(Emitter *e, int base);
static void emit_defer_body(Emitter *e, IRFunc *func, Node *db);   /* BUG-1298 */
/* BUG-1298: the flag id of a @once — its index among the function's @once NODES. */
static int emit_once_id(Emitter *e, Node *n) {
    for (int i = 0; i < e->once_n; i++)
        if (e->once_nodes[i] == (void *)n) return i;
    if (e->once_n == e->once_cap) {
        int nc = e->once_cap ? e->once_cap * 2 : 8;
        void **nn = (void **)realloc(e->once_nodes, (size_t)nc * sizeof(void *));
        if (!nn) return 0;
        e->once_nodes = nn;
        e->once_cap = nc;
    }
    e->once_nodes[e->once_n] = (void *)n;
    return e->once_n++;
}
static void emit_defer_stmt(Emitter *e, Node *s, IRFunc *func);

static void emit_ptr_to_elem(Emitter *e, Type *elem, bool is_volatile); /* BUG-1027, defined below */

/* emit array→slice coercion: wraps array expr in slice compound literal */
static void emit_array_as_slice(Emitter *e, Node *array_expr, Type *array_type, Type *slice_type) {
    emit(e, "((");
    emit_type(e, slice_type);
    emit(e, "){ (");
    /* BUG-1027: a funcptr/array element needs the `ret (**)(..)` / `T (*)[N]`
     * declarator spelling — `emit_type(inner)*` is invalid C for those. */
    emit_ptr_to_elem(e, array_type->array.inner,
                     slice_type && slice_type->slice.is_volatile);
    emit(e, ")");
    emit_expr(e, array_expr);
    emit(e, ", %llu })", (unsigned long long)array_type->array.size);
}

/* Path C: emit the C carrier type for an arbitrary-width integer.
 * Carrier = smallest native int that holds `bits` (odd widths are masked
 * to `bits` at arithmetic/read sites in Phase C). */
static void emit_intn_carrier(Emitter *e, uint32_t bits, bool is_signed) {
    if (is_signed) {
        if (bits <= 8) emit(e, "int8_t");
        else if (bits <= 16) emit(e, "int16_t");
        else if (bits <= 32) emit(e, "int32_t");
        else if (bits <= 64) emit(e, "int64_t");
        else emit(e, "__int128");
    } else {
        if (bits <= 8) emit(e, "uint8_t");
        else if (bits <= 16) emit(e, "uint16_t");
        else if (bits <= 32) emit(e, "uint32_t");
        else if (bits <= 64) emit(e, "uint64_t");
        else emit(e, "unsigned __int128");
    }
}

/* G1 (2026-08-01): carrier typedef suffix for a uN/iN width. The native carrier
 * is the smallest int >= bits, so a `?u5` / `[*]u5` has the EXACT C layout of a
 * `?u8` / `[*]u8` and can reuse that already-emitted NAMED typedef.
 *
 * Without this, non-native-width optional/slice compound types fell through to
 * the anonymous-struct fallback, which emits a FRESH `struct { ... }` at every
 * use site. Two such instances are distinct, incompatible C types, so any
 * copy/assign/return failed at gcc ("incompatible types when assigning to type
 * 'struct <anonymous>'") — `?uN` / `[*]uN` were simply unusable. This restores
 * the named-typedef invariant (compiler-internals.md: "Named typedefs required
 * for EVERY compound type — prevents anonymous struct duplication"). */
static const char *intn_carrier_suffix(uint32_t bits, bool is_signed) {
    if (is_signed)
        return (bits <= 8) ? "i8" : (bits <= 16) ? "i16" :
               (bits <= 32) ? "i32" : (bits <= 64) ? "i64" : "i128";
    return (bits <= 8) ? "u8" : (bits <= 16) ? "u16" :
           (bits <= 32) ? "u32" : (bits <= 64) ? "u64" : "u128";
}

/* ================================================================
 * BUG-1027 (2026-09-14): ONE query for the name of a slice typedef, for EVERY
 * element type — and the machinery that emits the typedef when no earlier
 * site already did.
 *
 * Three hand-rolled suffix switches (emit_type TYPE_SLICE, emit_type ?[*]T,
 * the NODE_SLICE literal) each listed the primitive/struct/union element kinds
 * and let every OTHER kind fall through an exhaustive case list into the G1
 * `TYPE_UINT` arm, which read `intn.bits` out of a non-intn Type (a pointer's
 * low bits, an array's inner pointer) and named the slice `_zer_slice_u128`.
 * Measured on a valid program: `[*]State s = arr; s[2]` read with a 16-byte
 * stride over a 4-byte enum array (wrong value, then a stack OOB read);
 * `[*]?*Task` read two pointers as one; `[*]Handle(T)` trapped; a funcptr
 * element emitted `(uint32_t (*)(uint32_t)*)` and did not compile. The corpus
 * only ever sliced `u*`/`i*`/struct, so no test exercised the fallthrough.
 *
 * Classification of an element type E:
 *   - primitive / bool / uN,iN / enum (int32_t) / Handle (uint64_t):
 *     reuse the PREAMBLE typedef `_zer_slice_<carrier>`
 *   - struct / union: `_zer_slice_<user name>`, emitted at the declaration
 *   - EXOTIC (pointer, optional value, funcptr, array, nested slice, *opaque,
 *     builtin containers): `_zer_xslice_<mangled>`, emitted by
 *     flush_exotic_slices once its named dependencies exist. Separate prefix
 *     (`x`) + length-prefixed user names keep the mangling injective, so a
 *     user struct named `p_Task` can never collide with `[*]*Task`.
 * ================================================================ */

/* growable byte buffer for building a mangled suffix (no fixed buffers) */
typedef struct { char *p; size_t len; size_t cap; } XSBuf;

static void xsb_put(XSBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = (char *)realloc(b->p, nc);
        if (!np) { fprintf(stderr, "zerc: out of memory\n"); exit(1); }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void xsb_puts(XSBuf *b, const char *s) { xsb_put(b, s, strlen(s)); }
static void xsb_putu(XSBuf *b, unsigned long long v) {
    char tmp[32]; /* a u64 has at most 20 decimal digits — bounded, not dynamic */
    int n = snprintf(tmp, sizeof tmp, "%llu", v);
    xsb_put(b, tmp, (size_t)n);
}
/* `<tag><len>_<prefix__name>` — length-prefixed so a user name containing '_'
 * cannot be confused with the grammar tokens around it */
static void xsb_user(XSBuf *b, const char *tag, const char *prefix, uint32_t plen,
                     const char *name, uint32_t nlen) {
    size_t total = (prefix && plen) ? (size_t)plen + 2 + nlen : nlen;
    xsb_puts(b, tag); xsb_putu(b, total); xsb_puts(b, "_");
    if (prefix && plen) { xsb_put(b, prefix, plen); xsb_puts(b, "__"); }
    xsb_put(b, name, nlen);
}

/* The mangled suffix of ANY type (exhaustive — a new TypeKind fails the build). */
static void xslice_suffix_append(XSBuf *b, Type *t) {
    t = type_unwrap_distinct(t);
    if (!t) { xsb_puts(b, "void"); return; }
    switch (t->kind) {
    case TYPE_VOID:   xsb_puts(b, "void"); break;
    case TYPE_BOOL:   xsb_puts(b, "u8"); break;   /* bool = uint8_t */
    case TYPE_U8:     xsb_puts(b, "u8"); break;
    case TYPE_U16:    xsb_puts(b, "u16"); break;
    case TYPE_U32:    xsb_puts(b, "u32"); break;
    case TYPE_U64:    xsb_puts(b, "u64"); break;
    case TYPE_USIZE:  xsb_puts(b, "usize"); break;
    case TYPE_I8:     xsb_puts(b, "i8"); break;
    case TYPE_I16:    xsb_puts(b, "i16"); break;
    case TYPE_I32:    xsb_puts(b, "i32"); break;
    case TYPE_I64:    xsb_puts(b, "i64"); break;
    case TYPE_F32:    xsb_puts(b, "f32"); break;
    case TYPE_F64:    xsb_puts(b, "f64"); break;
    case TYPE_UINT:   xsb_puts(b, intn_carrier_suffix(t->intn.bits, false)); break;
    case TYPE_SINT:   xsb_puts(b, intn_carrier_suffix(t->intn.bits, true)); break;
    case TYPE_ENUM:   xsb_puts(b, "i32"); break;   /* enums are int32_t */
    case TYPE_HANDLE: xsb_puts(b, "u64"); break;   /* Handle = uint64_t */
    case TYPE_OPAQUE: xsb_puts(b, "opq"); break;
    case TYPE_POINTER: {
        Type *in = type_unwrap_distinct(t->pointer.inner);
        if (type_dispatch_kind(in) == TYPE_OPAQUE) {
            /* *opaque is the VALUE type _zer_opaque, not a C pointer */
            xsb_puts(b, t->pointer.is_const ? (t->pointer.is_volatile ? "cvopq" : "copq")
                                            : (t->pointer.is_volatile ? "vopq" : "opq"));
        } else {
            xsb_puts(b, "p");
            if (t->pointer.is_const) xsb_puts(b, "c");
            if (t->pointer.is_volatile) xsb_puts(b, "v");
            xsb_puts(b, "_");
            xslice_suffix_append(b, t->pointer.inner);
        }
        break;
    }
    case TYPE_OPTIONAL:
        /* ?*T / ?funcptr have the SAME C type as the inner (null sentinel) */
        if (!is_null_sentinel(t->optional.inner)) xsb_puts(b, "o_");
        xslice_suffix_append(b, t->optional.inner);
        break;
    case TYPE_SLICE:
        xsb_puts(b, t->slice.is_volatile ? "sv_" : "s_");
        xslice_suffix_append(b, t->slice.inner);
        break;
    case TYPE_ARRAY:
        if (t->array.sizeof_type) {
            xsb_puts(b, "az_"); xslice_suffix_append(b, t->array.sizeof_type); xsb_puts(b, "_");
        } else {
            xsb_puts(b, "a"); xsb_putu(b, t->array.size); xsb_puts(b, "_");
        }
        xslice_suffix_append(b, t->array.inner);
        break;
    case TYPE_STRUCT:
        xsb_user(b, "N", t->struct_type.module_prefix, t->struct_type.module_prefix_len,
                 t->struct_type.name, t->struct_type.name_len);
        break;
    case TYPE_UNION:
        xsb_user(b, "M", t->union_type.module_prefix, t->union_type.module_prefix_len,
                 t->union_type.name, t->union_type.name_len);
        break;
    case TYPE_FUNC_PTR:
        xsb_puts(b, t->func_ptr.is_variadic ? "fv" : "f");
        xsb_putu(b, t->func_ptr.param_count);
        xsb_puts(b, "_");
        xslice_suffix_append(b, t->func_ptr.ret);
        for (uint32_t i = 0; i < t->func_ptr.param_count; i++) {
            xsb_puts(b, "_");
            xslice_suffix_append(b, t->func_ptr.params[i]);
        }
        break;
    case TYPE_POOL:
        xsb_puts(b, "pool"); xsb_putu(b, t->pool.count); xsb_puts(b, "_");
        xslice_suffix_append(b, t->pool.elem);
        break;
    case TYPE_RING:
        xsb_puts(b, "ring"); xsb_putu(b, t->ring.count); xsb_puts(b, "_");
        xslice_suffix_append(b, t->ring.elem);
        break;
    case TYPE_ARENA:     xsb_puts(b, "arena"); break;
    case TYPE_BARRIER:   xsb_puts(b, "barrier"); break;
    case TYPE_SEMAPHORE: xsb_puts(b, "sem"); break;
    case TYPE_SLAB:      xsb_puts(b, "slab"); break;
    case TYPE_DISTINCT:  xslice_suffix_append(b, t->distinct.underlying); break; /* unwrapped above */
    }
}

/* Does `[*]elem` need an emitted-on-demand typedef (no preamble/declaration one)? */
static bool slice_elem_is_exotic(Type *elem) {
    elem = type_unwrap_distinct(elem);
    if (!elem) return true;
    switch (elem->kind) {
    case TYPE_BOOL: case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
    case TYPE_USIZE: case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
    case TYPE_F32: case TYPE_F64: case TYPE_UINT: case TYPE_SINT:
    case TYPE_ENUM: case TYPE_HANDLE:           /* preamble carrier typedef */
    case TYPE_STRUCT: case TYPE_UNION:          /* emitted at the declaration */
        return false;
    case TYPE_VOID: case TYPE_POINTER: case TYPE_OPTIONAL: case TYPE_SLICE:
    case TYPE_ARRAY: case TYPE_FUNC_PTR: case TYPE_OPAQUE: case TYPE_POOL:
    case TYPE_RING: case TYPE_ARENA: case TYPE_BARRIER: case TYPE_SLAB:
    case TYPE_SEMAPHORE:
        return true;
    case TYPE_DISTINCT:
        return slice_elem_is_exotic(elem->distinct.underlying); /* unwrapped above */
    }
    return true;
}

/* THE query: emit the typedef name of the slice over `elem`.
 * is_opt selects the `?[*]T` wrapper typedef; is_volatile the `volatile [*]T` one. */
static void emit_slice_name(Emitter *e, Type *elem, bool is_volatile, bool is_opt) {
    elem = type_unwrap_distinct(elem);
    XSBuf b = {0};
    if (slice_elem_is_exotic(elem)) {
        xslice_suffix_append(&b, elem);
        emit(e, "%s%s", is_opt ? "_zer_xopt_slice_" : is_volatile ? "_zer_xvslice_" : "_zer_xslice_",
             b.p ? b.p : "");
        free(b.p);
        return;
    }
    emit(e, "%s", is_opt ? "_zer_opt_slice_" : is_volatile ? "_zer_vslice_" : "_zer_slice_");
    if (type_dispatch_kind(elem) == TYPE_STRUCT) { EMIT_STRUCT_NAME(e, elem); return; }
    if (type_dispatch_kind(elem) == TYPE_UNION)  { EMIT_UNION_NAME(e, elem); return; }
    xslice_suffix_append(&b, elem);   /* primitive / uN / enum / Handle carrier */
    emit(e, "%s", b.p ? b.p : "");
    free(b.p);
}

/* Emit the C type "pointer to elem" — the `ptr` field of a slice over `elem`
 * (name = "ptr") or the cast an array→slice coercion applies (name = NULL, an
 * abstract declarator). A funcptr element needs `ret (**name)(params)` and an
 * array element `base (*name)[N]`; `emit_type(elem)* name` is only right for
 * the other kinds. */
static void emit_ptr_to_elem_named(Emitter *e, Type *elem, bool is_volatile, const char *name) {
    elem = type_unwrap_distinct(elem);
    const char *nm = name ? name : "";
    if (type_dispatch_kind(elem) == TYPE_FUNC_PTR) {
        emit_type(e, elem->func_ptr.ret);
        emit(e, is_volatile ? " (* volatile *%s)(" : " (**%s)(", nm);
        for (uint32_t i = 0; i < elem->func_ptr.param_count; i++) {
            if (i > 0) emit(e, ", ");
            emit_type(e, elem->func_ptr.params[i]);
        }
        emit(e, ")");
        return;
    }
    if (type_dispatch_kind(elem) == TYPE_ARRAY) {
        Type *base = elem;
        while (base->kind == TYPE_ARRAY) base = base->array.inner;
        if (is_volatile) emit(e, "volatile ");
        emit_type(e, base);
        emit(e, " (*%s)", nm);
        for (Type *dim = elem; type_dispatch_kind(dim) == TYPE_ARRAY; dim = dim->array.inner) {
            if (dim->array.sizeof_type) {
                emit(e, "[sizeof("); emit_type(e, dim->array.sizeof_type); emit(e, ")]");
            } else {
                emit(e, "[%llu]", (unsigned long long)dim->array.size);
            }
        }
        return;
    }
    if (is_volatile) emit(e, "volatile ");
    emit_type(e, elem);
    emit(e, "*");
    if (name) emit(e, " %s", name);
}
static void emit_ptr_to_elem(Emitter *e, Type *elem, bool is_volatile) {
    emit_ptr_to_elem_named(e, elem, is_volatile, NULL);
}

/* ---- the exotic-slice registry ---- */

static bool user_type_same(Type *a, Type *b) {
    a = type_unwrap_distinct(a); b = type_unwrap_distinct(b);
    if (!a || !b || a->kind != b->kind) return false;
    const char *an, *bn, *ap, *bp; uint32_t anl, bnl, apl, bpl;
    if (type_dispatch_kind(a) == TYPE_STRUCT) {
        an = a->struct_type.name; anl = a->struct_type.name_len;
        ap = a->struct_type.module_prefix; apl = a->struct_type.module_prefix_len;
        bn = b->struct_type.name; bnl = b->struct_type.name_len;
        bp = b->struct_type.module_prefix; bpl = b->struct_type.module_prefix_len;
    } else if (type_dispatch_kind(a) == TYPE_UNION) {
        an = a->union_type.name; anl = a->union_type.name_len;
        ap = a->union_type.module_prefix; apl = a->union_type.module_prefix_len;
        bn = b->union_type.name; bnl = b->union_type.name_len;
        bp = b->union_type.module_prefix; bpl = b->union_type.module_prefix_len;
    } else {
        return a == b;
    }
    if (anl != bnl || memcmp(an, bn, anl) != 0) return false;
    if (!ap) apl = 0;
    if (!bp) bpl = 0;
    if (apl != bpl) return false;
    return apl == 0 || memcmp(ap, bp, apl) == 0;
}

/* Called by every site that emits a struct/union DEFINITION (declaration,
 * container stamp) so flush_exotic_slices knows which names are complete. */
static void record_user_type_emitted(Emitter *e, Type *t) {
    if (!t) return;
    if (e->emitted_user_count == e->emitted_user_capacity) {
        int nc = e->emitted_user_capacity ? e->emitted_user_capacity * 2 : 32;
        Type **np = (Type **)realloc(e->emitted_user_types, (size_t)nc * sizeof(Type *));
        if (!np) { fprintf(stderr, "zerc: out of memory\n"); exit(1); }
        e->emitted_user_types = np; e->emitted_user_capacity = nc;
    }
    e->emitted_user_types[e->emitted_user_count++] = t;
}

static bool user_type_emitted(Emitter *e, Type *t) {
    for (int i = 0; i < e->emitted_user_count; i++)
        if (user_type_same(e->emitted_user_types[i], t)) return true;
    return false;
}

static struct XSlice *xslice_find(Emitter *e, const char *suffix) {
    for (int i = 0; i < e->xslice_count; i++)
        if (strcmp(e->xslices[i].suffix, suffix) == 0) return &e->xslices[i];
    return NULL;
}

static void xslice_register(Emitter *e, Type *elem) {
    XSBuf b = {0};
    xslice_suffix_append(&b, elem);
    if (!b.p) return;
    if (!xslice_find(e, b.p)) {
        if (e->xslice_count == e->xslice_capacity) {
            int nc = e->xslice_capacity ? e->xslice_capacity * 2 : 16;
            struct XSlice *np = (struct XSlice *)realloc(e->xslices, (size_t)nc * sizeof(struct XSlice));
            if (!np) { fprintf(stderr, "zerc: out of memory\n"); exit(1); }
            e->xslices = np; e->xslice_capacity = nc;
        }
        char *owned = (char *)arena_alloc(e->arena, b.len + 1);
        memcpy(owned, b.p, b.len + 1);
        struct XSlice *x = &e->xslices[e->xslice_count++];
        x->suffix = owned;
        x->elem = type_unwrap_distinct(elem);
        x->emitted = false;
    }
    free(b.p);
}

/* pointer set (open addressing) — the visited set for the type walk; the type
 * graph is cyclic through struct fields (`struct N { ?*N next; }`) */
typedef struct { Type **keys; size_t cap; size_t count; } XPtrSet;

static bool xptrset_add(XPtrSet *s, Type *k) {
    if (s->count * 2 >= s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 1024;
        Type **nk = (Type **)calloc(nc, sizeof(Type *));
        if (!nk) { fprintf(stderr, "zerc: out of memory\n"); exit(1); }
        for (size_t i = 0; i < s->cap; i++) {
            Type *o = s->keys[i];
            if (!o) continue;
            size_t j = ((uintptr_t)o >> 4) % nc;
            while (nk[j]) j = (j + 1) % nc;
            nk[j] = o;
        }
        free(s->keys); s->keys = nk; s->cap = nc;
    }
    size_t j = ((uintptr_t)k >> 4) % s->cap;
    while (s->keys[j]) {
        if (s->keys[j] == k) return false;
        j = (j + 1) % s->cap;
    }
    s->keys[j] = k; s->count++;
    return true;
}

/* Walk every type reachable from `t`, registering each exotic slice element. */
static void collect_walk(Emitter *e, XPtrSet *seen, Type *t) {
    if (!t || !xptrset_add(seen, t)) return;
    switch (t->kind) {
    case TYPE_SLICE:
        if (slice_elem_is_exotic(t->slice.inner)) xslice_register(e, t->slice.inner);
        collect_walk(e, seen, t->slice.inner);
        break;
    case TYPE_POINTER:  collect_walk(e, seen, t->pointer.inner); break;
    case TYPE_OPTIONAL: collect_walk(e, seen, t->optional.inner); break;
    case TYPE_ARRAY:
        collect_walk(e, seen, t->array.inner);
        collect_walk(e, seen, t->array.sizeof_type);
        break;
    case TYPE_STRUCT:
        for (uint32_t i = 0; i < t->struct_type.field_count; i++)
            collect_walk(e, seen, t->struct_type.fields[i].type);
        break;
    case TYPE_UNION:
        for (uint32_t i = 0; i < t->union_type.variant_count; i++)
            collect_walk(e, seen, t->union_type.variants[i].type);
        break;
    case TYPE_FUNC_PTR:
        collect_walk(e, seen, t->func_ptr.ret);
        for (uint32_t i = 0; i < t->func_ptr.param_count; i++)
            collect_walk(e, seen, t->func_ptr.params[i]);
        break;
    case TYPE_POOL:     collect_walk(e, seen, t->pool.elem); break;
    case TYPE_RING:     collect_walk(e, seen, t->ring.elem); break;
    case TYPE_HANDLE:   collect_walk(e, seen, t->handle.elem); break;
    case TYPE_SLAB:     collect_walk(e, seen, t->slab.elem); break;
    case TYPE_DISTINCT: collect_walk(e, seen, t->distinct.underlying); break;
    case TYPE_VOID: case TYPE_BOOL: case TYPE_U8: case TYPE_U16: case TYPE_U32:
    case TYPE_U64: case TYPE_USIZE: case TYPE_I8: case TYPE_I16: case TYPE_I32:
    case TYPE_I64: case TYPE_F32: case TYPE_F64: case TYPE_ENUM: case TYPE_OPAQUE:
    case TYPE_ARENA: case TYPE_BARRIER: case TYPE_SEMAPHORE: case TYPE_UINT:
    case TYPE_SINT:
        break;  /* leaves: nothing nested */
    }
}

/* Register every exotic slice type the program names anywhere: the checker's
 * typemap holds the resolved type of every node AND every resolved type node
 * (RF3), so all slice types the emitter can ever spell are reachable from it;
 * container stamps are walked too. Idempotent — the registry dedupes. */
static void collect_exotic_slices(Emitter *e) {
    if (!e->checker) return;
    XPtrSet seen = {0};
    Checker *c = e->checker;
    for (uint32_t i = 0; i < c->type_map_size; i++)
        if (c->type_map[i].key) collect_walk(e, &seen, c->type_map[i].type);
    for (int i = 0; i < c->container_inst_count; i++) {
        collect_walk(e, &seen, c->container_instances[i].stamped_struct);
        collect_walk(e, &seen, c->container_instances[i].concrete_type);
    }
    free(seen.keys);
}

/* Can `[*]elem`'s typedef be emitted NOW? Every struct/union it names by VALUE
 * (or under an array, which C requires complete) must already be defined, and
 * every nested exotic slice typedef must already be emitted. A struct reached
 * only through a pointer needs no definition (`struct T*` may be incomplete),
 * which is what lets `struct Task { [*]?*Task kids; }` compile. */
static bool xslice_deps_ready(Emitter *e, Type *t) {
    t = type_unwrap_distinct(t);
    if (!t) return true;
    switch (t->kind) {
    case TYPE_STRUCT: case TYPE_UNION:
        return user_type_emitted(e, t);
    case TYPE_POINTER: {
        Type *in = type_unwrap_distinct(t->pointer.inner);
        if (type_dispatch_kind(in) == TYPE_STRUCT || type_dispatch_kind(in) == TYPE_UNION) return true;
        return xslice_deps_ready(e, in);
    }
    case TYPE_OPTIONAL: {
        Type *in = type_unwrap_distinct(t->optional.inner);
        return xslice_deps_ready(e, in);   /* value optional: `_zer_opt_X` needs X; ?*X same as *X */
    }
    case TYPE_SLICE: {
        if (slice_elem_is_exotic(t->slice.inner)) {
            XSBuf b = {0};
            xslice_suffix_append(&b, t->slice.inner);
            struct XSlice *x = b.p ? xslice_find(e, b.p) : NULL;
            free(b.p);
            return x && x->emitted;
        }
        return xslice_deps_ready(e, t->slice.inner);
    }
    case TYPE_ARRAY:
        return xslice_deps_ready(e, t->array.inner) &&
               xslice_deps_ready(e, t->array.sizeof_type);
    case TYPE_FUNC_PTR:
        if (!xslice_deps_ready(e, t->func_ptr.ret)) return false;
        for (uint32_t i = 0; i < t->func_ptr.param_count; i++)
            if (!xslice_deps_ready(e, t->func_ptr.params[i])) return false;
        return true;
    case TYPE_POOL:   return xslice_deps_ready(e, t->pool.elem);
    case TYPE_RING:   return xslice_deps_ready(e, t->ring.elem);
    case TYPE_DISTINCT: return xslice_deps_ready(e, t->distinct.underlying);
    case TYPE_VOID: case TYPE_BOOL: case TYPE_U8: case TYPE_U16: case TYPE_U32:
    case TYPE_U64: case TYPE_USIZE: case TYPE_I8: case TYPE_I16: case TYPE_I32:
    case TYPE_I64: case TYPE_F32: case TYPE_F64: case TYPE_ENUM: case TYPE_OPAQUE:
    case TYPE_ARENA: case TYPE_BARRIER: case TYPE_HANDLE: case TYPE_SLAB:
    case TYPE_SEMAPHORE: case TYPE_UINT: case TYPE_SINT:
        return true;
    }
    return true;
}

/* Emit every registered exotic slice typedef whose dependencies are satisfied.
 * Called before each pass-1 declaration and after pass 1 / the container
 * stamps, so a typedef lands before its first use and after the definitions
 * it needs. Iterates to a fixpoint so `[*][*]?u32` follows `[*]?u32`. */
static void flush_exotic_slices(Emitter *e) {
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < e->xslice_count; i++) {
            struct XSlice *x = &e->xslices[i];
            if (x->emitted || !xslice_deps_ready(e, x->elem)) continue;
            emit(e, "typedef struct { ");
            emit_ptr_to_elem_named(e, x->elem, false, "ptr");
            emit(e, "; size_t len; } _zer_xslice_%s;\n", x->suffix);
            emit(e, "typedef struct { ");
            emit_ptr_to_elem_named(e, x->elem, true, "ptr");
            emit(e, "; size_t len; } _zer_xvslice_%s;\n", x->suffix);
            emit(e, "typedef struct { _zer_xslice_%s value; uint8_t has_value; } _zer_xopt_slice_%s;\n",
                 x->suffix, x->suffix);
            x->emitted = true;
            progress = true;
        }
    }
}

/* Path C: after an arithmetic op, mask a uN/iN result to its bit width when the
 * carrier is wider than N (non-standard widths). Native 8/16/32/64/128 self-wrap.
 * uN -> bitmask; iN -> sign-extend (unsigned-shift then arithmetic >> to avoid
 * signed-shift UB). No-op for non-integer / native-width destinations. */
static void emit_intn_mask(Emitter *e, IRLocal *dst, const char *sp) {
    if (!dst || !dst->type) return;
    TypeKind k = type_dispatch_kind(dst->type);
    if (k != TYPE_UINT && k != TYPE_SINT) return;
    uint32_t nb = type_unwrap_distinct(dst->type)->intn.bits;
    if (nb == 8 || nb == 16 || nb == 32 || nb == 64 || nb == 128) return;
    uint32_t cw = (nb <= 8) ? 8 : (nb <= 16) ? 16 : (nb <= 32) ? 32 : (nb <= 64) ? 64 : 128;
    emit_indent(e);
    if (k == TYPE_UINT) {
        if (nb < 64)
            emit(e, "%s%.*s = (%s%.*s & 0x%llxULL);\n",
                 sp, (int)dst->name_len, dst->name,
                 sp, (int)dst->name_len, dst->name,
                 (unsigned long long)((1ULL << nb) - 1ULL));
        else
            emit(e, "%s%.*s = (%s%.*s & ((((unsigned __int128)1u) << %u) - 1u));\n",
                 sp, (int)dst->name_len, dst->name,
                 sp, (int)dst->name_len, dst->name, nb);
    } else {
        const char *su = (cw==8)?"uint8_t":(cw==16)?"uint16_t":(cw==32)?"uint32_t":(cw==64)?"uint64_t":"unsigned __int128";
        const char *ss = (cw==8)?"int8_t":(cw==16)?"int16_t":(cw==32)?"int32_t":(cw==64)?"int64_t":"__int128";
        uint32_t sh = cw - nb;
        emit(e, "%s%.*s = (%s)((%s)%s%.*s << %u) >> %u;\n",
             sp, (int)dst->name_len, dst->name,
             ss, su, sp, (int)dst->name_len, dst->name, sh, sh);
    }
}

/* Width-mask a non-native uN/iN LVALUE given as a printf-ready C string
 * (e.g. "(*_zer_np3)"). Emits `<lv> = wrap_to_N(<lv>); ` inline (trailing
 * space, no newline — for use inside a statement-expression). unsigned →
 * mask low N bits; signed → shift-left-then-arithmetic-shift-right to
 * sign-extend. No-op for native widths (8/16/32/64/128). Mirrors
 * emit_intn_mask (the IR_BINOP-temp masker) but for an arbitrary lvalue —
 * the assignment/compound-assign path is AST-passthrough and never reaches
 * emit_intn_mask, so odd-width stores need masking here (else silent wrong
 * value: `u3 y; y = a+b` would keep bit 3). */
/* Does this type CARRY an enum at any nesting depth? Emitter-side twin of
 * checker.c's `type_carries_enum` (that one is static, and the two files share
 * no header for these predicates). Same shape, same depth limit; keep them in
 * step — they answer the same question for the same rule, the checker deciding
 * whether to care and the emitter deciding where to look. */
static bool type_carries_enum_e(Type *t, int depth) {
    if (!t || depth > 32) return false;
    TypeKind k = type_dispatch_kind(t);
    Type *u = type_unwrap_distinct(t);
    if (!u) return false;
    if (k == TYPE_ENUM) return true;
    if (k == TYPE_OPTIONAL) return type_carries_enum_e(u->optional.inner, depth + 1);
    if (k == TYPE_ARRAY) return type_carries_enum_e(u->array.inner, depth + 1);
    if (k == TYPE_STRUCT) {
        for (uint32_t i = 0; i < u->struct_type.field_count; i++)
            if (type_carries_enum_e(u->struct_type.fields[i].type, depth + 1)) return true;
        return false;
    }
    if (k == TYPE_UNION) {
        for (uint32_t i = 0; i < u->union_type.variant_count; i++)
            if (type_carries_enum_e(u->union_type.variants[i].type, depth + 1)) return true;
        return false;
    }
    return false;
}

/* BUG-843: @bitcast could FORGE an out-of-variant enum, and the switch then
 * silently ran its LAST arm — `@bitcast(State, 7)` took `.done` while
 * `s == State.done` was false. It was the only route in: ZER has no int->enum
 * cast, so every other path to an enum value is a declared variant.
 *
 * Resolved by TRACKING, not banning (the Ban Decision Framework: none of the four
 * ban conditions applies). Reading an enum out of a hardware register field via
 * @bitcast is a legitimate firmware idiom; what is not legitimate is the value
 * silently becoming a variant it is not. The guard traps at the point of forgery
 * rather than letting a later switch pick an arbitrary arm. */
/* BUG-864 extends this in two directions that were both silent.
 *
 * (a) CARRIER. The guard was a TOP-LEVEL kind test, so `@bitcast(Box, 7)` where
 *     `struct Box { State s; }` returned early and forged `Box.s` exactly as
 *     effectively as the bare spelling. Same wrapper-hides-the-inner-kind family
 *     the audit_carrier_dispatch gate exists for. It now walks struct fields,
 *     optional payloads and array elements, checking every enum it reaches.
 * (b) SIBLING ROUTES. It was called only from the two @bitcast sites. @truncate
 *     (and @saturate / @cast) reach an enum target just as directly, and
 *     `@truncate(State, 7)` was accepted with no check at all. Every
 *     value-producing conversion into an enum-carrying target now calls it.
 *
 * `path` is the C lvalue expression to test; recursion appends `.field` /
 * `[i]` to it. Depth-limited like the type carriers in checker.c. */
/* BUG-929: `@try_enum(E, x)` -> `?E`, the CHECKED int-to-enum conversion.
 *
 * Emitted inline rather than as a runtime helper because the variant set is
 * per-enum. `?Enum` is `_zer_opt_i32` (enums are int32_t), so the statement
 * expression builds that struct directly. Temporaries are scoped by the statement
 * expression, so nesting `@try_enum` inside `@try_enum` shadows legally.
 *
 * Shared by BOTH emitter dispatch paths — the AST one and the IR-rewritten one.
 * CLAUDE.md: an intrinsic handled in only one path falls through to a placeholder
 * emission and segfaults at runtime, so the value expression is passed in as an
 * already-emitted callback rather than duplicating the body. */
/* BUG-1193: the membership test runs on the OPERAND's own value and type. It used
 * to narrow to int32 first, so `@try_enum(Gap, (u64)4294967306)` matched the
 * variant 10 and `@try_enum(Dir, (u32)0xFFFFFFFF)` matched -1 — the checked door
 * returning a variant for a value that is not one. `src` is the operand's type: a
 * variant is compared only when it is REPRESENTABLE in that type (a negative value
 * never matches an unsigned operand; 300 never matches a u8), and then exactly, in
 * the operand's type. Unknown type: fall back to the old int32 test. */
static void emit_try_enum_open(Emitter *e) {
    emit(e, "({ __auto_type _zer_tew = (");
}
static void emit_try_enum_close(Emitter *e, Type *t, Type *src) {
    Type *u = t ? type_unwrap_distinct(t) : NULL;
    Type *se = src ? type_unwrap_distinct(src) : NULL;
    int bits = (se && type_is_integer(se)) ? type_width(se) : 0;
    bool sgn = se && type_is_signed(se);
    emit(e, "); int32_t _zer_tev = (int32_t)_zer_tew; _zer_opt_i32 _zer_teo; "
            "_zer_teo.has_value = (0");
    if (u && type_dispatch_kind(u) == TYPE_ENUM) {
        for (uint32_t vi = 0; vi < u->enum_type.variant_count; vi++) {
            long long v = (long long)u->enum_type.variants[vi].value;
            if (bits <= 0) {                     /* operand type unknown */
                emit(e, " || _zer_tev == %lld", v);
                continue;
            }
            bool fits;
            if (sgn) {
                fits = bits >= 64 ||
                       (v >= -(1LL << (bits - 1)) && v <= (1LL << (bits - 1)) - 1);
            } else {
                fits = v >= 0 && (bits >= 63 || v <= (long long)((1ULL << bits) - 1));
            }
            if (!fits) continue;                 /* no value of the operand is v */
            emit(e, " || _zer_tew == (__typeof__(_zer_tew))%lldLL", v);
        }
    }
    emit(e, ") ? 1 : 0; _zer_teo.value = _zer_tev; _zer_teo; })");
}

static void emit_enum_variant_guard_path(Emitter *e, Type *t, const char *path,
                                         const char *what, int depth) {
    if (!t || depth > 8) return;
    Type *u = type_unwrap_distinct(t);
    if (!u) return;
    TypeKind k = type_dispatch_kind(u);
    if (k == TYPE_ENUM) {
        if (u->enum_type.variant_count == 0) return;
        emit(e, "if (!(");
        for (uint32_t vi = 0; vi < u->enum_type.variant_count; vi++) {
            if (vi) emit(e, " || ");
            emit(e, "%s == %lld", path, (long long)u->enum_type.variants[vi].value);
        }
        emit(e, ")) _zer_trap(\"%s produced a value that is not a declared variant "
                "of this enum\", __FILE__, __LINE__); ", what);
        return;
    }
    if (k == TYPE_STRUCT) {
        for (uint32_t i = 0; i < u->struct_type.field_count; i++) {
            Type *ft = u->struct_type.fields[i].type;
            if (!type_carries_enum_e(ft, 0)) continue;
            char sub[256];
            snprintf(sub, sizeof(sub), "%s.%.*s", path,
                     (int)u->struct_type.fields[i].name_len,
                     u->struct_type.fields[i].name);
            emit_enum_variant_guard_path(e, ft, sub, what, depth + 1);
        }
        return;
    }
    if (k == TYPE_OPTIONAL) {
        Type *in = u->optional.inner;
        if (!type_carries_enum_e(in, 0)) return;
        /* Only the payload of a PRESENT optional is meaningful; a null one
         * carries whatever the zeroing left, which is not a forged variant. */
        char sub[256];
        snprintf(sub, sizeof(sub), "%s.value", path);
        emit(e, "if (%s.has_value) { ", path);
        emit_enum_variant_guard_path(e, in, sub, what, depth + 1);
        emit(e, "} ");
        return;
    }
    if (k == TYPE_ARRAY) {
        Type *in = u->array.inner;
        if (!type_carries_enum_e(in, 0)) return;
        if (u->array.size == 0 || u->array.size > 4096) return;
        int li = e->temp_count++;
        char sub[256];
        snprintf(sub, sizeof(sub), "%s[_zer_egi%d]", path, li);
        emit(e, "for (size_t _zer_egi%d = 0; _zer_egi%d < %llu; _zer_egi%d++) { ",
             li, li, (unsigned long long)u->array.size, li);
        emit_enum_variant_guard_path(e, in, sub, what, depth + 1);
        emit(e, "} ");
        return;
    }
    /* A UNION is TAGGED in ZER, so its payload is only readable through the
     * variant switch, which re-checks the tag. Nothing to guard here. */
}

static void emit_bitcast_enum_guard(Emitter *e, Type *t, const char *lv) {
    if (!type_carries_enum_e(t, 0)) return;
    emit_enum_variant_guard_path(e, t, lv, "@bitcast", 0);
}

/* BUG-845: float -> integer conversion is C UNDEFINED when the truncated value is
 * not representable in the target (C11 6.3.1.4p1). Measured through ZER on one
 * emitted .c with one gcc: `f64 g = -1.5; (u32)g` prints 4294967295 at -O0 and 0
 * at -O2 — the same program, two answers, chosen by the optimiser. (A `volatile`
 * source SUPPRESSES the divergence, because that forces the cvttsd2si instruction
 * at every level while the bug lives in the CONSTANT-FOLDING path; probe with a
 * plain constant.) ARM saturates in hardware for a third answer.
 *
 * Guarded at every emission site with EXACT bounds — hex-float powers of two, no
 * rounding slop, so the guard never rejects a representable value. NaN fails every
 * comparison, so `!(in range)` traps on it too. */
static bool f2i_needs_guard(Type *src, Type *tgt) {
    if (!src || !tgt) return false;
    TypeKind sk = type_dispatch_kind(src);
    if (sk != TYPE_F32 && sk != TYPE_F64) return false;
    switch (type_dispatch_kind(tgt)) {
    case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_USIZE:
    case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
    case TYPE_UINT: case TYPE_SINT:
        return true;
    default: return false;
    }
}
static void f2i_bounds(Type *tgt, int *bits, bool *is_signed) {
    /* BUG-1064: width and signedness come from the SAME two queries every other
     * integer-width decision uses (type_width / type_is_signed), so u65..u128 /
     * i65..i128 and the enum carrier get their real bounds instead of a default. */
    Type *u = tgt ? type_unwrap_distinct(tgt) : NULL;
    *bits = u ? type_width(u) : 0;
    if (*bits <= 0) *bits = 32;
    if (*bits > 128) *bits = 128;
    *is_signed = u ? type_is_signed(u) : false;
}

/* BUG-1060..1064: the MIN or MAX of a ZER integer of `bits` width, as C
 * CONSTANT-EXPRESSION text of the matching C width.
 *
 * This is the ONE place a width's limits are spelled. Before it, five division
 * sites picked the MIN with an `8 / 16 / 32 / else-64` switch (so an i5, i12,
 * i48 or i128 compared against INT64_MIN and never trapped), two @saturate paths
 * computed `1LL << (w-1)` inside zerc (undefined for w > 64) and clamped every
 * width > 64 to the u64 maximum, and the float->int saturation gave up past 64
 * bits. Every one of them was the same question -- "what is the minimum / maximum
 * of this ZER width?" -- answered at N sites with N different coverages.
 *
 * Widths above 64 are expressed through `unsigned __int128`, which is what the
 * u65..u128 / i65..i128 carriers already are; `1 << 127` is formed UNSIGNED and
 * converted, so no signed shift overflows. `n` is the size of `buf`. */
static void int_limit_text(int bits, bool sg, bool want_max, char *buf, size_t n) {
    if (bits <= 0) bits = 32;
    if (bits > 128) bits = 128;
    if (!sg) {
        if (!want_max)        snprintf(buf, n, "0");
        else if (bits < 64)   snprintf(buf, n, "%lluULL", (1ULL << bits) - 1ULL);
        else if (bits == 64)  snprintf(buf, n, "18446744073709551615ULL");
        else if (bits < 128)  snprintf(buf, n, "((((unsigned __int128)1) << %d) - 1)", bits);
        else                  snprintf(buf, n, "(~(unsigned __int128)0)");
        return;
    }
    if (bits < 64) {
        long long m = (long long)(1ULL << (bits - 1));
        if (want_max) snprintf(buf, n, "%lldLL", m - 1);
        else          snprintf(buf, n, "(-%lldLL)", m);
        return;
    }
    if (bits == 64) {
        if (want_max) snprintf(buf, n, "9223372036854775807LL");
        else          snprintf(buf, n, "(-9223372036854775807LL - 1)");
        return;
    }
    if (want_max) snprintf(buf, n, "((__int128)((((unsigned __int128)1) << %d) - 1))", bits - 1);
    else          snprintf(buf, n, "(-((__int128)((((unsigned __int128)1) << %d) - 1)) - 1)", bits - 1);
}

/* The MIN of a SIGNED integer type, for the `MIN / -1` division trap. Every
 * division site asks this through here (BUG-1062). */
static void signed_min_text(Type *t, char *buf, size_t n) {
    Type *u = t ? type_unwrap_distinct(t) : NULL;
    int w = u ? type_width(u) : 0;
    int_limit_text(w, true, false, buf, n);
}

/* BUG-1065: the width a shift COUNT is compared against -- the ZER width of the
 * LEFT operand, passed to `_zer_shl` / `_zer_shr` as their third argument.
 *
 * The macros used to take the width from `sizeof(a) * 8`, the C CARRIER, which
 * is right only when the carrier is exactly the ZER type. It is not for an iN
 * (`i5` lives in an `int8_t`, so `x >> 5` .. `x >> 7` passed the guard and an
 * arithmetic shift of a negative value gave -1, where ZER's rule is 0) and it is
 * not for a narrow operand C has PROMOTED (`(x / y) >> 8` on an i8, whose left
 * side is emitted as a statement expression of type `int`, so the guard saw 32
 * and the shift gave -1). The macro still ALSO tests the carrier width, so an
 * unknown width (0 -> 128 here) can never make a C shift undefined. */
static int shift_guard_width(Type *lhs) {
    Type *u = lhs ? type_unwrap_distinct(lhs) : NULL;
    int w = (u && type_is_integer(u)) ? type_width(u) : 0;
    return w > 0 ? w : 128;
}

/* The four saturation limits, as C constant-expression text. Extracted so the
 * statement-expression form (emit_f2i_close) and the CONSTANT form
 * (emit_f2i_const, BUG-990) cannot drift apart — the bounds ARE the safety
 * property, and two copies of them is the multi-site shape this codebase keeps
 * getting bitten by. `n` is the size of EACH buffer. */
static void f2i_limits(int bits, bool sg, char *lo, char *hi, char *mn, char *mx,
                       size_t n) {
    /* lo / hi are EXACT hex-float powers of two, so they are exact as doubles for
     * every width up to 128 (0x1p128 ~ 3.4e38 is far inside the double range).
     * The signed `lo` is `-2^(b-1) - 1`; past 53 bits the `- 1` rounds away and
     * `lo` equals MIN itself, which is still the right boundary (`<= MIN` clamps
     * to MIN, anything above truncates in range). */
    if (sg) {
        snprintf(lo, n, "-0x1p%d - 1.0", bits - 1);
        snprintf(hi, n, "0x1p%d", bits - 1);
    } else {
        snprintf(lo, n, "-1.0");
        snprintf(hi, n, "0x1p%d", bits);
    }
    int_limit_text(bits, sg, false, mn, n);
    int_limit_text(bits, sg, true, mx, n);
}

/* Emits `({ <srcT> _v = ` — caller emits the source, then calls _close. */
static void emit_f2i_open(Emitter *e, Type *src, int tmp) {
    emit(e, "({ ");
    emit_type(e, src);
    emit(e, " _zer_f2i%d = ", tmp);
}
static void emit_f2i_close(Emitter *e, Type *tgt, int tmp) {
    int bits; bool sg;
    f2i_bounds(tgt, &bits, &sg);
    /* BUG-883: float -> integer out of range is DEFINED as SATURATING, with NaN -> 0.
     *
     * It was UNDEFINED (C11 6.3.1.4p1) and GCC exploited it — one emitted .c, one
     * compiler, `(u32)(-1.5)` gave 4294967295 at -O0 and 0 at -O2. BUG-845 removed
     * the UB by TRAPPING; this replaces that with a value, which is the owner's call
     * and matches what Rust settled on for `as` casts in 1.45 for the same reason.
     *
     * Why a value rather than a halt, in ZER's own terms: this codebase already
     * draws the line at MEMORY vs ARITHMETIC. Memory violations halt (slice OOB,
     * misaligned @inttoptr, a bad @pun); arithmetic results get DEFINED values —
     * integer overflow WRAPS rather than trapping. A float that does not fit is
     * arithmetic, so it belongs on the defined-value side. `@saturate` already names
     * exactly these semantics in the language, so the plain cast now agrees with the
     * primitive instead of contradicting it.
     *
     * NaN is tested FIRST and explicitly: every comparison against NaN is false, so
     * without `_v != _v` it would fall through the range tests into the raw cast —
     * the exact UB being removed. Bounds are exact hex-float powers of two, so no
     * representable value is ever clamped. */
    /* BUG-1064: every width 1..128 saturates. u65..u128 / i65..i128 used to keep
     * only a NaN trap and then a RAW cast -- undefined for +-inf and any value
     * outside the range, so `(u128)1e39` was whatever GCC chose. The bounds ARE
     * expressible at that width: `int_limit_text` spells them through
     * `unsigned __int128`, and the hex-float boundaries are exact doubles.
     * The statement expression emit_f2i_open began ends with the `})` below
     * (tools/audit_walker_fields.sh counts braces in comments and strings, and
     * this function used to carry that text twice, once per width branch). */
    {
        char lo[96], hi[96], mn[96], mx[96];
        f2i_limits(bits, sg, lo, hi, mn, mx, sizeof lo);
        emit(e, "; (_zer_f2i%d != _zer_f2i%d) ? (", tmp, tmp);
        emit_type(e, tgt); emit(e, ")0 : (_zer_f2i%d <= (%s)) ? (", tmp, lo);
        emit_type(e, tgt); emit(e, ")%s : (_zer_f2i%d >= (%s)) ? (", mn, tmp, hi);
        emit_type(e, tgt); emit(e, ")%s : (", mx);
        emit_type(e, tgt); emit(e, ")_zer_f2i%d; })", tmp);
    }
}

/* BUG-1060/1061: @saturate's clamp -- ONE implementation, used by BOTH
 * dispatch paths (the AST `emit_expr` handler and the IR `emit_rewritten_node`
 * handler). The two paths used to compute the clamp separately, and both were
 * wrong in different ways:
 *
 *   - an UNSIGNED 64-bit source into a SIGNED target compared `u64 < -128LL`.
 *     C's usual arithmetic conversions make that an UNSIGNED comparison (the
 *     -128 becomes 2^64-128), so every value clamped to MIN:
 *     `@saturate(i32, s.len)` for a length of 10 gave INT32_MIN.
 *   - a FLOAT source had no NaN test (NaN fell through to the raw C cast, which
 *     is undefined), and the integer MAX rounded UP as a double, so 2^63 into
 *     i64 and 2^64 into u64 were not "> MAX" and hit the raw cast too.
 *   - targets wider than 64 bits clamped to the u64 maximum, and the IR path
 *     evaluated `1LL << (w - 1)` inside zerc -- undefined for w > 64.
 *
 * The integer clamp below never compares a value against a NEGATIVE constant
 * unless the value is itself negative (so it is signed): the sign test comes
 * first, and against a non-negative constant C's conversions are exact for every
 * operand type. So the source type is not needed at all -- the form is correct
 * for u8..u128 and i8..i128 alike. A float source goes through the float->int
 * saturation the `(T)x` cast already uses (`emit_f2i_*`, BUG-883), which tests
 * NaN first and uses exact hex-float boundaries. */
static bool saturate_src_is_float(Emitter *e, Node *arg) {
    Type *st = arg ? checker_get_type(e->checker, arg) : NULL;
    return st && type_is_float(st);
}
static void emit_saturate_open(Emitter *e, Node *arg, int tmp) {
    if (saturate_src_is_float(e, arg)) {
        emit_f2i_open(e, type_unwrap_distinct(checker_get_type(e->checker, arg)), tmp);
        return;
    }
    emit(e, "({ __auto_type _zer_sat%d = ", tmp);
}
static void emit_saturate_close(Emitter *e, Node *arg, Type *t, int tmp) {
    if (saturate_src_is_float(e, arg)) {
        emit_f2i_close(e, t, tmp);
        return;
    }
    int bits; bool sg;
    f2i_bounds(t, &bits, &sg);
    char mn[96], mx[96];
    int_limit_text(bits, sg, false, mn, sizeof mn);
    int_limit_text(bits, sg, true, mx, sizeof mx);
    emit(e, "; (_zer_sat%d < 0) ? ", tmp);
    if (sg) {
        emit(e, "((_zer_sat%d < %s) ? (", tmp, mn);
        emit_type(e, t); emit(e, ")%s : (", mn);
        emit_type(e, t); emit(e, ")_zer_sat%d)", tmp);
    } else {
        emit(e, "(");
        emit_type(e, t); emit(e, ")0");
    }
    emit(e, " : ((_zer_sat%d > %s) ? (", tmp, mx);
    emit_type(e, t); emit(e, ")%s : (", mx);
    emit_type(e, t); emit(e, ")_zer_sat%d); })", tmp);
}

/* BUG-990: the same saturation as a C CONSTANT EXPRESSION — no statement
 * expression, so it is legal in a STATIC INITIALIZER.
 *
 *     u32 g = (u32)1e20;
 *
 * is a legal ZER global, and the checker accepts it, but the emitter reached the
 * BUG-883 guard through the AST path and wrote `uint32_t g = ({ ... });` at file
 * scope. GCC then said *"braced-group within expression allowed only inside a
 * function"*, pointing (via `#line`) at the user's own `.zer` line. The ZER
 * program is correct; only the emission shape was illegal there. Same class as
 * the `inf` literal above: the compiler's own output is what fails, and no ZER
 * diagnostic ever names the cause.
 *
 * The operand is emitted FOUR times, so this form is used ONLY when re-evaluating
 * it is free and observationally identical: no side effects (`expr_has_side_effects`)
 * and not volatile (`expr_is_volatile` — a repeated MMIO read is a hardware event,
 * not a free re-read). A global initializer is a constant expression by the
 * checker's own rule, so it always qualifies; anything that does not qualify keeps
 * the statement-expression form, which is only reachable inside a function where
 * that form is legal.
 *
 * Bounds come from the SAME `f2i_limits` the statement form uses — one source of
 * truth for the four limits. */
static bool emit_f2i_const(Emitter *e, Type *tgt, Node *operand) {
    int bits; bool sg;
    f2i_bounds(tgt, &bits, &sg);
    if (bits <= 0 || bits > 128) return false;
    if (!operand) return false;
    if (expr_has_side_effects(operand) || expr_is_volatile(e, operand)) return false;

    char lo[96], hi[96], mn[96], mx[96];
    f2i_limits(bits, sg, lo, hi, mn, mx, sizeof lo);
    #define F2I_OP() do { emit(e, "("); emit_expr(e, operand); emit(e, ")"); } while (0)
    emit(e, "(");
    F2I_OP(); emit(e, " != "); F2I_OP(); emit(e, " ? (");
    emit_type(e, tgt); emit(e, ")0 : ");
    F2I_OP(); emit(e, " <= (%s) ? (", lo);
    emit_type(e, tgt); emit(e, ")%s : ", mn);
    F2I_OP(); emit(e, " >= (%s) ? (", hi);
    emit_type(e, tgt); emit(e, ")%s : (", mx);
    emit_type(e, tgt); emit(e, ")");
    F2I_OP();
    emit(e, ")");
    #undef F2I_OP
    return true;
}

/* BUG-851: five emitter GIVE-UP paths emitted a comment plus a placeholder when the
 * emitter reached a shape it could not lower. Not a live bug — a live RISK, of exactly
 * the class tools/emit_audit.sh exists to guard, sitting in the emitter itself and not
 * covered by that script.
 *
 * The three CALLEE ones are the worst: a bare comment followed by "(a, b)" is a valid
 * C COMMA EXPRESSION evaluating to `b`, so the program compiles, runs, and CALLS
 * NOTHING — no diagnostic, no crash, wrong behaviour. The other two substitute a
 * literal zero for an expression. All five are silent miscompiles, and none of their
 * fingerprints was in the audit's pattern list.
 *
 * MEASURED BEFORE CHANGING ANYTHING: 0 of 1146 corpus programs emit any of the five
 * markers, so nothing reachable becomes an abort. Why the audit could not have caught
 * them regardless: it compiles 5 hand-picked samples and greps 4 fingerprints, so a
 * give-up in a shape none of those contains is invisible even with the right patterns
 * added. Making the emitter LOUD does not depend on a sample ever exercising the path. */
static void emit_unreachable(Emitter *e, const char *what, Node *n) {
    (void)e;
    fprintf(stderr,
            "INTERNAL ERROR: emitter cannot lower %s%s%s — please report with a minimal "
            "reproducer.\n", what,
            n ? " at line " : "", n ? "" : "");
    if (n) fprintf(stderr, "  (source line %d)\n", n->loc.line);
    abort();
}

/* ================================================================
 * BUG-1019 — the null-function-pointer guard
 *
 * ZER promises a non-null `*T`, and for a SCALAR funcptr it keeps that promise
 * structurally: `B f;` and `B g;` at global scope are both rejected ("function
 * pointer requires an initializer", BUG-866). But auto-zero fills an ARRAY
 * ELEMENT and a STRUCT FIELD of funcptr type with NULL, and neither carrier is
 * covered by that rule — ZER has no array-initializer syntax, so extending the
 * rule through the array was implemented and REVERTED (it would have removed the
 * dispatch-table idiom rather than initialise it; see tests/zer_gaps/
 * funcptr_array_null_element.zer for that history).
 *
 * So the value really can be NULL, and until now the emitted C was a raw
 * indirect call: `_zer_t3 = g_ops[0](_zer_t1, _zer_t2);`. HOSTED, that faults
 * and ZER's SIGSEGV handler prints a trap — which is why it looked handled. On
 * BARE METAL with no MMU there is no handler and no fault: address 0 is the
 * reset vector or ordinary memory, and the jump silently goes somewhere. That is
 * the silent-on-target class: missed at compile time AND at run time.
 *
 * TRACK, do not ban — the Ban Decision Framework's answer, and the one the gap
 * file names as fix #1. One predictable branch before an indirect call; GCC
 * elides it wherever it can see the assignment.
 *
 * WHICH CALLS. Every call whose callee is not a DIRECT function name. Not an
 * enumeration of the null-producing carriers — enumerating them is the
 * multi-site shape that keeps leaking here (a bare `B f = g_ops[0];` launders an
 * array element's NULL into a scalar name, so "only guard INDEX and FIELD"
 * would already be wrong). Unless the callee resolves to a function symbol, it
 * is guarded.
 *
 * SINGLE EVALUATION. `__typeof__` does not evaluate its operand, so emitting the
 * callee text twice — once inside `__typeof__`, once as the initialiser — still
 * evaluates it exactly once at run time. That matters: a callee like
 * `ops[next()]` must not run `next()` twice (the RF13 / BUG-661 double-eval
 * class).
 * ================================================================ */

/* BUG-1215: the TYPE operand of @offset when the parser left it as an identifier.
 * Writing `struct <name>` literally was wrong for every name that is not the C
 * struct tag: a typedef (`typedef A AA;` -> `struct AA`, undefined), a distinct
 * typedef, a union, a module-mangled struct. Resolve the name to its type and emit
 * that — ONE helper for both emitter paths. */
static void emit_offset_type_operand(Emitter *e, Node *name_node) {
    Type *t = e->checker ? checker_get_type(e->checker, name_node) : NULL;
    if ((!t || type_dispatch_kind(t) == TYPE_VOID) && e->checker && name_node &&
        name_node->kind == NODE_IDENT) {
        Symbol *s = scope_lookup(e->checker->global_scope, name_node->ident.name,
                                 (uint32_t)name_node->ident.name_len);
        if (s) t = s->type;
    }
    TypeKind k = type_dispatch_kind(t);
    if (k == TYPE_STRUCT || k == TYPE_UNION) { emit_type(e, type_unwrap_distinct(t)); return; }
    emit(e, "struct %.*s", name_node && name_node->kind == NODE_IDENT ?
         (int)name_node->ident.name_len : 0,
         name_node && name_node->kind == NODE_IDENT ? name_node->ident.name : "");
}

/* Does `callee` name a function DIRECTLY (so the call is not indirect)? */
static bool callee_is_direct_function(Emitter *e, Node *callee) {
    if (!callee || callee->kind != NODE_IDENT) return false;
    if (!e->checker) return false;
    Symbol *s = scope_lookup(e->checker->global_scope, callee->ident.name,
                             (uint32_t)callee->ident.name_len);
    if (s && s->is_function) return true;
    /* BUG-1211: a module's `static` function is registered only under its
     * mangled key `<module>__<name>`, so the raw lookup missed it and a plain
     * call to it was wrapped in the funcptr null guard —
     * `__typeof__(m__helper) _zer_fp0 = m__helper;` declares a FUNCTION, and GCC
     * refuses to initialise one ("initialized like a variable"). */
    if (e->current_module) {
        uint32_t nl = (uint32_t)callee->ident.name_len;
        uint32_t mkl = e->current_module_len + 2 + nl;
        char *mk = (char *)arena_alloc(e->arena, mkl + 1);
        if (mk) {
            memcpy(mk, e->current_module, e->current_module_len);
            mk[e->current_module_len] = '_';
            mk[e->current_module_len + 1] = '_';
            memcpy(mk + e->current_module_len + 2, callee->ident.name, nl);
            mk[mkl] = '\0';
            Symbol *ms = scope_lookup_local(e->checker->global_scope, mk, mkl);
            if (ms && ms->is_function) return true;
        }
    }
    return false;
}

/* Is this call an INDIRECT call through a non-optional function pointer?
 * An OPTIONAL funcptr (`?B`) is not guarded here: it cannot be called without
 * being unwrapped first, and the unwrap is what proves it non-null. */
static bool call_needs_null_funcptr_guard(Emitter *e, Node *callee) {
    if (!callee) return false;
    if (callee_is_direct_function(e, callee)) return false;
    Type *t = e->checker ? checker_get_type(e->checker, callee) : NULL;
    return t && type_dispatch_kind(t) == TYPE_FUNC_PTR;
}

/* ================================================================
 * BUG-1152 — the non-null pointer LOAD guard
 *
 * ZER's `*T` is non-null, and for a SCALAR the promise is structural: `*u32 p;`
 * is refused (nonnull_zero_hole, BUG-866/893). But auto-zero writes NULL into a
 * `*T` STRUCT FIELD and a `*T` ARRAY ELEMENT wherever the aggregate is
 * zero-initialised — `H w;`, `H[2] hs;`, every `alloc(T)` / Pool / Slab / Arena
 * slot — and nothing refused a read of it. Hosted, the dereference faulted into
 * the SIGSEGV trap; on BARE METAL address 0 is ordinary memory (the initial SP on
 * Cortex-M) and the read returned a wrong value, the write corrupted it. Silent
 * at compile time AND at run time on the target that matters.
 *
 * TRACK, do not ban (option (c) of the limitations entry; the BUG-1019 precedent
 * for funcptrs). Refusing every struct with a non-null field wherever it is
 * zero-initialised would refuse `alloc(T)` of it outright (the BUG-893 author
 * measured 34 corpus files); a definite-initialisation analysis is a new
 * subsystem. The guard is ONE predictable branch.
 *
 * WHERE: at the LOAD, not the dereference. Every `*T` that reaches a local, a
 * param or an operand arrives by one of three loads out of memory — a FIELD, an
 * array/slice ELEMENT, or a DEREF of `**T` — and a local / param `*T` is non-null
 * by construction (it needs an initialiser). Guarding the three loads therefore
 * makes every `*T` value non-null, so no dereference site needs a check and no
 * laundering route (`*u32 q = h.p; *q`) exists — the reason BUG-1019 could not
 * guard only INDEX/FIELD callees does not arise, because the guard is on the
 * load that produces the value, not on its use.
 *
 * NOT a load: the assignment TARGET (`h.p = &x`) and the `&` operand (`&h.p`) —
 * both name the location. A global initialiser (file scope) cannot contain a
 * statement expression and cannot read a field value anyway.
 *
 * Emission: `({ __auto_type _zer_nnN = LOAD; if (!_zer_nnN) _zer_trap(...);
 * _zer_nnN; })` — LOAD text once, so a nested chain stays linear, and
 * `__auto_type` drops only the TOP-level qualifier of the pointer value (the
 * pointee's const/volatile are part of the type and are kept). The IR's own
 * decomposed loads (IR_FIELD_READ / IR_INDEX_READ / IR_UNOP deref) get the same
 * check as a statement after the load: emit_nonnull_local_check.
 * ================================================================ */
#define ZER_NN_TRAP_MSG "read of a null non-null pointer (a *T field or element that was zero-initialized and never assigned)"

/* BUG-1175: an enum with NO variant whose value is 0. Auto-zero forges a
 * non-variant value of it wherever it is zero-initialised inside an aggregate
 * (a bare `E g;` is refused, BUG-930; a struct field, an array element, an
 * alloc(T) / Pool slot and an omitted designated-init field are not), and the
 * exhaustive switch then takes an ARM for it. Such a value is guarded at the
 * same LOAD sites as a non-null pointer: the zero is the only value auto-zero
 * can produce, and every other forge is guarded at its door. */
static bool enum_lacks_zero_variant(Type *t) {
    Type *u = t ? type_unwrap_distinct(t) : NULL;
    if (!u || type_dispatch_kind(u) != TYPE_ENUM || u->enum_type.variant_count == 0) return false;
    for (uint32_t i = 0; i < u->enum_type.variant_count; i++)
        if (u->enum_type.variants[i].value == 0) return false;
    return true;
}

/* What a guarded load must check: 0 = nothing, 1 = a non-null pointer,
 * 2 = a no-zero-variant enum. */
static int load_guard_kind(Type *t) {
    if (!t) return 0;
    if (type_dispatch_kind(t) == TYPE_POINTER) return 1;
    if (enum_lacks_zero_variant(t)) return 2;
    return 0;
}

static bool node_is_nonnull_ptr_load(Emitter *e, Node *n) {
    if (!n || !e->checker) return false;
    if (n->kind == NODE_UNARY) {
        if (n->unary.op != TOK_STAR) return false;
    } else if (n->kind != NODE_FIELD && n->kind != NODE_INDEX) {
        return false;
    }
    Type *t = checker_get_type(e->checker, n);
    if (!t) return false;
    /* An enum's `.variant` spelling is a NODE_FIELD on the enum TYPE, not a
     * load — only a field of a value/pointer object is. */
    if (n->kind == NODE_FIELD) {
        Type *ot = checker_get_type(e->checker, n->field.object);
        if (ot && type_dispatch_kind(ot) == TYPE_ENUM) return false;
    }
    return load_guard_kind(t) != 0;
}

/* Should this node's emission be wrapped in the load guard? */
static bool nn_guard_wanted(Emitter *e, Node *n) {
    if (!n || n == e->nn_bypass || n == e->nn_lvalue) return false;
    if (e->global_init_depth > 0) return false;
    return node_is_nonnull_ptr_load(e, n);
}

/* The location a node WRITES (or names), if it has one: an assignment target,
 * an `&` operand. Emitted as an lvalue, so never load-guarded. */
static Node *nn_lvalue_of(Node *n) {
    if (!n) return NULL;
    if (n->kind == NODE_ASSIGN) return n->assign.target;
    if (n->kind == NODE_UNARY && n->unary.op == TOK_AMP) return n->unary.operand;
    return NULL;
}

/* `*opaque` is the VALUE struct `_zer_opaque {ptr, type_id}`, not a C
 * pointer, so its null test reads `.ptr`. */
static const char *nn_null_member(Type *t) {
    Type *pt = t ? type_unwrap_distinct(t) : NULL;
    if (pt && type_dispatch_kind(pt) == TYPE_POINTER && pt->pointer.inner &&
        type_dispatch_kind(pt->pointer.inner) == TYPE_OPAQUE)
        return ".ptr";
    return "";
}
#define ZER_ENUM0_TRAP_MSG "read of an enum holding 0, which is not one of its variants (a zero-initialized field or element that was never assigned)"


/* The statement form, after an IR load into a local. */
static void emit_nonnull_local_check(Emitter *e, IRFunc *func, int local_id);

static void emit_intn_mask_lv(Emitter *e, Type *t, const char *lv) {
    if (!t) return;
    TypeKind k = type_dispatch_kind(t);
    if (k != TYPE_UINT && k != TYPE_SINT) return;
    uint32_t nb = type_unwrap_distinct(t)->intn.bits;
    if (nb == 8 || nb == 16 || nb == 32 || nb == 64 || nb == 128) return;
    uint32_t cw = (nb <= 8) ? 8 : (nb <= 16) ? 16 : (nb <= 32) ? 32 : (nb <= 64) ? 64 : 128;
    if (k == TYPE_UINT) {
        if (nb < 64)
            emit(e, "%s = (%s & 0x%llxULL); ", lv, lv,
                 (unsigned long long)((1ULL << nb) - 1ULL));
        else
            emit(e, "%s = (%s & ((((unsigned __int128)1u) << %u) - 1u)); ", lv, lv, nb);
    } else {
        const char *su = (cw==8)?"uint8_t":(cw==16)?"uint16_t":(cw==32)?"uint32_t":(cw==64)?"uint64_t":"unsigned __int128";
        const char *ss = (cw==8)?"int8_t":(cw==16)?"int16_t":(cw==32)?"int32_t":(cw==64)?"int64_t":"__int128";
        uint32_t sh = cw - nb;
        emit(e, "%s = (%s)((%s)%s << %u) >> %u; ", lv, ss, su, lv, sh, sh);
    }
}

/* True if t is a non-native-width uN/iN scalar (needs post-store masking). */
static bool type_is_nonnative_intn(Type *t) {
    if (!t) return false;
    TypeKind k = type_dispatch_kind(t);
    if (k != TYPE_UINT && k != TYPE_SINT) return false;
    uint32_t nb = type_unwrap_distinct(t)->intn.bits;
    return !(nb == 8 || nb == 16 || nb == 32 || nb == 64 || nb == 128);
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
    case TYPE_UINT:   emit_intn_carrier(e, t->intn.bits, false); break;
    case TYPE_SINT:   emit_intn_carrier(e, t->intn.bits, true); break;
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
        case TYPE_SLICE:
            /* ?[*]T → _zer_opt_slice_T / _zer_xopt_slice_<mangled> (BUG-1027: one query) */
            emit_slice_name(e, opt_inner->slice.inner, false, true);
            break;
        /* Stage 2 Part B (2026-04-28): exhaustive — fallback to
         * anonymous struct for any TYPE_KIND not handled above
         * (rare: ?FuncPtr, ?array, etc.). */
        case TYPE_OPTIONAL: case TYPE_ARRAY:
        case TYPE_FUNC_PTR: case TYPE_OPAQUE: case TYPE_POOL:
        case TYPE_RING: case TYPE_ARENA: case TYPE_BARRIER:
        case TYPE_SLAB: case TYPE_SEMAPHORE: case TYPE_DISTINCT:
        /* G1: ?uN → the carrier's named optional typedef, so every `?u5`
         * instance is ONE compatible C type. */
        case TYPE_UINT:
            emit(e, "_zer_opt_%s", intn_carrier_suffix(opt_inner->intn.bits, false));
            break;
        case TYPE_SINT:
            emit(e, "_zer_opt_%s", intn_carrier_suffix(opt_inner->intn.bits, true));
            break;
        }
        break;

    case TYPE_SLICE:
        /* [*]T / volatile [*]T → the named typedef, for EVERY element kind (BUG-1027) */
        emit_slice_name(e, t->slice.inner, t->slice.is_volatile, false);
        break;

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
    /* BUG-1220: a DISTINCT array type needs the array declarator too — `distinct
     * typedef u8[4] Quad; Quad q;` was emitted `uint8_t[4] q`, which is not C. */
    if (type_dispatch_kind(t) == TYPE_ARRAY) t = type_unwrap_distinct(t);

    if (t->kind == TYPE_ARRAY) {
        /* collect all array dimensions, emit base type + name + all dims */
        Type *base = t;
        while (base->kind == TYPE_ARRAY) base = base->array.inner;
        /* function pointer array: ret (*name[dim1][dim2])(params) */
        Type *base_eff = type_unwrap_distinct(base);
        /* BUG-879: peel a NULL-SENTINEL optional too. `?FuncPtr` IS the pointer
         * at runtime (no `.has_value` field), so an array of them is an array of
         * function pointers and needs this declarator shape — but the peel here
         * only handled `distinct`, so the element kind read as TYPE_OPTIONAL,
         * this branch was skipped, and the generic branch below emitted
         * `uint32_t (*)(uint32_t) ops[3]`: an abstract declarator with a name
         * glued on. Not C, and GCC said so at a line in generated code.
         *
         * It matters because the nullable form is the ONLY inhabitable one: an
         * array of NON-null funcptrs cannot be used, since auto-zero fills it
         * with NULL and ZER has no array initializer to fill it with anything
         * else. So every working funcptr array reaches exactly this path. */
        while (base_eff && type_dispatch_kind(base_eff) == TYPE_OPTIONAL &&
               is_null_sentinel(base_eff->optional.inner))
            base_eff = type_unwrap_distinct(base_eff->optional.inner);
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

/* BUG-1032: wrap a FOLDED integer value into the width/signedness the checker
 * gave the folded EXPRESSION. The untyped evaluator computes in int64, so
 * `const u8 B = 200; const u32 P = B + 100;` folded to 300 at file scope while
 * the same expression in a function is a u8 add and gives 44 — one program, two
 * values for one expression, decided by where it was written. The local path
 * wraps by assigning into a u8 IR temp; this is the file-scope twin of that. */
static int64_t fold_wrap_to_type(int64_t v, Type *t) {
    Type *eff = t ? type_unwrap_distinct(t) : NULL;
    if (!eff || !type_is_integer(eff)) return v;
    int w = type_width(eff);
    if (w <= 0 || w >= 64) return v;
    uint64_t mask = (1ULL << w) - 1ULL;
    uint64_t u = (uint64_t)v & mask;
    if (type_is_signed(eff) && (u >> (w - 1)) & 1ULL)
        return (int64_t)(u | ~mask);          /* sign-extend */
    return (int64_t)u;
}

/* BUG-939: emit an integer literal AT ITS RESOLVED WIDTH.
 *
 * Both AST literal emitters printed a bare `%llu` for any value fitting in 32 bits,
 * so a literal the checker had RETYPED to i64/u64 still reached C as an `int` — and
 * the arithmetic around it was then done in `int`:
 *
 *     i64 v = -(1 << 40);      // var-decl: IR temps are typed, correct
 *     i64 a;  a = -(1 << 40);  // assign:  emitted inline -> `-_zer_shl(1, 40)`
 *
 * LIT-1's retype is only half the fix; without a width suffix the retyped node still
 * prints as a 32-bit constant. Shared by BOTH emitters so the two spellings cannot
 * drift apart again — same reason emit_try_enum_open/close is shared. */
/* BUG-1063: the SIGNEDNESS comes from the type too, not only the width. A
 * literal the checker typed u32 (the default for every literal that fits 32 bits)
 * was printed bare, so C typed it `int` -- and every operation around it on the
 * AST-passthrough path (`x = ...`, `s.f = ...`, `g = ...`, an index) ran in
 * SIGNED 32-bit arithmetic while the IR var-decl path, whose temps are
 * `uint32_t`, ran it unsigned:
 *
 *     u32 v = (1 << 31) >> 28;   // 8           (IR temps, unsigned)
 *     r.m   = (1 << 31) >> 28;   // 4294967288  (bare `1`: INT_MIN >> 28 sign-extends)
 *     g     = ~0 >> 28;          // 4294967295  (~0 is -1, arithmetic shift)
 *     z = (2000000000 + 2000000000) % 7;   // signed overflow, then a negative %
 *     arr[~0 >> 28]              // checker folded 15, the program indexed -1 -> trap
 *
 * One literal, two C types, two answers. The rendering is now a function of the
 * literal's CHECKER TYPE alone -- the same type ir_lower gives the IR literal's
 * destination temp -- so both paths compute the expression at one width and one
 * signedness. Signed literals of <= 32 bits stay bare: a bare decimal is a C
 * `int` when it fits, and when it does not (the `2147483648` under a unary minus
 * in `-2147483648`) C widens it to `long` instead of wrapping it, which is the
 * value the source denotes. u8 / u16 stay bare as well: C promotes them to `int`
 * whatever their spelling. */
static void emit_int_literal(Emitter *e, Node *node) {
    unsigned long long v = (unsigned long long)node->int_lit.value;
    Type *lt = checker_get_type(e->checker, node);
    Type *eff = lt ? type_unwrap_distinct(lt) : NULL;
    int w = (eff && type_is_integer(eff)) ? type_width(eff) : 0;
    bool sg = w > 0 && type_is_signed(eff);
    if (v > 0xFFFFFFFFULL || w > 32) {
        /* The width comes from the TYPE, not the value: a small literal inside a
         * 64-bit expression must still be 64-bit or the operation wraps at 32. A
         * SIGNED 64-bit literal is `LL` so a comparison against a negative signed
         * value is not silently made unsigned. */
        emit(e, "%llu%s", v, (sg && v <= 0x7FFFFFFFFFFFFFFFULL) ? "LL" : "ULL");
        return;
    }
    if (w > 16 && !sg) { emit(e, "%lluU", v); return; }
    emit(e, "%llu", v);
}

/* BUG-1157: does this struct literal name an ARRAY-typed field? C cannot
 * initialise an array member from an array expression inside a compound
 * literal — the value decays to a pointer (`.arr = l` put an address in
 * arr[0]). Such a literal is emitted as a statement expression: the literal
 * without its array fields, then one memcpy per array field. */
static bool struct_init_names_array_field(Type *si_type, Node *node) {
    if (!si_type || !node || node->kind != NODE_STRUCT_INIT) return false;
    for (int i = 0; i < node->struct_init.field_count; i++) {
        Type *ft = struct_field_type_by_name(si_type, node->struct_init.fields[i].name,
                                             (uint32_t)node->struct_init.fields[i].name_len);
        if (ft && type_dispatch_kind(ft) == TYPE_ARRAY) return true;
    }
    return false;
}

/* BUG-1162/1163/1164: the ONE store path for a non-native-width uN/iN target,
 * shared by the AST (emit_expr) and IR-rewritten (emit_rewritten_node) paths —
 * they were two copies of the same intercept and had the same three defects:
 *   - it ran BEFORE the union-variant case, so `w.t = 5` into a `u3` variant
 *     never updated `_tag` (switch took the other arm; with a pointer variant
 *     beside it, a forged pointer) — the tag is now set here;
 *   - it ran before the bit-slice SET case, so `x[2..1] = 3` on a uN became
 *     `&(bit-extract rvalue)` — invalid C; a bit-slice target is left to its case;
 *   - it STORED the unmasked value and then re-masked through the same lvalue:
 *     two stores and an extra load, so a `volatile u12` register saw an
 *     out-of-field bit written transiently. The value is now computed and
 *     masked in a carrier temp and stored ONCE.
 * Returns false when the target is not this function's business. */
typedef void (*EmitNodeFn)(Emitter *, Node *, IRFunc *);
static void emit_bitslice_set(Emitter *e, Node *node, IRFunc *func, EmitNodeFn sub);   /* BUG-1198 */
static bool lvalue_through_packed(Emitter *e, Node *lv);   /* BUG-1324 */
static void emit_node_via_ast(Emitter *e, Node *n, IRFunc *f);
static bool emit_intn_store(Emitter *e, Node *node, IRFunc *func, EmitNodeFn en) {
    Node *tgt = node->assign.target;
    Type *nn_t = tgt ? checker_get_type(e->checker, tgt) : NULL;
    if (!type_is_nonnative_intn(nn_t)) return false;
    if (tgt->kind == NODE_SLICE) return false;          /* bit-slice SET */
    TokenType aop = node->assign.op;
    if (!(aop == TOK_EQ || aop == TOK_PLUSEQ || aop == TOK_MINUSEQ ||
          aop == TOK_STAREQ || aop == TOK_AMPEQ || aop == TOK_PIPEEQ ||
          aop == TOK_CARETEQ || aop == TOK_LSHIFTEQ))
        return false;
    /* union variant target (value or `*Union` auto-deref)? */
    Node *uobj = NULL; bool uptr = false; long utag = -1;
    if (tgt->kind == NODE_FIELD) {
        Type *ot = checker_get_type(e->checker, tgt->field.object);
        Type *oe = ot ? type_unwrap_distinct(ot) : NULL;
        if (oe && type_dispatch_kind(oe) == TYPE_POINTER && oe->pointer.inner &&
            type_dispatch_kind(oe->pointer.inner) == TYPE_UNION) {
            uptr = true; oe = type_unwrap_distinct(oe->pointer.inner);
        }
        if (oe && type_dispatch_kind(oe) == TYPE_UNION) {
            for (uint32_t i = 0; i < oe->union_type.variant_count; i++) {
                SUVariant *v = &oe->union_type.variants[i];
                if (v->name_len == (uint32_t)tgt->field.field_name_len &&
                    memcmp(v->name, tgt->field.field_name, v->name_len) == 0) {
                    utag = (long)i; break;
                }
            }
            if (utag >= 0) uobj = tgt->field.object;
        }
    }
    int tmp = e->temp_count++;
    char lv[96];
    emit(e, "({ ");
    if (uobj) {
        emit(e, "__typeof__(");
        en(e, uobj, func);
        emit(e, ") %s_zer_up%d = %s(", uptr ? "" : "*", tmp, uptr ? "" : "&");
        en(e, uobj, func);
        emit(e, "); _zer_up%d->_tag = %ld; ", tmp, utag);
        snprintf(lv, sizeof lv, "(_zer_up%d->%.*s)", tmp,
                 (int)tgt->field.field_name_len, tgt->field.field_name);
    } else if (lvalue_through_packed(e, tgt)) {
        /* BUG-1324: a field of a PACKED struct may sit at any byte offset, so a
         * plain `__typeof__(p.w) *` to it is misaligned (UBSan; a hard fault on
         * Cortex-M0 / RISC-V) — the pointer ZER forbids users to form (BUG-972).
         * An aligned(1) pointee keeps the single evaluation and makes GCC use
         * accesses that are legal at any address. */
        emit(e, "typedef __typeof__(");
        en(e, tgt, func);
        emit(e, ") __attribute__((aligned(1))) _zer_ua%d; _zer_ua%d *_zer_np%d = &(",
             tmp, tmp, tmp);
        en(e, tgt, func);
        emit(e, "); ");
        snprintf(lv, sizeof lv, "(*_zer_np%d)", tmp);
    } else {
        emit(e, "__typeof__(");
        en(e, tgt, func);
        emit(e, ") *_zer_np%d = &(", tmp);
        en(e, tgt, func);
        emit(e, "); ");
        snprintf(lv, sizeof lv, "(*_zer_np%d)", tmp);
    }
    char nv[40];
    snprintf(nv, sizeof nv, "_zer_nv%d", tmp);
    emit_type(e, nn_t);
    emit(e, " %s = ", nv);
    if (aop == TOK_EQ) {
        emit(e, "(");
        en(e, node->assign.value, func);
        emit(e, "); ");
    } else if (aop == TOK_LSHIFTEQ) {
        emit(e, "_zer_shl(%s, (", lv);
        en(e, node->assign.value, func);
        emit(e, "), %d); ", shift_guard_width(nn_t));
    } else {
        const char *cop = aop==TOK_PLUSEQ?"+":aop==TOK_MINUSEQ?"-":
                          aop==TOK_STAREQ?"*":aop==TOK_AMPEQ?"&":
                          aop==TOK_PIPEEQ?"|":"^";
        emit(e, "(%s %s (", lv, cop);
        en(e, node->assign.value, func);
        emit(e, ")); ");
    }
    emit_intn_mask_lv(e, nn_t, nv);
    emit(e, "%s = %s; %s; })", lv, nv, nv);
    return true;
}

/* BUG-1166: the ONE runtime type_id a `*opaque` carries for a pointee type —
 * the tag packed when a `*T` is wrapped and the tag expected when it is
 * unwrapped. It was seven copies of the same three lines, each answering 0
 * ("unknown provenance, allow") for every pointee that is not a struct / enum /
 * union, so a `*u32` wrapped in a function and returned as `*opaque` could be
 * cast to `*Big` (80 bytes) and written — the runtime check never fires on 0,
 * and the compile-time provenance is gone once the value crosses a return.
 * Scalars now get a RESERVED id (high bit set, so it can never collide with
 * the checker's sequential struct ids); pointer / slice / array / opaque
 * pointees keep 0 — their representation is not a fixed scalar. 0 is still
 * what C code (cinclude) produces: the FFI floor is unchanged. */
static uint32_t opaque_type_id(Type *inner);
/* @pun's advertised runtime check is NOMINAL — it compares struct / enum /
 * union ids and treats every other pointee as "no id" (the byte-view idiom
 * `@pun(const *u64, &s)` relies on that; the checker's
 * pun_type_id_check_can_fire mirrors exactly this). */
static uint32_t opaque_type_id_nominal(Type *inner) {
    if (!inner) return 0;
    inner = type_unwrap_distinct(inner);
    if (!inner) return 0;
    TypeKind k = type_dispatch_kind(inner);
    if (k == TYPE_STRUCT || k == TYPE_ENUM || k == TYPE_UNION) return opaque_type_id(inner);
    return 0;
}

static uint32_t opaque_type_id(Type *inner) {
    if (!inner) return 0;
    inner = type_unwrap_distinct(inner);
    if (!inner) return 0;
    switch (inner->kind) {
    case TYPE_STRUCT: return inner->struct_type.type_id;
    case TYPE_ENUM:   return inner->enum_type.type_id;
    case TYPE_UNION:  return inner->union_type.type_id;
    case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_USIZE:
    case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
    case TYPE_F32: case TYPE_F64: case TYPE_BOOL:
        return 0x80000000u | ((uint32_t)inner->kind << 8);
    case TYPE_UINT: case TYPE_SINT:
        return 0x80000000u | ((uint32_t)inner->kind << 8) | (inner->intn.bits & 0xFFu);
    case TYPE_VOID: case TYPE_POINTER: case TYPE_OPAQUE: case TYPE_SLICE:
    case TYPE_ARRAY: case TYPE_OPTIONAL: case TYPE_FUNC_PTR: case TYPE_HANDLE:
    case TYPE_POOL: case TYPE_RING: case TYPE_ARENA: case TYPE_BARRIER:
    case TYPE_SLAB: case TYPE_SEMAPHORE: case TYPE_DISTINCT:
        return 0;
    }
    return 0;
}

/* BUG-1161: is this assignment a write INTO a union variant that is not the
 * whole variant by plain `=` (a compound op, or a sub-field / element write)?
 * If so, return the union object, whether it is reached through a pointer,
 * and the variant index. Such a write must first RESET the union when it
 * changes the active variant, or the union's other bytes (the previous
 * variant's) are read back as the new one — see the checker's comment at
 * union_variant_write_is_partial. */
static Node *union_partial_write_target(Emitter *e, Node *node, bool *is_ptr,
                                        uint32_t *idx) {
    if (!node || node->kind != NODE_ASSIGN || !node->assign.target) return NULL;
    Node *walk = node->assign.target;
    while (walk) {
        if (walk->kind == NODE_FIELD) {
            Type *ot = checker_get_type(e->checker, walk->field.object);
            Type *oe = ot ? type_unwrap_distinct(ot) : NULL;
            bool ptr = false;
            if (oe && type_dispatch_kind(oe) == TYPE_POINTER && oe->pointer.inner &&
                type_dispatch_kind(oe->pointer.inner) == TYPE_UNION) {
                ptr = true; oe = type_unwrap_distinct(oe->pointer.inner);
            }
            if (oe && type_dispatch_kind(oe) == TYPE_UNION) {
                if (walk == node->assign.target && node->assign.op == TOK_EQ)
                    return NULL;                      /* whole-variant `=` */
                for (uint32_t i = 0; i < oe->union_type.variant_count; i++) {
                    SUVariant *v = &oe->union_type.variants[i];
                    if (v->name_len == (uint32_t)walk->field.field_name_len &&
                        memcmp(v->name, walk->field.field_name, v->name_len) == 0) {
                        *is_ptr = ptr; *idx = i;
                        return walk->field.object;
                    }
                }
                return NULL;
            }
            walk = walk->field.object;
        } else if (walk->kind == NODE_INDEX) {
            walk = walk->index_expr.object;
        } else {
            return NULL;
        }
    }
    return NULL;
}

/* Emit `(({ reset-if-variant-changes; 0; }), ` before such a write; the caller
 * emits the assignment itself and then `)`. */
static void emit_union_reset_prefix(Emitter *e, Node *uobj, bool is_ptr, uint32_t idx,
                                    IRFunc *func, EmitNodeFn en) {
    int t = e->temp_count++;
    emit(e, "(({ __typeof__(");
    en(e, uobj, func);
    emit(e, ") %s_zer_ur%d = %s(", is_ptr ? "" : "*", t, is_ptr ? "" : "&");
    en(e, uobj, func);
    emit(e, "); if (_zer_ur%d->_tag != %u) { memset(_zer_ur%d, 0, sizeof(*_zer_ur%d)); "
            "_zer_ur%d->_tag = %u; } 0; }), ", t, idx, t, t, t, idx);
}

static void emit_expr_impl(Emitter *e, Node *node);
static void emit_expr_impl_fn(Emitter *e, Node *n, IRFunc *f) {
    (void)f;
    emit_expr_impl(e, n);
}

/* BUG-1152: emit `node` (a guarded load) through `impl` inside the load guard.
 * ONE function holds both halves of the statement expression, so the emitted
 * braces stay balanced within it. */
static void emit_nn_guarded(Emitter *e, Node *node, IRFunc *func, EmitNodeFn impl) {
    int t = e->temp_count++;
    Type *ty = checker_get_type(e->checker, node);
    Node *saved_bp = e->nn_bypass;
    emit(e, "({ __auto_type _zer_nn%d = ", t);
    e->nn_bypass = node;
    impl(e, node, func);
    e->nn_bypass = saved_bp;
    emit(e, "; if (!_zer_nn%d%s) _zer_trap(\"%s\", __FILE__, __LINE__); _zer_nn%d; })",
         t, nn_null_member(ty),
         load_guard_kind(ty) == 2 ? ZER_ENUM0_TRAP_MSG : ZER_NN_TRAP_MSG, t);
}

/* BUG-1152: every emit_expr passes through the non-null load guard. */
static void emit_expr(Emitter *e, Node *node) {
    if (!node) return;
    Node *saved_lv = e->nn_lvalue;
    Node *lv = nn_lvalue_of(node);
    if (nn_guard_wanted(e, node)) {
        emit_nn_guarded(e, node, NULL, emit_expr_impl_fn);
        return;
    }
    if (lv) e->nn_lvalue = lv;
    /* BUG-1183: an assignment is an EXPRESSION in ZER and is emitted in value
     * position (`a[0] = (y = 5) + 1;`, `x + (x = 10)`); C's `=` binds loosest of
     * all, so without its own parentheses the enclosing operator's operand
     * swallowed it — `(y = 5U + 1U)` stored 6 into y, `(y = 3U == 3U)` stored 1,
     * `-(y=5)` became the invalid `-y = 5`. Parenthesised at the one dispatch
     * point for each emitter, so every nested position is covered. */
    bool asg_paren = node->kind == NODE_ASSIGN;
    if (asg_paren) emit(e, "(");
    bool ur_ptr = false; uint32_t ur_idx = 0;
    Node *ur = e->global_init_depth == 0 ? union_partial_write_target(e, node, &ur_ptr, &ur_idx) : NULL;
    if (ur) emit_union_reset_prefix(e, ur, ur_ptr, ur_idx, NULL, emit_node_via_ast);
    emit_expr_impl(e, node);
    if (ur) emit(e, ")");
    if (asg_paren) emit(e, ")");
    e->nn_lvalue = saved_lv;
}

static void emit_node_via_ast(Emitter *e, Node *n, IRFunc *f) {
    (void)f;
    emit_expr(e, n);
}

static void emit_expr_impl(Emitter *e, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_INT_LIT:
        emit_int_literal(e, node);
        break;

    case NODE_FLOAT_LIT:
        emit_double_lit(e, node->float_lit.value,
                           emit_type_is_f32(checker_get_type(e->checker, node)));
        break;

    case NODE_STRING_LIT:
        /* emit as _zer_slice_u8 compound literal.
         * length = sizeof("...") - 1 so the C compiler resolves escape
         * sequences at compile time. Source-char count overcounted ("\n"
         * is 2 source chars / 1 emitted byte), letting bounds checks
         * approve OOB reads on escape-bearing literals. */
        emit_zer_string_slice(e, node->string_lit.value, (int)node->string_lit.length, true);
        break;

    case NODE_CHAR_LIT:
        emit_char_lit(e, node);
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
        /* BUG-997 (from qo0mm9 / vigilant-tesla-o51x9p): inside a GLOBAL
         * initializer a name is not a C constant expression. The fold below in
         * emit_global_var covers only an INTEGER initializer that eval_const_expr
         * can evaluate; everything else emitted the NAME and GCC refused it, with
         * no ZER diagnostic and a line number in a .c file the user never opened:
         *
         *     const i32 K = -5;      i32 G = K;              // negative
         *     const f32 K = 1.5;     f32 G = K + 1.0;        // float target
         *     const bool K = true;   bool G = K;             // bool target
         *     const usize B = @size(u32) * 4;  usize C = B;  // intrinsic init
         *     const [*]u8 A = "hi";  const [*]u8 B = A;      // slice target
         *
         * Substituting the referenced global's OWN initializer is correct by
         * construction and needs no evaluator: that expression already passed the
         * global-initializer rules for ITS declaration, so it is emittable at file
         * scope, whatever its type. It composes — the ident may sit anywhere in
         * the expression, so `K + 1.0` works without a float folder.
         *
         * Restricted to a `const` global. A MUTABLE one is genuinely not a
         * compile-time constant and is rejected in the checker instead. The
         * depth bound is a backstop: a cycle never reaches here (BUG-975). */
        if (e->global_init_depth > 0 && e->global_init_depth < 64) {
            Symbol *gs = scope_lookup(e->checker->global_scope,
                node->ident.name, (uint32_t)node->ident.name_len);
            if (gs && gs->is_const && !gs->is_function && gs->func_node &&
                gs->func_node->kind == NODE_GLOBAL_VAR &&
                gs->func_node->var_decl.init &&
                gs->func_node->var_decl.init != node) {
                e->global_init_depth++;
                emit_expr(e, gs->func_node->var_decl.init);
                e->global_init_depth--;
                break;
            }
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
                /* BUG-1062: the MIN of THIS width, incl. iN and i128. */
                char dmin[96]; signed_min_text(div_type, dmin, sizeof dmin);
                emit(e, "; if (_zer_dd%d == %s) ", tmp, dmin);
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
            /* BUG-1031: inside a GLOBAL initializer the `_zer_shl` statement
             * expression is illegal C ("braced-group within expression allowed
             * only inside a function"), and the generic fold cannot decide a count
             * in [63,127] without the operand's width. Here the left operand's
             * TYPE is known: a constant count >= its width (or negative) is 0 by
             * the ZER rule, and an in-range constant count needs no guard. */
            if (e->global_init_depth > 0) {
                int64_t sc = eval_const_expr(node->binary.right);
                Type *lt = checker_get_type(e->checker, node->binary.left);
                Type *lte = lt ? type_unwrap_distinct(lt) : NULL;
                int lw = (lte && type_is_integer(lte)) ? type_width(lte) : 0;
                if (sc != CONST_EVAL_FAIL && lw > 0) {
                    if (sc < 0 || sc >= lw) {
                        emit(e, "((");
                        emit_type(e, lte);
                        emit(e, ")0)");
                    } else {
                        emit(e, "(");
                        emit_expr(e, node->binary.left);
                        emit(e, " %s %lld)", node->binary.op == TOK_LSHIFT ? "<<" : ">>",
                             (long long)sc);
                    }
                    break;
                }
            }
            emit(e, "%s(", node->binary.op == TOK_LSHIFT ? "_zer_shl" : "_zer_shr");
            emit_expr(e, node->binary.left);
            emit(e, ", ");
            emit_expr(e, node->binary.right);
            emit(e, ", %d)", shift_guard_width(checker_get_type(e->checker, node->binary.left)));
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
                    /* Stage 2 Part B (2026-04-28): exhaustive — only
                     * narrow integer results need a defensive cast. */
                    case TYPE_VOID: case TYPE_BOOL:
                    case TYPE_U32: case TYPE_U64: case TYPE_USIZE:
                    case TYPE_I32: case TYPE_I64:
                    case TYPE_F32: case TYPE_F64:
                    /* Path C: uN/iN — carrier-width wrap; odd-width masking
                     * is applied at the Phase-C mask sites, not here. */
                    case TYPE_UINT: case TYPE_SINT:
                    case TYPE_POINTER: case TYPE_OPTIONAL: case TYPE_SLICE:
                    case TYPE_ARRAY: case TYPE_STRUCT: case TYPE_ENUM:
                    case TYPE_UNION: case TYPE_FUNC_PTR: case TYPE_OPAQUE:
                    case TYPE_POOL: case TYPE_RING: case TYPE_ARENA:
                    case TYPE_BARRIER: case TYPE_HANDLE: case TYPE_SLAB:
                    case TYPE_SEMAPHORE: case TYPE_DISTINCT:
                        break;
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
        /* Native uN/iN width masking (odd widths u3/u21/i48/…). A store to a
         * non-native-width integer lvalue must re-wrap the result (uN mask /
         * iN sign-extend). The generic assignment path below emits the raw C
         * op with NO mask — var-decl init is masked via the IR_BINOP temp
         * (emit_intn_mask), but assignment & compound-assign are AST-passthrough
         * and skip it → silent wrong value (`u3 y; y = a+b` keeps bit 3; `s-=1`
         * underflows to 255). Emit store + mask through ONE hoisted pointer so
         * the target lvalue (incl. a side-effecting index) is evaluated exactly
         * once. Scalar targets only (array/union/bit-slice writes are handled by
         * the dedicated cases below and don't carry a scalar uN/iN type here).
         * /= %= >>= can't exceed the width (result magnitude ≤ operand), so they
         * skip this and keep their existing div-guard / shift paths. */
        if (emit_intn_store(e, node, NULL, emit_node_via_ast)) goto assign_done;   /* BUG-1162 */
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
        if (node->assign.target->kind == NODE_SLICE) {                /* BUG-1198 */
            Type *obj_type = checker_get_type(e->checker,node->assign.target->slice.object);
            if (obj_type && type_is_integer(obj_type)) {
                emit_bitslice_set(e, node, NULL, emit_node_via_ast);
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
        /* compound div/mod: target /= n / target %= n.
         *
         * Fix #3 (2026-05-02): mirror the IR path's complete guard set
         * (emitter.c:5815-5841). Two safety properties:
         *   (1) divisor != 0 (defense in depth — checker forces compile-
         *       time guard, but emitter still adds runtime trap)
         *   (2) on signed types, INT_MIN/-1 is C UB → trap.
         *
         * Pre-fix this AST path only checked (1). Function bodies are
         * IR-only since 2026-04-19, so user code reaches the IR path
         * with both guards. But other emission contexts (some
         * statement-expression fallbacks, top-level initializers,
         * comptime emission, future AST callers) still go through
         * emit_expr. Defense in depth: every emission site enforces
         * both invariants. */
        if (node->assign.op == TOK_SLASHEQ || node->assign.op == TOK_PERCENTEQ) {
            Type *tgt_type = checker_get_type(e->checker, node->assign.target);
            Type *tgt_eff = tgt_type ? type_unwrap_distinct(tgt_type) : NULL;
            bool is_signed_div = tgt_eff && type_is_signed(tgt_eff);
            const char *cop = node->assign.op == TOK_SLASHEQ ? "/" : "%";
            int tmp = e->temp_count++;
            /* Side-effect hoist — mirror IR path. Without this,
             * `arr[fn()] /= y` evaluates fn() twice when the INT_MIN
             * check fires (and once when it doesn't). */
            bool tgt_se = expr_has_side_effects(node->assign.target);
            if (tgt_se) {
                emit(e, "({ __typeof__(");
                emit_expr(e, node->assign.value);
                emit(e, ") _zer_dv%d = ", tmp);
                emit_expr(e, node->assign.value);
                emit(e, "; if (_zer_dv%d == 0) "
                       "_zer_trap(\"division by zero\", __FILE__, __LINE__); ",
                     tmp);
                emit(e, "__auto_type _zer_dp%d = &(", tmp);
                emit_expr(e, node->assign.target);
                emit(e, "); ");
                if (is_signed_div) {
                    emit(e, "if (_zer_dv%d == -1) { __typeof__(*_zer_dp%d) _zer_dd%d = *_zer_dp%d; ",
                         tmp, tmp, tmp, tmp);
                    /* BUG-1062: the MIN of THIS width, incl. iN and i128. */
                    char dmin[96]; signed_min_text(tgt_eff, dmin, sizeof dmin);
                    emit(e, "if (_zer_dd%d == %s) ", tmp, dmin);
                    emit(e, "_zer_trap(\"signed division overflow\", __FILE__, __LINE__); } ");
                }
                emit(e, "*_zer_dp%d %s= _zer_dv%d; })", tmp, cop, tmp);
                goto assign_done;
            }
            emit(e, "({ __typeof__(");
            emit_expr(e, node->assign.value);
            emit(e, ") _zer_dv%d = ", tmp);
            emit_expr(e, node->assign.value);
            emit(e, "; if (_zer_dv%d == 0) ", tmp);
            emit(e, "_zer_trap(\"division by zero\", __FILE__, __LINE__); ");
            if (is_signed_div) {
                emit(e, "if (_zer_dv%d == -1) { __typeof__(", tmp);
                emit_expr(e, node->assign.target);
                emit(e, ") _zer_dd%d = ", tmp);
                emit_expr(e, node->assign.target);
                /* BUG-1062: the MIN of THIS width, incl. iN and i128. */
                char dmin[96]; signed_min_text(tgt_eff, dmin, sizeof dmin);
                emit(e, "; if (_zer_dd%d == %s) ", tmp, dmin);
                emit(e, "_zer_trap(\"signed division overflow\", __FILE__, __LINE__); } ");
            }
            emit_expr(e, node->assign.target);
            emit(e, " %s= _zer_dv%d; })", cop, tmp);
            goto assign_done;
        }
        /* compound shift: target <<= n → target = _zer_shl(target, n)
         * If target has side effects, hoist via pointer to avoid double-eval */
        if (node->assign.op == TOK_LSHIFTEQ || node->assign.op == TOK_RSHIFTEQ) {
            /* Unified side-effect check — partial walker pre-fix missed
             * NODE_INDEX.index calls. See expr_has_side_effects above. */
            bool shift_side_effect = expr_has_side_effects(node->assign.target);
            const char *macro = node->assign.op == TOK_LSHIFTEQ ? "_zer_shl" : "_zer_shr";
            int shw = shift_guard_width(checker_get_type(e->checker, node->assign.target));
            if (shift_side_effect) {
                /* hoist target into pointer: *({ auto *_p = &target; *_p = macro(*_p, n); _p; }) — but simpler: */
                int tmp = e->temp_count++;
                emit(e, "({ __auto_type _zer_sp%d = &(", tmp);
                emit_expr(e, node->assign.target);
                emit(e, "); *_zer_sp%d = %s(*_zer_sp%d, ", tmp, macro, tmp);
                emit_expr(e, node->assign.value);
                emit(e, ", %d); })", shw);
            } else {
                emit_expr(e, node->assign.target);
                emit(e, " = %s(", macro);
                emit_expr(e, node->assign.target);
                emit(e, ", ");
                emit_expr(e, node->assign.value);
                emit(e, ", %d)", shw);
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
                emit_double_lit(e, node->call.comptime_float_value,
                                   emit_type_is_f32(checker_get_type(e->checker, node)));
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

        /* Task.alloc() / Task.free() — auto-Slab sugar */
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
                if (ml == 5 && memcmp(mn, "alloc", 5) == 0) {
                    /* Task.alloc() → slab.alloc() */
                    int tmp = e->temp_count++;
                    emit(e, "({uint8_t _zer_aok%d = 0; uint64_t _zer_ah%d = "
                         "_zer_slab_alloc(&%.*s, &_zer_aok%d); "
                         "(_zer_opt_u64){_zer_ah%d, _zer_aok%d}; })",
                         tmp, tmp, aslen, asname, tmp, tmp, tmp);
                    handled = true;
                } else if (ml == 9 && memcmp(mn, "alloc_ptr", 9) == 0) {
                    /* Task.alloc_ptr() → slab.alloc_ptr() */
                    int tmp = e->temp_count++;
                    emit(e, "({uint8_t _zer_aok%d = 0; uint64_t _zer_ah%d = "
                         "_zer_slab_alloc(&%.*s, &_zer_aok%d); ",
                         tmp, tmp, aslen, asname, tmp);
                    emit(e, "_zer_aok%d ? (", tmp);
                    emit_type(e, ot);
                    emit(e, "*)_zer_slab_get(&%.*s, _zer_ah%d) : (void*)0; })",
                         aslen, asname, tmp);
                    handled = true;
                } else if (ml == 4 && memcmp(mn, "free", 4) == 0) {
                    /* Task.free(h) → slab.free(h) */
                    emit(e, "_zer_slab_free(&%.*s, ", aslen, asname);
                    if (node->call.arg_count > 0)
                        emit_expr(e, node->call.args[0]);
                    emit(e, ")");
                    handled = true;
                } else if (ml == 8 && memcmp(mn, "free_ptr", 8) == 0) {
                    /* Task.free_ptr(p) → slab.free_ptr(p) */
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
            /* BUG-1019: guard an indirect call through a possibly-NULL funcptr.
             * AST dispatch path (the defer-body / spawn-arg emitter); the IR
             * paths carry the same guard — CLAUDE.md's "two emitter dispatch
             * paths" rule. */
            if (call_needs_null_funcptr_guard(e, node->call.callee)) {
                int fpt = e->temp_count++;
                emit(e, "({ __typeof__(");
                emit_expr(e, node->call.callee);
                emit(e, ") _zer_fp%d = ", fpt);
                emit_expr(e, node->call.callee);
                emit(e, "; if (!_zer_fp%d) _zer_trap(\"call through a null function "
                        "pointer\", __FILE__, __LINE__); _zer_fp%d; })", fpt, fpt);
            } else {
                emit_expr(e, node->call.callee);
            }
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
            Symbol *alloc_sym = node->field.handle_alloc;   /* BUG-1053 */
            if (!alloc_sym && node->field.object->kind == NODE_IDENT) {
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
                    emit(e, "*)_zer_slab_get(&"); emit_alloc_sym_cname(e, alloc_sym); emit(e, ", ");
                    emit_expr(e, node->field.object);
                    emit(e, "))->%.*s",
                         (int)node->field.field_name_len, node->field.field_name);
                } else if (at->kind == TYPE_POOL) {
                    emit(e, "((");
                    emit_type(e, at->pool.elem);
                    emit(e, "*)_zer_pool_get("); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".slots, "); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".gen, "); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".used, sizeof("); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".slots[0]), ");
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
        {
            /* G2 (2026-08-01): parenthesize a looser-binding object. */
            bool fp = field_obj_needs_parens(node->field.object);
            if (fp) emit(e, "(");
            emit_expr(e, node->field.object);
            if (fp) emit(e, ")");
        }
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
                    /* Stage 2 Part B (2026-04-28): exhaustive — only
                     * signed integer types need unsigned cast for bit
                     * extraction. Other kinds: no cast needed. */
                    case TYPE_VOID: case TYPE_BOOL:
                    case TYPE_U8: case TYPE_U16: case TYPE_U32:
                    case TYPE_U64: case TYPE_USIZE:
                    /* Path C: uN unsigned (no cast); iN handled via width in Phase C */
                    case TYPE_UINT: case TYPE_SINT:
                    case TYPE_F32: case TYPE_F64:
                    case TYPE_POINTER: case TYPE_OPTIONAL: case TYPE_SLICE:
                    case TYPE_ARRAY: case TYPE_STRUCT: case TYPE_ENUM:
                    case TYPE_UNION: case TYPE_FUNC_PTR: case TYPE_OPAQUE:
                    case TYPE_POOL: case TYPE_RING: case TYPE_ARENA:
                    case TYPE_BARRIER: case TYPE_HANDLE: case TYPE_SLAB:
                    case TYPE_SEMAPHORE: case TYPE_DISTINCT:
                        break;
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
                    /* runtime — single-eval: hoist start/end into temps.
                     * #18: the POSITION shift `obj >> _zer_lo` is UB in C when
                     * _zer_lo >= the operand's bit width (e.g. u64 >> 64). The F8
                     * fix guarded the extract-width MASK but left the shift
                     * unguarded — a silent miscompile violating ZER's "shift by
                     * >= width = 0" guarantee (GCC -O0 vs -O2 diverge; a different
                     * value again on ARM/RISC-V baremetal). Guard the shift on the
                     * object's declared bit width so an out-of-range position → 0. */
                    int objbits = type_width(obj_type);
                    if (objbits <= 0) objbits = 64;
                    int tmp = e->temp_count++;
                    /* BUG-1198: positions are uint64_t — an `int` wrapped a runtime
                     * position >= 2^31 negative (UBSan "shift exponent -2"; 3 at -O0,
                     * 0 at -O2). hi < lo is an empty field (0). */
                    emit(e, "({ uint64_t _zer_hi%d = (uint64_t)(", tmp);
                    emit_expr(e, node->slice.start);
                    emit(e, "); uint64_t _zer_lo%d = (uint64_t)(", tmp);
                    emit_expr(e, node->slice.end);
                    emit(e, "); uint64_t _zer_w%d = (_zer_hi%d < _zer_lo%d) ? 0 : _zer_hi%d - _zer_lo%d + 1; (((_zer_lo%d >= %d) ? (uint64_t)0 : (%s", tmp, tmp, tmp, tmp, tmp, tmp, objbits, ucast);
                    emit_expr(e, node->slice.object);
                    emit(e, " >> _zer_lo%d)) & ((_zer_w%d >= 64) ? ~(uint64_t)0 : (_zer_w%d == 0) ? (uint64_t)0 : ((1ull << _zer_w%d) - 1))); })",
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
        /* Runtime check: start <= end <= cap for variable bounds.
         *
         * Silent-gap fix (audit 2026-04-30): the trigger formerly required BOTH
         * `start` and `end` to be present and at least one non-constant. That
         * missed three silent OOB shapes:
         *   - `arr[start..]` with var `start > arr.len` → `len = cap - start`
         *     underflowed in `size_t`, producing a slice claiming the whole
         *     address space.
         *   - `arr[..end]` with var `end > arr.len` → `len = end` exceeded
         *     capacity with no check.
         *   - `arr[a..b]` with `b > arr.len` → only `a > b` was checked.
         * For slices (TYPE_SLICE) the capacity is dynamic, so even constant
         * bounds need a runtime check; the checker only catches the array
         * case at compile time. */
        bool obj_is_slice_early = obj_type && obj_type->kind == TYPE_SLICE;
        bool slice_needs_runtime_check = false;
        if (!type_is_integer(obj_type)) {
            if (node->slice.start) {
                int64_t v = eval_const_expr(node->slice.start);
                if (v == CONST_EVAL_FAIL) slice_needs_runtime_check = true;
            }
            if (node->slice.end) {
                int64_t v = eval_const_expr(node->slice.end);
                if (v == CONST_EVAL_FAIL) slice_needs_runtime_check = true;
            }
            /* Slices (dynamic cap): any specified bound needs runtime cap
             * verification even if literal-constant. */
            if (obj_is_slice_early && (node->slice.start || node->slice.end)) {
                slice_needs_runtime_check = true;
            }
        }

        /* Use named _zer_slice_T typedefs for ALL types (BUG-085 fix).
         * BUG-1027: ONE query (emit_slice_name) names the typedef for EVERY
         * element kind — the per-site switch here used to leave enum / Handle /
         * pointer / optional / funcptr / array / nested-slice elements to the
         * anonymous-struct fallback below, an incompatible C type at each site. */
        bool slice_type_emitted = false;
        if (slice_needs_runtime_check && !slice_obj_side_effect_early) {
            /* skip the normal struct literal — we'll wrap in stmt expr */
        } else if (eff_elem && !slice_obj_side_effect_early) {
            emit(e, "((");
            emit_slice_name(e, eff_elem, false, false);
            emit(e, "){ ");
            slice_type_emitted = true;
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
            /* BUG-262: hoist start/end into temps for single evaluation.
             * Silent-gap fix (audit 2026-04-30): handle open-ended forms
             * (`arr[start..]`, `arr[..end]`) and verify `end <= cap` and
             * `start <= cap`, not just `start <= end`. Hoist the object
             * into a temp so cap (`obj.len` for slices) is read once. */
            int sl_tmp = e->temp_count++;
            bool is_array = obj_type && obj_type->kind == TYPE_ARRAY;
            emit(e, "({ ");
            if (obj_is_slice) {
                /* Hoist slice object so .len is single-evaluation. */
                emit(e, "__typeof__(");
                emit_expr(e, node->slice.object);
                emit(e, ") _zer_so%d = ", sl_tmp);
                emit_expr(e, node->slice.object);
                emit(e, "; size_t _zer_cap%d = _zer_so%d.len; ", sl_tmp, sl_tmp);
            } else if (is_array) {
                emit(e, "size_t _zer_cap%d = %llu; ", sl_tmp,
                     (unsigned long long)obj_type->array.size);
            } else {
                emit(e, "size_t _zer_cap%d = 0; ", sl_tmp);
            }
            emit(e, "size_t _zer_ss%d = ", sl_tmp);
            if (node->slice.start) {
                emit(e, "(size_t)(");
                emit_expr(e, node->slice.start);
                emit(e, ")");
            } else {
                emit(e, "0");
            }
            emit(e, "; size_t _zer_se%d = ", sl_tmp);
            if (node->slice.end) {
                emit(e, "(size_t)(");
                emit_expr(e, node->slice.end);
                emit(e, ")");
            } else {
                emit(e, "_zer_cap%d", sl_tmp);
            }
            emit(e, "; ");
            emit(e, "if (_zer_ss%d > _zer_se%d) _zer_trap(\"slice start > end\", __FILE__, __LINE__); ",
                 sl_tmp, sl_tmp);
            emit(e, "if (_zer_se%d > _zer_cap%d) _zer_trap(\"slice end > len\", __FILE__, __LINE__); ",
                 sl_tmp, sl_tmp);
            emit(e, "(");
            emit_type(e, type_slice(e->arena, obj_type->kind == TYPE_ARRAY ?
                obj_type->array.inner : obj_type->slice.inner));
            emit(e, "){ &(");
            if (obj_is_slice) {
                emit(e, "_zer_so%d.ptr", sl_tmp);
            } else {
                emit_expr(e, node->slice.object);
            }
            emit(e, ")[_zer_ss%d], _zer_se%d - _zer_ss%d }; })",
                 sl_tmp, sl_tmp, sl_tmp);
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
                tid = opaque_type_id(inner);   /* BUG-1166 */
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
                expected_tid = opaque_type_id(inner);   /* BUG-1166 */
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
        } else if (tgt_eff && tgt_eff->kind == TYPE_BOOL && src_eff &&
                   (type_is_integer(src_eff) || type_is_float(src_eff) ||
                    src_eff->kind == TYPE_POINTER)) {
            /* To bool: use truthy conversion (!!x), not plain integer cast.
             * ZER emits bool as uint8_t so (uint8_t)5 gives 5, not 1 —
             * BUG-586 fixed this in IR_CAST but the AST path (used for
             * global initializers, which must be constant expressions and
             * therefore cannot take the IR path) was missed. */
            emit(e, "((uint8_t)!!(");
            emit_expr(e, node->typecast.expr);
            emit(e, "))");
        } else if (f2i_needs_guard(src_eff, tgt_eff)) {
            /* BUG-990: this AST path is the GLOBAL-INITIALIZER path (see the bool
             * arm above), where a statement expression is illegal C. Prefer the
             * constant-expression form; it declines an operand that cannot be
             * re-evaluated (side effects, volatile), which cannot occur in a
             * constant initializer, so the statement form below is then only
             * reached inside a function where it is legal. */
            if (!emit_f2i_const(e, tgt, node->typecast.expr)) {
                int tmp = e->temp_count++;                /* BUG-845 site 1 (AST) */
                emit_f2i_open(e, src_eff, tmp);
                emit_expr(e, node->typecast.expr);
                emit_f2i_close(e, tgt, tmp);
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
        bool si_arr = e->global_init_depth == 0 &&
                      struct_init_names_array_field(si_type, node);   /* BUG-1157 */
        int si_tmp = si_arr ? e->temp_count++ : 0;
        if (si_arr) {
            emit(e, "({ ");
            emit_type(e, si_type);
            emit(e, " _zer_si%d = ", si_tmp);
        }
        if (si_type) {
            emit(e, "(");
            emit_type(e, si_type);
            emit(e, ")");
        }
        emit(e, "{ ");
        bool si_first = true;
        for (int i = 0; i < node->struct_init.field_count; i++) {
            const char *fname = node->struct_init.fields[i].name;
            uint32_t fname_len = (uint32_t)node->struct_init.fields[i].name_len;
            if (si_arr) {
                Type *aft = struct_field_type_by_name(si_type, fname, fname_len);
                if (aft && type_dispatch_kind(aft) == TYPE_ARRAY) continue;
            }
            if (!si_first) emit(e, ", ");
            si_first = false;
            emit(e, ".%.*s = ", (int)fname_len, fname);
            Node *fval = node->struct_init.fields[i].value;
            Type *fv_type = checker_get_type(e->checker, fval);
            Type *wt = struct_init_opt_wrap_type(si_type, fname, fname_len, fv_type);
            if (wt) emit_opt_wrap_value(e, wt, fval);  /* F21 (coerces ?slice inside) */
            else {
                /* #14 (B): bare array into a plain [*]T or ?[*]T field → coerce to
                 * a slice literal (else C brace-flattens it into .ptr/.len). */
                Type *ftype = struct_field_type_by_name(si_type, fname, fname_len);
                Type *slice_tgt = aggregate_slice_coerce_target(ftype, fv_type);
                Type *vte = fv_type ? type_unwrap_distinct(fv_type) : NULL;
                if (slice_tgt && vte && type_dispatch_kind(vte) == TYPE_ARRAY)
                    emit_array_as_slice(e, fval, vte, slice_tgt);
                else
                    emit_expr(e, fval);
            }
        }
        if (si_first && si_arr) emit(e, "0");
        emit(e, " }");
        if (si_arr) {
            for (int i = 0; i < node->struct_init.field_count; i++) {
                const char *fname = node->struct_init.fields[i].name;
                uint32_t fname_len = (uint32_t)node->struct_init.fields[i].name_len;
                Type *aft = struct_field_type_by_name(si_type, fname, fname_len);
                if (!aft || type_dispatch_kind(aft) != TYPE_ARRAY) continue;
                emit(e, "; memcpy(&_zer_si%d.%.*s, (", si_tmp, (int)fname_len, fname);   /* BUG-1192 */
                emit_expr(e, node->struct_init.fields[i].value);
                emit(e, "), sizeof(_zer_si%d.%.*s))", si_tmp, (int)fname_len, fname);
            }
            emit(e, "; _zer_si%d; })", si_tmp);
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
                       node->intrinsic.args[0]->kind == NODE_IDENT &&
                       checker_get_type(e->checker, node->intrinsic.args[0])) {
                /* BUG-1038: the checker resolved the operand — a type name, a uN
                 * spelling, OR a VARIABLE (`u32 x; @size(x)`), whose type is what
                 * sizeof wants. The name-based fallback below spelled the variable
                 * as `struct x` (GCC: incomplete type). IR-path twin below. */
                emit_type(e, checker_get_type(e->checker, node->intrinsic.args[0]));
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
                emit_offset_type_operand(e, node->intrinsic.args[0]);   /* BUG-1215 */
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
                    tid = opaque_type_id(inner);   /* BUG-1166 */
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
                    expected_tid = opaque_type_id(inner);   /* BUG-1166 */
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
        } else if (nlen == 3 && memcmp(name, "pun", 3) == 0) {
            /* @pun(*T, expr) — desugars to inline *opaque round-trip with
             * type_id wrap + runtime check. Equivalent to:
             *   @ptrcast(*T, @ptrcast(*opaque, expr))
             * but emitted as a single block expression.
             *
             * Emits:
             *   ({ _zer_opaque _pc = (_zer_opaque){(void*)(SRC), SRC_TID};
             *      if (_pc.type_id != TGT_TID && _pc.type_id != 0)
             *          _zer_trap("...", __FILE__, __LINE__);
             *      (TGT_TYPE)_pc.ptr; })
             *
             * Runtime semantics: if SRC's type_id matches TGT's (identity or
             * provenance-preserved round-trip), no trap. Otherwise trap.
             * type_id == 0 sentinel matches anything (unknown provenance trust). */
            Type *tgt_type = node->intrinsic.type_arg ?
                resolve_tynode(e, node->intrinsic.type_arg) : NULL;
            Type *src_type = (node->intrinsic.arg_count > 0) ?
                checker_get_type(e->checker, node->intrinsic.args[0]) : NULL;
            Type *tgt_eff = tgt_type ? type_unwrap_distinct(tgt_type) : NULL;
            Type *src_eff = src_type ? type_unwrap_distinct(src_type) : NULL;

            /* determine source type_id */
            uint32_t src_tid = 0;
            if (src_eff && src_eff->kind == TYPE_POINTER && src_eff->pointer.inner) {
                Type *inner = type_unwrap_distinct(src_eff->pointer.inner);
                src_tid = opaque_type_id_nominal(inner);   /* @pun: nominal ids only (BUG-1166) */
            } else if (src_eff && src_eff->kind == TYPE_OPAQUE) {
                /* source already *opaque — its type_id flows through directly */
                src_tid = 0; /* will be read from the source's actual struct field */
            }

            /* determine target type_id */
            uint32_t tgt_tid = 0;
            if (tgt_eff && tgt_eff->kind == TYPE_POINTER && tgt_eff->pointer.inner) {
                Type *inner = type_unwrap_distinct(tgt_eff->pointer.inner);
                tgt_tid = opaque_type_id_nominal(inner);   /* @pun: nominal ids only (BUG-1166) */
            }

            /* If source is already *opaque, reuse its existing type_id and just
             * unwrap with check (single FROM-*opaque step). */
            bool src_is_opaque = (src_eff &&
                ((src_eff->kind == TYPE_POINTER && src_eff->pointer.inner &&
                  type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_OPAQUE) ||
                 src_eff->kind == TYPE_OPAQUE));

            if (src_is_opaque) {
                /* @pun on already-opaque source — only emit the FROM-*opaque check */
                if (tgt_tid > 0) {
                    int tmp = e->temp_count++;
                    emit(e, "({ _zer_opaque _zer_pn%d = ", tmp);
                    if (node->intrinsic.arg_count > 0)
                        emit_expr(e, node->intrinsic.args[0]);
                    emit(e, "; if (_zer_pn%d.type_id != %u && _zer_pn%d.type_id != 0) ",
                         tmp, (unsigned)tgt_tid, tmp);
                    emit(e, "_zer_trap(\"@pun type mismatch\", __FILE__, __LINE__); (");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")_zer_pn%d.ptr; })", tmp);
                } else {
                    /* target has no type_id (primitive pointer) — just unwrap .ptr */
                    emit(e, "(");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")(");
                    if (node->intrinsic.arg_count > 0)
                        emit_expr(e, node->intrinsic.args[0]);
                    emit(e, ").ptr");
                }
            } else {
                /* @pun on raw typed pointer — emit full wrap+unwrap inline */
                if (tgt_tid > 0) {
                    int tmp = e->temp_count++;
                    emit(e, "({ _zer_opaque _zer_pn%d = (_zer_opaque){(void*)(", tmp);
                    if (node->intrinsic.arg_count > 0)
                        emit_expr(e, node->intrinsic.args[0]);
                    emit(e, "), %u}; if (_zer_pn%d.type_id != %u && _zer_pn%d.type_id != 0) ",
                         (unsigned)src_tid, tmp, (unsigned)tgt_tid, tmp);
                    emit(e, "_zer_trap(\"@pun type mismatch\", __FILE__, __LINE__); (");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")_zer_pn%d.ptr; })", tmp);
                } else {
                    /* target has no type_id (primitive pointer like *u8) — plain cast */
                    emit(e, "((");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")(");
                    if (node->intrinsic.arg_count > 0)
                        emit_expr(e, node->intrinsic.args[0]);
                    emit(e, "))");
                }
            }
        } else if (nlen == 7 && memcmp(name, "bitcast", 7) == 0) {
            /* @bitcast(T, val) → memcpy type punning (valid C99+GCC) */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                int tmp2 = e->temp_count++;
                /* BUG-1001 (from v6o9c5, its BUG-979): an ARRAY source DECAYS under
                 * `__auto_type` (`__auto_type b = a` makes b a `uint8_t *`, sizeof 8),
                 * so `memcpy(&bco, &bci, N)` copied the POINTER's bytes — a wrong
                 * value for `@bitcast(u64, u8[8])` and an over-read past the pointer
                 * for anything wider. An array expression already denotes its bytes,
                 * so copy from it directly. (An array TARGET is refused by the
                 * checker, BUG-1000.) */
                Node *bsrc = node->intrinsic.arg_count > 0 ? node->intrinsic.args[0] : NULL;
                bool src_is_array = bsrc &&
                    type_dispatch_kind(checker_get_type(e->checker, bsrc)) == TYPE_ARRAY;
                emit(e, "({ ");
                if (!src_is_array) {
                    emit(e, "__auto_type _zer_bci%d = ", tmp2);
                    if (bsrc) emit_expr(e, bsrc);
                    emit(e, "; ");
                }
                emit_type(e, t);
                if (src_is_array) {
                    emit(e, " _zer_bco%d; memcpy(&_zer_bco%d, (", tmp, tmp);
                    emit_expr(e, bsrc);
                    emit(e, "), sizeof(_zer_bco%d)); ", tmp);
                } else {
                    emit(e, " _zer_bco%d; memcpy(&_zer_bco%d, &_zer_bci%d, sizeof(_zer_bco%d)); ",
                         tmp, tmp, tmp2, tmp);
                }
                /* #17: non-native uN/iN target — mask/sign-extend the punned carrier.
                 * The memcpy copies the full carrier (e.g. all 8 bits of a u5's
                 * uint8_t), leaving an over-width / un-sign-extended value; mask (uN)
                 * or sign-extend (iN), the same treatment @truncate applies. */
                if (type_is_nonnative_intn(t)) {
                    char lv[40]; snprintf(lv, sizeof lv, "_zer_bco%d", tmp);
                    emit_intn_mask_lv(e, t, lv);
                }
                { char lv2[40]; snprintf(lv2, sizeof lv2, "_zer_bco%d", tmp);
                  emit_bitcast_enum_guard(e, t, lv2); }   /* BUG-843 */
                emit(e, "_zer_bco%d; })", tmp);
            } else {
                emit(e, "0");
            }
        } else if (nlen == 8 && memcmp(name, "truncate", 8) == 0) {
            /* @truncate(T, val) → (T)(val). For a non-native uN/iN target the
             * cast alone keeps the over-width bits (`@truncate(u3,13)` → 13 not
             * 5), so mask the result here — covers INLINE uses (comparisons,
             * call args), not just stores. */
            Type *tt = node->intrinsic.type_arg ? resolve_tynode(e,node->intrinsic.type_arg) : NULL;
            /* BUG-864: an ENUM target needs the same variant guard @bitcast gets.
             * `@truncate(State, 7)` reaches the closed set by a different door and
             * had NO check at all, so the forged value silently ran the switch's
             * last arm — the exact defect BUG-843 closed for the other spelling. */
            if (type_is_nonnative_intn(tt) || type_carries_enum_e(tt, 0)) {
                int tmp = e->temp_count++;
                char lv[40]; snprintf(lv, sizeof lv, "_zer_tr%d", tmp);
                emit(e, "({ "); emit_type(e, tt);
                emit(e, " _zer_tr%d = (", tmp); emit_type(e, tt); emit(e, ")(");
                if (node->intrinsic.arg_count > 0) emit_expr(e, node->intrinsic.args[0]);
                else emit(e, "0");
                emit(e, "); ");
                emit_intn_mask_lv(e, tt, lv);
                emit_enum_variant_guard_path(e, tt, lv, "@truncate", 0);
                emit(e, "_zer_tr%d; })", tmp);
            } else {
                emit(e, "(");
                if (tt) emit_type(e, tt);
                emit(e, ")(");
                if (node->intrinsic.arg_count > 0)
                    emit_expr(e, node->intrinsic.args[0]);
                emit(e, ")");
            }
        } else if (nlen == 8 && memcmp(name, "saturate", 8) == 0) {
            /* @saturate(T, val) → clamp val to T's min/max range */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e,node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                /* BUG-910: @saturate is the THIRD door into an enum target, and it
                 * had no variant guard. BUG-843 closed @bitcast; BUG-891 added the
                 * carrier walk and @truncate at both dispatch paths — and left this
                 * sibling, so `@saturate(State, 7)` still forged a value outside the
                 * variant set and the exhaustive switch silently ran its LAST arm
                 * (measured: exit 3, no trap). @cast is NOT a fourth door: it
                 * requires a distinct typedef and cannot name a bare enum.
                 *
                 * WRAPPED rather than threaded into the clamp: the two dispatch
                 * paths compute the clamp differently and its value is a bare
                 * ternary, not an lvalue the guard can name. Binding the finished
                 * result to a temp reuses each path's emission VERBATIM, so the
                 * guard cannot drift from the clamp it guards. */
                bool sat_enum = t && type_carries_enum_e(t, 0);
                int seg = sat_enum ? e->temp_count++ : 0;
                if (sat_enum) { emit(e, "({ "); emit_type(e, t); emit(e, " _zer_seg%d = (", seg); }
                Node *sat_arg = node->intrinsic.arg_count > 0 ? node->intrinsic.args[0] : NULL;
                emit_saturate_open(e, sat_arg, tmp);
                if (sat_arg) emit_expr(e, sat_arg);
                else emit(e, "0");
                emit_saturate_close(e, sat_arg, t, tmp);
                if (sat_enum) {
                    char segp[40]; snprintf(segp, sizeof segp, "_zer_seg%d", seg);
                    emit(e, "); ");
                    emit_enum_variant_guard_path(e, t, segp, "@saturate", 0);
                    emit(e, "_zer_seg%d; })", seg);
                }
            } else {
                emit(e, "0");
            }
        } else if (nlen == 8 && memcmp(name, "inttoptr", 8) == 0) {
            /* @inttoptr(*T, addr) → (T*)(uintptr_t)(addr)
             * With mmio ranges: variable addresses get runtime range check
             * Auto-discovery removed (2026-04-01) — --no-strict-mmio with no
             * mmio declarations just emits plain cast (like C, programmer's choice) */
            /* GAP-2 fix (BUG-736, 2026-06-10, 6u360k audit): range and
             * alignment are ORTHOGONAL runtime axes (mirror of the
             * compile-time Gap 19 fix above in checker.c). The RANGE check
             * is gated on declared ranges — nothing to test against
             * otherwise (the 2026-04-01 plain-cast decision stands for
             * range under --no-strict-mmio). The ALIGNMENT check is a
             * property of the TARGET TYPE and must be emitted for variable
             * addresses even with zero mmio declarations — a misaligned
             * volatile *u32 load is a BusFault on Cortex-M0. */
            emit_inttoptr(e, node, NULL);   /* BUG-1058: one emission, both paths */
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
        } else if (nlen == 8 && memcmp(name, "try_enum", 8) == 0) {
            emit_try_enum_open(e);
            if (node->intrinsic.arg_count > 0) emit_expr(e, node->intrinsic.args[0]);
            else emit(e, "0");
            emit_try_enum_close(e, node->intrinsic.type_arg
                                   ? resolve_tynode(e, node->intrinsic.type_arg) : NULL,
                                node->intrinsic.arg_count > 0
                                   ? checker_get_type(e->checker, node->intrinsic.args[0]) : NULL);
        } else if (nlen == 5 && memcmp(name, "probe", 5) == 0) {
            emit(e, "_zer_probe((uintptr_t)(");
            if (node->intrinsic.arg_count > 0)
                emit_expr(e, node->intrinsic.args[0]);
            emit(e, "))");
        } else if (nlen >= 9 && memcmp(name, "atomic_", 7) == 0) {
            /* @atomic_add/sub/or/and/xor/load/store/cas — dual-path emission.
             * BUG-993: was `nlen >= 10`, which excludes `@atomic_or` (9 chars) —
             * the checker fixed exactly that off-by-one in BUG-427 and this copy
             * never followed. Unreachable today (the checker rejects any atomic
             * in a global initialiser, the only context that still reaches this
             * AST path) but it is the same predicate written twice with two
             * different answers, which is the drift this file keeps paying for.
             * The IR path already uses `>= 7`. */
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
                /* auto-orelse: return zero value instead of trap. Trap stays as comment.
                 * Caller already opened "{ " — emit defers+return+"}" inline. */
                emit_safety_early_return(e, false);
                emit(e, "} ");
            } else if (dest_is_slice) {
                emit(e, "; if (_zer_cs%d.len + 1 > _zer_cd%d.len) { ", tmp, tmp);
                emit_safety_early_return(e, false);
                emit(e, "} ");
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
        } else if (nlen == 7 && (memcmp(name, "bswap16", 7) == 0 ||
                                 memcmp(name, "bswap32", 7) == 0 ||
                                 memcmp(name, "bswap64", 7) == 0) &&
                   node->intrinsic.arg_count >= 1) {
            /* BH-18 #11: byte swap also reachable from AST path (e.g. global init). */
            emit(e, "__builtin_%.*s(", (int)nlen, name);
            emit_expr(e, node->intrinsic.args[0]);
            emit(e, ")");
        } else if (((nlen == 8 && memcmp(name, "popcount", 8) == 0) ||
                    (nlen == 3 && memcmp(name, "ctz", 3) == 0) ||
                    (nlen == 3 && memcmp(name, "clz", 3) == 0) ||
                    (nlen == 6 && memcmp(name, "parity", 6) == 0) ||
                    (nlen == 3 && memcmp(name, "ffs", 3) == 0)) &&
                   node->intrinsic.arg_count >= 1) {
            /* BH-18 #11: bit-query intrinsics emitted from the AST path (global init,
             * comptime-eval fallback). The IR-rewritten sibling at emitter.c:~8381 wraps
             * ctz/clz with a zero-guard returning bit width; mirror that here so the
             * value is correct in static contexts. popcount/parity/ffs are GCC-defined
             * at 0. */
            Type *arg_t = checker_get_type(e->checker, node->intrinsic.args[0]);
            int w = arg_t ? type_width(type_unwrap_distinct(arg_t)) : 32;
            const char *suffix = (w > 32) ? "ll" : "";
            bool is_ctz = (nlen == 3 && memcmp(name, "ctz", 3) == 0);
            bool is_clz = (nlen == 3 && memcmp(name, "clz", 3) == 0);
            if (is_ctz || is_clz) {
                /* AST emission is reachable from global initializers (file scope) where
                 * GCC statement expressions are not allowed. Use a conditional expression
                 * which double-evaluates the arg — safe for globals (constant expr) and
                 * matches the IR-path semantics: ctz(0)/clz(0) → bit-width. */
                emit(e, "(uint32_t)((");
                bitq_operand_open(e, w);
                emit_expr(e, node->intrinsic.args[0]);
                bitq_operand_close(e, w);
                emit(e, " == 0) ? %d : __builtin_%.*s%s(", bitq_count_width(w), (int)nlen, name, suffix);
                bitq_operand_open(e, w);
                emit_expr(e, node->intrinsic.args[0]);
                bitq_operand_close(e, w);
                emit(e, "))");
            } else {
                emit(e, "(uint32_t)__builtin_%.*s%s(", (int)nlen, name, suffix);
                bitq_operand_open(e, w);
                emit_expr(e, node->intrinsic.args[0]);
                bitq_operand_close(e, w);
                emit(e, ")");
            }
        } else if (nlen == 4 && memcmp(name, "addc", 4) == 0 && node->intrinsic.arg_count == 3) {
            emit(e, "_zer_do_addc_u64(");
            emit_expr(e, node->intrinsic.args[0]); emit(e, ", ");
            emit_expr(e, node->intrinsic.args[1]); emit(e, ", ");
            emit_expr(e, node->intrinsic.args[2]); emit(e, ")");
        } else if (nlen == 4 && memcmp(name, "subb", 4) == 0 && node->intrinsic.arg_count == 3) {
            emit(e, "_zer_do_subb_u64(");
            emit_expr(e, node->intrinsic.args[0]); emit(e, ", ");
            emit_expr(e, node->intrinsic.args[1]); emit(e, ", ");
            emit_expr(e, node->intrinsic.args[2]); emit(e, ")");
        } else if (nlen == 4 && memcmp(name, "mulw", 4) == 0 && node->intrinsic.arg_count == 2) {
            emit(e, "_zer_do_mulw_u64(");
            emit_expr(e, node->intrinsic.args[0]); emit(e, ", ");
            emit_expr(e, node->intrinsic.args[1]); emit(e, ")");
        } else {
            /* BUG-767 (copied from cool-johnson-dfcqr9): a runtime/privileged
             * intrinsic with no AST-path handler (e.g. @port_in32, @cpu_read_msr)
             * used in a global const-init previously emitted a placeholder + 0,
             * silently substituting zero for the runtime read. Emit an undeclared
             * identifier so GCC errors loudly instead of producing a binary that
             * reads 0 at startup. */
            emit(e, "__zer_intrinsic_%.*s_unsupported_in_constant_context", (int)nlen, name);
        }
        break;
    }

    /* Stage 2 Part B note (2026-04-28): emit_expr's `default:` is KEPT
     * intentionally. Exhaustive case enumeration here would create false
     * positives in `tools/walker_audit.sh`, which compares emit_expr's
     * case list against emit_rewritten_node — listing NODE_FILE etc. as
     * cases would falsely flag them as IR-path gaps. emit_expr is the
     * legacy AST diagnostic path (unreachable for well-formed ASTs in
     * the IR pipeline); the diagnostic fallback is the right behavior
     * for any kind not legitimately reachable here. */
    default:
        emit_unreachable(e, node_kind_name(node->kind), node);   /* BUG-851 */
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
    /* Fire pending IR defer bodies (LIFO, top down to `base`) WITHOUT popping —
     * used by a mid-body CONDITIONAL early-exit (auto-guard bounds return, @cstr
     * overflow return, orelse fallback return/break/continue) where the code path
     * that continues AFTER the early-exit still owns the same pending defers.
     * IR path: bodies live on e->defer_stack (pushed by IR_DEFER_PUSH) and are
     * keyed to cur_ir_func's locals. Before this, the auto-guard early-return
     * called emit_defers with a pending IR defer and the compiler ABORTED
     * ("emit_defers_from reached with N pending defers") on the extremely common
     * `defer free(x); arr[i]=…` idiom. */
    if (e->defer_stack.count <= base) return;
    if (!e->cur_ir_func) {
        /* No IR function context (AST/global-init path) should never have a
         * pending defer — that path has no defer statements. Loud if it does. */
        fprintf(stderr, "INTERNAL ERROR: emit_defers_from reached with %d pending "
                        "defers but no IR function context. Please report.\n",
                        e->defer_stack.count - base);
        abort();
    }
    IRFunc *func = (IRFunc *)e->cur_ir_func;
    for (int di = e->defer_stack.count - 1; di >= base; di--) {
        Node *db = e->defer_stack.stmts[di];
        if (!db) continue;
        emit_defer_body(e, func, db);
    }
}

/* emit ALL defers (for return — must fire every scope's defers) */
static void emit_defers(Emitter *e) {
    emit_defers_from(e, 0);
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
    /* Stage 2 Part B (2026-04-28): every TYNODE_ kind is enumerated as
     * a case above. -Wswitch enforces exhaustiveness — adding a new
     * TYNODE_ kind without a case here triggers a compile error. */
    }
    return ty_void;  /* defensive — unreachable */
}

/* ================================================================
 * TOP-LEVEL DECLARATION EMISSION
 * ================================================================ */

/* Find the container_instances[] index whose stamped struct IS `t` (pointer
 * identity), or -1. */
static int container_inst_index_of(Emitter *e, Type *t) {
    if (!e->checker || !t) return -1;
    for (int ci = 0; ci < e->checker->container_inst_count; ci++)
        if (e->checker->container_instances[ci].stamped_struct == t) return ci;
    return -1;
}

/* G5 (2026-08-02): emit one stamped container struct, but FIRST (recursively,
 * post-order) emit any stamped container it holds BY VALUE — a bare by-value
 * field, or an array of one. Both need the COMPLETE type in C.
 *
 * Pointer / optional / slice fields impose NO ordering dependency: a pointer to
 * an incomplete type is legal C, and that is precisely what lets the
 * `?*LNode(T) next` linked list (J4) work.
 *
 * WHY THIS IS REQUIRED BY J4, and only by J4: the tie-the-knot registration in
 * checker.c records a container in container_instances[] BEFORE resolving its
 * fields, so creation order can now place an OUTER stamp ahead of an INNER
 * stamp it holds by value (`container Outer(T){ Inner(T) x; }`). Emitting in
 * registration order then gives GCC "field 'x' has incomplete type". Verified:
 * with J4 applied and this fix absent, all three probe shapes (2-level nest,
 * 3-level nest, array-of-container) fail; without J4 none of them do. This DFS
 * decouples emission order from registration order.
 *
 * `inprog` guards a by-value cycle, which the checker already rejects (J4's
 * self-containment guard) — the guard here just prevents infinite recursion if
 * one ever slips through. */
static void emit_one_container_struct(Emitter *e, int ci, char *emitted, char *inprog) {
    if (emitted[ci] || inprog[ci]) return;
    Type *st = e->checker->container_instances[ci].stamped_struct;
    if (!st || type_dispatch_kind(st) != TYPE_STRUCT) { emitted[ci] = 1; return; }
    inprog[ci] = 1;
    for (uint32_t fi = 0; fi < st->struct_type.field_count; fi++) {
        Type *ft = type_unwrap_distinct(st->struct_type.fields[fi].type);
        while (type_dispatch_kind(ft) == TYPE_ARRAY)
            ft = type_unwrap_distinct(ft->array.inner);
        if (type_dispatch_kind(ft) == TYPE_STRUCT) {
            int dep = container_inst_index_of(e, ft);
            if (dep >= 0 && dep != ci) emit_one_container_struct(e, dep, emitted, inprog);
        }
    }
    inprog[ci] = 0;
    if (emitted[ci]) return;
    emitted[ci] = 1;
    /* BUG-867: spell the DEFINITION the same way every REFERENCE is spelled.
     * This used to emit the bare `st->struct_type.name` while a variable of the
     * type went through EMIT_STRUCT_NAME, which prepends `module_prefix`. For a
     * stamp created inside an imported module that meant the definition said
     * `struct Box_u32` and the use said `struct lib2__Box_u32` — GCC saw an
     * incomplete type, so a `container` template used across a module boundary
     * did not compile. */
    emit(e, "struct ");
    EMIT_STRUCT_NAME(e, st);
    emit(e, " {\n");
    e->indent++;
    for (uint32_t fi = 0; fi < st->struct_type.field_count; fi++) {
        SField *sf = &st->struct_type.fields[fi];
        emit_indent(e);
        emit_type_and_name(e, sf->type, sf->name, sf->name_len);
        emit(e, ";\n");
    }
    e->indent--;
    emit(e, "};\n\n");
    record_user_type_emitted(e, st);   /* BUG-1027: exotic-slice dependency set */
}

/* Emit all stamped container struct declarations, topologically ordered so a
 * by-value container field's struct is always complete before its user. */
static void emit_container_structs(Emitter *e) {
    if (!e->checker || e->checker->container_inst_count == 0) return;
    /* BUG-867: once per BUILD, not once per module — the instance list is shared
     * (a stamp is keyed by template + type argument, not by which module wrote
     * it), so the per-module call emitted `struct Box_u32` twice and GCC
     * reported a redefinition. Any `container` template used across a module
     * boundary hit this. */
    if (e->container_structs_emitted) return;
    e->container_structs_emitted = true;
    int n = e->checker->container_inst_count;
    char *emitted = (char *)calloc((size_t)n, 1);
    char *inprog = (char *)calloc((size_t)n, 1);
    if (!emitted || !inprog) { free(emitted); free(inprog); return; }
    for (int ci = 0; ci < n; ci++) emit_one_container_struct(e, ci, emitted, inprog);
    free(emitted);
    free(inprog);
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
    record_user_type_emitted(e, st);   /* BUG-1027: exotic-slice dependency set */
}

/* ================================================================
 * ASYNC FUNCTION EMISSION
 * Transforms async functions into state machine struct + poll function.
 * Uses Duff's device: switch(state) { case 0: ... case N: ... }
 * can jump INTO loops — valid C since 1983.
 * ================================================================ */

/* Refactor 2: unified async orelse block emission (implementation).
 * Forward-declared near top of file. */

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


static void emit_rewritten_node(Emitter *e, Node *node, IRFunc *func); /* forward decl for emit_structured_asm */

/* D-Alpha-7.5 Session B — Map x86_64 register name to GCC inline-asm constraint
 * letter. Returns NULL if unknown (Session C will add full per-arch tables).
 *
 * GCC machine constraints (x86 family):
 *   "a" = rax/eax/ax/al    "b" = rbx/ebx/bx/bl
 *   "c" = rcx/ecx/cx/cl    "d" = rdx/edx/dx/dl
 *   "S" = rsi/esi/si       "D" = rdi/edi/di
 *   "r" = any general-purpose register (fallback)
 *
 * The width (64/32/16/8) is determined by the bound expression's TYPE in GCC,
 * not by which register name we picked. So writing "rax" with a u32 bound
 * variable still works — GCC uses eax. */
static const char *asm_register_to_gcc_constraint(const char *name, size_t len) {
    if (len == 0) return NULL;
    /* Common x86 GP registers */
    if ((len == 3 && (memcmp(name, "rax", 3) == 0 || memcmp(name, "eax", 3) == 0))
        || (len == 2 && memcmp(name, "ax", 2) == 0)) return "a";
    if ((len == 3 && (memcmp(name, "rbx", 3) == 0 || memcmp(name, "ebx", 3) == 0))
        || (len == 2 && memcmp(name, "bx", 2) == 0)) return "b";
    if ((len == 3 && (memcmp(name, "rcx", 3) == 0 || memcmp(name, "ecx", 3) == 0))
        || (len == 2 && memcmp(name, "cx", 2) == 0)) return "c";
    if ((len == 3 && (memcmp(name, "rdx", 3) == 0 || memcmp(name, "edx", 3) == 0))
        || (len == 2 && memcmp(name, "dx", 2) == 0)) return "d";
    if ((len == 3 && (memcmp(name, "rsi", 3) == 0 || memcmp(name, "esi", 3) == 0))
        || (len == 2 && memcmp(name, "si", 2) == 0)) return "S";
    if ((len == 3 && (memcmp(name, "rdi", 3) == 0 || memcmp(name, "edi", 3) == 0))
        || (len == 2 && memcmp(name, "di", 2) == 0)) return "D";
    /* Generic fallback: "r" = any GPR. Used for register names we don't have
     * a specific letter for (r8-r15, etc.). Session C will replace this with
     * proper per-arch validation tables. */
    return "r";
}

/* D-Alpha-7.5 Session B — Emit a structured asm block as GCC inline asm.
 * Format: __asm__ __volatile__ ("instructions" : outputs : inputs : clobbers)
 * Each operand: "=CONSTRAINT"(expr) for outputs, "CONSTRAINT"(expr) for inputs.
 * Clobbers are quoted strings.
 * Safety string emitted as a preceding comment for audit-trail visibility. */
static void emit_structured_asm(Emitter *e, Node *a, IRFunc *func) {
    /* Audit-trail comment: every escape hatch documents itself in emitted C. */
    emit(e, "/* asm safety: %.*s */\n",
         (int)a->asm_stmt.safety_len, a->asm_stmt.safety);
    emit_indent(e);
    emit(e, "__asm__ __volatile__ (\"%.*s\"",
         (int)a->asm_stmt.instructions_len, a->asm_stmt.instructions);

    /* Outputs */
    if (a->asm_stmt.output_count > 0 || a->asm_stmt.input_count > 0
        || a->asm_stmt.clobber_count > 0) {
        emit(e, " : ");
        for (int i = 0; i < a->asm_stmt.output_count; i++) {
            if (i > 0) emit(e, ", ");
            AsmOperand *op = &a->asm_stmt.outputs[i];
            const char *cstr = asm_register_to_gcc_constraint(op->reg_name, op->reg_name_len);
            emit(e, "\"=%s\"(", cstr ? cstr : "r");
            emit_rewritten_node(e, op->expr, func);
            emit(e, ")");
        }
    }
    /* Inputs */
    if (a->asm_stmt.input_count > 0 || a->asm_stmt.clobber_count > 0) {
        emit(e, " : ");
        for (int i = 0; i < a->asm_stmt.input_count; i++) {
            if (i > 0) emit(e, ", ");
            AsmOperand *op = &a->asm_stmt.inputs[i];
            const char *cstr = asm_register_to_gcc_constraint(op->reg_name, op->reg_name_len);
            emit(e, "\"%s\"(", cstr ? cstr : "r");
            emit_rewritten_node(e, op->expr, func);
            emit(e, ")");
        }
    }
    /* Clobbers */
    if (a->asm_stmt.clobber_count > 0) {
        emit(e, " : ");
        for (int i = 0; i < a->asm_stmt.clobber_count; i++) {
            if (i > 0) emit(e, ", ");
            AsmOperand *op = &a->asm_stmt.clobbers[i];
            emit(e, "\"%.*s\"", (int)op->reg_name_len, op->reg_name);
        }
    }
    emit(e, ");\n");
}

/* BUG-651 fix (2026-05-02): single source of truth for function-level
 * GCC attributes. Called from BOTH the AST proto-only path
 * (emit_func_decl) AND the IR body-emitting path
 * (emit_regular_func_from_ir).
 *
 * Why a helper, not duplicated code: when the IR migration moved
 * function-body emission off the AST path, the early-return at
 * emit_func_decl line ~3681 happened BEFORE the inline attribute
 * emission. This silently dropped section/static attributes for any
 * function with a body. The first IR-side fix duplicated the inline
 * code into the IR path — but that just doubled the surface for the
 * NEXT attribute regression. One helper, two callers, future
 * `noreturn` / `weak` / `visibility` additions land in ONE place.
 *
 * `naked` is intentionally NOT emitted here. The IR migration
 * silently dropped it; existing tests/zer/asm_*.zer rely on the
 * implicit prologue/epilogue (their asm bodies omit explicit `ret`).
 * Restoring real naked semantics is a separate user-visible breaking
 * change tracked in docs/limitations.md.
 *
 * Caller passes the AST function-decl node (NODE_FUNC_DECL or
 * NODE_INTERRUPT). For interrupts, the GCC `interrupt` attribute is
 * emitted by the IR path's existing inline code — this helper covers
 * regular function attributes. */
static void emit_func_attributes(Emitter *e, Node *fn) {
    if (!fn || fn->kind != NODE_FUNC_DECL) return;
    if (fn->func_decl.section) {
        emit(e, "__attribute__((section(\"%.*s\"))) ",
             (int)fn->func_decl.section_len, fn->func_decl.section);
    }
    /* naked: see comment above. Intentionally disabled. */
    if (fn->func_decl.is_static) {
        emit(e, "static ");
    }
}

static void emit_func_decl(Emitter *e, Node *node) {
    ZTRACE("EMIT  function '%.*s'", (int)node->func_decl.name_len, node->func_decl.name);

    /* Function bodies are IR-only (no AST fallback). The AST emission
     * path drifted behind IR as features landed — silent fallback
     * masked bugs. Post-2026-04-19 policy: IR correctness is
     * load-bearing. Lowering or validation failure = abort. */
    if (node->func_decl.body) {
        /* BUG-1238: an async function lowered early (its state struct is already
         * out) is not lowered again — pre_lower_orelse rewrites the AST. */
        for (int ai = 0; ai < e->early_async_count; ai++) {
            IRFunc *pre = (IRFunc *)e->early_async_ir[ai];
            if (pre && pre->ast_node == node) { emit_func_from_ir(e, pre); return; }
        }
        IRFunc *ir = ir_lower_func(e->arena, e->checker, node);
        if (!ir) {
            fprintf(stderr,
                    "INTERNAL ERROR: IR lowering returned NULL for '%.*s' "
                    "at %s:%d — please report with a minimal reproducer.\n",
                    (int)node->func_decl.name_len, node->func_decl.name,
                    e->source_file ? e->source_file : "<unknown>",
                    node->loc.line);
            abort();
        }
        ir->module_prefix = e->current_module;
        ir->module_prefix_len = e->current_module_len;
        if (!ir_validate(ir)) {
            fprintf(stderr,
                    "INTERNAL ERROR: IR validation failed for '%.*s' "
                    "at %s:%d — please report with a minimal reproducer.\n",
                    (int)node->func_decl.name_len, node->func_decl.name,
                    e->source_file ? e->source_file : "<unknown>",
                    node->loc.line);
            abort();
        }
        /* Phase F: run IR analysis hook (zercheck_ir) on the lowered IR
         * BEFORE emitting C. Single ir_lower_func call per function avoids
         * AST re-mutation from pre_lower_orelse's destructive rewrite. */
        if (e->ir_hook) {
            e->ir_hook(e->ir_hook_ctx, ir);
        }
        emit_func_from_ir(e, ir);
        return;
    }

    Type *func_type = checker_get_type(e->checker,node);
    Type *ret = (func_type && func_type->kind == TYPE_FUNC_PTR) ?
        func_type->func_ptr.ret : NULL;

    /* Function-level GCC attributes (section/static/naked).
     * Single source of truth via helper — see emit_func_attributes(). */
    emit_func_attributes(e, node);

    emit_func_decl_head(e, ret, false);                 /* BUG-1111 */
    EMIT_MANGLED_NAME(e, node->func_decl.name, node->func_decl.name_len);
    emit_func_decl_params(e, node, func_type);
    emit_func_decl_tail(e, ret, false);
    emit(e, " ");

    /* Prototype-only path — functions with bodies took the IR return
     * at the top of this function. Only prototype / forward-decl shapes
     * reach here. */
    emit(e, ";\n\n");
}

/* BUG-1128: a PROTOTYPE for every function with a body, emitted before any
 * function or global that could name it. The checker registers every
 * declaration before checking any body, so ZER lets a function be used before
 * its definition — and the emitter wrote no prototype, leaving C's implicit
 * `int f()` declaration: `u64 later()` called from an earlier `main` was
 * "conflicting types for 'later'", a struct / optional / pointer return the same,
 * and a funcptr global initialised with a later function "undeclared". GCC 14
 * makes the implicit declaration itself an error. Same helpers as the definition
 * (attributes, head, mangled name, params, tail), so the two cannot disagree.
 * `main` is skipped (its return type may be promoted to int); comptime and async
 * functions do not exist as plain C functions. */
static void emit_func_prototype(Emitter *e, Node *node) {
    if (node->kind != NODE_FUNC_DECL || !node->func_decl.body) return;
    if (node->func_decl.is_comptime || node->func_decl.is_async) return;
    if (!e->current_module && node->func_decl.name_len == 4 &&
        memcmp(node->func_decl.name, "main", 4) == 0) return;
    Type *func_type = checker_get_type(e->checker, node);
    Type *ret = (func_type && func_type->kind == TYPE_FUNC_PTR) ?
        func_type->func_ptr.ret : NULL;
    emit_func_attributes(e, node);
    emit_func_decl_head(e, ret, false);
    EMIT_MANGLED_NAME(e, node->func_decl.name, node->func_decl.name_len);
    emit_func_decl_params(e, node, func_type);
    emit_func_decl_tail(e, ret, false);
    emit(e, ";\n");
}

/* BUG-1177: `typedef struct _zer_async_NAME _zer_async_NAME;` for every async
 * function, before any prototype. The state struct is defined where the async
 * function itself is emitted (in declaration order), so without this a function
 * declared EARLIER that takes `*_zer_async_NAME` named an unknown type — and the
 * pointer is the only legal way to pass a task. Names are unmangled, matching
 * emit_async_func_from_ir (BUG-866). */
static void emit_async_forward_typedefs(Emitter *e, Node *file_node) {
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *d = file_node->file.decls[i];
        if (d->kind != NODE_FUNC_DECL || !d->func_decl.is_async || !d->func_decl.body)
            continue;
        emit(e, "typedef struct _zer_async_%.*s _zer_async_%.*s;\n",
             (int)d->func_decl.name_len, d->func_decl.name,
             (int)d->func_decl.name_len, d->func_decl.name);
    }
}

/* BUG-1238: define every async state struct BEFORE any function or global.
 * The struct is emitted from the lowered IR (it holds every promoted local),
 * and was written where the async function itself is emitted — in source
 * order. So every use of a task spelled before that point met an incomplete
 * type: a GLOBAL task, a task in a struct FIELD, `alloc(task, n)`, or even a
 * LOCAL task in a function declared above the async one ("has initializer but
 * incomplete type"). The async functions are lowered here instead (once — the
 * lowered IR is kept and reused when the function body is emitted), and their
 * structs defined in dependency order: a task holding another task BY VALUE as
 * a local needs that one first. */
static void emit_async_state_struct(Emitter *e, IRFunc *func);
static bool async_local_needs(Type *t, IRFunc **fs, bool *done, int n, int depth) {
    if (!t || depth > 64) return false;
    Type *u = type_unwrap_distinct(t);
    if (!u) return false;
    switch (type_dispatch_kind(u)) {
    case TYPE_ARRAY: return async_local_needs(u->array.inner, fs, done, n, depth + 1);
    case TYPE_OPTIONAL: return async_local_needs(u->optional.inner, fs, done, n, depth + 1);
    case TYPE_STRUCT:
        if (u->struct_type.is_async_state) {
            for (int i = 0; i < n; i++) {
                if (done[i]) continue;
                uint32_t fl = fs[i]->name_len;
                if (u->struct_type.name_len == 11 + fl &&
                    memcmp(u->struct_type.name + 11, fs[i]->name, fl) == 0) return true;
            }
            return false;
        }
        for (uint32_t i = 0; i < u->struct_type.field_count; i++)
            if (async_local_needs(u->struct_type.fields[i].type, fs, done, n, depth + 1))
                return true;
        return false;
    default: return false;
    }
}
static void emit_early_async_structs(Emitter *e, Node *file_node) {
    int start = e->early_async_count;
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *d = file_node->file.decls[i];
        if (d->kind != NODE_FUNC_DECL || !d->func_decl.is_async || !d->func_decl.body)
            continue;
        IRFunc *ir = ir_lower_func(e->arena, e->checker, d);
        if (!ir) {
            fprintf(stderr, "INTERNAL ERROR: IR lowering returned NULL for async '%.*s'\n",
                    (int)d->func_decl.name_len, d->func_decl.name);
            abort();
        }
        ir->module_prefix = e->current_module;
        ir->module_prefix_len = e->current_module_len;
        if (!ir_validate(ir)) {
            fprintf(stderr, "INTERNAL ERROR: IR validation failed for async '%.*s'\n",
                    (int)d->func_decl.name_len, d->func_decl.name);
            abort();
        }
        if (e->ir_hook) e->ir_hook(e->ir_hook_ctx, ir);
        if (e->early_async_count >= e->early_async_cap) {
            int nc = e->early_async_cap < 8 ? 8 : e->early_async_cap * 2;
            void **nb = (void **)arena_alloc(e->arena, (size_t)nc * sizeof(void *));
            if (!nb) abort();
            if (e->early_async_count)
                memcpy(nb, e->early_async_ir, (size_t)e->early_async_count * sizeof(void *));
            e->early_async_ir = nb;
            e->early_async_cap = nc;
        }
        e->early_async_ir[e->early_async_count++] = ir;
    }
    int n = e->early_async_count - start;
    if (n <= 0) return;
    IRFunc **fs = (IRFunc **)(e->early_async_ir + start);
    bool *done = (bool *)arena_alloc(e->arena, (size_t)n * sizeof(bool));
    if (!done) abort();
    memset(done, 0, (size_t)n * sizeof(bool));
    int left = n;
    while (left > 0) {
        bool progress = false;
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            bool blocked = false;
            for (int li = 0; li < fs[i]->local_count && !blocked; li++) {
                if (fs[i]->locals[li].is_static) continue;
                done[i] = true;   /* a task never waits on itself */
                blocked = async_local_needs(fs[i]->locals[li].type, fs, done, n, 0);
                done[i] = false;
            }
            if (blocked) continue;
            emit_async_state_struct(e, fs[i]);
            done[i] = true; left--; progress = true;
        }
        if (!progress) {   /* a cycle by value is unrepresentable in C: emit anyway, GCC reports it */
            for (int i = 0; i < n; i++)
                if (!done[i]) { emit_async_state_struct(e, fs[i]); done[i] = true; left--; }
        }
    }
}

static void emit_global_var_inner(Emitter *e, Node *node);
static void emit_global_var(Emitter *e, Node *node) {
    /* BUG-997: mark the global-initializer context for the whole emission, so a
     * NODE_IDENT naming a const global anywhere in the initializer is replaced by
     * that global's own initializer rather than emitted as a name (invalid C). */
    e->global_init_depth++;
    emit_global_var_inner(e, node);
    e->global_init_depth--;
}
static void emit_global_var_inner(Emitter *e, Node *node) {
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
        emit_module_global_name(e, node->var_decl.name, (uint32_t)node->var_decl.name_len);   /* BUG-1040 */
        emit(e, " = {0};\n\n");
        return;
    }

    /* Ring(T, N) → ring struct */
    if (type && type->kind == TYPE_RING) {
        emit(e, "struct { ");
        emit_type(e, type->ring.elem);
        emit(e, " data[%llu]; uint32_t head; uint32_t tail; uint32_t count; } ",
             (unsigned long long)type->ring.count);
        emit_module_global_name(e, node->var_decl.name, (uint32_t)node->var_decl.name_len);   /* BUG-1040 */
        emit(e, " = {0};\n\n");
        return;
    }

    /* Arena → _zer_arena */
    if (type && type->kind == TYPE_ARENA) {
        emit(e, "_zer_arena ");
        emit_module_global_name(e, node->var_decl.name, (uint32_t)node->var_decl.name_len);   /* BUG-1040 */
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
        emit(e, "_zer_slab ");
        emit_module_global_name(e, node->var_decl.name, (uint32_t)node->var_decl.name_len);   /* BUG-1040 */
        emit(e, " = { .slot_size = sizeof(");
        emit_type(e, type->slab.elem);
        emit(e, ") };\n\n");
        return;
    }

    if (node->var_decl.is_static) emit(e, "static ");
    /* volatile on non-pointer scalars (pointers handled above) */
    if (node->var_decl.is_volatile && !(type && type_unwrap_distinct(type)->kind == TYPE_POINTER))
        emit(e, "volatile ");
    /* BUG-1115: a ZER `const` global is read-only by the checker's rules and was
     * emitted as a plain C object — so it occupied RAM (.data) rather than flash
     * (.rodata), contrary to the reference. Emit C `const` for value-shaped
     * types. Pointer / slice / funcptr kinds are left alone: there `const` in
     * ZER qualifies the POINTEE, which the type already spells. */
    if (node->var_decl.is_const && type) {
        switch (type_dispatch_kind(type)) {
        case TYPE_POINTER: case TYPE_SLICE: case TYPE_FUNC_PTR: case TYPE_OPAQUE:
        case TYPE_OPTIONAL: case TYPE_POOL: case TYPE_RING: case TYPE_SLAB:
        case TYPE_ARENA: case TYPE_BARRIER: case TYPE_SEMAPHORE: case TYPE_HANDLE:
            break;
        default:
            emit(e, "const ");
            break;
        }
    }

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
        Type *gi_type = checker_get_type(e->checker, node->var_decl.init);
        if (gtype_eff && gtype_eff->kind == TYPE_OPTIONAL &&
            !is_null_sentinel(gtype_eff->optional.inner) &&
            node->var_decl.init->kind == NODE_NULL_LIT) {
            emit(e, " = ");
            emit_opt_null_literal(e, type);
        } else if (gtype_eff && type_dispatch_kind(gtype_eff) == TYPE_OPTIONAL &&
                   !is_null_sentinel(gtype_eff->optional.inner) &&
                   !is_void_opt(type) &&
                   !(gi_type && type_dispatch_kind(gi_type) == TYPE_OPTIONAL)) {
            /* BUG-943: a value-optional global initialised with its bare PAYLOAD.
             * This branch handled exactly ONE optional shape — the null literal —
             * and everything else fell through to the scalar path below, so
             *     ?u32 g = 5;   ->   _zer_opt_u32 g = 5;
             * i.e. `gcc: error: invalid initializer`: a valid ZER program that
             * could not be BUILT. Measured on four shapes (?u32, ?bool, ?enum,
             * ?i64); only `= null` worked, because only `= null` had an arm.
             *
             * The payload must be const-FOLDED rather than emitted as an
             * expression, for the same reason BUG-939 folds below: a tree needing
             * `_zer_shl` emits a GCC statement expression, which is illegal at
             * file scope. When it does not fold, emit_opt_wrap_value is the shared
             * T -> ?T query (the same one assignment and struct-field init use). */
            /* BUG-1090: the TYPED fold first — the value a local of the same
             * spelling computes. The untyped fold is the fallback for a tree
             * the typed one does not model. */
            int64_t oval;
            if (!checker_fold_const_typed(e->checker, node->var_decl.init, &oval))
                oval = eval_const_expr(node->var_decl.init);
            if (oval != CONST_EVAL_FAIL) {
                oval = fold_wrap_to_type(oval, gi_type);   /* BUG-1032 */
                emit(e, " = (");
                emit_type(e, type);
                if (oval < 0) emit(e, "){ (%lld), 1 }", (long long)oval);
                else          emit(e, "){ %lluULL, 1 }", (unsigned long long)oval);
            } else {
                emit(e, " = ");
                emit_opt_wrap_value(e, type, node->var_decl.init);
            }
        } else {
            /* For const globals: try compile-time evaluation first.
             * This avoids GCC statement expression errors from _zer_shl/shr
             * macros which can't be used in global initializers. */
            bool emitted_const = false;
            /* BUG-939: the gate was `is_const`, so a NON-const global whose
             * initializer needs a macro emitted the macro — and `_zer_shl` is a GCC
             * STATEMENT EXPRESSION, which is illegal at file scope:
             *     i64 g = -(1 << 4);   ->  int64_t g = (-_zer_shl(1LL, 4LL));
             *                              error: braced-group ... only inside a function
             * i.e. a valid ZER program that could not be BUILT. Constness has nothing
             * to do with it: what matters is whether the initializer FOLDS, and a
             * foldable tree is identical either way. eval_const_expr returns
             * CONST_EVAL_FAIL for anything with a variable in it, so widening the
             * gate cannot fold something it should not. */
            /* BUG-1216: a comptime call whose result is a STRUCT (or a float)
             * is not an integer fold — its `comptime_value` slot is unused, 0 —
             * and `In g = MK(3);` was emitted `struct In g = 0;`. Emit the folded
             * literal instead. */
            Node *gci = node->var_decl.init;
            if (gci->kind == NODE_CALL && gci->call.is_comptime_resolved &&
                (gci->call.comptime_struct_init || gci->call.is_comptime_float)) {
                emit(e, " = ");
                emit_expr(e, gci);
                emitted_const = true;
            }
            if (!emitted_const) {
                /* BUG-1090: typed fold first (see the optional arm above).
                 * `const u32 K = (0 - 1) / 1073741824;` emitted `K = 0` — the
                 * untyped int64 reading — while the same initializer on a
                 * LOCAL computes 3 at run time. */
                int64_t cval;
                if (!checker_fold_const_typed(e->checker, node->var_decl.init, &cval))
                    cval = eval_const_expr(node->var_decl.init);
                if (cval != CONST_EVAL_FAIL) {
                    cval = fold_wrap_to_type(cval, gi_type);   /* BUG-1032 */
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
                /* BUG-1127: the array -> slice coercion, at the GLOBAL value-flow
                 * site. `[*]u8 s = buf;` emitted `s = buf;` ("invalid initializer"
                 * from GCC) — masked until now because BUG-997's checker rule
                 * refused the bare global name before the emitter saw it. */
                Type *gd = type_unwrap_distinct(type);
                Type *gv = gi_type ? type_unwrap_distinct(gi_type) : NULL;
                if (gd && gv && type_dispatch_kind(gd) == TYPE_SLICE &&
                    type_dispatch_kind(gv) == TYPE_ARRAY)
                    emit_array_as_slice(e, node->var_decl.init, gv, gd);
                else
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
    case NODE_YIELD: case NODE_AWAIT:
        break;
    /* Stage 2 Part B (2026-04-28): exhaustive — kinds without a body
     * that could contain a NODE_SPAWN to prescan. NODE_STATIC_ASSERT
     * is a compile-time-only check with no spawn inside. */
    case NODE_FILE: case NODE_STRUCT_DECL: case NODE_ENUM_DECL:
    case NODE_UNION_DECL: case NODE_TYPEDEF: case NODE_IMPORT:
    case NODE_CINCLUDE: case NODE_MMIO: case NODE_GLOBAL_VAR:
    case NODE_CONTAINER_DECL: case NODE_VAR_DECL: case NODE_RETURN:
    case NODE_BREAK: case NODE_CONTINUE: case NODE_GOTO:
    case NODE_LABEL: case NODE_ASM: case NODE_STATIC_ASSERT:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_IDENT: case NODE_BINARY: case NODE_UNARY:
    case NODE_ASSIGN: case NODE_CALL: case NODE_FIELD:
    case NODE_INDEX: case NODE_SLICE: case NODE_ORELSE:
    case NODE_INTRINSIC: case NODE_CAST: case NODE_TYPECAST:
    case NODE_SIZEOF: case NODE_STRUCT_INIT:
        break;
    }
}

static void emit_type(Emitter *e, Type *t); /* forward decl */

/* BUG-865: the C name of a spawn target, module-mangled when it is imported.
 *
 * `spawn tick();` where `tick` comes from `import safe_mod` emitted a forward
 * declaration `void tick();` and a wrapper body calling `tick()` — the
 * UNMANGLED name — while the module's definition is `safe_mod__tick`. Nothing
 * defines `tick`, so the program does not LINK. The checker accepts it (the
 * data-race scan even resolves the imported body correctly), so the only
 * signal is `ld returned 1 exit status` with no source line: spawning an
 * imported function was completely non-functional.
 *
 * The rule is the one `emit_expr`'s NODE_IDENT arm already uses for an ordinary
 * call — if the symbol carries a `module_prefix`, emit `prefix__name`. It was
 * spelled raw at FOUR spawn sites (two forward-declaration arms, two wrapper
 * bodies), which is why no single fix existed. One helper, four call sites. */
static void emit_spawn_target_name(Emitter *e, const char *name, size_t len) {
    Symbol *s = scope_lookup(e->checker->global_scope, name, (uint32_t)len);
    if (s && s->module_prefix) {
        emit(e, "%.*s__%.*s", (int)s->module_prefix_len, s->module_prefix,
             (int)len, name);
        return;
    }
    emit(e, "%.*s", (int)len, name);
}

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
            emit(e, " ");
            emit_spawn_target_name(e, sn->spawn_stmt.func_name,
                                   sn->spawn_stmt.func_name_len);
            emit(e, "(");
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
                    emit(e, " ");
                    emit_spawn_target_name(e, sn->spawn_stmt.func_name,
                                           sn->spawn_stmt.func_name_len);
                    emit(e, "(");
                    for (uint32_t pi = 0; pi < ret->func_ptr.param_count; pi++) {
                        if (pi > 0) emit(e, ", ");
                        emit_type(e, ret->func_ptr.params[pi]);
                    }
                    emit(e, ");\n");
                } else {
                    /* Fallback: just emit void func_name(); */
                    emit(e, "void ");
                    emit_spawn_target_name(e, sn->spawn_stmt.func_name,
                                           sn->spawn_stmt.func_name_len);
                    emit(e, "();\n");
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
            /* Emit arg struct typedef.  Field types follow the WORKER's
             * PARAM types, NOT the arg expression types, because the
             * wrapper later calls `worker(_a->a0, _a->a1, ...)` and
             * worker expects its parameter types.  Falling back to arg
             * type only when param type is unavailable (extern with no
             * signature, etc.) — coercion at the field-assignment site
             * (further below) bridges the gap. Discovered 2026-05-29:
             * spawn worker(42) where worker takes ?u32 silently
             * miscompiled because struct field was emitted as uint32_t
             * but worker takes _zer_opt_u32. */
            Symbol *worker_sym = scope_lookup(e->checker->global_scope,
                sn->spawn_stmt.func_name,
                (uint32_t)sn->spawn_stmt.func_name_len);
            Type *worker_ft = NULL;
            if (worker_sym && worker_sym->type) {
                Type *wt = type_unwrap_distinct(worker_sym->type);
                if (wt->kind == TYPE_FUNC_PTR) worker_ft = wt;
            }
            emit(e, "struct _zer_spawn_args_%d { ", sid);
            for (int i = 0; i < ac; i++) {
                Type *field_type = NULL;
                if (worker_ft && (uint32_t)i < worker_ft->func_ptr.param_count) {
                    field_type = worker_ft->func_ptr.params[i];
                }
                if (!field_type) {
                    field_type = checker_get_type(e->checker, sn->spawn_stmt.args[i]);
                }
                if (field_type) {
                    /* BUG-465: use emit_type_and_name with actual field name.
                     * For function pointers, name must be inside (*name)(params).
                     * Passing NULL + separate name breaks funcptr emission. */
                    char fname[8];
                    int flen = snprintf(fname, sizeof(fname), "a%d", i);
                    emit_type_and_name(e, field_type, fname, flen);
                    emit(e, "; ");
                }
            }
            emit(e, "};\n");
        }

        /* Emit wrapper function */
        emit(e, "static void *_zer_spawn_wrap_%d(void *_raw) {\n", sid);
        if (ac > 0) {
            emit(e, "    struct _zer_spawn_args_%d *_a = (struct _zer_spawn_args_%d *)_raw;\n", sid, sid);
            emit(e, "    ");
            emit_spawn_target_name(e, sn->spawn_stmt.func_name,
                                   sn->spawn_stmt.func_name_len);
            emit(e, "(");
            for (int i = 0; i < ac; i++) {
                if (i > 0) emit(e, ", ");
                emit(e, "_a->a%d", i);
            }
            emit(e, ");\n");
            emit(e, "    free(_a);\n");
        } else {
            emit(e, "    ");
            emit_spawn_target_name(e, sn->spawn_stmt.func_name,
                                   sn->spawn_stmt.func_name_len);
            emit(e, "();\n");
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
        record_user_type_emitted(e, ut);   /* BUG-1027: exotic-slice dependency set */
        break;
        #undef EMIT_UNAME
    }

    case NODE_ENUM_DECL: {
        Type *et = checker_get_type(e->checker, decl);
        emit(e, "/* enum %.*s */\n",
             (int)decl->enum_decl.name_len, decl->enum_decl.name);
        /* BUG-1191: print the CHECKER's values — the one fold, so the emitted
         * #define and every compile-time decision agree by construction. */
        Type *ete = et ? type_unwrap_distinct(et) : NULL;
        for (int j = 0; j < decl->enum_decl.variant_count; j++) {
            EnumVariant *v = &decl->enum_decl.variants[j];
            int32_t val = (ete && type_dispatch_kind(ete) == TYPE_ENUM &&
                           (uint32_t)j < ete->enum_type.variant_count)
                ? ete->enum_type.variants[j].value : j;
            emit(e, "#define _ZER_");
            if (et) EMIT_ENUM_NAME(e, et);
            else emit(e, "%.*s", (int)decl->enum_decl.name_len, decl->enum_decl.name);
            emit(e, "_%.*s %d\n", (int)v->name_len, v->name, val);
        }
        emit(e, "\n");
        break;
    }

    case NODE_IMPORT:
        /* Imports are resolved during checking — module symbols emitted via
         * topological order across files. Nothing to emit at the C level.
         * Previous "import N TODO" comment-only emission polluted compiled
         * output and matched the dead-stub pattern grep in the diff audit. */
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
        /* Interrupt handlers route through IR like regular function bodies
         * (post-2026-04-19 IR-only policy). `emit_regular_func_from_ir`
         * detects func->is_interrupt and emits the __attribute__((interrupt))
         * + _IRQHandler(void) signature; the body emits as normal IR blocks. */
        if (decl->interrupt.body) {
            IRFunc *ir = ir_lower_interrupt(e->arena, e->checker, decl);
            if (!ir) {
                fprintf(stderr,
                        "INTERNAL ERROR: IR lowering returned NULL for interrupt "
                        "'%.*s' at %s:%d — please report with a minimal reproducer.\n",
                        (int)decl->interrupt.name_len, decl->interrupt.name,
                        e->source_file ? e->source_file : "<unknown>",
                        decl->loc.line);
                abort();
            }
            ir->module_prefix = e->current_module;
            ir->module_prefix_len = e->current_module_len;
            if (!ir_validate(ir)) {
                fprintf(stderr,
                        "INTERNAL ERROR: IR validation failed for interrupt "
                        "'%.*s' at %s:%d — please report with a minimal reproducer.\n",
                        (int)decl->interrupt.name_len, decl->interrupt.name,
                        e->source_file ? e->source_file : "<unknown>",
                        decl->loc.line);
                abort();
            }
            emit_func_from_ir(e, ir);
        } else {
            /* Forward declaration only — emit just the signature. */
            /* 8ezecl (copied): `used` alongside `interrupt` — on baremetal the
             * only reference to the ISR is the vector table in a separate
             * .S/linker script the C TU never sees; without `used`,
             * `gcc -ffunction-sections -Wl,--gc-sections` silently tree-shakes
             * the handler and the vector slot keeps its default trap. */
            emit(e, "void __attribute__((interrupt, used)) %.*s_IRQHandler(void);\n",
                 (int)decl->interrupt.name_len, decl->interrupt.name);
        }
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
    case NODE_DO_WHILE:  /* Stage 2 Part B (2026-04-28): missing kind */
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

        /* BUG-1027: register every exotic slice type, then flush the ones whose
         * dependencies are ready before each declaration that may name one. */
        collect_exotic_slices(e);

        /* Pass 1: struct/enum/union/typedef declarations */
        for (int i = 0; i < file_node->file.decl_count; i++) {
            Node *d = file_node->file.decls[i];
            if (d->kind == NODE_IMPORT || d->kind == NODE_CINCLUDE) continue;
            if (d->kind == NODE_STRUCT_DECL || d->kind == NODE_ENUM_DECL ||
                d->kind == NODE_UNION_DECL || d->kind == NODE_TYPEDEF) {
                flush_exotic_slices(e);
                emit_top_level_decl(e, d, file_node, i);
            }
        }
        flush_exotic_slices(e);

        /* Emit stamped container struct declarations */
        emit_container_structs(e);
        flush_exotic_slices(e);

        /* Spawn wrappers (between struct decls and functions) */
        emit_spawn_wrappers(e);

        /* Pass 2a: globals first (ensures cross-module references work) */
        for (int i = 0; i < file_node->file.decl_count; i++) {
            Node *d = file_node->file.decls[i];
            if (d->kind == NODE_GLOBAL_VAR)
                emit_top_level_decl(e, d, file_node, i);
        }
        /* Pass 2b: functions */
        for (int i = 0; i < file_node->file.decl_count; i++) {
            Node *d = file_node->file.decls[i];
            if (d->kind == NODE_IMPORT || d->kind == NODE_CINCLUDE) continue;
            if (d->kind != NODE_STRUCT_DECL && d->kind != NODE_ENUM_DECL &&
                d->kind != NODE_UNION_DECL && d->kind != NODE_TYPEDEF &&
                d->kind != NODE_CONTAINER_DECL && d->kind != NODE_GLOBAL_VAR)
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
    /* Gap 14 (2026-04-27): force signed-overflow-wraps semantics regardless
     * of caller flags. ZER spec defines signed overflow as wrapping (not C
     * UB). When `zerc --run` invokes gcc, -fwrapv is added; but when a user
     * compiles the emitted .c file directly with their own gcc command, the
     * flag may be missing — silent UB at any optimization level. The pragma
     * enforces wrapv inside the .c file itself. Compilers without pragma
     * support (e.g., clang older than 18) ignore unknown pragmas silently. */
    emit(e, "#if defined(__GNUC__) && !defined(__clang__)\n");
    emit(e, "#pragma GCC optimize(\"wrapv\")\n");
    emit(e, "#endif\n");
    /* BUG-837: every bare-metal branch below keyed on __STDC_HOSTED__ — a
     * COMPILER-FLAG property (it is what -ffreestanding sets), not a target
     * property. zerc never learns which C flags the emitted file will be compiled
     * with, and there was no flag or macro for the user to declare it either. So a
     * bare-metal build whose flags leave __STDC_HOSTED__ at 1 — `gcc -m64 -nostdlib`
     * does exactly that — took every hosted branch.
     *
     * Most of those failures are LOUD (libc includes and the fprintf trap fail to
     * link). ONE is not: @critical on x86 fell through to __atomic_thread_fence,
     * a memory fence that does NOT disable interrupts. An interrupt landing inside
     * a @critical block runs anyway and every invariant the block was protecting is
     * unprotected, with no diagnostic and no fault.
     *
     * Scope, measured rather than assumed: ARM, RISC-V and AVR @critical key on the
     * ARCH macro and have always emitted the correct interrupt-disable sequence
     * regardless of __STDC_HOSTED__. The real exposure is bare-metal x86 — a kernel,
     * a bootloader, an EFI application.
     *
     * A pure WIDENING: a build that does not define ZER_FREESTANDING preprocesses
     * identically, and defining it can never turn a freestanding branch back into a
     * hosted one. */
    emit(e, "#if defined(ZER_FREESTANDING)\n");
    emit(e, "#  define _ZER_HOSTED 0\n");
    emit(e, "#elif defined(__STDC_HOSTED__)\n");
    emit(e, "#  define _ZER_HOSTED __STDC_HOSTED__\n");
    emit(e, "#else\n");
    emit(e, "#  define _ZER_HOSTED 1\n");
    emit(e, "#endif\n");
    /* C99 4.6 guarantees stdint.h and stddef.h on a FREESTANDING implementation
     * too, so they stay outside the gate. */
    emit(e, "#include <stdint.h>\n");
    emit(e, "#include <stddef.h>\n");
    emit(e, "#include <string.h>\n");
    emit(e, "#include <stdio.h>\n");
    emit(e, "#include <stdlib.h>\n");
    /* wasm32-wasi defines __STDC_HOSTED__==1 (it has a libc) but lacks
     * pthread/sched (WASI preview1 is single-threaded). Gate the POSIX-thread
     * headers + helpers on !__wasi__ (clang defines __wasi__ for that target)
     * so single-threaded ZER compiles to wasm; hosted (Linux/gcc) is
     * unaffected since !defined(__wasi__) is true there. Concurrency under
     * wasi stays unsupported (a loud undefined-symbol error at the use site). */
    emit(e, "#if _ZER_HOSTED && !defined(__wasi__)\n");
    emit(e, "#include <pthread.h>\n");
    emit(e, "#include <time.h>\n");
    emit(e, "#include <sched.h>\n");
    emit(e, "#endif\n");
    emit(e, "\n");
    /* B4: @once loser-wait relax — yield so the winner (running the @once body)
     * makes progress while losers spin on the done flag. Hosted only; freestanding
     * @once stays single-core (loser does not wait). */
    emit(e, "#if _ZER_HOSTED && !defined(__wasi__)\n");
    emit(e, "static inline void _zer_once_relax(void) { sched_yield(); }\n");
    emit(e, "#endif\n");
    emit(e, "\n");

    /* ZER optional type definitions */
    emit(e, "/* ZER optional types */\n");
    emit(e, "typedef struct { uint8_t value; uint8_t has_value; } _zer_opt_bool;\n");
    emit(e, "typedef struct { uint8_t value; uint8_t has_value; } _zer_opt_u8;\n");
    emit(e, "typedef struct { uint16_t value; uint8_t has_value; } _zer_opt_u16;\n");
    emit(e, "typedef struct { uint32_t value; uint8_t has_value; } _zer_opt_u32;\n");
    /* Carry-op result structs + helpers (@addc/@subb/@mulw). Spellable names, no _zer_ prefix. */
    emit(e, "typedef struct AddCarry64 { uint64_t sum; uint8_t carry; } AddCarry64;\n");
    emit(e, "static inline AddCarry64 _zer_do_addc_u64(uint64_t a, uint64_t b, uint64_t cin){ uint64_t s; unsigned char c1=__builtin_add_overflow(a,b,&s); unsigned char c2=__builtin_add_overflow(s,cin,&s); return (AddCarry64){ s, (uint8_t)(c1|c2) }; }\n");
    emit(e, "typedef struct SubBorrow64 { uint64_t diff; uint8_t borrow; } SubBorrow64;\n");
    emit(e, "static inline SubBorrow64 _zer_do_subb_u64(uint64_t a, uint64_t b, uint64_t bin){ uint64_t d; unsigned char b1=__builtin_sub_overflow(a,b,&d); unsigned char b2=__builtin_sub_overflow(d,bin,&d); return (SubBorrow64){ d, (uint8_t)(b1|b2) }; }\n");
    emit(e, "typedef struct MulWide64 { uint64_t lo; uint64_t hi; } MulWide64;\n");
    emit(e, "#if defined(__SIZEOF_INT128__)\n");
    emit(e, "static inline MulWide64 _zer_do_mulw_u64(uint64_t a, uint64_t b){ unsigned __int128 p=(unsigned __int128)a*(unsigned __int128)b; return (MulWide64){ (uint64_t)p, (uint64_t)(p>>64) }; }\n");
    emit(e, "#else\n");
    emit(e, "static inline MulWide64 _zer_do_mulw_u64(uint64_t a, uint64_t b){ uint64_t al=a&0xffffffffULL,ah=a>>32,bl=b&0xffffffffULL,bh=b>>32; uint64_t ll=al*bl,lh=al*bh,hl=ah*bl,hh=ah*bh; uint64_t mid=(ll>>32)+(lh&0xffffffffULL)+(hl&0xffffffffULL); uint64_t lo=(ll&0xffffffffULL)|(mid<<32); uint64_t hi=hh+(lh>>32)+(hl>>32)+(mid>>32); return (MulWide64){ lo, hi }; }\n");
    emit(e, "#endif\n");
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

    /* G1 (2026-08-01): 128-bit carriers, for uN/iN with 64 < N <= 128. Needed by
     * intn_carrier_suffix's "i128"/"u128" arm.
     *
     * GUARDED — rdh99l emitted these unconditionally, which breaks any target
     * without __int128 (32-bit ARM, most Cortex-M). The guard is 02nq43's
     * contribution; taking it means a target lacking __int128 still compiles,
     * it just cannot use a >64-bit uN compound type (which it could not have
     * represented anyway). */
    emit(e, "#if defined(__SIZEOF_INT128__)\n");
    emit(e, "typedef struct { unsigned __int128 value; uint8_t has_value; } _zer_opt_u128;\n");
    emit(e, "typedef struct { __int128 value; uint8_t has_value; } _zer_opt_i128;\n");
    emit(e, "typedef struct { unsigned __int128* ptr; size_t len; } _zer_slice_u128;\n");
    emit(e, "typedef struct { __int128* ptr; size_t len; } _zer_slice_i128;\n");
    emit(e, "typedef struct { volatile unsigned __int128* ptr; size_t len; } _zer_vslice_u128;\n");
    emit(e, "typedef struct { volatile __int128* ptr; size_t len; } _zer_vslice_i128;\n");
    emit(e, "typedef struct { _zer_slice_u128 value; uint8_t has_value; } _zer_opt_slice_u128;\n");
    emit(e, "typedef struct { _zer_slice_i128 value; uint8_t has_value; } _zer_opt_slice_i128;\n");
    emit(e, "#endif\n");
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
    /* BUG-1270: each allocator starts its slots' generations at its OWN seed.
     * With every Pool and Slab starting at 1, a Handle from pool A carried the
     * generation pool B's slot also held, so `pb.get(h_from_pa)` passed the check
     * and returned B's live object — through a parameter, a global or a Ring,
     * where no static rule can follow the handle. The seed is derived from the
     * allocator's own address (a Pool's gen array, a Slab's struct), so two live
     * allocators differ and no call site changes. */
    emit(e, "static inline uint32_t _zer_gen_seed(const void *owner) {\n");
    emit(e, "    uint64_t a = (uint64_t)(uintptr_t)owner;\n");
    emit(e, "    uint32_t x = (uint32_t)((a >> 3) ^ (a >> 35)) * 0x9E3779B9u;\n");
    emit(e, "    x ^= x >> 16;\n");
    emit(e, "    return x ? x : 1u;\n");
    emit(e, "}\n\n");
    emit(e, "static inline uint64_t _zer_pool_alloc(void *pool_ptr, size_t slot_size, "
            "uint32_t *gen, uint8_t *used, size_t capacity, uint8_t *ok) {\n");
    emit(e, "    for (uint32_t i = 0; i < capacity; i++) {\n");
    emit(e, "        if (!used[i]) {\n");
    emit(e, "            used[i] = 1;\n");
    /* BUG-858: a RECYCLED slot came back holding the previous object BIT FOR BIT,
     * against the documented "everything auto-zeroed" guarantee that Arena.alloc
     * honours. A `?*T` field then returns NON-NULL and dangling, so a program that
     * sets only the fields it cares about can unwrap and dereference it — a
     * use-after-free reachable from pure safe ZER, and INVISIBLE to ASan because
     * the reuse is inside ZER-owned storage.
     *
     * The asymmetry is why it survived: the FRESH-page path already callocs, so
     * only the REUSE path was affected. */
    emit(e, "            memset((char*)pool_ptr + (size_t)i * slot_size, 0, slot_size);\n");
    emit(e, "            if (gen[i] == 0) gen[i] = _zer_gen_seed(gen); /* BUG-1270; never 0 */\n");
    emit(e, "            *ok = 1;\n");
    emit(e, "            return ((uint64_t)gen[i] << 32) | i;\n");
    emit(e, "        }\n");
    emit(e, "    }\n");
    emit(e, "    *ok = 0;\n");
    emit(e, "    return 0;\n");
    emit(e, "}\n\n");

    /* ZER trap — called on safety violations (use-after-free, bounds, etc.).
     *
     * Baremetal audit (2026-05-05): two paths needed runtime portability —
     *   (a) x86 trap printed via fprintf + int3 — fprintf is hosted-only
     *       so a freestanding x86 build (kernel, EFI app) failed to link.
     *   (b) The "unknown architecture" fallback used fprintf + abort()
     *       unconditionally. On any non-{ARM,RISC-V,AVR,x86} freestanding
     *       target (PowerPC, MIPS, SPARC, custom ISA) link fails.
     * Resolution: per-arch instruction is emitted unconditionally;
     * fprintf+abort is only emitted on hosted targets via __STDC_HOSTED__.
     * Generic freestanding fallback uses __builtin_trap() so user code
     * still halts on a violation. */
    emit(e, "static void _zer_trap(const char *msg, const char *file, int line) {\n");
    emit(e, "#if _ZER_HOSTED\n");
    emit(e, "    fprintf(stderr, \"ZER TRAP: %%s at %%s:%%d\\n\", msg, file, line);\n");
    emit(e, "#else\n");
    emit(e, "    (void)msg; (void)file; (void)line;\n");
    emit(e, "#endif\n");
    emit(e, "#if defined(__arm__) || defined(__thumb__)\n");
    emit(e, "    __asm__ volatile(\"bkpt #0\"); for(;;) {}\n");
    emit(e, "#elif defined(__riscv)\n");
    emit(e, "    __asm__ volatile(\"ebreak\"); for(;;) {}\n");
    emit(e, "#elif defined(__AVR__)\n");
    emit(e, "    __asm__ volatile(\"break\"); for(;;) {}\n");
    emit(e, "#elif defined(__x86_64__) || defined(__i386__)\n");
    /* anqp95 (copied): the for(;;){} sentinel mirrors the ARM/RISC-V/AVR arms
     * and prevents falling through to undefined execution if SIGTRAP is masked
     * or no IDT #BP handler is installed. */
    emit(e, "    __asm__ volatile(\"int3\"); for(;;) {}\n");
    emit(e, "#elif _ZER_HOSTED\n");
    emit(e, "    abort();\n");
    emit(e, "#else\n");
    emit(e, "    __builtin_trap();\n");
    emit(e, "#endif\n");
    emit(e, "}\n\n");

    /* ZER shared struct auto-locking — BUG-473: use recursive mutex.
     * Recursive mutex handles re-entrant locking when function A calls
     * function B that also auto-locks the same shared struct.
     * Lazy init: first lock call initializes with PTHREAD_MUTEX_RECURSIVE.
     *
     * Baremetal audit (2026-05-05/23): pthread_mutex_t / pthread_cond_t are
     * only available when the pthread.h include above (gated on
     * __STDC_HOSTED__) brings them in. Gate the helper definitions
     * matching the include + thread barrier blocks. Bare-metal programs
     * that actually use `shared struct` still get a loud GCC error at the
     * call site (undefined function) — there is no portable lock primitive
     * ZER can fabricate without a runtime. */
    emit(e, "/* ZER shared struct auto-locking (recursive mutex) */\n");
    emit(e, "#if _ZER_HOSTED && !defined(__wasi__)\n");
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
    emit(e, "}\n");
    emit(e, "#endif /* _ZER_HOSTED */\n\n");

    /* ZER thread barrier — portable (mutex + condvar, like Rust) */
    emit(e, "/* ZER thread barrier */\n");
    emit(e, "#if _ZER_HOSTED && !defined(__wasi__)\n");
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
    /* BUG-849 runtime belt: a zero-initialised barrier has target 0, so the
     * `count >= target` test below is true on the FIRST arrival — every thread
     * sails straight through and the barrier reports success while synchronizing
     * nothing. The checker rejects the case it can attribute to a named symbol;
     * a barrier reached through a pointer parameter is beyond a per-file
     * analysis, so trap loudly instead of silently degrading. */
    emit(e, "    if (b->target == 0) _zer_trap(\"@barrier_wait on an uninitialized "
            "barrier — call @barrier_init(b, N) first\", __FILE__, __LINE__);\n");
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
    /* BUG-1022: name the REASON in the non-hosted branch.
     *
     * Barrier / Semaphore / shared struct / spawn / condvar are all pthread-based,
     * so they genuinely cannot work freestanding — that part is a floor, not a
     * bug. What WAS a bug is what the author saw: the types simply vanished and
     * GCC said `error: unknown type name '_zer_barrier'` pointing at a line of
     * GENERATED C, with nothing naming the ZER feature or the reason. Same defect
     * BUG-991 records for float literals, where GCC blamed the user's .zer line
     * for an emitter problem; here it blames a file the user never wrote.
     *
     * zerc cannot know at compile time whether the emitted C will be built with
     * -ffreestanding (that is a GCC flag applied later, or ZER_FREESTANDING), so
     * it cannot diagnose this itself. But it can put the REASON inside the error
     * GCC is going to print, which is what these macros do: the name that shows
     * up in "unknown type name" now says what went wrong and why.
     *
     * A macro, not an `#error`, precisely so a bare-metal program that does NOT
     * use threads is unaffected — which is the common firmware case and must keep
     * building. Nothing fires unless the program actually names one of them. */
    emit(e, "#else\n");
    emit(e, "/* Freestanding: pthreads do not exist, so Barrier/Semaphore cannot. These\n");
    emit(e, "   macros put the reason into GCC's \"unknown type name\" message rather than\n");
    emit(e, "   leaving the author staring at a missing typedef in generated C. */\n");
    emit(e, "#  define _zer_barrier   ZER_Barrier_requires_a_HOSTED_target__pthreads_are_unavailable_on_bare_metal\n");
    emit(e, "#  define _zer_semaphore ZER_Semaphore_requires_a_HOSTED_target__pthreads_are_unavailable_on_bare_metal\n");
    emit(e, "#endif\n\n");

    /* Universal fault handler + @probe — uses C standard signal() everywhere.
     * No platform-specific #ifdef. Works on any OS and bare-metal with libc.
     * Dual-mode: during @probe → recover and return null.
     *            during normal code → trap with error message (catches bad MMIO). */
    /* Universal fault handler + @probe.
     *
     * Fix #4 (2026-05-02): three modes selected by --probe-mode CLI flag:
     *   probe_mode == 0 (HOSTED, default):
     *     - Hosted C (any OS, bare-metal w/ newlib): signal() + setjmp() recovery
     *     - Freestanding C (no libc): direct read (current behavior preserved)
     *     - Compile-time #if __STDC_HOSTED__ chooses between them
     *   probe_mode == 1 (RAW):
     *     - Always direct read, no signal handler. User accepts that
     *       a faulting address gives a `has_value=1` with garbage.
     *     - For embedded users who can't install signal handlers and
     *       know the addresses they probe are safe.
     *   probe_mode == 2 (DISABLED): never reached — checker rejects
     *     @probe usage at compile time.
     *
     * On bare-metal where SIGSEGV doesn't exist as a recoverable signal
     * (most freestanding builds), the hosted-default's __STDC_HOSTED__
     * guard already short-circuits to the same direct-read path the
     * RAW mode emits. RAW just makes that the unconditional choice. */
    if (e->probe_mode == 1) {
        /* RAW mode — direct read only, no signal handler. */
        emit(e, "static _zer_opt_u32 _zer_probe(uintptr_t addr) {\n");
        emit(e, "    /* --probe-mode=raw: direct read, no fault recovery. */\n");
        emit(e, "    /* Caller is responsible for ensuring the address is safe. */\n");
        emit(e, "    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;\n");
        emit(e, "    uint32_t val = *p;\n");
        emit(e, "    return (_zer_opt_u32){ val, 1 };\n");
        emit(e, "}\n\n");
    } else {
        /* HOSTED mode (default) — current behavior with __STDC_HOSTED__ dispatch. */
        emit(e, "#if _ZER_HOSTED == 1 && !defined(__wasi__)\n");
        emit(e, "#include <setjmp.h>\n");
        emit(e, "#include <signal.h>\n\n");
        emit(e, "/* Universal memory fault handler — catches bad MMIO at runtime */\n");
        /* Axis D2 (2026-06-21): the probe re-entry flag and jmp_buf MUST be
         * per-thread. As process-global statics, two threads concurrently in
         * @probe regions raced the flag and the SIGSEGV-delivered thread could
         * longjmp into another thread's stale jmp_buf (cross-thread stack
         * corruption). `__thread` gives each thread its own copy; the signal is
         * delivered to the faulting thread, whose handler reads ITS flag and
         * longjmps to ITS setjmp site. Harmless (single copy) in single-threaded
         * programs. */
        emit(e, "static __thread volatile int _zer_in_probe = 0;\n");
        emit(e, "static __thread jmp_buf _zer_probe_jmp;\n\n");
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
        emit(e, "#else /* freestanding — no signal/setjmp, @probe direct read */\n");
        emit(e, "static _zer_opt_u32 _zer_probe(uintptr_t addr) {\n");
        emit(e, "    /* freestanding: no fault handler, direct read (same as raw mode) */\n");
        emit(e, "    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;\n");
        emit(e, "    uint32_t val = *p;\n");
        emit(e, "    return (_zer_opt_u32){ val, 1 };\n");
        emit(e, "}\n");
        emit(e, "#endif /* _ZER_HOSTED */\n\n");
    }

    /* safe shift — ZER spec: shift by >= width OR < 0 returns 0 (not UB like C).
     * Uses GCC statement expression to evaluate b exactly once.
     *
     * Gap 26 fix (2026-04-27): added (_b < 0) check. Previously, signed
     * shift count with negative value (e.g., i32 n = -1; x << n) bypassed
     * the >= width guard and fell through to (a) << -1 = C undefined
     * behavior. Cast to int64_t for the comparison so unsigned operands
     * compare correctly without wrap-to-negative.
     *
     * BUG-1065: `w` is the ZER width of the left operand (shift_guard_width). The
     * carrier test `sizeof(a) * 8` stays as well: the ZER width alone would let a
     * count reach an operand C has narrower than the ZER type claims, and a C
     * shift by >= its operand's width is undefined. */
    emit(e, "#define _zer_shl(a, b, w) ({ __typeof__(b) _b = (b); "
            "((int64_t)_b < 0 || (int64_t)_b >= (int64_t)(w) || "
            "(int64_t)_b >= (int64_t)(sizeof(a) * 8)) "
            "? (__typeof__(a))0 : (a) << _b; })\n");
    emit(e, "#define _zer_shr(a, b, w) ({ __typeof__(b) _b = (b); "
            "((int64_t)_b < 0 || (int64_t)_b >= (int64_t)(w) || "
            "(int64_t)_b >= (int64_t)(sizeof(a) * 8)) "
            "? (__typeof__(a))0 : (a) >> _b; })\n\n");

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
    emit(e, "        _zer_trap(\"use-after-free or wrong-pool handle: generation mismatch\", __FILE__, __LINE__);\n");
    emit(e, "    }\n");
    emit(e, "    return (char*)slots + idx * slot_size;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void _zer_pool_free(uint32_t *gen, uint8_t *used, "
            "uint64_t handle, size_t capacity) {\n");
    emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
    emit(e, "    uint32_t h_gen = (uint32_t)(handle >> 32);\n");
    /* SAFETY (silent-gap audit 2026-05-21): null-handle free is a no-op.
     * Auto-zero Handle (h_gen == 0) means "never allocated"; without this
     * guard, freeing it silently bumped gen[0] and could invalidate a
     * legitimate handle in slot 0.
     *
     * UNBLOCKED 2026-06-10 (BUG-743): the goto-fires-defer-twice emission
     * bug that prevented adding a stronger `gen[idx] != h_gen` trap here
     * is fixed. A follow-up change can now add the trap to catch
     * wrong-pool / stale-handle frees at runtime (matching the discipline
     * of `_zer_pool_get`). Not enabled in this session — needs a
     * compatibility audit against patterns that currently rely on
     * lenient free (e.g., double-free in disposers, free after
     * pool-clear). Documented in docs/limitations.md cross-function
     * wrong-pool gap as the natural follow-up. */
    emit(e, "    if (h_gen == 0) return;  /* null handle: no-op */\n");
    /* BUG-1270: a handle this pool never issued (another pool's, or a stale one)
     * used to free whatever live object sat in that slot. */
    emit(e, "    if (idx >= capacity || !used[idx] || gen[idx] != h_gen)\n");
    emit(e, "        _zer_trap(\"free of a handle this pool did not issue (wrong pool or already freed)\", __FILE__, __LINE__);\n");
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

    /* BUG-845: the capacity test itself must not overflow. `off + size >
     * capacity` wraps when `size` is near SIZE_MAX, and a wrapped sum compares
     * SMALL — so a request the arena obviously cannot satisfy was granted, and
     * the caller got a pointer into a 1 KiB buffer with a length in the
     * exabytes. Written as a subtraction on the capacity side, which cannot
     * wrap because `off <= capacity` is established first. The alignment
     * rounding is guarded the same way. */
    emit(e, "static inline void *_zer_arena_alloc(_zer_arena *a, size_t size, size_t align) {\n");
    /* BUG-839: BOTH additions here could wrap, and a wrap makes the capacity test
     * PASS for a request that does not fit — the same defeat-the-guard shape as the
     * un-guarded multiply at the call site. Check each before using it. */
    emit(e, "    if (align == 0 || a->offset > (size_t)-1 - (align - 1)) return (void*)0;\n");
    emit(e, "    size_t off = (a->offset + align - 1) & ~(align - 1);\n");
    emit(e, "    if (off > (size_t)-1 - size) return (void*)0;\n");
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
    /* BUG-858 (slab half): a RECYCLED slot came back holding the previous object BIT FOR BIT,
     * against the documented "everything auto-zeroed" guarantee that Arena.alloc
     * honours. A `?*T` field then returns NON-NULL and dangling, so a program that
     * sets only the fields it cares about can unwrap and dereference it — a
     * use-after-free reachable from pure safe ZER, and INVISIBLE to ASan because
     * the reuse is inside ZER-owned storage.
     *
     * The asymmetry is why it survived: the FRESH-page path already callocs, so
     * only the REUSE path was affected. */
    emit(e, "            memset(s->pages[i / _ZER_SLAB_PAGE_SLOTS] + (i %% _ZER_SLAB_PAGE_SLOTS) * s->slot_size, 0, s->slot_size);\n");
    emit(e, "            if (s->gen[i] == 0) s->gen[i] = _zer_gen_seed(s); /* BUG-1270; never 0 */\n");
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
    emit(e, "    if (s->gen[base] == 0) s->gen[base] = _zer_gen_seed(s); /* BUG-1270; never 0 */\n");
    emit(e, "    *ok = 1;\n");
    emit(e, "    return ((uint64_t)s->gen[base] << 32) | base;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void *_zer_slab_get(_zer_slab *s, uint64_t handle) {\n");
    emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
    emit(e, "    uint32_t gen = (uint32_t)(handle >> 32);\n");
    emit(e, "    if (idx >= s->total_slots || !s->used[idx] || s->gen[idx] != gen) {\n");
    emit(e, "        _zer_trap(\"slab: use-after-free, wrong-slab or invalid handle\", __FILE__, __LINE__);\n");
    emit(e, "    }\n");
    emit(e, "    size_t page = idx / _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "    size_t slot = idx %% _ZER_SLAB_PAGE_SLOTS;\n");
    emit(e, "    return s->pages[page] + slot * s->slot_size;\n");
    emit(e, "}\n\n");

    emit(e, "static inline void _zer_slab_free(_zer_slab *s, uint64_t handle) {\n");
    emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
    emit(e, "    uint32_t h_gen = (uint32_t)(handle >> 32);\n");
    /* SAFETY (silent-gap audit 2026-05-21): mirror _zer_pool_free's
     * null-handle no-op. See _zer_pool_free for rationale. */
    emit(e, "    if (h_gen == 0) return;  /* null handle: no-op */\n");
    emit(e, "    if (idx >= s->total_slots || !s->used[idx] || s->gen[idx] != h_gen)   /* BUG-1270 */\n");
    emit(e, "        _zer_trap(\"free of a handle this slab did not issue (wrong slab or already freed)\", __FILE__, __LINE__);\n");
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
        emit(e, "    /* Overflow guard: n*size mustn't wrap. Matches glibc behavior. */\n");
        emit(e, "    if (n != 0 && size > ((size_t)-1) / n) return (void*)0;\n");
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
        /* BUG-838: this validator could NEVER FIRE, and trying cost a boot hang.
         * It is gated to non-Linux/macOS/Windows — exactly the targets where
         * _zer_probe is the DIRECT-READ form that hardcodes has_value = 1. So the
         * check is a compile-time if(0): the trap is unreachable, and a user who
         * typos a base address — the stated purpose, "catches wrong datasheet
         * addresses at first power-on" — gets nothing.
         *
         * Worse than nothing. The read itself still happens, from a CONSTRUCTOR,
         * i.e. BEFORE main and therefore before any RCC clock-enable. On an STM32
         * that is a BusFault on a clock-gated peripheral: a boot hang with no
         * message, produced by a check that could not report anything.
         *
         * Emitted now only where the probe can actually FAIL, expressed at BOTH
         * layers: the emitter skips it under --probe-mode=raw/disabled (leaving a
         * comment saying why), and the C guard gained the _ZER_HOSTED term because
         * the fault-DETECTING probe is the setjmp/signal form and that needs a
         * hosted libc. Written as a condition on the probe FORM rather than by
         * deleting the feature, so a future fault-detecting freestanding probe
         * brings the validator back on its own. */
        if (real_ranges > 0 && e->probe_mode != 0) {
            emit(e, "/* MMIO startup validation SKIPPED: --probe-mode=%s cannot detect a\n"
                    "   faulting address (the probe hardcodes success), so the check could\n"
                    "   only ever be a no-op that still performs a pre-main bus access. */\n",
                    e->probe_mode == 1 ? "raw" : "disabled");
        }
        if (real_ranges > 0 && e->probe_mode == 0) {
            emit(e, "/* MMIO startup validation — verify declared ranges have real hardware */\n");
            emit(e, "#if _ZER_HOSTED && !defined(__wasi__) && "
                    "!defined(__linux__) && !defined(__APPLE__) && !defined(_WIN32)\n");
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

    /* BUG-1027: register every exotic slice type, then flush the ones whose
     * dependencies are ready before each declaration that may name one. */
    collect_exotic_slices(e);

    /* Pass 1: emit struct/enum/union/typedef declarations first */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *d = file_node->file.decls[i];
        if (d->kind == NODE_STRUCT_DECL || d->kind == NODE_ENUM_DECL ||
            d->kind == NODE_UNION_DECL || d->kind == NODE_TYPEDEF) {
            flush_exotic_slices(e);
            emit_top_level_decl(e, d, file_node, i);
        }
    }
    flush_exotic_slices(e);

    /* Emit stamped container struct declarations — after regular structs */
    emit_container_structs(e);
    flush_exotic_slices(e);

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

    /* BUG-1128: prototypes for every function with a body — after every type
     * they can name, before any function or global initializer that names them. */
    emit(e, "\n/* ZER function prototypes */\n");
    emit_async_forward_typedefs(e, file_node);
    for (int i = 0; i < file_node->file.decl_count; i++)
        emit_func_prototype(e, file_node->file.decls[i]);
    emit(e, "\n");
    emit_early_async_structs(e, file_node);   /* BUG-1238 */

    /* Emit spawn wrapper functions — after structs/slabs, before user functions */
    emit_spawn_wrappers(e);

    /* Pass 2a: every GLOBAL variable, before any function body. BUG-1214: ZER
     * lets a function use a global declared further down the file (the checker
     * registers every top-level name first), and the emitter wrote globals in
     * source order — `u32 use() { return later; } u32 later = 5;` reached GCC as
     * "'later' undeclared". A global's initializer can name only functions
     * (prototyped above) and other globals (their relative order is kept). */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *d = file_node->file.decls[i];
        if (d->kind == NODE_GLOBAL_VAR)
            emit_top_level_decl(e, d, file_node, i);
    }
    /* Pass 2b: everything else (functions, interrupts, ...) */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *d = file_node->file.decls[i];
        if (d->kind != NODE_STRUCT_DECL && d->kind != NODE_ENUM_DECL &&
            d->kind != NODE_UNION_DECL && d->kind != NODE_TYPEDEF &&
            d->kind != NODE_CONTAINER_DECL && d->kind != NODE_GLOBAL_VAR)
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
/* BUG-1040 (2026-09-21): the C name of a GLOBAL declared in the module being
 * emitted — `mod__name` inside a module, the bare name in main. The general
 * global-var path has mangled this way since BUG-218/222; the Pool / Ring /
 * Arena / Slab declaration arms bypassed it, so a module's `Arena scratch` was
 * emitted as bare `scratch` while every reference from main was
 * `arena_lib__scratch` (GCC: undeclared), and two modules each declaring
 * `Pool(T, 4) items` collided at link (redefinition). */
static void emit_module_global_name(Emitter *e, const char *name, uint32_t len) {
    if (e->current_module)
        emit(e, "%.*s__%.*s", (int)e->current_module_len, e->current_module, (int)len, name);
    else
        emit(e, "%.*s", (int)len, name);
}

/* BUG-1040: the C name of an ALLOCATOR found by type (find_unique_allocator) —
 * the Handle auto-deref `h.field` spells `pool.get(h)` with it. The lookup can
 * return either of a module global's two Symbols (raw key or mangled key, BUG-233),
 * so the prefix is added only when the name does not already carry it. */
static void emit_alloc_sym_cname(Emitter *e, Symbol *sym) {
    if (sym->module_prefix) {
        uint32_t pl = sym->module_prefix_len;
        bool already = sym->name_len > pl + 2 &&
            memcmp(sym->name, sym->module_prefix, pl) == 0 &&
            sym->name[pl] == '_' && sym->name[pl + 1] == '_';
        if (!already) emit(e, "%.*s__", (int)pl, sym->module_prefix);
    }
    emit(e, "%.*s", (int)sym->name_len, sym->name);
}

static void emit_local_name(Emitter *e, IRFunc *func, int local_id) {
    if (local_id < 0 || local_id >= func->local_count) return;
    IRLocal *l = &func->locals[local_id];
    if (func->is_async)
        emit(e, "self->%.*s", (int)l->name_len, l->name);
    else
        emit(e, "%.*s", (int)l->name_len, l->name);
}

/* BUG-1152: the statement form of the non-null load guard — after an IR load
 * (field / element / deref) whose destination is a non-optional `*T`. */
static void emit_nonnull_local_check(Emitter *e, IRFunc *func, int local_id) {
    if (local_id < 0 || local_id >= func->local_count) return;
    Type *t = func->locals[local_id].type;
    int gk = load_guard_kind(t);
    if (!gk) return;
    /* Same C line as the load, so the trap's __LINE__ is the load's #line. */
    emit(e, " if (!");
    emit_local_name(e, func, local_id);
    emit(e, "%s) _zer_trap(", nn_null_member(t));
    emit(e, "\"%s\", __FILE__, __LINE__);", gk == 2 ? ZER_ENUM0_TRAP_MSG : ZER_NN_TRAP_MSG);
}

/* ================================================================
 * Builtin Call Emitter — emit pool/slab/ring/arena/Task inline C
 * Extracted from emit_expr NODE_CALL. Uses emit_rewritten_node for args.
 * Returns true if handled, false if not a recognized builtin.
 * ================================================================ */
static void emit_rewritten_node(Emitter *e, Node *node, IRFunc *func); /* forward */
static bool emit_builtin_inline(Emitter *e, Node *node, IRFunc *func) {
    if (!node || node->kind != NODE_CALL || !node->call.callee ||
        node->call.callee->kind != NODE_FIELD || !node->call.callee->field.object ||
        node->call.callee->field.object->kind != NODE_IDENT) return false;
    const char *mn = node->call.callee->field.field_name;
    uint32_t ml = (uint32_t)node->call.callee->field.field_name_len;
    const char *on = node->call.callee->field.object->ident.name;
    uint32_t ol = (uint32_t)node->call.callee->field.object->ident.name_len;
    Type *ot = checker_get_type(e->checker, node->call.callee->field.object);
    if (!ot) { Symbol *s = scope_lookup(e->checker->global_scope, on, ol); if (s) ot = s->type; }
    if (!ot) return false;
    /* BUG-1040: every `%.*s` below spells the RECEIVER. A local (`Arena a` in
     * this function) keeps its name; a global is spelled exactly as the ident
     * emitter spells every other global — with the module prefix that the
     * declaration (emit_module_global_name) now carries. Before this, a
     * module's `scratch.alloc(Node)` emitted `&scratch` against a declaration
     * main referenced as `arena_lib__scratch`. */
    {
        bool is_local = false;
        if (func) {
            for (int li = 0; li < func->local_count; li++) {
                IRLocal *l = &func->locals[li];
                if ((l->name_len == ol && memcmp(l->name, on, ol) == 0) ||
                    (l->orig_name && l->orig_name_len == ol && memcmp(l->orig_name, on, ol) == 0)) {
                    is_local = true; break;
                }
            }
        }
        if (!is_local) {
            Symbol *gs = scope_lookup(e->checker->global_scope, on, ol);
            const char *pfx = NULL; uint32_t pl = 0;
            if (gs && gs->module_prefix) {
                if (e->current_module) { pfx = e->current_module; pl = e->current_module_len; }
                else { pfx = gs->module_prefix; pl = gs->module_prefix_len; }
            } else if (!gs && e->current_module) {
                pfx = e->current_module; pl = e->current_module_len;
            }
            if (pfx) {
                uint32_t ml2 = pl + 2 + ol;
                char *m = (char *)arena_alloc(e->arena, ml2 + 1);
                memcpy(m, pfx, pl); m[pl] = '_'; m[pl + 1] = '_';
                memcpy(m + pl + 2, on, ol); m[ml2] = '\0';
                on = m; ol = ml2;
            }
        }
    }
    Type *te = type_unwrap_distinct(ot);
    #define BA(i) emit_rewritten_node(e, node->call.args[i], func)
    /* Pool */
    if (te->kind == TYPE_POOL) {
        if (ml==5 && !memcmp(mn,"alloc",5) && node->call.arg_count==0) {
            int t=e->temp_count++; emit(e,"({uint8_t _zer_aok%d=0;uint64_t _zer_ah%d=_zer_pool_alloc(%.*s.slots,sizeof(%.*s.slots[0]),%.*s.gen,%.*s.used,%llu,&_zer_aok%d);(_zer_opt_u64){_zer_ah%d,_zer_aok%d};})",t,t,(int)ol,on,(int)ol,on,(int)ol,on,(int)ol,on,(unsigned long long)te->pool.count,t,t,t); return true;
        }
        if (ml==3 && !memcmp(mn,"get",3) && node->call.arg_count>0) {
            emit(e,"(("); emit_type(e,type_pointer(e->arena,te->pool.elem)); emit(e,")_zer_pool_get(%.*s.slots,%.*s.gen,%.*s.used,sizeof(%.*s.slots[0]),",(int)ol,on,(int)ol,on,(int)ol,on,(int)ol,on); BA(0); emit(e,",%llu))",(unsigned long long)te->pool.count); return true;
        }
        if (ml==4 && !memcmp(mn,"free",4) && node->call.arg_count>0) {
            emit(e,"_zer_pool_free(%.*s.gen,%.*s.used,",(int)ol,on,(int)ol,on); BA(0); emit(e,",%llu)",(unsigned long long)te->pool.count); return true;
        }
        if (ml==9 && !memcmp(mn,"alloc_ptr",9) && node->call.arg_count==0) {
            int t=e->temp_count++; emit(e,"({uint8_t _zer_aok%d=0;uint64_t _zer_ah%d=_zer_pool_alloc(%.*s.slots,sizeof(%.*s.slots[0]),%.*s.gen,%.*s.used,%llu,&_zer_aok%d);_zer_aok%d?(",t,t,(int)ol,on,(int)ol,on,(int)ol,on,(int)ol,on,(unsigned long long)te->pool.count,t,t); emit_type(e,type_pointer(e->arena,te->pool.elem)); emit(e,")_zer_pool_get(%.*s.slots,%.*s.gen,%.*s.used,sizeof(%.*s.slots[0]),_zer_ah%d,%llu):(void*)0;})",(int)ol,on,(int)ol,on,(int)ol,on,(int)ol,on,t,(unsigned long long)te->pool.count); return true;
        }
        if (ml==8 && !memcmp(mn,"free_ptr",8) && node->call.arg_count>0) {
            emit(e,"_zer_pool_free(%.*s.gen,%.*s.used,((uint64_t)((char*)(",(int)ol,on,(int)ol,on); BA(0); emit(e,")-(char*)%.*s.slots)/sizeof(%.*s.slots[0])),%llu)",(int)ol,on,(int)ol,on,(unsigned long long)te->pool.count); return true;
        }
    }
    /* Slab */
    if (te->kind == TYPE_SLAB) {
        if (ml==5 && !memcmp(mn,"alloc",5) && node->call.arg_count==0) {
            int t=e->temp_count++; emit(e,"({uint8_t _zer_aok%d=0;uint64_t _zer_ah%d=_zer_slab_alloc(&%.*s,&_zer_aok%d);_zer_aok%d?(_zer_opt_u64){_zer_ah%d,1}:(_zer_opt_u64){0,0};})",t,t,(int)ol,on,t,t,t); return true;
        }
        if (ml==9 && !memcmp(mn,"alloc_ptr",9) && node->call.arg_count==0) {
            int t=e->temp_count++; emit(e,"({uint8_t _zer_aok%d=0;uint64_t _zer_ah%d=_zer_slab_alloc(&%.*s,&_zer_aok%d);_zer_aok%d?(",t,t,(int)ol,on,t,t); emit_type(e,type_pointer(e->arena,te->slab.elem)); emit(e,")_zer_slab_get(&%.*s,_zer_ah%d):(void*)0;})",(int)ol,on,t); return true;
        }
        if (ml==3 && !memcmp(mn,"get",3) && node->call.arg_count>0) {
            emit(e,"(("); emit_type(e,type_pointer(e->arena,te->slab.elem)); emit(e,")_zer_slab_get(&%.*s,",(int)ol,on); BA(0); emit(e,"))"); return true;
        }
        if (ml==4 && !memcmp(mn,"free",4) && node->call.arg_count>0) {
            emit(e,"_zer_slab_free(&%.*s,",(int)ol,on); BA(0); emit(e,")"); return true;
        }
        if (ml==8 && !memcmp(mn,"free_ptr",8) && node->call.arg_count>0) {
            emit(e,"_zer_slab_free_ptr(&%.*s,(void*)",(int)ol,on); BA(0); emit(e,")"); return true;
        }
    }
    /* Ring */
    if (te->kind == TYPE_RING) {
        if (ml==4 && !memcmp(mn,"push",4) && node->call.arg_count>0) {
            /* Element-type coercion: if ring elem is ?T and arg is T or
             * null, wrap into the optional struct. Without this, the
             * `_zer_rp%d = arg` assignment was an invalid initializer
             * (no implicit T→?T conversion in C). Discovered 2026-05-29
             * via the systematic T-vs-?T coercion audit. */
            Type *elem_eff = type_unwrap_distinct(te->ring.elem);
            Type *arg_ast = checker_get_type(e->checker, node->call.args[0]);
            Type *arg_eff = arg_ast ? type_unwrap_distinct(arg_ast) : NULL;
            bool elem_is_opt = elem_eff && elem_eff->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(elem_eff->optional.inner);
            bool arg_is_opt = arg_eff && arg_eff->kind == TYPE_OPTIONAL;
            int t=e->temp_count++; emit(e,"({"); emit_type(e,te->ring.elem); emit(e," _zer_rp%d=",t);
            if (elem_is_opt && !arg_is_opt) {
                emit(e,"(");
                emit_type(e,te->ring.elem);
                emit(e,"){ ");
                if (node->call.args[0]->kind == NODE_NULL_LIT) {
                    emit(e,".has_value = 0 }");
                } else {
                    emit(e,".value = ");
                    BA(0);
                    emit(e,", .has_value = 1 }");
                }
            } else {
                BA(0);
            }
            emit(e,";_zer_ring_push(%.*s.data,&%.*s.head,&%.*s.tail,&%.*s.count,%llu,&_zer_rp%d,sizeof(_zer_rp%d));})",(int)ol,on,(int)ol,on,(int)ol,on,(int)ol,on,(unsigned long long)te->ring.count,t,t); return true;
        }
        if (ml==3 && !memcmp(mn,"pop",3)) {
            /* BUG-348 pairing: acquire fence AFTER data read, BEFORE tail
             * update — pairs with `_zer_ring_push`'s release fence. Without
             * it, weakly-ordered CPUs (ARM, RISC-V) may reorder the data
             * load after the consumer's tail decrement, letting a producer
             * see "ring drained" and overwrite the slot before the consumer
             * has actually finished reading.
             *
             * The AST-path emission (emitter.c ring.pop "({_zer_rp...})"
             * branch) already includes this fence. The IR-path emission
             * below previously omitted it — same regression class as
             * BUG-595/596 (AST→IR safety-wrapper stripping).
             *
             * Verified by inspection of generated C: pre-fix, `chan.pop()`
             * emitted `_zer_ro%d.value=chan.data[chan.tail]; %d.has_value=1;`
             * with NO acquire fence between the load and the tail update. */
            int t=e->temp_count++; Type *opt=type_optional(e->arena,te->ring.elem); emit(e,"({"); emit_type(e,opt); emit(e," _zer_ro%d={0};if(%.*s.count>0){_zer_ro%d.value=%.*s.data[%.*s.tail];__atomic_thread_fence(__ATOMIC_ACQUIRE);_zer_ro%d.has_value=1;%.*s.tail=(%.*s.tail+1)%%%llu;%.*s.count--;}_zer_ro%d;})",t,(int)ol,on,t,(int)ol,on,(int)ol,on,t,(int)ol,on,(int)ol,on,(unsigned long long)te->ring.count,(int)ol,on,t); return true;
        }
        if (ml==12 && !memcmp(mn,"push_checked",12) && node->call.arg_count>0) {
            /* ring.push_checked(val) → ?void (null if full).
             * Same elem-type coercion as push above. */
            Type *elem_eff = type_unwrap_distinct(te->ring.elem);
            Type *arg_ast = checker_get_type(e->checker, node->call.args[0]);
            Type *arg_eff = arg_ast ? type_unwrap_distinct(arg_ast) : NULL;
            bool elem_is_opt = elem_eff && elem_eff->kind == TYPE_OPTIONAL &&
                !is_null_sentinel(elem_eff->optional.inner);
            bool arg_is_opt = arg_eff && arg_eff->kind == TYPE_OPTIONAL;
            int t=e->temp_count++;
            emit(e,"({"); emit_type(e,te->ring.elem); emit(e," _zer_rp%d=",t);
            if (elem_is_opt && !arg_is_opt) {
                emit(e,"(");
                emit_type(e,te->ring.elem);
                emit(e,"){ ");
                if (node->call.args[0]->kind == NODE_NULL_LIT) {
                    emit(e,".has_value = 0 }");
                } else {
                    emit(e,".value = ");
                    BA(0);
                    emit(e,", .has_value = 1 }");
                }
            } else {
                BA(0);
            }
            emit(e,";_zer_opt_void _zer_rc%d;if(%.*s.count<%llu){",t,(int)ol,on,(unsigned long long)te->ring.count);
            emit(e,"_zer_ring_push(%.*s.data,&%.*s.head,&%.*s.tail,&%.*s.count,%llu,&_zer_rp%d,sizeof(_zer_rp%d));",
                 (int)ol,on,(int)ol,on,(int)ol,on,(int)ol,on,(unsigned long long)te->ring.count,t,t);
            emit(e,"_zer_rc%d=(_zer_opt_void){1};}else{_zer_rc%d=(_zer_opt_void){0};}_zer_rc%d;})",t,t,t);
            return true;
        }
    }
    /* Arena */
    if (te->kind == TYPE_ARENA) {
        if (ml==5 && !memcmp(mn,"reset",5)) { emit(e,"(%.*s.offset=0)",(int)ol,on); return true; }
        if (ml==12 && !memcmp(mn,"unsafe_reset",12)) { emit(e,"(%.*s.offset=0)",(int)ol,on); return true; }
        if (ml==4 && !memcmp(mn,"over",4) && node->call.arg_count>0) {
            /* Single-eval consideration:
             *   - Array arg: can't hoist (C can't assign arrays). Arg must be an
             *     lvalue (ident, field) — no side effect. Emit directly twice.
             *   - Slice arg: hoist to temp to avoid double-call (hoist lets
             *     function-call args like next_buf() run exactly once). */
            Type *at = checker_get_type(e->checker, node->call.args[0]);
            Type *ae = at ? type_unwrap_distinct(at) : NULL;
            bool is_slice = ae && ae->kind == TYPE_SLICE;
            if (is_slice) {
                int t = e->temp_count++;
                emit(e, "({ ");
                emit_type(e, ae);
                emit(e, " _zer_ob%d = ", t);
                BA(0);
                emit(e, "; (_zer_arena){(uint8_t*)_zer_ob%d.ptr, _zer_ob%d.len, 0}; })", t, t);
            } else {
                emit(e,"((_zer_arena){(uint8_t*)");
                BA(0);
                emit(e,",sizeof(");
                BA(0);
                emit(e,"),0})");
            }
            return true;
        }
        if (ml==5 && !memcmp(mn,"alloc",5) && node->call.arg_count>0 && node->call.args[0]->kind==NODE_IDENT) {
            Symbol *ts=scope_lookup(e->checker->global_scope,node->call.args[0]->ident.name,(uint32_t)node->call.args[0]->ident.name_len);
            /* BUG-1040: emit_type spells a struct WITH its module prefix (`struct
             * arena_lib__Node`); the hand-rolled `struct Node` here was an
             * incomplete type inside an imported module (same defect BUG-1029
             * fixed for alloc(T, n)). */
            if (ts&&ts->type) { emit(e,"(("); emit_type(e,type_pointer(e->arena,ts->type)); emit(e,")_zer_arena_alloc(&%.*s,sizeof(",(int)ol,on);
                emit_type(e,ts->type);
                emit(e,"),_Alignof("); emit_type(e,ts->type); emit(e,")))"); return true; }
        }
        if (ml==11 && !memcmp(mn,"alloc_slice",11) && node->call.arg_count>1 && node->call.args[0]->kind==NODE_IDENT) {
            /* arena.alloc_slice(T, n) → allocate n*sizeof(T), return ?[]T */
            Symbol *ts=scope_lookup(e->checker->global_scope,node->call.args[0]->ident.name,(uint32_t)node->call.args[0]->ident.name_len);
            if (ts&&ts->type) { int t=e->temp_count++;
                /* BUG-845: the byte count is `sizeof(T) * n` and it MUST be
                 * computed with an overflow check. The AST path has done this
                 * since BUG-266; this IR path — the only one that runs for a
                 * function body since the 2026-04 migration — did not, so the
                 * AST guard was dead code. Measured: a 1024-byte arena returned
                 * a slice reporting 2^61 elements (2^61 * 8 wraps to a 0-byte
                 * request, which trivially "fits"), and because the LENGTH is
                 * what every downstream bounds check consults, a write 1600
                 * bytes past the arena then succeeded with no trap and no
                 * diagnostic. On bare metal there is no fault handler to catch
                 * even the far-out-of-range case. */
                emit(e,"({size_t _zer_an%d=",t); BA(1); emit(e,";");
                /* BUG-839: BUG-266 added __builtin_mul_overflow around sizeof(T)*n on
                 * the AST path ONLY, and function bodies have been IR-only since
                 * 2026-04-19 — so the guard has been UNREACHABLE ever since. Measured:
                 * with a 1024-byte arena, alloc_slice(Big, 2^61) wrapped the byte count
                 * to 0, the zero-size bump SUCCEEDED, and the caller received a slice
                 * reporting 2305843009213693952 elements. Every later bounds check then
                 * PASSES — it compares against the bogus length — so every access is an
                 * unchecked OOB. Silent at compile time and silent at run time; a
                 * 32-bit target reaches the same wrap from a plain u32 count.
                 * This is why CLAUDE.md's AST->IR grep list carries __builtin_*_overflow:
                 * a wrapper whose absence produces a WRONG LENGTH defeats every
                 * downstream guard rather than merely removing one. */
                emit(e,"size_t _zer_asz%d; uint8_t *_zer_ap%d=__builtin_mul_overflow(sizeof(",t,t);
                emit_type(e,ts->type);   /* BUG-1040: module-prefixed spelling */
                emit(e,"),_zer_an%d,&_zer_asz%d)?(uint8_t*)0:(uint8_t*)_zer_arena_alloc(&%.*s,_zer_asz%d,",t,t,(int)ol,on,t);
                /* _Alignof(T) */
                emit(e,"_Alignof("); emit_type(e,ts->type); emit(e,"));");
                /* wrap in ?[]T */
                emit(e,"_zer_ap%d?(",t);
                Type *slice_t=type_slice(e->arena,ts->type); Type *opt_t=type_optional(e->arena,slice_t);
                emit_type(e,opt_t); emit(e,"){("); emit_type(e,slice_t); emit(e,"){(");
                emit_type(e,type_pointer(e->arena,ts->type)); emit(e,")_zer_ap%d,_zer_an%d},1}:(",t,t);
                emit_type(e,opt_t); emit(e,"){0};})");
                return true;
            }
        }
    }
    /* Task.alloc/free (auto-slab) */
    if (te->kind == TYPE_STRUCT) {
        const char *sn=te->struct_type.name; uint32_t sl=te->struct_type.name_len;
        if (ml==5 && !memcmp(mn,"alloc",5)) { int t=e->temp_count++; emit(e,"({uint8_t _zer_aok%d=0;uint64_t _zer_ah%d=_zer_slab_alloc(&_zer_auto_slab_%.*s,&_zer_aok%d);_zer_aok%d?(_zer_opt_u64){_zer_ah%d,1}:(_zer_opt_u64){0,0};})",t,t,(int)sl,sn,t,t,t); return true; }
        if (ml==9 && !memcmp(mn,"alloc_ptr",9)) { int t=e->temp_count++; emit(e,"({uint8_t _zer_aok%d=0;uint64_t _zer_ah%d=_zer_slab_alloc(&_zer_auto_slab_%.*s,&_zer_aok%d);_zer_aok%d?(struct %.*s*)_zer_slab_get(&_zer_auto_slab_%.*s,_zer_ah%d):(void*)0;})",t,t,(int)sl,sn,t,t,(int)sl,sn,(int)sl,sn,t); return true; }
        if (ml==4 && !memcmp(mn,"free",4) && node->call.arg_count>0) { emit(e,"_zer_slab_free(&_zer_auto_slab_%.*s,",(int)sl,sn); BA(0); emit(e,")"); return true; }
        if (ml==8 && !memcmp(mn,"free_ptr",8) && node->call.arg_count>0) { emit(e,"_zer_slab_free_ptr(&_zer_auto_slab_%.*s,(void*)",(int)sl,sn); BA(0); emit(e,")"); return true; }
    }
    #undef BA
    return false;
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
/* Does an lvalue path pass through a field of a PACKED struct? Taking its address
 * gives a possibly misaligned pointer (BUG-833's `&p.w`). */
static bool lvalue_through_packed(Emitter *e, Node *lv) {
    for (Node *r = lv; r; ) {
        if (r->kind == NODE_FIELD) {
            Type *ot = checker_get_type(e->checker, r->field.object);
            Type *oe = ot ? type_unwrap_distinct(ot) : NULL;
            if (oe && type_dispatch_kind(oe) == TYPE_POINTER)
                oe = type_unwrap_distinct(oe->pointer.inner);
            if (oe && type_dispatch_kind(oe) == TYPE_STRUCT && oe->struct_type.is_packed)
                return true;
            r = r->field.object;
        } else if (r->kind == NODE_INDEX) {
            r = r->index_expr.object;
        } else break;
    }
    return false;
}

/* BUG-1198: ONE bit-slice SET, for both emitter paths (`sub` is emit_expr's or
 * emit_rewritten_node's node emitter), every assignment operator, and every
 * position. It replaces two copies that between them:
 *   - read a VOLATILE register twice for a compound op (2 loads + 1 store — a
 *     clear-on-read status register saw its side effect twice);
 *   - took `&(p.w)` of a PACKED field, a misaligned `uint32_t*` (a hard fault on
 *     Cortex-M0 / RISC-V, measured with UBSan);
 *   - emitted a raw C shift for `<<=` / `>>=` (UBSan "shift exponent 70"; 3 at
 *     -O0, 0 at -O2) and a raw division for `/=` / `%=`;
 *   - with runtime positions: `hi < lo` cleared every bit from lo up, `hi` past the
 *     type's width overwrote the whole register, and on a `uN` wrote bits above N
 *     (a u12 left at 0x3F00). The documented rule (reference.md "Bit Extraction")
 *     is "a runtime position at or past the width is a defined no-op": the write
 *     changes nothing when hi >= W (then lo >= W too, or the field is cut) or
 *     hi < lo. W is the VALUE width (N for a uN / iN).
 * The value is read once into `_zer_bv`, the new value computed in 64 bits, and a
 * non-native signed `iN` sign-extended from bit N-1 so its carrier invariant holds.
 * Types wider than 64 bits are refused by the checker (the math is 64-bit). */
static void emit_bitslice_set(Emitter *e, Node *node, IRFunc *func, EmitNodeFn sub) {
    Node *obj = node->assign.target->slice.object;
    Node *hi_node = node->assign.target->slice.start;
    Node *lo_node = node->assign.target->slice.end;
    Type *ot = checker_get_type(e->checker, obj);
    Type *oe = ot ? type_unwrap_distinct(ot) : NULL;
    int W = oe ? type_width(oe) : 64;
    if (W <= 0 || W > 64) W = 64;
    TypeKind ok = oe ? type_dispatch_kind(oe) : TYPE_U64;
    bool narrow_signed = (ok == TYPE_SINT) && W < 64;
    bool packed = lvalue_through_packed(e, obj);
    int t = e->temp_count++;
    emit(e, "({ ");
    if (!packed) {
        emit(e, "__typeof__(");
        sub(e, obj, func);
        emit(e, ") *_zer_bp%d = &(", t);
        sub(e, obj, func);
        emit(e, "); ");
    }
    #define BS_LV() do { if (packed) { emit(e, "("); sub(e, obj, func); emit(e, ")"); } \
                         else emit(e, "(*_zer_bp%d)", t); } while (0)
    emit(e, "uint64_t _zer_bh%d = ", t);
    if (hi_node) { emit(e, "(uint64_t)("); sub(e, hi_node, func); emit(e, ")"); }
    else emit(e, "%d", W - 1);
    emit(e, "; uint64_t _zer_bl%d = ", t);
    if (lo_node) { emit(e, "(uint64_t)("); sub(e, lo_node, func); emit(e, ")"); }
    else emit(e, "0");
    emit(e, "; uint64_t _zer_bv%d = (uint64_t)", t);
    BS_LV();
    emit(e, "; uint64_t _zer_bt%d = (_zer_bh%d >= %d) ? %d : _zer_bh%d; ",
         t, t, W, W - 1, t);
    emit(e, "uint64_t _zer_bm%d = (_zer_bh%d >= %d || _zer_bh%d < _zer_bl%d) ? 0 : "
            "((((_zer_bt%d - _zer_bl%d + 1) >= 64) ? ~(uint64_t)0 : "
            "((1ull << (_zer_bt%d - _zer_bl%d + 1)) - 1)) << _zer_bl%d); ",
         t, t, W, t, t, t, t, t, t, t);
    emit(e, "uint64_t _zer_bn%d = ", t);
    TokenType op = node->assign.op;
    if (op == TOK_EQ) {
        emit(e, "(uint64_t)(");
        sub(e, node->assign.value, func);
        emit(e, ")");
    } else {
        const char *cop = " + ";
        bool is_div = false, is_shift = false;
        switch (op) {
        case TOK_PLUSEQ:    cop = " + ";  break;
        case TOK_MINUSEQ:   cop = " - ";  break;
        case TOK_STAREQ:    cop = " * ";  break;
        case TOK_SLASHEQ:   cop = " / ";  is_div = true; break;
        case TOK_PERCENTEQ: cop = " % ";  is_div = true; break;   /* an ARGUMENT: literal % */
        case TOK_AMPEQ:     cop = " & ";  break;
        case TOK_PIPEEQ:    cop = " | ";  break;
        case TOK_CARETEQ:   cop = " ^ ";  break;
        case TOK_LSHIFTEQ:  cop = " << "; is_shift = true; break;
        case TOK_RSHIFTEQ:  cop = " >> "; is_shift = true; break;
        default:            cop = " + ";  break;
        }
        emit(e, "({ uint64_t _zer_bc%d = (_zer_bm%d == 0) ? 0 : "
                "((_zer_bv%d >> _zer_bl%d) & (_zer_bm%d >> _zer_bl%d)); "
                "uint64_t _zer_br%d = (uint64_t)(", t, t, t, t, t, t, t);
        sub(e, node->assign.value, func);
        emit(e, "); ");
        if (is_div)
            emit(e, "if (_zer_br%d == 0) _zer_trap(\"division by zero\", __FILE__, __LINE__); ", t);
        if (is_shift)
            emit(e, "(_zer_br%d >= 64) ? (uint64_t)0 : (_zer_bc%d%s_zer_br%d); })", t, t, cop, t);
        else
            emit(e, "_zer_bc%d%s_zer_br%d; })", t, cop, t);
    }
    emit(e, "; uint64_t _zer_bf%d = (_zer_bv%d & ~_zer_bm%d) | ((_zer_bm%d == 0) ? 0 : "
            "((_zer_bn%d << _zer_bl%d) & _zer_bm%d)); ", t, t, t, t, t, t, t);
    if (narrow_signed)
        emit(e, "_zer_bf%d = (uint64_t)(((int64_t)(_zer_bf%d << %d)) >> %d); ",
             t, t, 64 - W, 64 - W);
    BS_LV();
    emit(e, " = (__typeof__(");
    BS_LV();
    emit(e, "))_zer_bf%d; })", t);
    #undef BS_LV
}

/* BUG-1058: ONE emission of `@inttoptr(*T, addr)` for both dispatch paths
 * (`func == NULL` = AST path, emit_expr; else IR path, emit_rewritten_node).
 * The two copies were identical and shared three defects:
 *   - the address was cast to `uintptr_t` BEFORE the range check, so on a
 *     32-bit target `0x1_4000_0010` was checked as `0x4000_0010` and read an
 *     in-range register it never named (measured with -m32);
 *   - the span test `ma + (sizeof(T) - 1) <= end` wraps for an address near the
 *     top of the space, so `0xFFFF...FE` passed a range that ends far below it;
 *   - with no range and no alignment need, nothing checked the address at all.
 * Now the address is held in `uint64_t` (the checker refuses a wider operand),
 * trapped if it does not fit in a pointer, range-checked without overflow, and
 * only then narrowed. A constant address is validated by the checker. */
static void emit_inttoptr(Emitter *e, Node *node, IRFunc *func) {
    if (!node->intrinsic.type_arg) return;
    /* BUG-1195: the CHECKER's result type — volatile for a constant address. */
    Type *t = checker_get_type(e->checker, node);
    if (!t || type_dispatch_kind(t) != TYPE_POINTER) t = resolve_tynode(e, node->intrinsic.type_arg);
    Node *arg = node->intrinsic.arg_count > 0 ? node->intrinsic.args[0] : NULL;
    bool var_addr = arg && arg->kind != NODE_INT_LIT && !node->intrinsic.addr_is_const;
    Type *inner = t ? type_unwrap_distinct(t) : NULL;
    if (inner && type_dispatch_kind(inner) == TYPE_POINTER)
        inner = inner->pointer.inner;
    /* F4: alignment via type_alignment_bytes (aggregate-aware). GAP-2 (BUG-736):
     * alignment is a property of the TARGET TYPE and is checked even with zero
     * declared mmio ranges (--no-strict-mmio); only the range check needs them. */
    int align = inner ? type_alignment_bytes(inner) : 0;
    if (!var_addr && node->intrinsic.addr_is_const) {
        emit(e, "((");
        emit_type(e, t);
        emit(e, ")(uintptr_t)0x%llxULL)", (unsigned long long)node->intrinsic.const_addr);
        return;
    }
    if (!var_addr) {
        emit(e, "((");
        emit_type(e, t);
        emit(e, ")(uintptr_t)(");
        if (arg) {
            if (func) emit_rewritten_node(e, arg, func); else emit_expr(e, arg);
        }
        emit(e, "))");
        return;
    }
    int tmp = e->temp_count++;
    emit(e, "({ uint64_t _zer_ma%d = (uint64_t)(", tmp);
    if (func) emit_rewritten_node(e, arg, func); else emit_expr(e, arg);
    emit(e, "); ");
    emit(e, "if (_zer_ma%d > (uint64_t)UINTPTR_MAX) _zer_trap(\"@inttoptr: address does "
            "not fit in a pointer\", __FILE__, __LINE__); ", tmp);
    if (e->checker->mmio_range_count > 0) {
        /* plt86m 2026-06-17: the ACCESS SPAN (sizeof T, GCC-evaluated so an
         * aggregate is right) must fit, not just the start address. Written as
         * `end - ma >= span - 1` after `ma <= end`, which cannot overflow. */
        emit(e, "if (!(");
        for (int ri = 0; ri < e->checker->mmio_range_count; ri++) {
            unsigned long long lo = (unsigned long long)e->checker->mmio_ranges[ri][0];
            unsigned long long hi = (unsigned long long)e->checker->mmio_ranges[ri][1];
            if (ri > 0) emit(e, " || ");
            emit(e, "(_zer_ma%d >= 0x%llxULL && _zer_ma%d <= 0x%llxULL && "
                    "0x%llxULL - _zer_ma%d >= (uint64_t)sizeof(",
                 tmp, lo, tmp, hi, hi, tmp);
            if (inner) emit_type(e, inner); else emit(e, "char");
            emit(e, ") - 1ULL)");
        }
        emit(e, ")) _zer_trap(\"@inttoptr: address outside mmio range\", __FILE__, __LINE__); ");
    }
    /* BUG-489: runtime alignment for a variable address. */
    if (align > 1)
        emit(e, "if (_zer_ma%d %% %d != 0) _zer_trap(\"@inttoptr: unaligned address\", "
                "__FILE__, __LINE__); ", tmp, align);
    emit(e, "(");
    emit_type(e, t);
    emit(e, ")(uintptr_t)_zer_ma%d; })", tmp);
}

static void emit_rewritten_node_impl(Emitter *e, Node *node, IRFunc *func);

/* BUG-1152: the IR-rewritten path passes through the same load guard. */
static void emit_rewritten_node(Emitter *e, Node *node, IRFunc *func) {
    if (!node) return;
    Node *saved_lv = e->nn_lvalue;
    Node *lv = nn_lvalue_of(node);
    if (nn_guard_wanted(e, node)) {
        emit_nn_guarded(e, node, func, emit_rewritten_node_impl);
        return;
    }
    if (lv) e->nn_lvalue = lv;
    bool asg_paren = node->kind == NODE_ASSIGN;   /* BUG-1183 — see emit_expr */
    if (asg_paren) emit(e, "(");
    bool ur_ptr = false; uint32_t ur_idx = 0;
    Node *ur = union_partial_write_target(e, node, &ur_ptr, &ur_idx);
    if (ur) emit_union_reset_prefix(e, ur, ur_ptr, ur_idx, func, emit_rewritten_node);
    emit_rewritten_node_impl(e, node, func);
    if (ur) emit(e, ")");
    if (asg_paren) emit(e, ")");
    e->nn_lvalue = saved_lv;
}

static void emit_rewritten_node_impl(Emitter *e, Node *node, IRFunc *func) {
    if (!node) return;

    switch (node->kind) {
    case NODE_IDENT: {
        /* Rewritten ident — emit name (IR local, global, or mangled cross-module) */
        const char *iname = node->ident.name;
        uint32_t ilen = (uint32_t)node->ident.name_len;
        if (e->in_async) {
            for (int i = 0; i < e->async_local_count; i++) {
                if (e->async_local_lens[i] == (size_t)ilen &&
                    memcmp(e->async_locals[i], iname, ilen) == 0) {
                    emit(e, "self->%.*s", (int)ilen, iname);
                    return;
                }
            }
        }
        /* Check if this is a cross-module function needing mangled name.
         * If the ident is NOT an IR local, look up in scope. If the symbol
         * has a module_prefix, emit module__name instead of bare name. */
        if (func) {
            int lid = -1;
            for (int li = 0; li < func->local_count; li++) {
                if ((func->locals[li].name_len == ilen &&
                     memcmp(func->locals[li].name, iname, ilen) == 0) ||
                    (func->locals[li].orig_name_len == ilen &&
                     memcmp(func->locals[li].orig_name, iname, ilen) == 0)) {
                    lid = li; break;
                }
            }
            if (lid < 0) {
                /* Not a local — apply module name mangling.
                 * For functions: use symbol's module_prefix (functions are
                 * unique per name across modules).
                 * For variables in current module: use e->current_module
                 * (same-named variables in different modules need correct prefix).
                 * This matches EMIT_MANGLED_NAME for the current module's own
                 * symbols, and symbol->module_prefix for cross-module calls. */
                Symbol *sym = scope_lookup(e->checker->global_scope, iname, ilen);
                /* BUG-1120: ONE rule for which module a non-local name belongs to.
                 * If the current module registered it (its own global, function or
                 * static all have the mangled key `<module>__<name>`), it is this
                 * module's; otherwise it is whoever owns the raw entry. The old
                 * split guessed: a FUNCTION always took the raw entry's module
                 * (two modules each defining `bump()` made module b call module
                 * a's), and a VARIABLE always took the current module (module b
                 * reading module a's `shared_total` emitted an undeclared
                 * `mb__shared_total`). */
                if (e->current_module) {
                    uint32_t mkl = e->current_module_len + 2 + ilen;
                    char *mk = (char *)arena_alloc(e->arena, mkl + 1);
                    if (mk) {
                        memcpy(mk, e->current_module, e->current_module_len);
                        mk[e->current_module_len] = '_';
                        mk[e->current_module_len + 1] = '_';
                        memcpy(mk + e->current_module_len + 2, iname, ilen);
                        mk[mkl] = '\0';
                        if (scope_lookup_local(e->checker->global_scope, mk, mkl)) {
                            emit(e, "%.*s__%.*s",
                                 (int)e->current_module_len, e->current_module,
                                 (int)ilen, iname);
                            return;
                        }
                    }
                }
                if (sym && sym->module_prefix) {
                    emit(e, "%.*s__%.*s",
                         (int)sym->module_prefix_len, sym->module_prefix,
                         (int)ilen, iname);
                    return;
                }
                /* No symbol found — if in a module context, assume
                 * module-private (static) variable → use current_module prefix */
                if (!sym && e->current_module) {
                    emit(e, "%.*s__%.*s",
                         (int)e->current_module_len, e->current_module,
                         (int)ilen, iname);
                    return;
                }
            }
        }
        emit(e, "%.*s", (int)ilen, iname);
        return;
    }

    case NODE_INT_LIT:
        emit_int_literal(e, node);
        return;
    case NODE_FLOAT_LIT:
        emit_double_lit(e, node->float_lit.value,
                           emit_type_is_f32(checker_get_type(e->checker, node)));
        return;
    case NODE_BOOL_LIT:
        emit(e, "%d", node->bool_lit.value ? 1 : 0);
        return;
    case NODE_CHAR_LIT:
        emit_char_lit(e, node);
        return;
    case NODE_NULL_LIT:
        emit(e, "0");
        return;
    case NODE_STRING_LIT:
        /* sizeof("...") - 1 so C resolves escapes; source-char count
         * was off-by-many for any escape sequence, silently inflating
         * .len past actual byte count. */
        emit_zer_string_slice(e, node->string_lit.value, (int)node->string_lit.length, false);
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
        /* BUG-604: shift safety (ZER spec: shift >= width = 0).
         * emit_rewritten_node is called for expressions preserved in
         * IR_ASSIGN/IR_CALL/IR_AWAIT/IR_RETURN inst->expr (e.g. array
         * index writes `arr[i] = x`, field writes `s.f = x`, await
         * conditions). Without this, raw C `<<`/`>>` leaks through,
         * producing UB on n >= width. Mirrors emit_expr NODE_BINARY
         * shift handling. */
        if (node->binary.op == TOK_LSHIFT || node->binary.op == TOK_RSHIFT) {
            /* uN/iN shift RESULT must be width-wrapped to N bits, exactly like
             * +,-,*,&,|,^ and the IR path (emit_intn_mask after _zer_shl at
             * IR_BINOP). _zer_shl/_zer_shr only guard shift-by->=CARRIER-width;
             * they do NOT mask the result to N. So `u3 a = 3; u32 x; x = a << 2;`
             * gave 12 instead of 4, and an iN shift kept the wrong SIGN. This
             * AST-passthrough store path was the ONLY one missing the mask —
             * var-decl / return / call-arg lower to IR_BINOP and were already
             * correct. The mask is a value no-op on a right shift but keeps iN
             * sign-extension right. */
            Type *rts = type_unwrap_distinct(checker_get_type(e->checker, node));
            TypeKind rsk = type_dispatch_kind(rts);
            bool narrow_s = (rsk == TYPE_U8 || rsk == TYPE_I8 ||
                             rsk == TYPE_U16 || rsk == TYPE_I16);
            if (narrow_s || type_is_nonnative_intn(rts)) {
                int tmp = e->temp_count++;
                char lvbuf[32];
                snprintf(lvbuf, sizeof(lvbuf), "_zer_sw%d", tmp);
                emit(e, "({ ");
                emit_type(e, rts);
                emit(e, " %s = %s(", lvbuf,
                     node->binary.op == TOK_LSHIFT ? "_zer_shl" : "_zer_shr");
                emit_rewritten_node(e, node->binary.left, func);
                emit(e, ", ");
                emit_rewritten_node(e, node->binary.right, func);
                emit(e, ", %d); ", shift_guard_width(checker_get_type(e->checker, node->binary.left)));
                emit_intn_mask_lv(e, rts, lvbuf);
                emit(e, "%s; })", lvbuf);
                return;
            }
            emit(e, "%s(", node->binary.op == TOK_LSHIFT ? "_zer_shl" : "_zer_shr");
            emit_rewritten_node(e, node->binary.left, func);
            emit(e, ", ");
            emit_rewritten_node(e, node->binary.right, func);
            emit(e, ", %d)", shift_guard_width(checker_get_type(e->checker, node->binary.left)));
            return;
        }
        /* BUG-604: signed division overflow (INT_MIN / -1 is C UB).
         *
         * Defense-in-depth div-by-zero trap (audit 2026-05-14): checker's
         * forced-guard at checker.c:2587 only catches IDENT/FIELD/CALL
         * divisors. Index (`arr[i]`), deref (`*p`), cast (`(i32)x`),
         * intrinsic (`@truncate(...)`), and binary (`a+b`) expressions
         * slip through to raw division — SIGFPE on x86/hosted, silent
         * garbage on ARM/RISC-V baremetal. Mirror compound `/=` (line
         * 5879) and always emit the runtime check here too. */
        if (node->binary.op == TOK_SLASH || node->binary.op == TOK_PERCENT) {
            Type *div_type = lt ? type_unwrap_distinct(lt) : NULL;
            bool is_signed_div = div_type && type_is_signed(div_type);
            int tmp = e->temp_count++;
            emit(e, "({ __typeof__(");
            emit_rewritten_node(e, node->binary.right, func);
            emit(e, ") _zer_dv%d = ", tmp);
            emit_rewritten_node(e, node->binary.right, func);
            emit(e, "; if (_zer_dv%d == 0) ", tmp);
            emit(e, "_zer_trap(\"division by zero\", __FILE__, __LINE__); ");
            if (is_signed_div) {
                emit(e, "if (_zer_dv%d == -1) { __typeof__(", tmp);
                emit_rewritten_node(e, node->binary.left, func);
                emit(e, ") _zer_dd%d = ", tmp);
                emit_rewritten_node(e, node->binary.left, func);
                /* BUG-1062: the MIN of THIS width, incl. iN and i128. */
                char dmin[96]; signed_min_text(div_type, dmin, sizeof dmin);
                emit(e, "; if (_zer_dd%d == %s) ", tmp, dmin);
                emit(e, "_zer_trap(\"signed division overflow\", __FILE__, __LINE__); } ");
            }
            emit(e, "(");
            emit_rewritten_node(e, node->binary.left, func);
            emit(e, " %s _zer_dv%d); })",
                 node->binary.op == TOK_SLASH ? "/" : "%", tmp);
            return;
        }
        /* G3 (2026-08-01): uN / narrow arithmetic WIDTH WRAP.
         * A plain `x = a + b` is kept whole by lower_expr (passthrough —
         * NODE_ASSIGN op==TOK_EQ), so it is emitted HERE rather than lowered to
         * IR_BINOP. No emit_intn_mask runs and no narrow_cast is applied, so a
         * narrow/uN result stored into a WIDER lvalue kept the un-wrapped,
         * C-integer-promoted value: `u3 a=7,b=7; u32 x; x = a+b;` gave 14, not
         * 6; `u8 200+100` gave 300, not 44. A SILENT value miscompile — it
         * compiles clean and returns the wrong answer. Both sibling paths
         * already wrap: the var-decl path via IR_BINOP + emit_intn_mask, the
         * AST path via narrow_cast.
         *
         * This is the AST->IR emission-diff class (CLAUDE.md): a safety wrapper
         * present on one emit path and missing on the other. emit_intn_mask was
         * NOT in that audit's grep list — added in this commit.
         *
         * Comparisons and logical ops yield bool and are untouched. Shift is
         * handled by _zer_shl/_zer_shr above; div/mod by the trap block above,
         * whose stmt-expr is the precedent for this one. */
        if (node->binary.op == TOK_PLUS || node->binary.op == TOK_MINUS ||
            node->binary.op == TOK_STAR || node->binary.op == TOK_AMP ||
            node->binary.op == TOK_PIPE || node->binary.op == TOK_CARET) {
            Type *rtw = type_unwrap_distinct(checker_get_type(e->checker, node));
            TypeKind rwk = type_dispatch_kind(rtw);
            bool narrow_native = (rwk == TYPE_U8 || rwk == TYPE_I8 ||
                                  rwk == TYPE_U16 || rwk == TYPE_I16);
            if (narrow_native || type_is_nonnative_intn(rtw)) {
                int tmp = e->temp_count++;
                char lvbuf[32];
                snprintf(lvbuf, sizeof(lvbuf), "_zer_bw%d", tmp);
                emit(e, "({ ");
                emit_type(e, rtw);
                emit(e, " %s = (", lvbuf);
                emit_rewritten_node(e, node->binary.left, func);
                emit(e, " %s ", op);
                emit_rewritten_node(e, node->binary.right, func);
                emit(e, "); ");
                emit_intn_mask_lv(e, rtw, lvbuf);
                emit(e, "%s; })", lvbuf);
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
        /* G3 (2026-08-01): same width wrap for the VALUE-PRODUCING unaries.
         * `u8 z = 0; u32 n; n = ~z;` kept the raw C result 0xFFFFFFFF instead
         * of 255. Only `-` and `~` produce a width-sensitive value; logical-not
         * yields bool, and deref / addr-of are not arithmetic. */
        if (node->unary.op == TOK_MINUS || node->unary.op == TOK_TILDE) {
            Type *rtu = type_unwrap_distinct(checker_get_type(e->checker, node));
            TypeKind ruk = type_dispatch_kind(rtu);
            bool narrow_u = (ruk == TYPE_U8 || ruk == TYPE_I8 ||
                             ruk == TYPE_U16 || ruk == TYPE_I16);
            if (narrow_u || type_is_nonnative_intn(rtu)) {
                int tmp = e->temp_count++;
                char lvbuf[32];
                snprintf(lvbuf, sizeof(lvbuf), "_zer_uw%d", tmp);
                emit(e, "({ ");
                emit_type(e, rtu);
                emit(e, " %s = (%s", lvbuf,
                     node->unary.op == TOK_MINUS ? "-" : "~");
                emit_rewritten_node(e, node->unary.operand, func);
                emit(e, "); ");
                emit_intn_mask_lv(e, rtu, lvbuf);
                emit(e, "%s; })", lvbuf);
                return;
            }
        }
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
                    /* BUG-1053: the checker's resolution (slab_source first), not
                     * a fresh unique-allocator search, which is NULL as soon as
                     * two allocators of this element type exist and used to
                     * emit a literal `0` for the read (and `0 = v` for a write). */
                    Symbol *alloc_sym = node->field.handle_alloc
                        ? node->field.handle_alloc
                        : find_unique_allocator(e->checker->global_scope, elem);
                    if (alloc_sym) {
                        Type *alloc_type = type_unwrap_distinct(alloc_sym->type);
                        if (alloc_type->kind == TYPE_SLAB) {
                            emit(e, "((");
                            emit_type(e, type_pointer(e->arena, elem));
                            emit(e, ")_zer_slab_get(&"); emit_alloc_sym_cname(e, alloc_sym); emit(e, ", ");
                            emit_rewritten_node(e, node->field.object, func);
                            emit(e, "))->%.*s",
                                 (int)node->field.field_name_len, node->field.field_name);
                        } else if (alloc_type->kind == TYPE_POOL) {
                            emit(e, "((");
                            emit_type(e, type_pointer(e->arena, elem));
                            emit(e, ")_zer_pool_get("); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".slots, "); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".gen, "); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".used, sizeof("); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".slots[0]), ");
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
            /* Fallback for nested NODE_FIELD (e.g. range-for `g.data.len` where
             * `g.data` is a cloned NODE_FIELD that may not be in the typemap):
             * walk struct fields to resolve the type of the inner field. */
            if (!obj_type && node->field.object &&
                node->field.object->kind == NODE_FIELD) {
                Node *inner = node->field.object;
                Type *inner_obj = checker_get_type(e->checker, inner->field.object);
                if (!inner_obj && inner->field.object &&
                    inner->field.object->kind == NODE_IDENT) {
                    Symbol *sym = scope_lookup(e->checker->global_scope,
                        inner->field.object->ident.name,
                        (uint32_t)inner->field.object->ident.name_len);
                    if (sym) inner_obj = sym->type;
                }
                if (inner_obj) {
                    Type *ie = type_unwrap_distinct(inner_obj);
                    if (ie->kind == TYPE_POINTER)
                        ie = type_unwrap_distinct(ie->pointer.inner);
                    if (ie->kind == TYPE_STRUCT) {
                        for (uint32_t i = 0; i < ie->struct_type.field_count; i++) {
                            SField *f = &ie->struct_type.fields[i];
                            if (f->name_len == inner->field.field_name_len &&
                                memcmp(f->name, inner->field.field_name,
                                       f->name_len) == 0) {
                                obj_type = f->type;
                                break;
                            }
                        }
                    }
                }
            }
            /* Array .len on a resolved nested-field object → emit literal size */
            if (obj_type && node->field.field_name_len == 3 &&
                memcmp(node->field.field_name, "len", 3) == 0) {
                Type *oe = type_unwrap_distinct(obj_type);
                if (oe->kind == TYPE_ARRAY) {
                    emit(e, "%uU", (unsigned)oe->array.size);
                    return;
                }
            }
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
                    Symbol *alloc_sym = node->field.handle_alloc   /* BUG-1053 */
                        ? node->field.handle_alloc
                        : find_unique_allocator(e->checker->global_scope, elem);
                    if (alloc_sym) {
                        Type *alloc_type = type_unwrap_distinct(alloc_sym->type);
                        emit(e, "((");
                        emit_type(e, type_pointer(e->arena, elem));
                        if (alloc_type->kind == TYPE_SLAB) {
                            emit(e, ")_zer_slab_get(&"); emit_alloc_sym_cname(e, alloc_sym); emit(e, ", ");
                        } else if (alloc_type->kind == TYPE_POOL) {
                            emit(e, ")_zer_pool_get("); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".slots, "); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".gen, "); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".used, sizeof("); emit_alloc_sym_cname(e, alloc_sym); emit(e, ".slots[0]), ");
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
            /* G2 (2026-08-01): same parenthesization on the IR path — the
             * emitter's dual-dispatch rule (AST ~2400 + IR ~6500). Fixing only
             * one path leaves the other mis-emitting. */
            {
                bool fp = field_obj_needs_parens(node->field.object);
                if (fp) emit(e, "(");
                emit_rewritten_node(e, node->field.object, func);
                if (fp) emit(e, ")");
            }
            emit(e, "%s%.*s", acc, (int)node->field.field_name_len, node->field.field_name);
        }
        return;
    }

    case NODE_INDEX: {
        /* Index: emit obj[idx] with bounds check.
         * Phase 3 fix (Gap 0): emit _zer_bounds_check for slices.
         * Audit 2026-05-26: also emit _zer_bounds_check for arrays when
         * the index is non-trivial (NODE_FIELD/NODE_INDEX/NODE_BINARY etc.).
         * Audit 2026-05-28: side-effect-bearing index OR object now uses
         * single-eval `*({...})` pattern to avoid double-evaluation.
         *
         * Pre-fix patterns:
         *   `arr[i.field]`, `arr[b.f.g]`, `arr[x+y]` silently read OOB
         *     (auto-guard only fired for NODE_IDENT/NODE_CALL).
         *   `slice[get_idx()]` silently called get_idx() TWICE (comma form). */
        Type *idx_obj_type = checker_get_type(e->checker, node->index_expr.object);
        Type *idx_obj_eff = idx_obj_type ? type_unwrap_distinct(idx_obj_type) : NULL;
        bool idx_slice = idx_obj_eff && idx_obj_eff->kind == TYPE_SLICE;
        bool idx_array = idx_obj_eff && idx_obj_eff->kind == TYPE_ARRAY;
        bool idx_se = expr_has_side_effects(node->index_expr.index);
        bool obj_se = expr_has_side_effects(node->index_expr.object);
        /* BUG-749 (2026-06-18): a volatile read in the index expression
         * must single-eval, exactly like a CALL/INTRINSIC. Without this
         * check, a fixed-array index of the form `arr[reg.status]` (with
         * volatile field) emits `_zer_bounds_check((size_t)(reg->status),
         * 16, ...) , arr)[reg->status]` — two C-level loads of the same
         * volatile location. Side-effects (read-clear, FIFO, sequence
         * counter) get duplicated; on baremetal the second read sees a
         * different value or pops a second entry. expr_is_volatile spans
         * the field chain; OR it in to route through the single-eval
         * statement-expression branch. */
        if (expr_is_volatile(e, node->index_expr.index)) idx_se = true;
        if (expr_is_volatile(e, node->index_expr.object)) obj_se = true;
        /* BUG-1098: an ident index the checker declined to auto-guard because the
         * statement may change it — the check and the access must read it ONCE,
         * whatever order C evaluates the rest of the statement in (the comma form
         * reads it twice, and `(check(i), a)[i] = g()` leaves g() unsequenced
         * against both reads). */
        if (idx_array && node->index_expr.index->kind == NODE_IDENT &&
            !checker_is_proven(e->checker, node) &&
            !checker_has_auto_guard(e->checker, node))
            idx_se = true;
        if (idx_slice) {
            if (idx_se || obj_se) {
                int tmp = e->temp_count++;
                emit(e, "*({ __typeof__(");
                emit_rewritten_node(e, node->index_expr.object, func);
                emit(e, ") _zer_obj%d = ", tmp);
                emit_rewritten_node(e, node->index_expr.object, func);
                emit(e, "; size_t _zer_idx%d = (size_t)(", tmp);
                emit_rewritten_node(e, node->index_expr.index, func);
                emit(e, "); _zer_bounds_check(_zer_idx%d, _zer_obj%d.len, "
                       "__FILE__, __LINE__); &_zer_obj%d.ptr[_zer_idx%d]; })",
                     tmp, tmp, tmp, tmp);
            } else {
                emit(e, "(_zer_bounds_check((size_t)(");
                emit_rewritten_node(e, node->index_expr.index, func);
                emit(e, "), ");
                emit_rewritten_node(e, node->index_expr.object, func);
                emit(e, ".len, __FILE__, __LINE__), ");
                emit_rewritten_node(e, node->index_expr.object, func);
                emit(e, ".ptr)[");
                emit_rewritten_node(e, node->index_expr.index, func);
                emit(e, "]");
            }
        } else if (idx_array && !checker_is_proven(e->checker, node) &&
                   node->index_expr.index->kind != NODE_INT_LIT &&
                   /* BUG-1011: a bare IDENT index normally relies on the auto-guard
                    * pre-pass — but a VOLATILE ident is left unguarded by the checker
                    * on purpose (the guard would read it once and the access again),
                    * so it must take THIS single-evaluation form instead: one load
                    * into a temp, the check and the access both on the temp. */
                   /* BUG-1098: and an IDENT the checker declined to auto-guard
                    * (the statement can change it before this read, so a hoisted
                    * guard would test a stale value) — the general rule is that
                    * an unproven access with no guard carries its own check. */
                   (node->index_expr.index->kind != NODE_IDENT ||
                    expr_is_volatile(e, node->index_expr.index) ||
                    !checker_has_auto_guard(e->checker, node)) &&
                   /* BH-18 #5 (copied from cool-johnson-t8vr3h): a bare-CALL index
                    * on a fixed array previously fell through to the raw emit,
                    * relying on the auto-guard pre-pass — which only fires for
                    * known-range callees, so unknown-range calls silently OOB'd.
                    * Routing NODE_CALL through this branch produces the single-eval
                    * *({ size_t _zer_idx = call(); bounds_check; &a[_zer_idx]; })
                    * pattern (the same shape NODE_BINARY/FIELD/INDEX already use). */
                   (idx_obj_eff->array.size > 0 || idx_obj_eff->array.sizeof_type)) {
            /* Inline bounds check for non-trivial array indices (FIELD/
             * INDEX/BINARY/UNARY/ASSIGN/ORELSE). Side-effect-bearing
             * expressions use single-eval `*({...})` to avoid double-eval. */
            if (idx_se || obj_se) {
                int tmp = e->temp_count++;
                emit(e, "*({ size_t _zer_idx%d = (size_t)(", tmp);
                emit_rewritten_node(e, node->index_expr.index, func);
                emit(e, "); _zer_bounds_check(_zer_idx%d, ", tmp);
                emit_array_size(e, idx_obj_eff);
                emit(e, ", __FILE__, __LINE__); &");
                emit_rewritten_node(e, node->index_expr.object, func);
                emit(e, "[_zer_idx%d]; })", tmp);
            } else {
                emit(e, "(_zer_bounds_check((size_t)(");
                emit_rewritten_node(e, node->index_expr.index, func);
                emit(e, "), ");
                emit_array_size(e, idx_obj_eff);
                emit(e, ", __FILE__, __LINE__), ");
                emit_rewritten_node(e, node->index_expr.object, func);
                emit(e, ")[");
                emit_rewritten_node(e, node->index_expr.index, func);
                emit(e, "]");
            }
        } else {
            /* Array / pointer indexing — C evaluates arr[expr] once on
             * its own, but if a caller wraps this in a compound op that
             * re-emits the lvalue, we still need single-eval. The
             * caller (compound shift/div) hoists via pointer; here we
             * keep the plain form. */
            emit_rewritten_node(e, node->index_expr.object, func);
            emit(e, "[");
            emit_rewritten_node(e, node->index_expr.index, func);
            emit(e, "]");
        }
        return;
    }

    case NODE_ASSIGN: {
        /* Assignments: emit target op= value from rewritten AST.
         * Complex patterns (bit extract, volatile array, shared lock) need emit_expr. */
        /* Check for complex patterns that need emit_expr */
        Type *tgt_type = checker_get_type(e->checker, node->assign.target);
        Type *tgt_eff = tgt_type ? type_unwrap_distinct(tgt_type) : NULL;

        /* Native uN/iN width masking (odd widths u3/u21/i48/…) — IR path twin of
         * the emit_expr intercept. A store to a non-native-width integer lvalue
         * must re-wrap the result (uN mask / iN sign-extend). var-decl init is
         * masked via the IR_BINOP temp (emit_intn_mask), but assignment &
         * compound-assign are AST-passthrough here and skip it → silent wrong
         * value (`u3 y; y = a+b` keeps bit 3; `s-=1` underflows to 255). Emit
         * store + mask through ONE hoisted pointer so a side-effecting index in
         * the target evaluates once. Scalar targets only (array/union/bit-slice
         * are handled by the dedicated cases below and don't carry a scalar
         * uN/iN type here). /= %= >>= can't exceed the width (result magnitude ≤
         * operand) so they keep their existing div-guard / shift paths. */
        if (emit_intn_store(e, node, func, emit_rewritten_node)) return;   /* BUG-1162 */

        /* BUG-582: Union variant assignment — port from emit_expr (line 1210+)
         * with extension: also handle `u.variant[i] = val` and deeper chains
         * by walking up the target through NODE_INDEX/NODE_FIELD until we
         * find a NODE_FIELD whose object is a union. Emits a statement
         * expression that hoists the union pointer, sets `_tag`, then does
         * the actual target assignment through the same pointer.
         *
         * Without this, `u._tag` stays at its zero-init value, and subsequent
         * `switch (u)` takes the wrong arm. The AST path had the plain-field
         * case but missed index/nested-field chains — same bug, different
         * manifestation. Fix here covers both. */
        if (node->assign.op == TOK_EQ && node->assign.target) {
            /* Walk up: find the NODE_FIELD whose object is a union. The object
             * may be a union VALUE, or (ZER auto-deref `ptr.variant`) a
             * POINTER-to-union — writing a variant through a `*Union` param must
             * still update the discriminant `_tag`, else `switch(u)` at the
             * caller reads the wrong arm (union type confusion, silent for
             * non-pointer variants). `obj_is_ptr` records which so emission can
             * use the pointer directly instead of taking its address (§E #31). */
            Node *walk = node->assign.target;
            Node *union_field = NULL;  /* the NODE_FIELD(union, variant) */
            bool obj_is_ptr = false;   /* object is *Union (auto-deref) */
            while (walk) {
                if (walk->kind == NODE_FIELD) {
                    Type *ot = checker_get_type(e->checker, walk->field.object);
                    Type *ot_eff = ot ? type_unwrap_distinct(ot) : NULL;
                    if (ot_eff && ot_eff->kind == TYPE_UNION) {
                        union_field = walk;
                        obj_is_ptr = false;
                        break;
                    }
                    if (ot_eff && ot_eff->kind == TYPE_POINTER) {
                        Type *pinner = type_unwrap_distinct(ot_eff->pointer.inner);
                        if (pinner && pinner->kind == TYPE_UNION) {
                            union_field = walk;
                            obj_is_ptr = true;
                            break;
                        }
                    }
                    walk = walk->field.object;
                } else if (walk->kind == NODE_INDEX) {
                    walk = walk->index_expr.object;
                } else if (walk->kind == NODE_UNARY && walk->unary.op == TOK_STAR) {
                    /* Deref — keep walking */
                    walk = walk->unary.operand;
                } else {
                    break;
                }
            }
            if (union_field) {
                Node *obj_node = union_field->field.object;
                Type *obj_type_raw = checker_get_type(e->checker, obj_node);
                Type *obj_type = obj_type_raw ? type_unwrap_distinct(obj_type_raw) : NULL;
                /* For the pointer case the variant metadata lives on the pointee
                 * union, not the pointer type. */
                if (obj_is_ptr && obj_type && obj_type->kind == TYPE_POINTER)
                    obj_type = type_unwrap_distinct(obj_type->pointer.inner);
                const char *vname = union_field->field.field_name;
                uint32_t vlen = (uint32_t)union_field->field.field_name_len;
                for (uint32_t i = 0; obj_type && i < obj_type->union_type.variant_count; i++) {
                    SUVariant *v = &obj_type->union_type.variants[i];
                    if (v->name_len == vlen && memcmp(v->name, vname, vlen) == 0) {
                        int tmp = e->temp_count++;
                        if (obj_is_ptr) {
                            /* obj_node is already a `*Union`; hoist it directly
                             * (no `&`) so `_zer_up->_tag` writes through it. */
                            emit(e, "({ __typeof__(");
                            emit_rewritten_node(e, obj_node, func);
                            emit(e, ") _zer_up%d = (", tmp);
                            emit_rewritten_node(e, obj_node, func);
                            emit(e, "); _zer_up%d->_tag = %u; ", tmp, i);
                        } else {
                        emit(e, "({ __typeof__(");
                        emit_rewritten_node(e, obj_node, func);
                        emit(e, ") *_zer_up%d = &(", tmp);
                        emit_rewritten_node(e, obj_node, func);
                        emit(e, "); _zer_up%d->_tag = %u; ", tmp, i);
                        }
                        /* Re-emit the target but replacing the hoisted
                         * union object with *_zer_up%d. For `u.variant = v`
                         * this is `_zer_up%d->variant = v`. For
                         * `u.variant[i] = v` this is `_zer_up%d->variant[i] = v`.
                         * Simplest: emit the full target via emit_rewritten_node
                         * (uses same object expr — not single-eval ideal but
                         * same as pre-fix behavior for other complex targets). */
                        emit_rewritten_node(e, node->assign.target, func);
                        emit(e, " = ");
                        emit_rewritten_node(e, node->assign.value, func);
                        emit(e, "; })");
                        return;
                    }
                }
            }
        }

        /* Bit extract SET: reg[hi..lo] OP= val — one emitter (BUG-1198). */
        if (node->assign.target && node->assign.target->kind == NODE_SLICE) {
            emit_bitslice_set(e, node, func, emit_rewritten_node);
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
        /* Compound shift: target <<= n / target >>= n.
         * BUG-608 (5HwfE) fixed binary `<<`/`>>` in this function; the
         * compound-assign forms were missed (BUG-612). Without _zer_shl/
         * _zer_shr, raw C `<<=`/`>>=` with n >= width is undefined behavior
         * (GCC -O0 produces hardware-masked result; -O2 sometimes folds
         * accidentally to spec-correct, but it's not guaranteed). Mirrors
         * emit_expr line 1375+: emit `target = _zer_shl(target, n)` with
         * pointer-hoist for side-effectful targets. (IR lowering already
         * extracts call/orelse out of targets, so the simple form is
         * normally safe — pointer-hoist is defense in depth.) */
        if (node->assign.op == TOK_LSHIFTEQ || node->assign.op == TOK_RSHIFTEQ) {
            /* Side-effect check: use the unified walker so indexed targets
             * like `arr[fn()] <<= n` are correctly classified. The prior
             * partial walker descended only through NODE_FIELD.object /
             * NODE_INDEX.object and missed side effects in
             * NODE_INDEX.index — silently double-evaluating fn(). */
            bool shift_side_effect = expr_has_side_effects(node->assign.target);
            const char *macro = node->assign.op == TOK_LSHIFTEQ ? "_zer_shl" : "_zer_shr";
            int shw = shift_guard_width(checker_get_type(e->checker, node->assign.target));
            if (shift_side_effect) {
                int tmp = e->temp_count++;
                emit(e, "({ __auto_type _zer_sp%d = &(", tmp);
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, "); *_zer_sp%d = %s(*_zer_sp%d, ", tmp, macro, tmp);
                emit_rewritten_node(e, node->assign.value, func);
                emit(e, ", %d); })", shw);
            } else {
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, " = %s(", macro);
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, ", ");
                emit_rewritten_node(e, node->assign.value, func);
                emit(e, ", %d)", shw);
            }
            return;
        }
        /* Compound div/mod: target /= n / target %= n.
         * BUG-608 covered binary `/`/`%`; compound versions were missed
         * (BUG-612). Two safety properties enforced:
         *   (1) divisor != 0 (defense in depth — checker forces compile-time
         *       guard, but emit_expr line 1361 still adds runtime trap)
         *   (2) on signed types, INT_MIN/-1 is C UB → trap. Mirrors
         *       emit_expr binary path at 5099-5127. */
        if (node->assign.op == TOK_SLASHEQ || node->assign.op == TOK_PERCENTEQ) {
            Type *div_type = tgt_eff;
            bool is_signed_div = div_type && type_is_signed(div_type);
            const char *cop = node->assign.op == TOK_SLASHEQ ? "/" : "%";
            int tmp = e->temp_count++;
            /* Side-effect hoist: if target contains a call (e.g.
             * `arr[fn()] /= y`), the INT_MIN check and the actual
             * division both re-emit the target — silently
             * double-evaluating fn(). Hoist via pointer to single-eval. */
            bool tgt_se = expr_has_side_effects(node->assign.target);
            if (tgt_se) {
                emit(e, "({ __typeof__(");
                emit_rewritten_node(e, node->assign.value, func);
                emit(e, ") _zer_dv%d = ", tmp);
                emit_rewritten_node(e, node->assign.value, func);
                emit(e, "; if (_zer_dv%d == 0) "
                       "_zer_trap(\"division by zero\", __FILE__, __LINE__); ",
                     tmp);
                emit(e, "__auto_type _zer_dp%d = &(", tmp);
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, "); ");
                if (is_signed_div) {
                    emit(e, "if (_zer_dv%d == -1) { __typeof__(*_zer_dp%d) _zer_dd%d = *_zer_dp%d; ",
                         tmp, tmp, tmp, tmp);
                    /* BUG-1062: the MIN of THIS width, incl. iN and i128. */
                    char dmin[96]; signed_min_text(div_type, dmin, sizeof dmin);
                    emit(e, "if (_zer_dd%d == %s) ", tmp, dmin);
                    emit(e, "_zer_trap(\"signed division overflow\", __FILE__, __LINE__); } ");
                }
                emit(e, "*_zer_dp%d %s= _zer_dv%d; })", tmp, cop, tmp);
                return;
            }
            emit(e, "({ __typeof__(");
            emit_rewritten_node(e, node->assign.value, func);
            emit(e, ") _zer_dv%d = ", tmp);
            emit_rewritten_node(e, node->assign.value, func);
            emit(e, "; if (_zer_dv%d == 0) ", tmp);
            emit(e, "_zer_trap(\"division by zero\", __FILE__, __LINE__); ");
            if (is_signed_div) {
                emit(e, "if (_zer_dv%d == -1) { __typeof__(", tmp);
                emit_rewritten_node(e, node->assign.target, func);
                emit(e, ") _zer_dd%d = ", tmp);
                emit_rewritten_node(e, node->assign.target, func);
                /* BUG-1062: the MIN of THIS width, incl. iN and i128. */
                char dmin[96]; signed_min_text(div_type, dmin, sizeof dmin);
                emit(e, "; if (_zer_dd%d == %s) ", tmp, dmin);
                emit(e, "_zer_trap(\"signed division overflow\", __FILE__, __LINE__); } ");
            }
            emit_rewritten_node(e, node->assign.target, func);
            emit(e, " %s= _zer_dv%d; })", cop, tmp);
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
                /* #15 (B): array into ?[*]T assignment-expression → coerce to a
                 * {ptr,len} slice literal (else the bare array flattens). */
                Type *aw_inner = tgt_eff->optional.inner
                    ? type_unwrap_distinct(tgt_eff->optional.inner) : NULL;
                if (aw_inner && type_dispatch_kind(aw_inner) == TYPE_SLICE &&
                    type_dispatch_kind(val_eff) == TYPE_ARRAY)
                    emit_array_as_slice(e, node->assign.value, val_eff, aw_inner);
                else
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
                emit_double_lit(e, node->call.comptime_float_value,
                                   emit_type_is_f32(checker_get_type(e->checker, node)));
            else
                emit(e, "%lld", (long long)node->call.comptime_value);
            return;
        }
        /* Universal alloc(T,n) -> ?[*]T (calloc) and free(slice) -> free(ptr).
         * ident-callee builtins; T recovered from args[0] type name. Mirrors the
         * arena.alloc_slice slice-wrap but heap-backed + escapable. See
         * docs/universal_alloc.md. */
        if (node->call.callee && node->call.callee->kind == NODE_IDENT) {
            const char *cn2 = node->call.callee->ident.name;
            uint32_t cl2 = (uint32_t)node->call.callee->ident.name_len;
            if (cl2 == 5 && memcmp(cn2, "alloc", 5) == 0 &&
                node->call.arg_count == 2 &&
                checker_get_type(e->checker, node) &&
                type_dispatch_kind(checker_get_type(e->checker, node)) == TYPE_OPTIONAL) {
                /* element type recovered from the result type ?[*]T — works for
                 * struct AND primitive element types uniformly. */
                Type *rt = checker_get_type(e->checker, node);       /* ?[*]T */
                Type *sl = type_unwrap_distinct(rt)->optional.inner;  /* [*]T  */
                if (sl && type_dispatch_kind(sl) == TYPE_SLICE) {
                    Type *elem = type_unwrap_distinct(sl)->slice.inner; /* T */
                    int t = e->temp_count++;
                    emit(e, "({size_t _zer_hn%d=", t);
                    emit_rewritten_node(e, node->call.args[1], func);
                    emit(e, ";void *_zer_hp%d=calloc(_zer_hn%d,sizeof(", t, t);
                    /* BUG-1029: emit_type spells a struct WITH its module prefix
                     * (`struct lib__Task`); the bare `struct Task` used here made
                     * every alloc(T,n) inside an imported module a GCC
                     * "incomplete type" error. BUG-1027: the cast needs the
                     * funcptr/array declarator spelling, not `emit_type(T)*`. */
                    emit_type(e, elem);
                    emit(e, "));_zer_hp%d?(", t);
                    emit_type(e, rt); emit(e, "){("); emit_type(e, sl); emit(e, "){(");
                    emit_ptr_to_elem(e, elem, false);
                    emit(e, ")_zer_hp%d,_zer_hn%d},1}:(", t, t);
                    emit_type(e, rt); emit(e, "){0};})");
                    return;
                }
            }
            if (cl2 == 4 && memcmp(cn2, "free", 4) == 0 &&
                node->call.arg_count == 1) {
                Type *at = checker_get_type(e->checker, node->call.args[0]);
                if (at && type_dispatch_kind(at) == TYPE_SLICE) {
                    emit(e, "free((void*)(");
                    emit_rewritten_node(e, node->call.args[0], func);
                    emit(e, ").ptr)");
                    return;
                }
            }
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
                    /* Builtin — emit inline C via emit_builtin_inline */
                    if (emit_builtin_inline(e, node, func)) return;
                    /* Unhandled builtin falls through to regular call */
                }
            }
        }
        /* BUG-1019: same guard on the IR-rewritten dispatch path. */
        if (call_needs_null_funcptr_guard(e, node->call.callee)) {
            int fpt = e->temp_count++;
            emit(e, "({ __typeof__(");
            emit_rewritten_node(e, node->call.callee, func);
            emit(e, ") _zer_fp%d = ", fpt);
            emit_rewritten_node(e, node->call.callee, func);
            emit(e, "; if (!_zer_fp%d) _zer_trap(\"call through a null function "
                    "pointer\", __FILE__, __LINE__); _zer_fp%d; })", fpt, fpt);
        } else {
            emit_rewritten_node(e, node->call.callee, func);
        }
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
                            emit(e, "struct _union_%.*s", (int)te->union_type.name_len, te->union_type.name);
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
                       node->intrinsic.args[0]->kind == NODE_IDENT &&
                       checker_get_type(e->checker, node->intrinsic.args[0])) {
                /* BUG-1038 — IR-path twin of the AST-path arm: the checker's
                 * resolved type of the operand (type name / uN / variable). */
                emit_type(e, checker_get_type(e->checker, node->intrinsic.args[0]));
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
                        emit(e, "struct _union_%.*s", (int)te->union_type.name_len, te->union_type.name);
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
        } else if (nlen == 8 && memcmp(name, "truncate", 8) == 0) {
            /* @truncate(T, val) → (T)(val). Non-native uN/iN: mask the result
             * (IR-path twin of the emit_expr site) — covers inline uses. */
            Type *tt = node->intrinsic.type_arg ? resolve_tynode(e, node->intrinsic.type_arg) : NULL;
            /* BUG-864 — IR-path twin. CLAUDE.md's dual-dispatch rule: an
             * intrinsic handler exists at BOTH the AST and the IR site, and a
             * safety wrapper added to one and not the other is a silent hole
             * that only some spellings reach. */
            if (type_is_nonnative_intn(tt) || type_carries_enum_e(tt, 0)) {
                int tmp = e->temp_count++;
                char lv[40]; snprintf(lv, sizeof lv, "_zer_tr%d", tmp);
                emit(e, "({ "); emit_type(e, tt);
                emit(e, " _zer_tr%d = (", tmp); emit_type(e, tt); emit(e, ")(");
                if (node->intrinsic.arg_count > 0) emit_rewritten_node(e, node->intrinsic.args[0], func);
                else emit(e, "0");
                emit(e, "); ");
                emit_intn_mask_lv(e, tt, lv);
                emit_enum_variant_guard_path(e, tt, lv, "@truncate", 0);
                emit(e, "_zer_tr%d; })", tmp);
            } else {
                emit(e, "(");
                if (tt) emit_type(e, tt);
                emit(e, ")(");
                if (node->intrinsic.arg_count > 0)
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ")");
            }
        } else if (nlen == 8 && memcmp(name, "saturate", 8) == 0) {
            /* @saturate(T, val) → clamp to T range */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                /* BUG-910: @saturate is the THIRD door into an enum target, and it
                 * had no variant guard. BUG-843 closed @bitcast; BUG-891 added the
                 * carrier walk and @truncate at both dispatch paths — and left this
                 * sibling, so `@saturate(State, 7)` still forged a value outside the
                 * variant set and the exhaustive switch silently ran its LAST arm
                 * (measured: exit 3, no trap). @cast is NOT a fourth door: it
                 * requires a distinct typedef and cannot name a bare enum.
                 *
                 * WRAPPED rather than threaded into the clamp: the two dispatch
                 * paths compute the clamp differently and its value is a bare
                 * ternary, not an lvalue the guard can name. Binding the finished
                 * result to a temp reuses each path's emission VERBATIM, so the
                 * guard cannot drift from the clamp it guards. */
                bool sat_enum = t && type_carries_enum_e(t, 0);
                int seg = sat_enum ? e->temp_count++ : 0;
                if (sat_enum) { emit(e, "({ "); emit_type(e, t); emit(e, " _zer_seg%d = (", seg); }
                Node *sat_arg = node->intrinsic.arg_count > 0 ? node->intrinsic.args[0] : NULL;
                emit_saturate_open(e, sat_arg, tmp);
                if (sat_arg) emit_rewritten_node(e, sat_arg, func);
                else emit(e, "0");
                emit_saturate_close(e, sat_arg, t, tmp);
                if (sat_enum) {
                    char segp[40]; snprintf(segp, sizeof segp, "_zer_seg%d", seg);
                    emit(e, "); ");
                    emit_enum_variant_guard_path(e, t, segp, "@saturate", 0);
                    emit(e, "_zer_seg%d; })", seg);
                }
            }
        } else if (nlen == 7 && memcmp(name, "bitcast", 7) == 0) {
            /* @bitcast(T, val) → memcpy type punning */
            if (node->intrinsic.type_arg) {
                Type *t = resolve_tynode(e, node->intrinsic.type_arg);
                int tmp = e->temp_count++;
                int tmp2 = e->temp_count++;
                /* BUG-1001: IR twin of the AST-path handler above — an array SOURCE
                 * is copied from directly (it decays under `__auto_type`). */
                Node *bsrc = node->intrinsic.arg_count > 0 ? node->intrinsic.args[0] : NULL;
                bool src_is_array = bsrc &&
                    type_dispatch_kind(checker_get_type(e->checker, bsrc)) == TYPE_ARRAY;
                emit(e, "({ ");
                if (!src_is_array) {
                    emit(e, "__auto_type _zer_bci%d = ", tmp2);
                    if (bsrc) emit_rewritten_node(e, bsrc, func);
                    emit(e, "; ");
                }
                emit_type(e, t);
                if (src_is_array) {
                    emit(e, " _zer_bco%d; memcpy(&_zer_bco%d, (", tmp, tmp);
                    emit_rewritten_node(e, bsrc, func);
                    emit(e, "), sizeof(_zer_bco%d)); ", tmp);
                } else {
                    emit(e, " _zer_bco%d; memcpy(&_zer_bco%d, &_zer_bci%d, sizeof(_zer_bco%d)); ",
                         tmp, tmp, tmp2, tmp);
                }
                /* #17: non-native uN/iN target — mask/sign-extend the punned carrier.
                 * The memcpy copies the full carrier (e.g. all 8 bits of a u5's
                 * uint8_t), leaving an over-width / un-sign-extended value; mask (uN)
                 * or sign-extend (iN), the same treatment @truncate applies. */
                if (type_is_nonnative_intn(t)) {
                    char lv[40]; snprintf(lv, sizeof lv, "_zer_bco%d", tmp);
                    emit_intn_mask_lv(e, t, lv);
                }
                { char lv2[40]; snprintf(lv2, sizeof lv2, "_zer_bco%d", tmp);
                  emit_bitcast_enum_guard(e, t, lv2); }   /* BUG-843 */
                emit(e, "_zer_bco%d; })", tmp);
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
                emit_offset_type_operand(e, node->intrinsic.args[0]);   /* BUG-1215 */
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
                    tid = opaque_type_id(inner);   /* BUG-1166 */
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
                    expected_tid = opaque_type_id(inner);   /* BUG-1166 */
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
        } else if (nlen == 3 && memcmp(name, "pun", 3) == 0) {
            /* @pun(*T, expr) — IR-rewritten emission path. Mirrors the
             * AST emission at line ~2871. See that comment block for the
             * full semantics description. */
            Type *tgt_type = node->intrinsic.type_arg ?
                resolve_tynode(e, node->intrinsic.type_arg) : NULL;
            Type *src_type = (node->intrinsic.arg_count > 0) ?
                checker_get_type(e->checker, node->intrinsic.args[0]) : NULL;
            Type *tgt_eff = tgt_type ? type_unwrap_distinct(tgt_type) : NULL;
            Type *src_eff = src_type ? type_unwrap_distinct(src_type) : NULL;

            uint32_t src_tid = 0;
            if (src_eff && src_eff->kind == TYPE_POINTER && src_eff->pointer.inner) {
                Type *inner = type_unwrap_distinct(src_eff->pointer.inner);
                src_tid = opaque_type_id_nominal(inner);   /* @pun: nominal ids only (BUG-1166) */
            }

            uint32_t tgt_tid = 0;
            if (tgt_eff && tgt_eff->kind == TYPE_POINTER && tgt_eff->pointer.inner) {
                Type *inner = type_unwrap_distinct(tgt_eff->pointer.inner);
                tgt_tid = opaque_type_id_nominal(inner);   /* @pun: nominal ids only (BUG-1166) */
            }

            bool src_is_opaque = (src_eff &&
                ((src_eff->kind == TYPE_POINTER && src_eff->pointer.inner &&
                  type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_OPAQUE) ||
                 src_eff->kind == TYPE_OPAQUE));

            if (src_is_opaque) {
                /* @pun on already-opaque source — FROM-*opaque check only */
                if (tgt_tid > 0) {
                    int tmp = e->temp_count++;
                    emit(e, "({ _zer_opaque _zer_pn%d = ", tmp);
                    if (node->intrinsic.arg_count > 0)
                        emit_rewritten_node(e, node->intrinsic.args[0], func);
                    emit(e, "; if (_zer_pn%d.type_id != %u && _zer_pn%d.type_id != 0) "
                         "_zer_trap(\"@pun type mismatch\", __FILE__, __LINE__); (",
                         tmp, (unsigned)tgt_tid, tmp);
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")_zer_pn%d.ptr; })", tmp);
                } else {
                    emit(e, "((");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")(");
                    if (node->intrinsic.arg_count > 0)
                        emit_rewritten_node(e, node->intrinsic.args[0], func);
                    emit(e, ").ptr)");
                }
            } else {
                /* @pun on raw typed pointer — full wrap+check inline */
                if (tgt_tid > 0) {
                    int tmp = e->temp_count++;
                    emit(e, "({ _zer_opaque _zer_pn%d = (_zer_opaque){(void*)(", tmp);
                    if (node->intrinsic.arg_count > 0)
                        emit_rewritten_node(e, node->intrinsic.args[0], func);
                    emit(e, "), %u}; if (_zer_pn%d.type_id != %u && _zer_pn%d.type_id != 0) "
                         "_zer_trap(\"@pun type mismatch\", __FILE__, __LINE__); (",
                         (unsigned)src_tid, tmp, (unsigned)tgt_tid, tmp);
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")_zer_pn%d.ptr; })", tmp);
                } else {
                    /* target has no type_id (primitive *T like *u8) — plain cast */
                    emit(e, "((");
                    if (tgt_type) emit_type(e, tgt_type);
                    emit(e, ")(");
                    if (node->intrinsic.arg_count > 0)
                        emit_rewritten_node(e, node->intrinsic.args[0], func);
                    emit(e, "))");
                }
            }
        } else if (nlen == 8 && memcmp(name, "inttoptr", 8) == 0) {
            /* @inttoptr(*T, addr) — integer to pointer.
             * Phase 3 fix #6+#7: port mmio range + alignment check from
             * AST emit_expr line 2631-2680. Constant addresses are
             * validated at compile time by the checker. Variable addresses
             * need runtime range check (must fall in declared mmio range)
             * and runtime alignment check (must match target type). */
            emit_inttoptr(e, node, func);   /* BUG-1058: one emission, both paths */
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
        } else if (nlen == 7 && memcmp(name, "barrier", 7) == 0) {
            emit(e, "__atomic_thread_fence(__ATOMIC_SEQ_CST)");
        } else if (nlen == 13 && memcmp(name, "barrier_store", 13) == 0) {
            emit(e, "__atomic_thread_fence(__ATOMIC_RELEASE)");
        } else if (nlen == 12 && memcmp(name, "barrier_load", 12) == 0) {
            emit(e, "__atomic_thread_fence(__ATOMIC_ACQUIRE)");
        } else if (nlen == 15 && memcmp(name, "barrier_acq_rel", 15) == 0) {
            /* D-Alpha-2: acquire + release fence */
            emit(e, "__atomic_thread_fence(__ATOMIC_ACQ_REL)");
        } else if (nlen == 13 && memcmp(name, "tlb_flush_all", 13) == 0) {
            /* D-Alpha-6: flush all TLB entries. Privileged. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    { uint64_t _zer_cr3; __asm__ __volatile__ (\"mov %%%%cr3, %%0; mov %%0, %%%%cr3\" : \"=r\"(_zer_cr3) :: \"memory\"); }\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"tlbi vmalle1\\n\\tdsb ish\\n\\tisb\" ::: \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"sfence.vma x0, x0\" ::: \"memory\");\n"
                "#else\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 16 && memcmp(name, "tlb_flush_global", 16) == 0) {
            /* D-Alpha-6: flush global (non-ASID) TLB entries. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    { uint64_t _zer_cr4; __asm__ __volatile__ (\n"
                "        \"mov %%%%cr4, %%0\\n\\t\"\n"
                "        \"btr $7, %%0\\n\\tmov %%0, %%%%cr4\\n\\t\"\n"
                "        \"bts $7, %%0\\n\\tmov %%0, %%%%cr4\"\n"
                "        : \"=r\"(_zer_cr4) :: \"memory\"); }\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"tlbi vmalle1is\\n\\tdsb ish\\n\\tisb\" ::: \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"sfence.vma x0, x0\" ::: \"memory\");\n"
                "#else\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 14 && memcmp(name, "tlb_flush_asid", 14) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-6: flush TLB entries matching ASID. */
            emit(e, "({ uint64_t _zer_asid = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    { uint64_t _zer_cr3; __asm__ __volatile__ (\"mov %%%%cr3, %%0; mov %%0, %%%%cr3\" : \"=r\"(_zer_cr3) :: \"memory\"); (void)_zer_asid; }\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"tlbi aside1, %%0\\n\\tdsb ish\\n\\tisb\" : : \"r\"(_zer_asid << 48) : \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"sfence.vma x0, %%0\" : : \"r\"(_zer_asid) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_asid; __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 14 && memcmp(name, "tlb_flush_addr", 14) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-6: flush single-page TLB entry for given virtual address. */
            emit(e, "({ uint64_t _zer_va = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"invlpg (%%0)\" : : \"r\"(_zer_va) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"tlbi vaae1, %%0\\n\\tdsb ish\\n\\tisb\" : : \"r\"(_zer_va >> 12) : \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"sfence.vma %%0, x0\" : : \"r\"(_zer_va) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_va; __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 15 && memcmp(name, "tlb_flush_range", 15) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-6: flush TLB entries over [start, end) — loops page-by-page. */
            emit(e, "({ uint64_t _zer_start = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_end = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "    for (uint64_t _zer_p = _zer_start & ~0xFFFULL; _zer_p < _zer_end; _zer_p += 0x1000ULL) {\n"
                "#if defined(__x86_64__)\n"
                "        __asm__ __volatile__ (\"invlpg (%%0)\" : : \"r\"(_zer_p) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "        __asm__ __volatile__ (\"tlbi vaae1, %%0\" : : \"r\"(_zer_p >> 12) : \"memory\");\n"
                "#elif defined(__riscv)\n"
                "        __asm__ __volatile__ (\"sfence.vma %%0, x0\" : : \"r\"(_zer_p) : \"memory\");\n"
                "#endif\n"
                "    }\n"
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"dsb ish\\n\\tisb\" ::: \"memory\");\n"
                "#endif\n"
                "})");
        } else if ((nlen == 17 && memcmp(name, "cache_flush_range", 17) == 0) &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-6: write-back + invalidate data cache range. */
            emit(e, "({ const uint8_t *_zer_ca = (const uint8_t*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uintptr_t _zer_cl = (uintptr_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "    for (uintptr_t _zer_i = 0; _zer_i < _zer_cl; _zer_i += 64) {\n"
                "#if defined(__x86_64__)\n"
                "        __asm__ __volatile__ (\"clflush (%%0)\" : : \"r\"(_zer_ca + _zer_i) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "        __asm__ __volatile__ (\"dc civac, %%0\" : : \"r\"(_zer_ca + _zer_i) : \"memory\");\n"
                "#endif\n"
                "    }\n"
                /* G4 (2026-08-01): the arch chain was `#if x86 / #elif aarch64 /
                 * #endif` with NO fallback, so on RISC-V and ARM32 the loop body
                 * was EMPTY — a SILENT no-op. For an invalidate before a DMA read
                 * that means the CPU keeps serving stale cached data with no
                 * diagnostic anywhere. Emit a full fence on every other arch so
                 * ordering is at least preserved and the op is not silent.
                 *
                 * Placed AFTER the loop, not inside it (the source branch put it
                 * in the loop body): one fence per CALL rather than one per
                 * 64-byte line — same ordering guarantee, O(1) instead of
                 * O(range/64). The now-empty loop is elided by GCC.
                 *
                 * Actual cache maintenance on those arches stays a
                 * HARDWARE-consequence floor (RISC-V needs the Zicbom extension);
                 * ZER does not claim to perform it, it just refuses to pretend
                 * silently that it did. */
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"dsb ish\" ::: \"memory\");\n"
                "#elif !defined(__x86_64__)\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if ((nlen == 17 && memcmp(name, "cache_clean_range", 17) == 0) &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-6: write-back without invalidate. */
            emit(e, "({ const uint8_t *_zer_ca = (const uint8_t*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uintptr_t _zer_cl = (uintptr_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "    for (uintptr_t _zer_i = 0; _zer_i < _zer_cl; _zer_i += 64) {\n"
                "#if defined(__x86_64__)\n"
                "        __asm__ __volatile__ (\"clwb (%%0)\" : : \"r\"(_zer_ca + _zer_i) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "        __asm__ __volatile__ (\"dc cvac, %%0\" : : \"r\"(_zer_ca + _zer_i) : \"memory\");\n"
                "#endif\n"
                "    }\n"
                /* G4 (2026-08-01): the arch chain was `#if x86 / #elif aarch64 /
                 * #endif` with NO fallback, so on RISC-V and ARM32 the loop body
                 * was EMPTY — a SILENT no-op. For an invalidate before a DMA read
                 * that means the CPU keeps serving stale cached data with no
                 * diagnostic anywhere. Emit a full fence on every other arch so
                 * ordering is at least preserved and the op is not silent.
                 *
                 * Placed AFTER the loop, not inside it (the source branch put it
                 * in the loop body): one fence per CALL rather than one per
                 * 64-byte line — same ordering guarantee, O(1) instead of
                 * O(range/64). The now-empty loop is elided by GCC.
                 *
                 * Actual cache maintenance on those arches stays a
                 * HARDWARE-consequence floor (RISC-V needs the Zicbom extension);
                 * ZER does not claim to perform it, it just refuses to pretend
                 * silently that it did. */
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"dsb ish\" ::: \"memory\");\n"
                "#elif !defined(__x86_64__)\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if ((nlen == 22 && memcmp(name, "cache_invalidate_range", 22) == 0) &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-6: invalidate data cache range (ARM: dc ivac; x86 uses clflush since no pure invalidate). */
            emit(e, "({ const uint8_t *_zer_ca = (const uint8_t*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uintptr_t _zer_cl = (uintptr_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "    for (uintptr_t _zer_i = 0; _zer_i < _zer_cl; _zer_i += 64) {\n"
                "#if defined(__x86_64__)\n"
                "        __asm__ __volatile__ (\"clflush (%%0)\" : : \"r\"(_zer_ca + _zer_i) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "        __asm__ __volatile__ (\"dc ivac, %%0\" : : \"r\"(_zer_ca + _zer_i) : \"memory\");\n"
                "#endif\n"
                "    }\n"
                /* G4 (2026-08-01): the arch chain was `#if x86 / #elif aarch64 /
                 * #endif` with NO fallback, so on RISC-V and ARM32 the loop body
                 * was EMPTY — a SILENT no-op. For an invalidate before a DMA read
                 * that means the CPU keeps serving stale cached data with no
                 * diagnostic anywhere. Emit a full fence on every other arch so
                 * ordering is at least preserved and the op is not silent.
                 *
                 * Placed AFTER the loop, not inside it (the source branch put it
                 * in the loop body): one fence per CALL rather than one per
                 * 64-byte line — same ordering guarantee, O(1) instead of
                 * O(range/64). The now-empty loop is elided by GCC.
                 *
                 * Actual cache maintenance on those arches stays a
                 * HARDWARE-consequence floor (RISC-V needs the Zicbom extension);
                 * ZER does not claim to perform it, it just refuses to pretend
                 * silently that it did. */
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"dsb ish\" ::: \"memory\");\n"
                "#elif !defined(__x86_64__)\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if ((nlen == 23 && memcmp(name, "cache_invalidate_icache", 23) == 0) &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-6: instruction cache invalidate — uses portable GCC builtin. */
            emit(e, "({ char *_zer_ib = (char*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uintptr_t _zer_il = (uintptr_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "    __builtin___clear_cache(_zer_ib, _zer_ib + _zer_il);\n"
                "})");
        } else if ((nlen == 16 && memcmp(name, "cache_flush_line", 16) == 0) &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-6: single cache line write-back + invalidate. */
            emit(e, "({ const void *_zer_cp = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"clflush (%%0)\" : : \"r\"(_zer_cp) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"dc civac, %%0\\n\\tdsb ish\" : : \"r\"(_zer_cp) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_cp; __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if ((nlen == 15 && memcmp(name, "cache_zero_line", 15) == 0) &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-6: zero a cache line without read-modify-write. Big perf win for memset. */
            emit(e, "({ void *_zer_zp = (void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"dc zva, %%0\" : : \"r\"(_zer_zp) : \"memory\");\n"
                "#else\n"
                "    /* Fallback: plain memset for one cache line (assume 64 bytes) */\n"
                "    for (int _zer_zi = 0; _zer_zi < 64; _zer_zi++) { ((uint8_t*)_zer_zp)[_zer_zi] = 0; }\n"
                "#endif\n"
                "})");
        } else if (nlen == 11 && memcmp(name, "barrier_dma", 11) == 0) {
            /* D-Alpha-7: DMA-coherence barrier — stronger than release fence.
             * Required for correct DMA on weakly-ordered archs (ARM, RISC-V). */
            emit(e, "({\n"
                "#if defined(__x86_64__) || defined(__i386__)\n"
                "    __asm__ __volatile__ (\"mfence\" ::: \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"dsb ishst\" ::: \"memory\");\n"
                "#elif defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\"dmb st\" ::: \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"fence iorw, iorw\" ::: \"memory\");\n"
                "#else\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 9 && memcmp(name, "cpu_pause", 9) == 0) {
            /* D-Alpha-7: spin-wait hint (relaxes CPU, saves power, avoids starving other hyperthread) */
            emit(e, "({\n"
                "#if defined(__x86_64__) || defined(__i386__)\n"
                "    __asm__ __volatile__ (\"pause\");\n"
                "#elif defined(__aarch64__) || defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\"yield\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\".insn i 0x0F, 0, x0, x0, 0x010\");  /* Zihintpause pause hint */\n"
                "#else\n"
                "    __asm__ __volatile__ (\"\" ::: \"memory\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 6 && memcmp(name, "cpu_id", 6) == 0) {
            /* D-Alpha-7: current CPU/core number. Returns u32.
             * Note: on most archs this requires reading a privileged system register
             * (mpidr_el1 on ARM, mhartid on RISC-V). On x86 user-mode we approximate via
             * a sched_getcpu-like fallback. For kernel mode, replace with proper syscall. */
            emit(e, "({ uint32_t _zer_cpu = 0;\n"
                "#if defined(__aarch64__)\n"
                "    { uint64_t _zer_mpidr; __asm__ __volatile__ (\"mrs %%0, mpidr_el1\" : \"=r\"(_zer_mpidr)); _zer_cpu = (uint32_t)(_zer_mpidr & 0xFF); }\n"
                "#elif defined(__riscv) && defined(__riscv_xlen) && __riscv_xlen >= 64\n"
                "    { unsigned long _zer_hart; __asm__ __volatile__ (\"csrr %%0, mhartid\" : \"=r\"(_zer_hart)); _zer_cpu = (uint32_t)_zer_hart; }\n"
                "#elif defined(__x86_64__) || defined(__i386__)\n"
                "    { uint32_t _zer_a, _zer_b; __asm__ __volatile__ (\"rdtscp\" : \"=a\"(_zer_a), \"=c\"(_zer_b) :: \"rdx\"); _zer_cpu = _zer_b & 0xFFF; }\n"
                "#endif\n"
                "_zer_cpu; })");
        } else if (nlen == 7 && memcmp(name, "cpu_wfe", 7) == 0) {
            /* D-Alpha-7: wait-for-event (paired with sev). On x86 falls back to pause. */
            emit(e, "({\n"
                "#if defined(__aarch64__) || defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\"wfe\" ::: \"memory\");\n"
                "#elif defined(__x86_64__) || defined(__i386__)\n"
                "    __asm__ __volatile__ (\"pause\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\".insn i 0x0F, 0, x0, x0, 0x010\");\n"
                "#else\n"
                "    __asm__ __volatile__ (\"\" ::: \"memory\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 7 && memcmp(name, "cpu_sev", 7) == 0) {
            /* D-Alpha-7: send-event (wakes WFE waiters). On non-ARM: memory fence as fallback. */
            emit(e, "({\n"
                "#if defined(__aarch64__) || defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\"sev\" ::: \"memory\");\n"
                "#else\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 14 && memcmp(name, "cpu_breakpoint", 14) == 0) {
            /* D-Alpha-7: debug trap for attach-debugger workflows */
            emit(e, "({\n"
                "#if defined(__x86_64__) || defined(__i386__)\n"
                "    __asm__ __volatile__ (\"int3\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"brk #0\");\n"
                "#elif defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\"bkpt #0\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"ebreak\");\n"
                "#else\n"
                "    __builtin_trap();\n"
                "#endif\n"
                "})");
        } else if (nlen == 16 && memcmp(name, "cpu_read_counter", 16) == 0) {
            /* D-Alpha-8: read cycle/time counter (u64). Non-privileged where available. */
            emit(e, "({ uint64_t _zer_cc = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_lo, _zer_hi;\n"
                "    __asm__ __volatile__ (\"rdtsc\" : \"=a\"(_zer_lo), \"=d\"(_zer_hi));\n"
                "    _zer_cc = ((uint64_t)_zer_hi << 32) | _zer_lo;\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mrs %%0, cntvct_el0\" : \"=r\"(_zer_cc));\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"rdtime %%0\" : \"=r\"(_zer_cc));\n"
                "#else\n"
                "    /* Fallback: zero counter */\n"
                "#endif\n"
                "_zer_cc; })");
        } else if (nlen == 10 && memcmp(name, "cpu_get_pc", 10) == 0) {
            /* D-Alpha-8: read current instruction pointer (u64). Useful for profiling. */
            emit(e, "({ uint64_t _zer_pc = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"leaq 0(%%%%rip), %%0\" : \"=r\"(_zer_pc));\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"adr %%0, .\" : \"=r\"(_zer_pc));\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"auipc %%0, 0\" : \"=r\"(_zer_pc));\n"
                "#endif\n"
                "_zer_pc; })");
        } else if (nlen == 15 && memcmp(name, "wait_on_address", 15) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-8: efficient polling — spin with pause hint until *addr != expected.
             * Not a true wait (no futex-style blocking); just avoids hammering bus. */
            emit(e, "({ volatile uint32_t *_zer_wa = (volatile uint32_t*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint32_t _zer_we = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "    while (__atomic_load_n(_zer_wa, __ATOMIC_ACQUIRE) == _zer_we) {\n"
                "#if defined(__x86_64__)\n"
                "        __asm__ __volatile__ (\"pause\");\n"
                "#elif defined(__aarch64__)\n"
                "        __asm__ __volatile__ (\"yield\");\n"
                "#elif defined(__riscv)\n"
                "        __asm__ __volatile__ (\".insn i 0x0F, 0, x0, x0, 0x010\");  /* pause hint (Zihintpause) */\n"
                "#endif\n"
                "    }\n"
                "})");
        } else if (nlen == 18 && memcmp(name, "cpu_flush_pipeline", 18) == 0) {
            /* D-Alpha-8: flush instruction pipeline. Required after modifying executable code,
             * updating system registers that affect subsequent fetches, etc. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"mfence\\n\\tlfence\" ::: \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"isb\" ::: \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"fence.i\" ::: \"memory\");\n"
                "#else\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_read_msr", 12) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-9: RDMSR — x86 only, reads Model-Specific Register. Privileged. */
            emit(e, "({ uint64_t _zer_msr = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_lo, _zer_hi, _zer_idx = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "    __asm__ __volatile__ (\"rdmsr\" : \"=a\"(_zer_lo), \"=d\"(_zer_hi) : \"c\"(_zer_idx));\n"
                "    _zer_msr = ((uint64_t)_zer_hi << 32) | _zer_lo;\n"
                "#endif\n"
                "_zer_msr; })");
        } else if (nlen == 13 && memcmp(name, "cpu_write_msr", 13) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-9: WRMSR — x86 only, writes Model-Specific Register. Privileged. */
            emit(e, "({ uint32_t _zer_mi = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_mv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_lo = (uint32_t)_zer_mv, _zer_hi = (uint32_t)(_zer_mv >> 32);\n"
                "    __asm__ __volatile__ (\"wrmsr\" :: \"a\"(_zer_lo), \"d\"(_zer_hi), \"c\"(_zer_mi));\n"
                "#else\n"
                "    (void)_zer_mi; (void)_zer_mv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_read_cr0", 12) == 0) {
            emit(e, "({ uint64_t _zer_cr = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movq %%%%cr0, %%0\" : \"=r\"(_zer_cr));\n"
                "#endif\n"
                "_zer_cr; })");
        } else if (nlen == 13 && memcmp(name, "cpu_write_cr0", 13) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            emit(e, "({ uint64_t _zer_cv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movq %%0, %%%%cr0\" :: \"r\"(_zer_cv) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_cv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_read_cr3", 12) == 0) {
            emit(e, "({ uint64_t _zer_cr = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movq %%%%cr3, %%0\" : \"=r\"(_zer_cr));\n"
                "#endif\n"
                "_zer_cr; })");
        } else if (nlen == 13 && memcmp(name, "cpu_write_cr3", 13) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            emit(e, "({ uint64_t _zer_cv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movq %%0, %%%%cr3\" :: \"r\"(_zer_cv) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_cv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_read_cr4", 12) == 0) {
            emit(e, "({ uint64_t _zer_cr = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movq %%%%cr4, %%0\" : \"=r\"(_zer_cr));\n"
                "#endif\n"
                "_zer_cr; })");
        } else if (nlen == 13 && memcmp(name, "cpu_write_cr4", 13) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            emit(e, "({ uint64_t _zer_cv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movq %%0, %%%%cr4\" :: \"r\"(_zer_cv) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_cv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 13 && memcmp(name, "cpu_read_xcr0", 13) == 0) {
            /* D-Alpha-9: XGETBV — reads extended control register 0 (XSAVE feature mask).
             * Not privileged but requires XSAVE feature support. */
            emit(e, "({ uint64_t _zer_xc = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_lo, _zer_hi;\n"
                "    __asm__ __volatile__ (\"xgetbv\" : \"=a\"(_zer_lo), \"=d\"(_zer_hi) : \"c\"(0));\n"
                "    _zer_xc = ((uint64_t)_zer_hi << 32) | _zer_lo;\n"
                "#endif\n"
                "_zer_xc; })");
        } else if (nlen == 14 && memcmp(name, "cpu_write_xcr0", 14) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-9: XSETBV — writes XCR0. Privileged (CPL=0) on most hardware. */
            emit(e, "({ uint64_t _zer_xv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_lo = (uint32_t)_zer_xv, _zer_hi = (uint32_t)(_zer_xv >> 32);\n"
                "    __asm__ __volatile__ (\"xsetbv\" :: \"a\"(_zer_lo), \"d\"(_zer_hi), \"c\"(0));\n"
                "#else\n"
                "    (void)_zer_xv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 11 && memcmp(name, "cpu_read_sp", 11) == 0) {
            /* D-Alpha-10: read stack pointer (u64). Non-privileged. */
            emit(e, "({ uint64_t _zer_sp = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movq %%%%rsp, %%0\" : \"=r\"(_zer_sp));\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mov %%0, sp\" : \"=r\"(_zer_sp));\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"mv %%0, sp\" : \"=r\"(_zer_sp));\n"
                "#endif\n"
                "_zer_sp; })");
        } else if (nlen == 11 && memcmp(name, "cpu_read_tp", 11) == 0) {
            /* D-Alpha-10: read thread pointer / TLS base (u64). Non-privileged.
             * Uses GCC builtin which handles arch-specific register. */
            emit(e, "((uint64_t)(uintptr_t)__builtin_thread_pointer())");
        } else if (nlen == 14 && memcmp(name, "cpu_read_flags", 14) == 0) {
            /* D-Alpha-10: read flags/status register (u64). Non-privileged on user bits. */
            emit(e, "({ uint64_t _zer_fl = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"pushfq\\n\\tpopq %%0\" : \"=r\"(_zer_fl) :: \"cc\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mrs %%0, nzcv\" : \"=r\"(_zer_fl));\n"
                "#endif\n"
                "_zer_fl; })");
        } else if (nlen == 13 && memcmp(name, "cpu_vendor_id", 13) == 0) {
            /* D-Alpha-10: read vendor ID (first 4 chars packed into u64).
             * x86: CPUID leaf 0 returns \"GenuineIntel\" / \"AuthenticAMD\" in EBX:EDX:ECX.
             * Just returns EBX as u64 (low 4 chars of vendor string). */
            emit(e, "({ uint64_t _zer_v = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_a, _zer_b, _zer_c, _zer_d;\n"
                "    __asm__ __volatile__ (\"cpuid\"\n"
                "        : \"=a\"(_zer_a), \"=b\"(_zer_b), \"=c\"(_zer_c), \"=d\"(_zer_d)\n"
                "        : \"a\"(0));\n"
                "    _zer_v = (uint64_t)_zer_b;\n"
                "#endif\n"
                "_zer_v; })");
        } else if (nlen == 16 && memcmp(name, "cpu_feature_bits", 16) == 0) {
            /* D-Alpha-10: basic feature bits (CPUID leaf 1 EDX on x86). Non-privileged. */
            emit(e, "({ uint64_t _zer_fb = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_a, _zer_b, _zer_c, _zer_d;\n"
                "    __asm__ __volatile__ (\"cpuid\"\n"
                "        : \"=a\"(_zer_a), \"=b\"(_zer_b), \"=c\"(_zer_c), \"=d\"(_zer_d)\n"
                "        : \"a\"(1));\n"
                "    _zer_fb = ((uint64_t)_zer_c << 32) | _zer_d;\n"
                "#endif\n"
                "_zer_fb; })");
        } else if (nlen == 12 && memcmp(name, "cpu_model_id", 12) == 0) {
            /* D-Alpha-10: model ID (CPUID leaf 1 EAX on x86 — family/model/stepping). */
            emit(e, "({ uint32_t _zer_m = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_a, _zer_b, _zer_c, _zer_d;\n"
                "    __asm__ __volatile__ (\"cpuid\"\n"
                "        : \"=a\"(_zer_a), \"=b\"(_zer_b), \"=c\"(_zer_c), \"=d\"(_zer_d)\n"
                "        : \"a\"(1));\n"
                "    _zer_m = _zer_a;\n"
                "#endif\n"
                "_zer_m; })");
        } else if (nlen == 11 && memcmp(name, "cpu_core_id", 11) == 0) {
            /* D-Alpha-10: physical core ID. Complex on modern SMT CPUs;
             * stub to 0 for user-mode (most users want logical cpu_id instead). */
            emit(e, "((uint32_t)0)");
        } else if (nlen == 16 && memcmp(name, "cpu_current_mode", 16) == 0) {
            /* D-Alpha-10: current privilege mode. 0=user, 1=kernel, 2=hypervisor, 3=monitor.
             * User-mode code always sees 0. Privileged code can read CS.RPL or equivalent. */
            emit(e, "((uint32_t)0)");
        } else if (nlen == 19 && memcmp(name, "cpu_cache_line_size", 19) == 0) {
            /* D-Alpha-10: L1 data cache line size in bytes.
             * x86/ARM64/RISC-V default is 64 bytes. Users needing exact runtime
             * value should call POSIX sysconf(_SC_LEVEL1_DCACHE_LINESIZE) directly. */
            emit(e, "((uint32_t)64)");
        } else if (nlen == 13 && memcmp(name, "cpu_num_cores", 13) == 0) {
            /* D-Alpha-10: number of logical cores — stub to 1.
             * Users needing runtime value should call POSIX sysconf(_SC_NPROCESSORS_ONLN)
             * or std::thread::hardware_concurrency equivalent directly. */
            emit(e, "((uint32_t)1)");
        } else if (nlen == 9 && memcmp(name, "cpu_reset", 9) == 0) {
            /* D-Alpha-11: trigger reset. Real reset is platform-specific (PSCI/SBI/
             * keyboard controller port); safe portable fallback = infinite halt loop.
             * Users on specific platforms override with platform-specific code. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"cli\\n\\t1: hlt\\n\\tjmp 1b\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"1: wfi\\n\\tb 1b\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"1: wfi\\n\\tj 1b\");\n"
                "#else\n"
                "    while (1) { __builtin_trap(); }\n"
                "#endif\n"
                "})");
        } else if (nlen == 14 && memcmp(name, "cpu_deep_sleep", 14) == 0) {
            /* D-Alpha-11: enter deepest idle. Real C-state entry requires
             * platform-specific firmware calls (PSCI_CPU_SUSPEND etc.);
             * simplest safe fallback is WFI (wait-for-interrupt). Privileged. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"hlt\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"wfi\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"wfi\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 13 && memcmp(name, "cpu_idle_hint", 13) == 0) {
            /* D-Alpha-11: softer idle hint — tells CPU we have nothing
             * urgent to do. On x86 uses PAUSE (short delay, power save).
             * Non-blocking (unlike cpu_wait_int/cpu_deep_sleep). */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"pause\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"yield\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\".insn i 0x0F, 0, x0, x0, 0x010\");  /* Zihintpause */\n"
                "#endif\n"
                "})");
        } else if (nlen == 16 && memcmp(name, "cpu_monitor_addr", 16) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-11: x86 MONITOR — set up address watch for MWAIT.
             * Privileged on some CPUs. ARM64/RISC-V: no direct equivalent (LDXR
             * sets exclusive monitor implicitly). */
            emit(e, "({ const void *_zer_maddr = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"monitor\" :: \"a\"(_zer_maddr), \"c\"(0), \"d\"(0));\n"
                "#else\n"
                "    (void)_zer_maddr;\n"
                "#endif\n"
                "})");
        } else if (nlen == 9 && memcmp(name, "cpu_mwait", 9) == 0) {
            /* D-Alpha-11: x86 MWAIT — wait until monitored address modified
             * OR interrupt arrives. Must be preceded by MONITOR setup.
             * Privileged on some CPUs. ARM64: WFE substitutes. RISC-V: WFI. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"mwait\" :: \"a\"(0), \"c\"(0));\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"wfe\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"wfi\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 11 && memcmp(name, "cpu_syscall", 11) == 0) {
            /* D-Alpha-12: issue syscall (user-side trap to kernel).
             * x86: syscall — fast syscall via MSR_LSTAR target
             * ARM64: svc #0 — supervisor call
             * RISC-V: ecall — environment call from U-mode to S/M */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"syscall\" ::: \"rcx\", \"r11\", \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"svc #0\" ::: \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"ecall\" ::: \"memory\");\n"
                "#else\n"
                "#error \"@cpu_syscall: no implementation for target architecture\"\n"
                "#endif\n"
                "})");
        } else if (nlen == 10 && memcmp(name, "cpu_sysret", 10) == 0) {
            /* D-Alpha-12: return from syscall (kernel->user transition).
             * Requires correctly-set return context (CS/RIP/RFLAGS/RSP/SS on x86).
             * x86: sysretq — fast return counterpart to syscall
             * ARM64: eret — return from exception using ELR/SPSR
             * RISC-V: sret — return from supervisor mode */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"sysretq\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"eret\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"sret\");\n"
                "#else\n"
                "#error \"@cpu_sysret: no implementation for target architecture\"\n"
                "#endif\n"
                "})");
        } else if (nlen == 8 && memcmp(name, "cpu_iret", 8) == 0) {
            /* D-Alpha-12: return from interrupt handler.
             * x86: iretq — restores CS/RIP/RFLAGS/RSP/SS from interrupt stack
             * ARM64: eret — same instruction as sysret (arch-unified)
             * RISC-V: mret — return from machine mode */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"iretq\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"eret\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"mret\");\n"
                "#else\n"
                "#error \"@cpu_iret: no implementation for target architecture\"\n"
                "#endif\n"
                "})");
        } else if (nlen == 18 && memcmp(name, "cpu_set_priv_stack", 18) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-12: set privileged-mode stack pointer for syscall entry.
             * x86: writes MSR_KERNEL_GS_BASE as target for swapgs+load convention.
             * ARM64: sets SP_EL1 for kernel stack.
             * RISC-V: writes mscratch (machine mode) or sscratch (supervisor). */
            emit(e, "({ uint64_t _zer_sp = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_lo = (uint32_t)_zer_sp, _zer_hi = (uint32_t)(_zer_sp >> 32);\n"
                "    /* MSR_KERNEL_GS_BASE = 0xC0000102 */\n"
                "    __asm__ __volatile__ (\"wrmsr\" :: \"a\"(_zer_lo), \"d\"(_zer_hi), \"c\"(0xC0000102));\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"msr sp_el0, %%0\" :: \"r\"(_zer_sp));\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrw mscratch, %%0\" :: \"r\"(_zer_sp));\n"
                "#else\n"
                "    (void)_zer_sp;\n"
                "#endif\n"
                "})");
        } else if (nlen == 18 && memcmp(name, "cpu_get_priv_level", 18) == 0) {
            /* D-Alpha-12: query current privilege level.
             * Returns 0=user, higher=more privileged.
             * x86: CS.RPL (low 2 bits of CS segment)
             * ARM64: CurrentEL >> 2
             * RISC-V: no direct user-mode query — returns 0 (privileged needs mstatus) */
            emit(e, "({ uint32_t _zer_pl = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint16_t _zer_cs;\n"
                "    __asm__ __volatile__ (\"movw %%%%cs, %%0\" : \"=r\"(_zer_cs));\n"
                "    _zer_pl = (uint32_t)(_zer_cs & 0x3);\n"
                "#elif defined(__aarch64__)\n"
                "    uint64_t _zer_el;\n"
                "    __asm__ __volatile__ (\"mrs %%0, CurrentEL\" : \"=r\"(_zer_el));\n"
                "    _zer_pl = (uint32_t)(_zer_el >> 2);\n"
                "#endif\n"
                "_zer_pl; })");
        } else if (nlen == 13 && memcmp(name, "cpu_hypercall", 13) == 0) {
            /* D-Alpha-12: invoke hypervisor (for code running as a guest). */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"vmcall\" ::: \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"hvc #0\" ::: \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"ecall\" ::: \"memory\");\n"
                "#else\n"
                "#error \"@cpu_hypercall: no implementation for target architecture\"\n"
                "#endif\n"
                "})");
        } else if (nlen == 15 && memcmp(name, "cpu_read_fsbase", 15) == 0) {
            emit(e, "({ uint64_t _zer_fs = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"rdfsbase %%0\" : \"=r\"(_zer_fs));\n"
                "#endif\n"
                "_zer_fs; })");
        } else if (nlen == 15 && memcmp(name, "cpu_read_gsbase", 15) == 0) {
            emit(e, "({ uint64_t _zer_gs = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"rdgsbase %%0\" : \"=r\"(_zer_gs));\n"
                "#endif\n"
                "_zer_gs; })");
        } else if (nlen == 16 && memcmp(name, "cpu_write_fsbase", 16) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            emit(e, "({ uint64_t _zer_fv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"wrfsbase %%0\" :: \"r\"(_zer_fv));\n"
                "#else\n"
                "    (void)_zer_fv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 16 && memcmp(name, "cpu_write_gsbase", 16) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            emit(e, "({ uint64_t _zer_gv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"wrgsbase %%0\" :: \"r\"(_zer_gv));\n"
                "#else\n"
                "    (void)_zer_gv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 8 && memcmp(name, "port_in8", 8) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-13: x86 inb — read byte from I/O port */
            emit(e, "({ uint16_t _zer_pp = (uint16_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint8_t _zer_pv = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"inb %%1, %%0\" : \"=a\"(_zer_pv) : \"Nd\"(_zer_pp));\n"
                "#endif\n"
                "_zer_pv; })");
        } else if (nlen == 9 && memcmp(name, "port_in16", 9) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-13: x86 inw — read word from I/O port */
            emit(e, "({ uint16_t _zer_pp = (uint16_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint16_t _zer_pv = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"inw %%1, %%0\" : \"=a\"(_zer_pv) : \"Nd\"(_zer_pp));\n"
                "#endif\n"
                "_zer_pv; })");
        } else if (nlen == 9 && memcmp(name, "port_in32", 9) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-13: x86 inl — read dword from I/O port */
            emit(e, "({ uint16_t _zer_pp = (uint16_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint32_t _zer_pv = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"inl %%1, %%0\" : \"=a\"(_zer_pv) : \"Nd\"(_zer_pp));\n"
                "#endif\n"
                "_zer_pv; })");
        } else if (nlen == 9 && memcmp(name, "port_out8", 9) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-13: x86 outb — write byte to I/O port */
            emit(e, "({ uint16_t _zer_pp = (uint16_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint8_t _zer_pv = (uint8_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"outb %%0, %%1\" :: \"a\"(_zer_pv), \"Nd\"(_zer_pp));\n"
                "#else\n"
                "    (void)_zer_pp; (void)_zer_pv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 10 && memcmp(name, "port_out16", 10) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            emit(e, "({ uint16_t _zer_pp = (uint16_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint16_t _zer_pv = (uint16_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"outw %%0, %%1\" :: \"a\"(_zer_pv), \"Nd\"(_zer_pp));\n"
                "#else\n"
                "    (void)_zer_pp; (void)_zer_pv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 10 && memcmp(name, "port_out32", 10) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            emit(e, "({ uint16_t _zer_pp = (uint16_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint32_t _zer_pv = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"outl %%0, %%1\" :: \"a\"(_zer_pv), \"Nd\"(_zer_pp));\n"
                "#else\n"
                "    (void)_zer_pp; (void)_zer_pv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 9 && memcmp(name, "cpu_xsave", 9) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-13: XSAVE — save extended processor state (AVX/AVX-512 regs). */
            emit(e, "({ void *_zer_xb = (void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_xm = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_lo = (uint32_t)_zer_xm, _zer_hi = (uint32_t)(_zer_xm >> 32);\n"
                "    __asm__ __volatile__ (\"xsave (%%0)\" :: \"r\"(_zer_xb), \"a\"(_zer_lo), \"d\"(_zer_hi) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_xb; (void)_zer_xm;\n"
                "#endif\n"
                "})");
        } else if (nlen == 10 && memcmp(name, "cpu_xrstor", 10) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-13: XRSTOR — restore extended processor state. */
            emit(e, "({ const void *_zer_xb = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_xm = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_lo = (uint32_t)_zer_xm, _zer_hi = (uint32_t)(_zer_xm >> 32);\n"
                "    __asm__ __volatile__ (\"xrstor (%%0)\" :: \"r\"(_zer_xb), \"a\"(_zer_lo), \"d\"(_zer_hi) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_xb; (void)_zer_xm;\n"
                "#endif\n"
                "})");
        } else if (nlen == 11 && memcmp(name, "cpu_read_dr", 11) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-13: read debug register (DR0-DR7 on x86). Switch on idx. */
            emit(e, "({ uint32_t _zer_di = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_dv = 0;\n"
                "#if defined(__x86_64__)\n"
                "    switch (_zer_di) {\n"
                "    case 0: __asm__ __volatile__ (\"movq %%%%dr0, %%0\" : \"=r\"(_zer_dv)); break;\n"
                "    case 1: __asm__ __volatile__ (\"movq %%%%dr1, %%0\" : \"=r\"(_zer_dv)); break;\n"
                "    case 2: __asm__ __volatile__ (\"movq %%%%dr2, %%0\" : \"=r\"(_zer_dv)); break;\n"
                "    case 3: __asm__ __volatile__ (\"movq %%%%dr3, %%0\" : \"=r\"(_zer_dv)); break;\n"
                "    case 6: __asm__ __volatile__ (\"movq %%%%dr6, %%0\" : \"=r\"(_zer_dv)); break;\n"
                "    case 7: __asm__ __volatile__ (\"movq %%%%dr7, %%0\" : \"=r\"(_zer_dv)); break;\n"
                "    default: break;\n"
                "    }\n"
                "#endif\n"
                "_zer_dv; })");
        } else if (nlen == 12 && memcmp(name, "cpu_write_dr", 12) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-13: write debug register. */
            emit(e, "({ uint32_t _zer_di = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_dv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    switch (_zer_di) {\n"
                "    case 0: __asm__ __volatile__ (\"movq %%0, %%%%dr0\" :: \"r\"(_zer_dv)); break;\n"
                "    case 1: __asm__ __volatile__ (\"movq %%0, %%%%dr1\" :: \"r\"(_zer_dv)); break;\n"
                "    case 2: __asm__ __volatile__ (\"movq %%0, %%%%dr2\" :: \"r\"(_zer_dv)); break;\n"
                "    case 3: __asm__ __volatile__ (\"movq %%0, %%%%dr3\" :: \"r\"(_zer_dv)); break;\n"
                "    case 6: __asm__ __volatile__ (\"movq %%0, %%%%dr6\" :: \"r\"(_zer_dv)); break;\n"
                "    case 7: __asm__ __volatile__ (\"movq %%0, %%%%dr7\" :: \"r\"(_zer_dv)); break;\n"
                "    default: break;\n"
                "    }\n"
                "#else\n"
                "    (void)_zer_di; (void)_zer_dv;\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_sbi_call", 12) == 0) {
            /* D-Alpha-13: RISC-V SBI call (ecall from S-mode to M-mode firmware). */
            emit(e, "({\n"
                "#if defined(__riscv)\n"
                "    __asm__ __volatile__ (\"ecall\" ::: \"memory\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_smc_call", 12) == 0) {
            /* D-Alpha-13: ARM TrustZone SMC (secure monitor call). */
            emit(e, "({\n"
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"smc #0\" ::: \"memory\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 14 && memcmp(name, "cache_flushopt", 14) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-13: CLFLUSHOPT (ordered alternative to CLFLUSH; needs feature bit). */
            emit(e, "({ const void *_zer_cf = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"clflushopt (%%0)\" :: \"r\"(_zer_cf) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_cf;\n"
                "#endif\n"
                "})");
        } else if (nlen == 15 && memcmp(name, "cache_writeback", 15) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-13: CLWB (writeback without invalidate — for persistent memory). */
            emit(e, "({ const void *_zer_cw = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"clwb (%%0)\" :: \"r\"(_zer_cw) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_cw;\n"
                "#endif\n"
                "})");
        } else if (nlen == 8 && memcmp(name, "nt_store", 8) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-13: non-temporal store (MOVNTI) — bypasses cache. */
            emit(e, "({ void *_zer_na = (void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_nv = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movnti %%1, (%%0)\" :: \"r\"(_zer_na), \"r\"(_zer_nv) : \"memory\");\n"
                "#else\n"
                "    *(uint64_t*)_zer_na = _zer_nv;  /* fallback: regular store */\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_read_pmc", 12) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-13: RDPMC — read performance monitoring counter. */
            emit(e, "({ uint32_t _zer_pmi = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_pmv = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_plo, _zer_phi;\n"
                "    __asm__ __volatile__ (\"rdpmc\" : \"=a\"(_zer_plo), \"=d\"(_zer_phi) : \"c\"(_zer_pmi));\n"
                "    _zer_pmv = ((uint64_t)_zer_phi << 32) | _zer_plo;\n"
                "#endif\n"
                "_zer_pmv; })");
        } else if (nlen == 9 && memcmp(name, "cpu_cpuid", 9) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-14: CPUID — returns (EBX << 32) | EAX packed into u64. */
            emit(e, "({ uint32_t _zer_cl = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint32_t _zer_cs = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, "); uint64_t _zer_cq = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_ea, _zer_eb, _zer_ec, _zer_ed;\n"
                "    __asm__ __volatile__ (\"cpuid\"\n"
                "        : \"=a\"(_zer_ea), \"=b\"(_zer_eb), \"=c\"(_zer_ec), \"=d\"(_zer_ed)\n"
                "        : \"a\"(_zer_cl), \"c\"(_zer_cs));\n"
                "    _zer_cq = ((uint64_t)_zer_eb << 32) | _zer_ea;\n"
                "#endif\n"
                "_zer_cq; })");
        } else if (nlen == 13 && memcmp(name, "cpu_cpuid_ecx", 13) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-14: CPUID returning ECX/EDX packed into u64. */
            emit(e, "({ uint32_t _zer_cl = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint32_t _zer_cs = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, "); uint64_t _zer_cq = 0;\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_ea, _zer_eb, _zer_ec, _zer_ed;\n"
                "    __asm__ __volatile__ (\"cpuid\"\n"
                "        : \"=a\"(_zer_ea), \"=b\"(_zer_eb), \"=c\"(_zer_ec), \"=d\"(_zer_ed)\n"
                "        : \"a\"(_zer_cl), \"c\"(_zer_cs));\n"
                "    _zer_cq = ((uint64_t)_zer_ed << 32) | _zer_ec;\n"
                "#endif\n"
                "_zer_cq; })");
        } else if (nlen == 7 && memcmp(name, "cpu_eoi", 7) == 0) {
            /* D-Alpha-14: End-of-interrupt signal to the interrupt controller.
             * x86: write 0 to LAPIC EOI register (MMIO, 0xFEE000B0 by default) —
             *      but that needs the LAPIC base known. Simplest form: WRMSR to
             *      IA32_X2APIC_EOI (0x80B) which works in x2APIC mode.
             * ARM64: write to ICC_EOIR1_EL1 system register (GICv3).
             * RISC-V: platform-specific (SiFive CLINT or PLIC).
             * This is a best-effort emission; users on bespoke platforms override. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    /* x2APIC EOI (MSR 0x80B) — writes 0 */\n"
                "    __asm__ __volatile__ (\"wrmsr\" :: \"a\"(0), \"d\"(0), \"c\"(0x80B));\n"
                "#elif defined(__aarch64__)\n"
                "    /* GICv3 EOI (ICC_EOIR1_EL1, op0=3 op1=0 CRn=12 CRm=12 op2=1) */\n"
                "    __asm__ __volatile__ (\"msr s3_0_c12_c12_1, xzr\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_read_cr2", 12) == 0) {
            /* D-Alpha-14: read CR2 — contains faulting address after #PF. Privileged. */
            emit(e, "({ uint64_t _zer_cr = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"movq %%%%cr2, %%0\" : \"=r\"(_zer_cr));\n"
                "#endif\n"
                "_zer_cr; })");
        } else if (nlen == 17 && memcmp(name, "cpu_cache_disable", 17) == 0) {
            /* D-Alpha-14: disable all caches — set CR0.CD (bit 30). Privileged.
             * Uses BTS (bit test+set) to avoid 32-bit-imm-with-64-bit-reg issues. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    uint64_t _zer_c0;\n"
                "    __asm__ __volatile__ (\n"
                "        \"movq %%%%cr0, %%0\\n\\t\"\n"
                "        \"btsq $30, %%0\\n\\t\"\n"
                "        \"movq %%0, %%%%cr0\\n\\t\"\n"
                "        \"wbinvd\"\n"
                "        : \"=&r\"(_zer_c0) :: \"memory\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 16 && memcmp(name, "cpu_cache_enable", 16) == 0) {
            /* D-Alpha-14: enable all caches — clear CR0.CD (bit 30). Privileged.
             * Uses BTR (bit test+reset) to avoid 32-bit-imm-with-64-bit-reg issues. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    uint64_t _zer_c0;\n"
                "    __asm__ __volatile__ (\n"
                "        \"movq %%%%cr0, %%0\\n\\t\"\n"
                "        \"btrq $30, %%0\\n\\t\"\n"
                "        \"movq %%0, %%%%cr0\"\n"
                "        : \"=&r\"(_zer_c0) :: \"memory\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 10 && memcmp(name, "cpu_fxsave", 10) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-14: FXSAVE — legacy FP/SSE state save (512 bytes, 16-byte aligned). */
            emit(e, "({ void *_zer_fb = (void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"fxsave (%%0)\" :: \"r\"(_zer_fb) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_fb;\n"
                "#endif\n"
                "})");
        } else if (nlen == 11 && memcmp(name, "cpu_fxrstor", 11) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-14: FXRSTOR — legacy FP/SSE state restore. */
            emit(e, "({ const void *_zer_fb = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"fxrstor (%%0)\" :: \"r\"(_zer_fb) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_fb;\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_fpu_init", 12) == 0) {
            /* D-Alpha-14: FINIT — initialize x87 FPU to defaults. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"fninit\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 10 && memcmp(name, "cpu_umwait", 10) == 0 &&
                   node->intrinsic.arg_count >= 2) {
            /* D-Alpha-14: UMWAIT — user-mode wait until monitored write or deadline.
             * hint: 0 = C0.2 (optimized), 1 = C0.1 (faster wakeup).
             * Requires WAITPKG feature. */
            emit(e, "({ uint32_t _zer_uh = (uint32_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "); uint64_t _zer_ud = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    uint32_t _zer_ulo = (uint32_t)_zer_ud, _zer_uhi = (uint32_t)(_zer_ud >> 32);\n"
                "    /* UMWAIT encoded as: F2 0F AE /6 */\n"
                "    __asm__ __volatile__ (\"umwait %%0\" :: \"r\"(_zer_uh), \"a\"(_zer_ulo), \"d\"(_zer_uhi));\n"
                "#else\n"
                "    (void)_zer_uh; (void)_zer_ud;\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_umonitor", 12) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-14: UMONITOR — set up user-mode address watch (pairs with UMWAIT). */
            emit(e, "({ const void *_zer_ua = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"umonitor %%0\" :: \"r\"(_zer_ua));\n"
                "#else\n"
                "    (void)_zer_ua;\n"
                "#endif\n"
                "})");
        } else if (nlen == 9 && memcmp(name, "cpu_endbr", 9) == 0) {
            /* D-Alpha-14: ENDBR64 — CET-IBT indirect-branch landing pad.
             * Emitted at start of functions that may be called via indirect branch.
             * No-op on CPUs without CET-IBT (encoded as multi-byte NOP). */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"endbr64\");\n"
                "#endif\n"
                "})");
        } else if (nlen == 10 && memcmp(name, "cpu_rdrand", 10) == 0) {
            /* D-Alpha-7: hardware RNG — returns ?u64 (optional because instruction can fail).
             * x86-64: RDRAND sets CF on success. Not universally available on ARM/RISC-V base. */
            emit(e, "({ _zer_opt_u64 _zer_rr = {0};\n"
                "#if defined(__x86_64__) && defined(__RDRND__)\n"
                "    uint64_t _zer_v; uint8_t _zer_ok;\n"
                "    __asm__ __volatile__ (\"rdrand %%0; setc %%1\" : \"=r\"(_zer_v), \"=r\"(_zer_ok) :: \"cc\");\n"
                "    if (_zer_ok) { _zer_rr.has_value = 1; _zer_rr.value = _zer_v; }\n"
                "#else\n"
                "    /* Not available on this target — returns null optional */\n"
                "#endif\n"
                "_zer_rr; })");
        } else if (nlen == 10 && memcmp(name, "cpu_rdseed", 10) == 0) {
            /* D-Alpha-7: hardware entropy source (stronger than rdrand). Intel/AMD only. */
            emit(e, "({ _zer_opt_u64 _zer_rs = {0};\n"
                "#if defined(__x86_64__) && defined(__RDSEED__)\n"
                "    uint64_t _zer_v; uint8_t _zer_ok;\n"
                "    __asm__ __volatile__ (\"rdseed %%0; setc %%1\" : \"=r\"(_zer_v), \"=r\"(_zer_ok) :: \"cc\");\n"
                "    if (_zer_ok) { _zer_rs.has_value = 1; _zer_rs.value = _zer_v; }\n"
                "#endif\n"
                "_zer_rs; })");
        } else if (nlen == 11 && memcmp(name, "unreachable", 11) == 0) {
            /* D-Alpha-2: GCC unreachable hint (undefined behavior if reached) */
            emit(e, "__builtin_unreachable()");
        } else if (nlen == 15 && memcmp(name, "cpu_disable_int", 15) == 0) {
            /* D-Alpha-3: disable interrupts. Per-arch via preprocessor. Privileged op. */
            emit(e, "({\n"
                "#if defined(__x86_64__) || defined(__i386__)\n"
                "    __asm__ __volatile__ (\"cli\" ::: \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"msr daifset, #2\" ::: \"memory\");\n"
                "#elif defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\"cpsid i\" ::: \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrci mstatus, 8\" ::: \"memory\");\n"
                "#else\n"
                "#  error \"@cpu_disable_int has no implementation for this target architecture (ZER supports x86, ARM, AArch64, RISC-V)\"\n"
                "#endif\n"
                "})");
        } else if (nlen == 14 && memcmp(name, "cpu_enable_int", 14) == 0) {
            /* D-Alpha-3: enable interrupts. */
            emit(e, "({\n"
                "#if defined(__x86_64__) || defined(__i386__)\n"
                "    __asm__ __volatile__ (\"sti\" ::: \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"msr daifclr, #2\" ::: \"memory\");\n"
                "#elif defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\"cpsie i\" ::: \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrsi mstatus, 8\" ::: \"memory\");\n"
                "#else\n"
                "#  error \"@cpu_enable_int has no implementation for this target architecture (ZER supports x86, ARM, AArch64, RISC-V)\"\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_wait_int", 12) == 0) {
            /* D-Alpha-3: halt/wait until next interrupt. */
            emit(e, "({\n"
                "#if defined(__x86_64__) || defined(__i386__)\n"
                "    __asm__ __volatile__ (\"hlt\" ::: \"memory\");\n"
                "#elif defined(__ARM_ARCH) || defined(__aarch64__) || defined(__riscv)\n"
                "    __asm__ __volatile__ (\"wfi\" ::: \"memory\");\n"
                "#else\n"
                "    /* no-op fallback */\n"
                "#endif\n"
                "})");
        } else if (nlen == 18 && memcmp(name, "cpu_save_int_state", 18) == 0) {
            /* D-Alpha-3: save current interrupt flag state. Returns u64. */
            emit(e, "({ uint64_t _zer_istate = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"pushfq; popq %0\" : \"=r\"(_zer_istate) :: \"memory\");\n"
                "#elif defined(__i386__)\n"
                "    __asm__ __volatile__ (\"pushfl; popl %0\" : \"=r\"(_zer_istate) :: \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mrs %0, daif\" : \"=r\"(_zer_istate) :: \"memory\");\n"
                "#elif defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')\n"
                "    { uint32_t _zer_p; __asm__ __volatile__ (\"mrs %0, primask\" : \"=r\"(_zer_p) :: \"memory\"); _zer_istate = _zer_p; }\n"
                "#elif defined(__ARM_ARCH)\n"
                "    { uint32_t _zer_p; __asm__ __volatile__ (\"mrs %0, cpsr\" : \"=r\"(_zer_p) :: \"memory\"); _zer_istate = _zer_p; }\n"
                "#elif defined(__riscv)\n"
                "    { unsigned long _zer_m; __asm__ __volatile__ (\"csrr %0, mstatus\" : \"=r\"(_zer_m) :: \"memory\"); _zer_istate = _zer_m; }\n"
                "#else\n"
                "#  error \"@cpu_save_int_state has no implementation for this target architecture (ZER supports x86, ARM, AArch64, RISC-V)\"\n"
                "#endif\n"
                "_zer_istate; })");
        } else if (nlen == 10 && memcmp(name, "mmu_set_pt", 10) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-5: set user/active page table base. Privileged. */
            emit(e, "({ uint64_t _zer_pt = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"mov %%0, %%%%cr3\" : : \"r\"(_zer_pt) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"msr ttbr0_el1, %%0\\n\\tisb\" : : \"r\"(_zer_pt) : \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrw satp, %%0\\n\\tsfence.vma\" : : \"r\"(_zer_pt) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_pt;\n"
                "#endif\n"
                "})");
        } else if (nlen == 10 && memcmp(name, "mmu_get_pt", 10) == 0) {
            /* D-Alpha-5: read active page table base. Returns u64. Privileged. */
            emit(e, "({ uint64_t _zer_pt = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"mov %%%%cr3, %%0\" : \"=r\"(_zer_pt));\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mrs %%0, ttbr0_el1\" : \"=r\"(_zer_pt));\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrr %%0, satp\" : \"=r\"(_zer_pt));\n"
                "#endif\n"
                "_zer_pt; })");
        } else if (nlen == 17 && memcmp(name, "mmu_set_kernel_pt", 17) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-5: set kernel page table (ARM TTBR1; aliased to TTBR0 on x86/RISC-V). */
            emit(e, "({ uint64_t _zer_kpt = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"msr ttbr1_el1, %%0\\n\\tisb\" : : \"r\"(_zer_kpt) : \"memory\");\n"
                "#elif defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"mov %%0, %%%%cr3\" : : \"r\"(_zer_kpt) : \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrw satp, %%0\\n\\tsfence.vma\" : : \"r\"(_zer_kpt) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_kpt;\n"
                "#endif\n"
                "})");
        } else if (nlen == 17 && memcmp(name, "mmu_get_kernel_pt", 17) == 0) {
            /* D-Alpha-5: read kernel page table. */
            emit(e, "({ uint64_t _zer_kpt = 0;\n"
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mrs %%0, ttbr1_el1\" : \"=r\"(_zer_kpt));\n"
                "#elif defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"mov %%%%cr3, %%0\" : \"=r\"(_zer_kpt));\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrr %%0, satp\" : \"=r\"(_zer_kpt));\n"
                "#endif\n"
                "_zer_kpt; })");
        } else if (nlen == 10 && memcmp(name, "mmu_enable", 10) == 0) {
            /* D-Alpha-5: turn on paging. Privileged.
             * x86-64 uses btsq (bit test and set) to set CR0.PG (bit 31) —
             * simpler than building a 64-bit imm with `or`. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"mov %%%%cr0, %%%%rax\\n\\t\"\n"
                "        \"btsq $31, %%%%rax\\n\\t\"\n"
                "        \"mov %%%%rax, %%%%cr0\" : : : \"rax\", \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"mrs x0, sctlr_el1\\n\\t\"\n"
                "        \"orr x0, x0, #1\\n\\t\"\n"
                "        \"msr sctlr_el1, x0\\n\\tisb\" : : : \"x0\", \"memory\");\n"
                "#else\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 11 && memcmp(name, "mmu_disable", 11) == 0) {
            /* D-Alpha-5: turn off paging. Privileged. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"mov %%%%cr0, %%%%rax\\n\\t\"\n"
                "        \"btrq $31, %%%%rax\\n\\t\"\n"
                "        \"mov %%%%rax, %%%%cr0\" : : : \"rax\", \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"mrs x0, sctlr_el1\\n\\t\"\n"
                "        \"bic x0, x0, #1\\n\\t\"\n"
                "        \"msr sctlr_el1, x0\\n\\tisb\" : : : \"x0\", \"memory\");\n"
                "#else\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 14 && memcmp(name, "mmu_is_enabled", 14) == 0) {
            /* D-Alpha-5: read paging enable bit. Returns bool. */
            emit(e, "({ uint64_t _zer_cr = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"mov %%%%cr0, %%0\" : \"=r\"(_zer_cr));\n"
                "    _zer_cr = (_zer_cr >> 31) & 1;\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mrs %%0, sctlr_el1\" : \"=r\"(_zer_cr));\n"
                "    _zer_cr = _zer_cr & 1;\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrr %%0, satp\" : \"=r\"(_zer_cr));\n"
                "    _zer_cr = (_zer_cr >> 60) != 0;  /* Sv39/48/57 set MODE != 0 when enabled */\n"
                "#endif\n"
                "(_zer_cr != 0); })");
        } else if (nlen == 18 && memcmp(name, "mmu_get_fault_addr", 18) == 0) {
            /* D-Alpha-5: read fault address after page fault. Privileged. */
            emit(e, "({ uint64_t _zer_fa = 0;\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"mov %%%%cr2, %%0\" : \"=r\"(_zer_fa));\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mrs %%0, far_el1\" : \"=r\"(_zer_fa));\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrr %%0, stval\" : \"=r\"(_zer_fa));\n"
                "#endif\n"
                "_zer_fa; })");
        } else if (nlen == 20 && memcmp(name, "mmu_get_fault_status", 20) == 0) {
            /* D-Alpha-5: read fault status/syndrome. Privileged. */
            emit(e, "({ uint64_t _zer_fs = 0;\n"
                "#if defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"mrs %%0, esr_el1\" : \"=r\"(_zer_fs));\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"csrr %%0, scause\" : \"=r\"(_zer_fs));\n"
                "#endif\n"
                "/* x86 fault status comes from interrupt frame, not a register */\n"
                "_zer_fs; })");
        } else if (nlen == 8 && memcmp(name, "mmu_sync", 8) == 0) {
            /* D-Alpha-5: synchronize pending page-table updates. */
            emit(e, "({\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"\" : : : \"memory\");  /* implicit via cr3 write */\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"dsb ishst\\n\\tisb\" : : : \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\"sfence.vma\\n\\tfence.i\" : : : \"memory\");\n"
                "#else\n"
                "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n"
                "#endif\n"
                "})");
        } else if (nlen == 16 && memcmp(name, "cpu_save_context", 16) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-4: save callee-saved GPRs to buffer.
             * Note: in emit() fmt string, % must be doubled for fprintf.
             * %%%%rbx in fmt -> %%rbx in output -> %rbx in GCC inline asm (literal register).
             * %%0 in fmt -> %0 in output (operand ref 0 for GCC asm). */
            emit(e, "({ void *_zer_ctx = (void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"movq %%%%rbx, 0(%%0)\\n\\t\"\n"
                "        \"movq %%%%r12, 8(%%0)\\n\\t\"\n"
                "        \"movq %%%%r13, 16(%%0)\\n\\t\"\n"
                "        \"movq %%%%r14, 24(%%0)\\n\\t\"\n"
                "        \"movq %%%%r15, 32(%%0)\\n\\t\"\n"
                "        : : \"r\"(_zer_ctx) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"stp x19, x20, [%%0, #0]\\n\\t\"\n"
                "        \"stp x21, x22, [%%0, #16]\\n\\t\"\n"
                "        \"stp x23, x24, [%%0, #32]\\n\\t\"\n"
                "        \"stp x25, x26, [%%0, #48]\\n\\t\"\n"
                "        \"stp x27, x28, [%%0, #64]\\n\\t\"\n"
                "        : : \"r\"(_zer_ctx) : \"memory\");\n"
                "#elif defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\n"
                "        \"stm %%0, {r4-r11}\\n\\t\"\n"
                "        : : \"r\"(_zer_ctx) : \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\n"
                "        \"sd s0, 0(%%0)\\n\\t\"\n"
                "        \"sd s1, 8(%%0)\\n\\t\"\n"
                "        \"sd s2, 16(%%0)\\n\\t\"\n"
                "        \"sd s3, 24(%%0)\\n\\t\"\n"
                "        \"sd s4, 32(%%0)\\n\\t\"\n"
                "        \"sd s5, 40(%%0)\\n\\t\"\n"
                "        \"sd s6, 48(%%0)\\n\\t\"\n"
                "        \"sd s7, 56(%%0)\\n\\t\"\n"
                "        \"sd s8, 64(%%0)\\n\\t\"\n"
                "        \"sd s9, 72(%%0)\\n\\t\"\n"
                "        \"sd s10, 80(%%0)\\n\\t\"\n"
                "        \"sd s11, 88(%%0)\\n\\t\"\n"
                "        : : \"r\"(_zer_ctx) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_ctx;\n"
                "#endif\n"
                "})");
        } else if (nlen == 19 && memcmp(name, "cpu_restore_context", 19) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-4: restore callee-saved GPRs from buffer. */
            emit(e, "({ const void *_zer_ctx = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"movq 0(%%0), %%%%rbx\\n\\t\"\n"
                "        \"movq 8(%%0), %%%%r12\\n\\t\"\n"
                "        \"movq 16(%%0), %%%%r13\\n\\t\"\n"
                "        \"movq 24(%%0), %%%%r14\\n\\t\"\n"
                "        \"movq 32(%%0), %%%%r15\\n\\t\"\n"
                "        : : \"r\"(_zer_ctx) : \"rbx\", \"r12\", \"r13\", \"r14\", \"r15\", \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"ldp x19, x20, [%%0, #0]\\n\\t\"\n"
                "        \"ldp x21, x22, [%%0, #16]\\n\\t\"\n"
                "        \"ldp x23, x24, [%%0, #32]\\n\\t\"\n"
                "        \"ldp x25, x26, [%%0, #48]\\n\\t\"\n"
                "        \"ldp x27, x28, [%%0, #64]\\n\\t\"\n"
                "        : : \"r\"(_zer_ctx) : \"x19\", \"x20\", \"x21\", \"x22\", \"x23\", \"x24\", \"x25\", \"x26\", \"x27\", \"x28\", \"memory\");\n"
                "#elif defined(__ARM_ARCH)\n"
                "    __asm__ __volatile__ (\n"
                "        \"ldm %%0, {r4-r11}\\n\\t\"\n"
                "        : : \"r\"(_zer_ctx) : \"r4\", \"r5\", \"r6\", \"r7\", \"r8\", \"r9\", \"r10\", \"r11\", \"memory\");\n"
                "#elif defined(__riscv)\n"
                "    __asm__ __volatile__ (\n"
                "        \"ld s0, 0(%%0)\\n\\t\"\n"
                "        \"ld s1, 8(%%0)\\n\\t\"\n"
                "        \"ld s2, 16(%%0)\\n\\t\"\n"
                "        \"ld s3, 24(%%0)\\n\\t\"\n"
                "        \"ld s4, 32(%%0)\\n\\t\"\n"
                "        \"ld s5, 40(%%0)\\n\\t\"\n"
                "        \"ld s6, 48(%%0)\\n\\t\"\n"
                "        \"ld s7, 56(%%0)\\n\\t\"\n"
                "        \"ld s8, 64(%%0)\\n\\t\"\n"
                "        \"ld s9, 72(%%0)\\n\\t\"\n"
                "        \"ld s10, 80(%%0)\\n\\t\"\n"
                "        \"ld s11, 88(%%0)\\n\\t\"\n"
                "        : : \"r\"(_zer_ctx) : \"s0\", \"s1\", \"s2\", \"s3\", \"s4\", \"s5\", \"s6\", \"s7\", \"s8\", \"s9\", \"s10\", \"s11\", \"memory\");\n"
                "#else\n"
                "    (void)_zer_ctx;\n"
                "#endif\n"
                "})");
        } else if (nlen == 12 && memcmp(name, "cpu_save_fpu", 12) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-4: save FPU/SIMD state to buffer.
             * x86-64: fxsave (512 bytes, 16-byte aligned)
             * ARM64: stp q0-q31 in pairs (512 bytes) */
            emit(e, "({ void *_zer_fpu = (void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"fxsave (%%0)\" : : \"r\"(_zer_fpu) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"stp q0, q1, [%%0, #0]\\n\\t\"\n"
                "        \"stp q2, q3, [%%0, #32]\\n\\t\"\n"
                "        \"stp q4, q5, [%%0, #64]\\n\\t\"\n"
                "        \"stp q6, q7, [%%0, #96]\\n\\t\"\n"
                "        \"stp q8, q9, [%%0, #128]\\n\\t\"\n"
                "        \"stp q10, q11, [%%0, #160]\\n\\t\"\n"
                "        \"stp q12, q13, [%%0, #192]\\n\\t\"\n"
                "        \"stp q14, q15, [%%0, #224]\\n\\t\"\n"
                "        : : \"r\"(_zer_fpu) : \"memory\");\n"
                "#else\n"
                "    /* Generic fallback: no FPU state tracked */\n"
                "    (void)_zer_fpu;\n"
                "#endif\n"
                "})");
        } else if (nlen == 15 && memcmp(name, "cpu_restore_fpu", 15) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-4: restore FPU/SIMD state from buffer. */
            emit(e, "({ const void *_zer_fpu = (const void*)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"fxrstor (%%0)\" : : \"r\"(_zer_fpu) : \"memory\");\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\n"
                "        \"ldp q0, q1, [%%0, #0]\\n\\t\"\n"
                "        \"ldp q2, q3, [%%0, #32]\\n\\t\"\n"
                "        \"ldp q4, q5, [%%0, #64]\\n\\t\"\n"
                "        \"ldp q6, q7, [%%0, #96]\\n\\t\"\n"
                "        \"ldp q8, q9, [%%0, #128]\\n\\t\"\n"
                "        \"ldp q10, q11, [%%0, #160]\\n\\t\"\n"
                "        \"ldp q12, q13, [%%0, #192]\\n\\t\"\n"
                "        \"ldp q14, q15, [%%0, #224]\\n\\t\"\n"
                "        : : \"r\"(_zer_fpu) : \"memory\");\n"
                "#else\n"
                "    (void)_zer_fpu;\n"
                "#endif\n"
                "})");
        } else if (nlen == 21 && memcmp(name, "cpu_restore_int_state", 21) == 0 &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-3: restore interrupt flag state from saved value. */
            emit(e, "({ uint64_t _zer_rstate = (uint64_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ");\n"
                "#if defined(__x86_64__)\n"
                "    __asm__ __volatile__ (\"pushq %0; popfq\" :: \"r\"(_zer_rstate) : \"memory\", \"cc\");\n"
                "#elif defined(__i386__)\n"
                "    { uint32_t _zer_r32 = (uint32_t)_zer_rstate; __asm__ __volatile__ (\"pushl %0; popfl\" :: \"r\"(_zer_r32) : \"memory\", \"cc\"); }\n"
                "#elif defined(__aarch64__)\n"
                "    __asm__ __volatile__ (\"msr daif, %0\" :: \"r\"(_zer_rstate) : \"memory\");\n"
                "#elif defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')\n"
                "    { uint32_t _zer_r32 = (uint32_t)_zer_rstate; __asm__ __volatile__ (\"msr primask, %0\" :: \"r\"(_zer_r32) : \"memory\"); }\n"
                "#elif defined(__ARM_ARCH)\n"
                "    { uint32_t _zer_r32 = (uint32_t)_zer_rstate; __asm__ __volatile__ (\"msr cpsr_c, %0\" :: \"r\"(_zer_r32) : \"memory\"); }\n"
                "#elif defined(__riscv)\n"
                "    { unsigned long _zer_m = (unsigned long)_zer_rstate; __asm__ __volatile__ (\"csrw mstatus, %0\" :: \"r\"(_zer_m) : \"memory\"); }\n"
                "#else\n"
                "    (void)_zer_rstate;\n"
                "#endif\n"
                "})");
        } else if (nlen == 6 && memcmp(name, "expect", 6) == 0 && node->intrinsic.arg_count >= 2) {
            /* D-Alpha-2: branch prediction hint */
            emit(e, "__builtin_expect((long)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "), (long)(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, "))");
        } else if (nlen == 7 && (memcmp(name, "bswap16", 7) == 0 ||
                                 memcmp(name, "bswap32", 7) == 0 ||
                                 memcmp(name, "bswap64", 7) == 0) && node->intrinsic.arg_count >= 1) {
            /* D-Alpha-2: byte swap — GCC builtin */
            emit(e, "__builtin_%.*s(", (int)nlen, name);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")");
        } else if (((nlen == 8 && memcmp(name, "popcount", 8) == 0) ||
                    (nlen == 3 && memcmp(name, "ctz", 3) == 0) ||
                    (nlen == 3 && memcmp(name, "clz", 3) == 0) ||
                    (nlen == 6 && memcmp(name, "parity", 6) == 0) ||
                    (nlen == 3 && memcmp(name, "ffs", 3) == 0)) &&
                   node->intrinsic.arg_count >= 1) {
            /* D-Alpha-2: bit queries (dispatch on width).
             *
             * AUDIT-2026-06-08: @ctz(0) and @clz(0) silently call
             * __builtin_ctz(0)/__builtin_clz(0) — undefined behavior in C
             * (GCC docs explicitly: "If x is 0, the result is undefined").
             * On x86 the underlying BSF/BSR leaves the destination register
             * untouched when the input is zero, leaking garbage as the
             * "trailing/leading zero count". On baremetal targets without
             * the BMI1 extension (TZCNT/LZCNT), the same undefined behavior
             * applies. @popcount(0)=0, @parity(0)=0, @ffs(0)=0 are all
             * defined by GCC — no fix needed for those.
             *
             * Fix: wrap @ctz / @clz in a zero-guard that returns the type
             * width (the natural mathematical answer: ctz(0) = bit width,
             * clz(0) = bit width). This matches Rust's u32::trailing_zeros
             * and Zig's @ctz semantics. */
            Type *arg_t = checker_get_type(e->checker, node->intrinsic.args[0]);
            int w = arg_t ? type_width(type_unwrap_distinct(arg_t)) : 32;
            const char *suffix = (w > 32) ? "ll" : "";
            bool is_ctz = (nlen == 3 && memcmp(name, "ctz", 3) == 0);
            bool is_clz = (nlen == 3 && memcmp(name, "clz", 3) == 0);
            if (is_ctz || is_clz) {
                /* AUDIT-2026-06-28: when arg has no side effects (static local
                 * init with a literal is the canonical case), prefer the
                 * conditional-expression form. Statement expressions are NOT
                 * valid in static-local initializers in C; the IR-path always
                 * emitted `({...})` for the zero-guard, so
                 * `static u32 v = @ctz(16);` cleanly compiled in ZER but failed
                 * GCC with "initializer element is not constant". The conditional
                 * form double-evaluates the arg, which is safe because the
                 * side-effect-free gate excludes calls / assigns / etc. */
                if (!expr_has_side_effects(node->intrinsic.args[0])) {
                    emit(e, "(uint32_t)((");
                    bitq_operand_open(e, w);
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                    bitq_operand_close(e, w);
                    emit(e, " == 0) ? %d : __builtin_%.*s%s(", bitq_count_width(w), (int)nlen, name, suffix);
                    bitq_operand_open(e, w);
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                    bitq_operand_close(e, w);
                    emit(e, "))");
                } else {
                    int t = e->temp_count++;
                    emit(e, "({ %s _zer_bz%d = ", w > 32 ? "uint64_t" : "uint32_t", t);
                    bitq_operand_open(e, w);
                    emit_rewritten_node(e, node->intrinsic.args[0], func);
                    bitq_operand_close(e, w);
                    emit(e, "; (uint32_t)(_zer_bz%d == 0 ? %d : __builtin_%.*s%s(_zer_bz%d)); })",
                         t, bitq_count_width(w), (int)nlen, name, suffix, t);
                }
            } else {
                emit(e, "(uint32_t)__builtin_%.*s%s(", (int)nlen, name, suffix);
                bitq_operand_open(e, w);
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                bitq_operand_close(e, w);
                emit(e, ")");
            }
        } else if (nlen == 12 && memcmp(name, "barrier_init", 12) == 0 && node->intrinsic.arg_count >= 2) {
            /* BUG-574: thread barrier init on IR path — same shape as AST emit_expr */
            Type *bt = checker_get_type(e->checker, node->intrinsic.args[0]);
            bool is_ptr = bt && type_unwrap_distinct(bt)->kind == TYPE_POINTER;
            emit(e, "_zer_barrier_init(");
            if (!is_ptr) emit(e, "&");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ", ");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ")");
        } else if (nlen == 12 && memcmp(name, "barrier_wait", 12) == 0 && node->intrinsic.arg_count >= 1) {
            /* BUG-574: thread barrier wait on IR path */
            Type *bt = checker_get_type(e->checker, node->intrinsic.args[0]);
            bool is_ptr = bt && type_unwrap_distinct(bt)->kind == TYPE_POINTER;
            emit(e, "_zer_barrier_wait(");
            if (!is_ptr) emit(e, "&");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")");
        } else if (nlen >= 7 && memcmp(name, "atomic_", 7) == 0) {
            /* @atomic_* intrinsics — Phase D-Alpha-1 (2026-04-23).
             * Extended from 8 to 15 intrinsics. All SEQ_CST ordering for now (Ordering param deferred).
             * Existing: load, store, cas, add, sub, or, and, xor
             * D-Alpha-1 new: xchg, nand, add_fetch, sub_fetch, or_fetch, and_fetch, xor_fetch */
            const char *aop = name + 7;
            uint32_t aolen = nlen - 7;
            if (aolen == 4 && !memcmp(aop, "load", 4) && node->intrinsic.arg_count > 0) {
                emit(e, "__atomic_load_n("); emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ", __ATOMIC_SEQ_CST)");
            } else if (aolen == 5 && !memcmp(aop, "store", 5) && node->intrinsic.arg_count > 1) {
                emit(e, "__atomic_store_n("); emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ", "); emit_rewritten_node(e, node->intrinsic.args[1], func);
                emit(e, ", __ATOMIC_SEQ_CST)");
            } else if (aolen == 3 && !memcmp(aop, "cas", 3) && node->intrinsic.arg_count > 2) {
                int t = e->temp_count++;
                emit(e, "({__typeof__(*"); emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ") _zer_cas_exp%d = ", t); emit_rewritten_node(e, node->intrinsic.args[1], func);
                emit(e, "; __atomic_compare_exchange_n("); emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ", &_zer_cas_exp%d, ", t); emit_rewritten_node(e, node->intrinsic.args[2], func);
                emit(e, ", 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })");
            } else if (node->intrinsic.arg_count > 1) {
                /* 2-arg ops that take (ptr, val) and return T.
                 * All emit GCC builtin with signature: builtin(ptr, val, ordering). */
                const char *gcc_op = NULL;
                /* existing fetch-old variants */
                if (aolen == 3 && !memcmp(aop, "add", 3)) gcc_op = "__atomic_fetch_add";
                else if (aolen == 3 && !memcmp(aop, "sub", 3)) gcc_op = "__atomic_fetch_sub";
                else if (aolen == 2 && !memcmp(aop, "or", 2)) gcc_op = "__atomic_fetch_or";
                else if (aolen == 3 && !memcmp(aop, "and", 3)) gcc_op = "__atomic_fetch_and";
                else if (aolen == 3 && !memcmp(aop, "xor", 3)) gcc_op = "__atomic_fetch_xor";
                /* D-Alpha-1: xchg + nand (return old value) */
                else if (aolen == 4 && !memcmp(aop, "xchg", 4)) gcc_op = "__atomic_exchange_n";
                else if (aolen == 4 && !memcmp(aop, "nand", 4)) gcc_op = "__atomic_fetch_nand";
                /* D-Alpha-1: *_fetch variants (return new value — operation applied first) */
                else if (aolen == 9 && !memcmp(aop, "add_fetch", 9)) gcc_op = "__atomic_add_fetch";
                else if (aolen == 9 && !memcmp(aop, "sub_fetch", 9)) gcc_op = "__atomic_sub_fetch";
                else if (aolen == 8 && !memcmp(aop, "or_fetch", 8)) gcc_op = "__atomic_or_fetch";
                else if (aolen == 9 && !memcmp(aop, "and_fetch", 9)) gcc_op = "__atomic_and_fetch";
                else if (aolen == 9 && !memcmp(aop, "xor_fetch", 9)) gcc_op = "__atomic_xor_fetch";

                if (gcc_op) {
                    emit(e, "%s(", gcc_op); emit_rewritten_node(e, node->intrinsic.args[0], func);
                    emit(e, ", "); emit_rewritten_node(e, node->intrinsic.args[1], func);
                    emit(e, ", __ATOMIC_SEQ_CST)");
                }
            }
        } else if (nlen == 4 && memcmp(name, "cstr", 4) == 0 && node->intrinsic.arg_count > 1) {
            /* @cstr(buf, str) — copy string to buffer with null terminator.
             * AUDIT-2026-06-08: previously omitted bounds check ("Simplified:
             * memcpy + null. Full version has bounds check + auto-return.")
             * — silent stack buffer overflow when source.len + 1 > buf size.
             * The AST sibling at line 3174 has the check; the IR path (used
             * for all function bodies since 2026-04-19) was the silent gap.
             * Fix: hoist source slice, hoist destination via pointer, check
             * `_zer_cs.len + 1 > capacity` and trap on overflow. Capacity is
             * `sizeof(*buf)` for array destinations and `_zer_cd.len` for
             * slice destinations. */
            int t = e->temp_count++;
            Type *buf_type = checker_get_type(e->checker, node->intrinsic.args[0]);
            TypeKind buf_k = type_dispatch_kind(buf_type);
            Type *buf_eff = buf_type ? type_unwrap_distinct(buf_type) : NULL;
            bool dest_is_slice = (buf_k == TYPE_SLICE);
            emit(e, "({ __auto_type _zer_cs%d = ", t);
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            if (dest_is_slice) {
                emit(e, "; __auto_type _zer_cd%d = ", t);
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, "; if (_zer_cs%d.len + 1 > _zer_cd%d.len) "
                       "_zer_trap(\"@cstr buffer overflow\", __FILE__, __LINE__);"
                       " memcpy(_zer_cd%d.ptr, _zer_cs%d.ptr, _zer_cs%d.len);"
                       " ((uint8_t*)_zer_cd%d.ptr)[_zer_cs%d.len] = 0;"
                       " (uint8_t*)_zer_cd%d.ptr; })",
                     t, t, t, t, t, t, t, t);
            } else if (buf_k == TYPE_ARRAY) {
                /* Array destination — known compile-time size */
                emit(e, "; if (_zer_cs%d.len + 1 > %llu) "
                       "_zer_trap(\"@cstr buffer overflow\", __FILE__, __LINE__);"
                       " memcpy(",
                     t, (unsigned long long)buf_eff->array.size);
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ", _zer_cs%d.ptr, _zer_cs%d.len); ((uint8_t*)", t, t);
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ")[_zer_cs%d.len] = 0; (uint8_t*)", t);
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, "; })");
            } else {
                /* Pointer destination (no size info) — no bounds check possible.
                 * Callers using raw `*u8` dest are rejected at checker (Gap 27,
                 * 2026-05-16), so this path is a defensive fallback. */
                emit(e, "; memcpy(");
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ", _zer_cs%d.ptr, _zer_cs%d.len); ((uint8_t*)", t, t);
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, ")[_zer_cs%d.len] = 0; (uint8_t*)", t);
                emit_rewritten_node(e, node->intrinsic.args[0], func);
                emit(e, "; })");
            }
        } else if (nlen == 9 && memcmp(name, "cond_wait", 9) == 0 && node->intrinsic.arg_count >= 2) {
            /* @cond_wait(shared_var, cond) — check pointer vs struct for accessor */
            Type *cvt = checker_get_type(e->checker, node->intrinsic.args[0]);
            const char *ca = (cvt && type_unwrap_distinct(cvt)->kind == TYPE_POINTER) ? "->" : ".";
            emit(e, "({ _zer_mtx_ensure_init_cv(&(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx, &(", ca);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx_inited, &(", ca);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_cond); pthread_mutex_lock(&(", ca);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); while(!(", ca);
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ")) pthread_cond_wait(&(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_cond, &(", ca);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); pthread_mutex_unlock(&(", ca);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); })", ca);
        } else if (nlen == 11 && memcmp(name, "cond_signal", 11) == 0 && node->intrinsic.arg_count >= 1) {
            Type *cst = checker_get_type(e->checker, node->intrinsic.args[0]);
            const char *csa = (cst && type_unwrap_distinct(cst)->kind == TYPE_POINTER) ? "->" : ".";
            emit(e, "({ _zer_mtx_ensure_init_cv(&(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx, &(", csa);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx_inited, &(", csa);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_cond); pthread_mutex_lock(&(", csa);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); pthread_cond_signal(&(", csa);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_cond); pthread_mutex_unlock(&(", csa);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); })", csa);
        } else if (nlen == 14 && memcmp(name, "cond_broadcast", 14) == 0 && node->intrinsic.arg_count >= 1) {
            Type *cbt = checker_get_type(e->checker, node->intrinsic.args[0]);
            const char *cba = (cbt && type_unwrap_distinct(cbt)->kind == TYPE_POINTER) ? "->" : ".";
            emit(e, "({ _zer_mtx_ensure_init_cv(&(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx, &(", cba);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx_inited, &(", cba);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_cond); pthread_mutex_lock(&(", cba);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); pthread_cond_broadcast(&(", cba);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_cond); pthread_mutex_unlock(&(", cba);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); })", cba);
        } else if (nlen == 14 && memcmp(name, "cond_timedwait", 14) == 0 && node->intrinsic.arg_count >= 3) {
            /* @cond_timedwait(shared_var, cond, timeout_ms) → ?void */
            Type *ctt = checker_get_type(e->checker, node->intrinsic.args[0]);
            const char *cta = (ctt && type_unwrap_distinct(ctt)->kind == TYPE_POINTER) ? "->" : ".";
            int t = e->temp_count++;
            emit(e, "({ _zer_mtx_ensure_init_cv(&(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx, &(", cta);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx_inited, &(", cta);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_cond); pthread_mutex_lock(&(", cta);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); struct timespec _zer_ts%d; clock_gettime(CLOCK_REALTIME, &_zer_ts%d); ", cta, t, t);
            emit(e, "{ uint64_t _zer_ms%d = ", t);
            emit_rewritten_node(e, node->intrinsic.args[2], func);
            emit(e, "; _zer_ts%d.tv_sec += _zer_ms%d / 1000; _zer_ts%d.tv_nsec += (_zer_ms%d %% 1000) * 1000000; ", t, t, t, t);
            emit(e, "if (_zer_ts%d.tv_nsec >= 1000000000) { _zer_ts%d.tv_sec++; _zer_ts%d.tv_nsec -= 1000000000; } } ", t, t, t);
            emit(e, "_zer_opt_void _zer_tw%d = {0}; ", t);
            emit(e, "while (!(");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ")) { int _zer_tr%d = pthread_cond_timedwait(&(", t);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_cond, &(", cta);
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx, &_zer_ts%d); if (_zer_tr%d) break; } ", cta, t, t);
            emit(e, "if (");
            emit_rewritten_node(e, node->intrinsic.args[1], func);
            emit(e, ") _zer_tw%d.has_value = 1; ", t);
            emit(e, "pthread_mutex_unlock(&(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")%s_zer_mtx); _zer_tw%d; })", cta, t);
        } else if (nlen == 11 && memcmp(name, "sem_acquire", 11) == 0 && node->intrinsic.arg_count >= 1) {
            Type *sat = checker_get_type(e->checker, node->intrinsic.args[0]);
            bool sa_ptr = sat && type_unwrap_distinct(sat)->kind == TYPE_POINTER;
            emit(e, "_zer_sem_acquire(");
            if (!sa_ptr) emit(e, "&");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")");
        } else if (nlen == 11 && memcmp(name, "sem_release", 11) == 0 && node->intrinsic.arg_count >= 1) {
            Type *srt = checker_get_type(e->checker, node->intrinsic.args[0]);
            bool sr_ptr = srt && type_unwrap_distinct(srt)->kind == TYPE_POINTER;
            emit(e, "_zer_sem_release(");
            if (!sr_ptr) emit(e, "&");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, ")");
        } else if (nlen == 8 && memcmp(name, "try_enum", 8) == 0) {
            emit_try_enum_open(e);
            if (node->intrinsic.arg_count > 0)
                emit_rewritten_node(e, node->intrinsic.args[0], func);
            else emit(e, "0");
            emit_try_enum_close(e, node->intrinsic.type_arg
                                   ? resolve_tynode(e, node->intrinsic.type_arg) : NULL,
                                node->intrinsic.arg_count > 0
                                   ? checker_get_type(e->checker, node->intrinsic.args[0]) : NULL);
        } else if (nlen == 5 && memcmp(name, "probe", 5) == 0 && node->intrinsic.arg_count > 0) {
            emit(e, "_zer_probe((uintptr_t)(");
            emit_rewritten_node(e, node->intrinsic.args[0], func);
            emit(e, "))");
        } else if (nlen == 6 && memcmp(name, "config", 6) == 0) {
            /* @config(key, default) → emit the default value (last arg) */
            if (node->intrinsic.arg_count > 0)
                emit_rewritten_node(e, node->intrinsic.args[node->intrinsic.arg_count - 1], func);
            else
                emit(e, "0");
        } else if (nlen == 4 && memcmp(name, "addc", 4) == 0 && node->intrinsic.arg_count == 3) {
            emit(e, "_zer_do_addc_u64(");
            emit_rewritten_node(e, node->intrinsic.args[0], func); emit(e, ", ");
            emit_rewritten_node(e, node->intrinsic.args[1], func); emit(e, ", ");
            emit_rewritten_node(e, node->intrinsic.args[2], func); emit(e, ")");
        } else if (nlen == 4 && memcmp(name, "subb", 4) == 0 && node->intrinsic.arg_count == 3) {
            emit(e, "_zer_do_subb_u64(");
            emit_rewritten_node(e, node->intrinsic.args[0], func); emit(e, ", ");
            emit_rewritten_node(e, node->intrinsic.args[1], func); emit(e, ", ");
            emit_rewritten_node(e, node->intrinsic.args[2], func); emit(e, ")");
        } else if (nlen == 4 && memcmp(name, "mulw", 4) == 0 && node->intrinsic.arg_count == 2) {
            emit(e, "_zer_do_mulw_u64(");
            emit_rewritten_node(e, node->intrinsic.args[0], func); emit(e, ", ");
            emit_rewritten_node(e, node->intrinsic.args[1], func); emit(e, ")");
        } else {
            /* BUG-993: this is the IR-path TWIN of the AST-path fallback that
             * BUG-767 hardened, and it was left on the pre-BUG-767 silent-`0`
             * form — in the ONLY path function bodies use. An intrinsic added to
             * the checker's dispatch chain without a matching arm here would
             * therefore compile to the literal 0: a privileged register read, an
             * MMIO probe or an atomic would silently become "zero", with no
             * diagnostic at compile time and nothing to notice at run time. That
             * is precisely the failure BUG-767 was raised for. Dead today (every
             * checker-accepted name has an arm above — swept), and it must FAIL
             * LOUD the moment it stops being dead: an undeclared identifier makes
             * GCC name the intrinsic instead of emitting a zero. */
            emit(e, "__zer_intrinsic_%.*s_has_no_IR_emitter_handler", (int)nlen, name);
        }
        return;
    }

    case NODE_SLICE: {
        /* Integer bit extraction: val[hi..lo] → ((val >> lo) & ((1 << (hi-lo+1)) - 1)) */
        Type *obj_type = checker_get_type(e->checker, node->slice.object);
        Type *obj_eff = obj_type ? type_unwrap_distinct(obj_type) : NULL;
        if (obj_eff && type_is_integer(obj_eff) && node->slice.start && node->slice.end) {
            /* Bit extraction: val[hi..lo]. F8: mirror the AST reference — a 64-bit
             * `1ull` mask with a `>=64`/`<=0` clamp and an unsigned cast of a signed
             * source. The prior IR form used a 32-bit `1U << (hi-lo+1)` with no width
             * guard, silently truncating any extract wider than 32 bits and invoking
             * C shift-UB when the extracted width reached 32. Single-eval (object,
             * start, end each emitted once; the `<=0` mask branch subsumes hi<lo). */
            const char *ucast = "";
            switch (obj_eff->kind) {
            case TYPE_I8:  ucast = "(uint8_t)";  break;
            case TYPE_I16: ucast = "(uint16_t)"; break;
            case TYPE_I32: ucast = "(uint32_t)"; break;
            case TYPE_I64: ucast = "(uint64_t)"; break;
            /* Exhaustive (Stage 2 Part B -Wswitch discipline): only signed ints
             * need the unsigned cast; every other kind is a no-op. No default:. */
            case TYPE_VOID: case TYPE_BOOL:
            case TYPE_U8: case TYPE_U16: case TYPE_U32:
            case TYPE_U64: case TYPE_USIZE:
            case TYPE_UINT: case TYPE_SINT:
            case TYPE_F32: case TYPE_F64:
            case TYPE_POINTER: case TYPE_OPTIONAL: case TYPE_SLICE:
            case TYPE_ARRAY: case TYPE_STRUCT: case TYPE_ENUM:
            case TYPE_UNION: case TYPE_FUNC_PTR: case TYPE_OPAQUE:
            case TYPE_POOL: case TYPE_RING: case TYPE_ARENA:
            case TYPE_BARRIER: case TYPE_HANDLE: case TYPE_SLAB:
            case TYPE_SEMAPHORE: case TYPE_DISTINCT:
                break;
            }
            /* #18: guard the POSITION shift on the object's bit width — `obj >>
             * _zer_lo` is C UB when _zer_lo >= width (e.g. u64 >> 64). F8 guarded
             * only the extract-width mask; the unguarded shift was a silent
             * miscompile (violates "shift by >= width = 0"). Mirrors the AST path. */
            int objbits = type_width(obj_eff);
            if (objbits <= 0) objbits = 64;
            int t = e->temp_count++;
            /* BUG-1198: uint64_t positions (see the AST twin). */
            emit(e, "({ uint64_t _zer_hi%d = (uint64_t)(", t);
            emit_rewritten_node(e, node->slice.start, func);
            emit(e, "); uint64_t _zer_lo%d = (uint64_t)(", t);
            emit_rewritten_node(e, node->slice.end, func);
            emit(e, "); uint64_t _zer_w%d = (_zer_hi%d < _zer_lo%d) ? 0 : _zer_hi%d - _zer_lo%d + 1; (((_zer_lo%d >= %d) ? (uint64_t)0 : (%s", t, t, t, t, t, t, objbits, ucast);
            emit_rewritten_node(e, node->slice.object, func);
            emit(e, " >> _zer_lo%d)) & ((_zer_w%d >= 64) ? ~(uint64_t)0 : (_zer_w%d == 0) ? (uint64_t)0 : ((1ull << _zer_w%d) - 1))); })",
                 t, t, t, t);
            return;
        }
        bool obj_slice = obj_eff && obj_eff->kind == TYPE_SLICE;
        /* Single-eval via GCC statement expression: hoist start/end to temps
         * so subtraction (end - start) reuses the temps, not re-evaluates exprs.
         *
         * Silent-gap fix (audit 2026-04-30): also verify `start <= cap` and
         * `end <= cap` to catch:
         *   - arr[start..]   with start > cap → underflow in `cap - start`
         *   - arr[..end]     with end > cap   → no check
         *   - arr[a..b]      with b > cap     → only `a > b` was checked
         * Cap = `obj.len` for slices, `array.size` for arrays. Hoist obj for
         * slices so .len is read once.
         *
         * `bounds_present` decides whether to emit traps. When neither start
         * nor end is given (full slice `arr[..]`), no bounds work to do. */
        bool bounds_present = node->slice.start || node->slice.end;
        if (obj_slice) {
            int t = e->temp_count++;
            emit(e, "({ ");
            /* Hoist obj so .len is single-evaluation (its type matches the
             * slice we're producing — same .ptr / .len shape). */
            if (bounds_present) {
                emit(e, "__typeof__(");
                emit_rewritten_node(e, node->slice.object, func);
                emit(e, ") _zer_so%d = ", t);
                emit_rewritten_node(e, node->slice.object, func);
                emit(e, "; size_t _zer_cap%d = _zer_so%d.len; ", t, t);
            }
            if (node->slice.start) {
                emit(e, "size_t _zer_ss%d = (size_t)(", t);
                emit_rewritten_node(e, node->slice.start, func);
                emit(e, "); ");
            }
            if (node->slice.end) {
                emit(e, "size_t _zer_se%d = (size_t)(", t);
                emit_rewritten_node(e, node->slice.end, func);
                emit(e, "); ");
            }
            /* Trap conditions:
             *   - explicit start && end: start > end (would underflow)
             *   - any explicit end: end > cap
             *   - explicit start (and no end → end := cap): start > cap (= start > end)
             */
            if (node->slice.start && node->slice.end) {
                emit(e, "if (_zer_ss%d > _zer_se%d) _zer_trap(\"slice start > end\", __FILE__, __LINE__); ",
                     t, t);
            }
            if (node->slice.end) {
                emit(e, "if (_zer_se%d > _zer_cap%d) _zer_trap(\"slice end > len\", __FILE__, __LINE__); ",
                     t, t);
            } else if (node->slice.start) {
                emit(e, "if (_zer_ss%d > _zer_cap%d) _zer_trap(\"slice start > len\", __FILE__, __LINE__); ",
                     t, t);
            }
            emit(e, "(");
            emit_type(e, obj_type);
            emit(e, "){ &(");
            if (bounds_present) emit(e, "_zer_so%d", t);
            else emit_rewritten_node(e, node->slice.object, func);
            emit(e, ".ptr)[");
            if (node->slice.start) emit(e, "_zer_ss%d", t);
            else emit(e, "0");
            emit(e, "], ");
            if (node->slice.end && node->slice.start) {
                emit(e, "_zer_se%d - _zer_ss%d", t, t);
            } else if (node->slice.end) {
                emit(e, "_zer_se%d", t);
            } else if (node->slice.start) {
                emit(e, "_zer_cap%d - _zer_ss%d", t, t);
            } else {
                emit_rewritten_node(e, node->slice.object, func);
                emit(e, ".len");
            }
            emit(e, " }; })");
        } else {
            /* Array sub-slice: arr[start..end] → (SliceType){ &arr[start], end-start } */
            Type *arr_type = obj_type ? type_unwrap_distinct(obj_type) : NULL;
            Type *elem = arr_type && arr_type->kind == TYPE_ARRAY ? arr_type->array.inner : NULL;
            if (elem) {
                int t = e->temp_count++;
                unsigned arr_cap = (unsigned)arr_type->array.size;
                emit(e, "({ ");
                if (node->slice.start) {
                    emit(e, "size_t _zer_ss%d = (size_t)(", t);
                    emit_rewritten_node(e, node->slice.start, func);
                    emit(e, "); ");
                }
                if (node->slice.end) {
                    emit(e, "size_t _zer_se%d = (size_t)(", t);
                    emit_rewritten_node(e, node->slice.end, func);
                    emit(e, "); ");
                }
                if (node->slice.start && node->slice.end) {
                    emit(e, "if (_zer_ss%d > _zer_se%d) _zer_trap(\"slice start > end\", __FILE__, __LINE__); ",
                         t, t);
                }
                if (node->slice.end) {
                    emit(e, "if (_zer_se%d > %uu) _zer_trap(\"slice end > len\", __FILE__, __LINE__); ",
                         t, arr_cap);
                } else if (node->slice.start) {
                    emit(e, "if (_zer_ss%d > %uu) _zer_trap(\"slice start > len\", __FILE__, __LINE__); ",
                         t, arr_cap);
                }
                emit(e, "((");
                emit_type(e, type_slice(e->arena, elem));
                emit(e, "){ &(");
                emit_rewritten_node(e, node->slice.object, func);
                emit(e, ")[");
                if (node->slice.start) emit(e, "_zer_ss%d", t);
                else emit(e, "0");
                emit(e, "], ");
                if (node->slice.end && node->slice.start) {
                    emit(e, "_zer_se%d - _zer_ss%d", t, t);
                } else if (node->slice.end) {
                    emit(e, "_zer_se%d", t);
                } else if (node->slice.start) {
                    emit(e, "%uu - _zer_ss%d", arr_cap, t);
                } else {
                    emit(e, "%uu", arr_cap);
                }
                emit(e, " }); })");
            } else {
                /* Truly unknown — emit placeholder */
                emit_unreachable(e, "this slice shape", node);   /* BUG-851 */
            }
        }
        return;
    }

    case NODE_TYPECAST: {
        /* Should be handled by IR_CAST, but in case it reaches here.
         * BUG-845 site 3: this arm is how a DEFER BODY is emitted, so it is a
         * REACHABLE third emission path, not a theoretical fallback — pmytnl
         * reports nearly missing it, and a guard at two of three sites leaves the
         * UB live in exactly the scope that is hardest to notice. */
        Type *t3 = node->typecast.target_type
                     ? resolve_tynode(e, node->typecast.target_type) : NULL;
        Type *s3 = checker_get_type(e->checker, node->typecast.expr);
        if (t3 && f2i_needs_guard(s3 ? type_unwrap_distinct(s3) : NULL,
                                  type_unwrap_distinct(t3))) {
            int tmp = e->temp_count++;
            emit_f2i_open(e, type_unwrap_distinct(s3), tmp);
            emit_rewritten_node(e, node->typecast.expr, func);
            emit_f2i_close(e, t3, tmp);
            return;
        }
        emit(e, "((");
        if (t3) emit_type(e, t3);
        emit(e, ")");
        emit_rewritten_node(e, node->typecast.expr, func);
        emit(e, ")");
        return;
    }

    case NODE_ORELSE: {
        /* BUG-800: SPAWN ARGUMENTS are emitted from RAW AST — they never pass
         * through IR lowering, so pre_lower_orelse never rewrites them and this
         * function is the only emitter that sees them. It had no NODE_ORELSE arm,
         * so `spawn w(none() orelse 3)` fell to the unhandled-kind fallback and
         * emitted a runtime `_zer_trap("compiler bug: ...")`. Loud, but a shipped
         * trap on valid code.
         *
         * Emitted as a GCC statement expression so the optional is evaluated ONCE
         * (the argument may have side effects), matching the AST emitter's shape. */
        Type *ot = checker_get_type(e->checker, node->orelse.expr);
        Type *oe = ot ? type_unwrap_distinct(ot) : NULL;
        if (node->orelse.fallback_is_return || node->orelse.fallback_is_break ||
            node->orelse.fallback_is_continue) {
            /* BUG-942: this arm used to say "the checker rejects it, so reaching
             * this is a compiler bug", and to name a SPAWN argument. Both were
             * wrong. The checker does not reject it, and it was reachable from
             * FOUR raw-AST argument positions, of which spawn was only one:
             * a builtin method's argument (`heap.free_ptr(mh orelse return)`),
             * the universal `free(slice)`, a spawn argument, and an asm operand.
             * The misnamed message sent every one of them looking at spawn.
             *
             * All four now hoist the orelse in ir_lower.c before emission (see
             * BUG-942 there), so this really is unreachable — but it is kept as a
             * backstop, and it now says WHAT it saw rather than guessing WHERE. */
            fprintf(stderr, "compiler bug: an orelse with a control-flow fallback "
                            "reached raw-AST emission un-hoisted, at line %d — see "
                            "BUG-942 in ir_lower.c\n", node->loc.line);
            emit(e, "(_zer_trap(\"un-hoisted orelse control-flow fallback\", "
                 "__FILE__, __LINE__), 0)");
            return;
        }
        if (oe && type_dispatch_kind(oe) == TYPE_OPTIONAL && is_null_sentinel(oe->optional.inner)) {
            /* ?*T — NULL sentinel, no wrapper struct */
            emit(e, "({ __typeof__(");
            emit_rewritten_node(e, node->orelse.expr, func);
            emit(e, ") _zer_oe = ");
            emit_rewritten_node(e, node->orelse.expr, func);
            emit(e, "; _zer_oe ? _zer_oe : ");
            emit_rewritten_node(e, node->orelse.fallback, func);
            emit(e, "; })");
            return;
        }
        emit(e, "({ __typeof__(");
        emit_rewritten_node(e, node->orelse.expr, func);
        emit(e, ") _zer_oe = ");
        emit_rewritten_node(e, node->orelse.expr, func);
        emit(e, "; _zer_oe.has_value ? _zer_oe.value : ");
        emit_rewritten_node(e, node->orelse.fallback, func);
        emit(e, "; })");
        return;
    }

    case NODE_STRUCT_INIT: {
        /* Should be handled by IR_STRUCT_INIT_DECOMP, but fallback */
        Type *si_type = checker_get_type(e->checker, node);
        bool si_arr = e->global_init_depth == 0 &&
                      struct_init_names_array_field(si_type, node);   /* BUG-1157 */
        int si_tmp = si_arr ? e->temp_count++ : 0;
        if (si_arr) {
            emit(e, "({ ");
            emit_type(e, si_type);
            emit(e, " _zer_si%d = ", si_tmp);
        }
        if (si_type) {
            emit(e, "(");
            emit_type(e, si_type);
            emit(e, ")");
        }
        emit(e, "{ ");
        bool si_first = true;
        for (int i = 0; i < node->struct_init.field_count; i++) {
            const char *fname = node->struct_init.fields[i].name;
            uint32_t fname_len = (uint32_t)node->struct_init.fields[i].name_len;
            if (si_arr) {
                Type *aft = struct_field_type_by_name(si_type, fname, fname_len);
                if (aft && type_dispatch_kind(aft) == TYPE_ARRAY) continue;
            }
            if (!si_first) emit(e, ", ");
            si_first = false;
            emit(e, ".%.*s = ", (int)fname_len, fname);
            Node *fval = node->struct_init.fields[i].value;
            /* F21: wrap a scalar into a value-optional field {val,1} (the
             * compound-literal assignment path `c = { .baud = 9600 };`). */
            Type *fv_type = checker_get_type(e->checker, fval);
            Type *wt = struct_init_opt_wrap_type(si_type, fname, fname_len, fv_type);
            Type *vte = fv_type ? type_unwrap_distinct(fv_type) : NULL;
            if (wt) {
                emit(e, "(");
                emit_type(e, wt);
                emit(e, "){ ");
                /* #14 (B): coerce array→slice inside a `?[*]T` field wrap. */
                Type *wt_eff = type_unwrap_distinct(wt);
                Type *wi = (type_dispatch_kind(wt_eff) == TYPE_OPTIONAL && wt_eff->optional.inner)
                           ? type_unwrap_distinct(wt_eff->optional.inner) : NULL;
                if (wi && type_dispatch_kind(wi) == TYPE_SLICE &&
                    vte && type_dispatch_kind(vte) == TYPE_ARRAY)
                    emit_array_as_slice(e, fval, vte, wi);
                else
                    emit_rewritten_node(e, fval, func);
                emit(e, ", 1 }");
            } else {
                /* #14 (B): bare array into a plain [*]T field → coerce to slice. */
                Type *ftype = struct_field_type_by_name(si_type, fname, fname_len);
                Type *slice_tgt = aggregate_slice_coerce_target(ftype, fv_type);
                if (slice_tgt && vte && type_dispatch_kind(vte) == TYPE_ARRAY)
                    emit_array_as_slice(e, fval, vte, slice_tgt);
                else
                    emit_rewritten_node(e, fval, func);
            }
        }
        if (si_first && si_arr) emit(e, "0");
        emit(e, " }");
        if (si_arr) {
            for (int i = 0; i < node->struct_init.field_count; i++) {
                const char *fname = node->struct_init.fields[i].name;
                uint32_t fname_len = (uint32_t)node->struct_init.fields[i].name_len;
                Type *aft = struct_field_type_by_name(si_type, fname, fname_len);
                if (!aft || type_dispatch_kind(aft) != TYPE_ARRAY) continue;
                emit(e, "; memcpy(&_zer_si%d.%.*s, (", si_tmp, (int)fname_len, fname);
                emit_rewritten_node(e, node->struct_init.fields[i].value, func);
                emit(e, "), sizeof(_zer_si%d.%.*s))", si_tmp, (int)fname_len, fname);
            }
            emit(e, "; _zer_si%d; })", si_tmp);
        }
        return;
    }

    default:
        /* AUDIT-LOUD: previously emitted "/ * unhandled node N * /0" which
         * silently miscompiled (a `0` is a valid C expression but the wrong
         * value). Now fails loudly at compiler-time (stderr diagnostic) AND
         * embeds a runtime trap into the emitted C so testing catches the
         * regression instead of producing wrong results. */
        fprintf(stderr, "compiler bug: emit_rewritten_node hit unhandled "
                "node kind %d at line %d (new NODE_ kind added to AST without "
                "emit_rewritten_node handler)\n",
                node->kind, node->loc.line);
        emit(e, "(_zer_trap(\"compiler bug: unhandled NODE kind in "
             "emit_rewritten_node\", __FILE__, __LINE__), 0)");
        return;
    }
}

/* Axis B5 (2026-06-21): find the shared-struct root touched by a defer-body
 * expression so the deferred access can be lock-wrapped. Defer bodies are
 * emitted as RAW AST at the IR_DEFER_FIRE site (not lowered through
 * emit_shared_lock_if_needed), so a `defer g.count = 0;` on a shared `g` was
 * emitted with NO mutex — an unlocked shared write that races other threads.
 * Mirrors ir_lower.c find_shared_root_expr; returns ONLY a genuinely-shared
 * root (so emit_shared_lock_mode never emits a lock on a struct that has no
 * _zer_mtx field, which would be a compile error in the emitted C). The shared
 * mutex is recursive (BUG-473), so wrapping here is safe even if a lock is
 * already held. */
static Node *emit_defer_shared_root(Emitter *e, Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == NODE_FIELD) {
        /* AUDIT-2026-06-28: at each FIELD step, check the OBJECT's type.
         * Defer-body shared access like `defer w.sp.v = X;` where `w.sp`
         * is `*shared S` was silently missed — the walker descended past
         * `w.sp` to `w` (non-shared) and returned NULL. No lock was
         * emitted around the deferred access = silent race at defer fire.
         * Companion to find_shared_root_expr in ir_lower.c. */
        Node *cur = expr;
        while (cur) {
            Node *next;
            if (cur->kind == NODE_FIELD) next = cur->field.object;
            else if (cur->kind == NODE_INDEX) next = cur->index_expr.object;
            else if (cur->kind == NODE_UNARY && cur->unary.op == TOK_STAR) next = cur->unary.operand;
            else break;
            Type *nt = checker_get_type(e->checker, next);
            if (nt) {
                Type *eff = type_unwrap_distinct(nt);
                if (eff->kind == TYPE_STRUCT &&
                    (eff->struct_type.is_shared || eff->struct_type.is_shared_rw))
                    return next;
                if (eff->kind == TYPE_POINTER) {
                    Type *inner = type_unwrap_distinct(eff->pointer.inner);
                    if (inner && inner->kind == TYPE_STRUCT &&
                        (inner->struct_type.is_shared || inner->struct_type.is_shared_rw))
                        return next;
                }
            }
            cur = next;
        }
    }
    /* Recurse via if/else chains (NOT a switch) — mirrors ir_lower.c
     * find_shared_root_expr and avoids the walker-default audit (no default:
     * clause to silently swallow new node kinds). */
    Node *found = NULL;
    if (expr->kind == NODE_BINARY) {
        found = emit_defer_shared_root(e, expr->binary.left);
        if (!found) found = emit_defer_shared_root(e, expr->binary.right);
    } else if (expr->kind == NODE_ASSIGN) {
        found = emit_defer_shared_root(e, expr->assign.target);
        if (!found) found = emit_defer_shared_root(e, expr->assign.value);
    } else if (expr->kind == NODE_CALL) {
        for (int i = 0; i < expr->call.arg_count && !found; i++)
            found = emit_defer_shared_root(e, expr->call.args[i]);
    } else if (expr->kind == NODE_UNARY) {
        found = emit_defer_shared_root(e, expr->unary.operand);
    } else if (expr->kind == NODE_INDEX) {
        found = emit_defer_shared_root(e, expr->index_expr.object);
        if (!found) found = emit_defer_shared_root(e, expr->index_expr.index);
    } else if (expr->kind == NODE_TYPECAST) {
        found = emit_defer_shared_root(e, expr->typecast.expr);
    } else if (expr->kind == NODE_INTRINSIC) {
        /* §E #28 form-coverage: a defer body reading a shared struct through an
         * intrinsic (`@truncate(u32, g.v)`) must still emit the mutex lock at
         * defer-fire — the walker previously returned NULL on NODE_INTRINSIC, so
         * the deferred access was emitted UNLOCKED = silent race. Condvar/barrier/
         * once intrinsics handle their own lock — don't double-wrap. */
        const char *nm = expr->intrinsic.name;
        size_t nlen = expr->intrinsic.name_len;
        bool intrinsic_handles_own_lock =
            (nlen >= 5 && memcmp(nm, "cond_", 5) == 0) ||
            (nlen >= 8 && memcmp(nm, "barrier_", 8) == 0) ||
            (nlen == 4 && memcmp(nm, "once", 4) == 0);
        if (!intrinsic_handles_own_lock) {
            for (int i = 0; i < expr->intrinsic.arg_count && !found; i++)
                found = emit_defer_shared_root(e, expr->intrinsic.args[i]);
        }
    } else if (expr->kind == NODE_ORELSE) {
        /* §E #28: `defer { x = a() orelse g.v; }`. */
        found = emit_defer_shared_root(e, expr->orelse.expr);
        if (!found) found = emit_defer_shared_root(e, expr->orelse.fallback);
    } else if (expr->kind == NODE_SLICE) {
        /* §E #28: `defer { s = g.buf[0..n]; }`. */
        found = emit_defer_shared_root(e, expr->slice.object);
        if (!found) found = emit_defer_shared_root(e, expr->slice.start);
        if (!found) found = emit_defer_shared_root(e, expr->slice.end);
    } else if (expr->kind == NODE_STRUCT_INIT) {
        /* §E #28: `defer { P p = { .x = g.v }; }`. */
        for (int i = 0; i < expr->struct_init.field_count && !found; i++)
            found = emit_defer_shared_root(e, expr->struct_init.fields[i].value);
    }
    return found;
}

/* Emit a single statement from a defer body. Defer bodies are stored as raw
 * AST (NODE_BLOCK or single stmt) — NOT lowered to IR — so the IR-path defer
 * emitter needs a way to translate stmt-level AST into C. Without this
 * helper, statement kinds (NODE_IF / NODE_FOR / NODE_WHILE / nested
 * NODE_BLOCK) would silently hit emit_rewritten_node's default and
 * miscompile to "/ * unhandled node N * /0", dropping the entire statement.
 *
 * Control-flow statements banned in defer per CLAUDE.md
 * (return/break/continue/goto) are still rejected at checker level — this
 * emitter accepts them defensively, but check_stmt will have errored
 * before lowering reached us. */
static void emit_defer_stmt(Emitter *e, Node *s, IRFunc *func) {
    if (!s) return;
    switch (s->kind) {
    case NODE_BLOCK: {
        emit_indent(e);
        emit(e, "{\n");
        e->indent++;
        for (int si = 0; si < s->block.stmt_count; si++) {
            emit_defer_stmt(e, s->block.stmts[si], func);
        }
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        return;
    }
    case NODE_EXPR_STMT:
        if (s->expr_stmt.expr) {
            /* §C #16: defer bodies are raw AST emitted here and never reached the
             * IR auto-guard pre-pass, so an unprovable fixed-array index in a defer
             * (`defer arr[i]=x`) wrote raw (silent OOB). Emit guards in TRAP mode
             * (early-return would re-fire the defer stack). Mirrors the IR gate. */
            e->guard_traps = true;
            emit_auto_guards(e, s->expr_stmt.expr);
            e->guard_traps = false;
            /* Axis B5: lock-wrap a deferred shared-struct access (the IR-path
             * lock-emission doesn't reach raw defer bodies). Write lock if the
             * expression is an assignment, else read lock. Recursive mutex
             * (BUG-473) makes this safe even if a lock is already held. */
            Node *sroot = emit_defer_shared_root(e, s->expr_stmt.expr);
            bool is_w = (s->expr_stmt.expr->kind == NODE_ASSIGN);
            if (sroot) emit_shared_lock_mode(e, sroot, is_w);
            emit_indent(e);
            emit_rewritten_node(e, s->expr_stmt.expr, func);
            emit(e, ";\n");
            if (sroot) emit_shared_unlock(e, sroot);
        }
        return;
    case NODE_RETURN:
        if (s->ret.expr) {
            e->guard_traps = true;
            emit_auto_guards(e, s->ret.expr);
            e->guard_traps = false;
        }
        emit_indent(e);
        emit(e, "return");
        if (s->ret.expr) {
            emit(e, " ");
            emit_rewritten_node(e, s->ret.expr, func);
        }
        emit(e, ";\n");
        return;
    case NODE_ASM:
        emit_indent(e);
        if (s->asm_stmt.is_structured) {
            emit_structured_asm(e, s, func);
        } else {
            emit(e, "__asm__ __volatile__(%.*s);\n",
                 (int)s->asm_stmt.code_len, s->asm_stmt.code);
        }
        return;
    case NODE_IF:
        if (s->if_stmt.cond) {
            e->guard_traps = true;
            emit_auto_guards(e, s->if_stmt.cond);
            e->guard_traps = false;
        }
        emit_indent(e);
        emit(e, "if (");
        if (s->if_stmt.cond) emit_rewritten_node(e, s->if_stmt.cond, func);
        emit(e, ") ");
        if (s->if_stmt.then_body) {
            if (s->if_stmt.then_body->kind == NODE_BLOCK) {
                emit(e, "{\n");
                e->indent++;
                for (int si = 0; si < s->if_stmt.then_body->block.stmt_count; si++) {
                    emit_defer_stmt(e, s->if_stmt.then_body->block.stmts[si], func);
                }
                e->indent--;
                emit_indent(e);
                emit(e, "}");
            } else {
                emit(e, "{ ");
                emit_defer_stmt(e, s->if_stmt.then_body, func);
                emit_indent(e);
                emit(e, "}");
            }
        } else {
            emit(e, "{}");
        }
        if (s->if_stmt.else_body) {
            emit(e, " else ");
            if (s->if_stmt.else_body->kind == NODE_BLOCK) {
                emit(e, "{\n");
                e->indent++;
                for (int si = 0; si < s->if_stmt.else_body->block.stmt_count; si++) {
                    emit_defer_stmt(e, s->if_stmt.else_body->block.stmts[si], func);
                }
                e->indent--;
                emit_indent(e);
                emit(e, "}\n");
            } else if (s->if_stmt.else_body->kind == NODE_IF) {
                /* else-if chain — recurse directly without extra block */
                emit_defer_stmt(e, s->if_stmt.else_body, func);
            } else {
                emit(e, "{ ");
                emit_defer_stmt(e, s->if_stmt.else_body, func);
                emit(e, "}\n");
            }
        } else {
            emit(e, "\n");
        }
        return;
    case NODE_WHILE:
        emit_indent(e);
        emit(e, "while (");
        if (s->while_stmt.cond) emit_rewritten_node(e, s->while_stmt.cond, func);
        emit(e, ") {\n");
        e->indent++;
        if (s->while_stmt.body && s->while_stmt.body->kind == NODE_BLOCK) {
            for (int si = 0; si < s->while_stmt.body->block.stmt_count; si++) {
                emit_defer_stmt(e, s->while_stmt.body->block.stmts[si], func);
            }
        } else if (s->while_stmt.body) {
            emit_defer_stmt(e, s->while_stmt.body, func);
        }
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        return;
    case NODE_FOR:
        /* Best-effort: emit C `for` directly. NODE_FOR's init can be a
         * NODE_VAR_DECL or an expression; step is an expression. */
        emit_indent(e);
        emit(e, "for (");
        if (s->for_stmt.init) {
            if (s->for_stmt.init->kind == NODE_VAR_DECL) {
                Type *t = checker_get_type(e->checker, s->for_stmt.init);
                if (t) emit_type(e, t);
                emit(e, " %.*s",
                     (int)s->for_stmt.init->var_decl.name_len,
                     s->for_stmt.init->var_decl.name);
                if (s->for_stmt.init->var_decl.init) {
                    emit(e, " = ");
                    emit_rewritten_node(e, s->for_stmt.init->var_decl.init, func);
                }
            } else {
                emit_rewritten_node(e, s->for_stmt.init, func);
            }
        }
        emit(e, "; ");
        if (s->for_stmt.cond) emit_rewritten_node(e, s->for_stmt.cond, func);
        emit(e, "; ");
        if (s->for_stmt.step) emit_rewritten_node(e, s->for_stmt.step, func);
        emit(e, ") {\n");
        e->indent++;
        if (s->for_stmt.body && s->for_stmt.body->kind == NODE_BLOCK) {
            for (int si = 0; si < s->for_stmt.body->block.stmt_count; si++) {
                emit_defer_stmt(e, s->for_stmt.body->block.stmts[si], func);
            }
        } else if (s->for_stmt.body) {
            emit_defer_stmt(e, s->for_stmt.body, func);
        }
        e->indent--;
        emit_indent(e);
        emit(e, "}\n");
        return;
    case NODE_VAR_DECL: {
        /* §E #28 form-coverage: lock-wrap deferred shared-struct reads in a
         * var-decl init too (not only NODE_EXPR_STMT). `defer { u32 z = g.v; }`
         * (or the intrinsic-wrapped form) must emit a rdlock, else the deferred
         * read at fire-time races. Read lock; recursive mutex is safe if a lock
         * is already held (BUG-473). */
        Node *sroot = s->var_decl.init ? emit_defer_shared_root(e, s->var_decl.init) : NULL;
        if (sroot) emit_shared_lock_mode(e, sroot, false);
        emit_indent(e);
        Type *t = checker_get_type(e->checker, s);
        if (t) emit_type(e, t);
        emit(e, " %.*s", (int)s->var_decl.name_len, s->var_decl.name);
        if (s->var_decl.init) {
            emit(e, " = ");
            emit_rewritten_node(e, s->var_decl.init, func);
        }
        emit(e, ";\n");
        if (sroot) emit_shared_unlock(e, sroot);
        return;
    }
    case NODE_BREAK:
    case NODE_CONTINUE:
        /* BUG-947: reachable only since the checker stopped banning a break or
         * continue whose TARGET LOOP is nested inside the defer body. The checker's
         * condition is exactly this emitter's precondition: it permits the jump only
         * when a loop was entered after the body began, so a `break;` emitted here
         * always has an enclosing loop emitted by THIS function, in THIS body.
         *
         * Safe against C's binding rule for the same reason: emit_defer_stmt emits
         * only `for` and `while` — it has no switch arm at all — so the nearest
         * enclosing C construct is that loop and nothing else. (A ZER `switch` in a
         * defer body still hits the loud default below; that is the pre-existing
         * gap refactor L removes, not a regression from this change.) */
        emit_indent(e);
        emit(e, s->kind == NODE_BREAK ? "break;\n" : "continue;\n");
        return;
    default:
        /* AUDIT-LOUD: silently miscompiling statement kinds in defer
         * is the original bug class this helper closed. Fail loudly. */
        fprintf(stderr, "compiler bug: emit_defer_stmt has no handler for "
                "node kind %d at line %d\n",
                s->kind, s->loc.line);
        emit_indent(e);
        emit(e, "_zer_trap(\"compiler bug: unsupported stmt kind in defer\", "
             "__FILE__, __LINE__);\n");
        return;
    }
}

/* Emit one IR instruction as C code */
static void emit_ir_inst(Emitter *e, IRInst *inst, IRFunc *func) {
    switch (inst->op) {

    case IR_ASSIGN: {
        /* BUG-1221: re-zero a declaration that executes again (see ir_lower). */
        if (inst->zero_dest && inst->dest_local >= 0) {
            emit(e, "memset((void *)&");
            emit_local_name(e, func, inst->dest_local);
            emit(e, ", 0, sizeof(");
            emit_local_name(e, func, inst->dest_local);
            emit(e, "));\n");
            break;
        }
        if (inst->dest_local >= 0 && inst->expr) {
            IRLocal *dest = &func->locals[inst->dest_local];

            /* Captures now go through IR_COPY (lowered at if-unwrap site).
             * ?void captures skipped at lowering time. No emit_expr needed. */

            /* ?void from void call now handled at lowering time:
             * void call → IR_ASSIGN(void), then IR_LITERAL(kind=6) for {1}. */

            /* Type adaptation between source and dest */
            Type *src_type = checker_get_type(e->checker, inst->expr);
            Type *src_eff = src_type ? type_unwrap_distinct(src_type) : NULL;
            Type *dst_type = dest->type;
            Type *dst_eff = dst_type ? type_unwrap_distinct(dst_type) : NULL;

            /* Array→array assignment: use memcpy (BUG-579: union array variant
             * capture needs this). Must emit BEFORE "dst = " prefix. */
            if (dst_eff && dst_eff->kind == TYPE_ARRAY &&
                src_eff && src_eff->kind == TYPE_ARRAY) {
                const char *sp = func->is_async ? "self->" : "";
                emit_indent(e);
                emit(e, "memcpy(%s%.*s, ", sp, (int)dest->name_len, dest->name);
                emit_rewritten_node(e, inst->expr, func);
                emit(e, ", sizeof(%s%.*s));\n", sp, (int)dest->name_len, dest->name);
                break;
            }

            emit_indent(e);
            if (func->is_async) {
                emit(e, "self->%.*s = ", (int)dest->name_len, dest->name);
            } else {
                emit(e, "%.*s = ", (int)dest->name_len, dest->name);
            }

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
                /* #15 (B): array into ?[*]T → coerce to a {ptr,len} slice literal
                 * (a bare array flattens into .value.ptr/.len, zeroing .len and
                 * defaulting .has_value=0 — a present optional built empty). */
                {
                    Type *aw_inner = dst_eff->optional.inner
                        ? type_unwrap_distinct(dst_eff->optional.inner) : NULL;
                    if (aw_inner && type_dispatch_kind(aw_inner) == TYPE_SLICE &&
                        src_eff && type_dispatch_kind(src_eff) == TYPE_ARRAY)
                        emit_array_as_slice(e, inst->expr, src_type, aw_inner);
                    else
                        emit_rewritten_node(e, inst->expr, func);
                }
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
            /* Emit callee (BUG-1019: guarded when the call is indirect — this is
             * the path that emitted the raw `g_ops[0](...)` through NULL). */
            {
                Node *icallee = (inst->expr && inst->expr->kind == NODE_CALL)
                                  ? inst->expr->call.callee : NULL;
                bool guard = icallee && call_needs_null_funcptr_guard(e, icallee);
                if (guard) {
                    int fpt = e->temp_count++;
                    emit(e, "({ __typeof__(");
                    emit_ir_call_callee(e, inst, func);
                    emit(e, ") _zer_fp%d = ", fpt);
                    emit_ir_call_callee(e, inst, func);
                    emit(e, "; if (!_zer_fp%d) _zer_trap(\"call through a null function "
                            "pointer\", __FILE__, __LINE__); _zer_fp%d; })", fpt, fpt);
                } else {
                    emit_ir_call_callee(e, inst, func);
                }
                emit(e, "(");
            }
            for (int i = 0; i < inst->call_arg_local_count; i++) {
                if (i > 0) emit(e, ", ");
                if (inst->call_arg_locals[i] >= 0) {
                    IRLocal *al = &func->locals[inst->call_arg_locals[i]];
                    Type *at = al->type ? type_unwrap_distinct(al->type) : NULL;
                    /* Check if param expects slice but arg is array → coerce */
                    Type *pt = (callee_ft && (uint32_t)i < callee_ft->func_ptr.param_count) ?
                        type_unwrap_distinct(callee_ft->func_ptr.params[i]) : NULL;
                    /* Optional-value wrap coercion: param `?T` (struct
                     * form, NOT null-sentinel pointer) and arg is the
                     * inner T or a null literal. Lowering doesn't
                     * insert an IR_CAST for this, so the emitter must
                     * wrap. Without this, the emitted C passed `uint32_t`
                     * or `void *` to a `_zer_opt_T` parameter and GCC
                     * rejected the call. Discovered 2026-05-29.
                     *
                     * For null arg (al->type is void* / opaque
                     * pointer): emit { .has_value = 0 }. For value arg:
                     * emit { .value = local, .has_value = 1 }. Note we
                     * suppress wrap if the arg is ALREADY of optional
                     * type (e.g., orelse-chained or optional-returning
                     * call) — in that case the local already holds the
                     * optional struct. */
                    bool pt_is_opt_value = pt && pt->kind == TYPE_OPTIONAL &&
                        !is_null_sentinel(pt->optional.inner);
                    bool at_is_opt = at && at->kind == TYPE_OPTIONAL;
                    if (pt_is_opt_value && !at_is_opt) {
                        emit(e, "(");
                        emit_type(e, pt);
                        emit(e, "){ ");
                        /* Detect null literal: arg type is opaque
                         * pointer (TYPE_POINTER to void/opaque) or the
                         * temp was emitted as `void*`. ir_lower stores
                         * NULL as a kind=4 literal that the emitter
                         * surfaces as `void*` or `0` — check al->type
                         * kind to disambiguate. */
                        if (at && at->kind == TYPE_POINTER) {
                            /* Null pointer literal stored in temp →
                             * emit zero-value optional. */
                            emit(e, ".has_value = 0 }");
                        } else if (at && type_dispatch_kind(at) == TYPE_ARRAY &&
                                   pt->optional.inner &&
                                   type_dispatch_kind(pt->optional.inner) == TYPE_SLICE) {
                            /* #15 (B): array → ?[*]T param — build the {ptr,len}
                             * slice INSIDE the optional (else the bare array
                             * flattens into .value, zeroing .len). Mirrors the
                             * non-optional array→slice coercion just below. */
                            Type *pi = type_unwrap_distinct(pt->optional.inner);
                            emit(e, ".value = (");
                            emit_type(e, pi);
                            emit(e, "){ ");
                            emit_local_name(e, func, inst->call_arg_locals[i]);
                            emit(e, ", %u }, .has_value = 1 }", (unsigned)at->array.size);
                        } else {
                            /* Normal value → wrap with has_value=1.
                             * Use designated initializers to be robust
                             * to field order in _zer_opt_T struct. */
                            emit(e, ".value = ");
                            emit_local_name(e, func, inst->call_arg_locals[i]);
                            emit(e, ", .has_value = 1 }");
                        }
                    } else if (at && pt && at->kind == TYPE_ARRAY && pt->kind == TYPE_SLICE) {
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
                    /* lower_expr returned -1 (void/array passthrough). For arrays,
                     * check the original AST arg — if param wants slice, emit
                     * array→slice coercion directly. */
                    Node *arg_expr = (inst->args && i < inst->arg_count) ? inst->args[i] : NULL;
                    Type *at_ast = arg_expr ? checker_get_type(e->checker, arg_expr) : NULL;
                    Type *at_eff = at_ast ? type_unwrap_distinct(at_ast) : NULL;
                    Type *pt = (callee_ft && (uint32_t)i < callee_ft->func_ptr.param_count) ?
                        type_unwrap_distinct(callee_ft->func_ptr.params[i]) : NULL;
                    if (at_eff && at_eff->kind == TYPE_ARRAY &&
                        pt && pt->kind == TYPE_SLICE && arg_expr) {
                        emit_array_as_slice(e, arg_expr, at_ast, callee_ft->func_ptr.params[i]);
                    } else if (arg_expr) {
                        emit_rewritten_node(e, arg_expr, func);
                    } else {
                        emit(e, "0");
                    }
                }
            }
            emit(e, ")");
        }
        emit(e, ";\n");
        break;
    }

    case IR_BRANCH: {
        /* @once branch: expr is NODE_ONCE, cond_local is -1 — emit atomic CAS
         * exchange so the body runs exactly once across all threads. Uses a
         * file-static uint32_t flag per @once (inst->source_line seeds the id
         * via a static counter).
         *
         * Pattern mirrors emit_stmt NODE_ONCE at emitter.c:3806:
         *   static uint32_t _zer_once_N = 0;
         *   if (!__atomic_exchange_n(&_zer_once_N, 1, __ATOMIC_ACQ_REL)) { body } */
        if (inst->expr && inst->expr->kind == NODE_ONCE && inst->cond_local < 0) {
            /* B4 (BUG-756): 3-state @once with LOSER-WAIT. The flag
             * `_zer_once_<oid>` is declared at function scope (see the pre-scan in
             * emit_regular_func_from_ir) so it is visible both here and at the join
             * block where the winner publishes "done". oid = false_block (bb_skip)
             * id — unique per @once and reachable from both sites.
             * States: 0 untouched, 1 in-progress, 2 done.
             *   winner: CAS 0->1 succeeds -> run body -> (join) store 2 (RELEASE)
             *   loser : CAS fails -> spin-load until ==2 (ACQUIRE) -> skip
             * The ACQUIRE load pairs with the winner's RELEASE store, so a loser
             * never observes the half-constructed state the winner published. */
            /* BUG-1298: the flag is keyed on the @once NODE, so the clones of a
             * defer body (one per fire site) share it; the CAS scratch keeps the
             * block id, which is unique per clone. */
            int oid = emit_once_id(e, inst->expr);
            int xid = inst->false_block;
            emit_indent(e);
            emit(e, "{\n");
            emit_indent(e);
            emit(e, "#if _ZER_HOSTED\n");
            emit_indent(e);
            emit(e, "uint32_t _zer_once_exp_%d = 0;\n", xid);
            emit_indent(e);
            emit(e, "if (__atomic_compare_exchange_n(&_zer_once_%d, &_zer_once_exp_%d, 1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) goto _zer_bb%d;\n",
                 oid, xid, inst->true_block);
            emit_indent(e);
            emit(e, "else { while (__atomic_load_n(&_zer_once_%d, __ATOMIC_ACQUIRE) != 2u) _zer_once_relax(); goto _zer_bb%d; }\n",
                 oid, inst->false_block);
            emit_indent(e);
            emit(e, "#else /* freestanding: non-atomic, single-core safe only (loser does not wait) */\n");
            emit_indent(e);
            emit(e, "if (_zer_once_%d == 0) { _zer_once_%d = 1; goto _zer_bb%d; } else goto _zer_bb%d;\n",
                 oid, oid, inst->true_block, inst->false_block);
            emit_indent(e);
            emit(e, "#endif\n");
            emit_indent(e);
            emit(e, "}\n");
            break;
        }
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

        /* BH-18 #10 (copied from cool-johnson-t8vr3h): a value-returning async
         * (`async u32`, ...) previously emitted `return <user_value>;` with NO
         * state-machine finalization, so subsequent polls re-ran the tail (each
         * poll N>1 re-executed side effects) and `while(poll()==0)` saw the user
         * value instead of the done-flag. Coerce the value-return into the
         * void-async termination pattern: the poll protocol is an `int` done-flag,
         * so emit `self->_zer_state = -1; return 1;` rather than the user value.
         *
         * BUG-863: the user value used to be DISCARDED here, which left
         * `async <non-void>` neither rejected nor retrievable — a silent
         * footgun. It now lands in the state struct's stable `_zer_result`
         * field, which `_zer_async_NAME_result(&task)` reads. The poll protocol
         * is unchanged: still an `int` done-flag, because "is it finished" and
         * "what did it produce" are different questions. */
        if (inst->src1_local >= 0 && func->is_async) {
            Type *aret = e->current_func_ret;
            Type *aret_eff = aret ? type_unwrap_distinct(aret) : NULL;
            if (aret_eff && type_dispatch_kind(aret_eff) != TYPE_VOID) {
                IRLocal *asrc = &func->locals[inst->src1_local];
                Type *asrc_eff = asrc->type ? type_unwrap_distinct(asrc->type) : NULL;
                /* Same optional wrapping the non-async arm below performs. A
                 * value-optional (`?u32`) is a `{value, has_value}` STRUCT, so
                 * assigning the bare local would be a type error; `?void` has
                 * only has_value; a null-sentinel optional (`?*T`) needs no
                 * wrap at all. lower_expr represents `null` as a `*void` local,
                 * which is how the null arm is recognised. */
                bool a_wrap = (type_dispatch_kind(aret_eff) == TYPE_OPTIONAL &&
                               !is_null_sentinel(aret_eff->optional.inner) &&
                               asrc_eff && type_dispatch_kind(asrc_eff) != TYPE_OPTIONAL);
                bool a_null_src = asrc_eff &&
                    type_dispatch_kind(asrc_eff) == TYPE_POINTER &&
                    asrc_eff->pointer.inner &&
                    type_dispatch_kind(asrc_eff->pointer.inner) == TYPE_VOID;
                emit(e, "self->_zer_result = ");
                if (a_wrap && is_void_opt(aret_eff)) {
                    emit(e, "(_zer_opt_void){ %d }", a_null_src ? 0 : 1);
                } else if (a_wrap && a_null_src) {
                    emit(e, "(");
                    emit_type(e, aret_eff);
                    emit(e, "){ 0 }");
                } else if (a_wrap) {
                    emit(e, "(");
                    emit_type(e, aret_eff);
                    emit(e, "){ ");
                    emit_local_name(e, func, inst->src1_local);
                    emit(e, ", 1 }");
                } else {
                    emit_local_name(e, func, inst->src1_local);
                }
                emit(e, "; ");
            }
            emit(e, "self->_zer_state = -1; return 1;\n");
            break;
        }

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
                /* Detect null literal source — lower_expr(NODE_NULL_LIT) creates
                 * a local of type *void as placeholder. Wrapping that into ?T
                 * must emit has_value=0, not 1. */
                bool is_null_src = src_eff && src_eff->kind == TYPE_POINTER &&
                                   src_eff->pointer.inner &&
                                   type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_VOID;
                if (is_void_opt(ret_eff)) {
                    if (is_null_src) {
                        emit(e, "return (_zer_opt_void){ 0 };\n");
                    } else {
                        emit_local_name(e, func, inst->src1_local);
                        emit(e, ";\n");
                        emit_indent(e);
                        emit(e, "return (_zer_opt_void){ 1 };\n");
                    }
                } else if (is_null_src) {
                    emit(e, "return (");
                    emit_type(e, ret_eff);
                    emit(e, "){ 0, 0 };\n");
                } else {
                    emit(e, "return (");
                    emit_type(e, ret_eff);
                    emit(e, "){ ");
                    /* #16b (relaxation 2026-07-20): an array LOCAL (a by-reference
                     * u8[N] param) returned as ?[*]T → build the {ptr,len} slice
                     * INSIDE the optional (else the decayed pointer is stored bare
                     * into .value and GCC rejects the type). */
                    Type *ri = ret_eff->optional.inner
                        ? type_unwrap_distinct(ret_eff->optional.inner) : NULL;
                    if (src_eff && type_dispatch_kind(src_eff) == TYPE_ARRAY &&
                        ri && type_dispatch_kind(ri) == TYPE_SLICE) {
                        emit(e, "(");
                        emit_type(e, ri);
                        emit(e, "){ ");
                        emit_local_name(e, func, inst->src1_local);
                        emit(e, ", %u }", (unsigned)src_eff->array.size);
                    } else {
                        emit_local_name(e, func, inst->src1_local);
                    }
                    emit(e, ", 1 };\n");
                }
            } else if (need_unwrap) {
                emit(e, "return ");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ".value;\n");
            } else {
                emit(e, "return ");
                /* #16b (relaxation 2026-07-20): an array LOCAL (a by-reference
                 * u8[N] param) returned as [*]T → build the {ptr,len} slice (else
                 * the decayed pointer is returned bare, GCC type mismatch). */
                if (src_eff && type_dispatch_kind(src_eff) == TYPE_ARRAY &&
                    ret_eff && type_dispatch_kind(ret_eff) == TYPE_SLICE) {
                    emit(e, "(");
                    emit_type(e, ret_eff);
                    emit(e, "){ ");
                    emit_local_name(e, func, inst->src1_local);
                    emit(e, ", %u }", (unsigned)src_eff->array.size);
                } else {
                    emit_local_name(e, func, inst->src1_local);
                }
                emit(e, ";\n");
            }
        } else {
            /* Void or bare return, or array→slice return via expr */
            if (func->is_async) {
                /* BUG-863: a BARE `return;` from a `?void` function means
                 * has_value=1 — that is what the non-async path emits
                 * (`return (_zer_opt_void){ 1 };`). The async twin has to say
                 * the same thing, or `?void` async results always read as null. */
                Type *bret = e->current_func_ret;
                Type *bret_eff = bret ? type_unwrap_distinct(bret) : NULL;
                if (bret_eff && is_void_opt(bret_eff))
                    emit(e, "self->_zer_result = (_zer_opt_void){ 1 }; ");
                emit(e, "self->_zer_state = -1; return 1;\n");
            } else if (inst->expr) {
                /* lower_expr returned -1 but expr was kept (array/void passthrough).
                 * Array→slice coercion at return site: emit slice wrapper. */
                Type *expr_type = checker_get_type(e->checker, inst->expr);
                Type *expr_eff = expr_type ? type_unwrap_distinct(expr_type) : NULL;
                Type *ret = e->current_func_ret;
                Type *ret_eff = ret ? type_unwrap_distinct(ret) : NULL;
                if (expr_eff && ret_eff &&
                    expr_eff->kind == TYPE_ARRAY && ret_eff->kind == TYPE_SLICE) {
                    emit(e, "return ");
                    emit_array_as_slice(e, inst->expr, expr_type, ret);
                    emit(e, ";\n");
                } else if (expr_eff && ret_eff && type_dispatch_kind(expr_eff) == TYPE_ARRAY &&
                           type_dispatch_kind(ret_eff) == TYPE_OPTIONAL && ret_eff->optional.inner &&
                           type_dispatch_kind(ret_eff->optional.inner) == TYPE_SLICE) {
                    /* #16 (B): array → ?[*]T return — build the {ptr,len} slice
                     * INSIDE the optional (else it fell through to a bare `return;`
                     * and the caller silently saw None). Only a global/static array
                     * reaches here (a local array return is rejected as dangling). */
                    Type *ri = type_unwrap_distinct(ret_eff->optional.inner);
                    emit(e, "return (");
                    emit_type(e, ret_eff);
                    emit(e, "){ ");
                    emit_array_as_slice(e, inst->expr, expr_type, ri);
                    emit(e, ", 1 };\n");
                } else {
                    /* Void expression in return — emit side effect, then bare return */
                    emit_rewritten_node(e, inst->expr, func);
                    emit(e, ";\n");
                    emit_indent(e);
                    emit(e, "return;\n");
                }
            } else {
                /* Bare `return;` semantics by return type:
                 *   ?void  → SUCCESS { has_value=1 } (no value form exists, so a
                 *            bare return is the natural "succeeded" statement).
                 *   ?T     → NONE { 0, 0 }. A bare `return;` carries no value, so
                 *            it is the None form (the value form is `return x;`).
                 *            This is what `orelse return;` propagation needs; the
                 *            old `{ 0, 1 }` (Some(0)) was a silent correctness bug
                 *            — the caller saw a spurious value. (limitations.md
                 *            "bare orelse return in a ?T function".)
                 *   ?*T    → null sentinel (None) via emit_return_null below.
                 * Only explicit `return null` also emits None. */
                Type *ret = e->current_func_ret;
                Type *eff = ret ? type_unwrap_distinct(ret) : NULL;
                if (eff && eff->kind == TYPE_OPTIONAL && !is_null_sentinel(eff->optional.inner)) {
                    if (is_void_opt(eff)) {
                        /* orelse-fallback return propagates FAILURE (None);
                         * a standalone bare `return;` is SUCCESS. */
                        emit(e, inst->ret_from_orelse
                                ? "return (_zer_opt_void){ 0 };\n"
                                : "return (_zer_opt_void){ 1 };\n");
                    } else {
                        emit(e, "return (");
                        emit_type(e, eff);
                        emit(e, "){ 0, 0 };\n");
                    }
                } else {
                    emit_return_null(e);
                    emit(e, "\n");
                }
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
            /* BUG-591: case label is the resume entry. Cond expression is
             * RE-EVALUATED on each poll via emit_rewritten_node so updates
             * to globals / fields between polls are visible. If cond_local
             * is set (legacy), fall back to the pre-computed local. */
            emit_indent(e);
            emit(e, "case %d:;\n", e->async_yield_id);
            /* BUG-1292: the condition's auto-guard runs on EVERY evaluation, so it
             * goes AFTER the resume label — emitted before it (the generic
             * per-instruction guard), a resumed poll jumped past the check and
             * indexed out of bounds (ASan global-buffer-overflow). */
            if (inst->expr) emit_auto_guards(e, inst->expr);
            emit_indent(e);
            emit(e, "if (!(");
            if (inst->expr) {
                emit_rewritten_node(e, inst->expr, func);
            } else if (inst->cond_local >= 0) {
                emit_local_name(e, func, inst->cond_local);
            } else {
                emit(e, "1"); /* shouldn't happen */
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
        /* DEAD CODE: ir_lower.c lowers NODE_SPAWN to IR_NOP{expr=spawn_node}
         * (see ir_lower.c:2716), not IR_SPAWN. The IR_NOP handler does the
         * real pthread_create emission via emit_rewritten_node on the
         * carried NODE_SPAWN. This branch is unreachable today; we keep
         * it as a hard error so any future regression that creates an
         * IR_SPAWN node fails LOUDLY rather than emitting a TODO comment
         * that compiles cleanly (silent miscompile). */
        fprintf(stderr,
                "INTERNAL ERROR: emit_ir_inst hit IR_SPAWN — ir_lower lowers "
                "spawn to IR_NOP{expr=NODE_SPAWN}; please report as a bug.\n");
        abort();
    }

    case IR_LOCK: {
        /* BUG-594: IR path shared struct auto-locking. ir_lower.c
         * NODE_BLOCK wraps each statement that touches a shared root
         * with IR_LOCK (before) + IR_UNLOCK (after). inst->expr is
         * the root ident; src2_local encodes write-lock (1) vs
         * read-lock (0) for shared(rw) structs. Reuses the same
         * emit_shared_lock_mode helper the AST path uses. */
        if (inst->expr) {
            emit_shared_lock_mode(e, inst->expr, inst->src2_local != 0);
        }
        break;
    }

    case IR_TRAP:
        /* BUG-957: the guard's early exit where a RETURN is not legal — inside
         * @critical, returning would skip the interrupt re-enable. Same emitted
         * text as the C-level guard used, now with the CFG knowing the path ends. */
        emit_indent(e);
        if (inst->literal_kind == 1)   /* BUG-1222 */
            emit(e, "_zer_trap(\"out-of-bounds access in a function whose return type has "
                    "no zero value (non-null pointer / enum without a 0 variant) — it cannot "
                    "return early\", __FILE__, __LINE__);\n");
        else
            emit(e, "_zer_trap(\"out-of-bounds access inside a held lock, @critical block, @once body, semaphore hold "
                    "or defer cleanup — cannot return without leaking it\", __FILE__, __LINE__);\n");
        break;

    case IR_UNLOCK: {
        if (inst->expr) {
            emit_shared_unlock(e, inst->expr);
        }
        break;
    }

    case IR_POOL_ALLOC: case IR_SLAB_ALLOC: case IR_SLAB_ALLOC_PTR:
    case IR_POOL_FREE: case IR_SLAB_FREE: case IR_SLAB_FREE_PTR:
    case IR_POOL_GET:
    case IR_ARENA_ALLOC: case IR_ARENA_ALLOC_SLICE: case IR_ARENA_RESET:
    case IR_RING_PUSH: case IR_RING_POP: case IR_RING_PUSH_CHECKED: {
        /* Phase 8d: builtin ops no longer created by lowering — pool/slab/
         * ring/arena method calls flow through IR_ASSIGN/IR_CALL.
         * AUDIT-LOUD: emit a runtime trap so any future lowering regression
         * that starts emitting these opcodes fails at runtime instead of
         * silently dropping operations. */
        fprintf(stderr, "compiler bug: emit_ir_inst hit dormant builtin "
                "op %d — ir_lower started emitting it without updating "
                "the emitter handler\n", inst->op);
        emit_indent(e);
        emit(e, "_zer_trap(\"compiler bug: dormant builtin IR op %d emitted\", "
             "__FILE__, __LINE__);\n", inst->op);
        break;
    }

    case IR_CRITICAL_BEGIN: {
        e->noreturn_scope_depth++;   /* BUG-835: a return here leaves interrupts OFF */
        emit_indent(e);
        emit(e, "{ /* @critical */\n");
        e->indent++;
        emit_indent(e);
        /* BUG-1020 (2026-09-15): the ARM and RISC-V arms used to key on the ARCH
         * macro ALONE, while x86 was gated on `!_ZER_HOSTED`. That asymmetry was
         * recorded as deliberate ("ARM, RISC-V and AVR have always emitted the
         * correct interrupt-disable sequence regardless of __STDC_HOSTED__") —
         * true for BARE METAL, and wrong for hosted, where the correct sequence
         * is the fence because user mode cannot mask interrupts. Measured with
         * real cross-toolchains rather than argued:
         *
         *   aarch64-linux-gnu-gcc : BUILD FAILS — `mrs x0, primask` / `cpsid i`
         *                           are ARMv7-M encodings that do not exist on
         *                           ARMv8-A. So `@critical` could not be compiled
         *                           at all for 64-bit Raspberry Pi OS, Apple
         *                           silicon Linux, Graviton, …
         *   arm-linux-gnueabihf   : BUILD FAILS — "selected processor does not
         *                           support requested special purpose register"
         *                           (PRIMASK is M-profile only).
         *   riscv64-linux-gnu-gcc : BUILDS CLEAN — and `csrrci mstatus` is a
         *                           MACHINE-mode CSR, so it faults at run time in
         *                           user mode. That is the silent one.
         *
         * PRIMASK exists only on ARM M-profile, so the M arm is selected by
         * PROFILE rather than by hosted-ness — that is the precise fact, and it
         * keeps Cortex-M bare metal working whether or not the build passes
         * -ffreestanding. A/R-profile and aarch64 bare metal get their own
         * correct sequences (CPSR + `cpsid i`, and DAIF + `msr daifset, #2`),
         * which previously did not build either.
         *
         * AVR is deliberately NOT gated on _ZER_HOSTED: avr-gcc reports
         * __STDC_HOSTED__ == 1 by default, so gating it would silently downgrade
         * every AVR build that does not pass -ffreestanding to a fence — the exact
         * regression this arm's x86 sibling was written to fix.
         *
         * A7-6 (2026-07-03): the "memory" clobber makes the interrupt-disable a
         * COMPILER barrier — without it GCC may hoist non-volatile loads/stores
         * across `cpsid i`/`cli`/`csrrci`, defeating the critical section for
         * anything the volatile-global rule misses. */
        emit(e, "#if defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')\n");
        emit_indent(e);
        emit(e, "uint32_t _zer_primask; __asm__ __volatile__(\"mrs %%0, primask\\n cpsid i\" : \"=r\"(_zer_primask) :: \"memory\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__aarch64__) && (!_ZER_HOSTED)\n");
        emit_indent(e);
        emit(e, "uint64_t _zer_daif; __asm__ __volatile__(\"mrs %%0, daif\\n\\tmsr daifset, #2\" : \"=r\"(_zer_daif) :: \"memory\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__ARM_ARCH) && (!_ZER_HOSTED)\n");
        emit_indent(e);
        emit(e, "uint32_t _zer_cpsr; __asm__ __volatile__(\"mrs %%0, cpsr\\n\\tcpsid i\" : \"=r\"(_zer_cpsr) :: \"memory\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__AVR__)\n");
        emit_indent(e);
        emit(e, "uint8_t _zer_sreg = SREG; __asm__ __volatile__(\"cli\" ::: \"memory\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__riscv) && (!_ZER_HOSTED)\n");
        emit_indent(e);
        emit(e, "unsigned long _zer_mstatus; __asm__ __volatile__(\"csrrci %%0, mstatus, 8\" : \"=r\"(_zer_mstatus) :: \"memory\");\n");
        emit_indent(e);
        /* Gap 10 fix (2026-05-16): bare-metal x86 (kernel/bootloader)
         * actually CAN disable interrupts via cli/sti — emit the real
         * instructions instead of a useless memory fence. Detected by
         * __STDC_HOSTED__ == 0 (freestanding) + x86 arch macro. On
         * hosted user-mode x86, cli/sti would SIGSEGV (CPL != 0), so
         * the fence-only fallback is correct there. */
        emit(e, "#elif (defined(__x86_64__) || defined(__i386__)) && (!_ZER_HOSTED)\n");
        emit_indent(e);
        emit(e, "uintptr_t _zer_x86_flags; __asm__ __volatile__(\"pushf\\n\\tpop %%0\\n\\tcli\" : \"=r\"(_zer_x86_flags) :: \"memory\");\n");
        emit_indent(e);
        emit(e, "#else\n");
        emit_indent(e);
        emit(e, "/* hosted x86 / unknown arch: cli/sti illegal in user mode — fall back to fence. */\n");
        emit_indent(e);
        emit(e, "__atomic_thread_fence(__ATOMIC_SEQ_CST);\n");
        emit_indent(e);
        emit(e, "#endif\n");
        break;
    }

    case IR_CRITICAL_END: {
        if (e->noreturn_scope_depth > 0) e->noreturn_scope_depth--;   /* BUG-835 */
        emit_indent(e);
        /* BUG-1020: mirrors IR_CRITICAL_BEGIN's cascade exactly — the two must
         * agree arm for arm, or a block saves state one way and restores it
         * another. Restoring the WHOLE saved word (PRIMASK / DAIF / CPSR /
         * mstatus / EFLAGS) rather than unconditionally re-enabling is what makes
         * nesting safe: an inner @critical inside an outer one leaves interrupts
         * disabled on exit, as it must. */
        emit(e, "#if defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"msr primask, %%0\" :: \"r\"(_zer_primask) : \"memory\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__aarch64__) && (!_ZER_HOSTED)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"msr daif, %%0\" :: \"r\"(_zer_daif) : \"memory\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__ARM_ARCH) && (!_ZER_HOSTED)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"msr cpsr_c, %%0\" :: \"r\"(_zer_cpsr) : \"memory\");\n");
        emit_indent(e);
        emit(e, "#elif defined(__AVR__)\n");
        emit_indent(e);
        /* A7-6: empty-asm memory barrier BEFORE re-enabling so section stores
         * complete before interrupts return (SREG= is not a compiler barrier). */
        emit(e, "__asm__ __volatile__(\"\" ::: \"memory\"); SREG = _zer_sreg;\n");
        emit_indent(e);
        emit(e, "#elif defined(__riscv) && (!_ZER_HOSTED)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"csrw mstatus, %%0\" :: \"r\"(_zer_mstatus) : \"memory\");\n");
        emit_indent(e);
        /* Gap 10 fix: restore EFLAGS on bare-metal x86. The push/pop
         * order matches IR_CRITICAL_BEGIN's save/disable sequence. */
        emit(e, "#elif (defined(__x86_64__) || defined(__i386__)) && (!_ZER_HOSTED)\n");
        emit_indent(e);
        emit(e, "__asm__ __volatile__(\"push %%0\\n\\tpopf\" :: \"r\"(_zer_x86_flags) : \"memory\", \"cc\");\n");
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
        /* Fire pending defers in LIFO order.
         * cond_local >= 0: scoped fire from top down to base (cond_local).
         * src2_local == 0 (default): emit bodies + pop.
         * src2_local == 1: emit bodies, no pop (for break/continue/orelse-exits).
         * src2_local == 2: pop only (no emit) — used at loop exit to clean up
         *                  after divergent paths already emitted their fire-no-pop.
         * cond_local == -1: fire all (no pop — function exit). */
        int base = (inst->cond_local >= 0) ? inst->cond_local : 0;
        /* src2_local: 0 = emit+pop, 1 = emit, no pop, 2 = pop only */
        bool pop = (inst->cond_local >= 0) && (inst->src2_local != 1);
        /* BUG-959/961: the defer BODIES are lowered into the IR — ir_lower splices a
         * clone at this fire point — with ONE exception: a body under the
         * cleanup-label GUARD keeps the raw-AST path, because making that guard a
         * visible branch defeats leak analysis (see materialise_defer_body). So emit
         * exactly the guarded bodies here and nothing else.
         *
         * IR_DEFER_PUSH / IR_DEFER_FIRE stay regardless: ir_validate checks their
         * balance, and e->defer_stack still feeds emit_defers_from at the few
         * remaining C-level guard exits, whose early return leaves the function
         * without passing through any IR fire point. */
        bool emit_bodies = (inst->src2_local != 2);
        if (!emit_bodies || !inst->defer_fire_emit_ast) {
            if (pop) e->defer_stack.count = base;
            break;
        }
        /* capture-on-FIRE (plt86m defer-goto): emit THIS fire's own snapshot of
         * live defer bodies (LIFO: index high = newest defer = first), NOT a
         * replay of the shared mutable defer_stack in block-ID order — that
         * replay dropped a sibling fall-through fire after a goto-path fire
         * popped the stack. The defer_stack push/pop bookkeeping (below) stays
         * for ir_validate balance but is no longer READ for body emission.
         * emit_defer_stmt handles NODE_BLOCK + every legit defer-body kind. */
        for (int di = inst->defer_fire_body_count - 1; di >= 0; di--) {
            Node *db = inst->defer_fire_bodies[di];
            if (!db) continue;
            /* both-reachable cleanup-label guard (plt86m defer-goto): a body
             * whose ORIGINAL defer depth (base+di) is < guard_below was fired
             * EAGERLY by a goto (which set the flag), so emit it as
             * `if (!flag) { body }` — skipped on the goto path, fired on the
             * fall-through path (flag still 0) AT this return, after eval. */
            int depth = base + di;
            bool guarded = (inst->defer_fire_guard_flag >= 0) &&
                           (depth < inst->defer_fire_guard_below);
            /* F2 (2026-08-03): ARMED gate. A forward `goto` can jump OVER this
             * defer's registration to a label past it; the compile-time defer
             * stack still lists it as pending, so this fire ran a body whose
             * registration never executed — `rel()` without `acq()`, a lock
             * underflow, silent. The flag is set where the defer REGISTERS
             * (ir_lower NODE_DEFER) and tested here, so it is right regardless
             * of how control reached this fire.
             *
             * INDEPENDENT of `guarded`, and they nest rather than combine:
             *   guarded  = "a goto already fired this eagerly, skip it"
             *   armed    = "this registration never ran, skip it"
             * Opposite polarities, both may apply to one fire. */
            int armed_flag = inst->defer_fire_flags ? inst->defer_fire_flags[di] : -1;
            if (guarded) {
                emit_indent(e);
                emit(e, "if (!");
                emit_local_name(e, func, inst->defer_fire_guard_flag);
                emit(e, ") {\n");
                e->indent++;
            }
            if (armed_flag >= 0) {
                emit_indent(e);
                emit(e, "if (");
                emit_local_name(e, func, armed_flag);
                emit(e, ") {\n");
                e->indent++;
            }
            emit_defer_body(e, func, db);
            if (armed_flag >= 0) {
                e->indent--;
                emit_indent(e);
                emit(e, "}\n");
            }
            if (guarded) {
                e->indent--;
                emit_indent(e);
                emit(e, "}\n");
            }
        }
        if (pop) e->defer_stack.count = base;
        break;
    }

    case IR_INTRINSIC: {
        /* IR_INTRINSIC no longer created by lowering — all flow through
         * IR_ASSIGN. AUDIT-LOUD: emit a runtime trap so any future
         * lowering regression is caught at runtime rather than silently
         * dropping the operation. */
        fprintf(stderr, "compiler bug: emit_ir_inst hit IR_INTRINSIC — "
                "ir_lower started emitting it without updating the emitter "
                "handler\n");
        emit_indent(e);
        emit(e, "_zer_trap(\"compiler bug: dormant IR_INTRINSIC emitted\", "
             "__FILE__, __LINE__);\n");
        break;
    }

    case IR_NOP:
        /* ASM, spawn, or switch pass-through */
        if (inst->expr) {
            if (inst->expr->kind == NODE_ASM) {
                /* Inline assembly — emit directly */
                Node *a = inst->expr;
                if (a->asm_stmt.is_structured) {
                    emit_indent(e);
                    emit_structured_asm(e, a, func);
                } else {
                    emit_indent(e);
                    emit(e, "__asm__ __volatile__(%.*s);\n",
                         (int)a->asm_stmt.code_len, a->asm_stmt.code);
                }
            } else if (inst->expr->kind == NODE_SPAWN) {
                /* Spawn — emit pthread_create with wrapper struct.
                 * Find pre-scanned spawn wrapper by node pointer. */
                Node *sp = inst->expr;
                int sid = -1;
                for (int wi = 0; wi < e->spawn_wrapper_count; wi++) {
                    if (e->spawn_wrappers[wi].spawn_node == sp) {
                        sid = e->spawn_wrappers[wi].id; break;
                    }
                }
                if (sid >= 0) {
                    int ac = sp->spawn_stmt.arg_count;
                    bool is_scoped = (sp->spawn_stmt.handle_name != NULL);
                    if (is_scoped) {
                        emit_indent(e);
                        emit(e, "pthread_t %.*s;\n",
                             (int)sp->spawn_stmt.handle_name_len, sp->spawn_stmt.handle_name);
                    }
                    emit_indent(e);
                    emit(e, "{ /* spawn %.*s */\n", (int)sp->spawn_stmt.func_name_len, sp->spawn_stmt.func_name);
                    e->indent++;
                    if (ac > 0) {
                        emit_indent(e);
                        emit(e, "struct _zer_spawn_args_%d *_sa = malloc(sizeof(struct _zer_spawn_args_%d));\n", sid, sid);
                        /* Audit 2026-05-26: lock around shared-field reads in
                         * spawn arg expressions. Audit 2026-05-29: also coerce
                         * T → ?T per worker param signature (so `_sa->aN`
                         * matches the wrapper struct's field types which now
                         * follow the param types). */
                        Symbol *worker_sym = scope_lookup(e->checker->global_scope,
                            sp->spawn_stmt.func_name,
                            (uint32_t)sp->spawn_stmt.func_name_len);
                        Type *worker_ft = NULL;
                        if (worker_sym && worker_sym->type) {
                            Type *wt = type_unwrap_distinct(worker_sym->type);
                            if (wt->kind == TYPE_FUNC_PTR) worker_ft = wt;
                        }
                        for (int ai = 0; ai < ac; ai++) {
                            Node *sroot = find_shared_root(e, sp->spawn_stmt.args[ai]);
                            if (sroot) {
                                emit_indent(e);
                                emit(e, "/* shared-read lock for spawn arg %d */\n", ai);
                                emit_shared_lock(e, sroot);
                            }
                            emit_indent(e);
                            emit(e, "_sa->a%d = ", ai);
                            Type *pt = NULL;
                            if (worker_ft && (uint32_t)ai < worker_ft->func_ptr.param_count) {
                                pt = type_unwrap_distinct(worker_ft->func_ptr.params[ai]);
                            }
                            Type *at_ast = checker_get_type(e->checker,
                                sp->spawn_stmt.args[ai]);
                            Type *at_eff = at_ast ? type_unwrap_distinct(at_ast) : NULL;
                            bool pt_is_opt_value = pt && pt->kind == TYPE_OPTIONAL &&
                                !is_null_sentinel(pt->optional.inner);
                            bool at_is_opt = at_eff && at_eff->kind == TYPE_OPTIONAL;
                            if (pt_is_opt_value && !at_is_opt) {
                                emit(e, "(");
                                emit_type(e, pt);
                                emit(e, "){ ");
                                /* null literal arg → has_value=0 */
                                if (sp->spawn_stmt.args[ai]->kind == NODE_NULL_LIT) {
                                    emit(e, ".has_value = 0 }");
                                } else {
                                    emit(e, ".value = ");
                                    emit_rewritten_node(e, sp->spawn_stmt.args[ai], func);
                                    emit(e, ", .has_value = 1 }");
                                }
                            } else if (pt && type_dispatch_kind(pt) == TYPE_SLICE &&
                                       at_eff && type_dispatch_kind(at_eff) == TYPE_ARRAY) {
                                /* BUG-1253: the array -> slice coercion, at the
                                 * spawn-argument sink (was a GCC type error). */
                                emit_array_as_slice(e, sp->spawn_stmt.args[ai], at_eff, pt);
                            } else {
                                emit_rewritten_node(e, sp->spawn_stmt.args[ai], func);
                            }
                            emit(e, ";\n");
                            if (sroot) {
                                emit_shared_unlock(e, sroot);
                            }
                        }
                    }
                    if (is_scoped) {
                        emit_indent(e);
                        emit(e, "pthread_create(&%.*s, NULL, _zer_spawn_wrap_%d, ",
                             (int)sp->spawn_stmt.handle_name_len, sp->spawn_stmt.handle_name, sid);
                    } else {
                        emit_indent(e);
                        emit(e, "{ pthread_t _t; pthread_create(&_t, NULL, _zer_spawn_wrap_%d, ", sid);
                    }
                    emit(e, "%s);\n", ac > 0 ? "(void*)_sa" : "NULL");
                    if (!is_scoped) {
                        emit_indent(e);
                        emit(e, "pthread_detach(_t); }\n");
                    }
                    e->indent--;
                    emit_indent(e);
                    emit(e, "}\n");
                }
            } else if (inst->expr->kind == NODE_SWITCH) {
                /* Enum/union/optional switch — emit if/else chain directly.
                 * Uses emit_rewritten_node for sub-expressions. */
                Node *sw = inst->expr;
                int sw_tmp = e->temp_count++;
                Type *sw_type = checker_get_type(e->checker, sw->switch_stmt.expr);
                Type *sw_eff = sw_type ? type_unwrap_distinct(sw_type) : NULL;
                bool is_union = sw_eff && sw_eff->kind == TYPE_UNION;
                bool is_opt = sw_eff && sw_eff->kind == TYPE_OPTIONAL &&
                              !is_null_sentinel(sw_eff->optional.inner);
                bool is_enum = sw_eff && sw_eff->kind == TYPE_ENUM;

                /* Hoist switch expression.
                 * For unions: use pointer to original so `|*v|` captures can modify it.
                 * For enum/optional: hoist into temp (value semantics fine). */
                emit_indent(e);
                if (is_union) {
                    bool sw_is_rvalue = (sw->switch_stmt.expr->kind == NODE_CALL);
                    if (sw_is_rvalue) {
                        /* rvalue: hoist into temp FIRST, then take its address */
                        emit(e, "{ __typeof__(");
                        emit_rewritten_node(e, sw->switch_stmt.expr, func);
                        emit(e, ") _zer_swt%d = ", sw_tmp);
                        emit_rewritten_node(e, sw->switch_stmt.expr, func);
                        emit(e, ";\n");
                        emit_indent(e);
                        emit(e, "__typeof__(_zer_swt%d) *_zer_sw%d = &_zer_swt%d;\n",
                             sw_tmp, sw_tmp, sw_tmp);
                    } else {
                        emit(e, "{ __typeof__(");
                        emit_rewritten_node(e, sw->switch_stmt.expr, func);
                        emit(e, ") *_zer_sw%d = &(", sw_tmp);
                        emit_rewritten_node(e, sw->switch_stmt.expr, func);
                        emit(e, ");\n");
                    }
                } else {
                    emit(e, "{ __typeof__(");
                    emit_rewritten_node(e, sw->switch_stmt.expr, func);
                    emit(e, ") _zer_sw%d = ", sw_tmp);
                    emit_rewritten_node(e, sw->switch_stmt.expr, func);
                    emit(e, ";\n");
                }
                /* Accessor: unions use `->` (pointer), others use `.` (value) */
                const char *sw_acc = is_union ? "->" : ".";

                for (int ai = 0; ai < sw->switch_stmt.arm_count; ai++) {
                    SwitchArm *arm = &sw->switch_stmt.arms[ai];
                    emit_indent(e);
                    if (arm->is_default) {
                        if (ai > 0) emit(e, "else ");
                        emit(e, "{\n");
                    } else {
                        if (ai > 0) emit(e, "else ");
                        emit(e, "if (");
                        if (is_union) {
                            emit(e, "_zer_sw%d%s_tag == %d", sw_tmp, sw_acc, ai);
                        } else if (is_opt) {
                            /* Optional: first arm = has_value, second = !has_value (or vice versa) */
                            if (arm->value_count > 0 && arm->values[0]->kind == NODE_NULL_LIT) {
                                emit(e, "!_zer_sw%d.has_value", sw_tmp);
                            } else {
                                emit(e, "_zer_sw%d.has_value", sw_tmp);
                            }
                        } else if (is_enum && arm->is_enum_dot) {
                            /* Enum: .variant → compare against _ZER_EnumName_variant */
                            const char *ename = sw_eff->enum_type.name;
                            uint32_t elen = sw_eff->enum_type.name_len;
                            for (int vi = 0; vi < arm->value_count; vi++) {
                                if (vi > 0) emit(e, " || ");
                                emit(e, "_zer_sw%d == ", sw_tmp);
                                /* Emit _ZER_EnumName_variant */
                                /* Arm value is NODE_IDENT with variant name */
                                if (arm->values[vi]->kind == NODE_IDENT) {
                                    if (sw_eff->enum_type.module_prefix) {
                                        emit(e, "_ZER_%.*s__%.*s_%.*s",
                                             (int)sw_eff->enum_type.module_prefix_len,
                                             sw_eff->enum_type.module_prefix,
                                             (int)elen, ename,
                                             (int)arm->values[vi]->ident.name_len,
                                             arm->values[vi]->ident.name);
                                    } else {
                                        emit(e, "_ZER_%.*s_%.*s",
                                             (int)elen, ename,
                                             (int)arm->values[vi]->ident.name_len,
                                             arm->values[vi]->ident.name);
                                    }
                                } else {
                                    emit_rewritten_node(e, arm->values[vi], func);
                                }
                            }
                        } else {
                            /* Integer/other values */
                            for (int vi = 0; vi < arm->value_count; vi++) {
                                if (vi > 0) emit(e, " || ");
                                emit(e, "_zer_sw%d == ", sw_tmp);
                                emit_rewritten_node(e, arm->values[vi], func);
                            }
                        }
                        emit(e, ") {\n");
                    }
                    e->indent++;

                    /* Capture */
                    if (arm->capture_name) {
                        emit_indent(e);
                        if (is_union && arm->capture_is_ptr) {
                            /* Mutable union capture: Type *v = &_sw->variant (pointer to ORIGINAL) */
                            if (arm->value_count > 0 && arm->values[0]->kind == NODE_IDENT) {
                                emit(e, "__typeof__(_zer_sw%d%s%.*s) *%.*s = &_zer_sw%d%s%.*s;\n",
                                     sw_tmp, sw_acc, (int)arm->values[0]->ident.name_len,
                                     arm->values[0]->ident.name,
                                     (int)arm->capture_name_len, arm->capture_name,
                                     sw_tmp, sw_acc, (int)arm->values[0]->ident.name_len,
                                     arm->values[0]->ident.name);
                            }
                        } else if (is_union) {
                            /* Immutable union capture: copy variant value.
                             * Arrays can't be assigned — use memcpy. */
                            if (arm->value_count > 0 && arm->values[0]->kind == NODE_IDENT) {
                                emit(e, "__typeof__(_zer_sw%d%s%.*s) %.*s; memcpy(&%.*s, &_zer_sw%d%s%.*s, sizeof(%.*s));\n",
                                     sw_tmp, sw_acc, (int)arm->values[0]->ident.name_len,
                                     arm->values[0]->ident.name,
                                     (int)arm->capture_name_len, arm->capture_name,
                                     (int)arm->capture_name_len, arm->capture_name,
                                     sw_tmp, sw_acc, (int)arm->values[0]->ident.name_len,
                                     arm->values[0]->ident.name,
                                     (int)arm->capture_name_len, arm->capture_name);
                            }
                        } else if (is_opt) {
                            /* Optional capture: unwrap .value */
                            if (is_void_opt(sw_eff)) {
                                /* ?void: no capture value */
                            } else {
                                emit(e, "__typeof__(_zer_sw%d.value) %.*s = _zer_sw%d.value;\n",
                                     sw_tmp, (int)arm->capture_name_len, arm->capture_name,
                                     sw_tmp);
                            }
                        } else {
                            /* Enum/integer capture */
                            emit(e, "__typeof__(_zer_sw%d) %.*s = _zer_sw%d;\n",
                                 sw_tmp, (int)arm->capture_name_len, arm->capture_name,
                                 sw_tmp);
                        }
                    }

                    /* Arm body — emit statements directly.
                     * Save defer_stack.count so we can fire+pop arm-scoped defers at arm end
                     * (defers declared inside arm reference arm-local vars; must not leak). */
                    int arm_defer_base = e->defer_stack.count;
                    if (arm->body) {
                        if (arm->body->kind == NODE_BLOCK) {
                            for (int si = 0; si < arm->body->block.stmt_count; si++) {
                                Node *bs = arm->body->block.stmts[si];
                                if (bs->kind == NODE_EXPR_STMT && bs->expr_stmt.expr) {
                                    emit_indent(e);
                                    emit_rewritten_node(e, bs->expr_stmt.expr, func);
                                    emit(e, ";\n");
                                } else if (bs->kind == NODE_RETURN) {
                                    emit_indent(e);
                                    /* Check if function returns optional → wrap */
                                    Type *fret = e->current_func_ret;
                                    Type *fret_eff = fret ? type_unwrap_distinct(fret) : NULL;
                                    if (bs->ret.expr && fret_eff &&
                                        fret_eff->kind == TYPE_OPTIONAL &&
                                        !is_null_sentinel(fret_eff->optional.inner)) {
                                        Type *expr_type = checker_get_type(e->checker, bs->ret.expr);
                                        if (expr_type && !type_is_optional(expr_type)) {
                                            if (is_void_opt(fret_eff)) {
                                                emit_rewritten_node(e, bs->ret.expr, func);
                                                emit(e, ";\n");
                                                emit_indent(e);
                                                emit(e, "return (_zer_opt_void){ 1 };\n");
                                            } else {
                                                emit(e, "return (");
                                                emit_type(e, fret_eff);
                                                emit(e, "){ ");
                                                emit_rewritten_node(e, bs->ret.expr, func);
                                                emit(e, ", 1 };\n");
                                            }
                                        } else if (bs->ret.expr->kind == NODE_NULL_LIT) {
                                            emit(e, "return ");
                                            emit_opt_null_literal(e, fret_eff);
                                            emit(e, ";\n");
                                        } else {
                                            emit(e, "return ");
                                            emit_rewritten_node(e, bs->ret.expr, func);
                                            emit(e, ";\n");
                                        }
                                    } else {
                                        emit(e, "return");
                                        if (bs->ret.expr) {
                                            emit(e, " ");
                                            emit_rewritten_node(e, bs->ret.expr, func);
                                        }
                                        emit(e, ";\n");
                                    }
                                } else if (bs->kind == NODE_BREAK) {
                                    /* break in switch → just end arm (no C break needed, using if/else) */
                                } else if (bs->kind == NODE_VAR_DECL) {
                                    /* Variable declaration inside switch arm.
                                     * NODE_ORELSE init requires special emission: the fallback
                                     * may be `return`/`break`/`continue` which can't be inside
                                     * a compound literal assignment expression. */
                                    Type *vt = checker_get_type(e->checker, bs);
                                    if (bs->var_decl.init && bs->var_decl.init->kind == NODE_ORELSE) {
                                        Node *or_node = bs->var_decl.init;
                                        Type *opt_type = checker_get_type(e->checker, or_node->orelse.expr);
                                        Type *opt_eff = opt_type ? type_unwrap_distinct(opt_type) : NULL;
                                        bool nullsent = opt_eff && opt_eff->kind == TYPE_OPTIONAL &&
                                                        is_null_sentinel(opt_eff->optional.inner);
                                        int tmp = e->temp_count++;
                                        /* Declare target */
                                        emit_indent(e);
                                        if (vt) emit_type_and_name(e, vt, bs->var_decl.name, bs->var_decl.name_len);
                                        else emit(e, "uint32_t %.*s", (int)bs->var_decl.name_len, bs->var_decl.name);
                                        emit(e, " = {0};\n");
                                        /* Emit: __typeof__(expr) tmp = expr; if (!tmp.has_value) { fallback } target = tmp.value; */
                                        emit_indent(e);
                                        emit(e, "{ __typeof__(");
                                        emit_rewritten_node(e, or_node->orelse.expr, func);
                                        emit(e, ") _zer_or%d = ", tmp);
                                        emit_rewritten_node(e, or_node->orelse.expr, func);
                                        emit(e, "; if (");
                                        if (nullsent) emit(e, "!_zer_or%d", tmp);
                                        else emit(e, "!_zer_or%d.has_value", tmp);
                                        emit(e, ") { ");
                                        if (or_node->orelse.fallback_is_return) {
                                            /* Fire pending defers (including outer ones), then return appropriate value */
                                            Type *fret = e->current_func_ret;
                                            Type *fret_eff = fret ? type_unwrap_distinct(fret) : NULL;
                                            /* Fire defers BEFORE return (skip arm-scoped since we're jumping out) */
                                            for (int di = e->defer_stack.count - 1; di >= 0; di--) {
                                                Node *db = e->defer_stack.stmts[di];
                                                if (db && db->kind == NODE_EXPR_STMT && db->expr_stmt.expr) {
                                                    emit_rewritten_node(e, db->expr_stmt.expr, func); emit(e, "; ");
                                                }
                                            }
                                            emit(e, "return");
                                            if (fret_eff) {
                                                if (fret_eff->kind == TYPE_OPTIONAL) {
                                                    emit(e, " "); emit_opt_null_literal(e, fret_eff);
                                                } else if (fret_eff->kind != TYPE_VOID) {
                                                    emit(e, " 0");
                                                }
                                            }
                                            emit(e, "; ");
                                        } else if (or_node->orelse.fallback && or_node->orelse.fallback->kind != NODE_BLOCK) {
                                            /* Value fallback: assign fallback to target */
                                            emit(e, "%.*s = ", (int)bs->var_decl.name_len, bs->var_decl.name);
                                            emit_rewritten_node(e, or_node->orelse.fallback, func);
                                            emit(e, "; goto _zer_arm_done%d; ", tmp);
                                        }
                                        emit(e, "} ");
                                        /* Success: assign .value (or plain for null sentinel) */
                                        emit(e, "%.*s = ", (int)bs->var_decl.name_len, bs->var_decl.name);
                                        if (nullsent) emit(e, "_zer_or%d", tmp);
                                        else emit(e, "_zer_or%d.value", tmp);
                                        emit(e, "; }\n");
                                        if (or_node->orelse.fallback && or_node->orelse.fallback->kind != NODE_BLOCK &&
                                            !or_node->orelse.fallback_is_return) {
                                            emit(e, "_zer_arm_done%d:;\n", tmp);
                                        }
                                    } else {
                                        emit_indent(e);
                                        if (vt) emit_type_and_name(e, vt, bs->var_decl.name, bs->var_decl.name_len);
                                        else emit(e, "uint32_t %.*s", (int)bs->var_decl.name_len, bs->var_decl.name);
                                        if (bs->var_decl.init) {
                                            emit(e, " = ");
                                            emit_rewritten_node(e, bs->var_decl.init, func);
                                        } else {
                                            emit(e, " = {0}");
                                        }
                                        emit(e, ";\n");
                                    }
                                } else if (bs->kind == NODE_DEFER) {
                                    /* Defer inside switch arm — push to defer stack */
                                    if (e->defer_stack.count >= e->defer_stack.capacity) {
                                        int nc = e->defer_stack.capacity * 2;
                                        if (nc < 16) nc = 16;
                                        Node **ns = (Node **)malloc(nc * sizeof(Node *));
                                        if (e->defer_stack.stmts) {
                                            memcpy(ns, e->defer_stack.stmts, e->defer_stack.count * sizeof(Node *));
                                            free(e->defer_stack.stmts);
                                        }
                                        e->defer_stack.stmts = ns;
                                        e->defer_stack.capacity = nc;
                                    }
                                    e->defer_stack.stmts[e->defer_stack.count++] = bs->defer.body;
                                } else if (bs->kind == NODE_IF) {
                                    /* If inside switch arm — emit condition + then/else bodies.
                                     * Helper walks a then/else body (block or single stmt). */
                                    #define EMIT_ARM_IF_BODY(body) do { \
                                        Node *_b = (body); \
                                        if (!_b) break; \
                                        if (_b->kind == NODE_BLOCK) { \
                                            for (int _bi = 0; _bi < _b->block.stmt_count; _bi++) { \
                                                Node *_is = _b->block.stmts[_bi]; \
                                                if (!_is) continue; \
                                                if (_is->kind == NODE_EXPR_STMT && _is->expr_stmt.expr) { \
                                                    emit_indent(e); emit_rewritten_node(e, _is->expr_stmt.expr, func); emit(e, ";\n"); \
                                                } else if (_is->kind == NODE_RETURN) { \
                                                    emit_indent(e); emit(e, "return"); \
                                                    if (_is->ret.expr) { emit(e, " "); emit_rewritten_node(e, _is->ret.expr, func); } \
                                                    emit(e, ";\n"); \
                                                } \
                                            } \
                                        } else if (_b->kind == NODE_EXPR_STMT && _b->expr_stmt.expr) { \
                                            emit_indent(e); emit_rewritten_node(e, _b->expr_stmt.expr, func); emit(e, ";\n"); \
                                        } else if (_b->kind == NODE_RETURN) { \
                                            emit_indent(e); emit(e, "return"); \
                                            if (_b->ret.expr) { emit(e, " "); emit_rewritten_node(e, _b->ret.expr, func); } \
                                            emit(e, ";\n"); \
                                        } \
                                    } while (0)
                                    emit_indent(e);
                                    emit(e, "if (");
                                    emit_rewritten_node(e, bs->if_stmt.cond, func);
                                    emit(e, ") {\n");
                                    e->indent++;
                                    EMIT_ARM_IF_BODY(bs->if_stmt.then_body);
                                    e->indent--;
                                    emit_indent(e); emit(e, "}");
                                    if (bs->if_stmt.else_body) {
                                        emit(e, " else {\n");
                                        e->indent++;
                                        EMIT_ARM_IF_BODY(bs->if_stmt.else_body);
                                        e->indent--;
                                        emit_indent(e); emit(e, "}");
                                    }
                                    emit(e, "\n");
                                    #undef EMIT_ARM_IF_BODY
                                } else {
                                    /* Other statement — use emit_rewritten_node */
                                    emit_indent(e);
                                    emit_rewritten_node(e, bs, func);
                                    emit(e, ";\n");
                                }
                            }
                        } else if (arm->body->kind == NODE_EXPR_STMT && arm->body->expr_stmt.expr) {
                            /* Single-expression arm: `Dir.north => result = 1` */
                            emit_indent(e);
                            emit_rewritten_node(e, arm->body->expr_stmt.expr, func);
                            emit(e, ";\n");
                        } else if (arm->body->kind == NODE_RETURN) {
                            emit_indent(e);
                            emit(e, "return");
                            if (arm->body->ret.expr) {
                                emit(e, " ");
                                emit_rewritten_node(e, arm->body->ret.expr, func);
                            }
                            emit(e, ";\n");
                        } else {
                            emit_indent(e);
                            emit_rewritten_node(e, arm->body, func);
                            emit(e, ";\n");
                        }
                    }
                    /* Fire + pop arm-scoped defers — declared inside arm reference
                     * arm-local vars that are out of scope after the arm block ends. */
                    if (e->defer_stack.count > arm_defer_base) {
                        for (int di = e->defer_stack.count - 1; di >= arm_defer_base; di--) {
                            Node *db = e->defer_stack.stmts[di];
                            if (!db) continue;
                            if (db->kind == NODE_EXPR_STMT && db->expr_stmt.expr) {
                                emit_indent(e);
                                emit_rewritten_node(e, db->expr_stmt.expr, func);
                                emit(e, ";\n");
                            } else if (db->kind == NODE_BLOCK) {
                                for (int bi = 0; bi < db->block.stmt_count; bi++) {
                                    Node *bs2 = db->block.stmts[bi];
                                    if (bs2 && bs2->kind == NODE_EXPR_STMT && bs2->expr_stmt.expr) {
                                        emit_indent(e);
                                        emit_rewritten_node(e, bs2->expr_stmt.expr, func);
                                        emit(e, ";\n");
                                    }
                                }
                            }
                        }
                        e->defer_stack.count = arm_defer_base;
                    }

                    e->indent--;
                    emit_indent(e);
                    emit(e, "}\n");
                }
                emit_indent(e);
                emit(e, "}\n");
            } else {
                /* Other passthrough — emit as expression */
                emit_indent(e);
                emit_rewritten_node(e, inst->expr, func);
                emit(e, ";\n");
            }
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
            /* Mutable capture: dst is *T, src is ?T (struct optional).
             * Emit v = &src.value to take pointer to the optional's storage. */
            bool need_addr_capture = (dst && dst->is_capture &&
                                     dst_eff && dst_eff->kind == TYPE_POINTER &&
                                     src_eff && src_eff->kind == TYPE_OPTIONAL &&
                                     !is_null_sentinel(src_eff->optional.inner) &&
                                     /* BUG-1028: a VALUE capture `|v|` of `?*opaque` has
                                      * capture type `*opaque`, which is TYPE_POINTER but
                                      * the C VALUE type _zer_opaque — it is the unwrap
                                      * `v = o.value`, not `&o.value` (which GCC refused,
                                      * so every `if (?*opaque) |v|` failed to compile).
                                      * A `|*v|` capture has pointee `*opaque`, not
                                      * `opaque`, so it still takes the address. */
                                     type_dispatch_kind(dst_eff->pointer.inner) != TYPE_OPAQUE);

            /* BUG-1054: mutable capture bound to the ADDRESS of the optional
             * (a non-local lvalue condition): dst is *T, src is *?T. */
            bool need_addr_capture_via_ptr = (dst && dst->is_capture &&
                                     dst_eff && dst_eff->kind == TYPE_POINTER &&
                                     src_eff && type_dispatch_kind(src_eff) == TYPE_POINTER &&
                                     src_eff->pointer.inner &&
                                     type_dispatch_kind(src_eff->pointer.inner) == TYPE_OPTIONAL &&
                                     !is_null_sentinel(type_unwrap_distinct(src_eff->pointer.inner)->optional.inner));

            const char *sp = func->is_async ? "self->" : "";

            /* Array→array copy: use memcpy (C can't assign arrays).
             * Handle BEFORE emitting "dst = " prefix. */
            bool need_arr_copy = (dst_eff && dst_eff->kind == TYPE_ARRAY &&
                                  src_eff && src_eff->kind == TYPE_ARRAY);
            if (need_arr_copy) {
                emit_indent(e);
                /* BUG-1192: sizeof the DESTINATION — an array PARAMETER source is
                 * a decayed pointer, so sizeof(src) was 8 and `u8[64] c = a;`
                 * copied 8 bytes. The types are equal, so the sizes are. */
                emit(e, "memcpy(%s%.*s, %s%.*s, sizeof(%s%.*s));\n",
                     sp, (int)dst->name_len, dst->name,
                     sp, (int)src->name_len, src->name,
                     sp, (int)dst->name_len, dst->name);
                break;
            }

            emit_indent(e);
            emit(e, "%s%.*s = ", sp, (int)dst->name_len, dst->name);

            /* Detect null literal source — lower_expr(NODE_NULL_LIT) creates
             * a local of type *void as placeholder. Wrapping that into ?T
             * must emit has_value=0, not 1. */
            bool is_null_src = src_eff && src_eff->kind == TYPE_POINTER &&
                               src_eff->pointer.inner &&
                               type_unwrap_distinct(src_eff->pointer.inner)->kind == TYPE_VOID;

            if (need_addr_capture_via_ptr) {
                emit(e, "&%s%.*s->value;\n",
                     sp, (int)src->name_len, src->name);
            } else if (need_addr_capture) {
                emit(e, "&%s%.*s.value;\n",
                     sp, (int)src->name_len, src->name);
            } else if (need_slice) {
                emit(e, "(");
                emit_type(e, dst_eff);
                emit(e, "){ %s%.*s, %u };\n",
                     sp, (int)src->name_len, src->name,
                     src_eff ? (unsigned)src_eff->array.size : 0);
            } else if (need_wrap && is_null_src) {
                if (is_void_opt(dst_eff)) {
                    emit(e, "(_zer_opt_void){ 0 };\n");
                } else {
                    emit(e, "(");
                    emit_type(e, dst_eff);
                    emit(e, "){ 0, 0 };\n");
                }
            } else if (need_wrap) {
                Type *aw_inner = dst_eff->optional.inner
                    ? type_unwrap_distinct(dst_eff->optional.inner) : NULL;
                if (aw_inner && type_dispatch_kind(aw_inner) == TYPE_SLICE &&
                    src_eff && type_dispatch_kind(src_eff) == TYPE_ARRAY) {
                    /* #15 (B): array → ?[*]T assignment — build the {ptr,len} slice
                     * INSIDE the optional (else the bare array flattens into
                     * .value.ptr/.len, zeroing .len and .has_value). Mirrors the
                     * need_slice branch's slice-literal construction. */
                    emit(e, "(");
                    emit_type(e, dst_eff);
                    emit(e, "){ (");
                    emit_type(e, aw_inner);
                    emit(e, "){ %s%.*s, %u }, 1 };\n",
                         sp, (int)src->name_len, src->name,
                         (unsigned)src_eff->array.size);
                } else {
                    emit(e, "(");
                    emit_type(e, dst_eff);
                    emit(e, "){ %s%.*s, 1 };\n",
                         sp, (int)src->name_len, src->name);
                }
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
            case 0: /* int */ {
                /* BUG-592: emit a type-matched literal. Always-ULL emission
                 * made `signed_local < 0ULL` wrong (C promotes signed
                 * operand to unsigned → huge value, never < 0).
                 *
                 * Emit as `(CType)N` so the literal has the target's exact
                 * type. For signed targets, this gives a signed value; for
                 * unsigned, unsigned. Subsequent comparisons with the temp
                 * have matching signedness on both sides. */
                emit(e, "(");
                if (dst->type) emit_type(e, dst->type);
                else emit(e, "uint64_t");
                /* 2026-08-02: print the literal with the DESTINATION's
                 * signedness. `%lld` unconditionally emitted a signed decimal,
                 * so a value >= 2^63 became negative
                 * (0x8000000000000000 -> -9223372036854775808) and the cast
                 * SIGN-EXTENDED it. For a <= 64-bit target that is harmless —
                 * `(uint32_t)-1` wraps to the same bits — but for a uN/iN with a
                 * 128-BIT CARRIER the cast widens instead of wrapping, so
                 * `u100 a = 0x8000000000000000;` set all the high bits. A silent
                 * miscompile: the GLOBAL-INIT path emits `...ULL` and was
                 * correct, so the two paths disagreed on the same literal.
                 *
                 * BUG-592 is preserved: it required a type-MATCHED literal
                 * (always-ULL broke `signed_local < 0ULL`), and a SIGNED
                 * destination still emits %lld here. */
                bool lit_dst_unsigned = dst->type &&
                    type_is_integer(dst->type) && !type_is_signed(dst->type);
                if (lit_dst_unsigned)
                    emit(e, ")%lluULL", (unsigned long long)(uint64_t)inst->literal_int);
                else
                    emit(e, ")%lld", (long long)inst->literal_int);
                break;
            }
            case 1: /* float */
                emit_double_lit(e, inst->literal_float,
                    inst->dest_local >= 0 && inst->dest_local < func->local_count &&
                    emit_type_is_f32(func->locals[inst->dest_local].type));
                break;
            case 2: /* string */
                /* sizeof("...") - 1 so C resolves escapes; source-char
                 * count overcounted (silent OOB on bounds-checked
                 * reads of escape-bearing literals). */
                emit_zer_string_slice(e, inst->literal_str, (int)inst->literal_str_len, false);
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
            default:
                /* Defense in depth: out-of-range literal_kind should never
                 * happen, but if a future lowerer emits one the bare `dest = ;`
                 * would be invalid C. Trap so the regression is visible. */
                fprintf(stderr, "compiler bug: IR_LITERAL with unhandled "
                        "literal_kind %u\n", (unsigned)inst->literal_kind);
                emit(e, "0 /* IR_LITERAL bad kind %u */",
                     (unsigned)inst->literal_kind);
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

            /* Phase 3 fix #5: shift safety. ZER spec: shift by >= width = 0.
             * Use _zer_shl/_zer_shr macros (defined in preamble) instead of
             * raw << / >>, which are C undefined behavior when n >= width. */
            if (inst->op_token == TOK_LSHIFT || inst->op_token == TOK_RSHIFT) {
                emit_indent(e);
                emit(e, "%s%.*s = %s(%s%.*s, %s%.*s, %d);\n",
                     sp, (int)dst->name_len, dst->name,
                     inst->op_token == TOK_LSHIFT ? "_zer_shl" : "_zer_shr",
                     sp, (int)s1->name_len, s1->name,
                     sp, (int)s2->name_len, s2->name,
                     shift_guard_width(s1->type));
                emit_intn_mask(e, dst, sp); /* Path C: wrap uN/iN shift result to its width */
                break;
            }

            /* Phase 3 fix #4: signed division overflow (INT_MIN / -1).
             * C says this is undefined. Trap explicitly when BOTH operands
             * signed and dividend is at the signed min while divisor is -1.
             *
             * Defense-in-depth div-by-zero trap (audit 2026-05-14): the
             * checker's forced-guard at checker.c:2587 only catches IDENT/
             * FIELD/CALL divisors. Index, deref, cast, intrinsic, and
             * compound expressions slip through to raw division, which is
             * a SIGFPE on x86/hosted and silent garbage on ARM/RISC-V
             * baremetal. Mirror compound `/=` (emit_rewritten_node line
             * 5879) and always emit the runtime check here too. */
            if (inst->op_token == TOK_SLASH || inst->op_token == TOK_PERCENT) {
                emit_indent(e);
                emit(e, "if (%s%.*s == 0) "
                        "_zer_trap(\"division by zero\", __FILE__, __LINE__);",
                     sp, (int)s2->name_len, s2->name);
                Type *lt = s1->type ? type_unwrap_distinct(s1->type) : NULL;
                /* BUG-1066: everything that can TRAP stays on the ONE C line the
                 * `#line` directive above names. The zero test used to end with a
                 * newline, so the overflow trap sat on the NEXT emitted line and
                 * reported the source line PLUS ONE (the BUG-1003 class). */
                if (lt && type_is_signed(lt)) {
                    /* BUG-1062: the MIN of THIS width -- an i5, i48 or i128
                     * compared against INT64_MIN before and never trapped. */
                    char minlit[96];
                    signed_min_text(lt, minlit, sizeof minlit);
                    emit(e, " if (%s%.*s == %s && %s%.*s == -1) "
                            "_zer_trap(\"signed division overflow\", __FILE__, __LINE__);",
                         sp, (int)s1->name_len, s1->name, minlit,
                         sp, (int)s2->name_len, s2->name);
                }
                emit(e, " %s%.*s = (%s%.*s %s %s%.*s);\n",
                     sp, (int)dst->name_len, dst->name,
                     sp, (int)s1->name_len, s1->name,
                     inst->op_token == TOK_SLASH ? "/" : "%",
                     sp, (int)s2->name_len, s2->name);
                emit_intn_mask(e, dst, sp); /* BUG-1062: wrap an iN/uN quotient to its width */
                break;
            }

            const char *op = "?";
            switch (inst->op_token) {
            case TOK_PLUS: op = "+"; break; case TOK_MINUS: op = "-"; break;
            case TOK_STAR: op = "*"; break;
            case TOK_AMP: op = "&"; break; case TOK_PIPE: op = "|"; break;
            case TOK_CARET: op = "^"; break;
            case TOK_EQEQ: op = "=="; break; case TOK_BANGEQ: op = "!="; break;
            case TOK_LT: op = "<"; break; case TOK_GT: op = ">"; break;
            case TOK_LTEQ: op = "<="; break; case TOK_GTEQ: op = ">="; break;
            case TOK_AMPAMP: op = "&&"; break; case TOK_PIPEPIPE: op = "||"; break;
            default: break;
            }
            /* BUG-592: cast one side when comparing signed vs unsigned.
             * Without the cast, C promotes the signed operand to unsigned,
             * making e.g. `(int32_t)-5 < (uint32_t)0` evaluate to false
             * (since -5 becomes 0xFFFFFFFB > 0).
             *
             * Pick the signed type when one side is signed: cast the
             * unsigned operand to the signed side's type. This matches
             * ZER's integer semantics where `x < 0` means signed less-than
             * regardless of the `0` literal's IR type. */
            bool is_cmp = (inst->op_token == TOK_LT || inst->op_token == TOK_GT ||
                           inst->op_token == TOK_LTEQ || inst->op_token == TOK_GTEQ ||
                           inst->op_token == TOK_EQEQ || inst->op_token == TOK_BANGEQ);
            Type *t1 = s1->type ? type_unwrap_distinct(s1->type) : NULL;
            Type *t2 = s2->type ? type_unwrap_distinct(s2->type) : NULL;
            bool s1_signed = t1 && type_is_signed(t1);
            bool s2_signed = t2 && type_is_signed(t2);
            emit_indent(e);
            if (is_cmp && s1_signed && !s2_signed) {
                emit(e, "%s%.*s = (%s%.*s %s (", sp, (int)dst->name_len, dst->name,
                     sp, (int)s1->name_len, s1->name, op);
                emit_type(e, s1->type);
                emit(e, ")%s%.*s);\n", sp, (int)s2->name_len, s2->name);
            } else if (is_cmp && !s1_signed && s2_signed) {
                emit(e, "%s%.*s = ((", sp, (int)dst->name_len, dst->name);
                emit_type(e, s2->type);
                emit(e, ")%s%.*s %s %s%.*s);\n",
                     sp, (int)s1->name_len, s1->name, op,
                     sp, (int)s2->name_len, s2->name);
            } else {
                emit(e, "%s%.*s = (%s%.*s %s %s%.*s);\n",
                     sp, (int)dst->name_len, dst->name,
                     sp, (int)s1->name_len, s1->name,
                     op,
                     sp, (int)s2->name_len, s2->name);
            }
            emit_intn_mask(e, dst, sp); /* Path C: wrap uN/iN arithmetic result to its width */
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
                emit(e, ";");
                emit_nonnull_local_check(e, func, inst->dest_local); /* BUG-1152 */
                emit(e, "\n");
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
            /* Path C: wrap uN/iN unary result — negation (-x) and complement
             * (~x) can leave bits above N set in the carrier. (! yields a bool;
             * deref/addr-of already jumped to unop_done.) */
            if (inst->op_token == TOK_MINUS || inst->op_token == TOK_TILDE) {
                const char *usp = func->is_async ? "self->" : "";
                emit_intn_mask(e, &func->locals[inst->dest_local], usp);
            }
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
            emit(e, "%s%.*s;", accessor,
                 (int)inst->field_name_len, inst->field_name);
            emit_nonnull_local_check(e, func, inst->dest_local); /* BUG-1152 */
            emit(e, "\n");
        }
        break;
    }

    case IR_INDEX_READ: {
        /* Emit: dest = src[idx] — from local IDs.
         * Phase 3 fix (Gap 0): restore slice bounds check lost in
         * commit 010ddea when this handler switched from
         * emit_expr(inst->expr) to direct local-ID emission.
         * Arrays: auto_guards separate pass handles them (if VRP
         *   can't prove safety, an `if (i >= N) return;` is
         *   inserted before the access). IR_INDEX_READ emits raw.
         * Slices: no separate pass; bounds check must be emitted
         *   inline here via comma operator form (same shape as
         *   emit_expr TYPE_SLICE branch at emitter.c:2060). */
        if (inst->dest_local >= 0 && inst->src1_local >= 0 && inst->src2_local >= 0) {
            IRLocal *s1 = &func->locals[inst->src1_local];
            Type *st = s1->type ? type_unwrap_distinct(s1->type) : NULL;
            emit_indent(e);
            emit_local_name(e, func, inst->dest_local);
            emit(e, " = ");
            if (st && st->kind == TYPE_SLICE) {
                emit(e, "(_zer_bounds_check((size_t)(");
                emit_local_name(e, func, inst->src2_local);
                emit(e, "), ");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ".len, __FILE__, __LINE__), ");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ".ptr)[");
                emit_local_name(e, func, inst->src2_local);
                emit(e, "];");
            } else {
                emit_local_name(e, func, inst->src1_local);
                emit(e, "[");
                emit_local_name(e, func, inst->src2_local);
                emit(e, "];");
            }
            emit_nonnull_local_check(e, func, inst->dest_local); /* BUG-1152 */
            emit(e, "\n");
        }
        break;
    }

    case IR_FIELD_WRITE: {
        /* Defensive: ir_lower never emits IR_FIELD_WRITE today (field-write
         * statements flow through IR_ASSIGN with the AST node passed via
         * `inst->expr` to emit_rewritten_node, which routes to AST emit_expr
         * with bounds/qualifier/safety wrappers). If it ever starts being
         * emitted, trap here so the regression surfaces as a runtime failure. */
        fprintf(stderr, "INTERNAL: IR_FIELD_WRITE emitted but emitter has no "
                        "handler — file a bug; field writes should flow "
                        "through IR_ASSIGN today.\n");
        emit_indent(e);
        emit(e, "_zer_trap(\"IR_FIELD_WRITE not implemented\", "
                "__FILE__, __LINE__);\n");
        break;
    }

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
                    tid = opaque_type_id(inner);   /* BUG-1166 */
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
                    expected_tid = opaque_type_id(inner);   /* BUG-1166 */
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
            /* To bool: use truthy conversion (!!x), not plain integer cast.
             * In C, `_Bool` has special conversion rules (non-zero → 1), but
             * ZER emits bool as uint8_t so a plain `(uint8_t)5` gives 5, not 1.
             * BUG-586: test expects `(bool)5 == true`. */
            else if (tgt_eff && tgt_eff->kind == TYPE_BOOL && inst->src1_local >= 0 &&
                     src_eff && (type_is_integer(src_eff) || type_is_float(src_eff) ||
                                 src_eff->kind == TYPE_POINTER)) {
                emit(e, "((uint8_t)!!(");
                emit_local_name(e, func, inst->src1_local);
                emit(e, "))");
            }
            /* Simple C cast */
            else if (inst->src1_local >= 0 && f2i_needs_guard(src_eff, tgt_eff)) {
                int tmp = e->temp_count++;                /* BUG-845 site 2 (IR_CAST) */
                emit_f2i_open(e, src_eff, tmp);
                emit_local_name(e, func, inst->src1_local);
                emit_f2i_close(e, tgt, tmp);
            }
            else if (inst->src1_local >= 0) {
                emit(e, "((");
                emit_type(e, tgt);
                emit(e, ")");
                emit_local_name(e, func, inst->src1_local);
                emit(e, ")");
            }
            emit(e, ";\n");
            /* BUG-946: Path C — a cast to a non-native uN/iN must be wrapped to N
             * bits. The cast emits only the CARRIER cast, and the carrier is the
             * smallest native type >= N, so `(u3)300` emitted `(uint8_t)300` and
             * evaluated to 44 instead of 4 — a silent wrong answer on a valid
             * program, in the class CLAUDE.md lists beside _zer_shl.
             *
             * Measured before fixing: var-decl init, call argument and return were
             * all wrong, while plain assignment and global assignment were already
             * right (they mask after the store) — the TWO-SPELLINGS split again.
             * All three wrong spellings funnel through IR_CAST, so one call here
             * covers them, and it is the SAME emit_intn_mask that IR_BINOP and
             * IR_UNOP use rather than a new expression-position wrapper. */
            if (inst->dest_local >= 0) {
                const char *csp = func->is_async ? "self->" : "";
                emit_intn_mask(e, &func->locals[inst->dest_local], csp);
            }
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
            /* BUG-1157: an ARRAY-typed field cannot be initialised from an
             * array expression inside a C compound literal (the array decays
             * to a pointer: `.arr = l` initialised arr[0] with an address, and
             * a global array `.arr = garr` fell to the `0` fallback). Such
             * fields are left out of the literal (zeroed) and copied by
             * memcpy right after it. */
            bool any_arr_field = false;
            bool first_field = true;
            for (int i = 0; i < inst->expr->struct_init.field_count; i++) {
                const char *fname = inst->expr->struct_init.fields[i].name;
                uint32_t fname_len = (uint32_t)inst->expr->struct_init.fields[i].name_len;
                Type *fty_arr = struct_field_type_by_name(inst->cast_type, fname, fname_len);
                if (fty_arr && type_dispatch_kind(fty_arr) == TYPE_ARRAY) {
                    any_arr_field = true;
                    continue;
                }
                if (!first_field) emit(e, ", ");
                first_field = false;
                emit(e, ".%.*s = ", (int)fname_len, fname);
                if (inst->call_arg_locals && i < inst->call_arg_local_count &&
                    inst->call_arg_locals[i] >= 0) {
                    int vloc = inst->call_arg_locals[i];
                    /* F21: wrap a scalar into a value-optional field {val,1}. */
                    Type *vt = (vloc < func->local_count) ? func->locals[vloc].type : NULL;
                    Type *wt = struct_init_opt_wrap_type(inst->cast_type, fname,
                                                         fname_len, vt);
                    Type *vte = vt ? type_unwrap_distinct(vt) : NULL;
                    if (wt) {
                        emit(e, "(");
                        emit_type(e, wt);
                        emit(e, "){ ");
                        /* #14 (B): coerce array→slice INSIDE a `?[*]T` field wrap —
                         * a bare array flattens into .value.ptr/.len (zeroing .len
                         * and .has_value). Build the {ptr,len} slice from the local
                         * name + array size (the var-decl struct-init DECOMP path). */
                        Type *wt_eff = type_unwrap_distinct(wt);
                        Type *wi = (type_dispatch_kind(wt_eff) == TYPE_OPTIONAL && wt_eff->optional.inner)
                                   ? type_unwrap_distinct(wt_eff->optional.inner) : NULL;
                        if (wi && type_dispatch_kind(wi) == TYPE_SLICE &&
                            vte && type_dispatch_kind(vte) == TYPE_ARRAY) {
                            emit(e, "(");
                            emit_type(e, wi);
                            emit(e, "){ ");
                            emit_local_name(e, func, vloc);
                            emit(e, ", %u }", (unsigned)vte->array.size);
                        } else {
                            emit_local_name(e, func, vloc);
                        }
                        emit(e, ", 1 }");
                    } else {
                        /* #14 (B): bare array into a plain [*]T field → coerce to a
                         * {ptr,len} slice literal (else it brace-flattens, .len=0). */
                        Type *ftype = struct_field_type_by_name(inst->cast_type, fname, fname_len);
                        Type *slice_tgt = aggregate_slice_coerce_target(ftype, vt);
                        if (slice_tgt && vte && type_dispatch_kind(vte) == TYPE_ARRAY) {
                            emit(e, "(");
                            emit_type(e, slice_tgt);
                            emit(e, "){ ");
                            emit_local_name(e, func, vloc);
                            emit(e, ", %u }", (unsigned)vte->array.size);
                        } else {
                            emit_local_name(e, func, vloc);
                        }
                    }
                } else {
                    /* BUG-1157: the value did not lower to a local (a GLOBAL
                     * array, an array FIELD, …). This used to emit a literal
                     * `0` — `R r = { .s = ga };` gave r.s.len == 0, silently.
                     * Emit the rewritten expression itself, with the same
                     * array->slice coercion the local path applies. */
                    Node *fv = inst->expr->struct_init.fields[i].value;
                    Type *fvt = fv ? checker_get_type(e->checker, fv) : NULL;
                    Type *fvte = fvt ? type_unwrap_distinct(fvt) : NULL;
                    Type *ftype = struct_field_type_by_name(inst->cast_type, fname, fname_len);
                    Type *wt = struct_init_opt_wrap_type(inst->cast_type, fname,
                                                         fname_len, fvt);
                    Type *wt_eff = wt ? type_unwrap_distinct(wt) : NULL;
                    Type *wi = (wt_eff && type_dispatch_kind(wt_eff) == TYPE_OPTIONAL &&
                                wt_eff->optional.inner)
                               ? type_unwrap_distinct(wt_eff->optional.inner) : NULL;
                    Type *slice_tgt = wt ? wi : aggregate_slice_coerce_target(ftype, fvt);
                    bool arr_to_slice = slice_tgt && type_dispatch_kind(slice_tgt) == TYPE_SLICE &&
                                        fvte && type_dispatch_kind(fvte) == TYPE_ARRAY;
                    if (wt) { emit(e, "("); emit_type(e, wt); emit(e, "){ "); }
                    if (arr_to_slice) {
                        emit(e, "(");
                        emit_type(e, slice_tgt);
                        emit(e, "){ ");
                        emit_rewritten_node(e, fv, func);
                        emit(e, ", %u }", (unsigned)fvte->array.size);
                    } else if (fv) {
                        emit_rewritten_node(e, fv, func);
                    } else {
                        emit(e, "0");
                    }
                    if (wt) emit(e, ", 1 }");
                }
            }
            if (first_field) emit(e, "0");
            emit(e, " };\n");
            if (any_arr_field) {
                for (int i = 0; i < inst->expr->struct_init.field_count; i++) {
                    const char *fname = inst->expr->struct_init.fields[i].name;
                    uint32_t fname_len = (uint32_t)inst->expr->struct_init.fields[i].name_len;
                    Type *fty_arr = struct_field_type_by_name(inst->cast_type, fname, fname_len);
                    if (!fty_arr || type_dispatch_kind(fty_arr) != TYPE_ARRAY) continue;
                    emit_indent(e);
                    /* BUG-1192: the SOURCE is the array's VALUE (it decays to the
                     * first element's address), never `&src` — for an array
                     * PARAMETER, which C has already decayed to a pointer, `&a` is
                     * the address of that pointer variable, so the copy read
                     * sizeof(field) bytes of the caller's stack frame. */
                    emit(e, "memcpy(&");
                    emit_local_name(e, func, inst->dest_local);
                    emit(e, ".%.*s, (", (int)fname_len, fname);
                    if (inst->call_arg_locals && i < inst->call_arg_local_count &&
                        inst->call_arg_locals[i] >= 0)
                        emit_local_name(e, func, inst->call_arg_locals[i]);
                    else
                        emit_rewritten_node(e, inst->expr->struct_init.fields[i].value, func);
                    emit(e, "), sizeof(");
                    emit_local_name(e, func, inst->dest_local);
                    emit(e, ".%.*s));\n", (int)fname_len, fname);
                }
            }
        }
        break;
    }

    case IR_INDEX_WRITE: {
        /* Defensive: ir_lower never emits IR_INDEX_WRITE today.
         * Index-write statements flow through IR_ASSIGN passing the AST
         * node to emit_rewritten_node which routes to emit_expr with
         * bounds checks. Reserved for future per-element IR refactor.
         * Pre-fix this case silently emitted a TODO comment, which would
         * be a silent-skip miscompile if lowering ever started emitting
         * IR_INDEX_WRITE. Audit-2026-05-08: trap so the regression is
         * visible as a runtime failure rather than a missing array store. */
        emit_indent(e);
        emit(e, "_zer_trap(\"IR_INDEX_WRITE not implemented\", "
                "__FILE__, __LINE__);\n");
        break;
    }

    case IR_ADDR_OF:
    case IR_DEREF_READ:
    case IR_CALL_DECOMP:
    case IR_INTRINSIC_DECOMP:
    case IR_ORELSE_DECOMP:
    case IR_SLICE_READ: {
        /* Dead-code guard: ir_lower.c does NOT emit these opcodes today
         * (the equivalent paths route through IR_ASSIGN + emit_rewritten_node).
         * Defense-in-depth: stderr diagnostic + runtime trap. */
        fprintf(stderr, "compiler bug: emit_ir_inst hit dormant 3AC op %d "
                "(IR_INDEX_WRITE/IR_ADDR_OF/IR_DEREF_READ/IR_CALL_DECOMP/"
                "IR_INTRINSIC_DECOMP/IR_ORELSE_DECOMP/IR_SLICE_READ) — "
                "lowerer emitted this op without an emitter handler\n",
                inst->op);
        emit_indent(e);
        emit(e, "_zer_trap(\"compiler bug: unhandled 3AC IR op %d\", "
             "__FILE__, __LINE__);\n", inst->op);
        break;
    }
    /* Exhaustive switch on IROpKind. `default:` removed 2026-05-10 — the
     * old default emitted a comment-only stub (silent miscompile). If a new
     * IROpKind is added without a case, GCC -Wswitch flags it at compile
     * time. (IR_NOP is handled in the case block above.) */
    }
}

/* BUG-1003 (from vgonmt f1265ee5 / vigilant-tesla-lzmkhn, its BUG-992/917): re-anchor the C preprocessor's line counter to
 * the ZER source line of the instruction about to be emitted.
 *
 * Function BODIES are IR-only, and IR block emission sets `e->source_file` to
 * NULL — source mapping was switched off wholesale because `#line` collided with
 * goto labels and statement expressions (the BUG-418 class). The consequence was
 * never written down: with ONE `#line` per function (at its declaration) and none
 * inside, every line inside every function body mapped to "function's line +
 * offset in the generated C", so EVERY runtime trap named a line that does not
 * exist (a 7-line file reported "ln1.zer:15"). The trap fires correctly; only
 * the location lies, which is why nothing caught it — the number is plausible.
 *
 * Emitting the directive HERE is safe where the wholesale approach was not: this
 * is called between instructions, at column 0, never inside a `({...})` statement
 * expression and never on the same line as a `{` or a label (the block label is
 * emitted with its own trailing newline).
 *
 * It is emitted for EVERY instruction, not only when the ZER line changes:
 * `#line N` numbers the NEXT line N and then counts up, so an instruction that
 * expands to three C lines leaves the counter at N+3 — a second instruction on
 * the SAME ZER line would be misreported without its own anchor. An instruction
 * with no location (0) re-anchors to the last known line rather than drifting. */
static void emit_line_map(Emitter *e, const char *src_file, int line, int *last) {
    if (!src_file) return;
    if (line <= 0) line = *last;
    if (line <= 0) return;
    emit(e, "#line %d \"%s\"\n", line, src_file);
    *last = line;
}

/* BUG-1298: publish "done" at a @once's join block — the block some @once branch in
 * `blocks` names as its false_block. `blocks` is the function's, or a defer
 * template's (whose instructions still carry the template's ORIGINAL ids). */
static void emit_once_join_publish(Emitter *e, IRBlock *blocks, int n, int block_id) {
    for (int bj = 0; bj < n; bj++) {
        for (int ij = 0; ij < blocks[bj].inst_count; ij++) {
            IRInst *in = &blocks[bj].insts[ij];
            if (in->op == IR_BRANCH && in->cond_local < 0 && in->expr &&
                in->expr->kind == NODE_ONCE && in->false_block == block_id) {
                emit(e, "#if _ZER_HOSTED\n");
                emit_indent(e);
                emit(e, "__atomic_store_n(&_zer_once_%d, 2u, __ATOMIC_RELEASE);\n",
                     emit_once_id(e, in->expr));
                emit(e, "#endif\n");
                return;
            }
        }
    }
}

static void emit_once_decls_in(Emitter *e, IRBlock *blocks, int n, const char *indent) {
    for (int bi = 0; bi < n; bi++) {
        for (int ii = 0; ii < blocks[bi].inst_count; ii++) {
            IRInst *in = &blocks[bi].insts[ii];
            if (in->op == IR_BRANCH && in->cond_local < 0 && in->expr &&
                in->expr->kind == NODE_ONCE) {
                int before = e->once_n;
                int id = emit_once_id(e, in->expr);
                if (e->once_n > before)
                    emit(e, "%sstatic uint32_t _zer_once_%d = 0;\n", indent, id);
            }
        }
    }
}

/* B4 (BUG-756) + BUG-1298: declare one function-scope flag per @once NODE — in the
 * body and in every defer template (a labelled function emits those inline). */
static void emit_once_decls(Emitter *e, IRFunc *func, const char *indent) {
    e->once_n = 0;
    emit_once_decls_in(e, func->blocks, func->block_count, indent);
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        for (int ii = 0; ii < bb->inst_count; ii++) {
            IRInst *in = &bb->insts[ii];
            if (in->op == IR_DEFER_PUSH && in->defer_tpl && in->defer_tpl->count > 0)
                emit_once_decls_in(e, in->defer_tpl->blocks, in->defer_tpl->count, indent);
        }
    }
}

/* One IR instruction with its C-level auto-guards. Shared by the regular and the
 * async block loops, and by inline defer-template emission (BUG-1298). */
static void emit_ir_inst_guarded(Emitter *e, IRInst *ins, IRFunc *func,
                                 const char *src_file, int *last_line) {
    /* BUG-1003: anchor the line counter BEFORE the guards, so an
     * auto-guard trap reports the access's line, not the previous one. */
    emit_line_map(e, src_file, ins->source_line, last_line);
    if (ins->expr) {
        IROpKind k = ins->op;
        /* Audit-fix (2026-06-30): widened to IR_AWAIT (cond carries
         * AST array indexing re-emitted per-poll) and IR_NOP (carries
         * NODE_SPAWN args copied in parent thread). Both were
         * silently miscompiling unproven arr[i] — emit_auto_guards
         * extended to descend NODE_SPAWN/NODE_AWAIT to pair. */
        /* BUG-952 (refactor M, the ordering half): IR_LOCK belongs in this
         * gate too. It carries the SHARED ROOT expression, and that root can
         * be INDEXED — `arr[i].v = 1` on a `shared struct S[4]`. Without it
         * the guard was emitted before the ASSIGN, which is INSIDE the lock,
         * so the emitted C read:
         *
         *   pthread_mutex_lock(&(_zer_bounds_check(i,4,…), arr)[i]._zer_mtx);
         *   if ((size_t)(i) >= 4u) { _zer_trap("…inside a held lock…"); }
         *
         * — the lock's own bounds check traps first, and even reaching the
         * guard it could only trap, because a lock is held and returning
         * would leak it. Measured: exit 133 on a program whose guard should
         * have taken a clean early return. Guarding the LOCK emits the check
         * BEFORE the lock, where the early return is still legal.
         *
         * This is the gate defect M exists to remove, and IR_LOCK is a
         * measured instance of it: an op kind carrying a guardable expr that
         * nobody had added. The list is hand-maintained and has been widened
         * reactively three times now (2026-05-03/06 async, 2026-06-30
         * AWAIT/NOP, and this). */
        if (ir_op_takes_auto_guards(k) &&
            !(k == IR_AWAIT && func->is_async)) {   /* BUG-1292: after its case label */
            bool sv_gt = e->guard_traps;              /* BUG-1291 */
            if (ins->in_defer_body) e->guard_traps = true;
            emit_auto_guards(e, ins->expr);
            e->guard_traps = sv_gt;
        }
    }
    emit_ir_inst(e, ins, func);
}

/* BUG-1298: the template ir_lower built for this defer body at its registration. */
static IRDeferTpl *ir_defer_tpl_for_body(IRFunc *func, Node *db) {
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        for (int ii = 0; ii < bb->inst_count; ii++) {
            IRInst *in = &bb->insts[ii];
            if (in->op == IR_DEFER_PUSH && in->defer_body == db &&
                in->defer_tpl && in->defer_tpl->count > 0)
                return in->defer_tpl;
        }
    }
    return NULL;
}

/* BUG-1298: emit ONE defer body at a fire point that ir_lower did not splice — a
 * fire in a function with a LABEL (the goto guard / ARMED flags wrap it in C), or
 * a C-level early exit (emit_defers_from).
 *
 * The body is emitted from its IR TEMPLATE, inline, with fresh block labels — the
 * same instructions a spliced body would have produced. It used to go through
 * emit_defer_stmt, a second statement emitter over the raw AST covering eleven
 * node kinds, which is exactly what refactor L removed for label-free functions:
 * measured on the pre-fix build, a shared read in a defer-body CONDITION took no
 * lock, and `switch` / `do-while` / `@critical` became `compiler bug: ... no
 * handler` plus a `_zer_trap` in place of valid code.
 *
 * A body with no template (none was lowered) keeps the AST path. */
static void emit_defer_body(Emitter *e, IRFunc *func, Node *db) {
    IRDeferTpl *t = func ? ir_defer_tpl_for_body(func, db) : NULL;
    if (!t) {
        if (db->kind == NODE_BLOCK) {
            /* F4 (2026-08-02): brace-scope the block-form body — it is emitted at
             * every exit path, so a declared local would otherwise be redefined. */
            emit_indent(e);
            emit(e, "{\n");
            e->indent++;
            for (int si = 0; si < db->block.stmt_count; si++)
                emit_defer_stmt(e, db->block.stmts[si], func);
            e->indent--;
            emit_indent(e);
            emit(e, "}\n");
        } else {
            emit_defer_stmt(e, db, func);
        }
        return;
    }
    /* Labels beyond any function block id, unique per emitted copy: a body is
     * emitted once per fire site, and C labels are function-scoped. */
    int base = (1 << 24) + e->defer_label_seq;
    e->defer_label_seq += t->count + 1;
    int end = base + t->count;
    int dummy_line = -1;
    int *last = e->ir_last_line ? e->ir_last_line : &dummy_line;
    emit_indent(e);
    emit(e, "{\n");
    e->indent++;
    for (int k = 0; k < t->count; k++) {
        IRBlock *tb = &t->blocks[k];
        emit(e, "_zer_bb%d:;\n", base + k);
        emit_once_join_publish(e, t->blocks, t->count, t->first + k);
        for (int ii = 0; ii < tb->inst_count; ii++) {
            IRInst c = tb->insts[ii];
            if (c.true_block  >= t->first && c.true_block  < t->first + t->count)
                c.true_block  = base + (c.true_block  - t->first);
            if (c.false_block >= t->first && c.false_block < t->first + t->count)
                c.false_block = base + (c.false_block - t->first);
            if (c.goto_block  >= t->first && c.goto_block  < t->first + t->count)
                c.goto_block  = base + (c.goto_block  - t->first);
            emit_ir_inst_guarded(e, &c, func, e->ir_src_file, last);
        }
        /* The exit block (and any block lowering left open) leaves the body. */
        if (!ir_block_is_terminated(tb)) {
            emit_indent(e);
            emit(e, "goto _zer_bb%d;\n", end);
        }
    }
    emit(e, "_zer_bb%d:;\n", end);
    e->indent--;
    emit_indent(e);
    emit(e, "}\n");
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

    /* Interrupt handlers have a distinct C signature shape — no params, no
     * return type, and the GCC `interrupt` attribute + _IRQHandler suffix.
     * `ir_lower_interrupt` sets func->is_interrupt and fn->kind=NODE_INTERRUPT. */
    if (func->is_interrupt) {
        /* 8ezecl (copied): emit `used` so -ffunction-sections + --gc-sections
         * (mainstream embedded flags) cannot silently tree-shake the ISR — the
         * only reference is the vector table in a separate TU. */
        emit(e, "void __attribute__((interrupt, used)) %.*s_IRQHandler(void) {\n",
             (int)fn->interrupt.name_len, fn->interrupt.name);
        e->indent++;
        e->current_func_ret = NULL;
    } else {
        /* BUG-651 fix (2026-05-01/02): function-level GCC attributes
         * (section/static) propagate from AST to emitted C. Pre-fix,
         * the IR migration silently dropped these — they only existed
         * on the AST proto-only path. Now both paths share one helper.
         *
         * NOTE: `__attribute__((naked))` is intentionally NOT emitted —
         * existing asm tests rely on the implicit prologue/epilogue and
         * would SIGILL if naked were re-enabled. Restoring true naked
         * semantics is tracked separately in docs/limitations.md. */
        emit_func_attributes(e, fn);

        /* Return type + name.
         * func->return_type may be the function TYPE (func_ptr) from typemap.
         * Extract the actual return type from func_ptr.ret. */
        Type *ret = func->return_type;
        if (ret && ret->kind == TYPE_FUNC_PTR) ret = ret->func_ptr.ret;

        /* BUG fix (2026-04-22): ZER `void main()` auto-promoted to C
         * `int main(void) { ... return 0; }`. Rationale: C99 requires
         * main to return int; `void main()` leaves exit code undefined
         * (whatever's in EAX). Caught by tests/zer_proof/A01_no_uaf —
         * safe program was exiting with code 2 instead of 0.
         * Only applies to the top-level main (no module prefix). */
        bool main_promote = (!func->module_prefix &&
                              func->name_len == 4 &&
                              memcmp(func->name, "main", 4) == 0 &&
                              (!ret || ret->kind == TYPE_VOID));

        /* If `ret` is itself a funcptr, the function RETURNS a funcptr —
         * C requires nested-paren syntax: RET (*name(params))(fp_args).
         * Without this branch, emitter would produce invalid C like
         * `RET (*)(fp_args) name(params)` which gcc rejects. */
        emit_func_decl_head(e, ret, main_promote);          /* BUG-1111 */
        e->current_main_promoted = main_promote;

        /* Mangled name */
        if (func->module_prefix) {
            emit(e, "%.*s__%.*s", (int)func->module_prefix_len, func->module_prefix,
                 (int)func->name_len, func->name);
        } else {
            emit(e, "%.*s", (int)func->name_len, func->name);
        }

        /* Parameters — use AST types (same resolution as AST emitter).
         * IR local types may be ty_void for complex params (struct, pointer). */
        /* BUG-1316: `main([*][*]u8 args)` — the C runtime calls main with
         * (argc, argv), so the ZER parameter is BUILT here from them: a stack
         * array of `[*]u8` (one per argument, length by scanning for the NUL) and
         * a slice over it. The checker admits no other parameter list for main. */
        bool main_args = !func->module_prefix && func->name_len == 4 &&
            memcmp(func->name, "main", 4) == 0 && fn->func_decl.param_count == 1;
        if (main_args) emit(e, "(int _zer_argc, char **_zer_argv)");
        else emit_func_decl_params(e, fn, checker_get_type(e->checker, fn));
        emit_func_decl_tail(e, ret, main_promote);
        emit(e, " {\n");
        e->indent++;
        if (main_args) {
            ParamDecl *ap = &fn->func_decl.params[0];
            Type *fty = checker_get_type(e->checker, fn);
            Type *at = (fty && type_dispatch_kind(fty) == TYPE_FUNC_PTR && fty->func_ptr.param_count == 1)
                ? fty->func_ptr.params[0] : resolve_tynode(e, ap->type);
            Type *au = type_unwrap_distinct(at);
            Type *elem = au->slice.inner;
            emit_indent(e);
            emit_type(e, elem);
            emit(e, " _zer_argbuf[_zer_argc > 0 ? _zer_argc : 1];\n");
            emit_indent(e);
            emit(e, "for (int _zer_ai = 0; _zer_ai < _zer_argc; _zer_ai++) {\n");
            emit_indent(e);
            emit(e, "    size_t _zer_al = 0; while (_zer_argv[_zer_ai][_zer_al]) _zer_al++;\n");
            emit_indent(e);
            emit(e, "    _zer_argbuf[_zer_ai].ptr = (uint8_t *)_zer_argv[_zer_ai];\n");
            emit_indent(e);
            emit(e, "    _zer_argbuf[_zer_ai].len = _zer_al;\n");
            emit_indent(e);
            emit(e, "}\n");
            emit_indent(e);
            emit_type_and_name(e, at, ap->name, ap->name_len);
            emit(e, " = { .ptr = _zer_argbuf, .len = (size_t)(_zer_argc > 0 ? _zer_argc : 0) };\n");
        }
        e->current_func_ret = ret; /* needed for IR_RETURN optional wrapping */
    }
    e->defer_stack.count = 0; /* clear defer stack from previous function */

    /* Declare local variables (skip params — they're parameters).
     * Static locals declared with static keyword (persists across calls).
     * Static init expressions are emitted at the declaration — checker
     * enforces compile-time-constant for `static` init, so emitting via
     * emit_rewritten_node produces a valid C initializer. Pre-fix, init
     * was silently dropped (e.g. `static u32 retries = 3;` got `= {0};`
     * — function returned 0 on every call). */
    for (int li = 0; li < func->local_count; li++) {
        IRLocal *l = &func->locals[li];
        if (l->is_param) continue;
        if (l->is_capture && l->type && l->type->kind == TYPE_VOID) continue;
        if (!l->type) continue;
        emit_indent(e);
        if (l->is_static) emit(e, "static ");
        /* #19 (VOL-1): a `volatile` scalar/aggregate local carries no volatile in
         * its Type (only slice/pointer do, propagated by the checker), so emit the
         * qualifier here. Skip pointer/slice — their volatile is already in the
         * emitted type (`volatile uint32_t *`); an outer prefix would change meaning
         * (volatile-pointer vs pointer-to-volatile). */
        if (l->is_volatile && l->type) {
            TypeKind vtk = type_dispatch_kind(l->type);
            if (vtk != TYPE_POINTER && vtk != TYPE_SLICE)
                emit(e, "volatile ");
        }
        emit_type_and_name(e, l->type, l->name, l->name_len);
        if (l->is_static && l->static_init) {
            /* BUG-1315: a static's initializer is a C CONSTANT expression. An
             * integer one is folded the way a global's is (BUG-1090 typed fold):
             * rendered as an expression, `static u32 i = 7 % 4;` emitted the
             * guarded-division statement expression and GCC refused it
             * ("initializer element is not constant"). */
            int64_t sv;
            if (type_is_integer(l->type) &&
                checker_fold_const_typed(e->checker, l->static_init, &sv) &&
                sv != CONST_EVAL_FAIL) {
                sv = fold_wrap_to_type(sv, l->type);
                if (sv < 0) emit(e, " = (%lld);\n", (long long)sv);
                else emit(e, " = %lluULL;\n", (unsigned long long)sv);
            } else {
                emit(e, " = ");
                emit_rewritten_node(e, l->static_init, func);
                emit(e, ";\n");
            }
        } else {
            emit(e, " = {0};\n"); /* auto-zero */
        }
    }

    /* B4 (BUG-756): declare a function-scope flag for each @once so the
     * loser-wait (emitted at the @once branch) and the winner's done-publish
     * (emitted at the join block below) can both reference it. The flag id is the
     * @once's bb_skip (false_block) id — unique within the function, and reachable
     * from both the branch (inst->false_block) and the join (bb->id). */
    emit_once_decls(e, func, "    ");

    /* Wholesale source mapping stays OFF during IR block emission — an
     * unconditional #line collides with goto labels and statement expressions
     * (BUG-418 class). BUG-1003 re-anchors it per INSTRUCTION instead, which is
     * safe because that point is always between statements at column 0. */
    const char *saved_source = e->source_file;
    e->source_file = NULL;
    int last_mapped_line = -1;
    const char *sv_ir_src = e->ir_src_file;     /* BUG-1298 */
    int *sv_ir_last = e->ir_last_line;
    e->ir_src_file = saved_source;
    e->ir_last_line = &last_mapped_line;

    /* Emit basic blocks */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];

        /* Label for every block (including bb0 — goto may target entry) */
        emit(e, "_zer_bb%d:;\n", bb->id);

        /* B4: if this block is a @once join (the bb_skip / false_block of some
         * @once branch), the winner reaches it after running the body — publish
         * "done" (RELEASE) so a spinning loser (ACQUIRE) observes the fully
         * constructed state. The loser also re-enters here and re-stores 2
         * (idempotent). Hosted only; freestanding @once is single-core (no wait). */
        emit_once_join_publish(e, func->blocks, func->block_count, bb->id);

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
            /* Emit auto-guards before statement-producing IR ops (bounds + UAF).
             * Checker marks NODE_INDEX with auto_guard_size when VRP can't prove
             * index in bounds; the guard returns zero before the OOB access.
             *
             * Gap 34 fix (2026-04-27): added IR_INDEX_READ. Pointer indexing
             * `reg[i]` on `volatile *u32 reg = @inttoptr(...)` lowers as
             * IR_INDEX_READ (not IR_ASSIGN like array indexing), and was
             * silently missing the auto-guard pass. Compiler emitted the
             * "auto-guard inserted" warning while emitting raw `reg[i]` with
             * NO guard — silent miscompile. Hosted: SIGSEGV catches if address
             * unmapped; baremetal: silent corruption (entire address space
             * valid). The handler comment claimed the pre-pass handles arrays
             * — true for IR_ASSIGN, was false for IR_INDEX_READ. */
            emit_ir_inst_guarded(e, &bb->insts[ii], func, saved_source,
                                 &last_mapped_line);
        }

        if (has_capture_scope) {
            e->indent--;
            emit_indent(e);
            emit(e, "}\n");
        }
    }

    /* Note: for void main() promoted to int main(), the IR-emitted return
     * (via emit_return_null) handles the bare return as `return 0;` when
     * current_main_promoted is true. C99 §5.1.2.2.3 also guarantees
     * fall-through from int main = return 0 implicitly. */

    e->source_file = saved_source;
    e->ir_src_file = sv_ir_src;
    e->ir_last_line = sv_ir_last;
    e->current_func_ret = NULL;
    e->current_main_promoted = false;
    e->indent--;
    emit(e, "}\n\n");
}

/* Emit an async function from IR — state struct + init + poll */
/* BUG-1238: the state struct + result accessor, split out so an async
 * function can have them emitted EARLY (emit_early_async_structs). */
static void emit_async_state_struct(Emitter *e, IRFunc *func) {
    Node *fn = func->ast_node;
    if (!fn) return;
    char mname[256];
    int flen = snprintf(mname, sizeof(mname), "%.*s",
        (int)func->name_len, func->name);
    if (flen >= (int)sizeof(mname)) flen = (int)sizeof(mname) - 1;
    /* BUG-863: a value-returning async needs somewhere to PUT the value.
     * `async u32 compute() { … return 42; }` compiled clean and the state
     * machine finalized correctly, but the value landed in an internal temp
     * (`self->_zer_t0`) with no caller-accessible accessor and an unstable name
     * — so a user who wrote `async <non-void>` could never read the result.
     * Neither rejected nor retrievable. A stable `_zer_result` field plus a
     * `_zer_async_NAME_result()` accessor is the additive half of that; the
     * `int` poll protocol (0 = pending, 1 = done) is untouched, because the
     * done-flag and the value are different questions. */
    Type *afn_type = checker_get_type(e->checker, fn);
    Type *async_ret = (afn_type && type_dispatch_kind(afn_type) == TYPE_FUNC_PTR)
        ? afn_type->func_ptr.ret : NULL;
    if (async_ret && type_dispatch_kind(async_ret) == TYPE_VOID) async_ret = NULL;

    /* State struct = ALL locals. BUG-1177: TAGGED, and the typedef itself is
     * emitted up front (emit_async_forward_typedefs), so a function declared
     * before this one can take a `*_zer_async_NAME` — the pointer is the only way
     * to hand a task around, since the value is not copyable. */
    emit(e, "struct _zer_async_%.*s {\n", flen, mname);
    emit(e, "    int _zer_state;\n");
    if (async_ret) {
        emit(e, "    ");
        emit_type_and_name(e, async_ret, "_zer_result", 11);
        emit(e, ";\n");
    }
    for (int li = 0; li < func->local_count; li++) {
        IRLocal *l = &func->locals[li];
        if (l->is_static) continue;
        if (!l->type) continue;
        emit(e, "    ");
        emit_type_and_name(e, l->type, l->name, l->name_len);
        emit(e, ";\n");
    }
    emit(e, "};\n\n");

    /* Result accessor — only for a non-void async. Reading it before the poll
     * protocol reports done yields the zeroed initial value, exactly like any
     * other field of a freshly `_init`ed task. */
    if (async_ret) {
        emit(e, "static inline ");
        emit_type(e, async_ret);
        emit(e, " _zer_async_%.*s_result(_zer_async_%.*s *self) {\n",
             flen, mname, flen, mname);
        /* BUG-1237: before done the field holds the zeroed initial value, which
         * for a non-null pointer is NULL and for an enum without a 0 variant is
         * no variant at all — a forged value (measured: a `*Box` result read
         * early was NULL, an `E{a=5,b=6}` result took an arm). Those two refuse
         * an early read; every other type keeps the documented zero. */
        if (checker_type_has_no_zero_value(async_ret))
            emit(e, "    if (self->_zer_state != -1) _zer_trap(\"async result read before "
                    "the task finished — its return type has no zero value\", "
                    "__FILE__, __LINE__);\n");
        emit(e, "    return self->_zer_result;\n");
        emit(e, "}\n\n");
    }

    /* BUG-1238: prototypes, so a function emitted before the async one can
     * call _init / _poll (the definitions stay where the function is). */
    emit(e, "static inline void _zer_async_%.*s_init(_zer_async_%.*s *self",
         flen, mname, flen, mname);
    for (int li = 0; li < func->local_count; li++) {
        if (!func->locals[li].is_param) continue;
        emit(e, ", ");
        emit_type_and_name(e, func->locals[li].type,
                           func->locals[li].name, func->locals[li].name_len);
    }
    emit(e, ");\n");
    emit(e, "static inline int _zer_async_%.*s_poll(_zer_async_%.*s *self);\n\n",
         flen, mname, flen, mname);
}

static void emit_async_func_from_ir(Emitter *e, IRFunc *func) {
    Node *fn = func->ast_node;
    if (!fn) return;

    /* Build mangled name */
    /* BUG-866: the async internal names are NOT module-mangled.
     *
     * They used to be — `_zer_async_lib1__acompute` for a coroutine in module
     * `lib1` — while the CHECKER registers the state-struct type and the
     * init/poll/result accessors under the UNMANGLED `_zer_async_acompute`
     * (checker.c, the NODE_FUNC_DECL async arm). So a user of an imported async
     * wrote exactly what the checker accepts, the emitter emitted it verbatim at
     * the use site, and GCC found no such type or function: async across module
     * boundaries did not compile at all, in any form, with the failure landing
     * as a GCC error in generated code rather than a ZER diagnostic.
     *
     * Dropping the prefix here makes all five names agree — the type, _init,
     * _poll, _result and the state struct — and it is the side that had to move:
     * the checker's registration is what the user's source spells, and the
     * accessor names are part of the documented API (reference.md "async").
     *
     * The cost is that two modules each defining an async function of the SAME
     * name now collide, as a C redefinition error. That is loud, and it was
     * already true of the state-struct TYPE name before this change (the
     * checker registered it unmangled either way). Recorded in
     * docs/limitations.md. */
    char mname[256];
    int flen = snprintf(mname, sizeof(mname), "%.*s",
        (int)func->name_len, func->name);
    if (flen >= (int)sizeof(mname)) flen = (int)sizeof(mname) - 1;

    {
        bool early = false;
        for (int ai = 0; ai < e->early_async_count && !early; ai++)
            early = e->early_async_ir[ai] == (void *)func;
        if (!early) emit_async_state_struct(e, func);
    }
    Type *afn_type = checker_get_type(e->checker, fn);
    Type *async_ret = (afn_type && type_dispatch_kind(afn_type) == TYPE_FUNC_PTR)
        ? afn_type->func_ptr.ret : NULL;
    if (async_ret && type_dispatch_kind(async_ret) == TYPE_VOID) async_ret = NULL;

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
        /* BUG-1239: an ARRAY param is copied by value into the task (it is a
         * pointer in the C init's parameter list); `self->a = a` is not C. */
        if (type_dispatch_kind(func->locals[li].type) == TYPE_ARRAY) {
            emit(e, "    memcpy(self->%.*s, %.*s, sizeof(self->%.*s));\n",
                 (int)func->locals[li].name_len, func->locals[li].name,
                 (int)func->locals[li].name_len, func->locals[li].name,
                 (int)func->locals[li].name_len, func->locals[li].name);
            continue;
        }
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
                        emit_rewritten_node(e, s->var_decl.init, func);
                    }
                    emit(e, ";\n");
                }
            }
        }
    }

    /* BUG-1240: the @once flags (the same pre-scan as the regular path). They
     * are C statics — one per @once, shared by every task, which is exactly
     * @once's "once per program". Missing here, a @once in an async body was an
     * undeclared identifier at GCC. */
    emit_once_decls(e, func, "    ");
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

    /* Disable source mapping during IR blocks (see the BUG-1003 note on the
     * regular path — the per-instruction re-anchor below replaces it). */
    const char *saved_source = e->source_file;
    e->source_file = NULL;
    int last_mapped_line = -1;
    const char *sv_ir_src = e->ir_src_file;     /* BUG-1298 */
    int *sv_ir_last = e->ir_last_line;
    e->ir_src_file = saved_source;
    e->ir_last_line = &last_mapped_line;

    /* BUG-863: IR_RETURN reads this to decide whether the async termination
     * also stores a result. The regular-function path sets it; this one never
     * did, because until now an async return had no value to carry. */
    Type *saved_ret = e->current_func_ret;
    e->current_func_ret = async_ret;

    /* Emit basic blocks */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        {
            emit_indent(e);
            emit(e, "_zer_bb%d:;\n", bb->id);
        }
        /* BUG-1298: the winner's done-publish was missing on the async path, so a
         * second arrival at an async @once spun forever on a flag stuck at 1. */
        emit_once_join_publish(e, func->blocks, func->block_count, bb->id);
        for (int ii = 0; ii < bb->inst_count; ii++) {
            /* Audit-fix (2026-05-03/06): the async loop mirrors the regular path's
             * auto-guard emission — one helper now, so the two cannot drift. */
            emit_ir_inst_guarded(e, &bb->insts[ii], func, saved_source,
                                 &last_mapped_line);
        }
    }

    e->source_file = saved_source;
    e->ir_src_file = sv_ir_src;
    e->ir_last_line = sv_ir_last;
    e->current_func_ret = saved_ret;
    emit(e, "    } self->_zer_state = -1; return 1;\n");
    emit(e, "}\n\n");

    /* Restore async context */
    e->in_async = saved_async;
}

/* Public: emit a function from its IR representation */
void emit_func_from_ir(Emitter *e, void *ir_func_ptr) {
    IRFunc *func = (IRFunc *)ir_func_ptr;
    if (!func) return;
    /* Expose the current func so a mid-body conditional early-exit can fire its
     * pending IR defer bodies (emit_defers_from) instead of aborting. */
    void *saved_ir_func = e->cur_ir_func;
    e->cur_ir_func = func;
    if (func->is_async) {
        emit_async_func_from_ir(e, func);
    } else {
        emit_regular_func_from_ir(e, func);
    }
    e->cur_ir_func = saved_ir_func;
}
