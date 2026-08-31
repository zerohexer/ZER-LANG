/* test_constrained_matrix.c — the CONSTRAINED-VALUE oracle (2026-08-31).
 *
 * WHAT A "CONSTRAINED TYPE" IS
 * ----------------------------
 * A type whose set of legal values is a STRICT SUBSET of the integer that
 * represents it. ZER has exactly two: `enum` (its declared variants) and `bool`
 * (0 and 1). Every other scalar can hold every bit pattern of its width, so for
 * them "forging" is not a concept.
 *
 * WHY IT NEEDS ITS OWN GRID
 * -------------------------
 * The consequence of a forged constrained value is SILENT and it is a
 * MIS-DISPATCH, not a fault:
 *
 *   - An exhaustive enum switch emits its LAST arm as an unconditional else.
 *     That elision is deliberate and load-bearing — ir_lower.c keeps it so the
 *     CFG merge at bb_exit does not gain a spurious predecessor, which would
 *     produce MAYBE_FREED false positives when arms free a handle. So the whole
 *     cost of a non-variant lands as "silently ran the last arm".
 *   - A forged bool makes BOTH `b == true` and `b == false` false, a state no
 *     ZER program can otherwise reach.
 *
 * Neither faults on hosted x86 and neither faults on bare metal. They are
 * exactly the class CLAUDE.md calls a silent gap: compile-time and runtime both
 * miss it.
 *
 * THE CLASS IS MULTI-SITE, WHICH IS WHY THE GRID EXISTS
 * ----------------------------------------------------
 * "May this value become a constrained one?" is answered at THREE separate
 * kinds of place, and the history is that each was fixed alone:
 *
 *   FLOW   — the 9 value-flow sinks behind `value_flows_to`. All nine accepted
 *            `Color c = 99` because ONE predicate (`is_literal_compatible`)
 *            treated an enum as an ordinary integer (BUG-913).
 *   OP     — arithmetic / bitwise / unary / compound. `c + c`, `c += 99`, `~c`
 *            all produced enum-TYPED values outside the variant set (BUG-914).
 *   DOOR   — the value-producing conversions @bitcast / @truncate / @saturate,
 *            which TRACK rather than ban: they emit a runtime variant guard at
 *            the point of forgery (BUG-843 / 864 / 891 / 910). `bool` was never
 *            wired into that guard (BUG-917).
 *   MINT   — @inttoptr / @ptrcast / @pun, which produce an ADDRESS. There is no
 *            value yet to guard and the later load is a bare read, so a pointer
 *            to a constrained type is rejected outright (BUG-918).
 *
 * Each of those was patched on its own before the axis was written down — the
 * shape CLAUDE.md names as the #1 recurring bug class. This grid is the gate
 * that makes a NEW site or a NEW constrained type fail the build instead of
 * shipping.
 *
 * VERDICT KINDS
 * -------------
 *   NEG   compile must FAIL, and the reason must belong to this class (a parse
 *         or unrelated type error is reported INVALID-PROBE, not a pass — a
 *         negative that passes for the wrong reason is a silent-green generator)
 *   TRAP  compile + gcc must SUCCEED and the program must die by SIGTRAP — this
 *         is the TRACKED half, where ZER deliberately allows the conversion and
 *         checks it at runtime
 *   POS   compile + gcc + run must succeed with exit 0 — the over-rejection
 *         boundary, so the tightenings cannot be widened into rejecting valid
 *         programs
 *
 * -Wswitch-enforced scenario enum: adding a constrained type or a new site
 * forces every function here to be updated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>

static int total = 0, passed = 0, failed = 0;
static int false_neg = 0, invalid_probe = 0, over_reject = 0, no_trap = 0;
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

/* Does the diagnostic belong to the constrained-value class? Asserting the
 * REASON is what stops a cell passing because some unrelated rule happened to
 * reject the same file. */
static int has_constrained_reason(const char *eb) {
    return strstr(eb, "is not a variant of enum") ||
           strstr(eb, "arithmetic on enum") ||
           strstr(eb, "bitwise operation on enum") ||
           strstr(eb, "compound assignment on enum") ||
           strstr(eb, "on enum '") ||
           strstr(eb, "cannot mint a pointer to") ||
           strstr(eb, "cannot target an array type");
}

