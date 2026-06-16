#!/usr/bin/env node
'use strict';
/* ================================================================
 * zerc — command-line driver, WASM edition
 *
 * Replaces the native zerc.exe. Loads zer.wasm (the same C compiler
 * sources, compiled to WebAssembly), turns ZER source into C, then
 * invokes the bundled GCC on the emitted C. The only native binary in
 * the toolchain is GCC (w64devkit, widely distributed / reputable) and
 * the OpenJS-signed node.exe that runs this script — neither is the
 * unsigned-mingw profile that trips Defender's Wacatac false positive.
 *
 * Usage: zerc <input.zer> [-o output] [--run] [--emit-c] [--emit-ir]
 *              [--no-strict-mmio] [--stack-limit N] [--target-bits N]
 *              [--target-arch x86_64|aarch64|riscv64]
 *              [--target-features f1,f2,...]
 * ================================================================ */

const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawnSync } = require('child_process');

const ZerModule = require(path.join(__dirname, 'zer.js'));

function die(msg, code) {
    process.stderr.write(msg + '\n');
    process.exit(code === undefined ? 1 : code);
}

// ---- target maps (mirror zerc_main.c) ----------------------------
const ARCH = { x86_64: 1, aarch64: 2, riscv64: 3 };
const FEATURE_BITS = {
    avx512f: 1 << 0, sse: 1 << 1, sse2: 1 << 2, avx: 1 << 3, avx2: 1 << 4,
    aes: 1 << 5, sha: 1 << 6, bmi: 1 << 7, bmi2: 1 << 8, lzcnt: 1 << 9,
    popcnt: 1 << 10, invpcid: 1 << 11, pku: 1 << 12, xsave: 1 << 13, smap: 1 << 14,
};
// GCC -m flags per feature (so the emitted C is compiled with them too).
const FEATURE_GCC = {
    avx512f: '-mavx512f', sse: '-msse', sse2: '-msse2', avx: '-mavx', avx2: '-mavx2',
    aes: '-maes', sha: '-msha', bmi: '-mbmi', bmi2: '-mbmi2', lzcnt: '-mlzcnt',
    popcnt: '-mpopcnt', invpcid: '-minvpcid', pku: '-mpku', xsave: '-mxsave',
};

// ---- parse args --------------------------------------------------
const argv = process.argv.slice(2);
let input = null, output = null;
let run = false, emitC = false, emitIr = false;
let ptrBits = 0;                 // 0 => wasm default (64)
let targetArch = 1;              // x86_64
let userFeatures = 0;
let noStrictMmio = 0, stackLimit = 0;
const gccFeatureFlags = [];

for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-o' && i + 1 < argv.length) { output = argv[++i]; }
    else if (a === '--run') { run = true; }
    else if (a === '--emit-c') { emitC = true; }
    else if (a === '--emit-ir') { emitIr = true; }
    else if (a === '--track-cptrs') { run = run; /* track handled via run||flag below */ }
    else if (a === '--no-strict-mmio') { noStrictMmio = 1; }
    else if (a === '--stack-limit' && i + 1 < argv.length) { stackLimit = parseInt(argv[++i], 10) || 0; }
    else if (a === '--target-bits' && i + 1 < argv.length) { ptrBits = parseInt(argv[++i], 10) || 0; }
    else if (a === '--target-arch' && i + 1 < argv.length) { targetArch = ARCH[argv[++i]] || 1; }
    else if (a === '--target-features' && i + 1 < argv.length) {
        for (const f of argv[++i].split(',').map((s) => s.trim())) {
            if (FEATURE_BITS[f]) userFeatures |= FEATURE_BITS[f];
            if (FEATURE_GCC[f]) gccFeatureFlags.push(FEATURE_GCC[f]);
        }
    }
    else if (a === '--version' || a === '-v') { console.log('zerc (wasm) 0.5.5'); process.exit(0); }
    else if (!a.startsWith('-')) { input = a; }
    // unknown flags ignored.
}
const trackCptrsFlag = argv.includes('--track-cptrs');

if (!input) die('Usage: zerc <input.zer> [-o output] [--run] [--emit-c] [--emit-ir] '
    + '[--no-strict-mmio] [--stack-limit N] [--target-bits N] [--target-arch A] [--target-features F]');
