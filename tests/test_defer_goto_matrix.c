/* test_defer_goto_matrix.c — defer x goto ARMING oracle (2026-08-02).
 *
 * WHY THIS EXISTS. Every other matrix in this repo checks ACCEPT/REJECT. This
 * one checks a RUNTIME VALUE, because the defect class it guards is a silent
 * miscompile, not a missing diagnostic: a defer body that runs on a path where
 * its registration never executed. That produces an unbalanced acquire/release
 * — a lock underflow, a double close, a free of something never allocated —
 * with no diagnostic anywhere. §F2 (2026-08-02) is exactly that bug:
 *
 *     _zer_bb1: rel(); return;      // the label — UNCONDITIONAL fire
 *     _zer_bb2: goto _zer_bb1;      // error path: acq() NEVER ran, yet rel() does
 *
 * THE INVARIANT UNDER TEST — one sentence:
 *     a defer body executes exactly once per EXECUTED registration,
 *     and never when its registration did not execute.
 *
 * MEASUREMENT. Each cell pairs `acq()` (bal += 1) with `defer rel()`
 * (bal -= 1) and a `bal += 100` marker on the fall-through path only. A correct
 * compiler therefore always leaves bal at 0 (goto taken) or 100 (not taken) —
 * the value is IDENTICAL across every defer position, because the property
 * being measured is BALANCE, not placement. The generated main() maps that to
 * a self-describing exit code:
 *     0   correct
 *     200 bal < 0  -> an UNARMED defer fired (the §F2 miscompile signature)
 *     100 some other wrong value
 *
 * WHY IT IS BUILT NOW, BEFORE THE FIX IT GUARDS. The durable §F2 fix (a
 * per-defer runtime "armed" flag) edits the defer emission core —
 * defer_count / defer_fire_bodies snapshots / active_guard_below /
 * restore_count — whose own comments record repeated prior fixes. A mistake
 * there is a SILENTLY dropped cleanup or double fire on one path. Before this
 * file there were 17 tests standing between "I edited defer emission" and that
 * outcome. This grid is the net; the fix goes in behind it, not before it.
 *
 * TODAY vs AFTER THE FIX. §F2 shipped as a SOUND INTERIM REJECT of the
 * `goto -> defer -> label` order, so those cells currently expect a rejection.
 * Each such cell ALSO records the semantically-correct balance it would have.
 * When the armed flag lands, delete `cell_currently_rejected()` and the grid
 * asserts the value everywhere — it is already the acceptance test for that fix.
 *
 * Axes are C enums switched with NO default, so adding a DeferPos / GotoKind /
 * Nest fails GCC -Wswitch until every site handles it. The grid cannot silently
 * shrink (tools/walker_default_audit.sh discipline).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static int miscompiled = 0, wrong_value = 0, unexpected_reject = 0, unexpected_accept = 0;
static const char *zerc_path = NULL;

static void find_zerc(void) {
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    if (system("test -x /tmp/zerc") == 0) { zerc_path = "/tmp/zerc"; return; }
    fprintf(stderr, "ERROR: cannot find zerc\n");
    exit(2);
}

/* ---- axes ---------------------------------------------------------------- */

/* Where the `defer` sits relative to the goto and the label. */
typedef enum {
    DP_NONE,          /* no defer at all — control: proves the goto itself works */
    DP_BEFORE_GOTO,   /* defer registered BEFORE the goto — armed on every path  */
    DP_BETWEEN,       /* gi < di < li — the §F2 shape: armed ONLY on fall-through */
    DP_AFTER_LABEL,   /* registered at/after the label — armed on every path there*/
    DP_COUNT
} DeferPos;

typedef enum {
    GK_NONE,          /* no goto/label — straight-line baseline */
    GK_TAKEN,         /* conditional goto, TAKEN at runtime     */
    GK_NOTTAKEN,      /* conditional goto, NOT taken at runtime */
    GK_COUNT
} GotoKind;

/* Where the goto STATEMENT is nested. Exercises find_goto_to_label's recursion
 * as much as the emitter's block handling. */
