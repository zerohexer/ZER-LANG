# ZER Compiler — Closed limitations (archive)

**Append-only history. Do not read this file whole** — it exists so that a closed entry's
measurement, root-cause discussion and "why the obvious fix was wrong" are not lost.
Look an entry up by its BUG number or a phrase of its title:

    grep -n "BUG-1651" docs/limitations-closed.md

Nothing here is open. What a fix left open is a separate entry in `docs/limitations.md`
(tagged `[residual of: …]`). An entry lands here in the same commit that closes it; new
entries go at the END. The durable memory of a closed hole is its regression test or grid
cell — this text is the explanation, not the protection.

Seeded 2026-11-13 from the pre-split `docs/limitations.md`: every section whose HEADING
declared it closed, the closed list items of the sections that stayed open (each group
under its source heading, marked `[closed items only …]`), and the old HANDOFF preamble.
Text is verbatim.

---



PREAMBLE — the 2026-08-26 HANDOFF block that opened the old file
# ZER Compiler — Known Limitations

Living document of known compiler limitations, audit findings, and deferred fixes.
Entries removed once fixed.

---

# HANDOFF — read this first (updated 2026-08-26: TRACKER 3 IS CLOSED)

**ALL NINE `vigilant-tesla` BRANCHES ARE FULLY CONSUMED. Every row of all three harvest
trackers is closed — 45 + 21 + 20.** Nothing remains to cherry-pick. All three harvests are done: 45 rows (tracker 1), 21 (tracker
2, mostly superseded), 20 (tracker 3). The cherry-pick recipe below is kept because it
WORKED and the next branch will want it, not because anything is pending.

Landed 2026-08-26, newest first — every one adopted from `as71kk` rather than re-derived:

| sha | rows | what |
|---|---|---|
| `015a6bc6` | A13, A14, tooling | indeterminate `lower_expr` return, `@trap` arity, warnings **28 -> 0**, 280 lines of dead code (incl. three STALE COPIES of a safety walker), the structural `.gitignore` (1084 binaries untracked, all verified ELF) |
| `b1949a25` | A15..A19 | five idioms ZER documented and refused: `&&` narrowing (my mis-closed row), const slice, const array size, funcptr arrays, bit extraction through a pointer. Adds the reference-example gate |
| `70a041e7` | A8, A9 | indirect-call keep carrier (ASan stack-use-after-return), six intrinsics storing through `const`, and the `grep -qF` harness bug + its two siblings |
| `5f1cb5ac` | A5, A6, A7 | a NULL `?u32` equal to 0, the drifted non-null rule, `free()` of an inline-array view |
| `75a76751` | A2, A3, A4 | the `@cstr` integer-to-pointer door, the SIGN half at eight sinks (`value_flows_to`), enum forging via `@truncate` and through carriers |

Earlier: `7c873edd` (view class + container), `c0180e3f` (float saturates), `ae033cd0`,
and the 45-row harvest 1.


This section says what was DECIDED (so it is not re-litigated), the recipe that made the
adoption cheap, and the corrections I made to my OWN earlier work so they are not
repeated.

## OPEN — residuals of the 2026-11-13 audit (BUG-1609..1643; measured, reproducers in `tests/zer_gaps/`)   [closed items only — the open ones stayed in limitations.md]
1. ~~A variable-index view in a callee~~ — CLOSED (BUG-1632 the free through `*H h =
   &s[i]` / a sub-slice local; BUG-1633 the store into a slice parameter's element, direct or
   through a view, and a slice local handed to either kind of callee).

3. ~~A getter of an ADDRESS~~ — CLOSED (BUG-1642: `return &g`; BUG-1641: a field read through
   a local view of a pointer param). Residual: a getter returning `&g.sub` (a projection of a
   global, not the bare global), a getter reading a field through a by-value COPY of the param's
   struct (`H h = c.h; return h.p;` — no slot entry exists to match), and a getter with two
   different `&G` returns (not summarised, so the result is an unrelated value).

4. ~~A global reached through a COPY or a capture~~ — CLOSED (BUG-1640) for a struct copy and
   an optional-struct capture. Residual: a copy of a global ARRAY of structs (the leaf path
   contains a variable-index family `g[*].p`, deliberately not minted as one slot) and a copy
   made through a pointer-to-global that is reassigned.

5. ~~A carrier forwarded to a funcptr PARAMETER~~ — CLOSED (BUG-1638).

6. ~~A `?*T` read out of a slice view of a local array~~ — CLOSED (BUG-1635).
   Residual precision (loud): a VARIABLE index through a view that does not start at 0
   (`[*]?*T u = arr[1..3]; for (i..) { free(u[i]); u[i] = null; }`) is "some element", so
   the reset reaches no specific slot and the loop is refused — the same answer as the
   direct `arr[i + 1]` spelling. Iterate the array, or a view starting at 0.

7. ~~An async task holding a pointer the FUNCTION was given~~ — CLOSED (BUG-1643, parameter and
   global-task-in-helper).

8. ~~`move struct` across a pointer / slice parameter~~ — CLOSED (BUG-1639). Residual
   (over-rejection, loud, unchanged): the crude call-site rule still transfers a ROOT written
   `&w` / `w` passed to ANY function — `peek(&w); peek(&w);` on a struct containing a move
   field is "use after move" although `peek` only reads. The summary bit 32 now exists to
   relax it; doing so is an accept-unsafe change (every way a callee can consume must be in
   the bit) and needs its own audit.

1. ~~A re-entrant `_poll` of the same task~~ — CLOSED (BUG-1628): the poll traps on re-entry.

2. ~~Funcptr reach through a POINTER / SLICE spawn argument~~ — CLOSED (BUG-1631).

3. ~~The scoped-borrow window is textual~~ — CLOSED (BUG-1648 `goto`; BUG-1651 a `break` /
   `continue` / `return` out of the window, and a join releasing ANOTHER handle's
   variable). The rule is now the one `proofs/operational/lambda_zer_concurrency/
   window_cfg.v` proves sound over an arbitrary control-flow graph: a CLOSED window
   (spawn..join in one statement list, no path leaving it with the thread running) uses the
   textual rule; an OPEN window is released only by its handle's TAIL JOIN (branch depth 0,
   outside `@once`, after the last goto / label), and in a function with a goto / label it
   is borrowed from the function's entry. **Residuals (precision, loud):** with an OPEN
   window the lent variable cannot be touched before that tail join (in a labelled function:
   anywhere before it, a write before the spawn included); one variable cannot be lent from
   two spawn sites in a labelled function; a join in EVERY arm of a branch, or a deferred
   join, never releases; an interior `goto` to a label that is itself inside the interior
   opens the window; the atomic-downgrade check treats an open window as never closing.
   Hoist the join, or move the spawn..join into its own function.

4. ~~A helper lends its pointer param and touches the same global by NAME~~ — CLOSED
   (BUG-1645: per-parameter lent-global fact, call-site check), and ~~"same object" by name
   misses a plain local alias~~ — CLOSED (BUG-1646: the pair root follows a pointer local's
   borrow root through a projection). **Residual (precision, loud):** the pair is keyed on the
   ROOT object, so two arguments naming DISJOINT fields of one struct (`sp(&t.x, &t.y)`) are
   refused as "both reach 't'"; a field-path disjointness test would recover it.

5. ~~A lent global reached by the parent's callee through a pointer FIELD of a global
   struct~~ — CLOSED (BUG-1647: the write-target query walks PART-writes of an aggregate name
   and follows the assigned `&g`).

6. ~~The spin-lock recogniser accepts a loop that does not lock~~ — CLOSED (BUG-1644: a
   `break`, a wrong expected value or a non-CAS condition is not a lock).

7. ~~ISR split read-modify-write through MEMORY~~ — CLOSED for regular (main-side) code and
   for a handler's own body (BUG-1650): a global carrier (any order, any number of hops, two
   functions), a local reached through a pointer, a callee's out-parameter, a loop back edge.
   **Residuals:** (a) the global carrier relation is flow-INSENSITIVE, so `mem = g; mem = 0;
   g = mem + 1;` is refused (loud); (b) the split done in HELPERS called only from a handler
   (`interrupt T { rd(); wr(); }`) is not attributed to the handler — the handler side of the
   rule (a handler is not preempted by main) is the conservative half, the main side is the
   lost update and is covered; ~~(c) the SPAWN scan's sink does not read the global carrier
   relation~~ — CLOSED (BUG-1652: both thread sinks now read the main checker's facts).
   **Residuals of BUG-1652 (precision, loud):** the spawner's-window rule ignores locks — a
   read-modify-write in the window under the same CAS lock the thread's store holds is
   refused; a thread's unlocked read-modify-write is refused even when it is the flag's only
   writer (use `@atomic_*`, or the `@once` form); the carrier relation is flow-insensitive,
   so in `vm = g; g = vm + 1;` the diagnostic may name the carrier `vm` rather than `g`.

8. ~~BUG-1625 sees a CHECKED body only~~ — CLOSED (BUG-1649): the post-pass over
   `Checker.sync_spawns` re-reads Handle auto-derefs in every body the checker finished after
   the spawn site. **Residual (precision, loud):** that late check does not apply the `@once`
   single-owner exemption — declare the spawn target above its spawn site.

- ~~A comptime shift by exactly 63~~ — CLOSED (BUG-1630).

## OPEN — residuals of the 2026-10-12 audit (BUG-1566..1596; measured)   [closed items only — the open ones stayed in limitations.md]
2. ~~The volatile-flag RMW rule refused a spawner's RMW against a thread that only READS the
   flag~~ — CLOSED 2026-10-13 (BUG-1604: thread STORES are recorded by the sync walk).

## OPEN — residuals of the 2026-10-10 audit (BUG-1511..1537; measured)   [closed items only — the open ones stayed in limitations.md]
3. ~~A helper called only from an interrupt handler counts as MAIN code~~ — CLOSED
   2026-10-15 (BUG-1608). Still conservative: a function is ISR-only only when it is `static`
   or the program has a ZER `main` (a `--lib` build keeps every non-static helper as main
   code, because C may call it), and names are matched across modules as one set (two
   modules' `bump` share their callers).

6. ~~Leak false negatives~~ — CLOSED 2026-10-13: one free at a literal index after a
   variable-index fill (BUG-1603), and an allocation consumed inline (BUG-1597).

8. ~~Type positions that do not parse a container stamp~~ — CLOSED 2026-10-11 (BUG-1556:
   `alloc` / `arena.alloc` / `alloc_slice` / `@size` / `@offset` take a stamp and a primitive).

9. ~~Loud name collisions~~ — CLOSED 2026-10-11 (BUG-1560: optional-slice typedefs are
   `_zer_optslice_*`; BUG-1559: a task frame containing itself is a checker error).

## OPEN — residuals of the 2026-09-28 round-4 hunt (BUG-1490..1510; measured)   [closed items only — the open ones stayed in limitations.md]
4. ~~Every call inside an async body widens every frame-local range (BUG-1503)~~ — CLOSED
   2026-11-13 (BUG-1628): a re-entrant `_poll` traps, so the widening is removed.

## OPEN — residuals of the 2026-09-28 single-evaluation / lock round (BUG-1470..1476)   [closed items only — the open ones stayed in limitations.md]
1. ~~A callee that `@cond_wait`s is not refused when reached from inside `@critical` or an
   interrupt handler~~ — CLOSED 2026-10-10 (BUG-1512, with every blocking wait in an ISR).

2. ~~A lock root whose place is a SLICE is not hoisted by `hoist_place_effects`~~ — CLOSED
   2026-11-13 (BUG-1618): a bit-slice place hoists its object's parts and both bit positions.

## OPEN — residuals of the 2026-09-28 passthrough / view round (BUG-1460..1465; measured)   [closed items only — the open ones stayed in limitations.md]
1. ~~Copying a whole GLOBAL aggregate into a local did not carry its slot entries~~ —
   CLOSED (BUG-1466).

## OPEN — residuals of the 2026-09-28 concurrency round (BUG-1420..1425; measured)   [closed items only — the open ones stayed in limitations.md]
2. ~~**The for-loop STEP takes only the statement's primary lock**~~ — CLOSED 2026-10-10
   (BUG-1513: it was a live race, and every CONDITION had the same defect; BUG-1518: the
   step was not in the deadlock check either).

3. ~~**`ensure_func_props` treats a call cycle as "no additional effects"**~~ — CLOSED
   2026-10-10 (BUG-1511, fixpoint over the cycle).

4. ~~A static-local or global initializer `= &tl`~~ — CLOSED 2026-10-10 (BUG-1514).

## OPEN — residuals of the 2026-09-27 rounds (BUG-1366..1399; measured)   [closed items only — the open ones stayed in limitations.md]
4. ~~A `packed` MMIO overlay with a misaligned multi-byte field~~ — CLOSED (BUG-1399).

## OPEN — residuals of the 2026-09-26 audit (BUG-1326..1357; measured)   [closed items only — the open ones stayed in limitations.md]
2. ~~**ISR/main RMW of the SAME register through separately formed pointers**~~ — CLOSED
   (BUG-1358) for pointers bound once to a CONSTANT `@inttoptr`. Still unkeyed: a
   register reached through a computed address, or two struct types overlaying one
   address with different field names (the key is address + field-name path).

## OPEN — residuals of the 2026-09-26 memory round (BUG-1360..1355; measured)   [closed items only — the open ones stayed in limitations.md]
1. ~~**A store THROUGH A GLOBAL POINTER into the global it points at is untracked**~~ —
   CLOSED 2026-09-27 (BUG-1366), with the LOCAL sibling it had hidden. Original text: (MEDIUM —
   accept-unsafe, found while closing BUG-1362, pre-existing in BOTH spellings).
   `?*T slot; *?*T gpp = &slot;` then `*gpp = a; free(a); rd()` (rd reads `slot`) returns
   the recycled object's value (exit 99), and so does the callee spelling `void reg(*T p) {
   *gpp = p; }`. The direct store has no key (`ir_extract_compound_key` does not follow a
   deref), and the store summary deliberately refuses `*global` (it names memory that is not
   the global's own storage). Fix sketch: resolve `*gpp` to the slot BUG-1242/1289 mint for a
   pointer defined ONCE (`global_name_never_mutated`), at the IR_ASSIGN store and in
   `ir_ps_target_rec`; otherwise treat the store as a global store of unknown key.

## OPEN — residuals of the 2026-09-25f audit (BUG-1308..1325; measured)   [closed items only — the open ones stayed in limitations.md]
1. ~~**`lib/` does not compile as shipped**~~ — CLOSED 2026-10-12: fmt imports str, io
   declares its C functions, the module-prefixed C externs (BUG-1593), the `*opaque` C ABI
   (BUG-1594), the const `*opaque` temp (BUG-1595) and the param-view false leak (BUG-1596)
   are fixed, and `tests/test_lib.sh` (in `make check`) builds and runs a program importing
   every module. Imports still resolve from the entry file's directory only, so `lib/` is
   copied beside the program; reference.md documents the library now that it builds.

2. ~~**Two literal-typing over-rejections.**~~ — CLOSED 2026-09-26 (BUG-1326, BUG-1327).

## CLOSED 2026-09-23f — the variable-index SLOT residuals of BUG-1074 (BUG-1130) and the struct wrapper through an ARRAY ELEMENT (BUG-1131/1132/1133)

Both former OPEN entries are closed; the residuals that remain are the narrowed OPEN entry
just below. Measured against the pre-change build (`6a64d4b1`): every probe below COMPILED
there and is refused now; SHAPES p33 (17 cells, 10 HOLE pre-change) and p34 (11 cells, 7 HOLE
and 1 OVER-REJECT pre-change) in `tools/sink_matrix.sh` pin them.

- **(1) free THROUGH a slot read / (2) conditional free before a slot read.** The missing
  variable was the INDEX: `arr[k]` with `k` a *trackable index local* (an integer local whose
  every write is visible — not address-taken, not static, the only local of that name) is now a
  precise key like `arr[3]`, so every sink sees it. Demoted to the wildcard the moment an
  instruction writes `k`. The canonical free-every-slot loop still compiles.
- **(3) leaks.** A store into THIS function's array is no longer an escape; `arr[k] = alloc(T)`
  and `arr[f(i)] = alloc(T)` are registered. The free-every-slot loop is a DRAIN (exempts what
  the array received at a variable index).
- **Found beside them:** a LITERAL read `arr[0]` never consulted what `arr[k]` stored (k may be
  0); `arr[k] = null` after freeing the slot's allocation was a false "use after free" (a store
  into an element is not a read of it).
- **BUG-1131.** A struct VALUE read out of an element or a field carries its allocations
  (`ir_carry_projection`); a MAY backstop covers a returned value whose fields the analysis could
  not follow (address-taken, read through a deref). **BUG-1132:** a struct LITERAL stored by
  ASSIGNMENT (`hs[0] = { .p = a }`, `h = { .p = a }`) registered NOTHING — a single-function
  use-after-free compiled. **BUG-1133:** the old entry's claim that chained wrappers
  (`H mk2(*T a) { return mk(a); }`) worked was FALSE — measured on the pre-change build, the
  chain dropped the view (the param handed on had no identity).

Mechanism: compiler-internals.md "Array slots — the three precisions".

---

## OPEN — what the variable-index slot and struct-wrapper fixes still cannot see (2026-09-23f, MEDIUM — accept-unsafe, narrowed; LOW — leak-silent / over-rejection)   [closed items only — the open ones stayed in limitations.md]
1. ~~**A slot freed through an index and read back after the index was WRITTEN.**~~ — CLOSED
   2026-09-25 (BUG-1302). The dropped FREED fact now survives on the array's wildcard
   (`slot_freed_idx1` / `slot_freed_unknown`): relative to the counter when every write to it
   since was a NON-WRAPPING increment (a for-step `k += 1` under `k < E` with no other write,
   `ir_lower` `for_step_cannot_wrap`; or a 64-bit counter moving by 1), otherwise "some slot".
   A read of the array through anything else, with no precise entry for that slot, is refused.
   **Residual (over-rejection):** a WHILE-loop consume (`while (k < n) { … free(tbl[k]) …
   k += 1; }` on a u32 counter) is not recognised as non-wrapping — use a `for`, or reset the
   slot (`tbl[k] = null;`), which is what the diagnostic teaches.

2. ~~**An UNTRACKABLE index keeps the BUG-1074 posture.**~~ — CLOSED for pure arithmetic
   index expressions (2026-09-25, BUG-1301): `g[k % 4]` is keyed `[(k%4)]` while no local in it
   is written (`ir_index_is_keyable` / `ir_index_key_text`; the kill checks every name inside
   the brackets). Still unkeyed: an index with a CALL, a global, a field or another index in it,
   an address-taken local, a name two locals share.

5. ~~**A draining callee's UAF side.**~~ — CLOSED 2026-09-25 (BUG-1300):
   `FuncSummary.frees_param_elems` (the callee frees a value read out of an element of param i,
   directly or through a callee with the bit); the caller widens the array's slots to
   MAYBE_FREED. A read-only callee has no bit and is unaffected. Params past 63 are not
   represented.

## OPEN — residuals of the 2026-09-23g audit (measured; reproducers inline)   [closed items only — the open ones stayed in limitations.md]
1. ~~**A callee frees an allocation through a GLOBAL, the caller reads its LOCAL alias**~~ —
   **CLOSED 2026-09-24 (BUG-1181)**: FuncSummary.freed_global + the call-site widening; SHAPE
   p36. The FUNCPTR-call residual is closed by BUG-1294 (an indirect call applies every
   summary's freed globals as MAYBE_FREED). Original text:
   (MEDIUM — accept-unsafe, silent). `g = a; drop_g(); a.v` where `void drop_g() { if (g) |p|
   { free(p); } g = null; }` compiles, and the read returns a recycled object's value. The
   store marks `a` escaped and no FuncSummary says "frees what global g points to" (the callee
   frees a capture of a global it never tracked). Sibling of the BUG-1049 residual "a global
   freed in one function and read in another". Fix sketch: a summary bit per global name
   ("may free the allocation global X holds", set when a free's argument traces to a read of
   X), applied at the call by freeing every caller handle aliased by the `(IR_GLOBAL_ROOT_ID,
   "X")` entry. The Handle spellings trap at run time (generation check), so the raw-pointer
   forms are the live ones.

2. ~~**The atomic-cell rule is blind to WHOLE-AGGREGATE access**~~ — CLOSED 2026-09-24g
   (BUG-1295): a copy, assignment, view or `&` of the whole object (in the function or in a
   callee reached during the window) conflicts with any atomic part of it.

4. ~~**`*u32[4]` (pointer to an array) emits invalid C**~~ — CLOSED 2026-09-25 (BUG-1304):
   refused at the type (`resolve_type` TYNODE_POINTER), with the slice / struct-wrapper remedy.
   Supporting it would need a C declarator (`uint32_t (*p)[4]`) at every emission site that
   names a type — none of the corpus spells it.

## OPEN — concurrency residuals of the ag9 round (2026-09-24e; MEDIUM — accept-unsafe races; LOW — over-rejections)   [closed items only — the open ones stayed in limitations.md]
- ~~**MEDIUM: a `*opaque` field of a shared struct cast back to a ZER pointer**~~ — CLOSED
  2026-09-25 (BUG-1299) for a cast (C-style, `@ptrcast`, `@pun`) of the field itself, of a local
  initialised or assigned from it (sticky `Symbol.opaque_from_shared`), and of an if-unwrap
  capture of it. **Residual (LOW):** the value handed to a ZER FUNCTION (`use(s.handle)` where
  `use` casts its `*opaque` param) or returned from one (`*opaque get() { return s.handle; }`)
  is not followed — a per-param "casts to a ZER pointer" summary would close it.

- ~~**LOW: parameter aliasing into a scoped spawn**~~ (`f(&v, &v)` where f spawns with one param
  and writes through the other) — CLOSED 2026-09-25 (BUG-1303): per-function (lent i, used j)
  param pairs (`Symbol.alias_pairs`), carried through callees that forward their params
  (fixpoint in `check_alias_spawn_calls`) and through a local copy of a param
  (`Symbol.param_alias_pos1`); a call passing one object as both is refused. Residual: the
  pairs are keyed by POSITION up to 64 params, and "same object" is decided by NAME
  (`alias_arg_root`) — two different pointers to one object that the checker cannot name
  alike (`f(p, q)` with `q` computed through a call) are not matched.

## CLOSED 2026-09-25 — defer bodies in a function WITH A LABEL used the AST emitter (BUG-1298)

Every defer body is now lowered to an IR template at its registration, in every function.
A labelled function still does not SPLICE it into the CFG (the goto guard / ARMED flags wrap
the fire in C, and as IR branches their correlation would defeat leak analysis — see
`defers_stay_on_ast`); the emitter emits the template INLINE at each such fire and at every
C-level early exit (`emit_defers_from`), through the same instruction emitter as the rest of
the function. Measured on the pre-fix build: the defer-body condition lock, the
var-decl/for-init/while-cond guards, and `switch` / `do-while` / `@critical` in the body.
zercheck_ir still checks such a body from its AST, and three holes in that path closed with
it: the wrong-pool check was never run on it, every defer's frees were credited to every
return (hiding a leak on a path that never passed the defer), and the eager fire at a `goto`
checked no USES. `emit_defer_stmt` remains only as the fallback for a body with no template.

**Still AST-checked (precision, not emission):** zercheck_ir treats a labelled function's
defer body with the older AST scans rather than as CFG. Making it CFG needs the guard-flag
correlation modelled (the Level-B guard machinery is for IMMUTABLE conditions; the guard
flag is not).

## CLOSED — the BUG-976 depth-cap enumeration is closed (2026-09-14, BUG-1016)

All eight caps the 2026-09-13 audit listed as answering in the ACCEPT direction are now
measured and fixed. Kept as a record of the method (the how-to lives in compiler-internals.md
"Bounded walks — the fail-open class"; the shared bounds `ZER_EXPR_WALK_MAX` /
`ZER_STMT_NEST_MAX` / `ZER_TYPE_NEST_MAX` are defined at the top of checker.c after the
includes):

| function | was | now | measured hole? |
|---|---|---|---|
| `packed_path_aggregate` / `packed_step_aggregate` | `> 64 -> NULL` / `i < 8` (accept) | expr bound, past it REPORT packed | **YES** — `&p.a.a…w[0]` on a struct nested 70 deep gave a `uint32_t*` on an odd byte, ACCEPTED (`packed_field_addr_70_deep_bug1016`) |
| `body_calls_funcptr_field` / `scan_funcptr_field_bindings` | `> 8 -> false` (accept) | WIDEN to 32 (call-graph, matches spawn scan) | **YES** — a racy funcptr-field binding 9 calls deep from the spawner was missed (`funcptr_field_binding_9deep_bug1016`) |
| `for_init_has_loop_jump` | `> 32 -> false` + if-chain missing INDEX/SLICE/FIELD | exhaustive switch, expr bound, past it TRUE | **YES** — `orelse break` 34 deep, and inside an array index, slipped the BUG-854 ambiguity ban (`for_init_loop_jump_34deep_bug1016`, `_index_bug1016`) |
| `compute_max_depth` (stack) | `> 256 -> 0` (under-reports) | bound = frame count (acyclic; cycle is is_recursive) | **YES** — a 300-deep non-recursive chain under-reported stack (19200B) and slipped `--stack-limit 17000` (`stack_chain_300_deep_bug1016`) |
| `global_init_scan` | `> 128 -> NULL` ("nothing offending") | expr bound, past it an OFFENCE | availability — a call 200 terms down a flat chain reached GCC as "initializer element is not constant"; now the ZER line (`global_init_call_deep_bug1016`) |
| `rmw_value_source_global` / `_taints_global` / `rmw_scan_body` | `> 256` / `> 16` / `> 24` | expr / stmt bound | hardening — the source cap (256) was below check_expr's reachable expression depth for a flat chain; no clean live discriminator (every reproducer trips a masking rule), but the cap was demonstrably below the reachable AST depth |
| `resolve_write_target_global` | `> 6 -> NULL` (accept) | type-nesting bound (pointer-init hops) | hardening — `**…u32` chains are bounded by the parser's type nesting; no clean reproducer (volatile rules mask the deep-pointer shapes) |
| `node_forwards_param_to_spawn` / `func_forwards_param_to_spawn` / `ct_expr_bits` / `tynode_keeps_storage_inline` / `unique_resource_name`'s `value_is_existing_resource` | mixed (`> 8 -> false`, `> 32`, `> 16`) | rounded to the conservative direction at the right bound | hardening — polarity/consistency; `tynode_keeps_storage_inline` was backwards by its own comment (INLINE is safe), now rounds to INLINE |

Zero corpus cost (2621 files, both binaries, zero diagnostic differences). The five
measured-live holes are pinned by the `*_bug1016.zer` negatives, each verified to reject
on the fix and (bar the global-init reason-only one) accept on the pre-fix build.

## CLOSED 2026-09-23 — the matrix grids wrote fixed `/tmp/_zer_*` paths

Every grid now includes `tests/zer_tmp.h`: a per-process `mkdtemp` directory (removed at
exit) and `ZT()`, which rewrites the `/tmp/_zer_` prefix of each path / command literal into
it. The literals keep their old spelling so a grep for a grid's file still finds it.
`test_vrp_fact_matrix` already used its own `mkdtemp`. Measured: all 12 grids green with a
second `make check` running in another worktree at the same time, and no directory left behind.
A NEW grid must include `zer_tmp.h` and wrap its paths — a fixed path is the defect back.

## CLOSED 2026-11-13 (BUG-1610) — an interrupt handler's own read-modify-write was refused even when only main READS (the tick-counter idiom)

**Closed** by `isr_rmw_regular_code_only_reads`: accepted when one handler reaches the cell,
no regular-code body assigns it (whole / compound / bit-slice) and its address is taken
nowhere. The reason given below for not shipping it ("unsound on a multi-core part") does not
apply to a reader: with ONE writer there is no update to lose on any number of cores, and the
`@critical` exemptions already assume a handler does not run concurrently with itself. A main
that WRITES the cell (even a plain `ticks = 0;`) is still refused — put both updates in
`@critical`. Original text:


`volatile u32 ticks; interrupt TIM2 { ticks += 1; } u32 main() { u32 t = ticks; ... }` is
refused ("read-modify-written and is shared"). On a single core the ISR's RMW cannot be split
by main — main never runs during the handler — so the only real hazards are main's own RMW
(caught, and since BUG-1059c exempt inside `@critical`) and a SECOND handler that preempts the
first (caught since BUG-1059d). The relaxation was deliberately NOT shipped: it is unsound on a
multi-core part where an ISR runs on another core concurrently with main, and the compiler has
no target fact saying "single core". The accepted spellings today are `@atomic_add(&ticks, 1)`
or `@critical { ticks += 1; }` in the handler. Fix sketch if taken up: a `--single-core`
(or target-derived) fact that licenses dropping `compound_in_isr` when `!multi_isr`.

## CLOSED 2026-09-23 (BUG-1124) — a pointer RETARGETED after its declaration was followed to its initializer only

Every RMW / ISR-sharing resolver followed `*u32 gp = &g;` and nothing else, so `gp = &h`
elsewhere (a global pointer, in any function), `p = &d; p = &g;` (a local), and a local COPY
`volatile *u32 r = gp;` all hid the real target — 9 hazard cells accepted across the spawn /
ISR / main sites (RMW FORM grid, `tests/test_hw_matrix.c`) plus the ISR "must be volatile" rule
(`tests/zer_fail/isr_retargeted_global_ptr_bug1124.zer`). Now one set query,
`for_each_write_target`. Corpus: 0 verdict changes over 2583 files.

**Floor that remains (same as an unresolvable pointer PARAM):** an aim whose VALUE cannot be
followed — a call result (`gp = pick();`), `&gp` handed to a function that stores through it,
a compound write, an asm output operand — contributes no target. The stack rule's funcptr
resolver answers the same question with `global_name_never_mutated` (refuses to trust any
reassigned funcptr), which is the conservative form; the data-pointer rules could adopt it
(treat such a pointer as "may designate any address-taken global") if a program ever needs it.

## CLOSED 2026-09-23 (BUG-1125) — a global lent to a scoped spawn, reached by the parent's CALLEES

A call made while a global is lent (BUG-1118) asks ONE callee walk — `walk_callee_globals`,
the G3 atomic-cell walk generalized with a visitor — whether it reaches that global, directly
or through any function it calls; a call through a function pointer is refused while a global
is lent (its target is unknown). The walk terminates by a visited set over bodies, not a depth
cap. Gate: SHAPE p32 in `tools/sink_matrix.sh` (6 of its reject cells are holes on the pre-fix
build). Corpus: 0 verdict changes over 2580 files. Residual (precision): the name match is
lexical, so a callee LOCAL that shadows the lent global's name is over-rejected.

## CLOSED 2026-09-23 (BUG-1120) — two modules declaring the same NON-static global / function name

Worse than recorded: on the raw-name collision `add_symbol_impl` returned the OTHER module's
Symbol and `register_decl` overwrote its fields with this module's declaration. Fixed at the
declaration: a colliding imported declaration gets its own Symbol (registered into a private
scope, listed in `Checker.module_own`), inserted into its module scope; the checker's 120 direct
global-scope lookups go through `global_decl_lookup`, and the emitter's non-local name rule is
ONE rule (the current module's mangled key exists -> this module's; else the raw owner's), which
also fixed module d reading module c's global (`d__x` undeclared). Tests:
`test_modules/twin1120.zer`, `test_modules/xref1120.zer`. Residual: post passes run with
`current_module == NULL`, so a whole-program pass that resolves a colliding name by string gets
the first-registered module's. (BUG-1199, 2026-09-24: the scans now ENTER the callee's module
when they descend into its body — `decl_module_enter` — and the ISR table keys on the resolved
Symbol; the qualified-reference half is the OPEN entry above.)

---

## CLOSED — one integer literal had TWO renderings in the emitted C (2026-09-15 -> closed 2026-09-23b, BUG-1063)

`u32 x = -4 / -2;` rendered `-(uint32_t)4 / -(uint32_t)2` on the 3AC path and a raw C
`(-4 / -2)` at the index position. The literal rendering is now ONE function of the
literal's checker type on both AST emitters (`emit_int_literal`: `U` for an unsigned
17..32-bit literal), the same type the 3AC path gives the IR temp — so `a[-4 / -2]` renders
`-4U / -2U`, the same u32 reading. The index position therefore stopped being
"self-consistent in the signed reading" and would have picked the UNSIGNED reading
silently, so it now refuses a divergent tree like every other sink
(`tests/zer_fail/const_divergent_index_bug1063.zer`). What remains is the over-rejection
half of the old fix sketch — narrowing the divergence rule to "reject only where the two
readings genuinely differ" (`u32 x = (-4 * -2) / 2;` is 4 both ways) — which needs a
width-exact fold, i.e. the entry directly below.

---

## CLOSED 2026-09-23 (BUG-1121..1123) — three defects the reference.md audit found

- **BUG-1121** a label inside `@critical` / `@once` is now a checker error (as a label in
  a `defer` body already was). The `@critical` case built C that GCC rejected; the
  `@once` case was a SILENT miscompile — `goto` from outside to a label in the body ran
  it every time (measured: three calls, `n == 3`). Tests `*_bug1121.zer`.
- **BUG-1122** `zerc` takes options before or after the input; a second positional is
  "more than one input file".
- **BUG-1123** `type_name` renders a slice as `[*]T` and prints `const` (the remedy text
  said `'[]u32'`, and a string literal into a `[*]u8` field read "cannot assign '[]u8'
  to '[]u8'").

## CLOSED 2026-09-23 (BUG-1049) — an allocation stored in a BARE GLOBAL is tracked

Closed in the order the 2026-09-15 attempt prescribed: `g = null;` became an overwrite for a
bare global (`ir_assign_target_is_tracked_slot`), the dangling-global sentence became
carrier-aware (`ir_report_dangling_global`: a non-optional global is told to be declared
`?[*]T` / `?*T`), THEN the bare-ident key landed in `ir_extract_compound_key`. Corpus cost
measured compiler-classified over 2585 files: zero verdict changes. Gate: SHAPE p27 in
`tools/sink_matrix.sh`. Found beside it and closed by the same key: `g = s; free(g); s[0]`
(the free THROUGH the global, the alias from a local). Residual (unchanged): a global freed in
one function and read in another is caught only by the exit/call-window dangling rules, as
for every global before.

---

## ~~OPEN — FIVE BRANCHES SURVEYED 2026-09-10: 102 holes~~ — ALL CLOSED 2026-09-13 (2 as BUG-975, 17 as BUG-976/977/978, 9 as BUG-979/980, 12 as BUG-981/982/983, 3 as BUG-984, 2 as BUG-985, 11 as BUG-987/988/989, 7 as BUG-992, 7 as BUG-994/995, 23 + 3 masked as BUG-996..1014); kept as the map of where each came from

**START HERE.** Measured, not read. Two passes, because one is not enough:

1. **main COMPILES IT CLEAN** — unambiguously a hole. 97 of these.
2. **main rejects it, but for a DIFFERENT reason than the test's own `// expect-error:`** —
   a MASKED hole, still open. 5 more, listed under "masked" below. The other 10 that looked
   masked are only WORDING differences on rules main already has (BUG-967/970/971/972) and
   are genuinely closed — do not chase them.

Nothing here needs re-deriving; pick a class and go.

Fetch any reproducer without checking out:

    git show origin/claude/loving-davinci-<branch>:tests/zer_fail/<name>.zer

### Fork points — this decides everything else

| branch | forked at | date | commits | live holes |
|---|---|---|---|---|
| `v6o9c5` | `da45edd6` (BUG-974) | 09-10 | 1 | 15 |
| `3sdup9` | `efee9006` (BUG-971) | 09-09 | 4 | 23 |
| `qo0mm9` | `f60a803d` | 09-08 | 8 | 28 (22 shared with vgonmt) |
| `ppnatu` | `70c81001` | 09-07 | 1 | 2 |
| `vgonmt` | `aeea0cd5` | 09-06 | 26 | 51 (22 shared with qo0mm9) |

**BUG NUMBERS COLLIDE** with main and with each other — they run to BUG-1003 for different
findings. Renumber on adoption.

**MOST OF WHAT THESE BRANCHES DID IS ALREADY IN MAIN.** `vgonmt` and `qo0mm9` each
independently redid refactor L; `vgonmt` also redid refactor M; three of them redid the
r3an9y class set. Of 165 distinct negatives: 31 already exist in main, 22 are rejected for
the right reason, 10 are rejected with different WORDING for a rule main already has. Do
not re-harvest those 63 — the 102 below are what is left.

### ~~🔴 FIRST: TWO compiler-availability bugs~~ — CLOSED 2026-09-10 as BUG-975

    const u32 A = A;                        // HUNG the compiler (never returned)
    const u32 A = B + 1; const u32 B = A+1; // SIGSEGV

Cause, exactly as the branch recorded it: `eval_const_expr_ex` has always had a
`depth > 256` bound, and it never applied — the IDENT arm called the resolver with no
depth and `resolve_const_ident` restarted the evaluator at **0**, so the bound was RESET
OUT OF EFFECT on every identifier hop. **The guard was present and structurally could not
fire** — the same fail-open shape as class 1 below.

Fixed with TWO mechanisms, because a cycle and a long chain are different diagnostics and
saying "cyclic" about a 70-link chain is a wrong answer, not a rough one:
`ConstIdentResolver` now takes the depth (one call site, `ast.h`) so the existing bound is
real, AND a name stack in `resolve_const_ident` detects an actual cycle. Tests
`global_init_cycle_self` / `_mutual` / `global_init_chain_too_deep` plus the boundary
positive `tests/zer/global_init_const_chain_ok.zer` (ordinary chains must still FOLD — a
const chain feeding an array size needs real compile-time folding).

**Class 1 below is the same shape at scale — start there next.**

### The 97 (all closed), by class — and which branch each was taken from

### ~~1. BOUNDED WALKS FAIL OPEN PAST THEIR CAP — 17 reproducers~~ — CLOSED 2026-09-10/11

All 17 rejected: 15 as **BUG-976** (nineteen guards enumerated, three remedies — FLIP a
boolean, REPORT that the walk stopped, WIDEN when there is no conservative value), and the
last 2 as **BUG-977/978**, which needed a design decision and are the part worth reading:

- **BUG-977** — a predicate feeding two rules of OPPOSITE polarity has no single fail-safe
  default. `ir_contains_move_struct_field_depth` already failed CLOSED for the copy sink,
  and that same `true` EXEMPTS at the leak sink. Fixed by making the walk TRI-STATE
  (`YES`/`NO`/`UNKNOWN`) with each sink choosing its own reading of UNKNOWN. **Struct
  nesting has no syntactic limit, so a cap raise alone could never have closed it** —
  `leak_handle_300_structs_deep.zer` is past the raised cap and is caught by the tri-state.
- **BUG-978** — `value_frame_bound_symbol` returns a `Symbol *` the caller NAMES in the
  diagnostic, so it could not be flipped; it now sets a `gave_up` flag and the caller
  reports the refusal naming the DESTINATION. The reachable window was measured first
  (9..95 — the parser and zercheck's convergence check bound it past that), which is why
  the report is a backstop rather than the fix.

**Two things to carry forward, not re-derive.** (a) *"Fail closed" is not a property of a
predicate — it is a property of the predicate PLUS the rule reading it.* Grep for other
predicates with more than one caller before assuming a flip is safe; a mark and its UNDO
must use the same reading or the undo silently fails to undo (zercheck_ir.c:5303 is such a
pair). (b) A walk over a TYPE never needs a depth cap at all: the type graph is finite and
acyclic, so a visited-set walk terminates. That is the durable end-state for all of these.

Residual: one over-rejection, entered separately above (a plain struct nested past 64
cannot be copied). `3sdup9`'s "depth-guard ledger" OPEN entry is superseded — the
enumeration was done on main and is recorded in BUGS-FIXED.md under BUG-976.

### ~~2. ALLOCATION "BARE SPELLING" FAMILY — 10, UAF / leak / dangling~~ — CLOSED 2026-09-13 as BUG-981/982/983

`3sdup9`'s zercheck_ir.c hunks from `8b1227c9` applied verbatim (three mechanisms: the
bare `slot = alloc(T)` spelling registers a compound; a GLOBAL projection is keyed by ONE
query at every sink, not just the store; a struct VALUE carries its compounds through
`ir_carry_compounds` at all four sites). Pre-fix the UAF RAN and returned a different
live object's value (99). Gate: **SHAPE p25 in `tools/sink_matrix.sh`**. Also closed the
two `global_projection_*` items of class 9 below (same mechanism).

### ~~3. `shared(rw)` RE-ENTRANCY — 6, data race~~ — CLOSED 2026-09-11 as BUG-980

A `pthread_rwlock` is NOT recursive, so the statement-level deadlock rule's premise
("a deadlock needs TWO locks") was false for `shared(rw)`: ONE type is enough, and the
same-type case is exactly what its `id0 != id1` guard excluded. Measured on the pre-fix
build: a held WRITE lock gave **exit 0 with silent corruption** (the second `wrlock`
returns EDEADLK unchecked, so the callee runs unlocked AND its unlock releases the
caller's lock), a held READ lock **HUNG**. The test is an INTERSECTION of the statement's
direct types with what its callees reach — a count can never see it, because the full
collection pass dedupes the callee's copy. Implemented as `lockchk_callee_only`, the
mirror of the existing `lockchk_direct_only`, so both modes run the SAME collector.
Gate: the RE-ENTRY GRID in `tests/test_conc_matrix.c` (callee form x lock kind — the
PLAIN column is what proves the rule is scoped). Residual over-rejection (read/read)
entered separately above.

### ~~4. LOOP COUNTER PAST END — 7, bounds~~ — CLOSED 2026-09-13 as BUG-992

`vgonmt`'s `a40104b8` loop-rule hunks (its BUG-958). A counted loop whose counter WILL take
a value past a fixed array's end is a compile error — the auto-guard's runtime form is a
silent early RETURN, so pre-fix `main` exited 0 with half the loop never run. Licensed by
the loop's WILL-hold fact, not by the MAY-hold range; `loop_body_straight_line` is a
proof-of-danger walk (no default, answers NO for anything untaught). Over-rejection
boundary pinned by 11 shapes in `tests/zer/loop_counter_bounds_ok.zer`.

### ~~5. FORGING DOORS — 11~~ — CLOSED 2026-09-13 as BUG-987/988/989

Bodies byte-identical on `qo0mm9` and `vgonmt` (both cherry-picked `vigilant-tesla-ef9cao`
`021ecaa`); taken from `qo0mm9`'s `afcc7ee1`, checker.c + types.h hunks only (the same
commit's emitter half is two OTHER live bugs — a global `(u32)1e20` and a `1e400` literal
both make the emitted C fail to build — taken as BUG-990/991 in the following commit). Three
rules, all statically decided:
`@pun` refused when its runtime type_id check CANNOT FIRE and the target carries a value
with a validity invariant (`type_carries_forgeable`); `@inttoptr` to an enum-carrying
pointee refused, routing to the guarded `@bitcast` idiom (door set stays at three);
`@container` on a WHOLE object refused (`Symbol.is_whole_object_addr`, the missing third
provenance state), at BOTH sinks. Pre-fix: an integer became a working pointer (wrote 42),
a forged enum dispatched to an arm, `@container(&i)` read BEFORE the object.

### ~~6. ATOMIC CELL x SCOPED SPAWN — 3, race window~~ — CLOSED 2026-09-11 as BUG-979

An exemption whose written rationale was narrower than its code: the comment said
"a SCOPED spawn is joined, so post-join access is safe" (true) and the code set NOTHING,
so the spawn..JOIN WINDOW was exempt too — the same race accepted purely for its
spelling. The flag is now REFCOUNTED: the join closes the window, but only the last live
one, and never when a fire-and-forget spawn also ran. Gate: the CONCURRENT-WINDOW GRID
in `tests/test_conc_matrix.c` (position x spawn-kind x access-kind).

### ~~7. `@ptrtoint(&local)` LAUNDERED THROUGH A CALL — 4~~ — CLOSED 2026-09-13 as BUG-995

ONE query `call_result_is_local_address_int` over the existing return summary, at the
three sinks; `expr_touches_local_derived` made exhaustive. Boundary: `ptrtoint_call_boundary_ok`.

### ~~8. FACTORY REACH through switch / do-while — 3~~ — CLOSED 2026-09-13 as BUG-994

Both factory walks are exhaustive switches now, and — found on adoption — their nesting
cap (8, fail-open, reachable 9..63) is the parser's bound with a report past it; a third
silent cap on the ISR binding walk reports too. REACH grid 114 -> 124 cells.

### ~~9. VIEW OF A LOCAL / GLOBAL PROJECTION / OPTIONAL PARAM / UNION CAPTURE — 7~~ — CLOSED 2026-09-13

`global_projection_*` as BUG-982; `view_of_local_*` + `union_capture_*` as BUG-984;
`opt_param_*` as BUG-985 (`3sdup9`'s `92cc9dfd`, read in full first — a relaxation with a
DOUBLE FREE inside it: `drop(mp); free(mp-unwrapped)` ran on main). Two residuals, each its
own OPEN entry above: the union POINTER capture `|*q|` (false leak) and re-unwrapping after
`p = null;` (false UAF).

**10. SMALLER CLASSES**
- ~~i64 literal range (3)~~ — CLOSED 2026-09-13 as BUG-1002
- ~~`bool` minting via `@ptrcast`/`@inttoptr` (2)~~ — CLOSED 2026-09-13 as BUG-1004
- ~~MMIO const-ident (2)~~ — CLOSED 2026-09-13 as BUG-996 (and the masked `_oob_index`); `ppnatu`'s `mmio_volatile_index_reject` CLOSED as BUG-1011 — and the ARRAY half of the same two-read guard, which no branch pinned, with it
- ~~global init (2)~~ — `global_init_from_mutable` CLOSED 2026-09-13 as BUG-997; `global_init_chain_too_deep` was already closed by BUG-975
- ~~multiview UAF (2)~~ — CLOSED 2026-09-13 as BUG-1008
- ~~struct-init field (2)~~ — CLOSED 2026-09-13 as BUG-1007
- ~~compound float↔int (2)~~ — CLOSED 2026-09-13 as BUG-1005
- ~~RMW via struct-init (2)~~ — CLOSED 2026-09-13 as BUG-999
- ~~`@bitcast` array target miscompile (1)~~ — CLOSED 2026-09-13 as BUG-1000 (and the array-SOURCE miscompile beside it, BUG-1001)
- ~~funcptr-binding alias survival (1)~~ — CLOSED 2026-09-13 as BUG-998
- ~~ISR RMW split across statements (1)~~ — CLOSED 2026-09-13 as BUG-1010 (both sinks; hw-matrix cells at both)
- ~~param-local double free (1)~~ — CLOSED 2026-09-13 as BUG-1006
- ~~spawn borrow, two unknown roots (1)~~ — CLOSED 2026-09-13 as BUG-1014 (refused; the lend-every-root precision fix is an OPEN entry above)
- ~~defer body with a label (1)~~ — CLOSED 2026-09-13 as BUG-1012 (main's AST defer path had no LABEL handler: a `compiler bug:` line and a trap; now refused, since goto is banned in a defer body and the label could never be a target)
- ~~asm operand shared read (1)~~ — CLOSED 2026-09-13 as BUG-1013 (rejected; both operand lists)

### MASKED — main rejects these, but for the WRONG reason, so the hole is still open (3)

Found by re-running the "already rejected" bucket against each test's own `expect-error`.
A rejection is not a closure until the REASON matches.

- ~~`mmio_const_ident_oob_index`~~ — CLOSED 2026-09-13 as BUG-996 (names the index now).
- ~~`opt_param_drop_then_caller_uaf`~~ / ~~`opt_param_other_optional_null_path_maybe`~~ —
  CLOSED 2026-09-13 as BUG-985.

Both `opt_param_*` belong with class 9 (optional-param drop), which already has two live
siblings — likely one fix.

### limitations.md entries worth taking (deduped against main's own)

- `3sdup9`: ~~the depth-guard ledger~~ (SUPERSEDED — the enumeration was done on main, BUG-976);
  ~~"a callee freeing an optional field
  through the IF-CAPTURE form stays MAYBE"~~ (TAKEN 2026-09-13 into the BUG-985 residual entry); ~~"freeing a UNION variant through its POINTER capture
  `|*q|` is a false leak"~~ (TAKEN 2026-09-13, own OPEN entry above) — both OVER-rejections
- `ppnatu`: "Arena methods through a POINTER or a STRUCT FIELD are not supported"; "ISR-vs-main
  tracking counts a helper as 'main' even when only the ISR calls it"
- `qo0mm9` / `vgonmt`: "`@inttoptr` to a pointer-carrying <type>"; the unverified-audit-lead lists
  ("2026-09-02 audit", "`pstdqk` audit leads") — leads, NOT findings; measure before believing
- **STALE, do not take:** the `BRANCH loving-davinci-r3an9y` entries in `3sdup9` / `ppnatu`, and
  `ppnatu`'s "scoped-spawn borrow RESIDUALS after BUG-969". That survey is CLOSED in main
  (BUG-967..974).

### `ppnatu` is superseded

One commit, 2 live holes, both duplicated elsewhere. Harvest `mmio_volatile_index_reject` and the
asm reproducer; skip the rest.

### Suggested order

Segfault (done) → class 1 (done) → classes 3 and 6 (done) → class 2 (done)
→ class 5 (forging doors) → the rest.

---

## CLOSED 2026-09-23 (BUG-1150, BUG-1151) — `opt = { .x = 1 }` into a `?Struct`; `u8[K]` with `const K = @size(T)`

Both were over-rejections; the second hid four wrong-size computations, one of them an OOB read
(BUGS-FIXED BUG-1151). Only a `@size(T)` whose size is the PLATFORM's (a Semaphore / Barrier /
allocator member) is still refused as an array size — honestly, instead of a length-0 array.

## CLOSED 2026-09-23 (BUG-1127) — a designated initializer at GLOBAL scope

The global path now types the literal from its destination (`validate_struct_init`, as the
local path does). Closing it exposed two neighbours, fixed with it: a MUTABLE global named
anywhere inside a global initializer (`u32 x = m + 1;`, `{ .f = m }`, `&arr[i]`) reached GCC
as "initializer element is not constant" — `global_init_scan` now resolves identifiers (an
ARRAY named bare is its address, a constant; indexing it is a read) — and the global
value-flow site lacked the array -> slice coercion (`[*]u8 s = buf;` emitted `s = buf`).
Tests: `tests/zer/global_designated_init_bug1127.zer`, `tests/zer_fail/global_init_mutable_*_bug1127.zer`.

## CLOSED 2026-09-23g — a non-null `*T` FIELD / ELEMENT was NULL wherever its aggregate is zero-initialized (BUG-1152)

Option (c) of the old entry was taken: every LOAD of a non-optional `*T` out of memory (a
field, an array/slice element, `*pp`) is checked, so the value that reaches a local, a param or
an operand is never NULL; it traps with "read of a null non-null pointer" on hosted AND bare
metal. Guarding the load (not the dereference) is what makes it complete — see the emitter's
BUG-1152 comment. Zero corpus cost. The same mechanism guards a no-zero-variant ENUM loaded out
of memory (BUG-1175). What remains is a design note, not a hole: `*T` in an aggregate is
non-null "by trap", not by construction. A definite-initialization analysis (option (b)) would
move the report to compile time; nothing depends on it for soundness.

## CLOSED 2026-09-25 — `&packed.byte_field` was rejected although a u8 cannot be misaligned (BUG-1305)

`addr_of_is_packed_field` now answers "no hazard" when the addressed operand's own type has
alignment 1 (`type_alignment_bytes`), the test the slice half (`packed_array_field_view`) has
made since BUG-972. `&p.b` (u8), `&p.w8[1]` (u8 array element) and `&p.bs` (a struct of bytes)
compile; every u32 / u32-array / aligned-struct form stays refused (the zer_fail packed set, all
re-run). An unknown operand type keeps the refusal. The pinned over-rejection
`tests/zer_fail/packed_u8_array_elem_addr.zer` was removed as its own header instructed;
`tests/zer/packed_byte_addr_ok_bug1305.zer` replaces it.

## ~~OPEN — a `shared struct` read in an ASM OPERAND takes NO LOCK~~ — CLOSED 2026-09-13 as BUG-1013

REJECTED rather than locked, by the Ban Decision Framework's first branch (a hardware
constraint): asm is legal only in a `naked` function, which has no prologue and no frame,
so a `pthread_mutex_lock` call around the operand is not something the compiler can emit
there. Both operand lists (input and output) go through the ONE query
`collect_shared_types_in_expr`; the message names the operand and the struct and gives the
fix (read the field into a local in an ordinary, auto-locked function and pass the value).
Pinned by `tests/zer_fail/asm_operand_shared_read.zer` (input) and
`asm_output_operand_shared.zer` (output). The CALL-laundered operand shape discussed in the
original entry is unchanged (it is the ii7a90 item O over-rejection, not a hole).

