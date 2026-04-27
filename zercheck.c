#include "zercheck.h"
#include "src/safety/handle_state.h"   /* zer_handle_state_is_invalid — VST-verified */
#include "src/safety/move_rules.h"     /* zer_type_kind_is_move_struct, _should_track */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/* ================================================================
 * ZER-CHECK Implementation
 *
 * All internal arrays are dynamic (malloc/realloc). No fixed limits.
 * ================================================================ */

/* ---- Error reporting ---- */

static void zc_error(ZerCheck *zc, int line, const char *fmt, ...) {
    if (zc->building_summary) return; /* suppress during summary phase */
    zc->error_count++;
    fprintf(stderr, "%s:%d: zercheck: ", zc->file_name, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void zc_warning(ZerCheck *zc, int line, const char *fmt, ...) {
    if (zc->building_summary) return;
    fprintf(stderr, "%s:%d: zercheck warning: ", zc->file_name, line);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* ---- Path state helpers ---- */

static void pathstate_init(PathState *ps) {
    ps->handles = NULL;
    ps->handle_count = 0;
    ps->handle_capacity = 0;
    ps->terminated = false;
}

/* deep copy a PathState (allocates new handle array) */
static PathState pathstate_copy(PathState *src) {
    PathState dst;
    dst.handle_count = src->handle_count;
    dst.handle_capacity = src->handle_count > 0 ? src->handle_count : 4;
    dst.terminated = false;  /* copies start non-terminated for branch analysis */
    dst.scope_depth = src->scope_depth; /* BUG-488: preserve scope depth for branch copies */
    dst.handles = (HandleInfo *)malloc(dst.handle_capacity * sizeof(HandleInfo));
    if (src->handle_count > 0) {
        memcpy(dst.handles, src->handles, src->handle_count * sizeof(HandleInfo));
    }
    return dst;
}

static HandleInfo *find_handle(PathState *ps, const char *name, uint32_t name_len);

/* Check if two PathStates have identical handle states (for fixed-point convergence) */
static bool pathstate_equal(PathState *a, PathState *b) {
    for (int i = 0; i < a->handle_count; i++) {
        HandleInfo *bh = find_handle(b, a->handles[i].name, a->handles[i].name_len);
        if (!bh) return false;
        if (a->handles[i].state != bh->state) return false;
    }
    return true;
}

static void pathstate_free(PathState *ps) {
    free(ps->handles);
    ps->handles = NULL;
    ps->handle_count = 0;
    ps->handle_capacity = 0;
}

static HandleInfo *find_handle(PathState *ps, const char *name, uint32_t name_len) {
    /* BUG-488: return highest scope_depth match (innermost scope).
     * When variables shadow, inner scope has higher depth. */
    HandleInfo *result = NULL;
    for (int i = 0; i < ps->handle_count; i++) {
        if (ps->handles[i].name_len == name_len &&
            memcmp(ps->handles[i].name, name, name_len) == 0) {
            if (!result || ps->handles[i].scope_depth >= result->scope_depth)
                result = &ps->handles[i];
        }
    }
    return result;
}

/* BUG-488: find handle only at current scope depth (for var-decl registration).
 * Returns NULL if the handle exists only in an outer scope — caller must add_handle
 * to create a new shadow. Source lookups (UAF check) use find_handle (any scope). */
static HandleInfo *find_handle_local(PathState *ps, const char *name, uint32_t name_len) {
    HandleInfo *result = NULL;
    for (int i = 0; i < ps->handle_count; i++) {
        if (ps->handles[i].name_len == name_len &&
            memcmp(ps->handles[i].name, name, name_len) == 0 &&
            ps->handles[i].scope_depth == ps->scope_depth) {
            result = &ps->handles[i];
        }
    }
    return result;
}

static HandleInfo *add_handle(PathState *ps, const char *name, uint32_t name_len) {
    if (ps->handle_count >= ps->handle_capacity) {
        int new_cap = ps->handle_capacity * 2;
        if (new_cap < 8) new_cap = 8;
        HandleInfo *new_handles = (HandleInfo *)realloc(ps->handles,
            new_cap * sizeof(HandleInfo));
        if (!new_handles) return NULL;
        ps->handles = new_handles;
        ps->handle_capacity = new_cap;
    }
    HandleInfo *h = &ps->handles[ps->handle_count++];
    memset(h, 0, sizeof(HandleInfo));
    h->name = name;
    h->name_len = name_len;
    h->state = HS_UNKNOWN;
    h->pool_id = -1;
    h->scope_depth = ps->scope_depth; /* BUG-488: track lexical scope */
    return h;
}

/* ---- Pool registry ---- */

static int find_pool_id(ZerCheck *zc, const char *name, uint32_t name_len) {
    for (int i = 0; i < zc->pool_count; i++) {
        if (zc->pools[i].name_len == name_len &&
            memcmp(zc->pools[i].name, name, name_len) == 0)
            return zc->pools[i].id;
    }
    return -1;
}

static int register_pool(ZerCheck *zc, const char *name, uint32_t name_len) {
    int id = find_pool_id(zc, name, name_len);
    if (id >= 0) return id;
    if (zc->pool_count >= zc->pool_capacity) {
        int new_cap = zc->pool_capacity * 2;
        if (new_cap < 8) new_cap = 8;
        ZcPool *new_pools = (ZcPool *)realloc(zc->pools,
            new_cap * sizeof(ZcPool));
        if (!new_pools) return -1;
        zc->pools = new_pools;
        zc->pool_capacity = new_cap;
    }
    id = zc->pool_count;
    zc->pools[zc->pool_count].name = name;
    zc->pools[zc->pool_count].name_len = name_len;
    zc->pools[zc->pool_count].id = id;
    zc->pool_count++;
    return id;
}

/* ---- Handle key extraction from expressions ---- */

/* BUG-357: Build a string key from a handle expression so we can track
 * handles stored in arrays (arr[0]) and struct fields (s.h).
 * Returns key length, or 0 if the expression can't be tracked statically.
 * Only constant array indices are supported — variable indices return 0.
 *
 * Examples:
 *   NODE_IDENT "h"           → "h"
 *   NODE_INDEX arr[0]        → "arr[0]"
 *   NODE_FIELD s.h           → "s.h"
 *   NODE_FIELD s.arr[1]      → "s.arr[1]"
 *   NODE_INDEX arr[i]        → 0 (variable index, can't track)
 */
static int handle_key_from_expr(Node *expr, char *buf, int bufsize) {
    if (!expr || bufsize < 2) return 0;

    if (expr->kind == NODE_IDENT) {
        int len = (int)expr->ident.name_len;
        if (len >= bufsize) return 0;
        memcpy(buf, expr->ident.name, len);
        buf[len] = '\0';
        return len;
    }

    if (expr->kind == NODE_FIELD) {
        /* recurse on object, then append ".field" */
        int base = handle_key_from_expr(expr->field.object, buf, bufsize);
        if (base <= 0) return 0;
        int flen = (int)expr->field.field_name_len;
        if (base + 1 + flen >= bufsize) return 0;
        buf[base] = '.';
        memcpy(buf + base + 1, expr->field.field_name, flen);
        int total = base + 1 + flen;
        buf[total] = '\0';
        return total;
    }

    if (expr->kind == NODE_INDEX) {
        /* only constant integer indices can be tracked */
        if (!expr->index_expr.index ||
            expr->index_expr.index->kind != NODE_INT_LIT) return 0;
        int base = handle_key_from_expr(expr->index_expr.object, buf, bufsize);
        if (base <= 0) return 0;
        uint64_t idx = expr->index_expr.index->int_lit.value;
        int written = snprintf(buf + base, bufsize - base, "[%llu]",
                               (unsigned long long)idx);
        if (written <= 0 || base + written >= bufsize) return 0;
        return base + written;
    }

    return 0;
}

/* B10: Arena-allocated handle key — no fixed buffer limit.
 * Uses stack buffer for common case, arena-copies the result.
 * Returns key length, or 0 on failure. Sets *out_key to arena-allocated string. */
static int handle_key_arena(ZerCheck *zc, Node *expr, const char **out_key) {
    char stack_buf[128];
    int len = handle_key_from_expr(expr, stack_buf, sizeof(stack_buf));
    if (len <= 0) { *out_key = NULL; return 0; }
    char *key = (char *)arena_alloc(zc->arena, len + 1);
    if (!key) { *out_key = NULL; return 0; }
    memcpy(key, stack_buf, len + 1);
    *out_key = key;
    return len;
}

/* forward declarations */
static FuncSummary *find_summary(ZerCheck *zc, const char *name, uint32_t name_len);
static bool is_move_struct_type(Type *t);
static HandleInfo *zc_ensure_move_registered(ZerCheck *zc, PathState *ps,
                                              const char *name, uint32_t len, int line);

/* ---- *opaque alloc/free detection (Level 1) ---- */

/* Check if a function call is a known allocator (extern returning *opaque).
 * Recognized: malloc, calloc, realloc, strdup, strndup, or any extern
 * function returning *opaque with no body. */
static bool is_alloc_call(ZerCheck *zc, Node *call) {
    if (!call || call->kind != NODE_CALL) return false;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_IDENT) return false;
    /* look up function symbol */
    Symbol *sym = scope_lookup(zc->checker->global_scope,
        callee->ident.name, (uint32_t)callee->ident.name_len);
    if (!sym || !sym->is_function || !sym->func_node) return false;
    /* must be extern (no body) */
    if (sym->func_node->func_decl.body) return false;
    /* return type must be *opaque or *T (pointer) */
    Type *ret = sym->type;
    if (ret && ret->kind == TYPE_FUNC_PTR) ret = ret->func_ptr.ret;
    if (!ret) return false;
    ret = type_unwrap_distinct(ret);
    if (ret->kind == TYPE_POINTER || ret->kind == TYPE_OPAQUE) return true;
    /* optional pointer (?*T) also counts */
    if (ret->kind == TYPE_OPTIONAL) {
        Type *inner = type_unwrap_distinct(ret->optional.inner);
        if (inner && (inner->kind == TYPE_POINTER || inner->kind == TYPE_OPAQUE))
            return true;
    }
    return false;
}

/* Gap 17 (2026-04-27): name-substring check for cleanup-style functions.
 * When a bodyless function has a non-void return (e.g. int status), the
 * signature alone is ambiguous (could be a getter, hash, etc.). The name
 * substring is the deciding heuristic — destructor names follow well-
 * established conventions across C ecosystems (free/destroy/close/release/
 * delete/dispose/drop/cleanup/deinit/fini/shutdown/term). Match is
 * substring (case-insensitive on ASCII) so prefixes like `db_close`,
 * `slab_destroy`, `mtx_drop`, `arena_release` all qualify. */
static bool name_looks_like_destructor(const char *name, uint32_t name_len) {
    static const char *KEYWORDS[] = {
        "free", "destroy", "close", "release", "delete", "dispose",
        "drop", "cleanup", "deinit", "fini", "shutdown", "term", NULL
    };
    if (!name || name_len == 0) return false;
    for (int k = 0; KEYWORDS[k]; k++) {
        size_t klen = strlen(KEYWORDS[k]);
        if (klen > name_len) continue;
        for (uint32_t off = 0; off + klen <= name_len; off++) {
            /* case-insensitive ASCII compare */
            bool match = true;
            for (size_t j = 0; j < klen; j++) {
                char a = name[off + j];
                char b = KEYWORDS[k][j];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (a != b) { match = false; break; }
            }
            if (match) return true;
        }
    }
    return false;
}

/* Check if a function call is a free/close/destroy — detected by:
 * 1. Named "free" (original check)
 * 2. Any bodyless (extern/cinclude) function taking *opaque or *T as first
 *    param and returning void — signature heuristic for compile-time tracking.
 *    Covers: db_close(*opaque), sqlite3_close(*opaque), destroy(*opaque), etc.
 *    The function is bodyless so we can't see inside — the signature tells us.
 * 3. Gap 17: bodyless function with non-void return, *opaque/*T first param,
 *    AND name substring matches destructor convention (free/destroy/close/
 *    release/delete/dispose/drop/cleanup/deinit/fini/shutdown/term). Catches
 *    common C status-returning destructors: int destroy(*Resource), int
 *    db_close(*opaque), etc. Without this, returning an error code silently
 *    bypassed UAF tracking. */
static bool is_free_call(ZerCheck *zc, Node *call, char *arg_key, int *arg_key_len, int key_bufsize) {
    if (!call || call->kind != NODE_CALL) return false;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_IDENT) return false;
    if (call->call.arg_count < 1) return false;
    /* Check 1: named "free" */
    if (callee->ident.name_len == 4 &&
        memcmp(callee->ident.name, "free", 4) == 0 &&
        call->call.arg_count == 1) {
        *arg_key_len = handle_key_from_expr(call->call.args[0], arg_key, key_bufsize);
        return *arg_key_len > 0;
    }
    /* Checks 2/3: signature heuristic — bodyless function */
    Symbol *sym = scope_lookup(zc->checker->global_scope,
        callee->ident.name, (uint32_t)callee->ident.name_len);
    if (sym && sym->is_function && sym->func_node &&
        !sym->func_node->func_decl.body) {
        Type *ret = sym->type;
        if (ret && ret->kind == TYPE_FUNC_PTR) ret = ret->func_ptr.ret;
        Type *ret_eff = ret ? type_unwrap_distinct(ret) : NULL;
        bool ret_is_void = ret_eff && ret_eff->kind == TYPE_VOID;
        bool name_is_dtor = name_looks_like_destructor(
            callee->ident.name, (uint32_t)callee->ident.name_len);
        /* Check 2: void return → always heuristic-free.
         * Check 3: non-void return → only if name suggests destructor. */
        bool sig_match = ret_is_void || (ret_eff && name_is_dtor);
        if (sig_match) {
            /* first param must be *opaque or *T (pointer) */
            if (sym->func_node->func_decl.param_count >= 1) {
                Type *p0 = NULL;
                if (sym->type && sym->type->kind == TYPE_FUNC_PTR &&
                    sym->type->func_ptr.param_count >= 1)
                    p0 = sym->type->func_ptr.params[0];
                if (p0) p0 = type_unwrap_distinct(p0);
                if (p0 && (p0->kind == TYPE_POINTER || p0->kind == TYPE_OPAQUE)) {
                    *arg_key_len = handle_key_from_expr(call->call.args[0], arg_key, key_bufsize);
                    return *arg_key_len > 0;
                }
            }
        }
    }
    return false;
}

/* ---- Helpers ---- */

/* Check if a block always exits (return/break/continue on all paths).
 * Used by if-without-else: if the then-branch always exits, handles
 * freed inside it are NOT MAYBE_FREED after the if — we only reach
 * post-if if the branch was NOT taken. */
static bool block_always_exits(Node *node) {
    if (!node) return false;
    if (node->kind == NODE_RETURN || node->kind == NODE_BREAK ||
        node->kind == NODE_CONTINUE) return true;
    if (node->kind == NODE_GOTO) return true;
    if (node->kind == NODE_BLOCK) {
        /* last statement in block determines exit */
        for (int i = node->block.stmt_count - 1; i >= 0; i--) {
            if (block_always_exits(node->block.stmts[i])) return true;
            /* skip non-control-flow statements */
            if (node->block.stmts[i]->kind != NODE_VAR_DECL &&
                node->block.stmts[i]->kind != NODE_EXPR_STMT &&
                node->block.stmts[i]->kind != NODE_DEFER)
                break;
        }
    }
    if (node->kind == NODE_IF && node->if_stmt.then_body && node->if_stmt.else_body) {
        return block_always_exits(node->if_stmt.then_body) &&
               block_always_exits(node->if_stmt.else_body);
    }
    if (node->kind == NODE_CRITICAL) {
        return block_always_exits(node->critical.body);
    }
    if (node->kind == NODE_ONCE) {
        return false; /* @once body may not execute — can't assume it always exits */
    }
    return false;
}

/* Scan a defer body for free/delete calls. Returns handle key if found. */
/* Scan a single statement for a free/delete call. Returns handle key length if found. */
static int defer_stmt_is_free(Node *node, char *key_buf, int key_bufsize) {
    if (!node) return 0;
    if (node->kind == NODE_EXPR_STMT && node->expr_stmt.expr &&
        node->expr_stmt.expr->kind == NODE_CALL) {
        Node *call = node->expr_stmt.expr;
        if (call->call.callee && call->call.callee->kind == NODE_FIELD) {
            const char *method = call->call.callee->field.field_name;
            uint32_t mlen = (uint32_t)call->call.callee->field.field_name_len;
            if ((mlen == 4 && memcmp(method, "free", 4) == 0) ||
                (mlen == 8 && memcmp(method, "free_ptr", 8) == 0)) {
                if (call->call.arg_count > 0) {
                    return handle_key_from_expr(call->call.args[0], key_buf, key_bufsize);
                }
            }
        }
        /* defer free(p) — bare free call */
        if (call->call.callee && call->call.callee->kind == NODE_IDENT &&
            call->call.callee->ident.name_len == 4 &&
            memcmp(call->call.callee->ident.name, "free", 4) == 0 &&
            call->call.arg_count > 0) {
            return handle_key_from_expr(call->call.args[0], key_buf, key_bufsize);
        }
    }
    return 0;
}

/* Scan a defer body for ALL free/delete calls. Marks each found handle as FREED.
 * Handles single-statement defers, block defers, AND nested control flow
 * (if/else/for/while/switch bodies — conservative: any reachable free counts).
 *
 * Conservative simplification: treat ANY free inside nested control flow as
 * "handle freed on scope exit." This may miss some double-free detection for
 * conditional frees, but defers run at scope exit so the handle goes out of
 * scope regardless. Avoids false-positive leak warnings (BUG-608). */
static void defer_scan_all_frees(Node *node, PathState *ps, int defer_line) {
    if (!node) return;
    char key_buf[128];
    int klen = defer_stmt_is_free(node, key_buf, sizeof(key_buf));
    if (klen > 0) {
        HandleInfo *h = find_handle(ps, key_buf, (uint32_t)klen);
        if (h && (h->state == HS_ALIVE || h->state == HS_MAYBE_FREED)) {
            h->state = HS_FREED;
            h->free_line = defer_line;
        }
    }
    switch (node->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            defer_scan_all_frees(node->block.stmts[i], ps, defer_line);
        break;
    case NODE_IF:
        defer_scan_all_frees(node->if_stmt.then_body, ps, defer_line);
        defer_scan_all_frees(node->if_stmt.else_body, ps, defer_line);
        break;
    case NODE_FOR:
        defer_scan_all_frees(node->for_stmt.body, ps, defer_line);
        break;
    case NODE_WHILE: case NODE_DO_WHILE:
        defer_scan_all_frees(node->while_stmt.body, ps, defer_line);
        break;
    case NODE_SWITCH:
        for (int i = 0; i < node->switch_stmt.arm_count; i++)
            defer_scan_all_frees(node->switch_stmt.arms[i].body, ps, defer_line);
        break;
    case NODE_CRITICAL:
        defer_scan_all_frees(node->critical.body, ps, defer_line);
        break;
    case NODE_ONCE:
        defer_scan_all_frees(node->once.body, ps, defer_line);
        break;
    default:
        break;
    }
}

/* ---- AST walking ---- */

static void zc_check_expr(ZerCheck *zc, PathState *ps, Node *node);
static void zc_check_stmt(ZerCheck *zc, PathState *ps, Node *node);
static void zc_apply_summary(ZerCheck *zc, PathState *ps, Node *call_node);

/* check if a call is pool.alloc/get/free and track state */
static void zc_check_call(ZerCheck *zc, PathState *ps, Node *node) {
    if (!node || node->kind != NODE_CALL) return;
    if (node->call.callee->kind != NODE_FIELD) return;

    Node *obj = node->call.callee->field.object;
    const char *method = node->call.callee->field.field_name;
    uint32_t mlen = (uint32_t)node->call.callee->field.field_name_len;

    if (obj->kind != NODE_IDENT) return;
    const char *pool_name = obj->ident.name;
    uint32_t plen = (uint32_t)obj->ident.name_len;

    /* check if this is a pool variable — try checker type first, then global scope */
    Type *pool_type = checker_get_type(zc->checker, obj);
    if (!pool_type) {
        Symbol *sym = scope_lookup(zc->checker->global_scope, pool_name, plen);
        if (sym) pool_type = sym->type;
    }
    /* Task.free(h) / Task.free_ptr(p) — auto-Slab free via struct method */
    if (pool_type && pool_type->kind == TYPE_STRUCT) {
        if ((mlen == 4 && memcmp(method, "free", 4) == 0) ||
            (mlen == 8 && memcmp(method, "free_ptr", 8) == 0)) {
            if (node->call.arg_count > 0) {
                const char *hkey;
                int hklen = handle_key_arena(zc, node->call.args[0], &hkey);
                if (hklen > 0) {
                    HandleInfo *h = find_handle(ps, hkey, (uint32_t)hklen);
                    if (h) {
                        if (h->state == HS_FREED) {
                            zc_error(zc, node->loc.line,
                                "double free: '%.*s' already freed at line %d",
                                hklen, hkey, h->free_line);
                        } else if (h->state == HS_MAYBE_FREED) {
                            zc_error(zc, node->loc.line,
                                "double free: '%.*s' may have been freed at line %d",
                                hklen, hkey, h->free_line);
                        }
                        h->state = HS_FREED;
                        h->free_line = node->loc.line;
                        /* propagate to aliases */
                        for (int ai = 0; ai < ps->handle_count; ai++) {
                            if (&ps->handles[ai] != h &&
                                ps->handles[ai].alloc_line == h->alloc_line &&
                                ps->handles[ai].pool_id == h->pool_id &&
                                ps->handles[ai].state == HS_ALIVE) {
                                ps->handles[ai].state = HS_FREED;
                                ps->handles[ai].free_line = node->loc.line;
                            }
                        }
                    } else {
                        /* untracked — register as FREED (9a pattern) */
                        char *akey = (char *)arena_alloc(zc->arena, hklen + 1);
                        if (akey) {
                            memcpy(akey, hkey, hklen + 1);
                            h = add_handle(ps, akey, (uint32_t)hklen);
                            if (h) {
                                h->state = HS_FREED;
                                h->pool_id = -2;
                                h->free_line = node->loc.line;
                            }
                        }
                    }
                }
            }
        }
        /* Task.new() alloc tracking handled in zc_check_var_init via alloc pattern */
        return;
    }

    if (!pool_type || (pool_type->kind != TYPE_POOL && pool_type->kind != TYPE_SLAB)) return;

    int pool_id = register_pool(zc, pool_name, plen);

    /* pool.free(h) — mark handle as freed
     * BUG-357: also handles arr[0], s.h via handle_key_from_expr */
    if ((mlen == 4 && memcmp(method, "free", 4) == 0) ||
        (mlen == 8 && memcmp(method, "free_ptr", 8) == 0)) {
        if (node->call.arg_count > 0) {
            const char *hkey;
            int hklen = handle_key_arena(zc, node->call.args[0], &hkey);
            if (hklen > 0) {
                HandleInfo *h = find_handle(ps, hkey, (uint32_t)hklen);
                if (h) {
                    if (h->state == HS_FREED) {
                        zc_error(zc, node->loc.line,
                            "double free: '%.*s' already freed at line %d",
                            hklen, hkey, h->free_line);
                    } else if (h->state == HS_MAYBE_FREED) {
                        zc_error(zc, node->loc.line,
                            "double free: '%.*s' may have been freed at line %d",
                            hklen, hkey, h->free_line);
                    }
                    h->state = HS_FREED;
                    h->free_line = node->loc.line;
                    /* propagate freed state to all aliases (same pool + same alloc line) */
                    for (int i = 0; i < ps->handle_count; i++) {
                        if (&ps->handles[i] != h &&
                            ps->handles[i].pool_id == h->pool_id &&
                            ps->handles[i].alloc_line == h->alloc_line &&
                            ps->handles[i].state == HS_ALIVE) {
                            ps->handles[i].state = HS_FREED;
                            ps->handles[i].free_line = node->loc.line;
                        }
                    }
                }
            }
        }
    }

    /* pool.get(h) — check handle is alive and correct pool
     * BUG-357: also handles arr[0], s.h via handle_key_from_expr */
    if (mlen == 3 && memcmp(method, "get", 3) == 0) {
        if (node->call.arg_count > 0) {
            const char *hkey;
            int hklen = handle_key_arena(zc, node->call.args[0], &hkey);
            if (hklen > 0) {
                HandleInfo *h = find_handle(ps, hkey, (uint32_t)hklen);
                if (h) {
                    if (h->state == HS_FREED) {
                        zc_error(zc, node->loc.line,
                            "use-after-free: '%.*s' freed at line %d",
                            hklen, hkey, h->free_line);
                    } else if (h->state == HS_MAYBE_FREED) {
                        zc_error(zc, node->loc.line,
                            "use-after-free: '%.*s' may have been freed at line %d",
                            hklen, hkey, h->free_line);
                    }
                    if (h->pool_id >= 0 && h->pool_id != pool_id) {
                        zc_error(zc, node->loc.line,
                            "wrong pool: '%.*s' allocated from pool %d, used on pool %d",
                            hklen, hkey, h->pool_id, pool_id);
                    }
                }
            }
        }
    }
}

/* Determine allocation color of a function's return value.
 * Transitive: if f() returns g(), and g() returns arena.alloc(), f is ARENA.
 * Cached on Symbol.returns_color_cache to avoid re-scanning. Depth-limited. */
static int _returns_color_depth = 0;

static int func_returns_color_by_name(ZerCheck *zc, const char *fname, uint32_t flen) {
    if (_returns_color_depth > 8) return ZC_COLOR_UNKNOWN;
    Symbol *sym = scope_lookup(zc->checker->global_scope, fname, flen);
    if (!sym || !sym->func_node || !sym->func_node->func_decl.body) return ZC_COLOR_UNKNOWN;
    /* Check cache — returns_color_cached is stored on Symbol */
    if (sym->returns_color_cached) return sym->returns_color_value;

    Node *body = sym->func_node->func_decl.body;
    if (body->kind != NODE_BLOCK) return ZC_COLOR_UNKNOWN;

    int result_color = -1; /* -1 = no returns seen */
    _returns_color_depth++;

    for (int i = 0; i < body->block.stmt_count; i++) {
        Node *stmt = body->block.stmts[i];
        if (stmt->kind != NODE_RETURN || !stmt->ret.expr) continue;
        Node *ret_expr = stmt->ret.expr;
        if (ret_expr->kind == NODE_ORELSE) ret_expr = ret_expr->orelse.expr;
        if (ret_expr->kind == NODE_NULL_LIT) continue;

        int this_color = ZC_COLOR_UNKNOWN;

        /* Direct arena/pool/slab.alloc() */
        if (ret_expr->kind == NODE_CALL && ret_expr->call.callee &&
            ret_expr->call.callee->kind == NODE_FIELD) {
            Node *obj = ret_expr->call.callee->field.object;
            const char *mn = ret_expr->call.callee->field.field_name;
            uint32_t ml = (uint32_t)ret_expr->call.callee->field.field_name_len;
            if (obj && ((ml == 5 && memcmp(mn, "alloc", 5) == 0) ||
                        (ml == 9 && memcmp(mn, "alloc_ptr", 9) == 0) ||
                        (ml == 11 && memcmp(mn, "alloc_slice", 11) == 0))) {
                Type *obj_type = checker_get_type(zc->checker, obj);
                if (!obj_type && obj->kind == NODE_IDENT) {
                    Symbol *osym = scope_lookup(zc->checker->global_scope,
                        obj->ident.name, (uint32_t)obj->ident.name_len);
                    if (osym) obj_type = osym->type;
                }
                if (obj_type && obj_type->kind == TYPE_ARENA) this_color = ZC_COLOR_ARENA;
                else if (obj_type && (obj_type->kind == TYPE_POOL || obj_type->kind == TYPE_SLAB))
                    this_color = ZC_COLOR_POOL;
            }
        }

        /* Transitive: return calls another function — recurse */
        if (this_color == ZC_COLOR_UNKNOWN && ret_expr->kind == NODE_CALL) {
            Node *cc = ret_expr->call.callee;
            const char *cfn = NULL; uint32_t cfl = 0;
            if (cc && cc->kind == NODE_IDENT) { cfn = cc->ident.name; cfl = (uint32_t)cc->ident.name_len; }
            else if (cc && cc->kind == NODE_FIELD) { cfn = cc->field.field_name; cfl = (uint32_t)cc->field.field_name_len; }
            if (cfn) this_color = func_returns_color_by_name(zc, cfn, cfl);
        }

        /* Param color inference: return is cast/direct of a parameter.
         * Walk through casts to find root ident, check if it's a param. */
        if (this_color == ZC_COLOR_UNKNOWN) {
            Node *root = ret_expr;
            while (root) {
                if (root->kind == NODE_TYPECAST) root = root->typecast.expr;
                else if (root->kind == NODE_INTRINSIC && root->intrinsic.arg_count > 0)
                    root = root->intrinsic.args[root->intrinsic.arg_count - 1];
                else break;
            }
            if (root && root->kind == NODE_IDENT) {
                Node *fn = sym->func_node;
                for (int pi = 0; pi < fn->func_decl.param_count; pi++) {
                    if (fn->func_decl.params[pi].name_len == root->ident.name_len &&
                        memcmp(fn->func_decl.params[pi].name, root->ident.name,
                               root->ident.name_len) == 0) {
                        /* return is cast of param[pi] — mark for color passthrough */
                        if (sym->returns_param_color == 0)
                            sym->returns_param_color = pi + 1; /* +1 so 0 = unset */
                        else if (sym->returns_param_color != pi + 1)
                            sym->returns_param_color = -1; /* mixed params */
                        this_color = -2; /* sentinel: "inherits from caller" */
                        break;
                    }
                }
            }
        }

        /* Merge: first return sets color, mismatch → UNKNOWN */
        if (this_color == -2) { /* param color — don't merge with alloc color */ }
        else if (result_color == -1) result_color = this_color;
        else if (result_color != this_color) { result_color = ZC_COLOR_UNKNOWN; break; }
    }

    _returns_color_depth--;
    if (result_color == -1) result_color = ZC_COLOR_UNKNOWN;

    /* Cache result */
    sym->returns_color_cached = true;
    sym->returns_color_value = result_color;
    /* returns_param_color already set during scan (0 = unset, -1 = mixed, 1+ = param index + 1) */
    return result_color;
}

static int func_returns_color(ZerCheck *zc, Node *call) {
    if (!call || call->kind != NODE_CALL) return ZC_COLOR_UNKNOWN;
    Node *callee = call->call.callee;
    const char *fname = NULL; uint32_t flen = 0;
    if (callee && callee->kind == NODE_IDENT) { fname = callee->ident.name; flen = (uint32_t)callee->ident.name_len; }
    else if (callee && callee->kind == NODE_FIELD) { fname = callee->field.field_name; flen = (uint32_t)callee->field.field_name_len; }
    if (!fname) return ZC_COLOR_UNKNOWN;
    return func_returns_color_by_name(zc, fname, flen);
}

/* check if an assignment is h = pool.alloc() — register handle */
static void zc_check_var_init(ZerCheck *zc, PathState *ps, Node *var_node) {
    if (!var_node) return;
    const char *vname = var_node->var_decl.name;
    uint32_t vlen = (uint32_t)var_node->var_decl.name_len;

    Node *init = var_node->var_decl.init;
    if (!init) return;

    /* h = pool.alloc() orelse ... */
    Node *alloc_call = init;
    if (init->kind == NODE_ORELSE) {
        alloc_call = init->orelse.expr;
    }

    if (alloc_call && alloc_call->kind == NODE_CALL &&
        alloc_call->call.callee->kind == NODE_FIELD) {
        Node *obj = alloc_call->call.callee->field.object;
        const char *method = alloc_call->call.callee->field.field_name;
        uint32_t mlen = (uint32_t)alloc_call->call.callee->field.field_name_len;

        if (((mlen == 5 && memcmp(method, "alloc", 5) == 0) ||
             (mlen == 9 && memcmp(method, "alloc_ptr", 9) == 0)) &&
            obj->kind == NODE_IDENT) {
            /* check if object is Pool/Slab (not Arena — arena allocs don't need individual free) */
            Type *obj_type = checker_get_type(zc->checker, obj);
            if (!obj_type) {
                Symbol *sym = scope_lookup(zc->checker->global_scope,
                    obj->ident.name, (uint32_t)obj->ident.name_len);
                if (sym) obj_type = sym->type;
            }
            if (obj_type && obj_type->kind == TYPE_ARENA) {
                /* arena.alloc() — skip handle tracking */
            } else {
            /* this is h = pool.alloc() */
            int pool_id = register_pool(zc, obj->ident.name,
                (uint32_t)obj->ident.name_len);
            HandleInfo *h = find_handle_local(ps, vname, vlen);
            if (h && h->state == HS_ALIVE) {
                zc_error(zc, var_node->loc.line,
                    "handle leak: '%.*s' overwritten while alive (allocated at line %d) — previous handle leaked",
                    (int)vlen, vname, h->alloc_line);
            }
            if (!h) h = add_handle(ps, vname, vlen);
            if (h) {
                h->state = HS_ALIVE;
                h->pool_id = pool_id;
                h->alloc_line = var_node->loc.line;
                h->alloc_id = zc->next_alloc_id++;
                h->source_color = ZC_COLOR_POOL;
            }
            } /* end else (not arena) */
        }
    }

    /* Level 1: *opaque p = malloc/calloc/strdup(...) — register as ALIVE */
    if (alloc_call && is_alloc_call(zc, alloc_call)) {
        HandleInfo *h = find_handle_local(ps, vname, vlen);
        if (h && h->state == HS_ALIVE) {
            zc_error(zc, var_node->loc.line,
                "pointer leak: '%.*s' overwritten while alive (allocated at line %d)",
                (int)vlen, vname, h->alloc_line);
        }
        if (!h) h = add_handle(ps, vname, vlen);
        if (h) {
            h->state = HS_ALIVE;
            h->pool_id = -2; /* -2 = malloc'd (not pool, not param) */
            h->alloc_line = var_node->loc.line;
            h->alloc_id = zc->next_alloc_id++;
            h->source_color = ZC_COLOR_MALLOC;
        }
    }

    /* Wrapper allocator: *opaque r = create_resource() orelse ...
     * Any function call (with body or not) returning ?*T or ?*opaque
     * that wasn't caught by is_alloc_call or pool.alloc patterns above.
     * EXCLUDE arena.alloc() — arena uses bulk reset, not individual free. */
    bool is_arena_alloc = false;
    if (alloc_call && alloc_call->kind == NODE_CALL &&
        alloc_call->call.callee && alloc_call->call.callee->kind == NODE_FIELD) {
        Node *aobj = alloc_call->call.callee->field.object;
        if (aobj && aobj->kind == NODE_IDENT) {
            Type *atype = checker_get_type(zc->checker, aobj);
            if (!atype) {
                Symbol *asym = scope_lookup(zc->checker->global_scope,
                    aobj->ident.name, (uint32_t)aobj->ident.name_len);
                if (asym) atype = asym->type;
            }
            if (atype && atype->kind == TYPE_ARENA) is_arena_alloc = true;
        }
    }
    /* Allocation coloring: check callee's return color transitively.
     * Arena wrappers (even chained: app → driver → hal → arena.alloc)
     * return ZC_COLOR_ARENA → skip handle tracking. */
    int callee_color = ZC_COLOR_UNKNOWN;
    if (!is_arena_alloc && alloc_call && alloc_call->kind == NODE_CALL) {
        callee_color = func_returns_color(zc, alloc_call);
        if (callee_color == ZC_COLOR_ARENA) is_arena_alloc = true;

        /* Param color inference: if callee returns cast of param[N],
         * the result is an ALIAS of the arg — same alloc_id, same color.
         * Covers *opaque adapter functions like opaque_to_block(). */
        if (callee_color == ZC_COLOR_UNKNOWN) {
            Node *cc = alloc_call->call.callee;
            const char *cfn = NULL; uint32_t cfl = 0;
            if (cc && cc->kind == NODE_IDENT) { cfn = cc->ident.name; cfl = (uint32_t)cc->ident.name_len; }
            else if (cc && cc->kind == NODE_FIELD) { cfn = cc->field.field_name; cfl = (uint32_t)cc->field.field_name_len; }
            if (cfn) {
                Symbol *callee_sym = scope_lookup(zc->checker->global_scope, cfn, cfl);
                if (callee_sym && callee_sym->returns_color_cached &&
                    callee_sym->returns_param_color > 0) {
                    int param_idx = callee_sym->returns_param_color - 1;
                    if (param_idx < alloc_call->call.arg_count) {
                        const char *akey;
                        int aklen = handle_key_arena(zc, alloc_call->call.args[param_idx], &akey);
                        if (aklen > 0) {
                            HandleInfo *arg_h = find_handle(ps, akey, (uint32_t)aklen);
                            if (arg_h) {
                                /* Make result an alias of the arg — same allocation */
                                HandleInfo *dst = find_handle_local(ps, vname, vlen);
                                if (!dst) dst = add_handle(ps, vname, vlen);
                                if (dst) {
                                    dst->state = arg_h->state;
                                    dst->pool_id = arg_h->pool_id;
                                    dst->alloc_line = arg_h->alloc_line;
                                    dst->free_line = arg_h->free_line;
                                    dst->alloc_id = arg_h->alloc_id;
                                    dst->source_color = arg_h->source_color;
                                }
                                callee_color = arg_h->source_color;
                                if (callee_color == ZC_COLOR_ARENA) is_arena_alloc = true;
                                /* Skip wrapper allocator — already aliased */
                                is_arena_alloc = true; /* prevent double-registration below */
                            }
                        }
                    }
                }
            }
        }
    }
    if (alloc_call && alloc_call->kind == NODE_CALL &&
        !find_handle_local(ps, vname, vlen) && !is_arena_alloc) {
        Type *ret_type = checker_get_type(zc->checker, alloc_call);
        if (ret_type) {
            Type *ret_eff = type_unwrap_distinct(ret_type);
            /* Check for ?*T, ?*opaque, *T, *opaque return */
            bool is_ptr_return = false;
            if (ret_eff->kind == TYPE_POINTER || ret_eff->kind == TYPE_OPAQUE)
                is_ptr_return = true;
            if (ret_eff->kind == TYPE_OPTIONAL) {
                Type *inner = type_unwrap_distinct(ret_eff->optional.inner);
                if (inner && (inner->kind == TYPE_POINTER || inner->kind == TYPE_OPAQUE))
                    is_ptr_return = true;
            }
            if (is_ptr_return) {
                HandleInfo *h = add_handle(ps, vname, vlen);
                if (h) {
                    h->state = HS_ALIVE;
                    h->pool_id = -2;
                    h->alloc_line = var_node->loc.line;
                    h->alloc_id = zc->next_alloc_id++;
                }
            }
        }
    }

    /* Interior pointer aliasing: *u32 p = &b.field — p is derived from b.
     * If b is tracked (alloc_ptr), p gets same alloc_id. When b is freed,
     * p is also FREED. Catches UAF via field-derived interior pointers. */
    {
        Node *addr_src = init;
        if (addr_src && addr_src->kind == NODE_UNARY &&
            addr_src->unary.op == TOK_AMP && addr_src->unary.operand) {
            Node *operand = addr_src->unary.operand;
            /* Walk to the root ident through field/index chains */
            Node *root = operand;
            while (root) {
                if (root->kind == NODE_FIELD) root = root->field.object;
                else if (root->kind == NODE_INDEX) root = root->index_expr.object;
                else if (root->kind == NODE_UNARY && root->unary.op == TOK_STAR)
                    root = root->unary.operand;
                else break;
            }
            if (root && root->kind == NODE_IDENT) {
                const char *rkey;
                int rklen = handle_key_arena(zc, root, &rkey);
                if (rklen > 0) {
                    HandleInfo *src = find_handle(ps, rkey, (uint32_t)rklen);
                    if (src) {
                        HandleInfo *dst = find_handle_local(ps, vname, vlen);
                        if (!dst) dst = add_handle(ps, vname, vlen);
                        if (dst) {
                            dst->state = src->state;
                            dst->pool_id = src->pool_id;
                            dst->alloc_line = src->alloc_line;
                            dst->free_line = src->free_line;
                            dst->alloc_id = src->alloc_id;
                            dst->source_color = src->source_color;
                        }
                    }
                }
            }
        }
    }

    /* Handle aliasing: Handle(T) alias = existing_handle;
     * BUG-357: also match NODE_INDEX and NODE_FIELD on init side.
     * Also: @ptrcast alias — *RealData r = @ptrcast(*RealData, handle)
     * makes r an alias of handle. Freeing r = freeing handle. */
    {
        Node *alias_src = init;
        /* Walk through @ptrcast/@bitcast/NODE_TYPECAST to find the actual source */
        while (alias_src) {
            if (alias_src->kind == NODE_INTRINSIC && alias_src->intrinsic.arg_count > 0)
                alias_src = alias_src->intrinsic.args[alias_src->intrinsic.arg_count - 1];
            else if (alias_src->kind == NODE_TYPECAST)
                alias_src = alias_src->typecast.expr;
            else if (alias_src->kind == NODE_ORELSE)
                alias_src = alias_src->orelse.expr;
            else break;
        }

        const char *src_key;
        int sklen = handle_key_arena(zc, alias_src, &src_key);
        if (sklen > 0) {
            HandleInfo *src = find_handle(ps, src_key, (uint32_t)sklen);
            if (src) {
                HandleInfo *dst = find_handle_local(ps, vname, vlen);
                if (!dst) dst = add_handle(ps, vname, vlen);
                if (dst) {
                    dst->state = src->state;
                    dst->pool_id = src->pool_id;
                    dst->alloc_line = src->alloc_line;
                    dst->free_line = src->free_line;
                    dst->alloc_id = src->alloc_id;
                            dst->source_color = src->source_color; /* same allocation */
                }
            }
            /* Struct copy aliasing: if source has tracked fields like "s1.h",
             * create aliases "s2.h" for each. Catches UAF via struct copy. */
            for (int hi = 0; hi < ps->handle_count; hi++) {
                if (ps->handles[hi].name_len > (uint32_t)sklen + 1 &&
                    memcmp(ps->handles[hi].name, src_key, sklen) == 0 &&
                    ps->handles[hi].name[sklen] == '.') {
                    /* found "s1.field" — create "s2.field" */
                    const char *suffix = ps->handles[hi].name + sklen;
                    uint32_t suf_len = ps->handles[hi].name_len - (uint32_t)sklen;
                    uint32_t new_len = vlen + suf_len;
                    char *akey = (char *)arena_alloc(zc->arena, new_len + 1);
                    if (akey) {
                        memcpy(akey, vname, vlen);
                        memcpy(akey + vlen, suffix, suf_len);
                        akey[new_len] = '\0';
                        HandleInfo *dst = find_handle(ps, akey, new_len);
                        if (!dst) dst = add_handle(ps, akey, new_len);
                        if (dst) {
                            dst->state = ps->handles[hi].state;
                            dst->pool_id = ps->handles[hi].pool_id;
                            dst->alloc_line = ps->handles[hi].alloc_line;
                            dst->free_line = ps->handles[hi].free_line;
                            dst->alloc_id = ps->handles[hi].alloc_id;
                            dst->source_color = ps->handles[hi].source_color;
                        }
                    }
                }
            }
        }
    }

    /* Move struct transfer handled AFTER zc_check_expr in zc_check_stmt(NODE_VAR_DECL)
     * to avoid false positive (source checked as ALIVE during expr walk). */
}

/* Check if a type is a move struct */
static bool is_move_struct_type(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    /* SAFETY: zer_type_kind_is_move_struct in src/safety/move_rules.c */
    int is_move_flag = (eff->kind == TYPE_STRUCT && eff->struct_type.is_move) ? 1 : 0;
    return zer_type_kind_is_move_struct(eff->kind, is_move_flag) != 0;
}

/* Check if a type contains a move struct field (one level deep).
 * A regular struct with a move struct field should propagate transfer
 * when the outer struct is passed to a function or assigned. */
static bool contains_move_struct_field(Type *t) {
    if (!t) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind == TYPE_STRUCT) {
        for (uint32_t i = 0; i < eff->struct_type.field_count; i++) {
            if (is_move_struct_type(eff->struct_type.fields[i].type))
                return true;
        }
    }
    /* Red Team V22: union containing move struct variant */
    if (eff->kind == TYPE_UNION) {
        for (uint32_t i = 0; i < eff->union_type.variant_count; i++) {
            if (is_move_struct_type(eff->union_type.variants[i].type))
                return true;
        }
    }
    return false;
}

