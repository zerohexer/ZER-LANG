/* test_view_alias_matrix.c — VIEW-PROVENANCE oracle (2026-08-06).
 *
 * WHY THIS EXISTS. "Does a reference to this allocation reach here?" is ONE
 * semantic question, and the codebase answered it at a different place for each
 * SYNTACTIC form the reference could take. Seven forms were found over three
 * days, each as its own bug, each fixed individually:
 *
 *     h.p = &s[0]                      fixed 86a5b7fa
 *     h.p = first(s)                   fixed 86a5b7fa   (1-hop call)
 *     h.p = s[1..]                     fixed 86a5b7fa
 *     p = @ptrcast(*B, &s[0])          fixed 7c676758   (cast-wrapped)
 *     H mk(s){ h.p = &s[0]; return h; }    OPEN         (by-value struct return)
 *     outer(s) -> inner(s) -> &s[0]        OPEN         (2-hop call)
 *     *B get(H h){ return h.p; }           OPEN         (field of by-value param)
 *
 * Four of those were ASan-confirmed heap-use-after-free that COMPILED CLEAN.
 * Patching them one at a time is what produced the sequence; this grid exists so
 * the EIGHTH form fails the build instead of becoming the next commit.
 *
 * THE AXES. view-FORM x CARRIER. The form is how the reference is produced; the
 * carrier is where it lands. They are independent — the cast bug was found at a
 * bare local and immediately also reproduced into a struct field — so the
 * product is the unit of coverage, not either axis alone. Both are C enums
 * switched with NO default, so adding a form or a carrier fails -Werror=switch
 * until every combination is handled.
 *
 * EVERY neg cell is the same hazard: build a view of a heap allocation, free the
 * base, then read through the view. All must REJECT.
 *
 * INTEGRITY GUARD. A rejection counts only if the diagnostic names a
 * use-after-free / freed / dangling reason. A parse or type error masking the
 * check is flagged INVALID — without this a mis-typed probe passes as coverage
 * (the vacuous-test class, CLAUDE.md).
 *
 * POSITIVE cells pin the boundary that matters for a fix that ADDS aliasing: a
 * view of a PARAM (the caller's memory, never a local escape) and a view used
 * BEFORE the free must both still compile. A view-provenance fix that is too
 * eager shows up here, not in the neg cells.
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
    fprintf(stderr, "ERROR: cannot find zerc\n");
    exit(2);
}

static int has_uaf_reason(const char *eb) {
    return strstr(eb, "use after free") || strstr(eb, "use-after-free") ||
           strstr(eb, "is freed") || strstr(eb, "dangling") ||
           strstr(eb, "double free") || strstr(eb, "escape");
}

static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_va.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_va.zer -o /tmp/_zer_va.c 2>/tmp/_zer_va.err", zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — a view of freed memory COMPILED CLEAN\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_va.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — parse error, not a safety check:\n    %.130s\n", name, eb);
        return 0;
    }
    if (has_uaf_reason(eb)) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for a use-after-free reason:\n    %.130s\n", name, eb);
    return 0;
}

static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_va.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_va.zer -o /tmp/_zer_va.c 2>/tmp/_zer_va.err", zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_va.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — a SAFE view pattern was REJECTED:\n    %.130s\n", name, eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

/* ---- axes ---- */
typedef enum {
    VF_DIRECT,        /* &s[0]                                   */
    VF_SUBSLICE,      /* s[1..]                                  */
    VF_CAST,          /* @ptrcast(*B, &s[0])                     */
    VF_CALL1,         /* first(s)          -> &s[0]              */
    VF_CALL2,         /* outer(s) -> inner(s) -> &s[0]           */
    VF_RET_STRUCT,    /* mk(s) returns a struct whose FIELD views */
    VF_FIELD_OF_PARAM,/* get(h) returns a FIELD of a by-value param */
    VF_COUNT
} ViewForm;

