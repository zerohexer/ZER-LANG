/*
 * ZER-CHECK on IR — Path-sensitive safety analysis on basic blocks
 *
 * This is the IR-based replacement for zercheck.c's AST walking.
 * Uses IRFunc's basic blocks as a real CFG instead of linear AST walk.
 * Handle states tracked per LOCAL id instead of string keys.
 *
 * Phase 6 of IR implementation. See docs/IR_Implementation.md Part 6.
 *
 * Status: FOUNDATION — core handle tracking framework. Does NOT yet
 * replace the AST zercheck. Both coexist during migration.
 */

#include "ir.h"
#include "zercheck.h"
#include "checker.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* Local error reporting — mirrors zercheck.c's zc_error but accessible here */
static void ir_zc_error(ZerCheck *zc, int line, const char *fmt, ...) {
    if (zc->building_summary) return;
    zc->error_count++;
    fprintf(stderr, "%s:%d: zercheck: ", zc->file_name, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* ================================================================
 * IR Handle State — tracked per LOCAL id (not string key)
 * ================================================================ */

typedef enum {
    IR_HS_UNKNOWN = 0,     /* not yet allocated */
    IR_HS_ALIVE,           /* allocated, valid */
    IR_HS_FREED,           /* freed on all paths */
    IR_HS_MAYBE_FREED,     /* freed on some paths */
    IR_HS_TRANSFERRED,     /* ownership transferred (move struct, spawn) */
} IRHandleState;

typedef struct {
    int local_id;          /* root IRLocal this tracks (compound root) */
    /* Phase B3: compound key support. For bare locals, path is NULL
     * and path_len is 0. For compound entities (s.handle, arr[0],
     * s.inner.next) path holds a string like ".handle" or "[0].inner"
     * built by ir_build_key_path. Arena-allocated, shared OK across
     * path states (copy-on-write semantics not needed — strings are
     * immutable). */
    const char *path;
    uint32_t path_len;
    IRHandleState state;
    int alloc_line;        /* where allocated */
    int free_line;         /* where freed */
    int alloc_id;          /* groups aliases — same alloc = same id */
    bool escaped;          /* returned, stored to global, etc. */
    /* Phase D1: allocation color — tracks where memory came from.
     * ZC_COLOR_POOL   — Pool/Slab, needs individual free or defer.
     * ZC_COLOR_ARENA  — Arena, freed by arena.reset(). Skip leak check.
     * ZC_COLOR_MALLOC — extern malloc/calloc, needs matching free.
     * ZC_COLOR_UNKNOWN (0) — param, cinclude, can't determine. */
    int source_color;
    /* Phase D3: ThreadHandle — from scoped spawn. Leak = "thread not joined". */
    bool is_thread_handle;
} IRHandleInfo;

/* Phase E: scoped spawn ThreadHandle tracking by name. Scoped spawn
 * (ThreadHandle th = spawn ...) doesn't create an IR local (emitter
 * emits its own pthread_t declaration). zercheck_ir tracks join status
 * by the handle's source name so spawn_no_join detection works on CFG. */
typedef struct {
    const char *name;
    uint32_t name_len;
    int spawn_line;
    bool joined;
} IRThreadTrack;

/* Per-block analysis state */
typedef struct {
    IRHandleInfo *handles;
    int handle_count;
    int handle_capacity;
    bool terminated;       /* block ends with return/unreachable */
    /* Phase D5: @critical nesting depth. Tracked via IR_CRITICAL_BEGIN
     * and IR_CRITICAL_END. While > 0, spawn is banned (would create
     * thread with interrupts disabled — hardware-unsafe). Alloc from
     * slab also banned (calloc/realloc may deadlock if interrupted). */
    int critical_depth;
    /* Phase E: ThreadHandle tracking (name-based, not local-id) */
    IRThreadTrack *threads;
    int thread_count;
    int thread_capacity;
} IRPathState;

/* ================================================================
 * Path State Operations
 * ================================================================ */

static void ir_ps_init(IRPathState *ps) {
    ps->handles = NULL;
    ps->handle_count = 0;
    ps->handle_capacity = 0;
    ps->terminated = false;
    ps->critical_depth = 0;
    ps->threads = NULL;
    ps->thread_count = 0;
    ps->thread_capacity = 0;
}

static IRPathState ir_ps_copy(IRPathState *src) {
    IRPathState dst;
    dst.handle_count = src->handle_count;
    dst.handle_capacity = src->handle_count > 0 ? src->handle_count : 4;
    dst.terminated = false;
    dst.critical_depth = src->critical_depth; /* Phase D5: preserve */
    dst.handles = (IRHandleInfo *)malloc(dst.handle_capacity * sizeof(IRHandleInfo));
    if (src->handles && src->handle_count > 0)
        memcpy(dst.handles, src->handles, src->handle_count * sizeof(IRHandleInfo));
    /* Phase E: copy ThreadHandle tracking */
    dst.thread_count = src->thread_count;
    dst.thread_capacity = src->thread_count > 0 ? src->thread_count : 2;
    dst.threads = (IRThreadTrack *)malloc(dst.thread_capacity * sizeof(IRThreadTrack));
    if (src->threads && src->thread_count > 0)
        memcpy(dst.threads, src->threads, src->thread_count * sizeof(IRThreadTrack));
    return dst;
}

static void ir_ps_free(IRPathState *ps) {
    free(ps->handles);
    ps->handles = NULL;
    ps->handle_count = 0;
    free(ps->threads);
    ps->threads = NULL;
    ps->thread_count = 0;
}

/* ThreadHandle by-name lookup and registration (Phase E). */
static IRThreadTrack *ir_find_thread(IRPathState *ps, const char *name,
                                      uint32_t name_len) {
    for (int i = 0; i < ps->thread_count; i++) {
        if (ps->threads[i].name_len == name_len &&
            memcmp(ps->threads[i].name, name, name_len) == 0)
            return &ps->threads[i];
    }
    return NULL;
}

static IRThreadTrack *ir_add_thread(IRPathState *ps, const char *name,
                                     uint32_t name_len, int line) {
    IRThreadTrack *existing = ir_find_thread(ps, name, name_len);
    if (existing) {
        existing->spawn_line = line;
        existing->joined = false;
        return existing;
    }
    if (ps->thread_count >= ps->thread_capacity) {
        int nc = ps->thread_capacity < 4 ? 4 : ps->thread_capacity * 2;
        IRThreadTrack *nt = (IRThreadTrack *)realloc(ps->threads,
            nc * sizeof(IRThreadTrack));
        if (!nt) return NULL;
        ps->threads = nt;
        ps->thread_capacity = nc;
    }
    IRThreadTrack *t = &ps->threads[ps->thread_count++];
    t->name = name;
    t->name_len = name_len;
    t->spawn_line = line;
    t->joined = false;
    return t;
}

/* Bare-local lookup: matches only entries with path_len == 0.
 * Phase B3: compound entries (path != NULL) with the same local_id are NOT
 * returned here — they represent different entities (e.g. `s` vs `s.handle`). */
static IRHandleInfo *ir_find_handle(IRPathState *ps, int local_id) {
    for (int i = 0; i < ps->handle_count; i++)
        if (ps->handles[i].local_id == local_id &&
            ps->handles[i].path_len == 0)
            return &ps->handles[i];
    return NULL;
}

/* Compound-aware lookup: matches (local_id, path) exactly. path_len=0 with
 * path=NULL is a bare local (equivalent to ir_find_handle). */
static IRHandleInfo *ir_find_compound_handle(IRPathState *ps, int local_id,
                                              const char *path, uint32_t path_len) {
    for (int i = 0; i < ps->handle_count; i++) {
        if (ps->handles[i].local_id != local_id) continue;
        if (ps->handles[i].path_len != path_len) continue;
        if (path_len == 0) return &ps->handles[i];
        if (ps->handles[i].path && path &&
            memcmp(ps->handles[i].path, path, path_len) == 0)
            return &ps->handles[i];
    }
    return NULL;
}

/* Grow handles array by 1 slot, return pointer to new slot (zeroed). */
static IRHandleInfo *ir_alloc_handle_slot(IRPathState *ps) {
    if (ps->handle_count >= ps->handle_capacity) {
        int nc = ps->handle_capacity < 8 ? 8 : ps->handle_capacity * 2;
        IRHandleInfo *nh = (IRHandleInfo *)realloc(ps->handles, nc * sizeof(IRHandleInfo));
        if (!nh) return NULL;
        ps->handles = nh;
        ps->handle_capacity = nc;
    }
    IRHandleInfo *h = &ps->handles[ps->handle_count++];
    memset(h, 0, sizeof(IRHandleInfo));
    h->state = IR_HS_UNKNOWN;
    return h;
}

static IRHandleInfo *ir_add_handle(IRPathState *ps, int local_id) {
    IRHandleInfo *existing = ir_find_handle(ps, local_id);
    if (existing) return existing;
    IRHandleInfo *h = ir_alloc_handle_slot(ps);
    if (h) h->local_id = local_id;
    return h;
}

/* Add a compound handle entry (or return existing). path must be arena-
 * allocated by the caller — this struct just stores the pointer. */
static IRHandleInfo *ir_add_compound_handle(IRPathState *ps, int local_id,
                                             const char *path, uint32_t path_len) {
    IRHandleInfo *existing = ir_find_compound_handle(ps, local_id, path, path_len);
    if (existing) return existing;
    IRHandleInfo *h = ir_alloc_handle_slot(ps);
    if (h) {
        h->local_id = local_id;
        h->path = path;
        h->path_len = path_len;
    }
    return h;
}

/* ================================================================
 * State Helpers (same as zercheck.c but on IRHandleState)
 * ================================================================ */

static bool ir_is_invalid(IRHandleInfo *h) {
    return h->state == IR_HS_FREED ||
           h->state == IR_HS_MAYBE_FREED ||
           h->state == IR_HS_TRANSFERRED;
}

static const char *ir_state_name(IRHandleState s) {
    switch (s) {
    case IR_HS_UNKNOWN:     return "unknown";
    case IR_HS_ALIVE:       return "alive";
    case IR_HS_FREED:       return "freed";
    case IR_HS_MAYBE_FREED: return "maybe-freed";
    case IR_HS_TRANSFERRED: return "transferred";
    }
    return "?";
}

/* ================================================================
 * Move struct type detection (Phase B1 — ported from zercheck.c:920-961)
 *
 * A `move struct` has is_move=true on its struct_type. Passing or
 * assigning a move-struct-typed value transfers ownership: source
 * becomes TRANSFERRED, destination takes over as ALIVE.
 *
 * contains_move_struct_field / should_track_move cover the case
 * where a regular struct has a move struct field — the outer struct
 * inherits transfer semantics.
 * ================================================================ */

static bool ir_is_move_struct_type(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    return (eff->kind == TYPE_STRUCT && eff->struct_type.is_move);
}

static bool ir_contains_move_struct_field(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind == TYPE_STRUCT) {
        for (uint32_t i = 0; i < eff->struct_type.field_count; i++) {
            if (ir_is_move_struct_type(eff->struct_type.fields[i].type))
                return true;
        }
    }
    /* Union containing move struct variant */
    if (eff->kind == TYPE_UNION) {
        for (uint32_t i = 0; i < eff->union_type.variant_count; i++) {
            if (ir_is_move_struct_type(eff->union_type.variants[i].type))
                return true;
        }
    }
    return false;
}

static bool ir_should_track_move(Type *t) {
    return t && (ir_is_move_struct_type(t) || ir_contains_move_struct_field(t));
}

/* Allocation ID counter for move struct new-ownership chains.
 * When ownership transfers (e.g., Token b = a), the destination
 * gets a fresh alloc_id representing a new ownership identity —
 * the source's alloc_id goes with its TRANSFERRED state. */
static int _ir_next_alloc_id = 1000000;  /* high base so it doesn't
                                            collide with local_id-based
                                            ids set at allocation */

/* ================================================================
 * Escape detection helpers (Phase B2)
 *
 * A handle "escapes" when it's stored somewhere outside the current
 * function's tracking scope — a global, a pointer-param's field, a
 * returned struct literal. The escape flag suppresses leak detection
 * for that handle at function exit (the caller now owns it).
 * ================================================================ */

/* Walk a target chain (NODE_FIELD / NODE_INDEX / NODE_UNARY_deref)
 * up to the root identifier. Returns the root NODE_IDENT or NULL. */
static Node *ir_target_root(Node *target) {
    Node *cur = target;
    while (cur) {
        if (cur->kind == NODE_FIELD) cur = cur->field.object;
        else if (cur->kind == NODE_INDEX) cur = cur->index_expr.object;
        else if (cur->kind == NODE_UNARY && cur->unary.op == TOK_STAR)
            cur = cur->unary.operand;
        else break;
    }
    return cur;
}

/* Returns true if the target expression's root is a global variable OR
 * a pointer parameter (in which case the field-write reaches callee-
 * external memory). Either way, any handle written through it escapes. */
static bool ir_target_root_escapes(ZerCheck *zc, Node *target) {
    Node *root = ir_target_root(target);
    if (!root || root->kind != NODE_IDENT) return false;
    /* Global check */
    if (scope_lookup(zc->checker->global_scope,
        root->ident.name, (uint32_t)root->ident.name_len) != NULL)
        return true;
    /* Pointer param: s.top = h where s is *Stack. */
    if (target && target->kind == NODE_FIELD) {
        Type *root_type = checker_get_type(zc->checker, root);
        if (root_type) {
            Type *rt = type_unwrap_distinct(root_type);
            if (rt && rt->kind == TYPE_POINTER) return true;
        }
    }
    return false;
}

/* Mark a local's handle (if tracked) as escaped. */
static void ir_mark_local_escaped(IRPathState *ps, int local_id) {
    if (local_id < 0) return;
    IRHandleInfo *h = ir_find_handle(ps, local_id);
    if (h) h->escaped = true;
}

/* Given an AST value expression (RHS of assign or return), find the
 * local it refers to (following through NODE_ORELSE to the primary
 * expression). Returns local id or -1. */
static int ir_find_value_local(IRFunc *func, Node *val) {
    if (!val) return -1;
    if (val->kind == NODE_ORELSE) val = val->orelse.expr;
    if (val && val->kind == NODE_IDENT) {
        return ir_find_local_exact_first(func,
            val->ident.name, (uint32_t)val->ident.name_len);
    }
    return -1;
}

/* ================================================================
 * CFG Merge — the key advantage over linear AST walk
 *
 * At basic block join points (multiple predecessors), merge handle
 * states from all incoming paths. This is NATURAL with basic blocks.
 * No hack, no block_always_exits check, no 2-pass workaround.
 * ================================================================ */

static IRPathState ir_merge_states(IRPathState *states, int state_count) {
    if (state_count == 0) {
        IRPathState empty;
        ir_ps_init(&empty);
        return empty;
    }
    if (state_count == 1) return ir_ps_copy(&states[0]);

    /* Start from first non-terminated state */
    int first_live = -1;
    for (int i = 0; i < state_count; i++) {
        if (!states[i].terminated) { first_live = i; break; }
    }
    if (first_live < 0) {
        /* All predecessors terminated — this block is unreachable */
        IRPathState result;
        ir_ps_init(&result);
        result.terminated = true;
        return result;
    }

    IRPathState result = ir_ps_copy(&states[first_live]);

    /* Merge each subsequent non-terminated predecessor */
    for (int si = first_live + 1; si < state_count; si++) {
        if (states[si].terminated) continue; /* dead path, skip */

        /* For each handle in result, check if same handle exists in this pred */
        for (int hi = 0; hi < result.handle_count; hi++) {
            IRHandleInfo *rh = &result.handles[hi];
            IRHandleInfo *ph = ir_find_handle(&states[si], rh->local_id);

            if (!ph) continue; /* handle not in this pred — keep result's state */

            /* Merge states: both freed → freed, one freed → maybe_freed, etc. */
            if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_FREED) {
                rh->state = IR_HS_MAYBE_FREED;
                rh->free_line = ph->free_line;
            } else if (rh->state == IR_HS_FREED && ph->state == IR_HS_ALIVE) {
                rh->state = IR_HS_MAYBE_FREED;
            } else if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_TRANSFERRED) {
                rh->state = IR_HS_MAYBE_FREED; /* conservative */
            } else if (rh->state == IR_HS_TRANSFERRED && ph->state == IR_HS_ALIVE) {
                rh->state = IR_HS_MAYBE_FREED;
            }
            /* Both same state → keep. Both freed → keep freed. */
        }

        /* Add handles from pred that aren't in result yet */
        for (int pi = 0; pi < states[si].handle_count; pi++) {
            if (!ir_find_handle(&result, states[si].handles[pi].local_id)) {
                IRHandleInfo *nh = ir_add_handle(&result, states[si].handles[pi].local_id);
                if (nh) *nh = states[si].handles[pi];
            }
        }
    }

    return result;
}

