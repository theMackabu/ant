// Compiled global reads check the global object's shape, and that no global
// lexical was declared since compilation, then load the slot directly.
// Anything that changes the answer must miss and take the generic path.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');

function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

globalThis.G = 1;
function read() { return G; }
function readOrUndef() { return typeof G === 'undefined' ? 'missing' : G; }
function inner() { return G; }
function outer() { return inner() + 0; }
for (let i = 0; i < 5000; i++) { read(); readOrUndef(); outer(); }

same(read(), 1, 'plain hit');
globalThis.G = 5;
same(read(), 5, 'reassigned');
same(outer(), 5, 'inlined reassigned');

// other globals added after compilation
for (let k = 0; k < 40; k++) globalThis['extra' + k] = k;
same(read(), 5, 'after adding globals');
globalThis.G = 6;
same(read(), 6, 'reassigned after adding globals');

// accessor, then a non-writable data property
Object.defineProperty(globalThis, 'G', { get() { return 'getter'; }, configurable: true });
same(read(), 'getter', 'getter');
same(outer(), 'getter0', 'inlined getter');
Object.defineProperty(globalThis, 'G', { value: 7, writable: false, configurable: true });
same(read(), 7, 'non-writable data');

// deleted
delete globalThis.G;
assert.throws(read, ReferenceError, 'deleted global throws');
same(readOrUndef(), 'missing', 'typeof deleted global');
globalThis.G = 8;
same(read(), 8, 'readded');

// Math replaced and restored
function abs(x) { return Math.abs(x); }
for (let i = 0; i < 5000; i++) abs(-i);
const RealMath = Math;
globalThis.Math = { abs: () => 'fake' };
same(abs(-3), 'fake', 'replaced Math');
globalThis.Math = RealMath;
same(abs(-3), 3, 'restored Math');

// the snapshot is what compiled code uses
const child = spawnSync(process.execPath, ['-e', `
globalThis.G = 1;
function read() { return G; }
let s = 0;
for (let i = 0; i < 20000; i++) s += read();
console.log('total ' + s);
`], { encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' }, maxBuffer: 64 * 1024 * 1024, timeout: 30000 });
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
const fn = child.stderr.match(/jit_read_[^\n]*:\s*func[\s\S]*?endfunc/);
assert.ok(fn, 'missing compilation for read');
assert.match(fn[0], /gg_snapshot_global/, 'global read uses the compile-time snapshot');

// a REPL `let` declared after compilation shadows the global property
const script = ['/usr/bin/script', '/bin/script'].find(p => fs.existsSync(p));
if (script) {
  const input = 'globalThis.G = 1\nfunction read() { return G; }\nfor (let i = 0; i < 20000; i++) read()\nread()\nlet G = 2\nread()\n.exit\n';
  const args = process.platform === 'darwin'
    ? ['-q', '/dev/null', process.execPath]
    : ['-qec', JSON.stringify(process.execPath), '/dev/null'];
  const repl = spawnSync(script, args, { input, encoding: 'utf8', timeout: 30000, env: { ...process.env, NO_COLOR: '1' } });
  const values = repl.stdout.replace(/\x1b\[[0-9;?]*[a-zA-Z]/g, '').split(/\r?\n/).filter(l => /^\d+$/.test(l));
  assert.deepStrictEqual(values.slice(-2), ['1', '2'], `REPL lexical shadow: ${repl.stdout.slice(-400)}`);
}

console.log('PASS compiled global reads follow every change');
