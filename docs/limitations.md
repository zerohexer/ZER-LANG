# ZER Compiler — Open Limitations

**This file holds OPEN entries only**: accept-unsafe holes, over-rejections, loud gaps,
deferred fixes, and the standing floors. It is meant to be read whole. `make check` enforces
it: a section or list item marked closed fails the build (`tools/audit_limitations.sh`), and
so does a file past its size budget (`tools/audit_docs_budget.sh`).

| Where | What |
|---|---|
| **this file** | what is open NOW — each entry measured, with a reproducer or a pointer to one |
| `docs/limitations-closed.md` | entries that were closed, moved here verbatim. Append-only. Never read whole: `grep -n "BUG-1651" docs/limitations-closed.md` |
| `BUGS-FIXED.md` | the fix ledger (symptom, root cause, fix, test) |

**Writing an entry.** Symptom, the smallest reproducer (or its `tests/zer_gaps/` file), root
cause, why the obvious fix is wrong if it is, a fix sketch. Say which kind it is:
accept-unsafe (a wrong program compiles), over-rejection (a right program is refused),
loud (GCC or a trap reports it), or floor (outside the language).

**Closing an entry — in the SAME commit as the fix.**
1. Move the entry's text to the END of `docs/limitations-closed.md`, under a heading that
   names the BUG number. Do not strike it through and leave it here.
2. Whatever the fix did NOT close is a NEW open entry of its own here, tagged
   `[residual of: <what was closed> — closed by BUG-n]`. A residual left inside a closed
   entry is how open work gets lost.
3. The fix itself goes in `BUGS-FIXED.md`; the regression test or grid cell is the durable
   memory — once a gate carries the knowledge, nothing needs to cite the closed entry again.

**A citation to a section that is not here** (other docs cite `docs/limitations.md "<title>"`):
the section was closed — grep the title in `docs/limitations-closed.md`. The old
`docs/limitations-legacy.md` (unmeasured pre-2026-09 entries) was triaged and deleted on
2026-11-14; its sections are in the closed archive under `[ARCHIVED 2026-11-14]` headings,
behind the measurement record "LEGACY LEDGER RETIRED 2026-11-14".

---

## STANDING — floors and long-lived gaps (these do not close; re-measure before relying on one)

- **Floors (out of scope for any language, by design):** `cinclude` / C-FFI code; hardware
  behaviour (datasheet facts); liveness — a deadlock or livelock that is a LOGIC error (lock
  order across statements is prevented by construction; waiting forever on a condition is
  not); the emitter and GCC are trusted (a sound checker is not a verified compiler).
- **`naked` grants asm permission but is NOT emitted as an attribute** (a DECISION, and the
  compiler says so: `warning: 'naked' is accepted for asm permission but the attribute is
  NOT emitted …`, BUG-852). The function is an ordinary C function with a prologue and a
  trailing `return;`. It is not restored by a flag flip, for measured reasons recorded at
  the warning in `checker.c`: most `tests/zer/asm_*.zer` CALL their naked function and
  would fault without an epilogue, and GCC allows only basic asm in a naked function while
  ZER's structured `asm { inputs: … }` — the form the operand tracking is built on — is
  extended asm. Decoupling "may write asm" from "emit no prologue" is the locked Option E
  rework (`docs/option_e_plan.md`). For true naked code today, write the stub in C and
  bring it in with `cinclude`. (An earlier revision of this entry called it a silent
  miscompile; that was wrong — the probe had grepped for errors only.)
- **Bare-metal `.bss` zeroing is a build-system contract (floor).** "Everything is
  auto-zeroed" holds for globals only if the startup code clears `.bss` before `main`. A
  hosted C runtime does; on bare metal the user's linker script and startup assembly must
  (the usual `__bss_start` .. `__bss_end` loop). ZER cannot enforce this from inside the
  emitted code: if the target's startup does not zero `.bss`, every guarantee about a
  global's initial state is void.
- **A `*opaque` that comes from C is tracked by a NAME heuristic (floor: `cinclude`).** A
  bodyless `void f(*opaque)` and a bodyless function whose name contains a destructor word
  (free, destroy, close, release, delete, dispose, drop, cleanup, deinit, fini, shutdown,
  term) are assumed to free their argument; a C destructor with an idiosyncratic name and
  no visible body is not recognised. Wrap it in a ZER function, whose body the analysis can
  read. **`--track-cptrs` is NOT a run-time backup for this**: its double-free check runs
  (inside the `malloc` / `free` wrappers), but its use-after-free check, `_zer_check_alive`,
  is emitted and never called from a function body (measured 2026-11-14 — zero call sites
  in the emitted C for `@ptrcast(*T, o)` and `(*T)o`). It is not wired on purpose: the check
  reads a 16-byte header in front of the pointer, which is itself an out-of-bounds read for
  any `*opaque` that did not come from the wrapped `malloc` (the address of a global, a
  stack object, an interior pointer) and a read of freed memory for one that did.
