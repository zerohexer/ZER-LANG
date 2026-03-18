# Type Checker Implementation Plan

Mental walkthrough of every component, edge case, and interaction point.
Purpose: increase confidence from 75% to 95% before writing code.

---

## PASS 1: Type Checker (runs on AST, produces typed AST)

### Component 1: Type Representation (~100 lines)

Already designed in zer-type-system.md. The Type struct is a tagged union tree.
Arena-allocated — one big arena, never free individually.

**Edge cases worked through:**
- `?*Task` = Optional { inner: Pointer { inner: Struct("Task") } }
- `?[]u8` = Optional { inner: Slice { inner: U8 } }
- `?void` = Optional { inner: Void } — special: 1-byte flag, no value
- `Pool(Task, 8)` = Pool { elem: Struct("Task"), count: 8 }
- `Handle(Task)` = Handle { elem: Struct("Task") }

**How to implement:** Direct translation of the spec's struct into C. Allocator is a bump arena.
Confidence: 99%

---

### Component 2: Symbol Table + Scope Chain (~200 lines)

```
Scope {
    parent: *Scope       // NULL for module level
    symbols: []Symbol    // dynamic array
    module_name: *char   // non-NULL for module scopes
}

Symbol {
    name, name_len, type
    is_keep, is_const, is_static
    file, line
}
```

**Operations:**
- `push_scope()` — new scope, parent = current
- `pop_scope()` — current = current.parent
- `lookup(name)` — walk chain upward, then check imports
- `add_symbol(name, type)` — add to current, check for redefinition in SAME scope

**Edge cases worked through:**
- Shadowing: inner scope redefines `x` → OK (not same scope)
- Redefinition: same scope defines `x` twice → COMPILE ERROR
- Import ambiguity: `init` exists in both uart and spi → COMPILE ERROR, must qualify
- For loop variable: `for (u32 i = ...)` → i scoped to loop body (push scope before, pop after)

**How to implement:** Linked list of scopes. Linear search within scope (small scope = fast).
Confidence: 98%

---

### Component 3: Integer Coercion Rules (~80 lines)

```
WIDENING (implicit, always safe):
  u8 → u16 → u32 → u64
  i8 → i16 → i32 → i64
  u8 → u16 can also go to i16, i32, i64 (fits)

NARROWING (explicit only):
  u32 → u16 → COMPILE ERROR, must @truncate or @saturate

SIGNED/UNSIGNED cross:
  i32 → u32 → COMPILE ERROR, must @bitcast

USIZE:
  == u32 on 32-bit, == u64 on 64-bit
  array .len is usize
```

**Implementation approach:**
```c
int type_width(Type *t) — returns bit width (8, 16, 32, 64)
bool type_is_signed(Type *t) — true for i8..i64
bool can_implicit_coerce(Type *from, Type *to) {
    if (same type) return true;
    if (both integer, same signedness, from.width < to.width) return true;
    if (from unsigned, to signed, from.width < to.width) return true;
    // array T[N] to slice []T
    if (from is array, to is slice, same elem type) return true;
    // mutable to const
    return false;
}
```

**Edge cases worked through:**
- `u8 → i8` — NO (same width, different sign)
- `u8 → i16` — YES (8 fits in 16 signed)
- `u8 → u32` — YES
- `i32 → u64` — NO (signed to unsigned, must be explicit)
- `f32 → f64` — YES (widening)
- `f64 → f32` — NO (narrowing)
- `u32 → f32` — NO (different category entirely)

Confidence: 97%

---

### Component 4: Expression Type Checking (~400 lines)

The core. Every expression resolves to a Type. Walk AST recursively.

**Binary expressions:**
```
type_check_binary(op, left, right):
  left_type = type_check(left)
  right_type = type_check(right)

  arithmetic (+, -, *, /, %):
    both must be numeric
    result type = common type (widen smaller to larger)
    if no common type → COMPILE ERROR

  comparison (==, !=, <, >, <=, >=):
    both must be same type (or coercible)
    result type = bool

  logical (&&, ||):
    both must be bool
    result type = bool

  bitwise (&, |, ^, <<, >>):
    both must be integer
    result type = common integer type

  assignment (=, +=, -=, etc.):
    LHS must be assignable (not const, not non-storable)
    RHS type must match LHS type (or be coercible)
    for compound: same rules as the underlying op
```

