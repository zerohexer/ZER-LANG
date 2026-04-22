/* src/safety/atomic_rules.h
 *
 * Pure predicates for @atomic_* intrinsic safety (typing.v Section E).
 * Linked into zerc, VST-verified in proofs/vst/verif_atomic_rules.v
 * against oracle proofs/operational/lambda_zer_typing/typing.v Section E.
 *
 * Oracle rules:
 *   E01: @atomic_* operand width must be 1, 2, 4, or 8 bytes
 *        (hardware-supported atomic sizes on all targets)
 *   E02: @atomic_* first arg must be pointer-to-integer
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_ATOMIC_RULES_H
#define ZER_SAFETY_ATOMIC_RULES_H

/* Returns 1 iff `bytes` is a valid atomic width (1, 2, 4, or 8).
 * Oracle: atomic_width_valid in typing.v Section E (E01). */
int zer_atomic_width_valid(int bytes);

/* Returns 1 iff the first argument of @atomic_* is a pointer to an
 * integer type.
 *
 * Args:
 *   is_ptr_to_int: 1 if caller determined arg is *T with T integer,
 *                  0 otherwise
 *
 * Oracle: atomic_arg_valid in typing.v Section E (E02). Trivial but
 * documents the rule. */
int zer_atomic_arg_is_ptr_to_int(int is_ptr_to_int);

#endif
