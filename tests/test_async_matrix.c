/* test_async_matrix.c — async (yield/await) soundness oracle (2026-06-07).
 * Domain 3 (final) of the limitations.md non-memory frontier.
 *
 * ZER async = stackless coroutines via a Duff's-device state machine. The
 * safety rules are structural bans whose justification is in the Ban Decision
 * Framework (CLAUDE.md): yield/await in defer = emission impossibility
 * (duplicate Duff case labels); yield/await in @critical = needs a runtime
 * (interrupt save/restore across suspend); shared access across a suspension =
 * lock held across yield (deadlock); spawn in async = thread lifetime needs a
 * type system ZER doesn't have.
 *
 * TWO cell kinds:
 *   - NEGATIVE: an async-unsafe construct MUST be rejected for the relevant
 *     reason. Compiling clean = false negative.
 *   - POSITIVE: a valid async pattern (plain yield/await, defer without a
 *     suspend inside it, locals promoted across yield) MUST compile.
 *
 * EMIT-ONLY harness (`-o /tmp/x.c`, no gcc): isolates the checker/zercheck
 * VERDICT; async codegen correctness is separately covered by tests/zer async
 * tests. Integrity guard: a NEG rejection by parse error is flagged INVALID.
 * -Wswitch-enforced scenario enum.
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

static int has_async_reason(const char *eb) {
    return strstr(eb, "yield") || strstr(eb, "await") || strstr(eb, "defer") ||
           strstr(eb, "critical") || strstr(eb, "async") ||
           strstr(eb, "suspension") || strstr(eb, "spawn") ||
           strstr(eb, "coroutine") || strstr(eb, "deadlock");
}

/* NEG: must reject for an async-safety reason. EMIT-ONLY harness. */
static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_as.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_as.zer -o /tmp/_zer_as.c 2>/tmp/_zer_as.err", zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — async-unsafe construct ACCEPTED\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_as.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not an async check\n", name);
        fprintf(stderr, "    %.110s\n", eb);
        return 0;
    }
    if (has_async_reason(eb)) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for an async-safety reason:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    return 0;
}

/* POS: a valid async pattern must compile (emit succeeds). */
static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_as.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_as.zer -o /tmp/_zer_as.c 2>/tmp/_zer_as.err", zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_as.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — valid async pattern REJECTED:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

typedef enum {
    /* NEGATIVE — async-unsafe (must reject) */
    AS_YIELD_IN_DEFER,     /* yield inside a defer body */
    AS_AWAIT_IN_DEFER,     /* await inside a defer body */
    AS_YIELD_IN_CRITICAL,  /* yield inside @critical */
    AS_AWAIT_IN_CRITICAL,  /* await inside @critical */
    AS_SPAWN_IN_ASYNC,     /* spawn inside an async function */
    /* POSITIVE — valid async (must compile) */
    AS_YIELD_OK,           /* plain yield in an async function */
    AS_AWAIT_OK,           /* await on a non-shared global condition */
    AS_DEFER_NO_SUSPEND_OK,/* defer with a non-suspending body + yield elsewhere */
    AS_LOCAL_ACROSS_YIELD_OK, /* a local live across yield (state-promoted) */
    AS_AWAIT_ON_SHARED_OK, /* await condition reads a shared struct — SAFE: each poll
                            * locks/reads/unlocks, lock released between polls, never
                            * held across the suspension (yield/await are statement-only,
                            * so a shared lock can't bracket a suspend). NOT a hole. */
    ASSCEN_COUNT
} ASScenario;

static int scenario_is_negative(ASScenario s) {
    switch (s) {
        case AS_YIELD_IN_DEFER: case AS_AWAIT_IN_DEFER:
        case AS_YIELD_IN_CRITICAL: case AS_AWAIT_IN_CRITICAL:
        case AS_SPAWN_IN_ASYNC:
            return 1;
        case AS_YIELD_OK: case AS_AWAIT_OK:
        case AS_DEFER_NO_SUSPEND_OK: case AS_LOCAL_ACROSS_YIELD_OK:
        case AS_AWAIT_ON_SHARED_OK:
            return 0;
        case ASSCEN_COUNT: break;
    }
    return 1;
}

