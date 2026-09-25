// Calc-U-1600 debugger glue for VS Code. The debug adapter itself runs
// inside the emulator (a DAP server on 127.0.0.1); this extension only
// connects to it and adds the Build & Load / reset commands.
const vscode = require('vscode');

function activeCalcuSession() {
    const s = vscode.debug.activeDebugSession;
    return s && s.type === 'calcu1600' ? s : undefined;
}

// Resolves once the named task has finished; rejects on a non-zero exit.
function runTask(name) {
    return vscode.tasks.fetchTasks().then(tasks => {
        const task = tasks.find(t => t.name === name);
        if (!task) throw new Error(`No task named "${name}"`);
        return new Promise((resolve, reject) => {
            const done = vscode.tasks.onDidEndTaskProcess(e => {
                if (e.execution.task.name !== name) return;
                done.dispose();
                if (e.exitCode === 0) resolve();
                else reject(new Error(`"${name}" failed (exit ${e.exitCode})`));
            });
            vscode.tasks.executeTask(task).then(undefined, err => { done.dispose(); reject(err); });
        });
    });
}

async function buildAndLoad() {
    const session = activeCalcuSession();
    if (!session) {
        vscode.window.showWarningMessage('Build & Load needs a running Calc-U-1600 debug session.');
        return;
    }
    const config = session.configuration;
    if (!config.program) {
        vscode.window.showWarningMessage('The launch configuration has no "program" to load.');
        return;
    }
    try {
        if (config.buildTask) await runTask(config.buildTask);
        const result = await session.customRequest('calcu1600/load', config.program);
        vscode.window.setStatusBarMessage(`Loaded ${result.start}-${result.end}`, 4000);
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
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('calcu1600', {
            createDebugAdapterDescriptor(session) {
                return new vscode.DebugAdapterServer(session.configuration.port || 4711, '127.0.0.1');
            }
        }),
        vscode.commands.registerCommand('calcu1600.buildAndLoad', buildAndLoad),
        vscode.commands.registerCommand('calcu1600.resetAndStop', () => reset('reset')),
        vscode.commands.registerCommand('calcu1600.allResetAndStop', () => reset('allReset'))
    );
}

function deactivate() {}

module.exports = { activate, deactivate };
