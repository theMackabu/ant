const assert = require('node:assert');
for (const object of [{ x: 1 }, Object.assign(Object.create(null), { x: 1 }), Object.assign(function() {}, { x: 1 })]) {
  Object.freeze(object);
  assert.strictEqual(Reflect.set(object, 'x', 2), false);
  assert.strictEqual(Reflect.set(object, 'x', 1), false);
  assert.strictEqual(object.x, 1);
}
const readOnly = Object.defineProperty({}, 'x', { value: 1, configurable: true });
assert.strictEqual(Reflect.set(readOnly, 'x', 2), false);
assert.strictEqual(readOnly.x, 1);
let stored;
const accessor = Object.freeze({ set x(value) { stored = value; }, get y() { return 1; } });
assert.strictEqual(Reflect.set(accessor, 'x', 2), true);
assert.strictEqual(stored, 2);
assert.strictEqual(Reflect.set(accessor, 'y', 2), false);
const sentinel = new Error('setter');
const throwing = Object.freeze({ set x(value) { throw sentinel; } });
assert.throws(() => Reflect.set(throwing, 'x', 2), error => error === sentinel);
const writable = { x: 1 };
assert.strictEqual(Reflect.set(writable, 'x', 2), true);
assert.strictEqual(writable.x, 2);
console.log('Reflect.set frozen and readonly own properties ok');
