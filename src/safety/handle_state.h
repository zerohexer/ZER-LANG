/* src/safety/handle_state.h
 *
 * Shared safety predicates on handle state values.
 *
 * Functions here are:
 *   1. LINKED into `zerc` (part of the real compiler — no duplicate).
 *   2. VERIFIED by VST in proofs/vst/verif_handle_state.v against a
 *      Coq spec that mirrors the predicate in
 *      proofs/operational/lambda_zer_typing/typing.v.
 *
 * The same .c file that `make zerc` compiles is what VST verifies via
 * `make check-vst` (CompCert clightgen processes the same source).
 * If someone modifies handle_state.c in a way that breaks the spec,
 * `make check-vst` fails — the correctness-oracle loop closes.
 *
 * See docs/formal_verification_plan.md Level 3 section.
 */
#ifndef ZER_SAFETY_HANDLE_STATE_H
#define ZER_SAFETY_HANDLE_STATE_H

/* Handle state values.
 *
 * The integer values MUST match HS_* in zercheck.h and IR_HS_* in
 * zercheck_ir.c. Changing order or adding a state requires syncing
 * all three places AND updating the Coq spec + VST proof. */
#define ZER_HS_UNKNOWN      0
#define ZER_HS_ALIVE        1
#define ZER_HS_FREED        2
#define ZER_HS_MAYBE_FREED  3
#define ZER_HS_TRANSFERRED  4

/* Returns 1 iff `state` is one in which any use or free is invalid.
 * FREED, MAYBE_FREED, TRANSFERRED all return 1; UNKNOWN and ALIVE
 * return 0.
 *
 * Callers:
 *   - zercheck.c:is_handle_invalid (dispatches to this)
 *   - zercheck_ir.c:ir_is_invalid  (dispatches to this)
 *
 * Corresponds to predicate is_invalid_state in
 * proofs/operational/lambda_zer_typing/typing.v. */
int zer_handle_state_is_invalid(int state);

#endif
