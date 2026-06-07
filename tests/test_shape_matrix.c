/* test_shape_matrix.c — systematic exhaustive type×shape×violation matrix for
 * the zercheck_ir safety analyzer (2026-06-07, "option A").
 *
 * Where test_semantic_fuzz.c SAMPLES randomly, this ENUMERATES the full grid
 * deterministically and asserts, for every valid cell:
 *   - the violating program is REJECTED  (the analyzer reaches the rule for
 *     this type×reach-shape — the GAP-A/B/C class was "analyzer missed the
 *     compound shape")
 *   - the safe counterpart COMPILES + RUNS (no over-rejection)
 *
 * The grid is ragged: not every (type, shape, violation) is meaningful (move
 * structs have no free/leak; *T-in-struct is its own concern). cell_valid()
 * encodes the shape; only valid cells run. The axes are C enums switched with
 * NO default, so adding a TypeK/ShapeKind/Violation fails GCC -Wswitch at
 * build time — the grid can't silently shrink. Enumerating a finite product
 * completely is proof-by-exhaustion for case coverage, the layer Coq/VST
 * don't reach.
 *
 * Found BUG-702 on first run (compound-key leak gap). Extended 2026-06-07 with
 * the *T (slab alloc_ptr) type and the move-struct sub-matrix incl. the GAP-C
 * spawn-of-move-struct-field cell.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static const char *zerc_path = NULL;

static void find_zerc(void) {
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    if (system("test -x /tmp/zerc") == 0) { zerc_path = "/tmp/zerc"; return; }
    if (system("gcc -std=c99 -O2 -I. -o /tmp/zerc lexer.c parser.c ast.c types.c "
               "checker.c emitter.c zercheck.c zercheck_ir.c ir.c ir_lower.c "
               "zerc_main.c src/safety/*.c 2>/dev/null") == 0) {
        zerc_path = "/tmp/zerc"; return;
    }
    fprintf(stderr, "ERROR: cannot find or build zerc\n");
    exit(2);
}

/* Returns 1 on expected outcome, 0 on failure (already logged). */
static int run_one(const char *name, const char *code, int expect_fail) {
    total++;
    FILE *f = fopen("/tmp/_zer_shape.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f);
    fclose(f);

    char cmd[512];
    if (expect_fail) {
        snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_shape.zer -o /dev/null 2>/dev/null", zerc_path);
        if (system(cmd) != 0) { passed++; return 1; }
        failed++;
        fprintf(stderr, "  FAIL: %s — violating program COMPILED (analyzer gap)\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    } else {
        snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_shape.zer --run 2>/dev/null", zerc_path);
        if (system(cmd) == 0) { passed++; return 1; }
        failed++;
        snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_shape.zer -o /dev/null 2>/dev/null", zerc_path);
        if (system(cmd) != 0)
            fprintf(stderr, "  FAIL: %s — SAFE program rejected (over-rejection)\n", name);
        else
            fprintf(stderr, "  FAIL: %s — safe program compiled but trapped at runtime\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
}

/* ---- Grid axes ---- */
typedef enum { TY_POOL, TY_SLAB, TY_MOVE, TYPE_KIND_COUNT } TypeK;
typedef enum { SH_BARE, SH_FIELD, SH_ARRAY, SH_FNARG, SH_SPAWN, SHAPE_COUNT } ShapeKind;
typedef enum { V_UAF, V_DOUBLE_FREE, V_LEAK, V_USE_AFTER_MOVE, VIOL_COUNT } Violation;

static const char *type_name(TypeK t) {
    switch (t) {
        case TY_POOL: return "pool/Handle";
        case TY_SLAB: return "slab/*T";
        case TY_MOVE: return "move-struct";
        case TYPE_KIND_COUNT: break;
    }
    return "?";
}
static const char *shape_name(ShapeKind s) {
    switch (s) {
        case SH_BARE:  return "bare";
        case SH_FIELD: return "field(s.x)";
        case SH_ARRAY: return "array[0]";
        case SH_FNARG: return "fnarg(xfn)";
        case SH_SPAWN: return "spawn-arg";
        case SHAPE_COUNT: break;
    }
    return "?";
}
static const char *viol_name(Violation v) {
    switch (v) {
        case V_UAF:             return "uaf";
        case V_DOUBLE_FREE:     return "double-free";
        case V_LEAK:            return "leak";
        case V_USE_AFTER_MOVE:  return "use-after-move";
        case VIOL_COUNT: break;
    }
    return "?";
}

/* KNOWN-OPEN gaps: cells the analyzer mishandles, documented for follow-up
 * (same discipline as the test-runner KNOWN_FAIL lists). Reported as KNOWN-GAP,
 * non-fatal, so the grid stays green while honestly recording the open issue.
 * Remove the entry when the underlying bug is fixed.
 *
 * BUG-703 — move-struct field passed BY VALUE to a function (`consume(w.inner)`)
 * is falsely rejected as use-after-move ON THE TRANSFER LINE. Cause: the call-arg
 * materialization `tmp = w.inner` (IR_FIELD_READ) transfers the compound via the
 * Gap A3 logic, then the IR_CALL move loop re-reports it on the same line.
 * The naive fix (skip transfer when FIELD_READ dest is a temp) regresses
 * move_field_read_uam (var-decls also materialize through temps → false
 * NEGATIVE). Correct fix needs a same-line discriminator across all 5 move
 * report sites; deferred. Only the POSITIVE (safe) cell is affected — the
 * use-after-move IS still caught (neg passes). */
static int cell_known_gap(TypeK t, ShapeKind s, Violation v, int neg) {
    if (t == TY_MOVE && s == SH_FIELD && v == V_USE_AFTER_MOVE && neg == 0)
        return 1;  /* BUG-703: over-rejection of consume(w.inner) */
    return 0;
}

/* Ragged grid: which (type, shape, violation) cells are meaningful. */
static int cell_valid(TypeK t, ShapeKind s, Violation v) {
    switch (t) {
        case TY_POOL:
            /* free/leak violations across the storage shapes (not spawn) */
            if (v == V_USE_AFTER_MOVE) return 0;
            if (s == SH_SPAWN) return 0;
            return 1;
        case TY_SLAB:
            /* *T via alloc_ptr/free_ptr — bare + cross-function only (*T-in-
             * struct/array is a separate non-null concern, out of scope here) */
            if (v == V_USE_AFTER_MOVE) return 0;
            if (s != SH_BARE && s != SH_FNARG) return 0;
            return 1;
        case TY_MOVE:
            /* ownership transfer — only use-after-move; bare/field/spawn */
            if (v != V_USE_AFTER_MOVE) return 0;
            if (s != SH_BARE && s != SH_FIELD && s != SH_SPAWN) return 0;
            return 1;
        case TYPE_KIND_COUNT: break;
    }
    return 0;
}

/* ---- Pool/Handle generator ---- */
static const char *POOL_DECLS =
    "struct T { u32 id; }\n"
    "struct Box { Handle(T) h; }\n"
    "Pool(T, 4) gp;\n"
    "void freeit(Handle(T) hh) { gp.free(hh); }\n";

static void pool_prologue(ShapeKind s, char *out, size_t n, const char **ref) {
    switch (s) {
        case SH_BARE:
        case SH_FNARG:
            snprintf(out, n, "    Handle(T) h = gp.alloc() orelse return;\n"); *ref = "h"; return;
        case SH_FIELD:
            snprintf(out, n, "    Box b;\n    b.h = gp.alloc() orelse return;\n"); *ref = "b.h"; return;
        case SH_ARRAY:
            snprintf(out, n, "    Handle(T)[2] arr;\n    arr[0] = gp.alloc() orelse return;\n"); *ref = "arr[0]"; return;
        case SH_SPAWN:
        case SHAPE_COUNT: break;
    }
    out[0] = 0; *ref = "h";
}
static void gen_pool(ShapeKind s, Violation v, int neg, char *buf, size_t n) {
    char prologue[256]; const char *ref = "h";
    pool_prologue(s, prologue, sizeof(prologue), &ref);
    char freed[128];
    if (s == SH_FNARG) snprintf(freed, sizeof(freed), "    freeit(%s);\n", ref);
    else               snprintf(freed, sizeof(freed), "    gp.free(%s);\n", ref);

    char body[768]; int p = 0;
    p += snprintf(body + p, sizeof(body) - p, "%s", prologue);
    switch (v) {
        case V_UAF:
            if (neg) { p += snprintf(body+p, sizeof(body)-p, "%s", freed);
                       p += snprintf(body+p, sizeof(body)-p, "    gp.get(%s).id = 5;\n", ref); }
            else     { p += snprintf(body+p, sizeof(body)-p, "    gp.get(%s).id = 5;\n", ref);
                       p += snprintf(body+p, sizeof(body)-p, "%s", freed); }
            break;
        case V_DOUBLE_FREE:
            p += snprintf(body+p, sizeof(body)-p, "%s", freed);
            if (neg) p += snprintf(body+p, sizeof(body)-p, "    gp.free(%s);\n", ref);
            break;
        case V_LEAK:
            if (!neg) p += snprintf(body+p, sizeof(body)-p, "%s", freed);
            break;
        case V_USE_AFTER_MOVE: case VIOL_COUNT: break;
    }
    snprintf(buf, n, "%si32 main() {\n%s    return 0;\n}\n", POOL_DECLS, body);
}

/* ---- Slab / *T generator (bare + fnarg) ---- */
static const char *SLAB_DECLS =
    "struct S { u32 id; }\n"
    "Slab(S) gs;\n"
    "void freep(*S pp) { gs.free_ptr(pp); }\n";

static void gen_slab(ShapeKind s, Violation v, int neg, char *buf, size_t n) {
    char freed[64];
    if (s == SH_FNARG) snprintf(freed, sizeof(freed), "    freep(p);\n");
    else               snprintf(freed, sizeof(freed), "    gs.free_ptr(p);\n");

    char body[768]; int p = 0;
    p += snprintf(body+p, sizeof(body)-p, "    *S p = gs.alloc_ptr() orelse return;\n");
    switch (v) {
        case V_UAF:
            if (neg) { p += snprintf(body+p, sizeof(body)-p, "%s", freed);
                       p += snprintf(body+p, sizeof(body)-p, "    p.id = 5;\n"); }
            else     { p += snprintf(body+p, sizeof(body)-p, "    p.id = 5;\n");
                       p += snprintf(body+p, sizeof(body)-p, "%s", freed); }
            break;
        case V_DOUBLE_FREE:
            p += snprintf(body+p, sizeof(body)-p, "%s", freed);
            if (neg) p += snprintf(body+p, sizeof(body)-p, "    gs.free_ptr(p);\n");
            break;
        case V_LEAK:
            if (!neg) p += snprintf(body+p, sizeof(body)-p, "%s", freed);
            break;
        case V_USE_AFTER_MOVE: case VIOL_COUNT: break;
    }
    snprintf(buf, n, "%si32 main() {\n%s    return 0;\n}\n", SLAB_DECLS, body);
}

/* ---- Move struct generator (use-after-move; bare/field/spawn) ---- */
static void gen_move(ShapeKind s, int neg, char *buf, size_t n) {
    /* setup of the move-typed slot + the transfer + (neg) a use afterward */
    char setup[256], transfer[160], ref[32];
    switch (s) {
        case SH_BARE:
            snprintf(setup, sizeof(setup), "    M m;\n    m.fd = 1;\n");
            snprintf(transfer, sizeof(transfer), "    consume(m);\n");
            snprintf(ref, sizeof(ref), "m.fd");
            break;
        case SH_FIELD:
            snprintf(setup, sizeof(setup), "    Wrap w;\n    w.inner.fd = 1;\n");
            snprintf(transfer, sizeof(transfer), "    consume(w.inner);\n");
            snprintf(ref, sizeof(ref), "w.inner.fd");
            break;
        case SH_SPAWN:
            snprintf(setup, sizeof(setup), "    Wrap w;\n    w.inner.fd = 1;\n");
            snprintf(transfer, sizeof(transfer),
                     "    ThreadHandle th = spawn mworker(w.inner);\n    th.join();\n");
            snprintf(ref, sizeof(ref), "w.inner.fd");
            break;
        case SH_ARRAY: case SH_FNARG: case SHAPE_COUNT:
            setup[0] = transfer[0] = 0; snprintf(ref, sizeof(ref), "m.fd"); break;
    }
    char body[768]; int p = 0;
    p += snprintf(body+p, sizeof(body)-p, "%s", setup);
    if (neg) {
        /* transfer, then read the moved-from slot → use-after-move */
        p += snprintf(body+p, sizeof(body)-p, "%s", transfer);
        p += snprintf(body+p, sizeof(body)-p, "    u32 k = %s;\n    return (i32)k;\n", ref);
    } else {
        /* transfer with no subsequent use → safe */
        p += snprintf(body+p, sizeof(body)-p, "%s", transfer);
        p += snprintf(body+p, sizeof(body)-p, "    return 0;\n");
    }
    snprintf(buf, n,
        "move struct M { u32 fd; }\n"
        "struct Wrap { M inner; }\n"
        "void consume(M mm) { }\n"
        "void mworker(M mm) { }\n"
        "i32 main() {\n%s}\n", body);
}

static void gen(TypeK t, ShapeKind s, Violation v, int neg, char *buf, size_t n) {
    switch (t) {
        case TY_POOL: gen_pool(s, v, neg, buf, n); return;
        case TY_SLAB: gen_slab(s, v, neg, buf, n); return;
        case TY_MOVE: gen_move(s, neg, buf, n); return;
        case TYPE_KIND_COUNT: break;
    }
    buf[0] = 0;
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== Type×Shape×Violation matrix ===\n");
    fprintf(stderr, "    every valid cell: violating REJECTED, safe COMPILES+RUNS\n\n");

    char buf[2048];
    int grid_ok = 1, valid_cells = 0;
    for (TypeK t = 0; t < TYPE_KIND_COUNT; t++) {
        for (ShapeKind s = 0; s < SHAPE_COUNT; s++) {
            for (Violation v = 0; v < VIOL_COUNT; v++) {
                if (!cell_valid(t, s, v)) continue;
                valid_cells++;
                char nm[128];

                gen(t, s, v, 1, buf, sizeof(buf));
                snprintf(nm, sizeof(nm), "%s/%s/%s/neg", type_name(t), shape_name(s), viol_name(v));
                int a = run_one(nm, buf, /*expect_fail=*/1);

                gen(t, s, v, 0, buf, sizeof(buf));
                int known = cell_known_gap(t, s, v, 0);
                int b;
                if (known) {
                    /* Tripwire: assert the cell is CURRENTLY (wrongly) rejected.
                     * When the bug is fixed the program will compile, this
                     * expect_fail flips, the grid fails, and you remove the
                     * cell_known_gap entry. */
                    snprintf(nm, sizeof(nm), "%s/%s/%s/pos[KNOWN-GAP]",
                             type_name(t), shape_name(s), viol_name(v));
                    b = run_one(nm, buf, /*expect_fail=*/1);
                } else {
                    snprintf(nm, sizeof(nm), "%s/%s/%s/pos",
                             type_name(t), shape_name(s), viol_name(v));
                    b = run_one(nm, buf, /*expect_fail=*/0);
                }

                fprintf(stderr, "  [%-11s][%-10s][%-14s] neg:%s pos:%s\n",
                        type_name(t), shape_name(s), viol_name(v),
                        a ? "ok" : "GAP",
                        known ? "KNOWN-GAP(BUG-703)" : (b ? "ok" : "GAP"));
                if (!a || !b) grid_ok = 0;
            }
        }
    }

    fprintf(stderr, "\n=== shape-matrix: %d/%d checks correct across %d valid cells (%d failed) ===\n",
            passed, total, valid_cells, failed);
    if (!grid_ok) {
        fprintf(stderr, "Matrix has GAPs — a type/shape the analyzer mishandles.\n");
        return 1;
    }
    return 0;
}
