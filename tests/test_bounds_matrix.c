/* test_bounds_matrix.c — bounded-index VERDICT oracle (2026-08-16).
 *
 * THE CLASS. "Does this index's proven range settle the access?" is a single
 * question with THREE possible answers, asked at every sink that has a
 * compile-time bound. Before this grid it was answered independently per sink,
 * and the answers had drifted apart:
 *
 *            | in-bounds      | always-OOB      | straddling
 *   array[LIT] proven          hard error        n/a
 *   array[id]  proven          *** MISSING ***   guard
 *   array[f()] proven          hard error        guard
 *   mmio [LIT] proven          hard error        n/a
 *   mmio [id]  *** MISSING *** *** MISSING ***   guard
 *
 * Each missing cell was silent at BOTH ends: no compile error, and the runtime
 * fallback (`if (i >= N) { return <zero>; }`) returns from the function instead
 * of trapping — on bare metal an init routine simply stops half-way with no
 * fault. The missing mmio in-bounds cell was the other direction: a provably
 * safe access still paid for a guard.
 *
 * WHAT THIS GRID MEASURES. Not "is there a diagnostic" but WHICH OF THE THREE
 * VERDICTS the sink reaches — because two of the three failures (silent guard
 * instead of error; guard instead of elision) produce a program that compiles
 * and runs, so an exit-code-only probe scores them both "fine". The always-OOB
 * cells additionally assert the REASON substring, so an unrelated rule that
 * happens to reject the file cannot pass the cell for it.
 *
 * Axes are C enums switched with NO default: adding a sink or a range class
 * fails the -Werror=switch build until every cell is filled in.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static int silent = 0, wrong_reason = 0, over_reject = 0, lost_elision = 0;
static const char *zerc_path = NULL;

static void find_zerc(void) {
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    if (system("test -x /tmp/zerc") == 0) { zerc_path = "/tmp/zerc"; return; }
    fprintf(stderr, "ERROR: cannot find zerc (build it first)\n");
    exit(2);
}

/* ---------------- axes ---------------- */

typedef enum {
    SINK_ARRAY_LIT,     /* u32[4] a;  a[<literal>]                  */
    SINK_ARRAY_IDENT,   /* u32[4] a;  u32 i = ...; a[i]             */
    SINK_ARRAY_CALL,    /* u32[4] a;  a[f()] with a return range    */
    SINK_MMIO_LIT,      /* @inttoptr-derived volatile *u32, r[<lit>]*/
    SINK_MMIO_IDENT,    /* ... u32 i = ...; r[i]                    */
    SINK_COUNT,
} Sink;

typedef enum {
    RC_IN_BOUNDS,       /* every value in range is a valid index  -> compile, NO guard */
    RC_ALWAYS_OOB,      /* no value in range is a valid index     -> compile ERROR     */
    RC_STRADDLE,        /* range crosses the bound                -> compile, guard    */
    RC_DEAD,            /* contradictory narrowing (empty range)  -> compile, no error */
    RC_COUNT,
} RangeClass;

static const char *sink_name(Sink s) {
    switch (s) {
    case SINK_ARRAY_LIT:   return "array[lit]";
    case SINK_ARRAY_IDENT: return "array[ident]";
    case SINK_ARRAY_CALL:  return "array[call]";
    case SINK_MMIO_LIT:    return "mmio[lit]";
    case SINK_MMIO_IDENT:  return "mmio[ident]";
    case SINK_COUNT:       return "?";
    }
    return "?";
}

static const char *rc_name(RangeClass r) {
    switch (r) {
    case RC_IN_BOUNDS:  return "in-bounds";
    case RC_ALWAYS_OOB: return "always-oob";
    case RC_STRADDLE:   return "straddle";
    case RC_DEAD:       return "dead";
    case RC_COUNT:      return "?";
    }
    return "?";
}

/* A literal index cannot straddle and cannot be dead — those cells have no
 * program to write, so they are skipped rather than faked. */
static int cell_is_valid(Sink s, RangeClass r) {
    int lit = (s == SINK_ARRAY_LIT || s == SINK_MMIO_LIT);
    if (lit && (r == RC_STRADDLE || r == RC_DEAD)) return 0;
    /* the call sink carries a RETURN range, which is a constant per callee:
     * straddling and dead have no faithful call-shaped encoding here. */
    if (s == SINK_ARRAY_CALL && (r == RC_STRADDLE || r == RC_DEAD)) return 0;
    return 1;
}

/* ---------------- program generation ----------------
 * Bound is 4 in every cell: a `u32[4]` array, and an mmio range of 16 bytes =
 * 4 u32 words. In-bounds index 2, out-of-bounds index 100. */