typedef enum {
    NS_FLAT,          /* directly in the function body */
    NS_IF,            /* inside an if body             */
    NS_LOOP,          /* inside a for body             */
    NS_SWITCH,        /* inside a switch arm           */
    NS_COUNT
} Nest;

static const char *dp_name(DeferPos d) {
    switch (d) {
    case DP_NONE:         return "no-defer";
    case DP_BEFORE_GOTO:  return "defer-before-goto";
    case DP_BETWEEN:      return "defer-between";
    case DP_AFTER_LABEL:  return "defer-after-label";
    case DP_COUNT:        break;
    }
    return "?";
}
static const char *gk_name(GotoKind g) {
    switch (g) {
    case GK_NONE:      return "no-goto";
    case GK_TAKEN:     return "goto-taken";
    case GK_NOTTAKEN:  return "goto-not-taken";
    case GK_COUNT:     break;
    }
    return "?";
}
static const char *ns_name(Nest n) {
    switch (n) {
    case NS_FLAT:    return "flat";
    case NS_IF:      return "in-if";
    case NS_LOOP:    return "in-loop";
    case NS_SWITCH:  return "in-switch";
    case NS_COUNT:   break;
    }
    return "?";
}

/* Without a goto there is no label, so the three defer POSITIONS collapse to
 * "a defer in a function" — keep only the baseline pair, and nesting is
 * meaningless (nothing to nest). */
static int cell_valid(DeferPos d, GotoKind g, Nest n) {
    if (g == GK_NONE) {
        if (n != NS_FLAT) return 0;
        return (d == DP_NONE || d == DP_BEFORE_GOTO);
    }
    return 1;
}

/* 2026-08-03: the durable armed-flag fix LANDED, so no cell expects a rejection
 * any more — every one asserts a runtime BALANCE. This function is kept as a
 * deliberate no-op rather than deleted: it documents that the grid was BUILT
 * against the interim reject and flipped wholesale, which is the evidence the
 * relaxation is correct across all 34 cells and not just the reproducer. */
static int cell_currently_rejected(DeferPos d, GotoKind g) {
    (void)d; (void)g;
    return 0;
}

/* Correct final balance. Identical for every defer position — that IS the
 * invariant: acquire/release stays balanced, and the +100 marker runs only when
 * the goto is not taken. */
static int expected_balance(GotoKind g) {
    return (g == GK_TAKEN) ? 0 : 100;
}

/* ---- generator ----------------------------------------------------------- */

static void emit_goto_site(Nest n, char *out, size_t cap) {
    switch (n) {
    case NS_FLAT:
        snprintf(out, cap, "    if (err == 1) { goto done; }\n");
        return;
    case NS_IF:
        snprintf(out, cap, "    if (err > 0) { if (err == 1) { goto done; } }\n");
        return;
    case NS_LOOP:
        snprintf(out, cap,
            "    for (u32 k = 0; k < 1; k += 1) { if (err == 1) { goto done; } }\n");
        return;
    case NS_SWITCH:
        snprintf(out, cap,
            "    switch (err) { 1 => { goto done; } default => { } }\n");
        return;
    case NS_COUNT: break;
    }
    out[0] = '\0';
}

static void gen(DeferPos d, GotoKind g, Nest n, char *buf, size_t cap) {
    char site[256];
    site[0] = '\0';
    if (g != GK_NONE) emit_goto_site(n, site, sizeof(site));

    const char *pair = "    acq(); defer rel();\n";
    const char *before = (d == DP_BEFORE_GOTO) ? pair : "";
    const char *between = (d == DP_BETWEEN) ? pair : "";
    const char *after = (d == DP_AFTER_LABEL) ? pair : "";
    const char *label = (g != GK_NONE) ? "done:\n" : "";
    int err_arg = (g == GK_TAKEN) ? 1 : 0;
    int want = expected_balance(g);

    snprintf(buf, cap,
        "i32 bal;\n"
        "void acq() { bal += 1; }\n"
        "void rel() { bal -= 1; }\n"
        "void f(u32 err) {\n"
        "%s"          /* defer BEFORE the goto */
        "%s"          /* the goto site (possibly nested) */
        "%s"          /* defer BETWEEN goto and label — the §F2 shape */
        "    bal += 100;\n"
        "%s"          /* the label */
        "%s"          /* defer AFTER the label */
        "    return;\n"
        "}\n"
        "u32 main() {\n"
        "    bal = 0;\n"
        "    f(%d);\n"
        "    if (bal == %d) { return 0; }\n"
        "    if (bal < 0) { return 200; }\n"   /* unarmed defer fired */
        "    return 100;\n"                    /* some other wrong value */
        "}\n",
        before, site, between, label, after, err_arg, want);
}

