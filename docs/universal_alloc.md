# Universal Allocation — Full Context Dump (`alloc` / `free`)

**Status:** PHASES 1-2 + PRIMITIVE-ELEMENT SUPPORT SHIPPED (2026-07-07).
Implemented and `make check` GREEN (ZER 898/0):
- `alloc(T) -> ?*T` (struct only; desugars to `T.alloc_ptr()`),
- `alloc(T, n) -> ?[*]T` for T = **any struct OR primitive** (`alloc(u32, n)`,
  `alloc(u8, n)`, `alloc(Node, n)` — the parser accepts a primitive type keyword
  as the first arg; the element type is resolved by name via
  `alloc_resolve_elem_type`, never check_expr'd as a value),
- `free(p:*T)` (desugars to `T.free_ptr`) and `free(s:[*]T)` (direct heap free).
All fully Model-1 tracked (UAF / double-free / leak, incl. through struct-field
compound keys; escape works). The `kv_table_create` shape (Appendix B) compiles +
runs + escapes.

STILL TODO: (a) the `[*]u8 -> [*]T` target-driven reinterpret coercion — the
un-lock for user-written HETEROGENEOUS custom allocators (one byte region carved
into mixed typed views). NOTE this is now *lower priority* because `alloc(T, n)`
works for every element type directly, so a plain "dynamic array of X" needs no
reinterpret; the coercion is only for hand-written bump/pool allocators. The
BASELINE for it is favorable — same-type slice assignment already propagates
`alloc_id` (verified: `[*]B b2 = b; free(b); b2[0]` is a caught UAF), so the
reinterpret's UAF-through-view would come mostly free. (b) migration that RETIRES
`alloc_ptr`/`free_ptr` (they still work alongside `alloc`/`free` as the reuse
target for the `*T` cases).

This document was the durable design handoff; it cost a very long multi-turn
session (many dead ends, three background design workflows, ~3M subagent tokens,
and many Docker verifications) to reach the design, then the phased build. Read
§2 for the surface; read §3 before you propose ANY alternative — almost every
"obvious" alternative was already tried and has a recorded compiler error proving
it dead.

**Date started:** 2026-07-06
**Owner framing (verbatim intent, preserved):** "find like the most universal
allocation … safe malloc, no need handle, no need arena, pool explicitly, since
malloc is brainless … our alloc_ptr is technically alloc auto infers without the
explicitness." And: "@slice_as intrinsics not good, we need max flexibility …
legalize it without intrinsics lock … how *T and [*]T brainlessly they can just
detect."

**One-line summary:** ZER will grow ONE brainless allocation surface —
`alloc(T)` → `?*T` and `alloc(T, n)` → `?[*]T`, plus `free(x)` — that unifies and
retires `alloc_ptr`/`free_ptr`, adds the currently-missing runtime-sized escaping
heap array, and lets `[*]u8 → [*]T` reinterpret happen as a target-driven
coercion (no `@slice_as` intrinsic). It reuses existing tracking machinery
end-to-end; it needs NO new safety-model oracle.

---

## Table of Contents

0. Reading note
1. The triggering problem (the C code that started it)
2. TL;DR — the decision (read this if you need the answer NOW)
3. The exhaustive dead-end map (every path tried + the exact compiler error)
4. The design exploration — C1 / C2 / C3 candidates + adversarial verdicts
5. The pivot to a universal brainless `alloc` (why intrinsics were rejected)
6. Decisions LOCKED (spelling, alloc_ptr fate)
7. Safety analysis — the soundness core
8. The exact machinery to reuse (file:line map — do NOT re-discover this)
9. Implementation plan + the flagged verification landmines
10. How locked the current allocator layer is (the motivation, quantified)
11. Compiler bugs found during this exploration
12. Relationship to the 4-model architecture and universal_pointer.md
13. Glossary
- Appendix A: every test program + its exact verified result
- Appendix B: the target `kv_table_create` code

---

## 1. The triggering problem

The user was translating a Linux-kernel-style hash table into ZER:

```c
struct kv_entry { char *key; char *value; struct kv_entry *next; };
struct kv_table { int size; struct kv_entry **buckets; };

struct kv_table *kv_table_create(int size) {
    struct kv_table *ht = kmalloc(sizeof(*ht));            // one node
    if (!ht) return NULL;
    ht->size = size;
    ht->buckets = kcalloc(size, sizeof(*ht->buckets));    // runtime-sized, zeroed
    if (!ht->buckets) { kfree(ht); return NULL; }
    return ht;
}
```

The hard requirement, refined across many turns until it was unambiguous:

- `buckets` must be `[*]T` (a slice — "pointer to many"), with **NO compile-time
  bound anywhere in the declaration**. The count lives only in the runtime `size`
  parameter, exactly like C's `**buckets`.
- The bucket array is sized at **runtime** (`size` is a parameter).
- The table (holding the buckets) is **returned** — i.e. the allocation must
  **escape** the creating function and persist.
- No `Arena`, no `*opaque`, no explicit `Pool`, no explicit `Slab` declaration,
  no `Handle`. "Brainless, just alloc."
- Runtime safety checks are acceptable ("runtime its fine").

This is precisely `malloc`/`kcalloc`: a **runtime-sized, escaping, freeable heap
allocation**. It turned out ZER's safe-allocator set deliberately has no
primitive for it — which is what this whole document is about.

---

## 2. TL;DR — the decision (read this if you need the answer NOW)

### The new surface

```
alloc(T)       -> ?*T       // ONE T on the heap. Retires Type.alloc_ptr().
alloc(T, n)    -> ?[*]T     // n contiguous auto-zeroed T on the heap. THE missing primitive.
free(x)        -> void      // frees *T or [*]T. Retires free_ptr(). Dispatches on shape.
[*]T v = someByteSlice;     // target-driven coercion [*]u8 -> [*]T. Retires @slice_as (NO intrinsic).
```

- `T` is **explicit** (an argument), like `arena.alloc(T)` / `@size(T)`. Shape is
  chosen by **arg count**: no `n` → single `*T`; with `n` → slice `[*]T`.
- Bare `alloc(n)` with the type inferred from the assignment target is a
  **later sugar**, NOT the first cut (see §6.1 for why — the `orelse` inference
  problem).
- `alloc_ptr`/`free_ptr` are kept as **thin aliases during migration, then
  deleted**. The end state is ONE surface (§6.2).
- `Handle`, `Arena`, `Pool` **stay** for when you *want* explicit control
  (generation-checked, bump-reset, fixed-capacity/ISR-safe). `alloc`/`free` is the
  brainless default.

### Why it's safe (the whole ballgame in one sentence)

A slice `{ptr, len}` is sound **iff `.len` is TRUE**. Every step of this design
keeps `.len` **allocation-derived** (set by the allocator as `n`, or computed as
`bytes.len / @size(T)` floored) — never a user-writable number. Forge the length
and you have a silent buffer overflow; the design structurally prevents forging.

### Why it needs almost no new machinery

- `alloc(T)` **is** today's `alloc_ptr` (same auto-slab, same `?*T`, same
  tracking) — just a nicer spelling.
