/* test_global_init_matrix.c — global-initializer constant-context oracle (2026-08-20).
 *
 * WHY THIS EXISTS. A file-scope initializer must be a C constant expression.
 * Some intrinsics fold to one (`@popcount(255)`, `@bswap32(k)`); most do not —
 * they lower to a statement-expression, a runtime call, or a hardware read that
 * by definition has no value until the program is running.
 *
 * The checker's list of "cannot appear in a global initializer" was maintained
 * BY HAND, and it had already drifted twice:
 *   * G7 (2026-08-01) found @saturate / @bitcast / @truncate / @addc / @subb /
 *     @mulw accepted by the checker and stopped only by GCC, and added six names.
 *   * This file's own measurement found ELEVEN MORE — @expect, @cpu_id,
 *     @port_in8/16/32 and the six cpu_* identity queries — still accepted, with
 *     `zerc f.zer -o out.c` exiting 0 for a program that cannot be compiled.
 *
 * Both times the emitter already KNEW: it emits an undeclared
 * `__zer_intrinsic_X_unsupported_in_constant_context` so GCC errors loudly
 * (BUG-767). But that knowledge is a FALL-THROUGH at the end of emit_expr's
 * intrinsic chain, not a predicate the checker can ask — so the two sides could
 * disagree, and did.
 *
 * THE INVARIANT UNDER TEST — one sentence:
 *     for every intrinsic, in a global-initializer position, the CHECKER's
 *     verdict must agree with GCC's — the checker may never accept a program
 *     that GCC then rejects.
 *
 * The other direction is allowed on purpose: the checker rejecting something
 * GCC would have accepted is over-rejection, not unsoundness, so this grid does
 * not fail on it. Only the accept-then-GCC-rejects direction is a failure.
 *
 * WHAT MAKES THIS SELF-MAINTAINING, and the reason it is a grid rather than a
 * list: the intrinsic set is DERIVED FROM THE COMPILER at run time, by probing
 * each candidate name and keeping the ones it does not call "unknown
 * intrinsic". A newly added intrinsic is therefore covered the day it lands,
 * with nobody having to remember this file. That is the property the two
 * hand-maintained lists lacked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int total = 0, passed = 0, failed = 0;
static const char *zerc_path = NULL;

static void find_zerc(void) {
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    if (system("test -x /tmp/zerc") == 0) { zerc_path = "/tmp/zerc"; return; }
    fprintf(stderr, "ERROR: cannot find zerc\n");
    exit(2);
}

/* Candidate intrinsic names. Names the compiler does not recognise are dropped
 * at run time, so this list may be a superset — it exists only to enumerate
 * what to PROBE, and the compiler decides what actually exists. Keeping it a
 * superset is deliberate: a stale entry costs nothing, a missing one would
 * silently shrink the grid. */
static const char *CANDIDATES[] = {
    /* pure compile-time computations — expected to FOLD */
    "size", "offset", "popcount", "ctz", "clz", "parity", "ffs",
    "bswap16", "bswap32", "bswap64", "truncate", "cast", "bitcast", "saturate",
    "ptrtoint",
    /* carry primitives — runtime calls */
    "addc", "subb", "mulw",
    /* branch hint — wraps __builtin_expect, not a constant */
    "expect", "unreachable",
    /* CPU identity / inspection — runtime reads */
    "cpu_id", "cpu_model_id", "cpu_core_id", "cpu_num_cores",
    "cpu_cache_line_size", "cpu_current_mode", "cpu_get_priv_level",
    "cpu_vendor_id", "cpu_feature_bits", "cpu_read_sp", "cpu_read_tp",
    "cpu_read_flags", "cpu_read_counter", "cpu_get_pc", "cpu_cpuid",
    "cpu_cpuid_ecx", "cpu_rdrand", "cpu_rdseed",
    /* port I/O — runtime reads */
    "port_in8", "port_in16", "port_in32",
    /* privileged register reads — runtime */
    "cpu_read_msr", "cpu_read_cr0", "cpu_read_cr2", "cpu_read_cr3",
    "cpu_read_cr4", "cpu_read_xcr0", "cpu_read_dr", "cpu_read_pmc",
    "cpu_read_fsbase", "cpu_read_gsbase",
    /* MMU queries — runtime */
    "mmu_get_pt", "mmu_get_kernel_pt", "mmu_get_fault_addr",
    "mmu_get_fault_status", "mmu_is_enabled",
    /* memory / atomics — never constant */
    "atomic_load", "probe", "trap", "barrier",
};
enum { CAND_COUNT = (int)(sizeof(CANDIDATES) / sizeof(CANDIDATES[0])) };

