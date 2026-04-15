# ZER IR — Implementation Design Document

## Why This Exists

This document captures the FULL context of the IR decision: the problems that led to it,
the alternatives considered, the design chosen, and the implementation plan. A fresh
session reads this and knows exactly what to build and why.

---

## Part 1: The Problem — Why AST-Direct Architecture Broke Down

### The chibicc Heritage

ZER started from chibicc's architecture: parse → AST → emit. chibicc is ~10K lines,
zero safety analysis. This works because chibicc's emitter just translates AST nodes
to assembly 1:1. No analysis passes walk the AST.

ZER added 29 safety analysis passes onto this architecture. Each pass re-walks the
AST independently, discovering the same structure (scopes, locals, control flow)
in its own way. This worked at 5K lines. At 25K+ lines, it broke down.

### The Symptoms (bugs from this session and prior)

**1. Incomplete Enumeration Bugs**
Every AST walker must handle every NODE_ type. When a new node type or implicit
variable introduction site is added, ALL walkers must be updated. Forgetting one = silent bug.

Specific instances:
- `collect_async_locals` — enumerated NODE_VAR_DECL, missed if-unwrap captures (|val|).
  Result: capture lived on C stack, reads garbage after yield+resume. MEMORY SAFETY BUG.
- `has_atomic_or_barrier` — standalone 60-line scanner, no caching. Re-scanned same body
  every time. Absorbed into FuncProps but the pattern was fragile.
- `resolve_type_for_emit` — missing TYNODE_SLAB, TYNODE_BARRIER, TYNODE_SEMAPHORE,
  TYNODE_CONTAINER. Silently returned ty_void on fallback path.
- Flag-handler matrix: NODE_YIELD missing defer_depth + critical_depth checks.
  NODE_AWAIT same. NODE_SPAWN missing in_interrupt. 5 bugs from incomplete flag checks.

**2. zercheck CFG Hacks**
zercheck does path-sensitive analysis (handle states, UAF, double-free, leaks) by
walking the AST linearly. But AST doesn't encode control flow explicitly:
- if/else merging: manually compute which branches exit, merge states
- Loops: dynamic fixed-point iteration (ceiling 32) because no real loop structure
- Backward goto: 2-pass re-walk hack, still falls back to runtime for cross-block cases
- Switch: manually iterate arms, track which arms terminate
- All of this implemented as special cases in a linear AST walk

With basic blocks: the CFG IS the data structure. Merging, loops, gotos are all just
edges in a graph. Path-sensitive analysis walks the graph naturally.

**3. VRP Manual Tracking**
Value Range Propagation tracks {min, max, known_nonzero} per variable NAME (string).
- `&x` taken → permanently invalid for entire function (address_taken flag)
- Compound assignment → manual invalidation
- Function call → wipe all non-const ranges
- Cross-function: separate return_range system

With SSA-like IR: each LOCAL has an ID. Range per ID at each program point.
PHI nodes at merge points automatically handle if/else range narrowing.
No string-based lookup. No manual invalidation.

**4. Emitter Complexity (6000+ lines)**
The emitter walks the AST and emits C. Every NODE_ type has its own emission path.
Many types have MULTIPLE paths (e.g., orelse has 3 paths: bare return, block, default).
Optional handling has 3 representations (?*T, ?T struct, ?void). Each combination
of node × optional × orelse × async is a separate code path.

Total: ~6000 lines of AST→C translation with hundreds of special cases.

With IR→C: flat instructions map 1:1 to C statements. Optional is normalized.
Orelse is lowered to branches. Async yield is an explicit instruction.
Estimated: ~4000 lines, fewer special cases.

**5. 29 Independent Walkers**
Each safety system walks the AST in its own way:
- collect_async_locals: recursive, scans NODE_VAR_DECL
- has_atomic_or_barrier: recursive, scans NODE_INTRINSIC
- scan_unsafe_global_access: recursive, follows calls (depth 8)
- scan_func_props: recursive, follows calls (DFS)
- scan_frame: recursive, collects callees
- collect_labels / validate_gotos: recursive, collects NODE_LABEL/NODE_GOTO
- emit_auto_guards: iterates auto_guard array
- Plus 22 more...

Each has its own recursion pattern. Each handles NODE_ types differently.
Adding NODE_DO_WHILE required updating 7+ walkers. Adding NODE_SPAWN required
updating 5+ walkers. Each update is a potential forgetting-bug.

With IR: ONE lowering pass handles all NODE_ types → flat IR. All subsequent
passes work on the same flat representation. Adding a new NODE_ type = update
the lowering pass ONCE.

### The Numbers

Bug analysis from BUGS-FIXED.md (500+ bugs, keyword frequency analysis):

| Bug class | Mentions | IR fixes? |
|---|---|---|
| Emitter issues | 293 | YES — flat IR→C emission |
| VRP/bounds | 210 | YES — SSA-like ranges |
| Distinct unwrap | 188 | HALF — emitter bugs yes, checker no |
| Optional handling | 134 | YES — normalized in IR |
| zercheck/UAF | 117 | YES — real CFG |
| Concurrency | 87 | PARTIAL — easier analysis |
| Module/import | 87 | NO — checker-level |
| Async | 54 | YES — flat locals |

IR eliminates ~60-70% of historical bug classes.
Combined with always-unwrap-distinct-in-typemap (~15%): ~80-85% eliminated.
Remaining ~15-20% are already stable (parser, modules).

---

## Part 2: Alternatives Considered

### Alternative 1: More AST Walkers (Status Quo)
Keep adding walkers for each new analysis need. Use audit tools (flag-handler matrix,
ctags) to catch enumeration bugs.

- Pro: No rewrite needed
- Con: Every new feature = update N walkers. N keeps growing. Bugs keep appearing.
- Con: zercheck stays linear with CFG hacks
- Con: VRP stays manual
- Verdict: REJECTED — the system is already showing strain at 29 walkers

### Alternative 2: Table-Driven Emitter (original v0.4 plan)
Replace the emitter's switch/case dispatch with lookup tables.

- Pro: Cleaner emitter organization
- Con: Still walks AST — all 29 walkers stay
- Con: Doesn't fix zercheck, VRP, async, enumeration bugs
- Con: Only improves emitter (~20% of bugs), not the other 80%
- Verdict: REJECTED — solves wrong problem. IR covers emitter AND everything else

### Alternative 3: Annotation Approach
Checker marks symbols with flags (needs_async_promotion, is_live_across_yield).
Emitter reads flags instead of re-discovering by AST enumeration.

- Pro: ~50 lines, fixes async capture bug immediately
- Con: Only fixes async. Doesn't fix zercheck CFG, VRP, emitter complexity
- Con: Still adds flags (more to maintain)
- Verdict: PARTIAL — good for immediate fix, not long-term solution

### Alternative 4: Effect System (language feature)
Add effect annotations to function signatures: `void worker() yields { ... }`

- Pro: Formal, user-visible
- Con: ZER sees all bodies — inference is complete. Annotations redundant.
- Con: New syntax for zero safety benefit
- Verdict: REJECTED — replaced by FuncProps (inferred function summaries)

### Alternative 5: IR (CHOSEN)
Lower AST to flat intermediate representation after type checking.

