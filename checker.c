#include "checker.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ================================================================
 * ZER Type Checker — walks AST, resolves types, checks correctness
 *
 * Architecture:
 *   1. First pass: register all top-level declarations (structs, enums,
 *      unions, typedefs, functions, globals) into global scope.
 *   2. Second pass: type-check all function bodies.
 *
 * This two-pass approach allows forward references (function A calls
 * function B defined later in the file).
 * ================================================================ */

/* ---- Error reporting ---- */

static void checker_add_diag(Checker *c, int line, int severity, const char *fmt, va_list ap) {
    if (c->diag_count >= c->diag_capacity) {
        int new_cap = c->diag_capacity * 2;
        if (new_cap < 32) new_cap = 32;
        Diagnostic *new_d = (Diagnostic *)realloc(c->diagnostics, new_cap * sizeof(Diagnostic));
        if (new_d) { c->diagnostics = new_d; c->diag_capacity = new_cap; }
    }
    if (c->diag_count < c->diag_capacity) {
        Diagnostic *d = &c->diagnostics[c->diag_count++];
        d->line = line;
        d->severity = severity;
        vsnprintf(d->message, sizeof(d->message), fmt, ap);
    }
}

static void checker_error(Checker *c, int line, const char *fmt, ...) {
    c->error_count++;
    va_list args;
    va_start(args, fmt);
    checker_add_diag(c, line, 1, fmt, args);
    va_end(args);
    fprintf(stderr, "%s:%d: error: ", c->file_name, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void checker_warning(Checker *c, int line, const char *fmt, ...) {
    c->warning_count++;
    va_list args;
    va_start(args, fmt);
    checker_add_diag(c, line, 2, fmt, args);
    va_end(args);
    fprintf(stderr, "%s:%d: warning: ", c->file_name, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* ---- Non-storable tracking ----
 * pool.get(h) returns a non-storable result.
 * BUG-346: moved from static globals into Checker struct for thread safety. */

static void non_storable_init(Checker *c) {
    c->non_storable_capacity = 64;
    c->non_storable_nodes = (Node **)arena_alloc(c->arena, c->non_storable_capacity * sizeof(Node *));
    c->non_storable_count = 0;
}

static void mark_non_storable(Checker *c, Node *n) {
    if (c->non_storable_count >= c->non_storable_capacity) {
        int new_cap = c->non_storable_capacity * 2;
        Node **new_arr = (Node **)arena_alloc(c->arena, new_cap * sizeof(Node *));
        memcpy(new_arr, c->non_storable_nodes, c->non_storable_count * sizeof(Node *));
        c->non_storable_nodes = new_arr;
        c->non_storable_capacity = new_cap;
    }
    c->non_storable_nodes[c->non_storable_count++] = n;
}

static bool is_non_storable(Checker *c, Node *n) {
    for (int i = 0; i < c->non_storable_count; i++) {
        if (c->non_storable_nodes[i] == n) return true;
    }
    return false;
}

/* ---- Scope helpers ---- */

static void push_scope(Checker *c) {
    c->current_scope = scope_new(c->arena, c->current_scope);
}

static void pop_scope(Checker *c) {
    c->current_scope = c->current_scope->parent;
}

static Symbol *add_symbol(Checker *c, const char *name, uint32_t name_len,
                          Type *type, int line) {
    /* BUG-276: warn on _zer_ prefixed names — reserved for compiler internals */
    if (name_len >= 5 && memcmp(name, "_zer_", 5) == 0) {
        checker_error(c, line,
            "identifier '%.*s' uses reserved prefix '_zer_' — "
            "may collide with compiler-generated names",
            (int)name_len, name);
    }
    Symbol *sym = scope_add(c->arena, c->current_scope, name, name_len,
                            type, line, c->file_name);
    if (!sym) {
        /* check if this is a cross-module type collision — give helpful error */
        Symbol *existing = scope_lookup(c->current_scope, name, name_len);
        if (existing && existing->type && type && c->current_module) {
            const char *existing_mod = NULL;
            if (existing->type->kind == TYPE_STRUCT) existing_mod = existing->type->struct_type.module_prefix;
            else if (existing->type->kind == TYPE_ENUM) existing_mod = existing->type->enum_type.module_prefix;
            else if (existing->type->kind == TYPE_UNION) existing_mod = existing->type->union_type.module_prefix;

            if (existing_mod != c->current_module) {
                /* cross-module type collision — allowed, per-module scope resolves it.
                 * First registration wins in global scope; module scope overrides. */
                return existing;
            }
        }
        checker_error(c, line, "redefinition of '%.*s'", (int)name_len, name);
    }
    return sym;
}

static Symbol *find_symbol(Checker *c, const char *name, uint32_t name_len, int line) {
    Symbol *sym = scope_lookup(c->current_scope, name, name_len);
    if (!sym) {
        checker_error(c, line, "undefined identifier '%.*s'", (int)name_len, name);
    }
    return sym;
}

/* ---- Node type storage ----
 * We store the resolved Type* for each expression node in a simple
 * hash map keyed by node pointer. This avoids modifying the Node struct. */

/* ---- Dynamic type map: open-addressing hash, grows when >70% full ---- */
/* RF1: typemap is now per-Checker (fields in Checker struct), not global */

#define TYPE_MAP_INIT_SIZE 4096

static void typemap_init(Checker *c) {
    c->type_map_size = TYPE_MAP_INIT_SIZE;
    c->type_map = (TypeMapEntry *)arena_alloc(c->arena, c->type_map_size * sizeof(TypeMapEntry));
    c->type_map_count = 0;
}

static void typemap_grow(Checker *c) {
    uint32_t old_size = c->type_map_size;
    TypeMapEntry *old_map = c->type_map;
    c->type_map_size = old_size * 2;
    c->type_map = (TypeMapEntry *)arena_alloc(c->arena,
        c->type_map_size * sizeof(TypeMapEntry));
    c->type_map_count = 0;
    for (uint32_t i = 0; i < old_size; i++) {
        if (old_map[i].key) {
            uint32_t idx = ((uintptr_t)old_map[i].key >> 3) % c->type_map_size;
            for (uint32_t j = 0; j < c->type_map_size; j++) {
                uint32_t slot = (idx + j) % c->type_map_size;
                if (!c->type_map[slot].key) {
                    c->type_map[slot] = old_map[i];
                    c->type_map_count++;
                    break;
                }
            }
        }
    }
}

static void typemap_set(Checker *c, Node *node, Type *type) {
    if (c->type_map_count * 10 > c->type_map_size * 7) {
        typemap_grow(c);
    }
    uint32_t idx = ((uintptr_t)node >> 3) % c->type_map_size;
    for (uint32_t i = 0; i < c->type_map_size; i++) {
        uint32_t slot = (idx + i) % c->type_map_size;
        if (!c->type_map[slot].key) {
            c->type_map[slot].key = node;
            c->type_map[slot].type = type;
            c->type_map_count++;
            return;
        }
        if (c->type_map[slot].key == node) {
            c->type_map[slot].type = type;
            return;
        }
    }
}

static Type *typemap_get(Checker *c, Node *node) {
    if (!c->type_map) return NULL;
    uint32_t idx = ((uintptr_t)node >> 3) % c->type_map_size;
    for (uint32_t i = 0; i < c->type_map_size; i++) {
        uint32_t slot = (idx + i) % c->type_map_size;
        if (c->type_map[slot].key == node) return c->type_map[slot].type;
        if (!c->type_map[slot].key) return NULL;
    }
    return NULL;
}

/* ---- Forward declarations ---- */
static Type *check_expr(Checker *c, Node *node);
static void check_stmt(Checker *c, Node *node);
static Type *resolve_type(Checker *c, TypeNode *tn);

/* Check if an expression node is a literal that can be assigned to target type.
 * Integer literals fit any integer. Float literals fit any float. null fits ?T. */
static bool is_literal_compatible(Node *expr, Type *target) {
    if (!expr || !target) return false;
    /* unwrap distinct for literal compatibility */
    Type *effective = type_unwrap_distinct(target);
    if (expr->kind == NODE_INT_LIT && type_is_integer(effective)) {
        /* range check: literal must fit in target type */
        uint64_t val = expr->int_lit.value;
        switch (effective->kind) {
        case TYPE_U8:    return val <= 255;
        case TYPE_U16:   return val <= 65535;
        case TYPE_U32:   return val <= 0xFFFFFFFFULL;
        case TYPE_U64:   return true;
        case TYPE_USIZE: return val <= (zer_target_ptr_bits == 64 ? UINT64_MAX : 0xFFFFFFFFULL);
        case TYPE_I8:    return val <= 127;
        case TYPE_I16:   return val <= 32767;
        case TYPE_I32:   return val <= 2147483647ULL;
        case TYPE_I64:   return true;
        default:         return true;
        }
    }
    /* bool is NOT an integer — no int→bool coercion (spec rule) */
    if (expr->kind == NODE_FLOAT_LIT && type_is_float(effective)) return true;
    if (expr->kind == NODE_NULL_LIT && type_is_optional(target)) return true;
    if (expr->kind == NODE_BOOL_LIT && effective->kind == TYPE_BOOL) return true;
    if (expr->kind == NODE_CHAR_LIT && effective->kind == TYPE_U8) return true;
    /* -5 is UNARY(MINUS, INT_LIT) — negative integer literal */
    if (expr->kind == NODE_UNARY && expr->unary.op == TOK_MINUS) {
        if (expr->unary.operand->kind == NODE_INT_LIT && type_is_integer(effective)) {
            uint64_t val = expr->unary.operand->int_lit.value;
            switch (effective->kind) {
            case TYPE_I8:    return val <= 128;
            case TYPE_I16:   return val <= 32768;
            case TYPE_I32:   return val <= 2147483648ULL;
            case TYPE_I64:   return true;
            /* unsigned types: negative literals never fit */
            case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_USIZE:
                return false;
            default:         return true;
            }
        }
        if (expr->unary.operand->kind == NODE_FLOAT_LIT && type_is_float(effective))
            return true;
    }
    return false;
}

/* eval_const_expr() is defined in ast.h (shared with emitter) */

/* BUG-374: recursively check if a call expression has any local-derived pointer
 * arguments. identity(identity(&x)) — the outer call's arg is a NODE_CALL,
 * which itself has &x. We recurse into nested calls (max depth 8 to prevent
 * pathological input). Returns true if any arg is &local or local-derived. */
static bool call_has_local_derived_arg(Checker *c, Node *call, int depth) {
    if (!call || call->kind != NODE_CALL || depth > 8) return false;
    for (int i = 0; i < call->call.arg_count; i++) {
        Node *arg = call->call.args[i];
        /* direct &local */
        if (arg->kind == NODE_UNARY && arg->unary.op == TOK_AMP) {
            Node *root = arg->unary.operand;
            while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                if (root->kind == NODE_FIELD) root = root->field.object;
                else root = root->index_expr.object;
            }
            if (root && root->kind == NODE_IDENT) {
                bool is_global = scope_lookup_local(c->global_scope,
                    root->ident.name, (uint32_t)root->ident.name_len) != NULL;
                Symbol *src = scope_lookup(c->current_scope,
                    root->ident.name, (uint32_t)root->ident.name_len);
                if (src && !src->is_static && !is_global) return true;
            }
        }
        /* local-derived ident */
        if (arg->kind == NODE_IDENT) {
            Symbol *src = scope_lookup(c->current_scope,
                arg->ident.name, (uint32_t)arg->ident.name_len);
            if (src && (src->is_local_derived || src->is_arena_derived))
                return true;
        }
        /* nested call returning pointer — recurse */
        if (arg->kind == NODE_CALL) {
            Type *arg_type = typemap_get(c, arg);
            if (arg_type && type_unwrap_distinct(arg_type)->kind == TYPE_POINTER) {
                if (call_has_local_derived_arg(c, arg, depth + 1))
                    return true;
            }
        }
    }
    return false;
}

/* ================================================================
 * TYPE RESOLUTION: TypeNode (syntactic) → Type (semantic)
 * ================================================================ */

/* BUG-220: recursive type size computation for @size constant evaluation.
 * Handles nested structs, arrays, pointers, slices with natural alignment. */
static int64_t compute_type_size(Type *t) {
    t = type_unwrap_distinct(t);
    /* BUG-275/280: target-dependent types — don't constant-fold.
     * Let the emitter use sizeof() which GCC resolves per target. */
    if (t->kind == TYPE_POINTER) return CONST_EVAL_FAIL;
    if (t->kind == TYPE_SLICE) return CONST_EVAL_FAIL;
    if (t->kind == TYPE_USIZE) return CONST_EVAL_FAIL;
    int w = type_width(t);
    if (w > 0) return w / 8;
    if (t->kind == TYPE_ARRAY) {
        int64_t elem_size = compute_type_size(t->array.inner);
        if (elem_size <= 0) return -1;
        /* BUG-344: overflow-safe multiplication */
        int64_t count = (int64_t)t->array.size;
        if (count > 0 && elem_size > INT64_MAX / count) return CONST_EVAL_FAIL;
        return elem_size * count;
    }
    if (t->kind == TYPE_OPTIONAL) {
        /* BUG-243/275: ?*T and ?FuncPtr are null-sentinel (same size as pointer).
         * Target-dependent — don't constant-fold, let emitter use sizeof(). */
        { Type *oi = type_unwrap_distinct(t->optional.inner);
          if (oi->kind == TYPE_POINTER || oi->kind == TYPE_FUNC_PTR)
              return CONST_EVAL_FAIL; }
        /* ?void = { has_value: u8 } = 1 byte */
        if (t->optional.inner->kind == TYPE_VOID) return 1;
        /* ?T = { T value; u8 has_value; } — aligned like a struct */
        int64_t inner_size = compute_type_size(t->optional.inner);
        if (inner_size <= 0) return -1;
        int align = (int)(inner_size > 8 ? 8 : inner_size);
        int64_t total = inner_size; /* value field */
        /* add has_value (u8) with alignment padding */
        total += 1;
        /* pad to alignment of largest field */
        if (align > 1 && (total % align) != 0)
            total += align - (total % align);
        return total;
    }
    if (t->kind == TYPE_UNION) {
        /* BUG-250: tagged union = int32_t tag + max(variant_sizes), aligned */
        int64_t max_variant = 0;
        for (uint32_t i = 0; i < t->union_type.variant_count; i++) {
            int64_t vs = compute_type_size(t->union_type.variants[i].type);
            if (vs == CONST_EVAL_FAIL) return CONST_EVAL_FAIL;
            if (vs > max_variant) max_variant = vs;
        }
        int64_t total = 4; /* int32_t _tag */
        /* BUG-364: align union data to max variant ALIGNMENT, not size.
         * For arrays, alignment = element alignment (same fix as BUG-350 for structs). */
        int data_align = 4; /* minimum: tag alignment */
        for (uint32_t i = 0; i < t->union_type.variant_count; i++) {
            Type *vt = type_unwrap_distinct(t->union_type.variants[i].type);
            int va;
            if (vt && vt->kind == TYPE_ARRAY) {
                Type *elem = vt->array.inner;
                while (elem && elem->kind == TYPE_ARRAY) elem = elem->array.inner;
                int64_t esz = compute_type_size(elem);
                va = (esz > 0 && esz <= 8) ? (int)esz : 1;
            } else if (vt && vt->kind == TYPE_STRUCT && vt->struct_type.field_count > 0) {
                va = 1;
                for (uint32_t k = 0; k < vt->struct_type.field_count; k++) {
                    int64_t ks = compute_type_size(vt->struct_type.fields[k].type);
                    if (ks > 0 && ks <= 8 && (int)ks > va) va = (int)ks;
                }
            } else {
                int64_t vs = compute_type_size(vt);
                va = (int)(vs > 8 ? 8 : (vs > 0 ? vs : 1));
            }
            if (va > data_align) data_align = va;
        }
        if (data_align > 1 && (total % data_align) != 0)
            total += data_align - (total % data_align);
        total += max_variant;
        /* pad to alignment of largest member */
        int struct_align = data_align > 4 ? data_align : 4; /* at least tag alignment */
        if (struct_align > 1 && (total % struct_align) != 0)
            total += struct_align - (total % struct_align);
        return total > 0 ? total : -1;
    }
    if (t->kind == TYPE_STRUCT) {
        int64_t total = 0;
        int max_align = 1;
        bool is_packed = t->struct_type.is_packed;
        for (uint32_t fi = 0; fi < t->struct_type.field_count; fi++) {
            int64_t fsize = compute_type_size(t->struct_type.fields[fi].type);
            /* BUG-275: if any field is target-dependent, whole struct is */
            if (fsize == CONST_EVAL_FAIL) return CONST_EVAL_FAIL;
            if (fsize <= 0) fsize = 4; /* fallback for unknown types */
            /* BUG-350: alignment must be based on element type, not total size.
             * u8[10] has alignment 1 (element u8), not 8 (capped size). */
            int falign;
            if (is_packed) {
                falign = 1;
            } else {
                Type *ft = type_unwrap_distinct(t->struct_type.fields[fi].type);
                if (ft->kind == TYPE_ARRAY) {
                    /* array alignment = element alignment */
                    Type *elem = ft->array.inner;
                    while (elem && elem->kind == TYPE_ARRAY) elem = elem->array.inner;
                    int64_t elem_sz = compute_type_size(elem);
                    falign = (elem_sz > 0 && elem_sz <= 8) ? (int)elem_sz : 1;
                } else if (ft->kind == TYPE_STRUCT && ft->struct_type.field_count > 0) {
                    /* struct alignment = max field alignment */
                    falign = 1;
                    for (uint32_t k = 0; k < ft->struct_type.field_count; k++) {
                        int64_t ks = compute_type_size(ft->struct_type.fields[k].type);
                        if (ks > 0 && ks <= 8 && (int)ks > falign) falign = (int)ks;
                    }
                } else {
                    falign = (int)(fsize > 8 ? 8 : (fsize > 0 ? fsize : 1));
                }
            }
            if (falign > max_align) max_align = falign;
            if (!is_packed && falign > 1 && (total % falign) != 0)
                total += falign - (total % falign);
            /* BUG-362: overflow-safe field summation */
            if (fsize > 0 && total > INT64_MAX - fsize) return CONST_EVAL_FAIL;
            total += fsize;
        }
        if (!is_packed && max_align > 1 && (total % max_align) != 0)
            total += max_align - (total % max_align);
        return total > 0 ? total : -1;
    }
    return -1;
}

static Type *resolve_type_inner(Checker *c, TypeNode *tn);

/* RF3: resolve_type stores result in typemap so emitter can read via checker_get_type */
static Type *resolve_type(Checker *c, TypeNode *tn) {
    if (!tn) return ty_void;
    /* check cache first */
    Type *cached = typemap_get(c, (Node *)tn);
    if (cached) return cached;
    /* resolve and store */
    Type *t = resolve_type_inner(c, tn);
    if (t) typemap_set(c, (Node *)tn, t);
    return t;
}

static Type *resolve_type_inner(Checker *c, TypeNode *tn) {
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
    case TYNODE_OPAQUE: return ty_opaque;
    case TYNODE_ARENA:  return ty_arena;

    case TYNODE_POINTER: {
        Type *inner = resolve_type(c, tn->pointer.inner);
        /* BUG-372: *void is invalid — use *opaque for type-erased pointers */
        if (inner && inner->kind == TYPE_VOID) {
            checker_error(c, tn->loc.line,
                "cannot create pointer to void — use '*opaque' for type-erased pointers");
        }
        return type_pointer(c->arena, inner);
    }

    case TYNODE_OPTIONAL: {
        Type *inner = resolve_type(c, tn->optional.inner);
        if (inner->kind == TYPE_OPTIONAL) {
            checker_error(c, tn->loc.line,
                "nested optional '??T' is not supported");
            return inner; /* return the inner ?T, not ??T */
        }
        return type_optional(c->arena, inner);
    }

    case TYNODE_SLICE: {
        Type *inner = resolve_type(c, tn->slice.inner);
        /* BUG-372: []void is invalid — void has no size */
        if (inner && inner->kind == TYPE_VOID) {
            checker_error(c, tn->loc.line,
                "cannot create slice of void — void has no size");
        }
        return type_slice(c->arena, inner);
    }

    case TYNODE_ARRAY: {
        Type *elem = resolve_type(c, tn->array.elem);
        /* evaluate size expression — must be compile-time constant */
        Type *size_type = check_expr(c, tn->array.size_expr);
        if (size_type && !type_is_integer(size_type)) {
            checker_error(c, tn->loc.line, "array size must be an integer");
        }
        /* evaluate compile-time constant size expression */
        uint32_t size = 0;
        Type *size_of = NULL; /* resolved @size type, if any */
        if (tn->array.size_expr) {
            int64_t val = eval_const_expr(tn->array.size_expr);
            /* BUG-199: handle @size(T) as compile-time constant */
            if (val == CONST_EVAL_FAIL && tn->array.size_expr->kind == NODE_INTRINSIC &&
                tn->array.size_expr->intrinsic.name_len == 4 &&
                memcmp(tn->array.size_expr->intrinsic.name, "size", 4) == 0) {
                if (tn->array.size_expr->intrinsic.type_arg) {
                    size_of = resolve_type(c, tn->array.size_expr->intrinsic.type_arg);
                } else if (tn->array.size_expr->intrinsic.arg_count > 0 &&
                           tn->array.size_expr->intrinsic.args[0]->kind == NODE_IDENT) {
                    Symbol *sym = scope_lookup(c->current_scope,
                        tn->array.size_expr->intrinsic.args[0]->ident.name,
                        (uint32_t)tn->array.size_expr->intrinsic.args[0]->ident.name_len);
                    if (sym) size_of = sym->type;
                }
                if (size_of) {
                    Type *unwrapped = type_unwrap_distinct(size_of);
                    int w = type_width(unwrapped);
                    if (w > 0) {
                        val = w / 8;
                    } else if (unwrapped->kind == TYPE_STRUCT) {
                        val = compute_type_size(unwrapped);
                    } else {
                        /* fallback: use recursive compute for any type */
                        val = compute_type_size(unwrapped);
                    }
                }
            }
            if (val == CONST_EVAL_FAIL) {
                /* BUG-275: if @size on target-dependent type (pointer/slice/struct-with-ptr),
                 * don't error — store the resolved Type for emitter to emit sizeof(). */
                if (tn->array.size_expr->kind == NODE_INTRINSIC &&
                    tn->array.size_expr->intrinsic.name_len == 4 &&
                    memcmp(tn->array.size_expr->intrinsic.name, "size", 4) == 0 &&
                    size_of) {
                    Type *arr_type = type_array(c->arena, elem, 0);
                    arr_type->array.sizeof_type = size_of;
                    return arr_type;
                }
                checker_error(c, tn->loc.line, "array size must be a compile-time constant");
            } else if (val <= 0) {
                checker_error(c, tn->loc.line, "array size must be > 0");
            } else {
                /* BUG-247: reject sizes that overflow uint32_t (>4GB arrays) */
                if ((uint64_t)val > UINT32_MAX) {
                    checker_error(c, tn->loc.line,
                        "array size %lld exceeds maximum (4GB)",
                        (long long)val);
                }
                size = (uint32_t)val;
            }
        }
        return type_array(c->arena, elem, size);
    }

    case TYNODE_NAMED: {
        /* look up type name in scope (struct, enum, union, typedef) */
        Symbol *sym = scope_lookup(c->current_scope, tn->named.name, (uint32_t)tn->named.name_len);
        if (!sym) {
            checker_error(c, tn->loc.line, "undefined type '%.*s'",
                          (int)tn->named.name_len, tn->named.name);
            return ty_void;
        }
        return sym->type;
    }

    case TYNODE_POOL: {
        Type *elem = resolve_type(c, tn->pool.elem);
        uint32_t count = 0;
        if (tn->pool.count_expr) {
            int64_t val = eval_const_expr(tn->pool.count_expr);
            if (val > 0) count = (uint32_t)val;
            else checker_error(c, tn->loc.line, "Pool count must be a positive compile-time constant");
        }
        return type_pool(c->arena, elem, count);
    }

    case TYNODE_RING: {
        Type *elem = resolve_type(c, tn->ring.elem);
        uint32_t count = 0;
        if (tn->ring.count_expr) {
            int64_t val = eval_const_expr(tn->ring.count_expr);
            if (val > 0) count = (uint32_t)val;
            else checker_error(c, tn->loc.line, "Ring count must be a positive compile-time constant");
        }
        return type_ring(c->arena, elem, count);
    }

    case TYNODE_HANDLE: {
        Type *elem = resolve_type(c, tn->handle.elem);
        return type_handle(c->arena, elem);
    }

    case TYNODE_SLAB: {
        Type *elem = resolve_type(c, tn->slab.elem);
        return type_slab(c->arena, elem);
    }

    case TYNODE_FUNC_PTR: {
        Type *ret = resolve_type(c, tn->func_ptr.return_type);
        uint32_t pc = (uint32_t)tn->func_ptr.param_count;
        Type **params = NULL;
        if (pc > 0) {
            params = (Type **)arena_alloc(c->arena, pc * sizeof(Type *));
            for (uint32_t i = 0; i < pc; i++) {
                params[i] = resolve_type(c, tn->func_ptr.param_types[i]);
            }
        }
        Type *fpt = type_func_ptr(c->arena, params, pc, ret);
        /* carry keep flags from TypeNode to Type */
        if (tn->func_ptr.param_keeps) {
            fpt->func_ptr.param_keeps = (bool *)arena_alloc(c->arena, pc * sizeof(bool));
            memcpy(fpt->func_ptr.param_keeps, tn->func_ptr.param_keeps, pc * sizeof(bool));
        }
        return fpt;
    }

    case TYNODE_CONST: {
        Type *inner = resolve_type(c, tn->qualified.inner);
        /* propagate const through pointer/slice */
        if (inner->kind == TYPE_POINTER) {
            return type_const_pointer(c->arena, inner->pointer.inner);
        }
        if (inner->kind == TYPE_SLICE) {
            return type_const_slice(c->arena, inner->slice.inner);
        }
        /* for value types, const is a variable qualifier, not a type property */
        return inner;
    }

    case TYNODE_VOLATILE: {
        Type *inner = resolve_type(c, tn->qualified.inner);
        /* propagate volatile to pointer/slice type for codegen */
        if (inner && inner->kind == TYPE_POINTER) {
            Type *vp = type_pointer(c->arena, inner->pointer.inner);
            vp->pointer.is_volatile = true;
            if (inner->pointer.is_const) vp->pointer.is_const = true;
            return vp;
        }
        if (inner && inner->kind == TYPE_SLICE) {
            Type *vs = type_volatile_slice(c->arena, inner->slice.inner);
            if (inner->slice.is_const) vs->slice.is_const = true;
            return vs;
        }
        return inner;
    }
    }

    return ty_void;
}

/* ================================================================
 * EXPRESSION TYPE CHECKING
 *
 * Every expression resolves to a Type*.
 * Sets the type in the type map and returns it.
 * ================================================================ */

/* find the common type for binary arithmetic */
static Type *common_numeric_type(Checker *c, Type *a, Type *b, int line) {
    if (type_equals(a, b)) return a;

    /* both integer — widen to larger */
    if (type_is_integer(a) && type_is_integer(b)) {
        int wa = type_width(a);
        int wb = type_width(b);
        if (wa >= wb && can_implicit_coerce(b, a)) return a;
        if (wb > wa && can_implicit_coerce(a, b)) return b;
        checker_error(c, line,
            "cannot mix '%s' and '%s' — explicit conversion required",
            type_name(a), type_name(b));
        return a;
    }

    /* both float — widen to larger */
    if (type_is_float(a) && type_is_float(b)) {
        return (type_width(a) >= type_width(b)) ? a : b;
    }

    /* int + float not allowed in ZER */
    checker_error(c, line,
        "cannot mix integer '%s' and float '%s'",
        type_name(a), type_name(b));
    return a;
}

/* ---- comptime function evaluation ----
 * Evaluates a comptime function body with substituted argument values.
 * Supports: return expressions, if/else branching, nested blocks.
 * Uses eval_const_expr with a param→value substitution table. */

typedef struct { const char *name; uint32_t name_len; int64_t value; } ComptimeParam;

static int64_t eval_const_expr_subst(Node *n, ComptimeParam *params, int param_count) {
    if (!n) return CONST_EVAL_FAIL;
    /* substitute parameter references */
    if (n->kind == NODE_IDENT) {
        for (int i = 0; i < param_count; i++) {
            if (n->ident.name_len == params[i].name_len &&
                memcmp(n->ident.name, params[i].name, params[i].name_len) == 0)
                return params[i].value;
        }
        return CONST_EVAL_FAIL;
    }
    if (n->kind == NODE_INT_LIT) return (int64_t)n->int_lit.value;
    if (n->kind == NODE_BOOL_LIT) return n->bool_lit.value ? 1 : 0;
    if (n->kind == NODE_UNARY) {
        int64_t v = eval_const_expr_subst(n->unary.operand, params, param_count);
        if (v == CONST_EVAL_FAIL) return CONST_EVAL_FAIL;
        if (n->unary.op == TOK_MINUS) return -v;
        if (n->unary.op == TOK_TILDE) return ~v;
        if (n->unary.op == TOK_BANG)  return v ? 0 : 1;
        return CONST_EVAL_FAIL;
    }
    if (n->kind == NODE_BINARY) {
        int64_t l = eval_const_expr_subst(n->binary.left, params, param_count);
        int64_t r = eval_const_expr_subst(n->binary.right, params, param_count);
        if (l == CONST_EVAL_FAIL || r == CONST_EVAL_FAIL) return CONST_EVAL_FAIL;
        switch (n->binary.op) {
        case TOK_PLUS:   return l + r;
        case TOK_MINUS:  return l - r;
        case TOK_STAR:   return l * r;
        case TOK_SLASH:  return r == 0 ? CONST_EVAL_FAIL : l / r;
        case TOK_PERCENT: return r == 0 ? CONST_EVAL_FAIL : l % r;
        case TOK_LSHIFT: return r < 0 || r >= 63 ? CONST_EVAL_FAIL : (int64_t)((uint64_t)l << r);
        case TOK_RSHIFT: return r < 0 || r >= 63 ? CONST_EVAL_FAIL : l >> r;
        case TOK_AMP:    return l & r;
        case TOK_PIPE:   return l | r;
        case TOK_CARET:  return l ^ r;
        case TOK_GT:     return l > r ? 1 : 0;
        case TOK_LT:     return l < r ? 1 : 0;
        case TOK_GTEQ:   return l >= r ? 1 : 0;
        case TOK_LTEQ:   return l <= r ? 1 : 0;
        case TOK_EQEQ:   return l == r ? 1 : 0;
        case TOK_BANGEQ: return l != r ? 1 : 0;
        case TOK_AMPAMP: return (l && r) ? 1 : 0;
        case TOK_PIPEPIPE: return (l || r) ? 1 : 0;
        default: return CONST_EVAL_FAIL;
        }
    }
    return CONST_EVAL_FAIL;
}

static int64_t eval_comptime_stmt(Node *n, ComptimeParam *params, int param_count);

static int64_t eval_comptime_block(Node *block, ComptimeParam *params, int param_count) {
    if (!block) return CONST_EVAL_FAIL;
    if (block->kind == NODE_BLOCK) {
        for (int i = 0; i < block->block.stmt_count; i++) {
            int64_t r = eval_comptime_stmt(block->block.stmts[i], params, param_count);
            if (r != CONST_EVAL_FAIL) return r; /* hit a return */
        }
        return CONST_EVAL_FAIL;
    }
    return eval_comptime_stmt(block, params, param_count);
}

static int64_t eval_comptime_stmt(Node *n, ComptimeParam *params, int param_count) {
    if (!n) return CONST_EVAL_FAIL;
    if (n->kind == NODE_RETURN) {
        if (n->ret.expr)
            return eval_const_expr_subst(n->ret.expr, params, param_count);
        return CONST_EVAL_FAIL;
    }
    if (n->kind == NODE_IF) {
        int64_t cond = eval_const_expr_subst(n->if_stmt.cond, params, param_count);
        if (cond == CONST_EVAL_FAIL) return CONST_EVAL_FAIL;
        if (cond) {
            return eval_comptime_block(n->if_stmt.then_body, params, param_count);
        } else if (n->if_stmt.else_body) {
            return eval_comptime_block(n->if_stmt.else_body, params, param_count);
        }
        return CONST_EVAL_FAIL;
    }
    if (n->kind == NODE_BLOCK) {
        return eval_comptime_block(n, params, param_count);
    }
    return CONST_EVAL_FAIL;
}

static Type *check_expr(Checker *c, Node *node) {
    if (!node) return ty_void;

    /* recursion depth guard — prevents stack overflow on pathological input */
    if (++c->expr_depth > 1000) {
        checker_error(c, node->loc.line,
            "expression nesting too deep (limit 1000) — simplify expression");
        c->expr_depth--;
        return ty_void;
    }

    Type *result = NULL;

    switch (node->kind) {
    /* ---- Literals ---- */
    case NODE_INT_LIT:
        /* integer literals are polymorphic — assignable to any integer type.
         * Default to i32 for standalone use, but coercion handles the rest.
         * We use a special approach: literals coerce to any integer type. */
        result = ty_u32;
        break;

    case NODE_FLOAT_LIT:
        /* float literals coerce to f32 or f64 */
        result = ty_f64;
        break;

    case NODE_STRING_LIT:
        /* string literals are []u8 (const slice of bytes) */
        result = type_const_slice(c->arena, ty_u8);
        break;

    case NODE_CHAR_LIT:
        result = ty_u8;
        break;

    case NODE_BOOL_LIT:
        result = ty_bool;
        break;

    case NODE_NULL_LIT:
        /* null has no type by itself — needs context.
         * For now, use a sentinel. Type inference from assignment context. */
        result = ty_void; /* placeholder — checker resolves at assignment */
        break;

    /* ---- Identifier ---- */
    case NODE_IDENT: {
        Symbol *sym = find_symbol(c, node->ident.name, (uint32_t)node->ident.name_len,
                                  node->loc.line);
        result = sym ? sym->type : ty_void;
        break;
    }

    /* ---- Binary expression ---- */
    case NODE_BINARY: {
        Type *left = check_expr(c, node->binary.left);
        Type *right = check_expr(c, node->binary.right);

        /* literal promotion: when one side is a literal and the other is
         * a known type, the literal adopts the other side's type.
         * This allows: i32 x = -5; i32 y = x + 10; (10 becomes i32) */
        if (is_literal_compatible(node->binary.left, right)) left = right;
        if (is_literal_compatible(node->binary.right, left)) right = left;

        switch (node->binary.op) {
        /* arithmetic: both numeric, result = common type */
        case TOK_PLUS: case TOK_MINUS: case TOK_STAR:
        case TOK_SLASH: case TOK_PERCENT:
            if (!type_is_numeric(left) || !type_is_numeric(right)) {
                checker_error(c, node->loc.line,
                    "arithmetic requires numeric types, got '%s' and '%s'",
                    type_name(left), type_name(right));
                result = left;
            } else {
                /* compile-time division by zero check.
                 * BUG-269: use eval_const_expr to catch expressions like (2-2) */
                if (node->binary.op == TOK_SLASH || node->binary.op == TOK_PERCENT) {
                    int64_t div_val = eval_const_expr(node->binary.right);
                    if (div_val == 0) {
                        checker_error(c, node->loc.line, "division by zero");
                    }
                }
                result = common_numeric_type(c, left, right, node->loc.line);
            }
            break;

        /* comparison: both same type (or coercible), result = bool */
        case TOK_EQEQ: case TOK_BANGEQ:
        case TOK_LT: case TOK_GT: case TOK_LTEQ: case TOK_GTEQ:
            /* reject slice/array comparison — C compares struct/pointer, not content.
             * BUG-315: unwrap distinct before checking (distinct []u8 is still a slice). */
            if ((node->binary.op == TOK_EQEQ || node->binary.op == TOK_BANGEQ)) {
                Type *eff_l = type_unwrap_distinct(left);
                Type *eff_r = type_unwrap_distinct(right);
                if ((eff_l->kind == TYPE_SLICE || eff_l->kind == TYPE_ARRAY) ||
                    (eff_r->kind == TYPE_SLICE || eff_r->kind == TYPE_ARRAY)) {
                    checker_error(c, node->loc.line,
                        "cannot compare '%s' with == — use element-wise comparison",
                        type_name(eff_l->kind == TYPE_SLICE || eff_l->kind == TYPE_ARRAY ? left : right));
                }
            }
            if (!type_equals(left, right) &&
                !can_implicit_coerce(left, right) &&
                !can_implicit_coerce(right, left)) {
                checker_error(c, node->loc.line,
                    "cannot compare '%s' and '%s'",
                    type_name(left), type_name(right));
            }
            result = ty_bool;
            break;

        /* logical: both bool, result = bool */
        case TOK_AMPAMP: case TOK_PIPEPIPE:
            if (!type_equals(left, ty_bool) || !type_equals(right, ty_bool)) {
                checker_error(c, node->loc.line,
                    "logical operators require bool, got '%s' and '%s'",
                    type_name(left), type_name(right));
            }
            result = ty_bool;
            break;

        /* bitwise: both integer, result = common type */
        case TOK_AMP: case TOK_PIPE: case TOK_CARET:
        case TOK_LSHIFT: case TOK_RSHIFT:
            if (!type_is_integer(left) || !type_is_integer(right)) {
                checker_error(c, node->loc.line,
                    "bitwise operators require integers, got '%s' and '%s'",
                    type_name(left), type_name(right));
                result = left;
            } else {
                result = common_numeric_type(c, left, right, node->loc.line);
            }
            break;

        default:
            result = left;
            break;
        }
        break;
    }

    /* ---- Unary expression ---- */
    case NODE_UNARY: {
        Type *operand = check_expr(c, node->unary.operand);

        switch (node->unary.op) {
        case TOK_MINUS:
            if (!type_is_numeric(operand)) {
                checker_error(c, node->loc.line,
                    "unary '-' requires numeric type, got '%s'", type_name(operand));
            }
            result = operand;
            break;

        case TOK_BANG:
            if (!type_equals(operand, ty_bool)) {
                checker_error(c, node->loc.line,
                    "'!' requires bool, got '%s'", type_name(operand));
            }
            result = ty_bool;
            break;

        case TOK_TILDE:
            if (!type_is_integer(operand)) {
                checker_error(c, node->loc.line,
                    "'~' requires integer, got '%s'", type_name(operand));
            }
            result = operand;
            break;

        case TOK_STAR: /* dereference */
            if (operand->kind != TYPE_POINTER) {
                checker_error(c, node->loc.line,
                    "cannot dereference non-pointer type '%s'", type_name(operand));
                result = ty_void;
            } else {
                result = operand->pointer.inner;
            }
            break;

        case TOK_AMP: /* address-of */
            result = type_pointer(c->arena, operand);
            /* BUG-197/228/254: walk operand to root for volatile/const propagation.
             * Handles &ident, &arr[i], &s.field, &s.arr[i].field etc. */
            {
                Node *root = node->unary.operand;
                while (root) {
                    if (root->kind == NODE_FIELD) root = root->field.object;
                    else if (root->kind == NODE_INDEX) root = root->index_expr.object;
                    else break;
                }
                if (root && root->kind == NODE_IDENT) {
                    Symbol *sym = scope_lookup(c->current_scope,
                        root->ident.name, (uint32_t)root->ident.name_len);
                    if (sym && sym->is_volatile) {
                        result->pointer.is_volatile = true;
                    }
                    if (sym && sym->is_const) {
                        result->pointer.is_const = true;
                    }
                    /* BUG-208: block &union_var inside mutable capture arm. */
                    if (c->union_switch_var &&
                        root->ident.name_len == c->union_switch_var_len &&
                        memcmp(root->ident.name, c->union_switch_var,
                               c->union_switch_var_len) == 0) {
                        checker_error(c, node->loc.line,
                            "cannot take address of union '%.*s' inside its switch arm — "
                            "pointer alias would bypass variant lock",
                            (int)c->union_switch_var_len, c->union_switch_var);
                    }
                }
            }
            break;

        default:
            result = operand;
            break;
        }
        break;
    }

    /* ---- Assignment ---- */
    case NODE_ASSIGN: {
        c->in_assign_target = true;
        Type *target = check_expr(c, node->assign.target);
        c->in_assign_target = false;
        Type *value = check_expr(c, node->assign.value);

        /* BUG-294/302: reject assignment to non-lvalue.
         * Walk field/index chains to find base — if it's a call returning a value
         * type (not a pointer, which auto-derefs to lvalue), reject.
         * Deref (*expr) makes it an lvalue, so stop walking at NODE_UNARY. */
        {
            Node *t = node->assign.target;
            while (t) {
                if (t->kind == NODE_FIELD) t = t->field.object;
                else if (t->kind == NODE_INDEX) t = t->index_expr.object;
                else break;
            }
            if (t && (t->kind == NODE_INT_LIT || t->kind == NODE_STRING_LIT ||
                t->kind == NODE_NULL_LIT || t->kind == NODE_BOOL_LIT)) {
                checker_error(c, node->loc.line,
                    "cannot assign to expression — not an lvalue");
            }
            /* function call: only reject if it returns a value type (not pointer) */
            if (t && t->kind == NODE_CALL) {
                Type *call_ret = checker_get_type(c, t);
                if (call_ret && call_ret->kind != TYPE_POINTER) {
                    checker_error(c, node->loc.line,
                        "cannot assign to expression — not an lvalue");
                }
            }
        }

        /* BUG-248: block direct assignment to union variable during switch capture.
         * msg = other_msg changes tag+data, invalidating capture pointer. */
        if (c->union_switch_var && node->assign.op == TOK_EQ) {
            Node *troot = node->assign.target;
            bool locked_via_alias = false;
            while (troot) {
                /* BUG-337: check if chain goes through a pointer to the locked union type.
                 * s.ptr.b where ptr is *U — ptr could alias the locked union.
                 * Only triggers when the field's object type is a pointer to the locked union. */
                if (c->union_switch_type && troot->kind == NODE_FIELD) {
                    Type *obj_type = checker_get_type(c, troot->field.object);
                    if (!obj_type) obj_type = check_expr(c, troot->field.object);
                    Type *unwrapped = type_unwrap_distinct(obj_type);
                    /* check if the object is a pointer to the locked union */
                    if (unwrapped && unwrapped->kind == TYPE_POINTER) {
                        Type *inner = type_unwrap_distinct(unwrapped->pointer.inner);
                        if (inner == c->union_switch_type)
                            locked_via_alias = true;
                    }
                }
                if (troot->kind == NODE_UNARY && troot->unary.op == TOK_STAR)
                    troot = troot->unary.operand;
                else if (troot->kind == NODE_FIELD)
                    troot = troot->field.object;
                else if (troot->kind == NODE_INDEX)
                    troot = troot->index_expr.object;
                else break;
            }
            if (troot && troot->kind == NODE_IDENT &&
                (locked_via_alias ||
                 (troot->ident.name_len == c->union_switch_var_len &&
                  memcmp(troot->ident.name, c->union_switch_var,
                         c->union_switch_var_len) == 0))) {
                checker_error(c, node->loc.line,
                    "cannot assign to union '%.*s' inside its switch arm — "
                    "active capture would become invalid",
                    (int)c->union_switch_var_len, c->union_switch_var);
            }
        }

        /* const check: cannot assign to const variable, const pointer target, or fields thereof */
        {
            Node *root = node->assign.target;
            bool through_pointer = false;
            bool through_const_pointer = false;
            /* walk field/index chain to find root ident */
            while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX ||
                            root->kind == NODE_UNARY)) {
                if (root->kind == NODE_UNARY && root->unary.op == TOK_STAR) {
                    /* deref: *p = val — check if p's type is const pointer */
                    Type *ptr_type = typemap_get(c, root->unary.operand);
                    if (ptr_type && ptr_type->kind == TYPE_POINTER && ptr_type->pointer.is_const) {
                        through_const_pointer = true;
                    }
                    through_pointer = true;
                    root = root->unary.operand;
                } else if (root->kind == NODE_FIELD) {
                    /* check if object is a pointer (auto-deref) */
                    Type *obj_type = typemap_get(c, root->field.object);
                    if (obj_type && obj_type->kind == TYPE_POINTER) {
                        through_pointer = true;
                        if (obj_type->pointer.is_const)
                            through_const_pointer = true;
                    }
                    root = root->field.object;
                } else {
                    root = root->index_expr.object;
                }
            }
            if (through_const_pointer) {
                checker_error(c, node->loc.line,
                    "cannot write through const pointer — data is read-only");
            }
            if (root && root->kind == NODE_IDENT && !through_pointer) {
                Symbol *sym = scope_lookup(c->current_scope,
                    root->ident.name, (uint32_t)root->ident.name_len);
                if (sym && sym->is_const) {
                    checker_error(c, node->loc.line,
                        "cannot assign to const variable '%.*s'",
                        (int)sym->name_len, sym->name);
                }
            }
        }

        /* non-storable check: pool.get(h) result cannot be stored */
        if (node->assign.op == TOK_EQ && is_non_storable(c, node->assign.value)) {
            checker_error(c, node->loc.line,
                "cannot store result of get() — use inline");
        }

        /* BUG-225: reject Pool/Ring/Slab assignment — unique resource types */
        if (node->assign.op == TOK_EQ && target &&
            (target->kind == TYPE_POOL || target->kind == TYPE_RING || target->kind == TYPE_SLAB)) {
            checker_error(c, node->loc.line,
                "cannot assign %s — resource types are not copyable",
                target->kind == TYPE_POOL ? "Pool" : target->kind == TYPE_RING ? "Ring" : "Slab");
        }

        /* string literal to mutable slice: runtime crash on write */
        if (node->assign.op == TOK_EQ &&
            node->assign.value->kind == NODE_STRING_LIT &&
            target && target->kind == TYPE_SLICE) {
            checker_error(c, node->loc.line,
                "string literal is read-only — use 'const []u8' for string storage");
        }

        /* scope escape: storing &local in static/global variable (or field thereof) */
        if (node->assign.op == TOK_EQ &&
            node->assign.value->kind == NODE_UNARY &&
            node->assign.value->unary.op == TOK_AMP &&
            node->assign.value->unary.operand->kind == NODE_IDENT) {
            /* Walk target chain (field/index/deref) to find root identifier */
            Node *root = node->assign.target;
            while (root) {
                if (root->kind == NODE_FIELD) root = root->field.object;
                else if (root->kind == NODE_INDEX) root = root->index_expr.object;
                else if (root->kind == NODE_UNARY && root->unary.op == TOK_STAR)
                    root = root->unary.operand;
                else break;
            }
            if (root && root->kind == NODE_IDENT) {
                Symbol *target_sym = scope_lookup(c->current_scope,
                    root->ident.name, (uint32_t)root->ident.name_len);
                Symbol *val_sym = scope_lookup(c->current_scope,
                    node->assign.value->unary.operand->ident.name,
                    (uint32_t)node->assign.value->unary.operand->ident.name_len);
                bool val_is_global = val_sym &&
                    scope_lookup_local(c->global_scope, val_sym->name, val_sym->name_len) != NULL;
                bool target_is_static = target_sym && target_sym->is_static;
                bool target_is_global = target_sym &&
                    scope_lookup_local(c->global_scope, target_sym->name, target_sym->name_len) != NULL;
                /* BUG-230/290: pointer parameter deref/fields can alias globals — treat as escape.
                 * Catches: p->field = &local, *p = &local, **p = &local */
                bool target_is_param_ptr = false;
                if (target_sym && !target_is_static && !target_is_global &&
                    target_sym->type && target_sym->type->kind == TYPE_POINTER) {
                    /* target involves dereferencing a pointer param — escapes to caller */
                    Node *t = node->assign.target;
                    if (t->kind == NODE_FIELD || t->kind == NODE_INDEX ||
                        (t->kind == NODE_UNARY && t->unary.op == TOK_STAR)) {
                        target_is_param_ptr = true;
                    }
                }
                if ((target_is_static || target_is_global || target_is_param_ptr) && val_sym &&
                    !val_sym->is_static && !val_is_global) {
                    checker_error(c, node->loc.line,
                        target_is_param_ptr ?
                        "cannot store pointer to local '%.*s' through pointer parameter '%.*s' — "
                        "may escape to caller's scope" :
                        "cannot store pointer to local '%.*s' in static/global variable '%.*s'",
                        (int)val_sym->name_len, val_sym->name,
                        (int)target_sym->name_len, target_sym->name);
                }
            }
        }

        /* BUG-194: On plain assignment, clear+recompute safety flags on target.
         * Without this: p = &local; p = &global; return p → false positive.
         * Also: p = &global; p = &local; return p → false negative. */
        if (node->assign.op == TOK_EQ) {
            Node *troot = node->assign.target;
            while (troot && (troot->kind == NODE_FIELD || troot->kind == NODE_INDEX)) {
                if (troot->kind == NODE_FIELD) troot = troot->field.object;
                else troot = troot->index_expr.object;
            }
            if (troot && troot->kind == NODE_IDENT) {
                Symbol *tsym = scope_lookup(c->current_scope,
                    troot->ident.name, (uint32_t)troot->ident.name_len);
                if (tsym) {
                    /* clear — will be re-set below if new value is unsafe */
                    tsym->is_local_derived = false;
                    tsym->is_arena_derived = false;
                    tsym->provenance_type = NULL;
                    tsym->container_struct = NULL;
                    tsym->container_field = NULL;
                    tsym->container_field_len = 0;
                    /* check if new value is &local — also check orelse fallback (BUG-314) */
                    {
                        Node *vcheck = node->assign.value;
                        /* BUG-314: walk into orelse — check both expr and fallback */
                        if (vcheck->kind == NODE_ORELSE) {
                            Node *fb = vcheck->orelse.fallback;
                            if (fb && fb->kind == NODE_UNARY &&
                                fb->unary.op == TOK_AMP &&
                                fb->unary.operand->kind == NODE_IDENT) {
                                Symbol *src = scope_lookup(c->current_scope,
                                    fb->unary.operand->ident.name,
                                    (uint32_t)fb->unary.operand->ident.name_len);
                                bool src_is_global = src && scope_lookup_local(c->global_scope,
                                    src->name, src->name_len) != NULL;
                                if (src && !src->is_static && !src_is_global)
                                    tsym->is_local_derived = true;
                            }
                            if (fb && fb->kind == NODE_IDENT) {
                                Symbol *src = scope_lookup(c->current_scope,
                                    fb->ident.name, (uint32_t)fb->ident.name_len);
                                if (src && src->is_local_derived) tsym->is_local_derived = true;
                                if (src && src->is_arena_derived) tsym->is_arena_derived = true;
                            }
                            vcheck = vcheck->orelse.expr;
                        }
                        if (vcheck->kind == NODE_UNARY &&
                            vcheck->unary.op == TOK_AMP &&
                            vcheck->unary.operand->kind == NODE_IDENT) {
                            Symbol *src = scope_lookup(c->current_scope,
                                vcheck->unary.operand->ident.name,
                                (uint32_t)vcheck->unary.operand->ident.name_len);
                            bool src_is_global = src && scope_lookup_local(c->global_scope,
                                src->name, src->name_len) != NULL;
                            if (src && !src->is_static && !src_is_global)
                                tsym->is_local_derived = true;
                        }
                    }
                    /* check if new value is an alias of local/arena-derived */
                    {
                        Node *vcheck = node->assign.value;
                        if (vcheck->kind == NODE_ORELSE) vcheck = vcheck->orelse.expr;
                        if (vcheck->kind == NODE_IDENT) {
                            Symbol *src = scope_lookup(c->current_scope,
                                vcheck->ident.name, (uint32_t)vcheck->ident.name_len);
                            if (src && src->is_local_derived) tsym->is_local_derived = true;
                            if (src && src->is_arena_derived) tsym->is_arena_derived = true;
                            /* provenance propagation through alias */
                            if (src && src->provenance_type) tsym->provenance_type = src->provenance_type;
                            if (src && src->container_struct) {
                                tsym->container_struct = src->container_struct;
                                tsym->container_field = src->container_field;
                                tsym->container_field_len = src->container_field_len;
                            }
                        }
                        /* @ptrcast provenance on assignment */
                        if (vcheck->kind == NODE_INTRINSIC &&
                            vcheck->intrinsic.name_len == 7 &&
                            memcmp(vcheck->intrinsic.name, "ptrcast", 7) == 0 &&
                            vcheck->intrinsic.arg_count > 0) {
                            Type *tgt_eff = type_unwrap_distinct(tsym->type);
                            if (tgt_eff && tgt_eff->kind == TYPE_POINTER &&
                                tgt_eff->pointer.inner->kind == TYPE_OPAQUE) {
                                Type *src_type = typemap_get(c, vcheck->intrinsic.args[0]);
                                if (src_type) tsym->provenance_type = src_type;
                            }
                        }
                        /* @container provenance: val = &struct.field */
                        if (vcheck->kind == NODE_UNARY && vcheck->unary.op == TOK_AMP &&
                            vcheck->unary.operand->kind == NODE_FIELD) {
                            Node *fn = vcheck->unary.operand;
                            Type *ot = typemap_get(c, fn->field.object);
                            if (ot) {
                                Type *st = type_unwrap_distinct(ot);
                                if (st && st->kind == TYPE_POINTER) st = type_unwrap_distinct(st->pointer.inner);
                                if (st && st->kind == TYPE_STRUCT) {
                                    tsym->container_struct = st;
                                    tsym->container_field = fn->field.field_name;
                                    tsym->container_field_len = (uint32_t)fn->field.field_name_len;
                                }
                            }
                        }
                    }
                }
            }
        }

        /* BUG-205/355: local-derived escape via assignment to global/static.
         * After flag propagation, check if target is global/static with local-derived value.
         * BUG-355: also walk through intrinsics (@ptrcast, @bitcast, @cast) to find root ident. */
        if (node->assign.op == TOK_EQ) {
            Node *vnode = node->assign.value;
            /* BUG-355: walk through intrinsics to find root ident */
            while (vnode && vnode->kind == NODE_INTRINSIC && vnode->intrinsic.arg_count > 0)
                vnode = vnode->intrinsic.args[vnode->intrinsic.arg_count - 1];
            if (vnode && vnode->kind == NODE_IDENT) {
                Symbol *val_sym = scope_lookup(c->current_scope,
                    vnode->ident.name, (uint32_t)vnode->ident.name_len);
                if (val_sym && val_sym->is_local_derived) {
                    Node *troot = node->assign.target;
                    while (troot && (troot->kind == NODE_FIELD || troot->kind == NODE_INDEX)) {
                        if (troot->kind == NODE_FIELD) troot = troot->field.object;
                        else troot = troot->index_expr.object;
                    }
                    if (troot && troot->kind == NODE_IDENT) {
                        Symbol *target_sym = scope_lookup(c->current_scope,
                            troot->ident.name, (uint32_t)troot->ident.name_len);
                        bool target_is_global = target_sym &&
                            (target_sym->is_static ||
                             scope_lookup_local(c->global_scope, target_sym->name,
                                                target_sym->name_len) != NULL);
                        if (target_is_global) {
                            checker_error(c, node->loc.line,
                                "cannot store local-derived pointer '%.*s' in "
                                "global/static variable '%.*s' — pointer will dangle "
                                "when function returns",
                                (int)val_sym->name_len, val_sym->name,
                                (int)target_sym->name_len, target_sym->name);
                        }
                    }
                }
            }
        }

        /* BUG-314: orelse &local escape to global via assignment.
         * g_ptr = opt orelse &local — if target is global, reject directly. */
        if (node->assign.op == TOK_EQ &&
            node->assign.value->kind == NODE_ORELSE) {
            Node *fb = node->assign.value->orelse.fallback;
            bool fb_is_local = false;
            if (fb && fb->kind == NODE_UNARY && fb->unary.op == TOK_AMP &&
                fb->unary.operand->kind == NODE_IDENT) {
                Symbol *src = scope_lookup(c->current_scope,
                    fb->unary.operand->ident.name,
                    (uint32_t)fb->unary.operand->ident.name_len);
                bool src_is_global = src && (src->is_static ||
                    scope_lookup_local(c->global_scope, src->name, src->name_len) != NULL);
                if (src && !src_is_global) fb_is_local = true;
            }
            if (fb && fb->kind == NODE_IDENT) {
                Symbol *src = scope_lookup(c->current_scope,
                    fb->ident.name, (uint32_t)fb->ident.name_len);
                if (src && src->is_local_derived) fb_is_local = true;
            }
            if (fb_is_local) {
                Node *troot = node->assign.target;
                while (troot && (troot->kind == NODE_FIELD || troot->kind == NODE_INDEX)) {
                    if (troot->kind == NODE_FIELD) troot = troot->field.object;
                    else troot = troot->index_expr.object;
                }
                if (troot && troot->kind == NODE_IDENT) {
                    Symbol *ts = scope_lookup(c->current_scope,
                        troot->ident.name, (uint32_t)troot->ident.name_len);
                    bool tgt_global = ts && (ts->is_static ||
                        scope_lookup_local(c->global_scope, ts->name, ts->name_len) != NULL);
                    if (tgt_global) {
                        checker_error(c, node->loc.line,
                            "orelse fallback stores local pointer in global — "
                            "pointer will dangle after function returns");
                    }
                }
            }
        }

        /* BUG-260: local-derived escape via dereferenced function call.
         * *pool.get(h) = &local or *func() = local_derived — function calls may
         * return pointers to global memory, so storing local pointers through them
         * is an escape. Walk target through deref/field/index; if root is NODE_CALL,
         * reject local-derived values. */
        if (node->assign.op == TOK_EQ) {
            Node *troot = node->assign.target;
            while (troot) {
                if (troot->kind == NODE_FIELD) troot = troot->field.object;
                else if (troot->kind == NODE_INDEX) troot = troot->index_expr.object;
                else if (troot->kind == NODE_UNARY && troot->unary.op == TOK_STAR)
                    troot = troot->unary.operand;
                else break;
            }
            if (troot && troot->kind == NODE_CALL) {
                /* target root is a function call — check if value is &local or local-derived */
                if (node->assign.value->kind == NODE_UNARY &&
                    node->assign.value->unary.op == TOK_AMP) {
                    Node *vroot = node->assign.value->unary.operand;
                    while (vroot && (vroot->kind == NODE_FIELD || vroot->kind == NODE_INDEX)) {
                        if (vroot->kind == NODE_FIELD) vroot = vroot->field.object;
                        else vroot = vroot->index_expr.object;
                    }
                    if (vroot && vroot->kind == NODE_IDENT) {
                        Symbol *src = scope_lookup(c->current_scope,
                            vroot->ident.name, (uint32_t)vroot->ident.name_len);
                        bool src_global = src && (src->is_static ||
                            scope_lookup_local(c->global_scope, src->name, src->name_len) != NULL);
                        if (src && !src_global) {
                            checker_error(c, node->loc.line,
                                "cannot store pointer to local '%.*s' through function call — "
                                "may escape to global memory",
                                (int)src->name_len, src->name);
                        }
                    }
                }
                if (node->assign.value->kind == NODE_IDENT) {
                    Symbol *val_sym = scope_lookup(c->current_scope,
                        node->assign.value->ident.name,
                        (uint32_t)node->assign.value->ident.name_len);
                    if (val_sym && val_sym->is_local_derived) {
                        checker_error(c, node->loc.line,
                            "cannot store local-derived pointer '%.*s' through function call — "
                            "may escape to global memory",
                            (int)val_sym->name_len, val_sym->name);
                    }
                }
            }
        }

        /* BUG-240/377: nested array→slice escape via assignment to global/static.
         * global_s = s.arr where s is local — walk value chain to root.
         * BUG-377: also check orelse fallback — g_slice = opt orelse local_buf. */
        if (node->assign.op == TOK_EQ && target && target->kind == TYPE_SLICE) {
            /* collect value nodes to check: direct value + orelse fallback */
            Node *arr_checks[2] = { NULL, NULL };
            int arr_check_count = 0;
            if (value && value->kind == TYPE_ARRAY) {
                arr_checks[arr_check_count++] = node->assign.value;
            }
            /* BUG-377: orelse fallback may be a local array */
            if (node->assign.value->kind == NODE_ORELSE &&
                node->assign.value->orelse.fallback) {
                Node *fb = node->assign.value->orelse.fallback;
                Type *fb_type = typemap_get(c, fb);
                if (fb_type && type_unwrap_distinct(fb_type)->kind == TYPE_ARRAY) {
                    arr_checks[arr_check_count++] = fb;
                }
            }
            for (int aci = 0; aci < arr_check_count; aci++) {
                Node *vroot = arr_checks[aci];
                while (vroot && (vroot->kind == NODE_FIELD || vroot->kind == NODE_INDEX)) {
                    if (vroot->kind == NODE_FIELD) vroot = vroot->field.object;
                    else vroot = vroot->index_expr.object;
                }
                if (vroot && vroot->kind == NODE_IDENT) {
                    Symbol *vsym = scope_lookup(c->current_scope,
                        vroot->ident.name, (uint32_t)vroot->ident.name_len);
                    bool val_is_global = vsym &&
                        (vsym->is_static || scope_lookup_local(c->global_scope,
                            vsym->name, vsym->name_len) != NULL);
                    if (vsym && !val_is_global) {
                        /* value root is local — check if target is global/static */
                        Node *troot = node->assign.target;
                        while (troot && (troot->kind == NODE_FIELD || troot->kind == NODE_INDEX)) {
                            if (troot->kind == NODE_FIELD) troot = troot->field.object;
                            else troot = troot->index_expr.object;
                        }
                        if (troot && troot->kind == NODE_IDENT) {
                            Symbol *tsym = scope_lookup(c->current_scope,
                                troot->ident.name, (uint32_t)troot->ident.name_len);
                            bool tgt_is_global = tsym &&
                                (tsym->is_static || scope_lookup_local(c->global_scope,
                                    tsym->name, tsym->name_len) != NULL);
                            if (tgt_is_global) {
                                checker_error(c, node->loc.line,
                                    "cannot store local array as slice in global/static — "
                                    "pointer will dangle after function returns");
                            }
                        }
                    }
                }
            }
        }

        /* arena lifetime escape: storing arena-derived pointer in global/static
         * Also propagates is_arena_derived through assignment targets */
        /* detect direct arena.alloc() in assignment value (including via orelse) */
        if (node->assign.op == TOK_EQ) {
            Node *alloc_call = node->assign.value;
            if (alloc_call->kind == NODE_ORELSE)
                alloc_call = alloc_call->orelse.expr;
            if (alloc_call && alloc_call->kind == NODE_CALL &&
                alloc_call->call.callee->kind == NODE_FIELD) {
                Node *obj = alloc_call->call.callee->field.object;
                const char *mname = alloc_call->call.callee->field.field_name;
                size_t mlen = alloc_call->call.callee->field.field_name_len;
                if (obj && ((mlen == 5 && memcmp(mname, "alloc", 5) == 0) ||
                           (mlen == 11 && memcmp(mname, "alloc_slice", 11) == 0))) {
                    Type *obj_type = typemap_get(c, obj);
                    if (obj_type && obj_type->kind == TYPE_ARENA) {
                        bool arena_is_global = false;
                        if (obj->kind == NODE_IDENT) {
                            arena_is_global = scope_lookup_local(c->global_scope,
                                obj->ident.name, (uint32_t)obj->ident.name_len) != NULL;
                        }
                        if (!arena_is_global) {
                            /* mark target root as arena-derived */
                            Node *root = node->assign.target;
                            while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                                if (root->kind == NODE_FIELD) root = root->field.object;
                                else root = root->index_expr.object;
                            }
                            if (root && root->kind == NODE_IDENT) {
                                Symbol *tsym = scope_lookup(c->current_scope,
                                    root->ident.name, (uint32_t)root->ident.name_len);
                                if (tsym) tsym->is_arena_derived = true;
                            }
                        }
                    }
                }
            }
        }
        if (node->assign.op == TOK_EQ &&
            node->assign.value->kind == NODE_IDENT) {
            Symbol *val_sym = scope_lookup(c->current_scope,
                node->assign.value->ident.name,
                (uint32_t)node->assign.value->ident.name_len);
            if (val_sym && val_sym->is_arena_derived) {
                /* walk target to find root */
                Node *root = node->assign.target;
                while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                    if (root->kind == NODE_FIELD) root = root->field.object;
                    else root = root->index_expr.object;
                }
                if (root && root->kind == NODE_IDENT) {
                    Symbol *target_sym = scope_lookup(c->current_scope,
                        root->ident.name, (uint32_t)root->ident.name_len);
                    bool target_is_global = target_sym &&
                        (target_sym->is_static ||
                         scope_lookup_local(c->global_scope, target_sym->name,
                                            target_sym->name_len) != NULL);
                    if (target_is_global) {
                        checker_error(c, node->loc.line,
                            "cannot store arena-derived pointer '%.*s' in "
                            "global/static variable '%.*s' — pointer will dangle "
                            "when arena is reset",
                            (int)val_sym->name_len, val_sym->name,
                            (int)target_sym->name_len, target_sym->name);
                    }
                    /* propagate arena-derived flag to target (alias tracking) */
                    if (target_sym && !target_is_global) {
                        target_sym->is_arena_derived = true;
                    }
                }
            }
        }

        /* scope escape: assigning local array to global slice (implicit coercion) */
        if (node->assign.op == TOK_EQ &&
            target && target->kind == TYPE_SLICE &&
            value && value->kind == TYPE_ARRAY &&
            node->assign.value->kind == NODE_IDENT) {
            const char *vname = node->assign.value->ident.name;
            uint32_t vlen = (uint32_t)node->assign.value->ident.name_len;
            Symbol *val_sym = scope_lookup(c->current_scope, vname, vlen);
            bool val_is_global = val_sym &&
                scope_lookup_local(c->global_scope, vname, vlen) != NULL;
            if (val_sym && !val_sym->is_static && !val_is_global) {
                /* check if target is global/static */
                Node *root = node->assign.target;
                while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                    if (root->kind == NODE_FIELD) root = root->field.object;
                    else root = root->index_expr.object;
                }
                if (root && root->kind == NODE_IDENT) {
                    Symbol *tgt_sym = scope_lookup(c->current_scope,
                        root->ident.name, (uint32_t)root->ident.name_len);
                    bool tgt_is_global = tgt_sym &&
                        (tgt_sym->is_static ||
                         scope_lookup_local(c->global_scope, tgt_sym->name, tgt_sym->name_len) != NULL);
                    if (tgt_is_global) {
                        checker_error(c, node->loc.line,
                            "cannot store local array '%.*s' in global/static slice — "
                            "pointer will dangle after function returns",
                            (int)vlen, vname);
                    }
                }
            }
        }

        /* const/volatile laundering: reject qualified → unqualified assignment */
        if (node->assign.op == TOK_EQ && target && value) {
            if (target->kind == TYPE_POINTER && value->kind == TYPE_POINTER &&
                value->pointer.is_const && !target->pointer.is_const) {
                checker_error(c, node->loc.line,
                    "cannot assign const pointer to mutable — would allow writing to read-only memory");
            }
            if (target->kind == TYPE_SLICE && value->kind == TYPE_SLICE &&
                value->slice.is_const && !target->slice.is_const) {
                checker_error(c, node->loc.line,
                    "cannot assign const slice to mutable — would allow writing to read-only memory");
            }
            /* BUG-282: volatile pointer → non-volatile assignment */
            if (target->kind == TYPE_POINTER && value->kind == TYPE_POINTER &&
                !target->pointer.is_volatile) {
                bool val_volatile = value->pointer.is_volatile;
                if (!val_volatile && node->assign.value->kind == NODE_IDENT) {
                    Symbol *vs = scope_lookup(c->current_scope,
                        node->assign.value->ident.name,
                        (uint32_t)node->assign.value->ident.name_len);
                    if (vs && vs->is_volatile) val_volatile = true;
                }
                /* check target too — if target sym is volatile, it's fine */
                bool tgt_volatile = target->pointer.is_volatile;
                if (!tgt_volatile) {
                    Node *tr = node->assign.target;
                    while (tr && (tr->kind == NODE_FIELD || tr->kind == NODE_INDEX))
                        tr = tr->kind == NODE_FIELD ? tr->field.object : tr->index_expr.object;
                    if (tr && tr->kind == NODE_IDENT) {
                        Symbol *ts = scope_lookup(c->current_scope,
                            tr->ident.name, (uint32_t)tr->ident.name_len);
                        if (ts && ts->is_volatile) tgt_volatile = true;
                    }
                }
                if (val_volatile && !tgt_volatile) {
                    checker_error(c, node->loc.line,
                        "cannot assign volatile pointer to non-volatile — "
                        "writes may be optimized away");
                }
            }
        }

        /* BUG-245: const array → mutable slice assignment blocked */
        if (node->assign.op == TOK_EQ &&
            target && target->kind == TYPE_SLICE && !target->slice.is_const &&
            value && value->kind == TYPE_ARRAY) {
            /* look up value symbol to check is_const */
            Node *vroot = node->assign.value;
            while (vroot && (vroot->kind == NODE_FIELD || vroot->kind == NODE_INDEX)) {
                if (vroot->kind == NODE_FIELD) vroot = vroot->field.object;
                else vroot = vroot->index_expr.object;
            }
            if (vroot && vroot->kind == NODE_IDENT) {
                Symbol *vsym = scope_lookup(c->current_scope,
                    vroot->ident.name, (uint32_t)vroot->ident.name_len);
                if (vsym && vsym->is_const) {
                    checker_error(c, node->loc.line,
                        "cannot assign const array to mutable slice — "
                        "would allow writing to read-only memory");
                }
                /* BUG-310: volatile array → volatile slice propagation.
                 * If source is volatile array and target is non-volatile slice, reject. */
                if (vsym && vsym->is_volatile &&
                    target->kind == TYPE_SLICE && !target->slice.is_volatile) {
                    checker_error(c, node->loc.line,
                        "cannot assign volatile array to non-volatile slice — "
                        "use 'volatile []%s' to preserve volatile qualifier",
                        type_name(target->slice.inner));
                }
            }
        }

        /* check type compatibility */
        if (node->assign.op == TOK_EQ) {
            if (!type_equals(target, value) &&
                !can_implicit_coerce(value, target) &&
                !is_literal_compatible(node->assign.value, target)) {
                checker_error(c, node->loc.line,
                    "cannot assign '%s' to '%s'",
                    type_name(value), type_name(target));
            }
            /* BUG-373: integer literal range check on assignment */
            if (node->assign.value->kind == NODE_INT_LIT &&
                target && type_is_integer(type_unwrap_distinct(target))) {
                if (!is_literal_compatible(node->assign.value, target)) {
                    checker_error(c, node->loc.line,
                        "integer literal %llu does not fit in '%s'",
                        (unsigned long long)node->assign.value->int_lit.value,
                        type_name(target));
                }
            }
        } else {
            /* compound assignment: += -= etc. — both must be numeric */
            if (!type_is_numeric(target) || !type_is_numeric(value)) {
                checker_error(c, node->loc.line,
                    "compound assignment requires numeric types");
            }
            /* bitwise compound (&= |= ^= <<= >>=) require integer, not float */
            if (node->assign.op == TOK_AMPEQ || node->assign.op == TOK_PIPEEQ ||
                node->assign.op == TOK_CARETEQ || node->assign.op == TOK_LSHIFTEQ ||
                node->assign.op == TOK_RSHIFTEQ) {
                if (type_is_float(target) || type_is_float(value)) {
                    checker_error(c, node->loc.line,
                        "bitwise compound assignment requires integer types, got '%s'",
                        type_name(target));
                }
            }
            /* reject narrowing: value wider than target (unless value is a literal) */
            if (type_is_numeric(target) && type_is_numeric(value) &&
                !is_literal_compatible(node->assign.value, target)) {
                int tw = type_width(target);
                int vw = type_width(value);
                if (tw > 0 && vw > 0 && vw > tw) {
                    checker_error(c, node->loc.line,
                        "compound assignment would narrow '%s' (%d-bit) into '%s' (%d-bit) — use @truncate",
                        type_name(value), vw, type_name(target), tw);
                }
            }
        }
        result = target;
        break;
    }

    /* ---- Function call ---- */
    case NODE_CALL: {
        /* check args first */
        Type **arg_types = NULL;
        if (node->call.arg_count > 0) {
            arg_types = (Type **)arena_alloc(c->arena,
                node->call.arg_count * sizeof(Type *));
            for (int i = 0; i < node->call.arg_count; i++) {
                arg_types[i] = check_expr(c, node->call.args[i]);
            }
        }

        /* builtin method call: expr.method(args) where expr is Pool/Ring/Arena */
        if (node->call.callee->kind == NODE_FIELD) {
            Node *field_node = node->call.callee;
            Type *obj = check_expr(c, field_node->field.object);
            const char *mname = field_node->field.field_name;
            uint32_t mlen = (uint32_t)field_node->field.field_name_len;

            /* BUG-236: reject mutating methods on const builtins */
            bool obj_is_const = false;
            {
                Node *oroot = field_node->field.object;
                while (oroot && (oroot->kind == NODE_FIELD || oroot->kind == NODE_INDEX)) {
                    if (oroot->kind == NODE_FIELD) oroot = oroot->field.object;
                    else oroot = oroot->index_expr.object;
                }
                if (oroot && oroot->kind == NODE_IDENT) {
                    Symbol *osym = scope_lookup(c->current_scope,
                        oroot->ident.name, (uint32_t)oroot->ident.name_len);
                    if (osym && osym->is_const) obj_is_const = true;
                }
            }

            /* Pool methods */
            if (obj->kind == TYPE_POOL) {
                if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'alloc' on const Pool");
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "pool.alloc() takes no arguments");
                    result = type_optional(c->arena, type_handle(c->arena, obj->pool.elem));
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 3 && memcmp(mname, "get", 3) == 0) {
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "pool.get() takes exactly 1 argument");
                    result = type_pointer(c->arena, obj->pool.elem);
                    typemap_set(c, field_node,result);
                    mark_non_storable(c, node); /* Rule 2: get() result non-storable */
                    break;
                }
                if (mlen == 4 && memcmp(mname, "free", 4) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'free' on const Pool");
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "pool.free() takes exactly 1 argument");
                    result = ty_void;
                    typemap_set(c, field_node,result);
                    break;
                }
                checker_error(c, node->loc.line,
                    "Pool has no method '%.*s' (available: alloc, get, free)",
                    (int)mlen, mname);
                result = ty_void;
                break;
            }

            /* Ring methods */
            if (obj->kind == TYPE_RING) {
                if (mlen == 4 && memcmp(mname, "push", 4) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'push' on const Ring");
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "ring.push() takes exactly 1 argument");
                    result = ty_void;
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 12 && memcmp(mname, "push_checked", 12) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'push_checked' on const Ring");
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "ring.push_checked() takes exactly 1 argument");
                    result = type_optional(c->arena, ty_void);
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 3 && memcmp(mname, "pop", 3) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'pop' on const Ring");
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "ring.pop() takes no arguments");
                    result = type_optional(c->arena, obj->ring.elem);
                    typemap_set(c, field_node,result);
                    break;
                }
                checker_error(c, node->loc.line,
                    "Ring has no method '%.*s' (available: push, push_checked, pop)",
                    (int)mlen, mname);
                result = ty_void;
                break;
            }

            /* Slab methods — same API as Pool but dynamically growable */
            if (obj->kind == TYPE_SLAB) {
                if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'alloc' on const Slab");
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "slab.alloc() takes no arguments");
                    result = type_optional(c->arena, type_handle(c->arena, obj->slab.elem));
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 3 && memcmp(mname, "get", 3) == 0) {
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "slab.get() takes exactly 1 argument");
                    result = type_pointer(c->arena, obj->slab.elem);
                    typemap_set(c, field_node,result);
                    mark_non_storable(c, node);
                    break;
                }
                if (mlen == 4 && memcmp(mname, "free", 4) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'free' on const Slab");
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "slab.free() takes exactly 1 argument");
                    result = ty_void;
                    typemap_set(c, field_node,result);
                    break;
                }
                checker_error(c, node->loc.line,
                    "Slab has no method '%.*s' (available: alloc, get, free)",
                    (int)mlen, mname);
                result = ty_void;
                break;
            }

            /* Arena methods */
            if (obj->kind == TYPE_ARENA) {
                if (mlen == 4 && memcmp(mname, "over", 4) == 0) {
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "Arena.over() takes exactly 1 argument");
                    result = ty_arena;
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                    /* Arena.alloc(T) → ?*T */
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'alloc' on const Arena");
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "arena.alloc() takes exactly 1 argument");
                    if (node->call.arg_count >= 1 &&
                        node->call.args[0]->kind == NODE_IDENT) {
                        const char *tname = node->call.args[0]->ident.name;
                        size_t tlen = node->call.args[0]->ident.name_len;
                        Symbol *sym = scope_lookup(c->current_scope, tname, tlen);
                        if (sym && sym->type) {
                            result = type_optional(c->arena,
                                type_pointer(c->arena, sym->type));
                        } else {
                            checker_error(c, node->loc.line,
                                "arena.alloc: unknown type '%.*s'", (int)tlen, tname);
                            result = ty_void;
                        }
                    } else {
                        result = ty_void;
                    }
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 5 && memcmp(mname, "reset", 5) == 0) {
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "arena.reset() takes no arguments");
                    /* Rule 3: arena.reset() outside defer = warning */
                    if (c->defer_depth == 0) {
                        checker_warning(c, node->loc.line,
                            "arena.reset() outside defer may cause dangling pointers — "
                            "use defer or unsafe_reset()");
                    }
                    result = ty_void;
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 11 && memcmp(mname, "alloc_slice", 11) == 0) {
                    /* Arena.alloc_slice(T, n) → ?[]T */
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'alloc_slice' on const Arena");
                    if (node->call.arg_count != 2)
                        checker_error(c, node->loc.line, "arena.alloc_slice() takes exactly 2 arguments");
                    if (node->call.arg_count >= 1 &&
                        node->call.args[0]->kind == NODE_IDENT) {
                        const char *tname = node->call.args[0]->ident.name;
                        size_t tlen = node->call.args[0]->ident.name_len;
                        Symbol *sym = scope_lookup(c->current_scope, tname, tlen);
                        if (sym && sym->type) {
                            result = type_optional(c->arena,
                                type_slice(c->arena, sym->type));
                        } else {
                            checker_error(c, node->loc.line,
                                "arena.alloc_slice: unknown type '%.*s'", (int)tlen, tname);
                            result = ty_void;
                        }
                    } else {
                        result = ty_void;
                    }
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 12 && memcmp(mname, "unsafe_reset", 12) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'unsafe_reset' on const Arena");
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "arena.unsafe_reset() takes no arguments");
                    result = ty_void;
                    typemap_set(c, field_node,result);
                    break;
                }
                checker_error(c, node->loc.line,
                    "Arena has no method '%.*s' (available: over, alloc, alloc_slice, reset, unsafe_reset)",
                    (int)mlen, mname);
                result = ty_void;
                break;
            }

            /* not a builtin — fall through to normal call resolution */
        }

        /* normal function call */
        Type *callee_type = check_expr(c, node->call.callee);
        /* unwrap distinct typedef for call dispatch */
        Type *effective_callee = type_unwrap_distinct(callee_type);

        if (effective_callee->kind == TYPE_FUNC_PTR) {
            /* verify arg count */
            if ((uint32_t)node->call.arg_count != effective_callee->func_ptr.param_count) {
                checker_error(c, node->loc.line,
                    "expected %u arguments, got %d",
                    effective_callee->func_ptr.param_count, node->call.arg_count);
            } else {
                /* verify each arg type */
                for (uint32_t i = 0; i < effective_callee->func_ptr.param_count; i++) {
                    Type *param = effective_callee->func_ptr.params[i];
                    Type *arg = arg_types[i];

                    /* const safety: reject string literal → mutable slice param
                     * (string data is in .rodata, writing segfaults)
                     * BUG-313: only reject if param is NOT const — const []u8 is safe */
                    if (node->call.args[i]->kind == NODE_STRING_LIT &&
                        param && param->kind == TYPE_SLICE &&
                        !param->slice.is_const) {
                        checker_error(c, node->loc.line,
                            "argument %d: cannot pass string literal to mutable []u8 parameter — "
                            "use const []u8 parameter or copy the string first",
                            i + 1);
                    }

                    /* const safety: reject const slice/pointer → mutable param */
                    if (arg && param) {
                        if (arg->kind == TYPE_SLICE && param->kind == TYPE_SLICE &&
                            arg->slice.is_const && !param->slice.is_const) {
                            checker_error(c, node->loc.line,
                                "argument %d: cannot pass const slice to mutable parameter",
                                i + 1);
                        }
                        if (arg->kind == TYPE_POINTER && param->kind == TYPE_POINTER &&
                            arg->pointer.is_const && !param->pointer.is_const) {
                            checker_error(c, node->loc.line,
                                "argument %d: cannot pass const pointer to mutable parameter",
                                i + 1);
                        }
                        /* BUG-263: volatile pointer → non-volatile param strips volatile.
                         * Check both type-level and symbol-level volatile. */
                        if (arg->kind == TYPE_POINTER && param->kind == TYPE_POINTER &&
                            !param->pointer.is_volatile) {
                            bool arg_volatile = arg->pointer.is_volatile;
                            if (!arg_volatile && node->call.args[i]->kind == NODE_IDENT) {
                                Symbol *as = scope_lookup(c->current_scope,
                                    node->call.args[i]->ident.name,
                                    (uint32_t)node->call.args[i]->ident.name_len);
                                if (as && as->is_volatile) arg_volatile = true;
                            }
                            if (arg_volatile) {
                                checker_error(c, node->loc.line,
                                    "argument %d: cannot pass volatile pointer to non-volatile parameter",
                                    i + 1);
                            }
                        }
                    }
                    /* const array → mutable slice coercion: check if arg var is const */
                    if (arg && arg->kind == TYPE_ARRAY &&
                        param && param->kind == TYPE_SLICE && !param->slice.is_const &&
                        node->call.args[i]->kind == NODE_IDENT) {
                        Symbol *arg_sym = scope_lookup(c->current_scope,
                            node->call.args[i]->ident.name,
                            (uint32_t)node->call.args[i]->ident.name_len);
                        if (arg_sym && arg_sym->is_const) {
                            checker_error(c, node->loc.line,
                                "argument %d: cannot pass const array '%.*s' to mutable slice parameter",
                                i + 1, (int)arg_sym->name_len, arg_sym->name);
                        }
                    }
                    /* BUG-310: volatile array → non-volatile slice param rejected.
                     * Volatile must propagate — use volatile []T param. */
                    if (arg && arg->kind == TYPE_ARRAY &&
                        param && param->kind == TYPE_SLICE &&
                        !param->slice.is_volatile &&
                        node->call.args[i]->kind == NODE_IDENT) {
                        Symbol *arg_sym = scope_lookup(c->current_scope,
                            node->call.args[i]->ident.name,
                            (uint32_t)node->call.args[i]->ident.name_len);
                        if (arg_sym && arg_sym->is_volatile) {
                            checker_error(c, node->loc.line,
                                "argument %d: cannot pass volatile array '%.*s' to non-volatile slice parameter — "
                                "use 'volatile []%s' parameter type",
                                i + 1, (int)arg_sym->name_len, arg_sym->name,
                                type_name(param->slice.inner));
                        }
                    }

                    if (!type_equals(param, arg) &&
                        !can_implicit_coerce(arg, param) &&
                        !is_literal_compatible(node->call.args[i], param)) {
                        checker_error(c, node->loc.line,
                            "argument %u: expected '%s', got '%s'",
                            i + 1, type_name(param), type_name(arg));
                    }
                }

                /* keep parameter validation: check call arguments.
                 * Works for BOTH direct function calls AND function pointer calls
                 * by using param_keeps on the resolved Type (BUG-277). */
                if (effective_callee->func_ptr.param_keeps) {
                    for (int i = 0; i < (int)effective_callee->func_ptr.param_count &&
                         i < node->call.arg_count; i++) {
                        if (!effective_callee->func_ptr.param_keeps[i]) continue;
                        /* keep param: arg must be static/global, not local.
                         * BUG-339: also check orelse fallback for &local.
                         * BUG-338: walk into intrinsics for &local. */
                        Node *arg_node = node->call.args[i];
                        /* unwrap orelse — check all branches recursively
                         * BUG-370: nested orelse chains: a orelse b orelse &x */
                        Node *keep_checks[8] = { arg_node, NULL };
                        int keep_check_count = 1;
                        {
                            Node *ow = arg_node;
                            int idx = 0;
                            while (ow && ow->kind == NODE_ORELSE && idx < 7) {
                                keep_checks[idx++] = ow->orelse.expr;
                                if (ow->orelse.fallback && ow->orelse.fallback->kind != NODE_ORELSE) {
                                    keep_checks[idx++] = ow->orelse.fallback;
                                    break;
                                }
                                ow = ow->orelse.fallback;
                            }
                            if (idx > 0) keep_check_count = idx;
                        }
                        for (int kc = 0; kc < keep_check_count; kc++) {
                        Node *karg = keep_checks[kc];
                        /* walk into intrinsics */
                        while (karg && karg->kind == NODE_INTRINSIC && karg->intrinsic.arg_count > 0)
                            karg = karg->intrinsic.args[karg->intrinsic.arg_count - 1];
                        if (karg && karg->kind == NODE_UNARY &&
                            karg->unary.op == TOK_AMP &&
                            karg->unary.operand->kind == NODE_IDENT) {
                            Symbol *arg_sym = scope_lookup(c->current_scope,
                                arg_node->unary.operand->ident.name,
                                (uint32_t)arg_node->unary.operand->ident.name_len);
                            if (arg_sym && !arg_sym->is_static) {
                                /* BUG-317: check both raw AND mangled keys for imported globals */
                                bool is_global = scope_lookup_local(c->global_scope,
                                    arg_sym->name, arg_sym->name_len) != NULL;
                                if (!is_global && c->current_module) {
                                    /* try mangled: module__name (BUG-332: double underscore) */
                                    uint32_t mkl = c->current_module_len + 2 + arg_sym->name_len;
                                    char mk[256];
                                    if (mkl < sizeof(mk)) {
                                        memcpy(mk, c->current_module, c->current_module_len);
                                        mk[c->current_module_len] = '_';
                                        mk[c->current_module_len + 1] = '_';
                                        memcpy(mk + c->current_module_len + 2, arg_sym->name, arg_sym->name_len);
                                        is_global = scope_lookup_local(c->global_scope, mk, mkl) != NULL;
                                    }
                                }
                                if (!is_global) {
                                    checker_error(c, node->loc.line,
                                        "argument %d: local variable '%.*s' cannot "
                                        "satisfy 'keep' parameter — must be static or global",
                                        i + 1,
                                        (int)arg_sym->name_len, arg_sym->name);
                                }
                            }
                        }
                        } /* end keep_checks loop (BUG-339) */
                        /* BUG-221/370: also reject local-derived pointers.
                         * Walk through orelse chain to check all branches. */
                        Node *ld_check = arg_node;
                        while (ld_check && ld_check->kind == NODE_ORELSE)
                            ld_check = ld_check->orelse.expr;
                        if (ld_check && ld_check->kind == NODE_IDENT) {
                            Symbol *arg_sym = scope_lookup(c->current_scope,
                                ld_check->ident.name, (uint32_t)ld_check->ident.name_len);
                            if (arg_sym && arg_sym->is_local_derived) {
                                checker_error(c, node->loc.line,
                                    "argument %d: local-derived pointer '%.*s' cannot "
                                    "satisfy 'keep' parameter — points to stack memory",
                                    i + 1, (int)arg_sym->name_len, arg_sym->name);
                            }
                            if (arg_sym && arg_sym->is_arena_derived) {
                                checker_error(c, node->loc.line,
                                    "argument %d: arena-derived pointer '%.*s' cannot "
                                    "satisfy 'keep' parameter — arena memory may be reset",
                                    i + 1, (int)arg_sym->name_len, arg_sym->name);
                            }
                        }
                        if (arg_node->kind == NODE_IDENT) {
                            Symbol *arg_sym = scope_lookup(c->current_scope,
                                arg_node->ident.name,
                                (uint32_t)arg_node->ident.name_len);
                            if (arg_sym && arg_sym->is_local_derived) {
                                checker_error(c, node->loc.line,
                                    "argument %d: local-derived pointer '%.*s' cannot "
                                    "satisfy 'keep' parameter — points to stack memory",
                                    i + 1,
                                    (int)arg_sym->name_len, arg_sym->name);
                            }
                            /* BUG-336: reject arena-derived pointers for keep params */
                            if (arg_sym && arg_sym->is_arena_derived) {
                                checker_error(c, node->loc.line,
                                    "argument %d: arena-derived pointer '%.*s' cannot "
                                    "satisfy 'keep' parameter — arena memory may be reset",
                                    i + 1,
                                    (int)arg_sym->name_len, arg_sym->name);
                            }
                            /* BUG-334: reject local arrays coerced to keep slices */
                            if (arg_sym && !arg_sym->is_static &&
                                arg_sym->type && arg_sym->type->kind == TYPE_ARRAY) {
                                bool sym_is_global = scope_lookup_local(c->global_scope,
                                    arg_sym->name, arg_sym->name_len) != NULL;
                                if (!sym_is_global) {
                                    checker_error(c, node->loc.line,
                                        "argument %d: local array '%.*s' cannot "
                                        "satisfy 'keep' parameter — stack memory is freed when function returns",
                                        i + 1,
                                        (int)arg_sym->name_len, arg_sym->name);
                                }
                            }
                        }
                    }
                }
            }
            result = effective_callee->func_ptr.ret;

            /* comptime function call — evaluate at compile time */
            if (node->call.callee->kind == NODE_IDENT) {
                Symbol *callee_sym = scope_lookup(c->current_scope,
                    node->call.callee->ident.name,
                    (uint32_t)node->call.callee->ident.name_len);
                if (callee_sym && callee_sym->is_comptime && callee_sym->func_node) {
                    Node *fn = callee_sym->func_node;
                    /* all args must be compile-time constant */
                    int pc = fn->func_decl.param_count;
                    ComptimeParam cparams[32];
                    bool all_const = true;
                    for (int ci = 0; ci < node->call.arg_count && ci < pc && ci < 32; ci++) {
                        int64_t v = eval_const_expr(node->call.args[ci]);
                        if (v == CONST_EVAL_FAIL) { all_const = false; break; }
                        cparams[ci].name = fn->func_decl.params[ci].name;
                        cparams[ci].name_len = (uint32_t)fn->func_decl.params[ci].name_len;
                        cparams[ci].value = v;
                    }
                    if (!all_const) {
                        checker_error(c, node->loc.line,
                            "comptime function '%.*s' requires all arguments to be compile-time constants",
                            (int)callee_sym->name_len, callee_sym->name);
                    } else if (fn->func_decl.body) {
                        int64_t val = eval_comptime_block(fn->func_decl.body, cparams, pc);
                        if (val == CONST_EVAL_FAIL) {
                            checker_error(c, node->loc.line,
                                "comptime function '%.*s' body could not be evaluated at compile time",
                                (int)callee_sym->name_len, callee_sym->name);
                        } else {
                            node->call.comptime_value = val;
                            node->call.is_comptime_resolved = true;
                        }
                    }
                }
            }
        } else {
            checker_error(c, node->loc.line,
                "cannot call non-function type '%s'",
                type_name(effective_callee));
            result = ty_void;
        }
        break;
    }

    /* ---- Field access ---- */
    case NODE_FIELD: {
        Type *obj = check_expr(c, node->field.object);
        const char *fname = node->field.field_name;
        uint32_t flen = (uint32_t)node->field.field_name_len;

        /* builtin method check — Pool, Ring, Arena */
        if (obj->kind == TYPE_POOL) {
            if (flen == 5 && memcmp(fname, "alloc", 5) == 0) {
                result = type_optional(c->arena, type_handle(c->arena, obj->pool.elem));
                break;
            }
            if (flen == 3 && memcmp(fname, "get", 3) == 0) {
                result = type_pointer(c->arena, obj->pool.elem);
                break;
            }
            if (flen == 4 && memcmp(fname, "free", 4) == 0) {
                result = ty_void;
                break;
            }
        }
        if (obj->kind == TYPE_RING) {
            if (flen == 4 && memcmp(fname, "push", 4) == 0) {
                result = ty_void;
                break;
            }
            if (flen == 12 && memcmp(fname, "push_checked", 12) == 0) {
                result = type_optional(c->arena, ty_void);
                break;
            }
            if (flen == 3 && memcmp(fname, "pop", 3) == 0) {
                result = type_optional(c->arena, obj->ring.elem);
                break;
            }
        }
        if (obj->kind == TYPE_ARENA) {
            if (flen == 4 && memcmp(fname, "over", 4) == 0) {
                result = ty_arena;
                break;
            }
            if (flen == 5 && memcmp(fname, "alloc", 5) == 0) {
                /* Arena.alloc(T) → ?*T */
                result = ty_void; /* resolved at call site with type arg */
                break;
            }
            if (flen == 11 && memcmp(fname, "alloc_slice", 11) == 0) {
                /* Arena.alloc_slice(T, n) → ?[]T */
                result = ty_void; /* resolved at call site with type arg */
                break;
            }
            if (flen == 5 && memcmp(fname, "reset", 5) == 0) {
                result = ty_void;
                break;
            }
            if (flen == 12 && memcmp(fname, "unsafe_reset", 12) == 0) {
                result = ty_void;
                break;
            }
        }

        /* unwrap distinct for all field access below (struct, enum, union, pointer deref) */
        obj = type_unwrap_distinct(obj);

        /* struct field lookup */
        if (obj->kind == TYPE_STRUCT) {
            for (uint32_t i = 0; i < obj->struct_type.field_count; i++) {
                SField *f = &obj->struct_type.fields[i];
                if (f->name_len == flen && memcmp(f->name, fname, flen) == 0) {
                    result = f->type;
                    break;
                }
            }
            if (!result) {
                checker_error(c, node->loc.line,
                    "struct '%.*s' has no field '%.*s'",
                    (int)obj->struct_type.name_len, obj->struct_type.name,
                    (int)flen, fname);
                result = ty_void;
            }
            break;
        }

        /* slice.ptr — returns pointer to element type (const if slice is const) */
        if (obj->kind == TYPE_SLICE && flen == 3 && memcmp(fname, "ptr", 3) == 0) {
            Type *ptr = type_pointer(c->arena, obj->slice.inner);
            if (obj->slice.is_const) ptr->pointer.is_const = true;
            result = ptr;
            break;
        }

        /* slice.len */
        if (obj->kind == TYPE_SLICE && flen == 3 && memcmp(fname, "len", 3) == 0) {
            result = ty_usize;
            break;
        }

        /* array.len */
        if (obj->kind == TYPE_ARRAY && flen == 3 && memcmp(fname, "len", 3) == 0) {
            result = ty_usize;
            break;
        }

        /* pointer auto-deref for field access: ptr.field = (*ptr).field */
        if (obj->kind == TYPE_POINTER &&
            type_unwrap_distinct(obj->pointer.inner)->kind == TYPE_STRUCT) {
            Type *inner = type_unwrap_distinct(obj->pointer.inner);
            for (uint32_t i = 0; i < inner->struct_type.field_count; i++) {
                SField *f = &inner->struct_type.fields[i];
                if (f->name_len == flen && memcmp(f->name, fname, flen) == 0) {
                    result = f->type;
                    break;
                }
            }
            if (!result) {
                checker_error(c, node->loc.line,
                    "no field '%.*s' on type '%s'",
                    (int)flen, fname, type_name(inner));
                result = ty_void;
            }
            break;
        }

        /* pointer auto-deref for union: ptr.variant = (*ptr).variant */
        if (obj->kind == TYPE_POINTER &&
            type_unwrap_distinct(obj->pointer.inner)->kind == TYPE_UNION) {
            /* union switch lock applies to pointer auto-deref too.
             * BUG-211: walk to root for field-based access */
            if (c->in_assign_target && c->union_switch_var) {
                /* BUG-244: walk ALL deref/field/index levels to find root */
                Node *mut_root = node->field.object;
                while (mut_root) {
                    if (mut_root->kind == NODE_UNARY && mut_root->unary.op == TOK_STAR)
                        mut_root = mut_root->unary.operand;
                    else if (mut_root->kind == NODE_FIELD)
                        mut_root = mut_root->field.object;
                    else if (mut_root->kind == NODE_INDEX)
                        mut_root = mut_root->index_expr.object;
                    else break;
                }
                if (mut_root && mut_root->kind == NODE_IDENT) {
                    bool name_match = (mut_root->ident.name_len == c->union_switch_var_len &&
                        memcmp(mut_root->ident.name, c->union_switch_var,
                               c->union_switch_var_len) == 0);
                    /* BUG-261: block mutation through pointer alias of same union type.
                     * Only applies to pointers — they might alias the locked variable.
                     * Direct locals of the same type are safe (different memory). */
                    bool type_match = false;
                    if (!name_match && c->union_switch_type) {
                        Symbol *ms = scope_lookup(c->current_scope,
                            mut_root->ident.name, (uint32_t)mut_root->ident.name_len);
                        if (ms && ms->type) {
                            Type *mt = type_unwrap_distinct(ms->type);
                            if (mt->kind == TYPE_POINTER) {
                                Type *inner = type_unwrap_distinct(mt->pointer.inner);
                                if (inner == c->union_switch_type) type_match = true;
                            }
                        }
                    }
                    if (name_match || type_match) {
                        checker_error(c, node->loc.line,
                            "cannot mutate union '%.*s' inside its own switch arm — "
                            "active capture would become invalid",
                            (int)(name_match ? c->union_switch_var_len : mut_root->ident.name_len),
                            name_match ? c->union_switch_var : mut_root->ident.name);
                        result = ty_void;
                        break;
                    }
                }
            }
            Type *inner = type_unwrap_distinct(obj->pointer.inner);
            for (uint32_t i = 0; i < inner->union_type.variant_count; i++) {
                SUVariant *v = &inner->union_type.variants[i];
                if (v->name_len == flen && memcmp(v->name, fname, flen) == 0) {
                    result = v->type;
                    break;
                }
            }
            if (!result) {
                checker_error(c, node->loc.line,
                    "no variant '%.*s' in union '%.*s'",
                    (int)flen, fname,
                    (int)inner->union_type.name_len, inner->union_type.name);
                result = ty_void;
            }
            break;
        }

        /* enum: State.idle → returns the enum type */
        if (obj->kind == TYPE_ENUM) {
            bool found = false;
            for (uint32_t i = 0; i < obj->enum_type.variant_count; i++) {
                SEVariant *v = &obj->enum_type.variants[i];
                if (v->name_len == flen && memcmp(v->name, fname, flen) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                checker_error(c, node->loc.line,
                    "no variant '%.*s' in enum '%.*s'",
                    (int)flen, fname,
                    (int)obj->enum_type.name_len, obj->enum_type.name);
            }
            result = obj; /* State.idle has type State */
            break;
        }

        /* union: writing (assignment LHS) is allowed — sets the active variant.
         * Reading (any other context) requires switch. */
        if (obj->kind == TYPE_UNION) {
            if (!c->in_assign_target) {
                checker_error(c, node->loc.line,
                    "cannot read union variant '%.*s' directly — must use switch",
                    (int)flen, fname);
                result = ty_void;
                break;
            }
            /* prevent mutating union variant while inside a switch arm on same variable.
             * BUG-211: walk field/index chain to find root ident for field-based unions */
            /* BUG-244: walk ALL deref/field/index levels to find root */
            if (c->union_switch_var) {
                Node *mut_root = node->field.object;
                while (mut_root) {
                    if (mut_root->kind == NODE_UNARY && mut_root->unary.op == TOK_STAR)
                        mut_root = mut_root->unary.operand;
                    else if (mut_root->kind == NODE_FIELD)
                        mut_root = mut_root->field.object;
                    else if (mut_root->kind == NODE_INDEX)
                        mut_root = mut_root->index_expr.object;
                    else break;
                }
                if (mut_root && mut_root->kind == NODE_IDENT) {
                    bool name_match = (mut_root->ident.name_len == c->union_switch_var_len &&
                        memcmp(mut_root->ident.name, c->union_switch_var,
                               c->union_switch_var_len) == 0);
                    /* BUG-261: block mutation through pointer alias of same union type.
                     * Only applies to pointers — they might alias the locked variable.
                     * Direct locals of the same type are safe (different memory). */
                    bool type_match = false;
                    if (!name_match && c->union_switch_type) {
                        Symbol *ms = scope_lookup(c->current_scope,
                            mut_root->ident.name, (uint32_t)mut_root->ident.name_len);
                        if (ms && ms->type) {
                            Type *mt = type_unwrap_distinct(ms->type);
                            if (mt->kind == TYPE_POINTER) {
                                Type *inner = type_unwrap_distinct(mt->pointer.inner);
                                if (inner == c->union_switch_type) type_match = true;
                            }
                        }
                    }
                    if (name_match || type_match) {
                        checker_error(c, node->loc.line,
                            "cannot mutate union '%.*s' inside its own switch arm — "
                            "active capture would become invalid",
                            (int)(name_match ? c->union_switch_var_len : mut_root->ident.name_len),
                            name_match ? c->union_switch_var : mut_root->ident.name);
                        result = ty_void;
                        break;
                    }
                }
            }
            for (uint32_t i = 0; i < obj->union_type.variant_count; i++) {
                SUVariant *v = &obj->union_type.variants[i];
                if (v->name_len == flen && memcmp(v->name, fname, flen) == 0) {
                    result = v->type;
                    break;
                }
            }
            if (!result) {
                checker_error(c, node->loc.line,
                    "no variant '%.*s' in union '%.*s'",
                    (int)flen, fname,
                    (int)obj->union_type.name_len, obj->union_type.name);
                result = ty_void;
            }
            break;
        }

        /* fallback: field access on non-struct/enum/union type */
        checker_error(c, node->loc.line,
            "cannot access field '%.*s' on type '%s'",
            (int)flen, fname, type_name(obj));
        result = ty_void;
        break;
    }

    /* ---- Index ---- */
    case NODE_INDEX: {
        Type *obj = check_expr(c, node->index_expr.object);
        Type *idx = check_expr(c, node->index_expr.index);

        if (!type_is_integer(idx)) {
            checker_error(c, node->loc.line,
                "array index must be integer, got '%s'", type_name(idx));
        }

        if (obj->kind == TYPE_ARRAY) {
            /* BUG-196: compile-time OOB for constant index */
            if (node->index_expr.index->kind == NODE_INT_LIT) {
                uint64_t idx_val = node->index_expr.index->int_lit.value;
                if (idx_val >= obj->array.size) {
                    checker_error(c, node->loc.line,
                        "array index %llu is out of bounds for array of size %llu",
                        (unsigned long long)idx_val, (unsigned long long)obj->array.size);
                }
            }
            result = obj->array.inner;
        } else if (obj->kind == TYPE_SLICE) {
            result = obj->slice.inner;
        } else if (obj->kind == TYPE_POINTER) {
            /* pointer indexing: *T[i] → T (same as C pointer arithmetic) */
            result = obj->pointer.inner;
        } else {
            checker_error(c, node->loc.line,
                "cannot index type '%s'", type_name(obj));
            result = ty_void;
        }
        break;
    }

    /* ---- Slice ---- */
    case NODE_SLICE: {
        Type *obj = check_expr(c, node->slice.object);
        if (node->slice.start) {
            Type *start = check_expr(c, node->slice.start);
            if (!type_is_integer(start)) {
                checker_error(c, node->loc.line, "slice start must be integer");
            }
        }
        if (node->slice.end) {
            Type *end = check_expr(c, node->slice.end);
            if (!type_is_integer(end)) {
                checker_error(c, node->loc.line, "slice end must be integer");
            }
        }

        /* compile-time check: start must be <= end (for array/slice sub-slicing only,
         * NOT for bit extraction which uses [high..low] where high > low is valid) */
        if (node->slice.start && node->slice.end &&
            !type_is_integer(obj)) {
            int64_t start_val = eval_const_expr(node->slice.start);
            int64_t end_val = eval_const_expr(node->slice.end);
            if (start_val != CONST_EVAL_FAIL && end_val != CONST_EVAL_FAIL && start_val > end_val) {
                checker_error(c, node->loc.line,
                    "slice start (%lld) is greater than end (%lld)",
                    (long long)start_val, (long long)end_val);
            }
        }

        /* BUG-217: compile-time slice bounds check for arrays */
        if (obj->kind == TYPE_ARRAY) {
            if (node->slice.end) {
                int64_t end_val = eval_const_expr(node->slice.end);
                if (end_val != CONST_EVAL_FAIL && end_val >= 0 && (uint64_t)end_val > obj->array.size) {
                    checker_error(c, node->loc.line,
                        "slice end %lld exceeds array size %llu",
                        (long long)end_val, (unsigned long long)obj->array.size);
                }
            }
            if (node->slice.start) {
                int64_t start_val = eval_const_expr(node->slice.start);
                if (start_val != CONST_EVAL_FAIL && start_val >= 0 && (uint64_t)start_val > obj->array.size) {
                    checker_error(c, node->loc.line,
                        "slice start %lld exceeds array size %llu",
                        (long long)start_val, (unsigned long long)obj->array.size);
                }
            }
            result = type_slice(c->arena, obj->array.inner);
        } else if (obj->kind == TYPE_SLICE) {
            result = obj; /* slice of slice = same slice type */
        } else if (type_is_integer(obj)) {
            /* bit extraction: reg[high..low] → integer result */
            /* validate constant indices are within type width */
            if (node->slice.start) {
                int64_t hi = eval_const_expr(node->slice.start);
                if (hi != CONST_EVAL_FAIL && hi >= 0 && hi >= type_width(obj)) {
                    checker_error(c, node->loc.line,
                        "bit index %lld out of range for %d-bit type '%s'",
                        (long long)hi, type_width(obj), type_name(obj));
                }
                /* BUG-288: reject hi < lo — negative width bit extraction */
                if (node->slice.end) {
                    int64_t lo = eval_const_expr(node->slice.end);
                    if (hi != CONST_EVAL_FAIL && lo != CONST_EVAL_FAIL && hi < lo) {
                        checker_error(c, node->loc.line,
                            "bit extraction high index (%lld) must be >= low index (%lld)",
                            (long long)hi, (long long)lo);
                    }
                }
            }
            result = obj;
        } else {
            checker_error(c, node->loc.line,
                "cannot slice type '%s'", type_name(obj));
            result = ty_void;
        }
        break;
    }

    /* ---- Orelse ---- */
    case NODE_ORELSE: {
        Type *expr = check_expr(c, node->orelse.expr);

        if (!type_is_optional(expr)) {
            checker_error(c, node->loc.line,
                "'orelse' requires optional type, got '%s'", type_name(expr));
            result = expr;
            break;
        }

        Type *unwrapped = type_unwrap_optional(expr);

        if (node->orelse.fallback_is_break && !c->in_loop) {
            checker_error(c, node->loc.line, "'orelse break' outside of loop");
        }
        if (node->orelse.fallback_is_continue && !c->in_loop) {
            checker_error(c, node->loc.line, "'orelse continue' outside of loop");
        }

        if (node->orelse.fallback_is_return ||
            node->orelse.fallback_is_break ||
            node->orelse.fallback_is_continue) {
            /* flow control — expression yields unwrapped type */
            result = unwrapped;
        } else if (node->orelse.fallback) {
            if (node->orelse.fallback->kind == NODE_BLOCK) {
                /* orelse { block } — statement-only, no result type.
                 * Block runs on null. Per spec: cannot be used as expression. */
                check_stmt(c, node->orelse.fallback);
                result = unwrapped;
            } else {
                Type *fallback = check_expr(c, node->orelse.fallback);
                /* fallback must match unwrapped type */
                if (!type_equals(unwrapped, fallback) &&
                    !can_implicit_coerce(fallback, unwrapped) &&
                    !is_literal_compatible(node->orelse.fallback, unwrapped)) {
                    checker_error(c, node->loc.line,
                        "orelse fallback type '%s' doesn't match '%s'",
                        type_name(fallback), type_name(unwrapped));
                }
                result = unwrapped;
            }
        } else {
            result = unwrapped;
        }
        break;
    }

    /* ---- Intrinsic ---- */
    case NODE_INTRINSIC: {
        const char *name = node->intrinsic.name;
        uint32_t nlen = (uint32_t)node->intrinsic.name_len;

        /* type-check arguments — skip field name args for @offset, @container */
        bool has_field_arg = (nlen == 6 && memcmp(name, "offset", 6) == 0) ||
                             (nlen == 9 && memcmp(name, "container", 9) == 0);
        for (int i = 0; i < node->intrinsic.arg_count; i++) {
            if (has_field_arg && i == node->intrinsic.arg_count - 1) {
                /* last arg is a field name — don't look up as variable */
                continue;
            }
            check_expr(c, node->intrinsic.args[i]);
        }

        if (nlen == 4 && memcmp(name, "size", 4) == 0) {
            /* BUG-231/320: reject @size(void) and @size(opaque) — no meaningful size.
             * Unwrap distinct first (BUG-320: distinct typedef void still has no size).
             * Check both type_arg path AND expression arg path (named types parsed as ident). */
            {
                Type *st = NULL;
                if (node->intrinsic.type_arg)
                    st = resolve_type(c, node->intrinsic.type_arg);
                else if (node->intrinsic.arg_count > 0)
                    st = typemap_get(c, node->intrinsic.args[0]);
                if (st) {
                    Type *st_eff = type_unwrap_distinct(st);
                    if (st_eff && (st_eff->kind == TYPE_VOID || st_eff->kind == TYPE_OPAQUE)) {
                        checker_error(c, node->loc.line,
                            "@size(%s) is invalid — type has no defined size",
                            type_name(st));
                    }
                }
            }
            result = ty_usize;
        } else if (nlen == 6 && memcmp(name, "offset", 6) == 0) {
            /* @offset(T, field) — validate field exists on struct T */
            {
                Type *struct_type = NULL;
                const char *field_name = NULL;
                uint32_t field_len = 0;
                if (node->intrinsic.type_arg) {
                    struct_type = resolve_type(c, node->intrinsic.type_arg);
                    if (node->intrinsic.arg_count > 0 &&
                        node->intrinsic.args[0]->kind == NODE_IDENT) {
                        field_name = node->intrinsic.args[0]->ident.name;
                        field_len = (uint32_t)node->intrinsic.args[0]->ident.name_len;
                    }
                } else if (node->intrinsic.arg_count >= 2 &&
                           node->intrinsic.args[0]->kind == NODE_IDENT) {
                    Symbol *sym = scope_lookup(c->current_scope,
                        node->intrinsic.args[0]->ident.name,
                        (uint32_t)node->intrinsic.args[0]->ident.name_len);
                    if (sym) struct_type = sym->type;
                    if (node->intrinsic.args[1]->kind == NODE_IDENT) {
                        field_name = node->intrinsic.args[1]->ident.name;
                        field_len = (uint32_t)node->intrinsic.args[1]->ident.name_len;
                    }
                }
                if (struct_type && struct_type->kind == TYPE_STRUCT && field_name) {
                    bool found = false;
                    for (uint32_t fi = 0; fi < struct_type->struct_type.field_count; fi++) {
                        if (struct_type->struct_type.fields[fi].name_len == field_len &&
                            memcmp(struct_type->struct_type.fields[fi].name, field_name, field_len) == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        checker_error(c, node->loc.line,
                            "@offset: struct '%.*s' has no field '%.*s'",
                            (int)struct_type->struct_type.name_len, struct_type->struct_type.name,
                            (int)field_len, field_name);
                    }
                }
            }
            result = ty_usize;
        } else if (nlen == 7 && memcmp(name, "ptrcast", 7) == 0) {
            /* @ptrcast(*T, expr) → *T — source AND target must be pointers */
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                /* BUG-375: target type must be a pointer */
                if (result) {
                    Type *res_eff = type_unwrap_distinct(result);
                    if (res_eff->kind != TYPE_POINTER && res_eff->kind != TYPE_FUNC_PTR) {
                        checker_error(c, node->loc.line,
                            "@ptrcast target must be a pointer type, got '%s'",
                            type_name(result));
                    }
                }
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = typemap_get(c, node->intrinsic.args[0]);
                    if (val_type) {
                        Type *eff = type_unwrap_distinct(val_type);
                        if (eff->kind != TYPE_POINTER && eff->kind != TYPE_FUNC_PTR) {
                            checker_error(c, node->loc.line,
                                "@ptrcast source must be a pointer, got '%s'",
                                type_name(val_type));
                        }
                        /* BUG-304: const stripping via @ptrcast */
                        if (eff->kind == TYPE_POINTER && eff->pointer.is_const &&
                            result && result->kind == TYPE_POINTER &&
                            !result->pointer.is_const) {
                            checker_error(c, node->loc.line,
                                "@ptrcast cannot strip const qualifier — "
                                "target must be const pointer");
                        }
                        /* BUG-258: volatile stripping via @ptrcast.
                         * Cannot cast volatile pointer to non-volatile — writes may
                         * be optimized away by GCC, causing silent hardware failure.
                         * Check both type-level volatile (pointer.is_volatile) and
                         * symbol-level volatile (var_decl.is_volatile on the source ident). */
                        if (eff->kind == TYPE_POINTER &&
                            result && result->kind == TYPE_POINTER &&
                            !result->pointer.is_volatile) {
                            bool src_volatile = eff->pointer.is_volatile;
                            /* also check if source ident has sym->is_volatile */
                            if (!src_volatile && node->intrinsic.args[0]->kind == NODE_IDENT) {
                                Symbol *src_sym = scope_lookup(c->current_scope,
                                    node->intrinsic.args[0]->ident.name,
                                    (uint32_t)node->intrinsic.args[0]->ident.name_len);
                                if (src_sym && src_sym->is_volatile) src_volatile = true;
                            }
                            if (src_volatile) {
                                checker_error(c, node->loc.line,
                                    "@ptrcast cannot strip volatile qualifier — "
                                    "target must be volatile pointer");
                            }
                        }
                        /* provenance check: casting FROM *opaque TO *T —
                         * if source has provenance, target must match */
                        if (eff->kind == TYPE_POINTER &&
                            eff->pointer.inner->kind == TYPE_OPAQUE &&
                            node->intrinsic.args[0]->kind == NODE_IDENT) {
                            Symbol *src_sym = scope_lookup(c->current_scope,
                                node->intrinsic.args[0]->ident.name,
                                (uint32_t)node->intrinsic.args[0]->ident.name_len);
                            if (src_sym && src_sym->provenance_type) {
                                Type *prov = type_unwrap_distinct(src_sym->provenance_type);
                                Type *tgt = type_unwrap_distinct(result);
                                /* compare: provenance type should match target pointer inner */
                                if (tgt && tgt->kind == TYPE_POINTER && prov &&
                                    prov->kind == TYPE_POINTER) {
                                    Type *prov_inner = type_unwrap_distinct(prov->pointer.inner);
                                    Type *tgt_inner = type_unwrap_distinct(tgt->pointer.inner);
                                    if (prov_inner && tgt_inner &&
                                        tgt_inner->kind != TYPE_OPAQUE &&
                                        !type_equals(prov_inner, tgt_inner)) {
                                        checker_error(c, node->loc.line,
                                            "@ptrcast type mismatch: source has provenance '%s' "
                                            "but target is '*%s'",
                                            type_name(src_sym->provenance_type),
                                            type_name(tgt_inner));
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                result = ty_void;
            }
        } else if (nlen == 7 && memcmp(name, "bitcast", 7) == 0) {
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                /* validate same width */
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = check_expr(c, node->intrinsic.args[0]);
                    int tw = type_width(result);
                    int vw = type_width(val_type);
                    /* BUG-325: type_width returns 0 for structs/unions — use compute_type_size */
                    if (tw == 0 && result) {
                        int64_t sz = compute_type_size(result);
                        if (sz != CONST_EVAL_FAIL) tw = (int)(sz * 8);
                    }
                    if (vw == 0 && val_type) {
                        int64_t sz = compute_type_size(val_type);
                        if (sz != CONST_EVAL_FAIL) vw = (int)(sz * 8);
                    }
                    if (tw > 0 && vw > 0 && tw != vw) {
                        checker_error(c, node->loc.line,
                            "@bitcast requires same-width types (target %d bits, source %d bits)", tw, vw);
                    }
                    /* BUG-341: volatile stripping via @bitcast (same as @ptrcast BUG-258) */
                    Type *veff = type_unwrap_distinct(val_type);
                    Type *reff = type_unwrap_distinct(result);
                    if (veff && veff->kind == TYPE_POINTER &&
                        reff && reff->kind == TYPE_POINTER &&
                        !reff->pointer.is_volatile) {
                        bool src_vol = veff->pointer.is_volatile;
                        if (!src_vol && node->intrinsic.args[0]->kind == NODE_IDENT) {
                            Symbol *vs = scope_lookup(c->current_scope,
                                node->intrinsic.args[0]->ident.name,
                                (uint32_t)node->intrinsic.args[0]->ident.name_len);
                            if (vs && vs->is_volatile) src_vol = true;
                        }
                        if (src_vol) {
                            checker_error(c, node->loc.line,
                                "@bitcast cannot strip volatile qualifier — "
                                "target must be volatile pointer");
                        }
                    }
                }
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "truncate", 8) == 0) {
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                /* validate source is numeric (unwrap distinct) */
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = check_expr(c, node->intrinsic.args[0]);
                    Type *effective = type_unwrap_distinct(val_type);
                    if (effective && !type_is_numeric(effective)) {
                        checker_error(c, node->loc.line,
                            "@truncate requires numeric source, got '%s'", type_name(val_type));
                    }
                }
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "saturate", 8) == 0) {
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                /* validate source is numeric and target is integer */
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = check_expr(c, node->intrinsic.args[0]);
                    Type *effective = type_unwrap_distinct(val_type);
                    if (effective && !type_is_numeric(effective)) {
                        checker_error(c, node->loc.line,
                            "@saturate requires numeric source, got '%s'", type_name(val_type));
                    }
                }
                if (!type_is_integer(result)) {
                    checker_error(c, node->loc.line,
                        "@saturate target must be an integer type, got '%s'", type_name(result));
                }
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "inttoptr", 8) == 0) {
            /* @inttoptr(*T, addr) — addr must be an integer, target must be pointer */
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                /* BUG-375: target type must be a pointer */
                if (result) {
                    Type *res_eff = type_unwrap_distinct(result);
                    if (res_eff->kind != TYPE_POINTER) {
                        checker_error(c, node->loc.line,
                            "@inttoptr target must be a pointer type, got '%s'",
                            type_name(result));
                    }
                }
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = typemap_get(c, node->intrinsic.args[0]);
                    if (val_type) {
                        Type *eff = type_unwrap_distinct(val_type);
                        if (!type_is_integer(eff)) {
                            checker_error(c, node->loc.line,
                                "@inttoptr address must be an integer, got '%s'",
                                type_name(val_type));
                        }
                    }
                    /* mmio range validation: @inttoptr REQUIRES mmio declarations
                     * unless --no-strict-mmio flag is set */
                    if (c->mmio_range_count == 0 && !c->no_strict_mmio) {
                        checker_error(c, node->loc.line,
                            "@inttoptr requires mmio range declarations — "
                            "add 'mmio 0xSTART..0xEND;' or use --no-strict-mmio");
                    }
                    /* BUG-371: also validate constant expressions, not just literals */
                    if (c->mmio_range_count > 0 && node->intrinsic.arg_count > 0) {
                        int64_t cval = eval_const_expr(node->intrinsic.args[0]);
                        if (cval != CONST_EVAL_FAIL) {
                        uint64_t addr = (uint64_t)cval;
                        bool in_range = false;
                        for (int ri = 0; ri < c->mmio_range_count; ri++) {
                            if (addr >= c->mmio_ranges[ri][0] && addr <= c->mmio_ranges[ri][1]) {
                                in_range = true;
                                break;
                            }
                        }
                        if (!in_range) {
                            checker_error(c, node->loc.line,
                                "@inttoptr address 0x%llx is outside all declared mmio ranges",
                                (unsigned long long)addr);
                        }
                    } }
                }
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "ptrtoint", 8) == 0) {
            /* @ptrtoint(ptr) — source must be a pointer */
            if (node->intrinsic.arg_count > 0) {
                Type *val_type = typemap_get(c, node->intrinsic.args[0]);
                if (val_type) {
                    Type *eff = type_unwrap_distinct(val_type);
                    if (eff->kind != TYPE_POINTER && eff->kind != TYPE_FUNC_PTR) {
                        checker_error(c, node->loc.line,
                            "@ptrtoint source must be a pointer, got '%s'",
                            type_name(val_type));
                    }
                }
            }
            result = ty_usize;
        } else if ((nlen == 7 && memcmp(name, "barrier", 7) == 0) ||
                   (nlen == 13 && memcmp(name, "barrier_store", 13) == 0) ||
                   (nlen == 12 && memcmp(name, "barrier_load", 12) == 0)) {
            result = ty_void;
        } else if (nlen == 4 && memcmp(name, "trap", 4) == 0) {
            result = ty_void;
        } else if (nlen == 4 && memcmp(name, "cstr", 4) == 0) {
            /* BUG-238: reject @cstr to const destination */
            if (node->intrinsic.arg_count >= 1 &&
                node->intrinsic.args[0]->kind == NODE_IDENT) {
                Symbol *dst_sym = scope_lookup(c->current_scope,
                    node->intrinsic.args[0]->ident.name,
                    (uint32_t)node->intrinsic.args[0]->ident.name_len);
                if (dst_sym && dst_sym->is_const) {
                    checker_error(c, node->loc.line,
                        "@cstr destination '%.*s' is const — cannot write to read-only buffer",
                        (int)dst_sym->name_len, dst_sym->name);
                }
            }
            /* BUG-241: also check if destination type is const pointer */
            if (node->intrinsic.arg_count >= 1) {
                Type *dst_type = typemap_get(c, node->intrinsic.args[0]);
                if (dst_type && dst_type->kind == TYPE_POINTER && dst_type->pointer.is_const) {
                    checker_error(c, node->loc.line,
                        "@cstr destination is a const pointer — cannot write to read-only memory");
                }
            }
            /* BUG-234: compile-time overflow check when dest is array and src is string literal */
            if (node->intrinsic.arg_count >= 2) {
                Type *dst_type = typemap_get(c, node->intrinsic.args[0]);
                if (dst_type && dst_type->kind == TYPE_ARRAY &&
                    node->intrinsic.args[1]->kind == NODE_STRING_LIT) {
                    uint64_t buf_size = dst_type->array.size;
                    uint64_t str_len = (uint64_t)node->intrinsic.args[1]->string_lit.length;
                    if (str_len + 1 > buf_size) {
                        checker_error(c, node->loc.line,
                            "@cstr buffer overflow: string length %llu + null terminator exceeds buffer size %llu",
                            (unsigned long long)str_len, (unsigned long long)buf_size);
                    }
                }
            }
            result = type_pointer(c->arena, ty_u8);
        } else if (nlen == 9 && memcmp(name, "container", 9) == 0) {
            /* @container(*T, ptr, field) — reverse of &struct.field */
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                /* BUG-375: first arg (ptr) must be a pointer */
                if (node->intrinsic.arg_count > 0) {
                    Type *ptr_type = typemap_get(c, node->intrinsic.args[0]);
                    if (ptr_type) {
                        Type *ptr_eff = type_unwrap_distinct(ptr_type);
                        if (ptr_eff->kind != TYPE_POINTER) {
                            checker_error(c, node->loc.line,
                                "@container source must be a pointer, got '%s'",
                                type_name(ptr_type));
                        }
                    }
                }
                /* validate field exists in target struct */
                Type *tgt = type_unwrap_distinct(result);
                if (tgt && tgt->kind == TYPE_POINTER) tgt = type_unwrap_distinct(tgt->pointer.inner);
                if (tgt && tgt->kind == TYPE_STRUCT &&
                    node->intrinsic.arg_count > 1 &&
                    node->intrinsic.args[1]->kind == NODE_IDENT) {
                    const char *fn = node->intrinsic.args[1]->ident.name;
                    uint32_t fl = (uint32_t)node->intrinsic.args[1]->ident.name_len;
                    bool found = false;
                    for (uint32_t fi = 0; fi < tgt->struct_type.field_count; fi++) {
                        if (tgt->struct_type.fields[fi].name_len == fl &&
                            memcmp(tgt->struct_type.fields[fi].name, fn, fl) == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        checker_error(c, node->loc.line,
                            "@container: struct '%.*s' has no field '%.*s'",
                            (int)tgt->struct_type.name_len, tgt->struct_type.name,
                            (int)fl, fn);
                    }
                }
                /* provenance check: if source pointer has container provenance,
                 * target struct + field must match */
                if (node->intrinsic.arg_count > 0 &&
                    node->intrinsic.args[0]->kind == NODE_IDENT) {
                    Symbol *src_sym = scope_lookup(c->current_scope,
                        node->intrinsic.args[0]->ident.name,
                        (uint32_t)node->intrinsic.args[0]->ident.name_len);
                    if (src_sym && src_sym->container_struct) {
                        /* check struct matches */
                        if (tgt && tgt->kind == TYPE_STRUCT &&
                            tgt != src_sym->container_struct) {
                            checker_error(c, node->loc.line,
                                "@container: pointer provenance is struct '%.*s' "
                                "but target is '%.*s'",
                                (int)src_sym->container_struct->struct_type.name_len,
                                src_sym->container_struct->struct_type.name,
                                (int)tgt->struct_type.name_len, tgt->struct_type.name);
                        }
                        /* check field matches */
                        if (node->intrinsic.arg_count > 1 &&
                            node->intrinsic.args[1]->kind == NODE_IDENT) {
                            const char *fn = node->intrinsic.args[1]->ident.name;
                            uint32_t fl = (uint32_t)node->intrinsic.args[1]->ident.name_len;
                            if (src_sym->container_field_len != fl ||
                                memcmp(src_sym->container_field, fn, fl) != 0) {
                                checker_error(c, node->loc.line,
                                    "@container: pointer was derived from field '%.*s' "
                                    "but used with field '%.*s'",
                                    (int)src_sym->container_field_len, src_sym->container_field,
                                    (int)fl, fn);
                            }
                        }
                    }
                }
                /* BUG-381: volatile check — if source pointer is volatile,
                 * target must also be volatile. Same pattern as @ptrcast BUG-258. */
                if (node->intrinsic.arg_count > 0 && result) {
                    Type *res_eff = type_unwrap_distinct(result);
                    if (res_eff->kind == TYPE_POINTER && !res_eff->pointer.is_volatile) {
                        Type *ptr_type = typemap_get(c, node->intrinsic.args[0]);
                        bool src_volatile = false;
                        if (ptr_type) {
                            Type *pe = type_unwrap_distinct(ptr_type);
                            if (pe->kind == TYPE_POINTER && pe->pointer.is_volatile)
                                src_volatile = true;
                        }
                        if (!src_volatile && node->intrinsic.args[0]->kind == NODE_IDENT) {
                            Symbol *src_sym = scope_lookup(c->current_scope,
                                node->intrinsic.args[0]->ident.name,
                                (uint32_t)node->intrinsic.args[0]->ident.name_len);
                            if (src_sym && src_sym->is_volatile) src_volatile = true;
                        }
                        if (src_volatile) {
                            checker_error(c, node->loc.line,
                                "@container cannot strip volatile qualifier — "
                                "target must be volatile pointer");
                        }
                    }
                }
            } else {
                result = ty_void;
            }
        } else if (nlen == 6 && memcmp(name, "config", 6) == 0) {
            /* @config returns the type of the default value */
            if (node->intrinsic.arg_count > 0) {
                result = check_expr(c, node->intrinsic.args[node->intrinsic.arg_count - 1]);
            } else {
                result = ty_void;
            }
        } else if (nlen == 4 && memcmp(name, "cast", 4) == 0) {
            /* @cast(T, val) — convert between distinct typedef and its underlying type.
             * Valid: @cast(Celsius, u32_val) — wrap underlying → distinct
             * Valid: @cast(u32, celsius_val) — unwrap distinct → underlying
             * Invalid: @cast(Fahrenheit, celsius_val) — cross-distinct */
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = typemap_get(c, node->intrinsic.args[0]);
                    if (val_type) {
                        bool tgt_distinct = result->kind == TYPE_DISTINCT;
                        bool src_distinct = val_type->kind == TYPE_DISTINCT;
                        if (!tgt_distinct && !src_distinct) {
                            checker_error(c, node->loc.line,
                                "@cast requires at least one distinct typedef");
                        } else if (tgt_distinct && src_distinct) {
                            /* cross-distinct: reject unless one directly wraps the other */
                            if (!type_equals(val_type, result->distinct.underlying) &&
                                !type_equals(result, val_type->distinct.underlying)) {
                                checker_error(c, node->loc.line,
                                    "@cast between unrelated distinct types");
                            }
                        } else if (tgt_distinct) {
                            /* wrap: source should match target's underlying */
                            if (!type_equals(val_type, result->distinct.underlying)) {
                                checker_error(c, node->loc.line,
                                    "@cast source type does not match distinct's underlying type");
                            }
                        } else {
                            /* unwrap: target should match source's underlying */
                            if (!type_equals(result, val_type->distinct.underlying)) {
                                checker_error(c, node->loc.line,
                                    "@cast target type does not match distinct's underlying type");
                            }
                        }
                    }
                    /* BUG-343: @cast cannot strip volatile or const qualifier
                     * (same pattern as @ptrcast BUG-258 and @bitcast BUG-341) */
                    Type *veff = type_unwrap_distinct(val_type);
                    Type *reff = type_unwrap_distinct(result);
                    if (veff && veff->kind == TYPE_POINTER &&
                        reff && reff->kind == TYPE_POINTER) {
                        /* volatile check */
                        if (!reff->pointer.is_volatile) {
                            bool src_vol = veff->pointer.is_volatile;
                            if (!src_vol && node->intrinsic.arg_count > 0 &&
                                node->intrinsic.args[0]->kind == NODE_IDENT) {
                                Symbol *vs = scope_lookup(c->current_scope,
                                    node->intrinsic.args[0]->ident.name,
                                    (uint32_t)node->intrinsic.args[0]->ident.name_len);
                                if (vs && vs->is_volatile) src_vol = true;
                            }
                            if (src_vol) {
                                checker_error(c, node->loc.line,
                                    "@cast cannot strip volatile qualifier — "
                                    "target must be volatile pointer");
                            }
                        }
                        /* const check */
                        if (veff->pointer.is_const && !reff->pointer.is_const) {
                            checker_error(c, node->loc.line,
                                "@cast cannot strip const qualifier — "
                                "target must be const pointer");
                        }
                    }
                }
            } else {
                result = ty_void;
            }
        } else {
            checker_error(c, node->loc.line,
                "unknown intrinsic '@%.*s'", (int)nlen, name);
            result = ty_void;
        }
        break;
    }

    default:
        result = ty_void;
        break;
    }

    if (!result) result = ty_void;
    typemap_set(c, node,result);
    c->expr_depth--;
    return result;
}

