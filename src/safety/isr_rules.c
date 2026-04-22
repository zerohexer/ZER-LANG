/* src/safety/isr_rules.c — Phase 2 Batch 1: ISR / @critical bans.
 *
 * See isr_rules.h. Oracle: CLAUDE.md "Ban Decision Framework"
 * + typing.v C03/C04/S04/S05.
 */
#include "isr_rules.h"

int zer_alloc_allowed_in_isr(int in_interrupt) {
    if (in_interrupt != 0) {
        return 0;   /* banned — hardware constraint */
    }
    return 1;
}

int zer_alloc_allowed_in_critical(int critical_depth) {
    if (critical_depth > 0) {
        return 0;   /* banned — interrupts disabled */
    }
    return 1;
}

int zer_spawn_allowed_in_isr(int in_interrupt) {
    if (in_interrupt != 0) {
        return 0;
    }
    return 1;
}

int zer_spawn_allowed_in_critical(int critical_depth) {
    if (critical_depth > 0) {
        return 0;
    }
    return 1;
}