- Pro: Eliminates 60-70% of historical bug classes
- Pro: Simplifies every analysis pass
- Pro: Every production compiler at ZER's scale has one
- Pro: One lowering pass instead of 29 walkers
- Pro: Real CFG for zercheck, SSA-like for VRP
- Pro: Flat locals for async (complete by construction)
- Pro: Simpler C emission (flat instructions → C statements)
- Con: ~3000 lines initial implementation
- Con: 2-3 months development time
- Con: Rewrite zercheck and emitter
- Verdict: CHOSEN — best long-term investment for solo developer

---

## Part 3: Design Inspiration — Rust MIR

### Why MIR, Not Other IRs

| IR | Level | Problem for ZER |
|---|---|---|
| LLVM IR | Low (near assembly) | Too low — ZER emits C, not assembly |
| QBE | Low (register alloc) | Same — too low |
| GCC GIMPLE | Mid (three-address) | Tied to GCC internals, not reusable |
| Go SSA | Mid (SSA) | Assumes garbage collector |
| Swift SIL | Mid (ownership) | Over-complex, ARC-specific |
| Zig ZIR | Mid (comptime) | Poorly documented, async removed |
| **Rust MIR** | **Mid (flat locals, BB)** | **Exact match — no GC, safety analysis, async** |

### What ZER Takes From MIR

1. **Flat local list** — every variable, temp, capture is an explicit LOCAL with an ID
2. **Basic blocks** — explicit control flow graph (branch, goto, return)
3. **YIELD as instruction** — async yield is a first-class IR instruction
4. **Type annotations on locals** — every LOCAL has a resolved Type*

### What ZER Skips From MIR

1. **Borrow annotations** — ZER uses handle states (Model 1), not borrows
2. **Lifetime regions** — ZER uses escape flags (Model 2)
3. **Drop/unwind edges** — ZER uses defer stack (emitter concern)
4. **Full three-address code** — ZER keeps tree expressions for cleaner C output
5. **Move paths** — ZER's move struct is simpler (HS_TRANSFERRED)
6. **Place projections** — ZER's field tracking is simpler

### ZER's Hybrid: MIR Control Flow + AST Expressions

Pure MIR decomposes all expressions into temps:
```
_1 = a.b;
_2 = _1[i];
_3 = _2.c;
_4 = d.e;
_5 = _3 + _4;
```

ZER keeps expressions intact within instructions:
```
bb0:
  %x = a.b[i].c + d.e     // tree expression preserved
  BRANCH (%x > 0) -> bb1, bb2
```

**Why:** ZER emits C, not assembly. C supports complex expressions. Decomposing them
into temps produces ugly C with unnecessary variables. Keeping trees produces
readable C that maps naturally to the source.

**Trade-off:** Analysis passes that need to inspect sub-expressions must still walk
expression trees within instructions. But the CONTROL FLOW is flat (basic blocks),
and LOCALS are flat (explicit list). This covers the main analysis needs.

---

## Part 4: IR Data Structures

### Core Types

```c
/* ---- IR Local: one per variable/temp/capture in the function ---- */
typedef struct {
    int id;                  /* unique within function, 0-based */
    const char *name;        /* source name (for C emission + debugging) */
    uint32_t name_len;
    Type *type;              /* resolved type from checker */
    bool is_param;           /* true = function parameter */
    bool is_capture;         /* true = if-unwrap or switch capture */
    bool is_temp;            /* true = compiler-generated temp */
    int source_line;         /* for error messages + #line directives */
} IRLocal;

/* ---- IR Instruction: one operation ---- */
typedef enum {
    IR_ASSIGN,       /* %dest = expr */
    IR_CALL,         /* %dest = func(args...) or void call */
    IR_BRANCH,       /* conditional: if (cond) goto bb_true else goto bb_false */
    IR_GOTO,         /* unconditional: goto bb */
    IR_RETURN,       /* return expr (or void) */
    IR_YIELD,        /* async yield point */
    IR_AWAIT,        /* async await condition */
    IR_SPAWN,        /* spawn func(args) */
    IR_INTRINSIC,    /* @ptrcast, @size, @truncate, etc. */
    IR_POOL_ALLOC,   /* pool.alloc() → ?Handle */
    IR_POOL_FREE,    /* pool.free(h) */
    IR_POOL_GET,     /* pool.get(h) → *T */
    IR_SLAB_ALLOC,   /* slab.alloc() → ?Handle */
    IR_SLAB_FREE,    /* slab.free(h) */
    IR_ARENA_ALLOC,  /* arena.alloc(T) → ?*T */
    IR_RING_PUSH,    /* ring.push(val) */
    IR_RING_POP,     /* ring.pop() → ?T */
    IR_LOCK,         /* shared struct lock */
    IR_UNLOCK,       /* shared struct unlock */
    IR_CRITICAL_BEGIN, /* interrupt disable */
    IR_CRITICAL_END,   /* interrupt enable */
    IR_DEFER_PUSH,   /* push defer body */
    IR_DEFER_FIRE,   /* fire all pending defers */
} IROpKind;

typedef struct {
    IROpKind op;
    int dest_local;          /* -1 if no destination (void call, branch) */
    Node *expr;              /* AST expression tree (for tree expressions) */
    int source_line;         /* for #line emission */

    /* Branch operands */
    int true_block;          /* target basic block ID (for IR_BRANCH) */
    int false_block;         /* target basic block ID (for IR_BRANCH) */
    int goto_block;          /* target basic block ID (for IR_GOTO) */

    /* Call operands */
    const char *func_name;
    uint32_t func_name_len;
    int arg_count;
    Node **args;             /* AST expression trees for arguments */

    /* Pool/Slab/Ring operands */
    int obj_local;           /* which LOCAL holds the pool/slab/ring */
    int handle_local;        /* which LOCAL holds the handle (for free/get) */
} IRInst;

/* ---- IR Basic Block: sequence of instructions + terminator ---- */
typedef struct {
    int id;                  /* unique within function */
    IRInst *insts;           /* array of instructions */
    int inst_count;
    int inst_capacity;

    /* Predecessors (for CFG analysis) */
    int *preds;              /* predecessor block IDs */
    int pred_count;
    int pred_capacity;
} IRBlock;

/* ---- IR Function: the complete lowered representation ---- */
typedef struct {
    const char *name;
    uint32_t name_len;
    Type *return_type;
    bool is_async;
    bool is_interrupt;

    /* Flat local list — THE state struct for async functions */
    IRLocal *locals;
    int local_count;
    int local_capacity;

    /* Basic blocks — THE CFG for zercheck */
    IRBlock *blocks;
    int block_count;
    int block_capacity;

    /* Entry block is always blocks[0] */

    /* Source function node (for checker cross-reference) */
    Node *ast_node;
} IRFunc;
```

### Design Decisions Explained

**Why `Node *expr` in IRInst (not decomposed)?**
ZER emits C, which supports complex expressions. `a.b[i].c + d.e` emits as one C
statement. Decomposing into temps would create:
```c
uint32_t _t1 = a.b;       // ugly, unnecessary
uint32_t _t2 = _t1[i];    // GCC optimizes away but code is unreadable
uint32_t _t3 = _t2.c;
```
By keeping the expression tree, the emitter produces clean C. The control flow
(which IS decomposed into basic blocks) is where analysis needs flat structure.

**Why explicit IR_POOL_ALLOC etc. (not generic IR_CALL)?**
Builtin methods (pool.alloc, ring.push, arena.alloc) are intercepted by the checker
and emit special C code. In the IR, they're explicit instructions so:
- zercheck sees IR_POOL_ALLOC → registers handle as ALIVE (no string matching)
- zercheck sees IR_POOL_FREE → marks handle as FREED (no method name parsing)
- Emitter emits the correct C preamble call (no obj_type dispatch)
Currently zercheck parses method names from AST NODE_CALL. With IR, the lowering
pass already classified the call. No re-classification needed.

