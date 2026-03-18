/* ================================================================
 * Firmware Pattern Stress Tests
 *
 * Real embedded patterns that exercise compiler edge cases.
 * Each test: ZER source → parse → check → emit C → GCC → run → verify.
 * Ordered by confidence (lowest first = most likely to break).
 * ================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lexer.h"
#include "ast.h"
#include "parser.h"
#include "types.h"
#include "checker.h"
#include "emitter.h"
#include "zercheck.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* compile ZER source to C, write to file */
static bool zer_to_c(const char *zer_source, const char *c_output_path) {
    Arena arena;
    arena_init(&arena, 512 * 1024);

    Scanner scanner;
    scanner_init(&scanner, zer_source);
    Parser parser;
    parser_init(&parser, &scanner, &arena, "test.zer");
    Node *file = parse_file(&parser);
    if (parser.had_error) { arena_free(&arena); return false; }

    Checker checker;
    checker_init(&checker, &arena, "test.zer");
    if (!checker_check(&checker, file)) { arena_free(&arena); return false; }

    ZerCheck zc;
    zercheck_init(&zc, &checker, &arena, "test.zer");
    if (!zercheck_run(&zc, file)) { arena_free(&arena); return false; }

    FILE *out = fopen(c_output_path, "w");
    if (!out) { arena_free(&arena); return false; }
    Emitter emitter;
    emitter_init(&emitter, out, &arena, &checker);
    emit_file(&emitter, file);
    fclose(out);
    arena_free(&arena);
    return true;
}

/* full pipeline: ZER → C → GCC → run → check exit code */
static void test_e2e(const char *zer_source, int expected_exit, const char *test_name) {
    tests_run++;

    if (!zer_to_c(zer_source, "_zer_fw_test.c")) {
        printf("  FAIL: %s — ZER compilation failed\n", test_name);
        tests_failed++;
        return;
    }

    int gcc_ret = system("gcc -std=c99 -O0 -w -o _zer_fw_test.exe _zer_fw_test.c 2>_zer_fw_err.txt");
    if (gcc_ret != 0) {
        printf("  FAIL: %s — GCC compilation failed\n", test_name);
        FILE *ef = fopen("_zer_fw_err.txt", "r");
        if (ef) {
            char buf[512];
            while (fgets(buf, sizeof(buf), ef)) printf("    gcc: %s", buf);
            fclose(ef);
        }
        tests_failed++;
        return;
    }

    int run_ret = system(".\\_zer_fw_test.exe");
    int exit_code = run_ret;

    if (exit_code != expected_exit) {
        printf("  FAIL: %s — expected exit %d, got %d\n",
               test_name, expected_exit, exit_code);
        tests_failed++;
    } else {
        tests_passed++;
    }
}

/* ================================================================
 * PATTERN 1: Deep pointer-to-struct field chains
 *
 * task->config.uart->regs.status
 * Exercises . vs -> decision at each level via checker_get_type()
 * ================================================================ */

