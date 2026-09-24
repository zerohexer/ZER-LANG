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

# p15b — THE SAME QUESTION AT THE HEAP / ALLOCATION-IDENTITY SINK (2026-09-04, BUG-931).
# p15 above proved the peel at the STACK-ESCAPE sinks. It did not cover the sink that
# asks "which allocation is this?", and zercheck_ir.c answered that with THREE partial
# peelers (typecast-only, intrinsics-only, intrinsics+.ptr) while checker.c had FOUR MORE
# hand-rolled ones. Each knew a different subset, so every carrier was caught at some
# sinks and invisible at others. Crossing CARRIER x SINK is what makes that visible:
# a cell missing here is a hole that no single-sink test can see.
echo "===== SHAPE p15b = launder carriers at the HEAP / arena / keep sinks ====="
cell p15b_heap_cast_glob     reject 'struct N9{u32 v;} ?*N9 gh9; u32 main(){ *N9 n=alloc(N9) orelse {return 1;}; gh9=(*N9)n; free(n); *N9 r=gh9 orelse {return 2;}; return r.v; }'
cell p15b_heap_cast_dfree    reject 'struct N9{u32 v;} u32 main(){ *N9 n=alloc(N9) orelse {return 1;}; free((*N9)n); free(n); return 0; }'
cell p15b_heap_distinct_uaf  reject 'struct N9{u32 v;} distinct typedef *N9 Pn9; u32 r9; void f(){ *N9 n=alloc(N9) orelse return; Pn9 c=@cast(Pn9,n); free(n); r9=c.v; } u32 main(){f();return 0;}'
cell p15b_heap_distinct_dfree reject 'struct N9{u32 v;} distinct typedef *N9 Pn9; void f(){ *N9 n=alloc(N9) orelse return; Pn9 c=@cast(Pn9,n); free(c); free(n); } u32 main(){f();return 0;}'
cell p15b_keep_inner_cast    reject '*u32 idf9(*u32 p){return p;} void st9(*u32 p){ g_p=idf9((*u32)p); } u32 main(){ u32 l=5; st9(&l); return 0; }'
cell p15b_keep_outer_cast    reject '*u32 idf9(*u32 p){return p;} void st9(*u32 p){ g_p=(*u32)idf9(p); } u32 main(){ u32 l=5; st9(&l); return 0; }'
cell p15b_structlit_cast     reject 'struct H9{?*u32 p;} H9 gh9b; void c(){ u32 l=5; gh9b={.p=(*u32)(&l)}; } u32 main(){c();return 0;}'
cell p15b_arena_ccast        reject 'struct N9{u32 v;} ?*N9 ga9; void c(){ u8[256] bk; Arena a9=Arena.over(bk); *N9 x=a9.alloc(N9) orelse return; ga9=(*N9)x; } u32 main(){c();return 0;}'
cell p15b_arena_twohop       reject 'struct N9{u32 v;} ?*N9 ga9b; void c(){ u8[256] bk; Arena a9=Arena.over(bk); *N9 x=a9.alloc(N9) orelse return; *N9 y=(*N9)x; ga9b=y; } u32 main(){c();return 0;}'
cell p15b_cstr_local_array   reject '?*u8 gc9; void c(){ u8[8] b; const [*]u8 s="hi"; gc9=@cstr(b,s); } u32 main(){c();return 0;}'
# BUG-1045: the keep-INFERENCE trace (keep_arg_caller_root) peeled every intrinsic to its
# LAST argument — the FIELD NAME for @container — so the transitive keep through a
# container_of launder was lost and a pointer into the caller's frame reached a global.
cell p15b_keep_container_trans reject 'struct L9{u32 x;} struct D9{u32 a; L9 list;} ?*D9 gd9; void in9(*D9 d){ gd9=d; } void out9(*L9 p){ in9(@container(*D9, p, list)); } u32 main(){ D9 d; out9(&d.list); return 0; }'
cell p15b_keep_cstr_trans      reject '?*u8 gc9c; void in9c(*u8 q){ gc9c=q; } void out9c([*]u8 b){ const [*]u8 s="hi"; in9c(@cstr(b,s)); } u32 main(){ u8[8] l; out9c(l); return 0; }'
# BOUNDARY: the peel must not turn a legitimate single free through a laundered name
# into a leak report, and a launder of GLOBAL storage stays legal at every sink.
cell p15b_safe_free_once     compile 'distinct typedef [*]u8 Buf9; u32 r9b; void f(){ [*]u8 b=alloc(u8,4) orelse return; b[0]=7; Buf9 c=@cast(Buf9,b); r9b=(u32)c[0]; free(c); } u32 main(){f(); if(r9b!=7){return 2;} return 0;}'
cell p15b_safe_cstr_global   compile 'u8[8] gb9; ?*u8 gc9b; void c(){ const [*]u8 s="hi"; gc9b=@cstr(gb9,s); } u32 main(){c();return 0;}'

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
# SHAPE p19 (BUG-968): A VIEW INTO AN ALLOCATION — the SPELLING axis.
#
# "What allocation does this view point into?" was answered by a walk that peeled
# FIELD and INDEX to an ident and stopped at anything else. Two things fell out of it,
# and crossing SPELLING x FORM is what makes them visible:
#
#   - `pool.get(h)` and `h` NAME THE SAME SLOT, but the get() spelling landed on a
#     NODE_CALL, so `&pool.get(h).v` aliased nothing while `&h.v` aliased correctly.
#   - a SLICE is a reference-forming node ("FORMING a reference aliases; READING a
#     value does not") but the alias branch admitted only `&`, so a slice view of a
#     field aliased nothing in EITHER spelling — including on a heap pointer, which
#     is how the second half was found.
#
# The consequence is not a would-be UAF. Pre-fix, p19_slot_reuse compiled clean and
# RETURNED 99: the stale view wrote into a freed slot that had been handed back out,
# corrupting a live different object — the Handle generation check bypassed because a
# view carries a raw pointer nothing re-checks.
echo "===== SHAPE p19 = a view into an allocation (spelling x form) ====="
cell p19_get_field_addr   reject 'struct T19{u32 v;} Pool(T19,4) p19; u32 main(){ Handle(T19) h=p19.alloc() orelse {return 1;}; *u32 q=&p19.get(h).v; p19.free(h); *q=7; return 0; }'
cell p19_get_index_addr   reject 'struct T19b{u8[4] a;} Pool(T19b,4) p19b; u32 main(){ Handle(T19b) h=p19b.alloc() orelse {return 1;}; *u8 q=&p19b.get(h).a[0]; p19b.free(h); *q=7; return 0; }'
cell p19_get_field_slice  reject 'struct T19c{u8[4] a;} Pool(T19c,4) p19c; u32 main(){ Handle(T19c) h=p19c.alloc() orelse {return 1;}; [*]u8 s=p19c.get(h).a[0..]; p19c.free(h); s[0]=7; return 0; }'
cell p19_slab_get_addr    reject 'struct T19d{u32 v;} Slab(T19d) s19; u32 main(){ Handle(T19d) h=s19.alloc() orelse {return 1;}; *u32 q=&s19.get(h).v; s19.free(h); *q=7; return 0; }'
cell p19_autoderef_slice  reject 'struct T19e{u8[4] a;} Pool(T19e,4) p19e; u32 main(){ Handle(T19e) h=p19e.alloc() orelse {return 1;}; [*]u8 s=h.a[0..]; p19e.free(h); s[0]=7; return 0; }'
cell p19_heap_slice       reject 'struct T19f{u8[4] a;} u32 main(){ *T19f t=alloc(T19f) orelse {return 1;}; [*]u8 s=t.a[0..]; free(t); s[0]=7; return 0; }'
cell p19_slot_reuse       reject 'struct T19g{u8[4] a;} Pool(T19g,4) p19g; u32 main(){ Handle(T19g) h1=p19g.alloc() orelse {return 1;}; [*]u8 s=h1.a[0..]; p19g.free(h1); Handle(T19g) h2=p19g.alloc() orelse {return 2;}; s[0]=99; u32 r=(u32)h2.a[0]; p19g.free(h2); return r; }'
cell p19_keep_get_stash   reject 'struct T19h{u32 v;} Pool(T19h,4) p19h; ?*u32 gq19=null; void st19(*u32 q){ gq19=q; } u32 main(){ Handle(T19h) h=p19h.alloc() orelse {return 1;}; st19(&p19h.get(h).v); p19h.free(h); return 0; }'
# BOUNDARY: a view into a LIVE slot is ordinary code and must stay legal, in both
# spellings and both forms. Over-rejecting here would break the read-modify-free
# sequence every pool user writes.
cell p19_safe_view_before_free compile 'struct T19i{u32 v;u8[4] a;} Pool(T19i,4) p19i; u32 main(){ Handle(T19i) h=p19i.alloc() orelse {return 1;}; *u32 q=&p19i.get(h).v; *q=5; [*]u8 s=p19i.get(h).a[1..]; s[0]=9; u32 r=p19i.get(h).v+(u32)p19i.get(h).a[1]; p19i.free(h); if (r!=14) { return 2; } return 0; }'
cell p19_safe_stack_slice      compile 'u32 main(){ u8[4] loc; loc[0]=3; [*]u8 s=loc[0..]; return (u32)s[0]-3; }'


