const assert = require('node:assert');
const slice = Function.prototype.call.bind(String.prototype.slice);

function expected(value, start, end) {
  const length = value.length;
  start = start === undefined ? 0 : Math.min(Math.trunc(start), length);
  end = end === undefined ? length : Math.min(Math.trunc(end), length);
  let result = '';
  for (let i = start; i < end; i++) result += value[i];
  return result;
}

const inputs = ['', 'a', 'abc\0def', 'x'.repeat(300), 'y'.repeat(10000)];
const bounds = [undefined, 0, -0, 0.9, 1, 1.9, 3, 100, 1e30, Infinity];
for (let round = 0; round < 100; round++) {
  for (const input of inputs) {
    for (const start of bounds) {
      for (const end of bounds) {
        assert.strictEqual(slice(input, start, end), expected(input, start, end));
      }
    }
    assert.strictEqual(slice(input), input);
  }
}

// Unicode and negative bounds retain the UTF-16 fallback.
assert.strictEqual(slice('a😀z', 1, 2), '\ud83d');
assert.strictEqual(slice('a😀z', 2, 3), '\ude00');
assert.strictEqual(slice('abcdef', -3, -1), 'de');
assert.strictEqual(slice(new String('abcdef'), 1, 3), 'bc');
const original = String.prototype.slice;
String.prototype.slice = () => 'replaced';
assert.strictEqual(slice('abcdef', 1, 3), 'bc');
String.prototype.slice = original;
console.log('ASCII slice bounds, allocation churn, Unicode and captured target ok');
