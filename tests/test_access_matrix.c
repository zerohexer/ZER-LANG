/* test_access_matrix.c — COVERAGE of the data-race judgement (2026-11-15).
 *
 * race_free.v proves: every two conflicting ACCESSES are ordered => no race.
 * What it cannot prove is that the compiler SEES every access — that each
 * construct which touches memory becomes an access node of the graph it judges.
 * Every accepted race of BUG-1666..1675 was that: a construct with no record
 * (a builtin method, a static local, a @once body, an asm operand, a poll, a
 * plain access of an atomic cell). Each was found by a probe somebody thought of.
 *
 * This grid is the enumeration, so that the next one is found by a red cell:
 *
 *   ROWS    — every FORM in which a ZER program touches an object: each node
 *             kind in each operand position, each value shape (scalar, field,
 *             element, optional, union, slice, pointee, container, handle),
 *             each route to it (name, local pointer, global pointer, parameter,
 *             returned pointer, pointer in a field, funcptr), each capture, each
 *             intrinsic that reads or writes through an argument.
 *   COLUMNS — every way two threads can meet over it (Ctx, below).
 *
 * A cell is a whole program. The row's form runs in one thread, a plain access
 * of the SAME object in the other; the verdict expected follows from the
 * definition of a data race alone (expect_reject) — never from what the
 * compiler does today.
 *
 * THE TIE TO THE AST: kind_role() is a switch over NodeKind with NO default.
 * A new node kind fails the build here (-Werror=switch) until it is classified;
 * classified KR_CARRIES, main() refuses to run until a row names it.
 *
 * Two polarities per form, so that a rejection is known to be FOR the race:
 * the same two accesses under one lock (CX_LOCKED) must compile. A form the
 * compiler refuses even when it is safe is listed in `over` — asserted to be
 * REJECTED, so the day precision improves the cell goes red and the bit is
 * cleared (the tests/zer_gaps discipline: an expectation, never a skip).
 *
 * INTEGRITY: a negative counts only if an `error:` LINE names a race. Warnings
 * are ignored — the downgraded spawn-scan finding prints "data race" on every
 * program here, so a substring test over the whole stderr would pass anything.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "zer_tmp.h"

static const char *zerc_path = NULL;
static void find_zerc(void) {
    const char *env = getenv("ZER_MATRIX_ZERC");
    if (env && *env) { zerc_path = env; return; }
    if (system("test -x ./zerc") == 0) { zerc_path = "./zerc"; return; }
    fprintf(stderr, "ERROR: cannot find ./zerc (or set ZER_MATRIX_ZERC)\n");
    exit(2);
}

/* ---- columns: how two threads meet ---- */
typedef enum {
    CX_PAIR_W,    /* thread{form}  ||  another thread{plain write}            */
    CX_PAIR_R,    /* thread{form}  ||  another thread{plain read}             */
    CX_SELF,      /* two instances of thread{form}                            */
    CX_THR_WIN,   /* thread{form}  ||  its spawner, between spawn and join    */
    CX_WIN_THR,   /* spawner{form} between spawn and join  ||  thread{write}  */
    CX_HELPER,    /* thread{ h() }, h{form}  ||  another thread{plain write}  */
    CX_GRAND,     /* thread{ spawn thread{form}; join }  ||  thread{write}    */
    CX_DETACH,    /* detached thread{form}  ||  the code after the spawn      */
    CX_COARSE,    /* CX_PAIR_W with no @atomic anywhere: the spawn scan's own */
    CX_WIN_CALL,  /* thread{form}  ||  a function its spawner calls meanwhile */
    CX_LOCKED,    /* CX_PAIR_W, both accesses under ONE lock                  */
    CX_AFTER,     /* thread{form}, joined; THEN the spawner writes            */
    CX_SEQ,       /* thread{form}, joined; THEN a second thread{write}        */
    CX__N
} Ctx;
#define B(c) (1u << (c))

static const char *ctx_name(Ctx c) {
    switch (c) {
    case CX_PAIR_W:  return "thr|thr-write";
    case CX_PAIR_R:  return "thr|thr-read";
    case CX_SELF:    return "thr|same-thr";
    case CX_THR_WIN: return "thr|window";
    case CX_WIN_THR: return "window|thr";
    case CX_HELPER:  return "helper|thr";
    case CX_GRAND:   return "grandchild|thr";
    case CX_DETACH:  return "detached|after";
    case CX_COARSE:  return "no-atomic";
    case CX_WIN_CALL:return "thr|window-callee";
    case CX_LOCKED:  return "locked";
    case CX_AFTER:   return "thr;then-spawner";
    case CX_SEQ:     return "thr;then-thr";
    case CX__N:      break;
    }
    return "?";
}

/* ---- rows ---- */
typedef struct {
    const char *name;
    NodeKind    car;     /* the node kind whose handling this form exercises   */
    int         writes;  /* the form WRITES the object (else it only reads it) */
    const char *decl;    /* the object and any helper functions                */
    const char *stmt;    /* the form: statements valid in any function body    */
    const char *wpart;   /* a plain WRITE of the same object                   */
    const char *rpart;   /* a plain READ of the same object                    */
    const char *init;    /* run by main before any spawn (NULL: nothing)       */
    unsigned    skip;    /* contexts that cannot host the form (say why)       */
    unsigned    over;    /* accept-expected contexts the compiler refuses      */
    unsigned    excused; /* reject-expected contexts that are NOT a race here  */
    /* the form reaches the object through a PARAMETER of its thread */
    const char *tsig;    /* "*u32 p"  — the thread's parameter (NULL: none)    */
    const char *targ;    /* "&g"      — what the spawn passes                  */
    const char *tfwd;    /* "p"       — how a helper is handed it on           */
} Form;

/* the commonest object */
#define G_DECL  "u32 g;\n"
#define G_W     "g = 7;"
#define G_R     "u32 rv = g; sink(rv);"
#define G_FORM(nm, car, wr, extra_decl, stmt) \
    { nm, car, wr, G_DECL extra_decl, stmt, G_W, G_R, NULL, 0, 0, 0 }

#define S_DECL  "struct S { u32 a; u32 b; }\nS gs;\n"
#define S_W     "gs.a = 7;"
#define S_R     "u32 rv = gs.a; sink(rv);"
#define A_DECL  "u32[4] ga;\n"
#define A_W     "ga[1] = 7;"
#define A_R     "u32 rv = ga[1]; sink(rv);"
#define O_DECL  "?u32 go;\n"
#define O_W     "go = 7;"
#define O_R     "u32 rv = go orelse 0; sink(rv);"
#define U_DECL  "union U { u32 a; u32 b; }\nU gu;\n"
#define U_W     "gu.a = 7;"
#define U_R     "switch (gu) { .a => |v| { sink(v); } .b => |v| { sink(v); } }"
#define P_DECL  "struct T { u32 v; }\nPool(T, 4) gpool;\n"
#define P_W     "?Handle(T) wh = gpool.alloc(); if (wh) |x| { gpool.free(x); }"
#define NAKED_NEVER(call) "volatile u32 never = 0; if (never == 42) { " call " }"