# ---------------------------------------------------------------------------
# SHAPE p20 (BUG-969): WHAT DOES A SCOPED-SPAWN ARGUMENT LEND? — the SPELLING axis.
#
# The borrow was established only for a LITERAL `&v` argument. Every other way of
# handing the same address to the thread lent NOTHING, so the parent could write the
# memory the child held: a pointer local bound to `&v`, a struct CARRYING the pointer,
# a slice VIEW of a local array, and a pointer PARAMETER (whose root lives in the
# caller, so the only lendable name is the pointer itself).
#
# `is_local_derived` could not answer this: it is a BOOLEAN saying a pointer points
# into SOME local, which is all the escape sinks need. The race guarded here is a write
# to the ROOT (`v = 3`), so the root must be NAMED — hence Symbol.borrow_root_name,
# recorded at the declaration.
echo "===== SHAPE p20 = what a scoped-spawn argument lends (spelling axis) ====="
cell p20_literal_amp      reject 'void w20(*u32 p){*p=5;} u32 main(){ u32 v=0; ThreadHandle t=spawn w20(&v); v=3; t.join(); return v; }'
cell p20_ptr_local        reject 'void w20(*u32 p){*p=5;} u32 main(){ u32 v=0; *u32 q=&v; ThreadHandle t=spawn w20(q); v=3; t.join(); return v; }'
cell p20_write_through    reject 'void w20(*u32 p){*p=5;} u32 main(){ u32 v=0; *u32 q=&v; ThreadHandle t=spawn w20(q); *q=3; t.join(); return v; }'
cell p20_struct_carrier   reject 'struct H20{*u32 p;} void wh20(H20 h){*h.p=5;} u32 main(){ u32 v=0; H20 h; h.p=&v; ThreadHandle t=spawn wh20(h); v=3; t.join(); return v; }'
cell p20_slice_view       reject 'void ws20([*]u8 s){s[0]=5;} u32 main(){ u8[4] a; [*]u8 s=a[0..2]; ThreadHandle t=spawn ws20(s); a[1]=3; t.join(); return 0; }'
cell p20_param_ptr        reject 'void w20(*u32 p){*p=5;} void f20(*u32 p){ ThreadHandle t=spawn w20(p); *p=3; t.join(); } u32 main(){ u32 v=0; f20(&v); return 0; }'
cell p20_threadlocal_alias reject 'threadlocal u32 tl20; void w20(*u32 p){*p=1;} u32 main(){ *u32 q=&tl20; ThreadHandle t=spawn w20(q); t.join(); return 0; }'
cell p20_interior_ptr     reject 'struct B20{u32 v;} void w20(*u32 p){*p=5;} u32 main(){ B20 b; ThreadHandle t=spawn w20(&b.v); b.v=3; t.join(); return b.v; }'
# BUG-1169: three more spellings — a PRE-EXISTING alias written through, a struct
# LITERAL carrier, and a pointer TO a carrier.
cell p20_prior_alias      reject 'void w20(*u32 p){*p=5;} u32 main(){ u32 v=1; *u32 al=&v; ThreadHandle t=spawn w20(&v); *al=7; t.join(); return v; }'
cell p20_literal_carrier  reject 'struct H20{*u32 p;} void wh20(H20 h){*h.p=5;} u32 main(){ u32 v=1; H20 h={.p=&v}; ThreadHandle t=spawn wh20(h); v=7; t.join(); return v; }'
cell p20_amp_carrier      reject 'struct H20{*u32 p;} void wp20(*H20 h){*h.p=5;} u32 main(){ u32 v=1; H20 h; h.p=&v; ThreadHandle t=spawn wp20(&h); v=7; t.join(); return v; }'
# BOUNDARY: lend only what actually reaches the parent's memory, and release at join.
# A SCALAR is copied; a pointer to a GLOBAL lends no local; an unrelated local stays
# writable; and every borrow ends at the join. Over-rejecting any of these would break
# ordinary concurrent code.
cell p20_safe_scalar_copy compile 'shared struct S20{u32 v;} S20 s20; void wv20(u32 v){s20.v=v;} u32 main(){ u32 n=3; ThreadHandle t=spawn wv20(n); n=4; t.join(); if(n!=4){return 1;} return 0; }'
cell p20_safe_global_root compile 'u32 g20; void w20(*u32 p){*p=5;} u32 main(){ *u32 gq=&g20; u32 other=1; ThreadHandle t=spawn w20(gq); other+=1; t.join(); if(g20!=5||other!=2){return 1;} return 0; }'
cell p20_safe_after_join  compile 'void w20(*u32 p){*p=5;} u32 main(){ u32 v=0; *u32 q=&v; ThreadHandle t=spawn w20(q); t.join(); v=v+1; if(v!=6){return 1;} return 0; }'


# ---------------------------------------------------------------------------
# SHAPE p21 (BUG-970): COPYING A UNIQUE RESOURCE — the value-flow SINK axis.
#
# Arena / Barrier / Semaphore alias their state when copied, and were refused NOWHERE.
# Pool / Ring / Slab were refused at exactly ONE site (assignment, BUG-225), which is
# why every other value-flow spelling stayed open for all six. Crossing TYPE x SINK is
# what makes that visible.
#
# Not a would-be bug: p21_arena_copy_value returned 2 pre-fix, meaning two separate
# `alloc(T)` calls on the two copies handed out the SAME BYTES.
#
# The BOUNDARY is the hard half. A FRESH value is not a copy (`Arena.over(buf)`), and
# returning a LOCAL by value is a MOVE — the local dies with the frame, so the caller
# becomes the only owner. Only a copy of something that OUTLIVES the copy is refused,
# which is why the return sink asks about the source's lifetime and the others do not.
echo "===== SHAPE p21 = copying a unique resource (type x value-flow sink) ====="
cell p21_arena_copy_value  reject 'u32 main(){ u8[64] b21; Arena a21=Arena.over(b21); Arena c21=a21; return 0; }'
cell p21_arena_param       reject 'void tk21(Arena a){ } u32 main(){ u8[64] b21; Arena a21=Arena.over(b21); tk21(a21); return 0; }'
cell p21_arena_orelse      reject 'u8[64] gb21; Arena ga21; u32 main(){ ga21=Arena.over(gb21); ?Arena n21=null; Arena c21=n21 orelse ga21; return 0; }'
cell p21_arena_ret_global  reject 'u8[64] gb21; Arena ga21; Arena get21(){ return ga21; } u32 main(){ ga21=Arena.over(gb21); Arena c21=get21(); return 0; }'
# The copy is INITIALISED here on purpose: without that, the pre-fix compiler rejected
# this cell via the "barrier never initialised" rule, so it would have passed on a
# broken compiler and tested nothing. Measured — that is exactly what it did.
cell p21_barrier_copy      reject 'Barrier gb21b; u32 main(){ @barrier_init(gb21b,1); Barrier c21=gb21b; @barrier_init(c21,1); @barrier_wait(c21); @barrier_wait(gb21b); return 0; }'
cell p21_semaphore_copy    reject 'Semaphore(1) gs21; u32 main(){ Semaphore(1) c21=gs21; @sem_acquire(c21); @sem_release(gs21); return 0; }'
cell p21_struct_carrier    reject 'struct C21{Arena a;u32 n;} u32 main(){ u8[64] b21; C21 x; x.a=Arena.over(b21); C21 y=x; return y.n; }'
cell p21_struct_init_field reject 'struct C21b{Arena a;u32 n;} u8[64] gb21c; Arena ga21c; u32 main(){ ga21c=Arena.over(gb21c); C21b x={.a=ga21c,.n=1}; return x.n-1; }'
cell p21_pool_param        reject 'struct T21{u32 v;} Pool(T21,4) gp21; void tk21(Pool(T21,4) q){ } u32 main(){ tk21(gp21); return 0; }'
cell p21_slab_param        reject 'struct T21b{u32 v;} Slab(T21b) gsl21; void tk21(Slab(T21b) q){ } u32 main(){ tk21(gsl21); return 0; }'
# BOUNDARY: a FRESH resource is not a copy; a LOCAL returned by value is a MOVE; and
# Barrier/Semaphore pointer params are the remedy the diagnostic actually names.
cell p21_safe_fresh        compile 'struct T21c{u32 v;} u8[64] gb21d; u8[64] gb21e; u32 main(){ Arena a21=Arena.over(gb21d); *T21c t=a21.alloc(T21c) orelse {return 1;}; t.v=4; u32 k21=t.v; a21=Arena.over(gb21e); *T21c u=a21.alloc(T21c) orelse {return 2;}; u.v=5; if(k21+u.v!=9){return 3;} return 0; }'
cell p21_safe_local_move   compile 'u8[64] gb21f; Arena mk21(){ Arena a=Arena.over(gb21f); return a; } u32 main(){ Arena a21=mk21(); return 0; }'
# BUG-1177: the ASSIGNMENT sink (BUG-970 wired seven sinks, not this one) and the
# async task, which is self-referential (a promoted `*T p = &x;` points into the task).
cell p21_arena_assign       reject 'u8[64] gb21h; u8[64] gb21i; u32 main(){ Arena a21=Arena.over(gb21h); Arena c21=Arena.over(gb21i); c21=a21; return 0; }'
cell p21_barrier_assign     reject 'Barrier gb21j; Barrier gb21k; u32 main(){ @barrier_init(gb21j,1); gb21k=gb21j; return 0; }'
cell p21_semaphore_assign   reject 'Semaphore(1) gs21c; Semaphore(1) gs21d; u32 main(){ gs21d=gs21c; return 0; }'
cell p21_async_copy_decl    reject 'async u32 ac21(){ u32 x=5; *u32 p=&x; yield; *p+=1; return x; } u32 main(){ _zer_async_ac21 t; _zer_async_ac21_init(&t); _zer_async_ac21_poll(&t); _zer_async_ac21 u=t; _zer_async_ac21_poll(&u); return 0; }'
cell p21_async_copy_assign  reject 'async u32 ac21b(){ u32 x=5; *u32 p=&x; yield; *p+=1; return x; } _zer_async_ac21b g21; void st21(){ _zer_async_ac21b t; _zer_async_ac21b_init(&t); _zer_async_ac21b_poll(&t); g21=t; } u32 main(){ st21(); _zer_async_ac21b_poll(&g21); return 0; }'
cell p21_async_copy_param   reject 'async u32 ac21c(){ yield; return 1; } void tk21c(_zer_async_ac21c v){ } u32 main(){ _zer_async_ac21c t; _zer_async_ac21c_init(&t); tk21c(t); return 0; }'
cell p21_safe_async_ptr     compile 'async u32 ac21d(){ u32 x=5; *u32 p=&x; yield; *p+=1; return x; } void drive21(*_zer_async_ac21d t){ while(_zer_async_ac21d_poll(t)==0){} } u32 main(){ _zer_async_ac21d t; _zer_async_ac21d_init(&t); drive21(&t); if(_zer_async_ac21d_result(&t)!=6){return 1;} return 0; }'
# BUG-1180: a Ring moves elements by value — the push is a copy no sink sees.
cell p21_ring_push_barrier  reject 'Barrier gb21m; Ring(Barrier,2) rq21; u32 main(){ @barrier_init(gb21m,1); rq21.push(gb21m); return 0; }'
cell p21_ring_push_async    reject 'async u32 ac21e(){ u32 x=5; *u32 p=&x; yield; *p+=1; return x; } Ring(_zer_async_ac21e,2) rq21b; u32 main(){ _zer_async_ac21e t; _zer_async_ac21e_init(&t); _zer_async_ac21e_poll(&t); rq21b.push(t); return 0; }'
# BUG-1178: re-initialising an arena by assignment is a RESET for what it handed out.
cell p21_arena_reinit_uaf   reject 'struct T21e{u32 v;} u8[64] gb21l; u32 main(){ Arena a21=Arena.over(gb21l); *T21e x=a21.alloc(T21e) orelse {return 1;}; x.v=7; a21=Arena.over(gb21l); *T21e y=a21.alloc(T21e) orelse {return 2;}; y.v=9; return x.v; }'
cell p21_safe_ptr_param    compile 'Barrier gb21g; Semaphore(2) gs21b; void tb21(*Barrier b){@barrier_wait(b);} void ts21(*Semaphore s){@sem_acquire(s);@sem_release(s);} u32 main(){ @barrier_init(gb21g,1); tb21(&gb21g); ts21(&gs21b); return 0; }'


