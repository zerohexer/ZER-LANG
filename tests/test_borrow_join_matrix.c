/* test_borrow_join_matrix.c — SCOPED-BORROW RELEASE x JOIN PLACEMENT
 *
 * WHY THIS GRID EXISTS
 * --------------------
 * A local lent to a scoped spawn (`ThreadHandle th = spawn w(&work);`) is
 * exclusively borrowed until `.join()`. The borrow tracker is a LINEAR
 * statement-order approximation, so a join nested inside a runtime-conditional
 * body cannot simply clear the borrow: the other path never joined, and
 * clearing it there was a measured accept-unsafe race.
 *
 * The conservative rule that closed that ("a join deeper than the spawn's
 * branch depth does not release") also rejects the ONE shape that is obviously
 * safe — a join on *every* arm of an if/else:
 *
 *     ThreadHandle th = spawn worker(&work);
 *     if (err) { th.join(); } else { th.join(); }
 *     work.x = 2;                       // rejected, though every path joined
 *
 * This grid is the ORACLE for relaxing exactly that and nothing more. It is
 * written BEFORE the relaxation, so the implementation is measured against a
 * stated contract instead of the contract being back-fitted to the code.
 *
 * THE CONTRACT
 * ------------
 *   ACCEPT  only when every arm of an if/else joins the handle unconditionally
 *           at exactly one branch level below the spawn.
 *   REJECT  everything else, including shapes that are *provably* safe by a
 *           deeper analysis. Over-rejection is a soft cost; a single accepted
 *           racing path is a shipped data race.
 *
 * A relaxation is the one change class where a bug is a shipped race, so the
 * negatives here outnumber the positives on purpose: they are the thing being
 * proven, and the positive is only the payoff.
 *
 * STATUS (2026-09-02): the RELAXATION IS NOT IMPLEMENTED. The nine negatives are
 * hard-gated and pass — that half is the soundness that exists today, and it is
 * now pinned. The two cells that require the relaxation are marked PENDING: they
 * are expected to be rejected, and the moment one COMPILES the matrix fails
 * loudly and tells you to promote it. That is the `tests/zer_gaps/` convention
 * (a gap that closes must fail, not silently start passing) applied to a grid.
 *
 * WHY IT WAS NOT IMPLEMENTED HERE, written down so the next session does not
 * have to re-derive it. The mechanism is small — record a handle whose join was
 * refused at exactly `th_spawn_branch_depth + 1`, once per arm, and release
 * after the if when BOTH arms recorded the SAME handle (per-handle, never
 * per-branch: that is what the two-handles cell exists to force). What is NOT
 * small is the soundness obligation, and every item on it is a hand-enumerated
 * form, which is precisely the shape that produces accept-unsafe holes here:
 *
 *   1. The `else` must exist. A missing else is a path that never joined.
 *   2. The join must be at exactly spawn_depth+1. Deeper means conditional
 *      WITHIN the arm (the nested-join cells).
 *   3. The handle must not have been re-spawned inside an arm. Guard: at release
 *      time require `sym->th_spawn_branch_depth == c->branch_depth`, which is
 *      false if a spawn inside an arm re-stamped it.
 *   4. A DIFFERENT handle declared inside an arm may borrow the same local
 *      (`if (a) { th.join(); ThreadHandle t2 = spawn w(&work); }`). Releasing on
 *      the outer handle's borrow list would clear a flag the inner handle owns.
 *      Whether this is reachable depends on the un-joined-handle check firing
 *      first — MEASURE it, do not assume it.
 *   5. `break`/`continue`/`return`/`goto` inside an arm: analysed as safe (the
 *      code after the if is not reached on those paths, and code before it was
 *      already checked with the borrow live), but "analysed as safe" is not the
 *      same as gated, and the tracker is a linear approximation.
 *
 * Items 4 and 5 are the ones to settle with measurement before writing any code.
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
    /* --- must ACCEPT (the relaxation, and shapes that already worked) --- */
    BJ_BOTH_ARMS,          /* if { join } else { join }        <-- the payoff  */
    BJ_HOISTED,            /* if {} else {}  join;             (already ok)    */
    BJ_STRAIGHT,           /* join; use;                       (already ok)    */
    BJ_BOTH_ARMS_NESTBLOCK,/* if { { join } } else { { join } } plain block ok  */

    /* --- must REJECT: some path reaches the use without having joined --- */
    BJ_THEN_ONLY_NO_ELSE,  /* if { join }               — fall-through races   */
    BJ_THEN_ONLY_ELSE,     /* if { join } else { }      — else races           */
    BJ_ELSE_ONLY,          /* if { } else { join }      — then races           */
    BJ_NEITHER,            /* if { } else { }           — nothing joined       */
    BJ_NESTED_THEN,        /* if { if { join } } else { join } — inner is cond  */
    BJ_NESTED_BOTH,        /* if { if { join } } else { if { join } }          */
    BJ_LOOP_BODY,          /* while { join }            — may run zero times   */
    BJ_SWITCH_ARMS,        /* switch arms both join     — not if/else, reject  */
    BJ_SECOND_HANDLE,      /* two handles, only ONE joined; the other is used  */
    BJ_COUNT
} BJCase;

