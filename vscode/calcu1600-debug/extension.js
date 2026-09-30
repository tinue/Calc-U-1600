// Calc-U-1600 debugger glue for VS Code. The debug adapter itself runs
// inside the emulator (a DAP server on 127.0.0.1); this extension connects
// to it and adds what makes it usable in any folder without copied files:
// ready-made configurations (debug the current file, reset and stop), the
// assembler builds, the Build & Load / reset commands, user settings for the
// tool paths and ROM listings, and "Create Debug Project…" (scaffold.js).
const vscode = require('vscode');
const fs = require('fs');
const os = require('os');
const path = require('path');
const scaffold = require('./scaffold');

// What "Debug current file" needs per machine: its assembler, the listing
// that assembler writes, the presets that fit, the bundled default preset.
const MACHINES = {
    pc1600: { label: 'PC-1600', assembler: 'zasm', listing: '.lst', presets: ['.pc1600'], preset: 'debug-pc1600.pc1600' },
    pc1500a: { label: 'PC-1500A', assembler: 'sdas', listing: '.rst', presets: ['.pc1500a', '.pc1500'], preset: 'debug-pc1500a.pc1500a' },
};

let extensionPath = '';

function setting(key) {
    return vscode.workspace.getConfiguration('calcu1600').get(key);
}

function expandHome(p) {
    return p === '~' || p.startsWith('~/') ? path.join(os.homedir(), p.slice(1)) : p;
}

// The assemblers assembleCommand() knows.
const ASSEMBLERS = ['zasm', 'sdas'];

// The assembler tools: the setting, else the environment variable, else
// found on the PATH.
function zasmPath() {
    return expandHome(setting('zasmPath') || process.env.CALCU_ZASM || 'zasm');
}

// sdaslh5801, sdld and makebin, in the configured folder or on the PATH.
function sdccTool(name) {
    const dir = setting('sdccBinPath') || process.env.CALCU_SDCC_BIN;
    return dir ? path.join(expandHome(dir), name) : name;
}

// ── Building ────────────────────────────────────────────────────────────

