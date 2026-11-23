// The client's side of usdcc's editor link (docs/VSCODE.md §3). Nothing of VS Code here,
// so that it can be tested with Node alone.
import * as fs from 'node:fs';
import * as net from 'node:net';
import * as os from 'node:os';
import * as path from 'node:path';

export const API = 1;

export interface Hello {
    api: number;
    version: string;
    pid: number;
    stage: string;
    stagePath: string;
    busy: boolean;
    /** Where a debugger can attach; 0 when none can. */
    debugPort?: number;
}
export interface OutputEvent {
    event: 'output';
    run: number;
    source: string;
    category: string;
    level: string;
    text: string;
}
export interface Frame {
    file: string;
    line: number;
    function: string;
}
export interface RunResult {
    ok: boolean;
    ms: number;
    undoSteps: number;
    error?: { type: string; message: string; file?: string; line?: number; frames: Frame[] };
}
export interface PluginInfo {
    id: string;
    name: string;
    folder: string;
    state: string;
    reason: string;
    notReloadable?: string;
    /** Where its Python failed, when it did. */
    file?: string;
    line?: number;
}
export interface Template {
    id: string;
    label: string;
    description: string;
}
/** Of createPlugin and addPlugin. */
export interface AddResult {
    ok: boolean;
    failure: string;
    plugin?: PluginInfo;
}
export interface ReloadResult {
    ok: boolean;
    ms: number;
    reloaded: string[];
    notes: string[];
    failure: string;
    plugin?: PluginInfo;
}

/** usdcc refused a request: busy, dialog, unavailable, not-found or bad-request; or the link is gone: closed, timeout. */
export class LinkError extends Error {
    constructor(readonly code: string, message: string) {
        super(message);
    }
}

export interface App {
    pid: number;
    address: string;
}

export function linkFolder(): string {
    if (process.platform === 'win32') return '\\\\.\\pipe\\';
    const runtime = process.env.XDG_RUNTIME_DIR;
    return runtime ? path.join(runtime, 'usdcc') : path.join(os.tmpdir(), `usdcc-${process.getuid!()}`);
}

/** The apps that let editors connect, from the names of their pipes. */
export async function findApps(testProfile = '', folder = linkFolder()): Promise<App[]> {
    const prefix = `${testProfile ? `usdcc-test-${testProfile}` : 'usdcc'}-editor-`;
    let names: string[];
    try {
        names = await fs.promises.readdir(folder);
    } catch {
        return [];
    }
    return names
        .filter((name) => name.startsWith(prefix) && /^\d+$/.test(name.slice(prefix.length)))
        .map((name) => ({ pid: Number(name.slice(prefix.length)), address: folder.endsWith('\\') ? folder + name : path.join(folder, name) }))
        .sort((a, b) => a.pid - b.pid);
}

interface Pending {
    resolve: (result: unknown) => void;
    reject: (error: Error) => void;
    onOutput?: (event: OutputEvent) => void;
    timer?: NodeJS.Timeout;
}

export class Link {
    private pending = new Map<number, Pending>();
    private nextId = 0;
    // Bytes, as a character can be split between two chunks.
    private chunks: Buffer[] = [];
    private closed = false;
    private closeHandlers: (() => void)[] = [];
    private eventHandlers: ((name: string) => void)[] = [];

    private constructor(private readonly socket: net.Socket) {
        socket.on('data', (data: Buffer) => this.read(data));
        socket.on('error', () => socket.destroy());
        socket.on('close', () => this.ended());
    }

    static connect(address: string, timeoutMs = 2000): Promise<Link> {
        return new Promise((resolve, reject) => {
            const socket = net.connect(address);
            const timer = setTimeout(() => socket.destroy(new Error('timed out')), timeoutMs);
            const failed = (error: NodeJS.ErrnoException) => {
                clearTimeout(timer);
                // Left by one that crashed. Pipes on Windows go away by themselves.
                if (process.platform !== 'win32' && error.code === 'ECONNREFUSED') fs.unlink(address, () => {});
                reject(new LinkError('closed', `Can't connect to usdcc: ${error.message}`));
            };
            socket.once('error', failed);
            socket.once('connect', () => {
                clearTimeout(timer);
                socket.off('error', failed);
                resolve(new Link(socket));
            });
        });
    }

    get isOpen(): boolean {
        return !this.closed;
    }

    onClose(handler: () => void): void {
        if (this.closed) handler();
        else this.closeHandlers.push(handler);
    }

