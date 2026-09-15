const assert = require('node:assert');

const cases = [
  [0, 0, 0], [-0, 0, 0], [1.5, 1, 1], [-1.5, -1, 4294967295],
  [2147483648, -2147483648, 2147483648],
  [-2147483649, 2147483647, 2147483647],
  [4294967295, -1, 4294967295], [4294967296, 0, 0], [4294967297, 1, 1],
  [-4294967295, 1, 1], [-4294967297, -1, 4294967295],
  [9007199254740991, -1, 4294967295], [-9007199254740991, 1, 1],
  [9223372036854774784, -1024, 4294966272],
  [-9223372036854774784, 1024, 1024],
  [2 ** 63, 0, 0], [-(2 ** 63), 0, 0], [1e100, 0, 0],
  [NaN, 0, 0], [Infinity, 0, 0], [-Infinity, 0, 0],
];

for (const useLocal of [false, true]) {
  for (const [value, signed, unsigned] of cases) {
    const prefix = useLocal ? 'let x = 0; x = value;' : '';
    const compute = new Function(useLocal ? 'value' : 'x', prefix +
      'return [x | 0, x >>> 0, ~x, x << 1, x >> 1, x >>> 1, 1 << x, -1 >>> x];');
    for (let i = 0; i < 600; i++) compute(i & 31);
    assert.deepStrictEqual(compute(value), [
      signed, unsigned, ~signed, signed << 1, signed >> 1, unsigned >>> 1,
      1 << (unsigned & 31), -1 >>> (unsigned & 31),
    ], 'word conversion of ' + String(value));
  }
}

console.log('PASS signed and unsigned word conversion ranges');
