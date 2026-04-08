# ZER-LANG Future Plans — Complete Architecture & Roadmap

## Core Philosophy

ZER is memory-safe C. Same syntax, same mental model. The compiler does the safety work.
ZER targets: embedded, OS kernels, userspace apps, CLI tools, and (with stdlib) web backends.

**ZER will NEVER have:** borrow checker, lifetime annotations, traits, generics, closures, exceptions, garbage collector, LLVM/IR/QBE backends.

**ZER WILL have:** type-stamped containers (monomorphization), table-driven compiler architecture, comprehensive stdlib, WASM target via emscripten.

---

## Architecture Decision: Why ZER Works Without a Borrow Checker

### The Insight

Rust solves memory safety through **mathematical proof** — the borrow checker builds constraint equations about lifetimes and solves them. This handles arbitrary programs but requires lifetime annotations, trait bounds, and 50,000 lines of solver code.

ZER solves memory safety through **state tracking** — follow each allocation through the program, mark it ALIVE when allocated, FREED when freed, error if used after FREED. This handles known allocation patterns (Pool/Slab/Arena) with ~2,000 lines of zercheck code.

### State Machine Per Allocation

```
UNKNOWN → ALIVE → FREED
                → MAYBE_FREED (conditional free — one branch freed, other didn't)
                → TRANSFERRED (passed to spawn — ownership moved to thread)
```

Every safety check maps to a state transition:
- **UAF:** FREED + use → compile error
- **Double free:** FREED + free → compile error
- **Leak:** ALIVE at function exit → compile error
- **Maybe freed:** MAYBE_FREED + use → compile error
- **Handle alias:** Two variables share same alloc_id. Free one → both marked FREED
- **Cross-function:** FuncSummary records "this function frees param[0]". Caller marks as FREED after call
- **Interior pointer:** `*u32 p = &b.field` shares alloc_id with `b`. Free `b` → `p` also FREED

### Why This Scales to OS/Userspace

Every program, no matter how complex, uses memory through a finite set of patterns:
1. Stack allocation (auto-zeroed, scope-bounded)
2. Pool/Slab/Arena (tracked by zercheck)
3. malloc/free via *opaque (tracked by zercheck levels 1-5)
4. Shared struct (auto-locked, thread-safe)

If ZER tracks these 4 patterns correctly, it catches the same bugs as Rust's borrow checker for the same programs. The difference: Rust proves safety for ARBITRARY patterns. ZER tracks safety for KNOWN patterns. For real programs, the known patterns cover 99%+ of code.

---

## Phase 1: Table-Driven Compiler Architecture (v0.4)

### Problem

Currently, adding a new AST node type (like NODE_ONCE, NODE_YIELD) requires manual edits in 15+ locations across checker.c, emitter.c, zercheck.c. Each file has 3-7 exhaustive switch statements that must all be updated. Missing one = silent bug.

### Solution: Node Descriptor Table

