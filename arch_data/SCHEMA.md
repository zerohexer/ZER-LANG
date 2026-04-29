# `.zerdata` Schema for Per-Instruction Safety Metadata

**Purpose:** Bridge between vendor specs (Intel SDM / ARM ARM / RISC-V spec) and
ZER's safety classification (8 categories C1-C8). Each entry maps an instruction
mnemonic to its safety category bitmap, operand constraints, required CPU
features, and vendor citation.

`.zerdata` files are **vendored at zerc build time** — parsed by
`scripts/gen_instruction_table.sh` which emits a vendored `.c` file linked into
the compiler binary. Not user-extensible at runtime (per architecture decision
2026-04-29).

## File naming

```
arch_data/
├── x86_64.zerdata          # F4 — Intel/AMD x86_64 instruction set
├── aarch64.zerdata         # F5 — ARM64 instruction set
└── riscv64.zerdata         # F6 — RISC-V (RV64GC) instruction set
```

One `.zerdata` per architecture. Sub-extensions can either be in the main file
(with `required_features` field) or in a separate `<arch>_<feature>.zerdata`
file if the data is large enough to warrant it.

## Syntax

INI-style sections, one per instruction. Lines starting with `#` are comments;
blank lines are ignored.

```
[MNEMONIC]
field_name = value
field_name = value
...
```

The section header `[MNEMONIC]` is the instruction name as it appears in asm
syntax (case-insensitive matching at lookup time, but conventionally lowercase
in the file).

## Required fields

Every entry MUST have:

| Field | Type | Description |
|---|---|---|
| `category` | bitmap expression | Safety category (C1-C8). Combined with `+` for multi-category. |
| `source` | quoted string | Vendor spec citation (e.g., `"Intel SDM Vol 2A BSR"`) |

## Optional fields (defaults documented)

| Field | Default | Description |
|---|---|---|
| `operand_count` | 0 | Number of operands. If >0, operand[N] entries should follow. |
| `operand[N].type` | UNKNOWN | Operand type (REGISTER, MEMORY, IMMEDIATE, etc.) |
| `operand[N].constraint` | ANY | Constraint on operand value (NONZERO, ALIGNED(N), BOUNDED(min,max), etc.) |
| `required_features` | NONE | Bitmap of required CPU features (e.g., AVX512F, AMX_TILE). Combined with `+`. |
| `consequence` | "" | One-line description of what goes wrong if the constraint is violated. Used in error messages. |
| `notes` | "" | Free-form notes for the human reader. Not parsed. |

## Category values (bitmap)

```
C1_VALUE_RANGE     # operand range (BSR on zero, shift count, division by zero, etc.)
C2_ALIGNMENT       # memory operand alignment (MOVAPS requires 16-byte align)
C3_STATE_MACHINE   # LL/SC pairing, MONITOR/MWAIT, etc.
C4_CPU_FEATURE     # requires specific CPU feature flag (AVX-512, AMX, etc.)
C5_PRIVILEGE       # kernel-only instruction (WRMSR, INVLPG, HLT)
C6_MEMORY_ADDR     # canonical address, segment, MMIO range
C7_PROVENANCE      # type-erased aliasing through asm boundary
C8_MEMORY_ORDER    # barrier/atomic ordering (System #30, Stage 5)
```

Multiple categories combined with `+`:
```
category = C2_ALIGNMENT + C5_PRIVILEGE
```

## Operand types

```
REGISTER                # any register valid for this arch
GP_REGISTER             # general-purpose register
XMM_REGISTER            # x86 SIMD 128-bit
YMM_REGISTER            # x86 SIMD 256-bit (requires AVX)
ZMM_REGISTER            # x86 SIMD 512-bit (requires AVX-512)
TILE_REGISTER           # x86 AMX tile (requires AMX_TILE)
SVE_VECTOR_REGISTER     # ARM SVE z0-z31
SVE_PREDICATE_REGISTER  # ARM SVE p0-p15
MEMORY                  # memory operand (any addressing mode)
MEMORY_ALIGNED          # memory operand requiring alignment (constraint follows)
IMMEDIATE               # immediate value (constant)
IMMEDIATE_BOUNDED       # immediate with explicit bounds
TILE_INDEX              # x86 AMX tile index (0-7)
ANY                     # unknown / not yet classified
```

## Constraint expressions

```
ANY                          # no constraint (default)
NONZERO                      # operand must be provably non-zero (VRP check)
ALIGNED(N)                   # memory operand must be N-byte aligned (N is power of 2)
BOUNDED(min, max)            # value must be in [min, max] inclusive
COMPOUND(c1, c2, ...)        # all constraints must hold (logical AND)
NOT_OVERFLOW_MIN_DIV_NEG_ONE # IDIV-specific: operand can't be MIN/-1 (overflow)
```

