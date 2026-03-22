# Bugs Fixed — ZER Compiler

Tracking compiler bugs found and fixed, ordered by discovery date.
Each entry: what broke, root cause, fix, and test that prevents regression.

---

## Round 1 — Firmware Pattern Stress Tests (2026-03-19)

### BUG-001: Enum value access `State.idle` fails type-check
- **Symptom:** `State.idle` type-checks as `void`, all enum value usage broken
- **Root cause:** Checker `NODE_FIELD` had no handler for `TYPE_ENUM`. Enum dot access fell through to "unresolved field" fallback returning `ty_void`
- **Fix:** Added TYPE_ENUM handler in checker.c that validates variant name and returns the enum type
- **Test:** `test_firmware_patterns.c` — enum state machine tests

### BUG-002: Enum values emit invalid C (`State.idle` instead of `_ZER_State_idle`)
- **Symptom:** GCC rejects emitted C — `State.idle` not valid in C
- **Root cause:** Emitter `NODE_FIELD` emitted `obj.field` for all types, didn't check for enum
- **Fix:** Added enum type check at top of NODE_FIELD in emitter — emits `_ZER_EnumName_variant`
- **Test:** `test_firmware_patterns.c` — all enum E2E tests

### BUG-003: Enum switch arms emit bare identifier
- **Symptom:** `.idle =>` in switch emits `if (_sw == idle)` — GCC error, `idle` undeclared
- **Root cause:** Non-union enum switch arms hit generic `emit_expr` path, not the _ZER_ prefixed path
- **Fix:** Added enum switch arm branch in emitter that emits `_ZER_EnumName_variant`
- **Test:** `test_firmware_patterns.c` — enum state machine + switch tests

### BUG-004: Defer not firing on return inside nested blocks
- **Symptom:** `defer cleanup(); if (cond) { return 1; }` — cleanup never runs
- **Root cause:** `NODE_BLOCK` saved/restored the ENTIRE defer stack, so inner blocks couldn't see outer defers. Return inside inner block found empty stack.
- **Fix:** Changed to base-offset approach: blocks track where their defers start, return emits ALL defers from top to 0, block exit emits only that block's defers
- **Test:** `test_firmware_patterns.c` — defer + early return, defer + orelse return

### BUG-005: Orelse-return path skipped defers
- **Symptom:** `defer mark(); u32 val = nothing() orelse return;` — mark() never called when orelse triggers return
- **Root cause:** The orelse-return expansion (`if (!has_value) return 0;`) didn't call `emit_defers()` before the return. The break/continue paths already had it.
- **Fix:** Added `emit_defers()` call in orelse-return expansion
- **Test:** `test_firmware_patterns.c` — defer + orelse return combo

### BUG-006: `&x.field` parsed as `(&x).field` instead of `&(x.field)`
- **Symptom:** `&sys.primary` returns `*System` then field access gives `Sensor` instead of `*Sensor`
- **Root cause:** `parse_unary` recursively called itself for the operand but returned directly to the primary parser — postfix (. [] ()) wasn't applied. So `&sys` was the unary, `.primary` was postfix on the result.
- **Fix:** Changed `parse_unary` to call `parse_postfix(parse_primary())` for non-prefix case, matching C precedence (postfix > prefix)
- **Test:** `test_firmware_patterns.c` — nested struct pointer chains, address-of nested fields

### BUG-007: Ring push wrote wrong size (`sizeof(int)` instead of `sizeof(u8)`)
- **Symptom:** `Ring(u8, 16)` push/pop FIFO returned wrong values — only first element correct
- **Root cause:** Emitter used `__auto_type` for push temp variable, which deduced `int` (4 bytes). `memcpy` then wrote 4 bytes per element into 1-byte slots, corrupting adjacent data.
- **Fix:** Emit the actual ring element type for the push temp variable
- **Test:** `test_firmware_patterns.c` — ring push/pop FIFO order

---

## Round 2 — Firmware Pattern Stress Tests (2026-03-19)

### BUG-008: Pointer indexing `(*u32)[i]` rejected
- **Symptom:** `data[0]` on `*u32` pointer fails with "cannot index type '*u32'"
- **Root cause:** Checker `NODE_INDEX` only handled TYPE_ARRAY and TYPE_SLICE, not TYPE_POINTER
- **Fix:** Added TYPE_POINTER case returning `pointer.inner`
- **Test:** `test_firmware_patterns2.c` — array passed via &arr[0]

### BUG-009: `@size(StructName)` emitted empty `sizeof()`
- **Symptom:** GCC error: `sizeof()` with no argument
- **Root cause:** Parser excluded `TOK_IDENT` from type_arg detection (line: `p->current.type != TOK_IDENT`). Named types like `Header` were parsed as expression args, not type_arg. Emitter only checked type_arg.
- **Fix:** Emitter falls back to looking up args[0] as a type name when type_arg is NULL
- **Test:** `test_firmware_patterns2.c` — @size(Header)

