const assert = require('node:assert');

class P {
  set s(v) { this._s = v * 2; }
}
Object.defineProperty(P.prototype, 'ro', { value: 1 });

class Q extends P {
  run() {
    super.k = 1;
    super[3] = 2;
    assert.ok(Object.hasOwn(this, 'k'));
    assert.strictEqual(this[3], 2);
    assert.ok(!Object.hasOwn(Q.prototype, 'k') && !Object.hasOwn(P.prototype, 'k'));

    super.s = 4;
    assert.strictEqual(this._s, 8);
    assert.ok(!Object.hasOwn(this, 's'));

    this.k = 1;
    assert.ok(Number.isNaN((super.k += 10)));
    assert.ok(Number.isNaN(super.k++));
    super.n ??= 7;
    super.n ??= 8;
    assert.strictEqual(this.n, 8);

    [super.a, super['b']] = [1, 2];
    ({ x: super.c } = { x: 3 });
    assert.deepStrictEqual([this.a, this.b, this.c], [1, 2, 3]);

    const sym = Symbol('q');
    super[sym] = 'sym';
    assert.strictEqual(this[sym], 'sym');
    assert.strictEqual((super.z = 'z'), 'z');
    (() => { super.w = 'arrow'; })();
    assert.strictEqual(this.w, 'arrow');

    assert.throws(() => { super.ro = 2; }, TypeError);
  }

  static st() {
    super.sv = 1;
    return Object.hasOwn(Q, 'sv');
  }
}

new Q().run();
assert.ok(Q.st());

class A extends Array {
  set() {
    super[0] = 'x';
    super.length = 1;
    return [this[0], this.length];
  }
}
const a = new A();
a.push(1, 2, 3);
assert.deepStrictEqual(a.set(), ['x', 1]);

const o = {
  __proto__: { set t(v) { this.tt = v; } },
  m() {
    super.t = 1;
    super.u = 2;
    return [this.tt, Object.hasOwn(this, 'u')];
  },
};
assert.deepStrictEqual(o.m(), [1, true]);

function test(name, body) {
  try {
    body();
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

const RUNS = 8000;
const FLIP = 5000;

test('hot super store writes the receiver and yields the value', () => {
  class A { constructor() { this.x = 0; } }
  class B extends A { put(v) { return super.x = v; } }
  const b = new B();
  for (let i = 0; i < RUNS; i++) {
    assert.strictEqual(b.put(i), i);
    assert.strictEqual(b.x, i);
  }
  assert.strictEqual(Object.hasOwn(A.prototype, 'x'), false);
});

test('hot super store sees a setter added on the base', () => {
  class A { constructor() { this.x = 0; } }
  class B extends A { put(v) { super.x = v; } }
  const b = new B();
  let hits = 0;
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.defineProperty(A.prototype, 'x', { set(v) { hits++; assert.strictEqual(this, b); }, configurable: true });
    b.put(i);
    assert.strictEqual(b.x, i < FLIP ? i : FLIP - 1);
  }
  assert.strictEqual(hits, RUNS - FLIP);
});

test('hot super store sees a setter added further up the chain', () => {
  class A {}
  class B extends A { constructor() { super(); this.x = 0; } }
  class C extends B { put(v) { super.x = v; } }
  const c = new C();
  let hits = 0;
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.defineProperty(A.prototype, 'x', { set(v) { hits++; }, configurable: true });
    c.put(i);
    assert.strictEqual(c.x, i < FLIP ? i : FLIP - 1);
  }
  assert.strictEqual(hits, RUNS - FLIP);
});

test('hot super store sees a setter added on Object.prototype', () => {
  class A { constructor() { this.zz = 0; } }
  class B extends A { put(v) { super.zz = v; } }
  const b = new B();
  let hits = 0;
  try {
    for (let i = 0; i < RUNS; i++) {
      if (i === FLIP) Object.defineProperty(Object.prototype, 'zz', { set(v) { hits++; }, configurable: true });
      b.put(i);
      assert.strictEqual(b.zz, i < FLIP ? i : FLIP - 1);
    }
  } finally {
    delete Object.prototype.zz;
  }
  assert.strictEqual(hits, RUNS - FLIP);
  b.put(-1);
  assert.strictEqual(b.zz, -1);
});

