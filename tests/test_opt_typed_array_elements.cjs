const assert = require('node:assert');

const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array];
const values = [0, -0, 1, -1, 1.5, 2.5, -2.5, 127, 128, 255, 256, 300, -129, 32767, 32768, 65535, 65536, 2 ** 31, 2 ** 32 + 5, -(2 ** 31) - 1,
  0.1, 1e-40, 3.4e38, 1e300, Infinity, -Infinity, NaN];

const readSrc = 'let out = []; for (let i = -1; i <= a.length; i++) out.push(a[i]); return out;';
const writeSrc = 'for (let i = 0; i < a.length; i++) a[i] = v[i % v.length]; return a;';
const hotRead = new Function('a', readSrc);
const hotWrite = new Function('a', 'v', writeSrc);
const cold = (src, ...names) => new Function(...names, src);

function same(actual, expected, label) {
  assert.strictEqual(actual.length, expected.length, label);
  for (let i = 0; i < actual.length; i++) assert.ok(Object.is(actual[i], expected[i]), `${label} [${i}]: ${actual[i]} vs ${expected[i]}`);
}

for (const K of kinds) {
  const warm = new K(64);
  for (let i = 0; i < 400; i++) { hotWrite(warm, values); hotRead(warm); }
}

for (const K of kinds) {
  for (let round = 0; round < 50; round++) {
    const hot = hotWrite(new K(values.length * 2), values);
    const ref = cold(writeSrc, 'a', 'v')(new K(values.length * 2), values);
    same(Array.from(hot), Array.from(ref), `${K.name} write`);
    same(hotRead(hot), cold(readSrc, 'a')(ref), `${K.name} read`);
  }
}

function sum(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
const f64 = new Float64Array(256).fill(0.5);
for (let i = 0; i < 2000; i++) assert.strictEqual(sum(f64), 128);

const bytes = new Uint8Array(f64.buffer);
bytes.fill(0xff, 0, 8);
bytes.fill(0xff, 12, 16);
bytes[15] = 0x7f;
const nanSum = sum(f64);
assert.ok(Number.isNaN(nanSum));
assert.ok(Number.isNaN(f64[0]) && typeof f64[0] === 'number');
assert.ok(Number.isNaN(f64[1]) && typeof f64[1] === 'number');
const f32 = new Float32Array(new Uint32Array([0xffffffff, 0x7fc00001, 0x3f800000]).buffer);
function read3(a) { return [a[0], a[1], a[2]]; }
for (let i = 0; i < 3000; i++) {
  const r = read3(f32);
  assert.ok(Number.isNaN(r[0]) && Number.isNaN(r[1]) && r[2] === 1);
}

const shared = new ArrayBuffer(64);
const u8 = new Uint8Array(shared), f64v = new Float64Array(shared), i32 = new Int32Array(shared, 8, 4);
function alias(u, f, k) { u[k] = 0x40; const first = f[0]; u[7] = 0x3f; return [first, f[0]]; }
for (let i = 0; i < 3000; i++) {
  u8.fill(0);
  const [first, second] = alias(u8, f64v, 7);
  assert.strictEqual(first, 2);
  assert.strictEqual(second, new Float64Array(new Uint8Array([0, 0, 0, 0, 0, 0, 0, 0x3f]).buffer)[0]);
}
function offsetRead(v) { let s = 0; for (let i = 0; i < v.length; i++) s += v[i]; return s; }
for (let i = 0; i < 3000; i++) {
  new Int32Array(shared).fill(0);
  i32.set([1, 2, 3, 4]);
  assert.strictEqual(offsetRead(i32), 10);
  assert.deepStrictEqual(Array.from(new Int32Array(shared, 0, 2)), [0, 0]);
}

function readAll(a, n) { const out = []; for (let i = 0; i < n; i++) out.push(a[i]); return out; }
const warmDetach = new Float64Array(8).fill(3);
for (let i = 0; i < 3000; i++) readAll(warmDetach, 8);
const detached = new Float64Array(8).fill(4);
structuredClone(detached.buffer, { transfer: [detached.buffer] });
same(readAll(detached, 3), [undefined, undefined, undefined], 'detached read');
const offsetView = new Float64Array(new ArrayBuffer(64), 16, 4).fill(2);
for (let i = 0; i < 3000; i++) readAll(offsetView, 4);
structuredClone(offsetView.buffer, { transfer: [offsetView.buffer] });
same(readAll(offsetView, 4), [undefined, undefined, undefined, undefined], 'detached offset read');
function writeAll(a, n) { for (let i = 0; i < n; i++) a[i] = 9; return a.length; }
for (let i = 0; i < 3000; i++) writeAll(new Uint8Array(8), 8);
assert.strictEqual(writeAll(detached, 8), 0);

function detachMidway(a, buffer) {
  let s = 0;
  for (let i = 0; i < 8; i++) {
    if (i === 4) structuredClone(buffer, { transfer: [buffer] });
    s += a[i] ?? 100;
  }
  return s;
}
for (let i = 0; i < 3000; i++) {
  const a = new Float64Array(8).fill(1);
  const expected = 4 + 400;
  assert.strictEqual(detachMidway(a, i % 500 === 0 ? a.buffer : new ArrayBuffer(8)), i % 500 === 0 ? expected : 8);
}

const resizable = new ArrayBuffer(64, { maxByteLength: 128 });
const tracking = new Uint8Array(resizable);
function shrinkMidway(a, buffer) {
  let s = 0;
  for (let i = 0; i < 64; i++) {
    if (i === 32 && buffer) buffer.resize(16);
    s += a[i] ?? 1000;
  }
  return s;
}
for (let i = 0; i < 3000; i++) {
  resizable.resize(64);
  tracking.fill(1);
  const shrink = i % 700 === 0;
  assert.strictEqual(shrinkMidway(tracking, shrink ? resizable : null), shrink ? 32 + 32 * 1000 : 64);
}

class Bytes extends Uint8Array {}
function first(a) { return a[0] + a[1]; }
const sub = new Bytes([5, 6]);
const plainArray = [7, 8], proxy = new Proxy(new Uint8Array([1, 2]), { get: (t, k) => (k === '0' ? 40 : Reflect.get(t, k)) });
const objectLike = { 0: 1, 1: 2, length: 2 };
for (let i = 0; i < 4000; i++) {
  assert.strictEqual(first(sub), 11);
  if (i > 3000) {
    assert.strictEqual(first(plainArray), 15);
    assert.strictEqual(first(proxy), 42);
    assert.strictEqual(first(objectLike), 3);
    assert.strictEqual(first(new Uint16Array([300, 400])), 700);
  }
}

function storeMixed(a, v) { a[0] = v; return a[0]; }
const target = new Int16Array(1);
for (let i = 0; i < 3000; i++) assert.strictEqual(storeMixed(target, i), i);
assert.strictEqual(storeMixed(target, '12'), 12);
assert.strictEqual(storeMixed(target, { valueOf: () => 70000 }), 70000 - 65536);
assert.strictEqual(storeMixed(target, 2.9), 2);
assert.strictEqual(storeMixed(target, -2.9), -2);
assert.throws(() => storeMixed(target, 1n), TypeError);

const clampedTarget = new Uint8ClampedArray(1);
for (let i = 0; i < 3000; i++) assert.strictEqual(storeMixed(clampedTarget, i - 1000), Math.min(255, Math.max(0, i - 1000)));
assert.strictEqual(storeMixed(clampedTarget, 1.5), 2);
assert.strictEqual(storeMixed(clampedTarget, 2.5), 2);
assert.strictEqual(storeMixed(clampedTarget, NaN), 0);

console.log('ok');
