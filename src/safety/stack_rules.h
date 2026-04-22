/* src/safety/stack_rules.h
 *
 * Pure predicates for stack-resource safety (typing.v Section S).
 * Linked into zerc, VST-verified in proofs/vst/verif_stack_rules.v.
 *
 * Oracle theorems (typing.v):
 *   S01_under_limit_ok / S01_over_limit_rejected
 *   S02_call_chain_over_limit_rejected (same predicate)
 */
#ifndef ZER_SAFETY_STACK_RULES_H
#define ZER_SAFETY_STACK_RULES_H

/* Returns 1 iff frame size is within the configured limit.
 * Used for both per-function frame check (S01) and call-chain
 * accumulated stack check (S02).
 *
 * Oracle: typing.v:740 stack_frame_valid.
 * Callers: checker.c check_stack_depth — --stack-limit enforcement. */
int zer_stack_frame_valid(int limit, int frame);

#endif