/* ---- Unified helpers (Option A refactor) ----
 * These prevent the BUG-468/469 class of bugs where a new state or type
 * pattern is added but not propagated to all check/merge sites.
 * New states: add to is_handle_invalid + is_handle_consumed.
 * New move-like types: add to should_track_move.
 * Error messages: update zc_report_invalid_use ONCE. */

/* Should this type be tracked for move/transfer semantics?
 * Covers: move struct directly, or regular struct containing move struct field. */
static bool should_track_move(Type *t) {
    if (!t) return false;
    /* SAFETY: zer_move_should_track in src/safety/move_rules.c */
    int is_move = is_move_struct_type(t) ? 1 : 0;
    int has_field = contains_move_struct_field(t) ? 1 : 0;
    return zer_move_should_track(is_move, has_field) != 0;
}

/* Is this handle in any state where use is invalid?
 * Used for all use-after-free/move/transfer checks.
 * Delegates to the VST-verified predicate in src/safety/handle_state.c. */
static bool is_handle_invalid(HandleInfo *h) {
    if (!h) return false;
    return zer_handle_state_is_invalid(h->state) != 0;
}

/* Is this handle consumed (freed, maybe-freed, or transferred)?
 * Used for path merge decisions (if/else, switch, loop).
 * Same 3-state semantic as is_handle_invalid; delegates to the same
 * VST-verified predicate. Kept as a distinct function for call-site
 * readability. */
