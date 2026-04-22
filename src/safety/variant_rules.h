/* src/safety/variant_rules.h
 *
 * Pure predicates for union variant safety (typing.v Section P).
 * Linked into zerc, VST-verified in proofs/vst/verif_variant_rules.v
 * against oracle proofs/operational/lambda_zer_typing/typing.v Section P.
 *
 * Oracle theorems (typing.v):
 *   P01_direct_read_unsafe / P01_switch_read_safe
 *   P02_no_self_mutation_in_arm / P02_other_ops_safe_in_arm
 *
 * See docs/formal_verification_plan.md Level 3 + docs/phase1_catalog.md §2.P.
 */
#ifndef ZER_SAFETY_VARIANT_RULES_H
#define ZER_SAFETY_VARIANT_RULES_H

/* Union read mode encoding matching typing.v P01 union_read_mode. */
#define ZER_URM_DIRECT   0
#define ZER_URM_SWITCH   1

/* Returns 1 iff the union variant read mode is safe.
 * DirectRead (reading a variant without switch-capture) is unsafe because
 * the active variant may not match. SwitchRead (inside a switch arm that
 * captured this variant) is safe because the tag was checked.
 *
 * Oracle: typing.v:277 read_mode_safe (P01).
 * Callers: checker.c union field access outside switch context. */
int zer_union_read_mode_safe(int mode);

/* Returns 1 iff a union-scoped operation is safe.
 * `self_access` = 1 means the operation mutates or takes address of the
 * SAME union variable that the enclosing switch arm is reading from.
 * Such self-access would invalidate the captured variant — the tag may
 * change mid-operation.
 *
 * Oracle: typing.v:298 arm_safe_op (P02).
 * Callers: checker.c union switch-arm mutation check via
 * check_union_switch_mutation helper. */
int zer_union_arm_op_safe(int self_access);

#endif
