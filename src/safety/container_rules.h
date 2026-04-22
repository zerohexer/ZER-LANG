/* src/safety/container_rules.h
 *
 * Pure predicates for container/field well-formedness
 * (typing.v T/K sections). Linked into zerc, VST-verified in
 * proofs/vst/verif_container_rules.v.
 *
 * Oracle rules (typing.v):
 *   T02: field type cannot be void
 *   T03: type_has_size (non-void types have sizeof)
 *   K01: container monomorphization depth bounded (container_depth_limit = 32)
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_CONTAINER_RULES_H
#define ZER_SAFETY_CONTAINER_RULES_H

/* Maximum container nesting depth (container_depth_limit in typing.v). */
#define ZER_CONTAINER_DEPTH_LIMIT 32

/* Returns 1 iff `depth` is within the nesting limit.
 * Rejects self-referential recursion (e.g., `container Node(T) {
 * ?*Node(T) next; }` gets monomorphized to at most 32 depth).
 *
 * Oracle: container_depth_valid in typing.v. */
int zer_container_depth_valid(int depth);

/* Returns 1 iff a field type is valid (non-void).
 * Struct/union fields with void type have no meaning; reject.
 *
 * Oracle: field_type_valid in typing.v (T02). */
int zer_field_type_valid(int is_void);

/* Returns 1 iff a type has a size (i.e., is not void).
 * Used for sizeof, array-element size, etc.
 *
 * Oracle: type_has_size in typing.v. */
int zer_type_has_size(int is_void);

#endif
