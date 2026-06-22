#!/usr/bin/env node
'use strict';
/* ================================================================
 * ZER Language Server — WASM edition
 *
 * A dependency-free JSON-RPC 2.0 LSP server. All compiler work
 * (lexer -> parser -> checker -> zercheck) is done by zer.wasm —
 * the SAME C compiler sources as the native build, just compiled to
 * WebAssembly. node (the host) is Microsoft-signed and trusted, and
 * zer.wasm is a data file node loads, so there is no unsigned native
 * binary in the spawn path for Windows Defender to flag.
 *
 * Mirrors the protocol surface of the former zer_lsp.c (diagnostics on
 * open/change/close). Communicates over stdin/stdout.
 * ================================================================ */

const path = require('path');

// emscripten MODULARIZE factory (EXPORT_NAME=ZerModule), wasm sits beside it.
const ZerModule = require(path.join(__dirname, 'zer.js'));

let M = null;          // instantiated wasm module
let diagnose = null;   // cwrapped zer_diagnostics_json(src, fname) -> json string
const pending = [];    // messages received before wasm finished loading
const stderrBuf = [];  // captured wasm stderr (zercheck_ir safety diagnostics)

// ---- document store: uri -> text ----------------------------------
const docs = new Map();

// ---- stdout: write a framed JSON-RPC message ----------------------
function send(msg) {
    const buf = Buffer.from(JSON.stringify(msg), 'utf8');
    process.stdout.write('Content-Length: ' + buf.length + '\r\n\r\n');
    process.stdout.write(buf);
}

// ---- run diagnostics through wasm and publish ---------------------
function publishDiagnostics(uri) {
    const text = docs.get(uri);
    if (text == null || !diagnose) return;

    stderrBuf.length = 0; // capture only this run's wasm stderr
    let arr = [];
    try {
        arr = JSON.parse(diagnose(text, 'input.zer'));
    } catch (e) {
        arr = [];
    }

    const mkRange = (ln1) => {
        const line = ln1 > 0 ? ln1 - 1 : 0; // compiler 1-based -> LSP 0-based
        return { start: { line, character: 0 }, end: { line, character: 999 } };
    };

    const diagnostics = arr.map((d) => ({
        range: mkRange(d.line),
        severity: d.severity || 1,
        source: 'zerc',
        message: d.message || '',
    }));

    // Merge zercheck_ir safety errors — printed to stderr as
    // "file:line: zercheck: msg" by the wasm during zer_diagnostics_json — so
    // the editor surfaces exactly what `zerc` would reject.
    for (const line of stderrBuf) {
        const m = /:(\d+):\s*zercheck:\s*(.+)$/.exec(line);
        if (m) {
            diagnostics.push({
                range: mkRange(parseInt(m[1], 10)),
                severity: 1,
                source: 'zercheck',
                message: m[2].trim(),
            });
        }
    }

    send({
        jsonrpc: '2.0',
        method: 'textDocument/publishDiagnostics',
        params: { uri, diagnostics },
    });
}

// ---- request/notification dispatch --------------------------------
function handle(msg) {
    const { id, method, params } = msg;
    switch (method) {
        case 'initialize':
            send({
                jsonrpc: '2.0',
                id,
                result: {
                    capabilities: {
                        textDocumentSync: { openClose: true, change: 1 /* full */ },
                    },
                    serverInfo: { name: 'zer-lsp-wasm', version: '0.5.0' },
                },
            });
            break;

        case 'initialized':
            break; // notification — nothing to do

        case 'shutdown':
            send({ jsonrpc: '2.0', id, result: null });
            break;

        case 'exit':
            process.exit(0);
            break;

        case 'textDocument/didOpen': {
            const td = params && params.textDocument;
            if (td) {
                docs.set(td.uri, td.text || '');
                publishDiagnostics(td.uri);
            }
            break;
        }

        case 'textDocument/didChange': {
            const td = params && params.textDocument;
            const changes = params && params.contentChanges;
            if (td && changes && changes.length) {
                // Full-sync: last change carries the whole document.
                docs.set(td.uri, changes[changes.length - 1].text || '');
                publishDiagnostics(td.uri);
            }
            break;
        }

        case 'textDocument/didClose': {
            const td = params && params.textDocument;
            if (td) {
                docs.delete(td.uri);
                send({
                    jsonrpc: '2.0',
                    method: 'textDocument/publishDiagnostics',
                    params: { uri: td.uri, diagnostics: [] },
                });
            }
            break;
        }

        default:
            // Respond to unknown *requests* (those with an id) so the client
            // doesn't hang; ignore unknown notifications.
            if (id !== undefined && id !== null) {
                send({ jsonrpc: '2.0', id, error: { code: -32601, message: 'Method not found' } });
            }
    }
}

// ---- stdin: parse Content-Length framed messages ------------------
let buffer = Buffer.alloc(0);
process.stdin.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    for (;;) {
        const headerEnd = buffer.indexOf('\r\n\r\n');
        if (headerEnd < 0) break;
        const header = buffer.slice(0, headerEnd).toString('ascii');
        const m = /Content-Length:\s*(\d+)/i.exec(header);
        if (!m) {
            buffer = buffer.slice(headerEnd + 4);
            continue;
        }
        const len = parseInt(m[1], 10);
        const bodyStart = headerEnd + 4;
        if (buffer.length < bodyStart + len) break; // wait for the rest
        const body = buffer.slice(bodyStart, bodyStart + len).toString('utf8');
        buffer = buffer.slice(bodyStart + len);

        let parsed;
        try {
            parsed = JSON.parse(body);
        } catch (e) {
            continue;
        }
        if (M) handle(parsed);
        else pending.push(parsed);
    }
});

process.stdin.on('end', () => process.exit(0));

// ---- load the wasm compiler, then drain queued messages -----------
ZerModule({ printErr: (s) => stderrBuf.push(s) }).then((mod) => {
    M = mod;
    diagnose = M.cwrap('zer_diagnostics_json', 'string', ['string', 'string']);
    // Desktop target defaults (64-bit usize, x86_64, SSE|SSE2) — matches the
    // bundled gcc; keeps the checker's pointer-width/asm checks correct.
    const setTarget = M.cwrap('zer_set_target', null, ['number', 'number', 'number', 'number', 'number']);
    setTarget(64, 1, (1 << 1) | (1 << 2), 0, 0);
    for (const msg of pending) handle(msg);
    pending.length = 0;
}).catch((err) => {
    // If the wasm fails to load, surface it to stderr (the client's log).
    process.stderr.write('zer-lsp-wasm: failed to load zer.wasm: ' + (err && err.stack || err) + '\n');
});