static void test_deep_struct_chains(void) {
    printf("\n--- Pattern 1: Deep struct pointer chains ---\n");

    /* 1a: struct.field.field (2 levels) */
    test_e2e(
        "struct Inner { u32 value; }\n"
        "struct Outer { Inner inner; }\n"
        "u32 main() {\n"
        "    Outer o;\n"
        "    o.inner.value = 42;\n"
        "    return o.inner.value - 42;\n"
        "}\n",
        0, "struct.field.field (2 levels)");

    /* 1b: struct.field.field.field (3 levels) */
    test_e2e(
        "struct Level3 { u32 val; }\n"
        "struct Level2 { Level3 deep; }\n"
        "struct Level1 { Level2 mid; }\n"
        "u32 main() {\n"
        "    Level1 top;\n"
        "    top.mid.deep.val = 99;\n"
        "    return top.mid.deep.val - 99;\n"
        "}\n",
        0, "struct.field.field.field (3 levels)");

    /* 1c: ptr->field.field (pointer to struct with nested struct) */
    test_e2e(
        "struct Config { u32 baud; }\n"
        "struct Uart { Config cfg; }\n"
        "u32 main() {\n"
        "    Uart u;\n"
        "    u.cfg.baud = 9600;\n"
        "    *Uart p = &u;\n"
        "    return p.cfg.baud - 9600;\n"
        "}\n",
        0, "ptr->field.field");

    /* 1d: struct.ptr->field (struct contains pointer to another) */
    test_e2e(
        "struct Regs { u32 status; }\n"
        "struct Driver { *Regs regs; }\n"
        "u32 main() {\n"
        "    Regs r;\n"
        "    r.status = 7;\n"
        "    Driver d;\n"
        "    d.regs = &r;\n"
        "    return d.regs.status - 7;\n"
        "}\n",
        0, "struct.ptr->field");

    /* 1e: ptr->ptr->field (double pointer chain) */
    test_e2e(
        "struct Reg { u32 val; }\n"
        "struct Periph { *Reg ctrl; }\n"
        "u32 main() {\n"
        "    Reg r;\n"
        "    r.val = 55;\n"
        "    Periph p;\n"
        "    p.ctrl = &r;\n"
        "    *Periph pp = &p;\n"
        "    return pp.ctrl.val - 55;\n"
        "}\n",
        0, "ptr->ptr->field (double chain)");

    /* 1f: 3-level mixed: ptr->struct.ptr->field */
    test_e2e(
        "struct HwReg { u32 data; }\n"
        "struct UartCfg { *HwReg tx_reg; }\n"
        "struct UartDev { UartCfg config; }\n"
        "u32 main() {\n"
        "    HwReg reg;\n"
        "    reg.data = 123;\n"
        "    UartDev uart;\n"
        "    uart.config.tx_reg = &reg;\n"
        "    *UartDev p = &uart;\n"
        "    return p.config.tx_reg.data - 123;\n"
        "}\n",
        0, "ptr->struct.ptr->field (3-level mixed)");

    /* 1g: function taking pointer to struct, accessing nested field */
    test_e2e(
        "struct Inner { u32 x; }\n"
        "struct Outer { Inner a; Inner b; }\n"
        "u32 get_sum(*Outer o) {\n"
        "    return o.a.x + o.b.x;\n"
        "}\n"
        "u32 main() {\n"
        "    Outer obj;\n"
        "    obj.a.x = 17;\n"
        "    obj.b.x = 25;\n"
        "    return get_sum(&obj) - 42;\n"
        "}\n",
        0, "function ptr param -> nested field access");
}

/* ================================================================
 * PATTERN 2: Orelse chains in realistic error handling
 * ================================================================ */

