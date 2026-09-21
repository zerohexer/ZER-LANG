/* test_vrp_position_matrix.c — the loop/control-flow POSITION oracle (2026-09-15).
 *
 * WHY THIS GRID EXISTS. ZER's value-range analysis runs on the AST, so every
 * control-flow construct must MANUALLY widen / narrow / snapshot / restore the
 * range environment at every POSITION it evaluates an expression. CLAUDE.md's
 * multi-site table lists this class with "NO auto-gate — checklist every
 * control-flow kind, and every POSITION a loop evaluates (init / cond / step /
 * body)". The checklist has now failed twice:
 *
 *   BUG-1015 (2026-09-13) — the for/while CONDITION was checked under the
 *   PRE-loop range. `for (i = 0; arr[i] > 0; i += 1)` proved `arr[i]` against
 *   i in [0,0], emitted a bare read, ASan global-buffer-overflow.
 *
 *   BUG-1017 (2026-09-15) — the for STEP, same defect, at the one position
 *   BUG-1015 did not cover. `for (k = 0; k < 8; k += a[k] + 1)` on a u32[4]
 *   proved `a[k]` against k in [0,0] and emitted a bare `k += a[k] + 1;` —
 *   no bounds check, no auto-guard and NO WARNING. ASan stack-buffer-overflow.
 *
 * A class that has silently leaked twice needs a gate, not a third checklist.
 *
 * THE ORACLE. A fixed-array index the checker PROVES in range produces
 * literally nothing: no runtime check, no auto-guard, and no diagnostic. So
 * "compiled with zero diagnostics" is the exact, observable signature of the
 * hole — and of correct elision. Two cell kinds:
 *
 *   STALE  — the index's value at the moment that position is EVALUATED is not
 *            the value its pre-position range describes (it is loop-carried, or
 *            written later in the same iteration, or reached by a back edge).
 *            The compiler must SAY SOMETHING: an error (provably out of bounds)
 *            or a warning (auto-guard inserted). Silence = the compiler proved
 *            a fact that is not true = a silent OOB. THIS IS THE HOLE.
 *
 *   PROVEN — the index really is in range at that position by the construct's
 *            own bound. The compiler must say NOTHING (zero-overhead elision).
 *            A diagnostic here means a fix for a STALE cell over-widened and
 *            cost precision the language advertises.
 *
 * Every cell uses a FIXED ARRAY deliberately. A slice carries a dynamic `.len`
 * and keeps its runtime bounds check whatever VRP concludes, so a slice cell
 * cannot discriminate — that is exactly why both BUG-1015 and BUG-1017 were
 * invisible until someone wrote the fixed-array form.
 *
 * EMIT-ONLY harness (`-o`, no gcc): isolates the CHECKER's verdict. Honors
 * ZER_MATRIX_ZERC so a new cell can be shown to FIRE against a pre-fix build.
 * -Wswitch-enforced scenario enum: a new position must be classified here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static int silent_hole = 0, invalid_probe = 0, over_reject = 0;
static const char *zerc_path = NULL;

static void find_zerc(void) {
    const char *env = getenv("ZER_MATRIX_ZERC");
    if (env && *env) { zerc_path = env; return; }
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    if (system("test -x /tmp/zerc") == 0) { zerc_path = "/tmp/zerc"; return; }
    fprintf(stderr, "ERROR: cannot find zerc (set ZER_MATRIX_ZERC)\n");
    exit(2);
}

/* Run the compiler on `code`; capture stderr into `eb`. Returns the exit status. */
static int compile_probe(const char *code, char *eb, size_t ebsz) {
    FILE *f = fopen("/tmp/_zer_vp.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); exit(2); }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_vp.zer -o /tmp/_zer_vp.c >/dev/null 2>/tmp/_zer_vp.err",
             zerc_path);
    int rc = system(cmd);
    eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_vp.err", "r");
    if (e) { size_t r = fread(eb, 1, ebsz - 1, e); eb[r] = 0; fclose(e); }
    return rc;
}

static int is_parse_error(const char *eb) {
    return strstr(eb, "expected ") || strstr(eb, "unexpected") ||
           strstr(eb, "parse failed") || strstr(eb, "parse error");
}

/* A bounds VERDICT about the index: either the auto-guard warning or the
 * provably-out-of-bounds error. Both name the array and the index. */
static int has_bounds_verdict(const char *eb) {
    return strstr(eb, "not proven in range") ||
           strstr(eb, "always out of bounds") ||
           strstr(eb, "runs PAST the end");
}

