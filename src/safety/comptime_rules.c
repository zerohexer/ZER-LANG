/* src/safety/comptime_rules.c — Phase 1 R section
 *
 * Comptime-soundness predicates. Linked into zerc, VST-verified in
 * proofs/vst/verif_comptime_rules.v against typing.v Section R.
 *
 * Oracle rules:
 *   R02 — comptime function args must be compile-time constants
 *   R04 — static_assert condition must be constant AND truthy
 *   R06 — comptime evaluation budget: < 1,000,000 operations
 *   R07 — expression nesting depth < 1000
 *
 * See docs/phase1_catalog.md §2.R.
 */
#include "comptime_rules.h"

int zer_comptime_arg_valid(int is_constant) {
    if (is_constant == 0) {
        return 0;
    }
    return 1;
}

int zer_static_assert_holds(int is_constant, int value) {
    if (is_constant == 0) {
        return 0;
    }
    if (value == 0) {
        return 0;
    }
    return 1;
}

int zer_comptime_ops_valid(int ops_count) {
    if (ops_count >= ZER_COMPTIME_OPS_BUDGET) {
        return 0;
    }
    return 1;
}

int zer_expr_nesting_valid(int depth) {
    if (depth >= ZER_EXPR_NESTING_LIMIT) {
        return 0;
    }
    return 1;
}
