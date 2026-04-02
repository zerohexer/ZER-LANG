const { LanguageClient, TransportKind } = require('vscode-languageclient/node');
const vscode = require('vscode');
const path = require('path');
const fs = require('fs');

let client;

function findBundled(context, name) {
    // Check bundled bin/ directory inside extension
    const bundled = path.join(context.extensionPath, 'bin', name);
    if (fs.existsSync(bundled)) return bundled;
    // Fallback to system PATH
    return name;
}

function activate(context) {
    const config = vscode.workspace.getConfiguration('zer');

    // LSP: use bundled zer-lsp.exe if available, else config/PATH
    let lspPath = config.get('lspPath', '');
    if (!lspPath) {
        lspPath = findBundled(context, process.platform === 'win32' ? 'zer-lsp.exe' : 'zer-lsp');
    }

    // Add bundled bin/ and bin/gcc/ to PATH for zerc --run and terminal use
    const binDir = path.join(context.extensionPath, 'bin');
    const gccDir = path.join(context.extensionPath, 'bin', 'gcc', 'bin');
    const sep = process.platform === 'win32' ? ';' : ':';
    if (fs.existsSync(binDir)) {
        process.env.PATH = binDir + sep + process.env.PATH;
    }
    if (fs.existsSync(gccDir)) {
        process.env.PATH = gccDir + sep + process.env.PATH;
    }

    const args = config.get('lspArgs', []);

    const serverOptions = {
        command: lspPath,
        args: args,
        transport: TransportKind.stdio
    };

    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'zer' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.zer')
        }
    };

    client = new LanguageClient(
        'zer-lsp',
        'ZER Language Server',
        serverOptions,
        clientOptions
    );

    client.start();
    context.subscriptions.push(client);

    // Show notification on first activation
    const bundledZerc = path.join(binDir, process.platform === 'win32' ? 'zerc.exe' : 'zerc');
    if (fs.existsSync(bundledZerc)) {
        vscode.window.setStatusBarMessage('ZER Language: ready (bundled compiler)', 5000);
    }
}

function deactivate() {
    if (client) {
        return client.stop();
    }
}

module.exports = { activate, deactivate };
