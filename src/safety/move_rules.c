/* src/safety/move_rules.c — move-struct safety predicates.
 *
 * See move_rules.h. Pure predicates matched by Coq specs in
 * proofs/vst/verif_move_rules.v. Oracle: lambda_zer_move.
 */
#include "move_rules.h"

int zer_type_kind_is_move_struct(int type_kind, int is_move_flag) {
    if (type_kind != ZER_TK_STRUCT) {
        return 0;
    }
    if (is_move_flag == 0) {
        return 0;
    }
    return 1;
}

int zer_move_should_track(int is_move_struct_direct, int contains_move_field) {
    if (is_move_struct_direct != 0) {
        return 1;
    }
    if (contains_move_field != 0) {
        return 1;
    }
    return 0;
}