/* ================================================================
 * Compound key extraction (Phase B3)
 *
 * Given an AST expression, produce a tracking key: (root_local_id, path).
 * Mirrors zercheck.c:172-213 handle_key_from_expr but resolves identifiers
 * to IR local IDs instead of producing name-based keys.
 *
 * Examples:
 *   NODE_IDENT("h")              → local=h_id,  path=""     (bare local)
 *   NODE_FIELD(s, "handle")      → local=s_id,  path=".handle"
 *   NODE_INDEX(arr, IntLit(0))   → local=arr_id, path="[0]"
 *   NODE_FIELD(NODE_FIELD(s,"a"), "b") → local=s_id, path=".a.b"
 *
 * Only constant integer indices are trackable (matches zercheck.c behavior).
 * Variable indices return -1 (caller falls back to "ungrouped").
 *
 * Returns 0 on success, -1 if expression isn't trackable as a key.
 * On success: *out_local = root local id; *out_path = arena string (NULL
 * if bare local); *out_path_len = length of path (0 for bare local).
 * ================================================================ */

static int ir_build_key_path(Node *expr, char *buf, int bufsize, int *out_base_len);

/* Build the path component. Returns number of chars written (not including NUL),
 * or -1 if expression can't be keyed. `out_base_len` receives the length of
 * the root-ident portion (always 0 here; root ident is NOT part of path). */
static int ir_build_key_path(Node *expr, char *buf, int bufsize, int *out_base_len) {
    if (!expr) return -1;
    if (expr->kind == NODE_IDENT) {
        if (out_base_len) *out_base_len = 0;
        return 0;  /* bare ident — empty path */
    }
    if (expr->kind == NODE_FIELD) {
        int parent_len = ir_build_key_path(expr->field.object, buf, bufsize, out_base_len);
        if (parent_len < 0) return -1;
        int fnlen = (int)expr->field.field_name_len;
        if (parent_len + 1 + fnlen >= bufsize) return -1;
        buf[parent_len] = '.';
        memcpy(buf + parent_len + 1, expr->field.field_name, fnlen);
        int total = parent_len + 1 + fnlen;
        buf[total] = '\0';
        return total;
    }
    if (expr->kind == NODE_INDEX) {
        if (!expr->index_expr.index ||
            expr->index_expr.index->kind != NODE_INT_LIT) return -1;
        int parent_len = ir_build_key_path(expr->index_expr.object, buf, bufsize, out_base_len);
        if (parent_len < 0) return -1;
        uint64_t idx = expr->index_expr.index->int_lit.value;
        int written = snprintf(buf + parent_len, bufsize - parent_len,
                               "[%llu]", (unsigned long long)idx);
        if (written <= 0 || parent_len + written >= bufsize) return -1;
        return parent_len + written;
    }
    return -1;
}

/* Walk to the root IDENT of a field/index chain. Returns the NODE_IDENT,
 * or NULL if the chain doesn't bottom out at an identifier. */
static Node *ir_key_root_ident(Node *expr) {
    Node *cur = expr;
    while (cur) {
        if (cur->kind == NODE_IDENT) return cur;
        if (cur->kind == NODE_FIELD) cur = cur->field.object;
        else if (cur->kind == NODE_INDEX) cur = cur->index_expr.object;
        else return NULL;
    }
    return NULL;
}

/* Extract a tracking key from an AST expression. Returns 0 on success,
 * -1 if the expression isn't keyable. Caller gets (out_local, out_path,
 * out_path_len). Bare local: path=NULL, path_len=0. Compound: path is
 * arena-allocated string like ".handle" or "[0].val". */
static int ir_extract_compound_key(ZerCheck *zc, IRFunc *func, Node *expr,
                                    int *out_local,
                                    const char **out_path,
                                    uint32_t *out_path_len) {
    *out_local = -1;
    *out_path = NULL;
    *out_path_len = 0;
    if (!expr) return -1;

    Node *root = ir_key_root_ident(expr);
    if (!root) return -1;
    int local = ir_find_local_exact_first(func,
        root->ident.name, (uint32_t)root->ident.name_len);
    if (local < 0) return -1;
    *out_local = local;

    /* Bare ident — no path */
    if (expr->kind == NODE_IDENT) return 0;

    /* Compound — build path into stack buffer then arena-copy */
    char stack_buf[256];
    int len = ir_build_key_path(expr, stack_buf, sizeof(stack_buf), NULL);
    if (len <= 0) return -1;
    char *path = (char *)arena_alloc(zc->arena, len + 1);
    if (!path) return -1;
    memcpy(path, stack_buf, len + 1);
    *out_path = path;
    *out_path_len = (uint32_t)len;
    return 0;
}

/* ================================================================
 * *opaque / extern alloc-free recognition (Phase C2 — 9a/9b/9c)
 *
 * For cross-module C interop (cinclude), the compiler needs to recognize
 * extern functions that allocate or free pointers even without an explicit
 * FuncSummary built by zc_ir_build_summary. The rules mirror zercheck.c:
 *
 *  Alloc:  bodyless function returning *opaque / *T / ?*T / ?*opaque
 *          e.g., malloc, sqlite3_open, fopen
 *
 *  Free:   bodyless void function whose first param is *opaque / *T
 *          e.g., free, sqlite3_close, fclose, destroy
 *          OR explicitly named "free"
 *
 * When we recognize these at IR_CALL sites, we update the tracked state
 * accordingly. 9a (struct field *opaque UAF) is covered by the existing
 * B3 compound keys + these alloc/free sites. 9b (cross-function free)
 * is covered by C1 FuncSummary OR the signature heuristic here for extern
 * functions that don't have summaries built. 9c (return freed pointer)
 * is handled in the IR_RETURN handler above.
 * ================================================================ */

/* Check if a call is to an extern function that returns a pointer-like
 * type (allocator heuristic). Returns true for malloc/fopen/create/etc. */
static bool ir_is_extern_alloc_call(ZerCheck *zc, Node *call) {
    if (!call || call->kind != NODE_CALL) return false;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_IDENT) return false;
    Symbol *sym = scope_lookup(zc->checker->global_scope,
        callee->ident.name, (uint32_t)callee->ident.name_len);
    if (!sym || !sym->is_function || !sym->func_node) return false;
    /* must be bodyless (extern or cinclude) */
    if (sym->func_node->func_decl.body) return false;
    Type *ret = sym->type;
    if (ret && ret->kind == TYPE_FUNC_PTR) ret = ret->func_ptr.ret;
    if (!ret) return false;
    ret = type_unwrap_distinct(ret);
    if (ret->kind == TYPE_POINTER || ret->kind == TYPE_OPAQUE) return true;
    if (ret->kind == TYPE_OPTIONAL) {
        Type *inner = type_unwrap_distinct(ret->optional.inner);
        if (inner && (inner->kind == TYPE_POINTER || inner->kind == TYPE_OPAQUE))
            return true;
    }
    return false;
}

/* Check if a call is to a function that frees its first argument.
 * Either explicitly named "free" OR bodyless void fn with *opaque/*T first
 * param (signature heuristic — catches destroy/close/cleanup patterns). */
static bool ir_is_extern_free_call(ZerCheck *zc, Node *call) {
    if (!call || call->kind != NODE_CALL) return false;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_IDENT) return false;
    if (call->call.arg_count < 1) return false;
    /* Explicit "free" */
    if (callee->ident.name_len == 4 &&
        memcmp(callee->ident.name, "free", 4) == 0) return true;
    /* Signature heuristic: bodyless void fn(*opaque/*T ...) */
    Symbol *sym = scope_lookup(zc->checker->global_scope,
        callee->ident.name, (uint32_t)callee->ident.name_len);
    if (!sym || !sym->is_function || !sym->func_node) return false;
    if (sym->func_node->func_decl.body) return false;
    Type *ret = sym->type;
    if (ret && ret->kind == TYPE_FUNC_PTR) ret = ret->func_ptr.ret;
    if (!ret || type_unwrap_distinct(ret)->kind != TYPE_VOID) return false;
    if (sym->func_node->func_decl.param_count < 1) return false;
    Type *p0 = NULL;
    if (sym->type && sym->type->kind == TYPE_FUNC_PTR &&
        sym->type->func_ptr.param_count >= 1)
        p0 = sym->type->func_ptr.params[0];
    if (!p0) return false;
    p0 = type_unwrap_distinct(p0);
    return p0->kind == TYPE_POINTER || p0->kind == TYPE_OPAQUE;
}

/* ================================================================
 * Pool/Slab/Task method call classification (Phase E)
 *
 * Even though ir.h defines IR_POOL_ALLOC / IR_SLAB_ALLOC / IR_POOL_FREE
 * etc. as distinct opcodes, the IR lowering (ir_lower.c) collapses them
 * all to generic IR_ASSIGN (for alloc / get) and IR_CALL (for free).
 * Per ir_lower.c:84: "IR_POOL_ALLOC etc. — collapsed to IR_ASSIGN in
 * Phase 8d".
 *
 * This means the specialized IR_POOL_ALLOC / IR_SLAB_FREE / etc. cases
 * in ir_check_inst are effectively dead code in practice. zercheck_ir
 * must recognize these method calls by INSPECTING the AST expression
 * inside IR_ASSIGN / IR_CALL instructions.
 *
 * Kind classification result:
 *   IRMC_NONE       — not a recognized builtin method
 *   IRMC_ALLOC      — pool/slab.alloc(), Task.alloc() → returns Handle
 *   IRMC_ALLOC_PTR  — slab/Task.alloc_ptr()          → returns *T
 *   IRMC_GET        — pool/slab.get(h)               → *T, UAF check only
 *   IRMC_FREE       — pool/slab.free(h), Task.free() → FREED
 *   IRMC_FREE_PTR   — slab/Task.free_ptr(p)          → FREED
 *   IRMC_ARENA_ALLOC — arena.alloc(T)                → ARENA color
 * ================================================================ */

typedef enum {
    IRMC_NONE = 0,
    IRMC_ALLOC,
    IRMC_ALLOC_PTR,
    IRMC_GET,
    IRMC_FREE,
    IRMC_FREE_PTR,
    IRMC_ARENA_ALLOC,
} IRMethodKind;

/* Classify a NODE_CALL expression as a builtin method call. Returns
 * IRMC_* kind, or IRMC_NONE if not recognized. The callee must be
 * NODE_FIELD with a method name matching one of the patterns. Arg
 * count is also validated where relevant. */
static IRMethodKind ir_classify_method_call(Node *call) {
    if (!call || call->kind != NODE_CALL) return IRMC_NONE;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_FIELD) return IRMC_NONE;
    const char *m = callee->field.field_name;
    uint32_t ml = (uint32_t)callee->field.field_name_len;
    /* alloc — Pool/Slab/Task with 0 args (arena.alloc has 1 arg — type) */
    if (ml == 5 && memcmp(m, "alloc", 5) == 0) {
        if (call->call.arg_count == 0) return IRMC_ALLOC;
        return IRMC_ARENA_ALLOC;  /* arena.alloc(Type) takes type arg */
    }
    if (ml == 9 && memcmp(m, "alloc_ptr", 9) == 0) return IRMC_ALLOC_PTR;
    if (ml == 3 && memcmp(m, "get", 3) == 0) return IRMC_GET;
    if (ml == 4 && memcmp(m, "free", 4) == 0) return IRMC_FREE;
    if (ml == 8 && memcmp(m, "free_ptr", 8) == 0) return IRMC_FREE_PTR;
    return IRMC_NONE;
}

