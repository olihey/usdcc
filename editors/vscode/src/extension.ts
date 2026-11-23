import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import * as vscode from 'vscode';
import type { Frame } from './link';
import { Connection, State } from './connection';
import { DEFAULT_PROFILE, LaunchOptions, Started, findProgram } from './launch';
import { AddResult, App, Link, LinkError, OutputEvent, PluginInfo, findApps, RunResult } from './link';
import { PluginsView } from './plugins';
import { dedent, footer, innermost, mergedPaths, outputText, scriptPath, selectedLines, tracebackLinks } from './text';

const POLL_MS = 2000;
const SAVE_DELAY_MS = 200;
const OUTPUT_LANGUAGE = 'usdcc-output';
const KEEP_VARIABLES = 'keepVariables';
const MANIFEST = 'usdcc-plugin.json';

/** The folder of the plugin that holds `file`: the nearest with a manifest. */
function pluginFolder(file: string): string | undefined {
    for (let folder = path.dirname(file); ; folder = path.dirname(folder)) {
        if (fs.existsSync(path.join(folder, MANIFEST))) return folder;
        if (folder === path.dirname(folder)) return undefined;
    }
}

function isInside(file: string, folder: string): boolean {
    const relative = path.relative(folder, file);
    return relative !== '' && relative !== '..' && !relative.startsWith('..' + path.sep) && !path.isAbsolute(relative);
}

class Extension implements vscode.Disposable {
    private readonly output = vscode.window.createOutputChannel('usdcc', OUTPUT_LANGUAGE);
    private readonly status = vscode.window.createStatusBarItem('usdcc.status', vscode.StatusBarAlignment.Left, 0);
    private readonly connection = new Connection(
        () => this.find(),
        (state) => this.show(state),
        (event) => {
            if (event === 'plugins-changed') void this.plugins.refresh(this.connection.current);
        },
    );
    private readonly plugins = new PluginsView();
    private readonly problems = vscode.languages.createDiagnosticCollection('usdcc');
    private readonly stopButton = vscode.window.createStatusBarItem('usdcc.stop', vscode.StatusBarAlignment.Left, 0);
    private connectedTo?: Link;
    private readonly disposables: vscode.Disposable[] = [];
    private readonly saved = new Map<string, NodeJS.Timeout>();
    private savingForReload = false;
    private running = false;
    private started?: Started;

    constructor(private readonly context: vscode.ExtensionContext) {
        this.status.name = 'usdcc';
        this.status.command = 'usdcc.connect';
        this.status.show();
        this.stopButton.name = 'usdcc: Stop Script';
        this.stopButton.text = '$(debug-stop) Stop';
        this.stopButton.tooltip = 'Stop what usdcc is running';
        this.stopButton.command = 'usdcc.stop';

        const command = (name: string, run: (...args: any[]) => unknown) => this.disposables.push(vscode.commands.registerCommand(name, run));
        command('usdcc.run', () => this.runFile());
        command('usdcc.runSelection', () => this.runSelection());
        command('usdcc.reloadPlugin', () => this.reloadPlugin());
        command('usdcc.start', async () => !!(await this.start({})));
        command('usdcc.attachDebugger', () => vscode.debug.startDebugging(undefined, { type: 'usdcc', request: 'attach', name: 'Attach to usdcc' }));
        command('usdcc.stop', () => this.stop());
        command('usdcc.newPlugin', (name?: string, template?: string, parent?: string) => this.newPlugin(name, template, parent));
        command('usdcc.refreshPlugins', () => this.plugins.refresh(this.connection.current));
        command('usdcc.plugins.reload', (plugin: PluginInfo) => this.savePlugin(plugin.folder).then((saved) => (saved ? this.reload(plugin.folder) : undefined)));
        command('usdcc.plugins.openFolder', (plugin: PluginInfo) => vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(plugin.folder)));
        command('usdcc.setUpWorkspace', () => this.setUpWorkspace());
        command('usdcc.connect', () => this.connect());
        command('usdcc.keepVariables', () => this.toggleKeepVariables());
        command('usdcc.resetVariables', () => this.resetVariables());
        command('usdcc.showOutput', () => this.output.show(true));

