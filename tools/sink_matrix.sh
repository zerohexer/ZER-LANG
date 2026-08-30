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

# ---------------------------------------------------------------------------
# SHAPE p16 (BUG-803): an ARENA-derived pointer is the SECOND lifetime this
# matrix must track, and until 2026-08-18 only the first one (a stack local) was
# asked at the SHARED helpers. The direct ident sinks had both; the struct
# literal, the launder-through-a-local and the orelse JOIN had only "local". So
# the byte-identical program escaped through whichever syntax reached a shared
# helper — ASan-confirmed stack-use-after-return on `g = { .p = arena_ptr }`
# while `g.p = arena_ptr` was rejected one line away.
#
# The axis is worth its own row because "frame-bound" has TWO causes with
# DIFFERENT deaths: a local dies at return, arena memory dies at return AND at
# `arena.reset()`. Any future sink must answer for both, which is why the leaf
# test is now one predicate (symbol_is_frame_bound_ptr) rather than a flag read.
echo "===== SHAPE p16 = an Arena-derived pointer at each shared-helper sink ====="
cell p16_arena_direct_field  reject 'struct N16{u32 v;} struct B16{ *N16 p; } B16 gb16; void c(){ u8[256] bk; Arena ar=Arena.over(bk); *N16 a=ar.alloc(N16) orelse return; gb16.p=a; } u32 main(){c();return 0;}'
cell p16_arena_struct_lit    reject 'struct N16{u32 v;} struct B16{ *N16 p; } B16 gb16; void c(){ u8[256] bk; Arena ar=Arena.over(bk); *N16 a=ar.alloc(N16) orelse return; gb16={ .p=a }; } u32 main(){c();return 0;}'
cell p16_arena_lit_launder   reject 'struct N16{u32 v;} struct B16{ *N16 p; } B16 gb16; void c(){ u8[256] bk; Arena ar=Arena.over(bk); *N16 a=ar.alloc(N16) orelse return; B16 t={ .p=a }; gb16=t; } u32 main(){c();return 0;}'
cell p16_arena_orelse_fb     reject 'struct N16{u32 v;} ?*N16 g16; ?*N16 none16(){return null;} void c(){ u8[256] bk; Arena ar=Arena.over(bk); *N16 a=ar.alloc(N16) orelse return; g16=none16() orelse a; } u32 main(){c();return 0;}'
cell p16_arena_lit_return    reject 'struct N16{u32 v;} struct B16{ *N16 p; } B16 mk16(){ u8[256] bk; Arena ar=Arena.over(bk); ?*N16 m=ar.alloc(N16); *N16 a=m orelse return; return { .p=a }; } u32 main(){ B16 x=mk16(); return 0;}'
# BOUNDARY: an arena pointer used LOCALLY, and a struct literal carrying a
# pointer to a GLOBAL, must both still compile — the widened predicate must not
# turn "carries a pointer" into "escapes".
cell p16_safe_arena_local    compile 'struct N16{u32 v;} u32 main(){ u8[256] bk; Arena ar=Arena.over(bk); ?*N16 m16=ar.alloc(N16); if (m16) |a| { a.v=7; if (a.v != 7) { return 1; } } return 0; }'
cell p16_safe_lit_global_ptr compile 'struct N16{u32 v;} struct B16{ *N16 p; } N16 gn16; B16 gb16b; void c(){ gb16b={ .p=&gn16 }; } u32 main(){c();return 0;}'

