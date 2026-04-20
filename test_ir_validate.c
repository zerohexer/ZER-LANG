/* ================================================================
 * Adversarial tests for ir_validate.
 *
 * Constructs malformed IRFunc instances directly (bypassing
 * ir_lower.c) and confirms ir_validate() catches them. Closes the
 * gap identified during the 2026-04-20 gap audit: "checks don't
 * false-positive on clean code" is NOT the same as "checks would
 * fire on real bugs." This file tests the latter.
 *
 * Every invalid test should trigger ir_validate to return false
 * AND print a diagnostic to stderr. Every valid test should return
 * true and print nothing.
 *
 * Run: ./test_ir_validate (stderr is expected to contain messages
 * from the invalid cases; stdout shows PASS/FAIL summary only).
 * ================================================================ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ir.h"
#include "types.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static void expect_invalid(IRFunc *f, const char *name) {
    tests_run++;
    fprintf(stderr, "  [expect invalid: %s] ", name);
    bool r = ir_validate(f);
    if (!r) { tests_passed++; fprintf(stderr, "\n"); }
    else {
        printf("  FAIL: %s — expected invalid, validator returned true\n", name);
        tests_failed++;
        fprintf(stderr, "\n");
    }
}

static void expect_valid(IRFunc *f, const char *name) {
    tests_run++;
    fprintf(stderr, "  [expect valid: %s] ", name);
    bool r = ir_validate(f);
    if (r) { tests_passed++; fprintf(stderr, "OK\n"); }
    else {
        printf("  FAIL: %s — expected valid, validator returned false\n", name);
        tests_failed++;
        fprintf(stderr, "\n");
    }
}

/* Zero-init an IRInst with all sentinels set to -1 (matching make_inst
 * in ir_lower.c). */
static IRInst clean_inst(IROpKind op) {
    IRInst i;
    memset(&i, 0, sizeof(i));
    i.op = op;
    i.dest_local = -1;
    i.true_block = -1;
    i.false_block = -1;
    i.goto_block = -1;
    i.cond_local = -1;
    i.src1_local = -1;
    i.src2_local = -1;
    i.obj_local = -1;
    i.handle_local = -1;
    return i;
}

/* Build a minimal valid function: one block with a bare RETURN. */
static IRFunc *make_minimal_func(Arena *arena) {
    IRFunc *f = ir_func_new(arena, "t", 1, ty_u32);
    int b = ir_add_block(f, arena);
    IRInst ret = clean_inst(IR_RETURN);
    ir_block_add_inst(&f->blocks[b], arena, ret);
    return f;
}

/* ================================================================
 * Existing structural checks (baseline from before phase 1)
 * ================================================================ */

static void test_no_blocks(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = ir_func_new(&a, "t", 1, ty_u32);
    /* No blocks added */
    expect_invalid(f, "function with zero basic blocks");
    arena_free(&a);
}

static void test_branch_out_of_range(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    IRInst br = clean_inst(IR_BRANCH);
    br.cond_local = 0; /* give it a condition so phase 1 doesn't catch first */
    br.true_block = 99; /* out of range */
    br.false_block = 0;
    ir_block_add_inst(&f->blocks[0], &a, br);
    expect_invalid(f, "BRANCH with true_block out of range");
    arena_free(&a);
}

static void test_goto_out_of_range(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    IRInst g = clean_inst(IR_GOTO);
    g.goto_block = 42;
    ir_block_add_inst(&f->blocks[0], &a, g);
    expect_invalid(f, "GOTO with target out of range");
    arena_free(&a);
}

static void test_local_out_of_range(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    IRInst c = clean_inst(IR_COPY);
    c.dest_local = 99; /* no locals added */
    c.src1_local = 0;
    ir_block_add_inst(&f->blocks[0], &a, c);
    expect_invalid(f, "COPY with dest_local index beyond local_count");
    arena_free(&a);
}

/* ================================================================
 * Phase 1 — per-op field invariants
 * ================================================================ */

static void test_branch_no_condition(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    IRInst br = clean_inst(IR_BRANCH);
    br.true_block = 0;
    br.false_block = 0;
    /* cond_local left at -1, expr left NULL */
    ir_block_add_inst(&f->blocks[0], &a, br);
    expect_invalid(f, "BRANCH with neither cond_local nor expr");
    arena_free(&a);
}

static void test_binop_missing_operand(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    int l = ir_add_local(f, &a, "x", 1, ty_u32, false, false, true, 1);
    IRInst op = clean_inst(IR_BINOP);
    op.dest_local = l;
    op.src1_local = l;
    /* src2_local left at -1 */
    ir_block_add_inst(&f->blocks[0], &a, op);
    expect_invalid(f, "BINOP with src2_local=-1");
    arena_free(&a);
}

static void test_unop_missing_source(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    int l = ir_add_local(f, &a, "x", 1, ty_u32, false, false, true, 1);
    IRInst op = clean_inst(IR_UNOP);
    op.dest_local = l;
    /* src1_local left at -1 */
    ir_block_add_inst(&f->blocks[0], &a, op);
    expect_invalid(f, "UNOP with src1_local=-1");
    arena_free(&a);
}

static void test_copy_missing_dest(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    int l = ir_add_local(f, &a, "x", 1, ty_u32, false, false, true, 1);
    IRInst op = clean_inst(IR_COPY);
    op.src1_local = l;
    /* dest_local left at -1 */
    ir_block_add_inst(&f->blocks[0], &a, op);
    expect_invalid(f, "COPY with dest_local=-1");
    arena_free(&a);
}