static bool is_handle_consumed(HandleInfo *h) {
    return zer_handle_state_is_invalid(h->state) != 0;
}

/* Report invalid use with correct message based on handle state.
 * Centralizes error messages — add new states here ONCE. */
static void zc_report_invalid_use(ZerCheck *zc, HandleInfo *h, int line,
                                   const char *key, int key_len) {
    if (h->state == HS_FREED) {
        zc_error(zc, line, "use-after-free: '%.*s' freed at line %d",
                 key_len, key, h->free_line);
    } else if (h->state == HS_MAYBE_FREED) {
        if (h->pool_id == -3) {
            zc_error(zc, line, "use after move: '%.*s' may have been moved on a previous path",
                     key_len, key);
        } else {
            zc_error(zc, line, "use-after-free: '%.*s' may have been freed at line %d",
                     key_len, key, h->free_line);
        }
    } else if (h->state == HS_TRANSFERRED) {
        if (h->pool_id == -3) {
            zc_error(zc, line, "use after move: '%.*s' ownership transferred at line %d",
                     key_len, key, h->transfer_line);
        } else {
            zc_error(zc, line, "use after transfer: '%.*s' ownership transferred to thread at line %d",
                     key_len, key, h->transfer_line);
        }
    }
}

/* Lazily register a move struct variable in PathState if not already tracked.
 * Called when we encounter a NODE_IDENT whose type is a move struct.
 * Uses find_handle (any scope) — this is a USE site, not a declaration site.
 * Shadow handling only at NODE_VAR_DECL (via find_handle_local). */
