const assert = require('node:assert');
function checks(value) {
  return [typeof value === 'string', typeof value == 'number',
    typeof value === 'boolean', 'undefined' === typeof value,
    'symbol' == typeof value, typeof value === 'bigint',
    typeof value !== 'string', 'number' != typeof value];
}
const names = ['string', 'number', 'boolean', 'undefined', 'symbol', 'bigint'];
const values = ['abc', '', 'x'.repeat(4096) + 'y', 0, -0, NaN, Infinity, -Infinity,
  1.5, true, false, undefined, null, {}, [], function () {}, Symbol('x'), 42n,
  new String('abc'), new Number(1), new Boolean(false),
  new Proxy({}, {}), new Proxy(function () {}, {})];
for (let i = 0; i < 30000; i++) {
  const value = values[i % values.length];
  const actualType = typeof value;
  const expected = names.map(name => actualType === name);
  expected.push(actualType !== names[0], actualType !== names[1]);
  assert.deepStrictEqual(checks(value), expected);
}
assert.strictEqual(typeof antMissingTypeTestBinding === 'undefined', true);
assert.throws(() => { const result = typeof lexical === 'string'; let lexical; return result; }, ReferenceError);
let reads = 0;
const holder = { get value() { reads++; return 'abc'; } };
assert.strictEqual(typeof holder.value === 'string', true);
assert.strictEqual(reads, 1);
const sentinel = new Error('getter');
assert.throws(() => typeof ({ get value() { throw sentinel; } }).value === 'string', e => e === sentinel);
assert.strictEqual(eval('typeof antMissingEvalTypeTestBinding === "undefined"'), true);
assert.strictEqual(Function('object', 'with (object) { return typeof value === "string"; }')({ value: 'abc' }), true);
console.log('primitive typeof comparisons, NaN, boxed values, proxies, TDZ and effects ok');