### BUG-010: Forward function declarations not supported
- **Symptom:** `u32 func(u32 n);` (with semicolon, no body) fails to parse — "expected '{'"
- **Root cause:** Parser unconditionally called `parse_block()` after parameter list
- **Fix:** Check for semicolon before `parse_block()`. If found, set body to NULL (forward decl)
- **Test:** `test_firmware_patterns2.c` — mutual recursion with forward decl

### BUG-011: Forward decl followed by definition = "redefinition"
- **Symptom:** Forward declare then define same function → checker error
- **Root cause:** `add_symbol` rejects duplicate names unconditionally
- **Fix:** Before adding, check if existing symbol is a forward-declared function (no body). If so, update it with the new body instead of erroring.
- **Test:** `test_firmware_patterns2.c` — mutual recursion

### BUG-012: break/continue emitted ALL defers (including outer scope)
- **Symptom:** `for { defer f(); for { break; } }` — inner break fires outer defer
- **Root cause:** `emit_defers()` emitted from index 0 (all defers). Break should only emit defers within the loop scope.
- **Fix:** Added `loop_defer_base` to Emitter. Loops save/restore it. Break/continue use `emit_defers_from(e, e->loop_defer_base)` instead of `emit_defers(e)`. Return still emits all.
- **Test:** `test_firmware_patterns2.c` — inner break + outer defer

---

## Round 3 — Firmware Pattern Stress Tests (2026-03-19)

### BUG-013: `return ring.pop()` from `?u8` function double-wraps optional
- **Symptom:** `?u8 uart_recv() { return rx_buf.pop(); }` emits `return (_zer_opt_u8){ <already_opt>, 1 }` — GCC error
- **Root cause:** Emitter always wraps return value in `{expr, 1}` for `?T` functions, even when expr is already `?T`
- **Fix:** Check if return expr's type already matches function return type via `checker_get_type` + `type_equals`. If so, return directly without wrapping.
- **Test:** `test_firmware_patterns3.c` — UART loopback with ring.pop() return

---

## Linked List Session (2026-03-19)

### BUG-014: Self-referential structs fail — "undefined type 'Node'"
- **Symptom:** `struct Node { ?*Node next; }` — "undefined type 'Node'" on the `?*Node` field
- **Root cause:** `register_decl` resolved field types BEFORE registering the struct name in scope. So `Node` wasn't in scope when its own field `?*Node` was resolved.
- **Fix:** Move `add_symbol` BEFORE field type resolution for both structs and unions.
- **Test:** `ZER-Test/linked_list.zer` — doubly linked list with ?*Node prev/next

### BUG-015: `orelse` precedence lower than `=` — assignment eats the orelse
- **Symptom:** `current = current.next orelse return` parsed as `(current = current.next) orelse return` instead of `current = (current.next orelse return)`
- **Root cause:** Precedence table had PREC_ORELSE below PREC_ASSIGN. Assignment consumed `current.next` as its RHS, leaving `orelse return` outside.
- **Debugging:** Confirmed via targeted debug: auto-deref returned kind=14 (TYPE_OPTIONAL) for `current.next`, but orelse handler received kind=13 (TYPE_POINTER). Typemap overwrite debug showed NO overwrites. This proved the orelse was receiving a different expression (`current` not `current.next`).
- **Fix:** Swap PREC_ASSIGN and PREC_ORELSE in the precedence enum. Update `parse_expression` to start at PREC_ASSIGN.
- **Test:** `ZER-Test/test_walk.zer` — linked list traversal with `current = current.next orelse return`

### BUG-016: Slice-to-pointer decay missing for C interop
- **Symptom:** `void puts(*u8 s); puts("Hello World");` — "expected '*u8', got '*u8'" (string literal is []u8, not *u8)
- **Root cause:** No implicit coercion from []T to *T. String literals are const []u8.
- **Fix:** Added []T → *T coercion in `can_implicit_coerce`. Emitter appends `.ptr` at call site when passing slice to pointer param. Pure extern forward declarations (no body) skipped in emission to avoid <stdio.h> conflicts.
- **Test:** Hello World: `void puts(*u8 s); puts("Hello World");` compiles and runs

---

## OS/Kernel Pattern Session (2026-03-19)

### BUG-017: `orelse return` in `?T` function emitted `return 0` instead of `return (?T){0,0}`
- **Symptom:** `?u32 task_create() { Handle h = pool.alloc() orelse return; ... }` — GCC error, `return 0` incompatible with `_zer_opt_u32`
- **Root cause:** Orelse-return emission only checked for void vs non-void. Didn't distinguish `?T` return type needing `{0, 0}`.
- **Fix:** Added TYPE_OPTIONAL check in orelse-return emission path.
- **Test:** `ZER-Test/scheduler.zer` — Pool-based task scheduler

