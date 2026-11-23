// Keeps connected to a running usdcc: finds it, reconnects when one starts, and knows what
// it has open. Nothing of VS Code here.
import { API, App, Hello, Link, LinkError } from './link';

export type State =
    | { kind: 'none' } // not running
    | { kind: 'several'; apps: { app: App; hello: Hello }[] } // and none picked
    | { kind: 'incompatible'; hello: Hello }
    | { kind: 'connected'; hello: Hello };

export class Connection {
    state: State = { kind: 'none' };
    private link?: Link;
    private picked?: number;
    private polling?: Promise<void>;
    private disposed = false;

    /** `find` and `changed` are called from poll(), which the owner calls every now and then. */
    constructor(private readonly find: () => Promise<App[]>, private readonly changed: (state: State) => void, private readonly event: (name: string) => void = () => {}) {}

    /** The link to use, if there's one. */
    get current(): Link | undefined {
        return this.state.kind === 'connected' && this.link?.isOpen ? this.link : undefined;
    }

    /** Asks the one connected to what it has open, or looks for one. One at a time. */
    poll(): Promise<void> {
        this.polling ??= this.pollOnce().finally(() => (this.polling = undefined));
        return this.polling;
    }

    /** The running apps, to pick from. */
    async apps(): Promise<{ app: App; hello: Hello }[]> {
        const found: { app: App; hello: Hello }[] = [];
        for (const app of await this.find()) {
            try {
                const link = await Link.connect(app.address);
                try {
                    found.push({ app, hello: await link.hello() });
                } finally {
                    link.close();
                }
            } catch (error) {
                if (!(error instanceof LinkError)) throw error;
            }
        }
        return found;
    }

    /** Uses this one from now on, until it quits. */
    async pick(pid: number): Promise<void> {
        await this.polling;
        this.picked = pid;
        this.drop();
        await this.poll();
    }

    /** Lets go, as after a change of which apps to look for. */
    async restart(): Promise<void> {
        await this.polling;
        this.picked = undefined;
        this.drop();
        await this.poll();
    }

    dispose(): void {
        this.disposed = true;
        this.drop();
    }

    private drop(): void {
        const link = this.link;
        this.link = undefined;
        link?.close();
    }

    private set(state: State): void {
        if (this.disposed) return;
        const same = JSON.stringify(state) === JSON.stringify(this.state);
        this.state = state;
        if (!same) this.changed(state);
    }

    private async pollOnce(): Promise<void> {
        if (this.link?.isOpen) {
            try {
                this.take(await this.link.hello());
                return;
            } catch (error) {
                if (!(error instanceof LinkError)) throw error;
                // A pipe's thread answers hello whatever the app does, so one that doesn't is
                // stuck, at a C++ breakpoint say. A run on it may still end: keep it.
                if (error.code === 'timeout') return;
            }
        }
        this.drop();
        let found = await this.apps();
        if (this.picked !== undefined) {
            const picked = found.filter(({ app }) => app.pid === this.picked);
            if (picked.length) found = picked;
            else this.picked = undefined; // it quit
        }
        if (found.length === 0) return this.set({ kind: 'none' });
        if (found.length > 1) return this.set({ kind: 'several', apps: found });
        try {
            const link = await Link.connect(found[0].app.address);
            if (this.disposed) return link.close();
            this.link = link;
            link.onEvent((name) => {
                if (this.link === link) this.event(name);
            });
            link.onClose(() => {
                if (this.link === link) {
                    this.link = undefined;
                    this.set({ kind: 'none' });
                }
            });
            this.take(await link.hello());
        } catch (error) {
            if (!(error instanceof LinkError)) throw error;
            this.drop();
            this.set({ kind: 'none' });
        }
    }

    private take(hello: Hello): void {
        this.set(hello.api === API ? { kind: 'connected', hello } : { kind: 'incompatible', hello });
    }
}