# ---------------------------------------------------------------------------
# SHAPE p22 (BUG-972): A MISALIGNED VIEW INTO A PACKED STRUCT — the OPERATION axis.
#
# The packed-field alignment rule guarded `&p.field` and nothing else. Four further
# ways to reach the identical misaligned access compiled clean, and they split into
# TWO OPERATIONS that no single predicate could cover:
#
#   address-of : `&p.w[0]` — one INDEX step between the field and the `&`, which the
#                gate (which demanded a DIRECT field access) walked straight past.
#   slice view : `p.w[0..]`, `[*]u32 s = p.w`, `fill(p.w)` — no `&` appears at all,
#                so the address-of predicate could never reach them.
#
# Measured on `packed struct P { u8 a; u32[2] w; }`: offsetof(w) == 1, so the emitted
# `uint32_t *` addresses an ODD byte. A hard fault on ARMv7-M / RISC-V, a split access
# on Cortex-M0+, and merely SLOW on x86 — which is why no hosted test could catch it.
#
# The BOUNDARY is the precision half and it is what the packed feature exists for: a
# BYTE array field is safe to view, because u8 elements cannot be misaligned. The
# slice rule keys on type_alignment_bytes, not on the packed attribute alone.
echo "===== SHAPE p22 = a misaligned view into a packed struct (operation axis) ====="
cell p22_elem_addr      reject 'packed struct P22{u8 a; u32[2] w;} u32 main(){ P22 p; *u32 q=&p.w[0]; *q=1; return p.w[0]-1; }'
cell p22_slice_expr     reject 'packed struct P22b{u8 a; u32[2] w;} u32 main(){ P22b p; [*]u32 s=p.w[0..]; s[0]=1; return p.w[0]-1; }'
cell p22_coerce_decl    reject 'packed struct P22c{u8 a; u32[2] w;} u32 main(){ P22c p; [*]u32 s=p.w; s[1]=1; return p.w[1]-1; }'
cell p22_call_arg       reject 'packed struct P22d{u8 a; u32[2] w;} void fl22([*]u32 s){s[0]=1;} u32 main(){ P22d p; fl22(p.w); return p.w[0]-1; }'
cell p22_scalar_field   reject 'packed struct P22e{u8 a; u32 w;} u32 main(){ P22e p; *u32 q=&p.w; *q=1; return p.w-1; }'
cell p22_nested_packed  reject 'packed struct I22{u8 a; u32[2] w;} struct O22{I22 i;} u32 main(){ O22 o; [*]u32 s=o.i.w[0..]; s[0]=1; return o.i.w[0]-1; }'
# BOUNDARY: a BYTE array field is safe to view — u8 elements cannot be misaligned, and
# rejecting that would break the packed-wire-format idiom the feature exists for. An
# UNPACKED struct is naturally aligned and must stay viewable in every spelling.
cell p22_safe_u8_view   compile 'packed struct F22{u8 k; u8[6] pay; u16 crc;} u32 sm22([*]u8 s){u32 t=0; for (u8 b in s) { t+=b; } return t;} u32 main(){ F22 f; f.pay[0]=1; f.pay[1]=2; [*]u8 v=f.pay; [*]u8 t=f.pay[0..2]; if (sm22(v)!=3) { return 1; } if (sm22(t)!=3) { return 2; } if (sm22(f.pay)!=3) { return 3; } return 0; }'
cell p22_safe_unpacked  compile 'struct U22{u8 a; u32[2] w;} u32 main(){ U22 u; u.w[0]=5; [*]u32 s=u.w; if (s[0]!=5) { return 1; } return 0; }'
# BUG-1171: the CARRIER sinks — a struct literal field and a field assignment.
cell p22_literal_field    reject 'packed struct P22c{u8 a; u32 w;} struct H22c{*u32 p;} P22c gp22c; u32 main(){ H22c h = { .p = &gp22c.w }; *h.p = 7; return 0; }'
cell p22_field_assign     reject 'packed struct P22d{u8 a; u32 w;} struct H22d{*u32 p;} P22d gp22d; u32 g22d; u32 main(){ H22d h = { .p = &g22d }; h.p = &gp22d.w; *h.p = 7; return 0; }'



# ---------------------------------------------------------------------------
# SHAPE p23 (BUG-973): TWO SHARED STRUCTS IN ONE SPAWN ARGUMENT — the SPELLING axis.
#
# The same-statement multi-shared-type rule (which exists because the emitter takes
# ONE lock per statement) had NODE_SPAWN classified as "no cond/init/expr that could
# read a shared struct". The exhaustive switch forced that classification to be
# explicit — the gate worked as designed — and the classification was simply WRONG: a
# spawn's arguments are parent-evaluated expressions of that statement. Measured:
#
#     spawn w(a.x + b.y);
#     -> pthread_mutex_lock(&a._zer_mtx);
#        _sa->a0 = (a.x + b.y);        //  b.y read with only A's mutex held
#        pthread_mutex_unlock(&a._zer_mtx);
#
# TWO SPELLINGS, TWO WALKERS. The bare statement goes through the STATEMENT arm; the
# scoped `ThreadHandle th = spawn …` arrives as a var-decl INITIALIZER and goes
# through the EXPRESSION arm. Fixing one leaves the other open, which is why the
# spelling is the axis.
#
# The BOUNDARY is per-ARGUMENT, and getting it wrong rejects correct code: the emitter
# locks one shared root per ARGUMENT, so `spawn w(a.x, b.y)` is two separate,
# correctly-held locks. A first draft accumulated across arguments and rejected it.
echo "===== SHAPE p23 = two shared structs in ONE spawn argument (spelling axis) ====="
cell p23_bare_stmt     reject 'shared struct A23{u32 x;} shared struct B23{u32 y;} A23 a23; B23 b23; void w23(u32 v){a23.x=v;} u32 main(){ spawn w23(a23.x + b23.y); return 0; }'
cell p23_scoped_decl   reject 'shared struct A23b{u32 x;} shared struct B23b{u32 y;} A23b a23b; B23b b23b; void w23b(u32 v){a23b.x=v;} u32 main(){ ThreadHandle t=spawn w23b(a23b.x + b23b.y); t.join(); return 0; }'
cell p23_second_arg    reject 'shared struct A23c{u32 x;} shared struct B23c{u32 y;} A23c a23c; B23c b23c; void w23c(u32 p,u32 q){a23c.x=p+q;} u32 main(){ spawn w23c(1, a23c.x + b23c.y); return 0; }'
# BOUNDARY: one shared root PER ARGUMENT is locked separately and is correct code.
# Rejecting this would be worse than the hole — it is the ordinary way to hand two
# shared values to a thread.
cell p23_safe_per_arg  compile 'shared struct A23d{u32 x;} shared struct B23d{u32 y;} A23d a23d; B23d b23d; void w23d(u32 v,u32 z){a23d.x=v+z;} u32 main(){ a23d.x=1; b23d.y=2; ThreadHandle t=spawn w23d(a23d.x, b23d.y); t.join(); if (a23d.x!=3) { return 1; } return 0; }'
cell p23_safe_one_type compile 'shared struct A23e{u32 x;u32 z;} A23e a23e; void w23e(u32 v){a23e.x=v;} u32 main(){ a23e.x=1; a23e.z=2; ThreadHandle t=spawn w23e(a23e.x + a23e.z); t.join(); return 0; }'


# ---------------------------------------------------------------------------
# SHAPE p24 (BUG-974): THE ZERO OF A BARE `orelse return` — the RETURN-TYPE axis.
#
# `orelse return` is bare by design: "no value; the return value comes from the
# function's return type". The emitter's fallback for a valueless return was
# `return 0;` for EVERY non-optional type, which is three different answers collapsed
# into one, and two of them are wrong:
#
#   integer / bool / float : 0 is the zero.                        correct
#   slice / struct / union : `return 0;` is not a value of that type — GCC refused it
#                            ("incompatible types when returning type 'int'"), so
#                            VALID ZER failed to compile. Now `(T){0}`.
#   *T / funcptr           : the type is non-null BY DEFINITION, so it has NO zero.
#                            `return 0;` handed the caller a NULL of a type that
#                            promises non-null. Rejected in the checker.
#
# A PLAIN bare `return;` in such a function was already rejected ("function must
# return '*T', not void"). The orelse spelling reaches no NODE_RETURN handler — the
# same reason the defer / @critical bans had to be repeated for it — so the axis here
# is the RETURN TYPE, crossed against both spellings.
echo "===== SHAPE p24 = the zero of a bare orelse return (return-type axis) ====="
cell p24_nonnull_ptr    reject 'struct T24{u32 v;} *T24 pk24(?*T24 o){ *T24 t=o orelse return; return t; } u32 main(){ return 0; }'
cell p24_funcptr        reject 'u32 db24(u32 x){return x*2;} *(u32) -> u32 pk24b(?u32 o){ u32 v=o orelse return; return db24; } u32 main(){ return 0; }'
cell p24_plain_bare_ptr reject 'struct T24c{u32 v;} *T24c pk24c(bool c){ if (c) { return; } T24c t; return &t; } u32 main(){ return 0; }'
# BOUNDARY: the types whose zero EXISTS must keep working, and the value must be the
# real zero — an EMPTY slice and a ZEROED struct, not whatever `return 0` compiled to.
# The slice/struct cells did not compile at all before this fix.
cell p24_safe_slice     compile '[*]u8 pk24d(?[*]u8 o){ [*]u8 s=o orelse return; return s; } u32 main(){ ?[*]u8 n=null; [*]u8 r=pk24d(n); if (r.len!=0) { return 1; } return 0; }'
cell p24_safe_struct    compile 'struct P24{u32 x;} P24 pk24e(?u32 o){ u32 v=o orelse return; P24 p; p.x=v; return p; } u32 main(){ ?u32 n=null; P24 r=pk24e(n); if (r.x!=0) { return 1; } return 0; }'
cell p24_safe_int       compile 'u32 pk24f(?u32 o){ u32 v=o orelse return; return v; } u32 main(){ ?u32 n=null; if (pk24f(n)!=0) { return 1; } ?u32 s=7; if (pk24f(s)!=7) { return 2; } return 0; }'
# The ?*T arm is the one the checker rule must NOT fire on: the null sentinel IS its
# zero, which is exactly the None the propagation intends. Written WITHOUT calling the
# function, because binding a pointer-returning call's result makes zercheck's ownership
# model report a leak — a pre-existing rejection on both builds, unrelated to this rule,
# which is what a first draft of this cell measured.
cell p24_safe_opt_ptr   compile 'struct T24g{u32 v;} ?*T24g pk24g(?*T24g o){ *T24g t=o orelse return; return t; } u32 main(){ return 0; }'



