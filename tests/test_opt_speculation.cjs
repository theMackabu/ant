// Speculation of the optimizing tier on dense elements and int32 math (step 6b): element accesses that exit when the
// array stops being dense, holds a hole or the index leaves the length, operations made wrapping by ranges and
// truncation, unsigned comparisons, Math.imul/min/max, booleans in bitwise operations, double feedback. Each case
// warms a function up on the inputs its speculation expects, then feeds it the ones that must exit. Every mode prints
// what --jitless prints: the default, ANT_DEBUG=opt:verify, opt:force and opt:off.
const assert = require('node:assert');

const warm = 3000;

function show(label, value) {
  console.log(label, JSON.stringify(value, (k, v) => (typeof v === 'number' && Object.is(v, -0) ? '-0' : v)));
}

// am3 of bench-v8 crypto: reads two arrays, writes one, int32 math on 14-bit digits.
function am3(src, x, dst, j, c, n) {
  let i = 0;
  const xl = x & 0x3fff, xh = x >> 14;
  while (--n >= 0) {
    let l = src[i] & 0x3fff;
    const h = src[i++] >> 14;
    const m = xh * l + h * xl;
    l = xl * l + ((m & 0x3fff) << 14) + dst[j] + c;
    c = (l >> 28) + (m >> 14) + xh * h;
    dst[j++] = l & 0xfffffff;
  }
  return c;
}

function digits(n, seed) {
  const a = [];
  for (let i = 0; i < n; i++) a.push((seed = (seed * 1103515245 + 12345) & 0xfffffff));
  return a;
}

function readAt(a, i) {
  return a[i];
}

function writeAt(a, i, v) {
  a[i] = v;
  return a.length;
}

function sumMasked(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = (s + (a[i] & 0xffff)) | 0;
  return s;
}

function scaleInPlace(a, n, f) {
  for (let i = 0; i < n; i++) a[i] = a[i] * f;
  return a[n - 1];
}

// The array changes under the loop: it goes sparse, gets a hole, shrinks, or gets a non-number.
function churn(a, n, what) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (i === 5) {
      if (what === 1) a[100000] = 1;
      else if (what === 2) delete a[7];
      else if (what === 3) a.length = 6;
      else if (what === 4) a[8] = 'x';
      else if (what === 5) Object.freeze(a);
    }
    s += a[i] | 0;
    if (what === 5 && i < 4) a[i] = i;
  }
  return [s, a.length];
}

// Ranges: products of masked values, sums of shifted values and increments after bounds checks wrap.
function ranges(a, x, y) {
  const p = (x & 0x3fff) * (y & 0x3fff);
  const q = (x >> 14) + (y >> 14);
  const r = (x >> 14) * (y & 0x3fff);
  let s = 0;
  for (let i = 0; i < a.length; i++) s = (s + a[i] + (i + 1)) | 0;
  return [p, q, r, s];
}

function truncatedSum(a, b, c) {
  return (a + b + c) | 0;
}

function hash(a, n) {
  let h = 0;
  for (let i = 0; i < n; i++) h = (h + a[i] + (h << 5)) | 0;
  return h;
}

function product(a, b) {
  return a * b;
}

function unsigned(a, b, out) {
  out[0] = (a >>> 0) < (b >>> 0);
  out[1] = (a >>> 0) >= (b >>> 0);
  out[2] = (a >>> 0) > 0x80000000;
  out[3] = (a >>> 0) <= 3;
  return out;
}

function imul(a, b) {
  return Math.imul(a, b);
}

function imulLoop(a, n) {
  let x = 0x811c9dc5 | 0;
  for (let i = 0; i < n; i++) x = Math.imul(x ^ a[i], 16777619);
  return x;
}

function min2(a, b) {
  return Math.min(a, b);
}

function max3(a, b, c) {
  return Math.max(a, b, c);
}

function boolBits(a, b, c, d) {
  return [(a < b) & (c < d), (a < b) | 0, ~(c < d), (a < b) << 3, (a === b) ^ 1];
}

function halfBelow(a, n, limit) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    const x = a[i] / 2;
    if (x < limit) c = (c + 1) | 0;
  }
  return c;
}

function mixedArith(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + a[i] * 0.5 - (a[i] >> 1);
  return s;
}

// Entered by on-stack replacement with locals made before the loop.
function osrDigits(x, n) {
  const xl = x & 0x3fff, xh = x >> 14;
  let s = 0;
  for (let i = 0; i < n; i++) s = (s + xl * (i & 0x3fff) + ((xh * 3) | 0)) | 0;
  return s;
}