**Unary expressions:**
```
  !expr → expr must be bool, result bool
  -expr → expr must be numeric, result same type
  ~expr → expr must be integer, result same type
  *expr → expr must be pointer, result = pointed-to type
  &expr → result = pointer to expr's type
```

**Function calls:**
```
  resolve function name → get function type (params + return)
  check arg count matches
  check each arg type matches param type (with coercion)
  result type = function's return type
```

**Field access (a.b):**
```
  1. resolve a's type
  2. if struct → look up field b → result = field type
  3. if builtin (Pool/Ring/Arena) → dispatch table
  4. else → try UFCS: look for fn b(*typeof(a), ...)
  5. none found → COMPILE ERROR
```

**Array indexing (a[i]):**
```
  a must be array or slice
  i must be integer (usize preferred)
  result = element type
  (bounds check insertion happens in safety pass, not type checker)
```

**Slice expression (a[i..j]):**
```
  a must be array or slice
  i, j must be integer
  result = slice of element type
```

**Edge cases worked through:**
- `tasks.get(h).priority` — get returns *Task (non-storable), but field access through it is OK (used immediately). Non-storable only blocks ASSIGNMENT to a variable.
- `a + b` where a is u8, b is u32 → widen a to u32, result u32
- `*reg = 0xFF` where reg is `volatile *u32` → dereference volatile pointer, assign u32. Volatile doesn't affect type checking, only codegen.

Confidence: 92%

---

### Component 5: Optional Unwrapping (~150 lines)

Five patterns, all follow the same core logic:

**Core unwrap function:**
```c
Type *unwrap_optional(Type *t) {
    assert(t->kind == TYPE_OPTIONAL);
    return t->optional.inner;  // ?T → T
}

Type *capture_type(Type *unwrapped, bool is_pointer_capture) {
    if (is_pointer_capture) {
        // |*val| → pointer to the unwrapped value
        return make_pointer_type(unwrapped);
    }
    // |val| → copy of unwrapped value
    return unwrapped;
}
```

**Pattern 1 & 2: if-unwrap**
```
type_check_if_unwrap(cond_expr, capture, body, else_body):
  cond_type = type_check(cond_expr)
  if cond_type is not ?T → COMPILE ERROR
  unwrapped = cond_type.optional.inner
  cap_type = capture_type(unwrapped, capture.is_pointer)
  push_scope()
  add_symbol(capture.name, cap_type, is_const = !capture.is_pointer)
  type_check(body)
  pop_scope()
  if (else_body) type_check(else_body)
```

**Pattern 3: switch on tagged union**
```
type_check_switch_union(expr, arms):
  expr_type = type_check(expr)
  if expr_type is not union → handle other switch types
  handled_variants = {}
  for each arm:
    variant = lookup variant name in union type
    if capture:
      cap_type = capture_type(variant.type, capture.is_pointer)
      push_scope()
      add_symbol(capture.name, cap_type, is_const = !capture.is_pointer)
      type_check(arm.body)
      pop_scope()
    handled_variants.add(variant.name)
  check all variants handled → if not, COMPILE ERROR
```

**Pattern 4 & 5: orelse**
```
type_check_orelse(lhs, rhs):
  lhs_type = type_check(lhs)
  if lhs_type is not ?T → COMPILE ERROR
  unwrapped = lhs_type.optional.inner

  if rhs is return/break/continue:
    result type = unwrapped  // flow diverts, expression yields T
  else if rhs is block:
    type_check(block)
    result type = unwrapped  // statement-only, block has no result
    // ERROR if used as expression value
  else:
    rhs_type = type_check(rhs)
    check rhs_type == unwrapped (or coercible)
    result type = unwrapped
```

**Edge cases worked through:**
- `?void` with `|val|` — val type is void. Can't use val for anything. Presence check only. The capture is syntactically valid but the body can't reference val meaningfully.
- `?*Task` with `|val|` — val is `*Task` (copy of pointer). Can write through: `val.priority = 5` OK.
- `?*Task` with `|*val|` — val is `**Task`. Rarely useful.
- `?u32` with `|*val|` — val is `*u32`. Can write: `*val = 99`. Modifies original.
- `orelse { block }` used as expression → COMPILE ERROR.
- Nested: `??T` — spec says "decide when encountered." For now, reject.

Confidence: 93%

---

### Component 6: UFCS Resolution (~100 lines)