static void write_src(const char *code) {
    FILE *f = fopen("/tmp/_zer_cn.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); exit(2); }
    fputs(code, f);
    fclose(f);
}

static void read_err(char *eb, size_t n) {
    eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_cn.err", "r");
    if (e) { size_t r = fread(eb, 1, n - 1, e); eb[r] = 0; fclose(e); }
}

/* NEG — must be rejected, for a reason in this class. */
static void run_neg(const char *name, const char *code) {
    total++;
    write_src(code);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_cn.zer -o /tmp/_zer_cn.c >/dev/null 2>/tmp/_zer_cn.err",
             zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — a forged constrained value was ACCEPTED\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return;
    }
    char eb[4096];
    read_err(eb, sizeof(eb));
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse failed")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by a PARSE error, so the cell tests nothing\n", name);
        fprintf(stderr, "    %.140s\n", eb);
        return;
    }
    if (has_constrained_reason(eb)) { passed++; return; }
    failed++;
    fprintf(stderr, "  FAIL [WRONG-REASON] %s — rejected, but not by the constrained-value rules:\n", name);
    fprintf(stderr, "    %.140s\n", eb);
}

/* TRAP — compiles and runs, and must die by SIGTRAP at the forgery point. */
static void run_trap(const char *name, const char *code) {
    total++;
    write_src(code);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_cn.zer -o /tmp/_zer_cn.c >/dev/null 2>/tmp/_zer_cn.err",
             zerc_path);
    if (system(cmd) != 0) {
        char eb[4096]; read_err(eb, sizeof(eb));
        failed++; over_reject++;
        fprintf(stderr, "  FAIL [OVER-REJECT] %s — a TRACKED conversion was rejected at compile time:\n", name);
        fprintf(stderr, "    %.140s\n", eb);
        return;
    }
    if (system("gcc -std=c99 -O2 -fwrapv -o /tmp/_zer_cn.bin /tmp/_zer_cn.c "
               ">/dev/null 2>&1") != 0) {
        failed++;
        fprintf(stderr, "  FAIL [GCC] %s — emitted C did not compile\n", name);
        return;
    }
    /* `system()` goes through /bin/sh, which reports a child killed by a signal
     * as exit status 128+signo rather than propagating WIFSIGNALED — so a real
     * SIGTRAP arrives here as WEXITSTATUS()==133. Accept both spellings: the
     * first draft checked only WIFSIGNALED and reported seven CLEAN cells as
     * missing guards, which is the harness lying about the compiler rather than
     * the other way round. */
    int st = system("/tmp/_zer_cn.bin >/dev/null 2>&1");
    int trapped = (st != -1) &&
                  ((WIFSIGNALED(st) && WTERMSIG(st) == SIGTRAP) ||
                   (WIFEXITED(st) && WEXITSTATUS(st) == 128 + SIGTRAP));
    if (trapped) { passed++; return; }
    failed++; no_trap++;
    fprintf(stderr, "  FAIL [NO-TRAP] %s — forged value ran to completion (status %d); "
                    "the runtime variant guard was not emitted\n", name, st);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
}

/* POS — the over-rejection boundary: compiles, links and returns 0. */
static void run_pos(const char *name, const char *code) {
    total++;
    write_src(code);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_cn.zer -o /tmp/_zer_cn.c >/dev/null 2>/tmp/_zer_cn.err",
             zerc_path);
    if (system(cmd) != 0) {
        char eb[4096]; read_err(eb, sizeof(eb));
        failed++; over_reject++;
        fprintf(stderr, "  FAIL [OVER-REJECT] %s — a VALID program was rejected:\n", name);
        fprintf(stderr, "    %.140s\n", eb);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return;
    }
    if (system("gcc -std=c99 -O2 -fwrapv -o /tmp/_zer_cn.bin /tmp/_zer_cn.c "
               ">/dev/null 2>&1") != 0) {
        failed++;
        fprintf(stderr, "  FAIL [GCC] %s — emitted C did not compile\n", name);
        return;
    }
    int st = system("/tmp/_zer_cn.bin >/dev/null 2>&1");
    if (st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0) { passed++; return; }
    failed++;
    fprintf(stderr, "  FAIL [WRONG-RESULT] %s — valid program did not exit 0 (status %d)\n",
            name, st);
}

/* ---- axes ---------------------------------------------------------------- */