static HandleInfo *zc_ensure_move_registered(ZerCheck *zc, PathState *ps,
                                              const char *name, uint32_t len, int line) {
    HandleInfo *h = find_handle(ps, name, len);
    if (h) return h;
    h = add_handle(ps, name, len);
    if (h) {
        h->state = HS_ALIVE;
        h->pool_id = -3;
        h->alloc_line = line;
        h->alloc_id = zc->next_alloc_id++;
        h->source_color = ZC_COLOR_UNKNOWN;
    }
    return h;
}

static void zc_check_expr(ZerCheck *zc, PathState *ps, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_CALL:
        zc_check_call(zc, ps, node);
        zc_apply_summary(zc, ps, node);
        /* Level 1: detect free(*opaque) — track as FREED */
        {
            char fkey[128];
            int fklen = 0;
            if (is_free_call(zc, node, fkey, &fklen, sizeof(fkey))) {
                HandleInfo *h = find_handle(ps, fkey, (uint32_t)fklen);
                if (h) {
                    if (h->state == HS_FREED) {
                        zc_error(zc, node->loc.line,
                            "double free: '%.*s' already freed at line %d",
                            fklen, fkey, h->free_line);
                    } else if (h->state == HS_MAYBE_FREED) {
                        zc_error(zc, node->loc.line,
                            "double free: '%.*s' may have been freed at line %d",
                            fklen, fkey, h->free_line);
                    }
                    h->state = HS_FREED;
                    h->free_line = node->loc.line;
                    /* propagate to aliases (same alloc_line) */
                    for (int ai = 0; ai < ps->handle_count; ai++) {
                        if (&ps->handles[ai] != h &&
                            ps->handles[ai].alloc_line == h->alloc_line &&
                            ps->handles[ai].pool_id == h->pool_id &&
                            ps->handles[ai].state == HS_ALIVE) {
                            ps->handles[ai].state = HS_FREED;
                            ps->handles[ai].free_line = node->loc.line;
                        }
                    }
                } else {
                    /* 9a/9b: free() on untracked key (e.g., parameter's field).
                     * Register as FREED so subsequent use is caught. */
                    char *akey = (char *)arena_alloc(zc->arena, fklen + 1);
                    if (akey) {
                        memcpy(akey, fkey, fklen + 1);
                        h = add_handle(ps, akey, (uint32_t)fklen);
                        if (h) {
                            h->state = HS_FREED;
                            h->pool_id = -2;
                            h->free_line = node->loc.line;
                        }
                    }
                }
            }
        }
        /* ThreadHandle.join() — mark ThreadHandle as FREED (joined) */
        if (node->call.callee && node->call.callee->kind == NODE_FIELD) {
            const char *jmn = node->call.callee->field.field_name;
            uint32_t jml = (uint32_t)node->call.callee->field.field_name_len;
            if (jml == 4 && memcmp(jmn, "join", 4) == 0) {
                Node *jobj = node->call.callee->field.object;
                if (jobj && jobj->kind == NODE_IDENT) {
                    HandleInfo *jh = find_handle(ps, jobj->ident.name, (uint32_t)jobj->ident.name_len);
                    if (jh && jh->state == HS_ALIVE) {
                        jh->state = HS_FREED;
                        jh->free_line = node->loc.line;
                    } else if (jh && jh->state == HS_FREED) {
                        zc_error(zc, node->loc.line,
                            "thread already joined: '%.*s' joined at line %d",
                            (int)jobj->ident.name_len, jobj->ident.name, jh->free_line);
                    }
                }
            }
        }
        /* Check if any argument is a freed *opaque handle — UAF at call site.
         * Skip for free/delete calls (the arg IS what we're freeing). */
        {
            bool is_free_site = false;
            Node *cc = node->call.callee;
            if (cc && cc->kind == NODE_IDENT && cc->ident.name_len == 4 &&
                memcmp(cc->ident.name, "free", 4) == 0)
                is_free_site = true;
            if (cc && cc->kind == NODE_FIELD) {
                const char *mn = cc->field.field_name;
                uint32_t ml = (uint32_t)cc->field.field_name_len;
                if ((ml == 4 && memcmp(mn, "free", 4) == 0) ||
                    (ml == 8 && memcmp(mn, "free_ptr", 8) == 0))
                    is_free_site = true;
            }
            /* Also skip if callee has a summary or heuristic that frees params */
            const char *cc_name = NULL; uint32_t cc_nlen = 0;
            if (cc && cc->kind == NODE_IDENT) { cc_name = cc->ident.name; cc_nlen = (uint32_t)cc->ident.name_len; }
            else if (cc && cc->kind == NODE_FIELD) { cc_name = cc->field.field_name; cc_nlen = (uint32_t)cc->field.field_name_len; }
            if (!is_free_site && cc_name) {
                FuncSummary *cs = find_summary(zc, cc_name, cc_nlen);
                /* Only skip UAF check if summary says it actually frees a param */
                if (cs) {
                    for (int fi = 0; fi < cs->param_count && !is_free_site; fi++)
                        if (cs->frees_param[fi]) is_free_site = true;
                }
                /* heuristic: bodyless void func(*opaque) */
                if (!is_free_site) {
                    Symbol *csym = scope_lookup(zc->checker->global_scope,
                        cc_name, cc_nlen);
                    if (csym && csym->is_function && csym->func_node &&
                        !csym->func_node->func_decl.body && csym->type) {
                        Type *cret = csym->type;
                        if (cret->kind == TYPE_FUNC_PTR) cret = cret->func_ptr.ret;
                        if (cret && type_unwrap_distinct(cret)->kind == TYPE_VOID)
                            is_free_site = true;
                    }
                }
            }
            if (!is_free_site) {
                /* Two passes: first check+recurse, then transfer.
                 * Can't transfer during check — NODE_IDENT recurse would see
                 * the arg as already-transferred (false positive). */
                for (int i = 0; i < node->call.arg_count; i++) {
                    Node *arg = node->call.args[i];
                    if (arg && arg->kind == NODE_IDENT) {
                        const char *akey;
                        int aklen = handle_key_arena(zc, arg, &akey);
                        if (aklen > 0) {
                            HandleInfo *ah = find_handle(ps, akey, (uint32_t)aklen);
                            if (ah && (ah->state == HS_FREED || ah->state == HS_MAYBE_FREED)) {
                                zc_error(zc, node->loc.line,
                                    "use after free: '%.*s' %s at line %d — cannot pass to function",
                                    aklen, akey,
                                    ah->state == HS_FREED ? "freed" : "may have been freed",
                                    ah->free_line);
                            }
                            /* Move struct: check use-after-transfer */
                            if (!ah) {
                                Type *arg_type = checker_get_type(zc->checker, arg);
                                if (should_track_move(arg_type))
                                    ah = zc_ensure_move_registered(zc, ps,
                                        arg->ident.name, (uint32_t)arg->ident.name_len,
                                        node->loc.line);
                            }
                            if (ah && ah->state == HS_TRANSFERRED && ah->pool_id == -3) {
                                zc_error(zc, node->loc.line,
                                    "use after move: '%.*s' ownership transferred at line %d",
                                    aklen, akey, ah->transfer_line);
                            }
                        }
                    }
                    zc_check_expr(zc, ps, arg);
                }
                /* Second pass: mark move struct args as transferred AFTER check */
                for (int i = 0; i < node->call.arg_count; i++) {
                    Node *arg = node->call.args[i];
                    if (arg && arg->kind == NODE_IDENT) {
                        const char *akey;
                        int aklen = handle_key_arena(zc, arg, &akey);
                        if (aklen > 0) {
                            HandleInfo *ah = find_handle(ps, akey, (uint32_t)aklen);
                            /* Direct move struct — already tracked */
                            if (ah && ah->state == HS_ALIVE && ah->pool_id == -3) {
                                ah->state = HS_TRANSFERRED;
                                ah->transfer_line = node->loc.line;
                            }
                            /* Struct containing move field — lazy register + transfer */
                            if (!ah) {
                                Type *arg_type = checker_get_type(zc->checker, arg);
                                if (should_track_move(arg_type)) {
                                    ah = zc_ensure_move_registered(zc, ps,
                                        arg->ident.name, (uint32_t)arg->ident.name_len,
                                        node->loc.line);
                                    if (ah && ah->state == HS_ALIVE) {
                                        ah->state = HS_TRANSFERRED;
                                        ah->transfer_line = node->loc.line;
                                    }
                                }
                            }
                        }
                    }
                    /* Red Team V26: &move_struct as call arg → conservatively mark
                     * source as transferred. Pointer lets callee consume content. */
                    if (arg && arg->kind == NODE_UNARY && arg->unary.op == TOK_AMP &&
                        arg->unary.operand && arg->unary.operand->kind == NODE_IDENT) {
                        Node *src = arg->unary.operand;
                        Type *src_type = checker_get_type(zc->checker, src);
                        if (should_track_move(src_type)) {
                            HandleInfo *mh = zc_ensure_move_registered(zc, ps,
                                src->ident.name, (uint32_t)src->ident.name_len,
                                node->loc.line);
                            if (mh && mh->state == HS_ALIVE) {
                                mh->state = HS_TRANSFERRED;
                                mh->transfer_line = node->loc.line;
                            }
                        }
                    }
                }
            } else {
                for (int i = 0; i < node->call.arg_count; i++)
                    zc_check_expr(zc, ps, node->call.args[i]);
            }
        }
        break;
    case NODE_ASSIGN:
        zc_check_expr(zc, ps, node->assign.target);
        zc_check_expr(zc, ps, node->assign.value);
        /* BUG-487: union variant overwrite leaks move struct.
         * m.id = 100 when m.k (move struct) is ALIVE → resource leaked.
         * Check if target is union field and any sibling variant is tracked+ALIVE. */
        if (node->assign.target->kind == NODE_FIELD) {
            Node *obj = node->assign.target->field.object;
            if (obj && obj->kind == NODE_IDENT) {
                Type *obj_type = checker_get_type(zc->checker, obj);
                Type *obj_eff = obj_type ? type_unwrap_distinct(obj_type) : NULL;
                if (obj_eff && obj_eff->kind == TYPE_UNION) {
                    /* Check if any variant of this union has a tracked move struct */
                    const char *uname = obj->ident.name;
                    uint32_t ulen = (uint32_t)obj->ident.name_len;
                    const char *assigned_variant = node->assign.target->field.field_name;
                    uint32_t av_len = (uint32_t)node->assign.target->field.field_name_len;
                    for (int hi = 0; hi < ps->handle_count; hi++) {
                        HandleInfo *h = &ps->handles[hi];
                        if (h->state != HS_ALIVE || h->pool_id != -3) continue;
                        /* Check if this handle is a variant of the same union: "m.k" */
                        if (h->name_len > ulen + 1 &&
                            memcmp(h->name, uname, ulen) == 0 &&
                            h->name[ulen] == '.') {
                            /* Different variant than what we're assigning? */
                            const char *tracked_variant = h->name + ulen + 1;
                            uint32_t tv_len = h->name_len - ulen - 1;
                            if (tv_len != av_len || memcmp(tracked_variant, assigned_variant, av_len) != 0) {
                                zc_error(zc, node->loc.line,
                                    "union variant overwrite leaks move struct: '%.*s' is alive "
                                    "(allocated at line %d) — assigning to '%.*s.%.*s' overwrites it",
                                    (int)h->name_len, h->name, h->alloc_line,
                                    (int)ulen, uname, (int)av_len, assigned_variant);
                            }
                        }
                    }
                }
            }
        }
        /* BUG-361/357: handle assignment from pool.alloc() — works for globals,
         * array elements, struct fields.
         * arr[0] = pool.alloc() orelse return → register "arr[0]" in PathState */
        {
            const char *tkey;
            int tklen = handle_key_arena(zc, node->assign.target, &tkey);
            if (tklen > 0) {
                Node *val = node->assign.value;
                if (val->kind == NODE_ORELSE) val = val->orelse.expr;
                if (val && val->kind == NODE_CALL && val->call.callee &&
                    val->call.callee->kind == NODE_FIELD) {
                    const char *mname = val->call.callee->field.field_name;
                    uint32_t mlen = (uint32_t)val->call.callee->field.field_name_len;
                    if ((mlen == 5 && memcmp(mname, "alloc", 5) == 0) ||
                        (mlen == 9 && memcmp(mname, "alloc_ptr", 9) == 0)) {
                        Node *obj = val->call.callee->field.object;
                        int pool_id = (obj && obj->kind == NODE_IDENT) ?
                            register_pool(zc, obj->ident.name, (uint32_t)obj->ident.name_len) : -1;
                        /* arena-allocate key so HandleInfo.name pointer remains valid */
                        char *akey = (char *)arena_alloc(zc->arena, tklen + 1);
                        if (akey) {
                            memcpy(akey, tkey, tklen + 1);
                            HandleInfo *h = find_handle(ps, akey, (uint32_t)tklen);
                            if (h && h->state == HS_ALIVE) {
                                zc_error(zc, node->loc.line,
                                    "handle leak: '%.*s' overwritten while alive (allocated at line %d) — previous handle leaked",
                                    tklen, tkey, h->alloc_line);
                            }
                            if (!h) h = add_handle(ps, akey, (uint32_t)tklen);
                            if (h) {
                                h->state = HS_ALIVE;
                                h->pool_id = pool_id;
                                h->alloc_line = node->loc.line;
                                h->alloc_id = zc->next_alloc_id++;
                            }
                        }
                    }
                }
                /* 9a: *opaque struct field from malloc — ctx.data = malloc(64) */
                if (val && is_alloc_call(zc, val)) {
                    char *akey = (char *)arena_alloc(zc->arena, tklen + 1);
                    if (akey) {
                        memcpy(akey, tkey, tklen + 1);
                        HandleInfo *h = find_handle(ps, akey, (uint32_t)tklen);
                        if (!h) h = add_handle(ps, akey, (uint32_t)tklen);
                        if (h) {
                            h->state = HS_ALIVE;
                            h->pool_id = -2; /* malloc'd */
                            h->alloc_line = node->loc.line;
                            h->alloc_id = zc->next_alloc_id++;
                        }
                    }
                }
                /* Interior pointer aliasing via assignment: p = &b.field
                 * If b is tracked, p gets same alloc_id. */
                if (val && val->kind == NODE_UNARY &&
                    val->unary.op == TOK_AMP && val->unary.operand) {
                    Node *operand = val->unary.operand;
                    Node *root = operand;
                    while (root) {
                        if (root->kind == NODE_FIELD) root = root->field.object;
                        else if (root->kind == NODE_INDEX) root = root->index_expr.object;
                        else if (root->kind == NODE_UNARY && root->unary.op == TOK_STAR)
                            root = root->unary.operand;
                        else break;
                    }
                    if (root && root->kind == NODE_IDENT) {
                        const char *rkey;
                        int rklen = handle_key_arena(zc, root, &rkey);
                        if (rklen > 0) {
                            HandleInfo *src = find_handle(ps, rkey, (uint32_t)rklen);
                            if (src) {
                                char *akey = (char *)arena_alloc(zc->arena, tklen + 1);
                                if (akey) {
                                    memcpy(akey, tkey, tklen + 1);
                                    HandleInfo *dst = find_handle(ps, akey, (uint32_t)tklen);
                                    if (!dst) dst = add_handle(ps, akey, (uint32_t)tklen);
                                    if (dst) {
                                        dst->state = src->state;
                                        dst->pool_id = src->pool_id;
                                        dst->alloc_line = src->alloc_line;
                                        dst->free_line = src->free_line;
                                        dst->alloc_id = src->alloc_id;
                            dst->source_color = src->source_color;
                                    }
                                }
                            }
                        }
                    }
                }
                /* Handle aliasing via assignment: alias = tracked_handle
                 * BUG-357: supports arr[0] = h, s.h = other_h, etc.
                 * BUG-462: unwrap orelse/ptrcast/typecast to find source ident
                 * (ents[0] = m0 orelse return → source is m0, not the orelse node) */
                {
                    Node *alias_val = node->assign.value;
                    while (alias_val) {
                        if (alias_val->kind == NODE_ORELSE) alias_val = alias_val->orelse.expr;
                        else if (alias_val->kind == NODE_INTRINSIC && alias_val->intrinsic.arg_count > 0)
                            alias_val = alias_val->intrinsic.args[alias_val->intrinsic.arg_count - 1];
                        else if (alias_val->kind == NODE_TYPECAST) alias_val = alias_val->typecast.expr;
                        else break;
                    }
                    const char *skey;
                    int sklen = handle_key_arena(zc, alias_val, &skey);
                    if (sklen > 0) {
                        HandleInfo *src = find_handle(ps, skey, (uint32_t)sklen);
                        if (src) {
                            char *akey = (char *)arena_alloc(zc->arena, tklen + 1);
                            if (akey) {
                                memcpy(akey, tkey, tklen + 1);
                                HandleInfo *dst = find_handle(ps, akey, (uint32_t)tklen);
                                if (!dst) dst = add_handle(ps, akey, (uint32_t)tklen);
                                if (dst) {
                                    dst->state = src->state;
                                    dst->pool_id = src->pool_id;
                                    dst->alloc_line = src->alloc_line;
                                    dst->free_line = src->free_line;
                                    dst->alloc_id = src->alloc_id;
                            dst->source_color = src->source_color;
                                }
                            }
                        }
                    }
                }
                /* Move struct: assignment transfers ownership from source.
                 * y = x where x is a move struct → x becomes HS_TRANSFERRED.
                 * y is registered as new ALIVE owner. */
                {
                    Node *alias_val = node->assign.value;
                    while (alias_val) {
                        if (alias_val->kind == NODE_ORELSE) alias_val = alias_val->orelse.expr;
                        else break;
                    }
                    if (alias_val && alias_val->kind == NODE_IDENT) {
                        const char *skey;
                        int sklen = handle_key_arena(zc, alias_val, &skey);
                        if (sklen > 0) {
                            HandleInfo *src = find_handle(ps, skey, (uint32_t)sklen);
                            /* Lazy-register if this is a move struct we haven't seen yet */
                            if (!src) {
                                Type *val_type = checker_get_type(zc->checker, alias_val);
                                if (val_type && is_move_struct_type(val_type)) {
                                    src = zc_ensure_move_registered(zc, ps,
                                        alias_val->ident.name, (uint32_t)alias_val->ident.name_len,
                                        node->loc.line);
                                }
                            }
                            if (src && src->pool_id == -3 && src->state == HS_ALIVE) {
                                /* Source is a move struct — transfer ownership */
                                src->state = HS_TRANSFERRED;
                                src->transfer_line = node->loc.line;
                                /* Register target as new owner */
                                char *akey = (char *)arena_alloc(zc->arena, tklen + 1);
                                if (akey) {
                                    memcpy(akey, tkey, tklen + 1);
                                    HandleInfo *dst = find_handle(ps, akey, (uint32_t)tklen);
                                    if (!dst) dst = add_handle(ps, akey, (uint32_t)tklen);
                                    if (dst) {
                                        dst->state = HS_ALIVE;
                                        dst->pool_id = -3;
                                        dst->alloc_line = src->alloc_line;
                                        dst->alloc_id = src->alloc_id;
                                    }
                                }
                            }
                        }
                    }
                }
                /* Escape detection: if target root is global or pointer param,
                 * the source handle escapes to external scope. */
                {
                    Node *troot = node->assign.target;
                    while (troot && (troot->kind == NODE_FIELD || troot->kind == NODE_INDEX)) {
                        if (troot->kind == NODE_FIELD) troot = troot->field.object;
                        else troot = troot->index_expr.object;
                    }
                    if (troot && troot->kind == NODE_IDENT) {
                        /* Check if root is global */
                        bool is_global = scope_lookup(zc->checker->global_scope,
                            troot->ident.name, (uint32_t)troot->ident.name_len) != NULL;
                        /* Check if root is a pointer param (s.top = h where s is *Stack).
                         * Can't use scope_lookup (scope may have reset). Check type via checker. */
                        bool is_param_ptr = false;
                        if (!is_global && node->assign.target->kind == NODE_FIELD) {
                            Type *root_type = checker_get_type(zc->checker, troot);
                            if (root_type) {
                                Type *rt = type_unwrap_distinct(root_type);
                                if (rt && rt->kind == TYPE_POINTER)
                                    is_param_ptr = true;
                            }
                        }
                        if (is_global || is_param_ptr) {
                            Node *val = node->assign.value;
                            if (val->kind == NODE_ORELSE) val = val->orelse.expr;
                            const char *vkey;
                            int vklen = handle_key_arena(zc, val, &vkey);
                            if (vklen > 0) {
                                HandleInfo *vh = find_handle(ps, vkey, (uint32_t)vklen);
                                if (vh) vh->escaped = true;
                            }
                        }
                    }
                }
            } else {
                /* target untrackable (variable index, complex expression).
                 * If value is a tracked handle, mark it as escaped —
                 * the allocation went somewhere we can't follow. */
                Node *val = node->assign.value;
                if (val->kind == NODE_ORELSE) val = val->orelse.expr;
                const char *vkey;
                int vklen = handle_key_arena(zc, val, &vkey);
                if (vklen > 0) {
                    HandleInfo *vh = find_handle(ps, vkey, (uint32_t)vklen);
                    if (vh) vh->escaped = true;
                }
            }
        }
        break;
    case NODE_BINARY:
        zc_check_expr(zc, ps, node->binary.left);
        zc_check_expr(zc, ps, node->binary.right);
        break;
    case NODE_FIELD:
        /* Level 9: check if a tracked pointer (from alloc_ptr) is accessed after free.
         * BUG-463: check EVERY prefix of the field/index chain, not just root.
         * For h.inner.data: check "h", "h.inner", "h.inner.data" — any tracked
         * prefix that is FREED means the entire expression is UAF.
         * This catches struct field pointer aliases: h.inner = w; free(w); h.inner.data */
        {
            /* First: check the full expression (handles the simple case) */
            const char *fkey;
            int fklen = handle_key_arena(zc, node, &fkey);
            bool found_uaf = false;
            int cur_line = node->loc.line;
            if (fklen > 0) {
                HandleInfo *h = find_handle(ps, fkey, (uint32_t)fklen);
                /* Only report FREED if free happened on a STRICTLY earlier line.
                 * Same-line free = likely the free call's own argument (pool.free(s.h)
                 * marks s.h FREED then expression check re-visits s.h on same line). */
                if (is_handle_invalid(h) &&
                    (h->state != HS_FREED || h->free_line < cur_line)) {
                    zc_report_invalid_use(zc, h, cur_line, fkey, fklen);
                    found_uaf = true;
                }
            }
            /* Walk every prefix: for h.inner.data, check h.inner then h */
            if (!found_uaf) {
                Node *cur = node->field.object;
                while (cur && !found_uaf) {
                    int cklen = handle_key_arena(zc, cur, &fkey);
                    if (cklen > 0) {
                        HandleInfo *h = find_handle(ps, fkey, (uint32_t)cklen);
                        if (is_handle_invalid(h) &&
                            (h->state != HS_FREED || h->free_line < cur_line)) {
                            zc_report_invalid_use(zc, h, cur_line, fkey, cklen);
                            found_uaf = true;
                        }
                    }
                    /* Step up one level in the chain */
                    if (cur->kind == NODE_FIELD) cur = cur->field.object;
                    else if (cur->kind == NODE_INDEX) cur = cur->index_expr.object;
                    else break;
                }
            }
        }
        zc_check_expr(zc, ps, node->field.object);
        break;
    case NODE_ORELSE:
        zc_check_expr(zc, ps, node->orelse.expr);
        if (node->orelse.fallback)
            zc_check_expr(zc, ps, node->orelse.fallback);
        break;
    case NODE_INTRINSIC: {
        /* Level 1: check @ptrcast on freed *opaque */
        if (node->intrinsic.name_len == 7 &&
            memcmp(node->intrinsic.name, "ptrcast", 7) == 0 &&
            node->intrinsic.arg_count > 0) {
            Node *src = node->intrinsic.args[0];
            const char *skey;
            int sklen = handle_key_arena(zc, src, &skey);
            if (sklen > 0) {
                HandleInfo *h = find_handle(ps, skey, (uint32_t)sklen);
                if (is_handle_invalid(h))
                    zc_report_invalid_use(zc, h, node->loc.line, skey, sklen);
            }
        }
        /* recurse into intrinsic args */
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            zc_check_expr(zc, ps, node->intrinsic.args[i]);
        break;
    }
    case NODE_INDEX:
        /* Check if object being indexed is a freed pointer — p[0] after free.
         * Catches interior pointer UAF: *u32 p = &b.field; free(b); p[0]. */
        {
            Node *obj = node->index_expr.object;
            const char *ikey;
            int iklen = handle_key_arena(zc, obj, &ikey);
            if (iklen > 0) {
                HandleInfo *h = find_handle(ps, ikey, (uint32_t)iklen);
                if (is_handle_invalid(h))
                    zc_report_invalid_use(zc, h, node->loc.line, ikey, iklen);
            }
        }
        zc_check_expr(zc, ps, node->index_expr.object);
        if (node->index_expr.index)
            zc_check_expr(zc, ps, node->index_expr.index);
        break;
    case NODE_UNARY:
        /* Level 1: check deref on freed *opaque — *ptr after free */
        if (node->unary.op == TOK_STAR) {
            const char *dkey;
            int dklen = handle_key_arena(zc, node->unary.operand, &dkey);
            if (dklen > 0) {
                HandleInfo *h = find_handle(ps, dkey, (uint32_t)dklen);
                if (is_handle_invalid(h))
                    zc_report_invalid_use(zc, h, node->loc.line, dkey, dklen);
            }
        }
        zc_check_expr(zc, ps, node->unary.operand);
        break;
    case NODE_TYPECAST:
        zc_check_expr(zc, ps, node->typecast.expr);
        break;
    case NODE_STRUCT_INIT:
        for (int i = 0; i < node->struct_init.field_count; i++)
            zc_check_expr(zc, ps, node->struct_init.fields[i].value);
        break;
    case NODE_SLICE:
        zc_check_expr(zc, ps, node->slice.object);
        break;
    case NODE_IDENT:
        /* Move struct: lazy registration + use-after-transfer check */
        {
            Type *ident_type = checker_get_type(zc->checker, node);
            if (should_track_move(ident_type)) {
                const char *iname = node->ident.name;
                uint32_t ilen = (uint32_t)node->ident.name_len;
                HandleInfo *h = zc_ensure_move_registered(zc, ps, iname, ilen, node->loc.line);
                if (is_handle_invalid(h))
                    zc_report_invalid_use(zc, h, node->loc.line, iname, (int)ilen);
            }
        }
        break;
    /* Leaf expressions — no children with handle state */
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_CAST: case NODE_SIZEOF:
    /* Statements — handled by zc_check_stmt, not zc_check_expr */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL: case NODE_CONTAINER_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_VAR_DECL:
    case NODE_BLOCK: case NODE_IF: case NODE_FOR: case NODE_WHILE: case NODE_DO_WHILE:
    case NODE_SWITCH: case NODE_RETURN: case NODE_BREAK:
    case NODE_CONTINUE: case NODE_DEFER: case NODE_GOTO:
    case NODE_LABEL: case NODE_EXPR_STMT: case NODE_ASM:
    case NODE_CRITICAL: case NODE_ONCE: case NODE_SPAWN:
    case NODE_YIELD: case NODE_AWAIT: case NODE_STATIC_ASSERT:
        break;
    }
}

