/* test_sharedlock_matrix.c — SHARED-STRUCT AUTO-LOCK x STATEMENT-BODY FORM
 *
 * WHY THIS GRID EXISTS (BUG-917, 2026-09-02)
 * ------------------------------------------
 * `shared struct` is ZER's headline concurrency mechanism: touching a field is
 * supposed to lock, act and unlock, with no annotation. The lock is emitted by
 * ir_lower's per-statement wrapper, which lives in the NODE_BLOCK case — so the
 * guarantee actually reads "every statement INSIDE A BLOCK is locked", and it
 * holds only because the parser makes every statement body a block.
 *
 * It made ONE exception. A switch arm written without braces:
 *
 *     0 => g.x = 5,          emitted   g.x = 5;                    (no mutex)
 *     0 => { g.x = 5; }      emitted   lock; g.x = 5; unlock;
 *
 * Same program, two spellings, one of them an unsynchronized write to shared
 * memory with no diagnostic anywhere. Nothing caught it: the concurrency matrix
 * asks accept-or-reject and BOTH spellings are accepted; the walker audits ask
 * about node kinds and the kind was handled. The missing question was
 * "was the lock EMITTED?", and no gate asked it.
 *
 * So this grid asks that one question, crossed over every statement-body form a
 * shared access can sit in. It reads the EMITTED C rather than an exit code,
 * because the defect produces no diagnostic and no deterministic runtime
 * signal — a data race is not something a single-threaded test can observe.
 *
 * ADDING A BODY FORM: add a cell. A form with no cell here is invisible, which
 * is exactly how the bare switch arm survived.
 *
 * VERIFIED TO FIRE: against a pre-BUG-917 build this grid fails on the
 * bare-switch-arm cell and passes everywhere else. A gate that has only ever
 * passed is a script, not a net.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static const char *zerc_path = NULL;

static void find_zerc(void) {
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    if (system("test -x /tmp/zerc") == 0) { zerc_path = "/tmp/zerc"; return; }
    fprintf(stderr, "ERROR: cannot find zerc (run 'make zerc' first)\n");
    exit(2);
}

typedef enum {
    LF_FUNC_TOPLEVEL,      /* straight in the function body            */
    LF_IF_THEN,            /* if (c) { ... }                           */
    LF_IF_ELSE,            /* else { ... }                             */
    LF_FOR_BODY,           /* for (..) { ... }                         */
    LF_WHILE_BODY,         /* while (..) { ... }                       */
    LF_DOWHILE_BODY,       /* do { ... } while (..);                   */
    LF_SWITCH_ARM_BLOCK,   /* 0 => { ... }                             */
    LF_SWITCH_ARM_BARE,    /* 0 => expr,      <-- BUG-917 lived here   */
    LF_SWITCH_DEFAULT_BARE,/* default => expr,                         */
    LF_CRITICAL_BODY,      /* @critical { ... }                        */
    LF_ONCE_BODY,          /* @once { ... }                            */
    LF_DEFER_BLOCK,        /* defer { ... }                            */
    LF_DEFER_BARE,         /* defer expr;                              */
    LF_NESTED_BLOCK,       /* { { ... } }                              */
    LF_COUNT
} LockForm;

static const char *form_name(LockForm f) {
    switch (f) {
    case LF_FUNC_TOPLEVEL:       return "function-body";
    case LF_IF_THEN:             return "if-then";
    case LF_IF_ELSE:             return "if-else";
    case LF_FOR_BODY:            return "for-body";
    case LF_WHILE_BODY:          return "while-body";
    case LF_DOWHILE_BODY:        return "do-while-body";
    case LF_SWITCH_ARM_BLOCK:    return "switch-arm-block";
    case LF_SWITCH_ARM_BARE:     return "switch-arm-BARE";
    case LF_SWITCH_DEFAULT_BARE: return "switch-default-BARE";
    case LF_CRITICAL_BODY:       return "critical-body";
    case LF_ONCE_BODY:           return "once-body";
    case LF_DEFER_BLOCK:         return "defer-block";
    case LF_DEFER_BARE:          return "defer-bare";
    case LF_NESTED_BLOCK:        return "nested-block";
    case LF_COUNT:               break;
    }
    return "?";
}

/* The shared WRITE is identical in every cell; only where it sits changes.
 * That is what makes the expected result trivially derivable (a lock must be
 * emitted, always) instead of hand-tabulated per cell. */
