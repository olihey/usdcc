// The protocol and the connection against a server that plays usdcc.
import assert from 'node:assert/strict';
import * as fs from 'node:fs';
import * as net from 'node:net';
import * as os from 'node:os';
import * as path from 'node:path';
import { after, test } from 'node:test';
import { Connection, State } from '../src/connection';
import { App, Link, LinkError, OutputEvent, findApps, linkFolder } from '../src/link';

type Reply = (message: any, socket: net.Socket) => void;

let count = 0;
const servers: FakeApp[] = [];
after(() => Promise.all(servers.map((server) => server.stop())));

class FakeApp {
    readonly pid = 900000000 + process.pid * 100 + ++count;
    readonly address: string;
    stage = 'a.usda';
    api = 1;
    reply: Reply = () => {};
    private readonly server: net.Server;
    private readonly sockets = new Set<net.Socket>();
    private readonly folder = process.platform === 'win32' ? '' : fs.mkdtempSync(path.join(os.tmpdir(), 'usdcc-'));

    constructor() {
        this.address = path.join(this.folder || '\\\\.\\pipe\\', `usdcc-test-node-editor-${this.pid}`);
        this.server = net.createServer((socket) => {
            this.sockets.add(socket);
            socket.on('close', () => this.sockets.delete(socket));
            socket.on('error', () => {});
            let text = '';
            socket.on('data', (data) => {
                text += data;
                const lines = text.split('\n');
                text = lines.pop()!;
                for (const line of lines) {
                    const message = JSON.parse(line);
                    if (message.method === 'hello') {
                        this.send(socket, { id: message.id, result: { api: this.api, version: '26.11.8', pid: this.pid, stage: this.stage, stagePath: '', busy: false } });
                    } else {
                        this.reply(message, socket);
                    }
                }
            });
        });
        servers.push(this);
    }
    get app(): App {
        return { pid: this.pid, address: this.address };
    }
    send(socket: net.Socket, object: object): void {
        socket.write(JSON.stringify(object) + '\n');
    }
    start(): Promise<this> {
        return new Promise((resolve) => this.server.listen(this.address, () => resolve(this)));
    }
    stop(): Promise<void> {
        for (const socket of this.sockets) socket.destroy();
        return new Promise((resolve) => this.server.close(() => resolve()));
    }
}

// Not narrowed by an earlier check, as it changes meanwhile.
const stateOf = (connection: Connection): any => connection.state;

const until = async (what: () => boolean) => {
    for (let i = 0; i < 200 && !what(); ++i) await new Promise((resolve) => setTimeout(resolve, 10));
    assert.ok(what());
};

test('a run sends its output, then its result', async () => {
    const app = await new FakeApp().start();
    let asked: any;
    app.reply = (message, socket) => {
        asked = message;
        const output = (text: string) => ({ event: 'output', run: message.id, source: 'python', category: 'python.stdout', level: 'info', text });
        // In pieces, splitting a character and a line.
        const bytes = Buffer.from([output('grün'), { event: 'of a newer usdcc' }, output('zwei'), { id: message.id, result: { ok: true, ms: 3, undoSteps: 1 } }].map((each) => JSON.stringify(each) + '\n').join(''));
        const at = bytes.indexOf(Buffer.from('ü')) + 1;
        socket.write(bytes.subarray(0, at));
        setTimeout(() => socket.write(bytes.subarray(at)), 30);
    };
    const link = await Link.connect(app.address);
    const events: OutputEvent[] = [];
    const result = await link.run({ code: 'x', file: 'C:/a.py', line: 4, scope: 'session' }, (event) => events.push(event));
    assert.deepEqual(asked.params, { code: 'x', file: 'C:/a.py', line: 4, scope: 'session' });
    assert.deepEqual(events.map((event) => event.text), ['grün', 'zwei']);
    assert.equal(result.undoSteps, 1);
    link.close();
});

