#include "types.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * Target configuration
 * ================================================================ */
int zer_target_ptr_bits = 32; /* default 32-bit for embedded targets */

/* ================================================================
 * Global type singletons
 * ================================================================ */

Type *ty_void;
Type *ty_bool;
Type *ty_u8;
Type *ty_u16;
Type *ty_u32;
Type *ty_u64;
Type *ty_usize;
Type *ty_i8;
Type *ty_i16;
Type *ty_i32;
Type *ty_i64;
Type *ty_f32;
Type *ty_f64;
Type *ty_opaque;
Type *ty_arena;

static Type *make_primitive(Arena *a, TypeKind kind) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = kind;
    return t;
}

void types_init(Arena *a) {
    ty_void   = make_primitive(a, TYPE_VOID);
    ty_bool   = make_primitive(a, TYPE_BOOL);
    ty_u8     = make_primitive(a, TYPE_U8);
    ty_u16    = make_primitive(a, TYPE_U16);
    ty_u32    = make_primitive(a, TYPE_U32);
    ty_u64    = make_primitive(a, TYPE_U64);
    ty_usize  = make_primitive(a, TYPE_USIZE);
    ty_i8     = make_primitive(a, TYPE_I8);
    ty_i16    = make_primitive(a, TYPE_I16);
    ty_i32    = make_primitive(a, TYPE_I32);
    ty_i64    = make_primitive(a, TYPE_I64);
    ty_f32    = make_primitive(a, TYPE_F32);
    ty_f64    = make_primitive(a, TYPE_F64);
    ty_opaque = make_primitive(a, TYPE_OPAQUE);
    ty_arena  = make_primitive(a, TYPE_ARENA);
}

/* ================================================================
 * Type constructors
 * ================================================================ */

Type *type_pointer(Arena *a, Type *inner) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_POINTER;
    t->pointer.inner = inner;
    t->pointer.is_const = false;
    t->pointer.is_volatile = false;
    return t;
}

Type *type_const_pointer(Arena *a, Type *inner) {
    Type *t = type_pointer(a, inner);
    t->pointer.is_const = true;
    return t;
}

Type *type_optional(Arena *a, Type *inner) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_OPTIONAL;
    t->optional.inner = inner;
    return t;
}

Type *type_slice(Arena *a, Type *inner) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_SLICE;
    t->slice.inner = inner;
    t->slice.is_const = false;
    return t;
}

Type *type_const_slice(Arena *a, Type *inner) {
    Type *t = type_slice(a, inner);
    t->slice.is_const = true;
    return t;
}

Type *type_volatile_slice(Arena *a, Type *inner) {
    Type *t = type_slice(a, inner);
    t->slice.is_volatile = true;
    return t;
}

/* NOTE: size == 0 with sizeof_type != NULL means "emit sizeof(T)" —
 * used for target-dependent array dimensions (@size on pointer/slice types).
 * The emitter checks sizeof_type first; if set, emits sizeof(T) instead of numeric size. */
Type *type_array(Arena *a, Type *inner, uint64_t size) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_ARRAY;
    t->array.inner = inner;
    t->array.size = size;
    return t;
}

Type *type_pool(Arena *a, Type *elem, uint64_t count) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_POOL;
    t->pool.elem = elem;
    t->pool.count = count;
    return t;
}

Type *type_ring(Arena *a, Type *elem, uint64_t count) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_RING;
    t->ring.elem = elem;
    t->ring.count = count;
    return t;
}

Type *type_handle(Arena *a, Type *elem) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_HANDLE;
    t->handle.elem = elem;
    return t;
}

Type *type_slab(Arena *a, Type *elem) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_SLAB;
    t->slab.elem = elem;
    return t;
}

Type *type_func_ptr(Arena *a, Type **params, uint32_t param_count, Type *ret) {
    Type *t = (Type *)arena_alloc(a, sizeof(Type));
    t->kind = TYPE_FUNC_PTR;
    t->func_ptr.params = params;
    t->func_ptr.param_count = param_count;
    t->func_ptr.ret = ret;
    return t;
}