const shellQuote = s => `'${String(s).replace(/'/g, `'\\''`)}'`;

// The shell command that assembles `file` (in its own folder) into
// <name>.bin plus the listing. `file`, `name` may be ${…} variables; VS
// Code substitutes them before the shell runs. Paths are absolute so the
// problem matchers find the file whatever the task's folder.
function assembleCommand(assembler, file, name) {
    const q = shellQuote;
    if (assembler === 'zasm') {
        // The zasm matcher takes the file from the "in file x.asm:" line
        // before each error. zasm names files relative to the source's
        // folder (the task's cwd), so they are made absolute here; the echo
        // covers a zasm that prints no header for the main file.
        return `set -o pipefail; echo ${q(`in file ${file}:`)}; ${q(zasmPath())} -uwy ${q(file)} ${q(name + '.lst')} ${q(name + '.bin')} 2>&1` +
            ` | sed "s|^in file \\([^/].*\\):\$|in file $PWD/\\1:|"`;
    }
    // sdaslh5801 -> sdld (writes the .rst with linked addresses) -> makebin;
    // the .org of the source sets the address, -s covers code up to FFFFH.
    return `${q(sdccTool('sdaslh5801'))} -plosgff ${q(file)}` +
        ` && printf -- '-muwx\\n-i %s\\n%s.rel\\n\\n-e\\n' ${q(name)} ${q(name)} > ${q(name + '.lnk')}` +
        ` && ${q(sdccTool('sdld'))} -nf ${q(name)}` +
        ` && ${q(sdccTool('makebin'))} -p -s 65536 -o 0x$(head -1 ${q(name + '.ihx')} | cut -c4-7) ${q(name + '.ihx')} ${q(name + '.bin')}`;
}

// A build task for one source file, or (file undefined) for the file in
// the editor.
function assembleTask(assembler, file, scope) {
    const current = !file;
    const execution = new vscode.ShellExecution(
        assembleCommand(assembler, current ? '${file}' : file,
                        current ? '${fileBasenameNoExtension}' : path.basename(file, path.extname(file))),
        { cwd: current ? '${fileDirname}' : path.dirname(file) });
    const definition = current ? { type: 'calcu1600', assembler } : { type: 'calcu1600', assembler, file };
    const task = new vscode.Task(definition, scope || vscode.TaskScope.Workspace,
                                 current ? `${assembler}: build current file` : `${assembler}: ${path.basename(file)}`,
                                 'calcu1600', execution, [`$calcu1600-${assembler}`]);
    task.group = vscode.TaskGroup.Build;
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Silent, clear: true };
    return task;
}

// Runs a task and resolves when it has finished; rejects on a non-zero exit.
async function runTask(task) {
    const execution = await vscode.tasks.executeTask(task);
    return new Promise((resolve, reject) => {
        const done = vscode.tasks.onDidEndTaskProcess(e => {
            if (e.execution !== execution) return;
            done.dispose();
            if (e.exitCode === 0) resolve();
            else reject(new Error(`"${task.name}" failed (exit ${e.exitCode}); see the Problems view`));
        });
    });
}

// A configuration's build: `build` ({assembler, file}), or a named task
// (`buildTask`, from tasks.json or this extension). Nothing to do without.
async function build(config) {
    if (config.build) {
        const { assembler, file } = config.build;
        if (!ASSEMBLERS.includes(assembler)) throw new Error(`"build.assembler" is ${ASSEMBLERS.map(a => `"${a}"`).join(' or ')}, not "${assembler}"`);
        const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file));
        return runTask(assembleTask(assembler, file, folder));
    }
    if (config.buildTask) {
        const task = (await vscode.tasks.fetchTasks()).find(t => t.name === config.buildTask);
        if (!task) throw new Error(`No task named "${config.buildTask}"`);
        return runTask(task);
    }
}

// ── Configurations ──────────────────────────────────────────────────────

function dynamicConfigurations() {
    return [
        { type: 'calcu1600', request: 'attach', name: 'Calc-U-1600: Debug current file on PC-1600 (zasm)', currentFile: 'pc1600' },
        { type: 'calcu1600', request: 'attach', name: 'Calc-U-1600: Debug current file on PC-1500A (sdas)', currentFile: 'pc1500a' },
        { type: 'calcu1600', request: 'attach', name: 'Calc-U-1600: Reset and stop', reset: 'reset', stopOnEntry: true },
    ];
}

function exists(p) {
    try { return fs.statSync(p).isFile(); } catch { return false; }
}

// The preset for "Debug current file": <name>.pc1600 next to the file,
// else debug.pc1600 in the workspace folder, else the bundled one.
function presetFor(file, machine) {
    const stem = file.slice(0, file.length - path.extname(file).length);
    for (const ext of machine.presets) if (exists(stem + ext)) return stem + ext;
    const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file));
    if (folder)
        for (const ext of machine.presets) {
            const p = path.join(folder.uri.fsPath, 'debug' + ext);
            if (exists(p)) return p;
        }
    return path.join(extensionPath, 'presets', machine.preset);
}

// Fills in a "Debug current file" configuration from the file in the editor.
function currentFileConfiguration(config) {
    const machine = MACHINES[config.currentFile];
    if (!machine) throw new Error(`"currentFile" is "pc1600" or "pc1500a", not "${config.currentFile}"`);
    const doc = vscode.window.activeTextEditor && vscode.window.activeTextEditor.document;
    if (!doc || doc.uri.scheme !== 'file' || !/\.(asm|s|z80|a80)$/i.test(doc.fileName))
        throw new Error('Open the .asm file to debug in the editor first.');
    const file = doc.fileName;
    const stem = file.slice(0, file.length - path.extname(file).length);
    return {
        ...config,
        build: { assembler: machine.assembler, file },
        preset: config.preset || presetFor(file, machine),
        program: { bin: stem + '.bin', listing: stem + machine.listing, after: 'stopOnEntry', ...(config.program || {}) },
    };
}

// The user's ROM listings and symbol tables join every session's own.
function withRomListings(config) {
    const listings = setting('romListings') || [];
    const symbols = setting('romSymbols') || [];
    return {
        ...config,
        listings: [...(config.listings || []), ...listings.map(expandEntry)],
        symbols: [...(config.symbols || []), ...symbols.map(expandEntry)],
    };
}

function expandEntry(e) {
    return typeof e === 'string' ? expandHome(e) : { ...e, path: expandHome(e.path || '') };
}

const resolver = {
    async resolveDebugConfiguration(folder, config) {
        // F5 without a launch.json: offer the ready-made configurations.
        if (!config.type && !config.request && !config.name) {
            const pick = await vscode.window.showQuickPick(
                dynamicConfigurations().map(c => ({ label: c.name.replace('Calc-U-1600: ', ''), config: c })),
                { placeHolder: 'Calc-U-1600: what to debug' });
            if (!pick) return undefined;
            config = pick.config;
        }
        if (!config.currentFile) return config;
        try {
            return currentFileConfiguration(config);
        } catch (err) {
            vscode.window.showErrorMessage(err.message);
            return undefined;
        }
    },
    async resolveDebugConfigurationWithSubstitutedVariables(folder, config) {
        if (config.port === undefined) config.port = setting('port');
        config = withRomListings(config);
        try {
            await build(config);
        } catch (err) {
            vscode.window.showErrorMessage(`Build: ${err.message || err}`);
            return undefined;
        }
        return config;
    },
};

// ── Commands ────────────────────────────────────────────────────────────

function activeCalcuSession() {
    const s = vscode.debug.activeDebugSession;
    return s && s.type === 'calcu1600' ? s : undefined;
}

async function buildAndLoad() {
    const session = activeCalcuSession();
    if (!session) {
        vscode.window.showWarningMessage('Build & Load needs a running Calc-U-1600 debug session.');
        return;
    }
    try {
        await build(session.configuration);
        // The app loads the configuration's program (maybe from the project
        // preset's debug: block), or without one sets the machine up again
        // (a ROM extension).
        const result = await session.customRequest('calcu1600/load', {});
        vscode.window.setStatusBarMessage(result && result.start ? `Loaded ${result.start}-${result.end}` : 'Reloaded', 4000);
    } catch (err) {
        vscode.window.showErrorMessage(`Build & Load: ${err.message || err}`);
    }
}

function reset(kind) {
    const session = activeCalcuSession();
    if (!session) return;
    session.customRequest('calcu1600/reset', { kind, stop: true })
        .then(undefined, err => vscode.window.showErrorMessage(`Reset: ${err.message || err}`));
}

function activate(context) {
    extensionPath = context.extensionPath;
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('calcu1600', {
            createDebugAdapterDescriptor(session) {
                return new vscode.DebugAdapterServer(session.configuration.port || setting('port') || 32168, '127.0.0.1');
            }
        }),
        vscode.debug.registerDebugConfigurationProvider('calcu1600', resolver),
        vscode.debug.registerDebugConfigurationProvider('calcu1600', { provideDebugConfigurations: dynamicConfigurations },
                                                        vscode.DebugConfigurationProviderTriggerKind.Dynamic),
        vscode.tasks.registerTaskProvider('calcu1600', {
            provideTasks: () => ASSEMBLERS.map(a => assembleTask(a)),
            resolveTask(task) {
                const { assembler, file } = task.definition;
                if (!ASSEMBLERS.includes(assembler)) return undefined;
                const t = assembleTask(assembler, file, task.scope);
                return new vscode.Task(task.definition, task.scope, task.name, 'calcu1600', t.execution, t.problemMatchers);
            },
        }),
        vscode.commands.registerCommand('calcu1600.buildAndLoad', buildAndLoad),
        vscode.commands.registerCommand('calcu1600.resetAndStop', () => reset('reset')),
        vscode.commands.registerCommand('calcu1600.allResetAndStop', () => reset('allReset')),
        vscode.commands.registerCommand('calcu1600.createProject', () => scaffold.createProject(extensionPath))
    );
}

function deactivate() {}

module.exports = { activate, deactivate };
