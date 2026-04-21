/* src/safety/coerce_rules.c — coercion rule predicates.
 *
 * See coerce_rules.h. Pure predicates matched by Coq specs in
 * proofs/vst/verif_coerce_rules.v. Linked into zerc via Makefile.
 */
#include "coerce_rules.h"

int zer_coerce_int_widening_allowed(int from_signed, int to_signed,
                                      int from_width, int to_width) {
    /* Flattened if-return chain for VST simplicity (no nested ifs). */
    /* Must be strictly widening — same-width handled elsewhere. */
    if (from_width >= to_width) {
        return 0;
    }
    /* Same sign + widening: always OK. */
    if (from_signed == to_signed) {
        return 1;
    }
    /* Different signs. Only OK case: unsigned → larger signed. */
    if (from_signed == 0) {
        return 1;
    }
    /* signed → unsigned: NOT allowed (sign bit interpretation differs). */
    return 0;
}

int zer_coerce_usize_same_width_allowed(int from_is_usize, int to_is_usize,
                                          int from_signed, int to_signed) {
    /* Fully flattened early-return chain. */
    if (from_signed != to_signed) {
        return 0;
    }
    if (from_is_usize != 0) {
        return 1;   /* from is USIZE — allowed */
    }
    if (to_is_usize != 0) {
        return 1;   /* to is USIZE — allowed */
    }
    return 0;       /* neither is USIZE — banned */
}

int zer_coerce_float_widening_allowed(int from_is_f32, int to_is_f64) {
    /* Flattened — no compound condition for VST. */
    if (from_is_f32 == 0) {
        return 0;
    }
    if (to_is_f64 == 0) {
        return 0;
    }
    return 1;
}

int zer_coerce_preserves_volatile(int from_volatile, int to_volatile) {
    /* If source had no volatile, nothing to preserve — always OK. */
    if (from_volatile == 0) {
        return 1;
    }
    /* Source had volatile. Destination must also have it. */
    if (to_volatile == 0) {
        return 0;
    }
    return 1;
}

int zer_coerce_preserves_const(int from_const, int to_const) {
    if (from_const == 0) {
        return 1;
    }
    if (to_const == 0) {
        return 0;
    }
    return 1;
}
