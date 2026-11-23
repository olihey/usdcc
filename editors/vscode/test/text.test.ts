import assert from 'node:assert/strict';
import { test } from 'node:test';
import { dedent, footer, innermost, mergedPaths, outputText, scriptPath, selectedLines, tracebackLinks } from '../src/text';

test('dedent', () => {
    assert.equal(dedent(['    a', '', '      b']), 'a\n\n  b\n');
    assert.equal(dedent(['\ta', '\t\tb']), 'a\n\tb\n');
    assert.equal(dedent(['a', '  b']), 'a\n  b\n');
    assert.equal(dedent(['  a', ' ', '  b']), 'a\n\nb\n');
    assert.equal(dedent(['']), '\n');
});

test('the lines of a selection', () => {
    assert.deepEqual(selectedLines(3, 3, 0), [3, 3]);
    assert.deepEqual(selectedLines(3, 5, 0), [3, 4]);
    assert.deepEqual(selectedLines(3, 5, 1), [3, 5]);
});

test('paths', () => {
    assert.equal(scriptPath('c:\\shots\\a.py'), 'C:/shots/a.py');
    assert.equal(scriptPath('/shots/a.py'), '/shots/a.py');
});

test('output and footer', () => {
    const event = { event: 'output' as const, run: 1, level: 'info', text: 'x' };
    assert.equal(outputText({ ...event, source: 'python', category: 'python.stdout' }), 'x');
    assert.equal(outputText({ ...event, source: 'usd', category: 'TF_WARN' }), 'TF_WARN: x');
    assert.equal(footer({ ok: true, ms: 12, undoSteps: 1 }), '    done in 12 ms, 1 undo step');
    assert.equal(footer({ ok: false, ms: 2, undoSteps: 0 }), '    failed after 2 ms');
});

test('links in a traceback', () => {
    const text = 'Traceback (most recent call last):\r\n  File "C:/a b.py", line 12, in f\r\n    x\r\n  File "/s/a.py", line 3\n  File "<string>", line 1\n';
    assert.deepEqual(tracebackLinks(text), [
        { line: 1, start: 2, end: 27, file: 'C:/a b.py', target: 12 },
        { line: 3, start: 2, end: 24, file: '/s/a.py', target: 3 },
    ]);
});

test('paths of ours and of the workspace', () => {
    assert.deepEqual(mergedPaths(['a', 'b'], ['c', 'a']), ['a', 'b', 'c']);
});

test('the innermost frame of ours', () => {
    const frames = [
        { file: 'a.py', line: 1, function: '<module>' },
        { file: 'a.py', line: 7, function: 'f' },
        { file: 'pxr.py', line: 9, function: 'g' },
    ];
    assert.equal(innermost(frames, (file) => file === 'a.py')?.line, 7);
    assert.equal(innermost(frames, () => false), undefined);
});