static void test_orelse_error_chains(void) {
    printf("\n--- Pattern 2: Orelse error handling chains ---\n");

    /* 2a: sequential orelse with early returns */
    test_e2e(
        "?u32 step1() { return 10; }\n"
        "?u32 step2() { return 20; }\n"
        "?u32 step3() { return 12; }\n"
        "u32 run() {\n"
        "    u32 a = step1() orelse return;\n"
        "    u32 b = step2() orelse return;\n"
        "    u32 c = step3() orelse return;\n"
        "    return a + b + c;\n"
        "}\n"
        "u32 main() {\n"
        "    return run() - 42;\n"
        "}\n",
        0, "sequential orelse chain (all succeed)");

    /* 2b: orelse chain where middle step fails */
    test_e2e(
        "?u32 step1() { return 10; }\n"
        "?u32 step2() { return null; }\n"
        "?u32 step3() { return 30; }\n"
        "u32 run() {\n"
        "    u32 a = step1() orelse return;\n"
        "    u32 b = step2() orelse return;\n"
        "    u32 c = step3() orelse return;\n"
        "    return 99;\n"
        "}\n"
        "u32 main() {\n"
        "    return run();\n"
        "}\n",
        0, "orelse chain (middle fails, returns 0)");

    /* 2c: orelse with defer cleanup on error path */
    test_e2e(
        "u32 cleanup_count = 0;\n"
        "void cleanup() { cleanup_count += 1; }\n"
        "?u32 maybe_fail(bool fail) {\n"
        "    if (fail) { return null; }\n"
        "    return 5;\n"
        "}\n"
        "u32 do_work() {\n"
        "    defer cleanup();\n"
        "    u32 val = maybe_fail(false) orelse return;\n"
        "    return val;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 result = do_work();\n"
        "    if (result != 5) { return 1; }\n"
        "    if (cleanup_count != 1) { return 2; }\n"
        "    return 0;\n"
        "}\n",
        0, "orelse + defer cleanup");

    /* 2d: nested orelse (primary fails, fallback succeeds) */
    test_e2e(
        "?u32 primary() { return null; }\n"
        "?u32 fallback() { return 42; }\n"
        "u32 main() {\n"
        "    u32 val = primary() orelse fallback() orelse 0;\n"
        "    return val - 42;\n"
        "}\n",
        0, "nested orelse chain");

    /* 2e: orelse with continue in loop */
    test_e2e(
        "?u32 maybe(u32 i) {\n"
        "    if (i == 2) { return null; }\n"
        "    return i;\n"
        "}\n"
        "u32 main() {\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 5; i += 1) {\n"
        "        u32 val = maybe(i) orelse continue;\n"
        "        sum += val;\n"
        "    }\n"
        "    return sum - 8;\n"
        "}\n",
        0, "orelse continue in loop (skip i=2)");

    /* 2f: orelse break */
    test_e2e(
        "?u32 nothing() { return null; }\n"
        "u32 main() {\n"
        "    u32 result = 99;\n"
        "    while (true) {\n"
        "        u32 val = nothing() orelse break;\n"
        "        result = val;\n"
        "    }\n"
        "    return result - 99;\n"
        "}\n",
        0, "orelse break exits loop");
}

/* ================================================================
 * PATTERN 3: Pool + Handle driver pattern
 * ================================================================ */

static void test_pool_driver_pattern(void) {
    printf("\n--- Pattern 3: Pool + Handle driver pattern ---\n");

    /* 3a: alloc, configure, read, free */
    test_e2e(
        "struct Conn { u32 fd; u32 flags; }\n"
        "Pool(Conn, 4) pool;\n"
        "u32 main() {\n"
        "    Handle(Conn) h = pool.alloc() orelse return;\n"
        "    pool.get(h).fd = 42;\n"
        "    pool.get(h).flags = 3;\n"
        "    u32 result = pool.get(h).fd;\n"
        "    pool.free(h);\n"
        "    return result - 42;\n"
        "}\n",
        0, "pool alloc/configure/read/free");

    /* 3b: multiple handles, independent lifecycle */
    test_e2e(
        "struct Task { u32 id; u32 prio; }\n"
        "Pool(Task, 8) tasks;\n"
        "u32 main() {\n"
        "    Handle(Task) a = tasks.alloc() orelse return;\n"
        "    Handle(Task) b = tasks.alloc() orelse return;\n"
        "    tasks.get(a).id = 10;\n"
        "    tasks.get(b).id = 20;\n"
        "    u32 sum = tasks.get(a).id + tasks.get(b).id;\n"
        "    tasks.free(a);\n"
        "    tasks.free(b);\n"
        "    return sum - 30;\n"
        "}\n",
        0, "multiple handles independent lifecycle");

    /* 3c: pool in loop — alloc/use/free per iteration */
    test_e2e(
        "struct Item { u32 val; }\n"
        "Pool(Item, 4) pool;\n"
        "u32 main() {\n"
        "    u32 total = 0;\n"
        "    for (u32 i = 0; i < 3; i += 1) {\n"
        "        Handle(Item) h = pool.alloc() orelse return;\n"
        "        pool.get(h).val = i + 1;\n"
        "        total += pool.get(h).val;\n"
        "        pool.free(h);\n"
        "    }\n"
        "    return total - 6;\n"
        "}\n",
        0, "pool alloc/use/free per loop iteration");
}

