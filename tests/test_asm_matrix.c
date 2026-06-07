/* test_asm_matrix.c — asm-safety soundness oracle (2026-06-08).
 *
 * Guards the DURABLE asm safety surface — the rules that survive the planned
 * Level C cleanup (docs/asm_lang_zer_safe.md): the S-rules (naked-only, max
 * instructions, no labels, mandatory safety string), empty-instruction reject,
 * and the active Z-rules at the asm operand boundary (Z8 const-output
 * qualifier preservation, Z11 non-keep-pointer-param + memory clobber). It does
 * NOT test the per-arch register/instruction VALIDATION tables (F4-F7), which
 * Level C deletes and delegates to GCC — no point guarding doomed code.
 *
 * This oracle doubles as the REGRESSION NET for the Level C ~7000-line deletion:
 * if that cleanup silently drops one of these durable checks, a NEG cell flips
 * to a false negative here.
 *
 * NEG: an asm-safety violation MUST be rejected for the relevant reason
 * (naked / safety / instruction / label / const / clobber). POS: a valid asm
 * construct MUST be accepted. EMIT-ONLY harness (`-o /tmp/x.c`, no gcc — naked
 * codegen / interrupt attrs aren't the point; the zercheck VERDICT is).
 * Integrity guard: a NEG rejection by parse error is flagged INVALID.
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

static int has_asm_reason(const char *eb) {
    return strstr(eb, "naked") || strstr(eb, "safety") || strstr(eb, "instruction") ||
           strstr(eb, "label") || strstr(eb, "const") || strstr(eb, "clobber") ||
           strstr(eb, "memory") || strstr(eb, "keep") || strstr(eb, "asm");
}

static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_am.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_am.zer -o /tmp/_zer_am.c 2>/tmp/_zer_am.err", zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — asm-unsafe construct ACCEPTED\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_am.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not an asm check\n", name);
        fprintf(stderr, "    %.110s\n", eb);
        return 0;
    }
    if (has_asm_reason(eb)) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for an asm-safety reason:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    return 0;
}

static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_am.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_am.zer -o /tmp/_zer_am.c 2>/tmp/_zer_am.err", zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_am.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — valid asm construct REJECTED:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

typedef enum {
    /* NEGATIVE — asm-safety violations (must reject) */
    AM_OUTSIDE_NAKED,      /* S1: asm in a non-naked function */
    AM_SHORT_SAFETY,       /* S4: safety: string < 30 chars */
    AM_TOO_MANY_INSN,      /* S2: > 16 instructions */
    AM_LABEL,              /* S3: a label inside the asm block */
    AM_EMPTY_INSN,         /* empty instructions: string */
    AM_CONST_OUTPUT,       /* Z8: asm output bound to a const variable */
    AM_NONKEEP_PTR_CLOBBER,/* Z11: non-keep pointer param input + memory clobber */
    /* POSITIVE — valid asm (must compile) */
    AM_IN_NAKED_OK,        /* asm in naked with a valid safety string */
    AM_OUTPUT_GLOBAL_OK,   /* output bound to a non-const global */
    AM_KEEP_PTR_CLOBBER_OK,/* keep pointer param input + memory clobber (Z11 valve) */
    AM_INPUT_VALUE_OK,     /* input binding from a value */
    AMSCEN_COUNT
} AMScenario;

static int scenario_is_negative(AMScenario s) {
    switch (s) {
        case AM_OUTSIDE_NAKED: case AM_SHORT_SAFETY: case AM_TOO_MANY_INSN:
        case AM_LABEL: case AM_EMPTY_INSN: case AM_CONST_OUTPUT:
        case AM_NONKEEP_PTR_CLOBBER:
            return 1;
        case AM_IN_NAKED_OK: case AM_OUTPUT_GLOBAL_OK:
        case AM_KEEP_PTR_CLOBBER_OK: case AM_INPUT_VALUE_OK:
            return 0;
        case AMSCEN_COUNT: break;
    }
    return 1;
}

static const char *scen_name(AMScenario s) {
    switch (s) {
        case AM_OUTSIDE_NAKED:       return "asm-outside-naked";
        case AM_SHORT_SAFETY:        return "safety-too-short";
        case AM_TOO_MANY_INSN:       return "too-many-instructions";
        case AM_LABEL:               return "label-in-asm";
        case AM_EMPTY_INSN:          return "empty-instructions";
        case AM_CONST_OUTPUT:        return "const-output-Z8";
        case AM_NONKEEP_PTR_CLOBBER: return "nonkeep-ptr+mem-clobber-Z11";
        case AM_IN_NAKED_OK:         return "asm-in-naked-ok";
        case AM_OUTPUT_GLOBAL_OK:    return "output-to-global-ok";
        case AM_KEEP_PTR_CLOBBER_OK: return "keep-ptr+mem-clobber-ok";
        case AM_INPUT_VALUE_OK:      return "input-binding-ok";
        case AMSCEN_COUNT: break;
    }
    return "?";
}

