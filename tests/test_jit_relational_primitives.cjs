const assert = require('node:assert');

const values = [undefined, null, true, false, 0, -1, 2.5, NaN, '', '3', 'abc', '10', ' 7 '];

function compareAll(a, b) {
  return [a < b, a <= b, a > b, a >= b].map(Number).join('');
}

const expected = [];
for (const a of values) for (const b of values) expected.push(compareAll(a, b));

function run() {
  let k = 0;
  for (const a of values) for (const b of values) {
    const lt = a < b, le = a <= b, gt = a > b, ge = a >= b;
    assert.strictEqual([lt, le, gt, ge].map(Number).join(''), expected[k++], `${String(a)} vs ${String(b)}`);
  }
}

for (let i = 0; i < 300; i++) run();

function bodyRange(result) {
  return result.end > result.start ? result.end - result.start : 0;
}
for (let i = 0; i < 5000; i++) {
  assert.strictEqual(bodyRange(i & 1 ? { end: i, start: 1 } : {}), i & 1 ? i - 1 : 0);
}

function viaValueOf(a, b) { return a > b; }
for (let i = 0; i < 5000; i++) assert.strictEqual(viaValueOf(i, 3), i > 3);
let calls = 0;
assert.strictEqual(viaValueOf({ valueOf() { calls++; return 9; } }, 3), true);
assert.strictEqual(calls, 1);
assert.throws(() => viaValueOf(Symbol('s'), 1), TypeError);
assert.strictEqual(viaValueOf(10n, 3), true);
assert.strictEqual(viaValueOf('2', 10n), false);

console.log('jit relational primitives: ok');