if (!fs.existsSync(input)) die("zerc: cannot open '" + input + "'");

// x86 baseline is SSE|SSE2; non-x86 starts empty (matches zerc_main.c).
const baseFeatures = targetArch === 1 ? (FEATURE_BITS.sse | FEATURE_BITS.sse2) : 0;
const targetFeatures = (baseFeatures | userFeatures) >>> 0;

// ---- locate the bundled GCC --------------------------------------
function findGcc() {
    if (process.platform === 'win32') {
        const bundled = path.join(__dirname, '..', 'bin', 'win32-x64', 'gcc', 'bin', 'gcc.exe');
        if (fs.existsSync(bundled)) return bundled;
        return 'gcc';
    }
    return 'cc';
}

// ---- compile -----------------------------------------------------
(async () => {
    const stderrLines = [];
    const M = await ZerModule({ printErr: (s) => { stderrLines.push(s); } });
    const setTarget = M.cwrap('zer_set_target', null, ['number', 'number', 'number', 'number', 'number']);
    setTarget(ptrBits, targetArch, targetFeatures, noStrictMmio, stackLimit);

    const src = fs.readFileSync(input, 'utf8');
    const base = path.basename(input);

    // --emit-ir: print the lowered IR and exit (debug).
    if (emitIr) {
        const emitIrFn = M.cwrap('zer_emit_ir', 'string', ['string', 'string']);
        let r;
        try { r = JSON.parse(emitIrFn(src, base)); } catch (e) { die('zerc: internal error'); }
        if (!r.ok) {
            for (const d of r.diagnostics || []) {
                process.stderr.write(base + ':' + d.line + ': error: ' + d.message + '\n');
            }
            die('error: compilation failed');
        }
        process.stdout.write(r.ir);
        process.exit(0);
    }

    // Level 3/4/5 *opaque + --wrap=malloc interception: on for --run / --track-cptrs.
    const trackCptrs = run || trackCptrsFlag;
    const emit = M.cwrap('zer_emit_c', 'string', ['string', 'string', 'number']);
    let res;
    try { res = JSON.parse(emit(src, base, trackCptrs ? 1 : 0)); }
    catch (e) { die('zerc: internal error parsing compiler output'); }

    if (!res.ok) {
        for (const d of res.diagnostics || []) {
            const sev = d.severity === 2 ? 'warning' : 'error';
            process.stderr.write(base + ':' + d.line + ': ' + sev + ': ' + d.message + '\n');
        }
        for (const line of stderrLines) {
            if (line.indexOf('zercheck') !== -1) process.stderr.write(line + '\n');
        }
        die('error: compilation failed');
    }

    // Write C; emit-only when -o foo.c.
    const exeExt = process.platform === 'win32' ? '.exe' : '';
    const stem = input.replace(/\.zer$/i, '');
    if (output && /\.c$/i.test(output)) {
        fs.writeFileSync(output, res.c);
        console.log('zerc: ' + input + ' -> ' + output);
        process.exit(0);
    }

    const exePath = output || (stem + exeExt);
    const keepC = emitC;
    const cPath = keepC ? (stem + '.c') : path.join(os.tmpdir(), 'zerc_' + process.pid + '.c');
    fs.writeFileSync(cPath, res.c);

    const gcc = findGcc();
    const gccArgs = ['-std=c99', '-O2', '-fwrapv', '-fno-strict-aliasing'];
    // --wrap is valid only when the emitter defined the __wrap_* functions
    // (track_cptrs). Detect in the emitted C so the two never desync.
    if (res.c.indexOf('__wrap_malloc') !== -1) {
        gccArgs.push('-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc');
    }
    for (const f of gccFeatureFlags) gccArgs.push(f);
    if (process.platform === 'win32') gccArgs.push('-mconsole');
    gccArgs.push('-o', exePath, cPath);

    const cc = spawnSync(gcc, gccArgs, { stdio: 'inherit' });
    if (!keepC) { try { fs.unlinkSync(cPath); } catch (e) {} }
    if (cc.status !== 0) die('zerc: gcc failed', cc.status || 1);
    console.log('zerc: ' + input + ' -> ' + exePath);

    if (run) {
        const r = spawnSync(path.resolve(exePath), [], { stdio: 'inherit' });
        process.exit(r.status === null ? 1 : r.status);
    }
})();
