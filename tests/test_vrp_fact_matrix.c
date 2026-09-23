/* test_vrp_fact_matrix.c — the VRP FACT-VALIDITY oracle (2026-09-23, BUG-1090..1100).
 *
 * WHY THIS GRID EXISTS. test_vrp_position_matrix asks "is the index checked under
 * the range that holds WHERE it is evaluated?". This grid asks the other half:
 * "is the RANGE ITSELF still a true fact about THIS variable?". One audit found
 * the same bounds-elision hole reached through six independent routes, each a
 * different reason a recorded fact stopped being true:
 *
 *   IDENTITY    — the fact belonged to a DIFFERENT variable of the same name
 *                 (ranges were keyed by name: a shadow read the outer range).
 *   CONSTANT    — the fact was computed with the wrong arithmetic (an untyped
 *                 int64 fold of a u32 tree: `(0 - 1) / 1073741824` is 0 in
 *                 int64 and 3 at run time).
 *   INVALIDATE  — something else changed the value after the fact was recorded
 *                 (a store through a pointer, a call on a back edge, a yield, an
 *                 alias that outlived the block that made it).
 *   HOIST       — the guard tested the value at the START of the statement and
 *                 the statement itself changed it before the access.
 *   SUMMARY     — a function's return range was read at the END of its body
 *                 instead of at each return.
 *
 * THE ORACLE IS A RUNTIME ONE. Every CATCH cell is a program whose access is
 * out of bounds AT RUN TIME while a broken compiler would PROVE it in range. The
 * program returns 42 right after the access. So:
 *     exit 42            -> the access ran unchecked: SILENT HOLE
 *     exit 0             -> the auto-guard returned early: caught
 *     exit 133           -> the inline bounds check trapped: caught
 *     compile error that is a BOUNDS verdict -> caught at compile time
 *     anything else      -> UNEXPECTED (a crash from the OOB write lands here too,
 *                           so a hole can never pass by corrupting memory)
 * The OOB target is a small GLOBAL array followed by padding, so the unchecked
 * write of a broken build lands in .bss and the program still reaches `return 42`.
 *
 * PROVEN cells pin the precision the same session bought (a for-in over an array,
 * a `const` loop bound): emit-only, they must compile in SILENCE.
 *
 * Honors ZER_MATRIX_ZERC so a cell can be shown to FIRE against a pre-fix build.
 * -Wswitch-enforced scenario enum: a new route must be classified here.
 */
#define _POSIX_C_SOURCE 200809L   /* mkdtemp */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int total = 0, passed = 0, failed = 0;
static int silent_hole = 0, unexpected = 0, over_reject = 0;
static const char *zerc_path = NULL;
/* A per-process work directory, NOT a fixed /tmp/_zer_* path: several matrix
 * binaries sharing one fixed path is the documented phantom-failure trap. */
static char wd[64];
static char p_src[96], p_exe[96], p_c[96], p_err[96];

static void make_workdir(void) {
    snprintf(wd, sizeof(wd), "/tmp/_zer_vf_XXXXXX");
    if (!mkdtemp(wd)) { fprintf(stderr, "cannot create work dir\n"); exit(2); }
    snprintf(p_src, sizeof(p_src), "%s/p.zer", wd);
    snprintf(p_exe, sizeof(p_exe), "%s/p", wd);
    snprintf(p_c, sizeof(p_c), "%s/p.c", wd);
    snprintf(p_err, sizeof(p_err), "%s/p.err", wd);
}

static void find_zerc(void) {
    const char *env = getenv("ZER_MATRIX_ZERC");
    if (env && *env) { zerc_path = env; return; }
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    if (system("test -x /tmp/zerc") == 0) { zerc_path = "/tmp/zerc"; return; }
    fprintf(stderr, "ERROR: cannot find zerc (set ZER_MATRIX_ZERC)\n");
    exit(2);
}

