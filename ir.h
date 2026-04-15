/*
 * ZER IR — Mid-level Intermediate Representation
 *
 * Sits between checker (AST) and emitter/zercheck (C emission + safety analysis).
 * MIR-inspired: flat locals, basic blocks, tree expressions.
 * See docs/IR_Implementation.md for full design.
 *
 * Pipeline: ZER source → Parser → AST → Checker → IR → zercheck/emitter → C → GCC
 */

#ifndef ZER_IR_H
#define ZER_IR_H

#include <stdio.h>
#include "ast.h"
#include "types.h"

/* ================================================================
 * IR Local — one per variable/temp/capture in a function
 *
 * The flat local list IS the async state struct.
 * No enumeration of NODE_ types — lowering creates all locals.
 * ================================================================ */
typedef struct {
    int id;                  /* unique within function, 0-based */
    const char *name;        /* source name (for C emission + debugging) */
    uint32_t name_len;
    Type *type;              /* resolved type from checker */

    /* Classification (for analysis passes) */
    bool is_param;           /* function parameter */
    bool is_capture;         /* if-unwrap |val| or switch arm |val| */
    bool is_temp;            /* compiler-generated temp (_zer_or0, _zer_uw0) */
    bool is_static;          /* static local — NOT promoted to async state struct */

    int source_line;         /* for error messages + #line directives */
} IRLocal;

/* ================================================================
 * IR Instruction — one operation within a basic block
 *
 * Tree expressions kept in `expr` field for clean C emission.
 * Control flow is explicit (BRANCH, GOTO, RETURN — not nested).
 * Builtin methods (pool/slab/ring/arena) are distinct op kinds.
 * ================================================================ */
typedef enum {
    /* --- Core operations --- */
    IR_ASSIGN,           /* dest = expr */
    IR_CALL,             /* dest = func(args) or void func(args) */
    IR_BRANCH,           /* if (cond) goto true_bb else goto false_bb */
    IR_GOTO,             /* goto target_bb */
    IR_RETURN,           /* return expr (or void) */

    /* --- Async --- */
    IR_YIELD,            /* suspend coroutine, resume at next block */
    IR_AWAIT,            /* yield until condition true */

    /* --- Concurrency --- */
    IR_SPAWN,            /* spawn func(args) — fire-and-forget or scoped */
    IR_LOCK,             /* shared struct mutex lock */
    IR_UNLOCK,           /* shared struct mutex unlock */

    /* --- Pool/Slab/Ring/Arena builtins --- */
    IR_POOL_ALLOC,       /* pool.alloc() → ?Handle(T) */
    IR_POOL_FREE,        /* pool.free(h) */
    IR_POOL_GET,         /* pool.get(h) → *T */
    IR_SLAB_ALLOC,       /* slab.alloc() → ?Handle(T) */
    IR_SLAB_FREE,        /* slab.free(h) */
    IR_SLAB_FREE_PTR,    /* slab.free_ptr(*T) */
    IR_SLAB_ALLOC_PTR,   /* slab.alloc_ptr() → ?*T */
    IR_ARENA_ALLOC,      /* arena.alloc(T) → ?*T */
    IR_ARENA_ALLOC_SLICE,/* arena.alloc_slice(T, n) → ?[]T */
    IR_ARENA_RESET,      /* arena.reset() */
    IR_RING_PUSH,        /* ring.push(val) */
    IR_RING_POP,         /* ring.pop() → ?T */
    IR_RING_PUSH_CHECKED,/* ring.push_checked(val) → ?void */

    /* --- Interrupt/critical --- */
    IR_CRITICAL_BEGIN,   /* disable interrupts (per-arch) */
    IR_CRITICAL_END,     /* re-enable interrupts */

    /* --- Defer --- */
    IR_DEFER_PUSH,       /* push defer body onto stack */
    IR_DEFER_FIRE,       /* fire all pending defers (LIFO) */

    /* --- Intrinsics --- */
    IR_INTRINSIC,        /* @ptrcast, @size, @truncate, @bitcast, etc. */

    /* --- No-op (for IR structure) --- */
    IR_NOP,              /* placeholder — no code emitted */
} IROpKind;

