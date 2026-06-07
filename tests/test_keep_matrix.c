/* test_keep_matrix.c — keep-axis soundness oracle (2026-06-07).
 *
 * Companion to test_escape_matrix.c. The escape matrix covers LOCAL-pointer
 * escapes; this covers the KEEP axis: a NON-KEEP pointer parameter persisted
 * into a long-lived sink (global / param-field / nested-field) violates the
 * non-keep contract ("non-keep = won't be stored persistently") and must be
 * rejected — the fix is `keep p`, verified at the call site.
 *
 * TWO cell kinds (the keep axis has an escape valve, so positives matter):
 *   - NEGATIVE: a NON-keep param persisted (possibly laundered: alias, @ptrcast,
 *     call-result) MUST be rejected FOR THE KEEP REASON. A cell compiling clean
 *     = false negative = unsafe persistence the analyzer missed.
 *   - POSITIVE: the `keep` escape valve — a KEEP param persisted (direct, alias,
 *     @ptrcast) MUST compile. A rejection here = over-rejection of legit keep
 *     code (the keep valve is broken).
 *
 * INTEGRITY GUARD: a NEGATIVE rejection only counts if the error mentions the
 * keep contract (substring "keep") — a parse/type error masking the check is
 * flagged INVALID (the probe-10/11 lesson).
 *
 * Axes are C enums switched with NO default — adding a KSink/KLaunder/KKind
 * fails GCC -Wswitch until handled. The grid can't silently shrink.
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

/* NEGATIVE: must reject FOR THE KEEP REASON. Returns 1 ok, 0 otherwise. */
static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_keep.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_keep.zer -o /dev/null 2>/tmp/_zer_keep.err", zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — non-keep persist COMPILED CLEAN\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_keep.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not keep check\n", name);
        fprintf(stderr, "    %.110s\n", eb);
        return 0;
    }
    if (strstr(eb, "keep")) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for the keep reason:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    return 0;
}

/* POSITIVE: the keep valve must compile. Returns 1 ok, 0 over-rejection. */
static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_keep.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_keep.zer -o /dev/null 2>/tmp/_zer_keep.err", zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_keep.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — keep valve REJECTED a safe program:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

/* ---- Grid axes ---- */
typedef enum { KS_GLOBAL, KS_PARAM_FIELD, KS_NESTED_FIELD, KSINK_COUNT } KSink;
typedef enum { KL_DIRECT, KL_ALIAS, KL_PTRCAST, KL_CALL, KLAUNDER_COUNT } KLaunder;
typedef enum { KK_NEG, KK_POS, KKIND_COUNT } KKind;

static const char *sink_name(KSink s) {
    switch (s) {
        case KS_GLOBAL:       return "global";
        case KS_PARAM_FIELD:  return "param.field";
        case KS_NESTED_FIELD: return "nested.field";
        case KSINK_COUNT: break;
    }
    return "?";
}
static const char *launder_name(KLaunder l) {
    switch (l) {
        case KL_DIRECT:  return "direct";
        case KL_ALIAS:   return "alias";
        case KL_PTRCAST: return "@ptrcast";
        case KL_CALL:    return "call-result";
        case KLAUNDER_COUNT: break;
    }
    return "?";
}
static const char *kind_name(KKind k) {
    switch (k) {
        case KK_NEG: return "neg(non-keep)";
        case KK_POS: return "pos(keep)";
        case KKIND_COUNT: break;
    }
    return "?";
}

static int cell_valid(KSink s, KLaunder l, KKind k) {
    (void)s;
    switch (k) {
        case KK_NEG: return 1;                  /* all launders, all sinks */
        case KK_POS: return l != KL_CALL;       /* identity-call strips keep — no keep valve */
        case KKIND_COUNT: break;
    }
    return 0;
}

static void gen(KSink s, KLaunder l, KKind k, char *buf, size_t n) {
    const char *src   = (k == KK_NEG) ? "*u32 p" : "keep *u32 p";
    const char *field = (l == KL_PTRCAST) ? "?*u8" : "?*u32";
    char decls[512]; decls[0] = 0;

    char launSetup[128]; launSetup[0] = 0;
    char E[128];
    switch (l) {
        case KL_DIRECT:  snprintf(E, sizeof(E), "p"); break;
        case KL_ALIAS:   snprintf(launSetup, sizeof(launSetup), "    *u32 q = p;\n");
                         snprintf(E, sizeof(E), "q"); break;
        case KL_PTRCAST: snprintf(E, sizeof(E), "@ptrcast(*u8, p)"); break;
        case KL_CALL:    strcat(decls, "*u32 idfn(*u32 ip) { return ip; }\n");
                         snprintf(E, sizeof(E), "idfn(p)"); break;
        case KLAUNDER_COUNT: E[0] = 0; break;
    }

    char glob[96]; glob[0] = 0;
    char func[512];
    switch (s) {
        case KS_GLOBAL:
            snprintf(glob, sizeof(glob), "%s gk = null;\n", field);
            snprintf(func, sizeof(func), "void esc_fn(%s) {\n%s    gk = %s;\n}\n", src, launSetup, E);
            break;
        case KS_PARAM_FIELD: {
            char sd[64]; snprintf(sd, sizeof(sd), "struct Holder { %s hp; }\n", field);
            strcat(decls, sd);
            snprintf(func, sizeof(func), "void esc_fn(*Holder h, %s) {\n%s    h.hp = %s;\n}\n", src, launSetup, E);
            break;
        }
        case KS_NESTED_FIELD: {
            char sd[96]; snprintf(sd, sizeof(sd), "struct Inn { %s hp; }\nstruct Outr { Inn inner; }\n", field);
            strcat(decls, sd);
            snprintf(func, sizeof(func), "void esc_fn(*Outr h, %s) {\n%s    h.inner.hp = %s;\n}\n", src, launSetup, E);
            break;
        }
        case KSINK_COUNT: func[0] = 0; break;
    }

    snprintf(buf, n, "%s%s%si32 main() { return 0; }\n", decls, glob, func);
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== Keep-axis matrix (non-keep param persistence) ===\n");
    fprintf(stderr, "    NEG: non-keep param persisted -> must REJECT for the keep reason\n");
    fprintf(stderr, "    POS: keep param persisted (the valve) -> must COMPILE\n\n");

    char buf[2048];
    int grid_ok = 1, valid_cells = 0;
    for (KKind k = 0; k < KKIND_COUNT; k++) {
        for (KSink s = 0; s < KSINK_COUNT; s++) {
            for (KLaunder l = 0; l < KLAUNDER_COUNT; l++) {
                if (!cell_valid(s, l, k)) continue;
                valid_cells++;
                char nm[128];
                snprintf(nm, sizeof(nm), "%s/%s/%s", kind_name(k), launder_name(l), sink_name(s));
                gen(s, l, k, buf, sizeof(buf));
                int ok = (k == KK_NEG) ? run_neg(nm, buf) : run_pos(nm, buf);
                fprintf(stderr, "  [%-13s][%-11s][%-12s] %s\n",
                        kind_name(k), launder_name(l), sink_name(s),
                        ok ? "ok" : "*** FAIL ***");
                if (!ok) grid_ok = 0;
            }
        }
    }

    fprintf(stderr, "\n=== keep-matrix: %d/%d cells correct ===\n", passed, valid_cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | over-rejections: %d\n",
            false_neg, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "KEEP MATRIX HAS HOLES — a non-keep-persist path the analyzer mishandles,\n");
        fprintf(stderr, "or a keep-valve over-rejection. Investigate before shipping.\n");
        return 1;
    }
    return 0;
}
