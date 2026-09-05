const assert = require('node:assert');
function concat(left, right) { return left + right; }
const cases = [];
for (let left = 0; left <= 15; left++) {
  for (let right = 0; right <= 15; right++) {
    cases.push(['a'.repeat(left), 'b'.repeat(right)]);
  }
}
cases.push(['a\0', 'b'], ['é', '😀'], ['\ud83d', '\ude00'], ['a', '\ud800']);
for (const size of [31, 32, 39, 40, 47, 48, 55, 56, 62, 63, 64, 65, 80]) {
  cases.push(['a'.repeat(size - 1), 'b'], ['a', 'b'.repeat(size - 1)]);
}
const expected = cases.map(([a, b]) => [a, b].join(''));
const survivors = [];
for (let round = 0; round < 1500; round++) {
  for (let i = 0; i < cases.length; i++) {
    const result = concat(cases[i][0], cases[i][1]);
    assert.strictEqual(result, expected[i]);
    assert.strictEqual(result.length, expected[i].length);
    if (round % 100 === 0) survivors.push([result, i]);
  }
}
for (const [value, i] of survivors) assert.strictEqual(value, expected[i]);
assert.strictEqual(concat(3, 4), 7);
assert.strictEqual(concat('a', 4), 'a4');
const events = [];
const left = { [Symbol.toPrimitive](hint) { events.push('left ' + hint); return 'a'; } };
const right = { [Symbol.toPrimitive](hint) { events.push('right ' + hint); return 'b'; } };
assert.strictEqual(concat(left, right), 'ab');
assert.deepStrictEqual(events, ['left default', 'right default']);
assert.throws(() => concat('a', Symbol()), TypeError);
console.log('JIT short concat size classes, GC survivors, Unicode and coercion ok');