```c
// node_table.h — ONE entry per node kind, ALL behavior in one place

typedef struct {
    const char *name;           // "YIELD", "ONCE", "SPAWN"

    // Classification
    bool is_leaf;               // no children to walk (like BREAK, CONTINUE)
    bool is_stmt;               // appears at statement level
    bool is_expr;               // appears in expression position
    bool has_body;              // contains a body block (CRITICAL, ONCE, DEFER)
    bool is_control_flow;       // affects control flow (IF, FOR, WHILE, SWITCH)

    // Child access — for generic walkers
    Node *(*get_body)(Node *n);         // NULL for leaves
    int (*get_child_count)(Node *n);    // number of walkable children
    Node *(*get_child)(Node *n, int i); // get child by index

    // Phase callbacks — NULL means "nothing to do"
    void (*check)(Checker *c, Node *n);                 // type checking
    void (*emit_stmt)(Emitter *e, Node *n);             // statement emission
    void (*emit_expr)(Emitter *e, Node *n);             // expression emission
    void (*zercheck)(ZerCheck *zc, PathState *ps, Node *n); // handle tracking
    void (*emit_guards)(Emitter *e, Node *n);           // bounds check guards

    // Walker behavior flags
    bool collect_labels;        // can contain goto labels
    bool validate_gotos;        // can contain goto statements
    bool scan_frame;            // contributes to stack depth
    bool contains_break;        // can contain break statements
    bool all_paths_return;      // can guarantee all paths return
} NodeDesc;

// The table — indexed by NodeKind enum
static const NodeDesc node_table[NODE_COUNT] = {
    [NODE_YIELD] = {
        .name = "YIELD",
        .is_leaf = true,
        .is_stmt = true,
        .emit_stmt = emit_yield,
    },
    [NODE_AWAIT] = {
        .name = "AWAIT",
        .is_stmt = true,
        .get_child_count = await_child_count,  // 1 (the condition)
        .get_child = await_get_child,
        .check = check_await,
        .emit_stmt = emit_await,
        .zercheck = zc_await,
    },
    [NODE_ONCE] = {
        .name = "ONCE",
        .is_stmt = true,
        .has_body = true,
        .get_body = get_once_body,
        .emit_stmt = emit_once,
        .zercheck = zc_once,
        .collect_labels = true,
        .validate_gotos = true,
        .scan_frame = true,
    },
    [NODE_SPAWN] = {
        .name = "SPAWN",
        .is_stmt = true,
        .get_child_count = spawn_child_count,
        .get_child = spawn_get_child,
        .check = check_spawn,
        .emit_stmt = emit_spawn,
        .zercheck = zc_spawn,
    },
    // ... all other nodes
};
```

### Generic Walkers Replace Exhaustive Switches

```c
// BEFORE: 7 separate exhaustive switches, each 50+ cases
void collect_labels(Node *node, ...) {
    switch (node->kind) {
    case NODE_BLOCK: /* ... */ break;
    case NODE_IF: /* ... */ break;
    case NODE_ONCE: collect_labels(node->once.body, ...); break;
    case NODE_CRITICAL: collect_labels(node->critical.body, ...); break;
    // ... 40 more cases
    }
}

// AFTER: one generic function
void collect_labels(Node *node, ...) {
    NodeDesc *desc = &node_table[node->kind];

    // Structural nodes (BLOCK, IF, FOR, WHILE, SWITCH) handled specially
    if (node->kind == NODE_BLOCK) { /* block walk */ return; }
    if (node->kind == NODE_IF) { /* if walk */ return; }
    if (node->kind == NODE_FOR) { /* for walk */ return; }
    if (node->kind == NODE_WHILE) { /* while walk */ return; }
    if (node->kind == NODE_SWITCH) { /* switch walk */ return; }

    // Feature nodes — use table
    if (!desc->collect_labels) return;  // leaf or irrelevant
    if (desc->has_body && desc->get_body) {
        Node *body = desc->get_body(node);
        if (body) collect_labels(body, ...);
    }
}
```

### Benefits
- Add new node: 1 table row + callbacks. Zero walker edits.
- Compile-time safety: missing table entry = uninitialized struct = NULL callbacks = defined behavior (no-op).
- GCC `-Wswitch` still catches missing structural nodes (5 nodes stay manual).
- Callbacks live next to node definition — easy to audit.

### Implementation Plan
1. Create `node_table.h` with NodeDesc struct and table
2. Extract each switch case into a named callback function
3. Replace walkers one at a time (collect_labels first, then validate_gotos, etc.)
4. Keep structural nodes (BLOCK/IF/FOR/WHILE/SWITCH) as manual cases
5. Remove exhaustive switches for feature nodes
6. Estimated: ~500 lines of refactor, 0 behavior change

---

## Phase 2: Container Keyword + Monomorphization (v0.4-v0.5)

### Problem

ZER currently has 4 builtin container types: Pool(T,N), Slab(T), Ring(T,N), Arena.
For OS/userspace, users need: DynArray, HashMap, String, Queue, Stack, etc.
Without user-defined containers, ZER needs a new compiler builtin per data structure.

