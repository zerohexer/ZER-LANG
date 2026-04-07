#ifndef ZER_CHECK_H
#define ZER_CHECK_H

#include "ast.h"
#include "types.h"
#include "checker.h"

/* ================================================================
 * ZER-CHECK — Path-Sensitive Handle Verification
 *
 * Runs after type checker, before emitter. Read-only pass.
 * Catches handle bugs that escape compile-time type checking:
 *   - Handle used after free
 *   - Handle from pool_a used on pool_b (wrong pool)
 *   - Handle freed in loop, used next iteration
 *
 * Technique: Typestate tracking with disjunctive paths (Pulse/ISL).
 * Zero false positives by construction (under-approximation).
 *
 * See zer-check-design.md for full design rationale.
 * ================================================================ */

/* handle typestate */
typedef enum {
    HS_UNKNOWN,         /* not yet seen */
    HS_ALIVE,           /* allocated, valid to use */
    HS_FREED,           /* freed, any use = bug */
    HS_MAYBE_FREED,     /* freed on some paths — use is a potential bug */
} HandleState;

/* per-handle tracking info */
typedef struct {
    const char *name;       /* variable name */
    uint32_t name_len;
    HandleState state;
    int pool_id;            /* which pool allocated this (-1 = unknown) */
    int alloc_line;         /* where allocated */
    int free_line;          /* where freed (if FREED) */
    int alloc_id;           /* unique allocation ID — aliases share same ID */
    int source_color;       /* ZC_COLOR_* — where the memory came from */
    bool escaped;           /* true if returned, stored in global, or stored in param field */
} HandleInfo;

/* one execution path's view of all handles — dynamic array */
typedef struct {
    HandleInfo *handles;    /* arena-allocated, grows as needed */
    int handle_count;
    int handle_capacity;
} PathState;

/* pool registry entry */
typedef struct {
    const char *name;
    uint32_t name_len;
    int id;
} ZcPool;

/* Allocation source color — tracks where memory came from */
#define ZC_COLOR_UNKNOWN  0  /* param, cinclude, can't determine */
#define ZC_COLOR_POOL     1  /* Pool/Slab — needs individual free */
#define ZC_COLOR_ARENA    2  /* Arena — freed by arena.reset(), no individual free */
#define ZC_COLOR_MALLOC   3  /* malloc/calloc — needs free() */

/* cross-function summary: what a function does to its Handle params */
typedef struct {
    const char *func_name;
    uint32_t func_name_len;
    int param_count;
    bool *frees_param;        /* definite free (all paths) */
    bool *maybe_frees_param;  /* conditional free (some paths) */
    int returns_color;        /* allocation color of return value (ZC_COLOR_*) */
    int returns_param_color;  /* -1 = N/A, 0+ = return inherits param[N]'s color */
} FuncSummary;

/* ZER-CHECK context */
typedef struct {
    Checker *checker;
    Arena *arena;
    const char *file_name;
    int error_count;

    /* disjunctive path states — dynamic */
    PathState *paths;
    int path_count;
    int path_capacity;

    /* pool variable registry — dynamic */
    ZcPool *pools;
    int pool_count;
    int pool_capacity;

    /* cross-function summaries — built in pre-scan, used during analysis */
    FuncSummary *summaries;
    int summary_count;
    int summary_capacity;
    bool building_summary;  /* suppress error reporting during summary phase */

    /* allocation ID counter — each unique allocation gets a unique ID */
    int next_alloc_id;

    /* imported module ASTs for cross-module summary building */
    Node **import_asts;
    int import_ast_count;
} ZerCheck;

/* ---- API ---- */
void zercheck_init(ZerCheck *zc, Checker *checker, Arena *arena, const char *file);
bool zercheck_run(ZerCheck *zc, Node *file_node);

#endif /* ZER_CHECK_H */
