const assert = require('node:assert');
const uncurry = target => Function.prototype.call.bind(target);
const slice = uncurry(String.prototype.slice);
const join = uncurry(Array.prototype.join);
const push = uncurry(Array.prototype.push);
const tag = uncurry(Object.prototype.toString);
const has = uncurry(Object.prototype.hasOwnProperty);
const apply = uncurry(Function.prototype.call);
for (let i = 0; i < 30000; i++) {
  assert.strictEqual(slice('abc', 1, 2), 'b');
  assert.strictEqual(join([1, 2], ':'), '1:2');
  assert.strictEqual(tag(null), '[object Null]');
  assert.strictEqual(tag(), '[object Undefined]');
  assert.strictEqual(has({ a: 1 }, 'a'), true);
  assert.strictEqual(apply(function (n) { 'use strict'; return this === null ? n : -1; }, null, 42), 42);
}
assert.strictEqual(slice.bind(null, 'abc')(1), 'bc');
assert.strictEqual(slice.bind(null, 'abc', 1)(), 'bc');
const many = Array.from({ length: 40 }, (_, i) => i);
const target = [];
assert.strictEqual(push.bind(null, target, ...many)(40), 41);
assert.deepStrictEqual(target, [...many, 40]);
let effects = 0;
const converted = { toString() { effects++; return 'abc'; } };
assert.strictEqual(slice(converted, 1), 'bc');
assert.strictEqual(effects, 1);
const sentinel = new Error('receiver');
assert.throws(() => slice({ toString() { throw sentinel; } }), e => e === sentinel);
assert.throws(() => uncurry(String.prototype.repeat)('x', -1));
const custom = uncurry(function () { 'use strict'; return this; });
assert.strictEqual(custom(null), null);
assert.strictEqual(custom(), undefined);
const original = String.prototype.slice;
try {
  String.prototype.slice = () => 'changed';
  assert.strictEqual(slice('abc', 1), 'bc');
} finally { String.prototype.slice = original; }
console.log('native uncurry receivers, bound arguments, callbacks and exceptions ok');
