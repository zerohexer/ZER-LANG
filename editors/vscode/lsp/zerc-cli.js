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
 * Usage: zerc <input.zer> [-o output] [--run] [--emit-c]
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

// ---- parse args --------------------------------------------------
const argv = process.argv.slice(2);
let input = null;
let output = null;
let run = false;
let emitC = false;

for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '-o' && i + 1 < argv.length) { output = argv[++i]; }
    else if (a === '--run') { run = true; }
    else if (a === '--emit-c') { emitC = true; }
    else if (a === '--version' || a === '-v') { console.log('zerc (wasm) 0.5.0'); process.exit(0); }
    else if (!a.startsWith('-')) { input = a; }
    // unknown flags are ignored (checker flags like --no-strict-mmio are not
    // yet plumbed through the wasm entry — see limitations).
}

if (!input) die('Usage: zerc <input.zer> [-o output] [--run] [--emit-c]');
if (!fs.existsSync(input)) die("zerc: cannot open '" + input + "'");

// ---- locate the bundled GCC --------------------------------------
function findGcc() {
    if (process.platform === 'win32') {
        // lsp/ -> ../bin/win32-x64/gcc/bin/gcc.exe
        const bundled = path.join(__dirname, '..', 'bin', 'win32-x64', 'gcc', 'bin', 'gcc.exe');
        if (fs.existsSync(bundled)) return bundled;
        return 'gcc'; // fall back to PATH
    }
    return 'cc';
}

// ---- emit C via wasm, then compile with gcc ----------------------
(async () => {
    // Capture the wasm module's stderr — zercheck_ir reports safety errors
    // (UAF/double-free/leak) via "file:line: zercheck: msg" to stderr.
    const stderrLines = [];
    const M = await ZerModule({ printErr: (s) => { stderrLines.push(s); } });
    const emit = M.cwrap('zer_emit_c', 'string', ['string', 'string', 'number']);

    // Level 3/4/5 *opaque inline-header tracking + compiled-in --wrap=malloc
    // cross-C-boundary UAF interception. The native driver enables it for --run
    // (and explicit --track-cptrs); match that exactly so the wasm CLI never
    // silently drops the runtime safety backstop that native --run provides.
    const trackCptrs = run || argv.includes('--track-cptrs');

    const src = fs.readFileSync(input, 'utf8');
    const base = path.basename(input);
    let res;
    try {
        res = JSON.parse(emit(src, base, trackCptrs ? 1 : 0));
    } catch (e) {
        die('zerc: internal error parsing compiler output');
    }

    if (!res.ok) {
        for (const d of res.diagnostics || []) {
            const sev = d.severity === 2 ? 'warning' : 'error';
            process.stderr.write(base + ':' + d.line + ': ' + sev + ': ' + d.message + '\n');
        }
        // zercheck_ir safety errors (UAF/double-free/leak) arrive on stderr.
        for (const line of stderrLines) {
            if (line.indexOf('zercheck') !== -1) process.stderr.write(line + '\n');
        }
        die('error: compilation failed');
    }

    // Determine output paths.
    const exeExt = process.platform === 'win32' ? '.exe' : '';
    const stem = input.replace(/\.zer$/i, '');
    let cPath, exePath, keepC;

    if (output && /\.c$/i.test(output)) {
        // -o foo.c  -> emit C only, keep it
        fs.writeFileSync(output, res.c);
        console.log('zerc: ' + input + ' -> ' + output);
        process.exit(0);
    }

    exePath = output || (stem + exeExt);
    cPath = emitC ? (stem + '.c') : path.join(os.tmpdir(), 'zerc_' + process.pid + '.c');
    keepC = emitC;
    fs.writeFileSync(cPath, res.c);

    const gcc = findGcc();
    const gccArgs = ['-std=c99', '-O2', '-fwrapv', '-fno-strict-aliasing'];
    // The --wrap allocator interception is only valid when the emitter actually
    // defined the __wrap_* functions (opaque/malloc tracking on). The native
    // driver gates the linker flags on the same condition; detect it directly
    // in the emitted C so the two can never desync (otherwise the C runtime's
    // own malloc references redirect to an undefined __wrap_malloc -> link fail).
    if (res.c.indexOf('__wrap_malloc') !== -1) {
        gccArgs.push('-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc');
    }
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
