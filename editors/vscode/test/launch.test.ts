import assert from 'node:assert/strict';
import * as fs from 'node:fs';
import * as os from 'node:os';
import * as path from 'node:path';
import { test } from 'node:test';
import { LaunchOptions, Started, findProgram, launchArguments, launchEnvironment } from '../src/launch';

const options: LaunchOptions = { program: '', profile: 'p', pluginPath: ['C:/a', 'C:/b'], stage: 'x.usda', args: ['--no-startup-scripts'], cwd: os.tmpdir(), env: { ADDED: '1', GONE: null } };

test('arguments and environment', () => {
    assert.deepEqual(launchArguments(options), ['--plugin-path', 'C:/a', '--plugin-path', 'C:/b', '--no-startup-scripts', 'x.usda']);
    const base = { KEPT: '1', GONE: '1', ELECTRON_RUN_AS_NODE: '1', VSCODE_PID: '1', USDCC_TEST_PROFILE: 'other' };
    assert.deepEqual(launchEnvironment(options, base), { KEPT: '1', ADDED: '1', USDCC_TEST_PROFILE: 'p' });
    assert.equal(launchEnvironment({ ...options, profile: '' }, base).USDCC_TEST_PROFILE, undefined);
});

test('the program, past the launcher', () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'usdcc-launch-'));
    const name = process.platform === 'win32' ? 'usdcc.exe' : 'usdcc';
    const bin = path.join(root, 'usdcc', 'current', 'bin', name);
    fs.mkdirSync(path.dirname(bin), { recursive: true });
    fs.writeFileSync(bin, '');
    const top = path.join(root, 'usdcc', 'current', name);
    fs.writeFileSync(top, '');
    const build = path.join(root, name);
    fs.writeFileSync(build, '');
    assert.equal(findProgram('', { LOCALAPPDATA: root }), bin);
    assert.equal(findProgram('', {}), undefined);
    assert.equal(findProgram(top), bin);
    assert.equal(findProgram(path.dirname(top)), bin, 'a folder');
    assert.equal(findProgram(build), build);
    assert.equal(findProgram(path.join(root, 'none')), undefined);
    fs.rmSync(root, { recursive: true });
});

test('one that ends, and one that cannot start', async () => {
    const ends = Started.start(process.execPath, { ...options, pluginPath: [], stage: '', args: ['-e', 'process.exit(3)'] });
    assert.equal(await ends.waitForApp(() => false, 10000), undefined);
    assert.equal(ends.exited, 3);
    const cannot = Started.start(path.join(os.tmpdir(), 'no such program'), options);
    assert.equal(await cannot.waitForApp(() => false, 10000), undefined);
    assert.ok(cannot.failure);
});
