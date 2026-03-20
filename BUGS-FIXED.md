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
