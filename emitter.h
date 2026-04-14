#ifndef ZER_EMITTER_H
#define ZER_EMITTER_H

#include "ast.h"
#include "types.h"
#include "checker.h"
#include <stdio.h>

/* ================================================================
 * ZER C Emitter
 *
 * Walks the typed AST and outputs valid C code.
 * The output compiles with GCC (C99 or later).
 *
 * ZER type → C type mapping:
 *   u8/u16/u32/u64    → uint8_t/uint16_t/uint32_t/uint64_t
 *   i8/i16/i32/i64    → int8_t/int16_t/int32_t/int64_t
 *   usize             → size_t
 *   f32/f64            → float/double
 *   bool               → _Bool (or uint8_t)
 *   void               → void
 *   *T                 → T*
 *   ?T                 → struct { T value; uint8_t has_value; }
 *   ?*T                → T* (null sentinel)
 *   []T                → struct { T *ptr; size_t len; }
 *   Pool(T,N)          → struct with slots, gen counters, used flags
 *   Handle(T)          → uint64_t (gen(32) << 32 | index(32))
 * ================================================================ */

/* deferred statement stack — grows dynamically */
typedef struct {
    Node **stmts;
    int count;
    int capacity;
} DeferStack;

/* spawn wrapper — deferred file-scope wrapper function for pthread_create */
typedef struct {
    int id;                 /* spawn_id for unique naming */
    Node *spawn_node;       /* the NODE_SPAWN for type info */
} SpawnWrapper;

typedef struct {
    FILE *out;              /* output file */
    Arena *arena;           /* for temporary allocations */
    Checker *checker;       /* for resolved type info */
    int indent;             /* current indentation level */
    int temp_count;         /* counter for temporary variable names */
    Type *current_func_ret; /* return type of current function */
    DeferStack defer_stack; /* current block's deferred statements */
    int loop_defer_base;    /* defer stack base at loop entry (for break/continue) */
    bool lib_mode;          /* --lib: no prefix on struct names, no preamble */
    bool track_cptrs;       /* --track-cptrs: Level 3+4+5 inline header tracking */
    const char *source_file; /* .zer source file name for #line directives */
    const char *current_module; /* module name for function/global mangling (NULL = main) */
    uint32_t current_module_len;

    /* spawn wrappers — collected during pass 1 scan, emitted before functions */
    SpawnWrapper *spawn_wrappers;
    int spawn_wrapper_count;
    int spawn_wrapper_capacity;
    int next_spawn_id;      /* counter for unique spawn wrapper IDs */

    /* async function emission state */
    bool in_async;              /* true when emitting inside an async function body */
    int async_yield_id;         /* counter for yield/await state IDs */
    const char **async_locals;  /* local variable names promoted to state struct */
    size_t *async_local_lens;   /* lengths of local variable names */
    int async_local_count;
    int async_local_capacity;

    /* async orelse/capture temps promoted to state struct (BUG-481 proper fix).
     * Pre-scanned from async body — compiler temps that might straddle yield. */
    struct AsyncTemp {
        Type *type;             /* the optional type (e.g., _zer_opt_u32) */
        int temp_id;            /* maps to _zer_async_tmp<id> in state struct */
    } *async_temps;
    int async_temp_count;
    int async_temp_capacity;
    int async_temp_next_id;

    /* condvar types — shared structs that use @cond_wait/@cond_signal.
     * These need pthread_mutex_t instead of spinlock. Tracked by type_id. */
    uint32_t *condvar_type_ids;
    int condvar_type_count;
    int condvar_type_capacity;
} Emitter;

/* ---- API ---- */
void emitter_init(Emitter *e, FILE *out, Arena *arena, Checker *checker);
void emit_file_module(Emitter *e, Node *file_node, bool with_preamble); /* unified */
void emit_file(Emitter *e, Node *file_node);              /* backward compat: with preamble */
void emit_file_no_preamble(Emitter *e, Node *file_node);  /* backward compat: without preamble */

/* IR-based emission (Phase 5) — emit one function from IR representation.
 * Include ir.h before calling this. Uses IRFunc typedef from ir.h. */
void emit_func_from_ir(Emitter *e, void *ir_func);

#endif /* ZER_EMITTER_H */
