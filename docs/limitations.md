# ZER Compiler — Known Limitations

Living document of known compiler limitations, audit findings, and deferred fixes.
Entries removed once fixed.

---

# SESSION 2026-08-27b — fresh audit (BUG-913..918), six findings

Not a harvest. All nine `vigilant-tesla` branches are consumed and all three harvest
trackers are closed (see the HANDOFF below, still accurate). This was a probe-driven audit
of the intrinsic surface. **`make check` exit 0**, ten gates including the new
`audit_float_literal.sh`.

Three silent safety holes, all the same shape — **an intrinsic advertises a check that is
not emitted for certain operand types**:

| bug | what was silent | consequence measured on main |
|---|---|---|
| BUG-916 | `@pun`'s runtime `type_id` trap is absent whenever either pointee is a primitive/slice/funcptr (only struct/enum/union carry an id, everything else packs 0 and the comparison folds to false) | an integer became a **working pointer with no `@inttoptr` and no `mmio`** — rc=42 writing through it. Also forged enum / bool / funcptr / slice-`len`. Hosted a wild address hits the SIGSEGV handler, which is `_ZER_HOSTED`-only, so **bare metal is a silent wild access** |
| BUG-917 | `@inttoptr(*State, addr)` — the FOURTH enum-forging door, in a set documented as closed at three | switch **returned 3** on a register holding 200; zero guard emissions in the generated C |
| BUG-915 | `@container` had a two-valued provenance domain for a three-valued fact; `&wholeObject` fell into "unknown, allow" | ASan **stack-buffer-underflow** (global source: global-buffer-underflow), no diagnostic, no trap |
| BUG-918 | the ROOT of BUG-916 — erasing a non-aggregate pointer to `*opaque` recorded `type_id = 0`, which means "unknown origin, a C pointer we cannot vouch for" rather than "no id available" | `@ptrcast(*Big, opaque_from_u32ptr)` **RAN**; ASan stack-buffer-overflow on the 16-byte read of a 4-byte object. The same program with a `*Sensor` origin traps correctly — two spellings disagreeing |

Plus two defects where the COMPILER'S OWN OUTPUT is invalid C, so the user sees a GCC
error against their own `.zer` line and no ZER diagnostic ever names the cause: a
non-finite float literal emitted as the bare token `inf` (BUG-913, five sites, now one
helper + a gate), and the float-to-int saturation guard emitted as a statement expression
at file scope (BUG-914).

**Method note worth keeping.** Every one of the six was found by PROBING the intrinsic
surface for "what does this actually do", not by reading for suspicious code. Three of the
five were sitting next to a comment that already described the hazard — BH-18 #4's own
text says *"the runtime type_id trap is skipped for an in-ZER primitive pointer ... so the
OOB is SILENT"*, and it fixed only the out-of-bounds half. **When a fix's rationale
describes a mechanism as broken, check whether the fix covered every consequence of that
mechanism or only the one that was reported.** BUG-918 came from a second habit worth
keeping: **after fixing one intrinsic, run the same probe against its siblings.** `@pun`
and `@ptrcast` share the `_zer_opaque` mechanism, and the sibling turned out to hold the
root cause.

**A sentinel that means two things is a hole waiting to happen.** `type_id == 0` carried
both "no id available for this kind" and "unknown origin — the FFI floor". The first is a
compiler limitation; the second is a positive claim that disables a check. Twelve sites
produced the first while the single reader interpreted it as the second. When a fix
introduces a sentinel, write down which of those it is.

## OPEN — `@inttoptr` to a pointer-carrying (not enum-carrying) pointee (LOW, unmeasured)

BUG-917 rejects `@inttoptr` to a type that carries an ENUM, because an exhaustive switch
downstream *elides work* on the assumption that every value is a declared variant, and that
elision was measured turning a bad value into a wrong dispatch.

The same intrinsic can produce a pointer to a struct carrying a `*T`, `[*]T`, `bool`,
optional or `Handle`, and reading those fields forges those values from hardware bits. That
was NOT shipped, deliberately:

- `@inttoptr` **is** the sanctioned integer-to-pointer door (mmio-gated and audit-visible),
  so a hardware register holding an address is the thing it exists to express;
- `lib/compat.zer` depends on `@inttoptr(*opaque, ...)` for its pointer arithmetic, so the
  blanket predicate (`type_carries_forgeable`) has a non-zero corpus cost;
- no wrong-dispatch or wrong-elision defect has been measured for these, unlike the enum
  case.

If this is taken up, measure first: find a downstream analysis that ELIDES a check on the
strength of one of these types, the way the exhaustive switch does for enums. Absent that,
this is an unmeasured tightening and should stay unshipped.

## NOTE — the exhaustive-enum switch's last-arm elision is the amplifier, not the hole

Worth writing down because it explains why every enum-door bug reads as severe. Lowering an
exhaustive `switch` emits the final arm as an **unconditional else**:

```
if (s == 0) -> arm0; else if (s == 1) -> arm1; else -> arm2;   /* no test on arm2 */
```

That is sound exactly while every enum value is a declared variant — which is what the
forge doors defend. So a missed door does not merely let a strange value through; it makes
that value *take an arm*, and the `return 77` fall-through after the switch becomes dead
code. Every enum-forge bug to date (BUG-843, 864, 891, 910, and now 917) reports as "the
switch silently ran its LAST arm" for this reason.

Making the switch defensive (test the last arm too, fall through on no match) would remove
the amplifier permanently and independently of door coverage. It was NOT done here: it
costs a comparison and a branch on every enum switch, it silently does nothing on a forged
value rather than trapping, and ZER's chosen answer is to trap at the point of forgery.
Recorded as the alternative in case the door set ever stops being closable.

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

## Where main stands

`make check` exit 0 — **1391 .zer**, modules 30/30, 200 fuzz, 139 convert, all ten matrices, all EIGHT
audit gates (`audit_reference_examples.sh` joined with BUG-900), sink matrix **88 cells / 0 mismatch**.

Landed this session, newest first:

| sha | what |
|---|---|
| `7c873edd` | BUG-884..888 — the call-RESULT view class (4 parts) + my container fix REPLACED + BUG-869/870/871 |
| `c0180e3f` | BUG-883 — float -> int is DEFINED as SATURATING |
| `ae033cd0` | BUG-857/858 — container SEGFAULT (superseded, see below) + recycled-slot auto-zero |
| earlier | the 45-row harvest 1, BUG-815..856 |

## DECIDED — do not re-open

**float -> int out of range SATURATES, NaN -> 0.** Owner's call, matching Rust's `as`.
The reasoning is MEMORY vs ARITHMETIC: memory violations halt (slice OOB, misaligned
`@inttoptr`, bad `@pun`); arithmetic gets a defined value, which is already why integer
overflow WRAPS. Two sub-decisions came with it and are also settled:

- the compile-time **literal** case is a WARNING, not an error — rejecting `(u32)(-1.5)`
  while the same value through a variable is defined was "two spellings disagree";
- **`@truncate` on a float STAYS REJECTED**, a deliberate divergence from `29fiao` and
  `qa249l`. `@truncate` means "keep the low bits" and a float has none, and it is
  redundant — verified: `(u32)x` saturates and `@saturate(u32,x)` is named for it, both
  giving 4294967295 for 1e20 and 0 for -1.5. Their `float_to_int_saturates` /
  `float_to_int_total` are therefore NOT taken verbatim;
  `tests/zer/float_to_int_saturates.zer` covers the same semantics.

## THE CHEAP PATH: cherry-pick, do not re-derive — this is how tracker 3 was closed

`origin/claude/vigilant-tesla-as71kk` forked at `9f8cda08` (one behind main) and carried
better implementations than the catalog entries described. **It cherry-picked.** Every
commit is now TAKEN; the table is the record of what each cost.

The lesson for the next branch: **read the fork point first.** A branch that forked at or
near main is auditing the CURRENT compiler and its findings are live; one that forked
earlier will overlap heavily with whatever landed since, and its BUG numbers will collide.
Of the nine branches, only `r1piyr` and `as71kk` forked near main, and they produced
almost everything that survived.

| commit | rows | files | conflicts with main? |
|---|---|---|---|
| ~~`ccdd49ee`~~ TAKEN | **A2** @cstr, **A3** sign/`value_flows_to`, **A4** enum siblings (861 = already on main) | checker.c emitter.c | ONE conflict in checker.c, resolved to HEAD (purely additive on both sides). Its 861 `memset` had to be DELETED — main's BUG-858 already emits it, so the merge doubled it in both allocators. It also carried SEVEN tracked ELF binaries; dropped. |
| ~~`dda5b33b`~~ TAKEN | **A5** `?T` compare, **A6** non-null drift, **A7** inline-array free | checker.c test_emit.c | ONE conflict in checker.c, resolved to HEAD. It is what finally USES `nonnull_zero_hole` / `is_null_sentinel_ck`, which had arrived dead with `a22de320`. |
| ~~`6af2497e`~~ TAKEN | **A8** indirect-call keep carrier | checker.c | none |
| ~~`1cb1f93e`~~ TAKEN | **A14** `lower_expr`, dead code, warnings 32 -> 0 | many | NONE. Measured on main: 28 warnings before, 0 after. |
| ~~`a1919aa5`~~ TAKEN | **A9** six intrinsics storing through const | checker.c | none |
| ~~`0a0ec95e`~~ TAKEN | **A15** short-circuit, **A16** const slice, **A17** const array size | checker.c checker.h | none. Also fixes `audit_walker_fields.sh` membership (prose -> structural), baseline 758 -> 684. |
| ~~`e937a1d8`~~ TAKEN | **A18** funcptr arrays | checker.c emitter.c parser.c parser.h | none. Brings `audit_reference_examples.sh` and WIRES it into `make check` — verified non-vacuous (RED on the pre-fix compiler). |
| ~~`0828d1f7`~~ TAKEN | **A19** bit extraction through a pointer | checker.c | none |
| ~~`15b4fe49`~~ TAKEN | **A13** `@trap` arity + `ubsan_sweep.sh` | checker.c | none |
| ~~`38c27c18` `0ff11690` `d2cd8394`~~ TAKEN | tooling: `ub_sweep.sh`, the rc_cond_004 flake, structural `.gitignore` | — | `d2cd8394` conflicts modify/delete on the binaries it exists to remove; resolved by deleting. The flake did NOT reproduce here (16/16 on a pre-fix build) — the race is real by construction, not by measurement on this box. |

Recipe that worked twice today:

```
git cherry-pick --no-commit <sha>
# resolve any conflict by taking THEIRS unless main's version is strictly better
rm -f *.o src/safety/*.o && make zerc          # ALWAYS after a .h edit
# run the branch's own tests, then make check
```

`f63bddcb` (the view class) applied with **zero** conflicts because it touches only
`zercheck.h` / `zercheck_ir.c`. `a22de320` conflicted twice in `checker.c` and both
resolved to "theirs".

## Verification that must not be skipped

1. **Reproduce on main FIRST.** Read the DIAGNOSTIC, never the exit code — `-o out.c`
   isolates the checker's verdict from GCC's, and the echoed source line contains the
   offending text, which defeats a loose grep.
2. **Build a pre-fix compiler and prove the test flips**:
   `git archive <prev-sha> | tar -x -C $S && (cd $S && rm -f *.o src/safety/*.o && make zerc)`
3. **A new gate must be shown to FIRE**, not merely pass. `bash tools/sink_matrix.sh $S/zerc`
   is the pattern — p18 reported 7 holes + 2 over-rejects pre-fix, 0 after.
4. `MAKE_CHECK_EXIT` must be echoed. Make aborts at the first failing audit, so a later
   gate silently never runs.

## Corrections I made to MY OWN work — do not repeat these

- **O3 / H14 (`&&` narrowing).** I closed it as "precision only" after probing an
  UNPROVABLE index, which correctly guards. With a PROVABLE index the `&&` spelling is
  HARD-REJECTED while the nested-if spelling compiles. My own tracker had predicted this
  ("becomes required if the always-OOB verdict is ever promoted") and I never checked
  that BUG-796 had already promoted it. Still open as **A15**.
- **BUG-857 (container cycle).** My fix stopped the SEGFAULT and traded it for an
  OVER-REJECTION of a pointer-broken cycle. Replaced by as71kk's BUG-868 in `7c873edd`.
  The rule: a cycle is infinite only if EVERY edge is by-value.
- **The view class, partial.** I wrote the arg-FORM half and reverted it. Correct call:
  the ARITY part turns `returns_param_color` into a MASK precisely because the BLOCK-TAG
  bug lets the int name the WRONG param, so widening the consumer alone makes the wrong
  answer apply more often. Landed whole in `7c873edd`.
- **I briefly blamed my own change** for three view-class boundary over-rejections that
  predated it. Check before attributing.

## Two gates that were themselves broken — the general lesson

`audit_walker_fields.sh` (which I installed) decided membership on **prose**, counting
the function's own name inside comments, so editing a comment moved a walker in or out of
the audited set. as71kk fixes it; baseline 758 -> 684.

as71kk's qualifier probe's **first oracle was vacuous** — `-Wdiscarded-qualifiers` is
silenced by the explicit cast ZER's emitter writes, so it printed OK against a
deliberately broken compiler. And its `ubsan_sweep.sh` needed `float-cast-overflow` named
explicitly, because it is NOT in GCC's `-fsanitize=undefined` — found by breaking the
BUG-845 guard and watching the sweep report CLEAN on the exact class it exists to find.

**A gate that has only ever printed OK is a script, not a net.** Both of these were
caught by deliberately breaking the compiler and checking the gate went red.

## The two closure probes are MEASUREMENT SCRIPTS, not gates — do not count them as coverage

`tools/grammar_closure_probe.sh` and `tools/qualifier_closure_probe.sh` landed with
BUG-896 and are **not wired into `make check`** — neither on main nor on `as71kk`, which
wires only `audit_reference_examples.sh`. They are slow full-intrinsic sweeps meant to be
run by hand when the intrinsic set or a qualifier rule changes.

Recording this because CLAUDE.md's own rule is that an un-executed test artifact decays to
zero value and then to NEGATIVE value once someone believes it covers something. A future
session seeing `qualifier_closure_probe.sh` in `tools/` must not conclude qualifier
closure is gated. It is measured, once, by whoever runs it.

`tools/qualifier_closure_baseline.txt` is the useful half: 25 justified strips, each
established by READING the emitted C (the pointer's value goes to an `"r"` asm operand
with a `"memory"` clobber, never a dereference), with the discriminating question written
down for the next reader. The CONST half is not baselined — those six became BUG-896.

## Recommended order — NOTHING REMAINS (kept as the record of what was done)

1. ~~**A3**, **A2**, **A4**~~ — DONE 2026-08-26 (BUG-889..891, `ccdd49ee`). `value_flows_to`
   is in; the eight negative-constant sinks each have their own test, which is the only way
   to prove one query really replaced all eight.
2. ~~**A5**, **A6**, **A7**, **A8**, **A9**~~ — ALL DONE 2026-08-26 (BUG-892..896,
   `dda5b33b` / `6af2497e` / `a1919aa5`). **Every accept-unsafe row in tracker 3 is closed.**
3. ~~**A15..A19**~~ — ALL DONE 2026-08-26 (BUG-897..901, `0a0ec95e` / `e937a1d8` /
   `0828d1f7`, no conflicts). A15 was my mis-closed row.
4. ~~**A13**, **A14**~~ — DONE 2026-08-26 (BUG-902/903, `15b4fe49` / `1cb1f93e`).
5. ~~Tooling~~ — ALL DONE 2026-08-26. `ubsan_sweep.sh` + `ub_sweep.sh` (measurement
   scripts, see the note above); the walker-field membership fix (758 -> 684, rode with
   BUG-897); the structural `.gitignore` (1084 binaries untracked, all verified ELF); the
   rc_cond_004 flake.

**HARVEST TRACKER 3 IS CLOSED — all 20 rows.** `as71kk` is fully consumed; nothing
remains to cherry-pick from it.

Still parked, deliberately, with reasons recorded in their own entries below: the comptime
array-element width, the extern `*opaque` type_id (both candidate fixes risk an
under-rejection), and `29fiao`'s BUG-852 (measured at 34 correct corpus files, so the
blanket rule was NOT shipped).

---


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
| **O3 — STILL OPEN** | `&&`/`||` do not narrow their RHS | **87xihb** BUG-800 | `if (i < 4 && arr[i] > 0)` — the canonical guarded idiom, and the exact shape the auto-guard warning tells users to write — still carries a runtime guard. **PRECISION ONLY on main** (warning, compiles). It becomes REQUIRED if the always-OOB verdict is ever promoted at short-circuit position |

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