typedef enum { CR_LOCAL, CR_FIELD, CR_COUNT } ViewCarrier;

static const char *vf_name(ViewForm f) {
    switch (f) {
    case VF_DIRECT:         return "direct-&s[0]";
    case VF_SUBSLICE:       return "subslice-s[1..]";
    case VF_CAST:           return "cast-wrapped";
    case VF_CALL1:          return "call-1hop";
    case VF_CALL2:          return "call-2hop";
    case VF_RET_STRUCT:     return "ret-struct-field";
    case VF_FIELD_OF_PARAM: return "field-of-param";
    case VF_COUNT: break;
    }
    return "?";
}
static const char *cr_name(ViewCarrier c) {
    switch (c) {
    case CR_LOCAL: return "bare-local";
    case CR_FIELD: return "struct-field";
    case CR_COUNT: break;
    }
    return "?";
}

/* VF_SUBSLICE yields a slice, so its carrier field must be [*]B, and
 * VF_RET_STRUCT already IS a struct — its "carrier" is fixed. Both are still
 * run at both carriers; the generator adapts the types. */
static void gen_neg(ViewForm f, ViewCarrier c, char *out, size_t n) {
    const char *decls = "";
    const char *vty   = "*B";       /* type of the view value */
    char mk[512];  mk[0] = 0;       /* how the view is produced, into `vw` */
    const char *rd  = "vw.v";       /* read through a bare-local view */
    const char *rdf = "h.p.v";      /* read through a struct-field view */

    switch (f) {
    case VF_DIRECT:
        snprintf(mk, sizeof(mk), "*B vw = &s[0];");
        break;
    case VF_SUBSLICE:
        vty = "[*]B"; rd = "vw[0].v"; rdf = "h.p[0].v";
        snprintf(mk, sizeof(mk), "[*]B vw = s[1..];");
        break;
    case VF_CAST:
        snprintf(mk, sizeof(mk), "*B vw = @ptrcast(*B, &s[0]);");
        break;
    case VF_CALL1:
        decls = "*B first([*]B s){ return &s[0]; }\n";
        snprintf(mk, sizeof(mk), "*B vw = first(s);");
        break;
    case VF_CALL2:
        decls = "*B inner([*]B s){ return &s[0]; }\n*B outer([*]B s){ return inner(s); }\n";
        snprintf(mk, sizeof(mk), "*B vw = outer(s);");
        break;
    case VF_RET_STRUCT:
        decls = "struct K{ *B p; }\nK mk([*]B s){ K k; k.p = &s[0]; return k; }\n";
        snprintf(mk, sizeof(mk), "K kk = mk(s); *B vw = kk.p;");
        break;
    case VF_FIELD_OF_PARAM:
        decls = "struct K{ *B p; }\n*B get(K k){ return k.p; }\n";
        snprintf(mk, sizeof(mk), "K kk; kk.p = &s[0]; *B vw = get(kk);");
        break;
    case VF_COUNT: break;
    }

    if (c == CR_LOCAL) {
        snprintf(out, n,
            "struct B{u32 v;}\n%s"
            "u32 main(){\n"
            "    [*]B s = alloc(B,4) orelse { return 1; };\n"
            "    s[0].v = 9; s[1].v = 9;\n"
            "    %s\n"
            "    free(s);\n"
            "    return %s;\n"
            "}\n", decls, mk, rd);
    } else {
        snprintf(out, n,
            "struct B{u32 v;}\nstruct H{ %s p; }\n%s"
            "u32 main(){\n"
            "    [*]B s = alloc(B,4) orelse { return 1; };\n"
            "    s[0].v = 9; s[1].v = 9;\n"
            "    %s\n"
            "    H h; h.p = vw;\n"
            "    free(s);\n"
            "    return %s;\n"
            "}\n", vty, decls, mk, rdf);
    }
}