/* ================================================================
 * Type queries
 * ================================================================ */

bool type_is_integer(Type *a) {
    a = type_unwrap_distinct(a);
    switch (a->kind) {
    case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
    case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
    case TYPE_USIZE:
    case TYPE_ENUM: /* enums are i32 internally */
        return true;
    default:
        return false;
    }
}

bool type_is_signed(Type *a) {
    a = type_unwrap_distinct(a);
    switch (a->kind) {
    case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
    case TYPE_ENUM: /* enums are i32 */
        return true;
    default:
        return false;
    }
}

bool type_is_unsigned(Type *a) {
    a = type_unwrap_distinct(a);
    switch (a->kind) {
    case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_USIZE:
        return true;
    default:
        return false;
    }
}

bool type_is_float(Type *a) {
    a = type_unwrap_distinct(a);
    return a->kind == TYPE_F32 || a->kind == TYPE_F64;
}

bool type_is_numeric(Type *a) {
    return type_is_integer(a) || type_is_float(a);
}

int type_width(Type *a) {
    a = type_unwrap_distinct(a);
    switch (a->kind) {
    case TYPE_U8:  case TYPE_I8:  case TYPE_BOOL: return 8;
    case TYPE_U16: case TYPE_I16: return 16;
    case TYPE_U32: case TYPE_I32: case TYPE_F32: case TYPE_ENUM: return 32;
    case TYPE_U64: case TYPE_I64: case TYPE_F64: case TYPE_HANDLE: return 64;
    case TYPE_USIZE: return zer_target_ptr_bits; /* matches target, not host */
    default: return 0;
    }
}

bool type_is_optional(Type *a) {
    /* BUG-409: unwrap distinct — distinct typedef ?*T is still optional */
    a = type_unwrap_distinct(a);
    return a->kind == TYPE_OPTIONAL;
}

Type *type_unwrap_optional(Type *a) {
    /* BUG-409: unwrap distinct first */
    a = type_unwrap_distinct(a);
    if (a->kind == TYPE_OPTIONAL) return a->optional.inner;
    return a; /* not optional — return as-is */
}

/* ================================================================
 * Type equality — nominal for structs/enums/unions, structural for rest
 * ================================================================ */

bool type_equals(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
    /* primitives: kind match is sufficient */
    case TYPE_VOID: case TYPE_BOOL:
    case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_USIZE:
    case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
    case TYPE_F32: case TYPE_F64:
    case TYPE_OPAQUE: case TYPE_ARENA:
        return true;

    /* pointer/optional/slice: recurse on inner.
     * const-aware: *const T != *T. This makes const checking work
     * recursively through any depth of pointers/slices. The safe
     * direction (mutable→const) is handled by can_implicit_coerce. */
    case TYPE_POINTER:
        if (a->pointer.is_const != b->pointer.is_const) return false;
        return type_equals(a->pointer.inner, b->pointer.inner);
    case TYPE_OPTIONAL:
        return type_equals(a->optional.inner, b->optional.inner);
    case TYPE_SLICE:
        if (a->slice.is_const != b->slice.is_const) return false;
        if (a->slice.is_volatile != b->slice.is_volatile) return false;
        return type_equals(a->slice.inner, b->slice.inner);

    /* array: inner + size */
    case TYPE_ARRAY:
        return a->array.size == b->array.size &&
               type_equals(a->array.inner, b->array.inner);

    /* struct/enum/union: nominal — pointer identity (same definition = same pointer) */
    case TYPE_STRUCT:
        return a == b; /* same definition */
    case TYPE_ENUM:
        return a == b;
    case TYPE_UNION:
        return a == b;

    /* func_ptr: compare return + all params */
    case TYPE_FUNC_PTR:
        if (a->func_ptr.param_count != b->func_ptr.param_count) return false;
        if (!type_equals(a->func_ptr.ret, b->func_ptr.ret)) return false;
        for (uint32_t i = 0; i < a->func_ptr.param_count; i++) {
            if (!type_equals(a->func_ptr.params[i], b->func_ptr.params[i]))
                return false;
            /* keep mismatch: if source has keep, target must too */
            bool a_keep = a->func_ptr.param_keeps && a->func_ptr.param_keeps[i];
            bool b_keep = b->func_ptr.param_keeps && b->func_ptr.param_keeps[i];
            if (a_keep != b_keep) return false;
        }
        return true;

    /* pool/ring: elem + count */
    case TYPE_POOL:
        return a->pool.count == b->pool.count &&
               type_equals(a->pool.elem, b->pool.elem);
    case TYPE_RING:
        return a->ring.count == b->ring.count &&
               type_equals(a->ring.elem, b->ring.elem);

    /* handle: elem */
    case TYPE_HANDLE:
        return type_equals(a->handle.elem, b->handle.elem);

    /* slab: elem */
    case TYPE_SLAB:
        return type_equals(a->slab.elem, b->slab.elem);

    /* distinct: nominal — pointer identity (same definition = same type) */
    case TYPE_DISTINCT:
        return a == b;
    }
    return false;
}

