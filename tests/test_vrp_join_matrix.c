/* test_vrp_join_matrix.c — VRP range-JOIN completeness oracle (2026-08-20).
 *
 * WHY THIS EXISTS. CLAUDE.md's multi-site table lists "VRP range JOIN (merge at
 * a control-flow join)" with the gate column reading:
 *
 *     "mirror vrp_snap_take/restore/join per node-kind; a missing kind = silent
 *      OOB. NO auto-gate — checklist every control-flow kind"
 *
 * A checklist is not a net. This class already produced BH-18 #2 (a branch-local
 * narrowing leaked past a join, the compiler proved an OOB index safe, and it
 * emitted no bounds check — CRITICAL, red-team-found, not proof-found). The
 * failure mode is a SILENT out-of-bounds access: no diagnostic at compile time,
 * no trap at runtime, and on bare metal no fault either. Exactly the class this
 * grid has to make loud.
 *
 * THE INVARIANT UNDER TEST — one sentence:
 *     at a control-flow join, a variable's range is the JOIN of the ranges on
 *     ALL incoming paths — never the range from one path alone.
 *
 * Both directions of that invariant fail differently, so both are axes:
 *   NARROW  a branch PROVES i < 4; the range must NOT survive the join, or an
 *           unbounded i is treated as bounded  -> under-rejection -> silent OOB.
 *   WIDEN   i is proven in range, then a branch WRITES something out of range;
 *           the pre-branch range must NOT survive  -> same silent OOB.
 *
 * MEASUREMENT — behavioural, and the expected value is derivable rather than
 * tabulated per cell (the property that made the defer/goto grid work):
 *
 *   HAZARD cells drive the index to 10 against a `u8[4]`. A correct compiler
 *   either rejects, traps, or inserts the auto-guard — whose emitted form is an
 *   early `return 0`. The tail returns 7. So the rule is EXACT and closed:
 *
 *       SAFE   iff  rejected, or exit 0 (guard fired), or exit 133 (trap)
 *       UNSAFE otherwise — exit 7 (ran to the tail unguarded), 134 (the OOB
 *              write tripped the stack protector), 139 (segv), 127 (the binary
 *              never ran) or anything else.
 *
 *   Stating it as a CLOSED SAFE-SET rather than "not 7" is load-bearing, and it
 *   is the second thing this file got wrong before it was measured. The first
 *   draft read "exit 7 means leak, anything else passes"; against a deliberately
 *   broken compiler the real signature turned out to be exit 134 — the out-of-
 *   bounds write smashes the stack canary before control ever reaches the tail —
 *   so every cell PASSED on a compiler with the join disabled. The same open
 *   rule also swallowed exit 127 (a binary that was never produced, the
 *   `zerc -o` path gotcha), which would have made the whole grid vacuous.
 *
 *   CONTROL cells drive the index to a value that IS provably in range on every
 *   incoming path, so the join must stay in range and the program must reach
 *   the tail: exit 7. A cell that exits 0 here means the guard fired
 *   spuriously, i.e. the join lost precision. Controls therefore pin the
 *   over-rejection boundary in the same grid that pins the soundness one, which
 *   is what stops a future "fix" from buying soundness with a blanket guard.
 *
 * Axes are C enums switched with NO default, so adding a JoinKind fails GCC
 * -Wswitch until every generator/name/expectation site handles it. The grid
 * cannot silently shrink (tools/walker_default_audit.sh discipline).
 *
 * VERIFIED TO DISCRIMINATE: not by construction but by measurement — see the
 * commit that adds this file. Every HAZARD cell was confirmed to be guarded on
 * today's compiler, and the harness was confirmed to report exit 7 as a failure
 * by inverting a cell's expectation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static int leaked = 0, over_rejected = 0, unexpected_reject = 0;
static const char *zerc_path = NULL;

static void find_zerc(void) {
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    if (system("test -x /tmp/zerc") == 0) { zerc_path = "/tmp/zerc"; return; }
    fprintf(stderr, "ERROR: cannot find zerc\n");
    exit(2);
}

/* ---- axes ---------------------------------------------------------------- */

/* Every control-flow construct that creates a JOIN. One cell per kind is the
 * whole point: BH-18 #2 was ONE kind missing its snapshot/restore. */
