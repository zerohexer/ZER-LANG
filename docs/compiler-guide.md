# How the ZER Compiler Works

You know C. You've written linked lists, hash tables, malloc'd structs. You've never built a compiler. This guide takes you from "I know C" to "I understand every stage of zerc" in one read.

The ZER compiler is ~9,500 lines of C. You can read the entire thing in a weekend. This guide shows you how each part works, with real code from the source.

---

## The Big Picture

A compiler is a pipeline. Text goes in, a binary comes out. Every compiler in history does the same 5 steps — zerc is no different:

```
"u32 x = 5;"          your source code (text)
       |
   [ LEXER ]           chops text into tokens
       |
   [ PARSER ]          builds a tree from tokens
       |
   [ CHECKER ]         proves the tree is valid
       |
   [ ZER-CHECK ]       catches use-after-free (optional)
       |
   [ EMITTER ]         turns the tree into C code
       |
"uint32_t x = 5;"     output C (GCC takes it from here)
```

Each stage takes the previous stage's output and transforms it. They're separate files. They don't overlap. If something is broken, you know exactly which file to look at.

| File | Lines | What it does |
|------|-------|-------------|
| `lexer.c` | ~600 | Text → tokens |
| `parser.c` | ~1500 | Tokens → tree (AST) |
| `checker.c` | ~1950 | Tree → typed tree (catches errors) |
| `zercheck.c` | ~440 | Handle safety analysis |
| `emitter.c` | ~2200 | Typed tree → C code |

---

## Stage 1: The Lexer

**File:** `lexer.c` (~600 lines)
**Input:** A string of source code
**Output:** A stream of tokens

The lexer is the simplest part. It reads characters one at a time and groups them into tokens — the "words" of the language.

```c
// This ZER code:
u32 x = 5;

// Becomes these tokens:
TOK_U32    "u32"
TOK_IDENT  "x"
TOK_EQ     "="
TOK_NUMBER "5"
TOK_SEMICOLON ";"
```

The core function is `next_token()`. It skips whitespace, looks at the current character, and decides what token it is:

```c
// From lexer.c — the main scanner function (simplified)
Token next_token(Scanner *s) {
    skip_whitespace(s);

    char c = peek(s);

    // Single-character tokens
    if (c == '(') { advance(s); return make_token(s, TOK_LPAREN, ...); }
    if (c == '{') { advance(s); return make_token(s, TOK_LBRACE, ...); }
    if (c == ';') { advance(s); return make_token(s, TOK_SEMICOLON, ...); }

    // Numbers: 42, 0xFF, 0b1010
    if (is_digit(c)) return scan_number(s);

    // Strings: "hello"
    if (c == '"') return scan_string(s);

    // Identifiers and keywords: x, u32, if, return
    if (is_alpha(c) || c == '_') return scan_identifier(s);

    // ...
}
```

### How keywords work

When the lexer sees `u32`, it first scans it as an identifier ("u32"). Then it checks a keyword table — if the identifier matches a keyword, it returns the keyword token instead:

```c
// From lexer.c — keyword detection (trie-style)
static TokenType check_keyword(const char *word, int word_len) {
    switch (word[0]) {
    case 'u':
        if (word_len == 3 && memcmp(word+1, "32", 2) == 0) return TOK_U32;
        // ...
    case 'i':
        if (word_len == 2 && word[1] == 'f') return TOK_IF;
        if (word_len == 6 && memcmp(word+1, "mport", 5) == 0) return TOK_IMPORT;
        // ...
    case 'r':
        if (word_len == 6 && memcmp(word+1, "eturn", 5) == 0) return TOK_RETURN;
        // ...
    }
    return TOK_IDENT;  // not a keyword — it's a user identifier
}
```

This is a hand-written trie — fast and simple. No hash table needed for ~50 keywords.

### What you need to know

- Tokens point into the original source string — no copying
- Each token has a type, a pointer to its text, a length, and a line number
- The lexer never fails silently — unrecognized characters produce `TOK_ERROR`
- You almost never need to touch the lexer unless you add a new keyword

---

## Stage 2: The Parser

**File:** `parser.c` (~1500 lines)
**Input:** Token stream from the lexer
**Output:** An Abstract Syntax Tree (AST)

The parser is where structure appears. The lexer sees `u32 x = 5 ;` as flat tokens. The parser sees "a variable declaration: type is u32, name is x, initializer is the integer 5."

