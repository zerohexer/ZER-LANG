/* test_cflow_matrix.c — control-flow / path-sensitivity soundness oracle
 * (2026-06-07).
 *
 * The shape/escape/keep matrices are all STRAIGHT-LINE (alloc; free; use in
 * sequence). They never wrap the ops in if / else / loop / switch-arm / defer /
 * early-exit. But the analyzer's hardest, most bug-prone code is exactly the CFG
 * merge + loop fixed-point that handles those (the whole Phase-E dual-run 257->0
 * effort was about getting merges right). This oracle crosses a control-flow
 * context with memory-safety violations to flush out CFG-merge holes.
 *
 * TWO cell kinds (CFG-merge bugs manifest BOTH ways):
 *   - NEGATIVE: an unsafe op-in-control-flow (free-in-one-branch + use-after =
 *     MAYBE_FREED; free-in-both = FREED; free-in-loop = cross-iter UAF;
 *     alloc-in-loop-no-free = leak; double-free across a branch) MUST be rejected
 *     for a memory-safety reason. Compiling clean = false negative = the merge
 *     under-approximated (the unacceptable class).
 *   - POSITIVE: a SAFE control-flow pattern (free+return in a branch then use
 *     after; alloc+use+free balanced inside a loop or branch) MUST compile.
 *     Rejection = over-rejection = the merge over-approximated (acceptable per
 *     the criterion, but still a regression worth catching — Phase E spent most
 *     of its effort killing exactly these false positives).
 *
 * INTEGRITY GUARD: a NEGATIVE rejection only counts if the error names a
 * memory-safety reason (use-after-free / double free / never freed / leaked /
 * transferred). A parse/type error masking the check is flagged INVALID.
 *
 * Axes are C enums switched with NO default — adding a CFType/CFScenario fails
 * GCC -Wswitch until handled. The grid can't silently shrink.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static int false_neg = 0, invalid_probe = 0, over_reject = 0;
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

/* NEGATIVE: must reject for a memory-safety reason. Returns 1 ok, 0 otherwise. */
static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_cf.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_cf.zer -o /dev/null 2>/tmp/_zer_cf.err", zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — unsafe control-flow COMPILED CLEAN\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_cf.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not a safety check\n", name);
        fprintf(stderr, "    %.110s\n", eb);
        return 0;
    }
    if (strstr(eb, "use after free") || strstr(eb, "double free") ||
        strstr(eb, "already freed") || strstr(eb, "may already be freed") ||
        strstr(eb, "never freed") || strstr(eb, "leaked") ||
        strstr(eb, "transferred") || strstr(eb, "ownership already moved")) {
        passed++; return 1;
    }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for a memory-safety reason:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    return 0;
}

/* POSITIVE: a safe control-flow pattern must compile. Returns 1 ok, 0 over-reject. */
static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_cf.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_cf.zer -o /dev/null 2>/tmp/_zer_cf.err", zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_cf.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — safe control-flow pattern REJECTED:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

/* ---- Grid axes ---- */
typedef enum { CFT_POOL, CFT_SLAB, CFTYPE_COUNT } CFType;
typedef enum {
    /* NEGATIVE (unsafe) */
    CF_IF_THEN_USE,      /* free in then only, use after          -> MAYBE_FREED */
    CF_IF_BOTH_USE,      /* free in both branches, use after      -> FREED       */
    CF_LOOP_USE,         /* free in loop body, use after loop     -> MAYBE_FREED */
    CF_LOOP_NEXT_ITER,   /* use-then-free in loop (iter 2 = UAF)  -> cross-iter  */
    CF_SWITCH_ONE_USE,   /* free in one arm, use after switch     -> MAYBE_FREED */
    CF_SWITCH_ALL_USE,   /* free in every arm, use after switch   -> FREED       */
    CF_DOUBLE_IF,        /* free in then, free again after        -> double-free */
    CF_LEAK_IF,          /* alloc in then, never freed            -> leak        */
    CF_LEAK_LOOP,        /* alloc in loop, never freed            -> leak        */
    CF_NESTED_IF_USE,    /* free in nested if, use after          -> MAYBE_FREED */
    CF_BREAK_USE,        /* free+break in loop, use after loop    -> MAYBE_FREED */
    CF_CONTINUE_NEXT,    /* free+continue iter0, use iter1        -> cross-iter  */
    CF_DEFER_DOUBLE,     /* defer free + explicit free            -> double-free */
    /* POSITIVE (safe) */
    CF_IF_THEN_RET,      /* free+return in then, use after        -> safe        */
    CF_LOOP_BALANCED,    /* alloc+use+free each iteration         -> safe        */
    CF_IF_BALANCED,      /* alloc+use+free inside then branch     -> safe        */
    CF_DEFER_SAFE,       /* defer free, use before exit           -> safe        */
    CF_DEFER_RET,        /* defer free + early return + use       -> safe        */
    CF_BREAK_BALANCED,   /* alloc+use+free+break in loop          -> safe        */
    CFSCEN_COUNT
} CFScenario;

