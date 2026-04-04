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

/* Check if a function call is free() — extern void function taking *opaque/*T. */
static bool is_free_call(Node *call, char *arg_key, int *arg_key_len, int key_bufsize) {
    if (!call || call->kind != NODE_CALL) return false;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_IDENT) return false;
    /* must be named "free" */
    if (callee->ident.name_len != 4 ||
        memcmp(callee->ident.name, "free", 4) != 0) return false;
    /* must have exactly 1 argument */
    if (call->call.arg_count != 1) return false;
    /* extract the argument key */
    *arg_key_len = handle_key_from_expr(call->call.args[0], arg_key, key_bufsize);
    return *arg_key_len > 0;
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
    /* Task.delete(h) / Task.delete_ptr(p) — auto-Slab free via struct method */
    if (pool_type && pool_type->kind == TYPE_STRUCT) {
        if ((mlen == 6 && memcmp(method, "delete", 6) == 0) ||
            (mlen == 10 && memcmp(method, "delete_ptr", 10) == 0)) {
            if (node->call.arg_count > 0) {
                char hkey[128];
                int hklen = handle_key_from_expr(node->call.args[0], hkey, sizeof(hkey));
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
            char hkey[128];
            int hklen = handle_key_from_expr(node->call.args[0], hkey, sizeof(hkey));
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
            char hkey[128];
            int hklen = handle_key_from_expr(node->call.args[0], hkey, sizeof(hkey));
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
             (mlen == 9 && memcmp(method, "alloc_ptr", 9) == 0) ||
             (mlen == 3 && memcmp(method, "new", 3) == 0) ||
             (mlen == 7 && memcmp(method, "new_ptr", 7) == 0)) &&
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
            HandleInfo *h = find_handle(ps, vname, vlen);
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
            }
            } /* end else (not arena) */
        }
    }

    /* Level 1: *opaque p = malloc/calloc/strdup(...) — register as ALIVE */
    if (alloc_call && is_alloc_call(zc, alloc_call)) {
        HandleInfo *h = find_handle(ps, vname, vlen);
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
        }
    }

    /* Handle aliasing: Handle(T) alias = existing_handle;
     * BUG-357: also match NODE_INDEX and NODE_FIELD on init side. */
    {
        char src_key[128];
        int sklen = handle_key_from_expr(init, src_key, sizeof(src_key));
        if (sklen > 0) {
            HandleInfo *src = find_handle(ps, src_key, (uint32_t)sklen);
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
                        }
                    }
                }
            }
        }
    }
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
            if (is_free_call(node, fkey, &fklen, sizeof(fkey))) {
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
        for (int i = 0; i < node->call.arg_count; i++)
            zc_check_expr(zc, ps, node->call.args[i]);
        break;
    case NODE_ASSIGN:
        zc_check_expr(zc, ps, node->assign.target);
        zc_check_expr(zc, ps, node->assign.value);
        /* BUG-361/357: handle assignment from pool.alloc() — works for globals,
         * array elements, struct fields.
         * arr[0] = pool.alloc() orelse return → register "arr[0]" in PathState */
        {
            char tkey[128];
            int tklen = handle_key_from_expr(node->assign.target, tkey, sizeof(tkey));
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
                        }
                    }
                }
                /* Handle aliasing via assignment: alias = tracked_handle
                 * BUG-357: supports arr[0] = h, s.h = other_h, etc. */
                {
                    char skey[128];
                    int sklen = handle_key_from_expr(node->assign.value, skey, sizeof(skey));
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
                                }
                            }
                        }
                    }
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
         * t.field where t is freed → use-after-free. Check the root ident. */
        {
            Node *root = node->field.object;
            while (root && (root->kind == NODE_FIELD || root->kind == NODE_INDEX)) {
                if (root->kind == NODE_FIELD) root = root->field.object;
                else root = root->index_expr.object;
            }
            if (root && root->kind == NODE_IDENT) {
                char fkey[128];
                int fklen = handle_key_from_expr(root, fkey, sizeof(fkey));
                if (fklen > 0) {
                    HandleInfo *h = find_handle(ps, fkey, (uint32_t)fklen);
                    if (h && h->state == HS_FREED) {
                        zc_error(zc, node->loc.line,
                            "use-after-free: '%.*s' freed at line %d",
                            fklen, fkey, h->free_line);
                    } else if (h && h->state == HS_MAYBE_FREED) {
                        zc_error(zc, node->loc.line,
                            "use-after-free: '%.*s' may have been freed at line %d",
                            fklen, fkey, h->free_line);
                    }
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
            char skey[128];
            int sklen = handle_key_from_expr(src, skey, sizeof(skey));
            if (sklen > 0) {
                HandleInfo *h = find_handle(ps, skey, (uint32_t)sklen);
                if (h && h->state == HS_FREED) {
                    zc_error(zc, node->loc.line,
                        "use-after-free: '%.*s' freed at line %d",
                        sklen, skey, h->free_line);
                } else if (h && h->state == HS_MAYBE_FREED) {
                    zc_error(zc, node->loc.line,
                        "use-after-free: '%.*s' may have been freed at line %d",
                        sklen, skey, h->free_line);
                }
            }
        }
        /* recurse into intrinsic args */
        for (int i = 0; i < node->intrinsic.arg_count; i++)
            zc_check_expr(zc, ps, node->intrinsic.args[i]);
        break;
    }
    case NODE_UNARY:
        /* Level 1: check deref on freed *opaque — *ptr after free */
        if (node->unary.op == TOK_STAR) {
            char dkey[128];
            int dklen = handle_key_from_expr(node->unary.operand, dkey, sizeof(dkey));
            if (dklen > 0) {
                HandleInfo *h = find_handle(ps, dkey, (uint32_t)dklen);
                if (h && h->state == HS_FREED) {
                    zc_error(zc, node->loc.line,
                        "use-after-free: '%.*s' freed at line %d",
                        dklen, dkey, h->free_line);
                } else if (h && h->state == HS_MAYBE_FREED) {
                    zc_error(zc, node->loc.line,
                        "use-after-free: '%.*s' may have been freed at line %d",
                        dklen, dkey, h->free_line);
                }
            }
        }
        zc_check_expr(zc, ps, node->unary.operand);
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
                    }
                }
            }
        }
        zc_check_stmt(zc, &then_state, node->if_stmt.then_body);

        if (node->if_stmt.else_body) {
            PathState else_state = pathstate_copy(ps);
            zc_check_stmt(zc, &else_state, node->if_stmt.else_body);

            /* merge: if freed on BOTH → FREED, if freed on ONE → MAYBE_FREED */
            for (int i = 0; i < ps->handle_count; i++) {
                HandleInfo *th = find_handle(&then_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                HandleInfo *eh = find_handle(&else_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                if (!th || !eh) continue;
                bool t_freed = (th->state == HS_FREED || th->state == HS_MAYBE_FREED);
                bool e_freed = (eh->state == HS_FREED || eh->state == HS_MAYBE_FREED);
                if (t_freed && e_freed) {
                    ps->handles[i].state = HS_FREED;
                    ps->handles[i].free_line = th->free_line;
                } else if (t_freed || e_freed) {
                    ps->handles[i].state = HS_MAYBE_FREED;
                    ps->handles[i].free_line = t_freed ? th->free_line : eh->free_line;
                }
            }
            pathstate_free(&else_state);
        } else {
            /* if without else: then-branch frees → MAYBE_FREED
             * (may or may not take the branch) */
            for (int i = 0; i < ps->handle_count; i++) {
                HandleInfo *th = find_handle(&then_state, ps->handles[i].name,
                                             ps->handles[i].name_len);
                if (th && (th->state == HS_FREED || th->state == HS_MAYBE_FREED)) {
                    ps->handles[i].state = HS_MAYBE_FREED;
                    ps->handles[i].free_line = th->free_line;
                }
            }
        }
        pathstate_free(&then_state);
        break;
    }

    case NODE_FOR:
    case NODE_WHILE: {
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
        zc_check_stmt(zc, ps, body);

        /* unconditional free inside loop — definite error */
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

        /* Loop second pass: check if any handle state changed from pre-loop.
         * If yes, run the body once more. If state still unstable → MAYBE_FREED.
         * This catches conditional frees that span iterations. */
        bool state_changed = false;
        for (int i = 0; i < ps->handle_count; i++) {
            HandleInfo *pre = find_handle(&pre_loop, ps->handles[i].name,
                                          ps->handles[i].name_len);
            if (pre && pre->state != ps->handles[i].state) {
                state_changed = true;
                break;
            }
        }
        if (state_changed) {
            PathState pass2_pre = pathstate_copy(ps);
            zc_check_stmt(zc, ps, body);
            /* if state still changing after 2nd pass → widen to MAYBE_FREED */
            for (int i = 0; i < ps->handle_count; i++) {
                HandleInfo *p2 = find_handle(&pass2_pre, ps->handles[i].name,
                                              ps->handles[i].name_len);
                if (p2 && p2->state != ps->handles[i].state &&
                    ps->handles[i].state != HS_MAYBE_FREED) {
                    ps->handles[i].state = HS_MAYBE_FREED;
                    ps->handles[i].free_line = ps->handles[i].free_line;
                }
            }
            pathstate_free(&pass2_pre);
        }

        pathstate_free(&pre_loop);
        break;
    }

    case NODE_RETURN:
        if (node->ret.expr)
            zc_check_expr(zc, ps, node->ret.expr);
        /* 9c: check if returning a freed/maybe-freed pointer */
        if (node->ret.expr) {
            char rkey[128];
            int rklen = handle_key_from_expr(node->ret.expr, rkey, sizeof(rkey));
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
        }
        break;

    case NODE_DEFER:
        break;

    case NODE_SWITCH: {
        zc_check_expr(zc, ps, node->switch_stmt.expr);
        /* track which handles are freed in each arm */
        bool *freed_all = NULL;
        bool *freed_any = NULL;
        int *freed_line = NULL;
        if (ps->handle_count > 0 && node->switch_stmt.arm_count > 0) {
            freed_all = calloc(ps->handle_count, sizeof(bool));
            freed_any = calloc(ps->handle_count, sizeof(bool));
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
                    bool arm_freed = ah && (ah->state == HS_FREED || ah->state == HS_MAYBE_FREED);
                    if (!arm_freed) {
                        freed_all[j] = false;
                    } else {
                        freed_any[j] = true;
                        if (i == 0) freed_line[j] = ah->free_line;
                    }
                }
            }
            pathstate_free(&arm_state);
        }
        /* merge: ALL arms freed → FREED, SOME arms freed → MAYBE_FREED */
        if (freed_all) {
            for (int j = 0; j < ps->handle_count; j++) {
                if (freed_all[j]) {
                    ps->handles[j].state = HS_FREED;
                    ps->handles[j].free_line = freed_line[j];
                } else if (freed_any[j]) {
                    ps->handles[j].state = HS_MAYBE_FREED;
                    ps->handles[j].free_line = freed_line[j];
                }
            }
            free(freed_all);
            free(freed_any);
            free(freed_line);
        }
        break;
    }

    default:
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
    return NULL;
}

