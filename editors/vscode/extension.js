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

    // --- PATH resolution: check what `where zerc` resolves to BEFORE we inject ---
    const platDir = path.join(context.extensionPath, 'bin', getPlatformDir());
    const sep = process.platform === 'win32' ? ';' : ':';
    const exeExt = process.platform === 'win32' ? '.exe' : '';
    const bundledZercPath = path.join(platDir, 'zerc' + exeExt);
    const gccBinDir = process.platform === 'win32' ? path.join(platDir, 'gcc', 'bin') : null;

    const pathsEqual = (a, b) =>
        a && b && path.normalize(a).toLowerCase() === path.normalize(b).toLowerCase();

    // What does `where zerc` resolve to on the user's actual PATH?
    let resolvedZercPath = null;
    try {
        const cmd = process.platform === 'win32' ? 'where zerc' : 'which zerc';
        resolvedZercPath = execSync(cmd, { encoding: 'utf-8' }).trim().split(/\r?\n/)[0] || null;
    } catch (e) { /* zerc not on PATH at all */ }

    // "PATH is fine" = the resolved zerc IS our bundled one for this version.
    // If resolved is from a different extension version (or doesn't resolve), we need update.
    const pathIsCurrent = pathsEqual(resolvedZercPath, bundledZercPath);

    // Add bundled bin/ to PATH for zerc and gcc (process + integrated terminal)
    if (fs.existsSync(platDir)) {
        process.env.PATH = platDir + sep + process.env.PATH;
    }
    if (process.platform === 'win32' && gccBinDir && fs.existsSync(gccBinDir)) {
        process.env.PATH = gccBinDir + sep + process.env.PATH;
    }

    // Propagate bundled bin/ to integrated terminal so zerc is available there
    const envVar = context.environmentVariableCollection;
    if (fs.existsSync(platDir)) {
        envVar.prepend('PATH', platDir + sep);
    }
    if (process.platform === 'win32' && gccBinDir && fs.existsSync(gccBinDir)) {
        envVar.prepend('PATH', gccBinDir + sep);
    }

    // Windows: offer to install to user PATH.
    // Per-version flag — prompts once per extension version. Reinstalls of the
    // same version don't re-prompt; upgrades prompt once, so stale paths from
    // old versions get replaced. Clicking "No" is remembered for that version.
    if (process.platform === 'win32' && !pathIsCurrent) {
        const version = context.extension.packageJSON.version;
        const versionKey = `zer.pathHandled.${version}`;
        const handled = context.globalState.get(versionKey);

        if (!handled) {
            const msg = resolvedZercPath
                ? `ZER: your PATH zerc points to a different version:\n${resolvedZercPath}\nUpdate to this extension's bundled zerc?`
                : 'ZER: zerc not on PATH. Add bundled zerc + gcc to your user PATH?';
            vscode.window.showWarningMessage(msg, 'Yes', 'No').then(choice => {
                if (choice === 'Yes') {
                    try {
                        // Read current user PATH, strip dead/stale zerc-language entries,
                        // then prepend this version's platDir + gccBinDir.
                        const readCmd = `powershell -NoProfile -Command "[Environment]::GetEnvironmentVariable('Path', 'User')"`;
                        const currentPath = execSync(readCmd, { encoding: 'utf-8' }).trim();
                        const entries = currentPath.split(';').filter(e => {
                            if (!e) return false;
                            // Remove any zerc-language entry (we're replacing with current version).
                            // Also auto-cleans entries pointing to uninstalled extension dirs.
                            if (e.toLowerCase().includes('zerc-language')) return false;
                            return true;
                        });
                        const newEntries = [platDir, gccBinDir, ...entries].join(';');
                        // Write via base64 to avoid quoting issues
                        const b64 = Buffer.from(
                            `[Environment]::SetEnvironmentVariable('Path', @'\n${newEntries}\n'@, 'User')`,
                            'utf-16le'
                        ).toString('base64');
                        execSync(`powershell -NoProfile -EncodedCommand ${b64}`, { stdio: 'ignore' });
                        context.globalState.update(versionKey, true);
                        vscode.window.showInformationMessage(
                            'ZER: PATH updated. Restart terminal (and VS Code) for the change to apply.'
                        );
                    } catch (err) {
                        vscode.window.showErrorMessage(
                            'ZER: Failed to update PATH. Add manually: ' + platDir
                        );
                    }
                } else {
                    // Remember "No" for this version — don't nag again unless upgraded.
                    context.globalState.update(versionKey, true);
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

    // Command: open language reference as markdown preview
    const refCmd = vscode.commands.registerCommand('zer.openReference', () => {
        const refPath = path.join(context.extensionPath, 'REFERENCE.md');
        if (fs.existsSync(refPath)) {
            const uri = vscode.Uri.file(refPath);
            vscode.commands.executeCommand('markdown.showPreview', uri);
        } else {
            vscode.window.showErrorMessage('ZER: REFERENCE.md not found in extension');
        }
    });
    context.subscriptions.push(refCmd);

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
