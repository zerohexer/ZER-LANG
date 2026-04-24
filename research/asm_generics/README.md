# Asm Generics — Research Artifacts

**Purpose:** This directory contains proof-of-concept `.zer` files that specify EXPECTED behavior of ZER's asm safety checker when strict mode (D-Alpha-7.5 Phase 2) lands. These are research artifacts, NOT regression tests.

**Why NOT in `tests/`:** These files exercise `unsafe asm` structured syntax (typed operand bindings, clobber lists, safety documentation) that isn't implemented yet. Running them through the current test suite would either:
- Fail for parse-error reasons (not the intended safety category reason)
- Pass the "must fail to compile" check vacuously (wrong reason)

Either outcome would falsely suggest the checker works when it doesn't exist yet.

**What these files ARE:** executable specifications demonstrating what each Category check should catch when implemented. They serve as:
1. Design input for the checker implementation
2. Regression tests AFTER strict mode lands (can be moved to `tests/zer_fail/` and `tests/zer/` then)
3. Research artifacts proving the category framework catches real UB patterns

## Structure

```
research/asm_generics/
├── README.md                          # This file
├── C1_value_range/                    # Category C1 — value-range preconditions
│   ├── reject/                         # Expected: checker rejects
│   │   ├── x86_bsr_unguarded.zer       # BSR without nonzero guard
│   │   ├── x86_div_unguarded.zer       # DIV without nonzero divisor
│   │   └── x86_idiv_overflow.zer       # IDIV compound (nonzero + non-overflow)
│   └── accept/                         # Expected: checker accepts (VRP proves guard)
│       └── x86_bsr_guarded.zer         # BSR inside `if (x != 0)` guard
├── [C2_alignment/]                    # Planned — next research session
├── [C3_state_machine/]                # Planned
├── [C4_cpu_feature/]                  # Planned
├── [C5_privilege/]                    # Planned
├── [C6_memory_addressability/]        # Planned
├── [C7_provenance_aliasing/]          # Planned
├── [C8_memory_ordering/]              # Planned (biggest — adds new System #30)
├── [C9_exclusive_pairing/]            # Planned (may merge into C3)
└── [C10_register_dependency/]         # Planned (may defer entirely)
```

## Naming convention

**Per category:** `C<N>_<kebab_name>/` (e.g., `C1_value_range/`)

**Per file:** `<arch>_<instruction_scenario>.zer`
- `arch`: x86, arm64, riscv
- `instruction_scenario`: what the file demonstrates (e.g., `bsr_unguarded`, `div_compound_overflow`)

**Folder per outcome:**
- `reject/` — files that the checker SHOULD reject at compile time
- `accept/` — files that the checker SHOULD accept (demonstrates valid guard/range proof)

## Relationship to other docs

- **Main research doc:** `docs/asm_preconditions_research.md` — contains ISA citations, category framework, methodology, verified findings per category
- **Plan doc:** `docs/asm_plan.md` — overall ZER-Asm plan, Option C commitment
- **Implementation target:** D-Alpha-7.5 Phase 2 in `docs/asm_plan.md` — when strict mode lands, these POC files become real regression tests

## Migration path (when strict mode lands)

Once D-Alpha-7.5 Phase 2 lands and `unsafe asm` structured syntax is parsed + checked:

1. Verify each file in `research/asm_generics/<category>/reject/` causes the expected compile error (check error message matches the documented "Expected error" in file comment)
2. Verify each file in `research/asm_generics/<category>/accept/` compiles cleanly
3. Move `reject/*.zer` → `tests/zer_fail/` and `accept/*.zer` → `tests/zer/`
4. Keep symlinks or leave files here as research history

Until that migration, these files are NOT in the active test suite.

## Current status

| Category | Status | Session | Deliverables |
|---|---|---|---|
| C1 Value-range | COMPLETE (2026-04-24) | 1 | 4 .zer files + ISA citations + 7 x86 instructions classified |
| C2 Alignment | Pending | 2 | — |
| C3 State machine | Pending | 3 | — |
| C4 CPU feature | Pending | 4 | — |
| C5 Privilege | Pending | 5 | — |
| C6 Memory addressability | Pending | 6 | — |
| C7 Provenance/aliasing | Pending | 7 | — |
| C8 Memory ordering | Pending (needs System #30 design) | 8 | — |
| C9 Exclusive pairing | Pending (may merge into C3) | — | — |
| C10 Register dependency | Pending (may defer entirely) | — | — |

Session methodology: `docs/asm_preconditions_research.md` "Session Methodology" section.
