# Changelog

All notable changes to ZER-LANG. Read this to understand project history and current state.

## 2026-03-23

### Major Features
- **`?FuncPtr`** — optional function pointers with null sentinel. `?void (*cb)(u32) = null;` works with if-unwrap, orelse, assignment. Zero overhead. `IS_NULL_SENTINEL` macro unifies TYPE_POINTER and TYPE_FUNC_PTR across all emitter paths.
- **Function pointer `typedef`** — `typedef u32 (*Callback)(u32);` now parses and emits correctly. Enables `[]Callback` slices.
- **Named slice typedefs for ALL types** — `[]T` no longer uses anonymous structs. Every primitive gets `_zer_slice_T` in preamble. Struct/union declarations emit `_zer_slice_StructName`. Slices work across function boundaries for all types.
- **`?[]T` optional slice typedefs** — `_zer_opt_slice_T` for all primitives, structs, unions.
- **Enum explicit values** — `enum { low = 1, med = 5, high = 10 }` now works correctly (was emitting loop index). Negative values (`err = -1`) also work. Auto-increment after explicit values like C.
- **`else if` confirmed working** — was always implemented in parser, docs incorrectly said it wasn't supported. Fixed all docs.

### Bug Fixes (Rounds 4-5)
- **BUG-067:** `*Union` pointer auto-deref now resolves variant type correctly
- **BUG-068:** Enum explicit values — emitter now uses stored value instead of loop index
- **BUG-069:** All `[]T` slice types now have named typedefs (was anonymous structs breaking across functions)
- **BUG-070:** `?FuncPtr` now supported with null sentinel (parser unwraps `?` around func ptr)
- **BUG-071:** Function pointer typedef now parses and emits correctly

### Audit Convergence
- Round 1: 12 bugs → Round 2: 9 → Round 3: 2 → Round 4: 2 → Round 5: 1
- Compiler clean across all audited dimensions

### Tests
- **946 tests + 491 fuzz, all passing**

## 2026-03-22 (continued — audit rounds 2-3)

### Bug Fixes (Round 2 — 9 bugs)
- **BUG-056:** Bitwise compound `&= |= ^= <<= >>=` on floats now rejected
- **BUG-057:** Union switch exhaustiveness now uses bitmask deduplication
- **BUG-058:** Union switch variant names now validated against union definition
- **BUG-059:** `@truncate`/`@saturate` now validate source is numeric
- **BUG-060:** Const check extended to field/index chains (not just direct ident)
- **BUG-061:** Compound assignment narrowing `u8 += u64` now rejected (literals exempt)
- **BUG-062:** `?UnionType` now gets `_zer_opt_UnionName` typedef (like structs)
- **BUG-063:** Expression-level `orelse return/break/continue` now fires defers
- **BUG-064:** `volatile` qualifier now emitted on pointer types (was completely stripped — critical for MMIO)

### Bug Fixes (Round 3 — 2 bugs)
- **BUG-065:** Union switch `|*v|` mutable capture now emits pointer, not value copy
- **BUG-066:** Var-decl `orelse return` in `?void` function now emits `{ 0 }` not `{ 0, 0 }`

### Systematic Negative Test Sweep
- Added 26 negative tests covering ALL previously-uncovered `checker_error()` paths
- Every rejection rule in the checker now has at least one test proving it fires

### Research & Implementation
- **Array-to-slice coercion** — now works at call sites, var-decl, and return (3 emission sites)
- **`@cast`** — parser fix allows TOK_IDENT as type_arg for @cast intrinsic

### Tests
- **938 tests + 491 fuzz, all passing**
- Audit trend: 12 → 9 → 2 bugs per round (converging)

## 2026-03-22

### License
- **Relicensed to GPL v3 + Runtime Exception** (GCC model). Companies can't fork and keep improvements private. Firmware compiled by zerc is yours.

