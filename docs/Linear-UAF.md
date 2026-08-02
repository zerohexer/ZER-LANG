# Linear UAF — can ZER reach ZERO runtime use-after-free checks?

**Status: DESIGN INVESTIGATION, nothing implemented.** Recorded 2026-08-03.

This document exists so a fresh session does not re-derive any of it. Everything below is
either **MEASURED** (a command was run, the output is quoted) or **PROPOSED** (a design
argument, not yet built). Those two are labelled everywhere, deliberately — the single
biggest way to waste a session here is to read a proposal as an existing fact.

**The question that started it:** ZER catches use-after-free at compile time in most cases,
but `Handle(T)` carries a RUNTIME generation check as a backstop. That check has a finite
generation space (2^32 per slot). Can pure ZER-to-ZER code reach **zero** runtime UAF
checks, so the generation counter — and its wraparound — stops mattering entirely?

**The answer: YES, and the mechanism already exists in the language.** The cost is that
handles stop being freely copyable. The blocker is not decidability; it is an emission
ordering fact (§6) that makes the first step architectural rather than a patch.

---

## Table of contents

1. [What a Handle actually is](#1-what-a-handle-actually-is-measured)
2. [The wraparound exposure, and why the doc understated it](#2-the-wraparound-exposure-measured)
3. [Why you cannot "reset" or "flush" the generation](#3-why-you-cannot-reset-the-generation-argued)
4. [What is ALREADY static — the 9-shape measurement](#4-what-is-already-static-measured)
5. [The check is UNCONDITIONAL, not a fallback](#5-the-check-is-unconditional-measured--the-key-finding)
6. [The architectural blocker: zercheck runs AFTER emit](#6-the-architectural-blocker-measured)
7. [The four-step plan, and which steps are runtime](#7-the-four-step-plan-proposed)
8. [The three routes to full static safety](#8-the-three-routes-to-full-static-safety-argued)
9. [Linearity: why it makes safety PER-FILE provable](#9-linearity-the-load-bearing-argument)
10. [The annotation question — which kind is allowed](#10-the-annotation-question-argued)
11. [Open bug found during this investigation](#11-open-bug-found-during-this-investigation-measured)
12. [Methodology: three vacuous probes in one session](#12-methodology-three-vacuous-probes-in-one-session)
13. [Summary for a fresh session](#13-summary-for-a-fresh-session)

---

## 1. What a Handle actually is (MEASURED)

`Handle(T)` is a `u64`, **not** a pointer. Layout, from `emitter.c:1213`:

```c
/* BUG-390: Handle = gen(32) << 32 | index(32) */
```

So **32-bit index, 32-bit generation**. (A common wrong guess is 16/16 — it is not.)

The runtime check, emitted into every program's preamble (`emitter.c:5685` region):

```c
static inline void *_zer_pool_get(void *slots, uint32_t *gen, uint8_t *used,
                                  size_t slot_size, uint64_t handle, size_t capacity) {
    uint32_t idx   = (uint32_t)(handle & 0xFFFFFFFF);
    uint32_t h_gen = (uint32_t)(handle >> 32);
    if (idx >= capacity || !used[idx] || gen[idx] != h_gen) {
        _zer_trap("use-after-free: handle generation mismatch", __FILE__, __LINE__);
    }
    return (char*)slots + idx * slot_size;
}
```

The free path (`emitter.c:5690` region):

```c
static inline void _zer_pool_free(uint32_t *gen, uint8_t *used,
                                  uint64_t handle, size_t capacity) {
    uint32_t idx   = (uint32_t)(handle & 0xFFFFFFFF);
    uint32_t h_gen = (uint32_t)(handle >> 32);
    if (h_gen == 0) return;              /* null handle: no-op */
    if (idx < capacity) {
        used[idx] = 0;
        gen[idx]++;
        if (gen[idx] == 0) gen[idx] = 1; /* skip 0: reserved for null handle */
    }
}
```

Three things to notice, all load-bearing later:

- Generation **0 is reserved** for the null handle, so the usable space is `2^32 - 1`.
- On wrap, `gen` silently returns to 1. **Nothing detects exhaustion.**
- The check tests `!used[idx]` as well as the generation, which catches the common
  free-then-use immediately — the generation only matters once the slot is **recycled**.

**Terminology note:** this is a *generational index*, the pattern used by Rust ECOSYSTEM
crates (`slotmap`, `generational-arena`). It is **not** how Rust the language works. Rust
has lifetimes and **no runtime UAF check at all** — which is precisely why Rust is the
existence proof in §8.

---

## 2. The wraparound exposure (MEASURED)

The hazard is ABA: a stale handle holding `(idx, G)` becomes valid again once slot `idx`
has been recycled `2^32 - 1` times and its generation returns to `G`.

`docs/4-27-2026-gaps.md:85` records this as **Gap 2 — Generation counter wraparound
(LOW theoretical)**, on the basis of "~12 years per slot at 10 free/sec".

**That classification was re-assessed 2026-08-03 and the doc was corrected.** The "LOW"
rests entirely on the assumed rate, which suits a sensor and not most of ZER's stated
target domain. Wrap time for ONE slot:

| Workload | rate | wrap |
|---|---|---|
| sensor (the original assumption) | 10/s | ~13.6 years |
| 1 kHz sampling loop | 1,000/s | **~50 days** |
| 10k packet/s network buffer pool | 10,000/s | **~5 days** |
| 100k/s DMA ring | 100,000/s | **~12 hours** |
| 1M/s hot loop | 1,000,000/s | **~1.2 hours** |

Packet buffers, DMA descriptors and high-rate sampling reach the wrap in **days or hours**,
and a firmware device running continuously for a month is ordinary.

**The failure mode is the worst kind available:** a stale handle whose generation happens to
match the recycled one passes the runtime check **silently**. No trap, no diagnostic. It is
the one UAF path that defeats both the compile-time analysis and the runtime backstop.

**Do not confuse two exhaustions:**

- **Slot exhaustion** (pool full) — already handled and loud: `alloc()` returns `?Handle`,
  you `orelse`. Not a safety issue.
- **Generation exhaustion** (wraparound) — the silent one. This document is about that.

---

## 3. Why you cannot "reset" the generation (ARGUED)

The intuitive mitigation is "flush" or "reset the counter when it is safe to do so". Every
version of this fails, and one of them is actively harmful. The reason is a circularity:

> **"Safe to reset" means "no stale handle exists". If you could prove that, you would not
> need the counter in the first place.**

Concretely, the most natural proposal:

> *Reset all generations to 1 when the pool is empty (all slots free).*

**This is worse than doing nothing.** A stale handle sitting in some variable still holds
`(idx, oldgen)`. Resetting does not destroy it — it aims the entire generation space back
at 1. Any stale handle with `gen == 1` now collides **immediately**, where before it needed
2^32 recycles. You would convert a 50-day exposure into an instant one.

Variants and why they fail:

| Idea | Verdict |
|---|---|
| reset when pool empty | **unsound**, and worse than not resetting (above) |
| reset with an epoch bump | needs bits you do not have in a `u64` handle |
| 24+40 split (16M slots, 1.1e12 gens) | sound, buys headroom, **does not remove the wall**: ~3.5 years at 10k/s, ~127 days at 100k/s; costs slots |
| 64-bit generation | needs a **128-bit handle** — you cannot fit it in the current `u64` |
| retire the slot on wrap | **sound**: mark the slot permanently dead. Collisions impossible. Pool shrinks; failure surfaces at `alloc()` — loud and recoverable |
| trap on wrap | **sound**: converts the silent UAF into a deterministic abort. ~2 lines |

**The only ways to actually remove it** are: a wider handle, refusing to recycle a slot once
its generation space is spent, or — the subject of the rest of this document — making the
runtime check not exist at all.

---

## 4. What is ALREADY static (MEASURED)

Before proposing anything, the existing coverage was measured. Nine UAF shapes were compiled
and classified by whether they are rejected at compile time or reach the runtime path.

**Result: 8 of 9 are pure compile-time.**

| # | Shape | Result |
|---|---|---|
| A | same-fn use after free | COMPILE-TIME — `zercheck: use after free` |
| B | double free | COMPILE-TIME — `zercheck: use after free` |
| C | conditional free then use (MAYBE_FREED) | COMPILE-TIME |
| D | cross-fn free via param | COMPILE-TIME |
| E | cross-fn free of param FIELD | COMPILE-TIME *(fixed 2026-08-02)* |
| F | interior pointer after free | COMPILE-TIME |
| G | alias via call return | COMPILE-TIME *(fixed 2026-08-02)* |
| H | dynamic index: `free(a[i])`, use `a[j]` | **auto-guard** (see below) |
| I | leak (ALIVE at exit) | COMPILE-TIME |
| — | `*opaque` round-trip then use | COMPILE-TIME |

### H deserves its own note — there is a THIRD mechanism

H does not reach the generation check. The compiler emits an **auto-guard** and warns:

```
warning: auto-guard inserted for 'a' — element may have been freed at dynamic index.
         Add explicit index guard for zero overhead
```

The guard returns early, so the read never happens. The program exits 0, and that is
**correct**, not a miss. (An earlier pass of this investigation wrongly recorded H as
"caught by neither layer" — the auto-guard was overlooked.)

So the layering is:

1. **zercheck** — static rejection (most shapes)
2. **auto-guard** — compile-time-inserted runtime branch, for dynamic-index frees
3. **generation check** — the backstop below both

---

## 5. The check is UNCONDITIONAL (MEASURED) — the key finding

This is the finding that reframes the whole question. **The generation check is NOT a
fallback for hard cases. It is emitted on every handle deref.**

Probe — a trivially provable program:

```zer
struct T{u32 v;}
Pool(T,4) p;
u32 main(){
    Handle(T) h = p.alloc() orelse { return 1; };   // ALIVE, provably
    h.v = 7;                                         // zercheck KNOWS h is alive here
    u32 x = h.v;
    p.free(h);
    if (x != 7) { return 2; }
    return 0;
}
```

**Result: 2 gen-check call sites emitted.**

zercheck *knows* `h` is ALIVE at `h.v = 7` — it would reject a use-after-free written
there — and the emitter calls the checked getter anyway.

### Contrast: bounds checking does it correctly

| probe | bounds-check emissions |
|---|---|
| proven index (`a[1]` on `u32[4]`) | 1 |
| unproven index (`a[i]`, i from a param) | 1 |

Bounds checking elides on proof and guards only what it cannot prove. That is CLAUDE.md's
**"prove, don't guard"** rule (`CLAUDE.md:1484`) working as designed.

**Handles never got that treatment.** Every deref pays whether or not the compiler already
proved it safe. This is also the cause of the known perf note (`CLAUDE.md:1500`):

> Handle gen check: ~60-130% in synthetic microbenchmark (tight loop doing nothing but
> pool.get), <5% in real code with actual computation per access

### How much code depends on the runtime path (MEASURED)

A first measurement said "487/487 programs emit a gen check (100%)". **That number is
meaningless** — the runtime is emitted into every program's preamble whether used or not.

Counting real CALL SITES (occurrences of `_zer_(pool|slab)_get(` excluding the `static
inline` definition):

```
compiled positives:             487
programs with a REAL call site:  87  (17.9%)
total call sites:               478
```

**Caveat for whoever acts on this:** 87 is the count of programs that call the checked
getter *today*, when the check is unconditional. It is **not** the count of programs that
genuinely need it. That second number is unknown and cannot be known until elision (§7 step
1) lands. Do not treat 87 as the size of the residual.

---

## 6. The architectural blocker (MEASURED)

Elision requires the emitter to ask zercheck "is this handle ALIVE here?". **It cannot,
because zercheck has not run yet.**

From `zerc_main.c`:

```
705:  emit_file(&emitter, m->ast);              <- emission happens HERE
726:  zercheck_ir(&zc_ir, zerc_ir_hook_funcs[i]);  <- analysis happens AFTER
```

The `ir_hook` (`zerc_main.c:196`) **only collects**:

```c
zerc_ir_hook_funcs[zerc_ir_hook_count++] = ir_func;
```

It stores the IRFunc; it does not analyze it. zercheck runs on the collected list after
`emit_file` has finished.

### There is a STALE COMMENT that says the opposite

`zerc_main.c:614` claims:

> each function's IR is analyzed RIGHT AFTER ir_lower_func, **BEFORE emit walks it**

**That is wrong.** The code at 705/726 is authoritative. Anyone trusting the comment will
design elision as a trivial patch and discover the problem mid-implementation.

### Why it cannot simply be reordered

The same comment block explains the real constraint, and this part IS accurate:

> `ir_lower_func` destructively rewrites AST (`pre_lower_orelse` replaces `NODE_ORELSE` with
> `NODE_IDENT`). Calling `ir_lower_func` twice on the same AST corrupts emission (idents
> lose their mappings to re-generated locals). The hook ensures exactly one `ir_lower_func`
> call per function — emitter and zercheck_ir share IR.

So the three options are all architectural:

1. **Analyze inside the hook** (what the comment claims already happens) — thread the
   analysis in rather than collecting.
2. **Two-pass emit** — analyze the collected IR, then emit. Costs a second walk.
3. **Side table** — record per-site elision decisions during analysis, consume on a later
   emit pass.

**None is a patch.** Budget accordingly.

---

## 7. The four-step plan (PROPOSED)

| Step | What | Runtime? | Cost | Status |
|---|---|---|---|---|
| **1** | Elide the check where zercheck proved ALIVE | **No** — removes runtime code | architectural (§6) | not started |
| **2** | Measure the residual call sites | **No** — pure analysis | free, once 1 lands | blocked on 1 |
| **3** | Reject the unprovable forms | **No** — compile-time rejection | needs 1+2 | blocked |
| **4** | Trap on wrap | **Yes** — one comparison in `_zer_pool_free` | ~2 lines | not started |

**Only step 4 adds runtime code.** Steps 1 and 3 subtract it. Step 2 changes nothing.

### Does the wraparound worry disappear?

**If 1+3 fully land — yes, completely.** Not because the counter got bigger, but because
**nothing reads it any more**:

- After **1**, a proven-ALIVE deref has no check, so the counter cannot be consulted there.
  zercheck's proof is what makes it safe.
- After **3**, every unprovable site is rejected at compile time.
- Therefore every surviving deref is provable, every check is elided, and a stale generation
  has nothing to fool. Step 4 becomes dead code.

**Conditional on step 3 being fully achievable**, which is what step 2 exists to determine.
If some legitimate patterns are genuinely unprovable with no alternative form — a handle
stored in a global and freed by a callback is the obvious candidate — the runtime path
survives for that residual, and so does the wraparound exposure **for that residual only**.
That is the case step 4 covers.

### Recommended order, and why it is not 1-2-3-4

**Do step 4 first.** Not because it is the best fix — it is the cheap floor — but because it
is the only one not gated behind §6, and it closes the failure mode that cannot be
diagnosed. Two lines, zero cost on the happy path.

### The caveat on step 1 that must not be lost

**Today the runtime check is a backstop for zercheck being wrong.** If the analyzer has a
hole, the generation check may still catch the UAF at runtime. Elision removes that second
line of defence and makes the static analysis a single point of failure.

The 2026-08-01/03 sweep found **45 real bugs in exactly that analyzer**, several of them
accept-unsafe. This is not a theoretical concern.

**Suggested resolution:** elide in release builds, keep the check in a debug/hardened mode.
That preserves the backstop where you want it and gets the performance where you need it,
and it is how bounds checking is conventionally handled.

---

## 8. The three routes to full static safety (ARGUED)

**The existence proof is Rust:** zero runtime UAF checks, achieved entirely by rejecting
what it cannot prove. So the goal is not theoretically blocked. Rice's theorem forbids
*precision* (accepting every safe program), never *soundness* — you may always reject what
you cannot prove.

Each route costs something specific. **The runtime check is what ZER currently pays INSTEAD
of paying one of these.**

### Route 1 — Linearity (`move struct`)

A handle consumed by `free` cannot be used after, and cannot be copied into a stale alias.
Fully static, no generation needed.

**Cost:** handles stop being freely copyable — which is the entire reason handles exist.
They are `u64` values precisely so they can be stored in collections without lifetimes. Make
them linear and you have rebuilt `*T` + ownership.

### Route 2 — Region / arena

No per-object free at all; bulk reset. Slots never recycle individually, so **ABA is
structurally impossible and the generation counter has nothing to do.** Fully static, and
`Arena` already exists in ZER.

**Cost:** you cannot free one object.

### Route 3 — Ban the unprovable forms

The `*T` → `[*]T` move: reject handles in globals, across `*opaque`, at dynamic-index frees.

**Cost:** expressiveness, and it is partial. Needs step 2 to size.

---

## 9. Linearity — the load-bearing argument

**Why linearity specifically, and not "better analysis":**

ZER's architecture **bans whole-program analysis** (CLAUDE.md: "Whole-program analysis —
BANNED from architecture. zercheck is per-file with summaries").

Aliasing is exactly what forces whole-program analysis. If a handle can be freely copied,
proving liveness at a use site requires knowing every copy's fate, anywhere in the program.

**Linearity makes safety PER-FILE provable**, because ownership is local: you never need to
know what other code does, only whether ownership left your scope. That is precisely the
model this architecture already requires.

### Every piece already exists (MEASURED where noted)

| Need | Mechanism | Verified? |
|---|---|---|
| no stale copies | `move struct` → `HS_TRANSFERRED` | **MEASURED** — see below |
| take out of a collection | `?Handle` + `null` (Rust's `Option::take`) | partially — blocked by §11 bug |
| cross-function | `FuncSummary` / `frees_param` / `frees_param_field` | exists |
| funcptr may free | argument-precise barrier | exists |
| slots never recycle | `Arena` | exists |

**MEASURED — linearity over a handle works today:**

```zer
struct T{u32 v;}
Pool(T,4) p;
move struct Owned { Handle(T) h; }
void consume(Owned o) { p.free(o.h); }
u32 main(){
    Owned o;
    o.h = p.alloc() orelse { return 1; };
    consume(o);
    return p.get(o.h).v;      // use AFTER the move
}
```
→ `REJECTED — zercheck: use after free`

**MEASURED — the aliasing case that was expected to NEED the runtime check is already
statically rejected:**

```zer
?Handle(T)[4] a;
a[0] = p.alloc() orelse { return 1; };
Handle(T) h = a[0] orelse { return 2; };
p.free(h);
Handle(T) h2 = a[0] orelse { return 3; };   // stale ALIAS still in the array
return h2.v;
```
→ `REJECTED — zercheck: use after free`

That result is genuinely encouraging for the thesis: the hardest-looking case is already
covered statically.

### What you lose

The pattern that dies is **"same handle stored in two places"** — e.g. a task in a scheduler
queue AND in a lookup table. With linear handles you restructure to one owner plus indices
or weak references. **This is the same cost Rust imposes**, and it is the main thing to
weigh before committing.

### The proposed shape

Make the runtime check **conditional on which handle you asked for**:

| Form | Static proof | Runtime check |
|---|---|---|
| linear / `move` handle | yes — consumed by free | **none emitted** |
| copyable `Handle(T)` | not in general | gen check (the price of copyability) |
| arena allocation | yes — no per-object free | **none, structurally** |

**The principle:** you pay the runtime check exactly when you asked for the property that
makes static proof impossible. No new concepts are required — every piece is in the language.

**Still gated on §6.** Right now even the move-wrapped handle emits the check, because the
emitter cannot tell the two apart.

---

## 10. The annotation question (ARGUED)

"Force the user to annotate" is a reasonable instinct but splits into two very different
things, and only one is compatible with ZER's thesis.

| Kind | Example | Verdict |
|---|---|---|
| **Asserting a fact** | "trust me, this handle is live" | **Rejected.** Unverifiable contract — Definition B ("user contracts can be wrong"). This is `unsafe` renamed. |
| **Requesting a stricter regime** | "apply ownership tracking to this type" | **Fine.** The compiler then VERIFIES it. Nothing is trusted. |

`move struct` is already the second kind. It claims nothing; it opts into a discipline that
gets checked.

### Why the `*T` → `[*]T` precedent works

The analogy is strong, but the reason matters, because it decides the above.

`[*]T` is **not a promise**. It is a **different representation that carries the length**.
The user does not assert "trust me, this is in bounds" — they switch to a type where the
property becomes *checkable*, and the compiler still verifies it.

**So: force-restructure = right. Force-annotate-as-assertion = wrong.**

For rejection (route 3 / step 3) to work the way `*T` does, there must be a **provable
alternative form** to push users into. ZER has candidates — `Arena`, a linear handle, a
scoped handle that cannot be stored. **If a case has no such alternative, rejecting it just
makes the language unable to express something, with no path forward.** That is the test to
apply to each rejection before adding it.

### Is the guarantee robust to a programmer who annotates WRONG? (MEASURED 2026-08-03)

The question a fresh session will ask: *if the design leans on `move struct`, can a
programmer who marks things wrongly — or not at all — create a UAF?* Measured on current
`zerc`:

| Programmer error | Result |
|---|---|
| Marks `move`, then uses after transfer | **REJECTED** — `use after free: 'o' is transferred` |
| Does NOT mark `move`, makes a stale copy, frees, reads the copy | **REJECTED** — `use after free: 'b' is freed` |
| Marks `move` but never consumes it | ACCEPTED (a LEAK gap — see below; not a UAF) |

**The annotation is not load-bearing for SAFETY.** The second row is the important one: with
NO annotation at all, the stale copy is still caught. The analysis never trusts the
annotation — it derives the fact. That is the Definition A discipline holding: ZER does not
accept a claim it cannot verify, so a wrong claim can only produce a compile error, never
unsafety.

What the annotation *is* load-bearing for: ergonomics, and (under §7) whether a runtime check
is emitted at all. Getting it wrong costs you over-rejection or a runtime check — never a UB
hole.

**So the honest formulation of "can ZER-to-ZER be fully static?" is:**

> The guarantee does not rest on the programmer at all. It rests **entirely on the analyzer
> being sound.**

That is the real risk, and it is not hypothetical: the 2026-08-01/02 sweep fixed **45 bugs**
in exactly that analyzer, several of them accept-unsafe. So "all UAF static" is a claim about
the **design**, and its truth in practice is exactly as good as the inference is sound.

This is precisely why CLAUDE.md's verification endgame (100%-sound checker via core λZER)
matters more for ZER than for Rust: Rust has human-written annotations sharing the burden;
ZER has nothing but the inference. Do not quote the static-safety claim without this
sentence attached.

### What the residual "runtime" becomes under the linear design (ARGUED)

Worth separating, because it is easy to read as a contradiction. Under linearity +
`?Handle`/`null`, taking a handle out of a collection NULLS the slot, so no stale handle
exists to check. The remaining runtime branch is the **`orelse` the programmer wrote** — an
ordinary optional unwrap, i.e. *program logic*, not a hidden UB backstop that traps.

That is a different kind of thing from a generation check:

| | generation check | `orelse` on `?Handle` |
|---|---|---|
| Written by | compiler, invisibly | programmer, visibly |
| On failure | `_zer_trap` — abort | takes the branch you wrote |
| Can be wrong | yes — ABA on wraparound (§2) | no — it is a value test |

"Zero runtime UAF checks" therefore does NOT mean "zero branches". It means no hidden trap
whose correctness depends on a counter that can wrap.

### Aliasing cases that are ALREADY static (MEASURED)

The shapes most likely to force a runtime check turn out to be handled today:

| Shape | Result |
|---|---|
| free through an array element, then read that element | **REJECTED** — `'a' is freed` |
| free `a[j]` (dynamic index), then read `a[1]` | **REJECTED** — `'a' is maybe-freed` |

The second is the conservative may-alias answer, which is the correct one. This is
encouraging for route 3: the hard aliasing cases are not the obstacle.

### One gap found while measuring (MEASURED)

`move struct Own { Handle(T) h; }` allocated and never consumed is **ACCEPTED** — the leak
check does not see through the move-struct wrapper. That is a **leak, not a UAF** (no
memory-unsafety), but it contradicts CLAUDE.md's "leaks are compile errors" and should be
filed if anyone builds on the linear design, since the linear design makes that wrapper the
primary idiom.

---

## 11. Open bug found during this investigation (MEASURED)

**Assigning an UNWRAPPED handle into an OPTIONAL location is accepted by the checker and
emits invalid C.**

```zer
?Handle(T)[4] a;
a[0] = p.alloc() orelse { return 1; };
```

- Checker: **accepts**, exit 0, emits the `.c`
- GCC: `error: incompatible types when assigning to type '_zer_opt_u64 {aka struct <anonymous>}' from type 'uint64_t'`

The emitter writes a bare `uint64_t` where a `_zer_opt_u64` struct is required — the T→?T
coercion is missing.

### Shape isolation (MEASURED, re-run 2026-08-03)

An earlier pass concluded this was **Handle-specific**. That was WRONG, and the reason is
instructive: the control used a *plain* `u32` RHS, not an orelse-unwrapped one, so it never
exercised the failing path — a **vacuous control** (§12). Re-running with the RHS held
constant gives the real picture:

| Target | RHS | Result |
|---|---|---|
| `?Handle[]` element | plain `Handle` variable | ok |
| `?Handle` struct field | plain `Handle` variable | ok |
| `?Handle` scalar | orelse-unwrapped | ok |
| `?Handle[]` element | orelse-unwrapped | **ACCEPTED then GCC ERROR** |
| `?Handle` struct field | orelse-unwrapped | **ACCEPTED then GCC ERROR** |
| `?u32[]` element | orelse-unwrapped `u32` | **ACCEPTED then GCC ERROR** |

**Corrected root cause.** It is NOT about Handle. It is: an **orelse (statement-expression)
RHS assigned into a PROJECTED optional target** (array element or struct field) misses the
T→?T coercion. Type-independent — `?u32[]` breaks identically. Scalar targets are fine, and
a plain-variable RHS is fine, so the failure needs BOTH the projected target AND the
statement-expression RHS.

That matters for whoever fixes it: the fix is in the assignment-coercion path for projected
lvalues where the RHS is an orelse temp, **not** in Handle typing. Looking in the Handle
code would have been a dead end.

Reproduce (both must be present):

```zer
struct T{u32 v;}
Pool(T,4) p;
u32 main(){
    ?Handle(T)[4] a;
    a[0] = p.alloc() orelse { return 1; };          // projected target + orelse RHS
    Handle(T) h = a[0] orelse { return 2; };
    p.free(h);
    return 0;
}
```

**Class:** silent-until-gcc, same as §G1 / §G2 / §F4 in the 2026-08-01/02 sweep.

**Why it matters here:** it blocks precisely the `?Handle` + `null` take-out-of-collection
pattern that linear handles need (§9). Fix this before attempting the linear-handle design.

---

## 12. Methodology: three vacuous probes in one session

Recorded because it happened **three times in one day, hours after the class was documented
and a gate was built for it.**

`CLAUDE.md` "VACUOUS TESTS" defines the class: *a test whose pass condition is weaker than
its claim*. The `// expect-error:` gate now enforces it for `tests/zer_fail/`. **Ad-hoc
probes have no such protection**, and that is where all three failures happened:

1. **Syntax error read as a safety result.** A probe batch used `orelse return 1` — but
   `orelse return` is BARE in ZER (CLAUDE.md syntax rule 12). All nine probes failed with
   `error: expected ';'` and were reported as "COMPILE-TIME rejected". Every row was wrong.
2. **Leak check masking the type bug.** Isolating §11, five probes reported
   "checker-rejected" — they were rejected by the **leak** check (`allocated ... but never
   freed`), never reaching the coercion. Nearly concluded there was no bug. Fixed by making
   each probe free its handle.
3. **`$?` read from the wrong command.** `./zerc ... | tail -4; echo $?` reports `tail`'s
   exit, not `zerc`'s. Reported "exit 0" for a program that had failed.

**The rules that would have caught all three:**

- A non-zero exit does **not** mean your rule fired. **Read the message**, every time.
- A probe batch must **distinguish a parse error from a real rejection**. The harness used
  afterwards does exactly this:
  ```sh
  elif grep -qE 'error: expected|parse failed' log; then echo "!! BAD PROBE"
  ```
- Never pipe before reading `$?`.
- If a probe result surprises you, suspect the probe before the compiler.

---

## 13. Summary for a fresh session

**If you read nothing else:**

1. `Handle(T)` = `u64`, idx(32) | gen(32). Generation 0 reserved. Wrap is **silent**.
2. **8 of 9 UAF shapes are already fully static.** The runtime check is a backstop, plus an
   auto-guard layer for dynamic-index frees.
3. **The check is emitted UNCONDITIONALLY** — even where zercheck proved the handle ALIVE.
   Bounds checking elides on proof; handles never got that treatment. This is the single
   biggest finding.
4. **You cannot reset/flush the generation.** "Safe to reset" ⇒ "no stale handle exists" ⇒
   you did not need the counter. Resetting to 1 is *worse* than not resetting.
5. **Zero-runtime UAF for pure ZER is achievable.** Rust is the existence proof. The
   mechanism — linearity — already exists as `move struct` and is verified working.
6. **The blocker is emission ordering** (`zerc_main.c` 705 emit / 726 analyze), and the
   in-tree comment at line 614 says the opposite. Trust the code.
7. **Do trap-on-wrap first.** It is the only step not gated behind that reordering, and it
   closes the only silent-UAF path.
8. **Elision has a real cost**: it removes the backstop for analyzer bugs, and this analyzer
   had 45 found in one sweep. Consider release-elides / debug-keeps.
9. **A wrong annotation cannot create unsafety** — measured both ways (marked-and-misused,
   and not-marked-at-all: both REJECTED). The annotation is not trusted; the fact is
   derived. The guarantee rests entirely on the ANALYZER being sound, which is the real
   risk — 45 bugs were found in it in one sweep. Never quote the static-safety claim
   without that caveat.
10. **Fix the §11 coercion bug** before the linear-handle work — it blocks the
    take-out-of-collection pattern that design depends on. Root cause is a **projected
    optional target + an orelse statement-expression RHS**, type-independent — NOT
    Handle-specific (an earlier pass got that wrong via a vacuous control).

**Nothing in this document has been implemented.** Steps 1-4 are all proposals. The
measurements are real and were re-runnable at commit `359f1b1e`.

### Related documents

- `docs/4-27-2026-gaps.md` §Gap 2 — generation wraparound, with the corrected rate table
- `CLAUDE.md` "prove, don't guard" (`:1484`) — the bounds precedent elision would follow
- `CLAUDE.md` "VACUOUS TESTS" — the probe-discipline class from §12
- `CLAUDE.md` Ban Decision Framework — the test to apply before any rejection in step 3
- `docs/compiler-internals.md` "Verification endgame" — why soundness, not precision, is the
  target, and why Rice does not block this
