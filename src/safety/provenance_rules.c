/* src/safety/provenance_rules.c — @ptrcast provenance predicates.
 *
 * See provenance_rules.h. Oracle: lambda_zer_opaque/iris_opaque_specs.v.
 * VST proofs in proofs/vst/verif_provenance_rules.v.
 */
#include "provenance_rules.h"

int zer_provenance_check_required(int src_prov_unknown, int dst_is_opaque) {
    /* Skip check on unknown source (C interop, documented design). */
    if (src_prov_unknown != 0) {
        return 0;
    }
    /* Skip check on opaque target (upcast — always safe per oracle). */
    if (dst_is_opaque != 0) {
        return 0;
    }
    /* Both ends concrete — structural match required. */
    return 1;
}

int zer_provenance_type_ids_compatible(int actual_id, int expected_id) {
    /* 0 = unknown (e.g., from cinclude) — accept per documented design. */
    if (actual_id == 0) {
        return 1;
    }
    /* Same ID — typed_ptr_agree matches. */
    if (actual_id == expected_id) {
        return 1;
    }
    return 0;
}

int zer_provenance_opaque_upcast_allowed(void) {
    /* Oracle: step_spec_opaque_cast is identity on the tag.
     * *T → *opaque never fails. */
    return 1;
}
