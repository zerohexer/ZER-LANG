# λZER-Concurrency — operational subset DESIGN (the Level-1 oracle that derives the tracking set)

**Status: DESIGN ONLY (no Coq yet). This file is the scope/spec.** It exists so the
concurrency *tracking set* (what the compiler must track to be data-race- and
cross-thread-UAF-safe) is **derived from a proof, not guessed** — locking the
architecture before any compiler code or Coq is written, per the "infinite time,
do not re-change architecture" decision (2026-06-20).

Supersedes, for the **memory-safety** rows, the schematic
`lambda_zer_handle/iris_concurrency.v` (every C/D/E lemma there is a trivial
`True` closure — no operational content). This subset is roadmap **priority 4**
("λZER-concurrency C+D+E, ~25 rows, 100-200 hrs, the hardest" — proof-internals.md).
It is the **first** subset to use Iris's *concurrent* threadpool semantics; all
prior subsets (handle/move/escape/mmio/opaque) are sequential single-step.

Companion (the engineering side / hole inventory): `docs/primitives-data-races.md`
§24 (the four-axis closure) and `docs/limitations.md` "## OPEN — Concurrency
memory-safety". Read those first for *why*; this file is *the formal what*.

---

## 0. The claim being proved (the exhaustiveness theorem)

> A λZER-concurrency program that types under the four-condition judgment is
> **data-race-free and free of cross-thread use-after-free** — for all schedules.

Concretely (Iris adequacy over the threadpool): a typed program has **no stuck
configuration**, where "stuck" is defined to include the two memory hazards:
- **DR-stuck:** two threads simultaneously hold conflicting access (≥1 write) to
  the same location without a common lock — *data race*.
- **UAF-stuck:** a thread accesses a location after it is freed / its frame died —
  *cross-thread use-after-free*.

Everything else (deadlock, livelock, lost-update *logic*) is **not** a stuck
state — it is the named liveness floor, deliberately outside this theorem (same
boundary as RustBelt: a `Mutex` deadlock is "safe").

## 1. Why four conditions — the necessary-condition decomposition (the spine)

A DR-stuck or UAF-stuck configuration requires ALL of:
1. **reach** — two threads reach the same mutable location;
2. **¬discipline** — ≥1 access is unsynchronized  [DR], OR
   **¬lifetime** — the location is dead/freed while still reachable  [UAF];
3. **visibility** — the analyzer must SEE the threads/locations to check 1–2.

The subset negates each with a piece of ghost state / invariant (below). Negate
all four ⇒ the stuck configuration is *unreachable in the logic* ⇒ adequacy gives
safety. The four pieces of ghost state ARE the minimal tracking set — that is the
whole point of doing this in Iris: **the proof obligations derive what to track.**

## 2. Operational semantics shape (`syntax.v` + `semantics.v`)

Minimal concurrent λ-calculus, Iris-`language` instance with a **threadpool**
(unlike the sequential subsets). Expression forms beyond the handle subset:
- `Spawn e` — forks a thread evaluating `e`; steps to a unit and appends `e` to
  the threadpool (`prim_step` returns a non-empty `efork` list — the standard Iris
  fork). Optionally `ScopedSpawn e` returning a join token (§4.3).
- `Join h` — consumes a join token; blocks until the forked thread is a value.
- Shared heap: locations `ℓ` carry a `share : Local tid | Shared`. Two access
  forms: `LocalLoad/Store ℓ` (only when `Local self`) and `LockedLoad/Store ℓ`
  (opens the shared invariant). `Alloc`, `Free ℓ`.
- `Atomic op ℓ` — a single logically-atomic step on a `Shared` integer cell.
- `OpaqueCall f args` — a boundary step the relation treats as *unknown* (§4.4).

State: `heap : gmap loc (val * share)` + the threadpool. `state_interp` ties the
heap to the ghost state below (the `iris_state.v` job, mirroring
`lambda_zer_escape/iris_escape_state.v`).

## 3. The ZER Iris idioms this reuses (grounding — already in-tree)

- **per-instance tag via `ghost_map`** — `region_ptr γ id r := id ↪[γ] r` with
  `_agree` / `_exclusive` / `_new` / `_lookup` lemmas
  (`lambda_zer_escape/iris_escape_resources.v`). The reach tag and the region tag
  are this exact pattern.
- **exclusive linear resource** — `alive_handle γ p i g` with
  `alive_handle_exclusive : alive ∗ alive ⊢ False`
  (`lambda_zer_handle/iris_resources.v`). The thread-join token is this exact
  pattern (C01/C02 already note "same as A10 leak / double-free").