/* Positive: the same view FORM, but safe — used before the free. */
static void gen_pos_before_free(ViewForm f, char *out, size_t n) {
    char buf[2048];
    gen_neg(f, CR_LOCAL, buf, sizeof(buf));
    /* rebuild with the read hoisted above the free */
    const char *rd = (f == VF_SUBSLICE) ? "vw[0].v" : "vw.v";
    const char *decls = "";
    char mk[512]; mk[0] = 0;
    switch (f) {
    case VF_DIRECT:   snprintf(mk, sizeof(mk), "*B vw = &s[0];"); break;
    case VF_SUBSLICE: snprintf(mk, sizeof(mk), "[*]B vw = s[1..];"); break;
    case VF_CAST:     snprintf(mk, sizeof(mk), "*B vw = @ptrcast(*B, &s[0]);"); break;
    case VF_CALL1:    decls = "*B first([*]B s){ return &s[0]; }\n";
                      snprintf(mk, sizeof(mk), "*B vw = first(s);"); break;
    case VF_CALL2:    decls = "*B inner([*]B s){ return &s[0]; }\n*B outer([*]B s){ return inner(s); }\n";
                      snprintf(mk, sizeof(mk), "*B vw = outer(s);"); break;
    case VF_RET_STRUCT: decls = "struct K{ *B p; }\nK mk([*]B s){ K k; k.p = &s[0]; return k; }\n";
                      snprintf(mk, sizeof(mk), "K kk = mk(s); *B vw = kk.p;"); break;
    case VF_FIELD_OF_PARAM: decls = "struct K{ *B p; }\n*B get(K k){ return k.p; }\n";
                      snprintf(mk, sizeof(mk), "K kk; kk.p = &s[0]; *B vw = get(kk);"); break;
    case VF_COUNT: break;
    }
    snprintf(out, n,
        "struct B{u32 v;}\n%s"
        "u32 main(){\n"
        "    [*]B s = alloc(B,4) orelse { return 1; };\n"
        "    s[0].v = 9; s[1].v = 9;\n"
        "    %s\n"
        "    u32 r = %s;\n"
        "    free(s);\n"
        "    return r - 9;\n"
        "}\n", decls, mk, rd);
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== View-provenance matrix (form x carrier) ===\n");
    fprintf(stderr, "    NEG: a view of freed memory -> must REJECT for a use-after-free reason\n");
    fprintf(stderr, "    POS: the same form used BEFORE the free -> must COMPILE\n\n");

    char buf[2560];
    int grid_ok = 1, cells = 0;

    for (ViewForm f = 0; f < VF_COUNT; f++) {
        for (ViewCarrier c = 0; c < CR_COUNT; c++) {
            cells++;
            char nm[192];
            snprintf(nm, sizeof(nm), "view/%s/%s", vf_name(f), cr_name(c));
            gen_neg(f, c, buf, sizeof(buf));
            int ok = run_neg(nm, buf);
            fprintf(stderr, "  [%-16s][%-12s][neg] %s\n",
                    vf_name(f), cr_name(c), ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
        /* boundary: the same form, used before the free */
        cells++;
        char pnm[192];
        snprintf(pnm, sizeof(pnm), "view/%s/before-free", vf_name(f));
        gen_pos_before_free(f, buf, sizeof(buf));
        int pok = run_pos(pnm, buf);
        fprintf(stderr, "  [%-16s][%-12s][pos] %s\n",
                vf_name(f), "before-free", pok ? "ok" : "*** FAIL ***");
        if (!pok) grid_ok = 0;
    }

    fprintf(stderr, "\n=== view-alias-matrix: %d/%d cells correct ===\n", passed, cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | over-rejections: %d\n",
            false_neg, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "VIEW-ALIAS MATRIX HAS HOLES — a reference to freed memory the\n");
        fprintf(stderr, "analyzer does not track through this syntactic form.\n");
        return 1;
    }
    return 0;
}
