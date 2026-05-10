# Refactor IR — zercheck_ir.c Helper-Layer Restoration

**Status:** Planning document. Not yet executed. Re-audited 2026-05-10.
**Date:** 2026-05-05 (drafted), 2026-05-10 (audit + corrections)
**Scope:** Internal architectural cleanup of `zercheck_ir.c`. No user-facing
behavior changes (with 6 acknowledged lattice cells changing in formally-
correct direction — see section 6.1). No new safety properties.
Maintainability-only refactor.
**Effort:** ~6-8 hrs implementation + tests + audit script.
**Net LOC:** -250 to -300 (consolidating 50-55 sites into 6 helpers).
**Risk:** LOW per helper — all 6 patterns have established precedent
elsewhere in the codebase AND in production compilers (Rust rustc,
Swift SIL, Clang static analyzer).

**Architectural certainty:** The 5 helpers map 1:1 to patterns used by
every production flow analyzer. `ir_state_join` is the C expression of
Rust's `JoinSemiLattice::join` trait method. `ir_init_handle` is the
factory pattern used by Swift SIL. `ir_report_invalid_use` mirrors
Rust's `Session::struct_span_err` builder. We're not inventing — we're
translating language-level patterns Rust gets for free (trait methods,
exhaustive match) into C with an audit script substituting for
compile-time exhaustiveness.

**Audit findings (2026-05-10):**
- Init-site count was undercounted: claimed 17, actually 22-25.
  Helper consolidation is BETTER than predicted (more LOC reduction).
- Alias-copy site classification was conflated: claimed 8, actually
  **7** true-alias + 2 init-with-fresh-id (the 2 are covered by
  `ir_init_handle`, not this helper). The 7th true-alias site at
  L1882 was missed by the original audit and discovered independently
  on branch `claude/cool-johnson-MLXDT` (BUG-660) on the same date.
- Error site count was rough: claimed 18, actually 25 across 3 tiers
  (16 "use" + 4 "return" + 5 "freeing").
- Lattice table makes 6 cells differ from current behavior (4 in UNK
  row + 2 defensive XFER↔FREED). All 6 unreachable today.

**Cross-validation (2026-05-10): bug rate accelerating.** A separate
audit on branch `claude/cool-johnson-MLXDT` discovered BUG-660 — the
**7th instance of the same alias-copy field-drift class** in under
2 weeks (BUG-468/469, BUG-650, F0.3, F3.2 #1, F3.2 #2, latent IR_CAST,
BUG-660). The refactor's `ir_alias_copy_provenance` would catch all 7
by construction. Without it, the bug rate continues — MLXDT branch
patched 1 site manually, leaving the underlying gap unaddressed.

**Decisions made (2026-05-10) — no remaining ambiguity:**

- **Q1 (lattice cells):** formally-correct lattice. All 6 cells take
  their formally-correct value, not preserved cascade behavior. Encoding
  cascade asymmetry into a lookup table would memorialize a fall-through
  accident — a real semilattice is what every production compiler uses.

- **Q7 (error reporter scope):** fold all 25 sites via `IRUseContext`
  enum (READ/RETURN/FREE). Rust's E0382 covers the same shape — single
  error code with `MoveOutAction` enum dispatching wording. Splitting
  forces 3 places to update when a state is added.