static void zc_check_stmt(ZerCheck *zc, PathState *ps, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_BLOCK: {
        /* BUG-488: scope depth tracking for variable shadowing.
         * Increment depth on entry, decrement on exit. Handles added
         * during this block get the new depth. On exit, remove handles
         * with depth > parent (inner scope variables out of scope). */
        int saved_handle_count = ps->handle_count;
        ps->scope_depth++;

        /* Backward goto UAF: scan block for labels, detect backward jumps.
         * A backward goto (label before goto) is like a loop body from
         * label to goto. Apply same 2-pass + widen-to-MAYBE_FREED pattern. */

        /* Phase 1: collect label positions in this block.
         * BUG-598: Stack-first dynamic pattern (CLAUDE.md rule #7): fixed
         * buffer for common case, realloc for blocks with >32 labels. Never
         * silently truncate — a dropped label means backward-goto UAF
         * detection could miss real bugs. */
        struct LabelRef { const char *name; uint32_t len; int idx; };
        struct LabelRef stack_labels[32];
        struct LabelRef *labels = stack_labels;
        int label_count = 0;
        int label_cap = 32;
        bool labels_heap = false;
        for (int i = 0; i < node->block.stmt_count; i++) {
            if (node->block.stmts[i]->kind == NODE_LABEL) {
                if (label_count >= label_cap) {
                    int new_cap = label_cap * 2;
                    struct LabelRef *new_labels = (struct LabelRef *)realloc(
                        labels_heap ? labels : NULL,
                        new_cap * sizeof(struct LabelRef));
                    if (!new_labels) break; /* OOM: stop collecting */
                    if (!labels_heap) {
                        memcpy(new_labels, stack_labels,
                               label_count * sizeof(struct LabelRef));
                    }
                    labels = new_labels;
                    label_cap = new_cap;
                    labels_heap = true;
                }
                labels[label_count].name = node->block.stmts[i]->label_stmt.name;
                labels[label_count].len = (uint32_t)node->block.stmts[i]->label_stmt.name_len;
                labels[label_count].idx = i;
                label_count++;
            }
        }

        /* Phase 2: walk statements, detect backward goto */
        for (int i = 0; i < node->block.stmt_count; i++) {
            zc_check_stmt(zc, ps, node->block.stmts[i]);

            /* after processing a goto, check if it jumps backward */
            if (node->block.stmts[i]->kind == NODE_GOTO) {
                const char *tgt = node->block.stmts[i]->goto_stmt.label;
                uint32_t tlen = (uint32_t)node->block.stmts[i]->goto_stmt.label_len;
                int label_idx = -1;
                for (int li = 0; li < label_count; li++) {
                    if (labels[li].len == tlen &&
                        memcmp(labels[li].name, tgt, tlen) == 0) {
                        label_idx = labels[li].idx;
                        break;
                    }
                }
                if (label_idx >= 0 && label_idx < i) {
                    /* backward goto = loop equivalent. Dynamic fixed-point:
                     * iterate until convergence. States form a finite lattice
                     * (5 values per handle, monotone transitions) so convergence
                     * is guaranteed. Ceiling of 32 is crash protection only. */
                    for (int iter = 0; iter < 32; iter++) {
                        PathState pre_goto = pathstate_copy(ps);
                        for (int j = label_idx; j <= i; j++)
                            zc_check_stmt(zc, ps, node->block.stmts[j]);
                        if (pathstate_equal(&pre_goto, ps)) {
                            pathstate_free(&pre_goto);
                            break;  /* converged */
                        }
                        pathstate_free(&pre_goto);
                    }
                }
            }
        }

        /* BUG-488: scope-exit — remove handles from this scope that shadow
         * outer handles. Keep non-shadowing handles (they need leak checking).
         * For shadowing handles: if same alloc_id as outer (alias), propagate
         * state. Otherwise, just remove (independent inner variable). */
        ps->scope_depth--;
        for (int hi = ps->handle_count - 1; hi >= 0; hi--) {
            HandleInfo *h = &ps->handles[hi];
            if (h->scope_depth <= ps->scope_depth) continue; /* not from this block */
            /* Check if this handle shadows an outer one with same name */
            for (int oi = 0; oi < ps->handle_count; oi++) {
                if (oi == hi) continue;
                if (ps->handles[oi].name_len == h->name_len &&
                    memcmp(ps->handles[oi].name, h->name, h->name_len) == 0 &&
                    ps->handles[oi].scope_depth < h->scope_depth) {
                    /* Shadows outer — propagate state only if same alloc (alias) */
                    if (ps->handles[oi].alloc_id == h->alloc_id) {
                        ps->handles[oi].state = h->state;
                        ps->handles[oi].free_line = h->free_line;
                    }
                    /* Remove inner shadow */
                    ps->handles[hi] = ps->handles[ps->handle_count - 1];
                    ps->handle_count--;
                    break;
                }
            }
        }

        if (labels_heap) free(labels);
        break;
    }

    case NODE_VAR_DECL:
        zc_check_var_init(zc, ps, node);
        if (node->var_decl.init)
            zc_check_expr(zc, ps, node->var_decl.init);
        /* BUG-494: eagerly register move struct var-decl at current scope.
         * Without this, inner K x shadows outer K x but has no handle.
         * find_handle in consume(x) finds the OUTER handle → wrong transfer.
         * Eager registration at var-decl ensures inner scope handle exists
         * BEFORE any use. find_handle (highest depth) then returns inner. */
        {
            Type *vt = checker_get_type(zc->checker, node);
            if (vt && is_move_struct_type(type_unwrap_distinct(vt))) {
                HandleInfo *eh = find_handle_local(ps, node->var_decl.name,
                    (uint32_t)node->var_decl.name_len);
                if (!eh) {
                    eh = add_handle(ps, node->var_decl.name,
                        (uint32_t)node->var_decl.name_len);
                    if (eh) {
                        eh->state = HS_ALIVE;
                        eh->pool_id = -3;
                        eh->alloc_line = node->loc.line;
                        eh->alloc_id = zc->next_alloc_id++;
                    }
                }
            }
        }
        /* Move struct transfer AFTER expr check to avoid false positive:
         * Token b = a; — zc_check_expr sees a as ALIVE, then we transfer.
         * Also handles: Token b = arr[0]; and Token b = s.field; (compound keys)
         * BUG-484: orelse fallback — Token b = opt orelse a; must transfer a. */
        if (node->var_decl.init) {
            Node *move_src = node->var_decl.init;
            Node *move_fallback = NULL; /* orelse fallback — also needs transfer */
            if (move_src->kind == NODE_ORELSE) {
                move_fallback = move_src->orelse.fallback;
                move_src = move_src->orelse.expr;
            }
            /* Check move struct type on the source expression */
            Type *src_type = move_src ? checker_get_type(zc->checker, move_src) : NULL;
            if (!src_type && move_src) {
                /* For NODE_INDEX/NODE_FIELD, resolve via the variable type */
                Node *root = move_src;
                while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                    if (root->kind == NODE_FIELD) root = root->field.object;
                    else root = root->index_expr.object;
                }
                if (root) src_type = checker_get_type(zc->checker, root);
            }
            bool src_is_move = src_type && is_move_struct_type(src_type);
            /* For array element: arr[0] where arr is T[N] and T is move struct */
            if (!src_is_move && move_src && move_src->kind == NODE_INDEX) {
                Type *obj_type = checker_get_type(zc->checker, move_src->index_expr.object);
                if (obj_type) {
                    Type *eff = type_unwrap_distinct(obj_type);
                    if (eff && eff->kind == TYPE_ARRAY && is_move_struct_type(eff->array.inner))
                        src_is_move = true;
                }
            }
            if (src_is_move) {
                const char *src_key;
                int sklen = handle_key_arena(zc, move_src, &src_key);
                if (sklen > 0) {
                    HandleInfo *src = find_handle(ps, src_key, (uint32_t)sklen);
                    if (!src) {
                        src = add_handle(ps, src_key, (uint32_t)sklen);
                        if (src) {
                            src->state = HS_ALIVE;
                            src->pool_id = -3;
                            src->alloc_line = node->loc.line;
                            src->alloc_id = zc->next_alloc_id++;
                        }
                    }
                    if (src && src->state == HS_ALIVE) {
                        src->state = HS_TRANSFERRED;
                        src->transfer_line = node->loc.line;
                        /* Register destination */
                        HandleInfo *dst = find_handle(ps, node->var_decl.name,
                            (uint32_t)node->var_decl.name_len);
                        if (!dst) dst = add_handle(ps, node->var_decl.name,
                            (uint32_t)node->var_decl.name_len);
                        if (dst) {
                            dst->state = HS_ALIVE;
                            dst->pool_id = -3;
                            dst->alloc_line = src->alloc_line;
                            dst->alloc_id = src->alloc_id;
                        }
                    }
                }
            }
            /* BUG-484: orelse fallback — Token b = opt orelse a; transfers a.
             * The fallback is used when opt is null. Must mark as transferred
             * so subsequent use of a is caught as use-after-move. */
            if (move_fallback && !node->var_decl.init->orelse.fallback_is_return &&
                !node->var_decl.init->orelse.fallback_is_break &&
                !node->var_decl.init->orelse.fallback_is_continue) {
                Node *fb = move_fallback;
                /* unwrap block: orelse { expr } — last expr is the value */
                if (fb->kind == NODE_BLOCK && fb->block.stmt_count > 0) {
                    Node *last = fb->block.stmts[fb->block.stmt_count - 1];
                    if (last->kind == NODE_EXPR_STMT) fb = last->expr_stmt.expr;
                }
                Type *fb_type = fb ? checker_get_type(zc->checker, fb) : NULL;
                bool fb_is_move = fb_type && is_move_struct_type(fb_type);
                if (!fb_is_move && fb && fb->kind == NODE_INDEX) {
                    Type *obj_type = checker_get_type(zc->checker, fb->index_expr.object);
                    if (obj_type) {
                        Type *eff = type_unwrap_distinct(obj_type);
                        if (eff && eff->kind == TYPE_ARRAY && is_move_struct_type(eff->array.inner))
                            fb_is_move = true;
                    }
                }
                if (fb_is_move) {
                    const char *fb_key;
                    int fbklen = handle_key_arena(zc, fb, &fb_key);
                    if (fbklen > 0) {
                        HandleInfo *fbh = find_handle(ps, fb_key, (uint32_t)fbklen);
                        if (!fbh) {
                            fbh = add_handle(ps, fb_key, (uint32_t)fbklen);
                            if (fbh) {
                                fbh->state = HS_ALIVE;
                                fbh->pool_id = -3;
                                fbh->alloc_line = node->loc.line;
                                fbh->alloc_id = zc->next_alloc_id++;
                            }
                        }
                        if (fbh && fbh->state == HS_ALIVE) {
                            fbh->state = HS_TRANSFERRED;
                            fbh->transfer_line = node->loc.line;
                        }
                    }
                }
            }
        }
        break;

    case NODE_EXPR_STMT:
        zc_check_expr(zc, ps, node->expr_stmt.expr);
        break;

    case NODE_IF: {
        /* check condition for handle use-after-free */
        if (node->if_stmt.cond)
            zc_check_expr(zc, ps, node->if_stmt.cond);
        /* fork paths at if/else */
        PathState then_state = pathstate_copy(ps);
        /* BUG-335: if-unwrap capture — register captured handle as HS_ALIVE.
         * Pattern: if (pool.alloc()) |h| { ... } */
        if (node->if_stmt.capture_name && node->if_stmt.cond) {
            Node *cond = node->if_stmt.cond;
            if (cond->kind == NODE_CALL && cond->call.callee &&
                cond->call.callee->kind == NODE_FIELD) {
                const char *mname = cond->call.callee->field.field_name;
                uint32_t mlen = (uint32_t)cond->call.callee->field.field_name_len;
                if (mlen == 5 && memcmp(mname, "alloc", 5) == 0) {
                    HandleInfo *h = add_handle(&then_state,
                        node->if_stmt.capture_name,
                        (uint32_t)node->if_stmt.capture_name_len);
                    if (h) {
                        h->state = HS_ALIVE;
                        h->pool_id = -1;
                        h->alloc_line = node->loc.line;
                        h->alloc_id = zc->next_alloc_id++;
                    }
                }
            }
            /* if (mh) |t| or if (arr[0]) |h| — capture is alias of condition */
            const char *cond_key;
            int cklen = handle_key_arena(zc, cond, &cond_key);
            if (cklen > 0) {
                HandleInfo *src = find_handle(ps, cond_key, (uint32_t)cklen);
                if (src) {
                    HandleInfo *cap = add_handle(&then_state,
                        node->if_stmt.capture_name,
                        (uint32_t)node->if_stmt.capture_name_len);
                    if (cap) {
                        cap->state = src->state;
                        cap->pool_id = src->pool_id;
                        cap->alloc_line = src->alloc_line;
                        cap->alloc_id = src->alloc_id; /* same allocation */
                    }
                }
            }
        }
        zc_check_stmt(zc, &then_state, node->if_stmt.then_body);

        /* After then-body: propagate alloc_id coverage from then_state.
         * If any handle with alloc_id X is FREED in then_state, mark
         * the corresponding handle in main ps as escaped — the allocation
         * was freed on the taken path. For if-unwrap on ?Handle, the
         * not-taken path means null (no allocation) → not a leak. */
        if (node->if_stmt.capture_name) {
            for (int ti = 0; ti < then_state.handle_count; ti++) {
                if (then_state.handles[ti].state == HS_FREED &&
                    then_state.handles[ti].alloc_id > 0) {
                    /* find matching alloc_id in main ps */
                    for (int pi = 0; pi < ps->handle_count; pi++) {
                        if (ps->handles[pi].alloc_id == then_state.handles[ti].alloc_id) {
                            ps->handles[pi].escaped = true;
                        }
                    }
                }
            }
        }

        if (node->if_stmt.else_body) {
            PathState else_state = pathstate_copy(ps);
            zc_check_stmt(zc, &else_state, node->if_stmt.else_body);

            /* CFG-aware merge: use terminated flags to determine which paths reach post-if */
            if (then_state.terminated && else_state.terminated) {
                /* Both branches exit — nothing reaches post-if */
                ps->terminated = true;
            } else if (then_state.terminated) {
                /* Only else falls through — copy else state to ps */
                for (int i = 0; i < ps->handle_count; i++) {
                    HandleInfo *eh = find_handle(&else_state, ps->handles[i].name,
                                                 ps->handles[i].name_len);
                    if (eh) ps->handles[i].state = eh->state;
                }
            } else if (else_state.terminated) {
                /* Only then falls through — copy then state to ps */
                for (int i = 0; i < ps->handle_count; i++) {
                    HandleInfo *th = find_handle(&then_state, ps->handles[i].name,
                                                 ps->handles[i].name_len);
                    if (th) ps->handles[i].state = th->state;
                }
            } else {
                /* Both fall through — merge: consumed on BOTH → FREED, on ONE → MAYBE_FREED */
                for (int i = 0; i < ps->handle_count; i++) {
                    HandleInfo *th = find_handle(&then_state, ps->handles[i].name,
                                                 ps->handles[i].name_len);
                    HandleInfo *eh = find_handle(&else_state, ps->handles[i].name,
                                                 ps->handles[i].name_len);
                    if (!th || !eh) continue;
                    bool t_freed = is_handle_consumed(th);
                    bool e_freed = is_handle_consumed(eh);
                    if (t_freed && e_freed) {
                        ps->handles[i].state = HS_FREED;
                        ps->handles[i].free_line = th->free_line;
                    } else if (t_freed || e_freed) {
                        ps->handles[i].state = HS_MAYBE_FREED;
                        ps->handles[i].free_line = t_freed ? th->free_line : eh->free_line;
                    }
                }
            }
            pathstate_free(&else_state);
        } else {
            /* if without else: then-branch frees → MAYBE_FREED
             * (may or may not take the branch).
             * BUT: if the then-branch terminated (return/break/continue/goto),
             * we only reach post-if if the branch was NOT taken — handle is
             * still ALIVE, not MAYBE_FREED. */
            for (int i = 0; i < ps->handle_count; i++) {
                HandleInfo *th = find_handle(&then_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                if (th && is_handle_consumed(th)) {
                    if (then_state.terminated) {
                        /* branch terminates — only non-taken path reaches here */
                        /* don't change state — it stays ALIVE */
                    } else {
                        ps->handles[i].state = HS_MAYBE_FREED;
                        ps->handles[i].free_line = th->free_line;
                    }
                }
            }
        }
        pathstate_free(&then_state);
        break;
    }

    case NODE_FOR:
    case NODE_WHILE:
    case NODE_DO_WHILE: {
        /* check init/cond/step for handle use-after-free */
        if (node->kind == NODE_FOR) {
            if (node->for_stmt.init) zc_check_stmt(zc, ps, node->for_stmt.init);
            if (node->for_stmt.cond) zc_check_expr(zc, ps, node->for_stmt.cond);
            if (node->for_stmt.step) zc_check_expr(zc, ps, node->for_stmt.step);
        } else {
            if (node->while_stmt.cond) zc_check_expr(zc, ps, node->while_stmt.cond);
        }
        Node *body = (node->kind == NODE_FOR) ?
            node->for_stmt.body : node->while_stmt.body;

        PathState pre_loop = pathstate_copy(ps);

        /* Dynamic fixed-point iteration: run loop body until handle states
         * stabilize. States form a finite lattice (5 values per handle,
         * monotone transitions) so convergence is mathematically guaranteed.
         * Ceiling of 32 is crash protection only — real code converges in 1-3. */
        bool first_pass = true;
        for (int iter = 0; iter < 32; iter++) {
            PathState iter_pre = pathstate_copy(ps);
            ps->terminated = false;  /* loop body can break/continue but loop itself continues */
            zc_check_stmt(zc, ps, body);
            ps->terminated = false;  /* loop never terminates enclosing scope */

            if (first_pass) {
                /* After first pass: check for unconditional free/transfer inside loop */
                for (int i = 0; i < ps->handle_count; i++) {
                    HandleInfo *pre = find_handle(&pre_loop, ps->handles[i].name,
                                                  ps->handles[i].name_len);
                    if (pre && pre->state == HS_ALIVE && is_handle_consumed(&ps->handles[i]) &&
                        ps->handles[i].state != HS_MAYBE_FREED) {
                        if (ps->handles[i].state == HS_TRANSFERRED) {
                            zc_error(zc, ps->handles[i].transfer_line,
                                "use after move: '%.*s' moved inside loop — ownership "
                                "already transferred on next iteration",
                                (int)ps->handles[i].name_len, ps->handles[i].name);
                        } else {
                            zc_error(zc, ps->handles[i].free_line,
                                "handle '%.*s' freed inside loop — may cause use-after-free "
                                "on next iteration (allocated at line %d)",
                                (int)ps->handles[i].name_len, ps->handles[i].name,
                                ps->handles[i].alloc_line);
                        }
                    }
                }
                first_pass = false;
            }

            /* Check convergence */
            if (pathstate_equal(&iter_pre, ps)) {
                pathstate_free(&iter_pre);
                break;  /* converged — states stable */
            }
            pathstate_free(&iter_pre);
        }

        pathstate_free(&pre_loop);
        break;
    }

    case NODE_RETURN:
        if (node->ret.expr)
            zc_check_expr(zc, ps, node->ret.expr);
        /* 9c: check if returning a freed/maybe-freed pointer */
        if (node->ret.expr) {
            const char *rkey;
            int rklen = handle_key_arena(zc, node->ret.expr, &rkey);
            if (rklen > 0) {
                HandleInfo *h = find_handle(ps, rkey, (uint32_t)rklen);
                if (h && h->state == HS_FREED) {
                    zc_error(zc, node->loc.line,
                        "returning freed pointer '%.*s' (freed at line %d)",
                        rklen, rkey, h->free_line);
                } else if (h && h->state == HS_MAYBE_FREED) {
                    zc_error(zc, node->loc.line,
                        "returning potentially freed pointer '%.*s' (freed at line %d)",
                        rklen, rkey, h->free_line);
                }
            }
            /* BUG-470: return move struct marks it as transferred */
            if (node->ret.expr->kind == NODE_IDENT) {
                Type *rt = checker_get_type(zc->checker, node->ret.expr);
                if (should_track_move(rt)) {
                    const char *rn = node->ret.expr->ident.name;
                    uint32_t rl = (uint32_t)node->ret.expr->ident.name_len;
                    HandleInfo *mh = zc_ensure_move_registered(zc, ps, rn, rl, node->loc.line);
                    if (mh && mh->state == HS_ALIVE) {
                        mh->state = HS_TRANSFERRED;
                        mh->transfer_line = node->loc.line;
                    }
                }
            }
        }
        ps->terminated = true;
        break;

    case NODE_DEFER:
        break;

    case NODE_CRITICAL:
        /* @critical { body } — check body for handle operations */
        if (node->critical.body)
            zc_check_stmt(zc, ps, node->critical.body);
        break;

    case NODE_ONCE:
        /* @once { body } — check body for handle operations */
        if (node->once.body)
            zc_check_stmt(zc, ps, node->once.body);
        break;

    case NODE_SWITCH: {
        zc_check_expr(zc, ps, node->switch_stmt.expr);
        /* CFG-aware switch merge: only non-terminated arms count for post-switch state.
         * Arms that return/break/goto don't reach post-switch — their freed state
         * shouldn't cause false MAYBE_FREED on the continuation path. */
        bool *freed_all = NULL;   /* freed in ALL non-terminated arms */
        bool *freed_any = NULL;   /* freed in SOME non-terminated arms */
        int *freed_line = NULL;
        bool all_arms_terminated = true;
        int fallthrough_count = 0;
        if (ps->handle_count > 0 && node->switch_stmt.arm_count > 0) {
            freed_all = calloc(ps->handle_count, sizeof(bool));
            freed_any = calloc(ps->handle_count, sizeof(bool));
            freed_line = calloc(ps->handle_count, sizeof(int));
            for (int i = 0; i < ps->handle_count; i++) freed_all[i] = true;
        }
        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            PathState arm_state = pathstate_copy(ps);
            zc_check_stmt(zc, &arm_state, node->switch_stmt.arms[i].body);
            if (arm_state.terminated) {
                /* arm exits — doesn't reach post-switch, don't count for merge */
                /* BUT still check if it consumed handles (for all-arms-freed detection) */
                if (freed_all) {
                    for (int j = 0; j < ps->handle_count; j++) {
                        HandleInfo *ah = find_handle(&arm_state, ps->handles[j].name,
                                                     ps->handles[j].name_len);
                        bool arm_freed = ah && is_handle_consumed(ah);
                        if (arm_freed && !freed_line[j])
                            freed_line[j] = ah->free_line;
                    }
                }
            } else {
                /* arm falls through — its state matters for merge */
                all_arms_terminated = false;
                fallthrough_count++;
                if (freed_all) {
                    for (int j = 0; j < ps->handle_count; j++) {
                        HandleInfo *ah = find_handle(&arm_state, ps->handles[j].name,
                                                     ps->handles[j].name_len);
                        bool arm_freed = ah && is_handle_consumed(ah);
                        if (!arm_freed) {
                            freed_all[j] = false;
                        } else {
                            freed_any[j] = true;
                            if (!freed_line[j]) freed_line[j] = ah->free_line;
                        }
                    }
                }
            }
            pathstate_free(&arm_state);
        }
        if (all_arms_terminated) {
            ps->terminated = true;
        }
        /* merge: among non-terminated arms: ALL freed → FREED, SOME → MAYBE_FREED */
        if (freed_all && fallthrough_count > 0) {
            for (int j = 0; j < ps->handle_count; j++) {
                if (freed_all[j]) {
                    ps->handles[j].state = HS_FREED;
                    ps->handles[j].free_line = freed_line[j];
                } else if (freed_any[j]) {
                    ps->handles[j].state = HS_MAYBE_FREED;
                    ps->handles[j].free_line = freed_line[j];
                }
            }
        }
        if (freed_all) free(freed_all);
        if (freed_any) free(freed_any);
        if (freed_line) free(freed_line);
        break;
    }

    case NODE_SPAWN:
        /* Mark non-shared pointer args as HS_TRANSFERRED */
        for (int si = 0; si < node->spawn_stmt.arg_count; si++) {
            Node *arg = node->spawn_stmt.args[si];
            /* Check if arg is &var or var (pointer ident) */
            Node *root = arg;
            if (root->kind == NODE_UNARY && root->unary.op == TOK_AMP)
                root = root->unary.operand;
            while (root && root->kind == NODE_FIELD) root = root->field.object;
            if (root && root->kind == NODE_IDENT) {
                /* Check if it's a shared struct — don't transfer shared */
                Type *rt = checker_get_type(zc->checker, root);
                if (rt) {
                    Type *reff = type_unwrap_distinct(rt);
                    bool is_shared_type = false;
                    if (reff->kind == TYPE_STRUCT && reff->struct_type.is_shared)
                        is_shared_type = true;
                    if (reff->kind == TYPE_POINTER) {
                        Type *inner = type_unwrap_distinct(reff->pointer.inner);
                        if (inner && inner->kind == TYPE_STRUCT && inner->struct_type.is_shared)
                            is_shared_type = true;
                    }
                    if (!is_shared_type) {
                        const char *akey;
                        int aklen = handle_key_arena(zc, root, &akey);
                        if (aklen > 0) {
                            HandleInfo *h = find_handle(ps, akey, (uint32_t)aklen);
                            if (h && h->state == HS_ALIVE) {
                                h->state = HS_TRANSFERRED;
                                h->transfer_line = node->loc.line;
                            }
                        }
                    }
                }
            }
        }
        /* Scoped spawn: register ThreadHandle as ALIVE (must be joined) */
        if (node->spawn_stmt.handle_name) {
            HandleInfo *th = add_handle(ps,
                node->spawn_stmt.handle_name,
                (uint32_t)node->spawn_stmt.handle_name_len);
            if (th) {
                th->state = HS_ALIVE;
                th->alloc_line = node->loc.line;
                th->alloc_id = zc->next_alloc_id++;
                th->source_color = ZC_COLOR_UNKNOWN;
                th->is_thread_handle = true;
            }
        }
        break;

    /* Exit nodes — mark terminated for CFG-aware merge */
    case NODE_GOTO: case NODE_BREAK: case NODE_CONTINUE:
        ps->terminated = true;
        break;

    /* Nodes not relevant to zercheck handle tracking */
    case NODE_YIELD: case NODE_AWAIT: case NODE_STATIC_ASSERT:
    case NODE_LABEL:
    case NODE_ASM:
    /* Expression nodes — handled by zc_check_expr via NODE_EXPR_STMT */
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_IDENT: case NODE_BINARY: case NODE_UNARY: case NODE_CALL:
    case NODE_FIELD: case NODE_INDEX: case NODE_SLICE: case NODE_ORELSE:
    case NODE_INTRINSIC: case NODE_CAST: case NODE_TYPECAST: case NODE_SIZEOF: case NODE_STRUCT_INIT:
    case NODE_ASSIGN:
    /* Top-level decls — zercheck only runs on function bodies */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL: case NODE_CONTAINER_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR:
        break;
    }
}