        const poll = setInterval(() => {
            if (vscode.window.state.focused) void this.connection.poll();
        }, POLL_MS);
        this.disposables.push(
            { dispose: () => clearInterval(poll) },
            vscode.window.onDidChangeWindowState((state) => {
                if (state.focused) void this.connection.poll();
            }),
            vscode.workspace.onDidChangeConfiguration((event) => {
                if (event.affectsConfiguration('usdcc.testProfile')) void this.connection.restart();
            }),
            vscode.workspace.onDidSaveTextDocument((document) => this.documentSaved(document)),
            vscode.debug.registerDebugConfigurationProvider('usdcc', {
                resolveDebugConfigurationWithSubstitutedVariables: (folder, config, token) => this.resolveDebug(folder, config, token),
                // F5 with no launch.json leaves it empty.
                resolveDebugConfiguration: (_folder, config) => (config.type ? config : { type: 'usdcc', request: 'launch', name: 'Start usdcc' }),
            }),
            vscode.debug.registerDebugConfigurationProvider(
                'usdcc',
                {
                    provideDebugConfigurations: () => [
                        { type: 'usdcc', request: 'launch', name: 'Start usdcc' },
                        { type: 'usdcc', request: 'attach', name: 'Attach to usdcc' },
                    ],
                },
                vscode.DebugConfigurationProviderTriggerKind.Dynamic,
            ),
            vscode.languages.registerDocumentLinkProvider({ language: OUTPUT_LANGUAGE }, {
                provideDocumentLinks: (document) =>
                    tracebackLinks(document.getText()).map(
                        (link) =>
                            new vscode.DocumentLink(
                                new vscode.Range(link.line, link.start, link.line, link.end),
                                vscode.Uri.file(link.file).with({ fragment: `L${link.target}` }),
                            ),
                    ),
            }),
        );
        this.show(this.connection.state);
        void this.connection.poll();
    }

    dispose(): void {
        for (const timer of this.saved.values()) clearTimeout(timer);
        this.connection.dispose();
        this.plugins.dispose();
        this.problems.dispose();
        this.stopButton.dispose();
        for (const each of this.disposables) each.dispose();
        this.status.dispose();
        this.output.dispose();
    }

    // --- The connection ---------------------------------------------------------------

    /** Those with the profile the setting says, and the one started here. */
    private async find(): Promise<App[]> {
        const apps = await findApps(vscode.workspace.getConfiguration('usdcc').get<string>('testProfile', ''));
        const started = await this.started?.app();
        return started && !apps.some((app) => app.pid === started.pid) ? [...apps, started] : apps;
    }

    private show(state: State): void {
        void vscode.commands.executeCommand('setContext', 'usdcc.connected', state.kind === 'connected');
        if (this.connection.current !== this.connectedTo) {
            this.connectedTo = this.connection.current;
            void this.plugins.refresh(this.connectedTo);
        }
        const keeps = this.keepsVariables ? '\n\nVariables are kept between runs.' : '';
        switch (state.kind) {
            case 'none':
                this.status.text = 'usdcc: not running';
                this.status.tooltip = "No usdcc that lets editors connect is running.";
                break;
            case 'several':
                this.status.text = '$(zap) usdcc: pick one';
                this.status.tooltip = `${state.apps.length} are running. Click to pick one.`;
                break;
            case 'incompatible':
                this.status.text = '$(warning) usdcc: update needed';
                this.status.tooltip = `usdcc ${state.hello.version} and this extension don't speak the same protocol. Update the older of the two.`;
                break;
            case 'connected':
                this.status.text = `$(zap) usdcc ${state.hello.version.split('+')[0]} · ${state.hello.stage || 'no stage'}`;
                this.status.tooltip = `usdcc ${state.hello.version}, process ${state.hello.pid}\n${state.hello.stagePath}${keeps}`;
                break;
        }
    }

    private async connect(): Promise<void> {
        const apps = await this.connection.apps();
        if (apps.length === 0) {
            void vscode.window.showInformationMessage("No usdcc that lets editors connect is running. See Settings > Python > Allow editors to connect in usdcc.");
            return;
        }
        let pid = apps[0].app.pid;
        if (apps.length > 1) {
            const picked = await vscode.window.showQuickPick(
                apps.map(({ app, hello }) => ({ label: hello.stage || 'no stage', description: `usdcc ${hello.version}, process ${app.pid}`, detail: hello.stagePath, pid: app.pid })),
                { placeHolder: 'The usdcc to run in' },
            );
            if (!picked) return;
            pid = picked.pid;
        }
        await this.connection.pick(pid);
    }

    /** The link, or a message saying there's none. */
    private async link(): Promise<Link | undefined> {
        if (!this.connection.current) await this.connection.poll();
        if (this.connection.state.kind === 'several') await this.connect();
        const link = this.connection.current;
        if (!link) {
            void vscode.window.showWarningMessage(
                this.connection.state.kind === 'incompatible' ? 'This usdcc and this extension need the same version of the protocol: update the older.' : 'No usdcc to run in: start it, and let editors connect.',
            );
        }
        return link;
    }

    // --- Running ----------------------------------------------------------------------

    private get keepsVariables(): boolean {
        return this.context.workspaceState.get<boolean>(KEEP_VARIABLES, false);
    }

    private async toggleKeepVariables(): Promise<void> {
        await this.context.workspaceState.update(KEEP_VARIABLES, !this.keepsVariables);
        this.show(this.connection.state);
        void vscode.window.setStatusBarMessage(this.keepsVariables ? 'usdcc: variables are kept between runs' : 'usdcc: each run starts fresh', 4000);
    }

    private async resetVariables(): Promise<void> {
        const link = await this.link();
        if (link) await this.guarded(() => link.resetSession());
    }

    private editor(): vscode.TextEditor | undefined {
        const editor = vscode.window.activeTextEditor;
        if (editor?.document.languageId === 'python') return editor;
        void vscode.window.showWarningMessage('Open a Python file to run it in usdcc.');
        return undefined;
    }

    private async runFile(): Promise<unknown> {
        const editor = vscode.window.activeTextEditor;
        // In a plugin, as in the Script Editor's plugin tab: running one of its files as a
        // script fails on its relative imports.
        if (editor?.document.uri.scheme === 'file' && pluginFolder(editor.document.uri.fsPath)) return this.reloadPlugin();
        const python = this.editor();
        return python && this.run(python.document, python.document.getText(), 1, '');
    }

    private async runSelection(): Promise<unknown> {
        const editor = this.editor();
        if (!editor) return undefined;
        const { start, end } = editor.selection;
        const [first, last] = selectedLines(start.line, end.line, end.character);
        const lines: string[] = [];
        for (let line = first; line <= last; ++line) lines.push(editor.document.lineAt(line).text);
        return this.run(editor.document, dedent(lines), first + 1, ' (selection)');
    }

    private async run(document: vscode.TextDocument, code: string, line: number, suffix: string): Promise<RunResult | undefined> {
        const link = await this.link();
        if (!link) return undefined;
        const name = path.basename(document.uri.path);
        const file = document.uri.scheme === 'file' ? scriptPath(document.uri.fsPath) : undefined;
        return this.working(`Run ${name}${suffix}`, async (onOutput) => {
            const result = await link.run({ code, line, scope: this.keepsVariables ? 'session' : 'fresh', ...(file ? { file } : { name: name + suffix }) }, onOutput);
            this.output.appendLine(footer(result));
            this.problems.clear();
            if (result.error) {
                this.mark(result.error, document);
                await this.reveal(result.error.frames, document);
            }
            return result;
        });
    }

    /** A problem on the line where it stopped, until the next run. */
    private mark(error: NonNullable<RunResult['error']>, ran: vscode.TextDocument): void {
        const frame = this.ours(error.frames, ran);
        if (!frame || error.type === 'KeyboardInterrupt') return;
        const line = frame.line - 1;
        const diagnostic = new vscode.Diagnostic(new vscode.Range(line, 0, line, 1000), `${error.type}: ${error.message}`);
        diagnostic.source = 'usdcc';
        this.problems.set(this.isFile(frame.file, ran) ? ran.uri : vscode.Uri.file(frame.file), [diagnostic]);
    }

    /** Whether a frame's file is the one that was run. */
    private isFile(file: string, ran: vscode.TextDocument): boolean {
        if (ran.uri.scheme !== 'file') return file === path.basename(ran.uri.path) || file.startsWith(`${path.basename(ran.uri.path)} (`);
        const [a, b] = [path.normalize(file), path.normalize(ran.uri.fsPath)];
        return process.platform === 'win32' ? a.toLowerCase() === b.toLowerCase() : a === b;
    }

    /** The innermost frame in the file that was run, or in the workspace. */
    private ours(frames: Frame[], ran: vscode.TextDocument): Frame | undefined {
        return innermost(frames, (file) => this.isFile(file, ran) || (path.isAbsolute(file) && !!vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file)) && fs.existsSync(file)));
    }

    private async stop(): Promise<boolean> {
        const link = this.connection.current;
        const stopping = !!link && !!(await this.guarded(() => link.stop()));
        if (!stopping) void vscode.window.setStatusBarMessage('usdcc: nothing is running', 3000);
        return stopping;
    }

    /** Moves the cursor to where a script stopped: the innermost frame of ours. */
    private async reveal(frames: Frame[], ran: vscode.TextDocument): Promise<void> {
        this.output.show(true);
        const frame = this.ours(frames, ran);
        if (!frame) return;
        const document = this.isFile(frame.file, ran) ? ran : await vscode.workspace.openTextDocument(vscode.Uri.file(frame.file));
        const line = Math.min(frame.line, document.lineCount) - 1;
        const column = document.lineAt(line).firstNonWhitespaceCharacterIndex;
        const at = new vscode.Range(line, column, line, column);
        // Where it's already shown, if it is.
        const shown = vscode.window.visibleTextEditors.find((editor) => editor.document === document);
        await vscode.window.showTextDocument(document, { selection: at, viewColumn: shown?.viewColumn });
    }

    // --- Starting and debugging -------------------------------------------------------

    /** Starts usdcc and connects to it, or to the one started before that still runs. */
    private async start(config: Partial<LaunchOptions>, folder?: vscode.WorkspaceFolder, token?: vscode.CancellationToken): Promise<boolean> {
        const at = folder?.uri.fsPath ?? vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? os.homedir();
        const options: LaunchOptions = {
            program: config.program || vscode.workspace.getConfiguration('usdcc').get<string>('program', ''),
            profile: config.profile ?? DEFAULT_PROFILE,
            pluginPath: config.pluginPath ?? (await this.pluginFolders()),
            stage: config.stage ?? '',
            args: config.args ?? [],
            cwd: at,
            env: config.env ?? {},
        };
        if (/[\\/]/.test(options.profile)) {
            void vscode.window.showErrorMessage('A profile is a name, without slashes.');
            return false;
        }
        if (!this.started?.running || JSON.stringify(this.started.options) !== JSON.stringify(options)) {
            const program = findProgram(options.program);
            if (!program) {
                void vscode.window.showErrorMessage(options.program ? `There is no usdcc at ${options.program}.` : 'No installed usdcc was found: set usdcc.program.');
                return false;
            }
            this.started = Started.start(program, options);
        }
        const started = this.started;
        const app = await vscode.window.withProgress({ location: vscode.ProgressLocation.Notification, title: 'Starting usdcc', cancellable: true }, (_progress, cancel) =>
            started.waitForApp(() => cancel.isCancellationRequested || !!token?.isCancellationRequested),
        );
        if (!app) {
            if (!started.running) {
                void vscode.window.showErrorMessage(started.failure ? `usdcc couldn't start: ${started.failure}` : `usdcc ended before it could be connected to (exit code ${started.exited}).`);
            } else {
                void vscode.window.showWarningMessage('usdcc is running, but not connected: does it let editors connect?');
            }
            return false;
        }
        await this.connection.pick(app.pid);
        return !!this.connection.current;
    }

    /** The folders holding the workspace's plugins. */
    private async pluginFolders(): Promise<string[]> {
        const manifests = await vscode.workspace.findFiles(`**/${MANIFEST}`, '**/{node_modules,build,deploy,out,.git}/**', 200);
        return [...new Set(manifests.map((uri) => path.dirname(path.dirname(uri.fsPath))))].sort();
    }

    /** What VS Code's Python debugger is to attach to. */
    private async resolveDebug(folder: vscode.WorkspaceFolder | undefined, config: vscode.DebugConfiguration, token?: vscode.CancellationToken): Promise<vscode.DebugConfiguration | undefined> {
        if (!vscode.extensions.getExtension('ms-python.debugpy')) {
            const install = 'Install';
            if ((await vscode.window.showErrorMessage('Debugging the Python in usdcc needs the Python Debugger extension.', install)) === install) {
                await vscode.commands.executeCommand('workbench.extensions.installExtension', 'ms-python.debugpy');
            }
            return undefined;
        }
        if (config.request === 'launch' && !(await this.start(config as Partial<LaunchOptions>, folder, token))) return undefined;
        const link = await this.link();
        if (!link) return undefined;
        const port = await this.guarded(() => link.debugListen());
        if (!port) return undefined;
        return {
            type: 'debugpy',
            request: 'attach',
            name: config.name || 'usdcc',
            connect: { host: '127.0.0.1', port },
            justMyCode: config.justMyCode ?? true,
            subProcess: false,
            // Kept with it, such as a preLaunchTask's counterpart.
            ...(config.postDebugTask ? { postDebugTask: config.postDebugTask } : {}),
        };
    }

    // --- Completion -------------------------------------------------------------------

    /** Tells the Python extension where usdcc, pxr and PySide6 are, for this workspace. */
    private async setUpWorkspace(): Promise<boolean> {
        if (!vscode.workspace.workspaceFolders?.length) {
            void vscode.window.showWarningMessage('Open a folder to set it up.');
            return false;
        }
        const link = await this.link();
        const found = link && (await this.guarded(() => link.setUpTypes()));
        if (!found) return false;
        const python = vscode.workspace.getConfiguration('python');
        try {
            const key = 'analysis.extraPaths';
            await python.update(key, mergedPaths(found.paths, python.inspect<string[]>(key)?.workspaceValue ?? []), vscode.ConfigurationTarget.Workspace);
            // A build's only: a deployed usdcc's own Python isn't for editors to use.
            if (found.executable && !python.inspect<string>('defaultInterpreterPath')?.workspaceValue) {
                await python.update('defaultInterpreterPath', found.executable, vscode.ConfigurationTarget.Workspace);
            }
        } catch (error) {
            const install = 'Install';
            const answer = await vscode.window.showErrorMessage(`The workspace's settings can't be changed: is the Python extension installed? (${error instanceof Error ? error.message : error})`, install);
            if (answer === install) await vscode.commands.executeCommand('workbench.extensions.installExtension', 'ms-python.python');
            return false;
        }
        void vscode.window.showInformationMessage('usdcc, pxr and PySide6 are completed in this workspace now.');
        return true;
    }

    // --- Plugins ----------------------------------------------------------------------

    private async reloadPlugin(): Promise<unknown> {
        const uri = vscode.window.activeTextEditor?.document.uri;
        const folder = uri?.scheme === 'file' ? pluginFolder(uri.fsPath) : undefined;
        if (!folder) {
            void vscode.window.showWarningMessage(`The current file isn't in a plugin's folder: none above it has a ${MANIFEST}.`);
            return undefined;
        }
        return (await this.savePlugin(folder)) ? this.reload(folder) : undefined;
    }

    /** Saves a plugin's files that are open; false when one can't be. */
    private async savePlugin(folder: string): Promise<boolean> {
        this.savingForReload = true;
        try {
            for (const document of vscode.workspace.textDocuments) {
                if (document.isDirty && document.uri.scheme === 'file' && isInside(document.uri.fsPath, folder) && !(await document.save())) {
                    void vscode.window.showWarningMessage(`${path.basename(document.uri.fsPath)} couldn't be saved, so the plugin wasn't reloaded.`);
                    return false;
                }
            }
        } finally {
            this.savingForReload = false;
        }
        return true;
    }

    private async reload(folder: string): Promise<unknown> {
        const link = await this.link();
        if (!link) return undefined;
        this.problems.clear();
        return this.working(`Reload ${path.basename(folder)}`, async (onOutput) => {
            let result;
            try {
                result = await link.reloadPlugin({ path: scriptPath(path.join(folder, MANIFEST)) }, onOutput);
            } catch (error) {
                // Made, or put there, since usdcc started, or in a folder it doesn't look in.
                if (!(error instanceof LinkError) || error.code !== 'not-found') throw error;
                return this.added(await link.addPlugin(scriptPath(folder), onOutput));
            }
            for (const note of result.notes) this.output.appendLine(note);
            if (result.ok) {
                this.output.appendLine(`    reloaded ${result.reloaded.join(', ')} in ${result.ms} ms`);
            } else {
                this.output.appendLine(`    not running: ${result.failure || result.plugin?.reason || ''}`);
                this.output.show(true);
            }
            return result;
        });
    }

    private added(result: AddResult): AddResult {
        this.output.appendLine(result.ok ? `    started ${result.plugin?.name}` : `    not running: ${result.failure}`);
        if (!result.ok) this.output.show(true);
        return result;
    }

    private async newPlugin(name?: string, template?: string, parent?: string): Promise<AddResult | undefined> {
        const link = await this.link();
        if (!link) return undefined;
        if (!template) {
            const templates = (await this.guarded(() => link.templates())) ?? [];
            const picked = await vscode.window.showQuickPick(templates.map((each) => ({ label: each.label, detail: each.description, id: each.id })), { placeHolder: 'What it starts with' });
            if (!picked) return undefined;
            template = picked.id;
        }
        name ??= (await vscode.window.showInputBox({ prompt: "The plugin's name", validateInput: (text) => (/[a-z0-9]/i.test(text) ? undefined : 'It needs some letters or digits.') }))?.trim();
        if (!name) return undefined;
        if (!parent) {
            const folders = await vscode.window.showOpenDialog({ canSelectFolders: true, canSelectFiles: false, openLabel: 'Make It Here', defaultUri: vscode.workspace.workspaceFolders?.[0]?.uri });
            if (!folders?.length) return undefined;
            parent = folders[0].fsPath;
        }
        const params = { name, template, parent: scriptPath(parent) };
        const result = await this.working(`New plugin ${name}`, async (onOutput) => this.added(await link.createPlugin(params, onOutput)));
        if (result?.plugin) {
            const [main] = await vscode.workspace.findFiles(new vscode.RelativePattern(vscode.Uri.file(result.plugin.folder), 'python/*/__init__.py'), undefined, 1);
            await vscode.window.showTextDocument(main ?? vscode.Uri.file(path.join(result.plugin.folder, MANIFEST)));
        }
        return result;
    }

    private documentSaved(document: vscode.TextDocument): void {
        if (this.savingForReload || document.uri.scheme !== 'file' || !this.connection.current) return;
        if (!vscode.workspace.getConfiguration('usdcc', document.uri).get<boolean>('reloadPluginOnSave', false)) return;
        const folder = pluginFolder(document.uri.fsPath);
        if (!folder) return;
        // Once for a Save All, and after whatever is running.
        clearTimeout(this.saved.get(folder));
        const later = () => {
            if (this.running) {
                this.saved.set(folder, setTimeout(later, SAVE_DELAY_MS));
                return;
            }
            this.saved.delete(folder);
            if (this.connection.current) void this.reload(folder);
        };
        this.saved.set(folder, setTimeout(later, SAVE_DELAY_MS));
    }

    // --- Both -------------------------------------------------------------------------

    private async working<T>(title: string, work: (onOutput: (event: OutputEvent) => void) => Promise<T>): Promise<T | undefined> {
        if (this.running) {
            void vscode.window.showWarningMessage('usdcc is still running what was sent before.');
            return undefined;
        }
        this.running = true;
        this.stopButton.show();
        this.output.appendLine(`>>> ${title}`);
        this.output.show(true);
        try {
            return await this.guarded(() =>
                vscode.window.withProgress({ location: vscode.ProgressLocation.Window, title: `usdcc: ${title}` }, () => work((event) => this.output.appendLine(outputText(event)))),
            );
        } finally {
            this.running = false;
            this.stopButton.hide();
            void this.connection.poll();
        }
    }

    private async guarded<T>(work: () => PromiseLike<T>): Promise<T | undefined> {
        try {
            return await work();
        } catch (error) {
            if (!(error instanceof LinkError)) throw error;
            this.output.appendLine(`    ${error.message}`);
            void vscode.window.showWarningMessage(error.message);
            return undefined;
        }
    }
}

export function activate(context: vscode.ExtensionContext): void {
    context.subscriptions.push(new Extension(context));
}
