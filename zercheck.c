#include "zercheck.h"
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
    zc->error_count++;
    fprintf(stderr, "%s:%d: zercheck: ", zc->file_name, line);
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
}

/* deep copy a PathState (allocates new handle array) */
static PathState pathstate_copy(PathState *src) {
    PathState dst;
    dst.handle_count = src->handle_count;
    dst.handle_capacity = src->handle_count > 0 ? src->handle_count : 4;
    dst.handles = (HandleInfo *)malloc(dst.handle_capacity * sizeof(HandleInfo));
    if (src->handle_count > 0) {
        memcpy(dst.handles, src->handles, src->handle_count * sizeof(HandleInfo));
    }
    return dst;
}

static void pathstate_free(PathState *ps) {
    free(ps->handles);
    ps->handles = NULL;
    ps->handle_count = 0;
    ps->handle_capacity = 0;
}

static HandleInfo *find_handle(PathState *ps, const char *name, uint32_t name_len) {
    for (int i = 0; i < ps->handle_count; i++) {
        if (ps->handles[i].name_len == name_len &&
            memcmp(ps->handles[i].name, name, name_len) == 0) {
            return &ps->handles[i];
        }
    }
    return NULL;
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

/* ---- AST walking ---- */

static void zc_check_expr(ZerCheck *zc, PathState *ps, Node *node);
static void zc_check_stmt(ZerCheck *zc, PathState *ps, Node *node);

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
    Type *pool_type = checker_get_type(obj);
    if (!pool_type) {
        Symbol *sym = scope_lookup(zc->checker->global_scope, pool_name, plen);
        if (sym) pool_type = sym->type;
    }
    if (!pool_type || pool_type->kind != TYPE_POOL) return;

    int pool_id = register_pool(zc, pool_name, plen);

    /* pool.free(h) — mark handle as freed */
    if (mlen == 4 && memcmp(method, "free", 4) == 0) {
        if (node->call.arg_count > 0 && node->call.args[0]->kind == NODE_IDENT) {
            const char *hname = node->call.args[0]->ident.name;
            uint32_t hlen = (uint32_t)node->call.args[0]->ident.name_len;
            HandleInfo *h = find_handle(ps, hname, hlen);
            if (h) {
                if (h->state == HS_FREED) {
                    zc_error(zc, node->loc.line,
                        "double free: '%.*s' already freed at line %d",
                        (int)hlen, hname, h->free_line);
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

    /* pool.get(h) — check handle is alive and correct pool */
    if (mlen == 3 && memcmp(method, "get", 3) == 0) {
        if (node->call.arg_count > 0 && node->call.args[0]->kind == NODE_IDENT) {
            const char *hname = node->call.args[0]->ident.name;
            uint32_t hlen = (uint32_t)node->call.args[0]->ident.name_len;
            HandleInfo *h = find_handle(ps, hname, hlen);
            if (h) {
                if (h->state == HS_FREED) {
                    zc_error(zc, node->loc.line,
                        "use-after-free: '%.*s' freed at line %d",
                        (int)hlen, hname, h->free_line);
                }
                if (h->pool_id >= 0 && h->pool_id != pool_id) {
                    zc_error(zc, node->loc.line,
                        "wrong pool: '%.*s' allocated from pool %d, used on pool %d",
                        (int)hlen, hname, h->pool_id, pool_id);
                }
            }
        }
    }
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

        if (mlen == 5 && memcmp(method, "alloc", 5) == 0 &&
            obj->kind == NODE_IDENT) {
            /* this is h = pool.alloc() */
            int pool_id = register_pool(zc, obj->ident.name,
                (uint32_t)obj->ident.name_len);
            HandleInfo *h = find_handle(ps, vname, vlen);
            if (!h) h = add_handle(ps, vname, vlen);
            if (h) {
                h->state = HS_ALIVE;
                h->pool_id = pool_id;
                h->alloc_line = var_node->loc.line;
            }
        }
    }

    /* Handle aliasing: Handle(T) alias = existing_handle;
     * If init is a simple identifier that matches a tracked handle,
     * register the new variable with the same state and pool. */
    if (init->kind == NODE_IDENT) {
        HandleInfo *src = find_handle(ps, init->ident.name,
            (uint32_t)init->ident.name_len);
        if (src) {
            HandleInfo *dst = find_handle(ps, vname, vlen);
            if (!dst) dst = add_handle(ps, vname, vlen);
            if (dst) {
                dst->state = src->state;
                dst->pool_id = src->pool_id;
                dst->alloc_line = src->alloc_line;
                dst->free_line = src->free_line;
            }
        }
    }
}

static void zc_check_expr(ZerCheck *zc, PathState *ps, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_CALL:
        zc_check_call(zc, ps, node);
        for (int i = 0; i < node->call.arg_count; i++)
            zc_check_expr(zc, ps, node->call.args[i]);
        break;
    case NODE_ASSIGN:
        zc_check_expr(zc, ps, node->assign.target);
        zc_check_expr(zc, ps, node->assign.value);
        /* Handle aliasing via assignment: alias = tracked_handle */
        if (node->assign.target->kind == NODE_IDENT &&
            node->assign.value->kind == NODE_IDENT) {
            HandleInfo *src = find_handle(ps, node->assign.value->ident.name,
                (uint32_t)node->assign.value->ident.name_len);
            if (src) {
                const char *tname = node->assign.target->ident.name;
                uint32_t tlen = (uint32_t)node->assign.target->ident.name_len;
                HandleInfo *dst = find_handle(ps, tname, tlen);
                if (!dst) dst = add_handle(ps, tname, tlen);
                if (dst) {
                    dst->state = src->state;
                    dst->pool_id = src->pool_id;
                    dst->alloc_line = src->alloc_line;
                    dst->free_line = src->free_line;
                }
            }
        }
        break;
    case NODE_BINARY:
        zc_check_expr(zc, ps, node->binary.left);
        zc_check_expr(zc, ps, node->binary.right);
        break;
    case NODE_FIELD:
        zc_check_expr(zc, ps, node->field.object);
        break;
    case NODE_ORELSE:
        zc_check_expr(zc, ps, node->orelse.expr);
        if (node->orelse.fallback)
            zc_check_expr(zc, ps, node->orelse.fallback);
        break;
    default:
        break;
    }
}

static void zc_check_stmt(ZerCheck *zc, PathState *ps, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < node->block.stmt_count; i++)
            zc_check_stmt(zc, ps, node->block.stmts[i]);
        break;

    case NODE_VAR_DECL:
        zc_check_var_init(zc, ps, node);
        if (node->var_decl.init)
            zc_check_expr(zc, ps, node->var_decl.init);
        break;

    case NODE_EXPR_STMT:
        zc_check_expr(zc, ps, node->expr_stmt.expr);
        break;

    case NODE_IF: {
        /* fork paths at if/else */
        PathState then_state = pathstate_copy(ps);
        zc_check_stmt(zc, &then_state, node->if_stmt.then_body);

        if (node->if_stmt.else_body) {
            PathState else_state = pathstate_copy(ps);
            zc_check_stmt(zc, &else_state, node->if_stmt.else_body);

            /* merge: only mark freed if freed on BOTH paths (under-approximation) */
            for (int i = 0; i < ps->handle_count; i++) {
                HandleInfo *th = find_handle(&then_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                HandleInfo *eh = find_handle(&else_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                if (th && eh && th->state == HS_FREED && eh->state == HS_FREED) {
                    ps->handles[i].state = HS_FREED;
                    ps->handles[i].free_line = th->free_line;
                }
            }
            pathstate_free(&else_state);
        } else {
            /* if without else: merge then-state back (under-approximation:
             * only mark freed if then-path frees, since we might not take it) */
            for (int i = 0; i < ps->handle_count; i++) {
                HandleInfo *th = find_handle(&then_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                if (th && th->state == HS_FREED) {
                    /* conservatively keep as ALIVE — under-approx avoids false positives.
                     * but check: if handle is used after this if, and then-path freed it,
                     * we can't catch it without full path analysis. Accept this false negative. */
                }
            }
        }
        pathstate_free(&then_state);
        break;
    }

    case NODE_FOR:
    case NODE_WHILE: {
        Node *body = (node->kind == NODE_FOR) ?
            node->for_stmt.body : node->while_stmt.body;

        PathState pre_loop = pathstate_copy(ps);
        zc_check_stmt(zc, ps, body);

        for (int i = 0; i < ps->handle_count; i++) {
            HandleInfo *pre = find_handle(&pre_loop, ps->handles[i].name,
                                          ps->handles[i].name_len);
            if (pre && pre->state == HS_ALIVE && ps->handles[i].state == HS_FREED) {
                zc_error(zc, ps->handles[i].free_line,
                    "handle '%.*s' freed inside loop — may cause use-after-free "
                    "on next iteration (allocated at line %d)",
                    (int)ps->handles[i].name_len, ps->handles[i].name,
                    ps->handles[i].alloc_line);
            }
        }
        pathstate_free(&pre_loop);
        break;
    }

    case NODE_RETURN:
        if (node->ret.expr)
            zc_check_expr(zc, ps, node->ret.expr);
        break;

    case NODE_DEFER:
        break;

    case NODE_SWITCH: {
        zc_check_expr(zc, ps, node->switch_stmt.expr);
        /* track which handles are freed in ALL arms (under-approximation) */
        bool *freed_all = NULL;
        int *freed_line = NULL;
        if (ps->handle_count > 0 && node->switch_stmt.arm_count > 0) {
            freed_all = calloc(ps->handle_count, sizeof(bool));
            freed_line = calloc(ps->handle_count, sizeof(int));
            for (int i = 0; i < ps->handle_count; i++) freed_all[i] = true;
        }
        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            PathState arm_state = pathstate_copy(ps);
            zc_check_stmt(zc, &arm_state, node->switch_stmt.arms[i].body);
            if (freed_all) {
                for (int j = 0; j < ps->handle_count; j++) {
                    HandleInfo *ah = find_handle(&arm_state, ps->handles[j].name,
                                                 ps->handles[j].name_len);
                    if (!ah || ah->state != HS_FREED) {
                        freed_all[j] = false;
                    } else if (i == 0) {
                        freed_line[j] = ah->free_line;
                    }
                }
            }
            pathstate_free(&arm_state);
        }
        /* merge: only mark freed if freed in ALL arms */
        if (freed_all) {
            for (int j = 0; j < ps->handle_count; j++) {
                if (freed_all[j]) {
                    ps->handles[j].state = HS_FREED;
                    ps->handles[j].free_line = freed_line[j];
                }
            }
            free(freed_all);
            free(freed_line);
        }
        break;
    }

    default:
        break;
    }
}

/* ---- Check a single function ---- */

static void zc_check_function(ZerCheck *zc, Node *func) {
    if (!func->func_decl.body) return;

    PathState ps;
    pathstate_init(&ps);
    zc_check_stmt(zc, &ps, func->func_decl.body);
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
}

bool zercheck_run(ZerCheck *zc, Node *file_node) {
    if (!file_node || file_node->kind != NODE_FILE) return true;

    /* register all pool variables */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_GLOBAL_VAR) {
            Symbol *sym = scope_lookup(zc->checker->global_scope,
                decl->var_decl.name, (uint32_t)decl->var_decl.name_len);
            if (sym && sym->type && sym->type->kind == TYPE_POOL) {
                register_pool(zc, decl->var_decl.name,
                    (uint32_t)decl->var_decl.name_len);
            }
        }
    }

    /* check each function and interrupt body */
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
    free(zc->pools);
    zc->pools = NULL;

    return zc->error_count == 0;
}
