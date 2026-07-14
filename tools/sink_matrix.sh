#!/bin/bash
# ============================================================================
# Per-sink escape / UAF matrix — verification harness for memory-safety fixes.
#
# ZER's escape/free analysis is a PER-SINK PATCHWORK (CLAUDE.md): the "is this
# value frame-bound?" question is re-implemented at every escape sink, and the
# same value SHAPE historically leaked through some sinks while being caught at
# others. This harness makes that matrix explicit: each cell is one
# (value-shape × sink) combination with an EXPECTED outcome.
#
#   - A frame-bound value reaching a real escape/free sink  -> MUST reject
#   - A safe use (param view, alive heap, scalar copy)      -> MUST compile
#
# A MISMATCH where expect=reject but actual=compile is a HOLE (a shipped UAF /
# dangling-pointer escape). A MISMATCH where expect=compile but actual=reject is
# an OVER-REJECT. Run after EVERY escape/free-analysis change; a fix must close
# its own cell(s) AND not regress any other cell.
#
# Usage: bash tools/sink_matrix.sh [zerc_path]   (default ./zerc)
# Exit: 0 iff every cell matches its expectation.
# ============================================================================
set +e
ZERC="${1:-./zerc}"
DIR="$(mktemp -d)"
trap 'rm -rf "$DIR"' EXIT

PRE='?*u32 g_p;
?[*]u32 g_s;
struct GH { ?*u32 p; }
GH g_h;
struct L { u32 f; u32 g; }
struct H { ?*u32 mp; }
void keepfn(keep *u32 p) { g_p = p; }
'

pass=0; fail=0; holes=""; overrej=""

# cell NAME EXPECT(reject|compile) CODE...
cell() {
  local name="$1" expect="$2" code="$3"
  printf '%s\n%s\n' "$PRE" "$code" > "$DIR/$name.zer"
  "$ZERC" "$DIR/$name.zer" -o "$DIR/$name.exe" >/dev/null 2>&1
  local ec=$?
  local actual; if [ $ec -ne 0 ]; then actual=reject; else actual=compile; fi
  local st
  if [ "$actual" = "$expect" ]; then st="ok"; pass=$((pass+1))
  else
    fail=$((fail+1))
    if [ "$expect" = reject ]; then st="HOLE"; holes="$holes $name"
    else st="OVER-REJECT"; overrej="$overrej $name"; fi
  fi
  printf '  %-34s want=%-7s got=%-7s %s\n' "$name" "$expect" "$actual" "$st"
}

echo "===== SHAPE p2 = &local[i]  (address of a LOCAL array element) ====="
cell p2__k1_return      reject 'u32[4] a; *u32 c() { u32[4] arr; return &arr[0]; } u32 main(){return 0;}'
cell p2__k2_store_glob  reject 'void c() { u32[4] arr; *u32 p = &arr[0]; g_p = p; } u32 main(){return 0;}'
cell p2__k7_reassign    reject 'void c() { u32[4] arr; *u32 p = &arr[0]; p = &arr[1]; g_p = p; } u32 main(){return 0;}'
cell p2__k2v_2step      reject 'void c() { u32[4] arr; *u32 p = &arr[0]; ?*u32 t = p; g_p = t; } u32 main(){return 0;}'
cell p2__k3_field_store reject 'void c() { u32[4] arr; *u32 p = &arr[0]; g_h.p = p; } u32 main(){return 0;}'
cell p2__k5_keep        reject 'void c() { u32[4] arr; *u32 p = &arr[0]; keepfn(p); } u32 main(){return 0;}'

echo "===== SHAPE p3 = &local.field  (address of a LOCAL struct field) ====="
cell p3__k1_return      reject '*u32 c() { L loc; return &loc.f; } u32 main(){return 0;}'
cell p3__k2_store_glob  reject 'void c() { L loc; *u32 p = &loc.f; g_p = p; } u32 main(){return 0;}'
cell p3__k7_reassign    reject 'void c() { L loc; *u32 p = &loc.f; p = &loc.g; g_p = p; } u32 main(){return 0;}'
cell p3__k2v_2step      reject 'void c() { L loc; *u32 p = &loc.f; ?*u32 t = p; g_p = t; } u32 main(){return 0;}'
cell p3__k3_field_store reject 'void c() { L loc; *u32 p = &loc.f; g_h.p = p; } u32 main(){return 0;}'
cell p3__k5_keep        reject 'void c() { L loc; *u32 p = &loc.f; keepfn(p); } u32 main(){return 0;}'

echo "===== SHAPE p5 = slice-of-local  ([*]T view over a LOCAL array) ====="
cell p5__k1_return      reject '[*]u32 c() { u32[4] arr; return arr[0..2]; } u32 main(){return 0;}'
cell p5__k2_store_glob  reject 'void c() { u32[4] arr; [*]u32 s = arr[0..2]; g_s = s; } u32 main(){return 0;}'
cell p5__k2v_2step      reject 'void c() { u32[4] arr; [*]u32 s = arr[0..2]; [*]u32 t = s; g_s = t; } u32 main(){return 0;}'
cell p5__k6_free        reject 'void c() { u32[4] arr; [*]u32 s = arr[0..2]; free(s); } u32 main(){return 0;}'

echo "===== SHAPE p7 = optional-ptr FIELD carrying &local (?*T of a local struct) ====="
cell p7__k2_store_glob  reject 'void c() { L loc; H h; h.mp = &loc.f; g_p = h.mp; } u32 main(){return 0;}'
cell p7__k3_field_store reject 'void c() { L loc; H h; h.mp = &loc.f; g_h.p = h.mp; } u32 main(){return 0;}'
cell p7__k2v_2step      reject 'void c() { L loc; H h; h.mp = &loc.f; ?*u32 t = h.mp; g_p = t; } u32 main(){return 0;}'

echo "===== SAFE baselines (MUST compile — over-reject guards) ====="
cell safe_param_view    compile '*u32 c([*]u32 p) { return &p[0]; } u32 main(){return 0;}'
cell safe_param_subslice compile '[*]u32 c([*]u32 p) { return p[0..2]; } u32 main(){return 0;}'
cell safe_scalar_copy   compile 'void c() { L loc; loc.f = 5; u32 v = loc.f; g_p = null; if (v == 5) { return; } } u32 main(){return 0;}'
cell safe_alive_subslice compile 'u32 main() { [*]u8 b = alloc(u8,8) orelse return; [*]u8 s = b[0..4]; s[0]=1; u8 v=s[0]; free(b); if (v != 1) { return 1; } return 0; }'

echo ""
echo "===== HEAP-VIEW UAF / double-free (subslice shape) ====="
cell heap_subslice_uaf  reject 'u32 main() { [*]u8 b = alloc(u8,8) orelse return; [*]u8 s = b[0..4]; free(b); s[0]=1; return 0; }'
cell heap_subslice_df   reject 'u32 main() { [*]u8 b = alloc(u8,8) orelse return; [*]u8 s = b[0..4]; free(b); free(s); return 0; }'

echo ""
echo "==================================================================="
echo "matrix: $pass ok, $fail mismatch"
[ -n "$holes" ]   && echo "HOLES (compile but should reject):$holes"
[ -n "$overrej" ] && echo "OVER-REJECTS (reject but should compile):$overrej"
[ $fail -eq 0 ] && echo "SINK MATRIX CLEAN" || echo "SINK MATRIX HAS $fail MISMATCH(es)"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
