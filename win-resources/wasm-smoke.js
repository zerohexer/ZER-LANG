// Smoke test for the ZER WASM bridge. Run with node inside the emsdk image.
// Validates: module loads, diagnostics entry returns JSON, emit entry returns C.
const ZerModule = require('/zer/zer.js');

(async () => {
    const M = await ZerModule();
    const diagnostics = M.cwrap('zer_diagnostics_json', 'string', ['string', 'string']);
    const emitC = M.cwrap('zer_emit_c', 'string', ['string', 'string', 'number']);
    const version = M.cwrap('zer_version', 'string', []);

    console.log('zer_version =', version());

    // 1) Clean program -> no diagnostics
    const ok = `i32 main() { i32 x = 5; return 0; }`;
    const d1 = diagnostics(ok, 'ok.zer');
    console.log('CLEAN diagnostics:', d1);

    // 2) Program with a type error -> diagnostics present
    const bad = `void puts(const *u8 s);\ni32 main() { puts("hi", 1); return 0; }`;
    const d2 = diagnostics(bad, 'bad.zer');
    console.log('ERROR diagnostics:', d2);

    // 3) Variadic (the feature we just shipped) -> clean
    const varg = `i32 printf(const *u8 fmt, ...);\ni32 main() { printf("v=%d\\n", 7); return 0; }`;
    const d3 = diagnostics(varg, 'varg.zer');
    console.log('VARIADIC diagnostics:', d3);

    // 4) Emit C without tracking (track_cptrs=0): must NOT define __wrap_malloc
    const e0 = JSON.parse(emitC(ok, 'ok.zer', 0));
    console.log('EMIT(track=0) ok=', e0.ok, ' C length=', e0.ok ? e0.c.length : 0,
                ' has __wrap_malloc=', e0.ok ? e0.c.includes('__wrap_malloc') : 'n/a');

    // 5) Emit C WITH tracking (track_cptrs=1, the --run path): MUST define
    //    __wrap_malloc (Level 5 cross-C-boundary UAF interception).
    const e1 = JSON.parse(emitC(ok, 'ok.zer', 1));
    console.log('EMIT(track=1) ok=', e1.ok, ' has __wrap_malloc=', e1.ok ? e1.c.includes('__wrap_malloc') : 'n/a');

    // 6) The production safety analyzer (zercheck_ir) MUST reject a double-free
    //    on the compile path — proves the CFG analyzer is wired into zer_emit_c,
    //    not just the type checker.
    const doubleFree = 'struct Task { u32 id; }\n'
        + 'Pool(Task, 4) pool;\n'
        + 'u32 main() {\n'
        + '    Handle(Task) h = pool.alloc() orelse return;\n'
        + '    pool.free(h);\n'
        + '    pool.free(h);\n'
        + '    return 0;\n'
        + '}\n';
    const e2 = JSON.parse(emitC(doubleFree, 'df.zer', 1));
    console.log('EMIT(double-free) ok=', e2.ok, '(MUST be false — zercheck_ir gate)');

    // 7) --emit-ir entry returns lowered IR text.
    const emitIr = M.cwrap('zer_emit_ir', 'string', ['string', 'string']);
    const ir = JSON.parse(emitIr(ok, 'ok.zer'));
    console.log('EMIT_IR ok=', ir.ok, ' len=', ir.ok ? ir.ir.length : 0);

    // 8) zer_set_target is callable and does not break emit (width plumb).
    const setTarget = M.cwrap('zer_set_target', null, ['number', 'number', 'number', 'number', 'number']);
    setTarget(32, 1, (1 << 1) | (1 << 2), 0, 0);
    const e32 = JSON.parse(emitC(ok, 'ok.zer', 0));
    setTarget(64, 1, (1 << 1) | (1 << 2), 0, 0); // restore desktop default
    console.log('EMIT after set_target(bits=32) ok=', e32.ok);

    // Assertions
    const fail = (m) => { console.error('SMOKE FAIL:', m); process.exit(1); };
    if (e2.ok) fail('zercheck_ir did NOT reject double-free — safety analyzer not wired into compile path!');
    if (!ir.ok || ir.ir.length < 10) fail('zer_emit_ir produced no IR');
    if (!e32.ok) fail('emit after zer_set_target(32) failed');
    if (d1 !== '[]') fail('clean program produced diagnostics');
    if (!d2.includes('argument') && !d2.includes('expected')) fail('error program lacked diagnostic');
    if (d3 !== '[]') fail('variadic program produced diagnostics');
    if (!e0.ok || e0.c.length < 50) fail('emit C (track=0) failed');
    if (!e0.c.includes('main')) fail('emitted C missing main');
    if (e0.c.includes('__wrap_malloc')) fail('track=0 must NOT define __wrap_malloc');
    if (!e1.ok) fail('emit C (track=1) failed');
    if (!e1.c.includes('__wrap_malloc')) fail('track=1 MUST define __wrap_malloc (Level 5 interception)');

    console.log('SMOKE OK — wasm frontend works, track_cptrs gates __wrap_malloc correctly');
})();
