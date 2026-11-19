'use strict';
const assert = require('node:assert');

{
  const proto = { marker: 1 };
  let traps = 0;
  const p = new Proxy({}, { getPrototypeOf() { traps++; return proto; } });
  assert.strictEqual(Object.getPrototypeOf(p.constructor === Object ? new Proxy(function () {}, { getPrototypeOf() { return proto; } }).bind() : null), proto);
}

console.log('ok');