- **An intrinsic's result carries no value range (precision only).** `u32 i = @popcount(x);
  tbl[i]` with `u8[33] tbl` is not proven in range although the result is at most 32; the
  index keeps its run-time guard, so nothing is unsafe — the check is just not elided.
- **The text of an `asm` block is a floor.** The Z-rules guard the typed operands; the
  structural rules (16 instructions, no label — one reading for both forms since BUG-1658)
  are audit rules over the block's shape. A jump to a symbol defined elsewhere, a computed
  jump, or a directive that defines a symbol without a colon is not seen: recognising those
  needs the per-instruction tables Option E delegates to GCC.
- **Proof coverage** — what is and is not machine-checked for the data-race class is entry
  B2 below; `proofs/vst/verif_window_rules.v` is written and has never been compiled.



## OPEN — measured while retiring the legacy ledger (2026-11-14; BUG-1658..1661)

Each item was run on the build that carries BUG-1658..1661. None is accept-unsafe.

1. **`orelse` with a value or block fallback inside a `defer` body is refused**
   (over-rejection). `defer cleanup(maybe() orelse 0);` and `defer { u32 z = none() orelse
   g; }` report *"cannot use 'orelse' with a value/block fallback inside a defer body"*.
   The ban dates from when a defer body was emitted from raw AST by an emitter with no
   orelse arm; the body is real IR now (refactor L, stage 2), so the reason is gone, but
   lifting a ban is a RELAXATION and has not been done. Rewrite: compute the value before
   the `defer`, or use `if (opt) |v| { … }` in the body.
2. **`@truncate` / `@saturate` do not fold in a `comptime` function body** (over-rejection):
   `comptime u32 TR() { u32 x = 300; return @truncate(u8, x); }` reports *"body could not be
   evaluated at compile time"*. A C-style cast does fold (`u8 y = (u8)x;`) — use it.
3. **A call whose callee returns only literals is treated as possibly returning its
   argument** (over-rejection, precision of the return summary):
   ```zer
   const [*]u8 pick(const [*]u8 unused) { return "literal"; }
   const [*]u8 via() { u8[4] b; return pick(b); }   // "stack memory may escape through function return"
   ```
   A literal return has no provenance, so the summary could classify it static
   (`ret_param_mask` 0), as `return null` already is. Real precision work on
   `call_result_static_given_args`, and a relaxation.
4. **Two emitter refactors are unfinished (architectural debt, no known defect).**
   *L, stage 3*: `emit_defer_stmt`, the last raw-AST statement emitter, still exists; a
   defer body is lowered to IR at registration and every fire clones that template, but the
   raw path is kept for the forms below. *M*: five bounds auto-guards are still spliced as
   C by `emit_auto_guards` instead of being IR branches — the ones in a loop condition or
   an `await` condition (re-evaluated, so a guard hoisted before the statement would be
   checked once) and the ones whose early return needs a zero value of an optional or
   struct type. The risk of both is the class they were started to remove: a safety rule
   that must be implemented twice. Design and measurements:
   `docs/limitations-closed.md`, "[ARCHIVED 2026-11-14] OPEN — BRANCH
   `loving-davinci-ii7a90`", sections L and M.

## OPEN — residuals of the 2026-11-13 audit (BUG-1609..1643; measured, reproducers in `tests/zer_gaps/`)

Found by the four probe runs of this session and NOT fixed. Each accept-unsafe item has an
executed reproducer in `tests/zer_gaps/` (compile-clean = the gap still holds; the runner
fails loudly when one closes). "exit 99" = a stale pointer wrote a recycled slot and the
program returned the corrupted value.

**A. Allocation tracker — a free / store / move the caller cannot see (MEDIUM, accept-unsafe).**
The view work of BUG-1620..1626 resolves a view whose root is a local aggregate, a pointer
that holds one value (a parameter, a local bound once), a whole or nested global slot, and a
stable global pointer — in the function AND in the summaries built from it. What is left is
each a missing SUMMARY fact, not a missing key:
2. **A field free the summary cannot NAME.** `frees_param_field` is one bit per parameter —
   "frees some field". The two-hop forwards are CLOSED (BUG-1634: `drop2(&c.h)`,
   `drop2(&g.h)`), but the bit carries neither WHICH field nor whether the callee RESET it:
   - inside a function, a pointer param's field freed by a callee stays ALIVE for a direct
     read (`drop(h); h.p.v = 99;` — no entry exists under `h` to mark, and marking every
     pointer field read after the call would refuse `free_a(h); free(h.b);`).
     `alloc_param_field_freed_by_callee_then_used`.
   - (over-rejection, loud) a teardown that frees AND resets its fields, called again after a
     PARTIAL refill, is "handed 'c', which carries a freed pointer at 'c.h2.p'" — one hop or
     two. Refill every field, or reset them in the caller.
   The durable fix is a per-path list (`{param, path, must, reset}`, the shape `param_store`
   and `ret_field` already have) in place of the two bool arrays.
3. [residual of: A getter of an ADDRESS — closed by BUG-1642, BUG-1641]
   Residual: a getter returning `&g.sub` (a projection of a
   global, not the bare global), a getter reading a field through a by-value COPY of the param's
   struct (`H h = c.h; return h.p;` — no slot entry exists to match), and a getter with two
   different `&G` returns (not summarised, so the result is an unrelated value).

4. [residual of: A global reached through a COPY or a capture — closed by BUG-1640]
   Residual: a copy of a global ARRAY of structs (the leaf path
   contains a variable-index family `g[*].p`, deliberately not minted as one slot) and a copy
   made through a pointer-to-global that is reassigned.

6. [residual of: A `?*T` read out of a slice view of a local array — closed by BUG-1635]
   Residual precision (loud): a VARIABLE index through a view that does not start at 0
   (`[*]?*T u = arr[1..3]; for (i..) { free(u[i]); u[i] = null; }`) is "some element", so
   the reset reaches no specific slot and the loop is refused — the same answer as the
   direct `arr[i + 1]` spelling. Iterate the array, or a view starting at 0.

8. [residual of: `move struct` across a pointer / slice parameter — closed by BUG-1639]
   Residual
   (over-rejection, loud, unchanged): the crude call-site rule still transfers a ROOT written
   `&w` / `w` passed to ANY function — `peek(&w); peek(&w);` on a struct containing a move
   field is "use after move" although `peek` only reads. The summary bit 32 now exists to
   relax it; doing so is an accept-unsafe change (every way a callee can consume must be in
   the bit) and needs its own audit.

9. **Silent leaks** — ~~`use(alloc(T) orelse return)`~~ (CLOSED, BUG-1636/1637), a
   `goto` that jumps over a `defer free(a)`, and a GLOBAL slot overwritten with null
   (`g.p = alloc(T); g.p = null;` — global entries are escaped, so the overwrite query stays
   silent; the LOCAL-slot form is reported since BUG-1627).
10. **Over-rejections seen (loud):** a field freed through a getter-returned pointer marks the
   POINTER freed (`*H h = geth(); if (h.p) |q| { free(q); } h.p = null;`); a callee that frees
   and RESETS a global's field through its pointer param is "left dangling" at the caller
   (`drop(&g_h)`); the destructor `if (t.l) |q| { free(q); } ... free(t);` is "returning while
   't.l' holds a freed pointer" (the non-optional spelling compiles); the grow idiom
   `nd.v = old.v; free(old); v.data = nd;` infers `keep` on the vector parameter; `*H h =
   &c.h; free(h.p);` on a HEAP `c` reports "wrong pool"; `return get2(&c.h)` with `c` a pointer
   parameter is "local-derived"; a range-for teardown over an array of structs is a false
   use-after-free; `defer th.join();` in a function with a label is "not joined".

**B. Concurrency (MEDIUM, accept-unsafe — each TSan- or ASan-confirmed).**
3. [residual of: The scoped-borrow window is textual — closed by BUG-1648, BUG-1651]
   **Residuals (precision, loud):** with an OPEN
   window the lent variable cannot be touched before that tail join (in a labelled function:
   anywhere before it, a write before the spawn included); one variable cannot be lent from
   two spawn sites in a labelled function; a join in EVERY arm of a branch, or a deferred
   join, never releases; an interior `goto` to a label that is itself inside the interior
   opens the window; the atomic-downgrade check treats an open window as never closing.
   Hoist the join, or move the spawn..join into its own function.

4. [residual of: A helper lends its pointer param and touches the same global by NAME — closed by BUG-1645, BUG-1646]
   **Residual (precision, loud):** the pair is keyed on the
   ROOT object, so two arguments naming DISJOINT fields of one struct (`sp(&t.x, &t.y)`) are
   refused as "both reach 't'"; a field-path disjointness test would recover it.

7. [residual of: ISR split read-modify-write through MEMORY — closed by BUG-1650]
   **Residuals:** (a) the global carrier relation is flow-INSENSITIVE, so `mem = g; mem = 0;
   g = mem + 1;` is refused (loud); (b) the split done in HELPERS called only from a handler
   (`interrupt T { rd(); wr(); }`) is not attributed to the handler — the handler side of the
   rule (a handler is not preempted by main) is the conservative half, the main side is the
   lost update and is covered; ~~(c) the SPAWN scan's sink does not read the global carrier
   relation~~ — CLOSED (BUG-1652: both thread sinks now read the main checker's facts).
   **Residuals of BUG-1652 (precision, loud):** a thread's unlocked read-modify-write is
   refused even when it is the flag's only writer (use `@atomic_*`, a CAS lock, or the
   `@once` form) — the rule the spawn scan always had; the carrier relation is
   flow-insensitive, so in `vm = g; g = vm + 1;` the diagnostic may name the carrier `vm`
   rather than `g`. (The spawner's window rule is lock-aware since BUG-1656.)

8. [residual of: BUG-1625 sees a CHECKED body only — closed by BUG-1649]
   **Residual (precision, loud):** that late check does not apply the `@once`
   single-owner exemption — declare the spawn target above its spawn site.

**B2. Proof status of the data-race class (2026-11-13 — read before claiming "proven").**
What is machine-checked, and where the chain stops:
- `window_cfg.v` (Coq 8.18 + stdpp, zero admits, compiled in the session): the spawn-window
  RULE is sound for every control-flow graph (`window_sound`), with the closed, tail and
  latched windows as instances, and the old textual window refuted on the break-out graph.
- `proofs/vst/verif_window_rules.v`: VST specs + proofs for the three extracted decisions
  (`zer_window_join_releases`, `zer_window_stmt_leaves`, `zer_window_counts_at_spawn`).
  **WRITTEN BUT NOT YET MACHINE-CHECKED** — neither VST nor RefinedC could be installed in
  the session that wrote it (no opam mirror reachable). It follows the cascade pattern of
  `verif_concurrency_rules.v` and is wired into `make check-vst`; the first run of that
  target is its verification. A brute-force comparison of the C against the proven
  characterization (1548 argument tuples, 0 mismatches) was run instead.
- NOT proven: that `checker.c` discharges each hypothesis of the instances (the syntactic
  walk `win_exits_live`, the branch-depth / jump census at the join) — by inspection and the
  conc-matrix "jump x spawn-window" cells only. NOT modelled at all: the spawn-target race
  scan, the atomic-cell and atomic-downgrade rules, shared-struct locking, the ISR rules.
  The Iris subset proves the four-condition closure sufficient; nothing proves the checker
  enforces it on every program form. "100% data-race-free, proven" is therefore NOT a claim
  this tree supports; "no known accept-unsafe hole in the spawn-window rule, and that rule is
  proven" is.
- Accept-unsafe items still recorded as OPEN in this class: none as of 2026-11-14 — the last
  three (a thread's split read-modify-write through memory, the `*opaque` handle handed on,
  one object passed under two names to a lending function) were closed by BUG-1652..1654.
  That is a statement about this ledger — what was probed and measured — and not a proof:
  each of those three was found by probing, not by a failing proof. Recorded and NOT a race
  as far as measured: the handler-side split RMW done in helpers (a handler is not preempted
  by main code). `cinclude` code is a floor.

**C. Values / bare metal (LOW, loud).**
- A shift COUNT literal is typed against the LEFT operand: `u3 y = x << 8;` is "cannot
  initialize 'u3' with 'u32'" while `x << 7` compiles (the documented result is 0).
- Shift typing: a shift's result is the COMMON type of its operands (by design), and a lone
  literal count adopts the left operand's type — so `u8 s; u32 d = s << 12;` shifts at u8
  width and is 0. Cast the value first (`(u32)s << 12`).
- `if (i < 8) { return reg[i]; }` on an MMIO pointer with max index 7 still gets the
  auto-guard (precision).
- `@cpu_save_context` / `@cpu_restore_context` emit `sd` / `ld` for riscv32 (RV64-only
  instructions): the build fails on a 32-bit RISC-V target.
- `**u32 hold = &(&X);` (address of an rvalue) is accepted by the checker and refused by GCC.

**D. Recorded design decisions (not defects).**
- A local array passed to a funcptr PARAMETER of type `[*]T` is not `keep` (a retaining
  callback then holds a dangling slice): the deliberate, measured exemption in
  `keep_edge_callee_keeps` — an unknown callback that takes a buffer is the read/fill idiom.
- `@container` on a pointer PARAMETER has unknown provenance and is allowed (CLAUDE.md
  "Provenance Tracking"); a caller passing `&whole_object` is the caller's bug to see.

## OPEN — residuals of the 2026-10-12 audit (BUG-1566..1596; measured)

1. **The `*opaque` C bridge (BUG-1594).** A C extern with an `*opaque` / `?*opaque` in its
   signature is called through a static inline wrapper selected by a function-like macro.
   (A VARIADIC extern with an `*opaque` is refused since BUG-1601.) Not bridged: TAKING THE
   ADDRESS of a bridged
   function (`fp = sensor_read`) names the C function, whose `void *` signature differs from
   the ZER function-pointer type — GCC refuses it (loud). With any `cinclude` in the
   program the bridge emits no prototype of its own (the header is the authority); a
   bridged function the header does not declare is then GCC's "implicit declaration"
   (loud). Without a `cinclude`, a non-bridged extern binds to its C symbol under a private
   name (BUG-1602), so the hard-coded stdlib skip list in `emit_top_level_decl` is now only
   needed WITH a `cinclude`, where ZER's prototype must still agree with the user's header
   (a mismatch is GCC's "conflicting types", loud). The bridge's own `void *` prototype is
   aliased the same way.
3. **Condition facts (BUG-1566/1607) are taken from EFFECT-FREE conditions only.** Since
   BUG-1607 `i < n` reads `n`'s CURRENT range (`if (n < 8 && i < n)` proves `i <= 6`, either
   conjunct order). Still not used: a bound that is an expression (`i < n - 1`, `i < s.len`
   of a slice) and a relation between two variables that have no range yet (no relational
   domain: `i < n` stored as a fact and applied when `n` is narrowed later is not kept).
4. **Range-for over an element** (`for (x in hs[k].s)`) is refused (BUG-1581) — assign the
   collection to a local. A relaxation would need a snapshot of the collection itself.
5. **`can_detach` (BUG-1589) follows direct calls, module-qualified calls and function
   names passed as arguments**; an indirect call through a funcptr reaches its candidate
   set through `func_props_merge_reached` only where the existing FuncProps machinery does.

## OPEN — residuals of the 2026-10-10 audit (BUG-1511..1537; measured)

1. **What the atomic/barrier downgrade check (BUG-1562/1563/1565) still trusts.** The
   orderings it RECOGNISES are spelled in one statement list: a lock is a spin `while` on
   `@atomic_cas` / `@atomic_xchg` up to its release; a hand-off is a write followed by a
   non-load atomic on the flag, and a read after a loop / `if` reading the flag atomically.
   It does not compare VALUES: a store to the flag after the storer's own wait on it is a
   relay (allowed), and the publisher's own store BEFORE its write is not distinguished
   from its publish. Bindings of pointers to globals follow `&g`, a slice, a struct
   literal, a pointer name's declaration and reassignments, and parameters — not a pointer
   read back out of memory (`*pp`, a pointer field of a global loaded at run time). A
   C caller is still invisible beyond "may run more than once".
2. **The split RMW through CALLS (BUG-1561/1564)** follows returns, stores into globals,
   through pointers (global, local, parameter), forwarding and call cycles. Not followed: a
   value passed back through memory a pointer argument points at (an out-parameter
   `void rd(*u32 o) { *o = g; }`), which the per-object summary does not model.
3. [residual of: A helper called only from an interrupt handler counts as MAIN code — closed by BUG-1608]
   CLOSED
   2026-10-15 (BUG-1608). Still conservative: a function is ISR-only only when it is `static`
   or the program has a ZER `main` (a `--lib` build keeps every non-static helper as main
   code, because C may call it), and names are matched across modules as one set (two
   modules' `bump` share their callers).

5. **Over-rejections from the allocation tracker:** (the free-every-slot loop over a GLOBAL
   array compiles since BUG-1605.) Freeing a heap linked list
   built from `alloc(Node)` + `?*Node next` is refused in the iterative and recursive forms.
   **Why it is a design change, not a patch (analysed 2026-10-15).** Every node comes from ONE
   allocation site, so they share one identity (`aid = local + 1`), and `n.next = head`
   aliases the field to that identity: freeing the first node marks them all freed. The
   tempting fix — give a read of `p.next` a FRESH identity — is UNSOUND three ways: a cycle
   (`last.next = first`, or `n.next = n`) makes `p.next` an already-freed node; a second
   pointer into the list (`tail`, a global) frees a node the traversal later reaches; two lists
   sharing a tail free it twice. A sound version needs a proven OWNERSHIP invariant on the
   link field — every object has at most one incoming `next` and the chain is acyclic — checked
   at EVERY store to that field program-wide (`n.next = head; head = n;` with `n` fresh and
   `head` dead afterwards preserves it; `tail.next = n; tail = n;` keeps a second pointer and
   needs `tail` treated as a borrow that dies before any free), plus recency abstraction in
   zercheck_ir (the node being freed is a materialised singleton taken out of the "rest of the
   list" summary, which stays alive). That is shape analysis: oracle first
   (`handle_flow_lattice.v`-style, with the ownership invariant as an explicit precondition —
   the Level B lesson), then the C. Workarounds today: a `Pool`/`Slab` with `Handle(Node)`
   links, or nodes in an array with index links.
   The variable-index global free (BUG-1530) widens a caller's `gs[0]` even when the
   callee's index provably differs.
7. **Block lifetimes (BUG-1554) are compared at ASSIGNMENTS only.** The other two ways a
   pointer to a local leaves its block — a callee storing it (`set(&outer, &x)`) and a
   store through a pointer (`*pp = &x`) — are refused by the stricter FRAME rules
   (keep inference / "through pointer or slice"), measured 2026-10-11. Sound, but those
   rules refuse them even when everything lives in one block (over-rejection).
10. **Over-rejection:** a comptime u64 value at or above 2^63 is "could not be evaluated"
   (the INT64_MIN sentinel / 64-bit evaluator). (A negative i128 divisor literal — CLOSED,
   BUG-1538.)
11. **A callee freeing through a pointer-to-reference param (BUG-1548) needs the argument's
   target.** `rel(pick(&a, &b, c))` is refused ("which object that is cannot be
   determined") rather than widening both candidates; a pointer-to-reference reached
   through a struct field of a param (`free(*b.pp)`) is not keyed.

## OPEN — residuals of the 2026-09-28 round-4 hunt (BUG-1490..1510; measured)

1. **Leak half of a joined identity (BUG-1490).** When a pointer names allocation A on one
   path and B on the other, a free through it marks the kept identity FREED on both paths, so
   a leak of A on the path that pointed at B is not reported. Memory safety is intact (every
   member is checked at a use); only the leak diagnostic is lost.
2. **A literal tree whose exact value lies outside int64** (`(i128)(0 - 10000000000000000000)`)
   is not retyped wider and still wraps at 64 bits (BUG-1505 residual — pre-existing).
3. **Label functions widen VRP over the whole body** when anything takes an address (BUG-1499):
   sound, but a function with a label and an `&` anywhere loses ranges it could keep.

## OPEN — residuals of the 2026-09-28 MMIO / parser / compile-time round (BUG-1480..1487)

1. **MMIO register sharing** does not follow a local alias of a parameter inside a helper
   (`r = p; r.x |= 1`), a struct carrying a register pointer, a function reached through a
   funcptr binding (walked with parameters unbound), or a global register pointer not
   initialised directly with `@inttoptr`; register READS are not recorded.
2. The parser's 256 depth limit protects an -O2 zerc on a 1 MB stack; an -O0 build crashes
   near 200 levels either way.
3. Remaining super-linear compile time: the VRP store scans every recorded range
   (`vrp_apply_store` / `vrp_key_relation` — 20,000 local declarations take ~2.3 s), and
   zercheck_ir looks function summaries up linearly (20,000 functions ~1.9 s).

## OPEN — residuals of the 2026-09-28 single-evaluation / lock round (BUG-1470..1476)

3. The moved volatile predicates resolve a root name in the checker's CURRENT module; the
   emitter calls them after checking, so a module global shadowed by a same-named main
   global could be judged by the wrong declaration's qualifier (unprobed).

## OPEN — residuals of the 2026-09-28 passthrough / view round (BUG-1460..1465; measured)

2. A slice local with a VARIABLE index, or defined with a non-literal start, is keyed on
   the slice local rather than the array (a global slice variable too).
3. A stable aim through a POINTER-typed root (`&hp.p`, `hp` a pointer) is not resolved.
4. Over-rejection: `if (gos) |*c| { c.p = a; } free(a); if (gos) |*c| { c.p = null; }` is
   "global left dangling at exit" — the two `if (gos)` tests are not known to agree.

## OPEN — module name mangling is not one-to-one (2026-09-28, LOW — loud)

`mod__name` joins the module and the identifier with `__`, so module `a` declaring
`b__c` and module `a__b` declaring `c` both emit `a__b__c` (a GCC redefinition), and
`a_` + `b` vs `a` + `_b` collide the same way. A proper fix is an unambiguous scheme
(length-prefixed, or refusing `__` in module and identifier names), which touches every
emitted name.

## OPEN — residuals of the 2026-09-28 allocation-tracker round (BUG-1410..1417; measured)

1. **BUG-1411 is narrowed, not closed**: an allocation made INSIDE a loop, stored at an
   untrackable index (`arr[g(i)] = a`) and freed through a slot read in the same loop body,
   then used by its own name, is still accepted (the per-instance wildcard exemption cannot
   tell one iteration's allocation from another's).
2. **A Ring filled in one function and drained in another** is not followed (the Ring
   transfer, BUG-1414, is per function).
3. **Over-rejection**: a free-every-element loop over STRUCT elements stored at literal
   indices (`hs[0].p = a; hs[1].p = b; for i { free(hs[i].p) }`) is refused — the same
   verdict the plain pointer-array version (`?*T[2]`) already had.

## OPEN — residuals of the 2026-09-28 concurrency round (BUG-1420..1425; measured)

1. **A funcptr LOCAL reaches every function of its signature** for the context bans
   (BUG-1425), so `*() -> u32 fp = tick;` inside @critical is refused when ANY `() -> u32`
   function (even `main`) allocates. A local bound once at its declaration could resolve to
   that one function (the spawn scan's binding walk) — over-rejection only.

## OPEN — an ARRAY parameter is treated as frame storage by the escape sinks (2026-09-28, LOW — over-rejection)

`[*]u32 head(u32[4] a) { return a[0..2]; }` is refused ("cannot return pointer to local
'a'"), although an array parameter ALIASES the caller's array (reference.md: an array
parameter does not copy) — the same shape with a `[*]u32` parameter is accepted
(BUG-764). Relaxing it is a multi-site change and an accept-unsafe risk: the parameter
setup does not taint an ARRAY param non-keep (only pointer / slice / opaque / carriers),
`classify_return_root` must classify the view as ARParam(n) so `g = head(local)` is
refused at the call site, and every store sink (global, out-param, spawn, Ring) must
agree. Until then, take a slice parameter.

## OPEN — residuals of the 2026-09-28 escape fixpoint (BUG-1402; LOW — over-rejection)

The loop / goto / defer join is sound by construction (it only ever ADDS may-taint) and
measured at zero corpus cost; its precision residue, each an over-rejection:

1. **A reassignment inside a branch or loop body never clears** (BUG-1274's sticky rule),
   so with the loop join `for (...) { p = &g; gp = p; p = &x; }` is refused although `gp`
   only ever receives `&g`. The precise form is a strong update inside the body plus a JOIN
   of the saved flags at the body's exit — a relaxation, so it needs the accept-unsafe
   discipline (CLAUDE.md) and the p61 cells as its net.
2. **A function with any label / goto is flow-insensitive** for escape flags: a pointer that
   holds `&local` at one point and `&global` at another may not be published anywhere in it.
   A per-label join (the flags at each `goto` joined into its label) would be precise.
3. **A defer body sees the union of everything a variable EVER holds in the function**,
   including values it held only BEFORE the defer was registered.
4. The re-walk runs only when the first walk saw a taint appear that an earlier sink could
   have missed, but then the function is checked two or more extra times (bounded by 16
   fixpoint rounds; past them the facts SATURATE to their union — BUG-1489 — which is sound
   and coarse: a function with a 17+-link backward copy chain gets every pointer tainted).

## OPEN — residuals of the 2026-09-27 rounds (BUG-1366..1399; measured)

1. **`*p = a` through a pointer-to-POINTER re-aimed by anything but `&name`** (BUG-1366,
   narrowed). A store through a deref is resolved when p is bound once to `&x`, is a global
   initialised `&x` and never re-aimed, or is a local re-aimed only between `&x_i` (weak
   store). A pointer to a pointer SLOT that is a parameter, a field / element (`*h.pp = a`),
   or a call result is not resolved. (Projections through a pointer to an AGGREGATE — `p.f
   = a` — are resolved through views, BUG-1395..1397, including re-aims through copies,
   calls and joins.)
2. **A view whose candidates are UNKNOWN** (BUG-1397). A pointer that may view more than
   eight slots, or that joined with an unknown view, stores under its own key only; a read
   of the real slot after the free is then not refused. A GLOBAL carrier whose view is
   established in ANOTHER function (`void init() { gss.s = &gs; }`) is likewise not
   followed — the view is a per-function fact. Fix sketch: a whole-program "this global
   pointer field is only ever `&G`" query, the projection sibling of
   `checker_global_never_mutated`.
3. **Lock order across a call — precision residue** (BUG-1398). A callee's instance is
   known when it is a named global, or a pointer parameter mapped through the call's
   argument (`&G`, or a local / parameter of the calling statement's function); an
   indirect call is checked through each function it may call. A callee touching its OWN
   local shared struct, a global POINTER, or a parameter reached through an indirect call
   inside another function's summary counts as "any instance", which refuses the
   statement (over-rejection, never an accepted deadlock). Corpus cost measured: zero.
5. **A store through a sub-slice with a VARIABLE bound is not keyed** (BUG-1383 closed the
   constant-bound case: `la[1..3][0]` is `la[1]`, for a local and a global array). `la[i..n][0]
   = a; free(a); … la[k]` still reaches the slot through no key; the slot's value is not
   tracked. Fix sketch: land a non-constant re-base in the array's wildcard slot so every
   element read afterwards sees MAYBE_FREED.
6. **Wording:** a use through a field of a freed allocation names the ROOT (`use after free:
   's' is freed`) instead of the field path; `use of freed value an unnamed temporary` for a
   read through a pointer deref.
7. **Over-rejections seen:** a swap / copy helper `void swap(*H x, *H y) { *T t = x.p; x.p =
   y.p; y.p = t; }` infers `keep` on its params, so `swap(&h1, &h2)` on locals is refused
   (the value stored is the POINTEE's field, not the param); a Ring push/pop of a struct
   carrying a pointer reports the pushed allocation as never freed after `free(x.p)` of the
   popped copy.

9. **>64-bit constants: an admitted set, not a 128-bit evaluator** (BUG-1384). A u65..u128
   global / static initializer must be literals under `+ - * & | ^ ~` and casts — a shift, a
   division or a name is refused though GCC would fold it correctly; a comptime function or
   comptime local wider than 64 bits is refused outright. The durable fix is a 128-bit
   constant evaluator (the typed fold and the comptime interpreter both carry int64_t).

10. **Compile time grows super-linearly on some long functions** (measured 2026-09-27;
   the linear local lookup, 76% of the 16,000-line case, is fixed — BUG-1394, 24 s -> 0.9 s).
   Remaining: a function holding N live allocations each followed by its own `orelse
   return` keeps a state per block and copies it (800: 3.5 s, mostly memory churn); a chain
   of N pointer copies is O(N^2) in `for_each_write_target` (800: 1 s). Neither hangs.
11. **Structural limits are parser caps, not iterative walkers** (BUG-1393). An `else if`
   chain is capped at 256 links and an array type at 64 dimensions, because every walker
   recurses per level. Lowering an else-if chain iteratively in each walker would lift the
   cap; nothing in the corpus comes close (the longest chain is 9 links).

## OPEN — residuals of the 2026-09-26 audit (BUG-1326..1357; measured)

1. **BUG-1327 migrated four callers only.** `eval_const_expr_ok` is the out-of-band entry
   point; the ~95 remaining `eval_const_expr` sentinel callers still read INT64_MIN as "not a
   constant" (conservative in every shape probed — they over-reject). The comptime
   interpreter (`eval_const_expr_subst`) and the ident resolver still speak the sentinel.
2. [residual of: **ISR/main RMW of the SAME register through separately formed pointers** — closed by BUG-1358]
   CLOSED
   (BUG-1358) for pointers bound once to a CONSTANT `@inttoptr`. Still unkeyed: a
   register reached through a computed address, or two struct types overlaying one
   address with different field names (the key is address + field-name path).

3. **Bare-metal emission residuals (LOW):** the union-variant reset on a VOLATILE union uses
   `memset` (qualifier cast away; GCC warns); IR blocks of a `@critical` body are emitted
   outside its braces and `goto` back in, so the save variable is formally indeterminate
   (C11 6.2.4) though GCC keeps it in a register; AVR `@critical` names `SREG` without
   `<avr/io.h>` (loud).
4. **Over-rejections seen (loud):** ~~a `const` global read from an ISR and from main is refused
   "must be declared volatile"~~ (CLOSED 2026-09-27, BUG-1369); a function
   ~~returning an `@inttoptr` pointer is reported as a leak~~ (CLOSED 2026-10-12, BUG-1599); `volatile f32` is "not single-word"
   on a 32-bit target.
5. **reference.md "use a packed struct for named bit-fields" is misleading**: `packed struct
   CR { u3 mode; u5 prio; }` lays the fields out in byte CARRIERS (u3 -> u8), not as bits —
   a register overlay built that way puts fields at the wrong bits. Use bit-slices
   `reg[hi..lo]` (documented as such since 2026-09-26).
7. **Emitter residuals from the value audit (measured):** a literal tree needing more
   than 64 bits folds at 64 outside a local var-decl (`x != 18446744073709551615 + 1` on a
   u128 compares against 0; `const u128 C = 18446744073709551615 + 1;` emits 0 — the
   untyped-evaluator entry above); a `uN`/`iN` LOAD on the AST emitter path is not re-wrapped to N bits (the IR path is,
   BUG-1359); a compound assignment reads its target
   AFTER evaluating the right side (`g += setg()` = 11 where `g = g + setg()` = 2) —
   deterministic and now documented in reference.md "Evaluation Order", but the two
   spellings differ; `const [*]u8 T =
   "hello"[2..];` at file scope emits a statement expression GCC refuses (loud); the AST
   emitter path (defer bodies of labelled functions, global initializers) got BUG-1349's
   parentheses but not its object-first evaluation order.
6. **`|*pp|` of a `?*T` then `free(*pp)`** is not linked to the allocation `o` holds by
   zercheck_ir (the emission is correct since BUG-1340); a later `free(a)` is accepted.
   Likewise `*?*T po = &o; if (*po) |q| { free(q); }`.

## OPEN — residuals of the 2026-09-26 memory round (BUG-1360..1355; measured)

2. **A callee store with TWO unnameable indices on one path is refused** (LOW — loud
   over-rejection). `void put(*S s, *T p, u32 i, u32 j) { s.m[i].row[j].p = p; }` — the
   caller's wildcard slot is keyed at the LAST index only, so no read could meet the entry;
   the call site reports it (BUG-1361). Store through one index, or index the outer level
   with a literal.
3. **A struct-valued call whose field views name DIFFERENT params does not resolve as a view
   source** (LOW — same class as the BUG-849 multi-view rule, not yet taught here).
   `unwrap(two(a, b))` where `two` returns `{ .p = a, .q = b }` and unwrap returns one
   field: the result aliases nothing, so it is registered as a fresh allocation (a false
   leak, and a free through it is not attributed). Fix sketch: a multi-view set from the
   union of the ret_field params, as `ir_fill_multiview_set` does for a bare mask.
4. **A callee that frees a field and stores a new value in it marks the caller's field
   freed** (LOW — over-rejection, the pre-existing field-widening coarseness, now also on
   the "maybe" path, BUG-1364). `re(&h); free(h.p);` is refused although `h.p` holds the new
   allocation. The summary does not say what the callee stored after the free; the store
   summary (BUG-1361) only records PARAM values. Fix sketch: record "field f of param i is
   REPLACED by a fresh allocation" and register it at the call site.
5. **A node freed while a field still holds a live allocation is reported at EXIT, not at
   the free** (LOW — wrong position, BUG-1365): `n.p = a; free(n);` says "'a' never freed"
   at the return. And an orelse fallback that returns while such a node is alive now reports
   the field's allocation as leaked there, where it used to be (wrongly) escaped — free the
   node in the fallback, or build the node after the last early exit.

---

## OPEN — residuals of the 2026-09-25f audit (BUG-1308..1325; measured)

1. [residual of: **`lib/` does not compile as shipped** — closed by BUG-1593, BUG-1594, BUG-1595, BUG-1596]
   CLOSED 2026-10-12: fmt imports str, io
   declares its C functions, the module-prefixed C externs (BUG-1593), the `*opaque` C ABI
   (BUG-1594), the const `*opaque` temp (BUG-1595) and the param-view false leak (BUG-1596)
   are fixed, and `tests/test_lib.sh` (in `make check`) builds and runs a program importing
   every module. Imports still resolve from the entry file's directory only, so `lib/` is
   copied beside the program; reference.md documents the library now that it builds.

3. **BUG-1310 precision.** An indirect call through a funcptr LOCAL / param / mutable global
   reaches every function of its SIGNATURE (the BUG-1290 end), so a helper calling a callback
   from a statement holding shared A is refused whenever ANY function of that signature
   writes a second shared type. Per-binding resolution (the spawn scan's local-binding
   walk) would narrow it. The stack-depth analysis still says "calls through function
   pointer with unknown target" for a callee bound in a const global initializer — the same
   `global_bound_functions` query would resolve it.
4. **BUG-1311 is single-key.** A getter returning DIFFERENT globals on different paths
   (`if (c) { return g1; } return g2;`) is still a borrow of nothing at the call site. A
   key SET (the view machinery) would cover it.
5. **`main([*][*]u8 args)` on a freestanding target** has no argc/argv to build from; the
   emitted `main(int, char **)` is only meaningful where a C runtime calls it. Use `u32
   main()` on bare metal.
6. **Seen, not fixed (over-rejections / loud):** a `?*T` parameter returned unchanged is
   reported as a leak (did not reproduce in the simple shape 2026-09-26). The comptime cast /
   const-global / division items and `alloc(u3, n)` are CLOSED (BUG-1328, BUG-1329).
7. **The expression-position deadlock forms** (nested calls, struct-init, orelse, switch
   subject, defer, range-for collection) were only partly probed by the concurrency agent
   before its run ended — no hole found in what ran.

---

## OPEN — what the variable-index slot and struct-wrapper fixes still cannot see (2026-09-23f, MEDIUM — accept-unsafe, narrowed; LOW — leak-silent / over-rejection)

Each item below was MEASURED on the BUG-1130..1133 build (probes: scratch `pr4/`).

1. [residual of: **A slot freed through an index and read back after the index was WRITTEN.** — closed by BUG-1302]
   **Residual (over-rejection):** a WHILE-loop consume (`while (k < n) { … free(tbl[k]) …
   k += 1; }` on a u32 counter) is not recognised as non-wrapping — use a `for`, or reset the
   slot (`tbl[k] = null;`), which is what the diagnostic teaches.

2. [residual of: **An UNTRACKABLE index keeps the BUG-1074 posture.** — closed by BUG-1301]
   CLOSED for pure arithmetic
   index expressions (2026-09-25, BUG-1301): `g[k % 4]` is keyed `[(k%4)]` while no local in it
   is written (`ir_index_is_keyable` / `ir_index_key_text`; the kill checks every name inside
   the brackets). Still unkeyed: an index with a CALL, a global, a field or another index in it,
   an address-taken local, a name two locals share.

3. **A drain exempts the whole array.** Which slots a free-through-a-variable-index loop
   reached is unknowable, so `for (i < 2) { free(arr[i]) }` over a 4-slot array leaks two
   allocations silently; so does handing the array to a callee (`drain(arr)` — needed, else a
   callee that frees every slot is a false leak). Leak-only (never a use-after-free); before
   BUG-1130 nothing stored at a variable index was leak-checked at all.
4. **A leak on an early return INSIDE the fill loop** (`for (i..) { *T a = alloc(T) orelse
   return; arr[i] = a; }`) is invisible: allocation ids are per LOCAL, so the null-edge drop of
   `a` (BUG-1071) also drops the earlier iterations' allocations that share its id.
6. **The dynamic-freed auto-guard's return leaks.** `pool.free(handles[k]); handles[j].f` gets a
   runtime `if (j == k) return;` (checker.c, Handle arrays) that the leak pass does not report,
   although the remaining handles leak when it fires (`tests/zer/dyn_array_autoguard_crash.zer`
   compiles clean and its case 2 takes that return with three handles live).

---

## OPEN — residuals of the 2026-09-23g audit (measured; reproducers inline)

1. [residual of: **A callee frees an allocation through a GLOBAL, the caller reads its LOCAL alias** — closed by BUG-1181]
   residual is closed by BUG-1294 (an indirect call applies every
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

3. **An indirect call widens arena allocations to MAYBE_FREED** whenever any function in the
   program resets a non-local arena (BUG-1172). Sound, but a funcptr call in a function that
   keeps using an arena pointer is then refused even when the target never resets. Precision
   would need the funcptr-binding resolution the spawn scan already has.
5. **The out-param rule (BUG-1173) reports at the function's line** for an implicit return at
   the end of a void function — cosmetic.
6. Over-rejections seen during the audit, not fixed: `u = t; t = u; consume(u); t.k` (t was
   re-initialized); a one-iteration loop that frees reported as a double free; a false leak
   after `set_g(a)`; a spawn target calling `free()` rejected as accessing the non-shared
   auto-slab global.

## OPEN — what remains of the ag10 allocator residuals after BUG-1268..1281 (2026-09-24g; LOW)

All six MEDIUM items of this entry are closed (arena backing ownership 1269/1281, heap backing
free 1268, wrong-pool across a boundary 1270, `@container` through a field 1271, by-value field
free 1272, per-arena reset precision 1268). What is left, each measured on the fixed build:
- **The wrong-pool check is a RUN-time trap, and probabilistic.** Each pool/slab seeds its slot
  generations from its own address (`_zer_gen_seed`), so a foreign handle traps at `get` /
  `free`. Two pools' generations for one slot index can only meet after ~2^31 recycles of that
  slot; a compile-time "which pool does the callee get through" summary would make it exact.
- **The arena backing rule is flow-INSENSITIVE** (LOW, over-rejection): a buffer handed to
  `Arena.over` anywhere in a function may not be mentioned anywhere else in it, even BEFORE the
  `over` (`lb[0] = 1; Arena a = Arena.over(lb);`). Zero corpus cost. A backing store reached
  through a dereference (`Arena.over(*b)`) is refused even when the arena does not escape.
- **A function that turns a param into a backing store consumes the caller's argument even
  when the arena dies in the callee** (`u32 use([*]u8 b) { Arena a = Arena.over(b); … }` —
  the caller's buffer is then unusable after the call). Precision: an "arena escapes the
  callee" test would allow the scratch idiom. Zero corpus cost today.
- **Module scope of the whole-program scan**: consuming params and global backing stores are
  resolved with `global_decl_lookup` outside any module context, so two modules declaring the
  SAME function or global name can confuse the scan (same limitation as BUG-1269's global pass).

## OPEN — residuals of BUG-1331..1336 (scoped-spawn carriers, 2026-09-26; LOW — precision)

The five holes (task `_init` args, a heap payload behind `&carrier`, a global pointer's aim,
a factory-built carrier, a parent `@atomic_*` on a lent object) are CLOSED; see BUGS-FIXED.md.
What they leave:
- **A heap payload is lent by NAME.** An unrooted pointer's own name stands for its pointee
  (`borrow_roots_of_ident`). A second pointer to the same heap object that the checker cannot
  name alike — obtained from a call (`*T q = same(p)` records p's roots only when the call's
  argument walk sees it) or through a field of another object — is not refused in the window.
  zercheck_ir's alloc identity would answer this exactly; the borrow lives in the checker.
- **Factory over-approximation (over-rejection).** `H h = mk();` lends EVERY global whose
  address `mk` (or a function it calls) forms, every global array it names whole and every
  global pointer's aim — not only the one its returned local holds (`ret_local_scan`).
- **A mutable global pointer** whose writes the aim query cannot follow (a call result) also
  lends its own name, so the parent may not name the pointer itself in the window.
- **A pointer local reassigned between two heap objects** now has two roots, so passing it to
  a scoped spawn is refused as "cannot resolve" (was accepted, lending only itself).
- The by-value `spawn w(h)` heap form keeps zercheck's permanent TRANSFER (see the ag9
  over-rejection below); the `&h` form uses the borrow, released at the join.

## OPEN — concurrency residuals of the ag9 round (2026-09-24e; MEDIUM — accept-unsafe races; LOW — over-rejections)

Each reproducer is in the ag9 report shape; none needs `cinclude`.
- [residual of: a shared struct's `*opaque` handle leaving its lock — closed by BUG-1299, BUG-1653, BUG-1657]
  **Over-rejections (loud):** a helper may use the handle only when the call statement
  itself reads the field (`use(r.handle);`); through a local (`h = r.handle; use(h);`) it
  is refused, correctly — no lock is held then. A function-pointer callee is refused (its
  keep summary is the worst case). The type gate is `type_carries_data_pointer`, so a
  pointer-carrying field of a LOCAL struct that also holds the handle is refused with it.

- **LOW (over-rejection, from BUG-1276): a fire-and-forget thread may not write a global inside
  `@once`** — the publish-once exemption needs the parent's window to END, which only a join
  gives. Use a scoped spawn. Also the parent may not call ANY function that reaches the global
  outside that same `@once` during the window.
- [residual of: one object passed as two arguments to a function that lends one of them to a
  thread — closed by BUG-1303, BUG-1646, BUG-1654]
  **Over-rejection (loud), left on purpose:** a nested call result the checker cannot name
  (`f(&a, fresh())`) is treated as possibly the same object as any other pointer argument
  of a LENDING function. The rewrite is one line and is accepted — bind the result to a
  local first (`*T q = fresh(); f(&a, q);`): a local's root is resolved through the same
  query the spawn borrow uses (measured: getter, alias, carrier, reassignment, branch all
  resolve). Teaching the nested spelling the same resolution was examined and not done: a
  pointer read through a PARAMETER (`hp.p`) must stay unknown, and the relaxation would
  rest on distinguishing that case inside a rule whose failure mode is a race. The pairs
  are keyed by POSITION up to 64 params.

- **Over-rejections:** a heap pointer lent to a scoped spawn stays TRANSFERRED after `th.join()`
  (`t.v` / `free(t)` after the join refused, "freed at line <spawn>"); a local Barrier lent to a
  scoped spawn cannot be `@barrier_wait`-ed by the parent; a join followed by a use in the same
  nested block (`while (c) { th.join(); return v; }`); diagnostics that name a TYPE as a global
  ("global 'Msg' is accessed from both interrupt and main").
- **LOW / liveness:** `@sem_acquire` / `@barrier_wait` inside `@critical` are accepted while
  `@cond_wait` there is banned; `@cond_signal` in an interrupt handler emits `pthread_mutex_lock`.

---

## OPEN — async residuals of BUG-1231..1244 (2026-09-24d; LOW — over-rejection / leak-silent)

- **A global task must be declared BELOW its async function** — the global's type is resolved at
  registration, before a later async declaration has introduced `_zer_async_NAME`. Over-rejection
  ("undefined type"); a local task is fine anywhere (BUG-1238 emits every state struct early).
- **A task cannot be a struct / union field, a slice element or a heap allocation** (BUG-1238,
  rejected). A struct is defined before any async state struct; interleaving the two orders
  would lift it. Hold a `*_zer_async_NAME`.
- **`_init` keep is per call, not per lifetime** (BUG-1231). A task reached through a pointer
  makes every pointer argument `keep`, even when the task provably finishes before the frame
  returns; a helper that forwards its own pointer parameter into such an `_init` makes that
  parameter keep, so its caller cannot pass `&local`. Init the task where it is declared.
- **A suspend widens every escaped / global-reachable allocation** (BUG-1232), whether or not
  any code could actually free it between polls. Copy what you need into a local before the
  `yield`, or re-read the global after it.
- **A task that frees its parameter owns it** (BUG-1233): once polled, the caller's entry is
  escaped, so a task abandoned before it reaches the free leaks silently.
- **BUG-1242/1289 resolve a pointer to the global's slot only when it is defined ONCE** (`*?*T slot
  = &gb;`); a pointer retargeted between globals is not minted. (The local-copy and helper forms
  of BUG-1241 are closed by BUG-1288.)
- **`await gs.flag == 1` on a `shared struct` is refused** ("lock would be held across
  suspension"). The condition is evaluated inside ONE poll, so a lock around just the
  condition would be sound; the emitter does not lock an await condition yet, so the ban
  (BH-18 #9) stays. Spell it as a loop that re-reads the field in its own statement:
  `while (true) { u32 f = gs.flag; if (f == 1) { break; } yield; }`.
- **The `_result` early-read trap reports a line of the generated C**, not the `.zer` call site
  (the accessor is a C inline function).

---

## OPEN — allocation-tracker residuals of the 2026-09-24c round (LOW — leak-silent / wrong reason)

- **A factory-returned struct's allocations are not leak-checked.** `S mk(){ S r; r.p =
  alloc(T); return r; } ... S a = mk();` discarding `a` compiles (reproducers `h1`..`h3`,
  `g4c` in the ag6 set). BUG-1227 records a FREE of such a field (so a double free / UAF is
  caught), but the entry is escaped: the summary has no per-FIELD "returns a fresh allocation"
  fact. Fix sketch: `FuncSummary.returns_field_alloc` — the compound paths of the returned
  struct local that are ALIVE owned allocations at every return — registered on the call's
  dest temp and carried by `ir_carry_compounds`.
- **An out-parameter's allocation is not leak-checked** (`fill(&m)` then drop `m`): BUG-1228's
  entry is escaped for the same reason — no summary says the callee stored a FRESH allocation
  through `*out`. Same fix shape (`stores_alloc_through_param`).
---

## OPEN — a qualified reference to the SECOND module's declaration of a shared name (2026-09-24, LOW — over-rejection; was a silent miscompile before BUG-1200)

Two imported modules c and d both declare `get`. `c.get()` (c registered first) works;
`d.get()` is REFUSED ("a qualified reference to the SECOND declaration of a shared name is not
supported yet"), and so is the bare `get()` from code that owns neither. Before BUG-1200 both
compiled and silently reached c's (`d.shared_name = 50` wrote c's global).

Why it is refused rather than resolved: the qualified form is rewritten to the bare ident,
and every later stage answers "which symbol is this?" BY NAME — the checker's ~130
`global_decl_lookup` sites, the VRP range keys, zercheck_ir's global keys `(-2, "name")`,
BUG-1181's `freed_global[]` summaries, and the emitter. Resolving `d.get` correctly in the
checker alone would give d's global TWO keys (`get` inside d, something else in main), so one
object's facts would split across them — a range proven on one read and an index through the
other, a free recorded under one and a use under the other. That is unsound, so the honest
answer until the naming is unified is the refusal.

Fix sketch: give every top-level declaration ONE canonical key — the module-mangled name
(`d__get`) — at the ident level (a `module` field on NODE_IDENT written by the qualified
rewrite AND by lookup inside the owning module), and key every by-name table (VRP, zercheck_ir
globals, freed_global, the ISR / atomic-cell / race tables) on it. Then the refusal is removed
and `d.get()` resolves. Tests to flip: `test_modules/m1200_qual_negative.zer`.

Also over-rejected by the same rule: main imports only `out1`, `out1` imports `in1`, both
declare `nm`; main's bare `nm` is refused as ambiguous although main never imported `in1`
(declarations are program-global today, so which one main "sees" is exactly the question the
rule refuses to guess).

---

## OPEN — two modules declaring a container template of the same NAME (2026-09-24, LOW — loud)

`container Box(T)` in module cx1 and a different `container Box(T)` in module cx2: the template
table and the stamped `Box_u32` are program-wide, so cx2's `Box(u32)` resolves to cx1's layout
and cx2's own body then fails to check ("array index 7 is out of bounds for array of size 2",
"no field 'n'"). Loud, never silent — the second module's code is checked against the first
layout and refused. Fix: key container templates (and their stamps) by module like struct types
(`module_prefix` on the stamped struct + a per-module template lookup). Reproducer:
`/tmp`-style pair — cx1.zer `container Box(T) { T[2] d; } Box(u32) b1;`, cx2.zer `container
Box(T) { T[8] d; u32 n; } Box(u32) b2; u32 f2() { b2.d[7] = 4; return b2.d[7]; }`.

---

## OPEN — `@probe` cannot return null on a freestanding target (2026-09-24, FLOOR — documented)

`@probe(addr)` recovers from a fault with `signal()` + `setjmp()`. A freestanding build has
neither, so the emitted probe is a direct read: a faulting address takes the CPU fault (HardFault,
bus error) instead of yielding null. reference.md used to say "works on any platform". This is a
platform floor, not a checker gap — recovering needs a target-specific fault handler that knows
the faulting instruction's length (Cortex-M: stacked PC in the exception frame, advance past the
LDR, set a flag). Fix sketch if wanted: a `--probe-handler=cortex-m` mode emitting a HardFault
handler + a volatile `_zer_in_probe` / `_zer_probe_faulted` pair, the same shape as the hosted
path, with the PC advance done in asm. Until then the docs say what happens.

---

## OPEN — atomic-cell diagnostics for an imported module's global name the wrong file (2026-09-24, LOW — diagnostic only)

`check_atomic_cell_safety` runs after every body is checked and reports at the recorded LINE
of the plain access under the CURRENT file name (main's). A plain access inside module m is
therefore printed as `main.zer:<m's line>` (measured: `test_modules/m1199_atomic_negative.zer`
reports line 10, which is in `m1199st.zer`). The verdict is right. Fix: record the file with
the line (`AtomicFieldEntry.plain_line` and the scalar twin) and switch `c->file_name` around
the report, as `check_interrupt_safety` now does (BUG-1199).

