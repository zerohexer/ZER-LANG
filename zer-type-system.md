# ZER Type System Design — Core Decisions

**Status:** Lexer and parser complete (376 tests). AST shape known. Ready for type checker implementation.
**Purpose:** The 4 load-bearing decisions that determine the type checker's architecture.
**Rule:** These are the skeleton. Everything else is muscle added incrementally.

---

## Decision 1: Internal Type Representation — DECIDED: Recursive Tree

**Decision:** Tagged struct with pointer to inner type. Arena-allocated nodes. Nominal equality.

**Justification:** Every production compiler uses recursive type trees — GCC, Clang, Go, Zig, chibicc, TCC, lcc. No exceptions. Flat enums can't handle arbitrary nesting like `?[]?*Task`. The tree handles any combination naturally.

### Type node definition

```c
typedef enum {
    // primitives
    TYPE_VOID, TYPE_BOOL,
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_U64, TYPE_USIZE,
    TYPE_I8, TYPE_I16, TYPE_I32, TYPE_I64,
    TYPE_F32, TYPE_F64,

    // compound
    TYPE_POINTER,       // inner = pointed-to type
    TYPE_OPTIONAL,      // inner = wrapped type
    TYPE_SLICE,         // inner = element type
    TYPE_ARRAY,         // inner = element type + size
    TYPE_STRUCT,        // fields list
    TYPE_ENUM,          // variants list
    TYPE_UNION,         // variants list (tagged)
    TYPE_FUNC_PTR,      // params + return type
    TYPE_OPAQUE,        // *opaque — no inner type

    // builtins (Decision 4)
    TYPE_POOL,          // elem type + count
    TYPE_RING,          // elem type + count
    TYPE_ARENA,         // no parameters
    TYPE_HANDLE,        // elem type (matches Pool's elem)
} TypeKind;

typedef struct Type {
    TypeKind kind;
    union {
        struct { struct Type *inner; } pointer;     // *T
        struct { struct Type *inner; } optional;    // ?T
        struct { struct Type *inner; } slice;       // []T
        struct { struct Type *inner; u32 size; } array;  // T[N]
        struct {
            struct StructField *fields;
            u32 field_count;
            const char *name;           // nominal identity
        } struct_type;
        struct {
            struct EnumVariant *variants;
            u32 variant_count;
            const char *name;
        } enum_type;
        struct {
            struct UnionVariant *variants;
            u32 variant_count;
            const char *name;
        } union_type;
        struct {
            struct Type **params;
            u32 param_count;
            struct Type *ret;
        } func_ptr;
        struct { struct Type *elem; u32 count; } pool;    // Pool(T, N)
        struct { struct Type *elem; u32 count; } ring;    // Ring(T, N)
        struct { struct Type *elem; } handle;              // Handle(T)
        // arena has no fields — TYPE_ARENA is sufficient
    };
    // source location for error messages
    const char *defined_in_file;
    u32 defined_at_line;
} Type;

typedef struct StructField {
    const char *name;
    struct Type *type;
    bool is_keep;       // for keep pointer fields
} StructField;

typedef struct EnumVariant {
    const char *name;
    i32 value;          // explicit or auto-assigned
} EnumVariant;

typedef struct UnionVariant {
    const char *name;
    struct Type *type;  // payload type for this variant
} UnionVariant;
```

### How types compose

```
?[]u8 in the tree:

  Type { kind: TYPE_OPTIONAL
    inner: -> Type { kind: TYPE_SLICE
      inner: -> Type { kind: TYPE_U8 }
    }
  }

Pool(Task, 8) in the tree:

  Type { kind: TYPE_POOL
    elem: -> Type { kind: TYPE_STRUCT, name: "Task", ... }
    count: 8
  }

void (*cb)(i32, *opaque) in the tree:

  Type { kind: TYPE_FUNC_PTR
    params: [ -> TYPE_I32, -> TYPE_OPAQUE ]
    param_count: 2
    ret: -> TYPE_VOID
  }
```

### Type equality

Nominal equality — same definition = same type. Two structs with identical fields but different names are different types.