/* ---- Cross-function summary helpers ---- */

static FuncSummary *find_summary(ZerCheck *zc, const char *name, uint32_t name_len) {
    for (int i = 0; i < zc->summary_count; i++) {
        if (zc->summaries[i].func_name_len == name_len &&
            memcmp(zc->summaries[i].func_name, name, name_len) == 0)
            return &zc->summaries[i];
    }
    /* no match */
    return NULL;
}

/* Build a summary for one function: what does it do to its Handle params?
 * Uses the existing zc_check_stmt walker with error suppression. */
/* Build / refine a FuncSummary for `func`. Returns true if the summary
 * values changed from the previous call (or if no summary existed yet).
 * The outer driver iterates until `changed == false` — this is how
 * mutual-recursion free-propagation converges (GAP 2 fix, 2026-04-19). */
static bool zc_build_summary(ZerCheck *zc, Node *func) {
    if (!func->func_decl.body) return false;
    if (func->func_decl.param_count == 0) return false;

    /* check if any param is Handle(T) or *T (trackable) */
    bool has_handle_param = false;
    for (int i = 0; i < func->func_decl.param_count; i++) {
        TypeNode *tnode = func->func_decl.params[i].type;
        if (tnode && (tnode->kind == TYNODE_HANDLE || tnode->kind == TYNODE_POINTER)) {
            has_handle_param = true;
            break;
        }
    }
    if (!has_handle_param) return false;

    /* suppress errors during summary phase */
    zc->building_summary = true;

    PathState ps;
    pathstate_init(&ps);

    /* register Handle and *T params as ALIVE */
    for (int i = 0; i < func->func_decl.param_count; i++) {
        TypeNode *tnode = func->func_decl.params[i].type;
        if (tnode && (tnode->kind == TYNODE_HANDLE || tnode->kind == TYNODE_POINTER)) {
            HandleInfo *h = add_handle(&ps, func->func_decl.params[i].name,
                (uint32_t)func->func_decl.params[i].name_len);
            if (h) {
                h->state = HS_ALIVE;
                h->pool_id = -1;
                h->alloc_line = func->loc.line;
            }
        }
    }

    /* walk the body — errors suppressed */
    zc_check_stmt(zc, &ps, func->func_decl.body);

    /* extract summary: check each param's final state */
    int pc = func->func_decl.param_count;
    bool *frees = calloc(pc, sizeof(bool));
    bool *maybe_frees = calloc(pc, sizeof(bool));
    for (int i = 0; i < pc; i++) {
        TypeNode *tnode = func->func_decl.params[i].type;
        /* 9b: track Handle, *T, and *opaque params for cross-function summary */
        if (!tnode || (tnode->kind != TYNODE_HANDLE &&
            tnode->kind != TYNODE_POINTER)) continue;
        HandleInfo *h = find_handle(&ps, func->func_decl.params[i].name,
            (uint32_t)func->func_decl.params[i].name_len);
        if (h) {
            if (h->state == HS_FREED) frees[i] = true;
            else if (h->state == HS_MAYBE_FREED) maybe_frees[i] = true;
        }
    }

    pathstate_free(&ps);
    zc->building_summary = false;

    /* GAP 2 fix: if summary already exists, UPDATE (not append) and
     * report whether values changed. Enables fixed-point iteration for
     * mutual recursion — earlier passes may have computed A's summary
     * without yet knowing B's free behavior; a second pass with B's
     * summary available can then refine A's summary. */
    FuncSummary *existing = find_summary(zc, func->func_decl.name,
        (uint32_t)func->func_decl.name_len);
    if (existing) {
        bool changed = false;
        /* param_count shouldn't drift between rebuilds, but guard anyway */
        if (existing->param_count == pc) {
            for (int i = 0; i < pc; i++) {
                if (existing->frees_param[i] != frees[i]) changed = true;
                if (existing->maybe_frees_param[i] != maybe_frees[i]) changed = true;
            }
        } else {
            changed = true;
        }
        if (!changed) {
            free(frees); free(maybe_frees);
            return false;
        }
        free(existing->frees_param);
        free(existing->maybe_frees_param);
        existing->param_count = pc;
        existing->frees_param = frees;
        existing->maybe_frees_param = maybe_frees;
        return true;
    }

    /* store new summary */
    if (zc->summary_count >= zc->summary_capacity) {
        int new_cap = zc->summary_capacity * 2;
        if (new_cap < 8) new_cap = 8;
        FuncSummary *new_s = realloc(zc->summaries, new_cap * sizeof(FuncSummary));
        if (!new_s) { free(frees); free(maybe_frees); return false; }
        zc->summaries = new_s;
        zc->summary_capacity = new_cap;
    }
    FuncSummary *s = &zc->summaries[zc->summary_count++];
    s->func_name = func->func_decl.name;
    s->func_name_len = (uint32_t)func->func_decl.name_len;
    s->param_count = pc;
    s->frees_param = frees;
    s->maybe_frees_param = maybe_frees;
    return true;
}

