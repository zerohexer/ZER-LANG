# Compilation Tracing — `--trace`, `zread`, `ztrace`

`zerc` can **narrate itself**: for any program, show which phase / unit / function /
instruction it processed, with your **source line beside each step**. It's a tool for
*understanding and debugging the compiler*, not for end users. Everything is gated behind
flags — **zero cost when off** (no flag → totally silent, normal compile speed).

## Commands

| Command | For | Shows |
|---|---|---|
| `./zerc f.zer -o out --trace` | the terse story | semantic anchors only (parse → lower → emit → check) |
| `./zread f.zer` | **understanding** | anchors + your **source line** appended to each (no call graph) |
| `./ztrace f.zer` | the full picture | call graph + anchors + source + handle states, repeats collapsed |

`zread` / `ztrace` are thin shell wrappers (repo root) over `zerc` / `zerc-trace`.

## The flags — one reference

**CLI flags** (parsed in `zerc_main.c`):

| Flag | Effect |
|---|---|
| `--trace` | enable the semantic anchors (to stderr) |
| `--trace-calls` | also the full function call graph (only meaningful in the `zerc-trace` build) |

**Env vars** (plain shell env, *not* a `.env` file; read via `getenv`):

| Env var | Effect | Read in | Default |
|---|---|---|---|
| `ZER_TRACE_FILTER` | call-graph name filter: comma-separated prefixes, or `all` for everything | `zer_trace.c` | curated skeleton |
| `ZER_TRACE_DEPTH` | cap printed call-graph nesting depth (`0` = unlimited) | `zer_trace.c` | 0 |
| `ZER_TRACE_CONVERGE` | show the safety fixpoint's throwaway convergence passes | `zer_trace.c` | off |
| `ZER_TRACE_STATES` | print the handle-state table (`q=alive`…); `all` = include compiler temps | `zercheck_ir.c` | off (`ztrace` sets `1`) |
| `ZREAD_EXPR` | (`zread`) also include `LOWER-EXPR` expression detail | `zread` | off |

Example: `ZER_TRACE_FILTER=lower,ir_ ZER_TRACE_DEPTH=6 ./ztrace f.zer`

## How to read the output

The output is your program **walked four times** — one line per unit, with its source:

1. `=== PARSE ===` — every declaration and statement found (bottom-up: statements print before their enclosing function, because parsing is depth-first).
2. `=== TYPECHECK ===` — types resolved (quiet unless there's an issue).
3. `=== LOWER + EMIT ===` — each statement becomes IR `(into block N)`; **N jumps when an `if`/`while`/`orelse` splits the flow** into branches.
4. `=== SAFETY ===` — each IR instruction (`[check] OP @line`) + handle-state changes (`[state] q=freed`).

Line prefixes:

- `[trace] …` — a **semantic anchor** (an event). *These are the story — read these.*
- `[check] …` — one IR instruction, in the safety pass.
- `[state] …` — the handle lattice after an instruction (only for programs that `alloc`/`free`).
- indented `foo()` — the **call graph** (which function called which). Plumbing; ignore it for *understanding*, use it for *following the code flow*.

## The anchor set (why these functions and no others)

An anchor prints when a function processes **one named unit** of the compiler's data
hierarchy. That's the complete, non-redundant set — found mechanically by
`grep 'switch (.*->kind)'` plus the per-unit `for` loops:

| Unit | Anchor function |
|---|---|
| declaration | `parse_declaration` |
| statement (parse / lower) | `parse_statement` / `lower_stmt` |
| expression (lower) | `lower_expr` |
| function (emit / check) | `emit_func_decl` / `zercheck_ir` |
| instruction | `ir_check_inst` |
| handle-state | `ir_trace_states` |

Every unit passes through its anchor **exactly once per phase**, so nothing is lost. Leaf
helpers (`advance`, `match`, `arena_alloc`, `ir_find_local`) are deliberately **not**
anchored — they're plumbing, they introduce no new unit.

## How it works

- **`ZTRACE(fmt, …)`** (defined in `ast.h`) — a `do{ if (g_zer_trace) fprintf(stderr,…) }`
  macro. The probes are 1–2 line `ZTRACE` / `fprintf` calls at the anchor sites, gated on
  `g_zer_trace` (set by `--trace`) → zero cost when off.
- **Call graph** (`zer_trace.c`, `zerc-trace` build only) — compiled with
  `-finstrument-functions`; GCC calls `__cyg_profile_func_enter/exit` around every
  function, and we resolve each function *address* to its *name* by reading the binary's
  own ELF `.symtab`. (This works for `static` functions, unlike `dladdr`; the binary is
  linked `-no-pie` so a symbol's `st_value` == its runtime address — no bias math.) Every
  function in `zer_trace.c` is `__attribute__((no_instrument_function))` so the tracer
  doesn't trace itself. **Normal `zerc` is not instrumented and never links this file.**

### Design note — this mirrors GCC/LLVM

The scattered probes are **idiomatic**, not mess: GCC and LLVM put their dump/trace calls
inside each pass too. What they centralize is the same three things this does:

| Concern | LLVM | GCC | here |
|---|---|---|---|
| output macro | `LLVM_DEBUG(dbgs()<<…)` | `dump_printf` | **`ZTRACE`** |
| scattered call sites | per-pass | per-pass | the probes |
| filter by identifier | `-debug-only=<DEBUG_TYPE>` | `-fdump-tree-<pass>` | **`ZER_TRACE_FILTER`** |
| flag registry | `cl::opt` auto-register | `dumpfile.cc` options table | `getenv` inline |

The only lighter piece is flag storage (`getenv` inline vs a central `cl::opt`/`dumpfile`
registry) — right-sized for the scale.

## Build

Two binaries. After editing the compiler, rebuild whichever you use:

```sh
make zerc        # for --trace / zread
make zerc-trace  # for --trace-calls / ztrace  (instrumented; slower build)
```

Forgetting `make zerc-trace` after a compiler edit is the common gotcha — `ztrace` then
shows *stale* behavior.