---

## OPEN — precision residuals of BUG-1177..1186 (2026-09-24, LOW — over-rejection; each is the conservative side of a new rule)

1. **An async task cannot be copied even BEFORE its first poll** (BUG-1177). A never-polled task
   holds no self-reference, so the copy is safe; the rule is type-level because ZER has no
   "not yet started" fact on a value. Corpus cost zero. Fix sketch: a per-task flow fact
   "polled" (reset by `_init`), with the copy refused only once it may be set.
2. **Re-pointing an arena at a DIFFERENT buffer invalidates what it handed out** (BUG-1178).
   `a = Arena.over(b2)` after allocations from `Arena.over(b1)` is safe — `b1` still holds
   them — but whether the two backings overlap (sub-slices of one buffer, a loop re-overing
   the same one) is not decidable in general, so any re-init is treated as `a.reset()`. Read
   the old allocation's data into a local before the re-init (what p21_safe_fresh does).
3. **The union-capture callee rule is TYPE-based** (BUG-1186). A callee that assigns a variant
   of ANOTHER `U` instance (a queue of messages of the same union type) is refused inside a
   `|*w|` arm, as is any call through a funcptr. Precise alternative: a per-callee summary of
   WHICH roots it writes (the BUG-1181 key machinery), with a pointer argument's pointee
   treated as a root. Remedy today: capture by value and write back after the switch.
