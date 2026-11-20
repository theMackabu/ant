const assert = require('node:assert');

function run(body) { for (let round = 0; round < 3; round++) body(round); }

{
  class A { m(x) { return x + 1; } get g() { return this.k * 2; } }
  A.prototype.d = 5;
  class B extends A {
    constructor() { super(); this.k = 3; this.v = 0; }
    m(x) { return super.m(x) + 10; }
    readD() { return super.d; }
    readG() { return super.g; }
    readMissing() { return super.nothing; }
    put(x) { super.v = x; return this.v; }
  }
  const b = new B();
  let sum = 0;
  for (let i = 0; i < 20000; i++) sum += b.m(i) + b.readD() + b.readG() + b.put(i) + (b.readMissing() === undefined ? 1 : 0);
  assert.strictEqual(sum, 20000 * 23 + 2 * (19999 * 20000 / 2));

  A.prototype.m = function (x) { return x + 2; };
  assert.strictEqual(b.m(1), 13);
  A.prototype.d = 6;
  assert.strictEqual(b.readD(), 6);
  Object.defineProperty(A.prototype, 'd', { get() { return this.k; }, configurable: true });
  assert.strictEqual(b.readD(), 3);
  A.prototype.nothing = 'now';
  assert.strictEqual(b.readMissing(), 'now');
  delete A.prototype.m;
  assert.throws(() => b.m(1), TypeError);
  Object.prototype.m = function (x) { return x * 100; };
  assert.strictEqual(b.m(1), 110);
  delete Object.prototype.m;

  let hits = 0;
  Object.defineProperty(A.prototype, 'v', { set(x) { hits += x; }, configurable: true });
  assert.strictEqual(b.put(7), 19999);
  assert.strictEqual(hits, 7);
  delete A.prototype.v;
  assert.strictEqual(b.put(8), 8);
  Object.defineProperty(Object.prototype, 'v', { set(x) { hits += x; }, configurable: true });
  assert.strictEqual(b.put(9), 8);
  assert.strictEqual(hits, 16);
  delete Object.prototype.v;
  Object.defineProperty(A.prototype, 'v', { value: 1, writable: false, configurable: true });
  assert.throws(() => b.put(10), TypeError);
  delete A.prototype.v;
  Object.defineProperty(b, 'v', { value: 11, writable: false, configurable: true });
  assert.throws(() => b.put(12), TypeError);
  assert.strictEqual(b.v, 11);

  const other = { __proto__: { m(x) { return -x; }, d: 'od' } };
  Object.setPrototypeOf(B.prototype, other.__proto__);
  assert.strictEqual(b.m(4), 6);
  assert.strictEqual(b.readD(), 'od');
  Object.setPrototypeOf(B.prototype, null);
  assert.throws(() => b.m(1), TypeError);
  assert.throws(() => b.readD(), TypeError);
  assert.throws(() => b.put(1), TypeError);
}

run(() => {
  class A { tag() { return 'A'; } }
  class B extends A { tag() { return super.tag() + 'B'; } set(x) { super.w = x; return this; } }
  class C extends B { tag() { return super.tag() + 'C'; } }
  const shapes = [new C(), Object.assign(new C(), { a: 1 }), Object.assign(new C(), { b: 1, w: 0 }), new B()];
  for (let i = 0; i < 20000; i++) {
    const o = shapes[i & 3];
    assert.strictEqual(o.tag(), o instanceof C ? 'ABC' : 'AB');
    assert.strictEqual(o.set(i).w, i);
  }
  const borrowed = B.prototype.set;
  const plain = {};
  assert.strictEqual(borrowed.call(plain, 5).w, 5);
  const list = [];
  assert.strictEqual(borrowed.call(list, 6).w, 6);
  const fn = function () {};
  assert.strictEqual(borrowed.call(fn, 7).w, 7);
  assert.strictEqual(B.prototype.tag.call(1), 'AB');
  const frozen = Object.freeze(Object.assign(new C(), { w: 1 }));
  for (let i = 0; i < 2000; i++) assert.throws(() => frozen.set(i), TypeError);
  assert.strictEqual(frozen.w, 1);
});

run(() => {
  const base = { hi() { return 'base'; }, n: 1 };
  const obj = { __proto__: base, n: 0, hi() { return super.hi() + '!'; }, bump(x) { super.n = x; return this.n; }, peek() { return super.n; } };
  for (let i = 0; i < 20000; i++) {
    assert.strictEqual(obj.hi(), 'base!');
    assert.strictEqual(obj.bump(i), i);
    assert.strictEqual(obj.peek(), 1);
  }
  base.n = 2;
  assert.strictEqual(obj.peek(), 2);
  const moved = { hi: obj.hi, __proto__: { hi() { return 'moved'; } } };
  assert.strictEqual(moved.hi(), 'base!');
  Object.setPrototypeOf(obj, { hi() { return 'swapped'; }, set n(x) { this.seen = x; } });
  assert.strictEqual(obj.hi(), 'swapped!');
  assert.strictEqual(obj.bump(9), 19999);
  assert.strictEqual(obj.seen, 9);
});

{
  class A { static s() { return 's'; } }
  class B extends A {
    static s() { return super.s() + '!'; }
    static put(x) { super.z = x; return this.z; }
    viaArrow() { return (() => super.constructor.name)(); }
  }
  for (let i = 0; i < 20000; i++) {
    assert.strictEqual(B.s(), 's!');
    assert.strictEqual(B.put(i), i);
    assert.strictEqual(new B().viaArrow(), 'A');
  }
}

console.log('ok');