# ---------------------------------------------------------------------------
# SHAPE p25 (BUG-981/982/983): AN ALLOCATION STORED INTO A SLOT — SPELLING x ROOT.
#
# "Which entry tracks this allocation?" has to give the SAME answer for every
# spelling that stores it and every root the slot hangs off. It did not:
#
#   h.p = alloc(T);                 bare `=` into a field  -> tracked NOWHERE (BUG-981)
#   h.p = alloc(T) orelse return;   the orelse spelling    -> tracked (through a temp)
#   g.p = ...  /  free(g.p)         a GLOBAL root          -> registered at the store
#                                    sink, resolvable at NO other sink (BUG-982)
#   b.p = a.p;                      slot-to-slot copy      -> no alias (BUG-982)
#   h.inner = i;  { .inner = i }    a struct VALUE         -> its compounds not
#   { .inner = { .p = alloc(T) } }  carried (BUG-983)
#
# The consequence is a real UAF with an observable wrong value: pre-fix,
# p25_slot_reuse compiled clean and RETURNED 99 — the stale field read the value of
# a DIFFERENT live object that had been handed the recycled slot. ASan cannot see it
# (`alloc(T)` is an auto-Slab; free() recycles rather than returning to libc).
#
# Each spelling that carries an allocation into a slot needs a cell HERE, crossed
# with the root it can hang off (local struct field / local array index / global).
echo "===== SHAPE p25 = an allocation stored into a slot (spelling x root) ====="
cell p25_bare_field_uaf      reject 'struct T25{u32 v;} struct H25{?*T25 p;} u32 main(){ H25 h; h.p=alloc(T25); *T25 q=h.p orelse {return 1;}; free(q); *T25 r=h.p orelse {return 2;}; return r.v; }'
cell p25_bare_field_leak     reject 'struct T25a{u32 v;} struct H25a{?*T25a p;} u32 main(){ H25a h; h.p=alloc(T25a); return 0; }'
cell p25_bare_field_overwrite reject 'struct T25b{u32 v;} struct H25b{?*T25b p;} u32 main(){ H25b h; h.p=alloc(T25b); h.p=alloc(T25b); *T25b q=h.p orelse {return 1;}; free(q); return 0; }'
cell p25_bare_index_uaf      reject 'struct T25c{u32 v;} u32 main(){ ?*T25c[2] arr; arr[0]=alloc(T25c); *T25c q=arr[0] orelse {return 1;}; free(q); *T25c r=arr[0] orelse {return 2;}; return r.v; }'
cell p25_bare_global_dangling reject 'struct T25d{u32 v;} struct H25d{?*T25d p;} H25d g25d; u32 main(){ g25d.p=alloc(T25d); *T25d q=g25d.p orelse {return 1;}; free(q); return 0; }'
cell p25_global_free_reread  reject 'struct T25e{u32 v;} struct H25e{*T25e p;} H25e g25e; u32 main(){ g25e.p=alloc(T25e) orelse {return 1;}; free(g25e.p); return g25e.p.v; }'
cell p25_slot_copy_uaf       reject 'struct T25f{u32 v;} struct H25f{?*T25f p;} u32 main(){ H25f a; H25f b; a.p=alloc(T25f) orelse {return 1;}; b.p=a.p; *T25f q=a.p orelse {return 2;}; free(q); *T25f r=b.p orelse {return 3;}; return r.v; }'
cell p25_slot_copy_global_uaf reject 'struct T25g{u32 v;} struct H25g{?*T25g p;} H25g g25g; u32 main(){ H25g a; a.p=alloc(T25g); g25g.p=a.p; *T25g q=a.p orelse {return 1;}; free(q); *T25g r=g25g.p orelse {return 2;}; return r.v; }'
cell p25_struct_into_slot_uaf reject 'struct T25h{u32 v;} struct I25h{?*T25h p;} struct H25h{I25h inner;} u32 main(){ I25h i; i.p=alloc(T25h); H25h h; h.inner=i; *T25h q=h.inner.p orelse {return 1;}; free(q); *T25h r=i.p orelse {return 2;}; return r.v; }'
cell p25_nested_init_uaf     reject 'struct T25i{u32 v;} struct I25i{?*T25i p;} struct H25i{I25i inner;} u32 main(){ H25i h={ .inner={ .p=alloc(T25i) } }; *T25i q=h.inner.p orelse {return 1;}; free(q); *T25i r=h.inner.p orelse {return 2;}; return r.v; }'
cell p25_nested_init_orelse_uaf reject 'struct T25j{u32 v;} struct I25j{*T25j p;} struct H25j{I25j inner;} u32 main(){ H25j h={ .inner={ .p=alloc(T25j) orelse {return 1;} } }; free(h.inner.p); return h.inner.p.v; }'
cell p25_slot_reuse          reject 'struct T25k{u32 v;} struct H25k{?*T25k p;} u32 main(){ H25k h; h.p=alloc(T25k); *T25k q=h.p orelse {return 1;}; q.v=7; free(q); *T25k o=alloc(T25k) orelse {return 2;}; o.v=99; *T25k r=h.p orelse {return 3;}; u32 seen=r.v; free(o); return seen; }'
# BOUNDARY: every SAFE use of a slot-stored allocation must stay legal — the taught
# `slot = null` reset after a free (local AND global root), re-filling after a free,
# a returned struct carrying the allocation out (not a leak), a struct value placed
# in an initializer and freed through the outer path (not a false leak of the inner).
cell p25_safe_reset_refill   compile 'struct T25l{u32 v;} struct H25l{?*T25l p;} H25l g25l; u32 main(){ H25l h; h.p=alloc(T25l); *T25l q=h.p orelse {return 1;}; q.v=7; u32 v=q.v; free(q); h.p=null; g25l.p=alloc(T25l); *T25l gq=g25l.p orelse {return 2;}; free(gq); g25l.p=alloc(T25l); *T25l gr=g25l.p orelse {return 3;}; free(gr); g25l.p=null; return v-7; }'
cell p25_safe_return_carries compile 'struct T25m{u32 v;} struct H25m{?*T25m p;} H25m mk25(){ H25m h; h.p=alloc(T25m); return h; } u32 main(){ H25m m=mk25(); *T25m q=m.p orelse {return 1;}; free(q); return 0; }'
cell p25_safe_carry_free_outer compile 'struct T25n{u32 v;} struct I25n{?*T25n p;} struct H25n{I25n inner;} u32 main(){ I25n i={ .p=alloc(T25n) }; H25n h={ .inner=i }; *T25n q=h.inner.p orelse {return 1;}; q.v=2; u32 v=q.v; free(q); return v-2; }'

# SHAPE p26 (BUG-1023): AN ALLOCATION THAT ARRIVES AS A CALL RESULT — the CARRIER axis.
#
# `alloc(T, n)` returns `?[*]T`, and "returnable from a factory" is the DOCUMENTED
# idiom for it. But the call-result registration listed POINTER / OPAQUE / HANDLE and
# NOT SLICE, so a heap slice arriving from a call was never registered as a tracked
# allocation and the ENTIRE Model-1 lifecycle silently did not apply to it:
#
#     [*]u32 mk() { [*]u32 s = alloc(u32, 4) orelse return; return s; }
#     u32 run()   { [*]u32 s = mk(); free(s); return s[0]; }   /* compiled CLEAN */
#
# ASan: heap-use-after-free. The double-free and the leak were accepted too — while
# the DIRECT spelling of all three, inside one function, was correctly rejected. One
# carrier handled, its sibling missed, at the one sink where the carrier set was
# written out by hand.
#
# `ir_type_is_ptrish` had the SAME omission, which is why the first fix looked like
# it worked and did not: with SLICE missing there, a function that allocates a slice
# was reported as making no allocation-capable call, so `ret_is_borrow` claimed its
# result was a BORROW of caller memory and suppressed the tracking again.
#
# The BOUNDARY cells are what keep the fix honest: a slice-returning function that
# allocates NOTHING (a string literal, a subslice of a param, a view) must not
# acquire a false "never freed". Measured corpus cost of the shipped form: ZERO over
# 2492 files; an earlier, ungated draft cost three.
echo "===== SHAPE p26 = an allocation arriving as a CALL RESULT (carrier axis) ====="
cell p26_slice_call_uaf    reject '[*]u32 mk26() { [*]u32 s = alloc(u32, 4) orelse return; return s; } u32 run26(){ [*]u32 s = mk26(); free(s); return s[0]; } u32 main(){ return run26(); }'
cell p26_slice_call_double reject '[*]u32 mk26b() { [*]u32 s = alloc(u32, 4) orelse return; return s; } u32 run26b(){ [*]u32 s = mk26b(); free(s); free(s); return 0; } u32 main(){ return run26b(); }'
cell p26_slice_call_leak   reject '[*]u32 mk26c() { [*]u32 s = alloc(u32, 4) orelse return; return s; } u32 run26c(){ [*]u32 s = mk26c(); return s[0]; } u32 main(){ return run26c(); }'
cell p26_optslice_call_uaf reject '?[*]u32 mk26d() { return alloc(u32, 4); } u32 run26d(){ [*]u32 s = mk26d() orelse return; free(s); return s[0]; } u32 main(){ return run26d(); }'
# BOUNDARY: a slice-returning function that allocates nothing is NOT an allocation.
cell p26_safe_literal      compile 'const [*]u8 nm26() { return "ZER"; } u32 main(){ const [*]u8 n = nm26(); if (n.len != 3) { return 1; } return 0; }'
cell p26_safe_param_view   compile '[*]u8 tr26([*]u8 s) { return s[1..3]; } u32 main(){ u8[4] b; b[1] = 9; [*]u8 t = tr26(b[0..4]); if (t[0] != 9) { return 1; } return 0; }'
# p26, the SLOT half (BUG-1024): the same factory result stored into a SLOT rather
# than a plain local. `ir_register_alloc_result_compound` only ever ran for a DIRECT
# builtin allocation (`h.p = alloc(T)`), so a FACTORY result landing in a field or an
# array element was registered nowhere.
#
# The shape is SLICE-ONLY, which is why nobody had written it: `[*]u32 mk() { ...
# orelse return; ... }` is expressible because the zero of a slice is a legal slice
# value, while the pointer spelling of the same function is refused outright by
# BUG-974 (the zero of a non-null `*T` is the NULL its type forbids). And
# `slot = <non-optional call>` lowers to ONE passthrough ASSIGN — no IR_CALL, no
# IR_FIELD_WRITE — so neither the call-result arm nor the field-write arm saw it.
cell p26_slot_field_uaf    reject 'struct H26{[*]u32 s;} [*]u32 mk26f() { [*]u32 s = alloc(u32, 4) orelse return; return s; } u32 run26f(){ H26 h; h.s = mk26f(); free(h.s); return h.s[0]; } u32 main(){ return run26f(); }'
cell p26_slot_index_uaf    reject '[*]u32 mk26g() { [*]u32 s = alloc(u32, 4) orelse return; return s; } u32 run26g(){ [*]u32[2] a; a[0] = mk26g(); free(a[0]); return a[0][0]; } u32 main(){ return run26g(); }'
cell p26_safe_slot_view    compile 'struct H26b{[*]u8 s;} [*]u8 tr26b([*]u8 s) { return s[1..3]; } u32 main(){ u8[4] b; b[1]=9; H26b h; h.s = tr26b(b[0..4]); if (h.s[0]!=9) { return 1; } return 0; }'
cell p26_safe_slot_freed   compile 'struct H26c{[*]u32 s;} [*]u32 mk26h() { [*]u32 s = alloc(u32, 4) orelse return; return s; } u32 run26h(){ H26c h; h.s = mk26h(); h.s[0]=7; u32 v=h.s[0]; free(h.s); return v; } u32 main(){ if (run26h()!=7) { return 1; } return 0; }'
cell p26_safe_factory_ok   compile '[*]u32 mk26e() { [*]u32 s = alloc(u32, 4) orelse return; return s; } u32 run26e(){ [*]u32 s = mk26e(); s[0] = 7; u32 v = s[0]; free(s); return v; } u32 main(){ if (run26e() != 7) { return 1; } return 0; }'

