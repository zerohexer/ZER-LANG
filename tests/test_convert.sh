#!/bin/bash
# Tests for zer-convert (Phase 1) and zer-upgrade (Phase 2)
# Run: bash tests/test_convert.sh
# Requires: gcc (to build tools)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PASS=0
FAIL=0
TMPDIR="${TMPDIR:-/tmp}"

# Build tools
echo "Building zer-convert and zer-upgrade..."
gcc -std=c99 -O2 -o "$ROOT_DIR/tools/zer-convert" "$ROOT_DIR/tools/zer-convert.c"
gcc -std=c99 -O2 -o "$ROOT_DIR/tools/zer-upgrade" "$ROOT_DIR/tools/zer-upgrade.c"

check_phase1() {
    local name="$1"
    local input="$2"
    local expected="$3"

    local infile="$TMPDIR/tc_in_$$.c"
    local outfile="$TMPDIR/tc_out_$$.zer"
    echo "$input" > "$infile"
    "$ROOT_DIR/tools/zer-convert" "$infile" -o "$outfile" 2>/dev/null

    if grep -qF -- "$expected" "$outfile" 2>/dev/null; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL [Phase1] $name"
        echo "  expected to contain: $expected"
        echo "  got: $(cat "$outfile" 2>/dev/null | head -20)"
    fi
    rm -f "$infile" "$outfile"
}

check_phase1_absent() {
    local name="$1"
    local input="$2"
    local absent="$3"

    local infile="$TMPDIR/tc_in_$$.c"
    local outfile="$TMPDIR/tc_out_$$.zer"
    echo "$input" > "$infile"
    "$ROOT_DIR/tools/zer-convert" "$infile" -o "$outfile" 2>/dev/null

    if ! grep -qF -- "$absent" "$outfile" 2>/dev/null; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL [Phase1-absent] $name"
        echo "  should NOT contain: $absent"
        echo "  got: $(grep -F "$absent" "$outfile")"
    fi
    rm -f "$infile" "$outfile"
}

check_phase2() {
    local name="$1"
    local input="$2"
    local expected="$3"

    local infile="$TMPDIR/tc_in2_$$.zer"
    local outfile="$TMPDIR/tc_out2_$$.zer"
    echo "$input" > "$infile"
    "$ROOT_DIR/tools/zer-upgrade" "$infile" -o "$outfile" 2>/dev/null

    if grep -qF -- "$expected" "$outfile" 2>/dev/null; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL [Phase2] $name"
        echo "  expected to contain: $expected"
        echo "  got: $(cat "$outfile" 2>/dev/null | head -20)"
    fi
    rm -f "$infile" "$outfile"
}

check_phase2_absent() {
    local name="$1"
    local input="$2"
    local absent="$3"

    local infile="$TMPDIR/tc_in2_$$.zer"
    local outfile="$TMPDIR/tc_out2_$$.zer"
    echo "$input" > "$infile"
    "$ROOT_DIR/tools/zer-upgrade" "$infile" -o "$outfile" 2>/dev/null

    if ! grep -qF -- "$absent" "$outfile" 2>/dev/null; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL [Phase2-absent] $name"
        echo "  should NOT contain: $absent"
        echo "  got: $(grep -F "$absent" "$outfile")"
    fi
    rm -f "$infile" "$outfile"
}

echo ""
echo "=== Phase 1: zer-convert tests ==="

# --- Type mapping ---
check_phase1 "int→i32" "int x = 5;" "i32 x = 5;"
check_phase1 "unsigned int→u32" "unsigned int x;" "u32 x;"
check_phase1 "unsigned char→u8" "unsigned char c;" "u8 c;"
check_phase1 "unsigned long→u64" "unsigned long n;" "u64 n;"
check_phase1 "signed char→i8" "signed char c;" "i8 c;"
check_phase1 "long long→i64" "long long x;" "i64 x;"
check_phase1 "short→i16" "short s;" "i16 s;"
check_phase1 "uint8_t→u8" "uint8_t b;" "u8 b;"
check_phase1 "uint32_t→u32" "uint32_t v;" "u32 v;"
check_phase1 "size_t→usize" "size_t n;" "usize n;"
check_phase1 "float→f32" "float f;" "f32 f;"
check_phase1 "double→f64" "double d;" "f64 d;"
check_phase1 "char→u8" "char c = 'a';" "u8 c = 'a';"
check_phase1 "bool→bool" "_Bool b;" "bool b;"

