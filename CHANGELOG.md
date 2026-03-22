# Changelog

All notable changes to ZER-LANG. Read this to understand project history and current state.

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

**Compiler:** 906 tests + 491 fuzz, all passing. ~9,300 lines.
**License:** GPL v3 + Runtime Exception.
**Language features:** All core features implemented. `cinclude` for C interop. No UFCS (dropped).
**Known gaps:** Array-to-slice coercion at var-decl level doesn't emit wrapper. No native backends (emit-C only). No standard library (freestanding by design).
**Next likely work:** ARM cross-compile demo (QEMU), more CVE demos, v0.1.0 tag.
