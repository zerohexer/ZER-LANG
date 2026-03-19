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
 *   Handle(T)          → uint32_t (gen << 16 | index)
 * ================================================================ */

/* deferred statement stack — grows dynamically */
typedef struct {
    Node **stmts;
    int count;
    int capacity;
} DeferStack;

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
} Emitter;

/* ---- API ---- */
void emitter_init(Emitter *e, FILE *out, Arena *arena, Checker *checker);
void emit_file(Emitter *e, Node *file_node);
void emit_file_no_preamble(Emitter *e, Node *file_node); /* for imported modules */

#endif /* ZER_EMITTER_H */
