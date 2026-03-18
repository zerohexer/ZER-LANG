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