echo ""
# SHAPE p27 (BUG-1049): an allocation held by a BARE GLOBAL — carrier x spelling x
# violation. `ir_global_projection_key` keyed `g.p` / `g[0]` but not bare `g`, so a
# free THROUGH the global resolved to no entry and every one of these HOLE cells
# compiled clean (the slice forms were ASan heap-use-after-free). The SAFE cells
# pin the taught remedy (`g = null;` after the free) and ordinary use.
echo "===== SHAPE p27 = an allocation held by a BARE global ====="
cell p27_slice_direct_uaf   reject '[*]u32 g27a; u32 main(){ g27a = alloc(u32, 4) orelse return; free(g27a); return g27a[0]; }'
cell p27_slice_alias_uaf    reject '[*]u32 g27b; u32 main(){ [*]u32 s = alloc(u32, 4) orelse return; g27b = s; free(g27b); return s[0]; }'
cell p27_slice_factory_uaf  reject '[*]u32 g27c; [*]u32 mk27c() { [*]u32 s = alloc(u32, 4) orelse return; return s; } u32 main(){ g27c = mk27c(); free(g27c); return g27c[0]; }'
cell p27_slice_double_free  reject '[*]u32 g27d; u32 main(){ g27d = alloc(u32, 4) orelse return; free(g27d); free(g27d); return 0; }'
cell p27_slice_dangling     reject '[*]u32 g27e; u32 main(){ g27e = alloc(u32, 4) orelse return; g27e[0] = 1; free(g27e); return 0; }'
cell p27_ptr_direct_uaf     reject 'struct T27{u32 v;} ?*T27 g27f; u32 main(){ g27f = alloc(T27); *T27 q = g27f orelse return; free(q); *T27 r = g27f orelse return; return r.v; }'
cell p27_ptr_free_through   reject 'struct T27g{u32 v;} ?*T27g g27g; u32 main(){ *T27g p = alloc(T27g) orelse return; g27g = p; *T27g q = g27g orelse return; free(q); return p.v; }'
cell p27_safe_reset         compile '?[*]u32 g27h; u32 main(){ [*]u32 s = alloc(u32, 4) orelse return; g27h = s; s[0] = 3; u32 v = s[0]; free(s); g27h = null; if (v != 3) { return 1; } return 0; }'
cell p27_safe_ptr_reset     compile 'struct T27i{u32 v;} ?*T27i g27i; u32 main(){ g27i = alloc(T27i); *T27i q = g27i orelse return; q.v = 1; free(q); g27i = null; return 0; }'
cell p27_safe_init_fini     compile '?[*]u32 g27j; void init27() { g27j = alloc(u32, 4) orelse return; } void fini27() { [*]u32 s = g27j orelse return; free(s); g27j = null; } u32 main(){ init27(); fini27(); return 0; }'
cell p27_safe_loop_reset    compile '?[*]u32 g27k; u32 main(){ for (u32 i = 0; i < 3; i += 1) { g27k = alloc(u32, 4) orelse return; [*]u32 s = g27k orelse return; s[0] = i; free(s); g27k = null; } return 0; }'

echo ""
# SHAPE p28 (BUG-1070/1071): a LEAK on ONE PATH — the path axis. The leak pass looked
# only at a block whose LAST instruction was RETURN (a `defer` put its body after
# it, so any function with a defer was exempt), skipped returns tagged "early exit",
# and merged coverage across returns (freed on SOME path counted for all). Every
# HOLE cell compiled pre-fix. The SAFE cells pin the remedies (defer, free-before-return).
echo "===== SHAPE p28 = a leak on ONE return path ====="
cell p28_leak_with_defer      reject 'u32 main(){ u32 n = 0; defer n += 1; [*]u32 a = alloc(u32, 4) orelse return; a[0] = 1; return 0; }'
cell p28_leak_if_return       reject 'u32 r28(u32 k){ [*]u32 a = alloc(u32, 4) orelse return; if (k == 1) { return 1; } free(a); return 0; } u32 main(){ return r28(1) - 1; }'
cell p28_leak_orelse_fallback reject 'struct T28{u32 v;} u32 main(){ *T28 a = alloc(T28) orelse return; *T28 b = alloc(T28) orelse { return 1; }; free(a); free(b); return 0; }'
cell p28_leak_switch_arm      reject 'enum M28 { a, b } u32 r28s(M28 m){ [*]u32 x = alloc(u32, 4) orelse return; switch (m) { .a => { return 1; } .b => { x[0] = 1; } } free(x); return 0; } u32 main(){ return r28s(M28.b); }'
cell p28_summary_through_defer reject 'u32 n28 = 0; void z28([*]u32 p){ defer n28 += 1; free(p); return; } u32 main(){ [*]u32 a = alloc(u32, 4) orelse return; z28(a); free(a); return 0; }'
cell p28_safe_defer_free      compile 'struct T28b{u32 v;} u32 r28b(u32 k){ *T28b a = alloc(T28b) orelse return; defer free(a); if (k == 1) { return 1; } return 0; } u32 main(){ return r28b(1) - 1; }'
cell p28_safe_free_each_path  compile 'u32 r28c(u32 k){ [*]u32 a = alloc(u32, 4) orelse return; if (k == 1) { free(a); return 1; } free(a); return 0; } u32 main(){ return r28c(1) - 1; }'

echo ""
# SHAPE p29 (BUG-1072): OVERWRITE of a live holder — spelling axis. Only
# `x = alloc(...)` was checked; the alias and slot spellings dropped the only
# reference silently.
echo "===== SHAPE p29 = overwrite of a live allocation holder ====="
cell p29_alias_slice          reject 'u32 main(){ [*]u32 a = alloc(u32, 4) orelse return; defer free(a); [*]u32 b = alloc(u32, 4) orelse return; b = a; b[0] = 1; return 0; }'
cell p29_alias_ptr            reject 'struct T29{u32 v;} u32 main(){ *T29 a = alloc(T29) orelse return; defer free(a); *T29 b = alloc(T29) orelse return; b = a; return b.v; }'
cell p29_slot_alias           reject 'struct T29b{u32 v;} struct H29b{*T29b p;} u32 main(){ *T29b a = alloc(T29b) orelse return; defer free(a); H29b h; h.p = alloc(T29b) orelse return; h.p = a; return h.p.v; }'
cell p29_slot_twice           reject 'struct T29c{u32 v;} struct H29c{*T29c p;} u32 main(){ H29c h; h.p = alloc(T29c) orelse return; h.p = alloc(T29c) orelse return; free(h.p); return 0; }'
cell p29_safe_alias_after_free compile 'u32 main(){ [*]u32 a = alloc(u32, 4) orelse return; defer free(a); [*]u32 b = alloc(u32, 4) orelse return; free(b); b = a; b[0] = 1; return 0; }'
cell p29_safe_second_holder   compile 'u32 main(){ [*]u32 a = alloc(u32, 4) orelse return; [*]u32 c = a; [*]u32 b = alloc(u32, 4) orelse { free(a); return 1; }; defer free(b); c = b; c[0] = 1; free(a); return 0; }'

echo ""
# SHAPE p30 (BUG-1075/1080/1076): a VIEW that arrives through a CALL — the
# multi-param pick (either param may come back), a struct-returning wrapper
# (the param comes back in a FIELD), and a callee that frees a FIELD through a
# pointer VIEW of the struct — crossed with the sink (field read / copy / free).
echo "===== SHAPE p30 = a view arriving through a call, at every sink ====="
cell p30_pick_field_read      reject 'struct T30{u32 v;} *T30 pk30(*T30 x, *T30 y, bool c){ if (c) { return x; } return y; } u32 main(){ *T30 a = alloc(T30) orelse return; *T30 d = alloc(T30) orelse { free(a); return 1; }; *T30 c = pk30(d, a, false); free(a); u32 r = c.v; free(d); return r; }'
cell p30_pick_copy            reject 'struct T30b{u32 v;} *T30b pk30b(*T30b x, *T30b y, bool c){ if (c) { return x; } return y; } u32 main(){ *T30b a = alloc(T30b) orelse return; *T30b d = alloc(T30b) orelse { free(a); return 1; }; *T30b c = pk30b(d, a, false); free(a); *T30b e = c; u32 r = e.v; free(d); return r; }'
cell p30_pick_free            reject 'struct T30c{u32 v;} *T30c pk30c(*T30c x, *T30c y, bool c){ if (c) { return x; } return y; } u32 main(){ *T30c a = alloc(T30c) orelse return; *T30c d = alloc(T30c) orelse { free(a); return 1; }; *T30c c = pk30c(d, a, false); free(c); free(a); free(d); return 0; }'
cell p30_wrap_field_uaf       reject 'struct T30d{u32 v;} struct H30d{*T30d p;} H30d mk30d(*T30d a){ H30d h = { .p = a }; return h; } u32 main(){ *T30d a = alloc(T30d) orelse return; H30d h = mk30d(a); free(a); return h.p.v; }'
cell p30_wrap_free_field      reject 'struct H30e{[*]u32 p;} H30e mk30e([*]u32 a){ H30e h = { .p = a }; return h; } u32 main(){ [*]u32 a = alloc(u32, 4) orelse return; H30e h = mk30e(a); free(h.p); u32 r = a[0]; free(a); return r; }'
cell p30_callee_field_ptrview reject 'struct T30f{u32 v;} struct H30f{*T30f p;} void z30f(*H30f h){ free(h.p); } u32 main(){ *T30f a = alloc(T30f) orelse return; H30f h = { .p = a }; *H30f hp = &h; z30f(hp); u32 r = h.p.v; free(a); return r; }'
cell p30_wrap_may_field       reject 'struct T30i{u32 v;} struct H30i{*T30i p;} T30i g30i; H30i mk30i(*T30i a, bool c){ if (c) { H30i h = { .p = a }; return h; } H30i k = { .p = &g30i }; return k; } u32 main(){ *T30i a = alloc(T30i) orelse return; H30i h = mk30i(a, true); free(a); return h.p.v; }'
cell p30_safe_wrap_may_field  compile 'struct T30j{u32 v;} struct H30j{*T30j p;} T30j g30j; H30j mk30j(*T30j a, bool c){ if (c) { H30j h = { .p = a }; return h; } H30j k = { .p = &g30j }; return k; } u32 main(){ *T30j a = alloc(T30j) orelse return; a.v = 7; H30j h = mk30j(a, true); u32 r = h.p.v; free(a); return r - 7; }'
cell p30_safe_pick_use_first  compile 'struct T30g{u32 v;} *T30g pk30g(*T30g x, *T30g y, bool c){ if (c) { return x; } return y; } u32 main(){ *T30g a = alloc(T30g) orelse return; *T30g d = alloc(T30g) orelse { free(a); return 1; }; *T30g c = pk30g(d, a, false); u32 r = c.v; free(a); free(d); return r; }'
cell p30_safe_wrap_free_field compile 'struct T30h{u32 v;} struct H30h{*T30h p;} H30h mk30h(*T30h a){ H30h h = { .p = a }; return h; } u32 main(){ *T30h a = alloc(T30h) orelse return; a.v = 7; H30h h = mk30h(a); u32 r = h.p.v; free(h.p); return r - 7; }'

echo ""
# SHAPE p31 (BUG-1073/1074): partial move then whole copy, and a slot addressed by
# a VARIABLE index.
echo "===== SHAPE p31 = partial move / variable-index slot ====="
cell p31_partial_move_copy    reject 'move struct K31{u32 k;} struct W31{K31 t; u32 x;} void c31(K31 t){ } u32 main(){ W31 w; w.t.k = 5; K31 a = w.t; W31 w2 = w; c31(w2.t); c31(a); return 0; }'
cell p31_partial_move_arg     reject 'move struct K31b{u32 k;} struct W31b{K31b t; u32 x;} void c31b(K31b t){ } void tk31b(W31b w){ c31b(w.t); } u32 main(){ W31b w; w.t.k = 5; K31b a = w.t; tk31b(w); c31b(a); return 0; }'
cell p31_var_index_uaf        reject 'struct T31{u32 v;} u32 r31(u32 k){ if (k >= 4) { return 0; } ?*T31[4] arr; *T31 a = alloc(T31) orelse return; arr[k] = a; free(a); *T31 q = arr[k] orelse return; return q.v; } u32 main(){ return r31(1); }'
cell p31_safe_var_index_loop  compile 'struct T31b{u32 v;} ?*T31b[4] t31b; u32 main(){ for (u32 i = 0; i < 4; i += 1) { *T31b a = alloc(T31b) orelse return; a.v = i; t31b[i] = a; } for (u32 i = 0; i < 4; i += 1) { *T31b q = t31b[i] orelse return; free(q); t31b[i] = null; } return 0; }'
cell p31_safe_field_use       compile 'move struct K31c{u32 k;} struct W31c{K31c t; u32 x;} void c31c(K31c t){ } u32 main(){ W31c w; w.t.k = 5; w.x = 3; K31c a = w.t; u32 y = w.x; c31c(a); return y - 3; }'