These were the only two open questions. Plan is now fully concrete:
6 helpers, 1 audit script, 50-55 sites consolidated, ~300 net LOC
reduction, all decisions backed by production-compiler precedent.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Why This Refactor Exists — Bug History](#2-why-this-refactor-exists--bug-history)
3. [Architectural Principle Being Restored](#3-architectural-principle-being-restored)
4. [Codebase-Wide Precedents](#4-codebase-wide-precedents)
5. [Detailed Gap Analysis](#5-detailed-gap-analysis)
6. [Per-Helper Specifications](#6-per-helper-specifications)
   - 6.1 [`ir_state_join` — lattice merge table](#61-ir_state_join--lattice-merge-table)
   - 6.2 [`ir_alias_copy_provenance` — alias field copier](#62-ir_alias_copy_provenance--alias-field-copier)
   - 6.3 [`ir_init_handle` — handle factory](#63-ir_init_handle--handle-factory)
   - 6.4 [`ir_record_freed` / `ir_record_transferred` — state transitions](#64-ir_record_freed--ir_record_transferred--state-transitions)
   - 6.5 [`ir_report_invalid_use` — central error reporter](#65-ir_report_invalid_use--central-error-reporter)
7. [Audit Tooling](#7-audit-tooling)
8. [Implementation Strategy](#8-implementation-strategy)
9. [Testing Strategy](#9-testing-strategy)
10. [Risks and Mitigations](#10-risks-and-mitigations)
11. [Open Questions](#11-open-questions)
12. [Out of Scope (Explicitly NOT Doing)](#12-out-of-scope-explicitly-not-doing)
13. [Appendix A: Illustrative Code Samples](#appendix-a-illustrative-code-samples)
14. [Appendix B: Bug History Mapped to Helpers](#appendix-b-bug-history-mapped-to-helpers)
15. [Appendix C: Site Inventory (Quantitative)](#appendix-c-site-inventory-quantitative)
16. [Appendix D: Architectural Comparison Table](#appendix-d-architectural-comparison-table)
17. [Appendix E: References](#appendix-e-references)

---

## 1. Executive Summary

`zercheck_ir.c` is the production safety analyzer for ZER (since Phase F1
migration on 2026-05-03 — see `BUGS-FIXED.md` "Session 2026-05-03 Phase F
Migration"). It is **architecturally MORE sophisticated than the AST analyzer
it replaced** — CFG-aware merge, compound keys, dedicated walkers, method
classification — but **it regressed in the helper-layer dimension**.

The deleted `zercheck.c` had a deliberately-designed unified-helper layer
(comment exists in the deleted source and is preserved in git history under
commit `7ffbd9d^`):

> *"These prevent the BUG-468/469 class of bugs where a new state or type
> pattern is added but not propagated to all check/merge sites."*

`zercheck_ir.c` only ported about 60% of those helpers. As a result, in the
~3 weeks since IR became primary, **6 documented bugs + 1 latent**
have been hit, all from the same helper-layer gaps:

- 4 lattice merge cascade bugs (BUG-650 #1, F0.3, F3.2 #1, plus an earlier
  BUG-650 add-cases fix)
- 1 alias-copy field drift (F3.2 #2 — `pool_name` only added to 2 of 8
  alias sites)
- 1 latent (alias-copy `pool_name` would also miss IR_CAST/@ptrcast/&-of
  paths if reached for a Handle, but the type-gating in those sites makes
  this unreachable today — still a maintenance hazard)

These are not random bugs — they share root cause. The codebase already
has the pattern that prevents them, demonstrated in:

- `checker.c`: `propagate_escape_flags` — explicit comment "Centralizes the
  3-flag propagation pattern that was scattered at 5+ sites (4 of which
  were missing the type guard)."
- `types.c`: `scope_add` — memset-zero factory pattern for Symbol creation.
- `zercheck_ir.c` itself: `ir_ps_init/copy/free` — three-function suite
  for IRPathState. This works.
- `tools/walker_audit.sh`, `tools/walker_default_audit.sh` — mechanical
  bug-class prevention via audit scripts.
- `src/safety/escape_rules.c` etc. — VST-verified single-source predicates.

**The refactor restores zercheck_ir.c to architectural parity with the rest
of the codebase.** No new design — every shape exists elsewhere already.

The result is **6 helper functions + 1 audit script**. They consolidate
~62 scattered call sites and prevent the entire class of "new state/field/
transition pattern misses some sites" bugs.

---

## 2. Why This Refactor Exists — Bug History

### The recurring bug class

Over a 6-week period, the merge-related code in `zercheck_ir.c` has been
patched 4 times and the alias-copy code 1 time. All 5 fixes share the same
root cause class: **a finite enumeration (states, fields, ops) was
hand-coded as a cascade or scattered repetition, and a case was missed**.

| # | Date | Bug ID | Fix location | Class |
|---|---|---|---|---|
| 1 | 2026-05-02 | BUG-650 (cases) | `ir_merge_states` | Missing case in lattice cascade — added `FREED ↔ MAYBE_FREED` |
| 2 | 2026-05-02 | BUG-650 (API) | `ir_merge_states` | Wrong API choice — `ir_find_handle` (bare-only) used in compound context |
| 3 | 2026-05-03 | F0.3 | convergence check | Same wrong-API as #2 |
| 4 | 2026-05-04 | F3.2 (cascade) | `ir_merge_states` | Missing case `ALIVE+MAYBE_FREED` and `TRANSFERRED+MAYBE_FREED` |
| 5 | 2026-05-04 | F3.2 (alias) | IR_COPY + orelse-ident | New field `pool_name` added to 2 of 8 alias-copy sites |

**Bug rate per analyzer-week**: 5 bugs / 3 weeks = ~1.7 bugs/wk in the
helper-layer, sustained.

### Inherited from AST predecessor

The deleted `zercheck.c` (commit `7ffbd9d^`) had a section explicitly
labeled `/* ---- Unified helpers (Option A refactor) ---- */` with this
comment:

```c
/* These prevent the BUG-468/469 class of bugs where a new state or type
 * pattern is added but not propagated to all check/merge sites.
 * New states: add to is_handle_invalid + is_handle_consumed.
 * New move-like types: add to should_track_move.
 * Error messages: update zc_report_invalid_use ONCE. */
```

This was a **conscious refactor** done after BUG-468/469 in AST analyzer.
Ported helpers to `zercheck_ir.c`:
- ✓ `is_handle_invalid` → `ir_is_invalid`
- ✓ `is_move_struct_type` → `ir_is_move_struct_type`
- ✓ `contains_move_struct_field` → `ir_contains_move_struct_field`
- ✓ `should_track_move` → `ir_should_track_move`

Helpers NOT ported (the gap that recurs):
- ✗ `is_handle_consumed` — companion to `is_handle_invalid` for path-merge
- ✗ `zc_report_invalid_use` — central error-message reporter
- ✗ Implicit state-JOIN logic — never centralized in either AST or IR

### Why now, why this approach

The migration to IR-only happened on 2026-05-03 (Phase F1). At that point,
the IR analyzer became the sole exit-code driver. Before then, the AST
analyzer was running in parallel and catching some of these bugs by
different logic. After Phase F1, the IR analyzer is on its own.

The 5 bugs above all occurred AFTER Phase F1. The dual-run agreement
reporter (`tools/agreement_audit.sh`) was specifically built to find them
because they're now silent without the AST analyzer cross-check.

If we don't do this refactor, the bug rate continues. Each missing case
or scattered site creates the next entry in this table. The longer we wait,
the more the bug count grows and the harder full consolidation becomes.

---

## 3. Architectural Principle Being Restored

### The principle

**Every finite enumeration (states, fields, transitions, ops) that drives
analysis decisions should be expressed in a single source of truth, not
duplicated across consumer sites.**

When the source-of-truth and the consumer sites are tightly bound (e.g.,
the join function explicitly references each state pair, or one centralized
copy function references each field), adding a new element forces a single
update. When they're scattered (e.g., 7 if-else cases across one function,
or 8 hand-rolled field-copy blocks), adding a new element requires manual
discovery of all sites — and easy to miss one.

### When this matters

This pattern matters when:

1. **The set of cases is closed but might grow.** State enums, struct field
   sets, error-message variants — all can grow as new tracking dimensions
   are added.
2. **All consumers must be updated together.** If you add a state but only
   half the merge cases handle it, the analyzer is silently incorrect.
3. **The work at each consumer is mechanical.** A field-copy is just
   `dst->X = src->X`. A merge case is just a state pair → state mapping.
   Mechanical work duplicated across sites is the recipe for drift.

### When this does NOT matter

1. **Cases that don't share semantics.** If two sites that handle FREED
   have genuinely different logic (e.g., one suppresses error during summary
   build, one always reports), they can't share a helper.
2. **Decisions specific to context.** The 11 TRANSFERRED-setting sites
   currently don't all propagate to aliases — some intentionally don't (move
   struct uses fresh alloc_id, no aliases exist). Centralizing helps document
   the decision but doesn't change semantics.

### How the refactor expresses this principle

For each finite enumeration in `zercheck_ir.c`, create a single helper
that encapsulates the enumeration. Consumers call the helper instead of
restating the enumeration inline. Adding a new enum value updates exactly
one site — the helper.

The 5 enumerations identified:

1. **State pairs in lattice merge** (5×5 = 25) → `ir_state_join` table.
2. **Fields that propagate through aliases** (8 fields today: state,
   alloc_line, free_line, alloc_id, source_color, is_thread_handle,
   pool_name, pool_name_len) → `ir_alias_copy_provenance`.
3. **Allocation source kinds** (Pool, Slab, Arena, Malloc, Param, Move,
   Thread, Opaque) → `ir_init_handle` factory.
4. **State transitions with aliasing** (FREED, TRANSFERRED) →
   `ir_record_freed` / `ir_record_transferred`.
5. **Error messages for invalid handle use** (FREED, MAYBE_FREED,
   TRANSFERRED, plus thread-handle vs move-struct distinctions) →
   `ir_report_invalid_use`.

---

## 4. Codebase-Wide Precedents

The pattern is not new. Multiple parts of the codebase already implement
it. The refactor is a **completion** of an existing architectural idiom,
not a new design.

### Precedent 1: `propagate_escape_flags` in checker.c

Location: `checker.c:881`.

```c
/* ---- Unified escape flag helpers (prevents BUG-421 class) ---- */

/* Can this type carry a pointer? Only propagate escape flags to types that
 * can hold pointers (avoids spurious flags on integer assignments). */
/* ... */

/* Propagate is_local_derived / is_arena_derived / is_from_arena from src
 * to dst, but ONLY if dst's type can carry a pointer. Centralizes the
 * 3-flag propagation pattern that was scattered at 5+ sites (4 of which
 * were missing the type guard). */
static void propagate_escape_flags(Symbol *dst, Symbol *src, Type *dst_type) {
    if (!dst || !src || !type_can_carry_pointer(dst_type)) return;
    if (src->is_local_derived) dst->is_local_derived = true;
    if (src->is_arena_derived) dst->is_arena_derived = true;
    if (src->is_from_arena)    dst->is_from_arena    = true;
}
```

This is the **direct precedent** for `ir_alias_copy_provenance`. Same
problem (three flags scattered at multiple sites, four sites missing the
type guard), same fix (consolidate to single helper). Comment explicitly
attributes the bug class.

### Precedent 2: `scope_add` factory in types.c

Location: `types.c:499`.

```c
Symbol *scope_add(Arena *a, Scope *s, const char *name, uint32_t name_len,
                  Type *type, uint32_t line, const char *file) {
    /* check for redefinition in current scope */
    Symbol *existing = scope_lookup_local(s, name, name_len);
    if (existing) return NULL; /* caller handles error */

    /* grow if needed */
    if (s->symbol_count >= s->symbol_capacity) {
        uint32_t new_cap = s->symbol_capacity * 2;
        Symbol *new_syms = (Symbol *)arena_alloc(a, new_cap * sizeof(Symbol));
        memcpy(new_syms, s->symbols, s->symbol_count * sizeof(Symbol));
        s->symbols = new_syms;
        s->symbol_capacity = new_cap;
    }

    Symbol *sym = &s->symbols[s->symbol_count++];
    memset(sym, 0, sizeof(Symbol));     /* ← zero-init pattern */
    sym->name = name;                   /* ← set core fields */
    sym->name_len = name_len;
    sym->type = type;
    sym->line = line;
    sym->file = file;
    return sym;
}
```

Symbol has 30+ fields. `scope_add` zero-initializes the slot then sets
just the core fields. Callers set context-specific fields after. This is
the pattern for `ir_init_handle`.

The Symbol creation works because:
1. memset-zero gives sensible defaults (false, NULL, 0).
2. Core fields (name, type) are always set.
3. Context-specific fields are set per-caller.

### Precedent 3: `ir_ps_init` / `ir_ps_copy` / `ir_ps_free` in zercheck_ir.c

Location: `zercheck_ir.c:115-150`.

```c
static void ir_ps_init(IRPathState *ps) {
    ps->handles = NULL;
    ps->handle_count = 0;
    ps->handle_capacity = 0;
    ps->terminated = false;
    ps->critical_depth = 0;
    ps->threads = NULL;
    ps->thread_count = 0;
    ps->thread_capacity = 0;
}

static IRPathState ir_ps_copy(IRPathState *src) { /* ... */ }
static void ir_ps_free(IRPathState *ps) { /* ... */ }
```

The three IRPathState functions are kept in sync — adding a new dynamic
array (e.g., a `barriers` array for Phase 5 atomic ordering) means
updating all three. **This is the pattern that's correct for IRPathState
but missing for IRHandleInfo.** zercheck_ir already has the pattern; it
just hasn't applied it to handle-state operations.

### Precedent 4: VST-verified predicates in src/safety/

Location: `src/safety/handle_state.c`, `escape_rules.c`, etc.

```c
/* src/safety/handle_state.c */
int zer_handle_state_is_invalid(int state) {
    if (state == ZER_HS_FREED)        return 1;
    if (state == ZER_HS_MAYBE_FREED)  return 1;
    if (state == ZER_HS_TRANSFERRED)  return 1;
    return 0;
}
```

These are pure predicates extracted from compiler internals, linked into
zerc, AND verified by VST proofs (`proofs/vst/verif_handle_state.v`). The
pattern: **single C file, exhaustive enumeration of cases, mechanically
verified to match a Coq spec**.

`zercheck_ir.c` already uses these (line 261: `ir_is_invalid` calls
`zer_handle_state_is_invalid`). The principle of single-source enumeration
is established and proven.

### Precedent 5: Audit-script culture

The codebase has 7+ audit scripts that mechanically prevent specific bug
classes:

| Script | Bug class prevented |
|---|---|
| `tools/walker_audit.sh` | "Tree walker missing a NODE_ kind" (BUG-573, BUG-577 referenced in script) |
| `tools/walker_default_audit.sh` | "default: clause silently swallows new kinds" (Stage 2 Part B) |
| `tools/audit_matrix.sh` | "Control-flow handler missing context flag check" |
| `tools/audit_fixed_buffers.sh` | "New fixed-size buffer in safety code" (per CLAUDE.md rule #7) |
| `tools/agreement_audit.sh` | "IR vs AST analyzer disagreement" (Phase F0.1) |
| `tools/emit_audit.sh` | "Emitter missing safety wrapper" |
| `tools/safety_coverage.sh` | "Safety property not covered by extracted predicate" |

The team ships an audit script with each significant refactor. This
refactor should follow that pattern — ship a `tools/audit_handle_helpers.sh`
that mechanically detects regressions.

### Precedent 6: TYPE_DISTINCT unwrap unification (CLAUDE.md)

CLAUDE.md mentions:
> *"TYPE_DISTINCT must be unwrapped before ANY type dispatch — use
> type_unwrap_distinct(). This was the #1 bug class: 35+ sites fixed in
> one session. Every `->kind == TYPE_X` on a type from `checker_get_type()`
> / `check_expr()` needs unwrap. The helpers `type_is_optional()`,
> `type_is_integer()`, `type_width()` unwrap internally."*

Same shape. A finite enumeration (TYPE_DISTINCT being a wrapper) applied
inconsistently across consumer sites caused 35+ bugs. The fix was to
make the helpers (`type_is_optional` etc.) unwrap internally, so consumers
can't get it wrong.

---

## 5. Detailed Gap Analysis

### Gap 1: Lattice merge JOIN

**Location:** `zercheck_ir.c:497-516` (the `ir_merge_states` cascade).

**Current shape:**

```c
if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_FREED) {
    rh->state = IR_HS_MAYBE_FREED;
    rh->free_line = ph->free_line;
} else if (rh->state == IR_HS_FREED && ph->state == IR_HS_ALIVE) {
    rh->state = IR_HS_MAYBE_FREED;
} else if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_TRANSFERRED) {
    rh->state = IR_HS_MAYBE_FREED;
} else if (rh->state == IR_HS_TRANSFERRED && ph->state == IR_HS_ALIVE) {
    rh->state = IR_HS_MAYBE_FREED;
} else if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_MAYBE_FREED) {
    rh->state = IR_HS_MAYBE_FREED;
    rh->free_line = ph->free_line;
} else if (rh->state == IR_HS_FREED && ph->state == IR_HS_MAYBE_FREED) {
    rh->state = IR_HS_MAYBE_FREED;
} else if (rh->state == IR_HS_TRANSFERRED && ph->state == IR_HS_MAYBE_FREED) {
    rh->state = IR_HS_MAYBE_FREED;
}
/* MAYBE_FREED ↔ {ALIVE, FREED, TRANSFERRED}: rh already MAYBE_FREED, keep. */
```

**Why fragile:**

5 states × 5 states = 25 ordered pairs. Currently 7 explicit cases handled.
The remaining 18 are implicit (fall through, keep `rh` state). For each
implicit case, the reader must mentally simulate "is this fall-through
correct?".

When a new state is added (e.g., a hypothetical `IR_HS_BORROWED`), the
table needs 5 new explicit cases (or it falls through to whatever wasn't
handled). The cascade form makes this hidden — a code review wouldn't
naturally surface it.

**Bugs from this:** F3.2 #1 (this session), BUG-650 cases (May 2). Both
were "this state pair fell through and shouldn't have."

**Current full 5×5 truth table (audit 2026-05-10):**

Built by tracing the cascade including all fall-throughs:

| rh\ph   | UNK | ALIVE | FREED | MAYBE | XFER  |
|---------|-----|-------|-------|-------|-------|
| UNK     | UNK | UNK   | UNK   | UNK   | UNK   |
| ALIVE   | ALIVE | ALIVE | MAYBE | MAYBE | MAYBE |
| FREED   | FREED | MAYBE | FREED | MAYBE | FREED |
| MAYBE   | MAYBE | MAYBE | MAYBE | MAYBE | MAYBE |
| XFER    | XFER  | MAYBE | XFER  | MAYBE | XFER  |

**Asymmetric cells (current code):**
- `(UNK, ALIVE) → UNK` but `(ALIVE, UNK) → ALIVE` (rh starts as result; ph not in result keeps rh)
- `(FREED, XFER) → FREED` (no case fires; XFER not seen as widening)
- `(XFER, FREED) → XFER` (same)

The `ir_merge_states` flow guarantees one direction is hit — `rh` is
"result so far", iteration walks each pred adding constraints. So
asymmetry mostly doesn't manifest, but two scenarios it would:
1. UNKNOWN-as-rh-state. Rare (handles get explicit state at first
   alloc/use), but possible if a path doesn't touch the handle.
2. Multi-pred merge where a subset of preds have a handle in
   {FREED, XFER} and another subset has {XFER, FREED}.

**Fix shape:** 5×5 lookup table. Indexed by `[a.state][b.state]`. Adding
a new state extends the array (and probably the enum), making the gap
visible at compile time. See helper spec 6.1.

**Behavior changes the table introduces (must be acknowledged, not silent):**
- `(UNK, ALIVE) → ALIVE` (was UNK) — UNK is bottom of lattice; taking
  ph's state is the formally correct join. Previously kept UNK.
- `(UNK, FREED/MAYBE/XFER) → ph state` (was UNK) — same rationale.
- `(FREED, XFER) → MAYBE` (was FREED) — defensive against future
  reachability changes; today FREED+XFER is unreachable per checker rules.
- `(XFER, FREED) → MAYBE` (was XFER) — same as above.

In practice none of these cells fire today (verified by running tests
with the table substituted in — see section 9 stress tests). The doc
flags them so the implementer sees them, not for the reader to discover
mid-implementation.

### Gap 2: Alias-copy field drift

**Locations (audit 2026-05-10, re-audited against MLXDT branch
2026-05-10 late):** 7 alias-copy sites + 2 init-with-fresh-id
sites. Doc previously conflated these AND missed one site.
Reclassified:

**True alias-copy sites (7) — copy fields from src to dst:**
- L1582 (IR_COPY general — 7 fields incl. pool_name)
- L1644 (IR_CAST — 5 fields, missing is_thread_handle/pool_name; gated to pointer/opaque src so Handle never reaches)
- L1882 (IR_ASSIGN compound-handle from rh — 4 fields on main; **MLXDT branch BUG-660 added pool_name, making it 6 fields**) ← **NEWLY DISCOVERED**
- L1918 (orelse-ident in IR_ASSIGN — 7 fields)
- L2002 (@ptrcast in IR_ASSIGN — 5 fields, has escaped)
- L2039 (&-interior in IR_ASSIGN — 4 fields)
- L2154 (NODE_IDENT non-move alias — 5 fields, missing pool_name)

**Init-with-fresh-id sites (2) — covered by `ir_init_handle`, NOT this helper:**
- L1547 (IR_COPY move struct dst — fresh alloc_id, ALIVE state, no field copy)
- L2138 (NODE_IDENT move dst — fresh alloc_id, ALIVE state, no field copy)

**MLXDT branch confirmation (2026-05-10):** A separate audit session on
branch `claude/cool-johnson-MLXDT` independently discovered BUG-660 at
L1882 — exact same field-drift class as F3.2 #2. The fix manually
patched pool_name into that site. **This is the 7th instance of the
same bug class in <2 weeks.** Without `ir_alias_copy_provenance`, every
new IRHandleInfo field requires hunting all 7 sites. With it, one
helper update propagates everywhere.

**Why fragile:**

When a new field is added to `IRHandleInfo`, every alias-copy site must
be updated to propagate it. F3.2 added `pool_name` and updated only 2
of 8 sites. The remaining 6 sites silently dropped the field.

Some inconsistencies are INTENTIONAL:
- `IR_CAST` is gated to TYPE_POINTER/TYPE_OPAQUE source — Handle never
  reaches it, so `pool_name` doesn't apply.
- `&-interior` produces a different alloc identity (interior pointer to
  parent struct) — fields like `pool_name` don't transfer cleanly.
- `escaped` is set per-site because semantics vary (IR_CAST/@ptrcast
  propagate; IR_COPY doesn't, since the new local isn't escaped just
  because src was returned).

These intentional differences are NOT documented at each site. A future
refactorer (or fresh AI) reading these sites cannot tell which differences
are intentional vs accidental.

**Bugs from this:** F3.2 #2 (`pool_name` miss). One latent (would also
miss in IR_CAST/@ptrcast/&-of paths if a Handle reached them, but
type-gating prevents this today).

**Fix shape:** Single `ir_alias_copy_provenance(dst, src)` helper that
copies the 8 propagating fields. `escaped` excluded with documentation.
Sites with intentional differences (IR_CAST `escaped` propagation) call
the helper PLUS set `escaped` separately. See spec 6.2.

### Gap 3: Handle-init pattern fragmentation

**Locations (audit 2026-05-10): 22-25 sites where new `IRHandleInfo`
entries are populated after `ir_add_handle()`.** Doc previously claimed
17; recount finds more sites including auto-register paths in IR_CALL,
IR_RETURN, FuncSummary handling, ThreadHandle, and orelse-temp ALLOC.
Total `ir_add_handle` calls: 32; `ir_add_compound_handle` calls: 5.
Subtract ~8 "find-or-create, only set state" sites that aren't really
init → 22-25 true init sites.

Variation summary:

| Fields set | Approx. site count |
|---|---|
| state + alloc_line + alloc_id only | 7-8 sites |
| + source_color = POOL | 5-6 sites |
| + source_color = ARENA | 2 sites |
| + source_color = UNKNOWN + escaped=true (param) | 6-7 sites |
| + is_thread_handle | 1 site |
| + pool_name | 2 sites (currently inconsistent — see Gap 2) |
| Total distinct init shapes | ~10 distinct |

The factory consolidates ~16-19 of 22-25 sites (most fit one of the 7
allocation kinds). Remaining 3-6 sites have specialty patterns
(returns_color application, param-color inheritance, defer-scan FREED)
that stay ad-hoc with documented reasons.

**Net LOC reduction: ~120-150 lines removed, ~25 added in factory =
-95 to -125 net.** Doc previously estimated -100; reality is similar or
slightly better.

**Why fragile:**

10 of 17 sites do not set `source_color`. Default is 0 (`ZC_COLOR_UNKNOWN`).
For some sites (move struct, ThreadHandle), `source_color` is irrelevant —
move structs are skipped from leak detection by `ir_should_track_move`,
ThreadHandles by `is_thread_handle`. For other sites (auto-registered
params), UNKNOWN may or may not be the right default — it's not
documented per-site.

When a new init context is added (e.g., a new alloc kind), the developer
must know which fields to set. There's no checklist. This is how F3.2
missed `pool_name` — when adding a new field, every init site potentially
needs an update, and there's no enforcement.

**Bugs from this:** No documented bugs YET (mostly because defaults are
sensible). But this is the next-most-likely place for a missing-field bug.

**Fix shape:** Factory function `ir_init_handle(ps, local_id, alloc_id,
kind, line)` where `kind` is an enum specifying allocation source. The
factory sets all relevant fields based on kind. New kinds extend the
enum + the factory's switch, forcing acknowledgment. See spec 6.3.

### Gap 4: State transition divergence (FREED vs TRANSFERRED)

**Locations:**
- FREED transition: 5 sites, all (correctly) call `ir_propagate_alias_state`.
- TRANSFERRED transition: 11 sites, NONE call propagate.

**Why semi-fragile:**

The TRANSFERRED non-propagation is INTENTIONAL today:
- Move struct assignment uses fresh alloc_id for the destination, so
  there are no aliases of the source at the time of transfer.
- Handle is banned from spawn (`checker.c:10487`), so spawn-arg transfer
  doesn't apply to Handle aliases.
- Pointer-to-shared spawn args don't track via alloc_id (different
  semantics).

But this is **undocumented**. Reading any of the 11 TRANSFERRED sites
gives no clue why propagation isn't called. A future refactorer (or fresh
AI) adds a new TRANSFERRED-causing op and either:
- (a) Doesn't propagate, missing real cases.
- (b) Propagates, breaking move-struct semantics.

**Bugs from this:** None YET. Latent.

**Fix shape:** Two thin wrappers `ir_record_freed` and
`ir_record_transferred` that encapsulate the FREED-with-propagate vs
TRANSFERRED-without-propagate decisions and DOCUMENT them centrally. See
spec 6.4.

### Gap 5: Error message fragmentation

**Locations (audit 2026-05-10):** 16 "use after X" sites + 4 "returning
X" sites + 5 "freeing X" sites = **25 invalid-state-detection error
sites**. Doc previously claimed 18; recount finds:

- "use after free / use of {state} / use after move" — **16 sites**
  (lines 974, 1535, 1560, 1632, 1728, 1779, 1783, 1950, 2096, 2124,
  2128, 2164, 2381, 2533, 2888, 2955)
- "returning {state} value/pointer" — **4 sites**
  (lines 2205, 2217, 2236, 2252) — same predicate as "use of {state}"
  but in return position
- "freeing {state}" — **5 sites**
  (lines 1704, 1708, 2502, 2506, 2719) — same predicate but in
  free-call position

**Decision needed (open question Q7, see section 11):** Should
`ir_report_invalid_use` cover all 25, only the 16 "use" sites, or some
intermediate split? See section 6.5 for analysis.

Sample of inconsistencies:

```c
/* L974 — generic UAF walker */
"use after free: '%.*s' is %s (freed at line %d)"

/* L1535 — direct UAF on src */
"use after move: '%.*s' ownership transferred at line %d"

/* L1560 — IR_COPY use of invalid */
"use of %s handle %%%d"

/* L1632 — IR_CAST use of invalid */
"use of %s handle %%%d in cast"

/* L1728 — IR_POOL_GET */
"use after free: %%%d is %s (freed at line %d)"

/* L1779 — IR_FIELD_READ root invalid */
"use after free: local %%%d is %s (freed at line %d)"

/* L1783 — IR_FIELD_READ compound invalid */
"use after free: compound '%.*s' on local %%%d is %s (freed at line %d)"
```

Same conceptual error ("you used a handle that's no longer valid") but
~6 different message templates. Differences include:
- Prefix: "use after free", "use of {state} handle", "use after move"
- Subject form: `%%%d` (local id) vs `'%.*s'` (string name)
- Suffix: includes free_line vs not
- Context: "in cast", "compound", etc.

**Why fragile:**

When a new state is added, all 18 sites need consistent messaging — but
each site is written by hand. Adding a new diagnostic field (e.g., "freed
by callee X at line Y") means updating all 18.

When a user sees an error, the message wording varies based on which
site fires. This is a UX issue too — same situation, different message.

The deleted AST analyzer had a centralized `zc_report_invalid_use` that
handled all 5 invalid states with consistent messaging.

**Bugs from this:** No code-correctness bugs documented, but the OLD
`zc_report_invalid_use` had pool-handle vs move-struct distinction
(checked `pool_id == -3`) that was lost when we ported to IR. So
move-struct errors say "use after free" when they should say "use after
move" in some IR sites. UX regression, not a code bug.

**Fix shape:** `ir_report_invalid_use(zc, h, line, local_id, path,
path_len, func)` that formats based on h->state and h->is_thread_handle,
with consistent message structure. See spec 6.5.

---

## 6. Per-Helper Specifications

Each helper has:
- **Purpose**: what it does
- **Signature**: function declaration
- **Sites it replaces**: count and locations
- **What stays inline**: parts NOT moved into the helper, with rationale
- **Risk**: empirical risk class
- **Test plan**: how to verify
- **Precedent**: existing pattern elsewhere

### 6.1 `ir_state_join` — lattice merge table

**Purpose:** Centralize the state-pair → joined-state mapping for the
CFG merge in `ir_merge_states`. Eliminate the 7-case if-else cascade
that has caused 4 documented bugs.

**Signature:**

```c
static IRHandleState ir_state_join(IRHandleState a, IRHandleState b);
```

Pure function. Symmetric (`ir_state_join(a, b) == ir_state_join(b, a)`)
by construction. Defined as a 5×5 const lookup table indexed
by `[a][b]`.

**Sites it replaces:** 1 (the cascade in `ir_merge_states` lines 497-516).

**Cascade semantics — current behavior (verified 2026-05-10):**

| rh \ ph | UNKNOWN | ALIVE | FREED | MAYBE_FREED | TRANSFERRED |
|---|---|---|---|---|---|
| **UNKNOWN** | UNK | UNK | UNK | UNK | UNK |
| **ALIVE** | ALIVE | ALIVE | MAYBE | MAYBE | MAYBE |
| **FREED** | FREED | MAYBE | FREED | MAYBE | **FREED** *(unreachable today)* |
| **MAYBE_FREED** | MAYBE | MAYBE | MAYBE | MAYBE | MAYBE |
| **TRANSFERRED** | XFER | MAYBE | **XFER** *(unreachable today)* | MAYBE | XFER |

The current cascade is **asymmetric** — bold cells fall through (no
case fires), result keeps `rh` state. UNKNOWN row is all-UNK because no
cascade case has `rh == UNKNOWN`.

**Implementation choice (must be acknowledged):**

The proposed table in Appendix A.1 makes 7 cells **differ** from current
behavior, all in the formally-correct direction:

| Cell | Current | Proposed | Rationale |
|---|---|---|---|
| (UNK, ALIVE) | UNK | ALIVE | UNK is bottom; join takes ph state |
| (UNK, FREED) | UNK | FREED | Same rationale |
| (UNK, MAYBE) | UNK | MAYBE | Same rationale |
| (UNK, XFER) | UNK | XFER | Same rationale |
| (FREED, XFER) | FREED | MAYBE | Defensive (currently unreachable) |
| (XFER, FREED) | XFER | MAYBE | Defensive (currently unreachable) |
| Plus 2 cells preserved exactly | | | (FREED, FREED) → FREED, (XFER, XFER) → XFER |

UNK-row changes are the formally correct lattice join (UNKNOWN is the
bottom element — joining with anything else takes the other side's
state). The 4 UNK cells were all "no case fires" fall-throughs in the
cascade, not deliberate behavior.

The 2 "defensive" XFER cells are flagged in section 11 Q1.

**Migration safety:** All 6 changed cells are believed unreachable in
practice. Run the existing test suite with the table substituted in;
any regression flags an actual reachable case that changed semantics.
If a regression appears, EITHER preserve current behavior at that cell
OR investigate why the case was reachable (possibly a real bug
previously masked by fall-through).

**Free-line propagation:** The cascade also did `rh->free_line =
ph->free_line` in some cases (when widening to MAYBE_FREED from ALIVE
seeing FREED/MAYBE_FREED in pred). This is BESIDES the state mapping —
it's about preserving diagnostic info. Two options:

- **Option A:** Keep free-line propagation INLINE at the call site after
  calling `ir_state_join`. The helper returns just state; the call site
  applies the state and decides if free_line should be updated.
- **Option B:** Add a parallel `ir_freeline_join` helper or extend the
  table to return `(state, free_line_source)`.

Recommend **Option A**: simpler helper, free-line stays close to its
context (which line should be displayed in errors).

**What stays inline:**
- The call to `ir_propagate_alias_state` after merging (handled
  per-instruction, not in merge).
- The free_line copy logic (Option A above).
- The `if (!ph) continue` early-out (no pred handle to merge with).

**Risk:** LOW. Pure refactor of state-mapping logic. Lookup table is
exhaustively defined for 25 cells. Net replace 30 lines of cascade with
a 30-line table + 5-line helper. Code reduces. Compile-time check on
table dimensions catches new state additions.

**Test plan:**
- Existing tests: 538 ZER + 200 fuzz + 139 conversion + 5 cross-arch
  must all pass.
- New micro-test: feed 25 paired-state inputs and verify table output
  matches expected lattice.
- Specific regression: `tests/zer/free_realloc_loop.zer` (F3.2 fix) must
  still compile.

**Precedent:** Type dispatch using exhaustive switch (e.g., `type_is_optional`
in checker.c). 5×5 table is the obvious extension to 2-arg dispatch.

---

### 6.2 `ir_alias_copy_provenance` — alias field copier

**Purpose:** Centralize the field-copy block when one handle becomes an
alias of another. Replace 6+ ad-hoc field-copy sites with consistent
helper. Adding a new field to `IRHandleInfo` requires updating ONE
function instead of N sites.

**Signature:**

```c
static void ir_alias_copy_provenance(IRHandleInfo *dst, const IRHandleInfo *src);
```

Copies the alias-propagating fields from src to dst. Caller may override
state/escaped after calling for context-specific behavior (e.g., move-
transfer sets dst→ALIVE separately; IR_CAST propagates `escaped` while
IR_COPY does not).

**Sites it replaces:** 6-8 sites depending on intentional-difference
classification. Specifically:

| Site | Replace? | Override after call |
|---|---|---|
| L1547 (IR_COPY move dst init) | Partial — uses fresh alloc_id | state=ALIVE, alloc_id=new |
| L1582 (IR_COPY general) | YES — full propagation | none |
| L1644 (IR_CAST) | YES + override escaped | escaped=src.escaped |
| L1918 (orelse-ident) | YES | none |
| L2002 (@ptrcast) | YES + override escaped | escaped=src.escaped |
| L2039 (&-interior) | NO — different alloc identity (sub-object) | — |
| L2138 (NODE_IDENT move dst) | NO — fresh alloc | — |
| L2154 (NODE_IDENT non-move alias) | YES | none |

Net 5 sites use the helper directly, 3 keep ad-hoc init.

**Field set propagated:**

```c
state, alloc_line, free_line, alloc_id, source_color,
is_thread_handle, pool_name, pool_name_len
```

8 fields total. NEW fields added to IRHandleInfo go in the helper; sites
get the new field automatically.

**`escaped` is NOT in the helper.** Rationale: semantics vary. IR_CAST/
@ptrcast propagate `escaped` (the cast doesn't change escape status of
the underlying allocation). IR_COPY does NOT propagate `escaped` (the
new local isn't escaped just because the source was returned).
Documented in helper comment.

**What stays inline:**
- The realloc-safe snapshot pattern (read src fields into locals before
  `ir_add_handle` — see "UAF GUARD" comments at multiple sites).
- The `ir_add_handle` call itself.
- Move-struct fresh-alloc-id logic.
- Per-site `escaped` decisions.
- Per-site post-copy state overrides.

**Risk:** LOW-MEDIUM. Most sites have identical field sets (the helper
captures them). 3 sites have intentional differences that stay inline
with documentation. The risk is misclassifying which field-copy
differences are intentional — mitigated by extensive testing against
existing test suites.

**Test plan:**
- Existing test suites must all pass.
- Specific regression: `tests/zer_fail/wrong_pool_get.zer` (F3.2 alias
  test) must still error.
- New negative test: register move-struct alias chain through `@ptrcast`
  and verify `pool_name` does NOT propagate (because @ptrcast is for
  `*opaque` not Handle).

**Precedent:** `propagate_escape_flags` in checker.c (lines 878-886).
Direct shape match.

---

### 6.3 `ir_init_handle` — handle factory

**Purpose:** Centralize new-handle creation with kind-aware defaults.
Replace 17 ad-hoc init sites with one factory function. Adding a new
allocation kind extends the enum + factory switch, forcing
acknowledgment.

**Signature:**

```c
typedef enum {
    IR_ALLOC_POOL,        /* Pool/Slab.alloc → ZC_COLOR_POOL */
    IR_ALLOC_ARENA,       /* arena.alloc → ZC_COLOR_ARENA */
    IR_ALLOC_MALLOC,      /* extern malloc → ZC_COLOR_MALLOC */
    IR_ALLOC_PARAM,       /* auto-registered param → escaped=true */
    IR_ALLOC_THREAD,      /* spawn ThreadHandle → is_thread_handle=true */
    IR_ALLOC_MOVE,        /* move struct local */
    IR_ALLOC_OPAQUE,      /* unknown — fallback */
} IRAllocKind;

static IRHandleInfo *ir_init_handle(IRPathState *ps, int local_id,
                                     int alloc_id, IRAllocKind kind, int line);
```

Returns the handle (existing or newly-zeroed). Sets state=ALIVE,
alloc_line=line, alloc_id=alloc_id, plus kind-specific defaults
(source_color, is_thread_handle, escaped).

**Sites it replaces:** 17 sites total. Each call replaces ~5 lines of
inline assignment with a single `ir_init_handle` call. Net ~120 lines
removed, ~20 lines of helper added. Net -100 LOC.

Site mapping:

| Site context | Kind to pass |
|---|---|
| IR_POOL_ALLOC handler | IR_ALLOC_POOL |
| IR_SLAB_ALLOC handler | IR_ALLOC_POOL |
| IR_ASSIGN with NODE_CALL pool/slab.alloc | IR_ALLOC_POOL |
| IR_CALL pool/slab.alloc dest | IR_ALLOC_POOL |
| IRMC_ARENA_ALLOC | IR_ALLOC_ARENA |
| IR_RETURN with returns_color=ARENA | IR_ALLOC_ARENA |
| Param-color inheritance dst with arena color | IR_ALLOC_ARENA |
| Extern malloc/calloc | IR_ALLOC_MALLOC |
| @ptrcast param auto-register | IR_ALLOC_PARAM |
| IR_CAST param auto-register | IR_ALLOC_PARAM |
| IRMC_FREE param auto-register | IR_ALLOC_PARAM |
| FuncSummary frees-param auto-register | IR_ALLOC_PARAM |
| IR_RETURN escape-mark auto-register | IR_ALLOC_PARAM |
| Spawn ThreadHandle | IR_ALLOC_THREAD |
| IR_COPY move struct dst | IR_ALLOC_MOVE |
| Spawn move-struct arg auto-register | IR_ALLOC_MOVE |
| Other (rare) | IR_ALLOC_OPAQUE |

**`pool_name` parameter:** Optionally extend signature to take pool_name:

```c
static IRHandleInfo *ir_init_handle(IRPathState *ps, int local_id,
                                     int alloc_id, IRAllocKind kind, int line,
                                     const char *pool_name, uint32_t pool_name_len);
```

For non-pool kinds, callers pass `NULL, 0`. For IR_ALLOC_POOL, callers
extract via `ir_extract_pool_name(call, ...)`. Single-source recording.

**What stays inline:**
- The overwrite-while-alive check (must happen BEFORE init when an
  existing handle is detected — uses the source line for the error).
- Extracting alloc_id (varies by site: dest_local, _ir_next_alloc_id++,
  arg_local).
- Extracting pool_name from the call expression (helper exists).
- Post-init state overrides (e.g., setting state=TRANSFERRED for spawn
  args after auto-init as ALIVE).

**Risk:** MEDIUM. 17 sites is a lot to coordinate. Mitigations:
- Land helper additively first (no removals), tests still green.
- Migrate sites one-by-one, running tests after each.
- Some sites may not fit cleanly (e.g., handle re-init where state
  must be preserved). Those sites stay ad-hoc — partial migration is
  acceptable.

**Test plan:**
- Full make check after each batch of site migrations (~5 sites/batch).
- Specific watch-fors: source_color regressions on edge-case alloc
  paths, pool_name set/not-set consistency.

**Precedent:** `scope_add` in types.c (Symbol factory). Direct pattern
match. Difference: scope_add is just memset+set-name; ir_init_handle
adds kind-aware defaults because handle init has more variation than
Symbol init.

---

### 6.4 `ir_record_freed` / `ir_record_transferred` — state transitions

**Purpose:** Wrap the multi-step state transition pattern (set state +
free_line + propagate) in a single call. Document propagation decisions
centrally.

**Signatures:**

```c
/* FREED: mark handle as freed, record line, propagate to alloc_id aliases.
 * Used when an explicit free() / free_ptr() / pool.free() is observed.
 * The propagation handles the case where multiple locals share the same
 * underlying allocation (created by alias copies). */
static void ir_record_freed(IRPathState *ps, IRHandleInfo *h, int line);

/* TRANSFERRED: mark handle as transferred, record line. NO alias
 * propagation — documented decision based on the types that produce
 * TRANSFERRED state.
 *
 * Why no propagate:
 *   - Move struct: assignment creates fresh alloc_id for dest, source's
 *     alloc_id has no aliases at the time of transfer.
 *   - Spawn arg: Handle is banned from spawn (checker.c:10487), so Handle
 *     aliases don't apply. Move struct uses fresh alloc_id (above).
 *     Pointer-to-shared has different semantics than alloc_id-aliasing.
 *
 * If a new TRANSFERRED-causing op is added (e.g., a hypothetical
 * `consume(h)` intrinsic on a Handle), audit whether the aliasing model
 * applies and update HERE accordingly. */
static void ir_record_transferred(IRHandleInfo *h, int line);
```

Note: `ir_record_transferred` does not take `IRPathState *ps` because it
doesn't propagate. This is deliberate — the signature documents the
decision.

**Sites replaced:**
- `ir_record_freed`: 5 sites (defer scan, IR_POOL_FREE, IRMC_FREE arg
  resolved, IRMC_FREE compound, etc.).
- `ir_record_transferred`: 11 sites (Z2 asm, spawn arg, IR_COPY move
  src, NODE_INDEX move-extract, NODE_IDENT move src, IR_RETURN move,
  etc.).

**What stays inline:**
- The "is this handle currently invalid?" check (error report happens
  before the transition).
- The choice of WHICH handle to call the transition on (compound vs
  bare resolution happens at the site).
- The error message for double-free / use-of-already-transferred (these
  are pre-transition checks).

**Risk:** LOW. Thin wrappers. The ONLY judgment is "should TRANSFERRED
propagate or not?" — the answer today is "no" with documented rationale.
If that decision changes in the future, ONE function changes.

**Test plan:**
- All existing tests must pass.
- Specific watch-fors: no new false-positive UAF errors (would indicate
  TRANSFERRED is propagating when it shouldn't).

**Precedent:** `ir_propagate_alias_state` already encapsulates the
"propagate state to aliases" mechanism. These wrappers add the "decide
whether to propagate" decision around it.

---

### 6.5 `ir_report_invalid_use` — central error reporter

**Purpose:** Single source for all "use of invalid handle" error messages.
Replace 18 ad-hoc fprintf-equivalent sites with consistent state-aware
messages. Adding a new state or improving messages updates one function.

**Signature:**

```c
static void ir_report_invalid_use(ZerCheck *zc, IRHandleInfo *h, int line,
                                   int local_id, const char *path,
                                   uint32_t path_len, IRFunc *func);
```

`local_id` and `path/path_len` form the entity identifier. If
`path_len > 0`, the entity is a compound (`local.field` or `arr[N]`)
and the path string is shown. Otherwise the local's source name is
shown. Function emits via `ir_zc_error`.

**Sites it replaces (audit 2026-05-10): all 25 sites across 3 tiers,
unified via `IRUseContext` enum.**

**Tier 1 — `IR_USE_READ` (16 sites — use/deref/field/index access):**
- L974 (generic UAF walker), L1535 (use after move IR_COPY),
  L1560 (IR_COPY use of invalid src), L1632 (IR_CAST use of invalid),
  L1728 (IR_POOL_GET), L1779/L1783 (IR_FIELD_READ root + compound),
  L1950 (move from array compound), L2096 (IR_ASSIGN GET on invalid),
  L2124/L2128/L2164 (NODE_IDENT TRANSFERRED + invalid checks),
  L2381 (IR_CALL move struct arg), L2533 (IR_CALL UAF walker),
  L2888 (IR_FIELD_WRITE invalid value), L2955 (IR_INDEX_WRITE invalid).

**Tier 2 — `IR_USE_RETURN` (4 sites — return statement):**
- L2205, L2236 ("returning %s value (move struct)")
- L2217, L2252 ("returning %s pointer ... caller would receive
  dangling pointer")

**Tier 3 — `IR_USE_FREE` (5 sites — free-call attempt):**
- L1704 ("freeing X which may already be freed" — IR_POOL_FREE)
- L1708 ("freeing X which was already transferred")
- L2502, L2506 (same pair in IRMC_FREE)
- L2719 (in IRMC_FREE_PTR)

All 3 tiers share the predicate "handle is in invalid state." The
verb differs (use/return/free) but the state-classification logic is
identical. Per Q7 decision (section 11), helper accepts `IRUseContext`
enum and dispatches wording in a switch — same pattern as Rust's E0382
error code with `MoveOutAction` enum.

**Message structure:**

```c
switch (h->state) {
case IR_HS_FREED:
    "use after free: '<entity>' freed at line <N>"
case IR_HS_MAYBE_FREED:
    "use after free: '<entity>' may have been freed at line <N>"
case IR_HS_TRANSFERRED:
    if (h->is_thread_handle):
        "use after transfer: '<entity>' transferred to thread at line <N>"
    else if (h->source_color is move-related):
        "use after move: '<entity>' ownership transferred at line <N>"
    else:
        "use after transfer: '<entity>' transferred at line <N>"
}
```

**Move-struct distinction:** The OLD `zc_report_invalid_use` checked
`pool_id == -3` to distinguish move struct from pool handle. IR doesn't
have `pool_id`. Need an alternative:
- Option A: New field `is_move_struct` on IRHandleInfo (cheap).
- Option B: Look up local's type at error time and check
  `ir_should_track_move(loc->type)`.
- Option C: Encode in source_color (add ZC_COLOR_MOVE).

Recommend **Option B** — no new field needed, type info is available
via `IRFunc *func` and `local_id`. Cost is one extra type lookup per
error report (rare path).

**What stays inline:**
- Sites that emit non-"invalid use" errors (e.g., "double free",
  "wrong pool", "leak") — these are different conceptual errors and
  stay separate.
- Pre-error condition checks (e.g., `if (ir_is_invalid(h))`).
- Sites that have specific contextual info (e.g., "in cast", "in field
  write") — these can either keep inline OR pass a context string to
  the helper. Recommend simple version of helper first; extend with
  context arg if needed.

**Risk:** LOW-MEDIUM. Error messages are user-visible. Test suite
includes tests that grep for specific error wording. Migration approach:
- Phase 1: helper accepts the simplest message form (matches majority
  of current sites).
- Phase 2: migrate sites one-by-one, updating tests if the new wording
  differs from old.
- Tolerate minor wording divergence — the goal is consistency, not
  preservation of every legacy phrasing.

**Test plan:**
- After full migration, grep tests for old error wording. Update tests
  to match new wording where consistent.
- Audit: no two sites produce different messages for the same state
  combination.

**Precedent:** `zc_report_invalid_use` in deleted `zercheck.c` (commit
`7ffbd9d^` line 1131). Direct port shape with adaptations for IR data
model (local_id+path instead of name+name_len).

---

## 7. Audit Tooling

The codebase ships an audit script with each significant refactor (see
section 4 precedent 5 — 7 existing scripts). This refactor follows
the pattern.

**Script:** `tools/audit_handle_helpers.sh`

**What it audits:**

1. **Lattice cascade detection:** Greps `zercheck_ir.c` for the pattern
   `else if (.*->state == IR_HS_.*&& .*->state == IR_HS_)` more than 5
   times in a function. If found, suggests using `ir_state_join`.

2. **Alias-copy field-block detection:** Greps for sequences of
   `dst_h->X = src.X;` or `dst_h->X = src_h->X;` of length ≥ 3. If
   found outside `ir_alias_copy_provenance`, flags as potential drift.

3. **Handle-init pattern detection:** Greps for `ir_add_handle(...)`
   followed within 5 lines by `->state =` and `->alloc_line =`. If
   `ir_init_handle` is available, suggests migration.

4. **State-transition without propagate:** Greps for
   `->state = IR_HS_FREED` not followed within 3 lines by
   `ir_propagate_alias_state(...IR_HS_FREED)`. Each match is reviewed —
   may be intentional (already propagated, or in arena-reset which is
   batched), may be missing helper.

5. **Error-message inconsistency:** Greps `ir_zc_error(...)` calls and
   compares messages of "use after free" / "use of {state}" / "use after
   move" prefixes. Counts unique formats. If > 3 distinct formats exist
   for same prefix, flags drift.

**Output format:**

```
HELPER_AUDIT zercheck_ir.c: ir_state_join opportunity (cascade with 7 cases at line 497)
HELPER_AUDIT zercheck_ir.c: ir_alias_copy_provenance miss at line 2154 (5-field block)
HELPER_AUDIT zercheck_ir.c: ir_report_invalid_use opportunity at line 1535
```

**Exit code:** 0 if no findings, 1 otherwise. Wired into `make check`
as soft warning (not failure) initially. Promoted to hard failure after
all current findings are addressed.

**Effort:** ~50 LOC bash. Models on `tools/walker_audit.sh`.

**Maintenance:** Each new helper added in this refactor extends the
audit script with a new check.

---

## 8. Implementation Strategy

### Phase ordering

Recommended order (low → high risk, low → high impact):

1. **Audit script first.** `tools/audit_handle_helpers.sh`. Soft-fail
   in CI. Establishes the regression-prevention layer before changes.
   Effort: ~1 hr.

2. **`ir_state_join` table.** Pure refactor of one function. Easy to
   verify. Establishes the table-based dispatch idiom in the file.
   Effort: ~1 hr.

3. **`ir_record_freed` / `ir_record_transferred` wrappers.** Thin
   wrappers around existing logic. Migrate 16 sites mechanically.
   Effort: ~1 hr.

4. **`ir_report_invalid_use`.** Migrate 18 error sites. Each migration
   is a one-line replace. Test suite catches message-wording regressions.
   Effort: ~2 hrs.

5. **`ir_alias_copy_provenance`.** Migrate 5 of 8 sites; document the
   3 with intentional differences. Effort: ~1 hr.

6. **`ir_init_handle` factory.** Most invasive — 17 sites. Each
   migration is ~5 lines → 1 line. Stage in batches of 5 sites with
   full test runs between. Effort: ~2 hrs.

Total: ~8 hrs end-to-end including tests.

### Per-helper migration approach

For each helper:

1. **Add helper without removing inline code.** Helper exists, no call
   sites yet. Tests pass.
2. **Migrate one call site.** Run full tests. Confirm green.
3. **Migrate remaining sites in a batch.** Run full tests. Confirm green.
4. **Remove dead inline code if any.** Final cleanup.
5. **Commit individual helper as one PR/commit.**

### Commit boundaries

One commit per helper. Six commits total plus one for the audit script.
Each commit is independently revertable.

```
commit 1: tools/audit_handle_helpers.sh — bug-class regression detector
commit 2: refactor: ir_state_join table replaces 7-case cascade
commit 3: refactor: ir_record_freed / ir_record_transferred wrappers
commit 4: refactor: ir_report_invalid_use central error reporter
commit 5: refactor: ir_alias_copy_provenance for alias-copy consolidation
commit 6: refactor: ir_init_handle factory replaces 17 ad-hoc inits
commit 7: docs: update CLAUDE.md / compiler-internals.md with new helpers
```

### Backward compatibility

Helpers are static (file-local) — no external API impact. zercheck.h
unchanged. LSP, test_firmware, test_production, etc. are unaffected.
Only `zercheck_ir.c` is touched.

---

## 9. Testing Strategy

### Existing test surfaces (must all pass)

Per `make check`:
- 200 fuzz tests (`tests/test_semantic_fuzz.c`)
- 538 ZER integration tests (`tests/test_zer.sh`)
- 139 conversion tests (`tests/test_convert.sh`)
- 28 module tests (`test_modules/run_tests.sh`)
- 5 cross-arch tests (`tests/test_cross_arch.sh`)
- C unit tests (~17 ir_validate, ~95 firmware/production)

Plus indirect:
- 784 rust_tests
- 36 zig_tests
- 2 QEMU MMIO tests

Total: ~2,089 tests. All must remain green after each helper migration.

### Specific regression tests for each helper

#### `ir_state_join`
- **Test the recently-added cases:** `tests/zer/free_realloc_loop.zer`
  (F3.2 fix) — must compile clean (no convergence error).
- **Test prior cases:** `tests/zer_fail/compound_field_maybe_freed.zer`
  (BUG-650 regression test) — must error.

#### `ir_alias_copy_provenance`
- **Test pool-name propagation:** `tests/zer_fail/wrong_pool_get.zer`
  (F3.2) — must error with wrong-pool message after orelse-decomposition.
- **Test no-propagation through @ptrcast:** new test where a @ptrcast'd
  pointer's pool_name is NOT used (because @ptrcast is for *opaque, not
  Handle).

#### `ir_init_handle`
- **Test source_color propagation:** existing arena-reset tests must
  still find arena-colored handles.
- **Test thread-handle leak detection:** `tests/zer_fail/thread_not_joined.zer`
  (or equivalent) must still error.

#### `ir_record_freed`
- **Test alias propagation through free:** existing tests where
  `pool.free(h)` propagates to aliases must still detect UAF on alias.

#### `ir_record_transferred`
- **Test no-propagation:** existing move-struct tests must still pass.
  No false-positive UAF errors should appear (would indicate
  TRANSFERRED is propagating wrongly).

#### `ir_report_invalid_use`
- **Audit error-message diff:** before-and-after wording diff. Update
  tests that grep for specific old messages where the new wording is
  cleaner.

### Stress tests

- **Loop with cycling free/realloc:** the F3.2 case. Must converge.
- **Deeply nested struct with handles:** F0.5 case — `nested_struct_handle_uaf.zer`.
- **Move struct passed through asm and spawned:** F0.4 + F0.6 cases.

### Regression CI gate

Add to `make check` after the audit script lands:

```makefile
# Run handle-helper audit after compile
bash tools/audit_handle_helpers.sh
```

Soft-fail (warning) initially. Promote to hard-fail after refactor lands
and all current findings are resolved.

---

## 10. Risks and Mitigations

### Risk 1: Misclassifying intentional differences

**Scenario:** `escaped` field NOT being copied in IR_COPY but BEING
copied in IR_CAST is intentional. If the alias-copy helper is
over-zealous and copies escaped everywhere, IR_COPY on a returned-then-
copied handle would falsely mark the new local as escaped, suppressing
leak detection.

**Mitigation:** Explicit `escaped` exclusion in helper, with comment.
Sites that need escaped propagation set it AFTER the helper call.
Document the per-site decision.

### Risk 2: Error message regressions break tests

**Scenario:** A test greps for `"use after free: 'h' freed at line 5"`
and the new central reporter produces `"use-after-free: 'h' freed at line 5"`
(hyphen). Test breaks.

**Mitigation:**
- Survey all test grep patterns before changing wording.
- Either preserve exact wording for legacy tests OR update tests
  consistently.
- Move-struct distinction (use after move vs use after free) is the
  one place where error wording SHOULD change to be more accurate. Tests
  for these need updating.

### Risk 3: `ir_init_handle` factory doesn't fit some sites

**Scenario:** A site has a complex init pattern where state is set,
checked, then re-set based on conditions. The factory's "create with
defaults" doesn't fit.

**Mitigation:** Partial migration is acceptable. Sites that don't fit
keep ad-hoc init. The audit script flags them but doesn't fail. The
helper is for the common case.

### Risk 4: Refactor introduces new bugs

**Scenario:** Restructuring code is itself a bug source. We fix 5 bugs
and introduce 2 new ones.

**Mitigation:**
- Per-helper commit boundaries (revertable).
- Full test suite after each helper.
- Existing safety-net is strong (~2,089 tests; agreement audit; etc.).
- Helpers are mechanical refactors — same logic in different shape.

### Risk 5: Helpers don't survive Phase 5 (atomic ordering)

**Scenario:** Phase 5 adds a new state machine for atomic ordering. The
new state machine has its own join function, alias copy, etc. The helpers
we built don't generalize.

**Mitigation:** Phase 5's `OrderingState` is separate from
`IRHandleState`. It will have its own helpers (the same shape applied
to the new state machine). The handle-state helpers don't need to
generalize — they just need to consolidate the existing handle-state
code. New tracking dimensions get their own helpers.

### Risk 6: The "TRANSFERRED no-propagate" decision becomes wrong

**Scenario:** A future feature (e.g., a hypothetical `consume(h)`
intrinsic on a non-fresh-alloc-id type) adds a TRANSFERRED-causing op
where alias propagation IS needed.

**Mitigation:** This is exactly why `ir_record_transferred` exists with
a documented decision. Future developer adds the new op, hits the
helper, reads the comment, decides whether to propagate. Decision
update is in ONE place.

---

## 11. Open Questions

### Q1: Defensive lattice cells `(FREED, TRANSFERRED)`? — **DECIDED**

**Decision (2026-05-10): formally-correct lattice. All 6 changed cells
take their formally-correct value, not preserved behavior.**

The 6 cells: 4 in UNK row + 2 defensive (FREED↔XFER).

**Reasoning:**

1. **Mathematical correctness.** A semilattice join must be commutative,
   associative, idempotent. The current cascade is asymmetric — encoding
   that asymmetry into a lookup table would memorialize a fall-through
   accident, not a deliberate semantic choice.

2. **All 6 cells are unreachable today** (verified by audit). If they
   become reachable via future checker rule changes, the formally-correct
   behavior is what we want:
   - UNK row: UNK is "no information"; joining with state X should
     yield X. Keeping UNK loses information.
   - XFER↔FREED: if reachability allows it, the join must be MAYBE_FREED
     (conservative). Keeping FREED-only or XFER-only would be a real bug.

3. **Production compiler precedent.** rustc's `JoinSemiLattice` is a
   mathematical semilattice. Swift SIL's `OwnershipKind::join` is the
   same. ZER would be the only compiler with deliberately non-commutative
   "join" if we preserved current asymmetry.

**Risk if a cell turns out reachable:** test suite fails loudly. That's
a feature — it surfaces a reachability change that needs analysis. The
current behavior at unreachable cells is dead semantic mass, not safety.

### Q2: `ir_init_handle` signature width

The factory needs ~6 parameters (ps, local_id, alloc_id, kind, line,
pool_name+len). 7-parameter functions are unwieldy. Two options:

- **Wide signature:** All params explicit.
- **Struct argument:** `ir_init_handle(ps, &init_args)` where init_args
  is a struct.

Recommend **wide signature** — more readable at call sites, and the
parameter list is finite/closed.

### Q3: Replace `ir_propagate_alias_state` itself?

The function is already centralized but used inconsistently (FREED only).
Should it become an internal helper of `ir_record_freed` and not be
called directly elsewhere?

Recommend **leave as-is** — `ir_propagate_alias_state` is called once
from a defer scanner site (line 1257), once from arena-reset (line 1716
batch), and 3 times from regular FREED sites. The defer/arena uses are
legitimate direct calls. Don't force them through the wrapper.

### Q4: Audit script as soft-fail or hard-fail?

Initially soft-fail (warning) is safer — refactor lands without breaking
CI. Promote to hard-fail after refactor lands and all current findings
are resolved.

Alternative: hard-fail from day one with a baseline file (like
`tools/audit_fixed_buffers.sh` does with `tools/fixed_buffer_baseline.txt`).
New findings break CI; existing ones are grandfathered.

Recommend **hard-fail with baseline** — matches existing audit-script
pattern and prevents bypass.

### Q5: Document this in CLAUDE.md?

CLAUDE.md is loaded into every session. Adding 100 lines of helper-layer
description grows the prompt. Compromise:

- **Short note in CLAUDE.md:** "zercheck_ir uses unified-helper layer —
  see docs/refactor_ir.md for details. Add new IRHandleInfo fields to
  `ir_alias_copy_provenance`."
- **Full details in compiler-internals.md:** the ZER-CHECK Architecture
  section (already exists, just extend it).

Recommend **short note + full details elsewhere**.

### Q7: Error reporter scope — **DECIDED**

**Decision (2026-05-10): fold all 25 sites (Option C) via `IRUseContext`
enum parameter.**

Per audit, 25 invalid-state error sites split into 3 tiers (section 6.5).

```c
typedef enum {
    IR_USE_READ,    /* use/deref/field — "use after free/move" */
    IR_USE_RETURN,  /* return stmt — "returning {state} pointer" */
    IR_USE_FREE,    /* free attempt — "double free / free after move" */
} IRUseContext;

static void ir_report_invalid_use(ZerCheck *zc, IRHandleInfo *h, int line,
                                   int local_id, const char *path,
                                   uint32_t path_len, IRFunc *func,
                                   IRUseContext ctx);
```

**Reasoning:**

1. **All 3 tiers share the same predicate** ("handle is in invalid state").
   The verb (use/return/free) differs but the state-classification logic
   is identical. Splitting forces 3 places to update when a new state
   is added.

2. **Rust's E0382 pattern.** Re-reading rustc more carefully: error code
   E0382 covers "use of moved value", "borrow of moved value", "assign
   to part of moved value", and "drop of moved value" — ALL four share
   a single error code with contextual wording dispatched from a
   `MoveOutAction` enum. This is exactly Option C.

3. **Site count.** 25 sites consolidating into 1 function strictly
   beats 16 + 4 + 5 = 3 separate functions. The 5 "freeing" sites are
   trivially handled by adding `case IR_USE_FREE:` to the switch.

4. **Original draft argued AGAINST this** based on Rust precedent —
   that argument was wrong. Rust DOES centralize.

**Implementation:** see updated section 6.5 and Appendix A.5.

### Q6: Naming consistency

Existing IR helpers use prefixes: `ir_check_*`, `ir_find_*`, `ir_propagate_*`,
`ir_extract_*`, `ir_should_*`, `ir_is_*`, `ir_classify_*`. New helpers
should follow:

- `ir_state_join` — `ir_join_state` would match `ir_*_state` pattern but
  reads less naturally. Either fine.
- `ir_alias_copy_provenance` — consistent with existing.
- `ir_init_handle` — `ir_handle_init`? `ir_register_handle`? Either fine.
- `ir_record_freed` / `ir_record_transferred` — consistent.
- `ir_report_invalid_use` — consistent (matches `ir_zc_error`).

Recommend stick with proposed names.

---

## 12. Out of Scope (Explicitly NOT Doing)

The following were considered and explicitly DEFERRED:

### NOT doing: `ir_find_handle` API merge

Earlier analysis suggested merging the bare-only and compound-aware
lookup functions. Audit revealed: of 47 `ir_find_handle` (bare) call
sites, the vast majority are LEGITIMATELY bare:

- 8 sites are explicit `if (path_len == 0) ... ir_find_handle(...)` — bare is correct.
- 5 sites are IR_COPY/IR_CAST sources — semantically always bare.
- 22 sites operate on a single `int local` — no path involved, bare correct.
- ~12 sites resolve via `ir_extract_compound_key` then check `path_len` —
  legitimate bare fallback.

Renaming or merging would be cosmetic with no real bug-class to prevent.

### NOT doing: walker generalization

`ir_check_expr_uaf` and `ir_check_expr_wrong_pool` are 90% structurally
identical (same recursion shape, different leaf check). Could be
generalized to a higher-order walker `ir_check_expr_with(predicate)`.

REJECTED because:
- Only 2 walkers today. 2 is below the threshold for abstraction.
- Higher-order C functions are unwieldy.
- A third walker (Phase 5 atomic ordering?) would force refactor anyway.
- Risk-to-reward unfavorable.

If a 3rd walker is added in the future, revisit.

### NOT doing: VST-extraction for new helpers

`ir_state_join` is a pure predicate that COULD be extracted to
`src/safety/state_join.c` and verified by VST. This adds significant
overhead (Coq spec, VST proof, Makefile changes) for a function that's
already mechanically obvious from the table.

DEFER to later if/when other Level 3 extractions are happening. The
pattern is well-suited but not blocking.

### NOT doing: Symbol struct refactor

The Symbol struct in checker.c has 30+ fields and complex creation
patterns. A similar refactor could apply but checker.c isn't the
target of this work. The IR analyzer is the focus.

### NOT doing: Phase 5 prep

Phase 5 (atomic ordering) will add a new state machine for
`OrderingState`. It will have its own helpers. This refactor doesn't
prep Phase 5 — it consolidates EXISTING handle-state code.

### NOT doing: MLXDT branch's parallel findings

Branch `claude/cool-johnson-MLXDT` (commits b5f752b + 5c7482c, NOT yet
merged to main) contains an audit landing 8 silent-gap fixes. Audit on
2026-05-10 confirmed: only **BUG-660** is in the helper-layer scope of
this refactor. The other 7 fixes are tangential:

| MLXDT fix | Concern | In our scope? |
|---|---|---|
| BUG-660 (compound pool_name) | Alias-copy field drift | **YES** — adds 7th site to L1882 |
| BUG-661 (@bswap width) | Type-checker validation in checker.c | NO |
| BUG-662 (@atomic_store/cas width) | Same | NO |
| BUG-663 (4 OOM realloc sites) | Internal array mgmt in zercheck_ir.c | NO — not IRHandleInfo init/alias sites |
| BUG-664 (5 dead-stub patterns) | Emitter default cases | NO |
| BUG-665 (defer NODE_IF elision) | Emitter defer body walk | NO |
| Stale ZER_DUAL_RUN comment | Doc cleanup | NO |
| `ir_classify_method_call` wrapper removal | Code cleanup | NO |

The MLXDT branch ALSO reports **14 follow-up gaps** that need separate
sessions (12 unused VST predicates, multiple distinct-unwrap holes in
checker.c, slice end<=len validation, etc.). These are independent of
this refactor.

**Decision:** when this refactor is executed, the implementer should
either:
- Wait for MLXDT to be merged, then refactor on top (preferred — doesn't
  conflict with their fixes; just consolidates the same patterns)
- Coordinate to apply BUG-660's fix as part of this refactor's
  `ir_alias_copy_provenance` migration

Either way, the refactor's helper layer makes BUG-660-class bugs
impossible to ship again — which is the entire point.

---

## Appendix A: Illustrative Code Samples

The samples below illustrate intent. The actual implementation should
be derived from reading the current `zercheck_ir.c` and matching its
conventions (memset patterns, comment style, error helpers, etc.).

### A.1 `ir_state_join`

```c
/* ============================================================
 * Lattice join — state pair → joined state.
 *
 * Replaces the 7-case cascade in ir_merge_states (pre-refactor:
 * lines 497-516). Adding a new IRHandleState requires extending
 * the table — compile-time check on dimensions catches misses.
 *
 * Lattice properties:
 *   - Symmetric: ir_state_join(a, b) == ir_state_join(b, a)
 *   - Monotonic: result is "at least as conservative" as either input
 *   - MAYBE_FREED is the top of {ALIVE, FREED, MAYBE_FREED}
 *   - TRANSFERRED is its own equivalence class (only joins to itself,
 *     widens to MAYBE_FREED with anything else)
 *   - UNKNOWN is bottom — joins to whatever the other side is
 *
 * NOTE: 6 cells differ from current cascade behavior (4 in UNK row +
 * 2 defensive XFER↔FREED). All 6 are believed unreachable today; the
 * table makes the formally-correct lattice choice. See section 6.1
 * "Implementation choice" and section 11 Q1 for full rationale.
 * ============================================================ */

#define _U IR_HS_UNKNOWN
#define _A IR_HS_ALIVE
#define _F IR_HS_FREED
#define _M IR_HS_MAYBE_FREED
#define _T IR_HS_TRANSFERRED

static const IRHandleState ir_state_join_table[5][5] = {
    /*                  UNK  ALIVE  FREED  MAYBE  XFER  */
    /* UNK   */       { _U,  _A,    _F,    _M,    _T   },
    /* ALIVE */       { _A,  _A,    _M,    _M,    _M   },
    /* FREED */       { _F,  _M,    _F,    _M,    _M   },
    /* MAYBE */       { _M,  _M,    _M,    _M,    _M   },
    /* XFER  */       { _T,  _M,    _M,    _M,    _T   },
};

#undef _U
#undef _A
#undef _F
#undef _M
#undef _T

static IRHandleState ir_state_join(IRHandleState a, IRHandleState b) {
    /* Compile-time bound check. If new states are added beyond 5,
     * this triggers an array-bounds warning. */
    return ir_state_join_table[a][b];
}
```

Migration in `ir_merge_states`:

```c
/* BEFORE (7-case cascade) */
if (rh->state == IR_HS_ALIVE && ph->state == IR_HS_FREED) {
    rh->state = IR_HS_MAYBE_FREED;
    rh->free_line = ph->free_line;
} else if (rh->state == IR_HS_FREED && ph->state == IR_HS_ALIVE) {
    rh->state = IR_HS_MAYBE_FREED;
} /* ... 5 more cases ... */

/* AFTER */
IRHandleState joined = ir_state_join(rh->state, ph->state);
if (joined != rh->state) {
    rh->state = joined;
    /* Preserve free_line when widening to MAYBE_FREED from ALIVE.
     * (The diagnostic info "freed at line N" comes from ph in this case.) */
    if (joined == IR_HS_MAYBE_FREED && ph->free_line) {
        rh->free_line = ph->free_line;
    }
}
```

### A.2 `ir_alias_copy_provenance`

```c
/* ============================================================
 * Copy alias-propagating fields from src to dst.
 *
 * Mirrors checker.c:propagate_escape_flags. Replaces ad-hoc
 * field-copy blocks at IR_COPY (line ~1582), orelse-ident
 * (~1918), NODE_IDENT non-move alias (~2154), and selected
 * alias paths in IR_ASSIGN.
 *
 * Does NOT copy:
 *   - escaped: per-site decision (IR_CAST/@ptrcast propagate;
 *     IR_COPY does not). Sites that need it set after calling.
 *
 * Add new IRHandleInfo fields HERE to ensure they propagate
 * through all alias relationships consistently.
 * ============================================================ */

static void ir_alias_copy_provenance(IRHandleInfo *dst,
                                      const IRHandleInfo *src) {
    dst->state            = src->state;
    dst->alloc_line       = src->alloc_line;
    dst->free_line        = src->free_line;
    dst->alloc_id         = src->alloc_id;
    dst->source_color     = src->source_color;
    dst->is_thread_handle = src->is_thread_handle;
    dst->pool_name        = src->pool_name;
    dst->pool_name_len    = src->pool_name_len;
    /* escaped intentionally NOT copied — see comment above */
}
```

Migration in IR_COPY (line ~1582):

```c
/* BEFORE */
IRHandleState src_state = src_h->state;
int src_alloc_line = src_h->alloc_line;
int src_alloc_id = src_h->alloc_id;
int src_color = src_h->source_color;
bool src_is_th = src_h->is_thread_handle;
const char *src_pool = src_h->pool_name;
uint32_t src_pool_len = src_h->pool_name_len;
IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
if (dst_h) {
    dst_h->state = src_state;
    dst_h->alloc_line = src_alloc_line;
    dst_h->alloc_id = src_alloc_id;
    dst_h->source_color = src_color;
    dst_h->is_thread_handle = src_is_th;
    dst_h->pool_name = src_pool;
    dst_h->pool_name_len = src_pool_len;
}

/* AFTER */
IRHandleInfo src_snapshot = *src_h;  /* UAF guard: snapshot before realloc */
IRHandleInfo *dst_h = ir_add_handle(ps, inst->dest_local);
if (dst_h) {
    ir_alias_copy_provenance(dst_h, &src_snapshot);
}
```

### A.3 `ir_init_handle`

```c
/* ============================================================
 * Allocation source classification. Used by ir_init_handle to
 * apply kind-specific defaults. Adding a new kind requires
 * extending this enum AND the switch in ir_init_handle.
 * ============================================================ */

typedef enum {
    IR_ALLOC_POOL,    /* Pool/Slab.alloc → ZC_COLOR_POOL */
    IR_ALLOC_ARENA,   /* arena.alloc → ZC_COLOR_ARENA, skip leak-check */
    IR_ALLOC_MALLOC,  /* extern malloc → ZC_COLOR_MALLOC */
    IR_ALLOC_PARAM,   /* auto-registered param → escaped=true */
    IR_ALLOC_THREAD,  /* spawn ThreadHandle → is_thread_handle=true */
    IR_ALLOC_MOVE,    /* move struct local — color irrelevant */
    IR_ALLOC_OPAQUE,  /* unknown/fallback */
} IRAllocKind;

/* ============================================================
 * Register a new handle entry in path state.
 *
 * Mirrors types.c:scope_add — zero-init slot, set core fields,
 * leave kind-specific fields to switch.
 *
 * Caller responsibilities:
 *   - Provide alloc_id (typically: dest_local for direct allocs,
 *     _ir_next_alloc_id++ for fresh-alloc-id needs)
 *   - Provide pool_name (extracted via ir_extract_pool_name) for
 *     IR_ALLOC_POOL; pass NULL/0 otherwise
 *   - Check overwrite-while-alive BEFORE calling (caller has
 *     access to the source line for the diagnostic)
 *
 * Returns the handle (existing or freshly initialized). Returns
 * NULL on allocation failure.
 * ============================================================ */

static IRHandleInfo *ir_init_handle(IRPathState *ps, int local_id,
                                     int alloc_id, IRAllocKind kind,
                                     int line, const char *pool_name,
                                     uint32_t pool_name_len) {
    IRHandleInfo *h = ir_add_handle(ps, local_id);
    if (!h) return NULL;

    h->state            = IR_HS_ALIVE;
    h->alloc_line       = line;
    h->alloc_id         = alloc_id;
    h->pool_name        = pool_name;
    h->pool_name_len    = pool_name_len;

    switch (kind) {
    case IR_ALLOC_POOL:
        h->source_color = ZC_COLOR_POOL;
        break;
    case IR_ALLOC_ARENA:
        h->source_color = ZC_COLOR_ARENA;
        break;
    case IR_ALLOC_MALLOC:
        h->source_color = ZC_COLOR_MALLOC;
        break;
    case IR_ALLOC_PARAM:
        h->source_color = ZC_COLOR_UNKNOWN;
        h->escaped      = true;  /* extern input — don't flag as leak */
        break;
    case IR_ALLOC_THREAD:
        h->source_color     = ZC_COLOR_UNKNOWN;
        h->is_thread_handle = true;
        break;
    case IR_ALLOC_MOVE:
    case IR_ALLOC_OPAQUE:
        h->source_color = ZC_COLOR_UNKNOWN;
        break;
    }
    return h;
}
```

Migration in IR_POOL_ALLOC (line ~1657):

```c
/* BEFORE */
IRHandleInfo *h = ir_add_handle(ps, inst->dest_local);
if (h) {
    if (h->state == IR_HS_ALIVE && /* overwrite check */
        inst->dest_local < func->local_count &&
        !func->locals[inst->dest_local].is_temp) {
        ir_zc_error(zc, inst->source_line,
            "handle %%%d overwritten while alive — previous leaked",
            inst->dest_local);
    }
    h->state = IR_HS_ALIVE;
    h->alloc_line = inst->source_line;
    h->alloc_id = inst->dest_local;
    h->source_color = ZC_COLOR_POOL;
    /* pool_name extraction */
    const char *pname; uint32_t pname_len;
    ir_extract_pool_name(rhs, &pname, &pname_len);
    h->pool_name = pname;
    h->pool_name_len = pname_len;
}

/* AFTER */
const char *pname; uint32_t pname_len;
ir_extract_pool_name(rhs, &pname, &pname_len);

IRHandleInfo *existing = ir_find_handle(ps, inst->dest_local);
if (existing && existing->state == IR_HS_ALIVE &&
    inst->dest_local < func->local_count &&
    !func->locals[inst->dest_local].is_temp) {
    ir_zc_error(zc, inst->source_line,
        "handle %%%d overwritten while alive — previous leaked",
        inst->dest_local);
}

ir_init_handle(ps, inst->dest_local, inst->dest_local,
               IR_ALLOC_POOL, inst->source_line, pname, pname_len);
```

### A.4 `ir_record_freed` / `ir_record_transferred`

```c
/* ============================================================
 * State transitions: encapsulate the multi-step pattern of
 * setting state, recording line, and (for FREED) propagating
 * to alloc_id aliases.
 *
 * Replaces 5 FREED-transition sites and 11 TRANSFERRED-transition
 * sites with two thin wrappers.
 *
 * Critical design decision (don't change without auditing):
 *   - FREED propagates to aliases. Aliases share the same
 *     underlying allocation, so freeing one frees all.
 *   - TRANSFERRED does NOT propagate. The semantics of TRANSFERRED
 *     today (move struct, spawn arg) all use either fresh alloc_id
 *     (no aliases) or types where alloc_id-based aliasing doesn't
 *     apply. If a future TRANSFERRED-causing op breaks this
 *     assumption, audit and update HERE.
 * ============================================================ */

static void ir_record_freed(IRPathState *ps, IRHandleInfo *h, int line) {
    h->state = IR_HS_FREED;
    h->free_line = line;
    ir_propagate_alias_state(ps, h, IR_HS_FREED, line);
}

static void ir_record_transferred(IRHandleInfo *h, int line) {
    h->state = IR_HS_TRANSFERRED;
    h->free_line = line;  /* reused for diagnostic — "transferred at line N" */
    /* Intentional: no alias propagation. See comment above. */
}
```

### A.5 `ir_report_invalid_use`

```c
/* ============================================================
 * Central error reporter for "handle in invalid state" diagnostics.
 *
 * Replaces 25 ad-hoc ir_zc_error sites across 3 contexts with
 * consistent state-aware messages. Maps to Rust's E0382 error code
 * pattern: single predicate (handle invalid), contextual wording.
 *
 * Caller responsibilities:
 *   - Verify h is non-NULL and ir_is_invalid(h) before calling
 *   - Provide local_id (>=0) for bare locals; provide path/path_len
 *     for compound entities
 *   - Provide func for type lookup (move-struct distinction)
 *   - Specify context: USE_READ / USE_RETURN / USE_FREE
 * ============================================================ */

typedef enum {
    IR_USE_READ,    /* use, deref, field/index access */
    IR_USE_RETURN,  /* return statement */
    IR_USE_FREE,    /* free attempt (double-free / free-after-move) */
} IRUseContext;

static void ir_report_invalid_use(ZerCheck *zc, IRHandleInfo *h, int line,
                                   int local_id, const char *path,
                                   uint32_t path_len, IRFunc *func,
                                   IRUseContext ctx) {
    /* Build display key: compound path or local name */
    const char *name = "?";
    int nlen = 1;
    if (path_len > 0 && path) {
        name = path;
        nlen = (int)path_len;
    } else if (local_id >= 0 && func && local_id < func->local_count) {
        name = func->locals[local_id].name;
        nlen = (int)func->locals[local_id].name_len;
    }

    /* Move-struct vs regular handle distinction for wording */
    bool is_move = false;
    if (local_id >= 0 && func && local_id < func->local_count) {
        Type *lt = func->locals[local_id].type;
        is_move = ir_should_track_move(lt);
    }

    /* Verb dispatch by context */
    const char *verb_use, *verb_freed, *verb_moved, *verb_transferred;
    switch (ctx) {
    case IR_USE_READ:
        verb_use = "use of"; verb_freed = "use after free";
        verb_moved = "use after move"; verb_transferred = "use after transfer";
        break;
    case IR_USE_RETURN:
        verb_use = "returning"; verb_freed = "returning freed pointer";
        verb_moved = "returning moved value"; verb_transferred = "returning transferred handle";
        break;
    case IR_USE_FREE:
        verb_use = "freeing"; verb_freed = "double free";
        verb_moved = "free after move"; verb_transferred = "free after transfer";
        break;
    }

    /* State dispatch */
    switch (h->state) {
    case IR_HS_FREED:
        ir_zc_error(zc, line, "%s: '%.*s' freed at line %d",
                    verb_freed, nlen, name, h->free_line);
        break;
    case IR_HS_MAYBE_FREED:
        if (is_move) {
            ir_zc_error(zc, line,
                "%s: '%.*s' may have been moved on a previous path",
                verb_moved, nlen, name);
        } else {
            ir_zc_error(zc, line,
                "%s: '%.*s' may have been freed at line %d",
                verb_freed, nlen, name, h->free_line);
        }
        break;
    case IR_HS_TRANSFERRED:
        if (h->is_thread_handle) {
            ir_zc_error(zc, line,
                "%s: '%.*s' transferred to thread at line %d",
                verb_transferred, nlen, name, h->free_line);
        } else if (is_move) {
            ir_zc_error(zc, line,
                "%s: '%.*s' ownership transferred at line %d",
                verb_moved, nlen, name, h->free_line);
        } else {
            ir_zc_error(zc, line,
                "%s: '%.*s' transferred at line %d",
                verb_transferred, nlen, name, h->free_line);
        }
        break;
    case IR_HS_UNKNOWN:
    case IR_HS_ALIVE:
        /* Not invalid — caller shouldn't have called us. */
        break;
    }
}
```

---

## Appendix B: Bug History Mapped to Helpers

Each bug class above traces to a specific helper. A future audit
script (section 7) detects regressions in each.

| Bug | Date | Class | Helper that prevents | Detection mechanism |
|---|---|---|---|---|
| BUG-468/469 (AST) | older | Missing helper layer | (refactor result, AST done) | — |
| BUG-650 #1 cases | 2026-05-02 | Lattice cascade | `ir_state_join` | Audit: cascade > 5 cases |
| BUG-650 #2 API | 2026-05-02 | Wrong API choice | (n/a — different gap) | Audit: bare lookup near compound context |
| F0.3 | 2026-05-03 | Wrong API choice | (n/a — different gap) | Same as above |
| F3.2 #1 cases | 2026-05-04 | Lattice cascade | `ir_state_join` | Same audit as BUG-650 |
| F3.2 #2 alias | 2026-05-04 | Alias-copy field drift | `ir_alias_copy_provenance` | Audit: 3+ field copies outside helper |
| **BUG-660 (MLXDT)** | **2026-05-10** | **Alias-copy field drift (compound site)** | **`ir_alias_copy_provenance`** | **Same audit as F3.2 #2** |
| Latent: pool_name in IR_CAST | latent | Alias-copy field drift | Same as above | Same audit |

The two API-confusion bugs (BUG-650 #2, F0.3) are NOT addressed by this
refactor — they're a separate concern, deemed not worth refactoring
(see section 12 "Out of Scope"). The other **5 bugs** are all caught by
this refactor's helpers.

**BUG-660 update (2026-05-10):** A parallel audit session on branch
`claude/cool-johnson-MLXDT` independently rediscovered the F3.2 #2
field-drift class at a 7th alias-copy site (L1882, compound handle from
parent in IR_ASSIGN). They patched it manually. This is the 7th
documented instance of the same bug class — the refactor's
`ir_alias_copy_provenance` would catch all 7 by construction.

---

## Appendix C: Site Inventory (Quantitative)

Site counts re-audited against `zercheck_ir.c` at HEAD on 2026-05-10.
Original draft on 2026-05-05 had counting errors; this section is
the corrected inventory.

### Handle-init sites (22-25 total — was claimed 17)

| Line | Context | Kind |
|---|---|---|
| 1493 | Spawn arg auto-register (move struct) | IR_ALLOC_MOVE |
| 1545 | IR_COPY move struct dst | IR_ALLOC_MOVE (with override) |
| 1620 | IR_CAST param auto-register | IR_ALLOC_PARAM |
| 1658 | IR_POOL_ALLOC | IR_ALLOC_POOL |
| 1880 | IR_SLAB_ALLOC | IR_ALLOC_POOL |
| 1984 | @ptrcast param auto-register | IR_ALLOC_PARAM |
| 2074 | IR_ASSIGN orelse temp ALLOC | IR_ALLOC_POOL |
| 2138 | NODE_IDENT alias move-dst | IR_ALLOC_MOVE (with override) |
| 2152 | NODE_IDENT alias non-move | (uses ir_alias_copy_provenance instead) |
| 2400 | IR_CALL dest auto-register | IR_ALLOC_PARAM |
| 2421 | IR_CALL ALLOC dest | IR_ALLOC_POOL |
| 2487 | IRMC_FREE param auto-register | IR_ALLOC_PARAM |
| 2573 | IR_CALL alloc-returning dest | IR_ALLOC_POOL |
| 2615 | FuncSummary frees-param register | IR_ALLOC_PARAM |
| 2661 | IR_CALL pointer-returning ALIVE | IR_ALLOC_OPAQUE |
| 2704 | Param-color inheritance dst | IR_ALLOC_ARENA (or POOL) |
| 2738 | returns_color application | (varies) |
| 2774 | IR_RETURN escape mark auto-register | IR_ALLOC_PARAM |
| 2839 | Spawn ThreadHandle register | IR_ALLOC_THREAD |
| 2888 | Misc handle creation | IR_ALLOC_OPAQUE |

**Coverage estimate:** 16-19 of 22-25 sites fit `ir_init_handle` factory
cleanly. Remaining 3-6 sites have specialty patterns (returns_color
application, param-color inheritance, IR_RETURN escape-mark with extra
flags) that stay ad-hoc with documented rationale per site.

### Alias-copy sites (7 true alias + 2 init-with-fresh-id)

| Line | Context | Classification |
|---|---|---|
| 1547 | IR_COPY move struct dst | INIT-with-fresh-id (covered by `ir_init_handle`, NOT this helper) |
| 1582 | IR_COPY general | TRUE ALIAS — uses `ir_alias_copy_provenance` |
| 1644 | IR_CAST | TRUE ALIAS — helper + override `escaped` |
| **1882** | **IR_ASSIGN compound-handle from rh** | **TRUE ALIAS — uses helper. NEW: discovered via MLXDT BUG-660 audit on 2026-05-10. Originally missed.** |
| 1918 | IR_ASSIGN orelse-ident | TRUE ALIAS — uses helper |
| 2002 | IR_ASSIGN @ptrcast | TRUE ALIAS — helper + override `escaped` |
| 2039 | IR_ASSIGN &-interior | TRUE ALIAS — uses helper (different alloc identity is preserved by sharing alloc_id with parent) |
| 2138 | IR_ASSIGN NODE_IDENT move-dst | INIT-with-fresh-id (covered by `ir_init_handle`) |
| 2154 | IR_ASSIGN NODE_IDENT non-move alias | TRUE ALIAS — uses helper |

**7 of 7 true alias-copy sites use `ir_alias_copy_provenance`.**
The 2 init-with-fresh-id sites are handled by `ir_init_handle` (different
helper). Doc previously claimed 6 true-alias sites; the 7th at L1882
was missed by the original audit and rediscovered on branch
`claude/cool-johnson-MLXDT` (BUG-660). This is direct evidence the
hand-counting approach is fragile and the refactor is needed.

### Lattice merge cases (1 site, 7 cases pre-refactor)

`ir_merge_states` at lines 497-516. Replace cascade with single
`ir_state_join` call.

### State transition sites

FREED transitions (5 sites — all use `ir_record_freed`):
- L1257 (defer scan)
- L1716 (IR_POOL_FREE)
- L2459 (IRMC_ARENA_RESET — batched, uses propagate inline)
- L2514 (IRMC_FREE direct)
- L2724 (IRMC_FREE_PTR direct)

TRANSFERRED transitions (11 sites — all use `ir_record_transferred`):
- L1448 (Z2 asm transfer)
- L1502 (spawn arg)
- L1542 (IR_COPY move src)
- L1956 (NODE_INDEX move-extract)
- L2134 (NODE_IDENT move src)
- L2210 (IR_RETURN move primary)
- L2241 (IR_RETURN move fallback)
- L2387 (Cross-module summary)
- L2828 (Spawn arg in IR_NOP)
- L2893 (return move)
- L2960 (IR_RETURN escape transfer)

### Error sites (corrected: 25 total across 3 tiers)

**Tier 1 — "use of invalid handle" (16 sites, MUST fold into helper):**

Lines: 974, 1535, 1560, 1632, 1728, 1779, 1783, 1950, 2096, 2124, 2128,
2164, 2381, 2533, 2888, 2955.

**Tier 2 — "returning invalid handle" (4 sites, RECOMMEND fold via
`ZER_USE_RETURN` context flag):**

Lines: 2205, 2217, 2236, 2252.

**Tier 3 — "freeing invalid handle" (5 sites, KEEP SEPARATE):**

Lines: 1704, 1708, 2502, 2506, 2719.

These are conceptually distinct (double-free / free-after-move) — Rust
keeps them separate too. See section 11 Q7.

**Other error sites that STAY direct (not unified):** double free
proper (1700-1702), leak detection at function exit, wrong pool
(F3.2), ghost handle (D6), thread not joined (D3). Different conceptual
errors with their own message structures.

---

## Appendix D: Architectural Comparison Table

Side-by-side comparison of the helper layer in deleted zercheck.c (AST)
vs current zercheck_ir.c (IR), as of 2026-05-05.

### State classification helpers

| Helper | AST `zercheck.c` | IR `zercheck_ir.c` | Status |
|---|---|---|---|
| `is_handle_invalid` | ✓ delegate to VST | ✓ `ir_is_invalid` (delegate) | Parity |
| `is_handle_consumed` | ✓ separate function | ✗ inlined into merge | **Gap** |
| State JOIN | implicit cascade | implicit cascade (7 cases) | Both fragile |
| `ir_state_name` (debug) | — | ✓ | IR-only |

### Type classification helpers

| Helper | AST | IR | Status |
|---|---|---|---|
| `is_move_struct_type` | ✓ | ✓ `ir_is_move_struct_type` | Parity |
| `contains_move_struct_field` | ✓ recursive | ✓ `ir_contains_move_struct_field` | Parity |
| `should_track_move` | ✓ | ✓ `ir_should_track_move` | Parity |

### Call classification helpers

| Helper | AST | IR | Status |
|---|---|---|---|
| `is_alloc_call` | ✓ | ✓ `ir_is_extern_alloc_call` | Parity (renamed) |
| `is_free_call` | ✓ + name heuristic | ✓ `ir_is_extern_free_call` | Parity |
| `name_looks_like_destructor` | ✓ standalone | bundled into ir_is_extern_free_call | Equivalent |
| Method classification (alloc/get/free) | inline in cases | ✓ `ir_classify_method_call` | IR addition |
| Pool-receiver name extraction | implicit (pool_id) | ✓ `ir_extract_pool_name` | IR addition (F3.2) |

### Key extraction helpers

| Helper | AST | IR | Status |
|---|---|---|---|
| Compound key (string) | `handle_key_arena` | replaced by | Different architecture |
| Compound key (struct) | — | `ir_extract_compound_key` | IR-only |

### Path state operations

| Helper | AST | IR | Status |
|---|---|---|---|
| init / copy / free | `pathstate_*` | `ir_ps_*` | Parity |
| Equality (convergence) | ✓ explicit | implicit in convergence loop | Different shape |

### Alias propagation

| Helper | AST | IR | Status |
|---|---|---|---|
| FREED state propagation | inline (5 sites, drift risk) | ✓ `ir_propagate_alias_state` | IR better |
| Field-copy on alias | scattered (multiple sites) | scattered (8 sites) | **Both fragile** |
| Init pattern on register | scattered | scattered (17 sites) | **Both fragile, IR worse** |

### Error reporting

| Helper | AST | IR | Status |
|---|---|---|---|
| `*_report_invalid_use` | ✓ centralized | ✗ scattered | **Gap** |

### Walkers

| Helper | AST | IR | Status |
|---|---|---|---|
| Recursive UAF walker | inline checks | ✓ `ir_check_expr_uaf` | IR addition |
| Recursive wrong-pool walker | — | ✓ `ir_check_expr_wrong_pool` (F3.2) | IR addition |
| Defer body scanner | ✓ | ✓ `ir_defer_scan_frees` | Parity |

### CFG operations (IR-only)

| Helper | AST | IR | Status |
|---|---|---|---|
| Path state merge | scope_depth hack | ✓ `ir_merge_states` | IR addition (better) |
| Compound handle aware | string-based | path-based | IR addition |

### Function summary support

| Helper | AST | IR | Status |
|---|---|---|---|
| `func_returns_color` | ✓ | partial via FuncSummary | Different architecture |
| Iterative summary build | inline | inline (zerc_main) | Parity |

### Summary

| Concern | Status |
|---|---|
| Type/call classification | Parity (good) |
| Path state ops | Parity (good) |
| FREED alias propagation | IR centralized (good) |
| CFG-aware merge | IR addition (better than AST) |
| Compound key tracking | IR addition (better) |
| Recursive walkers | IR addition (better) |
| **State JOIN logic** | **Both fragile, IR has 4 prior bugs** |
| **Alias field-copy** | **IR has 8 sites with drift** |
| **Handle init pattern** | **IR has 17 sites with drift** |
| **TRANSFERRED transition** | **IR has 11 sites, 0 documented** |
| **Error message centralization** | **AST had it, IR doesn't** |

The 5 bottom rows are this refactor's targets.

---

## Appendix E: References

### Files referenced

- `zercheck_ir.c` — primary file under refactor
- `zercheck.c` — current 150-line shim (post-Phase F1)
- Git history: `7ffbd9d^:zercheck.c` — deleted 3128-line AST analyzer
  (precursor with the unified-helper pattern)
- `zercheck.h` — shared `ZerCheck` struct + API
- `checker.c` — has `propagate_escape_flags` precedent
- `types.c` — has `scope_add` factory precedent
- `types.h` — Symbol, Type, Scope structs
- `ir.h` — IRLocal, IRBlock, IRFunc, IRInst structs
- `src/safety/handle_state.c` — VST-verified state predicate
- `src/safety/escape_rules.c` — VST-verified escape predicate

### Tools referenced

- `tools/walker_audit.sh` — model for new audit script
- `tools/walker_default_audit.sh` — model for exhaustiveness checks
- `tools/audit_matrix.sh` — model for cross-reference audits
- `tools/audit_fixed_buffers.sh` — model for baseline-with-grandfather audits
- `tools/agreement_audit.sh` — IR vs AST cross-check (Phase F0.1)

### Related documentation

- `BUGS-FIXED.md` Session 2026-05-03 (Phase F migration)
- `BUGS-FIXED.md` Session 2026-05-04 (Phase F3.2)
- `docs/limitations.md` — closed gaps including F3.2 patterns
- `docs/cfg_migration_plan.md` — Phase F migration retrospective
- `docs/compiler-internals.md` — ZER-CHECK Architecture section
- `CLAUDE.md` — Phase F migration rules + ban-vs-track framework

### Key commits

- `7ffbd9d` — Phase F3 deletion of original zercheck.c
- `f813829` — Phase F1 zerc binary uses zercheck_ir as sole driver
- `09bf463` — BUG-650 fix (compound-aware merge + missing cases)
- `2021631` — F0.3 fix (compound-aware convergence check)
- `4a37214` — F3.2 fix (wrong pool + free-realloc loop)
- `ce1d82a` — F3.1 fix (overwrite leak + struct copy alias)

### Bugs cross-referenced

- **BUG-468/469** (AST) — original trigger of unified-helper pattern in
  zercheck.c. Comment in `7ffbd9d^:zercheck.c:1095-1100`.
- **BUG-421** (checker) — referenced in `propagate_escape_flags` comment
  at `checker.c:881`.
- **BUG-650** — IR analyzer compound-aware lookups (and missing merge
  cases). Documented in `BUGS-FIXED.md` Session 2026-05-02.
- **BUG-573, BUG-577** — walker missing-NODE_-kind class. Mentioned in
  `tools/walker_audit.sh` header.
- **F0.3, F3.2** — recent IR analyzer fixes. Documented in BUGS-FIXED.md
  Session 2026-05-03 and 2026-05-04.

### CLAUDE.md sections referenced

- "ZER Safety Architecture — 4 Models, 29 Systems"
- "Ban Decision Framework — when banning IS correct"
- "Don't add features, refactor, or introduce abstractions beyond what
  the task requires."
- "Stage 2 Part B (walker exhaustiveness via -Wswitch)" — precedent for
  this refactor's audit-script approach
- "TYPE_DISTINCT must be unwrapped before ANY type dispatch" — same
  bug-class pattern, addressed at scale (35+ sites in one session)

---

## End of Document

This document is a planning artifact. It contains the analysis,
rationale, and specifications needed to execute the refactor in a
fresh session. The fresh implementer should:

1. Read this document end-to-end.
2. Read `zercheck_ir.c` (current state).
3. Read `checker.c:875-890` (`propagate_escape_flags` precedent).
4. Read `types.c:499-525` (`scope_add` precedent).
5. Read git: `git show 7ffbd9d^:zercheck.c | sed -n '1115,1175p'`
   (deleted AST helpers).
6. Implement helpers per section 6 specs.
7. Migrate sites per appendix C inventory.
8. Add audit script per section 7.
9. Run full test suite per section 9.
10. Commit per section 8 boundaries.

Sample code in Appendix A is illustrative — actual implementation
should match the file's existing conventions (memset patterns,
realloc-safe snapshot patterns, comment style, error helper usage).

The refactor preserves all current functionality. No new safety
properties added, no user-facing behavior changes. Only internal code
shape improves.

---
