# Changelog

All notable changes to ZER-LANG. Read this to understand project history and current state.

## 2026-03-23 (continued — agent-driven audit, 12 bugs)

### Bug Fixes (Round 9 — Agent Audit)
- **BUG-084:** Parser stack buffer overflow — `Node *values[16]` in switch arm parsing now bounds-checked
- **BUG-085:** Slice expression anonymous struct — NODE_SLICE now uses named `_zer_slice_T` for ALL primitive types (was only u8/u32)
- **BUG-086:** `emit_file_no_preamble` missing NODE_TYPEDEF — typedefs in imported modules now emitted
- **BUG-087:** `emit_file_no_preamble` missing NODE_INTERRUPT — interrupt handlers in imported modules now emitted
- **BUG-088:** `?DistinctFuncPtr` not null sentinel — `is_null_sentinel()` function unwraps TYPE_DISTINCT, `emit_type_and_name` handles `?Distinct(FuncPtr)` name placement
- **BUG-089:** Array-to-slice coercion UB — uses `eff_callee` (unwrapped) instead of `callee_type` for distinct func ptr
- **BUG-090:** Missing struct field error — `p.nonexistent` now errors instead of silently returning void. UFCS tests updated.
- **BUG-091:** `@cast` validation — unwrap now works (`@cast(u32, celsius)`), cross-distinct now blocked (`@cast(Fahrenheit, celsius)`)
- **BUG-092:** Builtin arg count validation — all 11 Pool/Ring/Arena methods now check argument count
- **BUG-093:** Field access on non-struct — `u32.foo` now errors instead of silently returning void
- **BUG-094:** NODE_CINCLUDE in AST debug — `node_kind_name()` and `ast_print()` now handle NODE_CINCLUDE
- **BUG-095:** Unchecked fread — `zerc_main.c` now checks return value, returns NULL on short read

### Bug Fixes (Round 13 — distinct struct field access + auto-zero, 2 bugs)
- **BUG-111:** Field access on distinct struct fails — unwrap distinct before struct/union/pointer dispatch in NODE_FIELD handler
- **BUG-112:** Auto-zero for distinct compound types emits `= 0` instead of `= {0}` — unwrap distinct in both global and local auto-zero paths

### Bug Fixes (Round 12 — intrinsic validation + remaining distinct, 5 bugs)
- **BUG-106:** `@ptrcast` now validates source is a pointer
- **BUG-107:** `@inttoptr` now validates source is an integer
- **BUG-108:** `@ptrtoint` now validates source is a pointer
- **BUG-109:** `@offset(T, field)` now validates field exists on struct
- **BUG-110:** `?[]DistinctType` unwraps distinct on slice element for named typedef

### Bug Fixes (Round 11 — TYPE_DISTINCT audit, 2 bugs)
- **BUG-104:** `?DistinctType` emits anonymous struct — unwrap TYPE_DISTINCT in `emit_type(TYPE_OPTIONAL)` inner switch
- **BUG-105:** `[]DistinctType` emits anonymous struct — unwrap TYPE_DISTINCT in `emit_type(TYPE_SLICE)` and NODE_SLICE expression

### Bug Fixes (Round 10 — interaction audit, 5 bugs)
- **BUG-099:** `\x` hex escape in char literals now parses hex digits correctly (was storing 'x')
- **BUG-100:** `orelse break`/`orelse continue` outside loop now rejected at checker
- **BUG-101:** Bare `return;` in `?*T` function now emits `return (T*)0;` (was invalid compound literal)
- **BUG-102:** Defer inside if-unwrap body now fires at block scope exit (was deferring to function exit). Same fix for union switch capture arms.
- **BUG-103:** Calling non-callable type (`u32 x = 5; x(10)`) now produces checker error

### Gap Fixes (hardening existing checks)
- **BUG-096:** Unknown builtin method names (pool.bogus, ring.clear, arena.destroy) now error with available method list
- **BUG-097:** Arena-derived flag propagated through aliases — `q = arena_ptr; global = q` now caught
- **BUG-098:** Union switch lock via pointer auto-deref — `switch (*ptr) { .a => |*v| { ptr.b = 99; } }` now caught