typedef enum {
    /* FLOW — an integer literal reaching each of the nine value-flow sinks */
    CN_LIT_VARDECL, CN_LIT_ASSIGN, CN_LIT_CALLARG, CN_LIT_RETURN,
    CN_LIT_GLOBALINIT, CN_LIT_STRUCTINIT, CN_LIT_ORELSE, CN_LIT_ARRAYELEM,
    CN_LIT_SPAWNARG, CN_LIT_NEGATIVE,
    /* OP — arithmetic producing an enum-typed value */
    CN_OP_BINARY, CN_OP_BITWISE, CN_OP_COMPOUND, CN_OP_UNARY_NOT, CN_OP_UNARY_MINUS,
    /* MINT — a pointer to a constrained type */
    CN_MINT_ENUM_INTTOPTR, CN_MINT_ENUM_PTRCAST, CN_MINT_ENUM_PUN,
    CN_MINT_BOOL_INTTOPTR, CN_MINT_BOOL_PTRCAST, CN_MINT_BOOL_PUN,
    CN_MINT_ENUM_ARRAY, CN_MINT_ENUM_OPTIONAL, CN_BITCAST_ARRAY_TARGET,
    /* DOOR — the TRACKED conversions: compile, then trap at runtime */
    CN_DOOR_ENUM_BITCAST, CN_DOOR_ENUM_TRUNCATE, CN_DOOR_ENUM_SATURATE,
    CN_DOOR_ENUM_STRUCT, CN_DOOR_ENUM_ARRAY, CN_DOOR_ENUM_OPTIONAL,
    CN_DOOR_BOOL_BITCAST, CN_DOOR_BOOL_STRUCT, CN_DOOR_BOOL_ARRAY,
    /* POS — the over-rejection boundary */
    CN_OK_LIT_INRANGE, CN_OK_NEG_VARIANT, CN_OK_COMPARE, CN_OK_CAST,
    CN_OK_INDEX, CN_OK_SWITCH, CN_OK_BOOL_ORDINARY, CN_OK_MINT_AGGREGATE,
    CN_OK_ADDR_OF, CN_OK_BOOL_CAST_NORMALIZES, CN_OK_DOOR_IN_RANGE,
    CNSCEN_COUNT
} CNScenario;

typedef enum { V_NEG, V_TRAP, V_POS } Verdict;

static Verdict verdict_of(CNScenario s) {
    switch (s) {
    case CN_LIT_VARDECL: case CN_LIT_ASSIGN: case CN_LIT_CALLARG:
    case CN_LIT_RETURN: case CN_LIT_GLOBALINIT: case CN_LIT_STRUCTINIT:
    case CN_LIT_ORELSE: case CN_LIT_ARRAYELEM: case CN_LIT_SPAWNARG:
    case CN_LIT_NEGATIVE:
    case CN_OP_BINARY: case CN_OP_BITWISE: case CN_OP_COMPOUND:
    case CN_OP_UNARY_NOT: case CN_OP_UNARY_MINUS:
    case CN_MINT_ENUM_INTTOPTR: case CN_MINT_ENUM_PTRCAST: case CN_MINT_ENUM_PUN:
    case CN_MINT_BOOL_INTTOPTR: case CN_MINT_BOOL_PTRCAST: case CN_MINT_BOOL_PUN:
    case CN_MINT_ENUM_ARRAY: case CN_MINT_ENUM_OPTIONAL:
    case CN_BITCAST_ARRAY_TARGET:
        return V_NEG;
    case CN_DOOR_ENUM_BITCAST: case CN_DOOR_ENUM_TRUNCATE: case CN_DOOR_ENUM_SATURATE:
    case CN_DOOR_ENUM_STRUCT: case CN_DOOR_ENUM_ARRAY: case CN_DOOR_ENUM_OPTIONAL:
    case CN_DOOR_BOOL_BITCAST: case CN_DOOR_BOOL_STRUCT: case CN_DOOR_BOOL_ARRAY:
        return V_TRAP;
    case CN_OK_LIT_INRANGE: case CN_OK_NEG_VARIANT: case CN_OK_COMPARE:
    case CN_OK_CAST: case CN_OK_INDEX: case CN_OK_SWITCH:
    case CN_OK_BOOL_ORDINARY: case CN_OK_MINT_AGGREGATE: case CN_OK_ADDR_OF:
    case CN_OK_BOOL_CAST_NORMALIZES: case CN_OK_DOOR_IN_RANGE:
        return V_POS;
    case CNSCEN_COUNT: break;
    }
    return V_NEG;
}

