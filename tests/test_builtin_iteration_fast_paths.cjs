const assert = require('node:assert');

function cat(a, b) { return a.concat(b); }
function cat2(a, x, b) { return a.concat(x, b); }
for (let i = 0; i < 5000; i++) {
  assert.deepStrictEqual(cat([1, 2], [3]), [1, 2, 3]);
  assert.deepStrictEqual(cat2([1], 2, [3, 4]), [1, 2, 3, 4]);
}

const own = [5, 6];
own[Symbol.isConcatSpreadable] = false;
assert.deepStrictEqual(cat([1], own).length, 2);
assert.strictEqual(cat([1], own)[1], own);
const objSpread = { length: 2, 0: 'a', 1: 'b', [Symbol.isConcatSpreadable]: true };
assert.deepStrictEqual(cat([1], objSpread), [1, 'a', 'b']);

let getterCalls = 0;
Object.defineProperty(Array.prototype, Symbol.isConcatSpreadable, {
  configurable: true,
  get() { getterCalls++; return getterCalls % 2 === 0; },
});
const mixed = cat([1, 2], [3]);
assert.strictEqual(getterCalls, 2);
assert.strictEqual(mixed.length, 2);
assert.deepStrictEqual(mixed[0], [1, 2]);
assert.strictEqual(mixed[1], 3);
delete Array.prototype[Symbol.isConcatSpreadable];
assert.deepStrictEqual(cat([1, 2], [3]), [1, 2, 3]);

Object.prototype[Symbol.isConcatSpreadable] = false;
const flat = cat([1], [2]);
assert.strictEqual(flat.length, 2);
assert.deepStrictEqual(flat[0], [1]);
delete Object.prototype[Symbol.isConcatSpreadable];
assert.deepStrictEqual(cat([1], [2]), [1, 2]);

const sources = [
  [1, 2, 3],
  [1.5, -1.5, 255.5, 256, -129, 1e10, NaN, Infinity, -Infinity, -0],
  [0.5, 1.5, 2.5, 254.5, 255.49],
];
const kinds = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array];
if (typeof Float16Array === 'function') kinds.push(Float16Array);
for (const Kind of kinds) {
  for (const src of sources) {
    const viaCtor = new Kind(src);
    const viaFrom = Kind.from(src);
    const viaMap = Kind.from(src, x => x);
    const viaSet = new Kind(src.length);
    for (let i = 0; i < src.length; i++) viaSet[i] = src[i];
    assert.deepStrictEqual(Array.from(viaCtor), Array.from(viaSet), Kind.name);
    assert.deepStrictEqual(Array.from(viaFrom), Array.from(viaSet), Kind.name);
    assert.deepStrictEqual(Array.from(viaMap), Array.from(viaSet), Kind.name);
  }
}
assert.deepStrictEqual(Array.from(new Uint8Array([1, , 3])), [1, 0, 3]);
assert.deepStrictEqual(Array.from(new Uint8Array(['7', true, null, { valueOf() { return 9; } }])), [7, 1, 0, 9]);
assert.throws(() => new BigInt64Array([1, 2]), TypeError);
assert.deepStrictEqual(Array.from(new BigInt64Array([1n, 2n])), [1n, 2n]);

assert.strictEqual(new Set([1, 2, 2]).size, 2);
assert.deepStrictEqual(Object.fromEntries([['a', 1]]), { a: 1 });
assert.strictEqual(new Map([[1, 2]]).get(1), 2);

let closes = 0;
Object.getPrototypeOf([][Symbol.iterator]()).return = function () { closes++; return {}; };
assert.throws(() => new Map([1]), TypeError);
assert.throws(() => Object.fromEntries([1]), TypeError);
assert.throws(() => Array.from([1, 2], () => { throw new RangeError('map'); }), RangeError);
assert.strictEqual(closes, 3);
delete Object.getPrototypeOf([][Symbol.iterator]()).return;

const ArrayIterProto = Object.getPrototypeOf([][Symbol.iterator]());
const savedNext = ArrayIterProto.next;
let nextCalls = 0;
ArrayIterProto.next = function () { nextCalls++; return savedNext.call(this); };
assert.strictEqual(new Set([1, 2]).size, 2);
assert.deepStrictEqual(Object.fromEntries([['b', 2]]), { b: 2 });
assert.strictEqual(new Uint8Array([1, 2]).length, 2);
assert.deepStrictEqual(Array.from([4, 5]), [4, 5]);
assert.strictEqual(nextCalls, 3 + 2 + 3 + 3);
ArrayIterProto.next = savedNext;

function* produce() { yield 1; yield 2; }
assert.deepStrictEqual(Array.from(Uint8Array.from(produce(), x => x * 2)), [2, 4]);
let typedCloses = 0;
const closing = { [Symbol.iterator]() { let i = 0; return { next() { return { value: i, done: i++ > 1 }; }, return() { typedCloses++; return {}; } }; } };
assert.strictEqual(Uint8Array.from(closing).length, 2);
assert.strictEqual(new Uint8Array(closing).length, 2);
assert.strictEqual(typedCloses, 0);

console.log('builtin iteration fast paths ok');
