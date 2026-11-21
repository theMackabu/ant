const assert = require('node:assert');

function P() { this.p = 1; }
class Q { constructor() { this.q = 2; } static s() { return 'Qs'; } get g() { return 'Qg'; } }
class R { constructor() { this.r = 3; } static s() { return 'Rs'; } }

const construct = (Class, times = 3000) => { let last; for (let i = 0; i < times; i++) last = new Class(i & 1); return last; };

const shapes = {
  explicit: (Parent) => class extends Parent { constructor() { super(); } },
  branchy: (Parent) => class extends Parent { constructor(c) { if (c) { super(); } else { super(); } } },
  implicit: (Parent) => class extends Parent {},
  fields: (Parent) => class extends Parent { f = 1; },
  arrow: (Parent) => class extends Parent { constructor() { (() => super())(); } },
};

for (const [name, make] of Object.entries(shapes)) {
  const Swapped = make(Q);
  assert.strictEqual(construct(Swapped).q, 2, name);
  Object.setPrototypeOf(Swapped, R);
  const swapped = construct(Swapped);
  assert.strictEqual(swapped.r, 3, name);
  assert.strictEqual('q' in swapped, false, name);
  assert.ok(swapped instanceof Swapped, name);
  Object.setPrototypeOf(Swapped, Q);
  assert.strictEqual(construct(Swapped).q, 2, `${name} swapped back`);

  const Detached = make(P);
  Object.setPrototypeOf(Detached, null);
  assert.throws(() => new Detached(1), TypeError, `${name} null parent`);

  const NotConstructor = make(Q);
  Object.setPrototypeOf(NotConstructor, {});
  assert.throws(() => new NotConstructor(1), TypeError, `${name} object parent`);
}

class ViaAssignment extends Q { constructor() { super(); } }
ViaAssignment.__proto__ = R;
assert.strictEqual(new ViaAssignment().r, 3);

class ViaReflect extends Q { constructor() { super(); } }
Reflect.setPrototypeOf(ViaReflect, R);
assert.strictEqual(new ViaReflect().r, 3);

class StaticAndInstance extends Q {
  constructor() { super(); this.fromHome = super.g; }
  static s() { return super.s(); }
  read() { return super.g; }
}
Object.setPrototypeOf(StaticAndInstance, R);
assert.strictEqual(StaticAndInstance.s(), 'Rs');
const instance = new StaticAndInstance();
assert.strictEqual(instance.r, 3);
assert.strictEqual(instance.fromHome, 'Qg');
assert.strictEqual(instance.read(), 'Qg');

class Middle extends Q { constructor() { super(); } }
class Leaf extends Middle { constructor() { super(); this.leaf = true; } }
Object.setPrototypeOf(Middle, R);
assert.deepStrictEqual({ ...new Leaf() }, { r: 3, leaf: true });

let seenTarget;
class Observer { constructor() { seenTarget = new.target; } }
class Observed extends Q { constructor() { super(); } }
Object.setPrototypeOf(Observed, Observer);
new Observed();
assert.strictEqual(seenTarget, Observed);

class FromNull extends null { constructor() { super(); } }
Object.setPrototypeOf(FromNull, R);
assert.strictEqual(new FromNull().r, 3);

class Base { constructor() { this.b = 1; } }
Object.setPrototypeOf(Base, R);
assert.strictEqual(new Base().b, 1);
assert.strictEqual(Base.s(), 'Rs');

class ViaProxy extends new Proxy(Q, {}) { constructor() { super(); this.own = 1; } }
assert.ok(new ViaProxy() instanceof Q);
Object.setPrototypeOf(ViaProxy, new Proxy(R, {}));
assert.deepStrictEqual({ ...new ViaProxy() }, { r: 3, own: 1 });

class DefaultViaProxy extends new Proxy(Q, {}) {}
assert.ok(new DefaultViaProxy() instanceof Q);
Object.setPrototypeOf(DefaultViaProxy, new Proxy(R, {}));
assert.strictEqual(new DefaultViaProxy().r, 3);

