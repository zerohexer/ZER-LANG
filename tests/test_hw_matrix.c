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
