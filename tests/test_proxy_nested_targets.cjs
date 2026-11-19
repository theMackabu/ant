'use strict';
const assert = require('node:assert');

{
  const inner = new Proxy({ fixed: 1 }, {});
  Object.defineProperty(inner, 'locked', { value: 1 });
  const outer = new Proxy(inner, {
    preventExtensions: Reflect.preventExtensions,
    isExtensible: Reflect.isExtensible,
    getPrototypeOf: Reflect.getPrototypeOf,
    get() { return 'lie'; },
    has() { return false; },
    deleteProperty() { return true; },
    ownKeys() { return []; },
  });
  assert.strictEqual(Object.isExtensible(outer), true);
  assert.throws(() => outer.locked, TypeError);
  assert.throws(() => 'locked' in outer, TypeError);
  assert.throws(() => delete outer.locked, TypeError);
  assert.throws(() => Reflect.ownKeys(outer), TypeError);
  assert.strictEqual(outer.fixed, 'lie');
  Object.preventExtensions(outer);
  assert.strictEqual(Object.isExtensible(outer), false);
  assert.strictEqual(Object.isExtensible(inner), false);
  assert.strictEqual(Object.getPrototypeOf(outer), Object.prototype);
  assert.throws(() => 'fixed' in outer, TypeError);
  const plainOuter = new Proxy(new Proxy({}, {}), {});
  Object.preventExtensions(plainOuter);
  assert.strictEqual(Reflect.isExtensible(plainOuter), false);
}

{
  let chain = {};
  for (let i = 0; i < 10000; i++) chain = new Proxy(chain, { set: Reflect.set, getOwnPropertyDescriptor: Reflect.getOwnPropertyDescriptor });
  assert.throws(() => { chain.b = 1; }, RangeError);
}

{
  let deep = { x: 1 };
  for (let i = 0; i < 400; i++) deep = new Proxy(deep, {
    getOwnPropertyDescriptor() { return { value: 1, writable: true, enumerable: true, configurable: true }; },
  });
  assert.strictEqual(Object.getOwnPropertyDescriptor(deep, 'x').value, 1);
  assert.deepStrictEqual(Object.keys(deep), ['x']);
  deep.x = 2;
  let seen = 0;
  for (const key in deep) seen++;
  assert.strictEqual(seen, 1);
  assert.deepStrictEqual({ ...deep }, { x: 2 });
  assert.strictEqual(JSON.stringify(deep), '{"x":2}');

  const calls = [];
  const logged = (target) => new Proxy(target, {
    isExtensible(t) { calls.push('isExtensible'); return Reflect.isExtensible(t); },
    getOwnPropertyDescriptor(t, k) { calls.push('gopd:' + typeof k + ':' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k); },
  });
  assert.strictEqual(Object.getOwnPropertyDescriptor(logged(logged({ x: 1 })), 'q'), undefined);
  assert.deepStrictEqual(calls, ['gopd:string:q', 'gopd:string:q', 'gopd:string:q']);
}

console.log('ok');
