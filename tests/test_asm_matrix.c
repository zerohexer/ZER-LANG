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
 *
 * BUG-1658 (2026-11-14) — two changes to the grid itself:
 *   1. Every NEG cell states the REASON it must be rejected for. The grid used to
 *      accept any stderr containing "naked", "asm", ... — and every naked function
 *      prints a WARNING containing both, so a cell rejected by an unrelated rule
 *      still read as correct.
 *   2. The TEXT grid: the structural rules S2 (<= 16 instructions) and S3 (no
 *      label) are questions about the text the ASSEMBLER sees, so they are crossed
 *      over asm FORM x SEPARATOR SPELLING x RULE. Before the fix the inline form
 *      had neither rule and only two separator spellings were seen at all.
 *   3. EMITTED-C cells: user text written into the generated C (the safety
 *      comment, the instruction / clobber / section strings, the inline
 *      literals) must not be able to end the construct it is written into. The
 *      driver builds with -std=c99, where the trigraph for a backslash is live, so
 *      each cell asserts the emitted text, not a verdict.
 *      Run the grid against a pre-fix compiler (ZER_MATRIX_ZERC=...): 30 cells
 *      go red.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zer_tmp.h"

static int total = 0, passed = 0, failed = 0;
static int false_neg = 0, invalid_probe = 0, over_reject = 0;
static const char *zerc_path = NULL;

