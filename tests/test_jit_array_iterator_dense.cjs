const assert = require('node:assert');

function collect(iterable, visit) {
  const out = [];
  for (const value of iterable) {
    out.push(value);
    if (visit) visit(value, out.length);
  }
  return out;
}

for (let i = 0; i < 3000; i++) assert.deepStrictEqual(collect([1, 2, 3]), [1, 2, 3]);
const object = {};
const symbol = Symbol('element');
const values = [undefined, null, false, true, 0, -0, NaN, 'x', 1n, symbol, object];
assert.deepStrictEqual(collect(values), values);
assert.deepStrictEqual(collect([]), []);
assert.deepStrictEqual(collect([1, , 3]), [1, undefined, 3]);

const grown = [1, 2, 3];
assert.deepStrictEqual(collect(grown, (_, n) => {
  if (n === 1) grown.push(4);
}), [1, 2, 3, 4]);
const truncated = [1, 2, 3];
assert.deepStrictEqual(collect(truncated, () => { truncated.length = 1; }), [1]);

let reads = 0;
const prototype = Object.create(Array.prototype);
Object.defineProperty(prototype, '1', { get() { reads++; return 20; } });
const inherited = [1, 2, 3];
Object.setPrototypeOf(inherited, prototype);
assert.deepStrictEqual(collect(inherited, (_, n) => {
  if (n === 1) delete inherited[1];
}), [1, 20, 3]);
assert.strictEqual(reads, 1);

const accessor = [1, 2, 3];
assert.deepStrictEqual(collect(accessor, (_, n) => {
  if (n === 1) Object.defineProperty(accessor, '1', { get() { return 21; } });
}), [1, 21, 3]);
const marker = new Error('iterator getter');
const throwing = [1, 2, 3];
Object.defineProperty(throwing, '1', { get() { throw marker; } });
assert.throws(() => collect(throwing), error => error === marker);
assert.throws(() => {
  for (const value of throwing) void value;
}, error => error === marker);
assert.throws(() => {
  const [first, second] = throwing;
  return first + second;
}, error => error === marker);
assert.deepStrictEqual(collect([4, 5]), [4, 5]);

assert.deepStrictEqual(collect(new Map([[1, 2], [3, 4]]).values()), [2, 4]);
assert.deepStrictEqual(collect(new Set([1, 2, 3])), [1, 2, 3]);
assert.deepStrictEqual(collect(new Uint8Array([5, 6])), [5, 6]);
assert.deepStrictEqual(collect((function* () { yield 7; yield 8; })()), [7, 8]);

function nested() {
  let total = 0;
  for (const row of [[1, 2], [3, 4]]) for (const value of row) total += value;
  return total;
}
for (let i = 0; i < 3000; i++) assert.strictEqual(nested(), 10);

function temporaryArray() {
  let total = 0;
  let garbage = [];
  for (const value of Array.from({ length: 32 }, (_, i) => ({ i }))) {
    for (let i = 0; i < 256; i++) garbage.push({ values: [i, i + 1] });
    if (garbage.length >= 1024) garbage = [];
    total += value.i;
  }
  return total;
}
for (let i = 0; i < 30; i++) assert.strictEqual(temporaryArray(), 496);
console.log('PASS dense array iteration preserves mutations, holes, accessors, nesting and GC roots');