static void write_src(const char *code) {
    FILE *f = fopen(p_src, "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); exit(2); }
    fputs(code, f); fclose(f);
}

static void read_err(char *eb, size_t n) {
    eb[0] = 0;
    FILE *e = fopen(p_err, "r");
    if (e) { size_t r = fread(eb, 1, n - 1, e); eb[r] = 0; fclose(e); }
}

static int is_bounds_verdict_error(const char *eb) {
    return strstr(eb, "error:") &&
           (strstr(eb, "out of bounds") || strstr(eb, "runs 0..") ||
            strstr(eb, "loop counter"));
}

static int has_bounds_diag(const char *eb) {
    return strstr(eb, "not proven in range") || strstr(eb, "out of bounds") ||
           strstr(eb, "runs PAST the end") || strstr(eb, "auto-guard") ||
           strstr(eb, "bounds check") || strstr(eb, "for-in loop");
}

/* CATCH: the OOB access must be stopped — at compile time or at run time. */
static int run_catch(const char *name, const char *code) {
    total++;
    char eb[8192];
    write_src(code);
    char cmd[512];
    /* A non-.c `-o` builds the exe NEXT TO THE SOURCE (<wd>/p) — which is
     * exactly the path passed, so the two agree. */
    snprintf(cmd, sizeof(cmd), "%s %s -o %s >/dev/null 2>%s", zerc_path, p_src, p_exe, p_err);
    int crc = system(cmd);
    read_err(eb, sizeof(eb));
    if (crc != 0) {
        if (is_bounds_verdict_error(eb)) { passed++; return 1; }
        failed++; unexpected++;
        fprintf(stderr, "  FAIL [UNEXPECTED-REJECT] %s — rejected, but not for bounds:\n", name);
        fprintf(stderr, "    %.200s\n", eb);
        return 0;
    }
    snprintf(cmd, sizeof(cmd), "%s >/dev/null 2>&1", p_exe);
    int rc = system(cmd);
    int code_ = (rc == -1) ? -1 : ((rc >> 8) & 0xff);
    if (code_ == 0 || code_ == 133) { passed++; return 1; }
    failed++;
    if (code_ == 42) {
        silent_hole++;
        fprintf(stderr, "  FAIL [SILENT-HOLE] %s — the out-of-bounds access RAN unchecked\n", name);
        fprintf(stderr, "        (exit 42 = reached the statement after it).\n");
    } else {
        unexpected++;
        fprintf(stderr, "  FAIL [UNEXPECTED] %s — exit %d (neither guard, trap, nor the\n", name, code_);
        fprintf(stderr, "        hole's 42 — an unchecked write may have corrupted state)\n");
    }
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

/* PROVEN: the index is really in range; the compiler must say NOTHING. */
static int run_proven(const char *name, const char *code) {
    total++;
    char eb[8192];
    write_src(code);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s -o %s >/dev/null 2>%s", zerc_path, p_src, p_c, p_err);
    int crc = system(cmd);
    read_err(eb, sizeof(eb));
    if (crc != 0 || has_bounds_diag(eb)) {
        failed++; over_reject++;
        fprintf(stderr, "  FAIL [OVER-REJECT] %s — lost its elision:\n    %.200s\n", name, eb);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    passed++; return 1;
}

typedef enum {
    /* ---- IDENTITY: the range belonged to another variable of the same name ---- */
    VF_SHADOW_BLOCK,       /* inner decl in a block read the outer range */
    VF_SHADOW_LOOP_BODY,   /* inner decl in a loop body */
    VF_SHADOW_INTERSECT,   /* inner decl's range intersected with the outer one */
    VF_SHADOW_IF_CAPTURE,  /* `if (m) |i|` */
    VF_SHADOW_UNION_CAP,   /* `.a => |a|` union switch capture */
    VF_SHADOW_MUTATE,      /* inner assignment mutated the OUTER entry in place */
    /* ---- CONSTANT: the constant was folded in the wrong arithmetic ---- */
    VF_CONST_IF_BOUND,     /* `if (i < (0 - 1) / 1073741824 + 1)` */
    VF_CONST_MASK,         /* `x & ((0 - 1) / 1073741824 + 1)` */
    VF_CONST_MOD,          /* `x % ((0 - 1) / 1073741824 + 2)` */
    VF_CONST_WHILE_BOUND,  /* while bound */
    VF_CONST_ASSIGN_RENDER,/* plain `i = (0 - 1) % 7;` once rendered as int arithmetic (-1, OOB)
                            * while VRP proved the typed 3. Since BUG-1063 the C is typed and the
                            * value IS 3 — so the cell asserts VALUE AGREEMENT: exit 0 when the
                            * runtime value is the one VRP proved, 42 when they disagree (the old
                            * rendering, whose unchecked store also lands in `pad`). */
    VF_CONST_COMPTIME,     /* comptime F() folded untyped */
    /* ---- INVALIDATE: something changed the value after the range was recorded ---- */
    VF_PTR_STORE_PARAM,    /* `*p = 4` with p == &g */
    VF_PTR_STORE_GLOBAL,   /* pointer obtained from a global */
    VF_LOOP_CALL_WHILE,    /* a call in a while body writes the global */
    VF_LOOP_CALL_FOR,      /* same, for body */
    VF_YIELD,              /* another poller writes the global across a yield */
    VF_AWAIT,              /* same across an await */
    VF_ADDR_BLOCK,         /* `&i` taken in a block that has ended */
    VF_ADDR_COND,          /* `&i` taken under a condition */
    VF_ADDR_LOOP_FIELD,    /* `h.p = &i` in a loop */
    /* ---- HOIST: the guard was tested before the statement changed the index ---- */
    VF_HOIST_CALL_GLOBAL,  /* `g() + arr[gi]` with g writing gi */
    VF_HOIST_ADDR_ARG,     /* `g(&i) + arr[i]` */
    VF_HOIST_SHORTCIRCUIT, /* `g() > 0 && arr[gi] == 0` */
    VF_HOIST_ASSIGN_COND,  /* `(i = get(4)) > 0 && arr[i] == 0` */
    VF_HOIST_STRUCT_RET,   /* the emitter's C guard in a struct-returning function */
    VF_HOIST_ORELSE_INDEX, /* `arr[big() orelse 0]` — the index lowered to a temp */
    /* ---- SUMMARY: a return range read at the end of the body ---- */
    VF_SUMMARY_GOTO,       /* a later guard's inverse credited to an earlier return */
    VF_SUMMARY_SHADOW,     /* `return x` naming an inner shadow */

    /* ---- PROVEN: precision the same fixes bought ---- */
    VF_OK_RANGE_FOR,       /* for-in over a fixed array: bound is the array size */
    VF_OK_CONST_BOUND,     /* `for (i < N)` with `const u32 N` */
    VF_OK_TYPED_CONST,     /* `u32 i = (0 - 1) / 1073741824;` is 3 — in range of u32[4] */
    VFSCEN_COUNT
} VFScenario;

static int scenario_is_catch(VFScenario s) {
    switch (s) {
    case VF_SHADOW_BLOCK: case VF_SHADOW_LOOP_BODY: case VF_SHADOW_INTERSECT:
    case VF_SHADOW_IF_CAPTURE: case VF_SHADOW_UNION_CAP: case VF_SHADOW_MUTATE:
    case VF_CONST_IF_BOUND: case VF_CONST_MASK: case VF_CONST_MOD:
    case VF_CONST_WHILE_BOUND: case VF_CONST_ASSIGN_RENDER: case VF_CONST_COMPTIME:
    case VF_PTR_STORE_PARAM: case VF_PTR_STORE_GLOBAL: case VF_LOOP_CALL_WHILE:
    case VF_LOOP_CALL_FOR: case VF_YIELD: case VF_AWAIT: case VF_ADDR_BLOCK:
    case VF_ADDR_COND: case VF_ADDR_LOOP_FIELD:
    case VF_HOIST_CALL_GLOBAL: case VF_HOIST_ADDR_ARG: case VF_HOIST_SHORTCIRCUIT:
    case VF_HOIST_ASSIGN_COND: case VF_HOIST_STRUCT_RET: case VF_HOIST_ORELSE_INDEX:
    case VF_SUMMARY_GOTO: case VF_SUMMARY_SHADOW:
        return 1;
    case VF_OK_RANGE_FOR: case VF_OK_CONST_BOUND: case VF_OK_TYPED_CONST:
        return 0;
    case VFSCEN_COUNT: break;
    }
    return 1;
}

static const char *scen_name(VFScenario s) {
    switch (s) {
    case VF_SHADOW_BLOCK:        return "identity/shadow-in-block";
    case VF_SHADOW_LOOP_BODY:    return "identity/shadow-in-loop-body";
    case VF_SHADOW_INTERSECT:    return "identity/shadow-intersect";
    case VF_SHADOW_IF_CAPTURE:   return "identity/if-capture";
    case VF_SHADOW_UNION_CAP:    return "identity/union-capture";
    case VF_SHADOW_MUTATE:       return "identity/inner-assign-mutates-outer";
    case VF_CONST_IF_BOUND:      return "constant/if-bound";
    case VF_CONST_MASK:          return "constant/mask";
    case VF_CONST_MOD:           return "constant/modulus";
    case VF_CONST_WHILE_BOUND:   return "constant/while-bound";
    case VF_CONST_ASSIGN_RENDER: return "constant/plain-assign-rendering";
    case VF_CONST_COMPTIME:      return "constant/comptime-fold";
    case VF_PTR_STORE_PARAM:     return "invalidate/store-through-param";
    case VF_PTR_STORE_GLOBAL:    return "invalidate/store-through-global-ptr";
    case VF_LOOP_CALL_WHILE:     return "invalidate/call-in-while-body";
    case VF_LOOP_CALL_FOR:       return "invalidate/call-in-for-body";
    case VF_YIELD:               return "invalidate/yield";
    case VF_AWAIT:               return "invalidate/await";
    case VF_ADDR_BLOCK:          return "invalidate/&i-in-ended-block";
    case VF_ADDR_COND:           return "invalidate/&i-under-condition";
    case VF_ADDR_LOOP_FIELD:     return "invalidate/&i-into-field-in-loop";
    case VF_HOIST_CALL_GLOBAL:   return "hoist/call-writes-global-index";
    case VF_HOIST_ADDR_ARG:      return "hoist/&index-passed-in-statement";
    case VF_HOIST_SHORTCIRCUIT:  return "hoist/short-circuit-rhs";
    case VF_HOIST_ASSIGN_COND:   return "hoist/assign-in-condition";
    case VF_HOIST_STRUCT_RET:    return "hoist/struct-returning-function";
    case VF_HOIST_ORELSE_INDEX:  return "hoist/orelse-index-temp";
    case VF_SUMMARY_GOTO:        return "summary/goto-then-guard";
    case VF_SUMMARY_SHADOW:      return "summary/return-of-inner-shadow";
    case VF_OK_RANGE_FOR:        return "proven/for-in-over-array";
    case VF_OK_CONST_BOUND:      return "proven/const-loop-bound";
    case VF_OK_TYPED_CONST:      return "proven/typed-constant";
    case VFSCEN_COUNT:           break;
    }
    return "?";
}

/* Shared prologue: the OOB target and a pad it spills into, plus a helper the
 * checker cannot see through. */
#define PRE "u32[4] arr;\nu32[64] pad;\nu32 get(u32 a) { return a; }\n"

static void gen(VFScenario s, char *buf, size_t n) {
    switch (s) {
    case VF_SHADOW_BLOCK:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = 1;\n"
            "    if (true) { u32 i = get(5); arr[i] = 7; }\n"
            "    return 42;\n}\n");
        break;
    case VF_SHADOW_LOOP_BODY:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = get(1);\n"
            "    if (i < 4) {\n"
            "        u32 j = 0;\n"
            "        while (j < 1) { u32 i = get(5); arr[i] = 7; j += 1; }\n"
            "    }\n"
            "    return 42;\n}\n");
        break;
    case VF_SHADOW_INTERSECT:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = get(1);\n"
            "    if (i < 4) { if (true) { u32 i = get(5) % 100; arr[i] = 7; } }\n"
            "    return 42;\n}\n");
        break;
    case VF_SHADOW_IF_CAPTURE:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = 1;\n"
            "    ?u32 m = get(5);\n"
            "    if (m) |i| { arr[i] = 7; }\n"
            "    return 42;\n}\n");
        break;
    case VF_SHADOW_UNION_CAP:
        snprintf(buf, n, "%s", PRE
            "union U { u32 a; u32 b; }\n"
            "u32 main() {\n"
            "    u32 a = 1;\n"
            "    U u;\n"
            "    u.a = get(5);\n"
            "    switch (u) {\n"
            "        .a => |a| { arr[a] = 7; }\n"
            "        .b => |b| { }\n"
            "    }\n"
            "    return 42;\n}\n");
        break;
    case VF_SHADOW_MUTATE:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = get(9);\n"
            "    if (i < 100) {\n"
            "        { u32 i = get(0); i = get(0) % 4; }\n"
            "        arr[i] = 7;\n"
            "    }\n"
            "    return 42;\n}\n");
        break;
    case VF_CONST_IF_BOUND:
        snprintf(buf, n, "%s", PRE
            "u32[1] one;\n"
            "u32 main() {\n"
            "    u32 i = get(3);\n"
            "    if (i < (0 - 1) / 1073741824 + 1) { one[i] = 7; }\n"
            "    return 42;\n}\n");
        break;
    case VF_CONST_MASK:
        snprintf(buf, n, "%s", PRE
            "u32[2] two;\n"
            "u32 main() {\n"
            "    u32 x = get(4);\n"
            "    u32 i = x & ((0 - 1) / 1073741824 + 1);\n"
            "    two[i] = 7;\n"
            "    return 42;\n}\n");
        break;
    case VF_CONST_MOD:
        snprintf(buf, n, "%s", PRE
            "u32[2] two;\n"
            "u32 main() {\n"
            "    u32 x = get(7);\n"
            "    u32 i = x % ((0 - 1) / 1073741824 + 2);\n"
            "    two[i] = 7;\n"
            "    return 42;\n}\n");
        break;
    case VF_CONST_WHILE_BOUND:
        snprintf(buf, n, "%s", PRE
            "u32[1] one;\n"
            "u32 main() {\n"
            "    u32 i = get(3);\n"
            "    while (i < (0 - 1) / 1073741824 + 1) { one[i] = 1; i += 1; }\n"
            "    return 42;\n}\n");
        break;
    case VF_CONST_ASSIGN_RENDER:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = 0;\n"
            "    i = (0 - 1) % 7;\n"
            "    if (i > 100) { i = 5; }\n"
            "    arr[i] = 1;\n"
            "    if (i != 3) { return 42; }\n"
            "    return 0;\n}\n");
        break;
    case VF_CONST_COMPTIME:
        snprintf(buf, n, "%s", PRE
            "u32[2] two;\n"
            "comptime u32 F() { return (0 - 1) / 1073741824; }\n"
            "u32 main() {\n"
            "    u32 i = F();\n"
            "    two[i] = 1;\n"
            "    return 42;\n}\n");
        break;
    case VF_PTR_STORE_PARAM:
        snprintf(buf, n, "%s", PRE
            "u32 g = 1;\n"
            "u32 f(*u32 p) { if (g < 4) { *p = 4; arr[g] = 7; } return 42; }\n"
            "u32 main() { return f(&g); }\n");
        break;
    case VF_PTR_STORE_GLOBAL:
        snprintf(buf, n, "%s", PRE
            "u32 g = 1;\n"
            "?*u32 gp = null;\n"
            "void setup() { gp = &g; }\n"
            "u32 main() {\n"
            "    setup();\n"
            "    *u32 p = gp orelse return;\n"
            "    if (g < 4) { *p = 4; arr[g] = 7; }\n"
            "    return 42;\n}\n");
        break;
    case VF_LOOP_CALL_WHILE:
        snprintf(buf, n, "%s", PRE
            "u32 g = 1;\n"
            "void bump() { g = 4; }\n"
            "u32 main() {\n"
            "    u32 n = 0;\n"
            "    if (g < 4) { while (n < 2) { arr[g] = 7; bump(); n += 1; } }\n"
            "    return 42;\n}\n");
        break;
    case VF_LOOP_CALL_FOR:
        snprintf(buf, n, "%s", PRE
            "u32 g = 1;\n"
            "void bump() { g = 4; }\n"
            "u32 main() {\n"
            "    if (g < 4) { for (u32 n = 0; n < 2; n += 1) { arr[g] = 7; bump(); } }\n"
            "    return 42;\n}\n");
        break;
    case VF_YIELD:
        snprintf(buf, n, "%s", PRE
            "u32 g = 1;\n"
            "u32 hit = 0;\n"
            "async void worker() { if (g < 4) { yield; arr[g] = 7; hit = 1; } }\n"
            "u32 main() {\n"
            "    _zer_async_worker task;\n"
            "    _zer_async_worker_init(&task);\n"
            "    i32 d = _zer_async_worker_poll(&task);\n"
            "    g = 4;\n"
            "    d = _zer_async_worker_poll(&task);\n"
            "    if (hit == 1) { return 42; }\n"
            "    return 0;\n}\n");
        break;
    case VF_AWAIT:
        snprintf(buf, n, "%s", PRE
            "u32 g = 1;\n"
            "u32 hit = 0;\n"
            "bool go = false;\n"
            "async void worker() { if (g < 4) { await go; arr[g] = 7; hit = 1; } }\n"
            "u32 main() {\n"
            "    _zer_async_worker task;\n"
            "    _zer_async_worker_init(&task);\n"
            "    i32 d = _zer_async_worker_poll(&task);\n"
            "    g = 4;\n"
            "    go = true;\n"
            "    d = _zer_async_worker_poll(&task);\n"
            "    if (hit == 1) { return 42; }\n"
            "    return 0;\n}\n");
        break;
    case VF_ADDR_BLOCK:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = get(1);\n"
            "    ?*u32 q = null;\n"
            "    if (true) { q = &i; }\n"
            "    *u32 p = q orelse return;\n"
            "    if (i < 4) { *p = 4; arr[i] = 7; }\n"
            "    return 42;\n}\n");
        break;
    case VF_ADDR_COND:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = get(1);\n"
            "    ?*u32 q = null;\n"
            "    if (i < 4) { q = &i; }\n"
            "    *u32 p = q orelse return;\n"
            "    if (i < 4) { *p = 4; arr[i] = 7; }\n"
            "    return 42;\n}\n");
        break;
    case VF_ADDR_LOOP_FIELD:
        snprintf(buf, n, "%s", PRE
            "struct H { *u32 p; }\n"
            "u32 main() {\n"
            "    u32 i = get(1);\n"
            "    u32 dummy = 0;\n"
            "    H h = { .p = &dummy };\n"
            "    for (u32 n = 0; n < 1; n += 1) { h.p = &i; }\n"
            "    if (i < 4) { *h.p = 4; arr[i] = 7; }\n"
            "    return 42;\n}\n");
        break;
    case VF_HOIST_CALL_GLOBAL:
        snprintf(buf, n, "%s", PRE
            "u32 gi = 1;\n"
            "u32 g() { gi = 4; return 0; }\n"
            "u32 main() {\n"
            "    u32 v = g() + arr[gi];\n"
            "    return 42 + v;\n}\n");
        break;
    case VF_HOIST_ADDR_ARG:
        snprintf(buf, n, "%s", PRE
            "u32 g(*u32 p) { *p = 4; return 0; }\n"
            "u32 main() {\n"
            "    u32 i = get(1);\n"
            "    u32 v = g(&i) + arr[i];\n"
            "    return 42 + v;\n}\n");
        break;
    case VF_HOIST_SHORTCIRCUIT:
        snprintf(buf, n, "%s", PRE
            "u32 gi = 1;\n"
            "u32 g() { gi = 4; return 1; }\n"
            "u32 main() {\n"
            "    bool b = g() > 0 && arr[gi] == 0;\n"
            "    return 42;\n}\n");
        break;
    case VF_HOIST_ASSIGN_COND:
        snprintf(buf, n, "%s", PRE
            "u32 main() {\n"
            "    u32 i = get(1);\n"
            "    u32 v = 0;\n"
            "    if ((i = get(4)) > 0 && arr[i] == 0) { v = 1; }\n"
            "    return 42;\n}\n");
        break;
    case VF_HOIST_STRUCT_RET:
        snprintf(buf, n, "%s", PRE
            "struct R { u32 a; }\n"
            "u32 gi = 1;\n"
            "u32 g() { gi = 4; return 0; }\n"
            "R f() { R r; r.a = g() + arr[gi] + 42; return r; }\n"
            "u32 main() { R r = f(); return r.a; }\n");
        break;
    case VF_HOIST_ORELSE_INDEX:
        snprintf(buf, n, "%s", PRE
            "?u32 big() { return 5; }\n"
            "u32 main() {\n"
            "    arr[big() orelse 0] = 7;\n"
            "    return 42;\n}\n");
        break;
    case VF_SUMMARY_GOTO:
        snprintf(buf, n, "%s", PRE
            "u32[2] two;\n"
            "u32 f(u32 x) {\n"
            "    if (x > 50) { goto tail; }\n"
            "    return x;\n"
            "tail:\n"
            "    if (x >= 2) { return 1; }\n"
            "    return 0;\n"
            "}\n"
            "u32 main() { two[f(2)] = 7; return 42; }\n");
        break;
    case VF_SUMMARY_SHADOW:
        snprintf(buf, n, "%s", PRE
            "u32[2] two;\n"
            "u32 f(u32 x) {\n"
            "    if (x >= 2) { return 0; }\n"
            "    { u32 x = get(3); return x; }\n"
            "}\n"
            "u32 main() { two[f(1)] = 7; return 42; }\n");
        break;
    case VF_OK_RANGE_FOR:
        snprintf(buf, n, "%s",
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 s = 0;\n"
            "    for (u32 x in a) { s += x; }\n"
            "    return s;\n}\n");
        break;
    case VF_OK_CONST_BOUND:
        snprintf(buf, n, "%s",
            "const u32 N = 4;\n"
            "u32[4] a;\n"
            "u32 main() {\n"
            "    for (u32 i = 0; i < N; i += 1) { a[i] = i; }\n"
            "    return a[3];\n}\n");
        break;
    case VF_OK_TYPED_CONST:
        snprintf(buf, n, "%s",
            "u32 main() {\n"
            "    u32[4] a;\n"
            "    u32 i = (0 - 1) / 1073741824;\n"
            "    a[i] = 1;\n"
            "    return a[3];\n}\n");
        break;
    case VFSCEN_COUNT: buf[0] = 0; break;
    }
}

