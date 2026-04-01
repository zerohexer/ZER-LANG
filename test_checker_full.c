#include <stdio.h>
#include <string.h>
#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"

/* ================================================================
 * ZER Type Checker — Full Specification Coverage Tests
 *
 * Every rule from ZER-LANG.md must have:
 *   - Positive test (valid code passes)
 *   - Negative test (invalid code errors)
 *
 * Derived systematically from spec sections 5-22.
 * ================================================================ */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static bool run_check(const char *source, Arena *arena) {
    Scanner s; scanner_init(&s, source);
    Parser p; parser_init(&p, &s, arena, "test");
    Node *f = parse_file(&p);
    if (p.had_error) return false;
    Checker c; checker_init(&c, arena, "test");
    return checker_check(&c, f);
}

static void ok(const char *src, const char *name) {
    Arena a; arena_init(&a, 128*1024); tests_run++;
    if (run_check(src, &a)) { tests_passed++; }
    else { printf("  FAIL(ok): %s\n", name); tests_failed++; }
    arena_free(&a);
}

static void err(const char *src, const char *name) {
    Arena a; arena_init(&a, 128*1024); tests_run++;
    if (!run_check(src, &a)) { tests_passed++; }
    else { printf("  FAIL(err): %s\n", name); tests_failed++; }
    arena_free(&a);
}

/* ================================================================
 * §5 TYPE SYSTEM
 * ================================================================ */

static void test_s5_integer_coercion(void) {
    printf("[§5 integer coercion — full matrix]\n");
    /* widening: same sign, smaller → larger */
    ok("void f() { u8 a = 0; u16 b = a; }", "u8 → u16");
    ok("void f() { u8 a = 0; u32 b = a; }", "u8 → u32");
    ok("void f() { u8 a = 0; u64 b = a; }", "u8 → u64");
    ok("void f() { u16 a = 0; u32 b = a; }", "u16 → u32");
    ok("void f() { u16 a = 0; u64 b = a; }", "u16 → u64");
    ok("void f() { u32 a = 0; u64 b = a; }", "u32 → u64");
    ok("void f() { i8 a = 0; i16 b = a; }", "i8 → i16");
    ok("void f() { i8 a = 0; i32 b = a; }", "i8 → i32");
    ok("void f() { i8 a = 0; i64 b = a; }", "i8 → i64");
    ok("void f() { i16 a = 0; i32 b = a; }", "i16 → i32");
    ok("void f() { i16 a = 0; i64 b = a; }", "i16 → i64");
    ok("void f() { i32 a = 0; i64 b = a; }", "i32 → i64");

    /* unsigned → larger signed */
    ok("void f() { u8 a = 0; i16 b = a; }", "u8 → i16");
    ok("void f() { u8 a = 0; i32 b = a; }", "u8 → i32");
    ok("void f() { u16 a = 0; i32 b = a; }", "u16 → i32");
    ok("void f() { u16 a = 0; i64 b = a; }", "u16 → i64");
    ok("void f() { u32 a = 0; i64 b = a; }", "u32 → i64");

    /* narrowing: MUST error */
    err("void f() { u32 a = 0; u16 b = a; }", "u32 → u16 REJECT");
    err("void f() { u32 a = 0; u8 b = a; }", "u32 → u8 REJECT");
    err("void f() { u64 a = 0; u32 b = a; }", "u64 → u32 REJECT");
    err("void f() { i32 a = 0; i16 b = a; }", "i32 → i16 REJECT");
    err("void f() { i64 a = 0; i32 b = a; }", "i64 → i32 REJECT");

    /* signed ↔ unsigned same width: MUST error */
    err("void f() { i32 a = 0; u32 b = a; }", "i32 → u32 REJECT");
    err("void f() { u32 a = 0; i32 b = a; }", "u32 → i32 REJECT");
    err("void f() { i16 a = 0; u16 b = a; }", "i16 → u16 REJECT");
    err("void f() { i8 a = 0; u8 b = a; }", "i8 → u8 REJECT");
}

static void test_s5_float_coercion(void) {
    printf("[§5 float coercion]\n");
    ok("void f() { f32 a = 1.0; f64 b = a; }", "f32 → f64 OK");
    err("void f() { f64 a = 1.0; f32 b = a; }", "f64 → f32 REJECT");
}

static void test_s5_int_float_mixing(void) {
    printf("[§5 integer ↔ float rejection]\n");
    err("void f() { u32 a = 1; f32 b = 1.0; u32 c = a + b; }", "u32 + f32 REJECT");
    err("void f() { i32 a = 1; f64 b = 1.0; f64 c = a + b; }", "i32 + f64 REJECT");
    err("void f() { u32 a = 1; f32 b = a; }", "u32 → f32 REJECT");
    err("void f() { f32 a = 1.0; u32 b = a; }", "f32 → u32 REJECT");
}

static void test_s5_literal_flexibility(void) {
    printf("[§5 literal flexibility]\n");
    ok("void f() { u8 a = 5; }", "int lit → u8");
    ok("void f() { u16 a = 5; }", "int lit → u16");
    ok("void f() { u32 a = 5; }", "int lit → u32");
    ok("void f() { u64 a = 5; }", "int lit → u64");
    ok("void f() { i8 a = 5; }", "int lit → i8");
    ok("void f() { i16 a = 5; }", "int lit → i16");
    ok("void f() { i32 a = 5; }", "int lit → i32");
    ok("void f() { i64 a = 5; }", "int lit → i64");
    ok("void f() { usize a = 5; }", "int lit → usize");
    ok("void f() { f32 a = 3.14; }", "float lit → f32");
    ok("void f() { f64 a = 3.14; }", "float lit → f64");
    ok("void f() { i32 a = -5; }", "negative lit → i32");
    ok("void f() { i8 a = -1; }", "negative lit → i8");
}

static void test_s5_optional_coercion(void) {
    printf("[§5 T → ?T coercion]\n");
    ok("void f() { ?u32 a = 5; }", "u32 → ?u32");
    ok("void f() { ?u32 a = null; }", "null → ?u32");
    ok("void f() { ?bool a = true; }", "bool → ?bool");
    err("void f() { u32 a = null; }", "null → u32 REJECT");
}

static void test_s5_array_slice_coercion(void) {
    printf("[§5 array → slice coercion]\n");
    ok("void process([]u8 d) { }\nvoid f() { u8[64] buf; process(buf); }",
       "u8[64] → []u8 OK");
    err("void process([]u32 d) { }\nvoid f() { u8[64] buf; process(buf); }",
        "u8[64] → []u32 REJECT (wrong elem)");
}

static void test_s5_const_coercion(void) {
    printf("[§5 const coercion]\n");
    ok("void read_only(const []u8 d) { }\nvoid f() { u8[64] buf; read_only(buf); }",
       "mutable → const OK");
    /* dropping const — const slice → mutable param rejected */
    err("void mutate([]u8 d) { }\nvoid f() { const []u8 s = \"hi\"; mutate(s); }",
        "const slice → mutable param REJECT");
    err("void mutate([]u8 d) { }\nvoid f() { mutate(\"hi\"); }",
        "string literal → mutable param REJECT");
}

static void test_s5_typedef(void) {
    printf("[§5 typedef]\n");
    ok("typedef u32 Milliseconds;\nvoid f() { Milliseconds m = 1000; }", "alias works");
    ok("distinct typedef u32 Celsius;\nvoid f() { Celsius c = 25; }", "distinct lit OK");
    err("distinct typedef u32 Celsius;\ndistinct typedef u32 Fahrenheit;\n"
        "void f() { Celsius c = 25; Fahrenheit f = c; }",
        "distinct types not interchangeable");
}

/* ================================================================
 * §6 POINTER MODEL
 * ================================================================ */

static void test_s6_scope_escape(void) {
    printf("[§6 scope escape]\n");
    err("*u32 f() { u32 local = 5; return &local; }",
        "return &local REJECT");
    ok("*u32 f() { static u32 s = 5; return &s; }",
       "return &static OK");
}

static void test_s6_store_through(void) {
    printf("[§6 store-through]\n");
    err("static *u32 g;\nvoid f() { u32 local = 5; g = &local; }",
        "store &local in global REJECT");
}

static void test_s6_keep(void) {
    printf("[§6 keep parameter]\n");
    err("void reg(keep *u32 ctx) { }\nvoid f() { u32 local = 5; reg(&local); }",
        "&local to keep REJECT");
    ok("void reg(keep *u32 ctx) { }\nstatic u32 g = 5;\nvoid f() { reg(&g); }",
       "&static to keep OK");
}

static void test_s6_deref(void) {
    printf("[§6 pointer dereference]\n");
    err("void f() { u32 x = 5; u32 y = *x; }", "deref non-pointer REJECT");
    ok("void f() { u32 x = 5; *u32 p = &x; u32 y = *p; }", "deref pointer OK");
}

/* ================================================================
 * §8 SAFETY MODEL
 * ================================================================ */

static void test_s8_exhaustive_switch(void) {
    printf("[§8 exhaustive switch]\n");
    ok("void a(){} void b(){}\nvoid f() { switch(42) { 0=>a(), default=>b(), } }",
       "int switch with default OK");
    err("void a(){}\nvoid f() { switch(42) { 0=>a(), } }",
        "int switch without default REJECT");
    ok("void a(){} void b(){}\nvoid f() { switch(true) { true=>a(), false=>b(), } }",
       "bool switch exhaustive OK");
    err("void a(){}\nvoid f() { switch(true) { true=>a(), } }",
        "bool switch missing false REJECT");
}

static void test_s8_no_fallthrough(void) {
    printf("[§8 switch arms isolated]\n");
    /* each arm is a separate expression/block — no fallthrough by syntax */
    ok("void a(){} void b(){}\nvoid f() { switch(42) { 0=>a(), 1=>b(), default=>a(), } }",
       "multiple arms OK");
}

/* ================================================================
 * §11 ERROR HANDLING — ?T + orelse
 * ================================================================ */

static void test_s11_orelse(void) {
    printf("[§11 orelse]\n");
    ok("void f() { ?u32 x = 5; u32 y = x orelse 0; }", "orelse value OK");
    ok("void f() { ?u32 x = 5; u32 y = x orelse return; }", "orelse return OK");
    ok("void f() { while(true) { ?u32 x = 5; u32 y = x orelse break; } }",
       "orelse break OK");
    ok("void f() { while(true) { ?u32 x = 5; u32 y = x orelse continue; } }",
       "orelse continue OK");
    err("void f() { u32 x = 5; u32 y = x orelse 0; }", "orelse non-optional REJECT");

    /* chained orelse */
    ok("?u32 a() { return null; }\n?u32 b() { return null; }\n"
       "void f() { u32 x = a() orelse b() orelse 0; }",
       "chained orelse OK");
}

static void test_s11_if_unwrap(void) {
    printf("[§11 if-unwrap]\n");
    ok("void f() { ?u32 m = 5; if (m) |v| { u32 x = v; } }", "if-unwrap OK");
    ok("void f() { ?u32 m = 5; if (m) |*v| { *v = 10; } }", "if-unwrap |*val| OK");
    err("void f() { u32 m = 5; if (m) |v| { } }", "if-unwrap non-optional REJECT");
    /* const capture: |val| is immutable */
    err("void f() { ?u32 m = 5; if (m) |v| { v = 10; } }",
        "assign to const capture REJECT");
}

/* ================================================================
 * §12 STRUCTS, ENUMS, UNIONS
 * ================================================================ */

static void test_s12_struct(void) {
    printf("[§12 struct]\n");
    ok("struct T { u32 x; u32 y; }\nvoid f() { T t; t.x = 5; }",
       "struct field access OK");
    ok("struct T { u32 x; }\nvoid f(T t) { *T p = &t; p.x = 5; }",
       "pointer auto-deref field OK");
    /* nonexistent field — must error (UFCS dropped) */
    err("struct T { u32 x; }\nvoid f() { T t; t.nonexistent; }",
        "struct nonexistent field → error");
}

static void test_s12_enum(void) {
    printf("[§12 enum]\n");
    ok("enum Color { red, green, blue }", "enum declaration OK");
}

static void test_s12_union(void) {
    printf("[§12 union]\n");
    err("union M { u32 a; u32 b; }\nvoid f(M m) { u32 x = m.a; }",
        "union field READ rejected");
    ok("union M { u32 a; u32 b; }\nvoid f(M m) { m.a = 5; }",
       "union field WRITE for construction OK");
    ok("union M { u32 a; u32 b; }\n"
       "void f(M m) {\n"
       "    switch (m) {\n"
       "        .a => |v| { u32 x = v; },\n"
       "        .b => |v| { u32 x = v; },\n"
       "    }\n"
       "}",
       "union switch capture (param) OK");
    ok("union M { u32 a; u32 b; }\n"
       "void f() {\n"
       "    M m;\n"
       "    switch (m) {\n"
       "        .a => |v| { u32 x = v; },\n"
       "        .b => |v| { u32 x = v; },\n"
       "    }\n"
       "}",
       "union switch capture (local var) OK");
}

/* ================================================================
 * §17 CONTROL FLOW
 * ================================================================ */

static void test_s17_control_flow(void) {
    printf("[§17 control flow]\n");
    ok("void f() { if (true) { } }", "if bool OK");
    ok("void f() { if (true) { } else { } }", "if-else OK");
    err("void f() { if (42) { } }", "if integer REJECT");
    ok("void f() { for (u32 i = 0; i < 10; i += 1) { } }", "for loop OK");
    ok("void f() { while (true) { } }", "while loop OK");
    err("void f() { break; }", "break outside loop REJECT");
    err("void f() { continue; }", "continue outside loop REJECT");
    ok("void f() { while(true) { break; } }", "break inside loop OK");
    ok("void f() { for(u32 i=0;i<10;i+=1) { continue; } }", "continue inside for OK");
}

static void test_s17_return(void) {
    printf("[§17 return]\n");
    ok("u32 f() { return 5; }", "return matching type OK");
    ok("void f() { return; }", "void return OK");
    ok("?u32 f() { return null; }", "return null from ?T OK");
    ok("?u32 f() { return 5; }", "return value from ?T OK");
    ok("?u32 f() { return; }", "bare return from ?T OK");
    err("u32 f() { return; }", "bare return from u32 REJECT");
    err("u32 f() { return true; }", "return bool from u32 REJECT");
}

/* ================================================================
 * §18 OPERATORS
 * ================================================================ */

static void test_s18_arithmetic(void) {
    printf("[§18 arithmetic operators]\n");
    ok("void f() { u32 a = 1; u32 b = 2; u32 c = a + b; }", "u32 + u32 OK");
    ok("void f() { u32 a = 1; u32 b = a - 1; }", "u32 - u32 OK");
    ok("void f() { u32 a = 2; u32 b = a * 3; }", "u32 * u32 OK");
    ok("void f() { u32 a = 10; u32 b = a / 2; }", "u32 / u32 OK");
    ok("void f() { u32 a = 10; u32 b = a % 3; }", "u32 % u32 OK");
    err("void f() { bool a = true; bool b = a + true; }", "bool + bool REJECT");
}

static void test_s18_bitwise(void) {
    printf("[§18 bitwise operators]\n");
    ok("void f() { u32 a = 0xFF; u32 b = a & 0x0F; }", "u32 & u32 OK");
    ok("void f() { u32 a = 0; u32 b = a | 0xFF; }", "u32 | u32 OK");
    ok("void f() { u32 a = 0xFF; u32 b = a ^ 0x0F; }", "u32 ^ u32 OK");
    ok("void f() { u32 a = 1; u32 b = a << 3; }", "u32 << u32 OK");
    ok("void f() { u32 a = 8; u32 b = a >> 1; }", "u32 >> u32 OK");
    ok("void f() { u32 a = 0xFF; u32 b = ~a; }", "~u32 OK");
    err("void f() { bool a = true; bool b = ~a; }", "~bool REJECT");
    err("void f() { bool a = true; bool b = a & true; }", "bool & bool REJECT");
}