/* Build a summary for one function: what does it do to its Handle params?
 * Uses the existing zc_check_stmt walker with error suppression. */
static void zc_build_summary(ZerCheck *zc, Node *func) {
    if (!func->func_decl.body) return;
    if (func->func_decl.param_count == 0) return;

    /* check if any param is Handle(T) or *T (trackable) */
    bool has_handle_param = false;
    for (int i = 0; i < func->func_decl.param_count; i++) {
        TypeNode *tnode = func->func_decl.params[i].type;
        if (tnode && (tnode->kind == TYNODE_HANDLE || tnode->kind == TYNODE_POINTER)) {
            has_handle_param = true;
            break;
        }
    }
    if (!has_handle_param) return;

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

    /* store summary */
    if (zc->summary_count >= zc->summary_capacity) {
        int new_cap = zc->summary_capacity * 2;
        if (new_cap < 8) new_cap = 8;
        FuncSummary *new_s = realloc(zc->summaries, new_cap * sizeof(FuncSummary));
        if (!new_s) { free(frees); free(maybe_frees); pathstate_free(&ps); zc->building_summary = false; return; }
        zc->summaries = new_s;
        zc->summary_capacity = new_cap;
    }
    FuncSummary *s = &zc->summaries[zc->summary_count++];
    s->func_name = func->func_decl.name;
    s->func_name_len = (uint32_t)func->func_decl.name_len;
    s->param_count = pc;
    s->frees_param = frees;
    s->maybe_frees_param = maybe_frees;

    pathstate_free(&ps);
    zc->building_summary = false;
}

