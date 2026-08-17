/* test_hw_matrix.c — ISR / atomics / MMIO soundness oracle (2026-06-07).
 * Domain 2 of the limitations.md non-memory frontier.
 *
 * SCOPE DISCIPLINE (docs/firmware_safety_extensions.md): this oracle tests
 * PROGRAM-CONSEQUENCE only — wrong USES of hardware-derived values that have a
 * structural shadow (the §8 "✓" set, the LEFT branch of the §10 fork). It does
 * NOT test HARDWARE-CONSEQUENCE (the floor / right branch): writing 9601 to a
 * baud register (a structurally-valid value), read-clears / W1C side effects
 * (§16 floor), or whether a region declaration matches the silicon (Definition
 * B). Those are correctly COMPILED by ZER — a NEG cell for any of them would be
 * a wrong expectation, not a hole. Pending-gap features (@section, region
 * kinds, @reset_handler, linker symbols) are also excluded — not built yet.
 *
 * EMIT-ONLY harness: uses `-o /tmp/x.c` (checker + zercheck + emit, NO gcc) so
 * interrupt-handler attributes that hosted x86 gcc would reject don't mask the
 * zercheck verdict. exit 0 = the safety analysis ACCEPTED the program.
 *
 * NEG: a program-consequence violation MUST be rejected for the relevant reason
 * (mmio range / alignment / volatile / interrupt-context / read-modify-write).
 * POS: a structurally-valid hardware access MUST be accepted.
 * Integrity guard: a NEG rejection by parse/type error is flagged INVALID.
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

static int has_hw_reason(const char *eb) {
    return strstr(eb, "mmio") || strstr(eb, "range") || strstr(eb, "align") ||
           strstr(eb, "volatile") || strstr(eb, "interrupt") ||
           strstr(eb, "read-modify-write") || strstr(eb, "atomic") ||
           strstr(eb, "spawn") || strstr(eb, "ISR");
}

/* NEG: must reject for a program-consequence reason. EMIT-ONLY harness. */
static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_hw.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_hw.zer -o /tmp/_zer_hw.c 2>/tmp/_zer_hw.err", zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — program-consequence violation ACCEPTED\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_hw.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not a hw-safety check\n", name);
        fprintf(stderr, "    %.110s\n", eb);
        return 0;
    }
    if (has_hw_reason(eb)) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for a hardware-safety reason:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    return 0;
}

/* POS: a structurally-valid hardware access must be accepted (emit succeeds). */
static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_hw.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_hw.zer -o /tmp/_zer_hw.c 2>/tmp/_zer_hw.err", zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_hw.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — valid hardware access REJECTED:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

typedef enum {
    /* NEGATIVE — program-consequence violations (must reject) */
    HW_MMIO_NO_DECL,        /* @inttoptr const, no mmio range declared */
    HW_MMIO_OOB,            /* @inttoptr const outside declared mmio range */
    HW_MMIO_MISALIGNED,     /* @inttoptr *u32 at non-4-aligned addr in range */
    HW_VOLATILE_STRIP,      /* strip volatile from an MMIO pointer via @ptrcast */
    HW_SLAB_IN_ISR,         /* slab.alloc() inside interrupt handler */
    HW_SPAWN_IN_ISR,        /* spawn inside interrupt handler */
    HW_ISR_GLOBAL_NONVOLATILE, /* non-volatile global shared interrupt+main */
    HW_ISR_COMPOUND_RMW,    /* volatile global compound-assign shared ISR+main */
    /* POSITIVE — structurally-valid hardware access (must compile) */
    HW_MMIO_OK,             /* @inttoptr in range, aligned, volatile */
    HW_POOL_IN_ISR_OK,      /* pool.alloc() in interrupt (Pool is ISR-safe) */
    HW_ATOMIC_GLOBAL_OK,    /* @atomic_store(&g, 0) on a u32 global */
    HW_ISR_VOLATILE_OK,     /* volatile global shared ISR+main, plain assign */
    HWSCEN_COUNT
} HWScenario;

static int scenario_is_negative(HWScenario s) {
    switch (s) {
        case HW_MMIO_NO_DECL: case HW_MMIO_OOB: case HW_MMIO_MISALIGNED:
        case HW_VOLATILE_STRIP: case HW_SLAB_IN_ISR: case HW_SPAWN_IN_ISR:
        case HW_ISR_GLOBAL_NONVOLATILE: case HW_ISR_COMPOUND_RMW:
            return 1;
        case HW_MMIO_OK: case HW_POOL_IN_ISR_OK: case HW_ATOMIC_GLOBAL_OK:
        case HW_ISR_VOLATILE_OK:
            return 0;
        case HWSCEN_COUNT: break;
    }
    return 1;
}

