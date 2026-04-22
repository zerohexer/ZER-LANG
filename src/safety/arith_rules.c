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

/* M08 — integer literal fits in target type.
 * Single-predicate design using tuint (unsigned int) args — avoids the
 * tlong forward_if goal structure issue observed in Batch 1 initial
 * attempt (see docs/proof-internals.md "tlong VST gotcha").
 *
 * Caller range-checks that the 64-bit literal value fits in uint32
 * FIRST, then calls this predicate with the per-type max. Works for
 * U8/U16/U32/USIZE(32-bit)/I8/I16/I32 — since ZER positive literals
 * are passed as uint64, and all these type maxes fit in uint32.
 *
 * U64 / I64 / USIZE(64-bit) skip this check (positive literal always
 * fits). Signed negative literals are handled separately via NODE_UNARY. */
int zer_literal_fits_u(unsigned int max_val, unsigned int lit) {
    if (lit > max_val) return 0;
    return 1;
}
