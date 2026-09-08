const assert = require('node:assert');

function nullEqual(value) { return typeof null === value; }
function nullUnequal(value) { return typeof null !== value; }
function numberEqual(value) { return typeof 42 === value; }
function booleanEqual(value) { return value === typeof true; }
function undefinedEqual(value) { return typeof undefined === value; }

for (let i = 0; i < 400; i++) {
  assert.strictEqual(nullEqual('object'), true, `null equality at ${i}`);
  assert.strictEqual(nullUnequal('object'), false, `null inequality at ${i}`);
  assert.strictEqual(numberEqual('number'), true, `number equality at ${i}`);
  assert.strictEqual(booleanEqual('boolean'), true, `boolean equality at ${i}`);
  assert.strictEqual(undefinedEqual('undefined'), true, `undefined equality at ${i}`);
}
assert.strictEqual(nullEqual(null), false);
assert.strictEqual(numberEqual(42), false);
assert.strictEqual(booleanEqual(true), false);
assert.strictEqual(undefinedEqual(undefined), false);
console.log('jit typeof value metadata: ok');