/* ================================================================
 * STATEMENT TYPE CHECKING
 * ================================================================ */

static void check_stmt(Checker *c, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_BLOCK:
        push_scope(c);
        for (int i = 0; i < node->block.stmt_count; i++) {
            check_stmt(c, node->block.stmts[i]);
        }
        pop_scope(c);
        break;

    case NODE_VAR_DECL:
    case NODE_GLOBAL_VAR: {
        Type *type = resolve_type(c, node->var_decl.type);
        /* void variables are invalid — void is for return types only */
        if (type && type->kind == TYPE_VOID) {
            checker_error(c, node->loc.line,
                "cannot declare variable of type 'void'");
        }
        /* Pool/Ring/Slab must be global or static — not on the stack */
        if (node->kind == NODE_VAR_DECL && type &&
            (type->kind == TYPE_POOL || type->kind == TYPE_RING || type->kind == TYPE_SLAB) &&
            !node->var_decl.is_static) {
            checker_error(c, node->loc.line,
                "%s must be declared as global or static — "
                "stack allocation risks overflow",
                type->kind == TYPE_POOL ? "Pool" : type->kind == TYPE_RING ? "Ring" : "Slab");
        }
        /* propagate const from var qualifier to slice/pointer type */
        if (node->var_decl.is_const && type) {
            if (type->kind == TYPE_SLICE && !type->slice.is_const) {
                type = type_const_slice(c->arena, type->slice.inner);
            } else if (type->kind == TYPE_POINTER && !type->pointer.is_const) {
                type = type_const_pointer(c->arena, type->pointer.inner);
            }
        }
        /* propagate volatile from var qualifier to slice type */
        if (node->var_decl.is_volatile && type) {
            if (type->kind == TYPE_SLICE && !type->slice.is_volatile) {
                type = type_volatile_slice(c->arena, type->slice.inner);
                if (node->var_decl.is_const) type->slice.is_const = true;
            }
        }
        /* BUG-239/253: non-null pointer (*T) requires initializer — auto-zero creates NULL.
         * Applies to both local (NODE_VAR_DECL) and global (NODE_GLOBAL_VAR). */
        if (!node->var_decl.init && type && type->kind == TYPE_POINTER) {
            checker_error(c, node->loc.line,
                "non-null pointer '*%s' requires an initializer — "
                "use '?*%s' for nullable pointers",
                type_name(type->pointer.inner), type_name(type->pointer.inner));
        }

        typemap_set(c, node,type); /* store for emitter to read via checker_get_type */

        if (node->var_decl.init) {
            Type *init_type = check_expr(c, node->var_decl.init);

            /* non-storable check: pool.get(h) result */
            if (is_non_storable(c, node->var_decl.init)) {
                checker_error(c, node->loc.line,
                    "cannot store result of get() — use inline");
            }

            /* string literal to mutable slice: runtime crash on write.
             * String literals are const []u8 — only assign to const variables. */
            if (node->var_decl.init->kind == NODE_STRING_LIT &&
                type && type->kind == TYPE_SLICE && !node->var_decl.is_const) {
                checker_error(c, node->loc.line,
                    "string literal is read-only — use 'const []u8' instead of '[]u8'");
            }

            /* const slice/pointer → mutable variable: blocked (prevents write to .rodata) */
            if (!node->var_decl.is_const && node->var_decl.init->kind == NODE_IDENT &&
                type && (type->kind == TYPE_SLICE || type->kind == TYPE_POINTER)) {
                Symbol *src = scope_lookup(c->current_scope,
                    node->var_decl.init->ident.name,
                    (uint32_t)node->var_decl.init->ident.name_len);
                if (src && src->is_const) {
                    checker_error(c, node->loc.line,
                        "cannot initialize mutable '%.*s' from const variable '%.*s'",
                        (int)node->var_decl.name_len, node->var_decl.name,
                        (int)src->name_len, src->name);
                }
            }

            /* check assignment compatibility */
            /* const laundering: reject const → mutable in init */
            if (type && init_type) {
                if (type->kind == TYPE_POINTER && init_type->kind == TYPE_POINTER &&
                    init_type->pointer.is_const && !type->pointer.is_const) {
                    checker_error(c, node->loc.line,
                        "cannot initialize mutable pointer from const — "
                        "would allow writing to read-only memory");
                }
                /* BUG-197/282: volatile pointer → non-volatile drops volatile qualifier.
                 * Check both type-level (pointer.is_volatile) and symbol-level (sym->is_volatile). */
                if (type->kind == TYPE_POINTER && init_type->kind == TYPE_POINTER &&
                    !type->pointer.is_volatile && !node->var_decl.is_volatile) {
                    bool src_volatile = init_type->pointer.is_volatile;
                    if (!src_volatile && node->var_decl.init->kind == NODE_IDENT) {
                        Symbol *vs = scope_lookup(c->current_scope,
                            node->var_decl.init->ident.name,
                            (uint32_t)node->var_decl.init->ident.name_len);
                        if (vs && vs->is_volatile) src_volatile = true;
                    }
                    if (src_volatile) {
                        checker_error(c, node->loc.line,
                            "cannot initialize non-volatile pointer from volatile — "
                            "writes through non-volatile pointer may be optimized away");
                    }
                }
                if (type->kind == TYPE_SLICE && init_type->kind == TYPE_SLICE &&
                    init_type->slice.is_const && !type->slice.is_const) {
                    checker_error(c, node->loc.line,
                        "cannot initialize mutable slice from const — "
                        "would allow writing to read-only memory");
                }
                /* BUG-310: volatile array → non-volatile slice rejected */
                if (type->kind == TYPE_SLICE && !type->slice.is_volatile &&
                    init_type->kind == TYPE_ARRAY &&
                    node->var_decl.init->kind == NODE_IDENT) {
                    Symbol *vs = scope_lookup(c->current_scope,
                        node->var_decl.init->ident.name,
                        (uint32_t)node->var_decl.init->ident.name_len);
                    if (vs && vs->is_volatile) {
                        checker_error(c, node->loc.line,
                            "cannot initialize non-volatile slice from volatile array — "
                            "use 'volatile []%s' to preserve volatile qualifier",
                            type_name(type->slice.inner));
                    }
                }
            }

            if (!type_equals(type, init_type) &&
                !can_implicit_coerce(init_type, type) &&
                !is_literal_compatible(node->var_decl.init, type)) {
                /* RF6: better error for null used with non-optional type */
                if (node->var_decl.init->kind == NODE_NULL_LIT) {
                    checker_error(c, node->loc.line,
                        "'null' can only be assigned to optional types (?*T, ?T) — "
                        "'%s' is not optional",
                        type_name(type));
                } else {
                    checker_error(c, node->loc.line,
                        "cannot initialize '%.*s' of type '%s' with '%s'",
                        (int)node->var_decl.name_len, node->var_decl.name,
                        type_name(type), type_name(init_type));
                }
            }
            /* BUG-373: integer literal range check — even if coercion passed,
             * verify the literal value fits the target type. Literals default
             * to ty_u32 so can_implicit_coerce may accept oversized values. */
            if (node->var_decl.init->kind == NODE_INT_LIT &&
                type && type_is_integer(type_unwrap_distinct(type))) {
                if (!is_literal_compatible(node->var_decl.init, type)) {
                    checker_error(c, node->loc.line,
                        "integer literal %llu does not fit in '%s'",
                        (unsigned long long)node->var_decl.init->int_lit.value,
                        type_name(type));
                }
            }
        }

        Symbol *sym = add_symbol(c, node->var_decl.name,
                                 (uint32_t)node->var_decl.name_len,
                                 type, node->loc.line);
        if (sym) {
            sym->is_const = node->var_decl.is_const;
            sym->is_volatile = node->var_decl.is_volatile;
            sym->is_static = node->var_decl.is_static;

            /* detect arena-derived pointers: x = arena.alloc(T) orelse ... */
            if (node->var_decl.init) {
                Node *alloc_call = node->var_decl.init;
                if (alloc_call->kind == NODE_ORELSE)
                    alloc_call = alloc_call->orelse.expr;
                if (alloc_call && alloc_call->kind == NODE_CALL &&
                    alloc_call->call.callee->kind == NODE_FIELD) {
                    Node *obj = alloc_call->call.callee->field.object;
                    const char *mname = alloc_call->call.callee->field.field_name;
                    size_t mlen = alloc_call->call.callee->field.field_name_len;
                    if (obj && ((mlen == 5 && memcmp(mname, "alloc", 5) == 0) ||
                               (mlen == 11 && memcmp(mname, "alloc_slice", 11) == 0))) {
                        Type *obj_type = typemap_get(c, obj);
                        if (obj_type && obj_type->kind == TYPE_ARENA) {
                            /* only mark as arena-derived if arena is LOCAL
                             * (global arenas outlive functions, pointers safe to return) */
                            bool arena_is_global = false;
                            if (obj->kind == NODE_IDENT) {
                                arena_is_global = scope_lookup_local(c->global_scope,
                                    obj->ident.name, (uint32_t)obj->ident.name_len) != NULL;
                            }
                            if (!arena_is_global)
                                sym->is_arena_derived = true;
                        }
                    }
                }
                /* propagate arena/local-derived from init expression
                 * Walk field/index chains to find root (handles w.ptr, arr[i])
                 * BUG-318: check BOTH orelse.expr AND orelse.fallback */
                {
                    Node *checks[2] = { node->var_decl.init, NULL };
                    int check_count = 1;
                    if (checks[0] && checks[0]->kind == NODE_ORELSE) {
                        checks[0] = node->var_decl.init->orelse.expr;
                        if (node->var_decl.init->orelse.fallback)
                            checks[check_count++] = node->var_decl.init->orelse.fallback;
                    }
                    for (int ci = 0; ci < check_count; ci++) {
                        Node *init_root = checks[ci];
                        while (init_root) {
                            if (init_root->kind == NODE_FIELD) init_root = init_root->field.object;
                            else if (init_root->kind == NODE_INDEX) init_root = init_root->index_expr.object;
                            /* BUG-338: walk into intrinsic args (ptrcast, bitcast) */
                            else if (init_root->kind == NODE_INTRINSIC && init_root->intrinsic.arg_count > 0)
                                init_root = init_root->intrinsic.args[init_root->intrinsic.arg_count - 1];
                            /* walk into & — &x root is x */
                            else if (init_root->kind == NODE_UNARY && init_root->unary.op == TOK_AMP)
                                init_root = init_root->unary.operand;
                            /* BUG-356: walk through deref — *pp root is pp */
                            else if (init_root->kind == NODE_UNARY && init_root->unary.op == TOK_STAR)
                                init_root = init_root->unary.operand;
                            else break;
                        }
                        if (init_root && init_root->kind == NODE_IDENT) {
                            Symbol *src = scope_lookup(c->current_scope,
                                init_root->ident.name,
                                (uint32_t)init_root->ident.name_len);
                            if (src && src->is_arena_derived)
                                sym->is_arena_derived = true;
                            if (src && src->is_local_derived)
                                sym->is_local_derived = true;
                        }
                    }
                }

                /* detect pointer to local: p = &local or p = &local.field
                 * BUG-202: also check orelse fallback: p = opt orelse &local */
                {
                    Node *init = node->var_decl.init;
                    /* check both direct &local AND orelse fallback &local */
                    Node *addr_exprs[4] = { NULL, NULL, NULL, NULL };
                    int addr_count = 0;
                    /* BUG-338: walk into intrinsics to find &local */
                    Node *init_unwrap = init;
                    while (init_unwrap && init_unwrap->kind == NODE_INTRINSIC &&
                           init_unwrap->intrinsic.arg_count > 0)
                        init_unwrap = init_unwrap->intrinsic.args[init_unwrap->intrinsic.arg_count - 1];
                    if (init_unwrap && init_unwrap->kind == NODE_UNARY && init_unwrap->unary.op == TOK_AMP) {
                        addr_exprs[addr_count++] = init_unwrap;
                    }
                    if (init->kind == NODE_UNARY && init->unary.op == TOK_AMP) {
                        if (addr_count == 0 || addr_exprs[0] != init)
                            addr_exprs[addr_count++] = init;
                    }
                    if (init->kind == NODE_ORELSE && init->orelse.fallback) {
                        Node *fb = init->orelse.fallback;
                        while (fb && fb->kind == NODE_INTRINSIC && fb->intrinsic.arg_count > 0)
                            fb = fb->intrinsic.args[fb->intrinsic.arg_count - 1];
                        if (fb && fb->kind == NODE_UNARY && fb->unary.op == TOK_AMP)
                            addr_exprs[addr_count++] = fb;
                    }
                    for (int ai = 0; ai < addr_count; ai++) {
                        Node *root = addr_exprs[ai]->unary.operand;
                        while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                            if (root->kind == NODE_FIELD) root = root->field.object;
                            else root = root->index_expr.object;
                        }
                        if (root && root->kind == NODE_IDENT) {
                            bool is_global = scope_lookup_local(c->global_scope,
                                root->ident.name, (uint32_t)root->ident.name_len) != NULL;
                            Symbol *src = scope_lookup(c->current_scope,
                                root->ident.name, (uint32_t)root->ident.name_len);
                            if (src && !src->is_static && !is_global) {
                                sym->is_local_derived = true;
                            }
                        }
                    }
                }
            }

            /* BUG-203/207/377: slice from local array — mark as local-derived.
             * []T s = local_array OR []T s = local_array[1..4]
             * Both create a slice pointing to stack memory.
             * BUG-377: also check orelse fallback — s = opt orelse local_buf. */
            if (node->var_decl.init && type && type->kind == TYPE_SLICE) {
                /* collect nodes to check: direct init + orelse fallback */
                Node *arr_roots[2] = { node->var_decl.init, NULL };
                int arr_root_count = 1;
                if (node->var_decl.init->kind == NODE_ORELSE &&
                    node->var_decl.init->orelse.fallback) {
                    arr_roots[arr_root_count++] = node->var_decl.init->orelse.fallback;
                }
                for (int ari = 0; ari < arr_root_count; ari++) {
                    Node *slice_root = arr_roots[ari];
                    /* walk through NODE_SLICE to find the object being sliced */
                    if (slice_root->kind == NODE_SLICE)
                        slice_root = slice_root->slice.object;
                    /* walk field/index chains */
                    while (slice_root && (slice_root->kind == NODE_FIELD ||
                                           slice_root->kind == NODE_INDEX)) {
                        if (slice_root->kind == NODE_FIELD) slice_root = slice_root->field.object;
                        else slice_root = slice_root->index_expr.object;
                    }
                    if (slice_root && slice_root->kind == NODE_IDENT) {
                        Type *root_type = typemap_get(c, slice_root);
                        Symbol *src = scope_lookup(c->current_scope,
                            slice_root->ident.name,
                            (uint32_t)slice_root->ident.name_len);
                        /* BUG-214: also propagate if source is already local-derived (slice-to-slice) */
                        if (src && src->is_local_derived) {
                            sym->is_local_derived = true;
                        } else if (root_type && type_unwrap_distinct(root_type)->kind == TYPE_ARRAY) {
                            bool is_global = src && scope_lookup_local(c->global_scope,
                                src->name, src->name_len) != NULL;
                            if (src && !src->is_static && !is_global) {
                                sym->is_local_derived = true;
                            }
                        }
                    }
                }
            }

            /* @ptrcast provenance: track original type when casting to *opaque.
             * *opaque ctx = @ptrcast(*opaque, sensor_ptr) → provenance = Sensor type.
             * Also propagate provenance through aliases: q = p where p has provenance. */
            if (sym && node->var_decl.init) {
                Node *init = node->var_decl.init;
                if (init->kind == NODE_ORELSE) init = init->orelse.expr;
                /* direct @ptrcast to *opaque — record source type */
                if (init->kind == NODE_INTRINSIC &&
                    init->intrinsic.name_len == 7 &&
                    memcmp(init->intrinsic.name, "ptrcast", 7) == 0 &&
                    init->intrinsic.arg_count > 0) {
                    Type *eff = type_unwrap_distinct(type);
                    if (eff && eff->kind == TYPE_POINTER &&
                        eff->pointer.inner->kind == TYPE_OPAQUE) {
                        /* target is *opaque — record source type */
                        Type *src_type = typemap_get(c, init->intrinsic.args[0]);
                        if (src_type) sym->provenance_type = src_type;
                    }
                }
                /* alias propagation: q = p where p has provenance
                 * BUG-358: also walk through @bitcast/@cast to find root ident */
                {
                    Node *prov_root = init;
                    while (prov_root && prov_root->kind == NODE_INTRINSIC &&
                           prov_root->intrinsic.arg_count > 0)
                        prov_root = prov_root->intrinsic.args[prov_root->intrinsic.arg_count - 1];
                    if (prov_root && prov_root->kind == NODE_IDENT) {
                        Symbol *src = scope_lookup(c->current_scope,
                            prov_root->ident.name, (uint32_t)prov_root->ident.name_len);
                        if (src && src->provenance_type)
                            sym->provenance_type = src->provenance_type;
                        if (src && src->container_struct) {
                            sym->container_struct = src->container_struct;
                            sym->container_field = src->container_field;
                            sym->container_field_len = src->container_field_len;
                        }
                    }
                }
            }

            /* @container provenance: track struct+field when ptr = &struct.field */
            if (sym && node->var_decl.init) {
                Node *init = node->var_decl.init;
                if (init->kind == NODE_UNARY && init->unary.op == TOK_AMP &&
                    init->unary.operand->kind == NODE_FIELD) {
                    Node *field_node = init->unary.operand;
                    Type *obj_type = typemap_get(c, field_node->field.object);
                    if (obj_type) {
                        Type *st = type_unwrap_distinct(obj_type);
                        /* walk through pointer auto-deref */
                        if (st && st->kind == TYPE_POINTER) st = type_unwrap_distinct(st->pointer.inner);
                        if (st && st->kind == TYPE_STRUCT) {
                            sym->container_struct = st;
                            sym->container_field = field_node->field.field_name;
                            sym->container_field_len = (uint32_t)field_node->field.field_name_len;
                        }
                    }
                }
            }

            /* BUG-360/374: function call returning pointer — if any pointer arg is
             * local-derived, conservatively mark the result as local-derived.
             * identity(&x) returns &x, must not escape.
             * BUG-374: recurse into nested calls — identity(identity(&x)). */
            if (sym && node->var_decl.init && type &&
                type->kind == TYPE_POINTER) {
                Node *call = node->var_decl.init;
                if (call->kind == NODE_ORELSE) call = call->orelse.expr;
                if (call->kind == NODE_CALL) {
                    if (call_has_local_derived_arg(c, call, 0)) {
                        sym->is_local_derived = true;
                    }
                }
            }
        }
        break;
    }

    case NODE_IF: {
        /* comptime if — evaluate condition at compile time, only check taken branch */
        if (node->if_stmt.is_comptime) {
            int64_t cval = eval_const_expr(node->if_stmt.cond);
            if (cval == CONST_EVAL_FAIL) {
                /* try looking up const bool/int ident */
                if (node->if_stmt.cond->kind == NODE_IDENT) {
                    Symbol *cs = scope_lookup(c->current_scope,
                        node->if_stmt.cond->ident.name,
                        (uint32_t)node->if_stmt.cond->ident.name_len);
                    if (cs && cs->is_const && cs->is_comptime) {
                        /* comptime bool constant — need the init value */
                        if (cs->func_node && cs->func_node->kind == NODE_GLOBAL_VAR &&
                            cs->func_node->var_decl.init)
                            cval = eval_const_expr(cs->func_node->var_decl.init);
                    }
                }
            }
            if (cval == CONST_EVAL_FAIL) {
                checker_error(c, node->loc.line,
                    "comptime if condition must be a compile-time constant");
            }
            /* store result for emitter */
            node->if_stmt.cond->int_lit.value = (uint64_t)(cval ? 1 : 0);
            /* only check the taken branch */
            if (cval) {
                if (node->if_stmt.then_body) check_stmt(c, node->if_stmt.then_body);
            } else {
                if (node->if_stmt.else_body) check_stmt(c, node->if_stmt.else_body);
            }
            break;
        }

        Type *cond = check_expr(c, node->if_stmt.cond);

        /* if-unwrap: cond must be ?T */
        if (node->if_stmt.capture_name) {
            if (!type_is_optional(cond)) {
                checker_error(c, node->loc.line,
                    "if-unwrap requires optional type, got '%s'", type_name(cond));
            } else {
                /* create scope with captured variable */
                push_scope(c);
                Type *unwrapped = type_unwrap_optional(cond);
                Type *cap_type;
                bool cap_const;
                if (node->if_stmt.capture_is_ptr) {
                    cap_type = type_pointer(c->arena, unwrapped);
                    /* BUG-305: if source is const, capture pointer must be const */
                    cap_const = false;
                    {
                        Node *cr = node->if_stmt.cond;
                        while (cr && (cr->kind == NODE_FIELD || cr->kind == NODE_INDEX))
                            cr = cr->kind == NODE_FIELD ? cr->field.object : cr->index_expr.object;
                        if (cr && cr->kind == NODE_IDENT) {
                            Symbol *cs = scope_lookup(c->current_scope,
                                cr->ident.name, (uint32_t)cr->ident.name_len);
                            if (cs && cs->is_const) {
                                cap_const = true;
                                cap_type->pointer.is_const = true;
                            }
                        }
                    }
                } else {
                    cap_type = unwrapped;
                    cap_const = true;
                }
                Symbol *cap = add_symbol(c, node->if_stmt.capture_name,
                    (uint32_t)node->if_stmt.capture_name_len,
                    cap_type, node->loc.line);
                if (cap) {
                    cap->is_const = cap_const;

                    /* BUG-212: propagate local/arena-derived from condition ident */
                    {
                        Node *croot = node->if_stmt.cond;
                        if (croot->kind == NODE_ORELSE) croot = croot->orelse.expr;
                        while (croot && (croot->kind == NODE_FIELD || croot->kind == NODE_INDEX)) {
                            if (croot->kind == NODE_FIELD) croot = croot->field.object;
                            else croot = croot->index_expr.object;
                        }
                        if (croot && croot->kind == NODE_IDENT) {
                            Symbol *csym = scope_lookup(c->current_scope,
                                croot->ident.name, (uint32_t)croot->ident.name_len);
                            if (csym && csym->is_local_derived) cap->is_local_derived = true;
                            if (csym && csym->is_arena_derived) cap->is_arena_derived = true;
                        }
                    }

                    /* propagate arena-derived from if-unwrap condition:
                     * if (arena.alloc(T)) |t| { ... } — t is arena-derived */
                    Node *cond_expr = node->if_stmt.cond;
                    if (cond_expr->kind == NODE_CALL &&
                        cond_expr->call.callee->kind == NODE_FIELD) {
                        Node *obj = cond_expr->call.callee->field.object;
                        const char *mname = cond_expr->call.callee->field.field_name;
                        size_t mlen = cond_expr->call.callee->field.field_name_len;
                        if (obj) {
                            Type *obj_type = typemap_get(c, obj);
                            if (obj_type && obj_type->kind == TYPE_ARENA &&
                                ((mlen == 5 && memcmp(mname, "alloc", 5) == 0) ||
                                 (mlen == 11 && memcmp(mname, "alloc_slice", 11) == 0))) {
                                cap->is_arena_derived = true;
                            }
                        }
                    }
                }

                check_stmt(c, node->if_stmt.then_body);
                pop_scope(c);

                if (node->if_stmt.else_body)
                    check_stmt(c, node->if_stmt.else_body);
                break;
            }
        }

        /* regular if: cond must be bool or optional */
        if (!type_equals(cond, ty_bool) && !type_is_optional(cond)) {
            checker_error(c, node->loc.line,
                "if condition must be bool or optional, got '%s'", type_name(cond));
        }
        check_stmt(c, node->if_stmt.then_body);
        if (node->if_stmt.else_body)
            check_stmt(c, node->if_stmt.else_body);
        break;
    }

    case NODE_FOR: {
        push_scope(c); /* for loop has its own scope */
        if (node->for_stmt.init) check_stmt(c, node->for_stmt.init);
        if (node->for_stmt.cond) {
            Type *cond = check_expr(c, node->for_stmt.cond);
            if (!type_equals(cond, ty_bool)) {
                checker_error(c, node->loc.line,
                    "for condition must be bool, got '%s'", type_name(cond));
            }
        }
        if (node->for_stmt.step) check_expr(c, node->for_stmt.step);

        bool prev_in_loop = c->in_loop;
        c->in_loop = true;
        check_stmt(c, node->for_stmt.body);
        c->in_loop = prev_in_loop;
        pop_scope(c);
        break;
    }

    case NODE_WHILE: {
        Type *cond = check_expr(c, node->while_stmt.cond);
        if (!type_equals(cond, ty_bool)) {
            checker_error(c, node->loc.line,
                "while condition must be bool, got '%s'", type_name(cond));
        }
        bool prev_in_loop = c->in_loop;
        c->in_loop = true;
        check_stmt(c, node->while_stmt.body);
        c->in_loop = prev_in_loop;
        break;
    }

    case NODE_SWITCH: {
        Type *expr = check_expr(c, node->switch_stmt.expr);
        /* BUG-271: unwrap distinct for union/enum switch dispatch */
        Type *expr_eff = expr ? type_unwrap_distinct(expr) : NULL;

        /* BUG-226: reject float switch — spec says "switch on float: NOT ALLOWED" */
        if (expr && type_is_float(expr)) {
            checker_error(c, node->loc.line,
                "cannot switch on float type '%s' — use if/else for float comparisons",
                type_name(expr));
        }

        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            SwitchArm *arm = &node->switch_stmt.arms[i];

            /* check match values — skip for enum/union dot syntax (.variant) */
            if (!arm->is_enum_dot) {
                for (int j = 0; j < arm->value_count; j++) {
                    check_expr(c, arm->values[j]);
                }
            }

            /* handle capture */
            if (arm->capture_name) {
                push_scope(c);
                Type *cap_type;
                bool cap_const;

                /* BUG-326: check if source is const — walk to root ident */
                bool switch_src_const = false;
                {
                    Node *cr = node->switch_stmt.expr;
                    while (cr && (cr->kind == NODE_FIELD || cr->kind == NODE_INDEX))
                        cr = cr->kind == NODE_FIELD ? cr->field.object : cr->index_expr.object;
                    if (cr && cr->kind == NODE_IDENT) {
                        Symbol *cs = scope_lookup(c->current_scope,
                            cr->ident.name, (uint32_t)cr->ident.name_len);
                        if (cs && cs->is_const) switch_src_const = true;
                    }
                }

                if (expr_eff->kind == TYPE_UNION) {
                    /* tagged union switch — look up variant type */
                    Type *variant_type = ty_void;
                    if (arm->value_count > 0 && arm->values[0]->kind == NODE_IDENT) {
                        const char *vname = arm->values[0]->ident.name;
                        uint32_t vlen = (uint32_t)arm->values[0]->ident.name_len;
                        for (uint32_t k = 0; k < expr_eff->union_type.variant_count; k++) {
                            SUVariant *v = &expr_eff->union_type.variants[k];
                            if (v->name_len == vlen && memcmp(v->name, vname, vlen) == 0) {
                                variant_type = v->type;
                                break;
                            }
                        }
                    }
                    if (arm->capture_is_ptr) {
                        cap_type = type_pointer(c->arena, variant_type);
                        cap_const = switch_src_const; /* BUG-326 */
                        if (cap_const) cap_type->pointer.is_const = true;
                    } else {
                        cap_type = variant_type;
                        cap_const = true;
                    }
                } else if (type_is_optional(expr)) {
                    /* optional switch with capture */
                    Type *unwrapped = type_unwrap_optional(expr);
                    if (arm->capture_is_ptr) {
                        cap_type = type_pointer(c->arena, unwrapped);
                        cap_const = switch_src_const; /* BUG-326 */
                        if (cap_const) cap_type->pointer.is_const = true;
                    } else {
                        cap_type = unwrapped;
                        cap_const = true;
                    }
                } else {
                    cap_type = expr;
                    cap_const = true;
                }

                Symbol *cap = add_symbol(c, arm->capture_name,
                    (uint32_t)arm->capture_name_len,
                    cap_type, arm->loc.line);
                if (cap) cap->is_const = cap_const;

                /* BUG-249: propagate safety flags from switch expression to capture.
                 * Same pattern as if-unwrap (BUG-212). */
                if (cap) {
                    Node *sw_root = node->switch_stmt.expr;
                    while (sw_root) {
                        if (sw_root->kind == NODE_UNARY && sw_root->unary.op == TOK_STAR)
                            sw_root = sw_root->unary.operand;
                        else if (sw_root->kind == NODE_FIELD)
                            sw_root = sw_root->field.object;
                        else if (sw_root->kind == NODE_INDEX)
                            sw_root = sw_root->index_expr.object;
                        else if (sw_root->kind == NODE_ORELSE)
                            sw_root = sw_root->orelse.expr;
                        else break;
                    }
                    if (sw_root && sw_root->kind == NODE_IDENT) {
                        Symbol *src = scope_lookup(c->current_scope,
                            sw_root->ident.name, (uint32_t)sw_root->ident.name_len);
                        if (src && src->is_local_derived) cap->is_local_derived = true;
                        if (src && src->is_arena_derived) cap->is_arena_derived = true;
                    }
                }

                /* lock union variable during switch arm to prevent type confusion.
                 * Handles: switch (d) and switch (*ptr) where ptr points to union */
                const char *saved_union_var = c->union_switch_var;
                uint32_t saved_union_var_len = c->union_switch_var_len;
                Type *saved_union_type = c->union_switch_type;
                if (expr_eff->kind == TYPE_UNION) {
                    c->union_switch_type = expr;
                    Node *sw_expr = node->switch_stmt.expr;
                    /* walk to root ident for union lock.
                     * Handles: switch(d), switch(*ptr), switch(s.msg), switch(s.msg.inner) */
                    /* BUG-244: walk ALL deref/field/index levels to find root.
                     * Handles: switch(**pp), switch(*ptr), switch(s.msg), etc. */
                    Node *lock_root = sw_expr;
                    while (lock_root) {
                        if (lock_root->kind == NODE_UNARY && lock_root->unary.op == TOK_STAR)
                            lock_root = lock_root->unary.operand;
                        else if (lock_root->kind == NODE_FIELD)
                            lock_root = lock_root->field.object;
                        else if (lock_root->kind == NODE_INDEX)
                            lock_root = lock_root->index_expr.object;
                        else break;
                    }
                    if (lock_root->kind == NODE_IDENT) {
                        c->union_switch_var = lock_root->ident.name;
                        c->union_switch_var_len = (uint32_t)lock_root->ident.name_len;
                    }
                }
                check_stmt(c, arm->body);
                c->union_switch_var = saved_union_var;
                c->union_switch_var_len = saved_union_var_len;
                c->union_switch_type = saved_union_type;
                pop_scope(c);
            } else {
                check_stmt(c, arm->body);
            }
        }

        /* exhaustiveness check — unwrap distinct for type dispatch */
        {
            Type *sw_type = type_unwrap_distinct(expr);
            bool has_default = false;
            for (int i = 0; i < node->switch_stmt.arm_count; i++) {
                if (node->switch_stmt.arms[i].is_default) {
                    has_default = true;
                    break;
                }
            }

            if (sw_type->kind == TYPE_ENUM) {
                /* enum switch: must handle all variants OR have default */
                if (!has_default) {
                    uint32_t total = sw_type->enum_type.variant_count;
                    /* track which variants are covered using a byte array (supports any count) */
                    uint8_t covered_stack[256] = {0};
                    uint8_t *covered = covered_stack;
                    if (total > 256) {
                        covered = (uint8_t *)arena_alloc(c->arena, total);
                        memset(covered, 0, total);
                    }
                    for (int i = 0; i < node->switch_stmt.arm_count; i++) {
                        SwitchArm *arm = &node->switch_stmt.arms[i];
                        for (int v = 0; v < arm->value_count; v++) {
                            Node *val = arm->values[v];
                            /* get variant name from arm value (dot-prefix or qualified) */
                            const char *vname = NULL;
                            size_t vname_len = 0;
                            if (val && val->kind == NODE_IDENT) {
                                /* .variant — dot-prefix form */
                                vname = val->ident.name;
                                vname_len = val->ident.name_len;
                            } else if (val && val->kind == NODE_FIELD) {
                                /* Dir.variant — qualified form */
                                vname = val->field.field_name;
                                vname_len = val->field.field_name_len;
                            }
                            if (vname) {
                                for (uint32_t vi = 0; vi < total; vi++) {
                                    if (sw_type->enum_type.variants[vi].name_len == vname_len &&
                                        memcmp(sw_type->enum_type.variants[vi].name, vname, vname_len) == 0) {
                                        covered[vi] = 1;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    /* count covered variants */
                    uint32_t handled = 0;
                    for (uint32_t b = 0; b < total; b++) {
                        if (covered[b]) handled++;
                    }
                    if (handled < total) {
                        checker_error(c, node->loc.line,
                            "switch on enum '%.*s' is not exhaustive — "
                            "handles %u of %u variants",
                            (int)sw_type->enum_type.name_len,
                            sw_type->enum_type.name, handled, total);
                    }
                }
            } else if (type_equals(sw_type, ty_bool)) {
                /* bool switch: must handle both true and false */
                if (!has_default) {
                    bool has_true = false, has_false = false;
                    for (int i = 0; i < node->switch_stmt.arm_count; i++) {
                        SwitchArm *arm = &node->switch_stmt.arms[i];
                        for (int v = 0; v < arm->value_count; v++) {
                            Node *val = arm->values[v];
                            if (val && val->kind == NODE_BOOL_LIT) {
                                if (val->bool_lit.value) has_true = true;
                                else has_false = true;
                            }
                        }
                    }
                    if (!has_true || !has_false) {
                        checker_error(c, node->loc.line,
                            "switch on bool must handle both true and false");
                    }
                }
            } else if (type_is_integer(sw_type)) {
                /* integer switch: must have default */
                if (!has_default) {
                    checker_error(c, node->loc.line,
                        "switch on integer must have a default arm");
                }
            } else if (sw_type->kind == TYPE_UNION) {
                /* validate variant names + exhaustiveness */
                uint32_t total = sw_type->union_type.variant_count;
                uint8_t ucov_stack[256] = {0};
                uint8_t *covered = ucov_stack;
                if (total > 256) {
                    covered = (uint8_t *)arena_alloc(c->arena, total);
                    memset(covered, 0, total);
                }
                for (int i = 0; i < node->switch_stmt.arm_count; i++) {
                    SwitchArm *arm = &node->switch_stmt.arms[i];
                    if (arm->is_default) continue;
                    for (int v = 0; v < arm->value_count; v++) {
                        Node *val = arm->values[v];
                        const char *vname = NULL;
                        size_t vname_len = 0;
                        if (val && val->kind == NODE_IDENT) {
                            vname = val->ident.name;
                            vname_len = val->ident.name_len;
                        } else if (val && val->kind == NODE_FIELD) {
                            vname = val->field.field_name;
                            vname_len = val->field.field_name_len;
                        }
                        if (vname) {
                            bool found = false;
                            for (uint32_t vi = 0; vi < total; vi++) {
                                if (sw_type->union_type.variants[vi].name_len == (uint32_t)vname_len &&
                                    memcmp(sw_type->union_type.variants[vi].name, vname, vname_len) == 0) {
                                    covered[vi] = 1;
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                checker_error(c, node->loc.line,
                                    "no variant '%.*s' in union '%.*s'",
                                    (int)vname_len, vname,
                                    (int)sw_type->union_type.name_len, sw_type->union_type.name);
                            }
                        }
                    }
                }
                if (!has_default) {
                    uint32_t handled = 0;
                    for (uint32_t b = 0; b < total; b++) {
                        if (covered[b]) handled++;
                    }
                    if (handled < total) {
                        checker_error(c, node->loc.line,
                            "switch on union '%.*s' is not exhaustive — "
                            "handles %u of %u variants",
                            (int)sw_type->union_type.name_len,
                            sw_type->union_type.name, handled, total);
                    }
                }
            }
        }
        break;
    }

    case NODE_RETURN: {
        /* return inside defer is illegal — would corrupt control flow */
        if (c->defer_depth > 0) {
            checker_error(c, node->loc.line,
                "cannot use 'return' inside defer block");
            break;
        }

        if (node->ret.expr) {
            Type *ret_type = check_expr(c, node->ret.expr);

            /* string literal returned as mutable slice → .rodata write risk
             * Covers both []u8 and ?[]u8 return types */
            if (node->ret.expr->kind == NODE_STRING_LIT && c->current_func_ret) {
                Type *ret = c->current_func_ret;
                if (ret->kind == TYPE_SLICE ||
                    (ret->kind == TYPE_OPTIONAL && ret->optional.inner->kind == TYPE_SLICE)) {
                    checker_error(c, node->loc.line,
                        "cannot return string literal as mutable slice — data is read-only");
                }
            }

            /* const/volatile laundering: reject returning qualified ptr as unqualified */
            if (ret_type && c->current_func_ret) {
                if (ret_type->kind == TYPE_POINTER && c->current_func_ret->kind == TYPE_POINTER &&
                    ret_type->pointer.is_const && !c->current_func_ret->pointer.is_const) {
                    checker_error(c, node->loc.line,
                        "cannot return const pointer as mutable — would allow writing to read-only memory");
                }
                if (ret_type->kind == TYPE_SLICE && c->current_func_ret->kind == TYPE_SLICE &&
                    ret_type->slice.is_const && !c->current_func_ret->slice.is_const) {
                    checker_error(c, node->loc.line,
                        "cannot return const slice as mutable — would allow writing to read-only memory");
                }
                /* BUG-281: volatile stripping on return */
                if (ret_type->kind == TYPE_POINTER && c->current_func_ret->kind == TYPE_POINTER &&
                    !c->current_func_ret->pointer.is_volatile) {
                    /* check both type-level and symbol-level volatile */
                    bool ret_volatile = ret_type->pointer.is_volatile;
                    if (!ret_volatile && node->ret.expr->kind == NODE_IDENT) {
                        Symbol *rs = scope_lookup(c->current_scope,
                            node->ret.expr->ident.name,
                            (uint32_t)node->ret.expr->ident.name_len);
                        if (rs && rs->is_volatile) ret_volatile = true;
                    }
                    if (ret_volatile) {
                        checker_error(c, node->loc.line,
                            "cannot return volatile pointer as non-volatile — "
                            "writes through result may be optimized away");
                    }
                }
            }

            /* scope escape: return local array as slice → dangling pointer
             * BUG-237: walk field/index chains to catch s.arr, s.inner.arr etc. */
            if (ret_type && ret_type->kind == TYPE_ARRAY &&
                c->current_func_ret && c->current_func_ret->kind == TYPE_SLICE) {
                Node *root = node->ret.expr;
                while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                    if (root->kind == NODE_FIELD) root = root->field.object;
                    else root = root->index_expr.object;
                }
                if (root && root->kind == NODE_IDENT) {
                    const char *vname = root->ident.name;
                    uint32_t vlen = (uint32_t)root->ident.name_len;
                    Symbol *sym = scope_lookup(c->current_scope, vname, vlen);
                    bool is_global = scope_lookup_local(c->global_scope, vname, vlen) != NULL;
                    if (sym && !sym->is_static && !is_global) {
                        checker_error(c, node->loc.line,
                            "cannot return local array as slice — "
                            "pointer will dangle after function returns");
                    }
                }
            }

            /* scope escape: return arena-derived pointer → dangling after stack unwind
             * Walk field/index chains to find root ident (BUG-155: h.ptr escape)
             * BUG-251: also walk through NODE_ORELSE to check both expr and fallback */
            {
                /* check up to 2 roots: the main expression, and orelse fallback if present */
                Node *roots[2] = { node->ret.expr, NULL };
                int root_count = 1;
                if (node->ret.expr->kind == NODE_ORELSE) {
                    roots[0] = node->ret.expr->orelse.expr;
                    if (node->ret.expr->orelse.fallback)
                        roots[root_count++] = node->ret.expr->orelse.fallback;
                }
                for (int ri = 0; ri < root_count; ri++) {
                    Node *root = roots[ri];
                    /* BUG-317: walk into @ptrcast/@bitcast in orelse fallback.
                     * Only when return type is pointer — value bitcasts are safe. */
                    if (root && root->kind == NODE_INTRINSIC &&
                        ret_type && ret_type->kind == TYPE_POINTER) {
                        const char *iname = root->intrinsic.name;
                        uint32_t ilen = (uint32_t)root->intrinsic.name_len;
                        /* BUG-351: @cast also needs escape check */
                        bool is_cast = (ilen == 7 && memcmp(iname, "ptrcast", 7) == 0) ||
                                       (ilen == 7 && memcmp(iname, "bitcast", 7) == 0) ||
                                       (ilen == 4 && memcmp(iname, "cast", 4) == 0);
                        if (is_cast && root->intrinsic.arg_count > 0)
                            root = root->intrinsic.args[root->intrinsic.arg_count - 1];
                    }
                    /* BUG-317: walk into &expr in orelse fallback */
                    if (root && root->kind == NODE_UNARY && root->unary.op == TOK_AMP) {
                        Node *inner = root->unary.operand;
                        while (inner && (inner->kind == NODE_FIELD || inner->kind == NODE_INDEX)) {
                            if (inner->kind == NODE_FIELD) inner = inner->field.object;
                            else inner = inner->index_expr.object;
                        }
                        if (inner && inner->kind == NODE_IDENT) {
                            Symbol *sym = scope_lookup(c->current_scope,
                                inner->ident.name, (uint32_t)inner->ident.name_len);
                            bool is_global = scope_lookup_local(c->global_scope,
                                inner->ident.name, (uint32_t)inner->ident.name_len) != NULL;
                            if (sym && !sym->is_static && !is_global) {
                                checker_error(c, node->loc.line,
                                    "cannot return pointer to local '%.*s' — "
                                    "stack memory is freed when function returns",
                                    (int)inner->ident.name_len, inner->ident.name);
                            }
                        }
                    }
                    while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                        if (root->kind == NODE_FIELD) root = root->field.object;
                        else root = root->index_expr.object;
                    }
                    if (root && root->kind == NODE_IDENT) {
                        Symbol *sym = scope_lookup(c->current_scope,
                            root->ident.name, (uint32_t)root->ident.name_len);
                        if (sym && sym->is_arena_derived) {
                            checker_error(c, node->loc.line,
                                "cannot return arena-derived pointer '%.*s' — "
                                "arena memory is freed when function returns",
                                (int)sym->name_len, sym->name);
                        }
                        if (sym && sym->is_local_derived) {
                            checker_error(c, node->loc.line,
                                "cannot return pointer to local '%.*s' — "
                                "stack memory is freed when function returns",
                                (int)sym->name_len, sym->name);
                        }
                    }
                }
            }

            /* scope escape: return &local or &local[i] or &local.field → error
             * Walk field/index chains from & operand to find root ident */
            if (node->ret.expr->kind == NODE_UNARY &&
                node->ret.expr->unary.op == TOK_AMP) {
                Node *root = node->ret.expr->unary.operand;
                while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                    if (root->kind == NODE_FIELD) root = root->field.object;
                    else root = root->index_expr.object;
                }
                if (root && root->kind == NODE_IDENT) {
                    const char *vname = root->ident.name;
                    uint32_t vlen = (uint32_t)root->ident.name_len;
                    Symbol *sym = scope_lookup(c->current_scope, vname, vlen);
                    bool is_global = scope_lookup_local(c->global_scope, vname, vlen) != NULL;
                    if (sym && !sym->is_static && !is_global) {
                        checker_error(c, node->loc.line,
                            "cannot return pointer to local variable '%.*s'",
                            (int)vlen, vname);
                    }
                }
            }

            /* BUG-246/256: scope escape via @ptrcast/@bitcast wrapping &local OR local-derived ident.
             * return @ptrcast(*u8, &x) where x is local → dangling pointer
             * return @ptrcast(*u8, p) where p is local-derived → dangling pointer */
            if (node->ret.expr->kind == NODE_INTRINSIC) {
                const char *iname = node->ret.expr->intrinsic.name;
                uint32_t ilen = (uint32_t)node->ret.expr->intrinsic.name_len;
                /* BUG-351: @cast also needs escape check */
                bool is_ptr_cast = (ilen == 7 && memcmp(iname, "ptrcast", 7) == 0) ||
                                   (ilen == 7 && memcmp(iname, "bitcast", 7) == 0) ||
                                   (ilen == 4 && memcmp(iname, "cast", 4) == 0);
                if (is_ptr_cast && node->ret.expr->intrinsic.arg_count > 0) {
                    Node *arg = node->ret.expr->intrinsic.args[
                        node->ret.expr->intrinsic.arg_count - 1];
                    /* check &local pattern */
                    if (arg->kind == NODE_UNARY && arg->unary.op == TOK_AMP) {
                        Node *root = arg->unary.operand;
                        while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                            if (root->kind == NODE_FIELD) root = root->field.object;
                            else root = root->index_expr.object;
                        }
                        if (root && root->kind == NODE_IDENT) {
                            Symbol *sym = scope_lookup(c->current_scope,
                                root->ident.name, (uint32_t)root->ident.name_len);
                            bool is_global = scope_lookup_local(c->global_scope,
                                root->ident.name, (uint32_t)root->ident.name_len) != NULL;
                            if (sym && !sym->is_static && !is_global) {
                                checker_error(c, node->loc.line,
                                    "cannot return pointer to local '%.*s' via @%.*s — "
                                    "stack memory is freed when function returns",
                                    (int)root->ident.name_len, root->ident.name,
                                    (int)ilen, iname);
                            }
                        }
                    }
                    /* BUG-256: check local/arena-derived ident through pointer cast.
                     * Only applies when result is a pointer type (not value bitcast). */
                    if (ret_type && ret_type->kind == TYPE_POINTER) {
                        Node *root = arg;
                        while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                            if (root->kind == NODE_FIELD) root = root->field.object;
                            else root = root->index_expr.object;
                        }
                        if (root && root->kind == NODE_IDENT) {
                            Symbol *sym = scope_lookup(c->current_scope,
                                root->ident.name, (uint32_t)root->ident.name_len);
                            if (sym && sym->is_local_derived) {
                                checker_error(c, node->loc.line,
                                    "cannot return local-derived pointer '%.*s' via @%.*s — "
                                    "stack memory is freed when function returns",
                                    (int)sym->name_len, sym->name,
                                    (int)ilen, iname);
                            }
                            if (sym && sym->is_arena_derived) {
                                checker_error(c, node->loc.line,
                                    "cannot return arena-derived pointer '%.*s' via @%.*s — "
                                    "arena memory is freed when function returns",
                                    (int)sym->name_len, sym->name,
                                    (int)ilen, iname);
                            }
                        }
                    }
                }
            }

            /* BUG-259: scope escape via @cstr — returns pointer to first arg (buffer).
             * return @cstr(local_buf, "hi") → dangling pointer to stack memory. */
            if (node->ret.expr->kind == NODE_INTRINSIC) {
                const char *iname = node->ret.expr->intrinsic.name;
                uint32_t ilen = (uint32_t)node->ret.expr->intrinsic.name_len;
                if (ilen == 4 && memcmp(iname, "cstr", 4) == 0 &&
                    node->ret.expr->intrinsic.arg_count > 0) {
                    Node *buf_arg = node->ret.expr->intrinsic.args[0];
                    Node *root = buf_arg;
                    while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                        if (root->kind == NODE_FIELD) root = root->field.object;
                        else root = root->index_expr.object;
                    }
                    if (root && root->kind == NODE_IDENT) {
                        Symbol *sym = scope_lookup(c->current_scope,
                            root->ident.name, (uint32_t)root->ident.name_len);
                        bool is_global = scope_lookup_local(c->global_scope,
                            root->ident.name, (uint32_t)root->ident.name_len) != NULL;
                        if (sym && !sym->is_static && !is_global) {
                            checker_error(c, node->loc.line,
                                "cannot return @cstr of local buffer '%.*s' — "
                                "stack memory is freed when function returns",
                                (int)sym->name_len, sym->name);
                        }
                    }
                }
            }

            /* BUG-360/374: return func(&local) — function call with local-derived
             * pointer arg returning a pointer type. Conservatively assume result
             * may be local-derived. BUG-374: recurse into nested calls —
             * return identity(identity(&x)) must also be caught. */
            if (node->ret.expr->kind == NODE_CALL &&
                ret_type && ret_type->kind == TYPE_POINTER) {
                if (call_has_local_derived_arg(c, node->ret.expr, 0)) {
                    checker_error(c, node->loc.line,
                        "cannot return result of call with local-derived pointer argument — "
                        "stack memory may escape through function return");
                }
            }

            /* scope escape: return opt orelse &local → fallback is local pointer */
            if (node->ret.expr->kind == NODE_ORELSE &&
                node->ret.expr->orelse.fallback) {
                Node *fb = node->ret.expr->orelse.fallback;
                if (fb->kind == NODE_UNARY && fb->unary.op == TOK_AMP) {
                    Node *root = fb->unary.operand;
                    while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                        if (root->kind == NODE_FIELD) root = root->field.object;
                        else root = root->index_expr.object;
                    }
                    if (root && root->kind == NODE_IDENT) {
                        const char *vname = root->ident.name;
                        uint32_t vlen = (uint32_t)root->ident.name_len;
                        bool is_global = scope_lookup_local(c->global_scope, vname, vlen) != NULL;
                        Symbol *sym = scope_lookup(c->current_scope, vname, vlen);
                        if (sym && !sym->is_static && !is_global) {
                            checker_error(c, node->loc.line,
                                "cannot return pointer to local variable '%.*s' via orelse fallback",
                                (int)vlen, vname);
                        }
                    }
                }
            }

            if (c->current_func_ret) {
                if (!type_equals(c->current_func_ret, ret_type) &&
                    !can_implicit_coerce(ret_type, c->current_func_ret) &&
                    !is_literal_compatible(node->ret.expr, c->current_func_ret)) {
                    checker_error(c, node->loc.line,
                        "return type '%s' doesn't match function return type '%s'",
                        type_name(ret_type), type_name(c->current_func_ret));
                }
            }
        } else {
            /* bare return — function must return void */
            if (c->current_func_ret && !type_equals(c->current_func_ret, ty_void)) {
                /* also OK for ?T functions (returning void = success for ?void) */
                if (!type_is_optional(c->current_func_ret)) {
                    checker_error(c, node->loc.line,
                        "function must return '%s', not void",
                        type_name(c->current_func_ret));
                }
            }
        }
        break;
    }

    case NODE_BREAK:
        if (c->defer_depth > 0) {
            checker_error(c, node->loc.line, "cannot use 'break' inside defer block");
        } else if (!c->in_loop) {
            checker_error(c, node->loc.line, "'break' outside of loop");
        }
        break;

    case NODE_CONTINUE:
        if (c->defer_depth > 0) {
            checker_error(c, node->loc.line, "cannot use 'continue' inside defer block");
        } else if (!c->in_loop) {
            checker_error(c, node->loc.line, "'continue' outside of loop");
        }
        break;

    case NODE_DEFER:
        c->defer_depth++;
        check_stmt(c, node->defer.body);
        c->defer_depth--;
        break;

    case NODE_EXPR_STMT:
        check_expr(c, node->expr_stmt.expr);
        break;

    case NODE_ASM:
        /* nothing to check — just assembly */
        break;

    default:
        break;
    }
}

