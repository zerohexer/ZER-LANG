/* test_conc_matrix.c — data-race / spawn / deadlock soundness oracle
 * (2026-06-07). First oracle for the NON-memory frontier (limitations.md
 * "next frontier" entry, Domain 1).
 *
 * The shape/escape/keep/cflow matrices guard memory safety. This guards the
 * concurrency accept/reject rules: spawn pointer-safety, spawned-body global
 * access, deadlock lock-ordering, spawn-in-@critical, and ThreadHandle join.
 *
 * TWO cell kinds:
 *   - NEGATIVE: a data race / deadlock / unjoined-thread MUST be rejected for a
 *     concurrency reason. Compiling clean = false negative (the unacceptable
 *     class — an unsynchronized program the analyzer let through).
 *   - POSITIVE: a correctly-synchronized pattern (shared struct auto-lock,
 *     scoped spawn + join, value args, separate-statement shared access) MUST
 *     compile. Rejection = over-rejection (acceptable, but logged).
 *
 * INTEGRITY GUARD: a NEG rejection only counts if the error names a concurrency
 * reason (data race / deadlock / non-shared / not joined / @critical / spawn /
 * thread). A parse/type error masking the check is flagged INVALID.
 *
 * Axes are a C enum switched with NO default — adding a COScenario fails GCC
 * -Wswitch until handled. Concurrency has no pool/slab type axis, so this is a
 * flat scenario list (like the curated control-flow scenarios).
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

static int has_conc_reason(const char *eb) {
    return strstr(eb, "data race") || strstr(eb, "deadlock") ||
           strstr(eb, "non-shared") || strstr(eb, "not joined") ||
           strstr(eb, "@critical") || strstr(eb, "spawn") ||
           strstr(eb, "thread") || strstr(eb, "shared");
}

/* NEGATIVE: must reject for a concurrency reason. */
static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_co.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_co.zer -o /dev/null 2>/tmp/_zer_co.err", zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — unsynchronized program COMPILED CLEAN\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_co.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not a concurrency check\n", name);
        fprintf(stderr, "    %.110s\n", eb);
        return 0;
    }
    if (has_conc_reason(eb)) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for a concurrency reason:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    return 0;
}

/* POSITIVE: a correctly-synchronized pattern must compile. */
static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_co.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_co.zer -o /dev/null 2>/tmp/_zer_co.err", zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_co.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — safe concurrency pattern REJECTED:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

typedef enum {
    /* NEGATIVE (unsynchronized — must reject) */
    CO_SPAWN_NONSHARED_PTR,    /* spawn f(&local), f takes non-shared *T */
    CO_SPAWN_NONSHARED_GLOBAL, /* spawned body reads/writes a non-shared global */
    CO_DEADLOCK_SAME_STMT,     /* a.x = b.y — two shared types in one statement */
    CO_SPAWN_IN_CRITICAL,      /* spawn inside @critical (interrupts disabled) */
    CO_THREADHANDLE_NOT_JOINED,/* scoped spawn ThreadHandle never joined */
    CO_SPAWN_TRANSITIVE_GLOBAL,/* spawn -> helper() -> non-shared global (transitive) */
    CO_SPAWN_SLAB,             /* spawned body uses a global Slab (non-atomic metadata) */
    CO_THREADHANDLE_JOIN_ONE_BRANCH, /* join only in one branch — unjoined on the other */
    /* POSITIVE (synchronized — must compile) */
    CO_SPAWN_SHARED_OK,        /* spawn f(&g), g is a shared struct (auto-locked) */
    CO_SCOPED_SPAWN_JOIN,      /* ThreadHandle th = spawn f(&w); th.join(); */
    CO_SPAWN_VALUE_ARGS,       /* spawn f(42) — value args copied, no sharing */
    CO_DEADLOCK_SEPARATE_OK,   /* a.x=1; b.y=2; — separate statements, no nesting */
    CO_SHARED_FIELD_OK,        /* shared struct field access (auto-lock) */
    CO_THREADHANDLE_JOIN_BOTH, /* join in both branches — joined on all paths */
    CO_SPAWN_THREADLOCAL,      /* spawned body touches a threadlocal global (per-thread) */
    COSCEN_COUNT
} COScenario;

