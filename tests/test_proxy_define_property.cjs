const assert = require('node:assert');

const getter = function () { return 1; };
const setter = function (v) {};

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

{
  const { proxy, seen } = recording({}, 'forward');
  Object.defineProperty(proxy, 'a', { value: 1 });
  assert.strictEqual(seen[0].plain, true);
  assert.strictEqual(typeof seen[0].desc.hasOwnProperty, 'function');
  assert.strictEqual(String(seen[0].desc), '[object Object]');
  assert.ok(seen[0].desc instanceof Object);
  assert.deepStrictEqual(seen[0].keys, ['value']);
  assert.ok(Object.isExtensible(seen[0].desc));
}

{
  const { proxy, seen } = recording({}, 'forward');
  Object.defineProperty(proxy, 'a', { value: 1, writable: 'yes', enumerable: 1, configurable: {} });
  Reflect.defineProperty(proxy, 'b', { value: 1, writable: '', enumerable: 0, configurable: null });
  Object.defineProperty(proxy, 'c', { value: 1, enumerable: undefined });
  Object.defineProperty(proxy, 'd', { get: getter, set: undefined, configurable: 'x' });
  Object.defineProperty(proxy, 'e', Object.create({ value: 9, enumerable: 'inherited' }));
  Object.defineProperty(proxy, 'f', { value: 1, extra: 2 });
  Object.defineProperty(proxy, 'g', {});
  assert.deepStrictEqual(seen.map(s => s.desc), [
    { value: 1, writable: true, enumerable: true, configurable: true },
    { value: 1, writable: false, enumerable: false, configurable: false },
    { value: 1, enumerable: false },
    { get: getter, set: undefined, configurable: true },
    { value: 9, enumerable: true },
    { value: 1 },
    {},
  ]);
  assert.deepStrictEqual(seen.map(s => s.keys), [
    ['value', 'writable', 'enumerable', 'configurable'],
    ['value', 'writable', 'enumerable', 'configurable'],
    ['value', 'enumerable'],
    ['get', 'set', 'configurable'],
    ['value', 'enumerable'],
    ['value'],
    [],
  ]);
  assert.ok(seen.every(s => s.plain));
}

{
  const { proxy, seen } = recording({ a: 1 }, 'forward');
  proxy.a = 5;
  proxy.b = 6;
  Object.defineProperties(proxy, { c: { value: 7 }, d: { get: getter, enumerable: 1 } });
  assert.deepStrictEqual(seen.map(s => [s.key, s.desc, s.plain]), [
    ['a', { value: 5 }, true],
    ['b', { value: 6, writable: true, enumerable: true, configurable: true }, true],
    ['c', { value: 7 }, true],
    ['d', { get: getter, enumerable: true }, true],
  ]);
}

{
  for (const result of [false, 0, '', null, undefined]) {
    const { proxy } = recording({}, result);
    assert.throws(() => Object.defineProperty(proxy, 'a', { value: 1 }), TypeError);
    assert.strictEqual(Reflect.defineProperty(proxy, 'a', { value: 1 }), false);
    assert.throws(() => Object.defineProperties(proxy, { a: { value: 1 } }), TypeError);
    assert.throws(() => Object.freeze(new Proxy({ a: 1 }, { defineProperty: () => result })), TypeError);
    assert.throws(() => Object.seal(new Proxy({ a: 1 }, { defineProperty: () => result })), TypeError);
    proxy.a = 1;
    assert.throws(() => { 'use strict'; proxy.a = 1; }, TypeError);
  }
  const thrower = new Proxy({}, { defineProperty() { throw new RangeError('trap'); } });
  assert.throws(() => Object.defineProperty(thrower, 'a', { value: 1 }), RangeError);
  assert.throws(() => Reflect.defineProperty(thrower, 'a', { value: 1 }), RangeError);
  assert.throws(() => { thrower.a = 1; }, RangeError);
}

