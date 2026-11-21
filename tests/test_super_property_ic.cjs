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

const RUNS = 8000;
const FLIP = 5000;

function hotLoop(step, flip, expected) {
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) flip();
    assert.strictEqual(step(i), expected(i, i >= FLIP));
  }
}

test('hot super method call follows a reassigned base method', () => {
  class A { k(i) { return i + 1; } }
  class B extends A { m(i) { return super.k(i); } }
  const b = new B();
  hotLoop((i) => b.m(i), () => { A.prototype.k = function (i) { return i - 1; }; }, (i, after) => after ? i - 1 : i + 1);
});

test('hot super data read follows a reassigned base value', () => {
  class A {}
  A.prototype.x = 7;
  class B extends A { m() { return super.x; } }
  const b = new B();
  hotLoop(() => b.m(), () => { A.prototype.x = 'changed'; }, (i, after) => after ? 'changed' : 7);
});

test('hot super read sees a property added on an intermediate prototype', () => {
  class A { k() { return 'A'; } }
  A.prototype.x = 'ax';
  class B extends A {}
  class C extends B { m() { return super.k() + super.x; } }
  const c = new C();
  hotLoop(() => c.m(), () => { B.prototype.k = function () { return 'B'; }; B.prototype.x = 'bx'; }, (i, after) => after ? 'Bbx' : 'Aax');
});

test('hot super read sees a deleted base property', () => {
  class A { k() { return 'A'; } }
  class B extends A { k() { return 'B'; } }
  B.prototype.x = 1;
  class C extends B { m() { return [super.k(), super.x].join(); } }
  const c = new C();
  hotLoop(() => c.m(), () => { delete B.prototype.k; delete B.prototype.x; }, (i, after) => after ? 'A,' : 'B,1');
});

test('hot super call follows Object.setPrototypeOf on the home object', () => {
  class A { k(i) { return i + 1; } }
  class B extends A { m(i) { return super.k(i); } }
  const other = { k(i) { return -i; } };
  const b = new B();
  hotLoop((i) => b.m(i), () => Object.setPrototypeOf(B.prototype, other), (i, after) => after ? -i : i + 1);
});

test('hot super call follows a prototype swap deeper in the chain', () => {
  class A { k(i) { return i + 1; } }
  class B extends A {}
  class C extends B { m(i) { return super.k(i); } }
  const other = { k(i) { return i * 2; } };
  const c = new C();
  hotLoop((i) => c.m(i), () => Object.setPrototypeOf(B.prototype, other), (i, after) => after ? i * 2 : i + 1);
});

test('hot super getter runs with the real receiver and follows redefinition', () => {
  class A { get tag() { return this.own + 1; } }
  class B extends A { constructor(own) { super(); this.own = own; } m() { return super.tag; } }
  const first = new B(10), second = new B(20);
  hotLoop((i) => (i & 1 ? first : second).m(), () => Object.defineProperty(A.prototype, 'tag', { get() { return this.own - 1; }, configurable: true }),
    (i, after) => (i & 1 ? 10 : 20) + (after ? -1 : 1));
});

test('hot super data read follows a change to an accessor and back', () => {
  class A {}
  A.prototype.x = 1;
  class B extends A { constructor() { super(); this.seen = 0; } m() { return super.x; } }
  const b = new B();
  hotLoop(() => b.m(), () => Object.defineProperty(A.prototype, 'x', { get() { this.seen++; return 2; }, configurable: true }), (i, after) => after ? 2 : 1);
  assert.strictEqual(b.seen, RUNS - FLIP);
  Object.defineProperty(A.prototype, 'x', { value: 3, configurable: true });
  assert.strictEqual(b.m(), 3);
});

test('hot super getter that throws propagates once per call', () => {
  let calls = 0;
  class A { get boom() { calls++; if (this.fail) throw new RangeError('boom'); return 1; } }
  class B extends A { m() { return super.boom; } }
  const b = new B();
  for (let i = 0; i < RUNS; i++) {
    b.fail = i >= FLIP;
    if (b.fail) assert.throws(() => b.m(), RangeError);
    else assert.strictEqual(b.m(), 1);
  }
  assert.strictEqual(calls, RUNS);
});

