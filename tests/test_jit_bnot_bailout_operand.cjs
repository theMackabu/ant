const assert = require('node:assert');

function stringNot(value) { return ~value; }
function objectNot(value) { return ~value; }
function bigintNot(value) { return ~value; }

let conversions = 0;
const operand = { valueOf() { conversions++; return 4; } };
for (let i = 0; i < 400; i++) {
  assert.strictEqual(stringNot('4'), -5, `string operand at ${i}`);
  assert.strictEqual(objectNot(operand), -5, `object operand at ${i}`);
  assert.strictEqual(bigintNot(4n), -5n, `bigint operand at ${i}`);
}
assert.strictEqual(conversions, 400, 'coercion occurs exactly once per call');
console.log('jit bnot bailout operand: ok');