static const char *scen_name(HWScenario s) {
    switch (s) {
        case HW_MMIO_NO_DECL:           return "mmio-no-decl";
        case HW_MMIO_OOB:               return "mmio-out-of-range";
        case HW_MMIO_MISALIGNED:        return "mmio-misaligned";
        case HW_VOLATILE_STRIP:         return "volatile-strip";
        case HW_SLAB_IN_ISR:            return "slab-in-isr";
        case HW_SPAWN_IN_ISR:           return "spawn-in-isr";
        case HW_ISR_GLOBAL_NONVOLATILE: return "isr-global-nonvolatile";
        case HW_ISR_COMPOUND_RMW:       return "isr-compound-rmw";
        case HW_MMIO_OK:                return "mmio-in-range-aligned";
        case HW_POOL_IN_ISR_OK:         return "pool-in-isr-ok";
        case HW_ATOMIC_GLOBAL_OK:       return "atomic-global-ok";
        case HW_ISR_VOLATILE_OK:        return "isr-volatile-plain-ok";
        case HWSCEN_COUNT: break;
    }
    return "?";
}

static void gen(HWScenario s, char *buf, size_t n) {
    switch (s) {
        case HW_MMIO_NO_DECL:
            snprintf(buf, n,
                "void f() { volatile *u32 r = @inttoptr(*u32, 0x40000000); r[0] = 1; }\n"
                "u32 main() { return 0; }\n");
            break;
        case HW_MMIO_OOB:
            snprintf(buf, n,
                "mmio 0x40000000..0x40000FFF;\n"
                "void f() { volatile *u32 r = @inttoptr(*u32, 0x50000000); r[0] = 1; }\n"
                "u32 main() { return 0; }\n");
            break;
        case HW_MMIO_MISALIGNED:
            snprintf(buf, n,
                "mmio 0x40000000..0x40000FFF;\n"
                "void f() { volatile *u32 r = @inttoptr(*u32, 0x40000001); r[0] = 1; }\n"
                "u32 main() { return 0; }\n");
            break;
        case HW_VOLATILE_STRIP:
            snprintf(buf, n,
                "mmio 0x40000000..0x40000FFF;\n"
                "void f() { volatile *u32 r = @inttoptr(*u32, 0x40000000);\n"
                "    *u32 plain = @ptrcast(*u32, r); plain[0] = 1; }\n"
                "u32 main() { return 0; }\n");
            break;
        case HW_SLAB_IN_ISR:
            snprintf(buf, n,
                "struct Task { u32 id; }\n"
                "Slab(Task) tasks;\n"
                "interrupt UART { Handle(Task) h = tasks.alloc() orelse return; tasks.free(h); }\n"
                "u32 main() { return 0; }\n");
            break;
        case HW_SPAWN_IN_ISR:
            snprintf(buf, n,
                "shared struct C { u32 v; }\n"
                "C g;\n"
                "void worker(*C c) { c.v = 1; }\n"
                "interrupt UART { spawn worker(&g); }\n"
                "u32 main() { return 0; }\n");
            break;
        case HW_ISR_GLOBAL_NONVOLATILE:
            snprintf(buf, n,
                "u32 g_flag;\n"
                "interrupt UART { g_flag = 1; }\n"
                "u32 main() { u32 x = g_flag; return x; }\n");
            break;
        case HW_ISR_COMPOUND_RMW:
            snprintf(buf, n,
                "volatile u32 g_cnt;\n"
                "interrupt UART { g_cnt += 1; }\n"
                "u32 main() { u32 x = g_cnt; return x; }\n");
            break;
        case HW_MMIO_OK:
            snprintf(buf, n,
                "mmio 0x40000000..0x40000FFF;\n"
                "void f() { volatile *u32 r = @inttoptr(*u32, 0x40000000); r[0] = 9601; }\n"
                "u32 main() { return 0; }\n");
            break;
        case HW_POOL_IN_ISR_OK:
            snprintf(buf, n,
                "struct Task { u32 id; }\n"
                "Pool(Task, 8) tasks;\n"
                "interrupt UART { Handle(Task) h = tasks.alloc() orelse return; tasks.free(h); }\n"
                "u32 main() { return 0; }\n");
            break;
        case HW_ATOMIC_GLOBAL_OK:
            snprintf(buf, n,
                "u32 g_ctr;\n"
                "u32 main() { @atomic_store(&g_ctr, 0); u32 v = @atomic_add(&g_ctr, 1); return v; }\n");
            break;
        case HW_ISR_VOLATILE_OK:
            snprintf(buf, n,
                "volatile u32 g_flag;\n"
                "interrupt UART { g_flag = 1; }\n"
                "u32 main() { u32 x = g_flag; return x; }\n");
            break;
        case HWSCEN_COUNT: buf[0] = 0; break;
    }
}


