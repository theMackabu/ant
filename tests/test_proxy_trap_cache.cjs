const assert = require('node:assert');

function probe(p) {
  const out = [];
  const step = f => { try { out.push(f()); } catch (e) { out.push(e.constructor.name); } };
  step(() => p.a);
  step(() => 'a' in p);
  step(() => { p.a = 5; return p.a; });
  step(() => Object.keys(p).join());
  step(() => { const keys = []; for (const k in p) keys.push(k); return keys.join(); });
  step(() => JSON.stringify(Object.getOwnPropertyDescriptor(p, 'a')));
  step(() => Object.getPrototypeOf(p) === Object.prototype);
  step(() => Object.isExtensible(p));
  step(() => delete p.zz);
  return out;
}

const plain = [1, true, 5, 'a,b', 'a,b', '{"value":5,"writable":true,"enumerable":true,"configurable":true}', true, true, true];

{
  const handler = {};
  const p = new Proxy({ a: 1, b: 2 }, handler);
  for (let i = 0; i < 3; i++) assert.deepStrictEqual(probe(p), i ? [5, ...plain.slice(1)] : plain);

  handler.get = () => 'trapped';
  assert.strictEqual(p.a, 'trapped');
  handler.has = () => false;
  assert.strictEqual('a' in p, false);
  handler.ownKeys = () => ['b'];
  assert.strictEqual(Object.keys(p).join(), 'b');
  const seen = [];
  for (const k in p) seen.push(k);
  assert.deepStrictEqual(seen, ['b']);
  let stored;
  handler.set = (t, k, v) => { stored = [k, v]; return true; };
  p.a = 9;
  assert.deepStrictEqual(stored, ['a', 9]);

  delete handler.get;
  assert.strictEqual(p.a, 5);
  handler.get = undefined;
  assert.strictEqual(p.a, 5);
  handler.get = null;
  assert.strictEqual(p.a, 5);
  handler.get = 1;
  assert.throws(() => p.a, TypeError);
  Object.defineProperty(handler, 'get', { get() { return () => 'from getter'; }, configurable: true });
  assert.strictEqual(p.a, 'from getter');
  delete handler.get;
  delete handler.has;
  delete handler.ownKeys;
  delete handler.set;
  assert.deepStrictEqual(probe(p), [5, ...plain.slice(1)]);
}

{
  const proto = {};
  const handler = Object.create(proto);
  const p = new Proxy({ a: 1, b: 2 }, handler);
  assert.deepStrictEqual(probe(p), plain);
  proto.get = () => 'inherited';
  assert.strictEqual(p.a, 'inherited');
  delete proto.get;
  assert.strictEqual(p.a, 5);
  Object.setPrototypeOf(handler, { has: () => false });
  assert.strictEqual('a' in p, false);
  Object.setPrototypeOf(handler, null);
  assert.strictEqual('a' in p, true);
}

{
  const p = new Proxy({ a: 1, b: 2 }, {});
  assert.deepStrictEqual(probe(p), plain);
  Object.prototype.get = () => 'from Object.prototype';
  try {
    assert.strictEqual(p.a, 'from Object.prototype');
  } finally {
    delete Object.prototype.get;
  }
  assert.strictEqual(p.a, 5);
}

{
  class Handler { get(t, k) { return k === 'a' ? 'class' : Reflect.get(t, k); } }
  const p = new Proxy({ a: 1, b: 2 }, new Handler());
  for (let i = 0; i < 3; i++) assert.strictEqual(p.a, 'class');
  assert.strictEqual(p.b, 2);
  Handler.prototype.get = () => 'replaced';
  assert.strictEqual(p.b, 'replaced');
}

{
  const log = [];
  const handler = new Proxy({}, { get(t, k) { log.push(k); return undefined; } });
  const p = new Proxy({ a: 1 }, handler);
  assert.strictEqual(p.a, 1);
  assert.strictEqual(p.a, 1);
  assert.deepStrictEqual(log, ['get', 'get']);
}

{
  const { proxy, revoke } = Proxy.revocable({ a: 1 }, {});
  assert.strictEqual(proxy.a, 1);
  const keys = [];
  for (const k in proxy) keys.push(k);
  assert.deepStrictEqual(keys, ['a']);
  revoke();
  assert.throws(() => proxy.a, TypeError);
  assert.throws(() => { for (const k in proxy) keys.push(k); }, TypeError);
  assert.throws(() => { proxy.a = 1; }, TypeError);
}

{
  const target = {};
  Object.defineProperty(target, 'ro', { value: 1, writable: false, configurable: true, enumerable: true });
  let hits = 0;
  Object.defineProperty(target, 'acc', { set(v) { hits += v; }, get() { return hits; }, configurable: true });
  target.w = 1;
  const p = new Proxy(target, {});
  p.ro = 2;
  assert.strictEqual(target.ro, 1);
  assert.throws(() => { 'use strict'; p.ro = 2; }, TypeError);
  p.acc = 3;
  assert.strictEqual(hits, 3);
  p.w = 4;
  assert.strictEqual(target.w, 4);
  p.fresh = 5;
  assert.strictEqual(target.fresh, 5);
  Object.freeze(target);
  p.w = 6;
  assert.strictEqual(target.w, 4);
  assert.strictEqual(Reflect.set(p, 'w', 7), false);
  const child = Object.create(p);
  child.own = 1;
  assert.strictEqual(Object.hasOwn(child, 'own'), true);
  assert.strictEqual(Object.hasOwn(target, 'own'), false);
}

{
  const inner = new Proxy({ a: 1, b: 2 }, {});
  const outer = new Proxy(inner, {});
  const keys = [];
  for (const k in outer) keys.push(k);
  assert.deepStrictEqual(keys, ['a', 'b']);
  const base = Object.create({ inherited: 1 }, { hidden: { value: 1, enumerable: false }, shown: { value: 1, enumerable: true } });
  const viaProxy = [];
  for (const k in new Proxy(base, {})) viaProxy.push(k);
  assert.deepStrictEqual(viaProxy, ['shown', 'inherited']);
  const order = [];
  const traced = new Proxy(base, { getPrototypeOf(t) { order.push('getPrototypeOf'); return Reflect.getPrototypeOf(t); } });
  const viaTrap = [];
  for (const k in traced) viaTrap.push(k);
  assert.deepStrictEqual(viaTrap, ['shown', 'inherited']);
  assert.ok(order.length >= 1);
}

console.log('ok');