/* Unwrap orelse-wrapped alloc: `pool.alloc() orelse return` — the IR
 * ASSIGN's expression is NODE_ORELSE(NODE_CALL, NODE_RETURN). We want
 * the primary call for classification. */
static Node *ir_unwrap_alloc_expr(Node *expr) {
    if (!expr) return NULL;
    if (expr->kind == NODE_ORELSE) return expr->orelse.expr;
    return expr;
}

/* Phase E: generic UAF walker for expressions embedded in IR_ASSIGN.
 *
 * When the IR emits `%3 = ASSIGN <expr>` where <expr> is a NODE_ASSIGN
 * wrapping `pool.get(h).id = 5`, or any complex expression containing
 * a use of a freed handle, we need to flag UAF. The walker:
 *   - Recursively visits NODE_FIELD / NODE_INDEX / NODE_CALL / NODE_UNARY /
 *     NODE_BINARY / NODE_ASSIGN / NODE_TYPECAST / NODE_SLICE / NODE_ORELSE
 *   - For NODE_CALL args: check each arg's root ident against tracked handles
 *   - For NODE_IDENT chains (field/index): check the root
 *   - Reports at most once per root_local per expression via a small
 *     reported-set passed by reference.
 *
 * Skips the callee expression itself to avoid double-flagging pool.get
 * callee (we flag via args). Skips addr-of operand to avoid flagging
 * `&freed.field` as a read (allowed in some patterns).
 *
 * Only flags if handle state is IR_HS_FREED / MAYBE_FREED / TRANSFERRED.
 */

typedef struct {
    int *ids;
    int count;
    int cap;
} UafReportSet;

static bool urs_has(UafReportSet *s, int id) {
    for (int i = 0; i < s->count; i++) if (s->ids[i] == id) return true;
    return false;
}

static void urs_add(UafReportSet *s, int id) {
    if (s->count >= s->cap) {
        s->cap = s->cap < 8 ? 8 : s->cap * 2;
        int *ni = (int *)realloc(s->ids, s->cap * sizeof(int));
        if (ni) s->ids = ni; else return;
    }
    s->ids[s->count++] = id;
}

static void ir_check_expr_uaf(ZerCheck *zc, IRFunc *func, IRPathState *ps,
                               Node *expr, int line, UafReportSet *rs);

static void ir_check_ident_uaf(ZerCheck *zc, IRFunc *func, IRPathState *ps,
                                Node *expr, int line, UafReportSet *rs) {
    if (!expr) return;
    int root_local;
    const char *path;
    uint32_t path_len;
    if (ir_extract_compound_key(zc, func, expr,
                                 &root_local, &path, &path_len) != 0) return;
    if (urs_has(rs, root_local)) return;
    IRHandleInfo *h;
    if (path_len == 0) h = ir_find_handle(ps, root_local);
    else h = ir_find_compound_handle(ps, root_local, path, path_len);
    if (!h) {
        /* Try root-only when a compound key wasn't found */
        if (path_len > 0) h = ir_find_handle(ps, root_local);
    }
    if (h && ir_is_invalid(h)) {
        const char *name = (root_local >= 0 && root_local < func->local_count)
            ? func->locals[root_local].name : "?";
        int nlen = (root_local >= 0 && root_local < func->local_count)
            ? (int)func->locals[root_local].name_len : 1;
        ir_zc_error(zc, line,
            "use after free: '%.*s' is %s (freed at line %d)",
            nlen, name, ir_state_name(h->state), h->free_line);
        urs_add(rs, root_local);
    }
}

static void ir_check_expr_uaf(ZerCheck *zc, IRFunc *func, IRPathState *ps,
                               Node *expr, int line, UafReportSet *rs) {
    if (!expr) return;
    switch (expr->kind) {
    case NODE_IDENT:
        ir_check_ident_uaf(zc, func, ps, expr, line, rs);
        break;
    case NODE_FIELD:
        /* Field access reads the root — check prefix */
        ir_check_ident_uaf(zc, func, ps, expr, line, rs);
        ir_check_expr_uaf(zc, func, ps, expr->field.object, line, rs);
        break;
    case NODE_INDEX:
        ir_check_ident_uaf(zc, func, ps, expr, line, rs);
        ir_check_expr_uaf(zc, func, ps, expr->index_expr.object, line, rs);
        ir_check_expr_uaf(zc, func, ps, expr->index_expr.index, line, rs);
        break;
    case NODE_CALL:
        /* Don't check the callee (pool.get itself). Check args. */
        for (int i = 0; i < expr->call.arg_count; i++)
            ir_check_expr_uaf(zc, func, ps, expr->call.args[i], line, rs);
        /* Still recurse into callee for nested calls e.g. (freed.method)() */
        if (expr->call.callee && expr->call.callee->kind == NODE_FIELD) {
            /* Check the callee's object (pool.get — "pool" isn't tracked;
             * but `freed.some_method()` — check "freed"). Only walk object
             * chain, not the field access itself. */
            ir_check_expr_uaf(zc, func, ps, expr->call.callee->field.object,
                              line, rs);
        }
        break;
    case NODE_UNARY:
        /* & operator is capture, not read — skip. Otherwise check operand. */
        if (expr->unary.op == TOK_AMP) break;
        ir_check_expr_uaf(zc, func, ps, expr->unary.operand, line, rs);
        break;
    case NODE_BINARY:
        ir_check_expr_uaf(zc, func, ps, expr->binary.left, line, rs);
        ir_check_expr_uaf(zc, func, ps, expr->binary.right, line, rs);
        break;
    case NODE_ASSIGN:
        /* Both target and value — target may contain pool.get(h). */
        ir_check_expr_uaf(zc, func, ps, expr->assign.target, line, rs);
        ir_check_expr_uaf(zc, func, ps, expr->assign.value, line, rs);
        break;
    case NODE_TYPECAST:
        ir_check_expr_uaf(zc, func, ps, expr->typecast.expr, line, rs);
        break;
    case NODE_SLICE:
        ir_check_expr_uaf(zc, func, ps, expr->slice.object, line, rs);
        ir_check_expr_uaf(zc, func, ps, expr->slice.start, line, rs);
        ir_check_expr_uaf(zc, func, ps, expr->slice.end, line, rs);
        break;
    case NODE_ORELSE:
        ir_check_expr_uaf(zc, func, ps, expr->orelse.expr, line, rs);
        /* Fallback is a separate branch — not part of success flow */
        break;
    case NODE_INTRINSIC:
        for (int i = 0; i < expr->intrinsic.arg_count; i++)
            ir_check_expr_uaf(zc, func, ps, expr->intrinsic.args[i], line, rs);
        break;
    case NODE_STRUCT_INIT:
        for (int i = 0; i < expr->struct_init.field_count; i++)
            ir_check_expr_uaf(zc, func, ps, expr->struct_init.fields[i].value,
                              line, rs);
        break;
    default:
        break;
    }
}

/* ================================================================
 * Defer body scanning (Phase C3)
 *
 * Ported from zercheck.c:343-388 (defer_stmt_is_free + defer_scan_all_frees).
 * Walks a defer body's AST to find free calls. When a handle is freed
 * inside a defer, it is covered at function exit — not a leak.
 *
 * Conservative: scans EVERY defer body in the function, not just those
 * on the specific exit path. Matches zercheck.c behavior. A handle
 * freed in any defer is considered potentially covered.
 * ================================================================ */

/* Check if an AST statement is a free call. Returns the argument
 * expression (the thing being freed) or NULL. Recognizes:
 *   - pool.free(x)    (NODE_FIELD callee, method "free")
 *   - slab.free(x)    (same — dispatches via builtin)
 *   - pool.free_ptr(x) / slab.free_ptr(x)
 *   - bare free(x)    (plain cstdlib from cinclude)
 *   - Task.free(x) / Task.free_ptr(x)
 */
static Node *ir_defer_free_arg(Node *node) {
    if (!node) return NULL;
    if (node->kind != NODE_EXPR_STMT || !node->expr_stmt.expr) return NULL;
    Node *call = node->expr_stmt.expr;
    if (call->kind != NODE_CALL || call->call.arg_count == 0) return NULL;

    Node *callee = call->call.callee;
    if (callee && callee->kind == NODE_FIELD) {
        const char *m = callee->field.field_name;
        uint32_t ml = (uint32_t)callee->field.field_name_len;
        if ((ml == 4 && memcmp(m, "free", 4) == 0) ||
            (ml == 8 && memcmp(m, "free_ptr", 8) == 0))
            return call->call.args[0];
    }
    if (callee && callee->kind == NODE_IDENT &&
        callee->ident.name_len == 4 &&
        memcmp(callee->ident.name, "free", 4) == 0)
        return call->call.args[0];
    return NULL;
}

/* Walk a defer body. For each free found, resolve the argument to a
 * tracked handle (bare or compound) and mark it FREED at defer_line.
 * Recursively walks NODE_BLOCK so multi-statement defers are covered. */
static void ir_defer_scan_frees(ZerCheck *zc, IRFunc *func, IRPathState *ps,
                                 Node *body, int defer_line) {
    if (!body) return;

    /* Try this node as a free statement */
    Node *farg = ir_defer_free_arg(body);
    if (farg) {
        int root_local;
        const char *path;
        uint32_t path_len;
        if (ir_extract_compound_key(zc, func, farg,
                                     &root_local, &path, &path_len) == 0) {
            IRHandleInfo *h;
            if (path_len == 0) h = ir_find_handle(ps, root_local);
            else h = ir_find_compound_handle(ps, root_local, path, path_len);
            if (h && (h->state == IR_HS_ALIVE ||
                      h->state == IR_HS_MAYBE_FREED)) {
                h->state = IR_HS_FREED;
                h->free_line = defer_line;
            }
        }
    }

    /* Recurse into block statements */
    if (body->kind == NODE_BLOCK) {
        for (int i = 0; i < body->block.stmt_count; i++) {
            ir_defer_scan_frees(zc, func, ps, body->block.stmts[i], defer_line);
        }
    }
}

/* Propagate state through aliases sharing alloc_id. When `target` is
 * marked FREED or TRANSFERRED, other entities (bare or compound) with
 * the same alloc_id represent the same underlying allocation and must
 * also be marked. */
static void ir_propagate_alias_state(IRPathState *ps, IRHandleInfo *target,
                                      IRHandleState new_state, int line) {
    int aid = target->alloc_id;
    if (aid == 0) return; /* untracked — no aliasing info */
    for (int i = 0; i < ps->handle_count; i++) {
        IRHandleInfo *h = &ps->handles[i];
        if (h == target) continue;
        if (h->alloc_id == aid && !ir_is_invalid(h)) {
            h->state = new_state;
            h->free_line = line;
        }
    }
}

/* ================================================================
 * Instruction Analysis — process one IR instruction
 * ================================================================ */

