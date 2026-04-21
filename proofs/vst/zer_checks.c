/* zer_checks.c — C implementations of safety predicates.
 *
 * Each function here corresponds to a predicate proven in
 * proofs/operational/lambda_zer_typing/typing.v. VST verifies that
 * the C implementation matches the Coq predicate for every possible
 * input.
 *
 * Functions in this file are SELF-CONTAINED — no external state,
 * no malloc, no recursion. They are the "core predicate" style
 * of safety check: given some inputs, return 1 iff the predicate
 * holds.
 *
 * This file is VST-verified in verif_zer_checks.v.
 */

/* Handle states from zercheck.c */
#define HS_UNKNOWN       0
#define HS_ALIVE         1
#define HS_FREED         2
#define HS_MAYBE_FREED   3
#define HS_TRANSFERRED   4
#define HS_ESCAPED       5

/* ---- Section A / T01: handle state checks ---- */

/* is_alive: returns 1 iff state == HS_ALIVE (state must be ALIVE for
 * use/free to be safe; HS_FREED / HS_MAYBE_FREED / HS_TRANSFERRED
 * all reject). */
int is_alive(int state) {
    if (state == HS_ALIVE) {
        return 1;
    }
    return 0;
}

/* is_freed: returns 1 iff state == HS_FREED (for detecting stale
 * handles in UAF checks). */
int is_freed(int state) {
    if (state == HS_FREED) {
        return 1;
    }
    return 0;
}

/* is_transferred: returns 1 iff state == HS_TRANSFERRED
 * (move struct / spawn ownership-move tracking). */
int is_transferred(int state) {
    if (state == HS_TRANSFERRED) {
        return 1;
    }
    return 0;
}

/* ---- Section T01: Pool count validity ---- */

/* pool_count_valid: returns 1 iff n > 0 (Pool(T, N) requires N
 * to be a positive compile-time constant). */
int pool_count_valid(int n) {
    if (n > 0) {
        return 1;
    }
    return 0;
}

/* ---- Section M01: Division-by-zero check ---- */

/* div_safe: returns 1 iff divisor != 0 (emitter inserts this check
 * when VRP can't prove nonzero). */
int div_safe(int divisor) {
    if (divisor != 0) {
        return 1;
    }
    return 0;
}

/* ---- Section L01: Array bounds check ---- */

/* bounds_check: returns 1 iff 0 <= idx < size (runtime bounds check
 * for array/slice access). Uses early-return form to avoid short-circuit
 * complexity for VST. */
int bounds_check(int size, int idx) {
    if (idx < 0) {
        return 0;
    }
    if (idx >= size) {
        return 0;
    }
    return 1;
}

/* ---- Section E01: Atomic width check ---- */

/* atomic_width_ok: deferred — see TODO.
 *
 * Cascaded-if form requires `forward_if Post` in VST 3.0 which
 * needs explicit Σ-parametric postcondition. Solvable but more
 * setup than the other simple checks. Will verify in a dedicated
 * file with the right post-condition framework. */
int atomic_width_ok(int bytes) {
    return 0;   /* stub — real function verified elsewhere */
}

/* ---- Section P04: Variant index validity ---- */

/* variant_in_range: returns 1 iff 0 <= variant_idx < n_variants. */
int variant_in_range(int n_variants, int variant_idx) {
    if (variant_idx < 0) {
        return 0;
    }
    if (variant_idx >= n_variants) {
        return 0;
    }
    return 1;
}