---

## ~~OPEN~~ CLOSED — BRANCH `loving-davinci-r3an9y` (2026-09-04): all 9 surveyed items fixed 2026-09-08..10; 3 unverified residuals remain

Surveyed 2026-09-04, measured against main at `225a5b9d`. A DIFFERENT branch family
from the `vigilant-tesla-*` set above, but it forked at the SAME base (`c8e58304`),
so **its BUG-913..924 collide with all eight of theirs** — renumber on adoption.

    git show origin/claude/loving-davinci-r3an9y:tests/zer_fail/<name>.zer

One commit, `df4e51ee`, unusually well documented — read its commit body before
starting, it names the fix mechanism for each. 48 test files extracted; 35 of its
negatives are LIVE on main. Several classes here appear in NO other branch.

### Directly extends work already landed — take these first

**A. ~~Enum auto-zero is a SEVENTH forging door (their BUG-917)~~ — CLOSED
2026-09-04 as BUG-930.** DO NOT REDO. Landed: `enum_no_zero_variant_init_check`, one
query at both declaration sites (local var-decl and global-var), placed beside the
"non-null pointer requires an initializer" rule that asks the same question about a
different type. Tests `tests/zer_fail/enum_no_zero_variant_{uninit,global_uninit}.zer`
plus boundary `tests/zer/enum_zero_variant_boundary_ok.zer`.

RESIDUAL, still open: their CONSUMER-side half, an `enum_nonvariant_trap` emitted at
both emitter paths, for storage that is zeroed where no declaration is visible to the
rule (a memset region, an array element reached through a pointer, cinclude-provided
memory). The declaration rule covers the door the author can see; the trap covers the
rest. Their implementation is on the branch if that residual is taken up.

Kept for context — this is what it was, and why it was upstream of the other six
doors: ZER's auto-zero guarantee gives every variable 0, and an enum whose variants
do not include 0 is therefore born invalid.

    enum E { a = 1, b = 2 }
    u32 main() { E e; switch (e) { .a => { return 1; } .b => { return 2; } } return 3; }
    // MEASURED: runs, exits 2 — took the `.b` arm for a value of 0

Their fix: a consumer-side `enum_nonvariant_trap` at BOTH emitter paths plus a
declaration rule for the bare-uninitialised door. That is TRACKING (trap), which is
right here — unlike a literal, the author wrote nothing to reject.

**B. ~~Packed-struct ARRAY field, four more doors (their BUG-922)~~ — CLOSED
2026-09-09 as BUG-972.** DO NOT REDO.

BUG-786 covered a deref through `&packed.field`. An ARRAY field escaped it at four
further doors, and they split into TWO OPERATIONS that no single predicate could cover:

- **address-of:** `&p.w[0]` — one INDEX step between the field and the `&`, which the
  gate (it demanded a DIRECT field access) walked straight past. `packed_path_aggregate`
  already peeled INDEX; only that gate did not.
- **slice view:** `p.w[0..]`, `[*]u32 s = p.w`, `fill(p.w)` — no `&` appears in the
  source at all, so the address-of predicate could never reach them. New predicate
  `packed_array_field_view` + one reporter at the seven value-flow sinks.

Measured on `packed struct P { u8 a; u32[2] w; }`: `offsetof(w) == 1`, so the emitted
`uint32_t *` addresses an ODD byte — a hard fault on ARMv7-M / RISC-V, a split access on
Cortex-M0+, and merely SLOW on x86, which is why no hosted test could have caught it.

**The precision half is what the packed feature exists for.** A BYTE array field is safe
to view — u8 elements cannot be misaligned — so the slice rule keys on
`type_alignment_bytes`, not on the packed attribute alone
(`tests/zer/packed_u8_array_view_ok.zer` pins it).

Gated by **SHAPE p22 in `tools/sink_matrix.sh`** (operation axis, 6 reject + 2 boundary).
Verified to FIRE: 5 of the 6 report HOLE against the pre-fix build — the sixth is
BUG-786's already-covered door, kept as pinned baseline — and both boundary cells stay
green on both sides. A `nested_packed` cell (a packed struct inside a plain one) was
added; it is not in the branch's set and it fires too.

**KNOWN OVER-REJECTION, recorded rather than hidden:** peeling INDEX also rejects
`&p.w[0]` where `w` is a BYTE array. That is UNIFORM with pre-existing behaviour (`&p.b`
on a plain `u8` field of a packed struct was already rejected), so it adds no new
inconsistency, and the corpus cost is zero — the suite is green across the 18 files using
packed structs.

Pinned as `tests/zer_fail/packed_u8_array_elem_addr.zer` and tracked as its OWN entry —
**"## OPEN — `&packed.byte_field` is rejected although a u8 cannot be misaligned"** above,
which carries the symptom, root cause, fix sketch and tripwire. It is filed there rather
than only here because a closed section is the wrong home for open work: a session
grepping `## OPEN` for what is left would never find it.

NOT filed under `tests/zer_gaps/` either — that directory's contract is "compile-clean IS
the gap", the opposite of an over-rejection, and the harness said so when it was tried. BUG-786 covered a
deref through `&packed.field`. An ARRAY field of a packed struct escapes the
alignment rule at four further doors — slice, coercion, call-arg and `&elem`:

    packed struct P { u8 a; u32[2] w; }
    u32 main() { P p; [*]u32 s = p.w[0..]; s[0] = 1; return p.w[0] - 1; }
    // MEASURED: 0 diagnostics

Their mechanism: `value_flows_to` takes the Checker, plus `packed_array_field_view`.
Note that changes the signature of the query BUG-842 introduced and BUG-927 extended
— reconcile rather than duplicating it.

### Classes that appear in NO other branch

**C. ~~A slice's `.ptr` / `.len` are ASSIGNABLE (their BUG-920)~~ — CLOSED 2026-09-08
as BUG-967.** DO NOT REDO.

The slice header is the whole basis of `[*]T` bounds safety, and it was writable, so the
guarantee was forgeable in one line — this compiled clean, RAN clean, and wrote 46 bytes
past a 4-byte array:

    u32 main() { u8[4] a; [*]u8 s = a; s.len = 100; s[50] = 1; return 0; }

Landed as ONE query, `view_header_field`, at TWO sinks — the NODE_ASSIGN target and
`&` — because those are the only ways to write the header. **Seventeen spellings were
measured live before implementing**, and all seventeen are covered by that one query
without a case each: plain and compound assign; `.ptr` and `.len`; a bare local, a
global, a struct field, a nested struct, an array-of-struct element, a pointer
auto-deref, a value unwrapped from an `orelse`, a sub-slice, and a write inside a defer
body; plus `&s.len` / `&s.ptr` bound to a local or passed as an argument. The array
sibling `a.len = 100` is the same question and was emitting `4U = 100ULL` — invalid C
that only GCC rejected.

**Keyed on the OBJECT'S TYPE, never on the field NAME.** A user struct may perfectly
well have a field called `ptr` or `len`, and this corpus has `JString.len`,
`Packet.len` and `Box.ptr`. A name-keyed rule would reject all of them.

**REJECT rather than track, because the corpus cost was measured at ONE file** — and
that file (`range_for_len_snapshot`) was writing the header only as a convenient way to
BUILD a slice, not because its subject needed it. Rewritten to build by slicing and to
widen the collection mid-loop, which is the legal route and tests the same property.
Nothing else in tests/, rust_tests/, zig_tests/, lib/, examples/ or test_modules/ writes
a slice header at all.

Tests: the branch's three verbatim (`slice_len_assign_forge`, `slice_ptr_assign_forge`,
`slice_len_addr_forge`) plus four more doors it did not have
(`slice_len_compound_forge`, `slice_header_via_struct_field`, `slice_ptr_addr_forge`,
`array_len_assign`) and the boundary positive `tests/zer/slice_header_read_ok.zer`,
which pins that reads still work AND that a user struct with `len` / `ptr` fields still
compiles.

**D. ~~Unique-resource COPY-BY-VALUE (their BUG-916)~~ — CLOSED 2026-09-09 as
BUG-970.** DO NOT REDO.

`Arena`, `Barrier` and `Semaphore` alias their state when copied and were refused
NOWHERE. `Pool` / `Ring` / `Slab` were refused at exactly ONE site (assignment,
BUG-225), which is why every other value-flow spelling stayed open for all six.

**Not a would-be bug.** `Arena b = a;` then one `alloc(T)` from each compiled clean and
the two allocations returned the SAME BYTES — the second write overwrote the first
(measured: exit 2).

Landed as ONE predicate plus ONE reporter at the value-flow sinks:
`unique_resource_name` (recursing struct/union FIELDS and array elements, so a carrier
cannot bypass it) and `reject_unique_resource_copy`, called from var-decl init,
global-var init, call argument, orelse fallback, designated-init field, spawn argument
and return.

**The boundary is the hard half, and it is what the original single-site rule was
avoiding.** A FRESH value is not a copy — `Arena.over(buf)` constructs state nobody else
owns — so the check asks about the VALUE (`value_is_existing_resource`: an ident, field,
index, or either arm of an `orelse`), not just the target type. And the RETURN sink is
deliberately NARROWER than the other six: returning a LOCAL by value is a MOVE (the
local dies with the frame, so the caller becomes the only owner — the idiomatic
factory), while returning a GLOBAL creates a second owner. Same question, different
answer, decided by the source's lifetime.

**The remedy differs by type, and the diagnostic says which — measured, not assumed.**
`*Barrier` / `*Semaphore` parameters work; `*Arena` / `*Pool` / `*Slab` / `*Ring` do NOT
(`a.alloc(T)` through a pointer is "cannot access field 'alloc'"). Those four are
addressed BY NAME and are conventionally global. The first draft of the message
recommended `*Arena` for everything, which would have sent readers into a second error;
`tests/zer/resource_pointer_param_ok.zer` exists so it cannot silently become wrong
again.

Gated by **SHAPE p21 in `tools/sink_matrix.sh`** (type x value-flow sink, 10 reject + 3
boundary). Verified to FIRE: all 10 report HOLE against a build of the commit before the
fix, and all 3 boundary cells stay green on both sides. Note `p21_barrier_copy` had to
INITIALISE the copy to discriminate — without that the pre-fix compiler rejected it via
the unrelated "barrier never initialised" rule, so the cell would have passed on a
broken compiler and tested nothing. `Arena`, `Barrier`
and `Semaphore` alias their state when copied:

    u8[64] buf; Arena a = Arena.over(buf);
    Arena b = a;                    // two Arenas over one backing store
    *T x = a.alloc(T) orelse return;
    *T y = b.alloc(T) orelse return;   // may return the SAME slot

Doors: plain copy, by-value param, return, orelse fallback, a struct CARRYING one,
and a struct-init field. Their fix is one predicate `type_unique_resource_name` +
`check_unique_resource_copy` at every value-flow site — the one-query shape.

**E. ~~STATIC LOCALS are invisible to the spawn race scan AND the ISR check
(BUG-915)~~ — CLOSED 2026-09-09 as BUG-971.** DO NOT REDO.

A `static u32 c = 0;` inside a function is ONE object for every thread that runs it and
for main-vs-ISR alike — the emitted C says so verbatim (`static uint32_t c = 0;` inside
the function). But it has no global-scope Symbol, and BOTH scans resolved names with
`scope_lookup(c->global_scope, …)`, so NEITHER saw it.

**The two sinks' NODE_VAR_DECL arms were byte-identical** (`scan_unsafe_global_access`
at one, `record_isr_globals` at the other) — the mirrored-sink family, confirmed by
diffing rather than assumed. The static local is recorded in that same arm at both,
beside the `*u32 p = &counter` alias it already records, and read at each IDENT arm. A
scan-scoped table rather than a threaded body root because the scan has SEVEN entry
points; threading through all of them is the multi-site risk the fix exists to remove.

The ISR half also needed `check_interrupt_safety` to stop `continue`-ing on a name with
no global Symbol, and its own diagnostic: there is no "declare it volatile" remedy worth
pointing at for a static local.

**A diagnostic-accuracy fix came with it.** EIGHT spawn messages hardcoded the noun
"non-shared global"; saying that about a static local sends the reader hunting for a
global that does not exist. One query, `scan_finding_noun()`, so the eight cannot drift.

**And a real trap, caught by five negatives failing "for the WRONG REASON":** one of
those sites selected between two format strings with a `?:`. A ternary shares ONE
argument list, so adding `%s` to only one branch made the other read a `char *` as an
`int` precision — and the compiler could not warn, because the format is not a literal.
It is now two calls with two arg lists.

Gated by the **STATIC-LOCAL GRID in `tests/test_hw_matrix.c`** — SITE x SHAPE, the same
cross-product as the volatile-width grid above it and for the same reason: the two sinks
are independent code with the identical defect, so DISAGREEMENT is a failure, not just a
miss. Verified to FIRE: run from the baseline tree, both `plain-static` cells report
FALSE-NEGATIVE and the two exemption cells (`static const`, single-word `static
volatile`) stay green on both sides.

**Note the harness trap:** `test_hw_matrix` AUTO-DETECTS `./zerc` from the cwd and
ignores `argv[1]`, so `/tmp/thw /path/to/old/zerc` silently measures the NEW compiler and
the grid appears to pass on a broken build. Run it FROM the baseline tree instead.

    void w() { static u32 c = 0; c += 1; }
    u32 main() { spawn w(); spawn w(); return 0; }

A static local has global lifetime but is not in the global scope, so neither sink
sees it. 3 tests, BOTH sinks — the ISR sibling is:

    void helper() { static u32 c = 0; c += 1; }
    interrupt TIM1 { helper(); }
    u32 main() { helper(); return 0; }

Their fix: a scan-scoped static table + `IsrGlobal` keyed by declaration. Fix both
sinks in the same commit — this is the mirrored-sink family.

**F. ~~Scoped-spawn borrow only ever covered a literal `&v` (their BUG-924)~~ — CLOSED
2026-09-09 as BUG-969.** DO NOT REDO.

The borrow was established only for a LITERAL `&v` argument, so every other way of
handing the same address to a thread lent NOTHING and the parent could write memory the
child held — all measured compiling clean: a pointer local bound to `&v`, a struct
CARRYING the pointer, a slice VIEW of a local array, a pointer PARAMETER, and a
threadlocal reached through a pointer local (which the D4 rule refused only in its
direct spelling).

**Why the existing flag could not answer it.** `is_local_derived` is a BOOLEAN — "this
points into SOME local" — which is all the ESCAPE sinks need ("does it outlive the
frame?"). The race guarded here is a parent write to the ROOT (`v = 3`), so the root
must be NAMED, and a boolean cannot. Hence `Symbol.borrow_root_name`, recorded at the
DECLARATION per this codebase's own rule that scope-sensitive facts belong at
declaration sites, never at use sites (BUG-488/494).

**Two names can be lent per argument and both matter:** the ROOT it points into, so
`v = 3` is caught; and the ARGUMENT itself, so a write THROUGH it (`*p = 3`) is caught
— which is the only thing available for a pointer PARAMETER, whose root lives in the
caller. Both go into `th_borrow_names`, so the join releases every one.

Gated by **SHAPE p20 in `tools/sink_matrix.sh`** (spelling axis, 8 reject + 3 boundary).
Verified to FIRE: against a build of the commit before the fix, 6 of the 8 report HOLE —
the other two (`literal_amp`, `interior_ptr`) are the spellings that already worked and
are kept as pinned baseline — and all 3 boundary cells stay green on both sides. The
boundary matters as much as the holes: a SCALAR argument is copied and lends nothing, a
pointer to a GLOBAL lends no local, and an unrelated local stays writable.
A pointer local, a struct carrier, a slice view, a param and a threadlocal alias all
lend nothing, so the parent can race the child:

    void w(*u32 p) { *p = 5; }
    u32 main() { u32 v = 0; *u32 q = &v; ThreadHandle th = spawn w(q);
                 v = 3; th.join(); return v; }

Their fix: `Symbol.borrow_root` recorded at the declaration sites, one sink
`scoped_spawn_borrow` for both spellings, unknown root REJECTED.

**G. ~~Views into a pool/slab slot escape UAF tracking (their BUG-919)~~ — CLOSED
2026-09-09 as BUG-968, and it was WIDER than recorded here.** DO NOT REDO.