# ---------------------------------------------------------------------------
# SHAPE p17 (BUG-807): the launder peel applied to a CALL ARGUMENT rather than
# to the stored value. p15 above covers `g = <launder>(&local)` — the launder
# wrapping the value at the sink. This row covers `g = f(<launder>(&local))` —
# the launder wrapping an ARGUMENT, one level further in, where
# arg_is_local_derived is the leaf that decides. That predicate was the one
# frame-bound question that never called the shared peeler, so all three wrapper
# forms walked past call_result_escapes and reached a global. ASan-confirmed
# stack-use-after-return on each. The C-style cast is the worst of the three:
# the emitter DELETES it, so the emitted C is byte-identical to the bare form
# one line away that IS rejected.
echo "===== SHAPE p17 = a launder wrapping a CALL ARGUMENT (not the stored value) ====="
cell p17_arg_bare          reject '*u32 id17(*u32 p){return p;} void c(){ u32 x=5; g_p=id17(&x); } u32 main(){c();return 0;}'
cell p17_arg_ptrcast       reject '*u32 id17(*u32 p){return p;} void c(){ u32 x=5; g_p=id17(@ptrcast(*u32,&x)); } u32 main(){c();return 0;}'
cell p17_arg_ccast         reject '*u32 id17(*u32 p){return p;} void c(){ u32 x=5; g_p=id17((*u32)(&x)); } u32 main(){c();return 0;}'
cell p17_arg_pun           reject '*u32 id17(*u32 p){return p;} void c(){ u32 x=5; g_p=id17(@pun(*u32,&x)); } u32 main(){c();return 0;}'
cell p17_arg_field_launder reject 'struct L17{u32 f;} *u32 id17(*u32 p){return p;} void c(){ L17 l; g_p=id17(@ptrcast(*u32,&l.f)); } u32 main(){c();return 0;}'
# BOUNDARY: a launder over a GLOBAL address is not frame-bound, and a laundered
# arg to a callee that returns a STATIC is settled by the return summary — the
# peel must not turn every wrapped argument into an escape.
cell p17_safe_arg_global   compile 'u32 gx17=1; *u32 id17b(*u32 p){return p;} void c(){ g_p=id17b(@ptrcast(*u32,&gx17)); } u32 main(){c();return 0;}'
cell p17_safe_arg_static   compile 'u32 gs17=2; *u32 pick17(*u32 p){return &gs17;} void c(){ u32 x=5; g_p=pick17(@ptrcast(*u32,&x)); } u32 main(){c();return 0;}'


# ---------------------------------------------------------------------------
# SHAPE p18 (BUG-845/846/848/849): "which allocation does a call RESULT view?"
# p17 asks whether an ARGUMENT is frame-bound. This row asks the dual question
# at the same sink — the callee returns a VIEW of one of its arguments, so the
# RESULT must inherit that argument's allocation. Four independent ways the
# answer was lost, each an ASan-confirmed heap-use-after-free:
#   arg FORM      — the two consumers resolved the aliased argument with a bare
#                   `kind == NODE_IDENT` test, so a field / subslice / launder
#                   argument registered a FRESH allocation instead (BUG-845).
#   def LOCALITY  — the inference searched only the RETURN's own block and did
#                   not follow the COPY a named binding lowers to (BUG-846).
#   BLOCK TAG     — is_early_exit blocks were skipped by the return summaries,
#                   which answers a leak-coverage question, not a return-value
#                   one (BUG-848).
#   ARITY         — the answer is a SET; one slot collapsed a disjunctive view
#                   to "unknown", and an unknown SLICE result is not tracked at
#                   all (BUG-849).
# A SLICE result makes these silent: it is not "pointer-ish", so the fallback
# registers nothing and there is no leak diagnostic to notice.
echo "===== SHAPE p18 = a call RESULT that VIEWS an argument ====="
cell p18_res_arg_field     reject '[*]u8 hd18([*]u8 s){return s[0..2];} struct B18{[*]u8 s;} u32 main(){ B18 b; b.s=alloc(u8,4) orelse {return 1;}; [*]u8 h=hd18(b.s); free(b.s); return h[0]; }'
cell p18_res_arg_subslice  reject '[*]u8 hd18([*]u8 s){return s[0..2];} u32 main(){ [*]u8 s=alloc(u8,4) orelse {return 1;}; [*]u8 h=hd18(s[0..4]); free(s); return h[0]; }'
cell p18_res_assign_field  reject '[*]u8 hd18([*]u8 s){return s[0..2];} struct B18{[*]u8 s;} struct H18{[*]u8 p;} u32 main(){ B18 b; b.s=alloc(u8,4) orelse {return 1;}; H18 h; h.p=hd18(b.s); free(b.s); return h.p[0]; }'
cell p18_res_crossblock    reject '[*]u8 hd18([*]u8 s){ [*]u8 v=s[0..2]; if (v.len>1) { return v; } return v; } u32 main(){ [*]u8 s=alloc(u8,4) orelse {return 1;}; [*]u8 h=hd18(s); free(s); return h[0]; }'
cell p18_res_early_exit    reject '[*]u8 pk18([*]u8 a,[*]u8 b,bool f){ if (f) { return b[0..1]; } return a[0..1]; } u32 main(){ [*]u8 s=alloc(u8,4) orelse {return 1;}; [*]u8 t=alloc(u8,4) orelse {return 2;}; [*]u8 h=pk18(s,t,true); free(t); u32 r=h[0]; free(s); return r; }'
cell p18_res_multi_param   reject '[*]u8 pk18([*]u8 a,[*]u8 b,bool f){ if (f) { return a[0..1]; } return b[0..1]; } u32 main(){ [*]u8 s=alloc(u8,4) orelse {return 1;}; [*]u8 t=alloc(u8,4) orelse {return 2;}; [*]u8 h=pk18(s,t,true); free(s); u32 r=h[0]; free(t); return r; }'
cell p18_res_global_arm    reject 'u8[8] gb18; [*]u8 pk18([*]u8 a,bool f){ if (f) { return a[0..1]; } return gb18[0..1]; } u32 main(){ [*]u8 s=alloc(u8,4) orelse {return 1;}; [*]u8 h=pk18(s,true); free(s); return h[0]; }'
# BOUNDARY: a view is NOT an allocation. Reading it before the backing store is
# freed is correct code, and the result must never be leak-checked as if the
# caller owned it (the BUG-847 relaxation).
cell p18_safe_use_then_free compile '[*]u8 hd18b([*]u8 s){return s[0..2];} u32 main(){ [*]u8 s=alloc(u8,4) orelse {return 1;}; s[0]=5; [*]u8 h=hd18b(s); u32 r=h[0]; free(s); if (r!=5) { return 2; } return 0; }'
cell p18_safe_stack_view    compile 'struct N18{u32 v;u32 w;} *u32 fo18(*N18 n){return &n.v;} u32 main(){ N18 nd; nd.v=7; *u32 p=fo18(&nd); if (*p != 7) { return 1; } return 0; }'
cell p18_safe_null_arm      compile '?*u32 mb18(*u32 p,bool ok){ if (ok) { return p; } return null; } u32 main(){ u32 loc=3; ?*u32 m=mb18(&loc,true); if (m) |pp| { if (*pp != 3) { return 1; } } return 0; }'