test('answers find their request, whatever the order', async () => {
    const app = await new FakeApp().start();
    const waiting: any[] = [];
    app.reply = (message, socket) => {
        waiting.push(message);
        if (waiting.length === 2) for (const each of waiting.reverse()) app.send(socket, { id: each.id, result: each.method });
    };
    const link = await Link.connect(app.address);
    assert.deepEqual(await Promise.all([link.request('one', {}), link.request('two', {})]), ['one', 'two']);
    link.close();
});

test('refusals, a closed link and no answer', async () => {
    const app = await new FakeApp().start();
    app.reply = (message, socket) => {
        if (message.method === 'run') app.send(socket, { id: message.id, error: { code: 'busy', message: 'usdcc is running a script.' } });
        if (message.method === 'resetSession') socket.destroy();
    };
    const is = (code: string) => (error: unknown) => error instanceof LinkError && error.code === code;
    const link = await Link.connect(app.address);
    await assert.rejects(link.run({ code: '' }, () => {}), is('busy'));
    await assert.rejects(link.request('silence', {}, undefined, 50), is('timeout'));
    await assert.rejects(link.resetSession(), is('closed'));
    await assert.rejects(link.hello(), is('closed'));
    await assert.rejects(Link.connect(app.address + '0'), is('closed'));
});

test('finding apps by the names of their pipes', async () => {
    const app = await new FakeApp().start();
    const folder = process.platform === 'win32' ? linkFolder() : path.dirname(app.address);
    assert.ok((await findApps('node', folder)).some((each) => each.pid === app.pid && each.address === app.address));
    assert.ok(!(await findApps('', folder)).some((each) => each.pid === app.pid), 'not without its profile');
    assert.deepEqual(await findApps('', path.join(os.tmpdir(), 'no such folder')), []);
});

test('connecting, following the stage, and reconnecting', async () => {
    const states: State[] = [];
    let apps: App[] = [];
    const connection = new Connection(async () => apps, (state) => states.push(state));
    await connection.poll();
    assert.equal(stateOf(connection).kind, 'none');

    const first = await new FakeApp().start();
    apps = [first.app];
    await connection.poll();
    assert.ok(stateOf(connection).kind === 'connected' && stateOf(connection).hello.stage === 'a.usda' && connection.current);
    first.stage = 'b.usda';
    await connection.poll();
    await connection.poll(); // no change: not told again
    assert.deepEqual(states.map((state) => state.kind === 'connected' && state.hello.stage), ['a.usda', 'b.usda']);

    await first.stop();
    await until(() => stateOf(connection).kind === 'none');
    assert.equal(connection.current, undefined);
    apps = [first.app, (await new FakeApp().start()).app]; // one is gone, its name left
    await connection.poll();
    assert.equal(stateOf(connection).kind, 'connected');
    connection.dispose();
});

test('several running', async () => {
    const [a, b] = [await new FakeApp().start(), await new FakeApp().start()];
    b.stage = 'b.usda';
    let apps = [a.app, b.app];
    const connection = new Connection(async () => apps, () => {});
    await connection.poll();
    assert.ok(stateOf(connection).kind === 'several' && stateOf(connection).apps.length === 2 && !connection.current);
    await connection.pick(b.pid);
    assert.ok(stateOf(connection).kind === 'connected' && stateOf(connection).hello.pid === b.pid);
    await connection.poll();
    assert.ok(stateOf(connection).kind === 'connected' && stateOf(connection).hello.pid === b.pid, 'stays picked');
    await b.stop();
    apps = [a.app];
    await until(() => stateOf(connection).kind === 'none');
    await connection.poll();
    assert.ok(stateOf(connection).kind === 'connected' && stateOf(connection).hello.pid === a.pid, 'the other, once it quit');
    connection.dispose();
});

test('a newer protocol', async () => {
    const app = await new FakeApp().start();
    app.api = 2;
    const connection = new Connection(async () => [app.app], () => {});
    await connection.poll();
    assert.equal(stateOf(connection).kind, 'incompatible');
    assert.equal(connection.current, undefined, 'nothing is sent to it');
    connection.dispose();
});