### What is an AST?

An AST (Abstract Syntax Tree) is a tree of nodes where each node represents one piece of your program:

```c
// This ZER code:
u32 add(u32 a, u32 b) {
    return a + b;
}

// Becomes this tree:
NODE_FUNC_DECL (name="add", return_type=u32)
  ├── param: (name="a", type=u32)
  ├── param: (name="b", type=u32)
  └── body: NODE_BLOCK
        └── NODE_RETURN
              └── NODE_BINARY (op=+)
                    ├── left: NODE_IDENT "a"
                    └── right: NODE_IDENT "b"
```

Every ZER program becomes a tree like this. The parser builds it. The checker validates it. The emitter walks it to produce C.

### The Node struct

Open `ast.h`. This is the most important data structure in the compiler:

```c
// From ast.h — the Node struct (simplified)
struct Node {
    NodeKind kind;      // what type of node (NODE_IF, NODE_BINARY, etc.)
    SrcLoc loc;         // file + line number (for error messages)

    union {
        // Each node kind has its own payload:
        struct { int64_t value; } int_lit;                    // NODE_INT_LIT
        struct { Node *left; TokenType op; Node *right; } binary;  // NODE_BINARY
        struct { Node *callee; Node **args; int arg_count; } call; // NODE_CALL
        struct { const char *name; size_t name_len; } ident;       // NODE_IDENT
        // ... one struct per node kind
    };
};
```

This is a tagged union — the `kind` field tells you which union member to read. If `kind == NODE_BINARY`, you access `node->binary.left`, `node->binary.op`, `node->binary.right`.

### How parsing works — recursive descent

The parser uses a technique called "recursive descent." Each grammar rule becomes a function:

```c
// To parse: if (condition) { body } else { else_body }
static Node *parse_if_stmt(Parser *p) {
    Node *n = new_node(p, NODE_IF);
    consume(p, TOK_LPAREN, "expected '(' after 'if'");
    n->if_stmt.cond = parse_expression(p);      // recurse into expression parser
    consume(p, TOK_RPAREN, "expected ')'");
    n->if_stmt.body = parse_block(p);            // recurse into block parser
    if (match(p, TOK_ELSE)) {
        n->if_stmt.else_body = parse_block(p);   // recurse again
    }
    return n;
}
```

The pattern is always:
1. Create a node
2. Consume expected tokens (error if wrong)
3. Recursively parse sub-expressions
4. Return the node

### Expression parsing — Pratt parser

Expressions like `a + b * c` need operator precedence (`*` before `+`). ZER uses a Pratt parser (precedence climbing):

```c
// From parser.c — precedence levels
typedef enum {
    PREC_NONE,
    PREC_ASSIGN,     // =  +=  -=
    PREC_ORELSE,     // orelse
    PREC_OR,         // ||
    PREC_AND,        // &&
    PREC_EQUALITY,   // ==  !=
    PREC_COMPARISON, // <  >  <=  >=
    PREC_TERM,       // +  -
    PREC_FACTOR,     // *  /  %
    PREC_UNARY,      // -  !  ~
    PREC_CALL,       // .  ()  []
} Precedence;
```

The parser asks: "what's the precedence of the next operator?" If it's higher than what we're currently parsing, we keep going deeper. Otherwise we stop and return what we have. This naturally handles `a + b * c` → `a + (b * c)`.

### What you need to know

- Every new syntax feature starts here — add a node kind, write a parse function
- `consume()` = "this token MUST be here or error"
- `match()` = "is this token here? if so, advance"
- `check()` = "is this token here? don't advance"
- The parser doesn't validate types or logic — that's the checker's job
- All nodes are allocated from an arena (bump allocator, never individually freed)

---

## Stage 3: The Type Checker

**File:** `checker.c` (~1950 lines)
**Input:** The AST from the parser
**Output:** A typed AST (every expression annotated with its type) + errors

This is where **90% of the bugs were** in our audit. The checker's job is to walk the AST and prove that every operation is type-safe. If you write `bool x = 42`, the checker catches it. If you write `u8 += u64`, the checker catches it.

### The Type struct

Open `types.h`. Types mirror the AST — they're tagged unions:

