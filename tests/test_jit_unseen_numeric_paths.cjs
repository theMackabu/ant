const assert = require('node:assert');

function rareArithmetic(mode, values, count) {
  let sum = mode === 2 ? 0n : 0;
  for (let i = 0; i < count; i++) {
    if (mode === 0) sum += i;
    else sum += (values.a + values.b) * (values.a - values.b) / values.c;
  }
  return sum;
}

function rareComparison(mode, a, b, count) {
  let sum = 0;
  for (let i = 0; i < count; i++) {
    if (mode === 0) sum++;
    else {
      if (a <= b) sum += 1;
      if (a >= b) sum += 2;
      if (a < b) sum += 4;
      if (a > b) sum += 8;
    }
  }
  return sum;
}

// Compile the loops without visiting their arithmetic/comparison branches.
for (let i = 0; i < 200; i++) {
  assert.strictEqual(rareArithmetic(0, null, 32), 496);
  assert.strictEqual(rareComparison(0, null, null, 32), 32);
}
assert.strictEqual(rareArithmetic(1, { a: 9, b: 3, c: 2 }, 16), 576);
assert.strictEqual(rareComparison(1, 2, 10, 16), 80);
assert.strictEqual(rareComparison(1, '2', '10', 16), 160);
assert.strictEqual(rareComparison(1, NaN, 10, 16), 0);
assert.strictEqual(rareComparison(1, Infinity, Infinity, 16), 48);

let reads = 0, conversions = 0;
const values = {
  a: 9, b: 3,
  get c() { reads++; return { valueOf() { conversions++; return 2; } }; }
};
assert.strictEqual(rareArithmetic(1, values, 16), 576);
assert.strictEqual(reads, 16);
assert.strictEqual(conversions, 16, 'bailout must not repeat the pending conversion');
assert.strictEqual(rareArithmetic(1, { a: '9', b: 3, c: 2 }, 16), 4464);
assert.strictEqual(rareArithmetic(2, { a: 9n, b: 3n, c: 2n }, 16), 576n);

let compares = 0;
const left = { valueOf() { compares++; return 2; } };
const right = { valueOf() { compares++; return 10; } };
assert.strictEqual(rareComparison(1, left, right, 16), 80);
assert.strictEqual(compares, 128);
console.log('PASS unseen numeric paths, type changes, comparisons and effectful bailout');