class BoundTarget extends Q { constructor() { super(); } }
const boundBefore = BoundTarget.bind(null);
Object.setPrototypeOf(boundBefore, R);
assert.strictEqual(new boundBefore().q, 2);
Object.setPrototypeOf(BoundTarget, R);
assert.strictEqual(new boundBefore().r, 3);
assert.strictEqual(new (BoundTarget.bind(null))().r, 3);

class DefaultBoundTarget extends Q {}
const defaultBound = DefaultBoundTarget.bind(null);
Object.setPrototypeOf(DefaultBoundTarget, R);
assert.strictEqual(new defaultBound().r, 3);

class HomeMoves extends Q {
  constructor() {
    super();
    this.direct = super.g;
    this.viaArrow = (() => super.g)();
  }
}
for (let i = 0; i < 3000; i++) assert.strictEqual(new HomeMoves().direct, 'Qg');
const otherHome = { get g() { return 'other'; } };
Object.setPrototypeOf(HomeMoves.prototype, otherHome);
assert.deepStrictEqual({ ...new HomeMoves() }, { q: 2, direct: 'other', viaArrow: 'other' });
Object.setPrototypeOf(HomeMoves, R);
Object.setPrototypeOf(HomeMoves.prototype, Q.prototype);
assert.deepStrictEqual({ ...new HomeMoves() }, { r: 3, direct: 'Qg', viaArrow: 'Qg' });

class DefaultFromNull extends null {}
assert.throws(() => new DefaultFromNull(), TypeError);
Object.setPrototypeOf(DefaultFromNull, R);
assert.strictEqual(new DefaultFromNull().r, 3);

class RetargetsItself extends Q {
  constructor() {
    Object.setPrototypeOf(RetargetsItself, R);
    super();
  }
}
assert.strictEqual(new RetargetsItself().r, 3);

class ArrowBeforeRetarget extends Q {
  constructor(Parent) {
    const callSuper = () => super();
    Object.setPrototypeOf(ArrowBeforeRetarget, Parent);
    callSuper();
  }
}
for (let i = 0; i < 3000; i++) {
  assert.strictEqual(new ArrowBeforeRetarget(R).r, 3);
  assert.strictEqual(new ArrowBeforeRetarget(Q).q, 2);
}

let plainCalls = 0;
let constructions = 0;
function Counted() { if (new.target) constructions++; else plainCalls++; }
const holder = { Counted };
class CallsParentPlainly extends Counted {
  constructor() {
    super();
    Counted();
    holder.Counted();
  }
}
for (let i = 0; i < 3000; i++) new CallsParentPlainly();
assert.strictEqual(constructions, 3000);
assert.strictEqual(plainCalls, 6000);

class CallsItselfPlainly extends Q {
  constructor(recurse) {
    super();
    if (recurse) CallsItselfPlainly();
  }
}
assert.throws(() => new CallsItselfPlainly(true), TypeError);
assert.strictEqual(new CallsItselfPlainly(false).q, 2);

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
const describe = (o) => JSON.stringify([Object.keys(o), Object.values(o).map(String)]);

test('hot derived construction builds the same object as the parent chain describes', () => {
  class A { constructor(x) { this.x = x; } }
  class B extends A { constructor(x, y) { super(x); this.y = y; } }
  class C extends B { constructor(x, y, z) { super(x, y); this.z = z; } }
  for (let i = 0; i < RUNS; i++) {
    const c = new C(i, i + 1, i + 2);
    assert.strictEqual(describe(c), describe({ x: i, y: i + 1, z: i + 2 }));
    assert.strictEqual(Object.getPrototypeOf(c), C.prototype);
    assert.strictEqual(c.constructor, C);
    const b = new B(i, 'b');
    assert.strictEqual(describe(b), describe({ x: i, y: 'b' }));
    assert.strictEqual(Object.getPrototypeOf(b), B.prototype);
  }
});

