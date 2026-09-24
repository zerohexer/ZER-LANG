#!/bin/bash
# Run all .zer integration tests
# tests/zer/       — must compile + run + exit 0  (positive tests)
# tests/zer_fail/  — must FAIL to compile         (negative tests)
# tests/zer_trap/  — must compile + run + EXIT NON-ZERO (runtime safety traps)
# tests/zer_proof/ — theorem-linked regression tests (see README)
#                    — *_bad.zer must FAIL to compile
#                    — other .zer files must compile + run + exit 0
# Usage: test_zer.sh [extra-flags]
#   e.g. test_zer.sh --some-future-flag

# ZER_MATRIX_ZERC overrides the compiler under test, exactly as all ten
# tests/test_*_matrix.c grids accept it (CLAUDE.md records why: verifying that a
# new test actually FIRES means running it against a PRE-FIX compiler, and a
# harness that hardcodes ./zerc silently grades the current one instead — so a
# run "against the baseline" proves nothing). The integration runner was the last
# harness without the override; added 2026-09-15 while proving BUG-1019's trap
# tests discriminate.
# Per-run diagnostic files. These were the fixed paths /tmp/_zer_neg_err.txt and
# /tmp/_zer_gap_err.txt, so two concurrent runs (a `make check` beside a hand run,
# or two worktrees) read each other's diagnostics and reported "rejected, but for
# the WRONG REASON" on correct negatives — 84 of them in one measured collision.
NEG_ERR=$(mktemp "${TMPDIR:-/tmp}/zer_neg_err.XXXXXX")
GAP_ERR=$(mktemp "${TMPDIR:-/tmp}/zer_gap_err.XXXXXX")
trap 'rm -f "$NEG_ERR" "$GAP_ERR"' EXIT
ZERC="${ZER_MATRIX_ZERC:-./zerc}"
EXTRA_FLAGS="$1"
PASS=0
FAIL=0
SKIP=0
TOTAL=0

# Tests with known pre-existing failures, documented in docs/limitations.md.
# Empty — BUG-590 closed the shadowing case, everything in tests/zer/
# compiles + runs + exits 0.
KNOWN_FAIL_POSITIVE=""

is_known_fail() {
    local needle="$1"
    for name in $KNOWN_FAIL_POSITIVE; do
        if [ "$name" = "$needle" ]; then return 0; fi
    done
    return 1
}

echo "=== ZER Integration Tests (positive) ${EXTRA_FLAGS:+[$EXTRA_FLAGS]} ==="