static void test_s18_comparison(void) {
    printf("[§18 comparison operators]\n");
    ok("void f() { u32 a = 1; bool b = a == 1; }", "u32 == u32 OK");
    ok("void f() { u32 a = 1; bool b = a != 0; }", "u32 != u32 OK");
    ok("void f() { u32 a = 1; bool b = a < 10; }", "u32 < u32 OK");
    ok("void f() { u32 a = 1; bool b = a > 0; }", "u32 > u32 OK");
    ok("void f() { u32 a = 1; bool b = a <= 10; }", "u32 <= u32 OK");
    ok("void f() { u32 a = 1; bool b = a >= 0; }", "u32 >= u32 OK");
}

static void test_s18_logical(void) {
    printf("[§18 logical operators]\n");
    ok("void f() { bool a = true; bool b = false; bool c = a && b; }", "&& OK");
    ok("void f() { bool a = true; bool b = false; bool c = a || b; }", "|| OK");
    ok("void f() { bool a = true; bool b = !a; }", "!bool OK");
    err("void f() { u32 a = 1; bool b = !a; }", "!u32 REJECT");
    err("void f() { u32 a = 1; bool b = a && true; }", "u32 && bool REJECT");
}

static void test_s18_assignment(void) {
    printf("[§18 assignment operators]\n");
    ok("void f() { u32 x = 0; x = 5; }", "= OK");
    ok("void f() { u32 x = 0; x += 1; }", "+= OK");
    ok("void f() { u32 x = 0; x -= 1; }", "-= OK");
    ok("void f() { u32 x = 1; x *= 2; }", "*= OK");
    ok("void f() { u32 x = 10; x /= 2; }", "/= OK");
    ok("void f() { u32 x = 10; x %= 3; }", "%= OK");
    ok("void f() { u32 x = 0xFF; x &= 0x0F; }", "&= OK");
    ok("void f() { u32 x = 0; x |= 0xFF; }", "|= OK");
    ok("void f() { u32 x = 0xFF; x ^= 0x0F; }", "^= OK");
    ok("void f() { u32 x = 1; x <<= 3; }", "<<= OK");
    ok("void f() { u32 x = 8; x >>= 1; }", ">>= OK");
    err("void f() { const u32 x = 5; x = 10; }", "assign to const REJECT");
    err("void f() { bool b = true; b += true; }", "bool += REJECT");
}

/* ================================================================
 * §19 DEFER
 * ================================================================ */

static void test_s19_defer(void) {
    printf("[§19 defer]\n");
    ok("void cleanup() { }\nvoid f() { defer cleanup(); }", "defer expr OK");
    ok("void a() {} void b() {}\nvoid f() { defer { a(); b(); } }", "defer block OK");
}

/* ================================================================
 * §22 POOL, RING, ARENA — BUILTINS
 * ================================================================ */

static void test_s22_pool(void) {
    printf("[§22 Pool builtins]\n");
    ok("struct T { u32 x; }\nPool(T, 8) p;\n"
       "void f() { Handle(T) h = p.alloc() orelse return; p.get(h).x = 5; p.free(h); }",
       "pool alloc/get/free OK");
    err("struct T { u32 x; }\nPool(T, 8) p;\n"
        "void f() { Handle(T) h = p.alloc() orelse return; *T ptr = p.get(h); }",
        "store get() result REJECT");
}

static void test_s22_ring(void) {
    printf("[§22 Ring builtins]\n");
    ok("Ring(u8, 256) r;\nvoid f() { r.push(0xFF); }",
       "ring push OK");
    ok("Ring(u8, 256) r;\nvoid f() { if (r.pop()) |b| { u8 x = b; } }",
       "ring pop + unwrap OK");
}

static void test_s22_arena(void) {
    printf("[§22 Arena builtins]\n");
    ok("Arena scratch;\nvoid f() { defer scratch.reset(); }",
       "arena reset inside defer OK (no warning)");

    /* BUG-026: arena.alloc(T) → ?*T */
    ok("struct Task { u32 id; }\nArena a;\n"
       "void f() { *Task t = a.alloc(Task) orelse return; t.id = 1; }",
       "arena.alloc(Task) returns ?*Task — unwrap to *Task OK");

    err("struct Task { u32 id; }\nArena a;\n"
        "void f() { u32 t = a.alloc(Task) orelse return; }",
        "arena.alloc(Task) returns ?*Task, assign to u32 REJECT");

    /* BUG-027: arena.alloc_slice(T, n) → ?[]T
     * Note: uses struct type since primitive keywords (u32 etc.) can't be
     * passed as arguments — they're keywords, not identifiers */
    ok("struct Elem { u32 v; }\nArena a;\n"
       "void f() { []Elem buf = a.alloc_slice(Elem, 10) orelse return; }",
       "arena.alloc_slice(Elem, 10) returns ?[]Elem — unwrap to []Elem OK");

    err("struct Elem { u32 v; }\nArena a;\n"
        "void f() { u32 buf = a.alloc_slice(Elem, 10) orelse return; }",
        "arena.alloc_slice(Elem, 10) returns ?[]Elem, assign to u32 REJECT");
}

/* ================================================================
 * §16 INTRINSICS
 * ================================================================ */

static void test_s16_intrinsics(void) {
    printf("[§16 intrinsics]\n");
    ok("void f() { usize s = @size(u32); }", "@size OK");
    ok("void f() { @barrier(); }", "@barrier OK");
    ok("void f() { @barrier_store(); }", "@barrier_store OK");
    ok("void f() { @barrier_load(); }", "@barrier_load OK");
    ok("void f() { u32 x = 5; usize a = @ptrtoint(&x); }", "@ptrtoint OK");
    err("void f() { @unknown(); }", "unknown intrinsic REJECT");
}

/* ================================================================
 * INTERACTION TESTS — features combined
 * ================================================================ */

static void test_interactions(void) {
    printf("[feature interactions]\n");

    /* orelse + if-unwrap */
    ok("?u32 read() { return 5; }\n"
       "void f() {\n"
       "    ?u32 val = read();\n"
       "    if (val) |v| { u32 x = v; }\n"
       "}",
       "optional → if-unwrap");

    /* defer + loop + break */
    ok("void cleanup() { }\n"
       "void f() {\n"
       "    defer cleanup();\n"
       "    while (true) { break; }\n"
       "}",
       "defer + loop + break");

    /* nested if-unwrap */
    ok("void f() {\n"
       "    ?u32 a = 5; ?u32 b = 10;\n"
       "    if (a) |va| { if (b) |vb| { u32 sum = va + vb; } }\n"
       "}",
       "nested if-unwrap");

    /* struct + pool + function */
    ok("struct Task { u32 pid; }\n"
       "Pool(Task, 4) tasks;\n"
       "u32 get_pid() {\n"
       "    Handle(Task) h = tasks.alloc() orelse return;\n"
       "    tasks.get(h).pid = 42;\n"
       "    tasks.free(h);\n"
       "    return 1;\n"
       "}",
       "struct + pool + return");

    /* UFCS dropped — must error */
    err("struct Task { u32 pid; }\n"
        "void run(*Task t) { }\n"
        "void f() { Task t; t.run(); }",
        "UFCS dropped: struct.method() → error");

    /* forward reference + recursion */
    ok("void a() { b(); }\nvoid b() { a(); }", "mutual recursion OK");

    /* multiple return paths */
    ok("u32 f(bool c) { if (c) { return 1; } return 0; }",
       "multiple return paths OK");

    /* for loop variable scoping */
    ok("void f() {\n"
       "    for (u32 i = 0; i < 10; i += 1) { u32 x = i; }\n"
       "}",
       "for loop var scoped to body");

    /* string literal */
    ok("void f() { const []u8 msg = \"hello\"; }", "string literal OK");
    err("void f() { []u8 msg = \"hello\"; }",
        "string literal to mutable slice → error (BUG-124)");

    /* void function no return */
    ok("void f() { u32 x = 5; }", "void function no return OK");
}

/* ================================================================
 * BOUNDARY / ADVERSARIAL TESTS
 * ================================================================ */

static void test_adversarial(void) {
    printf("[adversarial tests]\n");

    /* empty function */
    ok("void f() { }", "empty function OK");

    /* empty struct */
    ok("struct Empty { }", "empty struct OK");

    /* deeply nested expressions */
    ok("void f() { u32 x = ((((1 + 2) * 3) - 4) / 5); }",
       "deeply nested parens OK");

    /* many parameters */
    ok("void f(u32 a, u32 b, u32 c, u32 d, u32 e) { u32 x = a + b + c + d + e; }",
       "5 parameters OK");

    /* chained field access */
    ok("struct Inner { u32 x; }\nstruct Outer { Inner inner; }\n"
       "void f(Outer o) { u32 v = o.inner.x; }",
       "chained struct field OK");

    /* chained method calls */
    ok("struct T { u32 x; }\nPool(T, 4) p;\n"
       "void f() {\n"
       "    Handle(T) h = p.alloc() orelse return;\n"
       "    p.get(h).x = p.get(h).x + 1;\n"
       "    p.free(h);\n"
       "}",
       "get().field = get().field + 1 OK");

    /* index + slice */
    ok("void f() { u8[10] arr; u8 x = arr[0]; []u8 s = arr[0..5]; }",
       "array index + slice OK");

    /* deref + field */
    ok("struct T { u32 x; }\nvoid f(T val) { *T p = &val; u32 x = p.x; }",
       "deref + field OK");

    /* multiple declarations in block */
    ok("void f() { u32 a = 1; u32 b = 2; u32 c = 3; u32 d = a + b + c; }",
       "multiple local vars OK");

    /* different struct types not interchangeable */
    err("struct A { u32 x; }\nstruct B { u32 x; }\n"
        "void f() { A a; B b = a; }",
        "different struct types REJECT");
}

/* ================================================================
 * SECURITY REVIEW FIXES (BUG-078 through BUG-083)
 * ================================================================ */
