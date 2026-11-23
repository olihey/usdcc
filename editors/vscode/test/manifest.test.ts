// The schema of usdcc-plugin.json takes every manifest in the repository, and not what usdcc doesn't.
import assert from 'node:assert/strict';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { test } from 'node:test';
import Ajv from 'ajv';

const extension = path.resolve(__dirname, '../..');
const plugins = path.resolve(extension, '../../src/plugins');
const validate = new Ajv({ allErrors: true }).compile(JSON.parse(fs.readFileSync(path.join(extension, 'schemas/usdcc-plugin.schema.json'), 'utf8')));

test('every manifest here', () => {
    const manifests = fs.readdirSync(plugins).map((name) => path.join(plugins, name, 'usdcc-plugin.json')).filter((file) => fs.existsSync(file));
    assert.ok(manifests.length >= 2);
    for (const file of manifests) assert.ok(validate(JSON.parse(fs.readFileSync(file, 'utf8'))), `${file}: ${JSON.stringify(validate.errors)}`);
});

test('what usdcc takes and refuses', () => {
    const good = { id: 'a.b', name: 'A', version: '1', api: 1 };
    const built = { platform: 'windows-x64', compiler: 'MSVC', qt: '6', usd: '2608', usdcc: '1' };
    for (const extra of [{}, { python: { module: 'a_b', requires: ['numpy>=2'], indexes: ['https://x/simple'] } }, { usd: ['usd'], requires: ['c'] }, { native: { library: 'a', built_for: built } }]) {
        assert.ok(validate({ ...good, ...extra }), JSON.stringify(validate.errors));
    }
    for (const bad of [{ id: 'a b' }, { api: 2 }, { name: '' }, { python: { requires: ['x'] } }, { python: { module: 'a', indexes: ['https://x'] } }, { python: { module: 'a', requires: ['x'], indexes: ['http://x'] } }, { native: { library: 'a' } }, { pyhton: {} }]) {
        assert.ok(!validate({ ...good, ...bad }), JSON.stringify(bad));
    }
    assert.ok(!validate({ id: 'a', name: 'A', version: '1' }), 'no api');
});

test('snippets and the manifest of the extension name files that are there', () => {
    const manifest = JSON.parse(fs.readFileSync(path.join(extension, 'package.json'), 'utf8'));
    for (const file of [manifest.contributes.snippets[0].path, manifest.contributes.jsonValidation[0].url, manifest.contributes.viewsContainers.activitybar[0].icon]) {
        assert.ok(fs.existsSync(path.join(extension, file)), file);
    }
    for (const snippet of Object.values<any>(JSON.parse(fs.readFileSync(path.join(extension, 'snippets/usdcc.json'), 'utf8')))) assert.ok(snippet.prefix && snippet.body.length);
});