for f in tests/zer/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    # Per-file flags: first line `// zerc-flags: --foo --bar=baz` is parsed
    # and appended to ZERC invocation. Used for tests that need specific
    # target features (e.g., --target-features=avx512f).
    file_flags=$(head -5 "$f" | grep -oE '// zerc-flags: .*$' | sed 's|// zerc-flags: ||')
    # Timeout (2026-06-21): a positive test must compile + run + exit 0 within
    # ZER_RUN_TIMEOUT seconds. A DEADLOCK (e.g. a botched auto-lock leaving a
    # mutex held) would otherwise hang the whole `make check`; `timeout` makes
    # it a visible FAIL (exit 124) instead — the concurrency stress tests rely
    # on this so a latent deadlock surfaces in CI rather than shipping silently.
    ZER_RUN_TIMEOUT="${ZER_RUN_TIMEOUT:-30}"
    timeout "$ZER_RUN_TIMEOUT" $ZERC "$f" $EXTRA_FLAGS $file_flags --run 2>/dev/null
    ret=$?
    if [ $ret -eq 0 ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name"
    else
        if is_known_fail "$name"; then
            SKIP=$((SKIP + 1))
            echo "  SKIP: $name (exit $ret — known pre-existing issue, docs/limitations.md)"
        else
            FAIL=$((FAIL + 1))
            if [ $ret -eq 124 ]; then
                echo "  FAIL: $name (TIMEOUT after ${ZER_RUN_TIMEOUT}s — possible deadlock)"
            else
                echo "  FAIL: $name (exit $ret)"
            fi
            timeout "$ZER_RUN_TIMEOUT" $ZERC "$f" $EXTRA_FLAGS $file_flags --run 2>&1 | head -5
        fi
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
done

echo ""
echo "=== ZER Runtime-Trap Tests (must compile + trap at runtime) ==="

for f in tests/zer_trap/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    # Runtime-trap tests: compile clean, run, EXPECT non-zero exit (SIGTRAP = 133).
    # Per-file flags via '// zerc-flags: ...' first line (same as positive/negative
    # sections) — e.g. BUG-736's --no-strict-mmio alignment-trap test.
    file_flags=$(head -5 "$f" | grep -oE '// zerc-flags: .*$' | sed 's|// zerc-flags: ||')
    # BUG-835 harness, TWO weak-oracle holes closed:
    #  * NO TIMEOUT. A trap test whose program HANGS did not pass vacuously — it
    #    hung `make check` forever, so a regression test for the held-lock guard
    #    (which deadlocks pre-fix) could not have lived in the suite at all.
    #    124 is now an explicit FAIL: a hang is not a trap.
    #  * "any non-zero exit" is a WEAK ORACLE — a hang, a SIGSEGV, a wrong answer
    #    and a genuine safety trap were indistinguishable. Optional `// expect-trap`
    #    demands SIGTRAP (133) specifically, the same opt-in shape as
    #    `// expect-error` for negatives, so existing tests that abort by other
    #    means keep working.
    want_trap=$(head -5 "$f" | grep -c '// expect-trap$\|// expect-trap ')
    #  * WHERE it trapped was invisible. stderr went to /dev/null, so a trap test
    #    could not assert the message OR the reported source line — and BUG-1003
    #    lived under this suite the whole time: every trap named a line that does
    #    not exist in the file (function-declaration line + offset in the
    #    generated C), because IR block emission switched #line mapping off
    #    wholesale. The number looks plausible, which is exactly why no test
    #    caught it. `// expect-trap-at: N` asserts the reported location; the
    #    optional-directive shape matches `expect-error` and `expect-trap`.
    want_line=$(head -8 "$f" | grep -oE '// expect-trap-at: *[0-9]+' | grep -oE '[0-9]+' | head -1)
    #  * WHICH trap fired was invisible. `expect-trap` demands SIGTRAP and
    #    `expect-trap-at` demands a line, but NEITHER says which safety rule
    #    produced it — so a trap test passes when a DIFFERENT rule traps on the
    #    same program. That is the weak-oracle class this file already documents
    #    for negatives ("a negative test proves nothing until you read the
    #    diagnostic"), one directory over, and it was live: BUG-1019's null
    #    function-pointer call trapped identically before and after the fix,
    #    because HOSTED the raw jump to address 0 faults and ZER's SIGSEGV
    #    handler traps — a different mechanism, the same exit code, and on bare
    #    metal no trap at all. `// expect-trap-msg: <substring>` asserts the
    #    reason, same optional-directive shape as `expect-error`.
    want_msg=$(head -8 "$f" | sed -n 's|^// expect-trap-msg: *||p' | head -1)
    trap_out=$(timeout "${ZER_RUN_TIMEOUT:-20}" $ZERC "$f" $EXTRA_FLAGS $file_flags --run 2>&1)
    ret=$?
    if [ $ret -eq 124 ]; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (TIMED OUT — a hang is not a trap)"
    elif [ "$want_trap" -gt 0 ] && [ $ret -ne 133 ]; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (expected SIGTRAP/133, got exit $ret)"
    elif [ -n "$want_msg" ] && ! printf '%s' "$trap_out" | grep -qF -- "$want_msg"; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (expected trap message '$want_msg', got: $(printf '%s' "$trap_out" | grep -o 'ZER TRAP:.*' | head -1))"
    elif [ -n "$want_line" ] && ! printf '%s' "$trap_out" | grep -qE "\.zer:$want_line\b"; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (expected trap at line $want_line, got: $(printf '%s' "$trap_out" | grep -o 'ZER TRAP:.*' | head -1))"
    elif [ $ret -ne 0 ]; then
        PASS=$((PASS + 1))
        echo "  PASS: $name (correctly trapped, exit $ret)"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (should have trapped at runtime but exited 0)"
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
done

echo ""
echo "=== ZER Integration Tests (negative — must fail) ==="

for f in tests/zer_fail/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    # Per-file flags directive (same as positive branch). Some negatives
    # only fail under specific compiler configurations (e.g.,
    # --probe-mode=disabled rejecting @probe usage).
    file_flags=$(head -5 "$f" | grep -oE '// zerc-flags: .*$' | sed 's|// zerc-flags: ||')
    # Compile only (not --run), expect failure
    # 2026-08-03: capture the diagnostic instead of discarding it, so a test can
    # assert WHY it was rejected. Before this, a negative passed on ANY non-zero
    # exit — so an entire safety rule could be deleted and its negatives would
    # keep passing, as long as some OTHER rule happened to reject the same file.
    # That is a silent-green generator under every negative test. Measured twice
    # on the cross-block scoped-borrow entry: first a branch condition tripped
    # the read check, then the ThreadHandle leak check fired before the borrow
    # check was ever consulted.
    #
    # `// expect-error: <substring>` in the first 5 lines makes the reason an
    # ASSERTION. The directive is OPTIONAL — a file without one keeps the old
    # exit-code-only behaviour, so nothing breaks and it can be backfilled
    # highest-value-first.
    want=$(head -5 "$f" | grep -oE '// expect-error: .*$' | sed 's|// expect-error: ||')
    $ZERC "$f" $EXTRA_FLAGS $file_flags -o /dev/null 2>"$NEG_ERR"
    ret=$?
    if [ $ret -ne 0 ]; then
        # `-- ` before the pattern: without it grep parses an expect-error string
        # that STARTS WITH A DASH as its own options. Measured on
        # cli_bad_stack_limit, whose expected substring is
        # "--stack-limit must be a positive integer" — the diagnostic contained it
        # verbatim and the test still reported WRONG REASON. Any rule whose message
        # opens with a flag name was untestable through this harness.
        if [ -n "$want" ] && ! grep -qF -- "$want" "$NEG_ERR"; then
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (rejected, but for the WRONG REASON)"
            echo "        expected to contain: $want"
            echo "        actual: $(head -1 "$NEG_ERR" | cut -c1-100)"
        else
            PASS=$((PASS + 1))
            echo "  PASS: $name (correctly rejected)"
        fi
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (should have been rejected but compiled!)"
    fi
    : > "$NEG_ERR"
    rm -f "${f%.zer}.c" 2>/dev/null
done

echo ""
echo "=== ZER gap reproducers (tests/zer_gaps/) ==="
# 2026-08-03: this directory was executed by NOTHING for its whole existence.
# A triage found 23 of 43 files no longer exhibited their gap — 18 had been
# silently CLOSED (a safety rule fixed with no regression test to prove it), 5
# were MASKED by a different rule (the gap still open but invisible), and 1 no
# longer parsed. A parking lot, not a net.
#
# Expectation is INVERTED: a gap file is supposed to COMPILE, because
# compile-clean IS the gap. Three outcomes:
#   compiles                      -> ok, gap still open as recorded
#   rejected, gap-masked-by hits  -> ok, a KNOWN masking, deliberately recorded
#   rejected, anything else       -> FAIL: triage it
# so a gap closing is LOUD (promote it) and a gap becoming masked is loud too
# (the probe stopped testing what it claims). `// gap-masked-by: <substring>`
# in the header is the justification, same shape as an audit baseline row.
GAP_TOTAL=0; GAP_OK=0; GAP_FAIL=0
for f in tests/zer_gaps/*.zer; do
    [ -e "$f" ] || continue
    name=$(basename "$f" .zer)
    GAP_TOTAL=$((GAP_TOTAL + 1))
    maskby=$(head -3 "$f" | grep -oE '// gap-masked-by: .*$' | sed 's|// gap-masked-by: ||')
    $ZERC "$f" $EXTRA_FLAGS -o /dev/null 2>"$GAP_ERR"
    gret=$?
    # `-o /dev/null` is a NON-.c path, so zerc builds the exe NEXT TO THE SOURCE
    # (CLAUDE.md "zerc -o gotchas"). Clean both artefacts or the tree fills with
    # untracked binaries.
    rm -f "${f%.zer}.c" "${f%.zer}" 2>/dev/null
    if [ $gret -eq 0 ]; then
        GAP_OK=$((GAP_OK + 1))
        echo "  ok:   $name (gap still open)"
    # `--` here for the same reason as the expect-error site above: a
    # gap-masked-by string that STARTS WITH A DASH would be parsed by grep as
    # its own options. The two sites answer the same question and must not
    # drift; fixing only the one that had been measured is the sibling-site
    # mistake this project keeps recording.
    elif [ -n "$maskby" ] && grep -qF -- "$maskby" "$GAP_ERR"; then
        GAP_OK=$((GAP_OK + 1))
        echo "  ok:   $name (known masking: $maskby)"
    else
        GAP_FAIL=$((GAP_FAIL + 1))
        echo "  FAIL: $name — no longer exhibits its gap"
        echo "        $(head -1 "$GAP_ERR" | cut -c1-96)"
        echo "        -> if the gap CLOSED: verify it is the right rule, move to"
        echo "           tests/zer_fail/ with an // expect-error:, update limitations.md"
        echo "        -> if a DIFFERENT rule now masks it: add // gap-masked-by: <substring>"
    fi
    : > "$GAP_ERR"
done
echo "  gaps: $GAP_OK/$GAP_TOTAL accounted for, $GAP_FAIL needing triage"
if [ $GAP_FAIL -ne 0 ]; then
    FAIL=$((FAIL + GAP_FAIL))
fi

echo ""
echo "=== ZER Proof-linked tests (tests/zer_proof/) ==="
# Each *.zer here exercises a proven Iris theorem.
# *_bad.zer = violation program (must FAIL to compile).
# Others = safe programs (must compile + run + exit 0).

for f in tests/zer_proof/*.zer; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .zer)
    TOTAL=$((TOTAL + 1))
    if [[ "$name" == *_bad ]]; then
        # Negative: must fail to compile.
        $ZERC "$f" -o /dev/null 2>/dev/null
        ret=$?
        if [ $ret -ne 0 ]; then
            PASS=$((PASS + 1))
            echo "  PASS: $name (correctly rejected — theorem holds)"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (PROOF VIOLATION — compiler accepted a program the Iris theorem rejects)"
        fi
    else
        # Positive: must compile + run + exit 0.
        $ZERC "$f" --run 2>/dev/null
        ret=$?
        if [ $ret -eq 0 ]; then
            PASS=$((PASS + 1))
            echo "  PASS: $name"
        else
            FAIL=$((FAIL + 1))
            echo "  FAIL: $name (safe program should compile + run)"
        fi
    fi
    rm -f "${f%.zer}.c" 2>/dev/null
done

echo ""
echo "=== ZER Warning Verification (must compile + warn + exit 0) ==="

# Verify auto-guard warnings are emitted for dynamic array UAF
warn_check() {
    local f="$1" pattern="$2" name="$3"
    TOTAL=$((TOTAL + 1))
    output=$($ZERC "$f" --run 2>&1)
    ret=$?
    if [ $ret -eq 0 ] && echo "$output" | grep -q -- "$pattern"; then
        PASS=$((PASS + 1))
        echo "  PASS: $name (warning verified)"
    elif [ $ret -ne 0 ]; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (exit $ret, expected 0)"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (no warning matching '$pattern')"
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
}

warn_check tests/zer/dyn_array_autoguard_crash.zer "auto-guard inserted" "autoguard-warning-emitted"
warn_check tests/zer/dyn_array_guard.zer "auto-guard inserted" "dynguard-warning-emitted"

echo ""
echo "=== ZER No-Warning Verification (must compile + NO warnings + exit 0) ==="

nowarn_check() {
    local f="$1" name="$2"
    TOTAL=$((TOTAL + 1))
    output=$($ZERC "$f" --run 2>&1)
    ret=$?
    if [ $ret -eq 0 ] && ! echo "$output" | grep -qi "warning"; then
        PASS=$((PASS + 1))
        echo "  PASS: $name (no warnings, zero overhead)"
    elif [ $ret -ne 0 ]; then
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (exit $ret, expected 0)"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: $name (unexpected warning — auto-guard fired on proven index)"
        echo "$output" | grep -i "warning" | head -3
    fi
    rm -f "${f%.zer}.c" "${f%.zer}.exe" "${f%.zer}" 2>/dev/null
}

nowarn_check tests/zer/no_autoguard_proven.zer "no-autoguard-all-proven"
nowarn_check tests/zer/no_autoguard_stress.zer "no-autoguard-stress-31-accesses"
nowarn_check tests/zer/inline_call_range.zer "no-autoguard-inline-call"
nowarn_check tests/zer/inline_range_deep.zer "no-autoguard-deep-chain"
nowarn_check tests/zer/guard_clamp_range.zer "no-autoguard-guard-clamp"
nowarn_check tests/zer/no_warn_u64_atomic_64bit.zer "no-warn-u64-atomic-64bit-target"

echo ""
echo "=== Results ==="
echo "  Passed:  $PASS"
echo "  Failed:  $FAIL"
echo "  Skipped: $SKIP (known issues — see docs/limitations.md)"
echo "  Total:   $TOTAL"

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "ZER INTEGRATION TESTS FAILED"
    exit 1
fi

echo ""
echo "ALL ZER TESTS PASSED"