static int scenario_is_negative(COScenario s) {
    switch (s) {
        case CO_SPAWN_NONSHARED_PTR: case CO_SPAWN_NONSHARED_GLOBAL:
        case CO_DEADLOCK_SAME_STMT: case CO_SPAWN_IN_CRITICAL:
        case CO_THREADHANDLE_NOT_JOINED: case CO_SPAWN_TRANSITIVE_GLOBAL:
        case CO_SPAWN_SLAB: case CO_THREADHANDLE_JOIN_ONE_BRANCH:
            return 1;
        case CO_SPAWN_SHARED_OK: case CO_SCOPED_SPAWN_JOIN:
        case CO_SPAWN_VALUE_ARGS: case CO_DEADLOCK_SEPARATE_OK:
        case CO_SHARED_FIELD_OK: case CO_THREADHANDLE_JOIN_BOTH:
        case CO_SPAWN_THREADLOCAL:
            return 0;
        case COSCEN_COUNT: break;
    }
    return 1;
}

static const char *scen_name(COScenario s) {
    switch (s) {
        case CO_SPAWN_NONSHARED_PTR:     return "spawn-nonshared-ptr";
        case CO_SPAWN_NONSHARED_GLOBAL:  return "spawn-nonshared-global";
        case CO_DEADLOCK_SAME_STMT:      return "deadlock-same-stmt";
        case CO_SPAWN_IN_CRITICAL:       return "spawn-in-critical";
        case CO_THREADHANDLE_NOT_JOINED: return "threadhandle-not-joined";
        case CO_SPAWN_TRANSITIVE_GLOBAL: return "spawn-transitive-global";
        case CO_SPAWN_SLAB:              return "spawn-slab-access";
        case CO_THREADHANDLE_JOIN_ONE_BRANCH: return "join-one-branch";
        case CO_SPAWN_SHARED_OK:         return "spawn-shared-ok";
        case CO_SCOPED_SPAWN_JOIN:       return "scoped-spawn-join";
        case CO_SPAWN_VALUE_ARGS:        return "spawn-value-args";
        case CO_DEADLOCK_SEPARATE_OK:    return "deadlock-separate-ok";
        case CO_SHARED_FIELD_OK:         return "shared-field-ok";
        case CO_THREADHANDLE_JOIN_BOTH:  return "join-both-branches";
        case CO_SPAWN_THREADLOCAL:       return "spawn-threadlocal";
        case COSCEN_COUNT: break;
    }
    return "?";
}