static void test_security_review(void) {
    printf("[BUG-080: scope escape via struct field]\n");
    err("struct H { *u32 ptr; }\n"
        "H g;\n"
        "void f() {\n"
        "    u32 local = 42;\n"
        "    g.ptr = &local;\n"
        "}\n",
        "scope escape: global.field = &local");

    ok("struct H { *u32 ptr; }\n"
       "H g;\n"
       "u32 gval = 5;\n"
       "void f() {\n"
       "    g.ptr = &gval;\n"
       "}\n",
       "scope escape: global.field = &global (valid)");

    printf("[BUG-081: union type confusion]\n");
    err("union D { u32 a; u32 b; }\n"
        "void f() {\n"
        "    D d;\n"
        "    d.a = 1;\n"
        "    switch (d) {\n"
        "        .a => |*ptr| {\n"
        "            d.b = 2;\n"
        "        }\n"
        "        .b => |v| { }\n"
        "    }\n"
        "}\n",
        "union mutation during mutable capture");

    ok("union D { u32 a; u32 b; }\n"
       "void f() {\n"
       "    D d1;\n"
       "    D d2;\n"
       "    d1.a = 1;\n"
       "    d2.b = 2;\n"
       "    switch (d1) {\n"
       "        .a => |*ptr| {\n"
       "            d2.b = 99;\n"
       "        }\n"
       "        .b => |v| { }\n"
       "    }\n"
       "}\n",
       "union mutation of DIFFERENT union during capture (valid)");

    ok("union D { u32 a; u32 b; }\n"
       "void f() {\n"
       "    D d;\n"
       "    d.a = 1;\n"
       "    switch (d) {\n"
       "        .a => {\n"
       "            d.b = 2;\n"
       "        }\n"
       "        .b => { }\n"
       "    }\n"
       "}\n",
       "union mutation in non-capture arm (valid)");

    printf("[BUG-083: arena lifetime escape]\n");
    err("struct Data { u32 x; }\n"
        "struct Hold { *Data ptr; }\n"
        "Hold g;\n"
        "void f() {\n"
        "    u8[512] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    defer a.unsafe_reset();\n"
        "    *Data d = a.alloc(Data) orelse return;\n"
        "    g.ptr = d;\n"
        "}\n",
        "arena ptr escape to global struct field");

    ok("struct Data { u32 x; }\n"
       "void f() {\n"
       "    u8[512] buf;\n"
       "    Arena a = Arena.over(buf);\n"
       "    defer a.unsafe_reset();\n"
       "    *Data d = a.alloc(Data) orelse return;\n"
       "    d.x = 42;\n"
       "}\n",
       "arena ptr used locally (valid)");

    printf("[gap fixes: builtin method errors, arena alias, union lock via ptr]\n");

    /* unknown builtin methods */
    err("struct T { u32 x; }\n"
        "Pool(T, 4) p;\n"
        "void f() { p.bogus(); }\n",
        "Pool unknown method → error");

    err("Ring(u8, 16) r;\n"
        "void f() { r.clear(); }\n",
        "Ring unknown method → error");

    err("void f() {\n"
        "    u8[64] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    a.destroy();\n"
        "}\n",
        "Arena unknown method → error");

    /* arena-derived propagation through alias */
    err("struct D { u32 x; }\n"
        "*D g;\n"
        "void f() {\n"
        "    u8[512] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    defer a.unsafe_reset();\n"
        "    *D d = a.alloc(D) orelse return;\n"
        "    *D q = d;\n"
        "    g = q;\n"
        "}\n",
        "arena alias escape via var-decl → error");

    err("struct D { u32 x; }\n"
        "*D g;\n"
        "void f() {\n"
        "    u8[512] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    defer a.unsafe_reset();\n"
        "    *D d = a.alloc(D) orelse return;\n"
        "    *D q = a.alloc(D) orelse return;\n"
        "    q = d;\n"
        "    g = q;\n"
        "}\n",
        "arena alias escape via assignment → error");

    /* BUG-153: integer literal overflow */
    err("void f() { u8 x = 256; }",
        "u8 x = 256 → overflow REJECT");
    err("void f() { i8 x = 128; }",
        "i8 x = 128 → overflow REJECT");
    err("void f() { u8 x = -1; }",
        "u8 x = -1 → negative unsigned REJECT");
    ok("void f() { u8 x = 255; }",
       "u8 x = 255 → max OK");
    ok("void f() { i8 x = -128; }",
       "i8 x = -128 → min OK");

    /* BUG-154: bit extraction out of bounds */
    err("void f() { u8 v = 0; u32 x = v[15..0]; }",
        "u8 bit[15..0] → index 15 out of range REJECT");
    ok("void f() { u8 v = 0; u32 x = v[7..0]; }",
       "u8 bit[7..0] → max valid index OK");

    /* BUG-143: return arena-derived pointer from local arena */
    err("struct Task { u32 id; }\n"
        "*Task bad() {\n"
        "    u8[1024] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    *Task t = a.alloc(Task) orelse return;\n"
        "    return t;\n"
        "}\n",
        "arena return escape from local arena → error");

    /* BUG-193: cross-module type collision */
    /* tested via module tests — collision_test.zer gives helpful error */

    /* BUG-191: duplicate struct field names */
    err("struct S { u32 x; u32 x; }",
        "duplicate field 'x' in struct REJECT");

    /* BUG-192: return inside defer */
    err("u32 f() { defer { return 5; } return 1; }",
        "return inside defer REJECT");

    err("void f() { while (true) { defer { break; } } }",
        "break inside defer REJECT");

    /* BUG-190: missing return in non-void function */
    err("u32 bad(bool c) { if (c) { return 1; } }",
        "missing return — if without else REJECT");
    err("?u32 bad2(bool c) { if (c) { return 1; } }",
        "missing return — ?u32 function REJECT");
    ok("u32 good(bool c) { if (c) { return 1; } else { return 0; } }",
       "all paths return — if/else OK");
    ok("u32 good2(bool c) { if (c) { return 1; } return 0; }",
       "all paths return — fallthrough return OK");

    /* BUG-182: const array → mutable slice coercion */
    err("void mutate([]u32 s) { s[0] = 0; }\nvoid f() { const u32[4] arr; mutate(arr); }",
        "const array → mutable slice param REJECT");

    /* BUG-184: slice start > end (constant) */
    err("void f() { u8[10] arr; []u8 s = arr[5..2]; }",
        "slice arr[5..2] start > end REJECT");

    /* BUG-177: const pointer deref mutation */
    err("void f(const *u32 p) { *p = 5; }",
        "write through const pointer REJECT");

    /* BUG-178: const struct field mutation */
    err("struct S { u32 val; }\nvoid f(const *S p) { p.val = 10; }",
        "write through const pointer field REJECT");

    /* BUG-179: slice start > end */
    err("void f() { u8[10] arr; []u8 s = arr[5..2]; }",
        "slice start > end REJECT");

    /* BUG-174: global array init from variable */
    err("u32[4] a;\nu32[4] b = a;\n",
        "global array init from variable REJECT");

    /* BUG-175: void variable declaration */
    err("void f() { void x; }",
        "void variable REJECT");

    /* BUG-176: deep const — type_equals strict */
    err("void f() { u32 x; *u32 p = &x; const **u32 cp = &p; **u32 mp = cp; }",
        "deep const launder via **u32 REJECT");

    /* BUG-171: global non-constant initializer */
    err("u32 f() { return 1; }\nu32 g = f();\n",
        "global var with function call init REJECT");

    /* BUG-168: orelse fallback escape */
    err("*u32 leak(?*u32 opt) { u32 x = 42; return opt orelse &x; }",
        "return opt orelse &local REJECT");

    /* BUG-169: division by literal zero */
    err("void f() { u32 x = 10 / 0; }",
        "10 / 0 compile-time REJECT");
    err("void f() { u32 x = 10 %% 0; }",
        "10 %% 0 compile-time REJECT");

    /* BUG-170: slice/array comparison */
    err("void f() { u8[4] a; u8[4] b; []u8 sa = a[0..4]; []u8 sb = b[0..4]; if (sa == sb) {} }",
        "slice == slice REJECT");

    /* BUG-165: const laundering via assignment */
    err("void f() { u32 x = 42; const *u32 c = &x; *u32 m; m = c; }",
        "const ptr assign to mutable REJECT");

    /* BUG-166: const laundering via orelse init */
    err("?const *u32 get() { return null; }\n"
        "void f() { *u32 m = get() orelse return; }",
        "const ptr orelse to mutable var REJECT");

    /* BUG-162: slice-to-pointer null hole */
    err("void clear(*u8 data) { *data = 0; }\nvoid f() { []u8 empty; clear(empty); }",
        "[]u8 → *u8 coercion REJECT (null hole)");

    /* BUG-163: pointer escape via local variable */
    err("*u32 leak() { u32 x = 42; *u32 p = &x; return p; }",
        "return local-derived pointer REJECT");

    /* BUG-157: const laundering via return */
    err("*u32 wash(const *u32 p) { return p; }",
        "const ptr → mutable return REJECT");

    /* BUG-159: return &local[i] */
    err("*u8 bad() { u8[10] arr; return &arr[0]; }",
        "return &local[i] REJECT");

    /* BUG-161: local Pool on stack */
    err("struct Task { u32 id; }\nvoid f() { Pool(Task, 8) p; }",
        "local Pool on stack REJECT");

    /* BUG-155: arena return via struct field */
    err("struct Val { u32 x; }\n"
        "struct Holder { *Val ptr; }\n"
        "*Val bad() {\n"
        "    u8[1024] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    Holder h;\n"
        "    h.ptr = a.alloc(Val) orelse return;\n"
        "    return h.ptr;\n"
        "}\n",
        "arena return via struct field → error");

    /* BUG-158: arena-derived via field read */
    err("struct Val { u32 x; }\n"
        "struct Wrap { *Val p; }\n"
        "*Val leak() {\n"
        "    u8[1024] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    Wrap w;\n"
        "    w.p = a.alloc(Val) orelse return;\n"
        "    *Val p = w.p;\n"
        "    return p;\n"
        "}\n",
        "arena read via struct field → propagate + error");

    /* BUG-144: string literal → ?[]u8 return */
    err("?[]u8 get_opt() { return \"hello\"; }\n",
        "string literal → ?[]u8 return → error");

    /* union switch lock via pointer deref */
    err("union D { u32 a; u32 b; }\n"
        "void f(*D ptr) {\n"
        "    switch (*ptr) {\n"
        "        .a => |*v| {\n"
        "            ptr.b = 99;\n"
        "        }\n"
        "        .b => |v| { }\n"
        "    }\n"
        "}\n",
        "union mutation via *ptr in switch arm → error");

    /* BUG-100: orelse break/continue outside loop */
    err("?u32 get() { return 42; }\n"
        "u32 main() { u32 x = get() orelse break; return x; }\n",
        "orelse break outside loop → error");
    err("?u32 get() { return 42; }\n"
        "u32 main() { u32 x = get() orelse continue; return x; }\n",
        "orelse continue outside loop → error");
    ok("?u32 get() { return 42; }\n"
       "void f() {\n"
       "    for (u32 i = 0; i < 1; i += 1) {\n"
       "        u32 x = get() orelse break;\n"
       "    }\n"
       "}\n",
       "orelse break inside loop (valid)");

    /* BUG-101b: calling non-callable type */
    err("void main() { u32 x = 5; x(10); }\n",
        "call non-function u32 → error");
    err("void main() { bool b = true; b(); }\n",
        "call non-function bool → error");

    /* intrinsic argument validation */
    printf("[BUG-106/107/108/109: intrinsic arg validation]\n");
    err("u32 main() { *u32 p = @ptrcast(*u32, 42); return 0; }\n",
        "@ptrcast non-pointer source → error");
    err("struct S { u32 x; }\n"
        "u32 main() { S s; *u32 p = @inttoptr(*u32, s); return 0; }\n",
        "@inttoptr struct source → error");
    err("usize main() { u32 x = 5; return @ptrtoint(x); }\n",
        "@ptrtoint non-pointer → error");
    err("struct S { u32 x; u32 y; }\n"
        "usize main() { return @offset(S, bogus); }\n",
        "@offset non-existent field → error");
    ok("struct S { u32 x; u32 y; }\n"
       "usize f() { return @offset(S, y); }\n",
       "@offset valid field (OK)");

    /* BUG-118: arena-derived in if-unwrap capture */
    printf("[BUG-118: arena if-unwrap capture escape]\n");
    err("struct Task { u32 id; }\n"
        "*Task g;\n"
        "void f() {\n"
        "    u8[1024] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    defer a.unsafe_reset();\n"
        "    if (a.alloc(Task)) |t| {\n"
        "        g = t;\n"
        "    }\n"
        "}\n",
        "arena if-unwrap capture escape to global → error");

    /* BUG-120: return local array as slice — dangling pointer */
    printf("[BUG-120: return local array as slice]\n");
    err("[]u8 f() {\n"
        "    u8[64] buf;\n"
        "    buf[0] = 42;\n"
        "    return buf;\n"
        "}\n",
        "return local array as slice → error");
    ok("u8[64] g_buf;\n"
       "[]u8 f() { return g_buf; }\n",
       "return global array as slice (valid)");

    /* BUG-122: dangling slice via assignment */
    printf("[BUG-122: local array → global slice assignment]\n");
    err("[]u8 g;\n"
        "void f() { u8[64] b; g = b; }\n",
        "local array to global slice assignment → error");

    /* BUG-114: switch exhaustiveness on distinct enum */
    printf("[BUG-114: switch on distinct enum]\n");
    err("enum Color { red, green, blue }\n"
        "distinct typedef Color Shade;\n"
        "void f() {\n"
        "    Shade s = @cast(Shade, Color.red);\n"
        "    switch (s) { .red => { } }\n"
        "}\n",
        "distinct enum non-exhaustive → error");

    /* BUG-115: arena.alloc_slice escape */
    printf("[BUG-115: arena.alloc_slice escape]\n");
    err("struct D { u32 x; }\n"
        "[]D g;\n"
        "void f() {\n"
        "    u8[1024] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    defer a.unsafe_reset();\n"
        "    []D s = a.alloc_slice(D, 4) orelse return;\n"
        "    g = s;\n"
        "}\n",
        "arena.alloc_slice escape to global → error");

    /* BUG-092: builtin arg count validation */
    printf("[BUG-092: builtin wrong arg counts]\n");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) p;\n"
        "void f() { p.alloc(42); }\n",
        "pool.alloc(42) — too many args");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) p;\n"
        "void f() { p.get(); }\n",
        "pool.get() — missing arg");
    err("struct T { u32 x; }\n"
        "Pool(T, 4) p;\n"
        "void f() { p.free(); }\n",
        "pool.free() — missing arg");
    err("Ring(u8, 16) r;\n"
        "void f() { r.push(); }\n",
        "ring.push() — missing arg");
    err("Ring(u8, 16) r;\n"
        "void f() { r.pop(42); }\n",
        "ring.pop(42) — too many args");
}

/* ================================================================
 * NEGATIVE TEST SWEEP — every uncovered checker_error() path
 * Each test triggers a specific rejection rule that had no test.
 * ================================================================ */
