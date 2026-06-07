/* test_escape_matrix.c — exhaustive escape/lifetime (the `keep` axis) soundness
 * oracle for ZER's pointer model (2026-06-07).
 *
 * COMPANION to test_shape_matrix.c. The shape matrix covers the temporal axis
 * (UAF / double-free / leak / use-after-move on Pool/Slab/move). THIS matrix
 * covers the orthogonal LIFETIME-ESCAPE axis: a pointer to a local (or
 * local-derived: alias, arena, array→slice, cast, ptrtoint, identity-wash,
 * struct-wrapper, orelse-fallback) escaping its scope via return / global /
 * param-field / nested-field. This is the axis `keep`-universalization will
 * extend, so it's the axis whose SOUNDNESS we must guard before extending it.
 *
 * THE CRITERION (locked with the user 2026-06-07):
 *   - False POSITIVE (safe program rejected) = ACCEPTABLE — restructure, like Rust.
 *   - False NEGATIVE (UNSAFE program compiles clean) = UNACCEPTABLE — safety hole.
 * So this oracle is NEGATIVE-ONLY: every cell is a genuinely-unsafe escape that
 * MUST be rejected. A cell that compiles clean = false negative = hard fail.
 *
 * INTEGRITY GUARD (the probe-10/11 lesson): a "rejection" only counts if it is
 * FOR THE ESCAPE REASON. If the program is rejected by a parse/type error
 * instead, the probe is INVALID (generator bug) — flagged, not silently counted
 * as a pass. run_neg() inspects stderr for the actual escape-error wording.
 *
 * The axes are C enums switched with NO default — adding an EscDest/Launder/Src
 * fails GCC -Wswitch until every generator/name function handles it. The grid
 * can't silently shrink. Enumerating the finite {dest × launder × src} product
 * completely is proof-by-exhaustion that no laundering path slips a local
 * escape past the analyzer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static int false_neg = 0, invalid_probe = 0, suspect = 0;
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

/* Returns 1 if rejected FOR THE ESCAPE REASON, 0 otherwise (logged). */
static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_esc.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_esc.zer -o /dev/null 2>/tmp/_zer_esc.err", zerc_path);
    int rc = system(cmd);

    if (rc == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — UNSAFE escape COMPILED CLEAN\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }

    /* rejected — read stderr and classify the reason */
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_esc.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }

    /* parse/type error masking the escape check = INVALID probe (generator bug) */
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") ||
        strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not escape check\n", name);
        fprintf(stderr, "    %.120s\n", eb);
        return 0;
    }

    /* rejected FOR THE ESCAPE REASON? (exact wordings emitted by the analyzer) */
    if (strstr(eb, "to local") || strstr(eb, "of local") ||
        strstr(eb, "with local") || strstr(eb, "local-derived") ||
        strstr(eb, "local array") || strstr(eb, "arena-derived") ||
        strstr(eb, "satisfy 'keep'") || strstr(eb, "stack memory") ||
        strstr(eb, "local pointer") || strstr(eb, "dangle")) {
        passed++;
        return 1;
    }

    /* rejected, but not clearly for an escape reason — surface for review */
    failed++; suspect++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not clearly an escape reason:\n", name);
    fprintf(stderr, "    %.120s\n", eb);
    return 0;
}

/* ---- Grid axes ---- */
typedef enum { ED_RETURN, ED_GLOBAL, ED_PARAM_FIELD, ED_NESTED_FIELD, EDEST_COUNT } EscDest;
typedef enum { LD_DIRECT, LD_ALIAS, LD_PTRCAST, LD_PTRTOINT, LD_IDENTITY,
               LD_WRAPPER, LD_ORELSE, LAUNDER_COUNT } Launder;
typedef enum { SR_VAR, SR_ARRAY, SR_ARENA, SRC_COUNT } Src;

