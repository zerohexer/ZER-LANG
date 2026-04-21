# zer_proof — theorem-linked regression tests

**Each `.zer` file here exercises a specific proven theorem from `proofs/operational/lambda_zer_handle/iris_*.v` or `proofs/operational/lambda_zer_move/iris_*.v`.**

The purpose: turn the Iris specs into a **concrete correctness oracle for the compiler**. If the compiler changes in a way that violates a proven invariant, the matching test here fails, and the test name tells you exactly which theorem was violated.

## Naming convention

```
tests/zer_proof/
  <SectionRow>_<description>.zer           — POSITIVE (must compile + run OK)
  <SectionRow>_<description>_bad.zer       — NEGATIVE (must FAIL to compile)
```

Examples:
- `A01_no_uaf.zer` — valid program exercising the no-UAF guarantee
- `A01_no_uaf_bad.zer` — program that violates the guarantee (must be rejected)
- `A06_no_double_free_bad.zer` — matches `alive_handle_exclusive` theorem

## How it ties to Iris theorems

Each test file's header references the specific theorem:

```zer
// Iris theorem: spec_get (iris_specs.v)
// Covers row: A01 (use-after-free prevention)
// Expected: FAIL to compile

Pool(Task, 4) tasks;
struct Task { u32 id; }

void main() {
    ?Handle(Task) mh = tasks.alloc();
    if (mh) |h| {
        tasks.free(h);
        u32 x = tasks.get(h).id;  // <-- UAF: zercheck must reject
    }
}
```

When the compiler regresses (e.g., zercheck starts accepting this program), the test name + theorem reference tells you EXACTLY which Iris theorem was violated.

## Running

`make check` runs all tests in `tests/zer_proof/` automatically (once wired into `tests/test_zer.sh` — pending).

Positive tests: `zerc file.zer --run` must exit 0.
Negative tests: `zerc file.zer -o /dev/null` must exit non-zero.

## Coverage status

**In progress.** Seed tests for sections A (UAF, DF, leak) and B (move) added 2026-04-21. Full coverage of 55+ proven theorems requires ~20-30 hours of test authoring.

See `docs/proof-internals.md` "Layer 2 — wiring proofs to tests" for the priority list.
