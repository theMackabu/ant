'use strict';
const assert = require('node:assert');

{
  assert.deepStrictEqual(Reflect.ownKeys(new Proxy({}, { ownKeys() { return { length: 2, 0: 'p', 1: 'q' }; } })), ['p', 'q']);
  assert.throws(() => Reflect.ownKeys(new Proxy({}, { ownKeys() { return 5; } })), TypeError);
}

{
  const start = Date.now();
  assert.throws(() => Reflect.ownKeys(new Proxy({}, { ownKeys() { return { length: 3e6, 0: 'a' }; } })), TypeError);
  assert.throws(() => Reflect.ownKeys(new Proxy({}, { ownKeys() { return { length: 2 ** 32, 0: 'a' }; } })), RangeError);
  assert.deepStrictEqual(Reflect.ownKeys(new Proxy({}, { ownKeys() { return { length: 2, 0: 'a', 1: 'b' }; } })), ['a', 'b']);
  assert.ok(Date.now() - start < 1000, 'array-like ownKeys results are validated as they are read');
}

console.log('ok');
