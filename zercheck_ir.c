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
#include "src/safety/handle_state.h"   /* zer_handle_state_is_invalid — VST-verified */
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
    /* Level B guarded refinement (2026-06-27): the BLOCK index where this handle
     * was freed (-1 = not freed / unknown). Paired with the per-block guard sets
     * (ZerCheck.gr_block_guards) to decide, at a MAYBE_FREED use, whether the
     * use's guard is DISJOINT from the free's guard (`if(c){free} if(!c){use}`
     * recovery). Preserved through the ALIVE+FREED→MAYBE_FREED merge. */
    int free_block;
    /* Level B leak-coverage: set when this handle has been freed under a
     * condition AND its exact SINGLETON complement (free under (C,+) in one
     * block and (C,-) in another, each guard set being exactly {(C,·)} so there
     * is no enclosing condition that could skip both) — i.e. freed on ALL paths.
     * A MAYBE_FREED handle with this set is NOT a leak at exit. OR-carried
     * through merges (sound: a genuine complementary coverage admits no
     * still-alive path). */
    int freed_all_paths;
    /* Level B guard-multiplicity gate (audit 2026-07-06). `free_block` records
     * only ONE free site; a handle freed under COMPLEMENTARY guards
     * (`if(c){free} if(!c){free}`) forgets the second free, so a later use whose
     * guard matched the second free was wrongly judged disjoint from the first
     * (remembered) free and accepted — a real use-after-free / double-free. Set
     * when a SECOND guard-disjoint free is accepted; once set, the disjoint-use
     * relaxation is DISABLED for this handle (ir_use_guard_disjoint returns
     * false), so any subsequent use/free re-errors. OR-carried through merges
     * and set across the alias group. Sound: over-rejects only the exotic
     * non-complementary multi-guard-free case (soundness-first). */
    bool multi_freed;
    int alloc_id;          /* groups aliases — same alloc = same id */
    bool escaped;          /* returned, stored to global, etc. */
    /* bh18_1b (2026-07-01): this handle (or its alias group) tracks a
     * move-struct STACK LOCAL registered so `*T p = &a` can alias it (and the
     * later `T b = a` transfer can propagate TRANSFERRED to p). It is NOT an
     * allocation — the leak check MUST skip it (the move local itself is already
     * skipped by ir_should_track_move, but a pointer ALIAS `p` is not a move
     * type, so without this flag it would be flagged as a false leak). Inherited
     * by aliases through ir_snapshot_alias / ir_apply_alias. */
    bool is_move_local;
    /* Phase D1: allocation color — tracks where memory came from.
     * ZC_COLOR_POOL   — Pool/Slab, needs individual free or defer.
     * ZC_COLOR_ARENA  — Arena, freed by arena.reset(). Skip leak check.
     * ZC_COLOR_MALLOC — extern malloc/calloc, needs matching free.
     * ZC_COLOR_UNKNOWN (0) — param, cinclude, can't determine. */
    int source_color;
    /* Phase D3: ThreadHandle — from scoped spawn. Leak = "thread not joined". */
    bool is_thread_handle;
    /* F3.2 (2026-05-04): name of the Pool/Slab variable this handle was
     * allocated from (e.g., "pool_a"). NULL/0 if not from a Pool/Slab
     * method (params, malloc, arena, etc.). Used by IRMC_GET/IRMC_FREE
     * sites to detect cross-pool misuse: pool_a.alloc() then pool_b.get(h)
     * is undefined behavior at runtime (different slot arrays).
     *
     * String pointer is into the source AST (NODE_IDENT name); valid
     * for the duration of compilation, which outlives zercheck_ir
     * analysis. */
    const char *pool_name;
    uint32_t pool_name_len;
    /* Control-flow oracle (2026-06-07): set once a defer-double-free has been
     * reported for this handle, to avoid duplicate diagnostics when the C3
     * exit pass re-scans the same defer body. */
    bool defer_double_reported;
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

/* Forward declarations for functions used out-of-order */
static void ir_propagate_alias_state(IRPathState *ps, IRHandleInfo *target,
                                      IRHandleState new_state, int line);

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
    h->free_block = -1;   /* Level B: 0 is a valid block, so -1 = "not freed" */
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
 * Level B guarded-refinement support (2026-06-27)
 *
 * Certified by proofs/operational/lambda_zer_handle/handle_flow_lattice.v
 * Level B: a USE under guard ¬c is sound when the FREE is under the DISJOINT
 * guard c (`if(c){free(h)} ... if(!c){use(h)}`). The flat Level-A lattice
 * widens the post-free join to MAYBE_FREED and rejects the use; this refinement
 * recovers it WITH NO soundness loss — it fires ONLY when disjointness is
 * provable, else falls back to the conservative MAYBE_FREED.
 *
 * SOUNDNESS GATE — the "looks-disjoint-but-isn't" defense. We only ever track a
 * guard whose root is an IMMUTABLE boolean local: bool-typed, defined at most
 * once (value stable at every later read), and address NEVER taken (cannot be
 * mutated through an alias). Then the guard's truth value is identical at the
 * free's branch and at the use's branch, so `c` at the free and `c` at the use
 * are the SAME value and `c ∧ ¬c = False` genuinely holds. Anything else —
 * a reassigned condition, an &-taken condition, a comparison/`&&`/call result,
 * two unrelated conditions — is NOT tracked and stays MAYBE_FREED (Level A).
 * ================================================================ */

/* Resolve a branch-condition LOCAL to its ROOT boolean local + polarity by
 * tracing IR_UNOP(!) and IR_COPY chains back to a single definition. Sets
 * *polarity (true = condition equals root, false = condition equals ¬root) and
 * returns the root local id, or -1 if the condition is not a simple chain
 * rooted at one local (e.g. a comparison or call result is its own root — only
 * usable if ir_local_is_immutable_bool accepts it; `&&`/`||`/multi-def stop the
 * trace). Step-capped; no behavior change on its own. */
static int ir_resolve_cond_root(IRFunc *func, int cond_local, bool *polarity) {
    bool pol = true;
    int cur = cond_local;
    for (int steps = 0; steps < 16 && cur >= 0 && cur < func->local_count; steps++) {
        /* Find the UNIQUE defining instruction of `cur` (dest_local == cur).
         * If there isn't exactly one, `cur` is the root (stop tracing). */
        IRInst *def = NULL;
        int def_count = 0;
        for (int bi = 0; bi < func->block_count && def_count < 2; bi++) {
            IRBlock *bb = &func->blocks[bi];
            for (int ii = 0; ii < bb->inst_count; ii++) {
                if (bb->insts[ii].dest_local == cur) {
                    def = &bb->insts[ii];
                    if (++def_count >= 2) break;
                }
            }
        }
        if (def_count != 1 || !def) break;
        if (def->op == IR_UNOP && def->op_token == TOK_BANG && def->src1_local >= 0) {
            pol = !pol;
            cur = def->src1_local;
            continue;
        }
        if (def->op == IR_COPY && def->src1_local >= 0) {
            cur = def->src1_local;
            continue;
        }
        break; /* any other defining op → `cur` is the root */
    }
    if (cur < 0 || cur >= func->local_count) return -1;
    *polarity = pol;
    return cur;
}

/* Exhaustive recursive AST scan: does the subtree REASSIGN `name` (a NODE_ASSIGN
 * whose target ident is `name` — the defining NODE_VAR_DECL does NOT count) or
 * take its ADDRESS (`&name`)? Either makes the value non-stable, so a guard
 * built on it cannot be trusted to mean the same thing at the free and at the
 * use. This is the COMPLETE source-level check — writes and address-takes hide
 * in AST exprs (call args like `flip(&c)`, defer bodies, etc.) that the flat IR
 * instruction fields do not expose. No-default switch (walker_default_audit +
 * -Werror=switch): a new NodeKind forces a decision here; opaque/rare kinds
 * (cast/asm/static_assert/declarations) return true (assume mutation) so a gap
 * can only OVER-reject, never accept a mutated condition. */
static bool ast_name_mutated_or_addrd(Node *n, const char *name, uint32_t len) {
    if (!n) return false;
    switch (n->kind) {
    case NODE_ASSIGN: {
        Node *t = n->assign.target;
        if (t && t->kind == NODE_IDENT &&
            (uint32_t)t->ident.name_len == len &&
            memcmp(t->ident.name, name, len) == 0)
            return true;                            /* reassignment */
        return ast_name_mutated_or_addrd(n->assign.target, name, len) ||
               ast_name_mutated_or_addrd(n->assign.value, name, len);
    }
    case NODE_UNARY:
        if (n->unary.op == TOK_AMP && n->unary.operand &&
            n->unary.operand->kind == NODE_IDENT &&
            (uint32_t)n->unary.operand->ident.name_len == len &&
            memcmp(n->unary.operand->ident.name, name, len) == 0)
            return true;                            /* address taken */
        return ast_name_mutated_or_addrd(n->unary.operand, name, len);
    case NODE_BINARY:
        return ast_name_mutated_or_addrd(n->binary.left, name, len) ||
               ast_name_mutated_or_addrd(n->binary.right, name, len);
    case NODE_CALL: {
        if (ast_name_mutated_or_addrd(n->call.callee, name, len)) return true;
        for (int i = 0; i < n->call.arg_count; i++)
            if (ast_name_mutated_or_addrd(n->call.args[i], name, len)) return true;
        return false;
    }
    case NODE_FIELD:  return ast_name_mutated_or_addrd(n->field.object, name, len);
    case NODE_INDEX:  return ast_name_mutated_or_addrd(n->index_expr.object, name, len) ||
                             ast_name_mutated_or_addrd(n->index_expr.index, name, len);
    case NODE_SLICE:  return ast_name_mutated_or_addrd(n->slice.object, name, len) ||
                             ast_name_mutated_or_addrd(n->slice.start, name, len) ||
                             ast_name_mutated_or_addrd(n->slice.end, name, len);
    case NODE_ORELSE: return ast_name_mutated_or_addrd(n->orelse.expr, name, len) ||
                             ast_name_mutated_or_addrd(n->orelse.fallback, name, len);
    case NODE_TYPECAST: return ast_name_mutated_or_addrd(n->typecast.expr, name, len);
    case NODE_INTRINSIC: {
        for (int i = 0; i < n->intrinsic.arg_count; i++)
            if (ast_name_mutated_or_addrd(n->intrinsic.args[i], name, len)) return true;
        return false;
    }
    case NODE_STRUCT_INIT: {
        for (int i = 0; i < n->struct_init.field_count; i++)
            if (ast_name_mutated_or_addrd(n->struct_init.fields[i].value, name, len)) return true;
        return false;
    }
    case NODE_BLOCK: {
        for (int i = 0; i < n->block.stmt_count; i++)
            if (ast_name_mutated_or_addrd(n->block.stmts[i], name, len)) return true;
        return false;
    }
    case NODE_IF: return ast_name_mutated_or_addrd(n->if_stmt.cond, name, len) ||
                         ast_name_mutated_or_addrd(n->if_stmt.then_body, name, len) ||
                         ast_name_mutated_or_addrd(n->if_stmt.else_body, name, len);
    case NODE_FOR: return ast_name_mutated_or_addrd(n->for_stmt.init, name, len) ||
                          ast_name_mutated_or_addrd(n->for_stmt.cond, name, len) ||
                          ast_name_mutated_or_addrd(n->for_stmt.step, name, len) ||
                          ast_name_mutated_or_addrd(n->for_stmt.body, name, len);
    case NODE_WHILE:
    case NODE_DO_WHILE: return ast_name_mutated_or_addrd(n->while_stmt.cond, name, len) ||
                               ast_name_mutated_or_addrd(n->while_stmt.body, name, len);
    case NODE_SWITCH: {
        if (ast_name_mutated_or_addrd(n->switch_stmt.expr, name, len)) return true;
        for (int i = 0; i < n->switch_stmt.arm_count; i++)
            if (ast_name_mutated_or_addrd(n->switch_stmt.arms[i].body, name, len)) return true;
        return false;
    }
    case NODE_RETURN:    return ast_name_mutated_or_addrd(n->ret.expr, name, len);
    case NODE_EXPR_STMT: return ast_name_mutated_or_addrd(n->expr_stmt.expr, name, len);
    case NODE_VAR_DECL:  return ast_name_mutated_or_addrd(n->var_decl.init, name, len);
    case NODE_DEFER:     return ast_name_mutated_or_addrd(n->defer.body, name, len);
    case NODE_CRITICAL:  return ast_name_mutated_or_addrd(n->critical.body, name, len);
    case NODE_ONCE:      return ast_name_mutated_or_addrd(n->once.body, name, len);
    case NODE_AWAIT:     return ast_name_mutated_or_addrd(n->await_stmt.cond, name, len);
    case NODE_SPAWN: {
        for (int i = 0; i < n->spawn_stmt.arg_count; i++)
            if (ast_name_mutated_or_addrd(n->spawn_stmt.args[i], name, len)) return true;
        return false;
    }
    /* Leaves — cannot reassign or take the address of `name`. */
    case NODE_IDENT: case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_BREAK: case NODE_CONTINUE: case NODE_GOTO: case NODE_LABEL:
    case NODE_YIELD: case NODE_SIZEOF:
        return false;
    /* Opaque / rare / non-body kinds — conservative (assume mutation). */
    case NODE_CAST: case NODE_ASM: case NODE_STATIC_ASSERT:
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
        return true;
    }
    return true;  /* unreachable (exhaustive) — conservative */
}

/* A local is a TRACKABLE guard root iff bool-typed and NEITHER reassigned NOR
 * address-taken anywhere in the function (so its value is identical at the free's
 * branch and the use's branch — the soundness gate for the guarded refinement).
 * Uses the complete source-AST scan (writes/&c hide in exprs the IR flattens),
 * plus an IR_ADDR_OF backup. Conservative — when in doubt, false (→ MAYBE). */
static bool ir_local_is_immutable_bool(IRFunc *func, int local) {
    if (local < 0 || local >= func->local_count) return false;
    if (type_dispatch_kind(func->locals[local].type) != TYPE_BOOL) return false;
    const char *lname = func->locals[local].orig_name
        ? func->locals[local].orig_name : func->locals[local].name;
    uint32_t lname_len = func->locals[local].orig_name
        ? func->locals[local].orig_name_len : func->locals[local].name_len;
    if (!lname || lname_len == 0) return false;
    if (!func->ast_node || func->ast_node->kind != NODE_FUNC_DECL) return false;
    if (ast_name_mutated_or_addrd(func->ast_node->func_decl.body, lname, lname_len))
        return false;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        for (int ii = 0; ii < bb->inst_count; ii++)
            if (bb->insts[ii].op == IR_ADDR_OF && bb->insts[ii].src1_local == local)
                return false;   /* synthesized address-of the source walk can't see */
    }
    return true;
}

/* A guard = a (root bool local, polarity) decision known to hold (pol 1 = root
 * is true, 0 = root is false). A block's guard SET is every guard that holds on
 * EVERY path reaching that block. Small fixed cap (8) — overflow just drops
 * guards (conservative: fewer guards = fewer relaxations = still sound). */
typedef struct { int root; int8_t pol; } IRGuard;
typedef struct { IRGuard g[8]; int count; } IRGuardSet;

static bool ir_gs_has(const IRGuardSet *s, int root, int pol) {
    for (int i = 0; i < s->count; i++)
        if (s->g[i].root == root && s->g[i].pol == (int8_t)pol) return true;
    return false;
}
static void ir_gs_add(IRGuardSet *s, int root, int pol) {
    if (root < 0 || ir_gs_has(s, root, pol)) return;
    if (s->count < 8) {
        s->g[s->count].root = root;
        s->g[s->count].pol = (int8_t)pol;
        s->count++;
    }
}

/* The (root, polarity) decision a branch terminator contributes on its edge to
 * successor `succ`, or root=-1 if none (non-branch terminator, both edges to
 * succ, or a condition that is not an immutable boolean we can track). */
static IRGuard ir_edge_label(IRFunc *func, IRBlock *pred, int succ) {
    IRGuard none = { -1, 0 };
    if (pred->inst_count == 0) return none;
    IRInst *term = &pred->insts[pred->inst_count - 1];
    if (term->op != IR_BRANCH) return none;
    if (term->true_block == succ && term->false_block == succ) return none;
    bool pol;
    int root = ir_resolve_cond_root(func, term->cond_local, &pol);
    if (root < 0 || !ir_local_is_immutable_bool(func, root)) return none;
    if (term->true_block == succ)  { IRGuard r = { root, (int8_t)(pol ? 1 : 0) }; return r; }
    if (term->false_block == succ) { IRGuard r = { root, (int8_t)(pol ? 0 : 1) }; return r; }
    return none;
}

/* Per-block guard set = the guards holding on ALL paths to the block. SINGLE
 * forward pass: blocks are in topological order for forward edges, so a block's
 * forward preds are already computed; any block reachable via a BACK edge (pred
 * index >= own, i.e. a loop) gets the EMPTY set — sound and conservative (we
 * never claim a loop-carried guard). Entry / unreachable → empty. Misordering
 * (a forward pred numbered higher) only ever yields empty too → still sound.
 * Caller owns the returned array (func->block_count entries); free with free(). */
static IRGuardSet *ir_compute_block_guards(IRFunc *func) {
    IRGuardSet *bg = (IRGuardSet *)calloc(func->block_count > 0 ? func->block_count : 1,
                                          sizeof(IRGuardSet));
    if (!bg) return NULL;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        bg[bi].count = 0;
        if (bb->pred_count == 0) continue;
        bool back_edge = false;
        for (int pi = 0; pi < bb->pred_count; pi++)
            if (bb->preds[pi] >= bi) { back_edge = true; break; }
        if (back_edge) continue;
        for (int pi = 0; pi < bb->pred_count; pi++) {
            int pj = bb->preds[pi];
            IRGuardSet contrib = bg[pj];   /* by-value copy of the small struct */
            IRGuard lbl = ir_edge_label(func, &func->blocks[pj], bi);
            if (lbl.root >= 0) ir_gs_add(&contrib, lbl.root, lbl.pol);
            if (pi == 0) {
                bg[bi] = contrib;
            } else {
                IRGuardSet inter; inter.count = 0;
                for (int k = 0; k < bg[bi].count; k++)
                    if (ir_gs_has(&contrib, bg[bi].g[k].root, bg[bi].g[k].pol))
                        ir_gs_add(&inter, bg[bi].g[k].root, bg[bi].g[k].pol);
                bg[bi] = inter;
            }
        }
    }
    return bg;
}

/* Level B use-site decision (the relaxation): is a use/free of MAYBE_FREED
 * handle h SOUND at the current block because the use's guard is DISJOINT from
 * the free's guard? (`if(c){free(h)} ... if(!c){use(h)}` — free under c, use
 * under ¬c, c∧¬c=False, so the use never sees a freed handle.) Certified by
 * handle_flow_lattice.v Level B (guarded_use_sound / guarded_not_disjoint_rejects).
 *
 * SOUNDNESS — conservative in every direction:
 *   - ONLY IR_HS_MAYBE_FREED is eligible. IR_HS_FREED is a definite free on ALL
 *     paths (a real UAF/double-free) and IR_HS_TRANSFERRED is the move axis —
 *     both keep erroring.
 *   - "disjoint" = there is an immutable-bool root C with (C,p) in the FREE
 *     block's guard set and (C,¬p) in the USE block's — a genuine contradiction,
 *     so the free's path and the use's path are mutually exclusive. The guard
 *     sets contain ONLY immutable-bool roots (ir_local_is_immutable_bool), so C
 *     has the same value at both branches.
 *   - NULL guards / out-of-range blocks / no contradiction → false (→ reject).
 * So a wrong/absent guard can only OVER-reject, never accept an unsafe use. */
static bool ir_use_guard_disjoint(ZerCheck *zc, IRHandleInfo *h) {
    if (!h || h->state != IR_HS_MAYBE_FREED) return false;
    /* Guard-multiplicity gate (audit 2026-07-06): once this handle has been
     * freed under two distinct guards, `free_block` (a single site) can no
     * longer represent every free, so disjointness against it is unsound —
     * disable the relaxation (the handle is freed on ≥2 mutually-exclusive
     * paths; recovering the rare still-safe path would need the full free-guard
     * SET). Soundness-first: this only over-rejects. */
    if (h->multi_freed) return false;
    if (!zc->gr_block_guards) return false;
    int fb = h->free_block;
    int ub = zc->gr_cur_block;
    if (fb < 0 || fb >= zc->gr_block_count) return false;
    if (ub < 0 || ub >= zc->gr_block_count) return false;
    IRGuardSet *bg = (IRGuardSet *)zc->gr_block_guards;
    const IRGuardSet *gf = &bg[fb];
    const IRGuardSet *gu = &bg[ub];
    for (int i = 0; i < gf->count; i++) {
        /* free under (C,p), use under (C,¬p) → guards disjoint → use is safe */
        if (ir_gs_has(gu, gf->g[i].root, gf->g[i].pol ? 0 : 1))
            return true;
    }
    return false;
}

/* Level B leak-coverage: does freeing MAYBE_FREED handle h in the CURRENT block
 * COMPLETE its coverage to all paths? True iff the prior free's guard set and
 * the current block's guard set are each a SINGLE complementary decision —
 * {(C,+)} and {(C,-)}. The singleton requirement is the soundness gate: with no
 * other guards, C partitions ALL paths, so freeing under both polarities frees
 * on every path (no enclosing condition can skip both — that case has a count>1
 * guard set and is rejected here, staying a leak). */