```c
// From types.h — the Type struct (simplified)
struct Type {
    TypeKind kind;    // TYPE_U32, TYPE_POINTER, TYPE_STRUCT, etc.

    union {
        struct { Type *inner; bool is_const; bool is_volatile; } pointer;
        struct { Type *inner; } optional;
        struct { Type *inner; uint32_t size; } array;
        struct { Type *inner; } slice;
        struct { SField *fields; uint32_t field_count;
                 const char *name; } struct_type;
        struct { SEVariant *variants; uint32_t variant_count;
                 const char *name; } enum_type;
        // ...
    };
};
```

### The typemap — connecting nodes to types

The checker builds a map from AST nodes to their resolved types. This is how the emitter later knows what C type to emit:

```c
// Checker sets this during type checking:
typemap_set(node, resolved_type);

// Emitter reads it later:
Type *t = checker_get_type(node);
```

### How checking works

The checker has two main functions: `check_expr` (for expressions) and `check_stmt` (for statements). Both are giant switch statements over node kinds:

```c
// From checker.c — check_expr (simplified)
static Type *check_expr(Checker *c, Node *node) {
    Type *result = NULL;

    switch (node->kind) {
    case NODE_INT_LIT:
        result = ty_u32;    // integer literals default to u32
        break;

    case NODE_BINARY: {
        Type *left = check_expr(c, node->binary.left);    // recurse
        Type *right = check_expr(c, node->binary.right);  // recurse

        // literal promotion: 10 + my_i32 → 10 becomes i32
        if (is_literal_compatible(node->binary.left, right)) left = right;
        if (is_literal_compatible(node->binary.right, left)) right = left;

        // type compatibility check
        if (!type_equals(left, right)) {
            checker_error(c, node->loc.line,
                "cannot mix '%s' and '%s'", type_name(left), type_name(right));
        }

        // arithmetic needs numeric types
        if (node->binary.op == TOK_PLUS || node->binary.op == TOK_MINUS) {
            if (!type_is_numeric(left)) {
                checker_error(c, node->loc.line,
                    "arithmetic requires numeric types");
            }
        }

        result = left;  // result type = left operand type
        break;
    }

    case NODE_IDENT: {
        // look up the variable name in the scope chain
        Symbol *sym = scope_lookup(c->current_scope,
            node->ident.name, node->ident.name_len);
        if (!sym) {
            checker_error(c, node->loc.line,
                "undefined identifier '%.*s'", ...);
            result = ty_void;
        } else {
            result = sym->type;
        }
        break;
    }
    // ... 30+ more cases
    }

    // store the resolved type for the emitter
    typemap_set(node, result);
    return result;
}
```

### The scope chain

Variables live in scopes. Function bodies create a scope. Blocks create a scope. `if` bodies create a scope. The checker maintains a chain:

```c
// From types.c
Scope {
    Symbol *symbols;     // variables/functions in this scope
    Scope *parent;       // enclosing scope — lookup walks up this chain
}

// Lookup: walk up until found
Symbol *scope_lookup(Scope *scope, const char *name, uint32_t len) {
    while (scope) {
        // search symbols in this scope
        for (Symbol *s = scope->symbols; s; s = s->next) {
            if (s->name_len == len && memcmp(s->name, name, len) == 0)
                return s;
        }
        scope = scope->parent;  // try parent scope
    }
    return NULL;  // not found anywhere
}
```

### Two-pass checking

The checker runs two passes on each file:

1. **Register declarations** — walk all top-level items, add structs/enums/functions to scope
2. **Check bodies** — walk function bodies, validate types, fill typemap

This allows forward references — function A can call function B even if B is defined later in the file.

### Where bugs hide

Almost every bug we found in the checker was a **missing rejection**. The checker accepted code it should have rejected:

```c
// BUG-047: bool x = 42 was accepted
// The fix: remove int→bool from is_literal_compatible()

// BUG-050: @bitcast(i64, u32_val) was accepted (different widths)
// The fix: compare type_width(target) vs type_width(source)

// BUG-060: pt.x = 99 on const capture was accepted
// The fix: walk field chain to root ident, check const flag
```

The pattern: every `checker_error()` call is a rule. If a rule exists but no test exercises it, the rule might be wrong. That's why we wrote 26 negative tests covering every rejection path.

### What you need to know

