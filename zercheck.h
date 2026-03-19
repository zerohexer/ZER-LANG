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

#define ZC_MAX_HANDLES 128
#define ZC_MAX_PATHS 32

/* handle typestate */
typedef enum {
    HS_UNKNOWN,         /* not yet seen */
    HS_ALIVE,           /* allocated, valid to use */
    HS_FREED,           /* freed, any use = bug */
} HandleState;

/* per-handle tracking info */
typedef struct {
    const char *name;       /* variable name */
    uint32_t name_len;
    HandleState state;
    int pool_id;            /* which pool allocated this (-1 = unknown) */
    int alloc_line;         /* where allocated */
    int free_line;          /* where freed (if FREED) */
} HandleInfo;

/* one execution path's view of all handles */
typedef struct {
    HandleInfo handles[ZC_MAX_HANDLES];
    int handle_count;
} PathState;

/* ZER-CHECK context */
typedef struct {
    Checker *checker;
    Arena *arena;
    const char *file_name;
    int error_count;

    /* disjunctive path states */
    PathState paths[ZC_MAX_PATHS];
    int path_count;

    /* pool variable registry */
    struct { const char *name; uint32_t name_len; int id; } pools[64];
    int pool_count;
} ZerCheck;

/* ---- API ---- */
void zercheck_init(ZerCheck *zc, Checker *checker, Arena *arena, const char *file);
bool zercheck_run(ZerCheck *zc, Node *file_node);

#endif /* ZER_CHECK_H */