static bool ir_free_completes_coverage(ZerCheck *zc, IRHandleInfo *h) {
    if (!h || !zc->gr_block_guards) return false;
    int fb = h->free_block;
    int ub = zc->gr_cur_block;
    if (fb < 0 || fb >= zc->gr_block_count) return false;
    if (ub < 0 || ub >= zc->gr_block_count) return false;
    IRGuardSet *bg = (IRGuardSet *)zc->gr_block_guards;
    if (bg[fb].count != 1 || bg[ub].count != 1) return false;
    return bg[fb].g[0].root == bg[ub].g[0].root &&
           bg[fb].g[0].pol  != bg[ub].g[0].pol;
}

/* audit 2026-07-06: mark a handle (and its whole alloc_id alias group) as
 * freed under ≥2 distinct guards, which disables the disjoint-use relaxation
 * (ir_use_guard_disjoint). Called at every free site that accepts a second
 * guard-disjoint free. Alias-group loop mirrors ir_propagate_alias_state. */
static void ir_mark_multi_freed(IRPathState *ps, IRHandleInfo *h) {
    if (!h) return;
    h->multi_freed = true;
    int aid = h->alloc_id;
    if (aid == 0) return;
    for (int i = 0; i < ps->handle_count; i++)
        if (ps->handles[i].alloc_id == aid)
            ps->handles[i].multi_freed = true;
}

/* ================================================================
 * Alias-copy helper (audit 2026-06-04)
 *
 * Solves the recurring "new IRHandleInfo field added but not propagated
 * to all alias-copy sites" bug class. Refactor_ir.md helper 6.2 spec.
 *
 * Background: aliasing is when one local handle entry is created
 * representing the same underlying allocation as another. Each
 * call to ir_add_handle/ir_add_compound_handle may realloc ps->handles,
 * invalidating the source pointer — so a SNAPSHOT must be taken
 * BEFORE the add-capable call (audit 2026-04-26, BUG-617 family).
 *
 * Sites missing partial fields silently break safety checks:
 * - pool_name missing → wrong-pool detection (F3.2) bypassed
 * - escaped missing → false leak warnings on aliases that escape via src
 * - is_thread_handle missing → spawn-no-join detection bypass
 * - source_color missing → leak detection treats pool-aliases as malloc
 *
 * USAGE PATTERN:
 *   IRAliasSnapshot snap;
 *   ir_snapshot_alias(&snap, src_h);
 *   IRHandleInfo *dst_h = ir_add_handle(ps, dest_local);  // may realloc
 *   if (dst_h) {
 *       ir_apply_alias(dst_h, &snap);
 *       dst_h->state = ...;  // caller sets state explicitly (alive/inherit/etc)
 *   }
 * ================================================================ */

typedef struct {
    int alloc_line;
    int free_line;
    int alloc_id;
    int source_color;
    bool escaped;
    bool is_thread_handle;
    bool is_move_local;   /* bh18_1b: move-local handle/alias — leak-skip */
    const char *pool_name;
    uint32_t pool_name_len;
    IRHandleState state;  /* snapshot of source state, available if caller wants to inherit */
} IRAliasSnapshot;

static void ir_snapshot_alias(IRAliasSnapshot *snap, const IRHandleInfo *src) {
    snap->alloc_line = src->alloc_line;
    snap->free_line = src->free_line;
    snap->alloc_id = src->alloc_id;
    snap->source_color = src->source_color;
    snap->escaped = src->escaped;
    snap->is_thread_handle = src->is_thread_handle;
    snap->is_move_local = src->is_move_local;
    snap->pool_name = src->pool_name;
    snap->pool_name_len = src->pool_name_len;
    snap->state = src->state;
}

/* Copy all alias-provenance fields from snapshot to dst. State NOT copied —
 * caller sets state explicitly based on context (alive on creation,
 * inherit on alias, etc). Use snap->state if you want to inherit. */
static void ir_apply_alias(IRHandleInfo *dst, const IRAliasSnapshot *snap) {
    dst->alloc_line = snap->alloc_line;
    dst->free_line = snap->free_line;
    dst->alloc_id = snap->alloc_id;
    dst->source_color = snap->source_color;
    dst->escaped = snap->escaped;
    dst->is_thread_handle = snap->is_thread_handle;
    dst->is_move_local = snap->is_move_local;
    dst->pool_name = snap->pool_name;
    dst->pool_name_len = snap->pool_name_len;
}

/* ================================================================
 * State Helpers (same as zercheck.c but on IRHandleState)
 * ================================================================ */

/* Delegates to VST-verified predicate in src/safety/handle_state.c. */
static bool ir_is_invalid(IRHandleInfo *h) {
    return zer_handle_state_is_invalid(h->state) != 0;
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

/* F0.4 (2026-05-03): recursive depth-limited check ported from
 * zercheck.c:1056. Original ir_contains_move_struct_field only
 * checked ONE level deep, missing nested patterns like:
 *   move struct File { i32 fd; }
 *   struct Wrapper { File f; }
 *   struct Outer { Wrapper w; }   // contains_move at 2 levels deep
 * Audit found nested_move_struct_uaf.zer compiled clean under IR-only
 * because Outer wasn't recognized as move-tracking. Depth-limited
 * recursion (32 max) prevents infinite recursion on malformed types
 * while catching nested cases. */
static bool ir_contains_move_struct_field_depth(Type *t, int depth) {
    if (!t) return false;
    if (depth > 32) return false;
    Type *eff = type_unwrap_distinct(t);
    if (eff->kind == TYPE_STRUCT) {
        for (uint32_t i = 0; i < eff->struct_type.field_count; i++) {
            Type *ft = eff->struct_type.fields[i].type;
            if (ir_is_move_struct_type(ft)) return true;
            Type *ft_eff = ft ? type_unwrap_distinct(ft) : NULL;
            if (ft_eff && (ft_eff->kind == TYPE_STRUCT ||
                           ft_eff->kind == TYPE_UNION)) {
                if (ir_contains_move_struct_field_depth(ft, depth + 1))
                    return true;
            }
        }
    }
    if (eff->kind == TYPE_UNION) {
        for (uint32_t i = 0; i < eff->union_type.variant_count; i++) {
            Type *vt = eff->union_type.variants[i].type;
            if (ir_is_move_struct_type(vt)) return true;
            Type *vt_eff = vt ? type_unwrap_distinct(vt) : NULL;
            if (vt_eff && (vt_eff->kind == TYPE_STRUCT ||
                           vt_eff->kind == TYPE_UNION)) {
                if (ir_contains_move_struct_field_depth(vt, depth + 1))
                    return true;
            }
        }
    }
    return false;
}

static bool ir_contains_move_struct_field(Type *t) {
    return ir_contains_move_struct_field_depth(t, 0);
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

/* GAP-3 (BUG-739, 2026-06-10, 6u360k audit): pseudo-root for tracking
 * pointer-typed GLOBALS that receive a tracked allocation within the
 * current function. `g_ptr = p; heap.free_ptr(p); gp = g_ptr orelse
 * return; gp.value` was silently accepted — the store marked p escaped
 * but the global lost all connection to p's alloc_id, and alloc_ptr's
 * `*T` has no runtime generation net (unlike Handle).
 *
 * Mechanism: compound handles keyed (IR_GLOBAL_ROOT_ID, global_name)
 * reuse ALL existing machinery — CFG merge (compound-aware since
 * BUG-650), ir_propagate_alias_state on free, IRAliasSnapshot on
 * read-back. No new PathState fields.
 *
 * INVARIANT: these entries always carry escaped=true. The exit-pass
 * leak/ghost branches index func->locals[h->local_id] only AFTER the
 * `if (h->escaped) continue;` skip, so the -2 sentinel never reaches
 * a locals[] access. Keep the invariant when touching these entries.
 *
 * Scope: per-function (store→free→read-back within one body). Cross-
 * function global UAF needs FuncSummary work — see docs/limitations.md. */
#define IR_GLOBAL_ROOT_ID (-2)

/* True if ident names a module-level global NOT shadowed by any function
 * local. Locals shadow globals, so a same-named local wins. */
static bool ir_ident_is_unshadowed_global(ZerCheck *zc, IRFunc *func, Node *ident) {
    if (!ident || ident->kind != NODE_IDENT) return false;
    if (ir_find_local_exact_first(func, ident->ident.name,
                                  (uint32_t)ident->ident.name_len) >= 0)
        return false;
    return scope_lookup(zc->checker->global_scope, ident->ident.name,
                        (uint32_t)ident->ident.name_len) != NULL;
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

    /* Merge each subsequent non-terminated predecessor.
     *
     * BUG-650 (2026-05-02): both lookups MUST be compound-aware. Using
     * `ir_find_handle` (bare-only — filters by path_len == 0) silently
     * missed compound (struct.field) entries registered via
     * `ir_add_compound_handle`. Two failure modes resulted:
     *   1) First loop: a compound `b.h` FREED in one branch and ALIVE
     *      in another did not widen to MAYBE_FREED — the bare lookup
     *      returned NULL, the merge skipped this entry, result kept its
     *      pre-merge state. Silent UAF on subsequent use of `b.h`.
     *   2) Second loop's bare-keyed dedup re-added pred's compound rows
     *      as duplicates (the bare key didn't see the existing compound
     *      entry). Use-site lookup found whichever duplicate came first
     *      — non-deterministic state, sometimes ALIVE leaked through.
     *
     * Both call sites now use `ir_find_compound_handle` keyed on
     * (local_id, path, path_len). For the second loop's add-if-missing,
     * we use `ir_alloc_handle_slot` directly because `ir_add_handle`
     * goes through bare-only `ir_find_handle` and can't add a compound
     * entry.
     *
     * On main today this bug is masked by AST `zercheck.c` running in
     * parallel and catching it via different logic; the IR analyzer
     * will be the sole exit-code driver after Phase G deletes
     * zercheck.c, so the fix lands here pre-emptively. */
    for (int si = first_live + 1; si < state_count; si++) {
        if (states[si].terminated) continue; /* dead path, skip */

        /* For each handle in result, check if same handle exists in this pred */
        for (int hi = 0; hi < result.handle_count; hi++) {
            IRHandleInfo *rh = &result.handles[hi];
            IRHandleInfo *ph = ir_find_compound_handle(&states[si],
                rh->local_id, rh->path, rh->path_len);

            if (!ph) continue; /* handle not in this pred — keep result's state */

            /* Merge states: both freed → freed, one freed → maybe_freed, etc.
             *
             * F3.2 (2026-05-04): the lattice MUST be monotonic — once a
             * handle is widened to MAYBE_FREED on any path, it cannot
             * narrow back to ALIVE/FREED on a subsequent merge with a
             * pred that has ALIVE. Pre-fix: `ALIVE + MAYBE_FREED` (when
             * first_live's pred was ALIVE and a later pred is MAYBE_FREED)
             * fell through and kept ALIVE. Result: state oscillated
             * ALIVE↔MAYBE_FREED across loop iterations and convergence
             * never reached.
             *
             * Closes Pattern 3 of F3 limitations.md (free-then-realloc
             * loop FALSE POSITIVE). MAYBE_FREED is the join of ALIVE
             * and FREED in the safety lattice, so any pair containing
             * MAYBE_FREED widens to MAYBE_FREED. */
            if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_FREED) {
                rh->state = IR_HS_MAYBE_FREED;
                rh->free_line = ph->free_line;
            } else if (rh->state == IR_HS_FREED && ph->state == IR_HS_ALIVE) {
                rh->state = IR_HS_MAYBE_FREED;
            } else if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_TRANSFERRED) {
                rh->state = IR_HS_MAYBE_FREED; /* conservative */
            } else if (rh->state == IR_HS_TRANSFERRED && ph->state == IR_HS_ALIVE) {
                rh->state = IR_HS_MAYBE_FREED;
            } else if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_MAYBE_FREED) {
                rh->state = IR_HS_MAYBE_FREED;
                rh->free_line = ph->free_line;
            } else if (rh->state == IR_HS_FREED && ph->state == IR_HS_MAYBE_FREED) {
                rh->state = IR_HS_MAYBE_FREED; /* widen — pred saw maybe-free */
            } else if (rh->state == IR_HS_TRANSFERRED && ph->state == IR_HS_MAYBE_FREED) {
                rh->state = IR_HS_MAYBE_FREED;
            } else if (rh->state == IR_HS_FREED && ph->state == IR_HS_TRANSFERRED) {
                /* audit 2026-06-04: pre-fix this pair fell through and
                 * kept FREED, producing wrong diagnostic message
                 * (use-after-free instead of "consumed in some way"). */
                rh->state = IR_HS_MAYBE_FREED;
            } else if (rh->state == IR_HS_TRANSFERRED && ph->state == IR_HS_FREED) {
                rh->state = IR_HS_MAYBE_FREED;
                rh->free_line = ph->free_line;
            }
            /* Level B: carry the free BLOCK from whichever predecessor froze the
             * handle, so a MAYBE_FREED handle remembers WHERE it was freed (used
             * by the guard-disjointness check at the use). Mirror of the
             * free_line carry above; only fill when rh has none of its own. */
            if (rh->state == IR_HS_MAYBE_FREED && rh->free_block < 0 &&
                ph->free_block >= 0) {
                rh->free_block = ph->free_block;
            }
            /* Level B: OR-carry the all-paths-freed flag. Sound because it is
             * only set for SINGLETON complementary coverage (no path leaves the
             * handle alive), so a pred without it cannot contribute an alive
             * path that this would wrongly mask. */
            if (ph->freed_all_paths) rh->freed_all_paths = 1;
            /* audit 2026-07-06: OR-carry the guard-multiplicity flag so a use
             * downstream of a two-guard free sees the relaxation disabled
             * regardless of which predecessor path it arrives on. */
            if (ph->multi_freed) rh->multi_freed = true;
            /* MAYBE_FREED ↔ {ALIVE, FREED, TRANSFERRED}: rh already
             * MAYBE_FREED, keep it. Both same state → keep.
             * Both freed → keep freed. */
        }

        /* Add handles from pred that aren't in result yet (compound-aware
         * dedup; bypass ir_add_handle which uses bare-only ir_find_handle). */
        for (int pi = 0; pi < states[si].handle_count; pi++) {
            IRHandleInfo *src = &states[si].handles[pi];
            if (!ir_find_compound_handle(&result, src->local_id,
                                         src->path, src->path_len)) {
                IRHandleInfo *nh = ir_alloc_handle_slot(&result);
                if (nh) *nh = *src;
                /* Note: path is arena-allocated by ir_extract_compound_key.
                 * Sharing the pointer across path states is safe for the
                 * lifetime of zercheck_ir analysis (single-arena, single
                 * compile). */
            }
        }

        /* Axis-C fix BUG-743 (2026-06-21): merge ThreadHandle join
         * obligations across predecessors. PRE-FIX the merge unioned only
         * handles[]; threads[] rode SOLELY via ir_ps_copy(&states[first_live]),
         * so a scoped-spawn ThreadHandle created on a NON-first_live
         * predecessor was SILENTLY DROPPED at the CFG merge — the exit
         * join-scan then saw an empty threads[] and emitted no "not joined"
         * diagnostic. Because `&stack-local` into a scoped spawn is permitted
         * ONLY on the join premise (checker.c), the dropped obligation is a
         * false-green cross-thread stack-UAF. Formalized by the linear
         * join_tok merge obligation (join_tok_in_auth) in
         * proofs/operational/lambda_zer_concurrency/iris_region_join.v: a
         * linear resource dropped at a merge is unsound.
         *
         * Lattice: a thread present in ANY pred SURVIVES the merge (union by
         * name); `joined` is the AND over the preds that contain it (joined
         * only if joined on every such path). A thread joined on one branch
         * but not another stays UN-joined — mirrors the handle
         * MAYBE_FREED→error conservatism ("maybe not joined" is an error,
         * forcing join-on-all-paths). */
        for (int pti = 0; pti < states[si].thread_count; pti++) {
            IRThreadTrack *pt = &states[si].threads[pti];
            IRThreadTrack *rt = ir_find_thread(&result, pt->name, pt->name_len);
            if (rt) {
                /* AND: un-joined on any path wins; point the diagnostic at
                 * an un-joined spawn site. */
                if (!pt->joined) {
                    if (rt->joined) rt->spawn_line = pt->spawn_line;
                    rt->joined = false;
                }
            } else {
                IRThreadTrack *nt = ir_add_thread(&result, pt->name,
                                                  pt->name_len, pt->spawn_line);
                if (nt) nt->joined = pt->joined;
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

/* Stage 3 (2026-04-28): measure pass for ir_extract_compound_key.
 * Walks the same shapes as ir_build_key_path but only counts chars,
 * needs no buffer. Caller allocates exactly this many + 1 bytes.
 * Returns -1 if the expression isn't keyable. */
static int ir_measure_key_path(Node *expr) {
    if (!expr) return -1;
    if (expr->kind == NODE_IDENT) return 0;  /* bare ident — empty path */
    if (expr->kind == NODE_FIELD) {
        int base = ir_measure_key_path(expr->field.object);
        if (base < 0) return -1;
        return base + 1 + (int)expr->field.field_name_len;
    }
    if (expr->kind == NODE_INDEX) {
        if (!expr->index_expr.index ||
            expr->index_expr.index->kind != NODE_INT_LIT) return -1;
        int base = ir_measure_key_path(expr->index_expr.object);
        if (base < 0) return -1;
        char tmp[32];
        int idx_chars = snprintf(tmp, sizeof(tmp), "[%llu]",
            (unsigned long long)expr->index_expr.index->int_lit.value);
        if (idx_chars <= 0) return -1;
        return base + idx_chars;
    }
    return -1;
}

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

    /* Compound — measure-then-fill. Stage 3 (2026-04-28): exact-size
     * arena allocation. Walk once to compute the precise path length,
     * then allocate exactly that and fill. No fixed buffer, no retry,
     * no arbitrary cap — bounded only by available memory.
     * Replaces the previous 256-byte fixed buffer. */
    int need = ir_measure_key_path(expr);
    if (need < 0) return -1;  /* unkeyable */
    char *path = (char *)arena_alloc(zc->arena, need + 1);
    if (!path) return -1;
    int wrote = ir_build_key_path(expr, path, need + 1, NULL);
    if (wrote != need) return -1;  /* invariant violation */
    *out_path = path;
    *out_path_len = (uint32_t)need;
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

/* Check if function name matches one of the conventional destructor
 * keywords. Mirrors zercheck.c's pre-AST→IR-migration helper
 * `name_looks_like_destructor` (Gap 17 in docs/4-27-2026-gaps.md).
 *
 * AUDIT 2026-06-06 (GAP-D): the AST→IR migration claimed this helper
 * was "bundled into ir_is_extern_free_call" (docs/refactor_ir.md:2293)
 * but the bundling was never actually done — the bodyless heuristic
 * stayed void-return-only. Restoring widens detection to non-void
 * destructor patterns like `i32 destroy_resource(*Res r);`.
 *
 * Keep this list in sync with the 12 substrings from the AST-era
 * helper documented in CLAUDE.md "Stage 1" entry. */
static bool ir_name_looks_like_destructor(const char *name, uint32_t len) {
    static const char *kws[] = {
        "free", "destroy", "close", "release", "delete", "dispose",
        "drop", "cleanup", "deinit", "fini", "shutdown", "term"
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++) {
        uint32_t kl = (uint32_t)strlen(kws[i]);
        if (kl > len) continue;
        for (uint32_t j = 0; j + kl <= len; j++) {
            if (memcmp(name + j, kws[i], kl) == 0) return true;
        }
    }
    return false;
}

/* Check if a call is to a function that frees its first argument.
 * Either explicitly named "free" OR bodyless void fn with *opaque/*T first
 * param (signature heuristic — catches destroy/close/cleanup patterns)
 * OR bodyless non-void fn whose name matches a destructor convention
 * (Gap 17 / AUDIT 2026-06-06 GAP-D — catches `i32 destroy_resource(*R)`). */
static bool ir_is_extern_free_call(ZerCheck *zc, Node *call) {
    if (!call || call->kind != NODE_CALL) return false;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_IDENT) return false;
    if (call->call.arg_count < 1) return false;
    /* Explicit "free" */
    if (callee->ident.name_len == 4 &&
        memcmp(callee->ident.name, "free", 4) == 0) return true;
    /* Signature heuristic: bodyless fn(*opaque/*T ...).
     * Void return: always free-classified.
     * Non-void return: free-classified only if name looks like a destructor. */
    Symbol *sym = scope_lookup(zc->checker->global_scope,
        callee->ident.name, (uint32_t)callee->ident.name_len);
    if (!sym || !sym->is_function || !sym->func_node) return false;
    if (sym->func_node->func_decl.body) return false;
    Type *ret = sym->type;
    if (ret && ret->kind == TYPE_FUNC_PTR) ret = ret->func_ptr.ret;
    if (!ret) return false;
    bool is_void = (type_unwrap_distinct(ret)->kind == TYPE_VOID);
    if (!is_void) {
        /* AUDIT 2026-06-06 (GAP-D): widen heuristic to non-void returns
         * when the name suggests a destructor convention. */
        if (!ir_name_looks_like_destructor(callee->ident.name,
                                            (uint32_t)callee->ident.name_len))
            return false;
    }
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
 *   IRMC_ARENA_RESET — arena.reset() / unsafe_reset() → mark all ARENA handles FREED
 * ================================================================ */

typedef enum {
    IRMC_NONE = 0,
    IRMC_ALLOC,
    IRMC_ALLOC_PTR,
    IRMC_GET,
    IRMC_FREE,
    IRMC_FREE_PTR,
    IRMC_ARENA_ALLOC,
    IRMC_ARENA_RESET,   /* Gap 39 (2026-04-27): arena.reset()/unsafe_reset() */
} IRMethodKind;

/* Validate that the receiver of a method call is a builtin allocator
 * type (Pool/Slab/Ring/Arena), a TYPE_STRUCT (for Task.alloc auto-slab),
 * or unknown (NULL). Returns true if the receiver could legitimately
 * have one of the recognized builtin methods.
 *
 * Gap 32 fix (2026-04-27): name-only matching previously fired for any
 * method called `alloc`/`free`/etc., regardless of receiver type. With
 * future cinclude-extended struct types or user-method-style sugar, a
 * non-builtin .alloc() could trigger wrong handle tracking. Receiver
 * validation prevents that.
 *
 * NULL receiver type is permitted: ZER's checker doesn't always populate
 * the typemap for the receiver of method calls (especially after
 * desugaring). Returning false on NULL would silently drop legitimate
 * builtin classification. */
static bool ir_receiver_is_builtin_target(Checker *c, Node *callee) {
    if (!callee || callee->kind != NODE_FIELD) return false;
    Node *recv = callee->field.object;
    if (!recv) return false;
    Type *rt = checker_get_type(c, recv);
    if (!rt) {
        /* Fall back to symbol lookup for bare ident receivers. */
        if (recv->kind == NODE_IDENT && c) {
            Symbol *sym = scope_lookup(c->current_scope,
                recv->ident.name, (uint32_t)recv->ident.name_len);
            if (sym) rt = sym->type;
        }
    }
    if (!rt) return true;  /* unknown — don't drop classification */
    Type *eff = type_unwrap_distinct(rt);
    if (!eff) return true;
    /* Pointer to a known target counts (e.g., *Pool, *Slab via param). */
    if (eff->kind == TYPE_POINTER) eff = type_unwrap_distinct(eff->pointer.inner);
    if (!eff) return true;
    switch (eff->kind) {
        case TYPE_POOL:
        case TYPE_SLAB:
        case TYPE_RING:
        case TYPE_ARENA:
        case TYPE_STRUCT:   /* Task.alloc auto-slab sugar */
            return true;
        /* Stage 2 Part B (2026-04-28): exhaustive — every other TYPE_KIND
         * is rejected as a builtin method receiver. Adding a new TYPE_
         * forces a deliberate decision (does it have alloc/free methods?). */
        case TYPE_VOID: case TYPE_BOOL:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64: case TYPE_USIZE:
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
        case TYPE_F32: case TYPE_F64:
        case TYPE_POINTER: case TYPE_OPTIONAL: case TYPE_SLICE:
        case TYPE_ARRAY: case TYPE_ENUM: case TYPE_UNION:
        case TYPE_FUNC_PTR: case TYPE_OPAQUE: case TYPE_BARRIER:
        case TYPE_HANDLE: case TYPE_SEMAPHORE: case TYPE_DISTINCT:
            return false;
    }
    return false;
}

/* Classify a NODE_CALL expression as a builtin method call. Returns
 * IRMC_* kind, or IRMC_NONE if not recognized. The callee must be
 * NODE_FIELD with a method name matching one of the patterns AND the
 * receiver type must be a recognized builtin target. Arg count is
 * also validated where relevant. */
static IRMethodKind ir_classify_method_call_ex(Checker *c, Node *call) {
    if (!call || call->kind != NODE_CALL) return IRMC_NONE;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_FIELD) return IRMC_NONE;
    /* Gap 32: receiver must be a builtin target type. */
    if (!ir_receiver_is_builtin_target(c, callee)) return IRMC_NONE;
    const char *m = callee->field.field_name;
    uint32_t ml = (uint32_t)callee->field.field_name_len;
    /* alloc — Pool/Slab/Task with 0 args (arena.alloc has 1 arg — type) */
    if (ml == 5 && memcmp(m, "alloc", 5) == 0) {
        if (call->call.arg_count == 0) return IRMC_ALLOC;
        return IRMC_ARENA_ALLOC;  /* arena.alloc(Type) takes type arg */
    }
    if (ml == 9 && memcmp(m, "alloc_ptr", 9) == 0) return IRMC_ALLOC_PTR;
    /* ByY6r Gap 1 (2026-06-03): arena.alloc_slice(T, n) returns ?[*]T — must
     * be classified as ARENA alloc so the dest's source_color is ZC_COLOR_ARENA.
     * Without this, arena.reset() doesn't mark the slice FREED → silent UAF. */
    if (ml == 11 && memcmp(m, "alloc_slice", 11) == 0) return IRMC_ARENA_ALLOC;
    if (ml == 3 && memcmp(m, "get", 3) == 0) return IRMC_GET;
    if (ml == 4 && memcmp(m, "free", 4) == 0) return IRMC_FREE;
    if (ml == 8 && memcmp(m, "free_ptr", 8) == 0) return IRMC_FREE_PTR;
    /* Gap 39: arena.reset() / arena.unsafe_reset() invalidates ALL handles
     * allocated from this arena. Both bare-call and defer-wrapped invoke
     * this classification — defer body emits the call at scope exit, but
     * the user's intent is identical. */
    if (ml == 5 && memcmp(m, "reset", 5) == 0) return IRMC_ARENA_RESET;
    if (ml == 12 && memcmp(m, "unsafe_reset", 12) == 0) return IRMC_ARENA_RESET;
    return IRMC_NONE;
}

/* Backward-compat wrapper for callsites that don't have Checker handy.
 * Without checker, receiver-type validation is skipped (current behavior).
 * Prefer ir_classify_method_call_ex(c, call) at new callsites. */
static IRMethodKind ir_classify_method_call(Node *call) {
    return ir_classify_method_call_ex(NULL, call);
}

/* F3.2 (2026-05-04): extract the receiver name (Pool/Slab variable
 * name) from a builtin method call. Returns the source-level identifier
 * for `pool.alloc()` style calls (returns "pool"). Returns {NULL, 0}
 * if the receiver isn't a bare ident (e.g., `*p.alloc()` where p is a
 * pointer param — no source-level name to track).
 *
 * Used to populate IRHandleInfo.pool_name at alloc sites and to compare
 * against the receiver at IRMC_GET / IRMC_FREE sites for wrong-pool
 * detection. */
static void ir_extract_pool_name(Node *call, const char **out_name,
                                 uint32_t *out_len) {
    *out_name = NULL;
    *out_len = 0;
    if (!call || call->kind != NODE_CALL) return;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_FIELD) return;
    Node *recv = callee->field.object;
    if (!recv || recv->kind != NODE_IDENT) return;
    *out_name = recv->ident.name;
    *out_len = (uint32_t)recv->ident.name_len;
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
    if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
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
    /* Stage 2 Part B (2026-04-28): exhaustive — leaf/non-expr kinds.
     * Literals carry no handle reference; CAST/SIZEOF wrap the cast/sized
     * value but those don't propagate UAF state through this walker
     * (existing tests cover those paths via the recursive cases above
     * for inner exprs). Statement / declaration kinds shouldn't appear
     * as expressions; listed here so adding a new NODE_ kind to ast.h
     * forces a deliberate decision. */
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_CAST: case NODE_SIZEOF:
    /* Statement / decl kinds — defensive, shouldn't reach here */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
    case NODE_VAR_DECL: case NODE_BLOCK: case NODE_IF:
    case NODE_FOR: case NODE_WHILE: case NODE_SWITCH:
    case NODE_RETURN: case NODE_BREAK: case NODE_CONTINUE:
    case NODE_DEFER: case NODE_GOTO: case NODE_LABEL:
    case NODE_EXPR_STMT: case NODE_ASM: case NODE_CRITICAL:
    case NODE_ONCE: case NODE_SPAWN: case NODE_YIELD:
    case NODE_AWAIT: case NODE_DO_WHILE: case NODE_STATIC_ASSERT:
        break;
    }
}