/* STALE: the compiler must not be SILENT about this index. */
static int run_stale(const char *name, const char *code) {
    total++;
    char eb[8192];
    compile_probe(code, eb, sizeof(eb));
    if (is_parse_error(eb)) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — parse error, not a bounds verdict\n", name);
        fprintf(stderr, "    %.140s\n", eb);
        return 0;
    }
    if (has_bounds_verdict(eb)) { passed++; return 1; }
    failed++; silent_hole++;
    fprintf(stderr, "  FAIL [SILENT-HOLE] %s — fixed-array index at this position was\n", name);
    fprintf(stderr, "        PROVEN in range under a STALE environment: no guard, no warning.\n");
    fprintf(stderr, "    compiler said: %.140s\n", eb[0] ? eb : "(nothing)");
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

/* PROVEN: the compiler must be SILENT (zero-overhead elision kept). */
static int run_proven(const char *name, const char *code) {
    total++;
    char eb[8192];
    int rc = compile_probe(code, eb, sizeof(eb));
    if (rc != 0 || has_bounds_verdict(eb)) {
        failed++; over_reject++;
        fprintf(stderr, "  FAIL [OVER-REJECT] %s — an index this construct's own bound\n", name);
        fprintf(stderr, "        proves in range lost its elision (guard or diagnostic emitted).\n");
        fprintf(stderr, "    compiler said: %.140s\n", eb);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    passed++; return 1;
}

typedef enum {
    /* ---- STALE: the position's environment is not the evaluation-time value ---- */
    VP_FOR_COND,          /* BUG-1015 — index in the for CONDITION */
    VP_FOR_STEP,          /* BUG-1017 — index in the for STEP */
    VP_FOR_STEP_EXPRINIT, /* BUG-1017, expression-form init (`for (k = 0; ...)`) */
    VP_FOR_STEP_CALLARG,  /* BUG-1017, index nested in a call argument in the step */
    VP_FOR_STEP_NESTED,   /* BUG-1017, inner loop's step under the OUTER loop's range */
    VP_FOR_BODY,          /* VRP#4 — a non-counter var the body writes */
    VP_WHILE_COND,        /* BUG-1015 — index in the while CONDITION */
    VP_WHILE_BODY,        /* body read before the same iteration's write */
    VP_DOWHILE_COND,      /* do-while condition, after the body advanced the index */
    VP_DOWHILE_BODY,      /* do-while body */
    VP_GOTO_BACK,         /* backward goto re-enters with a changed index */
    VP_SWITCH_ARM,        /* switch arm under a loop-widened index */
    VP_ADDR_TAKEN,        /* B7 — index mutated through `&k` in the body */
    VP_FOR_BODY_LOWERED,  /* BUG-1034 — a SIGNED counter lowered in the body */
    VP_FOR_STEP_DEC,      /* BUG-1034 — a SIGNED counter with a DECREMENTING step */

    /* ---- PROVEN: the construct's own bound really does prove it ---- */
    VP_FOR_BODY_OK,       /* counter in the body, bounded by the loop condition */
    VP_FOR_STEP_OK,       /* counter in the STEP, bounded by the loop condition */
    VP_WHILE_BODY_OK,     /* counter in a while body, bounded by the condition */
    VP_CONST_OK,          /* a literal index inside the array */
    VP_FOR_SIGNED_OK,     /* BUG-1034 pin: signed counter, `i = i + 1` step, read-only body */
    VPSCEN_COUNT
} VPScenario;

static int scenario_is_stale(VPScenario s) {
    switch (s) {
        case VP_FOR_COND: case VP_FOR_STEP: case VP_FOR_STEP_EXPRINIT:
        case VP_FOR_STEP_CALLARG: case VP_FOR_STEP_NESTED: case VP_FOR_BODY:
        case VP_WHILE_COND: case VP_WHILE_BODY:
        case VP_DOWHILE_COND: case VP_DOWHILE_BODY:
        case VP_GOTO_BACK: case VP_SWITCH_ARM: case VP_ADDR_TAKEN:
        case VP_FOR_BODY_LOWERED: case VP_FOR_STEP_DEC:
            return 1;
        case VP_FOR_BODY_OK: case VP_FOR_STEP_OK:
        case VP_WHILE_BODY_OK: case VP_CONST_OK: case VP_FOR_SIGNED_OK:
            return 0;
        case VPSCEN_COUNT: break;
    }
    return 1;   /* unreachable under -Werror=switch; conservative anyway */
}

static const char *scen_name(VPScenario s) {
    switch (s) {
        case VP_FOR_COND:          return "for/cond";
        case VP_FOR_STEP:          return "for/step";
        case VP_FOR_STEP_EXPRINIT: return "for/step(expr-init)";
        case VP_FOR_STEP_CALLARG:  return "for/step(call-arg)";
        case VP_FOR_STEP_NESTED:   return "for/step(nested)";
        case VP_FOR_BODY:          return "for/body(other-var)";
        case VP_WHILE_COND:        return "while/cond";
        case VP_WHILE_BODY:        return "while/body";
        case VP_DOWHILE_COND:      return "do-while/cond";
        case VP_DOWHILE_BODY:      return "do-while/body";
        case VP_GOTO_BACK:         return "goto/back-edge";
        case VP_SWITCH_ARM:        return "switch/arm";
        case VP_ADDR_TAKEN:        return "for/body(&k alias)";
        case VP_FOR_BODY_LOWERED:  return "for/body(signed counter lowered)";
        case VP_FOR_STEP_DEC:      return "for/step(signed counter decremented)";
        case VP_FOR_BODY_OK:       return "for/body PROVEN";
        case VP_FOR_STEP_OK:       return "for/step PROVEN";
        case VP_WHILE_BODY_OK:     return "while/body PROVEN";
        case VP_CONST_OK:          return "literal index PROVEN";
        case VP_FOR_SIGNED_OK:     return "for/body(signed, i = i + 1) PROVEN";
        case VPSCEN_COUNT:         break;
    }
    return "?";
}

static void gen(VPScenario s, char *buf, size_t n) {
    switch (s) {
    case VP_FOR_COND:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 t = 0;\n"
            "    for (u32 k = 0; a[k] == 0; k += 1) { t += 1; }\n"
            "    return t;\n}\n");
        break;
    case VP_FOR_STEP:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 t = 0;\n"
            "    for (u32 k = 0; k < 8; k += a[k] + 1) { t += 1; }\n"
            "    return t;\n}\n");
        break;
    case VP_FOR_STEP_EXPRINIT:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 k = 0;\n"
            "    u32 t = 0;\n"
            "    for (k = 0; k < 8; k += a[k] + 1) { t += 1; }\n"
            "    return t;\n}\n");
        break;
    case VP_FOR_STEP_CALLARG:
        snprintf(buf, n,
            "u32 bump(u32 v) { return v + 1; }\n"
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 t = 0;\n"
            "    for (u32 k = 0; k < 8; k += bump(a[k])) { t += 1; }\n"
            "    return t;\n}\n");
        break;
    case VP_FOR_STEP_NESTED:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 t = 0;\n"
            "    for (u32 o = 0; o < 2; o += 1) {\n"
            "        for (u32 k = 0; k < 8; k += a[k] + 1) { t += 1; }\n"
            "    }\n"
            "    return t;\n}\n");
        break;
    case VP_FOR_BODY:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 j = 0;\n"
            "    u32 t = 0;\n"
            "    for (u32 i = 0; i < 8; i += 1) { t += a[j]; j += 1; }\n"
            "    return t;\n}\n");
        break;
    case VP_WHILE_COND:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 k = 0;\n"
            "    while (a[k] == 0) { k += 1; if (k > 50) { return 1; } }\n"
            "    return k;\n}\n");
        break;
    case VP_WHILE_BODY:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 k = 0;\n"
            "    u32 t = 0;\n"
            "    u32 n = 0;\n"
            "    while (n < 3) { t += a[k]; k += 3; n += 1; }\n"
            "    return t;\n}\n");
        break;
    case VP_DOWHILE_COND:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 k = 0;\n"
            "    u32 t = 0;\n"
            "    do { t += 1; k += 3; if (t > 50) { return 1; } } while (a[k] == 0);\n"
            "    return t;\n}\n");
        break;
    case VP_DOWHILE_BODY:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 k = 0;\n"
            "    u32 t = 0;\n"
            "    u32 n = 0;\n"
            "    do { t += a[k]; k += 3; n += 1; } while (n < 3);\n"
            "    return t;\n}\n");
        break;
    case VP_GOTO_BACK:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 k = 0;\n"
            "    u32 n = 0;\n"
            "top:\n"
            "    n += 1;\n"
            "    if (n > 3) { return n; }\n"
            "    u32 t = a[k];\n"
            "    k += 3;\n"
            "    goto top;\n"
            "}\n");
        break;
    case VP_SWITCH_ARM:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 k = 0;\n"
            "    u32 sel = 1;\n"
            "    while (sel < 2) { sel += 1; k += 9; }\n"
            "    switch (sel) {\n"
            "        1 => { return a[k]; }\n"
            "        default => { return a[k]; }\n"
            "    }\n"
            "}\n");
        break;
    case VP_ADDR_TAKEN:
        snprintf(buf, n,
            "void bump(*u32 p) { p[0] = 9; }\n"
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 t = 0;\n"
            "    for (u32 k = 0; k < 2; k += 1) { t += a[k]; bump(&k); }\n"
            "    return t;\n}\n");
        break;

    case VP_FOR_BODY_LOWERED:
        /* BUG-1034: the lower bound [init, ...] assumed the counter only goes UP.
         * i = 2, then -3+1 = -2 < 4: a[-2] was written with no check (ASan
         * stack-buffer-underflow). An UNSIGNED counter is safe here — push_var_range
         * clamps its minimum to 0 — so the cell is deliberately signed. */
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 t = 0;\n"
            "    u32 g = 0;\n"
            "    for (i32 i = 2; i < 4; i += 1) { t += a[i]; i -= 5; g += 1; if (g > 3) { return t; } }\n"
            "    return t;\n}\n");
        break;
    case VP_FOR_STEP_DEC:
        /* BUG-1034: the STEP itself lowers the counter: 3, 2, 1, 0, -1 ... */
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 t = 0;\n"
            "    u32 g = 0;\n"
            "    for (i32 i = 3; i < 4; i -= 1) { t += a[i]; g += 1; if (g > 6) { return t; } }\n"
            "    return t;\n}\n");
        break;
    case VP_FOR_SIGNED_OK:
        /* BUG-1034 precision pin: a signed counter whose only writer is a
         * non-negative constant increment, spelled `i = i + 1`, keeps its
         * proven [0,3] and the body read emits nothing. */
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 s = 0;\n"
            "    for (i32 i = 0; i < 4; i = i + 1) { s += a[i]; }\n"
            "    return s;\n}\n");
        break;
    case VP_FOR_BODY_OK:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 s = 0;\n"
            "    for (u32 i = 0; i < 4; i += 1) { s += a[i]; }\n"
            "    return s;\n}\n");
        break;
    case VP_FOR_STEP_OK:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    a[0] = 1; a[1] = 1; a[2] = 1; a[3] = 1;\n"
            "    u32 s = 0;\n"
            "    for (u32 j = 0; j < 4; j += a[j]) { s += 1; }\n"
            "    return s;\n}\n");
        break;
    case VP_WHILE_BODY_OK:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 i = 0;\n"
            "    u32 s = 0;\n"
            "    while (i < 4) { s += a[i]; i += 1; }\n"
            "    return s;\n}\n");
        break;
    case VP_CONST_OK:
        snprintf(buf, n,
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    a[2] = 5;\n"
            "    return a[2];\n}\n");
        break;
    case VPSCEN_COUNT: buf[0] = 0; break;
    }
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== VRP control-flow POSITION matrix ===\n");
    fprintf(stderr, "    STALE : index evaluated under a range that is not its value there\n");
    fprintf(stderr, "            -> the compiler must SAY SOMETHING (guard warning or error)\n");
    fprintf(stderr, "    PROVEN: index really in range by the construct's own bound\n");
    fprintf(stderr, "            -> the compiler must say NOTHING (elision kept)\n\n");

    char buf[2048];
    int grid_ok = 1, cells = 0;
    for (VPScenario s = 0; s < VPSCEN_COUNT; s++) {
        cells++;
        int stale = scenario_is_stale(s);
        char nm[160];
        snprintf(nm, sizeof(nm), "%s/%s", stale ? "stale" : "proven", scen_name(s));
        gen(s, buf, sizeof(buf));
        int ok = stale ? run_stale(nm, buf) : run_proven(nm, buf);
        fprintf(stderr, "  [%-6s][%-22s] %s\n",
                stale ? "stale" : "proven", scen_name(s), ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }

    fprintf(stderr, "\n=== vrp-position-matrix: %d/%d cells correct ===\n", passed, cells);
    fprintf(stderr, "    silent holes: %d | invalid probes: %d | over-rejections: %d\n",
            silent_hole, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "VRP POSITION MATRIX HAS HOLES — a control-flow position evaluates an\n");
        fprintf(stderr, "expression under a range environment that does not describe the value\n");
        fprintf(stderr, "there. A SILENT-HOLE cell is a fixed-array access with no bounds check,\n");
        fprintf(stderr, "no auto-guard and no diagnostic (BUG-1015 / BUG-1017 class).\n");
        return 1;
    }
    return 0;
}
