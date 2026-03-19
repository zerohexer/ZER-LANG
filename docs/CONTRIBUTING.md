# Contributing to ZER-LANG

You know C. You want to work on the ZER compiler. This guide gets you productive in one sitting.

## The Pipeline

Every `.zer` file goes through this pipeline:

```
source.zer
    ↓
┌─────────┐
│  LEXER  │  lexer.c — turns text into tokens (u32, if, +, {, etc.)
└────┬────┘
     ↓
┌─────────┐
│ PARSER  │  parser.c — turns tokens into an AST (tree of nodes)
└────┬────┘
     ↓
┌─────────┐
│   AST   │  ast.c/h — the tree structure. Every statement, expression,
└────┬────┘  type is a Node with a kind (NODE_IF, NODE_BINARY, etc.)
     ↓
┌─────────┐
│ CHECKER │  checker.c — walks the AST, resolves types, catches errors
└────┬────┘  "you can't add u32 to bool" — this is where that happens
     ↓
┌─────────┐
│ZER-CHECK│  zercheck.c — path-sensitive handle verification (optional pass)
└────┬────┘  catches use-after-free, double free, wrong pool
     ↓
┌─────────┐
│ EMITTER │  emitter.c — walks the typed AST, outputs valid C code
└────┬────┘
     ↓
  output.c → GCC → binary
```

**Every bug you'll fix lives in one of these files.** The pipeline is linear — each stage takes the previous stage's output and transforms it.

## File Map

```
COMPILER:
  lexer.c/h         Tokenizer. Input: string. Output: Token stream.
  parser.c/h        Parser. Input: tokens. Output: AST (Node tree).
  ast.c/h           AST node definitions + arena allocator.
  types.c/h         Semantic types (Type struct) + scope/symbol table.
  checker.c/h       Type checker. Input: AST. Output: typed AST + typemap.
  emitter.c/h       C emitter. Input: typed AST. Output: .c file.
  zercheck.c/h      Handle verification pass. Input: typed AST. Output: errors.
  zerc_main.c       Compiler driver. Orchestrates the pipeline.
  zer_lsp.c         LSP server (editor integration).

TESTS:
  test_lexer.c            218 lexer unit tests
  test_parser.c           70 parser tests
  test_parser_edge.c      88 parser edge cases
  test_checker.c          71 type checker tests
  test_checker_full.c     172 full checker tests
  test_extra.c            18 extra checker tests
  test_gaps.c             4 gap tests
  test_emit.c             76 end-to-end tests (ZER → C → GCC → run)
  test_zercheck.c         8 ZER-CHECK tests
  test_firmware_patterns.c    39 firmware E2E tests
  test_firmware_patterns2.c   41 firmware E2E tests
  test_firmware_patterns3.c   22 firmware E2E tests
  test_production.c           14 production firmware E2E tests
  test_modules/               Multi-file import tests

SPEC:
  ZER-LANG.md         Full language specification
  zer-type-system.md  Type system design decisions
  zer-check-design.md ZER-CHECK design
  BUGS-FIXED.md       All 24 bugs found and fixed
```

## How to Read the Code

### Start here: ast.h

Open `ast.h`. This defines every node kind in the language:

```c
typedef enum {
    NODE_INT_LIT,       // 42
    NODE_STRING_LIT,    // "hello"
    NODE_IDENT,         // variable_name
    NODE_BINARY,        // a + b
    NODE_UNARY,         // -x, &x, *x
    NODE_CALL,          // func(args)
    NODE_FIELD,         // obj.field
    NODE_IF,            // if (cond) { ... }
    NODE_FOR,           // for (init; cond; step) { ... }
    NODE_FUNC_DECL,     // u32 add(u32 a, u32 b) { ... }
    NODE_STRUCT_DECL,   // struct Point { u32 x; u32 y; }
    // ... etc
} NodeKind;
```

Every ZER program becomes a tree of these nodes. The parser creates them, the checker validates them, the emitter translates them to C.

### The Node struct (tagged union)

```c
struct Node {
    NodeKind kind;      // what type of node
    SrcLoc loc;         // file + line number

    union {
        struct { int64_t value; } int_lit;              // NODE_INT_LIT
        struct { Node *left; TokenType op; Node *right; } binary; // NODE_BINARY
        struct { Node *callee; Node **args; int arg_count; } call; // NODE_CALL
        // ... one struct per node kind
    };
};
```

This is the chibicc pattern — one struct with a union of all possible payloads. You check `node->kind` then access the right union member.

### types.h — the type system

```c
struct Type {
    TypeKind kind;    // TYPE_U32, TYPE_POINTER, TYPE_STRUCT, etc.

    union {
        struct { Type *inner; bool is_const; } pointer;   // TYPE_POINTER
        struct { Type *inner; } optional;                  // TYPE_OPTIONAL
        struct { SField *fields; uint32_t field_count;
                 const char *name; } struct_type;          // TYPE_STRUCT
        // ...
    };
};
```

