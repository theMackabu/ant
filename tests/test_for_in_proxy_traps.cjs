'use strict';
const assert = require('node:assert');

{
  const keys = (o) => { const out = []; for (const k in o) out.push(k); return out; };
  assert.deepStrictEqual(keys(new Proxy(Object.create({ inh: 1 }), {})), ['inh']);
  assert.deepStrictEqual(keys(Object.create(new Proxy({ viaProxy: 1 }, {}), { own: { value: 1, enumerable: true } })), ['own', 'viaProxy']);
  const log = [];
  const handler = {};
  for (const trap of ['ownKeys', 'getPrototypeOf', 'getOwnPropertyDescriptor']) handler[trap] = (...args) => { log.push(trap); return Reflect[trap](...args); };
  assert.deepStrictEqual(keys(new Proxy({ a: 1 }, handler)), ['a']);
  assert.deepStrictEqual(log, ['ownKeys', 'getPrototypeOf', 'getOwnPropertyDescriptor']);
}

console.log('ok');