```
type_equals(a, b):
  if a.kind != b.kind → false

  primitives (U8, U32, etc.): kind match is sufficient

  pointer/optional/slice: recurse on inner
    type_equals(a.pointer.inner, b.pointer.inner)

  array: recurse on inner + compare size

  struct/enum/union: compare by identity (pointer equality)
    same definition = same pointer (allocated once per definition)
    different definitions with same fields = different types

  pool/ring: compare elem type + count
  handle: compare elem type
  func_ptr: compare return + all params recursively
```

### Allocation

All Type nodes allocated from a compiler arena. Never freed individually. Entire compilation = one arena. Reset after compilation done. Same arena model ZER teaches — the compiler eats its own dog food.

---

## Decision 2: Symbol Table — DECIDED: Scope Chain + Separate Dataflow

**Decision:** Scope chain with hash map per scope for name resolution. Separate dataflow analysis pass with bitvectors for handle consumption and branch-dependent state.

**Justification:** Every production compiler uses scope chain. The research confirmed:
- chibicc: scope chain + hash per scope
- Go: scope chain + map per scope
- GCC: scope chain + identifier binding
- lcc's flat table had 5% performance overhead on scope exit (documented problem)

Handle consumption tracking is NOT a symbol table problem. Rust tracks "moved" state with separate dataflow bitvectors on a control flow graph — not flags on symbol table entries. Flags can't represent "maybe consumed on one branch." Bitvectors at each program point can.

### Symbol table structure

```c
typedef struct Scope {
    struct Scope *parent;           // enclosing scope (NULL for module level)
    Symbol *symbols;                // dynamic array of symbols
    u32 symbol_count;
    u32 symbol_capacity;
    const char *module_name;        // non-NULL for module-level scopes
} Scope;

typedef struct Symbol {
    const char *name;
    u32 name_len;
    Type *type;

    // qualifiers (static, known at definition time)
    bool is_keep;                   // keep parameter — can be stored
    bool is_const;                  // const qualifier
    bool is_static;                 // static storage duration

    // source location for error messages
    const char *file;
    u32 line;
} Symbol;
```

### Scope operations

```
ENTERING A SCOPE (every { in the source):
  new_scope = allocate Scope
  new_scope.parent = current_scope
  current_scope = new_scope

LEAVING A SCOPE (every } in the source):
  current_scope = current_scope.parent
  // old scope is abandoned (arena-allocated, freed at end of compilation)

LOOKUP (when compiler sees an identifier):
  scope = current_scope
  while scope != NULL:
    search scope.symbols for name
    if found → return symbol
    scope = scope.parent
  // not found in any scope → check imported modules
  for each imported module:
    search module.scope.symbols for name
    if found in exactly one → return symbol
    if found in multiple → COMPILE ERROR: ambiguous
  // not found anywhere → COMPILE ERROR: undefined

ADDING A SYMBOL (on variable/parameter declaration):
  add to current_scope.symbols
  check: does name already exist in current_scope?
    if yes → COMPILE ERROR: redefinition
    // shadowing in OUTER scope is OK (not redefinition)
```

### What is NOT in the symbol table

```
Handle consumption:    DATAFLOW PASS (separate, after type checking)
Scope escape analysis: DATAFLOW PASS
keep violation checks: DATAFLOW PASS (caller side — is argument long-lived?)
Branch-dependent state: DATAFLOW PASS (bitvectors per program point)

The symbol table answers: "does x exist? what type? is it const/keep/static?"
The dataflow pass answers: "is h consumed at THIS point in the program?"
```

### Dataflow pass architecture

```
Runs AFTER type checking. Operates on typed AST.

1. Build control flow graph (CFG) from typed AST
   - each basic block = sequence of statements with no branches
   - edges = if/else, loop, return, break, continue

2. Assign bit positions to trackable variables
   - every Handle variable gets a bit
   - every pointer parameter (for keep checking) gets a bit

3. Forward-propagate state through CFG
   - free(h) → set consumed bit for h
   - use of h → check consumed bit → ERROR if set

4. Branch merging at join points
   - if branch A consumed h and branch B didn't:
   - merged state = MAYBE CONSUMED
   - any use after merge → COMPILE ERROR:
     "h might be consumed (freed on line 42)"

5. Scope escape checks
   - return &local → ERROR (local pointer escapes)
   - global = &local → ERROR (stored in longer-lived location)
   - pass &local to keep param → ERROR (local can't satisfy keep)

Compiler cost: ~300-400 lines for the dataflow pass.
Separate from the ~200 line symbol table.
```

