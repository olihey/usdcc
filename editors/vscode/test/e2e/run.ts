// Tries the extension in the installed VS Code, against a real usdcc:
//
//     USDCC_EXE=<usdcc.exe> npm run e2e
//
// usdcc is started with a test profile and quit again; VS Code gets a profile of its own.
// With VSIX=<usdcc.vsix> it's that, installed, rather than this folder.
// VSCODE_EXE names VS Code's program when it isn't where its installer puts it.
import { ChildProcess, spawn, spawnSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { findApps } from '../../src/link';

const PROFILE = 'vscode-e2e';
const usdcc = process.env.USDCC_EXE;
const vscode =
    process.env.VSCODE_EXE ??
    [path.join(process.env.LOCALAPPDATA ?? '', 'Programs/Microsoft VS Code/Code.exe'), path.join(process.env.ProgramFiles ?? '', 'Microsoft VS Code/Code.exe')].find((file) => fs.existsSync(file));

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));
const exit = (process: ChildProcess) => new Promise<number | null>((resolve) => process.on('exit', resolve));

async function main(): Promise<number> {
    if (!usdcc || !vscode) throw new Error('USDCC_EXE and VSCODE_EXE are needed');
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'usdcc-e2e-'));
    const plugin = path.join(root, 'workspace/plugins/e2e');
    fs.mkdirSync(path.join(plugin, 'python/usdcc_e2e'), { recursive: true });
    fs.writeFileSync(path.join(plugin, 'usdcc-plugin.json'), JSON.stringify({ id: 'e2e.plugin', name: 'E2E', version: '1.0', api: 1, python: { module: 'usdcc_e2e' } }));
    fs.writeFileSync(path.join(plugin, 'python/usdcc_e2e/__init__.py'), 'import builtins\ndef activate(plugin):\n    builtins.usdcc_e2e = 1\n');
    fs.mkdirSync(path.join(root, 'user/User'), { recursive: true });
    fs.writeFileSync(path.join(root, 'user/User/settings.json'), JSON.stringify({ 'usdcc.testProfile': PROFILE, 'security.workspace.trust.enabled': false, 'telemetry.telemetryLevel': 'off', 'update.mode': 'none' }));

    // The Python extensions as the user has them, for the debugging part.
    const installed = path.join(os.homedir(), '.vscode/extensions');
    const names = fs.existsSync(installed) ? fs.readdirSync(installed).filter((name) => name.startsWith('ms-python.')) : [];
    for (const name of names) fs.cpSync(path.join(installed, name), path.join(root, 'extensions', name), { recursive: true });
    const stage = path.join(root, 'workspace/shot.usda');
    fs.writeFileSync(stage, '#usda 1.0\n');
    const app = spawn(usdcc, ['--no-startup-scripts', '--plugin-path', path.dirname(plugin), stage], { env: { ...process.env, USDCC_TEST_PROFILE: PROFILE } });
    const appExit = exit(app);
    let failed = true;
    try {
        for (let i = 0; i < 90 && !(await findApps(PROFILE)).length; ++i) await sleep(1000);
        const env: NodeJS.ProcessEnv = { ...process.env, E2E_PLUGIN: plugin };
        delete env.ELECTRON_RUN_AS_NODE;
        for (const name of Object.keys(env)) if (name.startsWith('VSCODE_')) delete env[name];
        let extension = path.resolve(__dirname, '../../..');
        if (process.env.VSIX) {
            const command = path.join(path.dirname(vscode), 'bin', process.platform === 'win32' ? 'code.cmd' : 'code');
            const quoted = [command, '--install-extension', process.env.VSIX, '--extensions-dir', path.join(root, 'extensions'), '--user-data-dir', path.join(root, 'user')].map((each) => `"${each}"`).join(' ');
            const installed = spawnSync(quoted, { shell: true, env, encoding: 'utf8' });
            console.log(`vsix: ${installed.stdout.trim().split('\n').pop()}`);
            // Tests need an extension in development: one that does nothing.
            extension = path.join(root, 'nothing');
            fs.mkdirSync(extension);
            fs.writeFileSync(path.join(extension, 'package.json'), JSON.stringify({ name: 'nothing', publisher: 'test', version: '0.0.1', engines: { vscode: '*' } }));
        }
        const editor = spawn(
            vscode,
            [path.join(root, 'workspace'), `--extensionDevelopmentPath=${extension}`, `--extensionTestsPath=${path.join(__dirname, 'suite')}`, `--user-data-dir=${path.join(root, 'user')}`, `--extensions-dir=${path.join(root, 'extensions')}`, '--disable-workspace-trust', '--skip-welcome', '--skip-release-notes'],
            { env, stdio: 'inherit' },
        );
        const timer = setTimeout(() => editor.kill(), 300000);
        failed = (await exit(editor)) !== 0;
        clearTimeout(timer);
    } finally {
        setTimeout(() => app.kill(), 15000).unref();
        if (failed) app.kill();
        console.log(`usdcc exit ${await appExit}`); // the suite quits it
        try {
            fs.rmSync(root, { recursive: true, force: true, maxRetries: 5, retryDelay: 500 });
        } catch {
            console.log(`left ${root}`);
        }
    }
    return failed ? 1 : 0;
}

main().then(
    (code) => process.exit(code),
    (error) => {
        console.error(error);
        process.exit(1);
    },
);