/* ================================================================
 * TOP-LEVEL DECLARATION REGISTRATION (Pass 1)
 * ================================================================ */

static void register_decl(Checker *c, Node *node) {
    switch (node->kind) {
    case NODE_STRUCT_DECL: {
        /* create struct type — register BEFORE resolving fields
         * so self-referential structs work: struct Node { ?*Node next; } */
        Type *t = (Type *)arena_alloc(c->arena, sizeof(Type));
        t->kind = TYPE_STRUCT;
        t->struct_type.name = node->struct_decl.name;
        t->struct_type.name_len = (uint32_t)node->struct_decl.name_len;
        t->struct_type.is_packed = node->struct_decl.is_packed;
        t->struct_type.field_count = (uint32_t)node->struct_decl.field_count;
        t->struct_type.module_prefix = c->current_module;
        t->struct_type.module_prefix_len = c->current_module_len;

        add_symbol(c, node->struct_decl.name,
                   (uint32_t)node->struct_decl.name_len,
                   t, node->loc.line);
        typemap_set(c, node,t);

        /* now resolve field types (self-type is in scope) */
        if (node->struct_decl.field_count > 0) {
            t->struct_type.fields = (SField *)arena_alloc(c->arena,
                node->struct_decl.field_count * sizeof(SField));
            for (int i = 0; i < node->struct_decl.field_count; i++) {
                FieldDecl *fd = &node->struct_decl.fields[i];
                /* check for duplicate field names */
                for (int j = 0; j < i; j++) {
                    if (node->struct_decl.fields[j].name_len == fd->name_len &&
                        memcmp(node->struct_decl.fields[j].name, fd->name, fd->name_len) == 0) {
                        checker_error(c, node->loc.line,
                            "duplicate field '%.*s' in struct '%.*s'",
                            (int)fd->name_len, fd->name,
                            (int)node->struct_decl.name_len, node->struct_decl.name);
                    }
                }
                SField *sf = &t->struct_type.fields[i];
                sf->name = fd->name;
                sf->name_len = (uint32_t)fd->name_len;
                sf->type = resolve_type(c, fd->type);
                /* BUG-224: reject void fields */
                if (sf->type && sf->type->kind == TYPE_VOID) {
                    checker_error(c, node->loc.line,
                        "struct field '%.*s' cannot have type 'void'",
                        (int)fd->name_len, fd->name);
                }
                /* BUG-287: Pool/Ring/Slab as struct fields not yet supported */
                if (sf->type && (sf->type->kind == TYPE_POOL || sf->type->kind == TYPE_RING || sf->type->kind == TYPE_SLAB)) {
                    checker_error(c, node->loc.line,
                        "Pool/Ring/Slab cannot be struct fields — must be global or static variables");
                }
                /* BUG-227/232/314: reject recursive struct by value (incomplete type in C).
                 * Unwrap arrays AND distinct — S[1] or distinct S contains S by value too. */
                {
                    Type *inner = sf->type;
                    while (inner && inner->kind == TYPE_ARRAY) inner = inner->array.inner;
                    inner = type_unwrap_distinct(inner);
                    if (inner == t) {
                        checker_error(c, node->loc.line,
                            "struct '%.*s' cannot contain itself by value — use '*%.*s' (pointer) instead",
                            (int)node->struct_decl.name_len, node->struct_decl.name,
                            (int)node->struct_decl.name_len, node->struct_decl.name);
                    }
                }
                sf->is_keep = fd->is_keep;
            }
        }
        break;
    }

    case NODE_ENUM_DECL: {
        Type *t = (Type *)arena_alloc(c->arena, sizeof(Type));
        t->kind = TYPE_ENUM;
        t->enum_type.name = node->enum_decl.name;
        t->enum_type.name_len = (uint32_t)node->enum_decl.name_len;
        t->enum_type.variant_count = (uint32_t)node->enum_decl.variant_count;
        t->enum_type.module_prefix = c->current_module;
        t->enum_type.module_prefix_len = c->current_module_len;

        if (node->enum_decl.variant_count > 0) {
            t->enum_type.variants = (SEVariant *)arena_alloc(c->arena,
                node->enum_decl.variant_count * sizeof(SEVariant));
            int32_t next_val = 0;
            for (int i = 0; i < node->enum_decl.variant_count; i++) {
                EnumVariant *ev = &node->enum_decl.variants[i];
                /* BUG-198: check for duplicate variant names */
                for (int j = 0; j < i; j++) {
                    if (node->enum_decl.variants[j].name_len == ev->name_len &&
                        memcmp(node->enum_decl.variants[j].name, ev->name, ev->name_len) == 0) {
                        checker_error(c, node->loc.line,
                            "duplicate variant '%.*s' in enum '%.*s'",
                            (int)ev->name_len, ev->name,
                            (int)node->enum_decl.name_len, node->enum_decl.name);
                    }
                }
                SEVariant *sv = &t->enum_type.variants[i];
                sv->name = ev->name;
                sv->name_len = (uint32_t)ev->name_len;
                if (ev->value) {
                    /* explicit value — evaluate */
                    if (ev->value->kind == NODE_INT_LIT) {
                        sv->value = (int32_t)ev->value->int_lit.value;
                    } else if (ev->value->kind == NODE_UNARY &&
                               ev->value->unary.op == TOK_MINUS &&
                               ev->value->unary.operand->kind == NODE_INT_LIT) {
                        /* negative value: -N */
                        sv->value = -(int32_t)ev->value->unary.operand->int_lit.value;
                    }
                    next_val = sv->value + 1;
                } else {
                    sv->value = next_val++;
                }
            }
        }

        add_symbol(c, node->enum_decl.name,
                   (uint32_t)node->enum_decl.name_len,
                   t, node->loc.line);
        typemap_set(c, node,t);
        break;
    }

    case NODE_UNION_DECL: {
        Type *t = (Type *)arena_alloc(c->arena, sizeof(Type));
        t->kind = TYPE_UNION;
        t->union_type.name = node->union_decl.name;
        t->union_type.name_len = (uint32_t)node->union_decl.name_len;
        t->union_type.variant_count = (uint32_t)node->union_decl.variant_count;
        t->union_type.module_prefix = c->current_module;
        t->union_type.module_prefix_len = c->current_module_len;

        /* register before resolving variants (same as struct) */
        add_symbol(c, node->union_decl.name,
                   (uint32_t)node->union_decl.name_len,
                   t, node->loc.line);
        typemap_set(c, node,t);

        if (node->union_decl.variant_count > 0) {
            t->union_type.variants = (SUVariant *)arena_alloc(c->arena,
                node->union_decl.variant_count * sizeof(SUVariant));
            for (int i = 0; i < node->union_decl.variant_count; i++) {
                UnionVariant *uv = &node->union_decl.variants[i];
                /* check for duplicate variant names */
                for (int j = 0; j < i; j++) {
                    if (node->union_decl.variants[j].name_len == uv->name_len &&
                        memcmp(node->union_decl.variants[j].name, uv->name, uv->name_len) == 0) {
                        checker_error(c, node->loc.line,
                            "duplicate variant '%.*s' in union '%.*s'",
                            (int)uv->name_len, uv->name,
                            (int)node->union_decl.name_len, node->union_decl.name);
                    }
                }
                SUVariant *sv = &t->union_type.variants[i];
                sv->name = uv->name;
                sv->name_len = (uint32_t)uv->name_len;
                sv->type = resolve_type(c, uv->type);
                /* BUG-224: reject void union variants */
                if (sv->type && sv->type->kind == TYPE_VOID) {
                    checker_error(c, node->loc.line,
                        "union variant '%.*s' cannot have type 'void'",
                        (int)uv->name_len, uv->name);
                }
                /* BUG-265/314: reject recursive union by value (incomplete type in C) */
                {
                    Type *inner = sv->type;
                    while (inner && inner->kind == TYPE_ARRAY) inner = inner->array.inner;
                    inner = type_unwrap_distinct(inner);
                    if (inner == t) {
                        checker_error(c, node->loc.line,
                            "union '%.*s' cannot contain itself by value — use '*%.*s' (pointer) instead",
                            (int)node->union_decl.name_len, node->union_decl.name,
                            (int)node->union_decl.name_len, node->union_decl.name);
                    }
                }
            }
        }
        break;
    }

    case NODE_TYPEDEF: {
        Type *underlying = resolve_type(c, node->typedef_decl.type);
        Type *type;
        if (node->typedef_decl.is_distinct) {
            /* distinct typedef: new nominal type, NOT interchangeable */
            type = (Type *)arena_alloc(c->arena, sizeof(Type));
            type->kind = TYPE_DISTINCT;
            type->distinct.underlying = underlying;
            type->distinct.name = node->typedef_decl.name;
            type->distinct.name_len = (uint32_t)node->typedef_decl.name_len;
        } else {
            /* regular typedef: alias, fully interchangeable */
            type = underlying;
        }
        add_symbol(c, node->typedef_decl.name,
                   (uint32_t)node->typedef_decl.name_len,
                   type, node->loc.line);
        typemap_set(c, node,type);
        break;
    }

    case NODE_FUNC_DECL: {
        /* create function type and register */
        Type *ret = resolve_type(c, node->func_decl.return_type);
        uint32_t pc = (uint32_t)node->func_decl.param_count;
        Type **params = NULL;
        if (pc > 0) {
            params = (Type **)arena_alloc(c->arena, pc * sizeof(Type *));
            for (uint32_t i = 0; i < pc; i++) {
                params[i] = resolve_type(c, node->func_decl.params[i].type);
            }
        }
        Type *func_type = type_func_ptr(c->arena, params, pc, ret);
        /* carry keep flags from ParamDecl to Type */
        {
            bool any_keep = false;
            for (uint32_t i = 0; i < pc; i++) {
                if (node->func_decl.params[i].is_keep) { any_keep = true; break; }
            }
            if (any_keep) {
                func_type->func_ptr.param_keeps = (bool *)arena_alloc(c->arena, pc * sizeof(bool));
                for (uint32_t i = 0; i < pc; i++) {
                    func_type->func_ptr.param_keeps[i] = node->func_decl.params[i].is_keep;
                }
            }
        }

        /* check for forward declaration → definition pattern */
        Symbol *existing = scope_lookup_local(c->current_scope,
            node->func_decl.name, (uint32_t)node->func_decl.name_len);
        Symbol *sym;
        if (existing && existing->is_function &&
            existing->func_node && !existing->func_node->func_decl.body &&
            node->func_decl.body) {
            /* forward decl exists, now providing the body — update */
            sym = existing;
            sym->func_node = node;
            sym->type = func_type;
            sym->line = node->loc.line;
        } else {
            sym = add_symbol(c, node->func_decl.name,
                             (uint32_t)node->func_decl.name_len,
                             func_type, node->loc.line);
        }
        if (sym) {
            sym->is_function = true;
            sym->is_static = node->func_decl.is_static;
            sym->is_comptime = node->func_decl.is_comptime;
            sym->func_node = node;
            /* BUG-218: store module prefix for function name mangling */
            sym->module_prefix = c->current_module;
            sym->module_prefix_len = c->current_module_len;
        }
        typemap_set(c, node,func_type);
        break;
    }

    case NODE_GLOBAL_VAR: {
        Type *type = resolve_type(c, node->var_decl.type);
        /* propagate const from var qualifier to slice/pointer type */
        if (node->var_decl.is_const && type) {
            if (type->kind == TYPE_SLICE && !type->slice.is_const) {
                type = type_const_slice(c->arena, type->slice.inner);
            } else if (type->kind == TYPE_POINTER && !type->pointer.is_const) {
                type = type_const_pointer(c->arena, type->pointer.inner);
            }
        }
        /* BUG-253: non-null pointer (*T) requires initializer at global scope too */
        if (!node->var_decl.init && type && type->kind == TYPE_POINTER) {
            checker_error(c, node->loc.line,
                "non-null pointer '*%s' requires an initializer — "
                "use '?*%s' for nullable pointers",
                type_name(type->pointer.inner), type_name(type->pointer.inner));
        }
        Symbol *sym = add_symbol(c, node->var_decl.name,
                                 (uint32_t)node->var_decl.name_len,
                                 type, node->loc.line);
        if (sym) {
            sym->is_const = node->var_decl.is_const;
            sym->is_volatile = node->var_decl.is_volatile;
            sym->is_static = node->var_decl.is_static;
            /* BUG-218/222: store module prefix for name mangling (including static) */
            sym->module_prefix = c->current_module;
            sym->module_prefix_len = c->current_module_len;
        }
        typemap_set(c, node,type);
        break;
    }

    case NODE_IMPORT:
        /* TODO: module resolution */
        break;

    case NODE_CINCLUDE:
        /* C header include — handled by emitter, nothing to check */
        break;

    case NODE_INTERRUPT:
        /* register as a void function */
        /* body checked in pass 2 */
        break;

    case NODE_MMIO: {
        /* register mmio address range for @inttoptr validation */
        if (c->mmio_range_count >= c->mmio_range_capacity) {
            int new_cap = c->mmio_range_capacity < 16 ? 16 : c->mmio_range_capacity * 2;
            uint64_t (*new_arr)[2] = (uint64_t (*)[2])arena_alloc(c->arena, new_cap * sizeof(uint64_t[2]));
            if (c->mmio_ranges && c->mmio_range_count > 0)
                memcpy(new_arr, c->mmio_ranges, c->mmio_range_count * sizeof(uint64_t[2]));
            c->mmio_ranges = new_arr;
            c->mmio_range_capacity = new_cap;
        }
        c->mmio_ranges[c->mmio_range_count][0] = node->mmio_decl.range_start;
        c->mmio_ranges[c->mmio_range_count][1] = node->mmio_decl.range_end;
        c->mmio_range_count++;
        if (node->mmio_decl.range_start > node->mmio_decl.range_end) {
            checker_error(c, node->loc.line,
                "mmio range start (0x%llx) must be <= end (0x%llx)",
                (unsigned long long)node->mmio_decl.range_start,
                (unsigned long long)node->mmio_decl.range_end);
        }
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 * TOP-LEVEL FUNCTION BODY CHECKING (Pass 2)
 * ================================================================ */

/* check if all control flow paths end in a return statement */
/* BUG-200: check if a node contains a break that targets the current loop.
 * Stops recursing into nested loops (their breaks target the inner loop). */
static bool contains_break(Node *node) {
    if (!node) return false;
    switch (node->kind) {
    case NODE_BREAK: return true;
    case NODE_WHILE: case NODE_FOR: return false; /* nested loop — break targets it */
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++) {
            if (contains_break(node->block.stmts[i])) return true;
        }
        return false;
    case NODE_IF:
        return contains_break(node->if_stmt.then_body) ||
               contains_break(node->if_stmt.else_body);
    case NODE_SWITCH:
        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            if (contains_break(node->switch_stmt.arms[i].body)) return true;
        }
        return false;
    /* BUG-204: orelse break hidden in expressions */
    case NODE_ORELSE:
        return node->orelse.fallback_is_break;
    case NODE_VAR_DECL:
        return node->var_decl.init ? contains_break(node->var_decl.init) : false;
    case NODE_EXPR_STMT:
        return node->expr_stmt.expr ? contains_break(node->expr_stmt.expr) : false;
    default: return false;
    }
}

static bool all_paths_return(Node *node) {
    if (!node) return false;
    switch (node->kind) {
    case NODE_RETURN:
        return true;
    case NODE_BLOCK:
        /* check if any statement in the block guarantees a return */
        for (int i = node->block.stmt_count - 1; i >= 0; i--) {
            if (all_paths_return(node->block.stmts[i])) return true;
            /* if this statement is not a return but is a block/if/switch,
             * it might still cover all paths */
        }
        return false;
    case NODE_IF:
        /* BUG-354: comptime if — only the taken branch matters */
        if (node->if_stmt.is_comptime) {
            int64_t cval = eval_const_expr(node->if_stmt.cond);
            if (cval && cval != CONST_EVAL_FAIL)
                return node->if_stmt.then_body && all_paths_return(node->if_stmt.then_body);
            else
                return node->if_stmt.else_body && all_paths_return(node->if_stmt.else_body);
        }
        /* both branches must return; else is required */
        if (!node->if_stmt.else_body) return false;
        return all_paths_return(node->if_stmt.then_body) &&
               all_paths_return(node->if_stmt.else_body);
    case NODE_SWITCH: {
        /* all arms must return, and switch must be exhaustive (default or all variants) */
        bool has_default = false;
        int arm_count = 0;
        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            if (node->switch_stmt.arms[i].is_default) has_default = true;
            if (!all_paths_return(node->switch_stmt.arms[i].body)) return false;
            arm_count++;
        }
        /* exhaustive if has default OR has at least one arm (enum/bool switches
         * are checked for exhaustiveness elsewhere — if we got here, they passed) */
        return has_default || arm_count > 0;
    }
    case NODE_WHILE:
        /* while(true) is an infinite loop — never exits normally.
         * BUT: if the body contains a break, the loop CAN exit,
         * so it's NOT a terminator. (BUG-200) */
        if (node->while_stmt.cond &&
            node->while_stmt.cond->kind == NODE_BOOL_LIT &&
            node->while_stmt.cond->bool_lit.value &&
            !contains_break(node->while_stmt.body)) {
            return true;
        }
        return false;
    case NODE_FOR:
        /* for (;;) { ... } — infinite loop with no condition.
         * Same break check as while(true). (BUG-200) */
        if (!node->for_stmt.cond && !contains_break(node->for_stmt.body)) {
            return true;
        }
        return false;
    case NODE_EXPR_STMT:
        /* orelse return/break in expression statement — check for trap calls etc */
        return false;
    default:
        return false;
    }
}