static void test_literal_no_dest(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    IRInst lit = clean_inst(IR_LITERAL);
    /* dest_local left at -1 */
    ir_block_add_inst(&f->blocks[0], &a, lit);
    expect_invalid(f, "LITERAL with no dest_local");
    arena_free(&a);
}

static void test_field_read_no_field_name(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    int l = ir_add_local(f, &a, "x", 1, ty_u32, false, false, true, 1);
    IRInst fr = clean_inst(IR_FIELD_READ);
    fr.dest_local = l;
    fr.src1_local = l;
    /* field_name left NULL */
    ir_block_add_inst(&f->blocks[0], &a, fr);
    expect_invalid(f, "FIELD_READ with NULL field_name");
    arena_free(&a);
}

static void test_index_read_missing_src2(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    int l = ir_add_local(f, &a, "x", 1, ty_u32, false, false, true, 1);
    IRInst ir = clean_inst(IR_INDEX_READ);
    ir.dest_local = l;
    ir.src1_local = l;
    /* src2_local left at -1 */
    ir_block_add_inst(&f->blocks[0], &a, ir);
    expect_invalid(f, "INDEX_READ with src2_local=-1");
    arena_free(&a);
}

static void test_cast_null_cast_type(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    int l = ir_add_local(f, &a, "x", 1, ty_u32, false, false, true, 1);
    IRInst c = clean_inst(IR_CAST);
    c.dest_local = l;
    c.src1_local = l;
    /* cast_type left NULL */
    ir_block_add_inst(&f->blocks[0], &a, c);
    expect_invalid(f, "CAST with NULL cast_type");
    arena_free(&a);
}

/* ================================================================
 * Phase 2 — defer balance + NULL-type-local
 * ================================================================ */

static void test_defer_push_without_fire(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    /* Add a DEFER_PUSH BEFORE the RETURN — there's no FIRE anywhere */
    IRInst push = clean_inst(IR_DEFER_PUSH);
    /* Add it to block[0] before the existing RETURN. Simulate: */
    /* Reset block 0: clear and re-add in order */
    f->blocks[0].inst_count = 0;
    ir_block_add_inst(&f->blocks[0], &a, push);
    IRInst ret = clean_inst(IR_RETURN);
    ir_block_add_inst(&f->blocks[0], &a, ret);
    expect_invalid(f, "DEFER_PUSH with no CFG-reachable DEFER_FIRE");
    arena_free(&a);
}

static void test_null_type_local(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    /* Manually add a local with NULL type */
    ir_add_local(f, &a, "bad", 3, NULL, false, false, true, 1);
    expect_invalid(f, "local with NULL type");
    arena_free(&a);
}

/* ================================================================
 * Positive controls — must remain valid
 * ================================================================ */

static void test_minimal_valid(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    expect_valid(f, "minimal function: single block with bare RETURN");
    arena_free(&a);
}

static void test_defer_push_fire_same_block(void) {
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    f->blocks[0].inst_count = 0;
    IRInst push = clean_inst(IR_DEFER_PUSH);
    ir_block_add_inst(&f->blocks[0], &a, push);
    IRInst fire = clean_inst(IR_DEFER_FIRE);
    /* src2_local = -1 (default) means fire-all-no-pop — emit_bodies=true */
    ir_block_add_inst(&f->blocks[0], &a, fire);
    IRInst ret = clean_inst(IR_RETURN);
    ir_block_add_inst(&f->blocks[0], &a, ret);
    expect_valid(f, "DEFER_PUSH followed by DEFER_FIRE in same block");
    arena_free(&a);
}

static void test_defer_fire_pop_only_does_not_count(void) {
    /* src2_local == 2 is "pop-only" — doesn't emit bodies.
     * PUSH without a real emit-bodies FIRE = dead defer. */
    Arena a; arena_init(&a, 16*1024);
    IRFunc *f = make_minimal_func(&a);
    f->blocks[0].inst_count = 0;
    IRInst push = clean_inst(IR_DEFER_PUSH);
    ir_block_add_inst(&f->blocks[0], &a, push);
    IRInst fire = clean_inst(IR_DEFER_FIRE);
    fire.src2_local = 2; /* pop-only — doesn't count as fire */
    ir_block_add_inst(&f->blocks[0], &a, fire);
    IRInst ret = clean_inst(IR_RETURN);
    ir_block_add_inst(&f->blocks[0], &a, ret);
    expect_invalid(f, "DEFER_PUSH followed only by pop-only FIRE (dead defer)");
    arena_free(&a);
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void) {
    Arena a; arena_init(&a, 4*1024);
    types_init(&a);
    arena_free(&a);

    printf("=== ir_validate adversarial tests ===\n");

    /* Stderr is noisy — that's expected. The validator prints
     * diagnostics for each malformed IR we feed it. */

    /* Baseline structural */
    test_no_blocks();
    test_branch_out_of_range();
    test_goto_out_of_range();
    test_local_out_of_range();

    /* Phase 1 — per-op invariants */
    test_branch_no_condition();
    test_binop_missing_operand();
    test_unop_missing_source();
    test_copy_missing_dest();
    test_literal_no_dest();
    test_field_read_no_field_name();
    test_index_read_missing_src2();
    test_cast_null_cast_type();

    /* Phase 2 */
    test_defer_push_without_fire();
    test_null_type_local();

    /* Positive controls */
    test_minimal_valid();
    test_defer_push_fire_same_block();
    test_defer_fire_pop_only_does_not_count();

    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
