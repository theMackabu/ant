const assert = require('node:assert');

const N = 8000;
const shapeOf = (o) => `${Object.getPrototypeOf(o) === null ? 'null' : Object.getPrototypeOf(o).constructor.name}:${Array.isArray(o) ? 'array' : typeof o}:${Object.keys(o).join(',')}`;

{
  class Returns { constructor(x) { return { x, tag: 1 }; } }
  class Plain { constructor(x) { this.x = x + 1; } }
  class Child extends Returns { constructor(x) { super(x); this.y = 1; } }
  const make = (i) => new Child(i);
  for (let i = 0; i < N; i++) {
    if (i === N / 2) Object.setPrototypeOf(Child, Plain);
    if (i === N - 1000) Object.setPrototypeOf(Child, Returns);
    const made = make(i);
    const replaced = i < N / 2 || i >= N - 1000;
    assert.strictEqual(shapeOf(made), replaced ? 'Object:object:x,tag,y' : 'Child:object:x,y', `returns ${i}`);
    assert.strictEqual(made.x, replaced ? i : i + 1);
    assert.strictEqual(made instanceof Child, !replaced);
  }
}

{
  let mode = 0;
  const marker = function marker() {};
  class Varied {
    constructor(x) {
      this.seen = x;
      if (mode === 1) return [x];
      if (mode === 2) return marker;
      if (mode === 3) return 5;
      if (mode === 4) return null;
      if (mode === 5) return 'text';
    }
  }
  class Child extends Varied { constructor(x) { super(x); this.own = x; } }
  const make = (i) => new Child(i);
  const expected = ['Child:object:seen,own', 'Array:array:0,own', 'Function:function:own', 'Child:object:seen,own', 'Child:object:seen,own', 'Child:object:seen,own'];
  for (let i = 0; i < N; i++) {
    mode = i < N / 2 ? 0 : (i >> 4) % 6;
    const made = make(i);
    assert.strictEqual(shapeOf(made), expected[mode], `varied ${i} mode ${mode}`);
    assert.strictEqual(made.own, i);
    if (mode === 2) { assert.strictEqual(made, marker); delete marker.own; }
  }
}

{
  class Wide {
    constructor(x) {
      let a = x + 1, b = a + 2, c = b + 3, d = c + 4, e = d + 5, f = e + 6, g = f + 7, h = g + 8;
      a = b + c; b = c + d; c = d + e; d = e + f; e = f + g; f = g + h; g = h + a; h = a + b;
      a = b - c; b = c - d; c = d - e; d = e - f; e = f - g; f = g - h; g = h - a; h = a - b;
      a = b + c; b = c + d; c = d + e; d = e + f; e = f + g; f = g + h; g = h + a; h = a + b;
      this.x = x;
      this.h = h | 0;
      this.target = new.target;
      this.count = arguments.length;
    }
  }
  class Other { constructor(x) { this.x = -x; this.target = new.target; } }
  class Child extends Wide { constructor(x, y) { super(x, y, 3); this.y = y; } }
  class Leaf extends Child { constructor(x) { super(x, 2); this.leaf = true; } }
  const reference = new Wide(7);
  for (let i = 0; i < N; i++) {
    if (i === N / 2) Object.setPrototypeOf(Child, Other);
    const child = new Child(7, i), leaf = new Leaf(7);
    if (i < N / 2) {
      assert.deepStrictEqual([child.x, child.h, child.count, child.y, leaf.x, leaf.count, leaf.leaf], [7, reference.h, 3, i, 7, 3, true]);
    } else {
      assert.deepStrictEqual([child.x, child.h, child.count, child.y, leaf.x, leaf.count, leaf.leaf], [-7, undefined, undefined, i, -7, undefined, true]);
    }
    assert.strictEqual(child.target, Child);
    assert.strictEqual(leaf.target, Leaf);
  }
}

{
  class Thrower { constructor(x) { if (x === N - 1500) throw new RangeError('parent'); this.x = x; this.pad = [x, x, x].length + (x & 1 ? 1 : 2) + (x > 3 ? 1 : 0); } }
  class Swapped { constructor(x) { return { swapped: x }; } }
  class Child extends Thrower {
    constructor(x, o) {
      let before = 0;
      try { super(x); before = 1; this.y = o.v; } catch (error) { if (!(error instanceof RangeError)) throw error; before = 2; this.caught = true; }
      this.before = before;
    }
  }
  const usual = { v: 1 }, odd = { extra: 0, v: 2 };
  for (let i = 0; i < N; i++) {
    if (i === N - 500) Object.setPrototypeOf(Child, Swapped);
    if (i === N - 1500) {
      assert.throws(() => new Child(i, usual), ReferenceError);
      continue;
    }
    const made = new Child(i, i === N - 2000 || i === N - 100 ? odd : usual);
    if (i < N - 500) assert.deepStrictEqual({ ...made }, { x: i, pad: made.pad, y: i === N - 2000 ? 2 : 1, before: 1 });
    else assert.deepStrictEqual({ ...made }, { swapped: i, y: i === N - 100 ? 2 : 1, before: 1 });
  }
}

