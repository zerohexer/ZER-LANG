/* src/safety/atomic_rules.c — atomic-intrinsic safety predicates.
 *
 * See atomic_rules.h. Oracle: typing.v Section E (E01, E02).
 * VST proofs in proofs/vst/verif_atomic_rules.v.
 */
#include "atomic_rules.h"

int zer_atomic_width_valid(int bytes) {
    if (bytes == 1) {
        return 1;
    }
    if (bytes == 2) {
        return 1;
    }
    if (bytes == 4) {
        return 1;
    }
    if (bytes == 8) {
        return 1;
    }
    return 0;
}

int zer_atomic_arg_is_ptr_to_int(int is_ptr_to_int) {
    if (is_ptr_to_int == 0) {
        return 0;
    }
    return 1;
}