# ---------------------------------------------------------------------------
# SHAPE p32 (BUG-1125): a GLOBAL lent to a scoped spawn, reached by a CALLEE of the
# parent during the window. BUG-1118 refused the parent's own `counter += 1` and
# accepted the same statement moved into `bump()` — the G3 asymmetry (moving a
# statement into a helper must not change whether it races). The reach axis is the
# callee FORM: direct, transitive, recursive, read-only, @atomic (still a race with
# the thread's plain write), and a funcptr call (target unknown -> refused).
echo "===== SHAPE p32 = a lent global reached through the parent's callees ====="
cell p32_direct_write     reject  'u32 g32; void w32(*u32 p){*p+=1;} void bump32(){ g32 += 1; } u32 main(){ ThreadHandle t=spawn w32(&g32); bump32(); t.join(); return 0; }'
cell p32_read             reject  'u32 g32; void w32(*u32 p){*p+=1;} u32 peek32(){ return g32; } u32 main(){ ThreadHandle t=spawn w32(&g32); u32 v=peek32(); t.join(); return v; }'
cell p32_transitive       reject  'u32 g32; void w32(*u32 p){*p+=1;} u32 peek32(){ return g32; } u32 mid32(){ return peek32(); } u32 main(){ ThreadHandle t=spawn w32(&g32); u32 v=mid32(); t.join(); return v; }'
cell p32_recursive        reject  'u32 g32; void w32(*u32 p){*p+=1;} u32 walk32(u32 n){ if(n==0){return g32;} return walk32(n-1); } u32 main(){ ThreadHandle t=spawn w32(&g32); u32 v=walk32(3); t.join(); return v; }'
cell p32_atomic_in_callee reject  'u32 g32; void w32(*u32 p){*p+=1;} void bump32(){ @atomic_add(&g32, 1); } u32 main(){ ThreadHandle t=spawn w32(&g32); bump32(); t.join(); return 0; }'
cell p32_funcptr_call     reject  'u32 g32; void w32(*u32 p){*p+=1;} void noop32(){ } u32 main(){ *() fp=noop32; ThreadHandle t=spawn w32(&g32); fp(); t.join(); return 0; }'
# BOUNDARY: a callee touching ANOTHER global, a call after the join, and a call
# before the spawn are all fine — and a ThreadHandle's own join() is not a funcptr.
cell p32_safe_other_global compile 'u32 g32; u32 o32; void w32(*u32 p){*p+=1;} void bump32(){ o32 += 1; } u32 main(){ ThreadHandle t=spawn w32(&g32); bump32(); t.join(); if(g32!=1||o32!=1){return 1;} return 0; }'
cell p32_safe_after_join   compile 'u32 g32; void w32(*u32 p){*p+=1;} void bump32(){ g32 += 1; } u32 main(){ ThreadHandle t=spawn w32(&g32); t.join(); bump32(); if(g32!=2){return 1;} return 0; }'
cell p32_safe_before_spawn compile 'u32 g32; void w32(*u32 p){*p+=1;} void bump32(){ g32 += 1; } u32 main(){ bump32(); ThreadHandle t=spawn w32(&g32); t.join(); if(g32!=2){return 1;} return 0; }'

echo ""
# SHAPE p33 (BUG-1130): a slot at a TRACKABLE index local (`arr[k]`, k never
# address-taken) is a precise entry like `arr[3]`, valid until k is written; any
# two indices of one array may meet except two distinct literals; storing into a
# LOCAL array is not an escape. All HOLE cells compiled on the pre-change build.
echo "===== SHAPE p33 = variable-index slot identity (free-through-read, maybe-free, leak) ====="
cell p33_free_via_read          reject 'struct T33a{u32 v;} u32 r33a(u32 k){ if (k >= 4) { return 0; } ?*T33a[4] arr; arr[k] = alloc(T33a); *T33a a = arr[k] orelse return; free(a); *T33a q = arr[k] orelse return; return q.v; } u32 main(){ return r33a(1); }'
cell p33_free_via_read_loop     reject 'struct T33b{u32 v;} ?*T33b[4] t33b; u32 main(){ for (u32 i = 0; i < 4; i += 1) { *T33b a = alloc(T33b) orelse return; t33b[i] = a; } u32 s = 0; for (u32 i = 0; i < 4; i += 1) { *T33b q = t33b[i] orelse return; free(q); *T33b r = t33b[i] orelse return; s += r.v; t33b[i] = null; } return s; }'
cell p33_double_free_via_read   reject 'struct T33c{u32 v;} u32 r33c(u32 k){ if (k >= 4) { return 0; } ?*T33c[4] arr; *T33c x = alloc(T33c) orelse return; arr[k] = x; *T33c a = arr[k] orelse return; free(a); *T33c b = arr[k] orelse return; free(b); return 0; } u32 main(){ return r33c(1); }'
cell p33_cond_free_same_index   reject 'struct T33d{u32 v;} u32 r33d(u32 k, bool c){ if (k >= 4) { return 0; } ?*T33d[4] arr; *T33d a = alloc(T33d) orelse return; arr[k] = a; if (c) { free(a); } *T33d q = arr[k] orelse return; return q.v; } u32 main(){ return r33d(1, true); }'
cell p33_cond_free_other_index  reject 'struct T33e{u32 v;} u32 r33e(u32 k, u32 j, bool c){ if (k >= 4) { return 0; } if (j >= 4) { return 0; } ?*T33e[4] arr; *T33e a = alloc(T33e) orelse return; arr[k] = a; if (c) { free(a); } *T33e q = arr[j] orelse return; return q.v; } u32 main(){ return r33e(1, 1, true); }'
cell p33_literal_read_var_store reject 'struct T33f{u32 v;} u32 r33f(u32 k){ if (k >= 4) { return 0; } ?*T33f[4] arr; *T33f a = alloc(T33f) orelse return; arr[k] = a; free(a); *T33f q = arr[0] orelse return; return q.v; } u32 main(){ return r33f(0); }'
cell p33_leak_keyed_direct      reject 'struct T33g{u32 v;} u32 r33g(u32 k){ if (k >= 4) { return 0; } ?*T33g[4] arr; arr[k] = alloc(T33g); return 0; } u32 main(){ return r33g(1); }'
cell p33_leak_keyed_stored      reject 'struct T33h{u32 v;} u32 r33h(u32 k){ if (k >= 4) { return 0; } ?*T33h[4] arr; *T33h a = alloc(T33h) orelse return; arr[k] = a; return 0; } u32 main(){ return r33h(1); }'
cell p33_leak_loop              reject 'struct T33i{u32 v;} u32 r33i(){ ?*T33i[4] arr; for (u32 i = 0; i < 4; i += 1) { arr[i] = alloc(T33i); } return 0; } u32 main(){ return r33i(); }'
cell p33_leak_unkeyed_index     reject 'struct T33j{u32 v;} u32 r33j(u32 k){ ?*T33j[4] arr; arr[k % 4] = alloc(T33j); return 0; } u32 main(){ return r33j(1); }'
# BOUNDARY: the fill/drain loop over a LOCAL array (with and without the null
# reset), a direct free loop, the got-counter idiom that stores one iteration's
# allocation and frees another's, a slot reset then re-read, a callee that drains
# the array through a slice, and two distinct param indices.
cell p33_safe_fill_drain        compile 'struct T33k{u32 v;} u32 r33k(){ ?*T33k[4] arr; for (u32 i = 0; i < 4; i += 1) { *T33k a = alloc(T33k) orelse return; a.v = i; arr[i] = a; } u32 s = 0; for (u32 i = 0; i < 4; i += 1) { *T33k q = arr[i] orelse return; s += q.v; free(q); arr[i] = null; } return s - 6; } u32 main(){ return r33k(); }'
cell p33_safe_consume_no_reset  compile 'struct T33l{u32 v;} u32 r33l(){ ?*T33l[4] arr; for (u32 i = 0; i < 4; i += 1) { *T33l a = alloc(T33l) orelse return; a.v = i; arr[i] = a; } u32 s = 0; for (u32 i = 0; i < 4; i += 1) { *T33l q = arr[i] orelse return; s += q.v; free(q); } return s - 6; } u32 main(){ return r33l(); }'
cell p33_safe_direct_free_loop  compile 'struct T33m{u32 v;} u32 r33m(){ ?*T33m[4] arr; for (u32 i = 0; i < 4; i += 1) { arr[i] = alloc(T33m); } for (u32 i = 0; i < 4; i += 1) { free(arr[i] orelse return); } return 0; } u32 main(){ return r33m(); }'
cell p33_safe_got_counter       compile 'struct T33n{u32 v;} ?*T33n[8] h33n; u32 main(){ u32 got = 0; for (u32 n = 0; n < 12; n += 1) { *T33n k = alloc(T33n) orelse return; if (got < 8) { h33n[got] = k; got += 1; } else { free(k); } } for (u32 j = 0; j < 8; j += 1) { *T33n q = h33n[j] orelse return; free(q); h33n[j] = null; } return 0; }'
cell p33_safe_reset_reread      compile 'struct T33o{u32 v;} u32 r33o(u32 k){ if (k >= 4) { return 0; } ?*T33o[4] arr; *T33o x = alloc(T33o) orelse return; arr[k] = x; *T33o a = arr[k] orelse return; free(a); arr[k] = null; if (arr[k]) |q| { return q.v; } return 0; } u32 main(){ return r33o(1); }'
cell p33_safe_drain_by_callee   compile 'struct T33q{u32 v;} void dr33q([*]?*T33q s){ for (u32 i = 0; i < s.len; i += 1) { *T33q q = s[i] orelse return; free(q); s[i] = null; } } u32 main(){ ?*T33q[4] arr; for (u32 i = 0; i < 4; i += 1) { *T33q a = alloc(T33q) orelse return; arr[i] = a; } dr33q(arr); return 0; }'
cell p33_safe_two_param_slots   compile 'struct T33p{u32 v;} u32 r33p(u32 k, u32 j){ if (k >= 4) { return 0; } if (j >= 4) { return 0; } ?*T33p[4] arr; *T33p a = alloc(T33p) orelse return; defer free(a); a.v = 1; arr[k] = a; *T33p q = arr[k] orelse return; return q.v - 1; } u32 main(){ return r33p(1, 2); }'

