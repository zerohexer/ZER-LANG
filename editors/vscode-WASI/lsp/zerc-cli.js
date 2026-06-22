#!/usr/bin/env node
'use strict';
/* ================================================================
 * zerc — command-line driver, WASI edition (fully-WASM toolchain)
 *
 * Loads zer.wasm (the ZER->C compiler) to turn ZER source into C, then
 * compiles that C to a WebAssembly module with the bundled wasi-sdk
 * clang (`--target=wasm32-wasi`) and RUNS it in node's built-in WASI.
 *
 * The compiled program is a `.wasm`, NEVER a native `.exe` — so no
 * freshly-built unsigned binary is ever created on the user's machine,
 * which eliminates the per-run Defender scan. The only native executable
 * is the OpenJS-signed node.exe hosting this script, plus the bundled,
 * reputable wasi-sdk clang used once per compile.
 *
 * The Level-4 malloc interception (`-Wl,--wrap=malloc,...`) is preserved:
 * wasm-ld supports --wrap (LLVM 9+, review D62380) with the same
 * __wrap_/__real_ semantics as GNU ld. The emitter's preamble is
 * __wasi__-gated so the emitted C is wasm32-wasi-portable.
 *
 * Usage: zerc <input.zer> [-o output] [--run] [--emit-c] [--emit-ir]
 *              [--no-strict-mmio] [--stack-limit N] [--target-bits N]
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

// ---- target maps (frontend / zer.wasm analysis only) -------------
const ARCH = { x86_64: 1, aarch64: 2, riscv64: 3 };
const FEATURE_BITS = {
    avx512f: 1 << 0, sse: 1 << 1, sse2: 1 << 2, avx: 1 << 3, avx2: 1 << 4,
    aes: 1 << 5, sha: 1 << 6, bmi: 1 << 7, bmi2: 1 << 8, lzcnt: 1 << 9,
    popcnt: 1 << 10, invpcid: 1 << 11, pku: 1 << 12, xsave: 1 << 13, smap: 1 << 14,
};

// ---- parse args --------------------------------------------------
const argv = process.argv.slice(2);
let input = null, output = null;
let run = false, emitC = false, emitIr = false;
let ptrBits = 0;                 // 0 => wasm default (64); wasm32 target is 32-bit, see below
let targetArch = 1;              // x86_64 (frontend analysis default)
let userFeatures = 0;
let noStrictMmio = 0, stackLimit = 0;

for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-o' && i + 1 < argv.length) { output = argv[++i]; }
    else if (a === '--run') { run = true; }
    else if (a === '--emit-c') { emitC = true; }
    else if (a === '--emit-ir') { emitIr = true; }
    else if (a === '--no-strict-mmio') { noStrictMmio = 1; }
    else if (a === '--stack-limit' && i + 1 < argv.length) { stackLimit = parseInt(argv[++i], 10) || 0; }
    else if (a === '--target-bits' && i + 1 < argv.length) { ptrBits = parseInt(argv[++i], 10) || 0; }
    else if (a === '--target-arch' && i + 1 < argv.length) { targetArch = ARCH[argv[++i]] || 1; }
    else if (a === '--target-features' && i + 1 < argv.length) {
        for (const f of argv[++i].split(',').map((s) => s.trim())) {
            if (FEATURE_BITS[f]) userFeatures |= FEATURE_BITS[f];
        }
    }
    else if (a === '--version' || a === '-v') { console.log('zerc (wasm/wasi) 0.5.7'); process.exit(0); }
    else if (!a.startsWith('-')) { input = a; }
    // unknown flags ignored.
}
const trackCptrsFlag = argv.includes('--track-cptrs');

if (!input) die('Usage: zerc <input.zer> [-o output] [--run] [--emit-c] [--emit-ir] '
    + '[--no-strict-mmio] [--stack-limit N] [--target-bits N]');
if (!fs.existsSync(input)) die("zerc: cannot open '" + input + "'");

// wasm32-wasi is a 32-bit-pointer target; tell the frontend so @size(usize),
// MMIO bounds etc. are computed for the actual run target. (Overridable.)
if (!ptrBits) ptrBits = 32;
// x86 baseline is SSE|SSE2 for the frontend's analysis; harmless for wasm output.
const baseFeatures = targetArch === 1 ? (FEATURE_BITS.sse | FEATURE_BITS.sse2) : 0;
const targetFeatures = (baseFeatures | userFeatures) >>> 0;

// ---- locate the bundled wasi-sdk clang ---------------------------
function findClang() {
    const plat = process.platform;
    const exe = plat === 'win32' ? 'clang.exe' : 'clang';
    const dir = plat === 'win32' ? 'win32-x64'
        : (plat === 'darwin' ? 'darwin-x64' : 'linux-x64');
    const bundled = path.join(__dirname, '..', 'bin', dir, 'wasi-sdk', 'bin', exe);
    if (fs.existsSync(bundled)) return bundled;
    return 'clang'; // fall back to a system clang (must support --target=wasm32-wasi)
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

    const stem = input.replace(/\.zer$/i, '');

    // -o foo.c => emit C only (same as native zerc).
    if (output && /\.c$/i.test(output)) {
        fs.writeFileSync(output, res.c);
        console.log('zerc: ' + input + ' -> ' + output);
        process.exit(0);
    }

    // Output is ALWAYS a .wasm module — never a native exe.
    let wasmPath;
    const isTempWasm = run && !output;
    if (output) {
        wasmPath = /\.wasm$/i.test(output) ? output : (output + '.wasm');
    } else if (run) {
        wasmPath = path.join(os.tmpdir(), 'zerc_' + process.pid + '.wasm');
    } else {
        wasmPath = stem + '.wasm';
    }

    const keepC = emitC;
    const cPath = keepC ? (stem + '.c') : path.join(os.tmpdir(), 'zerc_' + process.pid + '.c');
    fs.writeFileSync(cPath, res.c);

    const clang = findClang();
    const ccArgs = ['--target=wasm32-wasi', '-O2', '-fwrapv', '-fno-strict-aliasing'];
    // --wrap is valid only when the emitter defined the __wrap_* functions
    // (track_cptrs). wasm-ld honours --wrap (LLVM 9+). Detect in the emitted C
    // so flags + preamble never desync.
    if (res.c.indexOf('__wrap_malloc') !== -1) {
        ccArgs.push('-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc');
    }
    ccArgs.push('-o', wasmPath, cPath);

    const cc = spawnSync(clang, ccArgs, { stdio: 'inherit' });
    if (!keepC) { try { fs.unlinkSync(cPath); } catch (e) {} }
    if (cc.status !== 0) die('zerc: clang (wasm32-wasi) failed', cc.status || 1);
    console.log('zerc: ' + input + ' -> ' + wasmPath + '  (wasm32-wasi)');

    if (run) {
        // Run the .wasm in node's built-in WASI (needs the experimental flag).
        const runner = path.join(__dirname, 'wasi-run.mjs');
        const r = spawnSync(process.execPath,
            ['--experimental-wasi-unstable-preview1', runner, wasmPath],
            { stdio: 'inherit' });
        if (isTempWasm) { try { fs.unlinkSync(wasmPath); } catch (e) {} }
        process.exit(r.status === null ? 1 : r.status);
    }
})();