static void test_negative_sweep(void) {
    printf("[negative test sweep — 24 uncovered rejection paths]\n");

    /* 1. redefinition */
    err("void f() { u32 x = 1; u32 x = 2; }",
        "redefinition of variable in same scope");

    /* 2. undefined identifier */
    err("void f() { u32 x = nonexistent; }",
        "undefined identifier rejected");

    /* 3. undefined type */
    err("void f() { NoSuchType x; }",
        "undefined type name rejected");

    /* 4. cannot compare incompatible types */
    err("void f() { bool b = true; u32 x = 5; bool r = b == x; }",
        "compare bool == u32 rejected");

    /* 5. unary minus on non-numeric */
    err("void f() { bool b = true; bool c = -b; }",
        "unary minus on bool rejected");

    /* 6. bitwise compound on float */
    err("void f() { f32 x = 1.0; x &= 2; }",
        "f32 &= rejected (bitwise on float)");

    /* 7. compound narrowing */
    err("void f() { u8 x = 1; u64 big = 99; x += big; }",
        "u8 += u64 rejected (narrowing)");

    /* 8. wrong arg type in function call */
    err("void process(u32 x) { }\n"
        "void f() { process(true); }",
        "wrong arg type: bool for u32 rejected");

    /* 9. no field on pointer auto-deref */
    err("struct Pt { u32 x; }\n"
        "void f() { *Pt p = &p; u32 y = p.nonexistent; }",
        "no field on *Struct rejected");

    /* 10. invalid enum variant access */
    err("enum Color { red, green, blue }\n"
        "void f() { Color c = Color.purple; }",
        "nonexistent enum variant rejected");

    /* 11. invalid union variant write */
    err("struct A { u32 x; }\n"
        "union U { A a; }\n"
        "void f() { U u; u.nonexistent.x = 1; }",
        "nonexistent union variant write rejected");

    /* 12. array index must be integer */
    err("void f() { u8[4] buf; u8 x = buf[true]; }",
        "array index with bool rejected");

    /* 13. slice start must be integer */
    err("void f() { u8[10] buf; []u8 s = buf[true..5]; }",
        "slice start non-integer rejected");

    /* 14. slice end must be integer */
    err("void f() { u8[10] buf; []u8 s = buf[0..true]; }",
        "slice end non-integer rejected");

    /* 15. orelse fallback type mismatch */
    err("?u32 get() { return null; }\n"
        "void f() { bool x = get() orelse true; }",
        "orelse fallback type mismatch rejected");

    /* 16. @bitcast width mismatch */
    err("void f() { u32 x = 42; i16 y = @bitcast(i16, x); }",
        "@bitcast mismatched width rejected");

    /* 17. @truncate non-numeric source */
    err("struct Foo { u32 x; }\n"
        "void f() { Foo foo; u8 x = @truncate(u8, foo); }",
        "@truncate on struct rejected");

    /* 18. @saturate non-numeric source */
    err("struct Bar { u32 v; }\n"
        "void f() { Bar b; u8 x = @saturate(u8, b); }",
        "@saturate on struct rejected");

    /* 19. @saturate float target */
    err("void f() { u32 x = 100; f32 y = @saturate(f32, x); }",
        "@saturate float target rejected");

    /* 20. @cast non-distinct target */
    err("void f() { u32 x = 5; u32 y = @cast(u32, x); }",
        "@cast non-distinct target rejected");

    /* 21. for condition non-bool */
    err("void f() { for (u32 i = 0; i; i += 1) { } }",
        "for condition non-bool rejected");

    /* 22. while condition non-bool */
    err("void f() { u32 x = 1; while (x) { } }",
        "while condition non-bool rejected");

    /* 23. enum switch not exhaustive */
    err("enum Dir { north, south, east, west }\n"
        "void f() { Dir d = Dir.north; switch (d) { .north => { } .south => { } } }",
        "enum switch non-exhaustive rejected");

    /* 24. union switch not exhaustive */
    err("struct A { u32 x; }\nstruct B { u32 y; }\n"
        "union U { A a; B b; }\n"
        "void f() { U u; u.a.x = 1; switch (u) { .a => |v| { } } }",
        "union switch non-exhaustive rejected");

    /* 25. union switch invalid variant name */
    err("struct A { u32 x; }\nstruct B { u32 y; }\n"
        "union U { A a; B b; }\n"
        "void f() { U u; u.a.x = 1; switch (u) { .a => |v| { } .typo => |v| { } } }",
        "union switch invalid variant name rejected");

    /* 26. const field mutation on value capture */
    err("struct Pt { u32 x; u32 y; }\n"
        "?Pt make() { Pt p; p.x = 1; p.y = 2; return p; }\n"
        "void f() { ?Pt opt = make(); if (opt) |pt| { pt.x = 99; } }",
        "const capture field mutation rejected");

    /* BUG-194: sticky is_local_derived — reassign to safe value must clear flag */
    err("u32 g = 99;\n"
        "*u32 test() { u32 x = 42; *u32 p = &x; return p; }",
        "return local-derived pointer rejected");
    ok("u32 g = 99;\n"
       "*u32 test() { u32 x = 42; *u32 p = &x; p = &g; return p; }\n"
       "u32 main() { return 0; }",
       "reassign local-derived to global clears flag");

    /* BUG-194: is_local_derived must be SET during assignment, not just var-decl */
    err("u32 g = 99;\n"
        "*u32 test() { *u32 p = &g; u32 x = 42; p = &x; return p; }",
        "assign &local sets local-derived flag");

    /* BUG-224: void struct field / union variant rejected */
    err("struct S { void x; }\nu32 main() { return 0; }",
        "void struct field rejected");
    err("union U { void a; u32 b; }\nu32 main() { return 0; }",
        "void union variant rejected");

    /* BUG-225: Pool/Ring/Arena assignment rejected */
    err("static Pool(u32, 4) p;\nstatic Pool(u32, 4) q;\nu32 main() { p = q; return 0; }",
        "Pool assignment rejected");

    /* BUG-226: float switch rejected */
    err("u32 main() { f32 x = 1.0; switch (x) { default => { return 1; } } }",
        "float switch rejected");

    /* BUG-227/232: recursive struct by value rejected (including via array) */
    err("struct S { S next; }\nu32 main() { return 0; }",
        "recursive struct by value rejected");
    err("struct S { S[1] next; }\nu32 main() { return 0; }",
        "recursive struct via array rejected");

    /* BUG-228: const address-of leak — &const_var yields const pointer */
    err("const u32 x = 42;\nu32 main() { *u32 p = &x; return *p; }",
        "mutable pointer from &const rejected");

    /* BUG-230: pointer parameter escape — &local through pointer param field */
    err("struct H { *u32 p; }\nvoid leak(*H h) { u32 x = 5; h.p = &x; }",
        "local escape through pointer param rejected");

    /* BUG-231: @size(opaque) and @size(void) rejected */
    err("u32 main() { usize s = @size(opaque); return 0; }",
        "@size(opaque) rejected");
    err("u32 main() { usize s = @size(void); return 0; }",
        "@size(void) rejected");

    /* BUG-234: @cstr compile-time overflow check */
    err("u32 main() { u8[4] buf; @cstr(buf, \"hello world\"); return 0; }",
        "@cstr constant overflow rejected");

    /* BUG-236: const builtin mutating methods rejected */
    err("struct Task { u32 id; }\nconst Pool(Task, 4) tasks;\n"
        "u32 main() { Handle(Task) h = tasks.alloc() orelse return; return 0; }",
        "const Pool alloc rejected");

    /* BUG-237: nested array return as slice (struct field) */
    err("struct S { u8[10] arr; }\n[]u8 bad() { S s; return s.arr; }\n"
        "u32 main() { return 0; }",
        "nested array return escape rejected");

    /* BUG-238: @cstr to const destination */
    err("u32 main() { const u8[16] buf; @cstr(buf, \"hello\"); return 0; }",
        "@cstr to const array rejected");

    /* BUG-239: non-null pointer requires initializer */
    err("u32 main() { *u32 p; return 0; }",
        "non-null pointer without init rejected");

    /* BUG-240: nested array assign escape to global */
    err("struct S { u8[10] arr; }\nstatic []u8 global_s;\n"
        "void bad() { S s; global_s = s.arr; }\nu32 main() { return 0; }",
        "nested array assign to global rejected");

    /* BUG-241: @cstr to const pointer */
    err("void bad(const *u8 p) { @cstr(p, \"hi\"); }\nu32 main() { return 0; }",
        "@cstr to const pointer rejected");

    /* BUG-244: double-pointer union lock bypass */
    err("union Msg { u32 a; u32 b; }\n"
        "void bad(**Msg pp) {\n"
        "    switch (**pp) {\n"
        "        .a => |*v| { (*pp).b = 20; }\n"
        "        .b => { }\n"
        "    }\n"
        "}\nu32 main() { return 0; }",
        "double-ptr union mutation blocked");

    /* BUG-245: const array → mutable slice assignment */
    err("u32 main() { const u32[4] arr; []u32 s; s = arr; return 0; }",
        "const array to mutable slice assign rejected");

    /* BUG-246: @ptrcast loses local-derived */
    err("*u8 bad() { u32 x = 42; return @ptrcast(*u8, &x); }\n"
        "u32 main() { return 0; }",
        "@ptrcast(&local) return rejected");

    /* BUG-247: array size overflow */
    err("u32 main() { u8[1 << 33] arr; return 0; }",
        "array size > 4GB rejected");

    /* BUG-221: keep parameter rejects local-derived pointers */
    err("static Pool(u32, 4) pool;\n"
        "void store(keep *u32 p) { pool.alloc(); }\n"
        "void bad() { u32 x = 0; *u32 p = &x; store(p); }",
        "local-derived ptr to keep param rejected");

    /* BUG-217: compile-time slice bounds check */
    err("u32 main() { u8[10] a; []u8 s = a[0..15]; return 0; }",
        "slice end 15 exceeds array size 10");
    ok("u32 main() { u8[10] a; a[0] = 1; []u8 s = a[0..10]; return 0; }",
       "slice end 10 on array[10] accepted (end is exclusive)");

    /* BUG-213: static vars visible to own functions */
    ok("static u32 count = 0;\n"
       "void inc() { count += 1; }\n"
       "u32 main() { inc(); return count; }",
       "static variable visible to module functions");

    /* BUG-214: slice-to-slice propagates local-derived */
    err("[]u8 bad() { u8[10] a; []u8 s = a; []u8 s2 = s[0..2]; return s2; }",
        "sub-slice of local-derived slice blocked");

    /* BUG-211: union field bypass — lock walks to root */
    err("struct A { u32 x; }\nstruct B { u32 y; }\nunion M { A a; B b; }\nstruct S { M msg; }\n"
        "void f(S s) { switch (s.msg) { .a => |*v| { s.msg.b.y = 20; } .b => |*v| { } } }",
        "field-based union mutation blocked");

    /* BUG-212: if-unwrap capture propagates local-derived */
    err("*u32 bad() { u32 x = 42; ?*u32 opt = &x; if (opt) |p| { return p; } return @inttoptr(*u32, 1); }",
        "if-unwrap capture inherits local-derived");

    /* BUG-207: sub-slice from local array escape */
    err("[]u8 bad() { u8[10] a; []u8 s = a[1..4]; return s; }",
        "sub-slice from local array blocked");

    /* BUG-208: union alias via &union_var blocked in switch capture */
    err("struct A { u32 x; }\nstruct B { u32 y; }\nunion M { A a; B b; }\n"
        "void f() { M m; m.a.x = 1; switch (m) { .a => |*v| { *M p = &m; } .b => |*v| { } } }",
        "address-of union in switch arm rejected");

    /* BUG-204: orelse break in while(true) */
    err("?u32 mg() { return 5; }\n"
        "u32 bad() { while (true) { u32 x = mg() orelse break; return x; } }",
        "orelse break in while(true) rejected");

    /* BUG-205: local-derived escape via assignment to global */
    err("*u32 gp;\n"
        "void bad() { u32 x = 42; *u32 p = &x; gp = p; }",
        "local-derived assigned to global rejected");

    /* BUG-206: orelse loses local-derived */
    err("*u32 bad() { u32 x = 42; ?*u32 m = &x; *u32 p = m orelse return; return p; }",
        "orelse unwrap preserves local-derived");

    /* BUG-202: orelse &local propagates is_local_derived */
    err("*u32 bad() { u32 x = 42; ?*u32 m = null; *u32 p = m orelse &x; return p; }",
        "orelse &local marks local-derived");
    ok("u32 g = 99;\n"
       "*u32 ok() { ?*u32 m = null; *u32 p = m orelse &g; return p; }\n"
       "u32 main() { return *ok(); }",
       "orelse &global is safe");

    /* BUG-203: slice from local array marks local-derived */
    err("[]u8 bad() { u8[10] a; []u8 s = a; return s; }",
        "slice from local array blocked on return");
    ok("u8[10] g;\n"
       "[]u8 ok() { []u8 s = g; return s; }\n"
       "u32 main() { return 0; }",
       "slice from global array is safe");

    /* BUG-200: while(true)+break is NOT a terminator */
    err("u32 bad(bool c) { while (true) { if (c) { break; } return 1; } }",
        "while(true) with break rejected");
    ok("u32 good() { while (true) { return 1; } }\n"
       "u32 main() { return good(); }",
       "while(true) without break still accepted");

    /* BUG-201: type_width unwraps distinct */
    ok("distinct typedef u32 Meters;\n"
       "u8[@size(Meters)] buf;\n"
       "u32 main() { return 0; }",
       "@size(distinct u32) = 4 accepted as array size");

    /* BUG-198: duplicate enum variant names */
    err("enum Color { red, green, red }\nu32 main() { return 0; }",
        "duplicate enum variant rejected");
    ok("enum Color { red, green, blue }\nu32 main() { return 0; }",
       "distinct enum variants accepted");

    /* BUG-199: @size(T) in array size */
    ok("struct Task { u32 id; u32 priority; }\n"
       "u8[@size(Task)] buffer;\n"
       "u32 main() { return 0; }",
       "@size(T) accepted as array size constant");

    /* BUG-196: compile-time OOB for constant array index */
    err("u32 main() { u8[10] a; a[10] = 1; return 0; }",
        "constant index 10 on array[10] rejected");
    err("u32 main() { u8[4] a; a[100] = 1; return 0; }",
        "constant index 100 on array[4] rejected");
    ok("u32 main() { u8[10] a; a[9] = 1; return 0; }",
       "constant index 9 on array[10] accepted");

    /* BUG-197: volatile pointer decay — &volatile_var to non-volatile ptr */
    err("volatile u32 x = 0;\n"
        "void f() { *u32 p = &x; }",
        "non-volatile ptr from volatile rejected");
    ok("volatile u32 x = 0;\n"
       "u32 main() { volatile *u32 p = &x; return 0; }",
       "volatile ptr from volatile accepted");

    /* BUG-195: while(true) is a terminator for all_paths_return */
    ok("u32 loop_ret() { while (true) { return 1; } }\n"
       "u32 main() { return loop_ret(); }",
       "while(true) with return accepted");
    ok("u32 loop_if() { while (true) { u32 x = 1; if (x > 0) { return 1; } } }\n"
       "u32 main() { return loop_if(); }",
       "while(true) with conditional return accepted");

    /* BUG-248: union assignment during switch capture */
    err("struct A { u32 x; }\nstruct B { u32 y; }\nunion Msg { A a; B b; }\n"
        "void f() { Msg m; m.a.x = 1; Msg other; other.b.y = 99;\n"
        "  switch (m) { .a => |*v| { m = other; } .b => |*v| { } } }",
        "direct union assign inside capture arm rejected");
    ok("struct A { u32 x; }\nstruct B { u32 y; }\nunion Msg { A a; B b; }\n"
       "void f() { Msg m; m.a.x = 1; Msg other; other.b.y = 99;\n"
       "  switch (m) { .a => |*v| { v.x = 5; } .b => |*v| { } } }",
       "capture field mutation (not union itself) accepted");

    /* BUG-249: switch capture propagates is_local_derived/is_arena_derived */
    err("*u32 bad() {\n"
        "    u32 x = 42;\n"
        "    ?*u32 opt = &x;\n"
        "    switch (opt) {\n"
        "        null => { return @inttoptr(*u32, 1); }\n"
        "        default => |p| { return p; }\n"
        "    }\n"
        "}",
        "switch capture of local-derived optional rejected on return");

    /* BUG-250: @size(union) resolved as compile-time constant */
    ok("struct A { u32 x; }\nstruct B { u64 y; }\nunion Msg { A a; B b; }\n"
       "u8[@size(Msg)] buffer;\n"
       "u32 main() { return 0; }",
       "@size(union) accepted as array size constant");

    /* BUG-251: return opt orelse local_derived — orelse walk bypass */
    err("*u32 bad(?*u32 opt) {\n"
        "    u32 x = 42;\n"
        "    *u32 p = &x;\n"
        "    return opt orelse p;\n"
        "}",
        "return orelse local-derived fallback rejected");
    err("struct Task { u32 id; }\n"
        "*Task bad(?*Task opt) {\n"
        "    u8[64] buf;\n"
        "    Arena a = Arena.over(buf);\n"
        "    *Task t = a.alloc(Task) orelse return @inttoptr(*Task, 1);\n"
        "    return opt orelse t;\n"
        "}",
        "return orelse arena-derived fallback rejected");
    ok("u32 g = 99;\n"
       "*u32 ok_fn(?*u32 opt) { *u32 p = &g; return opt orelse p; }\n"
       "u32 main() { return 0; }",
       "return orelse global-derived fallback accepted");

    /* BUG-253: global non-null pointer requires initializer */
    err("*u32 g_ptr;\nu32 main() { return 0; }",
        "global *T without init rejected");
    ok("u32 val = 0;\n*u32 g_ptr = &val;\nu32 main() { return 0; }",
       "global *T with init accepted");
    ok("?*u32 g_ptr;\nu32 main() { return 0; }",
       "global ?*T without init accepted (nullable)");

    /* BUG-254: const leak via &arr[i] and &s.field */
    err("const u32[4] arr;\n"
        "void f() { *u32 p = &arr[0]; }",
        "&const_arr[idx] yields const ptr — mutable rejected");
    err("struct S { u32 x; }\n"
        "const S s;\n"
        "void f() { *u32 p = &s.x; }",
        "&const_struct.field yields const ptr — mutable rejected");
    ok("const u32[4] arr;\n"
       "u32 main() { const *u32 p = &arr[0]; return 0; }",
       "&const_arr[idx] to const ptr accepted");

    /* BUG-256: @ptrcast local/arena-derived ident bypass */
    err("*u8 bad() {\n"
        "    u32 x = 42;\n"
        "    *u32 p = &x;\n"
        "    return @ptrcast(*u8, p);\n"
        "}",
        "@ptrcast local-derived ident return rejected");
    ok("u32 g = 99;\n"
       "*u8 ok_fn() { *u32 p = &g; return @ptrcast(*u8, p); }\n"
       "u32 main() { return 0; }",
       "@ptrcast global-derived ident return accepted");

    /* BUG-258: volatile stripping via @ptrcast */
    err("volatile u32 hw = 0;\n"
        "void f() {\n"
        "    volatile *u32 reg = &hw;\n"
        "    *u32 p = @ptrcast(*u32, reg);\n"
        "}",
        "@ptrcast volatile to non-volatile rejected");
    ok("volatile u32 hw = 0;\n"
       "u32 main() {\n"
       "    volatile *u32 reg = &hw;\n"
       "    volatile *u8 p = @ptrcast(volatile *u8, reg);\n"
       "    return 0;\n"
       "}",
       "@ptrcast volatile to volatile accepted");

    /* BUG-259: @cstr return of local buffer */
    err("*u8 bad() {\n"
        "    u8[10] buf;\n"
        "    return @cstr(buf, \"hi\");\n"
        "}",
        "return @cstr(local_buf) rejected — dangling pointer");
    ok("u8[10] g_buf;\n"
       "*u8 ok_fn() { return @cstr(g_buf, \"hi\"); }\n"
       "u32 main() { return 0; }",
       "return @cstr(global_buf) accepted");

    /* BUG-260: local-derived escape through dereferenced function call */
    err("static Pool(*u32, 4) ptr_pool;\n"
        "void bad() {\n"
        "    u32 x = 42;\n"
        "    Handle(*u32) h = ptr_pool.alloc() orelse return;\n"
        "    *ptr_pool.get(h) = &x;\n"
        "}",
        "store &local through *pool.get() rejected");

    /* BUG-261: union alias bypass via pointer of same type */
    err("struct A { u32 x; }\nstruct B { u32 y; }\nunion Msg { A a; B b; }\n"
        "Msg g_msg;\n"
        "void exploit(*Msg alias) {\n"
        "    switch (g_msg) {\n"
        "        .a => |*ptr| {\n"
        "            alias.b.y = 99;\n"
        "        }\n"
        "        .b => |*v| { }\n"
        "    }\n"
        "}",
        "union alias mutation via same-type pointer rejected");
    ok("struct A { u32 x; }\nstruct B { u32 y; }\nunion Msg { A a; B b; }\n"
       "struct Other { u32 z; }\n"
       "Msg g_msg;\n"
       "void safe(*Other p) {\n"
       "    switch (g_msg) {\n"
       "        .a => |*ptr| {\n"
       "            p.z = 99;\n"
       "        }\n"
       "        .b => |*v| { }\n"
       "    }\n"
       "}\nu32 main() { return 0; }",
       "different-type pointer mutation in switch arm accepted");

    /* BUG-263: volatile pointer to non-volatile param rejected */
    err("void write_reg(*u32 p) { *p = 5; }\n"
        "volatile u32 hw = 0;\n"
        "void f() {\n"
        "    volatile *u32 reg = &hw;\n"
        "    write_reg(reg);\n"
        "}",
        "volatile *u32 to *u32 param rejected");
    ok("void write_reg(volatile *u32 p) { *p = 5; }\n"
       "volatile u32 hw = 0;\n"
       "u32 main() {\n"
       "    volatile *u32 reg = &hw;\n"
       "    write_reg(reg);\n"
       "    return 0;\n"
       "}",
       "volatile *u32 to volatile *u32 param accepted");

    /* usize 64-bit gap closure */
    ok("u32 main() { u32 x = 42; usize len = x; return @truncate(u32, len); }",
       "u32 to usize widening accepted");
    ok("u32 main() { usize x = 42; u32 y = @truncate(u32, x); return y; }",
       "@truncate(u32, usize) accepted");
    /* On 32-bit target (default), usize == u32 in width, so coercion is allowed */
    ok("u32 main() { usize x = 42; u32 y = x; return y; }",
       "usize to u32 same-width coercion on 32-bit target");

    /* 64-bit target: usize→u32 is narrowing, must be rejected */
    {
        int saved = zer_target_ptr_bits;
        zer_target_ptr_bits = 64;
        err("u32 main() { usize x = 42; u32 y = x; return y; }",
            "64-bit: usize to u32 narrowing rejected");
        ok("u32 main() { u32 x = 42; usize len = x; return @truncate(u32, len); }",
           "64-bit: u32 to usize widening accepted");
        ok("u32 main() { u64 x = 42; usize len = x; return @truncate(u32, len); }",
           "64-bit: u64 to usize same-width accepted");
        zer_target_ptr_bits = saved;
    }

    /* BUG-373: integer literal range check uses target width, not host */
    {
        int saved = zer_target_ptr_bits;
        zer_target_ptr_bits = 32;
        err("u32 main() { usize x = 5000000000; return 0; }",
            "BUG-373: 5B literal rejected on 32-bit usize");
        ok("u32 main() { usize x = 4294967295; return 0; }",
           "BUG-373: u32 max literal accepted on 32-bit usize");
        zer_target_ptr_bits = 64;
        ok("u32 main() { usize x = 5000000000; return @truncate(u32, x); }",
           "BUG-373: 5B literal accepted on 64-bit usize");
        zer_target_ptr_bits = saved;
    }

    /* BUG-374: nested identity washing via nested calls */
    err("*u32 identity(*u32 p) { return p; }\n"
        "*u32 leak() { u32 x = 5; return identity(identity(&x)); }\n"
        "u32 main() { return 0; }",
        "BUG-374: nested identity(identity(&x)) return caught");
    ok("u32 g_val = 1;\n"
       "*u32 identity(*u32 p) { return p; }\n"
       "*u32 safe() { return identity(&g_val); }\n"
       "u32 main() { return 0; }",
       "BUG-374: identity(&global) return allowed");

    /* BUG-377: local array escape via orelse fallback */
    err("[]u8 g_slice;\n"
        "void leak(?[]u8 opt) {\n"
        "    u8[10] local_buf;\n"
        "    g_slice = opt orelse local_buf;\n"
        "}\n"
        "u32 main() { return 0; }",
        "BUG-377: orelse local array assign to global caught");
    err("[]u8 g_slice;\n"
        "void leak(?[]u8 opt) {\n"
        "    u8[10] local_buf;\n"
        "    []u8 s = opt orelse local_buf;\n"
        "    g_slice = s;\n"
        "}\n"
        "u32 main() { return 0; }",
        "BUG-377: orelse local array var_decl then assign to global caught");

    /* BUG-375: missing target type validation for pointer intrinsics */
    err("mmio 0x0..0xFFFFFFFFFFFFFFFF;\n"
        "u32 main() { u32 x = @inttoptr(u32, 0x1000); return 0; }",
        "BUG-375: @inttoptr non-pointer target rejected");
    err("u32 main() { u32 x = 5; *u32 p = &x; u32 y = @ptrcast(u32, p); return 0; }",
        "BUG-375: @ptrcast non-pointer target rejected");
    err("struct Node { u32 data; u32 next; }\n"
        "u32 main() { Node n; *Node p = @container(*Node, n.data, data); return 0; }",
        "BUG-375: @container non-pointer source rejected");

    /* BUG-381: @container volatile stripping */
    err("struct Device { u32 status; u32 list; }\n"
        "mmio 0x0..0xFFFFFFFFFFFFFFFF;\n"
        "u32 main() {\n"
        "    volatile *u32 ptr = @inttoptr(*u32, 0x4000);\n"
        "    *Device d = @container(*Device, ptr, list);\n"
        "    return 0;\n"
        "}",
        "BUG-381: @container volatile stripping rejected");
    ok("struct Device { u32 status; u32 list; }\n"
       "mmio 0x0..0xFFFFFFFFFFFFFFFF;\n"
       "u32 main() {\n"
       "    volatile *u32 ptr = @inttoptr(*u32, 0x4000);\n"
       "    volatile *Device d = @container(volatile *Device, ptr, list);\n"
       "    return 0;\n"
       "}",
       "BUG-381: @container volatile preserved accepted");

    /* BUG-391: comptime function call as array size */
    ok("comptime u32 BIT(u32 n) { return 1 << n; }\n"
       "u32 main() { u8[BIT(3)] buf; buf[0] = 1; return @truncate(u32, buf[0]); }",
       "BUG-391: comptime call as array size");
    ok("comptime u32 SLOTS(u32 n) { return n * 4; }\n"
       "u32 main() { u32[SLOTS(2)] arr; arr[0] = 5; return arr[0]; }",
       "BUG-391: comptime with arithmetic as array size");

    /* BUG-392: union array lock — different elements should be independent */
    ok("union Msg { u32 data; u32 cmd; }\n"
       "Msg[2] msgs;\n"
       "void test() {\n"
       "    msgs[0].data = 5;\n"
       "    switch (msgs[0]) {\n"
       "        .data => |*v| { msgs[1].data = 20; }\n"
       "        .cmd => {}\n"
       "    }\n"
       "}\n"
       "u32 main() { test(); return 0; }",
       "BUG-392: switch(msgs[0]) allows msgs[1] mutation");
    err("union Msg { u32 data; u32 cmd; }\n"
        "Msg[2] msgs;\n"
        "void test() {\n"
        "    msgs[0].data = 5;\n"
        "    switch (msgs[0]) {\n"
        "        .data => |*v| { msgs[0].cmd = 99; }\n"
        "        .cmd => {}\n"
        "    }\n"
        "}\n"
        "u32 main() { test(); return 0; }",
        "BUG-392: switch(msgs[0]) still blocks msgs[0] mutation");

    /* BUG-389: eval_const_expr depth limit — deep nesting still works up to limit */
    ok("u32 main() { u8[((1+1)+(1+1))+((1+1)+(1+1))] buf; return 0; }",
       "BUG-389: nested constant expr accepted (within depth)");
    ok("u32 main() { u32 x = 1+2+3+4+5+6+7+8+9+10; return x; }",
       "BUG-389: chained additions accepted");

    /* BUG-390: Handle(T) is u64 — Pool/Slab alloc returns ?Handle (opt u64) */
    ok("struct T { u32 x; }\n"
       "Pool(T, 4) pool;\n"
       "void run() {\n"
       "    Handle(T) h = pool.alloc() orelse return;\n"
       "    pool.get(h).x = 42;\n"
       "    pool.free(h);\n"
       "}\n"
       "u32 main() { run(); return 0; }",
       "BUG-390: u64 Handle pool lifecycle accepted");

    /* BUG-386: Pool/Ring/Slab in union rejected */
    err("union Oops { Pool(u32, 4) p; u32 other; }\n"
        "u32 main() { return 0; }",
        "BUG-386: Pool in union rejected");
    err("union Oops { Ring(u32, 8) r; u32 other; }\n"
        "u32 main() { return 0; }",
        "BUG-386: Ring in union rejected");

    /* BUG-387: orelse keep fallback local-derived check */
    err("void reg(keep *u32 p) {}\n"
        "u32 main() {\n"
        "    u32 x = 5;\n"
        "    *u32 local_ptr = &x;\n"
        "    ?*u32 opt;\n"
        "    reg(opt orelse local_ptr);\n"
        "    return 0;\n"
        "}",
        "BUG-387: orelse keep fallback local-derived rejected");

    /* BUG-383: struct wrapper escape via field extraction */
    err("struct Wrap { *u32 p; }\n"
        "Wrap wrap(*u32 p) { Wrap w; w.p = p; return w; }\n"
        "*u32 leak() {\n"
        "    u32 x = 5;\n"
        "    return wrap(&x).p;\n"
        "}\n"
        "u32 main() { return 0; }",
        "BUG-383: return wrap(&x).p caught");
    ok("u32 g = 1;\n"
       "struct Wrap { *u32 p; }\n"
       "Wrap wrap(*u32 p) { Wrap w; w.p = p; return w; }\n"
       "*u32 safe() {\n"
       "    return wrap(&g).p;\n"
       "}\n"
       "u32 main() { return 0; }",
       "BUG-383: return wrap(&global).p allowed");

    /* BUG-310: volatile slice qualifier */
    err("volatile u8[16] hw_regs;\n"
        "void poll([]u8 regs) { while (regs[0] == 0) { } }\n"
        "u32 main() { poll(hw_regs); return 0; }",
        "volatile array to non-volatile slice param rejected");
    err("volatile u8[16] hw_regs;\n"
        "u32 main() { []u8 s = hw_regs; return 0; }",
        "volatile array to non-volatile slice var-decl rejected");
    err("volatile u8[16] hw_regs;\n"
        "[]u8 g_s;\n"
        "u32 main() { g_s = hw_regs; return 0; }",
        "volatile array to non-volatile slice assign rejected");
    ok("u8[16] buf;\n"
       "void process([]u8 data) { }\n"
       "u32 main() { process(buf); return 0; }",
       "non-volatile array to slice accepted");
    /* BUG-314: orelse &local escape to global */
    err("?*u32 g_ptr;\n"
        "void f(?*u32 opt) {\n"
        "    u32 x = 5;\n"
        "    g_ptr = opt orelse &x;\n"
        "}",
        "orelse &local escape to global rejected");
    ok("?*u32 g_ptr;\n"
       "u32 g_val = 42;\n"
       "void f(?*u32 opt) {\n"
       "    g_ptr = opt orelse &g_val;\n"
       "}",
       "orelse &global to global accepted");

    /* BUG-317: return orelse @ptrcast(&local) */
    err("*u8 leak(?*u8 opt) {\n"
        "    u8 x = 42;\n"
        "    return opt orelse @ptrcast(*u8, &x);\n"
        "}\n",
        "return orelse @ptrcast(&local) rejected");

    /* BUG-318: orelse fallback flag propagation */
    err("?*u32 g_ptr;\n"
        "void f(?*u32 opt) {\n"
        "    u32 x = 5;\n"
        "    *u32 p = &x;\n"
        "    *u32 q = opt orelse p;\n"
        "    g_ptr = q;\n"
        "}\n",
        "orelse alias local-derived escape rejected");

    /* BUG-320: @size(distinct void) */
    err("distinct typedef void MyVoid;\n"
        "u32 main() { return @truncate(u32, @size(MyVoid)); }",
        "@size(distinct void) rejected");

    /* BUG-315: distinct slice comparison */
    err("distinct typedef []u8 Buffer;\n"
        "bool f(Buffer a, Buffer b) {\n"
        "    return a == b;\n"
        "}",
        "distinct slice comparison rejected");

    /* volatile []T — proper propagation */
    ok("volatile u8[16] hw_regs;\n"
       "void poll(volatile []u8 regs) { }\n"
       "u32 main() { poll(hw_regs); return 0; }",
       "volatile array to volatile slice param accepted");
    ok("volatile u8[16] hw_regs;\n"
       "u32 main() { volatile []u8 s = hw_regs; return 0; }",
       "volatile array to volatile slice var-decl accepted");

    /* Slab(T) type checking */
    ok("struct Item { u32 val; }\nstatic Slab(Item) items;\n"
       "u32 main() {\n"
       "    ?Handle(Item) m = items.alloc();\n"
       "    Handle(Item) h = m orelse return;\n"
       "    items.get(h).val = 42;\n"
       "    u32 r = items.get(h).val;\n"
       "    items.free(h);\n"
       "    return r;\n"
       "}",
       "Slab alloc/get/free accepted");
    err("struct Item { u32 val; }\n"
        "u32 main() { Slab(Item) s; return 0; }",
        "Slab on stack rejected");
    err("struct Item { u32 val; }\nstatic Slab(Item) items;\n"
        "void f() { items.alloc(Item); }",
        "slab.alloc() with args rejected");

    /* BUG-313: string literal to const []u8 param should work */
    ok("void f(const []u8 s) { }\n"
       "u32 main() { f(\"hello\"); return 0; }",
       "string literal to const []u8 param accepted");
    err("void f([]u8 s) { }\n"
        "u32 main() { f(\"hello\"); return 0; }",
        "string literal to mutable []u8 param rejected");

    /* BUG-304: @ptrcast const stripping */
    err("const u32 val = 42;\n"
        "void f() { *u32 p = @ptrcast(*u32, &val); }",
        "@ptrcast strips const rejected");
    ok("const u32 val = 42;\n"
       "u32 main() { const *u32 p = @ptrcast(const *u32, &val); return 0; }",
       "@ptrcast preserves const accepted");

    /* BUG-305: const capture via |*v| */
    err("const ?u32 val = 42;\n"
        "void f() {\n"
        "    if (val) |*v| { *v = 99; }\n"
        "}",
        "|*v| on const source — write through const ptr rejected");

    /* BUG-295: nested distinct arithmetic */
    ok("distinct typedef u32 P1;\ndistinct typedef P1 P2;\n"
       "u32 main() { P2 x = @cast(P2, @cast(P1, 5)); return @cast(u32, @cast(P1, x)); }",
       "nested distinct arithmetic accepted");

    /* BUG-294: assign to non-lvalue */
    err("u32 get() { return 0; }\nvoid f() { get() = 5; }",
        "assign to function call rejected");
    err("struct S { u32 x; }\nS get_s() { S s; s.x = 0; return s; }\n"
        "void f() { get_s().x = 5; }",
        "assign to rvalue struct field rejected (BUG-302)");
    ok("u32 x = 0;\nvoid f() { x = 5; }\nu32 main() { return 0; }",
       "assign to variable accepted");

    /* BUG-290: local escape via *param = &local */
    err("void leak(**u32 p) {\n"
        "    u32 x = 5;\n"
        "    *p = &x;\n"
        "}",
        "store &local through *param rejected");
    ok("u32 g = 99;\n"
       "void safe(**u32 p) {\n"
       "    *p = &g;\n"
       "}\nu32 main() { return 0; }",
       "store &global through *param accepted");

    /* BUG-287: Pool/Ring as struct fields rejected */
    err("struct M { Pool(u32, 4) tasks; }\nu32 main() { return 0; }",
        "Pool as struct field rejected");
    err("struct M { Ring(u32, 8) buf; }\nu32 main() { return 0; }",
        "Ring as struct field rejected");

    /* BUG-288: bit extraction hi < lo rejected */
    err("u32 main() { u32 r = 0xFF; u32 x = r[0..7]; return x; }",
        "bit extract hi < lo rejected");
    ok("u32 main() { u32 r = 0xFF; u32 x = r[7..0]; return x; }",
       "bit extract hi >= lo accepted");

    /* BUG-281: volatile stripping on return */
    err("*u32 wash(volatile *u32 p) {\n"
        "    return p;\n"
        "}\nu32 main() { return 0; }",
        "return volatile ptr as non-volatile rejected");
    /* Note: volatile on function return types is not supported syntax.
     * The fix prevents returning volatile as non-volatile. */

    /* BUG-282: volatile stripping on init/assign */
    err("volatile u32 hw = 0;\n"
        "void f() {\n"
        "    volatile *u32 vp = &hw;\n"
        "    *u32 p = vp;\n"
        "}",
        "init non-volatile ptr from volatile rejected");
    err("volatile u32 hw = 0;\n"
        "u32 dummy = 0;\n"
        "*u32 p = &dummy;\n"
        "void f() {\n"
        "    volatile *u32 vp = &hw;\n"
        "    p = vp;\n"
        "}",
        "assign volatile ptr to non-volatile rejected");

    /* BUG-279: nested distinct null-sentinel */
    ok("distinct typedef *u32 Ptr1;\n"
       "distinct typedef Ptr1 Ptr2;\n"
       "u32 main() {\n"
       "    ?Ptr2 maybe = null;\n"
       "    if (maybe == null) { return 1; }\n"
       "    return 0;\n"
       "}",
       "nested distinct ?Ptr2 uses null-sentinel (not struct)");

    /* BUG-280: @size(usize) target-portable */
    ok("u8[@size(usize)] buf;\nu32 main() { return 0; }",
       "@size(usize) as array size accepted (sizeof-based)");

    /* BUG-277: keep in function pointer types */
    err("void store(keep *u32 p) { }\n"
        "u32 main() {\n"
        "    void (*fn)(*u32) = store;\n"
        "    return 0;\n"
        "}",
        "keep func to non-keep func ptr rejected (type mismatch)");
    err("void store(keep *u32 p) { }\n"
        "u32 main() {\n"
        "    u32 x = 5;\n"
        "    void (*fn)(keep *u32) = store;\n"
        "    fn(&x);\n"
        "    return 0;\n"
        "}",
        "keep func ptr call with local arg rejected");
    ok("u32 g = 99;\n"
       "void store(keep *u32 p) { }\n"
       "u32 main() {\n"
       "    void (*fn)(keep *u32) = store;\n"
       "    fn(&g);\n"
       "    return 0;\n"
       "}",
       "keep func ptr call with global arg accepted");

    /* BUG-275: @size on pointer/slice emits sizeof (target-portable) */
    ok("u8[@size(*u32)] buf;\nu32 main() { return 0; }",
       "@size(*u32) as array size accepted (sizeof-based)");
    ok("u8[@size([]u8)] buf;\nu32 main() { return 0; }",
       "@size([]u8) as array size accepted (sizeof-based)");

    /* BUG-276: _zer_ prefix reserved */
    err("u32 main() { u32 _zer_foo = 5; return _zer_foo; }",
        "_zer_ prefixed variable rejected");
    ok("u32 main() { u32 zer_foo = 5; return zer_foo; }",
       "zer_ prefixed variable accepted (no underscore prefix)");

    /* BUG-269: constant expression div-by-zero */
    err("u32 main() { u32 x = 10 / (2 - 2); return x; }",
        "const expr div-by-zero (2-2=0) rejected");
    ok("u32 main() { u32 x = 10 / (3 - 1); return x; }",
       "const expr div (3-1=2) accepted");

    /* BUG-270: array return type rejected */
    err("u8[10] get_buf() { u8[10] b; return b; }\nu32 main() { return 0; }",
        "array return type rejected");

    /* BUG-265: recursive union by value rejected */
    err("struct A { u32 x; }\nunion U { A a; U recursive; }\nu32 main() { return 0; }",
        "recursive union by value rejected");
    ok("struct A { u32 x; }\nunion U { A a; *U recursive; }\nu32 main() { return 0; }",
       "recursive union via pointer accepted");

    /* RF8: eval_const_expr with negative intermediates */
    ok("u8[10 - 5] arr;\nu32 main() { arr[0] = 1; return 0; }",
       "array size from subtraction (10-5=5) accepted");
    ok("u8[3 * 4 - 2] arr;\nu32 main() { return 0; }",
       "array size from mixed ops (3*4-2=10) accepted");
    err("u8[5 - 10] arr;\nu32 main() { return 0; }",
        "negative array size (5-10=-5) rejected");
    err("u8[0 - 1] arr;\nu32 main() { return 0; }",
        "negative array size (0-1=-1) rejected");
}