Two independent questions were tangled in one walk ("what allocation does this view
point into?", peeled FIELD/INDEX to an ident and stopped at anything else):

1. **The `get()` SPELLING.** `pool.get(h)` and `h` name the same slot, but the get()
   form landed on a `NODE_CALL`, so `&pool.get(h).v` aliased nothing while `&h.v`
   aliased correctly. Two spellings of one operation — the recurring shape.
2. **A SLICE never aliased AT ALL — and this half is NOT in the survey below.** The
   alias branch admitted only `&`, so a slice view of a field aliased nothing in
   EITHER spelling, and **on heap `alloc(T)` pointers too**: `*T t = alloc(T); [*]u8 s
   = t.arr[0..]; free(t); s[0] = 7;` compiled clean. Found by asking whether the gap
   was handle-specific. It is not. CLAUDE.md already stated the rule this violated —
   *"FORMING a reference aliases; READING a value does not"* — and a slice IS a
   reference-forming node.

**What it cost, measured rather than argued.** This compiled clean and RETURNED 99:

    Handle(T) h1 = p.alloc() orelse return;  [*]u8 s = h1.arr[0..];  p.free(h1);
    Handle(T) h2 = p.alloc() orelse return;  h2.arr[0] = 22;
    s[0] = 99;  return h2.arr[0];            // 99 — corrupted a LIVE object

The stale view wrote into a freed slot that had been handed back out. That is exactly
what the Handle generation counter exists to prevent, bypassed because a view carries
a raw pointer nothing re-checks. **ASan does not see it** — ZER's `alloc(T)` is an
auto-Slab, so `free()` recycles the slot rather than returning it to libc, which is
precisely why the compile-time tracking is the thing that matters.

Landed as ONE query per file — `ir_view_root_ident` (zercheck_ir.c) and
`keep_view_root_ident` (checker.c) — both peeling FIELD/INDEX/SLICE and seeing through
a Pool/Slab `.get(h)` to the handle. The checker twin is DELIBERATELY not wired into
`arg_is_local_derived`, which asks a different question ("does this point into STACK
memory?"): a pool slot lives in the global Pool, so seeing through get() there would
assert something false.

Gated by **SHAPE p19 in `tools/sink_matrix.sh`** (spelling x form, 8 reject cells + 2
boundary). Verified to FIRE: run against a build of the commit before the fix, all 8
report HOLE and both boundary cells stay green. Tests: the branch's five verbatim, plus
`handle_slice_view_uaf`, `heap_slice_view_uaf` and `pool_slot_reuse_corruption` for the
half it did not have, and the boundary positive `pool_get_view_before_free_ok`.

    Handle(T) h = p.alloc() orelse return;
    *u32 q = &p.get(h).v;
    p.free(h);
    *q = 7;                          // UAF, accepted

Forms: `pool_get_field_addr_uaf`, `_index_addr_uaf`, `_field_slice_uaf`,
`pool_get_field_addr_keep_stash`, `slab_get_field_addr_uaf`, plus the Handle-slice
spelling, which reaches the slot without calling get() at all:

    struct T { u8[4] arr; }
    Pool(T, 4) p;
    u32 main() {
        Handle(T) h = p.alloc() orelse return;
        [*]u8 s = h.arr[0..];      // view into the slot via Handle auto-deref
        p.free(h);
        s[0] = 7;                  // UAF, accepted
        return 0;
    } Their fix: `ir_view_root_handle` + `pool_get_handle_root`
at the keep sink.

**H. ~~`spawn w(a.x + b.y)` reads the second shared struct UNLOCKED (their BUG-914)~~ —
CLOSED 2026-09-10 as BUG-973.** DO NOT REDO.

The same-statement multi-shared-type rule exists because the emitter takes ONE lock per
statement. `NODE_SPAWN` was classified in its collector as "no cond/init/expr that could
read a shared struct" — an EXPLICIT classification, because the exhaustive-switch gate
forced one — and it was simply wrong: a spawn's arguments are parent-evaluated
expressions of that statement. Measured:

    spawn w(a.x + b.y);
    -> pthread_mutex_lock(&a._zer_mtx);
       _sa->a0 = (a.x + b.y);        //  b.y read with only A's mutex held
       pthread_mutex_unlock(&a._zer_mtx);

**Worth stating: the `-Werror=switch` gate did its job.** It made the classification
explicit rather than an omission. What it cannot do is check that an explicit answer is
the RIGHT one — so a no-default switch converts "silently missed" into "visibly wrong",
which is a real improvement and not a guarantee.

**TWO SPELLINGS, TWO WALKERS.** The bare `spawn w(…)` statement reaches the STATEMENT
collector; the scoped `ThreadHandle th = spawn w(…)` arrives as a var-decl INITIALIZER
and reaches the EXPRESSION collector. Fixing one leaves the other open — the scoped form
is not in the branch's test set and is added here as
`tests/zer_fail/spawn_arg_two_shared_scoped.zer`.

**The boundary is PER-ARGUMENT and getting it wrong rejects correct code.** The emitter
locks one shared root per ARGUMENT — verified in the emitted C: `a0 = a.x` under A's
mutex, then `a1 = b.y` under B's, separately — so `spawn w(a.x, b.y)` is two correctly
held locks. A first draft accumulated across arguments and rejected it; the hazard is
only two shared types inside ONE argument.

Gated by **SHAPE p23 in `tools/sink_matrix.sh`** (spelling axis, 3 reject + 2 boundary).
Verified to FIRE: all 3 report HOLE against the pre-fix build, both boundary cells green
on both sides.
Adjacent to BUG-795 (the callee-position walk) but a different sink: per-ARGUMENT
shared-type collection at NODE_SPAWN. 1 test, `spawn_arg_two_shared_types`.

    shared struct A { u32 x; }
    shared struct B { u32 y; }
    A a; B b;
    void w(u32 v) { a.x = v; }
    u32 main() { spawn w(a.x + b.y); return 0; }

**I. ~~Bare `orelse return` in a non-null-pointer or funcptr function (their
BUG-918)~~ — CLOSED 2026-09-10 as BUG-974. THE LAST ITEM IN THIS SURVEY.** DO NOT REDO.

`orelse return` is bare by design — "no value; the return value comes from the function's
return type". The emitter's fallback for a valueless return was `return 0;` for EVERY
non-optional type, which is THREE different answers collapsed into one, and two were
wrong:

| return type | `return 0;` | correct |
|---|---|---|
| integer / bool / float | 0 is the zero | already right |
| slice / struct / union | not a value of that type — GCC refused it, so VALID ZER did not compile | `(T){0}` |
| `*T` / funcptr | a NULL of a type that promises non-null | REJECT: there is no zero |

**Both directions of the same defect.** The pointer half is accept-unsafe (the caller
dereferences NULL — a fault when hosted, a read of address 0 on bare metal); the
aggregate half is an over-rejection so complete the program would not build, and the
diagnostic came from GCC rather than from ZER, which is the worst place for it.

**Why the orelse spelling escaped:** a PLAIN bare `return;` in such a function was
already rejected ("function must return '*T', not void"). The orelse fallback is a FLAG,
so it reaches no NODE_RETURN handler — precisely the reason the `defer` / `@critical`
bans had to be repeated for it, in a comment sitting a few lines above where this check
now goes. One flag, three checks the statement form gets for free.

`?*T` is untouched and stays legal: its zero IS the null sentinel, which is the None the
propagation intends.

Gated by **SHAPE p24 in `tools/sink_matrix.sh`** (return-type axis, 3 reject + 4
boundary). Verified to FIRE: against the pre-fix build the two pointer cells report HOLE
AND the slice/struct cells report OVER-REJECT — one grid capturing both directions.

**Note on writing that boundary:** the first `?*T` cell called the function and bound the
result, which zercheck's ownership model reports as a leak — a pre-existing rejection on
BOTH builds, unrelated to this rule. A boundary cell must isolate the rule under test, or
it measures something else.
Returned NULL as a non-null pointer, and produced a GCC error for slice/struct
returns. 2 tests: `orelse_return_nonnull_ptr_fn`, `orelse_return_funcptr_fn`.

    struct T { u32 v; }
    *T pick(?*T o) { *T t = o orelse return; return t; }   // returns NULL as *T

    u32 dbl(u32 x) { return x * 2; }
    *(u32) -> u32 pick2(?u32 o) { u32 v = o orelse return; return dbl; }

### Their remaining three (no live negative here; verify before adopting)

- **BUG-913** an if-unwrap capture overwrote a same-named outer local
- ~~**BUG-921** `f(*p);` as a statement mis-parsed as a funcptr declaration~~ — CLOSED 2026-09-25 (BUG-1306)
- **BUG-923** a bit-slice write on a `uN` value did not compile

### Suggested order for this branch

~~A~~ (DONE, BUG-930) → ~~C~~ (DONE, BUG-967) → ~~G~~ (DONE, BUG-968) → ~~F~~ (DONE,
BUG-969) → ~~D~~ (DONE, BUG-970) → ~~E~~ (DONE, BUG-971) → ~~B~~ (DONE, BUG-972) →
~~H~~ (DONE, BUG-973) → ~~I~~ (DONE, BUG-974).

**ALL NINE ITEMS IN THIS SURVEY ARE NOW CLOSED** (A=BUG-930, C=967, G=968, F=969, D=970,
E=971, B=972, H=973, I=974). What remains from this branch is only its three unverified
residuals listed below (BUG-913 / 921 / 923), which have no live negative here and must
be measured before adoption, plus A's consumer-side `enum_nonvariant_trap` residual.
A's consumer-side residual can be picked up with B, since both are emitter work.

## SUPERSEDED by harvest-2 H14 — `&&` / `||` narrowing (the "PRECISION only" call was WRONG)

**Corrected 2026-08-25.** See the harvest-2 tracker above: with a PROVABLE index the
`&&` spelling is HARD-REJECTED while the nested-if spelling of the same program compiles.
The original entry below probed an UNPROVABLE index, which correctly guards, and drew the
wrong conclusion. Kept for the measurement, not the verdict.

### (original entry)


`if (i < 4 && arr[i] > 0)` — the canonical guarded-access idiom, and the exact shape the
auto-guard warning tells users to write — still carries a runtime bounds guard, because
VRP never applies a short-circuit LHS to its RHS. Verified still live after the harvest:
the program compiles and runs correctly; the cost is one unnecessary branch.

**This is PRECISION, not safety** — hence not shipped with the 45. It becomes REQUIRED
the day the ALWAYS-OOB verdict is promoted at short-circuit position, because the same
idiom would then be REJECTED rather than merely guarded. 87xihb's BUG-800 carries the
implementation (`vrp_narrow_from_cond`: `ident <op> const` plus conjunctions, De Morgan
for the inverted disjunction, refusing volatile operands, a no-op on anything else) and
is deliberately NOT a second copy of the NODE_IF narrowing — that path stays
authoritative and keeps field keys, guard-body detection, known_nonzero and the
then/else JOIN; this covers only the position it structurally cannot reach, inside a
condition EXPRESSION.

---

## CLOSED 2026-08-26 (all 20) — harvest tracker 3: `as71kk` (verified against main 2026-08-25)

**Status: NOT implemented — a catalog.** Forked at `9f8cda08`, one commit behind main,
so it does NOT contain BUG-857/858. 17 commits, BUG-857..882 (numbering collides with
main's again — key by branch+sha).

**PROGRESS 2026-08-25:** the float question is DECIDED (saturate, BUG-883). A1 (the view
class, all four parts), A10, A11, A12 are landed from `as71kk` — and **my BUG-857
container fix was REPLACED by its BUG-868**, because mine traded the crash for an
over-rejection of a pointer-broken cycle. Remaining: 16.

### The headline: this branch SUPERSEDES most of harvest tracker 2

`as71kk` independently found and fixed **H1..H19** — and its implementations are better
than the catalog entries in three measurable ways, so the recommendation is to take ITS
versions rather than re-derive them from tracker 2:

- it fixes the call-RESULT view class as **all four parts together** (the reason I backed
  my partial out),
- it found ROOT CAUSES where tracker 2 recorded symptoms — the negative-constant rule had
  "nowhere to live" because the three-condition compatibility chain was written out
  **eight times**; the fix is one `value_flows_to` query, not eight patches,
- it records first attempts that were WRONG and why, which is the part a re-derivation
  loses.

### THE FLOAT QUESTION IS DECIDED: SATURATE (2026-08-25, BUG-883)

Owner's call: `float -> int` out of range now **saturates**, NaN -> 0, matching what
Rust settled on for `as` casts. In ZER's own terms the line is MEMORY vs ARITHMETIC —
memory violations halt (slice OOB, misaligned `@inttoptr`, a bad `@pun`); arithmetic
results get DEFINED values, which is already why integer overflow WRAPS rather than
trapping. `@saturate` already named these semantics, so the plain cast now agrees with
the primitive instead of contradicting it.

Consequences, both measured:
- the compile-time LITERAL rejection became a WARNING. Rejecting `(u32)(-1.5)` while the
  same value through a variable produced a defined result was the "two spellings of one
  program disagree" shape this codebase treats as a defect in itself.
- **`@truncate` on a float STAYS REJECTED — a deliberate divergence from `29fiao` and
  `qa249l`**, which make it saturate. `@truncate` means "keep the low bits"; a float has
  no low bits. And it is redundant: `(u32)x` saturates and `@saturate(u32, x)` is named
  for it — verified both give 4294967295 for 1e20 and 0 for -1.5. Making `@truncate` a
  third spelling would give one primitive two unrelated meanings by operand type.
  Those two branches' `float_to_int_saturates` / `float_to_int_total` are therefore NOT
  taken verbatim; `tests/zer/float_to_int_saturates.zer` covers the same semantics.

### as71kk DOES NOT DECIDE IT EITHER (recorded before the call above was made)

`as71kk` leaves `float -> int` trap-vs-saturate open and says so explicitly: *"the 8
remaining rejected positives are listed with what each actually needs, including the
float trap-vs-saturate decision that still needs an owner."* Main's TRAP therefore stands
until an owner decides. Two independent audits reaching "this needs an owner" is itself
the signal that it is a semantics choice, not a defect.

### A DEFECT IN WHAT I ALREADY SHIPPED

**My BUG-857 (container cycle) traded the crash for an OVER-REJECTION**, which is exactly
the first-draft mistake `as71kk` records against its own BUG-868:

```
container A2(T) { ?*B2(T) x; }
container B2(T) { A2(T) y; }      // 8 bytes, MUST compile — main REJECTS it
```

A cycle is infinite only if **every edge around it is by-value**. My stack records the
stamps; theirs records the stamps AND THE EDGE KIND, answered by a no-`default:` switch
over `TypeNodeKind` so a new kind fails the build. Their version is a strict improvement
and should replace mine. (Pre-fix the same program SEGFAULTED, so main is still ahead of
`9f8cda08` — but it is over-rejecting valid code today.)

My BUG-858 (auto-zero on slot reuse) equals their BUG-861; no action.

### LIVE — accept-unsafe

| # | Fix | as71kk id | Evidence on main |
|---|---|---|---|
| ~~A1~~ **DONE (BUG-884..887)** | **The call-RESULT view class, ALL FOUR parts** | 857/858/859/860 | 7 negatives accepted; ASan heap-UAF. arg FORM (bare ident test), def LOCALITY (search missed the COPY a named binding lowers to), BLOCK TAG (`is_early_exit` is leak-COVERAGE, misread as a statement about the return value — wrong in BOTH directions), ARITY (the answer is a SET; one slot collapsed it to unknown). `returns_param_mask` + PULL at the use site, because 18 direct FREED stores mean a push design must reach all of them. Gate: sink_matrix p18, 78 -> 88 |
| ~~A2~~ **DONE (BUG-889)** | **`@cstr` is an unguarded integer-to-pointer door** | 862 | 3 accepted. Every check was NEGATIVE, so a type matching none was accepted BY DEFAULT. Replaced with an ALLOW-list + arity |
| ~~A3~~ **DONE (BUG-890)** | **The SIGN half of "no implicit narrowing or sign conversion" was never enforced** | 863 | 8 accepted. `u32 a = -1;` -> 4294967295, while `u8 b = -1;` was rejected only INCIDENTALLY (−1 types as u32, and u32->u8 is a narrowing mismatch). Root cause: the three-condition chain written out at EIGHT sinks, so there was nowhere to add a condition. One `value_flows_to` query |
| ~~A4~~ **DONE (BUG-891)** | **Enum forging reached only some doors** | 864 | `@truncate(State,7)` unchecked; `@bitcast(Box,7)` with a struct carrier walked past. The guard now recurses struct fields, optional payloads and array elements, at BOTH emitter paths. **Extended the TRACKING, not a ban** — an earlier draft banned it and broke `bitcast_enum_variant_ok` |
| ~~A5~~ **DONE (BUG-892)** | **`?T` comparison reads the payload without `has_value`** | 865 | 2 accepted, and the worst-behaved of the aggregate family because it yields a WRONG ANSWER, not invalid C: auto-zero makes `.value` 0 when absent, so a NULL `?u32` compares EQUAL to 0 — `if (x == 0)` passes on a value that is not there |
| ~~A6~~ **DONE (BUG-893)** | **Non-null auto-zero check: two hand-written copies that DRIFTED** | 866 | The local copy grew a FUNC_PTR arm, the global one never did — so a global `*(u32,u32)->u32 gop;` was accepted and `gop(1,2)` called through address 0. One `nonnull_zero_hole` at both sites. Two test_emit cases relied on the gap; **one WAS the hazard** |
| ~~A7~~ **DONE (BUG-894)** | **`free(s.buf[0..4])` frees INLINE STACK storage** | 867 | The peel asked "is the ROOT a local array?" — the root is a STRUCT. Fixed by the honest question (did navigation stay inside the root's own storage), i.e. FORMED-view vs STORED-reference |
| ~~A8~~ **DONE (BUG-895)** | **Indirect-call keep worst-case tested the BARE kind** | 875 | 3 accepted: `?*T` param and a by-value struct carrier. ASan stack-use-after-return; the DIRECT call of each was already rejected. Uses the shared `type_carries_data_pointer`. Top-level slice/opaque exemption kept deliberately and measured |
| ~~A9~~ **DONE (BUG-896)** | **Six intrinsics STORE through a `const` buffer** | 877 | NEW — not in tracker 2. 12 pointer-taking CPU/cache intrinsics validated "pointer or array?" and nothing about qualifiers; 6 of them STORE (`@nt_store`, `@cpu_fxsave`, `@cpu_xsave`, `@cpu_save_context`, `@cpu_save_fpu`, `@cache_zero_line`). On bare metal that is a write into FLASH that neither faults nor takes effect |

### LIVE — invalid C / silent no-ops

| # | Fix | as71kk id | Evidence |
|---|---|---|---|
| ~~A10~~ **DONE (BUG-869)** | **Global-init guards were TOP-LEVEL kind tests** | 869 | 3 accepted: `@cpu_model_id() + 1`, `(u32)f()`, `-f()`, `@popcount(f())`. **The exact sibling of my BUG-842** — that split the `arg_count` precondition and left the top-level-kind SHAPE. Now asked at EVERY node by a no-`default:` walk |
| ~~A11~~ **DONE (BUG-870)** | **`@offset` validated nothing outside one guard** | 870 | 4 accepted; `offsetof(uint32_t, 1)` reached GCC. Also missed a `distinct` of a struct — the distinct-unwrap class this repo has a CI gate for |
| ~~A12~~ **DONE (BUG-871)** | **The fence family accepted and silently dropped arguments** | 871 | `@barrier(x)` looked like it did something with x. The 0-arg family immediately below had the check all along |
| ~~A13~~ **DONE (BUG-903)** | **`@trap(1,2,3)` silently drops its arguments** | 882 | NEW. Found by a companion sweep calling every intrinsic with an absurd arity — the only one left across ~155 |
| ~~A14~~ **DONE (BUG-902)** | **`lower_expr` could fall off the end of an int-returning function** | 876 | Callers use the value as a LOCAL ID and index `func->locals[]` with it. Latent, but the return was INDETERMINATE |

### LIVE — over-rejections

| # | Fix | as71kk id | Evidence |
|---|---|---|---|
| ~~A15~~ **DONE (BUG-897)** | **`if (i < 4 && a[i] > 0)` HARD-REJECTED** | 874 | = tracker-2 H14, the row I mis-closed as "precision only". Suppressed in short-circuit RHS position at all THREE sites that raise it (ident, call-return range, MMIO). A negative pins the boundary: `if (a[i] > 0 && i < 4)` puts the access on the LEFT, unconditional, and must still hard-reject |
| ~~A16~~ **DONE (BUG-898)** | **`const [*]u8 BANNER = "boot ok";` not declarable at all** | 872 | And it said so with the SAME type on both sides. The const/volatile fold written longhand at TWO sites |
| ~~A17~~ **DONE (BUG-899)** | **`const u32 CAP = 256; u8[CAP] buf;`** | 873 | Rejected as "not a compile-time constant", naming a constant |
| ~~A18~~ **DONE (BUG-900)** | **Arrays of function pointers never worked, in EITHER syntax** | 878/879 | Both `reference.md` and `CLAUDE.md` document `*(u32,u32) -> u32 [4] ops`. Parser: `parse_type` swallowed the `[3]` next to the RETURN type. Emitter: `?FuncPtr[N]` emitted an abstract declarator with a name glued on — not C. **The nullable form is the one that matters**, since a non-null funcptr array is auto-zeroed and ZER has no array initializer |
| ~~A19~~ **DONE (BUG-901)** | **Bit extraction through a pointer** | 881 | `volatile *u32 reg = @inttoptr(...); u32 bits = reg[9..8];` appears in BOTH docs and did not compile — it fell to the SUB-SLICE path where hi > lo is a bounds error. Fixed as a DESUGARING to `*p` so one implementation of bit extraction stays and cannot drift |
| ~~A20~~ **DONE (BUG-884..887)** | **The two view-class boundary positives** | rides with A1 | `view_optional_null_arm_ok`, `view_param_interior_ptr_ok` — over-reject on main today (tracker-2 H19) |

### Tooling — take all of it

| Item | Why |
|---|---|
| **`tools/ubsan_sweep.sh`** (NEW) | Compiles every program's emitted C under UBSan+ASan. Measures the "no undefined behavior" claim neither harness can check — a positive asserts exit 0, a negative asserts a diagnostic, and **UB produces neither**. 1097 programs, clean. **The calibration is the artefact:** `float-cast-overflow` is NOT in GCC's `-fsanitize=undefined` and must be named — found by breaking the BUG-845 guard and watching the sweep report CLEAN on the exact class it exists to find |
| `tools/qualifier_closure_probe.sh` (NEW) | Found A9. **Its first ORACLE was vacuous** — `-Wdiscarded-qualifiers` is silenced by the explicit cast ZER's emitter writes, so it printed OK against a deliberately broken compiler. `-Wcast-qual` is the warning that asks the question. 31 strips on the first honest run |
| `tools/grammar_closure_probe.sh` | Found A2. The closure claim had never been measured and failed on first measurement |
| `tools/ub_sweep.sh` | Differential -O0/-O2. Complementary to ubsan_sweep, not superseded by it — one finds UB the optimiser changes, the other asks the sanitizer directly |
| `audit_walker_fields.sh` membership fix | It decided membership on PROSE — counted the function's own name in comments, so editing a comment moved a walker in or out. Baseline 758 -> 684. **This is the gate I installed in §B** |
| Structural `.gitignore` | 565 tracked binaries. "Extension-less under a test tree" IS a build artifact by construction; verified all 565 were ELF |
| `rc_cond_004` flake | Failed 1 run in 8. **The race is IN THE TEST** — ZER's auto-locking is per-statement, so `q.items[q.tail] = val; q.tail = ...` is two lock scopes. 7-of-8 -> 25 consecutive |

### Recommended order

1. **A1** (view class, all four) — the biggest accept-unsafe family, and the reason my partial was reverted.
2. **Replace my BUG-857 with their BUG-868** — main over-rejects valid code today.
3. **A3** (`value_flows_to`) — one query retires eight duplicated sinks and unblocks the negative-constant family.
4. **A8, A9, A2, A7, A4, A5, A6** — independent accept-unsafe.
5. **A10** — my BUG-842's second axis.
6. Over-rejections **A15..A19**, then tooling.

Float stays parked until decided.

---

## CLOSED 2026-08-26 (all 21) — harvest tracker 2: `r1piyr` / `29fiao` / `qa249l`, verified against main 2026-08-25

**Status: NOT implemented — a catalog.** Every row verified on a clean build of main
(`1c4c64d2`, i.e. AFTER the 45-row harvest) by running the branch's own test and reading
the diagnostic. Rows main already closed are in the "already on main" table.

**PROGRESS 2026-08-25:** H10 (the container-cycle SEGFAULT) and H2 (recycled pool/slab
slots returning stale data) are landed as BUG-857/858. **H1 was attempted and BACKED
OUT** — the shared argument resolver closes 2 of its 7 negatives and regresses nothing,
but `29fiao` fixes this class as FOUR interlocking parts and the fourth turns
`returns_param_color` from an int into a MASK. BUG-848 means the current int can name the
WRONG param, so widening the consumer while the inference is still wrong makes the wrong
answer apply MORE often. Land all four together or none. Remaining: 19.

### Source branches — and why the fork point matters here

| Branch | Forked at | Commits | Overlap with the 45 |
|---|---|---|---|
| `r1piyr` | `1c4c64d2` = **current main** | 1 | **none** — audited the fully-harvested tree |
| `29fiao` | `8b289ede` (before §E/F/G) | 3 | partial |
| `qa249l` | `430bda19` (before §D) | 15 | heavy |

`r1piyr` is the highest-value branch precisely because it had nothing to harvest: its
eight findings are what survived the previous 45 fixes.

**BUG numbers collide AGAIN** — all three renumber 839..865 for different bugs. Key by
branch+sha.

### A CORRECTION TO THE PREVIOUS HARVEST, which I got wrong

`r1piyr` BUG-863 is the row I closed as **"O3, PRECISION only"**. It is not. I probed
with an UNPROVABLE index (`volatile u32 v = 2; u32 i = v;`), which correctly falls to the
auto-guard, and concluded there was no over-rejection. With a PROVABLE index the same
program is HARD-REJECTED:

```
u32 i = 9;
if (i < 4 && a[i] > 0) { }      // error: index 'i' is always out of bounds
if (i < 4) { if (a[i] > 0) { } } // COMPILES — same program, other spelling
```

Worse, my own tracker predicted it — *"becomes REQUIRED if the always-OOB verdict is ever
promoted at short-circuit position"* — and I never checked whether main had already
promoted it. It had: `index_range_verdict`'s error arm shipped with BUG-796. Writing the
prediction and not testing it is the failure here, not the missing feature.

### Already on main — do NOT re-implement

Arena backing / `.over()` discarded / Barrier init / discarded optional / CLI validation
/ struct+union comparison / RMW launders (intrinsic, call, orelse) / guarded-alias
coverage / static-return summaries / packed sinks / `@saturate` totality / the
`naked` warning / the value-returning-async warning / the emitter give-up paths.
Several report **"rejected, WRONG RULE"** — the rule fires, the wording differs from the
branch's `expect-error`. Cosmetic; adopt their phrasing only if a test is taken.

### LIVE — accept-unsafe

| # | Fix | Best version | Evidence on main |
|---|---|---|---|
| ~~H1~~ **DONE (BUG-884..887)** | **The call-RESULT view class** — "which allocation does a call result view?" answered wrong FOUR independent ways | **29fiao** BUG-845/846/848/849 | 7 negatives accepted; `view_arg_field_uaf` is **ASan heap-use-after-free**, reproduced here. A SLICE result makes every one silent: a slice is not "pointer-ish", so the lost answer registers NO allocation and there is no leak diagnostic either. Fixed with one shared query + a `returns_param_mask`; the use site PULLS state (18 direct FREED stores mean a push design must reach all of them). Gate: sink_matrix p18, 78 -> 88 cells |
| ~~H2~~ **DONE (BUG-858)** | **Recycled pool/slab slot is not zeroed** | **r1piyr** BUG-861 | Verified: `alloc_recycled_slot_zeroed` exits 1 — the slot comes back holding the previous object bit-for-bit, against the documented "everything auto-zeroed" guarantee that `Arena.alloc` honours. A `?*T` field returns non-null and dangling, so safe ZER can unwrap and deref it. **Invisible to ASan** — the reuse is inside ZER-owned storage |
| ~~H3~~ **DONE (BUG-895)** | **Indirect call worst-cases only a bare `*T` as keep** | **r1piyr** BUG-857 | 3 negatives accepted: `?*T` param and a by-value struct carrying a pointer let a stack pointer reach a retaining callback. The DIRECT call of each was already rejected — two spellings of one program disagreeing. The `[*]T` exemption is kept and should be recorded as a known accept-unsafe rather than left implied by a comment |
| ~~H4~~ **DONE (BUG-889)** | **`@cstr` is a second, unguarded integer-to-pointer door** | **29fiao** BUG-861 | 3 negatives accepted. `u32 gi = 4096; @cstr(gi, sl);` emits `memcpy(gi, ...)` and RUNS. That is the conversion the language says cannot be spelled. Every `@cstr` check was a NEGATIVE one, so a type matching none of them was accepted by default. **Breaches the grammar-closure claim** |
| ~~H5~~ **DONE (BUG-890)** | **Negative constant into an unsigned type** — 8 spellings | **qa249l** | `u32 a = -1;` silently becomes 4294967295. 8 negatives accepted (var-decl, assign, global, arg, return, orelse, spawn, struct-init) |
| ~~H6~~ **DONE (BUG-891)** | **Enum forging through `@truncate` and a struct carrier** | **qa249l** | 3 accepted. My BUG-843 closed `@bitcast` only; these are the sibling routes |
| ~~H7~~ **DONE (BUG-894)** | **`free` of an inline-array field slice** | **29fiao** | `free(s.buf[0..])` on an inline array accepted; the same without the index was already rejected |
| ~~H8~~ **DONE (BUG-893)** | **Non-null funcptr global** | **qa249l** | `funcptr_global_nonnull` accepted |
| ~~H9~~ **DONE (BUG-892)** | **Optional comparison** | **qa249l** | 2 accepted. My BUG-841 covered struct/union; `?T == ?T` and `?T == T` were left |

### LIVE — compiler crash

| # | Fix | Best version | Evidence |
|---|---|---|---|
| ~~H10~~ **DONE (BUG-857)** | **Mutually-recursive containers SEGFAULT the compiler** | **r1piyr** BUG-864 | `container A(T){B(T) x;} container B(T){A(T) y;}`. The self-containment guard compares against the one stamp its own frame owns, which cannot see a cycle closing on an OUTER stamp. Fix is a stack of in-progress stamps; pointer cycles and the canonical linked list must still compile |

### LIVE — invalid C reaches GCC (silent checker, loud backend)

| # | Fix | Best version | Evidence |
|---|---|---|---|
| ~~H11~~ **DONE (BUG-869)** | **Global-init constant guard is a TOP-LEVEL kind test** | **r1piyr** BUG-859 | 3 accepted: one wrapper (`+ 1`, a cast, a unary minus, an intrinsic argument) defeats all three checks and the failure lands on GCC. Needs one exhaustive walk. **This is the sibling of my BUG-842** — I split the arg_count precondition but left the top-level-kind shape |
| ~~H12~~ **DONE (BUG-870)** | **`@offset` validated nothing** | **29fiao** BUG-860 | 4 accepted. The field check sat inside a guard and did nothing when the guard failed. One case is the distinct-unwrap class, so `@offset` also now WORKS through a distinct typedef. A union is rejected with a reason (ZER unions are tagged, so a variant has no fixed offset) |
| ~~H13~~ **DONE (BUG-871/889/870/903)** | **Intrinsic arity unchecked** | **29fiao** | 3 accepted (`@barrier_*`, `@cstr`, `@offset` with wrong arity) |

### LIVE — over-rejections (valid code refused)

| # | Fix | Best version | Evidence |
|---|---|---|---|
| ~~H14~~ **DONE (BUG-897)** | **`&&` / `\|\|` RHS hard-rejects a provable index** | **r1piyr** BUG-863 | See the correction above. Downgrade the always-OOB verdict to the auto-guard path in short-circuit RHS position — **no runtime check is removed** |
| ~~H15~~ **DONE (BUG-899)** | **`const u32 CAP = 256; u8[CAP] buf;`** rejected as "not a compile-time constant", naming a constant | **r1piyr** BUG-860 | The size paths used the NON-scoped evaluator. The fold must be done IN PLACE: teaching only the checker made the emitter size the array **0** — a relaxation that trades an over-rejection for a wrong answer is worse than the over-rejection |
| ~~H16~~ **DONE (BUG-898)** | **Global `const [*]u8 BANNER = "boot ok";` not declarable at all** | **r1piyr** BUG-858 | And it said so with the SAME type printed on both sides. The const/volatile fold was written longhand in Pass 1 and not repeated in Pass 2 |
| ~~H17~~ **DONE (BUG-900/901)** | Bit-extract through a pointer; funcptr ARRAY syntax forms | **qa249l** | 2 positives rejected |
| ~~H18~~ **DONE (BUG-907 + BUG-908)** | Arena-internal link; guarded coverage through an alias; static-optional return + leak | **qa249l** | all 3 closed. The arena-internal link (BUG-908) landed as its OWN change: a relaxation over ~10 checker sites, verified against all 27 existing `arena*`/`escape_arena*` negatives plus two boundary probes I wrote myself. |
| ~~H19~~ **DONE (rides with BUG-884..887)** | Heap-slice free in an array element; the view-class boundary positives | **29fiao** | ride with H1 |

### LIVE — cross-module (unique to `qa249l`, nobody else looked)

| # | Fix | Evidence |
|---|---|---|
| ~~H20~~ **DONE (BUG-904)** | **`spawn <imported function>` does not LINK — the feature is completely non-functional** | Verified: `spawn_user.zer` emits `void tick();` and calls `tick()` while the definition is `spawn_mod__tick`. The checker ACCEPTS it (the data-race scan even resolves the imported body), so the only signal is `ld returned 1 exit status` with no source line. The name is spelled raw at FOUR emission sites |
| ~~H21~~ **DONE (BUG-905/906)** | **`async` and `container(T)` across a module boundary** | qa249l fixes both; needs re-measuring separately from H22 |

### DESIGN DECISION — not a bug, and it needs an owner call

**float -> integer out of range: SATURATE or TRAP?**

Main currently **TRAPS** (my BUG-845). Both `29fiao` (BUG-853) and `qa249l` (BUG-850)
independently chose **SATURATE**, with NaN -> 0. Both eliminate the UB; they disagree on
what replaces it. Their positives (`float_to_int_saturates`, `float_to_int_total`) fail
on main for exactly this reason — it is a semantics conflict, not a regression.

- **For SATURATE:** `@saturate` already exists in ZER with precisely these semantics, so
  the behaviour is already named in the language; ARM saturates in hardware; and ZER
  already chose "define it, don't trap it" for integer overflow (wrap). No new failure
  mode is introduced into working programs.
- **For TRAP:** every other out-of-range response in ZER traps (slice OOB, `@inttoptr`
  misalignment, `@pun` type mismatch). A saturated value is a WRONG ANSWER the program
  then continues to use; a trap surfaces the bug. On a sensor -> actuator path a clamped
  reading can be worse than a halt.
- **The "it's free" argument does NOT apply either way** — both need the same
  compare-and-branch, so this is purely about which failure mode you want.

Genuine judgment call; recorded rather than decided. Whichever wins, the loser's tests
must be updated, not deleted.

### Tooling and gates

| Item | Branch | Why |
|---|---|---|
| **`audit_walker_fields.sh` decided membership on PROSE** | r1piyr | Its recursion test counted the function name in COMMENTS AND STRINGS, so two non-recursive functions were audited by accident and **editing a comment moved a walker in or out of the audited set**. Baseline 758 -> 712. This is a gate I installed in §B; the membership bug came with it |
| `tools/grammar_closure_probe.sh` (new) | 29fiao | An integer in every argument slot of every intrinsic, with GCC's `-Werror=int-conversion` as the oracle. **The closure claim had never been measured and failed on first measurement** (H4). Verified non-vacuous: 2 breaches pre-fix, 0 after |
| `tools/ub_sweep.sh` had a real defect | 29fiao | It joined each run's stdout into one delimiter-separated string and re-split it, so any MULTI-LINE output split at the newline and was reported divergent with identical output |
| `emit_audit` widened from 5 samples to 1088 programs | 29fiao | The give-up paths I measured at "0 of 1146" are "one array-of-pointers away" per their analysis |
| sink_matrix 78 -> 88 (p18 axis) | 29fiao | gates H1 |
| Untrack ~560 tracked test binaries | 29fiao / qa249l | Both do it; **29fiao's is the better one** — a STRUCTURAL rule (an extension-less file under a test tree is a build artifact by construction, verified: all 558 were ELF) rather than qa249l's name list, which is the same deny-list shape this project keeps replacing with gates |
| Build warnings 27 -> 0 | r1piyr / qa249l | r1piyr's includes deleting **the drifted emitter copies of the shared-root walker that my BUG-817 left behind** — worth taking for that alone |

### Documented, deliberately NOT fixed (adopt the reasoning, not a fix)

- **29fiao BUG-852** — a non-optional pointer MEMBER is null after auto-zero; four
  spellings accepted, silent on bare metal where address 0 reads back. The blanket rule
  was implemented and **measured at 34 correct corpus files**, so it was not shipped.
  Four reproducers live in `tests/zer_gaps/` where compile-clean IS the gap.
- **29fiao BUG-859** — an arena pointer cannot be stored into another ARENA allocation.
  The obvious exemption was implemented and **opened a hole on the first negative**:
  `Symbol.is_from_arena` is already set on a pointer param by the very store being
  checked. Reverted, negative shipped as a lock.

### Conflict groups

1. **H1 + H19** — the view class and its boundary positives are one change.
2. **H11 is the sibling of my BUG-842.** I split the `arg_count` precondition; the
   TOP-LEVEL-KIND shape is still there. Same rule, second axis.
3. **H6 extends my BUG-843**, H9 extends **BUG-841**, H14 corrects **O3** — three rows
   where the previous harvest closed one spelling of a question and left others.
4. **The float decision gates two positives on each side** — resolve it before taking
   either branch's tests.

---

## CLOSED 2026-08-27 (BUG-912) — comptime array-element bindings carry no width

**The LOW rating was WRONG, and that is the lesson.** The entry argued the impact was low
because "the wrong value is a compile-time constant". The wrong VALUE is low-impact; what
the entry missed is what the value is USED FOR — `comptime if (arr_sum() > 255)` takes the
wrong arm, so conditional compilation EMITS THE WRONG CODE and discards the right one, with
no diagnostic anywhere. A severity call made from the shape of the wrong value rather than
from its consequence. Closed from `osp1a7`; `all_forms` 55 -> 0, `branch_select` 1 -> 0.

### Original entry, kept for the measurement


BUG-844 established the declared width at the three comptime binding sinks (params via
two call paths, and local var-decls) and applies it at every binary and unary operation,
so the interpreter now agrees with the emitted code for every scalar, signed,
mixed-width and nested-call form. **Array elements are the one binding kind still
unwidthed** — `ComptimeParam.array_values` is a bare `int64_t *` with no per-element
width, so:

    comptime u32 a_elem() { u8[2] v; v[0] = 200; v[1] = 100; return v[0] + v[1]; }

folds to 300 where the runtime gives 44. Measured precisely: 4z36e0's
`tests/zer/comptime_width_wrap_all_forms.zer` exits **55**, which is exactly that
check; every other form in that file passes. Not shipped as a test here because it
would be a known-failing positive — pjtawx's `comptime_width_wrap_agreement.zer` (which
passes) is installed instead.

**Fix sketch:** `array_values` needs the element width alongside it. The array binding
sites are the same `ct_ctx_set`/`ct_ctx_set_w` family; give the array form the element
type's width from the declared `T[N]` and wrap on element store and on element read.
Same shape as the scalar fix, one more sink.

**Why it is LOW:** the wrong value is a compile-time constant in a comptime function
using an array of a NON-native-width type, which no corpus program does — the corpus
cost of the whole BUG-844 change measured zero. It is recorded because a silent
disagreement between the interpreter and the emitted code is the exact class BUG-844
exists to close, and a partially-closed class is how this one came back.

---

## CLOSED — harvest tracker: five `claude/vigilant-tesla-*` branches, verified against main 2026-08-22 (the B1..B9 / F1..F4 rows are before/after verification tables, not findings)

**Status: NOT implemented. This is a catalog, not a changelog.** Every row below was
verified on a clean `make zerc` build of main (`5fef1d06`) by running **the branch's own
test file**, reading the DIAGNOSTIC rather than the exit code, and routing around maskings.
Rows that main already closed are listed in the "already on main" table so nobody
re-implements them.

**HARVEST CLOSED 2026-08-23 — 45/45.** §A (BUG-815..819), §B (820..829), §C (830..838),
§D (839..846) and §E/§F/§G (847..855) are all landed; every V/W/O/Q row below is struck
through. `make check` exit 0 with 1316 .zer tests and all seven audit gates. What is NOT
closed is recorded as its own OPEN entry above (the comptime array-element width), plus
the "reported but NOT fixed on any branch" list further down, which was never part of
the 45.

**PROGRESS: §C landed 2026-08-22** (BUG-830..838) — the bare-metal family. V4, V5, V7,
V8, V22, V23, V26, V27 and O2 struck through below. Also hardened the TRAP HARNESS
(no timeout + "any non-zero exit" was a weak oracle) and widened the runner's
`// zerc-flags:` directive from line 1 to the first five lines. Remaining: 14.

**PROGRESS: §B landed 2026-08-22** (BUG-820..829) — the walker FIELD-descent gate
`tools/audit_walker_fields.sh` is now in `make check` (and `make check-walker-fields`).
It reported **119 missing descents on main**; 21 vanished with a dead walker, 51 were
real descents added, 47 stale baseline rows removed in the same commit. W1..W10 and O1
are struck through below. Remaining: 23.

**PROGRESS: §A landed 2026-08-22** (BUG-815..819) — V1, V2, V3 and W7 are struck through
below. Sink matrix grew 65 -> 78 cells (SHAPES p16 + p17) and was verified non-vacuous:
8 holes against a from-HEAD pre-fix build, 0 after. 11 negatives installed, every one
confirmed to flip ACC -> REJ. Remaining: 41.

### Source branches

| Branch | Forked at | Commits | Theme |
|---|---|---|---|
| `vigilant-tesla-87xihb` | `47b0413a` (2 behind main) | 8 | bare-metal: volatile, packed, RMW, bounds |
| `vigilant-tesla-pjtawx` | `47b0413a` (2 behind main) | 14 | broadest: escape, lowering, bare-metal, atomics |
| `vigilant-tesla-4z36e0` | `5fef1d06` (main) | 13 | builtin init, optionals, comptime, global-init |
| `vigilant-tesla-pmytnl` | `5fef1d06` (main) | 2 | UB sweep: float casts, enum forging, CLI |
| `vigilant-tesla-39294y` | `5fef1d06` (main) | 3 | walker FIELD-descent gate + its 11 defects |

**BUG NUMBERS COLLIDE ACROSS BRANCHES — never cite one without its branch.** Four branches
independently number things BUG-802..814 for *different* bugs. `87xihb` and `pjtawx` forked
BEFORE main's `e7e51eea`, so they also rediscovered bugs main has since closed.

### Already on main — do NOT re-implement (verified: the branches' own negatives REJECT)

| Class | Branch rows | Main's fix |
|---|---|---|
| Deref-identity boundary (4 forms) | 87xihb BUG-796, pjtawx BUG-796 | BUG-798 |
| Provably-OOB index, array + MMIO sinks | 87xihb BUG-800, pjtawx BUG-799 | BUG-796 (`index_range_verdict`) |
| `volatile` laundered via `@ptrtoint`->`@inttoptr` | 87xihb BUG-802, pjtawx BUG-800 | BUG-797 (`is_volatile_addr_derived`) |
| `@inttoptr` bound to non-volatile destination | 87xihb BUG-802, pjtawx BUG-801 | BUG-799 |
| RMW reaching a global through a helper | 87xihb BUG-806 | BUG-801 (`rmw_param_mask`) |
| Merge conflict in `compound_field_maybe_freed.zer` | pmytnl BUG-806 | already resolved |

Diagnostic WORDING differs on three of these (`volatile_launder_ptrtoint_*`,
`packed_field_addr_deref`) — the branches' `expect-error` strings do not match main's
phrasing. Cosmetic; the rules fire.

### LIVE on main — accept-unsafe (a wrong program is accepted)

| # | Fix | Best version | Evidence on main |
|---|---|---|---|
| ~~V1~~ **DONE 2026-08-22 (BUG-815)** | `arg_is_local_derived` never called `unwrap_ptr_launder` — a laundered ARGUMENT escapes | **pjtawx** BUG-807 | `g = idfn(&x)` REJECTED; `(*u32)(&x)`, `@ptrcast`, `@pun` all ACCEPTED. The C-style cast is DELETED by the emitter, so emitted C is byte-identical to the rejected form. ASan stack-use-after-return. Argument-side sibling of main's BUG-791 |
| ~~V2~~ **DONE 2026-08-22 (BUG-816)** | ARENA lifetime missing from THREE frame-bound helpers | **pjtawx** BUG-803 | 4 negatives accepted: launder-then-store, orelse-fallback, struct-literal to global, struct-literal return |
| ~~V3~~ **DONE 2026-08-22 (BUG-817/818)** | Root-ident walk written 4x as two SEQUENTIAL loops — wrong for any ALTERNATING chain | **pjtawx** BUG-804 (`expr_root_ident` in ast.h) | `@atomic_load(&g_s.arr[0].f)` on a packed nested struct ACCEPTED; `@atomic_load(&g_i.f)` rejected one line away. Misaligned atomic = hard fault on ARM/RISC-V |
| ~~V4~~ **DONE (BUG-833)** | `&packed.field` gated at 1 of 5 sinks | **87xihb** BUG-804 (5 sinks + non-sticky) | assign / call-arg / alias sinks ACCEPTED. `39294y` BUG-813 is a 2-sink subset — take 87xihb, keep 39294y's `zer_gaps` file for the 4 sinks BOTH leave open |
| ~~V5~~ **DONE (BUG-834)** | Bit-range write is an implicit RMW, missed at BOTH sinks | **87xihb** BUG-803 | ACCEPTED while `flags += 1` is REJECTED — same operation, different spelling. `resolve_write_target_global` does not peel `NODE_SLICE` |
| ~~V6~~ **DONE (BUG-856)** | `expr_mentions_global` missed intrinsic/call/orelse | **39294y** BUG-811 | `g = @truncate(u32,g) + 1` ACCEPTED; `g = g + 1` REJECTED |
| ~~V7~~ **DONE (BUG-830)** | `type_equals` pointer arm checked `is_const` but NOT `is_volatile` | **87xihb** BUG-801 | struct-init field + funcptr param ACCEPTED. The SLICE arm 3 lines away checks both |
| ~~V8~~ **DONE (BUG-832)** | 4 of 6 sinks blind to the `?*T` optional axis | **pjtawx** BUG-810 | `?*T` call-arg and `?*T` return ACCEPTED. **Complementary to V7, not a duplicate** — V7 is the type-system root, V8 is the hand-rolled per-sink comparison |
| ~~V9~~ **DONE (BUG-842)** | `@atomic_*` in a global initializer emits broken C | **4z36e0** BUG-808 (structural) | Emits literally `uint32_t gres = ;`. pjtawx BUG-806 fixes only the atomics subset; 4z36e0 fixes the CAUSE — a name-check wrongly nested inside an `arg_count >= 1` precondition belonging to a different rule |
| ~~V10~~ **DONE (BUG-842)** | Zero-arg intrinsic skips the global-init guard entirely | **4z36e0** BUG-808 | `const u32 G = @cpu_model_id();` checker-accepted, only GCC rejects |
| ~~V11~~ **DONE (BUG-841)** | `struct == struct` / `union` comparison silently ALWAYS FALSE | **pjtawx** BUG-798 | Emitter answers a literal `0`; `a != b` is also false, which is the tell |
| ~~V12~~ **DONE (BUG-840)** | `switch` with a NON-FINAL `default` arm miscompiles | **pjtawx** BUG-808 | `switch(1) { default => r=9; 1 => r=1; }` returns **9**. Every arm after `default` becomes dead C |
| ~~V13~~ **DONE (BUG-846)** | Leak in a both-arms-return `if` never reported | **pjtawx** BUG-809 | 2 negatives accepted. Three independent layers, none sufficient alone |
| ~~V14~~ **DONE (BUG-839)** | `arena.alloc_slice` overflow guard is DEAD CODE (AST-only since the 2026-04 IR migration) | **pjtawx** BUG-797 | Test exits 1. Wraps the byte count to 0, then hands back a slice reporting 2^61 elements — **every downstream bounds check then passes** |
| ~~V15~~ **DONE (BUG-844)** | Comptime folds a DIFFERENT value than the same expression at runtime | **4z36e0** BUG-802/803 | Test exits 1. pjtawx BUG-802 is the same fix; 4z36e0's is later and sets the width at all THREE folding sinks |
| ~~V16~~ **DONE (BUG-843)** | `@bitcast` forges an out-of-variant enum; switch silently runs its LAST arm | **pmytnl** BUG-804 | `@bitcast(State,7)` takes `.done`, exit 12. Only route in — there is no int->enum cast |
| ~~V17~~ **DONE (BUG-845)** | float -> integer conversion is C UB at all three cast sites | **pmytnl** BUG-802 | Both negatives accepted, AND the divergence CONFIRMED end-to-end through ZER: `f64 g = -1.5; (u32)g` prints **4294967295 at -O0 and 0 at -O2** from one emitted .c on one gcc (7.5.0). **Probe note, recorded because it cost a wrong conclusion first time:** a `volatile` source SUPPRESSES the divergence (both levels give 4294967295) — volatile forces the `cvttsd2si` instruction at every -O level, while the bug lives in the CONSTANT-FOLDING path, where GCC folds with its own arbitrary-precision semantics instead. Probe with a plain constant, never a volatile one |
| ~~V18~~ **DONE (BUG-848)** | Barrier never initialised: `@barrier_wait` on `{0}` returns SUCCESS immediately | **4z36e0** BUG-806 | Accepted, runs, exits 0. Worse than the arena case — a dead barrier reports success |
| ~~V19~~ **DONE (BUG-848)** | Arena with no backing store: every `alloc()` returns null forever | **4z36e0** BUG-804 | Accepted. Hid 5 VACUOUS tests incl. a 120-line "real program" that ran 4 lines |
| ~~V20~~ **DONE (BUG-849)** | `a.over(buf);` as a bare statement is a silent no-op | **4z36e0** BUG-805 | `.over` is a constructor returning BY VALUE; as a statement it builds into a discarded temp |
| ~~V21~~ **DONE (BUG-850)** | A discarded `?T` silently throws the failure away | **4z36e0** BUG-807 | `rb.push_checked(a);` — the one method whose entire purpose is reporting overflow, silently not reporting it. Subsumes the ghost-handle check. Corpus cost measured at ZERO (all 6 hits are ghost-handle negatives) |
| ~~V22~~ **DONE (BUG-836)** | `--stack-limit` never sums main + ISRs, which share one stack | **pjtawx** BUG-811 | Two 200-byte chains pass `--stack-limit 256`. The tool AFFIRMS a budget the target cannot honour |
| ~~V23~~ **DONE (BUG-835)** | Auto-guard's `return` leaks a held lock and the interrupt-disable | **87xihb** BUG-805 | **The lock form HANGS (deadlock, verified exit 124); the `@critical` form exits 0 silently.** The compiler emits the exact construct it hard-errors users for writing. Main has `guard_traps` for defer bodies only — needs `noreturn_scope_depth` counted inside `emit_shared_lock_mode`/`emit_shared_unlock` and both `IR_CRITICAL` handlers |
| ~~V24~~ **DONE (BUG-855)** | Unrecognised CLI options silently ignored | **pmytnl** BUG-805 | `--totally-bogus-flag`, `--target-arch=nonsense`, `--stack-limit=abc` all exit 0 with no diagnostic. `--target-arch=arm64` (valid spelling is `aarch64`) silently builds x86 |
| ~~V25~~ **DONE (BUG-854)** | `break`/`continue` in a for-INITIALISER binds to the ENCLOSING loop | **pjtawx** BUG-813 | Accepted. Branch REJECTS rather than rebinds — the semantics is genuinely ambiguous (`continue` would jump to the step of a loop whose induction variable was never initialised). Corpus cost zero |
| ~~V26~~ **DONE (BUG-837)** | No way to declare a bare-metal target; `@critical` degrades to a fence | **pjtawx** BUG-812 | `_ZER_HOSTED`/`ZER_FREESTANDING` absent. Scope is narrower than the branch first claimed: ARM/RISC-V/AVR key on the ARCH macro and were always correct — real exposure is **bare-metal x86** (kernel, bootloader, EFI) |
| ~~V27~~ **DONE (BUG-838)** | MMIO boot validator could never fire, and trying cost a boot hang | **pjtawx** BUG-814 | Gated to exactly the targets where `_zer_probe` hardcodes success. The read still happens from a constructor — before any RCC clock-enable, i.e. a BusFault on a clock-gated peripheral |

### LIVE on main — the walker FIELD-descent family (all `39294y`, one gate found all of them)

`-Werror=switch` + `walker_default_audit.sh` guarantee every safety walker NAMES every
NodeKind. **Nothing checked whether an arm that names a kind actually DESCENDS its
children.** `tools/audit_walker_fields.sh` (+ a 930-line baseline) is the missing gate;
these 11 are its output, each verified live.

| # | Defect | Evidence on main |
|---|---|---|
| ~~W1~~ **DONE (BUG-820)** | `scan_body_shared_types` / `cond_pred_foreign_shared` / `record_atomic_plain_in_callee` missed `call.callee` | `cond_wait_foreign_shared_callee`, `shared_callee_transitive_deadlock` ACCEPTED. **Main's BUG-795 fixed this at the ir_lower + checker sites only — these two are still open** |
| ~~W2~~ **DONE (BUG-821)** | `scan_func_props` missed `orelse.fallback` / `struct_init` / `slice` / `await.cond` | 3 negatives accepted. `@critical { u32 v = maybe() orelse starter(); }` emits `pthread_create` between `cpsid i` and `msr primask`. Verified: direct form REJECTED, orelse form ACCEPTED |
| ~~W3~~ **DONE (BUG-822)** | spawn race scan treated `@critical` as a LEAF | `spawn_race_via_critical` accepted. `@critical` disables interrupts on one core and gives NO cross-thread exclusion — it reads as synchronisation and is not. (Branch notes `@once` was in v1 of this fix and was WRONG — it publishes with ACQ_REL; reverted) |
| ~~W4~~ **DONE (BUG-823)** | `expr_contains_yield` missed `orelse.fallback` | Shared mutex held ACROSS a coroutine suspend, verified in emitted C |
| ~~W5~~ **DONE (BUG-824)** | `check_call_provenance` reached a call at only three positions | 3 negatives accepted. `if (process(@ptrcast(*opaque,&g_motor)) > 0)` compiles; identical call as a var-decl init is rejected |
| ~~W6~~ **DONE in §A (BUG-817)** | the emitter's drifted copy of the shared-root walker | `defer { sink(g.cb()); }` emits with NO mutex. Fix unifies to one `ir_find_shared_root_expr`, deletes ~170 lines + 6 dead walkers. **Complements V3** (pjtawx does ast.h/checker/zercheck_ir; this does emitter.c) |
| ~~W7~~ **DONE 2026-08-22 (BUG-819)** | `ir_defer_scan_uses` looked at expression statements only | `defer_use_in_condition_uaf`, `defer_use_in_vardecl_uaf` accepted — unreported UAF |
| ~~W8~~ **DONE (BUG-825)** | `scan_frame` skipped loop cond/step, assign target, slice bounds, callee | recursion + `--stack-limit` blind to a call in a condition |
| ~~W9~~ **DONE (BUG-826)** | ISR sibling + VRP invalidation descents | = V6 |
| ~~W10~~ **DONE (BUG-829)** | `ir_defer_free_arg` knew only 3 builtin free spellings | = O1 |
| ~~W11~~ **DONE (BUG-852)** | `naked` silently dropped since the IR migration | see Q1 — still OPEN (§F) |

**Latent, documented, not fixable yet:** no walker descends `asm` operand expressions.
Unreachable only because `asm` is naked-only; opens seven holes at once the day S1 relaxes.

### LIVE on main — over-rejections (valid code refused)

| # | Fix | Best version | Evidence |
|---|---|---|---|
| ~~O1~~ **DONE (BUG-829)** | `defer sensor_close(dev)` reported as a leak; the DIRECT call is recognised | **39294y** BUG-812 (`ir_defer_free_arg`) | `cinterop_defer_close_ok` and `defer_extern_destructor_no_false_leak` both hard-error. This is the flagship "Safe C Library Interop" example in `reference.md` — the docs assert it compiles and it does not. pjtawx BUG-805 is the same fix; take either, keep both tests |
| ~~O2~~ **DONE (BUG-833)** | Sticky packed-derived flag refuses a re-cleared pointer | **87xihb** BUG-804 (boundary positive) | `packed_aligned_forms_ok` hard-errors on main. The fix SETS on a packed-derived RHS and CLEARS otherwise |
| **O3 — STILL OPEN** | `&&`/`||` do not narrow their RHS | **87xihb** BUG-800 | `if (i < 4 && arr[i] > 0)` — the canonical guarded idiom, and the exact shape the auto-guard warning tells users to write — still carries a runtime guard. (Since BUG-1273 that check is the INLINE single-read trap at the access, not the hoisted early-return guard, which returned from the function even when the RHS never ran.) **PRECISION ONLY on main** (warning, compiles). It becomes REQUIRED if the always-OOB verdict is ever promoted at short-circuit position |

### Quality / no-longer-silent (not soundness)

| # | Item | Best version |
|---|---|---|
| ~~Q1~~ **DONE (BUG-852)** | `naked` silently dropped -> warn at the declaration | **4z36e0**. Deeper measurement than 39294y's identical fix: GCC 13 x86-64 DOES support the attribute (emits `endbr64; body; ud2`, no `ret`), 16 of 18 `asm_*.zer` positives CALL their naked function so flipping it on SIGILLs all 16, and `naked` is OVERLOADED (it is the only way to get asm permission). 43 corpus files emit the warning — that number IS the finding |
| ~~Q2~~ **DONE (BUG-853)** | Value-returning `async` has no retrieval API -> warn | **4z36e0**. Reject was measured and rejected: corpus cost is 1 file, and it is the BH-18 #10 regression guard |
| ~~Q3~~ **DONE (BUG-851)** | Emitter's five give-up paths are silent miscompiles | **4z36e0**. `/* complex callee */(a, b)` is a valid C COMMA EXPRESSION — compiles, runs, CALLS NOTHING. Measured 0/1170 corpus programs reach them, so `emit_unreachable()` aborts nothing reachable |
| ~~Q4~~ **DONE (BUG-845)** | `@saturate` not total (float bounds rounded UP; AST signed-64 arm had no clamp) | **pmytnl**, rides with V17 |

### Tooling and gates (pure gain — no behaviour change)

| Tool | Branch | Why |
|---|---|---|
| `tools/audit_walker_fields.sh` + 930-line baseline | 39294y | **Highest value here.** The FIELD half of the walker discipline; found all 11 W-rows. Self-checked against `ast.h`. Its own blind spots (if/else chains, children handled before the switch) are stated in the script |
| `tests/test_vrp_join_matrix.c` (32 cells) | 4z36e0 | Closes CLAUDE.md's "VRP range JOIN — NO auto-gate, checklist every control-flow kind" row. Verified to FIRE by reproducing BH-18 #2 behind an env flag. Its first draft was VACUOUS (open-ended pass rule swallowed exit 134 and 127) — now a CLOSED safe-set |
| `tests/test_global_init_matrix.c` (54 cells) | 4z36e0 | DERIVES the intrinsic set from the compiler at run time, so a new intrinsic is covered the day it lands. Both prior hand-maintained lists lacked exactly that |
| Trap-harness: `timeout` + `// expect-trap` | 87xihb | Main HAS `ZER_RUN_TIMEOUT`; it LACKS `expect-trap`. "Any non-zero exit" cannot distinguish a hang, a SIGSEGV, a wrong answer and a genuine trap |
| `tools/audit_reference_examples.sh` | pjtawx (preferred) / 87xihb | Two implementations. pjtawx's checks BOTH directions and prints its skipped-fragment count; 87xihb's is opt-in-marked and asserts the REASON. Both were verified to fire by injection |
| `tools/audit_freestanding.sh` | pjtawx | Needs no cross-toolchain. Must test `-DZER_FREESTANDING` ALONE — under `-ffreestanding` old and new predicates agree, so that combination scores a broken compiler as PASSING |
| `tools/ub_sweep.sh`, `tools/negative_reason_audit.sh` | pmytnl | Differential UB detector (-O0/-O2, +/- `-fno-strict-aliasing`). The reason-audit reports the standing exposure: **125 of 575 negatives assert a reason, 450 do not** |
| `tools/audit_doc_examples.sh` | 39294y | Overlaps the two above; take one |
| SCOPED-BORROW grid (11 cells, conc-matrix 84 -> 93) | 39294y | Pre-work for the all-arms-join relaxation, with three verdicts (ACCEPT / OVERREJ / REJECT) so the grid stays green ACROSS the relaxation. The BV_REJECT rows are the preconditions the relaxation must discharge |

### Repo hygiene (owner call)

- **~558 compiled test binaries are TRACKED** (~9.1 MB). Every `make check` dirties them, so
  `git status` is useless for review. Best version: **pmytnl** (`.gitignore` by shape +
  the measurement). pjtawx removes 560; 4z36e0 covers 23. Untracking the existing set is a
  `git rm --cached` sweep — an owner decision, not a fix.
- **Build warnings 27 -> 0** (pmytnl): a `size_t`-into-`%.*s` varargs mismatch, a missing
  return in `lower_expr`, a sign-compare, two `calloc(int,...)` clamps, six self-closing
  comments, ~350 lines of provably-dead code.
- **`reference.md` intrinsic coverage**: four branches did this independently
  (4z36e0: 96, pjtawx: 98, pmytnl: 98, 39294y: 80). Best: **4z36e0** — signatures MEASURED
  by probing the compiler, return types disambiguated by narrow assignments (u32 widens to
  u64 and would otherwise read as u64), and it caught one of its own wrong claims by
  RUNNING the examples. Note `@cpu_rdrand`/`@cpu_rdseed` return `?u64`, not `u64`.

### Reported but NOT fixed on any branch — carry as OPEN, do not "harvest"

- ~~**extern `*opaque` crosses the C boundary with an UNINITIALISED `type_id`**~~ — CLOSED
  2026-10-12 (BUG-1594, the `void *` bridge). Original text: (4z36e0,
  HIGH). ZER emits `_zer_opaque zzz_ptr(void);` where the real C function is
  `void *zzz_ptr(void);`. `.ptr` arrives correctly by ABI luck; `.type_id` is register
  residue. `@ptrcast`'s `!= 0` escape then either FALSE-TRAPS on correct code (measured,
  exit 133) or — when residue equals a live type id, and they are small consecutive
  integers — passes a WRONG cast silently. 32 declarations in-tree, including
  `lib/io.zer`, `lib/compat.zer`, `lib/fmt.zer`. Deliberately not fixed: both candidate
  fixes are risky (one lands in the IR/AST dual-dispatch hazard; the other's failure mode
  is an UNDER-rejection). Exhibiting it needs linking a separate C TU, which no harness does.
- **`vrp_ir.c` is ORPHANED** — no caller, absent from both Makefile source lists,
  `strings zerc | grep -c vrp_ir` is 0. Re-measured on 4z36e0: Phase 0's headline
  justification ("retires a CRITICAL live under-rejection") is STALE — that was BH-18 #2,
  fixed 2026-06-26; 13 probes show every post-join index auto-guarded. The architectural
  reason to wire it survives; the urgency does not.
- **`f64` -> `f32` narrowing overflow is C UB** (pmytnl, LOW latent).
- ~~**Two DISTINCT `@once` blocks touching the same global are not mutually exclusive**
  (39294y, LOW).~~ CLOSED 2026-09-24g (BUG-1276): refused at the spawn scan.
- **Two arena allocations cannot be LINKED** (4z36e0, over-rejection). Both share the
  arena's lifetime. Found because the doc's Arena example showed exactly that line and had
  never been compiled.
- **A bodyless declaration reusing a libc NAME** gets a confusing GCC error (4z36e0, LOW,
  LOUD). Inherent to emit-C: the 17-name `is_cstdlib` list must stay declarable for interop.

### Conflict groups — take together or not at all

1. **V7 + V8** (volatile strip). V7 is the type-system root (`type_equals` +
   `can_implicit_coerce`); V8 is the four hand-rolled sinks and the `?*T` axis. Landing
   only V7 leaves `?*T` call-arg/return open; only V8 leaves struct-init and the funcptr
   param match open.
2. **V4 + O2** (packed). The 5-sink widening and the non-sticky boundary are one change;
   the widening ALONE turns O2 into a worse over-rejection.
3. **V3 + W6** (root-ident walk). pjtawx unifies ast.h/checker/zercheck_ir; 39294y unifies
   the two remaining emitter.c copies. Landing one leaves a drifted duplicate of the same
   walker — the exact shape both fixes exist to kill.
4. **V9 + V10** (global init). Same `if` condition; V9's names without V10's condition
   split changes nothing for 7 of the 11 names — which is what happened on 4z36e0's first
   attempt, and what its new grid caught.
5. **O3 is a PRECONDITION** for promoting the always-OOB verdict at short-circuit position.
   Main warns today, so O3 is precision-only — but it becomes required the moment that
   verdict is tightened.

### Verification method (so a fresh session can reproduce this table)

Clean `rm -f *.o src/safety/*.o && make zerc` on `5fef1d06`, then for each branch:
`git show origin/claude/vigilant-tesla-<b>:tests/zer_fail/<t>.zer`, compile with
`-o out.c` to isolate the CHECKER's verdict from GCC's, and grep the DIAGNOSTIC
(excluding the echoed source line, which contains the offending text and defeats a
loose grep). Three maskings were hit and routed around: the non-shared-global rule
masks the RMW rule (use a `volatile` global), the non-null-initializer rule masks the
escape rules (use `?*T`), and warnings are not rejections (check the exit code, not
the presence of a message).

---

## ~~harvested 2026-08-17 from seven audit branches~~ — **ALL 39 CLOSED 2026-08-17**

Five structural fixes (BUG-791..795) closed 26; the six remaining independent
classes closed with BUG-796..801. Every hole was verified live on main using the
branches' OWN test files, in CHECKER-ONLY mode, and every one now has a regression
test. Corpus cost measured at zero for each tightening.

Residuals of this work that are still open are listed below.

### Still OPEN — residuals
- **No automated gate for the MISSING-LOCK half of BUG-795.** A missing lock emits
  no diagnostic and has a nondeterministic runtime effect, so neither harness holds
  it; currently verified only by diffing emitted C against a pre-fix build.
  **Fix sketch:** assert lock/unlock PAIR COUNT around a statement, the way
  `tests/test_defer_goto_matrix.c` asserts an acquire/release balance.
- **~30 hand-rolled launder-peel sites remain** in checker.c against ~15 uses of the
  shared peeler. BUG-791 converted the ones that were live holes; the rest are the
  standing debt that keeps regenerating this class.
- **45 stale rows in `tools/type_dispatch_baseline.txt`** (content no longer in the
  sources). Harmless — the audit only fails on NEW sites — but it is a gate drifting
  from its site set. Rows made stale by BUG-793/796..801 were removed; these 45
  predate them and were left rather than bulk-edited unverified.

### Unverified — reproduce before acting (unchanged)
- j8f9t7's limitations.md carries a **SUSPECTED** block (code-reading only).
- jjfk1k verified-and-deliberately-skipped: `@once` + a user `@sem_acquire`/`release`
  pair; global-scope `@inttoptr` with a non-literal address emits invalid C; a
  negative literal into a wide unsigned type is accepted; `rust_tests/run_tests.sh`
  uses a hard `timeout 10` so pthread tests flake under load.

## DONE — HARVEST COMPLETE: all 45 fixes from eleven `claude/gifted-noether-*` branches landed (2026-08-01 → 2026-08-02)

**STATUS: all 45 landed.** §A 1/1 · §B 9/9 · §C 7/7 · §D 9/9 · §E 1/1 · §F 4/4 · §G 6/7 (+ G5 with
§J) · §H 2/2 · §I 1/1 · §J 4/4. Commits: `60394142` `0d5aa553` `22b7c56d` `bc11a6cb` `342396e0`
`213ee006` `f8374bc1` `06c0ed9d` `126bfb42` `2670c222` + this one.

**Method used throughout — keep it for the next wave.** Every reproducer was run against current main
BEFORE implementing, and every new test was verified DISCRIMINATING against a from-HEAD
`git archive` build. That caught, in order: three NON-DISCRIMINATING branch tests (§B2/§B3/§B9 index
a HEAP slice, whose runtime bounds check fires on a buggy AND a fixed compiler — rewritten against a
FIXED ARRAY); a WRONG root-cause attribution (§C7's defect was upstream of where the commit message
pointed, and its keep-call half was already fixed on main); a dependency that had to be MEASURED in
both directions (§G5 does not reproduce standalone but DOES once §J4 ties the knot); and a family
whose live sites were NOT the function the description named (§J2 is the var-decl/assignment gates,
not `check_volatile_strip`).

**One item remains OPEN and is tracked below, deliberately:** the durable fix for §F2
(per-defer runtime "armed" flag). §F2 shipped as a sound interim REJECT.

**The two class-kill gates built during this wave** (`tools/audit_carrier_dispatch.sh` + the
`LD_OPTWRAP` escape-matrix axis) are described in CLAUDE.md; a THIRD was measured and deliberately
NOT built (a region-presence gate for the AST→IR wrapper class would have been GREEN on the live
§G3 bug — see BUGS-FIXED.md).

### Original harvest analysis (retained for provenance)


**What this is.** A THIRD wave of `claude/*` audit branches (2026-07-21 → 07-31), created after the
2026-07-19 tracker below. Cross-referenced 2026-08-01 against current main (`31a355a3`):
**45 distinct fixes across 20 substantive commits, ALL absent from main.** Verified three ways:
(1) `git cherry -v main <branch>` is `+` for every commit; (2) main gained **zero compiler fixes**
since the common fork base `38b7a1e7` (2026-07-20) — its only post-fork commits are the compilation
tracer, a `.gitignore`, and docs; (3) each fix's distinctive marker was grepped in main and returns
0 hits (`ir_register_global_field_store`, `ir_find_store_source_local`, `intn_carrier_suffix`,
`freed_defer_id`, `vrp_widen_loop_addr_taken`, `field_obj_needs_parens`, `check_const_strip`,
`record_isr_funcname_binding`, `sc_expr_has_orelse`, `_zer_opt_u128`). Main still carries the OLD
line-based defer double-free gate (`free_line != defer_line`, zercheck_ir.c ~2012) that §F1 replaces.

**Nothing in this wave is already in main — there is no "do NOT re-open" list.** (Contrast the
2026-07-19 tracker, which had M1/M2.) `ir_measure_key_path` / `ir_key_root_ident` DO exist in main
and are reused by the §A fixes — that is a dependency, not a duplicate.

**Root-cause pattern.** Same MULTI-SITE / per-sink patchwork class as the previous two waves
(CLAUDE.md "MULTI-SITE SAFETY IS THE #1 RECURRING BUG CLASS"). The clusters: (a) the global-dangle
sink registered only a BARE ident store, so every `.field`/`[i]` projection slipped — found
independently **seven times** (§A); (b) VRP-JOIN still wired per node-kind, so `@once`/defer/orelse/
if-capture/loop-body/else-body each leak separately (§B); (c) pointer-only type-kind gates at the
escape sinks that must widen to `type_can_carry_pointer`/`type_carries_data_pointer` (§C); (d) the
spawn arg check missing one arg SHAPE per branch (§D). Coverage was audit-found, not proof-found.

**Rules for consuming this:** (1) apply the PROPER version per bug (noted per row), not a whole
branch — the duplication is extreme here (§A ×7, §B ×7 branches); (2) rebase onto current HEAD and
re-verify — each was green on its own fork base; (3) fixes pile onto the SAME functions across
branches → apply per-FAMILY, re-verify after each (conflict groups at the end); (4) drop junk commit
`9c18deb7` (542 rebuilt test binaries, 0 source change). To inspect: `git show <sha>`.
Severity: ~31 memory-safety (UAF / OOB / race), ~10 miscompiles, 2 crash/DoS, 1 over-rejection (J4).

### Source branches (fork base → commits)
| Branch | Base | Commits (short) |
|---|---|---|
| gifted-noether-bz5q89 | 38b7a1e7 | ff38052a |
| gifted-noether-38z6wi | 38b7a1e7 | 835eeb26, d1ccb8aa, ddf6ac47 (+9c18deb7 junk) |
| gifted-noether-3o10j6 | 38b7a1e7 | 9c67c06b, d1a7d9cc, d576ae2a |
| gifted-noether-xxhbdg | 38b7a1e7 | f80166fd (+ddf58e8f doc) |
| gifted-noether-n0odo5 | 38b7a1e7 | 2d6ec421, 17ec74ca |
| gifted-noether-rdh99l | 38b7a1e7 | 8af2573d, d9060007, 9a40ead4, 23f34ab1 |
| gifted-noether-8rv51h | 38b7a1e7 | e920ffb9 (+46cb1077 doc) |
| gifted-noether-7fxhb3 | 38b7a1e7 | 31796ef8 |
| gifted-noether-02nq43 | 38b7a1e7 | 7aac453a |
| gifted-noether-i0txin | 351860e4 | a9ba4c77, cd9bc560 (+84097263 doc) |
| gifted-noether-rvek5f | 351860e4 | 00f3c2af |

### A. G5 — heap pointer into a global's FIELD/INDEX dangles unflagged (CRITICAL UAF) — **DONE 2026-08-01**

**LANDED** as the 38z6wi⊕02nq43 synthesis described below (commit on main; see BUGS-FIXED.md
2026-08-01). `ir_register_global_field_store` wired into all THREE store sinks + the launder-aware
`ir_find_store_source_local` at the two global-dangle sinks. Verified strictly stronger than every
branch version: the combined case (laundered RHS INTO a projection, `g.p = @ptrcast(*N, n)`) is
caught, which NEITHER source branch catches alone. 5 negatives + 2 positives added, sink matrix
grown 44 → 48 and CLEAN, make check 0 (ZER 1021/0). BUG-742 conservatism verified preserved
(the conditional-free positive compiles). **This also closes §G "G5" in the 2026-07-19 tracker.**
Historical comparison of the 7 branch impls retained below for provenance.

**fixed 7× independently**

`g.p = n; free(n)` (g a struct/array global) leaves `g.p` dangling with NO diagnostic. Bare `g = n`
IS caught (GAP-3/BUG-739); only the projection sink was missed. This is §G's **G5** below — the
long-standing documented gap. All seven register a compound `(IR_GLOBAL_ROOT_ID, "g.p")` pseudo-root
sharing the RHS `alloc_id`, so free-propagation + the exit-dangle/call-window checks reach it, and
all correctly preserve the BUG-742 conservatism (DEFINITELY-freed only; `g.p = null;` resets).

| Impl | sha | Shape |
|---|---|---|
| bz5q89 | `ff38052a` | inline, 1 sink (IR_ASSIGN) |
| **38z6wi** | **`835eeb26`** | **helper `ir_register_global_field_store`, wired into ALL 3 store sinks (IR_ASSIGN / IR_FIELD_WRITE / IR_INDEX_WRITE)** |
| 3o10j6 | `9c67c06b` | inline, 1 sink |
| xxhbdg | `f80166fd` | inline, 1 sink |
| n0odo5 | `2d6ec421` | inline, 1 sink |
| 8rv51h | `e920ffb9` | helper `ir_build_global_field_key` (next to `ir_extract_compound_key`), 2 sinks |
| **02nq43** | **`7aac453a`** | **+ `ir_find_store_source_local`** — unwraps `@ptrcast/@pun/@bitcast/@cast/@container` and a slice `.ptr` read on the **RHS** |

**PROPER VERSION = 38z6wi ⊕ 02nq43 (take BOTH, they are orthogonal).** 38z6wi has the correct
structure: one shared helper at all three store sinks (the multi-site doctrine), the key built as
the FULL path `"g.p"` INCLUDING the root name (a relative `".p"` would collide across globals sharing
a field name), the `escaped=true` invariant preserved, and a null-reset branch. 02nq43 closes a facet
**all six others miss** — a LAUNDERED RHS (`g = @ptrcast(*T,n)`, `g = s.ptr`), both ASan-confirmed
heap-UAF. Synthesis: keep 38z6wi's helper, feed it 02nq43's `store_src_local` instead of `rhs_local`.
Both reuse `ir_measure_key_path` / `ir_key_root_ident`, already present in main.
Sink-matrix cells to add: `heap_glob_field_dangle` (bz5q89), `global_dangle_{ptrcast_launder,slice_ptr}` (02nq43).

### B. VRP range-JOIN leaks → silent OOB (CRITICAL) — **ALL 9 DONE 2026-08-01**

**LANDED (B1–B9).** Every one was verified REPRODUCING on main before the fix, then fixed and
re-verified. Operations are NOT uniform: MAY-RUN bodies (if-capture, orelse-block, `@once`) **JOIN**;
the `defer` body **RESTORES** (it runs at scope exit, so no following code can observe it); loop
bodies restore the post-pre-pass values (the body may run zero times).

| # | site | main (broken) | fixed |
|---|---|---|---|
| B1 | loop bodies FOR/WHILE/DO_WHILE | exit 42 | 0 |
| B2 | `if (o) \|v\|` capture | exit 100 | 0 |
| B3 | `orelse { block }` fallback | exit 107 | 0 |
| B4 | `defer { }` body | exit 2 | 0 |
| B5 | `@once { }` body | exit 100 | 0 |
| B6 | else-body nested guard | exit 100 | 0 |
| B7 | loop index via `&i` to a call | exit 42 | 0 |
| B8 | for signed non-const init | exit 1 | 0 |
| B9 | `find_return_range` orelse fallback | exit 18 | 0 |

**TEST-QUALITY WARNING (applies to anyone reusing these branches).** The branch reproducers for
**B2, B3 and B9 do NOT discriminate** — they index a HEAP SLICE, whose runtime bounds check fires
regardless of VRP, so they trap on a broken compiler AND a fixed one (all three PASSED on unfixed
main). Committed replacements use a FIXED ARRAY, where VRP elision is the actual mechanism, and each
was confirmed failing on main first. B9's test also is not at the path its commit message implies —
it is `tests/zer_trap/vrp_orelse_block_return_range_leak.zer`.

**JOIN-vs-RESTORE, resolved honestly.** `8rv51h` used RESTORE for `@once`/orelse, `7fxhb3` used JOIN
for `@once`. JOIN taken for all may-run bodies because it is sound BY CONSTRUCTION (it only widens,
so it can add a guard but never remove one). An attempt to exhibit RESTORE failing in the WIDENING
direction was **INCONCLUSIVE**: a control (`u32 idx = 1; garr[idx] = 5;` with no conditional body)
shows a literal-init variable index into a fixed array is not proven-elided in this build, so the
probe never discriminated the two operations. The choice rests on the construction argument, NOT on
a measured failure of RESTORE. Detail: BUGS-FIXED.md 2026-08-01.

Historical per-item detail retained below for provenance.


Main has the `vrp_snap_take/restore/join` machinery wired at NODE_IF / FOR / WHILE / SWITCH / LABEL
(§C #13 + the 2026-07-19 §A batch). These are the still-un-enumerated sites. **Mostly DISTINCT — take
nearly all.**

- **B1 — loop bodies FOR/WHILE/DO_WHILE leak a body narrowing past the join** (`rdh99l` `8af2573d`).
  Elides a later fixed-array bounds guard on a zero-trip loop. Snapshot the widened post-pre-pass
  ranges, restore after the body. Tests `vrp_loop_body_narrow_guard_{for,while}.zer`.
- **B2 — `if (o) |v| { }` capture path** (`8rv51h` `e920ffb9` ≡ `i0txin` `a9ba4c77`). The capture path
  early-breaks BEFORE the comparison/non-comparison VRP-JOIN block, so an in-body narrowing leaks past
  the merge. Both impls are the identical take/restore/join; take either. Tests
  `vrp_ifcapture_range_leak.zer` (8rv51h, zer_trap) / `vrp_capture_if_range_leak.zer` (i0txin).
- **B3 — `orelse { block }` fallback body** (`8rv51h` `e920ffb9`). WARNING: see the JOIN-vs-RESTORE note below.
  Test `vrp_orelse_block_range_leak.zer`.
- **B4 — `defer { }` body** (`7fxhb3` `31796ef8` ≡ `8rv51h` `e920ffb9`). RESTORE is CORRECT here (the
  body runs at scope exit, i.e. after all following statements — its narrowing must never apply to
  them). Tests `vrp_defer_body_narrowing_guarded.zer` / `vrp_defer_body_range_leak.zer`.
- **B5 — `@once { }` body** (**`7fxhb3` `31796ef8` — PROPER**; `8rv51h` `e920ffb9` is the weaker form).
  WARNING: 7fxhb3 uses **JOIN**, 8rv51h uses **RESTORE**. See the note below — take 7fxhb3.
  Test `vrp_once_body_narrowing_guarded.zer`.
- **B6 — else-body nested guard leaks its guard-inverse past the join** (`02nq43` `7aac453a`). The THEN
  body reset `var_range_count`; the ELSE body did not, so a guard nested in the else leaked to a later
  `arr[i]` on the THEN path (ASan stack-buffer-overflow). Fix is a symmetric reset that PRESERVES the
  current-if's own guard-inverse entries. Test `vrp_else_nested_guard_no_leak.zer`.
- **B7 — loop-carried index/divisor mutated via `&i` passed to a CALL keeps its stale narrow range**
  (`rvek5f` `00f3c2af` HOLE-A). `vrp_invalidate_loop_body_writes` widens only direct ASSIGN writes and
  skips calls; the per-call address-taken wipe (BUG-475) runs at the call site in program order, so
  `buf[i]; bump(&i);` proved `buf[i]` against the PRE-loop range → silent loop stack-buffer-overflow
  (ASan-confirmed, no compile error, no runtime trap). New companion pass `vrp_widen_loop_addr_taken`
  (no-default exhaustive AST walk) full-wipes every local whose address is taken anywhere in the loop
  body; run in the for/while/do-while drivers AFTER the cond narrowing. Over-widening is sound for VRP
  (only adds a guard) and only address-taken vars are touched, so plain counters keep the precise join.
  Test `vrp_loop_addrof_index.zer`.
- **B8 — `for (i32 i = start; i < 4; ...)` with a non-const-evaluable signed init** (`bz5q89`
  `ff38052a`). Seeded the loop-var range min to 0 unconditionally, but `i < N` only bounds ABOVE — a
  negative start was falsely proven in-bounds and the guard elided (ASan OOB read+write). while/do-while
  were already sound (INT64_MIN). Fix: for a non-const init seed `min = INT64_MIN` for a SIGNED loop var,
  keep `min = 0` for UNSIGNED (a true type invariant — precision preserved).
  Test `vrp_for_signed_neg_init_guard.zer`.
- **B9 — `find_return_range` drops a return hidden in an orelse-block fallback** (`n0odo5` `2d6ec421`).
  `x orelse { return BIG; }` attached to a var-decl init / expr-stmt / assignment RHS is a real return
  path the scan never reached; it fell to the `return true` default and SILENTLY dropped from the union,
  so the summary UNDER-approximated and a caller elided its bounds guard on `arr[f()]` (ASan-confirmed
  silent OOB write). Descend the orelse fallback with `in_branch=true`.
  Test `vrp_orelse_block_return_range_leak.zer`.

> WARNING: **JOIN vs RESTORE — resolve before implementing B3/B5.** `vrp_snap_restore` sets ranges back to
> the pre-body values; `vrp_snap_join` unions pre with post. For a **may-run** body the sound post-state
> is the UNION. RESTORE fixes the reported *narrowing* leak but not the widening direction: if `idx` is
> `[1,1]` pre and the body does `idx = 99`, restore yields `[1,1]`, a later `garr[idx]` is "proven", the
> guard is elided → OOB on the path where the body DID run. 7fxhb3 reasons about this explicitly
> ("@once needs JOIN (may-run) not full discard") and is the version to take for B5. The same argument
> applies to **B3** (an orelse block falls through on the Some path), where 8rv51h also used RESTORE —
> recommend upgrading it to JOIN. **Not empirically confirmed** (both branches' tests exercise only the
> narrowing direction); write a widening test case before committing either way. B4 (defer) is
> genuinely RESTORE — no code follows the body.

### C. Escape / dangling-pointer sinks (CRITICAL accept-unsafe) — **ALL 7 DONE 2026-08-01**

**LANDED (C1–C7).** All ten reproducers verified COMPILING on main first (a negative test that
compiles is unambiguously a hole — no confound). Every one is the same shape: a sink gating on a RAW
TYPE-KIND (`== TYPE_POINTER`) or requiring a BARE `NODE_IDENT`, so a wrapped type or a projected
value slipped while the plain form was caught.

Over-rejection guards verified: the BUG-764 param-view relaxations still compile
(`trim(s){return s[1..3];}`, `@cast(MySlice, param)`, param slice → optional global), the C6 keep
valve still accepts `&GLOBAL.field` / `&g_arr[0]`, and a pure-value Ring element still pushes from a
local. Sweep of all 44 pre-existing `tests/zer_fail/{keep,escape,return}_*.zer`: 0 regressions.

**Two corrections to the branch descriptions (do not trust them blindly):**
- **C7** is described by `rdh99l` `8af2573d` as "Ring.push/keep-call". The **keep-call half was
  already rejected on main** — only Ring.push was live. That commit ships NO test for either facet
  (its five tests cover the other four fixes in the bundle), so C7 needed a WRITTEN reproducer.
- **C7's real defect was upstream of where the description points.**
  `container_push_arg_escapes` already existed and was correctly gated; the failure was in
  `arg_is_local_derived`, whose NODE_CALL recursion fired only when the call's RESULT was a
  pointer/slice, excluding a pointer-carrying STRUCT.

Historical per-item detail retained below for provenance.


- **C1 — local array PROJECTED into a wrapped-slice global** (`38z6wi` `835eeb26` **⊕** `3o10j6`
  `d1a7d9cc` #1). `g = local.arr` / `g = grid[0]` into a `?[*]T` (or `distinct [*]T`) global slipped
  BOTH slice sinks: the bare-slice sink gates on `target->kind == TYPE_SLICE`, the wrapped sink gated on
  `value->kind == NODE_IDENT` → dangling global slice (ASan stack-use-after-return). **38z6wi is the
  broader fix** (covers `?[*]T` AND `distinct [*]T`, walks the value through FIELD/INDEX to the root,
  deliberately EXCLUDES plain `TYPE_SLICE` so it can't double-fire with the sibling sink at ~4982).
  **Take 3o10j6's by-ref array-PARAM exclusion on top** (`val_is_param`) — it prevents over-rejecting the
  BUG-764-class param view. Tests `escape_array_field_optslice_global.zer` (38z6wi),
  `escape_optslice_projected_global.zer` (3o10j6).
- **C2 — taint helper early-returns on `TYPE_OPTIONAL`** (`n0odo5` `2d6ec421`). `addr_of_is_local_derived`
  bailed on an optional carrier, so `?[*]u8 s = local[0..n]` left `s` UN-TAINTED and every downstream sink
  (`g = s` / `return s` / `&s[i]`) let the dangling stack slice escape. Upstream of ALL sinks —
  complementary to C1, not a duplicate. Unwrap the optional (mirrors `escape_type_carries_ref`); a
  `?u32`/`?bool` carrier unwraps to a non-slice inner and is correctly untouched.
  Test `escape_optional_slice_store_global.zer`.
- **C3 — return-field extraction doesn't unwrap `?T`/distinct** (`bz5q89` `ff38052a`). `return wrap(&x).p`
  with an optional/distinct/slice field escaped a local (stack-use-after-return); the plain-`*T` variant
  WAS caught. The sink gated on a raw pointer/slice type-kind check → widen to `type_carries_data_pointer`
  (matching the sibling direct-call return sink). Test `escape_return_field_optional.zer` + sink cell
  `p7__k_return_extract`.
- **C4 — inline `return @cast/@ptrcast(distinct-slice/ptr, local)`** (`rvek5f` `00f3c2af` HOLE-B). Plus the
  orelse-fallback form. TWO return-escape gates were pointer-only type-kind comparisons, excluding slice
  and distinct return types → both now use `type_can_carry_pointer` (unwraps distinct; covers
  slice/struct/union/opaque; still excludes a value `@bitcast`). The NON-inline form was already caught;
  param-view returns (BUG-764) still compile. Tests `return_cast_local_slice.zer`,
  `return_orelse_cast_local.zer`, `return_ptrcast_local_distinct.zer`.
- **C5 — `return @inttoptr(*u32, @ptrtoint(&local))`** (`i0txin` `a9ba4c77` HOLE 2). The intrinsic
  whitelist omitted `inttoptr`/`ptrtoint` and the pointer gate skipped a `?*T` optional return. Unwrap the
  chain of pointer-laundering intrinsics + accept an optional-wrapped pointer return type.
  Test `return_inttoptr_local_escape.zer`.
- **C6 — `keepfn(&loc.f)` / `keepfn(&arr[0])` handed DIRECTLY to a keep param** (`8rv51h` `e920ffb9`).
  The detector matched only a BARE `&ident`, and the local-derived walk never unwraps a leading `&`, so
  `&local.projection` slipped (the intermediate-variable form WAS caught via `is_local_derived`). Walk the
  `&`-operand's field/index chain to the root ident. Tests `keep_direct_local_{field,arrelem}.zer`,
  positive `keep_direct_global_field_ok.zer`, sink cells `p2/p3__k5_keep_direct` +
  `safe_keep_glob_{field,arr}_direct`.
- **C7 — Ring.push / keep-call of a call returning a pointer-carrying struct BY VALUE** (`rdh99l`
  `8af2573d`). Widen the NODE_CALL gate to `type_carries_data_pointer`.

### D. Spawn / concurrency (CRITICAL/HIGH) — **ALL 9 DONE 2026-08-01**

**LANDED (D1-D9).** All nine verified reproducing on main first. D3 is notable: the checker emitted
NO diagnostic at all — the latent cross-thread stack-UAF was masked only by an emitter limitation
(GCC rejected the slice assignment), so `zerc -o x.c` accepted it silently.

D1 routes spawn through the CENTRALISED `arg_is_local_derived` instead of re-deriving a weaker
answer, so spawn now agrees with every other escape sink (call-launder + struct-init literal).
D2/D3 were one edit: unwrap the optional carrier and add TYPE_ARRAY to the ptr-like dispatch.
D4/D6/D7 are the scoped-borrow cluster — threadlocal rejected, double-borrow rejected, and the
borrow record generalised from a single name to a LIST (types.h) so join() releases every borrow.
D5 closes the `&borrowed` call-arg launder the read-check deliberately skipped (`in_amp`).
D8 scans a function-NAME spawn arg at the spawn site. D9 rejects copying a whole shared struct by
value at var-decl (the call-arg form was already rejected).

Closes §G **G2** (D5) and **G4** (D4). G1 and G3 remain open.

Historical per-item detail retained below for provenance.


- **D1 — spawn of a by-value struct carrying `&local`** via a struct-init literal or a call-launder
  (`rdh99l` `8af2573d`). Cross-thread stack-UAF. Route spawn through the shared `arg_is_local_derived`
  and drop the holed struct-kind gate.
- **D2 — spawn `?*T` / `?[*]T` pointing at a stack local** (`i0txin` `a9ba4c77` HOLE 1). `is_ptr_like`
  tested `eff->kind` WITHOUT unwrapping the `TYPE_OPTIONAL` wrapper → the fire-and-forget cross-thread-UAF
  check never ran. Unwrap one optional level into `eff_pl` before the pointer/shared-carrier/struct-carrier
  dispatch. Test `spawn_optional_ptr_uaf.zer`. (Another instance of the `?T`-hides-inner-kind class.)
- **D3 — spawn bare local ARRAY arg at a `[*]T` param** (`n0odo5` `17ec74ca`). Arg-type `TYPE_ARRAY` was
  uncased, so the cross-thread stack-lifetime check never ran; only an emitter limitation masked the latent
  stack-UAF. Test `spawn_local_array_slice_race.zer`. Baselines one new already-unwrapped `_eff`
  type-dispatch site.
- **D4 — G4: `&threadlocal` to a SCOPED spawn** (`38z6wi` `d1ccb8aa`). The scoped-borrow setup loop skips
  threadlocals (they live in `global_scope`), so no borrow was established → main's concurrent write races,
  and the child writes the PARENT's TLS slot. Reject `&threadlocal` in the scoped arm (mirrors A5/BUG-757
  for fire-and-forget). By-value stays legal. TSan-confirmed. Closes §G **G4**.
- **D5 — G2: scoped-borrow laundered through a helper** (`38z6wi` `d1ccb8aa`). `poke(&x)` between
  `spawn worker(&x)` and `t.join()` compiled — the borrow read/write checks skip the `&`-take (`in_amp`)
  and never inspect call args. The NODE_CALL arg loop now rejects `&x` passed to a call while `x` is
  borrowed by a scoped spawn. Intra-function, cleared at `.join()`. TSan-confirmed. Closes §G **G2**.
- **D6 — scoped-spawn DOUBLE-borrow of the same `&local`** (`3o10j6` `d1a7d9cc` #3). The borrow flag was
  set unconditionally → worker-vs-worker race. Reject an already-borrowed local.
  Test `scoped_spawn_double_borrow.zer`, positive `scoped_spawn_sequential_borrow_ok.zer`.
- **D7 — scoped-spawn multi-arg borrow tracked only the FIRST `&ident`** (`3o10j6` `d1a7d9cc` #4) → race on
  the rest. The ThreadHandle now records ALL borrowed args (arena list on the Symbol, `types.h` change) and
  `join()` releases each. Test `scoped_spawn_multiarg_race.zer`, positive `scoped_spawn_multiarg_ok.zer`.
- **D8 — spawn funcptr ARGUMENT never scanned** (`7fxhb3` `31796ef8`). `spawn worker(bump)` where `worker`
  invokes its funcptr param: the concrete `bump` bound at the spawn SITE was never scanned (distinct from
  the BH-18 #8 local-funcptr facet), so a non-shared global RMW (or Pool/Slab/Ring metadata mutation) raced
  clean. Scan each function-name spawn arg's body at the spawn site (mirrors the BH-18 #8 conservative
  descent). Test `spawn_funcptr_arg_race.zer`, positive `spawn_funcptr_arg_safe_ok.zer`.
- **D9 — whole `shared struct` copied BY VALUE at var-decl / assign / return** (`rdh99l` `8af2573d`) clones
  its mutex + torn-read. Reject (mirrors the existing call-arg reject). Test
  `shared_struct_copy_by_value.zer`, positive `shared_struct_field_read_ok.zer`.

### E. ISR — **DONE 2026-08-02**

**Landed 2026-08-02.** E1 verified COMPILING on main before the fix; rejected after. The positive is
COMPILE-ONLY by necessity — gcc refuses an `interrupt` function on hosted x86-64 ("SSE instructions
aren't allowed in interrupt service routine"), and no existing `tests/zer` positive contains an
interrupt block. Verified by hand that the `volatile` form is still accepted, plus 0 regressions
across the 4 existing ISR positives.


- **E1 — ISR global reached through a LOCAL funcptr binding** (`3o10j6` `d576ae2a`, HIGH silent bare-metal).
  `*() fp = bump; fp();` inside an ISR bypassed the "must be declared volatile" and non-atomic-RMW checks —
  `record_isr_globals` follows only DIRECT calls into global functions, so a non-volatile global laundered
  through a funcptr let GCC -O2 hoist the access (silent hang / torn RMW). New
  `record_isr_funcname_binding` descends into a function bound to a funcptr at the var-decl/assignment
  binding site — the same mechanism `scan_funcname_binding` gives the spawn scanner (§C #11).
  **Complementary to main's shipped ISR-TRANS (§C #12), which closed only the direct-helper case.**
  Test `isr_funcptr_launder_volatile.zer` (promoted from `tests/zer_gaps/`).

### F. Defer — **ALL 4 DONE 2026-08-02** (F2 = sound interim reject; durable fix OPEN below)

**Landed 2026-08-02.** Every reproducer verified against main first:

| | Verified on main before the fix | After |
|---|---|---|
| F1 | `defer g.free_ptr(p); g.free_ptr(p);` COMPILED — double free at scope exit | rejected |
| F2 | exit **255** — the goto-skipped `rel()` fires, lock counter underflows | rejected (see below) |
| F3 | emitted C carried TWO `_zer_trap("compiler bug: unhandled NODE kind")` | clean checker error |
| F4 | gcc `redefinition of 'z'` — valid ZER failed to compile | compiles |

**F1's replacement is a per-defer INSTANCE ID, not a line number.** The old guard used
`free_line != defer_line` as a PROXY for "same defer body", which broke whenever the two frees shared
a physical line. `IRHandleInfo.freed_defer_id` is stamped on the freed handle and mirrored across its
alias group, written ONLY in the post-fixpoint scope-exit scan so no CFG-merge or snapshot handling
is needed. Id 0 is reserved for "not freed by a defer" (the memset-zeroed slot / an explicit free),
so the caller passes `k + 1`.

**F2's boundary was verified, and the positive is NEW.** The tracker warned that `xxhbdg`'s test set
has no positive for the legitimate idiom. `tests/zer/defer_before_goto_ok.zer` (written here) pins
three shapes that must keep compiling — defer BEFORE the goto, a goto with no defer after it, and a
defer after the label — and checks the lock counter stays BALANCED on both the goto and fall-through
paths. The reject is structurally unable to fire on defer-before-goto: the goto scan only inspects
statements at `gi < di`. Swept all 17 existing positives combining goto+defer: 0 regressions.

**Why an interim REJECT and not a ban-framework violation.** The Ban Decision Framework governs
banning VALID patterns. This shape currently MISCOMPILES — a silent lock underflow — so a compile
error naming the workaround is strictly better than the status quo, not a feature removal.

### Tech debt — the Makefile has NO header dependencies (found 2026-08-01)

`grep -cE '^\S+\.o:.*\.h' Makefile` returns **0**: no object rule names a header. So editing a
header does NOT rebuild the translation units that include it. This is benign for a
declaration-only change, but changing a STRUCT LAYOUT silently produces a MIXED-ABI binary — the
edited `.c` sees the new layout, every other stale `.o` sees the old one.

Hit while adding the D7 borrow list to `struct Symbol` in `types.h`: `make zerc` reported 0 errors
and then SEGFAULTED on a positive test. A `-O0` debug build of the same sources ran fine — the
classic layout-fragile signature. `rm -f *.o src/safety/*.o && make zerc` fixed it.

This generalises the documented stale-`.o` trap (CLAUDE.md blames an OOM-interrupted build); the
root cause is systemic, not accidental. **Until the Makefile grows header deps, ALWAYS
`rm -f *.o src/safety/*.o` after touching any `.h` — especially `types.h`/`ast.h`.** Proper fix:
`-MMD -MP` depfiles, or at minimum a blanket `$(OBJS): $(HEADERS)` rule.

---

## ~~branch survey 2026-08-10: 9 verified fixes~~ (ALL LANDED 2026-08-10, BUG-771..779)

**FULLY HARVESTED.** All 9 implemented and verified; `make check` breakage closed by P3.
Kept for the reproducers, the probe/masking warnings, and the branch attribution. The
"documented-but-NOT-fixed" list at the END of this entry is still OPEN — see there.

### (original entry follows)

## ~~branch survey 2026-08-06: 9 fixes + the view-alias family~~ (ALL LANDED 2026-08-06/08)

**FULLY HARVESTED — kept for the reproducers, the probe warnings, and the branch attribution.**
All 9 survey items landed (`e08a87d0` `efae5313` `43126a1c` `7c676758` `5748e904` `ec64526e`
`9ea6c864`), the view-alias family closed at **8 forms** (`5748e904` + `6f6d9186`, gated by
`tests/test_view_alias_matrix.c`, 24 cells), and the funcptr-bound-to-a-LOCAL spawn arg fixed
(`00dc785a`). Residuals that were NOT part of the 9 are listed under "Other documented-not-fixed
items" below and remain OPEN.

### (original entry follows)

**What this is.** Five `claude/gifted-noether-*` audit branches surveyed on 2026-08-06.
`dgbmqx` was fully harvested earlier; `8j6w19` had 3 of 5 landed (`bbe907d7`, `86a5b7fa`,
`20ef09cb`). `t20b31`, `2sjyjj` and `icejal` each forked from `20ef09cb` — current main at
survey time — so everything they carry is new. **Every item below was independently
reproduced on main before being listed**; nothing is repeated on the branch's word alone.
NOT merged or pulled — implement from this description.

**Two of the nine are regressions I introduced the same week** (#1 and #8), both marked.

---

### The 9 fixes to take

**#1 — an OPTIONAL wrapper defeats the spawn carrier gate (CRITICAL accept-unsafe).**
Found independently by all three branches. **This is a hole in the 2026-08-03 carrier fix
(`0e71b613`)**: `type_carries_handle` / `type_carries_nonshared_pointer` were added, but the
arms were gated behind a raw `eff->kind == TYPE_STRUCT || TYPE_UNION` pre-check, which an
optional wrapper skips — the `?T`-hides-the-inner-kind class, at the sink that fix existed to
close.

```zer
struct Msg{ *u32 p; }
void worker(?Msg om){ Msg m = om orelse { return; }; *m.p = 5; }
u32 f(){ u32 local = 0; Msg msg; msg.p = &local; ?Msg om = msg; spawn worker(om); return local; }
```
ACCEPTED; the bare `Msg` form is correctly rejected. Same for `?Box` carrying a `Handle`.

**PROBE WARNING — this one masks easily.** With a HEAP pointer plus `free`, the transfer rule
catches it and the gate looks fine. Only a STACK pointer (no free anywhere) isolates the gate.

*Three implementations.* `t20b31` drops the raw-kind pre-guard entirely and lets the recursive
predicate decide (cleanest, matches the class-kill philosophy); `2sjyjj` gates on a one-level
optional unwrap; `icejal` fully unwraps and gates on `type_dispatch_kind`. **Take t20b31's
shape plus icejal's `CAR_OPT_BYVAL` axis for `tests/test_conc_matrix.c`.**

**#2 — a VIEW wrapped in a CAST loses its heap alias (CRITICAL, ASan-proven).** `t20b31`.
```zer
*B p = @ptrcast(*B, &s[0]); free(s); return p.v;      // ASan heap-use-after-free
*opaque o = @ptrcast(*opaque, &s[0]); *B p = @ptrcast(*B, o); free(s); return p.v;   // also
```
Plain `&s[0]` is correctly rejected (fixed `86a5b7fa`), so the gap is precisely that the
cast family is not peeled. `t20b31` adds `ir_peel_cast_wrappers` (strips
`@ptrcast`/`@pun`/`@bitcast`/`@cast`/`@container`) at both view-alias sinks — the
projected-target assignment and the var-decl interior-pointer. Param-pointer casts still
compile. **See the FAMILY note below — do not fix this one in isolation.**

**#3 — VRP loop-body widening skips an `orelse` FALLBACK (CRITICAL, ASan-proven).** `2sjyjj`.
```zer
u32[8] a;
u32 main(){ u32 k = 0; u32 n = 0;
  while (n < 3) { a[k] = 7; none_val() orelse { k = 9; }; n += 1; }
  return 42; }
```
`vrp_invalidate_loop_body_writes` does not walk into an orelse fallback block, so the
loop-body write is invisible, `k` keeps its stale pre-loop `[0,0]`, VRP proves `a[k]` safe and
**elides the auto-guard**. ASan global-buffer-overflow.

**PROBE WARNING — two ways to mis-measure this, both hit during the survey:**
- The failing shape is a BARE `orelse` STATEMENT whose fallback block holds the write.
  `k = f() orelse 9` (assignment RHS) IS handled and will not reproduce.
- The guard here is the AUTO-GUARD (`if ((size_t)(k) >= 8u) { return 0; }`), not
  `_zer_bounds_check`. Grepping for the latter — and counting its preamble DEFINITION —
  reports guards where there are none. Compare against the control (`k = 9` written plainly),
  which emits the auto-guard and warns "index 'k' not proven in range".

Fix: add `NODE_ORELSE` (tried-expr + fallback) and `NODE_VAR_DECL` (init) to
`vrp_invalidate_loop_body_writes`; the sibling widener already recurses `NODE_ORELSE`.

**#4 — bit-slice WRITE at a runtime position >= width emits C UB.** `t20b31` (also `8j6w19`).
`reg[hi..lo] = 1` with `lo = 70` left `reg = 69` instead of an unchanged 5; UBSan: *"shift
exponent 70 is too large"*. The READ path is guarded
(`(_zer_lo0 >= 32) ? 0 : (reg >> _zer_lo0)`), the WRITE path is not (bare `<< _zer_bl1`).
`t20b31` clamps the position shift at BOTH emitter paths via `emit_bitslice_runtime_mask`.
**`8j6w19`'s own test is a WEAK ORACLE** — it exits 0 on the broken compiler at `-O2`; only
UBSan discriminates. Write a stronger test.

**#5 — bit-slice COMPOUND assign compiled as a plain assign (miscompile).** `icejal`.
`r[7..0] = 20; r[7..0] += 3;` yields 3, not 23. The IR-path bit-slice SET handler ignored
`node->assign.op` and stored the bare RHS, dropping both the current field and the operator.
All 10 compound ops broken — the register read-modify-write idiom bit-slices exist for.
Distinct bug from #4, same feature.

**#6 — `free([*]T)` misclassified as an unverifiable indirect call.** `t20b31` (also `8j6w19`).
Spurious *"calls through function pointer with unknown target"* on ~20 positives, **and a HARD
rejection under `--stack-limit`** (`error: entry 'main' call chain contains function pointer`)
— verified. The slice-free form keeps a bare `NODE_IDENT` callee; `t20b31` has `scan_frame`
consult the callee TYPE so only a real funcptr call is flagged (better than `8j6w19`'s
special-casing of the builtins).

**#7 — `@barrier_acq_rel` missing from `has_sync`.** `2sjyjj`. A spawn body fencing with it on
a non-shared global is hard-ERRORED instead of warned. Add it to the EXACT fence list — NOT a
`barrier` prefix match: `@barrier_init` / `@barrier_wait` are thread-rendezvous primitives,
not fences (`rt_conc_barrier_with_defer` catches the prefix attempt).

**#8 — `volatile *T` global falsely rejected (over-rejection). REGRESSION FROM `a3d8879f`.**
`2sjyjj`. `volatile_global_exempt_from_race_check` admits pointers to its scalar set, but
`type_width()` has **no `TYPE_POINTER` case** (verified: 0 hits) and returns 0, so the
`w > 0` test fails and a volatile pointer global is never exempt. Give a pointer its true
width in the exemption (or add the case to `type_width`).

**#9 — `container W(T) { Handle(T) h; }` fails with "undefined type 'T'".** `8j6w19`.
`subst_typenode` does not recurse into `handle.elem`. Handle is a `u64` so it monomorphizes
cleanly; Pool/Ring/Slab container fields stay rejected at the checker on purpose (the emitter
cannot emit the monomorphized struct yet).

---

### THE VIEW-ALIAS FAMILY — read this before fixing #2

Four MORE heap-UAFs were verified live during the survey, all documented-not-fixed on the
branches, and all the same semantic question (*"does a reference to this allocation reach
here?"*) asked at a different syntactic form. With the three already fixed (`86a5b7fa`), the
family has at least SEVEN members:

| form | status |
|---|---|
| `h.p = &s[0]` — projected target, view RHS | FIXED `86a5b7fa` |
| `h.p = first(s)` — through-call, 1 hop | FIXED `86a5b7fa` |
| `h.p = s[1..]` — subslice into a field | FIXED `86a5b7fa` |
| `p = @ptrcast(*B, &s[0])` — cast-wrapped view | FIXED `7c676758` |
| `H mk([*]B s){ h.p = &s[0]; return h; }` — by-value struct RETURN carrying a param-view field | FIXED 2026-08-06 |
| `outer(s) -> inner(s) -> &s[0]` — 2-hop through-call | FIXED 2026-08-06 |
| `*B get(H h){ return h.p; }` — return a FIELD of a by-value param | FIXED 2026-08-06 |

**ALL SEVEN CLOSED 2026-08-06**, and the class is now gated by
`tests/test_view_alias_matrix.c` (view-FORM x CARRIER, 21 cells, verified firing: 12/21 with
6 false negatives on a pre-fix build). An eighth form fails the build rather than becoming the
next commit.

**Recommendation: do NOT fix these one at a time.** Four separate patches to the same question
is another four rounds of the same whack-a-mole (see CLAUDE.md, "MULTI-SITE SAFETY IS THE #1
RECURRING BUG CLASS"). The durable shape is ONE view-provenance query — peel casts, peel
through-call summaries including by-value struct FIELDS and multi-hop — applied at every alias
sink, plus a grid crossing **view-form x carrier x sink** that would have caught all seven at
once.

### Other documented-not-fixed items, verified

- **funcptr bound to a LOCAL, passed as a spawn arg** (`icejal` E) — **FIXED 2026-08-06.**
  The scan looked the arg up in GLOBAL scope and required `is_function`, so a local binding
  fell through. The local's initializer is now resolved to the bound function, mirroring
  `scan_frame`'s indirect-call resolution. Only a directly-bound `= func_name` resolves —
  anything reassigned or computed stays unresolved, keeping this inside the per-file model.
- **`icejal` D-scoped (`?carrier` defeats the SCOPED free-before-join transfer)** — did NOT
  reproduce; the transfer rule catches it correctly. Listed here so nobody re-chases it.
- Not yet probed, lower severity: `&packed_field` forming a misaligned pointer (`2sjyjj`,
  HIGH, bare-metal, policy decision deferred); ISR global reached through a funcptr FIELD
  (`2sjyjj`, the ISR sibling of the spawn holes); `goto` INTO a `@critical`/`defer` block
  (`icejal` F1, LOW, GCC-backstopped); runtime over-width bit-field write truncating silently
  (`icejal` F2, LOW, design-arguable); `vrp_ir.c` `x & MASK` / `% N` missing a positive-mask
  guard (`2sjyjj`, LOW, latent dead code).

### Suggested order

`#1`, `#3`, `#8` first — two are regressions from this week, and `#3` is a silent OOB write.
Then the view-provenance unification covering `#2` and the three open family members together.
Then `#4`, `#5` (miscompiles), then `#6`, `#7`, `#9` (ergonomics).

---

## DONE (2026-07-15) — audit fixes across 12 parallel `claude/*` branches — TASK TRACKER COMPLETE

**ALL 41 unique fixes are now merged to main**, one verified fix at a time (2026-07-13 → 07-15),
each cherry-picked as the PROPER version, rebased onto HEAD, re-verified (build + neg/pos test(s) +
FOREGROUND make check + — for escape/free — the sink matrix). §A memory-safety #1–#7, §B escape
sinks #8–#13, §C VRP/bounds #13–#16, §D miscompiles #17–#25, §E concurrency #26–#31, §F crashes
#32–#35, §G bare-metal #36–#41 — ALL DONE. make check 984/0, sink matrix CLEAN (32 cells). The table
rows below are retained as a historical index (which source sha → which fix); the per-section **DONE** paragraphs record the applied form + regression test for each. Two low-risk follow-ups remain
tracked as their own OPEN entries (NOT part of the 41): the §E #28 `orelse-in-a-defer-body` loud-trap
(recommend a checker-side ban) and the §A #7 HOLE-A4 `Tok b = *p;` move-via-deref (needs an IR_UNOP
handler).

**Historical note (kept for provenance).** Twelve parallel `claude/*` audit sessions each
found + fixed overlapping soundness / miscompile / crash holes; NONE were merged to main
originally (verified 2026-07-13: `git cherry -v main <branch>` all `+`). The heavy overlap was
AMONG the branches (several bugs found 3–4×), NOT with main. **41 unique fixes** after
dedup — **11 landed (§D uN/iN + miscompiles #17–#25 fully done: uN/iN trio, `&&`/`||`
short-circuit, optional-None, designated-init, `@saturate`, signed-comptime, float-`_`;
+ §F crashes #32/#33/#34/#35, + §G bare-metal FULLY DONE #36–#41, + §A #1 subslice-alloc_id,
+ §A #3 free-non-heap-slice / §B #10 assign-slice-of-local escape, + §E #26 spawn-scan
wrapper-blind, + §A #2 cross-fn/by-value-field slice free, + §B #9 reassign-addr-of-local
escape, + §B #8 optional/array/nested-slice pointer-carrier — **SINK MATRIX CLEAN** —, + §B #12
Ring.push + §B #13 spawn-by-value + §B #11 arena-launder [**§B FULLY DONE**], + §C #15 cross-fn
VarRange leak + §C #16 defer bounds-guard + §C #14 find_return_range do-while/guard-body + §E #30 await resume-pred UAF + §E #28
defer-body shared lock + §E #31 union-tag-thru-pointer + §A #6 struct-copy compound-handle
+ §A #7 move-alias compound-key + §A #4 Level-B complementary-free-pair + §A #5 Level-B copy-chain immutability [**§A FULLY
DONE**] + §E #29 shared-cond unlock deadlock + §E #27 multi-root shared-lock [**§E FULLY DONE**]
+ §C #13 VRP branch/loop JOIN [**§C FULLY DONE**]), **ALL 41 FIXES MERGED — TRACKER COMPLETE.**
Every §A/§B/§C/§D/§E/§F/§G soundness/miscompile/crash/bare-metal hole found across the 12 `claude/*`
audit branches is now on main, each with its own regression test(s) and (for escape/free) a sink-matrix
cell. make check 984/0, sink matrix CLEAN.**

**Rules for consuming this:** (1) apply the PROPER version per bug (table below), not a
whole branch; (2) cherry-pick/rebase onto current HEAD, then re-verify — each was green on
its OWN fork base, not current main (esp. the uN/iN ones — this session changed that exact
code); (3) fixes pile onto the same functions across branches → apply per-FAMILY, re-verify
after each (conflict groups noted at the end); (4) drop junk commit `e4829572` (0-source
binary regen). To inspect any fix: `git show <sha>`.

### Source branches (fork base → commits)
| Branch | Base | Commits (short) |
|---|---|---|
| gifted-noether-k7l625 | b3b9f18a | 87a01415, ea58e5cc |
| gifted-noether-jfrmer | 72e74913 | 5a6889df |
| gifted-noether-a47dg2 | abdf629e | 9edc49b8 |
| gifted-noether-9rryue | 3d6d2704 | bf29ffdc |
| gifted-noether-5ergto | abdf629e | 85cc109e, a604ac57, 8d9514f3 |
| cool-johnson-53cbd5 | 67a53c56 | 582920db (+e4829572 junk) |
| cool-johnson-baujiz | e3fe5d46 | fb8091d6, 19471462, 2c7645b9 |
| nifty-gates-84coh3 | 54ecfc9e | 59a968cb |
| nifty-gates-jkaz5c | 3d6d2704 | 66332d39, 1fdaaffe |
| nifty-gates-m0v91c | 54ecfc9e | a3e1f66c |
| nifty-gates-ubjj9o | e3fe5d46 | f40ca06b, fb3315f2 |
| nifty-gates-ziwscu | 54ecfc9e | 586507fb, a8968db0, ce9af8cb (+56497f28 doc) |

### A. Memory safety — UAF / double-free / move (CRITICAL; absent in main)
**DONE: #1 subslice of a heap slice inherits the base `alloc_id` — view UAF/double-free
now caught (var-decl + assign forms; walk RHS to root IDENT, alias iff `alloc_id != 0` so
param/stack subslice untouched); `8d9514f3`, tests `subslice_{uaf,double_free,alive_ok}`,
per-sink-matrix verified (param-subslice + alive-subslice compile; base-direct UAF still
caught); 2026-07-15. make check 936/0. #3 reject `free()` of a non-heap slice
(stack-array/arena/local-derived) — raw libc `free()` on stack/arena memory was UB
(`-Wfree-nonheap-object`); `bf29ffdc`, relies on §B #10's local-derived marking, tests
`free_nonheap_slice_{direct,stack}`, sink cell `p5__k6_free` closed; 2026-07-15. make check
942/0. #2 cross-fn / by-value-field heap-slice free tracked — `frees_param` gate excluded
TYPE_SLICE, so a callee freeing a heap-slice param (`sink([*]B p){free(p)}`) or a slice field
of a by-value struct param (`fb(H h){free(h.buckets)}`) wasn't recorded → caller
double-free/UAF passed; `bf29ffdc` (776), tests `alloc_crossfn_slice_double_free` +
`alloc_byval_field_slice_uaf` + positive `slice_param_free_ownership_ok`; 2026-07-15. make
check 948/0. **All applicable `bf29ffdc` fixes now landed (§A #1/#2/#3, §B #10, §E #26).** #6
struct value-copy `Holder b = a` dropped a's COMPOUND handle rows → `free(raw); use(b.p)`
compiled a UAF (b.p untracked). Regression of the documented Pattern-4 two-pass replicate — the
src's tracked field is a compound entry (`path != NULL`), so `ir_find_handle` returns NULL and
the `!src_h` early-break skipped it. Restored the two-pass replicate (snapshot then add+alias,
realloc-safe) BEFORE the break; `59a968cb` A3, tests `struct_copy_compound_uaf` (neg) +
`struct_copy_compound_ok` (pos, no false-leak); 2026-07-15. make check 972/0. #7 move-alias via a
COMPOUND `&arr[i]`/`&b.field` — `*Tok p = &arr[0]; Tok b = arr[0]; use(p)` was a use-after-move
that compiled (bh18_1b registered the pointer↔move alias only for bare `&ident`, not compound
keys). Added the compound-key alias-registration block (register/alias the pointer against the
COMPOUND handle, gate the ident path on `!used_compound`); `582920db` #4. NOTE: that commit's 6
scattered `ir_propagate_alias_state` calls were SUPERSEDED by the unified `ir_mark_transferred`
sink (spawn-arg A3 already caught in main — verified empirically); only the compound A1/A2 were
holes. Tests `move_alias_{compound,field,spawn}_uaf`; 2026-07-15. make check 975/0. **(HOLE-A4
`Tok b = *p;` move-via-deref still deferred — needs a new IR_UNOP handler.)** #4 Level-B guarded-free
relaxation admitted a UAF/double-free after a COMPLEMENTARY free-pair (`if(c){free} if(!c){free}`
frees on ALL paths, but the single `free_block` recorded one free, so a later `if(!c){use/free}`
was wrongly judged disjoint from the *other* free and accepted). One-line gate `if
(h->freed_all_paths) return false;` at the top of `ir_use_guard_disjoint` (freed_all_paths set by
`ir_free_completes_coverage`, propagates monotonically; legit single-free-then-disjoint-use
preserved — provably transparent). `59a968cb` A1, tests `guarded_complement_{free_use,double_free}`;
2026-07-15. make check 977/0. #5 Level-B guard COPY-CHAIN defeated the immutability gate — a
reassigned INTERMEDIATE copy (`bool c2 = c; if(c){free} c2 = e; if(!c2){use}`) hides in an
expr-form IR_ASSIGN (dest_local == -1) the def-scan can't see, so `ir_resolve_cond_root` traced
c2→c and checked only c's immutability → the disjointness was a lie → silent UAF + double-free.
Gated each COPY/UNOP hop on the INTERMEDIATE's immutability (`ir_local_is_immutable_bool` +
forward decl); a STABLE copy-chain still compiles (relaxation preserved — proven by positive).
`66332d39` #2, tests `guarded_cond_copychain_reassigned` (neg) + `guarded_cond_copychain_stable_ok`
(pos); 2026-07-15. make check 979/0. **§A (memory safety #1–#7) FULLY DONE.**

### B. Escape / dangling-pointer sinks (CRITICAL; #8 base helper partial in main)
**DONE: #10 assignment-form slice-of-local (`[*]T s; s = arr; return s` / `g=s` / `&s[0]`)
escaped a dangling stack slice — the NODE_ASSIGN taint was gated on FIELD/INDEX targets,
excluding whole-ident slice targets; extracted the var-decl walk into a shared
`mark_slice_local_derived_from_value` helper used by BOTH sinks (they can no longer drift);
`bf29ffdc` (773), tests `{global,return}_reassigned_slice_local` + `return_reassigned_slice_ptr`
+ positive `slice_reassign_heap_ok`; 2026-07-15. make check 942/0. #9 reassignment `p =
&local[i]` / `p = &local.f` escaped the frame — the NODE_ASSIGN escape walker matched only a
bare `&local` (NODE_IDENT), skipping the field/index chain; extracted `addr_of_is_local_derived`
(with the pointer-into-nonlocal-ref relaxation preserved) shared by BOTH var-decl + assignment
sinks; `66332d39` (#1), tests `reassign_addr_local_{index_return,field_global}` + positive
`reassign_addr_param_slice_ok`, sink cells `p2/p3__k7_reassign` added+closed; 2026-07-15. make
check 951/0. #8 optional/array/nested-slice pointer-carrier escape — shared
`escape_type_carries_ref` (pointer|slice|optional-of-those) replaces the enumerated
{POINTER,SLICE} gates at the nested-call recursion (F1) + both NODE_ASSIGN field-descend sinks
+ the var-decl call-result sink (F3), + F3 two-step field-read propagation, +
`type_can_carry_pointer` TYPE_ARRAY (D1, 586507fb) and TYPE_OPTIONAL (sink-matrix-driven) arms
(both via the precise `type_carries_data_pointer`, so `?u32`/scalar arrays stay excluded);
`5a6889df` F1/F3 + `586507fb` D1, tests `escape_{nested_slice,optional_ptr_field}_launder` +
`array_field_launder_escape` + positive `optional_ptr_field_global_ok`; 2026-07-15. make check
955/0. **SINK MATRIX CLEAN — every escape/free hole closed.** #12 Ring.push of a
local-derived pointer element — a Ring is always global, so `rx.push(m)` where `m.p = &local`
dangles once the frame returns; new `container_push_arg_escapes` rejects at `push`/`push_checked`
(gated on `type_carries_data_pointer` so a pure-value element compiles); `a3e1f66c`, matrix
cell `p8__k8_ring_push` + baseline `safe_ring_value` (matrix now **27 ok / 0 holes**); tests
`ring_push_local_escape` + `ring_push_value_ok`; 2026-07-15. make check 957/0. #13 (§B,
re-scoped from #12) spawn of a by-value struct/union carrying a ptr-to-local — `Msg m;
m.p=&local; spawn worker(m)` compiled. ROOT CAUSE (diagnostic-confirmed): `m` IS correctly
marked local-derived (the whole-struct store-to-global sink rejects `g=m`); the bug was the
spawn CONSUMPTION side — `spawn_arg_is_stack_derived` was called ONLY inside `if (is_ptr_like)`
(pointer/slice/opaque), so a by-value struct arg was never checked. (The earlier "parent not
tainted" hypothesis was WRONG — store-side taint works fine.) Fix: else-branch on the
is_ptr_like block for local-derived struct/union args (`eff->kind == TYPE_STRUCT/UNION`
baselined); pure-value struct not over-rejected; scoped spawns exempt. Matrix cell
`p9__k9_spawn_val` + `safe_spawn_value` (matrix now **29 ok / 0 holes**); tests
`spawn_value_struct_local_escape` + `spawn_value_struct_pure_ok`; 2026-07-15. make check 959/0.
**§B #12 COMPLETE (Ring.push + spawn-by-value both closed).** #11 arena-over-local pointer
laundered to a global/param — `arg_is_local_derived` was blind to `is_arena_derived` (call-launder
`g=identity(arena_ptr)`; +5 sites, `85cc109e` D) AND the direct-call form `g=arena.alloc_slice(...)`
only TAINTED, never rejected (now `classify_escape_sink` → reject global/param; `87a01415` #4
ESCAPE #1; the ESCAPE #2 half `p=&local[i]; g=p` was already closed by §B #9); matrix shape p10
(`p10__k10_arena_call/direct`) + `safe_arena_local` (matrix now **32 ok / 0 holes**); tests
`escape_arena_{launder_global,direct_global,direct_param_field}` + positive
`escape_arena_launder_local_ok`; 2026-07-15. make check 963/0.
**§B (escape / dangling-pointer sinks #8–#13) FULLY DONE.**

### C. VRP / bounds — silent OOB (CRITICAL; absent)
**DONE: #15 VarRange map leaked across functions (name-keyed, never reset) → a stale range
from an earlier function elided a later same-named function's bounds guard (silent OOB);
`c->var_range_count = 0` at each `check_func_body` entry (NOT restored — the post-body
find_return_range pass reads it, next entry re-clears); `f40ca06b` F3, test
`vrp_crossfunc_range_no_leak` (runtime, fixed=exit 0); 2026-07-15. make check 964/0. #16
fixed-array index in a defer body dropped its bounds guard → silent OOB (defer bodies are
raw-AST emitted, never reached the IR auto-guard pre-pass); new `Emitter.guard_traps` makes the
auto-guard TRAP (an early-return in a defer re-fires the stack + skips cleanup), wired at the 3
indexable defer-body sites (expr/return/if-cond); `9edc49b8` E, test `defer_array_oob`
(compile-clean + runtime-trap); 2026-07-15. make check GREEN. #14 `find_return_range` mis-credited
do-while + guard-body returns → too-narrow return range → caller elided its bounds check (silent
OOB): F1 added NODE_DO_WHILE to the loop case (a do-while-body `return` escaped the scan;
`f40ca06b`); A2 threaded an `in_branch` param so a bare `return param` narrows ONLY at top level
(a guard-BODY return was wrongly credited the guard's inverse range; `59a968cb`) — sound
top-level narrowing kept. Tests `dowhile_return_range_oob` (trap) + `return_range_guard_body_bounds`
(exit 0); 2026-07-15. make check 967/0. #13 VRP branch-assign + loop-body range narrowing leaked
past the join → elided bounds guard (silent OOB): Finding A — an assignment inside an if-branch
(`if(mode==0){x=1}`) mutates x's VarRange IN PLACE, which the count-only restore can't undo, so the
branch-local [1,1] leaked past the join and `arr[x]` elided its guard (OOB at x=2e9); snapshot
pre-branch VALUES + JOIN each branch's result at the merge (new `vrp_snap_take/restore/join`, on both
comparison + non-comparison paths). Finding B — the loop-body widen pre-pass delegated to
`vrp_invalidate_for_assign` which NARROWS a literal rhs (`i=0`→[0,0]), so `arr[i]` before `i=0` was
proven against [0,0] not the carried value (OOB at i=2e9); new `vrp_join_assign_range` UNIONS
(widens, never narrows). Monotone → can only ADD guards → sound. `586507fb` A+B, tests
`vrp_branch_assign_guard_ok` + `vrp_loop_assign_guard_ok` (both exit 0, guard emitted); 2026-07-15.
make check 984/0. **§C (VRP / bounds silent-OOB #13–#16) FULLY DONE.**

### D. uN/iN + miscompiles (HIGH)
**DONE (merged to main): #17 assign/compound-assign mask, #18 `@truncate` mask
(inline+store), #19 bit-slice-read 64-bit guarded mask, #20 `&&`/`||` short-circuit
(all 2026-07-13), #21 optional bare/orelse return → None (2026-07-14).**
uN/iN sources: k7l625 (`87a01415` helpers `emit_intn_mask_lv`/`type_is_nonnative_intn` +
assign intercept both emit paths; `ea58e5cc` truncate) + jfrmer F8 (`5a6889df`, bitslice
read). k7l625's assign approach was chosen over jfrmer's F5 because it single-eval-masks a
side-effecting index target (`arr[f()] += n`) whereas jfrmer's bails on side effects. #20
short-circuit: ubjj9o `f40ca06b` (`lower_shortcircuit_to_dest`, IR branch-lowering; chosen
over a47dg2 `9edc49b8` passthrough which would hide the control flow from IR analysis).
#21 optional-None: k7l625 `87a01415` (#1+#2) — `?T` bare return `{0,0}` not `{0,1}`;
`?void` orelse propagates failure via new `IRInst.ret_from_orelse` (chosen over a47dg2
`9edc49b8` BUG-A which fixed only the `?T` case).
Tests: `tests/zer/{intn_assign_mask,intn_truncate_inline,bitslice_read_wide,shortcircuit_side_effects,shortcircuit_value_ok,optional_bare_return_none,optional_void_orelse_propagate}.zer`.
make check 919/0. #22 (2026-07-14): value-optional struct-field designated-init dropped
the value (`{.baud=9600}` on a `?u32` field → `has_value=0`, so `orelse` took the
fallback); fb3315f2's shared `struct_init_opt_wrap_type` wraps the scalar `{val,1}` on all
3 struct-init emission paths (test `optional_field_designated_init`; make check 921/0).
**#23/#24/#25 (2026-07-14):** `@saturate` unsigned from a ≥2^63 source returned 0 (the
`(int64_t)` clamp misread it — now compared in the source's own type; a3e1f66c);
signed comptime return not sign-extended → comptime-if compiled the wrong branch
(checker.c, a3e1f66c); float literal digit-group `_` truncated at `strtod` — strip before
parsing (parser.c, f40ca06b F8). Tests
`saturate_unsigned_large`/`comptime_signed_return`/`float_underscore_literal`. make check
924/0. **§D fully done.**

### E. Concurrency (CRITICAL/HIGH; absent)
**DONE: #26 spawn data-race scanner was wrapper-blind — `scan_unsafe_global_access`
treated NODE_TYPECAST/SLICE/STRUCT_INIT as non-recursing leaves and dropped the orelse
`.fallback`, so a worker reading a non-shared global via `(u32)g` / `g_arr[a..b]` / `P{.x=g}`
/ `maybe() orelse g` raced clean; gave them recursing cases (switch stays exhaustive) + scan
both orelse sides; `bf29ffdc` (772), tests `spawn_race_{cast,orelse_fallback,struct_init}`;
2026-07-15. make check 945/0. #30 UAF across an await (orelse cond) undetected — `IR_AWAIT` was
in `ir_compute_preds` `default`, dropping the resume predecessor edge when an orelse in the await
cond inserts intermediate blocks, so zercheck_ir lost handle state across the suspend (silent
UAF: `free(h); await (g orelse 0)==7; get(h)`); added `case IR_AWAIT:` to the IR_YIELD resume-edge
case (`dfs_reachable`/`cfg_reaches_fire` already group them); `586507fb` (1-line), test
`await_orelse_cond_uaf`; 2026-07-15. make check 968/0. #28 shared read inside a defer body emitted
UNLOCKED → silent data race: `emit_defer_shared_root` missed NODE_INTRINSIC/ORELSE/SLICE/STRUCT_INIT
AND `emit_defer_stmt`'s NODE_VAR_DECL never called the walker (so even `defer { u32 z = g.v; }` had
no lock); added the 4 node kinds (condvar/barrier/once self-lock skipped) + route var-decl init
through the walker + rdlock; `2c7645b9`, test `defer_shared_intrinsic_lock` (positive, 6
mutex_lock in emitted C); 2026-07-15. make check 969/0. #31 union variant write through a
`*Union` pointer skipped the `_tag` update → caller's `switch(u)` read the WRONG arm (union type
confusion, silent for non-ptr variants): the tag-update emission's walk-up only matched a union
VALUE object, not a POINTER-to-union; now matches the pointer case (`obj_is_ptr`), unwraps
`obj_type` through the pointer, and hoists the pointer directly (no `&`) so `_zer_up->_tag`
writes through it; `586507fb`, test `union_ptr_write_tag_ok` (3 `_eff->kind` reads baselined);
2026-07-15. make check 970/0. #29 shared-cond mutex LEAKED on an `orelse return/break` inside a
condition → deadlock (`if ((g.v orelse return) == 1){…}` on a shared struct took the early exit
without releasing the cond mutex). Added `LowerCtx.cond_shared_saved`: `emit_shared_lock_around_cond`
points `current_stmt_shared_root` at the cond root (so the in-cond orelse-exit emits the unlock),
`emit_shared_unlock_after_cond` restores it (after the `!root` early-out). `586507fb` C-F3, test
`shared_cond_orelse_unlock_ok` (runs exit 0, no deadlock); 2026-07-15. make check 980/0. #27
multi-root shared lock (`find_all_shared_roots_expr`, the SECONDARY-lock walker) blind to two
forms → a second shared read emitted UNLOCKED (silent race): C-F4 field-projection — `wa.sp.v`
where `wa.sp` is `*shared S` (root walked past it) now checks the OBJECT's type at EACH
projection step + locks the outermost shared sub-expr; B1 wrapper forms — the walker recursed
TYPECAST/SLICE but not NODE_INTRINSIC/ORELSE/STRUCT_INIT (`ga.v + @truncate(u32, gb.v)` etc.),
added the 3 cases (condvar/barrier/once self-lock). `586507fb` C-F4 + `19471462` B1, tests
`shared_multi_field_ptr_lock_ok` + `shared_rw_multi_lock_intrinsic`; 2026-07-15. make check 982/0.
**§E (concurrency #26–#31) FULLY DONE.**

**OPEN (from §E #28, low-risk — traps LOUDLY, not silent):** `defer { u32 z = maybe() orelse
g.v; }` hits a separate emitter gap — `emit_rewritten_node` has no NODE_ORELSE handler in defer
bodies (they bypass IR lowering), so it emits a runtime compiler-bug trap rather than valid C.
Recommended fix: checker-side reject `orelse in a defer body` (same class as the existing
return/break/continue/goto-in-defer bans). Not a soundness hole (loud, not silent).

### F. Parser / crashes / robustness
**DONE: #33 `type_name` buffer overflow → SIGSEGV (`59a968cb` A5, clamping `tn_append`;
UINT/SINT cases adapted for current main); #34 `(*ptr & mask)` parse regression (`ce9af8cb`
A7-12, speculation + `token_can_start_unary`; chosen over `66332d39` #5 whose simpler
heuristic misparses `(*ptr) + 3`) — all 8 QEMU examples parse again (2026-07-14); #35 defer
+ auto-guarded index compiler abort — `emit_defers_from` now fires the pending IR defers
via `cur_ir_func` instead of `abort()`ing (`66332d39` #6, chosen over `a3e1f66c`'s per-site
`emit_pending_ir_defers`; 2026-07-15). Tests `paren_deref_expr`, `defer_autoguard_earlyexit`.
make check 925/0. #32 parser stack-overflow DoS — `parse_type` + `parse_unary` wrapped in a
shared `p->depth` guard (limit 256); deep `****…`/`----…`/`Box(Box(…))` now report "nesting
too deep" instead of SIGSEGV (`a8968db0` A7-13; main already guarded `parse_primary`); tests
`parser_deep_{type,unary}_recursion`; 2026-07-15. make check 927/0. **§F fully done.**

### G. Bare-metal / ISR / qualifier
**DONE (2026-07-15): #36 `@critical` `"memory"` clobber on all 6 non-x86 arms (`a8968db0`
A7-6); #37 baremetal `@cpu_syscall/sysret/iret/hypercall` `#else #error` (were silent no-ops
on non-x86/ARM64/RISC-V targets; `582920db` #5, verified in emitted C); #41 `@container`
const-strip check (last cast form missing the BUG-304-family const check; `582920db` #2, test
`container_const_strip`). #39 ISR ban on BLOCKING sync (`@cond_wait`/`@cond_timedwait`/
`@barrier_wait`/`@sem_acquire`) — non-blocking wakes (`@cond_signal`/`@cond_broadcast`/
`@sem_release`) stay allowed (`1fdaaffe`, test `isr_cond_wait`; 2026-07-15). #40 ISR/@critical ban on the universal `alloc(T,n)`/`free(slice)`
DIRECT paths (emit calloc/free inline, never desugar to a banned method; `66332d39` #3, tests
`universal_{alloc,free}_slice_in_{critical,isr}`). #38 `@inttoptr` aggregate span/alignment
used `type_width` (=0 for aggregates) → `compute_type_size` (const span) + `type_alignment_bytes`
+ C `sizeof(target)` span (both emit paths); a 16-byte struct over an 8-byte mmio range now
overflows the range check (`5a6889df` F4, test `mmio_struct_range_overflow`). make check 933/0.
**§G FULLY DONE.**

### Conflict groups (apply per-family, re-verify after each)
- checker.c escape sinks: #8, 9, 10, 11, 12 (same region)
- zercheck_ir.c Level-B: #4, 5 (same free_block/guard machinery)
- ir_lower.c shared-lock: #27, 29 (cond-lock helpers)
- emitter.c uN/iN: #17, 18, 19 (same mask sites this session touched)
- emitter.c defer: #16, 35 (`emit_defer_stmt` / pending-defer)
- checker.c VRP: #13, 14, 15 (var_range save/restore + return-range)

**Next up (start here):** DONE: §D/§F/§G FULLY DONE; §A #1 (subslice-alloc_id) + §A #3 / §B #10
(slice escape/free from `bf29ffdc`) DONE 2026-07-15. Remaining memory-safety = §A #2 / #4–#7,
§B #8/#9/#11/#12, §C VRP/bounds (#13–#16), §E concurrency (#26–#31). **Recommended order (§E #26 + §A #2 + §B #9 + §B #8 DONE 2026-07-15 → SINK MATRIX CLEAN):**
the remaining fixes are NO LONGER sink-matrix cells (the matrix is clean), so order by risk:
→ §B #11/#12 (arena/Ring-spawn launder — escape sinks, same file, add a matrix cell each) →
§A #4–#7 (Level-B guard / struct-copy / move-alias — zercheck_ir.c) → §C VRP/bounds (#13–#16,
checker.c/emitter.c) → §E #27–#31 (concurrency — ir_lower.c/emitter.c). Each still verified
against the sink matrix (must stay CLEAN) + its own neg/pos + make check.
**Extra care** — these land in the same `zercheck_ir.c`/`checker.c` regions (conflict-group
siblings); a mistake in §A/§B ships a UAF, not an over-reject. Verify EACH against the full
sink matrix (`bash tools/sink_matrix.sh ./zerc`) per CLAUDE.md "Escape/keep analysis is a
PER-SINK PATCHWORK" — a fix must flip its own cell(s) and regress none. Note some overlaps
with un-taken halves already shipped (`66332d39` #4 frame-local free = §A ..; `5a6889df` F1/F3
= §B #8; `582920db` #1/#3/#4 baseline lines already pre-added; `bf29ffdc` BUG-775 = §A #1
already shipped). All verified absent from main.

### Per-sink verification matrix (`tools/sink_matrix.sh`) — RUN AFTER EVERY §A/§B/§C fix
`bash tools/sink_matrix.sh ./zerc` runs the {value-shape × escape/free-sink} grid (32 cells)
and classifies each: **ok** / **HOLE** (compiles but should reject = a shipped UAF/dangling
escape) / **OVER-REJECT** (rejects a safe program). This is the regression baseline for the
memory-safety cluster: escape/free analysis is a per-sink patchwork, so a fix must flip its
own cell(s) to ok AND leave every other cell unchanged. **CLEAN 2026-07-15: 32 ok, 0 HOLES,
0 over-rejects** (started this session at 17 ok / 6 HOLES @ 23 cells; grew to 32 as fixes added
cells). ALL escape/free holes closed: `p5__k6_free` (§A #3), `p2/p3__k7_reassign` (§B #9),
the 3 `p7×` optional-field-carrier + `p2/p3__k2v_2step` IDENT two-step (§B #8), `p8__k8_ring_push`
(§B #12 Ring), `p9__k9_spawn_val` (§B #12 spawn), `p10__k10_arena_call/direct` (§B #11 arena).
**§B escape sinks FULLY DONE.** **WIRED into `make check`**
as a permanent gate 2026-07-15 (`make check` runs `tools/sink_matrix.sh ./zerc` after the
build audits; standalone: `make check-sink-matrix`). **Add a new cell whenever a new
escape/free sink or value shape is introduced** — the gate then enforces it forever; a fix
that regresses any cell fails `make check`.

---

## DONE (2026-07-01) — BRANCH-IMPORT LANDED (9 fixes / 13 holes) — residual flags + open items below

**STATUS: all three tiers committed + `make check` GREEN, ZER 873/0.** Tier 1 `6c368761`
(6 holes: defer-goto, funcptr-race, intrinsic-arity, typedef-ptr-UAF, assign-launder, switch-
capture). Tier 2 `4cf2c479` (2 AST→IR drift holes: static-init `@ctz`, await/spawn auto-guard).
Tier 3 `1098202f` (field-projection blindness in 5 shared-type walkers). Permanent record:
BUGS-FIXED.md 2026-07-01 (three entries). The tier detail below is retained as HISTORY; the
two surviving **FLAGS** and the **STILL OPEN** list at the end of this entry are the live parts
— do not delete those when this history block is eventually pruned. (FLAG #3 was retracted: all
hunks applied cleanly onto the rewritten walkers.)

**What this was.** A review of six sibling bug-fix branches (`claude/cool-johnson-{sesjma,
a5erj3, 11ct36, anb3cw, anqp95, ongou2}`) produced a prioritized backlog of fixes that are
NOT yet in main. The code is ALREADY WRITTEN on those branches — this is a COPY-the-fix
(re-derive onto current main), **NOT a merge/pull** (the branches pre-date main's recent
walker rewrites and would conflict/revert them). This entry is the full handoff so a fresh
session can execute it with zero prior context. Each fix's source commit is named; inspect it
with `git show <sha> -- <file>` (fetch the branch first: `git fetch origin
'refs/heads/claude/cool-johnson-<name>:refs/remotes/origin/claude/cool-johnson-<name>'`).
Verify each tier with the Docker pattern (CLAUDE.md "Ad-hoc Docker verify"). Remove a row's
line + delete this whole entry once all tiers land + `make check` GREEN.

**CORRECTION (2026-07-01, verified):** sesjma/a5erj3/ongou2 all branch from `9ad13c0c`, which
ALREADY CONTAINS the walker rewrites (22061071/dafbc1f6/28e9562e/64ea3da2 are its ancestors);
11ct36 branches from `fcd2dc34`. **Every fix's source hunks were verified to apply CLEANLY to
current main via `git show <sha> -- <files> | git apply --check`** (all 7 commits CLEAN,
including the Tier-3 field-projection set and the older-base 11ct36). So the earlier Tier-3
"OVERLAPS main's rewrites / RE-DERIVE / FLAG #3" concern was based on a WRONG base assumption
and is RETRACTED — the a5erj3 field-projection fixes were written ON TOP of the rewritten
walkers and apply directly. Implementation is a faithful `git apply` of each source diff +
`git checkout <sha> -- <testfile>` for tripwires (NOT a merge). The "unify into one helper"
idea for the 5 walkers is OPTIONAL polish, deliberately deferred (the user asked to copy the
fixes, not introduce a new refactor mid-import). FLAG #1 and FLAG #2 stand; FLAG #3 retracted.

**Already in main — DO NOT re-take:** anb3cw `b6773a3d` (BUG-770/771) = main batch 7
`28a7455c`; anqp95 `e09da736` 4 fixes (slice `.ptr` volatile-strip, `@truncate(NonInt)`,
`_zer_trap` x86 sentinel, `@cpu_*` unknown-arch) = main batch 3 `9a94dad4`. **Ignore binary
regens** (`49f77bba`, `87d2e360`, `7a8feae1` — rebuilt test ELFs, 0 source lines).

**No theorem/oracle bugs anywhere** — every fix is checker/IR/emitter coverage; O2 is
*certified by* `capture_lattice.v`. Counting each walker site, 13 distinct holes → 9
root-cause fixes. All are PURE TIGHTENING (accept-unsafe → reject, or miscompile → correct);
none widen acceptance, so a mistake over-rejects (safe), EXCEPT none here touch a relaxation.

### TIER 1 — clean, no structural conflict with main. Do FIRST. (6 holes)

- **[T1.1] sesjma `31cfe9da` — defer + forward-goto fall-through silently drops the defer.**
  MEDIUM silent Pool-slot LEAK every call. File: `ir_lower.c`. CAUSE: `NODE_GOTO` eagerly fires
  the defer and zeroes `ctx->defer_count`; `NODE_LABEL`'s guard-install gate
  (`live_fallthrough && defer_count>0`) then sees 0 and skips → fall-through emits no fire;
  AND the `live_fallthrough` check `(inst_count>0) && !is_terminated` excludes empty-but-
  reachable join blocks (a no-else `if`'s `bb_join`). FIX: add `goto_fired_count` to
  `IRLabelMap`; `NODE_GOTO` records the pre-fire count on the target label (MAX across gotos);
  `NODE_LABEL` restores `ctx->defer_count = max(current, goto_fired_count)` on a live
  fallthrough; fix the `live_fallthrough` test to include empty reachable join blocks. Sibling
  of the 2026-06-20 defer-inside-if fix (this completes the family: function-scope defer
  outside any if-body). Copy the branch's tripwire test. CLASS: control-flow lowering /
  missing-site.

- **[T1.2] 11ct36 `ecd6f65d` — BH-18 #8: spawn data-race scan blind to funcptr forwarding.**
  CRITICAL data race. File: `checker.c`, `scan_unsafe_global_access` NODE_CALL handler. CAUSE:
  `worker(){ run_n(do_increment, n); }` + `spawn worker()` raced a global via the indirect
  call `cb()` inside `run_n`; the scan descended the direct callee but not funcptr args. FIX:
  follow every `NODE_IDENT` argument that resolves to a function symbol, descending into its
  body the same way as the direct callee; single shared `_scan_depth` counter (cap 32).
  Tripwire `tests/zer_fail/spawn_funcptr_global_race.zer`. CLASS: form-coverage / missing-site.

- **[T1.3] 11ct36 `ecd6f65d` — BH-18 #14: `@size()`/`@bitcast()` with no type arg → invalid C.**
  MEDIUM invalid C. File: `checker.c`. CAUSE: the arity `type_arg` gate let the zero-type case
  through. FIX: restructure the arity block — make family identification unconditional, then
  SPLIT "requires a type argument" from "expects N args after type"; preserve the
  `@size(NamedType)` parse path (BUG-316) via `size_named_path`. Tripwires
  `tests/zer_fail/intrinsic_{no_type_arg,bitcast_no_type}.zer`. CLASS: arity / missing-site.

- **[T1.4] a5erj3 `c2eb1652` — typedef-wrapped pointer destructor blinds FuncSummary → silent
  UAF + double-free.** CRITICAL UAF. File: `zercheck_ir.c`, FuncSummary builder. CAUSE: gated
  param-FREED observation on the SYNTACTIC `TypeNode` kind (`tnode->kind == TYNODE_HANDLE ||
  TYNODE_POINTER`); a `typedef *T TPtr` param is `TYNODE_NAMED`, so the gate silently dropped
  it → `frees_param[i]` never set. Distinct-unwrap class (BUG-409/GAP-F) on the TypeNode axis.
  FIX: gate on the IR local's RESOLVED `Type *` via
  `type_unwrap_distinct(func->locals[plocal].type)` and check `TYPE_POINTER/TYPE_HANDLE/
  TYPE_OPAQUE` — TYNODE form irrelevant. Mirror of the apply-side at `zercheck_ir.c:3974-3985`.
  Add the branch's 3 `tools/type_dispatch_baseline.txt` entries for the `pt_eff->kind` reads.
  Tripwire `tests/zer_fail/typedef_ptr_funcsummary_uaf.zer`. CLASS: distinct-unwrap / missing-
  site. FLAG: FLAG #2 (see below).

- **[T1.5] ongou2 `bbbdf95c` (hole 1) — assignment-form call-launder defeats escape check (3
  sinks).** CRITICAL UAF / dangling-global. File: `checker.c` ~4170 (the NODE_ASSIGN re-derivation
  block). CAUSE: `*Box p = &g; p = launder(&local_box); spawn worker(p);` compiled clean — the
  assignment path was missing the parallel of "Case D" (BUG-770) that the var-decl handler has
  at `checker.c:10027`. FIX: add the same arm to the assignment path, predicate
  `call_has_local_derived_arg` (the one 4 existing sinks use), type-gated on
  `type_can_carry_pointer` so scalar reductions (`acc = op(acc, data[i])`) aren't false-
  positived. Closes spawn / global-store / return sinks for the assignment-launder shape
  (pointer + slice). Tripwires `tests/zer_fail/assign_launder_{global,slice,spawn}.zer`. CLASS:
  per-sink escape patchwork / missing-sink.

- **[T1.6] ongou2 `bbbdf95c` (hole 2) — switch-default capture escapes ptr-to-local to a
  global (BH-18 #6 SIBLING).** CRITICAL UAF. File: `checker.c`, switch-arm capture handler (sibling
  of BH-18 #6 at `checker.c ~10459`). CAUSE: `switch(m){ default => |*v| { g = v; } }`
  accepted while the `if |*v|` sibling was rejected; the switch-arm capture-desugar didn't
  inherit the matched value's region. FIX: when the capture is a pointer (`|*v|`) AND the
  switch root resolves to a function-local, mark the capture `is_local_derived` (same rule as
  BH-18 #6). CERTIFIED by `capture_lattice.v` "capture inherits the payload's region".
  Tripwire `tests/zer_fail/switch_default_capture_escape.zer`. CLASS: per-sink capture
  patchwork / missing-sink.

### TIER 2 — AST→IR DRIFT pair. Take, THEN re-run the drift audit grep. (2 holes)

- **[T2.1] a5erj3 `9e47b9c4` (part c) — `static u32 v = @ctz(16);` emits invalid C.** MEDIUM
  invalid C. File: `emitter.c`, `@ctz`/`@clz` IR emitter. CAUSE: the IR path ALWAYS emitted
  the statement-expression `({...})` zero-guard wrapper; the AST path already had a conditional
  form. GCC rejects a stmt-expr in a static-local initializer ("initializer element is not
  constant"). FIX: `@ctz`/`@clz` IR emitter uses the conditional form when the arg has no side
  effects (safe to double-evaluate); keep the stmt-expr for side-effecting args.

- **[T2.2] ongou2 `bbbdf95c` (hole 3) — IR auto-guards gate missing `IR_AWAIT` and `IR_NOP`.**
  CRITICAL silent corruption (dropped bounds guard). File: `emitter.c`. CAUSE: `await arr[i]` /
  `spawn worker(arr[i])` with unproven `i` PRINTED "auto-guard inserted" but emitted RAW
  unchecked access (baremetal: corruption; hosted: SIGSEGV-rescued). FIX (two pairs): (a)
  `emitter.c:11241` and `:11380` — add `|| k == IR_AWAIT || k == IR_NOP` to BOTH auto-guards
  gate lists (regular IR + async paths); (b) `emitter.c:406` (`emit_auto_guards`) — replace
  the NODE_SPAWN/NODE_AWAIT fall-through-as-leaf with descent into `spawn_stmt.args[]` and
  `await_stmt.cond` (both were silently no-op'd as leaves). Tripwires (POSITIVE)
  `tests/zer/{await_array_index_autoguard,spawn_arg_array_index_autoguard}.zer`. The commit
  itself calls this "the same shape as BUG-595..612 (audit gap recurrence)."

  **AFTER T2: run the AST→IR emission diff audit** (CLAUDE.md "AST→IR emission diff audit"):
  `grep -nE "_zer_trap|_zer_bounds_check|_zer_shl|_zer_shr|_zer_probe" emitter.c` and confirm
  every AST-region (line < 4000) safety wrapper has an IR-path equivalent. Two drift recurrences
  in one week (T2.1 + T2.2) ⇒ the gate lists are a recurring weak point — sweep for siblings.

### TIER 3 — OVERLAPS main's recent walker rewrites. Do LAST, RE-DERIVE (do not copy). (5 holes, 1 class)

- **[T3] a5erj3 `9e47b9c4` (a,b) + `ef7fb239` + `5001940b` — field-projection blindness in 5
  shared-type walkers.** CRITICAL data race. Each walker walked to the innermost `NODE_IDENT` and
  checked only that ident's type, so an intermediate `*shared S` FIELD projection
  (`Wrap w; w.sp = &shared_g; w.sp.v = 99;`) passed silently → the write emitted with NO
  `pthread_mutex_lock`. The 5 walkers:
  1. `find_shared_root_expr` (`ir_lower.c` ~1144) — the lock emitter [`9e47b9c4` a]
  2. `collect_shared_types_in_expr` (`checker.c` ~16343) — same-statement deadlock detector [`9e47b9c4` b]
  3. `scan_body_shared_types` (`checker.c` ~16150) — transitive callee scan [`ef7fb239`]
  4. `cond_pred_foreign_shared` (`checker.c`) — `@cond_wait` scanner [`5001940b` a]
  5. `emit_defer_shared_root` (`emitter.c`) — defer-body lock walker [`5001940b` b]
  FIX PATTERN: at each FIELD/INDEX/deref step, check the OBJECT's resolved type (object-side,
  NOT the outer expression's own type — this preserves "writing a pointer field" [no pointee
  lock] vs "accessing through the pointer" [pointee lock]). When the object is `shared` or
  `*shared S`, THAT is the lock/scope root. For the `checker.c` walkers use `typemap_get`
  (populated by the check pass for params + intermediate projections) with `scope_lookup`
  fallback for bare globals.
  WARNING: **CRITICAL OVERLAP — re-derive, do NOT cherry-pick:** main rewrote walkers 2/3/4 AFTER
  a5erj3 branched (`collect_shared_types_in_expr` → `22061071`; `scan_body_shared_types` →
  `dafbc1f6`; `cond_pred_foreign_shared` → `28e9562e`; these are the exhaustive-switch + BH-18
  #7 subexpr-form fixes). The gap STILL EXISTS in main (verified: main's
  `collect_shared_types_in_expr` NODE_FIELD case still does
  `while(root->kind==NODE_FIELD) root=root->field.object`). A verbatim copy would conflict
  with / revert the exhaustive-switch work. **Re-apply the object-type-per-step check onto
  main's CURRENT structure, and ideally UNIFY the 5 sites into one shared helper
  (`shared_root_through_projections()`)** rather than patching 5 walkers a third time.
  Tripwires `tests/zer_fail/shared_field_pointer_multi.zer`, `tests/zer/shared_field_pointer_
  locks.zer`, `tests/zer_fail/shared_transitive_field_ptr.zer`, `tests/zer_fail/cond_wait_
  foreign_field_ptr.zer`. Add the branch's `type_dispatch_baseline.txt` entries for the
  already-unwrapped `eff->kind` reads. CLASS: form-coverage / per-sink patchwork (×5).

### THE 3 FLAGS (carry forward even after the fixes land)

- DONE: **FLAG #1 — AUDITED CLEAN (2026-07-01), no remaining drift.** Full AST→IR emission-diff
  audit run after T2: (1) WRAPPER-TYPE coverage — every AST-region (<4000) safety trap
  (`division by zero`, `signed division overflow`, `@inttoptr` range/align, `@ptrcast`/`@pun`
  mismatch, `slice start>end`/`end>len`, `type mismatch in cast`, `_zer_shl/shr`,
  `_zer_bounds_check`, `_zer_probe`) has an IR-path twin (code inspection); (2) ARRAY-INDEX
  CONTEXT coverage — read/branch/while/index-write/field-write/nested-field-index with an
  UNPROVEN (param) index are ALL auto-guarded (6 behavioral tests, no segv); (3) SHIFT contexts
  — binary `<<`, compound `<<=`, array-element `a[0]<<=` all emit `_zer_shl` (over-width →
  defined 0). STRUCTURAL REASON it's robust: 3AC lowering decomposes every access into its own
  gated instruction (IR_ASSIGN/IR_INDEX_READ/…), so the surrounding context can't drop the
  guard — which is exactly why `await`/`spawn` (whose exprs are NOT pre-decomposed, carried on
  IR_AWAIT/IR_NOP) were the ONLY drift, closed by T2.2. No automated `make check` gate added: a
  simple grep can't catch the gate-list-completeness risk (the real failure mode), and the
  manual protocol in compiler-internals.md "AST→IR emission diff audit" remains the tool. The
  two T2 holes were the live instances; the class is now closed.
- DONE: **FLAG #2 — RESOLVED (2026-07-01).** `tools/audit_type_dispatch.sh` now ALSO scans the
  syntactic `TypeNode` axis (`->kind == TYNODE_` / `!= TYNODE_`); the 12 legitimate existing
  sites are baselined and a NEW TYNODE dispatch trips the gate (validated by inject-and-revert).
  The distinct-unwrap class can no longer recur undetected on the TypeNode axis.
- FLAG: **FLAG #3 — RETRACTED (wrong-base assumption).** All hunks applied cleanly onto the
  rewritten walkers.

### STILL OPEN — triaged against current main 2026-07-01 (all confirmed LIVE except where noted)

- DONE: **AU-1 / AU-2 / AU-3 / AU-4 — FIXED 2026-07-01** (see BUGS-FIXED.md): defer LIFO use-after-free;
  deferred `arena.reset()`; nested struct-init escape; direct-assign struct-init escape. All were
  confirmed LIVE by triage, all now reject.
- DONE: **bh18_1b — FIXED 2026-07-01** (see BUGS-FIXED.md): move-struct use-after-move via a
  pre-existing pointer alias. Register the move local when `&a` is taken (flagged `is_move_local`
  so the leak check skips it + its alias) + propagate TRANSFERRED to the alloc_id group at the
  transfer. Tests `tests/zer_fail/move_alias_stale_read.zer` + `tests/zer/move_alias_ok.zer`.
- DONE: **bh18_12 — FIXED 2026-07-01** (see BUGS-FIXED.md): defer fired N× on a same-scope backward
  goto. Fix: per-label `defer_count_at_def`; a backward goto fires only defers registered AFTER
  the label (loop-body defers), leaving pre-label defers pending for the real exit. Forward gotos
  unchanged (base 0 + sesjma guard). Tests `tests/zer/defer_goto_{backward_once,loopbody_periter}.zer`.
- DONE: **AU-5 — FIXED 2026-07-01** (see BUGS-FIXED.md): the ISR/@critical/async context-restriction
  scan (`scan_func_props`) was blind to a function passed as a funcptr argument and invoked
  indirectly. Per primitives-data-races.md §2.3/§5.7 (context restrictions are Definition-A
  VERIFIED), closed by propagating a funcptr-arg function's props to the parent (mirrors BH-18 #8).
  Tests `tests/zer_fail/isr_alloc_via_funcptr.zer` + `tests/zer/funcptr_alloc_non_isr_ok.zer`.
- PAUSED: **AU-6** (privileged `@cpu_*` have no call-site context check) — **DEFERRED to the Option E
  ASM-safety rework** (`docs/asm_lang_zer_safe.md`, LOCKED). Under Effect-Row Composition the
  privileged `@cpu_*` ops are Tier-B LEAVES; their privilege safety is a declared effect-row
  category (`changes_privilege: requires_cpl0` + the mandatory `safety:` string), enforced as
  WITNESSED (QEMU CPL readback) or DECLARED+TAINTED (named floor), NOT a static per-call context
  gate — actual CPL is a runtime hardware fact ZER can't check statically. AU-6's context-check
  approach is superseded; do NOT implement it standalone. NOTE: the intrinsics STAY (they become
  the leaves; `option_e_plan.md` STEP 0 deletes the per-arch tables, not the intrinsics).
- `naked_attribute_silently_dropped` (intentional deferral).

---

## CLOSED 2026-09-25 — `tools/audit_matrix.sh` was STALE (16 false positives)

Rewritten as a BEHAVIOURAL matrix, as the fix sketch proposed: 25 cells, one compiled program
per construct × context, each reject cell asserting the rule's own wording (so another rule
cannot mask it), plus boundary cells that must compile. In `make check` after the emit audit.
Verified to fire against a stub compiler that accepts everything (21 mismatches).

## FIXED (BUG-762, 2026-06-22) — struct-wrapped-slice launder escaped via a field store

**Symptom (verified 2026-06-22 by the escape-sink enumeration's adversarial pass):**
a function that returns a STRUCT BY VALUE wrapping a pointer/slice PARAM, called with
a LOCAL, then has its field extracted and stored to a global, escapes undetected:
```
struct View { [*]u8 data; }
[*]u8 g_ptr;
View get_view([*]u8 s) { View v = { .data = s }; return v; } // compiles (param wrap — fine)
void caller() { u8[10] buf; View v2 = get_view(buf[0..10]); g_ptr = v2.data; } // ESCAPES
```
`v2.data` points into `buf`; `g_ptr = v2.data` is not flagged. (The DIRECT case —
returning a struct wrapping the function's OWN local — IS caught: `v` becomes
local-derived. The gap is only the call-RESULT propagation.)

**Root cause:** the var-decl provenance that marks `t` local-derived from
`t = f(local)` (via `call_has_local_derived_arg`) is gated on the return type being a
POINTER/SLICE — a STRUCT return wrapping a pointer/slice is not tagged, so `v2` stays
clean and the later `g_ptr = v2.data` isn't caught. Same family as BUG-760/761 (call
result provenance), one level up (struct-wrapped).

**FIX (BUG-762):** the actual gap was narrower than the sketch — `v2` IS marked
local-derived (the var-decl provenance at ~9868 already handles struct returns); the
store-escape check at ~4043 just didn't walk a `.field` value to its root. Fixed by
descending `NODE_FIELD`/`NODE_INDEX` on the value (gated on the value being a
pointer/slice so scalar-field stores aren't false-flagged). Zero over-rejection.
**Residual [OPEN, narrow]:** a NESTED struct field carrying pointers
(`g = v.inner_struct` where inner is a sub-struct) — the gate is pointer/slice only,
not `type_carries_data_pointer` on struct/union values; the whole-struct store
(`g = v`) is still caught, so this is a deep-nesting residual only.

---

## RESOLVED (2026-06-22e) — unified call-result provenance (the durable fix for BUG-760..763's class)

**Status: RESOLVED.** Stages 1-3 + the tail closed every call-result over-rejection,
fixed a shadowing UAF, added per-param + keep-inference precision, and centralized the
policy into `call_result_escapes`. The detail below is kept for history (the per-sink
patchwork class and the staged plan); the only intentionally-declined item is the
compute-once-cache-on-node optimization (see the end). No open over-rejection or safety
gap remains in this subsystem.


**Pattern (2026-06-22):** the return-borrow-from-param investigation surfaced FOUR
escape holes (BUG-760 store/return call-launder, BUG-761 slice-param keep inference,
BUG-762 struct-field store, BUG-763 keep call-launder) — ALL the same shape: a
per-sink escape check independently failing to recognise a *call-laundered* or
*slice-of-local* argument. Each was a separate site re-implementing "is this value
derived from a local?" and several got it incomplete. The remaining suspected
siblings (not yet verified/fixed): `call_has_nonkeep_derived_arg` lacks NODE_SLICE
descent; spawn-arg launder `spawn f(g_via_call(local))`; nested-struct-field
(`g = v.inner_struct`).

**Durable fix (the right architecture):** compute call-result provenance ONCE — when
a call's return type is value-carrying (pointer / slice / struct-or-union that
`type_carries_data_pointer`) and any arg is local/arena-derived, mark the result
symbol `is_local_derived` (and the keep-edge) in a SINGLE place (the var-decl +
assignment + arg-evaluation provenance), so every sink (store, return, keep,
struct-field, spawn) catches it through the one flag instead of re-walking. This is
ZER's "infer what Rust annotates (the `'a`)" — return provenance is the inferred
lifetime relationship. It would close the whole class (incl. the unverified siblings)
and make the return-borrow-from-param relaxation below trivially safe. Refactor-sized
(touches several escape sites); not whack-a-mole. Until then, each sink is patched
individually (BUG-760..763 done; siblings open).

**Theorem-first grounding (2026-06-22):** the lattice this refactor needs is now
MECHANIZED — `proofs/operational/lambda_zer_escape/param_lattice.v` (7 `Qed`, 0
admits, in the `make check-proofs` gate). It extends the operationally-proven
3-region oracle (only `RegStatic` escapes, `iris_escape_specs.v`) with the relational
constructor `ARParam n` (the per-function return summary "result aliases parameter
n", = the inferred `'a`) and proves: (T1) `subst_escape_sound` — resolving the
summary against the actual arg regions at the call site is sound (no under-rejection);
(T2/`pick_escapes_iff_chosen_static`) — a pick-one function escapes iff the CHOSEN arg
is static, not iff "no arg is local"; (T3) `precision_gain_unrelated_static` — the
current `old_approx` (checker.c:9937 `if (call_has_local_derived_arg) result=LOCAL`)
gratuitously rejects an `ARStatic`-returning callee on a local-containing call, while
the new analysis allows it AND is sound; (T4) `new_never_underrejects` — a true-LOCAL
result is never permitted to escape. The finite states the implementation must track
are exactly `{STATIC, LOCAL, ARENA}` + `PARAM(n)` + join + the call-site
substitution.

**Stages 1+2 SHIPPED (2026-06-22b/c, see BUGS-FIXED.md):** the per-function return
summary `{Symbol.ret_summary_complete, Symbol.ret_param_mask}` (mask bit n = some
return may be a view of parameter n; complete = every return classifiable as STATIC
or ARParam(n)), computed by an accumulator (`Checker.cur_ret_summary_complete` +
`cur_ret_param_mask`, updated at each return in the `NODE_RETURN` handler — sound by
construction, catches `orelse`-block returns and runs in-scope so params are
identifiable). `classify_return_root` classifies one return (with the
param-shadows-global fix: the name is the global only if the resolved binding IS the
global symbol, `src == gsym` — a Stage 1 UAF under-rejection, now fixed +
regression-tested). The call-site query `call_result_static_given_args` is the
substitution `resolve(R_f, argreg)`: result is static-escapable iff the summary is
complete AND every masked param's actual arg is static. It gates the FOUR
direct-call-result sinks (var-decl ~9958, assignment ~4315, return-of-call ~11308,
return-field ~11326). Kills both the **unrelated-static** over-rejection
(`g = lookup(local)`, lookup returns a global) and adds the **multi-param precision**
(`second(local, global)` returning param 1 is allowed — the returned param's arg is
the global; `longest(local, global)` returning EITHER stays rejected; `trim(local)`
stays rejected). Conservative: defaults `{false, 0}` → no under-rejection (T4). The
per-arg predicate `arg_is_local_derived` was extracted from `call_has_local_derived_arg`
(behavior-preserving) for the per-masked-param check. Tests:
`tests/zer/returns_static_no_overreject.zer`,
`tests/zer/escape_param_view_static_arg_ok.zer`,
`tests/zer_fail/returns_param_still_rejected.zer`,
`tests/zer_fail/escape_param_shadows_global.zer`,
`tests/zer_fail/escape_multi_return_local.zer`.

**Stage 3 SHIPPED (2026-06-22d, see BUGS-FIXED.md):** the call-result OVER-rejection
is now unified onto the ONE query `call_result_static_given_args` across EVERY
applicable sink. Probing the candidates showed Stage 3 was a single gate: the keep-call
sink (~5707) was the only ungated direct-call-result over-rejection (now gated — a
static call-result is RETAINABLE so it satisfies keep, `store(second(local, global))`
allowed); struct-field-store-to-global already flows through the gated Stage 2 assign
sink (via `classify_escape_sink`, verified compiling); spawn rejects non-shared pointer
args regardless (moot). Tests: `tests/zer/escape_keep_static_call_result_ok.zer`,
`tests/zer_fail/escape_keep_call_returns_local.zer`.

**What's deliberately NOT unified (different axis):** the keep-INFERENCE site (~4241,
`call_has_nonkeep_derived_arg`) asks "does the result launder non-keep param p?", NOT
"is the result static?" — gating it with `call_result_static_given_args` would
UNDER-infer keep. **DONE (2026-06-22e):** the keep-INFERENCE precision shipped via the
RIGHT query — `infer_keep_from_call_args` uses `ret_param_mask` to infer keep on the arg
at position i only if the callee may actually RETURN position i (the result-launder
path); the internal-retention escape path stays covered by the keep-call-site
transitivity. Tests: `tests/zer/keep_infer_scratch_not_kept_ok.zer`,
`tests/zer_fail/keep_infer_internal_keep_transitivity.zer`.

**Policy centralization DONE (2026-06-22e):** the five call-result sinks now consult ONE
function `call_result_escapes(c, call)` (= `call_has_local_derived_arg` AND
`!call_result_static_given_args`) instead of repeating the two-part predicate — the
escape policy lives in a single place. The literal compute-once-CACHE-on-node variant was
deliberately NOT done: it's a no-behavior-change optimization that adds stale-cache risk
to safety analysis (a stale region = under-rejection = UAF) for marginal compile-time
gain, and the re-walk it would save is trivial (a handful of args at check time). **This
entry is RESOLVED** — no open over-rejection, no safety gap, policy centralized; the
node-cache is intentionally declined.

---

## FIXED (BUG-764, 2026-06-22) — return-borrow-from-param relaxation shipped

**RESOLVED:** returning a sub-slice/`&`-element of a slice/pointer PARAM is now
allowed (3 return-escape sites relaxed to skip slice/pointer params that are not
`is_local_derived`). Verified against the complete sink matrix — every escape is
still caught (the BUG-760..763 fixes covered the sinks); `lib/str.zer`'s `bytes_trim*`
compile again. The empirical matrix proved the conservative proxy + the 4 fixes
suffice; the unified lattice refactor (above) is now optional future-proofing, not a
prerequisite. Original symptom kept below for history.

**Symptom (was, verified 2026-06-22):** returning a sub-slice or `&`-element of a
slice/pointer PARAMETER was rejected outright — `[*]u8 trim([*]u8 s){ return
s[i..j]; }` and `*u8 first([*]u8 s){ return &s[0]; }` both error `"cannot return
pointer to local 's'"`, regardless of `const`, `[]u8` vs `[*]u8`, or literal vs
variable bounds. So slice/string helpers that return a view into their input
(`trim`/`split`/`find`) can't be written; `lib/str.zer`'s `bytes_trim*` do NOT
compile. (This is the ZER analog of Rust's `fn f<'a>(s:&'a[u8])->&'a[u8]`, which
Rust expresses with a lifetime param.)

**Root cause:** the return-escape `sliced_borrow` promotion (checker.c ~11016)
promotes region STATIC→LOCAL for *any* non-static non-global root, which wrongly
includes a slice/pointer param (whose pointee is the CALLER's memory, not the
frame). The `&expr` return path (~10951) rejects `&s[0]` the same way. It is
**safe (conservative reject), never a UAF** — it errs toward rejection.

**Why it's only LOW / not rushed:** relaxing it safely requires the call-result
provenance to catch every escape *through* such a function at the call site. As of
2026-06-22 the major sinks are ALL verified to catch a call-laundered / slice-param
local: store-to-global + return (BUG-760, one-step AND two-step), struct-field of a
global (already worked), and keep-param pass-through (BUG-761). So the prerequisite
is essentially met. The safe relaxation itself is then a **~10-line change** in the
two return-escape sites: in the `sliced_borrow` promotion (~11016) and the `&expr`
return path (~10951), do NOT reject when the root symbol's type is a SLICE/POINTER
(external pointee — the caller's memory) — only ARRAY/STRUCT roots (frame storage)
escape; the `is_local_derived` bit already flags a slice LOCAL pointing to a local,
so that stays rejected. After relaxing, re-verify the full sink matrix with an
actual sub-slice-returning helper (`trim`) — local use allowed, every escape (g =
trim(local), t = trim(local);g=t, keepfn(trim(local)), struct-field = trim(local))
rejected. This is the "infer what Rust annotates (the `'a`)" enhancement; remaining
unverified sinks are the rare ones (Pool/Slab/Ring element store). NOTE: once
relaxed, `lib/str.zer`'s `bytes_trim*` should be revisited (they currently don't
compile).

---

## CLOSED — 6u360k audit (2026-06-09): all 8 gaps fixed (BUG-734..741)

All 8 silent gaps from branch `claude/cool-johnson-6u360k` are closed and
suite-guarded: GAP-5 orelse overwrite leak (BUG-734), GAP-1 @ptrcast concrete
type confusion (BUG-735), GAP-2 --no-strict-mmio runtime alignment trap
(BUG-736), GAP-8 by-value struct param laundering (BUG-737), GAP-7 container
composite type args (BUG-738), GAP-3 alloc_ptr global-alias UAF (BUG-739,
per-function scope — see follow-up below), GAP-4 funcptr double-free
(BUG-740, argument-precise barrier), GAP-6 variable-index array double-free
(BUG-741). Per-gap detail in BUGS-FIXED.md Session 2026-06-09/10 entries.
The `tests/audit_2026-06-09/` reproducer directory is retired — every
reproducer was promoted into `tests/zer_fail/` or `tests/zer_trap/`.

## ~~Gap 38 — function-return Handle bypassed zercheck_ir~~ (FIXED 2026-05-05)

`zercheck_ir.c` IR_CALL summary path didn't include `TYPE_HANDLE` /
`?TYPE_HANDLE` in its `is_ptr_return` check, so any wrapper function
returning a handle (`?Handle(T) get_handle() { return heap.alloc(); }`)
left the caller's local untracked. Subsequent double-free was silent.

Fixed by extending the check, gated on `summary->returns_color` being
a known allocator (`POOL`/`MALLOC`) so accessor/transfer wrappers like
`pop_free()` from a global queue don't misfire as leaks. The
defer-body scanner was simultaneously extended to consult FuncSummary
`frees_param[i]` so user free-wrappers (`defer device_destroy(h)` →
`pool.free(h)`) propagate FREED through the alias-id chain.

Tests: `tests/zer_fail/gap38_func_return_handle_dfree.zer`,
`tests/zer/gap38_func_return_handle_ok.zer`. See BUGS-FIXED.md
"BUG-661" for the full session entry.

## ~~Runtime preamble unguarded pthread types broke freestanding~~ (FIXED 2026-05-05)

`emitter.c:4564-4583` defined `_zer_mtx_ensure_init_cv` and
`_zer_mtx_ensure_init` referencing `pthread_mutex_t *`,
`pthread_cond_t *`, and `PTHREAD_MUTEX_RECURSIVE` outside any
`__STDC_HOSTED__` guard. `gcc -ffreestanding -D__STDC_HOSTED__=0`
failed with `'pthread_mutex_t' undeclared`. Wrapped the helper
definitions in the same hosted guard already used for the include and
the thread barrier block. See BUGS-FIXED.md "BUG-662".

## ~~`_zer_trap` libc-abort fallback broke freestanding~~ (FIXED 2026-05-05)

`_zer_trap` always emitted `fprintf(stderr,...)` and used `abort()` in
the unknown-arch fallback. Both need libc, so freestanding x86 (kernel
mode, EFI apps) or freestanding non-{ARM,RISC-V,AVR,x86} (MIPS,
PowerPC, SPARC, custom) failed to link. Restructured: the diagnostic
print is gated on `__STDC_HOSTED__`, per-arch trap instructions emit
unconditionally, and the `#else` fallback uses `__builtin_trap()` on
freestanding. See BUGS-FIXED.md "BUG-663".

## ~~`@once` lacks `__STDC_HOSTED__` guard~~ (FIXED 2026-05-02, commit `664b211`)

Wrapped atomic emission in `#if __STDC_HOSTED__` with a non-atomic
fallback for freestanding builds. See BUGS-FIXED.md "Fix #2".

## ~~`@probe` silently succeeds on freestanding~~ (FIXED 2026-05-02, commit `edce2a3`)

Added `--probe-mode={hosted,raw,disabled}` CLI flag with three modes:
hosted (signal-handler default), raw (direct read, no fault recovery),
disabled (compile-error if `@probe` used). See BUGS-FIXED.md "Fix #4".

## ~~`@critical` indirect return via callee~~ (INVESTIGATED 2026-05-02 — not a bug)

**Status:** Audit claim re-verified against actual emission and found
to be incorrect. No fix needed.

**Original claim (from claude/cool-johnson-apebs branch audit):**
calling a function from `@critical` lets the function's `return`
"escape" the `@critical` block without re-enabling interrupts:

```zer
void unlock() { return; }
@critical { unlock(); } // claimed: interrupts NOT re-enabled
```

**Why the claim is wrong:** A normal function call returns to its
caller. The caller is `@critical { ... }`. Execution returns to the
@critical body, continues to the closing brace, and the closing brace
emits the interrupt-restore code. Verified empirically by inspecting
emitted C: `cpsid i` at @critical entry, `msr primask, ...` at
@critical exit, function calls in between emit standard call/ret —
control flow returns to the @critical body, not to the function that
contains @critical.

A `can_escape` predicate (transitive return/break/continue/goto)
would have to reject EVERY function call from @critical (every
non-trivial function returns), making `@critical` essentially
unusable.

**What IS actually checked today** (sufficient):
1. Direct `return`/`break`/`continue`/`goto` inside `@critical` body —
   per-site rejected (NODE-level checks)
2. `yield`/`await` directly or transitively — `can_yield` propagation
3. `spawn` directly or transitively — `can_spawn` propagation
4. Heap alloc directly or transitively — `can_alloc` propagation
5. Inline asm directly or transitively — `can_*` via FuncSummary

The branch's audit was correct in spirit (transitive checks are good)
but misanalyzed control-flow semantics for plain function returns.

## ~~AST `emit_expr` compound `/=` and `%=` lack signed-overflow trap~~ (FIXED 2026-05-02, commit `b4d10ed`)

Ported the same `INT_MIN/-1` overflow trap pattern from the IR path
to the AST `emit_expr` sibling at emitter.c:1433–1444. Defense in
depth even though function bodies are IR-only since 2026-04-19. See
BUGS-FIXED.md "Fix #3".

## ~~u64 atomic warning fires on 64-bit targets~~ (FIXED 2026-05-02, commit `c4bbbe0`)

Gated the "may require libatomic on 32-bit" warning on
`target_ptr_bits < 64`. The fix uncovered a deeper bug — BUG-652:
`Checker.target_ptr_bits` was never initialized from the global, so
the field was memset-zeroed and any `target_ptr_bits < N` check
silently always-true. See BUGS-FIXED.md "Fix #1".

## ~~Codebase audit 2026-05-07 — 5 silent gaps closed~~ (FIXED 2026-05-07)

Full bug-hunt session. See BUGS-FIXED.md "Session 2026-05-07" for
per-bug detail.

- BUG-661 — `return s.len` (scalar field of local-derived slice)
  rejected as a pointer escape. Fixed: gate the field-walk escape
  check on `type_can_carry_pointer(ret_type)`.
- BUG-662 — `@critical { wrapper(); }` where wrapper transitively
  calls slab.alloc(): silently allowed. Fixed: enable ban_alloc=true
  on @critical's check_body_effects.
- BUG-663 — `slab.free()` / `slab.free_ptr()` / `Task.free()` /
  `Task.free_ptr()` inside @critical or interrupt handlers:
  silently allowed. Fixed: per-site check_isr_ban at the four free
  handlers.
- BUG-664 — Pool/Ring/Arena `.alloc()` flagged as heap by
  scan_func_props (false positive on transitive path through a
  pool wrapper). Fixed: gate can_alloc on receiver TYPE_SLAB or
  TYPE_STRUCT (Task auto-slab); Pool/Ring/Arena types excluded.
  Same change extends detection to free/free_ptr methods, closing
  the transitive variant of BUG-663.
- BUG-665 — `return g.v;` where `g` is `shared struct` leaked the
  auto-mutex past return. Multi-threaded programs deadlock on the
  next acquire. Fixed: ir_lower NODE_RETURN / NODE_BREAK /
  NODE_CONTINUE / NODE_GOTO emit IR_UNLOCK before the exit when an
  enclosing block holds a shared lock.

Cumulative fix size ~70 LOC across checker.c + ir_lower.c. Seven
regression tests added (3 positive + 4 negative). Full test suite
green (1,400+ tests across tests/zer, test_modules, rust_tests,
zig_tests).

Single-level tracking on the BUG-665 fix — nested cases where an
ancestor block holds a different shared root still leak the
outer lock. Tracked as a follow-up; the simple `return
shared.field;` form is now safe.

## ~~Gap 38 — function-return Handle bypasses zercheck_ir tracking~~ (FIXED 2026-05-16)

**Status:** zercheck_ir now treats `Handle(T)` / `?Handle(T)` returns
the same way it treats pointer returns — registers the dest local as
fresh ALIVE with `escaped=true`. The escape flag suppresses the
leak-at-exit check (caller commonly passes the handle to a destructor
via `defer device_destroy(h)`, where the destructor lives in another
module and its FuncSummary may not propagate) while still preserving
FREED-state transitions for double-free / UAF detection.

Two patterns now caught:

```zer
?Handle(Task) get_handle() { return heap.alloc(); }

?Handle(Task) mh = get_handle();
Handle(Task) a = mh orelse return;
heap.free(a);
heap.free(a); // detected: double-free
```

```zer
Handle(Task) h = get_handle() orelse return;
heap.free(h);
heap.free(h); // detected: double-free (orelse-IR_ASSIGN variant)
```

Two reproducers landed in `tests/zer_fail/`:
- `gap38_fn_return_handle_double_free.zer` (IR_CALL site fix)
- `gap38_handle_orelse_return_double_free.zer` (IR_ASSIGN orelse-decomp site fix)

Implementation: `zercheck_ir.c` — added TYPE_HANDLE to the existing
pointer-return detection in two branches (FuncSummary present and
not-present) of the IR_CALL handler, plus a new IRMC_NONE Handle
branch in the IR_ASSIGN handler for orelse-wrapped calls. Both sites
set `h->escaped = true` after registering.

## ~~Gap 27 — `@cstr` to raw `*u8` destination — no bounds check~~ (FIXED 2026-05-16)

**Status:** checker now rejects `*u8` (non-volatile, non-const) as
`@cstr` destination. Pattern that silently overflowed before:

```zer
u8[4] buf;
*u8 p = buf[0..].ptr;
@cstr(p, "Hello, world this is too long"); // 29-byte write to 4-byte buf
```

Fix: error with hint to use slice (`[*]u8`) or fixed array (`u8[N]`)
destination. Volatile pointers (MMIO registers) are still permitted —
size is hardware-fixed there. Reproducer: `tests/zer_fail/gap27_cstr_raw_ptr_dest.zer`.

Implementation: `checker.c` `@cstr` intrinsic handling — added
TYPE_POINTER rejection that excludes `is_volatile` (so MMIO writes
still compile) and `is_const` (already rejected earlier with a
different message).

## ~~Gap 10 — `@critical` on bare-metal x86 only emits memory fence~~ (FIXED 2026-05-16)

**Status:** `IR_CRITICAL_BEGIN` / `IR_CRITICAL_END` now emit an
`#elif (defined(__x86_64__) || defined(__i386__)) && (!defined(__STDC_HOSTED__) || __STDC_HOSTED__ == 0)`
branch that saves EFLAGS and runs `cli` (begin) / `popf` (end). On
hosted x86 user-mode the fence-only fallback is preserved — `cli`
SIGSEGV's at CPL > 0, and user code can't legitimately disable
interrupts there anyway.

Verified by disassembly of a freestanding-compiled @critical test:

```
0000000000000000 <main>:
   0: endbr64
   4: pushf
   5: pop %rax
   6: cli ; <-- actual interrupt disable
   7: addl $0x1,0x0(%rip) ; critical body
   e: push %rax
   f: popf ; <-- EFLAGS restore
  10: mov 0x0(%rip),%eax
  16: ret
```

The arch cascade order in `emitter.c` IR_CRITICAL_BEGIN/END is now:
`__ARM_ARCH` → `__AVR__` → `__riscv` → bare-metal x86 → fence
fallback. Bare-metal x86 takes precedence over the generic fence.

---

## ~~Pool/Slab.free of auto-zero Handle corrupts slot 0~~ (FIXED 2026-05-21, audit session)

**Symptom (pre-fix):** declaring `Handle(Item) h;` without initializer
auto-zeroes to `h_gen == 0`, `idx == 0`. Calling `pool.free(h)` on this
"null" Handle bumped `pool.gen[0]` and cleared `pool.used[0]` —
silently invalidating any legitimate handle that the pool had issued
for slot 0 (which has `h_gen >= 1`). Subsequent `pool.get(legit_h)`
would trap on gen mismatch, but the trap message blamed the legitimate
handle instead of the actual cause (the null-handle free).

**Root cause:** `_zer_pool_free` and `_zer_slab_free` lacked the
`h_gen == 0` short-circuit. Compile-time `zercheck_ir` did not flag
the use of an uninitialized Handle either, because UNKNOWN-state
handles fall through the IR_POOL_FREE handler (which only checks
ALIVE/FREED/MAYBE_FREED/TRANSFERRED transitions). Both compile-time
miss + runtime corruption = SILENT GAP class — caught only when a
legitimate handle later trapped on gen mismatch.

**Fix:** added `if (h_gen == 0) return;` no-op at top of
`_zer_pool_free` and `_zer_slab_free`. Auto-zero Handle free is now
silently ignored (the typical "null handle" pattern), preserving pool
state integrity. Regression tests:
- `tests/zer/null_handle_pool_free_noop.zer`
- `tests/zer/null_handle_slab_free_noop.zer`

**Stronger validation deferred.** Ideally `_zer_pool_free` would also
trap on `gen[idx] != h_gen` (matching `_zer_pool_get`'s discipline),
catching wrong-pool / stale-handle frees that compile-time
`zercheck_ir` misses (e.g., cross-function calls without a
pool-aware FuncSummary). Attempted in this session but reverted: it
trips the **defer-fires-twice-on-goto-to-same-scope** emitter bug
described below.

## FIXED (verified 2026-07-01 — was stale OPEN entry) — Defer fires twice when goto target is in same defer scope

**RESOLVED.** The forward-goto shape quoted below (`defer free(h); goto
cleanup; cleanup: return 0;`) now fires the defer exactly once — verified
against current main with the exact reproducer. Closed by the plt86m
(2026-06-17/2026-06-20) + sesjma (2026-06-29, forward-goto-fallthrough)
guard-flag work, and the backward-goto sibling (BH-18 #12, loop back-edges)
by the 2026-07-01 `defer_count_at_def` fix. This entry's write-up predates
both fixes and was never removed. Original symptom/root-cause kept below for
history.

**Original symptom:** `defer free(h); ... goto cleanup; ... cleanup: return 0;`
fired the defer body once at the goto site AND again at the return
site. **Original root cause:** `ir_lower.c` NODE_GOTO emitted an eager
"fire all, no pop" that the function-exit fire then repeated; fixed via a
per-label runtime guard flag (goto path sets it, return-fire checks it).

## ~~4 narrow zercheck patterns not in zercheck_ir~~ (FIXED 2026-05-04, Phase F3.2)

**Originally discovered:** 2026-05-03 Phase F3 audit when `test_zercheck.c`
was deleted. **All four patterns now caught by zercheck_ir.**

Pattern 2 (direct overwrite leak) and Pattern 4 (struct copy alias UAF)
were fixed in commit `ce1d82a` (Phase F3.1, 2026-05-03):
- IR_COPY handler detects overwrite-while-alive on non-temp dest
- Compound handles propagate through struct value copies (two-pass
  collect + replicate, survives ir_add_compound_handle realloc)

Pattern 1 (wrong pool detection) and Pattern 3 (free-then-realloc loop
FALSE POSITIVE) were fixed in this session (2026-05-04):

**Pattern 1**: added `pool_name`/`pool_name_len` fields to `IRHandleInfo`,
captured at alloc sites via `ir_extract_pool_name`, propagated through
COPY and orelse-ident alias paths. New `ir_check_expr_wrong_pool` walker
mirrors `ir_check_expr_uaf` recursion shape and flags GET/FREE calls
where receiver name differs from the handle's recorded pool. Catches
both `pool_b.get(h).id` (NODE_FIELD wrapping) and bare-statement
`pool_b.free(h)`. ~120 LOC.

**Pattern 3 root cause**: `ir_merge_states` was missing the
`ALIVE + MAYBE_FREED → MAYBE_FREED` case. The lattice was non-monotonic:
when `first_live` had ALIVE and a later pred had MAYBE_FREED, the join
fell through and kept ALIVE. Result: state oscillated ALIVE↔MAYBE_FREED
across loop iterations and the fixed point never converged. Fix added
the missing case (and the symmetric TRANSFERRED + MAYBE_FREED). ~6 LOC.

**Tests** (added 2026-05-04):
- `tests/zer_fail/wrong_pool_get.zer` — must reject pool_b.get(h)
- `tests/zer_fail/wrong_pool_free.zer` — must reject pool_b.free(h)
- `tests/zer/free_realloc_loop.zer` — must compile + run cleanly

---

## STALE DUPLICATE BLOCK REMOVED (2026-07-01) — 2026-05-01 audit findings

This block originally re-stated, verbatim, five findings from the 2026-05-01
audit that were ALL already fixed and documented elsewhere in this file
(diff-confirmed 2026-07-01 — identical text to the `~~struck-through~~ (FIXED
…)` entries below: `@once` guard, `@probe` freestanding, `@critical` indirect
return [investigated, not a bug], AST compound `/=`/`%=` trap, u64 atomic
warning), plus a duplicate of the `naked`-is-silent-marker entry (see the
fuller write-up later in this file, "`naked` attribute silently dropped on IR
path (deferred 2026-05-02)" — that one remains genuinely OPEN, is asm-related,
and per current direction folds into the Option E rework, not a standalone fix).
The original audit dump is preserved in git history; removed here to stop the
doc claiming five fixed bugs are open.

---

## ~~BUG-579~~ (FIXED 2026-04-18, v0.4.9)

Switch arm body gaps — enum/union/optional switches now fully lower to IR.

## ~~BUG-581~~ (FIXED 2026-04-18)

`zerc --run` now propagates exit codes via `WEXITSTATUS` on POSIX.

## ~~BUG-582~~ (FIXED 2026-04-18)

Union variant tag update is emitted on the IR path for all target chain
shapes (`u.v = x`, `u.v[i] = x`, nested fields).

## ~~BUG-590 group — per-block defer firing, variable shadowing, capture scoping~~ (FIXED 2026-04-18)

`IRLocal.scope_depth` + `IRLocal.hidden` + scope-aware `ir_find_local`
handle variable shadowing correctly. `NODE_BLOCK` fires+pops its own defers
at block exit using the same POP_ONLY bb_post trick as loops, so
early-exit paths (return/break/continue/orelse-return) that emit earlier
blocks still find the defer bodies on the emit-time stack. When the
enclosing construct manages defers itself (loop, if-branch, switch arm),
`block_defers_managed` suppresses the block's own fire to avoid duplicates.

## ~~BUG-591~~ (FIXED 2026-04-18)

`await` condition is now re-evaluated on every poll. The IR_AWAIT emitter
emits `case N:;` followed by a fresh evaluation of the AST cond via
`emit_rewritten_node`, instead of reusing a stale pre-computed local.

## ~~BUG-592~~ (FIXED 2026-04-18)

Signed/unsigned comparison in IR_BINOP: when one side is signed and the
other unsigned, cast the unsigned side to the signed type before
emitting the operator. Without this, `signed_local < 0ULL` evaluated to
`false` because C promoted the signed operand to unsigned first. Also
IR_LITERAL now emits `(CType)N` cast to match the local's type instead
of always-ULL suffix.

## ~~BUG-593~~ (FIXED 2026-04-18)

Comptime float functions now dispatch to `eval_comptime_float_expr`
directly when return type is `f32`/`f64`. The integer `eval_comptime_block`
path was doing integer arithmetic on raw double bit-patterns and
returning garbage instead of `CONST_EVAL_FAIL`, short-circuiting past
the float path.

---

## ~~Silent gaps — 6 closed 2026-06-03~~ (FIXED)

Targeted audit found and closed six distinct silent gaps:

1. **`arena.alloc_slice` not classified as IRMC_ARENA_ALLOC** — slice
   from `ar.alloc_slice(T, n)` was not tagged `ZC_COLOR_ARENA`, so
   `arena.reset()` did not flag it. Post-reset access compiled clean.
   Fixed in `zercheck_ir.c`.
2. **Shared-struct `return field` leaked the mutex** — IR_UNLOCK emitted
   after IR_RETURN became dead code. Cross-thread access deadlocked.
   Fixed in `ir_lower.c` NODE_BLOCK loop.
3. **For-init read of shared struct without lock** — `for (u32 i = g.v;
   ...)` raced the field. Fixed in `ir_lower.c` NODE_FOR handler.
4. **For-step write to shared struct without lock** — same shape as #3,
   step expression side. Fixed in `ir_lower.c` NODE_FOR handler.
5. **Shared struct passed by value** — embedded mutex copied; function
   locked its own copy → zero synchronization with caller. Fixed in
   `checker.c` call-arg validation.
6. **`@ptrcast` launders arena pointer escape** — the arena escape check
   only matched direct NODE_IDENT values; `g = @ptrcast(*u32, arena_ptr);`
   bypassed it. Fixed in `checker.c` NODE_ASSIGN handler by unwrapping
   `@ptrcast`/`@cast` before the ident check.

See `BUGS-FIXED.md` "Session 2026-06-03" for full root-cause + fix
narrative per gap. Tests in `tests/zer/shared_return_no_deadlock.zer`,
`tests/zer/shared_for_init_locked.zer`,
`tests/zer_fail/arena_alloc_slice_uaf.zer`,
`tests/zer_fail/shared_struct_by_value.zer`,
`tests/zer_fail/arena_ptrcast_escape.zer`.

---

## FIXED (incidentally, 2026-07-01 — verified, was stale OPEN entry) — defer body uses a handle the function body then frees → silent UAF (2026-06-15)

**RESOLVED — closed as a SIDE EFFECT of the AU-1 LIFO fix (BUGS-FIXED.md
2026-07-01), not targeted directly.** Verified against current main with the
exact reproducer below: now correctly rejected (`use after free: 'h' is freed`).
The legitimate same-defer `defer { use_item(h); gp.free(h); }` pattern still
compiles (no over-rejection). AU-1's fix (collect all `IR_DEFER_PUSH` in
registration order, process in reverse/LIFO order per return block, checking
each defer's uses against the LIVE path state before applying its frees) means
a single defer's use is checked against `ret_ps`, which already reflects any
main-body free that executed before the return — so this case was subsumed.
Original write-up kept below for history/context.

**Symptom:** a deferred call *uses* a handle (`defer use_item(h);`), the
function body then transitions that handle out of ALIVE (`gp.free(h)`,
`slab.free(h)`, move-consume) before returning; at scope exit the defer
fires on the stale handle. **No diagnostic.** Runtime: pool/slab gen
mismatch traps; move-struct is silent UB.

Reproducer (compiles cleanly when it shouldn't — confirmed on 2026-06-15):
```zer
struct Item { u32 id; }
Pool(Item, 4) gp;
void use_item(Handle(Item) h) { gp.get(h).id = 5; }
void run() {
    Handle(Item) h = gp.alloc() orelse return;
    defer use_item(h); // scheduled use at scope exit
    gp.free(h); // h now FREED in path state
    return; // defer fires use_item(h) on a FREED handle
}
```
The non-defer form `gp.free(h); use_item(h);` IS correctly rejected — only
the defer-scheduled use slips through.

**Root cause:** `zercheck_ir.c` `ir_defer_scan_frees` (~line 1351) walks
each defer body ONLY for free calls (so `defer gp.free(h)` folds into the
exit state correctly). It does NOT scan defer bodies for non-free *uses*
of a handle. The main-body walker sees `gp.free(h); return;` and signs off;
the deferred `use_item(h)` is never checked against the post-body state.

**Why the obvious fix isn't trivial:** defers fire LIFO and a single defer
body can legitimately use-then-free — `defer { use_item(h); gp.free(h); }`
is the canonical safe cleanup and must stay accepted. A naive "scan all
uses then apply all frees" over-rejects it.

**Fix sketch (deferred — net-new analysis pass):** at function exit, walk
defers in LIFO order; for each defer body walk its statements top-down
against a snapshot of the path state — a non-free USE checks the snapshot
(emit use-after-free/use-after-move if FREED/TRANSFERRED), a FREE applies
to the snapshot, then advance. Fold the snapshot into the return state.
Originally found in the 2026-06-07 audit (cool-johnson-InoCW); reproducer
`tests/zer_gaps/audit_2026-06-07_defer_use_after_body_free.zer`.

---

## FIXED (2026-07-01) — BH-18 #1 — move-struct pointer alias defeats ownership/free tracking (CRITICAL soundness) — ALL THREE (1a/1b/1c) CLOSED

**All three manifestations now verified rejected against current main.** 1b closed earlier in
the day (interior-pointer registration + propagation on `&x`); 1a and 1c closed via a 13-site
propagation refactor + a new IR_FIELD_READ registration path — see BUGS-FIXED.md 2026-07-01
("BH-18 #1a + #1c"). Full root-cause + fix detail there; summary:

- **1b (use-after-move stale read via `&x` alias) — FIXED.** Register the move-struct local
  when `&a` is taken + propagate TRANSFERRED to the alloc_id group at the transfer. Tests:
  `tests/zer_fail/move_alias_stale_read.zer`, `tests/zer/move_alias_ok.zer`.
- **1a (heap UAF + slab slot reuse via a move-struct FIELD's pointer copied out) — FIXED.**
  Confirmed NOT move-struct-specific (isolated to a plain `Box`/`*Task` field, no move struct).
  Root cause: a field READ used as an rvalue lowers to its own `IR_FIELD_READ` instruction
  (not `IR_ASSIGN`), so the existing `&b.c` interior-pointer alias logic never saw it. Fix:
  alias registration added to `case IR_FIELD_READ`, gated on an already-tracked compound key
  (sound by construction). Tests: `tests/zer_fail/{move_field_ptr_alias_uaf,
  field_ptr_alias_uaf}.zer`, `tests/zer/field_ptr_alias_safe_ok.zer`.
- **1c (double-consume/double-close via re-dereferencing the alias to resurrect a moved-from
  value) — FIXED.** Root cause: 13 separate sites in zercheck_ir.c set `state = TRANSFERRED`;
  only 1 (the 1b fix site) propagated to aliases. `close_file(f)` — a function-CALL-ARGUMENT
  consume — was one of the 12 silent ones. Fix: extracted `ir_mark_transferred(ps, h, line)`
  (sets state + propagates) and replaced all 13 raw assignments with calls to it — matching
  CLAUDE.md's documented "per-sink patchwork" class and its refactor remedy. Test:
  `tests/zer_fail/move_double_close_via_alias.zer`.

**Sibling gap found during verification — FIXED same session (2026-07-01).** A NESTED
index+field compound alias — `Holder[2] arr; *Task alias = arr[0].p;` — was confirmed live via
targeted reproducer, then root-caused (NOT the write-side registration as first hypothesized;
confirmed via instrumented tracing that the WRITE side registers correctly — `root=0,
path='[0].p'` — but the READ side never reached `IR_FIELD_READ` at all: the nested form lowers
via a DIFFERENT case, `IR_ASSIGN`, retaining the full compound AST rather than decomposing).
Fixed by adding the same registration logic to `case IR_ASSIGN`, gated on
`rhs->kind == NODE_FIELD || NODE_INDEX`. See BUGS-FIXED.md 2026-07-01 ("BH-18 #1a sibling:
nested index+field"). Tests: `tests/zer_fail/nested_index_field_alias_uaf.zer`,
`tests/zer/nested_index_field_alias_ok.zer`.

**Symptom (original, all three manifestations):** a `*T` pointer alias taken
**before** a `move struct` is consumed (or before its owned pointer field is
freed) is never linked to the source's `HS_TRANSFERRED`/`FREED` state. Three
escalating, clean-compiling manifestations — the worst is a genuine **heap
use-after-free with slab slot reuse**.

**1a — heap UAF + slot reuse (memory corruption):**
```zer
struct Task { u32 id; }
Slab(Task) pool;
move struct Owner { *Task p; }
void release(Owner o) { pool.free_ptr(o.p); }
u32 main() {
    Owner o;
    o.p = pool.alloc_ptr() orelse return;
    o.p.id = 100;
    *Task alias = o.p; // raw-ptr alias copied out before the move
    release(o); // moves o AND frees the slab slot (cross-function)
    *Task fresh = pool.alloc_ptr() orelse return; // reuses the just-freed slot
    fresh.id = 222;
    u32 corrupted = alias.id; // reads the REUSED slot -> 222, no trap
    pool.free_ptr(fresh);
    return corrupted;
}
```
Observed: clean compile, `EXIT=222` (reads memory now owned by `fresh`).
`@ptrtoint(alias) == @ptrtoint(fresh)` proves they share one slot. The
`alloc_ptr` raw-pointer path has **no** runtime generation backstop (emitted C
is a bare `alias->id` deref), so it corrupts **silently** — the Handle-field
variant at least traps at runtime (slab gen check, EXIT=133).

**1b — use-after-move stale read** (stack; logical violation, no corruption):
```zer
move struct Tok { u32 kind; }
u32 main() { Tok a; a.kind = 11; *Tok p = &a; Tok b = a; return p.kind; }
```
Observed: clean compile, `EXIT=11` — reads the moved-from value through `p`.

**1c — double-consume / double-close** (re-consumes a unique resource):
```zer
move struct FileHandle { i32 fd; }
i32 g_closes;
void close_file(FileHandle f) { g_closes += 1; }
u32 main() {
    FileHandle f; f.fd = 3;
    *FileHandle alias = &f;
    close_file(f); // first consume
    FileHandle reborn = *alias; // resurrect moved-from f via the alias
    close_file(reborn); // second consume of the SAME fd
    return (u32)g_closes; // EXIT=2 -> double-close
}
```

**Control (proves it is a gap, not design):** every NON-alias form IS caught.
Direct `*Task a = pool.alloc_ptr()...; *Task alias = a; release(a); alias.id`
→ `zercheck: use after free`. Direct `Tok b = a; a.kind` → `use after move:
'a' ... transferred`. Direct `close_file(f); close_file(f)` → `use after
move`. Only routing the post-move read/free through a `*T` alias taken before
the transfer escapes both nets.

**Root cause:** move-transfer marks the source variable `HS_TRANSFERRED`, but
(a) it does not propagate that state to a pre-existing pointer alias
(`*T p = &x`), and (b) for a move-struct *field* the transfer clears/overrides
the owned field allocation's tracking instead of linking the field's `alloc_id`
to its aliases before transfer. ZER already does the equivalent
alias-propagation for Pool/Slab interior pointers (shared `alloc_id`, BUG-488/494)
and the design note in `docs/refactor_ir.md` (~1036-1045) explicitly states
"TRANSFERRED does NOT propagate to aliases" under the assumption "source's
alloc_id has no aliases at the time of transfer" — taking `&f`/`alias = o.p`
before the move **violates that precondition**.

**Fix sketch (track, don't ban):** when `&x` is taken of a move-tracked
variable (or a raw-ptr copy of a move-struct's owned field is made), register
the alias to share `x`'s move/free-state key (declaration-site aliasing, same
pattern as handle `alloc_id`). Then `HS_TRANSFERRED`/`FREED` flows to the alias
and the use/free through it is gated. Suggested tripwire: `tests/zer_fail/`
three reproducers (1a/1b/1c) must each produce a use-after-free/use-after-move
error.

**Distinctness:** NOT BUG-740 (funcptr consume-maybe — caught here), NOT
BUG-742 (conditional global dangle), NOT defer item #9 (no defer, no Handle).

**2026-07-01 status:** the "`&x` taken of a move-tracked variable" half of this
sketch is DONE (closes 1b). The "raw-ptr copy of a move-struct's owned FIELD"
half (closes 1a) and the full-dereference-through-alias gap (closes 1c) are
NOT yet designed — see the PARTIALLY FIXED header above for what's confirmed
still live and why each needs separate investigation before implementing
(different code paths than the 1b fix touched).

---

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #2 — VRP range-narrowing scope leak → unchecked OOB write (CRITICAL soundness)

**ORACLE NOW EXISTS (2026-06-23):** `proofs/operational/lambda_zer_bounds/bounds_lattice.v`
certifies the bounds state set + the sound decision, and `elide_on_join_sound`
pins the exact rule this bug breaks — eliding must use the JOIN of predecessor
ranges, so a branch-local narrowing cannot license elision on a path it doesn't
hold. The durable fix is to write the C against this oracle: wire the orphaned
sound CFG-VRP (`vrp_ir.c`, currently absent from the Makefile) as the sole range
source, which closes this class by construction. The point-fix (save/restore
`var_range_count` on the non-comparison branch) remains the cheap interim.

**Symptom:** a recognized bounds guard (`if (idx >= N) { return; }`) **nested
inside a non-comparison `if` (e.g. `if (b)`)** leaks its range narrowing
(`idx <= N-1`) out to the unconditional path. The compiler then "proves" the
later array index safe and emits it with **no** `_zer_bounds_check` and **no**
auto-guard and **no** warning — but the guard only ran on the `b == true` path.

**Reproducer:**
```zer
u32 main() {
    u32[4] buf;
    buf[0] = 0;
    u32 idx = 0;
    for (u32 k = 0; k < 5; k += 1) { idx += 1; } // idx == 5 (laundered past VRP)
    bool b = false;
    if (b) {
        if (idx >= 4) { return 0; } // guard runs ONLY when b is true
    }
    buf[idx] = 2989; // idx==5 -> OOB write, NO guard emitted
    return buf[0];
}
```
Observed: clean compile (no warning), `--run` EXIT=0 (silent stack corruption).
ASan on the emitted C: `AddressSanitizer: stack-buffer-overflow ... WRITE of
size 4 ... [32,48) 'buf' <== Memory access at offset 52 overflows this
variable`. Expected: auto-guard or `_zer_bounds_check` (exactly what the
plain `buf[idx]` path emits when idx is unprovable), or a "not proven in range"
warning.

**Control (proves it is a scope leak, not guard-trust):**
- baseline (no inner guard) → "index not proven in range — auto-guard inserted", ASan clean.
- outer **comparison** `if (mode == 1) { if (idx>=4) return; }` → guard emitted, ASan clean (this branch saves/restores `saved_range_count`).
- **unconditional** `if (idx>=4) return;` at top level → no guard emitted but genuinely SOUND (ASan clean) — proves ZER does NOT blanket-trust guards; only the nested-non-comparison scope leak is wrong.
- cross-function `pick(5,false)` returning the guarded value → `find_return_range` derives a bogus `[0,3]` and the caller's index is unchecked too (ASan overflow).

**Root cause:** in the checker if-statement VRP handler, the
`/* non-comparison condition — no range narrowing */` branch calls
`check_stmt(then_body)` **without** saving/restoring `c->var_range_count`
around it. The nested guard's inverse-range push (`idx.max = 3`, intended to
"stay valid after the if") therefore persists past the `if (b)` body and is
treated as unconditionally valid.

**Fix sketch:** save/restore `var_range_count` (or scope the pushed ranges)
around the non-comparison branch exactly as the comparison branch already does
with `saved_range_count`. Tripwire: `tests/zer_trap/` — the reproducer must
trap (or `tests/zer/` with the auto-guard warning), never run clean.

---

## FIXED (2026-06-23) — BH-18 #3 — `@bitcast` forges integer↔pointer, bypassing the mmio/inttoptr gate (CRITICAL soundness)

**RESOLVED:** the @bitcast checker handler (checker.c ~7270) now wires the
VST-verified `zer_bitcast_operand_valid` (src/safety/cast_rules.c): it computes
`src_prim`/`dst_prim` (non-pointer-kind, via `type_dispatch_kind` so the
type-dispatch audit isn't tripped) and rejects when the two predicate results
DIFFER — i.e. exactly one operand is a pointer = the int↔ptr forge, pointing at
`@inttoptr`/`@ptrtoint`. Ptr↔ptr (use @ptrcast/@pun) and scalar↔scalar stay allowed.
Verified empirically: int→ptr AND ptr→int forges rejected with the operand error;
`@bitcast(u32, f32)` still compiles+runs; `make check` GREEN (Rust 784/0, Zig 36/0,
fuzz 200, type-dispatch audit OK — no ptr↔ptr regression). Tripwire:
`tests/zer_fail/bitcast_int_ptr.zer`.

**Symptom (was):** `@bitcast(*T, intval)` (and `@bitcast(uN, *T)`) reinterprets an
arbitrary integer as a pointer (and back) with a **clean compile** — no mmio
range check, no alignment check, no `@inttoptr`/`@ptrtoint` gate. On 64-bit a
pointer and `u64` are both 8 bytes, so `@bitcast`'s same-width check passes and
int↔ptr reinterpretation is permitted. Round-tripping through `u64` also
synthesizes the pointer arithmetic ZER explicitly bans.

**Reproducer (silent write through a forged pointer):**
```zer
u32 main() {
    u32 real = 12345;
    u64 raw = @bitcast(u64, &real);
    *u32 p = @bitcast(*u32, raw);
    p[0] = 99; // writes through a forged pointer, no gate
    return real; // EXIT=99
}
```
Observed: clean compile, `EXIT=99`. (Forged offset variant: bitcast `&arr[0]`
to u64, `+= 8`, bitcast back, deref → reads `arr[2]`, the banned `ptr+N`.)

**Control:** the identical conversion via the intended primitive is rejected —
`*u32 p = @inttoptr(*u32, addr);` → `error: @inttoptr requires mmio range
declarations`. And direct `*u32 q = p + 2;` → `error: arithmetic requires
numeric types`. So all three guards (`@inttoptr` mmio, `@ptrtoint`, no-ptr-math)
are enforced and `@bitcast` circumvents all three.

**Why it matters:** ZER claims grammar-level closure — "no in-language unsafe",
every value entering a pointer must cross a typed, mmio-validated boundary.
`@bitcast` is an unguarded escape hatch for the entire mechanism. (Note: the
runtime "ZER TRAP" some inputs hit is just the OS SIGSEGV handler for an
unmapped address — a mapped/in-range forged address reads/writes silently, as
EXIT=99 shows.)

**Root cause:** the `@bitcast` checker handler allows the cast whenever
`src`/`dst` widths match; it does not reject the case where exactly one of
`{src, dst}` is a pointer type.

**Fix sketch:** in the `@bitcast` checker, reject when exactly one operand is a
pointer (point users at `@inttoptr`/`@ptrtoint`, mirroring the existing C-style
`(*T)int` → "use @inttoptr" diagnostic). Pointer↔pointer and scalar↔scalar bit
reinterpretation stay allowed. Tripwire: `tests/zer_fail/bitcast_int_ptr.zer`.

---

## FIXED (2026-06-23) — BH-18 #4 — `@pun(*Struct, *primitive)` silently skips its runtime type_id trap → OOB read (CRITICAL soundness)

**RESOLVED (compile-time, the soundest place):** the @pun checker handler (checker.c
~7224) now rejects a WIDENING pun — when the source pointee and target pointee are
both CONCRETE known-sized (`compute_type_size`) and the target is larger, the pun
reads past the source = OOB, so it's a compile error (better than a skipped runtime
trap). An OPAQUE/unknown source pointee (the cinclude/FFI floor, e.g.
`@pun(*Sensor, *opaque)`) is EXCLUDED — it keeps the runtime type_id guard
(`type_dispatch_kind(eff->pointer.inner) != TYPE_OPAQUE` + `src_sz > 0`). Verified
empirically: `@pun(*Big, *u32)` (16←4) rejected with the OOB error; `pun_from_opaque`
(the FFI-floor positive test) still compiles + runs; `make check` GREEN (Rust 784/0 on
re-run — the one-off `rc_cond_004` failure was a pre-existing FLAKY concurrency test
[4/5], unrelated: it uses no `@pun`). Tripwire: `tests/zer_fail/pun_primitive_to_struct.zer`.

**Symptom (was):** `@pun`'s documented guarantee is "runtime type_id check that traps
on mismatch." A fully-typed in-ZER pointer to a **primitive** (`*u32`, `*u8`,
a slice `.ptr`, an `@inttoptr` result) packs `type_id == 0`, and the emitted
guard `if (type_id != TARGET && type_id != 0) trap;` short-circuits to false —
so the trap is skipped even when the sizes plainly mismatch.

**Reproducer:**
```zer
struct Big { u64 a; u64 b; }
u32 main() {
    u32 small = 7;
    *u32 sp = &small;
    *Big bp = @pun(*Big, sp); // 4-byte target punned to 16-byte struct, no trap
    return (u32)bp.b; // reads offset 8, past the 4-byte 'small'
}
```
Observed: clean compile, runtime garbage; ASan: `stack-buffer-overflow ... READ
of size 8 ... underflows this variable`. The emitted guard is
`(_zer_opaque){(void*)(sp), 0}; if (0 != 1 && 0 != 0) _zer_trap(...)` →
`(true && false)` → trap skipped.

**Control (proves the bypass is type_id==0-specific):** `@pun` between two
**struct** types (both type_ids nonzero) correctly traps:
`ZER TRAP: @pun type mismatch` / EXIT=133. And `@ptrcast(*Big, *u32)` is
correctly compile-rejected ("type confusion — use @pun"). Only `@pun` with a
primitive/slice source slips through.

**Root cause:** the `type_id == 0` escape clause in the `@pun` runtime guard was
intended for genuinely-unknown-provenance pointers (cinclude/extern `*opaque`),
but in-ZER primitive/slice-element pointers also carry `type_id == 0`.

**Distinctness:** This is the WORKS-AS-DESIGNED note "type_id=0 (cinclude)
skipping the **@ptrcast** check" applied to the **wrong** intrinsic and the
wrong source — it is `@pun` (not `@ptrcast`, which here compile-rejects) and an
**in-program** primitive pointer (not a cinclude `*opaque`).

**Fix sketch:** in the `@pun` lowering, do not apply the `!= 0` escape when the
source is a fully-typed in-ZER primitive/slice pointer; OR add a compile-time
size-widening check at the `@pun` site (the 4-vs-16-byte mismatch is statically
known). Tripwire: `tests/zer_fail/pun_primitive_to_struct.zer`.

---

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #5 — fixed-array bare-call index drops the bounds check (CRITICAL soundness)

**Symptom:** indexing a fixed-size array by a **bare function call**
(`a[idx()]`) emits a raw C subscript with **neither** the auto-guard (used for
simple variable indices) **nor** the `_zer_bounds_check` wrapper (used for
arithmetic indices and slices). This is the documented BUG-595..612
emission-diff class: a single-eval path for the side-effecting index never got
the bounds wrapper that `emit_expr` applies elsewhere.

**Reproducer:**
```zer
u32 g = 100;
u32 idx() { return g; }
u32 main() {
    u32[4] a;
    a[idx()] = 999; // idx()==100 -> OOB write into a u32[4], no trap
    return 5;
}
```
Observed: clean compile (no warning), `EXIT=5` (no trap). Emitted main body:
`_zer_t0 = a[idx()] = 999;` — bare subscript. Also reproduces for read
(`v = a[idx()]`) and compound (`a[idx()] <<= 1` — the `_zer_shl` shift guard is
kept but the bounds check is still dropped).

**Control (proves VRP is NOT proving it safe, the check was dropped):** the
SAME OOB value through any other index shape traps or auto-guards —
`a[idx()+0]` → `ZER TRAP: array index out of bounds` (EXIT=133); slice
`s[idx()]` (`[*]u32 s = a;`) → same trap; simple `u32 i = g; a[i]` → "not
proven in range — auto-guard inserted". Wrapping the call in trivial arithmetic
re-routes it through the guarded complex-expression path.

**Root cause:** the emitter path that single-evaluates a side-effecting index
for a **fixed array** (so `idx()` is called once) omits the bounds wrapper.
Slices go through a different, correctly-guarded path.

**Fix sketch:** port the `_zer_bounds_check` / auto-guard emission to the
fixed-array side-effecting-index single-eval path (mirror the slice path / the
`a[idx()+0]` complex-expression path). Run the BH audit grep before committing:
`grep -nE "_zer_trap|_zer_bounds_check|_zer_shl" emitter.c` — every AST-region
match needs an IR-path equivalent. Tripwire: `tests/zer_trap/array_call_index_oob.zer`.

---

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #6 — `if(opt)|*v|` capture escapes a pointer-to-local to a global (CRITICAL soundness)

**ORACLE NOW EXISTS (2026-06-23):** `proofs/operational/lambda_zer_capture/capture_lattice.v`
certifies the rule — a capture INHERITS the payload's region
(`capture_preserves_escape`), and `buggy_reset_unsound` witnesses this exact bug
(the capture defaulting to STATIC) as a soundness violation. The fix is to make
the `|v|`/`|*v|` desugaring set the capture's escape-provenance from the matched
value's region (don't reset), and add the tripwire — now verifiable against the
oracle.

**Symptom:** `if (opt) |*v| { ... }` binds `v = &m.value` — a pointer **into**
the local optional `m`. Storing `v` into a global is a dangling-pointer escape,
but the capture-desugared `v` does not carry local-derived provenance, so the
escape check is bypassed. After the function returns, the global points at dead
stack.

**Reproducer (scalar):**
```zer
?*u32 g = null;
void stash() {
    ?u32 m = 5;
    if (m) |*v| { g = v; } // pointer-to-local m escapes to global g
}
u32 main() { stash(); if (g) |gv| { return gv[0]; } return 0; }
```
Observed: clean compile (only a benign bounds-check warning), runtime returns
dead-stack garbage (varies; inserting a clobber between `stash()` and the read
changes the value — proving it reads a reclaimed frame, not the stored 5).
The struct variant (`?P` with `P { u32 x; }`) reproduces identically.

**Control:** the syntactically-direct form IS rejected —
`void stash(){ ?u32 m=5; g=&m.value; }` → `error: cannot store pointer to local
'm' in static/global variable 'g'`. The escape analysis exists and fires for
`&m.value` written by hand; it only misses the same address synthesized by the
`|*v|` capture binding.

**Root cause:** the `|*v|` capture desugars to a synthesized
`v = &m.value` whose result does not inherit the `is_local_derived` escape
provenance, so System-11 scope-escape analysis treats `g = v` as a normal
global store.

**Fix sketch:** mark the capture binding `v` from `|*v|` as local-derived
(pointer into the local optional's storage), so the existing escape check fires
on `g = v` exactly as it does for `g = &m.value`. Tripwire:
`tests/zer_fail/capture_ptr_escape_global.zer` (scalar + struct).

**Distinctness:** NOT BUG-742 (that is a freed-heap MAYBE_FREED-at-exit case);
this is an unconditional stack-local-address escape, caught in every direct
form, missed only through `|*v|` capture desugaring.

---

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #7 — shared multi-access via cast/intrinsic/index/orelse subexpr (HIGH race)

**Symptom:** reading a shared-struct field inside a `(T)cast`, `@intrinsic(...)`,
array index, or `orelse` subexpression — while assigning another shared
struct's field in the same statement — compiles clean. The shared-type
collector does not recurse into those node kinds, so it sees only ONE shared
type and stays silent; the emitter (lock-per-statement) then locks one struct
and reads the other **unlocked**.

**Reproducer:**
```zer
shared struct A { u32 x; }
shared struct B { u32 y; }
A a; B b;
void f(*A pa, *B pb) { pa.x = (u32)pb.y; } // reads B's field under only A's lock
u32 main() { return 0; }
```
Emitted `f()`:
```c
void f(struct A* pa, struct B* pb) {
    pthread_mutex_lock(&pa->_zer_mtx);
    _zer_t0 = pa->x = ((uint32_t)pb->y); // <-- reads pb->y of struct B with NO B lock
    pthread_mutex_unlock(&pa->_zer_mtx);
}
```
ThreadSanitizer confirms a real read/write race on `b` (the two threads hold
different mutexes M0=A, M1=B). Also reproduces via `@bitcast(u32, pb.y)` and
`pb.arr[pb.idx]`.

**Control:** the plain binary form `pa.x = pb.y;` IS rejected —
`error: deadlock: single statement accesses both 'A' (order 1) and 'B'
(order 2)`. Only wrapping one access in a cast/intrinsic/index/orelse evades it.

**Root cause:** `collect_shared_types_in_expr` (checker.c, ~14597) recurses
into `NODE_BINARY`/`NODE_ASSIGN`/`NODE_UNARY`/CALL-args but NOT into
`NODE_CAST`/`NODE_TYPECAST`/`NODE_INTRINSIC`/`NODE_INDEX` (the index
sub-expression)/`NODE_ORELSE`.

**Fix sketch:** add those node kinds to the collector's recursion so the
deadlock/multi-lock check sees both shared types (then the binary-form error
fires for the cast form too). Tripwire: `tests/zer_fail/shared_cast_subexpr.zer`.

---

## FIXED (2026-07-01, copied from cool-johnson-11ct36) — BH-18 #8 — `spawn` data-race scan is blind to function-pointer indirection → data race (HIGH race)

**RESOLVED.** Verified with the exact reproducer below against current main:
now correctly rejected (`error: spawn target 'worker' accesses non-shared
global 'g_counter' — data race`). Fix: `scan_unsafe_global_access` (checker.c)
now follows every `NODE_IDENT` call argument that resolves to a function
symbol, descending into its body the same way it descends into a direct
callee (shared `_scan_depth` cap 32). See BUGS-FIXED.md 2026-07-01
("Branch-import Tier 1"). Original write-up kept below for history.

**Symptom:** the spawn non-shared-global scan follows only **direct** calls. A
call through a function pointer (a `*()` param `cb()`, or a local
`*() fp = do_increment; fp()`) is invisible, so a non-shared global mutated in
the indirectly-reached callee is never flagged.

**Reproducer:**
```zer
u32 g_counter;
void do_increment() { g_counter = g_counter + 1; }
void run_n(*() cb, u32 n) { for (u32 i = 0; i < n; i += 1) { cb(); } }
void worker() { run_n(do_increment, 500000); }
u32 main() { spawn worker(); spawn worker(); return 0; }
```
Observed: clean compile (only a "stack depth not verifiable" warning), no
data-race error. TSan confirms a read+write race on `g_counter`.

**Control:** the direct-call form `void worker() { do_increment(); }` IS
rejected — `error: spawn target 'worker' accesses non-shared global
'g_counter' — data race`. Direct calls are even transitive (multi-level), so
the indirect miss is a genuine escape, not a depth limit. Contradicts the
limitations.md "spawn non-shared global (direct + transitive) — 0 holes"
claim, which tested direct-call depth only.

**Root cause:** `scan_unsafe_global_access` (checker.c, ~8491) only descends
`NODE_CALL` with a `NODE_IDENT` callee resolvable to a function symbol; it skips
funcptr call sites entirely instead of treating an unresolvable indirect call
conservatively.

**Fix sketch:** apply the BUG-740 argument-precise-barrier discipline — an
unresolvable indirect call inside a spawn target should widen to a possible-race
(error/conservative), not be silently skipped. (A trivial intraprocedural
resolution would even catch the `fp = do_increment; fp()` local case.) Tripwire:
`tests/zer_fail/spawn_funcptr_global_race.zer`.

---

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #9 — shared-struct read in `await` condition emitted unlocked (HIGH race)

**Symptom:** accessing a `shared struct` field in an `await` condition compiles
clean and emits an **unlocked** read, violating both the D02 "no shared access
in a yield/await statement" ban and the "shared struct = auto-locked" guarantee.

**Reproducer:**
```zer
shared struct Flag { u32 ready; u32 data; }
Flag g;
async void waiter() {
    await g.ready > 0; // shared read in await cond -> D02 should reject, doesn't
    g.data = g.ready; // (this one IS properly mutex-wrapped)
}
```
Emitted poll: `case 1:; if (!((g.ready > 0))) { self->_zer_state = 1; return 0; }`
— `g.ready` read with **no** `pthread_mutex_lock(&g._zer_mtx)`, while every
other access to `g` in the program is mutex-wrapped. The await condition is
re-evaluated on every poll while suspended; a concurrent `spawn setter()` (whose
write IS locked) races it. Pointer form `*Flag p = &g; await p.ready > 0;` also
slips through.

**Root cause:** the D02 ban (checker.c, ~5722) is gated on
`c->in_async_yield_stmt`, which is set only for `NODE_EXPR_STMT` and
`NODE_VAR_DECL` whose expression contains yield (checker.c, ~8677-8681). A bare
`await cond;` is a `NODE_AWAIT` statement (checker.c, ~11717) — neither node
kind — so the flag is never set. Because `yield`/`await` are statements (not
expressions), the await condition is the only realistic way to have a shared
access inside a suspending statement, so the unguarded path is exactly the
reachable one.

**Fix sketch:** one-line parity — set `c->in_async_yield_stmt` around the
`check_expr` of the `await` condition in the `NODE_AWAIT` handler (or detect
`NODE_AWAIT` in `check_stmt` like the other two node kinds). Tripwire:
`tests/zer_fail/await_shared_unlocked.zer`.

---

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #10 — value-returning `async` never finalizes its state machine (MEDIUM miscompile)

**Symptom:** `async u32` / `async ?u32` (any `return <value>;` in an async body)
never sets `self->_zer_state = -1` on completion and returns the user value
instead of the poll done-flag. Result: (1) the coroutine tail **re-executes on
every subsequent poll** (re-runs side effects), and (2) `while(poll()==0)`
breaks because the user value is indistinguishable from the "not done" flag.

**Reproducer:**
```zer
u32 side;
async u32 compute() { yield; side += 1000; return 42; }
u32 main() {
    side = 0;
    _zer_async_compute task;
    _zer_async_compute_init(&task);
    _zer_async_compute_poll(&task); // poll 1: yield
    _zer_async_compute_poll(&task); // poll 2: completes, side += 1000
    _zer_async_compute_poll(&task); // poll 3: should be no-op...
    _zer_async_compute_poll(&task); // poll 4: ...but re-runs the tail
    return side / 1000; // EXIT=3 (tail ran 3x); expected 1
}
```
Observed: `EXIT=3`. Emitted completion block: `... side += ...; self->_zer_t1 =
(uint32_t)42; return self->_zer_t1;` — no `self->_zer_state = -1`. With
`return 0;` the canonical `while(poll()==0)` loop infinite-loops.

**Control:** `async void` (same body) correctly emits `self->_zer_state = -1;
return 1;` and is idempotent → `EXIT=1`.

**Root cause:** emitter `IR_RETURN` handler (emitter.c, ~9413): the `is_async`
finalization (`self->_zer_state = -1; return 1;`) lives only in the void/bare
return branch (~9466-9468). The value-return branch (~9456-9463) never checks
`func->is_async`. BUG-509 fixed the void path only; the value path was never
fixed. The checker accepts `async <non-void>` without rejection.

**Fix sketch:** in the value-return branch, when `func->is_async`, emit
`self->_zer_state = -1;` before returning and reconcile the poll protocol (the
poll signature is `int` done-flag; a value-returning async needs a separate
value-retrieval mechanism, not a return-value overload). Tripwire:
`tests/zer/async_value_return_idempotent.zer` (or reject `async <non-void>`
until a real value-retrieval API exists).

---

## FIXED (verified 2026-07-01 — was stale OPEN entry) — BH-18 #11 — bit-query/byte-swap intrinsics emit `0` in global initializers (MEDIUM miscompile)

**RESOLVED.** Verified with the exact reproducer below against current main:
`u32 g = @popcount(255); u32 main() { return g; }` now returns `8` (was `0`).
The AST `NODE_INTRINSIC` emitter path now emits the GCC builtins for all 9
bit-query/byte-swap intrinsics in global initializers. Fixed in an untracked
prior session (sesjma's 2026-06-29 audit independently reconfirmed this same
fix on a different code path — see that entry's note below); this entry's
write-up predates it and was never removed. Original write-up kept for history.

**Symptom:** `@popcount`/`@ctz`/`@clz`/`@ffs`/`@parity`/`@bswap16`/`@bswap32`/
`@bswap64` used in a **global variable initializer** silently emit
`/* @X — unknown */0`. Clean compile, wrong value (and wrong control flow when
the global feeds a comparison).

**Reproducer:**
```zer
u32 g = @popcount(255); // emitted: uint32_t g = /* @popcount — unknown */0;
u32 main() { return g; } // EXIT=0 ; expected 8
```
Observed: `g == 0`. `@bswap32(1)` global → 0 (expected 16777216), and an
`if (g == 16777216)` then wrongly takes the false branch.

**Control:** the SAME intrinsic in a **function body** is correct
(`u32 x = @popcount(255);` → 8). And `@truncate`/`@bitcast`/`@size` ARE handled
in the global/AST path — so it is specifically these 9 intrinsics missing from
that path, not a general "no intrinsics in globals" limit.

**Root cause:** the documented two-emitter-path gotcha. The IR-rewritten path
(emitter.c, ~6561) handles popcount/ctz/clz/ffs/parity/bswap*, but the AST path
(`NODE_INTRINSIC`, emitter.c, ~2765 — used for global initializers) does not,
and falls through to the `/* @%.*s — unknown */0` placeholder. The checker
accepts the global because these return `u32` (type-checks fine).

**Fix sketch:** add handlers for the 9 bit-query/byte-swap intrinsics to the AST
`NODE_INTRINSIC` emitter path (mirror the IR path / the existing
`@truncate`/`@bitcast` AST handlers). Verify both paths with
`grep -n '"popcount"' emitter.c` returning TWO hits. Tripwire:
`tests/zer/bitquery_global_init.zer`.

---

## FIXED (2026-07-01) — BH-18 #12 — `defer` + backward `goto` fires the wrong count (MEDIUM miscompile; folds into known item "defer fires twice")

**RESOLVED.** Verified with the exact reproducer below against current main:
`counter` is now `1` (was `2`, parametric with back-edge count). Fix: per-label
`defer_count_at_def` recorded when `NODE_LABEL` is processed (= defers
registered before the label); a BACKWARD goto (target already defined) fires
only defers registered AFTER the label (loop-body defers), leaving pre-label
defers pending for the real exit. Forward gotos unchanged (base 0 + the
existing guard machinery). Verified no regression: a loop-BODY defer still
fires per-iteration. See BUGS-FIXED.md 2026-07-01
("BH-18 #12: defer fired N-times..."). Original write-up kept below.

**Symptom:** a function-scope `defer` is lowered onto the backward-`goto`
**back-edge** block instead of the real exit paths, so it fires once **per
back-edge traversal** — N times for N traversals, **0** times when the back-edge
is never taken (silent skipped cleanup / leak), and never at the true return.

**Reproducer:**
```zer
u32 counter;
void inc() { counter += 1; }
void run() {
    u32 i = 0;
    defer inc(); // registered once, before the label
    loop:
    i += 1;
    if (i < 3) { goto loop; } // 2 back-edges
}
u32 main() { run(); return counter; } // EXIT=2 ; expected 1
```
Observed: `EXIT=2`. Parametric: bound `i<1`→0 fires (cleanup skipped), `i<2`→1,
`i<5`→4. With `defer pool.free_ptr(h)` this becomes a clean-compiling
double-free; with the back-edge never taken it becomes a leak.

**Control:** the structured-loop analog (`while (i<3) { i += 1; }`, same defer)
correctly fires once → `EXIT=1`.

**Root cause:** ir_lower defer lowering attaches the function-scope defer to the
same-scope `goto` back-edge block rather than to all real exit paths.

**Distinctness / honesty note:** this is the **same defect** as the existing
OPEN item "defer fires twice when a goto target sits in the same defer scope" —
this entry is a sharper characterization (parametric fire count, plus the
0-fire leak and the double-free escalation), not a wholly new bug. Track it as
an escalation of that item, not a separate fix. Tripwire (shared with that
item): the goto-loop defer must fire exactly once.

---

## FIXED (verified 2026-07-01 — was stale OPEN entry) — BH-18 #13 — nested inline designated initializer rejected ("got void") (LOW false-reject)

**RESOLVED.** Verified with the exact reproducer below against current main:
`Outer o = { .pos = { .x = 3, .y = 4 }, .id = 9 };` now compiles and runs
(`EXIT=16`, was a compile error). `validate_struct_init` now recurses when a
field value is itself a `NODE_STRUCT_INIT`. Fixed in an untracked prior
session (11ct36's audit independently listed this as a stale-closed entry);
this write-up predates the fix and was never removed. Original write-up kept
for history.

**Symptom:** a designated initializer whose field value is itself an inline
brace literal is rejected — the inner literal is typed `void` because the
field's expected type isn't threaded into it. Reproduces in all three
value-flow sites (var-decl init, assignment, return).

**Reproducer:**
```zer
struct Inner { u32 x; u32 y; }
struct Outer { Inner pos; u32 id; }
u32 main() {
    Outer o = { .pos = { .x = 3, .y = 4 }, .id = 9 }; // error: field '.pos' expects 'Inner', got 'void'
    return o.pos.x + o.pos.y + o.id;
}
```
Observed: `error: field '.pos' expects 'Inner', got 'void'`.

**Control (proves the data model + emitter are fine):** hoisting the inner
literal to a named var compiles and runs — `Inner i = { .x = 3, .y = 4 };
Outer o = { .pos = i, .id = 9 };` → `EXIT=16`.

**Root cause:** `validate_struct_init` (checker.c, ~1194-1229) calls
`checker_get_type(df->value)` on each field value but does not recurse when
`df->value` is itself a `NODE_STRUCT_INIT`, so the inner literal never gets
validated against the field's declared type and types as `void`.

**Fix sketch:** in `validate_struct_init`, when `df->value->kind ==
NODE_STRUCT_INIT`, recurse `validate_struct_init(c, df->value, field_type,
line)` instead of comparing `checker_get_type` against the field type. (The
array-field variant `{ .data = { 10, 20, 30 } }` is a SEPARATE missing feature
— ZER has no bare-aggregate array-literal syntax at all; don't conflate.)
Tripwire: `tests/zer/nested_designated_init.zer`.

---

## FIXED (2026-07-01, copied from cool-johnson-11ct36) — BH-18 #14 — conversion-intrinsic arity is not validated (LOW diagnostic)

**Symptom:** the conversion/layout intrinsic family (`@truncate`, `@bitcast`,
`@saturate`, `@cast`, `@inttoptr`, `@ptrcast`, `@size`) does not validate
argument count: a **missing value operand** passes the checker and emits invalid
C (GCC then errors on a non-existent source line), and **excess trailing args**
are silently dropped.

**Reproducers:**
```zer
u32 main() { u8 x = @truncate(u8, 5, 6, 7); return (u32)x; } // EXIT=5 — args 6,7 silently dropped
```
```zer
u32 main() { u8 x = @truncate(u8); return 0; } // checker accepts; emits (uint8_t)(); GCC: "expected expression before ')'"
```
Observed: extra-args case compiles clean (`(uint8_t)(5)` emitted, EXIT=5);
missing-operand case passes the ZER checker (`--emit-c` exits 0) and only GCC
complains, mis-attributed to the wrong line.

**Control:** a normal user function enforces arity —
`add(1,2,3)` for a 2-param `add` → `error: expected 2 arguments, got 3`. And
`@atomic_load()` gives a clean checker error `@atomic_load requires 1
argument`. So the front-end has the mechanism; the conversion-intrinsic family
just doesn't apply it.

**Why it's diagnostic, not a soundness hole:** these programs never produce a
running binary, so there's no memory unsafety — the harm is silently-masked
typos (extra args) and a wrong-line/wrong-stage error (missing arg).

**Fix sketch:** add an exact-arity check to the conversion/layout intrinsic
checker handlers (reject too-few AND too-many), matching `@atomic_load`'s
"requires N argument" pattern. Tripwire: `tests/zer_fail/intrinsic_arity.zer`.

**RESOLVED.** Verified with both exact reproducers above against current main:
extra-args (`@truncate(u8, 5, 6, 7)`) and missing-operand (`@truncate(u8)`) are
BOTH now rejected at the ZER checker level (not GCC). The arity block was
restructured — family identification unconditional, then "requires a type
argument" split from "expects N args after type"; the `@size(NamedType)` parse
path (BUG-316) preserved via `size_named_path`. See BUGS-FIXED.md 2026-07-01
("Branch-import Tier 1").

---

## CLOSED — keep transitivity through a function-POINTER forward (2026-06-19)

Initially left open, then **closed the same day** (option B). `keep_edge_propagates`
now worst-cases EVERY pointer param of a funcptr call (it just calls
`keep_edge_callee_keeps`), so forwarding a param to ANY funcptr — direct funcptr
param, global funcptr, or stored callback — infers keep on the forwarded param.
A stack pointer therefore can never reach a retaining callback via a forwarder;
`invoke(&local, retaining_cb)` is rejected. This makes a *forward through* a
funcptr consistent with a *direct* funcptr call (`fn(&local)` was already
rejected), closing the funcptr-forwarding hole to **100% soundness** for the
keep/escape property.

**Cost (measured, tiny):** a read-only callback called with a STACK-local context
is now also rejected (`compute(&local_ctx, reader)`) — use a long-lived context.
This restricts exactly ONE pattern across the whole suite
(`rust_tests/rt_opaque_provenance_chain.zer`, updated to a global context). The
precise alternative (resolve the concrete callback's inferred keep per call site
via a forwarding summary — preserves the read-only-stack-local idiom) was judged
not worth ~150 lines to save one rare pattern; revisit if it bites in practice.

---

## CLOSED — "semantic-fuzzer flake / expr-nesting-too-deep" was a STALE corrupt `.o`, not a code bug (2026-06-19)

**This corrects a misdiagnosis.** A prior note here blamed an "uninitialized-read
UB" for the `make zerc` build spuriously rejecting trivial programs with
`error: expression nesting too deep (limit 1000)` (semantic fuzzer ~165/200).
That was WRONG — the compiler source is fine.

**Real root cause:** a stale, MISCOMPILED `src/safety/comptime_rules.o` left in
the working tree (dated 2026-06-06, gitignored). In that object,
`zer_expr_nesting_valid` read its argument from register `%ecx` instead of
`%edi` (wrong calling convention), so it received stale garbage instead of the
nesting depth and rejected ~80% of programs. The `.c` source is OLDER than the
bad `.o`, so `make` saw the object as up-to-date and never rebuilt it. The
corrupt object almost certainly came from an OOM-interrupted / corrupted build
on 2026-06-06.

**Why it looked layout/`vrp_ir`-dependent:** any build path that RECOMPILES
`comptime_rules.c` — a single-`gcc` invocation, the `vrp_ir`-linked dev build, a
fresh `git archive` checkout, normal CI — produced a correct object (reads
`%edi`) → 200/200. Only builds that REUSED the stale object failed. "Baseline
fails identically" was true precisely because baseline reused the SAME stale
object.

**Fix (done):** (1) `rm -f *.o src/safety/*.o` clears the corrupt object;
(2) the Makefile `clean:` target now removes `src/safety/*.o` (it previously
removed only top-level `*.o`, which let the bad object survive every
`make clean`). Verified: fresh `make zerc` → `zer_expr_nesting_valid` reads
`%edi`, semantic fuzzer 200/200, full `make check` green.

**Lesson for future sessions:** if a `make`-built `zerc` spuriously rejects
trivial programs but a `git archive` / single-`gcc` build of the SAME source
passes, suspect a stale `src/safety/*.o` FIRST — `objdump -d src/safety/<x>.o`
and check which register the function reads its argument from. Don't chase a
Heisenbug in the source.

---

## CLOSED — 2026-06-17 audit (from plt86m branch): ALL 9 gaps FIXED

Nine gaps documented on branch `claude/cool-johnson-plt86m`, INDEPENDENTLY
re-verified real (each reproduced on baseline). **ALL 9 are now FIXED**
(2026-06-19d/e + 2026-06-20). Each has a `tests/zer_fail/` tripwire (and a
`tests/zer/` positive guard where relevant); see BUGS-FIXED.md.

**FIXED 2026-06-20 — `defer_goto_fallthrough_drops` (was HIGH miscompile):** a
defer scope with both a `goto` exit and a sibling fall-through exit dropped the
defer body on the fall-through path (shared `defer_stack` consumed by the
goto-path fire in block-ID order). Fixed via **capture-on-FIRE + a per-label
runtime "fired" flag** (ir.h + ir_lower.c + emitter.c). Each `IR_DEFER_FIRE`
carries its own live-defer snapshot (order-independent emission, fixes the
drop). A goto-only cleanup label resets defer_count; a both-reachable cleanup
label installs a runtime guard — the goto sets a bool flag and the label's
return-fire emits each goto-fired body as `if (!flag) { body }`, so the goto
path skips (fired eagerly) and the fall-through path fires AT the return after
eval (BUG-442 preserved). Three earlier shapes (plain capture-on-FIRE; +reset;
+fall-through-edge-fire) were each insufficient and are documented in
BUGS-FIXED.md "Session 2026-06-20" so they are not re-tried. Tests:
`tests/zer/defer_goto_{fallthrough_fires,both_reachable,return_reads_deferred}.zer`.
Architecture: docs/compiler-internals.md "capture-on-FIRE + runtime-flag defer
emission". (The earlier "needs CFG defer-liveness dataflow" pessimism was wrong
— a per-label runtime flag suffices.)

**FIXED 2026-06-19e (5 gaps):**
- `container_const_strip` — reject a `TYNODE_CONST`/`TYNODE_VOLATILE` container
  type arg (checker.c TYNODE_CONTAINER); the qualifier was dropped at
  monomorphization. `tests/zer_fail/container_const_type_arg.zer`.
- `mmio_range_ignores_size` — the @inttoptr range gate now requires the whole
  span `[addr, addr+sizeof(T)-1]` to fit (checker.c constant path + both emitter
  variable-address runtime traps). `tests/zer_fail/inttoptr_size_past_range.zer`
  + `tests/zer/inttoptr_u8_at_range_end.zer`.
- `var_index_move_array` — a variable-index move from a move-struct array is now a
  hard error (zercheck_ir.c). `tests/zer_fail/move_array_var_index.zer`.
- `defer_use_after_alloc_ptr`, `defer_use_after_move` — new `ir_defer_scan_uses`
  walker checks defer-body USES against the pristine exit state.
  `tests/zer_fail/defer_use_after_{alloc_ptr,move}.zer` +
  `tests/zer/defer_free_pattern_ok.zer`.
- x9-11 completeness — a non-constant bit-query intrinsic arg in a global
  initializer is now a clean ZER error (checker.c NODE_GLOBAL_VAR init check,
  NODE_FIELD-guarded for enum constants). `tests/zer_fail/global_init_nonconst_intrinsic.zer`.

**FIXED 2026-06-19d (Theme B, 3 distinct-const sites):** var-decl-init,
assignment, and call-arg const guards now use `type_dispatch_kind` +
`type_unwrap_distinct` (+ a symbol-level check, since `const MyPtr` stores
`const` on the SYMBOL). `tests/zer_fail/distinct_const_{var_decl,param,slice_param}_launder.zer`.

Note `container_const_strip` was tagged "KNOWN/WAD-maybe" in the original audit;
on verification the plain-const variant IS rejected while the container variant
silently dropped const — a real asymmetry, so it was fixed (reject), not waived.

---
