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

/* Declaration position encoding for zer_container_position_valid.
 * Must match order in typing.v T02/T03. */
#define ZER_DP_GLOBAL   0
#define ZER_DP_FIELD    1
#define ZER_DP_VARIANT  2

/* Returns 1 iff Pool/Ring/Slab position is at global scope.
 * Rejects struct-field and union-variant positions.
 *
 * Oracle: container_position_valid in typing.v (T02, T03).
 * Callers: checker.c NODE_STRUCT_DECL field-type check + union variant check. */
int zer_container_position_valid(int decl_position);

/* Handle element kind encoding for zer_handle_element_valid.
 * Matches typing.v T04 classification. */
#define ZER_HE_STRUCT   0
#define ZER_HE_PRIM     1
#define ZER_HE_VOID     2

/* Returns 1 iff Handle(T)'s element type is struct.
 * Handle auto-deref requires struct fields to access.
 *
 * Oracle: handle_element_valid in typing.v (T04).
 * Callers: checker.c NODE_FIELD on Handle auto-deref path. */
int zer_handle_element_valid(int element_kind);

/* Type-category encoding for zer_container_source_valid.
 * Matches typing.v K01 classification. */
#define ZER_TCAT_PRIM    0
#define ZER_TCAT_PTR     1
#define ZER_TCAT_SLICE   2
#define ZER_TCAT_STRUCT  3

/* Returns 1 iff @container source is a pointer type.
 * @container(*T, ptr, field) requires ptr to be a concrete pointer,
 * not a primitive/slice/struct value.
 *
 * Oracle: container_source_valid in typing.v (K01).
 * Callers: checker.c NODE_INTRINSIC @container source check. */
int zer_container_source_valid(int type_category);

#endif