static const char *scen_name(CNScenario s) {
    switch (s) {
    case CN_LIT_VARDECL:            return "flow/literal-var-decl";
    case CN_LIT_ASSIGN:             return "flow/literal-assign";
    case CN_LIT_CALLARG:            return "flow/literal-call-arg";
    case CN_LIT_RETURN:             return "flow/literal-return";
    case CN_LIT_GLOBALINIT:         return "flow/literal-global-init";
    case CN_LIT_STRUCTINIT:         return "flow/literal-struct-init";
    case CN_LIT_ORELSE:             return "flow/literal-orelse-fallback";
    case CN_LIT_ARRAYELEM:          return "flow/literal-array-element";
    case CN_LIT_SPAWNARG:           return "flow/literal-spawn-arg";
    case CN_LIT_NEGATIVE:           return "flow/negative-literal";
    case CN_OP_BINARY:              return "op/enum-plus-enum";
    case CN_OP_BITWISE:             return "op/enum-shift-enum";
    case CN_OP_COMPOUND:            return "op/enum-compound-assign";
    case CN_OP_UNARY_NOT:           return "op/enum-bitnot";
    case CN_OP_UNARY_MINUS:         return "op/enum-unary-minus";
    case CN_MINT_ENUM_INTTOPTR:     return "mint/enum-inttoptr";
    case CN_MINT_ENUM_PTRCAST:      return "mint/enum-ptrcast";
    case CN_MINT_ENUM_PUN:          return "mint/enum-pun";
    case CN_MINT_BOOL_INTTOPTR:     return "mint/bool-inttoptr";
    case CN_MINT_BOOL_PTRCAST:      return "mint/bool-ptrcast";
    case CN_MINT_BOOL_PUN:          return "mint/bool-pun";
    case CN_MINT_ENUM_ARRAY:        return "mint/enum-array-pointee";
    case CN_MINT_ENUM_OPTIONAL:     return "mint/enum-optional-pointee";
    case CN_BITCAST_ARRAY_TARGET:   return "door/bitcast-array-target-rejected";
    case CN_DOOR_ENUM_BITCAST:      return "door/enum-bitcast-traps";
    case CN_DOOR_ENUM_TRUNCATE:     return "door/enum-truncate-traps";
    case CN_DOOR_ENUM_SATURATE:     return "door/enum-saturate-traps";
    case CN_DOOR_ENUM_STRUCT:       return "door/enum-in-struct-traps";
    case CN_DOOR_ENUM_ARRAY:        return "door/enum-in-array-traps";
    case CN_DOOR_ENUM_OPTIONAL:     return "door/enum-in-optional-traps";
    case CN_DOOR_BOOL_BITCAST:      return "door/bool-bitcast-traps";
    case CN_DOOR_BOOL_STRUCT:       return "door/bool-in-struct-traps";
    case CN_DOOR_BOOL_ARRAY:        return "door/bool-in-array-traps";
    case CN_OK_LIT_INRANGE:         return "ok/literal-names-a-variant";
    case CN_OK_NEG_VARIANT:         return "ok/negative-declared-variant";
    case CN_OK_COMPARE:             return "ok/enum-comparisons";
    case CN_OK_CAST:                return "ok/enum-explicit-cast";
    case CN_OK_INDEX:               return "ok/enum-as-array-index";
    case CN_OK_SWITCH:              return "ok/exhaustive-switch";
    case CN_OK_BOOL_ORDINARY:       return "ok/ordinary-bool";
    case CN_OK_MINT_AGGREGATE:      return "ok/mint-struct-carrying-enum";
    case CN_OK_ADDR_OF:             return "ok/address-of-enum-var";
    case CN_OK_BOOL_CAST_NORMALIZES:return "ok/bool-cast-normalizes";
    case CN_OK_DOOR_IN_RANGE:       return "ok/door-with-a-valid-value";
    case CNSCEN_COUNT: break;
    }
    return "?";
}

#define E "enum Color { red, green, blue }\n"
#define C "enum Code { ok = 10, warn = 20, bad = -1 }\n"

