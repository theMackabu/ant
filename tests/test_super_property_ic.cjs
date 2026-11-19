const assert = require('node:assert');

const HOT = 400;

function makeClasses() {
  class A { constructor() { this.x = 0; } }
  class B extends A {
    put(v) { super.x = v; return v; }
    get() { return super.x; }
    getName() { return super.name; }
    call() { return super.m(); }
    update() { return super.n++; }
  }
  return { A, B };
}

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

test('setter added on the parent after a cached store', () => {
  const { A, B } = makeClasses();
  const b = new B();
  warm((i) => b.put(i));
  assert.strictEqual(b.x, HOT - 1);
  assert.strictEqual(Object.hasOwn(A.prototype, 'x'), false);
  let hits = 0;
  Object.defineProperty(A.prototype, 'x', { set(v) { hits = v; }, configurable: true });
  b.put(77);
  assert.strictEqual(hits, 77);
  assert.strictEqual(b.x, HOT - 1);
});

test('setter added on Object.prototype after a cached store', () => {
  const { A, B } = makeClasses();
  const b = new B();
  warm((i) => b.put(i));
  let hits = 0;
  Object.defineProperty(Object.prototype, 'x', { set(v) { hits = v; }, configurable: true });
  try {
    b.put(5);
    assert.strictEqual(hits, 5);
    assert.strictEqual(b.x, HOT - 1);
  } finally {
    delete Object.prototype.x;
  }
  b.put(6);
  assert.strictEqual(b.x, 6);
});

test('parent data property made read-only', () => {
  const { A, B } = makeClasses();
  A.prototype.x = 'proto';
  const b = new B();
  warm((i) => b.put(i));
  assert.strictEqual(A.prototype.x, 'proto');
  assert.strictEqual(b.x, HOT - 1);

  Object.defineProperty(A.prototype, 'x', { writable: false });
  assert.throws(() => b.put(1), TypeError);
  assert.strictEqual(b.x, HOT - 1);
});

test('parent data property replaced by a setter', () => {
  const { A, B } = makeClasses();
  A.prototype.x = 1;
  const b = new B();
  warm((i) => b.put(i));
  let hits = 0;
  Object.defineProperty(A.prototype, 'x', { set(v) { hits = v; }, configurable: true });
  b.put(9);
  assert.strictEqual(hits, 9);
});

test('frozen receiver', () => {
  const { B } = makeClasses();
  const b = new B();
  warm((i) => b.put(i));
  Object.freeze(b);
  assert.throws(() => b.put(1), TypeError);
  assert.strictEqual(b.x, HOT - 1);
});

test('read-only own property', () => {
  const { B } = makeClasses();
  const b = new B();
  warm((i) => b.put(i));
  Object.defineProperty(b, 'x', { writable: false });
  assert.throws(() => b.put(1), TypeError);
  assert.strictEqual(b.x, HOT - 1);
});

test('own accessor without the parent seeing it', () => {
  const { B } = makeClasses();
  const b = new B();
  warm((i) => b.put(i));
  let seen;
  Object.defineProperty(b, 'x', { get() { return 'g'; }, set(v) { seen = v; }, configurable: true });
  assert.throws(() => b.put(3), TypeError);
  assert.strictEqual(seen, undefined);
  assert.strictEqual(b.x, 'g');
});

test('deleted own property is re-added', () => {
  const { B } = makeClasses();
  const b = new B();
  warm((i) => b.put(i));
  delete b.x;
  b.put(4);
  assert.strictEqual(b.x, 4);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(b, 'x'), { value: 4, writable: true, enumerable: true, configurable: true });
});

test('proxy receiver defines through its trap', () => {
  const { B } = makeClasses();
  const b = new B();
  warm((i) => b.put(i));
  const log = [];
  const proxy = new Proxy(b, {
    defineProperty(t, k, d) { log.push(k); return Reflect.defineProperty(t, k, d); },
    getOwnPropertyDescriptor(t, k) { return Reflect.getOwnPropertyDescriptor(t, k); },
  });
  B.prototype.put.call(proxy, 8);
  assert.deepStrictEqual(log, ['x']);
  assert.strictEqual(b.x, 8);
});

