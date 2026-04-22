/* src/safety/context_bans.h
 *
 * Pure predicates for control-flow statements allowed in given contexts.
 * Linked into zerc, VST-verified in proofs/vst/verif_context_bans.v.
 *
 * Context flags (all passed as 0 or positive int):
 *   defer_depth    — nesting depth inside `defer { ... }` bodies
 *   critical_depth — nesting depth inside `@critical { ... }` bodies
 *   in_loop        — 0 = outside any loop, nonzero = inside for/while/do-while
 *   in_naked       — 0 = regular function, nonzero = `naked` function (asm-only)
 *
 * Rules (from CLAUDE.md "Ban Decision Framework"):
 *   - return: banned in defer (corrupts defer cleanup) and @critical
 *             (skips interrupt re-enable). Else allowed.
 *   - break/continue: banned in defer, @critical. Must be in_loop.
 *   - goto: banned in defer and @critical (corrupts cleanup / skip interrupt).
 *   - defer: banned inside another defer (nesting confuses intent).
 *   - asm: only allowed inside `naked` functions (no prologue/epilogue).
 *
 * See docs/formal_verification_plan.md Level 3 + BUGS-FIXED.md
 * "Gemini red-team audit" for audit-before-extraction discipline.
 */
#ifndef ZER_SAFETY_CONTEXT_BANS_H
#define ZER_SAFETY_CONTEXT_BANS_H

/* Returns 1 iff `return` is allowed: not inside defer, not inside @critical. */
int zer_return_allowed_in_context(int defer_depth, int critical_depth);

/* Returns 1 iff `break` is allowed: in a loop AND not in defer AND not in @critical. */
int zer_break_allowed_in_context(int defer_depth, int critical_depth, int in_loop);

/* Returns 1 iff `continue` is allowed: same conditions as break. */
int zer_continue_allowed_in_context(int defer_depth, int critical_depth, int in_loop);

/* Returns 1 iff `goto` is allowed: not inside defer, not inside @critical. */
int zer_goto_allowed_in_context(int defer_depth, int critical_depth);

/* Returns 1 iff new `defer { ... }` is allowed: not already inside a defer. */
int zer_defer_allowed_in_context(int defer_depth);

/* Returns 1 iff inline `asm` is allowed: only inside `naked` functions. */
int zer_asm_allowed_in_context(int in_naked);

#endif
