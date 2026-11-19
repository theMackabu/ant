'use strict';
const assert = require('node:assert');

{
  const p = new Proxy({ q: 1 }, {});
  Object.defineProperty(p, 'hidden', { value: 1, enumerable: false });
  assert.strictEqual(Object.prototype.propertyIsEnumerable.call(p, 'q'), true);
  assert.strictEqual(Object.prototype.propertyIsEnumerable.call(p, 'hidden'), false);
  assert.strictEqual(Object.prototype.propertyIsEnumerable.call(p, 'missing'), false);
  const log = [];
  Object.prototype.propertyIsEnumerable.call(new Proxy({ q: 1 }, { getOwnPropertyDescriptor(t, k) { log.push(k); return Reflect.getOwnPropertyDescriptor(t, k); } }), 'q');
  assert.deepStrictEqual(log, ['q']);
}

{
  const calls = [];
  const logged = (target) => new Proxy(target, {
    isExtensible(t) { calls.push('isExtensible'); return Reflect.isExtensible(t); },
    getOwnPropertyDescriptor(t, k) { calls.push('gopd:' + typeof k + ':' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
  });
  calls.length = 0;
  assert.strictEqual(Object.prototype.propertyIsEnumerable.call(logged({ 1: 2 }), 1), true);
  assert.deepStrictEqual(calls, ['gopd:string:1']);
}

console.log('ok');