test('hot super read through a Proxy base', () => {
  const seen = [];
  const target = { k() { return 'target'; } };
  class A { k() { return 'A'; } }
  class B extends A { m() { return super.k(); } }
  const b = new B();
  const proxy = new Proxy(target, { get(t, key, receiver) { seen.push(receiver === b); return () => 'trapped'; } });
  hotLoop(() => b.m(), () => Object.setPrototypeOf(B.prototype, proxy), (i, after) => after ? 'trapped' : 'A');
  assert.strictEqual(seen.length, RUNS - FLIP);
  assert.ok(seen.every(Boolean));
});

test('hot super read with a Proxy base from the start', () => {
  let gets = 0;
  const home = { __proto__: new Proxy({}, { get(t, key) { gets++; return key; } }), m() { return super.name; } };
  for (let i = 0; i < RUNS; i++) assert.strictEqual(home.m(), 'name');
  assert.strictEqual(gets, RUNS);
});

test('hot super read throws once the home prototype is null', () => {
  class A { k() { return 1; } }
  class B extends A { m() { return super.k(); } }
  const b = new B();
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.setPrototypeOf(B.prototype, null);
    if (i >= FLIP) assert.throws(() => b.m(), TypeError);
    else assert.strictEqual(b.m(), 1);
  }
  const bare = { __proto__: null, m() { return super.x; } };
  for (let i = 0; i < RUNS; i++) assert.throws(() => bare.m(), TypeError);
});

test('hot super read of an absent property stays undefined until it appears', () => {
  class A {}
  class B extends A { m() { return super.later; } }
  const b = new B();
  b.later = 'own';
  hotLoop(() => b.m(), () => { A.prototype.later = 'base'; }, (i, after) => after ? 'base' : undefined);
});

test('hot super call shared by several subclasses', () => {
  class A { k() { return this.id * 2; } }
  class B extends A { constructor(id) { super(); this.id = id; } m() { return super.k(); } }
  class C extends B { k() { return 'C'; } }
  class D extends B { constructor(id) { super(id); this.extra = 1; } k() { return 'D'; } }
  class E extends D { k() { return 'E'; } }
  const receivers = [new B(1), new C(2), new D(3), new E(4), Object.assign(new B(5), { z: 1 })];
  hotLoop((i) => receivers[i % 5].m(), () => { A.prototype.k = function () { return this.id * 3; }; },
    (i, after) => ((i % 5) + 1) * (after ? 3 : 2));
});

test('hot static super call follows reassignment and a retargeted class', () => {
  class A { static k(i) { return i + 1; } static get g() { return this.name; } }
  A.x = 'ax';
  class B extends A { static m(i) { return super.k(i) + super.x + super.g; } }
  class Other { static k(i) { return i * 2; } static get g() { return 'other:' + this.name; } }
  Other.x = 'ox';
  hotLoop((i) => B.m(i), () => { A.k = (i) => i - 1; A.x = 'ay'; }, (i, after) => (after ? i - 1 : i + 1) + (after ? 'ay' : 'ax') + 'B');
  hotLoop((i) => B.m(i), () => Object.setPrototypeOf(B, Other), (i, after) => after ? i * 2 + 'ox' + 'other:B' : i - 1 + 'ay' + 'B');
});

test('hot super call in a constructor follows the base and the class prototype chain', () => {
  class A { k() { return 'A'; } }
  A.prototype.x = 1;
  class B extends A { constructor() { super(); this.u = super.k() + super.x; } }
  hotLoop(() => new B().u, () => { A.prototype.k = function () { return 'A2'; }; A.prototype.x = 2; }, (i, after) => after ? 'A22' : 'A1');
  hotLoop(() => new B().u, () => Object.setPrototypeOf(B.prototype, { k() { return 'P'; }, x: 9 }), (i, after) => after ? 'P9' : 'A22');
  class Q { k() { return 'Q'; } }
  Q.prototype.x = 5;
  hotLoop(() => new B().u, () => Object.setPrototypeOf(B, Q), (i, after) => 'P9');
});

