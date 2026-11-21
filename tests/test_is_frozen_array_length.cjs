'use strict';
const assert = require('node:assert');

{
  const integrity = (value) => [Object.isFrozen(value), Object.isSealed(value), Object.isExtensible(value)];
  assert.deepStrictEqual(integrity(Object.preventExtensions([])), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions([1])), [false, false, false]);
  assert.deepStrictEqual(integrity(Object.seal([1])), [false, true, false]);
  assert.deepStrictEqual(integrity(Object.freeze([1])), [true, true, false]);
  assert.deepStrictEqual(integrity([]), [false, false, true]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(Object.defineProperty([], 'a', { value: 1 }))), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(Object.defineProperty([], '0', { value: 1 }))), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions({})), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions({ a: 1 })), [false, false, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(new Uint8Array(0))), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(function () {})), [false, false, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions((function () { return arguments; })())), [false, false, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(Object.create(null))), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(new Map())), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(new String(''))), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(new String('a'))), [true, true, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions({ get a() { return 1; } })), [false, false, false]);
  assert.deepStrictEqual(integrity(Object.preventExtensions(Object.defineProperty({}, 'a', { value: 1 }))), [true, true, false]);
  for (const primitive of [1, 's', true, 1n, Symbol(), null, undefined])
    assert.deepStrictEqual(integrity(primitive), [true, true, false]);
}

console.log('ok');