/* Apply function summary at a call site: mark handle args as freed/maybe-freed
 * based on what the callee does to its params. */
static void zc_apply_summary(ZerCheck *zc, PathState *ps, Node *call_node) {
    if (!call_node || call_node->kind != NODE_CALL) return;
    Node *callee = call_node->call.callee;
    if (!callee || callee->kind != NODE_IDENT) return;

    FuncSummary *s = find_summary(zc, callee->ident.name,
        (uint32_t)callee->ident.name_len);
    if (!s) return;

    int arg_count = call_node->call.arg_count;
    if (arg_count > s->param_count) arg_count = s->param_count;

    for (int i = 0; i < arg_count; i++) {
        if (!s->frees_param[i] && !s->maybe_frees_param[i]) continue;

        /* get the handle key for this argument */
        char hkey[128];
        int hklen = handle_key_from_expr(call_node->call.args[i], hkey, sizeof(hkey));
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

    /* Leak detection: any handle still ALIVE or MAYBE_FREED at function exit
     * that was allocated inside this function (not a parameter) → warning.
     * Parameters (alloc_line == func->loc.line, pool_id == -1) are excluded —
     * the caller is responsible for freeing parameter handles. */
    for (int i = 0; i < ps.handle_count; i++) {
        bool is_param = (ps.handles[i].pool_id == -1 && ps.handles[i].alloc_line == (int)func->loc.line);
        if (is_param) continue;
        if (ps.handles[i].state == HS_ALIVE) {
            zc_warning(zc, ps.handles[i].alloc_line,
                "handle '%.*s' allocated but never freed in this function",
                (int)ps.handles[i].name_len, ps.handles[i].name);
        } else if (ps.handles[i].state == HS_MAYBE_FREED) {
            zc_warning(zc, ps.handles[i].alloc_line,
                "handle '%.*s' may not be freed on all paths",
                (int)ps.handles[i].name_len, ps.handles[i].name);
        }
    }

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
            if (sym && sym->type && (sym->type->kind == TYPE_POOL || sym->type->kind == TYPE_SLAB)) {
                register_pool(zc, decl->var_decl.name,
                    (uint32_t)decl->var_decl.name_len);
            }
        }
    }

    /* pre-scan: build cross-function summaries for all functions with Handle params */
    for (int i = 0; i < file_node->file.decl_count; i++) {
        Node *decl = file_node->file.decls[i];
        if (decl->kind == NODE_FUNC_DECL) {
            zc_build_summary(zc, decl);
        }
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
