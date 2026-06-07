# Universal Pointer Design — Full Context Dump

**Status:** Brainstorming / design-space exploration. NOT a decision document.
**UPDATE 2026-06-07:** a DECISION has now been made for the pointer-lifetime/escape
axis — compile-time-only `keep`, no runtime tag check. See **PART 5** at the end.
PART 5 supersedes PART 3's auto-detection "lock" (§29.6). Parts 1–4 remain the
exploration record (append-only); the other safety axes are still open.
**Date started:** 2026-05-21
**Last updated:** 2026-05-25
**Mode:** Exploration of tradeoffs across multiple pointer-safety designs. The
goal is to map the design space, not to converge on a single answer.
**Purpose of this doc:** Comprehensive record of every design we've explored,
every tradeoff we've identified, every rejected alternative, and every honest
limitation. Future sessions should read this BEFORE proposing new pointer
designs — most options have already been mapped here.

> **READING NOTE:** This document is exploration-mode, not commit-mode.
> No design here is "the answer." Each design is a probe into a corner of the
> tradeoff space with honest costs marked. When ZER eventually commits to a
> direction, that commitment will be a separate document. This doc captures
> the FULL CONTEXT of the exploration so we don't re-litigate ground we've
> already covered.

---

## Table of Contents

1. [Problem Statement and Motivation](#1-problem-statement-and-motivation)
2. [Current State of ZER's Pointer Model](#2-current-state-of-zers-pointer-model)
3. [The Goal — Universal Pointer Safety](#3-the-goal--universal-pointer-safety)
4. [The Conservation Law (Impossible Triangle)](#4-the-conservation-law-impossible-triangle)
5. [The Audience Question (Who Pays What)](#5-the-audience-question-who-pays-what)
6. [Designs Explored](#6-designs-explored)
   - 6.1 safe_ptr with Shadow Tag (initial proposal)
   - 6.2 Always-Fat Handle (Option A)
   - 6.3 Two-Type (Handle thin + safe_ptr fat)
   - 6.4 PortableHandle (cosmetic rename of safe_ptr — rejected)
   - 6.5 User-Extensible Slab returning Handle
   - 6.6 Slab-Table Indirection
   - 6.7 Tagged Pointer + Allocation Header (8-byte hybrid)
   - 6.8 100% Runtime-Checked Semantically (optimization-driven)
   - 6.9 4-Rule Strict Borrow Checker (zero annotation)
   - 6.10 Single-Annotation `keep` Borrow Checker
   - 6.11 Mode-Based (per-target configuration)
7. [Rejected Alternatives in Detail](#7-rejected-alternatives-in-detail)
8. [Comparison to Other Languages](#8-comparison-to-other-languages)
9. [The Orthogonal Safety Axes Framing](#9-the-orthogonal-safety-axes-framing)
10. [Shareholder Analysis Across All Designs](#10-shareholder-analysis-across-all-designs)
11. [Cost Models in Detail](#11-cost-models-in-detail)
12. [Multi-Lifetime Patterns — Are They Real?](#12-multi-lifetime-patterns--are-they-real)
13. [Implementation Notes per Design](#13-implementation-notes-per-design)
14. [Open Questions](#14-open-questions)
15. [Glossary of Terms](#15-glossary-of-terms)
16. [Appendix A: Conversation Highlights](#appendix-a-conversation-highlights)
17. [Appendix B: Cost-Profile Microbenchmarks (Theoretical)](#appendix-b-cost-profile-microbenchmarks-theoretical)

---

## 1. Problem Statement and Motivation

### 1.1 Where ZER currently sits

ZER's existing memory safety story is built on:

- **4 memory generics**: `Pool(T, N)`, `Slab(T)`, `Ring(T, N)`, `Arena` — each
  with explicit allocation semantics, compile-time tracked, zero runtime cost.
- **Handle(T)**: 8-byte index+generation reference, runtime gen-checked via
  the slab's gen array. Provides temporal safety for Pool/Slab allocations.
- **`*T` raw pointer**: 8 bytes, ZER-CHECK path-sensitively tracks UAF /
  double-free / leak. Conservatively works for ~95% of code patterns.
- **`[*]T` slice**: 16-byte fat pointer with `.ptr` and `.len`. Bounds-checked
  on every index access, often elided by VRP when bounds provable.
- **`?T` optional**: forces `orelse` unwrap, eliminates null deref.
- **`shared` annotation**: auto-locks mutex around field access for cross-thread
  data. Compile-time data race detection on spawn.
- **`mmio` declarations**: explicit address ranges for `@inttoptr`. Bounds
  checked at compile time for constants, runtime for variables.
- **`keep` annotation (limited)**: marks pointer parameters that can be stored
  in globals/struct fields. Currently used in a narrow context (System #21).
- **Allocation coloring**: tracks which allocator each `*T` came from, prevents
  cross-allocator escape.
- **zercheck CFG-based analyzer**: path-sensitive state machine on Handle
  states (ALIVE → FREED → MAYBE_FREED).

This combination covers ~95% of real systems-code patterns at zero runtime cost.

### 1.2 The 5% gap (the problem we're trying to solve)

Some patterns aren't fully expressible in ZER's current safe code:

1. **User-written custom allocators** — current ZER offers Pool/Slab/Ring/Arena
   but doesn't allow user-implementations returning safe pointers. Users who
   need novel allocator patterns (LIFO stack, NUMA-aware, lock-free, refcounted,
   research allocators) must drop to raw `*T` with no safety guarantee, or use
   `cinclude` to call C code.

2. **Pointers that travel through arbitrary code** — `*T` stored in a generic
   container (`Map<K, *T>`), captured by async closures, passed through
   callback contexts where the receiver doesn't know the originating allocator.
   ZER's current Handle requires the slab to be in scope at every use site.

3. **FFI boundaries where lifetime is unknowable** — pointers received from C
   libraries (`cinclude`) have no provenance metadata. Current ZER marks these
   as `*opaque` with runtime tagging (Level 1-5 system), but pure-`*T` use is
   compile-time-tracked best-effort.

4. **Patterns the conservative compile-time tracker rejects** — even though
   they're provably safe with more sophisticated analysis (Rust accepts some
   of these via lifetime parameters).

### 1.3 What "universal pointer safety" means

The aspirational goal: **every pointer in safe ZER code has its temporal
safety (no UAF, no double-free) and spatial safety (no OOB) guaranteed at
compile time or runtime, with no patterns rejected as inexpressible.**

The challenge: this conflicts with several competing design constraints
(zero runtime cost, no lifetime annotations, predictable performance,
8-byte pointers, no shadow memory overhead). These constraints can't all
be satisfied simultaneously — see Section 4.

---

## 2. Current State of ZER's Pointer Model

### 2.1 Existing pointer types and their costs

```
*T              8 bytes           single-element pointer
                                  - ZER-CHECK tracks UAF / double-free path-sensitively
                                  - Allocation coloring tracks provenance
                                  - Indexing emits compiler warning ("use [*]T")
                                  - Non-null by guarantee (auto-zero conflict for struct fields)
                                  - Used for: FFI, MMIO, single-element references

[*]T            16 bytes          slice {ptr, len}
                                  - Bounds-checked runtime via _zer_bounds_check
                                  - VRP elides check when bounds provable
                                  - Range-for iteration supported
                                  - Used for: multi-element access, arrays, buffers

?T              varies            optional value
                                  - ?u32 = struct{value, has_value}
                                  - ?*T = null sentinel (still 8 bytes)
                                  - ?void = has_value only (no value field)
                                  - Forced unwrap via `orelse`

Handle(T)       8 bytes           Pool/Slab generation-tracked index
                                  - High 32 bits = generation, low 32 = index
                                  - Runtime check on slab.get(h) via slab.gen[idx]
                                  - Auto-deref: h.field → slab.get(h).field
                                  - Requires slab to be reachable at use site
                                  - Used for: Pool/Slab allocations

*opaque         8 or 16 bytes     type-erased pointer
                                  - Level 1: compile-time zercheck
                                  - Level 2: poison-after-free pattern
                                  - Level 3: inline header (~1ns check)
                                  - Level 4: global malloc interception
                                  - Level 5: full runtime tracking
                                  - Used for: FFI boundaries, generic interfaces
```

### 2.2 Existing safety mechanisms covering pointer use

| Mechanism | What it covers | Where it lives |
|---|---|---|
| zercheck | UAF, double-free, leak on Handle | zercheck_ir.c |
| Allocation coloring | Cross-allocator escape | checker.c |
| Range propagation (VRP) | Bounds check elision | checker.c |
| Auto-guard insertion | Runtime bounds when unprovable | emitter.c |
| Escape detection | Pointer outliving scope | checker.c |
| MMIO range tracking | @inttoptr bounds | checker.c |
| Qualifier preservation | const/volatile through casts | types.c |
| FuncSummary / FuncProps | Cross-function lifetime / effect | checker.c |
| keep parameter | Pointer-can-be-stored marker | checker.c (limited use) |
| shared annotation | Cross-thread access | checker.c + emitter.c |
| range-for snapshot | Iterator invalidation | parser.c desugaring |

### 2.3 What's missing for "universal" safety

- **User-extensible allocators** with same safety as built-ins
- **Self-contained pointers** that travel through unknown code
- **Detection of escape sites** that aren't currently keep-checked
- **A single annotation** (instead of having multiple narrow mechanisms)

---

## 3. The Goal — Universal Pointer Safety

### 3.1 Properties we'd like to achieve

A hypothetical "perfect" universal pointer design would have:

1. **8-byte pointers** (no fat pointers, predictable RAM cost)
2. **Zero runtime overhead** (no per-deref checks)
3. **Universal coverage** (any pattern, any allocator, any usage)
4. **Zero annotations** (no `'a`, no `keep`, no `unsafe`)
5. **Compile-time guarantee** (bugs caught at compile, not runtime)
6. **No restructuring required** (any C-style code works)
7. **No special hardware required** (works on all archs GCC supports)
8. **Predictable cost** (no cache-miss cliffs or contention spikes)

### 3.2 Why these can't all be satisfied

There's a hard conservation law (Section 4) — you can have at most 4-5 of
these 8, never all 8. The trade is which ones to give up.

### 3.3 What ZER's audience actually needs

ZER's stated audience is **embedded/firmware/kernel developers transitioning
from C**. For this audience, the priorities are (approximately):

1. Predictable performance > clever optimization
2. Compile-time errors > runtime traps (for safety-critical code)
3. Low cognitive load > maximum expressiveness
4. Composability with existing C idioms > novel paradigms
5. Small pointer size matters but not absolutely (8 vs 16 bytes is debatable)
6. Runtime cost matters but not zero (a few percent acceptable)

The design space narrows once we acknowledge that ZER isn't trying to compete
with Rust on "maximum safety with zero runtime cost regardless of complexity"
or with C on "absolute zero overhead regardless of safety."

---

## 4. The Conservation Law (Impossible Triangle)

### 4.1 The fundamental trade

```
                  Universal coverage
                          /\
                         /  \
                        /    \
                       /      \
                      /        \
                     /          \
                    /            \
            Zero runtime     Zero annotations
                cost              required
```

**Pick at most two of three corners.** This isn't a soft trade — it's a
fundamental property of how compilers and programs work.

### 4.2 Where each existing language sits on the triangle

| Language | Universal coverage | Zero runtime | Zero annotations | Trade |
|---|---|---|---|---|
| C | ✗ (UB everywhere) | ✓ | ✓ | Sacrifices safety entirely |
| Rust | ✓ | ✓ | ✗ (lifetimes, traits) | Sacrifices annotation simplicity |
| Java/Go | ✓ | ✗ (GC) | ✓ | Sacrifices zero runtime |
| ZER current | Partial (~95%) | ✓ | ✓ | Sacrifices universal coverage (5% gap) |
| Hypothetical "perfect" | ✓ | ✓ | ✓ | Doesn't exist |

### 4.3 The three honest design points

Given the triangle, ZER's design choice for the universal pointer is between:

**Point A: Sacrifice runtime cost** (Universal + Zero annotations + Runtime cost)
- Examples: safe_ptr with shadow tag, tagged pointer + header, runtime-checked Handle
- ~1-5% whole-program overhead
- Some memory overhead (shadow, header)
- No annotations needed
- Universal pattern acceptance

**Point B: Sacrifice annotations** (Universal + Zero runtime + Annotations)
- Examples: Rust's full borrow checker, ZER's keep-design at universal scale
- Zero runtime cost
- 1-many annotations per function
- Universal pattern acceptance (with annotations)

**Point C: Sacrifice universal coverage** (Zero runtime + Zero annotations + Restricted patterns)
- Examples: ZER's current design, ML-Kit region inference
- Zero runtime cost
- Zero annotations
- ~5% of patterns rejected as inexpressible

Most of the conversation has been probing these three corners with different
implementations.

### 4.4 Why CHERI / hardware capabilities are a different category

Hardware capabilities (CHERI, ARM Morello) seem to break the triangle —
they offer universal coverage + low runtime + zero annotations. But they
require special hardware. From ZER's perspective (works on every arch GCC
supports), this is out of scope. ZER targets software-only universal safety.

---

## 5. The Audience Question (Who Pays What)

### 5.1 The framing

Every design has shareholders who pay for the design choice. The question
isn't "which is best?" — it's "which shareholders are we asking to pay,
and how much?"

### 5.2 ZER's stated vs implied audience

**Stated**: embedded/firmware/kernel developers transitioning from C with
SPARK/Ada-tier safety goals but without SPARK/Ada complexity.

**Implied by recent design directions**: shifting toward general-purpose
systems programming where users want safety without thinking deeply about
allocator choice or lifetime annotations.

These are different audiences with different acceptable tradeoffs. The
audience decision is upstream of the design decision.

### 5.3 Audience-specific design fit

| Audience | What they care about | Best-fit design |
|---|---|---|
| Cortex-M0 / cost-sensitive embedded | Every byte of RAM, deterministic timing | 4-rule borrow checker, two-type design, current ZER |
| Modern embedded (M4 and up) | Productivity, safety, moderate RAM | keep design, tagged+header, always-fat |
| Kernel/firmware (cycle-counting) | Minimize runtime cost, accept restructuring | keep design, 4-rule design |
| General systems | Universal safety without thinking | safe_ptr, tagged+header, always-fat |
| Rust refugees | Familiar lifetime model | Not a great match — they'll use Rust |
| C transitioners | Familiar syntax + safety | keep design, current ZER, tagged+header |

### 5.4 Modern MCU RAM reality

User pushed back on the "embedded = kilobytes" assumption. Reality check:

| MCU class | RAM range | Common use |
|---|---|---|
| Cortex-M0/M0+ | 8-32KB | Cost-sensitive sensors, simple controllers |
| Cortex-M3/M4 | 64-256KB | Industrial IoT, mid-range applications |
| Cortex-M7 | 256KB-1MB | High-end MCU applications |
| ESP32 variants | 320KB-8MB | WiFi/BT IoT, audio, video |
| Raspberry Pi Pico (RP2040) | 264KB | Maker projects, modern embedded |
| Automotive (Aurix, R-Car) | 4-16MB | ECUs, ADAS |
| Embedded Linux (Cortex-A) | 256MB-4GB | Edge servers, mid-tier devices |

The bottom of the market still cares about every byte. Middle and top
increasingly don't. If ZER targets Cortex-M3 and up, 16-byte pointers
become acceptable. If ZER targets Cortex-M0, every byte matters.

---

## 6. Designs Explored

### 6.1 safe_ptr with Shadow Tag (initial proposal)

**Design summary:**

```
safe_ptr<T> = 16 bytes (or 8 with tag in high bits)
  - {address, tag}
  - Tag set at allocation, written to shadow memory
  - Tag check on every deref: shadow[addr] == ptr.tag
  - Tag invalidated on free
  - Monotonic counter ensures tag never reuses → deterministic UAF detection
```

**Pros:**
- Universal coverage (any custom allocator)
- Zero annotations
- Self-contained (no slab in scope needed)
- Deterministic UAF detection (with monotonic counter)
- Composes with `[*]T` for spatial safety

**Cons:**
- Shadow memory overhead (~12.5% of heap)
- Per-deref runtime cost (3-5% typical, 50-300+ cycles worst case)
- 16-byte pointer (or 8-byte but separate shadow region)
- Framing risk ("ZER is just C with shadow tags")
- Sub-granule overflow not caught (within same shadow byte)
- TOCTOU race in multi-threaded code (without per-allocation lock or RCU/hazard)
- Probabilistic gap with finite tag bits (1/65K for 16-bit, eliminated by 64-bit monotonic)

**TOCTOU honest analysis:**

```
Thread A: check shadow[addr] == p.tag → pass
                                          ↑ race window
Thread B: free(p), realloc → shadow[addr] = new_tag
Thread A: *(p.addr) → reads freed/reallocated memory
```

For TOCTOU to be closed:
- Atomic check+deref (impossible without HW support)
- Per-allocation lock (expensive)
- Epoch-based reclamation (EBR) — defer free to safe epoch
- Hazard pointers — publish in-use addresses, scan before free
- Use ZER's existing `shared` analysis to forbid cross-thread without explicit shared variant

**Monotonic counter analysis:**

With 64-bit monotonic counter:
- 2^64 = 1.8×10^19 allocations
- At 1B allocs/sec: 570 years before wrap (NOT "age of universe" as initially overclaimed)
- Practically infinite for any real program
- Per-thread counter shards: 56-bit per thread = ~2200 years at 1M/sec per thread
- Thread ID recycling concern: monotonic thread ID allocation needed

**Shareholder cost:**
- Embedded users with tight RAM: shadow memory overhead is real
- General-purpose users: ~1-2% whole-program runtime cost
- Multi-threaded users: TOCTOU race requires protocol selection

**Decision status:** Probe. Not committed. The runtime cost is real and the
shadow memory overhead doesn't fit cost-sensitive embedded targets.

---

### 6.2 Always-Fat Handle (Option A) — 16 bytes always

**Design summary:**

```
Handle(T) = 16 bytes ALWAYS
  - {slab_pointer, index, generation}
  - Self-contained (carries slab reference)
  - Runtime check via embedded slab pointer + gen array
  - No thin variant exists — every Handle is universal
```

**Pros:**
- One pointer type for all pointer-like uses
- Universal portability (no slab-in-scope requirement)
- No PortableHandle, no safe_ptr needed
- Predictable cost (10-20 cycles per deref, slab ptr in register)
- No shadow memory overhead
- Composes with `[*]T` naturally
- Single mental model for users

**Cons:**
- 8 bytes extra per Handle field, EVERYWHERE
- Catastrophic on dense embedded data structures
- Linux kernel-style structs with thousands of Handle fields: ~8KB extra per struct
- Cortex-M0 microcontroller (32KB RAM total) loses several percent of total RAM
- "Pay for portability everywhere even when not needed"

**Shareholder cost:**
- Embedded with tight RAM: pays in field bloat (real cost)
- General-purpose: pays in tiny memory overhead (acceptable)
- Cache behavior: more pointers per cache line means fewer Handles fit
- ABI breakage: existing 8-byte Handle code must migrate

**Why we rejected Option A as universal answer:**

The user pushed back correctly: ZER's stated audience is embedded, and 8
extra bytes per Handle field is a real cost for dense data structures.
Making all Handles 16 bytes to gain portability the embedded audience
doesn't usually need is a misallocation of the cost.

**Decision status:** Probe. Rejected as universal answer (user pushed back),
but viable if ZER pivots to a non-embedded-first audience.

---

### 6.3 Two-Type (Handle thin + safe_ptr fat)

**Design summary:**

```
Handle(T) = 8 bytes
  - Existing thin Handle
  - Used in performance-critical hot paths
  - Requires slab in scope at use site

safe_ptr<T> = 16 bytes (opt-in)
  - Self-contained, portable
  - Used in cold paths, generic containers, callbacks, async
  - Same shadow tag mechanism
```

**Pros:**
- Embedded keeps 8-byte Handle for hot paths
- General-purpose can opt into safe_ptr where needed
- User picks per use case
- No forced cost on either side

**Cons:**
- Two pointer types in source — cognitive load
- "Which one do I use?" question for every declaration
- Code reviewers must evaluate the choice
- Beginners must learn the distinction

**Shareholder cost:**
- Beginners: learning two types, potential misuse
- Code reviewers: evaluating choice at every declaration
- Library authors: choosing API surface (Handle vs safe_ptr)
- Power users benefit from per-case optimal choice

**Decision status:** Probe. Viable but adds cognitive complexity. Not the
"single concept" design users seemed to be reaching for.

---

### 6.4 PortableHandle (cosmetic rename of safe_ptr — REJECTED)

**Design summary:**

PortableHandle(T) = same as safe_ptr<T> structurally:
```
PortableHandle(T) = {slab: *Slab(T), index: u32, generation: u32}
                  = 16 bytes
```

**Why this was proposed:**

To avoid the "framing risk" of safe_ptr ("ZER is just C with shadow tags").
By framing it as a Handle variant rather than a separate pointer system,
the language story stays "Handle family" instead of "Handle + shadow tag
parallel mechanism."

**Why this was rejected:**

The user correctly identified that **PortableHandle is just safe_ptr with
cosmetic renaming**. The mechanism is identical: 16-byte self-contained
runtime-checked reference. The naming change doesn't actually reduce
complexity — it just disguises that we're adding a parallel mechanism.

Honest framing: PortableHandle ≡ safe_ptr. Either name it honestly
(safe_ptr) or commit to making it actually different from safe_ptr (which
would require a fundamentally different mechanism).

**Decision status:** Rejected. Cosmetic renaming doesn't add value.

---

### 6.5 User-Extensible Slab returning Handle

**Design summary:**

Don't add safe_ptr or PortableHandle. Instead, make ZER's existing Slab(T)
user-implementable:

```
impl Slab<Task> for MyCustomAllocator {
    storage: u8[1024],
    used: usize,
    gens: u32[64],
    
    fn alloc(self) -> Handle(Task) { ... }
    fn free(self, h: Handle(Task)) { ... }
    fn get(self, h: Handle(Task)) -> *Task { gen check }
}
```

User writes any allocator they want; it returns Handle(T); downstream code
uses the same Handle infrastructure that already exists.

**Pros:**
- Full custom allocator support (any backing storage)
- Universal temporal safety (Handle gen check, deterministic)
- No new language type
- No new runtime infrastructure (no shadow memory)
- Composes with everything (auto-deref, escape analysis, leak detection)
- Embedded-friendly (no shadow memory tax)
- Single concept (Slab) extended with user impls

**Cons:**
- Handle requires slab to be reachable at use site (limits portability)
- Can't store Handle in generic container that outlives slab
- Can't pass Handle through opaque callback context
- Cross-module use requires importing the slab

**The portability gap:**

User correctly pointed out: Handle's "slab-must-be-in-scope" requirement
breaks for:
- Storage in generic Map<K, V>
- Pass to C callback that returns later
- Async future captures across .await
- Cross-module pointer queues

For these cases, the receiver doesn't have the slab reference. Handle
can't be dereferenced.

**Decision status:** Strong probe but incomplete on portability axis.

---

### 6.6 Slab-Table Indirection

**Design summary:**

Make Handle always 8 bytes but make it self-contained by encoding
allocator identity in the handle and looking up the slab in a global table:

```
Handle(T) = u64 = (slab_id << 48) | (index << 24) | gen
  - 16-bit slab_id (65K slabs max)
  - 24-bit index (16M slots per slab)
  - 24-bit gen (16M gen wraps)

Global slab table: slab_id → *Slab(T)

deref(h):
  slab = slab_table[h.slab_id]
  if (slab.gen[h.index] != h.gen) trap
  return slab.slots[h.index]
```

**Initial pros (as I oversold):**
- 8 bytes always (RAM-efficient)
- Self-contained (no slab in scope needed)
- Single pointer type
- Composes with everything

**Honest pros after user pushback:**
- Same as above, with caveats

**Cons (the user correctly identified):**

I initially oversold the per-deref cost as ~5 cycles. **The realistic worst
case is much higher:**

```
Cycle breakdown:
  Extract slab_id from handle:                  1 cycle
  Load slab_table[slab_id]:                     4-300+ cycles (cache dependent)
  Dereference slab pointer:                     1 cycle
  Load slab.gen[index]:                         4-300+ cycles (cache dependent)
  Compare to handle.gen:                        1 cycle
  Branch:                                       1 cycle

Best case (L1 hit on both):                    ~10-15 cycles
L2 hit on one:                                  ~20-30 cycles
L3 hit on one:                                  ~50-80 cycles
DRAM hit on one:                                ~200-300 cycles
Cache line ping-pong (multi-core contention):  ~300-1000+ cycles
NUMA cross-socket:                              ~500-1500+ cycles
```

For ZER's embedded audience requiring real-time predictability:
- Variable cycle cost is unacceptable for hard real-time
- Global table is a contention hotspot in multi-threaded code
- Adds an extra cache line load per deref vs always-fat Handle

**Comparison to alternatives:**

| Design | Best case | Realistic | Worst case (contended MT) | Predictable? |
|---|---|---|---|---|
| Current thin Handle | 5 cycles | 5-10 | 10-20 | ✓ |
| Always-fat Handle (16B) | 5 cycles | 5-10 | 10-20 | ✓ |
| **Slab-table Handle (8B)** | **10 cycles** | **20-50** | **100-1000+** | **✗ variable** |
| Two-type design | 5 cycles | 5-10 | 10-20 | ✓ |

**Decision status:** Rejected. The variable cost (especially in multi-threaded)
disqualifies it for ZER's predictable-performance audience. Always-fat Handle
strictly better on the dimensions that matter.

---

### 6.7 Tagged Pointer + Allocation Header (8-byte hybrid)

**Design summary:**

```
Pointer (8 bytes):
  [16-bit version tag][48-bit address]
                                          ↑
                                          48-bit = 256TB addressable

Allocation in memory:
  [8-byte header: version word]
  [user data starts here...]

Deref of pointer p:
  header = *(p.addr - 8)
  if (header.version != p.version) trap("UAF")
  use *(p.addr)

Free:
  header.version = invalidate_sentinel
```

8 bytes per pointer, 8 bytes per allocation, ~5-30 cycle runtime check
per deref (cache-dependent).

**Pros:**
- 8-byte pointer (same as raw *T)
- Header co-located with data (often cache-warm)
- No global table contention (vs slab-table)
- Predictable cost (one cache line for header)
- Composes with compile-time tracking (elide check when provable)

**Cons:**
- 8 bytes per allocation overhead
- ~5-30 cycle runtime check per deref when not elided
- FFI compatibility requires special handling (C pointers have no header)
- Header alignment concerns (user data starts at offset 8)

**Honest worst-case cost:**

| Scenario | Per-deref worst |
|---|---|
| Cold cache (first access) | ~200-300 cycles |
| Random pointer chase | ~100-200 cycles per node |
| Multi-threaded contention | ~500-1000 cycles |
| 5-level deep traversal, cold | 5 × 200 = 1000 cycles |
| Pathological pointer-chase | up to 50% slowdown momentarily |

Average case: ~2-5% whole-program overhead with compile-time elision.

**Comparison to existing ZER mechanism:**

This is essentially ZER's `*opaque` Level 3 ("inline header check") made
universal. The pattern already exists in ZER's codebase per CLAUDE.md
("~1ns inline header check at @ptrcast"). Just extending it to all `*T`.

**Decision status:** Probe. Viable as 8-byte universal runtime-checked
design. Worst case is real but bounded.

---

### 6.8 100% Runtime-Checked Semantically (optimization-driven)

**Design summary:**

Drop the 80/20 hybrid framing. Semantically, EVERY deref is runtime-checked.
Compile-time elision is purely an optimization (when missed, just runs the
check, no error).

```
Semantic model:
  Every *T deref runtime-checks the version against allocation header.
  No compile errors about pointer use.
  No restructuring required.
  No "inexpressible patterns" — everything compiles.

Implementation:
  Compiler elides checks when it can PROVE the pointer is alive in
  the current scope. This is just optimization — if compiler misses
  a case, no error, just runs the (cheap) check.
```

Same pattern as how `[*]T` bounds checks work today: semantically every
index is checked; in practice, VRP elides the check when it can prove
safety. Same pattern applied to pointer derefs.

**Pros:**
- No compile errors about pointer lifetime
- No restructuring forced
- Everything compiles
- Single conceptual model (always checked, sometimes optimized)
- Better than 80/20 hybrid: no inexpressible 5%

**Cons:**
- ~2-5% whole-program overhead (vs ~1% for 80/20)
- Slightly higher cost as the price for universal acceptance

**Whole-program cost estimate:**

```
Typical code (computation-heavy):     ~0.5-1%
Mixed code (typical app):              ~2-5%
Pointer-heavy (graph/list):            ~5-15%
Pathological (cold cache):             ~20-30% momentarily
```

**Decision status:** Probe. Cleaner conceptual model than 80/20.

---

### 6.9 4-Rule Strict Borrow Checker (zero annotation)

**Design summary:**

Pure compile-time enforcement on plain 8-byte `*T`:

```
Rule 1: Every *T has an allocation source (compile-time tracked)
        If unknowable → compile error

Rule 2: *T cannot outlive its allocation source's scope
        Path-sensitive tracking like zercheck handles

Rule 3: *T returned from function must trace to a param or 'static
        Else local allocation escapes

Rule 4: *T stored in struct/global must have lifetime ≥ container's
        Else container outlives pointee
```

**Pros:**
- 8 bytes always
- Zero runtime cost
- Zero annotations
- Compile-time guarantee

**Cons (the user pushed back on these):**

Real failure cases that compile error has no clean fix:

| Pattern | Fix |
|---|---|
| Generic container `Map<u32, *T>` | No fix in pure *T mode — restructure |
| Async capture across .await | Copy data instead — restructure |
| Linked list `*Node next` from non-allocator source | Forces allocator usage |
| Self-referential structures | Forces indices-into-Slab pattern |
| Cross-module pointer storage | Restructure or use Handle |
| Function pointer holding *T | Can't track through indirect call |

For these cases, compiler error has no 1-line fix — requires architectural
restructuring. ~5% of code is genuinely inexpressible.

**Honest scoring:**

```
~80% of code: works with no-brainer fixes (compiler suggestions)
~15% of code: works after moderate restructuring
~5% of code:  simply not expressible in pure *T mode
```

**Decision status:** Probe. Rejects ~5% of valid patterns. User pushed back —
this is too restrictive for "no brain" UX.

---

### 6.10 Single-Annotation `keep` Borrow Checker

**Design summary:**

ONE annotation extends ZER's existing keep parameter system to be universal:

```
Default: *T is non-escape (function-scope only, can't be stored)

Annotation: `keep`
  - On parameter:  fn f(keep p: *T) — caller guarantees p lives long enough
  - On local var:  keep *T x = ...  — x can be stored persistently
  - On struct field: struct S { keep *T data } — field stores pointers
  - On return:     implicit (caller scope determines lifetime)
  - On global:     implicit (always 'static)

Compiler enforces:
  - Non-keep *T cannot be stored persistently
  - keep *T propagates lifetime constraint to caller
  - Compiler chases keep pointers to allocation source
  - Error if source lifetime insufficient
```

**Pros:**
- 8 bytes always
- Zero runtime cost
- ONE annotation (`keep`), only at escape sites
- Compiler detects every keep-required site and tells user where to add it
- 95% of code: zero annotation needed (default)
- 5% of code: one word added per escape site (compiler-guided)
- Builds on ZER's existing keep infrastructure (System #21)
- Universal coverage for lifetime axis
- Composable with existing safety mechanisms

**Cons:**
- Treats all keep pointers as caller-scope (loses Rust multi-lifetime expressiveness)
- Some patterns require restructuring (rare)
- User must learn the `keep` keyword

**What compiler can auto-detect:**

| Pattern | Compiler action |
|---|---|
| `global_var = p` (non-keep) | Error: "add keep to parameter p" |
| `struct.field = p` where field is keep-slot | Error: "add keep" |
| `container.insert(p)` | Error: "add keep" |
| `spawn(f, p)` | Error: "add keep or shared" |
| `return p` where p is local-derived | Error: "returning non-keep pointer" |
| Pass non-keep to function expecting keep | Error: "propagate keep up" |
| Async capture | Error: "add keep — captured across .await" |

User experience: write *T everywhere, fix compile errors by adding `keep`
where compiler points. Same as adding `const` where compiler suggests.

**What compiler CAN'T detect (boundary cases):**

| Case | Resolution |
|---|---|
| Function pointer callback contents | Declare keep on funcptr type, once |
| `cinclude` C functions | Declare keep on ZER binding, once |
| Generic constraints | Declare keep on generic, once |

These require ONE explicit declaration at the boundary, not at every call site.

**Multi-lifetime patterns analysis:**

User pointed out: multi-lifetime is mostly a Rust-imposed problem. Real
systems C code doesn't model lifetimes in types, so doesn't need
multi-lifetime expressiveness.

```
Pattern                                   Multi-lifetime needed?
─────────────────────────────────────────────────────────────────
Linked list traversal                     No — same allocator
Tree manipulation                          No — same allocator
Hash map: key + value pointers             No — both alive at call
Compare two strings                        No — both alive during compare
Long-lived cache of short-lived data       Smell — fix by copying
Iterator merge from multiple streams       Rarely
Generic library APIs                       Library author concern
```

For ZER's audience: multi-lifetime gap is mostly theoretical, not practical.

**Coverage analysis:**

For pointer-lifetime axis specifically: keep + non-keep is 100% universal.

For OTHER safety axes (these aren't keep's job):
- Aliasing exclusivity: not modeled (Rust-specific concern)
- Iterator invalidation: range-for snapshot handles it
- Concurrent modification: shared annotation handles it
- Spatial bounds: [*]T handles it
- Null deref: ?T handles it
- Type confusion: type_equals handles it
- Use of uninitialized: auto-zero handles it

Each safety axis has its own mechanism. keep is the lifetime-axis mechanism.

**Decision status:** Strong probe. Possibly the "sweet spot" design.
- Almost-no annotation cost (one word at escape sites)
- Zero runtime cost
- ~No restructuring required (universal coverage of lifetime axis)
- 8 bytes always
- Builds on existing infrastructure
- Composes with other safety axes

---

### 6.11 Mode-Based (per-target configuration)

**Design summary:**

Different ZER compilation modes for different audiences:

```
Embedded mode (--target=embedded):
  *T = raw 8-byte pointer
  ZER-CHECK path-sensitive tracking (current behavior)
  Conservative compile-time only, residual gaps accepted
  
General-purpose mode (--target=general):
  *T = safe_ptr or keep-enforced or tagged+header
  Universal runtime-checked or compile-checked
  No residual gaps, accept runtime/annotation cost
```

User picks the mode appropriate to their substrate. Language has both
configurations, neither forced on the other.

**Pros:**
- Serves multiple audiences without compromising either
- Embedded users get current cost profile
- General-purpose users get universal safety
- Strategic flexibility for ZER's positioning

**Cons:**
- Language has two semantic modes
- Code may not work across modes
- Library authors must consider both targets
- Documentation must cover both
- Implementation complexity

**Decision status:** Probe. Reasonable compromise but adds significant
language-level complexity. Last-resort option if no single design works
for both audiences.

---

## 7. Rejected Alternatives in Detail

### 7.1 Borrow checker with full lifetime annotations (Rust-style)

**Why rejected:** Doesn't fit ZER's design philosophy ("smart compiler,
dumb user" — see CLAUDE.md). Lifetime annotations ('a, 'b, 'static),
Send/Sync traits, Pin, variance, HRTB — all impose cognitive load that
takes 3-6 months for experienced programmers to internalize.

ZER explicitly chose "no borrow checker" as a design pillar. Rust's
approach is internally consistent but optimizes for different priorities
than ZER's audience needs.

### 7.2 Garbage collection

**Why rejected:** Catastrophic for ZER's audience.
- GC pauses (microseconds to milliseconds) incompatible with real-time
- Memory overhead (2-5x typical heap inflation)
- No control over allocation timing
- Embedded systems often can't afford GC runtime
- Defeats ZER's "transparent memory model" philosophy

### 7.3 CHERI hardware capabilities

**Why rejected:** Requires special hardware (CHERI CPU, ARM Morello).
ZER targets every architecture GCC supports. Excluding non-CHERI
targets isn't acceptable. Could revisit if CHERI becomes ubiquitous.

### 7.4 Pure region inference (Cyclone, ML-Kit)

**Why rejected:** Conservative inference rejects valid programs. The
"region" concept doesn't map cleanly to ZER's existing allocator model
(Pool/Slab/Arena are already region-like). Adding region inference on
top would either replace these (rewriting too much) or duplicate them
(adding complexity without clear benefit).

### 7.5 Reference counting (Rc/Arc-style)

**Why rejected:** Cycle leak issue (well-documented in Rust). Per-pointer
refcount overhead (10-15ns per inc/dec). Doesn't fit ZER's preference
for explicit allocator scopes. Could be implemented as user library on
top of one of the universal pointer designs, but not as core mechanism.

### 7.6 Linear types (Linear Haskell, ATS)

**Why rejected:** Extremely restrictive (each value used exactly once).
Awkward for systems programming where aliasing is common. High cognitive
load. No path to incremental adoption.

### 7.7 Tag in pointer + shadow region (initial safe_ptr proposal)

**Why rejected as primary design:** Shadow memory overhead (12.5% of heap)
is a real cost for embedded. Sub-granule overflow not caught. TOCTOU race
in multi-threaded code requires additional infrastructure. User pushback
on framing risk ("just C with shadow tags").

Could still be viable in a tagged-pointer-with-header variant (Section 6.7).

### 7.8 Per-allocation lock for TOCTOU

**Why rejected:** Locks every memory access. Defeats lock-free patterns.
Multi-threaded scalability collapses. Use RCU/EBR/hazard pointers instead.

### 7.9 Whole-program escape analysis for auto-promotion

**Why rejected:** ZER explicitly bans whole-program analysis (CLAUDE.md).
Per-file analysis insufficient for auto-promotion (ABI layout issues).
Forces struct layout to depend on usage patterns elsewhere in the
program — breaks cross-module compatibility.

### 7.10 Variable-size pointers at runtime

**Why rejected:** Physically impossible in low-level languages. Struct
fields, function ABI, array elements all require fixed sizes. Variable-
size would require complete reorganization of how CPUs access memory.
No production language does this for good reason.

---

## 8. Comparison to Other Languages

### 8.1 Rust

```
Mechanism: Compile-time borrow checker + lifetime parameters + Send/Sync traits
Pointer types: &T, &mut T, *const T, *mut T, Box<T>, Rc<T>, Arc<T>, Weak<T>, Pin<T>
Memory: Safe by default in safe code, unsafe escape for low-level
Cost: Zero runtime (in safe Rust)
Annotation burden: Heavy ('a everywhere, Send/Sync impls, trait bounds)
Cognitive load: Very high (3-6 months to internalize)
```

**Strengths over ZER:**
- Catches aliasing-class bugs (iterator invalidation, &mut exclusivity)
- Zero runtime cost universally
- Compile-time guarantee for everything in safe code

**Weaknesses vs ZER:**
- High annotation burden
- Rejects some valid programs (false positives)
- 3-6 month learning curve
- "Fighting the borrow checker" is a real phenomenon
- Complex async + lifetime + Send/Sync interactions
- Permits leaks (Rc<T> cycles silently leak)

### 8.2 Zig

```
Mechanism: Comptime + explicit allocator interfaces
Pointer types: *T, [*]T, []T, ?*T, [*c]T (C ABI)
Memory: Programmer-explicit allocator passed to every alloc
Cost: Zero runtime
Annotation burden: Low (no lifetimes)
Cognitive load: Medium (allocator-passing everywhere)
```

**Strengths over ZER:**
- Allocator interface allows custom allocators in safe-ish code
- Comptime metaprogramming for compile-time computation
- Pointer arithmetic on `[*]T` is allowed and bounds-checked

**Weaknesses vs ZER:**
- No automatic UAF detection (programmer responsibility)
- No lifetime tracking
- Allocators passed manually to every function

### 8.3 Go

```
Mechanism: Garbage collection
Pointer types: *T, slice, map, channel
Memory: GC managed, escape analysis for stack/heap
Cost: GC pauses (microseconds to milliseconds), 2-5x memory overhead
Annotation burden: None
Cognitive load: Very low
```

**Strengths over ZER:**
- Zero annotation
- No memory bug classes possible (GC handles all)

**Weaknesses vs ZER:**
- GC pauses incompatible with real-time
- 2-5x memory overhead
- Embedded systems can't afford GC

### 8.4 Swift

```
Mechanism: Automatic Reference Counting (ARC)
Pointer types: T, Optional<T>, Unmanaged<T>, UnsafePointer<T>
Memory: ARC for class types, value semantics for structs
Cost: Per-pointer refcount inc/dec (~10ns each)
Annotation burden: Low (weak/strong/unowned references)
Cognitive load: Medium (when to use weak vs strong)
```

**Strengths over ZER:**
- Familiar to OO programmers
- Automatic cleanup

**Weaknesses vs ZER:**
- Refcount overhead
- Cycle leak issue (handled by weak refs)
- Less suited to systems programming

### 8.5 C

```
Mechanism: None — programmer responsibility
Pointer types: T*, void*, function pointers
Memory: malloc/free manual
Cost: Zero runtime, zero compile-time safety
Annotation burden: None
Cognitive load: Low (familiar to systems programmers)
```

**Why ZER exists:** C's "trust the programmer" model produces 70% of CVEs
in mature codebases as memory safety bugs. ZER aims to keep C's simplicity
while eliminating the bug classes.

### 8.6 ZER current state

```
Mechanism: 4 memory generics + path-sensitive zercheck + allocation coloring
Pointer types: *T, [*]T, ?T, Handle(T), *opaque
Memory: Pool/Slab/Ring/Arena explicit allocators
Cost: Zero runtime (compile-time only)
Annotation burden: Low (Pool/Slab/Ring/Arena selection)
Cognitive load: Low (familiar to embedded C programmers)
```

**Where ZER current state sits:**
- Stronger than C on safety
- Weaker than Rust on universal coverage (~5% gap)
- Same runtime cost as C (zero)
- Lower cognitive load than Rust
- Higher allocator awareness than Go/Java

The universal pointer design aims to close the 5% gap while preserving
ZER's other properties.

---

## 9. The Orthogonal Safety Axes Framing

### 9.1 Each safety axis has its own mechanism

ZER's safety isn't one big mechanism — it's a layered system where each
axis is covered by its own targeted mechanism.

```
Safety axis                    ZER mechanism                          Coverage
─────────────────────────────────────────────────────────────────────────────
Pointer lifetime / escape      keep / non-keep (proposed) OR          
                                  safe_ptr/tagged+header (proposed) OR
                                  current zercheck + allocation coloring  varies
Spatial bounds                 [*]T + .len + _zer_bounds_check        100%
Null safety                    ?T + orelse                             100%
Type safety                    type_equals                             100%
Initialization                 auto-zero                               100%
Concurrency safety             shared / threadlocal                    100%
Iteration safety               range-for snapshot                      100%
MMIO safety                    mmio declarations                       100%
Qualifier preservation         @ptrcast checks                         100%
Cross-allocator safety         allocation coloring                     100%
Stack overflow                 --stack-limit analysis                  100%
```

### 9.2 Each axis independent

The universal pointer design is about the **pointer-lifetime axis only**.
It doesn't replace or affect the other axes — they each have their own
mechanism that continues to work.

This is important: when evaluating universal pointer designs, only the
pointer-lifetime coverage matters. Other axes (bounds, null, types, etc.)
are already covered.

### 9.3 What this means for keep / safe_ptr

For the keep design:
- Covers pointer-lifetime axis: 100%
- Other axes covered by existing mechanisms
- Result: universal compile-time safety, multi-axis

For safe_ptr / tagged+header:
- Covers pointer-lifetime axis: 100% (runtime check)
- Other axes covered by existing mechanisms
- Result: universal safety, mixed compile/runtime, single annotation surface

For 4-rule borrow checker:
- Covers pointer-lifetime axis: ~80% (5% inexpressible)
- Other axes covered by existing mechanisms
- Result: high compile-time safety but with restructuring forced

---

## 10. Shareholder Analysis Across All Designs

### 10.1 Always-Fat Handle (16 bytes)

| Shareholder | Gets | Pays |
|---|---|---|
| General-purpose users | Universal portability, single concept | Tiny memory overhead per Handle field |
| Embedded with tight RAM | Same Handle works everywhere | 8 extra bytes per Handle field (significant on M0) |
| Kernel developers | Portable Handle across contexts | Cache lines hold fewer Handles |
| Beginners | Simple — one pointer type | Nothing |
| Library authors | API stability | Slightly larger struct sizes |

### 10.2 Tagged Pointer + Allocation Header (8 bytes)

| Shareholder | Gets | Pays |
|---|---|---|
| All users | 8-byte pointer, universal safety | 2-5% whole-program runtime overhead |
| Tight inner loops | Compile-time elision when possible | 5-15% overhead in pointer-chase code |
| Embedded users | 8 bytes per pointer (no field bloat) | 8 bytes per allocation (header) |
| FFI users | Compatible with raw *T at boundary | Header missing on C-allocated memory |
| Multi-threaded | Header co-located with data | Cache contention if shared header |

### 10.3 Single-Annotation keep Borrow Checker

| Shareholder | Gets | Pays |
|---|---|---|
| Default users (95%) | Just write *T, no annotation needed | Nothing |
| Storage-writing code (5%) | One word per escape annotation | Minor cognitive load |
| Compiler implementers | Builds on existing keep + zercheck | Some work to extend universally |
| Embedded users | 8-byte pointers, zero runtime cost | Annotation discipline |
| Power users | Most patterns work | Some complex patterns need restructuring |
| Generic container authors | More explicit container contracts | Slightly more explicit API |
| Migration from C | Drop-in for most code | Add keep where compiler complains |

### 10.4 4-Rule Strict Borrow Checker

| Shareholder | Gets | Pays |
|---|---|---|
| Users with simple patterns (80%) | Zero annotation, zero runtime | Nothing |
| Users with moderate patterns (15%) | Still works after restructuring | Some refactoring work |
| Users with complex patterns (5%) | Compile error, no clean fix in pure *T | Architectural change forced |
| Embedded users | 8 bytes, zero runtime | Acceptance that some patterns can't be written |

### 10.5 safe_ptr with Shadow Tag

| Shareholder | Gets | Pays |
|---|---|---|
| General-purpose users | Universal coverage, zero annotation | 1-2% whole-program runtime |
| Embedded users | Universal coverage | 12.5% heap as shadow memory (often unaffordable) |
| Multi-threaded users | Works without per-pointer locks | TOCTOU race needs additional protocol |
| Real-time users | Universal coverage | Variable cycle cost (cache-dependent) |

### 10.6 Mode-Based

| Shareholder | Gets | Pays |
|---|---|---|
| Embedded users | Current ZER behavior preserved | Mode flag at build time |
| General-purpose users | Universal safety opt-in | Mode flag at build time |
| Library authors | Both modes available | Must consider both targets |
| Documentation writers | Both modes to cover | Larger documentation surface |
| Beginners | Picks mode early | Choice of two semantic models |

### 10.7 User-Extensible Slab returning Handle

| Shareholder | Gets | Pays |
|---|---|---|
| Custom allocator authors | Implement any allocator returning Handle | unsafe blocks for raw memory work |
| Downstream users | Same Handle API as built-in Slab | None (existing pattern) |
| Embedded users | No new pointer type, no shadow memory | None |
| Users needing portability | Limited — Handle needs slab in scope | Architectural restructuring for cross-context |

---

## 11. Cost Models in Detail

### 11.1 Per-deref cycle costs

```
Mechanism                              Best (L1)   Realistic   Worst (cold/contention)
─────────────────────────────────────────────────────────────────────────────────────
Raw *T pointer                          1           1           1-5 (TLB)
Current Handle (slab in scope)          5           5-10        10-20
Always-fat Handle (16B)                 5           5-10        10-20
Tagged+header (compile-elided)          1           1           1-5
Tagged+header (runtime check)           5           10-30       100-500
Shadow tag in pointer top bits          5           10-30       100-500
Slab-table indirection                  10          20-50       100-1000+
safe_ptr (separate shadow region)       5           10-30       100-300
Hazard pointer + tag                    10          15-40       200-500
Epoch-based reclamation                 5           5-15        30-100
```

### 11.2 Memory overhead

```
Mechanism                              Per pointer   Per allocation   Global table
─────────────────────────────────────────────────────────────────────────────────────
Raw *T                                 8 bytes       0                0
Always-fat Handle                      16 bytes      0                0
Tagged+header                          8 bytes       8 bytes          0
Shadow tag (separate region)           8 bytes       0                12.5% of heap
Slab-table                             8 bytes       0                ~1MB (65K slabs)
safe_ptr (16-bit tag in pointer)       8 bytes       0                3% shadow
safe_ptr (32-bit tag in pointer)       12 bytes      0                6% shadow
safe_ptr (64-bit monotonic tag)        16 bytes      0                12.5% shadow
```

### 11.3 Whole-program impact estimates

```
Workload type                          With safe_ptr   With tagged+header   With keep
─────────────────────────────────────────────────────────────────────────────────────
Computation-heavy                      ~0.5%           ~0.5%                0%
Mixed application code                 ~2-3%           ~2-5%                0%
Pointer-heavy (graph/tree)             ~5-10%          ~5-15%               0%
Tight pointer-chase loops              ~10-20%         ~10-25%              0%
Real-time control loops                Variable        ~2-5% predictable    0%
Embedded firmware (typical)            ~1-2%           ~1-2%                0%
Kernel hot paths                       ~3-8%           ~3-8%                0%
```

### 11.4 Cache behavior analysis

**Tagged+header (header co-located):**
- For small allocations (≤56 bytes): header fits in same cache line as data
- First data access loads cache line containing header
- Subsequent checks free (header in L1)
- Best-case cost: ~1ns per check after warmup

**Slab-table:**
- Global table is shared cache line(s)
- Multi-threaded: cache line bouncing if multiple cores allocate/free
- Single-threaded: usually warm after first access
- Worst case: 200-500 cycles per check during cache thrashing

**Shadow tag (separate region):**
- Shadow region typically cold (large, sparse access)
- Each new allocation: shadow line cold
- Multi-allocation traversal: many cache misses
- Mitigation: align allocations to granule boundaries

### 11.5 Compile-time elision rates (estimates)

```
Code pattern                                  Elision rate
────────────────────────────────────────────────────────────
Linear use within function                    95%+
Pointer passed to single function and used    85%
Pointer stored in local var, used multiple    80%
Pointer in for-loop, body uses it             70%
Cross-function pointer with FuncSummary       60%
Pointer through generic container             20%
Pointer through async/callback                10%
Pointer through opaque function pointer       0% (must check)

Average elision rate: ~70-85% in typical code
```

### 11.6 Branch prediction impact

```
Once warmed up, version checks become:
  if (header.version != ptr.version) trap;
       ↑ branch predictor: 100% "not taken" in correct code
       
  Cycles: ~1-2 when predicted (entire check ~5-7 cycles total)
  
Mispredict cost: ~20 cycles (one-time when UAF actually fires)
```

---

## 12. Multi-Lifetime Patterns — Are They Real?

### 12.1 The user's observation

User pointed out: multi-lifetime expressiveness (Rust's `<'a, 'b>`) is
mostly a Rust-imposed concern, not a real systems-code concern.

### 12.2 Why Rust needs multi-lifetime

Rust models lifetimes as part of types. If you have two pointers with
different lifetimes, Rust treats them as different types until unified.
The multi-lifetime annotation is how Rust users unify them:

```rust
fn longest<'a, 'b>(x: &'a str, y: &'b str) -> &'a str {
    // Distinct lifetimes for x and y; result tied only to x
}
```

This expressiveness is needed because Rust's type system models the
distinction. Remove the distinction (treat lifetimes as programmer
responsibility), the expressiveness becomes unnecessary.

### 12.3 What real C code does

C has no lifetime modeling:

```c
const char *longest(const char *x, const char *y);
// Two pointers. Both T*. No lifetime in type.
// Programmer ensures both alive at call.
// Programmer ensures result used within validity scope.
```

Real C code rarely encounters "multi-lifetime patterns" because C doesn't
model lifetimes at all. The concept itself doesn't apply.

### 12.4 What ZER's keep design does

```
fn longest(x: [*]u8, y: [*]u8) -> [*]u8 {
    if x.len > y.len { return x; } else { return y; }
}
// No multi-lifetime annotation. Caller scope determines result lifetime.
// Programmer responsibility (with compile-time tracking where possible).
```

Closer to C's model. Sufficient for ~99% of real systems code.

### 12.5 When multi-lifetime expressiveness genuinely matters

```
Pattern                                          Frequency in real systems code
─────────────────────────────────────────────────────────────────────────────
Linked list traversal                             Very common — same allocator, single lifetime
Tree manipulation                                  Common — same allocator
Hash map: key + value                              Common — both alive at call
Compare two strings                                Very common — both alive during compare
Long-lived cache of short-lived data               Rare and a smell
Iterator merge from multiple streams               Rare
Generic library APIs maximally expressive          Library-only concern
Returning derived pointer from multiple inputs     Rare
```

For ZER's audience: multi-lifetime gap is theoretical, not practical.

### 12.6 Implication for design

Designs that don't support multi-lifetime expressiveness (keep design,
4-rule borrow checker) are NOT losing meaningful capability for ZER's
audience. The "less expressive than Rust" framing is accurate but the
gap is in features ZER's users don't typically need.

---

## 13. Implementation Notes per Design

### 13.1 keep design implementation

**Required compiler work:**

1. Extend existing keep marker handling to all pointer types (not just
   parameters)
2. Add keep marker on struct fields, local vars, globals (implicit), returns
3. Extend escape detection to recognize all keep-required sites
4. Implement keep cascade propagation up call stack
5. Emit clear error messages with directive fixes
6. Handle function pointer types with keep annotations
7. Update cinclude binding syntax to allow keep on extern function params

**Existing infrastructure that helps:**
- zercheck path-sensitive analysis
- FuncSummary cross-function tracking
- Allocation coloring
- Existing keep parameter (System #21)

**Estimated effort:** 2-4 weeks of compiler work. Mostly extending
existing systems, not building from scratch.

### 13.2 Tagged+header implementation

**Required compiler work:**

1. New pointer representation (16-bit version in top bits of 64-bit pointer)
2. Allocator integration: write version to header on alloc
3. Emit version check before every safe-pointer deref
4. Compile-time elision pass (similar to existing VRP for bounds)
5. FFI boundary handling (mark raw C pointers as unsafe escape)
6. Free emission: invalidate header on dealloc

**Required runtime work:**

1. Allocator header layout in Pool/Slab/Arena
2. Header invalidation primitives (_zer_header_invalidate)
3. Trap on version mismatch

**Estimated effort:** 4-6 weeks of compiler + runtime work.

### 13.3 safe_ptr implementation

**Required compiler work:**

1. New pointer type safe_ptr<T>
2. Shadow memory allocation at startup
3. Emit tag check before every safe_ptr deref
4. Compile-time elision (similar to bounds check elision)
5. Allocator trait for user implementations
6. Shadow memory invalidation on allocator drop

**Required runtime work:**

1. Shadow memory region management
2. Tag assignment (monotonic or per-thread)
3. Trap on tag mismatch
4. TOCTOU protocol (epoch, hazard, or shared-only)

**Estimated effort:** 6-10 weeks of compiler + runtime work.

### 13.4 Always-fat Handle implementation

**Required compiler work:**

1. Widen Handle layout from 8 to 16 bytes
2. Update auto-deref to use embedded slab pointer
3. Migrate existing 8-byte Handle code (significant)
4. Update FFI boundary patterns

**Estimated effort:** 2-3 weeks of compiler work + significant migration.

### 13.5 Mode-based implementation

**Required compiler work:**

1. Mode flag in compiler driver
2. Conditional codegen per mode
3. Cross-mode compatibility analysis
4. Documentation for both modes
5. Test suite for both modes

**Estimated effort:** 8-12 weeks for full mode-based support.

---

## 14. Open Questions

### 14.1 Audience commitment

**Question:** Does ZER target only embedded/firmware/kernel, or also
general-purpose systems programming?

**Why it matters:** Determines which designs are viable. Embedded-only:
keep design or current state. General-purpose acceptable: tagged+header
or safe_ptr also viable. Both audiences: mode-based design.

### 14.2 Cognitive load tolerance

**Question:** Is "compiler errors guide user to add `keep` annotation"
acceptable as cognitive load? Or does ZER want truly zero-annotation?

**Why it matters:** keep design requires user to add annotations.
safe_ptr / tagged+header require no annotations but pay runtime cost.

### 14.3 Runtime cost tolerance

**Question:** Is ~1-5% whole-program runtime overhead acceptable for
universal safety?

**Why it matters:** Tagged+header and safe_ptr require this. keep doesn't.

### 14.4 Pointer size sensitivity

**Question:** Is 8 bytes per pointer a hard requirement, or is 16 bytes
acceptable for the simplicity benefit?

**Why it matters:** Always-fat Handle requires 16 bytes; other designs
keep 8 bytes.

### 14.5 Framing risk tolerance

**Question:** Is "ZER is C with shadow tags" framing risk acceptable, or
should the design avoid runtime metadata regions?

**Why it matters:** safe_ptr and tagged+header introduce runtime metadata
(shadow region or allocation headers). keep doesn't.

### 14.6 Multi-threaded universal safety

**Question:** Should safe_ptr work cross-thread by default, or only with
explicit shared annotation?

**Why it matters:** Cross-thread requires TOCTOU mitigation (EBR, hazard,
or atomic refcount). Single-threaded is simpler.

### 14.7 FFI boundary semantics

**Question:** How do safe_ptr / Handle / keep interact with cinclude
C functions that return raw `T*` pointers?

**Why it matters:** Boundary semantics affect interop usability. Explicit
unsafe marker vs implicit conversion vs forbidden.

### 14.8 Migration story

**Question:** What's the migration path for existing ZER code if any
universal pointer design is adopted?

**Why it matters:** Breaking change vs additive change. Some designs
(always-fat Handle, mode-based) require migration. Others (keep,
tagged+header) are additive.

### 14.9 Composition with safe_ptr

**Question:** If keep is adopted, can it compose with safe_ptr-style
runtime checks for the cases keep can't fully cover?

**Why it matters:** Hybrid design might cover both compile-time and
runtime safety in different parts of the program.

### 14.10 Performance benchmarking

**Question:** Are the cost estimates in this doc accurate enough to
decide, or do we need actual benchmarks?

**Why it matters:** Worst-case scenarios (slab-table contention,
tagged+header cold cache) are hard to estimate without measurement.
Real benchmarks would tighten the decision.

---

## 15. Glossary of Terms

| Term | Definition |
|---|---|
| Universal pointer safety | Goal of having every pointer in safe code be safe at compile time or runtime, no rejected patterns |
| Pointer-lifetime axis | The safety dimension concerning when a pointer becomes invalid (UAF / dangling) |
| Spatial axis | The safety dimension concerning bounds (OOB array access) |
| Aliasing axis | The safety dimension concerning who can mutate what (Rust's `&mut` exclusivity) |
| Concurrency axis | The safety dimension concerning cross-thread access |
| `keep` annotation | Marker indicating a pointer can be stored persistently / escape function scope |
| Shadow tag | Per-allocation tag stored in separate shadow memory region |
| Allocation header | Per-allocation metadata stored co-located with the allocation |
| Slab-table | Global table mapping allocator IDs to allocator instances |
| safe_ptr | Generic name for the universal safe pointer (mechanism unspecified) |
| PortableHandle | Cosmetic rename of safe_ptr, rejected as duplicating concept |
| Always-fat Handle | Design where all Handles are 16 bytes carrying slab pointer |
| Tagged+header | Design with version in pointer + version in allocation header |
| 4-rule borrow checker | Strict compile-time tracking with no annotations |
| keep-design | Single-annotation borrow checker using `keep` keyword |
| Mode-based | Different compilation modes for different audiences |
| TOCTOU | Time-of-check-to-use race in concurrent code |
| EBR | Epoch-based reclamation (defer free to safe epoch) |
| Hazard pointers | Lock-free reclamation via per-thread hazard slots |
| Conservation law | Constraint that universal coverage + zero runtime + zero annotations is impossible |
| Impossible triangle | Visual representation of the conservation law |
| Shareholder analysis | Framework for identifying who pays the cost of each design |

---

## Appendix A: Conversation Highlights

### A.1 The starting point

Conversation began with the user proposing safe_ptr<T> as universal escape
hatch for custom allocators. The 4 built-in allocators (Pool/Slab/Ring/Arena)
cover ~95% of real systems-code patterns at zero runtime cost. safe_ptr
fills the ~5% gap for user-written allocators and patterns that don't fit
the 4 built-ins.

### A.2 The probe sequence

User explored multiple designs as probes into the tradeoff space:

1. safe_ptr with shadow tag (initial proposal)
2. Always-fat Handle (option A — sacrifice 8 bytes)
3. Two-type (Handle + safe_ptr — accept two concepts)
4. PortableHandle (cosmetic rename — rejected by user)
5. User-extensible Slab (don't add safe_ptr, extend Slab)
6. Slab-table indirection (8 bytes + global lookup)
7. Tagged pointer + allocation header (8 bytes + co-located header)
8. 100% runtime semantically (optimization-driven)
9. 4-rule borrow checker (zero runtime, restructure on 5%)
10. Single-annotation keep borrow checker
11. Mode-based (per-target configuration)

Each probe was honest about tradeoffs. None committed.

### A.3 Key insights from the conversation

**Insight 1: Conservation law is fundamental**

Universal coverage + zero runtime cost + zero annotations cannot all be
satisfied simultaneously. Pick at most two of three.

**Insight 2: PortableHandle = safe_ptr cosmetically renamed**

User correctly called out that the "Handle family" framing didn't reduce
complexity, just disguised it. Honest naming preferred.

**Insight 3: Slab-table cost was undersold**

I claimed "5 cycles per deref" for slab-table indirection. User pushed
back: realistic worst case is 50-300+ cycles in multi-threaded
contention. Honest cost: variable and potentially severe.

**Insight 4: Multi-lifetime is mostly Rust-imposed**

Real systems code doesn't need multi-lifetime expressiveness because
C doesn't model lifetimes in types. ZER's keep design (single-lifetime)
covers ~99% of real patterns.

**Insight 5: Each safety axis has its own mechanism**

Universal pointer safety is about the lifetime axis specifically. Other
axes (bounds, null, types, concurrency, iteration) have their own
mechanisms. Don't conflate axes when evaluating designs.

**Insight 6: keep is a strong probe**

Single-annotation keep checker:
- 8 bytes always
- Zero runtime cost
- Universal coverage on lifetime axis
- ~1 word per escape site (compiler-guided)
- Builds on existing ZER infrastructure
- ~No restructuring required

Hits a sweet spot in the tradeoff space.

**Insight 7: Audience commits design**

Embedded-first: keep design or current state.
General-purpose: tagged+header or safe_ptr.
Both: mode-based.

The audience decision is upstream of the design decision.

### A.4 User's stated preferences (gathered through conversation)

- Prefers 8-byte pointers if achievable
- Prefers no runtime cost if achievable
- Prefers minimal annotation cost
- Prefers compiler-guided error messages over silent acceptance
- Comfortable with restructuring "if compiler shows 1 right path among 9 wrong"
- Embedded RAM constraints matter but not absolutely (modern MCUs have MB-range RAM)
- Wants brainstorming / exploration mode, not premature commitment

### A.5 What's been explicitly rejected

- Borrow checker with full lifetime annotations (Rust-style)
- Garbage collection
- CHERI hardware capabilities
- Pure region inference
- Reference counting in core language
- Linear types
- Variable-size pointers at runtime
- Whole-program escape analysis for auto-promotion
- Slab-table indirection (variable cost disqualifies)
- PortableHandle (cosmetic rename, same as safe_ptr)

### A.6 What's still actively probing

- safe_ptr with shadow tag (16-byte version, monotonic counter)
- Tagged pointer + allocation header (8-byte hybrid)
- 4-rule strict borrow checker
- Single-annotation keep borrow checker
- Always-fat Handle (rejected as universal but viable for specific audience)
- Mode-based design

---

## Appendix B: Cost-Profile Microbenchmarks (Theoretical)

### B.1 Workload archetypes

**A. Computation-heavy** (~1 pointer deref per 100 arithmetic ops)
- Examples: signal processing, numerical algorithms, FFT
- Pointer access fraction: ~1%
- Expected universal safety overhead: ~0.5-1%

**B. Mixed application** (~1 deref per 10 ops)
- Examples: typical business logic, parsers, formatters
- Pointer access fraction: ~10%
- Expected universal safety overhead: ~2-5%

**C. Pointer-heavy** (~1 deref per 5 ops)
- Examples: graph traversal, tree manipulation, AST processing
- Pointer access fraction: ~20%
- Expected universal safety overhead: ~5-10%

**D. Tight pointer-chase** (>1 deref per op)
- Examples: linked list traversal, hash table probing
- Pointer access fraction: ~50%+
- Expected universal safety overhead: ~10-25%

### B.2 Per-deref cost matrix (theoretical)

```
Workload    | Raw *T | Handle | Always-fat | Tagged+header | safe_ptr  | keep      | Slab-table
                                                            (16B)        (compile)
─────────────────────────────────────────────────────────────────────────────────────────────────
A           | 1ns    | 5ns    | 5ns        | 1-5ns         | 1-5ns     | 1ns       | 20-50ns
B           | 1ns    | 5ns    | 5ns        | 5-15ns        | 5-15ns    | 1ns       | 50-100ns
C           | 1ns    | 5ns    | 5ns        | 10-30ns       | 10-30ns   | 1ns       | 100-200ns
D           | 1ns    | 5ns    | 5ns        | 30-100ns      | 30-100ns  | 1ns       | 200-500ns
```

### B.3 Whole-program overhead estimates (theoretical)

```
Workload    | Always-fat | Tagged+header | safe_ptr | keep | 4-rule
─────────────────────────────────────────────────────────────────────
A           | 0%         | 0.5%          | 1%       | 0%   | 0% (with restructuring on ~5%)
B           | 0%         | 2-5%          | 2-3%     | 0%   | 0% (with restructuring on ~5%)
C           | 0%         | 5-15%         | 5-10%    | 0%   | 0% (with restructuring on ~5%)
D           | 0%         | 10-25%        | 10-20%   | 0%   | 0% (with restructuring on ~5%)
```

### B.4 Memory overhead estimates

For a 100MB heap with ~1M allocations averaging 100 bytes each:

```
Mechanism                     | Memory overhead
─────────────────────────────────────────────────
Raw *T                        | 0 (baseline)
Always-fat Handle             | +8 bytes per Handle field — depends on struct count
Tagged+header                  | +8MB (8 bytes per allocation × 1M)
Shadow tag (1B per 64B granule)| +1.5MB (12.5KB granules × 8 = 100KB shadow... wait recalculate)
                              | Actually: 100MB / 64 = 1.56M granules × 1B = 1.56MB
Shadow tag (2B per 64B granule)| +3MB
Shadow tag (8B per 64B granule)| +12.5MB (truly large)
Slab-table (small table)       | +1KB (65K entries × small struct)
```

### B.5 Multi-threaded scaling estimates

```
Mechanism                | Single-thread | 4-thread | 16-thread (contended)
─────────────────────────────────────────────────────────────────────────
Raw *T                   | 1x            | 1x       | 1x
Always-fat Handle        | 1x            | 1x       | 1x (no shared state)
Tagged+header            | 1x            | 1x-1.1x  | 1.1x-1.3x (header in same cache line)
Shadow tag               | 1x            | 1.1x-1.3x| 1.5x-3x (shadow region shared)
Slab-table               | 1x            | 1.5x-3x  | 5x-20x (catastrophic contention)
keep (compile-time)      | 1x            | 1x       | 1x (no runtime state)
```

### B.6 Latency variance estimates

For real-time applications, p99 latency matters more than average:

```
Mechanism                | p50    | p99    | p99.9 (worst case)
─────────────────────────────────────────────────────────────────
Raw *T                   | 1ns    | 1ns    | 5ns (TLB miss)
Always-fat Handle        | 5ns    | 10ns   | 50ns (cache miss)
Tagged+header            | 5ns    | 30ns   | 300ns (cold cache)
Shadow tag               | 10ns   | 100ns  | 500ns (shadow cache miss)
Slab-table               | 20ns   | 200ns  | 2000ns (contention spike)
keep (compile-time)      | 1ns    | 1ns    | 5ns (TLB miss, same as raw)
```

For hard real-time: keep or always-fat Handle. Variable-cost options
(slab-table, shadow tag) unsuitable.

---

## Final Notes

This document is intended to be **read before proposing new pointer
designs**. If you find yourself reaching for an idea, search this doc
first — likely it's been mapped already, with honest tradeoffs noted.

The conversation that produced this document was explicitly in **brainstorming
mode** — each design probed the tradeoff space without committing. When ZER
eventually commits to a direction, that commitment will be a separate
document referencing the choices made here.

**No design in this document is "the answer."** Each is a point on a map
of options, with honest costs marked. The choice of point depends on:
- ZER's committed audience
- Acceptable runtime cost
- Acceptable annotation cost
- Acceptable pointer size
- Acceptable language complexity
- Acceptable migration cost

Several designs are viable. None is universally best.

**For future sessions:** if proposing a new universal pointer design,
verify against this document that:
1. The design isn't a rename of an existing probe
2. The design's tradeoffs are honestly assessed
3. The design respects the conservation law
4. The design's audience fit is clear

This document is the canonical context for ZER's universal pointer design
exploration as of 2026-05-25.

---

## Document History

| Date | Change |
|---|---|
| 2026-05-25 | Initial creation — comprehensive context dump from brainstorming conversation |
| 2026-05-25 | Expansion — added Sections 16-25 with concrete code examples, verification work, detailed cost analyses, FAQ, and migration stories |
| 2026-06-01 | PART 3 (auto-detection) + PART 4 (reconciliation) appended |
| 2026-06-07 | PART 5 — DECISION: compile-time-only `keep`, no runtime tag check; supersedes PART 3's lock. Escape-matrix oracle built + 4 escape holes (H1-H4) fixed; foundation verified sound (20/20) |

---

# PART 2: EXPANDED CONTEXT (added for completeness)

The original document captured the design space at high level. This expansion
adds the concrete details, code examples, verification work, and Q&A that
emerged during the conversation. Read this part for implementation-level
context.

---

## 16. Concrete Code Examples per Design

### 16.1 The reference example used throughout the conversation

The user kept returning to this Task/TaskQueue example:

```
struct Task {
    u32 task_id;
    [*]u8 name;
    u32 priority;
    ?*Task next;
}

struct TaskQueue {
    u32 num_lanes;
    [*]?*Task lanes;
}

Slab(Task) heap;

u32 main() {
    Handle(Task) t = heap.alloc() orelse { return 0; };
    heap.get(t).task_id = 1;
    heap.get(t).priority = 0;
    heap.free(t);
    return 0;
}
```

This example exercises: struct definition, optional pointer (`?*Task`),
slice of optional pointers (`[*]?*Task`), Slab allocator, Handle, auto-deref
via `heap.get(t).field`, orelse unwrap, explicit free.

Let me show what this looks like under each design we explored.

### 16.2 Current ZER (baseline)

```
struct Task {
    u32 task_id;
    [*]u8 name;
    u32 priority;
    ?*Task next;          // ?*T = nullable pointer, 8 bytes (null sentinel)
}

struct TaskQueue {
    u32 num_lanes;
    [*]?*Task lanes;      // slice of nullable pointers
}

Slab(Task) heap;           // global allocator

u32 main() {
    Handle(Task) t = heap.alloc() orelse { return 0; };  // 8-byte handle
    heap.get(t).task_id = 1;   // auto-deref + gen check
    heap.get(t).priority = 0;
    heap.free(t);              // gen invalidated
    return 0;
}
```

**Compiled C (simplified):**
```c
// auto-Slab created at TaskQueue declaration site
static _zer_slab _zer_auto_slab_Task = { .slot_size = sizeof(struct Task) };

uint32_t main() {
    uint8_t ok;
    uint64_t t_handle = _zer_slab_alloc(&heap, &ok);
    if (!ok) return 0;
    
    // each .field auto-deref:
    ((struct Task*)_zer_slab_get(&heap, t_handle))->task_id = 1;
    //   _zer_slab_get checks: idx valid? used[idx]? gen[idx] == handle.gen?
    //   if any fail → _zer_trap("use-after-free")
    
    ((struct Task*)_zer_slab_get(&heap, t_handle))->priority = 0;
    
    _zer_slab_free(&heap, t_handle);
    return 0;
}
```

**Cost profile:**
- Pointers: 8 bytes (Handle)
- Per-deref runtime cost: gen check ~3-10 cycles (Handle infrastructure)
- Compile-time elision: limited (each `heap.get(t)` re-evaluates)
- Per-allocation overhead: existing Slab metadata (gen array)
- Coverage: ~95% of patterns work via Pool/Slab/Arena

### 16.3 Variant A — Pure universal safe_ptr (16 bytes)

```
struct Task {
    u32 task_id;
    [*]u8 name;
    u32 priority;
    weak_ref<Task> next;        // weak — breaks any cycle
}

// Universal heap allocator (returns safe_ptr<T>):
u32 main() {
    safe_ptr<Task> t = heap.alloc(Task) orelse { return 0; };
    t.task_id = 1;                // direct deref, shadow tag checked
    t.priority = 0;
    heap.free(t);                  // tag invalidated
    // t.task_id = 99;             // would trap: UAF caught at runtime
    return 0;
}
```

**Cost profile:**
- Pointers: 16 bytes (safe_ptr = {addr, tag})
- Per-deref runtime cost: 3-5% per access (shadow tag check)
- Compile-time elision: aggressive (most derefs proven safe)
- Per-allocation overhead: shadow region (~12.5% of heap for full tags)
- Coverage: universal — write any pattern

### 16.4 Variant B — Mixed (Handle for hot path, safe_ptr for traveling)

```
struct Task {
    u32 task_id;
    [*]u8 name;
    u32 priority;
    ?Handle(Task) next;            // Handle inside Slab — zero overhead
}

struct TaskQueue {
    u32 num_lanes;
    safe_slice<weak_ref<Task>> lanes;   // queue uses universal — shared/escaping
}

Slab(Task) heap;                    // hot Task allocation stays fast

u32 main() {
    Handle(Task) t = heap.alloc() orelse { return 0; };
    heap.get(t).task_id = 1;        // 0% overhead — same as before
    heap.get(t).priority = 0;
    heap.free(t);
    return 0;
}
```

**Cost profile:**
- Pointers: 8B for Handle field, 16B for safe_slice elements
- Per-deref runtime cost: 0% on hot path, 3-5% on queue accesses
- Compile-time elision: same as current
- Per-allocation overhead: Slab metadata + small shadow for safe_slice elements
- Coverage: universal (Handle for in-scope, safe_ptr for portable)

### 16.5 Variant C — keep-design (compile-time, 8 bytes, single annotation)

```
struct Task {
    u32 task_id;
    [*]u8 name;
    u32 priority;
    keep ?*Task next;              // keep — field stores pointer, can outlive scope
}

struct TaskQueue {
    u32 num_lanes;
    keep [*]?*Task lanes;          // keep — slice elements are stored pointers
}

Slab(Task) heap;                    // existing allocator, returns Handle or *T

u32 main() {
    Handle(Task) t = heap.alloc() orelse { return 0; };
    heap.get(t).task_id = 1;
    heap.get(t).priority = 0;
    heap.free(t);
    return 0;
}

// Function storing a pointer would require keep:
fn store_for_later(keep *Task t) {       // keep on parameter
    global_task_queue.tasks[0] = t;       // compiler verifies: t lives long enough
}
```

**Cost profile:**
- Pointers: 8 bytes always
- Per-deref runtime cost: 0% (all compile-time)
- Compile-time elision: by construction (no runtime check exists)
- Per-allocation overhead: 0
- Coverage: universal on lifetime axis, 100% compile-time

### 16.6 Variant D — Tagged pointer + allocation header (8-byte hybrid)

```
struct Task {
    u32 task_id;
    [*]u8 name;
    u32 priority;
    ?*Task next;
}

struct TaskQueue {
    u32 num_lanes;
    [*]?*Task lanes;
}

allocator heap;     // user-extensible allocator returning *T with header

u32 main() {
    *Task t = heap.alloc(Task) orelse { return 0; };
    t.task_id = 1;       // compiler emits: header check + raw access
    t.priority = 0;
    heap.free(t);         // header version invalidated
    return 0;
}
```

**Compiled C (simplified):**
```c
struct _zer_alloc_header { uint64_t version; };

uint32_t main() {
    struct _zer_alloc_header *hdr = heap_alloc(sizeof(struct Task));
    if (!hdr) return 0;
    struct Task *t_raw = (struct Task *)(hdr + 1);
    uint64_t t_version = hdr->version;
    
    // Tagged pointer = (version << 48) | (uintptr_t)t_raw
    // Stored compactly in 8 bytes
    
    // Each deref:
    if (((struct _zer_alloc_header*)t_raw - 1)->version != t_version)
        _zer_trap("UAF");
    t_raw->task_id = 1;
    
    if (((struct _zer_alloc_header*)t_raw - 1)->version != t_version)
        _zer_trap("UAF");
    t_raw->priority = 0;
    
    ((struct _zer_alloc_header*)t_raw - 1)->version = 0;  // invalidate
    heap_free(hdr);
    return 0;
}
```

**Cost profile:**
- Pointers: 8 bytes (tagged in top bits)
- Per-deref runtime cost: 5-30 cycles per non-elided check
- Compile-time elision: aggressive (70-85% typical)
- Per-allocation overhead: 8 bytes (header)
- Coverage: universal

### 16.7 Variant E — 4-rule strict borrow checker

```
struct Task {
    u32 task_id;
    [*]u8 name;
    u32 priority;
    *Task next;             // compile error candidate — depends on Node lifetime context
}

// In pure 4-rule mode, the linked list pattern needs Slab to provide
// the lifetime context. Without Slab, `*Task next` can't be proven safe.

Slab(Task) heap;

u32 main() {
    *Task t = heap.alloc_ptr(Task) orelse { return 0; };
    t.task_id = 1;          // OK — t alive on this path
    t.priority = 0;
    heap.free_ptr(t);
    // t.task_id = 99;       // COMPILE ERROR: use after free
    return 0;
}

// Functions storing pointers fail without Slab context:
fn store_globally(p: *Task) {
    global_cache = p;        // COMPILE ERROR:
                             //   "storing local pointer in global"
                             //   "fix: use Pool/Slab/Arena to allocate, or use Handle"
}
```

**Cost profile:**
- Pointers: 8 bytes
- Per-deref runtime cost: 0%
- Compile-time coverage: ~80% (linear and provable cases)
- ~5% of patterns inexpressible (compile error, no fix in pure *T)
- Coverage: 80% with no-brainer fixes, 15% restructure, 5% blocked

### 16.8 Comparison: same Task example, all designs

```
Aspect                   | Current ZER  | safe_ptr 16B | keep         | tagged+header
─────────────────────────────────────────────────────────────────────────────────
Pointer size             | 8B (Handle)   | 16B          | 8B           | 8B
Per-deref cost           | gen check     | tag check    | 0            | header check
Allocator                | Slab (built)  | user heap    | Slab (built) | user heap
Annotation on field      | none          | weak_ref     | keep         | none
Cycles allowed           | yes (handles) | yes (weak)   | yes (keep)   | yes (header)
Custom allocator support | no            | yes          | yes          | yes
Embedded RAM friendly    | yes (8B)      | no (16B)     | yes (8B)     | yes (8B)
Runtime cost (typical)   | minimal       | 1-2%         | 0%           | 1-5%
Lines of source code     | shortest      | similar      | + keep words | similar
```

---

## 17. Code Verification — What ZER Actually Implements Today

I verified the following facts by reading the actual ZER source code during
the conversation. These are the ground truth for what current ZER catches
and how.

### 17.1 type_equals strict kind check — types.c:224-307

The fundamental enforcement that `*T` and `[*]T` are distinct types:

```c
// types.c:224
bool type_equals(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;       // ← LINE 227
                                                  // TYPE_POINTER ≠ TYPE_SLICE
    
    switch (a->kind) {
    case TYPE_POINTER:
        if (a->pointer.is_const != b->pointer.is_const) return false;
        return type_equals(a->pointer.inner, b->pointer.inner);
    case TYPE_SLICE:
        if (a->slice.is_const != b->slice.is_const) return false;
        if (a->slice.is_volatile != b->slice.is_volatile) return false;
        return type_equals(a->slice.inner, b->slice.inner);
    // ...
    }
}
```

**Implication:** Passing a `*T` where `[*]T` is expected (or vice versa)
is a compile error at the type system level. No silent conversion possible.

### 17.2 Pointer indexing warning — checker.c:5277-5340

The current behavior when user indexes `*T`:

```c
// checker.c:5277
} else if (obj->kind == TYPE_POINTER) {
    // ... checks for MMIO bound, VRP, etc. ...
    
    if (!ptr_proven && !obj->pointer.is_volatile) {
        checker_warning(c, node->loc.line,         // ← LINE 5335 — WARNING, not error
            "pointer indexing has no bounds check — "
            "use []%s (slice) for bounds-checked access",
            type_name(obj->pointer.inner));
    }
    result = obj->pointer.inner;
}
```

**Implication:** Today this is a WARNING. The proposed design (Section 6.10
and Sections 19-23) promotes this to a compile ERROR with a clear fix
directive ("use [*]T instead").

### 17.3 Array bounds — checker.c:5209-5265

The 4-tier bounds checking on arrays:

```c
if (obj->kind == TYPE_ARRAY) {
    // Tier 1: compile-time error for constant OOB
    if (node->index_expr.index->kind == NODE_INT_LIT) {
        uint64_t idx_val = node->index_expr.index->int_lit.value;
        if (idx_val >= obj->array.size) {
            checker_error(c, node->loc.line,
                "array index %llu is out of bounds for array of size %llu",
                idx_val, obj->array.size);
        }
        if (idx_val < obj->array.size) {
            mark_proven(c, node);  // ← Tier 2: proven safe, elide check
        }
    }
    
    // Tier 3: range propagation
    if (node->index_expr.index->kind == NODE_IDENT) {
        struct VarRange *r = find_var_range(c, ...);
        if (r && r->min_val >= 0 && r->max_val >= 0 &&
            (uint64_t)r->max_val < obj->array.size) {
            mark_proven(c, node);
        }
        
        // Tier 4: auto-guard with helpful warning
        if (!checker_is_proven(c, node)) {
            mark_auto_guard(c, node, obj->array.size);
            checker_warning(c, node->loc.line,
                "index '%.*s' not proven in range for array of size %llu — "
                "auto-guard inserted. Add 'if (%.*s >= %llu) { return; }' to eliminate guard",
                ...);
        }
    }
    
    // Tier 5: cross-function range from find_return_range
    if (!checker_is_proven(c, node) &&
        node->index_expr.index->kind == NODE_CALL) {
        // ...check callee's return range, mark proven if in bounds
    }
}
```

**Implication:** ZER's bounds checking on arrays is sophisticated. 4 tiers
covering compile-time constants, VRP-proven ranges, auto-guard with helpful
warnings, and cross-function range propagation. The pattern works well and
should be the template for similar safety mechanisms.

### 17.4 Slab UAF detection — emitter.c:4841-4859

The runtime gen check on slab access:

```c
// _zer_slab_get inline helper:
emit(e, "static inline void *_zer_slab_get(_zer_slab *s, uint64_t handle) {\n");
emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
emit(e, "    uint32_t gen = (uint32_t)(handle >> 32);\n");
emit(e, "    if (idx >= s->total_slots || !s->used[idx] || s->gen[idx] != gen) {\n");
emit(e, "        _zer_trap(\"slab: use-after-free or invalid handle\", __FILE__, __LINE__);\n");
emit(e, "    }\n");
// ...
}

// _zer_slab_free:
emit(e, "static inline void _zer_slab_free(_zer_slab *s, uint64_t handle) {\n");
emit(e, "    uint32_t idx = (uint32_t)(handle & 0xFFFFFFFF);\n");
emit(e, "    if (idx < s->total_slots) {\n");
emit(e, "        s->used[idx] = 0;\n");
emit(e, "        s->gen[idx]++;\n");  // gen bumped, old handles invalidated
emit(e, "        if (s->gen[idx] == 0) s->gen[idx] = 1; /* skip 0: reserved */\n");
```

**Implication:** This IS the shadow-tag pattern, just stored in the slab's
gen array instead of separate shadow memory. Same idea: tag at allocation,
check at use, invalidate at free. The proposed safe_ptr / tagged+header
designs generalize this pattern.

### 17.5 Bounds check helper — emitter.c:4734-4737

The runtime bounds check used by `[*]T`:

```c
emit(e, "static inline void _zer_bounds_check(size_t idx, size_t len, "
        "const char *file, int line) {\n");
emit(e, "    if (idx >= len) _zer_trap(\"array index out of bounds\", file, line);\n");
emit(e, "}\n");
```

**Implication:** Spatial bounds for slices is already mechanically enforced.
Compile-time elision via VRP for ~70-80% of cases. Runtime check for the
rest. This pattern composes cleanly with any pointer-lifetime mechanism.

### 17.6 Auto-guard emission — emitter.c:286-346

When VRP can't prove safety, compiler inserts the guard automatically:

```c
static void emit_auto_guards(Emitter *e, Node *node) {
    if (!node) return;
    switch (node->kind) {
    case NODE_INDEX: {
        uint64_t ag_size = checker_auto_guard_size(e->checker, node);
        if (ag_size > 0) {
            emit_indent(e);
            emit(e, "if ((size_t)(");
            emit_expr(e, node->index_expr.index);
            emit(e, ") >= %lluu) ", (unsigned long long)ag_size);
            if (e->current_func_ret && e->current_func_ret->kind != TYPE_VOID) {
                emit(e, "{\n");
                emit_defers(e);
                emit(e, "return ");
                emit_zero_value(e, e->current_func_ret);
                emit(e, "; }\n");
            } else {
                emit(e, "{\n");
                emit_defers(e);
                emit(e, "return; }\n");
            }
        }
        // ... recurse into object and index ...
    }
    // ... other cases
    }
}
```

**Implication:** ZER's compiler can INSERT safety checks at compile time
where it can't prove safety statically. The pattern emits a clean early-
return with proper cleanup (defers fire). This pattern could be reused for
pointer-lifetime checks in any of the proposed designs.

### 17.7 Coercion rules — types.c:326-395

The strict implicit coercion rules:

```c
bool can_implicit_coerce(Type *from, Type *to) {
    if (type_equals(from, to)) return true;
    
    // integer widening (delegates to VST-verified predicates)
    if (type_is_integer(from) && type_is_integer(to)) { /* widening */ }
    
    // T → ?T: value to optional (implicit wrap)
    Type *to_eff = type_unwrap_distinct(to);
    if (to_eff->kind == TYPE_OPTIONAL) {
        return type_equals(from, to_eff->optional.inner) ||
               can_implicit_coerce(from, to_eff->optional.inner);
    }
    
    // array to slice: T[N] → []T
    if (from->kind == TYPE_ARRAY && to->kind == TYPE_SLICE) {
        return type_equals(from->array.inner, to->slice.inner);
    }
    
    // slice qualifier widening: mutable → const, etc.
    if (from->kind == TYPE_SLICE && to->kind == TYPE_SLICE) { /* qualifiers */ }
    
    // mutable pointer to const pointer
    if (from->kind == TYPE_POINTER && to->kind == TYPE_POINTER) {
        if (to->pointer.is_const && !from->pointer.is_const) {
            return type_equals(from->pointer.inner, to->pointer.inner);
        }
    }
    
    // NOTE: slice to pointer ([]T → *T) was REMOVED per BUG-162
    //       (empty slice has ptr=NULL, violates *T non-null guarantee)
    
    return false;
}
```

**Implication:** No silent `*T ↔ [*]T` conversion. Array auto-decays to
slice (one direction). `*T` widens to `?*T` (optional wrap). Const widening
allowed. No other implicit conversions. This is enforced AT THE TYPE LEVEL,
not by warnings.

---

## 18. Shadow Tag Width — Detailed Analysis

The user pushed back on probabilistic UAF false-negatives in shadow tag
designs. Here's the detailed analysis of tag-width options.

### 18.1 The fundamental tradeoff

```
Tag width        Storage in pointer     Shadow memory per granule    False-negative rate
─────────────────────────────────────────────────────────────────────────────────────
4-bit (ARM MTE)   top 4 bits (free)     0.5 bytes per 16B granule     1/16 (~6%)
8-bit (scudo)     top byte (free)       1 byte per 16B granule         1/256 (~0.4%)
16-bit            top 16 bits (free)    2 bytes per 16B granule        1/65,536 (~0.0015%)
32-bit            fat pointer (12B)     4 bytes per 16B granule        1/4 billion
64-bit monotonic  fat pointer (16B)     8 bytes per 16B granule        zero (with no wrap)
```

### 18.2 Why top bits are "free"

x86_64 and ARM64 use 48-bit canonical addresses. The top 16 bits are
sign-extension on x86 and ignored on ARM64 (TBI feature). Storing a tag in
those bits costs zero additional pointer storage.

For 4/8/16-bit tags, the pointer stays 8 bytes total. For 32-bit and 64-bit
tags, the pointer becomes a fat pointer (12 or 16 bytes).

### 18.3 Why ARM MTE chose 4 bits

ARM's hardware MTE stores tags in MEMORY too (not just in pointers). Every
16-byte granule of physical memory has 4 bits of associated tag storage.
4 bits chosen because:

- 4 bits per 16-byte granule = 3.1% memory overhead (acceptable)
- 8 bits per 16-byte granule = 6.3% (getting expensive on mobile)
- 16 bits per 16-byte granule = 12.5% (too much for mobile)

ARM optimized for mobile memory footprint. Their 4-bit choice means 1/16
false-negative rate, which is bad for security but defensible for crash
debugging.

For ZER software shadow tag, you don't have ARM's hardware constraint.
The shadow region lives in regular DRAM that you allocate freely. **16-bit
is the right software default.**

### 18.4 Practical attack scenarios

```
Tag width    Attempts to land UAF    Time at 1 attempt/sec    Time at 1000 attempts/sec
────────────────────────────────────────────────────────────────────────────────────
8-bit         ~256                    ~4 minutes               ~0.25 seconds
16-bit        ~65K                    ~18 hours                ~65 seconds
32-bit        ~4 billion              ~130 years               ~50 days
64-bit        ~10^19                  ~580 billion years        ~580 million years
```

For long-running adversarial systems (web servers, kernel boundaries):
- 8-bit: REAL attack window
- 16-bit: marginal, requires sustained attack
- 32-bit: practically unattackable
- 64-bit: theoretically unattackable

### 18.5 Monotonic counter wrap analysis (corrected from earlier overclaim)

Initial claim was: "64-bit monotonic counter = effectively infinite, no wrap
in age of universe."

**Actual math (user corrected me):**
- 2^64 = 1.8 × 10^19 allocations
- At 1 billion allocs/sec: 1.8 × 10^10 seconds = **570 years**, not age of universe
- Age of universe is 14 billion years = at 1B/sec would consume 4 × 10^26 allocations
- This WOULD wrap a 64-bit counter ~10^7 times

**Honest framing:**
- 64-bit counter: ~570 years at 1B allocs/sec, practically infinite for any program
- BUT not "mathematically impossible" — it would wrap if a program ran for 570+ years
- For real programs: effectively zero false-negative rate

### 18.6 Per-thread counter sharding

To avoid global atomic_fetch_add contention:

```
Per-thread state: tls_counter = (thread_id << 56) | 0
Per-thread state: tls_max = (thread_id << 56) | (1 << 56)

alloc():
    tag = tls_counter++       // pure thread-local, no atomic
    if (tag == tls_max) panic("thread used 2^56 tags — extremely rare")
    return safe_ptr<T>{addr, tag}
```

- Top 8 bits = thread ID (256 threads max — fine for most systems)
- Bottom 56 bits = per-thread monotonic counter
- All tags globally unique (different threads have different top bits)
- No cross-thread atomic at alloc time

For systems with >256 threads (servers): 16-bit thread ID + 48-bit counter.

**Wrap analysis per-thread:**
- 2^56 = 7.2 × 10^16 allocations per thread
- At 1M allocs/sec per thread: ~2284 years before wrap
- Effectively infinite

**Thread ID recycling concern:**
- If thread dies and new thread gets same ID, they share tag prefix
- Need monotonic thread ID allocation (never reuse), OR
- Persist counter across thread lifetimes

### 18.7 Recommended choice for ZER

If ZER ships safe_ptr / tagged+header design:

**Default: 16-bit tag** (top 16 bits of 64-bit pointer)
- 8 bytes per pointer (free)
- ~3% memory overhead (2 bytes per 64-byte granule)
- 1/65K false-negative rate (acceptable for non-adversarial)

**Security mode: 32-bit tag** (fat pointer, 12 bytes)
- 12 bytes per pointer
- ~6% memory overhead
- 1/4B false-negative rate (effectively zero for any attack)

**Paranoid mode: 64-bit monotonic** (fat pointer, 16 bytes)
- 16 bytes per pointer
- ~12.5% memory overhead
- Mathematically zero for practical lifetimes

User picks per-project compile flag. Embedded defaults to 16-bit (RAM savings),
security-critical defaults to 32-bit, paranoid defaults to 64-bit.

---

## 19. The keep Annotation — Deep Dive

This section expands the keep design from Section 6.10 with concrete details.

### 19.1 The single rule

```
Default rule:
  *T parameter / local var / return value is NON-KEEP by default.
  Non-keep means: function-scope only, cannot be stored persistently.

Single annotation:
  `keep` on declarations means: this pointer can be stored / outlive scope.

Compiler enforcement:
  Non-keep *T cannot:
    - Be assigned to global variable
    - Be assigned to struct field marked keep
    - Be inserted into generic container that stores by reference
    - Be captured by spawned thread
    - Be captured by async closure across yield
    - Be returned in a way that escapes function scope
    
  When compiler detects above, emits ERROR with directive:
    "add `keep` to parameter/variable declaration at line N"
```

### 19.2 Where keep can appear

```
// On function parameter:
fn store_ptr(keep p: *Task) { global = p; }

// On local variable:
keep *Task t = some_source();    // t can be stored persistently
some_struct.field = t;             // OK

// On struct field:
struct Cache {
    keep *Task hot;                // field holds pointer that outlives Cache
}

// On return type (implicit — callers determine scope):
fn pick(keep a: *Task, keep b: *Task) -> *Task {
    return a;    // result tied to caller's keep-pointers
}

// On global (implicit — globals always keep slots):
*Task global_task;    // implicit keep — any assignment requires keep source

// On struct field in slice:
struct Cache {
    keep [*]*Task tasks;    // each element is a stored pointer
}

// On function pointer type:
*(keep p: *Task) callback;    // callback param marked keep
                               // callers must pass keep pointer
```

### 19.3 Auto-detection scenarios with concrete error messages

#### Scenario 1: Pointer escapes via global assignment

```
fn store_it(p: *Task) {
    global_cache = p;
}

// Compiler error:
// error: storing non-keep pointer 'p' in global 'global_cache' at line 2
//   global_cache is a `keep` slot (globals are implicit keep)
//   but parameter 'p' is non-keep
//   
//   Fix: change parameter to `keep p: *Task`
//   (caller must guarantee p lives as long as global_cache's scope)
//   
//   Or: copy data instead of storing pointer
//        e.g., global_cache_value = *p;
```

#### Scenario 2: Pointer escapes via struct field

```
struct Cache { keep *Task hot; }
fn warm_cache(c: *Cache, t: *Task) {
    c.hot = t;
}

// Compiler error:
// error: storing non-keep pointer 't' in keep slot 'c.hot' at line 3
//   Cache.hot is a keep field
//   but parameter 't' is non-keep
//   
//   Fix: change parameter to `keep t: *Task`
//   
//   Cascading: callers of warm_cache must also pass keep pointers
```

#### Scenario 3: Pointer escapes via container insert

```
Map<u32, *Task> registry;       // generic container — V is *T
fn register(id: u32, t: *Task) {
    registry.insert(id, t);
}

// Compiler error:
// error: inserting non-keep pointer 't' into container 'registry' at line 3
//   Map<u32, *Task>::insert requires keep on V parameter
//   (container stores values persistently)
//   
//   Fix: change parameter to `keep t: *Task`
//   
//   Or: change container type to Map<u32, Task> (stores by value)
//   
//   Or: change container type to Map<u32, Handle(Task)> (handle is index)
```

#### Scenario 4: Pointer escapes via spawn

```
fn worker(t: *Task) { ... }
fn main() {
    *Task t = heap.alloc_ptr(Task) orelse return;
    spawn worker(t);
}

// Compiler error:
// error: passing non-keep pointer 't' to spawn target at line 4
//   spawn captures arguments for cross-thread use
//   *Task crosses thread boundary, requires keep or shared
//   
//   Fix options:
//     1. change worker signature: `fn worker(keep t: *Task)` + ensure t lives long enough
//     2. change pointer type to shared: `shared *Task t`
//     3. use Handle instead of *T: `Handle(Task) t = heap.alloc()`
//     4. use scoped spawn: `ThreadHandle h = spawn worker(t); h.join();`
//        (compiler can prove t alive for join duration)
```

#### Scenario 5: Pointer escapes via async capture

```
async fn process(t: *Task) {
    yield;
    t.field = 5;  // ← would use t after potential allocator scope end
}

// Compiler error:
// error: capturing non-keep pointer 't' across yield at line 2
//   async function suspends at yield; t may not be valid on resume
//   
//   Fix: change parameter to `keep t: *Task`
//        Caller of async must guarantee t lives across all yields
//   
//   Or: copy data before yield, use copy after
//        e.g., u32 data = t.field; yield; use(data);
```

#### Scenario 6: Cascading propagation up call stack

```
fn outer(p: *Task) {
    inner(p);
}
fn inner(p: *Task) {
    global = p;
}

// First compile error (on inner):
// error: storing non-keep pointer 'p' in global at line 5
//   Fix: change inner's parameter to `keep p: *Task`

// User adds keep to inner:
fn inner(keep p: *Task) { global = p; }    // ← keep added

// Now recompile, error on outer:
// error: passing non-keep pointer 'p' to inner expecting keep at line 2
//   inner requires keep parameter (propagation from line 5)
//   
//   Fix: change outer's parameter to `keep p: *Task`

// User adds keep to outer:
fn outer(keep p: *Task) { inner(p); }    // ← keep added

// Now check all callers of outer; they must also be keep
```

The cascade terminates at:
- A function whose callers all pass `static`-derived pointers
- A function called with pointer from explicit `'static` allocator
- The compiler can't prove safety → user must restructure

### 19.4 Bidirectional flow — unnecessary keep warnings

The compiler also detects when `keep` was added but isn't needed:

```
fn unused_keep(keep p: *Task) {
    p.field = 5;        // never stores, never escapes
}

// Compiler warning (not error):
// warning: `keep` annotation on parameter 'p' at line 1 is unnecessary
//   p is never stored, returned, or captured
//   Remove `keep` to allow more flexible caller use
//   (callers won't be required to pass keep pointers)
```

User cleans up; or leaves it for forward compatibility.

### 19.5 Boundary declarations (where compiler can't auto-detect)

Three places require explicit keep declaration once:

#### Function pointer types

```
typedef KeepCallback = *(keep p: *Task);     // function pointer requires keep on param

// Usage:
KeepCallback cb = some_function;
fn install(c: KeepCallback) { ... }

// Compiler verifies any function assigned to KeepCallback has keep parameter
```

#### cinclude C function bindings

```
cinclude "mylib.h"

// User declares the ZER binding with keep where C function stores the pointer:
extern fn mylib_register_callback(keep ctx: *opaque, cb: *(keep *opaque) -> void) -> i32;

// Now ZER tracks: any *opaque passed to mylib_register_callback must be keep
```

#### Generic constraints (when applicable)

```
container Map(K, V) {
    K[64] keys;
    keep V[64] values;    // if V is a pointer type, values are keep slots
}

// Or with explicit constraint:
container Map(K, V) where keep V {
    ...
}
```

These are one-time declarations at boundary, not per-call-site.

### 19.6 Cascade termination

The compiler chases keep requirements up the call stack until reaching one of:

```
1. Allocator boundary:
   *Task t = static_pool.alloc();  // static_pool is 'static lifetime
                                     // t is automatically allowed to be keep
   global = t;                       // OK

2. 'static-derived source:
   const static *Task t = &builtin_task;  // static address
   global = t;                              // OK

3. Slab/Pool with global scope:
   Slab(Task) heap;  // global slab — 'static lifetime
   Handle(Task) h = heap.alloc() orelse return;
   global_handle = h;  // OK — Handle is allowed to be stored anywhere

4. Compile error (no valid termination):
   {
       Slab(Task) local_slab;  // function-scope slab
       *Task t = local_slab.alloc_ptr() orelse return;
       global = t;
       // ERROR: t derived from local_slab which doesn't outlive global
       //        Fix: declare local_slab at module scope
       //        Or: copy data before storing
   }
```

### 19.7 Real-world annotation density estimate

For a typical ZER codebase:

```
Function category               | Frequency  | keep usage
─────────────────────────────────────────────────────────────────────────
Pure computation (no pointers)  | ~30%       | 0 (no pointers)
Local pointer use (no escape)   | ~50%       | 0 (default)
Single-keep escape              | ~15%       | 1 keep on one param
Multi-keep escape               | ~4%        | 2-3 keep annotations
Complex multi-storage           | ~1%        | restructuring may be required
```

For a 10,000-line codebase:
- Expected total keep annotations: ~500-1500
- Each annotation triggered by compiler error (one-word fix)
- No deep reasoning required

Compare to Rust:
- 10,000-line codebase: ~2000-5000 lifetime annotations + Send/Sync + variance
- Plus restructuring of borrow-checker-rejected patterns
- Plus 3-6 months learning curve

### 19.8 Why keep doesn't catch aliasing violations

```
fn use_after_alias(v: *Vec) {
    let r = &v[0];
    v.push(some_value);    // could realloc, invalidating r
    use(r);                 // ← Rust catches at compile, keep doesn't
}
```

Aliasing violations (Rust's `&mut` exclusivity) are NOT a pointer-lifetime
issue. They're a separate concern about mutation rules. keep doesn't model
this; ZER doesn't catch this class of bug.

For practical purposes:
- Iterator invalidation: covered by ZER's range-for snapshot pattern
- Concurrent modification: covered by `shared` annotation
- Direct alias-with-mutation: NOT caught (real gap vs Rust)

This is the honest gap in the keep design vs Rust. For ZER's audience
(embedded/firmware/kernel), this gap is bounded — most code doesn't have
this pattern. For libraries doing complex data structure manipulation,
it's a real loss.

---

## 20. The 4-Rule Strict Borrow Checker — Deep Dive

This section expands Section 6.9 with detailed rule mechanics.

### 20.1 The four rules in detail

#### Rule 1: Every *T has an allocation source

```
Every *T must be derivable to one of:
  - A specific allocator instance (Pool/Slab/Arena/user-impl)
  - A static / global / constant address
  - A parameter received from a function (whose lifetime is caller's responsibility)
  - A field of a tracked struct (whose lifetime is struct's responsibility)

If source can't be determined:
  → compile error: "pointer source unknowable, use Handle or [*]T"
```

#### Rule 2: *T cannot outlive its allocation source

```
At every use of *T, compiler verifies:
  - source is alive on this path (not freed)
  - source is reachable from this scope

Implementation: same as ZER's existing zercheck path-sensitive analysis
  - HS_ALIVE → HS_FREED transition on free()
  - HS_MAYBE_FREED if freed on some paths
  - Compile error on use when MAYBE_FREED or FREED

Already implemented for Handle. Extend to all *T.
```

#### Rule 3: *T returned from function must trace to param or 'static

```
fn make() -> *Task {
    Task local;
    return &local;        // ERROR: returning pointer to local
}

fn select(a: *Task, b: *Task) -> *Task {
    return cond ? a : b;  // OK: returns one of params
}

fn from_static() -> *Task {
    return &static_task;  // OK: 'static lifetime
}

Implementation: escape analysis
  - Check return expression
  - Walk through any aliasing to find ultimate source
  - Error if source is local
```

#### Rule 4: *T stored in struct/global must have lifetime ≥ container's

```
struct Cache { *Task hot; }

global_cache: *Cache = ...;

fn warm(local_task: Task) {
    global_cache.hot = &local_task;  // ERROR: &local_task lifetime < global_cache lifetime
}

Implementation: lifetime inference
  - Determine container's lifetime (caller-determined or 'static)
  - Determine pointer source's lifetime
  - Error if pointer source < container
```

### 20.2 Concrete failure scenarios with no clean fix

#### Generic container holding *T

```
fn store_pointers(m: *Map<u32, *Task>, t: *Task) {
    m.insert(1, t);
}

Error: storing *T in generic container — lifetime relationship cannot be inferred
   The Map<u32, *Task> may outlive 't'. Without Pool/Slab/Handle, this is unsafe.
   
   No safe transformation exists in pure *T mode. Options:
     1. Change V to Task (store by value): Map<u32, Task>
     2. Restructure to not require shared pointer storage
     3. (excluded by pure *T mode) Use Handle(Task) with Slab-based map
```

In pure *T mode with the 4-rule checker, the user must:
- Accept "this pattern can't be written safely"
- Restructure (often architectural change, not 1-line)
- Use an excluded mechanism (Pool/Slab/Handle/safe_ptr)

This is the ~5% inexpressibility gap.

#### Linked list `*Node next`

```
struct Node {
    *Node next;
    u32 val;
}

Error: Node.next: *T field in struct that doesn't have allocator context
   Cannot prove Node.next lifetime relative to other Nodes
   
   Fix options:
     1. Use Slab(Node) and Handle(Node) next instead
     2. Use Arena and ensure all nodes in same arena scope
```

Linked lists in pure *T mode without allocator scope cannot prove lifetime.
User must adopt allocator pattern.

#### Self-referential structures

```
struct TreeNode {
    *TreeNode parent;
    *TreeNode left;
    *TreeNode right;
    u32 val;
}

Error: cyclic pointer references in TreeNode
   parent ↔ left/right relationships cannot be statically proven safe
   
   Fix: use Handle(TreeNode) with Slab-based tree (indices avoid cycles)
```

Self-referential = forces indices into a Slab. Pure *T can't handle.

### 20.3 The 80/15/5 breakdown

```
~80% of code: works with no-brainer fixes
   - "fix: take an output param instead of returning local pointer"
   - "fix: assign through caller-provided struct"
   - "fix: add allocator scope" (trivial)
   
~15% of code: works after moderate restructuring
   - "fix: refactor to use slice with explicit length"
   - "fix: copy data instead of storing pointer"
   - "fix: use Pool/Slab if you need indexed access" (medium change)

~5% of code: simply not expressible in pure *T mode
   - Generic containers holding *T
   - Async captures of *T
   - Self-referential structures via *T
   - Complex multi-pointer data structures
   - Cross-module pointer queues
```

For the 5%, the user has two choices:
- Restructure (often architectural rewrite)
- Use an "excluded" mechanism (Pool/Slab/Handle/safe_ptr)

### 20.4 Why this design has a place

Despite the 5% inexpressibility, the 4-rule design is:
- Strictly zero runtime cost
- Strictly zero annotation cost
- Catches 80% of pointer bugs at compile time
- Forces good practice (use Pool/Slab/Arena explicitly)

For users who can accept the restructuring cost on the 5%, this is the
cleanest design. For users who can't, the keep design (Section 19) adds
one annotation per escape site at the cost of compile-time-only.

### 20.5 The user's "no brainer if compiler picks 1 right path" framing

User argued: even if a scenario has "10 wrong options and 1 right one,"
if the compiler narrows it down via error messages, the user ends up at
the right answer — that's still "no brain."

**Honest verification:**
- For 80% of cases: 1-right-option exists, compiler clearly names it
- For 15% of cases: requires moderate restructuring, compiler suggests
- For 5% of cases: NO right option in pure *T mode

The "no brainer" property holds for 80-95%. The remaining 5% is a real
gap that no compiler error can fix — the pattern is genuinely unexpressible.

For the user to accept this design: they must accept "5% of patterns are
genuinely inexpressible without restructuring or using Pool/Slab/Handle."

---

## 21. Hardware MCU RAM Reality Check

User pushed back on "embedded means kilobytes" assumption. Detailed
breakdown of modern embedded MCU RAM:

### 21.1 The full MCU landscape

```
Class             | Typical RAM   | Common applications              | Year prominent
─────────────────────────────────────────────────────────────────────────────
8-bit (AVR, PIC)  | 1-8KB         | Sensors, simple controllers      | 1990s-present
Cortex-M0/M0+     | 8-32KB        | Cost-sensitive IoT, sensors      | 2010s-present
Cortex-M3         | 64-128KB      | Industrial, mid-range            | 2010s-present
Cortex-M4         | 64-256KB      | DSP-capable, IoT, motor control  | 2010s-present
Cortex-M7         | 256KB-1MB     | High-end MCU applications        | 2015+
Cortex-M33/M55    | 256KB-2MB     | TrustZone, AI inference          | 2018+
ESP32 variants    | 320KB-8MB     | WiFi/BT IoT, audio, video        | 2016+
RP2040 (Pi Pico)  | 264KB         | Maker, modern embedded           | 2021+
Cortex-A (basic)  | 256MB-1GB     | Single-board computers           | 2010s-present
Automotive ECUs   | 4-16MB        | Aurix, R-Car                     | 2015+
Industrial PLCs   | 1-32MB        | Modern PLCs                      | 2015+
Embedded Linux    | 256MB-16GB    | Edge computing, gateways         | 2010s-present
```

### 21.2 What this means for pointer size

For each class, the cost of 8 → 16 byte pointers (Always-fat Handle scenario):

```
Class             | RAM   | Typical Handle field count | 8B → 16B cost      | Acceptable?
─────────────────────────────────────────────────────────────────────────────────────
8-bit             | 4KB   | rarely uses pointers        | N/A                 | N/A
Cortex-M0         | 16KB  | ~50 Handle fields          | +400B (2.4% of RAM) | Marginal
Cortex-M4         | 128KB | ~500 Handle fields         | +4KB (3.1%)         | Acceptable
Cortex-M7         | 512KB | ~2000 Handle fields        | +16KB (3.1%)        | Easy
ESP32             | 4MB   | ~5000 Handle fields        | +40KB (1.0%)        | Trivial
Pi Pico           | 264KB | ~1000 Handle fields        | +8KB (3.0%)         | Acceptable
Automotive ECU    | 16MB  | ~10000 Handle fields       | +80KB (0.5%)        | Trivial
Embedded Linux    | 1GB+  | ~100K Handle fields        | +800KB (0.08%)      | Trivial
```

**Conclusion:** For Cortex-M3 and up, 16-byte pointers are acceptable.
For Cortex-M0 and 8-bit, every byte matters and 16-byte pointers are
marginal-to-prohibitive.

### 21.3 The audience question this raises

If ZER targets only Cortex-M0 and 8-bit class MCUs: must keep 8-byte
pointers, no fat-pointer designs viable.

If ZER targets Cortex-M3 and up: 16-byte pointers acceptable, Always-fat
Handle becomes viable.

If ZER targets both: need mode-based design or careful design selection.

The user's preference (from conversation): "even basic MCUs nowadays have
megabyte-range RAM." This suggests ZER might be willing to write off the
Cortex-M0 and 8-bit segment to gain simplicity.

### 21.4 Runtime cost on small MCUs

For runtime-checked designs (safe_ptr, tagged+header):

```
MCU             | Clock speed  | Per-deref cycles | Per-deref ns | Impact on 1M derefs
─────────────────────────────────────────────────────────────────────────────────
8-bit AVR       | 16 MHz       | ~30 cycles       | 1875 ns      | 1.875 seconds (catastrophic)
Cortex-M0       | 48 MHz       | ~20 cycles       | 417 ns       | 0.4 seconds (significant)
Cortex-M4       | 168 MHz      | ~15 cycles       | 89 ns        | 0.089 seconds (acceptable)
Cortex-M7       | 480 MHz      | ~10 cycles       | 21 ns        | 0.021 seconds (trivial)
ESP32           | 240 MHz      | ~15 cycles       | 62 ns        | 0.062 seconds (acceptable)
Pi Pico         | 133 MHz      | ~15 cycles       | 113 ns       | 0.113 seconds (acceptable)
Cortex-A53      | 1.4 GHz      | ~10 cycles       | 7 ns         | 0.007 seconds (trivial)
x86_64          | 3+ GHz       | ~10 cycles       | 3 ns         | 0.003 seconds (trivial)
```

For Cortex-M3 and up: runtime checks are acceptable.
For Cortex-M0: runtime checks cost real time but possibly tolerable.
For 8-bit: runtime checks are catastrophic.

---

## 22. The "Why Isn't Safe by Default" FAQ

User-driven questions and detailed answers from the conversation.

### 22.1 "Why isn't the default pointer just safe?"

**Honest answer:** Universal safety has nonzero cost. Cost has to go
somewhere. ZER's design philosophy is "transparent memory model" — the user
sees and controls the cost. Hiding the cost behind a safe-by-default
universal pointer would conflict with this philosophy.

**Specifically:**
- Universal compile-time safety = annotations (Rust's path)
- Universal runtime safety = per-pointer cost (safe_ptr's path)
- Universal compile-time-only = restricted patterns (4-rule's path)

ZER chose "explicit primitives with bounded safety" (Pool/Slab/Ring/Arena +
ZER-CHECK) as the default. Universal safety is opt-in via safe_ptr or
similar mechanism.

### 22.2 "Why not just use Rust's borrow checker?"

**Answer:** Cognitive load. Rust's borrow checker takes 3-6 months to
internalize. ZER's audience (C transitioners) values smooth onboarding.
ZER picks the runtime-cost direction instead of the annotation-cost
direction.

### 22.3 "Is the borrow checker even usable in Rust?"

**Honest answer:** Reaches ~85-90% usability for experts, not 100%. Real
gaps:
- Self-referential structs require Pin or arenas
- Doubly-linked lists are awkward (use indices)
- Async + lifetimes is famously hard
- HRTB error messages are incomprehensible
- Polonius project exists specifically to accept more safe programs

"Fighting the borrow checker" is a documented phenomenon. ~5% of valid
programs are rejected without unsafe or Rc<RefCell<>>.

### 22.4 "Why not just have a smart compiler that picks safe_ptr automatically?"

**Answer:** Requires whole-program analysis (which ZER explicitly bans).
Per-file analysis insufficient because pointer layout would change based
on use sites elsewhere, breaking struct layout stability across modules.

Plus: hidden cost on embedded targets (user thinks they're getting fast
Handle, compiler quietly emits slow one).

### 22.5 "Can we just use Handle for everything?"

**Answer:** Handle requires the slab to be reachable at the use site. For
pointers that travel through generic containers, async callbacks, cross-
module queues — the slab isn't reachable. Handle can't be dereferenced.

For these cases, a self-contained pointer (carries its own slab pointer or
tag) is required. Either always-fat Handle (16 bytes), or safe_ptr, or
tagged+header.

### 22.6 "What about CHERI / hardware capabilities?"

**Answer:** Requires special CPU hardware. ZER targets every arch GCC
supports, not just CHERI/Morello. Could revisit if CHERI becomes ubiquitous
(currently it's research-only in commercial hardware).

### 22.7 "Why does multi-lifetime matter? Real C code doesn't use it."

**Answer:** Multi-lifetime is mostly a Rust-imposed problem. Real C code
doesn't model lifetimes in types, so doesn't encounter the "two different
'a and 'b" pattern. ZER's keep design (single-lifetime) is sufficient for
~99% of real systems code.

The cases that need multi-lifetime:
- Long-lived caches of short-lived data (usually an architectural smell)
- Iterator merge from multiple streams (often unifiable)
- Generic library APIs (library author concern, not user)

For ZER's audience, the gap is theoretical not practical.

### 22.8 "Why not always 16-byte pointers everywhere?"

**Answer:** Real cost on embedded:
- 8 extra bytes per Handle field
- Dense data structures (kernel-style) lose 5-10% RAM
- Cache lines hold fewer pointers (less locality)

ZER's stated audience is embedded/firmware/kernel where this matters.
General-purpose users might not care; embedded users do.

### 22.9 "Can we just trap on UAF and call it safe?"

**Answer:** Trap is better than UB, but for hard-real-time systems, a
runtime trap is itself a failure mode. A flight controller can't trap.
For ZER's safety-critical target audience, compile-time guarantee is
strongly preferred over runtime trap.

For non-safety-critical: runtime trap is acceptable and is what safe_ptr /
tagged+header provide.

### 22.10 "Why can't slab-table indirection just always be 8 bytes?"

**Answer:** Variable cycle cost. The extra cache line lookup for slab_table
can spike to 50-1000+ cycles in worst case. Multi-threaded contention is
catastrophic (cache line bouncing).

For embedded predictable-latency code: unacceptable variability.
For multi-threaded servers: contention bottleneck.

The "8 bytes" benefit comes at a cost (variable cycles) that disqualifies
it for ZER's predictable-performance audience.

### 22.11 "Why is the safe_ptr framing risky?"

**Answer:** "ZER is C with shadow tags" sounds like ZER is just adding
sanitizer-style runtime checks to C. This positioning competes with:
- AddressSanitizer (debug-mode ASan)
- HardenedMalloc, scudo (production hardened allocators)
- Modern C++ with sanitizers

ZER's distinctive value is "static safety with low cognitive load." If
safe_ptr default makes runtime checks pervasive, ZER shifts to "runtime
safety like ASan but baked in" — a different and more crowded market
space.

The framing risk is about positioning, not about technical correctness.

### 22.12 "How does ZER handle the cases Rust catches but ZER doesn't?"

**Answer:** Most "Rust catches" cases are covered by other ZER mechanisms:
- Iterator invalidation → range-for snapshot pattern (per CLAUDE.md Gap 31)
- Data races → `shared` annotation + spawn analysis
- Iterator + mutation → range-for desugaring captures len at start

The cases Rust catches that ZER doesn't:
- `&mut` exclusivity within single thread (real gap)
- Some lifetime-variance edge cases (rare)
- HRTB-specific patterns (extremely rare)

For ZER's audience: acceptable trade.

---

## 23. Migration Stories

For each design, what does adoption look like?

### 23.1 Migration to keep design

**Step 1:** Update compiler to enforce keep universally (currently limited to
parameters in System #21).

**Step 2:** Recompile existing ZER codebase. Compiler emits errors at every
site that needs keep annotation.

**Step 3:** User adds `keep` keyword where compiler points (one word per site).

**Step 4:** Re-compile; errors propagate up call stack. Add more keep words.

**Step 5:** Cascade terminates at allocator boundaries. Code compiles.

**Estimated user effort for typical codebase:**
- 10K LOC: ~500-1500 keep annotations
- Each: one word, compiler-suggested
- Total time: ~2-4 hours of mechanical edits
- Verification: existing test suite

**Backward compatibility:** Existing code without keep continues to compile
unchanged unless it has actual escape violations (which are bugs anyway).

### 23.2 Migration to tagged pointer + header

**Step 1:** Update Pool/Slab/Arena to write headers on alloc.

**Step 2:** Update compiler to emit version checks before *T derefs.

**Step 3:** Update compile-time elision pass to skip checks when provable.

**Step 4:** Recompile; runtime traps appear if any UAF exists. Fix bugs.

**Estimated impact:**
- ABI break for *T (now 8 bytes with version in top, not just address)
- All allocators must be updated to write headers
- Per-allocation overhead: +8 bytes
- Whole-program runtime overhead: 1-5%
- FFI boundary requires explicit unsafe marker for incoming C pointers

**Backward compatibility:** Existing ZER code mostly works; FFI boundaries
need explicit unsafe markers.

### 23.3 Migration to safe_ptr (separate type)

**Step 1:** Add safe_ptr<T> as new pointer type.

**Step 2:** Add Allocator trait for user implementations.

**Step 3:** Add shadow memory runtime.

**Step 4:** Users opt into safe_ptr for use cases that need it.

**Estimated impact:**
- No mandatory migration; opt-in only
- Users with patterns that fit Pool/Slab/Arena continue unchanged
- Users with patterns that need safe_ptr opt in per allocation site
- Runtime cost only on safe_ptr paths (~3-5%)

**Backward compatibility:** Fully backward compatible. Additive change.

### 23.4 Migration to always-fat Handle

**Step 1:** Widen Handle layout from 8 to 16 bytes.

**Step 2:** Update auto-deref to use embedded slab pointer instead of
scope-based lookup.

**Step 3:** Recompile; all Handle field sizes double.

**Estimated impact:**
- ABI break for all Handle types
- All struct layouts with Handle fields change
- Cross-module compatibility requires re-link
- Memory overhead in dense Handle-using code
- No annotations required from user
- Universal portability gained

**Backward compatibility:** ABI break, but user-level source code unchanged.

### 23.5 Migration to 4-rule strict borrow checker

**Step 1:** Update zercheck to enforce 4 rules on all *T uses.

**Step 2:** Recompile; some user code gets compile errors with directives.

**Step 3:** User restructures code that hits the rules:
- 80% of cases: 1-line fix per compiler suggestion
- 15% of cases: moderate refactoring
- 5% of cases: major restructuring or use Pool/Slab

**Estimated impact:**
- No runtime cost
- No annotations required
- Some code rejected as inexpressible
- User must accept "5% of patterns can't be written in pure *T"

**Backward compatibility:** Some existing patterns may stop compiling.
Mostly the patterns are bugs (escape, lifetime issues) but some are
valid patterns the conservative checker rejects.

### 23.6 Migration cost summary

```
Design                        | User code changes  | Compiler work    | Runtime cost  | ABI break
─────────────────────────────────────────────────────────────────────────────────────────────────
keep                          | Add keep words      | Extend zercheck  | 0%            | No
tagged+header                 | None (mostly)       | Major (codegen)  | 1-5%          | Yes (*T)
safe_ptr (additive)           | Opt-in only         | New type system  | 0% default    | No
always-fat Handle             | None                | Widen Handle     | 0%            | Yes (Handle)
4-rule strict checker         | Restructure 5-20%   | Extend zercheck  | 0%            | No
mode-based                    | Per-target         | Conditional codegen | varies      | Per-mode
```

### 23.7 Lowest-friction migration: keep design

Of all designs, keep has:
- Smallest compiler change (extend existing infrastructure)
- Smallest user impact (add one word per compile error)
- Zero ABI break
- Zero runtime cost
- Universal coverage of lifetime axis

If "minimum migration cost" is a priority: keep design wins.

---

## 24. Strategic Positioning

### 24.1 What ZER's distinctive value is

ZER's claim is: **SPARK/Ada-tier safety without SPARK/Ada complexity**.

Specifically:
- Compile-time safety for memory bugs (UAF, double-free, OOB, null, etc.)
- No GC, no runtime overhead
- C-like syntax, low cognitive load
- 4 memory generics explicit in source
- Transparent memory model

This is a real niche with real users (embedded/firmware/kernel developers
transitioning from C who want safety without learning Rust).

### 24.2 The framing risk of safe_ptr default

If safe_ptr becomes default:
- ZER shifts toward "C with shadow tags" framing
- Competes with ASan, scudo, HardenedMalloc (crowded space)
- Loses distinctive embedded positioning
- Pool/Slab/Arena become "optional optimization" rather than core safety
- User base may not learn the 4 generics (just use safe_ptr always)
- ZER's transparency philosophy erodes

This is a positioning shift, not just a technical change.

### 24.3 The framing of keep

If keep is universal:
- ZER stays "compile-time safe systems language"
- "Single annotation" framing is novel and clear
- Pool/Slab/Arena remain first-class
- Distinctive position maintained
- Cognitive load stays low (1 word vs Rust's many)

### 24.4 The framing of always-fat Handle

If Handle always 16 bytes:
- ZER stays "transparent memory model" (just larger pointers)
- Simplifies mental model (one Handle, always works)
- 8B/16B distinction goes away
- Embedded users may push back on RAM cost
- Position shifts slightly toward general-purpose

### 24.5 Which positioning does ZER want?

This is the deeper question. The exploration has been probing technical
designs but the answer depends on positioning:

**Position A: Embedded-first, no compromise on cost**
- Choose: keep design or 4-rule strict
- Audience: Cortex-M0 to Cortex-A, kernel/driver developers
- Distinctive value: SPARK-tier safety, low cognitive load

**Position B: General-purpose systems, accept moderate cost**
- Choose: safe_ptr, tagged+header, or always-fat Handle
- Audience: kernel + servers + applications + embedded
- Distinctive value: universal safety without Rust's complexity

**Position C: Both, accept complexity**
- Choose: mode-based design
- Audience: everyone
- Distinctive value: most flexible safe systems language

Each is defensible. The choice determines which design is optimal.

### 24.6 Recommended positioning evaluation criteria

1. **Who are ZER's current users?** Embedded engineers?
2. **Who are ZER's intended growth audience?** Different?
3. **What's the value proposition vs Rust?** Lower learning curve?
4. **What's the value proposition vs C?** Safety guarantees?
5. **What competes with ZER for each audience?**
   - Embedded: TrustInSoft, SPARK/Ada, C with sanitizers
   - General: Rust, Zig, Go, Swift, modern C++
6. **What's ZER's marketing story per audience?**

Answers determine the design.

---

## 25. The Honest Final Status

### 25.1 No commitment has been made

This document captures exploration. Each design probed is a point on a
map of options. No design has been committed to.

### 25.2 The strongest contenders

After full exploration, three designs emerged as strongest candidates:

**keep design (Section 6.10, 19):**
- 8 bytes, zero runtime cost, single annotation, ZER-philosophy aligned
- Best fit if ZER stays embedded-first
- Best fit if "smart compiler + dumb user" remains philosophy
- Migration: lowest friction

**Tagged+header (Section 6.7, 16.6):**
- 8 bytes, 1-5% runtime cost, no annotation, universal coverage
- Best fit if user prefers no annotation over runtime cost
- Best fit if ZER pivots toward general-purpose
- Migration: moderate (ABI changes)

**Always-fat Handle (Section 6.2):**
- 16 bytes, zero runtime cost, no annotation, one concept
- Best fit if ZER targets Cortex-M3 and up (not Cortex-M0)
- Simplest mental model
- Migration: ABI break for Handle

### 25.3 Likely-to-be-rejected designs

After exploration, these designs are less likely to be the answer:

- **safe_ptr as separate type** (rejected for parallel mechanism complexity)
- **PortableHandle** (rejected as cosmetic rename)
- **Slab-table indirection** (rejected for variable cost)
- **4-rule strict borrow checker** (probably too restrictive)
- **Mode-based** (probably too complex)

### 25.4 What needs to happen next

1. **Audience commitment** — decide who ZER is primarily for
2. **Performance benchmarking** — real numbers, not estimates
3. **Migration analysis** — what does existing ZER code look like under each
4. **Documentation review** — does ZER's marketing story support each design
5. **Then choose** — based on the above, pick a design

### 25.5 What this document is for

This document is the **canonical context** for any future ZER session
working on universal pointer design. Before proposing a new approach,
read this document. Most ideas have been mapped here with honest tradeoffs.

The exploration was thorough. The map is comprehensive. The decision is
not yet made.

---

# End of Document

This document is approximately 4000+ lines covering the full exploration
context of ZER's universal pointer design. It is intended to be the
authoritative reference for the design space exploration as of 2026-05-25.

For new sessions: read this document end-to-end before proposing new
designs. Reference specific sections when discussing tradeoffs. Update
the document with new insights as exploration continues.

When a design commitment is eventually made, that commitment will be a
separate document referencing this map.

---

# PART 3: FALSE POSITIVE ANALYSIS AND AUTO-DETECTION (added 2026-06-01)

This part documents the false-positive scenarios that compile-time-only
lifetime inference produces, and how the universal pointer's runtime
tag check (when combined with compile-time elision) eliminates them
automatically without user annotation.

This part was added after a multi-instance verification discussion that
identified the auto-detection model as the resolution to the
false-positive concern. See Section 30 for the verification chain.

---

## 26. The 6 False-Positive Scenarios in Compile-Time-Only Inference

Compile-time-only lifetime inference (Section 6.9 4-rule strict, Section
6.10 keep-only) produces false positives — programs that are actually
safe at runtime but rejected because the compiler can't prove the
lifetime relationship statically.

These are not analysis bugs. They are fundamental limits of static
analysis: the proof exists at runtime but is not statically derivable.

### 26.1 Scenario 1 — Sibling allocator (runtime-branch provenance)

```
Pool(Task, 8) pool_a;
Pool(Task, 8) pool_b;

*Task p;
if (runtime_cond) {
    p = pool_a.alloc_ptr() orelse return;
} else {
    p = pool_b.alloc_ptr() orelse return;
}
use(p);  // ← compile-time-only inference: which pool's lifetime applies?
```

**Why compile-time can't prove:**

If `pool_a` and `pool_b` have different scopes and neither encloses
the other (siblings), the compiler must take the meet (intersection)
of both lifetimes. If `use(p)` outlives that intersection, the
compiler rejects.

**Why the program is actually safe:**

At runtime, one specific branch was taken. Whichever pool was picked
is still alive — `p` is valid. The compiler can't determine which
branch ran, so it conservatively assumes the worst.

**Status as false positive:** REAL. Compile-time-only would reject;
runtime check would accept.

### 26.2 Scenario 2 — Correlated state (value-dependent lifetime)

```
?*Task p = maybe_alloc();
bool valid = (p != null);
// ... other code ...
if (valid) {
    use(p);  // safe: valid implies p is non-null
}
```

**Why compile-time can't prove:**

Path-sensitive analysis would need to correlate `valid` with `p`'s
optional state across intervening code. This kind of value-correlation
tracking is expensive and incomplete in general.

**Why the program is actually safe:**

`valid` is true iff `p` was non-null at the assignment, and `p` doesn't
change. The runtime guarantee holds; the static proof requires
relational tracking the compiler doesn't do.

**Mitigation in idiomatic ZER:**

This specific pattern can be rewritten with `?T` + `orelse` + `if |p|`
unwrap, which IS compile-time provable:

```
if (maybe_alloc()) |p| {
    use(p);  // p in scope only when non-null — compile-time provable
}
```

So in idiomatic ZER, scenario 2 is partially mitigated by language
features. Non-idiomatic versions (with separate `valid` bool) still
produce false positives.

### 26.3 Scenario 3 — Cross-function lifetime past analysis depth

```
// Function chain 8+ levels deep
fn outermost() {
    Pool(Task, 8) tasks;
    middle1(&tasks);
}
fn middle1(t: *Pool(Task)) { middle2(t); }
fn middle2(t: *Pool(Task)) { middle3(t); }
// ... many levels ...
fn deepest(t: *Pool(Task)) -> *Task {
    return t.alloc_ptr() orelse return null;
    // ← lifetime tied to outermost's pool, but analysis depth may be exhausted
}
```

**Why compile-time can't prove:**

Cross-function analysis has finite depth/precision. FuncProps and
FuncSummary in ZER use lazy DFS with memoization (no fixed depth limit
in principle), but recursive functions trigger conservative widening,
and deeply-nested call chains may hit analysis cost cutoffs.

**Why the program is actually safe:**

The lifetime relationship is genuine (deep callee's return is bounded
by outermost's pool scope). The compiler just can't see the proof.

**Status as false positive:** REAL but less common in ZER than the
other Claude implied — ZER's FuncProps is reasonably deep. Recursive
functions are the main case.

### 26.4 Scenario 4 — Aliasing disjoint pointers

```
fn mutate_and_use(p: *Task, q: *Task) {
    p.field = 5;        // does this affect q?
    use(q.other_field); // safe iff p and q point to different allocations
}
```

**Why compile-time can't prove:**

General alias analysis is undecidable in the limit. The compiler can't
prove `p` and `q` point to different allocations without strong
provenance tracking, which is conservative by default.

**Why the program is actually safe:**

If `p` and `q` come from different allocator slots, mutating one doesn't
affect the other. The runtime fact (different allocations) holds; the
compiler can't see it.

**Important note for ZER:** This isn't really a keep / lifetime concern.
It's an aliasing-axis concern (Rust's `&mut` exclusivity). The runtime
tag check handles it correctly because each allocation has its own tag —
mutation through `p` doesn't invalidate `q`'s tag. So the universal
pointer's runtime check accepts this program as it should.

**Status as false positive for compile-time keep:** Marginal — keep
doesn't model aliasing exclusivity at all, so this isn't really a
keep-rejection case. Listed for completeness; orthogonal to lifetime.

### 26.5 Scenario 5 — Loop-carried lifetime

```
*Task p = init_alloc();
while (some_condition) {
    p = next_alloc(p);
    // each iteration: p is alive for this iteration
}
use(p);  // safe iff loop executed at least once and final p still alive
```

**Why compile-time can't prove:**

Loop fixed-point analysis must widen to a conservative meet. After
the loop, the compiler doesn't know which iteration was last, so it
can't track the specific final allocation's lifetime.

**Why the program is actually safe:**

At runtime, each iteration's `p` was alive for its scope. The final
`p` (whichever it is) is valid if the loop ran at least once and the
underlying allocator is still alive.

**Status as false positive:** REAL. Loops with reassigned pointers
are a known false-positive source for compile-time tracking.

### 26.6 Scenario 6 — Self-referential / cyclic structures

```
struct Node {
    *Node parent;
    *Node left;
    *Node right;
    u32 val;
}
```

**Why compile-time can't prove:**

Compile-time analysis sees `Node` has pointers to other `Node`s. It
can't prove these pointers stay valid because the lifetimes are
mutually circular.

**Why the program is actually safe:**

If all nodes come from the same allocator (Arena, Slab), they share
lifetime. Cycles are fine — everything dies together.

**Status as false positive for compile-time keep:** REAL. Compile-time
keep would force restructuring to indices.

**Already handled in ZER:** the design pushes self-referential structures
to Slab + Handle indices, which IS compile-time provable (Handles carry
no pointer-lifetime concern, only gen check). So this scenario is more
"wrong tool" than "false positive in real practice."

### 26.7 The unifying principle

All 6 scenarios share one shape:

> **The compiler rejects exactly when the safety proof depends on a
> runtime fact it can't statically establish** — which branch ran,
> which values correlate, what's disjoint, how deep the lifetime
> chain goes, whether the loop ran.

Rust handles most of these by making the user assert the fact via
annotation (`'a` lifetime says "trust me, this outlives that"),
converting an inference problem into a checking problem.

ZER refuses Rust-style lifetime annotations. So compile-time-only ZER
has only one sound option when inference fails: reject. That's the
trade — every false positive is a place where the proof exists at
runtime but not statically.

---

## 27. Auto-Detection — Per-Deref Classification

The resolution to the false-positive problem: **combine compile-time
inference with runtime tag check, with the compiler auto-detecting
per-deref whether elision is safe or runtime check must fire.**

### 27.1 The algorithm

```
For each *T deref site in the program:

  1. Run path-sensitive liveness analysis (existing zercheck infrastructure)
  
  2. Classify this specific deref:
     IF compiler can PROVE the pointer is alive on this path:
       → emit raw deref (0 runtime cost)
       → analogous to VRP-elided bounds check
       
     ELSE (any of the 6 scenarios above, or any other unprovable case):
       → emit runtime tag check before deref (~5 cycles)
       → check accepts if actually alive, traps if not
       
  3. Move to next deref site

No user annotation. No "mode" decision. Each deref independently
classified per what the compiler can prove on that specific path.
```

### 27.2 Why this eliminates all 6 false-positive scenarios

| Scenario | Compile-time status | Auto-decision | Runtime behavior |
|---|---|---|---|
| 1. Sibling allocators | Can't unify lifetimes | Insert tag check | Accepts (correct allocator's tag matches) |
| 2. Correlated state | Can't correlate | Insert tag check | Accepts when valid |
| 3. Cross-function depth | Past analysis horizon | Insert tag check | Accepts |
| 4. Aliasing disjoint | Not really keep concern | Insert tag check (or elide) | Accepts (independent tags) |
| 5. Loop-carried | Conservative widening | Insert per-iteration tag check | Accepts when alive |
| 6. Cyclic | Routed to Slab+Handle | (different mechanism) | (gen check) |

**All eliminated.** Zero false positives. The runtime check trivially has
the fact the static proof needed (current liveness state).

### 27.3 The per-deref granularity is the key

The compiler doesn't pick a MODE for a function or module. It picks
per-deref site:

```
fn example(p: *Task) {
    p.field1 = 5;          // ← deref #1: just received p, proven alive → 0 cost
    do_other_work();
    p.field2 = 10;         // ← deref #2: after opaque call, can't prove → CHECK
    return p.field3;       // ← deref #3: same scope after #2, can it elide?
                            //              depends on whether do_other_work was opaque
                            //              if opaque: also CHECK
                            //              if compiler can see body: maybe elide
}
```

Same variable `p`, three derefs, potentially three different decisions
based on what's provable AT EACH SITE.

This granularity is what eliminates false positives while keeping
overhead low.

### 27.4 The elision rate in typical code

```
Estimated deref classification for realistic ZER programs:

  Linear local use:                              ~95% (PROVEN → elided)
  Cross-function single-allocator:               ~80% provable
  Branch-dependent provenance (Scenario 1):      ~5% unprovable → check
  Generic container / async / opaque callback:   ~10% unprovable → check
  Loop-carried lifetime (Scenario 5):            ~30% unprovable → check
  Truly unknowable (cinclude boundary):          handled separately (unsafe)
  
Overall: ~70-85% of derefs elided, ~15-30% get runtime check
Whole-program overhead: ~1-3% typical
```

### 27.5 What the user writes — same code in all cases

```
// User writes plain code; compiler auto-detects per deref:

fn process(p1: *Task, p2: *Task, cond: bool) {
    *Task chosen;
    if (cond) { chosen = p1; } else { chosen = p2; }
    chosen.field = 5;
    // ↑ compiler can't prove which alloc → emits tag check
    //   runtime check accepts because actual allocation is alive
    //   ZERO false positive, ~5 cycles overhead
}

fn straightforward(p: *Task) {
    p.field = 5;
    p.priority = 10;
    // ↑ both derefs proven alive (no intervening free/opaque call)
    //   ZERO runtime cost
}
```

User writes the same shape of code regardless of provability. Compiler
decides per-deref.

### 27.6 Implementation reuses existing infrastructure

Everything needed for auto-detection already exists in ZER:

| Existing ZER infrastructure | Role in auto-detection |
|---|---|
| zercheck path-sensitive analysis | Determines "is p provably alive at this deref?" |
| FuncProps / FuncSummary | Cross-function liveness propagation |
| Allocation coloring | Tracks pointer provenance to allocator source |
| VRP elision pattern | Template for the elision logic (bounds check → tag check) |
| Emitter auto-guard insertion | Emits runtime check when compiler can't prove |

**No new analysis algorithm needed.** Extending existing bounds-check
elision pattern to pointer tag checks. ~500-1000 lines of new code
mostly in emission.

---

## 28. keep vs Runtime Tag Check — Two Cooperating Mechanisms

### 28.1 They are NOT the same mechanism

A common error in framing: calling the runtime check "keep" or saying
the compiler "routes to keep." These are two distinct mechanisms that
cooperate.

```
keep annotation:
  - Compile-time marker that a pointer can be stored persistently
  - Operates entirely during static analysis
  - ZERO runtime cost — purely informs the analysis
  - User-visible at intrinsic_def / function signature level
  
Runtime tag check:
  - Universal pointer mechanism (tag + header verification)
  - Operates at runtime on each unprovable deref
  - ~5 cycles cost when not elided
  - Compiler-emitted, not user-visible
```

### 28.2 How they compose

```
A pointer may have BOTH a keep annotation AND a runtime tag check:

  fn store(keep p: *Task) {
      global_cache.hot = p;     // keep cascade: provable safe → no runtime check
      // ...
      do_opaque_callback();
      use(global_cache.hot);    // after opaque call: can't prove alive
                                 // → runtime tag check fires
                                 // → keep annotation didn't prevent the check
                                 //   because keep is about ESCAPE, not LIVENESS-AT-USE
  }

keep says: "this pointer can be stored persistently"
Tag check says: "this pointer is currently valid"

Different axes. Both can apply to the same pointer. Both compose with
the auto-detection: keep helps the compile-time analysis succeed more
often (catching more cases as provable); tag check handles the cases
that remain unprovable.
```

### 28.3 The relationship table

| Question | Answered by |
|---|---|
| Can this pointer escape its scope? | `keep` annotation (compile-time) |
| Is this pointer currently alive at this deref? | Compile-time analysis + runtime tag check |
| Does this pointer come from the right allocator? | Allocation coloring (compile-time) |
| Is this address still mapped to a valid allocation? | Runtime tag check |
| Was this freed on some path between alloc and this use? | Compile-time path-sensitive (zercheck) |

`keep` and tag check answer DIFFERENT questions on DIFFERENT axes.

### 28.4 The clean formulation

```
WRONG formulation:
  "ZER infers lifetimes statically where provable. For unprovable
   cases, the compiler ROUTES TO KEEP, which carries a runtime check."

PROBLEM: "routes to keep" conflates two mechanisms.
         keep is compile-time. The runtime check is from the
         universal pointer's tag mechanism, not from keep.

CORRECT formulation:
  "ZER infers lifetimes statically where provable (zero cost).
   For unprovable cases, the compiler EMITS A RUNTIME TAG CHECK
   via the universal pointer mechanism. No safe program is ever
   rejected; the cost is a bounded runtime check on exactly the
   pointers whose lifetimes are runtime-dependent, and the compiler
   decides which those are per-deref."
```

The substitution: *"routes to keep, which carries a runtime check"*
→ *"emits a runtime tag check via the universal pointer mechanism."*

### 28.5 Why precision in naming matters

Conflating keep and runtime check confuses:
- Where the cost lives (compile-time analysis is free; runtime check costs ~5 cycles)
- What each mechanism actually does (escape vs liveness — different axes)
- How they compose (both can apply to the same pointer)
- The audit boundary (keep is greppable annotation; tag check is compiler-emitted)

Locked terminology going forward:

| Term | Meaning |
|---|---|
| `keep` annotation | Compile-time marker for pointer-can-escape; zero runtime cost |
| Runtime tag check | Universal pointer mechanism; ~5 cycles when not elided |
| Auto-detection | Per-deref compile-time decision: elide or emit tag check |
| Compile-time elision | Decision to emit raw deref (no runtime check) |
| Static liveness analysis | What the compiler uses to make the elision decision |

When future sessions reference "keep + runtime check," they mean the
COOPERATING composition of these two mechanisms — not a single
mechanism with multiple names.

---

## 29. The Clean Final Formulation

After resolving the false-positive concern and clarifying the
keep-vs-runtime-check vocabulary, the universal pointer design has the
following clean formulation:

### 29.1 The one-line summary

```
ZER infers lifetimes statically where provable (zero cost). For the
unprovable cases, the compiler emits a runtime tag check on the
pointer's tag/header. No safe program is ever rejected; the cost
is a bounded runtime check on exactly the pointers whose lifetimes
are runtime-dependent, and the compiler decides which those are
per-deref.
```

### 29.2 The full claim breakdown

```
Compile-time-provable derefs:
  - Static analysis succeeds (zercheck + FuncProps + allocation coloring)
  - Emitted as raw deref
  - Zero runtime cost
  - ~70-85% of derefs in typical code
  
Compile-time-unprovable derefs:
  - Falls into one of the 6 scenarios (Section 26)
  - Static analysis can't reach the proof
  - Emitted as raw deref + preceding runtime tag check
  - ~5 cycles per check, branch-predictable
  - ~15-30% of derefs in typical code

Whole-program overhead: ~1-3% typical
False positives: NONE (runtime check accepts what's actually valid)
False negatives: NONE (tag mismatch always traps deterministically)
User annotations required: NONE (keep is optional, helps analysis)
FFI boundary: explicit unsafe marker (raw *T from cinclude has no tag)
```

### 29.3 The honest scope

```
Within pure ZER (no cinclude, no raw asm escape):
  - All 6 false-positive scenarios eliminated
  - No safe program rejected
  - Bounded runtime cost on exactly the unprovable cases
  - User writes plain code without lifetime annotations
  
At the cinclude / raw asm boundary:
  - Pointers from C have no ZER tag in their header
  - Compiler emits raw access at this boundary (explicit unsafe)
  - User-declared safety via cinclude binding contract
  - Same boundary as Rust's unsafe extern
```

### 29.4 The "smart compiler, dumb user" property satisfied

```
User experience:
  1. Write plain ZER code with *T pointers (no lifetime annotations)
  2. Compiler decides per-deref: elide or runtime-check
  3. Provable case: zero cost
  4. Unprovable case: ~5 cycle check, no compile error
  5. Genuine UAF on linear path: compile error with directive
  6. Truly invalid runtime access: deterministic trap
  
The user does NOT:
  - Annotate lifetimes (no 'a)
  - Pick "mode" per pointer (no compile-time-only vs runtime)
  - Restructure code to satisfy a conservative compile-time analysis
  - See false positives that block valid programs
  
The user MIGHT:
  - Use `keep` to help compile-time analysis succeed in more places
    (purely optional — improves elision rate, never required)
  - Use Pool/Slab/Arena for hot paths where Handle's gen check is preferred
    (also optional — Handle is a different mechanism with same end safety)
```

### 29.5 Comparison to other safe languages

| Language | Lifetime mechanism | False positives | Runtime cost | Annotation burden |
|---|---|---|---|---|
| C | None | N/A | 0 | None (unsafe) |
| Rust | Borrow checker + lifetimes | Real (~5%) | 0 | High |
| Go/Java | GC | None | High (pauses) | None |
| ZER (this design) | Compile-time + auto-detect runtime tag check | NONE | ~1-3% | None (keep optional) |

ZER's distinctive position: same end-safety as Rust + GC languages,
no annotations + bounded runtime cost.

### 29.6 Lock-in summary

```
Design components (LOCKED 2026-06-01):

  1. Universal *T pointer with embedded version tag (8 bytes, top 16 bits)
  2. Per-allocation header containing current version
  3. Compile-time path-sensitive analysis (zercheck + FuncProps)
  4. Per-deref classification: elide if provable, check if not
  5. keep annotation as optional compile-time analysis aid
  6. Runtime tag check on unprovable derefs (~5 cycles)
  7. FFI boundary via cinclude marker (explicit unsafe)
  8. NO user lifetime annotations required for ordinary code
  
What's eliminated:
  - All 6 false-positive scenarios in compile-time-only inference
  - Rust-style lifetime annotation burden
  - Conservative rejection of safe programs
  - User restructuring forced by analysis incompleteness

What's preserved:
  - Zero runtime cost on the ~70-85% provable derefs
  - 8-byte pointer size (top bits used for version)
  - All existing ZER safety axes (bounds, null, types, concurrency)
  - Compile-time errors on genuinely buggy code (UAF on linear path, etc.)
```

---

## 30. Multi-Instance Verification Chain (added 2026-06-01)

The auto-detection model documented in Sections 26-29 emerged from a
multi-instance verification process. Captured here so future sessions
understand which claims are independently verified vs single-instance.

### 30.1 The verification chain

```
Instance 1: Initial proposal (compile-time only keep)
  → Identified 6 false-positive scenarios

Instance 2 (other Claude): Independent review
  → Confirmed 6 scenarios real
  → Proposed runtime tag check as resolution
  
Instance 3 (independent Claude): Verification with adversarial review
  → Identified verbatim "demand/promise asymmetry" refinement
  → Confirmed auto-detection eliminates all 6 scenarios
  → Identified vocabulary issue (conflating keep with tag check)
  
Instance 4 (this instance): Final verification + clean formulation
  → All 3 prior claims verified
  → Vocabulary distinction codified
  → 4-level conditional soundness preserved
  → Final formulation locked
```

### 30.2 What survived adversarial verification

Items that held up across multiple instances of adversarial review:

```
✓ The 6 false-positive scenarios are real (Section 26)
✓ Auto-detection per-deref eliminates them (Section 27)
✓ keep and runtime tag check are distinct mechanisms (Section 28)
✓ The clean one-line formulation (Section 29.1)
✓ ~1-3% whole-program overhead estimate
✓ ~70-85% elision rate in typical code
✓ No new analysis algorithm required (reuses existing infrastructure)
✓ FFI boundary is the only explicit-unsafe escape needed
```

### 30.3 What got refined through verification

```
Initial framing → Refined framing
─────────────────────────────────────────────────────────────────
"Routes to keep" → "Emits runtime tag check via universal pointer"
"keep + runtime" → "keep is compile-time; tag check is runtime; cooperate"
"Mode per function" → "Per-deref classification"
"User picks mode" → "Compiler auto-decides per use site"
"FROZEN catalog" → "STABLE, grows by promotion" (from asm doc lock corrections)
"Better than Rust" → "Different trade than Rust" (directional, not strictly better)
```

### 30.4 The Section 23 drift warning satisfied

The original universal_pointer.md (PART 2) included a drift warning
inspired by ZER's CLAUDE.md Section 23 pattern. The auto-detection
discussion stress-tested the design and added one verified refinement
(the keep-vs-runtime-check vocabulary distinction) that no single
instance caught on its own.

The verification chain — multiple independent instances reaching the
same conclusion, with each refinement actually changing the doc — is
evidence the design has converged. Future sessions don't need to
re-derive; reference Sections 26-29.

---

# End of PART 3

PART 3 documents the false-positive analysis and auto-detection
resolution. Combined with PART 1 (design space exploration) and PART 2
(expanded context), this document is the canonical reference for ZER's
universal pointer design.

The auto-detection model resolves the central tension between:
- Compile-time-only inference (which produces false positives)
- Pure runtime checking (which has whole-program overhead)

By auto-detecting per-deref and applying runtime check only where
compile-time can't prove, ZER achieves:
- Zero false positives
- ~1-3% whole-program overhead
- No user lifetime annotations
- All existing safety axes preserved

This is the design point that satisfies ZER's "smart compiler, dumb
user" philosophy while delivering universal pointer safety.

---

# PART 4: RECONCILIATION LAYER (added 2026-06-01)

This part exists because a careful adversarial review across the
multi-instance verification chain (Section 30) identified three places
where PART 3's confident summary collapsed PART 1's hedged numbers into
flat assertions, producing internal contradictions a reviewer reading
both parts together would catch.

This is the **drift pattern** ZER's CLAUDE.md Section 23 warns about,
operating across document layers: PART 1 was careful ("estimates,"
"best/realistic/worst"), PART 3's convergence summary quietly dropped
the hedges.

**The fix is append-only.** PART 1 and PART 3 are unchanged. This part
is the reconciliation layer that explicitly cites PART 1's canonical
hedged numbers and resolves where PART 3 over-asserted.

**Discipline going forward:** every new convergence layer in this doc
must include a reconciliation section like this one. The append-only
log stays honest IF each new layer cleans up its own potential
contradictions with earlier layers, instead of pretending the earlier
numbers don't exist.

---

## 31. Reconciliation with PART 1 — Honest Numbers and Caveats

### 31.1 The three contradictions

Multi-instance review identified three places where PART 3 over-asserted
relative to PART 1's careful hedging:

| Topic | PART 3 (over-asserted) | PART 1 (honest) | Resolution |
|---|---|---|---|
| Per-deref cost | "~5 cycles" flat (§27.1, §29.2) | "5 best, 10-30 realistic, 100-500 worst" (§11.1, B.6) | PART 1 is canonical; §31.2 |
| Elision rate | Stated as fact (§27.4) | Labeled "estimates" (§11.5) | PART 1 is canonical; §31.3 |
| False negatives | "NONE" (§29.2, §29.6 implicit) | "1/65536 for 16-bit tag" (§18.4) | PART 1 is canonical; §31.4 |

In all three cases, PART 1 has the careful version. PART 3's summary
stripped the hedges. This section restores them by reference.

### 31.2 Cost claim reconciliation

**PART 3's claim (over-asserted):**
- §27.1: "emit runtime tag check before deref (~5 cycles)"
- §29.2: "~5 cycles per check, branch-predictable"

**PART 1's canonical distribution (§11.1, Appendix B.6):**

```
Tagged+header per-deref cost — full distribution:

  L1 cache hit on header:        ~5 cycles       (best case, warm cache)
  L2 cache hit:                   ~10-15 cycles   (typical warm)
  L3 cache hit:                   ~30-50 cycles
  DRAM hit (cold cache):          ~150-300 cycles
  Multi-thread contention spike:  ~300-1000 cycles (cache line bouncing)
  
  p50 latency:  ~5 ns
  p99 latency:  ~30 ns
  p99.9 latency: ~300 ns (cold cache + contention)
```

**Reconciled claim (canonical going forward):**

> Per-deref runtime tag check cost: typically cache-warm ~5 cycles in
> steady-state hot code, but follows the full cache distribution in
> §11.1 — up to 100-500 cycles in cold cache or contended multi-threaded
> scenarios, with p99.9 around 300ns. The "~5 cycles" figure in PART 3 §27
> and §29 is the cache-warm common case, not the worst case. For
> real-time or worst-case latency analysis, defer to PART 1's distribution.

**Why this matters:** real-time systems care about p99.9 latency, not
p50. ZER's audience includes embedded/firmware/kernel — code where a
cache-cold pointer chase happening at the wrong moment matters. The
flat "~5 cycles" framing obscures this.

### 31.3 Elision rate reconciliation

**PART 3's claim (over-asserted):**
- §27.4: "~95% linear local, ~80% cross-function, ~30% loop-carried
  unprovable, overall 70-85% elided"
- §29.2: "~70-85% of derefs in typical code" (elided)

**PART 1's canonical labeling (§11.5):**

```
Compile-time elision rates (estimates):
  Linear use within function                    95%+
  Pointer passed to single function and used    85%
  Pointer stored in local var, used multiple    80%
  Pointer in for-loop, body uses it             70%
  Cross-function pointer with FuncSummary       60%
  Pointer through generic container             20%
  Pointer through async/callback                10%
  Pointer through opaque function pointer       0% (must check)

  Average elision rate: ~70-85% in typical code  ← LABELED "estimates"
```

**PART 1's open question (§14.10):**

> "Are the cost estimates accurate enough to decide, or do we need
> actual benchmarks? Worst-case scenarios (slab-table contention,
> tagged+header cold cache) are hard to estimate without measurement."

**Reconciled claim (canonical going forward):**

> The elision rates in §27.4 and the ~1-3% whole-program overhead claim
> in §29.2 are **estimates**, not measurements. They follow PART 1
> §11.5's hedged numbers. The compound figure (~1-3% overhead) is the
> product of two unmeasured estimates: elision rate × per-check cost.
> PART 1 §14.10's open question — "do we need actual benchmarks?" —
> has NOT been closed. The ~1-3% figure is a hypothesis awaiting empirical
> validation, not a measured property.

**Separation of concerns:**

| Concern | Status |
|---|---|
| Soundness (no false positives, no false negatives modulo tag width) | **LOCKED** — proven by construction |
| Performance (~1-3% whole-program overhead) | **HYPOTHESIS** — unbenchmarked estimate |

The soundness lock does NOT extend to the performance estimate. Future
sessions must NOT cite ~1-3% as if it's measured. It's an informed
guess based on PART 1's elision rate estimates. A benchmark is needed
to close PART 1 §14.10.

### 31.4 False-negative reconciliation (tag width caveat)

**PART 3's claim (over-asserted):**
- §29.2: "False negatives: NONE (tag mismatch always traps deterministically)"

**PART 1's canonical analysis (§18.4, Section 18 entirely):**

```
False-negative rates by tag width (PART 1 §18.4):

  4-bit tag (ARM MTE):    1/16   (~6%)
  8-bit tag (scudo):       1/256  (~0.4%)
  16-bit tag:              1/65,536 (~0.0015%)
  32-bit fat pointer:      1/4 billion (effectively zero)
  64-bit monotonic:        ~0 (deterministic within ~570 years)

Attack scenarios at 1000 attempts/sec:
  16-bit tag:  ~65 seconds to land UAF (concerning for adversarial systems)
  32-bit tag:  ~50 days   (effectively unattackable)
  64-bit:      ~580 million years (mathematically unattackable)
```

**PART 3 §29.6's lock:**
- "Universal *T pointer with embedded version tag (8 bytes, top 16 bits)"

**The contradiction:** PART 3 §29.6 locks 8-byte/16-bit-tag. PART 3 §29.2
claims "False negatives: NONE." But per PART 1 §18, 16-bit tag has a
1/65,536 false-negative rate. The 8-byte lock and the "no false
negatives" claim are inconsistent inside PART 3.

**Reconciled claim (canonical going forward):**

> The locked design (PART 3 §29.6) is 8-byte pointer with 16-bit tag in
> high bits — chosen for embedded RAM efficiency. This carries a
> 1/65,536 false-negative rate per PART 1 §18.4. The "False negatives:
> NONE" claim in §29.2 is therefore **scoped**: it means "deterministic
> trap on tag mismatch," not "zero probability of stale-tag collision."
> 
> For ZER's non-adversarial embedded audience, 1/65K is acceptable.
> For security-critical contexts, the 16-byte monotonic variant
> (§18.5-§18.6, §18.7) provides effectively-zero false-negative rate
> at the cost of 12-byte or 16-byte pointers.

**Two variants available (locked here):**

```
DEFAULT (PART 3 §29.6 lock):
  8-byte pointer, 16-bit tag in top bits
  False-negative rate: 1/65,536 (PART 1 §18.4)
  Acceptable for: embedded, firmware, non-adversarial systems
  
SECURITY-CRITICAL VARIANT (PART 1 §18.5-§18.7):
  12-byte fat pointer with 32-bit tag, OR
  16-byte fat pointer with 64-bit monotonic counter
  False-negative rate: 1/4 billion (32-bit) or ~0 (64-bit monotonic)
  Acceptable for: security boundaries, long-running adversarial systems
  Cost: larger pointers, slightly more shadow memory
```

Compile flag picks variant per project: `--tag-width=16` (default),
`--tag-width=32`, `--tag-width=64`. PART 1 §18.7 describes this.

### 31.5 The discipline going forward

**Every new convergence layer added to this document must include a
reconciliation subsection at its end, addressing:**

1. **Cost claims** — defer to PART 1's distribution; do not flatten
   to single numbers without citing the cache hierarchy
2. **Performance estimates** — label as "estimates" or "hypothesis";
   only call something measured if it's been benchmarked
3. **Soundness vs performance separation** — soundness can be locked
   (proven by construction); performance is empirical until benchmarked
4. **Tag-width / variant choices** — name the specific variant locked
   and its honest false-negative rate per PART 1 §18

**Without this discipline**, the append-only log accumulates internal
contradictions that adversarial readers find — exactly the failure mode
the multi-instance verification chain caught in PART 3.

**With this discipline**, each layer cleans up its own potential
contradictions with the past, and the doc stays honest as a geological
record where each stratum is dated and reconciled.

### 31.6 What's locked vs what's open

After reconciliation, the doc's claim status is:

```
LOCKED (proven by construction):
  ✓ Universal *T pointer design with embedded version tag
  ✓ Compile-time elision via existing path-sensitive analysis
  ✓ Auto-detection per-deref (elide if provable, check if not)
  ✓ No false positives in pure ZER code (Section 26-27)
  ✓ keep and runtime tag check are distinct cooperating mechanisms (§28)
  ✓ Deterministic trap on tag mismatch (modulo tag width)
  ✓ 8-byte default lock with 16-byte security variant available

OPEN (hypothesis, awaiting empirical validation):
  ⚠ Elision rate (~70-85% typical) — PART 1 §11.5 estimate
  ⚠ Whole-program overhead (~1-3% typical) — PART 1 §14.10 open
  ⚠ p99.9 latency in real workloads — PART 1 §11.1, B.6 ranges
  ⚠ Multi-threaded contention impact — PART 1 §11.5 estimate

SCOPED (true within named caveats):
  ◇ "No false negatives" — true MODULO tag-width false-negative rate
    (1/65,536 for 16-bit; near-zero for 32-bit/64-bit variants)
  ◇ "~5 cycles per check" — true for cache-warm steady-state;
    cold/contended follows PART 1 §11.1 distribution
  ◇ "ZER team work converges to fixed point" — true for X (per-decade)
    not Layer 1 (slow-grow via promotion, per asm doc §1.6.4)
```

### 31.7 The single benchmark that would close most opens

PART 1 §14.10 called for actual benchmarks. The minimum benchmark to
close most opens:

```
Benchmark requirements:
  1. Build a representative ZER program with universal pointer enabled
  2. Measure:
     a. Elision rate (count emitted tag checks / total derefs)
     b. p50/p99/p99.9 latency on tag check fires
     c. Whole-program overhead vs baseline (no checks)
  3. Run on:
     a. Single-threaded embedded target (Cortex-M4, 168 MHz)
     b. Multi-threaded server target (x86_64, 3 GHz, 4+ cores)
     c. Pointer-heavy workload (hash table, graph traversal)
     d. Computation-heavy workload (matrix, signal processing)

Expected outcome (per PART 1 estimates):
  Elision rate: 70-85%
  p99 latency: 30-100 ns on tag check
  Whole-program overhead: 1-5% typical, 5-15% pointer-heavy

If outcome matches estimates: PART 3's claims become MEASURED, can drop
the "estimate" hedges. If outcome differs: update PART 1 estimates and
PART 3 summary together.

Until this benchmark exists: PART 3 cost claims defer to PART 1
distribution, labeled as estimates.
```

### 31.8 The honest end-state of the document

After all reconciliations:

```
What this document IS:
  - A layered geological log of ZER's universal pointer design exploration
  - PART 1: design space mapped with honest hedges
  - PART 2: expanded context, examples, comparisons
  - PART 3: auto-detection convergence model (correctness story)
  - PART 4: reconciliation layer (this) tying summary back to hedged base

What this document IS NOT:
  - A benchmarked performance specification
  - A finalized compiler implementation guide
  - A finished, locked-forever artifact

What's actually locked:
  - The CORRECTNESS architecture (sound by construction)
  - The DESIGN choices (8-byte default, 16-byte security variant)
  - The VOCABULARY (keep vs tag check as distinct mechanisms)
  - The DISCIPLINE (append-only with reconciliation per new layer)

What's still hypothesis:
  - Performance numbers (estimates pending benchmark)
  - Real-world elision rates (theoretical until measured)
  - Worst-case latency in production (depends on workload)
```

---

# End of PART 4

PART 4 is the reconciliation layer. It does NOT replace PART 3 — it
references PART 3's claims and cites PART 1's hedged versions when
PART 3 over-asserted.

The discipline: future convergence layers must reconcile too. Append-
only logs stay honest only when each new layer cleans up its own
contradictions with the past, not when they pretend the past doesn't
exist.

The architecture is sound. The performance is estimated. Both are
true at the same time, and the doc now says so explicitly.

---

# PART 5: DECISION — COMPILE-TIME-ONLY `keep`, NO RUNTIME (2026-06-07)

Parts 1–4 were exploration. This part records an actual COMMITMENT for the
pointer-lifetime/escape axis, made with the project owner on 2026-06-07.

**PART 5 SUPERSEDES PART 3's design lock (§29.6).** PART 3 converged on the
auto-detection model (universal `*T` + version tag + per-deref runtime check)
and "locked" it. ZER will NOT build that. The chosen direction is the
**compile-time-only `keep` design** (Section 6.10 / 19), with **no runtime tag
check, no tagged pointers, no per-allocation header, no ABI change to `*T`**.
PART 3's auto-detection analysis remains valid as the *alternative not taken* —
it is preserved (append-only), not deleted.

## 32. The decision and why

**Decided:**
- Compile-time-only `keep` for the pointer-lifetime/escape axis.
- Reject the runtime tag check / auto-detection (PART 3).
- Pointers stay 8 bytes. **Zero runtime cost** on the lifetime axis.

**The deciding criterion (owner's framing):**
- A false POSITIVE (safe program rejected) is **ACCEPTABLE** — restructure to
  better code, exactly as Rust does.
- A false NEGATIVE (an UNSAFE program compiles clean — *"it IS wrong but our
  design makes it pass"*) is **UNACCEPTABLE** — a hole in the safety theorem.

**Why this settles runtime-vs-compile-time:** a compile-time analysis that is
CONSERVATIVE (rejects whenever it cannot prove liveness) is SOUND — it never
under-rejects, only over-rejects. That is the definition of a sound static
analysis (Rust's borrow checker is one). PART 3's runtime tag check was the
doc's answer to FALSE POSITIVES (reducing over-rejection); since over-rejection
is acceptable here, the runtime layer is **unnecessary for soundness**. So:
compile-time-only, accept the false positives.

## 33. Soundness verification — the escape matrix (`tests/test_escape_matrix.c`)

`keep` is sound iff three things hold (the false-negative risk lives entirely
here):
1. **Conservative default** — reject whenever liveness can't be proven.
2. **`keep` is VERIFIED at the call site** — the compiler checks the argument
   actually outlives the storage, cascading to a real `'static`/global source;
   it does NOT trust the annotation (the SPARK Definition-B trap that CLAUDE.md's
   goal section forbids).
3. **Every laundering path tracked or rejected** — alias, `@ptrcast`,
   `@ptrtoint`, array→slice, identity-wash, struct-wrapper, orelse-fallback — at
   every sink: return / global / param-field / nested-field.

To turn "sound" from a claim into a standing GUARD, we built an exhaustive
negative-only oracle: `tests/test_escape_matrix.c` — the
`{escape-dest × launder-path × local-source}` product, every cell a
genuinely-unsafe local escape that MUST be rejected **for the escape reason**
(integrity guard: a rejection by parse/type error is flagged INVALID, not
silently counted as a pass — the probe-10/11 lesson). `-Wswitch`-enforced so the
grid can't shrink. Companion to `test_shape_matrix.c` (temporal/UAF axis).

**First run found 4 real false negatives** — point-2/3 holes that were already
closed for the RETURN sink but NOT for global-via-`@ptrcast` or
param-field/nested-field sinks (unsafe programs compiling clean):
- **H1**: `global = @ptrcast(*T, &local)`
- **H2**: `param.field = local_array` (array→slice)
- **H3**: `param.field = q`  where `q = &local`
- **H4**: `nested.field = q` where `q = &local`

Root cause: the direct-`&local` check handled both global and param-field sinks,
but the *laundered* checks (local-derived-ident, array→slice) only fired at the
global sink, and the global check didn't unwrap `@ptrcast` on the value side.

**Fix (checker.c):** a shared `classify_escape_sink()` that walks any assignment
target to its root and reports global-vs-param-ptr sink; all three laundered
checks route through it (fire at BOTH sinks), and the direct check unwraps
intrinsics on the value. After the fix: **escape matrix 20/20, 0 false
negatives.** Regression tests:
`tests/zer_fail/escape_{ptrcast_global,array_param_field,alias_param_field,alias_nested_field}.zer`.

**Call-site verification (point 2) confirmed empirically:** passing `&local` to
a `keep` parameter is rejected with `local variable 'x' cannot satisfy 'keep'` —
`keep` is a *checked constraint*, not a *trusted contract*. This is the property
that makes the whole compile-time-only model sound.

## 34. The plan, and the one rule that keeps it sound

The escape FOUNDATION (the checks `keep` builds on) is now matrix-verified sound
(20/20). `keep`-universalization (Section 6.10 / 19) extends `keep` from the
narrow System #21 (params only) to struct fields, locals, and cascade. Because
`keep` is a call-site-VERIFIED accept and the base analysis rejects-on-
uncertainty, extending it can only (a) add verified accepts or (b) reject more —
it **cannot introduce a false negative UNLESS a new boundary default TRUSTS
instead of checks.**

**THE ONE RULE (soundness invariant for the implementation):** every new `keep`
boundary default must REJECT-on-uncertainty, never trust. Danger zones
(Section 19.5): `keep` on function-pointer types, on cinclude bindings, on
generic container fields. Each must default-to-reject when `keep` isn't declared.
The escape matrix is the standing guard — any boundary-default that trusts
instead of checks surfaces as a new matrix HOLE.

**Sequenced steps:**
1. ✅ Escape-matrix oracle + close H1–H4 (foundation verified sound).
2. As `keep` grows, extend the matrix's launder/dest axes (funcptr-stored,
   cinclude-stored, generic-container-stored) — each new boundary gets a cell.
3. Implement `keep` on struct fields / locals / cascade (Section 13.1), against
   the matrix.
4. For each boundary default: add the negative cell FIRST (must reject), then
   the feature.

## 35. Reconciliation with prior parts (per the PART 4 discipline)

| Topic | Prior part | PART 5 |
|---|---|---|
| Direction | PART 3 §29.6 "locked" universal `*T` + runtime tag check | **SUPERSEDED** — compile-time-only `keep`, no runtime check. PART 3 stands as alternative-not-taken. |
| Runtime cost | PART 3: ~1–3% (hypothesis) | **Zero** on the lifetime axis (compile-time only). The unbenchmarked estimate is moot for the chosen design. |
| False positives | PART 3 eliminated via runtime check | **ACCEPTED** (restructure) — the owner's explicit trade. |
| False negatives | PART 1/3: "none modulo tag width" | Goal: **zero**, by conservative compile-time analysis; GUARDED by the escape matrix (20/20, and no tag-width caveat — there is no tag). |
| Pointer size | PART 3: 8-byte default + 16-byte security variant | **8 bytes**, no variants (no tag). |

**LOCKED by PART 5:**
- compile-time-only `keep` for the lifetime axis (no runtime layer)
- the soundness criterion (over-reject OK, never under-reject)
- the escape matrix as the standing no-false-negative guard
- the boundary-default-must-reject implementation rule

**Still OPEN:**
- `keep`-universalization itself (struct fields / locals / cascade) — foundation
  verified, feature not yet implemented
- aliasing-exclusivity (`&mut`) axis — out of scope, unchanged (Section 28 / 22.12)

This part is append-only. PART 3's lock is superseded HERE (not by editing
PART 3) so the geological record stays honest: PART 3 was the exploration's
convergence; PART 5 is the owner's decision.

---

# End of PART 5
