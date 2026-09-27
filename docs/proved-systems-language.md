# A Proof-First Systems Language (working name: unnamed)

**Status: DESIGN (2026-09-28). Nothing here is built.** This is a **pickup document**: it records a
long design discussion in full, including the wrong turns, so a fresh session can continue without
re-deriving anything. The owner's intent is captured in §0; the design in Part I; the verification
chain (CompCert, RefinedC, VST, licensing) in Part II; every question already settled, with its
answer and reason, in Part III.

It supersedes the "ZER-Ω" idea (KernelQ repo, `docs/perfect-compiler-omegaonly-RP.md`) and is a **separate
language from ZER**: different syntax, different checker, same bare-metal goal. ZER stays as it is.
Sibling documents:

| document | what it is |
|---|---|
| KernelQ `docs/perfect-compiler-omegaonly-RP.md` (not committed) | the original ZER-Ω vision; several claims in it are impossible (Part III, Q1) |
| KernelQ `docs/perfect-compiler-possible.md` (not committed) | the first "possible" rewrite: proved checkers per safety class + tactics. **Superseded by this document** on one point: here there are no safety classes |
| `docs/zer-unified-compiler.md` | the same verification-chain decision (Part II here) written for ZER itself, plus the cost-shape method for unifying ZER |
| KernelQ `refinedc/artifacts/LS41_checker_proof/` | the cost-shape checker: the one proved-checker instance already built and deployed |

---

## §0 How to use this document, and the owner's intent

### 0.1 Reading order

| you want | read |
|---|---|
| the design in two minutes | §0.2, §1 |
| what code looks like | §2 |
| how it is checked and compiled | §4, Part II |
| why there is no runtime | §6 |
| what is trusted | §8 |
| why a question is already closed | Part III (numbered Q1-Q35) |
| how safety is chosen, and how to prove a bug exists | §6.0, §6A |
| how it compares to SPARK, and what success would mean | §10.1, §10.2 |
| what to build first | §12 |

### 0.2 The owner's intent, in their terms

- **Bare metal, no runtime, no GC.** A runtime is "a pain": fast to prototype with, expensive in the
  long run (crashes in production, GC pauses). Everything that can be decided before the program runs
  is decided then.
- **Proof-based, like Coq, but the program is meant to be executed.** You can watch the proof unfold
  in the editor; in the end the program is emitted as C and run as a program, not as a proof.
- **The proof is the program's own spec,** in the way SPARK/Ada carries contracts, not a separate
  artifact somewhere else that has to be cited and connected by hand (the pain with RefinedC + a
  separate Coq development).
- **No safety classes.** Memory safety is not a separate analysis; it is part of correctness.
- **The language forces proof, not safety.** Every claim about a program ends in `Qed`, whichever
  way it points: "this driver has no use-after-free" and "this PoC does reach a use-after-free" are
  both proved theorems. Forcing every program to be memory-safe would be unfair and would stop the
  language from talking about vulnerabilities, attacks and bug reproductions; it is not the way to
  100% usability. Safety is what a build profile asks for, not what the language imposes (§6.0, §6A).
- **A small kernel checks everything,** like Coq's. No SMT search deciding the verdict: stable,
  checked proof terms.
- **Human-written proof steps instead of machine search.** The human knows the strategy; writing it
  down means the checker follows it instead of searching. This is where the speed of checking comes
  from (Q10-Q13 record exactly where that is true and where it is not).
- **Easy to use and strongest result.** Of the three syntax styles considered (§2.4), the owner chose
  the code-first style with contracts and inline proof steps, plus models on types.

---

# PART I — THE DESIGN

## §1 The one paragraph

