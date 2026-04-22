/* src/safety/concurrency_rules.c — Phase 1 Batches 9-11
 *
 * Concurrency safety predicates (typing.v sections C, D, F).
 * Linked into zerc, VST-verified in proofs/vst/verif_concurrency_rules.v.
 *
 * Oracle tier: schematic — typing.v has real Coq theorems for all 11 but
 * no operational Iris subset yet (Phase 7 upgrades to operational).
 *
 * Oracle rules:
 *   C01/C02 thread_op_valid         — ThreadHandle alive/joined state machine
 *   C01     thread_cleanup_valid    — unjoined handle at scope exit = leak
 *   C03/C04/C05 spawn_context_valid — spawn banned in ISR/@critical/async
 *   C07     spawn_return_safe       — spawn target cannot return resource
 *   C09     spawn_arg_valid         — spawn arg must be shared ptr OR value
 *   C10     spawn_arg_is_handle     — Handle to spawn rejected
 *   D01     address_of_shared_valid — cannot take &shared_field
 *   D02     shared_in_suspend_valid — lock cannot be held across yield/await
 *   D04     volatile_compound_valid — volatile + compound RMW = race
 *   D05     isr_main_access_valid   — ISR+main access requires volatile
 *   F01-F04 yield_context_valid     — yield/await only in async, not in critical/defer
 */
#include "concurrency_rules.h"

/* C01/C02 — ThreadHandle state machine. joining=1 attempts join. */
int zer_thread_op_valid(int state, int joining) {
    if (joining == 0) {
        return 1;
    }
    if (state == ZER_THR_ALIVE) {
        return 1;
    }
    return 0;
}

/* C01 — unjoined thread at scope exit = leak. */
int zer_thread_cleanup_valid(int state) {
    if (state == ZER_THR_JOINED) {
        return 1;
    }
    return 0;
}

/* C03/C04/C05 — full spawn context check. */
int zer_spawn_context_valid(int in_isr, int in_critical, int in_async) {
    if (in_isr != 0) {
        return 0;
    }
    if (in_critical != 0) {
        return 0;
    }
    if (in_async != 0) {
        return 0;
    }
    return 1;
}

/* C07 — spawn target returning Handle or move struct = resource leak. */
int zer_spawn_return_safe(int returns_resource) {
    if (returns_resource != 0) {
        return 0;
    }
    return 1;
}

/* C09 — spawn arg must be shared pointer OR value (not non-shared ptr). */
int zer_spawn_arg_valid(int is_shared_ptr, int is_value) {
    if (is_shared_ptr != 0) {
        return 1;
    }
    if (is_value != 0) {
        return 1;
    }
    return 0;
}

/* C10 — Handle to spawn rejected (Pool not thread-safe). */
int zer_spawn_arg_is_handle_rejected(int is_handle) {
    if (is_handle != 0) {
        return 0;
    }
    return 1;
}

/* D01 — cannot take &(shared field) — bypasses auto-lock. */
int zer_address_of_shared_valid(int is_shared_field) {
    if (is_shared_field != 0) {
        return 0;
    }
    return 1;
}

/* D02 — lock cannot be held across yield/await.
 * Flat cascade: valid iff NOT (accesses_shared AND has_yield). */
int zer_shared_in_suspend_valid(int accesses_shared, int has_yield) {
    if (accesses_shared == 0) {
        return 1;
    }
    if (has_yield == 0) {
        return 1;
    }
    return 0;
}

/* D04 — volatile + compound RMW = non-atomic race.
 * Flat cascade: valid iff NOT (is_volatile AND is_compound_op). */
int zer_volatile_compound_valid(int is_volatile, int is_compound_op) {
    if (is_volatile == 0) {
        return 1;
    }
    if (is_compound_op == 0) {
        return 1;
    }
    return 0;
}

/* D05 — globals accessed from ISR+main MUST be volatile. */
int zer_isr_main_access_valid(int accessed_in_isr, int accessed_in_main, int is_volatile) {
    if (is_volatile != 0) {
        return 1;
    }
    if (accessed_in_isr == 0) {
        return 1;
    }
    if (accessed_in_main == 0) {
        return 1;
    }
    return 0;
}

/* F01-F04 — yield/await context: in_async required, not in critical/defer. */
int zer_yield_context_valid(int in_async, int in_critical, int in_defer) {
    if (in_async == 0) {
        return 0;
    }
    if (in_critical != 0) {
        return 0;
    }
    if (in_defer != 0) {
        return 0;
    }
    return 1;
}