### BUG-018: `Ring(Struct).pop()` return causes GCC anonymous struct mismatch
- **Symptom:** `?Event poll_event() { return event_queue.pop(); }` — GCC error, two anonymous structs with same layout but different types
- **Root cause:** `?StructName` emitted as anonymous `struct { ... }` everywhere, creating incompatible types for same layout.
- **Fix:** Named typedef `_zer_opt_StructName` emitted after every struct declaration. `emit_type` for TYPE_OPTIONAL(TYPE_STRUCT) uses the named typedef.
- **Test:** `ZER-Test/event_queue.zer` — Ring(Event) with enum dispatch

### BUG-019: Assigning `u32` to `?u32` emitted bare value (no optional wrapping)
- **Symptom:** `?u32 best = null; best = some_value;` — GCC error, assigning `uint32_t` to `_zer_opt_u32`
- **Root cause:** NODE_ASSIGN emission had no T→?T wrapping logic.
- **Fix:** Added optional wrapping in NODE_ASSIGN: if target is `?T` and value is `T`, emit `(type){value, 1}`. For null, emit `{0, 0}`.
- **Test:** `ZER-Test/net_stack.zer` — routing table with `?u32 best_gateway`

---

## Multi-Module Session (2026-03-19)

### BUG-020: Imported module enums/unions not emitted in C output
- **Symptom:** `DeviceStatus.offline` in imported module → GCC error `'DeviceStatus' undeclared`
- **Root cause:** `emit_file_no_preamble` only handled NODE_STRUCT_DECL, NODE_FUNC_DECL, NODE_GLOBAL_VAR. Missing NODE_ENUM_DECL (#define constants) and NODE_UNION_DECL.
- **Fix:** Added enum #define emission, union struct emission, and extern forward-decl skipping to `emit_file_no_preamble`.
- **Test:** `ZER-Test/multi/driver.zer` — imports device.zer with enum DeviceStatus

### BUG-020.1: Emitter enum value fallback for imported modules
- **Symptom:** `DeviceStatus.offline` emitted as `DeviceStatus.offline` (invalid C) instead of `_ZER_DeviceStatus_offline` in imported module functions
- **Root cause:** `checker_get_type(node->field.object)` returned NULL for imported module nodes — typemap had no entries. Enum value detection in NODE_FIELD failed.
- **Fix:** Added scope_lookup fallback in NODE_FIELD: if checker_get_type returns NULL and object is NODE_IDENT, look up the identifier in global scope.
- **Test:** `ZER-Test/multi/driver.zer` — enum values in imported module functions

### BUG-021: Imported module function bodies never type-checked
- **Symptom:** `gpio.mode = mode` in imported function emitted `gpio.mode` (dot) instead of `gpio->mode` (arrow) — pointer auto-deref failed
- **Root cause:** Only `checker_check` was called on the main file. Imported modules only had `checker_register_file` (declarations only, no function bodies). Typemap had no entries for imported module expressions.
- **Fix:** Added `checker_check_bodies()` — checks function bodies without re-registering declarations. Called on all imported modules before main.
- **Test:** `ZER-Test/multi/firmware.zer` — imported HAL functions with pointer params

### BUG-022: Main module registered before imports → types undefined
- **Symptom:** `ErrCode init_system()` in main file → "undefined type 'ErrCode'" even though error.zer is imported
- **Root cause:** `checker_register_file` processed modules in order [main, imports...]. Main's function signatures resolved before imported types were in scope.
- **Fix:** Register imported modules first (loop from index 1), then main module (index 0).
- **Test:** `ZER-Test/multi/firmware.zer` — uses ErrCode from error.zer in function signature

---

## Edge Case Session (2026-03-19)

### BUG-023: Enum value rejected as array index
- **Symptom:** `arr[Color.red]` → "array index must be integer, got 'Color'"
- **Root cause:** `type_is_integer()` didn't include TYPE_ENUM. Enums are i32 internally but weren't recognized as integers.
- **Fix:** Added TYPE_ENUM to `type_is_integer`, `type_is_signed`, and `type_width` (32-bit signed).
- **Test:** `ZER-Test/edge_cases.zer` — enum as array index

### BUG-024: `??u32` (nested optional) accepted but emits invalid C
- **Symptom:** `??u32` compiles but emits anonymous struct wrapping another anonymous struct — GCC rejects
- **Root cause:** Checker's `resolve_type` for TYNODE_OPTIONAL didn't reject optional-of-optional
- **Fix:** Added check in resolve_type: if inner type is already TYPE_OPTIONAL, emit error "nested optional '??T' is not supported"
- **Test:** `ZER-Test/test_opt_opt.zer` — rejected at compile time

---

## Spec Audit — Missing Features (2026-03-20)

### BUG-025: Function pointer declarations not parseable
- **Symptom:** `void (*callback)(u32 event);` fails to parse — "expected expression" error. Spec §13 vtable pattern impossible to write.
- **Root cause:** Parser had `/* TODO: function pointer declarations */` at line 1121. AST node `TYNODE_FUNC_PTR`, type system, checker, and emitter all supported function pointers, but the parser never created the node. No call site (struct fields, var decls, parameters, top-level) handled `type (*name)(params...)` syntax.
- **Fix:** Added `parse_func_ptr_after_ret()` helper. Added function pointer detection at 4 sites: `parse_func_or_var` (global), `parse_var_decl` (local), struct field parsing, and function parameter parsing. Fixed `emit_type_and_name` to emit correct C syntax `ret (*name)(params)`. Added lookahead in statement parser to detect `type (* ...` as var decl.
- **Test:** `test_emit.c` — 6 E2E tests (local var, reassign, parameter, struct field vtable, global, callback registration). `test_parser_edge.c` — 5 parser tests.

### BUG-026: `arena.alloc(T)` returns `void` instead of `?*T`
- **Symptom:** `Arena(1024) a; ?*Task t = a.alloc(Task);` — type checker accepts but emitter produces invalid C. `alloc()` resolved to `void` return type, so the optional wrapping was wrong.
- **Root cause:** Checker's builtin method handler for `alloc` on Arena types returned `ty_void` unconditionally. It didn't resolve the type argument from the call's `NODE_IDENT` arg via `scope_lookup`.
- **Fix:** Added type resolution in the `alloc` method handler: look up the type name argument via `scope_lookup`, then return `type_optional(type_pointer(sym->type))` — i.e., `?*T`.
- **Test:** `test_checker_full.c` — arena alloc type resolution

---

## Comprehensive Audit — Bugs 027-035 (2026-03-21)

### BUG-027: `arena.alloc_slice(T, n)` returns `void` instead of `?[]T`
- **Symptom:** Same class as BUG-026. `alloc_slice` placeholder in NODE_FIELD returned `ty_void`, but no NODE_CALL handler existed to resolve the actual type.
- **Root cause:** Missing `alloc_slice` handler in checker.c NODE_CALL Arena methods section.
- **Fix:** Added `alloc_slice` handler: look up type arg via `scope_lookup`, return `type_optional(type_slice(sym->type))`.
- **Test:** `test_checker_full.c` — arena alloc_slice type resolution

### BUG-028: `type_name()` single static buffer corrupts error messages
- **Symptom:** `"expected %s, got %s", type_name(a), type_name(b)` prints the same type for both — second call overwrites first buffer.
- **Root cause:** Single `type_name_buf[256]` used by all calls.
- **Fix:** Two alternating buffers (`type_name_buf0`, `type_name_buf1`) with a toggle counter.
- **Test:** Implicit — all checker error messages with two types now display correctly.

### BUG-029: `?void` bare return emits `{ 0, 1 }` for single-field struct
- **Symptom:** `_zer_opt_void` has only `has_value` field, but `return;` in `?void` function emitted `{ 0, 1 }` (2 initializers). GCC: "excess elements in struct initializer".
- **Root cause:** Return emission didn't distinguish `?void` from other `?T` types.
- **Fix:** Check if inner type is `TYPE_VOID` — emit `{ 1 }` for bare return, `{ 0 }` for return null. Also fixed if-unwrap to not access `.value` on `?void`.
- **Test:** `test_emit.c` — ?void bare return and return null E2E tests

### BUG-030: `?bool` has no named typedef
- **Symptom:** `?bool` fell to anonymous struct fallback in `emit_type`, causing type mismatch when mixing `?bool` values.
- **Root cause:** Missing `TYPE_BOOL` case in optional typedef switch.
- **Fix:** Added `_zer_opt_bool` typedef in preamble and `TYPE_BOOL` case in `emit_type`.
- **Test:** `test_emit.c` — ?bool function returning and unwrapping

### BUG-031: `@saturate` for signed types was just a C cast (UB)
- **Symptom:** `@saturate(i8, 200)` emitted `(int8_t)_zer_sat0` — undefined behavior if value out of range.
- **Root cause:** Signed path had "just cast for now" placeholder.
- **Fix:** Proper min/max clamping ternaries per signed width (i8: -128..127, i16: -32768..32767, i32: full range). Also fixed unsigned u32/u64 path that had broken control flow.
- **Test:** `test_emit.c` — @saturate(i8, 200)=127, @saturate(u8, 300)=255

### BUG-032: Optional var init with NODE_IDENT skips wrapping
- **Symptom:** `?u32 x = some_u32_var;` emitted without `{val, 1}` wrapper — GCC type mismatch.
- **Root cause:** Emitter assumed NODE_IDENT init "might already be ?T" and skipped wrapping unconditionally.
- **Fix:** Use `checker_get_type` to check if ident is already optional. If not, wrap it.
- **Test:** `test_emit.c` — ?u32 from plain u32 var and from optional var

### BUG-033: Float literal `%f` loses precision
- **Symptom:** `f64 pi = 3.141592653589793;` emitted as `3.141593` (6 decimal places).
- **Root cause:** `emit(e, "%f", ...)` default precision.
- **Fix:** Changed to `"%.17g"` for full double round-trip precision.
- **Test:** `test_emit.c` — f64 precision check

### BUG-034: `emit_type` for TYPE_FUNC_PTR produces incomplete C
- **Symptom:** Direct `emit_type` call for func ptr emitted `ret (*` with no parameter list or closing paren.
- **Root cause:** `emit_type` left name and params to caller, but not all callers use `emit_type_and_name`.
- **Fix:** `emit_type` now emits complete anonymous func ptr type: `ret (*)(params...)`.
- **Test:** `test_emit.c` — func ptr as parameter compiles correctly

### BUG-035: ZER-CHECK if/else merge false positives
- **Symptom:** Handle freed on only ONE branch of if/else was marked as FREED — false positive for subsequent use.
- **Root cause:** Merge condition used `||` (either branch) instead of `&&` (both branches).
- **Fix:** Only mark freed if freed on BOTH branches (under-approximation per design doc). Also added switch arm merge with ALL-arms-must-free logic. Added NODE_INTERRUPT body checking.
- **Test:** `test_zercheck.c` — one-branch free OK, both-branch use-after-free detected, switch merge tests

### Pool/Ring scope fix
- **Symptom:** Pool/Ring builtin method emission only looked up `global_scope`, breaking for local variables.
- **Root cause:** Emitter and zercheck used `scope_lookup(global_scope, ...)` only.
- **Fix:** Try `checker_get_type` first (works for any scope), fall back to global_scope.
- **Test:** Implicit — all existing Pool/Ring tests pass with new lookup path

## Arena E2E + Gap Fixes (2026-03-21)

### Arena E2E emission (feature)
- **Symptom:** Arena methods (alloc, alloc_slice, over, reset) type-checked but emitter output literal method calls → GCC rejected.
- **Root cause:** Emitter had no Arena method interception — Pool and Ring had it, Arena didn't.
- **Fix:** Added `_zer_arena` typedef + `_zer_arena_alloc()` runtime helper in preamble. Added method emission for `Arena.over(buf)`, `arena.alloc(T)`, `arena.alloc_slice(T, n)`, `arena.reset()`, `arena.unsafe_reset()`. Added `TOK_ARENA` in parser expression context. Added "Arena" symbol in checker global scope.
- **Test:** `test_emit.c` — 5 Arena E2E tests (alloc, alloc_slice, reset, exhaustion, multiple allocs)

### BUG-036: Slice indexing emits `slice[i]` instead of `slice.ptr[i]`
- **Symptom:** Indexing a `[]T` slice variable emitted `items[0]` — GCC rejected because `items` is a struct, not an array.
- **Root cause:** `NODE_INDEX` emission in `emit_expr` didn't check if object was a slice type.
- **Fix:** Added `TYPE_SLICE` check in NODE_INDEX: emit `.ptr` suffix when indexing a slice.
- **Test:** `test_emit.c` — arena.alloc_slice exercises slice indexing

### BUG-037: Slice `orelse return` unwrap uses anonymous struct incompatible types
- **Symptom:** `[]Elem items = expr orelse return;` → GCC error: "invalid initializer" — two distinct anonymous structs treated as incompatible.
- **Root cause:** Var decl orelse unwrap emitted `struct { T* ptr; size_t len; } items = _zer_or0.value;` — GCC treats the anonymous struct in the optional and the declared type as different types.
- **Fix:** Use `__auto_type` for slice type unwrap to inherit the exact type from the optional's `.value`.
- **Test:** `test_emit.c` — arena.alloc_slice with orelse return

### BUG-038: `?void orelse return` accesses non-existent `.value` field
- **Symptom:** `push_checked(x) orelse return;` → GCC error: `_zer_opt_void has no member named 'value'`.
- **Root cause:** Expression-level NODE_ORELSE handler emitted `_zer_tmp.value` for all non-pointer optionals, but `_zer_opt_void` is `{ has_value }` only — no value field.
- **Fix:** Added `is_void_optional` check in NODE_ORELSE expression handler. For `?void orelse return/break/continue`, emit inline `if (!has_value) { return; }` instead of extracting `.value`.
- **Test:** `test_emit.c` — ring.push_checked orelse return

### ring.push_checked() emission (feature)
- **Symptom:** `ring.push_checked(val)` type-checked as `?void` but emitter had no handler → fell through to generic call emission → GCC rejected.
- **Root cause:** Missing emitter case for push_checked alongside push and pop.
- **Fix:** Added `push_checked` handler in Ring method emission block. Checks `count < capacity` before pushing; returns `_zer_opt_void` with `has_value=1` on success, `{0}` on full.
- **Test:** `test_emit.c` — push_checked success + push_checked full ring returns null

### @container E2E test (test coverage)
- **Symptom:** `@container(*T, ptr, field)` had emitter implementation but no E2E test.
- **Fix:** Added E2E test: recover `*Node` from `&n.y` using @container, verify field access.
- **Test:** `test_emit.c` — @container recover Node from field pointer

### BUG-039: Arena alignment uses fixed `sizeof(void*)` instead of type alignment
- **Symptom:** `arena.alloc(T)` always aligned to pointer width. Types requiring stricter alignment (or smaller types wasting space on lax alignment) not handled.
- **Root cause:** `_zer_arena_alloc` hardcoded `sizeof(void*)` as alignment.
- **Fix:** Added `align` parameter to `_zer_arena_alloc()`. Call sites now pass `_Alignof(T)` for natural type alignment. ARM Cortex-M0 unaligned access faults are prevented.
- **Test:** `test_emit.c` — alloc Byte(u8) then Word(u32), verify Word is accessible (would fault on strict-alignment targets without fix)

### BUG-040: Signed integer overflow is undefined behavior in emitted C
- **Symptom:** ZER spec says `i32` overflow wraps. But emitted C uses raw `int32_t + int32_t` which is UB in C99. GCC at `-O2` can optimize assuming no signed overflow, breaking ZER's wrapping guarantee.
- **Root cause:** Emitter outputs plain arithmetic operators without wrapping protection.
- **Fix:** Added `-fwrapv` to GCC invocation in `zerc --run` and test harness. Added compile hint in emitted C preamble. This makes GCC treat signed overflow as two's complement wrapping, matching ZER semantics.
- **Test:** `test_emit.c` — `i8 x = 127; x = x + 1;` wraps to -128, bitcast to u8 = 128

### BUG-068: Explicit enum values (`enum { a = 5 }`) silently emit wrong constants
- **Symptom:** `enum Prio { low = 1, med = 5, high = 10 }` emits `#define _ZER_Prio_low 0`, `_ZER_Prio_med 1`, `_ZER_Prio_high 2` — uses loop index instead of declared value.
- **Root cause:** Emitter's enum `#define` loop uses `j` (loop counter) as the value, ignoring `v->value`.
- **Fix:** Rejected at parser level with "explicit enum values not yet supported" — prevents silent wrong behavior. Proper support deferred to v0.2.

### BUG-067: `*Union` pointer auto-deref returns `ty_void` in checker
- **Symptom:** `*Msg p = &msg; p.sensor = s;` fails with "cannot assign 'S' to 'void'" — checker doesn't auto-deref pointers to unions.
- **Root cause:** Pointer auto-deref path (line 982) only handled `TYPE_POINTER` where inner is `TYPE_STRUCT`, not `TYPE_UNION`.
- **Fix:** Added parallel auto-deref block for `TYPE_UNION` inner — looks up variant by name, returns variant type.

### BUG-066: Var-decl `orelse return` in `?void` function emits `{ 0, 0 }`
- **Symptom:** `u32 val = get() orelse return;` inside a `?void` function emits `return (_zer_opt_void){ 0, 0 };` — excess initializer for 1-field struct.
- **Root cause:** Var-decl orelse-return path had no `TYPE_VOID` check (the other 3 paths had it).
- **Fix:** Added `inner->kind == TYPE_VOID` → `{ 0 }` instead of `{ 0, 0 }`.

### BUG-065: Union switch `|*v|` mutable capture emits value copy
- **Symptom:** `switch (m) { .sensor => |*v| { v.temp = 99; } }` — mutation silently dropped. Emitted C copies the variant value, mutations go to the copy.
- **Root cause:** Capture always emitted `__auto_type v = union.field` regardless of `capture_is_ptr`.
- **Fix:** When `capture_is_ptr`, emit `__auto_type *v = &union.field` instead.

### BUG-064: `volatile` qualifier completely stripped from emitted C
- **Symptom:** `volatile *u32 reg = @inttoptr(...)` emits as `uint32_t* reg` — no volatile keyword. GCC optimizes away MMIO reads/writes.
- **Root cause:** Parser consumes `volatile` as a var-decl flag (`is_volatile`), not as part of the type node. Emitter never checked `is_volatile` to emit the keyword.
- **Fix:** `emit_global_var` and `emit_stmt(NODE_VAR_DECL)` propagate `is_volatile` to pointer type. `emit_type(TYPE_POINTER)` emits `volatile` prefix when `is_volatile` is set.

### BUG-063: Expression-level `orelse return/break/continue` skips defers
- **Symptom:** `defer cleanup(); get_val() orelse return;` — cleanup never called because the expression-level orelse handler emits `return` without `emit_defers()`.
- **Root cause:** Var-decl orelse path had `emit_defers()` but expression-level path in `emit_expr(NODE_ORELSE)` did not.
- **Fix:** Added `emit_defers()` before return and `emit_defers_from()` before break/continue in both void and non-void expression orelse paths.

### BUG-062: `?UnionType` optional emits anonymous struct — GCC type mismatch
- **Symptom:** `?Msg` (optional union) emits anonymous `struct { ... }` at each use — incompatible types.
- **Root cause:** `emit_type(TYPE_OPTIONAL)` had no `case TYPE_UNION:`. Union declarations didn't emit `_zer_opt_UnionName` typedef.
- **Fix:** Added `case TYPE_UNION:` → `_zer_opt_UnionName`. Added typedef emission after union declarations.

### BUG-061: Compound `u8 += u64` accepted — silent narrowing
- **Symptom:** Compound assignment didn't check type width compatibility. `u8 += u64` silently truncated.
- **Root cause:** Compound assignment only checked `type_is_numeric()`, not width compatibility.
- **Fix:** Added narrowing check (reject when value wider than target), with literal exemption (`u8 += 1` is fine).

### BUG-060: Const capture field mutation bypasses const check
- **Symptom:** `if (opt) |pt| { pt.x = 99; }` accepted — const-captured struct field modified.
- **Root cause:** Const check only examined `NODE_IDENT` targets, not field/index chains.
- **Fix:** Walk field/index chain to root ident, check const. Allow mutation through pointers (auto-deref).

### BUG-059: `@truncate`/`@saturate` accept non-numeric source
- **Symptom:** `@truncate(u8, some_struct)` accepted — struct passed to truncate.
- **Root cause:** No source type validation in intrinsic handlers.
- **Fix:** Validate source is numeric (unwrap distinct types before checking).

### BUG-058: Union switch arm variant names never validated
- **Symptom:** `.doesnt_exist =>` in union switch accepted — nonexistent variant.
- **Root cause:** Union switch arms skipped name validation entirely.
- **Fix:** Validate each arm's variant name against the union's variant list.

### BUG-057: Union switch exhaustiveness counts duplicates
- **Symptom:** `.sensor, .sensor =>` counts as 2 handled, hiding missing `.command`.
- **Root cause:** Raw `value_count` sum instead of unique variant tracking.
- **Fix:** Bitmask-based deduplication (same approach as enum fix BUG-048).

### BUG-056: Bitwise compound `&= |= ^= <<= >>=` accepted on floats
- **Symptom:** `f32 x = 1.0; x &= 2;` compiles — GCC rejects the emitted C.
- **Root cause:** Compound assignment only checked `type_is_numeric()`, which includes floats.
- **Fix:** Added explicit check: bitwise compound ops require integer types.

### BUG-055: `@cast` — parser excluded TOK_IDENT from type_arg
- **Symptom:** `@cast(Fahrenheit, c)` fails — checker returns ty_void because type_arg is NULL.
- **Root cause:** Parser's `is_type_token && type != TOK_IDENT` guard excluded all named types from being parsed as type_arg.
- **Fix:** Added `force_type_arg` for `@cast` intrinsic, allowing TOK_IDENT to be parsed as type.

### BUG-054: Array-to-slice coercion missing at call sites, var-decl, and return
- **Symptom:** `process(buf)` where buf is `u8[N]` and param is `[]u8` — GCC type mismatch.
- **Root cause:** Emitter passed raw array pointer instead of wrapping in slice compound literal.
- **Fix:** Added `emit_array_as_slice()` helper. Applied at 3 sites: call args, var-decl init, return.

### BUG-053: Slice-of-slice missing `.ptr` + open-end slice on slices
- **Symptom:** `data[1..3]` on a `[]u8` parameter emits `&(data)[1]` — subscript on struct.
- **Root cause:** Slice emission didn't add `.ptr` for slice-type objects. Open-end `slice[start..]` emitted length `0`.
- **Fix:** Added `.ptr` when object is TYPE_SLICE. Added `slice.len - start` for open-end on slices.

### BUG-052: `?T orelse return` as expression — guard completely missing
- **Symptom:** `get_val() orelse return;` emits `({ auto t = expr; t.value; })` — no guard, no return.
- **Root cause:** Non-void, non-pointer path in expression-level orelse handler extracted `.value` unconditionally.
- **Fix:** Added `if (!has_value) { return; }` guard with correct return type wrapping.

### BUG-051: `?void` var-decl null init emits wrong initializer
- **Symptom:** `?void x = null;` (global) emits `= 0` (scalar for struct). Local emits `{ 0, 0 }` (2 fields for 1-field struct).
- **Root cause:** Global path called `emit_expr(NULL_LIT)` which emits scalar 0. Local path didn't check for TYPE_VOID.
- **Fix:** Both paths now check `inner == TYPE_VOID` → emit `{ 0 }`.

### BUG-050: `@bitcast` accepts mismatched widths
- **Symptom:** `@bitcast(i64, u32_val)` accepted — spec requires same width.
- **Root cause:** No width validation in checker's @bitcast handler.
- **Fix:** Compare `type_width(target)` vs `type_width(source)`, error if different.

### BUG-049: Bool switch checks arm count, not actual coverage
- **Symptom:** `switch (x) { true => {} true => {} }` accepted — false never handled.
- **Root cause:** Checked `arm_count < 2` instead of tracking which values are covered.
- **Fix:** Track `has_true`/`has_false` flags from actual arm values.

### BUG-048: Enum switch exhaustiveness tricked by duplicate variants
- **Symptom:** `.idle, .idle =>` counts as 2, masking missing variants.
- **Root cause:** Raw `value_count` sum instead of unique variant tracking.
- **Fix:** Bitmask-based deduplication — each variant index tracked as a bit.

### BUG-047: `bool x = 42` accepted — int literal coerces to bool
- **Symptom:** Integer literal assigned to bool variable without error.
- **Root cause:** `is_literal_compatible` had `NODE_INT_LIT && TYPE_BOOL → true`.
- **Fix:** Removed that rule. Only `true`/`false` literals can initialize bool.

### BUG-046: `@trap()` rejected as unknown intrinsic
- **Symptom:** `@trap()` fails with "unknown intrinsic '@trap'" at checker.
- **Root cause:** Checker had no handler for `@trap` — fell to the `else` branch that reports unknown intrinsics.
- **Fix:** Added `@trap` handler in checker (returns `ty_void`) and emitter (emits `_zer_trap("explicit trap", __FILE__, __LINE__)`).
- **Test:** `test_emit.c` — conditional @trap skipped = 42

### BUG-045: Non-u8/u32 array slicing emits `void*` pointer — type mismatch
- **Symptom:** `u32[8]` sliced with `arr[0..3]` emits `struct { void* ptr; size_t len; }`, incompatible with `_zer_slice_u32`.
- **Root cause:** Slice emission only checked for u8, everything else got `void*` and an anonymous struct.
- **Fix:** Added u32 check → `_zer_slice_u32`. Other types use typed pointer instead of `void*`.
- **Test:** `test_emit.c` — u32 array slicing arr[0..3] → []u32, sum = 60

### BUG-044: Slice variables auto-zero emits `= 0` instead of `= {0}`
- **Symptom:** `[]u8 s;` (global or local) emits `_zer_slice_u8 s = 0;` — invalid initializer for struct.
- **Root cause:** `TYPE_SLICE` missing from the compound-type condition in both local and global auto-zero paths.
- **Fix:** Added `TYPE_SLICE` to both conditions.
- **Test:** `test_emit.c` — global+local slice auto-zero, len=0

### BUG-043: `?void` assign null emits `{ 0, 0 }` — excess initializer
- **Symptom:** `status = null;` where status is `?void` emits `(_zer_opt_void){ 0, 0 }` — 2 initializers for 1-field struct.
- **Root cause:** Assign-null path didn't special-case `?void` (which has only `has_value`, no `value` field).
- **Fix:** Check `inner->kind == TYPE_VOID` → emit `{ 0 }` instead of `{ 0, 0 }`.
- **Test:** `test_emit.c` — ?void assign null, has_value=0

### BUG-042: `?Enum` optional emits anonymous struct — GCC type mismatch
- **Symptom:** `?Status` (optional enum) emits `struct { int32_t value; uint8_t has_value; }` everywhere. Each anonymous struct is a different C type, causing "incompatible types" errors on return and assignment.
- **Root cause:** `emit_type` TYPE_OPTIONAL handler had no `case TYPE_ENUM:` — fell to `default` anonymous struct fallback.
- **Fix:** Added `case TYPE_ENUM: emit("_zer_opt_i32");` since enums are int32_t underneath.
- **Test:** `test_emit.c` — enum switch inside if-unwrap: `?Status` returned from function, unwrapped, switched on

### BUG-041: Bit extraction `[31..0]` emits `1u << 32` — undefined behavior
- **Symptom:** Full-width bit extraction `x[31..0]` on u32 emits `(1u << 32)` which is UB (shift equals type width).
- **Root cause:** Mask formula used `1u` (32-bit) which overflows when shift count reaches 32.
- **Fix:** Changed `1u` to `1ull` (64-bit) so shifts up to 63 are safe.
- **Test:** `test_emit.c` — `[0..0]` single bit, `[7..0]` low byte, `[15..8]` mid-range
