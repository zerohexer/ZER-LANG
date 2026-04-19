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
} IRHandleInfo;

/* Per-block analysis state */
typedef struct {
    IRHandleInfo *handles;
    int handle_count;
    int handle_capacity;
    bool terminated;       /* block ends with return/unreachable */
} IRPathState;

/* ================================================================
 * Path State Operations
 * ================================================================ */

static void ir_ps_init(IRPathState *ps) {
    ps->handles = NULL;
    ps->handle_count = 0;
    ps->handle_capacity = 0;
    ps->terminated = false;
}

static IRPathState ir_ps_copy(IRPathState *src) {
    IRPathState dst;
    dst.handle_count = src->handle_count;
    dst.handle_capacity = src->handle_count > 0 ? src->handle_count : 4;
    dst.terminated = false;
    dst.handles = (IRHandleInfo *)malloc(dst.handle_capacity * sizeof(IRHandleInfo));
    if (src->handles && src->handle_count > 0)
        memcpy(dst.handles, src->handles, src->handle_count * sizeof(IRHandleInfo));
    return dst;
}

static void ir_ps_free(IRPathState *ps) {
    free(ps->handles);
    ps->handles = NULL;
    ps->handle_count = 0;
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
        return ir_find_local(func,
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
    int local = ir_find_local(func,
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

    /* Allocation → register handle as ALIVE */
    case IR_POOL_ALLOC:
    case IR_SLAB_ALLOC:
    case IR_SLAB_ALLOC_PTR: {
        if (inst->dest_local >= 0) {
            IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
            if (h) {
                /* Check for overwrite of alive handle (leak) */
                if (h->state == IR_HS_ALIVE) {
                    ir_zc_error(zc, inst->source_line,
                        "handle %%%d overwritten while alive — previous allocation leaked",
                        inst->dest_local);
                }
                h->state = IR_HS_ALIVE;
                h->alloc_line = inst->source_line;
                h->alloc_id = inst->dest_local; /* simple: local_id = alloc_id */
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
     * `Token b = a` transfers ownership: a → TRANSFERRED, b → ALIVE (new id). */
    case IR_ASSIGN: {
        if (inst->dest_local >= 0 && inst->expr) {
            /* If source is an ident that's a tracked handle, create alias */
            if (inst->expr->kind == NODE_IDENT) {
                int src_local = ir_find_local(func,
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

        /* Case (a): direct ident return */
        if (primary && primary->kind == NODE_IDENT) {
            int ret_local = ir_find_local(func,
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

    /* Spawn → mark args as transferred */
    case IR_SPAWN: {
        /* Arguments passed to spawn transfer ownership */
        for (int i = 0; i < inst->arg_count; i++) {
            if (inst->args && inst->args[i] && inst->args[i]->kind == NODE_IDENT) {
                int arg_local = ir_find_local(func,
                    inst->args[i]->ident.name,
                    (uint32_t)inst->args[i]->ident.name_len);
                if (arg_local >= 0) {
                    IRHandleInfo *h = ir_find_handle(ps, arg_local);
                    if (h) h->state = IR_HS_TRANSFERRED;
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

    /* Leak detection: check handles at function exit (last block or return blocks) */
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        if (bb->inst_count == 0) continue;
        IRInst *last = &bb->insts[bb->inst_count - 1];
        if (last->op != IR_RETURN) continue;

        IRPathState *ps = &block_states[bi];
        for (int hi = 0; hi < ps->handle_count; hi++) {
            IRHandleInfo *h = &ps->handles[hi];
            if (h->escaped) continue;
            if (h->state == IR_HS_ALIVE) {
                ir_zc_error(zc, last->source_line,
                    "handle %%%d (local '%.*s') allocated at line %d but never freed — "
                    "add defer pool.free() or return the handle",
                    h->local_id,
                    (int)func->locals[h->local_id].name_len,
                    func->locals[h->local_id].name,
                    h->alloc_line);
            } else if (h->state == IR_HS_MAYBE_FREED) {
                ir_zc_error(zc, last->source_line,
                    "handle %%%d may not be freed on all paths",
                    h->local_id);
            }
        }
    }

    /* Cleanup */
    for (int bi = 0; bi < func->block_count; bi++)
        ir_ps_free(&block_states[bi]);
    free(block_states);

    return zc->error_count == 0;
}
