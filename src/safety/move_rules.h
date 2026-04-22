/* src/safety/move_rules.h
 *
 * Pure predicates for move-struct ownership rules.
 * Linked into zerc, VST-verified in proofs/vst/verif_move_rules.v
 * against oracle proofs/operational/lambda_zer_move/iris_move_specs.v.
 *
 * Oracle rules (λZER-move Section B):
 *   B01: use-after-move — transferred struct cannot be used
 *   B02: consume on pass/assign — move struct value transferred to callee
 *   step_spec_consume: transferred resource can't be used again
 *
 * A "move struct" has `is_move=true` on its struct_type. Callers of
 * these predicates convert (TypeKind, is_move_flag) → bool inputs.
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_MOVE_RULES_H
#define ZER_SAFETY_MOVE_RULES_H

/* TypeKind constant — must match types.h TYPE_STRUCT (index 17). */
#define ZER_TK_STRUCT  17

/* Returns 1 iff a type with the given (kind, is_move_flag) is a
 * move struct.
 *
 * Rule: kind must be STRUCT AND the struct must have is_move set.
 * Non-struct types never qualify, even if is_move_flag is set
 * (defensive — caller shouldn't pass that combination anyway).
 *
 * Oracle: is_move_struct_type in zercheck.c, derived from λZER-move
 * syntax which marks move-struct allocations with EAllocMove and
 * requires alive_move resource for subsequent use. */
int zer_type_kind_is_move_struct(int type_kind, int is_move_flag);

/* Returns 1 iff ownership transfer should be tracked for a type:
 *   - directly a move struct, OR
 *   - a regular struct/union containing a move-struct field/variant
 *
 * Args are pre-computed bools (0 or 1). Caller walks the type
 * structure once, passing the flags.
 *
 * Oracle: should_track_move in zercheck.c (unified helper). */
int zer_move_should_track(int is_move_struct_direct, int contains_move_field);

#endif