- `alloc(T, n)` and the `[*]u8→[*]T` coercion reuse: the slice `{ptr,len}`
  emission, the `IR_CAST` `alloc_id`-sharing edge (for UAF-through-a-view), the
  `ir_mark_freed` alloc_id-group loop, the leak pass, and the BUG-489 alignment
  trap. See §8 for exact file:line.
- **No new oracle.** It extends the shipped Model-1 handle-flow lattice
  (`proofs/.../lambda_zer_handle/handle_flow_lattice.v`): the region is a handle,
  the coercion is an alias edge, `free` is a group→FREED transition. The one
  genuinely-new lattice element is "**a slice-typed local carries + propagates an
  `alloc_id`**" (slices aren't heap-tracked handles today).

### Sequencing (vertical slice first — repo doctrine)

1. Implement `alloc(T)` → `?*T` on the existing auto-slab machinery. Proves the
   surface changes nothing about tracking. (Alias `alloc_ptr` to it.)
2. Add `alloc(T, n)` → `?[*]T` (heap slice) + the "slice carries alloc_id"
   tracking element. This is the new lattice element — verify it first.
3. Add the `[*]u8 → [*]T` target-driven coercion (the un-lock for custom
   allocators) + the alignment trap.
4. Migrate all `alloc_ptr`/`free_ptr` call sites; delete the aliases.

---

## 3. The exhaustive dead-end map

**Every one of these was tried and has a recorded compiler error or runtime
result. Do NOT re-try them expecting a different outcome.** This is the highest-
value section: it is why the answer is a new primitive and not a clever
composition of existing ones.

### 3.1 `alloc_ptr` — one object, no size, cannot be indexed

`Type.alloc_ptr()` is the malloc-of-ONE-object primitive. It does NOT take a size
and its `*T` result cannot be indexed as an array. Both halves verified:

```zer
Bucket.alloc_ptr(4)            // -> error: Bucket.alloc_ptr() takes no arguments
```
```zer
*Bucket b = Bucket.alloc_ptr() orelse return;
b[i].head = null;             // -> error: cannot index a single pointer '*Bucket'
                              //    as an array — '*T' is one object and carries no
                              //    length … it would be a silent buffer overflow.
                              //    Use '[*]Bucket'.
```

So you cannot build the bucket array from `alloc_ptr`: `alloc_ptr()` gives one
`sizeof(T)` object (never `n × sizeof(T)`), and you can't loop/index over a `*T`
("jump per 8 bytes" is exactly the pointer arithmetic ZER bans). The two things
you'd need to combine are each individually forbidden.

### 3.2 `Arena.alloc_slice(T, n)` — the only runtime-`[*]T`, but it CANNOT ESCAPE

`Arena.alloc_slice(T, n) → ?[*]T` is the **only** builtin that produces a
runtime-sized `[*]T`. But arena memory can be `.reset()`, so the checker forbids
its result from leaving the frame:

```zer
ht.buckets = arena_slice;     // -> error: cannot store arena-derived pointer 'b'
                              //    through pointer parameter 'ht' — pointer will
                              //    dangle when arena is reset
return ht;                    // -> error: cannot return arena-derived pointer 'ht'
                              //    — arena memory is freed when function returns
```

`kv_table_create` **returns** the table holding the buckets → Arena is
structurally disqualified. This is the whole reason a *new* escaping primitive is
needed. (Escape is gated by the `is_from_arena` flag + `ZC_COLOR_ARENA`; see §8.3.)

### 3.3 Fixed array — works, but needs a compile-time bound

```zer
Bucket[256] buckets;          // works, but 256 is a compile-time bound
```
The user explicitly rejected any bound in the declaration. A named `const` does
NOT help — it is not accepted as an array size:
```zer
const u32 N = 8;
?*Entry[N] buckets;           // -> error: array size must be a compile-time constant
```
Only a bare literal (`[8]`) or a `comptime` function call (`[BUCKET_COUNT()]`)
is accepted as an array size. (This asymmetry is itself a mild wart.)

### 3.4 Global arena — `alloc_slice` returns None at RUNTIME

An attempt to dodge §3.2 with a global arena. It **compiles** (via a checker gap,
see §11) but **fails at runtime**: a global `Arena` initialized with the in-place
`garena.over(gbacking)` form does not actually initialize, so `alloc_slice`
returns `None`:

```zer
Arena garena; u8[65536] gbacking;
u32 main() {
    garena.over(gbacking);
    [*]BucketSlot b = garena.alloc_slice(BucketSlot, 4) orelse { print("NONE"); return 9; };
    // -> prints "NONE", exit 9. alloc_slice returned None.
}
```
Only the **local** capture form works: `Arena ar = Arena.over(backing);` then
`ar.alloc_slice(...)` → len 4, runs. But a local arena can't escape (§3.2).

### 3.5 Slice-into-a-fixed-global-array — WORKS, but needs the backing array

The one thing that compiles AND runs AND escapes today:

```zer
Bucket[1024] backing;                     // fixed global backing (never dangles)
struct KvTable { u32 size; [*]Bucket buckets; }
?*KvTable make(u32 size) {
    *KvTable ht = KvTable.alloc_ptr() orelse return;
    if (size > 1024) { KvTable.free_ptr(ht); return null; }
    ht.size = size;
    ht.buckets = backing[0..size];        // runtime-sized VIEW into a global array
    return ht;                            // ESCAPES fine — global array never dangles
}
```
Verified: compiles, runs, `ht.buckets.len == 4`, and `make` returns the table.
This works because a slice into a **global fixed array** has static lifetime and
never dangles, so escape analysis permits it (unlike arena). But it still has the
`Bucket[1024]` backing bound the user rejected. This is the closest working
"today" answer and is the honest fallback if the new primitive is never built.

### 3.6 The judge-panel — 5 hash-table designs (all fall back to a bound)

A 5-candidate workflow (flat hand-written / container-generic / open-addressing /
raw-pointer / Pool-fixed) concluded the winner is the **flat hand-written
`hash_map_chained.zer` pattern** (Slab + `?Handle(Node) next` chaining + a
fixed-literal bucket array). Key incidental finding: the `container Chained(T) {
?Handle(Chained(T)) next; }` idea **does not compile** — TWO bugs:
1. `TYNODE_CONTAINER` registers the stamped struct name *before* resolving its
   fields, so a self-referential field re-enters and trips the pre-stamp
   collision check (checker.c ~2277-2293).
2. Deeper: `subst_typenode`'s `TYNODE_HANDLE` case does NOT recurse into
   `handle.elem`, so `T` in `?Handle(Chained(T))` never substitutes → "undefined
   type 'T'". **This breaks ANY `container` field of shape `Handle(T)`**, not just
   self-referential ones. (Logged as a separate finding; see §11.)

Conclusion of the panel: every runtime-sized option needs either a bound or a new
primitive. That is what motivated the primitive-design workflow (§4).

---

## 4. The design exploration — candidates + adversarial verdicts

A background workflow explored the minimal safe PRIMITIVE. Three candidates,
four adversarial lenses (length-truth/overflow, UAF-through-view/escape,
un-lock÷complexity, philosophy+impl+proof-cost).

### 4.1 C1 — `@heap(T, n) -> ?[*]T` (uniform generic intrinsic)

One intrinsic, any T, shape by arg presence. `.len = n` set by the intrinsic;
`calloc(n, sizeof(T))` (the two-arg form has a **stdlib overflow guard**: if
`n*sizeof(T)` overflows, calloc returns NULL → `orelse` fires). Trivially sound —
no reinterpret, no token. Reuses `IRMC_ALLOC_PTR` verbatim.
**Verdict:** SAFEST, CHEAPEST, fully solves the *user's* problem — but
`alloc_slice ≡ @heap`, so it IS the frozen catalog entry the architect rejected.
Un-locks the *type* axis only (homogeneous `[*]T`), not the allocator-KIND axis.

### 4.2 C2 — `heap.alloc(nbytes) -> ?[*]u8` + `@slice_as(T, bytes) -> [*]T`

Raw tracked heap bytes + a safe reinterpret whose `len' = bytes.len / @size(T)`
(floored, derived, never forgeable). Then `alloc_slice`, bump allocators, pools,
rings, custom allocators are all **ordinary ZER library code** over the two
primitives. This is `universal_pointer.md` §6.5's "user-extensible allocator"
un-lock, achieved with 2 additions instead of the runtime-tag/header path PART 5
rejected.
**Verdict:** the ONLY candidate that reaches the *architect's* goal (un-locks both
axes). Its one new soundness surface is the reinterpret (`@slice_as`): needs (a)
len derived solely from the runtime `bytes.len`, (b) alignment trap for
mid-region views. Extends the Model-1 lattice; no new oracle.

### 4.3 C3 — `Region` opaque type — DISQUALIFIED

A tracked heap block carrying its own `.size`, exposing only whole-region typed
views. The `.size` field is a **new forgeable length source**: if writable across
any boundary, `r.size = 4096` after an 8-byte alloc yields a `.len = 512` view
over 8 bytes → OOB. Fixable only by total opacity (harder than C1/C2, which carry
no extra token), and it collapses into "C2 + a wrapper" the moment `heap.free`
must accept views. More cost, less un-lock, a new forgeable surface. **Do not
build C3.**

### 4.4 The adversarial verdicts (condensed)

- **Length-truth:** C1 > C2 > C3. All sound *as specified*; C3 has the forgeable
  `.size` token. C1's guard is a C-stdlib guarantee (two-arg calloc); C2's is
  floored derivation; C3 adds a token that must be opacity-enforced.
- **UAF-through-view/escape:** C1 > C2 > C3. ALL share ONE hole: a heap
  allocation returned inside a struct then freed through a struct field in
  another function — the **P9/BUG-737** blind spot (a by-value-struct pointer/
  slice field sink the escape summary must descend into; the codebase already
  shipped a UAF through this shape once). C2 adds a single-function view-desync
  edge (`@slice_as` must register the alias in BOTH emitter paths); C3 adds a
  token-duality free-through-`*param` hole.
- **Un-lock ÷ complexity:** C2 > C1 > C3. Only C2 lets a NEW allocator KIND
  (heterogeneous/bump/general) be library code. C1/C3 only carve homogeneous
  `[*]T`. C3 pays the most, un-locks the least.
- **Philosophy + impl + proof:** C2 > C1 > C3. C2 is non-dominated, extends the
  Model-1 lattice, no new oracle. C1 is safest/cheapest but IS the locked entry.
  C3 is dominated ("C2 + wrapper" by its own analysis).

### 4.5 The hybrid recommendation (from the workflow)

**C1 as the vertical slice → generalize to C2.** C1's tracking is a strict SUBSET
of C2's — both need "a slice carries an alloc_id and propagates through subslice/
copy" (the one new lattice element). Prove it via C1 (zero reinterpret risk),
then add the reinterpret to reach C2. Same continuous proof surface, no new
oracle. This matches the repo's own "UAF vertical slice FIRST, then generalize"
doctrine.