4. **A variant-capture pointer assigned to a local DECLARED IN THE ARM** (`*P q = w; … q = w;`)
   is refused like an escape: the store sink does not ask where the target was declared.
5. **BUG-1181 is flow-insensitive** on the callee side: a local that is EVER loaded from `g` is
   treated as possibly holding `g`'s allocation at every free of it.

## OPEN — a forward `goto` fires EVERY pending defer, not only those of the scopes it leaves (2026-09-24, LOW — semantics / over-rejection, memory-safe)

`defer x = 77; if (k == 0) { goto out; } out: return x;` returns 77 on the goto path and the
original value otherwise, and `*T p = alloc(T)…; defer free(p); if (e) { goto out; } out:
return p.v;` is REFUSED (use after free) although the label is inside the defer's scope. The IR
(`NODE_GOTO` in ir_lower.c) fires everything above `fire_base = 0` for a forward goto and
guards the label's return-fire with a runtime flag (plt86m). CLAUDE.md and
`tests/zer/goto_defer.zer` / `rust_tests/rt_goto_fires_defer.zer` rely on "fire all";
reference.md used to state the scope-based rule (C++/Zig: a jump runs the defers of the scopes
it LEAVES) and now documents the implemented one. Memory-safe since BUG-1540 (2026-10-10):
before it, two forward gotos to two DIFFERENT labels fired the defer a second time at the
return (a double free), which this entry's old "memory-safe either way" missed; a goto now
disarms each defer it fires. zercheck sees the DEFER_FIRE on the goto path. Fix sketch if the scope-based rule is adopted: `fire_base` = the
defer count live at the label's lexical position (known at the LABEL for a backward goto; for
a forward goto the label's enclosing-scope defer depth is known from collect_labels), plus
the label's guard flag keyed per fired range.

