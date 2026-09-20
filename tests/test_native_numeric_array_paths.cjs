const assert = require('node:assert');

function min(a, b) { return Math.min(a, b); }
for (let i = 0; i < 20000; i++) assert.strictEqual(min(i, 500), i < 500 ? i : 500);
assert.strictEqual(Math.min(), Infinity);
assert.strictEqual(Math.min(Infinity, -Infinity), -Infinity);
assert.strictEqual(Object.is(min(0, -0), -0), true);
assert.strictEqual(Object.is(min(-0, 0), -0), true);
assert.strictEqual(Number.isNaN(min(NaN, 1)), true);
assert.strictEqual(Number.isNaN(min(1, NaN)), true);
assert.strictEqual(min('3', 4), 3);
assert.strictEqual(min(4, { valueOf() { return 2; } }), 2);
const failure = { marker: 'conversion failure' };
const throwing = { valueOf() { throw failure; } };
assert.throws(() => min(throwing, 1), error => error === failure);
assert.throws(() => min(1, throwing), error => error === failure);
assert.strictEqual(min(3, 4), 3);

function pop(array) { return array.pop(); }
for (let round = 0; round < 160; round++) {
  const values = Array.from({ length: 128 }, (_, i) => ({ i }));
  for (let i = 127; i >= 0; i--) {
    assert.strictEqual(pop(values).i, i);
    assert.strictEqual(values.length, i);
    assert.strictEqual(Object.hasOwn(values, i), false);
  }
  assert.strictEqual(pop(values), undefined);
  values.push(42);
  assert.strictEqual(pop(values), 42);
}
const holes = [1, , 3];
assert.strictEqual(pop(holes), 3);
assert.strictEqual(pop(holes), undefined);
assert.strictEqual(pop(holes), 1);
const sparse = [1];
sparse.length = 10000;
sparse[9999] = 7;
assert.strictEqual(pop(sparse), 7);
assert.strictEqual(sparse.length, 9999);
assert.strictEqual(pop(sparse), undefined);
const object = { 0: 5, length: 1 };
assert.strictEqual(Array.prototype.pop.call(object), 5);
assert.strictEqual(object.length, 0);
const target = [1, 2];
assert.strictEqual(pop(new Proxy(target, {})), 2);
assert.strictEqual(target.length, 1);
assert.throws(() => pop(new Proxy([1], {
  get(target, key, receiver) {
    if (key === '0') throw failure;
    return Reflect.get(target, key, receiver);
  },
})), error => error === failure);
console.log('native numeric and array paths passed');