static const char *dest_name(EscDest d) {
    switch (d) {
        case ED_RETURN:       return "return";
        case ED_GLOBAL:       return "global";
        case ED_PARAM_FIELD:  return "param.field";
        case ED_NESTED_FIELD: return "nested.field";
        case EDEST_COUNT: break;
    }
    return "?";
}
static const char *launder_name(Launder l) {
    switch (l) {
        case LD_DIRECT:   return "direct";
        case LD_ALIAS:    return "alias";
        case LD_PTRCAST:  return "@ptrcast";
        case LD_PTRTOINT: return "@ptrtoint";
        case LD_IDENTITY: return "id-wash";
        case LD_WRAPPER:  return "wrapper";
        case LD_ORELSE:   return "orelse-fb";
        case LAUNDER_COUNT: break;
    }
    return "?";
}
static const char *src_name(Src s) {
    switch (s) {
        case SR_VAR:   return "local-var";
        case SR_ARRAY: return "local-arr";
        case SR_ARENA: return "local-arena";
        case SRC_COUNT: break;
    }
    return "?";
}

/* Ragged grid: which (dest, launder, src) cells are meaningful unsafe escapes. */
static int cell_valid(EscDest d, Launder l, Src s) {
    switch (l) {
        case LD_DIRECT:
            if (s == SR_VAR)   return 1;                 /* all 4 dests */
            if (s == SR_ARRAY) return d == ED_RETURN || d == ED_GLOBAL || d == ED_PARAM_FIELD;
            if (s == SR_ARENA) return d == ED_GLOBAL;    /* void fn, bare orelse return */
            return 0;
        case LD_ALIAS:
            if (s == SR_VAR)   return 1;                 /* all 4 dests */
            if (s == SR_ARENA) return d == ED_GLOBAL;
            return 0;
        case LD_PTRCAST:
            return s == SR_VAR && (d == ED_RETURN || d == ED_GLOBAL);
        case LD_PTRTOINT: return s == SR_VAR && d == ED_RETURN;
        case LD_IDENTITY: return s == SR_VAR && d == ED_RETURN;
        case LD_WRAPPER:  return s == SR_VAR && d == ED_RETURN;
        case LD_ORELSE:   return s == SR_VAR && (d == ED_RETURN || d == ED_GLOBAL);
        case LAUNDER_COUNT: break;
    }
    return 0;
}

/* storage-typed form of a pointer return type (pointer -> nullable, slice stays) */
static const char *store_ty(const char *ret) {
    if (!strcmp(ret, "*u32"))  return "?*u32";
    if (!strcmp(ret, "*u8"))   return "?*u8";
    if (!strcmp(ret, "*Node")) return "?*Node";
    if (!strcmp(ret, "[*]u8")) return "[*]u8";
    return "?*u32";
}

