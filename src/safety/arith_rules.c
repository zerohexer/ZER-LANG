/* src/safety/arith_rules.c — Phase 1 M section
 *
 * Pure predicates for arithmetic safety (division, narrowing, literal fit).
 * Linked into zerc, VST-verified in proofs/vst/verif_arith_rules.v against
 * oracle proofs/operational/lambda_zer_typing/typing.v Section M.
 *
 * Oracle rules:
 *   M01 (div_valid):         divisor != 0 for compile-time division
 *   M02 (divisor_proven):    VRP-determined nonzero gate
 *   M07 (narrowing_valid):   src_width <= dst_width OR has_truncate
 *   M08 (literal_fits):      literal in [min_val, max_val]
 *
 * See docs/formal_verification_plan.md Level 3 + docs/phase1_catalog.md §2.M.
 */
#include "arith_rules.h"

int zer_div_valid(int divisor) {
    if (divisor == 0) return 0;
    return 1;
}

int zer_divisor_proven_nonzero(int has_proof) {
    if (has_proof == 0) return 0;
    return 1;
}

int zer_narrowing_valid(int src_width, int dst_width, int has_truncate) {
    if (src_width <= dst_width) return 1;
    if (has_truncate != 0) return 1;
    return 0;
}

/* NOTE: zer_literal_fits (M08) deferred to Batch 1b.
 * VST proof for tlong (int64) compare goals requires different tactic
 * than the int-based predicates above. Revisit with a split design
 * (separate u32 / i32 / u64 predicates using tuint/tint/tulong) to
 * avoid the tlong forward_if complexity. */
