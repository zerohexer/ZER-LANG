/* src/safety/mmio_rules.h
 *
 * Pure predicates for @inttoptr MMIO safety. Linked into zerc,
 * VST-verified in proofs/vst/verif_mmio_rules.v against the
 * λZER-mmio operational oracle
 * (proofs/operational/lambda_zer_mmio/iris_mmio_theorems.v).
 *
 * Oracle rules:
 *   H01/H02: addr NOT in any declared range → step_inttoptr stuck
 *   H03:     addr in range but misaligned → step_inttoptr stuck
 *   H04:     empty ranges list → all @inttoptr stuck (strict mode)
 *
 * These predicates are the per-check decisions. The checker / emitter
 * iterates over the declared range list and calls zer_mmio_addr_in_range
 * once per range.
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_MMIO_RULES_H
#define ZER_SAFETY_MMIO_RULES_H

/* Returns 1 iff `addr` is within the inclusive range [start, end].
 * Oracle: addr_in_ranges iterates over ranges and checks each via
 * this predicate in lambda_zer_mmio/semantics.v. */
int zer_mmio_addr_in_range(int addr, int start, int end);

/* Returns 1 iff @inttoptr is allowed given the two gate decisions.
 * Both must be 1 per step_inttoptr_ok in lambda_zer_mmio/semantics.v:
 * "BOTH range check AND alignment required" (theorems.v header). */
int zer_mmio_inttoptr_allowed(int in_any_range, int aligned);

#endif