**Why IR_LOCK/IR_UNLOCK (not hidden in emission)?**
Currently, shared struct access emits lock/unlock inline. The checker uses
`shared_type_collect` to detect multi-type locking (deadlock). With explicit
IR_LOCK/IR_UNLOCK instructions, deadlock detection is trivial: scan for
two IR_LOCK instructions in the same basic block without an IR_UNLOCK between.

**Why IR_YIELD is its own instruction (not IR_RETURN + metadata)?**
Yield in async compiles to `self->state = N; return 0; case N:;` — it's a
return AND a resume point. Making it a distinct instruction lets:
- FuncProps scan: just check for IR_YIELD in any block
- Async emitter: emit the Duff's device pattern
- zercheck: know that control flow splits (some state may not survive yield)

**Why IR_DEFER_PUSH/IR_DEFER_FIRE (not scope-based)?**
Currently, defers are tracked by the emitter's defer stack and fired at scope exits.
With IR, defer push/fire are explicit instructions. The lowering pass inserts
IR_DEFER_FIRE before every IR_RETURN, IR_GOTO (that exits scope), IR_BRANCH
(break/continue). No emitter-side defer tracking needed.

---

## Part 5: The Lowering Pass (AST → IR)

### Overview

```c
/* Lower one function's AST to IR.
 * Requires: checker has run (typemap populated, types resolved).
 * Produces: IRFunc with flat locals, basic blocks, typed instructions. */
IRFunc *lower_func(Arena *arena, Checker *checker, Node *func_decl);
```

### What the Lowering Pass Does

1. **Collect ALL locals** — walk AST once, find every:
   - Function parameter → IRLocal with is_param=true
   - NODE_VAR_DECL → IRLocal
   - If-unwrap capture (|val|) → IRLocal with is_capture=true
   - Switch arm capture → IRLocal with is_capture=true
   - Orelse/unwrap temps → IRLocal with is_temp=true
   - For-in range iterator → IRLocal
   This is the COMPLETE local list. No enumeration to forget.

2. **Create basic blocks** for control flow:
   - Function entry → bb0
   - if/else → branch to bb_then / bb_else, merge at bb_join
   - while/for → bb_cond, bb_body, bb_step, bb_exit (with back edge)
   - switch → bb_arm0, bb_arm1, ..., bb_default, bb_exit
   - goto/label → map label names to block IDs
   - yield → split block (yield terminates current block, resume starts new block)
   - defer → IR_DEFER_PUSH in current block, IR_DEFER_FIRE before exits

3. **Lower expressions** — keep expression trees mostly intact, but:
   - Resolve builtin method calls to specific IR ops (pool.alloc → IR_POOL_ALLOC)
   - Resolve if-unwrap to: eval condition → IR_BRANCH on has_value → assign capture
   - Resolve orelse to: eval expr → IR_BRANCH on has_value → assign or fallback
   - Resolve Handle auto-deref to: IR_POOL_GET + field access

4. **Insert implicit operations:**
   - Bounds checks → IR_BRANCH (check in range) + trap block
   - Division guards → IR_BRANCH (check nonzero) + trap block
   - Null checks → IR_BRANCH (check non-null) on ?*T
   - Auto-zero → not needed in IR (all locals zero-initialized at entry)

### Lowering Example

ZER source:
```zer
?u32 opt = get_val();
if (opt) |val| {
    yield;
    process(val);
}
```

AST:
```
NODE_VAR_DECL opt: ?u32 = NODE_CALL(get_val)
NODE_IF {
  cond: NODE_IDENT(opt)
  capture: val
  then: NODE_BLOCK {
    NODE_YIELD
    NODE_EXPR_STMT(NODE_CALL(process, NODE_IDENT(val)))
  }
}
```

IR:
```
LOCALS:
  %0: opt  : ?u32     (variable)
  %1: val  : u32      (capture)     ← capture is explicit local

bb0:
  %0 = CALL get_val()
  BRANCH %0.has_value -> bb1, bb2

bb1:                                 ← then-body
  %1 = %0.value                     ← capture assignment
  YIELD                              ← explicit yield
  CALL process(%1)
  GOTO bb2

bb2:                                 ← join point (after if)
  RETURN
```

**What changed:** capture `val` is LOCAL %1 — explicitly in the list. No enumeration
needed to find it. Async state struct = all LOCALs. YIELD is an instruction in bb1.
Control flow is explicit basic blocks. zercheck sees the CFG directly.

### Lowering for Each AST Node Type

**Statements:**

| AST Node | IR Lowering |
|---|---|
| NODE_VAR_DECL | Allocate IRLocal, emit IR_ASSIGN in current block |
| NODE_IF | Create bb_then + bb_else + bb_join, emit IR_BRANCH |
| NODE_IF + capture | Create IRLocal for capture, assign from condition |
| NODE_FOR | Create bb_cond + bb_body + bb_step + bb_exit |
| NODE_WHILE | Create bb_cond + bb_body + bb_exit (back edge from body to cond) |
| NODE_DO_WHILE | Create bb_body + bb_cond + bb_exit (body first, then cond) |
| NODE_SWITCH | Create bb per arm + bb_exit, IR_BRANCH chain |
| NODE_RETURN | IR_DEFER_FIRE, IR_RETURN |
| NODE_BREAK | IR_DEFER_FIRE (loop defers only), IR_GOTO to loop exit |
| NODE_CONTINUE | IR_DEFER_FIRE (loop defers only), IR_GOTO to loop cond/step |
| NODE_GOTO | IR_DEFER_FIRE (pending defers), IR_GOTO to target label's block |
| NODE_LABEL | Start new basic block (label target) |
| NODE_DEFER | IR_DEFER_PUSH (body reference) |
| NODE_YIELD | IR_YIELD (terminates block, next block is resume point) |
| NODE_AWAIT | IR_BRANCH on condition → yield block or continue block |
| NODE_SPAWN | IR_SPAWN with args |
| NODE_CRITICAL | IR_CRITICAL_BEGIN, lower body, IR_CRITICAL_END |
| NODE_ONCE | Lower to IR_BRANCH on atomic CAS flag |
| NODE_EXPR_STMT | IR_ASSIGN or IR_CALL (void) |
| NODE_ASM | IR_ASM (pass-through) |

**Expressions (kept as trees in IRInst.expr):**

| AST Node | IR Representation |
|---|---|
| NODE_BINARY | Kept as tree |
| NODE_UNARY | Kept as tree |
| NODE_CALL (regular) | IR_CALL instruction |
| NODE_CALL (builtin) | IR_POOL_ALLOC, IR_SLAB_ALLOC, etc. |
| NODE_FIELD | Kept as tree |
| NODE_INDEX | Kept as tree (bounds check inserted as IR_BRANCH before) |
| NODE_ORELSE | Lowered to: eval → IR_BRANCH → assign or fallback block |
| NODE_INTRINSIC | IR_INTRINSIC instruction |
| NODE_TYPECAST | Kept as tree |
| NODE_STRUCT_INIT | Kept as tree |

### What the Lowering Pass Does NOT Do

- **Type checking** — already done by checker on AST
- **Scope resolution** — already done by checker (typemap populated)
- **Error reporting** — all errors caught by checker before lowering
- **Optimization** — GCC handles this after C emission
- **Dead code elimination** — GCC handles this too

