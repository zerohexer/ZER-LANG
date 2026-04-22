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

/* T02/T03 — Pool/Ring/Slab must be at global scope, not struct field or
 * union variant. Input encoding: 0=DeclGlobal (OK), 1=DeclField (reject),
 * 2=DeclVariant (reject). */
int zer_container_position_valid(int decl_position) {
    if (decl_position == ZER_DP_GLOBAL) {
        return 1;
    }
    return 0;
}

/* T04 — Handle(T) element type must be struct (not primitive or void).
 * Input encoding: 0=ElemStruct (OK), 1=ElemPrim (reject), 2=ElemVoid (reject). */
int zer_handle_element_valid(int element_kind) {
    if (element_kind == ZER_HE_STRUCT) {
        return 1;
    }
    return 0;
}

/* K01 — @container source must be pointer type. Other categories rejected.
 * Input encoding: 0=CatPrim, 1=CatPtr (OK), 2=CatSlice, 3=CatStruct. */
int zer_container_source_valid(int type_category) {
    if (type_category == ZER_TCAT_PTR) {
        return 1;
    }
    return 0;
}
