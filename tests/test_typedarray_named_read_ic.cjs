const assert = require('node:assert');

const TypedArray = Object.getPrototypeOf(Uint8Array);
const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float16Array, Float32Array, Float64Array, BigInt64Array, BigUint64Array];

const bpe = o => o.BYTES_PER_ELEMENT;
const buf = o => o.buffer;
const len = o => o.length;
const bl = o => o.byteLength;
const bo = o => o.byteOffset;

for (let i = 0; i < 6000; i++) {
  const C = kinds[i % kinds.length];
  const ta = new C(4);
  assert.strictEqual(bpe(ta), C.BYTES_PER_ELEMENT);
  assert.strictEqual(len(ta), 4);
  assert.strictEqual(bl(buf(ta)), 4 * C.BYTES_PER_ELEMENT);
}

const dv = new DataView(new ArrayBuffer(8), 2);
const ab = new ArrayBuffer(6);
let acc = 0;
for (let i = 0; i < 20000; i++) acc += bl(dv) + bo(dv) + bl(ab) + bl(buf(dv));
assert.strictEqual(acc, 20000 * (6 + 2 + 6 + 8), 'DataView and ArrayBuffer accessors through a warm site');

const view = new Uint8Array(new ArrayBuffer(8), 2, 4);
for (let i = 0; i < 5000; i++) len(view) + bo(view);
structuredClone(view.buffer, { transfer: [view.buffer] });
assert.deepStrictEqual([len(view), bo(view), bl(view)], [0, 0, 0], 'detaching mid-flight is seen by warm sites');

Object.defineProperty(Uint8Array.prototype, 'NaN', { value: 'proto', configurable: true });
Object.defineProperty(Uint8Array.prototype, 'Infinity', { value: 'protoInf', configurable: true });
const readNaN = new Function('o', 'return o.NaN');
const readInf = new Function('o', 'return o.Infinity');
for (const make of [() => Object.create(Uint8Array.prototype), () => ({ __proto__: Uint8Array.prototype })]) {
  const plain = make();
  for (let i = 0; i < 20000; i++) { readNaN(plain); readInf(plain); }
  const ta = new Uint8Array(2);
  assert.deepStrictEqual([readNaN(ta), readInf(ta), readNaN(plain)], [undefined, undefined, 'proto'], 'canonical numeric names stay with the typed array');
}
delete Uint8Array.prototype.NaN;
delete Uint8Array.prototype.Infinity;

const shared = { foo: 1 };
const ta = new Uint8Array(2);
ta.foo = 2;
const foo = o => o.foo;
for (let i = 0; i < 5000; i++) assert.deepStrictEqual([foo(shared), foo(ta)], [1, 2], 'an own expando on a typed array');

assert.throws(() => buf(Object.create(Uint8Array.prototype)), TypeError, 'the getter still checks its receiver');
const patched = new Uint16Array(2);
for (let i = 0; i < 5000; i++) buf(patched);
Object.defineProperty(TypedArray.prototype, 'buffer', { get() { return 'patched'; }, configurable: true });
assert.strictEqual(buf(patched), 'patched', 'a patched %TypedArray% getter is seen by a warm site');

const moved = Object.setPrototypeOf(new Uint8Array(2), { BYTES_PER_ELEMENT: 'swapped' });
assert.strictEqual(bpe(moved), 'swapped', 'a typed array with a replaced prototype');

const traps = [];
const proxy = new Proxy(new Uint8Array(2), { get(t, k, r) { traps.push(k); return Reflect.get(t, k); } });
for (let i = 0; i < 300; i++) len(proxy);
assert.strictEqual(traps.length, 300, 'a proxy around a typed array runs its trap on every read');

const lens = [new Uint8Array(5), [1, 2], 'abc', (function () { return arguments; })(1, 2, 3), new String('xy')];
let total = 0;
for (let i = 0; i < 20000; i++) total += len(lens[i % lens.length]);
assert.strictEqual(total, 4000 * (5 + 2 + 3 + 3 + 2), 'length across typed arrays, arrays, strings, arguments and wrappers');
const sub = new (class extends Uint8Array { get length() { return 99; } })(3);
for (let i = 0; i < 3000; i++) len(sub);
assert.strictEqual(len(sub), 99);

console.log('typed array named read ic OK');