test('hot super store throws once the base property is read-only', () => {
  class A { constructor() { this.x = 0; } }
  A.prototype.x = 'base';
  class B extends A { put(v) { super.x = v; } }
  const b = new B();
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.defineProperty(A.prototype, 'x', { writable: false });
    if (i >= FLIP) assert.throws(() => b.put(i), TypeError);
    else b.put(i);
    assert.strictEqual(b.x, i < FLIP ? i : FLIP - 1);
  }
  assert.strictEqual(A.prototype.x, 'base');
});

test('hot super store follows Object.setPrototypeOf on the home object', () => {
  class A { constructor() { this.x = 0; } }
  class B extends A { put(v) { super.x = v; } }
  const b = new B();
  const seen = [];
  const other = { set x(v) { seen.push(this === b ? v : 'wrong'); } };
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.setPrototypeOf(B.prototype, other);
    b.put(i);
    assert.strictEqual(b.x, i < FLIP ? i : FLIP - 1);
  }
  assert.strictEqual(seen.length, RUNS - FLIP);
  assert.strictEqual(seen[0], FLIP);
  assert.strictEqual(seen.includes('wrong'), false);
});

test('hot super store throws on a frozen receiver and leaves it unchanged', () => {
  class A { constructor() { this.x = 0; } }
  class B extends A { put(v) { super.x = v; } }
  const b = new B();
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.freeze(b);
    if (i >= FLIP) assert.throws(() => b.put(i), TypeError);
    else b.put(i);
    assert.strictEqual(b.x, i < FLIP ? i : FLIP - 1);
  }
});

test('hot super store respects a receiver property turned read-only or into an accessor', () => {
  class A { constructor() { this.x = 0; this.y = 0; } }
  class B extends A { putX(v) { super.x = v; } putY(v) { super.y = v; } }
  const b = new B();
  let setterCalls = 0;
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) {
      Object.defineProperty(b, 'x', { writable: false });
      Object.defineProperty(b, 'y', { get() { return 'accessor'; }, set(v) { setterCalls++; }, configurable: true });
    }
    if (i >= FLIP) {
      assert.throws(() => b.putX(i), TypeError);
      assert.throws(() => b.putY(i), TypeError);
    } else {
      b.putX(i);
      b.putY(i);
    }
    assert.strictEqual(b.x, i < FLIP ? i : FLIP - 1);
    assert.strictEqual(b.y, i < FLIP ? i : 'accessor');
  }
  assert.strictEqual(setterCalls, 0);
});

test('hot super store re-creates a deleted receiver property', () => {
  class A { constructor() { this.x = 0; this.tail = 1; } }
  class B extends A { put(v) { super.x = v; } }
  const b = new B();
  for (let i = 0; i < RUNS; i++) {
    if (i % 1000 === 999) delete b.x;
    b.put(i);
    assert.strictEqual(b.x, i);
    assert.strictEqual(Object.hasOwn(b, 'x'), true);
  }
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(b, 'x'), { value: RUNS - 1, writable: true, enumerable: true, configurable: true });
});

test('hot super store shared by receivers of several shapes', () => {
  class A {}
  class B extends A { constructor() { super(); this.x = 0; } put(v) { super.x = v; } }
  class C extends B { constructor() { super(); this.c = 1; } }
  class D extends B { constructor() { super(); this.d = 1; this.e = 2; } }
  const frozen = Object.freeze(new C());
  const receivers = [new B(), new C(), new D(), Object.assign(new B(), { late: 1 }), Object.assign(new D(), { later: 1 })];
  for (let i = 0; i < RUNS; i++) {
    const target = receivers[i % receivers.length];
    target.put(i);
    assert.strictEqual(target.x, i);
    if (i % 997 === 0) assert.throws(() => frozen.put(i), TypeError);
  }
  assert.strictEqual(frozen.x, 0);
  assert.strictEqual(Object.hasOwn(A.prototype, 'x'), false);
  assert.strictEqual(Object.hasOwn(B.prototype, 'x'), false);
});

