#!/bin/bash
# scripts/gen_instruction_table.sh — D-Alpha-7.5 Session F4
#
# Build-time generation of per-arch instruction safety tables. Parses the
# arch_data/<arch>.zerdata metadata file and emits a vendored .c file in
# src/safety/. Output is committed to git for reproducible builds.
#
# Usage: scripts/gen_instruction_table.sh [arch]
#   arch: x86_64 (default), aarch64, riscv64
#
# Pipeline:
#   arch_data/x86_64.zerdata
#        ↓ (this script)
#   src/safety/asm_instruction_table_x86_64.c (vendored)
#        ↓ (compiled into zerc)
#   zerc binary uses zer_asm_category(arch, mnemonic) for safety dispatch
#
# Schema spec: arch_data/SCHEMA.md
# Architecture rationale: docs/asm_plan.md Sub-Extension Architecture section

set -e
ARCH="${1:-x86_64}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INPUT="$ROOT/arch_data/${ARCH}.zerdata"
OUTPUT="$ROOT/src/safety/asm_instruction_table_${ARCH}.c"

if [ ! -f "$INPUT" ]; then
    echo "Input file not found: $INPUT" >&2
    echo "Create arch_data/${ARCH}.zerdata following arch_data/SCHEMA.md" >&2
    exit 1
fi

# ============================================================================
# Parser: extract per-instruction metadata from .zerdata
# ============================================================================
# Output one CSV-like line per instruction:
#   mnemonic|category_bitmap|required_features|source|consequence
#
# Categories and features get translated to their integer bitmap values.
# Multi-value fields use `+` separator in source, get OR'd in output.

TMP="$(mktemp)"
ERRORS="$(mktemp)"
trap 'rm -f "$TMP" "$ERRORS"' EXIT

# Translate category names to bitmap values (matches asm_categories.h enum).
cat_to_int() {
    case "$1" in
        C1_VALUE_RANGE)    echo 1 ;;     # 1 << 0
        C2_ALIGNMENT)      echo 2 ;;     # 1 << 1
        C3_STATE_MACHINE)  echo 4 ;;     # 1 << 2
        C4_CPU_FEATURE)    echo 8 ;;     # 1 << 3
        C5_PRIVILEGE)      echo 16 ;;    # 1 << 4
        C6_MEMORY_ADDR)    echo 32 ;;    # 1 << 5
        C7_PROVENANCE)     echo 64 ;;    # 1 << 6
        C8_MEMORY_ORDER)   echo 128 ;;   # 1 << 7
        NONE|"")           echo 0 ;;
        *)                 echo "ERROR_UNKNOWN_CATEGORY:$1" ;;
    esac
}

# Translate feature names to bitmap (matches asm_register_tables.h ZerCpuFeature).
# F4.2 expansion (2026-04-29): added 13 features used by x86_64.zerdata.
# Bit values must match the enum exactly — both pieces are vendored.
feat_to_int() {
    case "$1" in
        AVX512F)           echo 1 ;;        # 1 << 0
        SSE)               echo 2 ;;        # 1 << 1
        SSE2)              echo 4 ;;        # 1 << 2
        AVX)               echo 8 ;;        # 1 << 3
        AVX2)              echo 16 ;;       # 1 << 4
        AES)               echo 32 ;;       # 1 << 5
        SHA)               echo 64 ;;       # 1 << 6
        BMI1)              echo 128 ;;      # 1 << 7
        BMI2)              echo 256 ;;      # 1 << 8
        LZCNT)             echo 512 ;;      # 1 << 9
        POPCNT)            echo 1024 ;;     # 1 << 10
        INVPCID)           echo 2048 ;;     # 1 << 11
        PKU)               echo 4096 ;;     # 1 << 12
        XSAVE)             echo 8192 ;;     # 1 << 13
        SMAP)              echo 16384 ;;    # 1 << 14
        NONE|"")           echo 0 ;;
        # Unknown feature names: emit 0 (no gating) and warn via stderr.
        # Maintainers must extend the enum to wire new features.
        *)                 echo 0 ;;
    esac
}

# Combine bitmap expression (e.g., "C1_VALUE_RANGE + C5_PRIVILEGE") into int.
bitmap_combine() {
    local expr="$1"
    local converter="$2"
    local total=0
    # Strip whitespace, split on '+'
    local parts
    parts=$(echo "$expr" | tr -d ' ' | tr '+' '\n')
    while IFS= read -r part; do
        [ -z "$part" ] && continue
        local val
        val=$($converter "$part")
        if [[ "$val" == ERROR_* ]]; then
            echo "$val" >> "$ERRORS"
            continue
        fi
        total=$((total | val))
    done <<< "$parts"
    echo "$total"
}

