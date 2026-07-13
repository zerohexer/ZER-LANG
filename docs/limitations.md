# ZER Compiler — Known Limitations

Living document of known compiler limitations, audit findings, and deferred fixes.
Entries removed once fixed.

---

## OPEN — unmerged audit fixes across 12 parallel `claude/*` branches (2026-07-13) — TASK TRACKER

**READ THIS BEFORE DOING ANY NEW AUDIT / BUG-HUNT.** Most holes are ALREADY FOUND and
FIXED on a branch — don't re-derive them. Twelve parallel `claude/*` audit sessions each
found + fixed overlapping soundness / miscompile / crash holes. **NONE are merged to
main** (verified 2026-07-13: `git cherry -v main <branch>` is all `+`; every sampled
regression-test file is absent from main; signature helpers absent). The heavy overlap is
AMONG the branches (several bugs found 3–4×), NOT with main. **41 unique fixes** after
dedup — **5 landed (uN/iN trio #17/#18/#19 + #20 `&&`/`||` short-circuit + #21 optional-None), 36 remaining.**

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

### A. Memory safety — UAF / double-free / move (🔴; absent in main)
| # | Fix | Proper source (sha) | Files |
|---|---|---|---|
| 1 | subslice of heap slice inherits base `alloc_id` (view UAF/DF) | 8d9514f3 | zercheck_ir.c |
| 2 | cross-fn slice-param free tracked (`frees_param` += TYPE_SLICE) | bf29ffdc | zercheck_ir.c |
| 3 | reject `free()` of non-heap (stack/arena/local-array) slice | bf29ffdc | checker.c |
| 4 | Level-B: block 2nd free under complementary guards (stale `free_block`) | 59a968cb (A1) or f40ca06b (F2) | zercheck_ir.c |
| 5 | Level-B: immutability gate defeated by reassigned intermediate copy | 66332d39 (#2) | zercheck_ir.c |
| 6 | struct value-copy preserves compound handle (Pattern-4 replicate) | 59a968cb (A3) | zercheck_ir.c |
| 7 | move-alias via `&arr[i]` / `&b.field` / spawn-arg | 582920db (#4) | zercheck_ir.c |

### B. Escape / dangling-pointer sinks (🔴; #8 base helper partial in main)
| # | Fix | Proper source (sha) | Files |
|---|---|---|---|
| 8 | optional/array pointer-carrier escape | 5a6889df (`escape_type_carries_ref`) **+ TYPE_ARRAY arm from** 586507fb (D1) | checker.c |
| 9 | reassign `p = &local[i]/.f` escapes frame | 66332d39 (#1, `addr_of_is_local_derived`) | checker.c |
| 10 | assignment-form slice-of-local (`r = arr; return r`) | bf29ffdc (773) | checker.c |
| 11 | arena direct-assign launder (`g = arena.alloc_slice`) | 85cc109e (D) + 87a01415 (#4) | checker.c |
| 12 | Ring.push / spawn of by-value aggregate carrying ptr-to-local | a3e1f66c | checker.c |

### C. VRP / bounds — silent OOB (🔴; absent)
| # | Fix | Proper source (sha) | Files |
|---|---|---|---|
| 13 | VRP guard-narrowing leaks past switch/if-capture/orelse/loop join | 586507fb (A+B, JOIN) | checker.c |
| 14 | `find_return_range` do-while body + guard-body over-credit | f40ca06b (F1) **+** 59a968cb (A2) — different arms | checker.c |
| 15 | VarRange map reset per-function (cross-fn name-key leak) | f40ca06b (F3) | checker.c |
| 16 | defer body keeps array bounds-guard (trapping mode) | 9edc49b8 (E, `guard_traps`) | emitter.c/.h |

### D. uN/iN + miscompiles (🟠)
**✅ DONE (merged to main): #17 assign/compound-assign mask, #18 `@truncate` mask
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
make check 919/0.
| # | Fix | Proper source (sha) | Files | Main |
|---|---|---|---|---|
| 22 | optional struct-field designated-init keeps value | fb3315f2 (3 paths, shared helper) | emitter.c | absent |
| 23 | `@saturate` unsigned from ≥2^63 source | a3e1f66c | emitter.c | absent |
| 24 | signed comptime return sign-extended | a3e1f66c | checker.c | absent |
| 25 | float literal digit-group `_` not truncated | f40ca06b (F8-parser) | parser.c | absent |

### E. Concurrency (🔴/🟠; absent)
| # | Fix | Proper source (sha) | Files |
|---|---|---|---|
| 26 | spawn data-race scanner recurses cast/slice/struct-init/orelse | 9edc49b8 (B) or bf29ffdc (772) | checker.c |
| 27 | multi-root shared lock: field-projection + intrinsic-wrapped reads | 586507fb (C-F4) **+** 19471462 (B1 intrinsic) | ir_lower.c |
| 28 | defer-body shared read emits the lock | 2c7645b9 | emitter.c |
| 29 | shared-cond mutex unlock on orelse-return/break (no deadlock) | 586507fb (C-F3) | ir_lower.c |
| 30 | `IR_AWAIT` keeps resume pred edge (UAF-across-await visible) | 586507fb (1-line) | ir.c |
| 31 | union variant write via `*Union` updates `_tag` | 586507fb | emitter.c |

### F. Parser / crashes / robustness
| # | Fix | Proper source (sha) | Files | Main |
|---|---|---|---|---|
| 32 | parser DoS: `parse_type` + prefix-modifier depth guard | a8968db0 (A7-13) | parser.c | ⚠️ partial (expr guard only) |
| 33 | `type_name` 256-byte buffer overflow → SIGSEGV | 59a968cb (A5, clamping `tn_append`) | types.c | absent |
| 34 | `(*ptr & mask)` parse regression (breaks QEMU firmware) | ce9af8cb (A7-12) | parser.c | absent |
| 35 | defer + auto-guard compiler abort (`N pending defers`) | 66332d39 (#6, `emit_pending_ir_defers`) | emitter.c/.h | absent |

### G. Bare-metal / ISR / qualifier (absent)
| # | Fix | Proper source (sha) | Files |
|---|---|---|---|
| 36 | `@critical` `"memory"` clobber on ARM/AVR/RISC-V | a8968db0 (A7-6) | emitter.c |
| 37 | baremetal `@cpu_syscall/sysret/iret/hypercall` `#else #error` | 582920db (#5) | emitter.c preamble |
| 38 | `@inttoptr` aggregate span/alignment (drop `type_width`=0) | 5a6889df (F4) | checker.c, emitter.c |
| 39 | ISR ban: `@cond_wait`/`@barrier_wait`/`@sem_acquire` | 1fdaaffe | checker.c |
| 40 | ISR ban: universal `alloc(T,n)`/`free(slice)` in ISR/@critical | 66332d39 (#3) | checker.c |
| 41 | `@container` const-strip check (last cast form) | 582920db (#2) | checker.c |

### Conflict groups (apply per-family, re-verify after each)
- checker.c escape sinks: #8, 9, 10, 11, 12 (same region)
- zercheck_ir.c Level-B: #4, 5 (same free_block/guard machinery)
- ir_lower.c shared-lock: #27, 29 (cond-lock helpers)
- emitter.c uN/iN: #17, 18, 19 (same mask sites this session touched)
- emitter.c defer: #16, 35 (`emit_defer_stmt` / pending-defer)
- checker.c VRP: #13, 14, 15 (var_range save/restore + return-range)

**Next up (start here):** ~~uN/iN trio #17/#18/#19, #20 short-circuit, #21 optional-None~~
✅ landed 2026-07-13/14. Remaining highest-value: the crashes — #33 `type_name` 256-buffer
overflow → SIGSEGV, #34 `(*ptr & mask)` parse regression (breaks QEMU firmware) — plus #22
optional-field designated-init and the memory-safety cluster (§A/§B). All verified absent
from main.

---

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
  🟡 silent Pool-slot LEAK every call. File: `ir_lower.c`. CAUSE: `NODE_GOTO` eagerly fires
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
  🔴 data race. File: `checker.c`, `scan_unsafe_global_access` NODE_CALL handler. CAUSE:
  `worker(){ run_n(do_increment, n); }` + `spawn worker()` raced a global via the indirect
  call `cb()` inside `run_n`; the scan descended the direct callee but not funcptr args. FIX:
  follow every `NODE_IDENT` argument that resolves to a function symbol, descending into its
  body the same way as the direct callee; single shared `_scan_depth` counter (cap 32).
  Tripwire `tests/zer_fail/spawn_funcptr_global_race.zer`. CLASS: form-coverage / missing-site.

- **[T1.3] 11ct36 `ecd6f65d` — BH-18 #14: `@size()`/`@bitcast()` with no type arg → invalid C.**
  🟡 invalid C. File: `checker.c`. CAUSE: the arity `type_arg` gate let the zero-type case
  through. FIX: restructure the arity block — make family identification unconditional, then
  SPLIT "requires a type argument" from "expects N args after type"; preserve the
  `@size(NamedType)` parse path (BUG-316) via `size_named_path`. Tripwires
  `tests/zer_fail/intrinsic_{no_type_arg,bitcast_no_type}.zer`. CLASS: arity / missing-site.

- **[T1.4] a5erj3 `c2eb1652` — typedef-wrapped pointer destructor blinds FuncSummary → silent
  UAF + double-free.** 🔴 UAF. File: `zercheck_ir.c`, FuncSummary builder. CAUSE: gated
  param-FREED observation on the SYNTACTIC `TypeNode` kind (`tnode->kind == TYNODE_HANDLE ||
  TYNODE_POINTER`); a `typedef *T TPtr` param is `TYNODE_NAMED`, so the gate silently dropped
  it → `frees_param[i]` never set. Distinct-unwrap class (BUG-409/GAP-F) on the TypeNode axis.
  FIX: gate on the IR local's RESOLVED `Type *` via
  `type_unwrap_distinct(func->locals[plocal].type)` and check `TYPE_POINTER/TYPE_HANDLE/
  TYPE_OPAQUE` — TYNODE form irrelevant. Mirror of the apply-side at `zercheck_ir.c:3974-3985`.
  Add the branch's 3 `tools/type_dispatch_baseline.txt` entries for the `pt_eff->kind` reads.
  Tripwire `tests/zer_fail/typedef_ptr_funcsummary_uaf.zer`. CLASS: distinct-unwrap / missing-
  site. 🚩 FLAG #2 (see below).

- **[T1.5] ongou2 `bbbdf95c` (hole 1) — assignment-form call-launder defeats escape check (3
  sinks).** 🔴 UAF / dangling-global. File: `checker.c` ~4170 (the NODE_ASSIGN re-derivation
  block). CAUSE: `*Box p = &g; p = launder(&local_box); spawn worker(p);` compiled clean — the
  assignment path was missing the parallel of "Case D" (BUG-770) that the var-decl handler has
  at `checker.c:10027`. FIX: add the same arm to the assignment path, predicate
  `call_has_local_derived_arg` (the one 4 existing sinks use), type-gated on
  `type_can_carry_pointer` so scalar reductions (`acc = op(acc, data[i])`) aren't false-
  positived. Closes spawn / global-store / return sinks for the assignment-launder shape
  (pointer + slice). Tripwires `tests/zer_fail/assign_launder_{global,slice,spawn}.zer`. CLASS:
  per-sink escape patchwork / missing-sink.

- **[T1.6] ongou2 `bbbdf95c` (hole 2) — switch-default capture escapes ptr-to-local to a
  global (BH-18 #6 SIBLING).** 🔴 UAF. File: `checker.c`, switch-arm capture handler (sibling
  of BH-18 #6 at `checker.c ~10459`). CAUSE: `switch(m){ default => |*v| { g = v; } }`
  accepted while the `if |*v|` sibling was rejected; the switch-arm capture-desugar didn't
  inherit the matched value's region. FIX: when the capture is a pointer (`|*v|`) AND the
  switch root resolves to a function-local, mark the capture `is_local_derived` (same rule as
  BH-18 #6). CERTIFIED by `capture_lattice.v` "capture inherits the payload's region".
  Tripwire `tests/zer_fail/switch_default_capture_escape.zer`. CLASS: per-sink capture
  patchwork / missing-sink.

### TIER 2 — AST→IR DRIFT pair. Take, THEN re-run the drift audit grep. (2 holes)

- **[T2.1] a5erj3 `9e47b9c4` (part c) — `static u32 v = @ctz(16);` emits invalid C.** 🟡
  invalid C. File: `emitter.c`, `@ctz`/`@clz` IR emitter. CAUSE: the IR path ALWAYS emitted
  the statement-expression `({...})` zero-guard wrapper; the AST path already had a conditional
  form. GCC rejects a stmt-expr in a static-local initializer ("initializer element is not
  constant"). FIX: `@ctz`/`@clz` IR emitter uses the conditional form when the arg has no side
  effects (safe to double-evaluate); keep the stmt-expr for side-effecting args.

- **[T2.2] ongou2 `bbbdf95c` (hole 3) — IR auto-guards gate missing `IR_AWAIT` and `IR_NOP`.**
  🔴 silent corruption (dropped bounds guard). File: `emitter.c`. CAUSE: `await arr[i]` /
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
  shared-type walkers.** 🔴 data race. Each walker walked to the innermost `NODE_IDENT` and
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
  ⚠️ **CRITICAL OVERLAP — re-derive, do NOT cherry-pick:** main rewrote walkers 2/3/4 AFTER
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

- ✅ **FLAG #1 — AUDITED CLEAN (2026-07-01), no remaining drift.** Full AST→IR emission-diff
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
- ✅ **FLAG #2 — RESOLVED (2026-07-01).** `tools/audit_type_dispatch.sh` now ALSO scans the
  syntactic `TypeNode` axis (`->kind == TYNODE_` / `!= TYNODE_`); the 12 legitimate existing
  sites are baselined and a NEW TYNODE dispatch trips the gate (validated by inject-and-revert).
  The distinct-unwrap class can no longer recur undetected on the TypeNode axis.
- 🚩 **FLAG #3 — RETRACTED (wrong-base assumption).** All hunks applied cleanly onto the
  rewritten walkers.

### STILL OPEN — triaged against current main 2026-07-01 (all confirmed LIVE except where noted)

- ✅ **AU-1 / AU-2 / AU-3 / AU-4 — FIXED 2026-07-01** (see BUGS-FIXED.md): defer LIFO use-after-free;
  deferred `arena.reset()`; nested struct-init escape; direct-assign struct-init escape. All were
  confirmed LIVE by triage, all now reject.
- ✅ **bh18_1b — FIXED 2026-07-01** (see BUGS-FIXED.md): move-struct use-after-move via a
  pre-existing pointer alias. Register the move local when `&a` is taken (flagged `is_move_local`
  so the leak check skips it + its alias) + propagate TRANSFERRED to the alloc_id group at the
  transfer. Tests `tests/zer_fail/move_alias_stale_read.zer` + `tests/zer/move_alias_ok.zer`.
- ✅ **bh18_12 — FIXED 2026-07-01** (see BUGS-FIXED.md): defer fired N× on a same-scope backward
  goto. Fix: per-label `defer_count_at_def`; a backward goto fires only defers registered AFTER
  the label (loop-body defers), leaving pre-label defers pending for the real exit. Forward gotos
  unchanged (base 0 + sesjma guard). Tests `tests/zer/defer_goto_{backward_once,loopbody_periter}.zer`.
- ✅ **AU-5 — FIXED 2026-07-01** (see BUGS-FIXED.md): the ISR/@critical/async context-restriction
  scan (`scan_func_props`) was blind to a function passed as a funcptr argument and invoked
  indirectly. Per primitives-data-races.md §2.3/§5.7 (context restrictions are Definition-A
  VERIFIED), closed by propagating a funcptr-arg function's props to the parent (mirrors BH-18 #8).
  Tests `tests/zer_fail/isr_alloc_via_funcptr.zer` + `tests/zer/funcptr_alloc_non_isr_ok.zer`.
- ⏸️ **AU-6** (privileged `@cpu_*` have no call-site context check) — **DEFERRED to the Option E
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

Audit of EVERY safety class against the MAX-ORACLE STANDARD (CLAUDE.md): a class is
"at maximum" iff it is (a) SOUND (zero under-rejection — never accepts unsafe), (b)
FLEXIBLE (minimal over-rejection), AND (c) backed by a MAX oracle (a Coq/Iris Level-1
spec whose finite-state set is COMPLETE and whose abstraction is the richest sound one,
not a flat/coarse one). This is the INDEX; per-hole detail lives in the linked entries
below. Verdict tally: ~14 live under-rejections (6 are 🔴 memory-corruption), ~4 real
over-rejections, and MOST classes are coarse-oracle or no-oracle — only 3 are genuinely
AT-MAX. Two clusters (type/provenance fully audited 2026-06-23; the other five audited
from this ledger after the parallel workflow rate-limited).

### NOT-SOUND — under-rejects (accepts unsafe). The urgent tier (close before precision work).

**Memory-corruption (🔴 UAF/OOB):**
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

**Data-race (🟠 concurrency):** shared-struct multi-access hidden in a cast/intrinsic/
index/orelse SUBEXPRESSION evades the same-statement deadlock/lock check (#7, ~1881); the
`spawn` data-race scan is blind to function-pointer indirection (#8, ~1925); shared access
in an `await` CONDITION is not locked (D02 false-negative, #9, ~1963); the one remaining
**cross-block scoped-borrow** hole (spawn + access in different CFG blocks — needs a
zercheck_ir borrow-set merge; concurrency entry ~2303).

**Miscompile (🟡 — unsound OUTPUT, not a UAF):** value-returning `async` never finalizes
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

1. **The two proof-backed near-free 🔴 wins:** `@bitcast` #3 (wire the already-VST-verified
   `zer_bitcast_operand_valid`, one call site + tripwire) and `@pun` #4 (the size guard).
2. The stateful 🔴 holes: #1 (decl-site alias), #2 (wire `vrp_ir.c`), #5 (emitter path),
   #6 (capture inherits region — oracle ready).
3. The 🟠 races (#7/#8/#9) + the cross-block scoped-borrow.
4. The 🟡 miscompiles.
5. THEN precision: implement `join_lattice.v` (disjunctive return) and `disjoint_lattice.v`
   (aliased mutation), and write the missing/richer oracles (relational bounds, the null
   flow-state lattice, distinct-unwrap, the no-oracle classes).

---

## OPEN — clang-wasi run pipeline: compiler bundling (pipeline DONE; bundle TBD)

**Status (2026-06-22): the fully-WASM run pipeline WORKS end-to-end — only the
compiler-bundling is unresolved.** Goal: VS Code "run" produces a `.wasm` (not a
native `.exe`) → no per-run Defender scan. Pipeline: `zerc.wasm` (ZER→C, done) →
`clang --target=wasm32-wasi` (C→wasm) → run in node-WASI.

DONE + committed:
- **Emitter `__wasi__` gate** (emitter.c ~4807-5066, commit 75ce5cfc): POSIX
  preamble (pthread/sched/signal) gated `!defined(__wasi__)` so emitted C compiles
  to wasm32-wasi; hosted unaffected; `make check` green.
- **`-WASI` VSIX scaffolding** (commit ceb3d543): `editors/vscode-WASI/`
  (`zerc-language-wasi`; `lsp/zerc-cli.js` → clang-wasi → app.wasm → node-WASI via
  `lsp/wasi-run.mjs`), `Dockerfile.vsix-WASI`, `make docker-vsix-wasi`.
- **PROVEN in the real build** — 3 in-container smoke tests pass: ZER→wasm→node-WASI
  prints + exits 0; double-free rejected (zercheck_ir); OOB slice read TRAPS at
  runtime. `wasm-ld --wrap=malloc` (Level-4 interception) confirmed (LLVM 9+/D62380).

THE OPEN ISSUE — bundling a C→wasm compiler that is small AND keeps `--wrap`:
- **native wasi-sdk clang**: full `--wrap` ✓ but **~1.4 GB/platform** (Windows
  clang.exe is a fat static LLVM; `llvm-strip --strip-debug` barely helped — it's
  code, not debug). vsce can't package ~2 GB. UNSHIPPABLE.
- **zig cc**: small (341 MB) but **rejects `-Wl,--wrap`** (verified: "unsupported
  linker arg"). Would need emit with `track_cptrs=0` (no `__wrap` machinery) →
  pure-ZER stays 100% compile-time safe; cinclude'd C loses the ~2% runtime net.
- **clang.wasm (CHOSEN)**: wasm-hosted clang+lld via **Wasmer** ("clang-in-browser",
  `@wasmer/sdk` ~15 MB, runs in node too). Recent clang+lld → `--wrap` ✓, smallest,
  zero native compiler (kills install-time scans too). API:
  `Wasmer.fromRegistry('clang/clang')` + a `Directory` (writeFile in.c / readFile
  out.wasm) + `entrypoint.run({args:['/p/x.c','-o','/p/x.wasm','-target','wasm32-wasi'],
  mount:{'/p':dir}})`.

**INVESTIGATION RESULT (2026-06-22) — clang.wasm via Wasmer is BLOCKED in node.**
Validated thoroughly: `@wasmer/sdk/node` (15 MB, runtime wasm INLINED → no CDN fetch
needed; the earlier "fetch failed" was the BROWSER entry). `Wasmer.fromRegistry('clang/clang')`
→ `clang-16` + `wasm-ld` (LLVM 16, so `--wrap` *should* work). clang-16 **COMPILES**
(cc1 runs in-process: `Target: wasm32-unknown-wasi`, sysroot mounted at `/sysroot`,
resource dir `/lib/clang/16` ✓) but **CANNOT LINK**: clang's spawn of the `wasm-ld`
subprocess fails with `clang-16: error: linker command failed with exit code 45` —
even for plain `int main(){return 7;}` (so it is NOT a `--wrap` problem; `--wrap` was
never reached). Worse, the WASIX runtime is **FLAKY in node** — ~4 of 5 invocations
HANG (node exits 13, "unfinished top-level await"). Root cause: the `clang/clang`
package targets the full **Wasmer runtime** (WASIX process-spawn + threads);
`@wasmer/sdk`-in-node lacks reliable WASIX subprocess spawning, which a clang→wasm-ld
toolchain fundamentally needs. A two-step `clang -c` + direct `wasm-ld` run would dodge
the spawn, but the per-run hang/flakiness makes it unusable for an in-editor tool
regardless. clang.wasm would need a real WASIX-capable host (e.g. native `wasmtime` —
which re-introduces a native binary). **NOT a clean node/VSIX bundle today.**

**Net: the only WORKING small option is `zig cc`** (341 MB, drops `--wrap` →
`track_cptrs=0`: pure-ZER stays 100% compile-time-safe, cinclude'd C loses the ~2%
runtime malloc net). Native clang is correct-but-unshippable (~1.4 GB). The proven
pipeline + the `-WASI` scaffolding + the smoke-test contract are committed regardless.

**clang.wasm integration plan (only if revisited via a WASIX-capable host):**
1. Pre-fetch (network) the `clang/clang` `.webc` + the `@wasmer/sdk` WASIX runtime
   wasm, BUNDLE them in the VSIX, and configure `@wasmer/sdk` to load them OFFLINE
   (no `fromRegistry` fetch at runtime — a plain `docker run` hit `init()`/registry
   "fetch failed" with github/npm reachable, so offline loading is mandatory).
2. Rewire `editors/vscode-WASI/lsp/zerc-cli.js`: replace the native-clang `spawnSync`
   with `@wasmer/sdk` running the bundled clang on the emitted C → `app.wasm`, then
   run via the existing `wasi-run.mjs`.
3. Update `Dockerfile.vsix-WASI`: drop the wasi-sdk download/trim; bundle
   `@wasmer/sdk` + the clang `.webc`; keep the 3 smoke tests.
4. Verify size (~50-100 MB target) + the 3 smoke tests + that `--wrap` survives.

The native-clang `Dockerfile.vsix-WASI` is committed as the proven-pipeline harness
(its smoke tests are the contract); the clang.wasm swap reuses it. The original GCC
VSIX (`Dockerfile.vsix`, `editors/vscode/`) is UNTOUCHED throughout.

---

## OPEN — WASM CLI: multi-file imports + macOS terminal zerc (LOW–MEDIUM)

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
View get_view([*]u8 s) { View v = { .data = s }; return v; }   // compiles (param wrap — fine)
void caller() { u8[10] buf; View v2 = get_view(buf[0..10]); g_ptr = v2.data; }  // ESCAPES
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
@critical { unlock(); }   // claimed: interrupts NOT re-enabled
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
heap.free(a);   // detected: double-free
```

```zer
Handle(Task) h = get_handle() orelse return;
heap.free(h);
heap.free(h);   // detected: double-free (orelse-IR_ASSIGN variant)
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
@cstr(p, "Hello, world this is too long");  // 29-byte write to 4-byte buf
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
   5: pop    %rax
   6: cli                    ; <-- actual interrupt disable
   7: addl   $0x1,0x0(%rip)  ; critical body
   e: push   %rax
   f: popf                   ; <-- EFLAGS restore
  10: mov    0x0(%rip),%eax
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
    @ptrcast(*MyType, h);  /* validate type via provenance */
    mylib_xyz_terminate_session(h);  /* obscure C name */
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
    defer use_item(h);    // scheduled use at scope exit
    gp.free(h);           // h now FREED in path state
    return;               // defer fires use_item(h) on a FREED handle
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

**How to reproduce (any fresh session):**
```
# build the compiler (native or in Docker; Docker avoids Windows AV):
gcc -O1 -w -I. -o zerc lexer.c parser.c ast.c types.c checker.c emitter.c \
    zercheck.c ir.c ir_lower.c zercheck_ir.c vrp_ir.c zerc_main.c src/safety/*.c
# OR: make zerc

zerc x.zer --run                 # compile+run; prints "zerc: running x" then process exit = main()'s return
zerc x.zer -o x.exe              # compile only (clean compile = no 'error:' line, exit 0)
zerc x.zer --emit-c -o x.c       # inspect the emitted C (proves dropped guards / placeholders)
# To PROVE an out-of-bounds access is real (#2,#4,#5), ASan the emitted C:
zerc x.zer --emit-c -o x.c && gcc -fsanitize=address -g -O0 x.c -o x_asan && ./x_asan
```
A "soundness hole" = clean compile + memory-unsafe/guarantee-violating at
runtime (the crown-jewel class — ZER claims 100% program-consequence
coverage). "miscompile" = clean compile + wrong runtime result. Severity tags:
🔴 critical (memory-unsafe), 🟠 high (race / double-free), 🟡 medium
(miscompile), 🟢 low (false-reject / diagnostic).

| # | Bug | Class | Root-cause area |
|---|---|---|---|
| 1 | move-struct pointer-alias defeats ownership/free tracking (heap UAF + use-after-move + double-consume) | 🔴 soundness | zercheck_ir move/alias |
| 2 | VRP range-narrow scope leak → OOB write | 🔴 soundness | checker.c if-VRP |
| 3 | `@bitcast` forges integer↔pointer | 🔴 soundness | checker `@bitcast` |
| 4 | `@pun(*Struct,*primitive)` skips type_id trap → OOB read | 🔴 soundness | emitter `@pun` guard |
| 5 | fixed-array bare-call index drops bounds check | 🔴 soundness | emitter index single-eval |
| 6 | `if (opt) \|*v\|` capture escapes to a global | 🔴 soundness | capture desugar + escape prov |
| 7 | shared-struct multi-access via cast/intrinsic/index/orelse subexpr evades lock check → race | 🟠 race | checker `collect_shared_types_in_expr` |
| 8 | `spawn` data-race scan blind to funcptr indirection → race | 🟠 race | checker `scan_unsafe_global_access` |
| 9 | shared-struct read in `await` condition unlocked → race | 🟠 race | checker `NODE_AWAIT` handler |
| 10 | value-returning `async` never finalizes state machine | 🟡 miscompile | emitter `IR_RETURN` async |
| 11 | bit-query/byte-swap intrinsics emit `0` in global initializers | 🟡 miscompile | emitter AST `NODE_INTRINSIC` |
| 12 | `defer` + backward `goto` fires wrong count (folds into known #5) | 🟡 miscompile | ir_lower defer/back-edge |
| 13 | nested inline designated initializer false-reject | 🟢 false-reject | checker `validate_struct_init` |
| 14 | conversion-intrinsic arity not validated (missing/excess args) | 🟢 diagnostic | checker intrinsic arg check |

---

## FIXED (2026-07-01) — BH-18 #1 — move-struct pointer alias defeats ownership/free tracking (🔴 soundness) — ALL THREE (1a/1b/1c) CLOSED

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
    *Task alias = o.p;          // raw-ptr alias copied out before the move
    release(o);                 // moves o AND frees the slab slot (cross-function)
    *Task fresh = pool.alloc_ptr() orelse return;  // reuses the just-freed slot
    fresh.id = 222;
    u32 corrupted = alias.id;   // reads the REUSED slot -> 222, no trap
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
    close_file(f);                  // first consume
    FileHandle reborn = *alias;      // resurrect moved-from f via the alias
    close_file(reborn);             // second consume of the SAME fd
    return (u32)g_closes;            // EXIT=2 -> double-close
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

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #2 — VRP range-narrowing scope leak → unchecked OOB write (🔴 soundness)

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
    for (u32 k = 0; k < 5; k += 1) { idx += 1; }   // idx == 5 (laundered past VRP)
    bool b = false;
    if (b) {
        if (idx >= 4) { return 0; }   // guard runs ONLY when b is true
    }
    buf[idx] = 2989;                  // idx==5 -> OOB write, NO guard emitted
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

## FIXED (2026-06-23) — BH-18 #3 — `@bitcast` forges integer↔pointer, bypassing the mmio/inttoptr gate (🔴 soundness)

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
    p[0] = 99;            // writes through a forged pointer, no gate
    return real;          // EXIT=99
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

## FIXED (2026-06-23) — BH-18 #4 — `@pun(*Struct, *primitive)` silently skips its runtime type_id trap → OOB read (🔴 soundness)

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
    *Big bp = @pun(*Big, sp);   // 4-byte target punned to 16-byte struct, no trap
    return (u32)bp.b;           // reads offset 8, past the 4-byte 'small'
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

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #5 — fixed-array bare-call index drops the bounds check (🔴 soundness)

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
    a[idx()] = 999;   // idx()==100 -> OOB write into a u32[4], no trap
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

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #6 — `if(opt)|*v|` capture escapes a pointer-to-local to a global (🔴 soundness)

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
    if (m) |*v| { g = v; }      // pointer-to-local m escapes to global g
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

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #7 — shared multi-access via cast/intrinsic/index/orelse subexpr (🟠 race)

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
void f(*A pa, *B pb) { pa.x = (u32)pb.y; }   // reads B's field under only A's lock
u32 main() { return 0; }
```
Emitted `f()`:
```c
void f(struct A* pa, struct B* pb) {
    pthread_mutex_lock(&pa->_zer_mtx);
    _zer_t0 = pa->x = ((uint32_t)pb->y);   // <-- reads pb->y of struct B with NO B lock
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

## FIXED (2026-07-01, copied from cool-johnson-11ct36) — BH-18 #8 — `spawn` data-race scan is blind to function-pointer indirection → data race (🟠 race)

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

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #9 — shared-struct read in `await` condition emitted unlocked (🟠 race)

**Symptom:** accessing a `shared struct` field in an `await` condition compiles
clean and emits an **unlocked** read, violating both the D02 "no shared access
in a yield/await statement" ban and the "shared struct = auto-locked" guarantee.

**Reproducer:**
```zer
shared struct Flag { u32 ready; u32 data; }
Flag g;
async void waiter() {
    await g.ready > 0;     // shared read in await cond -> D02 should reject, doesn't
    g.data = g.ready;      // (this one IS properly mutex-wrapped)
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

## FIXED (2026-06-26, copied from cool-johnson-t8vr3h) — BH-18 #10 — value-returning `async` never finalizes its state machine (🟡 miscompile)

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
    _zer_async_compute_poll(&task);   // poll 1: yield
    _zer_async_compute_poll(&task);   // poll 2: completes, side += 1000
    _zer_async_compute_poll(&task);   // poll 3: should be no-op...
    _zer_async_compute_poll(&task);   // poll 4: ...but re-runs the tail
    return side / 1000;               // EXIT=3 (tail ran 3x); expected 1
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

## FIXED (verified 2026-07-01 — was stale OPEN entry) — BH-18 #11 — bit-query/byte-swap intrinsics emit `0` in global initializers (🟡 miscompile)

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
u32 g = @popcount(255);   // emitted: uint32_t g = /* @popcount — unknown */0;
u32 main() { return g; }   // EXIT=0 ; expected 8
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

## FIXED (2026-07-01) — BH-18 #12 — `defer` + backward `goto` fires the wrong count (🟡 miscompile; folds into known item "defer fires twice")

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
    defer inc();              // registered once, before the label
    loop:
    i += 1;
    if (i < 3) { goto loop; }  // 2 back-edges
}
u32 main() { run(); return counter; }   // EXIT=2 ; expected 1
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

## FIXED (verified 2026-07-01 — was stale OPEN entry) — BH-18 #13 — nested inline designated initializer rejected ("got void") (🟢 false-reject)

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
    Outer o = { .pos = { .x = 3, .y = 4 }, .id = 9 };   // error: field '.pos' expects 'Inner', got 'void'
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

## FIXED (2026-07-01, copied from cool-johnson-11ct36) — BH-18 #14 — conversion-intrinsic arity is not validated (🟢 diagnostic)

**Symptom:** the conversion/layout intrinsic family (`@truncate`, `@bitcast`,
`@saturate`, `@cast`, `@inttoptr`, `@ptrcast`, `@size`) does not validate
argument count: a **missing value operand** passes the checker and emits invalid
C (GCC then errors on a non-existent source line), and **excess trailing args**
are silently dropped.

**Reproducers:**
```zer
u32 main() { u8 x = @truncate(u8, 5, 6, 7); return (u32)x; }   // EXIT=5 — args 6,7 silently dropped
```
```zer
u32 main() { u8 x = @truncate(u8); return 0; }   // checker accepts; emits (uint8_t)(); GCC: "expected expression before ')'"
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

## OPEN — Concurrency memory-safety: ~24/25 holes CLOSED, 1 open (cross-block scoped-borrow) + named floors (2026-06-22)

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

## Tracking notes

All entries in `KNOWN_FAIL` skip lists (tests/test_zer.sh,
rust_tests/run_tests.sh, zig_tests/run_tests.sh) are back-referenced here.
When fixing an entry, remove it from the relevant list to prevent
regression-hiding.

When a `tests/zer_gaps/` reproducer is fixed, move it to
`tests/zer_fail/` so it becomes a permanent regression guard.
