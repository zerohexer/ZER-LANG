/* src/safety/arith_rules.h
 *
 * Pure predicates for arithmetic safety — compile-time div-by-zero,
 * narrowing, integer literal range fit.
 *
 * Linked into zerc, VST-verified in proofs/vst/verif_arith_rules.v
 * against oracle proofs/operational/lambda_zer_typing/typing.v Section M.
 *
 * Oracle theorems (typing.v):
 *   M01_const_div_by_zero_rejected / M01_const_div_nonzero_ok
 *   M02_with_proof_ok / M02_without_proof_rejected
 *   M07_widening_ok / M07_narrowing_needs_truncate
 *   M08_fitting_literal_ok / M08_overflow_literal_rejected
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_ARITH_RULES_H
#define ZER_SAFETY_ARITH_RULES_H

/* Returns 1 iff divisor is nonzero. For compile-time constant division.
 * Oracle: typing.v:604 div_valid (M01).
 * Callers: checker.c NODE_BINARY SLASH/PERCENT at compile-time eval. */
int zer_div_valid(int divisor);

/* Returns 1 iff the divisor has been proven nonzero by VRP, const eval,
 * or a guard check. Caller passes has_proof=1 if proof exists, 0 otherwise.
 * Oracle: typing.v:619 divisor_proven_nonzero (M02).
 * Callers: checker.c divisor-not-proven-nonzero error sites. */
int zer_divisor_proven_nonzero(int has_proof);

/* Returns 1 iff narrowing assignment is safe.
 * Valid iff src_width <= dst_width OR has_truncate flag set.
 * Oracle: typing.v:631 narrowing_valid (M07).
 * Callers: checker.c compound assignment narrowing check. */
int zer_narrowing_valid(int src_width, int dst_width, int has_truncate);

/* NOTE: zer_literal_fits (M08) deferred to Batch 1b — tlong VST proof
 * pattern needs different tactic than int-based predicates. Revisit
 * with split u32/i32/u64 design. Call site in is_literal_compatible
 * stays inline for now. */

#endif