/* ================================================================
 * Coercion rules — from ZER-LANG.md Section 5
 *
 * Implicit coercion allowed:
 *   - Integer widening (same sign, smaller → larger)
 *   - Unsigned to larger signed (u8 → i16, u16 → i32, etc.)
 *   - Float widening (f32 → f64)
 *   - Array to slice (T[N] → []T)
 *   - Mutable to const ([]T → const []T, *T → const *T)
 *
 * NOT allowed implicitly:
 *   - Narrowing (u32 → u16) — must @truncate or @saturate
 *   - Signed ↔ unsigned same width — must @bitcast
 *   - Integer ↔ float — not allowed
 * ================================================================ */

bool can_implicit_coerce(Type *from, Type *to) {
    if (type_equals(from, to)) return true;

    /* integer widening: same sign, from.width < to.width */
    if (type_is_integer(from) && type_is_integer(to)) {
        bool from_signed = type_is_signed(from);
        bool to_signed = type_is_signed(to);
        int from_w = type_width(from);
        int to_w = type_width(to);

        /* same sign, smaller → larger */
        if (from_signed == to_signed && from_w < to_w) return true;
        /* same sign, same width: allow u32 → usize on 32-bit targets */
        if (from_signed == to_signed && from_w == to_w &&
            (from->kind == TYPE_USIZE || to->kind == TYPE_USIZE)) return true;

        /* unsigned to larger signed (u8 → i16, u16 → i32, etc.) */
        if (!from_signed && to_signed && from_w < to_w) return true;

        return false;
    }

    /* float widening: f32 → f64 */
    if (from->kind == TYPE_F32 && to->kind == TYPE_F64) return true;

    /* NOTE: f64 → f32 is NOT implicit. Must use explicit conversion.
     * But float LITERALS are handled specially in the checker (not here). */

    /* T → ?T: value to optional (implicit wrap)
     * BUG-409: unwrap distinct — distinct typedef ?T still accepts T */
    {
        Type *to_eff = type_unwrap_distinct(to);
        if (to_eff->kind == TYPE_OPTIONAL) {
            return type_equals(from, to_eff->optional.inner) ||
                   can_implicit_coerce(from, to_eff->optional.inner);
        }
    }

    /* array to slice: T[N] → []T */
    if (from->kind == TYPE_ARRAY && to->kind == TYPE_SLICE) {
        return type_equals(from->array.inner, to->slice.inner);
    }

    /* slice qualifier widening: mutable→const, non-volatile→volatile */
    if (from->kind == TYPE_SLICE && to->kind == TYPE_SLICE) {
        /* cannot strip volatile or const */
        if (from->slice.is_volatile && !to->slice.is_volatile) return false;
        if (from->slice.is_const && !to->slice.is_const) return false;
        /* allow widening (adding const or volatile) */
        if ((to->slice.is_const >= from->slice.is_const) &&
            (to->slice.is_volatile >= from->slice.is_volatile) &&
            type_equals(from->slice.inner, to->slice.inner)) {
            return true;
        }
    }

    /* mutable pointer to const pointer */
    if (from->kind == TYPE_POINTER && to->kind == TYPE_POINTER) {
        if (to->pointer.is_const && !from->pointer.is_const) {
            return type_equals(from->pointer.inner, to->pointer.inner);
        }
    }

    /* slice to pointer: []T → *T — REMOVED (BUG-162)
     * An empty slice has ptr=NULL, violating *T non-null guarantee.
     * Use .ptr explicitly if you need a pointer from a slice. */

    return false;
}