static void ir_check_inst(ZerCheck *zc, IRPathState *ps, IRInst *inst, IRFunc *func) {
    (void)zc; /* used for error reporting */

    switch (inst->op) {

    /* Allocation → register handle as ALIVE with POOL color (Phase D1)
     * Phase D5: slab.alloc banned in interrupt handlers and in @critical
     * blocks — calloc/realloc (slab growth) may deadlock if interrupted.
     * Pool.alloc is fine (no malloc underneath), so only check slab here. */
    case IR_POOL_ALLOC:
    case IR_SLAB_ALLOC:
    case IR_SLAB_ALLOC_PTR: {
        /* Phase D5: ISR + @critical bans for slab-backed allocation */
        if (inst->op == IR_SLAB_ALLOC || inst->op == IR_SLAB_ALLOC_PTR) {
            if (func->is_interrupt) {
                ir_zc_error(zc, inst->source_line,
                    "slab.alloc() banned in interrupt handler — "
                    "calloc may deadlock. Use Pool(T, N) with fixed capacity.");
            } else if (ps->critical_depth > 0) {
                ir_zc_error(zc, inst->source_line,
                    "slab.alloc() banned inside @critical block — "
                    "calloc may deadlock with interrupts disabled.");
            }
        }

        if (inst->dest_local >= 0) {
            IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
            if (h) {
                /* Check for overwrite of alive handle (leak). Skip temp
                 * locals — they represent intermediate values in loops
                 * and control flow; their reassignment is expected and
                 * the "real" allocation flow lives on user-visible
                 * variables via alias chains. */
                if (h->state == IR_HS_ALIVE &&
                    inst->dest_local < func->local_count &&
                    !func->locals[inst->dest_local].is_temp) {
                    ir_zc_error(zc, inst->source_line,
                        "handle %%%d overwritten while alive — previous allocation leaked",
                        inst->dest_local);
                }
                h->state = IR_HS_ALIVE;
                h->alloc_line = inst->source_line;
                h->alloc_id = inst->dest_local; /* simple: local_id = alloc_id */
                h->source_color = ZC_COLOR_POOL;
            }
        }
        break;
    }

    /* Phase D5: @critical block entry/exit — affects subsequent alloc/spawn checks */
    case IR_CRITICAL_BEGIN:
        ps->critical_depth++;
        break;
    case IR_CRITICAL_END:
        if (ps->critical_depth > 0) ps->critical_depth--;
        break;

    /* Phase E: IR_NOP wrapping NODE_SPAWN. Per emitter.c, spawn emits
     * IR_NOP with inst->expr = NODE_SPAWN (passthrough path). Reroute
     * to IR_SPAWN's logic so D3 ThreadHandle tracking fires. */
    case IR_NOP: {
        if (!inst->expr || inst->expr->kind != NODE_SPAWN) break;
        Node *sp = inst->expr;

        /* Phase D5: spawn bans */
        if (func->is_interrupt) {
            ir_zc_error(zc, inst->source_line,
                "spawn banned in interrupt handler — "
                "pthread_create with interrupts disabled is unsafe.");
        } else if (ps->critical_depth > 0) {
            ir_zc_error(zc, inst->source_line,
                "spawn banned inside @critical block — "
                "thread creation with interrupts disabled is unsafe.");
        }

        /* Transfer args (ownership to spawned thread) */
        for (int i = 0; i < sp->spawn_stmt.arg_count; i++) {
            Node *arg = sp->spawn_stmt.args[i];
            if (!arg || arg->kind != NODE_IDENT) continue;
            int arg_local = ir_find_local_exact_first(func,
                arg->ident.name, (uint32_t)arg->ident.name_len);
            if (arg_local < 0) continue;
            IRHandleInfo *h = ir_find_handle(ps, arg_local);
            if (h) h->state = IR_HS_TRANSFERRED;
        }

        /* Phase D3/E: scoped spawn with ThreadHandle — tracked by name.
         * No IR local exists (emitter handles pthread_t emission directly),
         * so track via name-based IRThreadTrack set on IRPathState. */
        if (sp->spawn_stmt.handle_name && sp->spawn_stmt.handle_name_len > 0) {
            ir_add_thread(ps, sp->spawn_stmt.handle_name,
                (uint32_t)sp->spawn_stmt.handle_name_len, inst->source_line);
        }
        break;
    }

    /* Phase E: IR_COPY is emitted for local-to-local copies (e.g., when
     * unwrapping ?Handle to bare Handle via orelse). Propagate handle
     * state and alloc_id from src1_local to dest_local so the dest is
     * a tracked alias of the source. */
    case IR_COPY: {
        if (inst->dest_local < 0 || inst->src1_local < 0) break;

        /* Phase E: move struct assignment — `Token b = a` transfers
         * ownership. src becomes TRANSFERRED, dest becomes ALIVE with
         * a fresh alloc_id. Detected by the src type being a move
         * struct (or containing move fields). */
        if (inst->dest_local < func->local_count &&
            inst->src1_local < func->local_count) {
            Type *src_type = func->locals[inst->src1_local].type;
            if (ir_should_track_move(src_type)) {
                IRHandleInfo *src_h = ir_find_handle(ps, inst->src1_local);
                if (src_h && src_h->state == IR_HS_TRANSFERRED) {
                    ir_zc_error(zc, inst->source_line,
                        "use after move: '%.*s' ownership transferred at line %d",
                        (int)func->locals[inst->src1_local].name_len,
                        func->locals[inst->src1_local].name,
                        src_h->free_line);
                }
                if (!src_h) src_h = ir_add_handle(ps, inst->src1_local);
                if (src_h) {
                    src_h->state = IR_HS_TRANSFERRED;
                    src_h->free_line = inst->source_line;
                }
                IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                if (dst_h) {
                    dst_h->state = IR_HS_ALIVE;
                    dst_h->alloc_line = inst->source_line;
                    dst_h->alloc_id = _ir_next_alloc_id++;
                }
                break;
            }
        }

        IRHandleInfo *src_h = ir_find_handle(ps, inst->src1_local);
        if (!src_h) break;
        /* Error if source is invalid */
        if (ir_is_invalid(src_h)) {
            ir_zc_error(zc, inst->source_line,
                "use of %s handle %%%d",
                ir_state_name(src_h->state), inst->src1_local);
        }
        /* Alias: dest inherits source's alloc_id and state */
        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
        if (dst_h) {
            dst_h->state = src_h->state;
            dst_h->alloc_line = src_h->alloc_line;
            dst_h->alloc_id = src_h->alloc_id;
            dst_h->source_color = src_h->source_color;
            dst_h->is_thread_handle = src_h->is_thread_handle;
        }
        break;
    }

    /* Phase D1: Arena allocation → ARENA color. Skipped in leak detection
     * because arena.reset() frees everything wholesale. */
    case IR_ARENA_ALLOC:
    case IR_ARENA_ALLOC_SLICE: {
        if (inst->dest_local >= 0) {
            IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
            if (h) {
                h->state = IR_HS_ALIVE;
                h->alloc_line = inst->source_line;
                h->alloc_id = inst->dest_local;
                h->source_color = ZC_COLOR_ARENA;
            }
        }
        break;
    }

    /* Free → mark as FREED, check for double-free */
    case IR_POOL_FREE:
    case IR_SLAB_FREE:
    case IR_SLAB_FREE_PTR: {
        int target = inst->handle_local;
        IRHandleInfo *h = NULL;

        /* First try bare local (decomposed path) */
        if (target >= 0) {
            h = ir_find_handle(ps, target);
        }

        /* Phase B3: if bare lookup failed AND inst->expr is a free call
         * with a compound argument (e.g. pool.free(s.handle)), extract
         * the compound key and look it up. */
        if (!h && inst->expr && inst->expr->kind == NODE_CALL &&
            inst->expr->call.arg_count >= 1) {
            Node *arg = inst->expr->call.args[0];
            int root_local;
            const char *path;
            uint32_t path_len;
            if (ir_extract_compound_key(zc, func, arg,
                                         &root_local, &path, &path_len) == 0) {
                h = ir_find_compound_handle(ps, root_local, path, path_len);
                if (h) target = root_local;  /* for error messages */
            }
        }

        if (h) {
            if (h->state == IR_HS_FREED) {
                ir_zc_error(zc, inst->source_line,
                    "double free: %%%d already freed at line %d",
                    target, h->free_line);
            } else if (h->state == IR_HS_MAYBE_FREED) {
                ir_zc_error(zc, inst->source_line,
                    "freeing %%%d which may already be freed",
                    target);
            } else if (h->state == IR_HS_TRANSFERRED) {
                ir_zc_error(zc, inst->source_line,
                    "freeing %%%d which was already transferred",
                    target);
            }
            h->state = IR_HS_FREED;
            h->free_line = inst->source_line;

            /* Mark aliases (bare or compound) with same alloc_id as FREED —
             * handled uniformly via ir_propagate_alias_state. */
            ir_propagate_alias_state(ps, h, IR_HS_FREED, inst->source_line);
        }
        break;
    }

    /* Get → check handle is ALIVE (UAF check) */
    case IR_POOL_GET: {
        int target = inst->handle_local;
        if (target >= 0) {
            IRHandleInfo *h = ir_find_handle(ps, target);
            if (h && ir_is_invalid(h)) {
                ir_zc_error(zc, inst->source_line,
                    "use after free: %%%d is %s (freed at line %d)",
                    target, ir_state_name(h->state), h->free_line);
            }
        }
        break;
    }

    /* Phase B3: FIELD_READ — check if the read expression (including any
     * compound prefix) has been freed. Mirrors zercheck.c:1480 BUG-463 logic:
     * for `s.handle.data`, check the full key first, then walk up to "s.handle",
     * then "s" — any freed prefix means the subsequent read is UAF.
     *
     * IR_FIELD_READ produces one level of field access at a time. The full
     * compound expression lives on inst->expr (NODE_FIELD), from which we
     * can extract all prefixes. */
    /* Phase E: Index reads (`arr[i]`, `ptr[0]`) — check base for UAF.
     * For interior pointers (field_ptr = &b.c; free(b); field_ptr[0])
     * the base local shares alloc_id with b; when b is freed, the base
     * is FREED too and reading it should trigger UAF. */
    case IR_INDEX_READ: {
        if (inst->expr) {
            UafReportSet rs = {0};
            ir_check_expr_uaf(zc, func, ps, inst->expr, inst->source_line, &rs);
            free(rs.ids);
        }
        break;
    }

    case IR_FIELD_READ: {
        if (inst->expr && inst->expr->kind == NODE_FIELD) {
            /* Walk from full expression up to root, checking each prefix. */
            Node *cur = inst->expr;
            while (cur) {
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, cur,
                                             &root_local, &path, &path_len) == 0) {
                    IRHandleInfo *h;
                    if (path_len == 0) {
                        h = ir_find_handle(ps, root_local);
                    } else {
                        h = ir_find_compound_handle(ps, root_local, path, path_len);
                    }
                    if (h && ir_is_invalid(h)) {
                        if (path_len == 0) {
                            ir_zc_error(zc, inst->source_line,
                                "use after free: local %%%d is %s (freed at line %d)",
                                root_local, ir_state_name(h->state), h->free_line);
                        } else {
                            ir_zc_error(zc, inst->source_line,
                                "use after free: compound '%.*s' on local %%%d is %s (freed at line %d)",
                                (int)path_len, path, root_local,
                                ir_state_name(h->state), h->free_line);
                        }
                        break; /* found — don't report parent prefixes too */
                    }
                }
                /* Step up one level in the chain */
                if (cur->kind == NODE_FIELD) cur = cur->field.object;
                else if (cur->kind == NODE_INDEX) cur = cur->index_expr.object;
                else break;
            }
        }
        break;
    }

    /* Assign → alias tracking or move transfer.
     * Phase B1: move struct types get TRANSFER semantics (not alias).
     * `Token b = a` transfers ownership: a → TRANSFERRED, b → ALIVE (new id).
     * Phase E: detect builtin method calls (pool.alloc, slab.alloc, get, etc.)
     * inside the assign's expression — these are collapsed into IR_ASSIGN
     * per ir_lower.c Phase 8d and must be recognized here to track state. */
    case IR_ASSIGN: {
        /* Phase E: move struct field-write reset BEFORE UAF check.
         * `m.code = 1` where m is a move struct currently TRANSFERRED
         * resets m to ALIVE — writing to a field is re-initialization,
         * not a use. Models CFG-loop semantics where each iteration
         * declares a fresh local `Msg m;` (AST zercheck sees fresh scope).
         * Must run BEFORE UAF walker so the target's use-as-read check
         * doesn't flag a freshly reset variable. */
        if (inst->expr && inst->expr->kind == NODE_ASSIGN &&
            inst->expr->assign.target &&
            inst->expr->assign.target->kind == NODE_FIELD) {
            Node *root = inst->expr->assign.target;
            while (root && root->kind == NODE_FIELD) root = root->field.object;
            while (root && root->kind == NODE_INDEX) root = root->index_expr.object;
            if (root && root->kind == NODE_IDENT) {
                int root_local = ir_find_local_exact_first(func,
                    root->ident.name, (uint32_t)root->ident.name_len);
                if (root_local >= 0 && root_local < func->local_count &&
                    ir_should_track_move(func->locals[root_local].type)) {
                    IRHandleInfo *rh = ir_find_handle(ps, root_local);
                    if (rh && rh->state == IR_HS_TRANSFERRED) {
                        rh->state = IR_HS_ALIVE;
                        rh->alloc_line = inst->source_line;
                    }
                }
            }
        }

        /* Phase E: generic UAF walker for any use inside the expression.
         * Catches patterns like `pool.get(h).id = 5` where IR_ASSIGN's
         * expr is NODE_ASSIGN wrapping NODE_FIELD(NODE_CALL(pool.get, h), id).
         * Walks recursively and flags any use of a FREED/TRANSFERRED handle. */
        if (inst->expr) {
            UafReportSet rs = {0};
            ir_check_expr_uaf(zc, func, ps, inst->expr, inst->source_line, &rs);
            free(rs.ids);
        }

        /* Phase E: NODE_ASSIGN(target, value) passthrough — field/index
         * writes. If target root is a global or escape-detector positive,
         * mark the RHS local escaped (suppresses leak detection).
         * Also registers compound handle for `s.field = h` pattern so
         * `s.field` and `h` share alloc_id for UAF tracking. */
        if (inst->expr && inst->expr->kind == NODE_ASSIGN) {
            Node *target_expr = inst->expr->assign.target;
            Node *value_expr = inst->expr->assign.value;
            int rhs_local = ir_find_value_local(func, value_expr);
            if (target_expr && rhs_local >= 0 &&
                ir_target_root_escapes(zc, target_expr)) {
                ir_mark_local_escaped(ps, rhs_local);
            }
            /* Phase E: target untrackable (variable-index array store,
             * complex expression). Value escapes because we can't track
             * through dynamic index. Mirrors zercheck.c:1460 pattern.
             * Example: `handles[i] = mh` where i is a variable. */
            if (target_expr && rhs_local >= 0 &&
                target_expr->kind == NODE_INDEX &&
                target_expr->index_expr.index &&
                target_expr->index_expr.index->kind != NODE_INT_LIT) {
                ir_mark_local_escaped(ps, rhs_local);
            }
            /* Compound key registration: `container.field = h` */
            if (target_expr && rhs_local >= 0) {
                IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
                if (rh && rh->state == IR_HS_ALIVE) {
                    int root_local;
                    const char *path;
                    uint32_t path_len;
                    if (ir_extract_compound_key(zc, func, target_expr,
                                                 &root_local, &path,
                                                 &path_len) == 0 &&
                        path_len > 0) {
                        IRHandleInfo *ch = ir_add_compound_handle(ps,
                            root_local, path, path_len);
                        if (ch) {
                            ch->state = IR_HS_ALIVE;
                            ch->alloc_line = rh->alloc_line;
                            ch->alloc_id = rh->alloc_id;
                            ch->source_color = rh->source_color;
                        }
                    }
                }
            }
        }
        /* Phase E: recognize pool/slab builtin method calls in the RHS.
         * Handled shapes:
         *   h = pool.alloc()                      → unwrap to NODE_CALL
         *   h = pool.alloc() orelse return        → unwrap NODE_ORELSE first
         *   x = pool.get(h)                       → UAF check on h
         *   h = mh orelse return                  → alias dest to source ident
         *   (pool.free is a statement, not assign — handled in IR_CALL) */
        if (inst->dest_local >= 0 && inst->expr) {
            Node *rhs = ir_unwrap_alloc_expr(inst->expr);

            /* Orelse-wrapped ident: `h = mh orelse return`. The primary
             * is a NODE_IDENT referencing a tracked local. Alias the
             * destination to source, mirroring the bare-ident path below. */
            if (inst->expr->kind == NODE_ORELSE && rhs && rhs->kind == NODE_IDENT) {
                int src_local = ir_find_local_exact_first(func,
                    rhs->ident.name, (uint32_t)rhs->ident.name_len);
                if (src_local >= 0) {
                    IRHandleInfo *src_h = ir_find_handle(ps, src_local);
                    if (src_h) {
                        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                        if (dst_h) {
                            dst_h->state = src_h->state;
                            dst_h->alloc_line = src_h->alloc_line;
                            dst_h->alloc_id = src_h->alloc_id;
                            dst_h->source_color = src_h->source_color;
                            dst_h->is_thread_handle = src_h->is_thread_handle;
                        }
                    }
                }
                break;
            }

            /* Phase E: @ptrcast alias tracking. `*opaque raw = @ptrcast(*opaque, s)`
             * creates an alias: raw should share alloc_id with s. When s is
             * freed, raw becomes FREED via ir_propagate_alias_state. The
             * type arg is stored in type_arg, not args — args[0] is the src.
             *
             * Auto-register param source (marked escaped=true so the handle
             * entries don't flag as leaks, but FuncSummary observes FREED
             * state if free_ptr is later called on an alias). Needed to
             * propagate cross-function `destroy_cat(opaque)` patterns where
             * the opaque param is ptrcast then freed. */
            if (rhs && rhs->kind == NODE_INTRINSIC &&
                rhs->intrinsic.name && rhs->intrinsic.name_len == 7 &&
                memcmp(rhs->intrinsic.name, "ptrcast", 7) == 0 &&
                rhs->intrinsic.arg_count >= 1) {
                Node *src = rhs->intrinsic.args[0];
                if (src && src->kind == NODE_IDENT) {
                    int src_local = ir_find_local_exact_first(func,
                        src->ident.name, (uint32_t)src->ident.name_len);
                    if (src_local >= 0) {
                        IRHandleInfo *src_h = ir_find_handle(ps, src_local);
                        if (!src_h && src_local < func->local_count &&
                            func->locals[src_local].is_param) {
                            src_h = ir_add_handle(ps, src_local);
                            if (src_h) {
                                src_h->state = IR_HS_ALIVE;
                                src_h->alloc_line = inst->source_line;
                                src_h->alloc_id = _ir_next_alloc_id++;
                                src_h->source_color = ZC_COLOR_UNKNOWN;
                                src_h->escaped = true;  /* external input */
                            }
                        }
                        if (src_h) {
                            IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                            if (dst_h) {
                                dst_h->state = src_h->state;
                                dst_h->alloc_line = src_h->alloc_line;
                                dst_h->alloc_id = src_h->alloc_id;
                                dst_h->source_color = src_h->source_color;
                                /* Propagate escaped so aliases don't leak */
                                dst_h->escaped = src_h->escaped;
                            }
                        }
                    }
                }
            }

            /* Phase E: interior pointer tracking. `*T field_ptr = &b.c`
             * lowers to IR_ASSIGN with expr = NODE_UNARY(TOK_AMP, NODE_FIELD(b, c)).
             * field_ptr should share alloc_id with b so when b is freed,
             * field_ptr is also flagged. Walk &expr down to root ident. */
            if (rhs && rhs->kind == NODE_UNARY && rhs->unary.op == TOK_AMP) {
                Node *target = rhs->unary.operand;
                /* Walk field/index chain to the root ident */
                while (target) {
                    if (target->kind == NODE_FIELD) target = target->field.object;
                    else if (target->kind == NODE_INDEX) target = target->index_expr.object;
                    else break;
                }
                if (target && target->kind == NODE_IDENT) {
                    int base_local = ir_find_local_exact_first(func,
                        target->ident.name, (uint32_t)target->ident.name_len);
                    if (base_local >= 0) {
                        IRHandleInfo *base_h = ir_find_handle(ps, base_local);
                        if (base_h && base_h->alloc_id != 0) {
                            IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                            if (dst_h) {
                                dst_h->state = base_h->state;
                                dst_h->alloc_line = base_h->alloc_line;
                                dst_h->alloc_id = base_h->alloc_id;
                                dst_h->source_color = base_h->source_color;
                            }
                        }
                    }
                }
                /* Don't break — continue alias path if RHS is just an ident too */
            }

            if (rhs && rhs->kind == NODE_CALL) {
                IRMethodKind mc = ir_classify_method_call(rhs);
                if (mc == IRMC_ALLOC || mc == IRMC_ALLOC_PTR) {
                    IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                    if (h) {
                        if (h->state == IR_HS_ALIVE &&
                            inst->dest_local < func->local_count &&
                            !func->locals[inst->dest_local].is_temp) {
                            ir_zc_error(zc, inst->source_line,
                                "handle %%%d overwritten while alive — previous leaked",
                                inst->dest_local);
                        }
                        h->state = IR_HS_ALIVE;
                        h->alloc_line = inst->source_line;
                        h->alloc_id = inst->dest_local;
                        h->source_color = ZC_COLOR_POOL;
                    }
                    break;
                } else if (mc == IRMC_ARENA_ALLOC) {
                    IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                    if (h) {
                        h->state = IR_HS_ALIVE;
                        h->alloc_line = inst->source_line;
                        h->alloc_id = inst->dest_local;
                        h->source_color = ZC_COLOR_ARENA;
                    }
                    break;
                } else if (mc == IRMC_GET) {
                    /* pool.get(h) — UAF check on h. Argument resolution. */
                    if (rhs->call.arg_count >= 1) {
                        Node *arg = rhs->call.args[0];
                        int root_local;
                        const char *path;
                        uint32_t path_len;
                        if (ir_extract_compound_key(zc, func, arg,
                                                     &root_local, &path, &path_len) == 0) {
                            IRHandleInfo *h;
                            if (path_len == 0) h = ir_find_handle(ps, root_local);
                            else h = ir_find_compound_handle(ps, root_local, path, path_len);
                            if (h && ir_is_invalid(h)) {
                                ir_zc_error(zc, inst->source_line,
                                    "use after free: local %%%d is %s (freed at line %d)",
                                    root_local, ir_state_name(h->state), h->free_line);
                            }
                        }
                    }
                    /* Fall through — dest may still need tracking if get result is pointer */
                }
            }
            /* If source is an ident that's a tracked handle, create alias */
            if (inst->expr->kind == NODE_IDENT) {
                int src_local = ir_find_local_exact_first(func,
                    inst->expr->ident.name,
                    (uint32_t)inst->expr->ident.name_len);
                if (src_local >= 0) {
                    IRHandleInfo *src_h = ir_find_handle(ps, src_local);
                    Type *src_type = (src_local < func->local_count)
                        ? func->locals[src_local].type : NULL;
                    bool is_move = ir_should_track_move(src_type);

                    if (is_move) {
                        /* Move transfer: source → TRANSFERRED, dest → new ALIVE */
                        if (src_h && src_h->state == IR_HS_TRANSFERRED) {
                            ir_zc_error(zc, inst->source_line,
                                "use of transferred value (local %%%d) — ownership already moved",
                                src_local);
                        } else if (src_h && ir_is_invalid(src_h)) {
                            ir_zc_error(zc, inst->source_line,
                                "use of %s handle %%%d",
                                ir_state_name(src_h->state), src_local);
                        }
                        /* Ensure source exists as handle so we can mark it */
                        if (!src_h) src_h = ir_add_handle(ps, src_local);
                        if (src_h) {
                            src_h->state = IR_HS_TRANSFERRED;
                            src_h->free_line = inst->source_line;
                        }
                        /* Destination takes new ownership identity */
                        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                        if (dst_h) {
                            dst_h->state = IR_HS_ALIVE;
                            dst_h->alloc_line = inst->source_line;
                            dst_h->alloc_id = _ir_next_alloc_id++;
                        }
                    } else {
                        /* Non-move: regular alias */
                        if (src_h && src_h->state == IR_HS_ALIVE) {
                            IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                            if (dst_h) {
                                dst_h->state = IR_HS_ALIVE;
                                dst_h->alloc_line = src_h->alloc_line;
                                dst_h->alloc_id = src_h->alloc_id;
                                dst_h->source_color = src_h->source_color;
                                dst_h->is_thread_handle = src_h->is_thread_handle;
                            }
                        }
                        /* Check use of invalid handle */
                        if (src_h && ir_is_invalid(src_h)) {
                            ir_zc_error(zc, inst->source_line,
                                "use of %s handle %%%d",
                                ir_state_name(src_h->state), src_local);
                        }
                    }
                }
            }
        }
        break;
    }

    /* Return → check no handles leaked, mark terminated.
     * Phase B1+B2: three shapes supported —
     *   (a) `return h` ident — move transfer OR mark escaped
     *   (b) `return opt orelse h` — walk into fallback, mark escaped
     *   (c) `return { .field = h, ... }` struct init — walk fields,
     *       mark each embedded handle escaped
     */
    case IR_RETURN: {
        Node *rexpr = inst->expr;
        /* Unwrap orelse: `return opt orelse fallback_h` — check BOTH
         * the primary (a tracked local reaches the return) and the
         * fallback (a handle used on null path). */
        Node *primary = rexpr;
        Node *fallback = NULL;
        if (rexpr && rexpr->kind == NODE_ORELSE) {
            primary = rexpr->orelse.expr;
            fallback = rexpr->orelse.fallback;
        }

        /* Phase E: ir_lower sets src1_local for simple `return ident` and
         * keeps expr NULL. Cover that case by treating src1_local as the
         * returned local when expr is missing. */
        int ret_local_direct = -1;
        if (!rexpr && inst->src1_local >= 0 &&
            inst->src1_local < func->local_count) {
            ret_local_direct = inst->src1_local;
            IRHandleInfo *h = ir_find_handle(ps, ret_local_direct);
            Type *ret_type = func->locals[ret_local_direct].type;
            if (ir_should_track_move(ret_type)) {
                if (h && ir_is_invalid(h)) {
                    ir_zc_error(zc, inst->source_line,
                        "returning %s value (local %%%d)",
                        ir_state_name(h->state), ret_local_direct);
                }
                if (!h) h = ir_add_handle(ps, ret_local_direct);
                if (h) {
                    h->state = IR_HS_TRANSFERRED;
                    h->free_line = inst->source_line;
                    h->escaped = true;
                }
            } else {
                if (h && ir_is_invalid(h)) {
                    ir_zc_error(zc, inst->source_line,
                        "returning %s pointer (local %%%d, freed at line %d) — "
                        "caller would receive dangling pointer",
                        ir_state_name(h->state), ret_local_direct, h->free_line);
                }
                if (h) h->escaped = true;
            }
        }

        /* Case (a): direct ident return */
        if (primary && primary->kind == NODE_IDENT) {
            int ret_local = ir_find_local_exact_first(func,
                primary->ident.name,
                (uint32_t)primary->ident.name_len);
            if (ret_local >= 0 && ret_local < func->local_count) {
                IRHandleInfo *h = ir_find_handle(ps, ret_local);
                Type *ret_type = func->locals[ret_local].type;
                if (ir_should_track_move(ret_type)) {
                    if (h && ir_is_invalid(h)) {
                        ir_zc_error(zc, inst->source_line,
                            "returning %s value (local %%%d)",
                            ir_state_name(h->state), ret_local);
                    }
                    if (!h) h = ir_add_handle(ps, ret_local);
                    if (h) {
                        h->state = IR_HS_TRANSFERRED;
                        h->free_line = inst->source_line;
                        h->escaped = true;
                    }
                } else {
                    /* Phase C2 (9c): returning a freed pointer. Any handle
                     * in a FREED/MAYBE_FREED/TRANSFERRED state is unsafe to
                     * hand to the caller. This catches `free(p); return p;`
                     * and any alias of a freed allocation. */
                    if (h && ir_is_invalid(h)) {
                        ir_zc_error(zc, inst->source_line,
                            "returning %s pointer (local %%%d, freed at line %d) — "
                            "caller would receive dangling pointer",
                            ir_state_name(h->state), ret_local, h->free_line);
                    }
                    if (h) h->escaped = true;
                }
            }
        }

        /* Case (b): orelse fallback that returns a handle —
         * ident or further nested — mark escaped. */
        if (fallback) {
            int fb_local = ir_find_value_local(func, fallback);
            if (fb_local >= 0) ir_mark_local_escaped(ps, fb_local);
        }

        /* Case (c): struct init — walk fields, mark embedded handles. */
        if (primary && primary->kind == NODE_STRUCT_INIT) {
            for (int fi = 0; fi < primary->struct_init.field_count; fi++) {
                Node *fv = primary->struct_init.fields[fi].value;
                int fv_local = ir_find_value_local(func, fv);
                if (fv_local >= 0) ir_mark_local_escaped(ps, fv_local);
            }
        }

        ps->terminated = true;
        break;
    }

    /* Phase C1 + C2: IR_CALL — apply callee's FuncSummary at the call
     * site, or recognize extern alloc/free via signature heuristic.
     *
     * Resolution order:
     *   1. FuncSummary built by zc_ir_build_summary / zercheck.c
     *   2. Signature heuristic for extern alloc (returns pointer)
     *      → register dest local as ALIVE
     *   3. Signature heuristic for extern free (void fn with *opaque arg0)
     *      → mark arg[0] local FREED
     *
     * Call site arg resolution tries two shapes:
     *   - decomposed: inst->call_arg_locals[i] gives the local directly
     *   - passthrough: inst->args[i] is an AST ident → find_local
     */
    case IR_CALL: {
        /* Phase E: generic UAF walker on the call args. Catches
         * use_ptr(freed_ident) / func(&freed.field) etc. */
        if (inst->expr) {
            UafReportSet rs = {0};
            ir_check_expr_uaf(zc, func, ps, inst->expr, inst->source_line, &rs);
            free(rs.ids);
        }
        /* Phase D3/E: ThreadHandle.join() — mark thread as joined.
         * ThreadHandles don't have IR locals (emitter owns their
         * pthread_t decl), so tracking is by name via IRThreadTrack. */
        if (inst->expr && inst->expr->kind == NODE_CALL &&
            inst->expr->call.callee &&
            inst->expr->call.callee->kind == NODE_FIELD) {
            Node *fld = inst->expr->call.callee;
            if (fld->field.field_name_len == 4 &&
                memcmp(fld->field.field_name, "join", 4) == 0 &&
                fld->field.object &&
                fld->field.object->kind == NODE_IDENT) {
                IRThreadTrack *t = ir_find_thread(ps,
                    fld->field.object->ident.name,
                    (uint32_t)fld->field.object->ident.name_len);
                if (t) {
                    if (t->joined) {
                        ir_zc_error(zc, inst->source_line,
                            "ThreadHandle '%.*s' already joined — "
                            "join consumes the handle, cannot join twice",
                            (int)t->name_len, t->name);
                    }
                    t->joined = true;
                }
            }
        }

        /* Phase E: move struct ownership transfer on function call args.
         * When `consume(f)` is called and f is a move struct (or contains
         * move struct fields), the argument transfers ownership and the
         * caller's local becomes TRANSFERRED. Subsequent use = UAF.
         *
         * Also handles `process(&k)` — taking address of move struct and
         * passing to function conservatively transfers ownership. */
        if (inst->expr && inst->expr->kind == NODE_CALL) {
            Node *call = inst->expr;
            for (int pi = 0; pi < call->call.arg_count; pi++) {
                Node *arg = call->call.args[pi];
                if (!arg) continue;
                /* Unwrap &expr to get root ident */
                Node *root = arg;
                if (root->kind == NODE_UNARY && root->unary.op == TOK_AMP)
                    root = root->unary.operand;
                if (!root || root->kind != NODE_IDENT) continue;
                int arg_local = ir_find_local_exact_first(func,
                    root->ident.name, (uint32_t)root->ident.name_len);
                if (arg_local < 0 || arg_local >= func->local_count) continue;
                Type *arg_type = func->locals[arg_local].type;
                if (!ir_should_track_move(arg_type)) continue;
                IRHandleInfo *h = ir_find_handle(ps, arg_local);
                if (h && h->state == IR_HS_TRANSFERRED) {
                    ir_zc_error(zc, inst->source_line,
                        "use after move: '%.*s' ownership transferred at line %d",
                        (int)func->locals[arg_local].name_len,
                        func->locals[arg_local].name, h->free_line);
                }
                if (!h) h = ir_add_handle(ps, arg_local);
                if (h) {
                    h->state = IR_HS_TRANSFERRED;
                    h->free_line = inst->source_line;
                }
            }
        }

        /* Phase E: recognize pool/slab/Task builtin methods in IR_CALL.
         * alloc/alloc_ptr when dest_local is set → register dest ALIVE.
         * free/free_ptr → mark handle FREED. get → UAF check. */
        if (inst->expr && inst->expr->kind == NODE_CALL) {
            IRMethodKind mc = ir_classify_method_call(inst->expr);
            /* Alloc via IR_CALL path (e.g., %1 = heap.alloc()) */
            if ((mc == IRMC_ALLOC || mc == IRMC_ALLOC_PTR) && inst->dest_local >= 0) {
                IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                if (h) {
                    if (h->state == IR_HS_ALIVE &&
                        inst->dest_local < func->local_count &&
                        !func->locals[inst->dest_local].is_temp) {
                        ir_zc_error(zc, inst->source_line,
                            "handle %%%d overwritten while alive — previous leaked",
                            inst->dest_local);
                    }
                    h->state = IR_HS_ALIVE;
                    h->alloc_line = inst->source_line;
                    h->alloc_id = inst->dest_local;
                    h->source_color = ZC_COLOR_POOL;
                }
                break;
            }
            if (mc == IRMC_ARENA_ALLOC && inst->dest_local >= 0) {
                IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                if (h) {
                    h->state = IR_HS_ALIVE;
                    h->alloc_line = inst->source_line;
                    h->alloc_id = inst->dest_local;
                    h->source_color = ZC_COLOR_ARENA;
                }
                break;
            }
            if ((mc == IRMC_FREE || mc == IRMC_FREE_PTR) &&
                inst->expr->call.arg_count >= 1) {
                Node *arg = inst->expr->call.args[0];
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, arg,
                                             &root_local, &path, &path_len) == 0) {
                    IRHandleInfo *h;
                    if (path_len == 0) h = ir_find_handle(ps, root_local);
                    else h = ir_find_compound_handle(ps, root_local, path, path_len);
                    /* Phase E: if freeing a param that was never registered
                     * as a handle, create one so FuncSummary can observe
                     * FREED state at return. Enables cross-function
                     * frees_param[i] inference. */
                    if (!h && path_len == 0 && root_local >= 0 &&
                        root_local < func->local_count &&
                        func->locals[root_local].is_param) {
                        h = ir_add_handle(ps, root_local);
                        if (h) {
                            h->state = IR_HS_ALIVE;
                            h->alloc_line = inst->source_line;
                            h->alloc_id = _ir_next_alloc_id++;
                            h->source_color = ZC_COLOR_UNKNOWN;
                        }
                    }
                    if (h) {
                        if (h->state == IR_HS_FREED) {
                            ir_zc_error(zc, inst->source_line,
                                "double free: local %%%d already freed at line %d",
                                root_local, h->free_line);
                        } else if (h->state == IR_HS_MAYBE_FREED) {
                            ir_zc_error(zc, inst->source_line,
                                "freeing local %%%d which may already be freed",
                                root_local);
                        } else if (h->state == IR_HS_TRANSFERRED) {
                            ir_zc_error(zc, inst->source_line,
                                "freeing local %%%d which was already transferred",
                                root_local);
                        }
                        h->state = IR_HS_FREED;
                        h->free_line = inst->source_line;
                        ir_propagate_alias_state(ps, h, IR_HS_FREED,
                                                  inst->source_line);
                    }
                }
                break;  /* Don't fall through to FuncSummary apply */
            }
            /* GET (as a statement, rare) — just UAF check */
            if (mc == IRMC_GET && inst->expr->call.arg_count >= 1) {
                Node *arg = inst->expr->call.args[0];
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, arg,
                                             &root_local, &path, &path_len) == 0) {
                    IRHandleInfo *h;
                    if (path_len == 0) h = ir_find_handle(ps, root_local);
                    else h = ir_find_compound_handle(ps, root_local, path, path_len);
                    if (h && ir_is_invalid(h)) {
                        ir_zc_error(zc, inst->source_line,
                            "use after free: local %%%d is %s (freed at line %d)",
                            root_local, ir_state_name(h->state), h->free_line);
                    }
                }
                break;
            }
        }

        /* Look up callee by name */
        const char *fn_name = inst->func_name;
        uint32_t fn_name_len = inst->func_name_len;
        if (!fn_name || fn_name_len == 0) break;

        FuncSummary *summary = NULL;
        for (int si = 0; si < zc->summary_count; si++) {
            if (zc->summaries[si].func_name_len == fn_name_len &&
                memcmp(zc->summaries[si].func_name, fn_name, fn_name_len) == 0) {
                summary = &zc->summaries[si]; break;
            }
        }

        /* Phase C2: if no summary, try signature heuristics on inst->expr
         * (the AST NODE_CALL). Covers extern malloc/free/destroy patterns. */
        if (!summary && inst->expr && inst->expr->kind == NODE_CALL) {
            Node *call = inst->expr;

            /* Extern alloc: register dest_local as ALIVE.
             * Phase D1: color as MALLOC if callee name is malloc/calloc/realloc,
             * otherwise UNKNOWN (cinclude custom allocator).
             * MALLOC requires matching free(); UNKNOWN is conservatively tracked
             * like POOL but can be escaped via returning. */
            if (inst->dest_local >= 0 && ir_is_extern_alloc_call(zc, call)) {
                IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                if (h) {
                    if (h->state == IR_HS_ALIVE &&
                        inst->dest_local < func->local_count &&
                        !func->locals[inst->dest_local].is_temp) {
                        ir_zc_error(zc, inst->source_line,
                            "handle %%%d overwritten while alive — previous allocation leaked",
                            inst->dest_local);
                    }
                    h->state = IR_HS_ALIVE;
                    h->alloc_line = inst->source_line;
                    h->alloc_id = _ir_next_alloc_id++;
                    /* Detect cstdlib allocators by name */
                    bool is_stdlib = false;
                    if (call->call.callee && call->call.callee->kind == NODE_IDENT) {
                        uint32_t nlen = (uint32_t)call->call.callee->ident.name_len;
                        const char *nm = call->call.callee->ident.name;
                        if ((nlen == 6 && memcmp(nm, "malloc", 6) == 0) ||
                            (nlen == 6 && memcmp(nm, "calloc", 6) == 0) ||
                            (nlen == 7 && memcmp(nm, "realloc", 7) == 0)) {
                            is_stdlib = true;
                        }
                    }
                    h->source_color = is_stdlib ? ZC_COLOR_MALLOC : ZC_COLOR_UNKNOWN;
                }
            }

            /* Extern free: mark first arg's local FREED. If no handle is
             * registered yet (param being freed directly, never saw alloc),
             * auto-register so state is tracked across the function. */
            if (ir_is_extern_free_call(zc, call) && call->call.arg_count >= 1) {
                Node *arg = call->call.args[0];
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, arg,
                                             &root_local, &path, &path_len) == 0) {
                    IRHandleInfo *h;
                    if (path_len == 0) h = ir_find_handle(ps, root_local);
                    else h = ir_find_compound_handle(ps, root_local, path, path_len);
                    if (!h && path_len == 0 && root_local >= 0 &&
                        root_local < func->local_count &&
                        func->locals[root_local].is_param) {
                        h = ir_add_handle(ps, root_local);
                        if (h) {
                            h->state = IR_HS_ALIVE;
                            h->alloc_line = inst->source_line;
                            h->alloc_id = _ir_next_alloc_id++;
                            h->source_color = ZC_COLOR_UNKNOWN;
                        }
                    }
                    if (h) {
                        if (h->state == IR_HS_FREED) {
                            ir_zc_error(zc, inst->source_line,
                                "double free: local %%%d already freed at line %d",
                                root_local, h->free_line);
                        } else if (h->state == IR_HS_MAYBE_FREED) {
                            ir_zc_error(zc, inst->source_line,
                                "freeing local %%%d which may already be freed",
                                root_local);
                        }
                        h->state = IR_HS_FREED;
                        h->free_line = inst->source_line;
                        ir_propagate_alias_state(ps, h, IR_HS_FREED,
                                                  inst->source_line);
                    }
                }
            }
            break;
        }

        if (!summary) break;

        /* Phase D7: if callee returns an ARENA-colored pointer, tag the
         * call's dest local so it's skipped in leak detection. Propagates
         * arena coloring through wrapper chains (create_task → outer). */
        if (inst->dest_local >= 0 && summary->returns_color == ZC_COLOR_ARENA) {
            IRHandleInfo *dh = ir_add_handle(ps, inst->dest_local);
            if (dh) {
                dh->state = IR_HS_ALIVE;
                dh->alloc_line = inst->source_line;
                if (dh->alloc_id == 0) dh->alloc_id = inst->dest_local;
                dh->source_color = ZC_COLOR_ARENA;
            }
        }

        /* For each param the summary affects, resolve arg local, apply state */
        for (int pi = 0; pi < summary->param_count; pi++) {
            if (!summary->frees_param[pi] && !summary->maybe_frees_param[pi])
                continue;

            int arg_local = -1;
            /* Decomposed path */
            if (inst->call_arg_locals && pi < inst->call_arg_local_count) {
                arg_local = inst->call_arg_locals[pi];
            }
            /* Passthrough path */
            if (arg_local < 0 && inst->args && pi < inst->arg_count &&
                inst->args[pi] && inst->args[pi]->kind == NODE_IDENT) {
                arg_local = ir_find_local_exact_first(func,
                    inst->args[pi]->ident.name,
                    (uint32_t)inst->args[pi]->ident.name_len);
            }
            if (arg_local < 0) continue;

            IRHandleInfo *h = ir_find_handle(ps, arg_local);
            /* Phase E: Auto-register when caller's own arg is a param and
             * the callee frees it. Enables FuncSummary chain propagation:
             * step_c frees its param → step_b calls step_c(r) → step_b's
             * r (also a param) gets marked FREED → step_b's summary
             * frees_param[0]=true. Mark escaped to avoid leak flag. */
            if (!h && arg_local < func->local_count &&
                func->locals[arg_local].is_param) {
                h = ir_add_handle(ps, arg_local);
                if (h) {
                    h->state = IR_HS_ALIVE;
                    h->alloc_line = inst->source_line;
                    h->alloc_id = _ir_next_alloc_id++;
                    h->source_color = ZC_COLOR_UNKNOWN;
                    h->escaped = true;  /* external input */
                }
            }
            if (!h) continue;

            /* Error on using an already-invalid handle */
            if (ir_is_invalid(h)) {
                ir_zc_error(zc, inst->source_line,
                    "passing %s handle %%%d to function that frees it",
                    ir_state_name(h->state), arg_local);
            }

            /* Apply summary: definite free → FREED; maybe → MAYBE_FREED */
            IRHandleState new_state = summary->frees_param[pi]
                ? IR_HS_FREED : IR_HS_MAYBE_FREED;
            h->state = new_state;
            h->free_line = inst->source_line;

            /* Propagate to aliases */
            ir_propagate_alias_state(ps, h, new_state, inst->source_line);
        }
        break;
    }

    /* Spawn → mark args as transferred. Phase D3: scoped spawn with
     * ThreadHandle registers the handle so its join/leak is tracked.
     * Phase D5: spawn inside @critical is a hardware-safety error
     * (pthread_create with interrupts disabled). */
    case IR_SPAWN: {
        /* Phase D5: spawn bans — interrupt handlers and @critical blocks */
        if (func->is_interrupt) {
            ir_zc_error(zc, inst->source_line,
                "spawn banned in interrupt handler — "
                "pthread_create with interrupts disabled is unsafe.");
        } else if (ps->critical_depth > 0) {
            ir_zc_error(zc, inst->source_line,
                "spawn banned inside @critical block — "
                "thread creation with interrupts disabled is unsafe.");
        }

        /* Arguments passed to spawn transfer ownership */
        for (int i = 0; i < inst->arg_count; i++) {
            if (inst->args && inst->args[i] && inst->args[i]->kind == NODE_IDENT) {
                int arg_local = ir_find_local_exact_first(func,
                    inst->args[i]->ident.name,
                    (uint32_t)inst->args[i]->ident.name_len);
                if (arg_local >= 0) {
                    IRHandleInfo *h = ir_find_handle(ps, arg_local);
                    if (h) h->state = IR_HS_TRANSFERRED;
                }
            }
        }

        /* Phase D3: scoped spawn produces a ThreadHandle. Register it so
         * leak detection can report "thread not joined" specifically. */
        if (inst->is_scoped_spawn && inst->handle_name && inst->handle_name_len > 0) {
            int th_local = ir_find_local_exact_first(func,
                inst->handle_name, inst->handle_name_len);
            if (th_local >= 0) {
                IRHandleInfo *h = ir_add_handle(ps, th_local);
                if (h) {
                    h->state = IR_HS_ALIVE;
                    h->alloc_line = inst->source_line;
                    h->alloc_id = _ir_next_alloc_id++;
                    h->is_thread_handle = true;
                    h->source_color = ZC_COLOR_UNKNOWN;
                }
            }
        }

        break;
    }

    /* Phase B1+B2: FIELD_WRITE — move transfer + escape detection.
     *
     * Move transfer (B1, closes Gap 5): `b.item = t` where Box(Tok) b —
     * t transferred to the container field. Subsequent t use = error.
     *
     * Escape detection (B2): `global.field = h` or `s.field = h` where
     * s is a pointer param — h escapes to external scope. Suppresses
     * leak detection at function exit for h.
     *
     * Handles two target shapes:
     *   - src1_local = container local (decomposed path)
     *   - inst->expr  = AST NODE_ASSIGN (passthrough path)
     */
    case IR_FIELD_WRITE: {
        int rhs_local = -1;
        Node *target_expr = NULL;
        Node *value_expr = NULL;
        if (inst->src2_local >= 0) {
            rhs_local = inst->src2_local;
        }
        if (inst->expr && inst->expr->kind == NODE_ASSIGN) {
            target_expr = inst->expr->assign.target;
            value_expr = inst->expr->assign.value;
            if (rhs_local < 0 && value_expr) {
                rhs_local = ir_find_value_local(func, value_expr);
            }
        }

        /* Move transfer (B1) */
        if (rhs_local >= 0 && rhs_local < func->local_count) {
            Type *rhs_type = func->locals[rhs_local].type;
            if (ir_should_track_move(rhs_type)) {
                IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
                if (rh && ir_is_invalid(rh)) {
                    ir_zc_error(zc, inst->source_line,
                        "use of %s value (local %%%d) in field write",
                        ir_state_name(rh->state), rhs_local);
                }
                if (!rh) rh = ir_add_handle(ps, rhs_local);
                if (rh) {
                    rh->state = IR_HS_TRANSFERRED;
                    rh->free_line = inst->source_line;
                }
            }
        }

        /* Escape detection (B2): if target root is global or pointer param,
         * any handle we write escapes external scope. */
        if (target_expr && ir_target_root_escapes(zc, target_expr)) {
            ir_mark_local_escaped(ps, rhs_local);
        }

        /* Phase B3: register compound handle for the field target.
         * `s.handle = alloc_result` where alloc_result is an ALIVE local
         * registers (local_of_s, ".handle") as tracked, sharing alloc_id
         * with the bare local. When either is freed, the other's state
         * propagates via ir_propagate_alias_state. */
        if (target_expr && rhs_local >= 0) {
            IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
            if (rh && rh->state == IR_HS_ALIVE) {
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, target_expr,
                                             &root_local, &path, &path_len) == 0
                    && path_len > 0) {
                    IRHandleInfo *ch = ir_add_compound_handle(ps, root_local,
                                                               path, path_len);
                    if (ch) {
                        ch->state = IR_HS_ALIVE;
                        ch->alloc_line = rh->alloc_line;
                        ch->alloc_id = rh->alloc_id;
                    }
                }
            }
        }
        break;
    }

    /* Phase B2: INDEX_WRITE — array element assignment. Mirror of
     * FIELD_WRITE. `arr[i] = h` where arr is global or arrives through
     * pointer param → h escapes. Also marks RHS TRANSFERRED if move
     * struct. Non-tracked RHS is ignored (common case). */
    case IR_INDEX_WRITE: {
        int rhs_local = -1;
        Node *target_expr = NULL;
        Node *value_expr = NULL;
        if (inst->src2_local >= 0) rhs_local = inst->src2_local;
        if (inst->expr && inst->expr->kind == NODE_ASSIGN) {
            target_expr = inst->expr->assign.target;
            value_expr = inst->expr->assign.value;
            if (rhs_local < 0 && value_expr) {
                rhs_local = ir_find_value_local(func, value_expr);
            }
        }
        /* Move transfer */
        if (rhs_local >= 0 && rhs_local < func->local_count) {
            Type *rhs_type = func->locals[rhs_local].type;
            if (ir_should_track_move(rhs_type)) {
                IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
                if (rh && ir_is_invalid(rh)) {
                    ir_zc_error(zc, inst->source_line,
                        "use of %s value (local %%%d) in array write",
                        ir_state_name(rh->state), rhs_local);
                }
                if (!rh) rh = ir_add_handle(ps, rhs_local);
                if (rh) {
                    rh->state = IR_HS_TRANSFERRED;
                    rh->free_line = inst->source_line;
                }
            }
        }
        /* Escape */
        if (target_expr && ir_target_root_escapes(zc, target_expr)) {
            ir_mark_local_escaped(ps, rhs_local);
        }
        /* Phase B3: register compound handle for const array-index target.
         * Variable indices aren't trackable (matches zercheck.c behavior). */
        if (target_expr && rhs_local >= 0) {
            IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
            if (rh && rh->state == IR_HS_ALIVE) {
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, target_expr,
                                             &root_local, &path, &path_len) == 0
                    && path_len > 0) {
                    IRHandleInfo *ch = ir_add_compound_handle(ps, root_local,
                                                               path, path_len);
                    if (ch) {
                        ch->state = IR_HS_ALIVE;
                        ch->alloc_line = rh->alloc_line;
                        ch->alloc_id = rh->alloc_id;
                    }
                }
            }
        }
        break;
    }

    default:
        break;
    }
}

