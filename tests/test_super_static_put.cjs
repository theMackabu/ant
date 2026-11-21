const assert = require('node:assert');

const HOT = 400;

function warm(fn) {
  for (let i = 0; i < HOT; i++) fn(i);
}

function test(name, body) {
  try {
    body();
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

function makeClasses() {
  class A {}
  class B extends A {
    static x = 0;
    static put(v) { super.x = v; return v; }
    static mark(v) { super.fresh = v; }
    static value(v) { return super.x = v; }
  }
  return { A, B };
}

test('store lands on the class that received the call', () => {
  const { A, B } = makeClasses();
  warm((i) => B.put(i));
  assert.strictEqual(B.x, HOT - 1);
  assert.strictEqual(Object.hasOwn(A, 'x'), false);
  assert.strictEqual(B.value(7), 7);
  assert.strictEqual(B.x, 7);
});

test('subclass receiver gets its own property', () => {
  const { A, B } = makeClasses();
  class C extends B {}
  warm((i) => B.put(i));
  C.put('c');
  assert.strictEqual(Object.hasOwn(C, 'x'), true);
  assert.strictEqual(C.x, 'c');
  assert.strictEqual(B.x, HOT - 1);
  warm((i) => C.put(i));
  assert.strictEqual(C.x, HOT - 1);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(C, 'x'), { value: HOT - 1, writable: true, enumerable: true, configurable: true });
  assert.strictEqual(Object.hasOwn(A, 'x'), false);
});

test('alternating class receivers', () => {
  const { B } = makeClasses();
  const subs = [];
  for (let i = 0; i < 12; i++) subs.push(class extends B { static x = -1; static [`k${i}`] = i; });
  for (let round = 0; round < 60; round++)
    for (let i = 0; i < subs.length; i++) subs[i].put(round * 100 + i);
  for (let i = 0; i < subs.length; i++) assert.strictEqual(subs[i].x, 5900 + i);
  assert.strictEqual(B.x, 0);
});

test('static setter on the parent', () => {
  const seen = [];
  class A { static set x(v) { seen.push([this, v]); } }
  class B extends A { static put(v) { super.x = v; } }
  warm((i) => B.put(i));
  assert.strictEqual(seen.length, HOT);
  assert.strictEqual(seen[HOT - 1][0], B);
  assert.strictEqual(seen[HOT - 1][1], HOT - 1);
  assert.strictEqual(Object.hasOwn(B, 'x'), false);
  class C extends B {}
  C.put('c');
  assert.strictEqual(seen[HOT][0], C);
});

test('static setter two levels up', () => {
  let last;
  class Root { static set x(v) { last = [this, v]; } }
  class A extends Root {}
  class B extends A { static put(v) { super.x = v; } }
  warm((i) => B.put(i));
  assert.deepStrictEqual(last, [B, HOT - 1]);
  Object.defineProperty(A, 'x', { value: 1, writable: true, configurable: true });
  B.put('own');
  assert.deepStrictEqual(last, [B, HOT - 1]);
  assert.strictEqual(B.x, 'own');
  assert.strictEqual(A.x, 1);
});

test('setter added to the parent after warm-up', () => {
  const { A, B } = makeClasses();
  warm((i) => B.put(i));
  let hits = 0;
  Object.defineProperty(A, 'x', { set(v) { hits = v; }, configurable: true });
  B.put(77);
  assert.strictEqual(hits, 77);
  assert.strictEqual(B.x, HOT - 1);
  delete A.x;
  B.put(78);
  assert.strictEqual(B.x, 78);
});

test('setter added to Function.prototype after warm-up', () => {
  const { B } = makeClasses();
  warm((i) => B.put(i));
  let hits = 0;
  Object.defineProperty(Function.prototype, 'x', { set(v) { hits = v; }, configurable: true });
  try {
    B.put(5);
    assert.strictEqual(hits, 5);
    assert.strictEqual(B.x, HOT - 1);
  } finally {
    delete Function.prototype.x;
  }
  B.put(6);
  assert.strictEqual(B.x, 6);
});

test('parent data property made read-only', () => {
  const { A, B } = makeClasses();
  A.x = 'parent';
  warm((i) => B.put(i));
  assert.strictEqual(A.x, 'parent');
  Object.defineProperty(A, 'x', { writable: false });
  assert.throws(() => B.put(1), TypeError);
  assert.strictEqual(B.x, HOT - 1);
});

test('receiver property made read-only, then an accessor', () => {
  const { B } = makeClasses();
  warm((i) => B.put(i));
  Object.defineProperty(B, 'x', { writable: false });
  assert.throws(() => B.put(1), TypeError);
  assert.strictEqual(B.x, HOT - 1);
  let hits = 0;
  Object.defineProperty(B, 'x', { get() { return 'g'; }, set(v) { hits++; }, configurable: true });
  assert.throws(() => B.put(2), TypeError);
  assert.strictEqual(hits, 0);
  assert.strictEqual(B.x, 'g');
});

test('frozen class receiver', () => {
  const { B } = makeClasses();
  warm((i) => B.put(i));
  Object.freeze(B);
  assert.throws(() => B.put(1), TypeError);
  assert.strictEqual(B.x, HOT - 1);
});

test('parent swapped after warm-up', () => {
  const { B } = makeClasses();
  warm((i) => B.put(i));
  let seen;
  class Other { static set x(v) { seen = [this, v]; } }
  Object.setPrototypeOf(B, Other);
  B.put('moved');
  assert.deepStrictEqual(seen, [B, 'moved']);
  assert.strictEqual(B.x, HOT - 1);
  Object.setPrototypeOf(B, null);
  assert.throws(() => B.put(1), TypeError);
  Object.setPrototypeOf(B, Function.prototype);
  B.put('back');
  assert.strictEqual(B.x, 'back');
});

test('non-writable function keys', () => {
  class A {}
  class B extends A {
    static setName(v) { super.name = v; }
    static setLength(v) { super.length = v; }
    static setPrototype(v) { super.prototype = v; }
  }
  for (let i = 0; i < HOT; i++) {
    assert.throws(() => B.setName('n'), TypeError);
    assert.throws(() => B.setLength(3), TypeError);
    assert.throws(() => B.setPrototype({}), TypeError);
  }
  assert.strictEqual(B.name, 'B');
  assert.strictEqual(B.length, 0);
  assert.strictEqual(typeof B.prototype.constructor, 'function');
});

test('prototype of a plain function receiver', () => {
  class B { static setPrototype(v) { super.prototype = v; } }
  function F() {}
  const first = { tag: 'first' };
  const second = { tag: 'second' };
  for (let i = 0; i < HOT; i++) {
    B.setPrototype.call(F, first);
    assert.strictEqual(Object.getPrototypeOf(new F()), first);
  }
  B.setPrototype.call(F, second);
  assert.strictEqual(Object.getPrototypeOf(new F()), second);
  assert.strictEqual(new F().tag, 'second');
  assert.throws(() => B.setPrototype({}), TypeError);
});

test('receivers that are not classes', () => {
  const { B } = makeClasses();
  warm((i) => B.put(i));
  const plain = { x: 1 };
  B.put.call(plain, 'plain');
  assert.strictEqual(plain.x, 'plain');
  const bare = Object.create(null);
  B.put.call(bare, 'bare');
  assert.strictEqual(bare.x, 'bare');
  const list = [];
  B.put.call(list, 'list');
  assert.strictEqual(list.x, 'list');
  const fn = function () {};
  B.put.call(fn, 'fn');
  assert.strictEqual(fn.x, 'fn');
  const arrow = () => {};
  B.put.call(arrow, 'arrow');
  assert.strictEqual(arrow.x, 'arrow');
  const bound = fn.bind(null);
  B.put.call(bound, 'bound');
  assert.strictEqual(bound.x, 'bound');
  assert.strictEqual(fn.x, 'fn');
  assert.throws(() => B.put.call(1, 'n'), TypeError);
  assert.throws(() => B.put.call('s', 'n'), TypeError);
  assert.throws(() => B.put.call(undefined, 'n'), TypeError);
  assert.strictEqual(B.x, HOT - 1);
});

test('proxy receiver sees defineProperty', () => {
  const { B } = makeClasses();
  warm((i) => B.put(i));
  const log = [];
  const target = function () {};
  const proxy = new Proxy(target, {
    getOwnPropertyDescriptor(t, k) { log.push(`gopd:${String(k)}`); return Reflect.getOwnPropertyDescriptor(t, k); },
    defineProperty(t, k, d) { log.push(`define:${String(k)}`); return Reflect.defineProperty(t, k, d); },
  });
  B.put.call(proxy, 'p');
  assert.deepStrictEqual(log, ['gopd:x', 'define:x']);
  assert.strictEqual(target.x, 'p');
});

test('proxy parent sees set', () => {
  const log = [];
  const parent = new Proxy(function () {}, {
    set(t, k, v, r) { log.push([k, v, r]); return true; },
  });
  class B { static put(v) { super.x = v; } }
  Object.setPrototypeOf(B, parent);
  warm((i) => B.put(i));
  assert.strictEqual(log.length, HOT);
  assert.strictEqual(log[HOT - 1][0], 'x');
  assert.strictEqual(log[HOT - 1][1], HOT - 1);
  assert.strictEqual(log[HOT - 1][2], B);
  assert.strictEqual(Object.hasOwn(B, 'x'), false);
});

test('bound function and builtin parents', () => {
  function target() {}
  const bound = target.bind(null);
  class B { static x = 0; static put(v) { super.x = v; } }
  Object.setPrototypeOf(B, bound);
  warm((i) => B.put(i));
  assert.strictEqual(B.x, HOT - 1);
  assert.strictEqual(Object.hasOwn(bound, 'x'), false);
  assert.strictEqual(Object.hasOwn(target, 'x'), false);
  let hits = 0;
  Object.defineProperty(bound, 'x', { set(v) { hits = v; }, configurable: true });
  B.put('s');
  assert.strictEqual(hits, 's');

  class L extends Array { static put(v) { super.tag = v; } }
  warm((i) => L.put(i));
  assert.strictEqual(L.tag, HOT - 1);
  assert.strictEqual(Object.hasOwn(Array, 'tag'), false);
  class M extends Map { static put(v) { super.tag = v; } }
  warm((i) => M.put(i));
  assert.strictEqual(M.tag, HOT - 1);
  assert.strictEqual(Object.hasOwn(Map, 'tag'), false);
});

test('adding a static property to fresh classes', () => {
  const { A, B } = makeClasses();
  for (let i = 0; i < HOT; i++) {
    class C extends B {}
    C.mark(i);
    assert.strictEqual(Object.hasOwn(C, 'fresh'), true);
    assert.strictEqual(C.fresh, i);
    assert.deepStrictEqual(Object.getOwnPropertyDescriptor(C, 'fresh'), { value: i, writable: true, enumerable: true, configurable: true });
  }
  assert.strictEqual(Object.hasOwn(B, 'fresh'), false);
  B.mark('b');
  assert.strictEqual(B.fresh, 'b');
  assert.strictEqual(B.x, 0);
  let hits = 0;
  Object.defineProperty(A, 'fresh', { set(v) { hits++; }, configurable: true });
  class D extends B {}
  D.mark('d');
  assert.strictEqual(hits, 1);
  assert.strictEqual(Object.hasOwn(D, 'fresh'), false);
  delete A.fresh;
  const closed = Object.preventExtensions(class extends B {});
  assert.throws(() => closed.mark(1), TypeError);
  assert.strictEqual(Object.hasOwn(closed, 'fresh'), false);
});

test('static next store reaches the iterator protector', () => {
  class B { static setNext(v) { super.next = v; } }
  const proto = Object.getPrototypeOf([][Symbol.iterator]());
  const original = proto.next;
  try {
    for (let i = 0; i < HOT; i++) B.setNext.call(proto, original);
    let total = 0;
    for (const v of [1, 2, 3]) total += v;
    assert.strictEqual(total, 6);
    let calls = 0;
    B.setNext.call(proto, function () { calls++; return original.call(this); });
    total = 0;
    for (const v of [1, 2, 3]) total += v;
    assert.strictEqual(total, 6);
    assert.strictEqual(calls, 4);
  } finally {
    proto.next = original;
  }
});

test('one site, parents that differ', () => {
  const seen = [];
  function make(parent) {
    return class extends parent { static x = 0; static put(v) { super.x = v; } };
  }
  class Plain {}
  class Trapped { static set x(v) { seen.push(v); } }
  class Fixed {}
  Object.defineProperty(Fixed, 'x', { value: 'ro', writable: false });
  const plain = make(Plain);
  const trapped = make(Trapped);
  const fixed = make(Fixed);
  for (let i = 0; i < HOT; i++) plain.put(i);
  trapped.put('t');
  assert.deepStrictEqual(seen, ['t']);
  assert.strictEqual(trapped.x, 0);
  assert.throws(() => fixed.put(1), TypeError);
  assert.strictEqual(fixed.x, 0);
  for (let i = 0; i < HOT; i++) {
    plain.put(i);
    trapped.put(i);
    assert.throws(() => fixed.put(i), TypeError);
  }
  assert.strictEqual(plain.x, HOT - 1);
  assert.strictEqual(trapped.x, 0);
  assert.strictEqual(fixed.x, 0);
  assert.strictEqual(seen.length, HOT + 1);
});

test('one site, parents that differ, unrelated receivers', () => {
  const seen = [];
  function make(parent) {
    return class extends parent { static put(v) { super.x = v; } };
  }
  class Plain {}
  class Trapped { static set x(v) { seen.push(v); } }
  class Fixed {}
  Object.defineProperty(Fixed, 'x', { value: 'ro', writable: false });
  const plain = make(Plain);
  const trapped = make(Trapped);
  const fixed = make(Fixed);
  const shared = { x: 0 };
  for (let i = 0; i < HOT; i++) plain.put.call(shared, i);
  trapped.put.call(shared, 'shared');
  assert.strictEqual(seen[seen.length - 1], 'shared');
  assert.strictEqual(shared.x, HOT - 1);
  assert.throws(() => fixed.put.call(shared, 1), TypeError);
  assert.strictEqual(shared.x, HOT - 1);
  const fn = function () {};
  fn.x = 0;
  for (let i = 0; i < HOT; i++) plain.put.call(fn, i);
  trapped.put.call(fn, 'fn');
  assert.strictEqual(seen[seen.length - 1], 'fn');
  assert.strictEqual(fn.x, HOT - 1);
});

console.log('ok');
