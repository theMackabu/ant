const assert = require('node:assert');

const sym = Symbol('s');

const withSym = { [sym]: 1 };
assert.strictEqual(Reflect.deleteProperty(withSym, sym), true);
assert.deepStrictEqual(Object.getOwnPropertySymbols(withSym), []);

const withIndex = { 1: 1 };
assert.strictEqual(Reflect.deleteProperty(withIndex, 1), true);
assert.deepStrictEqual(Object.keys(withIndex), []);

const withNul = { 'a\0b': 1, a: 2 };
assert.strictEqual(Reflect.deleteProperty(withNul, 'a\0b'), true);
assert.deepStrictEqual(Object.keys(withNul), ['a'], 'NUL key deletes the full key, not its prefix');

const withObjectKey = { k: 1 };
assert.strictEqual(Reflect.deleteProperty(withObjectKey, { toString() { return 'k'; } }), true);
assert.deepStrictEqual(Object.keys(withObjectKey), []);

const withUndefined = { undefined: 1 };
assert.strictEqual(Reflect.deleteProperty(withUndefined), true);
assert.deepStrictEqual(Object.keys(withUndefined), []);

assert.throws(() => Reflect.deleteProperty({}, { toString() { throw new RangeError('key'); } }), RangeError);
assert.throws(() => Reflect.deleteProperty(1, 'a'), TypeError);
assert.throws(() => Reflect.deleteProperty(undefined, 'a'), TypeError);

const array = [1, 2, 3];
assert.strictEqual(Reflect.deleteProperty(array, 1), true);
assert.strictEqual(1 in array, false);
assert.strictEqual(Reflect.deleteProperty([1], 'length'), false);

(function () {
  'use strict';
  assert.strictEqual(Reflect.deleteProperty(Object.freeze({ a: 1 }), 'a'), false, 'strict callers get false, not a throw');
  assert.strictEqual(Reflect.deleteProperty(Object.freeze({ [sym]: 1 }), sym), false);
  const proxy = new Proxy(Object.freeze({ a: 1, [sym]: 1 }), {});
  assert.strictEqual(Reflect.deleteProperty(proxy, 'a'), false);
  assert.strictEqual(Reflect.deleteProperty(proxy, sym), false);
  assert.throws(() => delete proxy[sym], TypeError);
})();

const trapped = new Proxy({}, { deleteProperty(target, key) { return typeof key === 'symbol'; } });
assert.strictEqual(Reflect.deleteProperty(trapped, sym), true);
assert.strictEqual(Reflect.deleteProperty(trapped, 'x'), false);

console.log('reflect deleteProperty ok');
