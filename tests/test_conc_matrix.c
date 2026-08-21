/* test_conc_matrix.c — data-race / spawn / deadlock soundness oracle
 * (2026-06-07). First oracle for the NON-memory frontier (limitations.md
 * "next frontier" entry, Domain 1).
 *
 * The shape/escape/keep/cflow matrices guard memory safety. This guards the
 * concurrency accept/reject rules: spawn pointer-safety, spawned-body global
 * access, deadlock lock-ordering, spawn-in-@critical, and ThreadHandle join.
 *
 * TWO cell kinds:
 *   - NEGATIVE: a data race / deadlock / unjoined-thread MUST be rejected for a
 *     concurrency reason. Compiling clean = false negative (the unacceptable
 *     class — an unsynchronized program the analyzer let through).
 *   - POSITIVE: a correctly-synchronized pattern (shared struct auto-lock,
 *     scoped spawn + join, value args, separate-statement shared access) MUST
 *     compile. Rejection = over-rejection (acceptable, but logged).
 *
 * INTEGRITY GUARD: a NEG rejection only counts if the error names a concurrency
 * reason (data race / deadlock / non-shared / not joined / @critical / spawn /
 * thread). A parse/type error masking the check is flagged INVALID.
 *
 * Axes are a C enum switched with NO default — adding a COScenario fails GCC
 * -Wswitch until handled. Concurrency has no pool/slab type axis, so this is a
 * flat scenario list (like the curated control-flow scenarios).
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

static int has_conc_reason(const char *eb) {
    return strstr(eb, "data race") || strstr(eb, "deadlock") ||
           strstr(eb, "non-shared") || strstr(eb, "not joined") ||
           strstr(eb, "@critical") || strstr(eb, "spawn") ||
           strstr(eb, "thread") || strstr(eb, "shared") ||
           /* 2026-08-03: a spawn argument marked TRANSFERRED is the
            * concurrency ownership mechanism (ir_mark_transferred at the
            * spawn-arg sink), so "is transferred" IS a concurrency reason.
            * Without this the carrier grid reported correct rejections as
            * SUSPECT. Deliberately NOT matching bare "use after free", which
            * would let an unrelated single-threaded UAF pass as coverage. */
           strstr(eb, "is transferred") ||
           /* 2026-08-10: the ISR sink's diagnostic. A global reached from BOTH an
            * interrupt handler and main code is the ISR-side data race, and its
            * message is "accessed from both interrupt and main code — must be
            * declared volatile". Without this the ISR reach grid reported nine
            * correct rejections as SUSPECT. Deliberately matching the SPECIFIC
            * phrase, not bare "interrupt": a loose substring matches the unrelated
            * ISR-SIGNATURE error, which is exactly how a pre-fix build masked
            * these tests when they were first written. */
           strstr(eb, "accessed from both interrupt");
}