static const char *scen_name(ASScenario s) {
    switch (s) {
        case AS_YIELD_IN_DEFER:        return "yield-in-defer";
        case AS_AWAIT_IN_DEFER:        return "await-in-defer";
        case AS_YIELD_IN_CRITICAL:     return "yield-in-critical";
        case AS_AWAIT_IN_CRITICAL:     return "await-in-critical";
        case AS_SPAWN_IN_ASYNC:        return "spawn-in-async";
        case AS_YIELD_OK:              return "async-yield-ok";
        case AS_AWAIT_OK:              return "async-await-ok";
        case AS_DEFER_NO_SUSPEND_OK:   return "async-defer-no-suspend-ok";
        case AS_LOCAL_ACROSS_YIELD_OK: return "local-across-yield-ok";
        case AS_AWAIT_ON_SHARED_OK:    return "await-on-shared-ok";
        case ASSCEN_COUNT: break;
    }
    return "?";
}

static void gen(ASScenario s, char *buf, size_t n) {
    switch (s) {
        case AS_YIELD_IN_DEFER:
            snprintf(buf, n,
                "async void worker() {\n    defer { yield; }\n    yield;\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_AWAIT_IN_DEFER:
            snprintf(buf, n,
                "u32 g_ready;\n"
                "async void worker() {\n    defer { await g_ready == 1; }\n    yield;\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_YIELD_IN_CRITICAL:
            snprintf(buf, n,
                "async void worker() {\n    @critical { yield; }\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_AWAIT_IN_CRITICAL:
            snprintf(buf, n,
                "u32 g_ready;\n"
                "async void worker() {\n    @critical { await g_ready == 1; }\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_SPAWN_IN_ASYNC:
            snprintf(buf, n,
                "shared struct C { u32 v; }\n"
                "C g;\n"
                "void w(*C c) { c.v = 1; }\n"
                "async void worker() {\n    spawn w(&g);\n    yield;\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_YIELD_OK:
            snprintf(buf, n,
                "async void worker() {\n    u32 x = 0;\n    yield;\n    x = 1;\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_AWAIT_OK:
            snprintf(buf, n,
                "u32 g_ready;\n"
                "async void waiter() {\n    await g_ready == 1;\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_DEFER_NO_SUSPEND_OK:
            snprintf(buf, n,
                "void cleanup() { }\n"
                "async void worker() {\n    defer cleanup();\n    yield;\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_LOCAL_ACROSS_YIELD_OK:
            snprintf(buf, n,
                "async void worker() {\n    u32 acc = 5;\n    yield;\n    acc = acc + 1;\n    yield;\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case AS_AWAIT_ON_SHARED_OK:
            snprintf(buf, n,
                "shared struct C { u32 ready; }\n"
                "C g;\n"
                "async void waiter() {\n    await g.ready == 1;\n}\n"
                "u32 main() { return 0; }\n");
            break;
        case ASSCEN_COUNT: buf[0] = 0; break;
    }
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== async (yield/await) matrix ===\n");
    fprintf(stderr, "    NEG: async-unsafe construct -> must REJECT for an async reason\n");
    fprintf(stderr, "    POS: valid async pattern -> must COMPILE (emit-only)\n\n");

    char buf[1024];
    int grid_ok = 1, valid_cells = 0;
    for (ASScenario s = 0; s < ASSCEN_COUNT; s++) {
        valid_cells++;
        int neg = scenario_is_negative(s);
        char nm[128];
        snprintf(nm, sizeof(nm), "%s/%s", neg ? "neg" : "pos", scen_name(s));
        gen(s, buf, sizeof(buf));
        int ok = neg ? run_neg(nm, buf) : run_pos(nm, buf);
        fprintf(stderr, "  [%-3s][%-26s] %s\n",
                neg ? "neg" : "pos", scen_name(s), ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }

    fprintf(stderr, "\n=== async-matrix: %d/%d cells correct ===\n", passed, valid_cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | over-rejections: %d\n",
            false_neg, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "ASYNC MATRIX HAS HOLES — an async-unsafe construct the analyzer\n");
        fprintf(stderr, "mishandles (false negative), or a valid async pattern over-rejected.\n");
        return 1;
    }
    return 0;
}
