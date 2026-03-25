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

/* Diagnostic entry — collected during checking, read by LSP */
typedef struct {
    int line;
    int severity;       /* 1=error, 2=warning */
    char message[256];
} Diagnostic;

/* typemap entry — maps AST Node* to resolved Type* */
typedef struct {
    Node *key;
    Type *type;
} TypeMapEntry;

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
    bool in_assign_target;  /* true when checking LHS of assignment */
    const char *union_switch_var;  /* variable name being switched on (union only) */
    uint32_t union_switch_var_len;
    const char *current_module;   /* module name for prefix (NULL = main module) */
    uint32_t current_module_len;
    int expr_depth;               /* recursion depth guard for check_expr */

    /* typemap — maps AST Node* to resolved Type* (was global, now per-checker) */
    TypeMapEntry *type_map;
    uint32_t type_map_size;
    uint32_t type_map_count;

    /* diagnostic list — grows dynamically, read by LSP */
    Diagnostic *diagnostics;
    int diag_count;
    int diag_capacity;
} Checker;

/* ---- API ---- */
void checker_init(Checker *c, Arena *arena, const char *file_name);
void checker_register_file(Checker *c, Node *file_node); /* register declarations only */
bool checker_check(Checker *c, Node *file_node);
bool checker_check_bodies(Checker *c, Node *file_node); /* check bodies only, decls already registered */
void checker_push_module_scope(Checker *c, Node *file_node); /* push scope with module's own types */
void checker_pop_module_scope(Checker *c); /* pop module scope */

/* returns the resolved Type* for an expression node (set during check) */
Type *checker_get_type(Checker *c, Node *node);

#endif /* ZER_CHECKER_H */
