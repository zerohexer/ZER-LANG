/* src/safety/optional_rules.h
 *
 * Pure predicates for optional-type safety rules (typing.v Section N).
 * Linked into zerc, VST-verified in proofs/vst/verif_optional_rules.v
 * against oracle proofs/operational/lambda_zer_typing/typing.v.
 *
 * Oracle rules:
 *   N01: non-null *T cannot accept null — null source requires optional dest
 *   N02: null can only be assigned to OPTIONAL types
 *   N03: if-unwrap `if (x) |v|` requires optional source
 *   N05: no nested ??T — parser/checker must reject
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_OPTIONAL_RULES_H
#define ZER_SAFETY_OPTIONAL_RULES_H

/* TypeKind constant — must match types.h TYPE_OPTIONAL (index 14). */
#define ZER_TK_OPTIONAL  14

/* Returns 1 iff a type with this kind permits null.
 * Covers N01, N02, N03: all three rules reduce to
 * `kind == TYPE_OPTIONAL`. The safety story: null can flow into a
 * type only if the type has an optional wrapper.
 *
 * Oracle: permits_null in typing.v Section N. */
int zer_type_permits_null(int type_kind);

/* Returns 1 iff the pair (outer_kind, inner_kind) forms a nested
 * optional `??T`. Both must be TYPE_OPTIONAL.
 *
 * Used at `TYNODE_OPTIONAL` resolution to reject `?distinct(?T)`
 * etc. (BUG-506 already unwraps distinct before calling this).
 *
 * Oracle: has_nested_optional in typing.v Section N. */
int zer_type_is_nested_optional(int outer_kind, int inner_kind);

#endif
