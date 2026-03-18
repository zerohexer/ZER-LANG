#ifndef ZER_CHECKER_H
#define ZER_CHECKER_H

#include "ast.h"
#include "types.h"

/* ================================================================
 * ZER Type Checker
 *
 * Walks the AST, resolves types, checks type correctness.
 * Produces a typed AST (annotates nodes with resolved Type*).
 *
 * Does NOT handle:
 * - Dataflow (handle consumption, scope escape) — separate pass
 * - ZER-CHECK (path-sensitive analysis) — separate pass
 * - Safety insertion (bounds, zero) — separate pass
 * ================================================================ */

typedef struct {
    Arena *arena;           /* arena for type allocations */
    Scope *global_scope;    /* module-level scope */
    Scope *current_scope;   /* current scope in traversal */
    const char *file_name;
    int error_count;
    int warning_count;
    Type *current_func_ret; /* return type of current function (for return stmt checking) */
    bool in_loop;           /* true when inside for/while (for break/continue checking) */
    int defer_depth;        /* > 0 when inside a defer block */
} Checker;

/* ---- API ---- */
void checker_init(Checker *c, Arena *arena, const char *file_name);
bool checker_check(Checker *c, Node *file_node);

/* returns the resolved Type* for an expression node (set during check) */
Type *checker_get_type(Node *node);

#endif /* ZER_CHECKER_H */
