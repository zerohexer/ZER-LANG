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
 * We track this with a dynamic array in the arena — no fixed cap. */

static Node **non_storable_nodes = NULL;
static int non_storable_count = 0;
static int non_storable_capacity = 0;
static Arena *non_storable_arena = NULL;

static void non_storable_init(Arena *a) {
    non_storable_arena = a;
    non_storable_capacity = 64;
    non_storable_nodes = (Node **)arena_alloc(a, non_storable_capacity * sizeof(Node *));
    non_storable_count = 0;
}

static void mark_non_storable(Node *n) {
    if (non_storable_count >= non_storable_capacity) {
        int new_cap = non_storable_capacity * 2;
        Node **new_arr = (Node **)arena_alloc(non_storable_arena, new_cap * sizeof(Node *));
        memcpy(new_arr, non_storable_nodes, non_storable_count * sizeof(Node *));
        non_storable_nodes = new_arr;
        non_storable_capacity = new_cap;
    }
    non_storable_nodes[non_storable_count++] = n;
}

static bool is_non_storable(Node *n) {
    for (int i = 0; i < non_storable_count; i++) {
        if (non_storable_nodes[i] == n) return true;
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

typedef struct {
    Node *key;
    Type *type;
} TypeMapEntry;

static TypeMapEntry *type_map = NULL;
static uint32_t type_map_size = 0;
static uint32_t type_map_count = 0;
static Arena *type_map_arena = NULL;

#define TYPE_MAP_INIT_SIZE 4096

static void typemap_init(Arena *a) {
    type_map_arena = a;
    type_map_size = TYPE_MAP_INIT_SIZE;
    type_map = (TypeMapEntry *)arena_alloc(a, type_map_size * sizeof(TypeMapEntry));
    type_map_count = 0;
}

static void typemap_grow(void) {
    uint32_t old_size = type_map_size;
    TypeMapEntry *old_map = type_map;
    type_map_size = old_size * 2;
    type_map = (TypeMapEntry *)arena_alloc(type_map_arena,
        type_map_size * sizeof(TypeMapEntry));
    type_map_count = 0;
    /* rehash all existing entries */
    for (uint32_t i = 0; i < old_size; i++) {
        if (old_map[i].key) {
            uint32_t idx = ((uintptr_t)old_map[i].key >> 3) % type_map_size;
            for (uint32_t j = 0; j < type_map_size; j++) {
                uint32_t slot = (idx + j) % type_map_size;
                if (!type_map[slot].key) {
                    type_map[slot] = old_map[i];
                    type_map_count++;
                    break;
                }
            }
        }
    }
}

static void typemap_set(Node *node, Type *type) {
    /* grow at 70% load to keep collisions low */
    if (type_map_count * 10 > type_map_size * 7) {
        typemap_grow();
    }
    uint32_t idx = ((uintptr_t)node >> 3) % type_map_size;
    for (uint32_t i = 0; i < type_map_size; i++) {
        uint32_t slot = (idx + i) % type_map_size;
        if (!type_map[slot].key) {
            type_map[slot].key = node;
            type_map[slot].type = type;
            type_map_count++;
            return;
        }
        if (type_map[slot].key == node) {
            type_map[slot].type = type;
            return;
        }
    }
}

static Type *typemap_get(Node *node) {
    if (!type_map) return NULL;
    uint32_t idx = ((uintptr_t)node >> 3) % type_map_size;
    for (uint32_t i = 0; i < type_map_size; i++) {
        uint32_t slot = (idx + i) % type_map_size;
        if (type_map[slot].key == node) return type_map[slot].type;
        if (!type_map[slot].key) return NULL;
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
        case TYPE_USIZE: return val <= 0xFFFFFFFFULL; /* 32-bit */
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

/* ================================================================
 * TYPE RESOLUTION: TypeNode (syntactic) → Type (semantic)
 * ================================================================ */

static Type *resolve_type(Checker *c, TypeNode *tn) {
    if (!tn) return ty_void;

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
        if (tn->array.size_expr) {
            int64_t val = eval_const_expr(tn->array.size_expr);
            /* BUG-199: handle @size(T) as compile-time constant */
            if (val < 0 && tn->array.size_expr->kind == NODE_INTRINSIC &&
                tn->array.size_expr->intrinsic.name_len == 4 &&
                memcmp(tn->array.size_expr->intrinsic.name, "size", 4) == 0) {
                Type *size_of = NULL;
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
                        /* sum of field sizes (no padding — packed-like) */
                        int64_t total = 0;
                        for (uint32_t fi = 0; fi < unwrapped->struct_type.field_count; fi++) {
                            int fw = type_width(unwrapped->struct_type.fields[fi].type);
                            total += (fw > 0) ? fw / 8 : 4; /* default 4 for pointers etc */
                        }
                        val = total > 0 ? total : -1;
                    } else if (size_of->kind == TYPE_POINTER ||
                               size_of->kind == TYPE_SLICE) {
                        /* pointer/slice size = target pointer width */
                        val = type_width(ty_usize) / 8;
                        if (size_of->kind == TYPE_SLICE) val *= 2; /* ptr + len */
                    }
                }
            }
            if (val > 0) {
                size = (uint32_t)val;
            } else if (val == 0) {
                checker_error(c, tn->loc.line, "array size must be > 0");
            } else {
                checker_error(c, tn->loc.line, "array size must be a compile-time constant");
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
        return type_func_ptr(c->arena, params, pc, ret);
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
        /* propagate volatile to pointer type for codegen */
        if (inner && inner->kind == TYPE_POINTER) {
            Type *vp = type_pointer(c->arena, inner->pointer.inner);
            vp->pointer.is_volatile = true;
            return vp;
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

static Type *check_expr(Checker *c, Node *node) {
    if (!node) return ty_void;

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
                /* compile-time division by zero check */
                if ((node->binary.op == TOK_SLASH || node->binary.op == TOK_PERCENT) &&
                    node->binary.right->kind == NODE_INT_LIT &&
                    node->binary.right->int_lit.value == 0) {
                    checker_error(c, node->loc.line, "division by zero");
                }
                result = common_numeric_type(c, left, right, node->loc.line);
            }
            break;

        /* comparison: both same type (or coercible), result = bool */
        case TOK_EQEQ: case TOK_BANGEQ:
        case TOK_LT: case TOK_GT: case TOK_LTEQ: case TOK_GTEQ:
            /* reject slice/array comparison — C compares struct/pointer, not content */
            if ((node->binary.op == TOK_EQEQ || node->binary.op == TOK_BANGEQ) &&
                ((left->kind == TYPE_SLICE || left->kind == TYPE_ARRAY) ||
                 (right->kind == TYPE_SLICE || right->kind == TYPE_ARRAY))) {
                checker_error(c, node->loc.line,
                    "cannot compare '%s' with == — use element-wise comparison",
                    type_name(left->kind == TYPE_SLICE || left->kind == TYPE_ARRAY ? left : right));
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
            /* BUG-197: volatile propagation — &volatile_var yields volatile pointer */
            if (node->unary.operand->kind == NODE_IDENT) {
                Symbol *sym = scope_lookup(c->current_scope,
                    node->unary.operand->ident.name,
                    (uint32_t)node->unary.operand->ident.name_len);
                if (sym && sym->is_volatile) {
                    result->pointer.is_volatile = true;
                }
                /* BUG-208: block &union_var inside mutable capture arm.
                 * Pointer alias would bypass the union switch lock. */
                if (c->union_switch_var &&
                    node->unary.operand->ident.name_len == c->union_switch_var_len &&
                    memcmp(node->unary.operand->ident.name, c->union_switch_var,
                           c->union_switch_var_len) == 0) {
                    checker_error(c, node->loc.line,
                        "cannot take address of union '%.*s' inside its switch arm — "
                        "pointer alias would bypass variant lock",
                        (int)c->union_switch_var_len, c->union_switch_var);
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
                    Type *ptr_type = checker_get_type(root->unary.operand);
                    if (ptr_type && ptr_type->kind == TYPE_POINTER && ptr_type->pointer.is_const) {
                        through_const_pointer = true;
                    }
                    through_pointer = true;
                    root = root->unary.operand;
                } else if (root->kind == NODE_FIELD) {
                    /* check if object is a pointer (auto-deref) */
                    Type *obj_type = checker_get_type(root->field.object);
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
        if (node->assign.op == TOK_EQ && is_non_storable(node->assign.value)) {
            checker_error(c, node->loc.line,
                "cannot store result of get() — use inline");
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
            /* Walk target chain (field/index) to find root identifier */
            Node *root = node->assign.target;
            while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                if (root->kind == NODE_FIELD) root = root->field.object;
                else root = root->index_expr.object;
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
                if ((target_is_static || target_is_global) && val_sym &&
                    !val_sym->is_static && !val_is_global) {
                    checker_error(c, node->loc.line,
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
                    /* check if new value is &local */
                    if (node->assign.value->kind == NODE_UNARY &&
                        node->assign.value->unary.op == TOK_AMP &&
                        node->assign.value->unary.operand->kind == NODE_IDENT) {
                        Symbol *src = scope_lookup(c->current_scope,
                            node->assign.value->unary.operand->ident.name,
                            (uint32_t)node->assign.value->unary.operand->ident.name_len);
                        bool src_is_global = src && scope_lookup_local(c->global_scope,
                            src->name, src->name_len) != NULL;
                        if (src && !src->is_static && !src_is_global) {
                            tsym->is_local_derived = true;
                        }
                    }
                    /* check if new value is an alias of local/arena-derived */
                    if (node->assign.value->kind == NODE_IDENT) {
                        Symbol *src = scope_lookup(c->current_scope,
                            node->assign.value->ident.name,
                            (uint32_t)node->assign.value->ident.name_len);
                        if (src && src->is_local_derived) tsym->is_local_derived = true;
                        if (src && src->is_arena_derived) tsym->is_arena_derived = true;
                    }
                }
            }
        }

        /* BUG-205: local-derived escape via assignment to global/static.
         * After flag propagation, check if target is global/static with local-derived value. */
        if (node->assign.op == TOK_EQ && node->assign.value->kind == NODE_IDENT) {
            Symbol *val_sym = scope_lookup(c->current_scope,
                node->assign.value->ident.name,
                (uint32_t)node->assign.value->ident.name_len);
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
                    Type *obj_type = checker_get_type(obj);
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

        /* const laundering: reject const → mutable assignment for ptr/slice */
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

            /* Pool methods */
            if (obj->kind == TYPE_POOL) {
                if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "pool.alloc() takes no arguments");
                    result = type_optional(c->arena, type_handle(c->arena, obj->pool.elem));
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 3 && memcmp(mname, "get", 3) == 0) {
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "pool.get() takes exactly 1 argument");
                    result = type_pointer(c->arena, obj->pool.elem);
                    typemap_set(field_node, result);
                    mark_non_storable(node); /* Rule 2: get() result non-storable */
                    break;
                }
                if (mlen == 4 && memcmp(mname, "free", 4) == 0) {
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "pool.free() takes exactly 1 argument");
                    result = ty_void;
                    typemap_set(field_node, result);
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
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "ring.push() takes exactly 1 argument");
                    result = ty_void;
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 12 && memcmp(mname, "push_checked", 12) == 0) {
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "ring.push_checked() takes exactly 1 argument");
                    result = type_optional(c->arena, ty_void);
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 3 && memcmp(mname, "pop", 3) == 0) {
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "ring.pop() takes no arguments");
                    result = type_optional(c->arena, obj->ring.elem);
                    typemap_set(field_node, result);
                    break;
                }
                checker_error(c, node->loc.line,
                    "Ring has no method '%.*s' (available: push, push_checked, pop)",
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
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                    /* Arena.alloc(T) → ?*T */
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
                    typemap_set(field_node, result);
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
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 11 && memcmp(mname, "alloc_slice", 11) == 0) {
                    /* Arena.alloc_slice(T, n) → ?[]T */
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
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 12 && memcmp(mname, "unsafe_reset", 12) == 0) {
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "arena.unsafe_reset() takes no arguments");
                    result = ty_void;
                    typemap_set(field_node, result);
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
                     * (string data is in .rodata, writing segfaults) */
                    if (node->call.args[i]->kind == NODE_STRING_LIT &&
                        param && param->kind == TYPE_SLICE) {
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

                    if (!type_equals(param, arg) &&
                        !can_implicit_coerce(arg, param) &&
                        !is_literal_compatible(node->call.args[i], param)) {
                        checker_error(c, node->loc.line,
                            "argument %u: expected '%s', got '%s'",
                            i + 1, type_name(param), type_name(arg));
                    }
                }

                /* keep parameter validation: check call arguments */
                if (node->call.callee->kind == NODE_IDENT) {
                    Symbol *func_sym = scope_lookup(c->current_scope,
                        node->call.callee->ident.name,
                        (uint32_t)node->call.callee->ident.name_len);
                    if (func_sym && func_sym->func_node &&
                        func_sym->func_node->kind == NODE_FUNC_DECL) {
                        Node *fdecl = func_sym->func_node;
                        for (int i = 0; i < fdecl->func_decl.param_count &&
                             i < node->call.arg_count; i++) {
                            if (fdecl->func_decl.params[i].is_keep) {
                                /* keep param: arg must be static/global, not local */
                                Node *arg_node = node->call.args[i];
                                if (arg_node->kind == NODE_UNARY &&
                                    arg_node->unary.op == TOK_AMP &&
                                    arg_node->unary.operand->kind == NODE_IDENT) {
                                    Symbol *arg_sym = scope_lookup(c->current_scope,
                                        arg_node->unary.operand->ident.name,
                                        (uint32_t)arg_node->unary.operand->ident.name_len);
                                    if (arg_sym && !arg_sym->is_static) {
                                        checker_error(c, node->loc.line,
                                            "argument %d: local variable '%.*s' cannot "
                                            "satisfy 'keep' parameter — must be static or global",
                                            i + 1,
                                            (int)arg_sym->name_len, arg_sym->name);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            result = effective_callee->func_ptr.ret;
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
            /* union switch lock applies to pointer auto-deref too */
            if (c->in_assign_target && c->union_switch_var &&
                node->field.object->kind == NODE_IDENT &&
                node->field.object->ident.name_len == c->union_switch_var_len &&
                memcmp(node->field.object->ident.name, c->union_switch_var,
                       c->union_switch_var_len) == 0) {
                checker_error(c, node->loc.line,
                    "cannot mutate union '%.*s' inside its own switch arm — "
                    "active capture would become invalid",
                    (int)c->union_switch_var_len, c->union_switch_var);
                result = ty_void;
                break;
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
            /* prevent mutating union variant while inside a switch arm on same variable */
            if (c->union_switch_var && node->field.object->kind == NODE_IDENT &&
                node->field.object->ident.name_len == c->union_switch_var_len &&
                memcmp(node->field.object->ident.name, c->union_switch_var,
                       c->union_switch_var_len) == 0) {
                checker_error(c, node->loc.line,
                    "cannot mutate union '%.*s' inside its own switch arm — "
                    "active capture would become invalid",
                    (int)c->union_switch_var_len, c->union_switch_var);
                result = ty_void;
                break;
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
            if (start_val >= 0 && end_val >= 0 && start_val > end_val) {
                checker_error(c, node->loc.line,
                    "slice start (%lld) is greater than end (%lld)",
                    (long long)start_val, (long long)end_val);
            }
        }

        if (obj->kind == TYPE_ARRAY) {
            result = type_slice(c->arena, obj->array.inner);
        } else if (obj->kind == TYPE_SLICE) {
            result = obj; /* slice of slice = same slice type */
        } else if (type_is_integer(obj)) {
            /* bit extraction: reg[high..low] → integer result */
            /* validate constant indices are within type width */
            if (node->slice.start) {
                int64_t hi = eval_const_expr(node->slice.start);
                if (hi >= 0 && hi >= type_width(obj)) {
                    checker_error(c, node->loc.line,
                        "bit index %lld out of range for %d-bit type '%s'",
                        (long long)hi, type_width(obj), type_name(obj));
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
            /* @ptrcast(*T, expr) → *T — source must be a pointer */
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = typemap_get(node->intrinsic.args[0]);
                    if (val_type) {
                        Type *eff = type_unwrap_distinct(val_type);
                        if (eff->kind != TYPE_POINTER && eff->kind != TYPE_FUNC_PTR) {
                            checker_error(c, node->loc.line,
                                "@ptrcast source must be a pointer, got '%s'",
                                type_name(val_type));
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
                    if (tw > 0 && vw > 0 && tw != vw) {
                        checker_error(c, node->loc.line,
                            "@bitcast requires same-width types (target %d bits, source %d bits)", tw, vw);
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
            /* @inttoptr(*T, addr) — addr must be an integer */
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = typemap_get(node->intrinsic.args[0]);
                    if (val_type) {
                        Type *eff = type_unwrap_distinct(val_type);
                        if (!type_is_integer(eff)) {
                            checker_error(c, node->loc.line,
                                "@inttoptr address must be an integer, got '%s'",
                                type_name(val_type));
                        }
                    }
                }
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "ptrtoint", 8) == 0) {
            /* @ptrtoint(ptr) — source must be a pointer */
            if (node->intrinsic.arg_count > 0) {
                Type *val_type = typemap_get(node->intrinsic.args[0]);
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
            result = type_pointer(c->arena, ty_u8);
        } else if (nlen == 9 && memcmp(name, "container", 9) == 0) {
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
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
                    Type *val_type = typemap_get(node->intrinsic.args[0]);
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
    typemap_set(node, result);
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
        /* Pool/Ring must be global or static — not on the stack */
        if (node->kind == NODE_VAR_DECL && type &&
            (type->kind == TYPE_POOL || type->kind == TYPE_RING) &&
            !node->var_decl.is_static) {
            checker_error(c, node->loc.line,
                "%s must be declared as global or static — "
                "stack allocation risks overflow",
                type->kind == TYPE_POOL ? "Pool" : "Ring");
        }
        /* propagate const from var qualifier to slice/pointer type */
        if (node->var_decl.is_const && type) {
            if (type->kind == TYPE_SLICE && !type->slice.is_const) {
                type = type_const_slice(c->arena, type->slice.inner);
            } else if (type->kind == TYPE_POINTER && !type->pointer.is_const) {
                type = type_const_pointer(c->arena, type->pointer.inner);
            }
        }
        typemap_set(node, type); /* store for emitter to read via checker_get_type */

        if (node->var_decl.init) {
            Type *init_type = check_expr(c, node->var_decl.init);

            /* non-storable check: pool.get(h) result */
            if (is_non_storable(node->var_decl.init)) {
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
                /* BUG-197: volatile pointer → non-volatile drops volatile qualifier.
                 * &volatile_var yields a volatile pointer (is_volatile on type).
                 * Target must also be volatile (var_decl.is_volatile or type.is_volatile). */
                if (type->kind == TYPE_POINTER && init_type->kind == TYPE_POINTER &&
                    init_type->pointer.is_volatile &&
                    !type->pointer.is_volatile && !node->var_decl.is_volatile) {
                    checker_error(c, node->loc.line,
                        "cannot initialize non-volatile pointer from volatile — "
                        "writes through non-volatile pointer may be optimized away");
                }
                if (type->kind == TYPE_SLICE && init_type->kind == TYPE_SLICE &&
                    init_type->slice.is_const && !type->slice.is_const) {
                    checker_error(c, node->loc.line,
                        "cannot initialize mutable slice from const — "
                        "would allow writing to read-only memory");
                }
            }

            if (!type_equals(type, init_type) &&
                !can_implicit_coerce(init_type, type) &&
                !is_literal_compatible(node->var_decl.init, type)) {
                checker_error(c, node->loc.line,
                    "cannot initialize '%.*s' of type '%s' with '%s'",
                    (int)node->var_decl.name_len, node->var_decl.name,
                    type_name(type), type_name(init_type));
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
                        Type *obj_type = checker_get_type(obj);
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
                /* propagate arena-derived from init expression
                 * Walk field/index chains to find root (handles w.ptr, arr[i]) */
                {
                    Node *init_root = node->var_decl.init;
                    /* BUG-206: walk through NODE_ORELSE to reach the expression root */
                    if (init_root && init_root->kind == NODE_ORELSE)
                        init_root = init_root->orelse.expr;
                    while (init_root && (init_root->kind == NODE_FIELD ||
                                         init_root->kind == NODE_INDEX)) {
                        if (init_root->kind == NODE_FIELD) init_root = init_root->field.object;
                        else init_root = init_root->index_expr.object;
                    }
                    if (init_root && init_root->kind == NODE_IDENT) {
                        Symbol *src = scope_lookup(c->current_scope,
                            init_root->ident.name,
                            (uint32_t)init_root->ident.name_len);
                        if (src && src->is_arena_derived) {
                            sym->is_arena_derived = true;
                        }
                        if (src && src->is_local_derived) {
                            sym->is_local_derived = true;
                        }
                    }
                }

                /* detect pointer to local: p = &local or p = &local.field
                 * BUG-202: also check orelse fallback: p = opt orelse &local */
                {
                    Node *init = node->var_decl.init;
                    /* check both direct &local AND orelse fallback &local */
                    Node *addr_exprs[2] = { NULL, NULL };
                    int addr_count = 0;
                    if (init->kind == NODE_UNARY && init->unary.op == TOK_AMP) {
                        addr_exprs[addr_count++] = init;
                    }
                    if (init->kind == NODE_ORELSE &&
                        init->orelse.fallback &&
                        init->orelse.fallback->kind == NODE_UNARY &&
                        init->orelse.fallback->unary.op == TOK_AMP) {
                        addr_exprs[addr_count++] = init->orelse.fallback;
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

            /* BUG-203/207: slice from local array — mark as local-derived.
             * []T s = local_array OR []T s = local_array[1..4]
             * Both create a slice pointing to stack memory. */
            if (node->var_decl.init && type && type->kind == TYPE_SLICE) {
                Node *slice_root = node->var_decl.init;
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
                    Type *root_type = checker_get_type(slice_root);
                    if (root_type && root_type->kind == TYPE_ARRAY) {
                        Symbol *src = scope_lookup(c->current_scope,
                            slice_root->ident.name,
                            (uint32_t)slice_root->ident.name_len);
                        bool is_global = src && scope_lookup_local(c->global_scope,
                            src->name, src->name_len) != NULL;
                        if (src && !src->is_static && !is_global) {
                            sym->is_local_derived = true;
                        }
                    }
                }
            }
        }
        break;
    }

    case NODE_IF: {
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
                    cap_const = false;
                } else {
                    cap_type = unwrapped;
                    cap_const = true;
                }
                Symbol *cap = add_symbol(c, node->if_stmt.capture_name,
                    (uint32_t)node->if_stmt.capture_name_len,
                    cap_type, node->loc.line);
                if (cap) {
                    cap->is_const = cap_const;

                    /* propagate arena-derived from if-unwrap condition:
                     * if (arena.alloc(T)) |t| { ... } — t is arena-derived */
                    Node *cond_expr = node->if_stmt.cond;
                    if (cond_expr->kind == NODE_CALL &&
                        cond_expr->call.callee->kind == NODE_FIELD) {
                        Node *obj = cond_expr->call.callee->field.object;
                        const char *mname = cond_expr->call.callee->field.field_name;
                        size_t mlen = cond_expr->call.callee->field.field_name_len;
                        if (obj) {
                            Type *obj_type = checker_get_type(obj);
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

                if (expr->kind == TYPE_UNION) {
                    /* tagged union switch — look up variant type */
                    Type *variant_type = ty_void;
                    if (arm->value_count > 0 && arm->values[0]->kind == NODE_IDENT) {
                        const char *vname = arm->values[0]->ident.name;
                        uint32_t vlen = (uint32_t)arm->values[0]->ident.name_len;
                        for (uint32_t k = 0; k < expr->union_type.variant_count; k++) {
                            SUVariant *v = &expr->union_type.variants[k];
                            if (v->name_len == vlen && memcmp(v->name, vname, vlen) == 0) {
                                variant_type = v->type;
                                break;
                            }
                        }
                    }
                    if (arm->capture_is_ptr) {
                        cap_type = type_pointer(c->arena, variant_type);
                        cap_const = false;
                    } else {
                        cap_type = variant_type;
                        cap_const = true;
                    }
                } else if (type_is_optional(expr)) {
                    /* optional switch with capture */
                    Type *unwrapped = type_unwrap_optional(expr);
                    if (arm->capture_is_ptr) {
                        cap_type = type_pointer(c->arena, unwrapped);
                        cap_const = false;
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

                /* lock union variable during switch arm to prevent type confusion.
                 * Handles: switch (d) and switch (*ptr) where ptr points to union */
                const char *saved_union_var = c->union_switch_var;
                uint32_t saved_union_var_len = c->union_switch_var_len;
                if (expr->kind == TYPE_UNION) {
                    Node *sw_expr = node->switch_stmt.expr;
                    /* direct: switch (d) */
                    if (sw_expr->kind == NODE_IDENT) {
                        c->union_switch_var = sw_expr->ident.name;
                        c->union_switch_var_len = (uint32_t)sw_expr->ident.name_len;
                    }
                    /* deref: switch (*ptr) */
                    else if (sw_expr->kind == NODE_UNARY &&
                             sw_expr->unary.op == TOK_STAR &&
                             sw_expr->unary.operand->kind == NODE_IDENT) {
                        c->union_switch_var = sw_expr->unary.operand->ident.name;
                        c->union_switch_var_len = (uint32_t)sw_expr->unary.operand->ident.name_len;
                    }
                }
                check_stmt(c, arm->body);
                c->union_switch_var = saved_union_var;
                c->union_switch_var_len = saved_union_var_len;
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

            /* const laundering: reject returning const ptr/slice as mutable */
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
            }

            /* scope escape: return local array as slice → dangling pointer */
            if (node->ret.expr->kind == NODE_IDENT &&
                ret_type && ret_type->kind == TYPE_ARRAY &&
                c->current_func_ret && c->current_func_ret->kind == TYPE_SLICE) {
                const char *vname = node->ret.expr->ident.name;
                uint32_t vlen = (uint32_t)node->ret.expr->ident.name_len;
                Symbol *sym = scope_lookup(c->current_scope, vname, vlen);
                bool is_global = scope_lookup_local(c->global_scope, vname, vlen) != NULL;
                if (sym && !sym->is_static && !is_global) {
                    checker_error(c, node->loc.line,
                        "cannot return local array '%.*s' as slice — "
                        "pointer will dangle after function returns",
                        (int)vlen, vname);
                }
            }

            /* scope escape: return arena-derived pointer → dangling after stack unwind
             * Walk field/index chains to find root ident (BUG-155: h.ptr escape) */
            {
                Node *root = node->ret.expr;
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
        typemap_set(node, t);

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
        typemap_set(node, t);
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
        typemap_set(node, t);

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
        typemap_set(node, type);
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
            sym->func_node = node;
        }
        typemap_set(node, func_type);
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
        Symbol *sym = add_symbol(c, node->var_decl.name,
                                 (uint32_t)node->var_decl.name_len,
                                 type, node->loc.line);
        if (sym) {
            sym->is_const = node->var_decl.is_const;
            sym->is_volatile = node->var_decl.is_volatile;
            sym->is_static = node->var_decl.is_static;
        }
        typemap_set(node, type);
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
    typemap_init(arena);

    /* init non-storable tracking */
    non_storable_init(arena);

    /* Arena type available in expression context for Arena.over() */
    scope_add(arena, c->global_scope, "Arena", 5, ty_arena, 0, file_name);
}

Type *checker_get_type(Node *node) {
    return typemap_get(node);
}

void checker_register_file(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        /* skip imports — they're handled by the compiler driver */
        if (decl->kind == NODE_IMPORT || decl->kind == NODE_CINCLUDE) continue;
        /* skip static (module-private) declarations */
        if (decl->kind == NODE_FUNC_DECL && decl->func_decl.is_static) continue;
        if (decl->kind == NODE_GLOBAL_VAR && decl->var_decl.is_static) continue;
        register_decl(c, decl);
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
            Type *t = typemap_get(decl);
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