echo ""
# SHAPE p34 (BUG-1131/1132/1133): a struct VALUE that reached the return through an
# element, a field, an assigned literal, a chained wrapper, a pointer deref or a
# callee that filled it through `*H` still carries the param's allocation in its
# field (ir_carry_projection / ir_store_struct_literal / the MAY backstop).
echo "===== SHAPE p34 = struct wrapper whose value came through an element / field / chain ====="
cell p34_elem_literal           reject 'struct T34a{u32 v;} struct H34a{*T34a p;} H34a mk34a(*T34a a){ H34a[2] hs; hs[0] = { .p = a }; return hs[0]; } u32 main(){ *T34a a = alloc(T34a) orelse return; H34a h = mk34a(a); free(a); return h.p.v; }'
cell p34_elem_unkeyed           reject 'struct T34b{u32 v;} struct H34b{*T34b p;} H34b mk34b(*T34b a, u32 k){ H34b[2] hs; hs[k % 2] = { .p = a }; return hs[k % 2]; } u32 main(){ *T34b a = alloc(T34b) orelse return; H34b h = mk34b(a, 1); free(a); return h.p.v; }'
cell p34_nested_field           reject 'struct T34c{u32 v;} struct H34c{*T34c p;} struct W34c{H34c h; u32 n;} H34c mk34c(*T34c a){ W34c w; w.h = { .p = a }; return w.h; } u32 main(){ *T34c a = alloc(T34c) orelse return; H34c h = mk34c(a); free(a); return h.p.v; }'
cell p34_assign_literal_same_fn reject 'struct T34d{u32 v;} struct H34d{*T34d p;} u32 main(){ *T34d a = alloc(T34d) orelse return; H34d[2] hs; hs[0] = { .p = a }; free(a); return hs[0].p.v; }'
cell p34_chained_wrapper        reject 'struct T34e{u32 v;} struct H34e{*T34e p;} H34e mk34e(*T34e a){ return { .p = a }; } H34e mk34e2(*T34e a){ return mk34e(a); } u32 main(){ *T34e a = alloc(T34e) orelse return; H34e h = mk34e2(a); free(a); return h.p.v; }'
cell p34_deref_backstop         reject 'struct T34f{u32 v;} struct H34f{*T34f p;} H34f mk34f(*T34f a){ H34f h = { .p = a }; *H34f hp = &h; return *hp; } u32 main(){ *T34f a = alloc(T34f) orelse return; H34f h = mk34f(a); free(a); return h.p.v; }'
cell p34_callee_fill_backstop   reject 'struct T34g{u32 v;} struct H34g{*T34g p;} void fl34g(*H34g o, *T34g a){ o.p = a; } H34g mk34g(*T34g a){ H34g h; fl34g(&h, a); return h; } u32 main(){ *T34g a = alloc(T34g) orelse return; H34g h = mk34g(a); free(a); return h.p.v; }'
# BOUNDARY: use before the free, free through the returned field, a field set to
# a GLOBAL (the backstop is gated on an opaque value), and a field holding a
# DIFFERENT param than the one freed.
cell p34_safe_elem_use_first    compile 'struct T34h{u32 v;} struct H34h{*T34h p;} H34h mk34h(*T34h a){ H34h[2] hs; hs[0] = { .p = a }; return hs[0]; } u32 main(){ *T34h a = alloc(T34h) orelse return; a.v = 5; H34h h = mk34h(a); u32 r = h.p.v; free(a); return r - 5; }'
cell p34_safe_elem_free_field   compile 'struct T34i{u32 v;} struct H34i{*T34i p;} H34i mk34i(*T34i a){ H34i[2] hs; hs[0] = { .p = a }; return hs[0]; } u32 main(){ *T34i a = alloc(T34i) orelse return; a.v = 5; H34i h = mk34i(a); u32 r = h.p.v; free(h.p); return r - 5; }'
cell p34_safe_global_field      compile 'struct T34k{u32 v;} struct H34k{*T34k p;} T34k g34k; H34k mk34k(*T34k a){ H34k h; h.p = &g34k; u32 x = a.v; return h; } u32 main(){ *T34k a = alloc(T34k) orelse return; g34k.v = 4; H34k h = mk34k(a); free(a); return h.p.v - 4; }'
cell p34_safe_other_param       compile 'struct T34j{u32 v;} struct H34j{*T34j p;} H34j mk34j(*T34j a, *T34j b){ H34j[2] hs; hs[0] = { .p = b }; u32 x = a.v; return hs[0]; } u32 main(){ *T34j b = alloc(T34j) orelse return; defer free(b); *T34j a = alloc(T34j) orelse return; b.v = 3; H34j h = mk34j(a, b); free(a); return h.p.v - 3; }'

# SHAPE p35 (BUG-1158/1159/1160): a STRUCT LITERAL carrying a frame-bound pointer —
# VALUE SHAPE x SINK. The literal walker knew four field-value shapes and every
# other sink routed through arg_is_local_derived, which had no literal case; an
# assigned literal never tainted its local; a spawn literal was never typed
# before the gates. One cell per shape the audit found, plus the sinks.
echo "===== SHAPE p35 = a struct literal carrying a frame-bound pointer (shape x sink) ====="
cell p35_array_coerce_global  reject 'struct R35{[*]u32 s;} R35 g35; void f35(){ u32[4] a; g35 = { .s = a }; } u32 main(){ f35(); return 0; }'
cell p35_field_value_return   reject 'struct H35{*u32 q;} struct R35{*u32 p;} R35 f35(){ u32 x = 5; H35 h = { .q = &x }; return { .p = h.q }; } u32 main(){ return *f35().p; }'
cell p35_orelse_value_return  reject 'struct R35{*u32 p;} ?*u32 no35(){ return null; } R35 f35(){ u32 x = 5; return { .p = no35() orelse &x }; } u32 main(){ return *f35().p; }'
cell p35_assign_local_return  reject 'struct R35{*u32 p;} R35 f35(){ u32 x = 5; R35 r; r = { .p = &x }; return r; } u32 main(){ return *f35().p; }'
cell p35_keep_call_arg        reject 'struct M35{*u32 p;} M35 gm35; void st35(M35 m){ gm35 = m; } void f35(){ u32 x = 5; st35({ .p = &x }); } u32 main(){ f35(); return 0; }'
cell p35_spawn_arg            reject 'struct M35{*u32 p;} void w35(M35 m){ u32 v = *m.p; } void f35(){ u32 x = 5; spawn w35({ .p = &x }); } u32 main(){ f35(); return 0; }'
# BOUNDARY: a scalar read out of a local array is a VALUE, and a global array is
# not frame-bound.
cell p35_safe_scalar_elem     compile 'struct N35{u32 n;} N35 g35; void f35(){ u32[4] a; a[0] = 3; g35 = { .n = a[0] }; } u32 main(){ f35(); return g35.n - 3; }'
cell p35_safe_global_array    compile 'struct R35{[*]u32 s;} u32[4] ga35; R35 g35; u32 main(){ g35 = { .s = ga35 }; return (u32)g35.s.len - 4; }'

# SHAPE p36 (BUG-1181): a CALLEE frees the allocation a GLOBAL holds; the caller
# still reads its LOCAL alias. Every reject cell compiled on the pre-fix build (the
# read returned a recycled slot's value — alloc(T) recycles, so ASan is blind).
echo "===== SHAPE p36 = a callee frees through a global (spelling x reach) ====="
cell p36_capture          reject 'struct T36{u32 v;} ?*T36 g36; void dg36(){ if (g36) |p| { free(p); } g36 = null; } u32 main(){ *T36 a = alloc(T36) orelse return; a.v = 5; g36 = a; dg36(); return a.v; }'
cell p36_orelse_unwrap    reject 'struct T36{u32 v;} ?*T36 g36; void dg36(){ *T36 p = g36 orelse return; free(p); g36 = null; } u32 main(){ *T36 a = alloc(T36) orelse return; a.v = 5; g36 = a; dg36(); return a.v; }'
cell p36_field_global     reject 'struct T36{u32 v;} struct H36{?*T36 p;} H36 gh36; void dg36(){ if (gh36.p) |p| { free(p); } gh36.p = null; } u32 main(){ *T36 a = alloc(T36) orelse return; a.v = 5; gh36.p = a; dg36(); return a.v; }'
cell p36_transitive       reject 'struct T36{u32 v;} ?*T36 g36; void dg36(){ if (g36) |p| { free(p); } g36 = null; } void out36(){ dg36(); } u32 main(){ *T36 a = alloc(T36) orelse return; a.v = 5; g36 = a; out36(); return a.v; }'
cell p36_double_free      reject 'struct T36{u32 v;} ?*T36 g36; void dg36(){ if (g36) |p| { free(p); } g36 = null; } u32 main(){ *T36 a = alloc(T36) orelse return; g36 = a; dg36(); free(a); return 0; }'
# BOUNDARY: a read BEFORE the call, and a callee that only uses the global.
cell p36_safe_read_before compile 'struct T36{u32 v;} ?*T36 g36; void dg36(){ if (g36) |p| { free(p); } g36 = null; } u32 main(){ *T36 a = alloc(T36) orelse return; a.v = 5; u32 r = a.v; g36 = a; dg36(); return r - 5; }'
cell p36_safe_no_free     compile 'struct T36{u32 v;} ?*T36 g36; void tg36(){ if (g36) |p| { p.v += 1; } } u32 main(){ *T36 a = alloc(T36) orelse return; a.v = 5; g36 = a; tg36(); u32 r = a.v; g36 = null; free(a); return r - 6; }'

# SHAPE p37 (BUG-1186): a union `|*w|` capture outliving its variant. The in-arm
# mutation rule saw only direct writes; a CALLEE, an alias, a global, or the
# capture escaping the arm changed the variant under it. Pre-fix every reject cell
# compiled, and an integer variant became a working pointer through `*w.ptr`.
echo "===== SHAPE p37 = a union pointer capture vs a variant change (spelling x reach) ====="
P37='struct P37{*u32 ptr;} union U37{u64 n; P37 p;} u32[4] b37; u32[4] v37;'
cell p37_callee_alias      reject "$P37"' void mu37(*U37 u, u64 a){ u.n = a; } u32 main(){ U37 u; P37 pp = { .ptr = &b37[0] }; u.p = pp; *U37 up = &u; switch (u) { .p => |*w| { mu37(up, 7); *w.ptr = 1; } .n => |n| { } } return 0; }'
cell p37_callee_global     reject "$P37"' U37 gu37; void rs37(){ gu37.n = 7; } u32 main(){ P37 pp = { .ptr = &b37[0] }; gu37.p = pp; switch (gu37) { .p => |*w| { rs37(); *w.ptr = 1; } .n => |n| { } } return 0; }'
cell p37_callee_transitive reject "$P37"' U37 gu37; void rs37(){ gu37.n = 7; } void ou37(){ rs37(); } u32 main(){ P37 pp = { .ptr = &b37[0] }; gu37.p = pp; switch (gu37) { .p => |*w| { ou37(); *w.ptr = 1; } .n => |n| { } } return 0; }'
cell p37_escape_assign     reject "$P37"' u32 main(){ U37 u; P37 pp = { .ptr = &b37[0] }; u.p = pp; ?*P37 k = null; switch (u) { .p => |*w| { k = w; } .n => |n| { } } u.n = 7; if (k) |kp| { *kp.ptr = 1; } return 0; }'
cell p37_escape_derived    reject "$P37"' u32 main(){ U37 u; P37 pp = { .ptr = &b37[0] }; u.p = pp; ?*P37 k = null; switch (u) { .p => |*w| { *P37 q = w; k = q; } .n => |n| { } } return 0; }'
cell p37_escape_return     reject "$P37"' U37 gu37; ?*P37 f37(){ switch (gu37) { .p => |*w| { return w; } .n => |n| { } } return null; } u32 main(){ ?*P37 r = f37(); return 0; }'
cell p37_escape_keep       reject "$P37"' U37 gu37; ?*P37 gk37; void st37(*P37 q){ gk37 = q; } u32 main(){ P37 pp = { .ptr = &b37[0] }; gu37.p = pp; switch (gu37) { .p => |*w| { st37(w); } .n => |n| { } } return 0; }'
# BOUNDARY: a callee that does not assign a U; a value READ out of the variant;
# a value capture (a copy).
cell p37_safe_pure_callee  compile "$P37"' u32 rd37(*P37 q){ return *q.ptr; } u32 main(){ U37 u; P37 pp = { .ptr = &b37[0] }; u.p = pp; u32 r = 0; switch (u) { .p => |*w| { r = rd37(w); *w.ptr = 1; } .n => |n| { } } return r; }'
cell p37_safe_field_read   compile "$P37"' u32 main(){ U37 u; P37 pp = { .ptr = &b37[0] }; u.p = pp; ?*u32 k = null; switch (u) { .p => |*w| { k = w.ptr; } .n => |n| { } } u.n = 7; if (k) |kp| { *kp = 1; } return 0; }'
cell p37_safe_value_cap    compile "$P37"' U37 gu37; void rs37(){ gu37.n = 7; } u32 main(){ P37 pp = { .ptr = &b37[0] }; gu37.p = pp; switch (gu37) { .p => |w| { rs37(); *w.ptr = 1; } .n => |n| { } } return 0; }'