function runCases() {
  const src = digits(40, 7), dst0 = digits(48, 11);
  for (let k = 0; k < warm; k++) am3(src, 0x1234567, dst0.slice(), 3, 0, 37);
  const dst = dst0.slice();
  show('am3', [am3(src, 0x1234567, dst, 3, 0, 37), dst.slice(0, 8), am3(src, -0x1234567, dst0.slice(), 0, 5, 40)]);
  const spread = src.slice();
  spread[100000] = 1;
  show('am3 sparse', am3(spread, 5, dst0.slice(), 0, 0, 12));
  for (let k = 0; k < warm; k++) am3(src, 0x1234567, dst0.slice(), 3, 0, 37);
  const short = src.slice(0, 10);
  show('am3 past end', am3(short, 0xfffffff, dst0.slice(), 0, 0, 20));
  const holey = src.slice();
  delete holey[3];
  show('am3 hole', am3(holey, 99, dst0.slice(), 0, 0, 10));
  show('am3 doubles', am3([1.5, 2 ** 40, -0, NaN, 7], 3, dst0.slice(), 0, 0, 5));
  show('am3 strings', am3(['12', 'x', 3], 3, ['5', 6, 7], 0, 0, 3));
  show('am3 dst holes', am3(src, 3, [1, , 3, , 5], 0, 0, 5));

  const dense = [1, 2, 3, 4, 5, 6, 7, 8];
  for (let k = 0; k < warm; k++) readAt(dense, k & 7);
  Array.prototype[9] = 'proto';
  const fromProto = readAt(dense, 9);
  delete Array.prototype[9];
  show('read at', [readAt(dense, 7), readAt(dense, 8), readAt(dense, -1), readAt(dense, 2 ** 31), readAt([, 1], 0), fromProto,
    readAt(dense, 1.5), readAt('abc', 1), readAt({ 0: 'o' }, 0)]);
  const truncated = [1, 2, 3, 4, 5, 6, 7, 8];
  truncated.length = 6;
  show('read past truncation', [readAt(truncated, 6), readAt(truncated, 7), truncated.length]);
  for (let k = 0; k < warm; k++) writeAt([1, 2, 3], k % 3, k);
  const shrunk = [1, 2, 3, 4, 5];
  shrunk.length = 3;
  show('write past truncation', [writeAt(shrunk, 3, 9), shrunk, writeAt(shrunk, 4, 8), shrunk]);
  const grow = [1, 2, 3], frozen = Object.freeze([1, 2]), sparse = [1, 2];
  show('write at', [writeAt(grow, 3, 4), writeAt(grow, 9, 5), grow, writeAt(frozen, 0, 9), frozen, writeAt(sparse, 1e6, 1),
    writeAt([1, , 3], 1, 2), writeAt([1, 2], -1, 0)]);

  const big = digits(64, 3);
  for (let k = 0; k < warm; k++) sumMasked(big, 64);
  show('sum masked', [sumMasked(big, 64), sumMasked(big, 70), sumMasked([0x7fffffff, -1, 2 ** 32 + 5, -0, 1.5], 5)]);
  for (let k = 0; k < warm; k++) scaleInPlace(big.slice(), 64, 1);
  show('scale', [scaleInPlace(big.slice(), 64, 3), scaleInPlace([1, 2, 3], 3, 0.5), scaleInPlace([1, , 3], 3, 2)]);
  for (let k = 0; k < warm; k++) churn(digits(12, k), 12, 0);
  show('churn', [churn(digits(12, 1), 12, 0), churn(digits(12, 1), 12, 1), churn(digits(12, 1), 12, 2), churn(digits(12, 1), 12, 3),
    churn(digits(12, 1), 12, 4), churn(digits(12, 1), 12, 5)]);

  for (let k = 0; k < warm; k++) ranges([1, 2, 3], k * 977, k * 131);
  show('ranges', [ranges([1, 2, 3], 0x7fffffff, 0x7fffffff), ranges([0x7fffffff, 0x7fffffff], -1, -0x80000000),
    ranges([1.5, 2], 0x3fff, -0x3fff), ranges([], 0, 0)]);
  for (let k = 0; k < warm; k++) truncatedSum(k, k, k);
  show('truncated sum', [truncatedSum(0x7fffffff, 0x7fffffff, 2), truncatedSum(-0x80000000, -1, 0), truncatedSum(1.5, 2.5, 0),
    truncatedSum(2 ** 53, 1, 1), truncatedSum('1', 2, 3)]);
  const words = [];
  for (let i = 0; i < 100; i++) words.push((i * 2654435761) | 0);
  for (let k = 0; k < warm; k++) hash(words, 100);
  show('hash', [hash(words, 100), hash([2 ** 31, -0, 0.5, NaN], 4)]);
  for (let k = 0; k < warm; k++) product(k, 3);
  show('product', [product(-5, 0), product(0, -5), product(0x10000, 0x10000), product(-0x8000, 0x10000), product(1.5, 2)]);

  for (let k = 0; k < warm; k++) unsigned(k, k >> 1, [0, 0, 0, 0]);
  show('unsigned', [unsigned(-1, 5, [0, 0, 0, 0]), unsigned(0x7fffffff, -0x80000000, [0, 0, 0, 0]), unsigned(3, 3, [0, 0, 0, 0]),
    unsigned(2 ** 32 + 2, 1, [0, 0, 0, 0]), unsigned(-0.5, 1.5, [0, 0, 0, 0])]);

  for (let k = 0; k < warm; k++) imul(k, 7);
  show('imul', [imul(0x7fffffff, 3), imul(-5, 12), imul(2.5, 3), imul('7', 2), imul(-0, 4), imul(4294967295, 5), imul(NaN, 2),
    imul(undefined, 2), imul(2 ** 31, 2)]);
  for (let k = 0; k < warm; k++) imulLoop(words, 100);
  show('imul loop', [imulLoop(words, 100), imulLoop([1.5, 'x', null], 3)]);
  show('imul arity', [Math.imul(3), Math.imul(), Math.imul(3, 4, 5)]);
  for (let k = 0; k < warm; k++) min2(k, 50);
  show('min', [min2(3, -3), min2(-0, 0), min2(0, -0), min2(NaN, 1), min2(2.5, 1), min2('4', 5), min2(-Infinity, 0)]);
  for (let k = 0; k < warm; k++) max3(k, k * 0.5, 3.25);
  show('max', [max3(-0, 0, -1), max3(0, -0, -1), max3(1, NaN, 3), max3(-Infinity, -1e308, -5), max3('9', 2, 3), max3(1, 2, {})]);
  show('min max arity', [Math.min(), Math.max(), Math.min(1, 2, 3, 4, 5, 6, 7, 8, 9, -10), Math.max(...[1, 5, 2])]);

  for (let k = 0; k < warm; k++) boolBits(k, 5, 3, k);
  show('bool bits', [boolBits(1, 2, 3, 4), boolBits(2, 1, 4, 3), boolBits(NaN, 1, 0, -0), boolBits('a', 'b', 1, 2)]);

  const evens = [];
  for (let i = 0; i < 1000; i++) evens.push(i * 2);
  for (let k = 0; k < warm / 10; k++) halfBelow(evens, 1000, 300);
  const odds = evens.map((v) => v + 1);
  show('half below', [halfBelow(evens, 1000, 300), halfBelow(odds, 1000, 300.25), halfBelow([-0, NaN, 1e300], 3, 1)]);
  const reals = [];
  for (let i = 0; i < 64; i++) reals.push(i + 0.25);
  for (let k = 0; k < warm; k++) mixedArith(reals, 64);
  show('mixed arith', [mixedArith(reals, 64), mixedArith([1, 2, 3], 3), mixedArith([2 ** 31, -0], 2)]);

  show('osr', [osrDigits(0x12345678, 200000), osrDigits(-1, 100000), osrDigits(0x3fff, 3)]);
}

const COMPILED = ['am3', 'readAt', 'writeAt', 'sumMasked', 'scaleInPlace', 'ranges', 'hash', 'unsigned', 'imul', 'imulLoop', 'min2', 'max3',
  'boolBits', 'halfBelow', 'mixedArith', 'osrDigits'];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of COMPILED)
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
      for (const [fn, kind] of [['am3', 'OutOfBounds'], ['am3', 'BadElements'], ['readAt', 'OutOfBounds'], ['writeAt', 'OutOfBounds'],
        ['imul', 'NotInt32'], ['min2', 'NotInt32']])
        assert.match(child.stderr, new RegExp(`jit: bailout .*func=${fn} .*reason=opt-exit kind=${kind}`), `${name}: ${fn} must exit (${kind})`);
    }
    console.log(`PASS opt speculation ${name}`);
  }
}