typedef enum {
    JK_IF_THEN,     /* if (c) { ... }                      */
    JK_IF_ELSE,     /* if (c) { } else { ... }             */
    JK_WHILE,       /* while (c) { ... }   — loop exit join */
    JK_FOR,         /* for (; c; ...) { }  — loop exit join */
    JK_DOWHILE,     /* do { } while (c);   — first-iteration asymmetry */
    JK_SWITCH,      /* switch (v) { arm => { ... } ... }   */
    JK_GOTO,        /* goto/label — join at the label      */
    JK_NESTED_IF,   /* if (c) { if (d) { ... } }           */
    JK_COUNT
} JoinKind;

typedef enum {
    DIR_NARROW,     /* branch proves a bound; must not survive the join */
    DIR_WIDEN,      /* branch destroys a bound; must not be forgotten   */
    DIR_COUNT
} Direction;

typedef enum {
    PL_HAZARD,      /* index really is out of range at the join  -> must be safe */
    PL_CONTROL,     /* index really is in range on every path    -> must compile through */
    PL_COUNT
} Payload;

static const char *jk_name(JoinKind k) {
    switch (k) {
    case JK_IF_THEN:   return "if-then";
    case JK_IF_ELSE:   return "if-else";
    case JK_WHILE:     return "while";
    case JK_FOR:       return "for";
    case JK_DOWHILE:   return "do-while";
    case JK_SWITCH:    return "switch-arm";
    case JK_GOTO:      return "goto-label";
    case JK_NESTED_IF: return "nested-if";
    case JK_COUNT:     break;
    }
    return "?";
}
static const char *dir_name(Direction d) {
    switch (d) {
    case DIR_NARROW: return "narrow";
    case DIR_WIDEN:  return "widen";
    case DIR_COUNT:  break;
    }
    return "?";
}
static const char *pl_name(Payload p) {
    switch (p) {
    case PL_HAZARD:  return "hazard";
    case PL_CONTROL: return "control";
    case PL_COUNT:   break;
    }
    return "?";
}

/* ---- generators ---------------------------------------------------------- */
/* Each generator emits the BODY between the declaration of `i` and the
 * post-join use. `i` is always laundered through a volatile so no constant
 * folding can pre-answer the question the join is supposed to answer. */

/* DIR_NARROW: i starts UNKNOWN (10 for hazard, 2 for control) and a branch
 * narrows it to < 4. The post-join use must not inherit that. For the control
 * the value really is in range on every path, so the join must stay proven. */
static void gen_narrow(JoinKind k, int hazard, char *out, size_t cap) {
    /* hazard: i = 10 (out of range).  control: both join paths assign 1 or 2. */
    if (!hazard) {
        /* CONTROL — the join of two in-range assignments must remain in range. */
        switch (k) {
        case JK_IF_THEN:
            snprintf(out, cap, "    i = 1;\n    if (gv > 0) { i = 2; }\n"); return;
        case JK_IF_ELSE:
            snprintf(out, cap, "    if (gv > 0) { i = 2; } else { i = 1; }\n"); return;
        case JK_WHILE:
            snprintf(out, cap, "    i = 0;\n    while (i < 3) { i += 1; }\n"); return;
        case JK_FOR:
            snprintf(out, cap, "    i = 0;\n    for (u32 k2 = 0; k2 < 3; k2 += 1) { i += 1; }\n"); return;
        case JK_DOWHILE:
            snprintf(out, cap, "    i = 1;\n    do { i = 2; } while (i < 0);\n"); return;
        case JK_SWITCH:
            snprintf(out, cap,
                "    i = 1;\n    switch (sel) { 3 => { i = 2; } default => { i = 3; } }\n"); return;
        case JK_GOTO:
            snprintf(out, cap,
                "    i = 1;\n    if (gv > 0) { goto lo; }\n    goto lj;\nlo:\n    i = 2;\nlj:\n"); return;
        case JK_NESTED_IF:
            snprintf(out, cap, "    i = 1;\n    if (gv > 0) { if (gv > 0) { i = 2; } }\n"); return;
        case JK_COUNT: break;
        }
        out[0] = '\0'; return;
    }
    /* HAZARD — i really is 10; only the BRANCH proves anything about it. */
    switch (k) {
    case JK_IF_THEN:
        snprintf(out, cap, "    if (i < 4) { buf[i] = 1; }\n"); return;
    case JK_IF_ELSE:
        snprintf(out, cap, "    if (i >= 4) { sink = 1; } else { buf[i] = 1; }\n"); return;
    case JK_WHILE:
        snprintf(out, cap, "    while (i < 4) { buf[i] = 1; i += 1; }\n"); return;
    case JK_FOR:
        snprintf(out, cap, "    for (; i < 4; i += 1) { buf[i] = 1; }\n"); return;
    case JK_DOWHILE:
        snprintf(out, cap, "    do { sink = 1; } while (i < 4);\n"); return;
    case JK_SWITCH:
        snprintf(out, cap,
            "    switch (i) { 0 => { sink = 1; } 1 => { sink = 2; } default => { sink = 3; } }\n"); return;
    case JK_GOTO:
        snprintf(out, cap,
            "    if (i < 4) { goto lo; }\n    goto lj;\nlo:\n    buf[i] = 1;\nlj:\n"); return;
    case JK_NESTED_IF:
        snprintf(out, cap, "    if (gv > 0) { if (i < 4) { buf[i] = 1; } }\n"); return;
    case JK_COUNT: break;
    }
    out[0] = '\0';
}

