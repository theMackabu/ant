'use strict';
const assert = require('node:assert');

{
  let count = 0;
  const descriptors = Object.getOwnPropertyDescriptors(new Proxy({ a: 1, [Symbol.iterator]: 2 }, { ownKeys(t) { count++; return Reflect.ownKeys(t); } }));
  assert.strictEqual(count, 1);
  assert.deepStrictEqual(Reflect.ownKeys(descriptors), ['a', Symbol.iterator]);
}

console.log('ok');
