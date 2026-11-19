const assert = require('node:assert');

const cases = [
  [0, '0'], [-0, '-0'], [1234.5678, '1,234.568'], [-1234.5678, '-1,234.568'], [0.0005, '0.001'], [0.00049, '0'], [-0.0001, '-0'],
  [999.9995, '1,000'], [1.005, '1.005'], [1e21, '1,000,000,000,000,000,000,000'], [1.2345e25, '12,345,000,000,000,000,000,000,000'],
  [1e-7, '0'], [123456789012345680000, '123,456,789,012,345,680,000'], [NaN, 'NaN'], [Infinity, '∞'], [-Infinity, '-∞'],
];
for (const [value, expected] of cases) {
  assert.strictEqual(value.toLocaleString(), expected);
  assert.strictEqual(new Intl.NumberFormat().format(value), expected);
}

const options = [
  [1234.5678, { maximumFractionDigits: 1 }, '1,234.6'], [2.5, { maximumFractionDigits: 0 }, '3'], [1.005, { maximumFractionDigits: 2 }, '1.01'],
  [5, { minimumFractionDigits: 2 }, '5.00'], [1234.5, { useGrouping: false }, '1234.5'], [0.5, { style: 'percent' }, '50%'],
  [0.12345, { style: 'percent', maximumFractionDigits: 1 }, '12.3%'], [7, { minimumIntegerDigits: 3 }, '007'], [1.23456, { maximumFractionDigits: '2' }, '1.23'],
];
for (const [value, option, expected] of options) {
  assert.strictEqual(value.toLocaleString('en-US', option), expected);
  assert.strictEqual(new Intl.NumberFormat('en-US', option).format(value), expected);
}

assert.throws(() => (1).toLocaleString('en-US', { minimumFractionDigits: 3, maximumFractionDigits: 1 }), RangeError);
assert.throws(() => (1).toLocaleString('en-US', { maximumFractionDigits: 101 }), RangeError);
assert.throws(() => (1).toLocaleString('en-US', { style: 'bogus' }), RangeError);
assert.throws(() => (1).toLocaleString('en-US', null), TypeError);
assert.throws(() => (1).toLocaleString('xx-invalid-locale-!!'), RangeError);
assert.throws(() => new Intl.NumberFormat('en-US', { maximumFractionDigits: 200 }), RangeError);
assert.strictEqual(new Intl.NumberFormat().format(), 'NaN');
assert.strictEqual(new Intl.NumberFormat().format('1234.5'), '1,234.5');
assert.deepStrictEqual(Object.keys(new Intl.NumberFormat('en-US', { maximumFractionDigits: 1 })), []);
assert.strictEqual([1234.5678, 0.0001, 1e21].toLocaleString(), '1,234.568,0,1,000,000,000,000,000,000,000');
assert.strictEqual([1234.5678, 5].toLocaleString('en-US', { minimumFractionDigits: 2 }), '1,234.568,5.00');

console.log('ok');