Types are resolved by the checker from `TypeNode` (what the programmer wrote) to `Type` (what it means semantically).

### The typemap

The checker stores a mapping from AST nodes to their resolved types:

```c
// checker sets this during type checking:
typemap_set(node, resolved_type);

// emitter reads it later:
Type *t = checker_get_type(node);
```

This is how the emitter knows to emit `->` vs `.` for field access — it checks if the object type is `TYPE_POINTER`.

## Key Patterns

### Arena Allocator (ast.c)

All AST nodes, types, and symbols are allocated from an arena — one big memory block.

```c
Arena arena;
arena_init(&arena, 256 * 1024);   // 256KB initial block
Node *n = arena_alloc(&arena, sizeof(Node));  // bump pointer, never free individually
// ... use nodes ...
arena_free(&arena);  // free everything at once
```

The arena grows by chaining new blocks — never invalidates existing pointers.

**Why this matters:** You never `free()` individual nodes. If you see a memory bug, it's not a use-after-free on a node — nodes live until `arena_free`.

### Scope Chain (types.c)

Variables are stored in a scope chain:

```c
Scope {
    Symbol *symbols;     // variables/functions in this scope
    Scope *parent;       // enclosing scope
}
```

`scope_lookup(scope, "x", 1)` walks up the chain until it finds `x`. Function bodies create a new scope. Blocks create a new scope.

### Two-Pass Checking (checker.c)

The checker runs two passes on each file:

1. **Register declarations** — walk all top-level items, add structs/enums/functions to scope
2. **Check bodies** — walk function bodies, validate types, fill typemap

This allows forward references — function A can call function B even if B is defined later.

### Defer Stack (emitter.c)

`defer` statements are pushed onto a stack. At block end, they're emitted in reverse. At `return`, ALL defers fire. At `break`/`continue`, only defers within the loop scope fire.

```c
int defer_base = e->defer_stack.count;    // remember where this block's defers start
// ... emit block contents ...
emit_defers_from(e, defer_base);          // emit only this block's defers
e->defer_stack.count = defer_base;        // pop them
```

### Optional Types

`?T` (non-pointer) → `struct { T value; uint8_t has_value; }`
`?*T` (pointer) → `T*` with NULL as sentinel (no struct wrapper)

The emitter handles these differently. When you see `has_value` checks in emitted C, that's `?T`. When you see null checks, that's `?*T`.

## Where to Look When Something Breaks

| Symptom | File | Area |
|---|---|---|
| "unexpected character" | `lexer.c` | `next_token()` |
| "expected expression at ..." | `parser.c` | `parse_primary()` or `parse_statement()` |
| "undefined identifier" | `checker.c` | `scope_lookup()` |
| "cannot assign X to Y" | `checker.c` | `NODE_ASSIGN` in `check_expr()` |
| "expected type, got type" | `checker.c` or `types.c` | `type_equals()` / `can_implicit_coerce()` |
| GCC rejects the emitted C | `emitter.c` | the specific `emit_*` function |
| Wrong runtime behavior | `emitter.c` | emitted C is valid but semantically wrong |
| use-after-free not caught | `zercheck.c` | handle tracking logic |

## How to Add a New Feature

Example: adding a `do { } while (cond);` loop.

### Step 1: Add the token (if needed)

`lexer.h` — add `TOK_DO` to the enum.
`lexer.c` — add `"do"` to the keyword trie in `check_keyword()`.

### Step 2: Add the AST node

`ast.h` — add `NODE_DO_WHILE` to `NodeKind` enum. Add the union member:
```c
struct { Node *body; Node *cond; } do_while;
```

### Step 3: Parse it

`parser.c` — in `parse_statement()`, add:
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

### Step 4: Type-check it

`checker.c` — in `check_stmt()`, add:
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

### Step 5: Emit C

`emitter.c` — in `emit_stmt()`, add:
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

Add positive test (valid code compiles and runs) and negative test (invalid code produces error). Add to `test_emit.c` for E2E.

### Step 7: Run `make check`

All 841+ tests must pass. Then commit.

## How to Fix a Bug

Read `CLAUDE.md` § "Debugging Workflow" for the full process. Summary:

1. Write the minimal `.zer` program that triggers the bug
2. State what you expect vs what you get
3. Add ONE `fprintf(stderr, ...)` at the decision point
4. Run. Read output. Remove debug.
5. Fix (should be 1-5 lines)
6. `make check` — all tests must pass
7. Add the bug to `BUGS-FIXED.md`
8. Commit

**Red flags — stop and revert:**
- Fix growing beyond 10 lines
- Debug prints in multiple files
- More than 2 rounds without confirmed root cause

## Building and Testing

```bash
make            # build zerc compiler
make zer-lsp    # build language server
make check      # run all 841 tests + module import tests
make release    # build release binaries in release/
make clean      # remove all build artifacts
```

## Commit Rules

- `make check` must pass before every commit
- Update `BUGS-FIXED.md` when fixing bugs
- Update `README.md` when test counts or features change