### Solution: `container` Keyword

```zer
// User defines a parameterized container — NOT generics
// This is C++ templates without SFINAE, Zig comptime without complexity

container DynArray(T) {
    // Fields
    *T data;
    u32 len;
    u32 capacity;

    // Methods — T is replaced with concrete type at each use site
    void push(T item) {
        if (this.len >= this.capacity) {
            this.grow();
        }
        this.data[this.len] = item;
        this.len += 1;
    }

    ?T pop() {
        if (this.len == 0) { return null; }
        this.len -= 1;
        return this.data[this.len];
    }

    *T get(u32 idx) {
        return &this.data[idx];  // bounds-checked
    }

    void grow() {
        u32 new_cap = this.capacity * 2;
        if (new_cap == 0) { new_cap = 8; }
        // ... realloc logic
    }
}

// Usage — compiler stamps out DynArray_Connection
DynArray(Connection) conns;
conns.push(new_conn);
?Connection c = conns.pop();
```

### How It Works (Monomorphization)

When compiler sees `DynArray(Connection)`:
1. Look up `container DynArray` definition
2. Copy the entire definition
3. Replace all `T` with `Connection` (text substitution)
4. Type-check the stamped result as a normal struct + methods
5. Emit as `struct _zer_DynArray_Connection { Connection *data; uint32_t len; ... }`

This is EXACTLY what Pool/Slab/Ring already do internally — the compiler generates a C struct per instantiation. The `container` keyword just lets users define their own.

### What This Is NOT
- NOT generics — no type constraints, no trait bounds, no where clauses
- NOT templates — no SFINAE, no specialization, no template metaprogramming
- NOT comptime types — no arbitrary compile-time code execution
- Just text substitution + type checking on the result

### Safety Integration
- zercheck tracks container allocations same as Slab/Pool
- `container.push(ptr)` where ptr is a pointer → Ring-style warning
- `this` is an implicit `*ContainerType` pointer — bounds-checked
- Container methods are inlined or emitted as regular functions

### Implementation Plan
1. Parser: `container Name(T) { fields; methods; }` → NODE_CONTAINER_DECL
2. Checker: register container template, stamp on use, type-check stamped result
3. Emitter: emit stamped struct + method functions
4. zercheck: track container instances same as builtins
5. Estimated: ~800 lines across parser/checker/emitter

---

## Phase 3: Standard Library (v0.5)

### Structure

```
lib/
  core/          # no-alloc basics (always available)
    str.zer      # string slices, bytes_equal, bytes_copy, @cstr
    fmt.zer      # integer/float to string, format into buffer
    math.zer     # abs, min, max, clamp, pow (comptime where possible)
    sort.zer     # insertion sort, quicksort on slices

  std/           # requires allocator (DynArray, Map)
    string.zer   # DynArray(u8) with helpers (append, split, trim, contains)
    io.zer       # File, stdin/stdout/stderr, read/write
    fs.zer       # open, close, read_file, write_file
    net.zer      # TCP socket, bind, listen, accept, connect
    http.zer     # HTTP/1.1 parser + response builder
    json.zer     # JSON parser into DynArray/Map structures
    args.zer     # command-line argument parsing
    env.zer      # environment variables
    time.zer     # clock, sleep, duration

  os/            # platform-specific
    linux.zer    # syscalls, epoll, mmap
    windows.zer  # Win32 API wrappers

  embed/         # embedded-specific (no OS)
    uart.zer     # UART driver patterns
    spi.zer      # SPI driver patterns
    gpio.zer     # GPIO abstraction
    timer.zer    # Hardware timer
```

### Dependencies

```
core/ → nothing (freestanding, no libc needed)
std/  → core/ + DynArray + Map (container keyword)
os/   → std/ + cinclude platform headers
embed/ → core/ only (no OS, no libc)
```

### Build Configuration (zer.toml)