/* ================================================================ */

/* ================================================================
 * RED TEAM AUDIT: BUG-314 through BUG-318
 * ================================================================ */
static void test_red_team_314_318(void) {
    printf("[Red Team audit: BUG-314 through BUG-318]\n");

    /* BUG-314: recursive struct via distinct typedef */
    err("distinct typedef S SafeS;\n"
        "struct S { SafeS next; }",
        "BUG-314: recursive struct via distinct");

    /* BUG-314: recursive union via distinct typedef */
    err("distinct typedef U SafeU;\n"
        "union U { SafeU a; u32 b; }",
        "BUG-314: recursive union via distinct");

    /* BUG-314 positive: distinct of DIFFERENT struct is fine */
    ok("struct A { u32 x; }\n"
       "distinct typedef A B;\n"
       "struct C { B val; }",
       "BUG-314: distinct of different struct OK");

    /* BUG-315: array return via distinct typedef */
    err("distinct typedef u8[10] Buffer;\n"
        "Buffer get_data() { Buffer b; return b; }",
        "BUG-315: array return via distinct");

    /* BUG-315 positive: distinct of non-array is fine to return */
    ok("distinct typedef u32 Meters;\n"
       "Meters get_m() { return @cast(Meters, 42); }",
       "BUG-315: distinct non-array return OK");

    /* BUG-316: @bitcast with named distinct type */
    ok("distinct typedef u32 Meters;\n"
       "void f() { u32 x = 5; Meters m = @bitcast(Meters, x); }",
       "BUG-316: @bitcast with distinct type");

    /* BUG-316: @truncate with named distinct type */
    ok("distinct typedef u16 Short;\n"
       "void f() { u32 x = 5; Short s = @truncate(Short, x); }",
       "BUG-316: @truncate with distinct type");

    /* BUG-316: @saturate with named distinct type */
    ok("distinct typedef u8 Byte;\n"
       "void f() { u32 x = 500; Byte b = @saturate(Byte, x); }",
       "BUG-316: @saturate with distinct type");

    /* BUG-318: large shift doesn't crash the compiler */
    ok("void f() { u64 x = 1; u64 y = x << 62; }",
       "BUG-318: large shift no compiler UB");

    /* BUG-318: constant expression with large shift */
    ok("u8[1 << 10] buf;\nvoid f() { buf[0] = 1; }",
       "BUG-318: const expr large shift");

    /* BUG-334: keep bypass via local array coercion */
    err("static ?[]u8 g = null;\n"
        "void reg(keep []u8 d) { g = d; }\n"
        "void f() { u8[16] buf; reg(buf); }",
        "BUG-334: local array to keep slice rejected");

    /* BUG-336: arena-derived to keep param */
    err("struct D { u32 v; }\n"
        "void reg(keep *D ctx) {}\n"
        "void f() { u8[512] mem; Arena a = Arena.over(mem);\n"
        "    *D p = a.alloc(D) orelse return; reg(p); }",
        "BUG-336: arena-derived to keep rejected");

    /* BUG-325: @bitcast struct width validation */
    err("struct Small { u32 x; }\n"
        "struct Big { u64 x; u64 y; }\n"
        "void f() { Small s; Big b = @bitcast(Big, s); }",
        "BUG-325: @bitcast different-size structs rejected");

    ok("struct A { u32 x; u32 y; }\n"
       "struct B { u64 z; }\n"
       "void f() { A a; B b = @bitcast(B, a); }",
       "BUG-325: @bitcast same-size structs accepted");

    /* BUG-326: switch capture const safety */
    err("void f() {\n"
        "    const ?u32 val = 42;\n"
        "    switch (val) {\n"
        "        default => |*v| { *v = 10; }\n"
        "    }\n"
        "}",
        "BUG-326: switch mutable capture on const source rejected");

    ok("void f() {\n"
       "    ?u32 val = 42;\n"
       "    switch (val) {\n"
       "        default => |*v| { *v = 10; }\n"
       "    }\n"
       "}",
       "BUG-326: switch mutable capture on non-const OK");
}