test('hot super store through a Proxy base', () => {
  class A { constructor() { this.x = 0; } }
  class B extends A { put(v) { super.x = v; } }
  const b = new B();
  const seen = [];
  const proxy = new Proxy({}, { set(t, key, value, receiver) { seen.push(receiver === b && key === 'x' ? value : 'wrong'); return true; } });
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.setPrototypeOf(B.prototype, proxy);
    b.put(i);
    assert.strictEqual(b.x, i < FLIP ? i : FLIP - 1);
  }
  assert.strictEqual(seen.length, RUNS - FLIP);
  assert.strictEqual(seen.includes('wrong'), false);
});

test('hot super store throws once the home prototype is null', () => {
  class A { constructor() { this.x = 0; } }
  class B extends A { put(v) { super.x = v; } }
  const b = new B();
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.setPrototypeOf(B.prototype, null);
    if (i >= FLIP) assert.throws(() => B.prototype.put.call(b, i), TypeError);
    else b.put(i);
    assert.strictEqual(b.x, i < FLIP ? i : FLIP - 1);
  }
});

test('hot super store in a static method and in a constructor', () => {
  class A { constructor() { this.x = 0; } }
  A.sx = 'base';
  class B extends A { constructor(v) { super(); super.x = v; } static put(v) { super.sx = v; } }
  let hits = 0;
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) {
      Object.defineProperty(A, 'sx', { set(v) { hits++; }, configurable: true });
      Object.defineProperty(A.prototype, 'x', { set(v) { hits++; }, configurable: true });
    }
    B.put(i);
    assert.strictEqual(Object.getOwnPropertyDescriptor(B, 'sx').value, i < FLIP ? i : FLIP - 1);
    assert.strictEqual(new B(i).x, i < FLIP ? i : undefined);
  }
  assert.strictEqual(hits, 3 * (RUNS - FLIP));
});

test('hot loop of super stores keeps up with a setter installed by a callee', () => {
  let hits = 0;
  class A { constructor() { this.x = 0; } }
  class B extends A {
    fill(n) {
      for (let i = 0; i < n; i++) {
        super.x = i;
        if (i === FLIP) this.install();
      }
      return this.x;
    }
    install() { Object.defineProperty(A.prototype, 'x', { set(v) { hits++; }, configurable: true }); }
  }
  assert.strictEqual(new B().fill(RUNS), FLIP);
  assert.strictEqual(hits, RUNS - FLIP - 1);
});

test('hot super store from an object literal method and an arrow', () => {
  const base = {};
  const literal = { __proto__: base, x: 0, put(v) { const set = () => { super.x = v; }; set(); return super.x = v + 1; } };
  let hits = 0;
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.defineProperty(base, 'x', { set(v) { hits++; }, get() { return 'hidden'; }, configurable: true });
    assert.strictEqual(literal.put(i), i + 1);
    assert.strictEqual(Object.getOwnPropertyDescriptor(literal, 'x').value, i < FLIP ? i + 1 : FLIP);
  }
  assert.strictEqual(hits, 2 * (RUNS - FLIP));
});

test('hot super store through a detached method follows the home prototype', () => {
  class A { constructor() { this.x = 0; } }
  class B extends A { put(v) { return super.x = v; } }
  const put = B.prototype.put, b = new B();
  let hits = 0;
  const other = { set x(v) { hits++; } };
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.setPrototypeOf(B.prototype, other);
    if (i === FLIP + 1000) Object.setPrototypeOf(B.prototype, new Proxy({}, { set() { hits += 10; return true; } }));
    if (i === FLIP + 2000) Object.setPrototypeOf(B.prototype, null);
    if (i >= FLIP + 2000) assert.throws(() => put.call(b, i), TypeError);
    else assert.strictEqual(put.call(b, i), i);
    assert.strictEqual(b.x, i < FLIP ? i : FLIP - 1);
  }
  assert.strictEqual(hits, 1000 + 10 * 1000);
});

console.log('ok');