**The honest ceiling (stated plainly, applies to ALL candidates):** ZER has NO
function generics (only container monomorphization). So even under C2 a library
`alloc_slice(T, n)` / `pool_alloc(T)` is **per-T** (hand-stamped or
`container`-monomorphized). "Allocator *kind* as library code" is reachable; a
single fully-*generic* `alloc_slice` is NOT. C2 is the *maximal* un-lock the
language allows — not a defect of the choice, a fact of no-function-generics.

---

## 5. The pivot to a universal brainless `alloc`

After the workflow, the owner rejected `@slice_as`-as-an-intrinsic:

> "@slice_as intrinsics not good, we need max flexibility … legalize it without
> intrinsics lock … how *T and [*]T brainlessly they can just detect."

### 5.1 Why the intrinsic was rejected

An intrinsic (`@slice_as`, `@heap`) is still a *named, fixed catalog entry* — the
same "locked" rigidity, one level up. The owner wants the un-lock to be a
**native, type-driven behavior**, like the existing brainless auto-inference:
- `*T` auto-forces to `[*]T` when used as "pointer to many."
- `keep` / not-`keep` is auto-inferred (universal_pointer.md PART 5).
- array auto-coerces to slice at call sites.

Allocation should follow the same philosophy: **the target type drives the shape,
with runtime checks where needed** — no named operator.

### 5.2 The vision

- Allocation is ONE operation (`alloc`), not per-type-method (`Type.alloc_ptr`),
  not per-shape-intrinsic (`@heap`/`@slice_as`).
- The **result shape** (`*T` single vs `[*]T` many) is chosen by the target /
  arg-count, brainlessly.
- The **reinterpret** (`[*]u8 → [*]T`, for building custom allocators) is a
  **target-driven coercion**, not an intrinsic: assigning a `[*]u8` region-slice
  to a `[*]T` target auto-coerces (len recomputed & derived, alignment
  runtime-trapped), exactly the brainless way `*T→[*]T` and array→slice already
  work. "Runtime its fine."
- It **reworks/retires `alloc_ptr`** into this universal surface.

### 5.3 The design (this is what gets built — see §2 for the surface)

`alloc(T)` → `?*T` (retires `alloc_ptr`, reuses the auto-slab).
`alloc(T, n)` → `?[*]T` (the escaping runtime-sized heap array).
`free(x)` → dispatches on `*T` vs `[*]T` (retires `free_ptr`).
`[*]T v = byteSlice;` → target-driven coercion (retires `@slice_as`; length
derived `bytes.len/@size(T)`, alignment trapped, `alloc_id` inherited).

This design is **SAFER than the raw `@slice_as` plan** in the common case: you
allocate the correct *typed* shape directly (`alloc(T, n)` gives `[*]T`), so the
dangerous bytes→T reinterpret only appears when a user *deliberately* builds a
custom allocator by carving one `[*]u8` region into typed views — and there the
coercion is target-annotated (intent on the page) and runtime-checked.

---

## 6. Decisions LOCKED

### 6.1 Spelling — explicit `alloc(T)` / `alloc(T, n)` core; bare `alloc(n)` later

**Decided: explicit T is the first cut. Bare `alloc(n)` (type inferred from the
target) is a strictly-additive later sugar.**

Reason (substrate, not taste): allocation returns an **optional** (`?*T` / `?[*]T`
— it can OOM) and is unwrapped with `orelse`:
```zer
*Node n = alloc(Node) orelse return;
```
For the **bare** form `alloc()` to type, the compiler must infer element type AND
shape by threading the target type (`*Node`) **backward through the `orelse`**
into the bare call. ZER types expressions bottom-up; that backward flow is new,
fragile inference with real ambiguity holes (bare `alloc(n)` with no clear target
= "n of what?"). Explicit `alloc(T, n)` types trivially bottom-up, is
unambiguous, and matches the idiom ZER already uses (`arena.alloc(T)`,
`arena.alloc_slice(T, n)`, `@size(T)`). "Brainless" is still met — ONE `alloc`,
any T, shape by arg count. Bare form is a superset you can add later once
target-inference is proven; you can always add brevity, you can't cheaply un-ship
a fragile inference model. The "shape from target type" magic the owner wants
still lives where it is genuinely needed and sound: the `[*]u8 → [*]T` coercion.

### 6.2 `alloc_ptr` fate — alias during migration, then DELETE