    /** Events that aren't a run's output, such as "plugins-changed". */
    onEvent(handler: (name: string) => void): void {
        this.eventHandlers.push(handler);
    }

    close(): void {
        this.socket.destroy();
        this.ended();
    }

    hello(timeoutMs = 3000): Promise<Hello> {
        return this.request<Hello>('hello', {}, undefined, timeoutMs);
    }
    /** `file` names the code in tracebacks; `line` is the one of the file it starts on. */
    run(params: { code: string; file?: string; name?: string; line?: number; scope?: 'fresh' | 'session' }, onOutput: (event: OutputEvent) => void): Promise<RunResult> {
        return this.request<RunResult>('run', params, onOutput);
    }
    reloadPlugin(params: { id?: string; path?: string }, onOutput: (event: OutputEvent) => void): Promise<ReloadResult> {
        return this.request<ReloadResult>('reloadPlugin', params, onOutput);
    }
    /** Puts the types where an editor can read them, and says where to look for modules. */
    setUpTypes(): Promise<{ executable: string; paths: string[] }> {
        return this.request<{ executable: string; paths: string[] }>('setUpTypes', {});
    }
    /** Whether there was something to stop. Whoever started it. */
    async stop(): Promise<boolean> {
        return (await this.request<{ stopping: boolean }>('stop', {}, undefined, 5000)).stopping;
    }
    async templates(): Promise<Template[]> {
        return (await this.request<{ templates: Template[] }>('templates', {})).templates;
    }
    createPlugin(params: { name: string; template: string; parent: string }, onOutput: (event: OutputEvent) => void): Promise<AddResult> {
        return this.request<AddResult>('createPlugin', params, onOutput);
    }
    /** Takes in a plugin's folder that usdcc doesn't know yet. */
    addPlugin(folder: string, onOutput: (event: OutputEvent) => void): Promise<AddResult> {
        return this.request<AddResult>('addPlugin', { folder }, onOutput);
    }
    async debugListen(): Promise<number> {
        return (await this.request<{ port: number }>('debugListen', {})).port;
    }
    resetSession(): Promise<unknown> {
        return this.request('resetSession', {});
    }
    async plugins(): Promise<PluginInfo[]> {
        return (await this.request<{ plugins: PluginInfo[] }>('plugins', {})).plugins;
    }

    /** No timeout unless given: a script may take as long as it likes. */
    request<T>(method: string, params: object, onOutput?: (event: OutputEvent) => void, timeoutMs?: number): Promise<T> {
        return new Promise<T>((resolve, reject) => {
            if (this.closed) {
                reject(new LinkError('closed', 'usdcc closed the connection.'));
                return;
            }
            const id = ++this.nextId;
            const entry: Pending = { resolve: resolve as (result: unknown) => void, reject, onOutput };
            if (timeoutMs) {
                entry.timer = setTimeout(() => {
                    this.pending.delete(id);
                    reject(new LinkError('timeout', "usdcc didn't answer."));
                }, timeoutMs);
            }
            this.pending.set(id, entry);
            this.socket.write(JSON.stringify({ id, method, params }) + '\n');
        });
    }

    private read(data: Buffer): void {
        let rest = data;
        for (let end = rest.indexOf(10); end >= 0; end = rest.indexOf(10)) {
            const text = Buffer.concat([...this.chunks, rest.subarray(0, end)]).toString('utf8');
            this.chunks = [];
            rest = rest.subarray(end + 1);
            this.line(text);
        }
        if (rest.length) this.chunks.push(rest);
    }

    private line(text: string): void {
        let message: any;
        try {
            message = JSON.parse(text);
        } catch {
            this.close(); // not usdcc
            return;
        }
        if (message === null || typeof message !== 'object') return;
        if (message.event !== undefined) {
            if (message.event === 'output') this.pending.get(message.run)?.onOutput?.(message as OutputEvent);
            else for (const handler of this.eventHandlers) handler(String(message.event));
            return;
        }
        const entry = this.pending.get(message.id);
        if (!entry) return;
        this.pending.delete(message.id);
        clearTimeout(entry.timer);
        if (message.error) entry.reject(new LinkError(String(message.error.code), String(message.error.message)));
        else entry.resolve(message.result);
    }

    private ended(): void {
        if (this.closed) return;
        this.closed = true;
        for (const entry of this.pending.values()) {
            clearTimeout(entry.timer);
            entry.reject(new LinkError('closed', 'usdcc closed the connection.'));
        }
        this.pending.clear();
        for (const handler of this.closeHandlers) handler();
    }
}
