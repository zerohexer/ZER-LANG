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
    TYPE_SLAB,          /* Slab(T) — dynamic growable pool, elem type */

    /* distinct typedef */
    TYPE_DISTINCT,      /* distinct typedef — nominal wrapper around underlying type */
} TypeKind;

/* ---- Struct field (semantic) ---- */
typedef struct {
    const char *name;
    uint32_t name_len;
    Type *type;
    bool is_keep;
    bool is_volatile;   /* BUG-414: volatile struct field (for array fields that lack Type-level flag) */
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
        struct { Type *inner; bool is_const; bool is_volatile; } slice;

        /* TYPE_ARRAY */
        struct { Type *inner; uint64_t size; Type *sizeof_type; /* non-NULL → emit sizeof(T) instead of numeric size */ } array;

        /* TYPE_STRUCT */
        struct {
            SField *fields;
            uint32_t field_count;
            const char *name;
            uint32_t name_len;
            bool is_packed;
            bool is_shared;
            const char *module_prefix;  /* NULL for main module */
            uint32_t module_prefix_len;
            uint32_t type_id;           /* BUG-393: runtime provenance tag */
        } struct_type;

        /* TYPE_ENUM */
        struct {
            SEVariant *variants;
            uint32_t variant_count;
            const char *name;
            uint32_t name_len;
            const char *module_prefix;
            uint32_t module_prefix_len;
            uint32_t type_id;           /* BUG-393: runtime provenance tag */
        } enum_type;

        /* TYPE_UNION */
        struct {
            SUVariant *variants;
            uint32_t variant_count;
            const char *name;
            uint32_t name_len;
            const char *module_prefix;
            uint32_t module_prefix_len;
            uint32_t type_id;           /* BUG-393: runtime provenance tag */
        } union_type;

        /* TYPE_FUNC_PTR */
        struct {
            Type **params;
            uint32_t param_count;
            Type *ret;
            bool *param_keeps;  /* per-param keep flags (NULL if none) */
        } func_ptr;

        /* TYPE_POOL */
        struct { Type *elem; uint64_t count; } pool;

        /* TYPE_RING */
        struct { Type *elem; uint64_t count; } ring;

        /* TYPE_HANDLE */
        struct { Type *elem; } handle;

        /* TYPE_SLAB */
        struct { Type *elem; } slab;

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
/* BUG-295: unwrap ALL levels of distinct, not just one */
static inline Type *type_unwrap_distinct(Type *t) {
    while (t && t->kind == TYPE_DISTINCT) t = t->distinct.underlying;
    return t;
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
    bool is_volatile;       /* volatile qualifier — &volatile_var yields volatile pointer */
    bool is_static;         /* static storage duration */
    bool is_arena_derived;  /* pointer from LOCAL arena.alloc() — cannot escape to global/static or return */
    bool is_local_derived;  /* pointer to local variable — cannot be returned */
    bool is_from_arena;     /* pointer from ANY arena (global or local) — cannot be stored in globals */

    /* @ptrcast provenance: compile-time check for simple variables (belt),
     * runtime type_id in _zer_opaque for complex paths (suspenders). BUG-393. */
    Type *provenance_type;  /* NULL = unknown origin (params, cinclude) */

    /* @container provenance: tracks which struct+field this pointer points inside */
    Type *container_struct;          /* NULL = unknown */
    const char *container_field;
    uint32_t container_field_len;

    /* for functions */
    bool is_function;
    bool is_comptime;       /* comptime function — evaluated at compile time */
    Node *func_node;        /* AST node for function body, if applicable */
    bool returns_color_cached;  /* zercheck: return color already computed */
    int returns_color_value;    /* zercheck: cached ZC_COLOR_* for return value */
    int returns_param_color;    /* zercheck: -1 = N/A, 0+ = return inherits param[N]'s color */

    /* Handle auto-deref: which Slab/Pool this handle was allocated from */
    Symbol *slab_source;        /* NULL = unknown (parameter, conditional) */

    /* MMIO pointer bound: derived from mmio range for @inttoptr pointers */
    uint64_t mmio_bound;        /* max valid index (0 = no bound) */

    /* cross-function range summary: return value range for simple functions */
    int64_t return_range_min;
    int64_t return_range_max;
    bool has_return_range;

    /* module prefix for name mangling (NULL = main module) */
    const char *module_prefix;
    uint32_t module_prefix_len;

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
 * Target configuration — set by checker, read by type_width
 * ================================================================ */
extern int zer_target_ptr_bits; /* default 32, set via --target-bits */

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
Type *type_volatile_slice(Arena *a, Type *inner);
Type *type_array(Arena *a, Type *inner, uint64_t size);
Type *type_pool(Arena *a, Type *elem, uint64_t count);
Type *type_ring(Arena *a, Type *elem, uint64_t count);
Type *type_handle(Arena *a, Type *elem);
Type *type_slab(Arena *a, Type *elem);
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
