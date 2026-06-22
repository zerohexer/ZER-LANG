// ================================================================
// wasi-run.mjs — run a compiled ZER program (.wasm) in node's WASI.
//
// Invoked as:  node --experimental-wasi-unstable-preview1 wasi-run.mjs <prog.wasm> [args...]
//
// The program is a standard wasm32-wasi command module (printf -> fd_write,
// exit code via proc_exit). stdio is wired to the host terminal; the current
// working directory is preopened as "/" so relative file I/O works. Returns
// the program's exit code as this process's exit code.
// ================================================================
import { readFileSync } from 'node:fs';
import { WASI } from 'node:wasi';
import process from 'node:process';

const wasmPath = process.argv[2];
if (!wasmPath) {
    process.stderr.write('wasi-run: no .wasm path given\n');
    process.exit(1);
}
const progArgs = process.argv.slice(3);

const wasi = new WASI({
    version: 'preview1',
    returnOnExit: true,
    args: [wasmPath, ...progArgs],
    env: process.env,
    preopens: { '/': process.cwd() },
});

try {
    const bytes = readFileSync(wasmPath);
    const module = await WebAssembly.compile(bytes);
    const instance = await WebAssembly.instantiate(module, wasi.getImportObject());
    process.exit(wasi.start(instance));
} catch (e) {
    // A wasm trap (bounds check / __builtin_trap / unreachable) surfaces here.
    process.stderr.write('ZER: program trapped: ' + (e && e.message ? e.message : e) + '\n');
    process.exit(134); // SIGABRT-ish
}
