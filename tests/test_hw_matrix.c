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
#include "zer_tmp.h"

static int total = 0, passed = 0, failed = 0;
static int false_neg = 0, invalid_probe = 0, over_reject = 0;
static const char *zerc_path = NULL;

/* ZER_MATRIX_ZERC overrides the search. Verifying that a NEW cell actually FIRES
 * means running this grid against a PRE-FIX compiler, and with a hardcoded
 * `./zerc` there was no way to do that — passing a path as argv[1] was silently
 * ignored, so a pre-fix run reported the same 37/37 as a fixed one and the
 * "gate proven non-vacuous" step quietly measured nothing (2026-08-28: it did
 * exactly that to me once). tools/sink_matrix.sh has always taken the path as
 * its first argument; this is the same affordance. */
static void find_zerc(void) {
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

static int has_hw_reason(const char *eb) {
    return strstr(eb, "mmio") || strstr(eb, "range") || strstr(eb, "align") ||
           strstr(eb, "volatile") || strstr(eb, "interrupt") ||
           strstr(eb, "read-modify-write") || strstr(eb, "atomic") ||
           strstr(eb, "spawn") || strstr(eb, "ISR");
}

/* NEG: must reject for a program-consequence reason. EMIT-ONLY harness. */
static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen(ZT("/tmp/_zer_hw.zer"), "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), ZT("%s /tmp/_zer_hw.zer -o /tmp/_zer_hw.c 2>/tmp/_zer_hw.err"), zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — program-consequence violation ACCEPTED\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen(ZT("/tmp/_zer_hw.err"), "r");
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
    FILE *f = fopen(ZT("/tmp/_zer_hw.zer"), "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), ZT("%s /tmp/_zer_hw.zer -o /tmp/_zer_hw.c 2>/tmp/_zer_hw.err"), zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen(ZT("/tmp/_zer_hw.err"), "r");
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
typedef enum { VSHAPE_WORD, VSHAPE_OVERWIDTH, VSHAPE_AGGREGATE, VSHAPE_OPTPTR, VSHAPE_OPTPTR_PLAIN, VSHAPE_COUNT } VShape;

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
    case VSHAPE_OPTPTR:    return "optional-pointer ?*volatile T";
    case VSHAPE_OPTPTR_PLAIN: return "optional-pointer ?*T (plain pointee)";
    case VSHAPE_COUNT: break;
    }
    return "?";
}
/* A single-word scalar is the sanctioned idiom -> ACCEPT. Everything else tears. */
/* BUG-1212: a `?*T` is a null-sentinel pointer — one word, like `*T`.
 * BUG-1249: but the exemption covers the WORD, and the scans cannot tell a read
 * of the word from a dereference of it — so the pointer is exempt only when its
 * pointee is itself volatile (or a shared struct). A plain pointee is negative. */
static int vshape_is_negative(VShape s) { return s != VSHAPE_WORD && s != VSHAPE_OPTPTR; }
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
typedef enum { RFORM_NAMED_COMPOUND, RFORM_WRITTEN_OUT, RFORM_GPTR_WRITTEN_OUT,
               RFORM_GPTR_COPY_WRITTEN_OUT, RFORM_LOCAL_ALIAS,
               RFORM_PTR_PARAM, RFORM_PTR_PARAM_2HOP, RFORM_GLOBAL_ALIAS,
               RFORM_SPLIT_STMT, RFORM_SPLIT_2HOP,
               RFORM_PARAM_SWITCH, RFORM_PARAM_ONCE, RFORM_PARAM_ORELSE,
               RFORM_PARAM_CALL_ARG, RFORM_PARAM_RETURN, RFORM_PARAM_IF_COND,
               RFORM_ALIAS_ARG, RFORM_CARRIER, RFORM_CARRIER_LIT,
               RFORM_FUNCPTR_LOCAL, RFORM_FUNCPTR_GLOBAL, RFORM_FUNCPTR_FIELD,
               RFORM_ALIAS_COPY, RFORM_CARRIER_COPY, RFORM_CARRIER_FIELD_ARG,
               RFORM_CARRIER_READONLY, RFORM_GLOBAL_RETARGET, RFORM_LOCAL_RETARGET,
               RFORM_RETARGET_ARG, RFORM_RETARGET_COPY_ARG, RFORM_RETARGET_COPY_DEREF,
               RFORM_CARRIER_TWO_LIT, RFORM_CARRIER_TWO_ASSIGN,
               RFORM_GLOBAL_CARRIER_LIT,
               RFORM_COUNT } RForm;
/* BUG-1043: the RMW grid has THREE sites, not two. The spawn scan and the ISR
 * walker are exhaustive descents of the body that performs the RMW; the MAIN
 * side is different code — plain main-line code calling a helper, answered by
 * the per-function summary func_rmw_param_mask (BUG-801) — and that summary was
 * a partial if-chain. Six POSITIONS of `*p += 1` inside the helper were invisible
 * to it and only to it, so a grid with two sites could not have shown the hole:
 * the ISR and spawn cells for those forms pass on the pre-fix compiler. RSite is
 * separate from VSite so the volatile-width and static-local grids, whose
 * `else` branch means "ISR", are untouched. */
typedef enum { RSITE_SPAWN, RSITE_ISR, RSITE_MAIN, RSITE_COUNT } RSite;
static const char *rsite_name(RSite s) {
    switch (s) {
    case RSITE_SPAWN: return "spawn";
    case RSITE_ISR:   return "isr";
    case RSITE_MAIN:  return "main";
    case RSITE_COUNT: break;
    }
    return "?";
}
static const char *rform_name(RForm f) {
    switch (f) {
    case RFORM_NAMED_COMPOUND:  return "named g+=1";
    case RFORM_WRITTEN_OUT:     return "written g=g+1";
    case RFORM_GPTR_WRITTEN_OUT: return "written *gp=*gp+1";
    case RFORM_GPTR_COPY_WRITTEN_OUT: return "written p=gp;*p=*p+1";
    case RFORM_LOCAL_ALIAS:     return "local *p+=1";
    case RFORM_PTR_PARAM:       return "param *p+=1";
    case RFORM_PTR_PARAM_2HOP:  return "param 2-hop";
    case RFORM_GLOBAL_ALIAS:    return "global *gp+=1";
    case RFORM_SPLIT_STMT:      return "split t=g;g=t+1";
    case RFORM_SPLIT_2HOP:      return "split 2-hop";
    case RFORM_PARAM_SWITCH:    return "param in switch";
    case RFORM_PARAM_ONCE:      return "param in @once";
    case RFORM_PARAM_ORELSE:    return "param in orelse{}";
    case RFORM_PARAM_CALL_ARG:  return "param as call arg";
    case RFORM_PARAM_RETURN:    return "param in return";
    case RFORM_PARAM_IF_COND:   return "param in if cond";
    case RFORM_ALIAS_ARG:       return "alias q=&g;bump(q)";
    case RFORM_CARRIER:         return "carrier h.p=&g";
    case RFORM_CARRIER_LIT:     return "carrier {.p=&g}";
    case RFORM_FUNCPTR_LOCAL:   return "funcptr fp(&g)";
    case RFORM_FUNCPTR_GLOBAL:  return "funcptr gfp(&g)";
    case RFORM_FUNCPTR_FIELD:   return "funcptr o.cb(&g)";
    case RFORM_ALIAS_COPY:      return "alias copy r=q";
    case RFORM_CARRIER_COPY:    return "carrier copy k=h";
    case RFORM_CARRIER_FIELD_ARG: return "carrier field bump(h.p)";
    case RFORM_CARRIER_READONLY:return "carrier read-only";
    case RFORM_GLOBAL_RETARGET: return "global gp=&g elsewhere";
    case RFORM_LOCAL_RETARGET:  return "local p=&d; p=&g";
    case RFORM_RETARGET_ARG:    return "retargeted gp as arg";
    case RFORM_RETARGET_COPY_ARG:   return "r=gp copy as arg";
    case RFORM_RETARGET_COPY_DEREF: return "r=gp copy *r+=1";
    case RFORM_CARRIER_TWO_LIT:     return "carrier {.p=&d,.q=&g}";
    case RFORM_CARRIER_TWO_ASSIGN:  return "carrier h.p=&d;h.q=&g";
    case RFORM_GLOBAL_CARRIER_LIT:  return "global carrier {.p=&g}";
    case RFORM_COUNT: break;
    }
    return "?";
}
/* The RMW body, and any helper it needs, for one form. */
static void rform_parts(RForm f, const char **helper, const char **body) {
    switch (f) {
    case RFORM_NAMED_COMPOUND: *helper = "";                                    *body = "g += 1;";        break;
    case RFORM_WRITTEN_OUT:    *helper = "";                                    *body = "g = g + 1;";     break;
    /* BUG-1277: the written-out RMW THROUGH a global pointer — the write side
     * resolved `*gp` to g, the read side matched g only by name. */
    case RFORM_GPTR_WRITTEN_OUT: *helper = "volatile *u32 gp = &g;";           *body = "*gp = *gp + 1;"; break;
    case RFORM_GPTR_COPY_WRITTEN_OUT: *helper = "volatile *u32 gp = &g;";      *body = "volatile *u32 p = gp; *p = *p + 1;"; break;
    case RFORM_LOCAL_ALIAS:    *helper = "";                                    *body = "volatile *u32 p = &g; *p += 1;"; break;
    case RFORM_PTR_PARAM:      *helper = "void bump(volatile *u32 p){ *p += 1; }"; *body = "bump(&g);";   break;
    case RFORM_PTR_PARAM_2HOP: *helper = "void inner(volatile *u32 p){ *p += 1; }\nvoid mid(volatile *u32 p){ inner(p); }";
                                                                                 *body = "mid(&g);";      break;
    case RFORM_GLOBAL_ALIAS:   *helper = "volatile *u32 gp = &g;";              *body = "*gp += 1;";      break;
    /* BUG-1010: the SAME operation split over two statements. Every form above
     * is answerable within ONE assignment; these two are not, and were accepted
     * at both sinks while `g += 1` and `g = g + 1` were rejected. The 2-hop cell
     * pins the taint being TRANSITIVE — a local reading a local that read g. */
    case RFORM_SPLIT_STMT:     *helper = "";                                    *body = "u32 t = g; g = t + 1;"; break;
    case RFORM_SPLIT_2HOP:     *helper = "";                                    *body = "u32 t = g; u32 u = t; g = u + 1;"; break;
    /* BUG-1043: the SAME `*p += 1` through a pointer param, in six POSITIONS the
     * main-side summary walk never descended. Each was accepted at the main site
     * (a torn ISR/main update, silent on bare metal) while the plain
     * RFORM_PTR_PARAM body was rejected — the position axis, not the spelling one. */
    case RFORM_PARAM_SWITCH:   *helper = "void bump(volatile *u32 p, u32 k){ switch (k) { 0 => { *p += 1; } default => { } } }";
                                                                                 *body = "bump(&g, 0);";   break;
    case RFORM_PARAM_ONCE:     *helper = "void bump(volatile *u32 p){ @once { *p += 1; } }"; *body = "bump(&g);"; break;
    case RFORM_PARAM_ORELSE:   *helper = "?u32 mb(u32 x){ if (x > 0) { return x; } return null; }\n"
                                         "void bump(volatile *u32 p, u32 k){ u32 v = mb(k) orelse { *p += 1; return; }; }";
                                                                                 *body = "bump(&g, 0);";   break;
    case RFORM_PARAM_CALL_ARG: *helper = "void take(u32 x){ }\nvoid bump(volatile *u32 p){ take((*p += 1)); }";
                                                                                 *body = "bump(&g);";      break;
    case RFORM_PARAM_RETURN:   *helper = "u32 bump(volatile *u32 p){ return (*p += 1); }";
                                                                                 *body = "u32 r = bump(&g);"; break;
    case RFORM_PARAM_IF_COND:  *helper = "void bump(volatile *u32 p){ if ((*p += 1) > 3) { } }";
                                                                                 *body = "bump(&g);";      break;
    /* BUG-1046: the RMW REACH axis — the pointer to g arrives at the RMW through a
     * vehicle the resolvers did not follow. The alias-as-argument form was live at
     * the MAIN site only (the scans had an alias table, the main sink matched a
     * literal `&g`); the carrier and the three funcptr-callee forms were live at
     * ALL THREE sites. The funcptr forms are refused as "handed to a call the
     * analysis cannot see" — a may-RMW, worded as such, not a proven one. */
    case RFORM_ALIAS_ARG:      *helper = "void bump(volatile *u32 p){ *p += 1; }";
                                                                                 *body = "volatile *u32 q = &g; bump(q);"; break;
    case RFORM_CARRIER:        *helper = "struct H { volatile *u32 p; }\nvoid bump(H h){ *h.p += 1; }";
                                                                                 *body = "H h; h.p = &g; bump(h);"; break;
    case RFORM_CARRIER_LIT:    *helper = "struct H { volatile *u32 p; }\nvoid bump(H h){ *h.p += 1; }";
                                                                                 *body = "H h = { .p = &g }; bump(h);"; break;
    case RFORM_FUNCPTR_LOCAL:  *helper = "void bump(volatile *u32 p){ *p += 1; }";
                                                                                 *body = "*(volatile *u32) fp = bump; fp(&g);"; break;
    case RFORM_FUNCPTR_GLOBAL: *helper = "void bump(volatile *u32 p){ *p += 1; }\n*(volatile *u32) gfp = bump;";
                                                                                 *body = "gfp(&g);"; break;
    case RFORM_FUNCPTR_FIELD:  *helper = "struct Ops { *(volatile *u32) cb; }\nvoid bump(volatile *u32 p){ *p += 1; }";
                                                                                 *body = "Ops o; o.cb = bump; o.cb(&g);"; break;
    /* The fourth vehicle: a COPY of a pointer / carrier that already designates g. */
    case RFORM_ALIAS_COPY:     *helper = "void bump(volatile *u32 p){ *p += 1; }";
                                                                                 *body = "volatile *u32 q = &g; volatile *u32 r = q; bump(r);"; break;
    case RFORM_CARRIER_COPY:   *helper = "struct H { volatile *u32 p; }\nvoid bump(H h){ *h.p += 1; }";
                                                                                 *body = "H h; h.p = &g; H k = h; bump(k);"; break;
    /* The fifth vehicle: the carrier's FIELD handed directly. */
    case RFORM_CARRIER_FIELD_ARG: *helper = "struct H { volatile *u32 p; }\nvoid bump(volatile *u32 p){ *p += 1; }";
                                                                                 *body = "H h; h.p = &g; bump(h.p);"; break;
    /* The boundary pin for the carrier binding: a helper that only READS through
     * the carrier has no RMW bit, so the binding alone must not reject. */
    case RFORM_CARRIER_READONLY: *helper = "struct H { volatile *u32 p; }\nu32 rd(H h){ return *h.p; }";
                                                                                 *body = "H h; h.p = &g; u32 v = rd(h); if (v > 100) { g = 1; }"; break;
    /* BUG-1124: a pointer RETARGETED after its declaration. The resolver followed
     * the initializer (`&d`) alone, so the write landing on `g` was invisible at
     * the ISR and MAIN sites (`d` is touched from one side only, so those cells
     * can only fire through the reassignment). At the SPAWN site the cell also
     * fires on `d` — any volatile RMW from a thread races — so it does not
     * discriminate there. */
    case RFORM_GLOBAL_RETARGET: *helper = "volatile u32 d;\nvolatile *u32 gp = &d;\nvoid aim(){ gp = &g; }";
                                                                                 *body = "aim(); *gp += 1;"; break;
    case RFORM_LOCAL_RETARGET:  *helper = "volatile u32 d;";
                                                                                 *body = "volatile *u32 p = &d; p = &g; *p += 1;"; break;
    case RFORM_RETARGET_ARG:    *helper = "volatile u32 d;\nvolatile *u32 gp = &d;\nvoid aim(){ gp = &g; }\n"
                                          "void bump(volatile *u32 p){ *p += 1; }";
                                                                                 *body = "aim(); bump(gp);"; break;
    /* A local COPY of the retargeted pointer aims wherever the original does. */
    case RFORM_RETARGET_COPY_ARG:   *helper = "volatile u32 d;\nvolatile *u32 gp = &d;\nvoid aim(){ gp = &g; }\n"
                                              "void bump(volatile *u32 p){ *p += 1; }";
                                                                                 *body = "aim(); volatile *u32 r = gp; bump(r);"; break;
    case RFORM_RETARGET_COPY_DEREF: *helper = "volatile u32 d;\nvolatile *u32 gp = &d;\nvoid aim(){ gp = &g; }";
                                                                                 *body = "aim(); volatile *u32 r = gp; *r += 1;"; break;
    /* BUG-1129: a carrier holding TWO globals bound only the first (the literal)
     * or only the LAST (two field assignments replaced the row). The RMW is
     * through the field aimed at `g`. */
    case RFORM_CARRIER_TWO_LIT:     *helper = "volatile u32 d;\nstruct H2 { volatile *u32 p; volatile *u32 q; }\n"
                                              "void bump2(H2 h){ *h.q += 1; }";
                                                                                 *body = "H2 h = { .p = &d, .q = &g }; bump2(h);"; break;
    case RFORM_CARRIER_TWO_ASSIGN:  *helper = "volatile u32 d;\nstruct H2 { volatile *u32 p; volatile *u32 q; }\n"
                                              "void bump2(H2 h){ *h.p += 1; }";
                                                                                 *body = "H2 h; h.p = &g; h.q = &d; bump2(h);"; break;
    /* BUG-1201: a GLOBAL carrier whose pointer comes from its struct-literal
     * initializer. The write-target walk followed `&x`, a pointer name, a slice and
     * `orelse` as an assigned value, never a struct literal. */
    case RFORM_GLOBAL_CARRIER_LIT:  *helper = "struct GH { volatile *u32 p; }\nGH gh = { .p = &g };";
                                                                                 *body = "*gh.p += 1;"; break;
    case RFORM_COUNT:          *helper = ""; *body = ""; break;
    }
}
static void gen_rmw(RSite site, RForm f, char *out, size_t n) {
    const char *helper; const char *body;
    rform_parts(f, &helper, &body);
    switch (site) {
    case RSITE_SPAWN:
        snprintf(out, n, "volatile u32 g;\n%s\nvoid w(){ %s }\n"
                         "u32 main(){ spawn w(); u32 x = g; return x & 1; }\n", helper, body);
        break;
    case RSITE_ISR:
        snprintf(out, n, "volatile u32 g;\n%s\ninterrupt TIMER { %s }\n"
                         "u32 main(){ u32 x = g; return x & 1; }\n", helper, body);
        break;
    /* BUG-1043: the RMW is in MAIN-line code (a helper main calls) and the ISR
     * does a plain write — the BUG-801 shape, answered by the per-function
     * summary rather than by either body scan. */
    case RSITE_MAIN:
        snprintf(out, n, "volatile u32 g;\n%s\nvoid w(){ %s }\n"
                         "interrupt TIMER { g = 1; }\n"
                         "u32 main(){ w(); return 0; }\n", helper, body);
        break;
    case RSITE_COUNT: out[0] = 0; break;
    }
}

static void gen_vol(VSite site, VShape shape, char *out, size_t n) {
    const char *decl;
    const char *wr;
    const char *rd;
    switch (shape) {
    case VSHAPE_WORD:      decl = "volatile u32 g;";                      wr = "g = 1;";   rd = "u32 x = g;";   break;
    case VSHAPE_OVERWIDTH: decl = "volatile u64 g;";                      wr = "g = 1;";   rd = "u64 x = g;";   break;
    case VSHAPE_AGGREGATE: decl = "struct P{u32 a; u32 b;}\nvolatile P g;"; wr = "g.a = 1;"; rd = "u32 x = g.a;"; break;
    case VSHAPE_OPTPTR:    decl = "volatile ?volatile *u32 g = null;";    wr = "g = null;"; rd = "volatile ?volatile *u32 x = g;"; break;
    case VSHAPE_OPTPTR_PLAIN: decl = "volatile ?*u32 g = null;";          wr = "g = null;"; rd = "volatile ?*u32 x = g;"; break;
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

/* ================================================================
 * STATIC-LOCAL GRID (BUG-971) — SITE x SHAPE, the same mirrored-sink shape.
 *
 * A `static u32 c = 0;` inside a function is ONE object for every thread that runs
 * it and for main-vs-ISR alike — the emitted C says so (`static uint32_t c = 0;` in
 * the function). But it has no global-scope Symbol, and BOTH race scans resolved
 * names with `scope_lookup(c->global_scope, …)`, so NEITHER saw it:
 *
 *     void w() { static u32 c = 0; c += 1; }
 *     u32 main() { spawn w(); spawn w(); return 0; }     // compiled clean
 *
 * Crossed with SITE for exactly the reason the volatile grid above is: the two
 * sinks are independent code, they had the identical defect, and a fix applied to
 * one and forgotten at the other is the recurring failure. Disagreement is a
 * failure here, not just a miss.
 *
 * The POSITIVE shapes are the same exemptions a global gets — `const` is immutable
 * and a single-word `volatile` is the established flag idiom — so the widening
 * cannot quietly reject ordinary code.
 * ================================================================ */
typedef enum { SLSHAPE_PLAIN, SLSHAPE_CONST, SLSHAPE_VOLATILE, SLSHAPE_COUNT } SLShape;
static const char *slshape_name(SLShape s) {
    switch (s) {
    case SLSHAPE_PLAIN:    return "plain-static";
    case SLSHAPE_CONST:    return "static-const";
    case SLSHAPE_VOLATILE: return "static-volatile-word";
    case SLSHAPE_COUNT: break;
    }
    return "?";
}
/* Only the plain one is a hazard; the two exemptions must keep compiling. */
static int slshape_is_negative(SLShape s) { return s == SLSHAPE_PLAIN; }
static void gen_sl(VSite site, SLShape shape, char *out, size_t n) {
    const char *decl;
    const char *use;
    switch (shape) {
    case SLSHAPE_PLAIN:    decl = "static u32 c = 0;";          use = "c = c + 1;";   break;
    case SLSHAPE_CONST:    decl = "static const u32 c = 4;";    use = "u32 z = c;";   break;
    case SLSHAPE_VOLATILE: decl = "static volatile u32 c = 0;"; use = "c = 1;";       break;
    case SLSHAPE_COUNT:    decl = ""; use = ""; break;
    }
    if (site == VSITE_SPAWN)
        snprintf(out, n,
            "void w(){ %s %s }\nu32 main(){ spawn w(); spawn w(); return 0; }\n",
            decl, use);
    else
        /* helper() is reached from BOTH the ISR and main, which is what makes the
         * static SHARED — the same from_isr && from_func pairing a global needs. */
        snprintf(out, n,
            "void helper(){ %s %s }\ninterrupt TIMER { helper(); }\n"
            "u32 main(){ helper(); return 0; }\n",
            decl, use);
}

/* run_neg/run_pos with per-cell zerc flags (the over-width cell needs a 32-bit
 * target). Same EMIT-ONLY contract and integrity guard as the helpers above. */
static int run_vol(const char *name, const char *code, const char *flags, int negative) {
    total++;
    FILE *f = fopen(ZT("/tmp/_zer_hw.zer"), "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[640];
    snprintf(cmd, sizeof(cmd), ZT("%s /tmp/_zer_hw.zer %s -o /tmp/_zer_hw.c 2>/tmp/_zer_hw.err"),
             zerc_path, flags);
    int rc = system(cmd);
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen(ZT("/tmp/_zer_hw.err"), "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (!negative) {
        if (rc == 0) { passed++; return 1; }
        failed++; over_reject++;
        fprintf(stderr, "  FAIL [OVER-REJECT] %s — the sanctioned single-word idiom was REJECTED:\n    %.140s\n", name, eb);
        return 0;
    }
    if (rc == 0) {
        failed++; false_neg++;
        /* BUG-971: run_vol now serves TWO grids (volatile width, and static locals),
         * so the wording cannot name one of them — a failure message that describes
         * the wrong defect sends the reader after the wrong bug. The cell NAME
         * ("vol/..." or "sl/...") and the printed program say which. */
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — an UNSYNCHRONISED SHARED ACCESS COMPILED CLEAN\n", name);
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

    /* ---- static-local grid: SITE x SHAPE (BUG-971, both sinks must agree) ---- */
    fprintf(stderr, "\n  -- static-local grid (spawn vs ISR must agree) --\n");
    for (VSite vs = 0; vs < VSITE_COUNT; vs++) {
        for (SLShape sp = 0; sp < SLSHAPE_COUNT; sp++) {
            valid_cells++;
            int neg = slshape_is_negative(sp);
            char nm[192];
            snprintf(nm, sizeof(nm), "sl/%s/%s", vsite_name(vs), slshape_name(sp));
            gen_sl(vs, sp, vbuf, sizeof(vbuf));
            int ok = run_vol(nm, vbuf, "", neg);
            fprintf(stderr, "  [%-5s][%-20s][%-3s] %s\n",
                    vsite_name(vs), slshape_name(sp), neg ? "neg" : "pos",
                    ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }

    /* RMW FORM grid (BUG-792) — every cell negative; all THREE sinks must agree
     * (BUG-1043 added the main-side summary as a site of its own). */
    fprintf(stderr, "\n--- RMW form grid (site x spelling) ---\n");
    for (RSite vs = 0; vs < RSITE_COUNT; vs++) {
        for (RForm rf = 0; rf < RFORM_COUNT; rf++) {
            valid_cells++;
            char rbuf[1024], rnm[192];
            snprintf(rnm, sizeof(rnm), "rmw/%s/%s", rsite_name(vs), rform_name(rf));
            gen_rmw(vs, rf, rbuf, sizeof(rbuf));
            /* The ONE positive cell, and it is the grid's boundary pin: at the
             * SPAWN sink a `@once` body is real synchronisation — it runs exactly
             * once program-wide and every later arrival waits for its release
             * publish (B4, once_loser_wait.zer) — so the RMW inside it has ONE
             * writer and main's single-word volatile read is the sanctioned flag
             * idiom. scan_unsafe_global_access keeps @once a leaf for that
             * reason, and this cell fails if someone "fixes" that. At the ISR
             * and MAIN sites the same body is a race: an interrupt can land
             * inside the once-body's read-modify-write (ISR site), or the ISR's
             * own store can (MAIN site), and @once orders nothing against an
             * interrupt. */
            int neg = !(vs == RSITE_SPAWN && rf == RFORM_PARAM_ONCE) &&
                      rf != RFORM_CARRIER_READONLY;   /* BUG-1046 boundary, every site */
            int ok = run_vol(rnm, rbuf, "", neg);
            fprintf(stderr, "  [%-5s][%-24s][%s] %s\n",
                    rsite_name(vs), rform_name(rf), neg ? "neg" : "pos",
                    ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }

    /* BUG-1010 BOUNDARY — the cross-statement taint must fire ONLY when the value
     * written back really came from the same global. These three are safe code
     * and an over-rejection here is a regression, so they are POSITIVE cells in
     * a grid that is otherwise all negative. Without them the cheapest way to
     * pass the two SPLIT cells above would be "any function that reads g and
     * writes g", which rejects a great deal of correct firmware. */
    fprintf(stderr, "\n--- RMW split-taint boundary (must COMPILE) ---\n");
    {
        static const char *bnames[4] = { "taint-cleared", "other-global", "not-shared",
                                         "retarget-plain-store" };
        static const char *bbodies[4] = {
            "volatile u32 g;\ninterrupt TIMER { g = 7; }\n"
            "u32 main(){ u32 t = g; t = 5; g = t; return g & 1; }\n",
            "volatile u32 g;\nvolatile u32 h;\ninterrupt TIMER { g = 7; }\n"
            "u32 main(){ u32 t = h; g = t + 1; return g & 1; }\n",
            "u32 plain;\nu32 main(){ u32 t = plain; plain = t + 1; return plain & 1; }\n",
            /* BUG-1124 boundary: retargeting a pointer (`gp = &h`) writes the
             * POINTER, not h — it must not read as an RMW of the new target. The
             * first draft of the fix rejected exactly this. */
            "volatile u32 g;\nvolatile u32 h;\nvolatile *u32 gp = &g;\n"
            "interrupt TIMER { *gp = 5; }\nvoid aim(){ gp = &h; }\n"
            "u32 main(){ aim(); u32 t = h; return t & 1; }\n"
        };
        for (int bi = 0; bi < 4; bi++) {
            valid_cells++;
            char bnm[192];
            snprintf(bnm, sizeof(bnm), "rmw-boundary/%s", bnames[bi]);
            int ok = run_vol(bnm, bbodies[bi], "", 0);
            fprintf(stderr, "  [%-22s][pos] %s\n", bnames[bi], ok ? "ok" : "*** FAIL ***");
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
