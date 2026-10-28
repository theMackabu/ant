const assert = require('node:assert');

function reference(a, b) {
  if (b === 0) return NaN;
  const x = Math.abs(a), y = Math.abs(b);
  const r = x - Math.floor(x / y) * y;
  return a < 0 ? -r : r;
}

function same(actual, expected, label) {
  if (!Object.is(actual, expected)) assert.fail(`${label}: ${Object.is(actual, -0) ? '-0' : actual} !== ${Object.is(expected, -0) ? '-0' : expected}`);
}

function byVar(a, b) { return a % b; }
function by7(a) { return a % 7; }
function byMinus7(a) { return a % -7; }
function by1(a) { return a % 1; }
function byMinus1(a) { return a % -1; }
function by16(a) { return a % 16; }
function byMinus16(a) { return a % -16; }
function byMin(a) { return a % -2147483648; }
function by1024(a) { return a % 1024; }
function sumVar(n, k) { let s = 0; for (let i = 0; i < n; i++) s = (s + i % k) | 0; return s; }
function sumPow(n) { let s = 0; for (let i = 0; i < n; i++) s = (s + i % 32) | 0; return s; }
function chain(n, k) { let x = 1; for (let i = 0; i < n; i++) x = (x * 31 + i) % k; return x; }

const fixed = [[by7, 7], [byMinus7, -7], [by1, 1], [byMinus1, -1], [by16, 16], [byMinus16, -16], [byMin, -2147483648], [by1024, 1024]];
const dividends = [0, 1, 2, 7, 15, 16, 17, 1023, 1024, 1025, 2147483647, -1, -2, -7, -8, -15, -16, -17, -1024, -1025, -2147483647, -2147483648];
const divisors = [1, -1, 2, 3, -3, 7, -7, 16, -16, 1000003, 2147483647, -2147483648, 0];

if (process.argv[2] === 'child') {
  for (let r = 0; r < 4000; r++) {
    const a = (r * 7919) % 100000;
    same(byVar(a, (r % 97) + 1), reference(a, (r % 97) + 1), 'warm var');
    for (const [f, d] of fixed) same(f(a), reference(a, d), `warm ${f.name}`);
  }
  for (let r = 0; r < 4000; r++) {
    const a = -((r * 7919) % 100000) - 1;
    const b = (r & 1 ? -1 : 1) * ((r % 97) + 2);
    if (a % b !== 0) same(byVar(a, b), reference(a, b), 'warm negative var');
  }
  for (const b of divisors) for (const a of dividends) same(byVar(a, b), reference(a, b), `${a} % ${b}`);
  for (const a of dividends) for (const [f, d] of fixed) same(f(a), reference(a, d), `${f.name}(${a})`);
  same(sumVar(1000000, 13), sumReference(1000000, 13), 'sumVar');
  same(sumPow(1000000), sumReference(1000000, 32), 'sumPow');
  same(chain(1000000, 10007), chainReference(1000000, 10007), 'chain');
  for (let r = 0; r < 3; r++) for (const a of dividends) for (const b of divisors) same(byVar(a, b), reference(a, b), `again ${a} % ${b}`);
  console.log('child ok');
} else {
  const { spawnSync } = require('node:child_process');
  for (const mode of ['default', 'opt:force', 'opt:verify', 'opt:off']) {
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: mode === 'default' ? '' : mode, NO_COLOR: '1' },
      timeout: 120000,
      maxBuffer: 16 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${mode}: ${child.error || child.stdout + child.stderr}`);
    assert.match(child.stdout, /child ok/, `${mode}: ${child.stdout}${child.stderr}`);
  }
  console.log('ok');
}

function sumReference(n, k) {
  let s = 0;
  for (let i = 0; i < n; i++) s = (s + reference(i, k)) | 0;
  return s;
}

function chainReference(n, k) {
  let x = 1;
  for (let i = 0; i < n; i++) x = reference(x * 31 + i, k);
  return x;
}
