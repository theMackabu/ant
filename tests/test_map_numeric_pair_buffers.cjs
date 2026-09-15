const assert = require('node:assert');

const pairs = [
  [0, -0], [149, 39], [-2147483648, 2147483647],
  [-Number.MAX_VALUE, Number.MIN_VALUE], [1e-7, 1.2345678901234567e-6],
  [NaN, Infinity], [-Infinity, -1e21],
];
const separators = [0, 1, 31, 32, 63, 64, 65, 94, 95, 96, 97, 126, 127, 128, 200, 1024]
  .map(length => ':'.repeat(length));
separators.push('λ'.repeat(55), '\0'.repeat(80));

for (const separator of separators) {
  const escaped = JSON.stringify(separator).slice(1, -1)
    .replace(/`/g, '\\`').replace(/\$\{/g, '\\${');
  const lookup = new Function('map', 'left', 'right',
    'return map.get(`${left}' + escaped + '${right}`);');
  const map = new Map();
  for (let i = 0; i < pairs.length; i++) {
    const [left, right] = pairs[i];
    map.set(String(left) + separator + String(right), i);
  }
  for (let round = 0; round < 600; round++) {
    for (let i = 0; i < pairs.length; i++) {
      const [left, right] = pairs[i];
      assert.strictEqual(lookup(map, left, right), i);
    }
  }
  assert.strictEqual(lookup(map, 12345, 67890), undefined);
}
console.log('PASS numeric Map pairs preserve short, boundary, long and binary separators');
