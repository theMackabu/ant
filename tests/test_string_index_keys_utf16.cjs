// string index keys count UTF-16 code units, not UTF-8 bytes
const assert = require('node:assert');

const forIn = value => {
  const keys = [];
  for (const key in value) keys.push(key);
  return keys;
};

assert.deepStrictEqual(forIn('é😀'), ['0', '1', '2']);
assert.deepStrictEqual(forIn(new String('é😀')), ['0', '1', '2']);
assert.deepStrictEqual(Object.keys('é😀'), ['0', '1', '2']);
assert.deepStrictEqual(Object.keys(new String('é😀')), ['0', '1', '2']);
assert.deepStrictEqual(forIn('abc'), ['0', '1', '2']);

console.log('string index keys utf16 OK');
