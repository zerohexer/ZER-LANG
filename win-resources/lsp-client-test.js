// Drives the WASM LSP server.js through a real handshake to verify it works.
// Spawns `node server.js`, sends initialize + didOpen (clean + error docs),
// reads framed responses, asserts diagnostics behave correctly.
const { spawn } = require('child_process');
const path = require('path');

const serverPath = process.argv[2] || path.join(__dirname, 'server.js');
const child = spawn(process.execPath, [serverPath], { stdio: ['pipe', 'pipe', 'inherit'] });

function send(msg) {
    const buf = Buffer.from(JSON.stringify(msg), 'utf8');
    child.stdin.write('Content-Length: ' + buf.length + '\r\n\r\n');
    child.stdin.write(buf);
}

// --- collect framed messages from the server ---
let buffer = Buffer.alloc(0);
const received = [];
child.stdout.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk]);
    for (;;) {
        const he = buffer.indexOf('\r\n\r\n');
        if (he < 0) break;
        const m = /Content-Length:\s*(\d+)/i.exec(buffer.slice(0, he).toString('ascii'));
        if (!m) { buffer = buffer.slice(he + 4); continue; }
        const len = parseInt(m[1], 10);
        const start = he + 4;
        if (buffer.length < start + len) break;
        const body = buffer.slice(start, start + len).toString('utf8');
        buffer = buffer.slice(start + len);
        received.push(JSON.parse(body));
    }
});

const fail = (m) => { console.error('LSP TEST FAIL:', m); process.exit(1); };

// Handshake
send({ jsonrpc: '2.0', id: 1, method: 'initialize', params: { capabilities: {} } });
send({ jsonrpc: '2.0', method: 'initialized', params: {} });

// Clean doc -> empty diagnostics
send({ jsonrpc: '2.0', method: 'textDocument/didOpen', params: {
    textDocument: { uri: 'file:///ok.zer', languageId: 'zer', version: 1,
        text: 'i32 main() { i32 x = 5; return 0; }' } } });

// Error doc -> non-empty diagnostics
send({ jsonrpc: '2.0', method: 'textDocument/didOpen', params: {
    textDocument: { uri: 'file:///bad.zer', languageId: 'zer', version: 1,
        text: 'void puts(const *u8 s);\ni32 main() { puts("hi", 1); return 0; }' } } });

// UAF doc -> zercheck_ir safety diagnostic (LSP parity with the compile path)
send({ jsonrpc: '2.0', method: 'textDocument/didOpen', params: {
    textDocument: { uri: 'file:///uaf.zer', languageId: 'zer', version: 1,
        text: 'struct Task { u32 id; }\nPool(Task, 4) pool;\nu32 main() { Handle(Task) h = pool.alloc() orelse return; pool.free(h); pool.free(h); return 0; }' } } });

setTimeout(() => {
    const initResp = received.find((r) => r.id === 1);
    if (!initResp || !initResp.result || !initResp.result.capabilities) fail('no initialize response');
    console.log('initialize OK, capabilities:', JSON.stringify(initResp.result.capabilities.textDocumentSync));

    const okDiag = received.find((r) => r.method === 'textDocument/publishDiagnostics' && r.params.uri === 'file:///ok.zer');
    const badDiag = received.find((r) => r.method === 'textDocument/publishDiagnostics' && r.params.uri === 'file:///bad.zer');
    if (!okDiag) fail('no diagnostics published for ok.zer');
    if (!badDiag) fail('no diagnostics published for bad.zer');

    console.log('ok.zer diagnostics:', JSON.stringify(okDiag.params.diagnostics));
    console.log('bad.zer diagnostics:', JSON.stringify(badDiag.params.diagnostics));

    if (okDiag.params.diagnostics.length !== 0) fail('clean doc should have no diagnostics');
    if (badDiag.params.diagnostics.length === 0) fail('error doc should have diagnostics');
    const msg = badDiag.params.diagnostics[0].message;
    if (!/argument|expected/.test(msg)) fail('unexpected diagnostic message: ' + msg);
    // verify 0-based line conversion (error is on source line 2 -> LSP line 1)
    if (badDiag.params.diagnostics[0].range.start.line !== 1) fail('expected 0-based line 1, got ' + badDiag.params.diagnostics[0].range.start.line);

    // zercheck_ir parity: the editor must surface the double-free.
    const uafDiag = received.find((r) => r.method === 'textDocument/publishDiagnostics' && r.params.uri === 'file:///uaf.zer');
    if (!uafDiag) fail('no diagnostics published for uaf.zer');
    console.log('uaf.zer diagnostics:', JSON.stringify(uafDiag.params.diagnostics));
    const hasSafety = uafDiag.params.diagnostics.some((d) => d.source === 'zercheck' || /free/i.test(d.message));
    if (!hasSafety) fail('LSP did not surface the zercheck_ir double-free diagnostic (parity gap)');

    console.log('LSP TEST OK — diagnostics + zercheck_ir safety parity');
    child.kill();
    process.exit(0);
}, 2500);
