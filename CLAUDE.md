# CLAUDE.md

> **This file is loaded into EVERY session, so it holds RULES and POINTERS only.** The long
> form — case studies, per-bug history, full tables — lives in docs read on demand:
>
> | Need | Read |
> |---|---|
> | A safety question answered at N sites: its ONE query, its gate, its history | `docs/multi-site-classes.md` |
> | Process lessons with their evidence (tests, audits, harvesting, debugging, proofs) | `docs/session-playbook.md` |
> | Architecture, emission patterns, IR invariants | `docs/compiler-internals.md` (1 MB: `grep -n '^## ' docs/compiler-internals.md` is its table of contents — read the section, never the whole file) |
> | Coq / Iris / VST | `docs/proof-internals.md` |
> | What is open NOW (meant to be read whole; gated) | `docs/limitations.md` |
> | A closed entry's discussion — grep by BUG number, never read whole | `docs/limitations-closed.md` |
> | Old entries nobody re-measured — hypotheses, a triage queue | `docs/limitations-legacy.md` |
> | Past bugs and root causes | `BUGS-FIXED.md` |
> | The language as users see it | `docs/reference.md`, `ZER-LANG-LLM.md` (compact) |
>
> An older doc citing `CLAUDE.md "<section title>"` that is not below: the section moved
> verbatim (2026-11-13) — grep the title in `docs/session-playbook.md` or
> `docs/multi-site-classes.md`.
>
> **Adding a lesson: one or two lines HERE, the story in the doc that owns it.** Nothing here
> quotes counts, cell numbers or dated history — those drift. RUN the gate; count from the tree.

## ZER's Goal — Program-Consequence Coverage (Locked Architectural Commitment)

**ZER's safety claim, stated precisely:**

> ZER guarantees 100% program-consequence coverage: every wrong use of a value in ZER source code is caught at the use site. This includes embedded/firmware data — once a value crosses into ZER through a typed boundary (intrinsic return, MMIO read, cinclude function, linker symbol, asm output, source literal), it is program data and ZER verifies every program-level operation on it (bounds, type, qualifier, region, context, lifetime, ownership, escape, provenance). Hardware-consequence — peripheral side effects, datasheet-specific value correctness, silicon behavior — is floor, out of scope for any language, surfaced at the narrowest typed boundary for code review but not verified by the compiler.

### The vocabulary that must not equivocate

The word "consequence" carries two meanings; conflating them produces false claims:

