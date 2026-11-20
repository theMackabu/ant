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

(function () {
  'use strict';
  const plain = [1, 2, 3];
  const proxy = new Proxy(plain, {});
  proxy[1] = 'b';
  proxy[5] = 'f';
  assert.deepStrictEqual(plain, [1, 'b', 3, , , 'f']);
  assert.strictEqual(plain.length, 6);
  proxy.length = 2;
  assert.deepStrictEqual(plain, [1, 'b']);

  const literal = () => [1, 2, 3];
  for (let i = 0; i < 2000; i++) literal();
  const first = literal();
  new Proxy(first, {})[0] = 'changed';
  assert.deepStrictEqual(first, ['changed', 2, 3]);
  assert.deepStrictEqual(literal(), [1, 2, 3]);

  const frozen = Object.freeze([1, 2]);
  assert.throws(() => { new Proxy(frozen, {})[0] = 9; }, TypeError);
  assert.deepStrictEqual(frozen, [1, 2]);

  const sealed = Object.seal([1, 2]);
  new Proxy(sealed, {})[0] = 9;
  assert.deepStrictEqual(sealed, [9, 2]);
  assert.throws(() => { new Proxy(sealed, {})[2] = 9; }, TypeError);

  const readonly = [1, 2];
  Object.defineProperty(readonly, 0, { value: 1, writable: false });
  assert.throws(() => { new Proxy(readonly, {})[0] = 9; }, TypeError);
  assert.strictEqual(readonly[0], 1);

  const seen = [];
  const holey = [1, , 3];
  Object.setPrototypeOf(holey, Object.create(Array.prototype, { 1: { set(v) { seen.push(v); }, configurable: true } }));
  new Proxy(holey, {})[1] = 'through';
  assert.deepStrictEqual(seen, ['through']);
  assert.strictEqual(Object.hasOwn(holey, 1), false);

  const accessor = [1, 2];
  Object.defineProperty(accessor, 0, { set(v) { seen.push('own ' + v); }, configurable: true });
  new Proxy(accessor, {})[0] = 'x';
  assert.deepStrictEqual(seen, ['through', 'own x']);

  const args = (function () { new Proxy(arguments, {})[0] = 'stored'; return arguments[0]; })('original');
  assert.strictEqual(args, 'stored');

  const typed = new Uint8Array(2);
  new Proxy(typed, {})[0] = 300;
  assert.strictEqual(typed[0], 44);

  const nested = [1, 2];
  new Proxy(new Proxy(nested, {}), {})[1] = 'deep';
  assert.deepStrictEqual(nested, [1, 'deep']);

  const strange = [1, 2];
  const sp = new Proxy(strange, {});
  sp['01'] = 'a';
  sp['1.0'] = 'b';
  sp['-0'] = 'c';
  sp['4294967295'] = 'd';
  assert.deepStrictEqual([...strange], [1, 2]);
  assert.deepStrictEqual(Object.keys(strange), ['0', '1', '01', '1.0', '-0', '4294967295']);

  const hot = [0, 0, 0];
  const hp = new Proxy(hot, {});
  for (let i = 0; i < 20000; i++) hp[i % 3] = i;
  assert.deepStrictEqual(hot, [19998, 19999, 19997]);
  Object.freeze(hot);
  assert.throws(() => { hp[0] = -1; }, TypeError);

  const grown = [];
  const gp = new Proxy(grown, {});
  for (let i = 0; i < 100; i++) assert.strictEqual(gp.push(i), i + 1);
  assert.strictEqual(grown.length, 100);
  assert.strictEqual(grown[99], 99);
  gp[100] = 'next';
  gp[102] = 'gap';
  assert.strictEqual(grown.length, 103);
  assert.strictEqual(Object.hasOwn(grown, 101), false);

  const fixed = [1, 2];
  fixed.length = 2;
  Object.defineProperty(fixed, 'length', { writable: false });
  assert.throws(() => { new Proxy(fixed, {})[2] = 3; }, TypeError);
  assert.deepStrictEqual(fixed, [1, 2]);

  const trapped = [];
  Object.defineProperty(Array.prototype, 3, { set(v) { trapped.push(v); }, configurable: true });
  try {
    const short = [0, 1, 2];
    new Proxy(short, {})[3] = 'setter';
    assert.deepStrictEqual(trapped, ['setter']);
    assert.strictEqual(short.length, 3);
  } finally {
    delete Array.prototype[3];
  }

  const frozenLiteral = () => [1, 2];
  for (let i = 0; i < 2000; i++) frozenLiteral();
  const appended = frozenLiteral();
  new Proxy(appended, {})[2] = 3;
  assert.deepStrictEqual(appended, [1, 2, 3]);
  assert.deepStrictEqual(frozenLiteral(), [1, 2]);

  const receiver = Object.create(new Proxy([1, 2], {}));
  receiver[0] = 'own';
  assert.strictEqual(Object.hasOwn(receiver, 0), true);
})();

console.log('ok');
