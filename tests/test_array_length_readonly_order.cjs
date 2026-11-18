'use strict';
const assert = require('node:assert');

const sloppyTruncate = new Function('a', 'a.length = -1; return a.length');

{
  const frozen = Object.freeze([1, 2]);
  assert.throws(() => { frozen.length = -1; }, TypeError);
  assert.throws(() => { frozen.length = 2n; }, TypeError);
  assert.throws(() => { frozen.length = 0; }, TypeError);
  assert.strictEqual(Reflect.set(frozen, 'length', -1), false);
  assert.strictEqual(Reflect.set(frozen, 'length', 0), false);
  assert.strictEqual(sloppyTruncate(frozen), 2);
  let coerced = 0;
  assert.throws(() => { frozen.length = { valueOf() { coerced++; return 1; } }; }, TypeError);
  assert.strictEqual(coerced, 0);
  assert.throws(() => Object.defineProperty(frozen, 'length', { value: -1 }), RangeError);

  const readOnly = Object.defineProperty([1, 2, 3], 'length', { writable: false });
  assert.throws(() => { readOnly.length = -1; }, TypeError);
  assert.strictEqual(Reflect.set(readOnly, 'length', 4294967296), false);

  const plain = [1, 2, 3];
  assert.throws(() => { plain.length = -1; }, RangeError);
  assert.throws(() => { plain.length = 1.5; }, RangeError);
  assert.throws(() => { plain.length = 4294967296; }, RangeError);
  assert.throws(() => { plain.length = 2n; }, TypeError);
  assert.throws(() => { plain.length = Symbol(); }, TypeError);
  assert.deepStrictEqual(plain, [1, 2, 3]);
  plain.length = '2';
  assert.deepStrictEqual(plain, [1, 2]);
  let calls = 0;
  plain.length = { valueOf() { calls++; return 1; } };
  assert.deepStrictEqual([plain.length, calls], [1, 2]);
}

console.log('ok');
