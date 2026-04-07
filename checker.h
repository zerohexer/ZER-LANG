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
    const char *source;     /* source text for error display (NULL = skip source line) */
    int error_count;
    int warning_count;
    Type *current_func_ret; /* return type of current function (for return stmt checking) */
    bool in_loop;           /* true when inside for/while (for break/continue checking) */
    int defer_depth;        /* > 0 when inside a defer block */
    int critical_depth;     /* > 0 when inside @critical block — ban return/break/continue/goto */
    bool in_assign_target;  /* true when checking LHS of assignment */
    const char *union_switch_var;  /* variable name being switched on (union only) */
    uint32_t union_switch_var_len;
    const char *union_switch_key;  /* BUG-392: full path key e.g. "msgs[0]" for array element locks */
    uint32_t union_switch_key_len;
    Type *union_switch_type;      /* the union type being switched — blocks alias mutation */
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

    /* non-storable node tracking (pool.get, slab.get results) — was global (BUG-346) */
    Node **non_storable_nodes;
    int non_storable_count;
    int non_storable_capacity;

    /* mmio range registry — @inttoptr validates addresses against these */
    uint64_t (*mmio_ranges)[2]; /* array of [start, end] pairs */
    int mmio_range_count;
    int mmio_range_capacity;
    bool no_strict_mmio;  /* --no-strict-mmio: allow @inttoptr without mmio declarations */

    /* Task.new() auto-slab: auto-created Slab(T) per struct type */
    struct { Type *elem_type; Symbol *slab_sym; } *auto_slabs;
    int auto_slab_count;
    int auto_slab_capacity;
    int target_ptr_bits;  /* target pointer width in bits (default 32 for embedded) */
    uint32_t next_type_id; /* BUG-393: counter for runtime provenance type IDs */

    /* BUG-393: compile-time provenance map for compound paths (h.p, arr[0]) */
    struct { char *key; uint32_t key_len; Type *provenance; } *prov_map;
    int prov_map_count;
    int prov_map_capacity;

    /* Value range propagation: tracks {min, max, known_nonzero} per variable.
     * Stack-based: newer entries shadow older. Save/restore via count. */
    struct VarRange {
        const char *name;
        uint32_t name_len;
        int64_t min_val;
        int64_t max_val;
        bool known_nonzero;
    } *var_ranges;
    int var_range_count;
    int var_range_capacity;

    /* Nodes proven safe by range propagation — emitter skips runtime checks */
    Node **proven_safe;
    int proven_safe_count;
    int proven_safe_capacity;

    /* Auto-guard nodes: unproven array accesses that need auto-inserted bounds guard.
     * Emitter inserts if (idx >= size) { return <zero>; } before the access. */
    struct AutoGuard {
        Node *node;         /* the NODE_INDEX node */
        uint64_t array_size; /* 0 = slice (use .len at runtime) */
    } *auto_guards;
    int auto_guard_count;
    int auto_guard_capacity;

    /* Dynamic-index freed handles: tracks pool.free(arr[variable]) for UAF auto-guard.
     * When arr[j] is later accessed via Handle auto-deref, emitter inserts
     * if (j == freed_idx) { return <zero>; } before the access. */
    struct DynFreed {
        const char *array_name;     /* root array variable name */
        uint32_t array_name_len;
        Node *freed_idx;            /* the index expression used in free() */
        bool all_freed;             /* true if freed in a loop (all elements) */
    } *dyn_freed;
    int dyn_freed_count;
    int dyn_freed_capacity;

    /* Cross-function provenance summaries: what provenance a function's return carries */
    struct ProvSummary {
        const char *func_name;
        uint32_t func_name_len;
        Type *return_provenance;  /* NULL = unknown */
    } *prov_summaries;
    int prov_summary_count;
    int prov_summary_capacity;

    /* Whole-program param provenance: what type each *opaque param expects */
    struct ParamExpect {
        const char *func_name;
        uint32_t func_name_len;
        int param_index;          /* which parameter */
        Type *expected_type;      /* what @ptrcast casts it to inside the function */
    } *param_expects;
    int param_expect_count;
    int param_expect_capacity;

    /* Interrupt safety: track globals accessed from ISR and regular code */
    bool in_interrupt;  /* true when checking NODE_INTERRUPT body */
    bool in_naked;      /* true when checking naked function body (MISRA Dir 4.3) */
    bool in_comptime_body; /* true when checking comptime function body — skip comptime arg validation */
    struct IsrGlobal {
        const char *name;
        uint32_t name_len;
        bool from_isr;          /* accessed inside interrupt body */
        bool from_func;         /* accessed inside regular function */
        bool compound_in_isr;   /* compound assign (|=, +=) in ISR */
        bool compound_in_func;  /* compound assign in regular func */
    } *isr_globals;
    int isr_global_count;
    int isr_global_capacity;

    /* Stack depth analysis: call graph + frame sizes */
    struct StackFrame {
        const char *name;
        uint32_t name_len;
        uint32_t frame_size;    /* estimated local variable bytes */
        const char **callees;   /* function names called from this function */
        uint32_t *callee_lens;
        int callee_count;
        int callee_capacity;
        bool is_recursive;      /* part of a call cycle */
    } *stack_frames;
    int stack_frame_count;
    int stack_frame_capacity;
} Checker;

/* ---- API ---- */
void checker_init(Checker *c, Arena *arena, const char *file_name);
void checker_register_file(Checker *c, Node *file_node); /* register declarations only */
bool checker_check(Checker *c, Node *file_node);
bool checker_check_bodies(Checker *c, Node *file_node); /* check bodies only, decls already registered */
void checker_post_passes(Checker *c, Node *file_node); /* stack depth + interrupt safety (after all bodies checked) */
void checker_push_module_scope(Checker *c, Node *file_node); /* push scope with module's own types */
void checker_pop_module_scope(Checker *c); /* pop module scope */

/* returns the resolved Type* for an expression node (set during check) */
Type *checker_get_type(Checker *c, Node *node);

/* returns true if this node was proven safe by value range propagation */
bool checker_is_proven(Checker *c, Node *node);

/* returns array_size if this node needs auto-guard, 0 if not */
uint64_t checker_auto_guard_size(Checker *c, Node *node);

/* Handle auto-deref: find unique Slab/Pool for a Handle's element type */
Symbol *find_unique_allocator(Scope *s, Type *elem_type);

#endif /* ZER_CHECKER_H */
