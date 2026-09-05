// No JS references retain these originals before the lazy module is loaded.
delete String.prototype.slice;
delete Array.isArray;
delete JSON.stringify;

function churn() {
  let values;
  for (let round = 0; round < 20; round++) {
    values = [];
    for (let i = 0; i < 10000; i++) values.push({ text: 'capture-' + round + '-' + i });
  }
  return values.length;
}
churn();
const p = require('ant:internal/primordials');
churn();
const assert = require('node:assert');
assert.strictEqual(p.StringPrototypeSlice('abcd', 1), 'bcd');
assert.strictEqual(p.ArrayIsArray([]), true);
assert.strictEqual(p.JSONStringify({ x: 1 }), '{"x":1}');
assert.strictEqual(require('ant:internal/primordials'), p);
console.log('primordial originals and lazy wrappers survive allocation churn');
