'use strict';

const assert = require('node:assert');

function hex(value) {
  return Buffer.from(value).toString('hex');
}

function sentinelBuffer(length) {
  const buffer = Buffer.alloc(length);
  for (let i = 0; i < length; i++) buffer[i] = 0xaa;
  return buffer;
}

const cases = [
  ['\ud800', 'efbfbd'],
  ['\udc00', 'efbfbd'],
  ['a\ud800b', '61efbfbd62'],
  ['\ud800\udc00', 'f0908080'],
  ['é', 'c3a9']
];

for (const [value, expected] of cases) {
  assert.strictEqual(Buffer.from(value).toString('hex'), expected);
  assert.strictEqual(Buffer.from(value, 'utf8').toString('hex'), expected);
  assert.strictEqual(Buffer.from(value, 'utf-8').toString('hex'), expected);
  assert.strictEqual(hex(new TextEncoder().encode(value)), expected);

  const direct = sentinelBuffer(16);
  const directWritten = direct.write(value);
  assert.strictEqual(direct.subarray(0, directWritten).toString('hex'), expected);

  const explicit = sentinelBuffer(16);
  const explicitWritten = explicit.write(value, 0, 16, 'utf8');
  assert.strictEqual(explicit.subarray(0, explicitWritten).toString('hex'), expected);

  const encodingSecond = sentinelBuffer(16);
  const encodingSecondWritten = encodingSecond.write(value, 'utf8');
  assert.strictEqual(
    encodingSecond.subarray(0, encodingSecondWritten).toString('hex'),
    expected
  );

  const encodingThird = sentinelBuffer(16);
  const encodingThirdWritten = encodingThird.write(value, 0, 'utf-8');
  assert.strictEqual(
    encodingThird.subarray(0, encodingThirdWritten).toString('hex'),
    expected
  );
}

for (const capacity of [0, 1, 2]) {
  const buffer = sentinelBuffer(capacity);
  assert.strictEqual(buffer.write('\ud800'), 0);
  assert.strictEqual(buffer.toString('hex'), 'aa'.repeat(capacity));
}

{
  const buffer = sentinelBuffer(3);
  assert.strictEqual(buffer.write('\ud800'), 3);
  assert.strictEqual(buffer.toString('hex'), 'efbfbd');
}

{
  const buffer = sentinelBuffer(3);
  assert.strictEqual(buffer.write('a\ud800b'), 1);
  assert.strictEqual(buffer.toString('hex'), '61aaaa');
}

/* Buffer#indexOf intentionally searches string needles as Ant's internal
   WTF-8 bytes, matching Node rather than Buffer.from's UTF-8 export rules. */
assert.strictEqual(Buffer.from([0xed, 0xa0, 0x80]).indexOf('\ud800'), 0);
assert.strictEqual(Buffer.from([0xef, 0xbf, 0xbd]).indexOf('\ud800'), -1);

/* The validity bits share the existing metadata word with cached UTF-16
   length. Exercise both initialization orders on immutable strings. */
const lengthFirst = 'x\ud800y';
assert.strictEqual(lengthFirst.length, 3);
assert.strictEqual(Buffer.byteLength(lengthFirst), 5);
assert.strictEqual(Buffer.byteLength(lengthFirst), 5);
assert.strictEqual(Buffer.from(lengthFirst).toString('hex'), '78efbfbd79');
assert.strictEqual(lengthFirst.length, 3);

const validityFirst = 'x\udc00y';
assert.strictEqual(Buffer.from(validityFirst).toString('hex'), '78efbfbd79');
assert.strictEqual(validityFirst.length, 3);
assert.strictEqual(Buffer.from(validityFirst).toString('hex'), '78efbfbd79');

/* Separately-created halves are two WTF-8 triples internally, but together
   they are one JavaScript surrogate pair and must export as one scalar. */
const joinedPair = String.fromCharCode(0xd800) + String.fromCharCode(0xdc00);
assert.strictEqual(joinedPair.length, 2);
assert.strictEqual(Buffer.byteLength(joinedPair), 4);
assert.strictEqual(Buffer.byteLength(joinedPair), 4);
assert.strictEqual(Buffer.from(joinedPair).toString('hex'), 'f0908080');
assert.strictEqual(hex(new TextEncoder().encode(joinedPair)), 'f0908080');

for (const capacity of [0, 1, 2, 3]) {
  const buffer = sentinelBuffer(capacity);
  assert.strictEqual(buffer.write(joinedPair), 0);
  assert.strictEqual(buffer.toString('hex'), 'aa'.repeat(capacity));

  const encoded = sentinelBuffer(capacity);
  const result = new TextEncoder().encodeInto(joinedPair, encoded);
  assert.deepStrictEqual(result, { read: 0, written: 0 });
  assert.strictEqual(encoded.toString('hex'), 'aa'.repeat(capacity));
}

{
  const buffer = sentinelBuffer(4);
  assert.strictEqual(buffer.write(joinedPair), 4);
  assert.strictEqual(buffer.toString('hex'), 'f0908080');

  const encoded = sentinelBuffer(4);
  assert.deepStrictEqual(
    new TextEncoder().encodeInto(joinedPair, encoded),
    { read: 2, written: 4 }
  );
  assert.strictEqual(encoded.toString('hex'), 'f0908080');
}

console.log('buffer WTF-8 UTF-8 export tests passed');
