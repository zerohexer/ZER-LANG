#!/bin/bash
# ============================================================================
# audit_reference_examples.sh — compile the ```zer examples in the docs.
#
# WHY THIS EXISTS
# ---------------
# docs/reference.md is the ONLY reference a ZER user has: there is no ZER on
# the web to search, and no LLM has ZER in its training data. An example that
# does not compile is therefore worse than a missing one — it teaches a syntax
# the compiler rejects, and the reader has no second source to check against.
#
# Measured 2026-08-23: the "Safe C Library Interop" example in reference.md
# asserts that `defer sensor_close(dev)` compiles. It does not (it is reported
# as a leak). Nothing noticed, because nothing ever compiled the examples.
#
# WHAT IT DOES
# ------------
# Every fenced ```zer block that looks like a WHOLE PROGRAM (it defines `main`)
# is compiled with `zerc -o <tmp>.c`, i.e. the CHECKER's verdict only — GCC's
# opinion is deliberately excluded so a doc example is not held to
# hosted-x86-64 codegen rules (an `interrupt` block, for instance, cannot be
# linked on the host but is perfectly good ZER).
#
# Blocks that are FRAGMENTS (no `main`) are counted and reported, not compiled.
# That count is the standing exposure and is printed on every run so it can be
# driven down deliberately rather than drifting.
#
# Per-block directives, anywhere in the block:
#   // audit: skip <reason>              — do not compile; the reason is printed
#   // audit: expect-error <substring>   — MUST fail, and the diagnostic must
#                                          contain <substring>
#
# Usage: bash tools/audit_reference_examples.sh [zerc_path] [file ...]
# Exit:  0 iff every compiled example matched its expectation.
# ============================================================================
set +e
ZERC="${1:-./zerc}"
shift 2>/dev/null
FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then FILES=(docs/reference.md); fi

DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

total=0; compiled=0; fragments=0; skipped=0; failed=0
failures=""

for f in "${FILES[@]}"; do
  [ -f "$f" ] || { echo "audit_reference_examples: no such file: $f"; exit 1; }
  # Split the file into ```zer blocks with their starting line numbers.
  awk -v OUT="$DIR" -v SRC="$f" '
    /^```zer[ \t]*$/ { inblk=1; n++; start=NR; file=sprintf("%s/blk_%04d.zer", OUT, n);
                       printf "" > file; printf "%d\n", start > (file ".line"); next }
    /^```[ \t]*$/    { if (inblk) { inblk=0; close(file) } next }
    inblk            { print >> file }
  ' "$f"

  for blk in "$DIR"/blk_*.zer; do
    [ -e "$blk" ] || continue
    total=$((total+1))
    line=$(cat "$blk.line" 2>/dev/null || echo 0)
    tag="$f:$line"

    skip_reason=$(grep -oE '// audit: skip .*$' "$blk" | head -1 | sed 's|// audit: skip ||')
    if [ -n "$skip_reason" ]; then
      skipped=$((skipped+1))
      echo "  skip     $tag  ($skip_reason)"
      rm -f "$blk" "$blk.line"
      continue
    fi

    want_err=$(grep -oE '// audit: expect-error .*$' "$blk" | head -1 | sed 's|// audit: expect-error ||')

    # A whole program defines main. Anything else is a fragment.
    if ! grep -qE '(^|[^A-Za-z_])main[ \t]*\(' "$blk"; then
      fragments=$((fragments+1))
      rm -f "$blk" "$blk.line"
      continue
    fi

    compiled=$((compiled+1))
    out=$("$ZERC" "$blk" -o "$blk.c" 2>&1)
    # `zerc -o x.c` prints a success line even when it errors, so read the
    # DIAGNOSTIC, never the exit code (CLAUDE.md, "read the diagnostic").
    diag=$(echo "$out" | grep -vE '^zerc: ' | grep -E '(^|[: ])(error|zercheck):')

    if [ -n "$want_err" ]; then
      if [ -z "$diag" ]; then
        failed=$((failed+1))
        failures="$failures\n  $tag: expected error containing '$want_err', but it COMPILED"
      elif ! echo "$diag" | grep -qF "$want_err"; then
        failed=$((failed+1))
        failures="$failures\n  $tag: expected '$want_err', got: $(echo "$diag" | head -1)"
      fi
    else
      if [ -n "$diag" ]; then
        failed=$((failed+1))
        failures="$failures\n  $tag: example does not compile: $(echo "$diag" | head -1)"
      fi
    fi
    rm -f "$blk" "$blk.line" "$blk.c"
  done
done

echo ""
echo "==================================================================="
echo "reference examples: $total blocks — $compiled compiled, $fragments fragments (no main), $skipped skipped"
if [ $failed -gt 0 ]; then
  echo "FAILURES ($failed):"
  printf "$failures\n"
  echo "REFERENCE EXAMPLES: $failed FAILURE(S)"
  exit 1
fi
echo "OK — every whole-program example in the docs compiles."
exit 0
