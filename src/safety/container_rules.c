/* src/safety/container_rules.c — container/field well-formedness.
 *
 * See container_rules.h. Oracle: typing.v (T, K sections).
 * VST proofs in proofs/vst/verif_container_rules.v.
 */
#include "container_rules.h"

int zer_container_depth_valid(int depth) {
    if (depth < 0) {
        return 0;
    }
    if (depth >= ZER_CONTAINER_DEPTH_LIMIT) {
        return 0;
    }
    return 1;
}

int zer_field_type_valid(int is_void) {
    if (is_void != 0) {
        return 0;
    }
    return 1;
}

int zer_type_has_size(int is_void) {
    if (is_void != 0) {
        return 0;
    }
    return 1;
}