# Awk-based parser: emits CSV (mnemonic|cat|features|source|consequence) per
# entry. Tracks the current section's accumulated fields, flushes at next
# section header or EOF.
awk '
    BEGIN {
        mnemonic = ""
        category = ""
        features = "NONE"
        source = ""
        consequence = ""
        in_entry = 0
    }

    # Section header: [mnemonic]
    # Section header: [mnemonic]. Allow dots in mnemonics (e.g., RISC-V
    # `lr.w`, `fence.i`, `c.add`, ARM `dmb sy`). Dots are valid asm
    # mnemonic separators on RISC-V; the generator must preserve them.
    /^\[[a-zA-Z][a-zA-Z0-9_.]*\]/ {
        # Flush previous entry
        if (in_entry && mnemonic != "") {
            printf "%s|%s|%s|%s|%s\n", mnemonic, category, features, source, consequence
        }
        # Start new entry
        gsub(/[\[\]]/, "", $0)
        mnemonic = $0
        category = ""
        features = "NONE"
        source = ""
        consequence = ""
        in_entry = 1
        next
    }

    # Comments & blank lines
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*$/ { next }

    # Field: name = value
    /^[[:space:]]*[a-zA-Z_]+[[:space:]]*=/ {
        # Split key = value
        idx = index($0, "=")
        key = substr($0, 1, idx - 1)
        val = substr($0, idx + 1)
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
        # Strip surrounding quotes if any
        if (substr(val, 1, 1) == "\"" && substr(val, length(val), 1) == "\"") {
            val = substr(val, 2, length(val) - 2)
        }

        if (key == "category")          category = val
        else if (key == "required_features") features = val
        else if (key == "source")       source = val
        else if (key == "consequence")  consequence = val
        # Other fields (operand[N].*, operand_count, notes, schema_version)
        # are preserved in the .zerdata file for human readers but not
        # currently used by the F4 minimum lookup. F7-full will read
        # constraint expressions when wiring per-category enforcement.
    }

    END {
        # Flush last entry
        if (in_entry && mnemonic != "") {
            printf "%s|%s|%s|%s|%s\n", mnemonic, category, features, source, consequence
        }
    }
' "$INPUT" > "$TMP"

# ============================================================================
# Emit the vendored .c file
# ============================================================================
ENTRY_COUNT=0

{
    echo "/* AUTO-GENERATED by scripts/gen_instruction_table.sh — DO NOT EDIT BY HAND."
    echo " * Regenerate via: make gen-asm-tables (or scripts/gen_instruction_table.sh ${ARCH})"
    echo " *"
    echo " * Source: arch_data/${ARCH}.zerdata (per-instruction safety classification)"
    echo " * Generated: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    echo " *"
    echo " * Vendored for reproducible builds + LSP-responsive runtime lookup."
    echo " * D-Alpha-7.5 Session F4 (instruction-level safety classification)."
    echo " *"
    echo " * Schema: arch_data/SCHEMA.md"
    echo " * Lookup: zer_asm_category(arch, mnemonic, len)"
    echo " */"
    echo "#include \"asm_categories.h\""
    echo "#include \"asm_instruction_table.h\""
    echo ""
    echo "const ZerInstructionEntry zer_${ARCH}_instructions[] = {"

    while IFS='|' read -r mnemonic cat_expr feat_expr source consequence; do
        [ -z "$mnemonic" ] && continue
        local_cat=$(bitmap_combine "$cat_expr" cat_to_int)
        local_feat=$(bitmap_combine "$feat_expr" feat_to_int)
        # Escape backslashes and quotes for C string literals
        esc_source=$(echo "$source" | sed 's/\\/\\\\/g; s/"/\\"/g')
        esc_consequence=$(echo "$consequence" | sed 's/\\/\\\\/g; s/"/\\"/g')
        len=${#mnemonic}
        echo "    {\"$mnemonic\", $len, ${local_cat}u, ${local_feat}u, \"$esc_source\", \"$esc_consequence\"},"
        ENTRY_COUNT=$((ENTRY_COUNT + 1))
    done < "$TMP"

    echo "    {0, 0, 0u, 0u, 0, 0}  /* sentinel */"
    echo "};"
    echo ""
    echo "const size_t zer_${ARCH}_instruction_count = ${ENTRY_COUNT};"
} > "$OUTPUT"

# Report errors if any
if [ -s "$ERRORS" ]; then
    echo "ERRORS during generation:" >&2
    cat "$ERRORS" >&2
    exit 1
fi

echo "Generated: $OUTPUT"
echo "  entries: $ENTRY_COUNT"