### Language Features
- **`cinclude` keyword** — `cinclude "header.h";` emits `#include "header.h"` in output C. C interop story: `import` = ZER module (full pipeline), `cinclude` = C header (pass-through to GCC).

### Bug Fixes (5 bugs)
- **BUG-042:** `?Enum` optional emitted anonymous struct — GCC type mismatch. Fix: added `case TYPE_ENUM` → `_zer_opt_i32`.
- **BUG-043:** `?void` assign null emitted `{ 0, 0 }` for 1-field struct. Fix: check `inner == TYPE_VOID` → emit `{ 0 }`.
- **BUG-044:** Slice vars (`[]u8`) auto-zero emitted `= 0` instead of `= {0}`. Fix: added `TYPE_SLICE` to auto-zero conditions.
- **BUG-045:** Non-u8/u32 array slicing emitted `void*` pointer. Fix: u32 → `_zer_slice_u32`, others get typed pointer.
- **BUG-046:** `@trap()` rejected as unknown intrinsic. Fix: added checker + emitter handlers.

### Tests
- Added `??T` nested optional rejection test (checker)
- Added E2E tests for all 5 bug fixes + `cinclude`
- **906 tests + 491 fuzz, all passing**

### Documentation
- **`docs/reference.md`** — Complete language reference (748 lines). Verified against actual compiler behavior, excludes unimplemented features.
- **`docs/CONTRIBUTING.md`** — Updated test counts, bug counts, added "Before You Start" prereqs.
- **`CLAUDE.md`** — Added agent-verify workflow, `docs/reference.md` to mandatory update checklist.
- **`BUGS-FIXED.md`** — 46 bugs documented.

### CVE Demos
- **`examples/cve-demos/`** — Side-by-side C vs ZER demos:
  - CVE-2014-0160 (Heartbleed): C leaks secrets, ZER traps at bounds check.
  - CVE-2021-3156 (Baron Samedit): C overflows heap, ZER catches at bounds check.

### Spec Changes
- **UFCS dropped** from spec — never implemented, not needed. C-style `func(&obj)` is sufficient. Builtin methods (pool/ring/arena) already have method syntax.

## 2026-03-21

### Bug Fixes
- **BUG-041:** Bit extraction `[31..0]` emitted `1u << 32` (UB). Fix: changed to `1ull`.

### Tests
- 16 new E2E tests (bit extraction, defer stress, struct edges, 5-variant enum switch)
- 5 new ZER-CHECK tests (pool.free in loop, alloc+free per iteration, double free)
- Fixed zercheck tests that used `i++` (invalid ZER)

### Documentation
- **`CLAUDE.md`** — Added syntax traps 11-14, emitter critical patterns, mandatory `compiler-internals.md` read directive, spawning agents ZER syntax rules block.
- **`docs/compiler-internals.md`** — Added code snippets for optional types, slice emission, orelse emission, builtin method patterns. Updated stale Arena status.

---

## Project State

**Compiler:** 946 tests + 491 fuzz, all passing. ~10,000 lines. 71 bugs found and fixed.
**License:** GPL v3 + Runtime Exception (GCC model).
**Language features:** All core features implemented. `cinclude` for C interop. `@cast` for distinct typedefs. `?FuncPtr` optional function pointers. Function pointer typedef. Named slice typedefs for all types. Array-to-slice coercion. Volatile emission. Enum explicit values. `else if` supported.
**Audit status:** 5 rounds completed, converged (12→9→2→2→1). 26 systematic negative tests covering all checker rejection paths.
**Demos:** CVE-2014-0160 (Heartbleed) + CVE-2021-3156 (Baron Samedit) side-by-side. ARM Cortex-M3 QEMU firmware (1225 bytes).
**Known limitations:** `[]FuncPtr` (slice of raw function pointers without typedef) still anonymous — use `typedef` first. No native backends (emit-C only).
**Next:** v0.1.0 tag.