### Tests
- **971 tests + 491 fuzz, all passing**

## 2026-03-23 (continued — security review fixes)

### Safety Architecture Overhaul (6 bugs from external security review)
- **BUG-078/079: Inline bounds checks** — Replaced statement-level bounds check hoisting with inline checks using comma operator: `(_zer_bounds_check(idx, len, ...), arr)[idx]`. Fixes two bugs at once: (1) missing checks in if/while/for conditions, (2) short-circuit `&&`/`||` evaluation now respected. Lvalue semantics preserved via array-to-pointer decay.
- **BUG-080: Scope escape via struct field** — `global.ptr = &local` now caught. Walk assignment target chain (NODE_FIELD/NODE_INDEX) to root identifier. Also catches global-scoped (not just static) targets.
- **BUG-081: Union type confusion** — Cannot mutate union variant while switch capture pointer is active. Compile-time error prevents silent memory corruption.
- **BUG-082: ZER-CHECK handle aliasing** — `Handle(T) alias = h1` now tracked. Freeing `h1` propagates to all aliases. Independent handles unaffected (no false positives).
- **BUG-083: Arena lifetime escape** — `arena.alloc(T)` results marked `is_arena_derived`. Storing in global/static variables is compile-time error.

### Tests
- **965 tests + 491 fuzz, all passing** (+8 checker security tests, +4 E2E bounds check tests, +3 zercheck alias tests)
- Zero regressions across all 4 QEMU demos

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

### Bug Fixes (Real-program testing — 3 bugs)
- **BUG-075:** `?Handle(T)` fell to anonymous struct default. Handle is u32 — added `TYPE_HANDLE -> _zer_opt_u32`.
- **BUG-076:** Union switch mutable capture `|*v|` emitted `__auto_type *v` which GCC rejects. Fixed: look up actual variant type for pointer declaration.
- **BUG-077:** Union switch mutable capture `|*v|` modified a copy, not the original. Fixed: switch now takes pointer `__auto_type *_zer_swp = &(expr)` so mutations persist.

### QEMU Demos (4 programs, exercising every feature)
- **shell.zer** — Task scheduler with Pool+Handle, ?FuncPtr callbacks, exhaustive switch
- **ringbuf_protocol.zer** — MODBUS-like parser with Ring, enum explicit values, ?FuncCode, CRC, error handling
- **stress_test.zer** — 18 checks covering tagged union captures, arena+defer loop, packed struct+bit extract, nested if-unwrap, array→slice coercion, distinct typedef, defer+orelse return, 3-level field access, ?FuncPtr callback table, combined arena+union+defer
- **rtos.zer** — Full cooperative RTOS: Pool tasks, Ring IPC, Arena scratch per tick, tagged union messages, ?FuncPtr entry points, volatile MMIO, 6804 bytes. **Compiled and ran correctly on first try.**

### Tests
- **950 tests + 491 fuzz, all passing**

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

**Compiler:** 1004 tests + 491 fuzz, all passing. ~10,000 lines. 112 bugs found and fixed.
**License:** GPL v3 + Runtime Exception (GCC model).
**Language features:** All core features implemented. `cinclude` for C interop. `@cast` for distinct typedefs. `?FuncPtr` optional function pointers. Function pointer typedef. Named slice typedefs for all types. Array-to-slice coercion. Volatile emission. Enum explicit values. `else if` supported.
**Safety:** Inline bounds checks (conditions + short-circuit safe). Scope escape via struct fields caught. Union type confusion blocked. ZER-CHECK handles aliasing. Arena lifetime escape detected.
**Audit status:** 13 rounds completed (12→9→2→2→1→2→CLEAN→6→12→5→2→5→2). 26 systematic negative tests. 4 QEMU real-program demos.
**Demos:** CVE-2014-0160 (Heartbleed) + CVE-2021-3156 (Baron Samedit) side-by-side. ARM Cortex-M3 QEMU firmware (1225 bytes).
**Known limitations:**
- `[]FuncPtr` (slice of raw function pointers without typedef) still anonymous — use `typedef` first.
- Scope escape / arena escape checks are intraprocedural (no cross-function lifetime analysis).
- No native backends (emit-C only).