static void find_zerc(void) {
    /* ZER_MATRIX_ZERC overrides the search: verifying that a new cell actually FIRES
     * means running this grid against a PRE-FIX compiler, and there was no way to
     * point it at one — every grid auto-detected ./zerc and IGNORED argv, so a run
     * "against the baseline" silently graded the current compiler (CLAUDE.md
     * recorded this for test_hw_matrix). All ten grids take the same override. */
    const char *env = getenv("ZER_MATRIX_ZERC");
    if (env && *env) { zerc_path = env; return; }
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

static int run_neg(const char *name, const char *code, const char *reason) {
    total++;
    FILE *f = fopen(ZT("/tmp/_zer_am.zer"), "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), ZT("%s /tmp/_zer_am.zer -o /tmp/_zer_am.c 2>/tmp/_zer_am.err"), zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — asm-unsafe construct ACCEPTED\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen(ZT("/tmp/_zer_am.err"), "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not an asm check\n", name);
        fprintf(stderr, "    %.110s\n", eb);
        return 0;
    }
    if (strstr(eb, reason)) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for its reason (\"%s\"):\n", name, reason);
    fprintf(stderr, "    %.300s\n", eb);
    return 0;
}

/* POS with an assertion on the EMITTED C: the program compiles and the output
 * does not contain `forbidden` (a silent emission defect has no verdict to read). */
static int run_pos_emit_lacks(const char *name, const char *code, const char *forbidden) {
    total++;
    FILE *f = fopen(ZT("/tmp/_zer_am.zer"), "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), ZT("%s /tmp/_zer_am.zer -o /tmp/_zer_am.c 2>/tmp/_zer_am.err"), zerc_path);
    if (system(cmd) != 0) {
        failed++; over_reject++;
        fprintf(stderr, "  FAIL [OVER-REJECT] %s — valid asm construct REJECTED\n", name);
        return 0;
    }
    FILE *o = fopen(ZT("/tmp/_zer_am.c"), "r");
    if (!o) { failed++; fprintf(stderr, "  FAIL %s — no emitted C\n", name); return 0; }
    /* the whole file at once: a forbidden text may span a line break */
    int hit = 0;
    fseek(o, 0, SEEK_END); long sz = ftell(o); fseek(o, 0, SEEK_SET);
    char *all = (sz > 0) ? (char *)malloc((size_t)sz + 1) : NULL;
    if (all) {
        size_t rd = fread(all, 1, (size_t)sz, o); all[rd] = 0;
        if (strstr(all, forbidden)) hit = 1;
        free(all);
    }
    fclose(o);
    if (!hit) { passed++; return 1; }
    failed++; false_neg++;
    fprintf(stderr, "  FAIL [SILENT-EMISSION] %s — emitted C contains \"%s\"\n", name, forbidden);
    return 0;
}

static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen(ZT("/tmp/_zer_am.zer"), "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), ZT("%s /tmp/_zer_am.zer -o /tmp/_zer_am.c 2>/tmp/_zer_am.err"), zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen(ZT("/tmp/_zer_am.err"), "r");
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

/* The reason each NEG cell must be rejected for — what the rule is SUPPOSED to
 * say, not a paste of a message. NULL for a positive. */
static const char *scen_reason(AMScenario s) {
    switch (s) {
        case AM_OUTSIDE_NAKED:       return "only allowed in naked functions";
        case AM_SHORT_SAFETY:        return "at least 30 characters";
        case AM_TOO_MANY_INSN:       return "max is 16";
        case AM_LABEL:               return "contains a label";
        case AM_EMPTY_INSN:          return "requires non-empty";
        case AM_CONST_OUTPUT:        return "writes to const variable";
        case AM_NONKEEP_PTR_CLOBBER: return "non-keep pointer parameter";
        case AM_IN_NAKED_OK: case AM_OUTPUT_GLOBAL_OK:
        case AM_KEEP_PTR_CLOBBER_OK: case AM_INPUT_VALUE_OK:
            return NULL;
        case AMSCEN_COUNT: break;
    }
    return NULL;
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
            /* 17 instructions, ';'-separated. Every other separator spelling is a
             * cell of the TEXT grid below. */
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

/* ---- the TEXT grid: FORM x SEPARATOR SPELLING x RULE (BUG-1658) ------------ */

typedef enum { TF_STRUCTURED, TF_INLINE, TF_COUNT } TextForm;
typedef enum {
    TS_SEMI,        /* nop;nop                                    */
    TS_REAL_NL,     /* an actual line break inside the literal    */
    TS_ESC_N,       /* the escape \n                              */
    TS_ESC_HEX,     /* the escape \x0a                            */
    TS_ADJACENT,    /* "nop\n" "nop" — adjacent literals          */
    TS_COUNT
} TextSep;
typedef enum {
    TR_SEVENTEEN,      /* NEG: 17 instructions                          */
    TR_SIXTEEN_OK,     /* POS: 16 instructions and a trailing separator */
    TR_LABEL_SECOND,   /* NEG: a label on the SECOND statement          */
    TR_COUNT
} TextRule;

static const char *tf_name(TextForm f) {
    switch (f) {
        case TF_STRUCTURED: return "structured";
        case TF_INLINE:     return "inline";
        case TF_COUNT: break;
    }
    return "?";
}
static const char *ts_name(TextSep s) {
    switch (s) {
        case TS_SEMI:     return "semicolon";
        case TS_REAL_NL:  return "real-newline";
        case TS_ESC_N:    return "escape-n";
        case TS_ESC_HEX:  return "escape-x0a";
        case TS_ADJACENT: return "adjacent-literals";
        case TS_COUNT: break;
    }
    return "?";
}
static const char *tr_name(TextRule r) {
    switch (r) {
        case TR_SEVENTEEN:    return "17-instructions";
        case TR_SIXTEEN_OK:   return "16-and-trailing-ok";
        case TR_LABEL_SECOND: return "label-on-2nd-statement";
        case TR_COUNT: break;
    }
    return "?";
}
/* The separator as it is written in ZER source. */
static const char *ts_text(TextSep s) {
    switch (s) {
        case TS_SEMI:     return ";";
        case TS_REAL_NL:  return "\n";
        case TS_ESC_N:    return "\\n";
        case TS_ESC_HEX:  return "\\x0a";
        case TS_ADJACENT: return "\\n\" \"";
        case TS_COUNT: break;
    }
    return ";";
}
/* A combination that is not a program of the form under test:
 *  - an actual line break inside an INLINE literal is refused by the inline
 *    form's own grammar (only literals and whitespace), a different rule;
 *  - adjacent literals exist only in the inline form (structured takes ONE). */
static int text_cell_valid(TextForm f, TextSep s) {
    if (f == TF_INLINE && s == TS_REAL_NL) return 0;
    if (f == TF_STRUCTURED && s == TS_ADJACENT) return 0;
    return 1;
}
static void text_wrap(TextForm f, const char *body, char *buf, size_t n) {
    switch (f) {
        case TF_STRUCTURED:
            snprintf(buf, n,
                "naked void f() {\n    asm {\n        instructions: \"%s\"\n"
                "        safety: \"" SAFE "\"\n    }\n}\n"
                "i32 main() { return 0; }\n", body);
            break;
        case TF_INLINE:
            snprintf(buf, n,
                "naked void f() {\n    asm(\"%s\");\n}\n"
                "i32 main() { return 0; }\n", body);
            break;
        case TF_COUNT: buf[0] = 0; break;
    }
}
static void text_gen(TextForm f, TextSep s, TextRule r, char *buf, size_t n) {
    char body[1024]; size_t bl = 0; body[0] = 0;
    const char *sep = ts_text(s);
    /* the trailing separator of an adjacent-literal cell must stay INSIDE a literal */
    const char *tail = (s == TS_ADJACENT) ? "\\n" : sep;
    switch (r) {
        case TR_SEVENTEEN:
            for (int i = 0; i < 17; i++)
                bl += (size_t)snprintf(body + bl, sizeof(body) - bl, "%snop", i ? sep : "");
            break;
        case TR_SIXTEEN_OK:
            for (int i = 0; i < 16; i++)
                bl += (size_t)snprintf(body + bl, sizeof(body) - bl, "%snop", i ? sep : "");
            snprintf(body + bl, sizeof(body) - bl, "%s", tail);
            break;
        case TR_LABEL_SECOND:
            /* a space after the separator: C's \x takes every hex digit that
             * follows, so `\x0aagain` would not be a line break at all */
            snprintf(body, sizeof(body), "nop%s again: jmp again", sep);
            break;
        case TR_COUNT: break;
    }
    text_wrap(f, body, buf, n);
}
static const char *text_reason(TextRule r) {
    switch (r) {
        case TR_SEVENTEEN:    return "max is 16";
        case TR_SIXTEEN_OK:   return NULL;
        case TR_LABEL_SECOND: return "contains a label";
        case TR_COUNT: break;
    }
    return NULL;
}

/* Label SPELLINGS (every legal assembler symbol form is a label) and the
 * statements that look like one and are not. */
static const char *const LABEL_SPELLINGS[] = { "again", ".L1", "$x", "7", "_a.b$c" };
static const char *const NOT_LABELS_STRUCTURED[] = {
    "lr.w t0, (a0)",                 /* a dotted mnemonic                */
    "movq %%fs:0x28, %%rax",         /* a colon that is not at a symbol  */
    ".ascii \\\"a:b\\\"",            /* a colon inside a quoted operand  */
};
static const char *const NOT_LABELS_INLINE[] = {
    "lr.w t0, (a0)",
    "movq %fs:0x28, %rax",
    ".ascii \\\"a:b\\\"",
};

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
        int ok = neg ? run_neg(nm, buf, scen_reason(s)) : run_pos(nm, buf);
        fprintf(stderr, "  [%-3s][%-28s] %s\n",
                neg ? "neg" : "pos", scen_name(s), ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }

    fprintf(stderr, "\n  -- text grid: form x separator spelling x rule --\n");
    for (TextForm f = 0; f < TF_COUNT; f++) {
        for (TextSep sp = 0; sp < TS_COUNT; sp++) {
            if (!text_cell_valid(f, sp)) continue;
            for (TextRule r = 0; r < TR_COUNT; r++) {
                valid_cells++;
                const char *reason = text_reason(r);
                char nm[160];
                snprintf(nm, sizeof(nm), "%s/%s/%s/%s", reason ? "neg" : "pos",
                         tf_name(f), ts_name(sp), tr_name(r));
                text_gen(f, sp, r, buf, sizeof(buf));
                int ok = reason ? run_neg(nm, buf, reason) : run_pos(nm, buf);
                fprintf(stderr, "  [%-3s][%-10s][%-17s][%-22s] %s\n", reason ? "neg" : "pos",
                        tf_name(f), ts_name(sp), tr_name(r), ok ? "ok" : "*** FAIL ***");
                if (!ok) grid_ok = 0;
            }
        }
    }
    fprintf(stderr, "\n  -- label spellings, and statements that are not labels --\n");
    for (TextForm f = 0; f < TF_COUNT; f++) {
        for (size_t li = 0; li < sizeof(LABEL_SPELLINGS) / sizeof(LABEL_SPELLINGS[0]); li++) {
            valid_cells++;
            char body[128], nm[160];
            snprintf(body, sizeof(body), "%s: nop", LABEL_SPELLINGS[li]);
            snprintf(nm, sizeof(nm), "neg/%s/label-spelling/%s", tf_name(f), LABEL_SPELLINGS[li]);
            text_wrap(f, body, buf, sizeof(buf));
            int ok = run_neg(nm, buf, "contains a label");
            fprintf(stderr, "  [neg][%-10s][label %-8s] %s\n", tf_name(f),
                    LABEL_SPELLINGS[li], ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
        for (size_t ni = 0; ni < sizeof(NOT_LABELS_STRUCTURED) / sizeof(NOT_LABELS_STRUCTURED[0]); ni++) {
            valid_cells++;
            const char *body = (f == TF_STRUCTURED) ? NOT_LABELS_STRUCTURED[ni]
                                                    : NOT_LABELS_INLINE[ni];
            char nm[160];
            snprintf(nm, sizeof(nm), "pos/%s/not-a-label/%d", tf_name(f), (int)ni);
            text_wrap(f, body, buf, sizeof(buf));
            int ok = run_pos(nm, buf);
            fprintf(stderr, "  [pos][%-10s][not-a-label %d ] %s\n", tf_name(f), (int)ni,
                    ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }
    /* User text written into the generated C must not be able to end the
     * construct it is written into. Asserted on the EMITTED C — each program
     * compiles either way, and the defect was the code that followed. */
    {
        static const struct { const char *name, *code, *forbidden; } EC[] = {
            { "safety-text-stays-in-its-comment",
              "u32 g = 7;\n"
              "naked void f() {\n    asm {\n        instructions: \"nop\"\n"
              "        safety: \"an ordinary looking audit sentence */ g = 99; /* tail\"\n"
              "    }\n}\n"
              "i32 main() { return 0; }\n",
              "*/ g = 99;" },
            /* -std=c99: the trigraph (spelled ?\?/ in this file so that it is not one
             * HERE) is a backslash, and backslash-newline is removed
             * BEFORE comments are recognised — so this spelled the terminator
             * across a line break. No line break may reach the comment. */
            { "safety-text-trigraph-line-splice",
              "u32 g = 7;\n"
              "naked void f() {\n    asm {\n        instructions: \"nop\"\n"
              "        safety: \"an ordinary looking audit sentence *?\?/\n/ g = 99; /* tail\"\n"
              "    }\n}\n"
              "i32 main() { return 0; }\n",
              "sentence *?\?/\n" },
            { "instructions-string-no-trigraph",
              "naked void f() {\n    asm {\n        instructions: \"nop # what?\?/\"\n"
              "        clobbers: [\"cc\"]\n"
              "        safety: \"" SAFE "\"\n    }\n}\n"
              "i32 main() { return 0; }\n",
              "what?\?/" },
            /* (No cell for a clobber name: it is validated against the register
             * table, so free text never reaches that string. It goes through the
             * same escaper regardless.) */
            { "inline-literal-no-trigraph",
              "naked void f() {\n    asm(\"nop # what?\?/\" \"\\n\" \"nop\");\n}\n"
              "i32 main() { return 0; }\n",
              "what?\?/" },
            { "section-name-no-trigraph",
              "section(\".text.hot?\?/\") u32 hot() { return 3; }\n"
              "i32 main() { return 0; }\n",
              "hot?\?/" },
        };
        for (size_t ei = 0; ei < sizeof(EC) / sizeof(EC[0]); ei++) {
            valid_cells++;
            char nm[160];
            snprintf(nm, sizeof(nm), "pos/emitted-c/%s", EC[ei].name);
            int ok = run_pos_emit_lacks(nm, EC[ei].code, EC[ei].forbidden);
            fprintf(stderr, "  [pos][emitted C][%-34s] %s\n", EC[ei].name, ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }
    /* What follows a NUL is still judged (the text is not trusted to be cut
     * off), and the inline form — captured raw by the parser — refuses an
     * escape outside ZER's set instead of leaving it to C's reading. */
    {
        static const struct { const char *name, *code, *reason; } NG[] = {
            { "label-after-nul",
              "naked void f() {\n    asm {\n        instructions: \"nop\\0again: jmp again\"\n"
              "        safety: \"" SAFE "\"\n    }\n}\n"
              "i32 main() { return 0; }\n",
              "contains a label" },
            { "inline-second-literal-c-escape",
              "naked void f() {\n    asm(\"nop\\n\" \"nop\\1\");\n}\n"
              "i32 main() { return 0; }\n",
              "invalid escape sequence" },
            { "inline-short-hex-escape",
              "naked void f() {\n    asm(\"nop\\n\" \"nop\\x0\");\n}\n"
              "i32 main() { return 0; }\n",
              "invalid escape sequence" },
        };
        for (size_t ni = 0; ni < sizeof(NG) / sizeof(NG[0]); ni++) {
            valid_cells++;
            char nm[160];
            snprintf(nm, sizeof(nm), "neg/asm-text/%s", NG[ni].name);
            int ok = run_neg(nm, NG[ni].code, NG[ni].reason);
            fprintf(stderr, "  [neg][asm text ][%-34s] %s\n", NG[ni].name, ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
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
