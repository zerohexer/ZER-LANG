/* src/safety/provenance_rules.h
 *
 * Pure predicates for @ptrcast / *opaque provenance tracking.
 * Linked into zerc, VST-verified in proofs/vst/verif_provenance_rules.v
 * against the λZER-opaque operational oracle
 * (proofs/operational/lambda_zer_opaque/iris_opaque_specs.v).
 *
 * Oracle rules (from typed_ptr_agree + step_spec_typed_cast):
 *   - Casting *T → *opaque: always safe (resource retained, tag unchanged)
 *   - Casting *opaque → *T': requires typed_ptr γ id T' (i.e. the pointer
 *     was originally allocated as T'). Tag mismatch = stuck (compile error).
 *   - Unknown provenance (type_id == 0, from cinclude / extern C): accepted
 *     as an explicit documented limitation (C interop boundary).
 *
 * See docs/formal_verification_plan.md Level 3.
 */
#ifndef ZER_SAFETY_PROVENANCE_RULES_H
#define ZER_SAFETY_PROVENANCE_RULES_H

/* Returns 1 iff a provenance type-match check is needed for this @ptrcast.
 *
 * Args (0 or 1):
 *   src_prov_unknown: 1 if source pointer's provenance is unknown
 *                     (e.g., from cinclude / extern C), 0 if tracked
 *   dst_is_opaque:    1 if cast target is *opaque, 0 if concrete *T
 *
 * Rule: check is needed ONLY if BOTH src is known AND dst is concrete.
 * Any skip path corresponds to a valid step in the Iris proof:
 *   - Unknown src → C interop boundary, accepted (documented design)
 *   - Opaque dst → upcast, always safe (step_spec_opaque_cast) */
int zer_provenance_check_required(int src_prov_unknown, int dst_is_opaque);

/* Returns 1 iff two type_id tags are compatible (same type OR at least
 * one is 0 = unknown).
 *
 * This matches the emitter's runtime check pattern:
 *   if (actual.type_id != expected && actual.type_id != 0) trap();
 *
 * Maps to the Iris lemma typed_ptr_agree: owning typed_ptr γ id t and
 * typed_ptr γ id t' forces t = t' (ghost-map exclusivity). The 0-case
 * is ZER's runtime escape hatch for C interop. */
int zer_provenance_type_ids_compatible(int actual_id, int expected_id);

/* Returns 1 iff `*T → *opaque` upcast is allowed.
 *
 * Always 1 — per step_spec_opaque_cast in the oracle, opaque upcast is
 * a no-op at the resource level. Encoding as a function locks the rule
 * behind VST verification so it can't silently be changed. */
int zer_provenance_opaque_upcast_allowed(void);

#endif