/* a row names its thread parameter only when it has one */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const Form forms[] = {
    /* ================= a READ of a scalar, in every operand position ======== */
    G_FORM("read: var-decl init",        NODE_VAR_DECL, 0, "", "u32 x = g; sink(x);"),
    G_FORM("read: assign value",         NODE_ASSIGN,   0, "", "u32 x = 0; x = g; sink(x);"),
    G_FORM("read: compound-assign value",NODE_ASSIGN,   0, "", "u32 x = 1; x += g; sink(x);"),
    G_FORM("read: binary left",          NODE_BINARY,   0, "", "u32 x = g + 1; sink(x);"),
    G_FORM("read: binary right",         NODE_BINARY,   0, "", "u32 x = 1 + g; sink(x);"),
    G_FORM("read: short-circuit right",  NODE_BINARY,   0, "", "bool t = true; if (t || g == 1) { sink(1); }"),
    G_FORM("read: unary operand",        NODE_UNARY,    0, "", "u32 x = ~g; sink(x);"),
    G_FORM("read: call argument",        NODE_CALL,     0, "", "sink(g);"),
    G_FORM("read: nested call argument", NODE_CALL,     0, "u32 idf(u32 v) { return v; }\n", "sink(idf(g));"),
    G_FORM("read: if condition",         NODE_IF,       0, "", "if (g == 3) { sink(1); }"),
    G_FORM("read: else-if condition",    NODE_IF,       0, "", "u32 k = 1; if (k == 2) { sink(1); } else if (g == 3) { sink(2); }"),
    G_FORM("read: if then-body",         NODE_IF,       0, "", "u32 k = 1; if (k == 1) { sink(g); }"),
    G_FORM("read: if else-body",         NODE_IF,       0, "", "u32 k = 1; if (k == 2) { sink(1); } else { sink(g); }"),
    G_FORM("read: while condition",      NODE_WHILE,    0, "", "u32 n = 0; while (n < 2 && g < 100) { n += 1; }"),
    G_FORM("read: while body",           NODE_WHILE,    0, "", "u32 n = 0; while (n < 2) { n += 1; sink(g); }"),
    G_FORM("read: do-while condition",   NODE_DO_WHILE, 0, "", "u32 n = 0; do { n += 1; } while (n < 2 && g < 100);"),
    G_FORM("read: do-while body",        NODE_DO_WHILE, 0, "", "u32 n = 0; do { n += 1; sink(g); } while (n < 2);"),
    G_FORM("read: for init",             NODE_FOR,      0, "", "for (u32 i = g; i < 2; i += 1) { }"),
    G_FORM("read: for condition",        NODE_FOR,      0, "", "for (u32 i = 0; i < 2 && i < g; i += 1) { }"),
    G_FORM("read: for step",             NODE_FOR,      0, "", "for (u32 i = 0; i < 2; i += 1 + (g & 1)) { }"),
    G_FORM("read: for body",             NODE_FOR,      0, "", "for (u32 i = 0; i < 2; i += 1) { sink(g); }"),
    G_FORM("read: switch subject",       NODE_SWITCH,   0, "", "switch (g) { 1 => { sink(1); } default => { } }"),
    G_FORM("read: switch arm body",      NODE_SWITCH,   0, "", "u32 k = 1; switch (k) { 1 => { sink(g); } default => { } }"),
    G_FORM("read: switch default body",  NODE_SWITCH,   0, "", "u32 k = 1; switch (k) { 1 => { } default => { sink(g); } }"),
    G_FORM("read: return value",         NODE_RETURN,   0, "u32 rd() { return g; }\n", "sink(rd());"),
    G_FORM("read: index expression",     NODE_INDEX,    0, "", "u32[4] la; la[0] = 1; u32 x = la[g & 3]; sink(x);"),
    G_FORM("read: index of an lvalue",   NODE_INDEX,    0, "", "u32[4] la; la[g & 3] = 1; sink(la[0]);"),
    G_FORM("read: slice start",          NODE_SLICE,    0, "", "u32[4] la; la[0] = 1; [*]u32 s = la[(g & 1)..4]; sink((u32)s.len);"),
    G_FORM("read: slice end",            NODE_SLICE,    0, "", "u32[4] la; la[0] = 1; [*]u32 s = la[0..(g & 3)]; sink((u32)s.len);"),
    G_FORM("read: struct-init value",    NODE_STRUCT_INIT, 0, "struct W { u32 a; }\n", "W w = { .a = g }; sink(w.a);"),
    G_FORM("read: struct-init assigned", NODE_STRUCT_INIT, 0, "struct W { u32 a; }\n", "W w; w = { .a = g }; sink(w.a);"),
    G_FORM("read: orelse fallback",      NODE_ORELSE,   0, "?u32 none() { return null; }\n", "u32 x = none() orelse g; sink(x);"),
    G_FORM("read: typecast operand",     NODE_TYPECAST, 0, "", "u64 x = (u64)g; sink((u32)x);"),
    G_FORM("read: implicit widening",    NODE_VAR_DECL, 0, "", "u64 x = g; sink((u32)x);"),
    G_FORM("read: intrinsic argument",   NODE_INTRINSIC,0, "", "u8 x = @truncate(u8, g); sink((u32)x);"),
    G_FORM("read: bit-query intrinsic",  NODE_INTRINSIC,0, "", "u32 x = @popcount(g); sink(x);"),
    G_FORM("read: @bitcast",             NODE_INTRINSIC,0, "", "i32 x = @bitcast(i32, g); sink((u32)x);"),
    G_FORM("read: @expect",              NODE_INTRINSIC,0, "", "if (@expect(g, 0) == 0) { sink(1); }"),
    G_FORM("read: defer body",           NODE_DEFER,    0, "", "defer sink(g);"),
    G_FORM("read: defer block",          NODE_DEFER,    0, "", "defer { sink(g); }"),
    G_FORM("read: @critical body",       NODE_CRITICAL, 0, "", "@critical { sink(g); }"),
    G_FORM("read: nested blocks",        NODE_BLOCK,    0, "", "{ { sink(g); } }"),
    /* a function that jumps has no lock region the walk will vouch for: a
     * `goto` can enter or leave one between the take and the drop */
    { "read: after a label", NODE_LABEL, 0, G_DECL,
      "u32 k = 0; again: k += 1; sink(g); if (k < 2) { goto again; }", G_W, G_R, NULL, 0, B(CX_LOCKED), 0 },
    { "read: jumped to", NODE_GOTO, 0, G_DECL, "goto done; done: sink(g);", G_W, G_R, NULL, 0, B(CX_LOCKED), 0 },
    G_FORM("read: spawn argument",       NODE_SPAWN,    0, "void st(u32 v) { }\n", "ThreadHandle q = spawn st(g); q.join();"),
    G_FORM("read: bit extraction",       NODE_SLICE,    0, "", "u32 x = g[3..0]; sink(x);"),
    G_FORM("read: recursion",            NODE_CALL,     0,
           "u32 rec(u32 n) { if (n == 0) { return g; } return rec(n - 1); }\n", "sink(rec(2));"),
    G_FORM("read: comptime-if branch",   NODE_IF,       0, "comptime u32 ON() { return 1; }\n",
           "comptime if (ON()) { sink(g); } else { sink(0); }"),
    G_FORM("read: expression statement", NODE_EXPR_STMT,0, "u32 idf(u32 v) { return v; }\n", "idf(g);"),
    { "read: @once body", NODE_ONCE, 0, G_DECL, "@once { sink(g); }", G_W, G_R, NULL, 0, 0, 0 },
    { "read: asm input", NODE_ASM, 0, "u64 g;\n"
      "naked void rdg() { asm { instructions: \"mov %0, %%rax\" inputs: { \"rdi\" = g }\n"
      "    clobbers: [ \"rax\" ] safety: \"reads one input register into a scratch register\" } }\n",
      NAKED_NEVER("rdg();"), "g = 7;", "u64 rv = g; sink((u32)rv);", NULL, 0, 0, 0 },
    { "read: await condition", NODE_AWAIT, 0, G_DECL "async void job() { await g > 0; }\n",
      "_zer_async_job mine; _zer_async_job_init(&mine); _zer_async_job_poll(&mine);",
      G_W, G_R, NULL, 0, 0, 0 },
    { "read: async body", NODE_YIELD, 0, G_DECL "async void job() { sink(g); yield; sink(g); }\n",
      "_zer_async_job mine; _zer_async_job_init(&mine); _zer_async_job_poll(&mine);",
      G_W, G_R, NULL, 0, 0, 0 },

    /* ================= a WRITE of a scalar ================================== */
    G_FORM("write: assign",              NODE_ASSIGN,   1, "", "g = 1;"),
    G_FORM("write: compound assign",     NODE_ASSIGN,   1, "", "g += 1;"),
    G_FORM("write: shift-assign",        NODE_ASSIGN,   1, "", "g <<= 1;"),
    G_FORM("write: for init",            NODE_FOR,      1, "", "for (g = 0; g < 2; g += 1) { }"),
    G_FORM("write: for step",            NODE_FOR,      1, "", "for (u32 i = 0; i < 2; g += 1) { i += 1; }"),
    G_FORM("write: defer",               NODE_DEFER,    1, "", "defer { g = 1; }"),
    G_FORM("write: @critical",           NODE_CRITICAL, 1, "", "@critical { g = 1; }"),
    G_FORM("write: switch arm",          NODE_SWITCH,   1, "", "u32 k = 1; switch (k) { 1 => { g = 1; } default => { } }"),
    G_FORM("write: bit range",           NODE_SLICE,    1, "", "g[3..0] = 5;"),
    G_FORM("write: comptime-if branch",  NODE_IF,       1, "comptime u32 ON() { return 1; }\n",
           "comptime if (ON()) { g = 1; } else { sink(0); }"),
    G_FORM("write: orelse block",        NODE_ORELSE,   1, "?u32 none() { return null; }\n",
           "u32 k = 0; if (k == 0) { u32 x = none() orelse { g = 1; return; }; sink(x); }"),
    { "write: @once body", NODE_ONCE, 1, G_DECL, "@once { g = 1; }", G_W, G_R, NULL, 0, 0,
      /* one @once block runs once program-wide: two instances cannot meet */
      B(CX_SELF) },
    { "write: asm output", NODE_ASM, 1, "u64 g;\n"
      "naked void wrg() { asm { instructions: \"mov $1, %0\" outputs: { \"rax\" = g }\n"
      "    safety: \"writes the constant one into the output register\" } }\n",
      NAKED_NEVER("wrg();"), "g = 7;", "u64 rv = g; sink((u32)rv);", NULL, 0, 0, 0 },
    { "write: async body", NODE_YIELD, 1, G_DECL "async void job() { g += 1; yield; g += 1; }\n",
      "_zer_async_job mine; _zer_async_job_init(&mine); _zer_async_job_poll(&mine);",
      G_W, G_R, NULL, 0, 0, 0 },
    { "write: static local", NODE_VAR_DECL, 1,
      "u32 bump() { static u32 n; n += 1; return n; }\n",
      "sink(bump());", "sink(bump());", "sink(bump());", NULL,
      /* the object has no name outside bump(): the read partner IS a write */
      B(CX_PAIR_R), 0, 0 },

    /* ================= the ROUTE to the object ============================== */
    G_FORM("route: local pointer, write",   NODE_UNARY, 1, "", "*u32 p = &g; *p = 4;"),
    G_FORM("route: local pointer, read",    NODE_UNARY, 0, "", "*u32 p = &g; u32 x = *p; sink(x);"),
    G_FORM("route: pointer re-aimed",       NODE_UNARY, 1, "", "u32 mine = 0; *u32 p = &mine; p = &g; *p = 4;"),
    G_FORM("route: callee parameter",       NODE_CALL,  1, "void bump(*u32 p) { *p += 1; }\n", "bump(&g);"),
    G_FORM("route: callee parameter, read", NODE_CALL,  0, "u32 peek(*u32 p) { return *p; }\n", "sink(peek(&g));"),
    G_FORM("route: two calls deep",         NODE_CALL,  1,
           "void bump(*u32 p) { *p += 1; }\nvoid fwd(*u32 p) { bump(p); }\n", "fwd(&g);"),
    G_FORM("route: global pointer, write",  NODE_UNARY, 1, "*u32 gp = &g;\n", "*gp = 4;"),
    G_FORM("route: global pointer, read",   NODE_UNARY, 0, "*u32 gp = &g;\n", "u32 x = *gp; sink(x);"),
    G_FORM("route: returned pointer",       NODE_CALL,  1, "*u32 getp() { return &g; }\n", "*u32 p = getp(); *p = 4;"),
    G_FORM("route: returned pointer, inline", NODE_CALL, 1,
           "*u32 getp() { return &g; }\nvoid bump(*u32 p) { *p += 1; }\n", "bump(getp());"),
    G_FORM("route: *opaque round trip",     NODE_INTRINSIC, 1, "",
           "*opaque o = @ptrcast(*opaque, &g); *u32 p = @ptrcast(*u32, o); *p = 4;"),
    G_FORM("route: C-style *opaque casts",  NODE_TYPECAST, 1, "",
           "*opaque o = (*opaque)&g; *u32 p = (*u32)o; *p = 4;"),
    G_FORM("route: pointer in a local struct", NODE_FIELD, 1, "struct H { *u32 p; }\n",
           "H h = { .p = &g }; *h.p = 4;"),
    G_FORM("route: optional pointer",       NODE_IF,    1, "",
           "?*u32 mp = &g; if (mp) |p| { *p = 4; }"),
    G_FORM("route: function pointer, direct", NODE_CALL, 1, "void wr() { g = 2; }\n",
           "*() -> void f = wr; f();"),
    G_FORM("route: function pointer, parameter", NODE_CALL, 1,
           "void wr() { g = 2; }\nvoid run(*() -> void f) { f(); }\n", "run(wr);"),
    G_FORM("route: function pointer, global", NODE_CALL, 1,
           "void wr() { g = 2; }\n*() -> void gf = wr;\n", "gf();"),
    { "route: pointer in a global struct", NODE_FIELD, 1, G_DECL "struct H { *u32 p; }\nH gh;\n",
      "*gh.p = 4;", G_W, G_R, "gh.p = &g;", 0, 0, 0 },
    { "route: the reference itself, read on the way to a write", NODE_UNARY, 0,
      "u32 a1;\nu32 a2;\n*u32 gp = &a1;\n",
      "u32 x = *gp; sink(x);", "gp = &a2;", "*u32 q = gp; sink(*q);", NULL, 0, 0, 0 },
    { "route: the reference itself, a store through it", NODE_UNARY, 0,
      "u32 a1;\nu32 a2;\n*u32 gp = &a1;\n",
      "*gp = 4;", "gp = &a2;", "*u32 q = gp; sink(*q);", NULL,
      /* a store through gp WRITES its pointee: two of them race on that */
      B(CX_SELF) | B(CX_PAIR_R), 0, 0 },

    /* ================= value SHAPES ========================================= */
    { "shape: field write", NODE_FIELD, 1, S_DECL, "gs.b = 1;", "gs.b = 7;", "u32 rv = gs.b; sink(rv);", NULL, 0, 0, 0 },
    { "shape: field read", NODE_FIELD, 0, S_DECL, "u32 x = gs.a; sink(x);", S_W, S_R, NULL, 0, 0, 0 },
    { "shape: whole-struct write", NODE_ASSIGN, 1, S_DECL, "S z; z.a = 0; z.b = 0; gs = z;", S_W, S_R, NULL, 0, 0, 0 },
    { "shape: whole-struct read", NODE_VAR_DECL, 0, S_DECL, "S z = gs; sink(z.a);", S_W, S_R, NULL, 0, 0, 0 },
    { "shape: struct-literal write", NODE_STRUCT_INIT, 1, S_DECL, "gs = { .a = 1, .b = 2 };", S_W, S_R, NULL, 0, 0, 0 },
    { "shape: struct by value to a call", NODE_CALL, 0, S_DECL "void eat(S s) { sink(s.a); }\n",
      "eat(gs);", S_W, S_R, NULL, 0, 0, 0 },
    { "shape: field through a pointer", NODE_FIELD, 1, S_DECL, "*S p = &gs; p.a = 2;", S_W, S_R, NULL, 0, 0, 0 },
    { "shape: address of a field", NODE_UNARY, 1, S_DECL, "*u32 p = &gs.a; *p = 2;", S_W, S_R, NULL, 0, 0, 0 },
    { "shape: nested field", NODE_FIELD, 1, "struct S { u32 a; u32 b; }\nstruct O { S in; u32[2] arr; }\nO gs;\n",
      "gs.in.a = 1;", "gs.in.a = 7;", "u32 rv = gs.in.a; sink(rv);", NULL, 0, 0, 0 },
    { "shape: array in a struct", NODE_INDEX, 1, "struct S { u32 a; u32 b; }\nstruct O { S in; u32[2] arr; }\nO gs;\n",
      "gs.arr[1] = 1;", "gs.arr[1] = 7;", "u32 rv = gs.arr[1]; sink(rv);", NULL, 0, 0, 0 },
    { "shape: struct in an array", NODE_INDEX, 1, "struct S { u32 a; u32 b; }\nS[2] gs;\n",
      "gs[1].a = 1;", "gs[1].a = 7;", "u32 rv = gs[1].a; sink(rv);", NULL, 0, 0, 0 },
    { "shape: element write", NODE_INDEX, 1, A_DECL, "ga[1] = 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: element read", NODE_INDEX, 0, A_DECL, "u32 x = ga[1]; sink(x);", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: element, variable index", NODE_INDEX, 1, A_DECL, "u32 k = 1; ga[k] += 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: slice view, write", NODE_SLICE, 1, A_DECL, "[*]u32 s = ga[0..4]; s[1] = 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: slice view, read", NODE_SLICE, 0, A_DECL, "[*]u32 s = ga[0..4]; sink(s[1]);", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: slice argument", NODE_CALL, 1, A_DECL "void fill([*]u32 s) { s[1] = 1; }\n",
      "fill(ga[0..4]);", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: array argument", NODE_CALL, 1, A_DECL "void fill([*]u32 s) { s[1] = 1; }\n",
      "fill(ga);", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: address of an element", NODE_UNARY, 1, A_DECL, "*u32 p = &ga[1]; *p = 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: global slice, write through", NODE_INDEX, 1, A_DECL "[*]u32 gsl;\n",
      "gsl[1] = 3;", A_W, A_R, "gsl = ga[0..4];", 0, 0, 0 },
    { "shape: global slice, the slice itself", NODE_FIELD, 0, A_DECL "[*]u32 gsl;\n",
      "sink((u32)gsl.len);", "gsl = ga[0..2];", "sink((u32)gsl.len);", "gsl = ga[0..4];", 0, 0, 0 },
    { "shape: optional write", NODE_ASSIGN, 1, O_DECL, "go = 5;", O_W, O_R, NULL, 0, 0, 0 },
    { "shape: optional null write", NODE_ASSIGN, 1, O_DECL, "go = null;", O_W, O_R, NULL, 0, 0, 0 },
    { "shape: optional orelse read", NODE_ORELSE, 0, O_DECL, "u32 x = go orelse 0; sink(x);", O_W, O_R, NULL, 0, 0, 0 },
    { "shape: optional if-unwrap", NODE_IF, 0, O_DECL, "if (go) |v| { sink(v); }", O_W, O_R, NULL, 0, 0, 0 },
    { "shape: optional if-unwrap by pointer", NODE_IF, 1, O_DECL, "if (go) |*v| { *v = 9; }", O_W, O_R, NULL, 0, 0, 0 },
    { "shape: union write", NODE_FIELD, 1, U_DECL, "gu.b = 5;", U_W, U_R, NULL, 0, 0, 0 },
    { "shape: union switch capture", NODE_SWITCH, 0, U_DECL,
      "switch (gu) { .a => |v| { sink(v); } .b => |v| { sink(v); } }", U_W, U_R, NULL, 0, 0, 0 },
    { "shape: union switch capture by pointer", NODE_SWITCH, 1, U_DECL,
      "switch (gu) { .a => |*v| { *v = 3; } .b => |*v| { *v = 4; } }", U_W, U_R, NULL, 0, 0, 0 },
    { "shape: enum write", NODE_ASSIGN, 1, "enum E { x, y }\nE ge;\n", "ge = E.y;", "ge = E.x;",
      "switch (ge) { .x => { sink(1); } .y => { sink(2); } }", NULL, 0, 0, 0 },
    { "shape: enum switch", NODE_SWITCH, 0, "enum E { x, y }\nE ge;\n",
      "switch (ge) { .x => { sink(1); } .y => { sink(2); } }", "ge = E.x;",
      "switch (ge) { .x => { sink(1); } .y => { sink(2); } }", NULL, 0, 0, 0 },
    { "shape: bool", NODE_ASSIGN, 1, "bool gb;\n", "gb = !gb;", "gb = true;", "if (gb) { sink(1); }", NULL, 0, 0, 0 },
    { "shape: float", NODE_ASSIGN, 1, "f32 gf;\n", "gf = gf * 2.0;", "gf = 1.0;", "f32 rv = gf; sink((u32)rv);", NULL, 0, 0, 0 },
    { "shape: arbitrary-width integer", NODE_ASSIGN, 1, "u21 gn;\n", "gn += 1;", "gn = 7;", "u21 rv = gn; sink((u32)rv);", NULL, 0, 0, 0 },
    { "shape: distinct typedef", NODE_ASSIGN, 1, "typedef u32 Ms;\nMs gm;\n", "gm = 1;", "gm = 7;", "Ms rv = gm; sink((u32)rv);", NULL, 0, 0, 0 },
    { "shape: move struct field", NODE_FIELD, 1, "move struct M { u32 fd; }\nM gm;\n",
      "gm.fd += 1;", "gm.fd = 7;", "u32 rv = gm.fd; sink(rv);", NULL, 0, 0, 0 },
    { "shape: function-pointer global, the pointer itself", NODE_CALL, 0,
      "void fa() { }\nvoid fb() { }\n*() -> void gf = fa;\n",
      "gf();", "gf = fb;", "*() -> void l = gf; l();", NULL, 0, 0, 0 },
    { "shape: pointer global, the pointer itself", NODE_IDENT, 1,
      "u32 a1;\nu32 a2;\n*u32 gp = &a1;\n", "gp = &a2;", "gp = &a1;", "*u32 q = gp; sink(*q);", NULL, 0, 0, 0 },

    /* ================= containers and handles =============================== */
    { "container: pool alloc/free", NODE_CALL, 1, P_DECL,
      "?Handle(T) h = gpool.alloc(); if (h) |x| { gpool.free(x); }", P_W, P_W, NULL, B(CX_PAIR_R), 0, 0 },
    { "container: slab alloc/free", NODE_CALL, 1, "struct T { u32 v; }\nSlab(T) gslab;\n",
      "?Handle(T) h = gslab.alloc(); if (h) |x| { gslab.free(x); }",
      "?Handle(T) wh = gslab.alloc(); if (wh) |x| { gslab.free(x); }",
      "?Handle(T) wh = gslab.alloc(); if (wh) |x| { gslab.free(x); }", NULL, B(CX_PAIR_R), 0, 0 },
    { "container: ring push", NODE_CALL, 1, "Ring(u32, 8) gring;\n",
      "gring.push(1);", "gring.push(2);", "u32 rv = gring.pop() orelse 0; sink(rv);", NULL, 0, 0, 0 },
    { "container: ring pop", NODE_CALL, 1, "Ring(u32, 8) gring;\n",
      "u32 x = gring.pop() orelse 0; sink(x);", "gring.push(2);", "u32 rv = gring.pop() orelse 0; sink(rv);", NULL, 0, 0, 0 },
    { "container: arena alloc", NODE_CALL, 1, "u8[256] gmem;\nArena gar;\n",
      "?*u32 p = gar.alloc(u32); if (p) |q| { *q = 1; }",
      "?*u32 wp = gar.alloc(u32); if (wp) |q| { *q = 2; }",
      "?*u32 wp = gar.alloc(u32); if (wp) |q| { *q = 2; }", "gar = Arena.over(gmem);", B(CX_PAIR_R), 0, 0 },
    { "container: arena reset", NODE_CALL, 1, "u8[256] gmem;\nArena gar;\n",
      "gar.reset();",
      "?*u32 wp = gar.alloc(u32); if (wp) |q| { *q = 2; }",
      "?*u32 wp = gar.alloc(u32); if (wp) |q| { *q = 2; }", "gar = Arena.over(gmem);", B(CX_PAIR_R), 0, 0 },
    { "container: auto-slab alloc/free", NODE_CALL, 1, "struct T { u32 v; }\n",
      "?Handle(T) h = T.alloc(); if (h) |x| { T.free(x); }",
      "?Handle(T) wh = T.alloc(); if (wh) |x| { T.free(x); }",
      "?Handle(T) wh = T.alloc(); if (wh) |x| { T.free(x); }", NULL, B(CX_PAIR_R), 0, 0 },
    { "container: universal alloc/free", NODE_CALL, 1, "struct T { u32 v; }\n",
      "?*T p = alloc(T); if (p) |q| { q.v = 1; free(q); }",
      "?*T wp = alloc(T); if (wp) |q| { q.v = 2; free(q); }",
      "?*T wp = alloc(T); if (wp) |q| { q.v = 2; free(q); }", NULL, B(CX_PAIR_R), 0, 0 },
    { "handle: field write through a global handle", NODE_FIELD, 1,
      "struct T { u32 v; }\nSlab(T) gslab;\nHandle(T) gh;\n",
      "gh.v = 1;", "gh.v = 7;", "u32 rv = gh.v; sink(rv);",
      "gh = gslab.alloc() orelse return;", 0, 0, 0 },
    { "handle: field read through a global handle", NODE_FIELD, 0,
      "struct T { u32 v; }\nSlab(T) gslab;\nHandle(T) gh;\n",
      "u32 x = gh.v; sink(x);", "gh.v = 7;", "u32 rv = gh.v; sink(rv);",
      "gh = gslab.alloc() orelse return;", 0, 0, 0 },
    { "handle: pool.get write", NODE_CALL, 1,
      "struct T { u32 v; }\nPool(T, 4) gpool;\nHandle(T) gh;\n",
      "gpool.get(gh).v = 1;", "gpool.get(gh).v = 7;", "u32 rv = gpool.get(gh).v; sink(rv);",
      "gh = gpool.alloc() orelse return;", 0, 0, 0 },

    /* ================= intrinsics that touch memory through an argument ===== */
    { "intrinsic: @cstr destination", NODE_INTRINSIC, 1, "u8[16] gbuf;\n",
      "@cstr(gbuf, \"hi\");", "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cstr source", NODE_INTRINSIC, 0, "u8[4] gsrc;\n",
      "u8[16] lb; @cstr(lb, gsrc[0..2]); sink((u32)lb[0]);", "gsrc[0] = 7;", "u32 rv = (u32)gsrc[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @atomic_add vs a plain access", NODE_INTRINSIC, 1, G_DECL,
      "@atomic_add(&g, 1);", G_W, G_R, NULL, 0, 0,
      /* an atomic write beside an atomic write is no race */
      B(CX_SELF) },
    { "intrinsic: @atomic_load vs a plain write", NODE_INTRINSIC, 0, G_DECL,
      "u32 x = @atomic_load(&g); sink(x);", G_W, G_R, NULL, 0, 0, 0 },
    { "intrinsic: @atomic_store through a parameter", NODE_INTRINSIC, 1,
      G_DECL "void aset(*u32 p) { @atomic_store(p, 1); }\n",
      "aset(&g);", G_W, G_R, NULL, 0, 0, B(CX_SELF) },
    { "intrinsic: @container to the enclosing struct", NODE_INTRINSIC, 1, S_DECL,
      "*u32 fp = &gs.b; *S o = @container(*S, fp, b); o.a = 2;", S_W, S_R, NULL, 0, 0, 0 },
    { "intrinsic: @pun view", NODE_INTRINSIC, 1, "struct S { u32 a; u32 b; }\nstruct V { u32 a; u32 b; }\nS gs;\n",
      "*V v = @pun(*V, &gs); v.a = 2;", S_W, S_R, NULL, 0, 0, 0 },
    /* privileged: compiled, never executed (the branch is dead at run time) */
    { "intrinsic: @cpu_fxsave buffer", NODE_INTRINSIC, 1, "u8[512] gbuf;\n",
      NAKED_NEVER("@cpu_fxsave(&gbuf[0]);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cpu_xsave buffer", NODE_INTRINSIC, 1, "u8[4096] gbuf;\n",
      NAKED_NEVER("@cpu_xsave(&gbuf[0], 7);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cpu_save_context buffer", NODE_INTRINSIC, 1, "u8[128] gbuf;\n",
      NAKED_NEVER("@cpu_save_context(&gbuf[0]);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cpu_save_fpu buffer", NODE_INTRINSIC, 1, "u8[512] gbuf;\n",
      NAKED_NEVER("@cpu_save_fpu(&gbuf[0]);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cache_zero_line", NODE_INTRINSIC, 1, "u8[64] gbuf;\n",
      NAKED_NEVER("@cache_zero_line(&gbuf[0]);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @nt_store", NODE_INTRINSIC, 1, "u64 g;\n",
      NAKED_NEVER("@nt_store(&g, 5);"), "g = 7;", "u64 rv = g; sink((u32)rv);", NULL, 0, 0, 0 },
    { "intrinsic: @nt_store through a parameter", NODE_INTRINSIC, 1,
      "u64 g;\nvoid nts(*u64 p) { volatile u32 never = 0; if (never == 42) { @nt_store(p, 5); } }\n",
      "nts(&g);", "g = 7;", "u64 rv = g; sink((u32)rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cpu_fxrstor buffer", NODE_INTRINSIC, 0, "u8[512] gbuf;\n",
      NAKED_NEVER("@cpu_fxrstor(&gbuf[0]);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cpu_xrstor buffer", NODE_INTRINSIC, 0, "u8[4096] gbuf;\n",
      NAKED_NEVER("@cpu_xrstor(&gbuf[0], 7);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cpu_restore_context buffer", NODE_INTRINSIC, 0, "u8[128] gbuf;\n",
      NAKED_NEVER("@cpu_restore_context(&gbuf[0]);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cpu_restore_fpu buffer", NODE_INTRINSIC, 0, "u8[512] gbuf;\n",
      NAKED_NEVER("@cpu_restore_fpu(&gbuf[0]);"), "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @wait_on_address word", NODE_INTRINSIC, 0, G_DECL,
      NAKED_NEVER("@wait_on_address(&g, 0);"), G_W, G_R, NULL, 0, 0, 0 },
    { "intrinsic: @cstr through a slice parameter", NODE_INTRINSIC, 1,
      "u8[16] gbuf;\nvoid put([*]u8 d) { @cstr(d, \"hi\"); }\n",
      "put(gbuf[0..16]);", "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @cstr into a struct field", NODE_INTRINSIC, 1,
      "struct B { u8[16] buf; u32 n; }\nB gs;\n",
      "@cstr(gs.buf, \"hi\");", "gs.buf[0] = 7;", "u32 rv = (u32)gs.buf[0]; sink(rv);", NULL, 0, 0, 0 },
    { "intrinsic: @addc operand", NODE_INTRINSIC, 0, "u64 g;\n",
      "AddCarry64 r = @addc(g, 1, 0); sink((u32)r.sum);", "g = 7;", "u64 rv = g; sink((u32)rv);", NULL, 0, 0, 0 },

    /* ================= through a PARAMETER of the thread ==================== */
    { "param: pointer, write", NODE_UNARY, 1, G_DECL, "*p = 4;", G_W, G_R, NULL, 0, 0, 0, "*u32 p", "&g", "p" },
    /* (lending ONE object to two live threads is refused whatever they do with
     * it: two readers are turned away too) */
    { "param: pointer, read", NODE_UNARY, 0, G_DECL, "u32 x = *p; sink(x);", G_W, G_R, NULL, 0, B(CX_SELF), 0, "*u32 p", "&g", "p" },
    { "param: pointer, compound", NODE_ASSIGN, 1, G_DECL, "*p += 1;", G_W, G_R, NULL, 0, 0, 0, "*u32 p", "&g", "p" },
    { "param: slice, write", NODE_INDEX, 1, A_DECL, "s[1] = 3;", A_W, A_R, NULL, 0, 0, 0, "[*]u32 s", "ga[0..4]", "s" },
    { "param: slice, read", NODE_INDEX, 0, A_DECL, "sink(s[1]);", A_W, A_R, NULL, 0, B(CX_SELF), 0, "[*]u32 s", "ga[0..4]", "s" },
    { "param: pointer to an element", NODE_UNARY, 1, A_DECL, "*p = 3;", A_W, A_R, NULL, 0, 0, 0, "*u32 p", "&ga[1]", "p" },
    { "param: struct pointer, field", NODE_FIELD, 1, S_DECL, "p.a = 2;", S_W, S_R, NULL, 0, 0, 0, "*S p", "&gs", "p" },
    { "param: pointer to a field", NODE_UNARY, 1, S_DECL, "*p = 2;", S_W, S_R, NULL, 0, 0, 0, "*u32 p", "&gs.a", "p" },
    { "param: opaque pointer", NODE_TYPECAST, 1, G_DECL, "*u32 q = (*u32)o; *q = 4;", G_W, G_R, NULL, 0, 0, 0,
      "*opaque o", "(*opaque)&g", "o" },
    { "param: struct holding a pointer, by value", NODE_FIELD, 1, G_DECL "struct H { *u32 p; }\n",
      "*h.p = 4;", G_W, G_R, NULL, 0, 0, 0, "H h", "{ .p = &g }", "h" },
    { "param: handed on to a callee", NODE_CALL, 1, G_DECL "void bump(*u32 q) { *q += 1; }\n",
      "bump(p);", G_W, G_R, NULL, 0, 0, 0, "*u32 p", "&g", "p" },
    { "param: intrinsic store", NODE_INTRINSIC, 1, "u8[16] gbuf;\n",
      "@cstr(d, \"hi\");", "gbuf[0] = 7;", "u32 rv = (u32)gbuf[0]; sink(rv);", NULL, 0, 0, 0,
      "[*]u8 d", "gbuf[0..16]", "d" },

    /* ================= more routes ========================================== */
    G_FORM("route: pointer to pointer",        NODE_UNARY, 1, "", "*u32 p = &g; **u32 pp = &p; **pp = 4;"),
    G_FORM("route: out-parameter",             NODE_CALL,  1, "void get(**u32 out) { *out = &g; }\n",
           "u32 mine = 0; *u32 p = &mine; get(&p); *p = 4;"),
    G_FORM("route: returned struct holding a pointer", NODE_CALL, 1,
           "struct H { *u32 p; }\nH mk() { H h = { .p = &g }; return h; }\n", "H h = mk(); *h.p = 4;"),
    G_FORM("route: optional pointer, orelse return", NODE_ORELSE, 1, "?*u32 find() { return &g; }\n",
           "u32 k = 0; if (k == 0) { *u32 p = find() orelse return; *p = 4; }"),
    G_FORM("route: deferred call",             NODE_DEFER, 1, "void wr() { g = 2; }\n", "defer wr();"),
    G_FORM("route: call in a spawn argument",  NODE_SPAWN, 0, "u32 rd() { return g; }\nvoid st(u32 v) { }\n",
           "ThreadHandle q = spawn st(rd()); q.join();"),
    G_FORM("route: mutual recursion",          NODE_CALL,  1,
           "void pong(u32 n);\nvoid ping(u32 n) { if (n > 0) { pong(n - 1); } }\n"
           "void pong(u32 n) { g += 1; if (n > 0) { ping(n - 1); } }\n", "ping(2);"),
    { "route: optional pointer global", NODE_IF, 1, G_DECL "?*u32 gop;\n",
      "if (gop) |p| { *p = 4; }", G_W, G_R, "gop = &g;", 0, 0, 0 },
    { "route: function pointer in a global struct", NODE_CALL, 1,
      G_DECL "void wr() { g = 2; }\nstruct Ops { *() -> void f; }\nOps gops;\n",
      "gops.f();", G_W, G_R, "gops.f = wr;", 0,
      /* the spawn scan's finding for a funcptr FIELD is an error even beside
       * an @atomic: the walk never gets to judge the lock or the join */
      B(CX_LOCKED) | B(CX_AFTER) | B(CX_SEQ), 0 },
    { "route: function pointer table", NODE_CALL, 1,
      G_DECL "void wr() { g = 2; }\nvoid no() { }\n*() -> void [2] gtab;\n",
      "gtab[1]();", G_W, G_R, "gtab[0] = no; gtab[1] = wr;", 0, 0, 0 },
    { "route: returned slice view", NODE_CALL, 1, A_DECL "[*]u32 view() { return ga[0..4]; }\n",
      "[*]u32 s = view(); s[1] = 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "route: slice in a global struct", NODE_INDEX, 1, A_DECL "struct V { [*]u32 s; }\nV gv;\n",
      "gv.s[1] = 3;", A_W, A_R, "gv.s = ga[0..4];", 0, 0, 0 },
    { "route: slice of structs", NODE_FIELD, 1, "struct S { u32 a; u32 b; }\nS[2] gs;\n",
      "[*]S s = gs[0..2]; s[1].a = 1;", "gs[1].a = 7;", "u32 rv = gs[1].a; sink(rv);", NULL, 0, 0, 0 },
    { "route: container stamp through a pointer", NODE_CALL, 1,
      "container Stack(T) { T[8] data; u32 top; }\nStack(u32) gst;\n"
      "void push(*Stack(u32) s, u32 v) { s.data[s.top & 7] = v; s.top += 1; }\n",
      "push(&gst, 1);", "gst.top = 0;", "u32 rv = gst.top; sink(rv);", NULL, 0, 0, 0 },

    /* ================= more shapes ========================================== */
    /* THE volatile EXEMPTION (volatile_global_exempt_from_race_check): a plain
     * load or store of a SINGLE-WORD volatile global is the flag idiom — one
     * machine instruction, never elided or fused. It is exempt by definition;
     * a read-modify-write of it is not, and neither is a volatile aggregate. */
    #define NEG_ALL (B(CX_PAIR_W) | B(CX_PAIR_R) | B(CX_SELF) | B(CX_THR_WIN) | B(CX_WIN_THR) | \
                     B(CX_HELPER) | B(CX_GRAND) | B(CX_DETACH) | B(CX_COARSE) | B(CX_WIN_CALL))
    { "shape: volatile word, plain store", NODE_ASSIGN, 1, "volatile u32 gv;\n",
      "gv = 1;", "gv = 7;", "u32 rv = gv; sink(rv);", NULL, 0, 0, NEG_ALL },
    { "shape: volatile word, plain load", NODE_VAR_DECL, 0, "volatile u32 gv;\n",
      "u32 x = gv; sink(x);", "gv = 7;", "u32 rv = gv; sink(rv);", NULL, 0, 0, NEG_ALL },
    { "shape: volatile word, read-modify-write", NODE_ASSIGN, 1, "volatile u32 gv;\n",
      "gv += 1;", "gv = 7;", "u32 rv = gv; sink(rv);", NULL, 0,
      /* refused in any spawned thread, whoever else runs: only a common lock excuses it */
      B(CX_AFTER) | B(CX_SEQ), 0 },
    { "shape: volatile struct", NODE_FIELD, 1, "struct S { u32 a; u32 b; }\nvolatile S gs;\n",
      "gs.a = 1;", S_W, S_R, NULL, 0, 0, 0 },
    { "shape: volatile array", NODE_INDEX, 1, "volatile u32[4] ga;\n",
      "ga[1] = 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: whole-array write", NODE_ASSIGN, 1, A_DECL, "u32[4] la; la[0] = 1; ga = la;", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: whole-array read", NODE_VAR_DECL, 0, A_DECL, "u32[4] la = ga; sink(la[0]);", A_W, A_R, NULL, 0, 0, 0 },
    { "shape: condvar predicate", NODE_INTRINSIC, 0, G_DECL "shared struct Q { u32 n; }\nQ gq;\n",
      "@cond_wait(gq, g < 100);", G_W, G_R, NULL, 0, 0, 0 },
    { "shape: static local array", NODE_INDEX, 1,
      "u32 bump(u32 i) { static u32[4] tab; tab[i & 3] += 1; return tab[i & 3]; }\n",
      "sink(bump(1));", "sink(bump(1));", "sink(bump(1));", NULL, B(CX_PAIR_R), 0, 0 },

    /* ================= THE PATH KEY: what is, and is not, apart ============= *
     * Two threads in two sub-objects of one global, at paths neither of which
     * extends the other — two fields, two constant elements, a field and an
     * element of another field — touch no common byte: no race, in any context
     * (`excused` everywhere). Every way of losing the path — the whole
     * aggregate, a variable index, a pointer into it, a view, a union's
     * variants — is the enclosing sub-object, and still refused. */
    #define D_DECL "struct D { u32 a; u32 b; u32[4] arr; }\nD gd;\n"
    /* excused wherever the partner is the SIBLING. (Two instances of a writing
     * form are two writes of one field: CX_SELF stays a race.) Without an
     * @atomic the spawn scan refuses the mention of `gd` outright — it has no
     * notion of a field — so CX_COARSE is a known over-rejection. */
    #define APART_W (NEG_ALL & ~B(CX_SELF))
    #define APART_R NEG_ALL
    { "apart: field write | sibling field write", NODE_FIELD, 1, D_DECL,
      "gd.a = 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: field read | sibling field write", NODE_FIELD, 0, D_DECL,
      "u32 x = gd.a; sink(x);", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_R },
    { "apart: field compound | sibling field write", NODE_ASSIGN, 1, D_DECL,
      "gd.a += 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: array field element | sibling field write", NODE_INDEX, 1, D_DECL,
      "gd.arr[1] = 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: array field, variable index | sibling", NODE_INDEX, 1, D_DECL,
      "u32 k = 1; gd.arr[k] += 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: bit range of a field | sibling", NODE_SLICE, 1, D_DECL,
      "gd.a[3..0] = 5;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: nested struct field | sibling", NODE_FIELD, 1,
      "struct I { u32 x; u32 y; }\nstruct D { I in; u32 b; }\nD gd;\n",
      "gd.in.x = 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: atomic field | plain sibling", NODE_INTRINSIC, 1, D_DECL,
      /* (two instances of it are atomic beside atomic) */
      "@atomic_add(&gd.a, 1);", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), NEG_ALL },
    { "apart: field in a condition | sibling", NODE_IF, 0, D_DECL,
      "if (gd.a == 3) { sink(1); }", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_R },
    { "apart: element | another element of a field", NODE_INDEX, 1, D_DECL,
      "gd.arr[1] = 1;", "gd.arr[2] = 7;", "u32 rv = gd.arr[2]; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: element | another element", NODE_INDEX, 1, A_DECL,
      "ga[2] = 1;", A_W, A_R, NULL, 0, B(CX_COARSE), APART_W },
    { "apart: atomic element | plain other element", NODE_INTRINSIC, 1, A_DECL,
      "@atomic_add(&ga[2], 1);", A_W, A_R, NULL, 0, B(CX_COARSE), NEG_ALL },
    { "apart: sub-field | sibling sub-field", NODE_FIELD, 1,
      "struct I { u32 x; u32 y; }\nstruct D { I in; u32 b; }\nD gd;\n",
      "gd.in.x = 1;", "gd.in.y = 7;", "u32 rv = gd.in.y; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: field of one element | field of another", NODE_FIELD, 1,
      "struct S { u32 a; u32 b; }\nS[2] gs;\n",
      "gs[0].a = 1;", "gs[1].a = 7;", "u32 rv = gs[1].a; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "apart: union variant | a sibling of the union", NODE_FIELD, 1,
      "union U { u32 a; u32 b; }\nstruct D { U u; u32 b; }\nD gd;\n",
      "gd.u.a = 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    /* ... and NOT apart */
    { "same field: write | write", NODE_FIELD, 1, D_DECL,
      "gd.a = 1;", "gd.a = 7;", "u32 rv = gd.a; sink(rv);", NULL, 0, 0, 0 },
    { "same field: nested", NODE_FIELD, 1,
      "struct I { u32 x; u32 y; }\nstruct D { I in; u32 b; }\nD gd;\n",
      "gd.in.x = 1;", "gd.in.x = 7;", "u32 rv = gd.in.x; sink(rv);", NULL, 0, 0, 0 },
    { "nested field | the whole inner struct", NODE_FIELD, 1,
      "struct I { u32 x; u32 y; }\nstruct D { I in; u32 b; }\nD gd;\n",
      "gd.in.x = 1;", "I z; z.x = 0; z.y = 0; gd.in = z;", "I z = gd.in; sink(z.y);", NULL, 0, 0, 0 },
    { "same element", NODE_INDEX, 1, D_DECL,
      "gd.arr[1] = 1;", "gd.arr[1] = 7;", "u32 rv = gd.arr[1]; sink(rv);", NULL, 0, 0, 0 },
    { "constant element | a variable index", NODE_INDEX, 1, D_DECL,
      "gd.arr[1] = 1;", "u32 k = 2; gd.arr[k] = 7;", "u32 k = 2; u32 rv = gd.arr[k]; sink(rv);", NULL, 0, 0, 0 },
    { "variable index | a constant element", NODE_INDEX, 1, A_DECL,
      "u32 k = 2; ga[k] = 1;", A_W, A_R, NULL, 0, 0, 0 },
    { "constant element | the whole array", NODE_INDEX, 1, A_DECL,
      "ga[2] = 1;", "u32[4] la; la[0] = 1; ga = la;", "u32[4] la = ga; sink(la[0]);", NULL, 0, 0, 0 },
    { "union inside a struct: variant | another variant", NODE_FIELD, 1,
      "union U { u32 a; u32 b; }\nstruct D { U u; u32 b; }\nD gd;\n",
      "gd.u.a = 1;", "gd.u.b = 7;", "switch (gd.u) { .a => |v| { sink(v); } .b => |v| { sink(v); } }", NULL, 0, 0, 0 },
    { "field | whole-struct write", NODE_FIELD, 1, D_DECL,
      "gd.a = 1;", "D z; z.a = 0; z.b = 0; z.arr[0] = 0; gd = z;", "D z = gd; sink(z.a);", NULL, 0, 0, 0 },
    { "field | whole-struct read", NODE_FIELD, 1, D_DECL,
      "gd.a = 1;", "D z; z.a = 0; z.b = 0; z.arr[0] = 0; gd = z;", "D z = gd; sink(z.b);", NULL, 0, 0, 0 },
    { "whole-struct read | sibling field write", NODE_VAR_DECL, 0, D_DECL,
      "D z = gd; sink(z.a);", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, 0, 0 },
    { "field | struct-literal write", NODE_STRUCT_INIT, 1, "struct D { u32 a; u32 b; }\nD gd;\n",
      "gd = { .a = 1, .b = 2 };", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, 0, 0 },
    { "field | sibling through a struct pointer", NODE_FIELD, 1, D_DECL,
      "*D p = &gd; p.a = 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, 0, 0 },
    { "field | the same field through its address", NODE_UNARY, 1, D_DECL,
      "*u32 p = &gd.b; *p = 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, 0, 0 },
    { "field | the same field through a callee", NODE_CALL, 1, D_DECL "void bump(*u32 p) { *p += 1; }\n",
      "bump(&gd.b);", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, 0, 0 },
    { "field | the same field through @container", NODE_INTRINSIC, 1, D_DECL,
      "*u32 fp = &gd.a; *D o = @container(*D, fp, a); o.b = 2;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, 0, 0 },
    { "field | the same field by a returned pointer", NODE_CALL, 1, D_DECL "*u32 fb() { return &gd.b; }\n",
      "*u32 p = fb(); *p = 1;", "gd.b = 7;", "u32 rv = gd.b; sink(rv);", NULL, 0, 0, 0 },
    { "array field: element | a slice view of the field", NODE_SLICE, 1, D_DECL,
      "[*]u32 s = gd.arr[0..4]; s[1] = 1;", "gd.arr[2] = 7;", "u32 rv = gd.arr[2]; sink(rv);", NULL, 0, 0, 0 },
    { "union: variant | another variant", NODE_FIELD, 1, U_DECL,
      "gu.a = 1;", "gu.b = 7;", U_R, NULL, 0, 0, 0 },
    { "pointer global: field through it | sibling through it", NODE_FIELD, 1, D_DECL "*D gp = &gd;\n",
      "gp.a = 1;", "gd.a = 7;", "u32 rv = gd.a; sink(rv);", NULL, 0, 0, 0 },
    { "struct in an array: field of one element | of another", NODE_FIELD, 1,
      "struct S { u32 a; u32 b; }\nS[2] gs;\n",
      "gs[0].a = 1;", "gs[0].a = 7;", "u32 rv = gs[0].a; sink(rv);", NULL, 0, 0, 0 },
    { "atomic field | plain write of the SAME field", NODE_INTRINSIC, 1, D_DECL,
      "@atomic_add(&gd.a, 1);", "gd.a = 7;", "u32 rv = gd.a; sink(rv);", NULL, 0, 0, B(CX_SELF) },
    { "local shadows the global: a field of the LOCAL pointer's target", NODE_FIELD, 1,
      D_DECL "D other;\n",
      "*D gd2 = &other; gd2.a = 1;", "other.a = 7;", "u32 rv = other.a; sink(rv);", NULL, 0, 0, 0 },

    /* ---- the path key's hypotheses, one row each ---- */
    /* H: the root name IS the global. A parameter (or local) of that name is not. */
    { "path: a parameter shadows the global's name", NODE_FIELD, 1, D_DECL "D other;\n"
      "void setb(*D gd) { gd.b = 1; }\n",
      "setb(&other);", "other.b = 7;", "u32 rv = other.b; sink(rv);", NULL, 0, 0, 0 },
    { "path: a local struct shadows the global's name", NODE_FIELD, 1, D_DECL "D other;\n"
      "void cp() { D gd; gd.a = 1; gd.b = 2; gd.arr[0] = 0; other = gd; }\n",
      "cp();", "other.b = 7;", "u32 rv = other.b; sink(rv);", NULL, 0, 0, 0 },
    /* H: no step goes through a reference — and the reference on the way is READ */
    { "path: store through a pointer field | the field re-aimed", NODE_UNARY, 0,
      "u32 a1;\nu32 a2;\nstruct H { *u32 p; u32 n; }\nH gh;\n",
      "*gh.p = 4;", "gh.p = &a2;", "*u32 q = gh.p; sink(*q);", "gh.p = &a1;",
      B(CX_SELF) | B(CX_PAIR_R), 0, 0 },
    { "path: field through a pointer field | the field re-aimed", NODE_FIELD, 0,
      "struct T2 { u32 x; }\nT2 tx1;\nT2 tx2;\nstruct H { *T2 p; u32 n; }\nH gh;\n",
      "gh.p.x = 4;", "gh.p = &tx2;", "*T2 q = gh.p; sink(q.x);", "gh.p = &tx1;",
      B(CX_SELF) | B(CX_PAIR_R), 0, 0 },
    { "path: field through a pointer field | the pointee's field", NODE_FIELD, 1,
      "struct T2 { u32 x; }\nT2 tx1;\nstruct H { *T2 p; u32 n; }\nH gh;\n",
      "gh.p.x = 4;", "tx1.x = 7;", "u32 rv = tx1.x; sink(rv);", "gh.p = &tx1;", 0, 0, 0 },
    { "path: pointer field of an element | that field re-aimed", NODE_UNARY, 0,
      "u32 a1;\nu32 a2;\nstruct H { *u32 p; u32 n; }\nH[2] gha;\n",
      "*gha[1].p = 4;", "gha[1].p = &a2;", "*u32 q = gha[1].p; sink(*q);", "gha[0].p = &a1; gha[1].p = &a1;",
      B(CX_SELF) | B(CX_PAIR_R), 0, 0 },
    { "path: slice field element | the slice field re-aimed", NODE_INDEX, 0,
      A_DECL "u32[4] gb;\nstruct V { [*]u32 s; u32 n; }\nV gv;\n",
      "gv.s[1] = 3;", "gv.s = gb[0..4];", "sink((u32)gv.s.len);", "gv.s = ga[0..4];",
      B(CX_SELF) | B(CX_PAIR_R), 0, 0 },
    { "path: handle field | a sibling of the handle", NODE_FIELD, 1,
      "struct T { u32 v; }\nSlab(T) gslab;\nstruct HH { Handle(T) h; u32 n; }\nHH gk;\n",
      "gk.h.v = 1;", "gk.h.v = 7;", "u32 rv = gk.h.v; sink(rv);", "gk.h = gslab.alloc() orelse return;", 0, 0, 0 },
    /* H: an index is appended only when it is a compile-time constant */
    { "path: const-named index | the same element", NODE_INDEX, 1, A_DECL "const u32 K = 1;\n",
      "ga[K] = 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "path: computed index | the same element", NODE_INDEX, 1, A_DECL,
      "ga[3 - 2] = 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "path: index read from a global | any element", NODE_INDEX, 1, A_DECL "u32 gi;\n",
      "ga[gi & 3] = 3;", A_W, A_R, NULL, 0, 0, 0 },
    { "path: two-dimensional, same cell", NODE_INDEX, 1, "u32[2][2] gm;\n",
      "gm[1][0] = 3;", "gm[1][0] = 7;", "u32 rv = gm[1][0]; sink(rv);", NULL, 0, 0, 0 },
    { "apart: two-dimensional, different cells", NODE_INDEX, 1, "u32[2][2] gm;\n",
      "gm[1][0] = 3;", "gm[1][1] = 7;", "u32 rv = gm[1][1]; sink(rv);", NULL, 0, B(CX_COARSE), APART_W },
    { "path: two-dimensional, a row | a cell of it", NODE_INDEX, 1, "u32[2][2] gm;\n",
      "u32[2] row; row[0] = 1; row[1] = 2; gm[1] = row;", "gm[1][1] = 7;", "u32 rv = gm[1][1]; sink(rv);", NULL, 0, 0, 0 },

    /* ================= an ATOMIC access beside a plain one (BUG-1677) ======= *
     * The thread's only shared access is atomic, so the spawn scan finds it
     * CLEAN and nothing downgrades: the judgement must still compare it with
     * whoever touches the cell plainly. Atomic beside atomic (CX_SELF) is no
     * race. Every route to the cell, every route of the plain partner. */
    #define ATOMIC_SELF_OK B(CX_SELF)
    { "atomic: element | plain element", NODE_INTRINSIC, 1, A_DECL,
      "@atomic_add(&ga[1], 1);", A_W, A_R, NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: element, variable index | plain element", NODE_INTRINSIC, 1, A_DECL,
      "u32 k = 1; @atomic_add(&ga[k], 1);", A_W, A_R, NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: nested field | plain nested field", NODE_INTRINSIC, 1,
      "struct I { u32 x; u32 y; }\nstruct D { I in; u32 b; }\nD gd;\n",
      "@atomic_add(&gd.in.x, 1);", "gd.in.x = 7;", "u32 rv = gd.in.x; sink(rv);", NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: through a local pointer | plain", NODE_INTRINSIC, 1, G_DECL,
      "*u32 p = &g; @atomic_add(p, 1);", G_W, G_R, NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: through a global pointer | plain", NODE_INTRINSIC, 1, G_DECL "*u32 gp = &g;\n",
      "@atomic_add(gp, 1);", G_W, G_R, NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: through a returned pointer | plain", NODE_INTRINSIC, 1, G_DECL "*u32 getp() { return &g; }\n",
      "@atomic_add(getp(), 1);", G_W, G_R, NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: through a struct pointer | plain field", NODE_INTRINSIC, 1, S_DECL,
      "*S p = &gs; @atomic_add(&p.a, 1);", S_W, S_R, NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: through a slice view | plain element", NODE_INTRINSIC, 1, A_DECL,
      /* forming the view counts as a plain READ of `ga`: beside another
       * instance's atomic write that is refused (an over-rejection) */
      "[*]u32 s = ga[0..4]; @atomic_add(&s[1], 1);", A_W, A_R, NULL, 0, B(CX_SELF), ATOMIC_SELF_OK },
    { "atomic: cas | plain", NODE_INTRINSIC, 1, G_DECL,
      "if (@atomic_cas(&g, 0, 1)) { sink(1); }", G_W, G_R, NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: xchg | plain", NODE_INTRINSIC, 1, G_DECL,
      "u32 old = @atomic_xchg(&g, 1); sink(old);", G_W, G_R, NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: load | plain write", NODE_INTRINSIC, 0, S_DECL,
      "u32 x = @atomic_load(&gs.a); sink(x);", S_W, S_R, NULL, 0, 0, 0 },
    { "atomic: thread parameter | plain", NODE_INTRINSIC, 1, G_DECL,
      /* (lending one object to two live threads is refused whatever they do) */
      "@atomic_add(p, 1);", G_W, G_R, NULL, 0, B(CX_SELF), ATOMIC_SELF_OK, "*u32 p", "&g", "p" },
    { "atomic: field by thread parameter | plain field", NODE_INTRINSIC, 1, S_DECL,
      "@atomic_add(p, 1);", S_W, S_R, NULL, 0, B(CX_SELF), ATOMIC_SELF_OK, "*u32 p", "&gs.a", "p" },
    /* the plain partner by another route */
    { "atomic: scalar | plain through a global pointer", NODE_INTRINSIC, 1, G_DECL "*u32 gp = &g;\n",
      "@atomic_add(&g, 1);", "*gp = 7;", "u32 rv = *gp; sink(rv);", NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: scalar | plain through a callee's parameter", NODE_INTRINSIC, 1,
      G_DECL "void bump(*u32 q) { *q += 1; }\nu32 peek(*u32 q) { return *q; }\n",
      "@atomic_add(&g, 1);", "bump(&g);", "sink(peek(&g));", NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: field | plain through a struct pointer", NODE_INTRINSIC, 1, S_DECL "*S gsp = &gs;\n",
      "@atomic_add(&gs.a, 1);", "gsp.a = 7;", "u32 rv = gsp.a; sink(rv);", NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: field | plain whole-struct copy", NODE_INTRINSIC, 1, S_DECL,
      "@atomic_add(&gs.a, 1);", "S z; z.a = 0; z.b = 0; gs = z;", "S z = gs; sink(z.b);", NULL, 0, 0, ATOMIC_SELF_OK },
    { "atomic: element | plain through a slice", NODE_INTRINSIC, 1, A_DECL "void fill([*]u32 s) { s[1] = 1; }\n",
      "@atomic_add(&ga[1], 1);", "fill(ga[0..4]);", A_R, NULL, 0, 0, ATOMIC_SELF_OK },

    /* ================= a synchronisation OBJECT (BUG-1678) =================== *
     * A Barrier synchronises its waiters, not its own set-up: @barrier_init
     * rewrites the object with plain stores. A wait beside a wait is no race;
     * a set-up beside either is. */
    { "sync object: barrier set-up", NODE_INTRINSIC, 1, "Barrier gb;\n",
      "@barrier_init(gb, 2);", "@barrier_init(gb, 2);", "@barrier_wait(gb);", NULL, 0, 0, 0 },
    { "sync object: barrier wait", NODE_INTRINSIC, 0, "Barrier gb;\n",
      "@barrier_wait(gb);", "@barrier_init(gb, 2);", "@barrier_wait(gb);", "@barrier_init(gb, 2);", 0, 0, 0 },
};
#pragma GCC diagnostic pop
#define NFORMS ((int)(sizeof(forms) / sizeof(forms[0])))

/* ---- the tie to the AST ----
 * Every node kind is either a place where an access can stand / be made
 * (KR_CARRIES: a row must name it) or one that cannot (KR_NONE, with the reason).
 * No default: a new NodeKind does not build until it is put on one side. */
typedef enum { KR_NONE, KR_CARRIES } KindRole;
static KindRole kind_role(NodeKind k) {
    switch (k) {
    /* declarations: no thread executes one. (A global's INITIALISER is a
     * constant expression, evaluated before any thread exists.) */
    case NODE_FILE: case NODE_FUNC_DECL: case NODE_STRUCT_DECL: case NODE_ENUM_DECL:
    case NODE_UNION_DECL: case NODE_TYPEDEF: case NODE_IMPORT: case NODE_CINCLUDE:
    case NODE_MMIO: case NODE_GLOBAL_VAR: case NODE_CONTAINER_DECL:
        return KR_NONE;
    /* an interrupt handler is not a thread: the ISR rules are their own class
     * (tests/test_hw_matrix.c), and race_free.v does not model them */
    case NODE_INTERRUPT:
        return KR_NONE;
    /* no operand, no object */
    case NODE_BREAK: case NODE_CONTINUE:
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_CHAR_LIT: case NODE_BOOL_LIT:
    case NODE_NULL_LIT:
        return KR_NONE;
    /* read-only storage: two readers never race, and nothing may write it */
    case NODE_STRING_LIT:
        return KR_NONE;
    /* evaluated by the compiler; a type operand names no object */
    case NODE_STATIC_ASSERT: case NODE_SIZEOF:
        return KR_NONE;
    /* vestigial: no pass builds one (checker.c says so where it lists it) */
    case NODE_CAST:
        return KR_NONE;
    /* everything a function body is made of */
    case NODE_VAR_DECL: case NODE_BLOCK: case NODE_IF: case NODE_FOR: case NODE_WHILE:
    case NODE_SWITCH: case NODE_RETURN: case NODE_DEFER: case NODE_GOTO: case NODE_LABEL:
    case NODE_EXPR_STMT: case NODE_ASM: case NODE_CRITICAL: case NODE_ONCE:
    case NODE_SPAWN: case NODE_YIELD: case NODE_AWAIT: case NODE_DO_WHILE:
    case NODE_IDENT: case NODE_BINARY: case NODE_UNARY: case NODE_ASSIGN: case NODE_CALL:
    case NODE_FIELD: case NODE_INDEX: case NODE_SLICE: case NODE_ORELSE:
    case NODE_INTRINSIC: case NODE_TYPECAST: case NODE_STRUCT_INIT:
        return KR_CARRIES;
    }
    return KR_NONE;
}

/* ---- what the definition of a data race says about a cell ---- */
static int expect_reject(const Form *f, Ctx c) {
    switch (c) {
    /* the form (a read or a write) beside a WRITE of the same object */
    case CX_PAIR_W: case CX_THR_WIN: case CX_WIN_THR: case CX_HELPER:
    case CX_GRAND: case CX_DETACH: case CX_COARSE: case CX_WIN_CALL:
        return 1;
    /* beside a read, or beside itself: a race only if the form writes */
    case CX_PAIR_R: case CX_SELF:
        return f->writes;
    /* one lock around both; or the thread is joined before the other runs */
    case CX_LOCKED: case CX_AFTER: case CX_SEQ:
        return 0;
    case CX__N: break;
    }
    return 0;
}

#define ATOM "@atomic_add(&ctr, 1); "
#define LK   "while (!@atomic_cas(&lk, 0, 1)) { } "
#define UL   " @atomic_store(&lk, 0);"

static void build(char *out, size_t cap, const Form *f, Ctx c) {
    const char *init = f->init ? f->init : "";
    const char *sig = f->tsig ? f->tsig : "";
    const char *arg = f->targ ? f->targ : "";
    const char *fwd = f->tfwd ? f->tfwd : "";
    /* the parameter as a local, where the form runs outside its thread */
    char loc[256]; loc[0] = 0;
    if (f->tsig) snprintf(loc, sizeof(loc), "%s = %s; ", f->tsig, f->targ);
    const char *pre = "u32 lk;\nu32 ctr;\nvoid sink(u32 v) { }\n";
    switch (c) {
    case CX_PAIR_W: case CX_PAIR_R:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM "%s }\nvoid t2() { " ATOM "%s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); ThreadHandle b = spawn t2(); "
                 "a.join(); b.join(); return 0; }\n",
                 pre, f->decl, sig, f->stmt, c == CX_PAIR_W ? f->wpart : f->rpart, init, arg);
        return;
    case CX_SELF:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM "%s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); ThreadHandle b = spawn t1(%s); "
                 "a.join(); b.join(); return 0; }\n", pre, f->decl, sig, f->stmt, init, arg, arg);
        return;
    case CX_THR_WIN:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM "%s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); %s a.join(); return 0; }\n",
                 pre, f->decl, sig, f->stmt, init, arg, f->wpart);
        return;
    case CX_WIN_THR:
        snprintf(out, cap, "%s%svoid t2() { " ATOM "%s }\n"
                 "void body() { %sThreadHandle b = spawn t2(); %s b.join(); }\n"
                 "u32 main() { %s body(); return 0; }\n",
                 pre, f->decl, f->wpart, loc, f->stmt, init);
        return;
    case CX_HELPER:
        snprintf(out, cap, "%s%svoid hlp(%s) { %s }\nvoid t1(%s) { " ATOM "hlp(%s); }\n"
                 "void t2() { " ATOM "%s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); ThreadHandle b = spawn t2(); "
                 "a.join(); b.join(); return 0; }\n",
                 pre, f->decl, sig, f->stmt, sig, fwd, f->wpart, init, arg);
        return;
    case CX_GRAND:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM "%s }\n"
                 "void t0(%s) { " ATOM "ThreadHandle q0 = spawn t1(%s); q0.join(); }\n"
                 "void t2() { " ATOM "%s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t0(%s); ThreadHandle b = spawn t2(); "
                 "a.join(); b.join(); return 0; }\n",
                 pre, f->decl, sig, f->stmt, sig, fwd, f->wpart, init, arg);
        return;
    case CX_DETACH:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM "%s }\n"
                 "u32 main() { %s spawn t1(%s); %s return 0; }\n",
                 pre, f->decl, sig, f->stmt, init, arg, f->wpart);
        return;
    case CX_COARSE:
        snprintf(out, cap, "%s%svoid t1(%s) { %s }\nvoid t2() { %s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); ThreadHandle b = spawn t2(); "
                 "a.join(); b.join(); return 0; }\n",
                 pre, f->decl, sig, f->stmt, f->wpart, init, arg);
        return;
    case CX_WIN_CALL:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM "%s }\nvoid wc() { %s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); wc(); a.join(); return 0; }\n",
                 pre, f->decl, sig, f->stmt, f->wpart, init, arg);
        return;
    case CX_LOCKED:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM LK "{ %s }" UL " }\n"
                 "void t2() { " ATOM LK "{ %s }" UL " }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); ThreadHandle b = spawn t2(); "
                 "a.join(); b.join(); return 0; }\n",
                 pre, f->decl, sig, f->stmt, f->wpart, init, arg);
        return;
    case CX_AFTER:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM "%s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); a.join(); %s return 0; }\n",
                 pre, f->decl, sig, f->stmt, init, arg, f->wpart);
        return;
    case CX_SEQ:
        snprintf(out, cap, "%s%svoid t1(%s) { " ATOM "%s }\nvoid t2() { " ATOM "%s }\n"
                 "u32 main() { %s ThreadHandle a = spawn t1(%s); a.join(); "
                 "ThreadHandle b = spawn t2(); b.join(); return 0; }\n",
                 pre, f->decl, sig, f->stmt, f->wpart, init, arg);
        return;
    case CX__N: break;
    }
    out[0] = 0;
}

/* 0 compiled clean, 1 rejected for a race, 2 rejected for something else,
 * 3 rejected by the parser (the probe itself is malformed) */
static int verdict(const char *code, char *why, size_t wcap) {
    FILE *f = fopen(ZT("/tmp/_zer_ax.zer"), "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); exit(2); }
    fputs(code, f); fclose(f);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), ZT("%s /tmp/_zer_ax.zer -o /tmp/_zer_ax_out.c >/dev/null 2>/tmp/_zer_ax.err"),
             zerc_path);
    why[0] = 0;
    if (system(cmd) == 0) return 0;
    FILE *e = fopen(ZT("/tmp/_zer_ax.err"), "r");
    int race = 0, parse = 0;
    char line[4096];
    while (e && fgets(line, sizeof(line), e)) {
        /* a warning proves nothing; zercheck reports as `file:N: zercheck: ...` */
        if (!strstr(line, ": error: ") && !strstr(line, ": zercheck: ")) continue;
        if (!why[0]) snprintf(why, wcap, "%.200s", line);
        if (strstr(line, "expected ") || strstr(line, "unexpected") || strstr(line, "parse error"))
            parse = 1;
        if (strstr(line, "data race") || strstr(line, "both access") ||
            strstr(line, "accessed atomically") || strstr(line, "@atomic"))
            race = 1;
    }
    if (e) fclose(e);
    if (parse) return 3;
    return race ? 1 : 2;
}

int main(void) {
    find_zerc();
    /* the tie: every node kind that can carry an access has a row */
    int tie_ok = 1;
    for (int k = 0; k <= (int)NODE_STRUCT_INIT; k++) {
        if (kind_role((NodeKind)k) != KR_CARRIES) continue;
        int n = 0;
        for (int i = 0; i < NFORMS; i++) if ((int)forms[i].car == k) n++;
        if (n == 0) {
            fprintf(stderr, "FAIL [NO-ROW] node kind %d can carry an access and no form exercises it\n", k);
            tie_ok = 0;
        }
    }
    if (!tie_ok) return 1;

    int cells = 0, ok = 0, false_neg = 0, wrong_reason = 0, bad_probe = 0;
    int over = 0, over_known = 0, stale = 0;
    static char code[8192];
    char why[256];
    for (int i = 0; i < NFORMS; i++) {
        const Form *f = &forms[i];
        for (int ci = 0; ci < CX__N; ci++) {
            Ctx c = (Ctx)ci;
            if (f->skip & B(c)) continue;
            int rej = expect_reject(f, c);
            if (f->excused & B(c)) rej = 0;
            int known_over = (f->over & B(c)) != 0;
            build(code, sizeof(code), f, c);
            int v = verdict(code, why, sizeof(why));
            cells++;
            const char *tag = "ok";
            int good = 0;
            if (v == 3) { bad_probe++; tag = "INVALID-PROBE"; }
            else if (rej) {
                if (v == 1) good = 1;
                else if (v == 0) { false_neg++; tag = "FALSE-NEGATIVE"; }
                else { wrong_reason++; tag = "WRONG-REASON"; }
            } else if (known_over) {
                if (v == 1) { good = 1; over_known++; tag = "known over-rejection"; }
                else if (v == 0) { stale++; tag = "STALE-OVER (now accepted: clear the bit)"; }
                else { wrong_reason++; tag = "WRONG-REASON"; }
            } else {
                if (v == 0) good = 1;
                else if (v == 1) { over++; tag = "OVER-REJECT"; }
                else { wrong_reason++; tag = "WRONG-REASON"; }
            }
            if (good) ok++;
            if (!good || known_over) {
                fprintf(stderr, "  %s [%s][%s] %s\n", good ? "note" : "FAIL", f->name, ctx_name(c), tag);
                if (!good) {
                    if (why[0]) fprintf(stderr, "    %s", why);
                    fprintf(stderr, "--- program ---\n%s--- end ---\n", code);
                }
            }
        }
    }
    fprintf(stderr, "\n=== access-matrix: %d/%d cells correct ===\n", ok, cells);
    fprintf(stderr, "    %d forms x %d contexts | false negatives: %d | wrong reason: %d | "
            "invalid probes: %d | over-rejections: %d new, %d known | stale: %d\n",
            NFORMS, (int)CX__N, false_neg, wrong_reason, bad_probe, over, over_known, stale);
    if (ok != cells) {
        fprintf(stderr, "ACCESS MATRIX HAS HOLES — a construct that touches memory is not judged\n"
                        "(FALSE-NEGATIVE: two threads race over it and the program compiles).\n");
        return 1;
    }
    return 0;
}
