#ifndef ZER_TYPES_H
#define ZER_TYPES_H

#include "ast.h"
#include <stdbool.h>
#include <stdint.h>

/* ================================================================
 * ZER Semantic Types
 *
 * These are RESOLVED types — what the type checker produces.
 * TypeNode (from ast.h) is what the programmer wrote (syntactic).
 * Type (this file) is what it means (semantic).
 *
 * Recursive tree structure, arena-allocated. Nominal equality.
 * Direct translation of zer-type-system.md Decision 1.
 * ================================================================ */

/* ---- Forward declarations ---- */
typedef struct Type Type;
typedef struct Symbol Symbol;
typedef struct Scope Scope;

/* ---- Type kinds ---- */
typedef enum {
    /* primitives */
    TYPE_VOID, TYPE_BOOL,
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_U64, TYPE_USIZE,
    TYPE_I8, TYPE_I16, TYPE_I32, TYPE_I64,
    TYPE_F32, TYPE_F64,

    /* compound */
    TYPE_POINTER,       /* *T — inner = pointed-to type */
    TYPE_OPTIONAL,      /* ?T — inner = wrapped type */
    TYPE_SLICE,         /* []T — inner = element type */
    TYPE_ARRAY,         /* T[N] — inner = element type + size */
    TYPE_STRUCT,        /* struct { fields } */
    TYPE_ENUM,          /* enum { variants } */
    TYPE_UNION,         /* tagged union { variants } */
    TYPE_FUNC_PTR,      /* fn pointer — params + return type */
    TYPE_OPAQUE,        /* *opaque — no inner type */

    /* builtins */
    TYPE_POOL,          /* Pool(T, N) — elem type + count */
    TYPE_RING,          /* Ring(T, N) — elem type + count */
    TYPE_ARENA,         /* Arena — no parameters */
    TYPE_HANDLE,        /* Handle(T) — elem type */

    /* distinct typedef */
    TYPE_DISTINCT,      /* distinct typedef — nominal wrapper around underlying type */
} TypeKind;

/* ---- Struct field (semantic) ---- */
typedef struct {
    const char *name;
    uint32_t name_len;
    Type *type;
    bool is_keep;
} SField;

/* ---- Enum variant (semantic) ---- */
typedef struct {
    const char *name;
    uint32_t name_len;
    int32_t value;          /* explicit or auto-assigned */
} SEVariant;

/* ---- Union variant (semantic) ---- */
typedef struct {
    const char *name;
    uint32_t name_len;
    Type *type;             /* payload type */
} SUVariant;

/* ---- The Type struct ---- */
struct Type {
    TypeKind kind;

    union {
        /* TYPE_POINTER */
        struct { Type *inner; bool is_const; bool is_volatile; } pointer;

        /* TYPE_OPTIONAL */
        struct { Type *inner; } optional;

        /* TYPE_SLICE */
        struct { Type *inner; bool is_const; } slice;

        /* TYPE_ARRAY */
        struct { Type *inner; uint64_t size; } array;

        /* TYPE_STRUCT */
        struct {
            SField *fields;
            uint32_t field_count;
            const char *name;
            uint32_t name_len;
            bool is_packed;
            const char *module_prefix;  /* NULL for main module */
            uint32_t module_prefix_len;
        } struct_type;

        /* TYPE_ENUM */
        struct {
            SEVariant *variants;
            uint32_t variant_count;
            const char *name;
            uint32_t name_len;
            const char *module_prefix;
            uint32_t module_prefix_len;
        } enum_type;

        /* TYPE_UNION */
        struct {
            SUVariant *variants;
            uint32_t variant_count;
            const char *name;
            uint32_t name_len;
            const char *module_prefix;
            uint32_t module_prefix_len;
        } union_type;

        /* TYPE_FUNC_PTR */
        struct {
            Type **params;
            uint32_t param_count;
            Type *ret;
        } func_ptr;

        /* TYPE_POOL */
        struct { Type *elem; uint64_t count; } pool;

        /* TYPE_RING */
        struct { Type *elem; uint64_t count; } ring;

