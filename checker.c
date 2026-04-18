#include "checker.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

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

/* Print the source line and a caret underline for the error location.
 * If source is NULL, does nothing. */
static void print_source_line(FILE *out, const char *source, int line) {
    if (!source || line < 1) return;
    /* find line N (1-based) */
    const char *p = source;
    for (int cur = 1; cur < line && *p; cur++) {
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    if (!*p) return;
    /* find end of line */
    const char *end = p;
    while (*end && *end != '\n' && *end != '\r') end++;
    int len = (int)(end - p);
    if (len == 0) return;
    /* find first non-whitespace for caret position */
    int first_nonws = 0;
    while (first_nonws < len && (p[first_nonws] == ' ' || p[first_nonws] == '\t'))
        first_nonws++;
    int content_len = len - first_nonws;
    if (content_len <= 0) return;
    /* print: " line | source_text" */
    fprintf(out, " %4d | %.*s\n", line, len, p);
    /* print: "      | ^^^^..." under the content */
    fprintf(out, "      | ");
    for (int i = 0; i < first_nonws; i++)
        fputc(p[i] == '\t' ? '\t' : ' ', out);
    for (int i = 0; i < content_len && i < 60; i++)
        fputc('^', out);
    fputc('\n', out);
}

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
    print_source_line(stderr, c->source, line);
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
    print_source_line(stderr, c->source, line);
}

/* ---- Non-storable tracking ----
 * pool.get(h) returns a non-storable result.
 * BUG-346: moved from static globals into Checker struct for thread safety. */

/* ---- Handle auto-deref: find unique Slab/Pool for a Handle's element type ---- */
Symbol *find_unique_allocator(Scope *s, Type *elem_type) {
    Symbol *found = NULL;
    for (Scope *sc = s; sc; sc = sc->parent) {
        for (uint32_t i = 0; i < sc->symbol_count; i++) {
            Type *t = sc->symbols[i].type;
            if (!t) continue;
            if ((t->kind == TYPE_SLAB && type_equals(t->slab.elem, elem_type)) ||
                (t->kind == TYPE_POOL && type_equals(t->pool.elem, elem_type))) {
                /* BUG-416 fix: imported module globals are registered under both
                 * raw name ("cross_world") and mangled name ("cross_entity__cross_world")
                 * in the global scope (BUG-233). Both point to the same Type*.
                 * Don't treat these as ambiguous — same Type* = same allocator. */
                if (found && found->type == t) continue; /* same allocator, skip */
                if (found) return NULL; /* genuinely different allocator — ambiguous */
                found = &sc->symbols[i];
            }
        }
    }
    return found;
}

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

static Symbol *add_symbol_internal(Checker *c, const char *name, uint32_t name_len,
                                   Type *type, int line) {
    /* Internal: no _zer_ prefix check — used for compiler-generated symbols */
    Symbol *sym = scope_add(c->arena, c->current_scope, name, name_len,
                            type, line, c->file_name);
    return sym;
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
            Type *et = type_unwrap_distinct(existing->type);
            if (et->kind == TYPE_STRUCT) existing_mod = et->struct_type.module_prefix;
            else if (et->kind == TYPE_ENUM) existing_mod = et->enum_type.module_prefix;
            else if (et->kind == TYPE_UNION) existing_mod = et->union_type.module_prefix;

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

/* Value range propagation helpers (defined after checker_init) */
static struct VarRange *find_var_range(Checker *c, const char *name, uint32_t name_len);
static void push_var_range(Checker *c, const char *name, uint32_t name_len,
                           int64_t min_val, int64_t max_val, bool known_nonzero);
static void mark_proven(Checker *c, Node *node);
static void mark_auto_guard(Checker *c, Node *node, uint64_t array_size);
static bool body_always_exits(Node *body);
static Type *prov_map_get(Checker *c, const char *key, uint32_t key_len);
static Type *find_return_provenance(Checker *c, Node *node);
static bool find_return_range(Checker *c, Node *node, int64_t *out_min, int64_t *out_max, bool *found);
static Type *find_param_cast_type(Checker *c, Node *node, const char *param_name, uint32_t param_len);
static void add_prov_summary(Checker *c, const char *name, uint32_t name_len, Type *prov);
static void track_isr_global(Checker *c, const char *name, uint32_t name_len, bool is_compound);
static Type *lookup_prov_summary(Checker *c, const char *name, uint32_t name_len);

/* Try to derive a bounded range from an expression.
 * x % N → [0, N-1], x & MASK → [0, MASK] (for constant N/MASK > 0).
 * Returns true and sets *out_min/*out_max if a bounded range was derived. */
/* Refactor 1: unified VRP range invalidation for assignments.
 * Handles both simple ident keys and compound keys (s.x).
 * For TOK_EQ: tries literal, derive_expr_range, call return range.
 * For compound ops (+=, -=, etc.): always wipes.
 * Prevents BUG-502 class: compound key path was missing compound op check. */
static void vrp_invalidate_for_assign(Checker *c, const char *key, uint32_t key_len,
                                       TokenType op, Node *value);

static bool derive_expr_range(Checker *c, Node *expr, int64_t *out_min, int64_t *out_max) {
    if (!expr || expr->kind != NODE_BINARY) return false;
    Node *rhs = expr->binary.right;
    int64_t rval = eval_const_expr(rhs);
    /* try const symbol lookup for ident RHS (e.g., MAP_SIZE) */
    if (rval == CONST_EVAL_FAIL && rhs->kind == NODE_IDENT) {
        Symbol *rsym = scope_lookup(c->current_scope,
            rhs->ident.name, (uint32_t)rhs->ident.name_len);
        if (rsym && rsym->is_const && rsym->func_node) {
            Node *init = NULL;
            if (rsym->func_node->kind == NODE_GLOBAL_VAR)
                init = rsym->func_node->var_decl.init;
            else if (rsym->func_node->kind == NODE_VAR_DECL)
                init = rsym->func_node->var_decl.init;
            if (init) rval = eval_const_expr(init);
        }
    }
    if (rval == CONST_EVAL_FAIL || rval <= 0) return false;
    if (expr->binary.op == TOK_PERCENT) {
        /* x % N → [0, N-1] */
        *out_min = 0;
        *out_max = rval - 1;
        return true;
    }
    if (expr->binary.op == TOK_AMP) {
        /* x & MASK → [0, MASK] (unsigned bitmask) */
        *out_min = 0;
        *out_max = rval;
        return true;
    }
    return false;
}

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

/* BUG-392: Build a string key from an expression for union switch locking.
 * Same pattern as zercheck's handle_key_from_expr. Returns key length or 0. */
static int build_expr_key(Node *expr, char *buf, int bufsize) {
    if (!expr || bufsize < 2) return 0;
    if (expr->kind == NODE_IDENT) {
        int len = (int)expr->ident.name_len;
        if (len >= bufsize) return 0;
        memcpy(buf, expr->ident.name, len);
        buf[len] = '\0';
        return len;
    }
    if (expr->kind == NODE_FIELD) {
        int base = build_expr_key(expr->field.object, buf, bufsize);
        if (base <= 0) return 0;
        int flen = (int)expr->field.field_name_len;
        if (base + 1 + flen >= bufsize) return 0;
        buf[base] = '.';
        memcpy(buf + base + 1, expr->field.field_name, flen);
        int total = base + 1 + flen;
        buf[total] = '\0';
        return total;
    }
    if (expr->kind == NODE_INDEX && expr->index_expr.index &&
        expr->index_expr.index->kind == NODE_INT_LIT) {
        int base = build_expr_key(expr->index_expr.object, buf, bufsize);
        if (base <= 0) return 0;
        int written = snprintf(buf + base, bufsize - base, "[%llu]",
                               (unsigned long long)expr->index_expr.index->int_lit.value);
        if (written <= 0 || base + written >= bufsize) return 0;
        return base + written;
    }
    if (expr->kind == NODE_UNARY && expr->unary.op == TOK_STAR) {
        int base = build_expr_key(expr->unary.operand, buf, bufsize);
        if (base <= 0) return 0;
        /* prepend * — shift right and insert */
        if (base + 1 >= bufsize) return 0;
        memmove(buf + 1, buf, base + 1);
        buf[0] = '*';
        return base + 1;
    }
    return 0;
}

/* Arena-allocated expr key — no fixed buffer limit.
 * Uses a generous stack buffer, arena-copies the result.
 * Returns {key, len} or {NULL, 0} on failure. */
typedef struct { const char *str; int len; } ExprKey;
static ExprKey build_expr_key_a(Checker *c, Node *expr) {
    char stack_buf[512];
    int len = build_expr_key(expr, stack_buf, sizeof(stack_buf));
    if (len <= 0) return (ExprKey){ NULL, 0 };
    char *key = (char *)arena_alloc(c->arena, len + 1);
    if (!key) return (ExprKey){ NULL, 0 };
    memcpy(key, stack_buf, len + 1);
    return (ExprKey){ key, len };
}

/* Track dynamic-index free for auto-guard (B1 refactor: unified helper).
 * When pool.free(arr[k]) or slab.free(arr[k]) is called with variable index k,
 * record the array+index so the emitter can auto-guard later arr[j].field access.
 * Previously duplicated in pool.free and slab.free handlers — caused BUG-471. */
static void track_dyn_freed_index(Checker *c, Node *call_node) {
    if (call_node->call.arg_count != 1) return;
    Node *arg = call_node->call.args[0];
    if (arg->kind != NODE_INDEX || arg->index_expr.object->kind != NODE_IDENT ||
        arg->index_expr.index->kind == NODE_INT_LIT) return;
    if (c->dyn_freed_count >= c->dyn_freed_capacity) {
        int newcap = c->dyn_freed_capacity ? c->dyn_freed_capacity * 2 : 8;
        struct DynFreed *nf = (struct DynFreed *)arena_alloc(c->arena, newcap * sizeof(struct DynFreed));
        if (nf) {
            if (c->dyn_freed) memcpy(nf, c->dyn_freed, c->dyn_freed_count * sizeof(struct DynFreed));
            c->dyn_freed = nf;
            c->dyn_freed_capacity = newcap;
        }
    }
    if (c->dyn_freed && c->dyn_freed_count < c->dyn_freed_capacity) {
        struct DynFreed *df = &c->dyn_freed[c->dyn_freed_count++];
        df->array_name = arg->index_expr.object->ident.name;
        df->array_name_len = (uint32_t)arg->index_expr.object->ident.name_len;
        df->freed_idx = arg->index_expr.index;
        df->all_freed = c->in_loop;
    }
}

/* B2 refactor: Check if union mutation is blocked by switch arm lock.
 * Walks field_object to root ident, checks name match + pointer alias + precise key.
 * Returns true if mutation should be blocked (caller emits error + breaks).
 * Previously duplicated in pointer-auto-deref union (line ~4577) and direct union (line ~4683). */
static bool check_union_switch_mutation(Checker *c, Node *field_object) {
    if (!c->union_switch_var) return false;
    Node *mut_root = field_object;
    while (mut_root) {
        if (mut_root->kind == NODE_UNARY && mut_root->unary.op == TOK_STAR)
            mut_root = mut_root->unary.operand;
        else if (mut_root->kind == NODE_FIELD) mut_root = mut_root->field.object;
        else if (mut_root->kind == NODE_INDEX) mut_root = mut_root->index_expr.object;
        else break;
    }
    if (!mut_root || mut_root->kind != NODE_IDENT) return false;
    bool name_match = (mut_root->ident.name_len == c->union_switch_var_len &&
        memcmp(mut_root->ident.name, c->union_switch_var, c->union_switch_var_len) == 0);
    bool type_match = false;
    if (!name_match && c->union_switch_type) {
        Symbol *ms = scope_lookup(c->current_scope,
            mut_root->ident.name, (uint32_t)mut_root->ident.name_len);
        if (ms && ms->type) {
            Type *mt = type_unwrap_distinct(ms->type);
            if (mt->kind == TYPE_POINTER) {
                Type *inner_t = type_unwrap_distinct(mt->pointer.inner);
                if (inner_t == c->union_switch_type) type_match = true;
            }
        }
    }
    if (!(name_match || type_match)) return false;
    bool blocked = true;
    if (name_match && c->union_switch_key && c->union_switch_key_len > 0 && !type_match) {
        ExprKey tgt_key = build_expr_key_a(c, field_object);
        if (tgt_key.len > 0 &&
            ((int)c->union_switch_key_len != tgt_key.len ||
             memcmp(tgt_key.str, c->union_switch_key, tgt_key.len) != 0))
            blocked = false;
    }
    if (blocked) {
        checker_error(c, field_object->loc.line,
            "cannot mutate union '%.*s' inside its own switch arm — "
            "active capture would become invalid",
            (int)(name_match ? c->union_switch_var_len : mut_root->ident.name_len),
            name_match ? c->union_switch_var : mut_root->ident.name);
    }
    return blocked;
}

/* BUG-393: compound key provenance map — set/get for struct fields and array elements */
static void prov_map_set(Checker *c, const char *key, uint32_t key_len, Type *prov) {
    /* check if key already exists — update in place */
    for (int i = 0; i < c->prov_map_count; i++) {
        if (c->prov_map[i].key_len == key_len &&
            memcmp(c->prov_map[i].key, key, key_len) == 0) {
            c->prov_map[i].provenance = prov;
            goto set_array_root;
        }
    }
    /* add new entry */
    if (c->prov_map_count >= c->prov_map_capacity) {
        int new_cap = c->prov_map_capacity < 16 ? 16 : c->prov_map_capacity * 2;
        void *np = realloc(c->prov_map, new_cap * sizeof(c->prov_map[0]));
        if (!np) return;
        c->prov_map = np;
        c->prov_map_capacity = new_cap;
    }
    char *akey = (char *)arena_alloc(c->arena, key_len + 1);
    if (!akey) return;
    memcpy(akey, key, key_len);
    akey[key_len] = '\0';
    c->prov_map[c->prov_map_count].key = akey;
    c->prov_map[c->prov_map_count].key_len = key_len;
    c->prov_map[c->prov_map_count].provenance = prov;
    c->prov_map_count++;

set_array_root:
    /* Array-level provenance: if key contains '[', also set root key.
     * "callbacks[0]" → root "callbacks".
     * BUG-466: Only enforce homogeneous provenance for VARIABLE-index access.
     * Constant-indexed access (e.g., ops[0].ctx = *Adder, ops[1].ctx = *Multiplier)
     * is safe — compiler knows which element at compile time.
     * Variable-index access would mean we can't distinguish elements. */
    {
        const char *bracket = memchr(key, '[', key_len);
        if (bracket) {
            uint32_t root_len = (uint32_t)(bracket - key);
            if (root_len > 0) {
                /* Check if index is constant (all digits between [ and ]) */
                const char *after_bracket = bracket + 1;
                const char *close = memchr(after_bracket, ']', key_len - root_len - 1);
                bool is_const_index = (close != NULL && close > after_bracket);
                if (is_const_index) {
                    for (const char *p = after_bracket; p < close; p++) {
                        if (*p < '0' || *p > '9') { is_const_index = false; break; }
                    }
                }
                if (!is_const_index) {
                    /* Variable index — enforce homogeneous root provenance */
                    Type *existing = prov_map_get(c, key, root_len);
                    if (existing && !type_equals(existing, prov)) {
                        checker_error(c, 0,
                            "heterogeneous *opaque array: '%.*s' has provenance '%s' but element assigned '%s'",
                            (int)root_len, key, type_name(existing), type_name(prov));
                    } else if (!existing) {
                        prov_map_set(c, key, root_len, prov);
                    }
                }
                /* Constant index: per-element provenance stored above, root left polymorphic */
            }
        }
    }
}

static Type *prov_map_get(Checker *c, const char *key, uint32_t key_len) {
    for (int i = 0; i < c->prov_map_count; i++) {
        if (c->prov_map[i].key_len == key_len &&
            memcmp(c->prov_map[i].key, key, key_len) == 0) {
            return c->prov_map[i].provenance;
        }
    }
    return NULL;
}

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
        /* local-derived ident OR local array (array→slice coercion) */
        if (arg->kind == NODE_IDENT) {
            Symbol *src = scope_lookup(c->current_scope,
                arg->ident.name, (uint32_t)arg->ident.name_len);
            if (src && src->is_local_derived)
                return true;
            /* local array passed as slice → points to stack */
            if (src && src->type && type_unwrap_distinct(src->type)->kind == TYPE_ARRAY) {
                bool is_global = scope_lookup_local(c->global_scope,
                    arg->ident.name, (uint32_t)arg->ident.name_len) != NULL;
                if (!src->is_static && !is_global) return true;
            }
        }
        /* nested call returning pointer — recurse */
        if (arg->kind == NODE_CALL) {
            Type *arg_type = typemap_get(c, arg);
            if (arg_type && type_unwrap_distinct(arg_type)->kind == TYPE_POINTER) {
                if (call_has_local_derived_arg(c, arg, depth + 1))
                    return true;
            }
        }
        /* orelse: identity(opt orelse &x) — check fallback for &local */
        if (arg->kind == NODE_ORELSE) {
            Node *fb = arg->orelse.fallback;
            /* check fallback for direct &local */
            if (fb && fb->kind == NODE_UNARY && fb->unary.op == TOK_AMP) {
                Node *root = fb->unary.operand;
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
            /* check fallback for local-derived ident */
            if (fb && fb->kind == NODE_IDENT) {
                Symbol *src = scope_lookup(c->current_scope,
                    fb->ident.name, (uint32_t)fb->ident.name_len);
                if (src && src->is_local_derived)
                    return true;
            }
            /* recurse into nested orelse: o1 orelse o2 orelse &x */
            if (fb && fb->kind == NODE_ORELSE) {
                /* treat the inner orelse as if it were the arg — re-check */
                Node *inner = fb;
                while (inner && inner->kind == NODE_ORELSE) {
                    Node *ifb = inner->orelse.fallback;
                    if (ifb && ifb->kind == NODE_UNARY && ifb->unary.op == TOK_AMP) {
                        Node *root = ifb->unary.operand;
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
                    if (ifb && ifb->kind == NODE_IDENT) {
                        Symbol *src = scope_lookup(c->current_scope,
                            ifb->ident.name, (uint32_t)ifb->ident.name_len);
                        if (src && src->is_local_derived)
                            return true;
                    }
                    inner = ifb;
                }
            }
        }
        /* @cstr(local,...) as arg — result points to local buffer */
        if (arg->kind == NODE_INTRINSIC && arg->intrinsic.name_len == 4 &&
            memcmp(arg->intrinsic.name, "cstr", 4) == 0 &&
            arg->intrinsic.arg_count > 0) {
            Node *buf = arg->intrinsic.args[0];
            while (buf && (buf->kind == NODE_FIELD || buf->kind == NODE_INDEX)) {
                if (buf->kind == NODE_FIELD) buf = buf->field.object;
                else buf = buf->index_expr.object;
            }
            if (buf && buf->kind == NODE_IDENT) {
                bool is_global = scope_lookup_local(c->global_scope,
                    buf->ident.name, (uint32_t)buf->ident.name_len) != NULL;
                Symbol *src = scope_lookup(c->current_scope,
                    buf->ident.name, (uint32_t)buf->ident.name_len);
                if (src && !src->is_static && !is_global) return true;
            }
        }
        /* struct field from call: wrap(&x).p — walk to call root, check args */
        {
            Node *froot = arg;
            while (froot && (froot->kind == NODE_FIELD || froot->kind == NODE_INDEX)) {
                if (froot->kind == NODE_FIELD) froot = froot->field.object;
                else froot = froot->index_expr.object;
            }
            if (froot && froot->kind == NODE_CALL && froot != arg) {
                if (call_has_local_derived_arg(c, froot, depth + 1))
                    return true;
            }
            /* field of local-derived struct: identity(h.p) where h is local-derived */
            if (froot && froot->kind == NODE_IDENT && froot != arg) {
                Symbol *src = scope_lookup(c->current_scope,
                    froot->ident.name, (uint32_t)froot->ident.name_len);
                if (src && src->is_local_derived)
                    return true;
            }
        }
    }
    return false;
}

/* ---- Unified escape flag helpers (prevents BUG-421 class) ---- */

/* Can this type carry a pointer? Only propagate escape flags to types that
 * can actually hold a reference to stack/arena memory. Scalar types (u32,
 * bool, enum, handle) cannot carry pointers — propagating flags to them
 * causes false positives (BUG-421). */
static bool type_can_carry_pointer(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    return eff && (eff->kind == TYPE_POINTER || eff->kind == TYPE_SLICE ||
                   eff->kind == TYPE_STRUCT || eff->kind == TYPE_UNION ||
                   eff->kind == TYPE_OPAQUE);
}

/* Propagate is_local_derived / is_arena_derived / is_from_arena from src to dst,
 * but ONLY if dst's type can carry a pointer. Centralizes the 3-flag propagation
 * pattern that was scattered at 5+ sites (4 of which were missing the type guard). */
static void propagate_escape_flags(Symbol *dst, Symbol *src, Type *dst_type) {
    if (!dst || !src || !type_can_carry_pointer(dst_type)) return;
    if (src->is_local_derived) dst->is_local_derived = true;
    if (src->is_arena_derived) dst->is_arena_derived = true;
    if (src->is_from_arena) dst->is_from_arena = true;
}

/* ---- ISR ban helper ---- */

/* Check if we're inside an interrupt handler and reject heap allocation.
 * Returns true if banned (error emitted). Centralizes the 4 ISR check sites. */
static bool check_isr_ban(Checker *c, int line, const char *method) {
    if (!c->in_interrupt) return false;
    checker_error(c, line,
        "%s not allowed in interrupt handler — "
        "malloc/calloc may deadlock. Use Pool(T, N) instead", method);
    return true;
}

/* ---- Designated init field validation ---- */
/* Validates a NODE_STRUCT_INIT against a target struct type.
 * Checks: all field names exist, all field value types match.
 * Used at 4 value-flow sites: var-decl, assignment, call arg, return. */
static bool validate_struct_init(Checker *c, Node *sinit, Type *target_type, int line) {
    Type *st = type_unwrap_distinct(target_type);
    if (st->kind != TYPE_STRUCT) {
        checker_error(c, line,
            "designated initializer requires struct type, got '%s'",
            type_name(target_type));
        return false;
    }
    for (int fi = 0; fi < sinit->struct_init.field_count; fi++) {
        DesigField *df = &sinit->struct_init.fields[fi];
        bool found = false;
        for (uint32_t si = 0; si < st->struct_type.field_count; si++) {
            if (st->struct_type.fields[si].name_len == (uint32_t)df->name_len &&
                memcmp(st->struct_type.fields[si].name, df->name, df->name_len) == 0) {
                found = true;
                Type *ft = st->struct_type.fields[si].type;
                Type *vt = checker_get_type(c, df->value);
                if (vt && ft && !type_equals(ft, vt) &&
                    !can_implicit_coerce(vt, ft) &&
                    !is_literal_compatible(df->value, ft)) {
                    checker_error(c, line,
                        "field '.%.*s' expects '%s', got '%s'",
                        (int)df->name_len, df->name,
                        type_name(ft), type_name(vt));
                }
                break;
            }
        }
        if (!found) {
            checker_error(c, line,
                "struct '%s' has no field '%.*s'",
                type_name(target_type), (int)df->name_len, df->name);
        }
    }
    return true;
}

/* ---- Auto-slab helper ---- */

/* Find or create an auto-Slab for a struct type (used by Task.new/new_ptr).
 * Returns the auto-slab Symbol. Deduplicates the 40-line creation block. */
static Symbol *find_or_create_auto_slab(Checker *c, Type *struct_type) {
    /* Check existing */
    for (int i = 0; i < c->auto_slab_count; i++) {
        if (type_equals(c->auto_slabs[i].elem_type, struct_type))
            return c->auto_slabs[i].slab_sym;
    }
    /* Create new */
    char slab_name[128];
    int sn_len = snprintf(slab_name, sizeof(slab_name),
        "_zer_auto_slab_%.*s",
        (int)struct_type->struct_type.name_len, struct_type->struct_type.name);
    if (sn_len >= (int)sizeof(slab_name)) sn_len = (int)sizeof(slab_name) - 1;
    Type *slab_type = (Type *)arena_alloc(c->arena, sizeof(Type));
    memset(slab_type, 0, sizeof(Type));
    slab_type->kind = TYPE_SLAB;
    slab_type->slab.elem = struct_type;
    char *name_copy = (char *)arena_alloc(c->arena, sn_len + 1);
    memcpy(name_copy, slab_name, sn_len + 1);
    Symbol *auto_sym = scope_add(c->arena, c->global_scope,
        name_copy, (uint32_t)sn_len, slab_type, 0, c->file_name);
    /* Register in auto_slabs array */
    if (c->auto_slab_count >= c->auto_slab_capacity) {
        int nc = c->auto_slab_capacity * 2;
        if (nc < 8) nc = 8;
        void *new_arr = arena_alloc(c->arena, nc * sizeof(c->auto_slabs[0]));
        if (c->auto_slab_count > 0)
            memcpy(new_arr, c->auto_slabs, c->auto_slab_count * sizeof(c->auto_slabs[0]));
        c->auto_slabs = new_arr;
        c->auto_slab_capacity = nc;
    }
    c->auto_slabs[c->auto_slab_count].elem_type = struct_type;
    c->auto_slabs[c->auto_slab_count].slab_sym = auto_sym;
    c->auto_slab_count++;
    return auto_sym;
}

/* ---- Volatile stripping check helper ---- */

/* Check if a cast/intrinsic strips volatile from source pointer.
 * Returns true if violation detected (error emitted). */
static bool check_volatile_strip(Checker *c, Node *src_expr, Type *src_type,
                                  Type *tgt_type, int line, const char *context) {
    Type *seff = type_unwrap_distinct(src_type);
    Type *teff = type_unwrap_distinct(tgt_type);
    if (!seff || seff->kind != TYPE_POINTER) return false;
    if (!teff || teff->kind != TYPE_POINTER) return false;
    if (teff->pointer.is_volatile) return false; /* target keeps volatile — ok */
    bool src_vol = seff->pointer.is_volatile;
    if (!src_vol && src_expr && src_expr->kind == NODE_IDENT) {
        Symbol *s = scope_lookup(c->current_scope,
            src_expr->ident.name, (uint32_t)src_expr->ident.name_len);
        if (s && s->is_volatile) src_vol = true;
    }
    if (src_vol) {
        checker_error(c, line,
            "%s cannot strip volatile qualifier — "
            "target must be volatile pointer", context);
        return true;
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
        if (type_unwrap_distinct(t->optional.inner)->kind == TYPE_VOID) return 1;
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

/* BUG-391: forward declarations for comptime evaluation in array sizes */
typedef struct {
    const char *name;
    uint32_t name_len;
    int64_t value;
    int64_t *array_values;  /* non-NULL for array bindings */
    int array_size;         /* element count (0 = scalar) */
} ComptimeParam;

/* Mutable comptime evaluation context — shared across block/loop boundaries.
 * Locals added in inner blocks are popped via saved_count on block exit.
 * Loop bodies share the same context (mutations persist across iterations). */
typedef struct {
    ComptimeParam stack[8];    /* stack-first small buffer */
    ComptimeParam *locals;     /* points to stack or malloc'd buffer */
    int count;
    int capacity;
} ComptimeCtx;

static void ct_ctx_init(ComptimeCtx *ctx, ComptimeParam *params, int param_count) {
    ctx->capacity = 8;
    ctx->locals = ctx->stack;
    ctx->count = 0;
    memset(ctx->stack, 0, sizeof(ctx->stack));
    if (param_count > 8) {
        ctx->capacity = param_count + 8;
        ctx->locals = (ComptimeParam *)malloc(ctx->capacity * sizeof(ComptimeParam));
        if (!ctx->locals) { ctx->locals = ctx->stack; ctx->capacity = 8; }
    }
    for (int i = 0; i < param_count && i < ctx->capacity; i++) {
        ctx->locals[i] = params[i];
        ctx->locals[i].array_values = NULL;
        ctx->locals[i].array_size = 0;
        ctx->count++;
    }
}

static void ct_ctx_free(ComptimeCtx *ctx) {
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->locals[i].array_values) free(ctx->locals[i].array_values);
    }
    if (ctx->locals != ctx->stack) free(ctx->locals);
}

static void ct_ctx_set(ComptimeCtx *ctx, const char *name, uint32_t name_len, int64_t value) {
    /* update existing */
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->locals[i].name_len == name_len &&
            memcmp(ctx->locals[i].name, name, name_len) == 0) {
            ctx->locals[i].value = value;
            return;
        }
    }
    /* add new */
    if (ctx->count >= ctx->capacity) {
        int nc = ctx->capacity * 2;
        ComptimeParam *nl = (ComptimeParam *)malloc(nc * sizeof(ComptimeParam));
        if (!nl) return;
        memcpy(nl, ctx->locals, ctx->count * sizeof(ComptimeParam));
        if (ctx->locals != ctx->stack) free(ctx->locals);
        ctx->locals = nl;
        ctx->capacity = nc;
    }
    ctx->locals[ctx->count].name = name;
    ctx->locals[ctx->count].name_len = name_len;
    ctx->locals[ctx->count].value = value;
    ctx->locals[ctx->count].array_values = NULL;
    ctx->locals[ctx->count].array_size = 0;
    ctx->count++;
}

static int64_t eval_comptime_block(Node *block, ComptimeCtx *ctx);
static Scope *_comptime_global_scope; /* set before eval for nested comptime calls */

/* Recursive TypeNode substitution: clone the TypeNode tree with type param T replaced.
 * Used by container monomorphization to handle arbitrarily nested T references:
 * ?*Container(T), *T, []T, T[N], etc. */
