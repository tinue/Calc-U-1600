// "Calc-U-1600: Create Debug Project…": writes a starting point for a new
// program or ROM extension into a workspace folder -- the source, the
// project preset (machine + debug: block) and a .gitignore from
// templates/<target>/, and one launch configuration. Never overwrites a
// file; the launch configuration is added to any that exist.
const vscode = require('vscode');
const fs = require('fs');
const path = require('path');

const TARGETS = [
    { id: 'pc1600-program', label: 'PC-1600 program', detail: 'Z80 code loaded at C0C5H (zasm)', assembler: 'zasm', preset: 'debug.pc1600', name: 'main' },
    { id: 'pc1500a-program', label: 'PC-1500A program', detail: 'LH5801 code loaded at &7C01 (sdaslh5801)', assembler: 'sdas', preset: 'debug.pc1500a', name: 'main' },
    { id: 'pc1600-bus-rom', label: 'PC-1600 ROM extension', detail: 'A ROM module in page 1, bank 6 of the 60-pin bus (zasm)', assembler: 'zasm', preset: 'debug.pc1600', name: 'rom' },
    { id: 'pc1500-bus-rom', label: 'PC-1500 ROM extension', detail: 'A ROM at &8000 on the 60-pin bus (sdaslh5801)', assembler: 'sdas', preset: 'debug.pc1500a', name: 'rom' },
];

async function pickFolder() {
    const folders = vscode.workspace.workspaceFolders || [];
    if (folders.length === 0) {
        vscode.window.showWarningMessage('Open the folder for the project first (File ▸ Open Folder…).');
        return undefined;
    }
    if (folders.length === 1) return folders[0];
    return vscode.window.showWorkspaceFolderPick({ placeHolder: 'The folder for the project' });
}

async function createProject(extensionPath) {
    const folder = await pickFolder();
    if (!folder) return;
    const target = await vscode.window.showQuickPick(TARGETS, { placeHolder: 'What are you going to write?' });
    if (!target) return;
    const name = await vscode.window.showInputBox({
        prompt: 'Name of the source file (without .asm)',
        value: target.name,
        validateInput: v => /^[A-Za-z_][A-Za-z0-9_-]*$/.test(v) ? undefined : 'Letters, digits, _ and -',
    });
    if (!name) return;

    const templateDir = path.join(extensionPath, 'templates', target.id);
    const written = [], kept = [];
    for (const file of fs.readdirSync(templateDir)) {
        const outName = file === 'gitignore' ? '.gitignore' : file.replace('NAME', name);
        const out = path.join(folder.uri.fsPath, outName);
        if (fs.existsSync(out)) {
            kept.push(outName);
            continue;
        }
        fs.writeFileSync(out, fs.readFileSync(path.join(templateDir, file), 'utf8').split('{{name}}').join(name));
        written.push(outName);
    }

    // The launch configuration: the project preset, and the build of this
    // one source. Added next to any that exist (VS Code edits launch.json).
    const launch = vscode.workspace.getConfiguration('launch', folder.uri);
    const configurations = launch.get('configurations') || [];
    const configName = `Debug ${name}`;
    if (!configurations.some(c => c.name === configName)) {
        configurations.push({
            type: 'calcu1600',
            request: 'attach',
            name: configName,
            project: '${workspaceFolder}/' + target.preset,
            build: { assembler: target.assembler, file: '${workspaceFolder}/' + name + '.asm' },
        });
        await launch.update('configurations', configurations, vscode.ConfigurationTarget.WorkspaceFolder);
        if (!launch.get('version')) await launch.update('version', '0.2.0', vscode.ConfigurationTarget.WorkspaceFolder);
        written.push(`.vscode/launch.json ("${configName}")`);
    }

    const source = path.join(folder.uri.fsPath, name + '.asm');
    if (fs.existsSync(source)) await vscode.window.showTextDocument(vscode.Uri.file(source));
    vscode.window.showInformationMessage(
        `Created ${written.join(', ') || 'nothing new'}` + (kept.length ? `; kept the existing ${kept.join(', ')}` : '') +
        `. Press F5 with "${configName}" to debug.`);
}

module.exports = { createProject };
