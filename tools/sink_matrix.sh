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

echo "===== SHAPE p8 = Ring element-store (by-value elem carrying a ptr into a local) ====="
cell p8__k8_ring_push   reject 'struct RM { *u32 q; } Ring(RM, 4) g_rx; void c() { u32 loc; RM m; m.q = &loc; g_rx.push(m); } u32 main(){return 0;}'

echo "===== SHAPE p9 = spawn of a by-value struct carrying a ptr into a local ====="
cell p9__k9_spawn_val   reject 'struct SM { *u32 q; } void wk(SM m) { } void c() { u32 loc; SM m; m.q = &loc; spawn wk(m); } u32 main(){return 0;}'

echo "===== SHAPE p10 = Arena-over-local pointer laundered to a global ====="
cell p10__k10_arena_call   reject 'struct AB { u32 v; } ?*AB g_ab; *AB idb(*AB b) { return b; } void c() { u8[512] bk; Arena ar = Arena.over(bk); *AB b = ar.alloc(AB) orelse return; g_ab = idb(b); } u32 main(){return 0;}'
cell p10__k10_arena_direct reject 'struct AI { u32 v; } [*]AI g_ai; void c() { u8[512] bk; Arena ar = Arena.over(bk); g_ai = ar.alloc_slice(AI, 4) orelse return; } u32 main(){return 0;}'

echo "===== SHAPE p11 = intrinsic-wrapped &local into a struct field (launder) ====="
# @ptrcast/@pun/@bitcast of &local stored into a struct field is NODE_INTRINSIC,
# not NODE_UNARY — the field-store taint only matched a bare &local, so the
# container escaped un-tainted via a later g=b / return b (ASan-confirmed
# stack-use-after-return). Fixed by unwrap_ptr_launder at the assign sink.
cell p11__ptrcast_store_glob reject 'struct Bx { *u32 p; } Bx g_bx; void c() { L loc; Bx b; b.p = @ptrcast(*u32, &loc.f); g_bx = b; } u32 main(){return 0;}'
cell p11__pun_store_glob     reject 'struct By { *u32 p; } By g_by; void c() { L loc; By b; b.p = @pun(*u32, &loc.f); g_by = b; } u32 main(){return 0;}'
cell p11__return_struct      reject 'struct Bz { *u32 p; } Bz c() { L loc; Bz b; b.p = @ptrcast(*u32, &loc.f); return b; } u32 main(){return 0;}'
cell p11__field_store        reject 'void c() { L loc; H h; h.mp = @ptrcast(*u32, &loc.f); g_h.p = h.mp; } u32 main(){return 0;}'

echo "===== SHAPE p12 = struct-copy of a local-derived array/struct ELEMENT ====="
# `g = arr[0]` where arr is a local-derived array of pointer-carrying structs
# copies the whole element (with its dangling pointer) into a global. The
# read-side descend-to-root gate was ref-type-only, skipping the struct-by-value
# element copy (ASan-confirmed stack-use-after-return). Widened to
# type_can_carry_pointer.
cell p12__elem_copy_glob   reject 'struct Bx { *u32 p; } Bx g_bx; void c() { u32 loc; Bx[2] arr; arr[0].p = &loc; g_bx = arr[0]; } u32 main(){return 0;}'
cell p12__elem_copy_launder reject 'struct Bw { *u32 p; } Bw g_bw; void c() { u32 loc; Bw[2] arr; arr[0].p = @ptrcast(*u32, &loc); g_bw = arr[0]; } u32 main(){return 0;}'

echo "===== SHAPE p13 = return a u8[N] array PARAM as a slice (BUG-764-class relaxation) ====="
# A u8[N] array param is by-reference (decays to a pointer into the caller's
# array), so returning it as a slice is a caller-memory view — SOUND at the
# function level. classify_return_root records the param index, so the CALL SITE
# rejects a caller that passes a LOCAL and lets the result escape to a global.
cell p13__return_param_arr_escape reject '?[*]u8 g13; ?[*]u8 rf(u8[4] a){ return a; } void c(){ u8[4] x; g13 = rf(x); } u32 main(){return 0;}'

echo "===== SAFE baselines (MUST compile — over-reject guards) ====="
# p13 accept side: returning a param array as a slice, result used locally (does
# not escape the caller frame) — must compile (the relaxation).
cell safe_return_param_arr_opt   compile '?[*]u8 rf(u8[4] a){ return a; } u32 main(){ u8[4] x; ?[*]u8 r = rf(x); return 0; }'
cell safe_return_param_arr_slice compile '[*]u8 rf(u8[4] a){ return a; } u32 main(){ u8[4] x; [*]u8 r = rf(x); return (u32)r.len; }'
cell safe_ptrcast_global compile 'struct Bg { *u32 p; } L g_loc; Bg g_bg; void c() { Bg b; b.p = @ptrcast(*u32, &g_loc.f); g_bg = b; } u32 main(){return 0;}'
cell safe_elem_copy_global compile 'struct Be { *u32 p; } u32 g_v; Be g_be; void c() { Be[2] arr; arr[0].p = &g_v; g_be = arr[0]; } u32 main(){return 0;}'
cell safe_elem_scalar     compile 'struct Bs { u32 n; *u32 p; } u32 g_i; u32 g_v; void c() { Bs[2] arr; arr[0].p = &g_v; g_i = arr[0].n; } u32 main(){return 0;}'
cell safe_param_view    compile '*u32 c([*]u32 p) { return &p[0]; } u32 main(){return 0;}'
cell safe_param_subslice compile '[*]u32 c([*]u32 p) { return p[0..2]; } u32 main(){return 0;}'
cell safe_ring_value    compile 'struct VM { u32 a; } Ring(VM, 4) g_vx; void c() { VM m; m.a = 1; g_vx.push(m); } u32 main(){return 0;}'
cell safe_arena_local   compile 'struct AB { u32 v; } u32 rv(*AB b){return b.v;} void c() { u8[512] bk; Arena ar = Arena.over(bk); *AB b = ar.alloc(AB) orelse return; b.v = rv(b); } u32 main(){return 0;}'
cell safe_spawn_value   compile 'struct SV { u32 a; } void wk(SV m) { } void c() { SV m; m.a = 1; spawn wk(m); } u32 main(){return 0;}'
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