### Module imports

```
Each .zer file has its own module-level Scope.
import uart; → adds a reference to uart's module Scope.

Lookup order:
  1. current scope chain (inner → outer within current file)
  2. imported module scopes (searched in import order)
  3. found in multiple imports → COMPILE ERROR: ambiguous
  4. not found → COMPILE ERROR: undefined

For UFCS: same lookup, but searching for fn name(*typeof(expr), args)
  found in multiple modules → COMPILE ERROR: ambiguous
  resolved → rewrite to direct call with module qualification
```

---

## Decision 3: Optional Unwrapping — DECIDED: Copy Default, Pointer with |*val|

**Decision:** `|val|` captures produce a copy of the unwrapped value (immutable). `|*val|` captures produce a pointer to the original (mutable). `?T` always unwraps to `T`. Consistent across all five unwrapping patterns.

**Justification:** 90% of captures are read-only (log sensor data, check a value, match a variant). Copy is safe, simple, correct for the common case. The 10% that need to modify in-place use `|*val|` — explicit, visible, same pointer semantics as the rest of ZER.

### Unwrapping rules

```
TYPE STRIPPING — ?T always unwraps to T:
  ?*Task   → *Task
  ?u32     → u32
  ?void    → void (presence only, no value to capture)
  ?[]u8    → []u8
  ?Handle(Task) → Handle(Task)

CAPTURE — |val| vs |*val|:
  |val|    copy of the unwrapped value. immutable.
  |*val|   pointer to the unwrapped value. mutable.
```

### How each pattern works in the type checker

**Pattern 1: if-unwrap**

```
?*Task maybe = find_task();
if (maybe) |val| {
    // type checker:
    // 1. check maybe type → ?*Task
    // 2. unwrap: ?*Task → *Task
    // 3. create new scope
    // 4. add Symbol { name: "val", type: *Task, is_const: true }
    // 5. type-check body in new scope
    // 6. pop scope

    val.priority = 5;       // val is *Task. write through pointer. OK.
}

if (maybe) |*val| {
    // same but:
    // 4. add Symbol { name: "val", type: **Task, is_const: false }
    //    (pointer to the optional's inner pointer)
    //    rarely useful for ?*T — |val| gives you *Task already
}
```

**Pattern 2: if-unwrap on value type**

```
?u32 maybe_num = read_sensor();
if (maybe_num) |val| {
    // val is u32. copy. immutable.
    process(val);           // OK
    val = 99;               // COMPILE ERROR: val is immutable
}

if (maybe_num) |*val| {
    // val is *u32. pointer to the optional's value field.
    *val = 99;              // OK — modifies original optional's value
}
```

**Pattern 3: tagged union switch**

```
union Message {
    SensorData sensor;
    Command command;
}

switch (msg) {
    .sensor => |data| {
        // data is SensorData. copy. immutable.
        log(data.temperature);      // OK — read
        data.temperature = 5;       // COMPILE ERROR: immutable
    },
    .command => |*cmd| {
        // cmd is *Command. pointer. mutable.
        cmd.retries += 1;           // OK — modifies original
    },
}

// type checker for each arm:
// 1. look up variant name in union type → get variant's Type
// 2. |val|  → create scope, add Symbol { type: VariantType, is_const: true }
// 3. |*val| → create scope, add Symbol { type: Pointer(VariantType), is_const: false }
// 4. type-check arm body in new scope
// 5. pop scope
// 6. after all arms: verify exhaustive (all variants handled)
```

**Pattern 4: orelse with value**

```
?u32 result = read_sensor();
u32 val = result orelse 0;

// type checker:
// 1. check result type → ?u32
// 2. unwrap: ?u32 → u32 (this is the expression's result type)
// 3. check orelse value type: 0 is u32 → matches. OK.
// 4. val type = u32
```

**Pattern 5: orelse with flow/block**