test('hot derived construction follows Object.setPrototypeOf between constructions', () => {
  class A { constructor(x) { this.a = x; } }
  class Other { constructor(x) { this.o = x * 2; } }
  class B extends A { constructor(x) { super(x); this.b = 1; } }
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) Object.setPrototypeOf(B, Other);
    if (i === FLIP + 1500) Object.setPrototypeOf(B, A);
    const viaOther = i >= FLIP && i < FLIP + 1500;
    const b = new B(i);
    assert.strictEqual(describe(b), describe(viaOther ? { o: i * 2, b: 1 } : { a: i, b: 1 }));
    assert.strictEqual(b instanceof B, true);
    assert.strictEqual(b instanceof A, true);
    assert.strictEqual(b instanceof Other, false);
  }
});

test('hot derived construction sees a parent swapped while arguments are evaluated', () => {
  class A { constructor(x) { this.a = x; } }
  class Other { constructor(x) { this.o = x; } }
  let swapAt = -1;
  const pick = (i) => { if (i === swapAt) Object.setPrototypeOf(B, Other); return i; };
  class B extends A { constructor(i) { super(pick(i)); } }
  swapAt = FLIP;
  for (let i = 0; i < RUNS; i++) assert.strictEqual(describe(new B(i)), describe(i >= FLIP ? { o: i } : { a: i }));
});

test('hot derived construction sees a parent swapped by the running parent', () => {
  class Other { constructor() { this.o = 1; } }
  class A { constructor(i) { this.a = i; if (i === FLIP) Object.setPrototypeOf(B, Other); } }
  class B extends A { constructor(i) { super(i); this.b = i; } }
  class C extends B { constructor(i) { super(i); this.c = i; } }
  for (let i = 0; i < RUNS; i++) {
    assert.strictEqual(describe(new C(i)), describe(i > FLIP ? { o: 1, b: i, c: i } : { a: i, b: i, c: i }));
  }
});

test('hot derived construction throws once the parent is null or not a constructor', () => {
  class A { constructor() { this.a = 1; } }
  class B extends A { constructor() { super(); } }
  class C extends A { constructor() { super(); } }
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) { Object.setPrototypeOf(B, null); Object.setPrototypeOf(C, () => {}); }
    if (i >= FLIP) {
      assert.throws(() => new B(), TypeError);
      assert.throws(() => new C(), TypeError);
    } else {
      assert.strictEqual(new B().a, 1);
      assert.strictEqual(new C().a, 1);
    }
  }
});

test('hot derived construction with a parent that returns another object or a builtin function', () => {
  const replacement = { replaced: true };
  let mode = 0;
  class A { constructor() { this.a = 1; if (mode === 1) return replacement; if (mode === 2) return Math.max; if (mode === 3) return 5; } }
  class B extends A { constructor() { super(); this.tag = 'b'; } }
  for (let i = 0; i < RUNS; i++) {
    mode = i < FLIP ? 0 : (i % 3) + 1;
    const made = new B();
    if (mode === 1) assert.strictEqual(made, replacement);
    else if (mode === 2) assert.strictEqual(made, Math.max);
    else assert.strictEqual(Object.getPrototypeOf(made), B.prototype);
    assert.strictEqual(made.tag, 'b');
    if (mode === 0 || mode === 3) assert.strictEqual(describe(made), describe({ a: 1, tag: 'b' }));
  }
  delete Math.max.tag;
  class Always { constructor() { return replacement; } }
  class D extends Always { constructor() { super(); this.count = (this.count || 0) + 1; } }
  for (let i = 0; i < RUNS; i++) assert.strictEqual(new D(), replacement);
  assert.strictEqual(replacement.count, RUNS);
});