- **Iris invariant `inv N P`** — guards a shared resource; opened only in an
  atomic step / under a lock token. The shared-struct discipline is this.

## 4. The four conditions as ghost state — THE TRACKING SET (derived)

### 4.1 Reach (Axis A) — the `shared` taint

**IMPLEMENTATION REFINEMENT (verified, `iris_shared_inv.v`, 2026-06-20): reach is
realized STRUCTURALLY, not by a ghost map — strictly simpler, same tracking set.**
A location is LOCAL ⟺ a thread exclusively owns its `pointsto l (DfracOwn 1) v`
(gen_heap points-to is already exclusive); SHARED ⟺ that points-to has been given
up into an Iris invariant `is_shared N l P := inv N (∃ w, l↦w ∗ P w)` (persistent ⟹
duplicable across threads). The `publish_shared` lemma is the reach transition
(consume `l↦v ∗ P v`, get the persistent `is_shared`). This removes the separate
share-tag ghost map below — the points-to *placement* IS the reach tag, and the
DISCIPLINE condition (§4.2) then falls out for free (no rule yields `l↦w` for a
shared `l` except by opening the invariant). The compiler still tracks a 1-bit
`shared` taint (DESIGN §5) — the syntactic approximation of "is the points-to in an
invariant" — so the tracking set is unchanged. The ghost-map description below is
the original sketch, retained for context.

