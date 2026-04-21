/* src/safety/coerce_rules.h
 *
 * Pure predicates for implicit type coercion rules. Linked into zerc,
 * VST-verified in proofs/vst/verif_coerce_rules.v.
 *
 * Called from types.c:can_implicit_coerce after the type structure
 * check (same-kind / optional wrap / etc.). These predicates encode
 * the PER-PRIMITIVE coercion rules:
 *   - integer widening (same sign, smaller → larger)
 *   - unsigned to larger signed (u8 → i16 etc.)
 *   - USIZE same-width cases (32-bit target)
 *   - float widening (f32 → f64 only; no narrowing)
 *   - qualifier preservation (cannot strip const or volatile)
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_COERCE_RULES_H
#define ZER_SAFETY_COERCE_RULES_H

/* Returns 1 iff integer widening is allowed from (from_signed, from_width)
 * to (to_signed, to_width), assuming different widths.
 *
 * Args (pass as 0 or 1):
 *   from_signed: 1 if source type is signed, 0 if unsigned
 *   to_signed:   1 if destination type is signed, 0 if unsigned
 *   from_width:  bit width of source type (8, 16, 32, 64)
 *   to_width:    bit width of destination type (8, 16, 32, 64)
 *
 * Rules:
 *   1. Same sign + from_width < to_width: allowed (widening)
 *   2. unsigned → signed + from_width < to_width: allowed
 *      (u8 → i16, u16 → i32, u32 → i64 — destination can represent all sources)
 *   3. Everything else: NOT allowed (use @truncate, @saturate, @bitcast)
 *
 * Does NOT handle USIZE same-width cases — those go through
 * zer_coerce_usize_same_width_allowed. Callers check same-width first. */
int zer_coerce_int_widening_allowed(int from_signed, int to_signed,
                                     int from_width, int to_width);

/* Returns 1 iff same-width integer coercion is allowed.
 * Only legal case: one side is USIZE (pointer-width) and signs match.
 * This handles u32 ↔ usize on a 32-bit target without narrowing. */
int zer_coerce_usize_same_width_allowed(int from_is_usize, int to_is_usize,
                                          int from_signed, int to_signed);

/* Returns 1 iff float widening is allowed. Only case: f32 → f64.
 *
 * from_is_f32: 1 if source is f32
 * to_is_f64:   1 if destination is f64
 *
 * f64 → f32 is NOT implicit (narrowing). */
int zer_coerce_float_widening_allowed(int from_is_f32, int to_is_f64);

/* Returns 1 iff coercion preserves volatile (i.e., doesn't strip it).
 * If from has volatile and to doesn't, that's stripping — banned.
 * Otherwise allowed (both plain, both volatile, or adding volatile). */
int zer_coerce_preserves_volatile(int from_volatile, int to_volatile);

/* Returns 1 iff coercion preserves const (i.e., doesn't strip it).
 * Same rule as volatile — cannot strip const during coercion. */
int zer_coerce_preserves_const(int from_const, int to_const);

#endif
