const assert = require('node:assert');

function fixture() {
  const array = [11, 22];
  for (const [key, value] of [
    [1.5, 101], [-1, 102], [-0.5, 103], [4294967295, 104],
    [4294967296, 105], [1e100, 106], [-1e100, 107],
    [NaN, 108], [Infinity, 109], [-Infinity, 110],
  ]) array[key] = value;
  return array;
}

const cases = [
  [0, 11], [-0, 11], [1, 22], [1.5, 101], [-1, 102], [-0.5, 103],
  [Number.MIN_VALUE, undefined], [-Number.MIN_VALUE, undefined],
  [2147483647, undefined], [4294967294, undefined],
  [4294967295, 104], [4294967296, 105], [1e100, 106], [-1e100, 107],
  [NaN, 108], [-NaN, 108], [Infinity, 109], [-Infinity, 110],
  ['1', 22], [undefined, undefined], [null, undefined],
];

// A separate hot function for each case keeps earlier fallbacks from masking it.
for (const useLocal of [false, true]) {
  for (const [key, expected] of cases) {
    const read = new Function('array', 'key', useLocal
      ? 'let index = 0; index = key; return array[index];'
      : 'return array[key];');
    const array = fixture();
    for (let i = 0; i < 600; i++) assert.strictEqual(read(array, i & 1), i & 1 ? 22 : 11);
    assert.strictEqual(read(array, key), expected, 'read ' + String(key));
    assert.strictEqual(array.length, 2);
  }
}

for (const key of [-0, -1, -0.5, 1.5, 4294967295, 4294967296, 1e100, NaN, Infinity, -Infinity]) {
  const write = new Function('array', 'key', 'value', 'array[key] = value;');
  const array = fixture();
  for (let i = 0; i < 600; i++) write(array, i & 1, i & 1 ? 22 : 11);
  write(array, key, 77);
  assert.strictEqual(array[key], 77, 'write ' + String(key));
  assert.strictEqual(array[0], Object.is(key, -0) ? 77 : 11);
  assert.strictEqual(array[1], 22);
  assert.strictEqual(array.length, 2);
}

console.log('PASS numeric array index ranges and property fallbacks');