static void check_func_body(Checker *c, Node *node) {
    if (node->kind == NODE_FUNC_DECL && node->func_decl.body) {
        /* resolve return type */
        Type *ret = resolve_type(c, node->func_decl.return_type);
        /* BUG-270/315: reject array return types — C forbids returning arrays.
         * Unwrap distinct — distinct typedef u8[10] Buffer is still an array. */
        { Type *ret_base = type_unwrap_distinct(ret);
        if (ret_base && ret_base->kind == TYPE_ARRAY) {
            checker_error(c, node->loc.line,
                "cannot return array type — use a struct wrapper or slice instead");
        }
        }
        c->current_func_ret = ret;

        /* create function scope with parameters */
        push_scope(c);
        for (int i = 0; i < node->func_decl.param_count; i++) {
            ParamDecl *p = &node->func_decl.params[i];
            Type *ptype = resolve_type(c, p->type);
            Symbol *sym = add_symbol(c, p->name, (uint32_t)p->name_len,
                                     ptype, p->loc.line);
            if (sym) {
                sym->is_keep = p->is_keep;
            }
        }

        check_stmt(c, node->func_decl.body);

        /* check that all paths return for non-void functions */
        if (ret && ret->kind != TYPE_VOID &&
            !all_paths_return(node->func_decl.body)) {
            checker_error(c, node->loc.line,
                "not all control flow paths return a value in function '%.*s'",
                (int)node->func_decl.name_len, node->func_decl.name);
        }

        pop_scope(c);
        c->current_func_ret = NULL;
    }

    if (node->kind == NODE_INTERRUPT && node->interrupt.body) {
        c->current_func_ret = ty_void;
        push_scope(c);
        check_stmt(c, node->interrupt.body);
        pop_scope(c);
        c->current_func_ret = NULL;
    }
}