/* ================================================================
 * Main Analysis — walk CFG in topological order
 * ================================================================ */

bool zercheck_ir(ZerCheck *zc, IRFunc *func) {
    if (!func || func->block_count == 0) return true;

    /* Allocate per-block states */
    IRPathState *block_states = (IRPathState *)calloc(func->block_count, sizeof(IRPathState));
    if (!block_states) return true;

    for (int bi = 0; bi < func->block_count; bi++)
        ir_ps_init(&block_states[bi]);

    /* Process blocks in order (topological for forward edges).
     * For back edges (loops), use fixed-point iteration. */
    bool changed = true;
    int iterations = 0;
    while (changed && iterations < 32) {
        changed = false;
        iterations++;

        for (int bi = 0; bi < func->block_count; bi++) {
            IRBlock *bb = &func->blocks[bi];

            /* Merge predecessor states */
            IRPathState merged;
            if (bb->pred_count == 0) {
                ir_ps_init(&merged); /* entry block — empty state */
            } else {
                IRPathState *pred_states = (IRPathState *)calloc(bb->pred_count, sizeof(IRPathState));
                for (int pi = 0; pi < bb->pred_count; pi++) {
                    pred_states[pi] = ir_ps_copy(&block_states[bb->preds[pi]]);
                }
                merged = ir_merge_states(pred_states, bb->pred_count);
                for (int pi = 0; pi < bb->pred_count; pi++)
                    ir_ps_free(&pred_states[pi]);
                free(pred_states);
            }

            /* Process instructions in this block */
            for (int ii = 0; ii < bb->inst_count; ii++) {
                ir_check_inst(zc, &merged, &bb->insts[ii], func);
            }

            /* Check if state changed (for fixed-point convergence) */
            if (merged.handle_count != block_states[bi].handle_count) {
                changed = true;
            } else {
                for (int hi = 0; hi < merged.handle_count; hi++) {
                    IRHandleInfo *mh = &merged.handles[hi];
                    IRHandleInfo *oh = ir_find_handle(&block_states[bi], mh->local_id);
                    if (!oh || oh->state != mh->state) {
                        changed = true;
                        break;
                    }
                }
            }

            /* Update block state */
            ir_ps_free(&block_states[bi]);
            block_states[bi] = merged;
        }
    }

    /* Phase C1: FuncSummary build / refine. When building a summary, examine
     * final state of param locals at each return block. Union across blocks:
     *   - FREED in every block with a return → frees_param[i] = true
     *   - FREED or MAYBE_FREED in some returns → maybe_frees_param[i] = true
     *   - never FREED/MAYBE_FREED → both false
     * Summary is attached to ZerCheck via find_summary / allocation loop
     * identical to zercheck.c so consumers (zc_apply_summary in both paths)
     * see the same shape. */
    if (zc->building_summary && func->ast_node && func->ast_node->kind == NODE_FUNC_DECL) {
        Node *fn = func->ast_node;
        int pc = fn->func_decl.param_count;
        if (pc > 0) {
            bool *frees = (bool *)calloc(pc, sizeof(bool));
            bool *maybe_frees = (bool *)calloc(pc, sizeof(bool));
            bool *any_return_saw_alive = (bool *)calloc(pc, sizeof(bool));
            bool *all_return_blocks_freed = (bool *)malloc(pc * sizeof(bool));
            for (int i = 0; i < pc; i++) all_return_blocks_freed[i] = true;
            int return_blocks = 0;

            for (int bi = 0; bi < func->block_count; bi++) {
                IRBlock *bb = &func->blocks[bi];
                if (bb->inst_count == 0) continue;
                if (bb->insts[bb->inst_count - 1].op != IR_RETURN) continue;
                return_blocks++;
                IRPathState *ps = &block_states[bi];

                for (int i = 0; i < pc; i++) {
                    ParamDecl *p = &fn->func_decl.params[i];
                    TypeNode *tnode = p->type;
                    if (!tnode || (tnode->kind != TYNODE_HANDLE &&
                        tnode->kind != TYNODE_POINTER)) {
                        all_return_blocks_freed[i] = false;
                        continue;
                    }
                    int plocal = ir_find_local_exact_first(func, p->name, (uint32_t)p->name_len);
                    if (plocal < 0) { all_return_blocks_freed[i] = false; continue; }
                    IRHandleInfo *h = ir_find_handle(ps, plocal);
                    if (!h) {
                        all_return_blocks_freed[i] = false;
                        continue;
                    }
                    if (h->state == IR_HS_FREED) {
                        maybe_frees[i] = true;  /* definitely seen on this path */
                    } else if (h->state == IR_HS_MAYBE_FREED) {
                        maybe_frees[i] = true;
                        all_return_blocks_freed[i] = false;
                    } else {
                        any_return_saw_alive[i] = true;
                        all_return_blocks_freed[i] = false;
                    }
                }
            }

            /* frees_param[i] = true iff EVERY return block had this param FREED */
            for (int i = 0; i < pc; i++) {
                if (return_blocks > 0 && all_return_blocks_freed[i]
                    && !any_return_saw_alive[i] && maybe_frees[i]) {
                    frees[i] = true;
                }
            }
            free(all_return_blocks_freed);
            free(any_return_saw_alive);

            /* Phase D7: Arena wrapper chain inference.
             * Determine returns_color by examining what each return block
             * returns. If all returns yield ARENA-colored values, the
             * function's returns_color is ARENA. This propagates arena
             * coloring through wrapper chains like:
             *   *T create(Arena *a) { return arena.alloc(T).ptr; }
             *   *T wrap() { return create(&g_arena); }           // ARENA
             *   *T outer() { return wrap(); }                     // ARENA */
            int inferred_color = -1;  /* -1 = unset, -2 = mixed */
            for (int bi = 0; bi < func->block_count; bi++) {
                IRBlock *bb = &func->blocks[bi];
                if (bb->inst_count == 0) continue;
                IRInst *last = &bb->insts[bb->inst_count - 1];
                if (last->op != IR_RETURN || !last->expr) continue;
                /* Identify returned local (direct ident or primary of orelse) */
                Node *ret_expr = last->expr;
                if (ret_expr->kind == NODE_ORELSE) ret_expr = ret_expr->orelse.expr;
                if (!ret_expr || ret_expr->kind != NODE_IDENT) {
                    inferred_color = -2; break;
                }
                int rlocal = ir_find_local_exact_first(func,
                    ret_expr->ident.name, (uint32_t)ret_expr->ident.name_len);
                if (rlocal < 0) { inferred_color = -2; break; }
                IRHandleInfo *rh = ir_find_handle(&block_states[bi], rlocal);
                int color = rh ? rh->source_color : ZC_COLOR_UNKNOWN;
                if (inferred_color == -1) inferred_color = color;
                else if (inferred_color != color) { inferred_color = -2; break; }
            }
            int returns_color_final =
                (inferred_color < 0) ? ZC_COLOR_UNKNOWN : inferred_color;

            /* Update or create summary — same logic as zercheck.c:2320+ */
            FuncSummary *existing = NULL;
            for (int si = 0; si < zc->summary_count; si++) {
                if (zc->summaries[si].func_name_len == (uint32_t)fn->func_decl.name_len &&
                    memcmp(zc->summaries[si].func_name, fn->func_decl.name,
                           fn->func_decl.name_len) == 0) {
                    existing = &zc->summaries[si]; break;
                }
            }
            if (existing) {
                bool changed = false;
                if (existing->param_count == pc) {
                    for (int i = 0; i < pc; i++) {
                        if (existing->frees_param[i] != frees[i]) changed = true;
                        if (existing->maybe_frees_param[i] != maybe_frees[i]) changed = true;
                    }
                } else {
                    changed = true;
                }
                if (existing->returns_color != returns_color_final) changed = true;
                if (changed) {
                    free(existing->frees_param);
                    free(existing->maybe_frees_param);
                    existing->param_count = pc;
                    existing->frees_param = frees;
                    existing->maybe_frees_param = maybe_frees;
                    existing->returns_color = returns_color_final;
                } else {
                    free(frees); free(maybe_frees);
                }
            } else {
                if (zc->summary_count >= zc->summary_capacity) {
                    int nc = zc->summary_capacity < 8 ? 8 : zc->summary_capacity * 2;
                    FuncSummary *ns = (FuncSummary *)realloc(zc->summaries,
                        nc * sizeof(FuncSummary));
                    if (ns) {
                        zc->summaries = ns;
                        zc->summary_capacity = nc;
                    }
                }
                if (zc->summary_count < zc->summary_capacity) {
                    FuncSummary *s = &zc->summaries[zc->summary_count++];
                    memset(s, 0, sizeof(FuncSummary));
                    s->func_name = fn->func_decl.name;
                    s->func_name_len = (uint32_t)fn->func_decl.name_len;
                    s->param_count = pc;
                    s->frees_param = frees;
                    s->maybe_frees_param = maybe_frees;
                    s->returns_color = returns_color_final;
                    s->returns_param_color = -1;
                } else {
                    free(frees); free(maybe_frees);
                }
            }
        }
    }

    /* Phase C3: before leak detection, scan every IR_DEFER_PUSH body in the
     * function and mark handles freed therein as FREED in the return-block
     * path states. Conservative: every defer's frees apply to every return
     * block. Matches zercheck.c defer_scan_all_frees at function exit.
     *
     * Without this, any handle freed only inside a `defer { pool.free(h); }`
     * would appear ALIVE at function exit and trigger a false leak error.
     *
     * We walk all blocks to collect defers once, then apply to each return
     * block's state. */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        if (bb->inst_count == 0) continue;
        IRInst *last = &bb->insts[bb->inst_count - 1];
        if (last->op != IR_RETURN) continue;

        IRPathState *ret_ps = &block_states[bi];
        for (int di = 0; di < func->block_count; di++) {
            IRBlock *db = &func->blocks[di];
            for (int dj = 0; dj < db->inst_count; dj++) {
                IRInst *inst = &db->insts[dj];
                if (inst->op != IR_DEFER_PUSH || !inst->defer_body) continue;
                ir_defer_scan_frees(zc, func, ret_ps, inst->defer_body,
                                     inst->source_line);
            }
        }
    }

    /* Phase D6: ghost handle detection — compute which allocated handles
     * are NEVER read subsequently. `pool.alloc()` as a bare expression
     * without an assignment target is the canonical case. The bare alloc
     * produces a temp local; if that local never appears as a source in
     * any later instruction, the allocation was discarded.
     *
     * Implementation: for each handle local_id that is ALIVE at any
     * return block, scan all instructions and check whether the local
     * appears as src1_local, src2_local, handle_local, call_arg_locals,
     * or inside inst->expr / inst->args AST trees. If never used → ghost.
     *
     * Conservative: any AST reference counts as "used" (we don't prove
     * it's actually read). Reduces false positives at cost of false
     * negatives (e.g., local assigned but never deref'd still passes). */
    /* Collect set of "used" locals across whole function body. */
    int *used_locals = (int *)calloc(func->local_count, sizeof(int));
    if (used_locals) {
        for (int bi = 0; bi < func->block_count; bi++) {
            IRBlock *bb = &func->blocks[bi];
            for (int ii = 0; ii < bb->inst_count; ii++) {
                IRInst *inst = &bb->insts[ii];
                if (inst->src1_local >= 0 && inst->src1_local < func->local_count)
                    used_locals[inst->src1_local] = 1;
                if (inst->src2_local >= 0 && inst->src2_local < func->local_count)
                    used_locals[inst->src2_local] = 1;
                if (inst->handle_local >= 0 && inst->handle_local < func->local_count)
                    used_locals[inst->handle_local] = 1;
                if (inst->cond_local >= 0 && inst->cond_local < func->local_count)
                    used_locals[inst->cond_local] = 1;
                for (int ai = 0; ai < inst->call_arg_local_count; ai++) {
                    if (inst->call_arg_locals &&
                        inst->call_arg_locals[ai] >= 0 &&
                        inst->call_arg_locals[ai] < func->local_count)
                        used_locals[inst->call_arg_locals[ai]] = 1;
                }
                /* Phase E: passthrough usage via AST inst->expr. Many IR
                 * ops carry their original AST (IR_ASSIGN with NODE_ORELSE,
                 * IR_RETURN with expr, IR_CALL args, etc.). Walk the AST
                 * for NODE_IDENTs that reference tracked locals. Without
                 * this, mh used in `h = mh orelse return` counts as unused. */
                if (inst->expr) {
                    /* Recursive walk limited to small depth — expressions
                     * are typically shallow. Use a manual stack to avoid
                     * runaway recursion on pathological ASTs. */
                    Node *stack[64];
                    int top = 0;
                    stack[top++] = inst->expr;
                    while (top > 0) {
                        Node *n = stack[--top];
                        if (!n) continue;
                        switch (n->kind) {
                        case NODE_IDENT: {
                            int li = ir_find_local_exact_first(func,
                                n->ident.name, (uint32_t)n->ident.name_len);
                            if (li >= 0 && li < func->local_count)
                                used_locals[li] = 1;
                            break;
                        }
                        case NODE_FIELD:
                            if (top < 63) stack[top++] = n->field.object; break;
                        case NODE_INDEX:
                            if (top < 62) {
                                stack[top++] = n->index_expr.object;
                                stack[top++] = n->index_expr.index;
                            } break;
                        case NODE_UNARY:
                            if (top < 63) stack[top++] = n->unary.operand; break;
                        case NODE_BINARY:
                            if (top < 62) {
                                stack[top++] = n->binary.left;
                                stack[top++] = n->binary.right;
                            } break;
                        case NODE_CALL:
                            if (top < 63) stack[top++] = n->call.callee;
                            for (int ai = 0; ai < n->call.arg_count && top < 63; ai++)
                                stack[top++] = n->call.args[ai];
                            break;
                        case NODE_ASSIGN:
                            if (top < 62) {
                                stack[top++] = n->assign.target;
                                stack[top++] = n->assign.value;
                            } break;
                        case NODE_ORELSE:
                            if (top < 62) {
                                stack[top++] = n->orelse.expr;
                                stack[top++] = n->orelse.fallback;
                            } break;
                        case NODE_TYPECAST:
                            if (top < 63) stack[top++] = n->typecast.expr; break;
                        case NODE_SLICE:
                            if (top < 61) {
                                stack[top++] = n->slice.object;
                                stack[top++] = n->slice.start;
                                stack[top++] = n->slice.end;
                            } break;
                        case NODE_INTRINSIC:
                            for (int ai = 0; ai < n->intrinsic.arg_count && top < 63; ai++)
                                stack[top++] = n->intrinsic.args[ai];
                            break;
                        case NODE_STRUCT_INIT:
                            for (int fi = 0; fi < n->struct_init.field_count && top < 63; fi++)
                                stack[top++] = n->struct_init.fields[fi].value;
                            break;
                        default: break;
                        }
                    }
                }
            }
        }
    }

    /* Phase E: alloc_id-grouped leak detection (restoration of union
     * semantics). An alloc_id is "covered" if any handle with that
     * alloc_id is FREED / TRANSFERRED / escaped in ANY return block.
     * Only flag alloc_ids with no covering event anywhere.
     *
     * Mirrors zercheck.c linear-scan final-state: linear scan continues
     * through early returns as if they didn't happen, so the state at
     * function-end represents the union of "what happened on all paths".
     *
     * Limitation: doesn't catch mixed-path leaks where one return frees
     * and another doesn't. Gen_uaf_003 style. Phase F task or documented
     * tradeoff. */
    int *covered_ids = NULL;
    int covered_cap = 0, covered_n = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        if (bb->inst_count == 0) continue;
        if (bb->insts[bb->inst_count - 1].op != IR_RETURN) continue;
        IRPathState *ps2 = &block_states[bi];
        for (int hi = 0; hi < ps2->handle_count; hi++) {
            IRHandleInfo *h = &ps2->handles[hi];
            bool cover = h->escaped ||
                         h->state == IR_HS_FREED ||
                         h->state == IR_HS_TRANSFERRED;
            if (!cover) continue;
            bool already = false;
            for (int ci = 0; ci < covered_n; ci++) {
                if (covered_ids[ci] == h->alloc_id) { already = true; break; }
            }
            if (already) continue;
            if (covered_n >= covered_cap) {
                covered_cap = covered_cap < 8 ? 8 : covered_cap * 2;
                int *nc = (int *)realloc(covered_ids, covered_cap * sizeof(int));
                if (nc) covered_ids = nc;
            }
            if (covered_n < covered_cap)
                covered_ids[covered_n++] = h->alloc_id;
        }
    }

    int *reported_ids = NULL;
    int reported_cap = 0, reported_n = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        if (bb->inst_count == 0) continue;
        IRInst *last = &bb->insts[bb->inst_count - 1];
        if (last->op != IR_RETURN) continue;
        /* Phase E: skip orelse-fallback blocks — they're reached only
         * when the optional was null, no allocation to leak. */
        if (bb->is_orelse_fallback) continue;

        IRPathState *ps = &block_states[bi];
        for (int hi = 0; hi < ps->handle_count; hi++) {
            IRHandleInfo *h = &ps->handles[hi];
            if (h->escaped) continue;
            if (h->source_color == ZC_COLOR_ARENA) continue;
            if (h->local_id >= 0 && h->local_id < func->local_count) {
                IRLocal *loc = &func->locals[h->local_id];
                Type *lt = loc->type;
                if (ir_should_track_move(lt)) continue;
                if (loc->is_temp) continue;
                /* Phase E: params aren't "allocated" locals — auto-
                 * registration (via pool.free(param), @ptrcast(*T, param),
                 * extern free(param)) creates handle entries used for
                 * FuncSummary building, but the param itself isn't a
                 * local allocation that leaks. Summary captures whether
                 * the callee frees it; callers get the frees_param flag. */
                if (loc->is_param) continue;
            }
            if (h->path_len > 0) continue;  /* compound — skip */

            /* Skip if alloc_id covered somewhere */
            bool covered = false;
            for (int ci = 0; ci < covered_n; ci++) {
                if (covered_ids[ci] == h->alloc_id) { covered = true; break; }
            }
            if (covered) continue;

            /* Skip if we already reported this alloc_id */
            bool reported = false;
            for (int ri = 0; ri < reported_n; ri++) {
                if (reported_ids[ri] == h->alloc_id) { reported = true; break; }
            }
            if (reported) continue;

            if (h->state == IR_HS_ALIVE) {
                if (h->is_thread_handle) {
                    ir_zc_error(zc, last->source_line,
                        "ThreadHandle '%.*s' not joined before function exit — "
                        "add th.join() or detach explicitly",
                        (int)func->locals[h->local_id].name_len,
                        func->locals[h->local_id].name);
                } else if (used_locals && !used_locals[h->local_id]) {
                    ir_zc_error(zc, h->alloc_line,
                        "ghost handle: allocation discarded — result of "
                        "alloc() at line %d is never assigned or used",
                        h->alloc_line);
                } else {
                    const char *alloc_verb = "pool.free";
                    if (h->source_color == ZC_COLOR_MALLOC) alloc_verb = "free";
                    ir_zc_error(zc, last->source_line,
                        "handle %%%d (local '%.*s') allocated at line %d but never freed — "
                        "add defer %s() or return the handle",
                        h->local_id,
                        (int)func->locals[h->local_id].name_len,
                        func->locals[h->local_id].name,
                        h->alloc_line, alloc_verb);
                }
                /* Remember this alloc_id so we don't report twice */
                if (reported_n >= reported_cap) {
                    reported_cap = reported_cap < 8 ? 8 : reported_cap * 2;
                    int *nr = (int *)realloc(reported_ids, reported_cap * sizeof(int));
                    if (nr) reported_ids = nr;
                }
                if (reported_n < reported_cap)
                    reported_ids[reported_n++] = h->alloc_id;
            }
        }
    }
    free(covered_ids);
    free(reported_ids);
    free(used_locals);

    /* Phase E: ThreadHandle join check. At each return block, any
     * unjoined ThreadHandle is a leak. Track reported names to dedup.
     * Skip orelse-fallback blocks (thread wasn't spawned on null path). */
    const char **reported_names = NULL;
    uint32_t *reported_name_lens = NULL;
    int rn_cap = 0, rn_count = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        if (bb->inst_count == 0) continue;
        IRInst *last = &bb->insts[bb->inst_count - 1];
        if (last->op != IR_RETURN) continue;
        if (bb->is_orelse_fallback) continue;
        IRPathState *ps = &block_states[bi];
        for (int ti = 0; ti < ps->thread_count; ti++) {
            IRThreadTrack *t = &ps->threads[ti];
            if (t->joined) continue;
            bool already = false;
            for (int ri = 0; ri < rn_count; ri++) {
                if (reported_name_lens[ri] == t->name_len &&
                    memcmp(reported_names[ri], t->name, t->name_len) == 0) {
                    already = true; break;
                }
            }
            if (already) continue;
            ir_zc_error(zc, last->source_line,
                "ThreadHandle '%.*s' not joined before function exit — "
                "add th.join() or detach explicitly",
                (int)t->name_len, t->name);
            if (rn_count >= rn_cap) {
                rn_cap = rn_cap < 4 ? 4 : rn_cap * 2;
                const char **nn = (const char **)realloc(reported_names,
                    rn_cap * sizeof(char *));
                uint32_t *nl = (uint32_t *)realloc(reported_name_lens,
                    rn_cap * sizeof(uint32_t));
                if (nn) reported_names = nn;
                if (nl) reported_name_lens = nl;
            }
            if (rn_count < rn_cap) {
                reported_names[rn_count] = t->name;
                reported_name_lens[rn_count] = t->name_len;
                rn_count++;
            }
        }
    }
    free(reported_names);
    free(reported_name_lens);

    /* Cleanup */
    for (int bi = 0; bi < func->block_count; bi++)
        ir_ps_free(&block_states[bi]);
    free(block_states);

    return zc->error_count == 0;
}