/* Apply function summary at a call site: mark handle args as freed/maybe-freed
 * based on what the callee does to its params. */
static void zc_apply_summary(ZerCheck *zc, PathState *ps, Node *call_node) {
    if (!call_node || call_node->kind != NODE_CALL) return;
    Node *callee = call_node->call.callee;
    if (!callee) return;
    /* Extract function name — handle both unqualified and qualified calls */
    const char *func_name = NULL;
    uint32_t func_name_len = 0;
    if (callee->kind == NODE_IDENT) {
        func_name = callee->ident.name;
        func_name_len = (uint32_t)callee->ident.name_len;
    } else if (callee->kind == NODE_FIELD) {
        /* module.func() — use field name (raw function name) */
        func_name = callee->field.field_name;
        func_name_len = (uint32_t)callee->field.field_name_len;
    } else {
        return;
    }

    FuncSummary *s = find_summary(zc, func_name, func_name_len);
    /* Signature heuristic fallback: if no summary exists but the function
     * takes *opaque/*T as param and returns void, treat as @frees(0).
     * Covers wrapper chains: app_stop → service_shutdown → resource_destroy.
     * Works for both bodyless (cinclude) and ZER functions in other modules. */
    bool heuristic_free = false;
    if (!s && call_node->call.arg_count >= 1) {
        Symbol *sym = scope_lookup(zc->checker->global_scope,
            func_name, func_name_len);
        /* Also try mangled name for imported module functions */
        if (!sym && callee->kind == NODE_FIELD && callee->field.object &&
            callee->field.object->kind == NODE_IDENT) {
            char mkey[256];
            uint32_t mlen = (uint32_t)callee->field.object->ident.name_len;
            if (mlen + 2 + func_name_len < sizeof(mkey)) {
                memcpy(mkey, callee->field.object->ident.name, mlen);
                mkey[mlen] = '_'; mkey[mlen+1] = '_';
                memcpy(mkey + mlen + 2, func_name, func_name_len);
                sym = scope_lookup(zc->checker->global_scope, mkey, mlen + 2 + func_name_len);
            }
        }
        if (sym && sym->is_function && sym->type &&
            sym->func_node && !sym->func_node->func_decl.body &&
            !(func_name_len == 4 && memcmp(func_name, "free", 4) == 0)) {
            /* Only for bodyless functions (extern/cinclude). Functions WITH
             * bodies get proper summaries via zc_build_summary. */
            Type *ret = sym->type;
            if (ret->kind == TYPE_FUNC_PTR) ret = ret->func_ptr.ret;
            if (ret && type_unwrap_distinct(ret)->kind == TYPE_VOID &&
                sym->type->kind == TYPE_FUNC_PTR &&
                sym->type->func_ptr.param_count >= 1) {
                Type *p0 = type_unwrap_distinct(sym->type->func_ptr.params[0]);
                if (p0 && (p0->kind == TYPE_POINTER || p0->kind == TYPE_OPAQUE)) {
                    heuristic_free = true;
                }
            }
        }
    }
    if (!s && !heuristic_free) return;
    /* If heuristic free, treat param 0 as freed */
    if (heuristic_free && !s) {
        const char *hkey;
        int hklen = handle_key_arena(zc, call_node->call.args[0], &hkey);
        if (hklen > 0) {
            HandleInfo *h = find_handle(ps, hkey, (uint32_t)hklen);
            if (h) {
                if (h->state == HS_FREED) {
                    zc_error(zc, call_node->loc.line,
                        "double free: '%.*s' already freed at line %d",
                        hklen, hkey, h->free_line);
                }
                h->state = HS_FREED;
                h->free_line = call_node->loc.line;
            }
        }
        return;
    }

    int arg_count = call_node->call.arg_count;
    if (arg_count > s->param_count) arg_count = s->param_count;

    for (int i = 0; i < arg_count; i++) {
        if (!s->frees_param[i] && !s->maybe_frees_param[i]) continue;

        /* get the handle key for this argument */
        const char *hkey;
        int hklen = handle_key_arena(zc, call_node->call.args[i], &hkey);
        if (hklen <= 0) continue;

        HandleInfo *h = find_handle(ps, hkey, (uint32_t)hklen);
        if (!h) continue;

        if (s->frees_param[i]) {
            if (h->state == HS_FREED) {
                zc_error(zc, call_node->loc.line,
                    "double free: '%.*s' freed by call to '%.*s' (already freed at line %d)",
                    hklen, hkey, (int)s->func_name_len, s->func_name, h->free_line);
            } else if (h->state == HS_MAYBE_FREED) {
                zc_error(zc, call_node->loc.line,
                    "double free: '%.*s' freed by call to '%.*s' (may have been freed at line %d)",
                    hklen, hkey, (int)s->func_name_len, s->func_name, h->free_line);
            }
            h->state = HS_FREED;
            h->free_line = call_node->loc.line;
            /* propagate to aliases */
            for (int j = 0; j < ps->handle_count; j++) {
                if (&ps->handles[j] != h &&
                    ps->handles[j].pool_id == h->pool_id &&
                    ps->handles[j].alloc_line == h->alloc_line &&
                    ps->handles[j].state == HS_ALIVE) {
                    ps->handles[j].state = HS_FREED;
                    ps->handles[j].free_line = call_node->loc.line;
                }
            }
        } else if (s->maybe_frees_param[i]) {
            if (h->state == HS_ALIVE) {
                h->state = HS_MAYBE_FREED;
                h->free_line = call_node->loc.line;
            }
        }
    }
}