static void gen(CNScenario s, char *b, size_t n) {
    switch (s) {
    case CN_LIT_VARDECL:
        snprintf(b, n, E "u32 main() { Color c = 99; return (u32)c; }\n"); break;
    case CN_LIT_ASSIGN:
        snprintf(b, n, E "u32 main() { Color c = Color.red; c = 99; return (u32)c; }\n"); break;
    case CN_LIT_CALLARG:
        snprintf(b, n, E "u32 f(Color c) { return (u32)c; }\nu32 main() { return f(99); }\n"); break;
    case CN_LIT_RETURN:
        snprintf(b, n, E "Color f() { return 99; }\nu32 main() { return (u32)f(); }\n"); break;
    case CN_LIT_GLOBALINIT:
        snprintf(b, n, E "Color g = 99;\nu32 main() { return (u32)g; }\n"); break;
    case CN_LIT_STRUCTINIT:
        snprintf(b, n, E "struct S { Color c; }\nu32 main() { S s = { .c = 99 }; return (u32)s.c; }\n"); break;
    case CN_LIT_ORELSE:
        snprintf(b, n, E "?Color m() { return null; }\n"
                        "u32 main() { Color c = m() orelse 99; return (u32)c; }\n"); break;
    case CN_LIT_ARRAYELEM:
        snprintf(b, n, E "u32 main() { Color[2] a; a[0] = 99; return (u32)a[0]; }\n"); break;
    case CN_LIT_SPAWNARG:
        snprintf(b, n, E "void w(Color c) { }\nu32 main() { spawn w(99); return 0; }\n"); break;
    case CN_LIT_NEGATIVE:
        snprintf(b, n, C "u32 main() { Code c = -7; return (u32)@bitcast(u32, c); }\n"); break;

    case CN_OP_BINARY:
        snprintf(b, n, E "u32 main() { Color c = Color.blue; Color d = c + c; return (u32)d; }\n"); break;
    case CN_OP_BITWISE:
        snprintf(b, n, E "u32 main() { Color c = Color.blue; Color d = c << c; return (u32)d; }\n"); break;
    case CN_OP_COMPOUND:
        snprintf(b, n, E "u32 main() { Color c = Color.red; c += 2; return (u32)c; }\n"); break;
    case CN_OP_UNARY_NOT:
        snprintf(b, n, E "u32 main() { Color c = Color.green; Color d = ~c; return (u32)d; }\n"); break;
    case CN_OP_UNARY_MINUS:
        snprintf(b, n, C "u32 main() { Code c = Code.ok; Code d = -c; return (u32)@bitcast(u32, d); }\n"); break;

    case CN_MINT_ENUM_INTTOPTR:
        snprintf(b, n, E "mmio 0x0..0xFFFFFFFF;\n"
                        "u32 main() { volatile *Color r = @inttoptr(*Color, 0x1000); return (u32)(*r); }\n"); break;
    case CN_MINT_ENUM_PTRCAST:
        snprintf(b, n, E "u32 main() { u32 raw = 99; *u32 p = &raw; *Color q = @ptrcast(*Color, p); return (u32)(*q); }\n"); break;
    case CN_MINT_ENUM_PUN:
        snprintf(b, n, E "struct W { u32 n; }\n"
                        "u32 main() { W w; w.n = 99; *W wp = &w; *Color cp = @pun(*Color, wp); return (u32)(*cp); }\n"); break;
    case CN_MINT_BOOL_INTTOPTR:
        snprintf(b, n, "mmio 0x0..0xFFFFFFFF;\n"
                       "u32 main() { volatile *bool r = @inttoptr(*bool, 0x1000); if (*r) { return 1; } return 0; }\n"); break;
    case CN_MINT_BOOL_PTRCAST:
        snprintf(b, n, "u8 raw() { return 2; }\n"
                       "u32 main() { u8 r = raw(); *u8 q = &r; *bool p = @ptrcast(*bool, q); if (*p) { return 1; } return 0; }\n"); break;
    case CN_MINT_BOOL_PUN:
        snprintf(b, n, "struct W { u8 n; }\n"
                       "u32 main() { W w; w.n = 2; *W wp = &w; *bool bp = @pun(*bool, wp); if (*bp) { return 1; } return 0; }\n"); break;
    case CN_MINT_ENUM_ARRAY:
        snprintf(b, n, E "mmio 0x0..0xFFFFFFFF;\n"
                        "u32 main() { volatile *Color[4] r = @inttoptr(*Color[4], 0x1000); return 0; }\n"); break;
    case CN_MINT_ENUM_OPTIONAL:
        snprintf(b, n, E "u32 main() { u32 raw = 99; *u32 p = &raw; *?Color q = @ptrcast(*?Color, p); return 0; }\n"); break;

    case CN_BITCAST_ARRAY_TARGET:
        snprintf(b, n, "u64 raw() { return 99; }\n"
                       "u32 main() { u64 r = raw(); u32[2] a = @bitcast(u32[2], r); return a[0]; }\n"); break;

    case CN_DOOR_ENUM_BITCAST:
        snprintf(b, n, E "u32 raw() { return 99; }\n"
                        "u32 main() { u32 r = raw(); Color c = @bitcast(Color, r); return (u32)c; }\n"); break;
    case CN_DOOR_ENUM_TRUNCATE:
        snprintf(b, n, E "u64 raw() { return 99; }\n"
                        "u32 main() { u64 r = raw(); Color c = @truncate(Color, r); return (u32)c; }\n"); break;
    case CN_DOOR_ENUM_SATURATE:
        snprintf(b, n, E "u64 raw() { return 99; }\n"
                        "u32 main() { u64 r = raw(); Color c = @saturate(Color, r); return (u32)c; }\n"); break;
    case CN_DOOR_ENUM_STRUCT:
        snprintf(b, n, E "struct H { Color c; }\nu32 raw() { return 99; }\n"
                        "u32 main() { u32 r = raw(); H h = @bitcast(H, r); return (u32)h.c; }\n"); break;
    /* BUG-919: @bitcast cannot TARGET an array (C has no array rvalue), so the
     * array-recursion arm of the guard is exercised through a one-field struct
     * — which is also the form the diagnostic tells users to write. */
    case CN_DOOR_ENUM_ARRAY:
        snprintf(b, n, E "struct A { Color[2] v; }\nu64 raw() { return 99; }\n"
                        "u32 main() { u64 r = raw(); A a = @bitcast(A, r); return (u32)a.v[0]; }\n"); break;
    case CN_DOOR_ENUM_OPTIONAL:
        snprintf(b, n, E "u64 raw() { return 0x0000000100000063; }\n"
                        "u32 main() { u64 r = raw(); ?Color o = @bitcast(?Color, r); Color c = o orelse Color.red; return (u32)c; }\n"); break;
    case CN_DOOR_BOOL_BITCAST:
        snprintf(b, n, "u8 raw() { return 2; }\n"
                       "u32 main() { u8 r = raw(); bool b = @bitcast(bool, r);\n"
                       "  if (b == true) { return 1; } if (b == false) { return 2; } return 3; }\n"); break;
    case CN_DOOR_BOOL_STRUCT:
        snprintf(b, n, "struct F { u8 a; bool ok; u16 pad; }\nu32 raw() { return 519; }\n"
                       "u32 main() { u32 r = raw(); F f = @bitcast(F, r);\n"
                       "  if (f.ok == true) { return 1; } if (f.ok == false) { return 2; } return 3; }\n"); break;
    case CN_DOOR_BOOL_ARRAY:
        snprintf(b, n, "struct A { bool[2] v; }\nu16 raw() { return 0x0002; }\n"
                       "u32 main() { u16 r = raw(); A a = @bitcast(A, r);\n"
                       "  if (a.v[0] == true) { return 1; } if (a.v[0] == false) { return 2; } return 3; }\n"); break;

    case CN_OK_LIT_INRANGE:
        snprintf(b, n, E "struct S { Color c; }\nColor g = 2;\n"
                        "u32 take(Color c) { return (u32)c; }\nColor give() { return 1; }\n"
                        "u32 main() { Color a = 0; Color b = 1; S s = { .c = 2 };\n"
                        "  Color[2] arr; arr[0] = 1;\n"
                        "  if ((u32)a + (u32)b + (u32)g + (u32)s.c + (u32)arr[0]\n"
                        "      + take(2) + (u32)give() == 9) { return 0; } return 1; }\n"); break;
    case CN_OK_NEG_VARIANT:
        snprintf(b, n, C "u32 main() { Code a = -1; Code b = 20; Code c = 10;\n"
                        "  if (a == Code.bad && b == Code.warn && c == Code.ok) { return 0; } return 1; }\n"); break;
    case CN_OK_COMPARE:
        snprintf(b, n, E "u32 main() { Color a = Color.red; Color b = Color.blue;\n"
                        "  if (a == Color.red && a != b && a < b && b > a) { return 0; } return 1; }\n"); break;
    case CN_OK_CAST:
        snprintf(b, n, E "u32 main() { Color c = Color.blue; u32 n = (u32)c;\n"
                        "  if (n == 2) { return 0; } return 1; }\n"); break;
    case CN_OK_INDEX:
        snprintf(b, n, E "u32 main() { u32[3] t; t[0]=7; t[1]=8; t[2]=9;\n"
                        "  Color c = Color.green; if (t[c] == 8) { return 0; } return 1; }\n"); break;
    case CN_OK_SWITCH:
        snprintf(b, n, E "u32 f(Color c) { switch (c) { .red => { return 1; } .green => { return 2; } .blue => { return 3; } } }\n"
                        "u32 main() { if (f(Color.green) == 2) { return 0; } return 1; }\n"); break;
    case CN_OK_BOOL_ORDINARY:
        snprintf(b, n, "struct F { bool a; bool b; }\nbool flip(bool x) { return !x; }\n"
                       "u32 main() { bool t = true; bool f = false; F s; s.a = true; s.b = false;\n"
                       "  bool[2] arr; arr[0] = t; arr[1] = f; ?bool ob = t; bool u = ob orelse false;\n"
                       "  if (t && !f && s.a && !s.b && arr[0] && !arr[1] && u && flip(f)) {\n"
                       "    switch (t) { true => { return 0; } false => { return 1; } } }\n"
                       "  return 2; }\n"); break;
    case CN_OK_MINT_AGGREGATE:
        snprintf(b, n, "enum Mode { off, on }\nstruct Sensor { Mode m; u32 v; }\n"
                       "struct Regs { Mode ctl; u32 data; }\nmmio 0x40000000..0x4000FFFF;\n"
                       "u32 main() { Sensor s; s.m = Mode.on; s.v = 3;\n"
                       "  *opaque ctx = @ptrcast(*opaque, &s);\n"
                       "  *Sensor back = @ptrcast(*Sensor, ctx);\n"
                       "  volatile *Regs r = @inttoptr(*Regs, 0x40000000);\n"
                       "  if (back.v == 3) { return 0; } return 1; }\n"); break;
    case CN_OK_ADDR_OF:
        snprintf(b, n, "enum Mode { off, on }\n"
                       "u32 main() { Mode m = Mode.on; *Mode p = &m;\n"
                       "  if (*p == Mode.on) { return 0; } return 1; }\n"); break;
    case CN_OK_BOOL_CAST_NORMALIZES:
        snprintf(b, n, "u8 raw() { return 2; }\n"
                       "u32 main() { u8 r = raw(); bool b = (bool)r;\n"
                       "  if (b == true) { return 0; } return 1; }\n"); break;
    case CN_OK_DOOR_IN_RANGE:
        snprintf(b, n, E "u32 raw() { return 1; }\n"
                        "u32 main() { u32 r = raw(); Color c = @bitcast(Color, r);\n"
                        "  if (c == Color.green) { return 0; } return 1; }\n"); break;
    case CNSCEN_COUNT: break;
    }
}