static int scenario_is_negative(CFScenario s) {
    switch (s) {
        case CF_IF_THEN_USE: case CF_IF_BOTH_USE: case CF_LOOP_USE:
        case CF_LOOP_NEXT_ITER: case CF_SWITCH_ONE_USE: case CF_SWITCH_ALL_USE:
        case CF_DOUBLE_IF: case CF_LEAK_IF: case CF_LEAK_LOOP:
        case CF_NESTED_IF_USE: case CF_BREAK_USE: case CF_CONTINUE_NEXT:
        case CF_DEFER_DOUBLE:
            return 1;
        case CF_IF_THEN_RET: case CF_LOOP_BALANCED: case CF_IF_BALANCED:
        case CF_DEFER_SAFE: case CF_DEFER_RET: case CF_BREAK_BALANCED:
            return 0;
        case CFSCEN_COUNT: break;
    }
    return 1;
}

static const char *type_name(CFType t) {
    switch (t) {
        case CFT_POOL: return "pool";
        case CFT_SLAB: return "slab";
        case CFTYPE_COUNT: break;
    }
    return "?";
}
static const char *scen_name(CFScenario s) {
    switch (s) {
        case CF_IF_THEN_USE:    return "if-then/use-after";
        case CF_IF_BOTH_USE:    return "if-both/use-after";
        case CF_LOOP_USE:       return "loop-free/use-after";
        case CF_LOOP_NEXT_ITER: return "loop/next-iter-uaf";
        case CF_SWITCH_ONE_USE: return "switch-one/use-after";
        case CF_SWITCH_ALL_USE: return "switch-all/use-after";
        case CF_DOUBLE_IF:      return "double-free-via-if";
        case CF_LEAK_IF:        return "leak-in-if";
        case CF_LEAK_LOOP:      return "leak-in-loop";
        case CF_NESTED_IF_USE:  return "nested-if/use-after";
        case CF_BREAK_USE:      return "break-free/use-after";
        case CF_CONTINUE_NEXT:  return "continue/next-iter-uaf";
        case CF_DEFER_DOUBLE:   return "defer+explicit-double";
        case CF_IF_THEN_RET:    return "if-then-return/use";
        case CF_LOOP_BALANCED:  return "loop-balanced";
        case CF_IF_BALANCED:    return "if-balanced";
        case CF_DEFER_SAFE:     return "defer-free/use";
        case CF_DEFER_RET:      return "defer+early-return/use";
        case CF_BREAK_BALANCED: return "break-balanced";
        case CFSCEN_COUNT: break;
    }
    return "?";
}

