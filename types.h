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
    TYPE_BARRIER,       /* Barrier — thread sync point, no parameters */
    TYPE_HANDLE,        /* Handle(T) — elem type */
    TYPE_SLAB,          /* Slab(T) — dynamic growable pool, elem type */
    TYPE_SEMAPHORE,     /* Semaphore(N) — counting semaphore, count */

    /* distinct typedef */
    TYPE_DISTINCT,      /* distinct typedef — nominal wrapper around underlying type */

    /* arbitrary-width integers (Path C: width-field integer representation).
     * APPENDED at the end so ZER_TK_* constants + verif_type_kind.v proofs
     * (which hardcode enum order) stay valid — never reorder the values above.
     * Signedness lives in the KIND (UINT vs SINT) so the kind-based verified
     * predicates remain meaningful; width lives in the .intn.bits field. */
    TYPE_UINT,          /* uN — unsigned, .intn.bits in 1..128 */
    TYPE_SINT,          /* iN — signed,   .intn.bits in 1..128 */
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

        /* TYPE_UINT / TYPE_SINT — arbitrary-width integer, width in bits (1..128).
         * Carrier chosen at emission by bit count (≤8 uint8_t … ≤128 __int128). */
        struct { uint32_t bits; } intn;

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
            bool is_shared_rw;      /* shared(rw) — reader-writer lock */
            bool is_move;           /* move struct — ownership transfer on pass/assign */
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
            bool is_variadic;   /* trailing ... (C-interop extern decls only) */
        } func_ptr;

        /* TYPE_POOL */
        struct { Type *elem; uint64_t count; } pool;

        /* TYPE_RING */
        struct { Type *elem; uint64_t count; } ring;

        /* TYPE_HANDLE */
        struct { Type *elem; } handle;

        /* TYPE_SLAB */
        struct { Type *elem; } slab;

        /* TYPE_SEMAPHORE */
        struct { uint32_t count; } semaphore;

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

/* Canonical dispatch-kind accessor (structural distinct-unwrap kill, 2026-06-07).
 *
 * Use this INSTEAD of `t->kind` whenever you dispatch a SAFETY decision on
 * the RESULT of checker_get_type()/check_expr()/resolve_type() — i.e. any
 * `result->kind == TYPE_X` where a missed distinct-unwrap would let a
 * `distinct typedef` slip past a safety check (the GAP-F / BUG-409 class,
 * the #1 historical bug class in this compiler). Unwraps ALL levels of
 * TYPE_DISTINCT and is NULL-safe.
 *
 * NULL → TYPE_VOID: callers checking `== TYPE_POINTER/SLICE/STRUCT/...`
 * correctly get a false match on NULL (matches the old `t && t->kind == X`
 * idiom). A site that genuinely dispatches on `== TYPE_VOID` must still
 * NULL-guard separately — but those read already-resolved `.inner` types,
 * not user-type-arg results, so they are NOT in this accessor's domain.
 *
 * Do NOT use for reads of already-resolved inner types
 * (`.pointer.inner->kind`, `.optional.inner->kind`, `.array.inner->kind`,
 * `.slice.inner->kind`) — those are post-resolution and rarely distinct;
 * converting them is churn. This accessor is for the top-level dispatch on
 * a freshly-obtained Type*. */
