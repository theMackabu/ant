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

{
  let calls = 0;
  const self = new Proxy({ a: 1 }, { getPrototypeOf() { calls++; return self; } });
  assert.throws(() => { for (const k in self); }, RangeError);
  assert.ok(calls > 1000);

  let hops = 0;
  const finite = new Proxy({ b: 1 }, { getPrototypeOf() { return ++hops > 2000 ? null : Object.create(finite); } });
  const seen = [];
  for (const k in finite) seen.push(k);
  assert.deepStrictEqual(seen, ['b']);
}

{
  const keys = (o) => { const out = []; for (const k in o) out.push(k); return out; };
  const hidden = Object.defineProperty({ shown: 1, alsoShown: 2 }, 'hidden', { value: 1, enumerable: false });
  assert.deepStrictEqual(keys(new Proxy(hidden, { ownKeys: Reflect.ownKeys })), ['shown', 'alsoShown']);
  assert.deepStrictEqual(keys(new Proxy(hidden, { ownKeys: () => ['hidden', 'shown', 'missing'] })), ['shown']);
  assert.deepStrictEqual(keys(new Proxy({ get acc() { throw new Error('read'); } }, { ownKeys: Reflect.ownKeys })), ['acc']);
  assert.deepStrictEqual(keys(new Proxy(new String('ab'), { ownKeys: Reflect.ownKeys })), ['0', '1']);
  assert.deepStrictEqual(keys(new Proxy([5, 6], { ownKeys: Reflect.ownKeys })), ['0', '1']);
  assert.deepStrictEqual(keys(new Proxy(new Proxy({ inner: 1 }, {}), { ownKeys: Reflect.ownKeys })), ['inner']);

  const handler = { ownKeys: Reflect.ownKeys };
  let prototypeReads = 0;
  handler.getPrototypeOf = (target) => {
    if (++prototypeReads === 1) handler.getOwnPropertyDescriptor = (t, k) => (k === 'b' ? { value: 2, enumerable: false, configurable: true } : Reflect.getOwnPropertyDescriptor(t, k));
    return Reflect.getPrototypeOf(target);
  };
  assert.deepStrictEqual(keys(new Proxy({ a: 1, b: 2, c: 3 }, handler)), ['a', 'c']);

  const lateHandler = { ownKeys: () => ['a', 'gone', 'b'] };
  const log = [];
  lateHandler.getPrototypeOf = (target) => {
    log.push('proto');
    if (log.length === 2) lateHandler.getOwnPropertyDescriptor = (t, k) => { log.push('desc ' + k); return Reflect.getOwnPropertyDescriptor(t, k); };
    return Reflect.getPrototypeOf(target);
  };
  assert.deepStrictEqual(keys(new Proxy({ a: 1, b: 2 }, lateHandler)), ['a', 'b']);
  assert.deepStrictEqual(log, ['proto', 'proto', 'desc b']);

  const midway = Proxy.revocable({ a: 1, b: 2 }, { ownKeys: Reflect.ownKeys, getPrototypeOf(target) { midway.revoke(); return null; } });
  assert.throws(() => keys(midway.proxy), TypeError);

  const { proxy, revoke } = Proxy.revocable({ a: 1, b: 2 }, { ownKeys: Reflect.ownKeys, getPrototypeOf(target) { return Reflect.getPrototypeOf(target); } });
  assert.deepStrictEqual(keys(proxy), ['a', 'b']);
  revoke();
  assert.throws(() => keys(proxy), TypeError);
}

{
  const keys = (o) => { const out = []; for (const k in o) out.push(k); return out; };
  const target = Object.create({ inherited: 1 }, { hidden: { value: 1, enumerable: false, configurable: true }, own: { value: 1, enumerable: true, configurable: true } });
  const handler = {};
  let proxy = target;
  for (let i = 0; i < 5; i++) proxy = new Proxy(proxy, i === 2 ? handler : {});
  for (let i = 0; i < 50; i++) assert.deepStrictEqual(keys(proxy), ['own', 'inherited']);
  target.later = 1;
  assert.deepStrictEqual(keys(proxy), ['own', 'later', 'inherited']);
  delete target.own;
  assert.deepStrictEqual(keys(proxy), ['later', 'inherited']);
  handler.ownKeys = () => ['later'];
  assert.deepStrictEqual(keys(proxy), ['later', 'inherited']);
  delete handler.ownKeys;
  handler.getPrototypeOf = () => null;
  assert.deepStrictEqual(keys(proxy), ['later']);
  delete handler.getPrototypeOf;
  assert.deepStrictEqual(keys(proxy), ['later', 'inherited']);

  assert.deepStrictEqual(keys(new Proxy([5, 6], {})), ['0', '1']);
  assert.deepStrictEqual(keys(new Proxy(new String('ab'), {})), ['0', '1']);
  assert.deepStrictEqual(keys(new Proxy(Object.assign(function () {}, { p: 1 }), {})), ['p']);
  const wrapped = new Uint8Array(2);
  assert.deepStrictEqual(keys(new Proxy(wrapped, {})), ['0', '1']);

  const revocable = Proxy.revocable({ a: 1 }, {});
  const outer = new Proxy(revocable.proxy, {});
  assert.deepStrictEqual(keys(outer), ['a']);
  revocable.revoke();
  assert.throws(() => keys(outer), TypeError);
}

console.log('ok');