## OPEN — the preamble includes `<string.h>`, `<stdio.h>`, `<stdlib.h>` unconditionally (2026-09-23, LOW — loud)

C99 4.6 guarantees only the freestanding headers; a `-nostdinc` bare-metal toolchain fails on
`string.h`. Every mainstream bare-metal toolchain (newlib, picolibc) provides them, and the
emitted code uses `memcpy`/`memset` in both modes, so the fix is to gate stdio/stdlib on
`_ZER_HOSTED` and route mem* through `__builtin_memcpy`/`__builtin_memset`.

## OPEN — one RMW-reach residual after BUG-1046 / BUG-1129, and one masked (2026-09-23, LOW — wording + latent)

**1. ~~A carrier that points at TWO globals binds the first only~~ — CLOSED (BUG-1129).**
`carrier_value_globals` enumerates every field of a struct literal and both orelse arms; the
scans' alias table and the main-side carrier table hold several rows per name (a rebind
clears, a projection assignment adds). 4 RMW-grid cells were holes pre-fix (ISR and MAIN
sites). **Residual (wording only):** carriers are keyed by the ROOT name, so every field of
`h` designates every global `h` holds — at the SPAWN site `*h.q += 1` with `h = { .p = &g1,
.q = &g2 }` is refused (correctly) but the message names `g1`. Field-keyed rows (the
atomic-cell path's compound keys) would make the sentence exact.

**2. ~~funcptr FIELD callee in a later-declared spawn target~~ — CLOSED (BUG-1052).**

**3. A GLOBAL funcptr rebound in another function (latent, MASKED — measured unreachable).**
ISR "facet 1" and the spawn funcptr descent follow `*() gcb = nop;`'s initializer only, so
`main() { gcb = bump; }` is not followed. MEASURED 2026-09-23: no spelling reaches it — a plain
global funcptr touched from both contexts is refused as non-volatile / non-shared, and a
`volatile` one as "not a single-word scalar". If either rule is ever relaxed, extend the
descent with `ast_name_writes` over every body (the BUG-1124 mechanism) in the same commit.

## OPEN — a PROVEN struct-field index still emits its runtime bounds check (2026-09-22, LOW — precision only, no safety consequence)

**Symptom.** Measured while ruling out a VRP-alias candidate:

    struct S { u32 i; }
    u32 main() { u32[4] arr; S s; s.i = 1; if (s.i < 4) { arr[s.i] = 7; } ... }

emits `_zer_bounds_check((size_t)(s.i), 4, ...)` although the checker accepted the access
without a diagnostic, while the scalar spelling (`u32 i = 1; if (i < 4) { arr[i] = 7; }`)
elides the check. The compound-key range (`"s.i"`) is pushed by the guard, but the
proven-mark / guard-lowering path on the IR side keys the access differently, so the
emitter's default (check) wins. Division is not a sibling: the runtime `division by zero`
trap is emitted for EVERY non-constant divisor, scalar or field, proven or not — that is
belt-and-suspenders by design, not a field-key gap.

**Why it is not a hole.** The direction is a redundant check, never a missing one. It is
why every `*u32 p = &s.d; if (s.d != 0) { *p = 0; ... / s.d }` alias probe this session
was safe: the trap fired.

**Fix sketch.** Find where `mark_proven` is consulted for NODE_INDEX on the IR emission
path and confirm it looks up the compound key (`build_expr_key_a`) the checker pushed, not
only a bare ident; then A/B the emitted C for the two spellings above. Measure the corpus
delta in emitted `_zer_bounds_check` count before and after.

## OPEN — AST-path container-method receivers are not module-prefixed (2026-09-21, LOW — reachable only from a global initializer since BUG-1298)

BUG-1040 prefixed the IR-path receiver (`emit_builtin_inline`) and the declaration arms. The
AST emitter's arena / pool / slab method arms (`emit_expr` NODE_CALL, `aname` / `sname`)
still spell the raw name. That path now runs only for global initializers (where only
`Arena.over` — no receiver — is legal); defer bodies in functions WITH a label go through the
IR emitter since BUG-1298. Fix, if a receiver ever becomes legal there: the same
local-vs-global mangling as the IR receiver; the AST path has no `IRFunc`, so the local
check must use the checker's scope instead.

---

## OPEN — the constant evaluator is UNTYPED, and its "not a constant" sentinel is a real value (2026-09-23b, LOW — over-rejection in every shape probed)

**Symptom.** Exactly one value of each 64-bit type is not a constant to the checker:

    i64 m = -9223372036854775807 - 1;   // error: cannot initialize 'm' of type 'i64' with 'u64'
    const u64 D = 9223372036854775808;
    u64 q = x / D;                      // error: divisor 'D' not proven nonzero
    u64 d = 9223372036854775808;
    u64 r = x / d;                      // error: divisor 'd' not proven nonzero

while `i64 m = 0 - 5000000000;`, `const u64 E = 9223372036854775809;` and `u64 d = 7;` are all
accepted.

**Root cause.** `eval_const_expr` (ast.h) computes in signed int64 and returns
`CONST_EVAL_FAIL`, defined as `INT64_MIN`, for "not a compile-time constant". Its comment
says INT64_MIN "won't appear in real constant expressions" — but it is i64's MIN and the bit
pattern of u64 2^63. The literal-tree retype (`int_literal_tree_fits`) sees the MIN fold as
a failure and leaves the tree u64-typed; the divisor proof and the var-decl VRP range see
2^63 as "unknown". Every such site falls back to its conservative answer, so every shape
probed OVER-rejects; none accepted anything unsafe.

**Why not patched at the three sites.** A `NODE_INT_LIT`-only special case would cover
`x / 9223372036854775808` and leave `-9223372036854775807 - 1` (a tree) broken. The defect
is the API: a value and a failure flag share one int64.

**Fix sketch.** `bool eval_const_expr_ok(Node *, int64_t *out)` (or a small struct with a
`uint64` payload and a signedness bit) beside the old entry point; migrate the ~150 call
sites (140 in checker.c, 13 in emitter.c) mechanically, the divisor proof, the var-decl
VRP init and `int_literal_tree_fits` first. While there, fold at the literal's WIDTH and
SIGNEDNESS (`fold_wrap_to_type` already does the wrap for the file-scope case) — that is
what BUG-1018's over-rejection narrowing above needs.

---

## OPEN — float division by zero follows the INTEGER rule (forced guard + runtime trap), not IEEE (2026-09-23b, LOW — a language decision, not a defect)

**What is true today.** `f64 x = 1.0 / y;` needs a nonzero proof, and an unproven one still
emits `if (_zer_dv == 0) _zer_trap("division by zero")`, so `1.0 / 0.0` halts rather than
giving `+inf`. Since BUG-1067 the proof IS writable — `if (y == 0.0) { return; }` or
`if (y != 0.0) { ... }`, and the diagnostic prescribes `== 0.0` for a float. Before it, no
guard could prove a float divisor (the prescribed `y == 0` is a type error on an f64), so
dividing by a float variable could not be written at all; that half was a bug and is
closed.

**Why the rest is recorded, not changed.** IEEE 754 defines float division by zero
(+-inf, NaN), and ZER's own float rule is "arithmetic gets a defined value" (reference.md
f32/f64 notes; float->int saturates). But the documented division rule — reference.md
"Division by zero | Forced guard — compile error if divisor not proven nonzero" — does not
exempt floats, and dropping it is a relaxation (reject -> accept, plus removing a runtime
trap). Two consistent answers exist: keep the guard (a zero divisor is almost always a bug,
and the trap names the line), or follow IEEE (no proof, no trap; `inf` flows on and
saturates if later converted to an integer). The owner's call. If adopted: the checker gate
is the `TOK_SLASH`/`TOK_PERCENT` arm of `check_expr` NODE_BINARY and the compound arm; the
emitter zero tests sit beside each of the seven `signed_min_text` call sites.

