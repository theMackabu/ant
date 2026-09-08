const assert = require('node:assert');

function choose(value) { if (value) return 11; return 22; }
function caller(value) { const result = choose(value); return result + 1; }
function andValue(value) { return value && 13; }
function andCaller(value) { const result = andValue(value); return result; }
function orValue(value) { return value || 17; }
function orCaller(value) { const result = orValue(value); return result; }

const values = [0, -0, NaN, '', null, undefined, 0n, false, 1, -1, 'x', {}, [], 1n, true];
for (let i = 0; i < 400; i++) {
  for (const value of values) {
    assert.strictEqual(caller(value), value ? 12 : 23, `if condition at ${i}`);
    assert.strictEqual(andCaller(value), value && 13, `and condition at ${i}`);
    assert.strictEqual(orCaller(value), value || 17, `or condition at ${i}`);
  }
}
console.log('jit inline truthiness: ok');