```toml
[project]
name = "my-app"
version = "0.1.0"
target = "x86_64-linux"     # or "arm-none-eabi", "wasm32"

[dependencies]
std = true                   # include std/ modules
embed = false                # exclude embedded modules

[build]
gcc = "gcc"                  # or cross-compiler path
flags = ["-O2"]
```

---

## Phase 4: WASM Target (v0.6)

### Approach

ZER already emits C. WASM support = emit C → compile with emscripten → .wasm.

```bash
zerc app.zer -o app.c
emcc app.c -o app.wasm -s STANDALONE_WASM
```

### What ZER Needs
- `--target wasm32` flag: set usize to 32-bit, disable pthreads
- WASM-specific stdlib: no filesystem, no sockets, import/export FFI
- emscripten compatibility: avoid GCC-specific extensions in emitted C
  (statement expressions work in clang/emcc, so most code works as-is)

### What ZER Does NOT Need
- No WASM-specific backend — emscripten handles codegen
- No WASI integration — just emit C, let emcc handle it
- No JavaScript interop — users use `cinclude` for JS FFI headers

---

## Phase 5: Self-Hosting (v1.0)

### Definition

`zerc.zer` compiles itself and produces an identical compiler.

```bash
gcc src/*.c -o zerc              # build from C (primary)
./zerc zer-src/main.zer -o zerc_zer.c  # compile ZER mirror
gcc zerc_zer.c -o zerc2          # build from emitted C
diff zerc zerc2                  # identical = v1.0 proven
```

### Why Not Now

LLMs have zero ZER training data — they write C syntax and call it ZER.
C development is faster with LLM assistance today.
ZER development becomes viable when users post real ZER code online.

### Migration Strategy

The C codebase stays primary until ZER has enough public code that LLMs learn the syntax natively. The ZER mirror (`zer-src/`) is the correctness proof, not the development codebase.

---

## Comparison: ZER Approach vs Alternatives

### ZER vs Rust (at scale)

| Aspect | Rust | ZER |
|---|---|---|
| Safety mechanism | Borrow checker (math proof) | State tracking (follow the pointer) |
| Complexity per feature | O(feature × all_other_features) | O(feature) — independent |
| Container support | Generics + traits (any T) | Monomorphization (stamp per T) |
| Adding new feature | Months (interaction testing) | Hours (table row + callback) |
| Compiler size | ~500K lines | ~25K lines |
| Learning curve | 6 months | 1 hour |
| Target | Everything | Systems + embedded + OS |

### ZER vs Zig

| Aspect | Zig | ZER |
|---|---|---|
| Comptime | Arbitrary code execution | Functions + if only |
| Backend | Custom + LLVM | Emit-C → GCC |
| Safety | Runtime checks | Compile-time + runtime |
| Memory model | Manual + allocator interface | Pool/Slab/Arena/Container |
| Syntax | New syntax | C syntax |

### ZER vs C3/C2/Odin

| Aspect | C3/C2/Odin | ZER |
|---|---|---|
| Safety | Partial (some checks) | Full (UAF, leak, race, deadlock at compile-time) |
| Concurrency | Manual or none | Built-in (shared struct, spawn, async) |
| Memory model | malloc/free | Pool/Slab/Arena (compile-time tracked) |
| Testing | Small test suites | 3,200+ tests, 415 from Rust's suite |

---

## Key Design Principles (Never Violate These)

1. **Emit-C permanently.** No IR, no LLVM, no QBE, no native backends. GCC is the backend.
2. **No borrow checker.** State tracking covers the same bugs for the target domain.
3. **No generics.** Monomorphization (type stamping) covers containers.
4. **No traits.** Function pointers + *opaque covers polymorphism.
5. **No closures.** Function pointers + context struct covers callbacks.
6. **No exceptions.** Optional return (?T) + orelse covers error handling.
7. **No garbage collector.** Pool/Slab/Arena/Container covers allocation.
8. **Features are independent.** Adding feature X never breaks feature Y.
9. **Table-driven architecture.** New nodes = table row + callbacks, not 15 file edits.
10. **C syntax.** A C developer reads ZER in 5 minutes. Never invent new syntax when C syntax works.