static void gen(CFType t, CFScenario s, char *buf, size_t n) {
    int pool = (t == CFT_POOL);
    const char *decl = pool ? "Pool(Slot, 16) gp;\n" : "Slab(Slot) gs;\n";
    const char *A = pool ? "Handle(Slot) h = gp.alloc() orelse return;"
                         : "*Slot p = gs.alloc_ptr() orelse return;";
    const char *F = pool ? "gp.free(h);" : "gs.free_ptr(p);";
    const char *U = pool ? "h.v = 1;" : "p.v = 1;";
    char body[1024];
    switch (s) {
        case CF_IF_THEN_USE:
            snprintf(body, sizeof(body),
                "    %s\n    if (c > 0) { %s }\n    %s\n", A, F, U);
            break;
        case CF_IF_BOTH_USE:
            snprintf(body, sizeof(body),
                "    %s\n    if (c > 0) { %s } else { %s }\n    %s\n", A, F, F, U);
            break;
        case CF_LOOP_USE:
            snprintf(body, sizeof(body),
                "    %s\n    for (u32 i = 0; i < c; i += 1) { %s }\n    %s\n", A, F, U);
            break;
        case CF_LOOP_NEXT_ITER:
            snprintf(body, sizeof(body),
                "    %s\n    for (u32 i = 0; i < c; i += 1) {\n        %s\n        %s\n    }\n", A, U, F);
            break;
        case CF_SWITCH_ONE_USE:
            snprintf(body, sizeof(body),
                "    %s\n    switch (c) {\n        0 => { %s }\n        default => { }\n    }\n    %s\n", A, F, U);
            break;
        case CF_SWITCH_ALL_USE:
            snprintf(body, sizeof(body),
                "    %s\n    switch (c) {\n        0 => { %s }\n        default => { %s }\n    }\n    %s\n", A, F, F, U);
            break;
        case CF_DOUBLE_IF:
            snprintf(body, sizeof(body),
                "    %s\n    if (c > 0) { %s }\n    %s\n", A, F, F);
            break;
        case CF_LEAK_IF:
            snprintf(body, sizeof(body),
                "    if (c > 0) {\n        %s\n        %s\n    }\n", A, U);
            break;
        case CF_LEAK_LOOP:
            snprintf(body, sizeof(body),
                "    for (u32 i = 0; i < c; i += 1) {\n        %s\n        %s\n    }\n", A, U);
            break;
        case CF_NESTED_IF_USE:
            snprintf(body, sizeof(body),
                "    %s\n    if (c > 0) { if (c > 1) { %s } }\n    %s\n", A, F, U);
            break;
        case CF_BREAK_USE:
            snprintf(body, sizeof(body),
                "    %s\n    for (u32 i = 0; i < c; i += 1) { %s break; }\n    %s\n", A, F, U);
            break;
        case CF_CONTINUE_NEXT:
            snprintf(body, sizeof(body),
                "    %s\n    for (u32 i = 0; i < c; i += 1) {\n        if (i == 0) { %s continue; }\n        %s\n    }\n", A, F, U);
            break;
        case CF_DEFER_DOUBLE:
            snprintf(body, sizeof(body),
                "    %s\n    defer %s\n    %s\n", A, F, F);
            break;
        case CF_IF_THEN_RET:
            snprintf(body, sizeof(body),
                "    %s\n    if (c > 0) { %s return; }\n    %s\n    %s\n", A, F, U, F);
            break;
        case CF_LOOP_BALANCED:
            snprintf(body, sizeof(body),
                "    for (u32 i = 0; i < c; i += 1) {\n        %s\n        %s\n        %s\n    }\n", A, U, F);
            break;
        case CF_IF_BALANCED:
            snprintf(body, sizeof(body),
                "    if (c > 0) {\n        %s\n        %s\n        %s\n    }\n", A, U, F);
            break;
        case CF_DEFER_SAFE:
            snprintf(body, sizeof(body),
                "    %s\n    defer %s\n    %s\n", A, F, U);
            break;
        case CF_DEFER_RET:
            snprintf(body, sizeof(body),
                "    %s\n    defer %s\n    if (c > 0) { %s return; }\n    %s\n", A, F, U, U);
            break;
        case CF_BREAK_BALANCED:
            snprintf(body, sizeof(body),
                "    for (u32 i = 0; i < c; i += 1) {\n        %s\n        %s\n        %s\n        if (i > 3) { break; }\n    }\n", A, U, F);
            break;
        case CFSCEN_COUNT: body[0] = 0; break;
    }
    snprintf(buf, n,
        "struct Slot { u32 v; }\n%s"
        "void f(u32 c) {\n%s}\n"
        "i32 main() { f(1); return 0; }\n", decl, body);
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== Control-flow / path-sensitivity matrix ===\n");
    fprintf(stderr, "    NEG: unsafe op-in-control-flow -> must REJECT for a memory-safety reason\n");
    fprintf(stderr, "    POS: safe control-flow merge pattern -> must COMPILE\n\n");

    char buf[2048];
    int grid_ok = 1, valid_cells = 0;
    for (CFType t = 0; t < CFTYPE_COUNT; t++) {
        for (CFScenario s = 0; s < CFSCEN_COUNT; s++) {
            valid_cells++;
            char nm[160];
            int neg = scenario_is_negative(s);
            snprintf(nm, sizeof(nm), "%s/%s/%s", neg ? "neg" : "pos", type_name(t), scen_name(s));
            gen(t, s, buf, sizeof(buf));
            int ok = neg ? run_neg(nm, buf) : run_pos(nm, buf);
            fprintf(stderr, "  [%-3s][%-4s][%-22s] %s\n",
                    neg ? "neg" : "pos", type_name(t), scen_name(s),
                    ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }

    fprintf(stderr, "\n=== cflow-matrix: %d/%d cells correct ===\n", passed, valid_cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | over-rejections: %d\n",
            false_neg, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "CFLOW MATRIX HAS HOLES — a control-flow merge the analyzer mishandles\n");
        fprintf(stderr, "(false negative = under-approximated merge; over-reject = too conservative).\n");
        return 1;
    }
    return 0;
}
