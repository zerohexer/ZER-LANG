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

static void checker_error(Checker *c, int line, const char *fmt, ...) {
    c->error_count++;
    fprintf(stderr, "%s:%d: error: ", c->file_name, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void checker_warning(Checker *c, int line, const char *fmt, ...) {
    c->warning_count++;
    fprintf(stderr, "%s:%d: warning: ", c->file_name, line);
    va_list args;
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
    Type *effective = target;
    if (target->kind == TYPE_DISTINCT) effective = target->distinct.underlying;
    if (expr->kind == NODE_INT_LIT && type_is_integer(effective)) return true;
    if (expr->kind == NODE_INT_LIT && effective->kind == TYPE_BOOL) return true; /* 0/1 → bool */
    if (expr->kind == NODE_FLOAT_LIT && type_is_float(effective)) return true;
    if (expr->kind == NODE_NULL_LIT && type_is_optional(target)) return true;
    if (expr->kind == NODE_BOOL_LIT && effective->kind == TYPE_BOOL) return true;
    if (expr->kind == NODE_CHAR_LIT && effective->kind == TYPE_U8) return true;
    /* -5 is UNARY(MINUS, INT_LIT) — negative integer literal */
    if (expr->kind == NODE_UNARY && expr->unary.op == TOK_MINUS) {
        if (expr->unary.operand->kind == NODE_INT_LIT && type_is_integer(effective))
            return true;
        if (expr->unary.operand->kind == NODE_FLOAT_LIT && type_is_float(effective))
            return true;
    }
    return false;
}

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
        /* for now, extract int literal value directly */
        uint32_t size = 0;
        if (tn->array.size_expr && tn->array.size_expr->kind == NODE_INT_LIT) {
            size = (uint32_t)tn->array.size_expr->int_lit.value;
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
        if (tn->pool.count_expr && tn->pool.count_expr->kind == NODE_INT_LIT) {
            count = (uint32_t)tn->pool.count_expr->int_lit.value;
        }
        return type_pool(c->arena, elem, count);
    }

    case TYNODE_RING: {
        Type *elem = resolve_type(c, tn->ring.elem);
        uint32_t count = 0;
        if (tn->ring.count_expr && tn->ring.count_expr->kind == NODE_INT_LIT) {
            count = (uint32_t)tn->ring.count_expr->int_lit.value;
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
        /* volatile doesn't affect the type system, only codegen */
        return resolve_type(c, tn->qualified.inner);
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
                result = common_numeric_type(c, left, right, node->loc.line);
            }
            break;

        /* comparison: both same type (or coercible), result = bool */
        case TOK_EQEQ: case TOK_BANGEQ:
        case TOK_LT: case TOK_GT: case TOK_LTEQ: case TOK_GTEQ:
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
            break;

        default:
            result = operand;
            break;
        }
        break;
    }

    /* ---- Assignment ---- */
    case NODE_ASSIGN: {
        Type *target = check_expr(c, node->assign.target);
        Type *value = check_expr(c, node->assign.value);

        /* const check: cannot assign to const variable */
        if (node->assign.target->kind == NODE_IDENT) {
            Symbol *sym = scope_lookup(c->current_scope,
                node->assign.target->ident.name,
                (uint32_t)node->assign.target->ident.name_len);
            if (sym && sym->is_const) {
                checker_error(c, node->loc.line,
                    "cannot assign to const variable '%.*s'",
                    (int)sym->name_len, sym->name);
            }
        }

        /* non-storable check: pool.get(h) result cannot be stored */
        if (node->assign.op == TOK_EQ && is_non_storable(node->assign.value)) {
            checker_error(c, node->loc.line,
                "cannot store result of get() — use inline");
        }

        /* scope escape: storing &local in static/global variable */
        if (node->assign.op == TOK_EQ &&
            node->assign.value->kind == NODE_UNARY &&
            node->assign.value->unary.op == TOK_AMP &&
            node->assign.value->unary.operand->kind == NODE_IDENT &&
            node->assign.target->kind == NODE_IDENT) {
            Symbol *target_sym = scope_lookup(c->current_scope,
                node->assign.target->ident.name,
                (uint32_t)node->assign.target->ident.name_len);
            Symbol *val_sym = scope_lookup(c->current_scope,
                node->assign.value->unary.operand->ident.name,
                (uint32_t)node->assign.value->unary.operand->ident.name_len);
            if (target_sym && val_sym &&
                target_sym->is_static && !val_sym->is_static) {
                checker_error(c, node->loc.line,
                    "cannot store pointer to local '%.*s' in static variable '%.*s'",
                    (int)val_sym->name_len, val_sym->name,
                    (int)target_sym->name_len, target_sym->name);
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
                    result = type_optional(c->arena, type_handle(c->arena, obj->pool.elem));
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 3 && memcmp(mname, "get", 3) == 0) {
                    result = type_pointer(c->arena, obj->pool.elem);
                    typemap_set(field_node, result);
                    mark_non_storable(node); /* Rule 2: get() result non-storable */
                    break;
                }
                if (mlen == 4 && memcmp(mname, "free", 4) == 0) {
                    result = ty_void;
                    typemap_set(field_node, result);
                    break;
                }
            }

            /* Ring methods */
            if (obj->kind == TYPE_RING) {
                if (mlen == 4 && memcmp(mname, "push", 4) == 0) {
                    result = ty_void;
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 12 && memcmp(mname, "push_checked", 12) == 0) {
                    result = type_optional(c->arena, ty_void);
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 3 && memcmp(mname, "pop", 3) == 0) {
                    result = type_optional(c->arena, obj->ring.elem);
                    typemap_set(field_node, result);
                    break;
                }
            }

            /* Arena methods */
            if (obj->kind == TYPE_ARENA) {
                if (mlen == 4 && memcmp(mname, "over", 4) == 0) {
                    result = ty_arena;
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                    result = ty_void; /* TODO: type arg */
                    typemap_set(field_node, result);
                    break;
                }
                if (mlen == 5 && memcmp(mname, "reset", 5) == 0) {
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
                if (mlen == 12 && memcmp(mname, "unsafe_reset", 12) == 0) {
                    result = ty_void;
                    typemap_set(field_node, result);
                    break;
                }
            }

            /* not a builtin — fall through to normal call resolution */
        }

        /* normal function call */
        Type *callee_type = check_expr(c, node->call.callee);

        if (callee_type->kind == TYPE_FUNC_PTR) {
            /* verify arg count */
            if ((uint32_t)node->call.arg_count != callee_type->func_ptr.param_count) {
                checker_error(c, node->loc.line,
                    "expected %u arguments, got %d",
                    callee_type->func_ptr.param_count, node->call.arg_count);
            } else {
                /* verify each arg type */
                for (uint32_t i = 0; i < callee_type->func_ptr.param_count; i++) {
                    Type *param = callee_type->func_ptr.params[i];
                    Type *arg = arg_types[i];
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
            result = callee_type->func_ptr.ret;
        } else {
            /* UFCS resolution: expr.method(args) → method(&expr, args)
             * Look for a free function named 'method' where first param
             * matches *typeof(expr). */
            if (node->call.callee->kind == NODE_FIELD) {
                Node *fn = node->call.callee;
                const char *mname = fn->field.field_name;
                uint32_t mlen = (uint32_t)fn->field.field_name_len;
                Type *obj_type = typemap_get(fn->field.object);

                Symbol *func_sym = scope_lookup(c->current_scope, mname, mlen);
                if (func_sym && func_sym->is_function &&
                    func_sym->type->kind == TYPE_FUNC_PTR) {
                    Type *ftype = func_sym->type;
                    /* check first param is pointer to obj's type */
                    if (ftype->func_ptr.param_count > 0) {
                        Type *first_param = ftype->func_ptr.params[0];
                        if (first_param->kind == TYPE_POINTER &&
                            (type_equals(first_param->pointer.inner, obj_type) ||
                             (obj_type && obj_type->kind == TYPE_POINTER &&
                              type_equals(first_param, obj_type)))) {
                            /* UFCS match — check remaining args */
                            uint32_t expected = ftype->func_ptr.param_count - 1;
                            if ((uint32_t)node->call.arg_count != expected) {
                                checker_error(c, node->loc.line,
                                    "expected %u arguments for UFCS call, got %d",
                                    expected, node->call.arg_count);
                            }
                            result = ftype->func_ptr.ret;
                            break;
                        }
                    }
                }
            }
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
                /* not a field — might be UFCS (resolved at call site)
                 * don't error here; let the call handler try UFCS.
                 * if it's not a call, the void result will propagate. */
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
        if (obj->kind == TYPE_POINTER && obj->pointer.inner->kind == TYPE_STRUCT) {
            Type *inner = obj->pointer.inner;
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

        /* union: direct field access forbidden — must switch first */
        if (obj->kind == TYPE_UNION) {
            checker_error(c, node->loc.line,
                "cannot access union variant '%.*s' directly — must use switch",
                (int)flen, fname);
            result = ty_void;
            break;
        }

        /* fallback: unresolved field access (might be UFCS) */
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
            result = obj->array.inner;
        } else if (obj->kind == TYPE_SLICE) {
            result = obj->slice.inner;
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

        if (obj->kind == TYPE_ARRAY) {
            result = type_slice(c->arena, obj->array.inner);
        } else if (obj->kind == TYPE_SLICE) {
            result = obj; /* slice of slice = same slice type */
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

        if (node->orelse.fallback_is_return ||
            node->orelse.fallback_is_break ||
            node->orelse.fallback_is_continue) {
            /* flow control — expression yields unwrapped type */
            result = unwrapped;
        } else if (node->orelse.fallback) {
            Type *fallback = check_expr(c, node->orelse.fallback);
            /* fallback must match unwrapped type */
            if (!type_equals(unwrapped, fallback) &&
                !can_implicit_coerce(fallback, unwrapped)) {
                checker_error(c, node->loc.line,
                    "orelse fallback type '%s' doesn't match '%s'",
                    type_name(fallback), type_name(unwrapped));
            }
            result = unwrapped;
        } else {
            result = unwrapped;
        }
        break;
    }

    /* ---- Intrinsic ---- */
    case NODE_INTRINSIC: {
        /* type-check arguments */
        for (int i = 0; i < node->intrinsic.arg_count; i++) {
            check_expr(c, node->intrinsic.args[i]);
        }
        /* return type depends on which intrinsic */
        const char *name = node->intrinsic.name;
        uint32_t nlen = (uint32_t)node->intrinsic.name_len;

        if (nlen == 4 && memcmp(name, "size", 4) == 0) {
            result = ty_usize;
        } else if (nlen == 6 && memcmp(name, "offset", 6) == 0) {
            result = ty_usize;
        } else if (nlen == 7 && memcmp(name, "ptrcast", 7) == 0) {
            /* @ptrcast(*T, expr) → *T */
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
            } else {
                result = ty_void;
            }
        } else if (nlen == 7 && memcmp(name, "bitcast", 7) == 0) {
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "truncate", 8) == 0) {
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "saturate", 8) == 0) {
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "inttoptr", 8) == 0) {
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
            } else {
                result = ty_void;
            }
        } else if (nlen == 8 && memcmp(name, "ptrtoint", 8) == 0) {
            result = ty_usize;
        } else if ((nlen == 7 && memcmp(name, "barrier", 7) == 0) ||
                   (nlen == 13 && memcmp(name, "barrier_store", 13) == 0) ||
                   (nlen == 12 && memcmp(name, "barrier_load", 12) == 0)) {
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
            /* @cast(T, val) — only valid between distinct typedefs with same underlying */
            if (node->intrinsic.type_arg) {
                result = resolve_type(c, node->intrinsic.type_arg);
                if (result->kind != TYPE_DISTINCT) {
                    checker_error(c, node->loc.line,
                        "@cast target must be a distinct typedef");
                }
                if (node->intrinsic.arg_count > 0) {
                    Type *val_type = typemap_get(node->intrinsic.args[0]);
                    if (val_type && val_type->kind == TYPE_DISTINCT &&
                        result->kind == TYPE_DISTINCT) {
                        if (!type_equals(val_type->distinct.underlying,
                                         result->distinct.underlying)) {
                            checker_error(c, node->loc.line,
                                "@cast between unrelated distinct types");
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

        if (node->var_decl.init) {
            Type *init_type = check_expr(c, node->var_decl.init);

            /* non-storable check: pool.get(h) result */
            if (is_non_storable(node->var_decl.init)) {
                checker_error(c, node->loc.line,
                    "cannot store result of get() — use inline");
            }

            /* check assignment compatibility */
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
            sym->is_static = node->var_decl.is_static;
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
                if (cap) cap->is_const = cap_const;

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

                check_stmt(c, arm->body);
                pop_scope(c);
            } else {
                check_stmt(c, arm->body);
            }
        }

        /* exhaustiveness check */
        {
            bool has_default = false;
            for (int i = 0; i < node->switch_stmt.arm_count; i++) {
                if (node->switch_stmt.arms[i].is_default) {
                    has_default = true;
                    break;
                }
            }

            if (expr->kind == TYPE_ENUM) {
                /* enum switch: must handle all variants OR have default */
                if (!has_default) {
                    uint32_t total = expr->enum_type.variant_count;
                    uint32_t handled = 0;
                    for (int i = 0; i < node->switch_stmt.arm_count; i++) {
                        handled += (uint32_t)node->switch_stmt.arms[i].value_count;
                    }
                    if (handled < total) {
                        checker_error(c, node->loc.line,
                            "switch on enum '%.*s' is not exhaustive — "
                            "handles %u of %u variants",
                            (int)expr->enum_type.name_len,
                            expr->enum_type.name, handled, total);
                    }
                }
            } else if (type_equals(expr, ty_bool)) {
                /* bool switch: must handle true and false */
                if (!has_default && node->switch_stmt.arm_count < 2) {
                    checker_error(c, node->loc.line,
                        "switch on bool must handle both true and false");
                }
            } else if (type_is_integer(expr)) {
                /* integer switch: must have default */
                if (!has_default) {
                    checker_error(c, node->loc.line,
                        "switch on integer must have a default arm");
                }
            } else if (expr->kind == TYPE_UNION) {
                /* union switch: must handle all variants OR have default */
                if (!has_default) {
                    uint32_t total = expr->union_type.variant_count;
                    uint32_t handled = 0;
                    for (int i = 0; i < node->switch_stmt.arm_count; i++) {
                        handled += (uint32_t)node->switch_stmt.arms[i].value_count;
                    }
                    if (handled < total) {
                        checker_error(c, node->loc.line,
                            "switch on union '%.*s' is not exhaustive — "
                            "handles %u of %u variants",
                            (int)expr->union_type.name_len,
                            expr->union_type.name, handled, total);
                    }
                }
            }
        }
        break;
    }

    case NODE_RETURN: {
        if (node->ret.expr) {
            Type *ret_type = check_expr(c, node->ret.expr);

            /* scope escape: return &local → error */
            if (node->ret.expr->kind == NODE_UNARY &&
                node->ret.expr->unary.op == TOK_AMP &&
                node->ret.expr->unary.operand->kind == NODE_IDENT) {
                const char *vname = node->ret.expr->unary.operand->ident.name;
                uint32_t vlen = (uint32_t)node->ret.expr->unary.operand->ident.name_len;
                Symbol *sym = scope_lookup(c->current_scope, vname, vlen);
                if (sym && !sym->is_static) {
                    checker_error(c, node->loc.line,
                        "cannot return pointer to local variable '%.*s'",
                        (int)vlen, vname);
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
        if (!c->in_loop) {
            checker_error(c, node->loc.line, "'break' outside of loop");
        }
        break;

    case NODE_CONTINUE:
        if (!c->in_loop) {
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
        /* create struct type and register */
        Type *t = (Type *)arena_alloc(c->arena, sizeof(Type));
        t->kind = TYPE_STRUCT;
        t->struct_type.name = node->struct_decl.name;
        t->struct_type.name_len = (uint32_t)node->struct_decl.name_len;
        t->struct_type.is_packed = node->struct_decl.is_packed;
        t->struct_type.field_count = (uint32_t)node->struct_decl.field_count;

        /* resolve field types */
        if (node->struct_decl.field_count > 0) {
            t->struct_type.fields = (SField *)arena_alloc(c->arena,
                node->struct_decl.field_count * sizeof(SField));
            for (int i = 0; i < node->struct_decl.field_count; i++) {
                FieldDecl *fd = &node->struct_decl.fields[i];
                SField *sf = &t->struct_type.fields[i];
                sf->name = fd->name;
                sf->name_len = (uint32_t)fd->name_len;
                sf->type = resolve_type(c, fd->type);
                sf->is_keep = fd->is_keep;
            }
        }

        add_symbol(c, node->struct_decl.name,
                   (uint32_t)node->struct_decl.name_len,
                   t, node->loc.line);
        break;
    }

    case NODE_ENUM_DECL: {
        Type *t = (Type *)arena_alloc(c->arena, sizeof(Type));
        t->kind = TYPE_ENUM;
        t->enum_type.name = node->enum_decl.name;
        t->enum_type.name_len = (uint32_t)node->enum_decl.name_len;
        t->enum_type.variant_count = (uint32_t)node->enum_decl.variant_count;

        if (node->enum_decl.variant_count > 0) {
            t->enum_type.variants = (SEVariant *)arena_alloc(c->arena,
                node->enum_decl.variant_count * sizeof(SEVariant));
            int32_t next_val = 0;
            for (int i = 0; i < node->enum_decl.variant_count; i++) {
                EnumVariant *ev = &node->enum_decl.variants[i];
                SEVariant *sv = &t->enum_type.variants[i];
                sv->name = ev->name;
                sv->name_len = (uint32_t)ev->name_len;
                if (ev->value) {
                    /* explicit value — evaluate */
                    if (ev->value->kind == NODE_INT_LIT) {
                        sv->value = (int32_t)ev->value->int_lit.value;
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
        break;
    }

    case NODE_UNION_DECL: {
        Type *t = (Type *)arena_alloc(c->arena, sizeof(Type));
        t->kind = TYPE_UNION;
        t->union_type.name = node->union_decl.name;
        t->union_type.name_len = (uint32_t)node->union_decl.name_len;
        t->union_type.variant_count = (uint32_t)node->union_decl.variant_count;

        if (node->union_decl.variant_count > 0) {
            t->union_type.variants = (SUVariant *)arena_alloc(c->arena,
                node->union_decl.variant_count * sizeof(SUVariant));
            for (int i = 0; i < node->union_decl.variant_count; i++) {
                UnionVariant *uv = &node->union_decl.variants[i];
                SUVariant *sv = &t->union_type.variants[i];
                sv->name = uv->name;
                sv->name_len = (uint32_t)uv->name_len;
                sv->type = resolve_type(c, uv->type);
            }
        }

        add_symbol(c, node->union_decl.name,
                   (uint32_t)node->union_decl.name_len,
                   t, node->loc.line);
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

        Symbol *sym = add_symbol(c, node->func_decl.name,
                                 (uint32_t)node->func_decl.name_len,
                                 func_type, node->loc.line);
        if (sym) {
            sym->is_function = true;
            sym->is_static = node->func_decl.is_static;
            sym->func_node = node;
        }
        break;
    }

    case NODE_GLOBAL_VAR: {
        Type *type = resolve_type(c, node->var_decl.type);
        Symbol *sym = add_symbol(c, node->var_decl.name,
                                 (uint32_t)node->var_decl.name_len,
                                 type, node->loc.line);
        if (sym) {
            sym->is_const = node->var_decl.is_const;
            sym->is_static = node->var_decl.is_static;
        }
        break;
    }

    case NODE_IMPORT:
        /* TODO: module resolution */
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
}

bool checker_check(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return false;

    /* Pass 1: register all top-level declarations */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        register_decl(c, file_node->file.decls[i]);
    }

    /* Pass 2: type-check all function bodies and global initializers */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        check_func_body(c, decl);

        /* check global var initializers */
        if (decl->kind == NODE_GLOBAL_VAR && decl->var_decl.init) {
            Type *type = resolve_type(c, decl->var_decl.type);
            Type *init = check_expr(c, decl->var_decl.init);
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
