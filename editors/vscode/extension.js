const { LanguageClient, TransportKind } = require('vscode-languageclient/node');
const vscode = require('vscode');

let client;

function activate(context) {
    const config = vscode.workspace.getConfiguration('zer');
    const command = config.get('lspPath', 'zer-lsp');
    const args = config.get('lspArgs', []);

    const serverOptions = {
        command: command,
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
}

function deactivate() {
    if (client) {
        return client.stop();
    }
}

module.exports = { activate, deactivate };