static void test_red_team_343_346(void) {
    /* BUG-343: @cast cannot strip volatile qualifier */
    err("mmio 0x40020000..0x40020FFF;\n"
       "distinct typedef *u32 SafePtr;\n"
       "u32 main() {\n"
       "    volatile *u32 reg = @inttoptr(*u32, 0x40020000);\n"
       "    SafePtr p = @cast(SafePtr, reg);\n"
       "    return 0;\n"
       "}",
       "BUG-343: @cast strips volatile");

    /* BUG-343: @cast cannot strip const qualifier */
    err("distinct typedef *u32 MutPtr;\n"
       "u32 main() {\n"
       "    u32 x = 5;\n"
       "    const *u32 cp = &x;\n"
       "    MutPtr p = @cast(MutPtr, cp);\n"
       "    return 0;\n"
       "}",
       "BUG-343: @cast strips const");

    /* BUG-343: @cast with volatile OK when target is volatile */
    ok("mmio 0x40020000..0x40020FFF;\n"
       "distinct typedef volatile *u32 VolPtr;\n"
       "u32 main() {\n"
       "    volatile *u32 reg = @inttoptr(*u32, 0x40020000);\n"
       "    VolPtr p = @cast(VolPtr, reg);\n"
       "    return 0;\n"
       "}",
       "BUG-343: @cast volatile preserved OK");

    /* BUG-344: compute_type_size overflow — @size of massive multi-dim array
     * should return CONST_EVAL_FAIL (emitter uses sizeof), not a wrapped value */
    ok("struct Big { u32[1000] data; }\n"
       "u32 main() {\n"
       "    usize s = @size(Big);\n"
       "    return @truncate(u32, s);\n"
       "}",
       "BUG-344: @size on struct OK (no overflow)");

    /* BUG-350: array alignment in compute_type_size — u8[10] alignment=1 not 8 */
    ok("struct S { u8 a; u8[10] data; u8 b; }\n"
       "u8[@size(S)] buf;\n"
       "u32 main() {\n"
       "    usize s = @size(S);\n"
       "    return @truncate(u32, s);\n"
       "}",
       "BUG-350: @size struct with array uses element alignment");
}

