/* src/safety/cast_rules.h
 *
 * Pure predicates for cast intrinsic shape checks (typing.v J-extended).
 * Linked into zerc, VST-verified in proofs/vst/verif_cast_rules.v.
 */
#ifndef ZER_SAFETY_CAST_RULES_H
#define ZER_SAFETY_CAST_RULES_H

/* J1 — conversion_kind encoding (typing.v:828-831). */
#define ZER_CONV_INT_TO_PTR   0
#define ZER_CONV_PTR_TO_INT   1
#define ZER_CONV_CSTYLE       2

/* Returns 1 iff integer↔pointer conversion uses proper intrinsic.
 * C-style (Type)expr casts are rejected; must use @inttoptr / @ptrtoint.
 * Oracle: typing.v:833 conversion_safe (J02, J03). */
int zer_conversion_safe(int kind);

/* Returns 1 iff src and dst widths are equal (same-size bitcast).
 * Oracle: typing.v:852 bitcast_valid (J05). */
int zer_bitcast_width_valid(int src_width, int dst_width);

/* Returns 1 iff bitcast operand is primitive (scalar).
 * Oracle: typing.v:864 bitcast_operand_valid (J06). */
int zer_bitcast_operand_valid(int is_primitive);

/* Returns 1 iff at least one side of @cast is a distinct typedef.
 * Oracle: typing.v:878 cast_distinct_valid (J07). */
int zer_cast_distinct_valid(int src_is_distinct, int dst_is_distinct);

/* Returns 1 iff @saturate/@truncate operand is numeric type.
 * Oracle: typing.v:892 saturate_operand_valid (J08). */
int zer_saturate_operand_valid(int is_numeric);

/* Returns 1 iff @ptrtoint source is a pointer.
 * Oracle: typing.v:904 ptrtoint_source_valid (J09). */
int zer_ptrtoint_source_valid(int is_pointer);

/* Returns 1 iff src and dst type tags are equal.
 * Used as catch-all for invalid cast detection.
 * Oracle: typing.v:916 cast_types_compatible (J10). */
int zer_cast_types_compatible(int src_tag, int dst_tag);

#endif
