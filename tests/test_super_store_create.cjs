'use strict';
const assert = require('node:assert');

class A {}
class B extends A { put(x) { super.v = x; return this; } }
const put = B.prototype.put;
const data = (value) => ({ value, writable: true, enumerable: true, configurable: true });

for (let i = 0; i < 400; i++) {
  const fresh = new B().put(i);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(fresh, 'v'), data(i));
  assert.strictEqual(Object.hasOwn(A.prototype, 'v'), false);
  assert.strictEqual(fresh.put(i + 1).v, i + 1);
  assert.deepStrictEqual(Object.keys(fresh), ['v']);

  const withProps = new B();
  withProps.a = 1;
  withProps.b = 2;
  assert.deepStrictEqual(Object.entries(withProps.put(i)), [['a', 1], ['b', 2], ['v', i]]);

  assert.throws(() => Object.preventExtensions(new B()).put(i), TypeError);
  assert.throws(() => Object.seal(new B()).put(i), TypeError);
  assert.throws(() => Object.freeze(new B()).put(i), TypeError);
  assert.strictEqual(Object.hasOwn(Object.preventExtensions(new B()), 'v'), false);

  const accessor = Object.defineProperty(new B(), 'v', { get() { return 1; }, set() { throw new Error('own setter'); }, configurable: true });
  assert.throws(() => accessor.put(i), TypeError);
  const readonly = Object.defineProperty(new B(), 'v', { value: 7, writable: false, configurable: true });
  assert.throws(() => readonly.put(i), TypeError);
  assert.strictEqual(readonly.v, 7);
}

{
  class Base {}
  class Mixed extends Base { put(x) { super.v = x; return this; } }
  const plain = () => new Mixed();
  const tagged = () => { const o = new Mixed(); o.a = 1; return o; };
  const held = () => { const o = new Mixed(); o.q = 1; o.v = 0; return o; };
  const kept = plain().put(0);
  for (let i = 0; i < 400; i++) {
    assert.deepStrictEqual(Object.keys(plain().put(i)), ['v']);
    assert.deepStrictEqual(Object.entries(held().put(i)), [['q', 1], ['v', i]]);
    assert.deepStrictEqual(Object.entries(tagged().put(i)), [['a', 1], ['v', i]]);
    assert.deepStrictEqual(Object.entries(kept.put(i)), [['v', i]]);
    assert.deepStrictEqual(Object.entries(tagged().put(i)), [['a', 1], ['v', i]]);
    assert.deepStrictEqual(Object.keys(plain().put(i)), ['v']);
  }
}

{
  class Base {}
  class Late extends Base { put(x) { super.v = x; return this; } }
  for (let i = 0; i < 200; i++) assert.strictEqual(new Late().put(i).v, i);
  let seen = 0;
  Object.defineProperty(Base.prototype, 'v', { set(x) { seen += x; }, configurable: true });
  for (let i = 0; i < 200; i++) assert.strictEqual(Object.hasOwn(new Late().put(1), 'v'), false);
  assert.strictEqual(seen, 200);
  Object.defineProperty(Base.prototype, 'v', { value: 1, writable: false, configurable: true });
  for (let i = 0; i < 20; i++) assert.throws(() => new Late().put(1), TypeError);
  delete Base.prototype.v;
  for (let i = 0; i < 200; i++) assert.deepStrictEqual(Object.getOwnPropertyDescriptor(new Late().put(i), 'v'), data(i));
  Object.defineProperty(Object.prototype, 'v', { set(x) { seen -= x; }, configurable: true });
  for (let i = 0; i < 50; i++) assert.strictEqual(Object.hasOwn(new Late().put(1), 'v'), false);
  assert.strictEqual(seen, 150);
  delete Object.prototype.v;
  Object.setPrototypeOf(Late.prototype, { set v(x) { seen = -x; } });
  assert.strictEqual(Object.hasOwn(new Late().put(5), 'v'), false);
  assert.strictEqual(seen, -5);
  Object.setPrototypeOf(Late.prototype, null);
  assert.throws(() => new Late().put(1), TypeError);
}

for (let i = 0; i < 200; i++) {
  const list = [];
  assert.strictEqual(put.call(list, i).v, i);
  assert.strictEqual(list.length, 0);
  const fn = function () {};
  assert.strictEqual(put.call(fn, i).v, i);
  const bare = Object.create(null);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(put.call(bare, i), 'v'), data(i));
  const log = [];
  const proxy = new Proxy({}, { defineProperty(t, k, d) { log.push(k); return Reflect.defineProperty(t, k, d); } });
  put.call(proxy, i);
  assert.deepStrictEqual(log, ['v']);
  assert.throws(() => put.call(1, i), TypeError);
}

{
  const kept = [];
  for (let i = 0; i < 60000; i++) {
    const o = new B();
    if (i & 1) o['k' + (i % 7)] = i;
    o.put({ i });
    if (i % 997 === 0) kept.push(o);
  }
  for (const o of kept) assert.strictEqual(typeof o.v.i, 'number');
}

{
  class Base {}
  class Fresh extends Base { put(x) { super.v = x; return this; } }
  for (let i = 0; i < 400; i++) {
    assert.deepStrictEqual(Object.getOwnPropertyDescriptor(new Fresh().put(i), 'v'), data(i));
    assert.throws(() => Object.preventExtensions(new Fresh()).put(i), TypeError);
    assert.throws(() => Object.seal(new Fresh()).put(i), TypeError);
    assert.throws(() => Object.freeze(new Fresh()).put(i), TypeError);
    const closed = Object.preventExtensions(new Fresh());
    try { closed.put(i); } catch {}
    assert.strictEqual(Object.hasOwn(closed, 'v'), false);
  }
}