static void gen(LockForm f, char *out, size_t n) {
    const char *body = "";
    switch (f) {
    case LF_FUNC_TOPLEVEL:       body = "    g.x = 5;"; break;
    case LF_IF_THEN:             body = "    if (k == 0) { g.x = 5; }"; break;
    case LF_IF_ELSE:             body = "    if (k == 9) { k = 1; } else { g.x = 5; }"; break;
    case LF_FOR_BODY:            body = "    for (u32 i = 0; i < 1; i += 1) { g.x = 5; }"; break;
    case LF_WHILE_BODY:          body = "    while (k > 0) { g.x = 5; k -= 1; }"; break;
    case LF_DOWHILE_BODY:        body = "    do { g.x = 5; } while (k > 0);"; break;
    case LF_SWITCH_ARM_BLOCK:    body = "    switch (k) { 0 => { g.x = 5; } default => { } }"; break;
    case LF_SWITCH_ARM_BARE:     body = "    switch (k) { 0 => g.x = 5, default => { } }"; break;
    case LF_SWITCH_DEFAULT_BARE: body = "    switch (k) { 0 => { } default => g.x = 5, }"; break;
    case LF_CRITICAL_BODY:       body = "    @critical { g.x = 5; }"; break;
    case LF_ONCE_BODY:           body = "    @once { g.x = 5; }"; break;
    case LF_DEFER_BLOCK:         body = "    defer { g.x = 5; }"; break;
    case LF_DEFER_BARE:          body = "    defer g.x = 5;"; break;
    case LF_NESTED_BLOCK:        body = "    { { g.x = 5; } }"; break;
    case LF_COUNT:               break;
    }
    snprintf(out, n,
        "shared struct S { u32 x; }\n"
        "S g;\n"
        "u32 main() {\n"
        "    u32 k = 0;\n"
        "%s\n"
        "    return 0;\n"
        "}\n", body);
}

/* Compile to C and assert the auto-lock reached the emitted output. */
static int run_cell(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_lock.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_lock.zer -o /tmp/_zer_lock.c >/tmp/_zer_lock.err 2>&1",
             zerc_path);
    if (system(cmd) != 0) {
        failed++;
        fprintf(stderr, "  FAIL [WONT-COMPILE] %s — the probe itself is broken:\n", name);
        { char eb[1024]; eb[0]=0; FILE *e = fopen("/tmp/_zer_lock.err","r");
          if (e) { size_t r = fread(eb,1,sizeof(eb)-1,e); eb[r]=0; fclose(e); }
          fprintf(stderr, "    %.180s\n", eb); }
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }

    /* Read the emitted C and look for the mutex around the shared write. */
    FILE *c = fopen("/tmp/_zer_lock.c", "r");
    if (!c) { failed++; fprintf(stderr, "  FAIL %s — no emitted C\n", name); return 0; }
    char *buf = (char *)malloc(1 << 20);
    size_t rd = fread(buf, 1, (1 << 20) - 1, c);
    buf[rd] = 0; fclose(c);

    /* The preamble defines helper functions that mention the mutex, so match the
     * SPECIFIC lock on our own global — `&g._zer_mtx` — not a bare "mutex". */
    int has_lock   = strstr(buf, "pthread_mutex_lock(&g._zer_mtx)") != NULL;
    int has_unlock = strstr(buf, "pthread_mutex_unlock(&g._zer_mtx)") != NULL;
    int has_write  = strstr(buf, "g.x = 5") != NULL;
    free(buf);

    if (!has_write) {
        failed++;
        fprintf(stderr, "  FAIL [VACUOUS] %s — the shared write was not emitted at all,\n"
                        "        so this cell proves nothing about locking\n", name);
        return 0;
    }
    if (has_lock && has_unlock) { passed++; return 1; }

    failed++;
    fprintf(stderr, "  FAIL [UNLOCKED] %s — shared write emitted with %s\n",
            name, has_lock ? "a lock but no unlock" : "NO mutex at all");
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== shared-struct auto-lock x statement-body-form matrix ===\n");
    fprintf(stderr, "    Every cell writes the SAME shared field; only WHERE it sits changes.\n");
    fprintf(stderr, "    A lock+unlock on that global must appear in the emitted C, always.\n\n");

    char buf[1024];
    for (LockForm f = 0; f < LF_COUNT; f++) {
        gen(f, buf, sizeof(buf));
        int ok = run_cell(form_name(f), buf);
        fprintf(stderr, "  [%-20s] %s\n", form_name(f), ok ? "ok" : "*** FAIL ***");
    }

    fprintf(stderr, "\n=== shared-lock matrix: %d cells, %d passed, %d failed ===\n",
            total, passed, failed);
    if (failed == 0) fprintf(stderr, "SHARED-LOCK MATRIX CLEAN\n");
    return failed == 0 ? 0 : 1;
}