/* ================================================================
 * ENTRY POINT
 * ================================================================ */

void checker_init(Checker *c, Arena *arena, const char *file_name) {
    memset(c, 0, sizeof(Checker));
    c->arena = arena;
    c->file_name = file_name;
    types_init(arena);
    c->global_scope = scope_new(arena, NULL);
    c->current_scope = c->global_scope;

    /* init dynamic type map */
    typemap_init(c);

    /* init non-storable tracking */
    non_storable_init(c);

    /* Arena type available in expression context for Arena.over() */
    scope_add(arena, c->global_scope, "Arena", 5, ty_arena, 0, file_name);
}

Type *checker_get_type(Checker *c, Node *node) {
    return typemap_get(c, node);
}

void checker_register_file(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        /* skip imports — they're handled by the compiler driver */
        if (decl->kind == NODE_IMPORT || decl->kind == NODE_CINCLUDE) continue;
        /* BUG-213/222: skip statics for imported modules (registered in module scope).
         * Main module (current_module == NULL) registers statics in global scope. */
        if (c->current_module) {
            if (decl->kind == NODE_FUNC_DECL && decl->func_decl.is_static) continue;
            if (decl->kind == NODE_GLOBAL_VAR && decl->var_decl.is_static) continue;
        }
        register_decl(c, decl);

        /* BUG-233: also register imported non-static functions/globals under MANGLED key.
         * Without this, two modules with same-named functions (e.g., both have init())
         * collide under raw key — the emitter inside module B's body resolves to module A's
         * symbol. Mangled key = "module_name", used by emitter's BUG-229 fallback. */
        if (c->current_module &&
            (decl->kind == NODE_FUNC_DECL || decl->kind == NODE_GLOBAL_VAR)) {
            Type *dt = typemap_get(c, decl);
            if (dt) {
                const char *dname = (decl->kind == NODE_FUNC_DECL) ?
                    decl->func_decl.name : decl->var_decl.name;
                uint32_t dname_len = (uint32_t)((decl->kind == NODE_FUNC_DECL) ?
                    decl->func_decl.name_len : decl->var_decl.name_len);
                /* RF4: use arena-allocated buffer for arbitrary length mangled names
                 * BUG-332: use double underscore __ separator to avoid collisions */
                uint32_t mkl = c->current_module_len + 2 + dname_len;
                char *mk_copy = (char *)arena_alloc(c->arena, mkl + 1);
                memcpy(mk_copy, c->current_module, c->current_module_len);
                mk_copy[c->current_module_len] = '_';
                mk_copy[c->current_module_len + 1] = '_';
                memcpy(mk_copy + c->current_module_len + 2, dname, dname_len);
                mk_copy[mkl] = '\0';
                Symbol *ms = scope_add(c->arena, c->global_scope,
                    mk_copy, mkl, dt, decl->loc.line, c->file_name);
                if (ms) {
                    ms->module_prefix = c->current_module;
                    ms->module_prefix_len = c->current_module_len;
                    if (decl->kind == NODE_FUNC_DECL) {
                        ms->is_function = true;
                        ms->func_node = decl;
                    }
                }
            }
        }
    }
}