**2026-09-23 (BUG-1090) — the rendering split now has a SAFETY consumer, and is handled
at it.** VRP trusts constants, so it must know which rendering RUNS. There are THREE, not
two: the 3AC path (var-decl init, conditions, returns, compound-assign RHS) computes each
node in its typed temp; a PLAIN assignment `x = <tree>` and the index arm are one C
expression with bare-`int` literals; and a defer body in a function with a label went
through the AST emitter (the same bare-literal rendering — until BUG-1298, which emits it
from IR; the conservative defer-body rule below is kept, it only costs precision). The typed fold (`tfold`)
models the first. The plain-assignment sink and any defer body trust only a constant for
which all three agree (`tfold_exact`: every intermediate exact, fits `int`, equals the
typed value) — measured necessary: trusting the typed value there let
`i = (0 - 1) % 7; arr[i]` write arr[0xFFFFFFFF]. The fix sketch above (one rendering
function of (literal, type)) is still the durable end-state; it would let the
plain-assignment sink use the typed fold directly.

---

## OPEN — untyped constant folds that remain at compile-time-only decisions (2026-09-23, LOW)

BUG-1090 routed every VRP ELISION sink (a range that removes a runtime check) through
the typed fold. These still fold with the untyped int64 `eval_const_expr*`:

- the compile-time division-by-zero proof (checker.c NODE_BINARY `/` `%`, compound
  `/=` `%=`) — the IR and AST emitters ALWAYS emit the runtime zero-divisor trap, so a
  wrong proof cannot remove a check; it can only mis-report (`x / ((0-1)/1073741824 - 3)`
  divides by 0 at run time and traps, with no compile-time error);
- `comptime if` / `static_assert` conditions and array / Pool / Ring / Semaphore sizes —
  checker and emitter use the SAME untyped value, so the program is self-consistent, but
  `comptime if ((0 - 1) / 1073741824 == 3)` takes the branch the untyped reading picks,
  which disagrees with the same expression at run time;
- the asm NONZERO / BOUNDED operand constraints (naked-only asm).

Fix sketch: `vrp_const_value` at the division proof (typed first, untyped fallback — the
error then fires on the TRUE zero); a typed fold for comptime-if needs the condition's
typemap, which the dead branch never gets, so it must fold before the branch is chosen.

## OPEN — a VRP range cannot hold a u64 at or above 2^63 (2026-09-23, precision only)

`VarRange` is `int64_t`, so `tfold` carries a u64 as its bit pattern and
`vrp_const_value` REFUSES a result that reads negative (`u64 i = 0 - 1;` gets no range).
Refusing only drops elision; no check is removed. Fix sketch: an unsigned flag on the
entry, or `uint64_t` bounds for unsigned roots.

## OPEN — a hoist-unsafe index gets a TRAP, not the early-return guard (2026-09-23, behavior note)

BUG-1098 gives an index the statement can change before reading (`g() + arr[gi]`, `g(&i)
+ arr[i]`) the inline single-read check, which TRAPS on failure, instead of the hoisted
auto-guard, which returns early. Both are safe; the difference is observable (exit 133
instead of a silent early return). An IR-level guard placed immediately before the
access (after the call) would restore the early-return form — that needs the guard
lowering to emit at the ACCESS's instruction, not the statement's start (refactor M
stage C). Corpus cost of the current form: zero files (no test relies on it).

---

## OPEN — `@cond_timedwait` on a freestanding build reports a raw C message (2026-09-15, LOW — message quality only)

**Symptom.** A bare-metal build of a program using `@cond_timedwait`:

    out.c:22:167: error: storage size of '_zer_ts0' isn't known

The feature genuinely cannot exist freestanding — it needs `struct timespec` and
`clock_gettime` — so refusing is right. What is wrong is that the refusal names a
temporary in generated C rather than the ZER feature and the reason. Same defect
BUG-1022 fixed for `Barrier` / `Semaphore`, at the one sibling it did not reach.

**Why it was not fixed with the others.** BUG-1022's trick is a `#define` in the
non-hosted branch that puts the reason into the type NAME GCC prints. That works
because `_zer_barrier` is a plain typedef name. `struct timespec` is a struct TAG
used inline at two emission sites, and GCC's "storage size isn't known" prints the
VARIABLE, not the type, so redefining the tag does not improve the message.

**Fix sketch.** Emit a guarded `#error` at the two `@cond_timedwait` emission
sites (`emitter.c` ~4301 and ~10545) — emitted only where a timedwait actually
occurs, so a freestanding program that does not use one is unaffected, exactly as
BUG-1022's macros are. The mechanical part is that both sites emit mid-expression
(`({ struct timespec ... `), so the directive has to be written on its own lines
before the statement expression opens.

**Measured**: 38 files in `rust_tests` hit this on each `bare` row of
`tools/cross_target_sweep.sh` (the Barrier/Semaphore floor plus these); all
`hosted` rows are 544/544 clean on every target.

---

## OPEN — the checker's fits-check on a `const` GLOBAL initializer uses the UNWRAPPED fold (2026-09-14, LOW — over-rejection, BUG-1032 residual)