/* F3.2 (2026-05-04): wrong-pool walker. Recurses into expressions
 * looking for embedded `pool.get(h)` / `pool.free(h)` calls and flags
 * if h was allocated from a different pool. Mirrors ir_check_expr_uaf's
 * recursion shape so it handles `pool_b.get(h).id` (NODE_FIELD wrapping
 * NODE_CALL), `arr[pool_b.get(h).idx]` (NODE_INDEX wrapping), etc.
 *
 * Reuses UafReportSet — once a wrong-pool error is reported for a root
 * local, suppress further reports for the same local in the same expr.
 *
 * Pattern 1 of F3 limitations.md (wrong pool detection). */
static void ir_check_expr_wrong_pool(ZerCheck *zc, IRFunc *func,
                                     IRPathState *ps, Node *expr,
                                     int line, UafReportSet *rs);

static void ir_check_call_wrong_pool(ZerCheck *zc, IRFunc *func,
                                     IRPathState *ps, Node *call,
                                     int line, UafReportSet *rs) {
    if (!call || call->kind != NODE_CALL) return;
    IRMethodKind mc = ir_classify_method_call_ex(zc->checker, call);
    if (mc != IRMC_GET && mc != IRMC_FREE && mc != IRMC_FREE_PTR) return;
    if (call->call.arg_count < 1) return;
    Node *arg = call->call.args[0];
    int root_local;
    const char *path;
    uint32_t path_len;
    if (ir_extract_compound_key(zc, func, arg,
                                 &root_local, &path, &path_len) != 0) return;
    if (urs_has(rs, root_local)) return;
    IRHandleInfo *h;
    if (path_len == 0) h = ir_find_handle(ps, root_local);
    else h = ir_find_compound_handle(ps, root_local, path, path_len);
    if (!h && path_len > 0) h = ir_find_handle(ps, root_local);
    if (!h || !h->pool_name || h->pool_name_len == 0) return;
    const char *cur_n; uint32_t cur_l;
    ir_extract_pool_name(call, &cur_n, &cur_l);
    if (!cur_n || cur_l == 0) return;
    if (cur_l == h->pool_name_len &&
        memcmp(cur_n, h->pool_name, cur_l) == 0) return;
    const char *verb = (mc == IRMC_GET) ? "used on" : "freed on";
    ir_zc_error(zc, line,
        "wrong pool: handle was allocated from '%.*s' but %s '%.*s'",
        (int)h->pool_name_len, h->pool_name, verb, (int)cur_l, cur_n);
    urs_add(rs, root_local);
}

static void ir_check_expr_wrong_pool(ZerCheck *zc, IRFunc *func,
                                     IRPathState *ps, Node *expr,
                                     int line, UafReportSet *rs) {
    if (!expr) return;
    switch (expr->kind) {
    case NODE_CALL:
        ir_check_call_wrong_pool(zc, func, ps, expr, line, rs);
        for (int i = 0; i < expr->call.arg_count; i++)
            ir_check_expr_wrong_pool(zc, func, ps, expr->call.args[i],
                                     line, rs);
        break;
    case NODE_FIELD:
        ir_check_expr_wrong_pool(zc, func, ps, expr->field.object, line, rs);
        break;
    case NODE_INDEX:
        ir_check_expr_wrong_pool(zc, func, ps, expr->index_expr.object,
                                 line, rs);
        ir_check_expr_wrong_pool(zc, func, ps, expr->index_expr.index,
                                 line, rs);
        break;
    case NODE_UNARY:
        if (expr->unary.op == TOK_AMP) break;
        ir_check_expr_wrong_pool(zc, func, ps, expr->unary.operand, line, rs);
        break;
    case NODE_BINARY:
        ir_check_expr_wrong_pool(zc, func, ps, expr->binary.left, line, rs);
        ir_check_expr_wrong_pool(zc, func, ps, expr->binary.right, line, rs);
        break;
    case NODE_ASSIGN:
        ir_check_expr_wrong_pool(zc, func, ps, expr->assign.target, line, rs);
        ir_check_expr_wrong_pool(zc, func, ps, expr->assign.value, line, rs);
        break;
    case NODE_TYPECAST:
        ir_check_expr_wrong_pool(zc, func, ps, expr->typecast.expr, line, rs);
        break;
    case NODE_SLICE:
        ir_check_expr_wrong_pool(zc, func, ps, expr->slice.object, line, rs);
        ir_check_expr_wrong_pool(zc, func, ps, expr->slice.start, line, rs);
        ir_check_expr_wrong_pool(zc, func, ps, expr->slice.end, line, rs);
        break;
    case NODE_ORELSE:
        ir_check_expr_wrong_pool(zc, func, ps, expr->orelse.expr, line, rs);
        break;
    case NODE_INTRINSIC:
        for (int i = 0; i < expr->intrinsic.arg_count; i++)
            ir_check_expr_wrong_pool(zc, func, ps, expr->intrinsic.args[i],
                                     line, rs);
        break;
    case NODE_STRUCT_INIT:
        for (int i = 0; i < expr->struct_init.field_count; i++)
            ir_check_expr_wrong_pool(zc, func, ps,
                                     expr->struct_init.fields[i].value,
                                     line, rs);
        break;
    /* Leaf / non-expr kinds — exhaustive enumeration mirrors UAF walker. */
    case NODE_IDENT:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_CAST: case NODE_SIZEOF:
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
    case NODE_VAR_DECL: case NODE_BLOCK: case NODE_IF:
    case NODE_FOR: case NODE_WHILE: case NODE_SWITCH:
    case NODE_RETURN: case NODE_BREAK: case NODE_CONTINUE:
    case NODE_DEFER: case NODE_GOTO: case NODE_LABEL:
    case NODE_EXPR_STMT: case NODE_ASM: case NODE_CRITICAL:
    case NODE_ONCE: case NODE_SPAWN: case NODE_YIELD:
    case NODE_AWAIT: case NODE_DO_WHILE: case NODE_STATIC_ASSERT:
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

/* AU-2 (2026-07-01): mark every ALIVE arena-colored handle (and any alias
 * sharing its alloc_id) FREED. Shared by the direct IRMC_ARENA_RESET path AND
 * the defer-body scanner — `defer arena.reset()` must invalidate arena handles
 * the same way a direct `arena.reset()` does. Two-pass (snapshot alloc_ids,
 * then mark) so aliases with ZC_COLOR_UNKNOWN are also caught. */
static void ir_mark_arena_handles_freed(IRPathState *ps, int line) {
    int aid_cap = ps->handle_count > 0 ? ps->handle_count : 1;
    int *aids = (int *)malloc((size_t)aid_cap * sizeof(int));
    if (!aids) return;
    int aid_count = 0;
    for (int hi = 0; hi < ps->handle_count; hi++) {
        IRHandleInfo *h = &ps->handles[hi];
        if (h->source_color == ZC_COLOR_ARENA && h->state == IR_HS_ALIVE)
            aids[aid_count++] = h->alloc_id;
    }
    for (int hi = 0; hi < ps->handle_count; hi++) {
        IRHandleInfo *h = &ps->handles[hi];
        if (h->state != IR_HS_ALIVE) continue;
        for (int ai = 0; ai < aid_count; ai++) {
            if (h->alloc_id == aids[ai]) {
                h->state = IR_HS_FREED;
                h->free_line = line;
                break;
            }
        }
    }
    free(aids);
}

/* AU-2: is an AST statement an `arena.reset()` / `arena.unsafe_reset()` call?
 * (NODE_FIELD callee, method "reset"/"unsafe_reset"). Mirrors ir_defer_free_arg
 * shape. Used by the defer scanner so a deferred reset invalidates arena
 * handles. */
static bool ir_defer_is_arena_reset(Node *node) {
    if (!node || node->kind != NODE_EXPR_STMT || !node->expr_stmt.expr) return false;
    Node *call = node->expr_stmt.expr;
    if (call->kind != NODE_CALL) return false;
    Node *callee = call->call.callee;
    if (!callee || callee->kind != NODE_FIELD) return false;
    const char *m = callee->field.field_name;
    uint32_t ml = (uint32_t)callee->field.field_name_len;
    return (ml == 5 && memcmp(m, "reset", 5) == 0) ||
           (ml == 12 && memcmp(m, "unsafe_reset", 12) == 0);
}

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
                /* Phase E: propagate to aliases sharing alloc_id.
                 * Without this, `Handle h = mh orelse return; defer free(h);`
                 * only marks h FREED, leaving mh (the ?Handle alias with
                 * same alloc_id) as ALIVE at function exit → false leak. */
                ir_propagate_alias_state(ps, h, IR_HS_FREED, defer_line);
            } else if (h && h->state == IR_HS_FREED &&
                       h->free_line != defer_line &&
                       !h->defer_double_reported) {
                /* Control-flow oracle CF_DEFER_DOUBLE (2026-06-07): the handle
                 * was already freed by something OTHER than this defer (an
                 * explicit body free, or a different defer), and this deferred
                 * free will free it AGAIN at scope exit = double free.
                 *
                 * The `free_line != defer_line` guard distinguishes a REAL
                 * double free from the legitimate `defer { if (e) { free(h); }
                 * else { free(h); } }` pattern: the recursive scan walks BOTH
                 * mutually-exclusive branches linearly, so the second branch
                 * sees h already FREED — but both frees carry THIS defer's line
                 * (set at the ALIVE->FREED mark below), so free_line==defer_line
                 * and we correctly skip. A genuine double free comes from an
                 * explicit free (different source line) or another defer
                 * (different defer line), so free_line!=defer_line.
                 *
                 * No ordering comparison: a defer registered AFTER an explicit
                 * free still fires at scope exit (`free(h); defer free(h);` IS a
                 * double free), so guarding on order would be a false NEGATIVE.
                 * The only cost is over-rejecting the rare `if (c) { free(h);
                 * return; } defer free(h);` ordering — acceptable per the
                 * soundness criterion; under-rejection is the hole this closes. */
                ir_zc_error(zc, defer_line,
                    "double free: deferred free of %%%d which was already "
                    "freed at line %d",
                    root_local, h->free_line);
                h->defer_double_reported = 1;
            }
        }
    }

    /* AU-2 (2026-07-01): a deferred arena.reset()/unsafe_reset() invalidates
     * every arena-colored handle, exactly like a direct reset. Without this a
     * `defer arena.reset(); defer use(p);` (or just leak detection on an
     * arena handle freed only via deferred reset) was blind. */
    if (ir_defer_is_arena_reset(body))
        ir_mark_arena_handles_freed(ps, defer_line);

    /* Recurse into block AND nested control-flow bodies (BUG-608).
     * Conservative: any reachable free inside defer marks handle FREED.
     * Misses some conditional-free double-detect but prevents false
     * leak on `defer { if (err) { free(h); } else { free(h); } }`. */
    switch (body->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < body->block.stmt_count; i++)
            ir_defer_scan_frees(zc, func, ps, body->block.stmts[i], defer_line);
        break;
    case NODE_IF:
        ir_defer_scan_frees(zc, func, ps, body->if_stmt.then_body, defer_line);
        ir_defer_scan_frees(zc, func, ps, body->if_stmt.else_body, defer_line);
        break;
    case NODE_FOR:
        ir_defer_scan_frees(zc, func, ps, body->for_stmt.body, defer_line);
        break;
    case NODE_WHILE: case NODE_DO_WHILE:
        ir_defer_scan_frees(zc, func, ps, body->while_stmt.body, defer_line);
        break;
    case NODE_SWITCH:
        for (int i = 0; i < body->switch_stmt.arm_count; i++)
            ir_defer_scan_frees(zc, func, ps, body->switch_stmt.arms[i].body, defer_line);
        break;
    case NODE_CRITICAL:
        ir_defer_scan_frees(zc, func, ps, body->critical.body, defer_line);
        break;
    case NODE_ONCE:
        ir_defer_scan_frees(zc, func, ps, body->once.body, defer_line);
        break;
    /* Stage 2 Part B (2026-04-28): exhaustive — leaf/non-control-flow
     * kinds have no scannable body for free detection. The defer scanner
     * only descends into block-shaped statements. */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
    case NODE_VAR_DECL: case NODE_RETURN: case NODE_BREAK:
    case NODE_CONTINUE: case NODE_DEFER: case NODE_GOTO:
    case NODE_LABEL: case NODE_EXPR_STMT: case NODE_ASM:
    case NODE_SPAWN: case NODE_YIELD: case NODE_AWAIT:
    case NODE_STATIC_ASSERT:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_IDENT: case NODE_BINARY: case NODE_UNARY:
    case NODE_ASSIGN: case NODE_CALL: case NODE_FIELD:
    case NODE_INDEX: case NODE_SLICE: case NODE_ORELSE:
    case NODE_INTRINSIC: case NODE_CAST: case NODE_TYPECAST:
    case NODE_SIZEOF: case NODE_STRUCT_INIT:
        break;
    }
}

/* plt86m audit 2026-06-17: walk a defer body and check non-free USES against
 * the supplied exit path state `ps`. A USE of an entity that is FREED /
 * move-TRANSFERRED at scope exit is a use-after-free / use-after-move — the
 * defer fires AFTER the body's frees/moves, but the deferred statement is
 * lifted out of the linear IR stream, so the ordinary use-checker
 * (ir_check_expr_uaf during the fixpoint) never sees it. We route it through
 * the SAME checker here. A free(x)/free_ptr(x) statement's argument is NOT a
 * use (freeing in a defer is the normal cleanup pattern — handled by
 * ir_defer_scan_frees), so skip those. `rs` dedups reports per root-local
 * across every return block + defer in the function. */
