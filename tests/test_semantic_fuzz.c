/* Semantic fuzzer for ZER-LANG zercheck pipeline.
 * Generates random valid ZER programs combining safety-critical patterns,
 * compiles with zerc, runs, and verifies:
 * - Safe programs: must compile AND run (exit 0)
 * - Unsafe programs: must be REJECTED by zerc (exit != 0)
 *
 * Patterns combined: alloc, free, defer, cast, interior ptr, *opaque,
 * arena wrapper, goto, Handle alias, function boundary.
 *
 * Usage: ./test_semantic_fuzz [seed] [count]
 *        Default: seed=42, count=200
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned int rng_state;
static int rng(int max) {
    rng_state = rng_state * 1103515245 + 12345;
    return (rng_state >> 16) % max;
}

static int passed = 0, failed = 0, total = 0;

static const char *zerc_path = NULL;

static void find_zerc(void) {
    /* Try common locations */
    if (system("./zerc --help >/dev/null 2>&1") == 0) { zerc_path = "./zerc"; return; }
    if (system("/tmp/zerc --help >/dev/null 2>&1") == 0) { zerc_path = "/tmp/zerc"; return; }
    /* Build it if needed */
    if (system("gcc -std=c99 -O2 -I. -o /tmp/zerc lexer.c parser.c ast.c types.c checker.c emitter.c zercheck.c zerc_main.c 2>/dev/null") == 0) {
        zerc_path = "/tmp/zerc";
        return;
    }
    fprintf(stderr, "ERROR: cannot find or build zerc\n");
    exit(1);
}