/* push a module-local scope with the module's own type declarations.
 * This allows same-named types in different modules — each module sees its own. */
void checker_push_module_scope(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;
    push_scope(c);
    /* re-register this module's struct/union/enum types into the local scope.
     * This overrides any same-named types from other modules in the global scope. */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_STRUCT_DECL || decl->kind == NODE_ENUM_DECL ||
            decl->kind == NODE_UNION_DECL) {
            Type *t = typemap_get(c, decl);
            if (t) {
                const char *name = NULL;
                uint32_t name_len = 0;
                if (decl->kind == NODE_STRUCT_DECL) {
                    name = decl->struct_decl.name;
                    name_len = (uint32_t)decl->struct_decl.name_len;
                } else if (decl->kind == NODE_ENUM_DECL) {
                    name = decl->enum_decl.name;
                    name_len = (uint32_t)decl->enum_decl.name_len;
                } else {
                    name = decl->union_decl.name;
                    name_len = (uint32_t)decl->union_decl.name_len;
                }
                scope_add(c->arena, c->current_scope, name, name_len,
                          t, decl->loc.line, c->file_name);
            }
        }
        /* BUG-222: register static functions and globals into module scope
         * AND into global scope (with module prefix for emitter lookup).
         * Module scope is for checker body-check; global scope is for emitter. */
        if ((decl->kind == NODE_FUNC_DECL && decl->func_decl.is_static) ||
            (decl->kind == NODE_GLOBAL_VAR && decl->var_decl.is_static)) {
            /* register into module scope (for checker body-check) */
            register_decl(c, decl);
            /* BUG-229: register into global scope with MANGLED key for emitter.
             * Uses "module_name" as key to avoid collision between
             * mod_a's static x and mod_b's static x. */
            Type *st = typemap_get(c, decl);
            if (st && c->current_module) {
                const char *sname = (decl->kind == NODE_FUNC_DECL) ?
                    decl->func_decl.name : decl->var_decl.name;
                uint32_t sname_len = (uint32_t)((decl->kind == NODE_FUNC_DECL) ?
                    decl->func_decl.name_len : decl->var_decl.name_len);
                /* RF4: arena-allocated mangled key — no fixed buffer limit
                 * BUG-332: double underscore __ separator */
                uint32_t mk_len = c->current_module_len + 2 + sname_len;
                char *mk_copy = (char *)arena_alloc(c->arena, mk_len + 1);
                memcpy(mk_copy, c->current_module, c->current_module_len);
                mk_copy[c->current_module_len] = '_';
                mk_copy[c->current_module_len + 1] = '_';
                memcpy(mk_copy + c->current_module_len + 2, sname, sname_len);
                mk_copy[mk_len] = '\0';
                /* register under MANGLED name — no collision possible */
                Symbol *gs = scope_add(c->arena, c->global_scope,
                    mk_copy, (uint32_t)mk_len, st, decl->loc.line, c->file_name);
                if (gs) {
                    gs->module_prefix = c->current_module;
                    gs->module_prefix_len = c->current_module_len;
                    gs->is_static = true;
                    if (decl->kind == NODE_FUNC_DECL) {
                        gs->is_function = true;
                        gs->func_node = decl;
                    }
                }
            }
        }
    }
}