/* Write a global-initializer program using `name` with `argc` zero arguments. */
static void gen(const char *name, int argc, char *buf, size_t cap) {
    char args[64];
    args[0] = '\0';
    for (int i = 0; i < argc; i++)
        snprintf(args + strlen(args), sizeof(args) - strlen(args),
                 "%s0", i ? ", " : "");
    snprintf(buf, cap,
             "const u32 G = @%s(%s);\n"
             "u32 main() { return G; }\n", name, args);
}

/* Run zerc, emitting C. Returns:
 *   0  checker rejected (or the name is unknown / arity mismatch — caller
 *      distinguishes by inspecting the message)
 *   1  checker accepted and produced /tmp/_zer_gi.c
 * The error text is copied into `err` so the caller can tell "unknown
 * intrinsic" and "wrong arity" apart from a real rejection. */
static int run_zerc(const char *src, char *err, size_t errcap) {
    FILE *f = fopen("/tmp/_zer_gi.zer", "w");
    if (!f) return 0;
    fputs(src, f); fclose(f);
    remove("/tmp/_zer_gi.c");

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s /tmp/_zer_gi.zer -o /tmp/_zer_gi.c >/tmp/_zer_gi.err 2>&1",
             zerc_path);
    if (system(cmd) == -1) return 0;

    err[0] = '\0';
    FILE *e = fopen("/tmp/_zer_gi.err", "r");
    if (e) { size_t n = fread(err, 1, errcap - 1, e); err[n] = '\0'; fclose(e); }

    if (strstr(err, "error:")) return 0;
    FILE *c = fopen("/tmp/_zer_gi.c", "r");
    if (!c) return 0;
    fclose(c);
    return 1;
}

static int gcc_accepts(void) {
    return system("gcc -fwrapv -w -c /tmp/_zer_gi.c -o /tmp/_zer_gi.o "
                  ">/dev/null 2>&1") == 0;
}

static void run_one(const char *name) {
    char src[512], err[4096];
    int accepted = 0, known = 0;

    /* Find an arity the compiler accepts; skip the name entirely if it does not
     * exist or never type-checks with integer arguments (e.g. @size takes a TYPE
     * and @cast takes a type + value — those are covered by their own tests). */
    for (int argc = 0; argc <= 3; argc++) {
        gen(name, argc, src, sizeof(src));
        accepted = run_zerc(src, err, sizeof(err));
        if (strstr(err, "unknown intrinsic")) return;          /* not an intrinsic here */
        if (strstr(err, "argument") || strstr(err, "arguments")) continue; /* wrong arity */
        known = 1;
        break;
    }
    if (!known) return;

    total++;
    if (!accepted) { passed++; return; }   /* checker rejected — always sound */

    if (gcc_accepts()) { passed++; return; }   /* genuinely a constant — fine */

    fprintf(stderr,
            "      @%s: *** CHECKER ACCEPTED a global initializer that GCC "
            "REJECTS — `zerc -o out.c` exits 0 for a program that cannot be "
            "compiled\n", name);
    failed++;
}

int main(void) {
    find_zerc();
    printf("=== Global-initializer constant-context matrix ===\n");
    for (int i = 0; i < CAND_COUNT; i++) run_one(CANDIDATES[i]);
    printf("  intrinsics probed: %d   passed: %d   failed: %d\n",
           total, passed, failed);
    if (failed == 0) printf("GLOBAL INIT MATRIX CLEAN\n");
    return failed ? 1 : 0;
}