static void test_mmio_provenance(void) {
    /* mmio: valid constant address in range */
    ok("mmio 0x40020000..0x40020FFF;\n"
       "u32 main() {\n"
       "    volatile *u32 reg = @inttoptr(*u32, 0x40020014);\n"
       "    return 0;\n"
       "}",
       "mmio: constant address in range OK");

    /* mmio: constant address outside range */
    err("mmio 0x40020000..0x40020FFF;\n"
       "u32 main() {\n"
       "    volatile *u32 reg = @inttoptr(*u32, 0x12345678);\n"
       "    return 0;\n"
       "}",
       "mmio: constant address outside range rejected");

    /* mmio: no ranges declared — @inttoptr rejected */
    err("u32 main() {\n"
       "    volatile *u32 reg = @inttoptr(*u32, 0x12345678);\n"
       "    return 0;\n"
       "}",
       "mmio: no ranges = @inttoptr rejected");

    /* mmio: multiple ranges, second range matches */
    ok("mmio 0x40020000..0x40020FFF;\n"
       "mmio 0x40011000..0x4001103F;\n"
       "u32 main() {\n"
       "    volatile *u32 reg = @inttoptr(*u32, 0x40011004);\n"
       "    return 0;\n"
       "}",
       "mmio: second range matches OK");

    /* mmio: start > end rejected */
    err("mmio 0x40020FFF..0x40020000;\n"
       "u32 main() { return 0; }",
       "mmio: start > end rejected");

    /* mmio: variable address — checker allows (runtime check in emitter) */
    ok("mmio 0x40020000..0x40020FFF;\n"
       "u32 main() {\n"
       "    u32 addr = 0x40020014;\n"
       "    volatile *u32 reg = @inttoptr(*u32, addr);\n"
       "    return 0;\n"
       "}",
       "mmio: variable address allowed at compile time");

    /* @ptrcast provenance: round-trip match OK */
    ok("struct Sensor { u32 id; }\n"
       "void use(*Sensor s) {}\n"
       "void test(*Sensor s) {\n"
       "    *opaque ctx = @ptrcast(*opaque, s);\n"
       "    *Sensor s2 = @ptrcast(*Sensor, ctx);\n"
       "    use(s2);\n"
       "}",
       "ptrcast provenance: round-trip match OK");

    /* @ptrcast provenance: wrong type caught at COMPILE TIME for simple idents */
    err("struct Sensor { u32 id; }\n"
       "struct Motor { u32 speed; }\n"
       "u32 main() {\n"
       "    Sensor s;\n"
       "    *opaque ctx = @ptrcast(*opaque, &s);\n"
       "    *Motor m = @ptrcast(*Motor, ctx);\n"
       "    return 0;\n"
       "}",
       "ptrcast provenance: wrong type rejected (compile-time)");

    /* @ptrcast provenance: unknown provenance allowed */
    ok("struct Sensor { u32 id; }\n"
       "u32 test(*opaque ctx) {\n"
       "    *Sensor s = @ptrcast(*Sensor, ctx);\n"
       "    return s.id;\n"
       "}",
       "ptrcast provenance: unknown (param) allowed");

    /* @ptrcast provenance: alias propagation caught at compile time */
    err("struct Sensor { u32 id; }\n"
       "struct Motor { u32 speed; }\n"
       "u32 main() {\n"
       "    Sensor s;\n"
       "    *opaque ctx = @ptrcast(*opaque, &s);\n"
       "    *opaque alias = ctx;\n"
       "    *Motor m = @ptrcast(*Motor, alias);\n"
       "    return 0;\n"
       "}",
       "ptrcast provenance: alias caught (compile-time)");

    /* @container: field validation — field exists */
    ok("struct Device { u32 id; u32 status; }\n"
       "u32 main() {\n"
       "    Device d;\n"
       "    *u32 p = &d.status;\n"
       "    *Device dev = @container(*Device, p, status);\n"
       "    return dev.id;\n"
       "}",
       "@container: valid field OK");

    /* @container: field validation — field does not exist */
    err("struct Device { u32 id; u32 status; }\n"
       "u32 main() {\n"
       "    Device d;\n"
       "    *u32 p = &d.status;\n"
       "    *Device dev = @container(*Device, p, nonexistent);\n"
       "    return 0;\n"
       "}",
       "@container: nonexistent field rejected");

    /* @container provenance: proven containment OK */
    ok("struct ListHead { *ListHead next; *ListHead prev; }\n"
       "struct Device { u32 id; ListHead list; }\n"
       "u32 main() {\n"
       "    Device dev;\n"
       "    *ListHead ptr = &dev.list;\n"
       "    *Device d = @container(*Device, ptr, list);\n"
       "    return d.id;\n"
       "}",
       "@container provenance: proven containment OK");

    /* @container provenance: wrong struct rejected */
    err("struct ListHead { *ListHead next; *ListHead prev; }\n"
       "struct Device { u32 id; ListHead list; }\n"
       "struct Other { u32 x; ListHead list; }\n"
       "u32 main() {\n"
       "    Device dev;\n"
       "    *ListHead ptr = &dev.list;\n"
       "    *Other o = @container(*Other, ptr, list);\n"
       "    return 0;\n"
       "}",
       "@container provenance: wrong struct rejected");

    /* @container provenance: wrong field rejected */
    err("struct ListHead { *ListHead next; *ListHead prev; }\n"
       "struct Device { u32 id; ListHead list; ListHead list2; }\n"
       "u32 main() {\n"
       "    Device dev;\n"
       "    *ListHead ptr = &dev.list;\n"
       "    *Device d = @container(*Device, ptr, list2);\n"
       "    return 0;\n"
       "}",
       "@container provenance: wrong field rejected");

    /* @container provenance: unknown provenance allowed */
    ok("struct ListHead { *ListHead next; *ListHead prev; }\n"
       "struct Device { u32 id; ListHead list; }\n"
       "u32 test(*ListHead ptr) {\n"
       "    *Device d = @container(*Device, ptr, list);\n"
       "    return d.id;\n"
       "}",
       "@container provenance: unknown (param) allowed");

    /* comptime: BIT macro equivalent */
    ok("comptime u32 BIT(u32 n) {\n"
       "    return 1 << n;\n"
       "}\n"
       "u32 main() {\n"
       "    u32 mask = BIT(3);\n"
       "    return mask;\n"
       "}",
       "comptime: BIT(3) = 8");

    /* comptime: MAX with if/else */
    ok("comptime u32 MAX(u32 a, u32 b) {\n"
       "    if (a > b) { return a; }\n"
       "    return b;\n"
       "}\n"
       "u32 main() {\n"
       "    u32 big = MAX(10, 20);\n"
       "    return big;\n"
       "}",
       "comptime: MAX(10, 20) = 20");

    /* comptime: non-constant args rejected */
    err("comptime u32 BIT(u32 n) {\n"
       "    return 1 << n;\n"
       "}\n"
       "u32 main() {\n"
       "    u32 x = 3;\n"
       "    u32 mask = BIT(x);\n"
       "    return mask;\n"
       "}",
       "comptime: non-constant args rejected");

    /* comptime: arithmetic expression */
    ok("comptime u32 ALIGN_UP(u32 n, u32 align) {\n"
       "    return (n + align - 1) & ~(align - 1);\n"
       "}\n"
       "u32 main() {\n"
       "    u32 aligned = ALIGN_UP(13, 8);\n"
       "    return aligned;\n"
       "}",
       "comptime: ALIGN_UP(13, 8) = 16");

    /* comptime: nested comptime call */
    ok("comptime u32 DOUBLE(u32 x) {\n"
       "    return x * 2;\n"
       "}\n"
       "u32 main() {\n"
       "    u32 val = DOUBLE(21);\n"
       "    return val;\n"
       "}",
       "comptime: DOUBLE(21) = 42");

    /* comptime: used in global const init */
    ok("comptime u32 BUF_SIZE(u32 n) {\n"
       "    return n * 4;\n"
       "}\n"
       "u32 main() {\n"
       "    u32 size = BUF_SIZE(8);\n"
       "    return size;\n"
       "}",
       "comptime: BUF_SIZE(8) = 32");

    /* comptime: bitwise operations */
    ok("comptime u32 FLAGS(u32 a, u32 b) {\n"
       "    return a | b;\n"
       "}\n"
       "u32 main() {\n"
       "    u32 f = FLAGS(4, 8);\n"
       "    return f;\n"
       "}",
       "comptime: bitwise OR FLAGS(4, 8) = 12");

    /* comptime if: true branch taken */
    ok("u32 main() {\n"
       "    u32 x = 0;\n"
       "    comptime if (1) {\n"
       "        x = 42;\n"
       "    }\n"
       "    return x;\n"
       "}",
       "comptime if: true branch taken");

    /* comptime if: false branch skipped, else taken */
    ok("u32 main() {\n"
       "    u32 x = 0;\n"
       "    comptime if (0) {\n"
       "        x = 99;\n"
       "    } else {\n"
       "        x = 42;\n"
       "    }\n"
       "    return x;\n"
       "}",
       "comptime if: else branch taken");

    /* comptime if: non-constant condition rejected */
    err("u32 main() {\n"
       "    u32 flag = 1;\n"
       "    comptime if (flag) {\n"
       "        u32 x = 5;\n"
       "    }\n"
       "    return 0;\n"
       "}",
       "comptime if: non-constant condition rejected");

    /* BUG-351: @cast escape — local address via distinct typedef */
    err("distinct typedef *u32 SafePtr;\n"
       "SafePtr leak() {\n"
       "    u32 x = 5;\n"
       "    return @cast(SafePtr, &x);\n"
       "}",
       "BUG-351: @cast local escape rejected");

    /* BUG-354: comptime if without else — return analysis respects taken branch */
    ok("u32 f() {\n"
       "    comptime if (1) { return 42; }\n"
       "}\n"
       "u32 main() { return f(); }",
       "BUG-354: comptime if (true) without else OK");

    /* BUG-354: comptime if false with else — else branch is the taken path */
    ok("u32 f() {\n"
       "    comptime if (0) { return 1; }\n"
       "    else { return 2; }\n"
       "}\n"
       "u32 main() { return f(); }",
       "BUG-354: comptime if (false) else branch OK");

    /* BUG-355: assignment escape through @ptrcast */
    err("mmio 0x0..0xFFFFFFFFFFFFFFFF;\n"
       "*u32 g_ptr = @inttoptr(*u32, 1);\n"
       "void f() {\n"
       "    u32 x = 5;\n"
       "    *u32 p = &x;\n"
       "    g_ptr = @ptrcast(*u32, p);\n"
       "}",
       "BUG-355: assign intrinsic wash rejected");

    /* BUG-355: assignment escape through @cast */
    err("mmio 0x0..0xFFFFFFFFFFFFFFFF;\n"
       "distinct typedef *u32 GPtr;\n"
       "GPtr g = @cast(GPtr, @inttoptr(*u32, 1));\n"
       "void f() {\n"
       "    u32 x = 5;\n"
       "    *u32 p = &x;\n"
       "    g = @cast(GPtr, p);\n"
       "}",
       "BUG-355: assign @cast escape rejected");

    /* BUG-356: deref flag loss — *pp washes is_local_derived */
    err("*u32 leak() {\n"
       "    u32 x = 10;\n"
       "    *u32 p = &x;\n"
       "    **u32 pp = &p;\n"
       "    *u32 p2 = *pp;\n"
       "    return p2;\n"
       "}",
       "BUG-356: deref flag propagation catches escape");

    /* BUG-393: compile-time provenance for struct fields (compound key) */
    err("struct Sensor { u32 val; }\n"
        "struct Motor { u32 speed; }\n"
        "struct Holder { *opaque p; }\n"
        "Sensor g_s;\n"
        "Holder g_h;\n"
        "void test() {\n"
        "    g_h.p = @ptrcast(*opaque, &g_s);\n"
        "    *Motor m = @ptrcast(*Motor, g_h.p);\n"
        "}\n",
        "BUG-393: struct field provenance mismatch (compile-time)");
    ok("struct Sensor { u32 val; }\n"
       "struct Holder { *opaque p; }\n"
       "Sensor g_s;\n"
       "Holder g_h;\n"
       "void test() {\n"
       "    g_h.p = @ptrcast(*opaque, &g_s);\n"
       "    *Sensor s = @ptrcast(*Sensor, g_h.p);\n"
       "}\n",
       "BUG-393: struct field provenance match (compile-time)");

    /* BUG-358: provenance preserved through @bitcast (compile-time) */
    err("struct Sensor { u32 id; }\n"
       "struct Motor { u32 speed; }\n"
       "void use(*Motor m) {}\n"
       "void test() {\n"
       "    Sensor s;\n"
       "    *opaque ctx = @ptrcast(*opaque, &s);\n"
       "    *opaque q = @bitcast(*opaque, ctx);\n"
       "    *Motor m = @ptrcast(*Motor, q);\n"
       "    use(m);\n"
       "}",
       "BUG-358: provenance through @bitcast (compile-time)");
}