/* ---- cell runners -------------------------------------------------------- */

/* Must compile, run, and exit 0 (balance correct). */
static int run_value(const char *nm, const char *src, int want_bal) {
    total++;
    FILE *f = fopen("/tmp/_zer_dg.zer", "w");
    if (!f) { failed++; return 0; }
    fputs(src, f); fclose(f);

    char cmd[512];
    /* NOTE (CLAUDE.md "zerc -o gotchas"): for a NON-.c `-o` path zerc builds the
     * exe NEXT TO THE SOURCE, not at the -o path. The source is
     * /tmp/_zer_dg.zer, so the binary lands at /tmp/_zer_dg. Passing an .exe
     * path and then running it is the trap — it yields exit 127 on every cell
     * and looks like a compiler failure. */
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_dg.zer -o /tmp/_zer_dg >/tmp/_zer_dg.err 2>&1", zerc_path);
    if (system(cmd) != 0) {
        fprintf(stderr, "      %s: REJECTED but should compile (bal should be %d)\n",
                nm, want_bal);
        unexpected_reject++; failed++; return 0;
    }
    int rc = system("/tmp/_zer_dg >/dev/null 2>&1");
    int code = (rc == -1) ? -1 : ((rc >> 8) & 0xff);
    if (code == 0) { passed++; return 1; }
    if (code == 200) {
        fprintf(stderr, "      %s: MISCOMPILE — an UNARMED defer fired "
                        "(balance went negative)\n", nm);
        miscompiled++;
    } else {
        fprintf(stderr, "      %s: wrong balance (exit %d, wanted bal=%d)\n",
                nm, code, want_bal);
        wrong_value++;
    }
    failed++; return 0;
}

/* Must be REJECTED, and for the defer/goto reason — not a parse or type error
 * masking it (the integrity guard every matrix here uses). */
static int run_reject(const char *nm, const char *src) {
    total++;
    FILE *f = fopen("/tmp/_zer_dg.zer", "w");
    if (!f) { failed++; return 0; }
    fputs(src, f); fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_dg.zer -o /tmp/_zer_dg >/tmp/_zer_dg.err 2>&1", zerc_path);
    if (system(cmd) == 0) {
        fprintf(stderr, "      %s: COMPILED but the interim reject should fire\n", nm);
        unexpected_accept++; failed++; return 0;
    }
    if (system("grep -q \"skips the registration of a\" /tmp/_zer_dg.err") != 0) {
        fprintf(stderr, "      %s: rejected for the WRONG reason "
                        "(a parse/type error is masking the check)\n", nm);
        failed++; return 0;
    }
    passed++; return 1;
}

/* ================================================================
 * LOOP-SCOPED DEFER x GOTO-OUT-OF-LOOP sub-grid (2026-08-10)
 *
 * The 34 cells above measure a BALANCE (acquire/release), which is invariant
 * to a defer firing an extra time on a path that also acquires an extra time.
 * They therefore passed BOTH before and after the bug fixed here — verified by
 * running them against the pre-fix compiler. What discriminates is the raw FIRE
 * COUNT of a LOOP-SCOPED defer when the loop body contains a forward goto out
 * of the loop.
 *
 * The bug: NODE_LABEL raised defer_count back to the goto's fired-count, which
 * RESURRECTED the loop-scoped defer that had already fired and popped at each
 * iteration exit. A 3-iteration loop fired its defer 4 times — on the path
 * where the goto was NEVER TAKEN (the success path). With `defer free(x)` that
 * is a double free.
 *
 * Both arms matter: goto-never-taken is the buggy path, goto-taken is the
 * control that must not regress. */