# SHAPE p38 (BUG-1187): a reference formed into a CALL's returned temporary. Every
# reject cell but the `&` one compiled pre-fix (ASan stack-use-after-return); `&`
# reached GCC as "lvalue required".
echo "===== SHAPE p38 = a view into a function's returned temporary (spelling x sink) ====="
P38='struct B38{u32[4] arr; [*]u32 s;} B38 mk38(u32 k){ B38 b; b.arr[2] = k; return b; }'
cell p38_slice_return      reject "$P38"' [*]u32 f38(u32 k){ return mk38(k).arr[0..]; } u32 main(){ [*]u32 s = f38(7); return s[2]; }'
cell p38_slice_global      reject "$P38"' [*]u32 g38; u32 main(){ g38 = mk38(5).arr[0..]; return 0; }'
cell p38_decay_return      reject "$P38"' [*]u32 f38(u32 k){ return mk38(k).arr; } u32 main(){ return f38(1)[2]; }'
cell p38_addr_of           reject "$P38"' u32 main(){ *u32 p = &mk38(9).arr[2]; return *p; }'
cell p38_slice_local       reject "$P38"' u32 main(){ [*]u32 s = mk38(3).arr[1..3]; return s[1]; }'
# BOUNDARY: bind the value first; a slice FIELD of the temporary points elsewhere.
cell p38_safe_bound        compile "$P38"' u32 main(){ B38 b = mk38(3); [*]u32 s = b.arr[0..]; return s[2] - 3; }'
cell p38_safe_value_read   compile "$P38"' u32 main(){ return mk38(4).arr[2] - 4; }'
cell p38_safe_slice_field  compile "$P38"' u32[4] ga38; B38 mks38(){ B38 b; b.s = ga38; return b; } u32 main(){ [*]u32 s = mks38().s[0..2]; return (u32)s.len - 2; }'

# SHAPE p39 (BUG-1225..1230): a freed / interior pointer reaching a FREE or USE sink
# through a CARRIER the tracker did not follow — a struct argument, a field of a
# variable-index element, a factory-returned struct, an out-param, a move struct,
# a sub-slice. Every reject cell compiled pre-fix (a recycled object read, a
# double free, or glibc's abort on a non-heap free).
echo "===== SHAPE p39 = freed / interior pointer through a carrier (carrier x sink) ====="
P39='struct T39{u32 v;} struct H39{*T39 p;} struct O39{?*T39 p;}'
cell p39_struct_arg_ptr    reject "$P39"' u32 rd39(*H39 h){ return h.p.v; } u32 main(){ *T39 a = alloc(T39) orelse return; H39 h = { .p = a }; free(a); return rd39(&h); }'
cell p39_struct_arg_value  reject "$P39"' u32 rd39(H39 h){ return h.p.v; } u32 main(){ *T39 a = alloc(T39) orelse return; H39 h = { .p = a }; free(a); return rd39(h); }'
cell p39_varidx_field      reject "$P39"' u32 main(){ O39[4] l; l[0].p = alloc(T39); u32 i = 0; if (l[i].p) |p| { free(p); } u32 r = 0; if (l[i].p) |q| { r = q.v; } return r; }'
cell p39_factory_field     reject "$P39"' H39 mk39(){ *T39 p = alloc(T39) orelse { H39 e; return e; }; H39 r = { .p = p }; return r; } u32 main(){ H39 a = mk39(); free(a.p); free(a.p); return 0; }'
cell p39_outparam          reject "$P39"' void fill39(*?*T39 o){ *o = alloc(T39); } u32 main(){ ?*T39 m = null; fill39(&m); if (m) |p| { free(p); } if (m) |p| { free(p); } return 0; }'
cell p39_move_callee       reject "$P39"' move struct M39{*T39 p;} void eat39(M39 m){ free(m.p); } u32 main(){ *T39 a = alloc(T39) orelse return; M39 m = { .p = a }; eat39(m); free(a); return 0; }'
cell p39_free_subslice     reject "$P39"' u32 main(){ [*]T39 s = alloc(T39, 4) orelse return; [*]T39 t = s[1..4]; free(t); return 0; }'
cell p39_free_subslice_fn  reject "$P39"' void k39([*]T39 x){ free(x); } u32 main(){ [*]T39 s = alloc(T39, 4) orelse return; k39(s[1..4]); return 0; }'
cell p39_free_elem_addr    reject "$P39"' u32 main(){ [*]T39 s = alloc(T39, 4) orelse return; *T39 e = &s[1]; free(e); return 0; }'
# BOUNDARY: the field reset after the free clears the carrier; a view that starts
# at 0 IS the allocation; a move struct consumed by the freeing callee is done.
cell p39_safe_reset        compile "$P39"' u32 rd39(*O39 h){ if (h.p) |q| { return q.v; } return 0; } u32 main(){ O39 h; h.p = alloc(T39); if (h.p) |p| { free(p); } h.p = null; return rd39(&h); }'
cell p39_safe_base_view    compile "$P39"' u32 main(){ [*]T39 s = alloc(T39, 4) orelse return; [*]T39 t = s[0..4]; free(t); return 0; }'
cell p39_safe_move_done    compile "$P39"' move struct M39{*T39 p;} void eat39(M39 m){ free(m.p); } u32 main(){ *T39 a = alloc(T39) orelse return; M39 m = { .p = a }; eat39(m); return 0; }'

# SHAPE p40 (BUG-1231..1233, 1241, 1242): an allocation or a frame pointer reaching
# a sink THROUGH AN ASYNC TASK or a callee store — the task keeps every `_init`
# argument, a poll runs the body, and a suspend lets the poller run anything.
echo "===== SHAPE p40 = async task / callee-store carrier (carrier x sink) ====="
P40='struct T40{u32 v;} struct S40{?*T40 b;} ?*T40 g40;'
cell p40_init_callers_task  reject "$P40"' async void f40(*u32 p){ yield; *p = 1; } void st40(*_zer_async_f40 t){ u32 x = 5; _zer_async_f40_init(t, &x); } u32 main(){ _zer_async_f40 t; st40(&t); _zer_async_f40_poll(&t); return 0; }'
cell p40_body_stores_param  reject "$P40"' ?*u32 gp40; async void f40(*u32 p){ gp40 = p; yield; } void st40(){ u32 x = 5; _zer_async_f40 t; _zer_async_f40_init(&t, &x); _zer_async_f40_poll(&t); } u32 main(){ st40(); return 0; }'
cell p40_free_between_polls reject "$P40"' async u32 f40(*T40 b){ yield; return b.v; } u32 main(){ *T40 p = alloc(T40) orelse return; _zer_async_f40 t; _zer_async_f40_init(&t, p); _zer_async_f40_poll(&t); free(p); _zer_async_f40_poll(&t); return 0; }'
cell p40_body_frees_param   reject "$P40"' async void f40(*T40 b){ yield; free(b); } u32 main(){ *T40 p = alloc(T40) orelse return; _zer_async_f40 t; _zer_async_f40_init(&t, p); while (_zer_async_f40_poll(&t) == 0) { } free(p); return 0; }'
cell p40_yield_global_alias reject "$P40"' async u32 f40(){ *T40 a = alloc(T40) orelse return; g40 = a; yield; return a.v; } u32 main(){ return 0; }'
cell p40_callee_stores      reject "$P40"' void in40(*S40 s, *T40 b){ s.b = b; } void rd40(*S40 s){ if (s.b) |b| { b.v = 1; } } u32 main(){ *T40 p = alloc(T40) orelse return; S40 t; in40(&t, p); free(p); rd40(&t); return 0; }'
cell p40_helper_poll_frees  reject "$P40"' async void f40(*T40 b){ yield; free(b); } void dr40(*_zer_async_f40 t){ while (_zer_async_f40_poll(t) == 0) { } } u32 main(){ *T40 p = alloc(T40) orelse return; _zer_async_f40 t; _zer_async_f40_init(&t, p); dr40(&t); return p.v; }'
cell p40_global_capture     reject "$P40"' void rel40(){ if (g40) |b| { free(b); } g40 = null; } void f40(){ if (g40) |b| { rel40(); b.v = 9; } } u32 main(){ g40 = alloc(T40); f40(); return 0; }'
# BOUNDARY: a task in this frame may hold this frame's pointers; a task that frees
# its param owns it; a carried field reset after the free is clean.
cell p40_safe_frame_task    compile "$P40"' async void f40(*u32 p){ yield; *p = 1; } u32 main(){ u32 x = 5; _zer_async_f40 t; _zer_async_f40_init(&t, &x); while (_zer_async_f40_poll(&t) == 0) { } return 0; }'
cell p40_safe_task_owns     compile "$P40"' async void f40(*T40 b){ yield; free(b); } u32 main(){ *T40 p = alloc(T40) orelse return; _zer_async_f40 t; _zer_async_f40_init(&t, p); while (_zer_async_f40_poll(&t) == 0) { } return 0; }'
cell p40_safe_store_reset   compile "$P40"' void in40(*S40 s, *T40 b){ s.b = b; } void rd40(*S40 s){ if (s.b) |b| { b.v = 1; } } u32 main(){ *T40 p = alloc(T40) orelse return; S40 t; in40(&t, p); free(p); t.b = null; rd40(&t); return 0; }'

echo "==================================================================="
echo "matrix: $pass ok, $fail mismatch"
[ -n "$holes" ]   && echo "HOLES (compile but should reject):$holes"
[ -n "$overrej" ] && echo "OVER-REJECTS (reject but should compile):$overrej"
[ $fail -eq 0 ] && echo "SINK MATRIX CLEAN" || echo "SINK MATRIX HAS $fail MISMATCH(es)"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