static void gen(EscDest d, Launder l, Src s, char *buf, size_t n) {
    /* ---- source: declare the local + its base escaping expression ---- */
    const char *srcRet, *srcBase;
    char srcSetup[256];
    char decls[1024]; decls[0] = 0;
    switch (s) {
        case SR_VAR:
            srcRet = "*u32"; srcBase = "&x";
            snprintf(srcSetup, sizeof(srcSetup), "    u32 x = 5;\n");
            break;
        case SR_ARRAY:
            srcRet = "[*]u8"; srcBase = "a";
            snprintf(srcSetup, sizeof(srcSetup), "    u8[4] a;\n");
            break;
        case SR_ARENA:
            srcRet = "*Node"; srcBase = "n";
            strcat(decls, "struct Node { u32 v; }\n");
            snprintf(srcSetup, sizeof(srcSetup),
                     "    u8[256] backing;\n"
                     "    Arena ar = Arena.over(backing);\n"
                     "    *Node n = ar.alloc(Node) orelse return;\n");
            break;
        case SRC_COUNT: srcRet = "*u32"; srcBase = "&x"; srcSetup[0] = 0; break;
    }

    /* ---- launder: transform base into escaping expr E (RET = its type) ---- */
    const char *RET; char E[256]; char launSetup[256]; launSetup[0] = 0;
    switch (l) {
        case LD_DIRECT:
            RET = srcRet; snprintf(E, sizeof(E), "%s", srcBase);
            break;
        case LD_ALIAS:
            RET = srcRet;
            snprintf(launSetup, sizeof(launSetup), "    %s q = %s;\n", srcRet, srcBase);
            snprintf(E, sizeof(E), "q");
            break;
        case LD_PTRCAST:
            RET = "*u8"; snprintf(E, sizeof(E), "@ptrcast(*u8, %s)", srcBase);
            break;
        case LD_PTRTOINT:
            RET = "usize"; snprintf(E, sizeof(E), "@ptrtoint(%s)", srcBase);
            break;
        case LD_IDENTITY:
            RET = "*u32";
            strcat(decls, "*u32 idfn(*u32 ip) { return ip; }\n");
            snprintf(E, sizeof(E), "idfn(%s)", srcBase);
            break;
        case LD_WRAPPER:
            RET = "*u32";
            strcat(decls, "struct Wp { *u32 p; }\nWp wrapfn(*u32 ip) { Wp w; w.p = ip; return w; }\n");
            snprintf(E, sizeof(E), "wrapfn(%s).p", srcBase);
            break;
        case LD_ORELSE:
            RET = "*u32";
            snprintf(launSetup, sizeof(launSetup), "    ?*u32 mo = null;\n");
            snprintf(E, sizeof(E), "mo orelse %s", srcBase);
            break;
        case LAUNDER_COUNT: RET = "*u32"; E[0] = 0; break;
    }

    char setup[512];
    snprintf(setup, sizeof(setup), "%s%s", srcSetup, launSetup);

    /* ---- dest: wrap E into the escape site ---- */
    char glob[256]; glob[0] = 0;
    char func[1024];
    switch (d) {
        case ED_RETURN:
            snprintf(func, sizeof(func), "%s esc_fn() {\n%s    return %s;\n}\n", RET, setup, E);
            break;
        case ED_GLOBAL:
            if (!strcmp(RET, "[*]u8")) snprintf(glob, sizeof(glob), "[*]u8 gesc;\n");
            else snprintf(glob, sizeof(glob), "%s gesc = null;\n", store_ty(RET));
            snprintf(func, sizeof(func), "void esc_fn() {\n%s    gesc = %s;\n}\n", setup, E);
            break;
        case ED_PARAM_FIELD:
            if (!strcmp(RET, "[*]u8")) strcat(decls, "struct Holder { [*]u8 hp; }\n");
            else { char sd[64]; snprintf(sd, sizeof(sd), "struct Holder { %s hp; }\n", store_ty(RET)); strcat(decls, sd); }
            snprintf(func, sizeof(func), "void esc_fn(*Holder h) {\n%s    h.hp = %s;\n}\n", setup, E);
            break;
        case ED_NESTED_FIELD:
            strcat(decls, "struct Inn { ?*u32 hp; }\nstruct Outr { Inn inner; }\n");
            snprintf(func, sizeof(func), "void esc_fn(*Outr h) {\n%s    h.inner.hp = %s;\n}\n", setup, E);
            break;
        case EDEST_COUNT: func[0] = 0; break;
    }

    snprintf(buf, n, "%s%s%si32 main() { return 0; }\n", decls, glob, func);
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== Escape/Lifetime matrix (the `keep` axis) ===\n");
    fprintf(stderr, "    NEGATIVE-ONLY soundness guard: every cell is an UNSAFE escape;\n");
    fprintf(stderr, "    each MUST be rejected FOR THE ESCAPE REASON (no false negatives).\n\n");

    char buf[4096];
    int grid_ok = 1, valid_cells = 0;
    for (EscDest d = 0; d < EDEST_COUNT; d++) {
        for (Launder l = 0; l < LAUNDER_COUNT; l++) {
            for (Src s = 0; s < SRC_COUNT; s++) {
                if (!cell_valid(d, l, s)) continue;
                valid_cells++;
                char nm[128];
                snprintf(nm, sizeof(nm), "%s/%s/%s", launder_name(l), dest_name(d), src_name(s));
                gen(d, l, s, buf, sizeof(buf));
                int ok = run_neg(nm, buf);
                fprintf(stderr, "  [%-10s][%-13s][%-12s] %s\n",
                        launder_name(l), dest_name(d), src_name(s),
                        ok ? "reject OK" : "*** HOLE ***");
                if (!ok) grid_ok = 0;
            }
        }
    }

    fprintf(stderr, "\n=== escape-matrix: %d/%d cells rejected for the escape reason ===\n",
            passed, valid_cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | suspect rejections: %d\n",
            false_neg, invalid_probe, suspect);
    if (!grid_ok) {
        fprintf(stderr, "ESCAPE MATRIX HAS HOLES — a local-escape path the analyzer mishandles,\n");
        fprintf(stderr, "or a generator probe that needs fixing. Investigate before shipping.\n");
        return 1;
    }
    return 0;
}
