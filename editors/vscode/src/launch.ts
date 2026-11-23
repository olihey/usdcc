// Starting usdcc, to connect to (docs/VSCODE.md §6.2). Nothing of VS Code here.
import { ChildProcess, spawn } from 'node:child_process';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { App, findApps } from './link';

export const DEFAULT_PROFILE = 'vscode';

export interface LaunchOptions {
    /** usdcc's program; found when empty. */
    program: string;
    /** A profile of its own, so a broken plugin can't disturb the user's; '' for the user's. */
    profile: string;
    /** Folders of plugin folders. */
    pluginPath: string[];
    stage: string;
    args: string[];
    cwd: string;
    env: Record<string, string | null>;
}

/** The program to start, or undefined. The launcher at the top of an install or a deploy
 *  starts another process and ends, so it's the one in bin. */
export function findProgram(given: string, env: NodeJS.ProcessEnv = process.env): string | undefined {
    const name = process.platform === 'win32' ? 'usdcc.exe' : 'usdcc';
    const candidates = given ? [given] : env.LOCALAPPDATA ? [path.join(env.LOCALAPPDATA, 'usdcc', 'current', 'bin', name)] : [];
    for (const candidate of candidates) {
        for (const file of [path.join(candidate, 'bin', name), path.join(path.dirname(candidate), 'bin', name), candidate]) {
            if (fs.existsSync(file) && fs.statSync(file).isFile()) return file;
        }
    }
    return undefined;
}

export function launchArguments(options: LaunchOptions): string[] {
    return [...options.pluginPath.flatMap((folder) => ['--plugin-path', folder]), ...options.args, ...(options.stage ? [options.stage] : [])];
}

export function launchEnvironment(options: LaunchOptions, base: NodeJS.ProcessEnv = process.env): NodeJS.ProcessEnv {
    const env: NodeJS.ProcessEnv = {};
    for (const [name, value] of Object.entries(base)) {
        // VS Code's own, which would make programs usdcc starts behave as its helpers.
        if (!/^(ELECTRON_|VSCODE_)/.test(name)) env[name] = value;
    }
    if (options.profile) env.USDCC_TEST_PROFILE = options.profile;
    else delete env.USDCC_TEST_PROFILE;
    for (const [name, value] of Object.entries(options.env)) {
        if (value === null) delete env[name];
        else env[name] = value;
    }
    return env;
}

/** A usdcc started here. It goes on when VS Code quits. */
export class Started {
    exited: number | null | undefined;
    failure: string | undefined;
    private constructor(readonly options: LaunchOptions, private readonly child: ChildProcess) {
        child.on('exit', (code) => (this.exited = code));
        child.on('error', (error) => {
            this.failure = error.message;
            this.exited = null;
        });
        child.unref();
    }

    static start(program: string, options: LaunchOptions): Started {
        return new Started(options, spawn(program, launchArguments(options), { cwd: options.cwd, env: launchEnvironment(options), detached: true, stdio: 'ignore' }));
    }

    get pid(): number | undefined {
        return this.child.pid;
    }
    get running(): boolean {
        return this.exited === undefined;
    }

    /** Its link, once it has one. */
    async app(): Promise<App | undefined> {
        return this.running ? (await findApps(this.options.profile)).find((app) => app.pid === this.pid) : undefined;
    }

    /** Waits for its link: undefined when it ends first, or `cancelled` says so. */
    async waitForApp(cancelled: () => boolean, timeoutMs = 120000): Promise<App | undefined> {
        for (const end = Date.now() + timeoutMs; this.running && !cancelled() && Date.now() < end; ) {
            const app = await this.app();
            if (app) return app;
            await new Promise((resolve) => setTimeout(resolve, 250));
        }
        return undefined;
    }
}