static const char *bj_name(BJCase c) {
    switch (c) {
    case BJ_BOTH_ARMS:           return "both-arms-join";
    case BJ_HOISTED:             return "hoisted-join";
    case BJ_STRAIGHT:            return "straight-line-join";
    case BJ_BOTH_ARMS_NESTBLOCK: return "both-arms-nested-block";
    case BJ_THEN_ONLY_NO_ELSE:   return "then-only-no-else";
    case BJ_THEN_ONLY_ELSE:      return "then-only-empty-else";
    case BJ_ELSE_ONLY:           return "else-only";
    case BJ_NEITHER:             return "neither-arm-joins";
    case BJ_NESTED_THEN:         return "nested-join-in-then";
    case BJ_NESTED_BOTH:         return "nested-join-both-arms";
    case BJ_LOOP_BODY:           return "join-in-loop-body";
    case BJ_SWITCH_ARMS:         return "join-in-switch-arms";
    case BJ_SECOND_HANDLE:       return "two-handles-one-joined";
    case BJ_COUNT:               break;
    }
    return "?";
}

/* PENDING = a cell whose correct answer requires the not-yet-implemented
 * relaxation. It must currently be REJECTED; if it ever compiles, the
 * relaxation has landed and the cell must be promoted to a plain positive. */
static int bj_is_pending(BJCase c) {
    return c == BJ_BOTH_ARMS || c == BJ_BOTH_ARMS_NESTBLOCK;
}

static int bj_is_negative(BJCase c) {
    switch (c) {
    case BJ_BOTH_ARMS: case BJ_HOISTED: case BJ_STRAIGHT:
    case BJ_BOTH_ARMS_NESTBLOCK:
        return 0;
    case BJ_THEN_ONLY_NO_ELSE: case BJ_THEN_ONLY_ELSE: case BJ_ELSE_ONLY:
    case BJ_NEITHER: case BJ_NESTED_THEN: case BJ_NESTED_BOTH:
    case BJ_LOOP_BODY: case BJ_SWITCH_ARMS: case BJ_SECOND_HANDLE:
        return 1;
    case BJ_COUNT: break;
    }
    return 1;
}

/* Every cell spawns, then places the join(s), then USES the borrowed local.
 * The use is what must be rejected when any path could still be running. */
static void gen(BJCase c, char *out, size_t n) {
    const char *mid = "";
    const char *extra_decl = "";
    switch (c) {
    case BJ_BOTH_ARMS:
        mid = "    if (err == 1) { th.join(); } else { th.join(); }"; break;
    case BJ_HOISTED:
        mid = "    if (err == 1) { } else { }\n    th.join();"; break;
    case BJ_STRAIGHT:
        mid = "    th.join();"; break;
    case BJ_BOTH_ARMS_NESTBLOCK:
        mid = "    if (err == 1) { { th.join(); } } else { { th.join(); } }"; break;
    case BJ_THEN_ONLY_NO_ELSE:
        mid = "    if (err == 1) { th.join(); }"; break;
    case BJ_THEN_ONLY_ELSE:
        mid = "    if (err == 1) { th.join(); } else { }"; break;
    case BJ_ELSE_ONLY:
        mid = "    if (err == 1) { } else { th.join(); }"; break;
    case BJ_NEITHER:
        mid = "    if (err == 1) { } else { }"; break;
    case BJ_NESTED_THEN:
        mid = "    if (err == 1) { if (err == 1) { th.join(); } } else { th.join(); }"; break;
    case BJ_NESTED_BOTH:
        mid = "    if (err == 1) { if (err == 1) { th.join(); } }\n"
              "    else { if (err == 0) { th.join(); } }"; break;
    case BJ_LOOP_BODY:
        mid = "    while (err > 0) { th.join(); err -= 1; }"; break;
    case BJ_SWITCH_ARMS:
        mid = "    switch (err) { 0 => { th.join(); } default => { th.join(); } }"; break;
    /* Two live borrows, and only ONE of them is joined on both arms. A release
     * keyed on "an if/else whose arms joined SOMETHING" instead of on the
     * specific handle would clear both — so this cell is what stops the
     * relaxation from being written per-BRANCH rather than per-HANDLE. */
    case BJ_SECOND_HANDLE:
        extra_decl = "    Work work2;\n"
                     "    ThreadHandle th2 = spawn worker(&work2);\n";
        mid = "    if (err == 1) { th.join(); } else { th.join(); }\n"
              "    u32 leaked = work2.x;\n"
              "    if (leaked == 999) { return 3; }\n"
              "    th2.join();"; break;
    case BJ_COUNT: break;
    }
    snprintf(out, n,
        "struct Work { u32 x; }\n"
        "void worker(*Work w) { w.x = 1; }\n"
        "u32 run(u32 err) {\n"
        "    Work work;\n"
        "    ThreadHandle th = spawn worker(&work);\n"
        "%s"
        "%s\n"
        "    work.x = 2;\n"
        "    return work.x;\n"
        "}\n"
        "u32 main() { return run(0) - 2; }\n",
        extra_decl, mid);
}