# ---------------------------------------------------------------------------
# SHAPE p19 (BUG-914): the C-STYLE cast launder at the HEAP-OWNERSHIP sinks.
#
# p15 covers the same wrapper at the FRAME-BOUND sinks (checker.c, BUG-791).
# This row is the sibling one layer down: zercheck_ir.c answers "which ALLOCATION
# does this value alias?", and its two peelers saw @ptrcast / @pun / @bitcast /
# @cast / @container but NOT `(*T)x` — casts postdate the analysis, and the
# BUG-791 fix taught only the checker. Measured on main before the fix:
#
#     g = n;                 -> rejected      (dangling global)
#     g = @ptrcast(*N, n);   -> rejected
#     g = (*N)n;             -> COMPILED, RAN, returned freed memory
#
# with byte-identical emitted C for all three (the emitter elides an identity
# pointer cast). Both peelers now consult the SHARED ast.h predicate, so a value
# cast `(u32)x` is still never peeled — that is what the two `safe` cells pin.
#
# VERIFIED NON-VACUOUS against a pre-fix build (git archive HEAD): the four
# GLOBAL-store cells were live HOLES, the two intra-function cells (alias_uaf,
# double_free) were already caught by the direct-alias path and are kept as
# class coverage rather than as evidence.
echo "===== SHAPE p19 = a C-style cast laundering a HEAP pointer past ownership ====="
cell p19_cast_global_store   reject 'struct N19{u32 v;} ?*N19 g19; u32 main(){ *N19 n=alloc(N19) orelse {return 1;}; n.v=7; g19=(*N19)n; free(n); *N19 r=g19 orelse {return 2;}; return r.v; }'
cell p19_cast_global_field   reject 'struct N19{u32 v;} struct H19{?*N19 p;} H19 gh19; u32 main(){ *N19 n=alloc(N19) orelse {return 1;}; n.v=7; gh19.p=(*N19)n; free(n); *N19 r=gh19.p orelse {return 2;}; return r.v; }'
cell p19_cast_double_hop     reject 'struct N19{u32 v;} ?*N19 g19b; u32 main(){ *N19 n=alloc(N19) orelse {return 1;}; n.v=7; g19b=(*N19)((*N19)n); free(n); *N19 r=g19b orelse {return 2;}; return r.v; }'
cell p19_cast_opaque_global  reject 'struct N19{u32 v;} ?*opaque g19c; u32 main(){ *N19 n=alloc(N19) orelse {return 1;}; n.v=7; g19c=(*opaque)n; free(n); return 0; }'
cell p19_cast_alias_uaf      reject 'struct N19{u32 v;} u32 main(){ *N19 n=alloc(N19) orelse {return 1;}; n.v=7; *N19 k=(*N19)n; free(n); return k.v; }'
# The checker-side sibling: a struct LITERAL field. struct_init_has_local_derived
# hand-rolled its own peel (last-arg, no cast arm) instead of calling the shared
# one, so `{ .p = (*u32)(&loc) }` escaped while the bare and @ptrcast spellings
# were both rejected. It now uses unwrap_ptr_launder for cases A-D alike.
cell p19_cast_struct_lit     reject 'void c19(){ u32 loc=5; g_h = { .p = (*u32)(&loc) }; } u32 main(){c19();return 0;}'
cell p19_cast_struct_lit_al  reject 'void c19(){ u32 loc=5; *u32 a=&loc; g_h = { .p = (*u32)a }; } u32 main(){c19();return 0;}'
# BUG-919: the INFERENCE half of the keep rule. call_has_nonkeep_derived_arg
# (detection) peels with the shared helper; infer_keep_from_call_args (inference)
# hand-rolled a naive last-arg loop, so a cast-laundered launder inferred NOTHING
# and the CALL SITE was never restricted. ASan-confirmed stack-use-after-return.
cell p19_cast_keep_infer     reject '*u32 idf19(*u32 p){return p;} void st19(*u32 p){ g_p = idf19((*u32)p); } void c19(){ u32 loc=5; st19(&loc); } u32 main(){c19();return 0;}'
cell p19_cast_keep_2hop      reject '*u32 idf19(*u32 p){return p;} void st19(*u32 p){ g_p = idf19((*u32)((*u32)p)); } void c19(){ u32 loc=5; st19(&loc); } u32 main(){c19();return 0;}'
cell p19_cast_double_free    reject 'struct N19{u32 v;} u32 main(){ *N19 n=alloc(N19) orelse {return 1;}; n.v=7; *N19 k=(*N19)n; free(n); free(k); return 0; }'
# BOUNDARY: a VALUE cast is not a reference and must never be peeled, and a
# correctly-reset global must still compile — otherwise the fix is an over-reject.
cell p19_safe_keep_global    compile 'u32 gv19=1; *u32 idf19b(*u32 p){return p;} void st19b(*u32 p){ g_p = idf19b((*u32)p); } u32 main(){ st19b(&gv19); return 0; }'
cell p19_safe_keep_scalar    compile 'u32 gi19; u32 cnt19(*u32 p){return *p;} void f19(*u32 p){ gi19 = cnt19((*u32)p); } u32 main(){ u32 l=5; f19(&l); if (gi19!=5) { return 1; } return 0; }'
cell p19_safe_struct_lit_g   compile 'u32 gd19=1; void c19(){ g_h = { .p = (*u32)(&gd19) }; } u32 main(){c19();return 0;}'
cell p19_safe_value_cast     compile 'struct N19{u32 v;} u32 g19v; u32 main(){ *N19 n=alloc(N19) orelse {return 1;}; n.v=7; g19v=(u32)n.v; free(n); if (g19v!=7) { return 2; } return 0; }'
cell p19_safe_reset_global   compile 'struct N19{u32 v;} ?*N19 g19d; u32 main(){ *N19 n=alloc(N19) orelse {return 1;}; n.v=7; g19d=(*N19)n; u32 r=n.v; g19d=null; free(n); if (r!=7) { return 2; } return 0; }'

echo ""
echo "==================================================================="
echo "matrix: $pass ok, $fail mismatch"
[ -n "$holes" ]   && echo "HOLES (compile but should reject):$holes"
[ -n "$overrej" ] && echo "OVER-REJECTS (reject but should compile):$overrej"
[ $fail -eq 0 ] && echo "SINK MATRIX CLEAN" || echo "SINK MATRIX HAS $fail MISMATCH(es)"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