test('hot derived constructor return values', () => {
  const other = { other: true };
  let mode = 0;
  class A { constructor() { this.a = 1; } }
  class B extends A { constructor() { super(); if (mode === 1) return other; if (mode === 2) return 1; if (mode === 3) return undefined; } }
  class C extends B { constructor() { super(); this.c = 1; } }
  for (let i = 0; i < RUNS; i++) {
    mode = i < FLIP ? 0 : i % 4;
    if (mode === 2) {
      assert.throws(() => new B(), TypeError);
      assert.throws(() => new C(), TypeError);
      continue;
    }
    const b = new B(), c = new C();
    assert.strictEqual(b === other, mode === 1);
    assert.strictEqual(c === other, mode === 1);
    assert.strictEqual(c.c, 1);
    if (mode !== 1) assert.strictEqual(describe(c), describe({ a: 1, c: 1 }));
  }
});

test('hot derived constructor that returns a primitive only sometimes', () => {
  class A { constructor() { this.a = 1; } }
  class B extends A { constructor(bad) { super(); if (bad) return 1; } }
  class C extends B { constructor(bad) { super(bad); this.c = 1; } }
  for (let i = 0; i < RUNS; i++) {
    const bad = i >= FLIP && i % 2 === 0;
    if (bad) {
      assert.throws(() => new B(bad), TypeError);
      assert.throws(() => new C(bad), TypeError);
    } else {
      assert.strictEqual(new B(bad).a, 1);
      assert.strictEqual(new C(bad).c, 1);
    }
  }
});

test('hot three-level construction passes new.target on after a middle constructor changes course', () => {
  class A { constructor(x) { this.target = new.target; this.x = x; } }
  class B extends A { constructor(x) { const y = x + 1; super(y); } }
  class C extends B { constructor(x) { super(x); this.c = new.target; } }
  for (let i = 0; i < RUNS; i++) {
    const x = i >= FLIP && i % 7 === 0 ? 'text' : i;
    const c = new C(x);
    assert.strictEqual(c.target, C);
    assert.strictEqual(c.c, C);
    assert.strictEqual(c.x, x + 1);
    assert.strictEqual(new B(x).target, B);
  }
});

test('hot three-level construction continues with the object a swapped grandparent returns', () => {
  const replacement = { replaced: true };
  class A { constructor(x) { this.a = x; } }
  class Other { constructor() { return replacement; } }
  let swapAt = -1;
  const pick = (i) => { if (i === swapAt) Object.setPrototypeOf(B, Other); return i; };
  class B extends A { constructor(i) { super(pick(i)); this.b = i; } }
  class C extends B { constructor(i) { super(i); this.c = i; } }
  swapAt = FLIP;
  for (let i = 0; i < RUNS; i++) {
    const c = new C(i);
    if (i < FLIP) {
      assert.strictEqual(describe(c), describe({ a: i, b: i, c: i }));
      continue;
    }
    assert.strictEqual(c, replacement);
    assert.strictEqual(describe(c), describe({ replaced: true, b: i, c: i }));
  }
});

test('hot derived construction keeps new.target through inlined parents', () => {
  class A { constructor(x) { this.sum = x + 1; this.target = new.target; } }
  class B extends A { constructor(x) { super(x); this.mid = new.target; } }
  class C extends B { constructor(x) { super(x); } }
  function Plain() {}
  for (let i = 0; i < RUNS; i++) {
    const x = i === FLIP || i === FLIP + 7 ? 'text' : i;
    const c = new C(x), b = new B(x);
    assert.strictEqual(c.sum, x + 1);
    assert.strictEqual(c.target, C);
    assert.strictEqual(c.mid, C);
    assert.strictEqual(b.target, B);
    assert.strictEqual(b.mid, B);
    const r = Reflect.construct(C, [x], Plain);
    assert.strictEqual(r.target, Plain);
    assert.strictEqual(r.mid, Plain);
    assert.strictEqual(Object.getPrototypeOf(r), Plain.prototype);
  }
});

