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
  # A `reject` cell asserts the CHECKER rejects. Compiling to an .exe let GCC's
  # opinion count, so a cell could score "ok" because GCC choked on the emitted C
  # while the checker said nothing — a weak oracle inside the gate itself. Measured
  # 2026-08-17: 3 of the 7 new p15 cells passed pre-fix for exactly that reason.
  # `compile` cells still go through GCC: they must produce a REAL working binary.
  local actual
  if [ "$expect" = reject ]; then
    if "$ZERC" "$DIR/$name.zer" -o "$DIR/$name.c" 2>&1 | grep -vE '^zerc: ' \
       | grep -qE '(^|[: ])(error|zercheck):'; then actual=reject; else actual=compile; fi
  else
    "$ZERC" "$DIR/$name.zer" -o "$DIR/$name.exe" >/dev/null 2>&1
    if [ $? -ne 0 ]; then actual=reject; else actual=compile; fi
  fi
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

# §C (2026-08-01) — escape-sink widenings. Each cell is a value shape that
# slipped a sink whose gate tested a raw type-kind or required a bare NODE_IDENT.
echo "===== C: escape-sink shape widenings ====="
cell c1_projected_arr_optslice reject 'struct SB { u8[8] a; } ?[*]u8 gsl; void c() { SB s; gsl = s.a; } u32 main(){return 0;}'
cell c2_optcarrier_local_slice reject '?[*]u8 gsl2; void c() { u8[16] buf; ?[*]u8 s = buf[0..8]; gsl2 = s; } u32 main(){return 0;}'
cell c6_keep_local_field       reject 'struct LF { u32 f; } void c() { LF loc; keepfn(&loc.f); } u32 main(){return 0;}'
cell c6_keep_local_arrelem     reject 'void c() { u32[4] a; keepfn(&a[0]); } u32 main(){return 0;}'
cell safe_c6_keep_global_field compile 'struct LF { u32 f; } LF g_lf; void c() { keepfn(&g_lf.f); } u32 main(){return 0;}'
cell safe_c1_param_slice_glob compile '?[*]u8 gsl3; void c([*]u8 p) { gsl3 = p; } u32 main(){return 0;}'

# G5 — heap pointer stored into a GLOBAL's field/index projection then freed.
# The bare `g = n` store was already covered (BUG-739); these pin the projection
# sink (all three store sites) and the launder-aware RHS. The reset cell pins the
# BUG-742 conservatism: only a DEFINITELY-freed global is flagged.
echo "===== G5 heap ptr -> GLOBAL field/index dangle ====="
cell heap_glob_field_dangle   reject 'struct N { u32 x; } struct HB { ?*N p; } HB gb; u32 main(){ ?*N m = alloc(N); *N n = m orelse return; gb.p = n; free(n); return 0; }'
cell heap_glob_arrelem_dangle reject 'struct N { u32 x; } struct HB { ?*N p; } HB gb2[2]; u32 main(){ ?*N m = alloc(N); *N n = m orelse return; gb2[0].p = n; free(n); return 0; }'
cell heap_glob_launder_field  reject 'struct N { u32 x; } struct HB { ?*N p; } HB gb; u32 main(){ ?*N m = alloc(N); *N n = m orelse return; gb.p = @ptrcast(*N, n); free(n); return 0; }'
cell safe_glob_field_reset   compile 'struct N { u32 x; } struct HB { ?*N p; } HB gb; u32 main(){ ?*N m = alloc(N); *N n = m orelse return; gb.p = n; free(n); gb.p = null; return 0; }'

# p15 — LAUNDER SHAPES (2026-08-17, BUG-791). "Is this value frame-bound?" is answered
# AFTER peeling launders. A peel present at one sink and absent from the shared peeler
# is the same hole wearing a different syntax. `(*T)x` was missing from
# unwrap_ptr_launder for the entire life of C-style casts — and the emitter ELIDES that
# cast, so the emitted C is byte-identical to the form that was correctly rejected.
# `orelse` is the JOIN case: it collapses to TWO nodes, not one, so peeling to the
# primary silently drops the fallback; it needs a predicate, not a peel.
# The safe cells pin the boundary so the peel cannot be widened into an over-rejection.
echo "===== SHAPE p15 = launder wrappers around a local-derived pointer ====="
cell p15_cast_store_glob    reject 'void c(){ L loc; *u32 p=&loc.f; g_p=(*u32)p; } u32 main(){c();return 0;}'
cell p15_cast_direct_glob   reject 'void c(){ u32 x=5; g_p=(*u32)(&x); } u32 main(){c();return 0;}'
cell p15_cast_opaque_glob   reject '?*opaque g_op15=null; void c(){ u32 x=5; g_op15=(*opaque)(&x); } u32 main(){c();return 0;}'
cell p15_cast_vardecl_glob  reject 'void c(){ u32 x=5; *u32 p=(*u32)(&x); g_p=p; } u32 main(){c();return 0;}'
cell p15_orelse_primary     reject 'u32 gd15=0; void c(){ L loc; ?*u32 t=&loc.f; g_p=t orelse &gd15; } u32 main(){c();return 0;}'
cell p15_orelse_fallback    reject '?*u32 mk15(){return null;} void c(){ L loc; loc.f=1; g_p=mk15() orelse &loc.f; } u32 main(){c();return 0;}'
cell p15_cast_free_arg      reject 'struct N15{u32 x;} u32 main(){ ?*N15 m=alloc(N15); *N15 n=m orelse {return 1;}; n.x=1; free((*N15)n); u32 v=n.x; free(n); return v; }'
# BOUNDARY: a VALUE cast manufactures a fresh value and must NOT be peeled; an orelse
# whose arms are both non-local must still compile; a cast of a GLOBAL address too.
cell p15_safe_value_cast    compile 'u32 gv15=0; void c(){ u32 x=5; gv15=(u32)x; } u32 main(){c();return 0;}'
cell p15_safe_orelse_global compile 'u32 gd15b=1; ?*u32 mk15b(){return null;} void c(){ g_p=mk15b() orelse &gd15b; } u32 main(){c();return 0;}'
cell p15_safe_cast_global   compile 'u32 gd15c=1; void c(){ g_p=(*u32)(&gd15c); } u32 main(){c();return 0;}'

echo ""
echo "==================================================================="
echo "matrix: $pass ok, $fail mismatch"
[ -n "$holes" ]   && echo "HOLES (compile but should reject):$holes"
[ -n "$overrej" ] && echo "OVER-REJECTS (reject but should compile):$overrej"
[ $fail -eq 0 ] && echo "SINK MATRIX CLEAN" || echo "SINK MATRIX HAS $fail MISMATCH(es)"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