```
u32 val = read_sensor() orelse return;
// type checker: ?u32 unwraps to u32. return diverts flow. val is u32.

u32 val = read_sensor() orelse break;
// same. val is u32. break exits loop.

queue.push_checked(cmd) orelse {
    log_error("full");
    report();
};
// type checker: ?void. block runs on null. statement-only.
// orelse { block } does NOT produce a value.
// COMPILE ERROR if used as expression: u32 x = func() orelse { ... };
```

### Implementation in the type checker

```
When processing an if-unwrap or switch capture:
  1. resolve the expression type → must be ?T or union
  2. compute the capture type (T or *T depending on |val| vs |*val|)
  3. push_scope()
  4. add_symbol(capture_name, capture_type, is_const)
  5. type_check(body)
  6. pop_scope()

When processing orelse:
  1. resolve LHS type → must be ?T
  2. unwrap to T
  3. resolve RHS:
     - if value expression: check type == T
     - if return/break/continue: OK (flow control)
     - if block: statement-only check. no result type.
  4. expression result type = T

Compiler cost: ~150 lines for unwrapping logic.
Fits naturally into the existing type checker flow.
```

---

## Decision 4: Builtins — DECIDED: Keyword + Dispatch Table

**Decision:** Pool, Ring, Arena, Handle are parser keywords with known syntax. Builtin methods resolved via dispatch table in the type checker, checked BEFORE UFCS. Handle(T) is type-only, not tied to a specific Pool instance (runtime generation counter catches wrong-pool).

**Justification:** Builtins are a fixed, small set (4 types, ~12 methods). Hardcoding them is simpler than building a generics system. If user-defined generics are added later, builtins can be migrated to use them. For v0.1, hardcoded is correct.

### Parser handling

```
Parser recognizes Pool, Ring, Arena, Handle as type keywords.

Pool(T, N):
  parser sees 'Pool' → expects '(' → parse type → ',' → parse integer → ')'
  produces AST node: PoolType { elem: TypeExpr, count: IntLiteral }

Ring(T, N):
  same syntax as Pool.

Handle(T):
  parser sees 'Handle' → expects '(' → parse type → ')'
  produces AST node: HandleType { elem: TypeExpr }

Arena:
  parser sees 'Arena' → no parameters.
  produces AST node: ArenaType { }
```

### Type checker — builtin method dispatch

```
When type checker sees: expr.method(args)

  1. resolve expr type
  2. is it TYPE_POOL, TYPE_RING, or TYPE_ARENA?
     YES → look up method in builtin dispatch table (below)
     NO  → fall through to:
  3. is method a struct field of expr's type?
     YES → field access
     NO  → fall through to:
  4. search for free function method(*typeof(expr), args)?
     YES → UFCS rewrite
     NO  → COMPILE ERROR: unknown method

Builtins checked FIRST. No ambiguity with UFCS or fields.
```

### Dispatch table

```
POOL(T, N) METHODS:
  +-----------------+---------------+------------------+----------------------+
  | method          | args          | returns          | special rule         |
  +-----------------+---------------+------------------+----------------------+
  | alloc()         | none          | ?Handle(T)       | none                 |
  | get(h)          | Handle(T)     | *T               | result non-storable  |
  | free(h)         | Handle(T)     | void             | consumes h (dataflow)|
  +-----------------+---------------+------------------+----------------------+

RING(T, N) METHODS:
  +-----------------+---------------+------------------+----------------------+
  | method          | args          | returns          | special rule         |
  +-----------------+---------------+------------------+----------------------+
  | push(val)       | T             | void             | overwrites oldest    |
  | push_checked(v) | T             | ?void            | none                 |
  | pop()           | none          | ?T               | none                 |
  +-----------------+---------------+------------------+----------------------+

ARENA METHODS:
  +-----------------+---------------+------------------+----------------------+
  | method          | args          | returns          | special rule         |
  +-----------------+---------------+------------------+----------------------+
  | over(buf)       | []u8          | Arena            | constructor          |
  | alloc(T)        | type          | ?*T              | none                 |
  | alloc_slice(T,n)| type, usize   | ?[]T             | none                 |
  | reset()         | none          | void             | warn outside defer   |
  | unsafe_reset()  | none          | void             | no warning           |
  +-----------------+---------------+------------------+----------------------+
```

