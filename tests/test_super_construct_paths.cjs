const assert = require('node:assert');

const N = 8000;
const shapeOf = (o) => `${Object.getPrototypeOf(o) === null ? 'null' : Object.getPrototypeOf(o).constructor.name}:${Array.isArray(o) ? 'array' : typeof o}:${Object.keys(o).join(',')}`;

{
  class Base { constructor(...rest) { this.rest = rest.length; this.first = rest[0]; this.target = new.target; } }
  class Other { constructor(a, b) { this.other = a + b; } }
  class Implicit extends Base {}
  class Deeper extends Implicit {}
  class Explicit extends Deeper { constructor(a, b) { super(a, b, 3, 4); this.own = 1; } }
  class Empty {}
  class OnEmpty extends Empty { constructor(x) { super(); this.x = x; } }
  class ImplicitOnEmpty extends Empty {}
  const three = (i) => new Implicit(i, 2, 3);
  const none = () => new Deeper();
  const explicit = (i) => new Explicit(i, 1);
  for (let i = 0; i < N; i++) {
    if (i === N / 2) Object.setPrototypeOf(Implicit, Other);
    if (i === N / 2 + 1000) Object.setPrototypeOf(Empty, Base);
    if (i === N / 2 + 2000) { Object.setPrototypeOf(OnEmpty, Base); Object.setPrototypeOf(ImplicitOnEmpty, Base); }
    const a = three(i), b = none(), c = explicit(i), d = new OnEmpty(i), e = new ImplicitOnEmpty(i, i);
    assert.ok(a instanceof Implicit && b instanceof Deeper && c instanceof Explicit && d instanceof OnEmpty && e instanceof ImplicitOnEmpty);
    if (i < N / 2) {
      assert.deepStrictEqual([a.rest, a.first, a.target, b.rest, b.first, b.target, c.rest, c.first, c.target, c.own], [3, i, Implicit, 0, undefined, Deeper, 4, i, Explicit, 1]);
    } else {
      assert.deepStrictEqual([a.other, a.rest, Number.isNaN(b.other), c.other, c.own, 'target' in c], [i + 2, undefined, true, i + 1, 1, false]);
    }
    if (i < N / 2 + 2000) assert.deepStrictEqual([{ ...d }, { ...e }], [{ x: i }, {}]);
    else assert.deepStrictEqual([d.rest, d.x, d.target, e.rest, e.first, e.target], [0, i, OnEmpty, 2, i, ImplicitOnEmpty]);
  }
  assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Deeper, [1], Other)), Other.prototype);
  assert.throws(() => Implicit(), TypeError);
}

{
  class First { constructor(x) { this.x = x; } }
  class Second { constructor(x) { this.x = x * 2; } }
  const factory = (Parent) => class extends Parent { constructor(x) { super(x); this.y = 1; } };
  const implicit = (Parent) => class extends Parent {};
  const A1 = factory(First), A2 = factory(First), A3 = factory(Second);
  const I1 = implicit(First), I2 = implicit(Second), I3 = implicit(First);
  const build = (Class, i) => new Class(i);
  const registry = { Explicit: A1, Implicit: I1 };
  const viaRegistry = (i) => new registry.Explicit(i).x + new registry.Implicit(i).x;
  let Current = I1;
  const viaBinding = (i) => new Current(i).x;
  const holder = Object.create({ Inherited: I1 });
  const viaInherited = (i) => new holder.Inherited(i).x;
  for (let i = 0; i < N; i++) {
    if (i === N / 2) { Object.setPrototypeOf(A2, Second); Object.setPrototypeOf(I3, Second); }
    if (i === N / 2 + 500) { registry.Explicit = A3; registry.Implicit = I2; Current = I2; Object.getPrototypeOf(holder).Inherited = I2; }
    const late = i >= N / 2;
    assert.strictEqual(viaRegistry(i), i < N / 2 + 500 ? 2 * i : 4 * i);
    assert.strictEqual(viaBinding(i) + viaInherited(i), i < N / 2 + 500 ? 2 * i : 4 * i);
    assert.deepStrictEqual(
      [build(A1, i).x, build(A2, i).x, build(A3, i).x, build(I1, i).x, build(I2, i).x, build(I3, i).x, new A2(i).y, new A1(i) instanceof A1],
      [i, late ? 2 * i : i, 2 * i, i, 2 * i, late ? 2 * i : i, 1, true],
    );
  }
}

{
  class First { constructor(x) { this.x = x; } }
  class Second { constructor(x) { this.x = -x; } }
  class Returns { constructor(x) { return { returned: x }; } }
  const factory = (Parent) => class extends Parent { constructor(x) { super(x); this.y = 1; } };
  const direct = (Parent) => class extends Parent { constructor(x) { super(x); this.y = 1; } };
  const early = factory(First), earlyReturns = direct(Returns);
  const construct = (Class, i) => new Class(i);
  let late = null, lateOther = null, lateReturns = null;
  for (let i = 0; i < N; i++) {
    if (i === N / 2) { late = factory(First); lateOther = factory(Second); lateReturns = direct(Returns); }
    if (i === 3 * N / 4) Object.setPrototypeOf(late, Second);
    assert.deepStrictEqual([{ ...construct(early, i) }, { ...construct(earlyReturns, i) }], [{ x: i, y: 1 }, { returned: i, y: 1 }]);
    if (!late) continue;
    assert.deepStrictEqual(
      [{ ...construct(late, i) }, { ...construct(lateOther, i) }, { ...construct(lateReturns, i) }, construct(late, i) instanceof late],
      [{ x: i < 3 * N / 4 ? i : -i, y: 1 }, { x: -i, y: 1 }, { returned: i, y: 1 }, true],
    );
  }
}

{
  function Legacy(x) { this.legacy = x; }
  class OnLegacy extends Legacy {}
  class OnNull extends null {}
  class Detached extends Legacy {}
  class NotConstructor extends Legacy {}
  for (let i = 0; i < N; i++) {
    if (i === N / 2) { Object.setPrototypeOf(Detached, null); Object.setPrototypeOf(NotConstructor, () => {}); }
    assert.strictEqual(new OnLegacy(i).legacy, i);
    assert.throws(() => new OnNull(), TypeError);
    if (i < N / 2) assert.deepStrictEqual([new Detached(i).legacy, new NotConstructor(i).legacy], [i, i]);
    else {
      assert.throws(() => new Detached(i), TypeError);
      assert.throws(() => new NotConstructor(i), TypeError);
    }
  }
}

console.log('ok');
