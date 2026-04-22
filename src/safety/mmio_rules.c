/* src/safety/mmio_rules.c — @inttoptr MMIO safety predicates.
 *
 * See mmio_rules.h. Pure predicates matched by Coq specs in
 * proofs/vst/verif_mmio_rules.v. Oracle: lambda_zer_mmio.
 */
#include "mmio_rules.h"

int zer_mmio_addr_in_range(int addr, int start, int end) {
    /* Inclusive range: addr ∈ [start, end]. */
    if (addr < start) {
        return 0;
    }
    if (addr > end) {
        return 0;
    }
    return 1;
}

int zer_mmio_inttoptr_allowed(int in_any_range, int aligned) {
    /* Oracle: step_inttoptr_ok requires BOTH. */
    if (in_any_range == 0) {
        return 0;
    }
    if (aligned == 0) {
        return 0;
    }
    return 1;
}