- **extern `*opaque` crosses the C boundary with an UNINITIALISED `type_id`** (4z36e0,
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
- **Two DISTINCT `@once` blocks touching the same global are not mutually exclusive**
  (39294y, LOW).
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

## OPEN — the four 2026-08-11 residuals, all measured 2026-08-16

Two CLOSED, one CONFIRMED LIVE with a precise narrowing, one confirmed live and awaiting a
POLICY decision. **Every probe below was routed around its masking rule** — the first pass
scored two of them "closed" on masked evidence.

### ~~ISR non-atomic RMW laundered through a pointer parameter~~ — **CLOSED (measured)**

`interrupt { rmw(&g); }` with `void rmw(*C p){ p.v += 1; }` is rejected on both routes:
with a plain param the QUALIFIER gate fires (*"cannot pass volatile pointer to non-volatile
param"*); with a `volatile *C` param the ISR rule fires (*"volatile global 'g' is shared
between interrupt and main"*). The hazard has no unblocked path.
**PROBE WARNING:** the obvious reproducer uses `p[0] += 1`, which is rejected by the
single-pointer-indexing rule and masks both. Use a struct field (`p.v += 1`).

### ~~`free`-of-a-param-field through an UNWRAP-TO-LOCAL~~ — **CLOSED 2026-08-16 (BUG-785)**

`lp = h.b` now registers an alias with the compound `(param, ".b")`, so a free propagates in
BOTH directions and the cross-function `frees_param_field` summary sees it. The existing
FIELD_READ alias arm linked the dest to the BASE object's handle; a by-value struct param
carries no allocation identity on the base — it lives on the COMPOUND — so the arm silently
did nothing. Added a fallback that mints a shared `alloc_id` on the compound and aliases the
unwrapped local into that group. Same POINTER-CARRYING gate as the existing arm, which is
what keeps `test_modules/move_user` green (verified: 0 errors).
Tests: `tests/zer_fail/frees_param_field_unwrap{,_reverse}.zer` +
`tests/zer/field_read_scalar_no_alias_ok.zer` (the scalar canary).

### (superseded) CONFIRMED LIVE — `free`-of-a-param-field through an UNWRAP-TO-LOCAL

The entry marked *"CLOSED 2026-08-08"* IS incomplete, as `2hg2v4` reported. Narrowed:

| shape | verdict |
|---|---|
| `void fb(H h){ free(h.b); }` — direct | **caught** |
| `void fb(H h){ [*]B lp = h.b; free(lp); }` | **LIVE** |
| `void fb(H h){ [*]B a = h.b; [*]B c = a; free(c); }` | **LIVE** |

**Root cause, measured — it is WIDER than the summary.** The summary scan
(`zercheck_ir.c` ~6220) looks for a compound handle rooted at the param
(`ch->local_id == plocal && ch->path_len > 0`). An unwrap-to-local frees a DIFFERENT local,
so the compound is never marked. But the alias is missing INTRA-function too, in BOTH
directions: `free(h.b); use(lp)` is also accepted. So `lp = h.b` registers no alias with the
compound at all — fixing only the summary scan would leave the intra-function half open.

**Fix shape:** register a var-decl/COPY whose source is a POINTER-CARRYING field of a
by-value struct param as an alias of the compound `(plocal, ".field")`, so the existing
alias-group propagation carries frees both ways. **DANGER — this is the exact class that
broke `test_modules/move_user` TWICE** (see CLAUDE.md "The pointer-vs-scalar refinement"):
the arm MUST be gated on the field being pointer-carrying, or a scalar field read
(`u32 v = t.id;`) inherits the parent's transferred/freed state.
**PROBE WARNING:** two of the three shapes are MASKED by the LEAK rule
(*"handle %0 (local 'h') allocated ... never freed"*) — read the reason, not the exit code.
The unmasked discriminator is `free(h.b); use(lp);`.

### ~~`&packed_field` forms a possibly-misaligned `*T`~~ — **CLOSED 2026-08-16 (BUG-786)**

**It was never a policy question — ZER had already decided it.** `tests/zer_fail/atomic_packed_field.zer`
(BUG-493) rejects `@atomic_add(&r.counter)` on a packed field precisely because it is misaligned
on ARM/RISC-V. The same question simply had no answer at the other sinks, which is the
one-question-many-sinks pattern, not an open design choice.

Resolved by TRACKING, per the Ban Decision Framework (rule 5: none of the four ban conditions
applies, so track). New `Symbol.is_packed_derived` — a Model-4 static annotation on the same
rails as `volatile`/`const` — set when a var-decl binds `&packed.field`, and consumed at the
DEREF sink. Index was already covered by the `*T`-indexing rule; atomics stay on BUG-493.

Measured: the emitter drops the packed-ness entirely (`uint32_t* _zer_t0 = &p.b;`), so nothing
downstream could recover it. Corpus cost ONE file — the gap reproducer, now promoted to
`tests/zer_fail/packed_field_addr_deref.zer`. Direct access `p.field` is unaffected and is what
the diagnostic points users to; pinned by `tests/zer/packed_field_direct_ok.zer`.

### (superseded) CONFIRMED LIVE — `&packed_field`

`packed struct P { u8 a; u32 b; } ... *u32 q = &p.b;` compiles with no diagnostic. The
non-packed control behaves identically, which is the point: the compiler cannot distinguish
them at the `&` site today.
**PROBE WARNING:** `return q[0]` masks this via the single-pointer-indexing rule; use `*q`.
**Still an owner decision** — ban `&` on a packed field, require an explicit unaligned-load
intrinsic, or accept with a diagnostic. Not something to settle by patching.

### deref-launder residual (LOW, latent)

The escape peel takes an intrinsic's LAST argument, so a hypothetical single-arg
pointer-taking intrinsic with a SCALAR result would mis-mark its result local-derived. The
one live instance (`@atomic_load(&local)`) is now unreachable — BUG-784 rejects atomics on a
stack local first. `@ptrtoint` and `@probe` were checked and are unaffected. Fixing it would
be a RELAXATION (the dangerous direction) for no currently-reachable gain.

---

## OPEN — concurrency sweep 2026-08-10: NO holes found; two doc/impl mismatches corrected

Systematic probe of the concurrency surface after BUG-783. **No accept-unsafe hole found.**
Recorded so a fresh session does not re-run it blind, and because the sweep's own METHOD
failed twice before it produced a trustworthy answer.

### Probe-harness failures that produced FALSE "holes" (read before writing a sweep)

1. **The detector missed `zercheck:` diagnostics.** The helper grepped `': error'`, which
   matches neither `file:3: zercheck: ...` nor a line starting `error:`. ThreadHandle-not-joined
   was reported as an accepted hole when it is caught. **Grep `-E 'error|zercheck:'`.**
2. **Three probes did not exercise their rule.** `volatile u64` from a spawn needs
   `--target-bits 32` (on a 64-bit target a u64 IS single-word, so acceptance is correct);
   the Ring pointer-element probe used a struct VALUE element; the async probe used a shape
   the rule deliberately allows. Each looked like a hole and was a malformed probe.

### Verified CLOSED by this sweep (each caught, correct reason)

ThreadHandle never joined; ISR non-atomic RMW via a pointer param; scoped-borrow write between
spawn/join; scoped-borrow CROSS-BLOCK; `threadlocal &` to a spawn; Handle nested in an optional
struct to a spawn; same-statement two-shared-type deadlock; spawn inside `@critical`; global
Pool from a spawn body; `slab.alloc` in an ISR; `@cond_wait` on a foreign shared struct;
`volatile u64` from a spawn at `--target-bits 32`; move-struct through a spawn arg; Ring push of
a local-derived pointer; shared read in an `await` condition; shared read in a `yield` statement.

### Doc/impl mismatch #1 — CORRECTED (CLAUDE.md)

CLAUDE.md's safety table said *"Shared struct field access inside async function -> compile
error"*. **That overstates the rule.** The real D02 ban is *shared access in a statement
CONTAINING yield/await* (checker.c ~7496, gated on `in_async_yield_stmt`), because locking is
PER-STATEMENT: `x = g.v; yield;` releases the lock before the suspend and is correctly
ACCEPTED. The row now states the real rule. Left as-is this would have sent a session hunting
a non-bug — or "fixing" a correct acceptance into an over-rejection.

### ~~Doc/impl mismatch #2~~ — **RESOLVED 2026-08-10: the DOCS were wrong, the compiler is right**

Asked to "enforce the docs", measuring first showed enforcement is **impossible and would be
wrong**:
- **`*shared T` is NOT CONSTRUCTIBLE.** `shared` is a struct-only qualifier (`shared u32 g;`
  -> *"expected 'struct' after 'shared'"*) and you cannot take the address of a shared
  struct's interior (*"cannot take address of a shared struct's interior"*). There is no way
  to build the documented operand, so enforcing it would reject EVERY `@atomic_*` call with
  no legal alternative.
- **Corpus: 46 of 48** atomic-using test files operate on a plain global, including the whole
  `rt_conc_atomic_*` Rust-translation set and the atomic-cell tests BUG-769/771 depend on.
- **It inverts the feature.** `shared struct` is a MUTEX; requiring one to perform an atomic
  operation means taking a lock to do lock-free programming.
- **The plain-global form is already sound**: a real atomic instruction is emitted, and the
  ATOMIC CELL rule rejects any MIXED plain+atomic access (that is exactly what BUG-769/771
  were), so all-atomic access to a plain global has no race.

**Everything ELSE in that doc sentence IS enforced** — verified: width must be 1/2/4/8 bytes
(`u128` and a non-native `u24` both rejected with that exact message) and the operand must be
an integer (`f32` and a struct both rejected). Only the `*shared T` phrase was wrong.

Docs corrected in `docs/reference.md` and `CLAUDE.md` to state the real contract. Pinned by
`tests/zer_fail/atomic_width_u128.zer` and `tests/zer/atomic_plain_global_contract_ok.zer`
(1/2/4/8-byte globals + a struct field + load/store round-trip).

### (superseded) original entry

`@atomic_load(&g)` on a **plain non-shared** global compiles clean. The docs describe atomics
as requiring a `*shared T` operand. Probed: this is **not a race** — the atomic-cell rule
(BUG-769/771) catches any MIXED plain+atomic access, so a purely-atomic access to a plain
global is sound, and the emitted code is a real atomic instruction. So the question is
whether to (a) enforce the documented `*shared T` requirement (a tightening; would reject
working code), or (b) relax the docs to match. **Not a safety issue either way** — recorded so
it is not re-probed as a suspected hole.

---

## OPEN — residuals after the 2026-08-10 survey (measured; reproducers in `tests/zer_gaps/`)

The 9 survey fixes + the 8th funcptr REACH form all landed. These are what the three
branches documented but did NOT fix, each **re-verified live on main 2026-08-10**.

### ~~HIGH ACCEPT-UNSAFE — pointer-deref var-decl alias (`*T k = *pp;`)~~ — **CLOSED 2026-08-10 (BUG-781)**

Closed by REJECTING the form, not by tracking it — the Level-A stance: when the analyzer
cannot prove which allocation an alias refers to, reject. Resolving `*pp` needs points-to
over a pointer-to-pointer, which the per-file + summaries model deliberately lacks, so
there is nothing to prove it WITH. **Cost measured before shipping: ZERO instances in the
whole corpus** (tests/zer, zer_fail, test_modules, rust_tests, zig_tests) — every
deref-init var-decl there is a SCALAR or struct-VALUE copy, which the predicate does not
match because it requires a POINTER-typed result. Restructure is one line and teachable
(`*T k = p;`), which is the bar CLAUDE.md sets for an acceptable false positive.
Tests: `tests/zer_fail/deref_alias_uaf_double_free.zer` + `tests/zer/deref_scalar_ok.zer`.

**Enumerating first found TWO more live forms the branch had not recorded** — deref into a
struct FIELD (`h.p = *pp;`) and deref RETURNED through a function (`return *pp;`). Both
still ACCEPTED; the var-decl sink is closed, those two sinks are not. Same predicate
(`deref_ptr_launder`) applies — wire it at the assignment and return sinks. Kept OPEN below
rather than claimed closed, because a partial fix that promotes the gap file is exactly the
false-confidence failure this ledger exists to prevent.

### ~~HIGH ACCEPT-UNSAFE (RESIDUAL) — deref-launder at the FIELD-STORE and RETURN sinks~~ — **CLOSED 2026-08-10 (BUG-782)**

`h.p = *pp;` and `*Node leak(**Node pp) { return *pp; }` now reject via the same
`deref_ptr_launder` predicate. **All three sinks of this class are closed** (var-decl,
field-store/assign, return). Tests: `tests/zer_fail/deref_alias_{uaf_double_free,field_store,return}.zer`;
`tests/zer/deref_scalar_ok.zer` pins the over-rejection boundary across all three sinks
(scalar var-decl, scalar assign, struct-VALUE copy, scalar return).

### ~~(superseded — original entry text follows)~~ pointer-deref var-decl alias

`tests/zer_gaps/deref_alias_uaf_double_free.zer`. **Measured live: the program RUNS and
returns 7** — it reads `n.v` after `free(k)` (stale value) and then double-frees. Heap facet
of HOLE-A4.

**Narrowed precisely 2026-08-10 — the gap is ONE form, not the whole class:**

| shape | verdict on main |
|---|---|
| `*Node k = n; free(k); n.v` — direct alias | **caught** |
| `*Node k = n; k = *pp;` — ASSIGN with a deref RHS | **caught** (k already had a handle) |
| `*Node k = *pp;` — VAR-DECL with a deref init | **ACCEPTED — the hole** |
| `... free(n); k.v` — same, use through k | **ACCEPTED** |

**Why the obvious patch is wrong.** In the IR the deref lowers to `%8 = UNOP <expr>` and the
var-decl to `%7 = COPY` from it; the UNOP temp carries no handle, so the copy inherits
nothing. Making the COPY inherit requires resolving `*pp` -> `pp` -> its init `&n` -> `n`'s
handle. That is a POINTS-TO question over a pointer-to-pointer, which the per-file +
summaries model deliberately does not have (whole-program analysis is banned from the
architecture). The enabling step is `&n` on a local that holds a tracked allocation.

**Two sound directions, both needing design rather than a patch:**
1. *Syntactic resolution* (cheap, narrow): if a var-decl init is `*X` and `X`'s own
   var-decl init was `&Y` (a bare ident, not reassigned anywhere — reuse
   `ast_name_mutated_or_addrd` as the stability gate), alias the new local to `Y`'s handle.
   Same shape as the funcptr local-binding resolution in `00dc785a`. Covers the reproducer;
   does NOT cover a reassigned or computed `pp`.
2. *Conservative barrier* (broad, needs care): taking `&n` where `n` holds a tracked
   allocation means aliases exist that the analyzer cannot follow — apply the
   argument-precise barrier principle and widen `n` to MAYBE_FREED on any free through an
   unresolved deref. Touches the CFG fixpoint; measure over-rejection first.

### MEDIUM silent bare-metal — non-atomic RMW in an ISR laundered through a pointer parameter

Sibling of BUG-774: the ISR compound-RMW check is intra-body, so `interrupt { rmw(&g); }`
with `void rmw(*u32 p) { p[0] += 1; }` is unchecked. The transitive machinery from BUG-774
(`record_isr_globals` following calls) exists; what is missing is propagating the
*compound-assign* fact through a pointer PARAM rather than a global name.

### MEDIUM (POLICY, not a bug) — `&packed_field` forms a misaligned `*T`

`tests/zer_gaps/packed_field_addr_misaligned.zer`. Confirmed + UBSan-reproduced.
**Deferred pending an owner decision**: whether ZER permits forming a possibly-misaligned
pointer at all. Options: ban `&` on a packed field, require an explicit unaligned-load
intrinsic, or accept with a diagnostic. Not something to settle by patching.

### LOW bare-metal — `volatile` lost through a `@ptrtoint -> @inttoptr` round-trip

`tests/zer_gaps/volatile_stripped_ptrtoint_roundtrip.zer`. Audit-visible (both intrinsics
are explicit at the use site), so it is a qualifier-propagation gap, not a silent one.

### Cross-references (tracked elsewhere, do NOT duplicate)

- **frees-param-field via UNWRAP-TO-LOCAL** (`2hg2v4`) — extends the frees-param-field entry
  marked "CLOSED 2026-08-08"; **that closure is INCOMPLETE.** Re-probe before trusting it.
- **comptime / const integer fold ignores per-operation width wrapping** (`2hg2v4`) —
  value-correctness, explicitly probed and found NOT a safety hole (checker and emitter read
  the same folded value). Deferred: the fix threads a destination width through the const
  evaluator.
- **`naked` attribute dropped on the IR path**, **`vrp_ir.c` dead code** (Phase 0 of
  `docs/unified-oracle-proved-ZER.md`), **ISR funcptr FIELD call not bound in a same-scope
  struct init** (the residual after BUG-774) — all already have entries.

---

## ~~branch survey 2026-08-10: 9 verified fixes~~ (ALL LANDED 2026-08-10, BUG-771..779)

**FULLY HARVESTED.** All 9 implemented and verified; `make check` breakage closed by P3.
Kept for the reproducers, the probe/masking warnings, and the branch attribution. The
"documented-but-NOT-fixed" list at the END of this entry is still OPEN — see there.

### (original entry follows)

## OPEN — branch survey 2026-08-10: 9 verified fixes to implement (`p0w5lj` / `2hg2v4` / `l3vn1i`) + a LIVE `make check` breakage

**READ THIS FIRST — `make check` IS CURRENTLY BROKEN AND HAS BEEN SINCE `65af3864`.**
`MAKE_CHECK_EXIT=2`. Make aborts at the walker-default-clause audit, so **five gates never
run**: fixed-buffer, type-dispatch, carrier-dispatch, emit-audit, and the sink matrix.
Cause: the G3 fix (`65af3864`) shipped `record_atomic_plain_in_callee` with a
`default: return;` in a `switch (node->kind)`, which CLAUDE.md forbids. **Fix P3 below closes
both the coverage hole and the build breakage — land it FIRST or nothing else can be verified.**

**HOW IT WAS MISSED, so nobody repeats it.** The verification grep was `grep -E 'OK — no'`,
which matches **"OK — no gaps. IR emitter covers every node kind…"** — a DIFFERENT audit that
runs EARLIER. That line was present, so four commits (`65af3864`, `88c012ac`, `dfccd3f5`,
`d995dbac`, `36404262`) were reported "make check exit 0, all gates" when make had actually
aborted. CLAUDE.md already warns *"exit=2 with those lines ABSENT means an earlier step
aborted make before the audits"* — the trap is that a DIFFERENT audit's OK line satisfies a
loose grep. **Grep for the SPECIFIC line (`no default: clauses`), and always echo the real
`MAKE_CHECK_EXIT`.**

---

**Survey scope.** Three `claude/gifted-noether-*` branches, one squashed commit each, forked
from recent main (`65af3864` / `ac97e11a` / `6f6d9186`). **Every fix below was independently
reproduced against current main before being listed** — mostly by running the branch's OWN
negative test with main's `zerc`. Nothing is repeated on a branch's word alone. NOT merged or
pulled; implement from these descriptions.

### The 9 fixes to take

| # | fix | source | how verified on main |
|---|---|---|---|
| **P3** | G3 walker `default:` skips 9 child-carriers **+ unbreaks `make check`** | `p0w5lj` | `atomic_cell_plain_via_switch_arm.zer` ACCEPTED |
| **L1** | `@container` launders a stack pointer past escape sinks | **`l3vn1i`** | `container_var_escape_global.zer` ACCEPTED |
| **L2** | `find_return_range` misses a return buried in a NESTED orelse | `l3vn1i` | 3 `zer_trap` files exit 0 (guard elided) |
| **L3** | ISR dispatch through a funcptr never scanned (3 idioms) | `l3vn1i` | 3 negatives ACCEPTED |
| **P1** | `return { .p = &local }` struct-literal escape at the RETURN sink | `p0w5lj` | `return_struct_init_local_escape.zer` ACCEPTED |
| **P2** | ISR body never resets the VarRange map -> elided bounds guard | `p0w5lj` | emit-C only (invisible on hosted) |
| **H1** | funcptr ARRAY-ELEMENT field callback `o.fns[0]()` spawn race | `2hg2v4` | `spawn_funcptr_array_field_race.zer` ACCEPTED |
| **H3** | uN/iN SHIFT result not width-wrapped on the AST-passthrough store path | `2hg2v4` | branch's `uN_width_wrap_all_forms.zer` exits 40 |
| **H4** | `@bitcast` / `@truncate`-to-uN in a global initializer emit illegal C | `2hg2v4` | both negatives ACCEPTED |

---

**P3 — the G3 atomic-cell walker shipped with a `default:` (`p0w5lj`). CRITICAL + build-breaking.**
`record_atomic_plain_in_callee` (checker.c, added `65af3864`) used `default: return;`, silently
skipping **switch / defer / orelse / do-while / @critical / @once / spawn / slice / struct-init**
child-carriers. A plain atomic-cell access inside any of those, in a helper called after a
fire-and-forget spawn, is not recorded and the race slips. Fix: enumerate the switch (descend the
child-carriers, explicit no-op leaves/decls, NO `default:`). This is a partial walk by intent, so
the no-op cases must be listed explicitly — that is exactly what the audit demands.
Test: `tests/zer_fail/atomic_cell_plain_via_switch_arm.zer`.

**L1 — `@container` launders a stack pointer past the escape sinks. CRITICAL (ASan stack-UAF).**
**DUPLICATE — `2hg2v4` also fixes this (its BUG-769, 3 sinks). TAKE `l3vn1i`'s:** it covers
**4** sinks (store-global, struct-field-of-global, return, keep), routes all **six** escape peels
through the shared `unwrap_ptr_launder`, peels launder intrinsics at the top of
`arg_is_local_derived`, **and fixes a latent sibling** — `unwrap_ptr_launder` peeled
`@cstr(buf, str)` to the string literal instead of its `args[0]` buffer.
Root cause: `@container(*T, ptr, field)`'s pointer is **args[0]**, but the sinks hand-rolled a
LAST-arg intrinsic unwrap (last arg = the field NAME), so `is_local_derived` was never consulted.
**PROBE WARNING — one of the branch's own negatives is MASKED:** `container_var_escape_return.zer`
IS rejected on main, but by the LEAK rule (`handle %0 (local 'd') allocated…`), not the escape
rule. Reading the exit code alone scores this "already fixed". Check the REASON.

**L2 — a return buried in a NESTED orelse fallback is dropped from the return range. CRITICAL
(ASan global-buffer-overflow).** The earlier B9 fix only saw a TOP-LEVEL orelse. These three
shapes drop the buried return, so the caller elides `arr[callee()]`'s guard:
`(mb() orelse { return 9; }) + b`, the same inside a call arg, and inside a return-expr wrapper.
Fix: `scan_expr_orelse_returns`, a recursive expr walker wired into the `NODE_RETURN` handler and
the general block. Conservative — it can only ADD guards.
Tests: `tests/zer_trap/vrp_buried_orelse_{binary,callarg,return}.zer` (all exit 0 on main = guard
elided = the bug).

**L3 — ISR dispatch through a function pointer is never scanned. HIGH (silent bare-metal race).**
The ISR sibling of the spawn funcptr family (`5ed17c2f`, `00dc785a`, `ac97e11a`) — the pattern
predicted and confirmed: spawn side fixed three times, ISR side never. A non-volatile global
touched only through an ISR-dispatched funcptr is invisible, and GCC `-O2` is free to hoist/tear
it. Three idioms now followed from `record_isr_globals` to the target body: a **global funcptr
variable**, a **funcptr field in a struct init**, and a **function name passed as an arg**.
Tests: `tests/zer_fail/isr_funcptr_{global_var,struct_field,arg}.zer` — all three ACCEPTED on main.

**P1 — `return { .p = &local }` dangles. HIGH.** A struct/union **literal** returned BY VALUE
carrying a pointer/slice to a frame-local. The var-decl and assign-to-global sinks already ran
`struct_init_has_local_derived`; the RETURN sink was the missing sibling (the multi-site class
again). Gate on `type_carries_data_pointer`. `p0w5lj` also adds sink-matrix shape **p14**
(3 reject + 2 safe cells) — take the matrix cells with the fix.
Tests: `return_struct_init_local_escape.zer` + two positives (`_global_ptr_ok`, `_param_ptr_ok`).

**P2 — an ISR body never resets the name-keyed VarRange map. HIGH (silent bare-metal OOB).**
Only `func_decl` reset it (per SS-C#15). A narrowed range from the preceding declaration leaks
into the `interrupt {}` body, marks a fixed-array index "proven", and elides the bounds
auto-guard -> stack OOB. One-line mirror of the `func_decl` reset.
**TEST-AUTHORING NOTE:** invisible on hosted x86 and an `interrupt` block cannot appear in a
runnable `tests/zer/` positive (GCC refuses ISRs on hosted x86-64). Verify **emit-C-only**
(`zerc f.zer -o f.c`) and say so in the commit.

**H1 — funcptr ARRAY-ELEMENT field callback evades the spawn race scan. HIGH.**
`o.fns[0]()` bound in the CALLER was accepted while the scalar sibling `o.cb()` was rejected:
`body_calls_funcptr_field` / `scan_funcptr_field_bindings` matched only a bare `NODE_FIELD`
callee/target, never `NODE_INDEX`-over-`FIELD`. Fix: a recursive `funcptr_field_access()` helper
used at BOTH sites. **This is a 7th form of the funcptr REACH class** — add its cell to the REACH
grid in `tests/test_conc_matrix.c` in the same commit, or it is invisible.

**H3 — uN/iN SHIFT result is not width-wrapped on the AST-passthrough store path. HIGH
(silent miscompile).** `u3 3<<2` gives 12, not 4; iN keeps the wrong sign. The G3 self-mask
EXCLUDED shifts, and `_zer_shl`/`_zer_shr` only guard shift-by->=carrier-width — they never mask
to N. var-decl / return / call-arg were safe (IR_BINOP + `emit_intn_mask`); **plain assign,
global store, struct-field, array-elem, and shift-feeding-div/mod/shr were not.** Fix: wrap via
`emit_intn_mask_lv`, mirroring the IR path. Take the branch's EXTENDED
`tests/zer/uN_width_wrap_all_forms.zer` (main already has the file; the branch appends the shift
forms — it exits **40** on main).

**H4 — `@bitcast` and `@truncate`-to-uN in a global initializer emit illegal C. MEDIUM (LOUD).**
The checker accepts them but the emitter produces a statement-expression, which is illegal at
file scope -> a GCC error the user cannot act on. Add both to the G7 global-init reject list so
the diagnostic comes from the checker. A native `@truncate` global still compiles.

### Suggested order

**P3 FIRST** — until it lands, `make check` aborts before five gates and the sink matrix, so no
subsequent fix can be properly verified. Then **L1 -> L2 -> L3** (largest safety surface, all
CRITICAL/HIGH), then **P1 -> P2**, then **H1 -> H3 -> H4**.

### Also on these branches: documented-but-NOT-fixed (verify before chasing)

From `p0w5lj` (5 reproducers added to `tests/zer_gaps/` with tripwires):
- **HIGH** cross-thread race via a **CALLER-supplied funcptr forwarded to `spawn`**
  (`spawn_funcptr_forwarded_param_race.zer`) — an 8th REACH form; the target's funcptr comes in
  as a PARAMETER, so no binding is visible in the file.
- **HIGH** pointer-deref var-decl alias (`*T k = *pp;`) untracked -> UAF + double-free. This is
  the HOLE-A4 heap facet, also confirmed independently by `2hg2v4`.
- **MEDIUM** non-atomic RMW in an ISR laundered through a pointer parameter.
- **MEDIUM** `&packed_field` misalignment (already tracked here; policy decision pending).
- **LOW** `volatile` lost through a `@ptrtoint -> @inttoptr` round-trip (audit-visible).

From `2hg2v4`:
- **CONFIRMED silent miscompile** — comptime/const integer fold ignores per-operation width
  wrapping. Computes in host `int64` and masks only the final result. Explicitly probed and found
  NOT a safety hole (checker and emitter read the same folded value), but a VALUE-correctness bug.
  Deferred because the correct fix threads a destination width through the const evaluator.
- **CONFIRMED accept-unsafe** — frees-param-field via UNWRAP-TO-LOCAL, extending the
  "CLOSED 2026-08-08" frees-param-field entry. **That earlier closure is therefore incomplete.**
- **LOW over-rejection** — a function returning a raw `*T` view of a global flagged as a leaked handle.
- **SUSPICION (not confirmed)** — `@atomic_load(&g)` on a plain non-shared global compiles clean.

From `l3vn1i`:
- ~~ISR funcptr FIELD *call* / factory-return residual after L3~~ — **CLOSED 2026-08-10
  (BUG-783).** All NINE reach forms are now covered at the ISR sink. Measuring first
  corrected the premise: seven were ALREADY caught (`record_isr_globals` descends var-decl
  inits, assignments and struct-init fields generically), and only the two FACTORY-return
  forms were live. Gated by a 9-cell ISR sub-grid in `tests/test_conc_matrix.c`
  (conc-matrix 84 cells, verified firing at 82/84 pre-fix).
- `naked` attribute silently dropped on the IR path (confirmed still live).
- `vrp_ir.c` DEAD CODE (349 lines) — cross-ref: this is Phase 0 of
  `docs/unified-oracle-proved-ZER.md`.

---

## OPEN — `Pool`/`Slab`/`Ring` as a container FIELD reports "undefined type 'T'" (LOW, message quality)

**Symptom.** `container C(T) { Pool(T,4) v; }` is rejected with `error: undefined type 'T'`.

**Status — the rejection is CORRECT, the message is not.** The emitter cannot stamp their
inline storage for a monomorphized container, so accepting them would produce a broken
emission rather than a working feature. But they are rejected because `subst_typenode` leaves
them unsubstituted, not by a deliberate check — so the diagnostic blames the type parameter
instead of naming the real restriction.

`Handle(T)` was in the same leaf group and was FIXED 2026-08-06: a Handle is a `u64`
(index + generation), so the stamped struct needs no per-T layout. That is exactly the
property Pool/Slab/Ring lack.

**Fix sketch.** Either add the emitter support, or add an explicit check in the container-field
validator that names the restriction ("Pool/Slab/Ring cannot be a container field — declare the
allocator as a global and store `Handle(T)`"). The second is a few lines and is what a user
actually needs. Do NOT simply add substitution in `subst_typenode` — the comment there records
why, and doing so would swap a clear type error for a broken emission.

**Documented for users** in `docs/reference.md` (container section), so the restriction is at
least discoverable while the message is poor.

---

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

## OPEN — scoped-borrow: a join on EVERY branch arm is over-rejected (LOW, precision)

**Symptom.** Joining on every arm of a branch and then using the borrowed local is
safe in reality, but rejected:

```zer
ThreadHandle th = spawn worker(&work);
if (f == 3) { th.join(); } else { th.join(); }
return work.x;              // rejected: "borrowed by a scoped spawn"
```

**Cause — deliberate.** The 2026-08-03 cross-block fix guards borrow release on
`Checker.branch_depth <= Symbol.th_spawn_branch_depth`. A join nested deeper than
the spawn is path-conditional, so the borrow survives the branch. That is what
closes the accept-unsafe race; the price is that an all-arms join is not
recognised as unconditional.

**Workaround (one line).** Hoist the join out of the branch — `if (f == 3) { ... }
th.join();`. The compiler error names the borrow, and this is the idiomatic shape
anyway.

**Why it was not fixed at the same time.** Recognising "joined on every arm"
requires all-paths analysis of the branch, i.e. the borrow set must become a
per-block lattice with a JOIN at merges (the shape CLAUDE.md's VRP-JOIN row
describes). That is a real refinement, and it is a RELAXATION — the change class
where a bug ships a race rather than an over-rejection. It should follow the
documented accept-unsafe discipline: build the exhaustive branch x join-position
grid FIRST, verify it fires against the pre-fix build, then relax. The
over-rejection is safe to live with meanwhile.

**Tripwire.** `tests/zer/scoped_borrow_branch_shapes.zer` pins the four shapes
that must keep compiling, so the guard cannot broaden unnoticed.

---

## OPEN — findings carried forward from the 2026-07-21→31 branch wave (documented, NOT fixed on any branch)

These are the audit findings the eleven branches recorded but did **not** fix. Deduped against the rest
of this file — items already tracked here (cross-block scoped-borrow, the `?T` optional-unwrap class-kill,
`vrp_ir.c` dead code) are noted as cross-references rather than repeated.

### ~~CRITICAL ACCEPT-UNSAFE — cross-function free of a FIELD of a by-value struct/union param~~ (`rdh99l` `9a40ead4`) — **CLOSED, measured 2026-08-08**
**Does NOT reproduce on main.** The reproducer below is rejected with `double free: local %0 already
freed at line 7` + `use after free`. Siblings probed the same day and ALSO rejected: 2-hop
(`outer(h) -> inner(h) -> free(h.f)`), free-then-USE (not double-free), and a NESTED field
(`free(h.in.f)`). Four over-rejection controls still compile (single correct free; read-only callee;
two independent allocs; plain by-value param). The union form is masked by the stronger
union-variant-read rule, so it is not separately testable. Closed by the by-value-struct-field
carrier work of 2026-08-06 (`5748e904`, `ir_arg_view_handle` + the (c3)/(c4) return-summary arms).
Pure ZER (no cinclude / `*opaque` / asm). Compiles clean; runtime glibc double-free abort or observable UAF.
```zer
struct B { u32 x; } struct H { [*]B buckets; }
void fb(H h) { free(h.buckets); } // frees a FIELD of a by-value struct param
u32 main() { H h; h.buckets = alloc(B, 4) orelse { return 0; };
             fb(h); free(h.buckets); return 0; } // analyzer thinks THIS is the first free
```
**Root cause.** The FuncSummary free-of-param scan (`zercheck_ir.c` ~5352) gates on the param's resolved
type being POINTER/HANDLE/OPAQUE/SLICE, so a by-value **STRUCT/UNION** param is dropped entirely; and even
for the accepted kinds it inspects only the BARE param handle (`ir_find_handle`, `path_len == 0`), never a
compound FIELD handle. There is no `frees_param_field` signal, so a callee freeing `h.buckets` records
nothing and the call site never widens `arg.field` to FREED. Distinct from BUG-737 (by-value field
STORE-to-global escape), P9 (by-value field launder), §A #6 (intra-function compound copy) and §A #2
(slice/pointer PARAM free).
WARNING: **Doc correction:** the existing negative `tests/zer_fail/alloc_byval_field_slice_uaf.zer` passes for the
WRONG reason (rejected as a LEAK, not because the field-free is tracked) — the moment a caller-side free
satisfies the leak check, the double-free/UAF passes silently. The 2026-07-15 tracker's §A #2 claim that
`fb(H h){free(h.buckets)}` "was recorded" is **inaccurate**.
**Fix sketch (attempted 2026-07-26, REVERTED — not a safe single-session change):** a coarse
`bool *frees_param_field` on FuncSummary (definite/all-path field free of an aggregate param), detected by
looking for a FREED compound handle rooted at the param local in every return block.

### ~~CRITICAL ACCEPT-UNSAFE — block-scoped `defer free/consume` UAF~~ (`i0txin` `84097263`) — **CLOSED, measured 2026-08-08**
**Does NOT reproduce on main.** The reproducer below is rejected: `use after free: local %0 is freed
(freed at line 5)` — the SAME diagnostic and line as the non-defer control, i.e. the defer-specific
gap the entry describes is gone. Closed by the defer instance-id work (`ir_defer_instance_id` /
`ir_fire_has_work_after` in `zercheck_ir.c`), which is the `freed_defer_id` shape §F1 called for.
A `defer free(p)` (or `defer consume(m)`) inside a NESTED block frees/moves at BLOCK EXIT (the intended ZER
semantics), and the emitter emits the free there — but a USE after the block and before the function returns
is silently accepted.
```zer
{ defer free(p); p.val = 222; } // p freed HERE
?*Node mq = alloc(Node); // reuses the freed slot
u32 r = p.val; // UAF read — ACCEPTED; reads 999
```
**Root cause (emitter/zercheck semantic mismatch).** `ir_lower.c` emits a block-scoped `IR_DEFER_FIRE` at
each enclosing block exit and the emitter frees there (correct). But `zercheck_ir.c`'s forward pass (~4955)
treats `IR_DEFER_FIRE` as a NO-OP, and Phase C3 (~5658-5718) applies defer bodies' frees ONLY at `IR_RETURN`
blocks (function-exit / Go semantics) — so the analyzer only "knows" the free at function exit and misses the
mid-function use. A non-defer `free(p)` in the same inner block IS caught, so the gap is defer-specific;
function-level `defer free(p); return p.val;` remains CORRECT (fires after the use).
WARNING: **The trap in the obvious fix:** applying the fired body's free at the `IR_DEFER_FIRE` point closes the
UAF, but `ir_defer_scan_frees` (~1987) has a double-free detector guarded on `free_line != defer_line`, and
Phase C3 ALSO re-applies every defer body at each return block — the two differing lines then trip a FALSE
double-free. A correct fix must coordinate the two application points (either make the forward pass own ALL
`IR_DEFER_FIRE` and have C3 skip already-processed fires, or tag each defer block-scoped vs function-scoped).
DEPENDENCY: Interacts with **§F1** (`freed_defer_id` replaces exactly that line-based guard) — land §F1 first and this
fix gets easier.

### ~~CRITICAL/MEDIUM goto-defer double-fire → double-FREE on the SUCCESS path~~ (`3o10j6` `d1a7d9cc`) — **REPRODUCED with the branch's exact file, then FIXED 2026-08-10**

**Yesterday's "CLOSED, both variants" verdict was WRONG — it came from a hand-written
reconstruction.** Second instance of that failure mode in two days (see G3). The reconstruction put
the loop in `main` with an inline check; the real file has the label followed by `return;` in a
SEPARATE function, and that is what triggers it. The verbatim file
(`origin/claude/gifted-noether-3o10j6:tests/zer_gaps/gap_goto_out_of_loop_defer_double_fire.zer`)
gave `loop_d == 4` for a 3-iteration loop on the FIRST run.

**The defect.** `NODE_LABEL` (ir_lower.c) raised `defer_count` back to the goto's fired-count. A goto
inside a loop body records a fired-count that INCLUDES the loop-scoped defer — but on the
fall-through path that defer has already fired AND popped at every iteration exit. Raising re-armed
it for one extra fire at the label, so a 3-iteration loop fired its defer **4 times, on the path
where the goto was NEVER TAKEN** (the success path). With `defer free(x)` that is a double free.

**The fix is a REMOVAL, and the evidence for it is that code and rationale had diverged.** The
raise's own comment justified it as *"defers registered AFTER what the goto fired ALSO need to
fire"* — but those are already inside `ctx->defer_count` and never needed a raise. The raise could
only ever take effect when the goto had fired MORE defers than are currently live, which is exactly
the popped-scope case. `ctx->defer_count` at the label IS the live set on that path. `restore_count`
is still computed and still positions the guard flag; only the resurrecting assignment is gone.

**Checked before removing, not after:** the raise was introduced by `6c368761` together with two
regression tests (`defer_goto_fallthrough_zero_fire.zer`, `defer_goto_handle_leak_regression.zer`) —
**both still pass without it**, as does the whole suite and all 12 defer+goto tests.

**The 34 existing defer-goto matrix cells did NOT discriminate** — they measure an acquire/release
BALANCE, which is invariant to an extra fire that also acquires, so they passed both before and
after. Verified against a pre-fix build. Added a **fire-count sub-grid** (`LoopGoto` axis, 2 cells,
matrix now 36) that does: **35/36 pre-fix (fires 4, want 3) → 36/36 after.** Regression test
`tests/zer/defer_goto_loop_fire_count.zer` covers both arms (goto never taken = 3, taken at i==1 = 2)
and was verified to FAIL pre-fix.


### ~~HIGH spawn target reaches a non-shared global through a funcptr FIELD callback~~ (`02nq43` `7aac453a`) — **CLOSED, measured 2026-08-08**
**Does NOT reproduce on main** — rejected with an exact diagnostic: *"spawn target 'worker' calls through
a function-pointer field whose bound function accesses non-shared global 'g_ctr'"*, both when the carrier
is read as a global and when it is passed as a spawn arg. Closed by `5ed17c2f`
(`scan_funcptr_field_bindings`). **PROBE WARNING — the entry's own reproducer is MASKED twice:**
`spawn worker(&gw)` on a non-shared `W` is rejected by the non-shared-pointer-to-spawn rule, and reading
a non-shared global `gw` inside the target is rejected by the global-access rule. The carrier must be a
`shared struct` for the funcptr-field rule to be the one under test.
```zer
void worker(*W w) { w.cb(); } // call through a funcptr FIELD
u32 main() { gw.cb = do_inc; spawn worker(&gw); return 0; } // ACCEPTED — g_ctr races
```
The DIRECT form (`worker` writing `g_ctr` itself) IS correctly rejected. This is the register-in-setup /
invoke-in-worker RTOS pattern, so the binding `gw.cb = do_inc` lives in `main`, OUTSIDE the scanned `worker`
body; a funcptr FIELD callee (`NODE_FIELD`) is not resolved. Same "check fires lexically but misses the access
reached through a funcptr the sink doesn't match" shape as the closed SPAWN-FP / ISR-TRANS holes (#11/#12),
one sink further out. **Sibling of §D8** (which closes the funcptr-ARG facet) — take §D8 first, then decide
whether the FIELD facet warrants the same treatment.

### ~~HIGH spawn target reaches a non-shared global through a RETURNED funcptr~~ (`rvek5f` `00f3c2af` HOLE-D) — **FIXED 2026-08-08** (LOCAL half was `00dc785a`)
Third and last form of one question. `scan_funcname_binding` required a `NODE_IDENT`, so a funcptr
obtained from a FACTORY CALL (`*() fp = get_fp(); fp();`) resolved to nothing and the callback was
never scanned. New `scan_returned_funcname` walks the callee's `return <global function name>` sites
and scans each returned body through the existing global-access scan. Flagging on ANY returned name
is the sound direction — if a racing function CAN be returned, the race is reachable. Depth-guarded
by the shared `_scan_global_depth`; a computed or param return resolves to nothing (no new rejection).
Tests: `tests/zer_fail/spawn_funcptr_returned_race.zer` (verified ACCEPTED pre-fix) +
`tests/zer/spawn_funcptr_returned_safe_ok.zer` (four over-rejection controls: no-op, threadlocal,
`@atomic_*`, and a factory used outside `spawn`).

**A FIFTH form was found by enumerating for the gate, not by any report:** a factory returning
ANOTHER factory's result (`get_a(){ return get_b(); }`) — the return expr is a `NODE_CALL`, so the
new resolver bailed exactly as the old one had. Closed by recursing through `scan_funcname_binding`
(bounded by `_scan_global_depth`; a self-recursive factory terminates). Four sequential fixes had
not exhausted this class. The whole REACH axis is now gated by the funcptr-reach grid in
`tests/test_conc_matrix.c` (6 reach forms x 4 payloads, verified firing: 65/67 with 2 false
negatives pre-fix, 67/67 after).
```zer
*() get_fp() { return touch; }
void worker() { *() fp = get_fp(); fp(); } // indirect call — scanner gives up
```
Genuine unsynchronized race; compiles clean with only a stack-depth *warning*. `scan_unsafe_global_access`
(checker.c ~10203-10264) follows only calls whose callee is a named global function (plus function-NAMES
passed as args, BH-18 #8). A call through a funcptr obtained from a return value / local is unresolvable →
the scanner gives up and the data-race rule is bypassed. Direct `fp = touch; fp()`, a global funcptr, and a
struct-field funcptr are all caught (the funcptr STORAGE is itself a flagged non-shared global) — only the
returned/local funcptr leaks.
**Fix sketch:** a spawn target (transitively) calling through an UNRESOLVABLE funcptr cannot be proven free of
non-shared-global access → conservatively REJECT (the can't-prove-safe ⇒ reject barrier discipline). NOT done
on the branch because it risks broadly over-rejecting legitimate callback-in-thread patterns; the
safe/ergonomic boundary is a design call.
Together with the funcptr-FIELD item above and §D8, this completes the spawn-funcptr facet map:
**ARG = fixed (§D8), FIELD = open, RETURNED/local = open, direct/global/struct-storage = already caught.**

### ~~HIGH G3 — atomic-cell plain access not transitive through a helper~~ (`3o10j6` + `38z6wi` + `fxvnsu` G3) — **REPRODUCED with the branch's exact file, then FIXED 2026-08-09**

**The hand-written reconstruction was WRONG, and the "did not reproduce in 5 shapes" verdict it
produced was wrong with it.** The probes used a helper taking a POINTER (`helper(*C p){ p.v += 1; }`);
the real shape is a helper writing the global BY NAME (`poke(){ g_ctr = 5; }`). Pulling the verbatim
file from `origin/claude/gifted-noether-fxvnsu:tests/zer_gaps/gap_atomic_cell_plain_access_via_helper.zer`
reproduced it immediately. **This is the concrete cost of a missing reproducer file** — exactly the
risk flagged when four G-probes had to be measured with reconstructions. Reconstruct only when you
must, say so, and go get the original before trusting a NEGATIVE result.

**The defect.** "Is this global an atomic cell?" is whole-program, but "flag the plain access" fired
only for accesses LEXICALLY inside the spawning function (gated on the per-function
`c->after_spawn_in_func`). So `spawn worker(); g_ctr = 5;` was rejected while `spawn worker(); poke();`
with `poke(){ g_ctr = 5; }` was accepted — **moving a statement into a helper changed whether it
raced.** TSan-confirmed on the branch.

**The fix** mirrors the ISR-TRANS fix (#12) — the same per-function to transitive shape. New
`record_atomic_plain_in_callee` (checker.c) descends a callee's body at a `NODE_CALL` made while a
fire-and-forget spawn is live and records its plain global accesses; the existing post-check
`check_atomic_cell_safety` still makes the decision, so a global that never becomes an atomic cell is
unaffected and this adds no rejection on its own. `@atomic_*` arg0 is skipped (the blessed atomic
access), mirroring `in_atomic_intrinsic_arg` on the direct path. Depth-8 bounded; 2-hop verified;
plain READS caught as well as writes.

**Over-rejection boundary pinned** by `tests/zer/atomic_cell_helper_safe_ok.zer`: a helper that
synchronizes with `@atomic_*`, one touching an unrelated global, one where no spawn exists, and one
called BEFORE the spawn all still compile. Negative =
`tests/zer_fail/atomic_cell_plain_via_helper.zer`, kept **byte-identical to the branch file** so it
pins the finding rather than a reconstruction; verified ACCEPTED on a pre-fix build.
Probed 2026-08-08: `&global` handed to a helper, the same two-hop (`worker -> mid -> helper`), and a
plain struct write through a helper with the cell also touched atomically — **all three REJECTED**
("spawn target accesses non-shared global"). The one shape that was accepted had BOTH sides atomic
(`@atomic_add` + `@atomic_store`), which is correct acceptance, not a hole. Either the transitive scan
now covers this, or the live shape is narrower than described. **Do not reopen without a reproducer that
is verified to compile clean on main** — and check the rejection REASON, since the plain-global rule
masks this one easily.
Already tracked as **G3** in the 2026-07-19 §G section — re-verified still open on both branches
(TSan-confirmed). Recorded here only so the wave's coverage map is complete; do not duplicate the entry.
The unified root with the now-fixed §D6/§D7 borrow holes: the scoped-borrow / plain-access analysis is
intra-name and intra-function and does not treat `&x` handed to a helper as an access. Making it
inter-procedural (a summary "does this callee access/borrow its pointer arg?") is subsystem-scale.

### ~~HIGH — variable-index bit-slice WRITE: unclamped position shift is C UB~~ (`n0odo5` `17ec74ca`) — **FIXED `ec64526e` 2026-08-06** (`emit_bitslice_runtime_mask`, both emitter paths; test uses a `volatile` position so it discriminates at `-O2`)
`reg[hi..lo] = v` with a RUNTIME `lo` lowers to `(uint64_t)(v) << _zer_bl` and `mask << _zer_bl`
(emitter.c ~1708-1743 AST path + the IR mirror). When `lo >= 64` the shift count is out of range → C UB
(x86 masks mod 64, other arches differ), violating ZER's stated "shift by ≥ width = 0 (defined)" guarantee.
The WIDTH computation is already clamped; the POSITION shift is not. The companion bit-slice READ path was
fixed for exactly this (audit #18, commit `c9e4abca`); the WRITE path was not.
**Severity LOW** — the result is stored back through `*_zer_bp` typed to the carrier width, so the store
truncates: a wrong VALUE / UB, **not** an out-of-bounds memory write.

### LOW LOW — value-returning `async` has no result-retrieval API (`7fxhb3` `31796ef8`, 2026-07-28)
`async u32 compute() { … return 42; }` compiles clean and the state machine correctly finalizes (BH-18 #10,
fixed 2026-06-26). But the returned value is stored in an internal temp (`self->_zer_t0`) with **no
caller-accessible accessor** — a user who writes `async <non-void>` can never read the result. Neither
rejected nor retrievable = a silent footgun. Resolution is either a real retrieval mechanism (a stable
`.result` field + `_zer_async_NAME_result(&task)`, distinct from the `int` poll done-flag) or REJECT
`async <non-void>` until such an API exists. Not memory-unsafe (there is no valid usage today).

### ~~LOW Over-rejection — container field of `Handle(T)` drops T substitution~~ (`i0txin` `84097263`) — **`Handle(T)` FIXED `9ea6c864` 2026-08-06**; `Pool`/`Ring`/`Slab` stay rejected (emitter cannot stamp inline storage) — message quality tracked as its own OPEN entry
`subst_typenode` (checker.c ~2158-2219) treats `TYNODE_HANDLE`/`POOL`/`RING`/`SLAB` as LEAVES and does not
recurse into their `.elem`, so `container W(T) { Handle(T) h; }` fails to substitute and emits a LOUD
`error: undefined type 'T'` at the template line. Fails loudly — no silent miscompile. The documented
`T`/`*T`/`?T`/`[*]T`/`T[N]` shapes substitute correctly. **Fix:** recurse into `.elem`/`.inner` for those
four TypeNode kinds.

### ~~LOW Over-rejection — struct-by-value return carrying a PARAM-view field~~ (`rvek5f` `00f3c2af`) — **FIXED `5748e904` 2026-08-06** (view-alias family form 5; gated by `tests/test_view_alias_matrix.c`)
`struct Sl { [*]u32 s; } Sl mk([*]u32 p) { Sl r; r.s = p[0..2]; return r; }` is rejected
("return pointer to local 'r'") though it is safe — the struct copy carries a CALLER-memory view out. The
bare-param-view relaxation (BUG-764) covers `return p[0..2];` but not a param view wrapped in a
returned-by-value struct. Errs conservative → no soundness threat.

### LOW Doc mismatch — bit-extraction on a `volatile *u32` MMIO register is over-rejected (`rvek5f` `00f3c2af`)
CLAUDE.md's "Hardware Support" quick reference shows
`volatile *u32 reg = @inttoptr(...); u32 bits = reg[9..8];`, but `reg[hi..lo]` on a POINTER parses as a slice
range → `error: slice start (9) is greater than end (8)`. Bit-extraction only works on a scalar VALUE.
Verified workaround: `u32 v = *reg; u32 bits = v[9..8];`. Either fix the docs to show the deref form, or make
`reg[hi..lo]` on a volatile scalar pointer auto-deref for bit-extraction.

### Cross-references (already tracked elsewhere in this file — do NOT duplicate)
- **HOLE-C / cross-block scoped-borrow, join-in-branch** (`rvek5f` `00f3c2af`) — **FIXED 2026-08-03.**
  rvek5f's trigger was exact: `th.join()` inside a branch cleared the per-Symbol `is_borrowed_by_thread`
  flag in AST-walk order, so an unguarded access on the OTHER (un-joined) path was not flagged.
  **Its proposed fix location was wrong** — the borrow lives in `checker.c` as a linear statement-order
  approximation, NOT in the IR analyzer, so a "zercheck_ir borrow-set CFG merge" would have edited a
  subsystem that never held the state. The actual fix is `Checker.branch_depth` +
  `Symbol.th_spawn_branch_depth`: a join releases the borrow only when it is no deeper in
  runtime-conditional nesting than the spawn. Regression test
  `tests/zer_fail/scoped_borrow_crossblock_early_return.zer` (verified to COMPILE pre-fix); boundary
  positives in `tests/zer/scoped_borrow_branch_shapes.zer`. The residual precision cost (a join on EVERY
  arm is now over-rejected) is its own OPEN entry above.
- **`?T` optional-wrapper class-kill — BUILT 2026-08-01.** Was the OPEN "optional-unwrap" class-kill.
  Closed as ONE gate for the whole wrapper family, not a `?T`-specific one: the wrapper set that hides
  an inner kind is FINITE (`?T`, `distinct T`, array-of, by-value struct/union CARRYING a pointer) and
  the 2026-08-01 sweep hit THREE of the four (C2/D2 optional, C7/D1 struct-carrier, C3/C4
  slice+distinct) — an optional-only gate would have left the struct-carrier shape open.
  - **Gate A (author-time)** `tools/audit_carrier_dispatch.sh` + `carrier_dispatch_baseline.txt`:
    freezes the 33 hand-rolled carrier disjunctions in checker.c/zercheck_ir.c and FAILS on a new one,
    forcing either a carrier predicate (`type_carries_data_pointer` / `type_can_carry_pointer` /
    `escape_type_carries_ref`) or a justified baseline row. Wired into `make check` (gate 6 of 6).
    Verified to fire by injecting a violation.
  - **Gate B (exhaustive)** `LD_OPTWRAP` axis in `tests/test_escape_matrix.c`: the escaping value is
    bound through an optional carrier before reaching each sink. `-Wswitch` on the enum forces every
    generator/name/validity site to handle it, so the grid cannot silently shrink. 35 -> 43 cells,
    all reject, 0 false negatives.
  - **NOT a blanket accessor** (the distinct playbook does not transfer): unwrapping `?T` is correct at
    a safety sink but WRONG at the ~93 emitter sites that dispatch on TYPE_OPTIONAL to decide
    `.has_value` / null-sentinel emission. Hence a linter that forces a per-site choice, not a rewrite.
  - Residual: the 33 baselined rows are "known, untriaged" — Gate B answers whether each handles every
    wrapper. Prefer converting a row to a predicate and DELETING it over leaving it frozen.
- **`vrp_ir.c` is fully dead code** (`rvek5f` `00f3c2af`) — already noted here; rvek5f re-confirms `vrp_ir()`
  / `IRVRPResult` have ZERO callers and are not in the Makefile, while all load-bearing bounds/division VRP
  is the AST VRP in checker.c. Either wire it (the tracked "wire the orphaned vrp_ir.c" direction) or delete
  it to stop implying live coverage.

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

## OPEN — BUGS: fixes available on four `claude/*` branches (fxvnsu / yd5ajq / 0h7oz9 / c4c09l), verified NOT in main (2026-07-19)

**What this is.** A SECOND set of `claude/*` audit branches, created AFTER the 41-fix tracker
above closed. Cross-referenced 2026-07-19 against current main (`5fe952f3`): **22 distinct fixes
are NOT in main** (each verified by grepping the fix's distinctive marker AND reading the main
code region) + **5 documented-but-unfixed gaps** (fxvnsu, reproducers already in
`tests/zer_gaps/`). Two of the 24 fixes ARE already in main (M1/M2 — do NOT re-open). Same
consumption rules as the 41-tracker: apply the PROPER version per bug, cherry-pick/rebase onto
HEAD, re-verify (build + neg/pos + FOREGROUND `make check` + sink matrix for escape/free).
`git show <sha>` to inspect. Severity: ~13 memory-safety (UAF/OOB/race incl. an analyzer
heap-UAF), ~9 miscompiles.

**Root-cause pattern (why these exist — so a fresh session sees the shape, not 22 unrelated
bugs).** Almost all are the MULTI-SITE / PER-NODE-KIND patchwork class (CLAUDE.md "MULTI-SITE
SAFETY IS THE #1 RECURRING BUG CLASS"): the same question answered independently at N
sites/node-kinds, a new form silently missed. The clusters: (a) VRP-JOIN wired per node-kind —
only NODE_IF landed via §C #13, so switch/for/goto/do-while still leak (§A #1–#4); (b) the `?T`
optional wrapper hiding the inner kind at N escape/coercion sites — NO enforced unwrap gate (the
OPEN "optional-unwrap" class-kill), producing §B #7–#9 + §D #14–#16; (c) the two emitter dispatch
paths (AST ~3xxx + IR ~7xxx) — §D; (d) uN/iN mask not threaded through every value op — §D #13/#17.
Coverage was audit-found, not proof-found. The durable end-states are the class-kills
(one-query-plus-a-gate) noted in CLAUDE.md.

### Source branches (fork base → commits)
| Branch | Base | Commits (short) |
|---|---|---|
| gifted-noether-fxvnsu | 5fe952f3 (current HEAD) | 9fea9990 |
| gifted-noether-yd5ajq | 260a80d9 | c6b72dc0, a44bbfd0, 11621483 |
| gifted-noether-0h7oz9 | 065679bd | 65fea9a9, d9bfb368, ea8264f5, 395ea87e, c9e4abca, 5f783476, 12684dc4 |
| gifted-noether-c4c09l | 3cc45d1d | c8badf4a, 6e72b400 |

### Already in main — do NOT re-open (2)
- **M1 — VRP-1 NODE_IF conditional-narrow leak** (`0h7oz9` `395ea87e`). CLOSED in main via **§C #13**
  (`vrp_snap_take/restore/join`, checker.c ~11128). 0h7oz9's `vrp_snapshot`/`vrp_join_after_region`
  is a REDUNDANT different impl — skip it (its `vrp_conditional_narrow_no_leak.zer` duplicates main's
  coverage).
- **M2 — §B#13 spawn by-value STRUCT/UNION carrying stack ptr** (`0h7oz9` `5f783476`). CLOSED in main
  (checker.c ~13598, the `else if (!is_scoped && (STRUCT||UNION) && spawn_arg_is_stack_derived)` arm).
  0h7oz9 additionally lists by-value **ARRAY** + a `type_carries_data_pointer` precision gate — marginal
  & likely moot (by-value arrays coerce to slices at the call site). Core closed; ARRAY extension is
  low-value.

### A. VRP range-JOIN class → silent OOB — ALL 4 DONE (landed 2026-07-19, make check 990/0)
**DONE 2026-07-19:** all four siblings (#1 switch, #2 for-body, #3 goto/label, #4 do-while)
applied to `checker.c` + regression tests (3 in `tests/zer_trap/` compile-clean + TRAP=133, the
do-while positive in `tests/zer/`). make check 990/0, all audits + sink matrix clean.
The branch-merge JOIN is re-implemented per node-kind. Main has NODE_IF (§C #13) + while/do-while
body-writes (`vrp_invalidate_loop_body_writes`, BUG-748). These four kinds still leak a narrowed range
past the join → a later `arr[idx]` proven in-bounds on a path where idx is wild → bounds guard elided →
silent OOB (read AND write; ASan-confirmed on the branches).

- **#1 — switch-arm range narrow leaks past the join** (`yd5ajq` `11621483` VRP#3). `switch(sel){ 7 =>
  {idx = wild%4;} default => {} }` mutates the VarRange in place; NODE_SWITCH has no
  snapshot/restore/union-join → [0,3] leaks to `arr[idx]` on the default path. Fix: mirror the if-handler
  (snapshot pre-switch, `vrp_snap_restore` before each arm, accumulate the UNION seeded with pre-switch
  state, restore the union after). Test `tests/zer_trap/vrp_switch_arm_range_leak.zer`. **In-main: NOT
  present** — main's NODE_SWITCH handler (checker.c ~11491) has ZERO `vrp_snap`/`var_range` save/restore.
- **#2 — for-body other-var range leak** (`yd5ajq` `11621483` VRP#4). `u32 j=0; for(i..){ arr[j]=..;
  j+=1; }` — the for-handler's `check_expr(step)` re-widens only the LOOP var, so `j` is seen as its
  pre-loop [0,0] in the body. Fix: add the `vrp_invalidate_loop_body_writes` pre-pass (the while/do-while
  handler already has it) before the loop-var range push. Test `tests/zer_trap/vrp_for_body_range_leak.zer`.
  **In-main: NOT present** — main's for-handler (checker.c ~11370) never calls
  `vrp_invalidate_loop_body_writes`.
- **#3 — goto/label ignored by single-pass VRP** (`yd5ajq` `a44bbfd0` VRP#5). `idx=wild%256; goto skip;
  narrow: idx=wild%4; skip: arr[idx]` — the source-order walk narrows at `narrow:` though `goto skip`
  bypasses it at runtime (also a backward-goto loop). Fix: NODE_LABEL handler widens EVERY tracked VRP
  range (a goto can enter carrying any value → sound over-approx of the join over all incoming edges).
  Test `tests/zer_trap/vrp_goto_skip_narrow_leak.zer`. **In-main: NOT present** — main's NODE_LABEL
  (checker.c ~12441) is just `break` ("labels are just markers").
- **#4 — do-while applies bottom-condition range to the first (unguarded) body iteration** (`fxvnsu`
  `9fea9990` BUG-D). `u32 i=seed(); do { s+=buf[i]; i+=1; } while (i<4);` with unprovable entry i=10 elides
  the guard. while/do-while SHARE the cond-narrow (`push_var_range` from the cond) — sound for while (cond
  checked first), unsound for do-while (body runs first). Fix: gate the cond-derived narrowing to
  `node->kind != NODE_DO_WHILE` (`vrp_invalidate_loop_body_writes` already widened body-written vars, so
  the loop var stays full-range → emitter inserts the runtime guard). Test
  `tests/zer/dowhile_bounds_first_iter.zer`. Distinct from §C #14 (do-while `find_return_range`).
  **In-main: NOT present** — main narrows for BOTH kinds (checker.c ~11464, no `!= NODE_DO_WHILE` gate).

### B. Escape / dangling-pointer class → accept-unsafe UAF — ALL 5 DONE (landed 2026-07-19)
**DONE: #7/#8/#9 DONE (make check 994/0):** the c4c09l `?[*]T`/`?*T` optional-carrier sub-cluster —
keep-registration + persist sink now taint TYPE_OPTIONAL carriers (#7); the return-dangling (#8)
and array-store-to-global (#9) escape checks now accept optional-of-slice (via `type_dispatch_kind`,
audit-clean). 4 negatives in `tests/zer_fail/` reject; ground-truth-probed bug-present-on-main first.
**DONE: #5/#6 DONE (make check 996/0, sink matrix 32→41 CLEAN):** yd5ajq intrinsic-launder (#5, new
`unwrap_ptr_launder` helper at the assign + orelse-fallback taint sinks — `@ptrcast(&local)` is
NODE_INTRINSIC, not NODE_UNARY) + struct-element-copy (#6, store-to-global descend gate widened to
`escape_type_carries_ref(vt) || type_can_carry_pointer(vt)`). Tests
`tests/zer_fail/escape_{intrinsic_field_store_global,array_elem_struct_copy_global}.zer` +
sink-matrix cells p11/p12 (+ 3 safe baselines); the 5 `rust_tests/rt_opaque_*` were converted to the
long-lived-context idiom (they had relied on the #5 loophole). Whole class closed.
Same escape class as CLAUDE.md's per-sink patchwork note, at 5 sinks main's taint missed. All
ASan-confirmed `stack-use-after-return`/`stack-buffer-overflow` on the branches. #7/#8/#9 are the
`?[*]T`/`?*T` optional-carrier sub-cluster (the OPEN "optional-unwrap" class-kill).

- **#5 — intrinsic-wrapped `&local` into a struct field escapes un-tainted** (`yd5ajq` `11621483`
  Escape#1). `Box b; b.p = @ptrcast(*u32,&local); g_box = b;` compiles → `g_box.p` dangles. The assign-sink
  taint marking the container root `is_local_derived` matched only a BARE `&local` (NODE_UNARY);
  `@ptrcast`/`@pun`/`@bitcast`/`@cast`/`@container`-wrapped `&local` is NODE_INTRINSIC → taint never fired.
  Fix: `unwrap_ptr_launder` helper (unwrap launder intrinsics → underlying pointer; @container→args[0],
  else last arg) at the assign-taint site + its orelse-fallback. Sink cell `p11`. Test
  `tests/zer_fail/escape_intrinsic_field_store_global.zer`. **In-main: NOT present** — `unwrap_ptr_launder`
  = 0 hits.
- **#6 — struct-copy of a local-derived array/struct ELEMENT** (`yd5ajq` `11621483` Escape#2). `Box[2]
  arr; arr[0].p=&local; g=arr[0];` compiles. The store tainted the array root, but the store-to-global sink
  gated its descend-to-root on `escape_type_carries_ref(vt)` (pointer/slice/opt-of-those only) — `arr[0]`
  has value-type `Box` (a by-value struct transitively carrying a pointer), so descend was skipped. Fix:
  widen the gate to `escape_type_carries_ref(vt) || type_can_carry_pointer(vt)` (the root
  `is_local_derived` check still gates; pointerless struct/scalar copies stay accepted). Sink cell `p12`.
  Test `tests/zer_fail/escape_array_elem_struct_copy_global.zer`. **In-main: NOT present** — main's gate is
  bare `escape_type_carries_ref(vt)` (checker.c ~4559).
- **#7 — keep inference blind to `?[*]T`/`?*T` optional-carrier param** (`c4c09l` `c8badf4a`). `?[*]u8 g;
  stash(?[*]u8 s){ g=s; }` did NOT infer keep on `s` (the plain `[*]u8` param did), so
  `stash(local_array)`/`stash(&local)` compiled → dangling global slice. Two per-sink lists omitted
  TYPE_OPTIONAL: param-registration + the keep-persist sink. Fix: add an optional-carrier branch at
  registration (`pk == TYPE_OPTIONAL && type_carries_data_pointer(ptype,0)` — the helper recurses the
  optional inner, so `?u32`/`?bool` stay untainted) and accept TYPE_OPTIONAL at the persist sink. Tests
  `tests/zer_fail/keep_optional_{slice,ptr}_param.zer`. **In-main: NOT present** — main's keep-registration
  (checker.c ~14899) lists POINTER/OPAQUE/SLICE, no TYPE_OPTIONAL branch.
- **#8 — `?[*]T` RETURN of a local array bypasses the escape check** (`c4c09l` `6e72b400` #4). `?[*]u8
  mk(){ u8[5] buf; return buf; }` compiles a dangling slice (the non-optional `[*]u8` form is rejected).
  The return-dangling check gated on `current_func_ret->kind == TYPE_SLICE`; the `?[*]T` (TYPE_OPTIONAL)
  wrapper hid it. Fix: unwrap the optional — a return target is slice-like if slice OR optional-of-slice.
  Test `tests/zer_fail/return_local_array_optslice.zer`. **In-main: NOT present** — main line ~12029 gates
  on bare `c->current_func_ret->kind == TYPE_SLICE`.
- **#9 — `?[*]T` global STORE of a local array bypasses the escape check** (`c4c09l` `6e72b400` #5).
  `?[*]u8 g; g = buf;` (local buf) compiles a dangling global pointer. Same root cause — the
  array→slice-into-global check gated on `target->kind == TYPE_SLICE`. Fix: widen to optional-of-slice
  targets. Test `tests/zer_fail/store_local_array_optslice_global.zer`. **In-main: NOT present** — main
  line ~5006 gates on `type_unwrap_distinct(target)->kind == TYPE_SLICE` (unwraps distinct, NOT optional).

### C. Concurrency / ISR (3)
**DONE: #10 DONE (landed 2026-07-19, make check 1002/0):** the B1 extra-lock emitter now CAPTURES the
locked roots into a caller array and the paired unlock REPLAYS exactly that set (reverse order)
instead of re-deriving — balanced across the destructive orelse rewrite — so the `!find_orelse` gate
is removed and `x = ga.maybe orelse gb.plain` locks gb too. Regression
`tests/zer/conc_orelse_multiroot_lock.zer`; the two `[16]` scratch arrays baselined.
**DONE: #11/#12 DONE (landed 2026-07-19, make check 1006/0):** #11 SPAWN-FP — `scan_unsafe_global_access`
now descends into a function-name binding (var-decl init / assignment / struct-init field) via
`scan_funcname_binding`, so a global RMW laundered through a local funcptr (`*() fp = do_inc; fp();`)
is no longer race-blind (shared file-scope depth budget). #12 ISR-TRANS — new `record_isr_globals`
walks the `interrupt {}` body and follows direct calls into global-function bodies (depth-guarded),
recording every global read + compound-assign target, so the "shared ISR/main → must be volatile" +
"volatile compound RMW → non-atomic" checks are transitive through helpers. Tests
`tests/zer_fail/{isr_transitive_rmw,isr_transitive_volatile,spawn_funcptr_local_race}.zer` +
`tests/zer/spawn_funcptr_shared_ok.zer`; `test_modules/hal.zer` updated to the safe pattern (it had
relied on the hole). **Concurrency/ISR class C COMPLETE.**
- **#10 — same-type two-instance shared read in an `orelse` under-locked → data race** (`yd5ajq`
  `c6b72dc0`, TSan-confirmed). `x = ga.maybe orelse gb.plain` where `ga`,`gb` are two instances of the
  SAME `shared struct S` locked only `ga` → `gb.plain` read unlocked. Two gaps: (1) the same-statement
  deadlock check dedups shared roots by `struct_type.type_id`, collapsing the same-type pair (a
  DIFFERENT-type pair IS caught); (2) the B1 extra-lock emitter was gated `if (se && !find_orelse(se))`
  (skipped for orelse, because unlock re-derives the root set and lowering destructively rewrites the
  NODE_ORELSE between lock/unlock). Fix: `emit_shared_lock_if_needed` CAPTURES the extra locked roots into
  a caller array; `emit_shared_unlock_if_needed` REPLAYS exactly that set (reverse order) instead of
  re-deriving → balanced across the rewrite → the `!find_orelse` gate is removed. Threaded through all 3
  lock/unlock call sites. Test `tests/zer/conc_orelse_multiroot_lock.zer`. **In-main: NOT present** —
  ir_lower.c ~1544/1562 still `if (se && !find_orelse(se))`.
- **#11 — spawn race scan blind to a global laundered via a local funcptr** (`0h7oz9` `ea8264f5`
  SPAWN-FP). `*() fp = do_inc; fp();` (or a funcptr struct field) — `scan_unsafe_global_access` followed a
  call transitively only when the callee resolved to a GLOBAL function, so a local funcptr callee's
  non-shared global RMW raced clean (reproducible lost update). Fix: `scan_funcname_binding` descends into
  a function-name binding (var-decl init / assignment / struct-init field), mirroring the existing
  function-name-as-arg descent; shared file-scope depth budget. Tests
  `tests/zer_fail/spawn_funcptr_local_race.zer` + `tests/zer/spawn_funcptr_shared_ok.zer`. **In-main: NOT
  present** — `scan_funcname_binding` = 0 hits.
- **#12 — ISR global-access checks not transitive through callees** (`0h7oz9` `ea8264f5` ISR-TRANS,
  silent bare-metal). The "shared ISR/main → must be volatile" + "volatile compound RMW → non-atomic"
  checks saw only globals lexically inside the `interrupt {}` body; an ISR touching a global through a
  helper bypassed both (non-volatile flag hoisted → hang; helper `counter += 1` torn RMW). Fix:
  `record_isr_globals` walks the interrupt body (in_interrupt), follows direct calls into global-function
  bodies (depth-guarded), records every global read + compound-assign target as an ISR access.
  (`test_modules/hal.zer` relied on the hole — updated to volatile + explicit read/write.) Tests
  `tests/zer_fail/isr_transitive_{volatile,rmw}.zer`. **In-main: NOT present** — `record_isr_globals` = 0
  hits.

### ~~D. Emitter miscompiles (7)~~ — **ALL 7 DONE** (#13/#17/#18/#19 landed 2026-07-19; #14/#15/#16 measured CLOSED 2026-08-09)
The array->`?[*]T` coercion family (#14 struct-init field, #15 var-decl/assign/call-arg/return, #16 global
array returned as `?[*]T`) was measured on main 2026-08-09: **all four value-flow sites carry `.len`
correctly** (`v.len == 4` at var-decl, call-arg, global return, and struct-init field; each run exits 0).
**DONE: #13/#17/#18/#19 DONE (landed 2026-07-19, make check 1000/0, all matrices + sink 41 CLEAN):**
#13 `@saturate(uN)` unsigned odd-width now clamps via `(1ULL<<w)-1` + carrier cast (both emitter
paths); #17 `@bitcast(uN/iN)` now masks/sign-extends the punned carrier via `emit_intn_mask_lv`
(both paths); #18 variable-index bit-extract now guards the POSITION shift on `type_width` (both
paths); #19 `volatile` scalar/aggregate locals now emit the qualifier (new `IRLocal.is_volatile`,
set in ir_lower, emitted in emit_regular_func_from_ir). 4 positive tests in `tests/zer/` compile +
run exit 0. **DONE: #15/#16 DONE + #14 mostly done (landed 2026-07-19, make check 1011/0, all audits +
sink 41 CLEAN):** the array→optional-slice coercion is now applied at every value-flow site —
`emit_opt_wrap_value` + two helpers (`struct_field_type_by_name`, `aggregate_slice_coerce_target`) +
AST/IR struct-init + IR var-decl `need_wrap` + IR assign-expression opt-wrap + **call-arg** opt-wrap +
the **#16 array→`?[*]T` return** branch (was falling through to a bare `return;` → caller saw None) — all
audit-clean via `type_dispatch_kind` (no baseline coupling). Verified: `optional_slice_coerce.zer`
(var-decl / assignment / call-arg / param-slice-return, exit 0) + `optional_slice_return_global.zer`
(global-array `?[*]T` return, exit 0). **DONE: #14 struct-init field COMPLETE (landed 2026-07-20, make
check 1012/0):** the missing site was the **IR_STRUCT_INIT_DECOMP** emitter (the var-decl struct-init
`Buf b = { .data = a }` / `W w = { .maybe = a }` path — distinct from the emit_rewritten_node
NODE_STRUCT_INIT path used by the assignment form). The DECOMP emitter has the field-value type
directly as `func->locals[vloc].type` (no node-typemap dependency), so the coercion builds the
`{ptr,len}` slice from the local name + `array.size` in both the optional-field-wrap and plain-slice-
field branches. Test `tests/zer/optional_slice_struct_field.zer` (var-decl + assignment, both field
kinds, exit 0). **The `?[*]T`/array→slice coercion class (#14/#15/#16) is now fully closed.**
  **DONE: BONUS — param-array-return relaxation SHIPPED (2026-07-20, make check 1014/0, sink matrix 44
  CLEAN):** returning a `u8[N]` array PARAM as a slice (`[*]u8 f(u8[N] a){ return a; }` / `?[*]u8`) is
  now ACCEPTED — array params are by-reference (they decay to a pointer into the CALLER's array,
  empirically verified), so the returned slice views caller memory, sound exactly like the BUG-764
  slice/pointer-param view class. The return-escape check (checker.c ~12287) now skips its error when
  the root ident is a param; the emitter (src1_local return path, both the `?[*]T`-wrap and plain
  `[*]T` branches) builds the `{ptr,len}` slice from the local name + `array.size`. **Accept-unsafe
  VERIFIED SAFE:** `classify_return_root` already records the param index in `ret_param_mask`, so the
  CALL SITE rejects a caller that passes a LOCAL and lets the result escape (`g = f(local)` ->
  "local-derived pointer argument ... may escape"). Tests `tests/zer/return_param_array_ok.zer` (local
  use, exit 0) + `tests/zer_fail/return_param_array_escape.zer` (escape rejected) + sink-matrix cells
  p13 (reject escape) + safe_return_param_arr_{opt,slice} (accept). A LOCAL array return still dangles
  -> still rejected.
  **Prior investigation 2026-07-19 (first attempt reverted — do NOT half-apply):** this is a **7-site emitter
  patchwork** — `emit_opt_wrap_value` coercion + two helpers (`struct_field_type_by_name`,
  `aggregate_slice_coerce_target`) + AST struct-init + IR struct-init (both wrap/non-wrap branches) +
  IR var-decl `need_wrap` + IR **assign** `need_wrap` + IR **return** `need_wrap` (the `emit_local_name`
  path — structurally different, harder) + **call-arg**. 5 of these were applied audit-clean (via
  `type_dispatch_kind`, no baseline coupling — fxvnsu's version instead baselined 14 raw `->kind` reads)
  and BUILD-GREEN, but the 3 tests still failed: `optional_slice_coerce` (run≠0 — assign/call-arg site)
  and `optional_slice_return_global` (run≠0 — the return `need_wrap` site) need the remaining sites.
  **CHECKER INTERACTION (the real blocker):** the fxvnsu `optional_slice_from_array` test does
  `?[*]u8 get(u8[4] a){ return a; }` (a is a PARAM array) — my just-landed **#8** fix (return an
  array as `?[*]T` now hits the escape check, consistent with the pre-existing NON-optional check)
  **rejects it**, where before #8 the optional wrapper hid it. Before finishing #14-16, DECIDE: is
  `return param_array` as a slice safe? If ZER array params decay to a caller-memory pointer (like a
  slice/pointer param — BUG-764 allows returning views of those), then #8 should be RELAXED for param
  arrays and the test compiles; if array params are by-value copies, #8 correctly rejects and the test
  must source from a global. Resolve that first, then apply the remaining 3 emitter sites + bring the 3
  tests (`optional_slice_{from_array,coerce,return_global}.zer`). Silent-miscompile severity (wrong
  `.len`), NOT memory-safety — lowest priority of the 22.
- **#13 — `@saturate(uN, v)` for a non-native UNSIGNED width never clamps** (`fxvnsu` `9fea9990` BUG-A).
  `@saturate(u7, 200)` returns 200 not 127. The unsigned emit path (BOTH emitter paths) hardcoded the max
  on a `{8,16,32,else→u64}` switch → every odd width fell to the u64 branch (never clamps), result cast to
  the carrier with no N-bit mask (the signed path already computed min/max from the width → correct). Fix:
  compute the unsigned max as `(1ULL<<w)-1` for `w<64` (keep the `.0` double form for `w>=64`), cast via
  `emit_type`. Test `tests/zer/saturate_unsigned_odd_width.zer`. **In-main: NOT present** — main emitter.c
  ~7186 hardcoded `{8,16,32,else}` switch.
- **#14 — fixed array into an optional-slice / slice AGGREGATE field not coerced (`.len` dropped)**
  (`fxvnsu` `9fea9990` BUG-B). `?[*]u8 s=a; return a; W w={.maybe=a}; Buf b={.data=a};` + the assignment
  form emitted the bare array ident; C brace-flattening dropped it into `.value.ptr`/`.len` (optional-slice:
  swallowed `has_value` into `.value.len` → a PRESENT optional built EMPTY; plain slice: `.len=0` → false
  OOB trap). Scalar/var-decl/call-arg paths coerced; the AGGREGATE-construction paths (optional-wrap,
  return-into-optional, struct-init field — across AST + IR + IR_STRUCT_INIT_DECOMP) did not. Fix: shared
  `struct_field_type_by_name` + `aggregate_slice_coerce_target` helpers; `emit_opt_wrap_value` coerces its
  `?slice` inner; all six aggregate sites coerce array→slice before the wrap. Test
  `tests/zer/optional_slice_from_array.zer`. **SUPERSET of #15/#16** — fxvnsu is on current HEAD, so this is
  the canonical base. **In-main: NOT present** — `aggregate_slice_coerce_target` absent; `need_wrap`
  (emitter.c ~9799) emits `(opt){<array>,1}` → `.len=0`.
- **#15 — array→`?[*]T` coercion dropped `.len` at var-decl/assign/call-arg/return** (`c4c09l` `6e72b400`
  #3). SAME CLASS as #14, narrower/earlier. Fixed the four value-flow sites to build `{ptr,len}` inside the
  optional when `optional.inner` is a slice and the source is an array. Tests
  `tests/zer/optional_slice_coerce.zer` + `optional_slice_return_global.zer`. (c4c09l covers the call-arg
  site; #14 covers struct-init-field — merge as ONE class, base on #14.) **In-main: NOT present** (same
  evidence as #14).
- **#16 — global array returned as `?[*]T` emits a bare `return`** (`c4c09l` `6e72b400` #6). The
  array→slice-return coercion gated on `ret_eff->kind == TYPE_SLICE` → an optional return fell to the void
  branch → caller saw None. Fixed alongside #15. Test `optional_slice_return_global.zer`. **In-main: NOT
  present** (same class).
- **#17 — `@bitcast(uN/iN, x)` never masked/sign-extended the punned carrier** (`c4c09l` `6e72b400` #1).
  `@bitcast(u5, i5(-1))` yielded 255 not 31; `@bitcast(i5, u5(31))` yielded 31 not -1. The memcpy-pun into a
  carrier temp omitted the non-native uN/iN mask `@truncate` has. Fix: apply `emit_intn_mask_lv` to
  `_zer_bco` when `type_is_nonnative_intn(t)` on BOTH dispatch paths. Test `tests/zer/bitcast_intn.zer`.
  **In-main: NOT present** — main emitter.c ~3204/7208 emit `_zer_bco` with no mask.
- **#18 — variable-index bit-extract emitted an unguarded POSITION shift (UB)** (`0h7oz9` `c9e4abca`
  BITSHIFT). `reg[hi..lo]` with runtime `lo` lowered to raw `reg >> _zer_lo` — C UB when `lo >= width`
  (GCC folds it differently at -O0 vs -O2 and on ARM/RISC-V), violating ZER's "shift by ≥ width = 0". The
  F8 fix guarded the extract-width MASK but not the shift. Fix: guard the shift on the object's `type_width`
  in both emit paths. Test `tests/zer/bitslice_read_runtime_shift_guard.zer`. **In-main: NOT present** —
  main emitter.c ~2565 emits raw `>> _zer_lo` (only the mask `& ((_zer_w>=64)?...)` is guarded).
- **#19 — volatile qualifier dropped on scalar/aggregate locals** (`0h7oz9` `d9bfb368` VOL-1, silent
  bare-metal). `volatile u32 d;` delay/timing loop silently optimized away — scalar/aggregate locals carry
  no volatile in their Type (only slice/pointer do), so the emitter emitted no qualifier. Fix: add an
  `is_volatile` IR-local flag (ir.h), set in ir_lower.c, emit the qualifier in emitter.c. Test
  `tests/zer/volatile_scalar_local.zer`. **In-main: NOT present** — `is_volatile` absent from ir.h.

### E. Checker miscompiles (2) — DONE: BOTH DONE (landed 2026-07-19, make check 1009/0)
**DONE: #20/#21 DONE:** #20 LIT-1 — `retype_const_int_to_target` retypes a PURE integer-literal expr to
the destination integer width (`is_pure_int_literal_expr` / `int_retype_target`) at var-decl init,
return, call-arg, struct designated-init field, AND binary-operand promotion, so `i64 a=-3` /
`u64=1<<40` compute in the target width (test `tests/zer/const_int_target_width.zer`). #21 orelse-block
— a non-diverging `orelse { block }` in value position now types `void` (via `orelse_block_diverges`,
an if-chain outside the -Wswitch audit) so the existing "cannot initialize with void" check rejects it,
while a diverging block keeps the unwrapped type and a bare-statement `f() orelse {…}` stays legal
(tests `tests/zer/orelse_block_diverge.zer` + `tests/zer_fail/orelse_block_value_nondiverge.zer`).
Checker-miscompile class E COMPLETE.
- **#20 — negative / u32-overflowing integer literal into a 64-bit type computed in u32** (`0h7oz9`
  `65fea9a9` LIT-1). `i64 a=-3` = 4294967293; `u64 x=100000*100000` wraps at 2^32; `u64 s=1<<40` truncated.
  Literals lowered in u32/u64 by value width; the assignment path already const-folds to the target width,
  but the other value-flow sites lowered u32-typed temps. Fix: `retype_const_int_to_target` retypes a PURE
  integer-literal expr (`is_pure_int_literal_expr`) to the destination integer width (`int_retype_target`,
  unwraps `?integer`) at var-decl init, return, call-arg, struct designated-init field, AND binary-operand
  promotion. Narrow targets still wrap; non-fitting constants still rejected. Test
  `tests/zer/const_int_target_width.zer`. **In-main: NOT present** — `retype_const_int_to_target` = 0 hits.
- **#21 — non-diverging `orelse { block }` in value position silently yields 0** (`c4c09l` `6e72b400`
  #2). `u32 x = f() orelse { g=1; };` (block falls through) compiles and sets x=0 — the checker typed the
  orelse `unwrapped` unconditionally (stale "GCC rejects value-position stmt-exprs" assumption the IR
  auto-zeroed temp defeats). Fix: `orelse_block_diverges()` helper (tail return/break/continue/goto,
  nested-if both-arms-diverge, `orelse return/...` tail); a FALL-THROUGH block types the orelse `void`
  (existing "cannot initialize with void" rejects value use); a bare-statement `f() orelse {...}` stays
  legal. Tests `tests/zer/orelse_block_diverge.zer` + `tests/zer_fail/orelse_block_value_nondiverge.zer`.
  **In-main: NOT present** — `orelse_block_diverges` = 0 hits.

### F. Analyzer heap-UAF (1 — a crash / stale safety decision in zercheck itself) — DONE (2026-07-19)
**DONE: #22 DONE (make check 1002/0):** the IR_ASSIGN non-move alias branch's invalid-use check is now
`else if` (mutually exclusive with the ALIVE-add branch that reallocs `ps->handles`), so the
dangling `src_h` is never re-read. Regression `tests/zer/handle_alias_realloc_uaf.zer` (10 straight-line
?Handle/orelse alias pairs to cross the capacity-8 realloc; compiles + runs exit 0 — the true guard is
an ASan build).
- **#22 — heap-use-after-free reading `src_h` after `ir_add_handle` realloc** (`0h7oz9` `12684dc4`). The
  non-move alias branch of `ir_check_inst` (IR_ASSIGN) captured `src_h` as a raw pointer into `ps->handles`,
  then called `ir_add_handle(dest_local)` which can realloc that array; the subsequent invalid-handle check
  dereferenced the dangling `src_h` (ASan crash on valid input; a stale safety DECISION on the shipped
  build, where the freed region is still mapped). The alive-add branch and the invalid-use check are
  mutually exclusive (ALIVE ⟹ not invalid). Fix: convert the second `if (src_h && ir_is_invalid(src_h))`
  to `else if`. Repro: two `?Handle`/orelse + array-store pairs push handle_count past capacity 8 at the
  aliasing assignment. (No dedicated test file — repro in the commit message.) **In-main: NOT present** —
  main zercheck_ir.c ~3819 still a plain `if` right after the reallocating `ir_add_handle` at ~3813.

### ~~G. Documented-but-unfixed gaps — `fxvnsu` `9fea9990`~~ — **4 of 5 CLOSED, measured 2026-08-09; G3 UNCERTAIN**

**MEASURED, not assumed** — each probed on main and the REJECTION REASON read:
- **G1 goto-skips-defer — CLOSED.** A `defer` registered after the goto source does not fire at the
  label (`lock == 1`, correct). The sibling goto-defer double-fire (`3o10j6`) was NOT closed —
  that reconstruction was wrong; it is fixed 2026-08-10, see its own entry.
- **G2 helper launders `&local` past a scoped borrow — CLOSED.** *"cannot pass '&d' to a call while
  it is borrowed by a scoped spawn"* — the exact rule, not a mask. Direct-write control also
  rejected; the no-access-between-spawn-and-join control still COMPILES (no over-rejection).
- **G4 `threadlocal &`-escape to a SCOPED spawn — CLOSED.** *"cannot pass '&tl' (threadlocal) to a
  scoped spawn — each thread has its own copy"*. The scoped path A5/BUG-757 left uncovered is covered.
- **G5 heap ptr into a struct-global FIELD — CLOSED.** *"global 'g.p' left dangling at function
  exit"* — the `.field` projection sink is reached; the bare-ident control is also caught.
- **G3 atomic-cell transitivity — CLOSED 2026-08-09.** Reproduced with the branch's VERBATIM file
  after a hand-written reconstruction had wrongly reported "does not reproduce"; see its own entry above.

**The reproducer files this heading claims are "in `tests/zer_gaps/`" DO NOT EXIST** (`gap_goto_skips_defer.zer`,
`gap_scoped_borrow_via_helper.zer`, `gap_threadlocal_amp_escape_scoped_spawn.zer`,
`gap_struct_global_field_dangle.zer` — none present; that directory holds an unrelated 24). Measured with
hand-written equivalents from the descriptions. **If someone still has `fxvnsu`, re-probe with its exact
files before trusting these four closures.** The same claim was false for the `3o10j6` goto-defer entry.
- **G1 — forward `goto` fires a defer it textually skipped** (MEDIUM miscompile). `gap_goto_skips_defer.zer`.
  The acquire / `goto done` / `defer release()` idiom underflows a lock counter (release fires on the error
  path where acquire never ran). Root cause: `ir_lower.c` fires function-scope defers as a static set at
  every exit/goto-target; only defers registered AFTER the goto source are runtime-skipped
  (`defer_count_at_def` handles back-edges; the "goto BEFORE the defer, label AFTER" shape is unhandled).
  Proper fix: per-defer runtime "armed" flags (like the plt86m `defer_fire_guard_flag`); sound interim:
  reject a forward goto that skips a defer registration. NOT the documented back-edge/bh18_12 case.
- **G2 — scoped-borrow exclusivity evaded by a helper laundering `&local`** (HIGH race, TSan-confirmed).
  `gap_scoped_borrow_via_helper.zer`. A stack local borrowed by a scoped `spawn` is exclusive until
  `.join()`; the DIRECT `local.v=7;` between spawn/join IS rejected but `poke(&local)` is not — the borrow
  check is intra-name and does not treat `&local` handed to a helper as an access. Same-block (distinct from
  the known-open cross-block case).
- **G3 — atomic-cell plain-access check not transitive through a helper** (HIGH race, TSan-confirmed).
  `gap_atomic_cell_plain_access_via_helper.zer`. "is atomic cell?" is whole-program, but "flag the plain
  access" fires only for accesses lexically inside a spawning/spawned function; move the plain write into
  `poke(){ g_ctr=5; }` and it is missed (the same plain write directly in `main` IS rejected).
- **G4 — `threadlocal &`-escape to a SCOPED spawn establishes no borrow** (HIGH race, TSan-confirmed).
  `gap_threadlocal_amp_escape_scoped_spawn.zer`. Passing `&tls` to a scoped spawn registers no borrow
  (unlike a stack local) → main's concurrent write between spawn/join is unflagged → the thread writes
  MAIN's TLS slot. A5 (BUG-757) closed threadlocal `&`-escape for fire-and-forget; the scoped-spawn path
  was not covered.
- **G5 — heap pointer stored into a struct-global FIELD dangles unflagged** (CRITICAL UAF).
  `gap_struct_global_field_dangle.zer`. `g.p = n; free(n);` (g a struct global) leaves `g.p` dangling, but
  the "global left dangling at exit" check (GAP-3/BUG-739, zercheck_ir.c ~3231) matches only a BARE global
  ident store (`g = n`, which IS caught) — the `.field` projection sink is missed (per-sink patchwork; cf.
  the P9 by-value-field-launder fix that descended the projection to the root). Cross-thread amplification:
  storing into a `shared struct` field + reading from a worker = silent cross-thread UAF on a reused slab
  slot (observably reproduced, exit=999). CAUTION for the fixer: extends the BUG-742 global-dangle
  conservatism — the maintainer deliberately does NOT flag MAYBE_FREED globals at exit (avoids noising the
  legit register-ctx-then-callback pattern); a field-projection fix must flag only a DEFINITELY-freed
  target, mirroring the bare-ident register/alias/exit machinery for the compound `(IR_GLOBAL_ROOT_ID,
  name.field)` key.

**Test-only (not a bug):** `fxvnsu` also rewrites the flaky `rust_tests/rc_cond_004` (a Rust-Mutex→ZER
translation assuming cross-statement atomicity ZER's per-statement locking doesn't provide → ~12%
lost-update failures breaking the `make check` gate; rewritten to accumulate via atomic compound-assign).
Main still has the flaky version.

---

## OPEN — type-erasure / safe `void*` (generic `*opaque` container over-rejected) (2026-07-16) (over-rejection, NOT a soundness hole)

**Symptom:** a GLib-`GHashTable`-style generic container that stores `*opaque` VALUES and returns
them (`?*opaque map_get(...)`) is rejected by zercheck — the `?*opaque` return trips the
allocation/ghost-handle heuristic (*"result of alloc() … never used"*), and the borrowed value
looks like an owned handle that must be freed. So the GLib `void*` pattern fights the safety layer.

**Root cause:** `*opaque` owned-ness is **type-driven** ("any `*opaque` = an owned allocation"),
whereas every OTHER ZER pointer (`*T`/`[*]T`) derives owned-ness from **provenance** (`alloc_id`).
An erased borrow (`(*opaque)&global`) has no `alloc_id` but is still treated as owned.

**Why the obvious workaround is fine for now:** `usize` value tokens (ZER's `gpointer` analog) work
TODAY (VERIFIED), and `container(K,V)` monomorphization / a tagged union cover compile-time-known
and closed-set cases. Only the SAFE re-type of an open-runtime erased value is missing.

**The fix (Path A, IN PROGRESS) — full design + reasoning + how-we-got-here + the C-cast sugar +
Rust comparison + implementation plan + test matrix live in `docs/universal_pointer.md` PART 6
/ §36.** One-line sketch: make `*opaque` owned-ness `alloc_id`/provenance-driven (not type-driven),
so the erased pointer becomes the fifth citizen of the pointer family (`{ptr, type_id}` = the
`[*]T` of erasure); re-type via the `type_id` trap; sugar is the C-cast `(*T)erased` (already works
+ already checked — verified). RELAXATION (bug = shipped UAF), so oracle-first.

**STATUS (2026-07-16):**
- **Level-1 oracle DONE** — `proofs/operational/lambda_zer_escape/erased_ownership_lattice.v`
  (0 admits, in `make check-proofs`), certifying the origin lattice + AOParam/AOBorrow split + both
  accept-unsafe traps as witnesses.
- **Step-2 Increment 0 SHIPPED (sound, inert)** — the `FuncSummary.ret_is_borrow` signal +
  exhaustive `ir_expr_has_ptrish_call` walker; computed + verified, NOT consumed (zero behavior
  change).
- **Step-2 Increment 1 (the consumer) SHIPPED (2026-07-16) — the content-borrow over-rejection is
  FIXED.** Two FuncSummary signals + a leak-suppress: `ret_is_borrow` (no allocation-capable call
  in the body) AND `ret_is_content` (every return is a value-read of a field/element or null — NOT
  a param-VIEW). Only `ret_is_borrow && ret_is_content` marks the result `escaped=true` (suppress
  the false leak) while keeping it REGISTERED (double-free/UAF intact). This IS the oracle's
  AOBorrow (suppress) vs AOParam (keep). `map_get`'s `*opaque` container now compiles
  (`erased_map_get_ok.zer`); `rt_drop_conflict_uaf` + `erased_wrapper_double_free` still reject.
  Full `make check` GREEN. **A naive `ret_is_borrow`-only gate was accept-unsafe** (broke the
  interior-pointer UAF); `ret_is_content` is load-bearing — see §36.17 + the zercheck_ir.c
  consumer comment. **Residual (pre-existing, NOT a regression):** a param-VIEW return
  (`return &s.field`) still over-rejects with a leak false-positive — deliberately kept because it
  incidentally catches the through-call interior-pointer UAF (which zercheck does not otherwise
  catch). Fixing that properly (infer `returns_param_color` for `&param.field` so the caller
  aliases the result to the arg → a real UAF, then leak-suppress the view too) is a separate future
  refinement.

## OPEN — native `uN`/`iN` follow-ups (2026-07-09) (none a soundness hole; polish only)

Native arbitrary-width integers `uN`/`iN` shipped (BUGS-FIXED.md 2026-07-09,
commits e7ea2bcb/d91d0742/80183261; `make check` GREEN). The type-kind
predicates are VST-verified for `TYPE_UINT`/`TYPE_SINT` (Level-3 restored,
`verif_type_kind.v`, verified via `make check-vst` 2026-07-10). Width-masking is
complete: `emit_intn_mask` runs on `IR_BINOP` (arithmetic + shift) and `IR_UNOP`
(negation/complement); `emit_intn_mask_lv` masks assignment + compound-assign stores
(`=`/`+=`/`-=`/`*=`/`&=`/`|=`/`^=`/`<<=`, single-eval on side-effecting targets); `@truncate`
masks inline+stored; bit-slice READ uses a 64-bit guarded mask (all 2026-07-13, merged from
k7l625/jfrmer branches); global-scope arithmetic needs no mask (verified safe by rejection
2026-07-12 — see BUGS-FIXED.md). Remaining edges (all polish/deferred):

- **VRP mask-elision (LOW — performance).** `emit_intn_mask` always emits the
  `& (2^N-1)` after `uN` arithmetic. Sound but not minimal: where VRP can prove
  the result already fits N bits (bounded counters, constants), the mask should
  be elided → single machine op. Also the invariant-preserving ops (`& | ^ >>`)
  never need the mask and could skip it unconditionally. Not built. (Explicitly
  deferred 2026-07-10 — a Rice-bounded precision optimization on the least-
  important axis, not a correctness matter; see BUGS-FIXED.md.)
- **Single-bit `reg[5]` shorthand — an ARCHITECT DECISION, not a mechanical fix.**
  A bare single index on a scalar integer (`reg[5]`) errors "cannot index type";
  use the range form `reg[5..5]` (works today, read + write). Making `reg[5]` a
  bit-access is a 2-line rewrite (`NODE_INDEX` → `NODE_SLICE[N..N]` in the
  checker), and it works — BUT it conflicts with an INTENTIONAL safety design:
  ZER deliberately rejects indexing a scalar integer (`u32_var[i]`) to catch
  "I thought this was an array" bugs (C unit test "index u32 rejected"). Bit
  access uses the explicit range syntax on purpose. Flipping `scalar[5]` from an
  error to a silent bit-access removes that guard — a safety tradeoff to decide
  deliberately, not slip in. (Attempted + reverted 2026-07-09: the rewrite broke
  that test; `reg[5..5]` remains the way unless the design is changed on purpose.)
- **`>64`-bit `uN` uses emulated multi-word arithmetic** (carrier is `__int128`
  ≤128; the `emit_intn_mask` __int128 branch). For hand-tuned big-int use the
  `@addc`/`@subb`/`@mulw` carry primitives + a limb struct (library).

---

## OPEN — findings surfaced during the universal-`alloc` build (2026-07-08) (mostly LOW/MEDIUM, none an active soundness hole)

Bugs found while building `alloc`/`free` (docs/universal_alloc.md) but out of that
scope, so NOT fixed. Full write-ups (repro + root cause) in
**docs/universal_alloc.md §11**. Triage:

- **MEDIUM — bare `orelse return;` inside a `?T`-returning function yields a wrong
  `None`.** `?u32 f(){ *E e = slot orelse return; return e.value; }` with `slot`
  null: the caller sees `f()` as HAVING a value. Only the BARE form in a `?T`
  function; the block form `orelse { …; return null; }` and explicit `return
  null;` are fine. A correctness bug (wrong runtime behavior), narrow.
- **MEDIUM — `subst_typenode`'s `TYNODE_HANDLE` case does not recurse into
  `handle.elem`.** Any `container` field shaped `Handle(T)`/`?Handle(T)` fails
  with "undefined type 'T'" (breaks self-referential `container Chained(T){
  ?Handle(Chained(T)) next; }` and more). Separate from the depth-32 recursion
  guard. Would need `subst_typenode` to recurse HANDLE like POINTER/OPTIONAL do.
- **LOW — global `Arena` in-place `garena.over(buf)` does not initialize** → a
  later `garena.alloc_slice(...)` returns `None` at runtime. Only `Arena x =
  Arena.over(buf)` (capture the return) works.
- **LOW — `global = arena.alloc_slice(...)` (direct form) compiles** when the
  temp-var form is rejected: the escape rejection (checker.c ~4694) only fires on
  a bare `NODE_IDENT` value; the direct call/orelse form only taints
  (checker.c ~4645). A false-negative escape gap (masked at runtime by the global-
  arena init bug above).
- **LOW — `[*]?*T` slice element emits broken C** ("incompatible types … struct
  anonymous" from GCC). Workaround: a named wrapper struct (`struct Bucket { ?*T
  head; }`). An anonymous-struct-in-slice-typedef emitter gap.
- **LOW — a named `const` is not accepted as an array size** (`?*E[N]` with a
  const N → "array size must be a compile-time constant"); only a literal or a
  `comptime` function call is. Cosmetic wart.

**Also OPEN — the pointer-return relaxation covers var-decl only.** `*T p = &r[i];
return p;` (where r is a param/heap slice) is accepted; the separate ASSIGNMENT
form `p = &r[i]` (p declared earlier, assigned later) is a different escape sink
(checker.c ~4138 area) and may still over-reject. Extend by mirroring the same
`root_is_ref && !is_local_derived` guard there. See BUGS-FIXED.md 2026-07-08.

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

## OPEN — `tools/audit_matrix.sh` is STALE (false positives mask real flag-handler gaps) (LOW — tool only, contracts sound)

**Symptom:** `bash tools/audit_matrix.sh checker.c` reports 16 "BUG: … missing …
check" gaps (RETURN/BREAK/CONTINUE/GOTO/YIELD/AWAIT/SPAWN × defer_depth/
critical_depth/in_loop/in_interrupt). **All 16 are FALSE POSITIVES** — the
contracts they claim are missing are actually enforced.

**Root cause:** the script hardcodes a line window (`$1 > 8500 && $1 < 11000`)
and extracts the handler body as "first `case NODE_X:` in that window → next 200
lines." checker.c has grown to 16k+ lines, and the SAME control-flow case labels
now appear in FIVE different switches (scan_frame, collect_labels, validate_gotos,
the real `check_stmt`, plus the emit-side). The script grabs a DECOY case (e.g.
`case NODE_RETURN:` at ~9062 in a non-checking switch) instead of the real
handler. The actual context-ban checks live at checker.c ~6730
(`zer_return_allowed_in_context(defer_depth, critical_depth)` and the break/
continue siblings) and ~11237 (the check_stmt switch) — both outside the tool's
window. Verified by hand: every one of the 16 contracts holds.

**Why it matters (and why LOW):** it is a MANUAL audit, NOT a `make check` gate,
so it gates nothing and cannot fail CI. BUT in its current state it cannot
surface a *real* flag-handler gap — the 16-line noise floor would bury it (same
failure mode as "CRLF masks the audits"). So the flag-handler dimension currently
has no working automated guard, unlike the switch-exhaustiveness dimension (now a
hard `-Werror=switch` gate, 2026-06-27).

**Fix sketch:** stop using a line range. Anchor on the real `check_stmt` function
(find its `switch (node->kind)` by walking from the `check_stmt` definition), and
within THAT switch only, extract each control-flow case to its `break`. Or
better, drop the grep heuristic entirely and assert the contracts a different way
(e.g. a small unit test that feeds each `return/break/.../spawn`-in-`defer`/
`@critical` program through the checker and asserts rejection — those negative
`.zer` tests already exist in `tests/zer_fail/`, so the tool is arguably
redundant and could be retired in favor of them).

**Tripwire:** none yet (the negative `.zer` tests in `tests/zer_fail/` —
`*_in_critical.zer`, defer-ban tests — are the real guarantee; this tool was
meant to be a static cross-check of them).

---

## OPEN — MAX-ORACLE GAP AUDIT (2026-06-23) — the master map: which safety classes are not-sound / not-flexible / coarse-or-no-oracle

> **THE PLAN THAT CONSUMES THIS AUDIT: `docs/unified-oracle-proved-ZER.md` (2026-08-10).**
> This entry is the per-class GAP MAP; that document is what to DO about it — the unified
> Level A product (provenance x liveness x ownership x bounds x capability, componentwise
> join, conjunction at every use site), with Level B relaxations underneath. Priority order
> there is driven by a measured finding: **every defect found 2026-08-08..10 was in a class
> with NO oracle, zero exceptions**, while handle/concurrency/escape/move/opaque produced
> none. Phase 0 is wiring the orphaned `vrp_ir.c` (349 lines, 0 Makefile refs, 0 symbols in
> the built binary — the sound CFG bounds analysis is not compiled).


Audit of EVERY safety class against the MAX-ORACLE STANDARD (CLAUDE.md): a class is
"at maximum" iff it is (a) SOUND (zero under-rejection — never accepts unsafe), (b)
FLEXIBLE (minimal over-rejection), AND (c) backed by a MAX oracle (a Coq/Iris Level-1
spec whose finite-state set is COMPLETE and whose abstraction is the richest sound one,
not a flat/coarse one). This is the INDEX; per-hole detail lives in the linked entries
below. Verdict tally: ~14 live under-rejections (6 are CRITICAL memory-corruption), ~4 real
over-rejections, and MOST classes are coarse-oracle or no-oracle — only 3 are genuinely
AT-MAX. Two clusters (type/provenance fully audited 2026-06-23; the other five audited
from this ledger after the parallel workflow rate-limited).

### NOT-SOUND — under-rejects (accepts unsafe). The urgent tier (close before precision work).

**Memory-corruption (CRITICAL UAF/OOB):**
- **`@bitcast` int↔ptr forge** (#3) — **[FIXED 2026-06-23 — wired the verified
  `zer_bitcast_operand_valid`; see the FIXED entry below]**. Was: `@bitcast(*T, intval)`
  reinterprets an integer as a pointer with a clean compile: on 64-bit a ptr and u64 are
  both 8 bytes so `zer_bitcast_width_valid` passes, and the handler (checker.c:7230-7270)
  calls ONLY the width + const/volatile-strip checks — never an int-vs-ptr operand check.
  A grammar-level breach of the "no in-language unsafe" closure (synthesizes the banned
  `ptr+N`). The fix predicate ALREADY EXISTS and is VST-verified —
  `zer_bitcast_operand_valid(is_primitive)` returns 0 for a pointer operand
  (src/safety/cast_rules.c:31, proofs/vst/verif_cast_rules.v) — but is NEVER CALLED from
  checker.c. Control: `(*T)int`→"use @inttoptr" (checker.c:6840), `(u32)ptr`→"use
  @ptrtoint" (6848) — every other path gates int↔ptr; only `@bitcast` bypasses all three.
  **Fix = one call site** (wire the proven predicate; reject when exactly one of {src,dst}
  is a pointer) + tripwire. The cleanest fix in this whole audit.
- **`@pun` `type_id==0` short-circuit** (#4) — **[FIXED 2026-06-23 — compile-time
  widening reject; see the FIXED entry below]**. Was: the emitted guard is
  `if (type_id != TGT && type_id != 0) trap` (emitter.c:2723/2870/2951/2972/6860/6921;
  comment at 2908 admits "type_id == 0 sentinel matches anything"). An in-ZER pointer to
  a PRIMITIVE (`*u32`/`*u8`/slice `.ptr`/`@inttoptr` result) packs `type_id==0`, so
  `(0 != TGT && 0 != 0)` = false → trap SKIPPED even for a statically-known size mismatch
  (`*u32 sp=&small; *Big bp=@pun(*Big,sp); bp.b` reads 8 past a 4-byte object). The opaque
  oracle (lambda_zer_opaque J04) models TRACKED provenance and never sanctions extending
  the unknown-tag(0) escape to a fully-typed in-program pointer. **Fix:** don't grant the
  `!=0` escape to a fully-typed in-ZER primitive/slice pointer (the 0-escape is only for
  genuinely-unknown cinclude `*opaque`), or add the compile-time size-widening check.
- **move-struct alias** (#1, line ~1551) — an alias taken BEFORE the move-transfer isn't
  registered in the source's state group, so TRANSFERRED doesn't propagate → free/move
  tracking defeated.
- **VRP scope-leak OOB** (#2, line ~1636) — a branch-local range narrowing leaks past a
  control-flow join (flat AST `var_range_count` not saved/restored on the non-comparison
  branch) → the compiler proves `buf[idx]` safe and emits NO bounds check on a path where
  `idx` is OOB. ROOT: the sound CFG-VRP `vrp_ir.c` is orphaned (absent from the Makefile,
  not even compiled); production runs the unsound flat pass. Oracle now exists
  (lambda_zer_bounds/bounds_lattice.v `elide_on_join_sound`); fix = wire `vrp_ir.c`.
- **fixed-array bare-call index** (#5, line ~1786) — `arr[f()]` on a fixed array drops the
  bounds check on the bare-call single-eval emission path.
- **`|*v|` capture escape** (#6, line ~1829) — the `if(opt)|*v|` capture binds `v=&m.value`
  (into a local) but the desugaring doesn't inherit `is_local_derived`, so `g=v` is treated
  as a normal global store → dangling global. The direct `g=&m.value` IS rejected; only the
  capture-synthesized address slips through. Oracle now exists
  (lambda_zer_capture/capture_lattice.v — `capture_preserves_escape` + `buggy_reset_unsound`
  witness the bug); fix = capture inherits the matched value's region.
- **defer-body UAF** (line ~1459) — a defer body uses a handle the function body then frees.
- **P9 by-value struct field-launder** (#P9) — **[FIXED 2026-06-24 — see BUGS-FIXED.md]**.
  Was: `void stash(Holder h){ g = h.p; }` + `stash({.p=&local})` COMPILED — a pointer field
  of a by-value struct PARAM stored to a global laundered a local-derived pointer (the direct
  `g = h.p` with a local `h` was already caught; only the through-a-param launder slipped).
  Found by the empirical probe sweep. Fixed: the keep-2a sink (checker.c ~4209) now descends
  the `param.field` projection to the root param and infers keep (theorem param_lattice.v T5
  projection_preserves_escape / buggy_projection_unsound). This was a form→state coverage
  gap, NOT a missing finite state — the per-sink-patchwork class the codebase warns about.

**Data-race (HIGH concurrency):** shared-struct multi-access hidden in a cast/intrinsic/
index/orelse SUBEXPRESSION evades the same-statement deadlock/lock check (#7, ~1881); the
`spawn` data-race scan is blind to function-pointer indirection (#8, ~1925); shared access
in an `await` CONDITION is not locked (D02 false-negative, #9, ~1963). The **cross-block
scoped-borrow** hole (spawn + access in different CFG blocks) is **FIXED 2026-08-03** via
`Checker.branch_depth` / `Symbol.th_spawn_branch_depth` — not the zercheck_ir borrow-set
merge previously sketched here, which named the wrong subsystem.

**Miscompile (MEDIUM — unsound OUTPUT, not a UAF):** value-returning `async` never finalizes
its state machine (#10, ~2001); bit-query/byte-swap intrinsics emit `0` in global
initializers (#11, ~2046); `defer` + backward `goto` fires the wrong defer count (#12,
~2080 / the "defer fires twice" entry ~799); compound `/=`/`%=` lack the signed-overflow
trap on the AST emit path (~938).

### NOT-FLEXIBLE — over-rejects (rejects correct code). Coarse abstractions.

- **Escape disjunctive return** — **NARROWER THAN PREVIOUSLY STATED (empirically corrected
  2026-06-24).** The common disjunctive shapes COMPILE: `pick(c){if c return &g1; return &g2}`
  (both static) and `either(p,c){if c return p; return &g}` called with a global arg both
  compile (Stages 1-2 handle them). The flat `ret_param_mask`'s only real loss vs the JOIN is
  using the surviving `ARParam` fact for DOWNSTREAM precision, not the escape verdict itself
  — for the escape decision, "summary incomplete → UNKNOWN → reject" and "summary contains
  ARLocal → reject" coincide, so a function with a genuinely-unclassifiable return path is
  rejected either way (and is usually unsafe to escape anyway). The RICH oracle
  (lambda_zer_escape/join_lattice.v) is still worth implementing for downstream precision +
  the relational tail, but the "EVERY call maximally conservative" framing was an over-claim.
- **Aliased mutation** — **DOES NOT REPRODUCE (empirically corrected 2026-06-24).** ZER has
  NO aliasing-XOR-mutability rule, so two live interior pointers into one array mutated
  through both (`*u32 p=&a[0]; *u32 q=&a[5]; *p=1; *q=2;`) COMPILES — ZER already accepts
  what Rust rejects here; there is nothing to "unblock". The disjoint oracle
  (lambda_zer_disjoint/disjoint_lattice.v) is NOT needed to accept aliased mutation. The
  genuine residual is narrower: the `alloc_id` **fate-sharing** false-positive (freeing one
  slice-half false-flags the other; mixing literal/variable index frees, BUG-741) — that is
  the only real over-rejection in this area, and it needs the relational layer.
- **Nested inline designated initializer** (#13) — **[FIXED 2026-06-24 — see BUGS-FIXED.md]**.
  Was: `Outer o = { .inner = { .x = 1 }, .y = 2 };` rejected with "field '.inner' … got
  'void'" (the inner `{ .x = 1 }` has no standalone type). Fixed: `validate_struct_init`
  (checker.c ~1441) now recurses on a `NODE_STRUCT_INIT` field value, validating it against
  the field type (which it inherits as context). 2/3-level nests compile+run; inner
  field-name/type errors still rejected.
- **MAYBE_FREED path-correlation — FIXED (2026-06-27): Level B guarded refinement SHIPPED.**
  `if(c){free(h)} if(!c){use(h)}` — and the matching double-free under `!c` + the leak check
  when freed under both `c` and `!c` — now COMPILE: freeing under one guard and using/freeing
  under the DISJOINT complement is recovered, gated on PROVABLE guard disjointness (else the
  Level-A MAYBE_FREED conservatism stands). Implementation (zercheck_ir.c): per-block
  immutable-bool guard sets (`ir_compute_block_guards`) + per-handle `free_block` /
  `freed_all_paths`; the use/double-free/leak sites relax via `ir_use_guard_disjoint` /
  `ir_free_completes_coverage`. SOUNDNESS GATE: `ir_local_is_immutable_bool` via a no-default
  exhaustive AST walk (`ast_name_mutated_or_addrd`) rejecting any reassigned/address-taken
  condition — two accept-unsafe holes (reassigned param; `&c` in a call arg) were found+closed
  during the build. Certified by handle_flow_lattice.v Level B. Tests:
  `tests/zer/guarded_maybe_freed_disjoint.zer` + 6 `tests/zer_fail/guarded_*`. Full detail:
  BUGS-FIXED.md 2026-06-27.
  - **DEFERRED — full state-TRUTHFULNESS (2026-06-28).** The shipped design is
    DECISION-layer (the `MAYBE_FREED` state stays coarse; the use/double-free/leak
    sites consult guard side-channels to get the right OUTCOME). The truer
    STATE-layer version (refine `MAYBE_FREED → ALIVE` in the lattice so every
    consumer sees the truth) was attempted and reverted — it regressed the leak on
    a nested `if` (the refinement bypasses the `freed_all_paths` set-path; the
    leak's "freed on ALL paths" is a coverage fact that can't be per-block state).
    Outcomes are already correct; state-truthfulness needs a per-handle
    free-guard-SET + a more precise (dominator/per-edge) guard computation. See
    `docs/compiler-internals.md` "Sound relaxation" (the reverted-attempt note).
- Every flat-lattice class carries residual over-rejection by construction (see below).

### ORACLE COVERAGE — the theorem layer, by class (the (c) criterion)

- **AT-MAX (sound + MAX-oracle-backed):** **CORRECTED (2026-06-24, maximality workflow
  overturned the original trio).** The verified at-max set is `move` (lambda_zer_move),
  `qualifier` and `volatile` (the property admits no richer sound abstraction). `@ptrcast`/
  `@container` provenance (lambda_zer_opaque) is SUFFICIENCY-only for the modeled fragment
  (PtrNull / type_id=0 / structured `@container` provenance unmodeled). MMIO is COARSE (see
  below). **Handle is NOT at-max:** its operational track leaves soundness obligations
  unproved (excluded from the gate) and the gated Iris track is flat 2-state with placeholder
  lemmas — BUT the abstract DOMAIN is now certified by
  `lambda_zer_handle/handle_flow_lattice.v` (the 4-state flow lattice + JOIN merge, Level A;
  the guarded MAYBE_FREED refinement, Level B). So handle's domain went uncertified → certified
  (2026-06-24); the proof-track admits/placeholders remain a separate cleanup.
- **COARSE oracle (exists but FLAT — precision left on the table):** escape (shipped
  `param_lattice.v` is flat; the rich `join_lattice.v` is spec-only), bounds (interval, not
  relational `i<j`), qualifier / capture / volatile (the 4 added 2026-06-23 are flat by
  design), optional/null (typing.v Section N is flat — certifies only `?T`-vs-`*T`
  type-discipline, NOT the flow-sensitive null-state lattice; rows N04/N06/N07/N08 unproven,
  typing.v:1324).
- **NO oracle (finite-state set UNCERTIFIED — discovered by red team):** distinct-typedef
  unwrap invariant (the #1 historical bug class BUG-409/GAP-F, guarded only by the
  line-frozen `tools/audit_type_dispatch.sh` linter, not an oracle), division-by-zero,
  integer-overflow-wrap, shift safety, ISR safety, naked/asm (Z9/Z10/Z13 never shipped),
  stack-overflow/recursion, `@critical` control-flow, and the intrinsic-miscompile classes
  (#10/#11/#12). The "next frontier" oracle backlog (concurrency/ISR/atomics/async/MMIO
  decision oracles) is line ~393.

### NAMED FLOORS — NOT gaps (out of scope, do not chase)

cinclude/FFI `type_id=0` (extern `*opaque`, C-domain), liveness/deadlock-livelock (out of
scope for ZER *and* Rust), hardware-consequence (datasheet/silicon correctness — physics
floor).

### PRIORITY (fixed by the "never allow unsafe" hard constraint — sound before precision)

1. **The two proof-backed near-free CRITICAL wins:** `@bitcast` #3 (wire the already-VST-verified
   `zer_bitcast_operand_valid`, one call site + tripwire) and `@pun` #4 (the size guard).
2. The stateful CRITICAL holes: #1 (decl-site alias), #2 (wire `vrp_ir.c`), #5 (emitter path),
   #6 (capture inherits region — oracle ready).
3. The HIGH races (#7/#8/#9) + the cross-block scoped-borrow.
4. The MEDIUM miscompiles.
5. THEN precision: implement `join_lattice.v` (disjunctive return) and `disjoint_lattice.v`
   (aliased mutation), and write the missing/richer oracles (relational bounds, the null
   flow-state lattice, distinct-unwrap, the no-oracle classes).

---

## OPEN — WASM CLI: multi-file imports + macOS terminal zerc (LOW–MEDIUM)

> **NOTE 2026-08-10 — the original Defender motivation no longer reproduces for the
> maintainer.** The WASM bridge exists because unsigned mingw PEs tripped Defender
> `Wacatac.B!ml` (CLAUDE.md "VS Code Extension (VSIX) Build"); the shipping VSIX is now
> reported to install and run with no scan. That is a single-machine observation, not a
> general guarantee — do NOT rip out the WASM bridge on the strength of it. **The two
> gaps below are FUNCTIONAL and independent of the scanning question:** multi-file
> `import` genuinely does not resolve through the wasm CLI/LSP regardless of why the
> wasm path exists. This entry stays canon.


The VS Code extension ships the compiler as WebAssembly (`zer_wasm.c` →
`lsp/zer.wasm`, driven by `lsp/server.js` and `lsp/zerc-cli.js`). As of
2026-06-16 the flag plumbing, `--emit-ir`, and full LSP safety parity are DONE
(`zer_set_target` carries `--target-bits/-arch/-features`, `--no-strict-mmio`,
`--stack-limit`; `zer_emit_ir`; `zer_diagnostics_json` runs `zercheck_ir` and
`server.js` merges its stderr into editor diagnostics). Two gaps remain:

- **Single-file only:** `zer_emit_c`/`zer_diagnostics_json` set
  `import_asts = NULL` and process one `file_node`. Multi-module programs
  (`import`) won't resolve cross-module symbols through the wasm CLI/LSP. Native
  zerc handles modules (topological emit across files). Fix sketch: thread a
  node-side import resolver (read imported `.zer` by path, parse, pass the AST
  array in) — emscripten `NODERAWFS` would let the checker's `fopen`-based
  import loader work directly, or pass `import_asts`/`import_ast_count` built in
  JS. Largest remaining piece; needs replicating zerc_main's module loop.
- **macOS terminal `zerc` dropped:** the wasm VSIX bundles a signed Windows
  `node.exe` for `zerc.cmd` and keeps a native `linux-x64` `zerc`, but no darwin
  CLI build remains. macOS LSP still works (wasm via Electron-node); only the
  macOS *terminal* `zerc` is absent. Fix sketch: bundle a signed darwin node +
  a `zerc` shell shim.
- **Cosmetic:** the CLI prints some checker errors twice (checker records
  certain diagnostics twice); pre-existing, not wasm-specific.

Cross-arch caveat: `--target-arch aarch64|riscv64` configures the *checker*
correctly, but the wasm CLI compiles with the bundled **x86_64** gcc — actual
cross-compilation needs a cross-gcc the bundle doesn't ship.

---

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

## OPEN — conditional global dangle (MAYBE_FREED at exit) unflagged (LOW, BUG-742 residual)

BUG-742 (2026-06-10) closed cross-function global UAF at the source: a
global definitely FREED at function exit or at a ZER/indirect call site is
a compile error (teaches `g = null;` after the free). DELIBERATELY out of
scope: globals in MAYBE_FREED state at those points — `if (c) {
heap.free_ptr(p); }` then exit leaves the global conditionally dangling,
unflagged. Why: BUG-740/741 widenings produce MAYBE_FREED + escaped on
legitimate hand-off patterns (`g_ptr = p; fp(p);` register-ctx-then-
callback), and flagging MAYBE at exit would reject exactly those. Fix
sketch if ever needed: distinguish widened-by-barrier (escaped at widening
time) from conditionally-freed-by-user (definite IRMC_FREE on one branch)
with a per-entry origin bit, and flag only the latter. Reproducer shape:
conditional free of a global-held pointer without reset on that branch.



---

## PLAN — asm Option E rework (Level C cleanup first)

The asm-safety architecture is slated to move to Option E (three-layer, no-favored-
ISA — `docs/asm_lang_zer_safe.md` §1.7). **Phase 1 = Level C cleanup**: delete the
per-arch infrastructure (~7,000 lines: register/instruction tables, categories
framework, probe scripts, `arch_data/*.zerdata`, stray `.v`), replace the
checker.c F7-full table dispatch with a hardcoded ~12-entry UB-classics list, and
delegate register/instruction/feature validation to GCC. **Intrinsics STAY in
Phase 1** — they get re-layered (operation→Layer 1, x86 asm→Layer 2 lib) only in
Phase 3. Verified file-by-file execution checklist (commit order, regression net,
the `.v`/`check-vst` coupling asm_lang §10 underspecifies):
**`docs/option_e_plan.md`** — fresh-session-executable. `tests/test_asm_matrix.c`
is the regression net for the deletion.

## OPEN — asm S2 instruction-count `\n`-escape bypass (audit rule, not safety)

The S2 rule (checker.c:10379) caps an asm block at 16 instructions for
auditability, counting actual-newline (0x0A) and `;` chars in the instruction
string. ZER's lexer keeps `\n` ESCAPE sequences literal (does not convert to
0x0A), but the emitted C asm string IS expanded by GCC — so
`instructions: "nop\nnop...×17"` passes S2 (counted as 1) yet assembles as 17
real instructions. The S2 "≤16 auditable" guarantee is therefore bypassable via
`\n` escapes.

**Severity: low — S2 is an AUDIT/maintainability rule, not a memory-safety /
program-consequence rule** (its own message: "forces small auditable blocks").
No safety/soundness false negative; the Z-rules, naked-only, qualifier/escape
checks are unaffected. Found by `tests/test_asm_matrix.c` (2026-06-08).

Fix sketch (when convenient): make the S2 counter also count `\n` (and `\t`)
escape pairs in the instruction string, OR normalize asm-string escapes at lex
time so the count matches what GCC assembles. The oracle's `too-many-instructions`
cell uses `;` separators to test S2 as designed in the meantime.

---

## STATUS — soundness oracle suite (read first, 2026-06-07)

Four exhaustive `-Wswitch`-enforced oracles guard the compiler's safety analysis.
A "hole" = a NEG cell that compiled clean (false negative = unsafe program
accepted, the unacceptable class) or a POS cell that was rejected (over-rejection,
acceptable but logged). Each is built + run by `make check`.

| Oracle | File | Cells | Domain | Status |
|---|---|---|---|---|
| Shape | `tests/test_shape_matrix.c` | 25 | temporal (UAF/double-free/leak/move) × type × reach-shape | green |
| Escape | `tests/test_escape_matrix.c` | 35 | local-pointer escape × launder × sink | green |
| Keep | `tests/test_keep_matrix.c` | 21 | non-keep-param persistence × launder × sink (+ keep valve) | green |
| Control-flow | `tests/test_cflow_matrix.c` | 38 | if/loop/switch/break/continue/defer merges × {pool,slab} | green |
| Concurrency | `tests/test_conc_matrix.c` | 15 | data-race / spawn / deadlock / ThreadHandle join | green |
| ISR/atomics/MMIO | `tests/test_hw_matrix.c` | 12 | MMIO range/align/decl, volatile-strip, ISR context + data-race (program-consequence only) | green |
| Async | `tests/test_async_matrix.c` | 10 | yield/await in defer/@critical, spawn-in-async, valid yield/await/defer/state-promotion | green |
| Asm | `tests/test_asm_matrix.c` | 11 | DURABLE asm surface: S1 naked-only, S2/S3/S4, empty-insn, Z8 const-output, Z11 non-keep-ptr+mem-clobber (NOT F4-F7 register tables) | green |

**Pointer-lifetime axis ("universal pointer") is DONE** (2026-06-07): the
compile-time `keep` model (PART 5 of `docs/universal_pointer.md`) — all 5 steps
complete, boundary defaults audited. This session closed 24 real holes (16 escape
+ 6 keep + 1 defer-double + 1 struct-copy). Do NOT re-investigate the pointer
axis for false negatives without a new launder/shape idea — the four oracles are
the standing guard; add a cell if you have a new idea.

**Next frontier = the non-memory domains** (concurrency / ISR / atomics / async /
MMIO). See the dedicated OPEN entry below.

---

## OPEN — next frontier: concurrency / ISR / atomics / async / MMIO oracles

Domains 1 (concurrency) and 2 (ISR/atomics/MMIO) now HAVE oracles (both
green, 0 holes — see the STATUS table). **Domain 3 (async) is the remaining
gap.** The non-memory domains needed their own harness shapes (concurrency is
timing/structural; ISR/privileged ops use the EMIT-ONLY + dead-branch pattern).
Recommended approach for the remainder: **survey first** — write a handful of
adversarial NEG programs, see which leak (compile clean when they should reject),
then build/extend the oracle.

Many rules here ARE accept/reject (oracle-able like the memory matrices); a few
(shared-struct auto-lock *correctness*) are emission-correctness, not accept/
reject, and need an emit-inspection check instead.

**Domain 1 — data-race / spawn / deadlock — DONE (2026-06-07).**
`tests/test_conc_matrix.c`, 15/15, **0 holes found** (regression lock-in — the
spawn/deadlock/join checks are structural bans, built with extensive
Rust-equivalent tests, and held up including the edge cases that found holes in
other domains: transitive non-shared global at call depth, Slab-access from
spawn, and ThreadHandle joined in only one branch). Covered NEG: spawn
non-shared ptr, spawn non-shared global (direct + transitive), deadlock
same-statement, spawn-in-@critical, ThreadHandle not joined (direct +
one-branch). POS: shared auto-lock, scoped spawn+join (incl. join-both-branches),
value args, separate-statement shared access, threadlocal. Residual (add when
convenient): `shared(rw)` concurrent-reader same-statement, Ring/Pool-from-spawn
(only Slab tested), condvar/@barrier/@once interactions, deeper deadlock cycles
(A→B→C ordering).

**Domain 2 — ISR / atomics / MMIO — DONE (2026-06-07).**
`tests/test_hw_matrix.c`, 12/12, **0 holes** (regression lock-in — MMIO/volatile/
ISR checks are mature). PROGRAM-CONSEQUENCE only, per
docs/firmware_safety_extensions.md: tests wrong USES with a structural shadow,
NOT the hardware floor. NEG: @inttoptr no-decl / out-of-range / misaligned,
volatile-strip, slab-in-ISR, spawn-in-ISR, ISR non-volatile shared global, ISR
volatile compound-RMW. POS: @inttoptr in-range+aligned (incl. writing 9601 — the
floor value COMPILES, demonstrating the split), pool-in-ISR, atomic global, ISR
volatile plain assign. EMIT-ONLY harness (interrupt attrs may not compile on
hosted gcc). DELIBERATELY EXCLUDED (floor / Definition B / pending gaps — a NEG
cell for these would be a WRONG expectation): 9601-vs-9600 baud value,
read-clears / W1C / sticky side effects (§16), region-kind hardware correctness,
`@section`/region-kinds/`@reset_handler`/linker-symbol features (not built).
Residual (add when convenient): @atomic non-1/2/4/8 width reject, MMIO
variable-index runtime-trap (belongs in tests/zer_trap, not emit-only).

**Domain 3 — async (yield/await) — DONE (2026-06-07).**
`tests/test_async_matrix.c`, 10/10, **0 holes** (regression lock-in — async bans
are structural). NEG: yield/await in defer, yield/await in @critical,
spawn-in-async. POS: yield, await, defer-without-suspend, local across yield
(state promotion), await-on-shared. **Key finding (corrected a wrong
expectation):** `await g.ready == 1` (await condition reads a shared struct) is
SAFE and correctly COMPILES — each poll locks/reads/unlocks and the lock is
released BETWEEN polls, never held across the suspension. `yield`/`await` are
STATEMENT-ONLY (can't embed in an expression — `g.v + yield` is "undefined
identifier 'yield'"), so a shared lock (held only for its own statement) can
never bracket a separate suspend statement. The "shared access in a statement
containing yield" rule (checker.c:5450) is therefore defensive/forward-compat and
effectively unreachable today — no false negative, the unsafe construction isn't
expressible. (The async analog of the 9601-floor lesson: don't assume a construct
is unsafe; verify. await-on-shared is the floor-equivalent that must COMPILE.)
Residual: if yield/await ever become expressions, revisit the 5450 reachability.

**ALL THREE FRONTIER DOMAINS DONE (2026-06-07).** The seven-oracle suite (shape,
escape, keep, cflow, conc, hw, async) covers the memory-safety axes AND the three
non-memory domains. Net for the frontier: 0 holes found (all regression lock-in)
— the concurrency/ISR/async checks are mature structural bans, in contrast to the
pointer-axis data-flow analyses (24 holes this session). What remains is lower-
value coverage debt (the per-domain "Residual" notes above + the shape-matrix
roadmap items below), not known false negatives.

**Breadth survey (2026-06-07):** direct-case guards CONFIRMED firing —
`spawn worker(&local)` (non-shared ptr → "data race"), `@inttoptr(const)` with no
`mmio` range (→ "requires mmio range"), `yield` in `defer` (→ "corrupts coroutine
state") all reject correctly. So the frontier is NOT wide-open; the oracle work is
finding LAUNDERED / merge / cross-context edge cases (as the memory oracles found
24), not plugging direct holes. Start each domain oracle from its accept/reject
rules above and add launder/merge variants.

**Harness notes:** privileged/hardware ops (ISR bodies, `@cpu_*`, MMIO) can't
`--run` in a hosted container — use EMIT-ONLY (`zerc f.zer -o /tmp/x.c`, exit 0 =
zercheck accepted) for POS, and the dead-branch pattern (`volatile u32 nt = 0;
if (nt == 42) { ...privileged... }`) to compile without executing. Integrity
guard: a NEG rejection must name the relevant safety reason (data race / deadlock
/ ISR / not joined / atomic width), not a parse error. Model the oracle on
`tests/test_cflow_matrix.c` (closest structure: NEG+POS, find_zerc, -Wswitch).

When a domain oracle lands: add it to the STATUS table above, wire into
`make check` (Makefile `check:` deps + run line), document in
`docs/compiler-internals.md`, and trim this entry's covered domain.

---

## OPEN — shape-matrix oracle: remaining coverage roadmap

`tests/test_shape_matrix.c` is the exhaustive memory-safety oracle (option A,
the replacement for the retired dual-run). It currently covers 25 cells:
3 types {pool/Handle, slab/`*T`, move-struct} × 7 reach-shapes {bare, field,
array, fnarg, field-xfn (GAP-A), spawn (GAP-C), deref (BUG-463)} × 4 violations
{uaf, double-free, leak, use-after-move}. Found+fixed BUG-702 (compound leak)
and BUG-703 (move-field over-rejection). These remaining axes are NOT coverage
yet. Ranked by likelihood of finding NEW under-rejections (vs regression
lock-in). NONE are bugs — they're coverage debt: surface where a silent
under-rejection could still hide.

**1. Control-flow / path-sensitivity axis — DONE (core), 2026-06-07.**
`tests/test_cflow_matrix.c` now covers `{pool, slab}` × 19 control-flow
scenarios (if-then/if-both/loop/next-iter/switch-one/switch-all/double-if/
leak-if/leak-loop/nested-if/break/continue/defer-double + the safe POS
counterparts), 38/38. Found+fixed BUG-727 (defer + explicit double-free was a
false negative). Residual control-flow shapes NOT yet enumerated (lower
likelihood, add when convenient): `orelse`-unwrap/fallback paths crossed with
free state; `goto` (forward past a use; backward re-use); deeply-nested (3+)
control flow; move-struct and `*opaque` types under control flow (the cflow
matrix uses pool Handle + slab `*T` only). None are known bugs — coverage debt.

**2. `*opaque`/extern type row — LOW value (lock-in).** A 4th type
(`TY_OPAQUE`: bodyless `?*opaque make()` extern-alloc + `void destroy(*opaque)`
extern-free). Codegen catch: positives can't `--run` (bodyless externs won't
link). Solution worked out: add a per-type `compile_only` flag and assert the
positive with EMIT-ONLY (`zerc f.zer -o /tmp/x.c`, no GCC/link) = exit 0 means
zercheck accepted; negative = `-o /dev/null` still fails at zercheck before
link. Locks in GAP-B (extern-alloc+orelse) and GAP-D (destructor heuristic).
Just audited 2026-06-06, so mostly regression lock-in.

**3. Smaller lock-in cells — LOW value.** wrong-pool (`pool_a` handle freed via
`pool_b` — BUG-471 class); move-array (`consume(arr[0])` move element —
BUG-476 class); alias chains (`h2=h1; free(h1); use(h2)`); return-escape
(`return h` — escape-vs-leak distinction); slab field/array storage (currently
pruned for the `*T`-in-struct non-null concern — verify whether it works).

**4. Different domain → SEPARATE harness.** Concurrency / ISR / atomics / async /
MMIO — promoted to its own actionable entry: see "OPEN — next frontier:
concurrency / ISR / atomics / async / MMIO oracles" above (per-domain
accept/reject rules + survey-first plan + harness notes).

When any axis is added: add the cells, update the "Extending the grid" coverage
list in `docs/compiler-internals.md`, and trim this entry.

---

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

## OPEN — `naked` attribute silently dropped on IR path

See full entry near the bottom of this file ("`naked` attribute
silently dropped on IR path (deferred 2026-05-02)") — kept in original
location to preserve the more detailed analysis added in the
2026-05-02 fix session.

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

## Remaining known failures

### No skipped tests

All tests run. The 2 mmio hardware tests moved to `rust_tests/qemu/` and
run under QEMU Cortex-M3 (see `docs/compiler-internals.md` "QEMU MMIO
test infrastructure").

---

## Phase 1 audit findings (2026-04-19 — 52 adversarial tests, 24 safety systems)

Full systematic audit of the 29-system framework. 52 adversarial `.zer`
programs were written, one or more per system, each attempting to
trigger a safety-system violation. Expected: every test compile-errors.
Observed: 7 real gaps + 1 silent miscompilation (fixed) + 7 over-pessimistic
spec claims (corrected — tests revealed the claims were weaker than
the implementation actually guarantees).

Reproducers live in `tests/zer_gaps/` (committed as documentation of
current behavior — NOT in the `make check` run, since they pass when
they should fail). When a gap is fixed, move the reproducer to
`tests/zer_fail/` as a regression test.

### Real safety/correctness gaps

**Status update (2026-05-21 audit):** all 7 originally-listed gaps from
the 2026-04-19 audit are NOW CAUGHT by the production compiler.
Re-verified empirically — see status column. Phase F migration to IR-CFG
analyzer + multiple bug fixes since then closed them. Left in place as
history; can be removed once a follow-up audit re-confirms.

| # | Short name | Gap | Status 2026-05-21 |
|---|---|---|---|
| 1 | **Cross-block backward goto UAF** | `free(h); goto LABEL; ... LABEL: ... use(h)` | **CAUGHT** — IR fixpoint convergence widens through backward edges; reports `use after free: 'mh' is maybe-freed` |
| 2 | **Same-line UAF suppressed** | `h.free(p); u32 x = p.x;` on same line | **CAUGHT** — zercheck_ir reports `use after free: 'h' is freed (freed at line 7)` |
| 3 | **`yield` outside async silently stripped** | `void go() { yield; }` compiles; emits no-op | **CAUGHT** — `error: 'yield' only allowed inside async function` |
| 4 | **async + shared struct across yield** | `shared struct` access across `yield` = deadlock | **CAUGHT** — checker.c:5034-5038 rejects shared struct access in yield statement |
| 5 | **`container<move struct>` loses move semantics** | `Box(Tok) b; b.item = t; consume(b.item); b.item.k` | **CAUGHT** — zercheck_ir reports `use after free: 'b' is transferred` (move tracking through field writes works for the documented pattern) |
| 6 | **`goto` into if-unwrap capture scope** | `goto inside; if (m) \|v\| { inside: ... }` | **CAUGHT** — `error: goto 'inside' jumps into if-unwrap/switch-capture arm without binding the capture` |
| 7 | **`defer` nested in `defer` body** | `defer { defer { ... } }` | **CAUGHT** — `error: 'defer' cannot be nested inside another 'defer' body` |

### Precision issues (not safety)

- **VRP doesn't propagate `u32 i = literal_value`** (`tests/zer_gaps/s12_range_oob.zer`) — direct `arr[10]` is rejected at compile time, but `u32 i = 10; arr[i];` only triggers auto-guard warning. Safe via auto-guard emission. VRP improvement opportunity.
- **`*opaque` cast to wrong type inside same function** (`tests/zer_gaps/s5_param_prov.zer`) — param used as both `*A` and `*B`. Compile accepts; runtime type_id check traps on the wrong-type cast. Same class as Gap 1 — compile blind, runtime catches.

### Fixed this session

- **Comptime loop truncation** — silent miscompilation where 10k iter cap
  stopped loops without error, returning truncated values. See BUG
  entry above / `tests/zer_fail/comptime_loop_truncation.zer`.
- **Mutual recursion handle tracking** (Gap 2 from earlier spec) —
  fixed via iterative FuncSummary refinement. See
  `tests/zer_fail/mutual_recursion_uaf.zer`.

### Spec corrections (claims were stronger than needed)

Systematic adversarial testing found several EDGE CASES in the safety
spec that were pessimistic — the implementation actually handles them:

- **Pass-by-value move transfer** is caught (spec said "not a transfer").
- **Mutual recursion with `% N` return range** is propagated (spec said cleared).
- **Simple 2D array UAF** is caught (spec said "not covered").
- **Defer fires after return expression** — using handle in return expr before defer free is legal (test was wrong, not a gap).
- **Semaphore release without acquire** is legal (initial-count pattern).
- **Per-statement shared(rw) locking** is correct (test was wrong).
- **goto skipping a `defer`** is correct (defer never pushed = no fire, semantically fine).

### Empirical coverage

- All 4 safety models covered.
- All 24 safety-critical systems tested at least once.
- Infrastructure systems 1 (Typemap) and 2 (Type ID) not adversarially
  tested — they're self-validating (broken Typemap = no test compiles;
  broken Type ID = cross-module cast mismatches). Existing 1400+
  passing tests indirectly validate them.

---

## Phase 2 audit findings (2026-04-19 — code-inspection targeted tests)

After the phase 1 behavioral audit, I read zercheck.c/checker.c/emitter.c
looking for structural weaknesses (fixed buffers, depth caps, TODO
markers) and wrote targeted tests for each candidate. Reproducers in
`tests/zer_gaps/audit2_*.zer`.

### ~~Severe — `[*]T` slice bounds check missing on IR path~~ (FIXED 2026-04-19, commit 3bdcf85)

Fixed as part of the Phase 3 sweep — `IR_INDEX_READ` + `emit_rewritten_node`
NODE_INDEX now emit `_zer_bounds_check` wrapper for slices. Both
READ and WRITE covered (comma operator preserves lvalue).
Retained below as audit history.

### Severe — `[*]T` slice bounds check missing on IR path (REGRESSION)

**Reproducers:** `audit2_slice_oob.zer`, `audit2_slice_star_oob.zer`.

`emitter.c:7498` `IR_INDEX_READ` handler emits raw `src.ptr[idx]` for
TYPE_SLICE sources with NO `_zer_bounds_check` call. The comment claims
"Bounds checks are in the AST path (emit_expr via IR_ASSIGN
passthrough)" — but function bodies have been IR-only since 2026-04-19,
so the AST TYPE_SLICE branch at `emitter.c:2045-2067` is never reached.

**Verified across three entry points:**
- stack array coerced to `[*]T` via `arr[0..]`
- arena-allocated slice from `ar.alloc_slice(T, n)`
- function parameter `[*]T s`

All emit `s.ptr[idx]` unchecked. Runtime silently reads stale/OOB
memory, exit 0.

**CLAUDE.md currently claims:**
> "[*]u8 data; dynamic pointer to many — {ptr, len}, bounds checked"

That claim is CURRENTLY FALSE for any slice indexing after IR migration.

**WRITE path also broken.** Verified `s[i] = 99` emits `s.ptr[i] = 99`
with no bounds check. `IR_INDEX_WRITE` handler at `emitter.c:7626` is
a stub (`TODO` comment). Slice element assignment is currently an
uncontained buffer overflow primitive.

**Regression timeline (confirmed via git archaeology):**
- Commit `010ddea` (2026-04-15, "Phase 8b: local-ID emission for
  BINOP/UNOP/FIELD_READ/INDEX_READ") replaced `emit_expr(inst->expr)`
  with direct local-ID emission. The AST emitter had the bounds
  check at `emitter.c:2045-2067` TYPE_SLICE branch; direct emission
  strips it.
- Commit `82335c3` (2026-04-17, "flip use_ir default") made IR path
  default. Regression became effective.
- Tests didn't catch it because VRP proves most real-world slice
  indexes safe, eliminating the need for runtime check.

**Fix:** ~15-20 lines in `IR_INDEX_READ` emitter — port the TYPE_SLICE
branch from AST `emit_expr`. Same needed for `IR_INDEX_WRITE` (which
is currently only a stub).
**Priority 0.** Highest-impact safety gap in the codebase.

### Major — backward goto cross-block (Gap 1 root cause confirmed)

**Reproducers:** `audit2_cross_block_goto.zer` (+ `_handle` variant
that traps at runtime proving the class), `audit2_goto_across_scope.zer`,
`audit2_labels_32_overflow.zer`.

`zercheck.c:1636` collects labels into block-local `labels[32]`.
`zercheck.c:1668` backward-goto iteration keyed on that array. Two
failure modes:

1. **Cross-block:** goto inside inner block targets label in outer
   block. Inner block's `labels[]` doesn't contain the outer label, so
   `label_idx = -1`, iteration doesn't fire, UAF across the cycle
   missed.
2. **Buffer overflow:** `labels[32]` is a fixed-size stack buffer.
   A block with 33+ labels silently drops the rest (CLAUDE.md rule
   #7 violation — stack-first dynamic pattern not applied here).
   Backward goto to a label past index 32 = label not found =
   no iteration.

Same root cause fix subsumes all three: replace block-local label
collection with CFG analysis, OR at minimum use stack-first dynamic
array for labels[]. The full CFG fix is ~300 lines.

### Moderate — `_scan_depth < 8` spawn transitive data race detection

**Reproducer:** `audit2_spawn_transitive_depth.zer`.

`checker.c:6466` caps transitive call-chain scanning at 8 levels. A
10-level chain `spawn entry() → f1 → f2 → ... → f10 → g = g + 1` does
not detect the non-shared global touched at the end of the chain.
CLAUDE.md already documents "Transitive through callees (8 levels)"
as the design limit, but 8 is low for real call graphs.

**No runtime fallback** — data races silently occur.

**Fix:** raise cap to 16-32, or memoize per-function scan result to
avoid re-analysis. The cap exists to prevent infinite recursion on
recursive call chains.

### Confirmed NOT-gaps (positive coverage — keep as regression tests)

- `audit2_funcsummary_chain.zer` — 6-level free chain propagates via
  FuncSummary iterative refinement.
- `audit2_nested_if_chain.zer` — 5-level else-if chain with handle
  freed in 4/5 arms correctly flagged MAYBE_FREED.
- `audit2_switch_partial_transfer.zer` — 5-arm switch with 3 freeing,
  2 not → MAYBE_FREED correctly emitted.

### Behavior to investigate further

- `audit2_defer_scan_nested.zer` — 32 levels of nested `if (c) {...}`
  with defer at innermost compiles clean. Unclear whether
  `scan_stack[32]` overflow was hit and zercheck still found the defer
  via direct walk, or whether depth wasn't actually > 31. Requires
  instrumentation to confirm.

### ~~AST→IR emission audit — 6 more runtime-check regressions~~ (FIXED 2026-04-19, commit 3bdcf85)

**All 7 Phase 3 regressions FIXED.** Full details in `BUGS-FIXED.md`
under BUG-595 through BUG-599. Test suite green: 290/290 ZER
integration, 139/139 convert, 200/200 negative, all subsuites
unchanged. The section below is retained as audit history and
methodology reference.

### AST→IR emission audit — 6 more runtime-check regressions found

After confirming the slice bounds check regression, I ran a systematic
AST→IR diff audit: grep every `_zer_trap` / `_zer_bounds_check` /
`_zer_shl` / `_zer_probe` call-site in `emit_expr` (AST path, now
mostly dead for function bodies), then wrote one reproducer test per
mechanism. Reproducers in `tests/zer_gaps/ast_*.zer`.

**All regressions in same window** — commits `010ddea` (2026-04-15,
Phase 8b local-ID emission) through `82335c3` (2026-04-17, IR default).

| # | Mechanism | AST path | IR path | Reproducer |
|---|---|---|---|---|
| 1 | Slice bounds check (READ) | `_zer_bounds_check` emitter.c:2045 | raw `s.ptr[i]` emitter.c:7498 | `audit2_slice_oob.zer` |
| 2 | Slice bounds check (WRITE) | same as READ | `IR_INDEX_WRITE` stub `/* TODO */` emitter.c:7626 | same |
| 3 | Slice range `arr[a..b]` (a > b) | `_zer_trap("slice start > end")` emitter.c:2258 | raw `se - ss` (underflow) | `ast_slice_empty_range.zer` |
| 4 | Signed division overflow (INT_MIN/-1) | `_zer_trap("signed division overflow")` emitter.c:1068 | raw `a / b` (C UB) | `ast_signed_div_overflow.zer` |
| 5 | Shift over width (`x << n` where n ≥ width) | `_zer_shl` macro (clamps to 0) | raw `x << n` (C UB) | `ast_shift_over_width.zer` |
| 6 | @inttoptr mmio range (variable address) | `_zer_trap("outside mmio range")` emitter.c:2650 | raw cast, no check | `ast_inttoptr_mmio.zer` |
| 7 | @inttoptr alignment (variable address) | `_zer_trap("unaligned address")` emitter.c:2660 | raw cast, no check | `ast_inttoptr_align.zer` |

**NOT regressions** (still protected):
- Division by zero — checker forces compile-time guard (can't reach without explicit `if (b==0)`)
- @ptrcast type mismatch — checker catches at compile time via provenance
- Compile-time-provable array OOB (`arr[10]` on `u32[4]`) — checker error
- Array runtime OOB with variable index — `emit_auto_guards` separate pass still works
- @trap / @probe — emitted correctly through IR (verified)
- Handle gen check — `_zer_slab_get` runtime always called, independent of emit path

**Root cause for all 7:** commit `010ddea` replaced `emit_expr(inst->expr)`
routing with direct local-ID emission in IR handlers. Every safety-emit
that `emit_expr` wrapped around expressions was stripped. Arrays
survived because `emit_auto_guards` runs as a separate pass before IR
lowering. Slices and the other mechanisms have no separate-pass
equivalent.

**Impact:** Currently shipping v0.4.5 produces binaries with:
- Unchecked buffer overflows on any `[*]T` indexing
- Silent integer UB on shifts and signed division
- No MMIO range or alignment safety on variable-address @inttoptr

**Fix is localized:** port each safety-emit from `emit_expr` into the
corresponding IR emitter handler. Estimated:
- IR_INDEX_READ/WRITE: ~30 lines
- IR_BINOP (shift, signed div): ~20 lines
- IR_CAST / @inttoptr handling: ~30 lines
- Slice `[a..b]` range check: ~10 lines

Total ~90 lines in emitter.c. No checker or IR data structure changes.
Would graduate the compiler from "unsafe in ways CLAUDE.md claims it
isn't" back to its pre-IR-migration safety level.

### Doc accuracy issue

CLAUDE.md states `alloc_ptr/free_ptr` is "100% compile-time safe for
pure ZER code." That is aspirational — zercheck has known gaps, and
**unlike Handle, `*T` from `alloc_ptr` has NO runtime generation
check**. Post-free pointer deref reads stale slab memory silently
(verified: Handle variant of `audit2_cross_block_goto` traps via gen
check; `*T` variant returns 0). Update doc to state: "compile-time
only for pure ZER; prefer Handle when runtime fallback is desired."

Also: only `*T` has `alloc_ptr/free_ptr`. `[*]T` has no equivalent —
it must come from `arena.alloc_slice` (whole-arena-reset semantics)
or from `arr[0..]` coercion (stack). CLAUDE.md should make this
explicit.

---

## Bare-metal: `.bss` zeroing requirement (Gap 13, 2026-04-27)

ZER's "everything auto-zeroed" guarantee depends on the C runtime startup
zeroing the `.bss` section before `main()` runs. On hosted targets
(Linux/Win/macOS), the C runtime (`crt0`/`crt1`) handles this automatically.

**On bare-metal targets (Cortex-M, RISC-V, custom kernels), the user-supplied
linker script + startup assembly MUST zero `.bss` before calling main.**

Without this, uninitialized globals hold whatever random values were in RAM
at boot — silently breaking ZER's auto-zero guarantee.

Standard pattern in startup `.S` files:
```asm
ldr r0, =__bss_start
ldr r1, =__bss_end
mov r2, #0
bss_zero:
    cmp r0, r1
    beq bss_done
    str r2, [r0], #4
    b bss_zero
bss_done:
```

See ARM Cortex-M reference startup code, ESP-IDF's `cpu_start.c`, or
Linux kernel's `head.S`. ZER cannot enforce this from inside its emitted
code — it's a build-system / startup contract.

If your target's startup does NOT zero `.bss`, ZER's safety guarantees
on global variable initial state are void. Verify your linker script
includes the `.bss` zeroing loop before claiming bare-metal correctness.

---

## *opaque ghost handle wrap pattern (Gap 4, 2026-04-27)

`*opaque` pointers crossing the C-interop boundary (via `cinclude`-declared
functions returning/taking `*opaque`) have heuristic-only lifetime tracking
in zercheck. Coverage is ~98% for typical patterns but the residual ~2%
requires the **wrap pattern** + `--track-cptrs` flag for full safety.

### When the heuristic suffices

zercheck recognizes the following patterns automatically:
- `void destroy(*opaque)` — bodyless void → assumed-free heuristic
- `int xyz_close(*opaque)` — bodyless non-void with destructor-name in
  the function name (free, destroy, close, release, delete, dispose,
  drop, cleanup, deinit, fini, shutdown, term) → assumed-free
  heuristic (Gap 17 fix, 2026-04-27)
- ZER wrapper functions where the body is visible — full FuncSummary
  tracking applies

### When the wrap pattern is required

If the C library has an idiosyncratic destructor name (no recognized
substring) AND the function body is invisible (cinclude only), the
heuristic doesn't fire. Solution: write a thin ZER wrapper.

```zer
// thin wrapper makes intent explicit and gives zercheck a body to scan
void mylib_dispose(*opaque h) {
    @ptrcast(*MyType, h); /* validate type via provenance */
    mylib_xyz_terminate_session(h); /* obscure C name */
}
```

### `--track-cptrs` runtime backup

For the residual 2% (anonymous casts, dynamic dispatch through C function
pointers), enable `--track-cptrs`. This compiles in:
- `__wrap_malloc`/`__wrap_free` linker wrap
- Inline allocation header on every `*opaque` pointer
- Runtime UAF detection at every `@ptrcast` deref

Cost: ~1ns per `@ptrcast` deref + extra header bytes per allocation. NOT
applicable to bare-metal (requires libc malloc).

### Coverage summary

| Pattern | Compile-time | Runtime backup | Total |
|---|---|---|---|
| Pure ZER (no `*opaque`) | 100% via Handle states | not needed | 100% |
| `*opaque` with wrap functions | ~99% | not needed | ~99% |
| `*opaque` raw cinclude + dtor-name fn | ~98% (heuristic) | not needed | ~98% |
| `*opaque` raw cinclude + opaque-name fn | ~80% (alias only) | `--track-cptrs` | ~99% |
| `*opaque` through dynamic funcptrs | ~50% (no track) | `--track-cptrs` | ~95% |

Recommendation: write ZER wrappers around C libraries. The wrap pattern
is documented architecture and pays back as runtime overhead saved +
clearer code intent.

---

## `naked` attribute silently dropped on IR path (deferred 2026-05-02)

**Status:** known regression from IR migration; not fixed because fixing
breaks every existing `tests/zer/asm_*.zer` test.

**Symptom:** ZER source declaring `naked void f() { asm { ... } }`
emits C without `__attribute__((naked))`. GCC therefore generates a
normal prologue + epilogue around the asm body. The asm appears to
"work" because the implicit prologue/epilogue saves/restores callee-
saved registers and ensures `ret` happens via the epilogue.

**Why this is a real loss of safety semantics:**

A genuinely naked function has no compiler-generated prologue/epilogue
— the user controls every byte. This matters for:

- Interrupt handlers using `iret` directly (no implicit `ret`)
- Boot/reset handlers that haven't set up the stack yet
- Context switch primitives that save callee-saved registers themselves
- Code that needs exact frame layout (no surprise spills)

With the implicit prologue, these scenarios silently malfunction (or
leak/corrupt registers) but the asm "compiles".

**Why it's deferred:**

Existing tests/zer/asm_*.zer (10+ files) all rely on the implicit
prologue/epilogue. Their asm bodies omit explicit `ret` instructions.
Re-enabling `__attribute__((naked))` would SIGILL each of them at
runtime (function falls off the end without `ret`).

Restoring true naked semantics requires:
1. Updating every existing asm test to include explicit `ret` /
   `iret` / `eret` per architecture
2. Adding a checker rule that bans `return expr;` inside naked
   functions (only `return;` or no return at all permitted)
3. Auditing user-facing docs and bumping any "naked" examples
4. A user-visible breaking change announcement

That's a separate migration effort, not in scope for the bug-fix
sessions. The current state (silent attribute drop) is documented
here so fresh sessions know it's INTENTIONAL deferral rather than
oversight.

**Workaround for users TODAY:** if you actually need true naked
semantics, write the function in C and link via `cinclude`. This is
the pattern used in real projects shipping production firmware.

**Tracking:** when the asm-test migration lands, re-enable the
attribute in `emit_func_attributes` (emitter.c) and remove this entry.

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

## OPEN — 2026-06-18 multi-agent bug-hunt (BH-18): index + reproduction harness

A 12-finder adversarial bug hunt against current HEAD (`cc374ab`) found and
triple-verified (finder → independent verifier → maintainer re-run in a
container) **14 distinct compiler bugs**. All compile **clean** unless noted.
Six are memory-unsafe soundness holes. Each entry below (BH-18 #1..#14) is
self-contained: minimal reproducer, exact observed/expected, the asymmetric
control that proves it is a gap (not a design choice), root cause, and a fix
sketch. A fresh session can reproduce every one with only the steps here.

> **WARNING: This index PREDATES the 41-fix merge-back (completed 2026-07-15).** Several
> BH-18 rows have since been CLOSED on main and must NOT be treated as still-open —
> cross-check the "## DONE … TASK TRACKER COMPLETE" section at the top of this
> file + `git log` before reproducing any row. Verified closed on main by the
> merge-back: **#2 (VRP range-narrow scope leak → OOB) = §C #13** (`vrp_snap_*` /
> `vrp_join_assign_range`, tests `vrp_{branch_assign,loop_assign}_guard_ok`);
> **#1 (move-struct pointer-alias) = §A #7 + the FIXED-2026-07-01 subsections below**;
> **#7 (shared multi-access via cast/intrinsic/index/orelse subexpr)** was closed
> independently 2026-06-27 (`collect_shared_types_in_expr` no-default switch under
> `-Werror=switch`, see CLAUDE.md). Remaining rows MAY overlap fixes merged in this
> window (e.g. #5, #8, #9) — verify each against the tracker + `git log` before
> treating it as open; do not assume open OR closed without checking.

**How to reproduce (any fresh session):**
```
# build the compiler (native or in Docker; Docker avoids Windows AV):
gcc -O1 -w -I. -o zerc lexer.c parser.c ast.c types.c checker.c emitter.c \
    zercheck.c ir.c ir_lower.c zercheck_ir.c vrp_ir.c zerc_main.c src/safety/*.c
# OR: make zerc

zerc x.zer --run # compile+run; prints "zerc: running x" then process exit = main()'s return
zerc x.zer -o x.exe # compile only (clean compile = no 'error:' line, exit 0)
zerc x.zer --emit-c -o x.c # inspect the emitted C (proves dropped guards / placeholders)
# To PROVE an out-of-bounds access is real (#2,#4,#5), ASan the emitted C:
zerc x.zer --emit-c -o x.c && gcc -fsanitize=address -g -O0 x.c -o x_asan && ./x_asan
```
A "soundness hole" = clean compile + memory-unsafe/guarantee-violating at
runtime (the crown-jewel class — ZER claims 100% program-consequence
coverage). "miscompile" = clean compile + wrong runtime result. Severity tags:
CRITICAL critical (memory-unsafe), HIGH high (race / double-free), MEDIUM medium
(miscompile), LOW low (false-reject / diagnostic).

| # | Bug | Class | Root-cause area |
|---|---|---|---|
| 1 | move-struct pointer-alias defeats ownership/free tracking (heap UAF + use-after-move + double-consume) | CRITICAL soundness | zercheck_ir move/alias |
| 2 | VRP range-narrow scope leak → OOB write | CRITICAL soundness | checker.c if-VRP |
| 3 | `@bitcast` forges integer↔pointer | CRITICAL soundness | checker `@bitcast` |
| 4 | `@pun(*Struct,*primitive)` skips type_id trap → OOB read | CRITICAL soundness | emitter `@pun` guard |
| 5 | fixed-array bare-call index drops bounds check | CRITICAL soundness | emitter index single-eval |
| 6 | `if (opt) \|*v\|` capture escapes to a global | CRITICAL soundness | capture desugar + escape prov |
| 7 | shared-struct multi-access via cast/intrinsic/index/orelse subexpr evades lock check → race | HIGH race | checker `collect_shared_types_in_expr` |
| 8 | `spawn` data-race scan blind to funcptr indirection → race | HIGH race | checker `scan_unsafe_global_access` |
| 9 | shared-struct read in `await` condition unlocked → race | HIGH race | checker `NODE_AWAIT` handler |
| 10 | value-returning `async` never finalizes state machine | MEDIUM miscompile | emitter `IR_RETURN` async |
| 11 | bit-query/byte-swap intrinsics emit `0` in global initializers | MEDIUM miscompile | emitter AST `NODE_INTRINSIC` |
| 12 | `defer` + backward `goto` fires wrong count (folds into known #5) | MEDIUM miscompile | ir_lower defer/back-edge |
| 13 | nested inline designated initializer false-reject | LOW false-reject | checker `validate_struct_init` |
| 14 | conversion-intrinsic arity not validated (missing/excess args) | LOW diagnostic | checker intrinsic arg check |

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

## OPEN — Concurrency memory-safety: all ~25 audited holes CLOSED + named floors (2026-06-22, updated 2026-08-08)

> **STATUS UPDATE 2026-08-08.** The "single remaining hole" named throughout this entry — the
> **cross-block scoped-borrow** case — was **CLOSED 2026-08-03**. The fix was NOT the
> subsystem-scale `zercheck_ir` borrow-set merge sketched below: the borrow is a linear
> statement-order approximation in `checker.c` (`Symbol.is_borrowed_by_thread`), not IR state,
> so an IR merge would have edited a component that never held it. Real defect: a `th.join()`
> nested in a branch cleared the borrow for code AFTER the branch, on paths that never joined.
> Fix = `Checker.branch_depth` + `Symbol.th_spawn_branch_depth` (~40 lines). Residual is
> PRECISION only (a join on every arm is over-rejected) — tracked as its own OPEN entry.
> **Text below is preserved as the historical audit record; read the sketch as superseded.**

**Scope of this entry:** ZER's concurrency PRIMITIVES are all implemented
(shared/spawn/atomics/Semaphore/Barrier/condvar/Ring/async/move). This entry is
the standing ledger of the **memory-safety gaps** in the concurrency model —
verified data races + cross-thread use-after-free that compile clean. Full
design + Rust mapping + closure: `docs/primitives-data-races.md` §24. Per-hole
file:line detail: workflow task outputs `wpbbu8v47` / `wwt4c31zh` / `wgvm1bid5`.
**Do NOT yet claim ZER is FULLY data-race-safe as shipped** — but the open surface
is now ONE hole, not ~25. ~24 of the ~25 audited holes are CLOSED + regression-tested
(BUG-743..759: Axis B complete, A6-full atomic-cell taint complete, A5 threadlocal
`&`-escape fixed, Axis C `threads[]` merge + scoped-borrow same-block read/write
fixed). The single remaining memory-safety hole is the **cross-block scoped-borrow
case** (spawn and access in different CFG blocks; same-block is covered) — detailed
below. Everything else is named FLOORS, out of scope for ZER *and* Rust: D1 cinclude
thread-capture (C-domain, safe path exists) and liveness (deadlock/livelock). So:
very close, not 100% — one subsystem-scale fix (a zercheck_ir borrow-set merge) away.

**IMPLEMENTATION PROGRESS (phase 2, session 2026-06-21b) — 9 holes CLOSED
(BUG-743..751), each verified by the full ZER suite + C unit tests (every fix
below has a negative test in `tests/zer_fail/` that runs in `make check`, which
IS the regression gate; full `make check` GREEN — ZER 774, Rust 784, Zig 36,
modules 139, all audits OK):**
- **[FIXED BUG-743, Axis C]** `ir_merge_states` now merges `threads[]` (union by
  name, `joined` = AND over preds) + the convergence check compares thread state
  — the false-green scoped-spawn stack-UAF is closed. (The concrete soundness bug
  the audit flagged as "most actionable.")
- **[FIXED BUG-744, Axis A1]** spawn-arg dispatch is exhaustive over
  pointer/slice/opaque (`[*]T` over stack / `(*opaque)&local` now caught).
- **[FIXED BUG-745, Axis C2]** fire-and-forget spawn of a pointer to a STACK
  local (incl. `*shared T`) now rejected (lifetime arm).
- **[FIXED BUG-746, Axis A3]** volatile compound-RMW in a spawn target now flags
  (wired `zer_volatile_compound_valid`); simple volatile store still allowed.
- **[FIXED BUG-747, Axis A4]** Arena removed from the spawn scanner's
  safe-exclusion (concurrent `arena.alloc()` races).
- **[FIXED BUG-748, Axis D2]** `@probe` `_zer_in_probe`/`_zer_probe_jmp` now
  `__thread` (no cross-thread longjmp corruption).
- **[FIXED BUG-749, Axis B5]** deferred shared-struct access (`defer g.x = v;`)
  now lock-wrapped in the emitter.
- **[FIXED BUG-750, Axis A6/#5]** the interior-extraction ban (`&shared.field`)
  now also covers pointer-rooted (`*Counter c; &c.value`) and array-element
  (`&shared.arr[i]`) bypasses; `&whole_struct` still allowed. Also: the
  positive-test runner now has a 30s `timeout` so a deadlocking auto-lock fails
  red (exit 124) instead of hanging CI — the visibility mechanism for the B
  lock-scope redesign.
- **[FIXED BUG-751 + BUG-759, Axis C scoped-borrow]** a parent WRITE (751) or READ
  (759) of a non-shared local lent via `&x` to a scoped spawn, between `spawn` and
  `th.join()`, now errors (the thread has exclusive `&mut`-style access until join).
  Both linear (same-block); the borrow flag is set at the spawn and cleared at join.
  **Remaining [OPEN]: cross-block** (spawn and access in different CFG blocks) — the
  proper fix is a borrow-set merge in zercheck_ir (like the `threads[]` merge),
  subsystem-scale and lower-value now that same-block read+write are both covered.
- **[FIXED BUG-752, Axis A6-full, #7 — atomic-cell inclusion model, slices 1/2/4
  DONE]** a scalar global used with `@atomic_*` is an atomic cell; in a
  fire-and-forget concurrent context, a plain WRITE (slice 1), a plain READ
  (slice 2), and an address-launder `&g` for non-atomic use (slice 4) are all
  flagged — the taint is non-strippable via `&` (only the `@atomic_*` target arg0
  is blessed). **Concurrency-aware (gated on fire-and-forget after-spawn), NOT
  strict-always** — strict false-positived 21 safe pre-spawn `g=0` inits; the
  gate keeps pre-spawn init + single-threaded + post-scoped-join access legal
  (matches "shared = reachable by ≥2 threads"). **LEARNING:** the inclusion model
  is NOT strictly simpler than the exclusion-list — it ALSO needs concurrency
  context. **Remaining [OPEN, narrow]:** struct-field atomics `@atomic_*(&s.f)` on
  a plain (non-shared) global struct (slice 3) — needs a parallel
  (struct-symbol, field) compound-key list (the scalar machinery keys on the
  Symbol, which has no per-field flag); uncommon + mostly logic race.
  **UPDATE: slice 3 DONE** — struct-field atomic cells now tracked field-precise
  (`Checker.atomic_fields`, write side). **A6-full atomic-cell taint COMPLETE**
  (scalar write/read/launder + struct-field, all concurrency-aware). The
  remaining exclusion-list entries (const/shared-struct/threadlocal/atomic/
  Barrier/Semaphore) are the genuinely-safe SYNCHRONIZED categories, not holes.
  Micro-residuals **[FIXED BUG-758]**: struct-field plain READS + `&s.f` launder
  now tracked (read hook at NODE_FIELD, launder hook at TOK_AMP, mirroring the
  scalar slices 2/4, gated on the fire-and-forget after-spawn context). **A6-full
  is now COMPLETE end to end** — scalar + struct-field, write/read/launder.

**AXIS B IS COMPLETE (2026-06-22): B1 multi-root (BUG-753), B2 union copy-out
(BUG-754), B3 cond_wait foreign-shared reject (BUG-755), B4 @once loser-wait
(BUG-756), B5 defer lock (BUG-749).** A6-full atomic-cell taint complete (BUG-752 +
758). **A5 threadlocal `&`-escape FIXED (BUG-757).** Scoped-borrow read+write are
both FIXED (BUG-751 + 759). The remaining OPEN hole is now ONLY the **scoped-borrow
CROSS-BLOCK case** (spawn and access in different CFG blocks — same-block read+write
are covered; the proper fix is a borrow-set merge in zercheck_ir, subsystem-scale).
**D1 (cinclude thread-capture) is RECLASSIFIED as a named FLOOR, not a hole**
(C-domain behavior, out of ZER's scope; safe path already exists via long-lived
data — see Axis D).
**Risk classification (confirmed 2026-06-21b):** a botched lock-scope redesign's
worst NEW failure is a DEADLOCK = a hang = the liveness floor (NOT a memory-safety
violation; out of scope for ZER *and* Rust), now made VISIBLE by the runner
timeout; the shared-scalar extension's risk is a FALSE-POSITIVE = a visible
compile error on a positive test. Neither is a new memory-safety hole, so both are
safe to iterate on ("loop till green"); the under-lock botch direction cannot make
memory safety worse than the already-open hole.

Three adversarial, code-grounded sweeps found **~25 holes**; the find-rate did NOT
decay (9 → 11 → ~10 new), proving they are generated by a few architectural roots,
not scattered bugs. All map to **four axes**:

**Axis A — exclusion-list reachability scanner** (`scan_unsafe_global_access`
checker.c + spawn-arg handler). Every exclusion / forgotten type-kind is a hole:
- **[FIXED BUG-746]** `volatile` whitelisted → thread `volatile` compound-RMW
  race. Now: `NODE_ASSIGN` case wires `zer_volatile_compound_valid`; volatile
  compound-RMW in a spawn target flags, simple volatile store still allowed.
  "or volatile" removed from the spawn fix suggestion.
- **[FIXED BUG-747]** `Arena` whitelisted → concurrent `arena.alloc()` races.
  Now removed from the safe-exclusion (Barrier/Semaphore kept — internally synced).
- **[FIXED BUG-757 — A5]** `threadlocal` whitelisted for direct access (CORRECT — a
  spawned thread reads its own TLS copy), but `&threadlocal` published to a
  non-threadlocal global/static was a cross-thread wrong-TLS/UAF. Now rejected at
  the escape-sink assignment check (checker.c) — an `else if` after the `&local`
  branch, since a threadlocal is global-scope (`val_is_global`) so the `&local`
  check skipped it. Storing `&tl` into ANOTHER threadlocal is within-thread →
  allowed.
- **[FIXED BUG-744]** `TYPE_SLICE` + `TYPE_OPAQUE` spawn args were uncased → now
  the spawn-arg dispatch is exhaustive over pointer/slice/opaque; the
  `spawn_arg_is_stack_derived` helper unwraps casts + `@ptrcast/@bitcast/@cast/@pun`
  (so `(*opaque)&local` and `[*]T` over a stack array are caught).
- Remaining fix direction: carrier-or-tainted *inclusion* model (the full A6
  shared-scalar taint) replaces the exclusion list entirely; a `-Wswitch`-style
  gate on the spawn-arg dispatch.

**Axis B — single-root auto-lock incompleteness** (per-statement
`current_stmt_shared_root`, ir_lower.c) — **ALL FIVE SUB-ITEMS NOW CLOSED
(2026-06-22)**. Was: locks only the first shared root; bypassed at:
- **[FIXED BUG-753 — B1]** `shared(rw)` multi-read (`x = ga.v + gb.v`) now locks
  ALL distinct shared roots, not just the first (`find_all_shared_roots_expr`;
  extras as read locks — deadlock-free since read locks compose and the
  multi-WRITE case is rejected by the deadlock check; lock/unlock re-derive the
  set, so no `current_stmt_shared_root`-set change was needed). Non-`orelse`
  statements only (narrow residual). Verified in emitted C.
- **[FIXED BUG-754 — B2]** union-switch on a shared struct field: the lvalue path
  built `sw_ref = &g.union` (a raw alias into the shared bytes) and the discriminant
  + capture reads happened AFTER the lock released — even the `|x|` VALUE capture was
  a cross-thread torn read / type confusion. FIX (copy-out, ir_lower.c): when the
  switch root is shared (`find_shared_root_expr`, covers `shared(rw)` too) take the
  RVALUE path, which copies the whole union into a LOCAL *under* the switch-expr lock;
  every subsequent tag/capture read is then of a private snapshot. The `|*x|` mutable
  capture of a shared union is REJECTED at the checker (it would alias the throwaway
  copy → lost mutation; mirrors the A6/#5 interior-extraction ban). No nested lock
  (lock scope unchanged), no new IR. Tests: `tests/zer/shared_union_switch_copyout.zer`,
  `tests/zer_fail/shared_union_switch_ptr_capture.zer`.
- **[FIXED BUG-755 — B3]** `@cond_wait`/`@cond_timedwait` predicate reading a shared
  struct OTHER than the cond struct: the predicate is re-evaluated under ONLY the cond
  mutex (pthread_cond_wait releases only that one lock), so a foreign shared read is an
  unsynchronized cross-thread race. FIX (checker-only reject, `cond_pred_foreign_shared`):
  reject a predicate that reads any shared root whose ROOT IDENT differs from the cond
  var's — instance-precise, so a 2nd INSTANCE of the SAME shared type is also caught,
  while the legit pointer-param `b`/`b.field` case passes. Locking the 2nd struct inside
  the cond mutex is not an option (AB-BA deadlock + cond_wait sleeps holding the extra
  lock); the textbook rule is "a condvar predicate reads only the cond mutex's own
  state." Over-rejects nothing (all 30 existing condvar predicates read only their cond
  struct). Tests: `tests/zer_fail/cond_wait_foreign_shared.zer`,
  `tests/zer_fail/cond_wait_foreign_same_type.zer` (instance-precise),
  `tests/zer/cond_wait_same_struct_multifield.zer` (the prescribed safe restructure).
- **[FIXED BUG-756 — B4]** `@once` loser now WAITS for the winner. 3-state flag
  (0/1/2): winner CAS 0→1 → body → store 2 (RELEASE) at the join block; loser spins
  on `!= 2` (ACQUIRE) → skip. The ACQUIRE/RELEASE pairing means a loser never reads
  the half-constructed published state. **The "BLOCKER" was illusory** — naming the
  function-scope flag `_zer_once_<bb_skip_id>` (the @once's `false_block` id, unique
  per @once, available as `inst->false_block` at the branch AND `bb->id` at the join)
  makes it reachable from both emission sites with ZERO IR change. Control flow
  (return/break/continue/goto) that exits a @once body is now banned (`Checker.in_once`)
  — an early exit would skip the done-publish and hang losers. Tests:
  `tests/zer/once_loser_wait.zer`, `tests/zer_fail/once_control_flow.zer`. Freestanding
  path unchanged (single-core, loser does not wait).
- **[FIXED BUG-749 — B5]** defer-body shared access: `emit_defer_stmt`'s
  `NODE_EXPR_STMT` now lock-wraps the deferred shared access
  (`emit_defer_shared_root` + `emit_shared_lock_mode`/`unlock`, recursive mutex).
  Narrow residue: shared access inside an `if`/`for`/`while` *condition* within a
  defer body is not yet wrapped (rare).
- Remaining-fix direction: one lock-scope-walker redesign covering all roots +
  switch-arm/cond bodies + the `@once` loser-wait. Deadlock-sensitive (multi-root
  WRITE locks must keep the same-statement-multi-shared-type ban; READ locks
  compose).

**Axis C — per-function CFG lattice (lifetime/temporal).**
- **[FIXED BUG-743]** `ir_merge_states` now merges `threads[]` (union by name,
  `joined` = AND over preds that contain it) and the convergence check compares
  per-thread join state — the false-green scoped-spawn stack-UAF (a `ThreadHandle`
  created on a non-`first_live` predecessor, silently dropped at the merge) is
  closed. Tests: `tests/zer_fail/spawn_branch_no_join.zer` (+ 2 positives).
- **[FIXED BUG-745]** stack-local pointer (incl. `*shared T`) to a fire-and-forget
  spawn now rejected (the lifetime arm — `spawn_arg_is_stack_derived`).
- **[OPEN]** remaining lifetime/temporal holes: free-after-publish across threads;
  detached grandchild outliving a scoped join; block-scoped Barrier/Semaphore
  outlived by a function-scoped join; async task struct raced by concurrent polls;
  block-scoped Barrier/Semaphore outlived by a function-scoped join. Fix
  direction: block-scope carrier lifetime tracking + publication lifetime-arm +
  Handle-gen runtime trap for freeable carrier payloads.
- **[FIXED BUG-751]** scoped-borrow exclusivity (the C3-investigation residual):
  a parent WRITE to a non-shared local lent via `&x` to a scoped spawn, between
  `spawn` and `th.join()`, now errors (the thread has exclusive `&mut`-style
  access). Symbol `is_borrowed_by_thread`/`th_borrows_name`; write-only + linear
  (statement-order) approximation. **[OPEN residue]** parent READ during the
  borrow window (a tighter case) and cross-block / aliased-pointer writes — a CFG
  version in zercheck_ir (borrow set merged like `threads[]`) would be exact.
- **General class warning (still OPEN):** a systematic "merged vs first_live-only"
  audit of EVERY `IRPathState` field — the Axis-C bug class is "a second lattice
  family not added to the merge". `threads[]` is now merged; verify no other field
  (e.g. future per-path lifetime tags) repeats the omission.

**Axis D — boundary/runtime concurrency-capture.** Concurrency entering with no
visible ZER node:
- **[FLOOR — D1, RECLASSIFIED 2026-06-21b — NOT a hole, NOT in scope]** FFI/cinclude:
  a ZER ptr/funcptr handed to a bodyless extern that `pthread_create`s internally.
  This is **C-domain behavior**, outside ZER's safety boundary by the same logic
  that makes ZER silent on *any* C-internal behavior (a `cinclude`d C function that
  double-frees, stashes, or over-writes your pointer is equally invisible —
  "C code is outside ZER's safety boundary", CLAUDE.md). It is NOT a
  program-consequence leak: the program-consequence claim is scoped to uses **in
  ZER source**; a C lib threading your pointer is a use **in C source**. It belongs
  with **deadlock and hardware-consequence as a named FLOOR**, not an OPEN hole. The
  earlier audit framing ("the one place the program-consequence claim leaks") was
  wrong — it was never *in* the claim.
  - **A VERIFICATION would be the contract-trap CLAUDE.md rejects:** trusting a
    `captures` annotation and claiming safety = accepting an unverifiable claim
    about C behavior = manufacturing FALSE safety. Do NOT build it as a closure.
  - **The safe path already exists today, no annotation needed (document this as
    the recipe):** the only hazard is *lifetime* (the C thread outlives the data),
    and ZER already lets you express data that outlives the thread — hand a
    capturing extern a **global**, a **global instance of a `shared struct`**, or
    **Pool/Slab-allocated** data (never `&stack_local`, the same discipline as
    "can't return `&local`"). That eliminates the cross-thread UAF with existing
    primitives. The remaining *mutual-exclusion* half (the C thread and ZER both
    touching the data) follows the **C library's own threading contract** — ZER's
    auto-lock does NOT reach into the C thread (C never acquires `_zer_mtx`); use
    `@atomic_*`/`*opaque` or the lib's locking, same as any FFI. This mirrors how
    ZER treats hardware: it hands you safe building blocks (lifetimes you control,
    `mmio` ranges), you apply them at the boundary.
  - **Optional future polish (NOT a closure):** a `captures`/`threads` marker on a
    cinclude param could be added purely as **audit visibility** (the asm
    `safety:`-string style) — it would let ZER enforce the in-scope *lifetime*
    consequence ("if you declare this param captured, the ZER pointer must be
    long-lived"). But it can't be inferred (no ZER body), doesn't protect the
    unaware user, and must NEVER be sold as "verified". Deferred unless users ask.
- **[FIXED BUG-748 — D2]** the compiler-emitted `@probe` runtime
  `_zer_in_probe`/`_zer_probe_jmp` are now `__thread` (were process-global statics
  raced by threads → cross-thread longjmp corruption). **[OPEN]** residue: other
  emitter-runtime statics (`@once` state — see B4, async task struct,
  shared-lazy-init CAS) want the same `__thread`-vs-`static` audit; a ZER fn
  installed as a POSIX signal handler via cinclude self-deadlocks its non-recursive
  mutex (the shared mutex is recursive, but a signal-handler re-entry on the SAME
  thread mid-critical-section is a separate hazard).

**The closure (ends all four):** put the invariant on the DATA — an inferred,
non-strippable `shared` taint (Model 4 extension of the `volatile` machinery)
propagated through `&`/casts/slices; auto-lock covers every sub-statement; the CFG
lattice merges every tracked-state family; the cinclude boundary is the named
FLOOR (D1 — safe via long-lived data, not a verification target); all frozen by a
CI audit gate so it cannot regress. `shared` on
scalars/pointers is INFERRED (keep/escape/provenance family) — the dumb user never
writes it; only the `shared struct` keyword stays, and it is demanded by an error.
This is the auto-inferred equivalent of Rust's `Send`/`Sync`+`'static` (Rust is the
existence proof it's achievable).

**Out of scope (named floor):** deadlock/livelock — undecidable, same boundary Rust
holds (a Rust `Mutex` AB-BA deadlock compiles fine); **D1 cinclude thread-capture**
— C-domain behavior, outside ZER's safety boundary (the safe path exists today:
hand capturing externs long-lived data — global / global `shared struct` instance /
Pool/Slab — never `&stack_local`; cross-C-thread mutual exclusion follows the C
lib's contract). ZER's per-statement auto-lock
already kills the lock-ordering deadlock class by construction.

**Status (2026-06-21b):** implementation phase 2 BEGUN — **7 of the ~25 holes
CLOSED** (Axis C `ir_merge_states` + A1 + C2 + A3 + A4 + D2 + B5; BUG-743..749),
each verified by the full ZER suite (769/769) + C unit tests, each with a
regression negative test in `tests/zer_fail/` that runs in `make check` (the gate).
The CLOSED set covers the reachability holes (volatile-RMW, Arena, slice/opaque
dispatch), the spawn lifetime arm, the runtime race (`@probe`), **the A6-full
atomic-cell inclusion taint (BUG-752)**, and **the ENTIRE Axis B lock-completeness
family — B1 multi-root (BUG-753), B2 union copy-out (BUG-754), B3 cond_wait
foreign-shared reject (BUG-755), B4 @once loser-wait (BUG-756), B5 defer (BUG-749)**.
The B1–B4 "deadlock-sensitive lock-scope-walker redesign" turned out NOT to need a
single global redesign: each was closed in place without ever holding two shared
locks at once (B1 read-locks-compose, B2 copy-out-under-the-one-lock, B3 reject not
lock, B4 a private once-flag not a struct mutex). Also FIXED: **A5** threadlocal
`&`-escape (BUG-757), the **A6 micro-residuals** (BUG-758, struct-field reads +
`&s.f` launder), and the **scoped-borrow read-side** (BUG-759, same-block).
REMAINING (annotated `[OPEN]` above) is now ONLY the **scoped-borrow CROSS-BLOCK**
case (a zercheck_ir borrow-set merge — subsystem-scale, lower-value). **D1 is a
FLOOR, not a remaining build**
(C-domain; safe path exists). And the still-unprobed residue (FFI callback tables;
other emitter-runtime statics; cross-module spawn/extern; `NODE_STRUCT_INIT` global
read in a spawn body).

---

### VERIFIED STILL OPEN 2026-08-01 — reproduced, with the exact mechanism

Probed against the 2026-07-21 `zerc` (i.e. AFTER the 12-branch merge-back). The entry
above is **accurate, not stale** — the cross-block scoped-borrow hole is live. Three
shapes, all compiled clean or correctly rejected as marked:

| Shape | Result |
|---|---|
| `join` in ONE arm of an `if`, access after the merge | **ACCEPTED — real data race** |
| `join` inside a `while` body, access after the loop | **ACCEPTED — real data race** |
| `join` on BOTH arms, access after (legal) | ACCEPTED — correct; this is the CONTROL |

```zer
struct Work { u32 x; }
void worker(*Work w) { w.x = 1; }
u32 flag_in; // NOTE: condition must NOT mention `work` (see gotcha)
u32 main() {
    Work work;
    ThreadHandle th = spawn worker(&work);
    if (flag_in == 99) {
        th.join(); // borrow cleared on ONE path only
    }
    work.x = 2; // other path: the thread is STILL running -> RACE
    th.join();
    return 0;
}
```

**Root cause — the borrow was never migrated to the CFG.** Handles moved to
`zercheck_ir.c` with a real `ir_merge_states`; the scoped-spawn borrow did not. It is
still a single boolean on the `Symbol`, mutated by a linear AST walk:

| Site | Code |
|---|---|
| `checker.c:14087` | `vs->is_borrowed_by_thread = true;` — set at `spawn` |
| `checker.c:5614` | `bv->is_borrowed_by_thread = false;` — cleared at `join`, **unconditionally** |
| `checker.c:3558` | read check |
| `checker.c:3970` | write check |

Walking into an `if` body clears the flag; walking out does not restore it. Everything
after the merge therefore sees "not borrowed", including the path on which the thread is
still live. There is no `preds[]`, so the state cannot be path-dependent — this is the
AST-cannot-represent-a-merge problem, still resident in the concurrency layer.

Note the `borrow` identifiers in `zercheck_ir.c` are `ret_is_borrow` — return-value
analysis for leak suppression, an unrelated concern. Grepping "borrow" there does **not**
find this.

**GOTCHA for whoever writes the regression test.** A naive test accidentally PASSES.
If the branch condition mentions the borrowed variable (`if (work.x == 0)`), the read
*inside the condition* trips the checker at `checker.c:3558` before the interesting
statement is reached, and the test looks like it rejects correctly. **The condition must
reference an unrelated variable** for the hole to surface. Three of four first-attempt
probes were masked this way.

**Fix (unchanged from the entry above): a borrow-set merge in `zercheck_ir.c`.** Move
the borrow from a `Symbol` boolean to a per-block set, union at joins, same shape as the
`threads[]` merge that fixed the false-green scoped-spawn stack-UAF. Conservative
direction at a join is UNION (still-borrowed wins), matching every other class's
fail-closed discipline.

**Why the obvious fix is wrong:** "never clear the flag" makes the CONTROL case above
(join on both arms, then access) a false positive, rejecting correct code. The
merge must be path-aware, not merely sticky.

**Tripwire tests to add (none exist today):**

```
tests/zer_fail/borrow_join_one_arm.zer must FAIL to compile — currently passes
tests/zer_fail/borrow_join_in_loop.zer must FAIL to compile — currently passes
tests/zer/borrow_join_both_arms.zer must COMPILE — pins against over-correction
```

The third is not optional: without it the sticky-flag "fix" would ship and nothing
would catch the over-rejection.

### RE-VERIFIED 2026-08-03 (HEAD `30a79744`, clean rebuild) — STILL OPEN, and the reproduction above is now MASKED

Re-ran the recorded shapes against a clean rebuild of current main. **The hole is still
live, and the recorded reproduction no longer exhibits it** — anyone re-running the old
repro will wrongly conclude this entry is fixed.

MEASURED per shape on `30a79744` (a first pass said "all three rejected for the wrong
reason"; that was too strong — one is now rejected CORRECTLY, which is a real gain from
the 2026-08-02/03 scoped-borrow work and should not be undone):

| Shape | Result |
|---|---|
| write in a branch, condition references an UNRELATED variable | REJECTED by the **borrow** rule — correct, and newly so |
| conditional join, no early return | REJECTED by the **ThreadHandle leak** rule — masked |
| conditional join **+ early return** | **ACCEPTED — this is the live hole** |

What actually happens now: a *conditionally* joined ThreadHandle trips the
join-on-all-paths LEAK check —

```
zercheck: ThreadHandle 'th' not joined before function exit — add th.join() or detach explicitly
```

— which fires *before* the borrow check is ever consulted. So the leak rule masks the
race rule. This is a SECOND instance of this entry's own test-design gotcha (the first
was: a branch condition that mentions the borrowed variable trips the read check first).
**The lesson generalizes: on this checker, a negative test proves nothing until you read
the actual diagnostic — several rules can reject the same program, and only one of them
is the one under test.**

**CORRECTED reproduction — satisfies the join discipline and still races:**

```zer
struct Work { u32 x; }
u32 flag_in = 0;
u32 worker(*Work w) { w.x = 1; return 0; }
u32 main() {
    Work work;
    ThreadHandle th = spawn worker(&work);
    if (flag_in == 99) { th.join(); return 0; }   // path A: joined, then exits
    work.x = 2;          // path B: thread STILL RUNNING -> REAL DATA RACE
    th.join();
    return 0;
}
```

`ACCEPTED` — compiles clean. The race is visible in the emitted C:

```c
462:    pthread_join(th, NULL);      /* path A, inside the if */
466:    _zer_t4 = work.x = 2;        /* path B, thread still running */
467:    pthread_join(th, NULL);
```

The early `return` is what makes it legal to the leak rule (path A is joined and exits;
path B joins at the end) while leaving the borrow live across the merge on path B.

**Controls that still behave correctly** (so the fix must preserve both):
- same-block write while borrowed -> REJECTED with the right message
  (`cannot write to 'work' while it is borrowed by a scoped spawn`)
- join on every path *before* the access -> ACCEPTED

**Revised tripwire set** (supersedes the three above — the first two would now pass for
the wrong reason and must be rewritten to this shape):

```
tests/zer_fail/borrow_join_one_arm_return.zer   must FAIL — currently ACCEPTED
tests/zer/borrow_join_all_paths.zer             must COMPILE — pins against over-correction
tests/zer_fail/borrow_same_block.zer            must FAIL — pins the rule still fires at all
```

The third is new and matters: it is the only one that would catch the borrow check being
disabled outright while the leak check silently absorbs the negative tests.

**Consequence for the soundness theorem:** this class currently fails OPEN at a CFG
join. An analysis that fails open cannot discharge a forward-simulation obligation —
the abstract state does not over-approximate the concrete one — so `checker_sound` is
not merely unproven for scoped borrows, it would be **false**. Same standing as the
provenance-at-indirect-call hole recorded elsewhere: these are prerequisites for the
Lean port, not independent bugs.

## Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.

When a `tests/zer_gaps/` reproducer is fixed, move it to
`tests/zer_fail/` so it becomes a permanent regression guard.