/* ================================================================
 * Type name for error messages
 * ================================================================ */

/* two alternating buffers so type_name(a), type_name(b) in one printf works */
static char type_name_buf0[256];
static char type_name_buf1[256];
static int type_name_which = 0;

static int type_name_write(Type *t, char *buf, int pos, int max) {
    if (!t || pos >= max - 1) return pos;

    switch (t->kind) {
    case TYPE_VOID:   return pos + snprintf(buf + pos, max - pos, "void");
    case TYPE_BOOL:   return pos + snprintf(buf + pos, max - pos, "bool");
    case TYPE_U8:     return pos + snprintf(buf + pos, max - pos, "u8");
    case TYPE_U16:    return pos + snprintf(buf + pos, max - pos, "u16");
    case TYPE_U32:    return pos + snprintf(buf + pos, max - pos, "u32");
    case TYPE_U64:    return pos + snprintf(buf + pos, max - pos, "u64");
    case TYPE_USIZE:  return pos + snprintf(buf + pos, max - pos, "usize");
    case TYPE_I8:     return pos + snprintf(buf + pos, max - pos, "i8");
    case TYPE_I16:    return pos + snprintf(buf + pos, max - pos, "i16");
    case TYPE_I32:    return pos + snprintf(buf + pos, max - pos, "i32");
    case TYPE_I64:    return pos + snprintf(buf + pos, max - pos, "i64");
    case TYPE_F32:    return pos + snprintf(buf + pos, max - pos, "f32");
    case TYPE_F64:    return pos + snprintf(buf + pos, max - pos, "f64");
    case TYPE_OPAQUE: return pos + snprintf(buf + pos, max - pos, "opaque");
    case TYPE_ARENA:  return pos + snprintf(buf + pos, max - pos, "Arena");
    case TYPE_POINTER:
        pos += snprintf(buf + pos, max - pos, "*");
        return type_name_write(t->pointer.inner, buf, pos, max);
    case TYPE_OPTIONAL:
        pos += snprintf(buf + pos, max - pos, "?");
        return type_name_write(t->optional.inner, buf, pos, max);
    case TYPE_SLICE:
        if (t->slice.is_volatile) pos += snprintf(buf + pos, max - pos, "volatile ");
        pos += snprintf(buf + pos, max - pos, "[]");
        return type_name_write(t->slice.inner, buf, pos, max);
    case TYPE_ARRAY:
        pos = type_name_write(t->array.inner, buf, pos, max);
        return pos + snprintf(buf + pos, max - pos, "[%llu]", (unsigned long long)t->array.size);
    case TYPE_STRUCT:
        return pos + snprintf(buf + pos, max - pos, "%.*s",
                              (int)t->struct_type.name_len, t->struct_type.name);
    case TYPE_ENUM:
        return pos + snprintf(buf + pos, max - pos, "%.*s",
                              (int)t->enum_type.name_len, t->enum_type.name);
    case TYPE_UNION:
        return pos + snprintf(buf + pos, max - pos, "%.*s",
                              (int)t->union_type.name_len, t->union_type.name);
    case TYPE_FUNC_PTR:
        return pos + snprintf(buf + pos, max - pos, "fn(...)");
    case TYPE_POOL:
        pos += snprintf(buf + pos, max - pos, "Pool(");
        pos = type_name_write(t->pool.elem, buf, pos, max);
        return pos + snprintf(buf + pos, max - pos, ", %llu)", (unsigned long long)t->pool.count);
    case TYPE_RING:
        pos += snprintf(buf + pos, max - pos, "Ring(");
        pos = type_name_write(t->ring.elem, buf, pos, max);
        return pos + snprintf(buf + pos, max - pos, ", %llu)", (unsigned long long)t->ring.count);
    case TYPE_HANDLE:
        pos += snprintf(buf + pos, max - pos, "Handle(");
        pos = type_name_write(t->handle.elem, buf, pos, max);
        return pos + snprintf(buf + pos, max - pos, ")");
    case TYPE_SLAB:
        pos += snprintf(buf + pos, max - pos, "Slab(");
        pos = type_name_write(t->slab.elem, buf, pos, max);
        return pos + snprintf(buf + pos, max - pos, ")");
    case TYPE_DISTINCT:
        return pos + snprintf(buf + pos, max - pos, "%.*s",
                              (int)t->distinct.name_len, t->distinct.name);
    }
    return pos;
}