/* ================================================================
 * VOLATILE-WIDTH GRID (2026-08-03) — SITE x SHAPE.
 *
 * WHY IT IS A CROSS-PRODUCT AND NOT A ROW. The defect this guards was not
 * "a check was missing" — it was TWO SITES DRIFTING APART. `volatile` exempts a
 * global from a data-race check at two independent places:
 *
 *     spawn path -> scan_unsafe_global_access   (thread races main)
 *     ISR   path -> check_interrupt_safety      (ISR races main)
 *
 * The spawn site got a width guard on 2026-08-03; the ISR site was missed and
 * kept accepting a tearing access for another commit. Crossing SITE with SHAPE
 * makes DISAGREEMENT itself a failure: both sites must give the same verdict for
 * the same shape, so fixing one and forgetting the other fails the build.
 *
 * THE RULE BEING PINNED. The exemption exists for the SINGLE-WORD volatile-flag
 * idiom. A scalar no wider than the target word is exempt; anything wider (or an
 * aggregate) lowers to several loads/stores, so a concurrent reader can TEAR —
 * observe half of one write and half of another. Both sites now ask one
 * predicate, volatile_global_exempt_from_race_check.
 *
 * TARGET-AWARE: the over-width cell runs at --target-bits 32, where u64 is two
 * stores. The same program is legitimately ACCEPTED at 64 bits, so the cell
 * carries the flag rather than assuming the host.
 *
 * EMIT-ONLY (inherited): ISR cells need `-o x.c`, because hosted x86 gcc rejects
 * the interrupt attribute and would mask the checker verdict entirely — that
 * masking is exactly why this hole survived a manual probe.
 * ================================================================ */

typedef enum { VSITE_SPAWN, VSITE_ISR, VSITE_COUNT } VSite;
typedef enum { VSHAPE_WORD, VSHAPE_OVERWIDTH, VSHAPE_AGGREGATE, VSHAPE_COUNT } VShape;

static const char *vsite_name(VSite s) {
    switch (s) {
    case VSITE_SPAWN: return "spawn";
    case VSITE_ISR:   return "isr";
    case VSITE_COUNT: break;
    }
    return "?";
}
static const char *vshape_name(VShape s) {
    switch (s) {
    case VSHAPE_WORD:      return "single-word-scalar";
    case VSHAPE_OVERWIDTH: return "over-width(u64@32)";
    case VSHAPE_AGGREGATE: return "aggregate-struct";
    case VSHAPE_COUNT: break;
    }
    return "?";
}
/* A single-word scalar is the sanctioned idiom -> ACCEPT. Everything else tears. */
static int vshape_is_negative(VShape s) { return s != VSHAPE_WORD; }
static const char *vshape_flags(VShape s) {
    return s == VSHAPE_OVERWIDTH ? "--target-bits 32" : "";
}

/* ---------------------------------------------------------------------------
 * RMW FORM grid (BUG-792) — site x HOW THE READ-MODIFY-WRITE IS SPELLED/REACHED.
 *
 * One question: "does this perform a non-atomic read-modify-write on a volatile
 * global?" It was answered SYNTACTICALLY — only `g += 1` with `g` named directly
 * — so five other spellings of the identical operation compiled clean. The spawn
 * variant of RFORM_PTR_PARAM is TSan-CONFIRMED racy.
 *
 * Crossed with SITE because spawn and ISR are mirrored sinks: this project's
 * recurring defect is fixing a form at one and leaving it broken at the other
 * (the `volatile` exemption did exactly that and shipped a tearing bare-metal
 * access). Every cell is NEGATIVE — each is a genuine lost-update race.
 *
 * No `default:` in the switches, so adding an RFORM value fails the build until
 * both sinks are taught it.
 * ------------------------------------------------------------------------- */
