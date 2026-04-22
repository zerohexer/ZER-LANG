/* src/safety/range_checks.c — range-validity predicates.
 *
 * See range_checks.h for design. Self-contained: no includes beyond
 * this header. Each function is pure, primitive-typed, and matches
 * a Coq spec in proofs/vst/verif_range_checks.v.
 */
#include "range_checks.h"

int zer_count_is_positive(int n) {
    if (n > 0) {
        return 1;
    }
    return 0;
}

int zer_index_in_bounds(int size, int idx) {
    if (idx < 0) {
        return 0;
    }
    if (idx >= size) {
        return 0;
    }
    return 1;
}

int zer_variant_in_range(int n_variants, int variant_idx) {
    if (variant_idx < 0) {
        return 0;
    }
    if (variant_idx >= n_variants) {
        return 0;
    }
    return 1;
}

int zer_slice_bounds_valid(int size, int start, int end_) {
    if (start > end_) {
        return 0;
    }
    if (end_ > size) {
        return 0;
    }
    return 1;
}

int zer_bit_index_valid(int width, int idx) {
    if (idx < 0) {
        return 0;
    }
    if (idx >= width) {
        return 0;
    }
    return 1;
}