test('prototype of the home prototype replaced', () => {
  const { B } = makeClasses();
  const b = new B();
  warm((i) => b.put(i));
  let hits = 0;
  Object.setPrototypeOf(B.prototype, { set x(v) { hits = v; } });
  b.put(11);
  assert.strictEqual(hits, 11);
  assert.strictEqual(b.x, HOT - 1);
});

test('receivers with different shapes', () => {
  const { A, B } = makeClasses();
  const receivers = [new B(), Object.assign(new B(), { y: 1 }), Object.assign(new B(), { z: 1, w: 2 })];
  for (let i = 0; i < HOT; i++) {
    const r = receivers[i % receivers.length];
    B.prototype.put.call(r, i);
    assert.strictEqual(r.x, i);
  }
  const plain = { x: 1 };
  B.prototype.put.call(plain, 2);
  assert.strictEqual(plain.x, 2);
  const fresh = {};
  B.prototype.put.call(fresh, 3);
  assert.deepStrictEqual(Object.keys(fresh), ['x']);
  assert.strictEqual(Object.hasOwn(A.prototype, 'x'), false);
});

test('cached read follows data, getter, delete and late definitions', () => {
  const { A, B } = makeClasses();
  A.prototype.name = 'data';
  const b = new B();
  warm(() => assert.strictEqual(b.getName(), 'data'));
  Object.defineProperty(A.prototype, 'name', { get() { return this === b ? 'receiver' : 'wrong'; }, configurable: true });
  warm(() => assert.strictEqual(b.getName(), 'receiver'));
  delete A.prototype.name;
  warm(() => assert.strictEqual(b.getName(), undefined));
  Object.prototype.name = 'late';
  try {
    assert.strictEqual(b.getName(), 'late');
  } finally {
    delete Object.prototype.name;
  }
  assert.strictEqual(b.getName(), undefined);
  b.name = 'own';
  assert.strictEqual(b.getName(), undefined);
});

test('cached method call follows redefinition and prototype swap', () => {
  const { A, B } = makeClasses();
  A.prototype.m = function () { return this; };
  const b = new B();
  warm(() => assert.strictEqual(b.call(), b));
  A.prototype.m = function () { return 'replaced'; };
  assert.strictEqual(b.call(), 'replaced');
  Object.setPrototypeOf(B.prototype, { m() { return 'swapped'; } });
  assert.strictEqual(b.call(), 'swapped');
});

test('update expression through a parent data property', () => {
  const { A, B } = makeClasses();
  A.prototype.n = 1;
  const b = new B();
  warm(() => assert.strictEqual(b.update(), 1));
  assert.strictEqual(b.n, 2);
  assert.strictEqual(A.prototype.n, 1);
});

test('object-literal homes sharing one method body', () => {
  const homes = [];
  for (let i = 0; i < 4; i++) {
    const parent = i % 2 ? { v: 'odd' } : { set v(x) { this.viaSetter = x; } };
    const home = { __proto__: parent, v: 0, put(x) { super.v = x; return this; } };
    homes.push(home);
  }
  for (let round = 0; round < HOT; round++) {
    const home = homes[round % 4];
    home.put(round);
    if (round % 2) assert.strictEqual(home.v, round);
    else assert.strictEqual(home.viaSetter, round);
  }
});

test('one store site, plain parent then setter parent', () => {
  const makePut = (parent) => ({ __proto__: parent, put(x) { super.v = x; } }).put;
  let viaSetter;
  const plainPut = makePut({});
  const setterPut = makePut({ set v(x) { viaSetter = x; } });
  const receiver = { v: 0 };
  warm((i) => plainPut.call(receiver, i));
  assert.strictEqual(receiver.v, HOT - 1);
  setterPut.call(receiver, 'through setter');
  assert.strictEqual(viaSetter, 'through setter');
  assert.strictEqual(receiver.v, HOT - 1);
});

test('one read site, two parents', () => {
  const makeGet = (parent) => ({ __proto__: parent, get() { return super.v; } }).get;
  const first = makeGet({ v: 'first' });
  const second = makeGet({ v: 'second' });
  warm(() => assert.strictEqual(first.call({}), 'first'));
  assert.strictEqual(second.call({}), 'second');
});

