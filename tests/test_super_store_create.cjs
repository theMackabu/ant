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

console.log('ok');