int main(void) {
    find_zerc();
    printf("=== CONSTRAINED-VALUE MATRIX (enum / bool) ===\n");
    printf("zerc: %s\n", zerc_path);
    for (int i = 0; i < CNSCEN_COUNT; i++) {
        CNScenario s = (CNScenario)i;
        char code[2048];
        code[0] = 0;
        gen(s, code, sizeof(code));
        if (!code[0]) {
            fprintf(stderr, "  FAIL [NO-PROGRAM] %s — generator produced nothing\n", scen_name(s));
            total++; failed++;
            continue;
        }
        switch (verdict_of(s)) {
        case V_NEG:  run_neg(scen_name(s), code);  break;
        case V_TRAP: run_trap(scen_name(s), code); break;
        case V_POS:  run_pos(scen_name(s), code);  break;
        }
    }
    printf("\nconstrained matrix: %d cells, %d passed, %d failed\n", total, passed, failed);
    if (failed) {
        printf("  false-negatives (forged value ACCEPTED): %d\n", false_neg);
        printf("  invalid probes (rejected by a parse error): %d\n", invalid_probe);
        printf("  over-rejections (valid program refused): %d\n", over_reject);
        printf("  missing runtime guards (no trap): %d\n", no_trap);
        return 1;
    }
    printf("CONSTRAINED MATRIX CLEAN\n");
    return 0;
}