test('hot derived construction survives a parent body that changes behaviour', () => {
  class A { constructor(x) { this.x = x; if (typeof x === 'string') this.extra = x.length; this.done = true; } }
  class B extends A { constructor(x) { super(x); this.y = this.x; } }
  class C extends B { constructor(x) { const before = typeof x; super(x); this.before = before; } }
  for (let i = 0; i < RUNS; i++) {
    const x = i >= FLIP && i % 5 === 0 ? 's' + i : i;
    const expected = typeof x === 'string' ? { x, extra: x.length, done: true, y: x } : { x, done: true, y: x };
    assert.strictEqual(describe(new B(x)), describe(expected));
    assert.strictEqual(describe(new C(x)), describe({ ...expected, before: typeof x }));
  }
});

test('hot derived construction propagates a parent exception and leaves no object behind', () => {
  let made = 0;
  class A { constructor(x) { if (x < 0) throw new RangeError('negative'); this.x = x; made++; } }
  class B extends A { constructor(x) { super(x); this.after = true; } }
  for (let i = 0; i < RUNS; i++) {
    if (i >= FLIP && i % 3 === 0) assert.throws(() => new B(-i - 1), RangeError);
    else assert.strictEqual(new B(i).after, true);
  }
  assert.strictEqual(made, RUNS - Math.ceil((RUNS - FLIP) / 3));
});

test('hot derived construction with class fields and arrows around super()', () => {
  class A { base = 'a'; constructor(x) { this.x = x; } }
  class B extends A { own = this.x + 1; self = () => this; }
  class C extends A { late = this.x * 2; constructor(x) { const early = () => this; super(x); this.early = early(); } }
  class D extends A { constructor(x) { const run = () => super(x); run(); this.viaArrow = true; } }
  for (let i = 0; i < RUNS; i++) {
    const b = new B(i), c = new C(i), d = new D(i);
    assert.strictEqual(describe({ ...b, self: 0 }), describe({ base: 'a', x: i, own: i + 1, self: 0 }));
    assert.strictEqual(b.self(), b);
    assert.strictEqual(describe({ ...c, early: 0 }), describe({ base: 'a', x: i, late: i * 2, early: 0 }));
    assert.strictEqual(c.early, c);
    assert.strictEqual(describe(d), describe({ base: 'a', x: i, viaArrow: true }));
  }
});

test('hot derived construction enforces this before super() and a single super()', () => {
  class A { constructor() { this.a = 1; } }
  let early = false, twice = false;
  class B extends A { constructor() { if (early) this.touch = 1; super(); if (twice) super(); } }
  for (let i = 0; i < RUNS; i++) {
    early = i >= FLIP && i % 2 === 0;
    twice = i >= FLIP && i % 2 === 1;
    if (early || twice) assert.throws(() => new B(), ReferenceError);
    else assert.strictEqual(new B().a, 1);
  }
});

test('hot bound classes and Reflect.construct with another newTarget', () => {
  class A { constructor(x, y) { this.x = x; this.y = y; } }
  class B extends A { constructor(x, y) { super(x, y); this.b = true; } }
  const Bound = B.bind(null, 'bound');
  class Target { constructor() { throw new Error('never constructed'); } }
  for (let i = 0; i < RUNS; i++) {
    const bound = new Bound(i);
    assert.strictEqual(describe(bound), describe({ x: 'bound', y: i, b: true }));
    assert.strictEqual(Object.getPrototypeOf(bound), B.prototype);
    const reflected = Reflect.construct(B, [i, 2], Target);
    assert.strictEqual(describe(reflected), describe({ x: i, y: 2, b: true }));
    assert.strictEqual(Object.getPrototypeOf(reflected), Target.prototype);
    assert.strictEqual(reflected instanceof B, false);
  }
});

