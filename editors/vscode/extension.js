const { LanguageClient, TransportKind } = require('vscode-languageclient/node');
const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const { execSync } = require('child_process');

let client;

function getPlatformDir() {
    const arch = process.arch === 'arm64' ? 'arm64' : 'x64';
    if (process.platform === 'win32') return 'win32-x64';
    if (process.platform === 'darwin') return `darwin-${arch}`;
    return 'linux-x64';
}

function findBundled(context, name) {
    const platDir = path.join(context.extensionPath, 'bin', getPlatformDir());
    const bundled = path.join(platDir, name);
    if (fs.existsSync(bundled)) return bundled;
    return name; // fallback to system PATH
}

function ensureMacOSBuild(context) {
    if (process.platform !== 'darwin') return;
    const platDir = path.join(context.extensionPath, 'bin', getPlatformDir());
    const zerc = path.join(platDir, 'zerc');
    if (fs.existsSync(zerc)) return; // already built
    const buildScript = path.join(platDir, 'build.sh');
    if (fs.existsSync(buildScript)) {
        try {
            vscode.window.showInformationMessage('ZER: compiling zerc and zer-lsp for macOS (first time only)...');
            execSync(`bash "${buildScript}"`, { timeout: 30000 });
        } catch (e) {
            vscode.window.showErrorMessage('ZER: failed to compile — run build.sh manually in extension bin/ directory');
        }
    }
}

function activate(context) {
    const config = vscode.workspace.getConfiguration('zer');

    // macOS: compile from source on first activation
    ensureMacOSBuild(context);

    // LSP: use bundled zer-lsp if available, else config/PATH
    let lspPath = config.get('lspPath', '');
    if (!lspPath) {
        const lspName = process.platform === 'win32' ? 'zer-lsp.exe' : 'zer-lsp';
        lspPath = findBundled(context, lspName);
    }

    // Check if zerc is on system PATH BEFORE we inject bundled dir
    const platDir = path.join(context.extensionPath, 'bin', getPlatformDir());
    const sep = process.platform === 'win32' ? ';' : ':';
    let zercOnSystemPath = false;
    try { execSync('where zerc', { stdio: 'ignore' }); zercOnSystemPath = true; } catch(e) {}

    // Add bundled bin/ to PATH for zerc and gcc
    if (fs.existsSync(platDir)) {
        process.env.PATH = platDir + sep + process.env.PATH;
    }
    // Windows: add bundled GCC
    if (process.platform === 'win32') {
        const gccDir = path.join(platDir, 'gcc', 'bin');
        if (fs.existsSync(gccDir)) {
            process.env.PATH = gccDir + sep + process.env.PATH;
        }
    }

    // Propagate bundled bin/ to integrated terminal so zerc is available there
    const envVar = context.environmentVariableCollection;
    if (fs.existsSync(platDir)) {
        envVar.prepend('PATH', platDir + sep);
    }
    if (process.platform === 'win32') {
        const gccBinDir = path.join(platDir, 'gcc', 'bin');
        if (fs.existsSync(gccBinDir)) {
            envVar.prepend('PATH', gccBinDir + sep);
        }

        // First-time: offer to add zerc to user PATH permanently
        const addedKey = 'zer.pathAdded';
        const alreadyAdded = context.globalState.get(addedKey);

        if (!alreadyAdded && !zercOnSystemPath) {
            vscode.window.showWarningMessage(
                'ZER: zerc not found on PATH. Add bundled zerc + gcc to your system PATH?',
                'Yes', 'No'
            ).then(choice => {
                if (choice === 'Yes') {
                    try {
                        const psCmd = `[Environment]::SetEnvironmentVariable('Path', '${platDir};${gccBinDir};' + [Environment]::GetEnvironmentVariable('Path', 'User'), 'User')`;
                        execSync(`powershell -Command "${psCmd}"`, { stdio: 'ignore' });
                        context.globalState.update(addedKey, true);
                        vscode.window.showInformationMessage('ZER: Added to PATH. Restart VS Code to use zerc in terminal.');
                    } catch (err) {
                        vscode.window.showErrorMessage('ZER: Failed to update PATH — add manually: ' + platDir);
                    }
                } else {
                    context.globalState.update(addedKey, true);
                }
            });
        }
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

    // Status message
    const zercName = process.platform === 'win32' ? 'zerc.exe' : 'zerc';
    const bundledZerc = path.join(platDir, zercName);
    if (fs.existsSync(bundledZerc)) {
        vscode.window.setStatusBarMessage('ZER: ready (bundled compiler)', 5000);
    }
}

function deactivate() {
    if (client) {
        return client.stop();
    }
}

module.exports = { activate, deactivate };