### Special rule implementation

```
NON-STORABLE (get result):
  type checker marks the expression node: non_storable = true
  when processing assignment:
    if RHS.non_storable → COMPILE ERROR: "cannot store result of get()"
  ~20 lines.

HANDLE CONSUMPTION (free):
  type checker: just verifies h is Handle(T) matching Pool(T, N).
  signals the dataflow pass: "free(h) at this program point."
  dataflow pass sets consumed bit. checks subsequent uses.
  separation of concerns — type checker checks types, dataflow checks liveness.

ARENA RESET WARNING:
  type checker: when processing arena.reset() call:
    walk up AST parents. if any parent is defer → OK.
    if no defer parent → emit WARNING.
  ~15 lines.
```

### Handle(T) — type-only, not Pool-specific

```
Handle(Task) is a type parameterized by Task.
It does NOT carry which Pool it came from.

Pool(Task, 8) pool_a;
Pool(Task, 4) pool_b;

Handle(Task) h = pool_a.alloc() orelse return;
pool_b.get(h);      // type checker: Handle(Task) matches Pool(Task, N)? yes. compiles.
                     // runtime: generation counter mismatch → TRAP.
                     // wrong pool, but caught at runtime, not silent.

WHY NOT POOL-SPECIFIC:
  tying Handle to a specific Pool variable requires tracking variable
  identity in the type system — that's borrow checker territory.
  type systems track TYPES, not variable names.
  the runtime generation counter already catches this. safe enough.
```

---

## Summary — All Four Decisions

```
DECISION        CHOICE                           COMPILER COST
-------------------------------------------------------------------
1. Type repr    recursive tree, arena-allocated   ~100 lines (types)
2. Symbol table scope chain + hash per scope      ~200 lines (symbols)
                + separate dataflow pass           ~400 lines (dataflow)
3. Unwrapping   |val| copy, |*val| pointer        ~150 lines
4. Builtins     keyword + dispatch table          ~185 lines
-------------------------------------------------------------------
TOTAL type system infrastructure:                 ~1,035 lines
```

---

## What NOT to plan (discover during implementation)

```
- Exact coercion edge cases (discover when tests fail)
- Error message formatting (iterate based on what feels useful)
- How defer interacts with orelse in nested loops (hit it, solve it)
- Packed struct alignment math (straightforward)
- Bit extraction codegen (mechanical)
- C emission formatting (GCC reads anything)
- Nested optionals ??T — decide when you encounter it
- fmt_write sugar desugaring details — implement when fmt module is written
```

---

## Implementation order

```
1. Build lexer → produces tokens                              ✅ DONE (218 tests)
2. Build parser → produces AST (now you know the concrete shape) ✅ DONE (158 tests)
3. REVISIT THIS DOCUMENT — update decisions based on actual AST  ✅ AST shape confirmed
4. Implement Type struct (Decision 1)
5. Implement Scope + Symbol (Decision 2)
6. Implement type checker core:
   a. primitives (u8, u32, bool, void)
   b. pointers (*T, ?*T, *opaque)
   c. slices and arrays ([]T, T[N])
   d. structs and enums
   e. optionals (?T) + orelse + |val| captures (Decision 3)
   f. tagged unions + switch captures
   g. function pointers
   h. builtins — Pool/Ring/Arena/Handle (Decision 4)
7. Implement dataflow pass (handle consumption, scope escape, keep checks)
8. Implement ZER-CHECK — path-sensitive handle verification
   → Runs after type checker, before safety passes
   → Catches handle-in-array UAF, wrong-pool, cross-iteration bugs
   → Zero false positives via under-approximation (Pulse/ISL technique)
   → ~470 lines. See zer-check-design.md for full design.
9. Implement safety passes (bounds insertion, zero insertion)
10. Implement C emitter
11. Milestone zero: u32 x = 5; compiles end-to-end through GCC
```

---

*This document is a PLAN, not a spec. Decisions are finalized but implementation details will be revised when the lexer and parser exist and the actual AST shape is known. The 4 decisions are the skeleton. Everything else is muscle added incrementally.*
