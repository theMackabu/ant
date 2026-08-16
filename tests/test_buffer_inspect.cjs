const assert = require('node:assert');
const { inspect } = require('node:util');

assert.strictEqual(inspect(Buffer.alloc(0)), '<Buffer >');
assert.strictEqual(inspect(Buffer.from('Hello')), '<Buffer 48 65 6c 6c 6f>');

const long = Buffer.alloc(52);
for (let i = 0; i < long.length; i++) long[i] = i;
assert.strictEqual(
  inspect(long),
  '<Buffer 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19 1a 1b 1c 1d 1e 1f 20 21 22 23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f 30 31 ... 2 more bytes>'
);

assert.strictEqual(
  inspect(new Uint8Array([72, 101])),
  'Uint8Array(2) [ 72, 101 ]'
);

console.log('buffer inspection matches Node');