{
  class Swaps { constructor(x) { return { swapped: x }; } }
  class Framed extends Swaps { constructor(x, o) { super(x); try { this.y = o.v; } catch (error) { this.y = -1; } } }
  class Keeps { constructor(x) { this.kept = x; if (x < 0) return 1; } }
  class FramedKeeps extends Keeps { constructor(x, o) { super(x); try { this.y = o.v; } catch (error) { this.y = -1; } } }
  class Early extends Swaps { constructor(list, o) { const copy = [...list]; super(copy[0]); this.y = o.v; } }
  const usual = { v: 1 }, odd = { extra: 0, v: 2 }, other = { more: 0, extra: 1, v: 3 };
  const box = [0];
  for (let i = 0; i < N; i++) {
    const o = i < 3 || i === N - 500 ? null : i === N - 2000 ? odd : i === N - 1000 ? other : usual;
    box[0] = i;
    assert.deepStrictEqual({ ...new Early(box, o || usual) }, { swapped: i, y: o ? o.v : 1 });
    assert.deepStrictEqual({ ...new Framed(i, o) }, { swapped: i, y: o ? o.v : -1 });
    assert.deepStrictEqual({ ...new FramedKeeps(i, o) }, { kept: i, y: o ? o.v : -1 });
  }
}

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
  class Base { constructor(a, b) { this.a = a; this.b = b; this.count = arguments.length; this.target = new.target; } }
  class Swaps { constructor(a) { return { swapped: a }; } }
  class Spread extends Base { constructor(...args) { super(...args); this.own = args.length; } }
  class FromList extends Base { constructor(list) { super(...list); this.own = 1; } }
  class Mixed extends Base { constructor(x, o, list) { if (list) super(...list); else super(x, 2); this.y = o.v; } }
  class Leaf extends Spread { constructor(x) { super(x, 'leaf'); this.leaf = true; } }
  const usual = { v: 1 }, odd = { extra: 0, v: 2 };
  const pair = [0, 'p'];
  function* counted() { yield 'g1'; yield 'g2'; yield 'g3'; }
  for (let i = 0; i < N; i++) {
    if (i === N / 2) { Object.setPrototypeOf(Mixed, Swaps); Object.setPrototypeOf(FromList, Swaps); }
    const o = i === N / 4 || i === N - 1000 ? odd : usual;
    pair[0] = i;
    const spread = new Spread(i, 'b', 'c'), list = new FromList(pair), leaf = new Leaf(i);
    const direct = new Mixed(i, o, null), applied = new Mixed(i, o, pair);
    assert.deepStrictEqual([spread.a, spread.b, spread.count, spread.target, spread.own], [i, 'b', 3, Spread, 3]);
    assert.deepStrictEqual([leaf.a, leaf.b, leaf.count, leaf.target, leaf.own, leaf.leaf], [i, 'leaf', 2, Leaf, 2, true]);
    if (i < N / 2) {
      assert.deepStrictEqual([list.a, list.b, list.count, list.target, list.own], [i, 'p', 2, FromList, 1]);
      assert.deepStrictEqual([direct.a, direct.b, direct.count, direct.target, direct.y], [i, 2, 2, Mixed, o.v]);
      assert.deepStrictEqual([applied.a, applied.b, applied.count, applied.target, applied.y], [i, 'p', 2, Mixed, o.v]);
    } else {
      assert.deepStrictEqual([{ ...list }, { ...direct }, { ...applied }], [{ swapped: i, own: 1 }, { swapped: i, y: o.v }, { swapped: i, y: o.v }]);
    }
  }
  assert.deepStrictEqual([new Spread().count, new Spread(...counted()).b, new Spread(...'xy').a], [0, 'g2', 'x']);
  const many = Array.from({ length: 40 }, (_, k) => k);
  assert.deepStrictEqual([new Spread(...many).count, new Spread(...many).own, new Spread(...[, 1]).a], [40, 40, undefined]);
  Object.setPrototypeOf(FromList, Base);
  assert.deepStrictEqual([new FromList([]).count, new FromList('ab').b, new FromList(many).count, new FromList(counted()).a], [0, 'b', 40, 'g1']);
  assert.throws(() => new FromList(5), TypeError);
  assert.throws(() => new FromList(undefined), TypeError);
  Object.setPrototypeOf(FromList, null);
  assert.throws(() => new FromList([1]), TypeError);
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
  class WithFields { a = 1; b = this.a + 1; constructor(x) { this.x = x; } }
  class Plain { constructor(x) { this.x = x + 100; } }
  class Child extends WithFields { c = 3; constructor(x) { super(x); this.y = this.b + this.c; } }
  class Implicit extends WithFields { d = 4; }
  for (let i = 0; i < N; i++) {
    if (i === N / 2) { Object.setPrototypeOf(Child, Plain); Object.setPrototypeOf(Implicit, Plain); }
    const child = new Child(i), implicit = new Implicit(i);
    if (i < N / 2) assert.deepStrictEqual([{ ...child }, { ...implicit }], [{ a: 1, b: 2, x: i, c: 3, y: 5 }, { a: 1, b: 2, x: i, d: 4 }]);
    else assert.deepStrictEqual([{ ...child, y: 0 }, Number.isNaN(child.y), { ...implicit }], [{ x: i + 100, c: 3, y: 0 }, true, { x: i + 100, d: 4 }]);
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