/* ================================================================
 * PATTERN 4: Enum state machine
 * ================================================================ */

static void test_state_machine(void) {
    printf("\n--- Pattern 4: Enum state machine ---\n");

    /* 4a: basic state machine step */
    test_e2e(
        "enum State { idle, connecting, connected, err }\n"
        "State step(State s) {\n"
        "    switch (s) {\n"
        "        .idle => { return State.connecting; }\n"
        "        .connecting => { return State.connected; }\n"
        "        .connected => { return State.idle; }\n"
        "        .err => { return State.idle; }\n"
        "    }\n"
        "}\n"
        "u32 main() {\n"
        "    State s = State.idle;\n"
        "    s = step(s);\n"
        "    s = step(s);\n"
        "    if (s == State.connected) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "enum state machine (idle->connecting->connected)");

    /* 4b: state machine with counter */
    test_e2e(
        "enum Phase { init, run, done }\n"
        "u32 counter = 0;\n"
        "Phase tick(Phase p) {\n"
        "    switch (p) {\n"
        "        .init => { counter = 0; return Phase.run; }\n"
        "        .run => {\n"
        "            counter += 1;\n"
        "            if (counter >= 5) { return Phase.done; }\n"
        "            return Phase.run;\n"
        "        }\n"
        "        .done => { return Phase.done; }\n"
        "    }\n"
        "}\n"
        "u32 main() {\n"
        "    Phase p = Phase.init;\n"
        "    for (u32 i = 0; i < 10; i += 1) {\n"
        "        p = tick(p);\n"
        "    }\n"
        "    if (counter != 5) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0, "state machine with counter");
}

/* ================================================================
 * PATTERN 5: Register access (packed struct + bit extraction)
 * ================================================================ */

static void test_register_patterns(void) {
    printf("\n--- Pattern 5: Register access patterns ---\n");

    /* 5a: packed struct read/write */
    test_e2e(
        "packed struct StatusReg {\n"
        "    u8 flags;\n"
        "    u8 error_code;\n"
        "    u16 count;\n"
        "}\n"
        "u32 main() {\n"
        "    StatusReg reg;\n"
        "    reg.flags = 0xA5;\n"
        "    reg.error_code = 3;\n"
        "    reg.count = 1000;\n"
        "    if (reg.flags != 0xA5) { return 1; }\n"
        "    if (reg.error_code != 3) { return 2; }\n"
        "    if (reg.count != 1000) { return 3; }\n"
        "    return 0;\n"
        "}\n",
        0, "packed struct read/write");

    /* 5b: bit extraction from u32 */
    test_e2e(
        "u32 main() {\n"
        "    u32 reg = 0xDEADBEEF;\n"
        "    u32 nibble = reg[7..4];\n"
        "    return nibble - 0xE;\n"
        "}\n",
        0, "bit extraction reg[7..4]");

    /* 5c: multiple bit extractions */
    test_e2e(
        "u32 main() {\n"
        "    u32 reg = 0b11001010;\n"
        "    u32 low = reg[3..0];\n"
        "    u32 high = reg[7..4];\n"
        "    if (low != 10) { return 1; }\n"
        "    if (high != 12) { return 2; }\n"
        "    return 0;\n"
        "}\n",
        0, "multiple bit extractions from same register");
}

/* ================================================================
 * PATTERN 6: Complex defer + control flow
 * ================================================================ */

static void test_defer_complex(void) {
    printf("\n--- Pattern 6: Complex defer patterns ---\n");

    /* 6a: defer in nested scopes */
    test_e2e(
        "u32 order = 0;\n"
        "void log_n(u32 n) { order = order * 10 + n; }\n"
        "u32 main() {\n"
        "    defer log_n(1);\n"
        "    {\n"
        "        defer log_n(2);\n"
        "        log_n(3);\n"
        "    }\n"
        "    log_n(4);\n"
        "    return 0;\n"
        "}\n",
        0, "defer in nested scopes");

    /* 6b: defer + early return */
    test_e2e(
        "u32 cleaned = 0;\n"
        "void cleanup() { cleaned = 1; }\n"
        "u32 early_exit(bool bail) {\n"
        "    defer cleanup();\n"
        "    if (bail) { return 1; }\n"
        "    return 0;\n"
        "}\n"
        "u32 main() {\n"
        "    early_exit(true);\n"
        "    if (cleaned != 1) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0, "defer fires on early return");

    /* 6c: defer + for loop + break */
    test_e2e(
        "u32 defer_count = 0;\n"
        "void inc() { defer_count += 1; }\n"
        "u32 main() {\n"
        "    for (u32 i = 0; i < 5; i += 1) {\n"
        "        defer inc();\n"
        "        if (i == 3) { break; }\n"
        "    }\n"
        "    return defer_count - 4;\n"
        "}\n",
        0, "defer in loop with break at i=3 (4 defers)");

    /* 6d: 3 defers reverse order */
    test_e2e(
        "u32 seq = 0;\n"
        "void a() { seq = seq * 10 + 1; }\n"
        "void b() { seq = seq * 10 + 2; }\n"
        "void c() { seq = seq * 10 + 3; }\n"
        "u32 run() {\n"
        "    defer a();\n"
        "    defer b();\n"
        "    defer c();\n"
        "    return 0;\n"
        "}\n"
        "u32 main() {\n"
        "    run();\n"
        "    if (seq != 321) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0, "3 defers reverse order (expect 321)");

    /* 6e: defer + orelse return combo */
    test_e2e(
        "u32 defer_fired = 0;\n"
        "void mark() { defer_fired = 1; }\n"
        "?u32 nothing() { return null; }\n"
        "u32 try_it() {\n"
        "    defer mark();\n"
        "    u32 val = nothing() orelse return;\n"
        "    return val;\n"
        "}\n"
        "u32 main() {\n"
        "    try_it();\n"
        "    if (defer_fired != 1) { return 1; }\n"
        "    return 0;\n"
        "}\n",
        0, "defer fires even on orelse return path");
}

/* ================================================================
 * PATTERN 7: Ring buffer
 * ================================================================ */

static void test_ring_buffer(void) {
    printf("\n--- Pattern 7: Ring buffer patterns ---\n");

    /* 7a: push and pop FIFO */
    test_e2e(
        "Ring(u8, 16) rx;\n"
        "u32 main() {\n"
        "    rx.push(10);\n"
        "    rx.push(20);\n"
        "    rx.push(30);\n"
        "    u8 a = rx.pop() orelse 0;\n"
        "    u8 b = rx.pop() orelse 0;\n"
        "    u8 c = rx.pop() orelse 0;\n"
        "    u32 sum = @truncate(u32, a) + @truncate(u32, b) + @truncate(u32, c);\n"
        "    return sum - 60;\n"
        "}\n",
        0, "ring push/pop FIFO order");

    /* 7b: pop from empty ring */
    test_e2e(
        "Ring(u8, 4) buf;\n"
        "u32 main() {\n"
        "    u8 val = buf.pop() orelse 77;\n"
        "    return val - 77;\n"
        "}\n",
        0, "ring pop empty returns null, orelse default");

    /* 7c: Ring(u32) different element type */
    test_e2e(
        "Ring(u32, 8) q;\n"
        "u32 main() {\n"
        "    q.push(100);\n"
        "    q.push(200);\n"
        "    u32 first = q.pop() orelse 0;\n"
        "    u32 second = q.pop() orelse 0;\n"
        "    return first + second - 300;\n"
        "}\n",
        0, "Ring(u32) push/pop");
}

/* ================================================================
 * PATTERN 8: Struct value passing + return
 * ================================================================ */

static void test_struct_passing(void) {
    printf("\n--- Pattern 8: Struct passing ---\n");

    /* 8a: struct passed by value */
    test_e2e(
        "struct Point { u32 x; u32 y; }\n"
        "u32 manhattan(Point p) { return p.x + p.y; }\n"
        "u32 main() {\n"
        "    Point p;\n"
        "    p.x = 10;\n"
        "    p.y = 32;\n"
        "    return manhattan(p) - 42;\n"
        "}\n",
        0, "struct passed by value");

    /* 8b: struct returned from function */
    test_e2e(
        "struct Vec2 { u32 x; u32 y; }\n"
        "Vec2 make_vec(u32 a, u32 b) {\n"
        "    Vec2 v;\n"
        "    v.x = a;\n"
        "    v.y = b;\n"
        "    return v;\n"
        "}\n"
        "u32 main() {\n"
        "    Vec2 v = make_vec(15, 27);\n"
        "    return v.x + v.y - 42;\n"
        "}\n",
        0, "struct returned from function");

    /* 8c: struct with nested struct passed to function */
    test_e2e(
        "struct Config { u32 baud; u32 parity; }\n"
        "struct Device { Config cfg; u32 id; }\n"
        "u32 get_baud(Device d) { return d.cfg.baud; }\n"
        "u32 main() {\n"
        "    Device dev;\n"
        "    dev.cfg.baud = 115200;\n"
        "    dev.cfg.parity = 0;\n"
        "    dev.id = 1;\n"
        "    u32 b = get_baud(dev);\n"
        "    if (b == 115200) { return 0; }\n"
        "    return 1;\n"
        "}\n",
        0, "nested struct passed by value to function");
}

/* ================================================================
 * PATTERN 9: Tagged union dispatch
 * ================================================================ */

static void test_tagged_union_dispatch(void) {
    printf("\n--- Pattern 9: Tagged union dispatch ---\n");

    /* 9a: union construction + switch */
    test_e2e(
        "struct SensorData { u32 temp; }\n"
        "struct Command { u32 code; }\n"
        "union Message {\n"
        "    SensorData sensor;\n"
        "    Command command;\n"
        "}\n"
        "u32 main() {\n"
        "    SensorData sd;\n"
        "    sd.temp = 42;\n"
        "    Message msg;\n"
        "    msg.sensor = sd;\n"
        "    u32 result = 0;\n"
        "    switch (msg) {\n"
        "        .sensor => |data| { result = data.temp; },\n"
        "        .command => |cmd| { result = cmd.code; },\n"
        "    }\n"
        "    return result - 42;\n"
        "}\n",
        0, "union construction + switch dispatch");

    /* 9b: union returned from function + switch at call site */
    test_e2e(
        "struct Ok { u32 val; }\n"
        "struct Err { u32 code; }\n"
        "union Result { Ok ok; Err err; }\n"
        "Result make_ok(u32 v) {\n"
        "    Ok o;\n"
        "    o.val = v;\n"
        "    Result r;\n"
        "    r.ok = o;\n"
        "    return r;\n"
        "}\n"
        "Result make_err(u32 c) {\n"
        "    Err e;\n"
        "    e.code = c;\n"
        "    Result r;\n"
        "    r.err = e;\n"
        "    return r;\n"
        "}\n"
        "u32 main() {\n"
        "    Result r = make_ok(42);\n"
        "    u32 out = 0;\n"
        "    switch (r) {\n"
        "        .ok => |v| { out = v.val; },\n"
        "        .err => |e| { out = e.code + 100; },\n"
        "    }\n"
        "    return out - 42;\n"
        "}\n",
        0, "union returned from function + switch at callsite");

    /* 9c: union error path */
    test_e2e(
        "struct Ok { u32 val; }\n"
        "struct Err { u32 code; }\n"
        "union Result { Ok ok; Err err; }\n"
        "Result make_err(u32 c) {\n"
        "    Err e;\n"
        "    e.code = c;\n"
        "    Result r;\n"
        "    r.err = e;\n"
        "    return r;\n"
        "}\n"
        "u32 main() {\n"
        "    Result r = make_err(5);\n"
        "    u32 out = 0;\n"
        "    switch (r) {\n"
        "        .ok => |v| { out = v.val; },\n"
        "        .err => |e| { out = e.code; },\n"
        "    }\n"
        "    return out - 5;\n"
        "}\n",
        0, "union error path dispatch");
}

/* ================================================================
 * PATTERN 10: Complex combos (real firmware would combine these)
 * ================================================================ */

static void test_combos(void) {
    printf("\n--- Pattern 10: Real firmware combos ---\n");

    /* 10a: pool + defer free on error */
    test_e2e(
        "struct Conn { u32 fd; }\n"
        "Pool(Conn, 4) conns;\n"
        "?u32 try_connect() { return null; }\n"
        "u32 freed = 0;\n"
        "u32 main() {\n"
        "    Handle(Conn) h = conns.alloc() orelse return;\n"
        "    conns.get(h).fd = 42;\n"
        "    u32 result = conns.get(h).fd;\n"
        "    conns.free(h);\n"
        "    return result - 42;\n"
        "}\n",
        0, "pool + defer pattern");

    /* 10b: enum + struct + switch + nested field */
    test_e2e(
        "struct Config { u32 baud; }\n"
        "enum Mode { uart, spi, i2c }\n"
        "u32 get_speed(Mode m, Config c) {\n"
        "    switch (m) {\n"
        "        .uart => { return c.baud; }\n"
        "        .spi => { return c.baud * 2; }\n"
        "        .i2c => { return c.baud / 4; }\n"
        "    }\n"
        "}\n"
        "u32 main() {\n"
        "    Config c;\n"
        "    c.baud = 100;\n"
        "    u32 speed = get_speed(Mode.spi, c);\n"
        "    return speed - 200;\n"
        "}\n",
        0, "enum + struct + switch combo");

    /* 10c: ring + orelse + loop accumulator */
    test_e2e(
        "Ring(u32, 8) fifo;\n"
        "u32 main() {\n"
        "    fifo.push(10);\n"
        "    fifo.push(20);\n"
        "    fifo.push(12);\n"
        "    u32 sum = 0;\n"
        "    for (u32 i = 0; i < 5; i += 1) {\n"
        "        u32 val = fifo.pop() orelse break;\n"
        "        sum += val;\n"
        "    }\n"
        "    return sum - 42;\n"
        "}\n",
        0, "ring drain loop with orelse break");

    /* 10d: nested struct + pointer + orelse */
    test_e2e(
        "struct Sensor { u32 id; u32 reading; }\n"
        "struct System { Sensor primary; Sensor backup; }\n"
        "?u32 read_sensor(*Sensor s) {\n"
        "    if (s.reading == 0) { return null; }\n"
        "    return s.reading;\n"
        "}\n"
        "u32 main() {\n"
        "    System sys;\n"
        "    sys.primary.id = 1;\n"
        "    sys.primary.reading = 0;\n"
        "    sys.backup.id = 2;\n"
        "    sys.backup.reading = 42;\n"
        "    u32 val = read_sensor(&sys.primary) orelse read_sensor(&sys.backup) orelse 0;\n"
        "    return val - 42;\n"
        "}\n",
        0, "nested struct + ptr + chained orelse fallback");
}

/* ================================================================ */

int main(void) {
    printf("=== Firmware Pattern Stress Tests ===\n");

    test_deep_struct_chains();
    test_orelse_error_chains();
    test_pool_driver_pattern();
    test_state_machine();
    test_register_patterns();
    test_defer_complex();
    test_ring_buffer();
    test_struct_passing();
    test_tagged_union_dispatch();
    test_combos();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) printf(", %d FAILED", tests_failed);
    printf(" ===\n");

    remove("_zer_fw_test.c");
    remove("_zer_fw_test.exe");
    remove("_zer_fw_test.o");
    remove("_zer_fw_err.txt");

    return tests_failed > 0 ? 1 : 0;
}