static void gen(COScenario s, char *buf, size_t n) {
    switch (s) {
        case CO_SPAWN_NONSHARED_PTR:
            snprintf(buf, n,
                "struct Work { u32 x; }\n"
                "void worker(*Work w) { w.x = 1; }\n"
                "u32 main() { Work local; spawn worker(&local); return 0; }\n");
            break;
        case CO_SPAWN_NONSHARED_GLOBAL:
            snprintf(buf, n,
                "u32 g_ctr;\n"
                "void worker() { g_ctr = g_ctr + 1; }\n"
                "u32 main() { spawn worker(); return 0; }\n");
            break;
        case CO_DEADLOCK_SAME_STMT:
            snprintf(buf, n,
                "shared struct Alpha { u32 x; }\n"
                "shared struct Beta { u32 y; }\n"
                "Alpha ga;\nBeta gb;\n"
                "u32 main() { ga.x = gb.y; return 0; }\n");
            break;
        case CO_SPAWN_IN_CRITICAL:
            snprintf(buf, n,
                "shared struct C { u32 v; }\n"
                "C g;\n"
                "void worker(*C c) { c.v = 1; }\n"
                "u32 main() { @critical { spawn worker(&g); } return 0; }\n");
            break;
        case CO_THREADHANDLE_NOT_JOINED:
            snprintf(buf, n,
                "struct W { u32 x; }\n"
                "void compute(*W w) { w.x = 1; }\n"
                "u32 main() { W work; ThreadHandle th = spawn compute(&work); return 0; }\n");
            break;
        case CO_SPAWN_SHARED_OK:
            snprintf(buf, n,
                "shared struct C { u32 v; }\n"
                "C g;\n"
                "void worker(*C c) { c.v = 1; }\n"
                "u32 main() { spawn worker(&g); return 0; }\n");
            break;
        case CO_SCOPED_SPAWN_JOIN:
            snprintf(buf, n,
                "struct W { u32 x; }\n"
                "void compute(*W w) { w.x = 1; }\n"
                "u32 main() { W work; ThreadHandle th = spawn compute(&work); th.join(); return 0; }\n");
            break;
        case CO_SPAWN_VALUE_ARGS:
            snprintf(buf, n,
                "void handler(u32 ev) { u32 x = ev; }\n"
                "u32 main() { spawn handler(42); return 0; }\n");
            break;
        case CO_DEADLOCK_SEPARATE_OK:
            snprintf(buf, n,
                "shared struct Alpha { u32 x; }\n"
                "shared struct Beta { u32 y; }\n"
                "Alpha ga;\nBeta gb;\n"
                "u32 main() { ga.x = 10; gb.y = 20; return 0; }\n");
            break;
        case CO_SHARED_FIELD_OK:
            snprintf(buf, n,
                "shared struct C { u32 v; }\n"
                "C g;\n"
                "u32 main() { g.v = 42; u32 x = g.v; return x - 42; }\n");
            break;
        case CO_SPAWN_TRANSITIVE_GLOBAL:
            snprintf(buf, n,
                "u32 g_ctr;\n"
                "void helper() { g_ctr = g_ctr + 1; }\n"
                "void worker() { helper(); }\n"
                "u32 main() { spawn worker(); return 0; }\n");
            break;
        case CO_SPAWN_SLAB:
            snprintf(buf, n,
                "struct Task { u32 id; }\n"
                "Slab(Task) gs;\n"
                "void worker() { Handle(Task) h = gs.alloc() orelse return; gs.free(h); }\n"
                "u32 main() { spawn worker(); return 0; }\n");
            break;
        case CO_THREADHANDLE_JOIN_ONE_BRANCH:
            snprintf(buf, n,
                "struct W { u32 x; }\n"
                "void compute(*W w) { w.x = 1; }\n"
                "u32 main() { W work; u32 c = 0; ThreadHandle th = spawn compute(&work);\n"
                "    if (c > 0) { th.join(); }\n    return 0; }\n");
            break;
        case CO_THREADHANDLE_JOIN_BOTH:
            snprintf(buf, n,
                "struct W { u32 x; }\n"
                "void compute(*W w) { w.x = 1; }\n"
                "u32 main() { W work; u32 c = 0; ThreadHandle th = spawn compute(&work);\n"
                "    if (c > 0) { th.join(); } else { th.join(); }\n    return 0; }\n");
            break;
        case CO_SPAWN_THREADLOCAL:
            snprintf(buf, n,
                "threadlocal u32 g_tl;\n"
                "void worker() { g_tl = g_tl + 1; }\n"
                "u32 main() { spawn worker(); return 0; }\n");
            break;
        case COSCEN_COUNT: buf[0] = 0; break;
    }
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== Concurrency (data-race / spawn / deadlock) matrix ===\n");
    fprintf(stderr, "    NEG: unsynchronized program -> must REJECT for a concurrency reason\n");
    fprintf(stderr, "    POS: correctly-synchronized pattern -> must COMPILE\n\n");

    char buf[1024];
    int grid_ok = 1, valid_cells = 0;
    for (COScenario s = 0; s < COSCEN_COUNT; s++) {
        valid_cells++;
        int neg = scenario_is_negative(s);
        char nm[128];
        snprintf(nm, sizeof(nm), "%s/%s", neg ? "neg" : "pos", scen_name(s));
        gen(s, buf, sizeof(buf));
        int ok = neg ? run_neg(nm, buf) : run_pos(nm, buf);
        fprintf(stderr, "  [%-3s][%-24s] %s\n",
                neg ? "neg" : "pos", scen_name(s), ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }

    fprintf(stderr, "\n=== conc-matrix: %d/%d cells correct ===\n", passed, valid_cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | over-rejections: %d\n",
            false_neg, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "CONC MATRIX HAS HOLES — a data race / deadlock / unjoined thread the\n");
        fprintf(stderr, "analyzer mishandles (false negative = unsynchronized program accepted).\n");
        return 1;
    }
    return 0;
}