void checker_pop_module_scope(Checker *c) {
    pop_scope(c);
}

/* check function bodies only (declarations already registered) */
bool checker_check_bodies(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return false;
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        check_func_body(c, decl);
        if (decl->kind == NODE_GLOBAL_VAR && decl->var_decl.init) {
            Type *type = resolve_type(c, decl->var_decl.type);
            Type *init = check_expr(c, decl->var_decl.init);
            /* global initializers must be constant expressions in C */
            Node *ginit = decl->var_decl.init;
            if (ginit->kind == NODE_CALL) {
                checker_error(c, decl->loc.line,
                    "global variable '%.*s' initializer must be a constant expression — "
                    "cannot call functions at global scope",
                    (int)decl->var_decl.name_len, decl->var_decl.name);
            }
            /* global array init from variable — invalid C (arrays can't be init'd from variables) */
            if (type && type->kind == TYPE_ARRAY && ginit->kind == NODE_IDENT) {
                checker_error(c, decl->loc.line,
                    "cannot initialize global array '%.*s' from variable — "
                    "global arrays must use literal initializers",
                    (int)decl->var_decl.name_len, decl->var_decl.name);
            }
            if (!type_equals(type, init) &&
                !can_implicit_coerce(init, type) &&
                !is_literal_compatible(decl->var_decl.init, type)) {
                checker_error(c, decl->loc.line,
                    "cannot initialize '%.*s' of type '%s' with '%s'",
                    (int)decl->var_decl.name_len, decl->var_decl.name,
                    type_name(type), type_name(init));
            }
            /* BUG-373: integer literal range check for globals */
            if (decl->var_decl.init->kind == NODE_INT_LIT &&
                type && type_is_integer(type_unwrap_distinct(type))) {
                if (!is_literal_compatible(decl->var_decl.init, type)) {
                    checker_error(c, decl->loc.line,
                        "integer literal %llu does not fit in '%s'",
                        (unsigned long long)decl->var_decl.init->int_lit.value,
                        type_name(type));
                }
            }
        }
    }
    return c->error_count == 0;
}

bool checker_check(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return false;

    /* Pass 1: register all top-level declarations */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        register_decl(c, file_node->file.decls[i]);
    }

    /* Pass 2: type-check all function bodies and global initializers */
    return checker_check_bodies(c, file_node);
}