A C-shaped systems language where **correctness is the only analysis**. Memory safety, bounds,
division, races are not checked by separate analyzers; they are proof obligations that fall out of
the program's specification, discharged in one program logic and checked by **a small trusted
kernel (Coq's)**. Specs are contracts on functions and models on types; proof steps are named inline
with `by`, so **nothing is searched in the verdict**. Proofs are ghost: checked, then erased. What
remains is plain C with **no runtime, no GC, no runtime checks**, compiled by **CompCert** (proved)
or GCC (trusted).

Three sentences to keep:

1. **Proofs check; code runs.** A proof tells you what will be true for every input; running the code
   produces the output for this input. The language never confuses the two (§3).
2. **A tactic chooses a proof, never a meaning.** `by` is never part of an interface and can never
   settle what the code leaves ambiguous (§5).
3. **Search is allowed in suggestions, never in the verdict** (§7).
4. **Proof is forced; safety is chosen.** Every claim is `Qed`, including claims that a bug exists
   (§6A). What a build demands (all safety proved, runtime checks allowed, or listed assumptions) is
   the user's profile (§6.0).

### 1.1 Kept and dropped

| kept | dropped |
|---|---|
| C-shaped surface, bare metal, emit C | safety classes as separate analyzers (ZER's 29 systems) |
| linear (use-once) types: in-place update with no GC | GC, reference counting, any runtime system |
| contracts (`requires` / `ensures` / `invariant`) and type models | SMT search in the verdict (F\*, Dafny, SPARK's route) |
| inline proof steps `by lemma` | runtime checks, traps, silent auto-guards |
| a small trusted kernel checking every proof | a large unverified checker as the thing trusted (ATS's route) |
| proof or refusal | "execution by recognition", zero annotations, one-axiom trust base (Ω doc) |
| a stack-depth bound (safety on bare metal) | an operation-count cost model (not needed here, Q20) |
| every claim proved (`Qed`), in either direction | safety forced on every program by the language |
| linear arithmetic by proved decision procedures (`lia`, `ring`) | SMT for arithmetic |

### 1.2 Why no safety classes

Memory safety is a corollary of correctness. No spec can be proved about code that reads freed
memory, because undefined behaviour makes every spec meaningless. In a separation logic (the logic
RefinedC, VST and Iris use), ownership is a **resource**: a freed pointer is no longer in your hands,
so no proof about it can exist. Checking correctness therefore checks safety, in one place, asked
once. This also removes the failure mode ZER's `docs/unified-oracle-proved-ZER.md` documents: one
safety question answered separately at N places and answered wrongly at one of them.

---

## §2 The shape of the language

Surface style: **code first, contracts on functions, models and invariants on types, proof steps at
the line they justify** ("B + type models", §2.4).

### 2.1 Types carry their meaning

```
struct Dict<N> lin { u32 keys[N]; u32 vals[N]; bool used[N]; }
    model     view(self) : Map<u32, u32>     // ghost: what this value means
    invariant inv(self)                      // holds between every call
```

- `lin`: the value is used exactly once; passing it on moves it, so an update can happen in place and
  nothing needs freeing by a runtime (Q17 explains why this, and not a functional core, is what
  removes the GC).
- `model`: the abstract meaning, ghost. A caller holding a `Dict` knows which map it holds, without
  re-stating it.
- `invariant`: travels with the type (SPARK calls this a type invariant), so functions do not repeat
  `requires inv(d)`.

### 2.2 Functions carry contracts; proof steps sit on lines

```
?u32 get(&Dict<N> d, u32 k)
    ensures result == d.model.find(k)
{
    usize i = slot(k);
    for (usize n = 0; n < N; n += 1)
        invariant all_probed_miss(d, k, n)          by (loop_step)
    {
        if (!d.used[i])     { return null; }        by (inv_no_gap)
        if (d.keys[i] == k) { return d.vals[i]; }   by (view_find_hit)
        i = next(i);
    }
    return null;                                    by (full_table_miss)
}

Dict<N> put(Dict<N> d, u32 k, u32 v)                 // takes d, gives it back (linear)
    requires d.model.has(k) || !full(d)
    ensures  result.model == d.model.add(k, v)
{ ... }
```

- A function with **no contract** still has its types: ownership, sizes, the invariant of every type
  it touches. Those alone generate the safety obligations (no out-of-bounds, no use of a moved value),
  so **Silver** (absence of runtime errors, Q15) comes from the types, not from a separate analysis.
- A **contract** is added only where more is wanted (**Gold/Platinum**: this function computes this).
- `by` names a lemma proved once, in a library or next to the code. The checker verifies that the
  named lemma closes the obligation at that line; it never looks for one.

### 2.3 What is emitted

```c
typedef struct { uint32_t keys[N]; uint32_t vals[N]; bool used[N]; } Dict;

bool dict_get(const Dict *d, uint32_t k, uint32_t *out) {
    size_t i = slot(k);
    for (size_t n = 0; n < N; n++) {
        if (!d->used[i])     return false;
        if (d->keys[i] == k) { *out = d->vals[i]; return true; }
        i = (i + 1) % N;
    }
    return false;
}
```

No checks, no ghost state, no proofs. The linear `Dict` is a plain pointer updated in place.

### 2.4 The three styles considered, and why "B + type models"

The same dictionary was sketched three ways. The owner judged B or C the simplest; B + type models
was chosen.

**A — Coq-style: definitions, then theorems.** (Coq, Bedrock2.)

```coq
Fixpoint probe {N} (d : &Dict N) (k : u32) (i : Fin N) (fuel : nat) : Option (Fin N) :=
  match fuel with
  | 0   => None
  | S f => if negb d.used[i] then Some i
           else if d.keys[i] =? k then Some i
           else probe d k (next i) f
  end.

Theorem get_correct : forall N (d : &Dict N) k,
  inv d -> get d k = Map.find k (view d).
Proof. intros N d k Hinv. unfold get. destruct (probe_spec d k Hinv) as [H|[H|H]]; ... Qed.
```

**B — code first, contracts, `by` at each line.** (SPARK, Verus.) §2.2 above, without type models.

**C — specs in the types (dependent / refinement types).** (F\*, Liquid Haskell.)

```
type Dict(N, m : Map<u32,u32>) = lin { keys: [u32; N], vals: [u32; N], used: [bool; N] }
    where view(self) == m && inv(self)

fn get(d: &Dict(N, m), k: u32) -> { r: ?u32 | r == m.find(k) } { ... }
fn put(d: Dict(N, m), k: u32, v: u32) -> Result< Dict(N, m.add(k, v)), Dict(N, m) > { ... }
```

| | A: Coq-style | B: code + `by` | C: specs in types | **B + type models** |
|---|---|---|---|---|
| easy to use | lowest | ✓ | ✗ | ✓ |
| strength (what can be proved) | same | same | same | same |
| composes for callers | medium | medium | ✓ | ✓ |
| error messages | fair | good (point at a line) | hard (errors are dependent-type errors) | good |
| contracts optional (Silver without writing specs) | ✗ | ✓ | hard | ✓ |
| maps onto RefinedC | medium | ✓ | medium | ✓✓ (RefinedC's own types are C types refined by a model, e.g. `dict<m>`) |

Reasons for B: people already know `requires`/`ensures` (SPARK, Verus, Dafny); a failing step points
at a line; proofs stay optional so the Silver/Gold balance holds; the surface maps almost one to one
onto RefinedC annotations; it stays C-shaped. What C contributes (a value carries its meaning) is
added to B as an **optional ghost model and invariant on a type**, without full dependent types.

### 2.5 Is it really as simple as B? Two roles

For the person writing the program, mostly yes. The Coq-style work does not disappear: it moves into
the lemmas that `by` points at. `by (view_find_hit)` is one easy line, but `view_find_hit` ("if slot
`i` holds key `k`, the model maps `k` to `vals[i]`") is a theorem someone proves, Coq-style.

| who | writes | feels like |
|---|---|---|
| **program author** (most code) | code + contracts + `by name` | **B** |
| **library / spec author** (once per data structure) | the lemmas behind the names | **A** (Coq) |

What keeps A-style work rare:

1. **Automation closes most obligations with no `by`:** bounds, ownership, linear arithmetic (by
   proved decision procedures, never SMT: `lia` for linear integer arithmetic, `ring` / `field`;
   each decides the goal or produces a certificate the kernel checks).
2. **A standard lemma library** for common models (maps, lists, arrays, ranges, list segments,
   trees, ring buffers), proved once and reused. Linked structures are the hardest part of the
   5-10x proof cost, so this library is essential, not optional.
3. **The suggester** (§7) writes the `by` when it can.

What is left is new facts about your own data structures, proved once. SPARK has the same split
(hard properties need hand-written ghost lemmas). **B hides A from the program author; it does not
abolish it.** Non-linear arithmetic needs a `by` (SMT is unreliable there too).

### 2.6 Three layers, visible only as used

Safety obligations are always generated, one per operation, but they are **implicit**. Proof text
appears only when a program claims more than safety, or when a step needs a named fact.

| layer | visible? | example |
|---|---|---|
| code | always | `m.len = 5;` |
| claims | only if you want more than safety | `requires`, `ensures`, `invariant`, `model` |
| proof steps | only where no default closes the goal | `by (sum_extend)` |

Most of a program looks like ordinary code (type-first declarations, `.` for field access through a
pointer, `alloc(T)`, from ZER's syntax). What the checker generates, written out as comments:

```
u32 read_len() {
    *Msg m = alloc(Msg);        // gives:  own(m), alive(m)
    m.len = 5;                  // needs:  own(m)          closed by lookup
    u32 n = m.len;              // needs:  own(m)          closed by lookup
    free(m);                    // needs:  own(m); takes own(m) away
    return n;
}
```

A claim makes proof text visible, and a `by` appears only for the step no default can close:

```
u32 sum_prefix([*]u32 xs, usize k)
    requires k <= xs.len
    ensures  result == sum(xs[0 .. k])
{
    u32 total = 0;
    for (usize i = 0; i < k; i += 1)
        invariant total == sum(xs[0 .. i])      by (sum_extend)   // human: the one real step
    {
        total += xs[i];                         // i < xs.len: lia from i < k, k <= xs.len
    }                                           // xs readable: lookup
    return total;
}
```

### 2.7 What closes an obligation: never search

"Automatic" in this document never means brute force, never `vm_compute` to find an answer, never a
solver searching. It means the checker applies a Coq-style tactic **by default** because the goal's
shape already says which one fits. Where no default fits, the human writes the tactic, as in Coq.

| obligation | what closes it | why it is not search |
|---|---|---|
| **ownership**: "you own `m` here" | **lookup**: is `own(m)` among the current facts? (`alloc` adds it, `free` removes it) | one rule per operation; the goal's shape picks it (how RefinedC's Lithium works) |
| **linear arithmetic**: `i < len`, `n == 5` | **`lia`** over the facts in scope (guards, `requires`, invariants) | a decision procedure: a fixed algorithm that decides its fragment and emits a certificate the kernel checks; not brute force over values |
| **everything else**: loop invariants, facts about a model, induction, non-linear arithmetic | **the human's `invariant` / `by (lemma)`** | supplied by the human; the checker only verifies it |

The third row cannot be automated in general: finding the right invariant or lemma is the genuinely
hard, undecidable part. The checker never guesses: it applies the one rule the goal's shape calls for,
runs a decision procedure guaranteed to finish, or asks for the tactic.

**How an unclosed obligation shows up:** as an **open goal at that line**, with the facts in scope,
the way Lean's infoview shows "unsolved goals" or Coq's editor shows the proof state. The defaults
close what they can silently; what remains is listed; the human adds an `invariant` or a `by` and the
goal disappears. Nothing is hidden and nothing is guessed: an open goal is simply a statement not yet
proved.

```
sum_prefix, line 6:  open goal
  facts:  k <= xs.len,  i < k,  total == sum(xs[0 .. i])
  goal:   total + xs[i] == sum(xs[0 .. i + 1])
  hint:   a lemma about sum over an extended range (suggester found: sum_extend)
```

---

## §3 Proofs check, code runs

This was the easiest place to go wrong (it was gone wrong on, Q10-Q13), so it is written down:

- **"Is this algorithm correct?"** A proof answers it for every input, without running anything. This
  replaces running tests for minutes and learning only about the inputs tried. This is real, and it is
  the power the owner was pointing at.
- **"What is the result for this input?"** Only running the code answers it. A proof that `sort` is
  correct does not sort your array. At compile time the run-time inputs do not exist yet (a UART byte
  arrives while the device runs), so a tactic cannot produce the program's output.
- **"Can I run something cheaper?"** A proof that the slow code equals a cheaper one (a loop equals a
  closed form, `loop_sum(n) = n(n+1)/2`) gives permission to run the cheaper one. That is legitimate
  when a human proves it; it is impossible as automatic recognition (program equivalence is
  undecidable).
- **Where tactics save time:** where finding is expensive and checking a supplied answer is cheap:

  | problem | find (search / brute force) | check (given the answer) |
  |---|---|---|
  | is `N` composite? | search for a factor | given `p`, `q`: one multiplication |
  | linear arithmetic (`lia`) | search for a combination of the facts | given the coefficients: add them |
  | unsatisfiability (SAT) | explore the search space | replay a proof log (DRAT) |
  | `∀ n ≤ 10⁶, P(n)` | compute `P` a million times | an induction proof: check a few steps |
  | a cost bound | spike and measure on sample inputs | a proof for all `n`: check it once |

- **Where they do not:** plain evaluation of a function on fixed inputs has no shortcut; checking
  `f(x) = y` costs about as much as computing `f(x)`. In Coq, `reflexivity` on `2 + 3 = 5` makes the
  kernel **compute** `2 + 3`; `vm_compute` does the same work on a compiled VM and is usually faster.
  A proof kernel also computes far slower than native code (KernelQ measured Coq at roughly 2M
  ops/sec; building a 200k-element list timed out at 2 minutes). Compute fixed inputs at compile time
  with `comptime`, or at run time as ordinary code.

---

## §4 Architecture: reuse the proved stack

```
source (C-shaped, contracts, models, by-steps)
   │  elaborator (untrusted, or proved later)
   ▼
C  +  RefinedC annotations  +  Coq lemma files (the by-targets)
   │
   ▼
RefinedC / Iris / Coq kernel   ── the small trusted kernel; refuses or accepts
   │  accepted
   ▼
erase ghosts ──► plain C (CompCert subset) ──► CompCert (proved)  or  GCC (trusted)
```

- **The small kernel is Coq's.** RefinedC is foundational: every accepted program comes with a Coq
  proof, and Iris's adequacy theorem reduces it to a closed Coq statement. No SMT anywhere.
- **The surface maps almost one to one onto RefinedC:** `requires` / `ensures` / `invariant` become
  RefinedC function and loop annotations (`rc::requires`, `rc::ensures`, loop invariants); a type's
  `model` becomes a RefinedC refined type (a C type refined by an abstract value); `by (lemma)`
  becomes a reference to a Coq lemma. This removes the RefinedC pain the owner named: specs and proof
  hints live **in the source**, and the elaborator generates the annotations and wiring nobody should
  write by hand (positional annotations, syntax keying, citing a separate Coq development).
- **The elaborator is untrusted at first.** A bug in it produces annotated C that fails to verify
  (a refusal), or annotations that say something other than the source meant; the second case is why
  it is in the trust ledger (§8) until it is proved or kept trivially syntactic.
- **Automation:** RefinedC's Lithium is syntax-directed and designed to avoid backtracking, which is
  close to "follow, do not search". Where it cannot close a goal alone, the source's `by` supplies the
  step.
- **Where the checking speed comes from:** the checker follows named steps (linear in program plus
  proof hints), there is no SMT, and lemmas are checked once and cached. Where a class of obligation is
  decidable and common, a **proved checker run as a boolean** (proof by reflection, as KernelQ's
  `check_membership` + `graded_run_bounded`) is the fastest form: its theorem is checked once, and each
  program only runs the boolean. Tactics replace search; reflection replaces a proof per program; the
  two combine.
- **Implementation language (open, recommendation):** the elaborator is untrusted and the kernel is
  Coq's, so the implementation language is free. OCaml fits the ecosystem (Coq, RefinedC's front end
  and Cerberus are OCaml; Coq extracts to OCaml). Trap if anything is extracted: never extract Coq's
  unary `nat`; use `Z` mapped to native or arbitrary-precision integers. Extraction itself is trusted
  (MetaCoq's verified erasure narrows it).

A dedicated kernel of the language's own is possible later. Coq's is the one to trust now.

### 4.1 The checker architecture, in general terms (the "LCF / de Bruijn" principle)

```
human proof steps ──► analyzer ──► certificate ──► tiny kernel ──► ACCEPT
   (hints)           (big, fast,    (proof object)   (small, trusted,
                      untrusted)                      the only thing trusted)
```

An analyzer bug can only produce a bad certificate, which the kernel rejects: a false reject, never a
false accept. Three placements were compared:

| option | speed | if the analyzer is buggy | examples |
|---|---|---|---|
| A. analyzer trusted | fastest | unsound accepts | Rust's borrow checker |
| B. analyzer emits a certificate, kernel checks it | fast + certificate size | false rejects only | proof-carrying code (Necula 1997), Coq/Lean tactics |
| C. analyzer proved correct once, run as a boolean | fastest and sound | impossible (a theorem) | KernelQ's `check_membership`; CompCert's validators |

This language uses B (RefinedC produces a Coq proof per program) and C where a decidable, common class
justifies a proved checker. Never A.

---

## §5 Tactics choose proofs, never meanings

Rust's lifetimes are contracts. Say four signatures A, B, C, D all fit a body and you mean D: A is
rejected, B is rejected, C compiles, you stop. C is consistent and safe but not what you meant, and
nothing tells you; you find out later when a caller D would have allowed is rejected, far from the
mistake. The annotation records what the checker tolerated, not what you intended.

A `by` step is a proof, and proofs are irrelevant to meaning (**proof irrelevance**): if two proof
steps both pass, the program, its behaviour and its interface are identical. There is no
consistent-but-wrong member to stop at. Three rules keep it that way:

1. **`by` never appears in a signature.**
2. **`by` chooses among proofs of one fixed meaning, never among meanings.** If the code allows two
   meanings, the refusal prints both, and the human narrows with code (a guard, a branch, an `orelse`).
3. **Contracts and models are the one place a claim is made.** A too-weak `ensures` still passes: that
   is the A/B/C/D risk, and it lives only there, visibly, where the human chose it. A contract can only
   make the compiler refuse more, never accept something unsafe. (The same holds for any *demand*,
   e.g. a stack budget.)

---

## §6.0 What a build demands is chosen

Every operation generates its obligation (a dereference needs "alive and owned", an index needs
"`i < len`", `free` needs "owned, and afterwards not"). Memory safety is not a separate class: it
is the precondition of each primitive, unfolded at the line where the operation happens. Each
obligation gets one of three answers:

| answer | meaning | at run time |
|---|---|---|
| `by (…)` or automatic | **proved** | nothing emitted |
| `check` | **checked** | a runtime check is emitted |
| `assume` | **trusted**, listed | nothing emitted; if false, behaviour is C's |

```
x = table[i];                  // automatic: proved from the guard above
y = buf[n];        check       // runtime bounds check
z = *raw_ptr;      assume      // listed in the report
```

Defaults can be set per function, module or property:

```
module fast_path  default assume        // prototype, or trusted hot code
module parser     default check         // untrusted input, keep runtime checks
module scheduler  prove races           // prove data-race freedom here
```

This is Coq's `Admitted` / Lean's `sorry` made into a first-class, reported choice. Every build prints
the actual guarantee:

```
proved:   212 obligations
checked:  14  (runtime checks emitted: parser)
assumed:  3   fast_path:41 (*raw_ptr alive), fast_path:57, dma:12
guarantee: memory-safe and race-free IF the 3 assumptions hold
```

**Build profiles:** a **strict** (release, safety-critical) build rejects any `assume` and any
`check`, which is the "no runtime" design of the rest of this section; a **development** build
defaults to `check`; a trusted fast path may `assume`, visibly.

**The catch: memory unsafety is not local.** A wrong assumed dereference can overwrite memory that a
proved function relies on (a lock, a length field). A proved function's guarantee therefore always
reads "proved, assuming the listed assumptions hold". Proving one property (races) while assuming
another (bounds) gives a conditional result, because an overflow can corrupt the lock the race proof
relies on. The report states the dependency.

A user can never make an accepted operation silently unsafe: "unsafe" exists only as a listed
`assume`, or as `cinclude` C (§9), or as a PoC whose bug is itself proved (§6A).

## §6 No runtime (the strict profile)

In the strict profile the emitted binary carries no GC, no reference counts, no allocator it did not
ask for, no runtime checks and no traps.

1. **Ghosts are erased.** Models, invariants, contracts and `by` steps are gone after checking.
   Erasing them cannot change behaviour, because none of them affects meaning (§5).
2. **Proof or refusal, never a check.** An operation whose safety is not proved is refused.
3. **Memory:** registers, statics, stack frames, and containers the program declares (`Pool`, `Slab`,
   `Ring`, `Arena`) as ordinary library code with their own contracts. Linear types give in-place
   update without a runtime. (Automatic input-sized regions, as in the Ω doc, would need an allocator
   in the runtime, so they are not part of this design.)
4. **Stack is bounded:** frame sizes are known; recursion needs a proved depth bound, otherwise it is
   refused. On bare metal there is often no guard page, so an overflow silently overwrites memory:
   this is a safety property, not a cost model.
5. **Arithmetic cannot trap:** wrapping is defined on bounded types; a divisor or shift amount must be
   proved in range.
6. **Values from outside cannot be proved, so they must be validated in code:**

   ```
   u32 raw = uart.dr;                         // type Raw<u32>: any value the width allows
   u8 idx = validate(raw, 0..16) orelse {     // the only way to get a usable value
       count_error(); return;                 // a handled path you wrote, never a crash
   };
   table[idx] = 1;                            // proved: idx < 16
   ```

   The check exists only at the boundary, once per value that enters, not on every use. This is the
   program handling a world it cannot prove things about, not a runtime protecting itself.
7. **MMIO:** interfaces come from per-target register descriptions (address, width, read-only /
   write-only / read-write, reserved bits, volatility): ARM SVD files, device trees, the vendors' ISA
   specifications. The checker enforces the interface (address in a declared region, width, no write
   to a read-only register, reserved bits preserved, volatile order kept). A read returns `Raw`.
   Universality comes from data tables per target, not new proofs per chip. Device behaviour ("reading
   this register clears a flag") is a named floor.
8. **Threads** are library calls (`spawn` / `join` lower to the OS or the program's scheduler) under a
   restricted concurrency profile (static tasks, protected objects, message passing, as SPARK's
   Ravenscar); race freedom and lock order are proof obligations.

What remains in every freestanding C program: start-up code (`crt0`, the linker script) and the OS or
hardware underneath. Those are floors (§8).

**Prototype, then prove:** a development mode may emit the proved conditions as runtime assertions
(fast iteration; they should never fire, and if one does, the hardware model or the compiler has a
bug). Modules are proved up to Silver one at a time; the release build is the refusing one, with no
checks. This is SPARK's workflow (Ada with checks → SPARK proved → checks suppressed).

---

## §6A Proving that a bug exists (incorrectness, `Qed` not `Admitted`)

The language must be able to prove things about unsafe programs, not only reject them: a PoC that a
use-after-free or a data race is really reachable, proved on purpose.

| | proves | logic | question |
|---|---|---|---|
| **correctness** | the bug cannot happen, on every run | separation logic (RefinedC, VST) | is it always safe? |
| **incorrectness** | the bug does happen, on some real run | **incorrectness logic** (O'Hearn, 2019); **incorrectness separation logic** (Raad et al., 2020) for memory bugs such as use-after-free | is this bug real? |

Incorrectness logic is not only theory: Meta's Pulse analyzer (in Infer) is built on it so that every
reported bug is provably real, not a false alarm.

```
void handle_close(Conn *c) { free(c.buf); }
void handle_read(Conn *c, u8 *out) { *out = c.buf[0]; }      // use after free if closed first

bug uaf_after_close
    reaches  use_after_free(c.buf)
    witness  { conn = new_conn(); calls = [handle_close, handle_read] }
{ by (replay) }

bug counter_race
    reaches  race(counter.value)
    witness  { schedule = [T1.read, T2.read, T1.write, T2.write] }
{ by (replay) }
```

The **witness** (an input, a call order, a thread interleaving) is the find-vs-check asymmetry
pointed at bugs: finding it is hard (fuzzing, a human, an AI); checking it is cheap (the kernel
replays the semantics until the bad state).

How far down a PoC proof can go:

- **Level 1, "the bug is reached":** fully doable at source level. The C semantics defines exactly
  when a use-after-free or a race happens.
- **Level 2, "and this is what the attacker gets":** C's semantics says nothing after undefined
  behaviour. Two ways:
  - **Elegant:** keep the memory in the language's own abstraction. With `Pool` / `Handle`, a
    use-after-free is a stale handle reading a reused slot, which is **defined** behaviour, so the
    whole attack is provable inside the language ("the stale handle reads the new owner's password
    field"). This is the right way to model most vulnerability PoCs.
  - **Heavy:** for real C UB (raw `free`, heap reuse), model the actual allocator and the compiled
    code against the ISA specification (the Islaris approach). Research-level per exploit.
- CompCert's theorem does not cover programs with UB, so a PoC proof speaks about the source (level
  1) or the language's own defined abstraction (level 2, elegant), never about what one compiler does
  with UB.

### 6A.1 Worked example: the simplest use-after-free, four ways

**(a) In a strict build it is refused, with the reason:**

```
struct Msg { u32 len; u8 data[64]; }

u32 read_len() {
    *Msg m = alloc(Msg);        // gives own(m)
    m.len = 5;
    free(m);                    // takes own(m) away
    return m.len;               // needs own(m): nothing gives it
}
```

```
error: read_len, line 5: 'm.len' needs "m is alive and owned".
  m was freed at line 4; after that line you no longer own m.
  options:  move the read before the free,
            or mark it 'check' / 'assume' (listed in the report),
            or prove it IS a bug with a 'bug' theorem.
```

**(b) Proved to be a bug (level 1):**

```
module poc_uaf  profile poc

u32 read_len() { *Msg m = alloc(Msg); m.len = 5; free(m); return m.len; }

bug read_after_free
    reaches  use_after_free(m)  at read_len:5
    witness  { call = read_len() }
{ by (replay) }
```

`by (replay)` runs the semantics along the witness (alloc, write, free, read) to the state "reading
freed memory"; the kernel checks the trace. The proof stops there: C says nothing about what the read
returns.

**(c) Proved consequence with the language's own `Pool` (level 2, defined behaviour):**

```
module poc_leak  profile poc

struct Account { u32 owner; u32 balance; u8 password[16]; }
static Pool(Account, 4) accounts;

void attack() {
    Handle(Account) mine = accounts.alloc();
    accounts.free(mine);                                 // slot returned, handle kept
    Handle(Account) victim = accounts.alloc();           // victim gets the same slot
    accounts.get(victim).password = "hunter2";
    u8 leaked[16] = accounts.get_stale(mine).password;   // stale handle, reused slot
}

bug stale_handle_leaks_password
    reaches  leaked == accounts.get(victim).password
    witness  { call = attack() }
{ by (replay; pool_reuses_last_freed) }                  // library lemma about Pool's reuse order
```

**(d) The fix:** read before freeing (`u32 n = m.len; free(m); return n;`). The strict build accepts it
with no proof text, and `read_after_free` no longer proves because the bad state is unreachable.

How it fits: `bug` theorems live in PoC or test modules and are never part of a strict build. They
double as regression records: after the fix, the correctness proof goes through and the `bug`
theorem **stops being provable**, a clean before/after record of the vulnerability.

---

## §7 Suggestions may search; the verdict may not

When an obligation is not closed, the diagnostic may do anything that helps: try lemmas, run an SMT
solver, list the meanings the code still allows. When it finds a proof, it **writes the `by` step into
the source** (as Isabelle's Sledgehammer prints a `by (metis …)` line to paste), and from then on the
checker only replays it. SMT's convenience without SMT's instability in the verdict: a slow or wrong
suggestion can only fail to help, never make anything accepted.

---

## §8 The trust ledger

Printed on every build:

- **Coq's kernel**, plus the axioms `Print Assumptions` reports for the proofs used.
- **RefinedC's front end** (~6,000 lines of OCaml, plus Cerberus) and **the Caesium semantics**
  (~1,500 lines of Coq), per the RefinedC paper. (Iris is not trusted: its adequacy theorem reduces to
  plain Coq.)
- **This language's elaborator** (surface → annotated C), until it is proved or kept trivially
  syntactic.
- **The compiler:** CompCert is proved (C → assembly), under the assumption "Caesium and CompCert C
  read this C the same way", kept small by emitting only the conservative fragment (Part II §II.4).
  With GCC instead: GCC itself is trusted.
- **Assembler, linker, the ISA model, silicon** (errata): floors, as in every verified system.
- **Hardware facts** at MMIO boundaries: datasheet behaviour.
- **Start-up code** (`crt0`, linker script).
- **Unproved C** brought in by `cinclude`: each module listed by name (§9).

---

## §9 The escape hatch is a proof

Nobody needs to write wrong code on purpose ("unsafe" in the sense of UB is never a feature). What
systems code needs is **correct code the automation cannot see**: a lock-free queue, a custom
allocator, a DMA buffer shared with hardware, zero-copy parsing, type punning a packet header. Those are
written in C, brought in with `cinclude`, and **proved with RefinedC against the spec the language
sees**. Only `cinclude` C without a proof is unproved, and it is named in the ledger. Rust's `unsafe`
is always trusted; here the escape hatch is proved in the same kernel.

The number of such modules in real programs is the measure of the language's expressiveness ("100%
usability in terms of correctness"): each one should become either a new construct in the language or
a proved module. It will not reach zero (hardware and FFI always exist), but every entry is closed or
proved.

---

## §10 Where it sits among existing languages

| | no SMT in verdict | small kernel | no GC | C-level control | Silver without writing contracts |
|---|---|---|---|---|---|
| SPARK | ✗ (Why3 → SMT) | ✗ | ✓ | ✓ | partly (range contracts are light) |
| F\* / Low\* | ✗ (Z3) | ✗ | ✓ (Low\*) | ✓ | ✗ |
| ATS | partly (built-in integer solver, optional Z3) | ✗ (large unverified type checker) | ✓ | ✓ | ✗ |
| Bedrock2 (Coq, MIT) | ✓ | ✓ | ✓ | ✓ (minimal language) | ✗ (research) |
| Verus / Dafny | ✗ | ✗ | Verus ✓ | Verus ✓ | ✗ |
| Lean 4 | ✓ | ✓ | ✗ (reference counting runtime) | ✗ | n/a |
| **this language** | ✓ | ✓ (Coq) | ✓ | ✓ | ✓ (from types) |

Lessons borrowed:

- **SPARK:** the level ladder; Silver for all code, higher levels opt-in; no mutable aliasing by
  default (keeps most proofs free of separation-logic reasoning); restricted concurrency profiles;
  prototype with checks, then prove and remove them.
- **Coq / Lean over F\*:** stability. A proof is a checked term; no solver version drift, no timeout in
  the verdict (Q23).
- **Bedrock2:** how far a Coq-only, no-GC, low-level route can go (a verified compiler to RISC-V, a
  verified IoT device end to end; related to Fiat Cryptography, whose generated C runs in Chrome /
  BoringSSL). A reference to read; about 3,900 commits of work. Its compiler targets RISC-V only.
- **KernelQ's cost-shape checker:** state the fragment the automation handles as its own grammar
  (written without reference to the checker), and measure over-rejection outside it with pinned,
  classified witnesses (NECESSARY / OVER / CLOSED …). Soundness here comes from RefinedC's foundational
  proof; what must be measured is the automation's completeness. Rule 2′: never add a check without a
  theorem that needs it.

### 10.1 Against SPARK in detail

| | SPARK | this language |
|---|---|---|
| who decides the verdict | SMT solvers (CVC5, Z3, Alt-Ergo) through Why3: search | the Coq kernel checking named steps |
| trust base | GNATprove / Why3's condition generator + the solvers, none proved | Coq's small kernel; RefinedC's soundness is proved in Coq/Iris |
| stability | proofs can time out or break across solver versions | a proof is a checked term |
| spec language | Ada expressions (executable, mostly first-order); ghost functional containers | Coq propositions: mathematical models, unbounded quantifiers, higher-order specs |
| pointers | ownership, no mutable aliasing, no pointer arithmetic | separation logic: pointer arithmetic, shared structures, lock-free code (with proofs) |
| hard cases | export to Coq/Isabelle via Why3 (a second world) | already Coq, same kernel |
| compiler | GNAT (trusted) | CompCert (proved) or GCC |
| arithmetic automation | SMT, strong | `lia` / `ring` (proved, linear); non-linear needs a `by` |
| maturity | decades, DO-178C certification credit, industrial users (avionics, rail, NVIDIA firmware) | nothing built |

**How SPARK handles memory without (much) pointer use**, which explains the table's pointer row:

1. **Data by value with parameter modes** (`in`, `out`, `in out`): the compiler chooses copy or
   reference; SPARK forbids overlapping actual parameters, so no aliasing through calls.
2. **Sizes fixed at creation:** unconstrained arrays (`String`), discriminated records.
3. **Linked structures become arrays plus indices:** `Next : Driver_Id` into a static pool instead of
   `node->next`. SPARK's formal containers (vectors, lists, maps) are bounded and array-backed with
   cursors.
4. **Real pointers since about 2019, under Rust-style ownership:** moves on assignment, borrows and
   observes, later leak freedom; no general aliasing, no pointer arithmetic. Many embedded projects
   use `pragma Restrictions (No_Allocators)` and never allocate.
5. **MMIO by address clauses:** `UART_DR : Unsigned_32 with Volatile, Address => ...`, a variable at a
   fixed address, no pointer.

SPARK's restriction is a deliberate trade (simple proofs, bounded pools that never fragment or
dangle), well suited to control software. It becomes a workaround only for code that is pointer-shaped
by nature: kernels, drivers, allocators, DMA rings, lock-free structures. There, this language writes
the pointers and proves them; SPARK reshapes into indices or leaves the proved subset.

**Code comparison, a keyboard-driver registry.** For an array of handler function pointers the two
are about the same size (SPARK must rule out null handlers by a precondition and write "old entries
unchanged" as a quantifier; here function pointers are non-null by type and the model states
`old ++ [h]`). For a Linux-style intrusive list (each driver carries its own `next`), this language
writes it directly, proved with list-segment lemmas:

```
struct KbdDriver { KeyHandler handler; ?*KbdDriver next; }
static ?*KbdDriver head;
    model drivers : List<*KbdDriver> = list_of(head)

void register(*KbdDriver d)
    requires d not_in drivers
    ensures  drivers == [d] ++ old(drivers)
{
    d.next = head;                                         by (list_cons)
    head = d;
}
```

while SPARK must use the index-pool form, move ownership into the list, or go outside SPARK. Summary:
**SPARK's style, extended to the pointer-shaped code kernels and drivers are made of, with a verdict
that has a much smaller trust base.** Simpler to trust, not simpler to write.

### 10.2 If it succeeds

- **Where it would really be used:** where assurance is already paid for: firmware, bootloaders,
  hypervisors (the pKVM kind of code), cryptography (where HACL\* and Fiat Cryptography already ship),
  avionics, automotive, medical, rail, space. The value: proved low-level code that is free, C-shaped
  and kernel-checked, instead of costly or trusted.
- **Where it would not:** web backends, apps, most business software. Rust already gives memory
  safety cheaply there, and even in the target world people will prove the critical few percent at
  Gold and leave the rest at Silver.
- **The AI point:** the rule "search in suggestions, never in the verdict" is the right shape for
  AI-written code. An AI proposes code, contracts and `by` steps (untrusted, as clever as it likes);
  the Coq kernel checks every step (small, never fooled); the output is proved C with no runtime. As
  more code is written by machines, "can I trust it?" gets harder by review and easy by kernel check.
  Systems such as AlphaProof already combine AI search with kernel checking in mathematics. Languages
  whose verdict depends on a large checker or an SMT solver cannot offer this as cleanly.
- **Honest odds:** most verification languages stay niche (ATS, F\*, Dafny). Success realistically
  looks like SPARK or seL4: a smaller user base in production where failure is expensive. The AI path
  is the one by which it could become broadly relevant.
- **Trust vs maturity:** VST + CompCert can close the trust gap; maturity (users, documentation,
  certification credit, training) comes only with time and adoption.

---

## §11 What this does not claim

- Not zero annotations: contracts and models are written where more than Silver is wanted, and `by`
  steps where the automation cannot close a goal. It is annotation-free for **safety**: no lifetimes,
  no ownership signatures beyond `lin` types.
- Not zero over-rejection: code the automation cannot prove is refused until narrowed or given a
  `by`; the refusal rate is measured, not denied. (100% precision for all programs is impossible by
  Rice; completeness inside a stated fragment is possible and is the target.)
- Not one end-to-end theorem: the "same reading" assumption between Caesium and CompCert C remains,
  kept small by the emitted fragment (Part II).
- Not a runtime-free I/O story: effects happen at run time; outside values are validated in code.
- Not "every program is safe": every accepted claim is proved; safety is proved when the build
  profile demands it, and bugs can be proved to exist (§6.0, §6A).
- Not proofs about what happens after real C undefined behaviour, except through a machine model or
  the language's own defined memory abstractions (§6A).
- No speed numbers: the emitted C runs at C speed; removing runtime checks gains little (ZER measured
  its bounds checks at about 0-4%). The gain is guarantees and predictability, not speed.

---

## §12 Build order

1. **Elaborator for a tiny subset:** integers, fixed arrays, `if`, `for`, functions with `requires` /
   `ensures` / `invariant` / `by`, emitting RefinedC-annotated C. First target: the dictionary of §2,
   `get` only.
2. **Linear types** (`lin`), moves, in-place update; `put`.
3. **Type models and invariants** mapped to RefinedC refined types.
4. **The emitter in the CompCert subset** (Part II §II.6), `gcc -std=c99 -pedantic-errors` in CI, then
   `ccomp` builds and a differential check (same outputs under both compilers).
5. **`Raw` values and MMIO descriptions** for one target.
6. **The suggester** (§7): lemma search and optional SMT, writing `by` steps into the source.
7. **The audit** of the automation's over-rejection, KernelQ-style.

---

# PART II — THE VERIFICATION CHAIN: COMPCERT, REFINEDC, VST

(Also recorded for ZER in `docs/zer-unified-compiler.md` §1-§4.)

## §II.1 The level map

| tool | proves | level |
|---|---|---|
| **RefinedC** | this C program meets its spec (no UB, memory safety, functional claims) | 1: program correctness |
| **VST** | this C program meets its spec | 1: program correctness |
| **CompCert** (`ccomp`) | the assembly behaves like the C program | 2: compiler correctness |

The full trust chain, level by level:

| level | question | who proves it, or where it is trusted |
|---|---|---|
| 1. spec ↔ source | does the program do what the spec says? | RefinedC / VST / SPARK proofs |
| 2. source → assembly | does the compiler preserve meaning? | CompCert proves it; GCC / LLVM / GNAT are trusted |
| 3. assembly model ↔ real ISA | is the compiler's ISA model the real ISA? | trusted, or checked against vendor formal specs (Arm ASL, RISC-V Sail, x86 Sail; see Islaris) |
| 4. assembler + linker | are the bytes right? | usually trusted |
| 5. ISA ↔ silicon | does the chip follow its spec? | a floor (errata); the vendor's job |

- **Each tool is gap-free for its own claim.** RefinedC's "the program is correct" does not depend on
  any compiler. CompCert's "the binary does what the C says" does not depend on any program proof.
- **VST and RefinedC do the same job** (KernelQ's LS.31 says the same: "VST ≈ RefinedC … Coq libraries
  with the SAME role"). The only difference in role is the C semantics each proves against: VST uses
  **Clight**, CompCert's own; RefinedC uses **Caesium**, its own.
- **The merged sentence** "this binary is correct", stated as ONE Coq theorem, needs both proofs to be
  about the same semantics. VST + CompCert get that for free (both Clight). RefinedC + CompCert need one
  link: "Caesium and CompCert C read this C file the same way". That link only matters if the single
  merged theorem is wanted (e.g. a paper claiming end to end with no assumption). For "a proved program
  compiled by a proved compiler", RefinedC + CompCert is complete.
- **CompCert's theorem has a precondition:** it applies only if the source has no undefined behaviour.
  A level-1 proof is what discharges it; CompCert alone does not make a program safe.
- **"Gold" in SPARK is not about the ISA.** Every program-proof level is about the source against its
  spec; how far down trust goes is the separate axis in the table above. Every verified system stops
  somewhere (seL4, CompCert, SPARK); what matters is that the line is printed, not hidden.
- **One IR, many backends:** CompCert shares its intermediate languages and has one backend per ISA
  (x86, ARM, AArch64, PowerPC, RISC-V), **each proved against that ISA's model**. A shared IR shrinks
  the per-ISA part; it does not remove the per-ISA trust.

## §II.2 RefinedC vs VST

### Trust base

| | RefinedC | VST |
|---|---|---|
| trusted front end | ~6,000 lines of OCaml translating C (via Cerberus's AIL) into Caesium, plus Cerberus | clightgen (CompCert's own C parser) |
| trusted semantics | Caesium, ~1,500 lines of Coq | Clight (CompCert's) |
| program logic trusted? | no: Iris's adequacy theorem reduces it to a closed Coq statement | no: soundness proved against Clight |
| foundational (a Coq proof per program) | yes | yes |
| one theorem down to machine code | no: nothing compiles from Caesium | yes, with CompCert, demonstrated twice |
| escape hatch to watch | `rc::trust_me` is a silent accept (needs an allowlist; KernelQ built one) | none built in |

(RefinedC figures are the paper's own description of its TCB.)

### What each has achieved

- **RefinedC** (PLDI 2021): automated foundational verification of C with refined ownership types;
  handles pointer arithmetic and fine-grained concurrency (spinlocks over atomics). Real-world case
  study: a substantial component of **Google's pKVM** hypervisor (the early allocator). The same group's
  **Islaris** (PLDI 2022) verifies machine code against the official Arm/RISC-V ISA specifications, also
  on pKVM; **RefinedRust** (PLDI 2024) carries the approach to Rust.
- **VST**: **OpenSSL SHA-256 and HMAC** (USENIX Security 2015), described as the first machine-checked
  result combining a program proof, CompCert's compiler proof and a cryptographic security proof with
  no gaps at the interfaces; **mbedTLS HMAC-DRBG**, composed with CompCert end to end.

### Ease

| | RefinedC | VST |
|---|---|---|
| automation | high (Lithium, syntax-directed, avoids backtracking) | low (manual Floyd tactics: `forward`, `entailer!`) |
| pains | positional, syntax-keyed annotations; hard error messages; KernelQ's build pin (hosted gcc 7.5, so `[[rc::...]]` attributes must be stripped for normal compiles); a C subset | long, tedious, predictable |
| docs / community | research papers | Appel's *Verifiable C*, larger community |

Measured in KernelQ (LS.31-LS.33): both cost about 5-10x the code in proof on the hard cases. RefinedC
verified an open-addressing hash table in about 35 s of checking in KernelQ's experiments.

### Verdict

VST is **more trusted** (its chain composes with CompCert into one theorem); RefinedC is **easier**
(automation). Neither is unsafe; the difference is where the guarantee stops.

## §II.3 The decision: RefinedC proves the program, `ccomp` compiles it

Why RefinedC and not VST: VST's only extra is the single merged theorem; the practical result, a proved
program compiled by a proved compiler, is the same; RefinedC's automation saves the manual tactic work
VST requires; and this language's surface maps onto RefinedC's refined types (§4).

Why `ccomp` instead of GCC, stated as a strict improvement:

| | RefinedC + GCC | RefinedC + `ccomp` |
|---|---|---|
| program proved | yes | yes |
| "the compiler reads the C the way Caesium does" | assumed | assumed |
| compiler can miscompile | **yes** (real GCC miscompilation bugs; aggressive UB-based optimisation) | **no** (proved) |

The "same reading" assumption exists with **every** compiler; switching to `ccomp` does not add it. It
removes compiler bugs and keeps only the assumption that was already there. There is no new risk.

## §II.4 The one assumption, and how to keep it small

The two semantics can differ only on exotic code:

- integer ↔ pointer casts (CompCert uses abstract memory blocks; Caesium has byte-level memory with
  pointer provenance);
- reading the individual bytes of a pointer;
- comparing pointers into different objects;
- edge cases of what each semantics calls undefined behaviour;
- and two unverified parsers read the same source text.

On ordinary C (structs, arrays, loops, normal pointers) both read the same program. Keep the emitted C
out of those patterns (a syntactic scan is enough) and the assumption covers only well-understood
code. Ledger line:

```
assumed: Caesium and CompCert C agree on the emitted fragment
         (the fragment excludes int<->ptr casts, pointer byte access, cross-object pointer comparison)
```

### Routes to close it (none required now)

| route | what you build | size |
|---|---|---|
| 1. accept the gap | nothing; the ledger line above | zero (chosen) |
| 2. fragment simulation | a proof that Caesium and CompCert C agree **on the restricted fragment** | a research project, months; publishable: RefinedC's automation with CompCert's end-to-end guarantee |
| 3. full simulation | Caesium ↔ CompCert C for all of C, reconciling two memory models | PhD scale; do not start |
| 4. check machine code directly | verify each binary against the vendor ISA spec (Islaris) | heavy per program |

Route 2 uses the cost-shape method (agree on a stated fragment, refuse or trust outside it). It is a
separate research project, to be done only after the cost-shape checker is finished and published.

## §II.5 CompCert's licence

Read from CompCert's `LICENSE` and the user manual §2.1 (2026-09-28; the manual's own text is
CC BY-NC-SA 4.0, which covers the document, not the compiler):

- **Non-commercial only:** most of the compiler. Allowed: "educational, research, or evaluation
  purposes only"; "Noncommercial use relates only to educational, research, personal or evaluation
  purposes. Any other use is commercial use."; not allowed: use "in connection with any activities
  which purpose is to procure a commercial gain" without a licence from AbsInt. Manual §2.1: "The
  public release above can be used freely for evaluation, research and educational purposes, but
  commercial uses require purchasing a license from AbsInt."
- **Dual-licensed LGPL 2.1+ (commercial use allowed):** `lib/`, `common/`, `cparser/`, `export/`
  (clightgen), and the C/Clight semantics (`Clight.v`, `ClightBigstep.v`, `Csem.v`, `Cstrategy.v`,
  `Csyntax.v`, `Ctypes.v`, `Ctyping.v`, `Cop.v`, …), the architecture description files, and the build
  files. `flocq/` and `MenhirLib/` are LGPL 3+. **`runtime/` is BSD 3-clause.**
- **Redistribution** of the non-commercial parts must keep the same terms.
- **Compiled output is not mentioned** in the licence, the manual or AbsInt's CompCert page. Silence,
  not explicit permission.

| case | licence needed? |
|---|---|
| building this language's compiler with `ccomp` for an open-source, non-commercial project, shipping only the compiler binary | no (research / personal use; `ccomp` is never shipped) |
| a company **running** a binary someone else built with `ccomp` | no: running a binary is not using CompCert; `runtime/` is BSD |
| a company compiling its own firmware through `ccomp` (even behind a wrapper) | **yes**: that company runs `ccomp` commercially |
| the project starts earning money (sales, paid support, funded development) | the `ccomp` build step becomes commercial: buy a licence or build releases with GCC |
| using VST, clightgen, the Clight semantics, `lib/` | free, commercial included (BSD / LGPL). This is why CertiCoq, VST, Jasmin and clightgen can use CompCert pieces freely |

Default output compiler stays GCC; `ccomp` is the certified-build mode. If any case turns commercial,
get AbsInt to confirm the output point in writing.

**A correction worth keeping:** it had been believed that VST / Clight need a paid licence. They do
not. Only running `ccomp` commercially is restricted.

## §II.6 CompCert's C subset, and emitting for both compilers

From the CompCert user manual (§6.4.1 keywords, §6.5-6.8):

- **No GCC statement expressions `({ … })`** (not listed as supported). ZER's emitter uses them at 193
  emission sites (`grep -c '"({' emitter.c`, 2026-09-28), which is why ZER cannot go through `ccomp`
  yet. This language must not use them from the start.
- **No `typeof`** (not a CompCert keyword). The emitter knows every type: write the name.
- **No variable-length arrays;** no complex types; `long double` only with `-flongdouble`.
- **`asm` only with `-finline-asm`.**
- **`switch` is restricted to the structured (MISRA) form** unless an option is set.
- Supported: `_Alignas`, `_Alignof`, `__attribute__` (some), `_Noreturn`, `_Generic`, `_Static_assert`,
  designated initializers; some `__builtin_*` (check each against the manual).

Emitting the common subset keeps GCC working:

1. **Keep single evaluation** where a statement expression would have been used; compute into a
   temporary first, and preserve evaluation order and `&&` / `||` short-circuiting:
   ```c
   // GCC only:
   x = a[({ size_t _i = f(); if (_i >= 16) _trap(); _i; })];
   // both:
   size_t _i = f(); if (_i >= 16) _trap(); x = a[_i];
   ```
   (In this language the trap disappears: the bound is proved, so only `size_t _i = f(); x = a[_i];`.)
2. **Missing builtins → plain C;** GCC usually recognises idioms such as popcount and byte swap and
   still emits the single instruction. Check the hot ones.
3. **No performance cost under GCC:** temporaries and small helpers compile to the same code at `-O2`.
4. **CI:** compile emitted C with `gcc -std=c99 -pedantic-errors` (catches extensions without `ccomp`
   installed); once `ccomp` is installed, build the test suite with both and compare outputs.

---

# PART III — SETTLED QUESTIONS (do not re-derive)

Each entry: the question as it came up, the answer, and the reason.

### About the original ZER-Ω vision

**Q1. Is the ZER-Ω document (`perfect-compiler-omegaonly-RP.md`) buildable as written?**
No. Impossible claims: (a) zero annotations + exact candidate sets + zero over-rejection for all
programs (exact safety of programs with unbounded loops and integers is undecidable); (b) the Atlas is a
finite catalogue for all programs (finitely many distinctions per program does not give one finite
catalogue; guard shapes over unbounded integers, especially non-linear, are unbounded); (c) "execution
by recognition" (recognising what arbitrary code computes is program equivalence, undecidable; only a
hand-written pattern list works, which is what the doc criticises GCC for); (d) a one-axiom trust base
(contradicted by its own ISA-spec trust and datasheet floors); (e) its benchmark table was never
measured. Its good ideas survive: no search at compile time, refusal that prints the candidate
meanings, narrowing by code, printing the trust base.

**Q2. Is "human gives the strategy, compiler only checks" possible?**
Yes; it is how every working verified system operates. Finding a proof is hard; checking a supplied
one is mechanical (Rust checks lifetimes it is given; Coq's kernel checks proof terms).

**Q3. Is a tactic "static following"?**
Only a tactic with no search (`exact`, `apply`, named rewrites). `lia`, `auto`, `omega` run decision
procedures or search. What is always deterministic is checking the resulting proof term. Checking is
still computation (conversion can reduce terms), so "no search" is achievable and "no computation" is
not; the cost is bounded by what the human wrote.

**Q4. Drawbacks of proof replay (human-supplied proofs)?**
The human pays the time the compiler saves (5-10x); proofs are brittle to refactoring; a proof of a
wrong spec passes (A/B/C/D, but only at contracts, §5); checking still computes; the annotation
language must be expressive enough or it needs an escape hatch; the model-to-binary link is the hard
part; error messages are about proof steps. Mitigations: decidable parts automated, hints only where
needed, results cached.

**Q5. Do hybrid systems exist (automatic easy parts, human hard parts, cached certificates)?**
Yes, in pieces: Rust (elision), F\*/Low\*, Dafny, Verus, SPARK, Frama-C WP, Liquid Haskell, RefinedC,
ATS for the first; Isabelle Sledgehammer, Coq micromega, SMTCoq, CompCert translation validation, and
`.vo`/`.olean` build caches for the second. No production compiler combines all of them.

**Q6. Should the safety analyzer be inside the kernel?**
No (§4.1). Keep it outside so its bugs can only cause false rejects. Option C (a proved checker run as
a boolean) is as fast as a trusted analyzer and sound; KernelQ's cost checker is one built instance.

**Q7. Is ZER-Ω the same as ZER?**
No. ZER's prover direction is Coq + RefinedC (KernelQ LS.30), closer to option B. This language is a
separate design.

### About annotations and intent

**Q8. Do human-written hints bring back Rust's A/B/C/D problem?**
Not if they are proof steps: proof irrelevance (§5). Earlier drafts had `own` / `ref` / `@region` on
parameters; those are interface annotations (Rust lifetimes under another name) and were removed.

**Q9. Is "100% annotation" compatible with no search?**
The rule is "the checker never chooses". Meaning choices are settled only by code; proof choices by a
`by`; determined facts (one possible answer, e.g. a handle's state right after `free`) may be computed,
because there is nothing to choose.

### About proving versus running

**Q10. Can tactics execute the program ("the tactic unfolds and gets the outcome")?**
No. At compile time the run-time inputs do not exist. A tactic can compute with known inputs
(`comptime`) or reason symbolically about all inputs; the program's output for real inputs comes only
from running it (§3).

**Q11. Is `reflexivity` a shortcut over `vm_compute` for `2 + 3 = 5`?**
No. `reflexivity` makes the kernel compute; `vm_compute` computes faster on a VM. Plain evaluation has
no shortcut (§3).

**Q12. Can plain C already use the witness shortcut?**
Yes, at run time: **certifying algorithms** (the LEDA library) return an answer plus a witness that a
simple checker verifies (factors of `N`, an odd cycle, a dual solution). What C cannot do is check a
property **for every input, at compile time**: that needs a logic. The value of this language is moving
the check from run time per input to compile time for all inputs, not the shortcut itself.

**Q13. Then is the whole idea "hallucinated"?**
No. The real part: a proof establishes correctness for all inputs without running anything, replacing
test runs. The conflated part: proving is not running; the correct algorithm still has to run. The
third, legitimate part: a human proof that slow code equals a cheaper one permits running the cheaper
one.

### About runtime, contracts and SPARK

**Q14. Runtime checks or none?**
None for values the program computes (proof or refusal); validation in code at boundaries for values
from outside (`Raw`); typed descriptions for hardware; a development mode with assertions (§6).

**Q15. Does "no runtime" require full SPARK-style contracts?**
No. AdaCore's levels: **Stone** (valid SPARK), **Bronze** (flow analysis, initialisation),
**Silver** (absence of runtime errors), **Gold** (key integrity properties), **Platinum** (full
functional correctness). Removing runtime checks needs only Silver, whose contracts are light; here
Silver comes from types (§2.2). (An earlier explanation called full correctness "Gold"; it is Platinum.)

**Q16. How does SPARK have no runtime?**
Ada inserts runtime checks by default; SPARK proves they cannot fail, then code is compiled with checks
suppressed (`-gnatp`). Bare-metal targets use minimal runtimes (the light, formerly zero-footprint,
runtime) and restricted tasking profiles (Ravenscar, Jorvik). Ada never had a GC; SPARK banned pointers
for years, and since about 2019 allows them under a Rust-inspired ownership model (moves, borrows,
observes), later also proving absence of leaks. SPARK forbids aliasing between mutable parameters,
which keeps most proofs simple. GNATstack computes worst-case stack. Proofs go through Why3 to SMT
solvers (CVC5, Z3, Alt-Ergo), i.e. search, with ghost code, lemmas and Coq/Isabelle for hard cases.
SPARK executes code, checked by contracts; this language executes code too; the difference is how the
proof is obtained (named steps, no SMT in the verdict).

**Q17. Does "proof-based" mean the program must be a functional language?**
No. Only the proof layer is functional (specs and lemmas, erased). A functional program language pulls
in a runtime: OCaml and Haskell use a GC; Lean 4 and Koka update in place only after checking a
**reference count at run time**. Zero-runtime in-place mutation needs uniqueness proved at compile time
(**linear / uniqueness types**, as Clean and ATS). So the language is C-shaped with `lin` types. Existing
systems that prove imperative code: SPARK (Ada), Frama-C and VST/RefinedC (C), Verus (Rust), Low\*.

**Q18. Should contracts be written everywhere (Lean/F\* style) or should safety be automatic?**
The balance SPARK uses: Silver automatic (here: from types), Gold/Platinum where a spec is written.
Contracts everywhere means the 5-10x cost everywhere.

**Q19. Contract vs proof: are they opposites?**
No. A contract is the spec; a proof is one way of checking it. Ada checks contracts at run time; SPARK
proves the same contracts statically; Lean proves propositions with tactics. SPARK is contracts and
proof; Ada alone is contracts at run time.

**Q20. Is a cost model needed?**
No operation-count cost model (that exists in KernelQ because rungs grade speed). Keep a stack-depth
bound, and heap bounds for pools, because on bare metal they are safety.

### About existing languages

**Q21. Why do trading firms use OCaml against C++ (Jane Street)?**
Not raw speed: development speed and correctness. Hot paths are written allocation-free (preallocated,
reused mutable buffers) so the GC has nothing to do; minor-heap allocation is a bump pointer; GC is
tuned so major collections are rare. OxCaml (their fork, open-sourced 2025) adds stack allocation via
modes (`local`), unboxed types, and a compile-time-checked `[@zero_alloc]` attribute, which is the same
idea as proving "no heap here" statically. OCaml's GC lives inside a compiler written in OCaml, never
in the programs that compiler emits.

**Q22. Is this just Ada, or just Lean?**
Neither. Ada is contracts checked at run time; SPARK adds proof via SMT; Lean has proof power but a
runtime and no bare-metal control. This language is SPARK's balance with Lean-style proof (small
kernel, checked terms), closest in spirit to F\*/Low\* but without SMT.

**Q23. Why is F\* not widely used, and why Coq?**
F\* is used in production but narrowly: HACL\* (Firefox NSS, Python `hashlib` since 3.12, Linux),
EverParse (Microsoft, e.g. Hyper-V), mostly from one group (Project Everest). It did not spread
because proofs depend on Z3 (timeouts, breakage across versions: "proof instability"), the community
is small, it is hard to learn (effects, Low\*'s memory model), and its sweet spot (crypto, parsers) is
narrow. Coq (renamed Rocq) is everywhere because it is old (1989) with a huge ecosystem (CompCert, VST,
Iris, RefinedC, MetaCoq, math-comp for the Four Colour and Feit-Thompson theorems), has a small kernel
with no solver in the trust base, proofs are stable checked terms, and universities teach it
(*Software Foundations*). Lean is the rising alternative (Mathlib), with the same design.

| | SMT automation (F\*, Dafny, SPARK) | tactics + small kernel (Coq, Lean) |
|---|---|---|
| getting started | easier | harder |
| long term | brittle (timeouts, solver drift) | stable |
| trust | kernel plus the solver | kernel only |

**Q24. Is ATS already this language?**
Right shape (dependent + linear types, C output, no GC, proofs as terms), but its proofs are checked by
a large unverified type checker (no small kernel, soundness not mechanised), integer constraints go to
a built-in solver (optionally Z3), it is very hard to use with notorious error messages, and it has a
tiny community (ATS3 in development for years).

**Q25. Is Bedrock2 already this language?**
Closest in the Coq world (low-level language in Coq, tactics, no SMT, no GC, a verified compiler to
RISC-V, a verified IoT device end to end), but a research tool with a minimal language and no
ergonomics for normal programmers, and RISC-V only. Porting its backend to all ISAs is possible but is
exactly the per-ISA proof work CompCert already did.

**Q26. Is the "tactics not search" design new?**
The pieces exist (proof-carrying code, RefinedC's no-backtracking automation, CompCert's proved
validators, reflection in Coq/Lean). The combination (a C-shaped bare-metal language, erased proof
layer, named proof steps, proved checkers, zero runtime checks, usable defaults) is uncommon. Do not
claim "nobody uses tactics".

### About the chain

**Q27. Is RefinedC + `ccomp` the same as VST?**
Same job, one difference (Part II §II.1-§II.4): VST shares CompCert's semantics, so one theorem; RefinedC
+ `ccomp` is two gap-free theorems joined by one assumption that already existed with GCC. Chosen:
RefinedC + `ccomp` (automation), with the conservative-fragment hygiene. VST is not needed unless a
single merged theorem is wanted.

### About safety being chosen, and proving bugs

**Q28. Is it really as simple to write as B?**
For program authors, mostly: the code is as easy as C, and automation closes bounds, ownership and
linear arithmetic. The Coq-style work moves into the lemmas `by` names, proved once per data
structure by a library author (§2.5).

**Q29. Doesn't avoiding SMT lose arithmetic automation?**
Mostly no: Coq's `lia`, `ring` and `field` are proved decision procedures (decide the goal or emit a
certificate the kernel checks), deterministic and complete for their fragment. Only non-linear
arithmetic is lost, where SMT is unreliable too. There is a reason Coq and Lean keep SMT out of the
kernel.

**Q30. Is this language much simpler than SPARK?**
No, for simple code it is about the same size (§10.1). It is simpler to trust (kernel vs SMT) and more
expressive for pointer-shaped code (separation logic vs no-aliasing), at the price of maturity.

**Q31. Should memory safety be forced on every program?**
No. The language forces proof, not safety. Forcing safety would make it unable to talk about
vulnerabilities; that is unfair and not the way to 100% usability. Obligations are proved, checked or
assumed, per the build profile, and always reported (§6.0).

**Q32. Can a user choose "unsafe" inside the language?**
Not silently. An accepted operation is either proved safe, runtime-checked, or a listed `assume`; or
the code is `cinclude` C (proved with RefinedC or listed). Memory unsafety is not local, so every
guarantee is stated relative to the listed assumptions.

**Q33. Can the language prove that a program IS vulnerable (a UAF or race PoC), with `Qed` rather than
`Admitted`?**
Yes, with incorrectness logic: a `bug` theorem states a bad state is reachable and gives a witness
(input, call order, schedule) that the kernel replays (§6A). Consequences after real C UB need a
machine model; with the language's own `Pool` / `Handle` the attack is defined behaviour and provable
end to end.

**Q34. Is plain-looking code (no `requires`, no `by`) still this language, or just C?**
It is this language: obligations are generated implicitly by each operation and closed by default
tactics; proof text is visible only for claims and for steps no default closes (§2.6).

**Q35. Does "automatic" mean the checker brute-forces or searches?**
No. It means a default Coq-style tactic chosen by the goal's shape: lookup for ownership, the `lia`
decision procedure for linear arithmetic. Invariants and lemmas cannot be found reliably by any
procedure, so they come from the human's `by`, exactly as in Coq (§2.7).

### About the project as a whole (owner's context)

- The owner concluded that C + RefinedC already delivers most of "possible ZER-Ω", and that a new
  language is only worth it for integration (code, spec and proof steps in one source, visible in one
  editor, with the annotations RefinedC needs generated rather than hand-written). That is this
  document's reason to exist.
- Separately recommended, not part of this language: finish and publish KernelQ's cost-shape checker
  (nearly done, a novel result: a proved checker for cost on the programmer's own charges, complete
  inside a stated fragment, deployed); and bring its method (soundness + completeness in a fragment +
  measured audit) to ZER (`docs/zer-unified-compiler.md` §5).

---

## Sources

- RefinedC paper (PLDI 2021): https://plv.mpi-sws.org/refinedc/paper.pdf
- RefinedC at PLDI 2021: https://pldi21.sigplan.org/details/pldi-2021-papers/11/RefinedC-Automating-the-Foundational-Verification-of-C-Code-with-Refined-Ownership-T
- Michael Sammler's publications (RefinedC, RefinedRust, thesis): https://pub.ista.ac.at/~msammler/
- Islaris (PLDI 2022): https://www.cl.cam.ac.uk/~pes20/2022-pldi-islaris.pdf
- Verified correctness and security of OpenSSL HMAC: https://www.cs.princeton.edu/~appel/papers/verified-hmac.pdf
- Verified Correctness and Security of mbedTLS HMAC-DRBG: https://arxiv.org/pdf/1708.08542
- CompCert LICENSE: https://github.com/AbsInt/CompCert/blob/master/LICENSE
- AbsInt CompCert page: https://www.absint.com/compcert/index.htm
- CompCert user manual (local copy read: `~/Downloads/manual.pdf`, §2.1 and §6)