static inline TypeKind type_dispatch_kind(Type *t) {
    t = type_unwrap_distinct(t);
    return t ? t->kind : TYPE_VOID;
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
    bool is_nonkeep_derived; /* pointer traces to a non-keep param — cannot be persisted (keep axis) */
    int nonkeep_root_param;  /* keep inference: index of the param this pointer traces to (valid only when is_nonkeep_derived) */
    bool is_keep_derived;   /* pointer traces to a KEEP param — a borrow; storing into a struct field requires a 'keep' field (field-level keep, Rust &'a analog) */
    bool is_thread_handle;  /* ThreadHandle from scoped spawn — must call .join() */
    /* Scoped-borrow exclusivity (Axis C, 2026-06-21): a non-shared local
     * borrowed by a scoped `spawn worker(&x)` is exclusively lent to the
     * thread until `th.join()`. A parent WRITE to it in that window is a data
     * race (Rust's `&mut` scoped-thread rule). `is_borrowed_by_thread` is set
     * on the borrowed local at the spawn and cleared at the join;
     * `th_borrows_name` records, on the ThreadHandle symbol, which local it
     * borrowed so join can clear it. Linear (statement-order) approximation —
     * sound for the straight-line spawn→write→join pattern; conservative for
     * branches. */
    bool is_borrowed_by_thread;
    const char *th_borrows_name;
    uint32_t th_borrows_name_len;
    /* A6-full atomic-cell inclusion (2026-06-21): set on a scalar GLOBAL the
     * first time it is the target of an `@atomic_*`. Strict (Rust) model: once a
     * location is atomic, ALL access must be atomic — a plain access anywhere
     * (incl. init) is a data race (mixed atomic/non-atomic). The "inclusion"
     * model that replaces the exclusion-list: instead of listing what's safe,
     * mark what's shared and require synchronized access. */
    bool is_atomic_cell;

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
    bool is_async;          /* async coroutine — calls emitted as init/poll pair, not direct */
    Node *func_node;        /* AST node for function body, if applicable */

    /* Function summaries: computed properties of function bodies (lazy, cached).
     * Used for context safety — @critical/defer/interrupt check callee properties.
     * See docs/FunctionSummaries.md for full design. */
    struct {
        bool computed;       /* true = cached, don't re-scan */
        bool in_progress;    /* true = DFS in progress (cycle detection) */
        bool can_yield;      /* body contains yield/await (directly or transitively) */
        bool can_spawn;      /* body contains spawn (directly or transitively) */
        bool can_alloc;      /* body contains slab.alloc/Task.new (directly or transitively) */
        bool has_sync;       /* body contains @atomic_* or @barrier (absorbs has_atomic_or_barrier) */
        /* Direct-only effect flags — set when the effect appears literally
         * in this function's immediate body (NOT through a callee). Used by
         * check_body_effects to suppress duplicate body-level errors when
         * per-site checks (NODE_SPAWN, slab.alloc, NODE_YIELD inside
         * @critical via the matching context flag) already fired. Without
         * this split, a direct `interrupt USART1 { slab.alloc(); }`
         * produces TWO errors for the same root cause. */
        bool has_direct_yield;
        bool has_direct_spawn;
        bool has_direct_alloc;
    } props;
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

    /* cross-function escape summary (Stage 1->2, 2026-06-22): the per-function
     * return provenance — the ARStatic/ARParam(n) lattice of
     * lambda_zer_escape/param_lattice.v. `ret_summary_complete` = every valued
     * return is classifiable (rooted at a global/static or `null` = STATIC, or a
     * view of a parameter = ARParam(n)); false if any return is unprovable, in
     * which case the result is never treated as static (conservative).
     * `ret_param_mask` = bit n set iff some return path may return a view of
     * parameter n. The call RESULT is static-escapable iff `ret_summary_complete`
     * AND every masked param's actual argument is itself static (the call-site
     * substitution `resolve(R_f, argreg)`). Stage 1's "returns_static" is the
     * special case `ret_summary_complete && ret_param_mask == 0`. Defaults
     * {false, 0} → the taint stays unless proven (no under-rejection, T4). */
    bool ret_summary_complete;
    uint64_t ret_param_mask;

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
extern Type *ty_barrier;

/* ================================================================
 * Type API
 * ================================================================ */

/* initialize global type singletons */
void types_init(Arena *arena);

/* constructors — allocate from arena */
Type *type_uint(Arena *a, uint32_t bits);   /* Path C: arbitrary-width unsigned int */
Type *type_sint(Arena *a, uint32_t bits);   /* Path C: arbitrary-width signed int */
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
Type *type_semaphore(Arena *a, uint32_t count);
Type *type_func_ptr(Arena *a, Type **params, uint32_t param_count, Type *ret);

/* queries */
bool type_equals(Type *a, Type *b);
bool type_is_integer(Type *a);
bool type_is_signed(Type *a);
bool type_is_unsigned(Type *a);
bool type_is_float(Type *a);
bool type_is_numeric(Type *a);
int  type_width(Type *a);          /* bit width: 8, 16, 32, 64 */
int  type_alignment_bytes(Type *a); /* required alignment in bytes; recurses
                                      * through aggregates for compound MMIO
                                      * targets; returns 0 if not computable */
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