`const u32 A = 4000000000; const u32 AA = A + A;` is refused with "integer literal 8000000000
does not fit in 'u32'", and `const u8 B = 200; const u8 P = B + 100;` with "300 does not fit in
'u8'" — while the same expressions in a function body wrap (3705032704 and 44) and, since
BUG-1032, the EMITTED file-scope value wraps too. The checker's literal-fits check folds the
initializer with the untyped int64 evaluator and compares the unwrapped result against the
declared type. Not a soundness hole (a wrapping const init is refused, never mis-valued), and
arguably a useful overflow diagnostic — but it is inconsistent with the language rule that
integer arithmetic wraps. Fix sketch: wrap the folded value to the checker's type of the
initializer EXPRESSION (the emitter's `fold_wrap_to_type` twin) before the fits comparison, or
keep the rejection and document it as an intentional const-init overflow check. Decide, then
make the two agree. Tripwire: `tests/zer/global_const_fold_wraps_bug1032.zer` covers the
shapes that DO compile.

## OPEN — a struct whose SLICE field holds VALUE optionals of itself, `struct T { [*]?T kids; }`, is a GCC "unknown type" (2026-09-14, LOW — loud, valid program refused)

`[*]?T` needs the typedef `_zer_xslice_o_<T>` whose `ptr` field is `_zer_opt_T*`, and
`_zer_opt_T` is an ANONYMOUS-struct typedef emitted after `struct T` — so it cannot be named
before `struct T` is complete, and the field inside `struct T` needs it first. The pointer form
`struct T { [*]?*T kids; }` compiles (a `struct T*` may be incomplete; `xslice_deps_ready`
knows this). Fix sketch: give the optional typedefs a struct TAG (`typedef struct _zer_opt_T_s
{...} _zer_opt_T;`) and forward-declare `struct _zer_opt_T_s;` from the exotic-slice flush, so
a pointer to it can be named before the definition. Measured: GCC error (`unknown type name
'_zer_xslice_o_N4_Task'` — the registry never flushes the slice typedef because its
dependency, `struct Task`, is the struct being defined), never a wrong program.

## OPEN — `@inttoptr` to a POINTER-carrying (not enum-carrying) pointee is not refused (2026-09-13, LOW — unmeasured tightening, deliberately unshipped)

BUG-989 rejects `@inttoptr` to a type that carries an ENUM, because an exhaustive switch
downstream *elides work* on the assumption that every value is a declared variant, and
that elision was measured turning a bad value into a wrong dispatch.

The same intrinsic can produce a pointer to a struct carrying a `*T`, `[*]T`, `bool`,
optional or `Handle`, and reading those fields forges those values from hardware bits.
That was NOT shipped, deliberately:

- `@inttoptr` **is** the sanctioned integer-to-pointer door (mmio-gated and audit-visible),
  so a hardware register holding an address is the thing it exists to express;
- `lib/compat.zer` depends on `@inttoptr(*opaque, ...)` for its pointer arithmetic, so the
  blanket predicate (`type_carries_forgeable`) has a non-zero corpus cost;
- no wrong-dispatch or wrong-elision defect has been measured for these, unlike the enum
  case.

If this is taken up, measure first: find a downstream analysis that ELIDES a check on the
strength of one of these types, the way the exhaustive switch does for enums. Absent that,
this is an unmeasured tightening and should stay unshipped. (Carried from `qo0mm9` /
`vigilant-tesla-ef9cao`.)

## NOTE — the exhaustive-enum switch's last-arm elision is the AMPLIFIER, not the hole

Worth writing down because it explains why every enum-door bug reads as severe. Lowering
an exhaustive `switch` emits the final arm as an **unconditional else**:

    if (s == 0) -> arm0; else if (s == 1) -> arm1; else -> arm2;   /* no test on arm2 */

That is sound exactly while every enum value is a declared variant — which is what the
forge doors defend. So a missed door does not merely let a strange value through; it makes
that value *take an arm*, and the `return 77` fall-through after the switch becomes dead
code. Every enum-forge bug to date (BUG-843, 864, 891, 910, and now 988/989) reports as
"the switch silently ran its LAST arm" for this reason.

Making the switch defensive (test the last arm too, fall through on no match) would remove
the amplifier permanently and independently of door coverage. It was NOT done: it costs a
comparison and a branch on every enum switch, it silently does nothing on a forged value
rather than trapping, and ZER's chosen answer is to trap at the point of forgery. Recorded
as the alternative in case the door set ever stops being closable.

---

## OPEN — a scoped-spawn carrier holding pointers into TWO locals is refused (2026-09-13, LOW — over-rejection, deliberate)

**Symptom.** `struct H { *u32 p; *u32 q; } H h; h.p = &v; h.q = &x; ThreadHandle t = spawn w(h);`
is refused: *"spawn argument 1 references locals whose identity the compiler cannot
resolve (a carrier holding pointers into two different locals)"*.

**Why.** BUG-969's `Symbol.borrow_root_name` records ONE root per carrier — the last
field write wins — so before BUG-1014 the carrier lent only `x`, and a parent write to
`v` before the join raced unseen (measured). Cannot prove ⇒ reject; the escape is to pass
`&v` and `&x` as separate arguments (each is lent), or copy the data by value.

**Fix sketch.** Make the root a small stack-first list (`borrow_roots[]`, arena on
overflow) filled by `record_borrow_root` and drained into `cand_n[]` at the spawn sink —
the sink already lends several names per argument (`th_borrow_names`), so the only
missing piece is the carrier remembering more than one. Gate: the reproducer above as a
POSITIVE plus `v = 1;` / `x = 1;` before the join as two negatives.

**Tripwire:** `tests/zer_fail/spawn_borrow_two_roots_unknown.zer` (pins the refusal).

---

## OPEN — re-unwrapping an optional after `p = null;` following a free is refused (2026-09-13, LOW — over-rejection, unchanged from before BUG-985)

**Symptom.** `*T q = mp orelse return; free(q); mp = null; *T r = mp orelse return;` is
refused as a use-after-free of `mp`. At runtime the second unwrap takes the null path; the
freed pointee is never touched.

**Why.** BUG-985 made `mp = null;` a RESET (not a use) but deliberately KEEPS the FREED
state on `mp`, so that the FuncSummary builder still sees the free when `mp` is a param
(`free(q); p = null;` in a callee frees the caller's allocation). The slot form has a
separate fact for this (`freed_then_reset`); the bare-ident form reuses the state itself.

**Fix sketch.** Give the bare ident the same treatment as the slot: on `mp = null;` set
`freed_then_reset` from the FREED state and move the state to a "known null" value the
orelse read treats as the null path; the summary reads the flag. Corpus cost of the
current over-rejection is zero (measured, 2379 files). (The if-capture form
`if (h.p) |q| { free(q); }` that used to stay MAYBE at the caller is CLOSED by BUG-1071:
the null edge of an optional test drops the optional's entries, so the callee's join is
FREED — `tests/zer/opt_param_capture_form_frees_ok.zer`. Re-unwrapping the field after the
callee's `h.p = null;` is still refused, the same residual as the bare-ident form above.)

**Tripwire:** none pinned — freezing a wrong "use after free" into `zer_fail` would freeze
the wrong reason.

---

## OPEN — freeing a UNION variant through its POINTER capture `|*q|` is a false leak (2026-09-13, LOW — over-rejection; the value capture `|q|` is CLOSED as BUG-984)

**Symptom (measured on main after BUG-984).**

    U u;  u.p = alloc(T);
    switch (u) { .p => |*q| { *T t = *q orelse return; free(t); }  .n => |n| { } }
    // zercheck: handle %0 (local 'u') allocated at line 5 but never freed

The value capture (`|q|`, a read of `sw_ref.p` off the hoisted `&u` view) aliases the
variant slot since BUG-984 and is accepted. The pointer capture is `&sw_ref.p` — a pointer
TO the slot — and the later `*q` deref read is not routed through the slot's compound, so
the free never reaches `(u, ".p")`.

**Fix sketch.** Treat `*q`, where `q`'s entry records "address of slot (root, path)", as a
read of that slot: give the `&<projection>` view arm a slot-address form of
`view_root_local` (root + path), and let the DEREF read arm re-root through it. Gate: the
program above as a positive, plus `free(t); *T t2 = *q orelse return; t2.v` as a negative.
Not pinned as a `zer_fail` — that would freeze a WRONG diagnostic ("never freed" on a
program that frees) into the suite.

(Carried from `claude/loving-davinci-3sdup9`'s own OPEN entry, re-measured here.)

---

## OPEN — a `shared(rw)` READ statement cannot call a function that also READS it (2026-09-11, LOW — over-rejection)

**Symptom.** `u32 r = g.v + f();` where `g` is `shared(rw)` and `f` only READS `g.v` is
refused with "a 'shared(rw)' lock is not re-entrant". Both accesses take a READ lock, and
POSIX lets one thread hold several concurrent read locks.

**Why it is refused anyway**, both measured rather than argued:
1. POSIX leaves it UNSPECIFIED whether a recursive `rdlock` succeeds while a writer is
   waiting. glibc's default is reader-preferring — and a zero-initialised
   `pthread_rwlock_t`, which is what the emitter produces, IS the default — so it returns
   0 today. Under glibc's documented `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` it
   deadlocks. Accepting it makes correctness depend on an attribute ZER does not set.
2. **The rule cannot see whether the callee reads or writes.** `FuncSharedTypes` records
   TYPE IDS only. "The callee only reads it" is not a fact available at the check, and
   guessing it is the direction that ACCEPTS — which is how the write/write case
   (BUG-980, silent corruption) would come back.

**Fix sketch.** Give `FuncSharedTypes` a parallel `writes[]` bit per recorded type, set in
`scan_body_shared_types` when the shared field access is an assignment target. Then exempt
direct-read + callee-read-only, mirroring the `both_rw && is_read_only` exemption the
two-type deadlock rule already has. Note that rule's exemption is a STATEMENT-LEVEL
heuristic (`stmt->kind != NODE_ASSIGN`) and does not know the callee either, so the
writes-bit would improve both.

**Corpus cost: zero** — measured over 2341 files, 17 of which use `shared(rw)`. The escape
is one line: read into a local first.

**Tripwire:** `tests/zer_fail/shared_rw_reentrant_read_read_overreject.zer`.

---

## OPEN — a plain struct nested deeper than 64 cannot be COPIED (2026-09-11, LOW — over-rejection, valid program refused)

**Symptom.** `H100 b = a;` where `H100` is a plain by-value struct nest 100 levels deep is
refused: *"cannot initialize this value — the type nests deeper than the resource walk
follows (64)"*. Nothing in it is a Pool / Slab / Ring / Arena / Barrier / Semaphore.

**Root cause, and why the fix is not "raise the cap".** `unique_resource_name` (checker.c)
walks a type looking for a unique-resource field. BUG-976 made it round toward reject past
its cap, which is correct — a fail-open answer there means a resource is copied and two
owners hand out the same bytes. But struct nesting has NO syntactic limit, so any finite
cap is reachable and the reject is permanent for types past it. Raising 8 -> 64 already
bought everything real; going further just moves the boundary.

**What was fixed and what was not.** BUG-977b fixed the DIAGNOSTIC — past the cap the walk
now returns a sentinel and the reporter says the walk stopped, instead of naming a
fictional resource type called `"resource"` and telling the author to make it a global.
The over-rejection itself stands, deliberately: it is the conservative direction, corpus
cost is zero (measured over 1534 files), and 64 levels of by-value nesting is not a shape
anyone writes.

**If it ever matters**, the real fix is a memoised walk keyed on `Type *` so the cap can go
away entirely — the type graph is finite and acyclic (the checker rejects self-containment
and a by-value field cannot forward-reference), so a visited-set walk always terminates and
needs no depth bound at all. That is the durable end-state for every one of these type
walks, not just this one.

**Tripwire:** `tests/zer_fail/resource_walk_stopped_100_deep.zer`.

---

---

## OPEN — WASM CLI: multi-file imports + macOS terminal zerc (LOW–MEDIUM; NOT re-measured 2026-11-14)

> **Carried over from the retired legacy ledger without a measurement.** Verifying it needs
> the emscripten build of `zer_wasm.c` and the editor bridge, neither of which is available
> in the environment the 2026-11-14 triage ran in. Treat it as a hypothesis until someone
> runs the wasm CLI on a two-file program.

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
