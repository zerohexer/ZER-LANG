/* src/safety/misc_rules.c — final Phase 1 predicates.
 *
 * See misc_rules.h. Pure predicates matched by Coq specs in
 * proofs/vst/verif_misc_rules.v.
 */
#include "misc_rules.h"

int zer_int_switch_has_default(int has_default_flag) {
    if (has_default_flag == 0) {
        return 0;
    }
    return 1;
}

int zer_bool_switch_covers_both(int has_default, int has_true, int has_false) {
    /* Flat cascade — any single condition is sufficient to OK/reject. */
    if (has_default != 0) {
        return 1;   /* default covers everything */
    }
    if (has_true == 0) {
        return 0;   /* missing true arm */
    }
    if (has_false == 0) {
        return 0;   /* missing false arm */
    }
    return 1;
}
