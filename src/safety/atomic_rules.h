/* src/safety/atomic_rules.h
 *
 * Pure predicates for @atomic_* intrinsic safety (typing.v Section E).
 * Linked into zerc, VST-verified in proofs/vst/verif_atomic_rules.v
 * against oracle proofs/operational/lambda_zer_typing/typing.v Section E.
 *
 * Oracle rules:
 *   E01: @atomic_* operand width must be 1, 2, 4, or 8 bytes
 *        (hardware-supported atomic sizes on all targets)
 *   E02: @atomic_* first arg must be pointer-to-integer
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_ATOMIC_RULES_H
#define ZER_SAFETY_ATOMIC_RULES_H

/* Returns 1 iff `bytes` is a valid atomic width (1, 2, 4, or 8).
 * Oracle: atomic_width_valid in typing.v Section E (E01). */
int zer_atomic_width_valid(int bytes);

/* Returns 1 iff the first argument of @atomic_* is a pointer to an
 * integer type.
 *
 * Args:
 *   is_ptr_to_int: 1 if caller determined arg is *T with T integer,
 *                  0 otherwise
 *
 * Oracle: atomic_arg_valid in typing.v Section E (E02). Trivial but
 * documents the rule. */
int zer_atomic_arg_is_ptr_to_int(int is_ptr_to_int);

/* E03 — @atomic_* on packed struct field rejected (misalignment hazard).
 * Oracle: typing.v:1158 atomic_on_packed_valid.
 * Caller: checker.c NODE_INTRINSIC @atomic_* on &packed_field. */
int zer_atomic_on_packed_valid(int is_packed_field);

/* E04 — @cond_wait/signal/broadcast first arg must be shared struct.
 * Oracle: typing.v:1166 condvar_arg_valid. */
int zer_condvar_arg_valid(int is_shared_struct);

/* E08 — Sync primitives (Semaphore/Barrier/Mutex) inside packed struct rejected.
 * Oracle: typing.v:1178 sync_in_packed_valid. */
int zer_sync_in_packed_valid(int is_packed_container);

#endif