Constraints map to existing ZER safety systems at check time:
- `NONZERO`, `BOUNDED` → VRP (System #12)
- `ALIGNED` → Qualifier tracking (System #20)
- (others to be added as needed)

## Required features

```
NONE                  # no special feature required (default)
AVX                   # x86 AVX
AVX2                  # x86 AVX2
AVX512F               # x86 AVX-512 Foundation
AVX512VL              # x86 AVX-512 Vector Length
AVX512BW              # x86 AVX-512 Byte/Word
AMX_TILE              # x86 AMX (Advanced Matrix Extensions)
SSE                   # x86 SSE
SSE2                  # x86 SSE2
SHA                   # x86 SHA-NI
AES                   # x86 AES-NI
APX                   # x86 APX (Advanced Performance Extensions, GCC 14+)
NEON                  # ARM NEON
SVE                   # ARM SVE
SVE2                  # ARM SVE2
SME                   # ARM SME
RVV                   # RISC-V Vector extension
```

Combined with `+`:
```
required_features = AVX512F + AVX512VL
```

## Worked examples

### Example 1: BSR (bit scan reverse)

```
[bsr]
operand_count = 2
operand[0].type = GP_REGISTER
operand[1].type = GP_REGISTER
operand[1].constraint = NONZERO
category = C1_VALUE_RANGE
source = "Intel SDM Vol 2A BSR"
consequence = "silent UB: destination undefined when source is zero"
notes = "BSF has identical safety contract"
```

### Example 2: MOVAPS (move aligned packed single)

```
[movaps]
operand_count = 2
operand[0].type = XMM_REGISTER
operand[1].type = MEMORY_ALIGNED
operand[1].constraint = ALIGNED(16)
category = C2_ALIGNMENT
required_features = SSE
source = "Intel SDM Vol 2B MOVAPS"
consequence = "#GP fault on misaligned memory operand"
```

### Example 3: WRMSR (write model-specific register, privileged)

```
[wrmsr]
operand_count = 0
category = C5_PRIVILEGE
source = "Intel SDM Vol 2B WRMSR"
consequence = "#GP fault if executed at CPL > 0"
notes = "Implicit operands: ECX (MSR index), EDX:EAX (value)"
```

### Example 4: VPXORQ (multi-category)

```
[vpxorq]
operand_count = 3
operand[0].type = ZMM_REGISTER
operand[1].type = ZMM_REGISTER
operand[2].type = ZMM_REGISTER
category = C4_CPU_FEATURE
required_features = AVX512F
source = "Intel SDM Vol 2C VPXORQ"
consequence = "#UD fault if AVX-512F not enabled"
```

### Example 5: MONITOR (multi-category, both C3 and C5)

```
[monitor]
operand_count = 0
category = C3_STATE_MACHINE + C5_PRIVILEGE
source = "Intel SDM Vol 2B MONITOR"
consequence = "#GP fault if not in ring 0; subsequent MWAIT pairing required"
notes = "Implicit operands: EAX (linear address), ECX (ext info), EDX (hints). Must be paired with MWAIT (C3 state machine)."
```

## Schema version

```
# At top of each .zerdata file (parser checks):
schema_version = 1
```

Bumping the schema version requires updating the generator script. Future
versions add fields (backward-compatible) or restructure (breaking).

## Validation rules (enforced by generator)

1. **Required fields present**: `category` and `source` mandatory; missing → error
2. **Category names valid**: only C1-C8 allowed in bitmap expressions
3. **Constraint type matches operand type**: `ALIGNED(N)` only on memory operands; `NONZERO` only on integer-typed operands
4. **Source non-empty**: forces vendor lookup, prevents copy-paste mistakes
5. **Mnemonic uniqueness**: no duplicate `[mnemonic]` sections per arch
6. **Feature names valid**: must be in the enumerated list above

## Future extensions

Schema version 2 (when needed):
- `latency` field (cycle counts for performance hints)
- `port_usage` (Intel uOp port info)
- `flags_modified` (RFLAGS bits affected)
- `aliases` (e.g., MOVD/MOVQ same instruction different operand widths)

Not needed for v1.0 safety enforcement; deferred until concrete need surfaces.

## Lookup at runtime

```c
/* Generated lookup: */
uint32_t cats = zer_asm_category(ZER_ARCH_X86_64, "bsr", 3);
/* Returns ZER_CAT_C1_VALUE_RANGE bitmap. */

/* Checker dispatches based on bitmap: */
if (cats & ZER_CAT_C1_VALUE_RANGE) {
    /* call VRP system to verify operand constraints */
}
if (cats & ZER_CAT_C5_PRIVILEGE) {
    /* check naked context + critical/kernel flag */
}
/* etc. for each category bit set */
```

This is the `F7-full` work — wiring categories to existing safety systems.
F4 produces the data; F7 acts on it.
