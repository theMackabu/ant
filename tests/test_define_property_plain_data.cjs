const assert = require('node:assert');

const full = value => ({ value, writable: true, enumerable: true, configurable: true });
const describe = (object, key) => Object.getOwnPropertyDescriptor(object, key);

{
  const object = {};
  for (let i = 0; i < 3000; i++) {
    assert.strictEqual(Object.defineProperty(object, 'a', full(i)), object);
    assert.strictEqual(object.a, i);
  }
  assert.deepStrictEqual(describe(object, 'a'), full(2999));
  assert.deepStrictEqual(Object.keys(object), ['a']);

  const shared = () => ({ a: 1 });
  const first = shared();
  const second = shared();
  Object.defineProperty(first, 'a', full(5));
  Object.defineProperty(first, 'b', full(6));
  assert.deepStrictEqual(first, { a: 5, b: 6 });
  assert.deepStrictEqual(second, { a: 1 });
}

{
  const frozen = Object.freeze({ a: 1 });
  assert.throws(() => Object.defineProperty(frozen, 'a', full(2)), TypeError);
  assert.throws(() => Object.defineProperty(frozen, 'b', full(2)), TypeError);
  assert.strictEqual(Reflect.defineProperty(frozen, 'a', full(2)), false);
  assert.strictEqual(Reflect.defineProperty(frozen, 'a', full(1)), false);
  assert.strictEqual(frozen.a, 1);

  const sealed = Object.seal({ a: 1 });
  assert.throws(() => Object.defineProperty(sealed, 'a', full(2)), TypeError);
  assert.throws(() => Object.defineProperty(sealed, 'b', full(2)), TypeError);
  assert.strictEqual(sealed.a, 1);
  Object.defineProperty(sealed, 'a', { value: 3 });
  assert.strictEqual(sealed.a, 3);

  const closed = Object.preventExtensions({ a: 1 });
  assert.throws(() => Object.defineProperty(closed, 'b', full(2)), TypeError);
  assert.strictEqual(Reflect.defineProperty(closed, 'b', full(2)), false);
  Object.defineProperty(closed, 'a', full(4));
  assert.strictEqual(closed.a, 4);
  assert.strictEqual('b' in closed, false);
}

{
  const object = {};
  Object.defineProperty(object, 'hidden', { value: 1, writable: true, enumerable: false, configurable: true });
  assert.deepStrictEqual(Object.keys(object), []);
  assert.strictEqual(describe(object, 'hidden').enumerable, false);
  Object.defineProperty(object, 'hidden', full(2));
  assert.deepStrictEqual(describe(object, 'hidden'), full(2));
  assert.deepStrictEqual(Object.keys(object), ['hidden']);

  Object.defineProperty(object, 'locked', { value: 1, writable: false, enumerable: true, configurable: true });
  Object.defineProperty(object, 'locked', full(2));
  object.locked = 3;
  assert.strictEqual(object.locked, 3);

  Object.defineProperty(object, 'fixed', { value: 1, writable: true, enumerable: true, configurable: false });
  assert.throws(() => Object.defineProperty(object, 'fixed', full(2)), TypeError);
  assert.strictEqual(object.fixed, 1);

  let calls = 0;
  Object.defineProperty(object, 'computed', { get: () => 7, set: () => calls++, enumerable: true, configurable: true });
  Object.defineProperty(object, 'computed', full(8));
  assert.deepStrictEqual(describe(object, 'computed'), full(8));
  object.computed = 9;
  assert.strictEqual(calls, 0);
  assert.strictEqual(object.computed, 9);

  Object.defineProperty(object, 'pinned', { get: () => 7, configurable: false });
  assert.throws(() => Object.defineProperty(object, 'pinned', full(8)), TypeError);
}

{
  const object = {};
  Object.defineProperty(object, 'loose', { value: 1, writable: 1, enumerable: 'yes', configurable: {} });
  assert.deepStrictEqual(describe(object, 'loose'), full(1));
  Object.defineProperty(object, 'falsy', { value: 1, writable: 0, enumerable: '', configurable: null });
  assert.deepStrictEqual(describe(object, 'falsy'), { value: 1, writable: false, enumerable: false, configurable: false });
  Object.defineProperty(object, 'readonly', { value: 1, writable: false, enumerable: true, configurable: true });
  object.readonly = 2;
  assert.deepStrictEqual(describe(object, 'readonly'), { value: 1, writable: false, enumerable: true, configurable: true });
  object.kept = 5;
  Object.defineProperty(object, 'kept', { writable: true, enumerable: true, configurable: true });
  assert.deepStrictEqual(describe(object, 'kept'), full(5));
  Object.defineProperty(object, 'valueless', { writable: true, enumerable: true, configurable: true });
  assert.deepStrictEqual(describe(object, 'valueless'), full(undefined));
  Object.defineProperty(object, 'partial', { value: 1, writable: true, enumerable: true });
  assert.strictEqual(describe(object, 'partial').configurable, false);
  assert.throws(() => Object.defineProperty(object, 'mixed', { value: 1, writable: true, enumerable: true, configurable: true, get() {} }), TypeError);
  assert.throws(() => Object.defineProperty(object, 'mixed', { value: 1, writable: true, enumerable: true, configurable: true, set: undefined }), TypeError);
  assert.strictEqual('mixed' in object, false);

  const inherited = Object.create({ writable: true, enumerable: true, configurable: true });
  inherited.value = 5;
  Object.defineProperty(object, 'inherited', inherited);
  assert.deepStrictEqual(describe(object, 'inherited'), full(5));

  const order = [];
  const observed = {
    get enumerable() { order.push('enumerable'); return true; },
    get configurable() { order.push('configurable'); return true; },
    get value() { order.push('value'); return 1; },
    get writable() { order.push('writable'); return true; },
  };
  Object.defineProperty(object, 'observed', observed);
  assert.deepStrictEqual(order, ['enumerable', 'configurable', 'value', 'writable']);
  assert.deepStrictEqual(describe(object, 'observed'), full(1));
}

