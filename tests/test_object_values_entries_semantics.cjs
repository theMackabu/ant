// Object.values and Object.entries must call getters, list integer keys first in
// ascending order, and handle string sources by UTF-16 code unit
const assert = require('node:assert');

assert.deepStrictEqual(Object.values({ get x() { return 42; } }), [42]);
assert.deepStrictEqual(Object.entries({ a: 1, get x() { return 42; } }), [['a', 1], ['x', 42]]);
assert.deepStrictEqual(Object.entries({ set s(v) {}, x: 1 }), [['s', undefined], ['x', 1]]);
assert.throws(() => Object.values({ get a() { throw new TypeError('getter'); } }), TypeError);

const hidden = { x: 1 };
Object.defineProperty(hidden, 'g', { get() { throw new Error('must not run'); }, enumerable: false });
assert.deepStrictEqual(Object.values(hidden), [1]);

assert.deepStrictEqual(Object.values({ b: 1, 2: 'x', 1: 'y', a: 2 }), ['y', 'x', 1, 2]);
assert.deepStrictEqual(Object.entries({ 10: 'a', 9: 'b', z: 1 }), [['9', 'b'], ['10', 'a'], ['z', 1]]);

const shaped = [1, , 3];
Object.defineProperty(shaped, 1, { value: 2, enumerable: true, writable: false, configurable: true });
assert.deepStrictEqual(Object.values(shaped), [1, 2, 3]);

const getterIndex = [1, 2, 3];
Object.defineProperty(getterIndex, 1, { get() { return 'G'; }, enumerable: true });
getterIndex.extra = 'e';
assert.deepStrictEqual(Object.entries(getterIndex), [['0', 1], ['1', 'G'], ['2', 3], ['extra', 'e']]);

const deletes = { get a() { delete this.b; return 1; }, b: 2, c: 3 };
assert.deepStrictEqual(Object.entries(deletes), [['a', 1], ['c', 3]]);

const hides = { get a() { Object.defineProperty(this, 'b', { enumerable: false }); return 1; }, b: 2, c: 3 };
assert.deepStrictEqual(Object.values(hides), [1, 3]);

const adds = { get a() { this.z = 9; return 1; }, b: 2 };
assert.deepStrictEqual(Object.entries(adds), [['a', 1], ['b', 2]]);

class Point { constructor() { this.x = 1; } get y() { return 2; } }
assert.deepStrictEqual(Object.values(new Point()), [1]);

function fn() {}
Object.defineProperty(fn, 'g', { get() { return 5; }, enumerable: true });
fn.k = 1;
assert.deepStrictEqual(Object.entries(fn), [['g', 5], ['k', 1]]);

const sym = Symbol('s');
assert.deepStrictEqual(Object.entries({ [sym]: 1, a: 3, get b() { return 4; } }), [['a', 3], ['b', 4]]);

assert.deepStrictEqual(Object.values('é😀'), ['é', '\ud83d', '\ude00']);
assert.deepStrictEqual(Object.entries('ab'), [['0', 'a'], ['1', 'b']]);
assert.deepStrictEqual(Object.entries(new String('é')), [['0', 'é']]);

const boxed = new String('hi');
boxed.x = 1;
boxed[5] = 'f';
assert.deepStrictEqual(Object.entries(boxed), [['0', 'h'], ['1', 'i'], ['5', 'f'], ['x', 1]]);

assert.deepStrictEqual(Object.values(new Int16Array([1, -2, 3])), [1, -2, 3]);
assert.deepStrictEqual(Object.values({ a: 1, b: 2 }), [1, 2]);
assert.deepStrictEqual((function () { return Object.values(arguments); })(1, 'b'), [1, 'b']);

function churn() {
  let junk = null;
  for (let i = 0; i < 20000; i++) junk = { i, s: 'x' + i, next: i % 32 ? junk : null };
  return junk;
}

for (let round = 0; round < 60; round++) {
  const source = {
    a: { v: 'a' + round },
    get c() { churn(); return { v: 'c' + round }; },
    d: 'd' + round,
    get e() { churn(); return 'e' + round; },
    7: { v: 7 },
  };
  const entries = Object.entries(source);
  churn();
  assert.deepStrictEqual(entries, [
    ['7', { v: 7 }], ['a', { v: 'a' + round }], ['c', { v: 'c' + round }], ['d', 'd' + round], ['e', 'e' + round],
  ]);
}

console.log('Object.values/entries semantics OK');
