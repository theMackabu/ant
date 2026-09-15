const assert = require('node:assert');
function loop(n, value) {
  let result = value;
  while (--n > 1) result *= n;
  return result;
}
function reference(n, value) {
  // Keep a separately written reference and explicit boundary expectations.
  let result = value;
  while (--n > 1) result = multiply(result, n);
  return result;
}
function multiply(a, b) { return a * b; }
for (let i = 0; i < 1000; i++) assert.strictEqual(loop(100, 100), reference(100, 100));
for (const n of [0, 1, 2, 3, 4, 5, 9, 10, 99, 100, 101, 2.5, 10.5, NaN, -Infinity])
  for (const value of [0, -0, 1, -1, 1.3, NaN, Infinity, -Infinity])
    assert.ok(Object.is(loop(n, value), reference(n, value)), `${n},${value}`);
assert.strictEqual(loop(1, 7), 7);
assert.strictEqual(loop(2, 7), 7);
assert.strictEqual(loop(3, 7), 14);
assert.strictEqual(loop(4, 7), 42);
assert.strictEqual(loop(5, 7), 168);
assert.strictEqual(loop(6, 7), 840);
assert.strictEqual(loop(7, 7), 5040);
assert.strictEqual(loop(4.5, 2), 26.25);
assert.ok(Object.is(loop(8, -0), -0));
assert.ok(Number.isNaN(loop(8, NaN)));
assert.strictEqual(loop(8, Infinity), Infinity);
// Calls, stores, and extra control-flow edges must retain their effects.
function effectful(n, observer) {
  let product = 1;
  while (--n > 1) { observer(n); product *= n; }
  return product;
}
let seen = [];
for (let i = 0; i < 300; i++) {
  seen = [];
  assert.strictEqual(effectful(7, n => seen.push(n)), 720);
  assert.deepStrictEqual(seen, [6, 5, 4, 3, 2]);
}
function joined(n) {
  let result = 1;
  while (--n > 1) {
    if (n === 3) break;
    result *= n;
  }
  return result;
}
for (let i = 0; i < 1000; i++) assert.strictEqual(joined(7), 120);
console.log('PASS numeric loop unroll');
