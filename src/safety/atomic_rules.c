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

/* E03 — @atomic_* on packed struct field. Packed fields may be misaligned;
 * GCC __atomic builtins require natural alignment. Reject. */
int zer_atomic_on_packed_valid(int is_packed_field) {
    if (is_packed_field != 0) {
        return 0;
    }
    return 1;
}

/* E04 — @cond_wait/signal/broadcast first arg must be a shared struct.
 * Condvar operations need the auto-locking mutex of a shared struct. */
int zer_condvar_arg_valid(int is_shared_struct) {
    if (is_shared_struct == 0) {
        return 0;
    }
    return 1;
}

/* E08 — Semaphore/Barrier/Mutex inside packed container. Same alignment
 * issue as E03 — pthread_mutex_t requires natural alignment. */
int zer_sync_in_packed_valid(int is_packed_container) {
    if (is_packed_container != 0) {
        return 0;
    }
    return 1;
}
