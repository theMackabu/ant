'use strict';

const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

if (process.platform === 'win32') {
  console.log('process stdin sources test skipped on win32');
  process.exit(0);
}

const child = path.join(__dirname, 'fixtures', 'process_stdin_sources_child.cjs');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-stdin-'));

const lines = [];
for (let i = 0; i < 6000; i++) lines.push(`line ${i} ${'x'.repeat(i % 50)} é€😀`);
const big = lines.join('\n') + '\n';
const small = 'hello\nworld\n';

const files = {
  big: path.join(dir, 'big.txt'),
  small: path.join(dir, 'small.txt'),
  empty: path.join(dir, 'empty.txt'),
};
fs.writeFileSync(files.big, big);
fs.writeFileSync(files.small, small);
fs.writeFileSync(files.empty, '');

function run(mode, source, content) {
  const file = source === 'devnull' ? '/dev/null' : files[content];
  const script = source === 'pipe'
    ? 'cat "$3" | exec "$0" "$1" "$2"'
    : 'exec "$0" "$1" "$2" < "$3"';

  const result = spawnSync('/bin/sh', ['-c', script, process.execPath, child, mode, file], {
    stdio: ['ignore', 'pipe', 'pipe'],
    timeout: 20000,
    env: { ...process.env, FORCE_COLOR: '0', NO_COLOR: '1' },
  });

  const label = `${mode} ${source} ${content}`;
  assert.strictEqual(result.status, 0, `${label}: status ${result.status} signal ${result.signal}\n${result.stderr}`);
  const out = String(result.stdout).trim();
  assert.ok(out.length > 0, `${label}: no output\n${result.stderr}`);
  return JSON.parse(out.split('\n').pop());
}

const sources = [];
for (const content of ['big', 'small', 'empty']) for (const source of ['file', 'pipe']) sources.push([source, content]);
sources.push(['devnull', 'empty']);

const textOf = { big, small, empty: '' };
const linesOf = text => (text === '' ? [] : text.replace(/\n$/, '').split('\n'));

for (const [source, content] of sources) {
  const label = `${source}/${content}`;
  const expected = textOf[content];

  const data = run('data', source, content);
  assert.strictEqual(data.bytes, Buffer.byteLength(expected), label);
  assert.strictEqual(data.text, expected, label);
  assert.strictEqual(data.buffers, true, label);

  assert.deepStrictEqual(run('encoding', source, content), { text: expected, strings: true }, label);
  assert.deepStrictEqual(run('iterate', source, content), { text: expected }, label);
  assert.deepStrictEqual(run('readable', source, content), { text: expected }, label);
  assert.deepStrictEqual(run('pause', source, content), { text: expected }, label);
  assert.deepStrictEqual(run('readline', source, content), { lines: linesOf(expected) }, label);
  assert.deepStrictEqual(run('syncFd', source, content), { text: expected }, label);
  assert.deepStrictEqual(run('syncDevStdin', source, content), { text: expected }, label);
  assert.deepStrictEqual(run('untouched', source, content), { ok: true }, label);
  if (content !== 'empty') assert.deepStrictEqual(run('iterateBreak', source, content), { first: true }, label);
}

fs.rmSync(dir, { recursive: true, force: true });
console.log('process stdin sources tests passed');