int main(void) {
    printf("=== ZER Type Checker — Full Spec Coverage ===\n\n");

    test_s5_integer_coercion();
    test_s5_float_coercion();
    test_s5_int_float_mixing();
    test_s5_literal_flexibility();
    test_s5_optional_coercion();
    test_s5_array_slice_coercion();
    test_s5_const_coercion();
    test_s5_typedef();
    test_s6_scope_escape();
    test_s6_store_through();
    test_s6_keep();
    test_s6_deref();
    test_s8_exhaustive_switch();
    test_s8_no_fallthrough();
    test_s11_orelse();
    test_s11_if_unwrap();
    test_s12_struct();
    test_s12_enum();
    test_s12_union();
    test_s17_control_flow();
    test_s17_return();
    test_s18_arithmetic();
    test_s18_bitwise();
    test_s18_comparison();
    test_s18_logical();
    test_s18_assignment();
    test_s19_defer();
    test_s22_pool();
    test_s22_ring();
    test_s22_arena();
    test_s16_intrinsics();
    test_interactions();
    test_adversarial();
    test_security_review();
    test_negative_sweep();
    test_red_team_314_318();
    test_red_team_343_346();
    test_mmio_provenance();

    /* ---- Forced division guard ---- */
    printf("\n[forced division: literal divisor OK]\n");
    ok("u32 main() { return 100 / 4; }",
       "forced div: literal nonzero divisor OK");

    printf("[forced division: variable with literal init OK]\n");
    ok("u32 main() { u32 d = 5; return 100 / d; }",
       "forced div: variable initialized to nonzero OK");

    printf("[forced division: after nonzero guard OK]\n");
    ok("u32 f(u32 d) {\n"
       "    if (d == 0) { return 0; }\n"
       "    return 100 / d;\n"
       "}",
       "forced div: after if(d==0)return guard OK");

    printf("[forced division: unguarded variable — error]\n");
    err("u32 f(u32 d) { return 100 / d; }",
        "forced div: unguarded variable divisor rejected");

    printf("[forced division: modulo also forced]\n");
    err("u32 f(u32 d) { return 100 % d; }",
        "forced div: unguarded modulo rejected");

    printf("[forced division: modulo with guard OK]\n");
    ok("u32 f(u32 d) {\n"
       "    if (d == 0) { return 0; }\n"
       "    return 100 % d;\n"
       "}",
       "forced div: modulo after guard OK");

    printf("[forced division: for loop variable OK]\n");
    ok("u32 f() {\n"
       "    u32 sum = 0;\n"
       "    for (u32 i = 1; i < 10; i += 1) {\n"
       "        sum += 100 / i;\n"
       "    }\n"
       "    return sum;\n"
       "}",
       "forced div: for loop var starting at 1 is nonzero");

    /* ---- Auto-keep on fn ptr pointer-params ---- */
    printf("\n[auto-keep: fn ptr call with &local → error]\n");
    err("u32 g_val = 0;\n"
        "void do_thing(*u32 p) { }\n"
        "void f() {\n"
        "    void (*cb)(*u32) = do_thing;\n"
        "    u32 local = 5;\n"
        "    cb(&local);\n"
        "}",
        "auto-keep: fn ptr call with &local rejected (auto-keep)");

    printf("[auto-keep: fn ptr call with &global → OK]\n");
    ok("u32 g_val = 0;\n"
       "void do_thing(*u32 p) { }\n"
       "void f() {\n"
       "    void (*cb)(*u32) = do_thing;\n"
       "    cb(&g_val);\n"
       "}",
       "auto-keep: fn ptr call with &global OK");

    printf("[auto-keep: direct call with &local (no keep) → OK]\n");
    ok("void use_ptr(*u32 p) { }\n"
       "void f() {\n"
       "    u32 local = 5;\n"
       "    use_ptr(&local);\n"
       "}",
       "auto-keep: direct call without keep → &local OK");

    /* ---- Array-level *opaque provenance ---- */
    printf("\n[array provenance: homogeneous *opaque → OK]\n");
    ok("struct Sensor { u32 id; }\n"
       "struct State { *opaque ctx; *opaque ctx2; }\n"
       "void f() {\n"
       "    Sensor s1; Sensor s2;\n"
       "    State st;\n"
       "    st.ctx = @ptrcast(*opaque, &s1);\n"
       "    st.ctx2 = @ptrcast(*opaque, &s2);\n"
       "}",
       "array provenance: struct fields same type OK");

    printf("[array provenance: different struct fields can differ → OK]\n");
    ok("struct Sensor { u32 id; }\n"
       "struct Motor { u32 speed; }\n"
       "struct Holder { *opaque sensor_ctx; *opaque motor_ctx; }\n"
       "Sensor g_s;\n"
       "Motor g_m;\n"
       "void f() {\n"
       "    Holder h;\n"
       "    h.sensor_ctx = @ptrcast(*opaque, &g_s);\n"
       "    h.motor_ctx = @ptrcast(*opaque, &g_m);\n"
       "}",
       "array provenance: different struct fields → different types OK");

    /* ---- Cross-function provenance summaries ---- */
    printf("\n[cross-func prov: correct type from fn return → OK]\n");
    ok("struct Sensor { u32 id; }\n"
       "Sensor g_sensor;\n"
       "*opaque get_ctx() {\n"
       "    return @ptrcast(*opaque, &g_sensor);\n"
       "}\n"
       "void f() {\n"
       "    *opaque ctx = get_ctx();\n"
       "    *Sensor s = @ptrcast(*Sensor, ctx);\n"
       "}",
       "cross-func prov: fn returns *Sensor provenance, cast matches OK");

    printf("[cross-func prov: wrong type from fn return → error]\n");
    err("struct Sensor { u32 id; }\n"
        "struct Motor { u32 speed; }\n"
        "Sensor g_sensor;\n"
        "*opaque get_ctx() {\n"
        "    return @ptrcast(*opaque, &g_sensor);\n"
        "}\n"
        "void f() {\n"
        "    *opaque ctx = get_ctx();\n"
        "    *Motor m = @ptrcast(*Motor, ctx);\n"
        "}",
        "cross-func prov: fn returns *Sensor, cast to *Motor rejected");

    /* ---- Struct field division guard ---- */
    printf("\n[struct field div: guarded → OK]\n");
    ok("struct Config { u32 divisor; }\n"
       "u32 f(Config cfg) {\n"
       "    if (cfg.divisor == 0) { return 0; }\n"
       "    return 100 / cfg.divisor;\n"
       "}",
       "struct field div: guarded cfg.divisor OK");

    printf("[struct field div: unguarded → error]\n");
    err("struct Config { u32 divisor; }\n"
        "u32 f(Config cfg) {\n"
        "    return 100 / cfg.divisor;\n"
        "}",
        "struct field div: unguarded cfg.divisor rejected");

    /* ---- Bounds auto-guard ---- */
    printf("\n[auto-guard: unproven param index → warning (compiles OK)]\n");
    ok("void f(u32 idx) {\n"
       "    u32[8] buf;\n"
       "    buf[idx] = 5;\n"
       "}",
       "auto-guard: unproven param index compiles (auto-guard inserted)");

    printf("[auto-guard: unproven global index → warning (compiles OK)]\n");
    ok("u32 g_idx = 0;\n"
       "u32[8] buf;\n"
       "void f() {\n"
       "    buf[g_idx] = 5;\n"
       "}",
       "auto-guard: unproven global index compiles (auto-guard inserted)");

    printf("[auto-guard: proven literal → no warning]\n");
    ok("void f() {\n"
       "    u32[8] buf;\n"
       "    buf[3] = 5;\n"
       "}",
       "auto-guard: literal index proven — no auto-guard");

    printf("[auto-guard: proven for-loop → no warning]\n");
    ok("void f() {\n"
       "    u32[8] buf;\n"
       "    for (u32 i = 0; i < 8; i += 1) { buf[i] = i; }\n"
       "}",
       "auto-guard: for-loop index proven — no auto-guard");

    printf("[auto-guard: proven after guard → no warning]\n");
    ok("u32 f(u32 idx) {\n"
       "    u32[8] buf;\n"
       "    if (idx >= 8) { return 0; }\n"
       "    return buf[idx];\n"
       "}",
       "auto-guard: guarded index proven — no auto-guard");

    /* ---- Whole-program *opaque param provenance ---- */
    printf("\n[whole-program prov: correct type passed → OK]\n");
    ok("struct Sensor { u32 id; }\n"
       "void process(*opaque ctx) {\n"
       "    *Sensor s = @ptrcast(*Sensor, ctx);\n"
       "}\n"
       "Sensor g_s;\n"
       "void f() {\n"
       "    process(@ptrcast(*opaque, &g_s));\n"
       "}",
       "whole-program prov: caller passes *Sensor, callee casts to *Sensor OK");

    printf("[whole-program prov: wrong type passed → error]\n");
    err("struct Sensor { u32 id; }\n"
        "struct Motor { u32 speed; }\n"
        "void process(*opaque ctx) {\n"
        "    *Sensor s = @ptrcast(*Sensor, ctx);\n"
        "}\n"
        "Motor g_m;\n"
        "void f() {\n"
        "    process(@ptrcast(*opaque, &g_m));\n"
        "}",
        "whole-program prov: caller passes *Motor, callee expects *Sensor → error");

    printf("[whole-program prov: unknown provenance (param) → OK (runtime handles)]\n");
    ok("struct Sensor { u32 id; }\n"
       "void process(*opaque ctx) {\n"
       "    *Sensor s = @ptrcast(*Sensor, ctx);\n"
       "}\n"
       "void f(*opaque unknown) {\n"
       "    process(unknown);\n"
       "}",
       "whole-program prov: unknown provenance → OK (runtime type_id)");

    /* ---- @probe intrinsic ---- */
    printf("\n[@probe: basic usage → returns ?u32]\n");
    ok("void f() {\n"
       "    ?u32 val = @probe(0x40020000);\n"
       "}",
       "@probe: basic constant address returns ?u32");

    printf("[@probe: with orelse → unwrap]\n");
    ok("u32 f() {\n"
       "    u32 val = @probe(0x40020000) orelse 0;\n"
       "    return val;\n"
       "}",
       "@probe: orelse unwrap to u32");

    printf("[@probe: with if-unwrap → capture]\n");
    ok("u32 f() {\n"
       "    if (@probe(0x40020000)) |val| {\n"
       "        return val;\n"
       "    }\n"
       "    return 0;\n"
       "}",
       "@probe: if-unwrap capture");

    printf("[@probe: variable address]\n");
    ok("u32 f(u32 addr) {\n"
       "    ?u32 val = @probe(addr);\n"
       "    return val orelse 0;\n"
       "}",
       "@probe: variable address");

    printf("[@probe: wrong arg type → error]\n");
    err("struct S { u32 x; }\n"
        "void f() {\n"
        "    S s;\n"
        "    ?u32 val = @probe(s);\n"
        "}",
        "@probe: struct arg rejected");

    printf("[@probe: no args → error]\n");
    err("void f() { @probe(); }",
        "@probe: no args rejected");

    /* ---- Interrupt safety ---- */
    printf("\n--- interrupt safety ---\n");

    printf("[ISR safety: shared global without volatile → error]\n");
    err("u32 counter = 0;\n"
        "interrupt TIMER1 { counter += 1; }\n"
        "void f() { u32 x = counter; }\n",
        "ISR safety: shared global without volatile");

    printf("[ISR safety: shared volatile global → ok]\n");
    ok("volatile u32 counter = 0;\n"
       "interrupt TIMER1 { counter = counter + 1; }\n"
       "void f() { u32 x = counter; }\n",
       "ISR safety: volatile shared global OK");

    printf("[ISR safety: volatile + compound assign → error (race)]\n");
    err("volatile u32 flags = 0;\n"
        "interrupt UART_RX { flags |= 0x01; }\n"
        "void f() { flags |= 0x02; }\n",
        "ISR safety: compound assign on shared volatile = race");

    printf("[ISR safety: global only in ISR → ok (no sharing)]\n");
    ok("u32 isr_only = 0;\n"
       "interrupt TIMER1 { isr_only = 1; }\n"
       "void f() { u32 x = 5; }\n",
       "ISR safety: global only in ISR — no sharing, OK");

    printf("[ISR safety: global only in func → ok]\n");
    ok("u32 func_only = 0;\n"
       "void f() { func_only = 1; }\n",
       "ISR safety: global only in func — OK");

    printf("[ISR safety: volatile + plain assign (no compound) → ok]\n");
    ok("volatile u32 flag = 0;\n"
       "interrupt TIMER1 { flag = 1; }\n"
       "void f() { flag = 0; }\n",
       "ISR safety: volatile plain assign both sides OK");

    /* ---- Stack depth / recursion ---- */
    printf("\n--- stack depth analysis ---\n");

    printf("[stack: direct recursion → warning (not error)]\n");
    ok("void f() { f(); }\n"
       "void g() { }\n",
       "stack: direct recursion compiles (warning only)");

    printf("[stack: mutual recursion → warning (not error)]\n");
    ok("void a() { b(); }\n"
       "void b() { a(); }\n",
       "stack: mutual recursion compiles (warning only)");

    printf("[stack: no recursion → ok]\n");
    ok("void a() { }\n"
       "void b() { a(); }\n"
       "void c() { b(); }\n",
       "stack: chain call, no recursion OK");

    /* ---- Range invalidation on reassignment ---- */
    printf("\n--- range invalidation ---\n");

    printf("[range: reassignment invalidates proven range]\n");
    ok("u32 get_input() { return 0; }\n"
       "u32 main() {\n"
       "    u32 i = 5;\n"
       "    u32[10] arr;\n"
       "    if (i < 10) {\n"
       "        i = get_input();\n"
       "        arr[i] = 1;\n"
       "    }\n"
       "    return 0;\n"
       "}\n",
       "range: reassigned i not proven — auto-guard fires");

    /* ---- Compound division guard ---- */
    printf("\n--- compound division guard ---\n");

    printf("[/= unproven divisor → error]\n");
    err("u32 get_input() { return 0; }\n"
        "u32 main() {\n"
        "    u32 x = 100;\n"
        "    u32 d = get_input();\n"
        "    x /= d;\n"
        "    return x;\n"
        "}\n",
        "/= compound: unproven divisor rejected");

    printf("[/= literal nonzero → ok]\n");
    ok("u32 main() {\n"
       "    u32 x = 100;\n"
       "    x /= 5;\n"
       "    return x;\n"
       "}\n",
       "/= literal 5: proven nonzero OK");

    /* ---- Struct wrapper escape detection ---- */
    printf("\n--- struct wrapper local escape ---\n");

    printf("[struct escape: identity(wrap(&x).p) → error]\n");
    err("struct Box { *u32 p; }\n"
        "Box wrap(*u32 p) { Box b; b.p = p; return b; }\n"
        "*u32 identity(*u32 p) { return p; }\n"
        "*u32 leak() { u32 x = 5; return identity(wrap(&x).p); }\n",
        "struct escape: identity(wrap(&x).p) caught");

    printf("[struct escape: Box b = wrap(&x); return b.p → error]\n");
    err("struct Box { *u32 p; }\n"
        "Box wrap(*u32 p) { Box b; b.p = p; return b; }\n"
        "*u32 leak() { u32 x = 5; Box b = wrap(&x); return b.p; }\n",
        "struct escape: intermediate var b = wrap(&x), return b.p caught");

    printf("[struct escape: wrap(global_ptr).p → ok]\n");
    ok("struct Box { *u32 p; }\n"
       "u32 g = 5;\n"
       "Box wrap(*u32 p) { Box b; b.p = p; return b; }\n"
       "*u32 safe() { return wrap(&g).p; }\n",
       "struct escape: global pointer — no escape, OK");

    printf("[struct escape: identity(@cstr(local,...)) direct → error]\n");
    err("*u8 identity(*u8 p) { return p; }\n"
        "*u8 leak() {\n"
        "    u8[10] local;\n"
        "    return identity(@cstr(local, \"hi\"));\n"
        "}\n",
        "@cstr escape: identity(@cstr(local,...)) direct arg caught");

    printf("[struct escape: @cstr local-derived → error]\n");
    err("*u8 identity(*u8 p) { return p; }\n"
        "*u8 leak() {\n"
        "    u8[10] local;\n"
        "    *u8 p = @cstr(local, \"hi\");\n"
        "    return identity(p);\n"
        "}\n",
        "@cstr escape: p = @cstr(local,...) is local-derived");

    printf("[struct escape: slice via struct wrapper → error]\n");
    err("struct Box { []u8 data; }\n"
        "Box wrap([]u8 d) { Box b; b.data = d; return b; }\n"
        "[]u8 leak() {\n"
        "    u8[10] local;\n"
        "    return wrap(local).data;\n"
        "}\n",
        "slice escape: wrap(local_array).data caught");

    printf("[struct escape: identity(opt orelse &x) → error]\n");
    err("*u32 identity(*u32 p) { return p; }\n"
        "*u32 leak(?*u32 opt) {\n"
        "    u32 x = 5;\n"
        "    return identity(opt orelse &x);\n"
        "}\n",
        "orelse escape: identity(opt orelse &local) caught");

    /* ---- Cross-platform portability ---- */
    printf("\n--- cross-platform portability ---\n");

    printf("[portability: @ptrtoint to usize → ok]\n");
    ok("mmio 0x0..0xFFFFFFFFFFFFFFFF;\n"
       "void f() {\n"
       "    u32 x = 42;\n"
       "    usize addr = @ptrtoint(&x);\n"
       "}\n",
       "portability: @ptrtoint to usize OK");

    /* Note: @ptrtoint to u32 produces a WARNING (not error),
     * so ok() still passes — warnings don't count as failures.
     * The warning is for portability only. */
    printf("[portability: @ptrtoint to u32 → warning (compiles)]\n");
    ok("mmio 0x0..0xFFFFFFFFFFFFFFFF;\n"
       "void f() {\n"
       "    u32 x = 42;\n"
       "    u32 addr = @ptrtoint(&x);\n"
       "}\n",
       "portability: @ptrtoint to u32 — warning but compiles");

    /* ---- @inttoptr alignment check ---- */
    printf("\n[@inttoptr alignment: u32 at misaligned address → error]\n");
    err("mmio 0x40020000..0x40020FFF;\n"
        "void f() {\n"
        "    volatile *u32 reg = @inttoptr(*u32, 0x40020001);\n"
        "}\n",
        "@inttoptr alignment: u32 at 0x40020001 (not 4-byte aligned)");

    printf("[@inttoptr alignment: u16 at odd address → error]\n");
    err("mmio 0x40020000..0x40020FFF;\n"
        "void f() {\n"
        "    volatile *u16 reg = @inttoptr(*u16, 0x40020001);\n"
        "}\n",
        "@inttoptr alignment: u16 at 0x40020001 (not 2-byte aligned)");

    printf("[@inttoptr alignment: u32 at aligned address → ok]\n");
    ok("mmio 0x40020000..0x40020FFF;\n"
       "void f() {\n"
       "    volatile *u32 reg = @inttoptr(*u32, 0x40020000);\n"
       "}\n",
       "@inttoptr alignment: u32 at 0x40020000 (4-byte aligned OK)");

    printf("[@inttoptr alignment: u16 at even address → ok]\n");
    ok("mmio 0x40020000..0x40020FFF;\n"
       "void f() {\n"
       "    volatile *u16 reg = @inttoptr(*u16, 0x40020002);\n"
       "}\n",
       "@inttoptr alignment: u16 at 0x40020002 (2-byte aligned OK)");

    printf("[@inttoptr alignment: u8 at any address → ok]\n");
    ok("mmio 0x40020000..0x40020FFF;\n"
       "void f() {\n"
       "    volatile *u8 reg = @inttoptr(*u8, 0x40020001);\n"
       "}\n",
       "@inttoptr alignment: u8 at 0x40020001 (1-byte aligned OK)");

    printf("[@inttoptr alignment: u64 at 4-byte aligned → error]\n");
    err("mmio 0x40020000..0x40020FFF;\n"
        "void f() {\n"
        "    volatile *u64 reg = @inttoptr(*u64, 0x40020004);\n"
        "}\n",
        "@inttoptr alignment: u64 at 0x40020004 (not 8-byte aligned)");

    printf("[@inttoptr alignment: u64 at 8-byte aligned → ok]\n");
    ok("mmio 0x40020000..0x40020FFF;\n"
       "void f() {\n"
       "    volatile *u64 reg = @inttoptr(*u64, 0x40020008);\n"
       "}\n",
       "@inttoptr alignment: u64 at 0x40020008 (8-byte aligned OK)");

    /* ---- Nested comptime calls ---- */
    printf("\n[comptime nested: BUF_SIZE calls BIT]\n");
    ok("comptime u32 BIT(u32 n) { return 1 << n; }\n"
       "comptime u32 BUF_SIZE() { return BIT(3) * 4; }\n"
       "u32[BUF_SIZE()] buf;\n"
       "u32 main() { return 0; }\n",
       "comptime nested: BUF_SIZE()=BIT(3)*4=32 as array size");

    printf("[comptime: mutual recursion → error (not crash)]\n");
    err("comptime u32 crash(u32 n) { return crash(n); }\n"
        "u32[crash(1)] arr;\n"
        "u32 main() { return 0; }\n",
        "comptime: infinite recursion caught, not segfault");

    printf("[comptime global: BIT(3) at global scope]\n");
    ok("comptime u32 BIT(u32 n) { return 1 << n; }\n"
       "u32 mask = BIT(3);\n"
       "u32 main() { return mask; }\n",
       "comptime global: u32 mask = BIT(3) at global scope");

    printf("[comptime nested: triple nesting]\n");
    ok("comptime u32 A() { return 2; }\n"
       "comptime u32 B() { return A() * 3; }\n"
       "comptime u32 C() { return B() + 1; }\n"
       "u32[C()] arr;\n"
       "u32 main() { return 0; }\n",
       "comptime nested: C()=B()+1=A()*3+1=7 as array size");

    printf("[@probe: scan loop pattern]\n");
    ok("u32 scan() {\n"
       "    u32 count = 0;\n"
       "    for (u32 addr = 0x40000000; addr < 0x40010000; addr += 0x1000) {\n"
       "        if (@probe(addr)) |val| {\n"
       "            count += 1;\n"
       "        }\n"
       "    }\n"
       "    return count;\n"
       "}",
       "@probe: discovery scan loop");

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