typedef enum { LG_NEVER, LG_TAKEN, LG_COUNT } LoopGoto;

static const char *lg_name(LoopGoto g) {
    switch (g) {
    case LG_NEVER: return "goto-never-taken";
    case LG_TAKEN: return "goto-taken-at-i1";
    case LG_COUNT: break;
    }
    return "?";
}
/* 3 iterations: never taken -> one fire per iteration = 3.
 * taken at i==1 -> i=0 iteration-exit fire + i=1 goto fire = 2. */
static int lg_expected(LoopGoto g) { return g == LG_NEVER ? 3 : 2; }

static void gen_loop_goto(LoopGoto g, char *out, size_t n) {
    snprintf(out, n,
        "i32 fires;\n"
        "void f(bool early) {\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        defer fires += 1;\n"
        "        if (early == true && i == 1) { goto done; }\n"
        "    }\n"
        "done:\n"
        "    return;\n"
        "}\n"
        "u32 main() {\n"
        "    fires = 0;\n"
        "    f(%s);\n"
        "    if (fires != %d) { return (u32)fires; }\n"   /* nonzero = actual count */
        "    return 0;\n"
        "}\n",
        g == LG_TAKEN ? "true" : "false", lg_expected(g));
}

int main(void) {
    find_zerc();
    fprintf(stderr, "\n=== defer x goto ARMING matrix ===\n");
    fprintf(stderr, "    invariant: a defer body runs exactly once per EXECUTED\n");
    fprintf(stderr, "    registration, and never when its registration did not run.\n");
    fprintf(stderr, "    measured as acquire/release BALANCE, not placement.\n");
    fprintf(stderr, "    (2026-08-03: armed-flag fix landed — ALL cells assert values.)\n\n");

    char buf[2048];
    int grid_ok = 1, valid_cells = 0;

    for (DeferPos d = 0; d < DP_COUNT; d++) {
        for (GotoKind g = 0; g < GK_COUNT; g++) {
            for (Nest n = 0; n < NS_COUNT; n++) {
                if (!cell_valid(d, g, n)) continue;
                valid_cells++;
                char nm[160];
                int rej = cell_currently_rejected(d, g);
                snprintf(nm, sizeof(nm), "%s/%s/%s", dp_name(d), gk_name(g), ns_name(n));
                gen(d, g, n, buf, sizeof(buf));
                int ok = rej ? run_reject(nm, buf)
                             : run_value(nm, buf, expected_balance(g));
                fprintf(stderr, "  [%-17s][%-14s][%-9s] %-6s %s\n",
                        dp_name(d), gk_name(g), ns_name(n),
                        rej ? "reject" : "value",
                        ok ? "ok" : "*** FAIL ***");
                if (!ok) grid_ok = 0;
            }
        }
    }

    /* ---- loop-scoped defer x goto-out-of-loop (FIRE COUNT, not balance) ---- */
    fprintf(stderr, "\n  -- loop-scoped defer x goto-out-of-loop (raw fire count) --\n");
    char lbuf[1024];
    for (LoopGoto g = 0; g < LG_COUNT; g++) {
        valid_cells++;
        char nm[128];
        snprintf(nm, sizeof(nm), "loopdefer/%s", lg_name(g));
        gen_loop_goto(g, lbuf, sizeof(lbuf));
        int ok = run_value(nm, lbuf, lg_expected(g));
        fprintf(stderr, "  [%-18s] want %d  %s\n",
                lg_name(g), lg_expected(g), ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }

    fprintf(stderr, "\n=== defer-goto-matrix: %d/%d cells correct ===\n",
            passed, valid_cells);
    fprintf(stderr, "    miscompiles (unarmed defer fired): %d | wrong balance: %d\n",
            miscompiled, wrong_value);
    fprintf(stderr, "    unexpected rejects: %d | unexpected accepts: %d\n",
            unexpected_reject, unexpected_accept);
    if (!grid_ok) {
        fprintf(stderr, "DEFER-GOTO MATRIX HAS HOLES — a defer fires on a path whose\n");
        fprintf(stderr, "registration never executed, or a balanced pattern regressed.\n");
        return 1;
    }
    return 0;
}
