'use strict';
const assert = require('node:assert');

function bitwise(x, y) { return [x & y, x | y, x ^ y, x << y, x >> y, x >>> y, ~x]; }
// Expected values independently checked with Node. Warm with fractions before
// testing boundaries that must fall back to the general ToInt32 conversion.
const cases = [
  [1.75, 3.75, [1, 3, 2, 8, 0, 0, -2]],
  [-1.75, 3.75, [3, -1, -4, -8, -1, 536870911, 0]],
  [2147483647.75, 1.75, [1, 2147483647, 2147483646, -2, 1073741823, 1073741823, -2147483648]],
  [2147483648.75, 1.75, [0, -2147483647, -2147483647, 0, -1073741824, 1073741824, 2147483647]],
  [4294967294.75, 33.75, [32, -1, -33, -4, -1, 2147483647, 1]],
  [-2147483648.75, -1.75, [-2147483648, -1, 2147483647, 0, -1, 1, 2147483647]],
  [4294967296.75, 3.75, [0, 3, 3, 0, 0, 0, -1]],
  [-0, 0, [0, 0, 0, 0, 0, 0, -1]],
  [NaN, 3, [0, 3, 3, 0, 0, 0, -1]],
  [Infinity, 1, [0, 1, 1, 0, 0, 0, -1]],
  [-Infinity, 1, [0, 1, 1, 0, 0, 0, -1]],
  [Number.MAX_VALUE, 1, [0, 1, 1, 0, 0, 0, -1]],
];
for (const [x, y, expected] of cases) {
  for (let i = 0; i < 1000; i++) {
    const actual = bitwise(x, y);
    for (let j = 0; j < expected.length; j++)
      assert.ok(Object.is(actual[j], expected[j]), `operator ${j}, inputs ${x}, ${y}`);
  }
}

function or(x, y) { return x | y; }
for (let i = 0; i < 1000; i++) assert.strictEqual(or(1.75, 2.75), 3);
const events = [];
const left = { valueOf() { events.push('left'); return 1.75; } };
const right = { valueOf() { events.push('right'); return 2.75; } };
assert.strictEqual(or(left, right), 3);
assert.strictEqual(events.join(','), 'left,right');
assert.strictEqual(or(1n, 2n), 3n);
assert.throws(() => or(1n, 2), TypeError);

// Fractional array indices still require exact-integer guards.
const array = [10, 20];
array['0.75'] = 30;
function get(a, i) { return a[i]; }
for (let i = 0; i < 1000; i++) assert.strictEqual(get(array, 0), 10);
assert.strictEqual(get(array, 0.75), 30);
console.log('PASS fractional bitwise conversion');