/* ---- Check a single function ---- */

static void zc_check_function(ZerCheck *zc, Node *func) {
    if (!func->func_decl.body) return;

    PathState ps;
    pathstate_init(&ps);

    /* register Handle(T) parameters as alive handles so use-after-free
     * on parameters is caught within the function body (BUG-117 fix).
     * BUG-385: also scan struct/union parameters for Handle fields. */
    for (int i = 0; i < func->func_decl.param_count; i++) {
        TypeNode *tnode = func->func_decl.params[i].type;
        const char *pname = func->func_decl.params[i].name;
        uint32_t plen = (uint32_t)func->func_decl.params[i].name_len;
        if (tnode && tnode->kind == TYNODE_HANDLE) {
            HandleInfo *h = add_handle(&ps, pname, plen);
            if (h) {
                h->state = HS_ALIVE;
                h->pool_id = -1;
                h->alloc_line = func->loc.line;
            }
        }
        /* BUG-385: if param is a struct, scan for Handle fields.
         * Build compound keys like "s.h" for each Handle field. */
        if (tnode && tnode->kind == TYNODE_NAMED) {
            Symbol *type_sym = scope_lookup(zc->checker->global_scope,
                tnode->named.name, (uint32_t)tnode->named.name_len);
            if (type_sym && type_sym->type) {
                Type *st = type_unwrap_distinct(type_sym->type);
                if (st && st->kind == TYPE_STRUCT) {
                    for (uint32_t fi = 0; fi < st->struct_type.field_count; fi++) {
                        Type *ft = type_unwrap_distinct(st->struct_type.fields[fi].type);
                        if (ft && ft->kind == TYPE_HANDLE) {
                            /* build "param.field" key */
                            uint32_t flen = st->struct_type.fields[fi].name_len;
                            int klen = (int)(plen + 1 + flen);
                            char *key = (char *)arena_alloc(zc->arena, klen + 1);
                            if (key) {
                                memcpy(key, pname, plen);
                                key[plen] = '.';
                                memcpy(key + plen + 1,
                                       st->struct_type.fields[fi].name, flen);
                                key[klen] = '\0';
                                HandleInfo *h = add_handle(&ps, key, (uint32_t)klen);
                                if (h) {
                                    h->state = HS_ALIVE;
                                    h->pool_id = -1;
                                    h->alloc_line = func->loc.line;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    zc_check_stmt(zc, &ps, func->func_decl.body);

    /* Defer free scanning: scan ENTIRE function body (recursively) for
     * defer { free(h) } patterns. Handles freed in defer are not leaked.
     * Must scan inside loops, if-bodies, blocks — defers can appear anywhere. */
    {
        /* Recursive defer scanner — walks all statement types */
        struct { Node **stmts; int count; } scan_stack[32];
        int scan_depth = 0;
        if (func->func_decl.body && func->func_decl.body->kind == NODE_BLOCK) {
            scan_stack[0].stmts = func->func_decl.body->block.stmts;
            scan_stack[0].count = func->func_decl.body->block.stmt_count;
            scan_depth = 1;
        }
        while (scan_depth > 0) {
            scan_depth--;
            Node **stmts = scan_stack[scan_depth].stmts;
            int count = scan_stack[scan_depth].count;
            for (int di = 0; di < count; di++) {
                Node *stmt = stmts[di];
                if (!stmt) continue;
                if (stmt->kind == NODE_DEFER) {
                    defer_scan_all_frees(stmt->defer.body, &ps, stmt->loc.line);
                }
                /* Recurse into nested blocks */
                if (stmt->kind == NODE_BLOCK && scan_depth < 31) {
                    scan_stack[scan_depth].stmts = stmt->block.stmts;
                    scan_stack[scan_depth].count = stmt->block.stmt_count;
                    scan_depth++;
                }
                if (stmt->kind == NODE_IF) {
                    if (stmt->if_stmt.then_body && stmt->if_stmt.then_body->kind == NODE_BLOCK && scan_depth < 31) {
                        scan_stack[scan_depth].stmts = stmt->if_stmt.then_body->block.stmts;
                        scan_stack[scan_depth].count = stmt->if_stmt.then_body->block.stmt_count;
                        scan_depth++;
                    }
                    if (stmt->if_stmt.else_body && stmt->if_stmt.else_body->kind == NODE_BLOCK && scan_depth < 31) {
                        scan_stack[scan_depth].stmts = stmt->if_stmt.else_body->block.stmts;
                        scan_stack[scan_depth].count = stmt->if_stmt.else_body->block.stmt_count;
                        scan_depth++;
                    }
                }
                if ((stmt->kind == NODE_FOR || stmt->kind == NODE_WHILE) && scan_depth < 31) {
                    Node *body = (stmt->kind == NODE_FOR) ? stmt->for_stmt.body : stmt->while_stmt.body;
                    if (body && body->kind == NODE_BLOCK) {
                        scan_stack[scan_depth].stmts = body->block.stmts;
                        scan_stack[scan_depth].count = body->block.stmt_count;
                        scan_depth++;
                    }
                }
                if (stmt->kind == NODE_CRITICAL && stmt->critical.body &&
                    stmt->critical.body->kind == NODE_BLOCK && scan_depth < 31) {
                    scan_stack[scan_depth].stmts = stmt->critical.body->block.stmts;
                    scan_stack[scan_depth].count = stmt->critical.body->block.stmt_count;
                    scan_depth++;
                }
                if (stmt->kind == NODE_ONCE && stmt->once.body &&
                    stmt->once.body->kind == NODE_BLOCK && scan_depth < 31) {
                    scan_stack[scan_depth].stmts = stmt->once.body->block.stmts;
                    scan_stack[scan_depth].count = stmt->once.body->block.stmt_count;
                    scan_depth++;
                }
            }
        }
    }

    /* Mark handles that escape via return as escaped.
     * Scan function body for return statements containing handle idents. */
    if (func->func_decl.body && func->func_decl.body->kind == NODE_BLOCK) {
        for (int si = 0; si < func->func_decl.body->block.stmt_count; si++) {
            Node *stmt = func->func_decl.body->block.stmts[si];
            if (stmt->kind == NODE_RETURN && stmt->ret.expr) {
                const char *rkey;
                int rklen = handle_key_arena(zc, stmt->ret.expr, &rkey);
                if (rklen > 0) {
                    HandleInfo *rh = find_handle(&ps, rkey, (uint32_t)rklen);
                    if (rh) rh->escaped = true;
                }
            }
        }
    }

    /* Leak detection — alloc_id grouping.
     * For each allocation (unique alloc_id), check ALL handles that share it.
     * If ANY handle in the group is FREED or escaped → allocation is covered.
     * If ALL handles in the group are ALIVE and not escaped → LEAK ERROR.
     *
     * This naturally handles:
     * - mh/h pairs: same alloc_id, h is FREED → mh skipped
     * - return mh: mh.escaped = true → skipped
     * - handles[i] = mh: mh.escaped = true (untrackable target) → skipped
     * - defer free(h): h is FREED → skipped
     * - Parameters: pool_id == -1, excluded */
    /* BUG-492: collect unique alloc_ids that are covered (any member freed/escaped).
     * Dynamic array — no fixed limit. Stack-first [64] with malloc overflow. */
    int covered_stack[64];
    int *covered_ids = covered_stack;
    int covered_count = 0;
    int covered_capacity = 64;
    for (int i = 0; i < ps.handle_count; i++) {
        if (ps.handles[i].state == HS_FREED || ps.handles[i].escaped ||
            ps.handles[i].state == HS_TRANSFERRED) {
            bool already = false;
            for (int c = 0; c < covered_count; c++) {
                if (covered_ids[c] == ps.handles[i].alloc_id) { already = true; break; }
            }
            if (!already) {
                if (covered_count >= covered_capacity) {
                    int nc = covered_capacity * 2;
                    int *new_ids = (int *)malloc(nc * sizeof(int));
                    if (new_ids) {
                        memcpy(new_ids, covered_ids, covered_count * sizeof(int));
                        if (covered_ids != covered_stack) free(covered_ids);
                        covered_ids = new_ids;
                        covered_capacity = nc;
                    }
                }
                if (covered_count < covered_capacity)
                    covered_ids[covered_count++] = ps.handles[i].alloc_id;
            }
        }
    }
    for (int i = 0; i < ps.handle_count; i++) {
        bool is_param = (ps.handles[i].pool_id == -1 &&
                         ps.handles[i].alloc_line == (int)func->loc.line);
        if (is_param) continue;
        /* Arena-colored allocations don't need individual free */
        if (ps.handles[i].source_color == ZC_COLOR_ARENA) continue;
        /* Move struct vars don't need free — they're stack values */
        if (ps.handles[i].pool_id == -3) continue;
        if (ps.handles[i].state != HS_ALIVE && ps.handles[i].state != HS_MAYBE_FREED) continue;
        /* check if this allocation is covered */
        bool covered = false;
        for (int c = 0; c < covered_count; c++) {
            if (covered_ids[c] == ps.handles[i].alloc_id) { covered = true; break; }
        }
        if (covered) continue;
        if (ps.handles[i].state == HS_ALIVE) {
            if (ps.handles[i].is_thread_handle) {
                zc_error(zc, ps.handles[i].alloc_line,
                    "thread not joined: '%.*s' spawned but never joined — "
                    "add '%.*s.join()' before function returns",
                    (int)ps.handles[i].name_len, ps.handles[i].name,
                    (int)ps.handles[i].name_len, ps.handles[i].name);
            } else {
                zc_error(zc, ps.handles[i].alloc_line,
                    "handle '%.*s' allocated but never freed — add 'defer pool.free(%.*s)' "
                    "after allocation, or return the handle to the caller",
                    (int)ps.handles[i].name_len, ps.handles[i].name,
                    (int)ps.handles[i].name_len, ps.handles[i].name);
            }
        } else if (ps.handles[i].state == HS_MAYBE_FREED) {
            zc_error(zc, ps.handles[i].alloc_line,
                "handle '%.*s' may not be freed on all paths — ensure all branches "
                "free the handle or add 'defer' for automatic cleanup",
                (int)ps.handles[i].name_len, ps.handles[i].name);
        }
        /* Deduplicate: don't report same allocation twice */
        if (covered_count >= covered_capacity) {
            int nc = covered_capacity * 2;
            int *new_ids = (int *)malloc(nc * sizeof(int));
            if (new_ids) {
                memcpy(new_ids, covered_ids, covered_count * sizeof(int));
                if (covered_ids != covered_stack) free(covered_ids);
                covered_ids = new_ids;
                covered_capacity = nc;
            }
        }
        if (covered_count < covered_capacity)
            covered_ids[covered_count++] = ps.handles[i].alloc_id;
    }

    if (covered_ids != covered_stack) free(covered_ids);
    pathstate_free(&ps);
}

/* ================================================================
 * Entry point
 * ================================================================ */

void zercheck_init(ZerCheck *zc, Checker *checker, Arena *arena, const char *file) {
    memset(zc, 0, sizeof(ZerCheck));
    zc->checker = checker;
    zc->arena = arena;
    zc->file_name = file;
    zc->next_alloc_id = 1; /* 0 = no allocation (params, uninitialized) */
}

bool zercheck_run(ZerCheck *zc, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return true;

    /* register all pool variables */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_GLOBAL_VAR) {
            Symbol *sym = scope_lookup(zc->checker->global_scope,
                decl->var_decl.name, (uint32_t)decl->var_decl.name_len);
            if (sym && sym->type && (sym->type->kind == TYPE_POOL || sym->type->kind == TYPE_SLAB)) {
                register_pool(zc, decl->var_decl.name,
                    (uint32_t)decl->var_decl.name_len);
            }
        }
    }

    /* Pre-scan: build cross-function summaries for all functions.
     * Scan imported modules first (dependencies before dependents),
     * then main module.
     *
     * GAP 2 fix (2026-04-19): iterate rebuilding ALL summaries until
     * values stabilize. Previously the loop only ADDED missing summaries;
     * existing summaries were never refined. Mutual recursion (A calls B
     * calls A) produced A's summary on pass 1 without knowing B's free
     * behavior — and it stayed wrong. Now zc_build_summary returns true
     * when it refines an existing summary, and we loop while any refined. */
    for (int pass = 0; pass < 16; pass++) {
        bool changed = false;
        /* imported modules first */
        for (int mi = 0; mi < zc->import_ast_count; mi++) {
            Node *mod = zc->import_asts[mi];
            if (!mod || mod->kind != NODE_FILE) continue;
            for (int i = 0; i < mod->file.decl_count; i++) {
                Node *decl = mod->file.decls[i];
                if (decl->kind == NODE_FUNC_DECL &&
                    zc_build_summary(zc, decl))
                    changed = true;
            }
        }
        /* main module */
        for (int i = 0; i < file_node->file.decl_count; i++) {
            Node *decl = file_node->file.decls[i];
            if (decl->kind == NODE_FUNC_DECL &&
                zc_build_summary(zc, decl))
                changed = true;
        }
        if (!changed) break;
    }

    /* main analysis: check each function and interrupt body */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_FUNC_DECL) {
            zc_check_function(zc, decl);
        } else if (decl->kind == NODE_INTERRUPT && decl->interrupt.body) {
            PathState ps;
            pathstate_init(&ps);
            zc_check_stmt(zc, &ps, decl->interrupt.body);
            pathstate_free(&ps);
        }
    }

    /* cleanup */
    for (int i = 0; i < zc->summary_count; i++) {
        free(zc->summaries[i].frees_param);
        free(zc->summaries[i].maybe_frees_param);
    }
    free(zc->summaries);
    zc->summaries = NULL;
    free(zc->pools);
    zc->pools = NULL;

    return zc->error_count == 0;
}