test('parent setter is called for every store and follows redefinition', () => {
  const calls = [];
  class A { set v(x) { calls.push(['first', this, x]); } }
  class B extends A { put(x) { super.v = x; return x; } }
  const b = new B();
  warm((i) => b.put(i));
  assert.strictEqual(calls.length, HOT);
  assert.deepStrictEqual(calls[HOT - 1], ['first', b, HOT - 1]);
  assert.strictEqual(Object.hasOwn(b, 'v'), false);

  Object.defineProperty(A.prototype, 'v', { set(x) { calls.push(['second', this, x]); }, configurable: true });
  b.put('s');
  assert.deepStrictEqual(calls.at(-1), ['second', b, 's']);

  Object.defineProperty(A.prototype, 'v', { get() { return 1; }, set: undefined, configurable: true });
  assert.throws(() => b.put('g'), TypeError);

  delete A.prototype.v;
  b.put('own');
  assert.strictEqual(b.v, 'own');
});

test('setter two levels up is shadowed by a later nearer definition', () => {
  let seen;
  class Root { set v(x) { seen = ['root', x]; } }
  class Mid extends Root {}
  class Leaf extends Mid { put(x) { super.v = x; } }
  const leaf = new Leaf();
  warm((i) => leaf.put(i));
  assert.deepStrictEqual(seen, ['root', HOT - 1]);

  Object.defineProperty(Mid.prototype, 'v', { set(x) { seen = ['mid', x]; }, configurable: true });
  leaf.put(1);
  assert.deepStrictEqual(seen, ['mid', 1]);

  delete Mid.prototype.v;
  Object.setPrototypeOf(Mid.prototype, { set v(x) { seen = ['swapped', x]; } });
  leaf.put(2);
  assert.deepStrictEqual(seen, ['swapped', 2]);
});

test('setter that throws propagates and keeps working', () => {
  class A { set v(x) { if (x === 'bad') throw new RangeError('bad'); this.stored = x; } }
  class B extends A { put(x) { super.v = x; } }
  const b = new B();
  warm((i) => b.put(i));
  assert.throws(() => b.put('bad'), RangeError);
  b.put('good');
  assert.strictEqual(b.stored, 'good');
});

test('setter receives primitive receivers unchanged in strict code', () => {
  let receiver;
  const home = { __proto__: { set v(x) { 'use strict'; receiver = this; } }, put(x) { 'use strict'; super.v = x; } };
  warm((i) => home.put.call(home, i));
  home.put.call(7, 1);
  assert.strictEqual(receiver, 7);
});

test('many receiver shapes at one site keep per-receiver semantics', () => {
  class A { constructor() { this.x = 0; } }
  class B extends A { put(v) { super.x = v; return v; } }
  const kinds = ['plain', 'plain', 'plain', 'readonly', 'accessor', 'frozen', 'plain', 'plain', 'plain', 'plain'];
  const receivers = [];
  let accessorWrites = 0;
  for (let i = 0; i < 40; i++) {
    const kind = kinds[i % kinds.length];
    const r = new B();
    r['extra' + i] = i;
    if (kind === 'readonly') Object.defineProperty(r, 'x', { value: 'fixed', writable: false });
    if (kind === 'accessor') Object.defineProperty(r, 'x', { get() { return 'acc'; }, set() { accessorWrites++; }, configurable: true });
    if (kind === 'frozen') Object.freeze(r);
    receivers.push({ r, kind });
  }

  for (let round = 0; round < 20; round++)
    for (let i = 0; i < receivers.length; i++) {
      const { r, kind } = receivers[i];
      const value = round * 100 + i;
      if (kind === 'plain') {
        B.prototype.put.call(r, value);
        assert.strictEqual(r.x, value);
      } else {
        assert.throws(() => B.prototype.put.call(r, value), TypeError, kind);
        assert.strictEqual(r.x, kind === 'readonly' ? 'fixed' : kind === 'accessor' ? 'acc' : 0);
      }
    }
  assert.strictEqual(accessorWrites, 0);

  const fresh = {};
  B.prototype.put.call(fresh, 'added');
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(fresh, 'x'), { value: 'added', writable: true, enumerable: true, configurable: true });

  const array = [];
  B.prototype.put.call(array, 'on array');
  assert.strictEqual(array.x, 'on array');

  let viaParent;
  Object.defineProperty(A.prototype, 'x', { set(v) { viaParent = v; }, configurable: true });
  B.prototype.put.call(receivers[0].r, 'parent');
  assert.strictEqual(viaParent, 'parent');
  assert.notStrictEqual(receivers[0].r.x, 'parent');
});

console.log('ok');
