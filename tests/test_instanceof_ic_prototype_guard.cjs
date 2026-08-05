const assert = require('node:assert');

const weakSet = new WeakSet();
const weakMap = new WeakMap();

assert.strictEqual(weakSet instanceof WeakSet, true);
assert.strictEqual(weakMap instanceof WeakSet, false);
assert.strictEqual(weakMap instanceof WeakMap, true);

function privateFieldAdd(receiver, state, value) {
  return state.has(receiver)
    ? (() => { throw new TypeError('Cannot add the same private member more than once'); })()
    : state instanceof WeakSet
      ? state.add(receiver)
      : state.set(receiver, value);
}

const receiver = {};
privateFieldAdd(receiver, new WeakSet());
privateFieldAdd(receiver, new WeakMap(), 1);

assert.strictEqual(new WeakMap() instanceof WeakSet, false);

function A() {}
function B() {}

const a = Object.create(A.prototype);
const b = Object.create(B.prototype);
a.x = 1;
b.x = 1;

function isA(value) {
  return value instanceof A;
}

assert.strictEqual(isA(a), true);
assert.strictEqual(isA(b), false);

const oldA = new A();
assert.strictEqual(oldA instanceof A, true);
A.prototype = {};
assert.strictEqual(oldA instanceof A, false);
assert.strictEqual(new A() instanceof A, true);

function primitiveIsA(value) {
  return value instanceof A;
}
for (let i = 0; i < 5000; i++)
  assert.strictEqual(primitiveIsA('value'), false);
Object.defineProperty(A, Symbol.hasInstance, {
  configurable: true,
  value(value) { return value === 'accepted'; },
});
assert.strictEqual(primitiveIsA('accepted'), true);
delete A[Symbol.hasInstance];
assert.strictEqual(primitiveIsA('accepted'), false);

let hasInstanceCalls = 0;
class Custom {
  static [Symbol.hasInstance](value) {
    hasInstanceCalls++;
    return Boolean(value && value.accepted);
  }
}

const accepted = { accepted: true };
const rejected = { accepted: false };
assert.strictEqual(accepted instanceof Custom, true);
assert.strictEqual(rejected instanceof Custom, false);
assert.strictEqual(accepted instanceof Custom, true);
assert.ok(hasInstanceCalls >= 3);

console.log('instanceof-ic-prototype-guard:ok');