- **Program-consequence** — what happens when a value is used wrongly inside ZER source. Caught at 100%. The use is in the program, so ZER owns it.
- **Hardware-consequence** — what happens when a hardware fact is wrong relative to user belief (peripheral doesn't actually clear on read, baud value is wrong-for-this-board). Floor. The fact lives in the datasheet, never enters the program, so ZER has nothing to verify.

Any external claim must use the split. Never let one word "consequence" cover both meanings — that's the equivocation that produced earlier false "100% safe" framings.

### The boundary

ZER's boundary is **provenance of facts, not provenance of values.** Once a value crosses into ZER it is program data regardless of where it came from (hardware register, file, network, literal). Once a fact lives in the datasheet rather than the source, it is hardware-domain regardless of how closely the program relates to it. ZER is 100% over program-domain (operations on values in the program). ZER is silent on hardware-domain (silicon behavior, datasheet facts).

### Why ZER exists

ZER exists because escape hatches (Rust's `unsafe`, contract-based verification that accepts wrong user claims) compromise the safety theorem. ZER's theorem holds for all programs because:

- **No in-language `unsafe` keyword.** The grammar enforces that values must enter through typed boundaries (checker.c:5601-5608 — no integer-to-pointer cast except through `@inttoptr` with mandatory `mmio`).
- **Closure argument is grammar-enforced**, not "modulo `unsafe`." This is the position Anders et al. 2024 identified as "infinite unsafe impedance" — hypothetically possible but absent from production languages. ZER occupies it.
- **No contract-based verification of hardware properties** (Definition B). User contracts about hardware can be wrong; ZER refuses to accept claims it cannot verify against reality. Instead: ZER provides explicit-intent primitives where the user's claim is visible at the use site (audit visibility) rather than hidden in contract specifications.
- **Definition A scoping discipline.** ZER verifies access mechanism structurally; semantic correctness against external substrates (hardware spec, vendor HAL, linker script) is user responsibility supplied through primitive choice. The trust gap is bounded by frozen catalog (~33 safety classes, ~12 firmware structural relationships) — not unbounded per-call contracts.

### Document map for the safety claim

- **`docs/primitives-data-races.md`** — Definition A architecture across five safety domains (memory, type, ASM, concurrency, I/O), the closure argument, anti-patterns, the drift pattern documented in §22-23
- **`docs/asm_lang_zer_safe.md`** — Level C (delegate ISA-specific to GCC) and Level D (user-extensible intrinsics with finite verifier)
- **`docs/universal_pointer.md`** — 4-axis pointer safety with 8-desiderata conservation analysis
- **`docs/firmware_safety_extensions.md`** — program-consequence vs hardware-consequence vocabulary, finite firmware shadow set, four firmware extension gaps (`@section`, region kinds as type discipline, vector table convention, linker symbols), explicit anti-patterns including the drift caught in earlier drafts
- **`docs/proof-internals.md`** — Coq+Iris+VST proof infrastructure (author review pending per proof-internals.md disclosure)
- **`docs/limitations.md`** — documented gaps that bound the "today" claim below the target (naked attribute IR-path drop, intrinsic VRP narrowing missing, bare-metal `.bss` zeroing build-system contract, ~2% opaque destructor heuristic). None permit silent program-consequence violations.

### What ZER explicitly is NOT

- Not "100% safe" without qualifier (that's the equivocation trap)
- Not "memory safe with footnotes" (that's Rust's `unsafe` problem)
- Not contract-based verification (that's SPARK; user contracts can be wrong about hardware)
- Not a verified compiler for hardware semantics (no language is; the floor exists by physics)
- Not "production-ready for any specific board" (requires vendor HAL + BSP work via cinclude)
- Not a replacement for SPARK in safety-critical certified domains (different audience, different trade-off)

### What ZER IS

- Memory-safe systems language with grammar-level closure (no in-language `unsafe`)
- 100% program-consequence coverage at the use site for every value in ZER source
- Unified safety architecture across five domains via Definition A
- Audit-visible primitive selection (user's hardware claim visible at use site, not hidden in contract spec)
- Architecture-agnostic (delegates ISA to GCC, vendor-specific to user libraries via cinclude)
- Currently shipping (publicly available, VS Code extension, GitHub repo)

### The Verification Endgame — 100% sound checker via core λZER (LOCKED DIRECTION)

Full text: `docs/session-playbook.md` "The Verification Endgame". The plan is WRITTEN:
**read `docs/unified-oracle-proved-ZER.md` before any work on unification, oracles or the
per-class safety architecture. Do NOT re-derive it.**

- **Goal:** a machine-checked 100%-SOUND checker — `accepted(p) → ∀ reachable c, ¬ bad(c)` —
  for all programs, over the REAL checker. Soundness is Rice-immune. 100% PRECISION is
  impossible and is NOT a goal: an over-rejected program never runs, so it is harmless.
  ZER is annotation-free, so the theorem certifies the auto-inference itself.
- **Method:** core λZER (~15-20 semantic primitives) with TWO judgments — a type system
  (statics; lives in `checker.c` on the AST) and an operational semantics (dynamics;
  IR-shaped) — plus a VERIFIED desugaring and a forward simulation. A missing abstract
  state is a diagram that will not close = a stuck proof. Not 53 per-node semantics.
  Never say "core = IR" (that drops the type-system half).
- **Scope:** language level. Floors, not failures: `cinclude` / C-FFI, hardware. Separate
  axis: emitter + GCC — a sound CHECKER is not a verified compiler to asm.
- **Sequencing:** not Coq-first while the C checker still moves. Core semantics + a UAF
  vertical slice first, then class by class, then one `accepted → safe` theorem.
  Oracle-driven always: spec against the SEMANTICS, never against current C.
- **Current gap:** the per-class oracles prove each transfer sound over the states LISTED,
  not that the listed states COVER the semantics.

## Critical gotchas (each cost at least one cycle — long form: `docs/session-playbook.md`)

**Build / tooling**
- **Stale objects.** The Makefile has ZERO header dependencies. After touching ANY `.h` — or
  whenever a `make zerc` build rejects trivial programs or segfaults on a test that just
  passed — `rm -f *.o src/safety/*.o` before `make zerc`. Sanitizers cannot see this class;
  `objdump` can.
- **`make check` reports failure ONLY in its exit code.** Always record `MAKE_CHECK_EXIT=$?`.
  Make stops at the first failing step, so every later gate silently never runs. Verify each
  gate by ITS OWN verdict line (table below) — never `grep 'OK — no'`, which matches an
  earlier gate. On a Windows checkout normalise CRLF first
  (`find . -name "*.sh" -exec sed -i 's/\r$//' {} +`) or the run dies before the audits.
- `make check` takes 5-25 minutes: run it in the background and wait on a status file; do not
  poll by hand (`pgrep -x make`, not `pgrep -f`). `grep -c error build.log` lies
  (`-Werror=switch` is on the command line); use `grep -cE '^[^ ]*\.c:[0-9]+:[0-9]+: error:'`.
- **`zerc f.zer -o out.c` is the checker-only probe** (a `.c` suffix means emit only; exit 1
  on a checker error). Any other `-o` path — `/dev/null` included — runs GCC, which rejects
  for reasons of its own (every `interrupt` block on hosted x86-64) and builds the exe NEXT
  TO THE SOURCE. Read the diagnostic, never the exit code.
- **Windows host (the dev machine).** Never execute compiled binaries natively and never use
  a writable bind mount (unsigned `.exe` = Defender Wacatac). Use `make docker-check`, or the
  `:ro` mount + tar-into-container pattern (playbook "Ad-hoc Docker verify"; VST and lattice
  variants beside it). An agent that compiles must be told the same; after any agent run,
  `git status` + `git diff` — a "design only" agent may still edit files.
- `test_modules/run_tests.sh` uses a relative `../zerc`: run it FROM `test_modules/`.
- `audit_type_dispatch.sh` matches inside comments: never write the literal raw kind
  comparison in a comment; in code use `type_dispatch_kind(t) == TYPE_X`.
- Coq comments NEST: a C example like `(*opaque)` inside a `(* … *)` comment opens another.

**Compiler structure**
- **Two emitter dispatch paths.** Every intrinsic / coercion / safety wrapper needs a handler
  in BOTH the AST path and the IR-rewritten path of `emitter.c` (`grep -n '"name"' emitter.c`
  must show two hits), or the IR path emits a placeholder that segfaults at run time.
- **`type_name()` rotates TWO static buffers** (types.c): a third call in one `printf` /
  `checker_error` overwrites the first. Copy each result into a local buffer.
- **Entry points.** The compile path is `zerc_main.c` → `checker_check_bodies` +
  `check_keep_inference` + `checker_post_passes_files`; `checker_check` is the LSP / unit-test
  path. Wire a new deferred pass in BOTH. A whole-program pass goes INSIDE
  `checker_post_passes_files`, which is fed every module — never a second call in the driver.
- **Never hold two different shared-struct locks at once**: per-statement locking is what
  makes lock order deadlock-free by construction.
- **An AST node rewritten in place by `ir_lower` must survive a second lowering** (record it
  through `hoist_undo` / `hoist_restore`); the shim and `test_firmware` lower twice — run
  `test_firmware` after touching `ir_lower.c`.
- **A terminator is the last instruction of its block** (`ir_add_inst_checked`; exit
  consumers ask `ir_block_is_live_return`). A `defer` push without a reachable fire is a
  hard `ir_validate` error. A new IR op needs its `ir_validate` case.
- **A ternary between two FORMAT STRINGS switches `-Wformat` off**: use two calls.
- **An exhaustive switch proves every kind was CONSIDERED, not considered correctly.** When
  you put a kind in a no-op arm, write WHY it is a no-op.
- Layout-fragile Heisenbug (vanishes under prints / ASan / `-O0`): env-gate code paths on the
  SAME binary, `gdb` break + `bt`, `objdump` the emitted code (playbook, same title).

**Escape / keep analysis** (map: compiler-internals.md "Escape & keep analysis")
- The policy for every call-result sink is ONE function, `call_result_escapes`; "which non-keep
  params may this value carry?" is ONE query, `keep_value_roots`. Call them; never re-inline.
- **Forming a reference aliases; reading a value does not.** A widened alias / escape rule
  must be rooted at `&expr` or a slice; a bare `h.field` / `s[0]` reads a value out. Reading a
  POINTER-carrying field yields a reference, a SCALAR field does not — gate every
  field-reading alias arm on the field type (`test_modules/move_user.zer` broke twice on this).
- Returning a view of a slice / pointer PARAMETER is allowed (the call site rejects a caller
  that lets a local escape through it); returning a view of a LOCAL is not.

## MULTI-SITE SAFETY — the #1 recurring bug class

The SAME semantic question is answered independently at N sites; a fix at one leaves the
siblings broken, and a NEW site / node kind / value shape is silently missed. Every hole of
this kind was red-team-found, never proof-found. **Before editing any analysis with this
shape: (1) ENUMERATE the sibling set, (2) fix / verify EVERY member — a green test at one
site proves nothing about the others, (3) add the new shape's cell to the class's gate IN
THE SAME COMMIT.** The durable end-state is ONE query + a gate, never N call sites.

- **Choosing the gate.** Sites are a `switch` on an enum → `-Werror=switch`, no `default:`
  (an if-chain over node kinds is invisible to both walker audits: convert it, do not
  extend it). Scattered sinks → an audit-script baseline (`file:content`). "Do the listed
  forms COVER the semantics?" → a Coq max-oracle.
- **A gate that has only ever printed OK is a script, not a net.** Break the compiler on
  purpose (or run the grid against a pre-fix build: `ZER_MATRIX_ZERC=<path> ./test_x_matrix`)
  and watch it go RED — this applies to the gate's SCOPE as much as to its assertion.
- **A stale gate is worse than none.** Remove the cell / baseline row when a site is unified
  away; add one when a shape appears.
- An EXEMPTION (`volatile`, `@once`, "scoped spawn is joined") is a multi-site class too:
  grep every site granting it, and check each qualifier of its written rationale is TESTED.

**Index: class → the ONE query → gate.** The full row (the sites, the history, the traps)
is in `docs/multi-site-classes.md` — READ THE ROW before touching the class. `pNN` = a
SHAPE in `tools/sink_matrix.sh`; "conc" / "hw" = `tests/test_conc_matrix.c` /
`tests/test_hw_matrix.c`.

| Class (the question) | The ONE query / helper | Gate |
|---|---|---|
| Escape / keep: is this value frame-bound? (source AND destination side) | `call_result_escapes`, `arg_is_local_derived`, `classify_escape_sink` | sink matrix — a shape with no cell is invisible |
| VRP: where an expression is evaluated vs checked (loop cond, for step, counter lower bound) | loop-carried ranges; `ast_name_mutated_or_addrd` | `tests/test_vrp_position_matrix.c` |
| VRP: is the recorded range still TRUE here? (identity, constant, invalidation, hoist, summary) | `vrp_key_root`, `vrp_const_value`, `vrp_widen_global_like`, `vrp_guard_hoist_sound`, `vrp_record_return_range` | `tests/test_vrp_fact_matrix.c` (runtime oracle) |
| Condition facts: what does a true / false condition prove? | `vrp_push_cond_facts` gated on `vrp_cond_effect_free` | vrp-position matrix |
| Return-range summary: which `return`s were unioned? | `scan_expr_orelse_returns` at every expression position | `zer_trap/return_range_orelse_in_*` |
| Is this NAME the global? (identity, not existence) | `name_is_global_here`, `name_is_shadowed_here` | p70, p71 |
| Is this name ever written? (trusting an initializer) | `ast_name_writes_r` (mode), `global_name_never_mutated` | `funcptr_global_*_bug1609` |
| Which context runs a body: main or interrupt? | `isrref_body_isr_only` | hw `isr-helper-*` / `isr-only-*` |
| A C function at the ZER boundary (prefix, `*opaque` bridge, alias, variadic, signature) | `unprefix_c_externs`, `emit_c_extern_opaque_bridge`, `emit_c_extern_alias_decl`, `variadic_arg_is_c_scalar`, `check_c_extern_signatures` | `tests/test_lib.sh` |
| Node-kind walkers | exhaustive `switch`, no `default:` | `-Werror=switch`, `walker_default_audit.sh`, `audit_walker_fields.sh` |
| Type-kind dispatch | `type_dispatch_kind()` | `audit_type_dispatch.sh` |
| A wrapper hides the inner kind (`?T`, distinct, array, struct carrying a pointer) | `type_carries_data_pointer`, `type_can_carry_pointer`, `escape_type_carries_ref` | `audit_carrier_dispatch.sh` (emitter.c excluded on purpose) |
| `volatile` race-check exemption | `volatile_global_exempt_from_race_check` | hw SITE x SHAPE volatile grid |
| Spawn-arg gates: does this arg let the child reach my memory? | `type_carries_handle`, `type_carries_nonshared_pointer` | conc CARRIER grid |
| Funcptr REACH: which function does this callback run? (spawn AND ISR sinks) | `scan_funcname_binding`, `scan_returned_funcname`, `func_forwards_param_to_spawn`, `funcptr_sig_racy_target`, `global_bound_functions`, `indirect_callee_functions`, `for_each_orelse_block`; from an argument's type: `funcptr_first_reached` | conc REACH grid + ISR sub-grid |
| Non-atomic RMW on a shared global (site x spelling x position x reach x call x aim) | `resolve_write_target_global`, `assign_reads_own_target`, `for_each_write_target`, `vf_summary`, `call_rmw_globals`, `carrier_value_global` | hw RMW FORM grid |
| Is this statement an RMW of volatile `g` — in a THREAD, or in a spawner's WINDOW? | the main checker classifies once and records a fact per statement (`vol_rmw_fact_add`); `sync_walk` reads it (`sync_vol_rmw_facts`); locked: `check_volatile_locked_rmw`. Never teach the spawn scan a spelling by hand | hw "volatile RMW at the thread sinks" |
| A copy of a volatile global held in MEMORY (split RMW) | `mem_edge_add` / `check_mem_split_rmw`, `rmw_taint_maintain`, `VfSummary.gin` | hw "RMW split through memory" |
| ISR single writer (tick-counter relaxation) | `isr_rmw_regular_code_only_reads` | hw SINGLE-WRITER grid |
| Static locals in a race scan | `static_local_record` | hw STATIC-LOCAL grid |
| When is a plain access CONCURRENT? | refcounted `after_spawn_in_func` | conc CONCURRENT-WINDOW grid |
| A window fact kept in TEXT ORDER (is the thread running HERE?) | `win_exits_live` (closed vs open), `zer_window_join_releases`, `esc_latch_enter`; proven: `lambda_zer_concurrency/window_cfg.v` | conc jump x spawn-window grid |
| Scoped-spawn borrow: what does this argument LEND? | `Symbol.borrow_root_name`, `walk_callee_globals` | p20, p32 |
| A `shared(rw)` lock held across a call | `lockchk_direct_only` ∩ `lockchk_callee_only` | conc RE-ENTRY grid |
| Lock order across a call (another INSTANCE of a held type) | `FuncSharedTypes.inst_*`, `lockchk_check_call` | p58 |
| Two shared structs in ONE spawn argument | `collect_shared_in_spawn_args` | p23 |
| An async task: what it holds, who runs while it is suspended | `async_init_keep_sig`, `ir_async_task_call`, `ir_suspend_barrier`; re-entry traps (`_zer_running`) | p40, `zer_trap/async_reentrant_poll_*` |
| Launder peel: does this wrapper keep provenance / allocation identity? | `unwrap_ptr_launder` (checker), `ir_peel_launder` (zercheck_ir); `orelse` is a JOIN → `value_frame_bound_symbol` | p15, p15b |
| Call-result VIEW: which call is this value a view of? | `value_escaping_call` | p74, p75, p76 |
| A taint "kill": does the reassignment replace the value on EVERY path? | `bk_replaces` | p72, p73 |
| Escape taint POSITION (back edge, goto, defer body) | the escape fixpoint in `check_func_body`; `esc_restore` | p61, `audit_esc_restore.sh` |
| Block lifetime: does the destination outlive the block? | `check_block_lifetime_escape` | p69 |
| A struct LITERAL's field value | `struct_init_has_local_derived` → `arg_is_local_derived` | p35 |
| Which FRAME holds an array that becomes a view? | `array_storage_frame_root`, `array_view_frame_root` | p60 |
| Forming a slice over an ARRAY (qualifiers, packed) | `reject_array_view_hazards`, `make_array_view_explicit` | — |
| Misaligned view into a packed struct | `addr_of_is_packed_field`, `packed_array_field_view` | p22 |
| View-header write (`s.len = …`) | `view_header_field` | — |
| Unique-resource COPY (Arena / Pool / Ring / Barrier …) | `unique_resource_name`, `reject_unique_resource_copy` | p21 |
| Keep inference: which non-keep params does this VALUE carry? | `keep_value_roots`, `taint_nonkeep_from_value` | p51 |
| Value-flow compatibility (8 sites; the global init is the forgotten one) | `value_flows_to` | — |
| The carrier of an incoming allocation (SLICE too) | call-result registration AND `ir_type_is_ptrish` — they must agree | p26 |
| Two SPELLINGS of one operation (`x = alloc()` vs `T x = alloc()`) | `ir_register_alloc_result` | write both spellings |
| A CALL kept inside a passthrough instruction (the assign spelling, an orelse fallback, an intrinsic argument) | `ir_passthrough_calls`, `ir_apply_passthrough_calls` | p54 |
| An allocation stored into a SLOT | `ir_register_alloc_result_compound`, `ir_global_projection_key`, `ir_carry_compounds` | p25 |
| A store / read THROUGH a pointer: which object does `*p` name? | `ir_ptr_stable_aim`, `ir_reroot_views`, `ir_view_set_of_value` | p53, p55, p57 |
| Allocation VIEW roots: which slot does `&place` name? | `ir_view_set_of_value`, `ir_place_fixed_below_root`, `ir_local_view_place` | p77 |
| View into an allocation: what does this view point into? | `ir_view_root_ident`, `keep_view_root_ident` | p19 |
| What a callee STORES of its params | `ir_ps_scan` → `ir_apply_param_stores` | p50 |
| What a callee FREES through a pointer-to-reference param | `ir_param_deref_slot`, `ir_apply_param_deref_frees` | p68 |
| A call argument that is itself a call result | `ir_resolve_returned_arg` | p49 |
| Raw-AST argument positions in zercheck_ir | call BOTH `ir_check_expr_uaf` and `ir_check_expr_wrong_pool` | `audit_walker_fields.sh` |
| A construct modelled as a FLAG (`orelse return` / `break` / `continue`) | restate every rule of the real statement beside the flag | p24 |
| Assignment TARGET before VALUE (three lowering sites) | `hoist_place_effects`, `hoist_target_indices` | `assign_target_before_value_bug1618` |
| Synthesized AST naming an IR local | `make_local_ident` | — |
| Defer-body statement coverage | ONE lowering path (`emit_defer_body`, `IRInst.defer_tpl`) | defer-goto matrix |
| Use-before-init of a resource (Arena, Barrier) | `check_resource_init` | — |
| A whole-program POST PASS sees every module | `checker_post_passes_files` | `test_modules/*_user_negative.zer` |
| Trusting a GLOBAL's declaration initializer | `global_name_never_mutated` | — |
| A constant MMIO address | `mmio_const_addr` | — |
| Does this expression mention name X? | one no-`default:` walker | — |
| Emitter dual dispatch (AST + IR) | two handlers | two `grep` hits; `emit_audit.sh` |
| Indirect call through a possibly-NULL funcptr (three call emitters) | `call_needs_null_funcptr_guard`, `emit_ir_call_callee` | `emit_audit.sh` required fingerprint |
| A value LOADED out of memory that auto-zero may have forged | `load_guard_kind`, `nn_guard_wanted` | `zer_trap/nonnull_*` |
| A forgeable scalar / enum-forge doors (exactly three guarded: `@bitcast`, `@truncate`, `@saturate`) | the emitter's forgeable-scalar guard walk, `bitcast_target_mints`, `type_carries_forgeable` | `zer_trap/*_enum_forged_*` |
| An intrinsic's ADVERTISED runtime check: for which operand types is it constant or absent? | `pun_type_id_check_can_fire` (mirrors the emitter) | checklist |
| The runtime type tag a `*opaque` carries | `opaque_type_id`, `opaque_type_id_nominal` | — |
| A write INTO a union variant that is not the whole variant | `union_partial_write_target`, `emit_union_reset_prefix` | — |
| A new value-producing op (uN / iN mask) | `emit_intn_mask` at every op | `tests/zer/uN_width_wrap_all_forms.zer` |
| Rendering a `double` into C | `emit_double_lit` | `audit_float_literal.sh` |
| Naming a slice typedef: which C type is `[*]E`? | `emit_slice_name`, `emit_ptr_to_elem` | `slice_elem_*_bug1027` |
| The C NAME of a user identifier / a module's container global | `c_reserved_ident`, `local_avoid_global_name`, `emit_module_global_name`, `emit_alloc_sym_cname`; generated names are `_zer_`-prefixed | `test_modules/arena_user.zer` |
| A privileged instruction chosen by target macros; user mode vs bare metal | the precise target macro; `_ZER_USER_MODE` | `emit_audit.sh` per-target; `tools/cross_target_sweep.sh` (by hand) |
| Documented examples still compile and run | — | `audit_reference_examples.sh` |

## What a green `make check` covers

`MAKE_CHECK_EXIT=0` AND each verdict line present — grep the SPECIFIC line:

| gate | its verdict line |
|---|---|
| `walker_audit.sh` | `OK — no gaps. IR emitter covers every node kind the AST emitter does.` |
| `walker_default_audit.sh` | `OK — no default: clauses remain in node-kind / op-kind switches.` |
| `audit_walker_fields.sh` | `OK — no new walker field-coverage gaps` |
| `audit_fixed_buffers.sh` | `OK — no new fixed-size buffer declarations.` |
| `audit_type_dispatch.sh` | `OK — no new raw type-dispatch sites.` |
| `audit_carrier_dispatch.sh` | `OK — no new hand-rolled carrier dispatches.` |
| `audit_esc_restore.sh` | `OK — every realloc'd Checker buffer is kept by esc_restore` |
| `audit_limitations.sh` | `OK — limitations.md holds open entries only` |
| `emit_audit.sh` | `OK — no dead-stub markers in emitted C` |
| `sink_matrix.sh` | `SINK MATRIX CLEAN` |
| `audit_reference_examples.sh` | `OK — every non-baselined reference.md example builds.` |
| `audit_float_literal.sh` | `OK — every emitted float literal goes through emit_double_lit.` |
| `audit_matrix.sh` | `OK — every construct x context verdict holds.` |

Plus the axis-crossed grids `tests/test_*_matrix.c` (shape, escape, keep, cflow, conc,
view-alias, hw, async, asm, defer-goto, vrp-position, vrp-fact), each printing
`=== <name>-matrix: N/N cells correct ===`, and `tests/test_lib.sh` (the standard library
in `lib/` builds and runs). A grid may assert a RUNTIME VALUE, not just accept / reject —
use that when the defect is a silent miscompile. Grids are safe to run concurrently (each
uses `ZT()` from `tests/zer_tmp.h`; a new grid must too).

**NOT in `make check` — run by hand:** `tools/ubsan_sweep.sh`, `tools/ub_sweep.sh`,
`tools/grammar_closure_probe.sh`, `tools/qualifier_closure_probe.sh`,
`tools/cross_target_sweep.sh`, the firmware examples. **Nothing in `make check` compiles
the emitted C for any target but the host**: after touching per-arch emission (`@critical`,
`@cpu_*`, barriers, the trap) run `bash tools/cross_target_sweep.sh`.

## ZER Language — Complete Quick Reference

ZER is memory-safe C. Same syntax, same mental model — but the compiler prevents buffer overflows, use-after-free, null dereferences, and silent memory corruption. Compiles to C, then GCC handles backends.

### Primitive Types
```
u8  u16  u32  u64        unsigned integers
i8  i16  i32  i64        signed integers
uN  iN                   arbitrary-width integers (u21, i48, u3 … N=1..128; native, carrier+mask, wraps at 2^N)
usize                    pointer-width unsigned (width auto-detected from the target GCC; --target-bits overrides)
f32  f64                 floating point
bool                     true / false (NOT an integer — no bool↔int coercion)
void                     return type only, cannot declare void variables
```

### Compound Types
```
u8[256] buf;             fixed array — NOTE: size AFTER type, BEFORE name
[*]u8 data;              dynamic pointer to many — {ptr, len}, bounds checked (replaces []T)
[]u8 data;               DEPRECATED — same as [*]u8, warns "use [*]T instead"
*Task ptr;               pointer — guaranteed non-null
?*Task maybe;            optional pointer — might be null (null sentinel, zero overhead)
?u32 result;             optional value — struct { u32 value; u8 has_value; }
?bool flag;              optional bool — struct { u8 value; u8 has_value; }
?void status;            optional void — struct { u8 has_value; } (NO value field!)
*opaque raw;             type-erased pointer (C's void*)
```

### Builtin Container Types
```
Pool(Task, 8) tasks;     fixed-slot allocator — compile-time count, ALWAYS global
Slab(Task) tasks;         dynamic slab allocator — grows on demand, ALWAYS global
Ring(u8, 256) rx_buf;    circular buffer — ALWAYS global
Arena scratch;            bump allocator — (over, alloc, alloc_slice, reset)
Barrier b;               thread sync point — (@barrier_init, @barrier_wait)
Handle(Task) h;          u64: index(32) + generation(32), not a pointer
Semaphore(3) slots;      counting semaphore — @sem_acquire/@sem_release, thread-safe
```

### User-Defined Containers (Monomorphization)
```
// Define parameterized struct template — NOT generics, just type stamping
container Stack(T) {
    T[64] data;
    u32 top;
}

Stack(u32) s;                // stamps concrete struct Stack_u32
Stack(Point) ps;             // stamps separate struct Stack_Point

// Functions are regular free functions, NOT methods (no `this`)
void stack_push(*Stack(u32) s, u32 val) { s.data[s.top] = val; s.top += 1; }

// T substitution works in: T, *T, ?T, []T, T[N] field types
container Wrapper(T) { *T ptr; ?T maybe; T[4] arr; }
```

### Designated Initializers
```
Point p = { .x = 10, .y = 20 };     // var-decl init
p = { .x = 100, .y = 200 };         // assignment (C99 compound literal)
Config c = { .baud = 9600 };         // partial — unmentioned fields auto-zero
```

### CRITICAL Syntax Differences from C

**These cause the most wasted turns in fresh sessions:**

1. **Braces ALWAYS required** for if/else/while/for bodies. No braceless one-liners.
   ```
   if (x > 5) { return 1; }      // OK
   if (x > 5) return 1;           // PARSE ERROR — "expected '{'"
   ```

2. **C-style casts ARE supported** for type conversion. `@saturate`, `@bitcast`, and `@pun` remain for the rare cases.
   ```
   (u32)small                     // OK — widening (or narrowing truncate)
   (f32)count                     // OK — int to float value convert
   (*opaque)sensor                // OK — *T to *opaque (sets provenance)
   (*Motor)ctx                    // OK — *opaque to *T (provenance checked)
   (*Task)t                       // OK — identity cast where t is already *Task (elided, zero overhead)
   (*u32)int_val                  // ERROR — use @inttoptr (mmio safety)
   (u32)ptr                       // ERROR — use @ptrtoint
   (*B)a_ptr                      // ERROR — types differ. Use @pun(*B, src) for explicit pun
   (*u32)volatile_ptr             // ERROR — cannot strip volatile
   (*u32)const_ptr                // ERROR — cannot strip const
   @pun(*Motor, sensor_ptr)       // OK — explicit type-punning with runtime type_id check
   @saturate(i8, big)             // OK — clamp to [-128, 127] (explicit only)
   @bitcast(u32, float_val)       // OK — reinterpret bits (explicit only)
   ```

   **Pointer-cast rule (LOCKED — added 2026-06-06, see BUGS-FIXED.md session
   2026-06-06):** Direct `(*T)src` where types match → elide, zero overhead.
   Direct `(*U)src` where types differ → compile error pointing to `@pun`.
   `@pun(*T, src)` → audit-visible escape, inlines *opaque round-trip with
   runtime type_id check that traps on mismatch. For byte parsing use `[*]u8`
   slices, not pointer casts.

3. **Variable declarations: type before name, no `struct` keyword in usage.**
   ```
   struct Task { u32 id; }        // declaration uses 'struct' keyword
   Task t;                        // usage: just the name, NO 'struct' prefix
   *Task ptr = &t;                // pointer to Task
   ```

4. **Array declaration: size between type and name.**
   ```
   u8[256] buf;                   // ZER: type[size] name
   // NOT: u8 buf[256];           // C style — won't work
   ```

5. **Enum values: dot syntax, not bare names.**
   ```
   enum State { idle, running, done }
   State s = State.idle;          // qualified access
   switch (s) {
       .idle => { ... }           // dot-prefixed in switch arms
       .running => { ... }
       .done => { ... }
   }
   ```

6. **Switch uses `=>` arrows, no fallthrough, no `case` keyword.**
   ```
   switch (x) {
       .a => { ... }              // enum arm
       .b, .c => { ... }          // multi-value arm
       default => { ... }         // default arm (required for int switches)
   }
   ```

7. **Optional unwrapping: `orelse` and `if |capture|`.**
   ```
   u32 val = maybe_func() orelse 0;           // default value
   u32 val = maybe_func() orelse return;       // propagate failure
   u32 val = maybe_func() orelse { cleanup(); return; }  // block fallback

   if (maybe_val) |v| { use(v); }             // if-unwrap with capture
   if (maybe_val) |*v| { v.field = 5; }       // mutable capture (pointer)
   ```

8. **No `++`/`--` operators.** Use `+= 1` / `-= 1`.

9. **No `malloc`/`free`.** Use Pool, Slab, Ring, or Arena builtins.

10. **String literals are `[*]u8` (slices), not `char*`.** `[*]T` is the preferred syntax — reads as "pointer to many." `[]T` still works but emits a deprecation warning ("use [*]T instead"). Use `[*]T` in all new code.
    ```
    const [*]u8 msg = "Hello";    // slice with .ptr and .len — a literal is READ-ONLY, so the slice must be const
    ```

11. **`else if` is supported.** Both forms work:
    ```
    if (a) { ... } else if (b) { ... }       // OK — parsed as nested if
    if (a) { ... } else { if (b) { ... } }   // OK — same result
    ```

12. **`orelse return` is bare — no value.** Return value comes from function's return type.
    ```
    *Task t = pool.alloc(Task) orelse return;    // OK — bare return
    *Task t = pool.alloc(Task) orelse return 1;  // PARSE ERROR
    ```

13. **`arena.alloc(T)` / `alloc(T)` / `@size(T)` take a type name, a primitive keyword,
    or a container stamp** (BUG-1556 — the last two used to be parse errors).
    ```
    arena.alloc(Task)           // OK
    arena.alloc(u32)            // OK — ?*u32
    arena.alloc_slice(u8, n)    // OK — ?[*]u8
    alloc(Box(u32))             // OK — a container stamp
    ```

14. **For loops use `i += 1`, compound init, and C-style structure.**
    ```
    for (u32 i = 0; i < 10; i += 1) { ... }   // OK
    for (u32 i = 0; i < 10; i++) { ... }       // PARSE ERROR — no ++
    ```

15. **`goto` and labels are supported.** Forward and backward jumps. Safe because auto-zero + defer neutralize goto's traditional dangers. Banned inside `defer` and `@critical` blocks.
    ```
    goto cleanup;                              // forward jump
    // ...
    cleanup:
        free_resources();
        return 1;
    ```

16. **Inline asm uses bare `asm(...)`** (renamed from `unsafe asm` 2026-04-25 —
    the keyword was cosmetic; safety keys on the structural `in_naked` flag).
    Allowed ONLY inside `naked` functions (S1 interim guard). Two forms:
    - Inline: `asm("nop");` — operand-free only. The GCC extended form
      `asm("..." : outputs : inputs : clobbers);` is a compile error since BUG-1297
      (its operands were raw text no analysis saw); operands go in the structured form.
    - Structured: `asm { instructions: "..."  inputs: { "rax" = expr }
      outputs: { ... }  clobbers: { ... }  safety: "..." }` — the `safety:`
      string (>= 30 chars) is MANDATORY (S4 audit-trail rule).
    Max 16 instructions (S2), no labels (S3), register names validated per-arch,
    10 of 13 Z-rules wired (Z1-Z8, Z11, Z12) — UAF/move/VRP/provenance/escape/
    qualifier/MMIO tracking stays ACTIVE through asm operand boundaries (unlike
    Rust, which goes blind inside `unsafe { asm!() }`). Users prefer
    `@intrinsic()` calls (130 shipping); asm is the escape hatch.
    Direction: Option E rework — `docs/asm_lang_zer_safe.md` +
    `docs/option_e_plan.md`. Per-session implementation history:
    `docs/compiler-internals.md` D-Alpha-7.5 Session A..F sections.

17. **Variadic `...` — bodyless extern declarations ONLY (2026-06-16).** For
    C-interop so `printf` et al. work:
    ```
    i32 printf(const *u8 fmt, ...);   // OK — bodyless extern
    printf("x=%d\n", 7);               // OK — call site needs >= fixed params
    void f(...);                        // ERROR — needs >= 1 named param first
    void g(*u8 s, ...) { }              // ERROR — variadic with a body is banned
    ```
    A ZER function with a body cannot be variadic (it would read untyped,
    unverified args — the unchecked-boundary ban). `...` must be the final
    parameter; ≥1 named param required before it.

### Intrinsics (@ builtins)
```
@size(T)                 sizeof — returns usize
@truncate(T, val)        keep low bits (big→small). REJECTED on a FLOAT operand — a float has no
                         low bits; use `(T)x` (saturates) or `@saturate(T, x)`. Deliberate
                         divergence from two audit branches, which made it saturate: that gives
                         one primitive two unrelated meanings by operand type, and is redundant.
@saturate(T, val)        clamp to min/max of T
@bitcast(T, val)         reinterpret bits (same width required, qualifier-checked)
@cast(T, val)            distinct typedef conversion only (qualifier-checked)
@inttoptr(*T, addr)      integer to pointer (mmio-range-validated if ranges declared)
@ptrtoint(ptr)           pointer to integer (usize)
@ptrcast(*T, ptr)        pointer type cast (provenance-tracked, qualifier-checked)
@pun(*T, src)            explicit type-pun (runtime type_id check, qualifier-checked, audit-visible escape for raw casts)
@barrier()               full memory barrier (seq_cst)
@barrier_store()         store barrier (release)
@barrier_load()          load barrier (acquire)
@barrier_acq_rel()       acquire+release fence            [D-Alpha-2]
@offset(T, field)        offsetof
@container(*T, ptr, f)   container_of (field-validated, provenance-tracked)
@trap()                  crash intentionally
@probe(addr)             safe MMIO read — returns ?u32, null if address faults
@try_enum(E, val)        CHECKED int->enum — returns ?E, null if not a declared variant
                         (@bitcast is the unchecked-but-trapping route; a bare int is REJECTED)
@unreachable()           GCC unreachable hint (UB if reached)  [D-Alpha-2]
@expect(val, expected)   branch prediction hint                [D-Alpha-2]
```

### Atomic Intrinsics (15, all SEQ_CST ordering)
```
# Load / store / cas:
@atomic_load(*T) -> T
@atomic_store(*T, val)
@atomic_cas(*T, expected, desired) -> bool

# Fetch-old (returns value BEFORE op):
@atomic_add(*T, val) -> T          # existing
@atomic_sub(*T, val) -> T
@atomic_or(*T, val) -> T
@atomic_and(*T, val) -> T
@atomic_xor(*T, val) -> T
@atomic_nand(*T, val) -> T         [D-Alpha-1]
@atomic_xchg(*T, val) -> T         [D-Alpha-1] swap, returns old

# Fetch-new (returns value AFTER op):
@atomic_add_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_sub_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_or_fetch(*T, val) -> T     [D-Alpha-1]
@atomic_and_fetch(*T, val) -> T    [D-Alpha-1]
@atomic_xor_fetch(*T, val) -> T    [D-Alpha-1]
```
All atomics: first arg is the address of an INTEGER global or struct field, width
1/2/4/8 bytes (u128 / non-native uN / float / struct are rejected). It does NOT need
to be a `shared struct` — verified 2026-08-10 that `*shared T` is NOT CONSTRUCTIBLE
(`shared` is struct-only, and you cannot take the address of a shared struct's
interior), so the old wording described a rule no program could satisfy. The real
safety net is the ATOMIC CELL rule: once a global is touched atomically in a
concurrent context, every other access must be atomic too — enforced at plain-access
sites including through helpers (BUG-769/771).
Ordering parameter (relaxed/acquire/release/acq_rel/seq_cst) deferred to later batch.

### Bit Query / Byte Swap Intrinsics (D-Alpha-2)
```
@bswap16(u16) -> u16     byte swap
@bswap32(u32) -> u32
@bswap64(u64) -> u64
@popcount(x) -> u32      count 1 bits  (dispatches on u32 vs u64 width)
@ctz(x) -> u32           count trailing zeros
@clz(x) -> u32           count leading zeros
@parity(x) -> u32        0=even / 1=odd parity
@ffs(x) -> u32           find first set bit, 1-indexed (0 if input is 0)
```
All use GCC builtins — auto-port to x86-64/ARM64/RISC-V without per-arch work.

### Carry primitives + native uN / iN
```
@addc(a, b, carry_in)  -> AddCarry64  { u64 sum;  u8 carry;  }
@subb(a, b, borrow_in) -> SubBorrow64 { u64 diff; u8 borrow; }
@mulw(a, b)            -> MulWide64   { u64 lo;   u64 hi;     }
```
`uN` / `iN` (N = 1..128) are native: the carrier is the smallest native int ≥ N and
arithmetic is masked to N bits (`emit_intn_mask`), so odd widths wrap at 2^N. An over-width
bit-slice write `reg[hi..lo] = LIT` is a compile error. Single-bit: `reg[5..5]`.

### CPU / privileged intrinsics (130 in all — catalog: `docs/reference.md`; signatures by batch: playbook)
Interrupt control (`@cpu_disable_int`, `@cpu_enable_int`, `@cpu_wait_int`,
`@cpu_save_int_state`, `@cpu_restore_int_state`), context switch (`@cpu_save_context`,
`@cpu_restore_context`, `@cpu_save_fpu`, `@cpu_restore_fpu` — a `*u8` or `u8[N]` buffer),
MSR / CR / XCR0 (`@cpu_read_msr`, `@cpu_write_msr`, `@cpu_read_cr3`, …), inspection
(`@cpu_read_sp`, `@cpu_cpuid`, `@cpu_vendor_id`, …), power (`@cpu_deep_sleep`, …), mode
transitions (`@cpu_syscall`, `@cpu_iret`, …), port I/O (`@port_in8` … `@port_out32`), cache /
non-temporal (`@cache_flushopt`, `@nt_store`), FPU state (`@cpu_fxsave`, `@cpu_xsave`).
Privileged ones fault in user mode — test them in a DEAD BRANCH so they compile without
executing: `volatile u32 never_true = 0; if (never_true == 42) { @cpu_disable_int(); }`.

### `asm`
Bare `asm(...)`, only inside a `naked` function; the structured form's `safety:` string
(≥ 30 chars) is mandatory; max 16 instructions, no labels. Operand-boundary tracking (UAF,
move, VRP, provenance, escape, qualifier, MMIO) stays active through asm operands. Z1 / Z2
live in `zercheck_ir.c`, Z3..Z13 in `checker.c`. **Direction is LOCKED (Option E): read
`docs/asm_lang_zer_safe.md` + `docs/option_e_plan.md` before touching asm safety; do not grow
per-instruction tables.** Mnemonics may contain dots (`lr.w`).

### mmio Declaration (MANDATORY for @inttoptr)
```
mmio 0x40020000..0x40020FFF;   // declare valid MMIO address range
mmio 0x40011000..0x4001103F;   // multiple ranges allowed
// @inttoptr with constant address outside ranges → compile error
// @inttoptr with variable address → runtime range check + trap
// No mmio declarations + @inttoptr → compile error (strict by default)
// --no-strict-mmio flag: allow @inttoptr without mmio declarations
// For tests: mmio 0x0..0xFFFFFFFFFFFFFFFF; (allow all addresses)
```

### Provenance Tracking (@ptrcast + @container)
```
// @ptrcast tracks original type through *opaque round-trips:
*opaque ctx = @ptrcast(*opaque, &sensor);  // provenance = *Sensor
*Sensor s = @ptrcast(*Sensor, ctx);        // OK — matches provenance
*Motor m = @ptrcast(*Motor, ctx);          // COMPILE ERROR — wrong type

// @container validates field existence + tracks &struct.field origin:
*ListHead ptr = &dev.list;                  // provenance = (Device, list)
*Device d = @container(*Device, ptr, list); // OK — proven
*Other o = @container(*Other, ptr, list);   // COMPILE ERROR — wrong struct

// Unknown provenance (params, cinclude) → allowed (can't prove wrong)
// Propagates through aliases and clears+re-derives on assignment
```

### Function Pointers — Two Syntaxes (both supported, same TYNODE_FUNC_PTR)

**Variant 2A — C-style (compatible with kernel C, requires typedef for some forms):**
```
u32 (*fn)(u32, u32) = add;              // local variable
void (*callback)(u32 event);             // global variable
struct Ops { u32 (*compute)(u32); }      // struct field
u32 apply(u32 (*op)(u32, u32), x, y);   // parameter
?void (*on_event)(u32) = null;           // optional — null sentinel
typedef u32 (*BinOp)(u32, u32);          // function pointer typedef
typedef ?u32 (*OptHandler)(u32);         // typedef with optional return
BinOp[4] ops;                            // array of function pointers (via typedef)
```

**Variant 2C — ZER-native (typedef-free everywhere, follows `*T name` convention, 2026-04-25):**
```
*(u32, u32) -> u32 fn = add;             // local variable — name LAST, type first
*(u32 event) callback;                    // void return = no arrow
struct Ops { *(u32) -> u32 compute; }     // struct field
u32 apply(*(u32, u32) -> u32 op, x, y);  // parameter
?*(u32) -> u32 on_event = null;           // nullable funcptr
*(u32) -> ?u32 lookup;                    // funcptr returning optional
*(u32, u32) -> u32 [4] ops;               // array — INLINE, no typedef!
*(u32, u32) -> u32 select_op(u32 kind);   // return type — INLINE, no typedef!
*(keep *Handler) register_fn;              // keep on individual params
```

Discriminator: `*` BEFORE `(` = 2C, `*` INSIDE `(` = 2A. Parser branches on this. Both produce identical `TYPE_FUNC_PTR` so checker/IR/emitter/zercheck_ir/all 29 safety systems are operator-agnostic.

**`?` on funcptr — context-dependent (BUG-420):**
- 2A: `?RetType (*name)(params)` (var/param/field/global) → **nullable funcptr**
- 2A: `typedef ?RetType (*Name)(params)` → **funcptr returning optional**
- 2C: `?*(args) -> R` → nullable funcptr, `*(args) -> ?R` → funcptr returning optional. Position is unambiguous, no special typedef rule.

### Field access: `.` everywhere (no `->` for indirection)

ZER uses `.` for ALL field access — values, pointers, Handles. No `->` operator in expression context. Modeled on Rust/Zig (both modern systems langs use `.` everywhere).

```
Task v;          v.field;       // value — direct
*Task ptr;       ptr.field;     // pointer — auto-deref
Handle(T) h;    h.field;        // Handle — auto-lookup via slab.get + gen check
```

The `->` thin arrow is **type-level only** — appears solely as the return-type separator in 2C funcptr syntax. Never in expression context. No collision.

Why `.` everywhere (decided 2026-04-25 after deep design discussion):
- Refactor safety: change `*Task next` to `Handle(Task) next` and call sites unchanged
- Chain ergonomics: `bob.list.next.prev` reads uniformly (no `bob->list.next->prev` alternation)
- Modern systems-lang convention: Rust, Zig both use `.` everywhere — proven in kernel/embedded
- 29 safety systems all run at IR/Symbol/Type level — operator is parser-only, no semantic loss
- Mental model still taught by VISIBLE TYPES (`*T`, `?*T`, `Handle(T)`), not by deref operator

### Defer
```
void f() {
    *u8 buf = alloc();
    defer free(buf);                     // runs at scope exit, in reverse order
    if (error) { return; }               // defer fires on ALL return paths
}
```

### Comptime (compile-time evaluation — replaces C macros + #ifdef)
```
// comptime functions — type-checked, zero runtime cost:
comptime u32 BIT(u32 n) { return 1 << n; }
comptime u32 MAX(u32 a, u32 b) {
    if (a > b) { return a; }
    return b;
}
u32 mask = BIT(3);            // → 8 at compile time
u32 big = MAX(10, 20);        // → 20 at compile time
// All args MUST be compile-time constants — variables rejected
// Nested calls work: comptime u32 QUAD(u32 x) { return DOUBLE(DOUBLE(x)); }
// Const idents as args: const u32 P = BIT(0); comptime if (HAS(P, 1)) { ... }

// comptime if — conditional compilation (#ifdef replacement):
comptime if (1) {
    // this code is checked and emitted
} else {
    // this code is stripped entirely
}
// Condition: literal, const, comptime call, or expression of these
// Comptime calls in conditions: comptime if (FUNC()) { ... }
// Comparisons: comptime if (VER() > 1) { ... }
// const from comptime: const u32 P = PLATFORM(); comptime if (P) { ... }
// Only the taken branch is type-checked — dead branch ignored
```

### Thread Safety
```
// shared struct — auto-locked, no annotations needed:
shared struct Counter { u32 value; u32 total; }
Counter g;
g.value = 42;              // auto: lock → write → unlock
g.total = g.value + 1;     // SEPARATE lock scope — locking is PER-STATEMENT, NOT
                           // grouped (current_stmt_shared_root, ir_lower.c). Two
                           // statements = two lock/unlock pairs; the lock is
                           // released between them. This is what makes cross-
                           // statement lock ordering deadlock-free; it also means
                           // multi-statement check-then-act is NOT atomic (see the
                           // `locked g {}` design discussion in
                           // docs/primitives-data-races.md §24).

// spawn — fire-and-forget thread creation:
spawn worker(&g);           // OK — *shared struct, auto-locked
spawn process(&data);       // ERROR — non-shared pointer, data race
spawn handler(42, true);    // OK — value args copied, no sharing

// scoped spawn — ThreadHandle + join (allows *T args):
ThreadHandle th = spawn compute(&work);  // OK — *T allowed, will be joined
th.join();                  // MUST call before function returns
// zercheck: ThreadHandle not joined = compile error

// condvar — thread synchronization:
@cond_wait(shared_var, shared_var.count > 0);  // wait for condition
@cond_signal(shared_var);   // wake one waiting thread
@cond_broadcast(shared_var); // wake all waiting threads
// shared structs using condvar auto-upgrade to pthread_mutex_t

// threadlocal — per-thread storage:
threadlocal u32 counter;    // each thread has its own copy (__thread)

// Lock ordering — deadlock prevention (BUG-464 redesign):
shared struct A { u32 x; }
shared struct B { u32 y; }
A a; B b;
a.x = 1; b.y = 2;          // OK — separate statements, no nested locks
b.y = 2; a.x = 1;          // OK — separate statements, locks released between
a.x = b.y;                 // ERROR — same statement accesses both A and B
```


> **Concurrency safety status — read before claiming anything.** Do NOT claim ZER is fully
> data-race-free. The known cross-thread holes are closed and gated (`tests/test_conc_matrix.c`,
> `tests/test_hw_matrix.c`); the open ones — and exactly what is and is not PROVEN — are in
> `docs/limitations.md` (section B / B2). Floors: `cinclude` code; liveness. Design + Rust
> mapping: `docs/primitives-data-races.md` §24. Proofs: `proofs/operational/lambda_zer_concurrency/`.
> History of the audit: playbook "CONCURRENCY MEMORY-SAFETY STATUS".

### Move Struct — Ownership Transfer
```
// move struct: passing or assigning transfers ownership, original invalid
move struct FileHandle { i32 fd; }
FileHandle f;
f.fd = 42;
consume(f);          // ownership transferred
f.fd;                // COMPILE ERROR — use after move

// Assignment also transfers:
move struct Token { u32 kind; }
Token a;
a.kind = 1;
Token b = a;         // ownership transfers to b
a.kind;              // COMPILE ERROR — use after move
```

ZER is copy-by-default (opposite of Rust). `move struct` opts IN to ownership tracking.
Only for types representing unique resources (file descriptors, hardware handles, one-shot tokens).
Uses zercheck's existing HS_TRANSFERRED state — zero new infrastructure.

### Hardware Support
```
volatile *u32 reg = @inttoptr(*u32, 0x4002_0014);  // MMIO register
u32 bits = reg[9..8];                               // bit extraction
interrupt USART1 { handle_rx(); }                    // interrupt handler
packed struct Packet { u8 id; u16 val; u8 crc; }    // unaligned struct
```

### What ZER Does NOT Have
- No classes, inheritance, templates, generics (use `container` keyword for type-stamped containers)
- No exceptions, try/catch
- No garbage collector
- Heap allocation IS `alloc(T)` / `alloc(T,n)` / `free(x)` — a typed, zercheck-tracked malloc-equivalent (see docs/universal_alloc.md). What ZER does NOT have: a RAW untyped `void*` malloc. Pool/Slab/Ring/Arena/Handle remain as explicit-control allocators.
- No implicit narrowing or sign conversion
- No undefined behavior (overflow wraps, shift by >=width = 0)
- No `++`/`--`, no comma operator
- No C-style cast between two DIFFERENT pointer types (`@pun` is the audit-visible route); value casts `(T)x` ARE supported and explicit
- No preprocessor (#define → `comptime` functions, #ifdef → `comptime if`)
- No header files (use `import`)
- No preprocessor (#define, #ifdef)

### Feature Judgement — What NOT to Add

When considering new features, apply the **primitives test**: if the use case can be solved with existing language primitives, it's a library concern, NOT a language feature. ZER's concurrency is complete at the language level:

- **Park/unpark** — DON'T ADD. Condvar already covers thread wake/sleep. Two mechanisms for the same thing = complexity.
- **Future/Pin** — DON'T ADD. ZER async compiles to concrete state machines (Duff's device). No trait abstraction needed — each async function IS its own type. Pin exists in Rust only because borrow checker can't handle self-referential structs.
- **Arc (ref counting)** — DON'T ADD. Pool/Slab/Handle covers allocation. Ref counting adds runtime overhead and cycle leak risk.
- **Async runtime** — DON'T ADD as language feature. User writes the poll loop (bare-metal) or wraps with epoll (Linux). Library concern, not language.
- **Unbounded channel** — DON'T ADD. `Ring(T, N)` is bounded by design — prevents OOM on embedded. Unbounded is a library-level choice.
- **Generics** — DON'T ADD. `container` keyword covers user-defined containers via monomorphization (type stamping). No type constraints, no SFINAE, no template metaprogramming — just text substitution + type checking on the result.
- **Whole-program analysis** — BANNED from architecture. zercheck is per-file with summaries. Heuristics cover callbacks. See design decisions.
- **Partial moves** — DON'T ADD. Copy field value before consuming whole struct. Adds 200 lines of per-field tracking for a one-line workaround.

### Safety guarantees (mechanism per class — per-bug notes: playbook "Safety Guarantees")
| Class | Prevention |
|---|---|
| Buffer overflow | bounds check on every index; a PROVEN index elides it (range propagation); an unproven one gets an auto-guard; a counted loop that provably runs past a fixed array is a compile error |
| Use-after-free, double free, leak | handle generation counter + zercheck_ir (ALIVE / FREED / MAYBE_FREED, alloc-id groups, cross-function summaries, interior pointers share the parent's id). Leaks are compile errors |
| Null dereference | `*T` is non-null, `?T` must be unwrapped; every LOAD of a `*T` field / element is null-checked (auto-zero could have forged it); an indirect call through a funcptr is guarded |
| Uninitialized memory | everything is auto-zeroed |
| Integer overflow, shifts | wraps (defined); a shift ≥ width is 0 |
| Float → int out of range | saturates; NaN → 0 |
| Silent truncation / sign change | must be explicit (`@truncate`, `@saturate`, a cast) |
| Division by zero | the divisor must be proven nonzero |
| Dangling pointer | escape analysis at every sink (return, global store, keep, struct field, spawn arg, orelse fallback) + block lifetimes; arena pointers cannot reach a global |
| Enum forging | a runtime variant guard at exactly three doors (`@bitcast`, `@truncate`, `@saturate`); `@inttoptr` / `@pun` into an enum-carrying target are rejected |
| Wrong pointer cast | provenance tracking at compile time; `_zer_opaque` type id at run time; `@container` field validation |
| MMIO | `mmio` ranges (mandatory for `@inttoptr`), alignment, index bounds from the range; a plain `*T` is not indexable |
| Qualifier strip | `volatile` / `const` are preserved through every cast and view |
| Data races | `shared struct` auto-lock; spawn-arg gates; the spawn-target body scan; the scoped-spawn borrow window; the atomic-cell rule; ISR / main sharing rules (missing `volatile`, non-atomic RMW) |
| Lock-order deadlock | two shared types in one statement is an error; locking is per statement |
| Ownership | `move struct`: use after transfer is an error; a `ThreadHandle` must be joined |
| Context | `return` / `break` / `continue` / `goto` in `@critical` or `defer`; alloc in an ISR; spawn in `@critical` / ISR; yield in `defer` / `@critical`; a shared access in a statement that suspends — all errors |
| Stack | recursion warning; `--stack-limit N` makes frame / chain size an error |

### Pitfalls when WRITING ZER (stdlib lessons)
- `orelse return` is bare (no value); for a `bool` function restructure with an if-unwrap.
- A string literal is read-only: bind it to `const [*]u8`. `.ptr` of a const slice is
  `const *u8` — cast to `const *opaque`.
- `cinclude` only emits the `#include`: declare every C function you call as a ZER signature.
  C MACROS (`stderr`) are not visible — wrap them in a C helper.
- The `_zer_` prefix is reserved for the compiler.
- Returning a sub-slice / `&elem` of a slice or pointer PARAM is allowed; of a LOCAL is not.
- `pool.get(h)` is non-storable (use it inline, or `h.field`); a `const Handle` is a const
  KEY — the data stays writable. After freeing a pointer held in a global, reset the global.

## Project map

Pipeline: `lexer.c` → `parser.c` → `ast.c` → `checker.c` (types, escape / keep, VRP,
concurrency, ISR) → `ir_lower.c` → `zercheck_ir.c` (handles, moves, threads on the CFG) →
`emitter.c` → GCC. Also `types.c`, `ir.c` (IR + `ir_validate`), `vrp_ir.c`, `src/safety/*.c`
(pure predicates: linked into zerc AND VST-verified from the same file), `zerc_main.c`
(driver), `zer_lsp.c`, `tools/zer-convert.c` / `zer-upgrade.c` (C → ZER).
- **`zercheck_ir.c` is the sole safety analyzer; `zercheck.c` is a shim — never add safety
  code there.** `IR_POOL_ALLOC` and friends are never emitted by design (method detection is
  `ir_classify_method_call_ex`).
- **Emit-C via GCC is permanent.** Never suggest LLVM, QBE or a native backend. The compiler
  is developed in C; self-hosting (`zerc.zer` compiles itself identically) is the v1.0
  proof, not the development path.
- Function bodies are IR-only. Before touching `ir_lower.c`, `ir.c` or an `IR_*` handler in
  `emitter.c`, read compiler-internals.md "IR Path Validation" (the invariants each came
  from a miscompile).
- Flags: `--run`, `--emit-c`, `--emit-ir`, `--lib`, `--no-strict-mmio`, `--target-bits N`,
  `--gcc PATH`, `--stack-limit N`, `--target-arch=`, `--target-features=`. Emitted C needs
  `-fwrapv` and GCC extensions. User type names emit as-is; internal names are `_zer_`.
- Navigate with `make tags` + grep; never read a source file end to end.

## Building & testing

`make zerc` · `make check` (everything) · `make docker-check` (the Windows host) ·
`make check-proofs` (Coq / Iris + the admit gate) · `make check-vst` (VST on `src/safety/*.c`).

| Directory | Expectation | Runner |
|---|---|---|
| `tests/zer/` | compiles, runs, exits 0 | `tests/test_zer.sh` |
| `tests/zer_fail/` | fails to compile FOR THE STATED REASON | same |
| `tests/zer_trap/` | compiles clean, traps at run time (`// expect-trap`, `// expect-trap-at: N`) | same |
| `tests/zer_gaps/` | known gaps, expectation INVERTED (compiling IS the gap; a closed gap fails loudly; a masked one needs `// gap-masked-by:`) | same |
| `test_modules/` | multi-file (`_negative` suffix for negatives) | `test_modules/run_tests.sh` |
| `rust_tests/`, `zig_tests/` | translations ONLY; contain negatives (`reject_*`, or `// EXPECTED: compile error`) | their own |
| `test_*.c`, `tests/test_*_matrix.c` | C unit tests; axis-crossed grids | `make check` |

Count tests from the tree (`ls tests/zer/*.zer | wc -l`), never from a document.

### Test rules
- **Every negative states WHY: `// expect-error: <substring>` in the first 5 lines** — the
  reason the rule is SUPPOSED to give, never a paste of today's output. Many independent
  rules reject the same file, so a non-zero exit proves nothing; this gets WORSE as the
  compiler gets better.
- A test that needs compiler flags says so on its first line: `// zerc-flags: …`.
- Every test directory is executed by a runner; wire a new one in the same commit. An
  unexecuted test decays to zero value, then to negative value once it is believed.
- Every spec rule has a positive AND a negative test; every safety feature has a
  `tests/zer_fail/` file through the full `zerc` pipeline (unit tests alone once hid that
  zercheck was never called from the compiler).
- Bug found → write the reproducer BEFORE the fix and show it FAILS on a pre-fix build.
  Keep a baseline compiler for the whole session
  (`git archive HEAD | tar -x -C /tmp/headb && make -C /tmp/headb zerc`) and A/B every
  "X was broken, my change fixes it" against it. Check WHICH binary each row ran, and grep
  for the diagnostic that is actually printed (`compiler bug:` is not `error:`).
- Use shapes the analysis cannot trivially prove: a FIXED ARRAY for VRP (a slice keeps its
  runtime check and masks the bug); values laundered through a loop for runtime wrappers.
- Hosted x86-64 limits: an `interrupt` block cannot be in a runnable positive (GCC refuses
  it) — verify compile-only; an MMIO positive must never touch the address (use an
  out-of-range index so the guard returns).
- New feature — test the interactions: both `orelse return` and `orelse { return; }`,
  `defer`, loops, `switch`, const / volatile, `*opaque`, if-unwrap capture, `@critical`,
  ISR, BOTH the assign and the var-decl spelling, all value-flow sites (init, assign, call
  arg, return). Then write one REAL 10-60 line program: real programs found bugs that
  1,700 unit tests missed.
- A new diagnostic that fires on a corpus file which is not your own new negative is either
  a real bug or a test that was passing for the wrong reason — check which, every time.
  Measure the corpus cost of any tightening by COMPILING the corpus with the rule and
  grepping for your diagnostic; a source grep sees only syntax, and a truncated list is not
  evidence.

## How to work here

**Before code.** Read the function, its callers and its data flow; decide the architecture;
then write. Grep `BUGS-FIXED.md` and `docs/limitations.md` for the area. A documented OPEN
hole is a HYPOTHESIS: reproduce it on main first (most stale entries were already closed or
masked by a stronger rule), read the REASON not the exit code, and fetch a branch's
reproducer verbatim (`git show <branch>:<path>`) — a reconstructed probe can confirm a
hole, never refute one. A doc's motivation text goes stale when a neighbouring change lands.

**The no-debt rule — every change is the proper fix, not a shortcut.**
1. No duplicate logic: extract the helper FIRST.
2. No shared mutable AST nodes: a desugaring clones.
3. An ordering dependency is commented at both sites.
4. No copy-on-call for state the caller must see mutated.
5. Test the real pattern, not the simple case.
6. 100%, not 99%: handle the edge or reject it at compile time — never silently wrong.
7. **Rule #7 — never a fixed-size buffer for dynamic data**: stack-first + arena / realloc
   overflow (`audit_fixed_buffers.sh`). A table that silently drops is a hole: set an
   overflow flag and answer conservatively.
8. Scope-sensitive state is written at DECLARATION sites (the current scope:
   `scope_lookup_local`) and only read at use sites (any scope: `scope_lookup`). Scope
   logic added at a use site breaks every loop / branch that reads an outer variable.

**Proper vs pragmatic.** A fix belongs at the root (declaration, data structure, shared
helper), not where the bug shows. The same fix at 4+ sites, a patch that fixes A and breaks
B, or three failed attempts → stop: you need a new abstraction. **Track, don't ban** — ban
only for a hardware / OS constraint, an emission impossibility, or something needing a
runtime or type system ZER lacks (cross-check Zig and Rust). An over-rejection is acceptable
only when the rewrite it forces is a teachable discipline. Features: if existing primitives
solve it, it is a library concern (see "Feature Judgement").

**A bounded walk rounds toward REJECT.** A depth cap or a full table must give the
conservative answer — which depends on the CALLER's polarity: flip a boolean; make a silent
walk REPORT; widen a recorder that has no conservative value; go tri-state when two callers
read the answer oppositely; return a sentinel (not a made-up name) when the diagnostic needs
a pointer. A generous cap AND a conservative answer, both. A cap reset on a hop is not a
cap; a cap over AST nesting is as reachable as one over a call graph; a walk over TYPES
needs no cap (the type graph is acyclic). After adopting foreign code re-run
`grep -nE "depth *> *[0-9]+\)" checker.c zercheck_ir.c`.

**A relaxation (reject → accept) is the one change class where a bug ships a
use-after-free or a race.** Diagnose the layer first (usually a coarse domain at a join);
the soundness gate is a complete no-`default:` AST walk, never an IR-field scan; prefer
overriding the DECISION over refining fixpoint state; land the tracking with no behaviour
change (green `make check`) before flipping it; and write the rule's hypotheses down as a
model first — for a "fact F holds between A and B" rule ask which edges LEAVE the region
and which edges ENTER the code after B (the BUG-1651 Coq model found three races the prose
argument had not). Compiler-internals.md "Sound relaxation".

**Editing discipline.** Verify that an edit LANDED and RUNS: a script that asserts midway
writes nothing; a pattern insert can land in the wrong function; a hook can compile and
never fire — one trace line proving the predicate is REACHED settles it. Back up before a
scripted deletion and delete by explicit count.

**Debugging.** Commit or stash first. Minimal repro → expected vs actual in ONE sentence →
ONE targeted print at the decision point → confirm the root cause → fix (1-5 lines; if it
grows you are fixing the wrong thing) → `make check`. A compiler segfault: rebuild with
`-fsanitize=address -g -O0` and read the trace. Three cycles without a confirmed cause, a
fix past ~10 lines, spreading prints or platform ifdefs → revert and restate. When a
function returns NULL, find WHICH path returned it. Imported globals are registered under
both the raw and the mangled name — a "unique match" scan must compare `->type`. Field
access has two paths (value, and pointer auto-deref): check both.

**Checklists.** New builtin method → checker + emitter (both paths) + zercheck_ir. New AST
node → lexer, ast, parser, checker, ir_lower / emitter, and every exhaustive walker
(`-Werror=switch` lists them). New coercion → every value-flow site, through
`value_flows_to`. New emission path → every safety wrapper ported (see the audit section
below). New safety rule → which of the 4 models, its gate cell, a negative with
`expect-error`, and its `docs/multi-site-classes.md` row if N sites answer it.

**Docs go in the SAME commit, after `make check` is green.**
- a fix → `BUGS-FIXED.md` (symptom, root cause, fix, test);
- an open gap, deferred fix, over-rejection or residual → an entry in `docs/limitations.md`
  — a gap recorded only in a commit message or a code comment is lost. That file holds OPEN
  entries only (`audit_limitations.sh` fails the build otherwise): when a fix closes one,
  MOVE its text to the end of `docs/limitations-closed.md`, and write whatever the fix left
  open as its own entry tagged `[residual of: … — closed by BUG-n]`. Never strike an entry
  through and leave it. An entry in `docs/limitations-legacy.md` is a hypothesis: measure
  it, then promote it or archive it — that file only shrinks;
- durable architecture → `docs/compiler-internals.md`; a language change → `docs/reference.md`;
- a class or gate → its row in `docs/multi-site-classes.md` + ONE index line here;
- a process lesson → `docs/session-playbook.md` + at most two lines here.

**Harvesting a `claude/*` audit branch.** The fork point is the triage (a branch that forked
long ago overlaps what landed since, and its BUG numbers collide); cherry-pick, don't
re-derive; a CLEAN APPLY is not evidence of no conflict — grep for the rule you already
have; run the branch's tests against YOUR main; branch commits carry ELF binaries. Pick
"their fix or mine" by measurement. Protocol: compiler-internals.md "Harvesting an audit
branch".

**Agents** are optional. One that writes ZER must be given the syntax rule block (playbook
"Spawning Agents That Write ZER Code"); one that compiles must follow the Windows-host
rule; verify every finding yourself before acting on it.

## Pushback discipline — capitulation is not helpfulness

When the user disagrees, re-derive the answer from the substrate (code, docs, a
measurement), independently of which way they lean.
1. **User right** → change position AND name the specific technical error in your earlier
   reasoning. "You're right, I overstated" is not a reason.
2. **User wrong** → hold the position and show the specific thing they are missing. Their
   confidence is not evidence.
3. **A genuine judgment call** → say so and lay out both sides, without picking the side
   they leaned to.
4. Never open with "you're right" before re-checking. A flip within one message of pushback
   is drift, not reasoning.

The test: could you defend the new position against equal pushback from the opposite
direction? If not, it was capitulation. The inverse holds too — do not re-litigate settled
questions (see "Confirmed NOT bugs"). Full text: playbook, same title.

## Proof work — read `docs/proof-internals.md` first

- **Three levels — keep them apart in every claim.** L1: a Coq / Iris oracle (the abstract
  rule is sound). L2: tests (the compiler rejects known violations). L3: VST on
  `src/safety/*.c` (the C that is linked into zerc matches the spec for every input).
- **THE ONE RULE: write specs against the L1 ORACLE, never against current C.** A
  code-driven spec is a tautology that freezes bugs into CI. No oracle → write it first.
- **The max-oracle standard.** Every distinguishable state AND every operational
  PRECONDITION the decision relies on is a variable of the model — a soundness hole found by
  red team rather than by a failing proof means the model lacked that variable (Level B's
  guard stability; BUG-1651's edges leaving the window). Merge = JOIN with the unsafe class
  as the default. Per oracle: T1 soundness, T3 a precision witness, T4 no-under-rejection.
  Zero admits, listed in `_CoqProject` AND in the `make check-proofs` admit gate. Name the
  honest LIMIT.
- 100% precision is impossible (Rice). Pay down only the PAYABLE over-rejection (relational
  domains, reduced product), each increment proven; never chase the forced tail.
- VST-friendly C: a flat cascade of early-return ifs, no nesting, no compound conditions.
  `src/safety/*.v` is clightgen output — never hand-edit it.
- **Say what is NOT proven.** A proof file that was written but never compiled is not a
  proof (`docs/limitations.md` B2 lists any such file).

## Safety architecture — 4 models (tables: `docs/safety-model.md`)

A new safety feature belongs to one of:
1. **State machine** — an entity with a lifecycle (handle states, moves, thread handles).
2. **Program-point property** — a fact about a value that changes at assignments and
   branches (ranges, provenance, escape flags, context flags).
3. **Function summary** — a per-function fact read at call sites (FuncProps, free / store
   summaries, return range, stack frames, spawn and shared-type scans).
4. **Static annotation** — declared once, checked at uses (qualifiers, MMIO ranges, keep).

Fits none → it is a combination of these; otherwise stop and document why.

## Diff-Based Post-Release Audit / AST→IR Emission Diff Audit

After a large structural change, green tests are not sufficient (protocols:
compiler-internals.md, same titles). The standing rule: an IR handler that replaces
`emit_expr(...)` with direct emission silently STRIPS the safety wrappers `emit_expr`
applied, and VRP hides the loss from tests. Before committing an emitter refactor:

    grep -nE "_zer_trap|_zer_bounds_check|_zer_shl|_zer_shr|_zer_probe|emit_intn_mask" emitter.c

Every AST-region match needs an IR-path equivalent. Audit BOTH operator forms (binary and
compound assignment are different cases). A region-presence gate does NOT discriminate;
behavioural coverage across emission forms does (`tests/zer/uN_width_wrap_all_forms.zer`).

## Intentional — not bugs

- MAYBE_FREED conservatism: a handle freed on one branch is unusable after the join unless
  the guards are provably disjoint. Add `return;` after the free, or restructure.
- No pointer arithmetic; `[*]T → *T` coercion only for C externs; a plain `*T` is not
  indexable; `@inttoptr(*T, 0)` is allowed (MMIO at address 0).
- `*opaque` from C has type id 0 (unknown) and passes the cast check: C is outside the boundary.
- Handles passed to a funcptr call are consume-maybe; a global left MAYBE_FREED at exit is
  deliberately not flagged.
- Pool (fixed, ISR-safe), Slab (growable, not ISR-safe) and Arena (bump, bulk reset) are
  three things on purpose. `Task.alloc()` uses one implicit auto-slab per struct type.
- Emitted struct initialisers are always DESIGNATED, never positional.

## Confirmed NOT Bugs — DO NOT Re-Investigate

These were thoroughly investigated during the 2026-04-14 full codebase audit (25,757 lines) and confirmed correct:

1. **`types.c can_implicit_coerce` missing distinct unwrap** — BY DESIGN. Distinct types intentionally block implicit conversion. Only T→?T (line 367) unwraps.
2. **`type_equals` nominal for distinct** — BY DESIGN. Pointer identity = same definition.
3. **Const/volatile laundering checks without unwrap** (checker.c lines 3179, 3190, 3888, 3896, 6278, 6286, 7577, 7588) — Belt-and-suspenders. `can_implicit_coerce` catches at type level. Missing unwrap gives generic error instead of specific message. NOT safety holes.
4. **`pathstate_equal` asymmetric check** (zercheck.c:62) — Correct for loop convergence. New handles from loop body are expected.
5. **`import_asts[64]`** (zerc_main.c:460) — Graceful degradation, not crash.
6. **Emitter `asname[128]`** (emitter.c:1746) — Used with `%.*s` which stops at null. Safe.
7. **`@offset` struct type check** (checker.c:5254) — Cosmetic only, not safety.
8. **`scan_unsafe_global_access` static depth counter** (checker.c:6109) — Safe, LSP is single-threaded.


## Git Rules

- NEVER add Co-Authored-By or any Claude/AI attribution to commits
- All commits must be under zerohexer@gmail.com only
- No other authors, no co-authors, no AI mentions in commit messages