const char *type_name(Type *t) {
    char *buf = (type_name_which++ & 1) ? type_name_buf1 : type_name_buf0;
    buf[0] = '\0';
    type_name_write(t, buf, 0, 256);
    return buf;
}

void type_print(Type *t) {
    printf("%s", type_name(t));
}

/* ================================================================
 * Scope operations
 * ================================================================ */

Scope *scope_new(Arena *a, Scope *parent) {
    Scope *s = (Scope *)arena_alloc(a, sizeof(Scope));
    s->parent = parent;
    s->symbol_count = 0;
    s->symbol_capacity = 16;
    s->symbols = (Symbol *)arena_alloc(a, s->symbol_capacity * sizeof(Symbol));
    return s;
}

Symbol *scope_add(Arena *a, Scope *s, const char *name, uint32_t name_len,
                  Type *type, uint32_t line, const char *file) {
    /* check for redefinition in current scope */
    Symbol *existing = scope_lookup_local(s, name, name_len);
    if (existing) return NULL; /* caller handles error */

    /* grow if needed */
    if (s->symbol_count >= s->symbol_capacity) {
        uint32_t new_cap = s->symbol_capacity * 2;
        Symbol *new_syms = (Symbol *)arena_alloc(a, new_cap * sizeof(Symbol));
        memcpy(new_syms, s->symbols, s->symbol_count * sizeof(Symbol));
        s->symbols = new_syms;
        s->symbol_capacity = new_cap;
    }

    Symbol *sym = &s->symbols[s->symbol_count++];
    memset(sym, 0, sizeof(Symbol));
    sym->name = name;
    sym->name_len = name_len;
    sym->type = type;
    sym->line = line;
    sym->file = file;
    return sym;
}

Symbol *scope_lookup_local(Scope *s, const char *name, uint32_t name_len) {
    for (uint32_t i = 0; i < s->symbol_count; i++) {
        if (s->symbols[i].name_len == name_len &&
            memcmp(s->symbols[i].name, name, name_len) == 0) {
            return &s->symbols[i];
        }
    }
    return NULL;
}

Symbol *scope_lookup(Scope *s, const char *name, uint32_t name_len) {
    Scope *cur = s;
    while (cur) {
        Symbol *sym = scope_lookup_local(cur, name, name_len);
        if (sym) return sym;
        cur = cur->parent;
    }
    return NULL;
}
