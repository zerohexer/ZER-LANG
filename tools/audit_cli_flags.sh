#!/bin/bash
# audit_cli_flags.sh — the driver must never silently ignore an argument.
#
# WHY THIS EXISTS. zerc's option loop had no final `else`, so any argument it
# did not recognise was dropped without a word. That is quiet in the worst way
# for the safety-relevant flags: `--no-strict-mmi` (one character short of
# --no-strict-mmio) compiled under STRICT mmio, and a mistyped `--target-bit 32`
# compiled for the host's pointer width — the exact configuration axis the
# `volatile u64` tearing check depends on. The user asked for one thing, got the
# opposite, and had no way to tell.
#
# This class has no home in tests/zer* (those harnesses are about SOURCE, not
# invocation), so it gets its own gate. Each row asserts an EXIT CODE and, for
# rejections, that the message names the offending argument — an exit-code-only
# check would pass if zerc died for some unrelated reason.
#
# Run from the repo root. Exits non-zero on any mismatch.

ZERC="${1:-./zerc}"
[ -x "$ZERC" ] || { echo "audit_cli_flags: no zerc at $ZERC"; exit 2; }

SRC=$(mktemp /tmp/_zer_cli_XXXXXX.zer)
printf 'u32 main() { return 0; }\n' > "$SRC"
OUT=$(mktemp /tmp/_zer_cli_out_XXXXXX.c)
FAIL=0
OK=0

# want_reject <expected-substring> <args...>
want_reject() {
    local want="$1"; shift
    local err
    err=$("$ZERC" "$SRC" "$@" -o "$OUT" 2>&1)
    local ret=$?
    if [ $ret -eq 0 ]; then
        echo "  FAIL: [$*] was ACCEPTED — the driver silently ignored it"
        FAIL=$((FAIL + 1))
    elif ! printf '%s' "$err" | grep -qF -- "$want"; then
        echo "  FAIL: [$*] rejected, but not for the expected reason"
        echo "        expected to contain: $want"
        echo "        actual: $(printf '%s' "$err" | head -1)"
        FAIL=$((FAIL + 1))
    else
        OK=$((OK + 1))
    fi
}

# want_accept <args...>
want_accept() {
    if "$ZERC" "$SRC" "$@" -o "$OUT" >/dev/null 2>&1; then
        OK=$((OK + 1))
    else
        echo "  FAIL: [$*] was REJECTED — a valid invocation must still work"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== CLI argument audit ==="

# --- unknown options ---
want_reject "unknown option" --totally-bogus-flag
want_reject "unknown option" --no-strict-mmi          # typo of --no-strict-mmio
want_reject "unknown option" -x

# --- options whose required value is missing ---
# NOTE: these must be the LAST argument, so want_reject (which appends `-o`)
# cannot express them — with a `-o` behind it the option is not value-less at
# all, it eats `-o` as its value. That mistake is what first surfaced the
# swallowed-option bug below, so the two helpers stay separate on purpose.
want_reject_last() {
    local want="$1"; shift
    local err; err=$("$ZERC" "$SRC" "$@" 2>&1); local ret=$?
    if [ $ret -eq 0 ]; then
        echo "  FAIL: [$*] (as last arg) was ACCEPTED"; FAIL=$((FAIL + 1))
    elif ! printf '%s' "$err" | grep -qF -- "$want"; then
        echo "  FAIL: [$*] rejected, but not for the expected reason"
        echo "        expected to contain: $want"
        echo "        actual: $(printf '%s' "$err" | head -1)"
        FAIL=$((FAIL + 1))
    else OK=$((OK + 1)); fi
}
want_reject_last "requires a value" --target-bits
want_reject_last "requires a value" --stack-limit
want_reject_last "requires a value" --gcc

# --- numeric values must be validated, not atoi'd ---
# Unchecked, every one of these produced a pointer width or stack limit of ZERO
# and compiled anyway, silently changing every width-dependent safety decision.
want_reject "--target-bits expects" --target-bits abc
want_reject "--target-bits expects" --target-bits 0
want_reject "--target-bits expects" --target-bits 128
want_reject "--stack-limit expects" --stack-limit abc
want_reject "--stack-limit expects" --stack-limit -5

# --- enumerated options with a bad value ---
want_reject "unknown --target-arch" --target-arch=sparc
want_reject "unknown --probe-mode" --probe-mode=bogus

# --- a second source file must not be silently dropped ---
want_reject "unexpected argument" /dev/null

# --- valid invocations must be unaffected ---
want_accept
want_accept --no-strict-mmio
want_accept --target-bits 32
want_accept --target-arch=aarch64
want_accept --probe-mode=raw
want_accept --target-features=avx512f
want_accept --lib
want_accept --stack-limit 4096

rm -f "$SRC" "$OUT" "${SRC%.zer}" "${SRC%.zer}.c" 2>/dev/null

if [ $FAIL -ne 0 ]; then
    echo "CLI AUDIT FAILED: $FAIL of $((OK + FAIL)) checks — an argument is handled silently."
    exit 1
fi
echo "OK — no silently-ignored CLI arguments ($OK checks)."
exit 0