static void ir_defer_scan_uses(ZerCheck *zc, IRFunc *func, IRPathState *ps,
                                Node *body, int defer_line, UafReportSet *rs) {
    if (!body) return;

    if (body->kind == NODE_EXPR_STMT && body->expr_stmt.expr &&
        ir_defer_free_arg(body) == NULL) {
        ir_check_expr_uaf(zc, func, ps, body->expr_stmt.expr, defer_line, rs);
    }

    switch (body->kind) {
    case NODE_BLOCK:
        for (int i = 0; i < body->block.stmt_count; i++)
            ir_defer_scan_uses(zc, func, ps, body->block.stmts[i], defer_line, rs);
        break;
    case NODE_IF:
        ir_defer_scan_uses(zc, func, ps, body->if_stmt.then_body, defer_line, rs);
        ir_defer_scan_uses(zc, func, ps, body->if_stmt.else_body, defer_line, rs);
        break;
    case NODE_FOR:
        ir_defer_scan_uses(zc, func, ps, body->for_stmt.body, defer_line, rs);
        break;
    case NODE_WHILE: case NODE_DO_WHILE:
        ir_defer_scan_uses(zc, func, ps, body->while_stmt.body, defer_line, rs);
        break;
    case NODE_SWITCH:
        for (int i = 0; i < body->switch_stmt.arm_count; i++)
            ir_defer_scan_uses(zc, func, ps, body->switch_stmt.arms[i].body, defer_line, rs);
        break;
    case NODE_CRITICAL:
        ir_defer_scan_uses(zc, func, ps, body->critical.body, defer_line, rs);
        break;
    case NODE_ONCE:
        ir_defer_scan_uses(zc, func, ps, body->once.body, defer_line, rs);
        break;
    /* exhaustive (no default:) per the -Wswitch walker rule — leaf/non-
     * control-flow kinds carry no scannable sub-body. */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL:
    case NODE_ENUM_DECL: case NODE_UNION_DECL: case NODE_TYPEDEF:
    case NODE_IMPORT: case NODE_CINCLUDE: case NODE_INTERRUPT:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
    case NODE_VAR_DECL: case NODE_RETURN: case NODE_BREAK:
    case NODE_CONTINUE: case NODE_DEFER: case NODE_GOTO:
    case NODE_LABEL: case NODE_EXPR_STMT: case NODE_ASM:
    case NODE_SPAWN: case NODE_YIELD: case NODE_AWAIT:
    case NODE_STATIC_ASSERT:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STRING_LIT:
    case NODE_CHAR_LIT: case NODE_BOOL_LIT: case NODE_NULL_LIT:
    case NODE_IDENT: case NODE_BINARY: case NODE_UNARY:
    case NODE_ASSIGN: case NODE_CALL: case NODE_FIELD:
    case NODE_INDEX: case NODE_SLICE: case NODE_ORELSE:
    case NODE_INTRINSIC: case NODE_CAST: case NODE_TYPECAST:
    case NODE_SIZEOF: case NODE_STRUCT_INIT:
        break;
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

/* GAP-4 (BUG-740, 2026-06-10, 6u360k audit): is this call INDIRECT
 * (through a function pointer) rather than a direct function call?
 * Direct calls get the callee's FuncSummary frees_param propagation;
 * an indirect callee is unknown, so the argument-precise barrier
 * (ir_indirect_call_barrier) applies instead. Recognized shapes:
 *   - callee ident naming a funcptr-typed function LOCAL (2A or 2C)
 *   - callee ident naming a GLOBAL funcptr VARIABLE (is_function
 *     false — real functions and extern decls have is_function true,
 *     so C-interop direct calls are NOT treated as indirect)
 *   - callee NODE_FIELD whose type is a funcptr (struct Ops vtables);
 *     builtin Pool/Slab/Ring/Arena methods never reach the barrier
 *     call sites (classified and dispatched before it). */
static bool ir_call_is_indirect(ZerCheck *zc, IRFunc *func, Node *call) {
    if (!call || call->kind != NODE_CALL || !call->call.callee) return false;
    Node *callee = call->call.callee;
    if (callee->kind == NODE_IDENT) {
        int l = ir_find_local_exact_first(func, callee->ident.name,
                                          (uint32_t)callee->ident.name_len);
        if (l >= 0 && l < func->local_count) {
            return type_dispatch_kind(func->locals[l].type) == TYPE_FUNC_PTR;
        }
        Symbol *s = scope_lookup(zc->checker->global_scope, callee->ident.name,
                                 (uint32_t)callee->ident.name_len);
        if (s && !s->is_function) {
            return type_dispatch_kind(s->type) == TYPE_FUNC_PTR;
        }
        return false;
    }
    if (callee->kind == NODE_FIELD) {
        Type *ft = checker_get_type(zc->checker, callee);
        return ft && type_dispatch_kind(ft) == TYPE_FUNC_PTR;
    }
    /* 8ezecl (copied): array/slice-indexed funcptr callee (`cbs[0](h)`,
     * `vt.cbs[i](h)`). The argument-precise barrier (BUG-740) must fire for any
     * funcptr callee — NODE_INDEX of a funcptr array is the direct sibling of
     * NODE_FIELD on a struct funcptr field; without this the double-free / UAF
     * across an array-indexed callback was missed. */
    if (callee->kind == NODE_INDEX) {
        Type *ft = checker_get_type(zc->checker, callee);
        return ft && type_dispatch_kind(ft) == TYPE_FUNC_PTR;
    }
    return false;
}

/* GAP-4 (BUG-740): argument-precise indirect-call barrier.
 *
 * Principle: anything HANDED to an unknown callee may have been freed
 * by it — and only what was handed. Each tracked handle passed as an
 * argument (bare, compound `b.h`, `&h`, or a struct root carrying
 * compound entries) widens ALIVE → MAYBE_FREED with escaped=true
 * (the callee may now own it), and the widening propagates to the
 * whole alloc_id alias group. Handles NOT passed are untouched.
 *
 * Resulting behavior at the caller:
 *   free after fp(h)  → "maybe freed" double-free error (the GAP-4 bug)
 *   use after fp(h)   → use-after-conditional-free error
 *   silence after     → clean (ownership handed to the callee)
 *   h never passed    → untouched (no noise)
 * The idiomatic restructure is to pass DATA (pool.get(h).field), not
 * the handle, when the caller keeps ownership. Mirrors the existing
 * conservative-proxy stance of call_has_nonkeep_derived_arg. */
static void ir_indirect_call_barrier(ZerCheck *zc, IRFunc *func,
                                     IRPathState *ps, Node *call, int line) {
    for (int ai = 0; ai < call->call.arg_count; ai++) {
        Node *arg = call->call.args[ai];
        if (!arg) continue;
        if (arg->kind == NODE_UNARY && arg->unary.op == TOK_AMP)
            arg = arg->unary.operand;
        int root_local;
        const char *path;
        uint32_t path_len;
        if (ir_extract_compound_key(zc, func, arg,
                                    &root_local, &path, &path_len) != 0)
            continue;
        /* Widen matching entries: exact compound for `b.h` args; for a
         * bare root, every entry on that root (the bare handle AND any
         * compound fields — a by-value struct copy carries them all). */
        for (int hi = 0; hi < ps->handle_count; hi++) {
            IRHandleInfo *h = &ps->handles[hi];
            if (h->local_id != root_local) continue;
            if (path_len > 0) {
                if (h->path_len != path_len) continue;
                if (!h->path || memcmp(h->path, path, path_len) != 0) continue;
            }
            if (h->state != IR_HS_ALIVE) continue;
            h->state = IR_HS_MAYBE_FREED;
            h->free_line = line;
            h->escaped = true;
            /* Group propagation — aliases share the allocation. Also
             * escape them: ownership of the ALLOCATION was handed off,
             * so an untouched alias must not flag as a leak. */
            int aid = h->alloc_id;
            if (aid != 0) {
                for (int gi = 0; gi < ps->handle_count; gi++) {
                    IRHandleInfo *g = &ps->handles[gi];
                    if (g == h || g->alloc_id != aid) continue;
                    if (ir_is_invalid(g)) continue;
                    g->state = IR_HS_MAYBE_FREED;
                    g->free_line = line;
                    g->escaped = true;
                }
            }
        }
    }
}

/* BUG-742 (2026-06-10) call-window rule: a call to code that may READ
 * globals — any ZER-defined callee (has a FuncSummary) or any indirect
 * callee — while a global pseudo-root entry is definitely FREED may
 * observe the dangle. Together with the exit rule (return while a
 * global is FREED), this makes a dangling global UNOBSERVABLE at every
 * boundary per-function analysis cannot see across — closing the
 * cross-function global UAF class without any summary plumbing.
 * Builtin Pool/Slab/Ring/Arena methods and bodyless externs are
 * excluded: builtins cannot read user globals; externs are outside the
 * safety boundary (cinclude territory). The fix the rule teaches is
 * one line: reset the global ('g = null;') right after the free. */
static void ir_check_dangling_globals_at_call(ZerCheck *zc, IRPathState *ps,
                                              int line) {
    for (int i = 0; i < ps->handle_count; i++) {
        IRHandleInfo *h = &ps->handles[i];
        if (h->local_id != IR_GLOBAL_ROOT_ID) continue;
        if (h->state != IR_HS_FREED) continue;
        if (h->path_len == 0) continue;
        ir_zc_error(zc, line,
            "call may observe dangling global '%.*s' (target freed at "
            "line %d) — reset it ('%.*s = null;') before making calls",
            (int)h->path_len, h->path, h->free_line,
            (int)h->path_len, h->path);
    }
}

/* BUG-742 helper: does a FuncSummary exist for this callee name?
 * Proxy for "ZER-defined function" — bodyless externs never get one. */
static bool ir_callee_has_summary(ZerCheck *zc, const char *name,
                                  uint32_t name_len) {
    if (!name || name_len == 0) return false;
    for (int si = 0; si < zc->summary_count; si++) {
        if (zc->summaries[si].func_name_len == name_len &&
            memcmp(zc->summaries[si].func_name, name, name_len) == 0)
            return true;
    }
    return false;
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

    /* Phase E: IR_NOP wrapping NODE_SPAWN or NODE_ASM. Per emitter.c,
     * spawn and asm emit IR_NOP with inst->expr = the AST node
     * (passthrough path). Reroute to per-kind logic. */
    case IR_NOP: {
        if (!inst->expr) break;

        /* D-Alpha-7.5 Session E3: Z1 (Handle UAF through asm) + Z2
         * (move struct → HS_TRANSFERRED through asm). Forward-compat
         * today: asm is naked-only restricted, naked excludes Pool/Slab
         * allocations (V4 audit rule), so Handle/move-struct operands
         * are unreachable. Activates when S1 relaxes alongside Z6/Z9/
         * Z10/Z13. The check is correct, just dormant.
         *
         * SAFETY: Z1 reuses #7 Handle States (operationally proven in
         * lambda_zer_handle subset). Z2 reuses #10 Move Tracking
         * (operationally proven in lambda_zer_move subset). */
        if (inst->expr->kind == NODE_ASM &&
            inst->expr->asm_stmt.is_structured) {
            Node *asm_node = inst->expr;

            /* Z1: each input operand that resolves to a Handle local
             * must be ALIVE. Walk through NODE_FIELD (e.g., h.index) or
             * direct NODE_IDENT to find the root local. */
            for (int i = 0; i < asm_node->asm_stmt.input_count; i++) {
                AsmOperand *op = &asm_node->asm_stmt.inputs[i];
                if (!op->expr) continue;
                Node *root = op->expr;
                while (root) {
                    if (root->kind == NODE_FIELD) root = root->field.object;
                    else if (root->kind == NODE_INDEX) root = root->index_expr.object;
                    else if (root->kind == NODE_UNARY && root->unary.op == TOK_AMP)
                        root = root->unary.operand;
                    else break;
                }
                if (!root || root->kind != NODE_IDENT) continue;
                int op_local = ir_find_local_exact_first(func,
                    root->ident.name, (uint32_t)root->ident.name_len);
                if (op_local < 0) continue;
                IRHandleInfo *h = ir_find_handle(ps, op_local);
                if (!h) continue;
                /* Z1: invalid handle (FREED/MAYBE_FREED/TRANSFERRED)
                 * used as asm operand → UAF / use-after-move. */
                if (ir_is_invalid(h)) {
                    ir_zc_error(zc, inst->source_line,
                        "asm input '%.*s' uses %s handle %%%d "
                        "(Z1 rule — handle must be ALIVE at asm operand)",
                        (int)op->reg_name_len, op->reg_name,
                        ir_state_name(h->state), op_local);
                }
                /* Z2: if local's type is a move struct, asm consumes
                 * the value — mark TRANSFERRED so subsequent uses
                 * trigger the existing use-after-move check. */
                if (op_local >= 0 && op_local < func->local_count) {
                    Type *lt = (Type *)func->locals[op_local].type;
                    if (ir_should_track_move(lt) && h->state == IR_HS_ALIVE) {
                        h->state = IR_HS_TRANSFERRED;
                        h->free_line = inst->source_line;
                    }
                }
            }

            /* Output operands: asm writes opaque value. Existing checker.c
             * Z3 (VRP invalidation) + Z4 (provenance clearing) cover the
             * AST-level state; IR-level handle state is unaffected
             * (asm output isn't an alloc / free / transfer event). */
            break;
        }

        if (inst->expr->kind != NODE_SPAWN) break;
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

        /* Transfer args (ownership to spawned thread).
         *
         * F0.6 (2026-05-03): for move-struct args, auto-register as
         * ALIVE before marking TRANSFERRED. Move struct locals aren't
         * tracked until they're transferred — without auto-registration,
         * spawn(move_struct) was a no-op and use-after-thread-transfer
         * went undetected (B02_use_after_thread_transfer_bad).
         *
         * AUDIT 2026-06-06 (GAP-C): pre-fix only matched NODE_IDENT, so
         * `spawn worker(b.t)` (NODE_FIELD) and `spawn worker(arr[i])`
         * silently skipped TRANSFERRED, producing silent UAM. Use
         * ir_extract_compound_key to resolve both shapes uniformly,
         * mirroring the FIELD_WRITE move-transfer path (line 3119+) and
         * the IR_CALL extern_free path. */
        for (int i = 0; i < sp->spawn_stmt.arg_count; i++) {
            Node *arg = sp->spawn_stmt.args[i];
            if (!arg) continue;
            int root_local;
            const char *path;
            uint32_t path_len;
            if (ir_extract_compound_key(zc, func, arg,
                                         &root_local, &path, &path_len) != 0)
                continue;
            IRHandleInfo *h = (path_len == 0)
                ? ir_find_handle(ps, root_local)
                : ir_find_compound_handle(ps, root_local, path, path_len);
            /* BUG-733 (verified from branch InoCW, 2026-06-09): use-after-move
             * on a spawn argument. The transfer below just overwrote an
             * already-TRANSFERRED state, silently accepting `spawn w(t);
             * spawn w(t)`, `spawn w(b.t); spawn w(b.t)`, and loop re-spawn.
             * Mirror the IR_CALL / IR_COPY move check: flag BEFORE re-transfer.
             * Placed before auto-register so a freshly-ALIVE handle from
             * auto-register below is never flagged. */
            if (h && h->state == IR_HS_TRANSFERRED &&
                root_local < func->local_count) {
                if (path_len == 0)
                    ir_zc_error(zc, inst->source_line,
                        "use after move: '%.*s' ownership transferred at line %d",
                        (int)func->locals[root_local].name_len,
                        func->locals[root_local].name, h->free_line);
                else
                    ir_zc_error(zc, inst->source_line,
                        "use after move: compound '%.*s' on local '%.*s' "
                        "transferred at line %d",
                        (int)path_len, path,
                        (int)func->locals[root_local].name_len,
                        func->locals[root_local].name, h->free_line);
            }
            /* AUDIT 2026-06-12: also reject spawn arg that's already FREED /
             * MAYBE_FREED. Pre-fix the TRANSFERRED overwrite at line 1897
             * silently masked a real use-after-free — `*T t = alloc_ptr();
             * free_ptr(t); spawn worker(t);` compiled clean and the spawned
             * thread read dangling slab memory. Mirror the TRANSFERRED check
             * shape; the same `urs_has` dedup isn't needed since spawn-arg is
             * a single use site per loop iteration. */
            if (h && (h->state == IR_HS_FREED ||
                      h->state == IR_HS_MAYBE_FREED) &&
                root_local < func->local_count) {
                if (path_len == 0)
                    ir_zc_error(zc, inst->source_line,
                        "use after free: '%.*s' is %s (freed at line %d) — "
                        "spawned thread would read dangling memory",
                        (int)func->locals[root_local].name_len,
                        func->locals[root_local].name,
                        ir_state_name(h->state), h->free_line);
                else
                    ir_zc_error(zc, inst->source_line,
                        "use after free: compound '%.*s' on local '%.*s' is %s "
                        "(freed at line %d) — spawned thread would read "
                        "dangling memory",
                        (int)path_len, path,
                        (int)func->locals[root_local].name_len,
                        func->locals[root_local].name,
                        ir_state_name(h->state), h->free_line);
            }
            if (!h && root_local < func->local_count) {
                /* Auto-register move-struct args (bare or compound) so
                 * TRANSFERRED can be observed. For compound paths, walk
                 * the root local type and verify the targeted field is
                 * itself a move struct — auto-registering arbitrary
                 * compounds would over-warn. */
                Type *lt = (Type *)func->locals[root_local].type;
                bool track = false;
                if (path_len == 0) {
                    track = ir_should_track_move(lt);
                } else {
                    /* Best-effort: track if either the root or any of the
                     * path's effective field types is a move struct. The
                     * compound-key string starts with '.' for fields. */
                    track = ir_should_track_move(lt);
                    if (!track) {
                        /* Walk the path one field at a time. */
                        Type *cur = lt;
                        uint32_t pi2 = 0;
                        while (cur && pi2 < path_len) {
                            if (path[pi2] != '.') break;
                            pi2++;
                            /* Read field name until next '.' or end */
                            uint32_t fs = pi2;
                            while (pi2 < path_len && path[pi2] != '.') pi2++;
                            Type *cur_eff = type_unwrap_distinct(cur);
                            if (!cur_eff || cur_eff->kind != TYPE_STRUCT) break;
                            Type *next = NULL;
                            for (uint32_t fi = 0;
                                 fi < cur_eff->struct_type.field_count; fi++) {
                                SField *sf = &cur_eff->struct_type.fields[fi];
                                if (sf->name_len == (pi2 - fs) &&
                                    memcmp(sf->name, path + fs, pi2 - fs) == 0) {
                                    next = sf->type;
                                    break;
                                }
                            }
                            if (!next) break;
                            cur = next;
                        }
                        if (cur && ir_should_track_move(cur)) track = true;
                    }
                }
                if (track) {
                    if (path_len == 0)
                        h = ir_add_handle(ps, root_local);
                    else
                        h = ir_add_compound_handle(ps, root_local, path, path_len);
                    if (h) {
                        h->state = IR_HS_ALIVE;
                        h->alloc_line = inst->source_line;
                        if (h->alloc_id == 0) h->alloc_id = _ir_next_alloc_id++;
                    }
                }
            }
            if (h) {
                h->state = IR_HS_TRANSFERRED;
                h->free_line = inst->source_line;
            }
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
                    /* bh18_1b (2026-07-01): propagate TRANSFERRED to pointer
                     * aliases of the moved-from local — `*T p = &a;` taken
                     * before `T b = a;` shares a's alloc_id, so a later use
                     * through p is use-after-move. Mirrors the free path. */
                    ir_propagate_alias_state(ps, src_h, IR_HS_TRANSFERRED,
                                             inst->source_line);
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
        /* Alias: dest inherits source's alloc_id and state.
         *
         * UAF GUARD (audit 2026-04-26): src_h points into ps->handles which
         * can be realloc'd by ir_add_handle below — using src_h after the
         * add is a heap-use-after-free. Snapshot fields BEFORE the
         * realloc-capable add.
         *
         * audit 2026-06-04: unified via ir_apply_alias to prevent
         * field-drift bug class. */
        IRAliasSnapshot snap;
        ir_snapshot_alias(&snap, src_h);
        /* BUG-734 (verified from branch InoCW, 2026-06-09): handle-overwrite-
         * while-alive leak was missing from IR_COPY. The orelse desugaring
         * routes every `h = pool.alloc() orelse ...` through a temp + IR_COPY
         * into the user-named local, so the alloc-site "overwritten while
         * alive" check (which gates on !is_temp) only ever saw the temp.
         * Result, silently accepted: `Handle h = gp.alloc() orelse return;
         * h = gp.alloc() orelse return;` — first alloc leaked, no error.
         * Mirror the alloc-site check: if dst was ALIVE with a DIFFERENT
         * alloc_id, the overwrite drops the previous allocation. Read prev
         * dst fields BEFORE the realloc-capable ir_add_handle below. */
        {
            IRHandleInfo *prev_dst = ir_find_handle(ps, inst->dest_local);
            if (prev_dst && prev_dst->state == IR_HS_ALIVE &&
                !prev_dst->escaped &&
                inst->dest_local < func->local_count &&
                !func->locals[inst->dest_local].is_temp &&
                prev_dst->alloc_id != snap.alloc_id) {
                ir_zc_error(zc, inst->source_line,
                    "handle %%%d overwritten while alive — previous allocation leaked",
                    inst->dest_local);
            }
        }
        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
        if (dst_h) {
            ir_apply_alias(dst_h, &snap);
            dst_h->state = snap.state;
        }
        break;
    }

    /* Phase F: IR_CAST (C-style pointer cast `(T*)ptr`). If source has
     * a tracked handle, dest becomes an alias sharing alloc_id.
     * Mirrors @ptrcast alias tracking. Enables param-color inference
     * for wrapper functions like `*T unwrap(*opaque raw) { return (T*)raw; }`.
     *
     * If src is a param without a handle, auto-register it (as escaped
     * so it doesn't flag as leak in the wrapper's own leak check) so
     * param-color inference can match alloc_ids at summary building. */
    case IR_CAST: {
        if (inst->dest_local < 0 || inst->src1_local < 0) break;
        /* Only propagate for pointer casts — check src local's type */
        if (inst->src1_local < func->local_count) {
            Type *src_type = func->locals[inst->src1_local].type;
            Type *eff = src_type ? type_unwrap_distinct(src_type) : NULL;
            bool src_is_ptr = eff && (eff->kind == TYPE_POINTER ||
                                       eff->kind == TYPE_OPAQUE);
            if (eff && eff->kind == TYPE_OPTIONAL) {
                Type *inner = type_unwrap_distinct(eff->optional.inner);
                if (inner && (inner->kind == TYPE_POINTER ||
                              inner->kind == TYPE_OPAQUE))
                    src_is_ptr = true;
            }
            if (!src_is_ptr) break;
        }
        IRHandleInfo *src_h = ir_find_handle(ps, inst->src1_local);
        if (!src_h && inst->src1_local < func->local_count &&
            func->locals[inst->src1_local].is_param) {
            src_h = ir_add_handle(ps, inst->src1_local);
            if (src_h) {
                src_h->state = IR_HS_ALIVE;
                src_h->alloc_line = inst->source_line;
                src_h->alloc_id = _ir_next_alloc_id++;
                src_h->source_color = ZC_COLOR_UNKNOWN;
                src_h->escaped = true;
            }
        }
        if (!src_h) break;
        if (ir_is_invalid(src_h)) {
            ir_zc_error(zc, inst->source_line,
                "use of %s handle %%%d in cast",
                ir_state_name(src_h->state), inst->src1_local);
        }
        /* UAF GUARD + alias-copy unification (audit 2026-06-04):
         * snapshot fields before realloc-capable add. Pre-fix this site
         * was missing pool_name+len and is_thread_handle, allowing wrong-
         * pool checks to be bypassed via C-style pointer cast. */
        IRAliasSnapshot snap;
        ir_snapshot_alias(&snap, src_h);
        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
        if (dst_h) {
            ir_apply_alias(dst_h, &snap);
            dst_h->state = snap.state;
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
                if (ir_use_guard_disjoint(zc, h)) {
                    /* free under a guard disjoint from the prior free — no
                     * double-free. If it is the exact complement, the handle is
                     * now freed on ALL paths (clears the leak check). */
                    if (ir_free_completes_coverage(zc, h)) h->freed_all_paths = 1;
                    /* audit 2026-07-06: this handle now has TWO free sites under
                     * distinct guards. free_block still points at the first; the
                     * flag disables further disjoint relaxation so a use/free
                     * matching the SECOND free can no longer slip through (was a
                     * UAF/double-free). */
                    ir_mark_multi_freed(ps, h);
                } else {
                    ir_zc_error(zc, inst->source_line,
                        "freeing %%%d which may already be freed",
                        target);
                }
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
            if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
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
            UafReportSet pool_rs = {0};
            ir_check_expr_wrong_pool(zc, func, ps, inst->expr,
                                     inst->source_line, &pool_rs);
            free(pool_rs.ids);
            free(rs.ids);
        }
        break;
    }

    /* BUG-765 (2026-06-22): unary reads — `*p` (pointer deref) lowers to
     * IR_UNOP carrying the NODE_UNARY on inst->expr (the dormant
     * IR_DEREF_READ is never emitted). Dereferencing an INTERIOR pointer
     * after its parent is freed (`*u32 p = &b.a; free(b); *p`) is a UAF,
     * but IR_UNOP was a no-op here — only the INDEX form (`p[0]`,
     * IR_INDEX_READ above) was checked, leaving the deref form a silent
     * hole. Run the same UAF walker. ir_check_expr_uaf skips `&`
     * (TOK_AMP = capture, not read) and reports nothing for non-handle
     * operands, so this is also safe for `-x` / `!x` / `~x`. Sub-exprs are
     * decomposed into their own ops, so `*p` is always its own IR_UNOP —
     * no overlap with the IR_ASSIGN/IR_INDEX_READ walkers. */
    case IR_UNOP: {
        if (inst->expr) {
            UafReportSet rs = {0};
            ir_check_expr_uaf(zc, func, ps, inst->expr, inst->source_line, &rs);
            UafReportSet pool_rs = {0};
            ir_check_expr_wrong_pool(zc, func, ps, inst->expr,
                                     inst->source_line, &pool_rs);
            free(pool_rs.ids);
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
                    if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
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
        /* Gap A3 (2026-05-06, sNsjM): move-struct field-read transfers ownership.
         * `Tok t = b.inner` where b.inner is a move-struct field MUST mark the
         * source compound (b, ".inner") as TRANSFERRED. Pre-fix: only
         * NODE_INDEX (array-element move) was handled, field-read move-transfer
         * was silently dropped → `Tok t = b.inner; use(&b.inner)` compiled
         * clean → silent UAM. */
        if (inst->dest_local >= 0 &&
            inst->dest_local < func->local_count) {
            Type *dest_type = func->locals[inst->dest_local].type;
            if (ir_should_track_move(dest_type) && inst->expr) {
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, inst->expr,
                                             &root_local, &path,
                                             &path_len) == 0 &&
                    path_len > 0) {
                    IRHandleInfo *ch = ir_find_compound_handle(ps,
                        root_local, path, path_len);
                    if (ch && ch->state == IR_HS_TRANSFERRED) {
                        ir_zc_error(zc, inst->source_line,
                            "use after move: '%.*s' on local %%%d "
                            "ownership transferred at line %d",
                            (int)path_len, path, root_local,
                            ch->free_line);
                    }
                    if (!ch) ch = ir_add_compound_handle(ps, root_local,
                                                          path, path_len);
                    if (ch) {
                        ch->state = IR_HS_TRANSFERRED;
                        ch->free_line = inst->source_line;
                    }
                }
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
            UafReportSet pool_rs = {0};
            ir_check_expr_wrong_pool(zc, func, ps, inst->expr,
                                     inst->source_line, &pool_rs);
            free(pool_rs.ids);
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
            /* Compound key registration: `container.field = h`
             *
             * Pre-fix (audit 2026-06-04): only copied state/alloc_line/
             * alloc_id/source_color — missing pool_name (wrong-pool bypass
             * via struct field), escaped, is_thread_handle. Also had latent
             * UAF: rh read after realloc-capable ir_add_compound_handle.
             * Both fixed via unified ir_apply_alias snapshot pattern. */
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
                        IRAliasSnapshot snap;
                        ir_snapshot_alias(&snap, rh);
                        IRHandleInfo *ch = ir_add_compound_handle(ps,
                            root_local, path, path_len);
                        if (ch) {
                            ir_apply_alias(ch, &snap);
                            ch->state = IR_HS_ALIVE;
                        }
                    }
                }
            }

            /* GAP-3 (BUG-739): bare global ident store — `g_ptr = p`.
             * Register/overwrite the global's pseudo-root entry sharing
             * p's alloc_id, so a later free reaches it via
             * ir_propagate_alias_state and read-backs alias from it.
             * The ident name string points into the AST — outlives this
             * analysis (same lifetime argument as pool_name). */
            if (target_expr && target_expr->kind == NODE_IDENT &&
                ir_ident_is_unshadowed_global(zc, func, target_expr)) {
                IRHandleInfo *grh = (rhs_local >= 0)
                    ? ir_find_handle(ps, rhs_local) : NULL;
                if (grh && grh->state == IR_HS_ALIVE && grh->alloc_id != 0) {
                    IRAliasSnapshot gsnap;
                    ir_snapshot_alias(&gsnap, grh);
                    IRHandleInfo *gh = ir_add_compound_handle(ps,
                        IR_GLOBAL_ROOT_ID, target_expr->ident.name,
                        (uint32_t)target_expr->ident.name_len);
                    if (gh) {
                        ir_apply_alias(gh, &gsnap);
                        gh->state = IR_HS_ALIVE;
                        gh->escaped = true; /* INVARIANT — see IR_GLOBAL_ROOT_ID */
                    }
                } else {
                    /* Non-tracked value (null reset, param, unknown):
                     * clear any stale binding so `g = p; free(p);
                     * g = null; read g` doesn't false-positive. */
                    IRHandleInfo *gh = ir_find_compound_handle(ps,
                        IR_GLOBAL_ROOT_ID, target_expr->ident.name,
                        (uint32_t)target_expr->ident.name_len);
                    if (gh) {
                        gh->state = IR_HS_UNKNOWN;
                        gh->alloc_id = 0;
                    }
                }
            }

            /* Gap A2 (2026-05-06, sNsjM): move-struct field-write transfers
             * ownership. `b.field = t` where t is a move struct (or contains
             * move fields) MUST mark t as TRANSFERRED. Subsequent use of t =
             * use-after-move. Hosted + baremetal: silent UAM since C struct
             * copy makes t's bytes readable but logically invalid. */
            if (target_expr && rhs_local >= 0 && rhs_local < func->local_count &&
                (target_expr->kind == NODE_FIELD ||
                 target_expr->kind == NODE_INDEX)) {
                Type *rhs_type = func->locals[rhs_local].type;
                if (ir_should_track_move(rhs_type)) {
                    IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
                    if (rh && ir_is_invalid(rh)) {
                        ir_zc_error(zc, inst->source_line,
                            "use of %s value (local %%%d) in field/index write",
                            ir_state_name(rh->state), rhs_local);
                    }
                    if (!rh) rh = ir_add_handle(ps, rhs_local);
                    if (rh) {
                        rh->state = IR_HS_TRANSFERRED;
                        rh->free_line = inst->source_line;
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
             * destination to source, mirroring the bare-ident path below.
             *
             * audit 2026-06-04: pre-fix was missing `escaped`. Also had a
             * latent UAF — only s_pool/s_pool_len were snapshotted; the
             * other src_h-> reads after ir_add_handle could fault on
             * realloc. Unified via ir_apply_alias. */
            if (inst->expr->kind == NODE_ORELSE && rhs && rhs->kind == NODE_IDENT) {
                int src_local = ir_find_local_exact_first(func,
                    rhs->ident.name, (uint32_t)rhs->ident.name_len);
                if (src_local >= 0) {
                    IRHandleInfo *src_h = ir_find_handle(ps, src_local);
                    if (src_h) {
                        IRAliasSnapshot snap;
                        ir_snapshot_alias(&snap, src_h);
                        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                        if (dst_h) {
                            ir_apply_alias(dst_h, &snap);
                            dst_h->state = snap.state;
                        }
                    }
                } else if (ir_ident_is_unshadowed_global(zc, func, rhs)) {
                    /* GAP-3 (BUG-739): `gp = g_ptr orelse return` read-back
                     * from a tracked global. Alias dest to the global's
                     * pseudo-root entry; the inherited state (FREED after a
                     * free of any alias) flows through the COPY chain to the
                     * user local and fires at the use site. escaped=true is
                     * inherited via the snapshot — no false leak. */
                    IRHandleInfo *gh = ir_find_compound_handle(ps,
                        IR_GLOBAL_ROOT_ID, rhs->ident.name,
                        (uint32_t)rhs->ident.name_len);
                    if (gh && gh->alloc_id != 0) {
                        IRAliasSnapshot snap;
                        ir_snapshot_alias(&snap, gh);
                        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                        if (dst_h) {
                            ir_apply_alias(dst_h, &snap);
                            dst_h->state = snap.state;
                        }
                    }
                }
                break;
            }

            /* Phase E: move struct from array element — `Token copy = arr[0]`
             * where arr is Token[N] and Token is a move struct transfers
             * ownership from arr[0]. Register compound handle (arr, "[0]")
             * as TRANSFERRED; subsequent access to arr[0] triggers UAF.
             * Only handles literal-index (NODE_INT_LIT) compound keys.
             *
             * Gap A3 (2026-05-06, sNsjM): also covers NODE_FIELD — `Tok t = b.inner`
             * where b.inner is a move-struct field. Pre-fix: only NODE_INDEX was
             * handled; field-read move-transfer was silently dropped. */
            if (rhs && (rhs->kind == NODE_INDEX || rhs->kind == NODE_FIELD) &&
                inst->dest_local >= 0 &&
                inst->dest_local < func->local_count &&
                ir_should_track_move(func->locals[inst->dest_local].type)) {
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, rhs,
                                             &root_local, &path, &path_len) == 0 &&
                    path_len > 0) {
                    IRHandleInfo *ch = ir_find_compound_handle(ps, root_local,
                                                                path, path_len);
                    if (ch && ch->state == IR_HS_TRANSFERRED) {
                        ir_zc_error(zc, inst->source_line,
                            "use after move: '%.*s' ownership transferred at line %d",
                            (int)path_len, path, ch->free_line);
                    }
                    if (!ch) ch = ir_add_compound_handle(ps, root_local,
                                                          path, path_len);
                    if (ch) {
                        ch->state = IR_HS_TRANSFERRED;
                        ch->free_line = inst->source_line;
                    }
                } else if (rhs->kind == NODE_INDEX &&
                           rhs->index_expr.index &&
                           rhs->index_expr.index->kind != NODE_INT_LIT) {
                    /* Companion to BUG-741 (the variable-index FREE barrier): a
                     * VARIABLE-index MOVE out of a move-struct array element —
                     * `Token m = arr[i]`. Key extraction only accepts literal
                     * indices (ir_measure/build_key_path return -1 otherwise),
                     * so the move was previously untracked and every element
                     * stayed ALIVE, making a later literal-index read (arr[0]) a
                     * silent use-after-move. The moved element's identity is
                     * unknowable at compile time and — unlike the FREE path —
                     * there are no pre-registered literal `[N]` move siblings to
                     * widen, so this is a hard error (audit Expected). The
                     * enclosing ir_should_track_move(dest type) guard keeps this
                     * off non-move arrays (`u32 x = arr[i]` is unaffected). */
                    ir_zc_error(zc, inst->source_line,
                        "variable-index move from a move-struct array — any "
                        "element may have been moved; index by a literal or "
                        "hand off the whole array");
                }
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
                            /* audit 2026-06-04: unified via ir_apply_alias.
                             * Pre-fix was missing pool_name+len and
                             * is_thread_handle — wrong-pool detection
                             * silently bypassed via @ptrcast through
                             * *opaque. */
                            IRAliasSnapshot snap;
                            ir_snapshot_alias(&snap, src_h);
                            IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                            if (dst_h) {
                                ir_apply_alias(dst_h, &snap);
                                dst_h->state = snap.state;
                            }
                        }
                    }
                }
            }

            /* Phase E: interior pointer tracking. `*T field_ptr = &b.c`
             * lowers to IR_ASSIGN with expr = NODE_UNARY(TOK_AMP, NODE_FIELD(b, c)).
             * field_ptr should share alloc_id with b so when b is freed,
             * field_ptr is also flagged. Walk &expr down to root ident.
             *
             * audit 2026-06-04: pre-fix was missing pool_name+len, escaped,
             * is_thread_handle — wrong-pool detection bypassed via
             * `*T fp = &b.field; Handle k = *fp;` interior pointer chain. */
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
                        /* bh18_1b (2026-07-01): a move-struct local isn't a
                         * tracked handle until it's transferred, so `*T p = &a`
                         * taken BEFORE `T b = a` had nothing to alias and the
                         * transfer never reached p (use-after-move via the alias
                         * slipped). Register the move base ALIVE now (flagged
                         * is_move_local so the leak check skips it AND its alias)
                         * so the alias link forms and the transfer propagates
                         * TRANSFERRED to p. */
                        if (base_local < func->local_count &&
                            ir_should_track_move(func->locals[base_local].type)) {
                            if (!base_h) base_h = ir_add_handle(ps, base_local);
                            if (base_h) {
                                base_h->is_move_local = true;
                                if (base_h->state == IR_HS_UNKNOWN) {
                                    base_h->state = IR_HS_ALIVE;
                                    base_h->alloc_line = inst->source_line;
                                }
                                if (base_h->alloc_id == 0)
                                    base_h->alloc_id = _ir_next_alloc_id++;
                            }
                        }
                        if (base_h && base_h->alloc_id != 0) {
                            IRAliasSnapshot snap;
                            ir_snapshot_alias(&snap, base_h);
                            IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                            if (dst_h) {
                                ir_apply_alias(dst_h, &snap);
                                dst_h->state = snap.state;
                            }
                        }
                    }
                }
                /* Don't break — continue alias path if RHS is just an ident too */
            }

            if (rhs && rhs->kind == NODE_CALL) {
                IRMethodKind mc = ir_classify_method_call_ex(zc->checker, rhs);
                /* GAP-4 (BUG-740): result-assigned indirect call —
                 * `u32 x = fp(h);`. Same argument-precise barrier as the
                 * statement-form IR_CALL hook. BUG-742: same dangling-
                 * global call-window check too — result-assigned calls to
                 * ZER functions (`u32 v = g();`) or through funcptrs are
                 * observation points just like statement calls. */
                if (mc == IRMC_NONE && ir_call_is_indirect(zc, func, rhs)) {
                    ir_check_dangling_globals_at_call(zc, ps,
                                                      inst->source_line);
                    ir_indirect_call_barrier(zc, func, ps, rhs,
                                             inst->source_line);
                } else if (mc == IRMC_NONE &&
                           rhs->call.callee &&
                           rhs->call.callee->kind == NODE_IDENT &&
                           ir_callee_has_summary(zc,
                               rhs->call.callee->ident.name,
                               (uint32_t)rhs->call.callee->ident.name_len)) {
                    ir_check_dangling_globals_at_call(zc, ps,
                                                      inst->source_line);
                }
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
                        /* F3.2: record source-level pool name for
                         * cross-pool misuse detection at GET/FREE sites
                         * (handled by ir_check_expr_wrong_pool walker). */
                        ir_extract_pool_name(rhs, &h->pool_name,
                                             &h->pool_name_len);
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
                            if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
                                ir_zc_error(zc, inst->source_line,
                                    "use after free: local %%%d is %s (freed at line %d)",
                                    root_local, ir_state_name(h->state), h->free_line);
                            }
                            /* F3.2: cross-pool misuse — handle came from
                             * a different Pool/Slab than the receiver. */
                            /* F3.2 wrong-pool check is centralized in
                             * ir_check_expr_wrong_pool walker invoked
                             * via the IR_ASSIGN entry. */
                        }
                    }
                    /* Fall through — dest may still need tracking if get result is pointer */
                } else if (mc == IRMC_NONE && inst->dest_local >= 0 &&
                           inst->dest_local < func->local_count) {
                    /* p3Qz0 Gap 38 (2026-05-16): non-method function call
                     * returning Handle(T) or ?Handle(T) — register dest as
                     * fresh ALIVE allocation. Covers orelse-wrapped case:
                     *   Handle(Task) h = get_handle() orelse return;
                     * which lowers to IR_ASSIGN with NODE_ORELSE expr.
                     * Sets escaped=true so leak-at-exit doesn't fire across
                     * function boundaries; FREED transitions still tracked. */
                    Type *dt = func->locals[inst->dest_local].type;
                    Type *eff = dt ? type_unwrap_distinct(dt) : NULL;
                    if (eff && eff->kind == TYPE_OPTIONAL) {
                        eff = type_unwrap_distinct(eff->optional.inner);
                    }
                    if (eff && eff->kind == TYPE_HANDLE) {
                        IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                        if (h) {
                            if (h->state == IR_HS_ALIVE &&
                                !func->locals[inst->dest_local].is_temp) {
                                ir_zc_error(zc, inst->source_line,
                                    "handle %%%d overwritten while alive — previous leaked",
                                    inst->dest_local);
                            }
                            h->state = IR_HS_ALIVE;
                            h->alloc_line = inst->source_line;
                            h->alloc_id = _ir_next_alloc_id++;
                            h->source_color = ZC_COLOR_UNKNOWN;
                            h->escaped = true;
                        }
                        break;
                    }
                    /* GAP-B fix (2026-06-07): extern alloc returning ?*T fused
                     * with var-decl + orelse in IR_ASSIGN shape. Pre-fix, only
                     * TYPE_HANDLE branch above registered ALIVE state; non-Handle
                     * pointer returns from extern alloc fell through, so the
                     * post-free use was silently allowed.
                     *
                     * Two-step form (?*Res maybe = my_alloc(); *Res p = maybe orelse return;)
                     * already worked because the IR_CALL extern-alloc handler at
                     * line ~2935 fires when inst->expr is NODE_CALL. The fused
                     * form fails there because inst->expr is NODE_ORELSE.
                     *
                     * Fix: mirror the TYPE_HANDLE branch for TYPE_POINTER/OPAQUE
                     * when the unwrapped call is recognized as extern alloc by
                     * ir_is_extern_alloc_call. The escaped=true flag prevents
                     * leak-at-exit false positives across the function boundary
                     * (caller may legitimately pass the pointer to a destructor).
                     * Reproducer: tests/audit_2026_06_06/audit_extern_alloc_orelse_uaf.zer */
                    if (eff && (eff->kind == TYPE_POINTER || eff->kind == TYPE_OPAQUE) &&
                        rhs && rhs->kind == NODE_CALL &&
                        ir_is_extern_alloc_call(zc, rhs)) {
                        IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                        if (h) {
                            if (h->state == IR_HS_ALIVE &&
                                !func->locals[inst->dest_local].is_temp) {
                                ir_zc_error(zc, inst->source_line,
                                    "handle %%%d overwritten while alive — previous leaked",
                                    inst->dest_local);
                            }
                            h->state = IR_HS_ALIVE;
                            h->alloc_line = inst->source_line;
                            h->alloc_id = _ir_next_alloc_id++;
                            h->source_color = ZC_COLOR_MALLOC;
                            h->escaped = true;
                        }
                        break;
                    }
                    /* BH-19 #1 (2026-06-19): parity with Gap 38 (Handle path).
                     * Bodied ZER factory returning `?*T` from a fused
                     * `*T h = factory() orelse return;` — orelse-wrapped
                     * IR_ASSIGN's expr is NODE_ORELSE, so the regular
                     * IR_CALL/IR_ASSIGN-NODE_CALL handlers below don't see it.
                     * Pre-fix, only TYPE_HANDLE returns (Gap 38) and extern
                     * `?*T` allocs (GAP-B) were registered here; bodied ZER
                     * factories returning `?*T` silently fell through, leaving
                     * the destination untracked. Subsequent free + free was
                     * accepted (asymmetric: the `?Handle` factory variant
                     * caught it). Mirror Gap 38: register ALIVE+escaped so
                     * FREED transitions fire while leak-at-exit doesn't
                     * (caller commonly hands ownership to a destructor).
                     * Repro: tests/zer_fail/bh19_factory_alloc_ptr_*.zer */
                    if (eff && (eff->kind == TYPE_POINTER || eff->kind == TYPE_OPAQUE) &&
                        rhs && rhs->kind == NODE_CALL) {
                        IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                        if (h) {
                            if (h->state == IR_HS_ALIVE &&
                                !func->locals[inst->dest_local].is_temp) {
                                ir_zc_error(zc, inst->source_line,
                                    "handle %%%d overwritten while alive — previous leaked",
                                    inst->dest_local);
                            }
                            h->state = IR_HS_ALIVE;
                            h->alloc_line = inst->source_line;
                            h->alloc_id = _ir_next_alloc_id++;
                            h->source_color = ZC_COLOR_UNKNOWN;
                            h->escaped = true;
                        }
                        break;
                    }
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
                        /* Non-move: regular alias.
                         *
                         * audit 2026-06-04: pre-fix was missing pool_name+len
                         * and escaped — wrong-pool detection bypassed when
                         * alloc goes through ?Handle (which decomposes via
                         * IR_ASSIGN-with-NODE_IDENT temps). Unified via
                         * ir_apply_alias. */
                        if (src_h && src_h->state == IR_HS_ALIVE) {
                            IRAliasSnapshot snap;
                            ir_snapshot_alias(&snap, src_h);
                            IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                            if (dst_h) {
                                ir_apply_alias(dst_h, &snap);
                                dst_h->state = IR_HS_ALIVE;
                            }
                        }
                        /* Check use of invalid handle */
                        if (src_h && ir_is_invalid(src_h)) {
                            ir_zc_error(zc, inst->source_line,
                                "use of %s handle %%%d",
                                ir_state_name(src_h->state), src_local);
                        }
                    }
                } else if (ir_ident_is_unshadowed_global(zc, func,
                                                         inst->expr)) {
                    /* GAP-3 (BUG-739): bare global read-back —
                     * `?*Item m = g_ptr;`. Alias dest to the global's
                     * pseudo-root entry, inheriting its state so a
                     * post-free read-back flags at the use site. */
                    IRHandleInfo *gh = ir_find_compound_handle(ps,
                        IR_GLOBAL_ROOT_ID, inst->expr->ident.name,
                        (uint32_t)inst->expr->ident.name_len);
                    if (gh && gh->alloc_id != 0) {
                        IRAliasSnapshot snap;
                        ir_snapshot_alias(&snap, gh);
                        IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
                        if (dst_h) {
                            ir_apply_alias(dst_h, &snap);
                            dst_h->state = snap.state;
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
                if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
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
                if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
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
                    if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
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
                    if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
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
        /* BUG-703: undo same-line move-compound materialization BEFORE any
         * use-check. `consume(w.inner)` lowers to `tmp = w.inner` (IR_FIELD_READ,
         * which runs the Gap A3 move-transfer on the compound) + this IR_CALL.
         * So by the time we get here the compound is already TRANSFERRED — and
         * both the generic UAF walker (below) and the move loop (further down)
         * would re-report it as use-after-move ON THE TRANSFER LINE. That's a
         * false over-rejection of a valid by-value move-field call.
         *
         * Fix: for a move-typed COMPOUND arg whose transfer happened on THIS
         * call's own line (free_line == source_line), reset it to ALIVE. The
         * move loop below then re-transfers the bare root as the single
         * authority. Genuine PRIOR transfers (free_line < source_line) are NOT
         * reset and still reported. A later use of the compound is still caught
         * via the bare-root prefix in the FIELD_READ use-check. Per-compound
         * (not per-root) so a sibling compound transferred earlier is unaffected.
         * Residual edge (documented in limitations.md): two move-consumes of the
         * SAME compound on ONE physical line can't be distinguished. */
        if (inst->expr && inst->expr->kind == NODE_CALL) {
            Node *bcall = inst->expr;
            for (int bpi = 0; bpi < bcall->call.arg_count; bpi++) {
                Node *barg = bcall->call.args[bpi];
                if (!barg) continue;
                if (barg->kind == NODE_UNARY && barg->unary.op == TOK_AMP)
                    barg = barg->unary.operand;
                if (barg->kind != NODE_FIELD && barg->kind != NODE_INDEX) continue;
                Type *bt = checker_get_type(zc->checker, barg);
                if (!bt || !ir_should_track_move(bt)) continue;
                int broot; const char *bpath; uint32_t bplen;
                if (ir_extract_compound_key(zc, func, barg,
                                            &broot, &bpath, &bplen) != 0) continue;
                if (bplen == 0) continue;
                IRHandleInfo *bch = ir_find_compound_handle(ps, broot, bpath, bplen);
                if (bch && bch->state == IR_HS_TRANSFERRED &&
                    bch->free_line == inst->source_line) {
                    bch->state = IR_HS_ALIVE;  /* undo same-line materialization */
                }
            }
        }
        /* Phase E: generic UAF walker on the call args. Catches
         * use_ptr(freed_ident) / func(&freed.field) etc. */
        if (inst->expr) {
            UafReportSet rs = {0};
            ir_check_expr_uaf(zc, func, ps, inst->expr, inst->source_line, &rs);
            UafReportSet pool_rs = {0};
            ir_check_expr_wrong_pool(zc, func, ps, inst->expr,
                                     inst->source_line, &pool_rs);
            free(pool_rs.ids);
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
                /* Unwrap &expr to get target */
                Node *target = arg;
                if (target->kind == NODE_UNARY && target->unary.op == TOK_AMP)
                    target = target->unary.operand;

                /* Gap 42 fix (2026-04-27, Stage 2): walk through
                 * NODE_FIELD/NODE_INDEX to the root ident. Previously
                 * only bare-NODE_IDENT roots were handled, so
                 * `consume(b.item)` where b.item is a move struct field
                 * was silently skipped. Now `b.item` walks through to
                 * `b` (root ident); the walker checks if the FIELD/INDEX
                 * type itself is a move struct (or contains one) — if so,
                 * the consume transfers ownership of the inner part. */
                Type *target_type = checker_get_type(zc->checker, target);
                Node *root = target;
                while (root && (root->kind == NODE_FIELD ||
                                root->kind == NODE_INDEX)) {
                    if (root->kind == NODE_FIELD) root = root->field.object;
                    else root = root->index_expr.object;
                }
                if (!root || root->kind != NODE_IDENT) continue;
                int arg_local = ir_find_local_exact_first(func,
                    root->ident.name, (uint32_t)root->ident.name_len);
                if (arg_local < 0 || arg_local >= func->local_count) continue;
                Type *arg_type = func->locals[arg_local].type;
                /* Either the target field-type itself is a move struct
                 * (e.g. `consume(b.item)` with item: File(move)), OR the
                 * root container's type contains a move field (covered
                 * by direct pass `consume(b)` where b: struct{File f}). */
                bool target_is_move = target_type &&
                    ir_should_track_move(target_type);
                bool root_is_move = ir_should_track_move(arg_type);
                if (!target_is_move && !root_is_move) continue;

                /* Gap A3 follow-up (2026-05-06, sNsjM): when target is a
                 * compound (b.inner, arr[0].field), check the compound handle
                 * FIRST — if it was already TRANSFERRED by an earlier field-read
                 * move (`Tok t = b.inner`), passing &b.inner is use-after-move.
                 * Without this, only the bare handle for root b was checked,
                 * so compound TRANSFERRED state was silently ignored. */
                IRHandleInfo *compound_h = NULL;
                if (target_is_move &&
                    (target->kind == NODE_FIELD ||
                     target->kind == NODE_INDEX)) {
                    int croot;
                    const char *cpath;
                    uint32_t cpath_len;
                    if (ir_extract_compound_key(zc, func, target,
                                                 &croot, &cpath,
                                                 &cpath_len) == 0 &&
                        cpath_len > 0) {
                        compound_h = ir_find_compound_handle(ps, croot,
                                                              cpath, cpath_len);
                        if (compound_h && compound_h->state == IR_HS_TRANSFERRED) {
                            ir_zc_error(zc, inst->source_line,
                                "use after move: compound '%.*s' on local "
                                "'%.*s' transferred at line %d",
                                (int)cpath_len, cpath,
                                (int)func->locals[arg_local].name_len,
                                func->locals[arg_local].name,
                                compound_h->free_line);
                        }
                    }
                }

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
            IRMethodKind mc = ir_classify_method_call_ex(zc->checker, inst->expr);
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
                    /* F3.2: record source-level pool name for
                     * cross-pool misuse detection at GET/FREE sites. */
                    ir_extract_pool_name(inst->expr, &h->pool_name,
                                         &h->pool_name_len);
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
            /* Gap 39 (2026-04-27): arena.reset() / unsafe_reset() invalidates
             * all handles whose source_color is ZC_COLOR_ARENA, AND any
             * aliases sharing alloc_id (so unwrapped pointers from
             * `?[*]T s = ar.alloc_slice(...) orelse ...` are also flagged).
             * Two-pass: collect ARENA alloc_ids first (since reallocs from
             * adding may invalidate pointers — Gap 20-25 family), then
             * walk and mark any handle in the set FREED. */
            if (mc == IRMC_ARENA_RESET) {
                /* AU-2 (2026-07-01): the two-pass arena-handle invalidation is
                 * now ir_mark_arena_handles_freed, shared with the defer-body
                 * scanner so a deferred arena.reset() behaves identically. */
                ir_mark_arena_handles_freed(ps, inst->source_line);
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
                            if (ir_use_guard_disjoint(zc, h)) {
                                if (ir_free_completes_coverage(zc, h))
                                    h->freed_all_paths = 1;
                                ir_mark_multi_freed(ps, h);  /* audit 2026-07-06 */
                            } else {
                                ir_zc_error(zc, inst->source_line,
                                    "freeing local %%%d which may already be freed",
                                    root_local);
                            }
                        } else if (h->state == IR_HS_TRANSFERRED) {
                            ir_zc_error(zc, inst->source_line,
                                "freeing local %%%d which was already transferred",
                                root_local);
                        }
                        /* F3.2 wrong-pool check is centralized in
                         * ir_check_expr_wrong_pool walker invoked at
                         * the IR_CALL entry point below. */
                        h->state = IR_HS_FREED;
                        h->free_line = inst->source_line;
                        ir_propagate_alias_state(ps, h, IR_HS_FREED,
                                                  inst->source_line);
                    }
                } else if (arg && arg->kind == NODE_INDEX &&
                           arg->index_expr.index &&
                           arg->index_expr.index->kind != NODE_INT_LIT) {
                    /* GAP-6 (BUG-741, 2026-06-10, 6u360k audit): free through
                     * a VARIABLE index — `heap.free(arr[k])`. Key extraction
                     * only accepts literal indices, so this free was
                     * previously untracked entirely: `free(arr[k]);
                     * free(arr[0])` with k==0 was a silent double free
                     * (and _zer_slab_free no-ops the second free at runtime).
                     *
                     * Rule (same principle as the BUG-740 indirect-call
                     * barrier): an operation the analyzer can't resolve may
                     * consume ANY tracked element of that array.
                     *  (1) a literal-indexed sibling already definitely
                     *      FREED → this free may target it → error
                     *      (catches `free(arr[0]); free(arr[k])`).
                     *  (2) widen ALIVE literal-indexed siblings (and their
                     *      alias groups) to MAYBE_FREED + escaped — a later
                     *      literal free errors (`free(arr[k]); free(arr[0])`,
                     *      the reproducer), the exit pass stays quiet.
                     * Free-everything loops stay clean: variable-index
                     * STORES already escape-untrack their values (no
                     * '['-entries exist), and widened MAYBE siblings do not
                     * re-trigger (1), which fires on definite FREED only. */
                    Node *aroot = arg->index_expr.object;
                    while (aroot && (aroot->kind == NODE_FIELD ||
                                     aroot->kind == NODE_INDEX)) {
                        if (aroot->kind == NODE_FIELD)
                            aroot = aroot->field.object;
                        else
                            aroot = aroot->index_expr.object;
                    }
                    int aloc = (aroot && aroot->kind == NODE_IDENT)
                        ? ir_find_local_exact_first(func, aroot->ident.name,
                              (uint32_t)aroot->ident.name_len)
                        : -1;
                    if (aloc >= 0 && aloc < func->local_count) {
                        for (int vhi = 0; vhi < ps->handle_count; vhi++) {
                            IRHandleInfo *vh = &ps->handles[vhi];
                            if (vh->local_id != aloc) continue;
                            if (vh->path_len == 0 || !vh->path ||
                                vh->path[0] != '[') continue;
                            if (vh->state == IR_HS_FREED) {
                                ir_zc_error(zc, inst->source_line,
                                    "variable-index free may double-free "
                                    "'%.*s%.*s' already freed at line %d — "
                                    "don't mix literal- and variable-index "
                                    "frees on the same array",
                                    (int)func->locals[aloc].name_len,
                                    func->locals[aloc].name,
                                    (int)vh->path_len, vh->path,
                                    vh->free_line);
                            } else if (vh->state == IR_HS_ALIVE) {
                                vh->state = IR_HS_MAYBE_FREED;
                                vh->free_line = inst->source_line;
                                vh->escaped = true;
                                int vaid = vh->alloc_id;
                                if (vaid != 0) {
                                    for (int vgi = 0; vgi < ps->handle_count; vgi++) {
                                        IRHandleInfo *vg = &ps->handles[vgi];
                                        if (vg == vh || vg->alloc_id != vaid)
                                            continue;
                                        if (ir_is_invalid(vg)) continue;
                                        vg->state = IR_HS_MAYBE_FREED;
                                        vg->free_line = inst->source_line;
                                        vg->escaped = true;
                                    }
                                }
                            }
                        }
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
                    if (h && ir_is_invalid(h) && !ir_use_guard_disjoint(zc, h)) {
                        ir_zc_error(zc, inst->source_line,
                            "use after free: local %%%d is %s (freed at line %d)",
                            root_local, ir_state_name(h->state), h->free_line);
                    }
                    /* F3.2 wrong-pool check centralized in walker. */
                }
                break;
            }
        }

        /* GAP-4 (BUG-740): indirect call — unknown callee, no FuncSummary
         * to apply. Argument-precise barrier instead: what was handed in
         * may have been freed. See ir_indirect_call_barrier.
         * BUG-742: an unknown callee may also READ globals — check the
         * dangling-global window first (prior state, before this call's
         * own effects). */
        if (inst->expr && inst->expr->kind == NODE_CALL &&
            ir_call_is_indirect(zc, func, inst->expr)) {
            ir_check_dangling_globals_at_call(zc, ps, inst->source_line);
            ir_indirect_call_barrier(zc, func, ps, inst->expr,
                                     inst->source_line);
            break;
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

        /* BUG-742 call-window rule: a ZER-defined callee (summary exists)
         * can read globals — calling it while a global is definitely
         * dangling may observe freed memory. Bodyless externs never have
         * a summary and are excluded (outside the safety boundary). */
        if (summary) {
            ir_check_dangling_globals_at_call(zc, ps, inst->source_line);
        }

        /* Phase F: param-color inference application.
         * If the callee's summary has returns_param_color > 0, the
         * return value is an ALIAS of args[param_color-1]. Make dest
         * share the arg's alloc_id/state/color.
         * Covers `*T unwrap(*opaque raw) { return (*T)raw; }` pattern. */
        bool dest_aliased_from_param = false;
        if (summary && summary->returns_param_color > 0 &&
            inst->dest_local >= 0 && inst->expr &&
            inst->expr->kind == NODE_CALL) {
            int param_idx = summary->returns_param_color - 1;
            if (param_idx < inst->expr->call.arg_count) {
                Node *arg = inst->expr->call.args[param_idx];
                if (arg && arg->kind == NODE_IDENT) {
                    int arg_local = ir_find_local_exact_first(func,
                        arg->ident.name, (uint32_t)arg->ident.name_len);
                    if (arg_local >= 0) {
                        IRHandleInfo *arg_h = ir_find_handle(ps, arg_local);
                        if (arg_h) {
                            IRHandleInfo *dh = ir_add_handle(ps, inst->dest_local);
                            if (dh) {
                                /* Audit 2026-06-11: raw field copy missed
                                 * pool_name + escaped + is_thread_handle, so
                                 * wrong-pool detection silently bypassed when
                                 * the handle round-tripped through a passthrough
                                 * function. Use ir_apply_alias to copy ALL
                                 * tracked state — same shape as IR_COPY at 2758. */
                                IRAliasSnapshot snap;
                                ir_snapshot_alias(&snap, arg_h);
                                ir_apply_alias(dh, &snap);
                                dh->state = arg_h->state;
                                dest_aliased_from_param = true;
                            }
                        }
                    }
                }
            }
        }

        /* Phase C2: signature heuristics on inst->expr (the AST NODE_CALL).
         * Covers extern malloc/free/destroy patterns and pointer-returning
         * function calls without a FuncSummary.
         *
         * Also: when summary exists but returns_color is POOL/UNKNOWN AND
         * no param-color alias was set, register dest as ALIVE using the
         * summary's color. Mirrors zercheck.c:786-808 unconditional
         * pointer-return registration. ARENA summaries skip entirely. */
        bool summary_non_arena = summary &&
            summary->returns_color != ZC_COLOR_ARENA;
        if (summary_non_arena && !dest_aliased_from_param &&
            inst->dest_local >= 0 && inst->expr &&
            inst->expr->kind == NODE_CALL) {
            bool already_tracked = (ir_find_handle(ps, inst->dest_local) != NULL);
            if (!already_tracked) {
                Type *ret = checker_get_type(zc->checker, inst->expr);
                Type *ret_eff = ret ? type_unwrap_distinct(ret) : NULL;
                bool is_ptr_return = false;
                /* p3Qz0 Gap 38 (2026-05-16): TYPE_HANDLE added to register
                 * function-returned handles for double-free tracking. */
                if (ret_eff && (ret_eff->kind == TYPE_POINTER ||
                                ret_eff->kind == TYPE_OPAQUE ||
                                ret_eff->kind == TYPE_HANDLE))
                    is_ptr_return = true;
                if (ret_eff && ret_eff->kind == TYPE_OPTIONAL) {
                    Type *inner = type_unwrap_distinct(ret_eff->optional.inner);
                    if (inner && (inner->kind == TYPE_POINTER ||
                                  inner->kind == TYPE_OPAQUE ||
                                  inner->kind == TYPE_HANDLE))
                        is_ptr_return = true;
                }
                if (is_ptr_return) {
                    IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
                    if (h) {
                        h->state = IR_HS_ALIVE;
                        h->alloc_line = inst->source_line;
                        h->alloc_id = _ir_next_alloc_id++;
                        h->source_color = summary->returns_color;
                        /* Mark Handle returns as escaped — leak-at-exit shouldn't
                         * fire (ownership came from another function; caller often
                         * passes to a destructor). Double-free state still works. */
                        if (ret_eff && (ret_eff->kind == TYPE_HANDLE ||
                            (ret_eff->kind == TYPE_OPTIONAL &&
                             type_unwrap_distinct(ret_eff->optional.inner) &&
                             type_unwrap_distinct(ret_eff->optional.inner)->kind == TYPE_HANDLE))) {
                            h->escaped = true;
                        }
                    }
                }
            }
        }

        if (!summary && inst->expr && inst->expr->kind == NODE_CALL) {
            Node *call = inst->expr;

            /* Phase E: any pointer-returning call treated as allocation
             * (mirrors zercheck.c:786-808). Applies to both extern
             * (bodyless) and bodied functions. Extern is a subset of
             * this check — kept separate for MALLOC coloring.
             *
             * Skip conditions:
             *   - Call doesn't return a pointer type (no alloc)
             *   - FuncSummary says returns_color is ARENA (bulk-reset)
             *   - Dest already has a handle registered (avoid double-wrap)
             *   - Extern heuristic already applies (avoids re-registration) */
            bool already_handled = ir_is_extern_alloc_call(zc, call);
            bool treat_as_alloc = already_handled;
            if (!already_handled && inst->dest_local >= 0 &&
                inst->dest_local < func->local_count) {
                /* Check callee return type */
                Type *ret = checker_get_type(zc->checker, call);
                Type *ret_eff = ret ? type_unwrap_distinct(ret) : NULL;
                bool is_ptr_return = false;
                /* p3Qz0 Gap 38: TYPE_HANDLE added for double-free tracking. */
                if (ret_eff && (ret_eff->kind == TYPE_POINTER ||
                                ret_eff->kind == TYPE_OPAQUE ||
                                ret_eff->kind == TYPE_HANDLE))
                    is_ptr_return = true;
                if (ret_eff && ret_eff->kind == TYPE_OPTIONAL) {
                    Type *inner = type_unwrap_distinct(ret_eff->optional.inner);
                    if (inner && (inner->kind == TYPE_POINTER ||
                                  inner->kind == TYPE_OPAQUE ||
                                  inner->kind == TYPE_HANDLE))
                        is_ptr_return = true;
                }
                /* Check if dest already has handle (e.g., arena-colored) */
                bool already_tracked = (ir_find_handle(ps, inst->dest_local) != NULL);
                if (is_ptr_return && !already_tracked) treat_as_alloc = true;
            }
            if (inst->dest_local >= 0 && treat_as_alloc) {
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
                    /* p3Qz0 Gap 38: mark Handle-returning calls as escaped so
                     * leak-at-exit doesn't fire; FREED transitions still work. */
                    Type *dt = (inst->dest_local < func->local_count) ?
                        func->locals[inst->dest_local].type : NULL;
                    Type *dt_eff = dt ? type_unwrap_distinct(dt) : NULL;
                    if (dt_eff && dt_eff->kind == TYPE_OPTIONAL)
                        dt_eff = type_unwrap_distinct(dt_eff->optional.inner);
                    if (dt_eff && dt_eff->kind == TYPE_HANDLE)
                        h->escaped = true;
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
                            if (ir_use_guard_disjoint(zc, h)) {
                                if (ir_free_completes_coverage(zc, h))
                                    h->freed_all_paths = 1;
                                ir_mark_multi_freed(ps, h);  /* audit 2026-07-06 */
                            } else {
                                ir_zc_error(zc, inst->source_line,
                                    "freeing local %%%d which may already be freed",
                                    root_local);
                            }
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

        /* For each param the summary affects, resolve arg local, apply state.
         *
         * AUDIT 2026-06-06 (GAP-A): the passthrough path used to filter
         * on `inst->args[pi]->kind == NODE_IDENT`, so compound arguments
         * like `c.h` (NODE_FIELD) or `arr[i]` (NODE_INDEX) were silently
         * skipped. Cross-function double-free of a struct-field handle
         * compiled cleanly. Mirror the compound-key resolution that the
         * extern_free path already uses (see lines 2902-2942). */
        for (int pi = 0; pi < summary->param_count; pi++) {
            if (!summary->frees_param[pi] && !summary->maybe_frees_param[pi])
                continue;

            int arg_local = -1;
            const char *compound_path = NULL;
            uint32_t compound_path_len = 0;
            /* Decomposed path — call_arg_locals already pre-resolved to a
             * bare local (often an IR-synthesized temp produced by
             * FIELD_READ / INDEX_READ when the caller wrote a compound
             * expression like `f(c.h)`). */
            if (inst->call_arg_locals && pi < inst->call_arg_local_count) {
                arg_local = inst->call_arg_locals[pi];
            }
            /* AUDIT 2026-06-06 (GAP-A): even when the decomposed path
             * found a temp, the original AST may carry a compound
             * expression — `inst->call_arg_locals[pi]` is the post-FIELD_READ
             * temp, which has no tracked handle. Re-resolve against
             * `inst->args[pi]` so compound shapes flow through the same
             * code path as bare locals. Mirror the extern_free path
             * (lines 2902-2942) which already uses ir_extract_compound_key. */
            if (inst->args && pi < inst->arg_count && inst->args[pi]) {
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, inst->args[pi],
                                             &root_local, &path,
                                             &path_len) == 0) {
                    /* Prefer the compound path if it finds a real handle. */
                    IRHandleInfo *probe = (path_len == 0)
                        ? ir_find_handle(ps, root_local)
                        : ir_find_compound_handle(ps, root_local, path, path_len);
                    if (probe) {
                        arg_local = root_local;
                        compound_path = path;
                        compound_path_len = path_len;
                    } else if (arg_local < 0) {
                        /* No decomposed local either — fall back to compound
                         * resolution so auto-registration of param args still
                         * fires below. */
                        arg_local = root_local;
                        compound_path = path;
                        compound_path_len = path_len;
                    }
                }
            }
            if (arg_local < 0) continue;

            IRHandleInfo *h;
            if (compound_path_len > 0) {
                h = ir_find_compound_handle(ps, arg_local,
                                            compound_path, compound_path_len);
            } else {
                h = ir_find_handle(ps, arg_local);
            }
            /* Phase E: Auto-register when caller's own arg is a param and
             * the callee frees it. Enables FuncSummary chain propagation:
             * step_c frees its param → step_b calls step_c(r) → step_b's
             * r (also a param) gets marked FREED → step_b's summary
             * frees_param[0]=true. Mark escaped to avoid leak flag.
             *
             * Auto-registration only applies to bare-local param args; a
             * compound (`*p.field`) is too speculative to forge state for. */
            if (!h && compound_path_len == 0 &&
                arg_local < func->local_count &&
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

        /* Arguments passed to spawn transfer ownership.
         *
         * AUDIT 2026-06-06 (GAP-C): pre-fix only matched NODE_IDENT, so
         * `spawn worker(b.t)` (NODE_FIELD) and `spawn worker(arr[i])`
         * (NODE_INDEX) silently skipped the TRANSFERRED transition,
         * producing silent use-after-move. Use ir_extract_compound_key
         * to resolve both bare-local and compound shapes. */
        for (int i = 0; i < inst->arg_count; i++) {
            if (!inst->args || !inst->args[i]) continue;
            int root_local;
            const char *path;
            uint32_t path_len;
            if (ir_extract_compound_key(zc, func, inst->args[i],
                                         &root_local, &path, &path_len) != 0)
                continue;
            IRHandleInfo *h = (path_len == 0)
                ? ir_find_handle(ps, root_local)
                : ir_find_compound_handle(ps, root_local, path, path_len);
            if (h) h->state = IR_HS_TRANSFERRED;
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
         * propagates via ir_propagate_alias_state.
         *
         * audit 2026-06-04: was most incomplete site — only copied state/
         * alloc_line/alloc_id. Missing source_color, pool_name+len,
         * escaped, is_thread_handle. Plus latent UAF (rh read after
         * realloc). All fixed via ir_apply_alias. */
        if (target_expr && rhs_local >= 0) {
            IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
            if (rh && rh->state == IR_HS_ALIVE) {
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, target_expr,
                                             &root_local, &path, &path_len) == 0
                    && path_len > 0) {
                    IRAliasSnapshot snap;
                    ir_snapshot_alias(&snap, rh);
                    IRHandleInfo *ch = ir_add_compound_handle(ps, root_local,
                                                               path, path_len);
                    if (ch) {
                        ir_apply_alias(ch, &snap);
                        ch->state = IR_HS_ALIVE;
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
         * Variable indices aren't trackable (matches zercheck.c behavior).
         *
         * audit 2026-06-04: same alias-copy field-drift as IR_FIELD_WRITE
         * above. Fixed via ir_apply_alias unified helper. */
        if (target_expr && rhs_local >= 0) {
            IRHandleInfo *rh = ir_find_handle(ps, rhs_local);
            if (rh && rh->state == IR_HS_ALIVE) {
                int root_local;
                const char *path;
                uint32_t path_len;
                if (ir_extract_compound_key(zc, func, target_expr,
                                             &root_local, &path, &path_len) == 0
                    && path_len > 0) {
                    IRAliasSnapshot snap;
                    ir_snapshot_alias(&snap, rh);
                    IRHandleInfo *ch = ir_add_compound_handle(ps, root_local,
                                                               path, path_len);
                    if (ch) {
                        ir_apply_alias(ch, &snap);
                        ch->state = IR_HS_ALIVE;
                    }
                }
            }
        }
        break;
    }

    /* Stage 2 Part B (2026-04-28): exhaustive — IR ops that don't
     * affect handle/move/lock state are no-ops here. Adding a new IR_
     * forces a deliberate decision (does it allocate/free/transfer?). */
    case IR_BRANCH: case IR_GOTO: case IR_YIELD: case IR_AWAIT:
    case IR_LOCK: case IR_UNLOCK:
    case IR_ARENA_RESET: case IR_RING_PUSH: case IR_RING_POP:
    case IR_RING_PUSH_CHECKED:
    case IR_DEFER_PUSH: case IR_DEFER_FIRE:
    case IR_INTRINSIC:
    case IR_BINOP: case IR_LITERAL:
    case IR_ADDR_OF: case IR_DEREF_READ:
    case IR_CALL_DECOMP: case IR_INTRINSIC_DECOMP:
    case IR_ORELSE_DECOMP: case IR_SLICE_READ:
    case IR_STRUCT_INIT_DECOMP:
        break;
    }
}

/* ================================================================
 * Main Analysis — walk CFG in topological order
 * ================================================================ */

/* F0.5 (2026-05-03): walk a struct/union type recursively, registering
 * each Handle field as a compound handle on the path state.
 *
 * Path format matches ir_extract_compound_key: ".field1.field2..." —
 * dot-prefixed, NO root identifier name. The local_id is the param's
 * IR local; the path is just the dotted field chain.
 *
 * Mirrors zercheck.c:2769-2862 (Gap 29 fix) but uses IR's path scheme.
 * Without this, function params of nested-struct types never have
 * their inner handles tracked → use-after-free goes undetected.
 *
 * Depth-limited at 32 to prevent infinite recursion on malformed
 * recursive types. */
static void ir_register_nested_handles(IRPathState *ps, void *arena_ptr,
    int local_id, Type *t, const char *path, uint32_t path_len, int depth)
{
    if (depth > 32 || !t) return;
    Type *eff = type_unwrap_distinct(t);
    if (!eff) return;
    Arena *arena = (Arena *)arena_ptr;

    if (eff->kind == TYPE_UNION) {
        /* Variants share the path (variant doesn't add path component). */
        for (uint32_t vi = 0; vi < eff->union_type.variant_count; vi++) {
            Type *vt = type_unwrap_distinct(eff->union_type.variants[vi].type);
            if (vt && (vt->kind == TYPE_STRUCT || vt->kind == TYPE_UNION)) {
                ir_register_nested_handles(ps, arena, local_id, vt,
                    path, path_len, depth + 1);
            }
        }
        return;
    }
    if (eff->kind != TYPE_STRUCT) return;

    for (uint32_t fi = 0; fi < eff->struct_type.field_count; fi++) {
        Type *ft = type_unwrap_distinct(eff->struct_type.fields[fi].type);
        if (!ft) continue;
        const char *fname = eff->struct_type.fields[fi].name;
        uint32_t fnl = eff->struct_type.fields[fi].name_len;

        /* Build "<path>.<field>" — extends the existing dotted path */
        uint32_t new_plen = path_len + 1 + fnl;
        char *new_path = (char *)arena_alloc(arena, new_plen + 1);
        if (!new_path) continue;
        memcpy(new_path, path, path_len);
        new_path[path_len] = '.';
        memcpy(new_path + path_len + 1, fname, fnl);
        new_path[new_plen] = '\0';

        if (ft->kind == TYPE_HANDLE) {
            IRHandleInfo *h = ir_add_compound_handle(ps, local_id,
                new_path, new_plen);
            if (h) {
                h->state = IR_HS_ALIVE;
                h->alloc_line = 0;  /* param — no specific alloc site */
                h->alloc_id = local_id;
                h->source_color = 0;
            }
        } else if (ft->kind == TYPE_STRUCT || ft->kind == TYPE_UNION) {
            ir_register_nested_handles(ps, arena, local_id, ft,
                new_path, new_plen, depth + 1);
        }
    }
}

bool zercheck_ir(ZerCheck *zc, IRFunc *func) {
    if (!func || func->block_count == 0) return true;

    /* Allocate per-block states */
    IRPathState *block_states = (IRPathState *)calloc(func->block_count, sizeof(IRPathState));
    if (!block_states) return true;

    for (int bi = 0; bi < func->block_count; bi++)
        ir_ps_init(&block_states[bi]);

    /* Level B guarded refinement (2026-06-27): per-block guard sets — which
     * immutable-bool conditions hold on ALL paths to each block. Computed once;
     * read at MAYBE_FREED use sites to recover `if(c){free} if(!c){use}` when the
     * use's guard is disjoint from the free's. Stored on zc for the free/use
     * sites inside ir_check_inst. */
    IRGuardSet *block_guards = ir_compute_block_guards(func);
    zc->gr_block_guards = block_guards;
    zc->gr_block_count = func->block_count;
    zc->gr_cur_block = 0;

    /* F0.5 (2026-05-03): nested-handle param registration is done
     * INSIDE the fixed-point loop on the merged state for the entry
     * block — pre-loop registration on block_states[0] would be
     * overwritten when the loop reinitializes merged for entry. */

    /* Process blocks in order (topological for forward edges).
     * For back edges (loops), use fixed-point iteration.
     *
     * FAIL-CLOSED (BUG-fix from Gemini audit 2026-04-21): if the fixed
     * point doesn't converge within the bound, emit a compile error
     * rather than silently accepting a partial/incorrect safety state.
     * The lattice is finite (5 states × N handles), so 32 iterations is
     * plenty for any realistic program. Convergence failure means the
     * program is pathologically complex; we refuse to compile it rather
     * than miss a potential UAF.
     *
     * BUG-600 error-spam fix: fixed-point loop can visit each block up
     * to 32 times. Without suppression, ir_check_inst emits the same
     * error on every iteration (30+ duplicates on adversarial tests).
     * Fix: suppress via building_summary during convergence, then run
     * one more pass on the converged state with errors enabled. */
    bool saved_suppress = zc->building_summary;
    zc->building_summary = true;

    bool changed = true;
    int iterations = 0;
    const int MAX_ITERATIONS = 32;
    while (changed && iterations < MAX_ITERATIONS) {
        changed = false;
        iterations++;

        for (int bi = 0; bi < func->block_count; bi++) {
            IRBlock *bb = &func->blocks[bi];

            /* Merge predecessor states */
            IRPathState merged;
            if (bb->pred_count == 0) {
                /* Phase E: dead-code-after-return blocks have no
                 * predecessors (the preceding return terminated flow).
                 * Inherit state from the previous block in IR order so
                 * zercheck can catch use-after-move on statements that
                 * come after a return. Matches AST linear-scan behavior.
                 * Entry block (bi == 0) still gets empty state. */
                if (bi > 0 && func->blocks[bi - 1].inst_count > 0) {
                    IRInst *prev_last = &func->blocks[bi - 1].insts[
                        func->blocks[bi - 1].inst_count - 1];
                    if (prev_last->op == IR_RETURN) {
                        merged = ir_ps_copy(&block_states[bi - 1]);
                    } else {
                        ir_ps_init(&merged);
                    }
                } else {
                    ir_ps_init(&merged); /* entry block — empty state */
                    /* F0.5 (2026-05-03): register param nested-handle
                     * fields on the entry block's merged state. Done
                     * here (not on block_states[0] before the loop)
                     * because the merged state replaces block_states[0]
                     * each iteration. */
                    if (bi == 0 && func->ast_node &&
                        func->ast_node->kind == NODE_FUNC_DECL) {
                        Node *fn = func->ast_node;
                        for (int pi = 0; pi < fn->func_decl.param_count; pi++) {
                            ParamDecl *pp = &fn->func_decl.params[pi];
                            if (!pp->type || pp->type->kind != TYNODE_NAMED) continue;
                            Symbol *type_sym = scope_lookup(zc->checker->global_scope,
                                pp->type->named.name,
                                (uint32_t)pp->type->named.name_len);
                            if (!type_sym || !type_sym->type) continue;
                            Type *st = type_unwrap_distinct(type_sym->type);
                            if (!st || (st->kind != TYPE_STRUCT &&
                                        st->kind != TYPE_UNION)) continue;
                            int plocal = ir_find_local_exact_first(func,
                                pp->name, (uint32_t)pp->name_len);
                            if (plocal < 0) continue;
                            ir_register_nested_handles(&merged, zc->arena,
                                plocal, st, "", 0, 0);
                        }
                    }
                }
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
            zc->gr_cur_block = bi;
            for (int ii = 0; ii < bb->inst_count; ii++) {
                ir_check_inst(zc, &merged, &bb->insts[ii], func);
            }
            /* Level B: tag handles freed IN this block (state FREED, no
             * free_block yet) with bi. Inherited-freed handles already carry
             * their origin block; MAYBE_FREED handles get free_block via the
             * merge carry. free_block is NOT part of the convergence check
             * (state only), so this can't perturb the fixed point. */
            for (int hi = 0; hi < merged.handle_count; hi++) {
                if (merged.handles[hi].state == IR_HS_FREED &&
                    merged.handles[hi].free_block < 0) {
                    merged.handles[hi].free_block = bi;
                }
            }

            /* Check if state changed (for fixed-point convergence).
             *
             * F0.3 (2026-05-03): use ir_find_compound_handle (compound-
             * aware), not ir_find_handle (bare-only). When merged
             * contains compound handles (e.g., s.top from struct field
             * tracking), bare-only lookup returns the wrong entry or
             * NULL — comparison spuriously says "changed" and the
             * fixed-point never converges.
             *
             * This was causing 4 false positives in audit:
             *   data_structures.zer (Pool with linked Handle field)
             *   move_array_safe.zer (move-tracked array elements)
             *   orelse_block_ptr.zer (orelse block creating compound state)
             *   super_sensor_logger.zer (Slab with nested Handle fields)
             * All hit the iteration cap not because they need many
             * iterations, but because the convergence check was broken. */
            if (merged.handle_count != block_states[bi].handle_count) {
                changed = true;
            } else {
                for (int hi = 0; hi < merged.handle_count; hi++) {
                    IRHandleInfo *mh = &merged.handles[hi];
                    IRHandleInfo *oh = ir_find_compound_handle(
                        &block_states[bi], mh->local_id, mh->path, mh->path_len);
                    if (!oh || oh->state != mh->state) {
                        changed = true;
                        break;
                    }
                }
            }

            /* Axis-C fix BUG-743 (2026-06-21): the fixed point must also
             * track ThreadHandle join obligations. Without this, a thread
             * whose `joined` flips across a back-edge merge (but whose
             * handle set is unchanged) is missed and the analysis
             * "converges" prematurely on a stale threads[] — re-opening the
             * dropped-obligation hole through loops. Compare thread_count
             * and per-thread `joined` by name. */
            if (!changed) {
                if (merged.thread_count != block_states[bi].thread_count) {
                    changed = true;
                } else {
                    for (int ti = 0; ti < merged.thread_count; ti++) {
                        IRThreadTrack *mt = &merged.threads[ti];
                        IRThreadTrack *ot = ir_find_thread(
                            &block_states[bi], mt->name, mt->name_len);
                        if (!ot || ot->joined != mt->joined) {
                            changed = true;
                            break;
                        }
                    }
                }
            }

            /* Update block state */
            ir_ps_free(&block_states[bi]);
            block_states[bi] = merged;
        }
    }

    /* BUG-600: restore suppression state and run ONE final pass on the
     * converged state with errors enabled. State is stable (no !changed
     * for full pass), so re-running produces exactly the same final
     * errors, each emitted once instead of once per iteration. */
    zc->building_summary = saved_suppress;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];

        IRPathState merged;
        if (bb->pred_count == 0) {
            if (bi > 0 && func->blocks[bi - 1].inst_count > 0) {
                IRInst *prev_last = &func->blocks[bi - 1].insts[
                    func->blocks[bi - 1].inst_count - 1];
                if (prev_last->op == IR_RETURN) {
                    merged = ir_ps_copy(&block_states[bi - 1]);
                } else {
                    ir_ps_init(&merged);
                }
            } else {
                ir_ps_init(&merged);
                /* F0.5 (2026-05-03): final pass also needs param
                 * nested-handle registration on entry block. Without
                 * this, the post-fixed-point pass sees empty state for
                 * entry block and fails to detect UAF on nested
                 * handles. Same logic as the fixed-point loop's entry
                 * init above. */
                if (bi == 0 && func->ast_node &&
                    func->ast_node->kind == NODE_FUNC_DECL) {
                    Node *fn = func->ast_node;
                    for (int pi = 0; pi < fn->func_decl.param_count; pi++) {
                        ParamDecl *pp = &fn->func_decl.params[pi];
                        if (!pp->type || pp->type->kind != TYNODE_NAMED) continue;
                        Symbol *type_sym = scope_lookup(zc->checker->global_scope,
                            pp->type->named.name,
                            (uint32_t)pp->type->named.name_len);
                        if (!type_sym || !type_sym->type) continue;
                        Type *st = type_unwrap_distinct(type_sym->type);
                        if (!st || (st->kind != TYPE_STRUCT &&
                                    st->kind != TYPE_UNION)) continue;
                        int plocal = ir_find_local_exact_first(func,
                            pp->name, (uint32_t)pp->name_len);
                        if (plocal < 0) continue;
                        ir_register_nested_handles(&merged, zc->arena,
                            plocal, st, "", 0, 0);
                    }
                }
            }
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

        zc->gr_cur_block = bi;
        for (int ii = 0; ii < bb->inst_count; ii++) {
            ir_check_inst(zc, &merged, &bb->insts[ii], func);
        }
        /* Level B: tag handles freed in this block (the final pass also writes
         * block_states[bi], read by later blocks' merges in this same pass). */
        for (int hi = 0; hi < merged.handle_count; hi++) {
            if (merged.handles[hi].state == IR_HS_FREED &&
                merged.handles[hi].free_block < 0) {
                merged.handles[hi].free_block = bi;
            }
        }

        ir_ps_free(&block_states[bi]);
        block_states[bi] = merged;
    }

    /* FAIL-CLOSED: if the fixed point didn't converge, emit a compile
     * error. Skip during summary-building (would add spurious errors
     * for partial analysis). */
    if (changed && iterations >= MAX_ITERATIONS && !saved_suppress) {
        int fail_line = func->ast_node ? func->ast_node->loc.line : 0;
        ir_zc_error(zc, fail_line,
            "safety analysis did not converge within %d iterations — program too complex "
            "for path-sensitive analysis. Simplify control flow (fewer loops / "
            "deeper nesting / backward gotos) or file a bug.",
            MAX_ITERATIONS);
    }

    /* Phase C1: FuncSummary build / refine. When building a summary, examine
     * final state of param locals at each return block. Union across blocks:
     *   - FREED in every block with a return → frees_param[i] = true
     *   - FREED or MAYBE_FREED in some returns → maybe_frees_param[i] = true
     *   - never FREED/MAYBE_FREED → both false
     * Summary is attached to ZerCheck via find_summary / allocation loop
     * identical to zercheck.c so consumers (zc_apply_summary in both paths)
     * see the same shape. */
    if (getenv("IR_SUMMARY_DEBUG") && func->ast_node &&
        func->ast_node->kind == NODE_FUNC_DECL) {
        fprintf(stderr, "ZCIR: building=%d fn='%.*s' pc=%d blocks=%d sumcount=%d\n",
            zc->building_summary,
            func->ast_node->func_decl.name_len,
            func->ast_node->func_decl.name ? func->ast_node->func_decl.name : "?",
            func->ast_node->func_decl.param_count, func->block_count,
            zc->summary_count);
        for (int si = 0; si < zc->summary_count; si++) {
            fprintf(stderr, "  SUMM[%d]: %.*s pc=%d frees=%s rc=%d\n", si,
                zc->summaries[si].func_name_len,
                zc->summaries[si].func_name ? zc->summaries[si].func_name : "?",
                zc->summaries[si].param_count,
                (zc->summaries[si].param_count > 0 && zc->summaries[si].frees_param && zc->summaries[si].frees_param[0]) ? "y" : "n",
                zc->summaries[si].returns_color);
        }
    }
    if (zc->building_summary && func->ast_node && func->ast_node->kind == NODE_FUNC_DECL) {
        Node *fn = func->ast_node;
        int pc = fn->func_decl.param_count;
        /* Phase F (2026-04-20): moved returns_color inference OUT of the
         * pc > 0 gate. Arena wrapper chain inference applies even to
         * zero-param functions: `?*T wrap() { return arena.alloc(T); }`
         * must have returns_color=ARENA to propagate through callers. */
        bool *frees = NULL;
        bool *maybe_frees = NULL;
        if (pc > 0) {
            frees = (bool *)calloc(pc, sizeof(bool));
            maybe_frees = (bool *)calloc(pc, sizeof(bool));
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
                    int plocal = ir_find_local_exact_first(func, p->name, (uint32_t)p->name_len);
                    if (plocal < 0) { all_return_blocks_freed[i] = false; continue; }
                    /* Use the RESOLVED Type from IR local rather than the syntactic
                     * TypeNode: `typedef *T TPtr; void destroy(TPtr p)` has
                     * tnode->kind == TYNODE_NAMED, but the resolved type is a
                     * pointer (or a TYPE_DISTINCT wrapping a pointer). Gating on
                     * TYNODE_POINTER/TYNODE_HANDLE silently skipped any typedef'd
                     * destructor parameter — frees_param never set, so caller's
                     * UAF/double-free both passed silently. */
                    Type *pt_eff = func->locals[plocal].type
                        ? type_unwrap_distinct(func->locals[plocal].type) : NULL;
                    if (!pt_eff ||
                        (pt_eff->kind != TYPE_POINTER &&
                         pt_eff->kind != TYPE_HANDLE &&
                         pt_eff->kind != TYPE_OPAQUE)) {
                        all_return_blocks_freed[i] = false;
                        continue;
                    }
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
        }

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
            if (last->op != IR_RETURN) continue;
            if (bb->is_orelse_fallback) continue;
            if (bb->is_early_exit) continue;
            /* Phase F: return can have src1_local (simple `return ident`
             * or `return call()` with result in local) OR expr (direct
             * NODE_IDENT / NODE_ORELSE for complex shapes). Check both. */
            int rlocal = -1;
            if (last->src1_local >= 0) {
                rlocal = last->src1_local;
            } else if (last->expr) {
                Node *ret_expr = last->expr;
                if (ret_expr->kind == NODE_ORELSE) ret_expr = ret_expr->orelse.expr;
                if (ret_expr && ret_expr->kind == NODE_IDENT) {
                    rlocal = ir_find_local_exact_first(func,
                        ret_expr->ident.name, (uint32_t)ret_expr->ident.name_len);
                }
            }
            if (rlocal < 0) { inferred_color = -2; break; }
            IRHandleInfo *rh = ir_find_handle(&block_states[bi], rlocal);
            int color = rh ? rh->source_color : ZC_COLOR_UNKNOWN;
            if (inferred_color == -1) inferred_color = color;
            else if (inferred_color != color) { inferred_color = -2; break; }
        }
        int returns_color_final =
            (inferred_color < 0) ? ZC_COLOR_UNKNOWN : inferred_color;

        /* Phase F: param-color inference. When the function returns a
         * cast of a param (e.g., `*T unwrap(*opaque raw) { return (*T)raw; }`),
         * the return is an ALIAS of the arg — callers should share the
         * arg's alloc_id, not register a fresh one. Mirrors zercheck.c's
         * returns_param_color + line 748-784 param-color inference.
         *
         * Detection: if returns_color_final is UNKNOWN AND every return
         * block returns a local that traces to a param (via cast/ptrcast
         * alias chain), set returns_param_color = param_index + 1. */
        int returns_param_color_final = -1;
        if (returns_color_final == ZC_COLOR_UNKNOWN && pc > 0) {
            int inferred_param = -2;  /* -2 unset, -1 mixed, >=0 param idx */
            for (int bi = 0; bi < func->block_count; bi++) {
                IRBlock *bb = &func->blocks[bi];
                if (bb->inst_count == 0) continue;
                IRInst *last = &bb->insts[bb->inst_count - 1];
                if (last->op != IR_RETURN) continue;
                if (bb->is_orelse_fallback) continue;
                if (bb->is_early_exit) continue;
                int rlocal = -1;
                if (last->src1_local >= 0) rlocal = last->src1_local;
                else if (last->expr) {
                    Node *re = last->expr;
                    if (re->kind == NODE_ORELSE) re = re->orelse.expr;
                    if (re && re->kind == NODE_IDENT) {
                        rlocal = ir_find_local_exact_first(func,
                            re->ident.name, (uint32_t)re->ident.name_len);
                    }
                }
                if (rlocal < 0) { inferred_param = -1; break; }
                /* Check if rlocal's handle shares alloc_id with any param.
                 * This captures the "return cast of param" alias pattern. */
                IRHandleInfo *rh = ir_find_handle(&block_states[bi], rlocal);
                if (!rh) { inferred_param = -1; break; }
                int match_param = -1;
                for (int pi = 0; pi < pc; pi++) {
                    ParamDecl *p = &fn->func_decl.params[pi];
                    int plocal = ir_find_local_exact_first(func,
                        p->name, (uint32_t)p->name_len);
                    if (plocal < 0) continue;
                    IRHandleInfo *ph = ir_find_handle(&block_states[bi], plocal);
                    if (!ph) continue;
                    if (ph->alloc_id == rh->alloc_id && ph->alloc_id != 0) {
                        match_param = pi; break;
                    }
                }
                if (match_param < 0) { inferred_param = -1; break; }
                if (inferred_param == -2) inferred_param = match_param;
                else if (inferred_param != match_param) { inferred_param = -1; break; }
            }
            if (inferred_param >= 0) returns_param_color_final = inferred_param + 1;
        }

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
                    if (existing->frees_param[i] != (frees ? frees[i] : false)) changed = true;
                    if (existing->maybe_frees_param[i] != (maybe_frees ? maybe_frees[i] : false)) changed = true;
                }
            } else {
                changed = true;
            }
            if (existing->returns_color != returns_color_final) changed = true;
            if (existing->returns_param_color != returns_param_color_final) changed = true;
            if (changed) {
                free(existing->frees_param);
                free(existing->maybe_frees_param);
                existing->param_count = pc;
                existing->frees_param = frees;
                existing->maybe_frees_param = maybe_frees;
                existing->returns_color = returns_color_final;
                existing->returns_param_color = returns_param_color_final;
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
                s->returns_param_color = returns_param_color_final;
            } else {
                free(frees); free(maybe_frees);
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
    /* plt86m audit 2026-06-17: also check defer-body USES (not just frees)
     * against each return block's PRISTINE exit state — a deferred USE of a
     * handle the body already freed / move-transferred is a use-after-free /
     * use-after-move. The uses-pass runs BEFORE the frees-pass below so a
     * `defer free(h); defer use(h)` (LIFO-valid: use fires first, against a
     * still-ALIVE h) is not false-flagged; the shared `defer_use_rs` dedups
     * reports per root-local across all return blocks. */
    UafReportSet defer_use_rs = {0};
    /* AU-1 (2026-07-01): defers fire in LIFO (reverse-registration) order at
     * scope exit. A `defer use(h)` registered BEFORE a `defer free(h)` therefore
     * fires AFTER it — the use sees a FREED handle (a real UAF). The old split
     * (all-uses against pristine state, THEN all-frees) missed this: it checked
     * every use before applying any free. Fix: collect defers in registration
     * order, then per return block process them in FIRE order (reverse) — for
     * each defer, check its USES against the current state, THEN apply its
     * FREES. So a use sees exactly the frees of later-registered defers (which
     * fire first). The safe shape `defer free(h); defer use(h)` (use fires first,
     * against ALIVE h) still passes — its free is applied after its use is
     * checked. Leak detection is unaffected (the FINAL state has every free
     * applied regardless of order). */
    IRInst **dfs = NULL; int dfn = 0, dfc = 0;
    for (int di = 0; di < func->block_count; di++) {
        IRBlock *db = &func->blocks[di];
        for (int dj = 0; dj < db->inst_count; dj++) {
            IRInst *inst = &db->insts[dj];
            if (inst->op != IR_DEFER_PUSH || !inst->defer_body) continue;
            if (dfn == dfc) {
                int ndc = dfc ? dfc * 2 : 8;
                IRInst **nd = (IRInst **)realloc(dfs, (size_t)ndc * sizeof(IRInst *));
                if (!nd) break;
                dfs = nd; dfc = ndc;
            }
            dfs[dfn++] = inst;
        }
    }
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        if (bb->inst_count == 0) continue;
        IRInst *last = &bb->insts[bb->inst_count - 1];
        if (last->op != IR_RETURN) continue;
        IRPathState *ret_ps = &block_states[bi];
        for (int k = dfn - 1; k >= 0; k--) {   /* LIFO fire order */
            ir_defer_scan_uses(zc, func, ret_ps, dfs[k]->defer_body,
                               dfs[k]->source_line, &defer_use_rs);
            ir_defer_scan_frees(zc, func, ret_ps, dfs[k]->defer_body,
                                dfs[k]->source_line);
        }
    }
    free(dfs);
    free(defer_use_rs.ids);

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
                    /* Recursive walk — manual stack to avoid runaway recursion
                     * on pathological ASTs.
                     * Stage 3 (2026-04-28): stack-first dynamic. Fixed 64-slot
                     * stack silently truncated walks for ASTs deeper than 64
                     * nodes; missed NODE_IDENTs in the truncated tail caused
                     * false-positive "unused local" leak reports. Stack 64,
                     * overflow to arena (doubling). */
                    Node *fast_stack[64];
                    Node **stack = fast_stack;
                    int top = 0;
                    int stk_cap = 64;
                    /* Helper: ensure room for `need` more slots before pushes */
                    #define ZER_STK_RESERVE(need_) do { \
                        int need__ = (need_); \
                        if (top + need__ > stk_cap) { \
                            int ncap = stk_cap * 2; \
                            while (ncap < top + need__) ncap *= 2; \
                            Node **nbuf = (Node **)arena_alloc(zc->arena, ncap * sizeof(Node *)); \
                            if (!nbuf) goto stk_overflow; \
                            memcpy(nbuf, stack, top * sizeof(Node *)); \
                            stack = nbuf; \
                            stk_cap = ncap; \
                        } \
                    } while (0)
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
                            ZER_STK_RESERVE(1);
                            stack[top++] = n->field.object; break;
                        case NODE_INDEX:
                            ZER_STK_RESERVE(2);
                            stack[top++] = n->index_expr.object;
                            stack[top++] = n->index_expr.index;
                            break;
                        case NODE_UNARY:
                            ZER_STK_RESERVE(1);
                            stack[top++] = n->unary.operand; break;
                        case NODE_BINARY:
                            ZER_STK_RESERVE(2);
                            stack[top++] = n->binary.left;
                            stack[top++] = n->binary.right;
                            break;
                        case NODE_CALL:
                            ZER_STK_RESERVE(1 + n->call.arg_count);
                            stack[top++] = n->call.callee;
                            for (int ai = 0; ai < n->call.arg_count; ai++)
                                stack[top++] = n->call.args[ai];
                            break;
                        case NODE_ASSIGN:
                            ZER_STK_RESERVE(2);
                            stack[top++] = n->assign.target;
                            stack[top++] = n->assign.value;
                            break;
                        case NODE_ORELSE:
                            ZER_STK_RESERVE(2);
                            stack[top++] = n->orelse.expr;
                            stack[top++] = n->orelse.fallback;
                            break;
                        case NODE_TYPECAST:
                            ZER_STK_RESERVE(1);
                            stack[top++] = n->typecast.expr; break;
                        case NODE_SLICE:
                            ZER_STK_RESERVE(3);
                            stack[top++] = n->slice.object;
                            stack[top++] = n->slice.start;
                            stack[top++] = n->slice.end;
                            break;
                        case NODE_INTRINSIC:
                            ZER_STK_RESERVE(n->intrinsic.arg_count);
                            for (int ai = 0; ai < n->intrinsic.arg_count; ai++)
                                stack[top++] = n->intrinsic.args[ai];
                            break;
                        case NODE_STRUCT_INIT:
                            ZER_STK_RESERVE(n->struct_init.field_count);
                            for (int fi = 0; fi < n->struct_init.field_count; fi++)
                                stack[top++] = n->struct_init.fields[fi].value;
                            break;
                        /* Stage 2 Part B (2026-04-28): exhaustive — leaf
                         * literals + non-expression kinds. Used-locals
                         * tracking only cares about NODE_IDENT references;
                         * leaves carry no ident, statement/decl kinds
                         * shouldn't appear in inst->expr. */
                        case NODE_INT_LIT: case NODE_FLOAT_LIT:
                        case NODE_STRING_LIT: case NODE_CHAR_LIT:
                        case NODE_BOOL_LIT: case NODE_NULL_LIT:
                        case NODE_CAST: case NODE_SIZEOF:
                        case NODE_FILE: case NODE_FUNC_DECL:
                        case NODE_STRUCT_DECL: case NODE_ENUM_DECL:
                        case NODE_UNION_DECL: case NODE_TYPEDEF:
                        case NODE_IMPORT: case NODE_CINCLUDE:
                        case NODE_INTERRUPT: case NODE_MMIO:
                        case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
                        case NODE_VAR_DECL: case NODE_BLOCK:
                        case NODE_IF: case NODE_FOR: case NODE_WHILE:
                        case NODE_SWITCH: case NODE_RETURN:
                        case NODE_BREAK: case NODE_CONTINUE:
                        case NODE_DEFER: case NODE_GOTO: case NODE_LABEL:
                        case NODE_EXPR_STMT: case NODE_ASM:
                        case NODE_CRITICAL: case NODE_ONCE:
                        case NODE_SPAWN: case NODE_YIELD: case NODE_AWAIT:
                        case NODE_DO_WHILE: case NODE_STATIC_ASSERT:
                            break;
                        }
                    }
                    stk_overflow: ;  /* arena_alloc failure: stop walk early */
                    #undef ZER_STK_RESERVE
                }
            }
        }
    }

    /* Phase E: alloc_id-grouped leak detection with early-exit skip.
     *
     * Coverage collection: iterate all RETURN blocks, collect alloc_ids
     * that are freed/transferred/escaped. Skip:
     *   - orelse-fallback (optional was null, nothing to leak)
     *   - early-exit (if-then-always-exits path, bypasses canonical flow)
     *
     * Mirrors zercheck.c's block_always_exits semantic. */
    int *covered_ids = NULL;
    int covered_cap = 0, covered_n = 0;
    for (int bi = 0; bi < func->block_count; bi++) {
        IRBlock *bb = &func->blocks[bi];
        if (bb->inst_count == 0) continue;
        if (bb->insts[bb->inst_count - 1].op != IR_RETURN) continue;
        if (bb->is_orelse_fallback) continue;
        if (bb->is_early_exit) continue;
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
        if (bb->is_orelse_fallback) continue;
        /* Phase E: skip early-exit blocks for leak checking too —
         * they represent conditional bypass paths whose state isn't
         * canonical. The fall-through return holds the authoritative
         * leak state. */
        if (bb->is_early_exit) continue;

        IRPathState *ps = &block_states[bi];
        for (int hi = 0; hi < ps->handle_count; hi++) {
            IRHandleInfo *h = &ps->handles[hi];
            /* Cross-function global UAF, exit rule (BUG-742, 2026-06-10,
             * follow-up from BUG-739): a GLOBAL pseudo-root entry that is
             * definitely FREED at a return block means the function returns
             * while the global points at freed memory — ANY later reader
             * (any function, any call depth) observes the dangle, which
             * per-function analysis cannot see. Closing it at the source
             * makes the cross-function case unrepresentable: no summaries
             * needed. The one-line fix is hygiene the rule teaches —
             * `g = null;` after the free (the BUG-739 store hook resets the
             * entry on null assignment). MAYBE_FREED is deliberately NOT
             * flagged here: BUG-740/741 widenings produce MAYBE on
             * legitimate hand-off patterns (register-ctx-then-callback);
             * definite dangles only. Checked BEFORE the escaped skip —
             * global entries always carry escaped=true by invariant. */
            if (h->local_id == IR_GLOBAL_ROOT_ID &&
                h->state == IR_HS_FREED && h->path_len > 0) {
                bool g_already = false;
                for (int ri = 0; ri < reported_n; ri++) {
                    if (reported_ids[ri] == h->alloc_id) { g_already = true; break; }
                }
                if (!g_already) {
                    ir_zc_error(zc, last->source_line,
                        "global '%.*s' left dangling at function exit — its "
                        "target was freed at line %d; reset it ('%.*s = null;') "
                        "after the free, or free through it before returning",
                        (int)h->path_len, h->path, h->free_line,
                        (int)h->path_len, h->path);
                    if (reported_n >= reported_cap) {
                        reported_cap = reported_cap < 8 ? 8 : reported_cap * 2;
                        int *nr = (int *)realloc(reported_ids,
                            reported_cap * sizeof(int));
                        if (nr) reported_ids = nr;
                    }
                    if (reported_n < reported_cap)
                        reported_ids[reported_n++] = h->alloc_id;
                }
                continue;
            }
            if (h->escaped) continue;
            if (h->source_color == ZC_COLOR_ARENA) continue;
            /* bh18_1b: move-local handle and its pointer aliases are not
             * allocations — skip (the move local itself is also caught by the
             * ir_should_track_move skip below; this also covers the `*T p = &a`
             * alias whose own type is a pointer, not a move struct). */
            if (h->is_move_local) continue;
            if (h->local_id >= 0 && h->local_id < func->local_count) {
                IRLocal *loc = &func->locals[h->local_id];
                Type *lt = loc->type;
                if (ir_should_track_move(lt)) continue;
                if (loc->is_temp) continue;
                if (loc->is_param) continue;
            }
            /* Compound entities (s.h, arr[0]): historically skipped wholesale,
             * which laundered real leaks — `b.h = gp.alloc()` never freed
             * compiled clean while the bare-handle equivalent was rejected
             * (found by tests/test_shape_matrix.c, 2026-06-07). Leak-check them
             * too, but only when they carry a real allocation origin
             * (source_color set + alloc_line known). Pure field-reads / param
             * struct-field registrations (BUG-385) have UNKNOWN color and are
             * skipped here — and param roots are already filtered above. The
             * escaped / covered-alloc_id / move / temp filters guard the rest. */
            if (h->path_len > 0 &&
                (h->source_color == ZC_COLOR_UNKNOWN || h->alloc_line <= 0))
                continue;

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
            } else if (h->state == IR_HS_MAYBE_FREED && !h->freed_all_paths) {
                /* Level B: skip if freed_all_paths — the handle was freed under
                 * a condition AND its exact singleton complement, so it is freed
                 * on every path despite the conservative MAYBE_FREED join.
                 * Phase E: MAYBE_FREED at non-fallback non-early-exit
                 * return block. With exhaustive switch fix (elide
                 * unreachable fallthrough) and is_early_exit tagging,
                 * spurious MAYBE_FREED from CFG merge conservatism is
                 * eliminated. Remaining MAYBE_FREED is genuine —
                 * handle freed on some paths but not all, like:
                 *   goto-loop with conditional free → MAYBE_FREED
                 *   after fixed-point.
                 * Matches zercheck.c:2700 "may not be freed on all paths". */
                ir_zc_error(zc, last->source_line,
                    "handle '%.*s' may not be freed on all paths — "
                    "ensure all branches free the handle or add 'defer' "
                    "for automatic cleanup",
                    (int)func->locals[h->local_id].name_len,
                    func->locals[h->local_id].name);
                if (reported_n >= reported_cap) {
                    reported_cap = reported_cap < 8 ? 8 : reported_cap * 2;
                    int *nr = (int *)realloc(reported_ids,
                        reported_cap * sizeof(int));
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
    free(block_guards);            /* Level B: per-block guard sets */
    zc->gr_block_guards = NULL;
    zc->gr_block_count = 0;

    return zc->error_count == 0;
}