Ghost map `share_tag γ_s ℓ : Local tid | Shared` (the `region_ptr` pattern over a
2-point lattice). `state_interp` pins `heap[ℓ].share = share_tag[ℓ]`.
- `LocalLoad/Store ℓ` requires `share_tag γ_s ℓ (Local self)` — **a thread can
  touch a Local location only if it owns the Local-self token.** A second thread
  cannot fabricate `Local self'` for the same ℓ (agreement ⇒ self=self').
- To make ℓ reachable by another thread you must **publish** it: a step that flips
  `Local self → Shared`, allocating its invariant (§4.2). This is the *only* way to
  reach, so every cross-thread location is `Shared` ⇒ guarded ⇒ §4.2 applies.
- **Compiler tracking derived:** a one-bit-lattice `shared` taint per location,
  inferred (not annotated) at the publication points and propagated through
  `&`/casts/slices — exactly `Send`/`Sync`, exactly Axis A's "carrier-or-tainted"
  inclusion model that replaces the exclusion-list scanner.

### 4.2 Discipline (Axis B) — the shared invariant
Each `Shared ℓ` carries `inv N (∃ v, ℓ ↦ v ∗ payload v)`. Access is **only** via
`LockedLoad/Store` (opens the inv) or `Atomic` (logically-atomic, opens-and-closes
in one step). There is **no rule** to get `ℓ ↦ v` for a `Shared ℓ` without opening
the invariant ⇒ an unsynchronized access to shared data is **unprovable** ⇒
DR-stuck is unreachable. Multi-root / union-arm / cond-predicate / defer-body
accesses must each open their root's invariant: the proof *forces* the lock to wrap
**every** shared sub-access.
- **Compiler tracking derived:** "every access to a tainted location is under its
  carrier lock or atomic" — Axis B's lock-scope-walker that must cover *all* roots
  and *all* sub-statements (the bug class: any unlocked shared sub-access).

### 4.3 Lifetime (Axis C) — region tag + linear join token
Two pieces:
- **region tag** `region_ptr γ_r id r` (reuse escape verbatim): a published pointer
  carries `r ∈ {Static, Heap, ScopedR}`. The publish step (§4.1) and the
  store-into-carrier / atomic-store / ring-push steps all **require** the published
  pointer's region ≠ `LocalStack` (else UAF-stuck). This is `'static`.
- **linear join token** `join_tok γ_j tid` (the `alive_handle` exclusivity pattern):
  `ScopedSpawn e` borrowing region `ScopedR` produces `join_tok`; `Join` consumes
  it; the region `ScopedR` cannot be closed/freed while `join_tok` is live (a
  linear residual = leak, same as A10). This is `thread::scope`.
- **THE ir_merge_states BUG, formalized:** `join_tok` is a **linear** resource, so
  it must be threaded through *every* control-flow merge. The compiler's
  `ir_merge_states` merges only `handles[]` and drops `threads[]` at CFG merges =
  **dropping a linear resource at a merge** = unsound (the leak/borrow obligation
  vanishes ⇒ false-green UAF). The proof makes this a *theorem-level* obligation:
  any analysis must carry the join token across merges. (Fix this first — it is
  the one place the existing enforcer is provably bypassed.)
- **Compiler tracking derived:** per-pointer region tag (the escape lattice) +
  per-thread linear join obligation **merged at every CFG join** + the Handle-gen
  runtime trap for freeable carrier payloads.

### 4.4 Visibility (Axis D) — the boundary capability / frame
`OpaqueCall` steps over code the relation **cannot see** (cinclude C that may
`pthread_create`; emitted runtime). Iris-wise it is a frame the proof cannot
instantiate ⇒ the program can only be proved safe if the value handed across
**already** satisfies the *closed* invariant on its own: it must be **copy-safe**,
or carry both `Shared` (§4.2) **and** region `Static/Heap` (§4.3) — i.e. a
self-synchronizing, long-lived capability that needs no further analyzer help.
Anything else ⇒ the proof is stuck ⇒ **conservative-reject** (the only sound move,
since whole-program analysis is banned by architecture).
- **Compiler tracking derived:** a `threads`/`captures` capability on cinclude
  funcptr/pointer params (require `shared`+non-stack), `--strict-interop`
  default-reject otherwise, and thread-safe (`__thread`/guarded) emitted runtime.

## 5. The DERIVED tracking set (the answer to "what to track")

Read directly off §4 — minimal and complete *by construction of the proof*:

| Track (compiler state) | Iris source (§) | Axis | Existing system to extend |
|---|---|---|---|
| per-location `shared` taint (1-bit lattice, inferred+propagated) | reach token §4.1 | A | qualifier tracking (like `volatile`); spawn-scan → inclusion model |
| per-shared-location lock/atomic discipline at **every** access | shared inv §4.2 | B | the per-statement auto-lock → all-roots/all-sub-stmts walker |
| per-pointer region tag (Static/Heap/Scoped/LocalStack) | region §4.3 | C | escape analysis (`is_local_derived`) |
| per-thread **linear** join obligation, **merged at every CFG join** | join_tok §4.3 | C | `IRThreadTrack` + **fix `ir_merge_states` to merge `threads[]`** |
| freeable-carrier-payload → Handle-gen runtime trap | region+linearity §4.3 | C | Handle generation counter |
| boundary capability (`shared`+static) at opaque/cinclude crossings | frame §4.4 | D | NEW: cinclude `threads` capability + `--strict-interop` |

That table is the architecture. Nothing outside it needs tracking for cross-thread
**memory** safety (the §1 decomposition is the argument; the §0 theorem is what
makes it certain once proved). Resource exhaustion (unbounded spawn / OOM) is the
liveness floor, **not** tracked here.

## 6. Why this prevents re-architecture

The tracking set is *read off the proof obligations*, not proposed and then
checked. If §4's invariant is the right safety statement (the §0 theorem, against
the operational semantics — the Level-1 oracle, NEVER against current C), then any
compiler that discharges these obligations is sound, and any that omits one is
exactly a hole. So the compiler is built **to this table once**; the proof can only
*confirm* the table, never force a different shape of tracking. That is the
"infinite-time, no-rework" trade made concrete.

## 7. Build order (Coq files, dependency order — when implementation starts)

1. `syntax.v`, `semantics.v` — threadpool language instance (the new part; mirror
   `iris_lang.v` but with a non-trivial `efork`). **Hardest single step** — get the
   concurrent `language` instance + adequacy plumbing right first.
2. `iris_share_resources.v` — reach token (copy `escape_resources` shape).
3. `iris_shared_inv.v` — the shared invariant + locked/atomic access specs.
4. `iris_region_join.v` — region tag (reuse escape) + linear `join_tok`
   (reuse `alive_handle` exclusivity); the **merge lemma** is the centerpiece
   (formalizes the ir_merge_states obligation).
5. `iris_boundary.v` — the opaque frame / capability rule.
6. `iris_concurrency_theorems.v` — DR-free + UAF-free via adequacy; the §1
   exhaustiveness statement.
7. Retire the schematic C/D/E memory rows in `iris_concurrency.v` (keep the
   liveness/context rows — deadlock D03, ISR C03/C04, async F — as schematic or
   move to a separate liveness file; they are NOT in the §0 theorem).

**Parallel, architecture-independent, do anytime:** the `ir_merge_states`
`threads[]`-merge fix (§4.3) — it is correct under any version of this design and
is the one shipped-enforcer soundness bug.

## 8. Honest scope

- This proves the **spec** is sound (Level 1). Wiring it to the compiler is Level
  2 (one `tests/zer_proof/` pair per theorem) + Level 3 (VST on the extracted
  predicates: the taint check, the region check, the merge function).
- 100-200 hrs, multi-session. Step 1 (concurrent language instance) is the gate;
  everything else reuses in-tree patterns.
- Liveness (deadlock/livelock) and resource exhaustion are explicitly **out** of
  the §0 theorem — named floor.