```
resolve_method_call(expr, method_name, args):
  expr_type = type_check(expr)

  // Step 1: builtin check
  if expr_type is Pool/Ring/Arena:
    result = builtin_dispatch(expr_type, method_name, args)
    if found → return result

  // Step 2: field check
  if expr_type is struct:
    field = lookup_field(expr_type, method_name)
    if found:
      if field.type is func_ptr → this is a field call
      else → this is field access (not a call)
      // BUT: if ALSO found as free function → COMPILE ERROR: ambiguous
      return field.type

  // Step 3: UFCS lookup
  // Search for fn method_name(*typeof(expr), args...)
  fn = lookup_function(method_name)
  if found:
    // check first param type matches *typeof(expr)
    // rewrite: expr.method(args) → method(&expr, args)
    return fn.return_type

  // Step 4: nothing found
  COMPILE ERROR: no method 'method_name' on type X
```

**Edge cases worked through:**
- `t.run()` where `run` is both a field and a free function → COMPILE ERROR: ambiguous
- `t.priority` → field access, not UFCS (no args, and it's a data field)
- `t.run()` across modules — uart has `run(*Task)`, spi has `run(*Task)` → COMPILE ERROR: ambiguous. Must qualify: `uart.run(&t)`.
- Chained UFCS: `t.validate().process()` — validate returns some type, process is UFCS on that type. Works naturally.

Confidence: 94%

---

### Component 7: Builtin Dispatch (~185 lines)

Hardcoded dispatch table. Simple switch statement.

```c
Type *check_builtin_method(Type *receiver, char *method, Args *args) {
    switch (receiver->kind) {
    case TYPE_POOL:
        if (streq(method, "alloc")) {
            check_arg_count(args, 0);
            // return ?Handle(T) where T = receiver->pool.elem
            return make_optional(make_handle(receiver->pool.elem));
        }
        if (streq(method, "get")) {
            check_arg_count(args, 1);
            check_arg_type(args[0], make_handle(receiver->pool.elem));
            Type *result = make_pointer(receiver->pool.elem);
            result->non_storable = true;  // SPECIAL RULE
            return result;
        }
        if (streq(method, "free")) {
            check_arg_count(args, 1);
            check_arg_type(args[0], make_handle(receiver->pool.elem));
            // signal dataflow: args[0] is consumed
            mark_consumed(args[0]);
            return &type_void;
        }
        break;

    case TYPE_RING:
        // push, push_checked, pop — similar pattern
        break;

    case TYPE_ARENA:
        // over, alloc, alloc_slice, reset, unsafe_reset
        // reset: check if inside defer → warn if not
        break;
    }
    return NULL; // not a builtin method
}
```

Confidence: 96%

---

## PASS 2: Dataflow Analysis (runs AFTER type checking, on typed AST)

### Component 8: Handle Consumption Tracking (~300 lines)

This is the hardest part. But the spec's design makes it tractable.

**Architecture:**
```
1. Build CFG from typed AST
   - Basic block = sequence of statements, no branches
   - For each function, build a list of basic blocks with edges

2. Assign bit positions
   - Scan function for Handle variables → each gets a bit
   - Bitvector size = number of Handle variables (usually 1-3)

3. Forward propagation
   - Each basic block has an entry state and exit state (bitvectors)
   - Process statements in order:
     - free(h) → set bit for h
     - use of h → check bit → ERROR if set

4. Branch merging
   - if/else: merged state = entry_bit OR (then_exit AND else_exit)
   - Actually: for each bit, if consumed in ANY branch → MAYBE
   - if consumed in ALL branches → DEFINITELY consumed
   - MAYBE + use → "h might be consumed (freed on line X)"
   - DEFINITELY + use → "h consumed (freed on line X)"
```

**Simplification the spec enables:**
- Handle variables are LOCAL only (not in arrays/structs for compile-time)
- Handles in arrays/structs → runtime generation counter catches it
- So we only track simple local variables, not arbitrary expressions
- "Passed to function" → conservatively mark as POTENTIALLY consumed
- This means: scan function's local Handle vars, track their state

**Edge cases worked through:**
```
// Simple:
Handle(Task) h = tasks.alloc() orelse return;
tasks.free(h);
tasks.get(h);  // ERROR: h consumed on line above

// Branch:
if (condition) { tasks.free(h); }
tasks.get(h);  // ERROR: h MIGHT be consumed

// Both branches:
if (condition) { tasks.free(h); } else { tasks.free(h); }
tasks.get(h);  // ERROR: h consumed

// Loop:
while (cond) { tasks.free(h); }  // ERROR: h might be freed twice
// Actually: first iteration frees, second iteration uses → caught

// Passed to function:
some_func(h);
tasks.get(h);  // ERROR: h potentially consumed
```

**Implementation sketch:**
```c
typedef struct {
    u64 consumed;      // bit = 1 means consumed
    u64 maybe;         // bit = 1 means maybe consumed
} HandleState;

void analyze_block(Block *b, HandleState *state) {
    for each statement in b:
        if stmt is free(h):
            if state->consumed & h_bit → ERROR: double free
            state->consumed |= h_bit;
        if stmt uses h:
            if state->consumed & h_bit → ERROR: use after free
            if state->maybe & h_bit → ERROR: might be consumed
        if stmt is call(h):
            state->maybe |= h_bit;  // conservative
}

HandleState merge(HandleState a, HandleState b) {
    // consumed in both = definitely consumed
    // consumed in one = maybe consumed
    HandleState result;
    result.consumed = a.consumed & b.consumed;
    result.maybe = (a.consumed | b.consumed) & ~result.consumed;
    return result;
}
```

Confidence: 85% → this is where most risk lives. The algorithm is straightforward but getting CFG construction right for all control flow (loops, nested if/else, break/continue, defer) requires careful work.

---

### Component 9: Scope Escape Analysis (~100 lines)

```
For each assignment and return:
  if RHS is &local_var:
    if LHS is global/static → ERROR: local escapes
    if LHS is return → ERROR: returning pointer to local
    if LHS is keep parameter → ERROR: local can't satisfy keep

Track storage class of each variable:
  STACK — local variable, dies on function return
  STATIC — static/global, lives forever
  POOL — allocated from Pool, lives until free()
```

**Implementation:**
```c
typedef enum { STORAGE_STACK, STORAGE_STATIC, STORAGE_POOL } StorageClass;

void check_assignment(Node *lhs, Node *rhs) {
    if (rhs is address_of && rhs.operand is local) {
        StorageClass lhs_storage = get_storage(lhs);
        if (lhs_storage == STORAGE_STATIC) {
            error("cannot store pointer to local in static variable");
        }
    }
}

void check_return(Node *return_expr) {
    if (return_expr is address_of && return_expr.operand is local) {
        error("cannot return pointer to local variable");
    }
}

void check_keep_arg(Node *arg, bool param_is_keep) {
    if (param_is_keep && get_storage(arg) == STORAGE_STACK) {
        error("local variable cannot satisfy keep parameter");
    }
}
```

Confidence: 95%

---

### Component 10: Non-Storable Check (~20 lines)

```c
void check_assignment(Node *lhs, Node *rhs) {
    if (rhs->non_storable) {
        error("cannot store result of get() — use inline");
    }
}
```

Applied when RHS is a `pool.get(h)` call. Simple flag check.

Confidence: 99%

---

### Component 11: Arena Reset Warning (~15 lines)

```c
void check_arena_reset(Node *call, Node *parent_chain) {
    // walk up parents looking for defer
    Node *p = parent_chain;
    while (p) {
        if (p->kind == NODE_DEFER) return;  // OK
        p = p->parent;
    }
    warning("arena.reset() outside defer may cause dangling pointers");
}
```

Confidence: 99%

---

## Summary of Confidence by Component

```
Component                          Lines    Confidence
------------------------------------------------------
1. Type representation             ~100     99%
2. Symbol table + scope chain      ~200     98%
3. Integer coercion rules          ~80      97%
4. Expression type checking        ~400     92%
5. Optional unwrapping             ~150     93%
6. UFCS resolution                 ~100     94%
7. Builtin dispatch                ~185     96%
8. Handle consumption (dataflow)   ~300     85%
9. Scope escape analysis           ~100     95%
10. Non-storable check             ~20      99%
11. Arena reset warning            ~15      99%
------------------------------------------------------
TOTAL                              ~1,650
WEIGHTED CONFIDENCE                ~93%
```

The remaining 7% risk is concentrated in:
- Handle consumption across complex control flow (nested loops, defer + break)
- CFG construction for ZER-specific constructs (orelse as flow control)
- Edge cases in expression type checking we haven't thought of yet

Mitigation: implement incrementally. Get primitives working first,
add optionals, add builtins, add dataflow last.
