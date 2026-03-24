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
}

/* ================================================================ */

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

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