static void run_test(const char *name, const char *code, int expect_fail) {
    total++;
    FILE *f = fopen("/tmp/_zer_fuzz.zer", "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return; }
    fputs(code, f);
    fclose(f);

    char cmd[512];
    int result;

    if (expect_fail) {
        snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_fuzz.zer -o /dev/null 2>/dev/null", zerc_path);
        result = system(cmd);
        if (result != 0) {
            passed++;
        } else {
            failed++;
            fprintf(stderr, "  FAIL: %s — expected rejection but compiled OK\n", name);
            fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        }
    } else {
        snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_fuzz.zer --run 2>/dev/null", zerc_path);
        result = system(cmd);
        if (result == 0) {
            passed++;
        } else {
            snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_fuzz.zer -o /dev/null 2>/dev/null", zerc_path);
            int compile_result = system(cmd);
            if (compile_result != 0) {
                failed++;
                fprintf(stderr, "  FAIL: %s — safe program rejected by compiler\n", name);
                snprintf(cmd, sizeof(cmd), "%s /tmp/_zer_fuzz.zer -o /dev/null 2>&1 | head -5", zerc_path);
                system(cmd);
            } else {
                failed++;
                fprintf(stderr, "  FAIL: %s — compiled but crashed at runtime!\n", name);
            }
            fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
        }
    }
}

/* ---- Safe program generators ---- */

static void gen_safe_arena_chain(char *buf, int depth) {
    char *p = buf;
    p += sprintf(p, "struct Block%d { u32 a; u32 b; }\n", depth);
    p += sprintf(p, "Arena ar%d;\n", depth);

    /* Build wrapper chain */
    for (int i = 0; i < depth; i++) {
        if (i == 0) {
            p += sprintf(p, "?*Block%d wrap%d_%d() { return ar%d.alloc(Block%d); }\n",
                depth, depth, i, depth, depth);
        } else {
            p += sprintf(p, "?*Block%d wrap%d_%d() { return wrap%d_%d(); }\n",
                depth, depth, i, depth, i - 1);
        }
    }

    p += sprintf(p, "u32 test_arena_chain_%d() {\n", depth);
    p += sprintf(p, "    ?*Block%d mb = wrap%d_%d();\n", depth, depth, depth - 1);
    p += sprintf(p, "    *Block%d b = mb orelse return;\n", depth);
    p += sprintf(p, "    b.a = %d;\n", depth * 10);
    p += sprintf(p, "    if (b.a != %d) { return 1; }\n", depth * 10);
    p += sprintf(p, "    defer ar%d.reset();\n", depth);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_pool_defer(char *buf, int id, int handle_count) {
    char *p = buf;
    if (handle_count > 4) handle_count = 4;
    p += sprintf(p, "struct Item%d { u32 val; }\n", id);
    p += sprintf(p, "Pool(Item%d, 8) pool%d;\n", id, id);
    p += sprintf(p, "u32 test_pool_defer_%d() {\n", id);
    for (int i = 0; i < handle_count; i++) {
        p += sprintf(p, "    ?Handle(Item%d) mh%d = pool%d.alloc();\n", id, i, id);
        p += sprintf(p, "    Handle(Item%d) h%d = mh%d orelse return;\n", id, i, i);
        p += sprintf(p, "    defer pool%d.free(h%d);\n", id, i);
        p += sprintf(p, "    pool%d.get(h%d).val = %d;\n", id, i, (i + 1) * 11);
    }
    p += sprintf(p, "    u32 sum = 0;\n");
    for (int i = 0; i < handle_count; i++) {
        p += sprintf(p, "    sum += pool%d.get(h%d).val;\n", id, i);
    }
    int expected = 0;
    for (int i = 0; i < handle_count; i++) expected += (i + 1) * 11;
    p += sprintf(p, "    if (sum != %d) { return 1; }\n", expected);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_opaque_roundtrip(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Src%d { u32 x; u32 y; }\n", id);
    p += sprintf(p, "Slab(Src%d) slab%d;\n", id, id);
    p += sprintf(p, "*Src%d unwrap%d(*opaque raw) { return (*Src%d)raw; }\n", id, id, id);
    p += sprintf(p, "u32 test_opaque_%d() {\n", id);
    p += sprintf(p, "    ?*Src%d ms = slab%d.alloc_ptr();\n", id, id);
    p += sprintf(p, "    *Src%d s = ms orelse return;\n", id);
    p += sprintf(p, "    defer slab%d.free_ptr(s);\n", id);
    p += sprintf(p, "    s.x = %d; s.y = %d;\n", id * 3, id * 7);
    p += sprintf(p, "    *opaque raw = (*opaque)s;\n");
    p += sprintf(p, "    *Src%d back = unwrap%d(raw);\n", id, id);
    p += sprintf(p, "    if (back.x != %d) { return 1; }\n", id * 3);
    p += sprintf(p, "    if (back.y != %d) { return 2; }\n", id * 7);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_cast_chain(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "u32 test_cast_%d() {\n", id);
    unsigned val = 1000 + id * 137;
    p += sprintf(p, "    u32 big = %u;\n", val);
    p += sprintf(p, "    u16 mid = (u16)big;\n");
    p += sprintf(p, "    u8 small = (u8)mid;\n");
    p += sprintf(p, "    u32 wide = (u32)small;\n");
    unsigned expected = (unsigned char)(unsigned short)val;
    p += sprintf(p, "    if (wide != %u) { return 1; }\n", expected);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_interior_ptr(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Blk%d { u32 a; u32 b; u32 c; }\n", id);
    p += sprintf(p, "Slab(Blk%d) heap%d;\n", id, id);
    p += sprintf(p, "u32 test_interior_%d() {\n", id);
    p += sprintf(p, "    ?*Blk%d mb = heap%d.alloc_ptr();\n", id, id);
    p += sprintf(p, "    *Blk%d b = mb orelse return;\n", id);
    p += sprintf(p, "    b.b = %d;\n", id * 13);
    p += sprintf(p, "    *u32 p = &b.b;\n");
    p += sprintf(p, "    u32 val = p[0];\n");  /* use BEFORE free — safe */
    p += sprintf(p, "    heap%d.free_ptr(b);\n", id);
    p += sprintf(p, "    if (val != %d) { return 1; }\n", id * 13);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

/* ---- Unsafe program generators (must be rejected) ---- */

static void gen_unsafe_uaf(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Uaf%d { u32 v; }\n", id);
    p += sprintf(p, "Pool(Uaf%d, 4) upool%d;\n", id, id);
    p += sprintf(p, "u32 test_uaf_%d() {\n", id);
    p += sprintf(p, "    ?Handle(Uaf%d) mh = upool%d.alloc();\n", id, id);
    p += sprintf(p, "    Handle(Uaf%d) h = mh orelse return;\n", id);
    p += sprintf(p, "    upool%d.get(h).v = 42;\n", id);
    p += sprintf(p, "    upool%d.free(h);\n", id);
    p += sprintf(p, "    u32 val = upool%d.get(h).v;\n", id); /* UAF */
    p += sprintf(p, "    return val;\n");
    p += sprintf(p, "}\n");
}

static void gen_unsafe_interior_uaf(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct IUaf%d { u32 a; u32 b; }\n", id);
    p += sprintf(p, "Slab(IUaf%d) iheap%d;\n", id, id);
    p += sprintf(p, "u32 test_iuaf_%d() {\n", id);
    p += sprintf(p, "    ?*IUaf%d mb = iheap%d.alloc_ptr();\n", id, id);
    p += sprintf(p, "    *IUaf%d b = mb orelse return;\n", id);
    p += sprintf(p, "    b.a = 42;\n");
    p += sprintf(p, "    *u32 p = &b.a;\n");
    p += sprintf(p, "    iheap%d.free_ptr(b);\n", id);
    p += sprintf(p, "    u32 val = p[0];\n"); /* UAF via interior ptr */
    p += sprintf(p, "    return val;\n");
    p += sprintf(p, "}\n");
}

static void gen_unsafe_double_free(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Df%d { u32 v; }\n", id);
    p += sprintf(p, "Pool(Df%d, 4) dfpool%d;\n", id, id);
    p += sprintf(p, "u32 test_df_%d() {\n", id);
    p += sprintf(p, "    ?Handle(Df%d) mh = dfpool%d.alloc();\n", id, id);
    p += sprintf(p, "    Handle(Df%d) h = mh orelse return;\n", id);
    p += sprintf(p, "    dfpool%d.free(h);\n", id);
    p += sprintf(p, "    dfpool%d.free(h);\n", id); /* double free */
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_unsafe_leak(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Lk%d { u32 v; }\n", id);
    p += sprintf(p, "Pool(Lk%d, 4) lkpool%d;\n", id, id);
    p += sprintf(p, "u32 test_leak_%d() {\n", id);
    p += sprintf(p, "    ?Handle(Lk%d) mh = lkpool%d.alloc();\n", id, id);
    p += sprintf(p, "    Handle(Lk%d) h = mh orelse return;\n", id);
    p += sprintf(p, "    lkpool%d.get(h).v = 42;\n", id);
    /* no free — leak */
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_unsafe_cast_wrong_type(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct CastA%d { u32 x; }\n", id);
    p += sprintf(p, "struct CastB%d { u32 y; }\n", id);
    p += sprintf(p, "u32 test_cast_wrong_%d() {\n", id);
    p += sprintf(p, "    CastA%d a; a.x = 42;\n", id);
    p += sprintf(p, "    *CastA%d pa = &a;\n", id);
    p += sprintf(p, "    *CastB%d pb = (*CastB%d)pa;\n", id, id); /* direct *A→*B */
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_unsafe_provenance(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct ProvA%d { u32 x; }\n", id);
    p += sprintf(p, "struct ProvB%d { u32 y; }\n", id);
    p += sprintf(p, "Slab(ProvA%d) prslab%d;\n", id, id);
    p += sprintf(p, "u32 test_prov_%d() {\n", id);
    p += sprintf(p, "    ?*ProvA%d ma = prslab%d.alloc_ptr();\n", id, id);
    p += sprintf(p, "    *ProvA%d a = ma orelse return;\n", id);
    p += sprintf(p, "    defer prslab%d.free_ptr(a);\n", id);
    p += sprintf(p, "    *opaque raw = (*opaque)a;\n");
    p += sprintf(p, "    *ProvB%d wrong = (*ProvB%d)raw;\n", id, id); /* wrong provenance */
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

/* ---- New generators: goto, comptime, handle alias, enum, while+break ---- */

static void gen_safe_goto_defer(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Gd%d { u32 v; }\n", id);
    p += sprintf(p, "Pool(Gd%d, 4) gdpool%d;\n", id, id);
    p += sprintf(p, "u32 test_goto_defer_%d() {\n", id);
    p += sprintf(p, "    ?Handle(Gd%d) mh = gdpool%d.alloc();\n", id, id);
    p += sprintf(p, "    Handle(Gd%d) h = mh orelse return;\n", id);
    p += sprintf(p, "    defer gdpool%d.free(h);\n", id);
    p += sprintf(p, "    gdpool%d.get(h).v = %d;\n", id, id * 7);
    p += sprintf(p, "    goto done%d;\n", id);
    p += sprintf(p, "    gdpool%d.get(h).v = 0;\n", id);
    p += sprintf(p, "done%d:\n", id);
    p += sprintf(p, "    if (gdpool%d.get(h).v != %d) { return 1; }\n", id, id * 7);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_comptime(char *buf, int id) {
    char *p = buf;
    unsigned a = 3 + (id % 10);
    unsigned b = 7 + (id % 5);
    unsigned max = a > b ? a : b;
    p += sprintf(p, "comptime u32 MAX%d(u32 a, u32 b) {\n", id);
    p += sprintf(p, "    if (a > b) { return a; }\n");
    p += sprintf(p, "    return b;\n");
    p += sprintf(p, "}\n");
    p += sprintf(p, "u32 test_comptime_%d() {\n", id);
    p += sprintf(p, "    u32 val = MAX%d(%u, %u);\n", id, a, b);
    p += sprintf(p, "    if (val != %u) { return 1; }\n", max);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_handle_alias(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Ha%d { u32 v; }\n", id);
    p += sprintf(p, "Pool(Ha%d, 4) hapool%d;\n", id, id);
    p += sprintf(p, "u32 test_halias_%d() {\n", id);
    p += sprintf(p, "    ?Handle(Ha%d) mh = hapool%d.alloc();\n", id, id);
    p += sprintf(p, "    Handle(Ha%d) h1 = mh orelse return;\n", id);
    p += sprintf(p, "    Handle(Ha%d) h2 = h1;\n", id);
    p += sprintf(p, "    hapool%d.get(h1).v = %d;\n", id, id * 3);
    p += sprintf(p, "    u32 val = hapool%d.get(h2).v;\n", id);
    p += sprintf(p, "    hapool%d.free(h1);\n", id);
    p += sprintf(p, "    if (val != %d) { return 1; }\n", id * 3);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_enum_switch(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "enum Cmd%d { start, stop, reset }\n", id);
    p += sprintf(p, "u32 run_cmd%d(Cmd%d c) {\n", id, id);
    p += sprintf(p, "    switch (c) {\n");
    p += sprintf(p, "        .start => { return 1; }\n");
    p += sprintf(p, "        .stop => { return 2; }\n");
    p += sprintf(p, "        .reset => { return 3; }\n");
    p += sprintf(p, "    }\n");
    p += sprintf(p, "}\n");
    p += sprintf(p, "u32 test_enum_%d() {\n", id);
    p += sprintf(p, "    if (run_cmd%d(Cmd%d.start) != 1) { return 1; }\n", id, id);
    p += sprintf(p, "    if (run_cmd%d(Cmd%d.stop) != 2) { return 2; }\n", id, id);
    p += sprintf(p, "    if (run_cmd%d(Cmd%d.reset) != 3) { return 3; }\n", id, id);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_while_break(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Wb%d { u32 count; }\n", id);
    p += sprintf(p, "Pool(Wb%d, 4) wbpool%d;\n", id, id);
    p += sprintf(p, "u32 test_while_%d() {\n", id);
    p += sprintf(p, "    ?Handle(Wb%d) mh = wbpool%d.alloc();\n", id, id);
    p += sprintf(p, "    Handle(Wb%d) h = mh orelse return;\n", id);
    p += sprintf(p, "    defer wbpool%d.free(h);\n", id);
    p += sprintf(p, "    wbpool%d.get(h).count = 0;\n", id);
    int limit = 5 + (id % 20);
    int brk = 2 + (id % (limit - 1));
    p += sprintf(p, "    u32 i = 0;\n");
    p += sprintf(p, "    while (i < %d) {\n", limit);
    p += sprintf(p, "        wbpool%d.get(h).count = i;\n", id);
    p += sprintf(p, "        i += 1;\n");
    p += sprintf(p, "        if (i == %d) { break; }\n", brk);
    p += sprintf(p, "    }\n");
    p += sprintf(p, "    if (wbpool%d.get(h).count != %d) { return 1; }\n", id, brk - 1);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_task_new(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Tk%d { u32 id; u32 priority; }\n", id);
    p += sprintf(p, "u32 test_task_%d() {\n", id);
    p += sprintf(p, "    ?*Tk%d mt = Tk%d.new_ptr();\n", id, id);
    p += sprintf(p, "    *Tk%d t = mt orelse return;\n", id);
    p += sprintf(p, "    t.id = %d;\n", id);
    p += sprintf(p, "    t.priority = %d;\n", id % 5);
    p += sprintf(p, "    if (t.id != %d) { return 1; }\n", id);
    p += sprintf(p, "    Tk%d.delete_ptr(t);\n", id);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_safe_funcptr_callback(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "u32 double%d(u32 x) { return x + x; }\n", id);
    p += sprintf(p, "u32 triple%d(u32 x) { return x + x + x; }\n", id);
    p += sprintf(p, "u32 apply%d(u32 (*op)(u32), u32 val) { return op(val); }\n", id);
    p += sprintf(p, "u32 test_funcptr_%d() {\n", id);
    unsigned val = 10 + id;
    p += sprintf(p, "    u32 a = apply%d(double%d, %u);\n", id, id, val);
    p += sprintf(p, "    u32 b = apply%d(triple%d, %u);\n", id, id, val);
    p += sprintf(p, "    if (a != %u) { return 1; }\n", val * 2);
    p += sprintf(p, "    if (b != %u) { return 2; }\n", val * 3);
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

static void gen_unsafe_alias_uaf(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Au%d { u32 v; }\n", id);
    p += sprintf(p, "Pool(Au%d, 4) aupool%d;\n", id, id);
    p += sprintf(p, "u32 test_alias_uaf_%d() {\n", id);
    p += sprintf(p, "    ?Handle(Au%d) mh = aupool%d.alloc();\n", id, id);
    p += sprintf(p, "    Handle(Au%d) h1 = mh orelse return;\n", id);
    p += sprintf(p, "    Handle(Au%d) h2 = h1;\n", id);
    p += sprintf(p, "    aupool%d.free(h1);\n", id);
    p += sprintf(p, "    u32 val = aupool%d.get(h2).v;\n", id); /* UAF via alias */
    p += sprintf(p, "    return val;\n");
    p += sprintf(p, "}\n");
}

static void gen_unsafe_goto_uaf(char *buf, int id) {
    char *p = buf;
    p += sprintf(p, "struct Gu%d { u32 v; }\n", id);
    p += sprintf(p, "Pool(Gu%d, 4) gupool%d;\n", id, id);
    p += sprintf(p, "u32 test_goto_uaf_%d() {\n", id);
    p += sprintf(p, "    ?Handle(Gu%d) mh = gupool%d.alloc();\n", id, id);
    p += sprintf(p, "    Handle(Gu%d) h = mh orelse return;\n", id);
    p += sprintf(p, "    u32 count = 0;\n");
    p += sprintf(p, "retry%d:\n", id);
    p += sprintf(p, "    gupool%d.get(h).v = count;\n", id);
    p += sprintf(p, "    gupool%d.free(h);\n", id);
    p += sprintf(p, "    goto retry%d;\n", id); /* backward goto after free */
    p += sprintf(p, "    return 0;\n");
    p += sprintf(p, "}\n");
}

int main(int argc, char **argv) {
    unsigned seed = argc > 1 ? (unsigned)atoi(argv[1]) : 42;
    int count = argc > 2 ? atoi(argv[2]) : 200;
    rng_state = seed;

    find_zerc();
    printf("=== ZER Semantic Fuzzer ===\n");
    printf("Seed: %u, Count: %d\n\n", seed, count);

    char prog[8192];
    char name[128];

    for (int i = 0; i < count; i++) {
        char decls[4096] = "";
        char calls[2048] = "";
        char *cp = calls;
        int pattern = rng(18);

        switch (pattern) {
        case 0: case 1: { /* safe arena chain */
            int depth = 1 + rng(4);
            gen_safe_arena_chain(decls, depth);
            cp += sprintf(cp, "    if (test_arena_chain_%d() != 0) { return 1; }\n", depth);
            snprintf(name, sizeof(name), "safe_arena_chain_%d_depth%d", i, depth);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 2: case 3: { /* safe pool + defer */
            int hcount = 1 + rng(4);
            gen_safe_pool_defer(decls, i, hcount);
            cp += sprintf(cp, "    if (test_pool_defer_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_pool_defer_%d_h%d", i, hcount);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 4: { /* safe opaque roundtrip */
            gen_safe_opaque_roundtrip(decls, i);
            cp += sprintf(cp, "    if (test_opaque_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_opaque_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 5: { /* safe cast chain */
            gen_safe_cast_chain(decls, i);
            cp += sprintf(cp, "    if (test_cast_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_cast_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 6: { /* safe interior ptr (use before free) */
            gen_safe_interior_ptr(decls, i);
            cp += sprintf(cp, "    if (test_interior_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_interior_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 7: { /* unsafe UAF or interior UAF */
            if (rng(2)) {
                gen_unsafe_uaf(decls, i);
                cp += sprintf(cp, "    return test_uaf_%d();\n", i);
                snprintf(name, sizeof(name), "unsafe_uaf_%d", i);
            } else {
                gen_unsafe_interior_uaf(decls, i);
                cp += sprintf(cp, "    return test_iuaf_%d();\n", i);
                snprintf(name, sizeof(name), "unsafe_interior_uaf_%d", i);
            }
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s}\n", decls, calls);
            run_test(name, prog, 1);
            break;
        }
        case 8: { /* unsafe double free or leak */
            if (rng(2)) {
                gen_unsafe_double_free(decls, i);
                cp += sprintf(cp, "    return test_df_%d();\n", i);
                snprintf(name, sizeof(name), "unsafe_double_free_%d", i);
            } else {
                gen_unsafe_leak(decls, i);
                cp += sprintf(cp, "    return test_leak_%d();\n", i);
                snprintf(name, sizeof(name), "unsafe_leak_%d", i);
            }
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s}\n", decls, calls);
            run_test(name, prog, 1);
            break;
        }
        case 9: { /* unsafe cast wrong type or provenance */
            if (rng(2)) {
                gen_unsafe_cast_wrong_type(decls, i);
                cp += sprintf(cp, "    return test_cast_wrong_%d();\n", i);
                snprintf(name, sizeof(name), "unsafe_cast_wrong_%d", i);
            } else {
                gen_unsafe_provenance(decls, i);
                cp += sprintf(cp, "    return test_prov_%d();\n", i);
                snprintf(name, sizeof(name), "unsafe_provenance_%d", i);
            }
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s}\n", decls, calls);
            run_test(name, prog, 1);
            break;
        }
        case 10: { /* safe goto + defer */
            gen_safe_goto_defer(decls, i);
            cp += sprintf(cp, "    if (test_goto_defer_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_goto_defer_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 11: { /* safe comptime */
            gen_safe_comptime(decls, i);
            cp += sprintf(cp, "    if (test_comptime_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_comptime_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 12: { /* safe handle alias */
            gen_safe_handle_alias(decls, i);
            cp += sprintf(cp, "    if (test_halias_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_handle_alias_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 13: { /* safe enum switch */
            gen_safe_enum_switch(decls, i);
            cp += sprintf(cp, "    if (test_enum_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_enum_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 14: { /* safe while + break */
            gen_safe_while_break(decls, i);
            cp += sprintf(cp, "    if (test_while_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_while_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 15: { /* safe Task.new */
            gen_safe_task_new(decls, i);
            cp += sprintf(cp, "    if (test_task_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_task_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 16: { /* safe function pointer callback */
            gen_safe_funcptr_callback(decls, i);
            cp += sprintf(cp, "    if (test_funcptr_%d() != 0) { return 1; }\n", i);
            snprintf(name, sizeof(name), "safe_funcptr_%d", i);
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s    return 0;\n}\n", decls, calls);
            run_test(name, prog, 0);
            break;
        }
        case 17: { /* unsafe alias UAF or goto UAF */
            if (rng(2)) {
                gen_unsafe_alias_uaf(decls, i);
                cp += sprintf(cp, "    return test_alias_uaf_%d();\n", i);
                snprintf(name, sizeof(name), "unsafe_alias_uaf_%d", i);
            } else {
                gen_unsafe_goto_uaf(decls, i);
                cp += sprintf(cp, "    return test_goto_uaf_%d();\n", i);
                snprintf(name, sizeof(name), "unsafe_goto_uaf_%d", i);
            }
            snprintf(prog, sizeof(prog), "%s\nu32 main() {\n%s}\n", decls, calls);
            run_test(name, prog, 1);
            break;
        }
        }
    }

    printf("\n=== Semantic Fuzz Results ===\n");
    printf("  Passed: %d\n", passed);
    printf("  Failed: %d\n", failed);
    printf("  Total:  %d\n", total);
    if (failed == 0) printf("ALL SEMANTIC FUZZ TESTS PASSED\n");

    return failed > 0 ? 1 : 0;
}
