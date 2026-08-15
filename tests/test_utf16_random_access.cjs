const assert = require('node:assert');

const left = 'é'.repeat(4_096) + 'tail';
const right = 'Ω'.repeat(4_096) + 'tail';

for (let i = 0; i < 2_000; i++) {
  const value = (i & 1) === 0 ? left : right;
  assert.strictEqual(value.endsWith('tail'), true);
  assert.strictEqual(value.endsWith('Ω', value.length - 4), (i & 1) !== 0);
}

const pattern = 'aé𝄞Ω';
const mixed = pattern.repeat(2_000);
const offset = pattern.length * 1_337;

assert.strictEqual(mixed.charCodeAt(offset), 0x61);
assert.strictEqual(mixed.charCodeAt(offset + 1), 0xe9);
assert.strictEqual(mixed.charCodeAt(offset + 2), 0xd834);
assert.strictEqual(mixed.charCodeAt(offset + 3), 0xdd1e);
assert.strictEqual(mixed.codePointAt(offset + 2), 0x1d11e);
assert.strictEqual(mixed.slice(offset, offset + pattern.length), pattern);
assert.strictEqual(mixed.slice(offset + 2, offset + 3), '\uD834');
assert.strictEqual(mixed.slice(offset + 3, offset + 4), '\uDD1E');
assert.strictEqual(mixed.startsWith('\uD834', offset + 2), true);
assert.strictEqual(mixed.startsWith('\uDD1E', offset + 3), true);
assert.strictEqual(mixed.endsWith('\uD834', offset + 3), true);
assert.strictEqual(mixed.endsWith('\uDD1E', offset + 4), true);

for (let round = 0; round < 12; round++) {
  let transient = `${round}:` + 'éΩ'.repeat(4_096) + ':done';
  assert.strictEqual(transient.endsWith(':done'), true);
  assert.strictEqual(transient.endsWith(':done'), true);
  transient = null;

  const churn = [];
  for (let i = 0; i < 10_000; i++) churn.push({ round, i, text: `${round}:${i}` });

  assert.strictEqual(left.endsWith('tail'), true);
  assert.strictEqual(right.endsWith('tail'), true);
  assert.strictEqual(mixed.codePointAt(offset + 2), 0x1d11e);
}

console.log('utf16 random access: ok');