test('hot derived construction over builtins, plain functions and default constructors', () => {
  function Plain(x) { this.plain = x; }
  class FromPlain extends Plain { constructor(x) { super(x); this.d = 1; } }
  class Implicit extends FromPlain {}
  class List extends Array { constructor(n) { super(n); this.tagged = true; } }
  class Failure extends Error { constructor(m) { super(m); this.code = 7; } }
  for (let i = 0; i < RUNS; i++) {
    assert.strictEqual(describe(new FromPlain(i)), describe({ plain: i, d: 1 }));
    const implicit = new Implicit(i);
    assert.strictEqual(describe(implicit), describe({ plain: i, d: 1 }));
    assert.strictEqual(Object.getPrototypeOf(implicit), Implicit.prototype);
    if (i % 50 === 0) {
      const list = new List(3);
      assert.strictEqual(list.length, 3);
      assert.strictEqual(Array.isArray(list), true);
      assert.strictEqual(list.tagged, true);
      const failure = new Failure('m' + i);
      assert.strictEqual(failure.message, 'm' + i);
      assert.strictEqual(failure.code, 7);
      assert.strictEqual(failure instanceof Failure, true);
    }
  }
});

test('hot classes from one factory with different parents', () => {
  const make = (Parent) => class extends Parent { constructor(x) { super(x); this.made = true; } };
  class P1 { constructor(x) { this.p1 = x; } }
  class P2 { constructor(x) { this.p2 = x; } }
  const classes = [make(P1), make(P2), make(P1), make(class extends P2 { constructor(x) { super(x + 1); } })];
  for (let i = 0; i < RUNS; i++) {
    const k = i & 3;
    const expected = [{ p1: i, made: true }, { p2: i, made: true }, { p1: i, made: true }, { p2: i + 1, made: true }][k];
    assert.strictEqual(describe(new classes[k](i)), describe(expected));
  }
});

test('hot derived construction with super property access after super()', () => {
  class A { constructor() { this.x = 0; } k(i) { return i + 1; } }
  class B extends A { constructor(i) { super(); super.x = i; this.u = super.k(i); } }
  for (let i = 0; i < RUNS; i++) {
    if (i === FLIP) A.prototype.k = function (j) { return j - 1; };
    assert.strictEqual(describe(new B(i)), describe({ x: i, u: i < FLIP ? i + 1 : i - 1 }));
  }
});

test('hot derived construction rejects parents that are not constructors', () => {
  class A { constructor() { this.a = 1; } }
  const parents = [{ m() {} }.m, async function f() {}, function* g() {}, () => {}, Math.max, {}];
  for (const parent of parents) {
    class B extends A { constructor() { super(); } }
    for (let i = 0; i < RUNS; i++) {
      if (i === FLIP) Object.setPrototypeOf(B, parent);
      if (i < FLIP) assert.strictEqual(new B().a, 1);
      else if (i % 100 === 0) assert.throws(() => new B(), TypeError);
    }
  }
});

test('hot derived construction keeps following a parent that is replaced repeatedly', () => {
  const args = [1, 2];
  class P { constructor(a, b, c) { this.v = b; } }
  class Q { constructor(a, b, c) { this.v = c; } }
  class R { constructor(a, b, c) { this.v = a + b + c; } }
  class Plain extends P { constructor(i) { super(1, 2, i); } }
  class Spread extends P { constructor(i) { super(...args, i); } }
  const parents = [P, Q, R, P, Q, R, Q];
  const value = (parent, i) => parent === P ? 2 : parent === Q ? i : 3 + i;
  for (const derived of [Plain, Spread]) {
    for (let i = 0; i < RUNS * parents.length; i++) {
      const parent = parents[Math.floor(i / RUNS)];
      if (i % RUNS === 0) Object.setPrototypeOf(derived, parent);
      const made = new derived(i);
      if (made.v !== value(parent, i)) assert.fail(`${derived.name} under ${parent.name} at ${i}: ${made.v}`);
    }
  }
});

console.log('ok');
