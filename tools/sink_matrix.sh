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
cell p21_safe_fresh        compile 'struct T21c{u32 v;} u8[64] gb21d; u8[64] gb21e; u32 main(){ Arena a21=Arena.over(gb21d); *T21c t=a21.alloc(T21c) orelse {return 1;}; t.v=4; a21=Arena.over(gb21e); *T21c u=a21.alloc(T21c) orelse {return 2;}; u.v=5; if(t.v+u.v!=9){return 3;} return 0; }'
cell p21_safe_local_move   compile 'u8[64] gb21f; Arena mk21(){ Arena a=Arena.over(gb21f); return a; } u32 main(){ Arena a21=mk21(); return 0; }'
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

# SHAPE p25 (BUG-1004): A FRAME ADDRESS LAUNDERED AS AN INTEGER THROUGH A CALL.
#
# Every direct spelling of `@ptrtoint(&local)` reaching a global or a return was
# rejected (`g = @ptrtoint(&l)`, `g = a + 0`, `g.f = a`, `arr[0] = a`). The CALL
# spelling was not: the call-result escape sink is gated on the result type CARRYING
# a data pointer, and `usize` carries none — so `g = idfn(@ptrtoint(&l))` and
# `return idfn(@ptrtoint(&l))` compiled, and so did `g = leak(&l)` against
# `usize leak(*u32 p) { return @ptrtoint(p); }` (the pointer went in, the address
# came out as an integer). ONE query `call_result_is_local_address_int`: with a
# complete return summary the decision is relational (callee may return param n AND
# arg n is a frame address, pointer or integer); without one, an address-valued
# integer argument. The boundary cells are what keep the rule narrow: a scalar
# READ (`s.len`) is not a view, and a bare `&buf` handed to a function that returns
# a plain count is the `strlen` shape.
echo "===== SHAPE p25 = frame address as an INTEGER through a call ====="
cell p25_idfn_global     reject 'usize g25; usize idfn25(usize x){ return x; } u32 main(){ u32 l=5; g25 = idfn25(@ptrtoint(&l)); return 0; }'
cell p25_idfn_return     reject 'usize idfn25b(usize x){ return x; } usize leak25b(){ u32 l=5; return idfn25b(@ptrtoint(&l)); } u32 main(){ return 0; }'
cell p25_ptr_param       reject 'usize g25c; usize leak25c(*u32 p){ return @ptrtoint(p); } u32 main(){ u32 l=5; g25c = leak25c(&l); return 0; }'
cell p25_int_alias       reject 'usize g25d; usize idfn25d(usize x){ return x; } u32 main(){ u32 l=5; usize a=@ptrtoint(&l); g25d = idfn25d(a); return 0; }'
cell p25_arith_arg       reject 'usize g25e; usize idfn25e(usize x){ return x; } u32 main(){ u32 l=5; g25e = idfn25e(@ptrtoint(&l) + 4); return 0; }'
# BOUNDARY: a scalar field read is RET_STATIC (no view); a callee returning a fresh
# count from a pointer param has an empty mask; a global's address is not frame-bound.
cell p25_safe_len_read   compile 'usize g25f; usize len25f([*]u8 s){ return s.len; } u32 main(){ u8[4] b; g25f = len25f(b); return (u32)(g25f - 4); }'
cell p25_safe_count      compile 'usize g25g; usize cnt25g(*u32 p){ if (*p == 0) { return 0; } return 1; } u32 main(){ u32 l=5; g25g = cnt25g(&l); return (u32)(g25g - 1); }'
cell p25_safe_global_addr compile 'usize g25h; u32 gv25h; usize idfn25h(usize x){ return x; } u32 main(){ g25h = idfn25h(@ptrtoint(&gv25h)); return 0; }'


echo ""
echo "==================================================================="
echo "matrix: $pass ok, $fail mismatch"
[ -n "$holes" ]   && echo "HOLES (compile but should reject):$holes"
[ -n "$overrej" ] && echo "OVER-REJECTS (reject but should compile):$overrej"
[ $fail -eq 0 ] && echo "SINK MATRIX CLEAN" || echo "SINK MATRIX HAS $fail MISMATCH(es)"
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