/* A valid >=30-char safety string reused where the cell isn't testing safety. */
#define SAFE "Documented asm operation for the asm-safety oracle test path"

static void gen(AMScenario s, char *buf, size_t n) {
    switch (s) {
        case AM_OUTSIDE_NAKED:
            snprintf(buf, n,
                "void f() {\n    asm {\n        instructions: \"nop\"\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_SHORT_SAFETY:
            snprintf(buf, n,
                "naked void f() {\n    asm {\n        instructions: \"nop\"\n"
                "        safety: \"too short\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_TOO_MANY_INSN:
            /* 17 instructions, ';'-separated. S2 counts actual-newline (0x0A) and
             * ';' in the instruction string. NOTE: '\n' ESCAPE sequences are NOT
             * counted (ZER's lexer keeps them literal; GCC later expands them) —
             * that S2 '\n'-bypass is a documented audit-rule limitation, not a
             * safety hole, so this cell uses ';' to test S2 as designed. */
            snprintf(buf, n,
                "naked void f() {\n    asm {\n"
                "        instructions: \"nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop\"\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_LABEL:
            snprintf(buf, n,
                "naked void f() {\n    asm {\n        instructions: \"spin: nop\"\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_EMPTY_INSN:
            snprintf(buf, n,
                "naked void f() {\n    asm {\n        instructions: \"\"\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_CONST_OUTPUT:
            snprintf(buf, n,
                "const u32 g_ro = 5;\n"
                "naked void f() {\n    asm {\n        instructions: \"movl $1, %%0\"\n"
                "        outputs: { \"rax\" = g_ro }\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_NONKEEP_PTR_CLOBBER:
            snprintf(buf, n,
                "naked void f(*u32 p) {\n    asm {\n        instructions: \"movl $0, (%%0)\"\n"
                "        inputs: { \"rdi\" = p }\n        clobbers: [\"memory\"]\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_IN_NAKED_OK:
            snprintf(buf, n,
                "naked void f() {\n    asm {\n        instructions: \"nop\"\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_OUTPUT_GLOBAL_OK:
            snprintf(buf, n,
                "u32 g_result;\n"
                "naked void f() {\n    asm {\n        instructions: \"movl $42, %%0\"\n"
                "        outputs: { \"rax\" = g_result }\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_KEEP_PTR_CLOBBER_OK:
            snprintf(buf, n,
                "naked void f(keep *u32 p) {\n    asm {\n        instructions: \"movl $0, (%%0)\"\n"
                "        inputs: { \"rdi\" = p }\n        clobbers: [\"memory\"]\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AM_INPUT_VALUE_OK:
            snprintf(buf, n,
                "naked void f(u32 v) {\n    asm {\n        instructions: \"movl %%0, %%%%eax\"\n"
                "        inputs: { \"rdi\" = v }\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n");
            break;
        case AMSCEN_COUNT: buf[0] = 0; break;
    }
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== asm-safety matrix (durable surface — survives Level C) ===\n");
    fprintf(stderr, "    NEG: asm-safety violation -> must REJECT for an asm reason\n");
    fprintf(stderr, "    POS: valid asm construct -> must COMPILE (emit-only)\n");
    fprintf(stderr, "    (per-arch register/instruction validation F4-F7 NOT tested — Level C deletes it)\n\n");

    char buf[2048];
    int grid_ok = 1, valid_cells = 0;
    for (AMScenario s = 0; s < AMSCEN_COUNT; s++) {
        valid_cells++;
        int neg = scenario_is_negative(s);
        char nm[128];
        snprintf(nm, sizeof(nm), "%s/%s", neg ? "neg" : "pos", scen_name(s));
        gen(s, buf, sizeof(buf));
        int ok = neg ? run_neg(nm, buf) : run_pos(nm, buf);
        fprintf(stderr, "  [%-3s][%-28s] %s\n",
                neg ? "neg" : "pos", scen_name(s), ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }

    fprintf(stderr, "\n=== asm-matrix: %d/%d cells correct ===\n", passed, valid_cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | over-rejections: %d\n",
            false_neg, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "ASM MATRIX HAS HOLES — an asm-safety violation the analyzer mishandles\n");
        fprintf(stderr, "(false negative), or a valid asm construct over-rejected.\n");
        return 1;
    }
    return 0;
}
