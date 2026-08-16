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

function captureColored(value) {
  const previousForceColor = process.env.FORCE_COLOR;
  const write = process.stdout.write;
  let output = '';

  process.env.FORCE_COLOR = '1';
  process.stdout.write = chunk => (output += chunk, true);
  console.log(value);
  process.stdout.write = write;

  if (previousForceColor === undefined) delete process.env.FORCE_COLOR;
  else process.env.FORCE_COLOR = previousForceColor;
  return output;
}

assert.strictEqual(
  captureColored(Buffer.from('Hello')),
  '\u001b[37m<\u001b[0mBuffer ' +
    '\u001b[33m48\u001b[0m \u001b[33m65\u001b[0m ' +
    '\u001b[33m6c\u001b[0m \u001b[33m6c\u001b[0m ' +
    '\u001b[33m6f\u001b[0m\u001b[37m>\u001b[0m\n'
);

const typedArrays = [
  new Int8Array([1]),
  new Uint8Array([1]),
  new Uint8ClampedArray([1]),
  new Int16Array([1]),
  new Uint16Array([1]),
  new Int32Array([1]),
  new Uint32Array([1]),
  new Float16Array([1]),
  new Float32Array([1]),
  new Float64Array([1]),
  new BigInt64Array([1n]),
  new BigUint64Array([1n]),
];

for (const value of typedArrays) {
  const output = captureColored(value);
  const name = value.constructor.name;
  assert.ok(output.startsWith(`${name}(\u001b[33m1\u001b[0m)`));
  assert.ok(output.includes(`\u001b[33m1${name.startsWith('Big') ? 'n' : ''}\u001b[0m`));
}

assert.ok(
  captureColored(Uint8Array.from([10, 255]).buffer).includes(
    '\u001b[37m<\u001b[33m0a\u001b[0m \u001b[33mff\u001b[0m\u001b[37m>'
  )
);

console.log('buffer inspection formatting and colors passed');
