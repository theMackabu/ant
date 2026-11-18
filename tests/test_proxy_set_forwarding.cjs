const assert = require('node:assert');

const getter = function () { return 1; };

function recording(target, result) {
  const seen = [];
  const proxy = new Proxy(target, {
    defineProperty(t, key, desc) {
      seen.push({ key, desc, plain: Object.getPrototypeOf(desc) === Object.prototype, keys: Reflect.ownKeys(desc) });
      return result === 'forward' ? Reflect.defineProperty(t, key, desc) : result;
    },
  });
  return { proxy, seen };
}

function nonConfigurable(desc) {
  const target = {};
  Object.defineProperty(target, 'a', { ...desc, configurable: false });
  return target;
}

(function () {
  'use strict';

  function logged(handler, log) {
    return new Proxy(handler, { get(h, name) { log.push(String(name)); return h[name]; } });
  }

  {
    const log = [];
    const p = new Proxy({}, logged({ defineProperty: Reflect.defineProperty }, log));
    p.x = 1;
    assert.deepStrictEqual(log, ['set', 'getOwnPropertyDescriptor', 'defineProperty']);
    assert.strictEqual(p.x, 1);
  }

  {
    const sym = Symbol('s');
    const refused = new Proxy({}, { defineProperty() { return false; } });
    assert.throws(() => { refused[sym] = 1; }, TypeError);
    assert.throws(() => { refused.x = 1; }, TypeError);
    const calls = [];
    const target = {};
    const p = new Proxy(target, { defineProperty(t, k, d) { calls.push(k); return Reflect.defineProperty(t, k, d); } });
    p[sym] = 2;
    assert.deepStrictEqual(calls, [sym]);
    assert.strictEqual(target[sym], 2);
  }

  {
    const p = new Proxy({}, { getOwnPropertyDescriptor() { return { value: 1, configurable: true }; } });
    assert.throws(() => { p.x = 5; }, TypeError);
    let seen = 0;
    const counted = new Proxy({ x: 1 }, { getOwnPropertyDescriptor(t, k) { seen++; return Reflect.getOwnPropertyDescriptor(t, k); } });
    Object.create(counted).x = 2;
    assert.strictEqual(Reflect.set(counted, 'x', 3, {}), true);
    assert.strictEqual(seen, 0);
    counted.x = 4;
    assert.strictEqual(seen, 1);
    assert.strictEqual(counted.x, 4);
  }
})();

{
  const readOnly = nonConfigurable({ value: 1, writable: false });
  const accessor = {};
  Object.defineProperty(accessor, 'a', { get: getter, set(v) { this.stored = v; }, configurable: true });
  const setterless = nonConfigurable({ get: getter });
  for (const target of [Object.freeze({ a: 1 }), readOnly, setterless]) {
    const { proxy, seen } = recording(target, true);
    proxy.a = 5;
    assert.strictEqual(Reflect.set(proxy, 'a', 5), false);
    assert.throws(() => { 'use strict'; proxy.a = 5; }, TypeError);
    assert.strictEqual(seen.length, 0);
  }
  const { proxy, seen } = recording(accessor, true);
  proxy.a = 5;
  assert.strictEqual(seen.length, 1);
  assert.deepStrictEqual([seen[0].key, seen[0].desc], ['stored', { value: 5, writable: true, enumerable: true, configurable: true }]);
  assert.strictEqual(Object.getOwnPropertyDescriptor(accessor, 'a').get, getter);
}

console.log('ok');
