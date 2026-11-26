const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-parseargs-'));
const script = path.join(dir, 'pa.cjs');
fs.writeFileSync(script, `
const { parseArgs } = require('node:util');
console.log(JSON.stringify(parseArgs({ allowPositionals: true, options: { foo: { type: 'string' }, v: { type: 'boolean', short: 'v' } } })));
`);

const run = args => {
  const r = spawnSync(process.execPath, args, { encoding: 'utf8' });
  assert.strictEqual(r.status, 0, r.stderr);
  return JSON.parse(r.stdout.trim().split('\n').pop());
};

try {
  const file = run([script, 'a', '--foo', 'bar', '-v', 'b']);
  assert.deepStrictEqual(file.positionals, ['a', 'b']);
  assert.strictEqual(file.values.foo, 'bar');
  assert.strictEqual(file.values.v, true);

  assert.deepStrictEqual(run([script]).positionals, []);

  const code = "const { parseArgs } = require('node:util'); console.log(JSON.stringify(parseArgs({ allowPositionals: true, options: { foo: { type: 'string' } } })))";
  const ev = run(['-e', code, 'a', '--foo', 'bar', 'b']);
  assert.deepStrictEqual(ev.positionals, ['a', 'b']);
  assert.strictEqual(ev.values.foo, 'bar');

  const pre = path.join(dir, 'pre.cjs');
  fs.writeFileSync(pre, code.replace('--foo', ''));
  assert.deepStrictEqual(run(['-r', pre, '-e', '0', 'a', 'q']).positionals, ['a', 'q']);

  const shadow = "var __filename = 'x'; " + code;
  assert.deepStrictEqual(run(['-e', shadow, 'a', 'b']).positionals, ['a', 'b']);

  const fake = path.join(dir, 'fake.cjs');
  fs.writeFileSync(fake, "globalThis.__filename = '[eval]';\n" + fs.readFileSync(script, 'utf8'));
  assert.deepStrictEqual(run([fake, 'a']).positionals, ['a']);

  const printed = spawnSync(process.execPath, ['-p', code.replace(/console\.log\((.*)\)$/, '$1'), 'a', 'b'], { encoding: 'utf8' });
  assert.strictEqual(printed.status, 0, printed.stderr);
  assert.deepStrictEqual(JSON.parse(printed.stdout.trim().split('\n')[0]).positionals, ['a', 'b']);

  fs.writeFileSync(script + '.null.cjs', "const { parseArgs } = require('node:util');\nconsole.log(JSON.stringify(parseArgs({ args: null, allowPositionals: true })));\n");
  assert.deepStrictEqual(run([script + '.null.cjs', 'a']).positionals, ['a']);

  console.log('ok');
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}
