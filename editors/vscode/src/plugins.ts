// The Plugins view, and the problems of plugins that failed.
import * as fs from 'node:fs';
import * as path from 'node:path';
import * as vscode from 'vscode';
import { LinkError, PluginInfo } from './link';
import type { Link } from './link';

const ICONS: Record<string, [string, string?]> = {
    active: ['pass', 'testing.iconPassed'],
    failed: ['error', 'testing.iconFailed'],
    refused: ['warning', 'problemsWarningIcon.foreground'],
    'needs-packages': ['package', 'problemsWarningIcon.foreground'],
    disabled: ['circle-slash'],
    ready: ['circle-outline'],
};

export class PluginsView implements vscode.TreeDataProvider<PluginInfo>, vscode.Disposable {
    plugins: PluginInfo[] = [];
    private readonly emitter = new vscode.EventEmitter<void>();
    readonly onDidChangeTreeData = this.emitter.event;
    private readonly view = vscode.window.createTreeView('usdcc.plugins', { treeDataProvider: this });
    private readonly problems = vscode.languages.createDiagnosticCollection('usdcc plugins');
    private asked = 0;

    dispose(): void {
        this.view.dispose();
        this.problems.dispose();
        this.emitter.dispose();
    }

    /** Asks usdcc again; none without a link. */
    async refresh(link: Link | undefined): Promise<void> {
        const asked = ++this.asked;
        let plugins: PluginInfo[] = [];
        try {
            if (link) plugins = await link.plugins();
        } catch (error) {
            if (!(error instanceof LinkError)) throw error;
        }
        if (asked !== this.asked) return; // a later one has the answer
        this.plugins = plugins;
        this.problems.clear();
        const byFile = new Map<string, vscode.Diagnostic[]>();
        for (const plugin of plugins) {
            if (!plugin.file || !plugin.line) continue;
            const line = Math.max(0, plugin.line - 1);
            const diagnostic = new vscode.Diagnostic(new vscode.Range(line, 0, line, 1000), `${plugin.name}: ${plugin.reason}`);
            diagnostic.source = 'usdcc';
            byFile.set(plugin.file, [...(byFile.get(plugin.file) ?? []), diagnostic]);
        }
        for (const [file, diagnostics] of byFile) this.problems.set(vscode.Uri.file(file), diagnostics);
        this.emitter.fire();
    }

    getChildren(element?: PluginInfo): PluginInfo[] {
        return element ? [] : this.plugins;
    }

    getTreeItem(plugin: PluginInfo): vscode.TreeItem {
        const item = new vscode.TreeItem(plugin.name || plugin.id);
        item.id = plugin.id;
        item.description = plugin.state === 'active' ? '' : plugin.state.replace('-', ' ');
        item.tooltip = [plugin.id, plugin.reason, plugin.notReloadable ? `Can't be reloaded: ${plugin.notReloadable}` : '', plugin.folder].filter(Boolean).join('\n');
        const [icon, color] = ICONS[plugin.state] ?? ICONS.ready;
        item.iconPath = new vscode.ThemeIcon(icon, color ? new vscode.ThemeColor(color) : undefined);
        item.contextValue = plugin.notReloadable ? 'plugin' : 'reloadable';
        // Where it failed, else its manifest.
        const target = plugin.file && fs.existsSync(plugin.file) ? vscode.Uri.file(plugin.file) : vscode.Uri.file(path.join(plugin.folder, 'usdcc-plugin.json'));
        const line = plugin.file && plugin.line ? plugin.line - 1 : 0;
        item.command = { command: 'vscode.open', title: 'Open', arguments: [target, { selection: new vscode.Range(line, 0, line, 0) }] };
        return item;
    }
}
