#!/bin/bash
# audit_esc_restore.sh — every heap buffer the checker REALLOCs must be kept by
# esc_restore (checker.c, BUG-1402).
#
# The escape fixpoint re-walks a function body after restoring the Checker to a
# snapshot taken at the function's entry. A field of `Checker` that points at a
# realloc'd buffer may have been freed and moved by the walk being undone, so the
# restore must keep the CURRENT pointer + capacity (the count still rolls back).
# A new realloc'd table that is not listed there leaves the restored Checker
# holding a freed pointer — a heap use-after-free inside zerc that only fires in
# the rare functions that need the re-walk. This gate makes adding such a table
# without listing it a build failure.
set -u
cd "$(dirname "$0")/.."
# A field is realloc'd when `realloc(c->F` appears, when `c->F = X` follows a
# realloc within a few lines, or when `&c->F` is handed to a realloc'ing helper
# (the rmw tables, whose helper reallocs through `*tab`).
fields=$(python3 - <<'PY'
import re
L = open('checker.c').read().split('\n')
out = set()
for i, l in enumerate(L):
    if 'realloc(' not in l: continue
    for m in re.finditer(r'realloc\(c->([a-z_]+)', l): out.add(m.group(1))
    for j in range(i, min(i + 6, len(L))):
        for m in re.finditer(r'\bc->([a-z_]+)\s*=\s*[a-z_]+\s*;', L[j]): out.add(m.group(1))
for l in L:
    for m in re.finditer(r'rmw_tab_set\(&c->([a-z_]+)', l): out.add(m.group(1))
out = {f for f in out if not re.search(r'(_cap|capacity|_count|_n)$', f)}
print('\n'.join(sorted(out)))
PY
)
body=$(awk '/^static void esc_restore\(Checker \*c, const Checker \*snap\) \{/,/^}/' checker.c)
[ -n "$body" ] || { echo "FAIL — esc_restore not found in checker.c"; exit 1; }
missing=0
for f in $fields; do
    if ! echo "$body" | grep -qE "(c->$f = cur\.$f|ESC_KEEP_BUF\($f,)"; then
        echo "  checker.c: realloc'd Checker field '$f' is not kept by esc_restore"
        missing=$((missing+1))
    fi
done
if [ $missing -gt 0 ]; then
    echo "FAIL — $missing realloc'd Checker buffer(s) missing from esc_restore (see BUG-1402)"
    exit 1
fi
echo "OK — every realloc'd Checker buffer is kept by esc_restore ($(echo $fields | wc -w) buffers)."