{
  const lying = target => new Proxy(target, { defineProperty: () => 1 });
  const ok = (target, key, desc) => {
    assert.strictEqual(Reflect.defineProperty(lying(target), key, desc), true);
    assert.strictEqual(Object.defineProperty(lying(target), key, desc) !== undefined, true);
  };
  const bad = (target, key, desc) => {
    assert.throws(() => Reflect.defineProperty(lying(target), key, desc), TypeError);
    assert.throws(() => Object.defineProperty(lying(target), key, desc), TypeError);
  };

  ok({}, 'a', { value: 1 });
  ok({ a: 1 }, 'a', { value: 2, writable: false });
  bad({}, 'a', { value: 1, configurable: false });
  bad({ a: 1 }, 'a', { configurable: false });
  bad(Object.preventExtensions({}), 'a', { value: 1 });
  ok(Object.preventExtensions({ a: 1 }), 'a', { value: 2 });

  const writable = () => nonConfigurable({ value: 1, writable: true, enumerable: true });
  ok(writable(), 'a', { value: 2 });
  ok(writable(), 'a', { configurable: false });
  ok(writable(), 'a', { enumerable: true });
  ok(writable(), 'a', {});
  bad(writable(), 'a', { writable: false });
  bad(writable(), 'a', { value: 1, writable: false, configurable: false });
  bad(writable(), 'a', { configurable: true });
  bad(writable(), 'a', { enumerable: false });
  bad(writable(), 'a', { get: getter });

  const readOnly = () => nonConfigurable({ value: 1, writable: false, enumerable: false });
  ok(readOnly(), 'a', { value: 1 });
  ok(readOnly(), 'a', { writable: false });
  ok(readOnly(), 'a', { value: 1, writable: false, enumerable: false, configurable: false });
  bad(readOnly(), 'a', { value: 2 });
  bad(readOnly(), 'a', { writable: true });
  bad(readOnly(), 'a', { enumerable: true });
  bad(readOnly(), 'a', { set: setter });
  ok(nonConfigurable({ value: NaN }), 'a', { value: NaN });
  bad(nonConfigurable({ value: 0 }), 'a', { value: -0 });

  const accessor = () => nonConfigurable({ get: getter, set: setter });
  ok(accessor(), 'a', { get: getter });
  ok(accessor(), 'a', { set: setter, enumerable: false });
  bad(accessor(), 'a', { get() {} });
  bad(accessor(), 'a', { set: undefined });
  bad(accessor(), 'a', { value: 1 });
  bad(accessor(), 'a', { writable: false });
  bad(Object.freeze({ a: 1 }), 'b', {});
  bad(Object.freeze({ a: 1 }), 'a', { value: 2 });
  ok(Object.freeze({ a: 1 }), 'a', { value: 1 });
  ok(Object.seal({ a: 1 }), 'a', { value: 2 });
}

{
  const mutating = new Proxy(nonConfigurable({ value: 1, writable: false }), {
    defineProperty(t, k, desc) { desc.value = 1; delete desc.configurable; return true; },
  });
  assert.throws(() => Reflect.defineProperty(mutating, 'a', { value: 2 }), TypeError);
  assert.throws(() => Reflect.defineProperty(mutating, 'b', { value: 2, configurable: false }), TypeError);
}

{
  const trapless = target => new Proxy(target, {});
  assert.strictEqual(Reflect.defineProperty(trapless(Object.freeze({ a: 1 })), 'a', { value: 2 }), false);
  assert.strictEqual(Reflect.defineProperty(trapless(Object.freeze({ a: 1 })), 'b', { value: 2 }), false);
  assert.strictEqual(Reflect.defineProperty(trapless(Object.freeze({ a: 1 })), 'a', { value: 1 }), true);
  assert.strictEqual(Reflect.defineProperty(trapless(Object.preventExtensions({})), 'a', {}), false);
  assert.strictEqual(Reflect.defineProperty(trapless(nonConfigurable({ get: getter })), 'a', { value: 1 }), false);
  assert.strictEqual(Reflect.defineProperty(trapless(trapless(Object.freeze({}))), 'a', { value: 1 }), false);
  assert.throws(() => Object.defineProperty(trapless(Object.freeze({ a: 1 })), 'a', { value: 2 }), TypeError);
  assert.throws(() => Reflect.defineProperty(trapless({}), 'a', { get: 1 }), TypeError);
  assert.throws(() => Reflect.defineProperty(trapless({}), 'a', { get: getter, value: 1 }), TypeError);
  const target = {};
  assert.strictEqual(Reflect.defineProperty(trapless(target), 'a', { value: 1, enumerable: 'yes' }), true);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(target, 'a'), { value: 1, writable: false, enumerable: true, configurable: false });
}