/* DIR_WIDEN: i starts PROVEN in range (2), then a branch writes 10 (hazard) or
 * another in-range value (control). The pre-branch range must not survive. */
static void gen_widen(JoinKind k, int hazard, char *out, size_t cap) {
    const char *v = hazard ? "10" : "3";
    switch (k) {
    case JK_IF_THEN:
        snprintf(out, cap, "    i = 2;\n    if (gv > 0) { i = %s; }\n", v); return;
    case JK_IF_ELSE:
        snprintf(out, cap, "    i = 2;\n    if (gv > 5) { sink = 1; } else { i = %s; }\n", v); return;
    case JK_WHILE:
        /* i climbs from 2 to the bound, which is only known at runtime */
        snprintf(out, cap, "    i = 2;\n    while (i < lim) { i += 1; }\n"); return;
    case JK_FOR:
        snprintf(out, cap, "    i = 2;\n    for (u32 k2 = 0; k2 < lim; k2 += 1) { i = %s; }\n", v); return;
    case JK_DOWHILE:
        snprintf(out, cap, "    i = 2;\n    do { i = %s; } while (gv > 5);\n", v); return;
    case JK_SWITCH:
        snprintf(out, cap, "    i = 2;\n    switch (sel) { 3 => { i = %s; } default => { } }\n", v); return;
    case JK_GOTO:
        snprintf(out, cap,
            "    i = 2;\n    if (gv > 0) { goto wo; }\n    goto wj;\nwo:\n    i = %s;\nwj:\n", v); return;
    case JK_NESTED_IF:
        snprintf(out, cap, "    i = 2;\n    if (gv > 0) { if (gv > 0) { i = %s; } }\n", v); return;
    case JK_COUNT: break;
    }
    out[0] = '\0';
}

/* For DIR_WIDEN + JK_WHILE the loop bound `lim` decides the final value, so the
 * hazard/control distinction rides on lim rather than on an assigned literal. */
static int widen_lim(JoinKind k, int hazard) {
    if (k == JK_WHILE) return hazard ? 10 : 3;
    return hazard ? 1 : 1;   /* other kinds: lim only drives the for-loop trip count */
}

static void gen(JoinKind k, Direction d, Payload p, char *buf, size_t cap) {
    char body[512];
    int hazard = (p == PL_HAZARD);
    if (d == DIR_NARROW) gen_narrow(k, hazard, body, sizeof(body));
    else                 gen_widen (k, hazard, body, sizeof(body));

    /* The initial value only matters for DIR_NARROW (the widen generators all
     * assign i themselves). 10 is out of range for u8[4]; 2 is in range. */
    int init = (d == DIR_NARROW) ? (hazard ? 10 : 0) : 0;
    int lim  = widen_lim(k, hazard);
    int sel  = 3;

    snprintf(buf, cap,
        "u32 sink;\n"
        "u32 main() {\n"
        "    u8[4] buf;\n"
        "    volatile u32 gvv = 1; u32 gv = gvv;\n"
        "    volatile u32 iv = %d; u32 i = iv;\n"
        "    volatile u32 limv = %d; u32 lim = limv;\n"
        "    volatile u32 selv = %d; u32 sel = selv;\n"
        "%s"                       /* the join construct */
        "    buf[i] = 2;\n"        /* THE POST-JOIN USE — the whole point */
        "    sink = buf[0];\n"
        "    return 7;\n"          /* reaching here unguarded == the leak */
        "}\n",
        init, lim, sel, body);
}