int main(void) {
    find_zerc();
    make_workdir();
    fprintf(stderr, "=== VRP fact-validity matrix ===\n");
    fprintf(stderr, "    CATCH : a run-time out-of-bounds access a broken compiler proves in range\n");
    fprintf(stderr, "            -> must be stopped (compile-time bounds error, guard, or trap)\n");
    fprintf(stderr, "    PROVEN: really in range -> must compile in silence\n\n");
    char buf[4096];
    int grid_ok = 1, cells = 0;
    for (VFScenario s = 0; s < VFSCEN_COUNT; s++) {
        cells++;
        int c = scenario_is_catch(s);
        gen(s, buf, sizeof(buf));
        int ok = c ? run_catch(scen_name(s), buf) : run_proven(scen_name(s), buf);
        fprintf(stderr, "  [%-6s][%-40s] %s\n", c ? "catch" : "proven", scen_name(s),
                ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }
    fprintf(stderr, "\n=== vrp-fact-matrix: %d/%d cells correct ===\n", passed, cells);
    fprintf(stderr, "    silent holes: %d | unexpected: %d | over-rejections: %d\n",
            silent_hole, unexpected, over_reject);
    {
        char rm[128];
        snprintf(rm, sizeof(rm), "rm -rf %s", wd);
        if (system(rm) != 0) { /* best effort */ }
    }
    if (!grid_ok) {
        fprintf(stderr, "VRP FACT MATRIX HAS HOLES — a recorded range was not a true fact about\n");
        fprintf(stderr, "the variable at the access (identity / constant / invalidation / hoist /\n");
        fprintf(stderr, "summary). A SILENT-HOLE cell is an unchecked out-of-bounds write.\n");
        return 1;
    }
    (void)total; (void)failed;
    return 0;
}