static void gen(Sink s, RangeClass r, char *buf, size_t n) {
    const char *mmio_decl = "mmio 0x40000000..0x4000000F;\n";
    const char *reg_decl  = "    volatile *u32 reg = @inttoptr(*u32, 0x40000000);\n";

    switch (s) {
    case SINK_ARRAY_LIT:
        snprintf(buf, n,
            "u32 main() {\n    u32[4] arr;\n    return arr[%s];\n}\n",
            r == RC_IN_BOUNDS ? "2" : "100");
        break;

    case SINK_ARRAY_IDENT:
        switch (r) {
        case RC_IN_BOUNDS:
            snprintf(buf, n, "u32 main() {\n    u32[4] arr;\n    u32 i = 2;\n    return arr[i];\n}\n");
            break;
        case RC_ALWAYS_OOB:
            snprintf(buf, n, "u32 main() {\n    u32[4] arr;\n    u32 i = 100;\n    return arr[i];\n}\n");
            break;
        case RC_STRADDLE:
            snprintf(buf, n,
                "u32 seed() { return 9; }\n"
                "u32 main() {\n    u32[4] arr;\n    u32 i = 100;\n"
                "    if (seed() > 0) { i = 2; }\n    return arr[i];\n}\n");
            break;
        case RC_DEAD:
            snprintf(buf, n,
                "u32 main() {\n    u32[4] arr;\n    u32 i = 0;\n"
                "    if (i > 5) { return arr[i]; }\n    return 0;\n}\n");
            break;
        case RC_COUNT: buf[0] = 0; break;
        }
        break;

    case SINK_ARRAY_CALL:
        /* Two shapes find_return_range actually derives: `x % N` -> [0, N-1]
         * and a constant return -> [K, K]. `x % N + K` derives NOTHING, so a
         * probe written that way silently tests the unproven path instead —
         * exactly the malformed-probe trap this suite keeps hitting. */
        snprintf(buf, n,
            "u32 idx(u32 x) { %s }\n"
            "u32 main() {\n    u32[4] arr;\n    return arr[idx(7)];\n}\n",
            r == RC_IN_BOUNDS ? "return x % 3;" : "return 100;");
        break;

    case SINK_MMIO_LIT:
        snprintf(buf, n, "%su32 main() {\n%s    return reg[%s];\n}\n",
                 mmio_decl, reg_decl, r == RC_IN_BOUNDS ? "2" : "100");
        break;

    case SINK_MMIO_IDENT:
        switch (r) {
        case RC_IN_BOUNDS:
            snprintf(buf, n, "%su32 main() {\n%s    u32 i = 2;\n    return reg[i];\n}\n",
                     mmio_decl, reg_decl);
            break;
        case RC_ALWAYS_OOB:
            snprintf(buf, n, "%su32 main() {\n%s    u32 i = 100;\n    return reg[i];\n}\n",
                     mmio_decl, reg_decl);
            break;
        case RC_STRADDLE:
            snprintf(buf, n,
                "%su32 seed() { return 9; }\nu32 main() {\n%s    u32 i = 100;\n"
                "    if (seed() > 0) { i = 2; }\n    return reg[i];\n}\n",
                mmio_decl, reg_decl);
            break;
        case RC_DEAD:
            snprintf(buf, n,
                "%su32 main() {\n%s    u32 i = 0;\n"
                "    if (i > 5) { return reg[i]; }\n    return 0;\n}\n",
                mmio_decl, reg_decl);
            break;
        case RC_COUNT: buf[0] = 0; break;
        }
        break;

    case SINK_COUNT: buf[0] = 0; break;
    }
}

/* Compile the cell. Returns 0 = accepted, 1 = rejected. Fills `err` with
 * stderr and `emitted` with the generated C (empty when rejected). */
static int compile_cell(const char *code, char *err, size_t errn,
                        char *emitted, size_t emn) {
    FILE *f = fopen("/tmp/_zer_bnd.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); exit(2); }
    fputs(code, f); fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_bnd.zer -o /tmp/_zer_bnd.c >/tmp/_zer_bnd.err 2>&1",
             zerc_path);
    system(cmd);

    err[0] = 0; emitted[0] = 0;
    FILE *e = fopen("/tmp/_zer_bnd.err", "r");
    if (e) { size_t got = fread(err, 1, errn - 1, e); err[got] = 0; fclose(e); }

    /* `-o <.c>` returns 0 even on checker errors (CLAUDE.md "zerc -o gotchas"),
     * so the VERDICT comes from the diagnostic text, never the exit code. */
    int rejected = (strstr(err, "error") != NULL) || (strstr(err, "zercheck:") != NULL);
    if (!rejected) {
        FILE *c = fopen("/tmp/_zer_bnd.c", "r");
        if (c) { size_t got = fread(emitted, 1, emn - 1, c); emitted[got] = 0; fclose(c); }
    }
    return rejected;
}

