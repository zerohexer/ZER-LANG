/* src/safety/misc_rules.h
 *
 * Final batch of Phase 1 predicates — miscellaneous small rules
 * from remaining typing.v sections. Linked into zerc, VST-verified in
 * proofs/vst/verif_misc_rules.v.
 *
 * Oracle rules:
 *   Q03: switch on int must have default (typing.v Section Q)
 *   I01-I11: cast preserves qualifiers — added-only (Section I)
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_MISC_RULES_H
#define ZER_SAFETY_MISC_RULES_H

/* Returns 1 iff an int switch has a default case.
 * Rule: int switches must have `default =>` because there are
 * infinitely many ints — explicit exhaustiveness impossible.
 *
 * Arg `has_default_flag`: caller determines by scanning switch arms,
 * passes 1 if any arm is default, 0 otherwise.
 *
 * Oracle: int_switch_has_default in typing.v Section Q (Q03). */
int zer_int_switch_has_default(int has_default_flag);

/* Returns 1 iff a bool switch covers both true and false (or has default).
 * Rule: switch on bool must be exhaustive — bools have exactly 2 values.
 *
 * Args (0 or 1):
 *   has_default: 1 if switch has `default =>` arm
 *   has_true:    1 if switch has `true =>` arm (or default)
 *   has_false:   1 if switch has `false =>` arm (or default)
 *
 * Oracle: bool_switch_exhaustive in typing.v Section Q (Q01). */
int zer_bool_switch_covers_both(int has_default, int has_true, int has_false);

#endif