- `check_expr` returns the type of an expression
- `check_stmt` validates a statement (doesn't return a type)
- `checker_error` is called when something is wrong — the program still continues checking (to report multiple errors)
- `is_literal_compatible` handles int/float literal flexibility (10 can be u8 or u64 depending on context)
- `can_implicit_coerce` handles safe automatic conversions (u8 → u32 is fine, u32 → u8 is not)
- `type_equals` checks if two types are identical

---

## Stage 4: ZER-CHECK

**File:** `zercheck.c` (~440 lines)
**Input:** Typed AST
**Output:** Errors for handle safety violations

This is an optional pass that catches use-after-free bugs at compile time. It tracks Pool handles through the control flow:

```zer
Pool(Task, 8) tasks;
Handle(Task) h = tasks.alloc() orelse return;
tasks.free(h);
tasks.get(h).pid = 5;  // ZER-CHECK: use-after-free! h was freed
```

ZER-CHECK walks the AST and maintains a state for each handle variable: ALIVE, FREED, or UNKNOWN. When it sees `pool.free(h)`, it marks `h` as FREED. When it sees `pool.get(h)` after that, it errors.

It's path-sensitive — it handles `if/else` branches by forking the state and merging at the join point. This is the most "computer science-y" part of the compiler, but it's only 440 lines because it only tracks one thing (handle state).

### What you need to know

- ZER-CHECK is independent of the rest of the pipeline — you can skip it
- It under-approximates: if it can't prove safety, it assumes safe (no false positives in simple cases)
- It catches: use-after-free, double free, wrong pool
- It doesn't catch: every possible handle misuse in complex control flow

---

## Stage 5: The Emitter

**File:** `emitter.c` (~2200 lines)
**Input:** Typed AST (with typemap from checker)
**Output:** A `.c` file containing valid C99

This is where **most of the bugs were**. The emitter walks the typed AST and outputs C code. It sounds simple — and for most nodes it is. The complexity comes from ZER features that don't exist in C.

### Simple emission

Most nodes emit trivially:

```c
// From emitter.c — emit_expr (simplified)
case NODE_INT_LIT:
    emit(e, "%lld", node->int_lit.value);
    break;

case NODE_IDENT:
    emit(e, "%.*s", (int)node->ident.name_len, node->ident.name);
    break;

case NODE_BINARY:
    emit(e, "(");
    emit_expr(e, node->binary.left);
    emit(e, " %s ", operator_string(node->binary.op));
    emit_expr(e, node->binary.right);
    emit(e, ")");
    break;
```

`u32 x = a + b;` → `uint32_t x = (a + b);` — straightforward tree walk.

### Hard emission: optional types

ZER's `?T` doesn't exist in C. The emitter must translate it:

```c
// ZER type:     ?u32           (might have a value)
// C type:       struct { uint32_t value; uint8_t has_value; }

// ZER type:     ?*Task         (might be null)
// C type:       Task*          (NULL = none, non-NULL = some)

// ZER type:     ?void          (success or failure, no value)
// C type:       struct { uint8_t has_value; }
//               ⚠️ NO .value field — accessing it is a GCC error
```

This is the #1 source of emitter bugs. The `?void` case has only ONE field, but every other `?T` has TWO. Every code path that emits optional null (`{ 0, 0 }` vs `{ 0 }`) must check for this.

### Hard emission: orelse

`orelse` compiles to different C depending on context:

```c
// ZER: u32 x = get_val() orelse 0;
// C (var-decl path):
__auto_type _zer_or0 = get_val();
if (!_zer_or0.has_value) { /* use default */ }
uint32_t x = _zer_or0.value;

// ZER: get_val() orelse return;
// C (expression path):
({__auto_type _zer_tmp0 = get_val();
  if (!_zer_tmp0.has_value) { cleanup(); return 0; }
  _zer_tmp0.value; })
```

The expression path uses GCC statement expressions `({...})` — a GCC extension that lets you embed statements inside an expression and yield a value.

### Hard emission: builtin methods

`pool.alloc()`, `ring.push()`, `arena.alloc()` are not function calls — they're compiler-known operations that emit inline C:

```c
// From emitter.c — builtin method interception (simplified)
if (obj_type->kind == TYPE_POOL && method == "alloc") {
    // emit inline C for pool allocation
    emit(e, "({ _zer_opt_u32 r; uint32_t h = _zer_pool_alloc(...); ... })");
    handled = true;  // skip normal call emission
}
```

The pattern: check if the callee is a field access on a Pool/Ring/Arena, match the method name, emit inline C, set `handled = true` to skip the normal function call path.

### Hard emission: enum switch

ZER switch compiles to C `if/else if` chains, not C `switch`. This is because ZER switch arms can have captures (`|val|`) and multi-value matches (`.a, .b =>`), which C switch can't express cleanly:

```c
// ZER:
switch (state) {
    .idle    => { start(); }
    .running => { work(); }
}

// Emitted C:
{
    __auto_type _zer_sw0 = state;
    if (_zer_sw0 == _ZER_State_idle) {
        start();
    }
    else if (_zer_sw0 == _ZER_State_running) {
        work();
    }
}
```

This means `break` inside a ZER switch arm breaks the **enclosing loop** (not the switch) — because in C, the switch is just an `if/else` chain with no C `switch` to break out of. This is correct ZER semantics.

### The preamble

The emitter outputs a preamble at the top of every `.c` file containing:

- `#include` for stdint.h, stddef.h, string.h, stdio.h, stdlib.h
- Optional type typedefs: `_zer_opt_u32`, `_zer_opt_void`, etc.
- Slice type typedefs: `_zer_slice_u8`, `_zer_slice_u32`
- Pool runtime: `_zer_pool_alloc`, `_zer_pool_get`, `_zer_pool_free`
- Ring runtime: `_zer_ring_push`
- Arena runtime: `_zer_arena`, `_zer_arena_alloc`
- Bounds check: `_zer_bounds_check`, `_zer_trap`

The `--lib` flag strips this preamble — compile-time safety (checker) still runs, but runtime checks (bounds, traps) are not emitted.

### Where bugs hide

Emitter bugs produce valid-looking C that GCC rejects or that runs incorrectly:

```c
// BUG-064: volatile completely stripped
// ZER: volatile *u32 reg = @inttoptr(*u32, 0x40020014);
// Wrong C: uint32_t* reg = ...        (GCC optimizes away MMIO reads!)
// Fixed C: volatile uint32_t* reg = ...

// BUG-053: slice-of-slice missing .ptr
// ZER: []u8 sub = sl[1..3];
// Wrong C: &(sl)[1]      (sl is a struct, can't subscript)
// Fixed C: &(sl.ptr)[1]  (access the pointer field first)

// BUG-054: array→slice coercion missing
// ZER: process(buf);    (buf is u8[256], param is []u8)
// Wrong C: process(buf)             (buf decays to uint8_t*, not a slice struct)
// Fixed C: process(((_zer_slice_u8){ (uint8_t*)buf, 256 }))
```

### What you need to know

- `emit_expr` handles expressions, `emit_stmt` handles statements
- `emit_type` converts a Type to its C name (`TYPE_U32` → `"uint32_t"`)
- `checker_get_type(node)` retrieves the type the checker assigned to a node
- `resolve_type_for_emit(e, type_node)` converts a parser TypeNode to a semantic Type
- `handled = true` in builtin method emission means "don't emit a normal function call"
- Every `{ 0, 0 }` must check for `?void` (only `{ 0 }`)
- `__auto_type` is GCC's type inference — avoids repeating anonymous struct types
- `({...})` is GCC statement expressions — used for inline orelse and builtins

---

## Tracing Through an Example

Let's trace `u32 x = 5;` through every stage:

**1. Lexer** → 4 tokens:
```
TOK_U32 "u32"  |  TOK_IDENT "x"  |  TOK_EQ "="  |  TOK_NUMBER "5"  |  TOK_SEMICOLON ";"
```

**2. Parser** → 1 AST node:
```
NODE_VAR_DECL
  type: TYNODE_U32
  name: "x"
  init: NODE_INT_LIT (value=5)
```

**3. Checker** → validates and annotates:
```
check_stmt(NODE_VAR_DECL):
  resolve_type(TYNODE_U32) → TYPE_U32
  check_expr(NODE_INT_LIT) → ty_u32
  type_equals(TYPE_U32, ty_u32) → true ✓
  typemap_set(node, TYPE_U32)
```

**4. Emitter** → outputs C:
```c
uint32_t x = 5;
```

Now trace something harder — `u32 val = get() orelse 0;`:

**1. Lexer** → tokens: `TOK_U32 TOK_IDENT TOK_EQ TOK_IDENT TOK_LPAREN TOK_RPAREN TOK_ORELSE TOK_NUMBER TOK_SEMICOLON`

**2. Parser** → tree:
```
NODE_VAR_DECL
  type: TYNODE_U32
  name: "val"
  init: NODE_ORELSE
    expr: NODE_CALL (callee: NODE_IDENT "get")
    fallback: NODE_INT_LIT (value=0)
```

**3. Checker** →
```
check_expr(NODE_ORELSE):
  check_expr(NODE_CALL "get") → ?u32 (optional)
  type_unwrap_optional(?u32) → u32  (the inner type)
  check_expr(NODE_INT_LIT 0) → u32
  type_equals(u32, u32) → true ✓  (fallback matches)
  result = u32
```

**4. Emitter** →
```c
__auto_type _zer_or0 = get();
if (!_zer_or0.has_value) { /* orelse 0 */ }
uint32_t val = _zer_or0.has_value ? _zer_or0.value : 0;
```

---

## How to Add a New Feature

Example: adding a `do { } while (cond);` loop.

### Step 1: Lexer — add the keyword (if needed)

`lexer.h` — add `TOK_DO` to the TokenType enum.
`lexer.c` — add `"do"` to `check_keyword()`:
```c
case 'd':
    if (word_len == 2 && word[1] == 'o') return TOK_DO;
```

### Step 2: AST — add the node kind

`ast.h` — add `NODE_DO_WHILE` to the NodeKind enum, and add the union member:
```c
struct { Node *body; Node *cond; } do_while;
```

### Step 3: Parser — parse it

`parser.c` — in `parse_statement()`:
```c
if (match(p, TOK_DO)) {
    Node *n = new_node(p, NODE_DO_WHILE);
    n->do_while.body = parse_block(p);
    consume(p, TOK_WHILE, "expected 'while' after do body");
    consume(p, TOK_LPAREN, "expected '('");
    n->do_while.cond = parse_expression(p);
    consume(p, TOK_RPAREN, "expected ')'");
    consume(p, TOK_SEMICOLON, "expected ';'");
    return n;
}
```

### Step 4: Checker — type-check it

`checker.c` — in `check_stmt()`:
```c
case NODE_DO_WHILE: {
    check_stmt(c, node->do_while.body);
    Type *cond = check_expr(c, node->do_while.cond);
    if (!type_equals(cond, ty_bool)) {
        checker_error(c, node->loc.line, "do-while condition must be bool");
    }
    break;
}
```

### Step 5: Emitter — emit C

`emitter.c` — in `emit_stmt()`:
```c
case NODE_DO_WHILE:
    emit_indent(e);
    emit(e, "do ");
    emit_stmt(e, node->do_while.body);
    emit_indent(e);
    emit(e, "while (");
    emit_expr(e, node->do_while.cond);
    emit(e, ");\n");
    break;
```

### Step 6: Test it

Write a positive test (valid code runs correctly) and a negative test (invalid code produces the right error). Run `make check`.

---

## How to Fix a Bug

1. **Reproduce** — write the smallest ZER program that triggers it
2. **State what's wrong** — "X returns Y but should return Z"
3. **One debug print** — add ONE `fprintf(stderr, ...)` at the decision point
4. **Confirm root cause** — read the debug output, don't guess
5. **Fix** — should be 1-5 lines
6. **`make check`** — all 940+ tests must pass
7. **Update docs** — BUGS-FIXED.md, compiler-internals.md if patterns changed

If the fix grows beyond 10 lines, you're fixing the wrong thing. Stop and reconsider.

---

## The Mental Model

Once you've read this guide:

- **Something parses wrong?** → `parser.c`, find the `parse_*` function
- **Type error missing or wrong?** → `checker.c`, find the `NODE_*` case in `check_expr` or `check_stmt`
- **GCC rejects the emitted C?** → `emitter.c`, find the `NODE_*` case in `emit_expr` or `emit_stmt`
- **Use-after-free not caught?** → `zercheck.c`, check handle state tracking
- **New keyword needed?** → `lexer.h` + `lexer.c`
- **New syntax needed?** → `ast.h` + `parser.c` + `checker.c` + `emitter.c` + tests

The compiler is 9,500 lines. You can read it all. There is no magic.