        /* TYPE_HANDLE */
        struct { Type *elem; } handle;

        /* TYPE_DISTINCT */
        struct {
            Type *underlying;
            const char *name;
            uint32_t name_len;
        } distinct;
    };

    /* source location for error messages */
    const char *defined_in_file;
    uint32_t defined_at_line;
};

/* Unwrap TYPE_DISTINCT to get the underlying concrete type.
 * Use this before any switch on type->kind to prevent distinct types
 * from falling through to default/anonymous-struct paths.
 * Safe to call on any type — returns the type unchanged if not distinct. */
static inline Type *type_unwrap_distinct(Type *t) {
    return (t && t->kind == TYPE_DISTINCT) ? t->distinct.underlying : t;
}

/* ================================================================
 * Symbol Table — Scope Chain (Decision 2)
 * ================================================================ */

struct Symbol {
    const char *name;
    uint32_t name_len;
    Type *type;

    bool is_keep;           /* keep parameter — can be stored */
    bool is_const;          /* const qualifier */
    bool is_static;         /* static storage duration */
    bool is_arena_derived;  /* pointer from arena.alloc() — cannot escape to global/static */
    bool is_local_derived;  /* pointer to local variable — cannot be returned */

    /* for functions */
    bool is_function;
    Node *func_node;        /* AST node for function body, if applicable */

    /* source location */
    const char *file;
    uint32_t line;
};

struct Scope {
    Scope *parent;          /* enclosing scope (NULL for module level) */
    Symbol *symbols;        /* dynamic array */
    uint32_t symbol_count;
    uint32_t symbol_capacity;
    const char *module_name; /* non-NULL for module-level scopes */
};

/* ================================================================
 * Global type singletons — primitives allocated once
 * ================================================================ */

extern Type *ty_void;
extern Type *ty_bool;
extern Type *ty_u8;
extern Type *ty_u16;
extern Type *ty_u32;
extern Type *ty_u64;
extern Type *ty_usize;
extern Type *ty_i8;
extern Type *ty_i16;
extern Type *ty_i32;
extern Type *ty_i64;
extern Type *ty_f32;
extern Type *ty_f64;
extern Type *ty_opaque;
extern Type *ty_arena;

/* ================================================================
 * Type API
 * ================================================================ */

/* initialize global type singletons */
void types_init(Arena *arena);

/* constructors — allocate from arena */
Type *type_pointer(Arena *a, Type *inner);
Type *type_const_pointer(Arena *a, Type *inner);
Type *type_optional(Arena *a, Type *inner);
Type *type_slice(Arena *a, Type *inner);
Type *type_const_slice(Arena *a, Type *inner);
Type *type_array(Arena *a, Type *inner, uint64_t size);
Type *type_pool(Arena *a, Type *elem, uint64_t count);
Type *type_ring(Arena *a, Type *elem, uint64_t count);
Type *type_handle(Arena *a, Type *elem);
Type *type_func_ptr(Arena *a, Type **params, uint32_t param_count, Type *ret);

/* queries */
bool type_equals(Type *a, Type *b);
bool type_is_integer(Type *a);
bool type_is_signed(Type *a);
bool type_is_unsigned(Type *a);
bool type_is_float(Type *a);
bool type_is_numeric(Type *a);
int  type_width(Type *a);          /* bit width: 8, 16, 32, 64 */
bool type_is_optional(Type *a);
Type *type_unwrap_optional(Type *a); /* ?T → T */

/* coercion: can 'from' be implicitly converted to 'to'? */
bool can_implicit_coerce(Type *from, Type *to);

/* name for error messages */
const char *type_name(Type *t);
void type_print(Type *t);

/* ================================================================
 * Scope API
 * ================================================================ */

Scope *scope_new(Arena *a, Scope *parent);
Symbol *scope_add(Arena *a, Scope *s, const char *name, uint32_t name_len,
                  Type *type, uint32_t line, const char *file);
Symbol *scope_lookup(Scope *s, const char *name, uint32_t name_len);
Symbol *scope_lookup_local(Scope *s, const char *name, uint32_t name_len);

#endif /* ZER_TYPES_H */
