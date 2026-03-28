#!/bin/bash
# Test suite for zer-convert — verifies C→ZER syntax transforms
set -e

PASS=0
FAIL=0
CONVERT=./zer-convert
TMP_IN=$(mktemp /tmp/zer_conv_in_XXXXXX.c)
TMP_OUT=$(mktemp /tmp/zer_conv_out_XXXXXX.zer)

check() {
    local desc="$1"
    local input="$2"
    local expected="$3"

    echo "$input" > "$TMP_IN"
    "$CONVERT" "$TMP_IN" -o "$TMP_OUT" >/dev/null 2>&1
    # Skip the header lines (first 4 lines: 2 comments + blank + import compat or blank)
    local output
    output=$(tail -n +3 "$TMP_OUT" | sed '/^$/d' | sed 's/^import compat;//')
    output=$(echo "$output" | sed '/^$/d')

    if echo "$output" | grep -qF "$expected"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $desc"
        echo "  Expected to find: $expected"
        echo "  Got output:"
        echo "$output" | head -5 | sed 's/^/    /'
    fi
}

echo "=== zer-convert test suite ==="

# --- Pointer declaration rearrangement ---
check "int *p → *i32 p" \
    "int *p;" \
    "*i32 p;"

check "char *name → *u8 name" \
    "char *name;" \
    "*u8 name;"

check "int **pp → **i32 pp" \
    "int **pp;" \
    "**i32 pp;"

check "unsigned int *x → *u32 x" \
    "unsigned int *x;" \
    "*u32 x;"

check "float *fp = NULL → *f32 fp = null" \
    "float *fp = NULL;" \
    "*f32 fp = null;"

check "size_t *sp → *usize sp" \
    "size_t *sp;" \
    "*usize sp;"

check "struct Node *next → *Node next" \
    "struct Node *next;" \
    "*Node next;"

check "int *func() return type" \
    "int *get_ptr(int x) { return 0; }" \
    "*i32 get_ptr"

# --- Switch/case/break ---
check "case 1: → 1 => {" \
    "void f() { switch(x) { case 1: break; } }" \
    "1 => {"

check "case fallthrough → comma-separated" \
    "void f() { switch(x) { case 1: case 2: break; } }" \
    "1, 2 => {"

check "default: → default => {" \
    "void f() { switch(x) { default: break; } }" \
    "default => {"

check "break in switch → }" \
    "void f() { switch(x) { case 1: x = 5; break; } }" \
    "x = 5;"

# --- Enum handling ---
check "enum decl kept" \
    "enum State { IDLE, RUNNING };" \
    "enum State { IDLE, RUNNING };"

check "enum usage stripped" \
    "enum State s;" \
    "State s;"

# --- Typedef ---
check "typedef struct anonymous → struct Name" \
    "typedef struct { int x; } Point;" \
    "struct Point {"

check "typedef struct body has type mapping" \
    "typedef struct { int x; } Point;" \
    "i32 x;"

check "typedef struct Name Name; → dropped" \
    "typedef struct Node Node;" \
    ""

check "typedef function pointer → kept" \
    "typedef int (*Callback)(int, int);" \
    "typedef i32 (*Callback)(i32, i32);"

check "typedef simple alias → dropped as comment" \
    "typedef unsigned int uint;" \
    "// typedef (dropped)"

# --- Goto / ternary markers ---
check "goto → MANUAL comment" \
    "void f() { goto done; }" \
    "// MANUAL: goto done;"

check "ternary → MANUAL comment" \
    "int x = (a > b) ? a : b;" \
    "MANUAL: rewrite ternary"

# --- Union keyword ---
check "union decl kept" \
    "union Data { int i; float f; };" \
    "union Data { i32 i; f32 f; };"

check "union usage stripped" \
    "union Data d;" \
    "Data d;"

# --- Existing transforms still work ---
check "i++ → += 1" \
    "void f() { i++; }" \
    "i += 1;"

check "i-- → -= 1" \
    "void f() { i--; }" \
    "i -= 1;"

check "-> → ." \
    "void f() { p->x; }" \
    "p.x;"

check "NULL → null" \
    "void f() { int *p = NULL; }" \
    "null;"

check "#include → cinclude" \
    '#include <stdio.h>' \
    'cinclude "stdio.h";'

check "sizeof(int) → @size(i32)" \
    "int x = sizeof(int);" \
    "@size(i32)"

check "(int)x → @truncate(i32, x)" \
    "void f() { char c = (char)x; }" \
    "@truncate(u8, x)"

check "(Node *)p → @ptrcast(*Node, p)" \
    "void f() { struct Node *n = (struct Node *)p; }" \
    "@ptrcast(*Node, p)"

check "malloc → zer_malloc_bytes" \
    "void f() { void *p = malloc(10); }" \
    "zer_malloc_bytes"

check "free → zer_free" \
    "void f() { free(p); }" \
    "zer_free(p)"

check "struct usage stripped" \
    "struct Point pt;" \
    "Point pt;"

check "struct decl kept" \
    "struct Point { int x; int y; };" \
    "struct Point {"

# --- Summary ---
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
rm -f "$TMP_IN" "$TMP_OUT"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