typedef struct IRInst {
    IROpKind op;
    int dest_local;          /* LOCAL id for result (-1 = no dest / void) */
    Node *expr;              /* AST expression tree (kept for C emission) */
    int source_line;         /* for #line emission */

    /* Branch/goto targets (basic block indices) */
    int true_block;          /* IR_BRANCH: target if true */
    int false_block;         /* IR_BRANCH: target if false */
    int goto_block;          /* IR_GOTO: target block */
    int cond_local;          /* IR_BRANCH: if >= 0, branch on this LOCAL's has_value (orelse pattern) */

    /* Call/spawn operands */
    const char *func_name;
    uint32_t func_name_len;
    int arg_count;
    Node **args;             /* AST expression trees for arguments */
    bool is_scoped_spawn;    /* IR_SPAWN: ThreadHandle variant */
    const char *handle_name; /* IR_SPAWN: ThreadHandle variable name */
    uint32_t handle_name_len;

    /* Builtin operands */
    int obj_local;           /* LOCAL id of pool/slab/ring/arena */
    int handle_local;        /* LOCAL id of handle (for free/get) */
    Type *alloc_type;        /* for arena.alloc(T) — the T */

    /* Defer operand */
    Node *defer_body;        /* IR_DEFER_PUSH: AST of defer body (emitter walks it) */

    /* Intrinsic operand */
    const char *intrinsic_name;
    uint32_t intrinsic_name_len;
} IRInst;

/* ================================================================
 * IR Basic Block — sequence of instructions + terminator
 *
 * Last instruction is always a terminator (BRANCH, GOTO, RETURN, YIELD).
 * Predecessors computed after lowering for CFG analysis.
 * ================================================================ */
typedef struct {
    int id;                  /* unique within function, 0-based */
    const char *label;       /* optional: source label name (for goto targets) */
    uint32_t label_len;

    /* Instructions */
    IRInst *insts;
    int inst_count;
    int inst_capacity;

    /* Predecessors (filled by ir_compute_preds after lowering) */
    int *preds;
    int pred_count;
    int pred_capacity;
} IRBlock;

/* ================================================================
 * IR Function — the complete lowered representation
 *
 * locals[] IS the async state struct (complete by construction).
 * blocks[] IS the CFG (explicit edges, no hacks).
 * Entry block is always blocks[0].
 * ================================================================ */
typedef struct {
    const char *name;
    uint32_t name_len;
    Type *return_type;

    /* Function properties */
    bool is_async;
    bool is_interrupt;
    bool is_naked;

    /* Flat local list — THE async state struct */
    IRLocal *locals;
    int local_count;
    int local_capacity;

    /* Basic blocks — THE CFG */
    IRBlock *blocks;
    int block_count;
    int block_capacity;

    /* Source AST node (for checker cross-reference) */
    Node *ast_node;

    /* Module context */
    const char *module_prefix;
    uint32_t module_prefix_len;
} IRFunc;

/* ================================================================
 * IR Construction API (used by lowering pass)
 * ================================================================ */

/* Create a new IR function from arena */
IRFunc *ir_func_new(Arena *arena, const char *name, uint32_t name_len, Type *ret_type);

/* Add a local to the function. Returns the local ID. */
int ir_add_local(IRFunc *func, Arena *arena,
                 const char *name, uint32_t name_len, Type *type,
                 bool is_param, bool is_capture, bool is_temp, int line);

/* Look up a local by name. Returns local ID or -1 if not found. */
int ir_find_local(IRFunc *func, const char *name, uint32_t name_len);

/* Create a new basic block. Returns the block ID. */
int ir_add_block(IRFunc *func, Arena *arena);

/* Add an instruction to a basic block. */
void ir_block_add_inst(IRBlock *block, Arena *arena, IRInst inst);

/* ================================================================
 * IR Analysis API (used by zercheck, VRP, FuncProps)
 * ================================================================ */

/* Compute predecessor lists for all blocks (call after lowering) */
void ir_compute_preds(IRFunc *func, Arena *arena);

/* Check if a block's last instruction is a terminator */
bool ir_block_is_terminated(IRBlock *block);

/* ================================================================
 * IR Validation (call after lowering, before analysis/emission)
 * ================================================================ */

/* Validate IR structural invariants. Returns true if valid.
 * Prints errors to stderr if invalid (lowering bug). */
bool ir_validate(IRFunc *func);

/* ================================================================
 * IR Pretty-Printer (for debugging: zerc --emit-ir)
 * ================================================================ */

/* Print human-readable IR to FILE* (stdout or file) */
void ir_print(FILE *out, IRFunc *func);

/* ================================================================
 * IR Lowering (AST → IR)
 * ================================================================ */

/* Lower one function's typed AST to IR. Requires checker to have run.
 * Returns NULL if node is not a function or has no body.
 * Note: Checker is an opaque type here — include checker.h in the calling code. */
IRFunc *ir_lower_func(Arena *arena, void *checker, Node *func_decl);

/* Lower an interrupt handler body to IR. */
IRFunc *ir_lower_interrupt(Arena *arena, void *checker, Node *interrupt);

#endif /* ZER_IR_H */