/* Does the ACCESS carry a bounds guard? Both forms count: the auto-guard
 * (`if (i >= 4u) return ...`) and the trapping `_zer_bounds_check`.
 *
 * Scoped to the body of `main` on purpose: `_zer_bounds_check` is defined as a
 * static inline in the preamble of EVERY emitted file, so a whole-file strstr
 * reports "guarded" for every cell and the elision half of the grid silently
 * always passes. (It did, on the first run of this file.) */
static int has_guard(const char *emitted) {
    const char *body = strstr(emitted, "uint32_t main(void) {");
    if (!body) return 0;
    const char *end = strstr(body, "\n}");
    size_t len = end ? (size_t)(end - body) : strlen(body);
    for (size_t i = 0; i + 5 < len; i++) {
        if (memcmp(body + i, ">= 4u", 5) == 0) return 1;
        if (i + 17 < len && memcmp(body + i, "_zer_bounds_check", 17) == 0) return 1;
    }
    return 0;
}

static int run_cell(Sink s, RangeClass r, const char *code) {
    total++;
    char err[8192], emitted[65536];
    int rejected = compile_cell(code, err, sizeof(err), emitted, sizeof(emitted));

    int ok = 0;
    switch (r) {
    case RC_ALWAYS_OOB:
        if (!rejected) {
            silent++;
            fprintf(stderr, "  FAIL [SILENT] %s/%s — provably-OOB access COMPILED "
                            "(runtime falls into the guard's early return)\n",
                    sink_name(s), rc_name(r));
        } else if (!strstr(err, "out of bounds") && !strstr(err, "out of range")) {
            wrong_reason++;
            fprintf(stderr, "  FAIL [WRONG-REASON] %s/%s — rejected, but not for the "
                            "bound. Another rule is masking this cell:\n    %.200s\n",
                    sink_name(s), rc_name(r), err);
        } else ok = 1;
        break;

    case RC_IN_BOUNDS:
        if (rejected) {
            over_reject++;
            fprintf(stderr, "  FAIL [OVER-REJECT] %s/%s — provably safe access "
                            "REJECTED:\n    %.200s\n", sink_name(s), rc_name(r), err);
        } else if (has_guard(emitted)) {
            lost_elision++;
            fprintf(stderr, "  FAIL [LOST-ELISION] %s/%s — index is provably in "
                            "bounds but a runtime guard was still emitted\n",
                    sink_name(s), rc_name(r));
        } else ok = 1;
        break;

    case RC_STRADDLE:
        /* Must compile AND keep the guard: eliding here would be a real OOB. */
        if (rejected) {
            over_reject++;
            fprintf(stderr, "  FAIL [OVER-REJECT] %s/%s — a straddling range is not "
                            "an error:\n    %.200s\n", sink_name(s), rc_name(r), err);
        } else if (!has_guard(emitted)) {
            silent++;
            fprintf(stderr, "  FAIL [UNGUARDED] %s/%s — range crosses the bound and "
                            "NO guard was emitted\n", sink_name(s), rc_name(r));
        } else ok = 1;
        break;

    case RC_DEAD:
        /* An empty range is unreachable code: diagnosing it would reject dead
         * code that is perfectly legal. */
        if (rejected) {
            over_reject++;
            fprintf(stderr, "  FAIL [OVER-REJECT] %s/%s — unreachable access "
                            "diagnosed as an error:\n    %.200s\n",
                    sink_name(s), rc_name(r), err);
        } else ok = 1;
        break;

    case RC_COUNT: break;
    }

    if (ok) passed++; else failed++;
    return ok;
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== Bounded-index verdict matrix (sink x range class) ===\n");
    fprintf(stderr, "    in-bounds  -> compile, guard ELIDED\n");
    fprintf(stderr, "    always-oob -> compile ERROR naming the bound\n");
    fprintf(stderr, "    straddle   -> compile, guard PRESENT\n");
    fprintf(stderr, "    dead       -> compile, no diagnostic\n\n");

    char buf[2048];
    int grid_ok = 1, valid_cells = 0;
    for (Sink s = 0; s < SINK_COUNT; s++) {
        for (RangeClass r = 0; r < RC_COUNT; r++) {
            if (!cell_is_valid(s, r)) continue;
            valid_cells++;
            gen(s, r, buf, sizeof(buf));
            int ok = run_cell(s, r, buf);
            fprintf(stderr, "  [%-12s][%-10s] %s\n",
                    sink_name(s), rc_name(r), ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }

    fprintf(stderr, "\n=== bounds-matrix: %d/%d cells correct ===\n", passed, valid_cells);
    fprintf(stderr, "    silent: %d | wrong reason: %d | over-reject: %d | lost elision: %d\n",
            silent, wrong_reason, over_reject, lost_elision);
    if (!grid_ok) {
        fprintf(stderr, "BOUNDS MATRIX HAS HOLES — a bounded-index sink disagrees with "
                        "the others about what its range proves.\n");
        return 1;
    }
    (void)failed;
    return 0;
}
