#include "zercheck.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ================================================================
 * ZER-CHECK Implementation
 *
 * Algorithm:
 *   1. Scan function for Handle variables and Pool declarations
 *   2. Walk statements, track typestate per handle per path
 *   3. At branches (if/else), fork paths
 *   4. Report errors with concrete path info
 *   5. Under-approximate: if unsure, stay silent
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

static PathState *current_path(ZerCheck *zc) {
    if (zc->path_count == 0) {
        zc->path_count = 1;
        memset(&zc->paths[0], 0, sizeof(PathState));
    }
    return &zc->paths[0]; /* primary path */
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
    if (ps->handle_count >= ZC_MAX_HANDLES) return NULL;
    HandleInfo *h = &ps->handles[ps->handle_count++];
    memset(h, 0, sizeof(HandleInfo));
    h->name = name;
    h->name_len = name_len;
    h->state = HS_UNKNOWN;
    h->pool_id = -1;
    return h;
}

/* find pool id by variable name */
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
    if (zc->pool_count >= 64) return -1;
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

    /* check if this is a pool variable */
    Symbol *sym = scope_lookup(zc->checker->global_scope, pool_name, plen);
    if (!sym || !sym->type || sym->type->kind != TYPE_POOL) return;

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

    /* check if this is a Handle type */
    Type *vtype = NULL;
    if (var_node->var_decl.type) {
        /* look up in checker scope */
        Symbol *sym = scope_lookup(zc->checker->global_scope, vname, vlen);
        if (sym) vtype = sym->type;
    }

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
}

static void zc_check_expr(ZerCheck *zc, PathState *ps, Node *node) {
    if (!node) return;

    switch (node->kind) {
    case NODE_CALL:
        zc_check_call(zc, ps, node);
        /* recurse into args */
        for (int i = 0; i < node->call.arg_count; i++)
            zc_check_expr(zc, ps, node->call.args[i]);
        break;
    case NODE_ASSIGN:
        zc_check_expr(zc, ps, node->assign.target);
        zc_check_expr(zc, ps, node->assign.value);
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
        PathState then_state = *ps;
        zc_check_stmt(zc, &then_state, node->if_stmt.then_body);

        if (node->if_stmt.else_body) {
            PathState else_state = *ps;
            zc_check_stmt(zc, &else_state, node->if_stmt.else_body);

            /* merge: if freed on BOTH paths → definitely freed
             * if freed on ONE path → maybe freed (flag it) */
            for (int i = 0; i < ps->handle_count; i++) {
                HandleInfo *th = find_handle(&then_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                HandleInfo *eh = find_handle(&else_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                if (th && eh) {
                    if (th->state == HS_FREED && eh->state == HS_FREED) {
                        ps->handles[i].state = HS_FREED;
                        ps->handles[i].free_line = th->free_line;
                    } else if (th->state == HS_FREED || eh->state == HS_FREED) {
                        /* freed on one path — conservatively mark as freed
                         * to catch use-after-free on that path */
                        ps->handles[i].state = HS_FREED;
                        ps->handles[i].free_line = th->state == HS_FREED ?
                            th->free_line : eh->free_line;
                    }
                }
            }
        } else {
            /* no else — then_state might have freed handles */
            for (int i = 0; i < then_state.handle_count; i++) {
                HandleInfo *orig = find_handle(ps, then_state.handles[i].name,
                                               then_state.handles[i].name_len);
                if (orig && then_state.handles[i].state == HS_FREED) {
                    /* freed in then-branch but might not execute — keep alive
                     * This is under-approximation: don't report unless certain */
                }
            }
        }
        break;
    }

    case NODE_FOR:
    case NODE_WHILE: {
        /* analyze loop body once — check for cross-iteration bugs */
        Node *body = (node->kind == NODE_FOR) ?
            node->for_stmt.body : node->while_stmt.body;

        /* save state before loop */
        PathState pre_loop = *ps;

        /* first iteration */
        zc_check_stmt(zc, ps, body);

        /* check: anything freed in body that was alive at loop entry?
         * If so, second iteration would use-after-free */
        for (int i = 0; i < ps->handle_count; i++) {
            HandleInfo *pre = find_handle(&pre_loop, ps->handles[i].name,
                                          ps->handles[i].name_len);
            if (pre && pre->state == HS_ALIVE && ps->handles[i].state == HS_FREED) {
                /* handle was alive before loop, freed inside.
                 * If loop iterates again, it's use-after-free.
                 * Only report if the handle is USED at the top of the loop body. */
                /* For now, report a warning */
                zc_error(zc, ps->handles[i].free_line,
                    "handle '%.*s' freed inside loop — may cause use-after-free "
                    "on next iteration (allocated at line %d)",
                    (int)ps->handles[i].name_len, ps->handles[i].name,
                    ps->handles[i].alloc_line);
            }
        }
        break;
    }

    case NODE_RETURN:
        if (node->ret.expr)
            zc_check_expr(zc, ps, node->ret.expr);
        break;

    case NODE_DEFER:
        /* defer bodies run later — don't analyze now */
        break;

    case NODE_SWITCH:
        /* check switch expression */
        zc_check_expr(zc, ps, node->switch_stmt.expr);
        /* check each arm */
        for (int i = 0; i < node->switch_stmt.arm_count; i++) {
            PathState arm_state = *ps;
            zc_check_stmt(zc, &arm_state, node->switch_stmt.arms[i].body);
        }
        break;

    default:
        break;
    }
}

/* ---- Check a single function ---- */

static void zc_check_function(ZerCheck *zc, Node *func) {
    if (!func->func_decl.body) return;

    /* init path state */
    PathState ps;
    memset(&ps, 0, sizeof(PathState));

    zc_check_stmt(zc, &ps, func->func_decl.body);
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

    /* check each function body */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_FUNC_DECL) {
            zc_check_function(zc, decl);
        }
    }

    return zc->error_count == 0;
}