typedef enum { RFORM_NAMED_COMPOUND, RFORM_WRITTEN_OUT, RFORM_LOCAL_ALIAS,
               RFORM_PTR_PARAM, RFORM_PTR_PARAM_2HOP, RFORM_GLOBAL_ALIAS,
               /* BUG-803 (2026-08-17): a BIT-RANGE write is an IMPLICIT RMW —
                * the emitter lowers `g[3..0] = v` to `*p = (*p & ~mask) | v`.
                * It is spelled with `=` and its RHS never names the global, so
                * both is_rmw disjuncts were false and it slipped at BOTH sinks
                * while every `+=` spelling above was caught. The SPELLING axis
                * of the same question. */
               RFORM_BIT_RANGE,
               /* BUG-806 (2026-08-17): the RMW is performed by a HELPER, attributed
                * to the caller via the MEMOISED per-function summary. The ISR side of
                * this family was already closed by ISR-TRANS; the MAIN side had no
                * equivalent. Distinct from RFORM_PTR_PARAM above, which the ISR sink
                * reaches by walking the interrupt body — this cell exercises the
                * SUMMARY path, so both sinks must answer. */
               RFORM_HELPER_SUMMARY,
               RFORM_COUNT } RForm;
static const char *rform_name(RForm f) {
    switch (f) {
    case RFORM_NAMED_COMPOUND:  return "named g+=1";
    case RFORM_WRITTEN_OUT:     return "written g=g+1";
    case RFORM_LOCAL_ALIAS:     return "local *p+=1";
    case RFORM_PTR_PARAM:       return "param *p+=1";
    case RFORM_PTR_PARAM_2HOP:  return "param 2-hop";
    case RFORM_GLOBAL_ALIAS:    return "global *gp+=1";
    case RFORM_BIT_RANGE:       return "bitrange g[3..0]=v";
    case RFORM_HELPER_SUMMARY:  return "helper summary";
    case RFORM_COUNT: break;
    }
    return "?";
}
/* The RMW body, and any helper it needs, for one form. */
static void rform_parts(RForm f, const char **helper, const char **body) {
    switch (f) {
    case RFORM_NAMED_COMPOUND: *helper = "";                                    *body = "g += 1;";        break;
    case RFORM_WRITTEN_OUT:    *helper = "";                                    *body = "g = g + 1;";     break;
    case RFORM_LOCAL_ALIAS:    *helper = "";                                    *body = "volatile *u32 p = &g; *p += 1;"; break;
    case RFORM_PTR_PARAM:      *helper = "void bump(volatile *u32 p){ *p += 1; }"; *body = "bump(&g);";   break;
    case RFORM_PTR_PARAM_2HOP: *helper = "void inner(volatile *u32 p){ *p += 1; }\nvoid mid(volatile *u32 p){ inner(p); }";
                                                                                 *body = "mid(&g);";      break;
    case RFORM_GLOBAL_ALIAS:   *helper = "volatile *u32 gp = &g;";              *body = "*gp += 1;";      break;
    case RFORM_BIT_RANGE:      *helper = "";                                    *body = "g[3..0] = 5;";   break;
    case RFORM_HELPER_SUMMARY: *helper = "void h_bump(volatile *u32 p){ *p += 1; }"; *body = "h_bump(&g);"; break;
    case RFORM_COUNT:          *helper = ""; *body = ""; break;
    }
}
static void gen_rmw(VSite site, RForm f, char *out, size_t n) {
    const char *helper; const char *body;
    rform_parts(f, &helper, &body);
    if (site == VSITE_SPAWN)
        snprintf(out, n, "volatile u32 g;\n%s\nvoid w(){ %s }\n"
                         "u32 main(){ spawn w(); u32 x = g; return x & 1; }\n", helper, body);
    else
        snprintf(out, n, "volatile u32 g;\n%s\ninterrupt TIMER { %s }\n"
                         "u32 main(){ u32 x = g; return x & 1; }\n", helper, body);
}

static void gen_vol(VSite site, VShape shape, char *out, size_t n) {
    const char *decl;
    const char *wr;
    const char *rd;
    switch (shape) {
    case VSHAPE_WORD:      decl = "volatile u32 g;";                      wr = "g = 1;";   rd = "u32 x = g;";   break;
    case VSHAPE_OVERWIDTH: decl = "volatile u64 g;";                      wr = "g = 1;";   rd = "u64 x = g;";   break;
    case VSHAPE_AGGREGATE: decl = "struct P{u32 a; u32 b;}\nvolatile P g;"; wr = "g.a = 1;"; rd = "u32 x = g.a;"; break;
    case VSHAPE_COUNT:     decl = ""; wr = ""; rd = ""; break;
    }
    if (site == VSITE_SPAWN) {
        snprintf(out, n,
            "%s\nvoid w(u32 a){ %s }\nu32 main(){ %s spawn w(1); return 0; }\n",
            decl, wr, rd);
    } else {
        snprintf(out, n,
            "%s\nvoid reader(){ %s }\ninterrupt TIMER { %s }\n"
            "u32 main(){ reader(); return 0; }\n",
            decl, rd, wr);
    }
}