The lowering pass is a TRANSLATION, not an analysis. It reads the typed AST
and produces the flat IR. It should be ~1000-1500 lines, straightforward.

---

## Part 6: Analysis Passes on IR

### zercheck on IR (rewrite ~2700 → ~1500 lines)

Currently zercheck walks AST linearly with CFG hacks. With IR:

```c
/* For each basic block in topological order: */
for (int bi = 0; bi < func->block_count; bi++) {
    IRBlock *bb = &func->blocks[bi];
    PathState state = merge_predecessors(bb);  /* CFG merge — natural */

    for (int ii = 0; ii < bb->inst_count; ii++) {
        IRInst *inst = &bb->insts[ii];
        switch (inst->op) {
        case IR_POOL_ALLOC:
            register_handle(&state, inst->dest_local, HS_ALIVE);
            break;
        case IR_POOL_FREE:
            mark_freed(&state, inst->handle_local);
            break;
        case IR_ASSIGN:
            propagate_alias(&state, inst->dest_local, inst->expr);
            break;
        /* ... */
        }
    }
}
```

**What simplifies:**
- No string keys (`char key[128]` → `int local_id`)
- No linear walk hacks (CFG is the data structure)
- No backward goto workaround (topological order + fixed-point on back edges)
- No block_always_exits check (block terminator is explicit: RETURN, BRANCH, GOTO)
- No FuncSummary pre-building hack (walk callee's IR directly)
- Handle states per LOCAL id (array), not per name (hash/scan)

### VRP on IR (rewrite ~500 → ~300 lines)

Currently VRP tracks ranges per variable name with manual invalidation.
With IR using LOCAL ids:

```c
typedef struct {
    int64_t min, max;
    bool known_nonzero;
    bool valid;
} IRRange;

/* Range per local per block (sparse — only track what's needed) */
IRRange ranges[func->local_count];

/* At merge points (block with multiple predecessors): */
for (int li = 0; li < local_count; li++) {
    ranges[li] = intersect(pred1_ranges[li], pred2_ranges[li]);
}

/* At &x: */
case IR_INTRINSIC:  /* addr_of */
    ranges[target_local].valid = false;  /* invalidate by LOCAL id */
```

**What simplifies:**
- No string-based variable lookup
- No manual invalidation on compound assignment (lowering produces IR_ASSIGN)
- No address_taken permanent flag (invalidate at the specific program point)
- Merge at join points is natural (basic block predecessors)

### FuncProps on IR (already clean, minor improvements)

FuncProps already uses the right model (lazy DFS, cached on Symbol).
With IR, the scanner is even simpler:

```c
/* Instead of recursive AST walk, just scan instruction list */
for (int bi = 0; bi < func->block_count; bi++) {
    for (int ii = 0; ii < func->blocks[bi].inst_count; ii++) {
        switch (func->blocks[bi].insts[ii].op) {
        case IR_YIELD:  sym->props.can_yield = true; break;
        case IR_SPAWN:  sym->props.can_spawn = true; break;
        case IR_SLAB_ALLOC: case IR_POOL_ALLOC:
            sym->props.can_alloc = true; break;
        /* ... */
        }
    }
}
```

No recursive walk needed. Just iterate blocks and instructions.

### Async State Promotion on IR (trivial)

```c
/* The state struct IS the local list */
for (int li = 0; li < func->local_count; li++) {
    emit_struct_field(func->locals[li].name, func->locals[li].type);
}
/* Done. Every local is in the struct. No enumeration. */
```

This is the entire async local collection. Compare to the current 80+ lines
of `collect_async_locals` + separate struct emission walk + capture emission.

---

## Part 7: C Emission from IR

### Overview

```c
/* Emit one function from IR to C */
void emit_func_from_ir(Emitter *e, IRFunc *func);
```

### Basic Block → C Label + Statements

```c
for (int bi = 0; bi < func->block_count; bi++) {
    IRBlock *bb = &func->blocks[bi];

    /* Emit label (skip for bb0 = entry) */
    if (bi > 0) emit(e, "_bb%d:;\n", bb->id);

    /* Emit instructions */
    for (int ii = 0; ii < bb->inst_count; ii++) {
        emit_ir_inst(e, &bb->insts[ii], func);
    }
}
```

### Instruction → C Statement

| IR Instruction | C Output |
|---|---|
| `%x = expr` | `type x = expr;` (or `self->x = expr;` in async) |
| `CALL func(args)` | `func(args);` |
| `BRANCH cond → bb1, bb2` | `if (cond) goto _bb1; else goto _bb2;` |
| `GOTO bb` | `goto _bb;` |
| `RETURN expr` | `return expr;` |
| `YIELD` | `self->_zer_state = N; return 0; case N:;` |
| `IR_POOL_ALLOC` | `_zer_pool_alloc(...)` |
| `IR_LOCK` | `pthread_mutex_lock(...)` |
| `IR_CRITICAL_BEGIN` | `#if defined(__ARM_ARCH) __asm__...` |

### Structured C Recovery (optional, for readability)

Basic blocks + gotos produce correct but ugly C. For readability, the emitter
can optionally reconstruct structured control flow:

```c
/* Detect if/else diamond pattern: */
if (bb has exactly 2 successors, both converge to same join block)
    → emit as if/else instead of goto

/* Detect while loop pattern: */
if (bb is a back-edge target with condition check)
    → emit as while() instead of goto + label

/* Detect switch pattern: */
if (bb has N successors based on same variable)
    → emit as if/else chain (ZER's switch → C if/else)
```

This is what decompilers (IDA, Ghidra) do. Well-studied algorithms.
Not required for correctness — just for readable C output.

### Async Emission from IR

```c
/* State struct = all locals */
emit(e, "typedef struct {\n");
emit(e, "    int _zer_state;\n");
for (int li = 0; li < func->local_count; li++) {
    emit(e, "    ");
    emit_type_and_name(e, func->locals[li].type,
                       func->locals[li].name, func->locals[li].name_len);
    emit(e, ";\n");
}
emit(e, "} _zer_async_%.*s;\n", func->name_len, func->name);

/* Poll function = Duff's device */
emit(e, "static inline int _zer_async_%.*s_poll(...) {\n", ...);
emit(e, "    switch (self->_zer_state) { case 0:;\n");

for (int bi = 0; bi < func->block_count; bi++) {
    /* ... emit each block ... */
    /* When IR_YIELD encountered: */
    emit(e, "    self->_zer_state = %d; return 0;\n", yield_id);
    emit(e, "    case %d:;\n", yield_id);
    yield_id++;
}

emit(e, "    } self->_zer_state = -1; return 1;\n}\n");
```

Complete. No enumeration. No missing captures. The local list IS the struct.

---

## Part 8: IR Validation Pass

### Why Validate

Rust runs MIR validation after every transform. It catches malformed IR before
it reaches LLVM, with clear error messages instead of confusing LLVM errors.

ZER should do the same: validate IR after lowering, before analysis/emission.

### What to Validate

```c
bool validate_ir(IRFunc *func) {
    /* 1. Every referenced local exists in the local list */
    /* 2. Every branch target is a valid block ID */
    /* 3. Every block has exactly one terminator (BRANCH, GOTO, RETURN, YIELD) */
    /* 4. Entry block (0) has no predecessors */
    /* 5. All blocks are reachable from entry */
    /* 6. Type consistency: dest_local type matches expression type */
    /* 7. No duplicate local IDs */
    /* 8. Async functions have at least one IR_YIELD */
    /* 9. IR_POOL_FREE references a local that was IR_POOL_ALLOC'd */
}
```

If validation fails, it's a LOWERING BUG — clear error message pointing at the
lowering pass, not a confusing GCC error from emitted C.

---

## Part 9: Implementation Plan

### Phase 1: IR Data Structures (~200 lines)
- Define IRLocal, IRInst, IRBlock, IRFunc in new `ir.h`
- Arena allocation helpers for IR construction
- IR pretty-printer for debugging (human-readable dump)

### Phase 2: Lowering Pass (~1500 lines)
- `lower_func()` in new `ir_lower.c`
- Handle all NODE_ types (statements + expressions)
- Collect locals (params, var_decls, captures, temps)
- Create basic blocks for control flow
- Lower builtins to specific IR ops
- Handle defer, orelse, if-unwrap as IR patterns

### Phase 3: IR Validation (~200 lines)
- `validate_ir()` in `ir.h` or `ir_lower.c`
- Check all structural invariants
- Run after lowering, assert before emission

### Phase 4: C Emission from IR (~1500 lines)
- `emit_func_from_ir()` in emitter.c (or new `ir_emit.c`)
- Basic block → label + statements
- Async → Duff's device from IR_YIELD
- Optional structured recovery for readability
- Gradually replace AST emission (function by function)

### Phase 5: zercheck on IR (~1500 lines)
- Rewrite zercheck to work on IRFunc
- PathState per basic block (not per AST statement)
- Handle states per LOCAL id (not per name string)
- CFG merge at block predecessors (natural, no hacks)
- FuncSummary from IR instruction scan

### Phase 6: VRP on IR (~300 lines)
- Range per LOCAL id per block
- Merge at join points (basic block predecessors)
- No string lookup, no manual invalidation
- &x invalidation scoped to the specific IR point

### Phase 7: Delete Old Code
- Remove `collect_async_locals` (~80 lines)
- Remove `has_atomic_or_barrier` (already absorbed by FuncProps)
- Remove `scan_unsafe_global_access` (~100 lines — absorbed by FuncProps IR scan)
- Remove AST-walking portions of zercheck (~1200 lines)
- Remove AST-walking portions of emitter (~2000 lines)
- Remove VRP manual tracking (~200 lines)
- Net: ~3500 lines removed

### Estimated Totals

| Component | New Lines | Removed Lines |
|---|---|---|
| ir.h (data structures) | 200 | 0 |
| ir_lower.c (lowering) | 1500 | 0 |
| ir_emit.c (C emission) | 1500 | ~2000 (old emitter) |
| zercheck.c (rewrite) | 1500 | ~1200 (old zercheck AST walk) |
| VRP (rewrite) | 300 | ~200 (old VRP) |
| Validation | 200 | 0 |
| Delete old walkers | 0 | ~100 (standalone scanners) |
| **Total** | **~5200** | **~3500** |
| **Net** | **~1700 lines added** | |

### Migration Strategy

**Incremental, not big-bang:**

1. Build IR + lowering. Validate. Don't connect to emitter yet.
2. Add `--emit-ir` flag to zerc for debugging (print IR to stdout).
3. Connect IR emitter for ONE test function. Compare C output with AST emitter.
4. Gradually route more functions through IR path.
5. When all 4000+ tests pass through IR path, remove AST emission path.
6. Rewrite zercheck on IR. Same gradual migration.
7. Rewrite VRP on IR.

At every step, all tests must pass. If a step breaks tests, the old path is still there.

---

## Part 10: What This Means for the 4 Safety Models

The 4 models don't change. The IR changes HOW they're implemented, not WHAT they do.

| Model | Current (AST) | With IR |
|---|---|---|
| 1: State Machines | zercheck walks AST, string keys | zercheck walks IR, LOCAL ids |
| 2: Point Properties | VRP manual tracking + context flags | VRP on IR blocks + flags stay for checker |
| 3: Function Summaries | FuncProps scans AST recursively | FuncProps scans IR instruction list |
| 4: Static Annotations | Set on Symbol, checked in checker | Unchanged (checker still on AST) |

The models are the WHAT. AST vs IR is the HOW. The 4 models remain the correct
framework for categorizing safety features. The IR just makes each model's
implementation simpler.

---

## Part 11: What IR Does NOT Change

- **Parser** — still produces AST (syntax is tree-shaped)
- **Checker** — still works on AST (type inference needs tree context)
- **Typemap** — still Node* → Type* (checker output)
- **Module system** — still import/register/check in topo order
- **GCC backend** — still emit C → GCC (IR doesn't replace GCC)
- **4 safety models** — same framework, same categorization
- **Ban decision framework** — same (hardware/emission/runtime/type system)
- **Test infrastructure** — same (all tests still exercise full pipeline)
- **`--emit-c` output** — same C code (just generated from IR instead of AST)

---

## Part 12: Relationship to Other Planned Work

### v0.4 was "table-driven emitter"
IR replaces this plan. Table-driven organized the emitter's switch dispatch.
IR eliminates the switch dispatch entirely — flat instructions don't need dispatch.

### v1.0 is self-hosting (zerc.zer compiles itself)
IR makes self-hosting EASIER:
- Fewer lines to translate (net -1700 lines)
- Simpler patterns (flat instructions vs nested AST switch)
- The IR data structures in C translate naturally to ZER structs

### Always-unwrap-distinct-in-typemap
Still planned. Independent of IR. Checker-level fix (~20 lines).
Eliminates the remaining ~15% of historical bugs that IR doesn't cover.

### FuncProps (function summaries)
Already implemented. Works on AST now. Will become simpler on IR
(instruction scan instead of recursive AST walk). No rewrite needed —
just simplify when IR is available.

---

## Part 13: Risk Assessment

### What Could Go Wrong

1. **Lowering bugs** — lowering produces wrong IR. Mitigated by: IR validation +
   4000+ existing tests. Wrong IR → wrong C → test failure.

2. **Performance regression** — extra lowering step. Mitigated by: IR is in-memory
   structs, not file I/O. Lowering is linear in AST size. Negligible vs GCC time.

3. **Incomplete lowering** — new NODE_ type not handled in lowering. Mitigated by:
   exhaustive switch (-Wswitch). GCC warns when a NODE_ type is missing from
   the lowering switch. Same protection as the 7 exhaustive AST walkers, but
   in ONE place instead of 7.

4. **Migration regressions** — tests break during gradual migration. Mitigated by:
   dual-path execution (old AST path + new IR path, compare outputs).

### What Won't Go Wrong

- GCC backend unaffected (still receives C code)
- Parser unaffected (still produces AST)
- Checker unaffected (still works on AST)
- Test suite unaffected (still exercises same ZER programs)
- User-visible behavior unaffected (same errors, same C output)

---

## Part 14: Decision Record

### Problem
29 AST walkers, incomplete enumeration bugs, zercheck CFG hacks, VRP manual tracking.
60-70% of 500+ historical bugs traceable to AST-direct analysis.

### Decision
Build MIR-inspired IR (flat locals, basic blocks, tree expressions).
Sits between checker and emitter. Still emits C → GCC.

### Alternatives Rejected
- More AST walkers (status quo — doesn't scale)
- Table-driven emitter (only fixes emitter, not zercheck/VRP/async)
- Annotation approach (only fixes async, not zercheck/VRP/emitter)
- Effect system (ZER sees all bodies — inference complete, annotations redundant)
- Native backend / LLVM (ZER emits C permanently)

### Rationale
- Every production compiler at ZER's scale has an IR
- One lowering pass replaces 29 walkers
- Flat locals eliminate enumeration bugs (async capture ghost class)
- Basic blocks eliminate CFG hacks (zercheck)
- SSA-like locals simplify VRP
- Net -1700 lines long-term
- MIR design proven by Rust (same constraints: no GC, safety analysis, async)

### Implementation
7 phases, incremental migration. Old AST path preserved until all tests pass on IR.
~5200 new lines, ~3500 removed. 2-3 months for solo developer.

### Status
**All 7 phases implemented (2026-04-15).** ~2870 new lines. All 4000+ tests pass.

| Phase | File | Lines | Status |
|---|---|---|---|
| 1 | ir.h | 241 | Data structures, API ✓ |
| 1 | ir.c | 416 | Construction, validation, pretty-printer ✓ |
| 2 | ir_lower.c | 960 | AST → IR lowering ✓ |
| 3 | (in ir.c) | — | Validation ✓ |
| 4 | (in zerc_main.c) | +25 | --emit-ir flag ✓ |
| 5 | (in emitter.c) | +425 | emit_func_from_ir ✓ |
| 6 | zercheck_ir.c | 452 | Handle tracking on CFG ✓ |
| 7 | vrp_ir.c | 349 | Range propagation on locals ✓ |

**Next:** Wire IR path as default (migration). Route functions through IR, compare output with AST path, delete AST path when all tests pass.

### Checker vs IR System Split (23/6)

**Rule:** "What does it MEAN?" → checker (AST). "Is it SAFE?" → IR.

**Checker (6 systems, run before IR exists):**
These CREATE the typed AST that IR is lowered from. Can't move to IR.
- #1 Typemap — Node → Type resolution
- #2 Type ID — runtime provenance tags
- #19 MMIO Ranges — constant address validation
- #20 Qualifiers — const/volatile type-level checks
- #25 Containers — monomorphization stamping
- #26 Comptime — compile-time evaluation

**IR (23 systems, run after lowering):**
Everything that needs data flow, paths, or CFG.
- All of Model 1 (handle states, move tracking, alloc coloring, alloc ID)
- Most of Model 2 (VRP, provenance, escape flags, auto-guard, dynamic freed, context flags for scope-exit, union lock)
- All of Model 3 (FuncProps, FuncSummary, ProvSummary, ParamProv, RetRange, StackFrames, SpawnScan, SharedTypes, ISR)
- Most of Model 4 (non-storable, keep params → should move to IR)
- Defer stack (#23) already moved (IR_DEFER_PUSH/FIRE)

**Analogy:** Same architecture as Rust (HIR type check → MIR borrow check), Go (type check → SSA analysis), Swift (Sema → SIL). Types first, safety second. Every compiler does this.

**ZER vs Rust pipeline comparison:**
```
Rust: AST → HIR → type check → MIR → borrow check → LLVM IR  (4 representations)
ZER:  AST →        type check → IR  → zercheck      → C        (2 representations)
```
ZER skips HIR because ZER syntax has minimal sugar (no closures, pattern matching, lifetime elision). The AST IS simple enough to type-check directly.

---

## Part 15: Concrete Lowering Examples for Tricky Patterns

These are the patterns that a fresh session implementing the lowering pass will
struggle with. Each shows the ZER source, the AST shape, and the expected IR output.

### 15.1: orelse (3 forms)

**orelse return (bare):**
```zer
*Task t = pool.alloc() orelse return;
```
AST: `NODE_VAR_DECL { init: NODE_ORELSE { expr: NODE_CALL(pool.alloc), fallback_is_return: true } }`

IR:
```
  %_or0 = IR_POOL_ALLOC pool
  BRANCH %_or0.has_value -> bb_ok, bb_fail

bb_fail:
  IR_DEFER_FIRE
  RETURN void

bb_ok:
  %t = %_or0.value
  ... continue ...
```

**orelse { block }:**
```zer
*Task t = pool.alloc() orelse { cleanup(); return; };
```
IR:
```
  %_or0 = IR_POOL_ALLOC pool
  BRANCH %_or0.has_value -> bb_ok, bb_fail

bb_fail:
  IR_CALL cleanup()
  IR_DEFER_FIRE
  RETURN void

bb_ok:
  %t = %_or0.value
  ... continue ...
```

**orelse default_value:**
```zer
u32 val = maybe_func() orelse 0;
```
IR:
```
  %_or0 = IR_CALL maybe_func()
  BRANCH %_or0.has_value -> bb_has, bb_default

bb_has:
  %val = %_or0.value
  GOTO bb_join

bb_default:
  %val = 0
  GOTO bb_join

bb_join:
  ... continue using %val ...
```

Note: all three orelse forms produce the same IR PATTERN (branch on has_value).
The variation is in the fallback block content. This eliminates the emitter's
current 3-path orelse dispatch.

### 15.2: Handle Auto-Deref

```zer
Pool(Task, 8) pool;
Handle(Task) h = pool.alloc() orelse return;
u32 id = h.id;
h.id = 42;
```
AST: `h.id` is `NODE_FIELD(NODE_IDENT(h), "id")` with Handle auto-deref.
Checker resolves `h.id` → `pool.get(h).id`.

IR (lowering inserts the get):
```
LOCALS:
  %0: h    : Handle(Task)
  %1: id   : u32
  %2: _get : *Task          ← temp for pool.get result

bb0:
  %0 = IR_POOL_ALLOC pool   → orelse lowering (branch on has_value)
  ...
bb_ok:
  %2 = IR_POOL_GET pool, %0           ← explicit get
  %1 = FIELD_READ %2, "id"            ← read through pointer
  ...
  %2 = IR_POOL_GET pool, %0           ← another get for write
  FIELD_WRITE %2, "id", 42            ← write through pointer
```

The lowering makes the pool.get EXPLICIT. Currently the emitter discovers this
at emission time by checking TYPE_HANDLE. With IR, the lowering does it once.

Note: `_get` temp is non-storable — same as current. But in IR it's just a LOCAL
that zercheck knows is from IR_POOL_GET (transient, not cacheable).

### 15.3: Shared Struct Auto-Locking

```zer
shared struct Counter { u32 value; }
Counter g;
g.value += 1;
```
Currently the emitter wraps each statement accessing `g` with lock/unlock.
The checker detects shared struct access and groups by statement.

IR (lowering inserts lock/unlock):
```
bb0:
  IR_LOCK g                            ← mutex lock
  %_tmp = FIELD_READ &g, "value"
  %_tmp2 = %_tmp + 1
  FIELD_WRITE &g, "value", %_tmp2
  IR_UNLOCK g                          ← mutex unlock
```

The lowering pass determines lock scope (per-statement grouping, same as current
checker's `Shared Type Collect`). Lock/unlock become explicit IR instructions.
Deadlock detection: scan for two IR_LOCK without intervening IR_UNLOCK in same block.

### 15.4: Spawn Wrapper

```zer
void worker(u32 x, *Task t) { ... }
spawn worker(42, &task);
```
Currently the emitter pre-scans for NODE_SPAWN, creates wrapper structs and
functions, then references them at spawn emission.

IR:
```
bb0:
  IR_SPAWN worker, args=[42, &task], is_scoped=false
```

The IR_SPAWN instruction carries the function name and args. The C emitter
handles wrapper struct generation — this is EMISSION concern, not analysis concern.
The IR doesn't need to know about wrappers. zercheck sees IR_SPAWN and checks
args for non-shared pointers, marks variables as HS_TRANSFERRED.

### 15.5: Defer Chains

```zer
void transfer() {
    mutex_lock(&lock);
    defer mutex_unlock(&lock);
    *Task t = pool.alloc() orelse return;
    defer pool.free(t);
    process(t);
}
```
IR:
```
LOCALS:
  %0: t : Handle(Task)

bb0:
  IR_CALL mutex_lock(&lock)
  IR_DEFER_PUSH [IR_CALL mutex_unlock(&lock)]     ← defer #0

  %_or0 = IR_POOL_ALLOC pool
  BRANCH %_or0.has_value -> bb_ok, bb_fail

bb_fail:
  IR_DEFER_FIRE                                     ← fires defer #0 (mutex_unlock)
  RETURN void

bb_ok:
  %0 = %_or0.value
  IR_DEFER_PUSH [IR_POOL_FREE pool, %0]            ← defer #1

  IR_CALL process(%0)

  IR_DEFER_FIRE                                     ← fires defer #1 then #0 (LIFO)
  RETURN void
```

Key: IR_DEFER_FIRE fires ALL pending defers in LIFO order. The lowering pass
inserts IR_DEFER_FIRE before EVERY exit (return, goto-out-of-scope, break, continue).
Currently the emitter does this in `emit_defers()`. With IR, the lowering does it
once, and the emitter just emits the deferred calls.

### 15.6: Switch with Union Capture

```zer
union Data { u32 num; f32 flt; }
Data d;
switch (d) {
    .num => |val| { process_int(val); }
    .flt => |*v| { v[0] = 3.14; }
}
```
IR:
```
LOCALS:
  %0: d       : Data (union)
  %1: val     : u32           ← value capture
  %2: v       : *f32          ← pointer capture

bb0:
  %_tag = FIELD_READ &d, "_tag"
  BRANCH (%_tag == 0) -> bb_num, bb_check_flt

bb_check_flt:
  BRANCH (%_tag == 1) -> bb_flt, bb_exit

bb_num:
  %1 = FIELD_READ &d, "num"              ← value capture: copy
  IR_CALL process_int(%1)
  GOTO bb_exit

bb_flt:
  %2 = FIELD_ADDR &d, "flt"              ← pointer capture: address
  FIELD_WRITE %2, [0], 3.14
  GOTO bb_exit

bb_exit:
  ... continue ...
```

Both capture types (value |val| and pointer |*v|) become explicit LOCALs.
The lowering determines capture type from the AST (capture_is_ptr flag).

### 15.7: Async Function with Multiple Yield Points

```zer
async void pipeline(?u32 input) {
    if (input) |val| {
        u32 doubled = val * 2;
        yield;
        u32 result = doubled + 1;
        yield;
        output(result);
    }
}
```
IR:
```
LOCALS:
  %0: input    : ?u32    (param)
  %1: val      : u32     (capture)    ← in state struct
  %2: doubled  : u32     (variable)   ← in state struct
  %3: result   : u32     (variable)   ← in state struct

bb0:                                   ← state 0
  BRANCH %0.has_value -> bb1, bb_end

bb1:
  %1 = %0.value
  %2 = %1 * 2
  YIELD                                ← state 1 — splits block

bb2:                                   ← resume point (state 1)
  %3 = %2 + 1
  YIELD                                ← state 2 — splits block

bb3:                                   ← resume point (state 2)
  IR_CALL output(%3)
  GOTO bb_end

bb_end:
  RETURN
```

ALL locals (%0-%3) are in the state struct. YIELD splits blocks. The Duff's device
emitter maps state N to case N:. No enumeration of which locals need promotion —
the LOCAL list IS the struct.

### 15.8: Nested @critical + Defer + Return

```zer
u32 read_safe() {
    @critical {
        u32 val = read_hw_reg();
        if (val == 0) { return 0; }
    }
    return 1;
}
```
IR:
```
LOCALS:
  %0: val : u32

bb0:
  IR_CRITICAL_BEGIN
  %0 = IR_CALL read_hw_reg()
  BRANCH (%0 == 0) -> bb_early_ret, bb_continue

bb_early_ret:
  IR_CRITICAL_END                      ← must re-enable before return!
  RETURN 0

bb_continue:
  IR_CRITICAL_END                      ← normal exit
  RETURN 1
```

Key: the lowering pass inserts IR_CRITICAL_END before EVERY exit from the @critical
block (same as IR_DEFER_FIRE pattern). Currently the emitter does this with
`critical_depth` tracking. With IR, it's explicit instructions — can't be forgotten.

Note: the checker's existing `critical_depth > 0` check for return/break/continue/goto
is REPLACED by the lowering pass inserting IR_CRITICAL_END. The ban on
`return in @critical` becomes unnecessary — the lowering makes it safe by inserting
the re-enable. BUT — this changes the language semantics (currently banned, would
become allowed). Decision: keep the ban for now, revisit post-IR.

---

## Part 16: Cross-Module IR

### Question: Does Each Imported Module Get IR?

**Answer: No.** Only the function being analyzed gets lowered to IR.

The checker processes ALL modules (imported + main) on AST. The typemap covers
all modules. When lowering a function in the main module, and it calls a function
from an imported module, the lowering just emits IR_CALL with the callee name.

FuncProps (function summaries) handle the transitive analysis. If zercheck needs
to know "does callee free its parameter?", it reads the callee's FuncSummary
(already computed). It does NOT need the callee's IR.

**Cross-module flow:**
```
imported module → AST → checker → FuncSummary (on Symbol)
                                    ↑
main module → AST → checker → lower to IR → zercheck reads FuncSummary for callees
                                           → emit C from IR
```

The imported module's C emission still goes through AST (emit_file_no_preamble).
Only the main module (or all modules, incrementally) moves to IR emission.

**Future:** lower ALL modules to IR. But not required for Phase 1. Start with
main module only. Imported modules still use AST emission path.

### What About Inlining?

ZER doesn't inline (GCC handles inlining after C emission). The IR never needs
to inline a callee's body. IR_CALL is always a call, never expanded.

---

## Part 17: Comptime and IR

### Question: Do Comptime Functions Get Lowered to IR?

**Answer: No.** Comptime functions are evaluated at COMPILE TIME by the checker's
`eval_comptime_block()`. They produce constant values that are inlined at call sites.
By the time lowering runs, comptime calls are already resolved to NODE_INT_LIT
or comptime_struct_init values.

```zer
comptime u32 MAX(u32 a, u32 b) { if (a > b) { return a; } return b; }
u32 x = MAX(10, 20);  // checker resolves to: u32 x = 20;
```

The lowering pass sees `NODE_VAR_DECL x = NODE_INT_LIT(20)`. No comptime involved.
The comptime function body is never lowered to IR.

### What About comptime if?

```zer
comptime if (PLATFORM() == 1) {
    // this code is emitted
} else {
    // this code is stripped
}
```

The checker resolves comptime if at check time. Only the taken branch appears
in the AST that reaches the lowering pass. The dead branch is already gone.

### What About static_assert?

```zer
static_assert(BUF_SIZE() >= 64, "buffer too small");
```

Evaluated by checker. Never reaches lowering. Not in IR.

### Summary: Comptime is Checker-Only

```
comptime function → checker evaluates → constant value in AST → lowering sees constant
comptime if       → checker strips dead branch → lowering sees only taken branch
static_assert     → checker checks → nothing in AST for lowering
```

The IR never handles comptime. The checker resolves everything before lowering starts.

---

## Part 18: The Design Journey — From Flags to IR

This section captures the REASONING that led to the IR decision. A fresh session
should understand not just WHAT we're building but WHY we arrived here.

### Stage 1: Flag Checks (initial approach)

When NODE_YIELD was added (v0.2.2), the checker used context flags:
```c
case NODE_YIELD:
    // Should check: c->critical_depth, c->defer_depth
    // But nobody added these checks
    break;
```

The flag-handler matrix audit found 5 missing checks. Root cause: N operations ×
M contexts = N×M manual checks. Each new operation or context requires updating
all handlers. Forgettable.

### Stage 2: Function Summaries (FuncProps)

Instead of checking flags at each NODE_ handler, scan function bodies for
properties (can_yield, can_spawn, can_alloc) and check at context entry points
(@critical, defer, interrupt). One scan, transitive, cached.

This fixed the 5 bugs and added transitive detection (calling a function that
yields inside @critical = error). Implemented as tracking system #29.

BUT: FuncProps only covers context safety. Doesn't fix zercheck CFG hacks,
VRP manual tracking, emitter complexity, async capture enumeration.

### Stage 3: The Model Question

We asked: what's ZER's safety MODEL? Investigated:
- Rust: one model (ownership types) — requires trait system ZER doesn't have
- Zig: philosophy + runtime checks — ZER has no runtime
- Effects: language feature — ZER sees all bodies, inference sufficient
- Function summaries: inferred properties — works for context safety

Discovered: ZER has 4 implicit models (verified against all 29 systems):
1. State Machines — entity lifecycle (handle states, move tracking)
2. Program Point Properties — values at control flow points (VRP, escape, provenance)
3. Function Summaries — per-function properties (FuncProps, zercheck summaries)
4. Static Annotations — set-once declarations (qualifiers, MMIO, keep)

The 4 models are correct and sufficient. The problem isn't the models — it's
the IMPLEMENTATION (29 AST walkers).

### Stage 4: The IR Decision

Analysis of 500+ bugs showed 60-70% are caused by AST-direct analysis:
- Incomplete enumeration (async captures, TYNODE gaps, flag checks)
- zercheck CFG hacks (linear walk, backward goto workaround)
- VRP manual tracking (string keys, permanent address_taken)
- Emitter complexity (3 orelse paths, optional variants, Handle dispatch)

Every production compiler at ZER's scale has an IR. ZER outgrew AST-only.
MIR-inspired design: flat locals + basic blocks + tree expressions.
Still emits C → GCC. The IR is internal — replaces AST walking, not GCC.

### The Key Insight

The 4 safety models don't change. The IR changes HOW they're implemented:
- State machines: zercheck on IR (LOCAL ids, not string keys)
- Point properties: VRP on IR (per-block ranges, not manual tracking)
- Function summaries: scan IR instructions (not recursive AST walk)
- Static annotations: unchanged (checker-level, stays on AST)

The models are the WHAT. AST vs IR is the HOW.

### Why Not Earlier?

ZER started from chibicc (no IR, no safety analysis). Safety passes were added
incrementally onto the AST-direct architecture. Each pass was small (~200 lines)
and worked. The pain accumulated gradually — no single moment said "add IR now."

The trigger was the async capture ghost bug: a memory safety violation caused by
incomplete AST enumeration. Combined with the bug history analysis (60-70% from
AST walking), the case became clear.

Rust had the same journey: AST-direct → MIR added 4 years into development.
Same trigger (AST too complex for analysis). Same solution (mid-level IR).

---

## Part 19: Testing Strategy for IR

### Differential Testing (Phase 4 migration)

During migration, both paths exist (AST emission + IR emission). For each test:
```bash
./zerc test.zer --emit-c -o test_ast.c    # old path (AST)
./zerc test.zer --emit-ir-c -o test_ir.c  # new path (IR)
diff test_ast.c test_ir.c                  # should be identical (or equivalent)
```

"Equivalent" not "identical" — the IR path may produce different variable names
or slightly different C structure (goto/labels vs if/else). But the BEHAVIOR
must be identical (same test exit code).

### IR-Specific Tests

```
tests/ir/lower_basic.zer         — basic function lowers correctly
tests/ir/lower_if_capture.zer    — if-unwrap capture becomes LOCAL
tests/ir/lower_orelse.zer        — all 3 orelse forms produce correct IR
tests/ir/lower_async.zer         — async locals complete in struct
tests/ir/lower_defer_chain.zer   — defer fire inserted at all exits
tests/ir/lower_switch_capture.zer — union switch captures become LOCALs
tests/ir/lower_handle_deref.zer  — Handle auto-deref lowered to pool.get
tests/ir/lower_shared_lock.zer   — shared struct access gets lock/unlock
tests/ir/lower_spawn.zer         — spawn args in IR_SPAWN instruction
tests/ir/lower_critical.zer      — critical begin/end at all exits
tests/ir/lower_goto.zer          — goto/label → basic block edges
tests/ir/lower_nested.zer        — nested if/for/while/switch all correct
```

### IR Validation as Continuous Check

```c
/* In zerc_main.c pipeline, after lowering: */
IRFunc *ir = lower_func(arena, &checker, func_node);
if (!validate_ir(ir)) {
    fprintf(stderr, "INTERNAL ERROR: IR validation failed for %s\n", func_name);
    abort();  /* lowering bug — fail loud */
}
```

Validation runs on EVERY compilation, not just in test mode. If lowering ever
produces malformed IR, the compiler crashes with a clear message instead of
emitting wrong C that causes confusing GCC errors.

### Existing Tests as Regression Suite

All 4000+ existing tests exercise the full pipeline. During migration, every
test must produce the same exit code through the IR path. The test suite IS
the regression suite — no separate IR test suite needed for correctness.
The IR-specific tests (above) are for DEVELOPMENT (testing individual lowering
patterns during implementation), not for ongoing regression.

---

## Part 20: Open Questions for Implementation

These are decisions that will be made during implementation, not upfront:

1. **Should IR_ASSIGN keep expression trees or decompose?**
   Design says: keep trees. But if zercheck needs to inspect sub-expressions
   (e.g., `x = pool.get(h).field` — need to see pool.get), some decomposition
   may be needed. Decide during zercheck rewrite.

2. **Should basic blocks use array indices or pointers?**
   Design shows int IDs. Pointers avoid array resizing but complicate serialization.
   Decide during Phase 1 (data structures).

3. **Should defer bodies be inlined or referenced?**
   IR_DEFER_PUSH could store a pointer to the defer body AST (emitter walks it)
   or a reference to a separate IR block sequence (fully lowered). Inline is
   simpler but means the same defer body appears at every exit point.
   Decide during Phase 2 (lowering).

4. **Should the IR pretty-printer use text or graphviz?**
   Text is simpler for terminal debugging. Graphviz (dot format) shows CFG
   visually. Both are useful. Start with text, add graphviz later.

5. **Should IR be a separate compilation unit or part of checker.c?**
   Recommendation: `ir.h` (data structures) + `ir_lower.c` (lowering) +
   `ir_emit.c` (C emission). Keep zercheck in `zercheck.c` but reading IR
   instead of AST. Clean file boundaries.

6. **How to handle the `--emit-c` flag during migration?**
   Recommendation: `--emit-c` always uses current path (AST initially, IR later).
   Add `--emit-ir` for debugging (prints IR text format). No user-visible change.
