// Runs inside VS Code (run.ts): the commands, against the usdcc run.ts started.
import assert from 'node:assert/strict';
import * as fs from 'node:fs';
import * as path from 'node:path';
import * as vscode from 'vscode';

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

async function open(file: string, content?: string): Promise<vscode.TextEditor> {
    if (content !== undefined) fs.writeFileSync(file, content);
    return vscode.window.showTextDocument(await vscode.workspace.openTextDocument(file));
}

/** The output channel's text, shown or not. */
function output(): vscode.TextDocument | undefined {
    return vscode.workspace.textDocuments.find((document) => document.languageId === 'usdcc-output');
}

export async function run(): Promise<void> {
    const extension = vscode.extensions.all.find((each) => each.packageJSON.name === 'usdcc' && each.packageJSON.main);
    assert.ok(extension, 'it is there');
    await extension.activate();
    const workspace = vscode.workspace.workspaceFolders![0].uri.fsPath;
    const plugin = process.env.E2E_PLUGIN!;

    // Unsaved text runs as it looks.
    const scratch = await open(path.join(workspace, 'a.py'), 'raise SystemError("the saved text")\n');
    const all = new vscode.Range(0, 0, scratch.document.lineCount, 0);
    await scratch.edit((edit) => edit.replace(all, 'import usdcc.usd\ns = usdcc.usd.current_stage()\ns.DefinePrim("/FromEditor")\nprint("ran unsaved")\n'));
    let result: any = await vscode.commands.executeCommand('usdcc.run');
    assert.ok(result?.ok && result.undoSteps === 1, `Run File: ${JSON.stringify(result)}`);
    console.log('e2e: Run File ok');

    // A selection that fails: the cursor goes to the file's line.
    const failing = await open(path.join(workspace, 'b.py'), 'x = 1\n\n\ndef f():\n    if True:\n        raise ValueError("no")\n');
    failing.selection = new vscode.Selection(4, 0, 6, 0);
    result = await vscode.commands.executeCommand('usdcc.runSelection');
    assert.ok(!result?.ok && result.error.type === 'ValueError' && result.error.line === 6, `Run Selection: ${JSON.stringify(result)}`);
    assert.equal(vscode.window.activeTextEditor?.document.uri.fsPath.toLowerCase(), failing.document.uri.fsPath.toLowerCase());
    assert.equal(vscode.window.activeTextEditor?.selection.active.line, 5, 'the cursor is where it stopped');
    console.log('e2e: Run Selection ok');

    // The output and its links.
    await sleep(1000);
    const channel = output();
    assert.ok(channel, `an output channel: ${vscode.workspace.textDocuments.map((each) => `${each.uri} ${each.languageId}`)}`);
    assert.ok(channel.getText().includes('ran unsaved') && channel.getText().includes('1 undo step'), `output: ${channel.getText()}`);
    const links = await vscode.commands.executeCommand<vscode.DocumentLink[]>('vscode.executeLinkProvider', channel.uri, 100);
    const link = links.find((each) => each.target?.fragment === 'L6');
    assert.ok(link && link.target!.fsPath.toLowerCase().endsWith('b.py'), `links: ${JSON.stringify(links)}`);
    console.log('e2e: output ok');

    // Kept variables.
    await vscode.commands.executeCommand('usdcc.keepVariables');
    await open(path.join(workspace, 'c.py'), 'kept = 5\n');
    await vscode.commands.executeCommand('usdcc.run');
    await open(path.join(workspace, 'd.py'), 'assert kept == 5\n');
    result = await vscode.commands.executeCommand('usdcc.run');
    assert.ok(result?.ok, `kept: ${JSON.stringify(result)}`);
    await vscode.commands.executeCommand('usdcc.resetVariables');
    result = await vscode.commands.executeCommand('usdcc.run');
    assert.equal(result?.error?.type, 'NameError');
    await vscode.commands.executeCommand('usdcc.keepVariables');
    console.log('e2e: variables ok');

    // Run File in a plugin saves and reloads it; a broken one says so.
    const source = path.join(plugin, 'python/usdcc_e2e/__init__.py');
    const code = (value: string) => `import builtins\ndef activate(plugin):\n    builtins.usdcc_e2e = ${value}\n`;
    const check = async (value: number, why: string) => {
        await open(path.join(workspace, `check${value}.py`), `import builtins\nassert builtins.usdcc_e2e == ${value}, builtins.usdcc_e2e\n`);
        const checked: any = await vscode.commands.executeCommand('usdcc.run');
        assert.ok(checked?.ok, `${why}: ${JSON.stringify(checked)}`);
    };
    const replace = async (value: string) => {
        const editor = await open(source);
        await editor.edit((edit) => edit.replace(new vscode.Range(0, 0, editor.document.lineCount, 0), code(value)));
        return editor;
    };
    await replace('2');
    result = await vscode.commands.executeCommand('usdcc.run');
    assert.ok(result?.ok && fs.readFileSync(source, 'utf8').includes('= 2'), `reload: ${JSON.stringify(result)}`);
    await check(2, 'reloaded');
    await replace('(');
    result = await vscode.commands.executeCommand('usdcc.reloadPlugin');
    assert.ok(result && !result.ok && result.plugin.state === 'failed', `broken: ${JSON.stringify(result)}`);
    console.log('e2e: reload ok');

    // Its problem, and the run's before it.
    await sleep(1000);
    assert.ok(vscode.languages.getDiagnostics(vscode.Uri.file(source)).some((each) => each.source === 'usdcc'), 'a problem on the plugin');
    await open(path.join(workspace, 'bad.py'), '\n1 / 0\n');
    await vscode.commands.executeCommand('usdcc.run');
    const marked = vscode.languages.getDiagnostics(vscode.Uri.file(path.join(workspace, 'bad.py'))).filter((each) => each.source === 'usdcc');
    assert.ok(marked.length === 1 && marked[0].range.start.line === 1 && marked[0].message.includes('ZeroDivisionError'), `a problem on the line: ${JSON.stringify(marked)}`);
    console.log('e2e: problems ok');

    // Stop, from here as the button does.
    await open(path.join(workspace, 'endless.py'), 'while True:\n    pass\n');
    const endless = vscode.commands.executeCommand<any>('usdcc.run');
    await sleep(1500);
    assert.ok(await vscode.commands.executeCommand('usdcc.stop'), 'there is something to stop');
    result = await Promise.race([endless, sleep(10000)]);
    assert.equal(result?.error?.type, 'KeyboardInterrupt');
    assert.equal(vscode.languages.getDiagnostics(vscode.Uri.file(path.join(workspace, 'bad.py'))).length, 0, "the last run's problem is gone");
    console.log('e2e: stop ok');

    // A plugin from a template, running at once; and the view's own problem-free state.
    result = await vscode.commands.executeCommand('usdcc.newPlugin', 'E2E Made', 'menu_action', path.join(workspace, 'plugins'));
    assert.ok(result?.ok && vscode.window.activeTextEditor?.document.fileName.endsWith('__init__.py'), `new plugin: ${JSON.stringify(result)}`);
    // One put there by hand, which usdcc doesn't know: Reload Plugin takes it in.
    const byHand = path.join(workspace, 'plugins/by_hand/python/usdcc_by_hand/__init__.py');
    fs.mkdirSync(path.dirname(byHand), { recursive: true });
    fs.writeFileSync(path.join(workspace, 'plugins/by_hand/usdcc-plugin.json'), JSON.stringify({ id: 'e2e.hand', name: 'Hand', version: '1', api: 1, python: { module: 'usdcc_by_hand' } }));
    await open(byHand, 'def activate(plugin):\n    print("by hand")\n');
    result = await vscode.commands.executeCommand('usdcc.reloadPlugin');
    assert.ok(result?.ok, `by hand: ${JSON.stringify(result)}`);
    // A manifest is checked as it's typed.
    const manifest = await open(path.join(workspace, 'plugins/wrong/usdcc-plugin.json'.replace('wrong/', 'by_hand/')));
    await manifest.edit((edit) => edit.replace(new vscode.Range(0, 0, manifest.document.lineCount, 0), '{ "id": "a b", "pyhton": {} }'));
    let checked: vscode.Diagnostic[] = [];
    for (let i = 0; i < 20 && checked.length < 2; ++i) {
        await sleep(500);
        checked = vscode.languages.getDiagnostics(manifest.document.uri);
    }
    assert.ok(checked.length >= 2, `the manifest's problems: ${checked.map((each) => each.message)}`);
    await vscode.commands.executeCommand('workbench.action.files.revert');
    console.log('e2e: plugins ok');

    // On save.
    await vscode.workspace.getConfiguration('usdcc').update('reloadPluginOnSave', true, vscode.ConfigurationTarget.Global);
    const editor = await replace('3');
    await editor.document.save();
    await sleep(2500);
    await check(3, 'reloaded on save');
    console.log('e2e: reload on save ok');

    // Completion, if the Python extension is there, and Pylance.
    if (vscode.extensions.getExtension('ms-python.vscode-pylance')) {
        assert.ok(await vscode.commands.executeCommand('usdcc.setUpWorkspace'), 'Set Up Workspace');
        const text = 'import usdcc.usd\nstage = usdcc.usd.current_stage()\nassert stage\nstage.GetPrimAtPath("/a").\n';
        const typed = await open(path.join(workspace, 'complete.py'), text);
        let labels: string[] = [];
        for (let i = 0; i < 40 && !labels.includes('GetAttribute'); ++i) {
            await sleep(1000);
            const list = await vscode.commands.executeCommand<vscode.CompletionList>('vscode.executeCompletionItemProvider', typed.document.uri, new vscode.Position(3, 26), '.');
            labels = list.items.map((item) => (typeof item.label === 'string' ? item.label : item.label.label));
        }
        assert.ok(labels.includes('GetAttribute'), `completed: ${labels.slice(0, 20)}`);
        console.log('e2e: completion ok');
    } else {
        console.log('e2e: no Pylance, so no completion');
    }

    // The debugger, if VS Code's Python Debugger is there.
    if (vscode.extensions.getExtension('ms-python.debugpy')) {
        const stops = await open(path.join(workspace, 'stops.py'), 'value = 1\nvalue += 41\nprint("stopped once", value)\n');
        vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(stops.document.uri, new vscode.Position(1, 0)))]);
        const places: string[] = [];
        const stopped = new Promise<number>((resolve) =>
            vscode.debug.registerDebugAdapterTrackerFactory('*', {
                createDebugAdapterTracker: (session) => ({
                    onDidSendMessage: async (message: any) => {
                        if (message.type !== 'event' || message.event !== 'stopped') return;
                        const stack = await session.customRequest('stackTrace', { threadId: message.body.threadId });
                        resolve(stack.stackFrames[0].line);
                        places.push(`${stack.stackFrames[0].name}:${stack.stackFrames[0].line}`);
                        await session.customRequest('continue', { threadId: message.body.threadId });
                    },
                }),
            }),
        );
        assert.ok(await vscode.commands.executeCommand('usdcc.attachDebugger'), 'it attaches');
        await sleep(2000);
        await vscode.window.showTextDocument(stops.document);
        const running = vscode.commands.executeCommand('usdcc.run');
        assert.equal(await Promise.race([stopped, sleep(20000)]), 2, 'it stops at the breakpoint');
        result = await running;
        assert.ok(result?.ok, `and goes on: ${JSON.stringify(result)}`);
        // In a plugin's menu action, triggered as a click does.
        const action = path.join(plugin, 'python/usdcc_e2e/__init__.py');
        const acting = await open(action);
        await acting.edit((edit) => edit.replace(new vscode.Range(0, 0, acting.document.lineCount, 0), 'import builtins, usdcc.app\ndef act():\n    builtins.usdcc_e2e_acted = True\ndef activate(plugin):\n    builtins.usdcc_e2e = 3\n    usdcc.app.add_menu_action("Tools/E2E/Act", act)\n'));
        assert.ok((await vscode.commands.executeCommand<any>('usdcc.reloadPlugin'))?.ok);
        vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(new vscode.Location(acting.document.uri, new vscode.Position(2, 0)))]);
        await sleep(1000);
        places.length = 0;
        await open(
            path.join(workspace, 'click.py'),
            [
                'from PySide6.QtCore import QTimer',
                'from PySide6.QtWidgets import QApplication, QMenu',
                'found = []',
                'for menu in [each for each in QApplication.allWidgets() if isinstance(each, QMenu)]:',
                '    try:',
                '        found += [action for action in menu.actions() if action.text().replace("&", "") == "Act"]',
                '    except RuntimeError:',
                '        pass  # a popup menu that has gone',
                'assert found, [each.title() for each in QApplication.allWidgets() if isinstance(each, QMenu)]',
                'QTimer.singleShot(200, found[0].trigger)',
                '',
            ].join('\n'),
        );
        result = await vscode.commands.executeCommand('usdcc.run');
        assert.ok(result?.ok, `the action is there: ${JSON.stringify(result)} ${output()?.getText().slice(-900)}`);
        for (let i = 0; i < 40 && !places.length; ++i) await sleep(250);
        assert.ok(places.some((place) => place.startsWith('act:3')), `it stops in the menu action: ${places}`);
        await sleep(1000);
        await vscode.debug.stopDebugging();
        console.log('e2e: attach ok');

        // F5 from nothing: another usdcc, with the workspace's plugin. A breakpoint in its action.
        await vscode.workspace.getConfiguration('usdcc').update('program', process.env.USDCC_EXE, vscode.ConfigurationTarget.Global);
        assert.ok(await vscode.debug.startDebugging(vscode.workspace.workspaceFolders![0], { type: 'usdcc', request: 'launch', name: 'F5', profile: 'vscode-e2e-f5', args: ['--no-startup-scripts'] }), 'F5');
        await sleep(2000);
        assert.equal(vscode.debug.activeDebugSession?.type, 'debugpy');
        await open(path.join(workspace, 'f5.py'), 'import os, builtins\nassert os.environ["USDCC_TEST_PROFILE"] == "vscode-e2e-f5"\nassert builtins.usdcc_e2e == 3\n');
        result = await vscode.commands.executeCommand('usdcc.run');
        assert.ok(result?.ok, `the one F5 started, with the plugin: ${JSON.stringify(result)}`);
        await vscode.debug.stopDebugging();
        await open(path.join(workspace, 'quit5.py'), 'from PySide6.QtWidgets import QApplication\nQApplication.quit()\n');
        result = await vscode.commands.executeCommand('usdcc.run');
        assert.ok(result?.ok, `quit F5's: ${JSON.stringify(result)}`);
        await sleep(6000);
        console.log('e2e: F5 ok');
    } else {
        console.log('e2e: no Python Debugger, so no debugging');
    }

    // Last: quits the app. Its answer still arrives.
    await open(path.join(workspace, 'quit.py'), 'import usdcc.usd\nfrom PySide6.QtWidgets import QApplication\nusdcc.usd.current_stage().Save()  # or it asks\nQApplication.quit()\n');
    result = await vscode.commands.executeCommand('usdcc.run');
    assert.ok(result?.ok, `quit: ${JSON.stringify(result)}`);
    console.log('e2e: quit ok');
}
