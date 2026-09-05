const assert = require('node:assert');
const charCodeAt = Function.prototype.call.bind(String.prototype.charCodeAt);
function length(value) { return value.length; }
function concat(a, b) { return a + b; }
assert.throws(() => length(null), TypeError);
assert.throws(() => length(undefined), TypeError);
const cases = [
  ['abcdefghijklmnop', 'qrstuvwxyz', 26],
  ['abcdefghijklmnop', '😀é\ud800', 20],
  ['😀'.repeat(20), 'z'.repeat(20), 60],
  ['a'.repeat(200), '\0'.repeat(200), 400],
];
const keep = [];
for (let round = 0; round < 30000; round++) {
  const [a, b, size] = cases[round & 3];
  const value = concat(a, b);
  // Exercise uncached length, a flattening consumer, then repeated cached reads.
  assert.strictEqual(length(value), size);
  assert.strictEqual(charCodeAt(value, 0), charCodeAt(a, 0));
  for (let i = 0; i < 8; i++) assert.strictEqual(length(value), size);
  if (round % 100 === 0) keep.push([value, size]);
}
for (const [value, size] of keep) assert.strictEqual(length(value), size);
assert.strictEqual(length([1, 2, 3]), 3);
assert.strictEqual(length(new String('😀')), 2);
let reads = 0;
assert.strictEqual(length({get length() { reads++; return 42; }}), 42);
assert.strictEqual(reads, 1);
assert.throws(() => length(null), TypeError);
assert.throws(() => length(undefined), TypeError);
const sentinel = {};
let after = false;
try {
  length({get length() { throw sentinel; }});
  after = true;
} catch (error) { assert.strictEqual(error, sentinel); }
assert.strictEqual(after, false);
console.log('cached rope ASCII/UTF-16 length, survivors and property fallback ok');
