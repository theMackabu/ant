const assert = require('node:assert');

function add(a, b) { return a.value + b.value; }
function subtract(a, b) { return a.value - b.value; }
function multiply(a, b) { return a.value * b.value; }
function divide(a, b) { return a.value / b.value; }
function chain(a, b, c) { return (a.value + b.value) * c.value; }
function chainAdd(a, b, c) { return (a.value + b.value) + c.value; }
function join(a, b, flag) { return flag ? a.value / b.value : a.value * b.value; }

for (let i = 0; i < 1200; i++) {
  const a = { value: i + 0.5 }, b = { value: 2.5 };
  assert.strictEqual(add(a, b), i + 3);
  assert.strictEqual(subtract(a, b), i - 2);
  assert.strictEqual(multiply(a, b), (i + 0.5) * 2.5);
  assert.strictEqual(divide(a, b), (i + 0.5) / 2.5);
  assert.strictEqual(chain(a, b, b), (i + 3) * 2.5);
  assert.strictEqual(chainAdd(a, b, b), i + 5.5);
  assert.strictEqual(join(a, b, i & 1), i & 1 ? (i + 0.5) / 2.5 : (i + 0.5) * 2.5);
}

for (const [a, b] of [[0, 0], [-0, 2], [Infinity, Infinity], [NaN, 3]]) {
  const lhs = { value: a }, rhs = { value: b };
  assert.ok(Object.is(add(lhs, rhs), a + b));
  assert.ok(Object.is(subtract(lhs, rhs), a - b));
  assert.ok(Object.is(multiply(lhs, rhs), a * b));
  assert.ok(Object.is(divide(lhs, rhs), a / b));
}

// Change each operand after numeric warmup, including a bailout with an
// already computed unboxed value below the failing operand on the stack.
assert.strictEqual(add({ value: '4' }, { value: 2 }), '42');
assert.strictEqual(add({ value: 2 }, { value: '4' }), '24');
assert.strictEqual(subtract({ value: 9n }, { value: 4n }), 5n);
assert.strictEqual(multiply({ value: 9n }, { value: 4n }), 36n);
assert.strictEqual(divide({ value: 9n }, { value: 4n }), 2n);
let conversions = 0;
const converted = { valueOf() { conversions++; return 4; } };
assert.strictEqual(chain({ value: 1.5 }, { value: 2.5 }, { value: converted }), 16);
assert.strictEqual(conversions, 1);
const conversionError = new Error('conversion failed');
const throwing = { valueOf() { throw conversionError; } };
assert.throws(() => chainAdd({ value: 1.5 }, { value: 2.5 }, { value: throwing }),
  error => error === conversionError);
assert.strictEqual(join({ value: '12' }, { value: 3 }, true), 4);

console.log('PASS boxed arithmetic results');
