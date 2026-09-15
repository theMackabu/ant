const assert = require('node:assert');

function postIncrement(value) {
  const old = value++;
  return [old, value];
}
function countPostIncrement(n) {
  let count = 0;
  for (let i = 0; i < n; i++) if (i >= 0) count++;
  return count;
}
// This body has no other opcode that requests bailout storage.
let captured = 0;
function postIncrementCaptured() { return captured++; }
function postDecrement(value) {
  const old = value--;
  return [old, value];
}
for (let i = 0; i < 500; i++) {
  assert.deepStrictEqual(postIncrement(i + 0.5), [i + 0.5, i + 1.5]);
  assert.strictEqual(postIncrementCaptured(), i);
  assert.deepStrictEqual(postDecrement(i + 0.5), [i + 0.5, i - 0.5]);
  assert.strictEqual(countPostIncrement(100), 100);
}
captured = '41';
assert.strictEqual(postIncrementCaptured(), 41);
assert.strictEqual(captured, 42);
captured = 41n;
assert.strictEqual(postIncrementCaptured(), 41n);
assert.strictEqual(captured, 42n);
for (const [input, old, next] of [
  [undefined, NaN, NaN], [null, 0, 1], [false, 0, 1], [true, 1, 2],
  ['41', 41, 42], ['bad', NaN, NaN], [-0, -0, 1],
  [Infinity, Infinity, Infinity], [-Infinity, -Infinity, -Infinity],
  [9007199254740991, 9007199254740991, 9007199254740992],
  [41n, 41n, 42n],
]) {
  const result = postIncrement(input);
  assert.ok(Object.is(result[0], old), 'post-increment old value');
  assert.ok(Object.is(result[1], next), 'post-increment new value');
}
let conversions = 0;
assert.deepStrictEqual(postIncrement({ valueOf() { conversions++; return 8; } }), [8, 9]);
assert.strictEqual(conversions, 1);
assert.throws(() => postIncrement(Symbol('x')), TypeError);
assert.deepStrictEqual(postDecrement('41'), [41, 40]);
assert.deepStrictEqual(postDecrement(41n), [41n, 40n]);
assert.throws(() => postDecrement(Symbol('x')), TypeError);
assert.strictEqual(countPostIncrement(100000), 100000);
console.log('PASS numeric post-increment');