test('hot super call from an arrow and from an object literal method', () => {
  class A { k(i) { return i + 1; } }
  class B extends A { m(i) { const f = () => super.k(i); return f(); } }
  const b = new B();
  hotLoop((i) => b.m(i), () => { A.prototype.k = function (i) { return i + 2; }; }, (i, after) => i + (after ? 2 : 1));
  const base = { k(i) { return i + 1; }, x: 3 };
  const literal = { __proto__: base, m(i) { return super.k(i) + super.x; } };
  hotLoop((i) => literal.m(i), () => { base.x = 4; Object.setPrototypeOf(literal, { k: () => 0, x: -1 }); }, (i, after) => after ? -1 : i + 4);
});

test('hot super call in classes made by one factory with different parents', () => {
  const make = (Parent) => class extends Parent { m() { return super.k(); } };
  class P1 { k() { return 1; } }
  class P2 { k() { return 2; } }
  class P3 extends P1 { k() { return 3; } }
  const objects = [new (make(P1))(), new (make(P2))(), new (make(P3))(), new (make(P1))()];
  hotLoop((i) => objects[i & 3].m(), () => { P1.prototype.k = function () { return 10; }; }, (i, after) => [after ? 10 : 1, 2, 3, after ? 10 : 1][i & 3]);
});

test('hot loop inside a method keeps up with changes made by the super callee', () => {
  class A {
    k(i) {
      if (i === FLIP) A.prototype.k = function (j) { if (j === FLIP + 1000) Object.setPrototypeOf(B.prototype, { k: () => 1000000 }); return 2; };
      return 1;
    }
  }
  class B extends A { sum(n) { let s = 0; for (let i = 0; i < n; i++) s += super.k(i); return s; } }
  assert.strictEqual(new B().sum(RUNS), FLIP + 1 + 2 * 1000 + 1000000 * (RUNS - FLIP - 1001));
});

test('hot super getter loop survives the getter retargeting the home', () => {
  let flips = 0;
  class A { get v() { if (++this.n === FLIP) { flips++; Object.setPrototypeOf(B.prototype, { get v() { return 100; } }); } return 1; } }
  class B extends A { constructor() { super(); this.n = 0; } sum(n) { let s = 0; for (let i = 0; i < n; i++) s += super.v; return s; } }
  assert.strictEqual(new B().sum(RUNS), FLIP + 100 * (RUNS - FLIP));
  assert.strictEqual(flips, 1);
});

test('hot super call passes this through sloppy and bound callees', () => {
  class A {}
  A.prototype.sloppy = function () { return this; };
  A.prototype.bound = function () { return this; }.bind('fixed');
  class B extends A { m() { return [super.sloppy(), super.bound()]; } }
  const b = new B();
  for (let i = 0; i < RUNS; i++) {
    const [self, fixed] = b.m();
    assert.strictEqual(self, b);
    assert.strictEqual(String(fixed), 'fixed');
  }
  const viaCall = B.prototype.m.call(7);
  assert.strictEqual(typeof viaCall[0], 'object');
  assert.strictEqual(Number(viaCall[0]), 7);
});

test('hot super method detached from its class keeps its home', () => {
  class A { k() { return 'A'; } }
  class B extends A { m() { return super.k(); } }
  const stolen = { k() { return 'own'; }, m: B.prototype.m };
  hotLoop(() => stolen.m(), () => Object.setPrototypeOf(B.prototype, { k() { return 'moved'; } }), (i, after) => after ? 'moved' : 'A');
});

test('hot super read through a detached method follows the home prototype', () => {
  class A { k() { return 'A'; } }
  A.prototype.x = 1;
  class B extends A { m() { return super.k() + super.x; } static s() { return super.label; } }
  A.label = 'a';
  const m = B.prototype.m, s = B.s, receiver = {};
  const other = { k() { return 'O'; }, x: 2 };
  function Other() {}
  Other.label = 'o';
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) { Object.setPrototypeOf(B.prototype, other); Object.setPrototypeOf(B, Other); }
    assert.strictEqual(m.call(receiver), i < FLIP ? 'A1' : 'O2');
    assert.strictEqual(s.call(receiver), i < FLIP ? 'a' : 'o');
  }
});

console.log('ok');