function reshaping(change) {
  class Base {}
  Object.defineProperty(Base.prototype, 'v', { configurable: true, set(x) {
    Object.defineProperty(Base.prototype, 'v', { value: 0, writable: true, enumerable: true, configurable: true });
    change(this);
    Object.defineProperty(this, 'v', { value: x, writable: true, enumerable: true, configurable: true });
  } });
  class Derived extends Base {
    constructor() { super(); this.a = 'A'; this.n = 'N'; }
    put(x) { super.v = x; return this; }
  }
  return Derived;
}

for (const [name, change, first] of [
  ['reconfigures', (o) => Object.defineProperty(o, 'n', { enumerable: false, writable: false }), '{"a":"A","v":0}'],
  ['deletes', (o) => { delete o.a; }, '{"n":"N","v":0}'],
  ['adds', (o) => { o.extra = 1; }, '{"a":"A","n":"N","extra":1,"v":0}'],
  ['replaces', (o) => { delete o.n; o.m = 'M'; }, '{"a":"A","m":"M","v":0}'],
  ['nothing', () => {}, '{"a":"A","n":"N","v":0}'],
]) {
  const Derived = reshaping(change);
  assert.strictEqual(JSON.stringify(new Derived().put(0)), first, name);
  for (let i = 1; i < (name === 'reconfigures' ? 6000 : 300); i++) {
    const o = new Derived().put(i);
    assert.strictEqual(JSON.stringify(o), `{"a":"A","n":"N","v":${i}}`, name);
    assert.deepStrictEqual(Object.getOwnPropertyDescriptor(o, 'n'), data('N'), name);
    assert.deepStrictEqual(Object.getOwnPropertyDescriptor(o, 'v'), data(i), name);
  }
}

{
  const home = { __proto__: { set v(x) {
    Object.defineProperty(Object.getPrototypeOf(home), 'v', { value: 0, writable: true, configurable: true });
    Object.defineProperty(this, 'k', { enumerable: false });
    Object.defineProperty(this, 'v', { value: x, writable: true, enumerable: true, configurable: true });
  } }, put(x) { super.v = x; return this; } };
  assert.deepStrictEqual(Object.keys(home.put.call({ k: 1 }, 0)), ['v']);
  for (let i = 0; i < 300; i++) assert.deepStrictEqual(Object.entries(home.put.call({ k: 1 }, i)), [['k', 1], ['v', i]]);
}

{
  class Base { static set v(x) {
    Object.defineProperty(Base, 'v', { value: 0, writable: true, configurable: true });
    Object.defineProperty(this, 'k', { writable: false });
    Object.defineProperty(this, 'v', { value: x, writable: true, enumerable: true, configurable: true });
  } }
  class Derived extends Base { static put(x) { super.v = x; return this; } }
  assert.strictEqual(Object.getOwnPropertyDescriptor(Derived.put.call({ k: 1 }, 0), 'k').writable, false);
  for (let i = 0; i < 300; i++) assert.deepStrictEqual(Object.getOwnPropertyDescriptor(Derived.put.call({ k: 1 }, i), 'k'), data(1));
}

{
  class Base { get v() { return 1; } }
  class GetterOnly extends Base { put(x) { super.v = x; return this; } }
  for (let i = 0; i < 300; i++) {
    const o = new GetterOnly();
    assert.throws(() => o.put(i), TypeError);
    assert.strictEqual(Object.hasOwn(o, 'v'), false);
  }
}

{
  class Base {}
  class Attrs extends Base { constructor() { super(); this.k = 1; } put(x) { super.v = x; return this; } }
  for (let i = 0; i < 300; i++) assert.deepStrictEqual(Object.entries(new Attrs().put(i)), [['k', 1], ['v', i]]);
  for (let i = 0; i < 50; i++) {
    const hidden = Object.defineProperty(new Attrs(), 'k', { enumerable: false });
    assert.deepStrictEqual(Object.entries(hidden.put(i)), [['v', i]]);
    assert.deepStrictEqual(Object.getOwnPropertyDescriptor(hidden, 'k'), { value: 1, writable: true, enumerable: false, configurable: true });
    const locked = Object.defineProperty(new Attrs(), 'k', { writable: false });
    assert.strictEqual(Object.getOwnPropertyDescriptor(locked.put(i), 'k').writable, false);
    assert.deepStrictEqual(Object.entries(new Attrs().put(i)), [['k', 1], ['v', i]]);
    const trapped = [];
    const proxy = new Proxy(new Attrs(), { defineProperty(t, key, d) { trapped.push(key); return Reflect.defineProperty(t, key, d); } });
    assert.strictEqual(Attrs.prototype.put.call(proxy, i).v, i);
    assert.deepStrictEqual(trapped, ['v']);
    assert.throws(() => Object.freeze(new Attrs()).put(i), TypeError);
  }
}

{
  class Base {}
  Object.defineProperty(Base.prototype, 'v', { configurable: true, set(x) {
    Object.defineProperty(Base.prototype, 'v', { value: 0, writable: true, enumerable: true, configurable: true });
    Object.defineProperty(this, 'v', { value: x, writable: true, enumerable: false, configurable: true });
  } });
  class Hidden extends Base { put(x) { super.v = x; return this; } }
  assert.strictEqual(Object.getOwnPropertyDescriptor(new Hidden().put(0), 'v').enumerable, false);
  for (let i = 1; i < 300; i++) assert.deepStrictEqual(Object.getOwnPropertyDescriptor(new Hidden().put(i), 'v'), data(i));
}

console.log('ok');