# --- Operators ---
check_phase1 "i++→i+=1" "void f() { i++; }" "+= 1"
check_phase1 "i--→i-=1" "void f() { i--; }" "i -= 1"
check_phase1 "arrow→dot" "void f() { p->x; }" "p.x"
check_phase1 "NULL→null" "void *p = NULL;" "null"

# --- Preprocessor ---
check_phase1 "#include→cinclude" '#include <stdio.h>' 'cinclude "stdio.h";'
check_phase1 "#include quotes" '#include "myheader.h"' 'cinclude "myheader.h";'
check_phase1 "#define const" '#define SIZE 256' 'const u32 SIZE = 256;'
check_phase1 "#define macro→comment" '#define MAX(a,b) ((a)>(b)?(a):(b))' '// MANUAL:'

# --- sizeof ---
check_phase1 "sizeof(T)→@size(T)" "sizeof(Node)" "@size(Node)"
check_phase1 "sizeof(T*)→@size(*T)" "sizeof(Node *)" "@size(*Node)"

# --- C-style casts ---
check_phase1 "(int)x→@truncate" "void f() { (int)x; }" "@truncate(i32, x)"
check_phase1 "(Node*)p→@ptrcast" "void f() { (Node *)p; }" "@ptrcast(*Node, p)"
check_phase1 "(void*)p→@ptrcast(*opaque)" "void f() { (void *)p; }" "@ptrcast(*opaque, p)"

# --- void * ---
check_phase1 "void*→*opaque" "void *ptr;" "*opaque ptr;"
check_phase1 "const void*→const *opaque" "const void *p;" "const *opaque p;"

# --- struct keyword ---
check_phase1 "struct usage dropped" "void f(struct Node *n) {}" "void f(Node *n)"
check_phase1 "struct decl kept" "struct Task { int id; }" "struct Task"
check_phase1_absent "struct in usage gone" "void f(struct Node *n) {}" "struct Node"

# --- enum keyword ---
check_phase1 "enum usage dropped" "void f(enum state s) {}" "void f(state s)"
check_phase1 "enum decl kept" "enum color { RED, GREEN };" "enum color"
check_phase1_absent "enum in usage gone" "void f(enum state s) {}" "enum state"

# --- Array reorder ---
check_phase1 "int arr[10]→i32[10] arr" "void f() { int arr[10]; }" "i32[10] arr"
check_phase1 "char buf[256]→u8[256] buf" "void f() { char buf[256]; }" "u8[256] buf"
check_phase1 "uint32_t x[8]→u32[8] x" "void f() { uint32_t x[8]; }" "u32[8] x"

# --- switch/case/break ---
check_phase1 "case→.VALUE=>" 'void f() { switch(x) { case 1: break; } }' ".1 => {"
check_phase1 "default=>" 'void f() { switch(x) { default: break; } }' "default => {"
check_phase1 "break→}" 'void f() { switch(x) { case 1: break; } }' "}"
check_phase1_absent "no case keyword" 'void f() { switch(x) { case 1: break; } }' "case 1"

# --- do-while ---
check_phase1 "do-while→while(true)" "void f() { do { x++; } while (x < 10); }" "while (true)"
check_phase1 "do-while break cond" "void f() { do { x++; } while (x < 10); }" "if (!(x < 10)) { break; }"
check_phase1 "do-while body transform" "void f() { do { x++; } while (x < 10); }" "x += 1"

# --- typedef struct ---
check_phase1 "typedef struct→struct Name" "typedef struct { int x; } Point;" "struct Point {"
check_phase1 "typedef struct body types" "typedef struct { int x; } Point;" "i32 x"
check_phase1_absent "no typedef in output" "typedef struct { int x; } Point;" "typedef struct"

# --- typedef with tag ---
check_phase1 "typedef tag→name mapping" "typedef struct node { struct node *next; } Node;" "Node *next"
check_phase1_absent "tag removed in body" "typedef struct node { struct node *next; } Node;" "node *next"

# --- malloc/free → compat ---
check_phase1 "malloc→zer_malloc_bytes" "void f() { malloc(10); }" "zer_malloc_bytes"
check_phase1 "free→zer_free" "void f() { free(p); }" "zer_free"

# --- string functions → compat ---
check_phase1 "strlen→zer_strlen" "void f() { strlen(s); }" "zer_strlen"
check_phase1 "strcmp→zer_strcmp" "void f() { strcmp(a,b); }" "zer_strcmp"
check_phase1 "memcpy→zer_memcpy" "void f() { memcpy(d,s,n); }" "zer_memcpy"
check_phase1 "memset→zer_memset" "void f() { memset(d,0,n); }" "zer_memset"

