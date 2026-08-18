#!/bin/bash
# ============================================================================
# docs/reference.md example gate.
#
# reference.md is the user-facing language reference, and ZER users cannot look
# anything up on the web — it has to be right. "Right" drifts in TWO directions,
# and this gate checks both:
#
#   POSITIVE blocks — a complete program (has a `main`) that the docs present as
#     working. It MUST compile. A doc example that stopped compiling is a promise
#     the compiler no longer keeps. Found live 2026-08-18: the flagship "Safe C
#     Library Interop" example failed with a FALSE leak (BUG-805).
#
#   NEGATIVE blocks — a complete program whose comments say COMPILE ERROR /
#     PARSE ERROR. It MUST be rejected. This is the vacuous-test rule (CLAUDE.md)
#     applied to documentation: a doc that claims an error which no longer fires
#     teaches a rule the compiler does not enforce, which is worse than silence.
#
# SKIPPED, deliberately, with the reason printed so the skip set stays honest:
#   - FRAGMENTS (no `main`) — a declaration, a type, an expression. Compiling
#     these would need synthesised scaffolding, and a wrong guess would report a
#     failure the doc did not cause.
#   - MULTI-FILE examples (a `// <name>.zer:` banner) — they need an import tree.
#   - Blocks needing a cinclude header that does not ship with the docs.
#
# Usage: bash tools/audit_reference_examples.sh [zerc_path] [reference_md]
# Exit: 0 iff every checked block matches its expectation.
# ============================================================================
set +e
ZERC="${1:-./zerc}"
REF="${2:-docs/reference.md}"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

if [ ! -x "$ZERC" ]; then echo "reference-example audit: no zerc at $ZERC" >&2; exit 1; fi
if [ ! -f "$REF" ]; then echo "reference-example audit: no $REF" >&2; exit 1; fi

python3 - "$ZERC" "$REF" "$DIR" <<'PYEOF'
import re, subprocess, sys, os

zerc, ref_path, workdir = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(ref_path, encoding='utf-8').read().split('\n')

# Collect fenced ```zer blocks with their starting line number.
blocks, cur, start = [], None, 0
for i, l in enumerate(lines):
    if cur is None:
        if re.match(r'^```zer\s*$', l):
            cur, start = [], i + 2   # 1-based line of the first code line
        continue
    if l.strip() == '```':
        blocks.append((start, '\n'.join(cur)))
        cur = None
    else:
        cur.append(l)

ERR_MARK = re.compile(r'COMPILE ERROR|PARSE ERROR|ERROR —|ERROR -', re.I)
MULTIFILE = re.compile(r'^\s*//\s*\w+\.zer\s*:', re.M)
HAS_MAIN = re.compile(r'\b(?:u32|i32|void)\s+main\s*\(')

checked = skipped = 0
failures = []
skips = {}

def compile_text(tag, ln, text):
    """Compile `text` and return its DIAGNOSTIC lines (empty == accepted)."""
    path = os.path.join(workdir, '%s_%d.zer' % (tag, ln))
    open(path, 'w', encoding='utf-8').write(text)
    r = subprocess.run([zerc, path, '-o', os.path.join(workdir, '%s_%d.c' % (tag, ln))],
                       capture_output=True, text=True)
    out = (r.stdout or '') + (r.stderr or '')
    # Read the DIAGNOSTIC, never the exit code (CLAUDE.md): emit-C mode prints a
    # success line, and a non-empty stream is not by itself an error.
    return [x for x in out.split('\n') if re.search(r'(^|[: ])(error|zercheck):', x)]


def block_is_negative(body):
    """A block is a NEGATIVE only when an error marker annotates a LIVE line.

    The docs very often show the failing form COMMENTED OUT next to the working
    one ("// tasks.get(t).id = 1;   // COMPILE ERROR: use-after-free"), and those
    blocks are POSITIVES — the whole point is that what remains compiles.
    Treating them as negatives was this gate's own first false positive, caught
    on its first run."""
    for src_line in body.split('\n'):
        if not ERR_MARK.search(src_line):
            continue
        if src_line.split('//')[0].strip():      # live code carrying the marker
            return True
    return False


for ln, body in blocks:
    if MULTIFILE.search(body):
        skipped += 1; skips['multi-file example'] = skips.get('multi-file example', 0) + 1; continue

    if not HAS_MAIN.search(body):
        # FRAGMENT. It cannot be compiled as-is, and 115 of the 154 in this file
        # genuinely need context the prose supplies (a type named in the
        # surrounding paragraph, a variable from an earlier block), so a plain
        # "must compile" rule here would report failures the doc did not cause.
        #
        # But ONE direction is still checkable with no false-alarm risk: a
        # fragment the docs annotate as an ERROR on a LIVE line must be rejected
        # under EVERY wrapping we try. If it compiles under any of them, the doc
        # is teaching a rule the compiler does not enforce — the same rot the
        # complete-program negatives catch, and the direction that decays
        # silently as the compiler relaxes.
        if block_is_negative(body):
            as_top = compile_text('fragtop', ln, body + '\nu32 main() { return 0; }\n')
            as_body = compile_text('fragbody', ln, 'u32 main() {\n' + body + '\nreturn 0;\n}\n')
            if not as_top or not as_body:
                checked += 1
                failures.append((ln, 'fragment documented as an ERROR but COMPILED '
                                     '(wrapped) — the doc teaches a rule the compiler '
                                     'no longer enforces'))
            else:
                checked += 1
            continue
        skipped += 1
        skips['fragment (no main)'] = skips.get('fragment (no main)', 0) + 1
        continue

    # A block is a NEGATIVE only when an error marker annotates a LIVE line.
    # The docs very often show the failing form COMMENTED OUT next to the
    # working one ("// tasks.get(t).id = 1;   // COMPILE ERROR: use-after-free"),
    # and those blocks are POSITIVES — the whole point is that what remains
    # compiles. Treating them as negatives was this gate's own first false
    # positive, caught on its first run.
    expect_error = block_is_negative(body)
    diag = compile_text('ref', ln, body + '\n')
    # A missing cinclude header is the DOC's environment, not the compiler's verdict.
    if any('No such file or directory' in x and '.h' in x for x in diag):
        skipped += 1; skips['needs a cinclude header'] = skips.get('needs a cinclude header', 0) + 1
        continue
    rejected = bool(diag)
    checked += 1

    if expect_error and not rejected:
        failures.append((ln, 'documented as an ERROR but COMPILED — the doc teaches '
                             'a rule the compiler no longer enforces'))
    elif not expect_error and rejected:
        failures.append((ln, 'documented as working but was REJECTED: ' +
                             (diag[0][:160] if diag else '?')))

print("=== docs/reference.md example audit ===")
print("  zer blocks: %d   checked: %d   skipped: %d" % (len(blocks), checked, skipped))
for reason, n in sorted(skips.items()):
    print("    skipped %3d — %s" % (n, reason))
if failures:
    print("\n  %d BLOCK(S) DISAGREE WITH THE COMPILER:" % len(failures))
    for ln, msg in failures:
        print("    %s:%d: %s" % (ref_path, ln, msg))
    print("\n  Fix the example, or fix the compiler — a reference users cannot")
    print("  verify against the web has to match the thing it documents.")
    sys.exit(1)
print("OK — every complete reference.md example agrees with the compiler.")
PYEOF
