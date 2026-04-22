/* src/safety/range_checks.h
 *
 * Pure range-validity predicates used by the parser and checker
 * when processing Pool(T, N), Ring(T, N), Semaphore(N), array sizes,
 * switch variant indices, etc.
 *
 * Linked into zerc. Verified by `make check-vst` via CompCert
 * clightgen + VST. See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_RANGE_CHECKS_H
#define ZER_SAFETY_RANGE_CHECKS_H

/* Returns 1 iff n > 0. Used to validate Pool(T, N), Ring(T, N),
 * Semaphore(N), array sizes — every "positive compile-time constant
 * count" specifier in ZER. If 0 or negative, the type is rejected.
 *
 * Callers:
 *   - checker.c Pool/Ring/Semaphore count validation (~3 sites)
 *
 * Passing int (not int64) because the C caller already narrowed via
 * (uint32_t) cast in the emitted type. Counts beyond INT_MAX are not
 * meaningful for static memory pools. */
int zer_count_is_positive(int n);

/* Returns 1 iff 0 <= idx < size. Bounds check predicate for array/slice
 * access where VRP couldn't prove safety. Early-return form (not
 * short-circuit) keeps the VST proof simple.
 *
 * Callers:
 *   - emitter.c inline bounds check (when present) */
int zer_index_in_bounds(int size, int idx);

/* Returns 1 iff 0 <= variant_idx < n_variants. Used when emitting
 * tagged-union access — the tag must identify a valid variant.
 *
 * Callers:
 *   - checker.c union variant dispatch (where applicable) */
int zer_variant_in_range(int n_variants, int variant_idx);

/* Returns 1 iff start <= end_ AND end_ <= size. Slice bounds check.
 * Uses end_ (not end) because `end` is a Coq vernacular keyword.
 * Oracle: typing.v:554 slice_bounds_valid (L02, L03).
 * Callers: checker.c slice expression bounds check. */
int zer_slice_bounds_valid(int size, int start, int end_);

/* Returns 1 iff 0 <= idx < width. Bit index in range for N-bit type.
 * Same shape as zer_index_in_bounds but separate predicate for typing.v
 * L06 mapping (distinct from array bounds L01).
 * Oracle: typing.v:584 bit_index_valid.
 * Callers: checker.c bit-extract slice on integer type. */
int zer_bit_index_valid(int width, int idx);

#endif