**Decided: not a permanent alias. Thin alias transitionally (so existing call
sites don't all break at once while converting), then removed. End state = one
surface.**

Aliasing is *not inaccurate* — `alloc(T)` is semantically identical to
`alloc_ptr` (same auto-slab, same `?*T`, same tracking), so `alloc_ptr` = "the
`*T` case of `alloc`" is a true synonym. But a *permanent* alias leaves two names
for one thing — the exact "not brainless" duplication the whole effort removes.
It is pre-1.0 with a fully-owned tree, so migration is a mechanical find-replace,
not a compatibility burden. `alloc_ptr` → 36 files in `tests/zer/` at last count
(plus examples/lib) — mechanical.

---

## 7. Safety analysis — the soundness core

This is a **reject→accept relaxation** (a new capability that ACCEPTS programs
previously impossible). Per CLAUDE.md that is the ONE change class where a bug =
a SHIPPED use-after-free or buffer overflow. Every property below must hold.

### 7.1 The length-truth invariant (the whole ballgame)

A slice `{ptr, len}` is safe iff `.len` is TRUE (ptr genuinely has
`len*@size(T)` valid bytes). **Today `.len` is structurally allocation-/array-
derived and non-forgeable** — verified: every construction site derives `.len`
from an allocation count, an array length, or a bit-range; there is NO
slice-to-slice reinterpret and NO `(ptr, len)` constructor taking user
expressions. The new design MUST preserve this:
- `alloc(T, n)` → `.len = n`, and `calloc(n, @size(T))` reserves exactly
  `n*@size(T)` bytes. Two-arg calloc guards `n*@size(T)` overflow at the stdlib
  level (returns NULL → `orelse`). Truthful by construction.
- `[*]u8 → [*]T` coercion → `.len' = bytes.len / @size(T)`, **floored** (integer
  division), a pure function of the *real* region. `len' * @size(T) ≤ bytes.len`
  always. Remainder understates (safe). Structurally incapable of overstating.
- **No user integer is ever accepted as a length.** The user chooses a
  *primitive* (`alloc`) or writes a *target type* (`[*]T v = ...`); they never
  assert a length fact. This is exactly the closure line: "a value the user can
  forge the length of = the infinite-unsafe-impedance position ZER refuses."

### 7.2 UAF-through-a-view (the load-bearing reuse)

The hazard: free the `[*]u8` region, then use a `[*]T` view of it (or vice
versa). **The machinery already exists and is the right shape:**
- `ir_snapshot_alias` / `ir_apply_alias` copy `alloc_id` + provenance.
- `IR_CAST` (Phase F) ALREADY makes a cast destination share the source's
  `alloc_id` ("If source has a tracked handle, dest becomes an alias sharing
  alloc_id. Mirrors @ptrcast alias tracking.").
- `ir_mark_freed` walks EVERY handle with matching `alloc_id` (the group loop).

So if the `[*]T` coercion registers the view as an `alloc_id` alias of the byte
region (exactly as `IR_CAST` does), then `free(region)` marks the whole group
FREED → using either the region or any view after free is caught. **Double-free
and leak fall out of the same alloc_id-group machinery for free.** This is not new
safety infrastructure; it is extending a shipped pointer-cast edge to the
slice-view case. See §8.5 for the exact lines.

### 7.3 Escape (why it works where arena doesn't)

Escape is gated by the `is_from_arena` flag + `source_color`. Arena results are
`ZC_COLOR_ARENA` and marked `is_from_arena`, so store-to-global / store-through-
param / return are hard errors. `alloc_ptr` NEVER sets `is_from_arena` and is
`ZC_COLOR_POOL`, so it escapes freely. The new `alloc` follows `alloc_ptr`:
POOL-colored, not arena → returnable, struct-storable. Escape marks the handle
`escaped=true` (ownership → caller). See §8.3.

### 7.4 Leak / double-free

Leak: the exit pass scans live handles and errors on ALIVE-and-not-escaped at
function exit; it SKIPS only `ZC_COLOR_ARENA`. `alloc` is POOL → included → a
never-freed non-escaped `alloc` is a leak error (same as `alloc_ptr` today).
Double-free: freeing an already-FREED alloc_id group hits the existing
double-free arm. See §8.6.

### 7.5 No pointer arithmetic / grammar closure

`alloc(T, n)` takes a *count*, returns a fat pointer — no address surfaces.
Carving is bounds-checked subslicing `base[a..b]`, never `ptr+N`.
`@ptrtoint`/`@inttoptr` stay mmio-gated. No in-language `unsafe`, no `(ptr,len)`
constructor from user expressions. The user chooses a primitive / writes a target
type; they never assert a fact. Inside the closure (Definition A frozen-catalog
entry, not an escape hatch).

### 7.6 Alignment

`calloc` returns `max_align_t`-aligned memory → a whole-allocation `[*]u8` base
is aligned for any `T`. Danger only for a **mid-region** view (`base[off..]` at a
non-aligned `off`). Mitigation: reuse the BUG-489 runtime trap
`if ((usize)ptr % @align(T)) _zer_trap("unaligned")`. Safer initial policy:
restrict the `[*]u8 → [*]T` coercion to **whole-allocation views only**, relax to
mid-region later with the trap. Alignment is a runtime access-UB issue, NOT a
length-forgery issue — it never lets `.len` overstate.

### 7.7 The honest ceilings (do not over-promise "un-locked")

1. **No function generics.** A library `alloc_slice(T,n)` / custom allocator is
   per-T (hand-written or `container`-stamped). Allocator *kind* as library code
   is reachable; a single *generic* allocator is not. This is a language fact.
2. **Implicit reinterpret visibility.** The `[*]u8 → [*]T` coercion is *sound*
   (len derived, alignment trapped, alloc_id shared) but *bold* — a reader might
   not see that bytes are being reinterpreted as a struct. Mitigation: allow the
   coercion ONLY at a typed declaration/assignment site where the `[*]T` target
   is written explicitly (intent on the page), never silently mid-expression.
3. **Bytes→struct** is safe ONLY because ZER structs are plain-data + auto-zeroed:
   a calloc'd (all-zero) region is a valid zero-constructed `T` (null `?*T`,
   gen-0 = invalid handle). No ZER struct has a non-zero-required invariant. If
   that ever changes, this coercion needs revisiting.

---

## 8. The exact machinery to reuse (file:line map — do NOT re-discover this)

Line numbers are from the 2026-07-06 tree; they drift — grep the named symbol if
off. This map cost real tool-calls to assemble; use it.

### 8.1 Auto-slab (the "brainless auto-infer without explicitness")

- `find_or_create_auto_slab(Checker*, Type* struct_type)` — **checker.c:1583**.
  Lazily creates a global `_zer_auto_slab_<Type>` on first use, keyed by type,
  deduplicated in `c->auto_slabs[]`. THIS is the mechanism `alloc(T)` generalizes:
  you never declare a Slab; referencing the alloc conjures it.
- `Task.alloc_ptr()` dispatch + typing → `?*T` — **checker.c:5428-5436** (calls
  `find_or_create_auto_slab`, sets `result = type_optional(type_pointer(obj))`).
- `slab.alloc_ptr()` — **checker.c:5282-5314**. `pool.alloc_ptr()` —
  **checker.c:5132-5162**.

### 8.2 IRMC classification + Model-1 tracking

- `IRMC_*` enum (add `IRMC_ALLOC_SLICE` / reuse for `alloc`) —
  **zercheck_ir.c:1320-1329**.
- `ir_classify_method_call_ex` (memcmp arms; alloc_ptr → `IRMC_ALLOC_PTR` at
  ~1406, alloc_slice → `IRMC_ARENA_ALLOC` at ~1410) — **zercheck_ir.c:1402-1420**.
- Receiver-type whitelist (exhaustive switch, `-Werror=switch`) —
  **zercheck_ir.c:1365-1384**.
- alloc handler: `IRMC_ALLOC` / `IRMC_ALLOC_PTR` register a handle
  `state=IR_HS_ALIVE, alloc_id=dest_local, source_color=ZC_COLOR_POOL` —
  **zercheck_ir.c:3306-3325**. `IRMC_ARENA_ALLOC` → `ZC_COLOR_ARENA` at ~3333.
  **`alloc(T,n)` reuses the ALLOC_PTR arm** (POOL color, escapable).

### 8.3 Escape gate (why alloc_ptr escapes, arena doesn't)

- `is_from_arena` / `is_arena_derived` set at arena alloc — **checker.c:4670,
  4699-4718**.
- Store-to-global / store-through-param rejection — **checker.c:4702-4713**.
  NOTE the rejection only fires when the assignment VALUE is a bare `NODE_IDENT`
  (the temp-var form); the DIRECT `global = arena.alloc_slice(...)` form only
  *taints* (checker.c:4645-4675) and does NOT reject — a known gap (§11).
- Cross-function return blocked via summary `returns_color == ZC_COLOR_ARENA` —
  **zercheck_ir.c:4113, 4284-4290**.
- `alloc` must be **POOL-colored and never set `is_from_arena`** → escapes like
  `alloc_ptr`.

### 8.4 Slice representation + construction

- `typedef struct { T* ptr; size_t len; } _zer_slice_T;` — **emitter.c:4878-4888**
  (generic form ~3739).
- Subslice construction `(_zer_slice_T){ &buf[start], end - start }` —
  **emitter.c:2387-2389**. This is the exact compound-literal shape `alloc(T,n)`
  and the coercion emit.
- Arena `alloc_slice` emission (the `[*]T {ptr, len}` build, `.len = n`) —
  **emitter.c:5694-5712** (IR path) and **emitter.c:1958-1998** (AST path). Swap
  `_zer_arena_alloc` → a heap `calloc` helper for `alloc(T,n)`.
- Auto-slab `alloc_ptr` emission — **emitter.c:5718** (IR) / **5565-5566** (slab)
  / **2003-2044** (AST).
- Runtime helpers `_zer_slab_alloc` / `_zer_slab_free_ptr` (hand-emitted string
  literals) — **emitter.c:5260-5331, 5318**. Add `_zer_heap_alloc` /
  `_zer_heap_free` (calloc/free) siblings here.

### 8.5 IR_CAST alias / alloc_id group (the UAF-through-view core)

- `ir_snapshot_alias` / `ir_apply_alias` (copy alloc_id + provenance) —
  **zercheck_ir.c:647-673**.
- `IR_CAST` Phase F makes the dest share the source's alloc_id —
  **zercheck_ir.c:2578-2623**, the snapshot/apply at **2616-2622**. The
  `[*]u8→[*]T` coercion registers the view alias HERE (same edge).
- `ir_mark_freed` alloc_id-group loop (`if (h->alloc_id == aid) ...`) —
  **zercheck_ir.c:2012-2031**. `free(region)` → marks the whole group FREED.

### 8.6 Leak pass

- Exit leak scan of live handles; SKIPS `ZC_COLOR_ARENA` only —
  **zercheck_ir.c:5530-5562** (skip at 5531; comment at ~86).

### 8.7 Alignment trap (for mid-region views)

- BUG-489 `@inttoptr` alignment trap `if (addr % align != 0) _zer_trap(...)` —
  **emitter.c:3111-3116**. Reuse the pattern for a non-whole-allocation
  `[*]u8→[*]T` coercion.

### 8.8 The TWO emitter dispatch paths (the recurring segfault trap)

Every intrinsic/builtin-call needs a handler in BOTH:
1. AST path — `emitter.c` around **line 2754** onward (and the alloc cluster
   ~1866-2044).
2. IR-rewritten path — `emitter.c` around **line 6451** onward (and the alloc
   cluster ~5561-5721).
Miss one → the IR rewriter falls through to a placeholder (`/* @name */ 0`) that
segfaults at runtime. Verify: `grep -n '"alloc"' emitter.c` should show TWO hits
per method. `tools/audit_matrix.sh` / walker audits help.

---

## 9. Implementation plan + flagged verification landmines

### 9.1 Sequencing (vertical slice first)

1. **`alloc(T)` → `?*T`.** Parse `alloc` as a builtin call taking a type arg;
   route to the SAME `find_or_create_auto_slab` + `?*T` typing as `alloc_ptr`;
   emit the SAME `_zer_slab_alloc`. Alias `alloc_ptr → alloc`. Proves the surface
   changes NOTHING about tracking. Full `make check` green.
2. **`alloc(T, n)` → `?[*]T`.** New heap-slice path: `calloc(n, @size(T))` →
   `(_zer_slice_T){ptr, n}` wrapped in `?[*]T`. Classify as `IRMC_ALLOC_PTR`-like
   (POOL, escapable). **Implement the new lattice element: a slice-typed local
   carries an `alloc_id` and propagates through subslice/copy/assign.** Verify
   landmine #1 (below) FIRST.
3. **`free(x)`.** Dispatch on `*T` (→ `_zer_slab_free_ptr`) vs `[*]T` (→
   `free(x.ptr)` + mark alloc_id group FREED). Alias `free_ptr → free`.
4. **`[*]u8 → [*]T` coercion.** Target-driven, at typed declaration/assignment
   sites only (initially). Emit `(_zer_slice_T){ (T*)bs.ptr, bs.len/@size(T) }` +
   alignment trap; register the alloc_id alias via the IR_CAST edge. Verify
   landmine #3.
5. **Migrate + delete aliases.** find-replace `alloc_ptr`→`alloc`,
   `free_ptr`→`free`; remove the aliases; `make check` green.
6. Docs (reference.md, CLAUDE.md quick-ref, this file → mark IMPLEMENTED),
   BUGS-FIXED.md, negative tests in `tests/zer_fail/`, positive in `tests/zer/`.

### 9.2 The flagged verification landmines (attack these hardest)

1. **Subslice `alloc_id` inheritance for a slice-typed local.** `buckets[a..b]`
   MUST keep the region's `alloc_id`. A view that loses it escapes tracking →
   silent UAF. This is the one genuinely-new lattice element. Verify FIRST with a
   free-then-use-subview negative test.
2. **Cross-fn struct-field free (P9 / BUG-737).** `free(h.buckets)` where
   `buckets` is a pointer/slice field of a BY-VALUE struct param. The escape
   summary (`frees_param`) must descend into the projection, not match only a
   bare ident. The codebase ALREADY shipped a UAF through this exact shape (P9);
   re-run the full escape-sink matrix.
3. **Coercion registered in BOTH emitter paths (§8.8).** The recurring
   "two handlers or it segfaults" trap.

### 9.3 Files touched

- **checker.c** — `alloc`/`free` builtin dispatch + typing (`?*T` / `?[*]T` /
  void); POOL color (never `is_from_arena`); the `[*]u8→[*]T` coercion legality
  at typed sites.
- **emitter.c** — both dispatch paths; `_zer_heap_alloc`/`_zer_heap_free`
  runtime helpers; the coercion emit + alignment trap.
- **zercheck_ir.c** — `IRMC_*` arm reusing `IRMC_ALLOC_PTR`; `free` → alloc_id
  group FREED; the slice-carries-alloc_id propagation; the coercion alias edge.
- **parser.c** — `alloc`/`free` as builtin-call forms (if not already
  method-parsed).
- **runtime** — calloc/free helpers.
- **tests** — `tests/zer/` positive (single, slice, escape-in-returned-struct,
  custom bump allocator), `tests/zer_fail/` negative (free-then-use region,
  free-then-use view, double-free, leak, cross-fn struct-field free, coercion
  length overflow attempt).

### 9.4 Proof / oracle impact

EXTENDS `proofs/.../lambda_zer_handle/handle_flow_lattice.v` — states
{UNINIT/ALIVE/FREED/MAYBE}, merge = JOIN, use-requires-ALIVE all UNCHANGED. Only
new element: a slice-typed local carries + propagates `alloc_id` (an alias edge
already in the pointer-cast model). **No new oracle.** The alignment trap is a
runtime (BUG-489-class) check, outside the static proof. Follow the MAX-ORACLE
discipline if strengthening: the "length is allocation-derived" invariant is the
property to certify; model it as a hypothesis, not assume it.

---

## 10. How locked the current allocator layer is (the motivation, quantified)

Every allocator method is hand-wired into ~8-10 disjoint sites across 3 files,
with NO table/registry/shared abstraction. For one new shape you edit:
- **Checker:** 2 dispatch clusters (per receiver kind, memcmp'd) +
  a second copy on the NODE_FIELD path (checker.c ~6244) + arena-escape
  special-cases by method name (checker.c:4654-4655, 10013, 10590, 11979).
- **Emitter:** BOTH dispatch paths (AST ~1866-2044, IR ~5561-5721).
- **zercheck:** an IRMC enum value + a classify arm + a handler branch + the
  receiver-type whitelist.
- **Escape rule** (arena-color vs escapable).
- **Runtime helper** (hand-emitted C string).

"Locked" = one allocator shape ≈ 8-10 edits, none reusable by ZER-level code.
The universal `alloc` collapses the *default* path to ONE builtin; the `[*]u8→
[*]T` coercion lets custom allocator KINDS be ZER library code (bounded by
no-function-generics, §7.7).

---

## 11. Compiler bugs found during this exploration

Real, verified, worth logging in `docs/limitations.md` (not yet done — do it if
you touch these areas):

1. **Bare `orelse return;` inside a `?T`-returning function produces a wrong
   None.** Isolated repro:
   ```zer
   ?u32 get_it() { *Entry e = slot orelse return; return e.value; }
   ```
   With `slot` null, the caller sees `get_it()` as HAVING a value when it should
   be None. Every prior `?T` function used explicit `return null;`; the bare form
   in a `?T` function is broken. (Block form `orelse { ...; return null; }` is
   fine.)
2. **Global `Arena` in-place `over()` doesn't initialize** → `alloc_slice`
   returns None at runtime (§3.4). Only `Arena x = Arena.over(buf)` (capture the
   return) works. The method form `garena.over(buf)` (discard return) leaves the
   global uninitialized.
3. **`global = arena.alloc_slice(...)` (direct form) compiles when it should
   reject.** The escape rejection (checker.c:4694-4711) only fires when the
   assignment VALUE is a bare `NODE_IDENT` (temp-var form); the direct call/orelse
   form only *taints* (checker.c:4645-4675). A false-negative escape gap (though
   the runtime bug #2 masks it for global arenas).
4. **`[*]?*KvEntry` slice element emits broken C** — "incompatible types … struct
   anonymous" from GCC. Needs a named wrapper struct (`struct Bucket { ?*KvEntry
   head; }`) — an anonymous-struct-in-slice-typedef emitter gap.
5. **`subst_typenode` `TYNODE_HANDLE` case doesn't recurse into `handle.elem`** →
   ANY `container` field of shape `Handle(T)`/`?Handle(T)` fails with "undefined
   type 'T'" (breaks self-referential `container Chained(T){?Handle(Chained(T))
   next;}` and more). Separate from the depth-32 recursion guard. (Found by the
   §3.6 judge-panel.)
6. **Named `const` not accepted as an array size** (§3.3) — only a literal or a
   `comptime` call. Mild wart.

---

## 12. Relationship to the 4-model architecture and universal_pointer.md

- **Safety model:** `alloc`/`free` is **Model 1 (State Machine)** — the handle-
  state lifecycle (ALIVE→FREED/MAYBE_FREED), the same model as `alloc_ptr`,
  `Handle`, and move-tracking. It adds NO new model. The coercion is a Model-1
  alias edge. See CLAUDE.md "ZER Safety Architecture — 4 Models".
- **universal_pointer.md:** that doc is the *pointer-lifetime/escape* axis
  (`keep`, compile-time, PART 5 decided). THIS doc is the *allocation* axis. They
  compose: `alloc` produces escapable heap pointers; `keep` governs whether a
  borrowed pointer may be stored. §6.5 of universal_pointer.md (user-extensible
  allocators) is the gap the `[*]u8→[*]T` coercion partially closes (bounded by
  no-function-generics). §6.5's honest blocker was "custom-allocator authors pay
  unsafe blocks for raw memory work" — the coercion removes that by keeping the
  length allocation-derived so no `unsafe` is needed.
- **Definition A / closure argument (CLAUDE.md goal section):** `alloc` is a
  frozen-catalog typed boundary, not an `unsafe` escape hatch. The length is never
  user-forgeable, so it does not open the "infinite unsafe impedance" position ZER
  refuses. This is the property that keeps the whole thing inside the closure.

---

## 13. Glossary

- **`*T`** — single non-null pointer, ONE object, NO length; indexing `*T` is a
  compile error (silent-overflow ban). Only `[*]T` is indexable.
- **`[*]T`** — slice / fat pointer `{ptr, len}`, bounds-checked on every index
  against `.len`. Sound iff `.len` is TRUE.
- **`?T`** — optional; `?*T` = null-sentinel pointer; unwrapped via `orelse` /
  `if |v|`.
- **auto-slab** — a global `Slab` conjured lazily per struct type on first
  `Type.alloc_ptr()` (checker.c:1583). The "brainless" mechanism `alloc(T)`
  generalizes.
- **alloc_id** — a per-allocation id shared by all aliases; freeing any alias
  marks the whole `alloc_id` group FREED. The core of UAF/double-free tracking.
- **source_color** — `ZC_COLOR_POOL` (escapable, leak-checked) vs
  `ZC_COLOR_ARENA` (non-escaping, leak-skipped). `alloc` is POOL.
- **is_from_arena** — flag that makes a pointer non-escaping. `alloc` NEVER sets
  it.
- **length-derived / non-forgeable** — `.len` comes from an allocation count, an
  array length, a bit-range, or a floored `bytes.len/@size(T)` — never a user
  integer. The invariant that keeps slices safe.
- **the coercion** — target-driven `[*]u8 → [*]T` reinterpret at a typed site;
  replaces `@slice_as`. Length derived, alignment trapped, alloc_id inherited.
- **the un-lock** — making `alloc_slice` + custom allocator KINDS ZER library
  code instead of hardcoded builtins. Bounded by no-function-generics.

---

## Appendix A: every test program + its exact verified result

All compiled in Docker (`zer-check:latest`, `:ro` mount + tar-to-/build). Results
are literal.

**A1. alloc_ptr can't be indexed:**
`b[i]` where `b:*Bucket` → `error: cannot index a single pointer '*Bucket' as an
array … it would be a silent buffer overflow. Use '[*]Bucket'`. COMPILE FAIL.

**A2. alloc_ptr takes no size:** `Bucket.alloc_ptr(4)` → `error: Bucket.alloc_ptr()
takes no arguments`. COMPILE FAIL.

**A3. Arena slice can't escape:** storing an `arena.alloc_slice` result in a
returned struct → `error: cannot store arena-derived pointer 'b' through pointer
parameter 'ht' — pointer will dangle when arena is reset` + `error: cannot return
arena-derived pointer 'ht'`. COMPILE FAIL.

**A4. Global arena alloc_slice:** `garena.over(gbacking); garena.alloc_slice(...)`
→ COMPILES, but returns None at RUNTIME (prints "NONE", exit 9). Local
`Arena ar = Arena.over(backing)` → len 4, RUNS. (Bug #2.)

**A5. const as array size:** `?*Entry[N] buckets;` (N a const) → `error: array
size must be a compile-time constant`. Literal `[8]` works. COMPILE FAIL.

**A6. Slice-into-fixed-global-array (the working "today" answer):**
`ht.buckets = backing[0..size]` where `backing:Bucket[1024]` → COMPILES, RUNS,
`ht.buckets.len == 4`, table returns fine. (Needs the `[1024]` backing bound.)

**A7. Full working chained hash map, no Handle, no Arena, fixed literal bucket
array + auto-slab entries** (`?*Entry next`, `Entry.alloc_ptr()`,
`Entry.free_ptr()`) → COMPILES, RUNS, all insert/get/delete/collision assertions
pass, exit 0. This is the shipped-today fallback.

**A8. `[*]?*KvEntry` slice element** → GCC "incompatible types … struct
anonymous". Needs a named `Bucket` wrapper. (Bug #4.)

---

## Appendix B: the target `kv_table_create` (what `alloc` makes compile)

Once `alloc` ships, this compiles and runs literally — runtime `size`,
`[*]Bucket` with NO bound, no arena, no fixed array, table returned:

```zer
struct KvEntry { [*]u8 key; [*]u8 value; ?*KvEntry next; }
struct Bucket  { ?*KvEntry head; }
struct KvTable { u32 size; [*]Bucket buckets; }   // [*]T — no bound, ever

?*KvTable kv_table_create(u32 size) {
    *KvTable ht = alloc(KvTable) orelse return;    // kmalloc(table)  (was KvTable.alloc_ptr())
    ht.size = size;
    ht.buckets = alloc(Bucket, size) orelse {      // kcalloc(size, sizeof(Bucket)) — auto-zeroed
        free(ht);                                  // if NULL: free ht, return NULL
        return null;
    };
    return ht;                                     // escapes — heap-derived, POOL-colored, allowed
}

void kv_table_free(*KvTable ht) {
    free(ht.buckets);                              // free the bucket array
    free(ht);                                      // free the table
}
```

Bare-sugar future form (§6.1), once target-inference is added:
```zer
ht.buckets = alloc(size);   // Bucket inferred from the [*]Bucket target
```

---

*End of dump. When this is implemented, update the Status line at the top, mark
the §9 steps done, move the §11 bugs to docs/limitations.md (or fix them), and add
the reference.md / CLAUDE.md quick-ref entries. Until then: the design is locked,
the machinery is mapped, the dead ends are proven — build it.*
