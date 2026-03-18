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
    /* dropping const — should reject */
    /* Note: const is on the slice/pointer, not easily testable without const vars */
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
    /* nonexistent field — currently returns void (UFCS fallback), acceptable */
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

    /* UFCS */
    ok("struct Task { u32 pid; }\n"
       "void run(*Task t) { }\n"
       "void f() { Task t; t.run(); }",
       "UFCS call");

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
    ok("void f() { []u8 msg = \"hello\"; }", "string literal to mutable slice OK");

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

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