static TypeNode *subst_typenode(Arena *a, TypeNode *tn,
                                 const char *param_name, uint32_t param_len,
                                 TypeNode *replacement) {
    if (!tn) return NULL;
    /* Leaf: if this is the type param, return the replacement */
    if (tn->kind == TYNODE_NAMED &&
        tn->named.name_len == param_len &&
        memcmp(tn->named.name, param_name, param_len) == 0) {
        return replacement;
    }
    /* Recurse into wrappers */
    switch (tn->kind) {
    case TYNODE_POINTER: {
        TypeNode *r = (TypeNode *)arena_alloc(a, sizeof(TypeNode));
        *r = *tn;
        r->pointer.inner = subst_typenode(a, tn->pointer.inner, param_name, param_len, replacement);
        return r;
    }
    case TYNODE_OPTIONAL: {
        TypeNode *r = (TypeNode *)arena_alloc(a, sizeof(TypeNode));
        *r = *tn;
        r->optional.inner = subst_typenode(a, tn->optional.inner, param_name, param_len, replacement);
        return r;
    }
    case TYNODE_SLICE: {
        TypeNode *r = (TypeNode *)arena_alloc(a, sizeof(TypeNode));
        *r = *tn;
        r->slice.inner = subst_typenode(a, tn->slice.inner, param_name, param_len, replacement);
        return r;
    }
    case TYNODE_ARRAY: {
        TypeNode *r = (TypeNode *)arena_alloc(a, sizeof(TypeNode));
        *r = *tn;
        r->array.elem = subst_typenode(a, tn->array.elem, param_name, param_len, replacement);
        return r;
    }
    case TYNODE_CONST: case TYNODE_VOLATILE: {
        TypeNode *r = (TypeNode *)arena_alloc(a, sizeof(TypeNode));
        *r = *tn;
        r->qualified.inner = subst_typenode(a, tn->qualified.inner, param_name, param_len, replacement);
        return r;
    }
    case TYNODE_CONTAINER: {
        TypeNode *r = (TypeNode *)arena_alloc(a, sizeof(TypeNode));
        *r = *tn;
        r->container.type_arg = subst_typenode(a, tn->container.type_arg, param_name, param_len, replacement);
        return r;
    }
    default:
        return tn; /* no substitution needed (primitives, named non-param types) */
    }
}

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
    case TYNODE_ARENA:   return ty_arena;
    case TYNODE_BARRIER: return ty_barrier;

    case TYNODE_SEMAPHORE: {
        uint32_t count = 0;
        if (tn->semaphore.count_expr) {
            check_expr(c, tn->semaphore.count_expr);
            int64_t val = eval_const_expr(tn->semaphore.count_expr);
            if (val >= 0) count = (uint32_t)val;
            else checker_error(c, tn->loc.line, "Semaphore count must be a non-negative compile-time constant");
        }
        return type_semaphore(c->arena, count);
    }

    case TYNODE_POINTER: {
        Type *inner = resolve_type(c, tn->pointer.inner);
        /* BUG-372: *void is invalid — use *opaque for type-erased pointers.
         * BUG-506: unwrap distinct — distinct typedef void is still void. */
        if (inner && type_unwrap_distinct(inner)->kind == TYPE_VOID) {
            checker_error(c, tn->loc.line,
                "cannot create pointer to void — use '*opaque' for type-erased pointers");
        }
        return type_pointer(c->arena, inner);
    }

    case TYNODE_OPTIONAL: {
        Type *inner = resolve_type(c, tn->optional.inner);
        /* BUG-506: unwrap distinct — ?distinct(?T) is still nested optional */
        if (type_unwrap_distinct(inner)->kind == TYPE_OPTIONAL) {
            checker_error(c, tn->loc.line,
                "nested optional '??T' is not supported");
            return inner; /* return the inner ?T, not ??T */
        }
        return type_optional(c->arena, inner);
    }

    case TYNODE_SLICE: {
        Type *inner = resolve_type(c, tn->slice.inner);
        /* BUG-372: []void is invalid — void has no size.
         * BUG-506: unwrap distinct. */
        if (inner && type_unwrap_distinct(inner)->kind == TYPE_VOID) {
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
            /* BUG-391: comptime function call as array size.
             * eval_const_expr can't resolve function calls (no Checker access).
             * Try resolving NODE_CALL on comptime functions here. */
            if (val == CONST_EVAL_FAIL &&
                tn->array.size_expr->kind == NODE_CALL &&
                tn->array.size_expr->call.callee->kind == NODE_IDENT) {
                Symbol *csym = scope_lookup(c->current_scope,
                    tn->array.size_expr->call.callee->ident.name,
                    (uint32_t)tn->array.size_expr->call.callee->ident.name_len);
                if (csym && csym->is_comptime && csym->func_node) {
                    Node *fn = csym->func_node;
                    int pc = fn->func_decl.param_count;
                    ComptimeParam stack_cp[8];
                    memset(stack_cp, 0, sizeof(stack_cp));
                    ComptimeParam *cparams = pc <= 8 ? stack_cp :
                        (ComptimeParam *)arena_alloc(c->arena, pc * sizeof(ComptimeParam));
                    bool all_const = cparams != NULL;
                    for (int ci = 0; ci < tn->array.size_expr->call.arg_count &&
                         ci < pc && all_const; ci++) {
                        int64_t cv = eval_const_expr(tn->array.size_expr->call.args[ci]);
                        if (cv == CONST_EVAL_FAIL) { all_const = false; break; }
                        cparams[ci].name = fn->func_decl.params[ci].name;
                        cparams[ci].name_len = (uint32_t)fn->func_decl.params[ci].name_len;
                        cparams[ci].value = cv;
                    }
                    if (all_const && fn->func_decl.body) {
                        _comptime_global_scope = c->global_scope;
                        ComptimeCtx cctx;
                        ct_ctx_init(&cctx, cparams, pc);
                        val = eval_comptime_block(fn->func_decl.body, &cctx);
                        ct_ctx_free(&cctx);
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
            /* BUG-423: resolve comptime calls before eval */
            check_expr(c, tn->pool.count_expr);
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
            /* BUG-423: resolve comptime calls before eval */
            check_expr(c, tn->ring.count_expr);
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

    case TYNODE_CONTAINER: {
        /* Container instantiation: Stack(u32) → stamp concrete struct.
         * Depth limit prevents infinite recursion from self-referential containers. */
        static int _container_depth = 0;
        if (++_container_depth > 32) {
            _container_depth--;
            checker_error(c, tn->loc.line,
                "container instantiation depth exceeded (max 32) — "
                "possible infinite recursive container type");
            return ty_void;
        }
        const char *cname = tn->container.name;
        uint32_t cnlen = (uint32_t)tn->container.name_len;
        Type *concrete = resolve_type(c, tn->container.type_arg);

        /* Check cache first */
        for (int ci = 0; ci < c->container_inst_count; ci++) {
            if (c->container_instances[ci].tmpl_name_len == cnlen &&
                memcmp(c->container_instances[ci].tmpl_name, cname, cnlen) == 0 &&
                type_equals(c->container_instances[ci].concrete_type, concrete)) {
                return c->container_instances[ci].stamped_struct;
            }
        }

        /* Find template */
        struct ContainerTemplate *tmpl = NULL;
        for (int ci = 0; ci < c->container_tmpl_count; ci++) {
            if (c->container_templates[ci].name_len == cnlen &&
                memcmp(c->container_templates[ci].name, cname, cnlen) == 0) {
                tmpl = &c->container_templates[ci];
                break;
            }
        }
        if (!tmpl) {
            checker_error(c, tn->loc.line, "undefined container '%.*s'", (int)cnlen, cname);
            return ty_void;
        }

        /* Stamp: create TYPE_STRUCT with mangled name "Name_ConcreteType" */
        char mangled[256];
        const char *ctype_name = type_name(concrete);
        int mlen = snprintf(mangled, sizeof(mangled), "%.*s_%s", (int)cnlen, cname, ctype_name);
        if (mlen >= (int)sizeof(mangled)) mlen = (int)sizeof(mangled) - 1;
        char *mname = (char *)arena_alloc(c->arena, mlen + 1);
        memcpy(mname, mangled, mlen + 1);

        Type *st = (Type *)arena_alloc(c->arena, sizeof(Type));
        st->kind = TYPE_STRUCT;
        st->struct_type.name = mname;
        st->struct_type.name_len = (uint32_t)mlen;
        st->struct_type.is_packed = false;
        st->struct_type.is_shared = false;
        st->struct_type.is_shared_rw = false;
        st->struct_type.is_move = false;
        st->struct_type.field_count = (uint32_t)tmpl->field_count;
        st->struct_type.type_id = c->next_type_id++;
        st->struct_type.module_prefix = c->current_module;
        st->struct_type.module_prefix_len = c->current_module_len;

        /* Register in scope so field access works */
        add_symbol(c, mname, (uint32_t)mlen, st, tn->loc.line);

        /* Resolve fields with T substituted */
        if (tmpl->field_count > 0) {
            st->struct_type.fields = (SField *)arena_alloc(c->arena,
                tmpl->field_count * sizeof(SField));
            for (int fi = 0; fi < tmpl->field_count; fi++) {
                FieldDecl *fd = &tmpl->fields[fi];
                SField *sf = &st->struct_type.fields[fi];
                sf->name = fd->name;
                sf->name_len = (uint32_t)fd->name_len;
                sf->is_keep = false;
                sf->is_volatile = (fd->type && fd->type->kind == TYNODE_VOLATILE);
                /* Substitute type param T at any depth in the TypeNode tree.
                 * Recursive clone: replaces TYNODE_NAMED matching T with concrete type's TypeNode.
                 * Handles: T, *T, ?T, []T, T[N], ?*Container(T), etc. */
                sf->type = resolve_type(c, subst_typenode(c->arena, fd->type,
                    tmpl->type_param, tmpl->type_param_len, tn->container.type_arg));
            }
        } else {
            st->struct_type.fields = NULL;
        }

        /* Cache the instance */
        if (c->container_inst_count >= c->container_inst_capacity) {
            int nc = c->container_inst_capacity < 8 ? 8 : c->container_inst_capacity * 2;
            struct ContainerInstance *na = (struct ContainerInstance *)arena_alloc(c->arena,
                nc * sizeof(struct ContainerInstance));
            if (c->container_instances && c->container_inst_count > 0)
                memcpy(na, c->container_instances, c->container_inst_count * sizeof(struct ContainerInstance));
            c->container_instances = na;
            c->container_inst_capacity = nc;
        }
        struct ContainerInstance *ci = &c->container_instances[c->container_inst_count++];
        ci->tmpl_name = cname;
        ci->tmpl_name_len = cnlen;
        ci->concrete_type = concrete;
        ci->stamped_struct = st;

        _container_depth--;
        return st;
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
        /* propagate const through pointer/slice.
         * BUG-506: unwrap distinct — const distinct(*T) should propagate. */
        Type *inner_eff = type_unwrap_distinct(inner);
        if (inner_eff->kind == TYPE_POINTER) {
            return type_const_pointer(c->arena, inner_eff->pointer.inner);
        }
        if (inner_eff->kind == TYPE_SLICE) {
            return type_const_slice(c->arena, inner_eff->slice.inner);
        }
        /* for value types, const is a variable qualifier, not a type property */
        return inner;
    }

    case TYNODE_VOLATILE: {
        Type *inner = resolve_type(c, tn->qualified.inner);
        /* propagate volatile to pointer/slice type for codegen.
         * BUG-506: unwrap distinct. */
        {
        Type *iv = inner ? type_unwrap_distinct(inner) : NULL;
        if (iv && iv->kind == TYPE_POINTER) {
            Type *vp = type_pointer(c->arena, iv->pointer.inner);
            vp->pointer.is_volatile = true;
            if (iv->pointer.is_const) vp->pointer.is_const = true;
            return vp;
        }
        if (iv && iv->kind == TYPE_SLICE) {
            Type *vs = type_volatile_slice(c->arena, iv->slice.inner);
            if (iv->slice.is_const) vs->slice.is_const = true;
            return vs;
        }
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

/* ComptimeParam and eval_comptime_block forward-declared above resolve_type_inner (BUG-391) */

/* Forward declare for recursive comptime calls */
static int64_t eval_const_expr_subst(Node *n, ComptimeParam *params, int param_count);

/* Resolve a comptime NODE_CALL within eval_const_expr_subst.
 * Looks up the callee as a comptime function and recursively evaluates.
 * Uses _comptime_global_scope (declared above, line ~1082) and _comptime_call_depth. */
static int _comptime_call_depth = 0;  /* recursion guard for nested comptime calls */
static int64_t eval_comptime_call_subst(Node *call, ComptimeParam *outer_params, int outer_count) {
    if (!call || call->kind != NODE_CALL || !call->call.callee ||
        call->call.callee->kind != NODE_IDENT || !_comptime_global_scope)
        return CONST_EVAL_FAIL;
    if (_comptime_call_depth > 16) return CONST_EVAL_FAIL; /* prevent infinite recursion */
    _comptime_call_depth++;
    Symbol *sym = scope_lookup(_comptime_global_scope,
        call->call.callee->ident.name, (uint32_t)call->call.callee->ident.name_len);
    if (!sym || !sym->is_comptime || !sym->func_node) return CONST_EVAL_FAIL;
    Node *fn = sym->func_node;
    int pc = fn->func_decl.param_count;
    if (pc != call->call.arg_count) return CONST_EVAL_FAIL;
    ComptimeParam stack_cp[8];
    memset(stack_cp, 0, sizeof(stack_cp));
    ComptimeParam *cparams = pc <= 8 ? stack_cp :
        (ComptimeParam *)malloc(pc * sizeof(ComptimeParam));
    if (!cparams) return CONST_EVAL_FAIL;
    if (cparams != stack_cp) memset(cparams, 0, pc * sizeof(ComptimeParam));
    bool need_free = (cparams != stack_cp);
    for (int i = 0; i < pc; i++) {
        int64_t av = eval_const_expr_subst(call->call.args[i], outer_params, outer_count);
        if (av == CONST_EVAL_FAIL) { if (need_free) free(cparams); _comptime_call_depth--; return CONST_EVAL_FAIL; }
        cparams[i].name = fn->func_decl.params[i].name;
        cparams[i].name_len = (uint32_t)fn->func_decl.params[i].name_len;
        cparams[i].value = av;
    }
    ComptimeCtx cctx;
    ct_ctx_init(&cctx, cparams, pc);
    int64_t result = eval_comptime_block(fn->func_decl.body, &cctx);
    ct_ctx_free(&cctx);
    if (need_free) free(cparams);
    _comptime_call_depth--;
    return result;
}

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
    /* Array indexing: arr[i] — look up array binding */
    if (n->kind == NODE_INDEX && n->index_expr.object &&
        n->index_expr.object->kind == NODE_IDENT) {
        const char *aname = n->index_expr.object->ident.name;
        uint32_t alen = (uint32_t)n->index_expr.object->ident.name_len;
        for (int i = 0; i < param_count; i++) {
            if (params[i].name_len == alen &&
                memcmp(params[i].name, aname, alen) == 0 &&
                params[i].array_values && params[i].array_size > 0) {
                int64_t idx = eval_const_expr_subst(n->index_expr.index, params, param_count);
                if (idx == CONST_EVAL_FAIL || idx < 0 || idx >= params[i].array_size)
                    return CONST_EVAL_FAIL;
                return params[i].array_values[idx];
            }
        }
        return CONST_EVAL_FAIL;
    }
    /* Enum variant: State.idle → variant int value */
    if (n->kind == NODE_FIELD && n->field.object &&
        n->field.object->kind == NODE_IDENT && _comptime_global_scope) {
        Symbol *esym = scope_lookup(_comptime_global_scope,
            n->field.object->ident.name, (uint32_t)n->field.object->ident.name_len);
        /* BUG-506: unwrap distinct for enum variant resolution */
        Type *esym_eff = (esym && esym->type) ? type_unwrap_distinct(esym->type) : NULL;
        if (esym_eff && esym_eff->kind == TYPE_ENUM) {
            const char *vname = n->field.field_name;
            uint32_t vlen = (uint32_t)n->field.field_name_len;
            for (uint32_t i = 0; i < esym_eff->enum_type.variant_count; i++) {
                SEVariant *v = &esym_eff->enum_type.variants[i];
                if (v->name_len == vlen && memcmp(v->name, vname, vlen) == 0)
                    return (int64_t)v->value;
            }
        }
    }
    if (n->kind == NODE_INT_LIT) return (int64_t)n->int_lit.value;
    if (n->kind == NODE_BOOL_LIT) return n->bool_lit.value ? 1 : 0;
    /* nested comptime function calls — depth guard prevents stack overflow */
    if (n->kind == NODE_CALL) {
        static int _subst_depth = 0;
        if (++_subst_depth > 32) { _subst_depth--; return CONST_EVAL_FAIL; }
        int64_t r = eval_comptime_call_subst(n, params, param_count);
        _subst_depth--;
        return r;
    }
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


/* Evaluate a comptime assignment: compute RHS, apply operator, update ctx */
static int64_t ct_eval_assign(ComptimeCtx *ctx, Node *asgn) {
    if (!asgn || asgn->kind != NODE_ASSIGN) return CONST_EVAL_FAIL;

    /* Array element assignment: arr[i] = val */
    if (asgn->assign.target->kind == NODE_INDEX &&
        asgn->assign.target->index_expr.object->kind == NODE_IDENT) {
        const char *aname = asgn->assign.target->index_expr.object->ident.name;
        uint32_t alen = (uint32_t)asgn->assign.target->index_expr.object->ident.name_len;
        int64_t idx = eval_const_expr_subst(asgn->assign.target->index_expr.index,
                                             ctx->locals, ctx->count);
        int64_t rhs = eval_const_expr_subst(asgn->assign.value, ctx->locals, ctx->count);
        if (idx == CONST_EVAL_FAIL || rhs == CONST_EVAL_FAIL) return CONST_EVAL_FAIL;
        for (int k = 0; k < ctx->count; k++) {
            if (ctx->locals[k].name_len == alen &&
                memcmp(ctx->locals[k].name, aname, alen) == 0 &&
                ctx->locals[k].array_values && idx >= 0 && idx < ctx->locals[k].array_size) {
                ctx->locals[k].array_values[idx] = rhs;
                return 0;
            }
        }
        return CONST_EVAL_FAIL;
    }

    if (asgn->assign.target->kind != NODE_IDENT)
        return CONST_EVAL_FAIL;
    const char *name = asgn->assign.target->ident.name;
    uint32_t nlen = (uint32_t)asgn->assign.target->ident.name_len;
    int64_t rhs = eval_const_expr_subst(asgn->assign.value, ctx->locals, ctx->count);
    if (rhs == CONST_EVAL_FAIL) return CONST_EVAL_FAIL;
    /* find current value for compound assign */
    int64_t cur = 0;
    for (int k = 0; k < ctx->count; k++) {
        if (ctx->locals[k].name_len == nlen && memcmp(ctx->locals[k].name, name, nlen) == 0) {
            cur = ctx->locals[k].value; break;
        }
    }
    int64_t newval;
    switch (asgn->assign.op) {
    case TOK_EQ:        newval = rhs; break;
    case TOK_PLUSEQ:    newval = cur + rhs; break;
    case TOK_MINUSEQ:   newval = cur - rhs; break;
    case TOK_STAREQ:    newval = cur * rhs; break;
    case TOK_SLASHEQ:   newval = rhs ? cur / rhs : 0; break;
    case TOK_PERCENTEQ: newval = rhs ? cur % rhs : 0; break;
    case TOK_LSHIFTEQ:  newval = (rhs >= 0 && rhs < 64) ? (int64_t)((uint64_t)cur << rhs) : 0; break;
    case TOK_RSHIFTEQ:  newval = (rhs >= 0 && rhs < 64) ? cur >> rhs : 0; break;
    case TOK_AMPEQ:     newval = cur & rhs; break;
    case TOK_PIPEEQ:    newval = cur | rhs; break;
    case TOK_CARETEQ:   newval = cur ^ rhs; break;
    default: return CONST_EVAL_FAIL;
    }
    ct_ctx_set(ctx, name, nlen, newval);
    return 0; /* success (not a return value) */
}

/* Evaluate a comptime float expression with parameter substitution.
 * Returns NAN on failure. Handles: float literals, +, -, *, /, param refs. */
static double eval_comptime_float_expr(Node *n, ComptimeParam *params, int param_count) {
    if (!n) return NAN;
    if (n->kind == NODE_FLOAT_LIT) return n->float_lit.value;
    if (n->kind == NODE_INT_LIT) return (double)n->int_lit.value;
    if (n->kind == NODE_IDENT) {
        for (int i = 0; i < param_count; i++) {
            if (n->ident.name_len == params[i].name_len &&
                memcmp(n->ident.name, params[i].name, params[i].name_len) == 0) {
                /* Params are int64 — cast to double (float params passed as bits) */
                double d;
                memcpy(&d, &params[i].value, sizeof(d));
                return d;
            }
        }
        return NAN;
    }
    if (n->kind == NODE_UNARY && n->unary.op == TOK_MINUS) {
        double v = eval_comptime_float_expr(n->unary.operand, params, param_count);
        return isnan(v) ? NAN : -v;
    }
    if (n->kind == NODE_BINARY) {
        double l = eval_comptime_float_expr(n->binary.left, params, param_count);
        double r = eval_comptime_float_expr(n->binary.right, params, param_count);
        if (isnan(l) || isnan(r)) return NAN;
        switch (n->binary.op) {
        case TOK_PLUS:  return l + r;
        case TOK_MINUS: return l - r;
        case TOK_STAR:  return l * r;
        case TOK_SLASH: return r != 0.0 ? l / r : NAN;
        default: return NAN;
        }
    }
    return NAN;
}

/* Find the return expression in a comptime function body (for float/struct eval). */
static Node *find_comptime_return_expr(Node *block) {
    if (!block) return NULL;
    if (block->kind == NODE_RETURN && block->ret.expr)
        return block->ret.expr;
    if (block->kind == NODE_BLOCK) {
        for (int i = 0; i < block->block.stmt_count; i++) {
            Node *r = find_comptime_return_expr(block->block.stmts[i]);
            if (r) return r;
        }
    }
    if (block->kind == NODE_IF) {
        Node *r = find_comptime_return_expr(block->if_stmt.then_body);
        if (r) return r;
        return find_comptime_return_expr(block->if_stmt.else_body);
    }
    return NULL;
}


/* Evaluate a comptime struct return: evaluate each field value in the
 * NODE_STRUCT_INIT, produce a new NODE_STRUCT_INIT with constant field values.
 * Returns arena-allocated NODE_STRUCT_INIT with NODE_INT_LIT values, or NULL. */
static Node *eval_comptime_struct_return(Arena *arena, Node *struct_init,
                                          ComptimeParam *params, int param_count) {
    if (!struct_init || struct_init->kind != NODE_STRUCT_INIT) return NULL;
    int fc = struct_init->struct_init.field_count;
    if (fc == 0) return NULL;

    DesigField *fields = (DesigField *)arena_alloc(arena, fc * sizeof(DesigField));
    if (!fields) return NULL;

    for (int i = 0; i < fc; i++) {
        DesigField *src = &struct_init->struct_init.fields[i];
        fields[i].name = src->name;
        fields[i].name_len = src->name_len;
        int64_t val = eval_const_expr_subst(src->value, params, param_count);
        if (val == CONST_EVAL_FAIL) return NULL;
        /* Create constant NODE_INT_LIT for the evaluated value */
        Node *lit = (Node *)arena_alloc(arena, sizeof(Node));
        memset(lit, 0, sizeof(Node));
        lit->kind = NODE_INT_LIT;
        lit->int_lit.value = (uint64_t)val;
        fields[i].value = lit;
    }

    Node *result = (Node *)arena_alloc(arena, sizeof(Node));
    memset(result, 0, sizeof(Node));
    result->kind = NODE_STRUCT_INIT;
    result->struct_init.fields = fields;
    result->struct_init.field_count = fc;
    return result;
}

/* Create an array binding in comptime context */
static void ct_ctx_set_array(ComptimeCtx *ctx, const char *name, uint32_t name_len,
                              int64_t *values, int size) {
    /* grow if needed */
    if (ctx->count >= ctx->capacity) {
        int nc = ctx->capacity * 2;
        ComptimeParam *nl = (ComptimeParam *)malloc(nc * sizeof(ComptimeParam));
        if (!nl) return;
        memcpy(nl, ctx->locals, ctx->count * sizeof(ComptimeParam));
        if (ctx->locals != ctx->stack) free(ctx->locals);
        ctx->locals = nl;
        ctx->capacity = nc;
    }
    ctx->locals[ctx->count].name = name;
    ctx->locals[ctx->count].name_len = name_len;
    ctx->locals[ctx->count].value = 0;
    ctx->locals[ctx->count].array_values = values;
    ctx->locals[ctx->count].array_size = size;
    ctx->count++;
}

static int64_t eval_comptime_block(Node *block, ComptimeCtx *ctx) {
    static int depth = 0;
    static int64_t _comptime_ops = 0;  /* global instruction budget */
    if (!block) return CONST_EVAL_FAIL;
    if (depth++ > 32) { depth--; return CONST_EVAL_FAIL; }
    if (depth == 1) _comptime_ops = 0;  /* reset on top-level call */

    /* Save count for block scoping — locals added inside are popped on exit.
     * Loop bodies do NOT save/restore (mutations must persist across iterations). */
    int saved_count = ctx->count;
    int64_t result = CONST_EVAL_FAIL;

    if (block->kind == NODE_BLOCK) {
        for (int i = 0; i < block->block.stmt_count; i++) {
            Node *stmt = block->block.stmts[i];

            /* Variable declaration */
            if (stmt->kind == NODE_VAR_DECL) {
                /* Array var-decl: T[N] arr; → create zero-filled array binding */
                if (stmt->var_decl.type && stmt->var_decl.type->kind == TYNODE_ARRAY) {
                    int64_t sz = eval_const_expr(stmt->var_decl.type->array.size_expr);
                    if (sz > 0 && sz <= 1024) {
                        int64_t *arr = (int64_t *)calloc((size_t)sz, sizeof(int64_t));
                        if (arr) {
                            ct_ctx_set_array(ctx, stmt->var_decl.name,
                                (uint32_t)stmt->var_decl.name_len, arr, (int)sz);
                        }
                    }
                    continue;
                }
                if (stmt->var_decl.init) {
                    int64_t val = eval_const_expr_subst(stmt->var_decl.init, ctx->locals, ctx->count);
                    if (val == CONST_EVAL_FAIL) { goto ct_done; }
                    ct_ctx_set(ctx, stmt->var_decl.name, (uint32_t)stmt->var_decl.name_len, val);
                }
                continue;
            }

            /* Assignment */
            if (stmt->kind == NODE_EXPR_STMT && stmt->expr_stmt.expr &&
                stmt->expr_stmt.expr->kind == NODE_ASSIGN) {
                if (ct_eval_assign(ctx, stmt->expr_stmt.expr) == CONST_EVAL_FAIL)
                    { goto ct_done; }
                continue;
            }

            /* For loop */
            if (stmt->kind == NODE_FOR) {
                if (stmt->for_stmt.init && stmt->for_stmt.init->kind == NODE_VAR_DECL &&
                    stmt->for_stmt.init->var_decl.init) {
                    int64_t val = eval_const_expr_subst(stmt->for_stmt.init->var_decl.init, ctx->locals, ctx->count);
                    if (val == CONST_EVAL_FAIL) { goto ct_done; }
                    ct_ctx_set(ctx, stmt->for_stmt.init->var_decl.name,
                        (uint32_t)stmt->for_stmt.init->var_decl.name_len, val);
                }
                for (int iter = 0; iter < 10000; iter++) {
                    if (++_comptime_ops > 1000000) { goto ct_done; }
                    if (stmt->for_stmt.cond) {
                        int64_t cond = eval_const_expr_subst(stmt->for_stmt.cond, ctx->locals, ctx->count);
                        if (cond == CONST_EVAL_FAIL || !cond) break;
                    }
                    /* body — recursive call shares ctx (mutations persist) */
                    int64_t r = eval_comptime_block(stmt->for_stmt.body, ctx);
                    if (r != CONST_EVAL_FAIL) { result = r; goto ct_done; }
                    /* step */
                    if (stmt->for_stmt.step && stmt->for_stmt.step->kind == NODE_ASSIGN) {
                        if (ct_eval_assign(ctx, stmt->for_stmt.step) == CONST_EVAL_FAIL)
                            { goto ct_done; }
                    }
                }
                continue;
            }

            /* While loop */
            if (stmt->kind == NODE_WHILE || stmt->kind == NODE_DO_WHILE) {
                for (int iter = 0; iter < 10000; iter++) {
                    if (++_comptime_ops > 1000000) { goto ct_done; }
                    /* do-while: execute body before checking condition on first iteration */
                    if (stmt->kind == NODE_DO_WHILE && iter == 0) {
                        int64_t r = eval_comptime_block(stmt->while_stmt.body, ctx);
                        if (r != CONST_EVAL_FAIL) { result = r; goto ct_done; }
                    }
                    int64_t cond = eval_const_expr_subst(stmt->while_stmt.cond, ctx->locals, ctx->count);
                    if (cond == CONST_EVAL_FAIL || !cond) break;
                    if (stmt->kind == NODE_WHILE || iter > 0) {
                        int64_t r = eval_comptime_block(stmt->while_stmt.body, ctx);
                        if (r != CONST_EVAL_FAIL) { result = r; goto ct_done; }
                    }
                }
                continue;
            }

            /* Switch */
            if (stmt->kind == NODE_SWITCH) {
                int64_t sw_val = eval_const_expr_subst(stmt->switch_stmt.expr, ctx->locals, ctx->count);
                if (sw_val != CONST_EVAL_FAIL) {
                    bool matched = false;
                    for (int ai = 0; ai < stmt->switch_stmt.arm_count; ai++) {
                        SwitchArm *arm = &stmt->switch_stmt.arms[ai];
                        if (arm->is_default) {
                            int64_t r = eval_comptime_block(arm->body, ctx);
                            if (r != CONST_EVAL_FAIL) { result = r; goto ct_done; }
                            matched = true; break;
                        }
                        for (int vi = 0; vi < arm->value_count; vi++) {
                            int64_t arm_val = eval_const_expr_subst(arm->values[vi], ctx->locals, ctx->count);
                            if (arm_val != CONST_EVAL_FAIL && arm_val == sw_val) {
                                int64_t r = eval_comptime_block(arm->body, ctx);
                                if (r != CONST_EVAL_FAIL) { result = r; goto ct_done; }
                                matched = true; break;
                            }
                        }
                        if (matched) break;
                    }
                }
                continue;
            }

            /* Return */
            if (stmt->kind == NODE_RETURN && stmt->ret.expr) {
                result = eval_const_expr_subst(stmt->ret.expr, ctx->locals, ctx->count);
                break;
            }

            /* If/else */
            if (stmt->kind == NODE_IF) {
                int64_t cond = eval_const_expr_subst(stmt->if_stmt.cond, ctx->locals, ctx->count);
                if (cond != CONST_EVAL_FAIL) {
                    if (cond) {
                        int64_t r = eval_comptime_block(stmt->if_stmt.then_body, ctx);
                        if (r != CONST_EVAL_FAIL) { result = r; break; }
                    } else if (stmt->if_stmt.else_body) {
                        int64_t r = eval_comptime_block(stmt->if_stmt.else_body, ctx);
                        if (r != CONST_EVAL_FAIL) { result = r; break; }
                    }
                }
                continue;
            }

            /* Nested block */
            if (stmt->kind == NODE_BLOCK) {
                int64_t r = eval_comptime_block(stmt, ctx);
                if (r != CONST_EVAL_FAIL) { result = r; break; }
                continue;
            }
        }
    } else if (block->kind == NODE_RETURN && block->ret.expr) {
        result = eval_const_expr_subst(block->ret.expr, ctx->locals, ctx->count);
    } else if (block->kind == NODE_IF) {
        int64_t cond = eval_const_expr_subst(block->if_stmt.cond, ctx->locals, ctx->count);
        if (cond != CONST_EVAL_FAIL) {
            if (cond) result = eval_comptime_block(block->if_stmt.then_body, ctx);
            else if (block->if_stmt.else_body) result = eval_comptime_block(block->if_stmt.else_body, ctx);
        }
    }
ct_done:
    /* Free array bindings before popping block-local vars */
    for (int pi = saved_count; pi < ctx->count; pi++) {
        if (ctx->locals[pi].array_values) {
            free(ctx->locals[pi].array_values);
            ctx->locals[pi].array_values = NULL;
        }
    }
    ctx->count = saved_count; /* pop block-local vars */
    depth--;
    return result;
}

/* Const ident resolver callback for eval_const_expr_ex.
 * Looks up const symbols via scope chain, recursively evaluates init value.
 * BUG-430: enables const u32 perms = ...; comptime if (FUNC(perms)) pattern.
 * Uses eval_const_expr_ex with itself as callback — zero code duplication. */
static int64_t resolve_const_ident(void *ctx, const char *name, uint32_t name_len) {
    Checker *c = (Checker *)ctx;
    Symbol *sym = scope_lookup(c->current_scope, name, name_len);
    if (!sym) sym = scope_lookup(c->global_scope, name, name_len);
    if (sym && sym->is_const && sym->func_node) {
        Node *init = (sym->func_node->kind == NODE_VAR_DECL ||
                      sym->func_node->kind == NODE_GLOBAL_VAR)
                     ? sym->func_node->var_decl.init : NULL;
        if (init) return eval_const_expr_ex(init, 0, resolve_const_ident, ctx);
    }
    return CONST_EVAL_FAIL;
}

/* Resolve enum variant: State.idle → int value. Returns CONST_EVAL_FAIL if not an enum field. */
static int64_t resolve_enum_field(Checker *c, Node *n) {
    if (!n || n->kind != NODE_FIELD || !n->field.object ||
        n->field.object->kind != NODE_IDENT) return CONST_EVAL_FAIL;
    Symbol *esym = scope_lookup(c->current_scope,
        n->field.object->ident.name, (uint32_t)n->field.object->ident.name_len);
    if (!esym) esym = scope_lookup(c->global_scope,
        n->field.object->ident.name, (uint32_t)n->field.object->ident.name_len);
    if (!esym || !esym->type || esym->type->kind != TYPE_ENUM) return CONST_EVAL_FAIL;
    uint32_t vlen = (uint32_t)n->field.field_name_len;
    for (uint32_t i = 0; i < esym->type->enum_type.variant_count; i++) {
        SEVariant *v = &esym->type->enum_type.variants[i];
        if (v->name_len == vlen && memcmp(v->name, n->field.field_name, vlen) == 0)
            return (int64_t)v->value;
    }
    return CONST_EVAL_FAIL;
}

/* Evaluate constant expression with scope access for const symbol lookup.
 * Also resolves enum variant dot access: State.idle → variant int value.
 * Handles binary/unary expressions containing enum fields by recursing. */
static int64_t eval_const_expr_scoped(Checker *c, Node *n) {
    if (!n) return CONST_EVAL_FAIL;
    /* Enum field: State.idle → int */
    if (n->kind == NODE_FIELD) {
        int64_t ev = resolve_enum_field(c, n);
        if (ev != CONST_EVAL_FAIL) return ev;
    }
    /* Binary: recurse with enum support (eval_const_expr_ex can't see enum fields) */
    if (n->kind == NODE_BINARY) {
        int64_t l = eval_const_expr_scoped(c, n->binary.left);
        int64_t r = eval_const_expr_scoped(c, n->binary.right);
        if (l != CONST_EVAL_FAIL && r != CONST_EVAL_FAIL) {
            /* Delegate to eval_const_expr_ex binary handler by creating temp */
            Node tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.kind = NODE_BINARY;
            tmp.binary.op = n->binary.op;
            Node lit_l, lit_r;
            memset(&lit_l, 0, sizeof(lit_l));
            memset(&lit_r, 0, sizeof(lit_r));
            lit_l.kind = NODE_INT_LIT; lit_l.int_lit.value = (uint64_t)l;
            lit_r.kind = NODE_INT_LIT; lit_r.int_lit.value = (uint64_t)r;
            tmp.binary.left = &lit_l;
            tmp.binary.right = &lit_r;
            return eval_const_expr_ex(&tmp, 0, NULL, NULL);
        }
    }
    return eval_const_expr_ex(n, 0, resolve_const_ident, c);
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
        /* interrupt safety: track global variable access from ISR vs regular code */
        if (sym && !sym->is_function && c->current_func_ret != NULL) {
            Symbol *gs = scope_lookup(c->global_scope, node->ident.name,
                                      (uint32_t)node->ident.name_len);
            if (gs && gs == sym) {
                /* this ident IS a global — track ISR/func access */
                track_isr_global(c, node->ident.name,
                                 (uint32_t)node->ident.name_len, false);
            }
        }
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
                    /* range propagation: mark proven if divisor is nonzero.
                     * Handles both simple idents (d) and struct fields (cfg.d). */
                    /* if eval_const_expr didn't resolve, try const symbol lookup */
                    if (div_val == CONST_EVAL_FAIL && node->binary.right->kind == NODE_IDENT) {
                        Symbol *dsym = scope_lookup(c->current_scope,
                            node->binary.right->ident.name,
                            (uint32_t)node->binary.right->ident.name_len);
                        if (dsym && dsym->is_const && dsym->func_node) {
                            Node *init = NULL;
                            if (dsym->func_node->kind == NODE_GLOBAL_VAR)
                                init = dsym->func_node->var_decl.init;
                            else if (dsym->func_node->kind == NODE_VAR_DECL)
                                init = dsym->func_node->var_decl.init;
                            if (init) div_val = eval_const_expr(init);
                        }
                    }
                    if (div_val != CONST_EVAL_FAIL && div_val != 0) {
                        mark_proven(c, node); /* constant nonzero divisor */
                    } else {
                        /* try ident lookup first, then compound key for struct fields */
                        ExprKey dkey = {NULL, 0};
                        if (node->binary.right->kind == NODE_IDENT) {
                            dkey.str = node->binary.right->ident.name;
                            dkey.len = (int)node->binary.right->ident.name_len;
                        } else {
                            dkey = build_expr_key_a(c, node->binary.right);
                        }
                        if (dkey.len > 0) {
                            struct VarRange *r = find_var_range(c, dkey.str, (uint32_t)dkey.len);
                            if (r && (r->known_nonzero || r->min_val > 0)) {
                                mark_proven(c, node);
                            }
                        }
                    }
                    /* Forced division guard: if divisor is ident, struct field, or
                     * function call and not proven nonzero → compile error.
                     * Covers all cases: variables, struct fields, function returns. */
                    if (div_val != 0 && !checker_is_proven(c, node)) {
                        if (node->binary.right->kind == NODE_IDENT ||
                            node->binary.right->kind == NODE_FIELD) {
                            ExprKey dname = build_expr_key_a(c, node->binary.right);
                            if (dname.len > 0) {
                                checker_error(c, node->loc.line,
                                    "divisor '%.*s' not proven nonzero — add 'if (%.*s == 0) { return; }' before division",
                                    dname.len, dname.str, dname.len, dname.str);
                            }
                        } else if (node->binary.right->kind == NODE_CALL) {
                            /* Function call as divisor: check if callee has proven return range */
                            bool call_proven = false;
                            if (node->binary.right->call.callee &&
                                node->binary.right->call.callee->kind == NODE_IDENT) {
                                Symbol *csym = scope_lookup(c->current_scope,
                                    node->binary.right->call.callee->ident.name,
                                    (uint32_t)node->binary.right->call.callee->ident.name_len);
                                if (csym && csym->has_return_range && csym->return_range_min > 0)
                                    call_proven = true;
                            }
                            if (!call_proven) {
                                checker_error(c, node->loc.line,
                                    "divisor from function call not proven nonzero — "
                                    "store result in variable and add 'if (d == 0) { return; }' guard");
                            }
                        }
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
            /* BUG-426: allow ! on integers (not just bool) for comptime if
             * patterns like `comptime if (!FEATURE())`. Common C idiom.
             * Result is always bool. */
            if (!type_equals(operand, ty_bool) && !type_is_integer(operand)) {
                checker_error(c, node->loc.line,
                    "'!' requires bool or integer, got '%s'", type_name(operand));
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
            /* BUG-410: unwrap distinct — distinct typedef *T is still a pointer */
            {
            Type *deref_inner = type_unwrap_distinct(operand);
            if (deref_inner->kind != TYPE_POINTER) {
                checker_error(c, node->loc.line,
                    "cannot dereference non-pointer type '%s'", type_name(operand));
                result = ty_void;
            } else {
                result = deref_inner->pointer.inner;
            }
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
                    /* BUG-479: mark variable as address-taken in VRP.
                     * Once &var exists ANYWHERE, VRP range is permanently
                     * unreliable — pointer writes can change value at any time.
                     * Covers ALL paths: var-decl init, assignment, call args,
                     * struct field store, return. Single check point = 100%. */
                    {
                        struct VarRange *r = find_var_range(c,
                            root->ident.name, (uint32_t)root->ident.name_len);
                        if (r) {
                            r->min_val = INT64_MIN;
                            r->max_val = INT64_MAX;
                            r->known_nonzero = false;
                            r->address_taken = true;
                        } else {
                            push_var_range(c, root->ident.name,
                                (uint32_t)root->ident.name_len,
                                INT64_MIN, INT64_MAX, false);
                            r = find_var_range(c, root->ident.name,
                                (uint32_t)root->ident.name_len);
                            if (r) r->address_taken = true;
                        }
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
                    /* shared struct: ban &s.field — pointer would bypass auto-locking */
                    if (sym && sym->type) {
                        Type *st = type_unwrap_distinct(sym->type);
                        if (st->kind == TYPE_STRUCT && st->struct_type.is_shared &&
                            node->unary.operand->kind == NODE_FIELD) {
                            checker_error(c, node->loc.line,
                                "cannot take address of shared struct field — "
                                "pointer would bypass auto-locking");
                        }
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

        /* BUG-487: union variant assignment may overwrite move struct.
         * m.id = 100 when m.k (move struct variant) might be active → resource leak.
         * Only fires for unions that contain at least one move struct variant. */
        if (node->assign.target->kind == NODE_FIELD) {
            Node *uobj = node->assign.target->field.object;
            if (uobj && uobj->kind == NODE_IDENT) {
                Symbol *usym = scope_lookup(c->current_scope, uobj->ident.name,
                    (uint32_t)uobj->ident.name_len);
                if (usym && usym->type) {
                    Type *ueff = type_unwrap_distinct(usym->type);
                    if (ueff && ueff->kind == TYPE_UNION) {
                        /* check if ANY variant is a move struct */
                        bool has_move_variant = false;
                        for (uint32_t vi = 0; vi < ueff->union_type.variant_count; vi++) {
                            Type *vt = type_unwrap_distinct(ueff->union_type.variants[vi].type);
                            if (vt && vt->kind == TYPE_STRUCT && vt->struct_type.is_move) {
                                has_move_variant = true;
                                break;
                            }
                        }
                        if (has_move_variant) {
                            checker_error(c, node->loc.line,
                                "cannot assign to variant of union containing move struct — "
                                "previous variant's resource may be leaked. "
                                "Use switch to safely read+destroy before changing variant");
                        }
                    }
                }
            }
        }

        /* BUG-496: Arena value escape to global — Arena.over(local_buf) stores
         * a pointer to stack memory in the Arena struct. Storing that Arena
         * VALUE in a global means the buf pointer dangles after function returns.
         * Only reject when the Arena VALUE comes from a LOCAL variable
         * (Arena.over(stack_buf)). Global Arena = global buffer = safe. */
        if (node->assign.op == TOK_EQ && value) {
            Type *val_eff = type_unwrap_distinct(value);
            bool has_arena = (val_eff && val_eff->kind == TYPE_ARENA);
            if (!has_arena && val_eff && val_eff->kind == TYPE_STRUCT) {
                for (uint32_t fi = 0; fi < val_eff->struct_type.field_count; fi++) {
                    Type *ft = type_unwrap_distinct(val_eff->struct_type.fields[fi].type);
                    if (ft && ft->kind == TYPE_ARENA) { has_arena = true; break; }
                }
            }
            if (has_arena) {
                /* Check if value source is LOCAL (not global/static) */
                Node *vroot = node->assign.value;
                while (vroot && (vroot->kind == NODE_FIELD || vroot->kind == NODE_INDEX)) {
                    if (vroot->kind == NODE_FIELD) vroot = vroot->field.object;
                    else vroot = vroot->index_expr.object;
                }
                bool val_is_local = false;
                if (vroot && vroot->kind == NODE_IDENT) {
                    Symbol *vsym = scope_lookup(c->current_scope,
                        vroot->ident.name, (uint32_t)vroot->ident.name_len);
                    val_is_local = vsym && !vsym->is_static &&
                        !scope_lookup_local(c->global_scope, vsym->name, vsym->name_len);
                }
                if (val_is_local) {
                    /* Check if target is global/static */
                    Node *troot = node->assign.target;
                    while (troot && (troot->kind == NODE_FIELD || troot->kind == NODE_INDEX)) {
                        if (troot->kind == NODE_FIELD) troot = troot->field.object;
                        else troot = troot->index_expr.object;
                    }
                    if (troot && troot->kind == NODE_IDENT) {
                        Symbol *tsym = scope_lookup(c->current_scope,
                            troot->ident.name, (uint32_t)troot->ident.name_len);
                        bool target_is_global = tsym &&
                            (tsym->is_static || scope_lookup_local(c->global_scope,
                                tsym->name, tsym->name_len) != NULL);
                        if (target_is_global) {
                            checker_error(c, node->loc.line,
                                "cannot store local Arena value in global/static — "
                                "Arena.over(local_buf) contains pointer to stack memory "
                                "that will dangle when function returns");
                        }
                    }
                }
            }
        }

        /* interrupt safety: track compound assignment (|=, +=, etc.) on globals.
         * Walk field/index chains to root ident (catches g_state.flags |= 1). */
        if (node->assign.op != TOK_EQ) {
            Node *isr_root = node->assign.target;
            while (isr_root && (isr_root->kind == NODE_FIELD ||
                                isr_root->kind == NODE_INDEX ||
                                (isr_root->kind == NODE_UNARY && isr_root->unary.op == TOK_STAR))) {
                if (isr_root->kind == NODE_FIELD) isr_root = isr_root->field.object;
                else if (isr_root->kind == NODE_INDEX) isr_root = isr_root->index_expr.object;
                else isr_root = isr_root->unary.operand;
            }
            if (isr_root && isr_root->kind == NODE_IDENT) {
                Symbol *gs = scope_lookup(c->global_scope, isr_root->ident.name,
                                          (uint32_t)isr_root->ident.name_len);
                if (gs && !gs->is_function) {
                    track_isr_global(c, isr_root->ident.name,
                                     (uint32_t)isr_root->ident.name_len, true);
                }
            }
        }

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
                /* BUG-392: if we have a precise key (e.g. "msgs[0]"), compare
                 * against target's full key. Different array elements are safe. */
                bool blocked = true;
                if (c->union_switch_key && c->union_switch_key_len > 0 &&
                    !locked_via_alias) {
                    ExprKey tgt_key = build_expr_key_a(c, node->assign.target);
                    if (tgt_key.len > 0) {
                        /* strip the trailing .field from target key for comparison.
                         * msgs[1].data → compare "msgs[1]" against "msgs[0]" */
                        const char *last_dot = NULL;
                        for (int di = tgt_key.len - 1; di >= 0; di--) {
                            if (tgt_key.str[di] == '.') { last_dot = &tgt_key.str[di]; break; }
                        }
                        int cmp_len = last_dot ? (int)(last_dot - tgt_key.str) : tgt_key.len;
                        if (cmp_len != (int)c->union_switch_key_len ||
                            memcmp(tgt_key.str, c->union_switch_key, cmp_len) != 0) {
                            blocked = false; /* different element, safe */
                        }
                    }
                }
                if (blocked) {
                    checker_error(c, node->loc.line,
                        "cannot mutate union '%.*s' inside its own switch arm — "
                        "active capture would become invalid",
                        (int)c->union_switch_var_len, c->union_switch_var);
                }
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
                    /* Handle auto-deref: h.field goes through slab.get() indirection.
                     * const Handle = const key, NOT const data. Same as const *T. */
                    Type *fld_obj_type = typemap_get(c, root->field.object);
                    if (fld_obj_type && type_unwrap_distinct(fld_obj_type)->kind == TYPE_HANDLE) {
                        through_pointer = true;
                    }
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

        /* non-storable check: pool.get(h) pointer result cannot be stored.
         * BUG-405: only block when result is a pointer — scalar field values
         * (u32, bool etc.) from Handle auto-deref are safe to store. */
        if (node->assign.op == TOK_EQ && is_non_storable(c, node->assign.value)) {
            Type *ns_type = checker_get_type(c, node->assign.value);
            if (ns_type) ns_type = type_unwrap_distinct(ns_type);
            if (!ns_type || ns_type->kind == TYPE_POINTER || ns_type->kind == TYPE_SLICE ||
                ns_type->kind == TYPE_STRUCT || ns_type->kind == TYPE_UNION) {
                checker_error(c, node->loc.line,
                    "cannot store result of get() — use inline");
            }
        }

        /* BUG-225: reject Pool/Ring/Slab assignment — unique resource types.
         * BUG-506: unwrap distinct. */
        { Type *teff = target ? type_unwrap_distinct(target) : NULL;
        if (node->assign.op == TOK_EQ && teff &&
            (teff->kind == TYPE_POOL || teff->kind == TYPE_RING || teff->kind == TYPE_SLAB)) {
            checker_error(c, node->loc.line,
                "cannot assign %s — resource types are not copyable",
                teff->kind == TYPE_POOL ? "Pool" : teff->kind == TYPE_RING ? "Ring" : "Slab");
        } }

        /* string literal to mutable slice: runtime crash on write.
         * BUG-424: allow assignment to const slice fields (const []u8 is safe). */
        if (node->assign.op == TOK_EQ &&
            node->assign.value->kind == NODE_STRING_LIT &&
            target && type_unwrap_distinct(target)->kind == TYPE_SLICE &&
            !type_unwrap_distinct(target)->slice.is_const) {
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
        /* BUG-502: range invalidation for ALL assignment ops, not just TOK_EQ.
         * Compound assignments (+=, -=, *=, etc.) also change the value.
         * if (i < 5) { i += 20; arr[i]; } → stale range without this fix. */
        {
            Node *troot = node->assign.target;
            while (troot && (troot->kind == NODE_FIELD || troot->kind == NODE_INDEX)) {
                if (troot->kind == NODE_FIELD) troot = troot->field.object;
                else troot = troot->index_expr.object;
            }
            if (troot && troot->kind == NODE_IDENT) {
                Symbol *tsym = scope_lookup(c->current_scope,
                    troot->ident.name, (uint32_t)troot->ident.name_len);
                if (tsym) {
                    /* Refactor 1: unified VRP invalidation via helper.
                     * One call for simple ident, one for compound key.
                     * Both use same logic — no more inconsistency between paths. */
                    vrp_invalidate_for_assign(c, troot->ident.name,
                        (uint32_t)troot->ident.name_len,
                        node->assign.op, node->assign.value);
                    /* compound key range (e.g., "s.x" for struct field) */
                    if (node->assign.target->kind == NODE_FIELD) {
                        ExprKey ckey = build_expr_key_a(c, node->assign.target);
                        if (ckey.len > 0) {
                            vrp_invalidate_for_assign(c, ckey.str, (uint32_t)ckey.len,
                                node->assign.op, node->assign.value);
                        }
                    }
                    /* clear — will be re-set below if new value is unsafe.
                     * ONLY clear if assigning the whole variable (NODE_IDENT target).
                     * Field/index assignments (h.val = 42) must NOT clear flags on
                     * root — other fields (h.p) may still hold unsafe pointers. */
                    if (node->assign.target->kind == NODE_IDENT) {
                        tsym->is_local_derived = false;
                        tsym->is_arena_derived = false;
                        tsym->provenance_type = NULL;
                        tsym->container_struct = NULL;
                        tsym->container_field = NULL;
                        tsym->container_field_len = 0;
                    }
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
                                if (src) propagate_escape_flags(tsym, src, tsym->type);
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
                            if (src) propagate_escape_flags(tsym, src, tsym->type);
                            /* provenance propagation through alias (compile-time belt) */
                            if (src && src->provenance_type) tsym->provenance_type = src->provenance_type;
                            if (src && src->container_struct) {
                                tsym->container_struct = src->container_struct;
                                tsym->container_field = src->container_field;
                                tsym->container_field_len = src->container_field_len;
                            }
                        }
                        /* @ptrcast provenance on assignment (compile-time belt) */
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
                            /* BUG-393: also store in compound key map for h.p, arr[0] */
                            {
                                Type *val_type = typemap_get(c, vcheck->intrinsic.args[0]);
                                if (val_type) {
                                    ExprKey tkey = build_expr_key_a(c, node->assign.target);
                                    if (tkey.len > 0) prov_map_set(c, tkey.str, (uint32_t)tkey.len, val_type);
                                }
                            }
                        }
                        /* BUG-393: propagate provenance from source ident to compound target */
                        if (vcheck->kind == NODE_IDENT) {
                            Symbol *src = scope_lookup(c->current_scope,
                                vcheck->ident.name, (uint32_t)vcheck->ident.name_len);
                            if (src && src->provenance_type) {
                                ExprKey tkey = build_expr_key_a(c, node->assign.target);
                                if (tkey.len > 0) prov_map_set(c, tkey.str, (uint32_t)tkey.len, src->provenance_type);
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
                        /* @ptrtoint(&local) on assignment → mark target as local-derived.
                         * Catches: b.addr = @ptrtoint(&x) where b is a struct. */
                        if (vcheck->kind == NODE_INTRINSIC &&
                            vcheck->intrinsic.name_len == 8 &&
                            memcmp(vcheck->intrinsic.name, "ptrtoint", 8) == 0 &&
                            vcheck->intrinsic.arg_count > 0) {
                            Node *ptarg = vcheck->intrinsic.args[0];
                            if (ptarg && ptarg->kind == NODE_UNARY && ptarg->unary.op == TOK_AMP) {
                                Node *root = ptarg->unary.operand;
                                while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                                    if (root->kind == NODE_FIELD) root = root->field.object;
                                    else root = root->index_expr.object;
                                }
                                if (root && root->kind == NODE_IDENT) {
                                    bool is_global = scope_lookup_local(c->global_scope,
                                        root->ident.name, (uint32_t)root->ident.name_len) != NULL;
                                    Symbol *src = scope_lookup(c->current_scope,
                                        root->ident.name, (uint32_t)root->ident.name_len);
                                    if (src && !src->is_static && !is_global)
                                        tsym->is_local_derived = true;
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

        /* BUG-440: non-keep pointer parameter stored in global/static.
         * Spec: non-keep *T is "non-storable — use it, read it, write through it."
         * Storing to global violates this contract — caller may pass &local. */
        if (node->assign.op == TOK_EQ) {
            Node *vnode = node->assign.value;
            while (vnode && vnode->kind == NODE_INTRINSIC && vnode->intrinsic.arg_count > 0)
                vnode = vnode->intrinsic.args[vnode->intrinsic.arg_count - 1];
            if (vnode && vnode->kind == NODE_IDENT) {
                Symbol *val_sym = scope_lookup(c->current_scope,
                    vnode->ident.name, (uint32_t)vnode->ident.name_len);
                Type *vt = val_sym ? type_unwrap_distinct(val_sym->type) : NULL;
                /* A non-keep pointer parameter stored in global violates the
                 * non-keep contract. Detect parameters: func_node is NULL
                 * (local var-decls and globals always set func_node). */
                bool val_is_global = val_sym && scope_lookup_local(c->global_scope,
                    val_sym->name, val_sym->name_len) != NULL;
                bool is_ptr_param = val_sym && !val_sym->is_keep &&
                    !val_sym->is_static && !val_is_global &&
                    val_sym->func_node == NULL && /* parameters have no func_node */
                    vt && (vt->kind == TYPE_POINTER || vt->kind == TYPE_OPAQUE);
                if (is_ptr_param) {
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
                                "cannot store non-keep pointer parameter '%.*s' in "
                                "global/static '%.*s' — add 'keep' qualifier to parameter",
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
            if (value && type_unwrap_distinct(value)->kind == TYPE_ARRAY) {
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
                        /* mark target root as arena-derived — ALL arenas,
                         * including global. arena.reset() invalidates all pointers. */
                        Node *root = node->assign.target;
                        while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                            if (root->kind == NODE_FIELD) root = root->field.object;
                            else root = root->index_expr.object;
                        }
                        if (root && root->kind == NODE_IDENT) {
                            Symbol *tsym = scope_lookup(c->current_scope,
                                root->ident.name, (uint32_t)root->ident.name_len);
                            if (tsym) {
                                tsym->is_arena_derived = true;
                                tsym->is_from_arena = true;
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
            if (val_sym && (val_sym->is_arena_derived || val_sym->is_from_arena)) {
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
                        target_sym->is_from_arena = true;
                    }
                }
            }
        }

        /* scope escape: assigning local array to global slice (implicit coercion) */
        if (node->assign.op == TOK_EQ &&
            target && type_unwrap_distinct(target)->kind == TYPE_SLICE &&
            value && type_unwrap_distinct(value)->kind == TYPE_ARRAY &&
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
            target && type_unwrap_distinct(target)->kind == TYPE_SLICE &&
            !type_unwrap_distinct(target)->slice.is_const &&
            value && type_unwrap_distinct(value)->kind == TYPE_ARRAY) {
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

        /* Designated initializer in assignment: validate fields */
        if (node->assign.op == TOK_EQ && node->assign.value->kind == NODE_STRUCT_INIT && target) {
            if (validate_struct_init(c, node->assign.value, target, node->loc.line)) {
                value = target;
                typemap_set(c, node->assign.value, target);
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
            /* forced division guard for /= and %= — same check as NODE_BINARY */
            if (node->assign.op == TOK_SLASHEQ || node->assign.op == TOK_PERCENTEQ) {
                Node *divisor = node->assign.value;
                /* literal nonzero → ok */
                bool div_ok = false;
                int64_t dv = eval_const_expr(divisor);
                if (dv == CONST_EVAL_FAIL && divisor->kind == NODE_IDENT) {
                    Symbol *dsym = scope_lookup(c->current_scope,
                        divisor->ident.name, (uint32_t)divisor->ident.name_len);
                    if (dsym && dsym->is_const && dsym->func_node) {
                        Node *dinit = NULL;
                        if (dsym->func_node->kind == NODE_GLOBAL_VAR)
                            dinit = dsym->func_node->var_decl.init;
                        else if (dsym->func_node->kind == NODE_VAR_DECL)
                            dinit = dsym->func_node->var_decl.init;
                        if (dinit) dv = eval_const_expr(dinit);
                    }
                }
                if (dv != CONST_EVAL_FAIL && dv != 0) div_ok = true;
                /* range-proven nonzero → ok */
                if (!div_ok && divisor->kind == NODE_IDENT) {
                    struct VarRange *r = find_var_range(c, divisor->ident.name,
                        (uint32_t)divisor->ident.name_len);
                    if (r && r->known_nonzero) div_ok = true;
                }
                if (!div_ok && divisor->kind == NODE_IDENT) {
                    checker_error(c, node->loc.line,
                        "divisor '%.*s' not proven nonzero — "
                        "add 'if (%.*s == 0) { return; }' before division",
                        (int)divisor->ident.name_len, divisor->ident.name,
                        (int)divisor->ident.name_len, divisor->ident.name);
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

        /* module-qualified call: config.func() → rewrite callee to NODE_IDENT
         * with the raw function name. The existing unqualified call resolution
         * already finds imported functions by raw name in global scope.
         * Must happen before check_expr on callee object, because module names
         * aren't variables and would fail as "undefined identifier". */
        if (node->call.callee->kind == NODE_FIELD &&
            node->call.callee->field.object->kind == NODE_IDENT) {
            const char *maybe_mod = node->call.callee->field.object->ident.name;
            uint32_t maybe_mod_len = (uint32_t)node->call.callee->field.object->ident.name_len;
            const char *func_name = node->call.callee->field.field_name;
            uint32_t func_len = (uint32_t)node->call.callee->field.field_name_len;
            /* check if this is a module name (not a variable/type).
             * A20: unwrap distinct — distinct struct variable is still a type, not a module. */
            Symbol *var_sym = scope_lookup(c->current_scope, maybe_mod, maybe_mod_len);
            Type *vs_eff = (var_sym && var_sym->type) ? type_unwrap_distinct(var_sym->type) : NULL;
            if (!var_sym || (vs_eff && vs_eff->kind != TYPE_STRUCT &&
                vs_eff->kind != TYPE_ENUM && vs_eff->kind != TYPE_UNION &&
                vs_eff->kind != TYPE_POOL && vs_eff->kind != TYPE_SLAB &&
                vs_eff->kind != TYPE_RING && vs_eff->kind != TYPE_ARENA)) {
                /* try as module-qualified call — look up module__func in global scope */
                uint32_t mang_len = maybe_mod_len + 2 + func_len;
                char *mangled = (char *)arena_alloc(c->arena, mang_len + 1);
                if (mangled) {
                    memcpy(mangled, maybe_mod, maybe_mod_len);
                    mangled[maybe_mod_len] = '_';
                    mangled[maybe_mod_len + 1] = '_';
                    memcpy(mangled + maybe_mod_len + 2, func_name, func_len);
                    mangled[mang_len] = '\0';
                    Symbol *mod_func = scope_lookup(c->global_scope, mangled, mang_len);
                    if (mod_func && mod_func->is_function) {
                        /* rewrite callee to raw function name — the existing
                         * unqualified resolution finds it in global scope */
                        node->call.callee->kind = NODE_IDENT;
                        node->call.callee->ident.name = func_name;
                        node->call.callee->ident.name_len = func_len;
                        goto normal_call;
                    }
                }
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

            /* ThreadHandle.join() — scoped spawn join */
            if (field_node->field.object->kind == NODE_IDENT) {
                Symbol *osym2 = scope_lookup(c->current_scope,
                    field_node->field.object->ident.name,
                    (uint32_t)field_node->field.object->ident.name_len);
                if (osym2 && osym2->is_thread_handle) {
                    if (mlen == 4 && memcmp(mname, "join", 4) == 0) {
                        if (node->call.arg_count != 0)
                            checker_error(c, node->loc.line, "ThreadHandle.join() takes no arguments");
                        result = ty_void;
                        typemap_set(c, field_node, result);
                        break;
                    }
                    checker_error(c, node->loc.line,
                        "ThreadHandle has no method '%.*s' (available: join)",
                        (int)mlen, mname);
                    result = ty_void;
                    break;
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
                    /* BUG-471: type-check handle element against pool element */
                    if (node->call.arg_count == 1) {
                        Type *arg_t = check_expr(c, node->call.args[0]);
                        if (arg_t) {
                            Type *aeff = type_unwrap_distinct(arg_t);
                            if (aeff->kind == TYPE_HANDLE &&
                                !type_equals(type_unwrap_distinct(aeff->handle.elem),
                                             type_unwrap_distinct(obj->pool.elem))) {
                                checker_error(c, node->loc.line,
                                    "pool.free() expects Handle(%s), got Handle(%s)",
                                    type_name(obj->pool.elem), type_name(aeff->handle.elem));
                            }
                        }
                    }
                    track_dyn_freed_index(c, node);
                    result = ty_void;
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 9 && memcmp(mname, "alloc_ptr", 9) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'alloc_ptr' on const Pool");
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "pool.alloc_ptr() takes no arguments");
                    result = type_optional(c->arena, type_pointer(c->arena, obj->pool.elem));
                    typemap_set(c, field_node, result);
                    break;
                }
                if (mlen == 8 && memcmp(mname, "free_ptr", 8) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'free_ptr' on const Pool");
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "pool.free_ptr() takes exactly 1 argument");
                    if (node->call.arg_count == 1) {
                        Type *arg_t = check_expr(c, node->call.args[0]);
                        Type *expected = type_pointer(c->arena, obj->pool.elem);
                        if (arg_t && !type_equals(type_unwrap_distinct(arg_t), type_unwrap_distinct(expected))) {
                            checker_error(c, node->loc.line,
                                "pool.free_ptr() expects '*%s', got '%s'",
                                type_name(obj->pool.elem), type_name(arg_t));
                        }
                    }
                    result = ty_void;
                    typemap_set(c, field_node, result);
                    break;
                }
                checker_error(c, node->loc.line,
                    "Pool has no method '%.*s' (available: alloc, alloc_ptr, get, free, free_ptr)",
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
                    /* Warn if pushing pointer through Ring (channel safety) */
                    if (node->call.arg_count == 1) {
                        Type *elt = obj->ring.elem;
                        Type *eeff = elt ? type_unwrap_distinct(elt) : NULL;
                        if (eeff && (eeff->kind == TYPE_POINTER || eeff->kind == TYPE_OPAQUE))
                            checker_warning(c, node->loc.line,
                                "pushing pointer through Ring channel — "
                                "pointer may not be valid in receiver context");
                    }
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
                    /* Same pointer warning for push_checked */
                    if (node->call.arg_count == 1) {
                        Type *elt = obj->ring.elem;
                        Type *eeff = elt ? type_unwrap_distinct(elt) : NULL;
                        if (eeff && (eeff->kind == TYPE_POINTER || eeff->kind == TYPE_OPAQUE))
                            checker_warning(c, node->loc.line,
                                "pushing pointer through Ring channel — "
                                "pointer may not be valid in receiver context");
                    }
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
                    check_isr_ban(c, node->loc.line, "slab.alloc()");
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
                    /* BUG-471: type-check handle element against slab element */
                    if (node->call.arg_count == 1) {
                        Type *arg_t = check_expr(c, node->call.args[0]);
                        if (arg_t) {
                            Type *aeff = type_unwrap_distinct(arg_t);
                            if (aeff->kind == TYPE_HANDLE &&
                                !type_equals(type_unwrap_distinct(aeff->handle.elem),
                                             type_unwrap_distinct(obj->slab.elem))) {
                                checker_error(c, node->loc.line,
                                    "slab.free() expects Handle(%s), got Handle(%s)",
                                    type_name(obj->slab.elem), type_name(aeff->handle.elem));
                            }
                        }
                    }
                    track_dyn_freed_index(c, node);
                    result = ty_void;
                    typemap_set(c, field_node,result);
                    break;
                }
                if (mlen == 9 && memcmp(mname, "alloc_ptr", 9) == 0) {
                    check_isr_ban(c, node->loc.line, "slab.alloc_ptr()");
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'alloc_ptr' on const Slab");
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "slab.alloc_ptr() takes no arguments");
                    result = type_optional(c->arena, type_pointer(c->arena, obj->slab.elem));
                    typemap_set(c, field_node, result);
                    break;
                }
                if (mlen == 8 && memcmp(mname, "free_ptr", 8) == 0) {
                    if (obj_is_const)
                        checker_error(c, node->loc.line,
                            "cannot call mutating method 'free_ptr' on const Slab");
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "slab.free_ptr() takes exactly 1 argument");
                    if (node->call.arg_count == 1) {
                        Type *arg_t = check_expr(c, node->call.args[0]);
                        Type *expected = type_pointer(c->arena, obj->slab.elem);
                        if (arg_t && !type_equals(type_unwrap_distinct(arg_t), type_unwrap_distinct(expected))) {
                            checker_error(c, node->loc.line,
                                "slab.free_ptr() expects '*%s', got '%s'",
                                type_name(obj->slab.elem), type_name(arg_t));
                        }
                    }
                    result = ty_void;
                    typemap_set(c, field_node, result);
                    break;
                }
                checker_error(c, node->loc.line,
                    "Slab has no method '%.*s' (available: alloc, alloc_ptr, get, free, free_ptr)",
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

            /* Task.new() / Task.delete() — auto-Slab sugar */
            obj = type_unwrap_distinct(obj);
            if (obj->kind == TYPE_STRUCT) {
                if (mlen == 3 && memcmp(mname, "new", 3) == 0) {
                    check_isr_ban(c, node->loc.line, "Task.new()");
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "%.*s.new() takes no arguments",
                            (int)obj->struct_type.name_len, obj->struct_type.name);
                    find_or_create_auto_slab(c, obj);
                    result = type_optional(c->arena, type_handle(c->arena, obj));
                    typemap_set(c, field_node, result);
                    break;
                }
                if (mlen == 7 && memcmp(mname, "new_ptr", 7) == 0) {
                    check_isr_ban(c, node->loc.line, "Task.new_ptr()");
                    if (node->call.arg_count != 0)
                        checker_error(c, node->loc.line, "%.*s.new_ptr() takes no arguments",
                            (int)obj->struct_type.name_len, obj->struct_type.name);
                    find_or_create_auto_slab(c, obj);
                    result = type_optional(c->arena, type_pointer(c->arena, obj));
                    typemap_set(c, field_node, result);
                    break;
                }
                if (mlen == 6 && memcmp(mname, "delete", 6) == 0) {
                    /* Task.delete(h) → void — same as slab.free(h) */
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "%.*s.delete() takes exactly 1 argument",
                            (int)obj->struct_type.name_len, obj->struct_type.name);
                    result = ty_void;
                    typemap_set(c, field_node, result);
                    break;
                }
                if (mlen == 10 && memcmp(mname, "delete_ptr", 10) == 0) {
                    /* Task.delete_ptr(p) → void — same as slab.free_ptr(p) */
                    if (node->call.arg_count != 1)
                        checker_error(c, node->loc.line, "%.*s.delete_ptr() takes exactly 1 argument",
                            (int)obj->struct_type.name_len, obj->struct_type.name);
                    if (node->call.arg_count == 1) {
                        Type *arg_t = check_expr(c, node->call.args[0]);
                        Type *expected = type_pointer(c->arena, obj);
                        if (arg_t && !type_equals(type_unwrap_distinct(arg_t), type_unwrap_distinct(expected))) {
                            checker_error(c, node->loc.line,
                                "%.*s.delete_ptr() expects '*%.*s', got '%s'",
                                (int)obj->struct_type.name_len, obj->struct_type.name,
                                (int)obj->struct_type.name_len, obj->struct_type.name,
                                type_name(arg_t));
                        }
                    }
                    result = ty_void;
                    typemap_set(c, field_node, result);
                    break;
                }
            }

            /* not a builtin — fall through to normal call resolution */
        }

        /* normal function call */
        normal_call:;
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
                    if (arg && type_unwrap_distinct(arg)->kind == TYPE_ARRAY &&
                        param && type_unwrap_distinct(param)->kind == TYPE_SLICE &&
                        !type_unwrap_distinct(param)->slice.is_const &&
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
                    if (arg && type_unwrap_distinct(arg)->kind == TYPE_ARRAY &&
                        param && type_unwrap_distinct(param)->kind == TYPE_SLICE &&
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

                    /* []T → *T auto-coerce for extern (no body) C functions only.
                     * Emitter will auto-emit .ptr at call site.
                     * Const check: string literals and const slices require const *T param. */
                    bool slice_to_ptr_ok = false;
                    if (arg && arg->kind == TYPE_SLICE &&
                        param && param->kind == TYPE_POINTER &&
                        type_equals(arg->slice.inner, param->pointer.inner)) {
                        /* const safety: string literal or const slice → must be const *T */
                        bool arg_is_const = arg->slice.is_const ||
                            node->call.args[i]->kind == NODE_STRING_LIT;
                        if (arg_is_const && !param->pointer.is_const) {
                            checker_error(c, node->loc.line,
                                "argument %d: cannot pass const []%s to non-const '*%s' — use 'const *%s'",
                                i + 1, type_name(arg->slice.inner),
                                type_name(param->pointer.inner),
                                type_name(param->pointer.inner));
                        }
                        /* only allow for extern functions (forward decl, no body) */
                        if (node->call.callee->kind == NODE_IDENT) {
                            Symbol *csym = scope_lookup(c->current_scope,
                                node->call.callee->ident.name,
                                (uint32_t)node->call.callee->ident.name_len);
                            if (csym && csym->is_function && csym->func_node &&
                                !csym->func_node->func_decl.body) {
                                slice_to_ptr_ok = true;
                            }
                        }
                    }
                    /* Designated init as call arg: validate fields against param struct type */
                    if (node->call.args[i]->kind == NODE_STRUCT_INIT && param) {
                        if (validate_struct_init(c, node->call.args[i], param, node->loc.line)) {
                            arg = param;
                            typemap_set(c, node->call.args[i], param);
                        }
                    }

                    if (!type_equals(param, arg) &&
                        !can_implicit_coerce(arg, param) &&
                        !is_literal_compatible(node->call.args[i], param) &&
                        !slice_to_ptr_ok) {
                        checker_error(c, node->loc.line,
                            "argument %u: expected '%s', got '%s'",
                            i + 1, type_name(param), type_name(arg));
                    }
                }

                /* keep parameter validation: check call arguments.
                 * Works for BOTH direct function calls AND function pointer calls
                 * by using param_keeps on the resolved Type (BUG-277).
                 * Auto-keep: if callee is a function POINTER (not direct call),
                 * all pointer params are treated as keep — compiler can't see
                 * inside the fn ptr target, must assume worst case. */
                bool is_fn_ptr_call = (node->call.callee->kind != NODE_IDENT ||
                    !scope_lookup(c->current_scope,
                        node->call.callee->ident.name,
                        (uint32_t)node->call.callee->ident.name_len) ||
                    !scope_lookup(c->current_scope,
                        node->call.callee->ident.name,
                        (uint32_t)node->call.callee->ident.name_len)->is_function);
                if (effective_callee->func_ptr.param_keeps || is_fn_ptr_call) {
                    for (int i = 0; i < (int)effective_callee->func_ptr.param_count &&
                         i < node->call.arg_count; i++) {
                        /* auto-keep: fn ptr calls treat ALL pointer params as keep */
                        bool param_is_keep = false;
                        if (effective_callee->func_ptr.param_keeps &&
                            effective_callee->func_ptr.param_keeps[i]) {
                            param_is_keep = true;
                        } else if (is_fn_ptr_call) {
                            Type *pt = effective_callee->func_ptr.params[i];
                            Type *ptu = type_unwrap_distinct(pt);
                            if (ptu && ptu->kind == TYPE_POINTER) param_is_keep = true;
                        }
                        if (!param_is_keep) continue;
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
                                karg->unary.operand->ident.name,
                                (uint32_t)karg->unary.operand->ident.name_len);
                            if (arg_sym && !arg_sym->is_static) {
                                /* BUG-317: check both raw AND mangled keys for imported globals */
                                bool is_global = scope_lookup_local(c->global_scope,
                                    arg_sym->name, arg_sym->name_len) != NULL;
                                if (!is_global && c->current_module) {
                                    /* try mangled: module__name (BUG-332: double underscore) */
                                    uint32_t mkl = c->current_module_len + 2 + arg_sym->name_len;
                                    char *mk = (char *)arena_alloc(c->arena, mkl + 1);
                                    if (mk) {
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
                        /* BUG-221/370/387: also reject local-derived pointers.
                         * Walk through orelse chain to check BOTH expr and fallback sides.
                         * BUG-387: orelse fallback may be a local-derived ident. */
                        {
                            /* collect all terminal idents from orelse chain */
                            Node *ld_nodes[8];
                            int ld_count = 0;
                            Node *ld_walk = arg_node;
                            while (ld_walk && ld_walk->kind == NODE_ORELSE && ld_count < 7) {
                                /* check fallback side */
                                if (ld_walk->orelse.fallback &&
                                    ld_walk->orelse.fallback->kind != NODE_ORELSE) {
                                    ld_nodes[ld_count++] = ld_walk->orelse.fallback;
                                }
                                ld_walk = ld_walk->orelse.expr;
                            }
                            if (ld_walk) ld_nodes[ld_count++] = ld_walk;
                            for (int ldi = 0; ldi < ld_count; ldi++) {
                                if (ld_nodes[ldi]->kind != NODE_IDENT) continue;
                                Symbol *arg_sym = scope_lookup(c->current_scope,
                                    ld_nodes[ldi]->ident.name,
                                    (uint32_t)ld_nodes[ldi]->ident.name_len);
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
                    ComptimeParam stack_cp2[8];
                    memset(stack_cp2, 0, sizeof(stack_cp2));
                    ComptimeParam *cparams = pc <= 8 ? stack_cp2 :
                        (ComptimeParam *)arena_alloc(c->arena, pc * sizeof(ComptimeParam));
                    if (cparams && cparams != stack_cp2)
                        memset(cparams, 0, pc * sizeof(ComptimeParam));
                    bool all_const = cparams != NULL;
                    for (int ci = 0; ci < node->call.arg_count && ci < pc && all_const; ci++) {
                        /* BUG-430: use scoped eval to resolve const idents */
                        int64_t v = eval_const_expr_scoped(c, node->call.args[ci]);
                        if (v == CONST_EVAL_FAIL) {
                            /* Float literal: store double bits as int64 for float comptime */
                            if (node->call.args[ci]->kind == NODE_FLOAT_LIT) {
                                double d = node->call.args[ci]->float_lit.value;
                                memcpy(&v, &d, sizeof(v));
                            } else if (node->call.args[ci]->kind == NODE_UNARY &&
                                       node->call.args[ci]->unary.op == TOK_MINUS &&
                                       node->call.args[ci]->unary.operand->kind == NODE_FLOAT_LIT) {
                                double d = -node->call.args[ci]->unary.operand->float_lit.value;
                                memcpy(&v, &d, sizeof(v));
                            } else {
                                all_const = false; break;
                            }
                        }
                        cparams[ci].name = fn->func_decl.params[ci].name;
                        cparams[ci].name_len = (uint32_t)fn->func_decl.params[ci].name_len;
                        cparams[ci].value = v;
                    }
                    if (!all_const) {
                        /* BUG-425: inside comptime function body, args may be
                         * params not yet substituted — skip error here, the real
                         * evaluation happens at the call site with concrete values */
                        if (!c->in_comptime_body) {
                            checker_error(c, node->loc.line,
                                "comptime function '%.*s' requires all arguments to be compile-time constants",
                                (int)callee_sym->name_len, callee_sym->name);
                        }
                    } else if (fn->func_decl.body) {
                        _comptime_global_scope = c->global_scope;
                        /* BUG-593: for float-returning comptime functions, skip
                         * the integer eval_comptime_block path entirely. Float
                         * args are stored as int64 bit-patterns in cparams;
                         * integer multiply on those bits returns non-FAIL
                         * garbage and short-circuits past the float path.
                         * Dispatch to eval_comptime_float_expr directly. */
                        Type *ret_ty_check = resolve_type(c, fn->func_decl.return_type);
                        bool ret_is_float = ret_ty_check &&
                            (ret_ty_check->kind == TYPE_F32 || ret_ty_check->kind == TYPE_F64);
                        int64_t val = CONST_EVAL_FAIL;
                        if (!ret_is_float) {
                            ComptimeCtx cctx;
                            ct_ctx_init(&cctx, cparams, pc);
                            val = eval_comptime_block(fn->func_decl.body, &cctx);
                            ct_ctx_free(&cctx);
                        }
                        if (val != CONST_EVAL_FAIL) {
                            node->call.comptime_value = val;
                            node->call.is_comptime_resolved = true;
                        } else {
                            /* Scalar eval failed — try struct return.
                             * Find return { .x = val } in body, evaluate field values. */
                            Node *ret_expr = find_comptime_return_expr(fn->func_decl.body);
                            Node *si = (ret_expr && ret_expr->kind == NODE_STRUCT_INIT) ? ret_expr : NULL;
                            if (si) {
                                Node *csi = eval_comptime_struct_return(c->arena, si, cparams, pc);
                                if (csi) {
                                    node->call.comptime_struct_init = csi;
                                    node->call.is_comptime_resolved = true;
                                    /* Set type on the struct_init for emitter */
                                    typemap_set(c, csi, result);
                                } else {
                                    checker_error(c, node->loc.line,
                                        "comptime function '%.*s' body could not be evaluated at compile time",
                                        (int)callee_sym->name_len, callee_sym->name);
                                }
                            } else {
                                /* Try float return: comptime f32/f64 functions */
                                Type *fret = resolve_type(c, fn->func_decl.return_type);
                                if (fret && (fret->kind == TYPE_F32 || fret->kind == TYPE_F64)) {
                                    Node *ret_expr = find_comptime_return_expr(fn->func_decl.body);
                                    if (ret_expr) {
                                        double fval = eval_comptime_float_expr(ret_expr, cparams, pc);
                                        if (!isnan(fval)) {
                                            node->call.comptime_float_value = fval;
                                            node->call.is_comptime_float = true;
                                            node->call.is_comptime_resolved = true;
                                        }
                                    }
                                }
                                if (!node->call.is_comptime_resolved) {
                                    checker_error(c, node->loc.line,
                                        "comptime function '%.*s' body could not be evaluated at compile time",
                                        (int)callee_sym->name_len, callee_sym->name);
                                }
                            }
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

        /* VRP invalidation on function call:
         * 1. (BUG-475) &var passed as arg — function might modify through pointer
         * 2. (BUG-478) Global variables — function might modify any global
         * 3. (BUG-479) Local vars with address taken — pointer alias may exist */
        for (int ai = 0; ai < node->call.arg_count; ai++) {
            Node *arg = node->call.args[ai];
            if (arg && arg->kind == NODE_UNARY && arg->unary.op == TOK_AMP) {
                Node *operand = arg->unary.operand;
                while (operand && (operand->kind == NODE_FIELD || operand->kind == NODE_INDEX)) {
                    if (operand->kind == NODE_FIELD) operand = operand->field.object;
                    else operand = operand->index_expr.object;
                }
                if (operand && operand->kind == NODE_IDENT) {
                    struct VarRange *r = find_var_range(c, operand->ident.name,
                        (uint32_t)operand->ident.name_len);
                    if (r) {
                        r->min_val = INT64_MIN;
                        r->max_val = INT64_MAX;
                        r->known_nonzero = false;
                    }
                }
            }
        }
        /* BUG-478: Any function call might modify global variables.
         * Invalidate VRP ranges for all globals in the range stack.
         * Skip comptime calls (pure, no side effects). */
        if (!node->call.is_comptime_resolved) {
            for (int ri = 0; ri < c->var_range_count; ri++) {
                struct VarRange *r = &c->var_ranges[ri];
                /* Check if this range entry is for a global variable */
                Symbol *rsym = scope_lookup_local(c->global_scope, r->name, r->name_len);
                if (rsym && !rsym->is_function && !rsym->is_const) {
                    r->min_val = INT64_MIN;
                    r->max_val = INT64_MAX;
                    r->known_nonzero = false;
                }
            }
        }

        break;
    }

    /* ---- Field access ---- */
    case NODE_FIELD: {
        const char *fname = node->field.field_name;
        uint32_t flen = (uint32_t)node->field.field_name_len;

        /* BUG-432: module-qualified variable access: config.VERSION
         * Must intercept BEFORE check_expr on object, because module
         * names aren't variables — check_expr would error "undefined".
         * Same pattern as NODE_CALL module-qualified call (BUG-416). */
        if (node->field.object->kind == NODE_IDENT) {
            const char *maybe_mod = node->field.object->ident.name;
            uint32_t maybe_mod_len = (uint32_t)node->field.object->ident.name_len;
            Symbol *var_sym = scope_lookup(c->current_scope, maybe_mod, maybe_mod_len);
            if (!var_sym) {
                /* not a variable — try as module__field in global scope */
                uint32_t mang_len = maybe_mod_len + 2 + flen;
                char *mangled = (char *)arena_alloc(c->arena, mang_len + 1);
                if (mangled) {
                    memcpy(mangled, maybe_mod, maybe_mod_len);
                    mangled[maybe_mod_len] = '_';
                    mangled[maybe_mod_len + 1] = '_';
                    memcpy(mangled + maybe_mod_len + 2, fname, flen);
                    mangled[mang_len] = '\0';
                    Symbol *gsym = scope_lookup(c->global_scope, mangled, mang_len);
                    if (gsym && gsym->type) {
                        /* rewrite to NODE_IDENT with the raw field name.
                         * The emitter resolves via mangled lookup in global scope.
                         * Using raw name avoids double-mangling. */
                        node->kind = NODE_IDENT;
                        node->ident.name = fname;
                        node->ident.name_len = flen;
                        typemap_set(c, node, gsym->type);
                        result = gsym->type;
                        break;
                    }
                }
            }
        }

        Type *obj_raw = check_expr(c, node->field.object);
        /* BUG-410: unwrap distinct for field access dispatch */
        Type *obj = type_unwrap_distinct(obj_raw);

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

        /* Handle auto-deref: h.field → slab.get(h).field */
        if (obj->kind == TYPE_HANDLE) {
            /* Note: const Handle does NOT block mutation through auto-deref.
             * Handle is a key (like file descriptor), not a pointer.
             * const key doesn't mean const data — same as const int fd = open();
             * write(fd, data, n) is valid. If user wants immutable data,
             * don't write to it — the Handle itself being const just means
             * you can't reassign the handle variable. */
            Type *elem = type_unwrap_distinct(obj->handle.elem);
            if (elem->kind == TYPE_STRUCT) {
                /* verify an allocator exists for this Handle type */
                Symbol *alloc_sym = NULL;
                if (node->field.object->kind == NODE_IDENT) {
                    Symbol *hsym = scope_lookup(c->current_scope,
                        node->field.object->ident.name,
                        (uint32_t)node->field.object->ident.name_len);
                    if (hsym) alloc_sym = hsym->slab_source;
                }
                if (!alloc_sym) {
                    alloc_sym = find_unique_allocator(c->current_scope, obj->handle.elem);
                    if (!alloc_sym)
                        alloc_sym = find_unique_allocator(c->global_scope, obj->handle.elem);
                }
                if (!alloc_sym) {
                    checker_error(c, node->loc.line,
                        "no Pool or Slab found for Handle(%.*s) — cannot auto-deref. "
                        "Use explicit pool.get(h).%.*s",
                        (int)elem->struct_type.name_len, elem->struct_type.name,
                        (int)flen, fname);
                    result = ty_void;
                    break;
                }
                /* Dynamic-index UAF check: if handles[j] and handles was
                 * dynamically freed, either error (all_freed) or auto-guard */
                if (node->field.object->kind == NODE_INDEX &&
                    node->field.object->index_expr.object->kind == NODE_IDENT) {
                    const char *aname = node->field.object->index_expr.object->ident.name;
                    uint32_t alen = (uint32_t)node->field.object->index_expr.object->ident.name_len;
                    for (int dfi = 0; dfi < c->dyn_freed_count; dfi++) {
                        struct DynFreed *df = &c->dyn_freed[dfi];
                        if (df->array_name_len == alen &&
                            memcmp(df->array_name, aname, alen) == 0) {
                            if (df->all_freed) {
                                checker_error(c, node->loc.line,
                                    "all elements of '%.*s' were freed in loop — "
                                    "cannot access '%.*s[...].%.*s'",
                                    (int)alen, aname, (int)alen, aname, (int)flen, fname);
                            } else {
                                checker_warning(c, node->loc.line,
                                    "auto-guard inserted for '%.*s' — element may have been freed "
                                    "at dynamic index. Add explicit index guard for zero overhead",
                                    (int)alen, aname);
                                /* Store the UAF guard info for emitter.
                                 * Reuse auto_guard array — emitter checks dyn_freed to
                                 * distinguish bounds guard from UAF guard. */
                                if (c->auto_guard_count >= c->auto_guard_capacity) {
                                    int nc = c->auto_guard_capacity ? c->auto_guard_capacity * 2 : 16;
                                    struct AutoGuard *ng = (struct AutoGuard *)arena_alloc(c->arena, nc * sizeof(struct AutoGuard));
                                    if (ng) {
                                        if (c->auto_guards) memcpy(ng, c->auto_guards, c->auto_guard_count * sizeof(struct AutoGuard));
                                        c->auto_guards = ng;
                                        c->auto_guard_capacity = nc;
                                    }
                                }
                                if (c->auto_guards && c->auto_guard_count < c->auto_guard_capacity) {
                                    /* Use array_size = UINT64_MAX as sentinel for "UAF guard" */
                                    c->auto_guards[c->auto_guard_count].node = node;
                                    c->auto_guards[c->auto_guard_count].array_size = UINT64_MAX;
                                    c->auto_guard_count++;
                                }
                            }
                            break;
                        }
                    }
                }

                /* find the struct field */
                for (uint32_t i = 0; i < elem->struct_type.field_count; i++) {
                    SField *f = &elem->struct_type.fields[i];
                    if (f->name_len == flen && memcmp(f->name, fname, flen) == 0) {
                        result = f->type;
                        /* mark as non-storable — same as pool.get(h).field */
                        mark_non_storable(c, node);
                        break;
                    }
                }
                if (!result) {
                    checker_error(c, node->loc.line,
                        "struct '%.*s' has no field '%.*s'",
                        (int)elem->struct_type.name_len, elem->struct_type.name,
                        (int)flen, fname);
                    result = ty_void;
                }
                break;
            }
            checker_error(c, node->loc.line,
                "Handle element type '%s' is not a struct — cannot auto-deref",
                type_name(elem));
            result = ty_void;
            break;
        }

        /* Red Team V14/V18: shared struct access in async functions.
         * Only ban if we're inside `c->in_async_yield_stmt` — a statement
         * that contains yield/await. Shared access in non-yielding statements
         * is safe (lock acquired and released within same poll call).
         * V18: also check through pointers (*shared_struct).field */
        if (c->in_async_yield_stmt) {
            Type *check_shared = obj;
            if (check_shared->kind == TYPE_POINTER)
                check_shared = type_unwrap_distinct(check_shared->pointer.inner);
            if (check_shared->kind == TYPE_STRUCT &&
                (check_shared->struct_type.is_shared || check_shared->struct_type.is_shared_rw)) {
                checker_error(c, node->loc.line,
                    "cannot access shared struct in statement containing yield/await — "
                    "lock would be held across suspension, causing deadlock");
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
                /* B2 refactor: unified union switch lock check */
                if (check_union_switch_mutation(c, node->field.object)) {
                    result = ty_void;
                    break;
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
            /* B2 refactor: unified union switch lock check */
            if (check_union_switch_mutation(c, node->field.object)) {
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
        Type *obj_raw = check_expr(c, node->index_expr.object);
        /* BUG-410: unwrap distinct for array/slice/pointer index dispatch */
        Type *obj = type_unwrap_distinct(obj_raw);
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
                /* literal index in range → proven safe */
                if (idx_val < obj->array.size) {
                    mark_proven(c, node);
                }
            }
            /* range propagation: check if index ident has proven range */
            if (node->index_expr.index->kind == NODE_IDENT) {
                struct VarRange *r = find_var_range(c,
                    node->index_expr.index->ident.name,
                    (uint32_t)node->index_expr.index->ident.name_len);
                if (r && r->min_val >= 0 && r->max_val >= 0 &&
                    (uint64_t)r->max_val < obj->array.size) {
                    mark_proven(c, node);
                }
                /* Auto-guard: if not proven, mark for auto-guard insertion in emitter.
                 * Compiler inserts if (idx >= size) { return <zero>; } invisibly.
                 * Warn so programmer knows they can add a guard for zero overhead. */
                if (!checker_is_proven(c, node)) {
                    mark_auto_guard(c, node, obj->array.size);
                    checker_warning(c, node->loc.line,
                        "index '%.*s' not proven in range for array of size %llu — "
                        "auto-guard inserted. Add 'if (%.*s >= %llu) { return; }' to eliminate guard",
                        (int)node->index_expr.index->ident.name_len,
                        node->index_expr.index->ident.name,
                        (unsigned long long)obj->array.size,
                        (int)node->index_expr.index->ident.name_len,
                        node->index_expr.index->ident.name,
                        (unsigned long long)obj->array.size);
                }
            }
            /* Inline call range: arr[func()] where func has return range */
            if (!checker_is_proven(c, node) &&
                node->index_expr.index->kind == NODE_CALL &&
                node->index_expr.index->call.callee &&
                node->index_expr.index->call.callee->kind == NODE_IDENT) {
                Symbol *csym = scope_lookup(c->current_scope,
                    node->index_expr.index->call.callee->ident.name,
                    (uint32_t)node->index_expr.index->call.callee->ident.name_len);
                if (!csym) csym = scope_lookup(c->global_scope,
                    node->index_expr.index->call.callee->ident.name,
                    (uint32_t)node->index_expr.index->call.callee->ident.name_len);
                if (csym && csym->has_return_range &&
                    csym->return_range_min >= 0 &&
                    (uint64_t)csym->return_range_max < obj->array.size) {
                    mark_proven(c, node);
                }
            }
            result = obj->array.inner;
        } else if (obj->kind == TYPE_SLICE) {
            /* range propagation for slices: if index proven < slice bound */
            if (node->index_expr.index->kind == NODE_INT_LIT) {
                /* literal index on slice — can't prove at compile time (len unknown) */
            } else if (node->index_expr.index->kind == NODE_IDENT) {
                /* If guard was against a constant AND we know slice len, we could prove.
                 * For now: slices with range-proven indices still get runtime check
                 * (slice len is runtime). This is correct — we'll optimize when we
                 * track slice-len as a known value. */
            }
            result = obj->slice.inner;
        } else if (obj->kind == TYPE_POINTER) {
            /* pointer indexing: check mmio_bound if available, else warn */
            bool ptr_proven = false;
            uint64_t mmio_bound = 0;

            /* Path 1: variable with mmio_bound (volatile *T var = @inttoptr) */
            if (node->index_expr.object->kind == NODE_IDENT) {
                Symbol *psym = scope_lookup(c->current_scope,
                    node->index_expr.object->ident.name,
                    (uint32_t)node->index_expr.object->ident.name_len);
                if (psym && psym->mmio_bound > 0)
                    mmio_bound = psym->mmio_bound;
            }
            /* Path 2: direct @inttoptr(...)[N] — no variable, compute bound inline */
            if (mmio_bound == 0 && node->index_expr.object->kind == NODE_INTRINSIC &&
                node->index_expr.object->intrinsic.name_len == 8 &&
                memcmp(node->index_expr.object->intrinsic.name, "inttoptr", 8) == 0 &&
                node->index_expr.object->intrinsic.arg_count > 0) {
                int64_t addr = eval_const_expr(node->index_expr.object->intrinsic.args[0]);
                if (addr != CONST_EVAL_FAIL) {
                    for (int ri = 0; ri < c->mmio_range_count; ri++) {
                        if ((uint64_t)addr >= c->mmio_ranges[ri][0] &&
                            (uint64_t)addr <= c->mmio_ranges[ri][1]) {
                            uint64_t rsz = c->mmio_ranges[ri][1] - (uint64_t)addr + 1;
                            int esz = type_width(obj->pointer.inner) / 8;
                            if (esz > 0) mmio_bound = rsz / (uint64_t)esz;
                            break;
                        }
                    }
                }
            }

            /* Check index against mmio_bound */
            if (mmio_bound > 0) {
                if (node->index_expr.index->kind == NODE_INT_LIT) {
                    /* constant index — compile-time check */
                    uint64_t idx = node->index_expr.index->int_lit.value;
                    if (idx >= mmio_bound) {
                        checker_error(c, node->loc.line,
                            "MMIO index %llu is out of range (max %llu from mmio declaration)",
                            (unsigned long long)idx, (unsigned long long)mmio_bound - 1);
                    }
                    ptr_proven = true;
                    mark_proven(c, node);
                } else {
                    /* variable index — auto-guard using mmio_bound as array size */
                    mark_auto_guard(c, node, mmio_bound);
                    checker_warning(c, node->loc.line,
                        "MMIO index '%.*s' not proven in range (max %llu) — auto-guard inserted",
                        node->index_expr.index->kind == NODE_IDENT ?
                            (int)node->index_expr.index->ident.name_len : 1,
                        node->index_expr.index->kind == NODE_IDENT ?
                            node->index_expr.index->ident.name : "?",
                        (unsigned long long)mmio_bound - 1);
                    ptr_proven = true;
                }
            }
            if (!ptr_proven && !obj->pointer.is_volatile) {
                checker_warning(c, node->loc.line,
                    "pointer indexing has no bounds check — "
                    "use []%s (slice) for bounds-checked access",
                    type_name(obj->pointer.inner));
            }
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
        Type *obj_raw = check_expr(c, node->slice.object);
        /* BUG-410: unwrap distinct for slice/array/integer dispatch */
        Type *obj = type_unwrap_distinct(obj_raw);
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
                 * Yield inside: safe at var-decl level (BUG-481 state struct temps).
                 * Unsafe at expression level — caught by GCC ("switch jumps into
                 * statement expression"). No checker ban needed. */
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

    /* ---- C-style typecast: (Type)expr ---- */
    case NODE_TYPECAST: {
        Type *target = resolve_type(c, node->typecast.target_type);
        Type *source = check_expr(c, node->typecast.expr);
        Type *tgt_eff = type_unwrap_distinct(target);
        Type *src_eff = type_unwrap_distinct(source);

        /* Validate the cast is sensible */
        bool valid = false;

        /* integer ↔ integer: widening (silent) or narrowing (truncate) */
        if (type_is_integer(target) && type_is_integer(source)) valid = true;
        /* integer ↔ float: value conversion */
        if (type_is_integer(target) && type_is_float(source)) valid = true;
        if (type_is_float(target) && type_is_integer(source)) valid = true;
        /* float ↔ float: f32 ↔ f64 */
        if (type_is_float(target) && type_is_float(source)) valid = true;
        /* bool ↔ integer */
        if ((tgt_eff->kind == TYPE_BOOL && type_is_integer(source)) ||
            (type_is_integer(target) && src_eff->kind == TYPE_BOOL)) valid = true;
        /* pointer ↔ pointer: (*Motor)ctx, (*opaque)sensor — with safety checks */
        if ((tgt_eff->kind == TYPE_POINTER || tgt_eff->kind == TYPE_OPAQUE) &&
            (src_eff->kind == TYPE_POINTER || src_eff->kind == TYPE_OPAQUE)) {
            valid = true;

            /* BUG-448: const stripping — same check as @ptrcast BUG-304 */
            if (src_eff->kind == TYPE_POINTER && src_eff->pointer.is_const &&
                tgt_eff->kind == TYPE_POINTER && !tgt_eff->pointer.is_const) {
                checker_error(c, node->loc.line,
                    "cast cannot strip const qualifier — target must be const pointer");
            }

            /* BUG-447: volatile stripping — same check as @ptrcast BUG-258 */
            check_volatile_strip(c, node->typecast.expr, source, target, node->loc.line, "cast");

            /* BUG-446: provenance check — same as @ptrcast provenance tracking.
             * When source is *opaque with known provenance, target must match. */
            if (src_eff->kind == TYPE_POINTER &&
                src_eff->pointer.inner->kind == TYPE_OPAQUE &&
                tgt_eff->kind == TYPE_POINTER) {
                Type *prov_type = NULL;
                if (node->typecast.expr->kind == NODE_IDENT) {
                    Symbol *src_sym = scope_lookup(c->current_scope,
                        node->typecast.expr->ident.name,
                        (uint32_t)node->typecast.expr->ident.name_len);
                    if (src_sym) prov_type = src_sym->provenance_type;
                }
                if (!prov_type) {
                    ExprKey skey = build_expr_key_a(c, node->typecast.expr);
                    if (skey.len > 0)
                        prov_type = prov_map_get(c, skey.str, (uint32_t)skey.len);
                }
                if (prov_type) {
                    Type *prov = type_unwrap_distinct(prov_type);
                    if (tgt_eff->kind == TYPE_POINTER && prov &&
                        prov->kind == TYPE_POINTER) {
                        Type *prov_inner = type_unwrap_distinct(prov->pointer.inner);
                        Type *tgt_inner = type_unwrap_distinct(tgt_eff->pointer.inner);
                        if (prov_inner && tgt_inner &&
                            tgt_inner->kind != TYPE_OPAQUE &&
                            !type_equals(prov_inner, tgt_inner)) {
                            checker_error(c, node->loc.line,
                                "cast type mismatch: source has provenance '*%s' "
                                "but target is '*%s'",
                                type_name(prov_inner), type_name(tgt_inner));
                        }
                    }
                }
            }

            /* BUG-449: *A to *B direct cast (not through *opaque) — reject.
             * Must go through *opaque round-trip for provenance tracking. */
            if (src_eff->kind == TYPE_POINTER && tgt_eff->kind == TYPE_POINTER &&
                src_eff->pointer.inner->kind != TYPE_OPAQUE &&
                tgt_eff->pointer.inner->kind != TYPE_OPAQUE) {
                Type *src_inner = type_unwrap_distinct(src_eff->pointer.inner);
                Type *tgt_inner = type_unwrap_distinct(tgt_eff->pointer.inner);
                if (src_inner && tgt_inner && !type_equals(src_inner, tgt_inner)) {
                    checker_error(c, node->loc.line,
                        "cannot cast '*%s' to '*%s' — use *opaque round-trip "
                        "for type-punning",
                        type_name(src_inner), type_name(tgt_inner));
                }
            }

            /* BUG-446: set provenance when casting *T → *opaque.
             * Walk through &expr to find root ident for provenance. */
            if (src_eff->kind == TYPE_POINTER &&
                src_eff->pointer.inner->kind != TYPE_OPAQUE &&
                (tgt_eff->kind == TYPE_OPAQUE ||
                 (tgt_eff->kind == TYPE_POINTER && tgt_eff->pointer.inner->kind == TYPE_OPAQUE))) {
                Node *prov_src = node->typecast.expr;
                /* walk through &, field, index to find root ident */
                while (prov_src) {
                    if (prov_src->kind == NODE_UNARY && prov_src->unary.op == TOK_AMP)
                        prov_src = prov_src->unary.operand;
                    else if (prov_src->kind == NODE_FIELD)
                        prov_src = prov_src->field.object;
                    else if (prov_src->kind == NODE_INDEX)
                        prov_src = prov_src->index_expr.object;
                    else break;
                }
                if (prov_src && prov_src->kind == NODE_IDENT) {
                    Symbol *src_sym = scope_lookup(c->current_scope,
                        prov_src->ident.name,
                        (uint32_t)prov_src->ident.name_len);
                    if (src_sym) {
                        src_sym->provenance_type = source;
                    }
                }
            }
        }
        /* BUG-450: integer → pointer — reject, use @inttoptr for MMIO safety */
        if (tgt_eff->kind == TYPE_POINTER && type_is_integer(source)) {
            checker_error(c, node->loc.line,
                "cannot cast integer to pointer — use @inttoptr(*T, addr) "
                "with mmio range declaration");
        }
        /* BUG-451: pointer → integer — reject, use @ptrtoint */
        if (type_is_integer(target) && (src_eff->kind == TYPE_POINTER || src_eff->kind == TYPE_OPAQUE)) {
            checker_error(c, node->loc.line,
                "cannot cast pointer to integer — use @ptrtoint(ptr)");
        }
        /* distinct typedef: (Celsius)raw_u32, (u32)celsius */
        if (target->kind == TYPE_DISTINCT || source->kind == TYPE_DISTINCT) valid = true;

        if (!valid) {
            checker_error(c, node->loc.line,
                "invalid cast from '%s' to '%s'", type_name(source), type_name(target));
        }

        result = target;
        break;
    }

    /* ---- Designated struct init ---- */
    case NODE_STRUCT_INIT: {
        /* Type-check all field value expressions.
         * Actual struct field validation is deferred to var-decl/assign
         * because the target struct type is only known from context. */
        for (int i = 0; i < node->struct_init.field_count; i++) {
            check_expr(c, node->struct_init.fields[i].value);
        }
        /* No type returned — context (var-decl, assign) sets the real type */
        result = ty_void;
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
                        /* BUG-258: volatile stripping via @ptrcast */
                        check_volatile_strip(c, node->intrinsic.args[0], val_type, result,
                                             node->loc.line, "@ptrcast");
                        /* provenance check: compile-time belt.
                         * Simple idents: check Symbol. Compound paths (h.p, arr[0]):
                         * check prov_map. BUG-393 runtime type_id is the suspenders. */
                        if (eff->kind == TYPE_POINTER &&
                            eff->pointer.inner->kind == TYPE_OPAQUE) {
                            Type *prov_type = NULL;
                            /* try simple ident first */
                            if (node->intrinsic.args[0]->kind == NODE_IDENT) {
                                Symbol *src_sym = scope_lookup(c->current_scope,
                                    node->intrinsic.args[0]->ident.name,
                                    (uint32_t)node->intrinsic.args[0]->ident.name_len);
                                if (src_sym) prov_type = src_sym->provenance_type;
                            }
                            /* BUG-393: try compound key map (h.p, arr[0]) */
                            if (!prov_type) {
                                ExprKey skey = build_expr_key_a(c, node->intrinsic.args[0]);
                                if (skey.len > 0)
                                    prov_type = prov_map_get(c, skey.str, (uint32_t)skey.len);
                            }
                            if (prov_type) {
                                Type *prov = type_unwrap_distinct(prov_type);
                                Type *tgt = type_unwrap_distinct(result);
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
                                            type_name(prov_type),
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
                    check_volatile_strip(c, node->intrinsic.args[0], val_type, result,
                                         node->loc.line, "@bitcast");
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
                        /* alignment check: constant address must be aligned to target type */
                        if (result) {
                            Type *inner = type_unwrap_distinct(result);
                            if (inner->kind == TYPE_POINTER && inner->pointer.inner) {
                                int w = type_width(inner->pointer.inner);
                                int align = w > 0 ? w / 8 : 0;
                                if (align > 1 && (addr % (uint64_t)align) != 0) {
                                    checker_error(c, node->loc.line,
                                        "@inttoptr address 0x%llx is not aligned to %d bytes (required for %s)",
                                        (unsigned long long)addr, align, type_name(inner->pointer.inner));
                                }
                            }
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
            /* cross-platform warning: if @ptrtoint result is used in a context
             * that narrows to a fixed-width type, warn about portability.
             * Checked at NODE_VAR_DECL init below — flag the node here. */
        } else if ((nlen == 7 && memcmp(name, "barrier", 7) == 0) ||
                   (nlen == 13 && memcmp(name, "barrier_store", 13) == 0) ||
                   (nlen == 12 && memcmp(name, "barrier_load", 12) == 0)) {
            result = ty_void;
        } else if (nlen == 4 && memcmp(name, "trap", 4) == 0) {
            result = ty_void;
        } else if (nlen == 5 && memcmp(name, "probe", 5) == 0) {
            /* @probe(addr) → ?u32: try reading MMIO address, null if faults */
            if (node->intrinsic.arg_count != 1) {
                checker_error(c, node->loc.line, "@probe requires exactly 1 argument (address)");
            } else {
                Type *addr_type = typemap_get(c, node->intrinsic.args[0]);
                if (addr_type && !type_is_integer(addr_type)) {
                    checker_error(c, node->loc.line,
                        "@probe argument must be integer address, got '%s'", type_name(addr_type));
                }
            }
            result = type_optional(c->arena, ty_u32);
        } else if (nlen >= 9 && memcmp(name, "atomic_", 7) == 0) {
            /* BUG-427: was >= 10, but @atomic_or is 9 chars */
            /* @atomic_add, @atomic_sub, @atomic_or, @atomic_and, @atomic_xor,
             * @atomic_load, @atomic_store, @atomic_cas */
            const char *op = name + 7;
            int oplen = nlen - 7;
            bool is_load = (oplen == 4 && memcmp(op, "load", 4) == 0);
            bool is_store = (oplen == 5 && memcmp(op, "store", 5) == 0);
            bool is_cas = (oplen == 3 && memcmp(op, "cas", 3) == 0);
            bool is_arith = (oplen == 3 && (memcmp(op, "add", 3) == 0 || memcmp(op, "sub", 3) == 0 ||
                             memcmp(op, "and", 3) == 0 || memcmp(op, "xor", 3) == 0)) ||
                            (oplen == 2 && memcmp(op, "or", 2) == 0);
            if (!is_load && !is_store && !is_cas && !is_arith) {
                checker_error(c, node->loc.line, "unknown atomic intrinsic '@%.*s'", (int)nlen, name);
            }
            if (is_load) {
                /* @atomic_load(&var) → T */
                if (node->intrinsic.arg_count != 1)
                    checker_error(c, node->loc.line, "@atomic_load requires 1 argument");
                else {
                    Type *at = typemap_get(c, node->intrinsic.args[0]);
                    if (at && at->kind == TYPE_POINTER && type_is_integer(at->pointer.inner)) {
                        result = at->pointer.inner;
                        int aw = type_width(at->pointer.inner);
                        if (aw != 8 && aw != 16 && aw != 32 && aw != 64) {
                            checker_error(c, node->loc.line,
                                "@atomic_load target must be 1, 2, 4, or 8 bytes (got %d-bit type)", aw);
                        } else if (aw == 64) {
                            checker_warning(c, node->loc.line,
                                "@atomic_load on 64-bit type may require libatomic on 32-bit targets");
                        }
                    } else {
                        checker_error(c, node->loc.line, "@atomic_load argument must be pointer to integer");
                        result = ty_u32;
                    }
                }
            } else if (is_store) {
                /* @atomic_store(&var, val) → void */
                if (node->intrinsic.arg_count != 2)
                    checker_error(c, node->loc.line, "@atomic_store requires 2 arguments");
                result = ty_void;
            } else if (is_cas) {
                /* @atomic_cas(&var, expected, desired) → bool */
                if (node->intrinsic.arg_count != 3)
                    checker_error(c, node->loc.line, "@atomic_cas requires 3 arguments");
                result = ty_bool;
            } else {
                /* @atomic_add/sub/or/and/xor(&var, val) → T (old value) */
                if (node->intrinsic.arg_count != 2)
                    checker_error(c, node->loc.line, "@%.*s requires 2 arguments", (int)nlen, name);
                else {
                    Type *at = typemap_get(c, node->intrinsic.args[0]);
                    if (at && at->kind == TYPE_POINTER && type_is_integer(at->pointer.inner)) {
                        result = at->pointer.inner;
                        /* validate atomic width: must be 1, 2, 4, or 8 bytes */
                        int aw = type_width(at->pointer.inner);
                        if (aw != 8 && aw != 16 && aw != 32 && aw != 64) {
                            checker_error(c, node->loc.line,
                                "@%.*s target must be 1, 2, 4, or 8 bytes (got %d-bit type)",
                                (int)nlen, name, aw);
                        } else if (aw == 64) {
                            checker_warning(c, node->loc.line,
                                "@%.*s on 64-bit type may require libatomic on 32-bit targets "
                                "(AVR, Cortex-M0, RISC-V without A extension)",
                                (int)nlen, name);
                        }
                    } else {
                        checker_error(c, node->loc.line, "@%.*s first argument must be pointer to integer", (int)nlen, name);
                        result = ty_u32;
                    }
                }
            }
            /* BUG-493: reject @atomic_* on packed struct fields.
             * Packed fields can be misaligned. GCC __atomic builtins
             * require natural alignment — misaligned = hard fault on ARM/RISC-V. */
            if (node->intrinsic.arg_count >= 1) {
                Node *aarg = node->intrinsic.args[0];
                if (aarg->kind == NODE_UNARY && aarg->unary.op == TOK_AMP &&
                    aarg->unary.operand->kind == NODE_FIELD) {
                    /* Walk to root ident to find if struct is packed */
                    Node *root = aarg->unary.operand;
                    while (root->kind == NODE_FIELD) root = root->field.object;
                    while (root->kind == NODE_INDEX) root = root->index_expr.object;
                    if (root->kind == NODE_IDENT) {
                        Symbol *sym = scope_lookup(c->current_scope,
                            root->ident.name, (uint32_t)root->ident.name_len);
                        if (sym && sym->type) {
                            Type *st = type_unwrap_distinct(sym->type);
                            if (st->kind == TYPE_STRUCT && st->struct_type.is_packed) {
                                checker_error(c, node->loc.line,
                                    "@%.*s on packed struct field — may be misaligned. "
                                    "Atomic operations require natural alignment. "
                                    "Use a non-packed struct or copy to aligned local first",
                                    (int)nlen, name);
                            }
                        }
                    }
                }
            }
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
                    Type *ptr_type = typemap_get(c, node->intrinsic.args[0]);
                    if (ptr_type)
                        check_volatile_strip(c, node->intrinsic.args[0], ptr_type, result,
                                             node->loc.line, "@container");
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
                    /* BUG-343: @cast cannot strip volatile or const qualifier */
                    if (node->intrinsic.arg_count > 0)
                        check_volatile_strip(c, node->intrinsic.args[0], val_type, result,
                                             node->loc.line, "@cast");
                    /* const check */
                    {
                        Type *veff = type_unwrap_distinct(val_type);
                        Type *reff = type_unwrap_distinct(result);
                        if (veff && veff->kind == TYPE_POINTER &&
                            reff && reff->kind == TYPE_POINTER &&
                            veff->pointer.is_const && !reff->pointer.is_const) {
                            checker_error(c, node->loc.line,
                                "@cast cannot strip const qualifier — "
                                "target must be const pointer");
                        }
                    }
                }
            } else {
                result = ty_void;
            }
        } else if (nlen >= 5 && memcmp(name, "cond_", 5) == 0) {
            /* @cond_wait(shared_var, condition), @cond_signal(shared_var), @cond_broadcast(shared_var) */
            const char *cop = name + 5;
            int coplen = nlen - 5;
            bool is_wait = (coplen == 4 && memcmp(cop, "wait", 4) == 0);
            bool is_timedwait = (coplen == 9 && memcmp(cop, "timedwait", 9) == 0);
            bool is_signal = (coplen == 6 && memcmp(cop, "signal", 6) == 0);
            bool is_broadcast = (coplen == 9 && memcmp(cop, "broadcast", 9) == 0);
            if (!is_wait && !is_timedwait && !is_signal && !is_broadcast) {
                checker_error(c, node->loc.line,
                    "unknown condvar intrinsic '@%.*s' — use @cond_wait, @cond_timedwait, @cond_signal, or @cond_broadcast",
                    (int)nlen, name);
            }
            if (is_wait) {
                if (node->intrinsic.arg_count != 2)
                    checker_error(c, node->loc.line,
                        "@cond_wait requires 2 arguments: @cond_wait(shared_var, condition)");
            } else if (is_timedwait) {
                if (node->intrinsic.arg_count != 3)
                    checker_error(c, node->loc.line,
                        "@cond_timedwait requires 3 arguments: @cond_timedwait(shared_var, condition, timeout_ms)");
                /* Type-check the timeout arg (must be integer) */
                if (node->intrinsic.arg_count >= 3) {
                    Type *tt = check_expr(c, node->intrinsic.args[2]);
                    if (tt && !type_is_integer(type_unwrap_distinct(tt)))
                        checker_error(c, node->loc.line,
                            "@cond_timedwait timeout must be an integer (milliseconds)");
                }
            } else {
                if (node->intrinsic.arg_count != 1)
                    checker_error(c, node->loc.line,
                        "@%.*s requires 1 argument: @%.*s(shared_var)",
                        (int)nlen, name, (int)nlen, name);
            }
            /* Validate first arg is a shared struct variable */
            if (node->intrinsic.arg_count >= 1) {
                Type *sarg = check_expr(c, node->intrinsic.args[0]);
                if (sarg) {
                    Type *seff = type_unwrap_distinct(sarg);
                    bool ok = false;
                    if (seff->kind == TYPE_STRUCT && seff->struct_type.is_shared) ok = true;
                    if (seff->kind == TYPE_POINTER) {
                        Type *inner = type_unwrap_distinct(seff->pointer.inner);
                        if (inner && inner->kind == TYPE_STRUCT && inner->struct_type.is_shared) ok = true;
                    }
                    if (!ok) {
                        checker_error(c, node->loc.line,
                            "@%.*s first argument must be a shared struct variable",
                            (int)nlen, name);
                    }
                }
            }
            /* Type-check the condition expression for @cond_wait */
            /* @cond_timedwait returns ?void — null on timeout, has_value on success.
             * Also register condvar type for cond_timedwait */
            if (is_timedwait) {
                result = type_optional(c->arena, ty_void);
            }
            if ((is_wait || is_timedwait) && node->intrinsic.arg_count >= 2) {
                Type *ct = check_expr(c, node->intrinsic.args[1]);
                if (ct) {
                    Type *ceff = type_unwrap_distinct(ct);
                    if (ceff->kind != TYPE_BOOL && !type_is_integer(ceff)) {
                        checker_error(c, node->loc.line,
                            "@cond_wait condition must be bool or integer expression");
                    }
                }
            }
            if (!is_timedwait) result = ty_void;
        } else if (nlen >= 8 && memcmp(name, "barrier_", 8) == 0) {
            /* @barrier_init(var, count), @barrier_wait(var) */
            const char *bop = name + 8;
            int boplen = nlen - 8;
            bool is_binit = (boplen == 4 && memcmp(bop, "init", 4) == 0);
            bool is_bwait = (boplen == 4 && memcmp(bop, "wait", 4) == 0);
            if (!is_binit && !is_bwait) {
                checker_error(c, node->loc.line,
                    "unknown barrier intrinsic '@%.*s' — use @barrier_init or @barrier_wait",
                    (int)nlen, name);
            }
            if (is_binit) {
                if (node->intrinsic.arg_count != 2)
                    checker_error(c, node->loc.line,
                        "@barrier_init requires 2 arguments: @barrier_init(barrier_var, thread_count)");
                if (node->intrinsic.arg_count >= 2) {
                    Type *bt = check_expr(c, node->intrinsic.args[0]);
                    /* Validate first arg is Barrier or *Barrier */
                    Type *bt_eff = bt ? type_unwrap_distinct(bt) : NULL;
                    if (bt_eff && bt_eff->kind == TYPE_POINTER)
                        bt_eff = type_unwrap_distinct(bt_eff->pointer.inner);
                    if (bt_eff && bt_eff->kind != TYPE_BARRIER)
                        checker_error(c, node->loc.line,
                            "@barrier_init first argument must be Barrier type, got '%s'",
                            type_name(bt));
                    Type *ct = check_expr(c, node->intrinsic.args[1]);
                    if (ct && !type_is_integer(type_unwrap_distinct(ct)))
                        checker_error(c, node->loc.line,
                            "@barrier_init count must be an integer");
                }
            } else if (is_bwait) {
                if (node->intrinsic.arg_count != 1)
                    checker_error(c, node->loc.line,
                        "@barrier_wait requires 1 argument: @barrier_wait(barrier_var)");
                if (node->intrinsic.arg_count >= 1) {
                    Type *bt = check_expr(c, node->intrinsic.args[0]);
                    Type *bt_eff = bt ? type_unwrap_distinct(bt) : NULL;
                    if (bt_eff && bt_eff->kind == TYPE_POINTER)
                        bt_eff = type_unwrap_distinct(bt_eff->pointer.inner);
                    if (bt_eff && bt_eff->kind != TYPE_BARRIER)
                        checker_error(c, node->loc.line,
                            "@barrier_wait argument must be Barrier type, got '%s'",
                            type_name(bt));
                }
            }
            result = ty_void;
        } else if (nlen == 11 && memcmp(name, "sem_acquire", 11) == 0) {
            /* @sem_acquire(semaphore_var) — decrement, block if zero */
            if (node->intrinsic.arg_count != 1)
                checker_error(c, node->loc.line,
                    "@sem_acquire requires 1 argument");
            if (node->intrinsic.arg_count >= 1) {
                Type *st = check_expr(c, node->intrinsic.args[0]);
                Type *st_eff = st ? type_unwrap_distinct(st) : NULL;
                if (st_eff && st_eff->kind == TYPE_POINTER)
                    st_eff = type_unwrap_distinct(st_eff->pointer.inner);
                if (st_eff && st_eff->kind != TYPE_SEMAPHORE)
                    checker_error(c, node->loc.line,
                        "@sem_acquire argument must be Semaphore type, got '%s'",
                        type_name(st));
            }
            result = ty_void;
        } else if (nlen == 11 && memcmp(name, "sem_release", 11) == 0) {
            /* @sem_release(semaphore_var) — increment, wake one waiter */
            if (node->intrinsic.arg_count != 1)
                checker_error(c, node->loc.line,
                    "@sem_release requires 1 argument");
            if (node->intrinsic.arg_count >= 1) {
                Type *st = check_expr(c, node->intrinsic.args[0]);
                Type *st_eff = st ? type_unwrap_distinct(st) : NULL;
                if (st_eff && st_eff->kind == TYPE_POINTER)
                    st_eff = type_unwrap_distinct(st_eff->pointer.inner);
                if (st_eff && st_eff->kind != TYPE_SEMAPHORE)
                    checker_error(c, node->loc.line,
                        "@sem_release argument must be Semaphore type, got '%s'",
                        type_name(st));
            }
            result = ty_void;
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

/* ================================================================
 * FUNCTION SUMMARIES (FuncProps) — inferred function properties
 *
 * Lazy DFS: scan function body, follow callees transitively, cache on Symbol.
 * Used by @critical/defer/interrupt to check for banned operations.
 * Replaces has_atomic_or_barrier() standalone scanner.
 * See docs/FunctionSummaries.md for full design.
 * ================================================================ */

/* Forward declaration — mutual recursion with scan_func_props */
static void ensure_func_props(Checker *c, Symbol *sym);

/* Recursive AST walker: collect all function properties in one pass.
 * Sets bools on props for: yield, spawn, alloc, sync.
 * Follows direct function calls transitively via ensure_func_props. */
static void scan_func_props(Checker *c, Node *node, Symbol *parent_sym) {
    if (!node) return;
    /* Short-circuit: if all properties already found, stop scanning */
    if (parent_sym->props.can_yield && parent_sym->props.can_spawn &&
        parent_sym->props.can_alloc && parent_sym->props.has_sync)
        return;

    switch (node->kind) {
    /* --- Direct effect detection --- */
    case NODE_YIELD:
    case NODE_AWAIT:
        parent_sym->props.can_yield = true;
        return;

    case NODE_SPAWN:
        parent_sym->props.can_spawn = true;
        /* Also scan spawn args for effects */
        for (int i = 0; i < node->spawn_stmt.arg_count; i++)
            scan_func_props(c, node->spawn_stmt.args[i], parent_sym);
        return;

    /* --- Sync detection (absorbs has_atomic_or_barrier) --- */
    case NODE_INTRINSIC: {
        const char *n = node->intrinsic.name;
        uint32_t nl = (uint32_t)node->intrinsic.name_len;
        if ((nl >= 7 && memcmp(n, "atomic_", 7) == 0) ||
            (nl == 7 && memcmp(n, "barrier", 7) == 0) ||
            (nl == 13 && memcmp(n, "barrier_store", 13) == 0) ||
            (nl == 12 && memcmp(n, "barrier_load", 12) == 0))
            parent_sym->props.has_sync = true;
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            scan_func_props(c, node->intrinsic.args[i], parent_sym);
        return;
    }

    /* --- Call: detect alloc + follow callee transitively --- */
    case NODE_CALL:
        /* Scan callee expression and arguments */
        if (node->call.callee)
            scan_func_props(c, node->call.callee, parent_sym);
        for (int i = 0; i < node->call.arg_count; i++)
            scan_func_props(c, node->call.args[i], parent_sym);

        /* Detect alloc: slab.alloc(), slab.alloc_ptr(), Task.new(), Task.new_ptr() */
        if (node->call.callee && node->call.callee->kind == NODE_FIELD) {
            const char *mn = node->call.callee->field.field_name;
            uint32_t ml = (uint32_t)node->call.callee->field.field_name_len;
            if ((ml == 5 && memcmp(mn, "alloc", 5) == 0) ||
                (ml == 9 && memcmp(mn, "alloc_ptr", 9) == 0) ||
                (ml == 3 && memcmp(mn, "new", 3) == 0) ||
                (ml == 7 && memcmp(mn, "new_ptr", 7) == 0))
                parent_sym->props.can_alloc = true;
        }

        /* Transitive: follow direct function calls */
        if (node->call.callee && node->call.callee->kind == NODE_IDENT) {
            Symbol *callee = scope_lookup(c->global_scope,
                node->call.callee->ident.name,
                (uint32_t)node->call.callee->ident.name_len);
            if (callee && callee->is_function) {
                ensure_func_props(c, callee);
                if (callee->props.can_yield) parent_sym->props.can_yield = true;
                if (callee->props.can_spawn) parent_sym->props.can_spawn = true;
                if (callee->props.can_alloc) parent_sym->props.can_alloc = true;
                if (callee->props.has_sync)  parent_sym->props.has_sync = true;
            }
        }

        /* Module-qualified calls: config.func() rewritten to NODE_FIELD(NODE_IDENT, field).
         * The field name is the function name after module prefix rewrite. */
        if (node->call.callee && node->call.callee->kind == NODE_FIELD &&
            node->call.callee->field.object &&
            node->call.callee->field.object->kind == NODE_IDENT) {
            /* Try mangled name: module__func */
            const char *mod = node->call.callee->field.object->ident.name;
            uint32_t modl = (uint32_t)node->call.callee->field.object->ident.name_len;
            const char *fn = node->call.callee->field.field_name;
            uint32_t fnl = (uint32_t)node->call.callee->field.field_name_len;
            char mangled[256];
            if (modl + 2 + fnl < sizeof(mangled)) {
                memcpy(mangled, mod, modl);
                mangled[modl] = '_'; mangled[modl+1] = '_';
                memcpy(mangled + modl + 2, fn, fnl);
                Symbol *callee = scope_lookup(c->global_scope, mangled, modl + 2 + fnl);
                if (callee && callee->is_function) {
                    ensure_func_props(c, callee);
                    if (callee->props.can_yield) parent_sym->props.can_yield = true;
                    if (callee->props.can_spawn) parent_sym->props.can_spawn = true;
                    if (callee->props.can_alloc) parent_sym->props.can_alloc = true;
                    if (callee->props.has_sync)  parent_sym->props.has_sync = true;
                }
            }
        }
        return;

    /* --- Recursive walk into child nodes --- */
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            scan_func_props(c, node->block.stmts[i], parent_sym);
        return;
    case NODE_IF:
        scan_func_props(c, node->if_stmt.cond, parent_sym);
        scan_func_props(c, node->if_stmt.then_body, parent_sym);
        scan_func_props(c, node->if_stmt.else_body, parent_sym);
        return;
    case NODE_FOR:
        scan_func_props(c, node->for_stmt.init, parent_sym);
        scan_func_props(c, node->for_stmt.cond, parent_sym);
        scan_func_props(c, node->for_stmt.step, parent_sym);
        scan_func_props(c, node->for_stmt.body, parent_sym);
        return;
    case NODE_WHILE: case NODE_DO_WHILE:
        scan_func_props(c, node->while_stmt.cond, parent_sym);
        scan_func_props(c, node->while_stmt.body, parent_sym);
        return;
    case NODE_SWITCH:
        scan_func_props(c, node->switch_stmt.expr, parent_sym);
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            scan_func_props(c, node->switch_stmt.arms[i].body, parent_sym);
        return;
    case NODE_RETURN:
        scan_func_props(c, node->ret.expr, parent_sym);
        return;
    case NODE_EXPR_STMT:
        scan_func_props(c, node->expr_stmt.expr, parent_sym);
        return;
    case NODE_VAR_DECL:
        scan_func_props(c, node->var_decl.init, parent_sym);
        return;
    case NODE_ASSIGN:
        scan_func_props(c, node->assign.target, parent_sym);
        scan_func_props(c, node->assign.value, parent_sym);
        return;
    case NODE_BINARY:
        scan_func_props(c, node->binary.left, parent_sym);
        scan_func_props(c, node->binary.right, parent_sym);
        return;
    case NODE_UNARY:
        scan_func_props(c, node->unary.operand, parent_sym);
        return;
    case NODE_FIELD:
        scan_func_props(c, node->field.object, parent_sym);
        return;
    case NODE_INDEX:
        scan_func_props(c, node->index_expr.object, parent_sym);
        scan_func_props(c, node->index_expr.index, parent_sym);
        return;
    case NODE_ORELSE:
        scan_func_props(c, node->orelse.expr, parent_sym);
        return;
    case NODE_DEFER:
        scan_func_props(c, node->defer.body, parent_sym);
        return;
    case NODE_CRITICAL:
        scan_func_props(c, node->critical.body, parent_sym);
        return;
    case NODE_ONCE:
        scan_func_props(c, node->once.body, parent_sym);
        return;
    default:
        return;
    }
}

/* Lazy compute function properties with DFS cycle detection.
 * Call this before reading sym->props. Idempotent — second call is O(1). */
static void ensure_func_props(Checker *c, Symbol *sym) {
    if (!sym || !sym->is_function) return;
    if (sym->props.computed) return;
    if (sym->props.in_progress) return; /* cycle — conservative (no additional effects) */
    if (!sym->func_node) return;        /* no body (cinclude, forward decl without body) */

    sym->props.in_progress = true;

    Node *body = NULL;
    if (sym->func_node->kind == NODE_FUNC_DECL)
        body = sym->func_node->func_decl.body;
    else if (sym->func_node->kind == NODE_INTERRUPT)
        body = sym->func_node->interrupt.body;

    if (body) {
        scan_func_props(c, body, sym);
    }

    sym->props.in_progress = false;
    sym->props.computed = true;
}

/* Check a body subtree for banned effects. Used at context entry points
 * (@critical, defer, interrupt). Creates a temporary scan, follows callees. */
static void check_body_effects(Checker *c, Node *body, int line,
                                bool ban_yield, const char *yield_msg,
                                bool ban_spawn, const char *spawn_msg,
                                bool ban_alloc, const char *alloc_msg) {
    if (!body) return;
    /* Create a temporary "pseudo-symbol" for scanning this body subtree.
     * We can't use the real function's Symbol because this might be a
     * sub-block (@critical body), not the whole function. */
    Symbol tmp = {0};
    tmp.is_function = true;  /* enable transitive following */
    tmp.props.computed = false;
    tmp.props.in_progress = true; /* prevent re-entry into this temp */
    scan_func_props(c, body, &tmp);

    if (ban_yield && tmp.props.can_yield)
        checker_error(c, line, "%s", yield_msg);
    if (ban_spawn && tmp.props.can_spawn)
        checker_error(c, line, "%s", spawn_msg);
    if (ban_alloc && tmp.props.can_alloc)
        checker_error(c, line, "%s", alloc_msg);
}

/* Check if a function body contains any @atomic_* or @barrier calls.
 * If yes, the developer is doing manual synchronization — race warnings not errors.
 * LEGACY wrapper — uses FuncProps internally now. */
static bool has_atomic_or_barrier(Node *node) {
    if (!node) return false;
    if (node->kind == NODE_INTRINSIC) {
        const char *n = node->intrinsic.name;
        uint32_t nl = (uint32_t)node->intrinsic.name_len;
        if ((nl >= 7 && memcmp(n, "atomic_", 7) == 0) ||
            (nl == 7 && memcmp(n, "barrier", 7) == 0) ||
            (nl == 13 && memcmp(n, "barrier_store", 13) == 0) ||
            (nl == 12 && memcmp(n, "barrier_load", 12) == 0))
            return true;
    }
    switch (node->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            if (has_atomic_or_barrier(node->block.stmts[i])) return true;
        return false;
    case NODE_IF:
        return has_atomic_or_barrier(node->if_stmt.cond) ||
               has_atomic_or_barrier(node->if_stmt.then_body) ||
               has_atomic_or_barrier(node->if_stmt.else_body);
    case NODE_FOR:
        return has_atomic_or_barrier(node->for_stmt.init) ||
               has_atomic_or_barrier(node->for_stmt.cond) ||
               has_atomic_or_barrier(node->for_stmt.step) ||
               has_atomic_or_barrier(node->for_stmt.body);
    case NODE_WHILE: case NODE_DO_WHILE:
        return has_atomic_or_barrier(node->while_stmt.cond) ||
               has_atomic_or_barrier(node->while_stmt.body);
    case NODE_EXPR_STMT:
        return has_atomic_or_barrier(node->expr_stmt.expr);
    case NODE_RETURN:
        return has_atomic_or_barrier(node->ret.expr);
    case NODE_DEFER:
        return has_atomic_or_barrier(node->defer.body);
    case NODE_BINARY:
        return has_atomic_or_barrier(node->binary.left) ||
               has_atomic_or_barrier(node->binary.right);
    case NODE_UNARY:
        return has_atomic_or_barrier(node->unary.operand);
    case NODE_CALL:
        if (has_atomic_or_barrier(node->call.callee)) return true;
        for (int i = 0; i < node->call.arg_count; i++)
            if (has_atomic_or_barrier(node->call.args[i])) return true;
        return false;
    case NODE_ASSIGN:
        return has_atomic_or_barrier(node->assign.target) ||
               has_atomic_or_barrier(node->assign.value);
    case NODE_VAR_DECL:
        return has_atomic_or_barrier(node->var_decl.init);
    case NODE_ORELSE:
        return has_atomic_or_barrier(node->orelse.expr);
    case NODE_SWITCH:
        if (has_atomic_or_barrier(node->switch_stmt.expr)) return true;
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            if (has_atomic_or_barrier(node->switch_stmt.arms[i].body)) return true;
        return false;
    default: return false;
    }
}

/* Check if a function body accesses non-shared, non-const, non-threadlocal globals.
 * Used to validate spawn targets — accessing such globals from a spawned thread is a data race. */
static bool scan_unsafe_global_access(Checker *c, Node *node,
                                       const char **out_name, uint32_t *out_len) {
    if (!node) return false;
    if (node->kind == NODE_IDENT) {
        Symbol *sym = scope_lookup(c->global_scope,
            node->ident.name, (uint32_t)node->ident.name_len);
        if (sym && !sym->is_function && sym->type) {
            /* Skip: const, volatile (explicit low-level opt-in), threadlocal,
             * shared, Pool/Slab/Ring/Arena/Barrier */
            if (sym->is_const) return false;
            if (sym->is_volatile) return false;
            /* threadlocal: check the AST node for is_threadlocal flag */
            if (sym->func_node &&
                (sym->func_node->kind == NODE_VAR_DECL || sym->func_node->kind == NODE_GLOBAL_VAR) &&
                sym->func_node->var_decl.is_threadlocal) return false;
            Type *t = type_unwrap_distinct(sym->type);
            if (t->kind == TYPE_STRUCT && (t->struct_type.is_shared || t->struct_type.is_shared_rw))
                return false;
            /* Arena (bump allocator) and Barrier (has own mutex) are safe.
             * Pool, Slab, Ring are NOT thread-safe — alloc/free/push/pop have
             * non-atomic metadata access. Must use from single thread or wrap
             * in shared struct. */
            if (t->kind == TYPE_ARENA || t->kind == TYPE_BARRIER || t->kind == TYPE_SEMAPHORE)
                return false;
            /* Non-shared global — potential data race */
            *out_name = node->ident.name;
            *out_len = (uint32_t)node->ident.name_len;
            return true;
        }
        return false;
    }
    /* Recurse into children */
    switch (node->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            if (scan_unsafe_global_access(c, node->block.stmts[i], out_name, out_len)) return true;
        return false;
    case NODE_IF:
        if (scan_unsafe_global_access(c, node->if_stmt.cond, out_name, out_len)) return true;
        if (scan_unsafe_global_access(c, node->if_stmt.then_body, out_name, out_len)) return true;
        return scan_unsafe_global_access(c, node->if_stmt.else_body, out_name, out_len);
    case NODE_FOR:
        if (scan_unsafe_global_access(c, node->for_stmt.init, out_name, out_len)) return true;
        if (scan_unsafe_global_access(c, node->for_stmt.cond, out_name, out_len)) return true;
        if (scan_unsafe_global_access(c, node->for_stmt.step, out_name, out_len)) return true;
        return scan_unsafe_global_access(c, node->for_stmt.body, out_name, out_len);
    case NODE_WHILE: case NODE_DO_WHILE:
        if (scan_unsafe_global_access(c, node->while_stmt.cond, out_name, out_len)) return true;
        return scan_unsafe_global_access(c, node->while_stmt.body, out_name, out_len);
    case NODE_RETURN:
        return scan_unsafe_global_access(c, node->ret.expr, out_name, out_len);
    case NODE_EXPR_STMT:
        return scan_unsafe_global_access(c, node->expr_stmt.expr, out_name, out_len);
    case NODE_VAR_DECL:
        return scan_unsafe_global_access(c, node->var_decl.init, out_name, out_len);
    case NODE_ASSIGN:
        if (scan_unsafe_global_access(c, node->assign.target, out_name, out_len)) return true;
        return scan_unsafe_global_access(c, node->assign.value, out_name, out_len);
    case NODE_BINARY:
        if (scan_unsafe_global_access(c, node->binary.left, out_name, out_len)) return true;
        return scan_unsafe_global_access(c, node->binary.right, out_name, out_len);
    case NODE_UNARY:
        return scan_unsafe_global_access(c, node->unary.operand, out_name, out_len);
    case NODE_FIELD:
        return scan_unsafe_global_access(c, node->field.object, out_name, out_len);
    case NODE_INDEX:
        if (scan_unsafe_global_access(c, node->index_expr.object, out_name, out_len)) return true;
        return scan_unsafe_global_access(c, node->index_expr.index, out_name, out_len);
    case NODE_CALL:
        /* Check callee expression (e.g., global_slab.alloc() — global_slab is in callee) */
        if (scan_unsafe_global_access(c, node->call.callee, out_name, out_len)) return true;
        /* Check call arguments for global access */
        for (int i = 0; i < node->call.arg_count; i++)
            if (scan_unsafe_global_access(c, node->call.args[i], out_name, out_len)) return true;
        /* Transitive: follow direct function calls into callee body.
         * This catches helper() accessing non-shared globals from spawned context. */
        if (node->call.callee && node->call.callee->kind == NODE_IDENT) {
            Symbol *csym = scope_lookup(c->global_scope,
                node->call.callee->ident.name, (uint32_t)node->call.callee->ident.name_len);
            if (csym && csym->is_function && csym->func_node &&
                csym->func_node->kind == NODE_FUNC_DECL &&
                csym->func_node->func_decl.body) {
                /* Depth limit to prevent infinite recursion on recursive call chains */
                static int _scan_depth = 0;
                if (_scan_depth < 8) {
                    _scan_depth++;
                    bool found = scan_unsafe_global_access(c,
                        csym->func_node->func_decl.body, out_name, out_len);
                    _scan_depth--;
                    if (found) return true;
                }
            }
        }
        return false;
    case NODE_INTRINSIC: {
        /* Skip @atomic_* intrinsic arguments — atomic ops are thread-safe.
         * This prevents false warnings for @atomic_store(&gflag, 1). */
        const char *iname = node->intrinsic.name;
        uint32_t ilen = (uint32_t)node->intrinsic.name_len;
        if (ilen >= 7 && memcmp(iname, "atomic_", 7) == 0)
            return false; /* all atomic args are safe */
        /* Non-atomic intrinsics: scan arguments normally */
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            if (scan_unsafe_global_access(c, node->intrinsic.args[i], out_name, out_len)) return true;
        return false;
    }
    case NODE_ORELSE:
        return scan_unsafe_global_access(c, node->orelse.expr, out_name, out_len);
    case NODE_DEFER:
        return scan_unsafe_global_access(c, node->defer.body, out_name, out_len);
    case NODE_SWITCH:
        if (scan_unsafe_global_access(c, node->switch_stmt.expr, out_name, out_len)) return true;
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            if (scan_unsafe_global_access(c, node->switch_stmt.arms[i].body, out_name, out_len)) return true;
        return false;
    default:
        return false;
    }
}

/* Check if an expression tree contains yield or await */
static bool expr_contains_yield(Node *n) {
    if (!n) return false;
    if (n->kind == NODE_YIELD || n->kind == NODE_AWAIT) return true;
    switch (n->kind) {
    case NODE_BINARY: return expr_contains_yield(n->binary.left) || expr_contains_yield(n->binary.right);
    case NODE_UNARY: return expr_contains_yield(n->unary.operand);
    case NODE_CALL:
        for (int i = 0; i < n->call.arg_count; i++)
            if (expr_contains_yield(n->call.args[i])) return true;
        return false;
    case NODE_ASSIGN: return expr_contains_yield(n->assign.target) || expr_contains_yield(n->assign.value);
    case NODE_FIELD: return expr_contains_yield(n->field.object);
    case NODE_INDEX: return expr_contains_yield(n->index_expr.object) || expr_contains_yield(n->index_expr.index);
    case NODE_ORELSE: return expr_contains_yield(n->orelse.expr);
    default: return false;
    }
}

static void check_stmt(Checker *c, Node *node) {
    if (!node) return;

    /* In async functions, set in_async_yield_stmt for statements containing yield/await.
     * Shared struct access is only banned in these statements (lock held across suspension). */
    bool saved_yield_stmt = c->in_async_yield_stmt;
    if (c->in_async && !c->in_async_yield_stmt) {
        if (node->kind == NODE_EXPR_STMT && expr_contains_yield(node->expr_stmt.expr))
            c->in_async_yield_stmt = true;
        else if (node->kind == NODE_VAR_DECL && expr_contains_yield(node->var_decl.init))
            c->in_async_yield_stmt = true;
    }

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
        /* Function pointer without initializer — auto-zero creates NULL funcptr.
         * Calling it would segfault. Require init or use ?FuncPtr for nullable. */
        if (!node->var_decl.init && type &&
            type_unwrap_distinct(type)->kind == TYPE_FUNC_PTR) {
            checker_error(c, node->loc.line,
                "function pointer requires an initializer — "
                "use '?' prefix for nullable function pointer");
        }

        typemap_set(c, node,type); /* store for emitter to read via checker_get_type */

        if (node->var_decl.init) {
            Type *init_type = check_expr(c, node->var_decl.init);

            /* non-storable check: pool.get(h) pointer result.
             * BUG-405: only block when result is a pointer — scalar values
             * from Handle auto-deref (h.id, h.count) are safe to store. */
            if (is_non_storable(c, node->var_decl.init)) {
                Type *ns_type = type_unwrap_distinct(init_type);
                if (!ns_type || ns_type->kind == TYPE_POINTER || ns_type->kind == TYPE_SLICE ||
                    ns_type->kind == TYPE_STRUCT || ns_type->kind == TYPE_UNION) {
                    checker_error(c, node->loc.line,
                        "cannot store result of get() — use inline");
                }
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

            /* Designated initializer: validate fields against target struct type */
            if (node->var_decl.init->kind == NODE_STRUCT_INIT && type) {
                if (validate_struct_init(c, node->var_decl.init, type, node->loc.line)) {
                    init_type = type;
                    typemap_set(c, node->var_decl.init, type);
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
                } else if (init_type->kind == TYPE_OPTIONAL &&
                           type_equals(init_type->optional.inner, type)) {
                    /* ?T assigned to T without orelse — suggest unwrap */
                    checker_error(c, node->loc.line,
                        "cannot initialize '%.*s' of type '%s' with '%s' — "
                        "add 'orelse { return; }' to unwrap",
                        (int)node->var_decl.name_len, node->var_decl.name,
                        type_name(type), type_name(init_type));
                } else {
                    checker_error(c, node->loc.line,
                        "cannot initialize '%.*s' of type '%s' with '%s'",
                        (int)node->var_decl.name_len, node->var_decl.name,
                        type_name(type), type_name(init_type));
                }
            }
            /* cross-platform portability: @ptrtoint to fixed-width type is fragile.
             * u32 x = @ptrtoint(ptr) works on 32-bit but loses bits on 64-bit.
             * Warn even if types match on current target — use usize instead. */
            if (type && type_is_integer(type_unwrap_distinct(type)) &&
                type_unwrap_distinct(type)->kind != TYPE_USIZE &&
                node->var_decl.init->kind == NODE_INTRINSIC &&
                node->var_decl.init->intrinsic.name_len == 8 &&
                memcmp(node->var_decl.init->intrinsic.name, "ptrtoint", 8) == 0) {
                checker_warning(c, node->loc.line,
                    "@ptrtoint result stored in '%s' — use 'usize' for portability "
                    "(pointer width varies: 32-bit on ARM Cortex-M, 64-bit on ARM64/x86_64)",
                    type_name(type));
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

        /* BUG-499: reject variable shadowing of function parameters in async functions.
         * In async, params and locals share the state struct — same field name.
         * Shadowing would overwrite the param value. Regular functions are fine
         * (separate stack slots for param and local). */
        if (c->in_async && c->current_func_ret) {
            /* Walk parent scope chain to check if name matches a param */
            Symbol *existing = scope_lookup(c->current_scope,
                node->var_decl.name, (uint32_t)node->var_decl.name_len);
            if (existing && existing->line != node->loc.line) {
                checker_error(c, node->loc.line,
                    "variable '%.*s' shadows function parameter in async function — "
                    "async locals share state struct with params, shadowing overwrites param value. "
                    "Use a different name",
                    (int)node->var_decl.name_len, node->var_decl.name);
            }
        }

        Symbol *sym = add_symbol(c, node->var_decl.name,
                                 (uint32_t)node->var_decl.name_len,
                                 type, node->loc.line);
        if (sym) {
            sym->is_const = node->var_decl.is_const;
            sym->is_volatile = node->var_decl.is_volatile;
            sym->is_static = node->var_decl.is_static;
            /* BUG-430: store AST node for const init lookup (enables
             * const u32 perms = ...; comptime if (FUNC(perms)) pattern) */
            sym->func_node = node;

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
                            /* BUG-455: ALL arena allocs (including global arena)
                             * cannot be stored in globals. Flag separately. */
                            sym->is_from_arena = true;
                        }
                        /* Handle auto-deref: track slab_source from pool/slab.alloc() */
                        if (obj_type && (obj_type->kind == TYPE_POOL || obj_type->kind == TYPE_SLAB)) {
                            if (obj->kind == NODE_IDENT) {
                                Symbol *alloc_src = scope_lookup(c->current_scope,
                                    obj->ident.name, (uint32_t)obj->ident.name_len);
                                if (!alloc_src)
                                    alloc_src = scope_lookup(c->global_scope,
                                        obj->ident.name, (uint32_t)obj->ident.name_len);
                                if (alloc_src) sym->slab_source = alloc_src;
                            }
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
                            /* BUG-421: only propagate local/arena-derived flags when the
                             * target variable can actually carry a pointer. Scalar types
                             * (u32, bool, etc.) can't escape local memory even if the
                             * source struct was marked local-derived. Without this check,
                             * u32 val = struct_result.field falsely inherits the flag. */
                            if (src) propagate_escape_flags(sym, src, type);
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

            /* @ptrtoint(&local) — mark result as local-derived.
             * The integer carries the local's address; if it escapes (return, global store)
             * and is later used with @inttoptr, the pointer dangles. */
            if (node->var_decl.init && node->var_decl.init->kind == NODE_INTRINSIC &&
                node->var_decl.init->intrinsic.name_len == 8 &&
                memcmp(node->var_decl.init->intrinsic.name, "ptrtoint", 8) == 0 &&
                node->var_decl.init->intrinsic.arg_count > 0) {
                Node *ptarg = node->var_decl.init->intrinsic.args[0];
                if (ptarg && ptarg->kind == NODE_UNARY && ptarg->unary.op == TOK_AMP) {
                    Node *root = ptarg->unary.operand;
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
                /* @ptrcast provenance: compile-time belt for simple variables */
                if (init->kind == NODE_INTRINSIC &&
                    init->intrinsic.name_len == 7 &&
                    memcmp(init->intrinsic.name, "ptrcast", 7) == 0 &&
                    init->intrinsic.arg_count > 0) {
                    Type *eff = type_unwrap_distinct(type);
                    if (eff && eff->kind == TYPE_POINTER &&
                        eff->pointer.inner->kind == TYPE_OPAQUE) {
                        Type *src_type = typemap_get(c, init->intrinsic.args[0]);
                        if (src_type) sym->provenance_type = src_type;
                    }
                }
                /* C-style cast to *opaque — same provenance as @ptrcast */
                if (init->kind == NODE_TYPECAST) {
                    Type *eff = type_unwrap_distinct(type);
                    if (eff && eff->kind == TYPE_POINTER &&
                        eff->pointer.inner->kind == TYPE_OPAQUE) {
                        Type *src_type = typemap_get(c, init->typecast.expr);
                        if (src_type) sym->provenance_type = src_type;
                    }
                }
                /* alias propagation for provenance + @container
                 * BUG-358: walk through @bitcast/@cast/NODE_TYPECAST to find root ident */
                {
                    Node *prov_root = init;
                    if (init->kind == NODE_ORELSE) prov_root = init->orelse.expr;
                    while (prov_root) {
                        if (prov_root->kind == NODE_INTRINSIC && prov_root->intrinsic.arg_count > 0)
                            prov_root = prov_root->intrinsic.args[prov_root->intrinsic.arg_count - 1];
                        else if (prov_root->kind == NODE_TYPECAST)
                            prov_root = prov_root->typecast.expr;
                        else break;
                    }
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

            /* Cross-function provenance: if init is a call to a function with
             * known return provenance, propagate to the variable. */
            if (sym && !sym->provenance_type && node->var_decl.init) {
                Node *call_init = node->var_decl.init;
                if (call_init->kind == NODE_ORELSE) call_init = call_init->orelse.expr;
                if (call_init && call_init->kind == NODE_CALL &&
                    call_init->call.callee->kind == NODE_IDENT) {
                    Type *var_eff = type_unwrap_distinct(type);
                    if (var_eff && var_eff->kind == TYPE_POINTER &&
                        var_eff->pointer.inner->kind == TYPE_OPAQUE) {
                        Type *rprov = lookup_prov_summary(c,
                            call_init->call.callee->ident.name,
                            (uint32_t)call_init->call.callee->ident.name_len);
                        if (rprov) sym->provenance_type = rprov;
                    }
                }
            }

            /* @cstr local-derived: p = @cstr(local_buf, "hi") → p is local-derived */
            if (sym && node->var_decl.init &&
                node->var_decl.init->kind == NODE_INTRINSIC &&
                node->var_decl.init->intrinsic.name_len == 4 &&
                memcmp(node->var_decl.init->intrinsic.name, "cstr", 4) == 0 &&
                node->var_decl.init->intrinsic.arg_count > 0) {
                Node *buf = node->var_decl.init->intrinsic.args[0];
                while (buf && (buf->kind == NODE_FIELD || buf->kind == NODE_INDEX)) {
                    if (buf->kind == NODE_FIELD) buf = buf->field.object;
                    else buf = buf->index_expr.object;
                }
                if (buf && buf->kind == NODE_IDENT) {
                    Symbol *bsym = scope_lookup(c->current_scope,
                        buf->ident.name, (uint32_t)buf->ident.name_len);
                    bool is_global = scope_lookup_local(c->global_scope,
                        buf->ident.name, (uint32_t)buf->ident.name_len) != NULL;
                    if (bsym && !bsym->is_static && !is_global)
                        sym->is_local_derived = true;
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

            /* BUG-360/374: function call with local-derived arg — if any pointer
             * arg is local-derived, conservatively mark the result as local-derived.
             * identity(&x) returns &x, must not escape.
             * BUG-374: recurse into nested calls — identity(identity(&x)).
             * BUG-383: also walk through field/index chains — wrap(&x).p.
             * Applies to BOTH pointer results AND struct results (struct may
             * contain pointer fields carrying the local pointer). */
            if (sym && node->var_decl.init && type &&
                (type->kind == TYPE_POINTER || type->kind == TYPE_STRUCT || type->kind == TYPE_SLICE)) {
                Node *call = node->var_decl.init;
                if (call->kind == NODE_ORELSE) call = call->orelse.expr;
                /* BUG-383: walk through field/index to find call root */
                while (call && (call->kind == NODE_FIELD || call->kind == NODE_INDEX)) {
                    if (call->kind == NODE_FIELD) call = call->field.object;
                    else call = call->index_expr.object;
                }
                if (call && call->kind == NODE_CALL) {
                    if (call_has_local_derived_arg(c, call, 0)) {
                        sym->is_local_derived = true;
                    }
                }
            }
        }

        /* MMIO pointer bound: if init is @inttoptr, derive bound from mmio range */
        if (node->var_decl.init && sym && type &&
            type_unwrap_distinct(type)->kind == TYPE_POINTER) {
            Node *init_expr = node->var_decl.init;
            if (init_expr->kind == NODE_INTRINSIC &&
                init_expr->intrinsic.name_len == 8 &&
                memcmp(init_expr->intrinsic.name, "inttoptr", 8) == 0 &&
                init_expr->intrinsic.arg_count > 0) {
                int64_t addr = eval_const_expr(init_expr->intrinsic.args[0]);
                if (addr != CONST_EVAL_FAIL) {
                    for (int ri = 0; ri < c->mmio_range_count; ri++) {
                        if ((uint64_t)addr >= c->mmio_ranges[ri][0] &&
                            (uint64_t)addr <= c->mmio_ranges[ri][1]) {
                            uint64_t range_size = c->mmio_ranges[ri][1] - (uint64_t)addr + 1;
                            Type *inner = type_unwrap_distinct(type);
                            int elem_size = type_width(inner->pointer.inner) / 8;
                            if (elem_size > 0) {
                                sym->mmio_bound = range_size / (uint64_t)elem_size;
                            }
                            break;
                        }
                    }
                }
            }
        }

        /* BUG-479: address_taken VRP invalidation now handled in check_expr
         * TOK_AMP handler — covers ALL &var sites (var-decl, assign, call arg,
         * struct field, return). No per-site duplicate code needed. */

        /* Value range propagation: track literal init values */
        if (node->var_decl.init && type && type_is_integer(type)) {
            int64_t val = eval_const_expr(node->var_decl.init);
            if (val != CONST_EVAL_FAIL) {
                push_var_range(c, node->var_decl.name,
                    (uint32_t)node->var_decl.name_len, val, val, val != 0);
            } else {
                /* derive range from expression: x % N → [0, N-1], x & MASK → [0, MASK] */
                int64_t rmin, rmax;
                if (derive_expr_range(c, node->var_decl.init, &rmin, &rmax)) {
                    push_var_range(c, node->var_decl.name,
                        (uint32_t)node->var_decl.name_len, rmin, rmax, rmin > 0);
                } else if (node->var_decl.init->kind == NODE_CALL &&
                           node->var_decl.init->call.callee->kind == NODE_IDENT) {
                    /* cross-function range: check if callee has return range summary */
                    Symbol *csym = scope_lookup(c->current_scope,
                        node->var_decl.init->call.callee->ident.name,
                        (uint32_t)node->var_decl.init->call.callee->ident.name_len);
                    if (csym && csym->has_return_range) {
                        push_var_range(c, node->var_decl.name,
                            (uint32_t)node->var_decl.name_len,
                            csym->return_range_min, csym->return_range_max,
                            csym->return_range_min > 0);
                    }
                }
            }
        }

        break;
    }

    case NODE_IF: {
        /* comptime if — evaluate condition at compile time, only check taken branch */
        if (node->if_stmt.is_comptime) {
            /* type-check the condition first so comptime calls get resolved */
            check_expr(c, node->if_stmt.cond);
            int64_t cval = eval_const_expr(node->if_stmt.cond);
            if (cval == CONST_EVAL_FAIL) {
                /* try resolved comptime call: comptime if (FUNC()) */
                if (node->if_stmt.cond->kind == NODE_CALL &&
                    node->if_stmt.cond->call.is_comptime_resolved) {
                    cval = node->if_stmt.cond->call.comptime_value;
                }
            }
            if (cval == CONST_EVAL_FAIL) {
                /* try looking up const bool/int ident */
                if (node->if_stmt.cond->kind == NODE_IDENT) {
                    Symbol *cs = scope_lookup(c->current_scope,
                        node->if_stmt.cond->ident.name,
                        (uint32_t)node->if_stmt.cond->ident.name_len);
                    if (cs && cs->is_const) {
                        /* const variable — try to resolve init value.
                         * Covers: const bool X = true, const u32 P = COMPTIME_FUNC() */
                        if (cs->func_node && cs->func_node->var_decl.init) {
                            cval = eval_const_expr(cs->func_node->var_decl.init);
                            /* If init is a resolved comptime call, use its value */
                            if (cval == CONST_EVAL_FAIL &&
                                cs->func_node->var_decl.init->kind == NODE_CALL &&
                                cs->func_node->var_decl.init->call.is_comptime_resolved) {
                                cval = cs->func_node->var_decl.init->call.comptime_value;
                            }
                        }
                    }
                }
            }
            if (cval == CONST_EVAL_FAIL) {
                checker_error(c, node->loc.line,
                    "comptime if condition must be a compile-time constant");
            }
            /* store result for emitter — convert cond to int literal
             * so eval_const_expr in emitter picks it up */
            node->if_stmt.cond->kind = NODE_INT_LIT;
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
                    /* Red Team V13: move struct value capture creates duplicate owner.
                     * Force pointer capture for move types. */
                    Type *uw_eff = type_unwrap_distinct(unwrapped);
                    if (uw_eff && uw_eff->kind == TYPE_STRUCT && uw_eff->struct_type.is_move) {
                        checker_error(c, node->loc.line,
                            "move struct cannot be captured by value — use |*%.*s| for pointer capture",
                            (int)node->if_stmt.capture_name_len, node->if_stmt.capture_name);
                    }
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
                            if (csym) propagate_escape_flags(cap, csym, cap->type);
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
                                cap->is_from_arena = true;
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

        /* Value range propagation: extract constraints from condition */
        {
            int saved_range_count = c->var_range_count;
            Node *if_cond = node->if_stmt.cond;

            /* detect comparison pattern: ident OP const or const OP ident */
            const char *cmp_var = NULL;
            uint32_t cmp_var_len = 0;
            int64_t cmp_val = CONST_EVAL_FAIL;
            TokenType cmp_op = TOK_EOF;
            bool var_on_left = false;

            if (if_cond && if_cond->kind == NODE_BINARY) {
                cmp_op = if_cond->binary.op;
                Node *lhs = if_cond->binary.left;
                Node *rhs = if_cond->binary.right;

                /* try left side as ident or struct field
                 * TOCTOU: skip volatile variables — value can change between
                 * guard check and use, so range narrowing is unsound */
                if (lhs->kind == NODE_IDENT) {
                    Symbol *lsym = scope_lookup(c->current_scope,
                        lhs->ident.name, (uint32_t)lhs->ident.name_len);
                    bool is_vol = (lsym && lsym->is_volatile);
                    cmp_val = eval_const_expr(rhs);
                    if (cmp_val != CONST_EVAL_FAIL && !is_vol) {
                        cmp_var = lhs->ident.name;
                        cmp_var_len = (uint32_t)lhs->ident.name_len;
                        var_on_left = true;
                    }
                } else if (lhs->kind == NODE_FIELD) {
                    cmp_val = eval_const_expr(rhs);
                    if (cmp_val != CONST_EVAL_FAIL) {
                        ExprKey ek = build_expr_key_a(c, lhs);
                        if (ek.len > 0) {
                            cmp_var = ek.str;
                            cmp_var_len = (uint32_t)ek.len;
                            var_on_left = true;
                        }
                    }
                }
                /* try right side — also skip volatile (TOCTOU) */
                if (!cmp_var && rhs->kind == NODE_IDENT) {
                    Symbol *rsym = scope_lookup(c->current_scope,
                        rhs->ident.name, (uint32_t)rhs->ident.name_len);
                    bool r_vol = (rsym && rsym->is_volatile);
                    cmp_val = eval_const_expr(lhs);
                    if (cmp_val != CONST_EVAL_FAIL && !r_vol) {
                        cmp_var = rhs->ident.name;
                        cmp_var_len = (uint32_t)rhs->ident.name_len;
                        var_on_left = false;
                    }
                } else if (!cmp_var && rhs->kind == NODE_FIELD) {
                    cmp_val = eval_const_expr(lhs);
                    if (cmp_val != CONST_EVAL_FAIL) {
                        ExprKey ek = build_expr_key_a(c, rhs);
                        if (ek.len > 0) {
                            cmp_var = ek.str;
                            cmp_var_len = (uint32_t)ek.len;
                            var_on_left = false;
                        }
                    }
                }
            }

            /* detect comparison with == 0 / != 0 */
            if (if_cond && if_cond->kind == NODE_BINARY && cmp_var) {
                bool is_guard = body_always_exits(node->if_stmt.then_body) &&
                                !node->if_stmt.else_body;

                /* Inside then-block: apply condition directly */
                if (var_on_left) {
                    switch (cmp_op) {
                    case TOK_LT:      /* var < val → var.max = val - 1 */
                        push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, cmp_val - 1,
                                       cmp_val > 1);
                        break;
                    case TOK_LTEQ:    /* var <= val → var.max = val */
                        push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, cmp_val,
                                       cmp_val > 0 || cmp_val < 0);
                        break;
                    case TOK_GT:      /* var > val → var.min = val + 1 */
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val + 1, INT64_MAX, true);
                        break;
                    case TOK_GTEQ:    /* var >= val → var.min = val */
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val, INT64_MAX,
                                       cmp_val > 0 || cmp_val < 0);
                        break;
                    case TOK_EQEQ:    /* var == val → var.min = var.max = val */
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val, cmp_val,
                                       cmp_val != 0);
                        break;
                    case TOK_BANGEQ:  /* var != val → if val == 0, known_nonzero */
                        if (cmp_val == 0)
                            push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, INT64_MAX, true);
                        break;
                    default: break;
                    }
                } else {
                    /* val OP var → flip: val < var means var > val */
                    switch (cmp_op) {
                    case TOK_LT:      /* val < var → var > val → var.min = val + 1 */
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val + 1, INT64_MAX, true);
                        break;
                    case TOK_LTEQ:
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val, INT64_MAX,
                                       cmp_val > 0 || cmp_val < 0);
                        break;
                    case TOK_GT:
                        push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, cmp_val - 1,
                                       cmp_val > 1);
                        break;
                    case TOK_GTEQ:
                        push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, cmp_val,
                                       cmp_val > 0 || cmp_val < 0);
                        break;
                    case TOK_EQEQ:
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val, cmp_val,
                                       cmp_val != 0);
                        break;
                    case TOK_BANGEQ:
                        if (cmp_val == 0)
                            push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, INT64_MAX, true);
                        break;
                    default: break;
                    }
                }

                check_stmt(c, node->if_stmt.then_body);
                c->var_range_count = saved_range_count; /* restore */

                /* Guard pattern: if (cond) { return; } → apply INVERSE after the if */
                if (is_guard && var_on_left) {
                    switch (cmp_op) {
                    case TOK_GTEQ:    /* if (var >= val) return → var < val → var.max = val - 1 */
                        push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, cmp_val - 1,
                                       cmp_val > 1);
                        break;
                    case TOK_GT:      /* if (var > val) return → var <= val → var.max = val */
                        push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, cmp_val,
                                       cmp_val > 0 || cmp_val < 0);
                        break;
                    case TOK_LT:      /* if (var < val) return → var >= val → var.min = val */
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val, INT64_MAX,
                                       cmp_val > 0 || cmp_val < 0);
                        break;
                    case TOK_LTEQ:    /* if (var <= val) return → var > val → var.min = val + 1 */
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val + 1, INT64_MAX, true);
                        break;
                    case TOK_EQEQ:    /* if (var == 0) return → var != 0 → known_nonzero */
                        if (cmp_val == 0)
                            push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, INT64_MAX, true);
                        break;
                    default: break;
                    }
                } else if (is_guard && !var_on_left) {
                    /* val OP var guard → apply inverse (flip) */
                    switch (cmp_op) {
                    case TOK_GTEQ:    /* if (val >= var) return → var > val */
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val + 1, INT64_MAX, true);
                        break;
                    case TOK_GT:
                        push_var_range(c, cmp_var, cmp_var_len, cmp_val, INT64_MAX,
                                       cmp_val > 0 || cmp_val < 0);
                        break;
                    case TOK_LT:
                        push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, cmp_val - 1,
                                       cmp_val > 1);
                        break;
                    case TOK_LTEQ:
                        push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, cmp_val,
                                       cmp_val > 0 || cmp_val < 0);
                        break;
                    case TOK_EQEQ:
                        if (cmp_val == 0)
                            push_var_range(c, cmp_var, cmp_var_len, INT64_MIN, INT64_MAX, true);
                        break;
                    default: break;
                    }
                }

                if (node->if_stmt.else_body) {
                    /* else-block gets the inverse of then-block's range
                     * (but we already restored, so just check else normally) */
                    check_stmt(c, node->if_stmt.else_body);
                }
            } else {
                /* non-comparison condition — no range narrowing */
                check_stmt(c, node->if_stmt.then_body);
                if (node->if_stmt.else_body)
                    check_stmt(c, node->if_stmt.else_body);
            }
            /* guard ranges stay — they're valid after the if */
        }
        break;
    }

    case NODE_FOR: {
        push_scope(c); /* for loop has its own scope */
        if (node->for_stmt.init) check_stmt(c, node->for_stmt.init);
        if (node->for_stmt.cond) {
            Type *fcond = check_expr(c, node->for_stmt.cond);
            if (!type_equals(fcond, ty_bool)) {
                checker_error(c, node->loc.line,
                    "for condition must be bool, got '%s'", type_name(fcond));
            }
        }
        if (node->for_stmt.step) check_expr(c, node->for_stmt.step);

        /* Value range propagation: for (i = 0; i < N; ...) → i in [0, N-1] */
        int saved_range_count = c->var_range_count;
        if (node->for_stmt.cond && node->for_stmt.cond->kind == NODE_BINARY) {
            Node *fc = node->for_stmt.cond;
            TokenType fop = fc->binary.op;
            const char *loop_var = NULL;
            uint32_t loop_var_len = 0;
            int64_t bound_val = CONST_EVAL_FAIL;

            /* pattern: ident < const or ident < ident.len */
            if (fc->binary.left->kind == NODE_IDENT) {
                loop_var = fc->binary.left->ident.name;
                loop_var_len = (uint32_t)fc->binary.left->ident.name_len;
                bound_val = eval_const_expr(fc->binary.right);
            }

            /* try to get init value for min */
            int64_t init_val = 0; /* default min */
            if (node->for_stmt.init && node->for_stmt.init->kind == NODE_VAR_DECL &&
                node->for_stmt.init->var_decl.init) {
                int64_t iv = eval_const_expr(node->for_stmt.init->var_decl.init);
                if (iv != CONST_EVAL_FAIL) init_val = iv;
            }

            if (loop_var && bound_val != CONST_EVAL_FAIL) {
                if (fop == TOK_LT) {
                    push_var_range(c, loop_var, loop_var_len, init_val, bound_val - 1,
                                   init_val > 0);
                } else if (fop == TOK_LTEQ) {
                    push_var_range(c, loop_var, loop_var_len, init_val, bound_val,
                                   init_val > 0);
                }
            }
        }

        bool prev_in_loop = c->in_loop;
        c->in_loop = true;
        check_stmt(c, node->for_stmt.body);
        c->in_loop = prev_in_loop;
        c->var_range_count = saved_range_count; /* ranges invalid after loop */
        pop_scope(c);
        break;
    }

    case NODE_WHILE:
    case NODE_DO_WHILE: {
        Type *cond = check_expr(c, node->while_stmt.cond);
        if (!type_equals(cond, ty_bool)) {
            checker_error(c, node->loc.line,
                "%s condition must be bool, got '%s'",
                node->kind == NODE_DO_WHILE ? "do-while" : "while",
                type_name(cond));
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
                        /* BUG-480: move struct value capture in switch creates copy —
                         * two owners of unique resource. Force pointer capture.
                         * Same pattern as V13 if-unwrap (line ~6794). */
                        {
                            Type *vt_eff = type_unwrap_distinct(variant_type);
                            if (vt_eff && vt_eff->kind == TYPE_STRUCT && vt_eff->struct_type.is_move) {
                                checker_error(c, arm->loc.line,
                                    "move struct cannot be captured by value in switch — "
                                    "use |*%.*s| for pointer capture",
                                    (int)arm->capture_name_len, arm->capture_name);
                            }
                        }
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
                        /* BUG-480: same move struct check for optional switch */
                        {
                            Type *uw_eff = type_unwrap_distinct(unwrapped);
                            if (uw_eff && uw_eff->kind == TYPE_STRUCT && uw_eff->struct_type.is_move) {
                                checker_error(c, arm->loc.line,
                                    "move struct cannot be captured by value in switch — "
                                    "use |*%.*s| for pointer capture",
                                    (int)arm->capture_name_len, arm->capture_name);
                            }
                        }
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
                        if (src) propagate_escape_flags(cap, src, cap->type);
                    }
                }

                /* lock union variable during switch arm to prevent type confusion.
                 * Handles: switch (d) and switch (*ptr) where ptr points to union */
                const char *saved_union_var = c->union_switch_var;
                uint32_t saved_union_var_len = c->union_switch_var_len;
                const char *saved_union_key = c->union_switch_key;
                uint32_t saved_union_key_len = c->union_switch_key_len;
                Type *saved_union_type = c->union_switch_type;
                if (expr_eff->kind == TYPE_UNION) {
                    c->union_switch_type = expr;
                    Node *sw_expr = node->switch_stmt.expr;
                    /* BUG-392: build full expression key for precise locking.
                     * switch(msgs[0]) → key "msgs[0]", allows msgs[1] mutation.
                     * walk to root ident for backward-compat root lock too. */
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
                    /* build full key for precise comparison */
                    ExprKey sw_key = build_expr_key_a(c, sw_expr);
                    if (sw_key.len > 0) {
                        c->union_switch_key = sw_key.str;
                        c->union_switch_key_len = (uint32_t)sw_key.len;
                    } else {
                        c->union_switch_key = NULL;
                        c->union_switch_key_len = 0;
                    }
                }
                check_stmt(c, arm->body);
                c->union_switch_var = saved_union_var;
                c->union_switch_var_len = saved_union_var_len;
                c->union_switch_key = saved_union_key;
                c->union_switch_key_len = saved_union_key_len;
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
        /* return inside @critical skips interrupt re-enable */
        if (c->critical_depth > 0) {
            checker_error(c, node->loc.line,
                "cannot use 'return' inside @critical block — interrupts would not be re-enabled");
            break;
        }

        if (node->ret.expr) {
            Type *ret_type = check_expr(c, node->ret.expr);

            /* string literal returned as mutable slice → .rodata write risk
             * Covers both []u8 and ?[]u8 return types.
             * BUG-406: allow return from const []u8 functions (string literals are const). */
            if (node->ret.expr->kind == NODE_STRING_LIT && c->current_func_ret) {
                /* BUG-506: unwrap distinct on return type for mutable slice check */
                Type *ret = type_unwrap_distinct(c->current_func_ret);
                if ((ret->kind == TYPE_SLICE && !ret->slice.is_const) ||
                    (ret->kind == TYPE_OPTIONAL &&
                     type_unwrap_distinct(ret->optional.inner)->kind == TYPE_SLICE &&
                     !type_unwrap_distinct(ret->optional.inner)->slice.is_const)) {
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

            /* scope escape: return @ptrtoint(&local) — address of local escapes as integer.
             * Catches direct return without intermediate variable.
             * Indirect case (usize a = @ptrtoint(&x); return a) caught by is_local_derived. */
            if (node->ret.expr->kind == NODE_INTRINSIC &&
                node->ret.expr->intrinsic.name_len == 8 &&
                memcmp(node->ret.expr->intrinsic.name, "ptrtoint", 8) == 0 &&
                node->ret.expr->intrinsic.arg_count > 0) {
                Node *ptarg = node->ret.expr->intrinsic.args[0];
                if (ptarg && ptarg->kind == NODE_UNARY && ptarg->unary.op == TOK_AMP) {
                    Node *root = ptarg->unary.operand;
                    while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                        if (root->kind == NODE_FIELD) root = root->field.object;
                        else root = root->index_expr.object;
                    }
                    if (root && root->kind == NODE_IDENT) {
                        bool is_global = scope_lookup_local(c->global_scope,
                            root->ident.name, (uint32_t)root->ident.name_len) != NULL;
                        if (!is_global) {
                            checker_error(c, node->loc.line,
                                "cannot return @ptrtoint of local '%.*s' — "
                                "address will dangle after function returns",
                                (int)root->ident.name_len, root->ident.name);
                        }
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
                ret_type && (ret_type->kind == TYPE_POINTER || ret_type->kind == TYPE_SLICE)) {
                if (call_has_local_derived_arg(c, node->ret.expr, 0)) {
                    checker_error(c, node->loc.line,
                        "cannot return result of call with local-derived pointer argument — "
                        "stack memory may escape through function return");
                }
            }

            /* BUG-383: return wrap(&x).p — struct wrapper bypasses BUG-360 because
             * the function returns a struct, not a pointer/slice. Walk through field/index
             * chains to find if root is a NODE_CALL with local-derived args.
             * Fires when the final return type is a pointer OR slice. */
            if (ret_type && (ret_type->kind == TYPE_POINTER || ret_type->kind == TYPE_SLICE)) {
                Node *rroot = node->ret.expr;
                while (rroot && (rroot->kind == NODE_FIELD || rroot->kind == NODE_INDEX)) {
                    if (rroot->kind == NODE_FIELD) rroot = rroot->field.object;
                    else rroot = rroot->index_expr.object;
                }
                if (rroot && rroot->kind == NODE_CALL) {
                    if (call_has_local_derived_arg(c, rroot, 0)) {
                        checker_error(c, node->loc.line,
                            "cannot return pointer extracted from call with local-derived "
                            "argument — stack memory may escape through struct field");
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

            /* Designated init in return: validate against function return type */
            if (node->ret.expr->kind == NODE_STRUCT_INIT && c->current_func_ret) {
                if (validate_struct_init(c, node->ret.expr, c->current_func_ret, node->loc.line)) {
                    ret_type = c->current_func_ret;
                    typemap_set(c, node->ret.expr, c->current_func_ret);
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
        } else if (c->critical_depth > 0) {
            checker_error(c, node->loc.line,
                "cannot use 'break' inside @critical block — interrupts would not be re-enabled");
        } else if (!c->in_loop) {
            checker_error(c, node->loc.line, "'break' outside of loop");
        }
        break;

    case NODE_GOTO:
        /* goto target label validation done in check_function_body (post-pass) */
        if (c->defer_depth > 0) {
            checker_error(c, node->loc.line, "cannot use 'goto' inside defer block");
        } else if (c->critical_depth > 0) {
            checker_error(c, node->loc.line,
                "cannot use 'goto' inside @critical block — interrupts would not be re-enabled");
        }
        break;

    case NODE_LABEL:
        /* labels are just markers — no type checking needed */
        break;

    case NODE_CONTINUE:
        if (c->defer_depth > 0) {
            checker_error(c, node->loc.line, "cannot use 'continue' inside defer block");
        } else if (c->critical_depth > 0) {
            checker_error(c, node->loc.line,
                "cannot use 'continue' inside @critical block — interrupts would not be re-enabled");
        } else if (!c->in_loop) {
            checker_error(c, node->loc.line, "'continue' outside of loop");
        }
        break;

    case NODE_DEFER:
        /* Ban yield/await in defer body — corrupts Duff's device state machine.
         * Function summaries catch both direct and transitive cases. */
        check_body_effects(c, node->defer.body, node->loc.line,
            true, "cannot yield inside defer block — corrupts coroutine state machine",
            false, NULL,
            false, NULL);
        c->defer_depth++;
        check_stmt(c, node->defer.body);
        c->defer_depth--;
        break;

    case NODE_STATIC_ASSERT: {
        /* Type-check the condition first — resolves comptime calls */
        check_expr(c, node->static_assert_stmt.cond);
        int64_t val = eval_const_expr_scoped(c, node->static_assert_stmt.cond);
        if (val == CONST_EVAL_FAIL) {
            checker_error(c, node->loc.line,
                "static_assert condition must be a compile-time constant");
        } else if (!val) {
            if (node->static_assert_stmt.message) {
                checker_error(c, node->loc.line,
                    "static_assert failed: %.*s",
                    (int)node->static_assert_stmt.message_len,
                    node->static_assert_stmt.message);
            } else {
                checker_error(c, node->loc.line, "static_assert failed");
            }
        }
        break;
    }

    case NODE_EXPR_STMT:
        check_expr(c, node->expr_stmt.expr);
        /* Ghost handle: warn if pool.alloc()/slab.alloc() result is discarded.
         * The handle is leaked — must assign to a variable. */
        if (node->expr_stmt.expr && node->expr_stmt.expr->kind == NODE_CALL &&
            node->expr_stmt.expr->call.callee &&
            node->expr_stmt.expr->call.callee->kind == NODE_FIELD) {
            Node *call_obj = node->expr_stmt.expr->call.callee->field.object;
            const char *mname = node->expr_stmt.expr->call.callee->field.field_name;
            uint32_t mlen = (uint32_t)node->expr_stmt.expr->call.callee->field.field_name_len;
            if (((mlen == 5 && memcmp(mname, "alloc", 5) == 0) ||
                 (mlen == 9 && memcmp(mname, "alloc_ptr", 9) == 0)) && call_obj->kind == NODE_IDENT) {
                Type *ot = checker_get_type(c, call_obj);
                if (!ot) {
                    Symbol *s = scope_lookup(c->current_scope, call_obj->ident.name,
                        (uint32_t)call_obj->ident.name_len);
                    if (s) ot = s->type;
                }
                if (ot && (ot->kind == TYPE_POOL || ot->kind == TYPE_SLAB)) {
                    checker_error(c, node->loc.line,
                        "discarded alloc result — handle leaked. Assign to a variable: "
                        "'Handle(...) h = %.*s.alloc() orelse return;'",
                        (int)call_obj->ident.name_len, call_obj->ident.name);
                }
            }
        }
        break;

    case NODE_ASM:
        /* MISRA Dir 4.3: asm must be encapsulated in naked functions */
        if (!c->in_naked) {
            checker_error(c, node->loc.line,
                "asm statements only allowed in naked functions — "
                "use @critical or @atomic_* for safe alternatives (MISRA Dir 4.3)");
        }
        break;

    case NODE_CRITICAL:
        /* @critical { body } — check body, ban return/break/continue/goto
         * (jumping out skips interrupt re-enable — leaves system broken).
         * Also ban yield/spawn via function summaries (direct + transitive). */
        check_body_effects(c, node->critical.body, node->loc.line,
            true, "cannot yield inside @critical block — interrupts stay disabled across suspend",
            true, "cannot spawn inside @critical block — thread creation with interrupts disabled",
            false, NULL);
        c->critical_depth++;
        if (node->critical.body) {
            check_stmt(c, node->critical.body);
        }
        c->critical_depth--;
        break;

    case NODE_ONCE:
        /* @once { body } — check body */
        if (node->once.body) {
            check_stmt(c, node->once.body);
        }
        break;

    case NODE_YIELD:
        /* yield — suspend current async task.
         * BUG-481: yield in orelse at var-decl level = safe (state struct temps).
         * BUG-495: yield in orelse at expression level = GCC error
         * ("switch jumps into statement expression"). This is a GCC limitation —
         * case labels can't be inside ({...}). The developer must extract to var-decl.
         * No checker ban needed — GCC catches it with a clear error message. */
        break;

    case NODE_AWAIT: {
        /* await expr — check the condition expression */
        if (node->await_stmt.cond) {
            check_expr(c, node->await_stmt.cond);
        }
        break;
    }

    case NODE_SPAWN: {
        /* spawn func(args); — validate function, check arg safety.
         * Also: scan spawned function body for non-shared global access (data race). */
        Symbol *func_sym = scope_lookup(c->global_scope,
            node->spawn_stmt.func_name, (uint32_t)node->spawn_stmt.func_name_len);
        if (!func_sym || !func_sym->is_function) {
            checker_error(c, node->loc.line,
                "spawn target '%.*s' is not a function",
                (int)node->spawn_stmt.func_name_len, node->spawn_stmt.func_name);
            break;
        }
        /* Red Team V23: spawn target return type check.
         * Move struct or Handle return = resource leak (error).
         * Other non-void return = lost value (warning). */
        if (func_sym->func_node && func_sym->func_node->kind == NODE_FUNC_DECL) {
            Type *ret = resolve_type(c, func_sym->func_node->func_decl.return_type);
            if (ret && ret->kind != TYPE_VOID) {
                Type *ret_eff = type_unwrap_distinct(ret);
                bool is_resource = (ret_eff->kind == TYPE_HANDLE) ||
                    (ret_eff->kind == TYPE_STRUCT && ret_eff->struct_type.is_move) ||
                    (ret_eff->kind == TYPE_OPTIONAL && type_unwrap_distinct(ret_eff->optional.inner)->kind == TYPE_HANDLE);
                if (is_resource) {
                    checker_error(c, node->loc.line,
                        "spawn target '%.*s' returns '%s' — resource would leak. "
                        "Use void return type for spawn targets",
                        (int)node->spawn_stmt.func_name_len, node->spawn_stmt.func_name,
                        type_name(ret));
                } else {
                    checker_warning(c, node->loc.line,
                        "spawn target '%.*s' returns '%s' — return value lost",
                        (int)node->spawn_stmt.func_name_len, node->spawn_stmt.func_name,
                        type_name(ret));
                }
            }
        }
        bool is_scoped = (node->spawn_stmt.handle_name != NULL);
        /* Check each argument — type-check + pointer safety */
        for (int i = 0; i < node->spawn_stmt.arg_count; i++) {
            Type *arg_type = check_expr(c, node->spawn_stmt.args[i]);
            if (!arg_type) continue;
            /* A7: string literal to mutable slice — same check as regular call (line 3871).
             * String data is in .rodata, spawned thread writing it = segfault. */
            if (node->spawn_stmt.args[i]->kind == NODE_STRING_LIT &&
                func_sym->func_node && func_sym->func_node->kind == NODE_FUNC_DECL &&
                i < func_sym->func_node->func_decl.param_count) {
                Type *param_type = resolve_type(c, func_sym->func_node->func_decl.params[i].type);
                if (param_type && param_type->kind == TYPE_SLICE && !param_type->slice.is_const) {
                    checker_error(c, node->loc.line,
                        "spawn argument %d: cannot pass string literal to mutable []u8 parameter — "
                        "string data is read-only, use const []u8", i + 1);
                }
            }
            Type *eff = type_unwrap_distinct(arg_type);
            /* Pointer args: scoped spawn allows *T (will be joined), fire-and-forget requires *shared */
            if (eff->kind == TYPE_POINTER) {
                Type *inner = type_unwrap_distinct(eff->pointer.inner);
                if (inner && inner->kind == TYPE_STRUCT && inner->struct_type.is_shared) {
                    /* OK — shared struct pointer, auto-locked */
                } else if (is_scoped) {
                    /* OK — scoped spawn, thread will be joined before scope exit */
                } else {
                    checker_error(c, node->loc.line,
                        "argument %d: cannot pass non-shared pointer to spawn — "
                        "data race. Use shared struct, copy by value, or use "
                        "ThreadHandle to join before scope exit",
                        i + 1);
                }
            }
            /* Handle args: ban — pool.get() not thread-safe */
            if (eff->kind == TYPE_HANDLE) {
                checker_error(c, node->loc.line,
                    "argument %d: cannot pass Handle to spawn — "
                    "pool.get() is not thread-safe",
                    i + 1);
            }
        }
        /* BUG-491: type-check spawn args against function parameter types.
         * The pointer safety check above validates shared vs non-shared,
         * but doesn't check const/volatile qualifier preservation.
         * spawn worker(&const_val) must fail like a normal call would. */
        if (func_sym->func_node && func_sym->func_node->kind == NODE_FUNC_DECL) {
            int pc = func_sym->func_node->func_decl.param_count;
            for (int i = 0; i < node->spawn_stmt.arg_count && i < pc; i++) {
                Type *arg_type = checker_get_type(c, node->spawn_stmt.args[i]);
                Type *param_type = resolve_type(c, func_sym->func_node->func_decl.params[i].type);
                if (!arg_type || !param_type) continue;
                /* const pointer → mutable param: reject */
                if (arg_type->kind == TYPE_POINTER && param_type->kind == TYPE_POINTER &&
                    arg_type->pointer.is_const && !param_type->pointer.is_const) {
                    checker_error(c, node->loc.line,
                        "spawn argument %d: cannot pass const pointer to mutable parameter",
                        i + 1);
                }
                /* volatile pointer → non-volatile param: reject */
                if (arg_type->kind == TYPE_POINTER && param_type->kind == TYPE_POINTER &&
                    !param_type->pointer.is_volatile) {
                    bool arg_vol = arg_type->pointer.is_volatile;
                    if (!arg_vol && node->spawn_stmt.args[i]->kind == NODE_UNARY &&
                        node->spawn_stmt.args[i]->unary.op == TOK_AMP &&
                        node->spawn_stmt.args[i]->unary.operand->kind == NODE_IDENT) {
                        Symbol *as = scope_lookup(c->current_scope,
                            node->spawn_stmt.args[i]->unary.operand->ident.name,
                            (uint32_t)node->spawn_stmt.args[i]->unary.operand->ident.name_len);
                        if (as && as->is_volatile) arg_vol = true;
                    }
                    if (arg_vol) {
                        checker_error(c, node->loc.line,
                            "spawn argument %d: cannot pass volatile pointer to non-volatile parameter",
                            i + 1);
                    }
                }
                /* Type mismatch */
                /* A15: add is_literal_compatible + validate_struct_init (match regular call) */
                if (node->spawn_stmt.args[i]->kind == NODE_STRUCT_INIT && param_type) {
                    if (validate_struct_init(c, node->spawn_stmt.args[i], param_type, node->loc.line)) {
                        arg_type = param_type;
                        typemap_set(c, node->spawn_stmt.args[i], param_type);
                    }
                }
                if (!type_equals(param_type, arg_type) &&
                    !can_implicit_coerce(arg_type, param_type) &&
                    !is_literal_compatible(node->spawn_stmt.args[i], param_type)) {
                    checker_error(c, node->loc.line,
                        "spawn argument %d: expected '%s', got '%s'",
                        i + 1, type_name(param_type), type_name(arg_type));
                }
            }
        }

        /* Register ThreadHandle variable in scope */
        if (is_scoped) {
            /* ThreadHandle is u64 wrapping pthread_t */
            Symbol *sym = add_symbol(c, node->spawn_stmt.handle_name,
                (uint32_t)node->spawn_stmt.handle_name_len,
                ty_u64, node->loc.line);
            if (sym) {
                sym->is_const = false;
                sym->is_thread_handle = true;
                typemap_set(c, node, ty_u64);
            }
        }
        /* Scan spawned function body for non-shared global access (data race).
         * Like Rust's Send/Sync check — spawned code can't touch non-shared globals. */
        if (func_sym && func_sym->func_node &&
            func_sym->func_node->kind == NODE_FUNC_DECL &&
            func_sym->func_node->func_decl.body) {
            const char *bad_name = NULL;
            uint32_t bad_len = 0;
            if (scan_unsafe_global_access(c, func_sym->func_node->func_decl.body,
                                           &bad_name, &bad_len)) {
                /* If function uses @atomic_* or @barrier — developer is doing manual
                 * synchronization (lock-free pattern). Warn, don't error.
                 * If NO synchronization at all — definitely unsafe, error. */
                ensure_func_props(c, func_sym);
                bool has_sync = func_sym->props.has_sync;
                if (has_sync) {
                    checker_warning(c, node->loc.line,
                        "spawn target '%.*s' accesses non-shared global '%.*s' — "
                        "potential data race (atomic/barrier present, verify ordering)",
                        (int)node->spawn_stmt.func_name_len, node->spawn_stmt.func_name,
                        (int)bad_len, bad_name);
                } else {
                    checker_error(c, node->loc.line,
                        "spawn target '%.*s' accesses non-shared global '%.*s' — "
                        "data race. Use shared struct, threadlocal, @atomic_*, or volatile",
                        (int)node->spawn_stmt.func_name_len, node->spawn_stmt.func_name,
                        (int)bad_len, bad_name);
                }
            }
        }
        /* Ban spawn inside @critical (now also covered by check_body_effects on @critical,
         * but keep direct check for clear error message at the spawn site) */
        if (c->critical_depth > 0) {
            checker_error(c, node->loc.line,
                "cannot use 'spawn' inside @critical block — "
                "thread creation with interrupts disabled is unsafe");
        }
        /* Ban spawn inside async function — spawned thread outlives coroutine */
        if (c->in_async) {
            checker_error(c, node->loc.line,
                "cannot use 'spawn' inside async function — "
                "spawned thread may outlive coroutine lifetime");
        }
        break;
    }

    default:
        break;
    }
    c->in_async_yield_stmt = saved_yield_stmt;
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
        t->struct_type.is_shared = node->struct_decl.is_shared;
        t->struct_type.is_shared_rw = node->struct_decl.is_shared_rw;
        t->struct_type.is_move = node->struct_decl.is_move;
        t->struct_type.field_count = (uint32_t)node->struct_decl.field_count;
        t->struct_type.type_id = c->next_type_id++;
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
                /* BUG-414: detect volatile qualifier on field TypeNode */
                sf->is_volatile = (fd->type && fd->type->kind == TYNODE_VOLATILE);
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
                /* BUG-498: synchronization primitives in packed struct → misaligned.
                 * pthread_mutex_t requires natural alignment. Packed structs can place
                 * fields at unaligned offsets → hard fault on ARM/RISC-V. */
                if (node->struct_decl.is_packed && sf->type) {
                    Type *ft = type_unwrap_distinct(sf->type);
                    if (ft->kind == TYPE_SEMAPHORE || ft->kind == TYPE_BARRIER ||
                        (ft->kind == TYPE_STRUCT && ft->struct_type.is_shared)) {
                        checker_error(c, node->loc.line,
                            "synchronization primitive '%.*s' cannot be inside packed struct — "
                            "pthread_mutex_t requires natural alignment",
                            (int)sf->name_len, sf->name);
                    }
                }
                /* Red Team V10: move struct inside shared struct → ownership breach.
                 * A thread can "move" the field out, leaving shared struct in zombie state. */
                if (node->struct_decl.is_shared && sf->type) {
                    Type *ft = type_unwrap_distinct(sf->type);
                    if (ft->kind == TYPE_STRUCT && ft->struct_type.is_move) {
                        checker_error(c, node->loc.line,
                            "move struct '%.*s' cannot be a field of shared struct '%.*s' — "
                            "ownership transfer from shared memory creates data race",
                            (int)sf->name_len, sf->name,
                            (int)node->struct_decl.name_len, node->struct_decl.name);
                    }
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
        t->enum_type.type_id = c->next_type_id++;
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
        t->union_type.type_id = c->next_type_id++;
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
                /* BUG-386: Pool/Ring/Slab in union not supported (same as BUG-287 for structs) */
                if (sv->type && (sv->type->kind == TYPE_POOL || sv->type->kind == TYPE_RING || sv->type->kind == TYPE_SLAB)) {
                    checker_error(c, node->loc.line,
                        "Pool/Ring/Slab cannot be union variants — must be global or static variables");
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

        /* Async function: register state struct type + init/poll functions */
        if (node->func_decl.is_async && sym) {
            /* Build mangled name: _zer_async_funcname */
            char aname[256];
            int alen = snprintf(aname, sizeof(aname), "_zer_async_%.*s",
                (int)node->func_decl.name_len, node->func_decl.name);
            if (alen >= (int)sizeof(aname)) alen = (int)sizeof(aname) - 1;
            /* Register state struct as an opaque type (fields not accessible from ZER) */
            Type *async_type = (Type *)arena_alloc(c->arena, sizeof(Type));
            async_type->kind = TYPE_STRUCT;
            async_type->struct_type.name = arena_alloc(c->arena, alen + 1);
            memcpy((char *)async_type->struct_type.name, aname, alen + 1);
            async_type->struct_type.name_len = alen;
            async_type->struct_type.field_count = 0;
            async_type->struct_type.fields = NULL;
            async_type->struct_type.type_id = c->next_type_id++;
            char *aname_copy = arena_alloc(c->arena, alen + 1);
            memcpy(aname_copy, aname, alen + 1);
            add_symbol_internal(c, aname_copy, alen, async_type, node->loc.line);

            /* Register _zer_async_funcname_init as function taking *async_type + original params (BUG-477) */
            char iname[256];
            int ilen = snprintf(iname, sizeof(iname), "_zer_async_%.*s_init",
                (int)node->func_decl.name_len, node->func_decl.name);
            if (ilen >= (int)sizeof(iname)) ilen = (int)sizeof(iname) - 1;
            int init_pc = 1 + node->func_decl.param_count; /* *self + original params */
            Type **ip = (Type **)arena_alloc(c->arena, init_pc * sizeof(Type *));
            ip[0] = type_pointer(c->arena, async_type);
            for (int pi = 0; pi < node->func_decl.param_count; pi++) {
                ip[1 + pi] = resolve_type(c, node->func_decl.params[pi].type);
            }
            Type *init_ft = type_func_ptr(c->arena, ip, init_pc, ty_void);
            char *iname_copy = arena_alloc(c->arena, ilen + 1);
            memcpy(iname_copy, iname, ilen + 1);
            Symbol *isym = add_symbol_internal(c, iname_copy, ilen, init_ft, node->loc.line);
            if (isym) isym->is_function = true;

            /* Register _zer_async_funcname_poll as function taking *async_type, returning i32 */
            char pname[256];
            int plen = snprintf(pname, sizeof(pname), "_zer_async_%.*s_poll",
                (int)node->func_decl.name_len, node->func_decl.name);
            if (plen >= (int)sizeof(pname)) plen = (int)sizeof(pname) - 1;
            Type **pp = (Type **)arena_alloc(c->arena, sizeof(Type *));
            pp[0] = type_pointer(c->arena, async_type);
            Type *poll_ft = type_func_ptr(c->arena, pp, 1, ty_i32);
            char *pname_copy = arena_alloc(c->arena, plen + 1);
            memcpy(pname_copy, pname, plen + 1);
            Symbol *psym = add_symbol_internal(c, pname_copy, plen, poll_ft, node->loc.line);
            if (psym) psym->is_function = true;
        }
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
            sym->func_node = node; /* store AST node for const init lookup */
            /* MMIO pointer bound for globals */
            if (node->var_decl.init && type && type_unwrap_distinct(type)->kind == TYPE_POINTER) {
                Node *gi = node->var_decl.init;
                if (gi->kind == NODE_INTRINSIC && gi->intrinsic.name_len == 8 &&
                    memcmp(gi->intrinsic.name, "inttoptr", 8) == 0 &&
                    gi->intrinsic.arg_count > 0) {
                    int64_t addr = eval_const_expr(gi->intrinsic.args[0]);
                    if (addr != CONST_EVAL_FAIL) {
                        for (int ri = 0; ri < c->mmio_range_count; ri++) {
                            if ((uint64_t)addr >= c->mmio_ranges[ri][0] &&
                                (uint64_t)addr <= c->mmio_ranges[ri][1]) {
                                uint64_t rsz = c->mmio_ranges[ri][1] - (uint64_t)addr + 1;
                                Type *inn = type_unwrap_distinct(type);
                                int esz = type_width(inn->pointer.inner) / 8;
                                if (esz > 0) sym->mmio_bound = rsz / (uint64_t)esz;
                                break;
                            }
                        }
                    }
                }
            }
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

    case NODE_CONTAINER_DECL: {
        /* Store container template — stamped on use via TYNODE_CONTAINER */
        if (c->container_tmpl_count >= c->container_tmpl_capacity) {
            int nc = c->container_tmpl_capacity < 8 ? 8 : c->container_tmpl_capacity * 2;
            struct ContainerTemplate *na = (struct ContainerTemplate *)arena_alloc(c->arena,
                nc * sizeof(struct ContainerTemplate));
            if (c->container_templates && c->container_tmpl_count > 0)
                memcpy(na, c->container_templates, c->container_tmpl_count * sizeof(struct ContainerTemplate));
            c->container_templates = na;
            c->container_tmpl_capacity = nc;
        }
        struct ContainerTemplate *ct = &c->container_templates[c->container_tmpl_count++];
        ct->name = node->container_decl.name;
        ct->name_len = (uint32_t)node->container_decl.name_len;
        ct->type_param = node->container_decl.type_param;
        ct->type_param_len = (uint32_t)node->container_decl.type_param_len;
        ct->fields = node->container_decl.fields;
        ct->field_count = node->container_decl.field_count;
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 * GOTO / LABEL VALIDATION
 * ================================================================ */

/* Collect all labels in a function body, then validate all gotos have targets */
typedef struct { const char *name; size_t len; int line; } LabelInfo;

static void collect_labels(Node *node, LabelInfo *labels, int *count, int max) {
    if (!node) return;
    switch (node->kind) {
    case NODE_LABEL:
        if (*count < max) {
            labels[*count].name = node->label_stmt.name;
            labels[*count].len = node->label_stmt.name_len;
            labels[*count].line = node->loc.line;
            (*count)++;
        }
        break;
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            collect_labels(node->block.stmts[i], labels, count, max);
        break;
    case NODE_IF:
        collect_labels(node->if_stmt.then_body, labels, count, max);
        collect_labels(node->if_stmt.else_body, labels, count, max);
        break;
    case NODE_FOR:
        collect_labels(node->for_stmt.body, labels, count, max);
        break;
    case NODE_WHILE: case NODE_DO_WHILE:
        collect_labels(node->while_stmt.body, labels, count, max);
        break;
    case NODE_SWITCH:
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            collect_labels(node->switch_stmt.arms[i].body, labels, count, max);
        break;
    case NODE_DEFER:
        collect_labels(node->defer.body, labels, count, max);
        break;
    case NODE_CRITICAL:
        collect_labels(node->critical.body, labels, count, max);
        break;
    case NODE_ONCE:
        collect_labels(node->once.body, labels, count, max);
        break;
    /* Nodes that cannot contain labels */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL: case NODE_ENUM_DECL:
    case NODE_UNION_DECL: case NODE_TYPEDEF: case NODE_IMPORT: case NODE_CINCLUDE:
    case NODE_INTERRUPT: case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
    case NODE_VAR_DECL: case NODE_RETURN: case NODE_BREAK: case NODE_CONTINUE:
    case NODE_GOTO: case NODE_EXPR_STMT: case NODE_ASM: case NODE_SPAWN:
    case NODE_YIELD: case NODE_AWAIT: case NODE_STATIC_ASSERT:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT: case NODE_CHAR_LIT:
    case NODE_BOOL_LIT: case NODE_NULL_LIT: case NODE_IDENT:
    case NODE_BINARY: case NODE_UNARY: case NODE_ASSIGN: case NODE_CALL:
    case NODE_FIELD: case NODE_INDEX: case NODE_SLICE: case NODE_ORELSE:
    case NODE_INTRINSIC: case NODE_CAST: case NODE_TYPECAST: case NODE_SIZEOF: case NODE_STRUCT_INIT:
        break;
    }
}

static void validate_gotos(Checker *c, Node *node, LabelInfo *labels, int label_count) {
    if (!node) return;
    switch (node->kind) {
    case NODE_GOTO: {
        bool found = false;
        for (int i = 0; i < label_count; i++) {
            if (labels[i].len == node->goto_stmt.label_len &&
                memcmp(labels[i].name, node->goto_stmt.label, labels[i].len) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            checker_error(c, node->loc.line,
                "goto target '%.*s' not found in this function",
                (int)node->goto_stmt.label_len, node->goto_stmt.label);
        }
        break;
    }
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            validate_gotos(c, node->block.stmts[i], labels, label_count);
        break;
    case NODE_IF:
        validate_gotos(c, node->if_stmt.then_body, labels, label_count);
        validate_gotos(c, node->if_stmt.else_body, labels, label_count);
        break;
    case NODE_FOR:
        validate_gotos(c, node->for_stmt.body, labels, label_count);
        break;
    case NODE_WHILE: case NODE_DO_WHILE:
        validate_gotos(c, node->while_stmt.body, labels, label_count);
        break;
    case NODE_SWITCH:
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            validate_gotos(c, node->switch_stmt.arms[i].body, labels, label_count);
        break;
    case NODE_DEFER:
        validate_gotos(c, node->defer.body, labels, label_count);
        break;
    case NODE_CRITICAL:
        validate_gotos(c, node->critical.body, labels, label_count);
        break;
    case NODE_ONCE:
        validate_gotos(c, node->once.body, labels, label_count);
        break;
    /* Nodes that cannot contain goto */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL: case NODE_ENUM_DECL:
    case NODE_UNION_DECL: case NODE_TYPEDEF: case NODE_IMPORT: case NODE_CINCLUDE:
    case NODE_INTERRUPT: case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
    case NODE_VAR_DECL: case NODE_RETURN: case NODE_BREAK: case NODE_CONTINUE:
    case NODE_LABEL: case NODE_EXPR_STMT: case NODE_ASM: case NODE_SPAWN:
    case NODE_YIELD: case NODE_AWAIT: case NODE_STATIC_ASSERT:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT: case NODE_CHAR_LIT:
    case NODE_BOOL_LIT: case NODE_NULL_LIT: case NODE_IDENT:
    case NODE_BINARY: case NODE_UNARY: case NODE_ASSIGN: case NODE_CALL:
    case NODE_FIELD: case NODE_INDEX: case NODE_SLICE: case NODE_ORELSE:
    case NODE_INTRINSIC: case NODE_CAST: case NODE_TYPECAST: case NODE_SIZEOF: case NODE_STRUCT_INIT:
        break;
    }
}

static void check_goto_labels(Checker *c, Node *func_body) {
    /* A16: stack-first dynamic pattern — no fixed limit on labels per function */
    LabelInfo stack_labels[128];
    LabelInfo *labels = stack_labels;
    int label_count = 0;
    int label_cap = 128;
    collect_labels(func_body, labels, &label_count, label_cap);
    /* If we hit the limit, grow and re-collect */
    if (label_count >= label_cap) {
        label_cap = label_count * 2;
        labels = (LabelInfo *)arena_alloc(c->arena, label_cap * sizeof(LabelInfo));
        label_count = 0;
        collect_labels(func_body, labels, &label_count, label_cap);
    }

    /* Check for duplicate labels */
    for (int i = 0; i < label_count; i++) {
        for (int j = i + 1; j < label_count; j++) {
            if (labels[i].len == labels[j].len &&
                memcmp(labels[i].name, labels[j].name, labels[i].len) == 0) {
                checker_error(c, labels[j].line,
                    "duplicate label '%.*s' (first defined at line %d)",
                    (int)labels[j].len, labels[j].name, labels[i].line);
            }
        }
    }

    /* Validate all gotos have matching labels */
    validate_gotos(c, func_body, labels, label_count);
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
    case NODE_WHILE: case NODE_DO_WHILE: case NODE_FOR: return false; /* nested loop — break targets it */
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
    case NODE_CRITICAL:
        return contains_break(node->critical.body);
    case NODE_ONCE:
        return contains_break(node->once.body);
    case NODE_DEFER:
        return false; /* break banned in defer — checker rejects it */
    case NODE_YIELD:
        return false; /* yield is a leaf, cannot contain break */
    case NODE_AWAIT:
        return node->await_stmt.cond ? contains_break(node->await_stmt.cond) : false;
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
    case NODE_DO_WHILE:
        /* while(true) / do...while(true) is an infinite loop — never exits normally.
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
    case NODE_CRITICAL:
        return all_paths_return(node->critical.body);
    case NODE_ONCE:
        return all_paths_return(node->once.body);
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

        if (node->func_decl.is_naked) {
            c->in_naked = true;
            /* MISRA Dir 4.3: naked functions must only contain asm statements.
             * Non-asm code uses stack that was never allocated (no prologue). */
            if (node->func_decl.body && node->func_decl.body->kind == NODE_BLOCK) {
                for (int si = 0; si < node->func_decl.body->block.stmt_count; si++) {
                    Node *s = node->func_decl.body->block.stmts[si];
                    if (s->kind != NODE_ASM && s->kind != NODE_RETURN) {
                        checker_error(c, s->loc.line,
                            "naked function must only contain asm and return — "
                            "non-asm code uses stack that was never allocated");
                        break;
                    }
                }
            }
        }
        bool saved_comptime = c->in_comptime_body;
        bool saved_async = c->in_async;
        if (node->func_decl.is_comptime) c->in_comptime_body = true;
        if (node->func_decl.is_async) c->in_async = true;
        check_stmt(c, node->func_decl.body);
        c->in_comptime_body = saved_comptime;
        c->in_async = saved_async;
        c->in_naked = false;

        /* check that all paths return for non-void functions */
        if (ret && ret->kind != TYPE_VOID &&
            !all_paths_return(node->func_decl.body)) {
            checker_error(c, node->loc.line,
                "not all control flow paths return a value in function '%.*s'",
                (int)node->func_decl.name_len, node->func_decl.name);
        }

        /* Cross-function provenance: if function returns *opaque, record
         * what provenance the return expression carries. Used at call sites. */
        {
            Type *ret_eff = type_unwrap_distinct(ret);
            if (ret_eff && ret_eff->kind == TYPE_POINTER &&
                ret_eff->pointer.inner->kind == TYPE_OPAQUE) {
                Type *rprov = find_return_provenance(c, node->func_decl.body);
                if (rprov) {
                    add_prov_summary(c, node->func_decl.name,
                        (uint32_t)node->func_decl.name_len, rprov);
                }
            }
        }

        /* Cross-function range summary: if function returns integer and
         * all return paths have derivable ranges (% N, & MASK), store on symbol. */
        {
            Type *ret_eff = type_unwrap_distinct(ret);
            if (ret_eff && type_is_integer(ret_eff) && node->func_decl.body) {
                int64_t rmin = 0, rmax = 0;
                bool found = false;
                if (find_return_range(c, node->func_decl.body, &rmin, &rmax, &found) && found) {
                    Symbol *fsym = scope_lookup(c->current_scope,
                        node->func_decl.name, (uint32_t)node->func_decl.name_len);
                    if (fsym) {
                        fsym->return_range_min = rmin;
                        fsym->return_range_max = rmax;
                        fsym->has_return_range = true;
                    }
                }
            }
        }

        /* Whole-program param provenance: for each *opaque parameter,
         * scan body for @ptrcast to determine expected type. */
        for (int pi = 0; pi < node->func_decl.param_count; pi++) {
            Type *pt = resolve_type(c, node->func_decl.params[pi].type);
            Type *ptu = type_unwrap_distinct(pt);
            if (ptu && ptu->kind == TYPE_POINTER && ptu->pointer.inner->kind == TYPE_OPAQUE) {
                Type *expected = find_param_cast_type(c, node->func_decl.body,
                    node->func_decl.params[pi].name,
                    (uint32_t)node->func_decl.params[pi].name_len);
                if (expected) {
                    /* store: function X expects param pi to be *T */
                    if (c->param_expect_count >= c->param_expect_capacity) {
                        int nc = c->param_expect_capacity * 2;
                        if (nc < 8) nc = 8;
                        c->param_expects = realloc(c->param_expects, nc * sizeof(struct ParamExpect));
                        c->param_expect_capacity = nc;
                    }
                    struct ParamExpect *pe = &c->param_expects[c->param_expect_count++];
                    pe->func_name = node->func_decl.name;
                    pe->func_name_len = (uint32_t)node->func_decl.name_len;
                    pe->param_index = pi;
                    pe->expected_type = expected;
                }
            }
        }

        /* Validate goto targets and duplicate labels */
        check_goto_labels(c, node->func_decl.body);

        pop_scope(c);
        c->current_func_ret = NULL;
    }

    if (node->kind == NODE_INTERRUPT && node->interrupt.body) {
        /* Ban spawn and heap alloc in interrupt handlers via function summaries.
         * Catches transitive cases: interrupt calls helper() which calls slab.alloc(). */
        check_body_effects(c, node->interrupt.body, node->loc.line,
            false, NULL,
            true, "cannot spawn inside interrupt handler — pthread_create in ISR is unsafe",
            true, "cannot allocate inside interrupt handler — heap allocation may deadlock");
        c->current_func_ret = ty_void;
        c->in_interrupt = true;
        push_scope(c);
        check_stmt(c, node->interrupt.body);
        pop_scope(c);
        c->in_interrupt = false;
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
    c->next_type_id = 1; /* 0 = unknown provenance (BUG-393) */

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

void checker_set_type(Checker *c, Node *node, Type *type) {
    typemap_set(c, node, type);
}

/* ---- Value Range Propagation helpers ---- */

/* Find the most recent range entry for a variable (stack: scan from end) */
static struct VarRange *find_var_range(Checker *c, const char *name, uint32_t name_len) {
    for (int i = c->var_range_count - 1; i >= 0; i--) {
        if (c->var_ranges[i].name_len == name_len &&
            memcmp(c->var_ranges[i].name, name, name_len) == 0)
            return &c->var_ranges[i];
    }
    return NULL;
}

/* Push a new range entry. If an existing range exists for this var,
 * intersect (narrow) rather than replace — ensures ranges only tighten.
 * For unsigned types, min is clamped to 0 (can't be negative). */
static void push_var_range(Checker *c, const char *name, uint32_t name_len,
                           int64_t min_val, int64_t max_val, bool known_nonzero) {
    /* BUG-479: skip narrowing for address-taken variables — pointer alias
     * may modify the value, so guard-narrowed range is unreliable. */
    struct VarRange *existing = find_var_range(c, name, name_len);
    if (existing && existing->address_taken) return;

    /* clamp min to 0 for unsigned variables */
    if (min_val < 0) {
        Symbol *sym = scope_lookup(c->current_scope, name, name_len);
        if (sym && sym->type && type_is_unsigned(sym->type)) {
            min_val = 0;
        }
    }
    /* intersect with existing range if present */
    if (existing) {
        if (existing->min_val > min_val) min_val = existing->min_val;
        if (existing->max_val < max_val) max_val = existing->max_val;
        if (existing->known_nonzero) known_nonzero = true;
    }
    if (c->var_range_count >= c->var_range_capacity) {
        int new_cap = c->var_range_capacity * 2;
        if (new_cap < 16) new_cap = 16;
        c->var_ranges = realloc(c->var_ranges, new_cap * sizeof(struct VarRange));
        c->var_range_capacity = new_cap;
    }
    struct VarRange *r = &c->var_ranges[c->var_range_count++];
    r->name = name;
    r->name_len = name_len;
    r->min_val = min_val;
    r->max_val = max_val;
    r->known_nonzero = known_nonzero;
    r->address_taken = false;
}

/* Refactor 1: unified VRP range update on assignment.
 * One function handles both simple ident keys ("i") and compound keys ("s.x").
 * For TOK_EQ: try literal → derive_expr_range → call return range → wipe.
 * For compound ops (+=, -=, etc.): always wipe.
 * Eliminates BUG-502 class: compound key path previously had different logic
 * from simple ident path (missing compound op check, missing call return range). */
static void vrp_invalidate_for_assign(Checker *c, const char *key, uint32_t key_len,
                                       TokenType op, Node *value) {
    struct VarRange *r = find_var_range(c, key, key_len);
    if (!r) return;
    if (op == TOK_EQ && value) {
        /* Direct assignment: try to derive new range from value */
        if (value->kind == NODE_INT_LIT) {
            int64_t v = (int64_t)value->int_lit.value;
            r->min_val = v;
            r->max_val = v;
            r->known_nonzero = (v != 0);
        } else {
            int64_t rmin, rmax;
            if (derive_expr_range(c, value, &rmin, &rmax)) {
                r->min_val = rmin;
                r->max_val = rmax;
                r->known_nonzero = (rmin > 0);
            } else if (value->kind == NODE_CALL &&
                       value->call.callee && value->call.callee->kind == NODE_IDENT) {
                Symbol *csym = scope_lookup(c->current_scope,
                    value->call.callee->ident.name,
                    (uint32_t)value->call.callee->ident.name_len);
                if (csym && csym->has_return_range) {
                    r->min_val = csym->return_range_min;
                    r->max_val = csym->return_range_max;
                    r->known_nonzero = (csym->return_range_min > 0);
                } else {
                    r->min_val = INT64_MIN;
                    r->max_val = INT64_MAX;
                    r->known_nonzero = false;
                }
            } else {
                r->min_val = INT64_MIN;
                r->max_val = INT64_MAX;
                r->known_nonzero = false;
            }
        }
    } else {
        /* Compound assignment (+=, -=, etc.) — always wipe */
        r->min_val = INT64_MIN;
        r->max_val = INT64_MAX;
        r->known_nonzero = false;
    }
}

/* Mark a node as proven safe — emitter will skip runtime check */
static void mark_proven(Checker *c, Node *node) {
    if (c->proven_safe_count >= c->proven_safe_capacity) {
        int new_cap = c->proven_safe_capacity * 2;
        if (new_cap < 16) new_cap = 16;
        c->proven_safe = realloc(c->proven_safe, new_cap * sizeof(Node *));
        c->proven_safe_capacity = new_cap;
    }
    c->proven_safe[c->proven_safe_count++] = node;
}

bool checker_is_proven(Checker *c, Node *node) {
    for (int i = 0; i < c->proven_safe_count; i++) {
        if (c->proven_safe[i] == node) return true;
    }
    return false;
}

/* Mark a node as needing auto-guard insertion by the emitter */
static void mark_auto_guard(Checker *c, Node *node, uint64_t array_size) {
    if (c->auto_guard_count >= c->auto_guard_capacity) {
        int new_cap = c->auto_guard_capacity * 2;
        if (new_cap < 16) new_cap = 16;
        c->auto_guards = realloc(c->auto_guards, new_cap * sizeof(struct AutoGuard));
        c->auto_guard_capacity = new_cap;
    }
    struct AutoGuard *g = &c->auto_guards[c->auto_guard_count++];
    g->node = node;
    g->array_size = array_size;
}

uint64_t checker_auto_guard_size(Checker *c, Node *node) {
    for (int i = 0; i < c->auto_guard_count; i++) {
        if (c->auto_guards[i].node == node) return c->auto_guards[i].array_size;
    }
    return 0;
}

/* ================================================================
 * INTERRUPT SAFETY — track globals shared between ISR and regular code
 * ================================================================ */
static void track_isr_global(Checker *c, const char *name, uint32_t name_len, bool is_compound) {
    /* find existing entry */
    for (int i = 0; i < c->isr_global_count; i++) {
        struct IsrGlobal *g = &c->isr_globals[i];
        if (g->name_len == name_len && memcmp(g->name, name, name_len) == 0) {
            if (c->in_interrupt) {
                g->from_isr = true;
                if (is_compound) g->compound_in_isr = true;
            } else {
                g->from_func = true;
                if (is_compound) g->compound_in_func = true;
            }
            return;
        }
    }
    /* add new entry */
    if (c->isr_global_count >= c->isr_global_capacity) {
        int new_cap = c->isr_global_capacity * 2;
        if (new_cap < 16) new_cap = 16;
        c->isr_globals = realloc(c->isr_globals, new_cap * sizeof(struct IsrGlobal));
        c->isr_global_capacity = new_cap;
    }
    struct IsrGlobal *g = &c->isr_globals[c->isr_global_count++];
    memset(g, 0, sizeof(*g));
    g->name = name;
    g->name_len = name_len;
    if (c->in_interrupt) {
        g->from_isr = true;
        if (is_compound) g->compound_in_isr = true;
    } else {
        g->from_func = true;
        if (is_compound) g->compound_in_func = true;
    }
}

/* Post-check: validate interrupt safety for shared globals */
static void check_interrupt_safety(Checker *c) {
    for (int i = 0; i < c->isr_global_count; i++) {
        struct IsrGlobal *g = &c->isr_globals[i];
        if (!g->from_isr || !g->from_func) continue; /* not shared */
        /* shared global — check volatile */
        Symbol *sym = scope_lookup(c->global_scope, g->name, g->name_len);
        if (!sym) continue;
        if (!sym->is_volatile) {
            checker_error(c, sym->line,
                "global '%.*s' is accessed from both interrupt and main code — "
                "must be declared volatile",
                (int)g->name_len, g->name);
        } else if (g->compound_in_isr || g->compound_in_func) {
            /* volatile but compound assignment — race condition */
            checker_error(c, sym->line,
                "volatile global '%.*s' has compound assignment (+=, |=, etc.) "
                "shared between interrupt and main code — "
                "read-modify-write is not atomic, use explicit read/mask/write",
                (int)g->name_len, g->name);
        }
    }
}

/* ================================================================
 * STACK DEPTH ANALYSIS — build call graph, compute max depth
 * ================================================================ */
static struct StackFrame *find_or_add_frame(Checker *c, const char *name, uint32_t name_len) {
    for (int i = 0; i < c->stack_frame_count; i++) {
        if (c->stack_frames[i].name_len == name_len &&
            memcmp(c->stack_frames[i].name, name, name_len) == 0)
            return &c->stack_frames[i];
    }
    if (c->stack_frame_count >= c->stack_frame_capacity) {
        int new_cap = c->stack_frame_capacity * 2;
        if (new_cap < 16) new_cap = 16;
        c->stack_frames = realloc(c->stack_frames, new_cap * sizeof(struct StackFrame));
        c->stack_frame_capacity = new_cap;
    }
    struct StackFrame *f = &c->stack_frames[c->stack_frame_count++];
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->name_len = name_len;
    return f;
}

static void add_callee(struct StackFrame *f, const char *name, uint32_t name_len) {
    /* dedup */
    for (int i = 0; i < f->callee_count; i++) {
        if (f->callee_lens[i] == name_len && memcmp(f->callees[i], name, name_len) == 0)
            return;
    }
    if (f->callee_count >= f->callee_capacity) {
        int new_cap = f->callee_capacity * 2;
        if (new_cap < 8) new_cap = 8;
        f->callees = realloc(f->callees, new_cap * sizeof(const char *));
        f->callee_lens = realloc(f->callee_lens, new_cap * sizeof(uint32_t));
        f->callee_capacity = new_cap;
    }
    f->callees[f->callee_count] = name;
    f->callee_lens[f->callee_count] = name_len;
    f->callee_count++;
}

/* estimate type size for stack frame calculation */
static uint32_t estimate_type_size(Type *t) {
    if (!t) return 0;
    t = type_unwrap_distinct(t);
    switch (t->kind) {
    case TYPE_BOOL: case TYPE_U8: case TYPE_I8: return 1;
    case TYPE_U16: case TYPE_I16: return 2;
    case TYPE_U32: case TYPE_I32: case TYPE_F32: case TYPE_USIZE: return 4;
    case TYPE_U64: case TYPE_I64: case TYPE_F64: return 8;
    case TYPE_POINTER: case TYPE_OPAQUE: return 4; /* conservative: 32-bit */
    case TYPE_HANDLE: return 8; /* u64 */
    case TYPE_ARRAY: return (uint32_t)(t->array.size * estimate_type_size(t->array.inner));
    case TYPE_SLICE: return 8; /* ptr + len */
    case TYPE_OPTIONAL: {
        Type *inner = type_unwrap_distinct(t->optional.inner);
        if (inner->kind == TYPE_POINTER || inner->kind == TYPE_FUNC_PTR) return 4; /* null sentinel */
        if (inner->kind == TYPE_VOID) return 1; /* has_value only */
        return estimate_type_size(inner) + 1; /* value + has_value */
    }
    case TYPE_STRUCT: {
        uint32_t total = 0;
        for (uint32_t i = 0; i < t->struct_type.field_count; i++)
            total += estimate_type_size(t->struct_type.fields[i].type);
        return total;
    }
    default: return 4;
    }
}

/* scan function body for local variable sizes and callee names */
static void scan_frame(Checker *c, struct StackFrame *frame, Node *node) {
    if (!node) return;
    switch (node->kind) {
    case NODE_VAR_DECL: {
        Type *t = typemap_get(c, node);
        if (!t && node->var_decl.type) t = typemap_get(c, (Node *)node->var_decl.type);
        /* rough estimate: resolve type from the type node name */
        if (t) frame->frame_size += estimate_type_size(t);
        else frame->frame_size += 4; /* unknown, assume 4 */
        if (node->var_decl.init) scan_frame(c, frame, node->var_decl.init);
        break;
    }
    case NODE_CALL:
        if (node->call.callee && node->call.callee->kind == NODE_IDENT) {
            Symbol *sym = scope_lookup(c->global_scope,
                node->call.callee->ident.name,
                (uint32_t)node->call.callee->ident.name_len);
            if (sym && sym->is_function) {
                /* Direct function call */
                add_callee(frame, node->call.callee->ident.name,
                           (uint32_t)node->call.callee->ident.name_len);
            } else if (sym && sym->type && type_unwrap_distinct(sym->type)->kind == TYPE_FUNC_PTR) {
                /* Function pointer call: check if variable was initialized with a known function.
                 * Enables indirect recursion detection: void (*fp)() = func_a; fp(); */
                bool resolved = false;
                if (sym->func_node &&
                    (sym->func_node->kind == NODE_VAR_DECL || sym->func_node->kind == NODE_GLOBAL_VAR) &&
                    sym->func_node->var_decl.init &&
                    sym->func_node->var_decl.init->kind == NODE_IDENT) {
                    const char *target = sym->func_node->var_decl.init->ident.name;
                    uint32_t tlen = (uint32_t)sym->func_node->var_decl.init->ident.name_len;
                    Symbol *tsym = scope_lookup(c->global_scope, target, tlen);
                    if (tsym && tsym->is_function) {
                        add_callee(frame, target, tlen);
                        resolved = true;
                    }
                }
                if (!resolved) frame->has_indirect_call = true;
            } else if (!sym || !sym->is_function) {
                /* Unknown callee (parameter, field, etc.) — can't compute stack depth */
                frame->has_indirect_call = true;
            }
        }
        for (int i = 0; i < node->call.arg_count; i++)
            scan_frame(c, frame, node->call.args[i]);
        break;
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            scan_frame(c, frame, node->block.stmts[i]);
        break;
    case NODE_IF:
        scan_frame(c, frame, node->if_stmt.cond);
        scan_frame(c, frame, node->if_stmt.then_body);
        scan_frame(c, frame, node->if_stmt.else_body);
        break;
    case NODE_FOR:
        scan_frame(c, frame, node->for_stmt.init);
        scan_frame(c, frame, node->for_stmt.body);
        break;
    case NODE_WHILE: case NODE_DO_WHILE:
        scan_frame(c, frame, node->while_stmt.body);
        break;
    case NODE_RETURN:
        scan_frame(c, frame, node->ret.expr);
        break;
    case NODE_EXPR_STMT:
        scan_frame(c, frame, node->expr_stmt.expr);
        break;
    case NODE_ASSIGN:
        scan_frame(c, frame, node->assign.value);
        break;
    case NODE_BINARY:
        scan_frame(c, frame, node->binary.left);
        scan_frame(c, frame, node->binary.right);
        break;
    case NODE_UNARY:
        scan_frame(c, frame, node->unary.operand);
        break;
    case NODE_ORELSE:
        scan_frame(c, frame, node->orelse.expr);
        break;
    case NODE_SWITCH:
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            scan_frame(c, frame, node->switch_stmt.arms[i].body);
        break;
    case NODE_DEFER:
        scan_frame(c, frame, node->defer.body);
        break;
    case NODE_CRITICAL:
        scan_frame(c, frame, node->critical.body);
        break;
    case NODE_ONCE:
        scan_frame(c, frame, node->once.body);
        break;
    case NODE_INTRINSIC:
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            scan_frame(c, frame, node->intrinsic.args[i]);
        break;
    case NODE_FIELD:
        scan_frame(c, frame, node->field.object);
        break;
    case NODE_INDEX:
        scan_frame(c, frame, node->index_expr.object);
        if (node->index_expr.index)
            scan_frame(c, frame, node->index_expr.index);
        break;
    case NODE_TYPECAST:
        scan_frame(c, frame, node->typecast.expr);
        break;
    case NODE_STRUCT_INIT:
        for (int i = 0; i < node->struct_init.field_count; i++)
            scan_frame(c, frame, node->struct_init.fields[i].value);
        break;
    case NODE_SLICE:
        scan_frame(c, frame, node->slice.object);
        break;
    /* Leaf nodes — no children to recurse into */
    case NODE_INT_LIT:
    case NODE_FLOAT_LIT:
    case NODE_STRING_LIT:
    case NODE_CHAR_LIT:
    case NODE_BOOL_LIT:
    case NODE_NULL_LIT:
    case NODE_IDENT:
    case NODE_BREAK:
    case NODE_CONTINUE:
    case NODE_GOTO:
    case NODE_LABEL:
    case NODE_ASM:
    case NODE_CAST:
    case NODE_SIZEOF:
        break;
    case NODE_YIELD:
        break; /* leaf — no children */
    case NODE_AWAIT:
        /* await expr — scan the condition expression */
        if (node->await_stmt.cond)
            scan_frame(c, frame, node->await_stmt.cond);
        break;
    case NODE_SPAWN:
        /* spawn func(args) — scan args for function calls */
        for (int i = 0; i < node->spawn_stmt.arg_count; i++)
            scan_frame(c, frame, node->spawn_stmt.args[i]);
        break;
    /* Top-level decls — scan_frame only called on function bodies */
    case NODE_FILE:
    case NODE_FUNC_DECL:
    case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL:
    case NODE_UNION_DECL:
    case NODE_TYPEDEF:
    case NODE_IMPORT:
    case NODE_CINCLUDE:
    case NODE_INTERRUPT:
    case NODE_MMIO:
    case NODE_CONTAINER_DECL:
    case NODE_GLOBAL_VAR:
    case NODE_STATIC_ASSERT:
        break;
    }
}

/* DFS to find max stack depth — detect recursion via visited array */
static uint32_t compute_max_depth(Checker *c, struct StackFrame *frame,
                                   bool *visited, int depth) {
    if (depth > 256) return 0; /* safety limit */
    /* find frame index */
    int idx = (int)(frame - c->stack_frames);
    if (idx < 0 || idx >= c->stack_frame_count) return 0;
    if (visited[idx]) {
        frame->is_recursive = true;
        return 0; /* cycle detected */
    }
    visited[idx] = true;
    uint32_t max_child = 0;
    for (int i = 0; i < frame->callee_count; i++) {
        struct StackFrame *callee = NULL;
        for (int j = 0; j < c->stack_frame_count; j++) {
            if (c->stack_frames[j].name_len == frame->callee_lens[i] &&
                memcmp(c->stack_frames[j].name, frame->callees[i], frame->callee_lens[i]) == 0) {
                callee = &c->stack_frames[j];
                break;
            }
        }
        if (callee) {
            uint32_t child_depth = compute_max_depth(c, callee, visited, depth + 1);
            if (child_depth > max_child) max_child = child_depth;
            /* Propagate indirect call flag up the call chain */
            if (callee->has_indirect_call) frame->has_indirect_call = true;
        }
    }
    visited[idx] = false;
    return frame->frame_size + max_child;
}

/* Build call graph and report stack depth + recursion warnings */
static void check_stack_depth(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;
    /* build frames for all functions and interrupts */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_FUNC_DECL && decl->func_decl.body && !decl->func_decl.is_comptime) {
            struct StackFrame *f = find_or_add_frame(c, decl->func_decl.name,
                (uint32_t)decl->func_decl.name_len);
            /* add param sizes */
            for (int p = 0; p < decl->func_decl.param_count; p++) {
                Type *pt = NULL;
                if (decl->func_decl.params[p].type)
                    pt = typemap_get(c, (Node *)decl->func_decl.params[p].type);
                f->frame_size += pt ? estimate_type_size(pt) : 4;
            }
            scan_frame(c, f, decl->func_decl.body);
        }
        if (decl->kind == NODE_INTERRUPT && decl->interrupt.body) {
            struct StackFrame *f = find_or_add_frame(c, decl->interrupt.name,
                (uint32_t)decl->interrupt.name_len);
            scan_frame(c, f, decl->interrupt.body);
        }
    }
    /* compute max depth from main and each interrupt */
    if (c->stack_frame_count > 0) {
        bool *visited = calloc(c->stack_frame_count, sizeof(bool));
        for (int i = 0; i < c->stack_frame_count; i++) {
            struct StackFrame *f = &c->stack_frames[i];
            uint32_t max_depth = compute_max_depth(c, f, visited, 0);
            if (f->is_recursive) {
                checker_warning(c, 0,
                    "function '%.*s' is recursive — unbounded stack growth on embedded",
                    (int)f->name_len, f->name);
            }
            /* Stack limit check (like GCC -Wstack-usage=N):
             * --stack-limit N → error when any function's frame OR
             * entry point's total call chain exceeds N bytes. */
            if (c->stack_limit > 0) {
                /* Per-function frame size check (catches big local arrays) */
                if (f->frame_size > c->stack_limit) {
                    checker_error(c, 0,
                        "function '%.*s' local stack %u bytes exceeds --stack-limit %u",
                        (int)f->name_len, f->name,
                        (unsigned)f->frame_size, (unsigned)c->stack_limit);
                }
                /* Entry point total depth check (catches deep call chains) */
                if (!f->is_recursive && max_depth > c->stack_limit) {
                    bool is_main = (f->name_len == 4 && memcmp(f->name, "main", 4) == 0);
                    bool is_isr = false;
                    for (int d = 0; d < file_node->file.decl_count; d++) {
                        if (file_node->file.decls[d]->kind == NODE_INTERRUPT &&
                            file_node->file.decls[d]->interrupt.name_len == f->name_len &&
                            memcmp(file_node->file.decls[d]->interrupt.name, f->name, f->name_len) == 0) {
                            is_isr = true; break;
                        }
                    }
                    if (is_main || is_isr) {
                        checker_error(c, 0,
                            "%s '%.*s' max call chain stack %u bytes exceeds --stack-limit %u",
                            is_isr ? "interrupt" : "entry",
                            (int)f->name_len, f->name,
                            (unsigned)max_depth, (unsigned)c->stack_limit);
                    }
                }
                /* Indirect call check: if entry point's call chain contains
                 * unresolvable function pointer calls, stack depth is unverifiable */
                if (f->has_indirect_call && !f->is_recursive) {
                    bool is_main = (f->name_len == 4 && memcmp(f->name, "main", 4) == 0);
                    if (is_main) {
                        checker_error(c, 0,
                            "entry '%.*s' call chain contains function pointer call with "
                            "unknown target — stack depth unverifiable with --stack-limit",
                            (int)f->name_len, f->name);
                    }
                }
            }
            /* Without --stack-limit: warn about indirect calls in any function */
            if (c->stack_limit == 0 && f->has_indirect_call && !f->is_recursive) {
                checker_warning(c, 0,
                    "function '%.*s' calls through function pointer with unknown target — "
                    "stack depth not verifiable",
                    (int)f->name_len, f->name);
            }
        }
        free(visited);
    }
}

/* Cross-function provenance: find return provenance in a function body */
static Type *find_return_provenance(Checker *c, Node *node) {
    if (!node) return NULL;
    if (node->kind == NODE_RETURN && node->ret.expr) {
        Node *rexpr = node->ret.expr;
        /* check for @ptrcast(*opaque, source) — source carries provenance */
        if (rexpr->kind == NODE_INTRINSIC && rexpr->intrinsic.name_len == 7 &&
            memcmp(rexpr->intrinsic.name, "ptrcast", 7) == 0 &&
            rexpr->intrinsic.arg_count > 0) {
            return typemap_get(c, rexpr->intrinsic.args[0]);
        }
        /* check if return expr is an ident with known provenance */
        if (rexpr->kind == NODE_IDENT) {
            Symbol *sym = scope_lookup(c->current_scope, rexpr->ident.name,
                (uint32_t)rexpr->ident.name_len);
            if (sym && sym->provenance_type) return sym->provenance_type;
            /* also check prov_map for compound keys */
            ExprKey pkey = build_expr_key_a(c, rexpr);
            if (pkey.len > 0) {
                Type *p = prov_map_get(c, pkey.str, (uint32_t)pkey.len);
                if (p) return p;
            }
        }
        return NULL;
    }
    /* recurse into blocks and if-bodies */
    if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++) {
            Type *p = find_return_provenance(c, node->block.stmts[i]);
            if (p) return p;
        }
    }
    if (node->kind == NODE_IF) {
        Type *p = find_return_provenance(c, node->if_stmt.then_body);
        if (p) return p;
        return find_return_provenance(c, node->if_stmt.else_body);
    }
    return NULL;
}

/* Scan a function body for return expressions with derivable range.
 * If ALL return expressions have the same derivable range, set *out_min/*out_max. */
static bool find_return_range(Checker *c, Node *node, int64_t *out_min, int64_t *out_max, bool *found) {
    if (!node) return true;
    if (node->kind == NODE_RETURN && node->ret.expr) {
        int64_t rmin, rmax;
        if (derive_expr_range(c, node->ret.expr, &rmin, &rmax)) {
            if (!*found) {
                *out_min = rmin;
                *out_max = rmax;
                *found = true;
            } else {
                /* widen to union of ranges */
                if (rmin < *out_min) *out_min = rmin;
                if (rmax > *out_max) *out_max = rmax;
            }
            return true;
        }
        /* try constant expression: return 0, return 5, return N + 1 */
        int64_t cval = eval_const_expr_scoped(c, node->ret.expr);
        if (cval != CONST_EVAL_FAIL) {
            if (!*found) {
                *out_min = cval;
                *out_max = cval;
                *found = true;
            } else {
                if (cval < *out_min) *out_min = cval;
                if (cval > *out_max) *out_max = cval;
            }
            return true;
        }
        /* try chained call range: return func() where func has return range */
        if (node->ret.expr->kind == NODE_CALL &&
            node->ret.expr->call.callee &&
            node->ret.expr->call.callee->kind == NODE_IDENT) {
            Symbol *csym = scope_lookup(c->current_scope,
                node->ret.expr->call.callee->ident.name,
                (uint32_t)node->ret.expr->call.callee->ident.name_len);
            if (!csym) csym = scope_lookup(c->global_scope,
                node->ret.expr->call.callee->ident.name,
                (uint32_t)node->ret.expr->call.callee->ident.name_len);
            if (csym && csym->has_return_range) {
                int64_t rmin = csym->return_range_min;
                int64_t rmax = csym->return_range_max;
                if (!*found) {
                    *out_min = rmin;
                    *out_max = rmax;
                    *found = true;
                } else {
                    if (rmin < *out_min) *out_min = rmin;
                    if (rmax > *out_max) *out_max = rmax;
                }
                return true;
            }
        }
        /* try parameter ident: return param — check if preceding guard
         * constrains it. Pattern: if (param >= N) { return C; } return param;
         * The guard ensures param < N at this return point.
         * Use VarRange if available (set by check_stmt during body checking). */
        if (node->ret.expr->kind == NODE_IDENT) {
            struct VarRange *r = find_var_range(c,
                node->ret.expr->ident.name,
                (uint32_t)node->ret.expr->ident.name_len);
            if (r && r->min_val >= 0 && r->max_val >= 0) {
                int64_t rmin = r->min_val, rmax = r->max_val;
                if (!*found) {
                    *out_min = rmin;
                    *out_max = rmax;
                    *found = true;
                } else {
                    if (rmin < *out_min) *out_min = rmin;
                    if (rmax > *out_max) *out_max = rmax;
                }
                return true;
            }
        }
        return false; /* return without derivable range — give up */
    }
    if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++) {
            if (!find_return_range(c, node->block.stmts[i], out_min, out_max, found))
                return false;
        }
        return true;
    }
    if (node->kind == NODE_IF) {
        if (!find_return_range(c, node->if_stmt.then_body, out_min, out_max, found))
            return false;
        return find_return_range(c, node->if_stmt.else_body, out_min, out_max, found);
    }
    if (node->kind == NODE_SWITCH) {
        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            if (!find_return_range(c, node->switch_stmt.arms[i].body, out_min, out_max, found))
                return false;
        }
        return true;
    }
    if (node->kind == NODE_FOR || node->kind == NODE_WHILE) {
        Node *body = (node->kind == NODE_FOR) ? node->for_stmt.body : node->while_stmt.body;
        return find_return_range(c, body, out_min, out_max, found);
    }
    if (node->kind == NODE_CRITICAL) {
        return find_return_range(c, node->critical.body, out_min, out_max, found);
    }
    if (node->kind == NODE_ONCE) {
        return find_return_range(c, node->once.body, out_min, out_max, found);
    }
    return true; /* non-return statement — ok */
}

/* Find what type a *opaque parameter is cast to inside a function body.
 * Scans for @ptrcast(*T, param_name) → returns *T (the target pointer type).
 * Uses typemap to get the resolved type of the @ptrcast node. */
static Type *find_param_cast_type(Checker *c, Node *node,
                                   const char *param_name, uint32_t param_len) {
    if (!node) return NULL;
    if (node->kind == NODE_INTRINSIC && node->intrinsic.name_len == 7 &&
        memcmp(node->intrinsic.name, "ptrcast", 7) == 0 &&
        node->intrinsic.arg_count > 0) {
        Node *src = node->intrinsic.args[0];
        if (src->kind == NODE_IDENT && src->ident.name_len == param_len &&
            memcmp(src->ident.name, param_name, param_len) == 0) {
            /* found @ptrcast(*T, param) — return the result type from typemap */
            Type *result = typemap_get(c, node);
            if (result) return result;
        }
    }
    /* recurse into all statement/expression kinds */
    switch (node->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++) {
            Type *t = find_param_cast_type(c, node->block.stmts[i], param_name, param_len);
            if (t) return t;
        }
        break;
    case NODE_IF: {
        Type *t = find_param_cast_type(c, node->if_stmt.then_body, param_name, param_len);
        if (t) return t;
        return find_param_cast_type(c, node->if_stmt.else_body, param_name, param_len);
    }
    case NODE_VAR_DECL:
        return find_param_cast_type(c, node->var_decl.init, param_name, param_len);
    case NODE_EXPR_STMT:
        return find_param_cast_type(c, node->expr_stmt.expr, param_name, param_len);
    case NODE_RETURN:
        return find_param_cast_type(c, node->ret.expr, param_name, param_len);
    case NODE_ASSIGN:
        return find_param_cast_type(c, node->assign.value, param_name, param_len);
    case NODE_CALL:
        for (int i = 0; i < node->call.arg_count; i++) {
            Type *t = find_param_cast_type(c, node->call.args[i], param_name, param_len);
            if (t) return t;
        }
        break;
    default: break;
    }
    return NULL;
}

static void add_prov_summary(Checker *c, const char *name, uint32_t name_len, Type *prov) {
    if (c->prov_summary_count >= c->prov_summary_capacity) {
        int new_cap = c->prov_summary_capacity * 2;
        if (new_cap < 8) new_cap = 8;
        c->prov_summaries = realloc(c->prov_summaries, new_cap * sizeof(struct ProvSummary));
        c->prov_summary_capacity = new_cap;
    }
    struct ProvSummary *s = &c->prov_summaries[c->prov_summary_count++];
    s->func_name = name;
    s->func_name_len = name_len;
    s->return_provenance = prov;
}

static Type *lookup_prov_summary(Checker *c, const char *name, uint32_t name_len) {
    for (int i = 0; i < c->prov_summary_count; i++) {
        if (c->prov_summaries[i].func_name_len == name_len &&
            memcmp(c->prov_summaries[i].func_name, name, name_len) == 0)
            return c->prov_summaries[i].return_provenance;
    }
    return NULL;
}

/* Check if an if-body guarantees early exit (return/break/continue) */
static bool body_always_exits(Node *body) {
    if (!body) return false;
    if (body->kind == NODE_RETURN || body->kind == NODE_BREAK ||
        body->kind == NODE_CONTINUE) return true;
    if (body->kind == NODE_BLOCK && body->block.stmt_count > 0) {
        Node *last = body->block.stmts[body->block.stmt_count - 1];
        return last->kind == NODE_RETURN || last->kind == NODE_BREAK ||
               last->kind == NODE_CONTINUE;
    }
    /* expression statement with orelse return/break */
    if (body->kind == NODE_EXPR_STMT && body->expr_stmt.expr &&
        body->expr_stmt.expr->kind == NODE_ORELSE &&
        (body->expr_stmt.expr->orelse.fallback_is_return ||
         body->expr_stmt.expr->orelse.fallback_is_break)) return true;
    return false;
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
        /* Top-level static_assert — evaluate during body checking phase */
        if (decl->kind == NODE_STATIC_ASSERT) {
            check_expr(c, decl->static_assert_stmt.cond);
            int64_t val = eval_const_expr_scoped(c, decl->static_assert_stmt.cond);
            if (val == CONST_EVAL_FAIL) {
                checker_error(c, decl->loc.line,
                    "static_assert condition must be a compile-time constant");
            } else if (!val) {
                if (decl->static_assert_stmt.message) {
                    checker_error(c, decl->loc.line, "static_assert failed: %.*s",
                        (int)decl->static_assert_stmt.message_len,
                        decl->static_assert_stmt.message);
                } else {
                    checker_error(c, decl->loc.line, "static_assert failed");
                }
            }
        }
        if (decl->kind == NODE_GLOBAL_VAR && decl->var_decl.init) {
            Type *type = resolve_type(c, decl->var_decl.type);
            Type *init = check_expr(c, decl->var_decl.init);
            /* global initializers must be constant expressions in C */
            Node *ginit = decl->var_decl.init;
            if (ginit->kind == NODE_CALL && !ginit->call.is_comptime_resolved) {
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

/* Post-check: validate *opaque call-site provenance against param expectations.
 * Scans all call expressions in the file, checks argument provenance. */
static void check_call_provenance(Checker *c, Node *node) {
    if (!node) return;
    if (node->kind == NODE_CALL && node->call.callee->kind == NODE_IDENT) {
        const char *fname = node->call.callee->ident.name;
        uint32_t flen = (uint32_t)node->call.callee->ident.name_len;

        /* check each argument against param expectations */
        for (int i = 0; i < node->call.arg_count; i++) {
            /* find expected type for this param */
            Type *expected = NULL;
            for (int pe = 0; pe < c->param_expect_count; pe++) {
                if (c->param_expects[pe].param_index == i &&
                    c->param_expects[pe].func_name_len == flen &&
                    memcmp(c->param_expects[pe].func_name, fname, flen) == 0) {
                    expected = c->param_expects[pe].expected_type;
                    break;
                }
            }
            if (!expected) continue;

            /* get argument provenance */
            Type *arg_prov = NULL;
            Node *arg = node->call.args[i];

            /* @ptrcast(*opaque, &source) — source type is the provenance */
            if (arg->kind == NODE_INTRINSIC && arg->intrinsic.name_len == 7 &&
                memcmp(arg->intrinsic.name, "ptrcast", 7) == 0 &&
                arg->intrinsic.arg_count > 0) {
                arg_prov = typemap_get(c, arg->intrinsic.args[0]);
            }
            /* ident with known provenance */
            if (!arg_prov && arg->kind == NODE_IDENT) {
                Symbol *sym = scope_lookup(c->global_scope, arg->ident.name,
                    (uint32_t)arg->ident.name_len);
                if (sym && sym->provenance_type) arg_prov = sym->provenance_type;
            }

            if (!arg_prov) continue; /* unknown provenance — runtime handles it */

            /* compare: expected is *Sensor, arg_prov is the source type (*Sensor or *Motor) */
            Type *exp_inner = type_unwrap_distinct(expected);
            if (exp_inner && exp_inner->kind == TYPE_POINTER) exp_inner = exp_inner->pointer.inner;
            Type *arg_inner = type_unwrap_distinct(arg_prov);
            if (arg_inner && arg_inner->kind == TYPE_POINTER) arg_inner = arg_inner->pointer.inner;

            if (exp_inner && arg_inner && !type_equals(exp_inner, arg_inner)) {
                checker_error(c, node->loc.line,
                    "wrong *opaque type: function '%.*s' expects '%s' for parameter %d, "
                    "but caller passes '%s'",
                    (int)flen, fname, type_name(expected), i + 1, type_name(arg_prov));
            }
        }
    }

    /* recurse into all node types */
    switch (node->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            check_call_provenance(c, node->block.stmts[i]);
        break;
    case NODE_IF:
        check_call_provenance(c, node->if_stmt.then_body);
        check_call_provenance(c, node->if_stmt.else_body);
        break;
    case NODE_FOR:
        check_call_provenance(c, node->for_stmt.body);
        break;
    case NODE_WHILE: case NODE_DO_WHILE:
        check_call_provenance(c, node->while_stmt.body);
        break;
    case NODE_FUNC_DECL:
        check_call_provenance(c, node->func_decl.body);
        break;
    case NODE_EXPR_STMT:
        check_call_provenance(c, node->expr_stmt.expr);
        break;
    case NODE_VAR_DECL:
        check_call_provenance(c, node->var_decl.init);
        break;
    case NODE_RETURN:
        check_call_provenance(c, node->ret.expr);
        break;
    default: break;
    }
}

bool checker_check(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return false;

    /* Pass 1: register all top-level declarations */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        register_decl(c, file_node->file.decls[i]);
    }

    /* Pass 2: type-check all function bodies and global initializers */
    bool ok = checker_check_bodies(c, file_node);

    /* Pass 3: whole-program *opaque param provenance validation */
    if (c->param_expect_count > 0) {
        for (int i = 0; i < file_node->file.decl_count; i++) {
            check_call_provenance(c, file_node->file.decls[i]);
        }
    }

    /* Pass 4: interrupt safety — validate shared globals */
    if (c->isr_global_count > 0) {
        check_interrupt_safety(c);
    }

    /* Pass 5: stack depth analysis — detect recursion */
    check_stack_depth(c, file_node);

    return c->error_count == 0;
}

/* Find the shared struct type accessed in an expression (for lock ordering) */
static Type *find_shared_type_in_expr(Checker *c, Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == NODE_FIELD) {
        Node *root = expr;
        while (root->kind == NODE_FIELD) root = root->field.object;
        while (root->kind == NODE_INDEX) root = root->index_expr.object;
        if (root->kind == NODE_IDENT) {
            Type *t = typemap_get(c, root);
            if (!t) {
                Symbol *sym = scope_lookup(c->current_scope,
                    root->ident.name, (uint32_t)root->ident.name_len);
                if (sym) t = sym->type;
            }
            if (t) {
                Type *eff = type_unwrap_distinct(t);
                if (eff->kind == TYPE_STRUCT && eff->struct_type.is_shared) return eff;
                if (eff->kind == TYPE_POINTER) {
                    Type *inner = type_unwrap_distinct(eff->pointer.inner);
                    if (inner && inner->kind == TYPE_STRUCT && inner->struct_type.is_shared)
                        return inner;
                }
            }
        }
    }
    /* Recurse */
    if (expr->kind == NODE_BINARY) {
        Type *l = find_shared_type_in_expr(c, expr->binary.left);
        return l ? l : find_shared_type_in_expr(c, expr->binary.right);
    }
    if (expr->kind == NODE_ASSIGN) {
        Type *l = find_shared_type_in_expr(c, expr->assign.target);
        return l ? l : find_shared_type_in_expr(c, expr->assign.value);
    }
    if (expr->kind == NODE_UNARY) return find_shared_type_in_expr(c, expr->unary.operand);
    if (expr->kind == NODE_CALL) {
        for (int i = 0; i < expr->call.arg_count; i++) {
            Type *r = find_shared_type_in_expr(c, expr->call.args[i]);
            if (r) return r;
        }
    }
    return NULL;
}

/* Find shared type in a statement */
static Type *find_shared_type_in_stmt(Checker *c, Node *stmt) {
    if (!stmt) return NULL;
    switch (stmt->kind) {
    case NODE_EXPR_STMT: return find_shared_type_in_expr(c, stmt->expr_stmt.expr);
    case NODE_VAR_DECL: return find_shared_type_in_expr(c, stmt->var_decl.init);
    case NODE_RETURN: return find_shared_type_in_expr(c, stmt->ret.expr);
    case NODE_IF: return find_shared_type_in_expr(c, stmt->if_stmt.cond);
    case NODE_WHILE: case NODE_DO_WHILE: return find_shared_type_in_expr(c, stmt->while_stmt.cond);
    case NODE_FOR: {
        Type *r = find_shared_type_in_expr(c, stmt->for_stmt.init);
        if (!r && stmt->for_stmt.cond) r = find_shared_type_in_expr(c, stmt->for_stmt.cond);
        return r;
    }
    case NODE_SWITCH: return find_shared_type_in_expr(c, stmt->switch_stmt.expr);
    default: return NULL;
    }
}

/* ---- Per-function shared type cache for deadlock detection (BUG-474 proper fix) ----
 * DFS with memoization + cycle detection. No depth limit. Each function
 * computed once, result cached. Call graph traversal visits each function
 * at most once per query chain (in_progress flag = cycle = stop). */

static struct FuncSharedTypes *find_func_shared_cache(Checker *c,
    const char *name, uint32_t len) {
    for (int i = 0; i < c->func_shared_cache_count; i++) {
        if (c->func_shared_cache[i].func_name_len == len &&
            memcmp(c->func_shared_cache[i].func_name, name, len) == 0)
            return &c->func_shared_cache[i];
    }
    return NULL;
}

static struct FuncSharedTypes *add_func_shared_cache(Checker *c,
    const char *name, uint32_t len) {
    if (c->func_shared_cache_count >= c->func_shared_cache_capacity) {
        int nc = c->func_shared_cache_capacity < 16 ? 16 : c->func_shared_cache_capacity * 2;
        c->func_shared_cache = realloc(c->func_shared_cache, nc * sizeof(struct FuncSharedTypes));
        c->func_shared_cache_capacity = nc;
    }
    struct FuncSharedTypes *entry = &c->func_shared_cache[c->func_shared_cache_count++];
    memset(entry, 0, sizeof(*entry));
    entry->func_name = name;
    entry->func_name_len = len;
    return entry;
}

static void fsc_add_type_id(struct FuncSharedTypes *fsc, uint32_t type_id) {
    for (int i = 0; i < fsc->type_count; i++)
        if (fsc->type_ids[i] == type_id) return; /* dedup */
    if (fsc->type_count >= fsc->type_capacity) {
        int nc = fsc->type_capacity < 8 ? 8 : fsc->type_capacity * 2;
        uint32_t *nids = realloc(fsc->type_ids, nc * sizeof(uint32_t));
        if (!nids) return;
        fsc->type_ids = nids;
        fsc->type_capacity = nc;
    }
    fsc->type_ids[fsc->type_count++] = type_id;
}

/* Scan a function body for direct shared struct field accesses (non-transitive).
 * Collects type_ids into the FuncSharedTypes entry. */
static void scan_body_shared_types(Checker *c, Node *node, struct FuncSharedTypes *fsc);

/* Compute shared types for a function transitively via DFS.
 * Uses in_progress flag for cycle detection, computed flag for memoization. */
static void compute_func_shared_types(Checker *c, const char *fname, uint32_t flen) {
    struct FuncSharedTypes *fsc = find_func_shared_cache(c, fname, flen);
    if (!fsc) fsc = add_func_shared_cache(c, fname, flen);
    if (fsc->computed || fsc->in_progress) return; /* memoized or cycle */
    fsc->in_progress = true;

    Symbol *sym = scope_lookup(c->global_scope, fname, flen);
    if (!sym || !sym->is_function || !sym->func_node ||
        sym->func_node->kind != NODE_FUNC_DECL || !sym->func_node->func_decl.body) {
        fsc->computed = true;
        fsc->in_progress = false;
        return;
    }

    /* 1. Scan body for direct shared accesses */
    scan_body_shared_types(c, sym->func_node->func_decl.body, fsc);

    /* 2. For each callee, compute transitively and merge */
    Node *body = sym->func_node->func_decl.body;
    if (body->kind == NODE_BLOCK) {
        for (int i = 0; i < body->block.stmt_count; i++)
            scan_body_shared_types(c, body->block.stmts[i], fsc);
    }

    fsc->computed = true;
    fsc->in_progress = false;
}

/* Recursive body scanner — finds shared field accesses + callee calls */
static void scan_body_shared_types(Checker *c, Node *node, struct FuncSharedTypes *fsc) {
    if (!node) return;
    /* Direct shared field access */
    if (node->kind == NODE_FIELD) {
        Node *root = node;
        while (root->kind == NODE_FIELD) root = root->field.object;
        while (root->kind == NODE_INDEX) root = root->index_expr.object;
        if (root->kind == NODE_IDENT) {
            Symbol *sym = scope_lookup(c->global_scope,
                root->ident.name, (uint32_t)root->ident.name_len);
            if (!sym) sym = scope_lookup(c->current_scope,
                root->ident.name, (uint32_t)root->ident.name_len);
            if (sym && sym->type) {
                Type *eff = type_unwrap_distinct(sym->type);
                if (eff->kind == TYPE_STRUCT && eff->struct_type.is_shared)
                    fsc_add_type_id(fsc, eff->struct_type.type_id);
                if (eff->kind == TYPE_POINTER) {
                    Type *inner = type_unwrap_distinct(eff->pointer.inner);
                    if (inner && inner->kind == TYPE_STRUCT && inner->struct_type.is_shared)
                        fsc_add_type_id(fsc, inner->struct_type.type_id);
                }
            }
        }
    }
    /* Function call — compute callee transitively and merge */
    if (node->kind == NODE_CALL && node->call.callee &&
        node->call.callee->kind == NODE_IDENT) {
        const char *cn = node->call.callee->ident.name;
        uint32_t cl = (uint32_t)node->call.callee->ident.name_len;
        compute_func_shared_types(c, cn, cl);
        struct FuncSharedTypes *callee_fsc = find_func_shared_cache(c, cn, cl);
        if (callee_fsc) {
            for (int i = 0; i < callee_fsc->type_count; i++)
                fsc_add_type_id(fsc, callee_fsc->type_ids[i]);
        }
    }
    /* Recurse into all children */
    if (node->kind == NODE_BINARY) {
        scan_body_shared_types(c, node->binary.left, fsc);
        scan_body_shared_types(c, node->binary.right, fsc);
    }
    if (node->kind == NODE_ASSIGN) {
        scan_body_shared_types(c, node->assign.target, fsc);
        scan_body_shared_types(c, node->assign.value, fsc);
    }
    if (node->kind == NODE_UNARY)
        scan_body_shared_types(c, node->unary.operand, fsc);
    if (node->kind == NODE_CALL) {
        for (int i = 0; i < node->call.arg_count; i++)
            scan_body_shared_types(c, node->call.args[i], fsc);
    }
    if (node->kind == NODE_BLOCK) {
        for (int i = 0; i < node->block.stmt_count; i++)
            scan_body_shared_types(c, node->block.stmts[i], fsc);
    }
    if (node->kind == NODE_RETURN && node->ret.expr)
        scan_body_shared_types(c, node->ret.expr, fsc);
    if (node->kind == NODE_EXPR_STMT)
        scan_body_shared_types(c, node->expr_stmt.expr, fsc);
    if (node->kind == NODE_VAR_DECL && node->var_decl.init)
        scan_body_shared_types(c, node->var_decl.init, fsc);
    if (node->kind == NODE_IF) {
        scan_body_shared_types(c, node->if_stmt.cond, fsc);
        scan_body_shared_types(c, node->if_stmt.then_body, fsc);
        scan_body_shared_types(c, node->if_stmt.else_body, fsc);
    }
    if (node->kind == NODE_WHILE || node->kind == NODE_DO_WHILE) {
        scan_body_shared_types(c, node->while_stmt.cond, fsc);
        scan_body_shared_types(c, node->while_stmt.body, fsc);
    }
    if (node->kind == NODE_FOR) {
        scan_body_shared_types(c, node->for_stmt.init, fsc);
        scan_body_shared_types(c, node->for_stmt.cond, fsc);
        scan_body_shared_types(c, node->for_stmt.step, fsc);
        scan_body_shared_types(c, node->for_stmt.body, fsc);
    }
    if (node->kind == NODE_SWITCH) {
        scan_body_shared_types(c, node->switch_stmt.expr, fsc);
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            scan_body_shared_types(c, node->switch_stmt.arms[i].body, fsc);
    }
    if (node->kind == NODE_DEFER)
        scan_body_shared_types(c, node->defer.body, fsc);
    if (node->kind == NODE_CRITICAL)
        scan_body_shared_types(c, node->critical.body, fsc);
    if (node->kind == NODE_ONCE)
        scan_body_shared_types(c, node->once.body, fsc);
    if (node->kind == NODE_ORELSE) {
        scan_body_shared_types(c, node->orelse.expr, fsc);
        scan_body_shared_types(c, node->orelse.fallback, fsc);
    }
    if (node->kind == NODE_INTRINSIC) {
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            scan_body_shared_types(c, node->intrinsic.args[i], fsc);
    }
}

/* Collect ALL shared types in an expression (not just the first).
 * Returns count of distinct shared types found (max 2 for deadlock check). */
static int collect_shared_types_in_expr(Checker *c, Node *expr,
                                         Type **types, int max_types, int count) {
    if (!expr || count >= max_types) return count;
    if (expr->kind == NODE_FIELD) {
        Node *root = expr;
        while (root->kind == NODE_FIELD) root = root->field.object;
        while (root->kind == NODE_INDEX) root = root->index_expr.object;
        if (root->kind == NODE_IDENT) {
            Type *t = typemap_get(c, root);
            if (!t) {
                Symbol *sym = scope_lookup(c->current_scope,
                    root->ident.name, (uint32_t)root->ident.name_len);
                if (sym) t = sym->type;
            }
            if (t) {
                Type *eff = type_unwrap_distinct(t);
                Type *shared = NULL;
                if (eff->kind == TYPE_STRUCT && eff->struct_type.is_shared) shared = eff;
                if (!shared && eff->kind == TYPE_POINTER) {
                    Type *inner = type_unwrap_distinct(eff->pointer.inner);
                    if (inner && inner->kind == TYPE_STRUCT && inner->struct_type.is_shared)
                        shared = inner;
                }
                if (shared) {
                    /* Check if already in the list */
                    bool dup = false;
                    for (int i = 0; i < count; i++) {
                        if (types[i]->struct_type.type_id == shared->struct_type.type_id) {
                            dup = true; break;
                        }
                    }
                    if (!dup && count < max_types) {
                        types[count++] = shared;
                    }
                }
            }
        }
    }
    /* Recurse */
    if (expr->kind == NODE_BINARY) {
        count = collect_shared_types_in_expr(c, expr->binary.left, types, max_types, count);
        count = collect_shared_types_in_expr(c, expr->binary.right, types, max_types, count);
    }
    if (expr->kind == NODE_ASSIGN) {
        count = collect_shared_types_in_expr(c, expr->assign.target, types, max_types, count);
        count = collect_shared_types_in_expr(c, expr->assign.value, types, max_types, count);
    }
    if (expr->kind == NODE_UNARY)
        count = collect_shared_types_in_expr(c, expr->unary.operand, types, max_types, count);
    /* Statement nodes that may appear when scanning callee bodies transitively */
    if (expr->kind == NODE_RETURN && expr->ret.expr)
        return collect_shared_types_in_expr(c, expr->ret.expr, types, max_types, count);
    if (expr->kind == NODE_EXPR_STMT)
        return collect_shared_types_in_expr(c, expr->expr_stmt.expr, types, max_types, count);
    if (expr->kind == NODE_VAR_DECL && expr->var_decl.init)
        return collect_shared_types_in_expr(c, expr->var_decl.init, types, max_types, count);
    if (expr->kind == NODE_CALL) {
        for (int i = 0; i < expr->call.arg_count && count < max_types; i++)
            count = collect_shared_types_in_expr(c, expr->call.args[i], types, max_types, count);
        /* Transitive: look up callee's cached shared types (BUG-474 proper fix).
         * Uses DFS with memoization — no depth limit, handles mutual recursion.
         * Each function computed once via compute_func_shared_types(). */
        if (count < max_types && expr->call.callee && expr->call.callee->kind == NODE_IDENT) {
            const char *cn = expr->call.callee->ident.name;
            uint32_t cl = (uint32_t)expr->call.callee->ident.name_len;
            compute_func_shared_types(c, cn, cl);
            struct FuncSharedTypes *fsc = find_func_shared_cache(c, cn, cl);
            if (fsc) {
                for (int fi = 0; fi < fsc->type_count && count < max_types; fi++) {
                    /* Check if already in the output list */
                    bool dup = false;
                    for (int di = 0; di < count; di++) {
                        if (types[di]->struct_type.type_id == fsc->type_ids[fi]) {
                            dup = true; break;
                        }
                    }
                    if (!dup) {
                        /* Find the Type* for this type_id — scan global scope */
                        for (uint32_t si = 0; si < c->global_scope->symbol_count; si++) {
                            Symbol *s = &c->global_scope->symbols[si];
                            if (s->type) {
                                Type *eff = type_unwrap_distinct(s->type);
                                if (eff->kind == TYPE_STRUCT && eff->struct_type.is_shared &&
                                    eff->struct_type.type_id == fsc->type_ids[fi]) {
                                    types[count++] = eff;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return count;
}

static int collect_shared_types_in_stmt(Checker *c, Node *stmt, Type **types, int max_types) {
    if (!stmt) return 0;
    switch (stmt->kind) {
    case NODE_EXPR_STMT: return collect_shared_types_in_expr(c, stmt->expr_stmt.expr, types, max_types, 0);
    case NODE_VAR_DECL: return collect_shared_types_in_expr(c, stmt->var_decl.init, types, max_types, 0);
    case NODE_RETURN: return collect_shared_types_in_expr(c, stmt->ret.expr, types, max_types, 0);
    case NODE_IF: return collect_shared_types_in_expr(c, stmt->if_stmt.cond, types, max_types, 0);
    case NODE_WHILE: case NODE_DO_WHILE: return collect_shared_types_in_expr(c, stmt->while_stmt.cond, types, max_types, 0);
    case NODE_FOR: {
        int n = collect_shared_types_in_expr(c, stmt->for_stmt.init, types, max_types, 0);
        if (stmt->for_stmt.cond)
            n = collect_shared_types_in_expr(c, stmt->for_stmt.cond, types, max_types, n);
        return n;
    }
    case NODE_SWITCH: return collect_shared_types_in_expr(c, stmt->switch_stmt.expr, types, max_types, 0);
    default: return 0;
    }
}

static void check_block_lock_ordering(Checker *c, Node *block) {
    if (!block || block->kind != NODE_BLOCK) return;

    /* BUG-464: Deadlock detection based on the correct locking model.
     *
     * The emitter wraps each shared struct access in lock→op→unlock per statement
     * group. Groups contain consecutive accesses to the SAME shared variable.
     * Different shared types are NEVER locked simultaneously across statements.
     *
     * The ONLY real deadlock scenario: a SINGLE statement/expression accesses
     * TWO DIFFERENT shared types — the emitter only locks one, leaving the other
     * unprotected, and another thread locking them in opposite order deadlocks.
     *
     * Cross-statement ordering (A then B then A) is safe because locks are fully
     * released between groups. */

    for (int i = 0; i < block->block.stmt_count; i++) {
        Node *stmt = block->block.stmts[i];

        /* Check for multi-shared-type expressions within a single statement */
        Type *found[4];
        int n = collect_shared_types_in_stmt(c, stmt, found, 4);
        if (n >= 2) {
            /* Two different shared types in one statement — potential deadlock.
             * BUG-500: skip for shared(rw) read-only statements. rwlock allows
             * concurrent readers — no deadlock. Only writes need exclusive lock. */
            uint32_t id0 = found[0]->struct_type.type_id;
            uint32_t id1 = found[1]->struct_type.type_id;
            bool both_rw = found[0]->struct_type.is_shared_rw &&
                           found[1]->struct_type.is_shared_rw;
            /* Check if statement is read-only (no NODE_ASSIGN to shared field) */
            bool is_read_only = (stmt->kind != NODE_EXPR_STMT ||
                !stmt->expr_stmt.expr || stmt->expr_stmt.expr->kind != NODE_ASSIGN) &&
                stmt->kind != NODE_ASSIGN;
            if (stmt->kind == NODE_VAR_DECL) is_read_only = true; /* reading into local */
            if (both_rw && is_read_only) {
                /* Safe: two shared(rw) read-only accesses — concurrent readers OK */
            } else if (id0 != id1) {
                Type *lo = (id0 < id1) ? found[0] : found[1];
                Type *hi = (id0 < id1) ? found[1] : found[0];
                checker_error(c, stmt->loc.line,
                    "deadlock: single statement accesses both '%.*s' (order %u) and '%.*s' (order %u) — "
                    "split into separate statements to avoid holding two locks",
                    (int)lo->struct_type.name_len, lo->struct_type.name, lo->struct_type.type_id,
                    (int)hi->struct_type.name_len, hi->struct_type.name, hi->struct_type.type_id);
            }
        }

        /* Recurse into nested blocks */
        if (stmt->kind == NODE_BLOCK) check_block_lock_ordering(c, stmt);
        if (stmt->kind == NODE_IF) {
            check_block_lock_ordering(c, stmt->if_stmt.then_body);
            check_block_lock_ordering(c, stmt->if_stmt.else_body);
        }
        if (stmt->kind == NODE_FOR) check_block_lock_ordering(c, stmt->for_stmt.body);
        if (stmt->kind == NODE_WHILE) check_block_lock_ordering(c, stmt->while_stmt.body);
    }
}

/* Walk all functions and check lock ordering */
static void check_lock_ordering(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_FUNC_DECL && decl->func_decl.body) {
            check_block_lock_ordering(c, decl->func_decl.body);
        }
    }
}

void checker_post_passes(Checker *c, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return;

    /* Whole-program *opaque param provenance validation */
    if (c->param_expect_count > 0) {
        for (int i = 0; i < file_node->file.decl_count; i++) {
            check_call_provenance(c, file_node->file.decls[i]);
        }
    }

    /* Interrupt safety — validate shared globals */
    if (c->isr_global_count > 0) {
        check_interrupt_safety(c);
    }

    /* Stack depth analysis — detect recursion */
    check_stack_depth(c, file_node);

    /* Deadlock detection — lock ordering on shared structs.
     * Within any block, shared struct accesses must be in ascending type_id order.
     * If A (type_id=1) is accessed, then B (type_id=2), then A again after B → OK.
     * But if B is accessed first, then A → "potential deadlock: lock ordering violation." */
    check_lock_ordering(c, file_node);
}