/* run_neg/run_pos with per-cell zerc flags (the over-width cell needs a 32-bit
 * target). Same EMIT-ONLY contract and integrity guard as the helpers above. */
static int run_vol(const char *name, const char *code, const char *flags, int negative) {
    total++;
    FILE *f = fopen("/tmp/_zer_hw.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_hw.zer %s -o /tmp/_zer_hw.c 2>/tmp/_zer_hw.err",
             zerc_path, flags);
    int rc = system(cmd);
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_hw.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (!negative) {
        if (rc == 0) { passed++; return 1; }
        failed++; over_reject++;
        fprintf(stderr, "  FAIL [OVER-REJECT] %s — the sanctioned single-word idiom was REJECTED:\n    %.140s\n", name, eb);
        return 0;
    }
    if (rc == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — a TEARING volatile access COMPILED CLEAN\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — parse error, not a safety check:\n    %.140s\n", name, eb);
        return 0;
    }
    if (has_hw_reason(eb)) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for a hardware/concurrency reason:\n    %.140s\n", name, eb);
    return 0;
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== ISR / atomics / MMIO matrix (program-consequence, NOT hardware floor) ===\n");
    fprintf(stderr, "    NEG: wrong USE of a hw-derived value (structural shadow) -> must REJECT\n");
    fprintf(stderr, "    POS: structurally-valid hardware access -> must COMPILE (emit-only)\n");
    fprintf(stderr, "    (floor cases — 9601 baud value, read-clears, region hw-correctness — NOT tested)\n\n");

    char buf[1024];
    int grid_ok = 1, valid_cells = 0;
    for (HWScenario s = 0; s < HWSCEN_COUNT; s++) {
        valid_cells++;
        int neg = scenario_is_negative(s);
        char nm[128];
        snprintf(nm, sizeof(nm), "%s/%s", neg ? "neg" : "pos", scen_name(s));
        gen(s, buf, sizeof(buf));
        int ok = neg ? run_neg(nm, buf) : run_pos(nm, buf);
        fprintf(stderr, "  [%-3s][%-24s] %s\n",
                neg ? "neg" : "pos", scen_name(s), ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }

    /* ---- volatile-width grid: SITE x SHAPE (the two exemption sites must agree) ---- */
    fprintf(stderr, "\n  -- volatile-width grid (spawn vs ISR exemption must agree) --\n");
    char vbuf[1024];
    for (VSite vs = 0; vs < VSITE_COUNT; vs++) {
        for (VShape vp = 0; vp < VSHAPE_COUNT; vp++) {
            valid_cells++;
            int neg = vshape_is_negative(vp);
            char nm[192];
            snprintf(nm, sizeof(nm), "vol/%s/%s", vsite_name(vs), vshape_name(vp));
            gen_vol(vs, vp, vbuf, sizeof(vbuf));
            int ok = run_vol(nm, vbuf, vshape_flags(vp), neg);
            fprintf(stderr, "  [%-5s][%-18s][%-3s] %s\n",
                    vsite_name(vs), vshape_name(vp), neg ? "neg" : "pos",
                    ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }

    /* RMW FORM grid (BUG-792) — every cell negative; both sinks must agree. */
    fprintf(stderr, "\n--- RMW form grid (site x spelling) ---\n");
    for (VSite vs = 0; vs < VSITE_COUNT; vs++) {
        for (RForm rf = 0; rf < RFORM_COUNT; rf++) {
            valid_cells++;
            char rbuf[1024], rnm[192];
            snprintf(rnm, sizeof(rnm), "rmw/%s/%s", vsite_name(vs), rform_name(rf));
            gen_rmw(vs, rf, rbuf, sizeof(rbuf));
            int ok = run_vol(rnm, rbuf, "", 1);
            fprintf(stderr, "  [%-5s][%-15s][neg] %s\n",
                    vsite_name(vs), rform_name(rf), ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }

    fprintf(stderr, "\n=== hw-matrix: %d/%d cells correct ===\n", passed, valid_cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | over-rejections: %d\n",
            false_neg, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "HW MATRIX HAS HOLES — a program-consequence violation the analyzer\n");
        fprintf(stderr, "mishandles (false negative), or a valid hw access over-rejected.\n");
        return 1;
    }
    return 0;
}
