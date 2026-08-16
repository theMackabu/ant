const assert = require('assert');

function bigintOps(a, b, shift) {
  return [
    a + b,
    a - b,
    a * b,
    a / b,
    a % b,
    -a,
    a & b,
    a | b,
    a ^ b,
    ~a,
    a << shift,
    a >> shift,
  ];
}

const a = 123456789012345678901234567890n;
const b = 9876543210987654321n;
const shift = 7n;
const expected = bigintOps(a, b, shift).map(String);

for (let i = 0; i < 350; i++) {
  assert.deepStrictEqual(bigintOps(a, b, shift).map(String), expected);
}

function add(a, b) {
  return a + b;
}

function divide(a, b) {
  return a / b;
}

for (let i = 0; i < 350; i++) {
  assert.strictEqual(String(add(20n, 22n)), '42');
  assert.strictEqual(String(divide(84n, 2n)), '42');
}

function caughtMessage(callback) {
  try {
    callback();
    return 'no error';
  } catch (error) {
    return error.message;
  }
}

assert.match(caughtMessage(() => add(1n, 1)), /Cannot mix BigInt/);
assert.match(caughtMessage(() => divide(1n, 0n)), /Division by zero/);

console.log('JIT BigInt operation tests passed');