/* NEGATIVE: must reject for a concurrency reason. */
static int run_neg(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_co.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_co.zer -o /dev/null 2>/tmp/_zer_co.err", zerc_path);
    if (system(cmd) == 0) {
        failed++; false_neg++;
        fprintf(stderr, "  FAIL [FALSE-NEGATIVE] %s — unsynchronized program COMPILED CLEAN\n", name);
        fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        return 0;
    }
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_co.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    if (strstr(eb, "expected ") || strstr(eb, "unexpected") || strstr(eb, "parse error")) {
        failed++; invalid_probe++;
        fprintf(stderr, "  FAIL [INVALID-PROBE] %s — rejected by parse error, not a concurrency check\n", name);
        fprintf(stderr, "    %.110s\n", eb);
        return 0;
    }
    if (has_conc_reason(eb)) { passed++; return 1; }
    failed++;
    fprintf(stderr, "  FAIL [SUSPECT] %s — rejected, but not for a concurrency reason:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    return 0;
}

/* POSITIVE: a correctly-synchronized pattern must compile. */
static int run_pos(const char *name, const char *code) {
    total++;
    FILE *f = fopen("/tmp/_zer_co.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return 0; }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_co.zer -o /dev/null 2>/tmp/_zer_co.err", zerc_path);
    if (system(cmd) == 0) { passed++; return 1; }
    failed++; over_reject++;
    char eb[4096]; eb[0] = 0;
    FILE *e = fopen("/tmp/_zer_co.err", "r");
    if (e) { size_t r = fread(eb, 1, sizeof(eb) - 1, e); eb[r] = 0; fclose(e); }
    fprintf(stderr, "  FAIL [OVER-REJECT] %s — safe concurrency pattern REJECTED:\n", name);
    fprintf(stderr, "    %.110s\n", eb);
    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
    return 0;
}

typedef enum {
    /* NEGATIVE (unsynchronized — must reject) */
    CO_SPAWN_NONSHARED_PTR,    /* spawn f(&local), f takes non-shared *T */
    CO_SPAWN_NONSHARED_GLOBAL, /* spawned body reads/writes a non-shared global */
    CO_DEADLOCK_SAME_STMT,     /* a.x = b.y — two shared types in one statement */
    CO_SPAWN_IN_CRITICAL,      /* spawn inside @critical (interrupts disabled) */
    CO_THREADHANDLE_NOT_JOINED,/* scoped spawn ThreadHandle never joined */
    CO_SPAWN_TRANSITIVE_GLOBAL,/* spawn -> helper() -> non-shared global (transitive) */
    CO_SPAWN_SLAB,             /* spawned body uses a global Slab (non-atomic metadata) */
    CO_THREADHANDLE_JOIN_ONE_BRANCH, /* join only in one branch — unjoined on the other */
    /* POSITIVE (synchronized — must compile) */
    CO_SPAWN_SHARED_OK,        /* spawn f(&g), g is a shared struct (auto-locked) */
    CO_SCOPED_SPAWN_JOIN,      /* ThreadHandle th = spawn f(&w); th.join(); */
    CO_SPAWN_VALUE_ARGS,       /* spawn f(42) — value args copied, no sharing */
    CO_DEADLOCK_SEPARATE_OK,   /* a.x=1; b.y=2; — separate statements, no nesting */
    CO_SHARED_FIELD_OK,        /* shared struct field access (auto-lock) */
    CO_THREADHANDLE_JOIN_BOTH, /* join in both branches — joined on all paths */
    CO_SPAWN_THREADLOCAL,      /* spawned body touches a threadlocal global (per-thread) */
    COSCEN_COUNT
} COScenario;

static int scenario_is_negative(COScenario s) {
    switch (s) {
        case CO_SPAWN_NONSHARED_PTR: case CO_SPAWN_NONSHARED_GLOBAL:
        case CO_DEADLOCK_SAME_STMT: case CO_SPAWN_IN_CRITICAL:
        case CO_THREADHANDLE_NOT_JOINED: case CO_SPAWN_TRANSITIVE_GLOBAL:
        case CO_SPAWN_SLAB: case CO_THREADHANDLE_JOIN_ONE_BRANCH:
            return 1;
        case CO_SPAWN_SHARED_OK: case CO_SCOPED_SPAWN_JOIN:
        case CO_SPAWN_VALUE_ARGS: case CO_DEADLOCK_SEPARATE_OK:
        case CO_SHARED_FIELD_OK: case CO_THREADHANDLE_JOIN_BOTH:
        case CO_SPAWN_THREADLOCAL:
            return 0;
        case COSCEN_COUNT: break;
    }
    return 1;
}

static const char *scen_name(COScenario s) {
    switch (s) {
        case CO_SPAWN_NONSHARED_PTR:     return "spawn-nonshared-ptr";
        case CO_SPAWN_NONSHARED_GLOBAL:  return "spawn-nonshared-global";
        case CO_DEADLOCK_SAME_STMT:      return "deadlock-same-stmt";
        case CO_SPAWN_IN_CRITICAL:       return "spawn-in-critical";
        case CO_THREADHANDLE_NOT_JOINED: return "threadhandle-not-joined";
        case CO_SPAWN_TRANSITIVE_GLOBAL: return "spawn-transitive-global";
        case CO_SPAWN_SLAB:              return "spawn-slab-access";
        case CO_THREADHANDLE_JOIN_ONE_BRANCH: return "join-one-branch";
        case CO_SPAWN_SHARED_OK:         return "spawn-shared-ok";
        case CO_SCOPED_SPAWN_JOIN:       return "scoped-spawn-join";
        case CO_SPAWN_VALUE_ARGS:        return "spawn-value-args";
        case CO_DEADLOCK_SEPARATE_OK:    return "deadlock-separate-ok";
        case CO_SHARED_FIELD_OK:         return "shared-field-ok";
        case CO_THREADHANDLE_JOIN_BOTH:  return "join-both-branches";
        case CO_SPAWN_THREADLOCAL:       return "spawn-threadlocal";
        case COSCEN_COUNT: break;
    }
    return "?";
}

static void gen(COScenario s, char *buf, size_t n) {
    switch (s) {
        case CO_SPAWN_NONSHARED_PTR:
            snprintf(buf, n,
                "struct Work { u32 x; }\n"
                "void worker(*Work w) { w.x = 1; }\n"
                "u32 main() { Work local; spawn worker(&local); return 0; }\n");
            break;
        case CO_SPAWN_NONSHARED_GLOBAL:
            snprintf(buf, n,
                "u32 g_ctr;\n"
                "void worker() { g_ctr = g_ctr + 1; }\n"
                "u32 main() { spawn worker(); return 0; }\n");
            break;
        case CO_DEADLOCK_SAME_STMT:
            snprintf(buf, n,
                "shared struct Alpha { u32 x; }\n"
                "shared struct Beta { u32 y; }\n"
                "Alpha ga;\nBeta gb;\n"
                "u32 main() { ga.x = gb.y; return 0; }\n");
            break;
        case CO_SPAWN_IN_CRITICAL:
            snprintf(buf, n,
                "shared struct C { u32 v; }\n"
                "C g;\n"
                "void worker(*C c) { c.v = 1; }\n"
                "u32 main() { @critical { spawn worker(&g); } return 0; }\n");
            break;
        case CO_THREADHANDLE_NOT_JOINED:
            snprintf(buf, n,
                "struct W { u32 x; }\n"
                "void compute(*W w) { w.x = 1; }\n"
                "u32 main() { W work; ThreadHandle th = spawn compute(&work); return 0; }\n");
            break;
        case CO_SPAWN_SHARED_OK:
            snprintf(buf, n,
                "shared struct C { u32 v; }\n"
                "C g;\n"
                "void worker(*C c) { c.v = 1; }\n"
                "u32 main() { spawn worker(&g); return 0; }\n");
            break;
        case CO_SCOPED_SPAWN_JOIN:
            snprintf(buf, n,
                "struct W { u32 x; }\n"
                "void compute(*W w) { w.x = 1; }\n"
                "u32 main() { W work; ThreadHandle th = spawn compute(&work); th.join(); return 0; }\n");
            break;
        case CO_SPAWN_VALUE_ARGS:
            snprintf(buf, n,
                "void handler(u32 ev) { u32 x = ev; }\n"
                "u32 main() { spawn handler(42); return 0; }\n");
            break;
        case CO_DEADLOCK_SEPARATE_OK:
            snprintf(buf, n,
                "shared struct Alpha { u32 x; }\n"
                "shared struct Beta { u32 y; }\n"
                "Alpha ga;\nBeta gb;\n"
                "u32 main() { ga.x = 10; gb.y = 20; return 0; }\n");
            break;
        case CO_SHARED_FIELD_OK:
            snprintf(buf, n,
                "shared struct C { u32 v; }\n"
                "C g;\n"
                "u32 main() { g.v = 42; u32 x = g.v; return x - 42; }\n");
            break;
        case CO_SPAWN_TRANSITIVE_GLOBAL:
            snprintf(buf, n,
                "u32 g_ctr;\n"
                "void helper() { g_ctr = g_ctr + 1; }\n"
                "void worker() { helper(); }\n"
                "u32 main() { spawn worker(); return 0; }\n");
            break;
        case CO_SPAWN_SLAB:
            snprintf(buf, n,
                "struct Task { u32 id; }\n"
                "Slab(Task) gs;\n"
                "void worker() { Handle(Task) h = gs.alloc() orelse return; gs.free(h); }\n"
                "u32 main() { spawn worker(); return 0; }\n");
            break;
        case CO_THREADHANDLE_JOIN_ONE_BRANCH:
            snprintf(buf, n,
                "struct W { u32 x; }\n"
                "void compute(*W w) { w.x = 1; }\n"
                "u32 main() { W work; u32 c = 0; ThreadHandle th = spawn compute(&work);\n"
                "    if (c > 0) { th.join(); }\n    return 0; }\n");
            break;
        case CO_THREADHANDLE_JOIN_BOTH:
            snprintf(buf, n,
                "struct W { u32 x; }\n"
                "void compute(*W w) { w.x = 1; }\n"
                "u32 main() { W work; u32 c = 0; ThreadHandle th = spawn compute(&work);\n"
                "    if (c > 0) { th.join(); } else { th.join(); }\n    return 0; }\n");
            break;
        case CO_SPAWN_THREADLOCAL:
            snprintf(buf, n,
                "threadlocal u32 g_tl;\n"
                "void worker() { g_tl = g_tl + 1; }\n"
                "u32 main() { spawn worker(); return 0; }\n");
            break;
        case COSCEN_COUNT: buf[0] = 0; break;
    }
}


/* ================================================================
 * CARRIER GRID (2026-08-03) — the "wrapper hides the inner kind" class
 * at the spawn ARGUMENT sink.
 *
 * WHY THIS EXISTS. The scenario list above is FLAT, and every one of its
 * spawn cells hands the payload BARE. That is exactly the shape the gate
 * already tested, so the grid was green while six real holes were open:
 * wrapping the same Handle / heap pointer / slice in a by-value struct
 * slipped every arm of the spawn-arg gate. Measured before the fix — all
 * ACCEPTED, and the emitted worker really derefed across the thread
 * boundary (`_zer_pool_get(..., b.h, 4)->v`) while the parent freed.
 *
 * PRIMITIVES FRAMING (docs/primitives-data-races.md §7.2, §13.1). Share(P),
 * the set of memory two contexts can reach, is bounded to `shared struct`,
 * `*shared T`, globals-via-spawn-scanner, and threadlocal escape. A payload
 * copied into the child inside a by-value struct is in NONE of those, so it
 * is sharing OUTSIDE the sanctioned set. The closure argument says such a
 * program must be rejected; this grid is what keeps that true as new carrier
 * shapes are added.
 *
 * THE AXES. carrier x payload x sink. The carrier axis is the bug class: a
 * green BARE row proves nothing about the wrapped rows, which is precisely
 * how this shipped. Cells are C enums switched with NO default, so adding a
 * carrier or payload fails -Wswitch until every combination is handled.
 *
 * EVERY cell is a genuine hazard, so every cell must REJECT for a
 * concurrency reason. run_neg's integrity guard rejects a cell that was only
 * stopped by a parse/type error, so a mis-typed probe cannot pass as
 * coverage (the vacuous-test class, CLAUDE.md).
 * ================================================================ */

/* CAR_OPT_BYVAL added 2026-08-06: an OPTIONAL wrapping a by-value carrier
 * struct. The grid had CAR_OPT (optional over a bare payload) and CAR_BYVAL
 * (struct over a bare payload) but never their COMPOSITION, and that is exactly
 * where the gate broke — a raw `eff->kind == STRUCT` pre-guard skipped the
 * carrier predicate for anything optional-wrapped, and the `?Handle` arm
 * swallowed every other optional before it could reach the recursive check.
 * Two axes each covered individually is not the same as their product. */
typedef enum { CAR_BARE, CAR_BYVAL, CAR_NESTED, CAR_OPT, CAR_OPT_BYVAL, CAR_COUNT } CACarrier;
typedef enum { PAY_HANDLE, PAY_PTR, PAY_SLICE, PAY_COUNT } CAPayload;
typedef enum { SINK_FF, SINK_SCOPED_FREE_B4_JOIN, SINK_COUNT } CASink;

static const char *car_name(CACarrier c) {
    switch (c) {
    case CAR_BARE:   return "bare";
    case CAR_BYVAL:  return "byval-struct";
    case CAR_NESTED: return "nested-struct";
    case CAR_OPT:    return "optional";
    case CAR_OPT_BYVAL: return "optional-of-struct";
    case CAR_COUNT:  break;
    }
    return "?";
}
static const char *pay_name(CAPayload p) {
    switch (p) {
    case PAY_HANDLE: return "Handle";
    case PAY_PTR:    return "heap-ptr";
    case PAY_SLICE:  return "slice";
    case PAY_COUNT:  break;
    }
    return "?";
}
static const char *sink_name(CASink s) {
    switch (s) {
    case SINK_FF:                   return "fire-and-forget";
    case SINK_SCOPED_FREE_B4_JOIN:  return "scoped/free-before-join";
    case SINK_COUNT: break;
    }
    return "?";
}

/* ================================================================
 * FUNCPTR-REACH GRID (2026-08-08)
 *
 * ONE question — "does the callback this spawn target invokes touch a
 * non-shared global?" — answered independently at SIX syntactic REACH forms.
 * It was patched FOUR TIMES, one form per session, each time with the other
 * forms unexamined:
 *
 *   direct name      `*() fp = bump; fp();`             long-rejected
 *   struct FIELD     `o.cb = bump; o.cb();`             5ed17c2f
 *   LOCAL arg        `*() fp = bump; spawn w(fp);`      00dc785a
 *   factory 1-hop    `*() fp = get_fp(); fp();`         2026-08-08
 *   factory 2-hop    `get_a(){ return get_b(); }`       2026-08-08 (found BY
 *                                                       enumerating for this
 *                                                       grid, not reported)
 *
 * That last one is the argument for the grid existing: four sequential fixes
 * had not exhausted the class, and the fifth hole surfaced only when the forms
 * were written down as an axis. Crossing REACH with PAYLOAD also pins the
 * over-rejection boundary — the scan reaches MORE callbacks now, so a callback
 * that is genuinely safe (threadlocal / @atomic_* / touches nothing) must keep
 * compiling.
 *
 * A new reach form with no cell here is INVISIBLE. Add the cell in the same
 * commit as the form. */
typedef enum { RCH_DIRECT, RCH_REASSIGN, RCH_FIELD, RCH_ARRAY,
               RCH_FACTORY1, RCH_FACTORY2, RCH_FIELD_ARRAY, RCH_FWD_PARAM, RCH_COUNT } CAReach;
typedef enum { RPAY_RACY, RPAY_TLS, RPAY_ATOMIC, RPAY_NONE, RPAY_COUNT } CARPay;

static const char *reach_name(CAReach r) {
    switch (r) {
    case RCH_DIRECT:   return "direct-name";
    case RCH_REASSIGN: return "reassigned-local";
    case RCH_FIELD:    return "struct-field";
    case RCH_ARRAY:    return "array-element";
    case RCH_FACTORY1: return "factory-1hop";
    case RCH_FACTORY2: return "factory-2hop";
    case RCH_FIELD_ARRAY: return "field-array-elem";
    case RCH_FWD_PARAM:   return "forwarded-param";
    case RCH_COUNT:    break;
    }
    return "?";
}
static const char *rpay_name(CARPay p) {
    switch (p) {
    case RPAY_RACY:   return "racy-global";
    case RPAY_TLS:    return "threadlocal";
    case RPAY_ATOMIC: return "atomic";
    case RPAY_NONE:   return "touches-nothing";
    case RPAY_COUNT:  break;
    }
    return "?";
}

/* Only the racy payload must be REJECTED; the other three pin over-rejection. */
static int reach_is_negative(CARPay p) { return p == RPAY_RACY; }

static void gen_reach(CAReach r, CARPay p, char *out, size_t n) {
    /* the callback body, per payload */
    const char *decls, *body;
    switch (p) {
    case RPAY_RACY:   decls = "u32 g;";            body = "g += 1;"; break;
    case RPAY_TLS:    decls = "threadlocal u32 g;";body = "g += 1;"; break;
    case RPAY_ATOMIC: decls = "u32 g;";            body = "u32 o = @atomic_add(&g, 1);"; break;
    case RPAY_NONE:   decls = "u32 g;";            body = "u32 z = 0; z += 1;"; break;
    default:          decls = "u32 g;";            body = "g += 1;"; break;
    }
    /* how `worker` reaches the callback */
    const char *extra = "", *wbody = "";
    switch (r) {
    case RCH_DIRECT:
        wbody = "*() fp = cb; fp();"; break;
    case RCH_REASSIGN:
        extra = "void other() { }\n";
        wbody = "*() fp = other; fp = cb; fp();"; break;
    case RCH_FIELD:
        extra = "struct Ops { *() h; }\n";
        wbody = "Ops o; o.h = cb; o.h();"; break;
    case RCH_ARRAY:
        extra = "typedef *() Cb;\n";
        wbody = "Cb[2] t; t[0] = cb; t[0]();"; break;
    case RCH_FACTORY1:
        extra = "*() mk() { return cb; }\n";
        wbody = "*() fp = mk(); fp();"; break;
    case RCH_FACTORY2:
        extra = "*() mk2() { return cb; }\n*() mk() { return mk2(); }\n";
        wbody = "*() fp = mk(); fp();"; break;
    /* 7th form (2026-08-10): a funcptr ARRAY ELEMENT of a struct FIELD. The tell
     * and the binding collector both matched only a bare NODE_FIELD, so the
     * INDEX-over-FIELD callee slipped the gate while `o.cb()` was rejected. */
    case RCH_FIELD_ARRAY:
        extra = "typedef *() -> void VoidFn;\nstruct Ops { VoidFn[2] fns; }\n";
        wbody = "Ops o; o.fns[0] = cb; o.fns[0]();"; break;
    /* 8th form (2026-08-10): the funcptr arrives as a PARAMETER of the spawning
     * function, supplied by ITS caller — no binding is visible inside the
     * spawner, so the answer needs a per-function forwarding summary. */
    case RCH_FWD_PARAM:
        extra = "void inner(*() -> void f) { f(); }\n";
        wbody = "spawn inner(cb);"; break;
    default: break;
    }
    snprintf(out, n,
        "%s\n"
        "void cb() { %s }\n"
        "%s"
        "void worker() { %s }\n"
        "u32 main() { spawn worker(); return 0; }\n",
        decls, body, extra, wbody);
}

/* `?[*]T` and `?Handle` are spellable; a nested optional is not, and the
 * optional carrier over a slice adds no distinct dispatch path. */
static int carrier_cell_valid(CACarrier c, CAPayload p) {
    if (c == CAR_OPT && p == PAY_SLICE) return 0;   /* ?[*]T adds no new dispatch path */
    (void)p;
    return 1;
}

/* Build one cell. The worker ALWAYS derefs the payload and the parent ALWAYS
 * frees it, so a passing cell reflects the gate, not an inert probe. */
static void gen_carrier(CACarrier c, CAPayload p, CASink k,
                        char *out, size_t n) {
    const char *fld;              /* payload field / arg type            */
    const char *alloc;            /* how the parent allocates it         */
    const char *freecall;         /* how the parent frees it             */
    switch (p) {
    case PAY_HANDLE: fld = "Handle(T)"; alloc = "Handle(T) src = pl.alloc() orelse { return 1; };"; freecall = "pl.free(src);"; break;
    case PAY_PTR:    fld = "*T";        alloc = "*T src = alloc(T) orelse { return 1; };";          freecall = "free(src);";    break;
    case PAY_SLICE:  fld = "[*]T";      alloc = "[*]T src = alloc(T,4) orelse { return 1; };";      freecall = "free(src);";    break;
    case PAY_COUNT:  fld = "*T"; alloc = ""; freecall = ""; break;
    }

    char decls[512]; decls[0] = 0;
    char argty[128], setup[256], acc[128];
    switch (c) {
    case CAR_BARE:
        snprintf(argty, sizeof(argty), "%s", fld);
        snprintf(setup, sizeof(setup), "%s a = src;", fld);
        snprintf(acc,   sizeof(acc),   "a");
        break;
    case CAR_BYVAL:
        snprintf(decls, sizeof(decls), "struct B{ %s q; }\n", fld);
        snprintf(argty, sizeof(argty), "B");
        snprintf(setup, sizeof(setup), "B a; a.q = src;");
        snprintf(acc,   sizeof(acc),   "a.q");
        break;
    case CAR_NESTED:
        snprintf(decls, sizeof(decls), "struct I{ %s q; }\nstruct O{ I i; }\n", fld);
        snprintf(argty, sizeof(argty), "O");
        snprintf(setup, sizeof(setup), "O a; a.i.q = src;");
        snprintf(acc,   sizeof(acc),   "a.i.q");
        break;
    case CAR_OPT:
        snprintf(argty, sizeof(argty), "?%s", fld);
        snprintf(setup, sizeof(setup), "?%s a; a = src;", fld);
        snprintf(acc,   sizeof(acc),   "a");
        break;
    case CAR_OPT_BYVAL:
        snprintf(decls, sizeof(decls), "struct B{ %s q; }\n", fld);
        snprintf(argty, sizeof(argty), "?B");
        snprintf(setup, sizeof(setup), "B bv; bv.q = src; ?B a; a = bv;");
        snprintf(acc,   sizeof(acc),   "u.q");
        break;
    case CAR_COUNT: argty[0]=0; setup[0]=0; acc[0]=0; break;
    }

    /* The worker body: always a real dereference of the payload. */
    char body[256];
    if (c == CAR_OPT_BYVAL) {
        /* unwrap the optional to a local, then reach the carried payload */
        if (p == PAY_SLICE)
            snprintf(body, sizeof(body), "B u = a orelse { return; }; u32 x = u.q[0].v;");
        else
            snprintf(body, sizeof(body), "B u = a orelse { return; }; u32 x = u.q.v;");
    } else if (c == CAR_OPT) {
        if (p == PAY_HANDLE)
            snprintf(body, sizeof(body), "Handle(T) u = %s orelse { return; }; u32 x = u.v;", acc);
        else
            snprintf(body, sizeof(body), "*T u = %s orelse { return; }; u32 x = u.v;", acc);
    } else if (p == PAY_SLICE) {
        snprintf(body, sizeof(body), "u32 x = %s[0].v;", acc);
    } else {
        snprintf(body, sizeof(body), "u32 x = %s.v;", acc);
    }

    const char *spawn_and_free =
        (k == SINK_FF) ? "spawn w(a);\n    %s\n"
                       : "ThreadHandle th = spawn w(a);\n    %s\n    th.join();\n";
    char tail[256];
    snprintf(tail, sizeof(tail), spawn_and_free, freecall);

    snprintf(out, n,
        "struct T{u32 v;}\n"
        "%s"
        "Pool(T,4) pl;\n"
        "void w(%s a){ %s }\n"
        "u32 main(){\n"
        "    %s\n"
        "    %s\n"
        "    %s"
        "    return 0;\n"
        "}\n",
        decls, argty, body, alloc, setup, tail);
}

/* ================================================================
 * ISR REACH sub-grid (2026-08-10)
 *
 * The SAME nine reach forms, at the INTERRUPT sink instead of the spawn sink.
 * This axis exists because the two sinks drifted: the spawn side was fixed
 * seven times while the ISR side lagged, and every time that asymmetry existed
 * it produced a bug (BUG-774 closed three ISR forms; the two factory forms were
 * still live afterwards and are closed here).
 *
 * NEGATIVE cells only. An `interrupt` block cannot appear in a RUNNABLE positive
 * — GCC refuses ISRs on hosted x86-64 ("SSE instructions aren't allowed in
 * interrupt service routine") — so the safe-payload columns have no home in a
 * run-based grid. The over-rejection boundary for the ISR sink is pinned instead
 * by tests/zer_fail + the compile-only controls recorded in BUGS-FIXED.md.
 *
 * A new reach form must get a cell HERE as well as in the spawn grid. */
typedef enum { IR_DIRECT, IR_GLOBAL_FP, IR_ARG, IR_STRUCT_INIT, IR_LOCAL_BIND,
               IR_FIELD_ASSIGN, IR_FIELD_ARRAY, IR_FACTORY1, IR_FACTORY2,
               IR_COUNT } IsrReach;

static const char *isr_name(IsrReach r) {
    switch (r) {
    case IR_DIRECT:       return "direct-call";
    case IR_GLOBAL_FP:    return "global-funcptr";
    case IR_ARG:          return "funcname-arg";
    case IR_STRUCT_INIT:  return "struct-init-field";
    case IR_LOCAL_BIND:   return "local-binding";
    case IR_FIELD_ASSIGN: return "field-assign";
    case IR_FIELD_ARRAY:  return "field-array-elem";
    case IR_FACTORY1:     return "factory-1hop";
    case IR_FACTORY2:     return "factory-2hop";
    case IR_COUNT:        break;
    }
    return "?";
}

static void gen_isr_reach(IsrReach r, char *out, size_t n) {
    const char *extra = "", *body = "";
    switch (r) {
    case IR_DIRECT:       body = "bump();"; break;
    case IR_GLOBAL_FP:    extra = "*() -> void gcb = bump;\n"; body = "gcb();"; break;
    case IR_ARG:          extra = "void inv(*() -> void f) { f(); }\n"; body = "inv(bump);"; break;
    case IR_STRUCT_INIT:  extra = "struct Ops { *() -> void cb; }\n";
                          body = "Ops o = { .cb = bump }; o.cb();"; break;
    case IR_LOCAL_BIND:   body = "*() -> void fp = bump; fp();"; break;
    case IR_FIELD_ASSIGN: extra = "struct Ops { *() -> void cb; }\n";
                          body = "Ops o; o.cb = bump; o.cb();"; break;
    case IR_FIELD_ARRAY:  extra = "typedef *() -> void VFn;\nstruct Ops { VFn[2] fns; }\n";
                          body = "Ops o; o.fns[0] = bump; o.fns[0]();"; break;
    case IR_FACTORY1:     extra = "*() -> void mk() { return bump; }\n";
                          body = "*() -> void fp = mk(); fp();"; break;
    case IR_FACTORY2:     extra = "*() -> void mk2() { return bump; }\n*() -> void mk() { return mk2(); }\n";
                          body = "*() -> void fp = mk(); fp();"; break;
    default: break;
    }
    snprintf(out, n,
        "u32 g;\n"
        "void bump() { g += 1; }\n"
        "%s"
        "interrupt TIM1 { %s }\n"
        "u32 main() { g = 1; return g; }\n",
        extra, body);
}


/* ---------------------------------------------------------------------------
 * SCOPED-BORROW grid — join POSITION x whether the borrow may be released.
 *
 * `ThreadHandle th = spawn worker(&work);` BORROWS `work` until `th.join()`.
 * The 2026-08-03 cross-block fix releases the borrow only when the join is no
 * deeper in runtime-conditional nesting than the spawn, because a join inside a
 * branch runs on SOME paths only:
 *
 *     ThreadHandle th = spawn worker(&work);
 *     if (c) { th.join(); }
 *     work.x = 2;                  // path where c is false: RACE
 *
 * That guard is what makes the rule sound, and its documented price is that a
 * join on EVERY arm is over-rejected (docs/limitations.md, "scoped-borrow: a
 * join on EVERY branch arm is over-rejected").
 *
 * THIS GRID IS THE PRE-WORK FOR THAT RELAXATION, built before it rather than
 * after, per the accept-unsafe discipline in CLAUDE.md ("build the exhaustive
 * grid FIRST, verify it fires against the pre-fix build, then relax").
 *
 * Three verdicts, not two:
 *   BV_REJECT   a genuine race — MUST be rejected. If a future relaxation flips
 *               one of these, it has shipped a data race.
 *   BV_ACCEPT   an idiom that must keep compiling.
 *   BV_OVERREJ  currently rejected, and SAFE either way. These are exactly the
 *               cells the relaxation is allowed to flip. They are reported, not
 *               failed, so the grid stays green across the change — and the
 *               BV_REJECT rows are what tells you whether the flip was sound.
 *
 * The BV_REJECT rows are chosen to cover the PRECONDITIONS an "all arms joined"
 * relaxation has to discharge, because a relaxation is only as good as the
 * conditions it checks (the Level-B lesson: the hole is the variable the model
 * left out):
 *   - a missing else arm            (BJ_THEN_ONLY)
 *   - a switch with no default      (BJ_SWITCH_NO_DEFAULT)
 *   - a join nested DEEPER than the arm   (BJ_NESTED_DEEPER)
 *   - a `goto` that skips the join  (BJ_GOTO_SKIP)
 *   - a RE-spawn after the join     (BJ_RESPAWN_AFTER)
 * ------------------------------------------------------------------------- */
typedef enum {
    BJ_STRAIGHT, BJ_HOISTED, BJ_LOOP_BODY, BJ_SAME_BRANCH,
    BJ_BOTH_ARMS, BJ_EVERY_SWITCH_ARM,
    BJ_THEN_ONLY, BJ_SWITCH_NO_DEFAULT, BJ_NESTED_DEEPER,
    BJ_GOTO_SKIP, BJ_RESPAWN_AFTER,
    BJ_COUNT
} BJoin;

typedef enum { BV_ACCEPT, BV_REJECT, BV_OVERREJ } BVerdict;

static const char *bj_name(BJoin j) {
    switch (j) {
    case BJ_STRAIGHT:          return "straight join";
    case BJ_HOISTED:           return "join after branch";
    case BJ_LOOP_BODY:         return "spawn+join in loop";
    case BJ_SAME_BRANCH:       return "spawn+join same arm";
    case BJ_BOTH_ARMS:         return "join on BOTH arms";
    case BJ_EVERY_SWITCH_ARM:  return "join on every sw arm";
    case BJ_THEN_ONLY:         return "join on ONE arm";
    case BJ_SWITCH_NO_DEFAULT: return "switch, no default";
    case BJ_NESTED_DEEPER:     return "join nested deeper";
    case BJ_GOTO_SKIP:         return "goto skips the join";
    case BJ_RESPAWN_AFTER:     return "re-spawn after join";
    case BJ_COUNT: break;
    }
    return "?";
}

static BVerdict bj_verdict(BJoin j) {
    switch (j) {
    case BJ_STRAIGHT: case BJ_HOISTED:
    case BJ_LOOP_BODY: case BJ_SAME_BRANCH:
        return BV_ACCEPT;
    /* safe in reality, rejected today — the relaxation's target set */
    case BJ_BOTH_ARMS: case BJ_EVERY_SWITCH_ARM:
        return BV_OVERREJ;
    /* every one of these has a path that reaches the use WITHOUT having joined */
    case BJ_THEN_ONLY: case BJ_SWITCH_NO_DEFAULT: case BJ_NESTED_DEEPER:
    case BJ_GOTO_SKIP: case BJ_RESPAWN_AFTER:
        return BV_REJECT;
    case BJ_COUNT: break;
    }
    return BV_REJECT;
}

/* The substring the rejection MUST contain. A grid whose only oracle is
 * "exit != 0" passes for whichever rule fires first, and this project has been
 * bitten by exactly that here before (a conditionally-joined ThreadHandle
 * tripping the LEAK check instead of the borrow check).
 *
 * BJ_GOTO_SKIP is MASKED and is labelled so rather than pretended otherwise: on
 * the goto path `t` is never joined at all, so the LEAK rule rejects the program
 * first and the borrow rule never gets to speak. The masking is itself sound —
 * the race cannot ship — but this cell does NOT currently exercise the goto
 * precondition. Whoever implements the all-arms relaxation must re-probe this
 * shape with the leak removed, because "a goto can jump over the join" is one of
 * the conditions that relaxation has to discharge. */
static const char *bj_expect(BJoin j) {
    switch (j) {
    case BJ_GOTO_SKIP:         return "not joined";   /* MASKED — see above */
    case BJ_THEN_ONLY: case BJ_SWITCH_NO_DEFAULT:
    case BJ_NESTED_DEEPER: case BJ_RESPAWN_AFTER:
        return "borrowed by a scoped spawn";
    case BJ_STRAIGHT: case BJ_HOISTED: case BJ_LOOP_BODY:
    case BJ_SAME_BRANCH: case BJ_BOTH_ARMS: case BJ_EVERY_SWITCH_ARM:
        return NULL;   /* positives / safe-either-way — nothing to match */
    case BJ_COUNT: break;
    }
    return NULL;
}

/* Every cell: spawn borrowing &work, place the join per the axis, then READ
 * work after. The read is what the borrow rule guards. */
static void gen_borrow(BJoin j, char *out, size_t n) {
    const char *body = "";
    switch (j) {
    case BJ_STRAIGHT:
        body = "  ThreadHandle t = spawn worker(&w);\n  t.join();\n"; break;
    case BJ_HOISTED:
        body = "  ThreadHandle t = spawn worker(&w);\n"
               "  if (f == 3) { f = 1; }\n  t.join();\n"; break;
    case BJ_LOOP_BODY:
        body = "  for (u32 i = 0; i < 2; i += 1) {\n"
               "    ThreadHandle t = spawn worker(&w);\n    t.join();\n  }\n"; break;
    case BJ_SAME_BRANCH:
        body = "  if (f == 3) {\n"
               "    ThreadHandle t = spawn worker(&w);\n    t.join();\n"
               "    if (w.x > 99) { return 9; }\n  }\n"; break;
    case BJ_BOTH_ARMS:
        body = "  ThreadHandle t = spawn worker(&w);\n"
               "  if (f == 3) { t.join(); } else { t.join(); }\n"; break;
    case BJ_EVERY_SWITCH_ARM:
        body = "  ThreadHandle t = spawn worker(&w);\n"
               "  switch (f) { 3 => { t.join(); } default => { t.join(); } }\n"; break;
    case BJ_THEN_ONLY:
        body = "  ThreadHandle t = spawn worker(&w);\n"
               "  if (f == 3) { t.join(); }\n"; break;
    case BJ_SWITCH_NO_DEFAULT:
        /* an int switch REQUIRES a default, so the missing-arm shape is a
         * default that does NOT join — same hole, spellable. */
        body = "  ThreadHandle t = spawn worker(&w);\n"
               "  switch (f) { 3 => { t.join(); } default => { f = 1; } }\n"; break;
    case BJ_NESTED_DEEPER:
        body = "  ThreadHandle t = spawn worker(&w);\n"
               "  if (f == 3) { if (f > 0) { t.join(); } } else { t.join(); }\n"; break;
    case BJ_GOTO_SKIP:
        body = "  ThreadHandle t = spawn worker(&w);\n"
               "  if (f == 3) { goto done; }\n  t.join();\n  done:\n"; break;
    case BJ_RESPAWN_AFTER:
        body = "  ThreadHandle t = spawn worker(&w);\n  t.join();\n"
               "  ThreadHandle t2 = spawn worker(&w);\n"; break;
    case BJ_COUNT: break;
    }
    /* BJ_RESPAWN_AFTER must still join t2 or the LEAK rule masks the borrow
     * rule — the classic weak-oracle trap. Join it AFTER the read. */
    const char *tail = (j == BJ_RESPAWN_AFTER)
        ? "  u32 v = w.x;\n  t2.join();\n  return v & 0;\n"
        : "  u32 v = w.x;\n  return v & 0;\n";
    snprintf(out, n,
        "struct W { u32 x; }\n"
        "void worker(*W p) { p.x += 1; }\n"
        "u32 main() {\n"
        "  W w; w.x = 0;\n  u32 f = 3;\n"
        "%s%s"
        "}\n", body, tail);
}

int main(void) {
    find_zerc();
    fprintf(stderr, "=== Concurrency (data-race / spawn / deadlock) matrix ===\n");
    fprintf(stderr, "    NEG: unsynchronized program -> must REJECT for a concurrency reason\n");
    fprintf(stderr, "    POS: correctly-synchronized pattern -> must COMPILE\n\n");

    char buf[1024];
    int grid_ok = 1, valid_cells = 0;
    for (COScenario s = 0; s < COSCEN_COUNT; s++) {
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

    /* ---- carrier x payload x sink grid ---- */
    fprintf(stderr, "\n  -- carrier grid (wrapper-hides-inner-kind at the spawn-arg sink) --\n");
    char cbuf[2048];
    for (CACarrier c = 0; c < CAR_COUNT; c++) {
        for (CAPayload p = 0; p < PAY_COUNT; p++) {
            if (!carrier_cell_valid(c, p)) continue;
            for (CASink k = 0; k < SINK_COUNT; k++) {
                valid_cells++;
                char nm[192];
                snprintf(nm, sizeof(nm), "carrier/%s/%s/%s",
                         car_name(c), pay_name(p), sink_name(k));
                gen_carrier(c, p, k, cbuf, sizeof(cbuf));
                int ok = run_neg(nm, cbuf);
                fprintf(stderr, "  [%-13s][%-9s][%-23s] %s\n",
                        car_name(c), pay_name(p), sink_name(k),
                        ok ? "ok" : "*** FAIL ***");
                if (!ok) grid_ok = 0;
            }
        }
    }

    /* ---- funcptr REACH x payload grid ---- */
    fprintf(stderr, "\n  -- funcptr-reach grid (how the spawn target reaches its callback) --\n");
    char rbuf[2048];
    for (CAReach r = 0; r < RCH_COUNT; r++) {
        for (CARPay p = 0; p < RPAY_COUNT; p++) {
            valid_cells++;
            int neg = reach_is_negative(p);
            char nm[192];
            snprintf(nm, sizeof(nm), "reach/%s/%s", reach_name(r), rpay_name(p));
            gen_reach(r, p, rbuf, sizeof(rbuf));
            int ok = neg ? run_neg(nm, rbuf) : run_pos(nm, rbuf);
            fprintf(stderr, "  [%-16s][%-15s][%-3s] %s\n",
                    reach_name(r), rpay_name(p), neg ? "neg" : "pos",
                    ok ? "ok" : "*** FAIL ***");
            if (!ok) grid_ok = 0;
        }
    }

    /* ---- ISR reach sub-grid (same forms, interrupt sink) ---- */
    fprintf(stderr, "\n  -- ISR reach grid (same forms at the INTERRUPT sink) --\n");
    char ibuf[1024];
    for (IsrReach r = 0; r < IR_COUNT; r++) {
        valid_cells++;
        char nm[160];
        snprintf(nm, sizeof(nm), "isr-reach/%s", isr_name(r));
        gen_isr_reach(r, ibuf, sizeof(ibuf));
        int ok = run_neg(nm, ibuf);
        fprintf(stderr, "  [%-18s][isr] %s\n", isr_name(r), ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }

    /* ---- scoped-borrow grid (join position x release) ---- */
    fprintf(stderr, "\n  -- scoped-borrow grid (where the join sits vs. where the use sits) --\n");
    char bbuf[1024];
    int overrej_known = 0;
    for (BJoin j = 0; j < BJ_COUNT; j++) {
        BVerdict v = bj_verdict(j);
        char nm[160];
        snprintf(nm, sizeof(nm), "borrow/%s", bj_name(j));
        gen_borrow(j, bbuf, sizeof(bbuf));
        if (v == BV_OVERREJ) {
            /* Reported, never failed: safe in EITHER direction. These are the
             * cells a future "all arms joined" relaxation is allowed to flip;
             * the BV_REJECT rows above are what say whether it was sound. */
            FILE *fp = fopen("/tmp/_zer_co.zer", "w");
            if (fp) { fputs(bbuf, fp); fclose(fp); }
            char cmd[512];
            snprintf(cmd, sizeof(cmd),
                     "%s /tmp/_zer_co.zer -o /tmp/_zer_co.c 2>/dev/null", zerc_path);
            int accepted = (system(cmd) == 0);
            overrej_known++;
            fprintf(stderr, "  [%-22s][safe-either-way] %s\n", bj_name(j),
                    accepted ? "ACCEPTED (relaxation landed)"
                             : "rejected (known over-rejection)");
            continue;
        }
        valid_cells++;
        int ok = (v == BV_REJECT) ? run_neg(nm, bbuf) : run_pos(nm, bbuf);
        /* second oracle: the rejection must be the EXPECTED rule, not whichever
         * one happened to fire first. */
        if (ok && v == BV_REJECT && bj_expect(j)) {
            FILE *ef = fopen("/tmp/_zer_co.err", "r");
            char eb2[4096]; eb2[0] = 0;
            if (ef) { size_t r2 = fread(eb2, 1, sizeof(eb2) - 1, ef); eb2[r2] = 0; fclose(ef); }
            if (!strstr(eb2, bj_expect(j))) {
                ok = 0; failed++; passed--;
                fprintf(stderr, "  FAIL [WRONG-RULE] %s — rejected, but not by the rule\n"
                                "       under test. want %s; got: %.120s\n",
                        nm, bj_expect(j), eb2);
            }
        }
        fprintf(stderr, "  [%-22s][%-3s] %s\n", bj_name(j),
                v == BV_REJECT ? "neg" : "pos", ok ? "ok" : "*** FAIL ***");
        if (!ok) grid_ok = 0;
    }
    fprintf(stderr, "  (%d cell(s) are safe in either direction — the relaxation target set)\n",
            overrej_known);

    fprintf(stderr, "\n=== conc-matrix: %d/%d cells correct ===\n", passed, valid_cells);
    fprintf(stderr, "    false negatives: %d | invalid probes: %d | over-rejections: %d\n",
            false_neg, invalid_probe, over_reject);
    if (!grid_ok) {
        fprintf(stderr, "CONC MATRIX HAS HOLES — a data race / deadlock / unjoined thread the\n");
        fprintf(stderr, "analyzer mishandles (false negative = unsynchronized program accepted).\n");
        return 1;
    }
    return 0;
}