{
  const wrapper = new String('ab');
  assert.throws(() => Object.defineProperty(wrapper, '0', full('z')), TypeError);
  assert.throws(() => Object.defineProperty(wrapper, 'length', full(9)), TypeError);
  Object.defineProperty(wrapper, 'extra', full(1));
  assert.strictEqual(wrapper.extra, 1);
  assert.strictEqual(wrapper[0], 'a');

  const bytes = new Uint8Array(2);
  Object.defineProperty(bytes, '0', full(7));
  Object.defineProperty(bytes, 'label', full('named'));
  assert.strictEqual(bytes[0], 7);
  assert.strictEqual(bytes.label, 'named');
  assert.deepStrictEqual(Object.keys(bytes), ['0', '1', 'label']);
  assert.throws(() => Object.defineProperty(bytes, '5', full(1)), TypeError);

  const buffer = Buffer.alloc(2);
  Object.defineProperty(buffer, '1', full(9));
  assert.strictEqual(buffer[1], 9);
  assert.strictEqual(buffer.length, 2);

  const list = [1, 2, 3];
  Object.defineProperty(list, 'length', { value: 1, writable: true, enumerable: false, configurable: false });
  assert.throws(() => Object.defineProperty(list, 'length', full(2)), TypeError);
  Object.defineProperty(list, '4', full(9));
  assert.strictEqual(list.length, 5);
  assert.strictEqual(3 in list, false);

  const indexed = {};
  Object.defineProperty(indexed, '1', full('one'));
  Object.defineProperty(indexed, 'b', full('bee'));
  Object.defineProperty(indexed, '0', full('zero'));
  assert.deepStrictEqual(Object.keys(indexed), ['0', '1', 'b']);

  function mapped(a) {
    Object.defineProperty(arguments, '0', full(9));
    return a;
  }
  assert.strictEqual(mapped(1), 9);

  function callable() {}
  Object.defineProperty(callable, 'name', full('renamed'));
  assert.strictEqual(callable.name, 'renamed');
  assert.throws(() => Object.defineProperty(callable, 'prototype', full(1)), TypeError);

  const symbol = Symbol('key');
  const keyed = {};
  Object.defineProperty(keyed, symbol, full(1));
  assert.deepStrictEqual(describe(keyed, symbol), full(1));
}

{
  const prototype = {};
  let setterCalls = 0;
  Object.defineProperty(prototype, 'guarded', { set() { setterCalls++; }, configurable: true });
  Object.defineProperty(prototype, 'constant', { value: 1, writable: false });
  const child = Object.create(prototype);
  Object.defineProperty(child, 'guarded', full(1));
  Object.defineProperty(child, 'constant', full(2));
  assert.strictEqual(setterCalls, 0);
  assert.strictEqual(child.guarded, 1);
  assert.strictEqual(child.constant, 2);
  assert.strictEqual(Object.hasOwn(child, 'constant'), true);
}

{
  const target = {};
  const proxy = new Proxy(target, {});
  for (let i = 0; i < 3000; i++) {
    Object.defineProperty(proxy, 'a', full(i));
    assert.strictEqual(Reflect.defineProperty(proxy, 'b', full(i)), true);
  }
  assert.deepStrictEqual(target, { a: 2999, b: 2999 });
  Object.freeze(target);
  assert.throws(() => Object.defineProperty(proxy, 'a', full(1)), TypeError);
  assert.strictEqual(Reflect.defineProperty(proxy, 'a', full(1)), false);

  const read = [];
  const reader = holder => {
    for (let i = 0; i < 3000; i++) read.push(holder.a);
  };
  const holder = { a: 0 };
  reader(holder);
  Object.defineProperty(holder, 'a', full(1));
  reader(holder);
  assert.strictEqual(read[2999], 0);
  assert.strictEqual(read[5999], 1);
}

console.log('ok');
