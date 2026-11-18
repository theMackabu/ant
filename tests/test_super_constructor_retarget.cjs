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

console.log('ok');
