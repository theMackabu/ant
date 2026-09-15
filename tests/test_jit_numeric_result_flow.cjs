const assert = require('node:assert');

function sumProduct(a, b, c, d) {
  return (a.value + b.value) * (c.value + d.value);
}

function difference(a, b) {
  return (a.value - b.value) / 1;
}

function quotient(a, b) {
  return (a.value / b.value) + 1;
}

function product(a, b) {
  return (a.value * b.value) - 1;
}

const box = value => ({ value });
for (let i = 0; i < 1000; i++) {
  assert.strictEqual(sumProduct(box(1), box(2), box(3), box(4)), 21);
  assert.strictEqual(difference(box(9), box(2)), 7);
  assert.strictEqual(quotient(box(9), box(3)), 4);
  assert.strictEqual(product(box(9), box(3)), 26);
}

assert.ok(Object.is(sumProduct(box(-0), box(-0), box(1), box(0)), -0));
assert.ok(Object.is(difference(box(-0), box(0)), -0));
assert.ok(Number.isNaN(sumProduct(box(Infinity), box(-Infinity), box(1), box(2))));
assert.ok(Number.isNaN(quotient(box(0), box(0))));
assert.strictEqual(quotient(box(1), box(0)), Infinity);
assert.strictEqual(difference(box(2147483647), box(-1)), 2147483648);
assert.strictEqual(product(box(4294967295), box(2)), 8589934589);

let reads = 0, conversions = 0;
const a = box(1);
const d = {
  get value() {
    reads++;
    return { valueOf() { conversions++; a.value = 99; return 5; } };
  }
};
assert.strictEqual(sumProduct(a, box(2), box(3), d), 24);
assert.strictEqual(reads, 1);
assert.strictEqual(conversions, 1);
assert.strictEqual(a.value, 99, 'bailout must preserve the already computed left operand');

assert.strictEqual(sumProduct(box(1n), box(2n), box(3n), box(4n)), 21n);
assert.strictEqual(difference(box('9'), box('2')), 7);
assert.ok(Number.isNaN(sumProduct(box('x'), box(2), box(3), box(4))));
console.log('PASS numeric result flow, NaN, signed zero, BigInt and effectful bailout');
