/* test_shape_matrix.c — systematic exhaustive shape×violation matrix for the
 * zercheck_ir safety analyzer (2026-06-07, "option A").
 *
 * Where test_semantic_fuzz.c SAMPLES randomly, this ENUMERATES the full grid
 * deterministically and asserts, for every cell:
 *   - the violating program is REJECTED  (the analyzer reaches the rule for
 *     this reach-shape — the GAP-A/B/C class was "analyzer missed the shape")
 *   - the safe counterpart COMPILES + RUNS (no over-rejection)
 *
 * The grid axes are C enums switched with NO default: adding a ShapeKind or
 * Violation value fails GCC -Wswitch at build time. That discipline is what
 * turns "testing" into proof-by-exhaustion over a finite domain — a cell can
 * never be silently dropped.
 *
 * Bug class targeted: compound-key reachability. The recent audit gaps were
 * all "entity reached via s.h / arr[0] / cross-function arg, analyzer tracked
 * only the bare ident." This harness makes every reach-shape a first-class,
 * permanently-checked cell.
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
typedef enum { SH_BARE, SH_FIELD, SH_ARRAY, SH_FNARG, SHAPE_COUNT } ShapeKind;
typedef enum { V_UAF, V_DOUBLE_FREE, V_LEAK, VIOL_COUNT } Violation;

static const char *shape_name(ShapeKind s) {
    switch (s) {
        case SH_BARE:  return "bare";
        case SH_FIELD: return "field(s.h)";
        case SH_ARRAY: return "array[0]";
        case SH_FNARG: return "fnarg(xfn)";
        case SHAPE_COUNT: break;
    }
    return "?";
}
static const char *viol_name(Violation v) {
    switch (v) {
        case V_UAF:         return "uaf";
        case V_DOUBLE_FREE: return "double-free";
        case V_LEAK:        return "leak";
        case VIOL_COUNT: break;
    }
    return "?";
}

/* Per-shape: the prologue that declares+allocates the slot, and the `ref`
 * token used to reach it. -Wswitch (no default) enforces completeness when a
 * new ShapeKind is added. */
static void shape_prologue(ShapeKind s, char *out, size_t n, const char **ref) {
    switch (s) {
        case SH_BARE:
        case SH_FNARG:
            snprintf(out, n, "    Handle(T) h = gp.alloc() orelse return;\n");
            *ref = "h";
            return;
        case SH_FIELD:
            snprintf(out, n, "    Box b;\n    b.h = gp.alloc() orelse return;\n");
            *ref = "b.h";
            return;
        case SH_ARRAY:
            snprintf(out, n, "    Handle(T)[2] arr;\n    arr[0] = gp.alloc() orelse return;\n");
            *ref = "arr[0]";
            return;
        case SHAPE_COUNT: break;
    }
    out[0] = 0; *ref = "h";
}

/* The free statement. Only SH_FNARG routes through a cross-function helper
 * (exercises FuncSummary.frees_param — the GAP-A path). */
static void free_stmt(ShapeKind s, const char *ref, char *out, size_t n) {
    switch (s) {
        case SH_FNARG: snprintf(out, n, "    freeit(%s);\n", ref); return;
        case SH_BARE:
        case SH_FIELD:
        case SH_ARRAY: snprintf(out, n, "    gp.free(%s);\n", ref); return;
        case SHAPE_COUNT: break;
    }
    out[0] = 0;
}

static const char *DECLS =
    "struct T { u32 id; }\n"
    "struct Box { Handle(T) h; }\n"
    "Pool(T, 4) gp;\n"
    "void freeit(Handle(T) hh) { gp.free(hh); }\n";

/* Build a program for (shape, viol). neg=1 → violating, neg=0 → safe. */
static void gen(ShapeKind s, Violation v, int neg, char *buf, size_t n) {
    char prologue[256]; const char *ref = "h";
    char freed[128];
    shape_prologue(s, prologue, sizeof(prologue), &ref);
    free_stmt(s, ref, freed, sizeof(freed));

    char body[768];
    int p = 0;
    p += snprintf(body + p, sizeof(body) - p, "%s", prologue);

    switch (v) {
        case V_UAF:
            if (neg) {
                /* free, then use → use-after-free */
                p += snprintf(body + p, sizeof(body) - p, "%s", freed);
                p += snprintf(body + p, sizeof(body) - p, "    gp.get(%s).id = 5;\n", ref);
            } else {
                /* use, then free → safe */
                p += snprintf(body + p, sizeof(body) - p, "    gp.get(%s).id = 5;\n", ref);
                p += snprintf(body + p, sizeof(body) - p, "%s", freed);
            }
            break;
        case V_DOUBLE_FREE:
            p += snprintf(body + p, sizeof(body) - p, "%s", freed);
            if (neg) /* second (direct) free → double free */
                p += snprintf(body + p, sizeof(body) - p, "    gp.free(%s);\n", ref);
            break;
        case V_LEAK:
            if (!neg) /* free at end → no leak */
                p += snprintf(body + p, sizeof(body) - p, "%s", freed);
            /* neg: alloc only, never freed → leak (compile error) */
            break;
        case VIOL_COUNT: break;
    }

    snprintf(buf, n, "%si32 main() {\n%s    return 0;\n}\n", DECLS, body);
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== Shape×Violation matrix (Pool Handle) ===\n");
    fprintf(stderr, "    reach-shape × safety-violation, every cell neg+pos\n\n");

    /* Coverage matrix print + per-cell assertions. */
    char buf[2048];
    int grid_ok = 1;
    for (ShapeKind s = 0; s < SHAPE_COUNT; s++) {
        for (Violation v = 0; v < VIOL_COUNT; v++) {
            char nm[96];
            gen(s, v, 1, buf, sizeof(buf));
            snprintf(nm, sizeof(nm), "%s/%s/neg", shape_name(s), viol_name(v));
            int a = run_one(nm, buf, /*expect_fail=*/1);

            gen(s, v, 0, buf, sizeof(buf));
            snprintf(nm, sizeof(nm), "%s/%s/pos", shape_name(s), viol_name(v));
            int b = run_one(nm, buf, /*expect_fail=*/0);

            fprintf(stderr, "  [%-11s][%-12s] neg:%s pos:%s\n",
                    shape_name(s), viol_name(v),
                    a ? "ok" : "GAP", b ? "ok" : "GAP");
            if (!a || !b) grid_ok = 0;
        }
    }

    fprintf(stderr, "\n=== shape-matrix: %d/%d cells correct (%d failed) ===\n",
            passed, total, failed);
    if (!grid_ok) {
        fprintf(stderr, "Matrix has GAPs — a reach-shape the analyzer mishandles.\n");
        return 1;
    }
    return 0;
}