# --- I/O functions stay as-is ---
check_phase1 "printf stays" '#include <stdio.h>
void f() { printf("hi"); }' 'printf("hi")'
check_phase1_absent "no zer_printf" '#include <stdio.h>
void f() { printf("hi"); }' "zer_printf"

echo ""
echo "=== Phase 2: zer-upgrade tests ==="

# --- Layer 1: compat replacements ---
check_phase2 "zer_strlen→.len" \
    'import compat;
void f() { usize n = zer_strlen(s); }' \
    "s.len"

check_phase2 "zer_strcmp==0→bytes_equal" \
    'import compat;
void f() { if (zer_strcmp(a, b) == 0) {} }' \
    "bytes_equal(a, b)"

check_phase2 "zer_strcmp!=0→!bytes_equal" \
    'import compat;
void f() { if (zer_strcmp(a, b) != 0) {} }' \
    "!bytes_equal(a, b)"

check_phase2 "zer_memcpy→bytes_copy" \
    'import compat;
void f() { zer_memcpy(dst, src, n); }' \
    "bytes_copy(dst, src)"

check_phase2 "zer_memset 0→bytes_zero" \
    'import compat;
void f() { zer_memset(buf, 0, n); }' \
    "bytes_zero(buf)"

check_phase2 "zer_memset val→bytes_fill" \
    'import compat;
void f() { zer_memset(buf, 0xFF, n); }' \
    "bytes_fill(buf, 0xFF)"

check_phase2 "zer_exit→@trap" \
    'import compat;
void f() { zer_exit(1); }' \
    "@trap()"

# --- Layer 2: malloc/free → Slab ---
check_phase2 "malloc→slab.alloc" \
    'import compat;
struct Task { i32 id; }
void f() {
    Task *t = @ptrcast(*Task, zer_malloc_bytes(@size(Task)));
}' \
    "task_slab.alloc()"

check_phase2 "free→slab.free" \
    'import compat;
struct Task { i32 id; }
void f() {
    Task *t = @ptrcast(*Task, zer_malloc_bytes(@size(Task)));
    zer_free(t);
}' \
    "task_slab.free(t_h)"

check_phase2 "field access→slab.get" \
    'import compat;
struct Task { i32 id; }
void f() {
    Task *t = @ptrcast(*Task, zer_malloc_bytes(@size(Task)));
    t.id = 5;
}' \
    "task_slab.get(t_h).id"

check_phase2 "slab declaration auto-generated" \
    'import compat;
struct Task { i32 id; }
void f() {
    Task *t = @ptrcast(*Task, zer_malloc_bytes(@size(Task)));
}' \
    "static Slab(Task) task_slab;"

# --- Primitive type exclusion ---
check_phase2_absent "no Slab(u8)" \
    'import compat;
void f() {
    u8 *buf = @ptrcast(*u8, zer_malloc_bytes(100));
}' \
    "Slab(u8)"

check_phase2_absent "no Slab(i32)" \
    'import compat;
void f() {
    i32 *arr = @ptrcast(*i32, zer_malloc_bytes(40));
}' \
    "Slab(i32)"

# --- Struct field rewriting ---
check_phase2 "struct field→Handle" \
    'import compat;
struct Task { i32 id; }
struct List {
    Task *head;
}
void f() {
    Task *t = @ptrcast(*Task, zer_malloc_bytes(@size(Task)));
}' \
    "?Handle(Task) head;"

# --- String literal preservation ---
check_phase2 "string literal preserved" \
    'import compat;
struct Task { i32 id; }
void f() {
    Task *t = @ptrcast(*Task, zer_malloc_bytes(@size(Task)));
    printf("t=%d", t.id);
}' \
    '"t=%d"'

# --- import management ---
check_phase2 "import str added" \
    'import compat;
void f() { usize n = zer_strlen(s); }' \
    "import str;"

check_phase2_absent "import compat removed when clean" \
    'import compat;
void f() { usize n = zer_strlen(s); }' \
    "import compat;"

echo ""
echo "=== Results ==="
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo "  Total:  $((PASS + FAIL))"

# Cleanup
rm -f "$ROOT_DIR/tools/zer-convert" "$ROOT_DIR/tools/zer-upgrade"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "SOME TESTS FAILED"
    exit 1
else
    echo ""
    echo "ALL TESTS PASSED"
    exit 0
fi
