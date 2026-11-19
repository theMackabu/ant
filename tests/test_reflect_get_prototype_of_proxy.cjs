'use strict';
const assert = require('node:assert');

{
  assert.strictEqual(Reflect.getPrototypeOf(new Proxy({}, {})), Object.prototype);
  const proto = { marker: 1 };
  let traps = 0;
  const p = new Proxy({}, { getPrototypeOf() { traps++; return proto; } });
  assert.strictEqual(Reflect.getPrototypeOf(p), proto);
  assert.strictEqual(traps, 1);
}

console.log('ok');
