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
    HS_TRANSFERRED,     /* ownership transferred to another thread via spawn */
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
    int transfer_line;      /* where ownership was transferred (spawn) */
    bool escaped;           /* true if returned, stored in global, or stored in param field */
    bool is_thread_handle;  /* ThreadHandle from scoped spawn — leak = "thread not joined" */
    int scope_depth;        /* BUG-488: lexical scope depth for variable shadowing */
} HandleInfo;

/* one execution path's view of all handles — dynamic array */
typedef struct {
    HandleInfo *handles;    /* arena-allocated, grows as needed */
    int handle_count;
    int handle_capacity;
    bool terminated;        /* true if block hit return/break/continue/goto — doesn't fall through */
    int scope_depth;        /* BUG-488: current lexical scope depth — set by NODE_BLOCK handler */
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
    /* rdh99l (2026-08-02): the callee frees a FIELD of param i (a compound
     * handle rooted at the param — `free(h.buckets)` where h is a by-value
     * struct/union OR a `*Struct` pointer param). The whole-param frees_param
     * arrays above only track a bare param free, so a cross-function
     * double-free / UAF of a param FIELD compiled clean. definite = every
     * return path frees some field; maybe = some path frees some field. The
     * call site widens the caller's tracked field handles rooted at the arg.
     * NULL when param_count == 0 (memset-zeroed). */
    bool *frees_param_field;
    bool *maybe_frees_param_field;
    int returns_color;        /* allocation color of return value (ZC_COLOR_*) */
    int returns_param_color;  /* -1 = N/A, 0+ = return inherits param[N]'s color */
    /* BUG-849 (2026-08-23): the SET of params the return may be a view of.
     * bit n set = some return path hands back a view of param n. This is the
     * honest shape of the question `returns_param_color` asks with one slot;
     * the color stays the exact-single-param fast path (every existing consumer
     * keeps working), and the mask carries the disjunctive case that used to
     * collapse to "unknown" — which is the ACCEPT-UNSAFE direction, because an
     * unknown-provenance pointer result is registered as an unrelated fresh
     * allocation and a slice result is not registered at all.
     * 0 when nothing was proven. Only meaningful together with
     * `returns_all_views`. */
    uint32_t returns_param_mask;
    /* Every non-early-exit, non-fallback return was CLASSIFIED as a view of a
     * param, a view of static storage (`&global`), or a null literal — i.e. no
     * return path can be a fresh allocation. False = at least one return could
     * not be classified, so no view claim may be made at all. */
    bool returns_all_views;
    /* PART 6 (erased-ref ownership, 2026-07-16): the function's body makes NO
     * pointer/opaque/handle-returning CALL, so any pointer it RETURNS is a
     * borrow of caller/global memory, never a fresh allocation (every alloc
     * form — pool.alloc/slab.alloc/alloc()/malloc/arena.alloc — is a
     * pointer-ish-returning call; `&local` return is escape-rejected). The
     * caller must then NOT register the call-result as a new owned allocation.
     * SOUND polarity: true only when PROVEN (default false = conservative,
     * still-track). Realises `aorigin = AOBorrow` from
     * proofs/.../erased_ownership_lattice.v. */
    bool ret_is_borrow;
    /* PART 6 Increment 1: EVERY real (non-early-exit) return is a CONTENT read —
     * a value-read of a pointer field/element, or null — NOT a param-VIEW
     * (address-of / bare pointer / subslice, which aliases a param's own
     * allocation).  Only `ret_is_borrow && ret_is_content` is leak-suppressed
     * (AOBorrow); a param-VIEW stays fully tracked (AOParam — the interior-
     * pointer UAF class).  Default false = conservative (treat as a view). */
    bool ret_is_content;
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

    /* Level B guarded refinement (2026-06-27) — per-function, set by the IR
     * analysis driver, read at free/use sites. gr_block_guards is an
     * IRGuardSet[gr_block_count] (opaque void* — type is zercheck_ir-internal);
     * gr_cur_block is the block currently being analyzed. See zercheck_ir.c
     * "Level B guarded-refinement support" and
     * proofs/operational/lambda_zer_handle/handle_flow_lattice.v Level B. */
    void *gr_block_guards;
    int gr_block_count;
    int gr_cur_block;
    /* BUG-920: defer_origin of the instruction being checked (0 = not from a
     * defer body). A handle freed by registration k and freed again by an
     * instruction of origin k is the same body RE-FIRING on a path that
     * already ran it (goto eager fire + guarded label exit) — idempotent, not a
     * double free / UAF. See ir_use_guard_disjoint. */
    int cur_defer_origin;
} ZerCheck;

/* ---- API ---- */
void zercheck_init(ZerCheck *zc, Checker *checker, Arena *arena, const char *file);
bool zercheck_run(ZerCheck *zc, Node *file_node);

#endif /* ZER_CHECK_H */