for (const forward of [true, false]) {
  const make = () => {
    const target = { data: 1, [Symbol.for('sym')]: 2 };
    Object.defineProperty(target, 'acc', { get: getter, set: setter, enumerable: true, configurable: true });
    return target;
  };
  const wrap = (target, seen) => new Proxy(target, forward ? {
    defineProperty(t, key, desc) { seen.push([key, desc, Object.getPrototypeOf(desc) === Object.prototype, Reflect.ownKeys(desc)]); return Reflect.defineProperty(t, key, desc); },
  } : {});

  let seen = [];
  let target = make();
  let proxy = wrap(target, seen);
  assert.strictEqual(Object.freeze(proxy), proxy);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(target, 'acc'), { get: getter, set: setter, enumerable: true, configurable: false });
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(target, 'data'), { value: 1, writable: false, enumerable: true, configurable: false });
  assert.ok(Object.isFrozen(target));
  assert.ok(Object.isFrozen(proxy));
  if (forward) assert.deepStrictEqual(seen, [
    ['data', { writable: false, configurable: false }, true, ['writable', 'configurable']],
    ['acc', { configurable: false }, true, ['configurable']],
    [Symbol.for('sym'), { writable: false, configurable: false }, true, ['writable', 'configurable']],
  ]);

  seen = [];
  target = make();
  proxy = wrap(target, seen);
  assert.strictEqual(Object.seal(proxy), proxy);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(target, 'acc'), { get: getter, set: setter, enumerable: true, configurable: false });
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(target, 'data'), { value: 1, writable: true, enumerable: true, configurable: false });
  assert.ok(Object.isSealed(proxy));
  assert.ok(!Object.isFrozen(proxy));
  if (forward) assert.deepStrictEqual(seen.map(s => [s[0], s[1], s[2]]), [
    ['data', { configurable: false }, true],
    ['acc', { configurable: false }, true],
    [Symbol.for('sym'), { configurable: false }, true],
  ]);

  seen = [];
  target = make();
  proxy = wrap(target, seen);
  assert.strictEqual(Object.preventExtensions(proxy), proxy);
  assert.strictEqual(seen.length, 0);
  assert.ok(!Object.isExtensible(target));
  assert.throws(() => Object.defineProperty(proxy, 'fresh', { value: 1 }), TypeError);
  assert.strictEqual(Reflect.defineProperty(proxy, 'fresh', { value: 1 }), false);
  assert.strictEqual(Reflect.defineProperty(proxy, 'data', { value: 3 }), true);
  assert.strictEqual(target.data, 3);
}

{
  const accessorOnly = {};
  Object.defineProperty(accessorOnly, 'acc', { get: getter, configurable: true });
  const frozen = Object.freeze(new Proxy(accessorOnly, {}));
  assert.ok(Object.isFrozen(frozen));
  assert.strictEqual(Object.getOwnPropertyDescriptor(accessorOnly, 'acc').get, getter);
}

{
  const seen = [];
  const trap = { defineProperty(target, key, desc) { seen.push(Object.keys(desc).join()); return Reflect.defineProperty(target, key, desc); } };
  const reads = [];
  const watched = new Proxy({ configurable: true, enumerable: true, value: 1, writable: true }, {
    has(target, key) { reads.push(key); return Reflect.has(target, key); },
  });
  Object.defineProperty(new Proxy({}, trap), 'x', { enumerable: true, writable: true, configurable: true, value: 1 });
  Object.defineProperty(new Proxy({}, trap), 'x', { set(v) {}, configurable: true, get() {}, enumerable: false });
  Reflect.defineProperty(new Proxy({}, trap), 'x', { configurable: true, value: 2 });
  Object.defineProperties(new Proxy({}, trap), { x: { enumerable: true, value: 2 } });
  Object.defineProperty(new Proxy({}, trap), 'x', watched);
  assert.deepStrictEqual(seen, [
    'value,writable,enumerable,configurable', 'get,set,enumerable,configurable', 'value,configurable', 'value,enumerable',
    'value,writable,enumerable,configurable',
  ]);
  assert.deepStrictEqual(reads, ['enumerable', 'configurable', 'value', 'writable', 'get', 'set']);
}

console.log('ok');