/* ---- cell runner --------------------------------------------------------- */

/* A HAZARD cell is SAFE if the compiler refuses it, if it traps at runtime, or
 * if the auto-guard fires (early `return 0`). It is UNSAFE only if the program
 * runs all the way to the tail, because that means the OOB access executed with
 * nothing standing in front of it.
 *
 * A CONTROL cell must reach the tail: anything else is the join losing
 * precision, which is how a "fix" that guards everything would pass a
 * soundness-only grid. */
static void run_cell(JoinKind k, Direction d, Payload p) {
    char src[2048], nm[128];
    gen(k, d, p, src, sizeof(src));
    snprintf(nm, sizeof(nm), "%s/%s/%s", jk_name(k), dir_name(d), pl_name(p));
    total++;

    FILE *f = fopen("/tmp/_zer_vrpj.zer", "w");
    if (!f) { failed++; return; }
    fputs(src, f); fclose(f);

    char cmd[512];
    /* CLAUDE.md "zerc -o gotchas": a non-.c `-o` path builds the exe NEXT TO
     * THE SOURCE, so the binary lands at /tmp/_zer_vrpj, not at the -o path. */
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_vrpj.zer -o /tmp/_zer_vrpj >/tmp/_zer_vrpj.err 2>&1", zerc_path);
    int compiled = (system(cmd) == 0);

    if (!compiled) {
        if (p == PL_HAZARD) { passed++; return; }   /* refusing is a sound answer */
        fprintf(stderr, "      %s: REJECTED but the index is provably in range "
                        "on every path (join lost precision)\n", nm);
        unexpected_reject++; failed++; return;
    }

    int rc = system("/tmp/_zer_vrpj >/dev/null 2>&1");
    int code = (rc == -1) ? -1 : ((rc >> 8) & 0xff);

    if (p == PL_HAZARD) {
        /* CLOSED safe-set — see the header. Anything outside it means the guard
         * did not stand in front of the access. */
        if (code == 0 || code == 133) { passed++; return; }
        if (code == 7) {
            fprintf(stderr, "      %s: *** VRP LEAKED ACROSS THE JOIN — the "
                            "post-join index ran UNGUARDED to the tail\n", nm);
        } else if (code == 134 || code == 139) {
            fprintf(stderr, "      %s: *** VRP LEAKED ACROSS THE JOIN — the "
                            "post-join index wrote OUT OF BOUNDS (exit %d: stack "
                            "protector / segv)\n", nm, code);
        } else {
            fprintf(stderr, "      %s: *** unexpected exit %d — the binary did not "
                            "run as expected; treat as a failure, not a pass\n", nm, code);
        }
        leaked++; failed++; return;
    }
    if (code == 7) { passed++; return; }
    fprintf(stderr, "      %s: guard fired on a provably-in-range index "
                    "(exit %d) — the join lost precision\n", nm, code);
    over_rejected++; failed++;
}

int main(void) {
    find_zerc();
    printf("=== VRP range-JOIN matrix (join-kind x direction x payload) ===\n");
    for (int k = 0; k < JK_COUNT; k++)
        for (int d = 0; d < DIR_COUNT; d++)
            for (int p = 0; p < PL_COUNT; p++)
                run_cell((JoinKind)k, (Direction)d, (Payload)p);

    printf("  cells: %d   passed: %d   failed: %d\n", total, passed, failed);
    if (leaked)         printf("  *** %d cell(s) LEAKED a range across a join (silent OOB)\n", leaked);
    if (over_rejected)  printf("  %d cell(s) lost precision at the join\n", over_rejected);
    if (unexpected_reject) printf("  %d cell(s) rejected a safe program\n", unexpected_reject);
    if (failed == 0) printf("VRP JOIN MATRIX CLEAN\n");
    return failed ? 1 : 0;
}
