/* src/safety/isr_rules.h
 *
 * Phase 2 Batch 1: hardware-context ban rules (decision extraction).
 *
 * Oracle: CLAUDE.md "Ban Decision Framework" — hardware constraint
 * bans (malloc in ISR deadlocks, pthread_create with interrupts
 * disabled is hardware-unsafe). typing.v sections C03-C04, S04-S05.
 *
 * Callers: checker.c check_isr_ban + slab/pool alloc sites, spawn
 * in ISR/@critical ban sites, delete in ISR sites.
 *
 * See docs/formal_verification_plan.md Phase 2 section.
 */
#ifndef ZER_SAFETY_ISR_RULES_H
#define ZER_SAFETY_ISR_RULES_H

/* Returns 1 iff heap-allocation (slab/Task .alloc) is allowed.
 * Banned in interrupt handlers — malloc/calloc may deadlock if the
 * heap mutex was already held when the interrupt fired.
 *
 * Arg (0 or 1):
 *   in_interrupt: 1 if currently inside an `interrupt` function body */
int zer_alloc_allowed_in_isr(int in_interrupt);

/* Returns 1 iff heap-allocation is allowed given @critical depth.
 * Banned inside `@critical { ... }` blocks — with interrupts disabled,
 * a blocking allocator would deadlock the OS.
 *
 * Arg:
 *   critical_depth: current @critical nesting depth (0 = outside) */
int zer_alloc_allowed_in_critical(int critical_depth);

/* Returns 1 iff `spawn` (thread creation) is allowed in the current
 * interrupt context. Same rule as alloc — pthread_create in ISR is
 * a hardware-unsafe operation. */
int zer_spawn_allowed_in_isr(int in_interrupt);

/* Returns 1 iff `spawn` is allowed given @critical depth.
 * Banned inside @critical because creating a thread with interrupts
 * disabled corrupts the scheduler state. */
int zer_spawn_allowed_in_critical(int critical_depth);

#endif