static int borrow_reason(const char *eb) {
    return strstr(eb, "borrowed by a scoped spawn") != NULL;
}

static int run_cell(BJCase c, const char *code) {
    total++;
    int neg = bj_is_negative(c);
    int pending = bj_is_pending(c);
    const char *name = bj_name(c);

    FILE *f = fopen("/tmp/_zer_bj.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_bj.zer -o /tmp/_zer_bj.c >/tmp/_zer_bj.err 2>&1", zerc_path);
    int rc = system(cmd);

    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_bj.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }

    if (pending) {
        /* Inverted expectation: today this MUST be rejected, and by the borrow
         * rule (not by something unrelated, which would make the cell vacuous). */
        if (rc != 0 && borrow_reason(eb)) { passed++; return 1; }
        failed++;
        if (rc == 0) {
            fprintf(stderr, "  GAP CLOSED [%s] — this now COMPILES, so the scoped-borrow\n"
                            "        relaxation has landed. Remove it from bj_is_pending()\n"
                            "        and let it be gated as a plain positive.\n", name);
        } else {
            fprintf(stderr, "  FAIL [WRONG-REASON] %s — rejected, but not by the borrow rule:\n"
                            "    %.150s\n", name, eb);
        }
        return 0;
    }

    if (neg) {
        if (rc == 0) {
            failed++;
            fprintf(stderr, "  FAIL [ACCEPT-UNSAFE] %s — a path reaches the use without joining,\n"
                            "        and the compiler accepted it\n", name);
            fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
            return 0;
        }
        if (!borrow_reason(eb)) {
            failed++;
            fprintf(stderr, "  FAIL [WRONG-REASON] %s — rejected, but not by the borrow rule:\n"
                            "    %.150s\n", name, eb);
            return 0;
        }
        passed++; return 1;
    }
    if (rc == 0) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — every path joins, yet it was rejected:\n"
                    "    %.150s\n", name, eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== scoped-borrow release x join-placement matrix ===\n");
    fprintf(stderr, "    NEG:     some path reaches the use unjoined -> must REJECT for the borrow reason\n");
    fprintf(stderr, "    POS:     every path joins -> must COMPILE\n");
    fprintf(stderr, "    PENDING: needs the unimplemented relaxation -> must still be rejected;\n");
    fprintf(stderr, "             if it starts compiling, promote the cell (see the file header)\n\n");

    char buf[1024];
    for (BJCase c = 0; c < BJ_COUNT; c++) {
        gen(c, buf, sizeof(buf));
        int ok = run_cell(c, buf);
        fprintf(stderr, "  [%-7s][%-24s] %s\n",
                bj_is_pending(c) ? "PENDING" : (bj_is_negative(c) ? "neg" : "pos"),
                bj_name(c), ok ? "ok" : "*** FAIL ***");
    }

    fprintf(stderr, "\n=== borrow-join matrix: %d cells, %d passed, %d failed ===\n",
            total, passed, failed);
    if (failed == 0) fprintf(stderr, "BORROW-JOIN MATRIX CLEAN\n");
    return failed == 0 ? 0 : 1;
}
