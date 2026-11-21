const assert = require('node:assert');

const HOT = 400;

function test(name, body) {
  try {
    body();
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

function makeClasses() {
  class A { constructor() { this.a = 1; } }
  class B extends A {
    constructor(v) { super(); super.x = v; }
    add(v) { super.y = v; return v; }
  }
  return { A, B };
}

function build(B, from) {
  let last;
  for (let i = 0; i < HOT; i++) last = new B(from + i);
  return last;
}

test('constructor store adds an own data property', () => {
  const { A, B } = makeClasses();
  const b = build(B, 0);
  assert.strictEqual(b.x, HOT - 1);
  assert.deepStrictEqual(Object.keys(b), ['a', 'x']);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(b, 'x'), { value: HOT - 1, writable: true, enumerable: true, configurable: true });
  assert.strictEqual(Object.hasOwn(A.prototype, 'x'), false);
  assert.strictEqual(Object.hasOwn(B.prototype, 'x'), false);
  let sum = 0;
  for (let i = 0; i < HOT; i++) sum += new B(i).x;
  assert.strictEqual(sum, HOT * (HOT - 1) / 2);
});

test('several additions in one constructor', () => {
  class A {}
  class B extends A {
    constructor(v) {
      super();
      super.p0 = v; super.p1 = v + 1; super.p2 = v + 2; super.p3 = v + 3; super.p4 = v + 4; super.p5 = v + 5;
      super.p6 = v + 6; super.p7 = v + 7; super.p8 = v + 8; super.p9 = v + 9; super.p10 = v + 10; super.p11 = v + 11;
      super.p12 = v + 12; super.p13 = v + 13;
    }
  }
  for (let i = 0; i < HOT; i++) {
    const b = new B(i * 100);
    const keys = Object.keys(b);
    assert.strictEqual(keys.length, 14);
    for (let k = 0; k < 14; k++) {
      assert.strictEqual(keys[k], `p${k}`);
      assert.strictEqual(b[`p${k}`], i * 100 + k);
    }
  }
});

test('setter added to the parent prototype after warm-up', () => {
  const { A, B } = makeClasses();
  build(B, 0);
  const seen = [];
  Object.defineProperty(A.prototype, 'x', { set(v) { seen.push([this, v]); }, configurable: true });
  const b = new B('s');
  assert.strictEqual(seen.length, 1);
  assert.strictEqual(seen[0][0], b);
  assert.strictEqual(seen[0][1], 's');
  assert.strictEqual(Object.hasOwn(b, 'x'), false);
  delete A.prototype.x;
  assert.strictEqual(new B('again').x, 'again');
});

test('setter added to Object.prototype after warm-up', () => {
  const { B } = makeClasses();
  build(B, 0);
  let hits = 0;
  Object.defineProperty(Object.prototype, 'x', { set(v) { hits = v; }, configurable: true });
  try {
    const b = new B(5);
    assert.strictEqual(hits, 5);
    assert.strictEqual(Object.hasOwn(b, 'x'), false);
  } finally {
    delete Object.prototype.x;
  }
  assert.strictEqual(new B(6).x, 6);
});

test('read-only data property appears on the chain', () => {
  const { A, B } = makeClasses();
  build(B, 0);
  Object.defineProperty(A.prototype, 'x', { value: 'ro', writable: false, configurable: true });
  assert.throws(() => new B(1), TypeError);
  Object.defineProperty(A.prototype, 'x', { value: 'rw', writable: true });
  const b = new B(2);
  assert.strictEqual(b.x, 2);
  assert.strictEqual(Object.hasOwn(b, 'x'), true);
  assert.strictEqual(A.prototype.x, 'rw');
  Object.defineProperty(Object.prototype, 'x', { value: 'deep', writable: false, configurable: true });
  try {
    assert.strictEqual(new B(3).x, 3);
    delete A.prototype.x;
    assert.throws(() => new B(4), TypeError);
  } finally {
    delete Object.prototype.x;
  }
  assert.strictEqual(new B(5).x, 5);
});

test('writable data property on the parent prototype', () => {
  const { A, B } = makeClasses();
  A.prototype.x = 'proto';
  const b = build(B, 0);
  assert.strictEqual(b.x, HOT - 1);
  assert.strictEqual(Object.hasOwn(b, 'x'), true);
  assert.strictEqual(A.prototype.x, 'proto');
  let hits = 0;
  Object.defineProperty(A.prototype, 'x', { get() { return 'g'; }, set(v) { hits++; }, configurable: true });
  const c = new B(1);
  assert.strictEqual(hits, 1);
  assert.strictEqual(Object.hasOwn(c, 'x'), false);
  Object.defineProperty(A.prototype, 'x', { get() { return 'g'; }, set: undefined, configurable: true });
  assert.throws(() => new B(2), TypeError);
});

test('home prototype swapped after warm-up', () => {
  const { A, B } = makeClasses();
  build(B, 0);
  const seen = [];
  const other = { set x(v) { seen.push(v); } };
  Object.setPrototypeOf(B.prototype, other);
  const b = new B('moved');
  assert.deepStrictEqual(seen, ['moved']);
  assert.strictEqual(Object.hasOwn(b, 'x'), false);
  Object.setPrototypeOf(B.prototype, A.prototype);
  assert.strictEqual(new B('back').x, 'back');
  Object.setPrototypeOf(B.prototype, null);
  assert.throws(() => new B(1), TypeError);
});

test('grandparent swapped after warm-up', () => {
  const { A, B } = makeClasses();
  build(B, 0);
  let hits = 0;
  Object.setPrototypeOf(A.prototype, { set x(v) { hits = v; } });
  const b = new B(9);
  assert.strictEqual(hits, 9);
  assert.strictEqual(Object.hasOwn(b, 'x'), false);
});

test('receiver that cannot take the property', () => {
  const { B } = makeClasses();
  const b = new B(0);
  for (let i = 0; i < HOT; i++) assert.strictEqual(b.add.call({}, i), i);
  const closed = Object.preventExtensions({});
  assert.throws(() => b.add.call(closed, 1), TypeError);
  assert.strictEqual(Object.hasOwn(closed, 'y'), false);
  const sealed = Object.seal({});
  assert.throws(() => b.add.call(sealed, 1), TypeError);
  const frozen = Object.freeze({ y: 0 });
  assert.throws(() => b.add.call(frozen, 1), TypeError);
  assert.strictEqual(frozen.y, 0);
  const open = {};
  b.add.call(open, 'open');
  assert.strictEqual(open.y, 'open');
  assert.throws(() => b.add.call(3, 1), TypeError);
  assert.throws(() => b.add.call(null, 1), TypeError);
});

test('subclasses reach the same constructor site', () => {
  const { B } = makeClasses();
  const subs = [];
  for (let i = 0; i < 12; i++) {
    subs.push(class extends B {
      constructor(v) { super(v); this[`own${i}`] = i; }
    });
  }
  for (let round = 0; round < 60; round++) {
    for (let i = 0; i < subs.length; i++) {
      const o = new subs[i](round * 100 + i);
      assert.strictEqual(o.x, round * 100 + i);
      assert.deepStrictEqual(Object.keys(o), ['a', 'x', `own${i}`]);
      assert.strictEqual(Object.getPrototypeOf(o), subs[i].prototype);
    }
  }
  assert.strictEqual(new B('base').x, 'base');
});

test('receivers with and without the property', () => {
  const { B } = makeClasses();
  const b = new B(0);
  for (let i = 0; i < HOT; i++) {
    const fresh = {};
    const held = { y: -1 };
    const other = { q: 1 };
    b.add.call(fresh, i);
    b.add.call(held, i + 1);
    b.add.call(other, i + 2);
    assert.strictEqual(fresh.y, i);
    assert.strictEqual(held.y, i + 1);
    assert.deepStrictEqual(Object.keys(held), ['y']);
    assert.deepStrictEqual(other, { q: 1, y: i + 2 });
  }
});

test('delete then add again', () => {
  const { B } = makeClasses();
  const b = new B(0);
  const o = { k: 1 };
  for (let i = 0; i < HOT; i++) {
    b.add.call(o, i);
    assert.strictEqual(o.y, i);
    assert.strictEqual(delete o.y, true);
    assert.strictEqual('y' in o, false);
  }
  assert.deepStrictEqual(Object.keys(o), ['k']);
});

test('unusual receivers', () => {
  const { B } = makeClasses();
  const b = new B(0);
  for (let i = 0; i < HOT; i++) b.add.call({}, i);
  const bare = Object.create(null);
  b.add.call(bare, 'bare');
  assert.strictEqual(bare.y, 'bare');
  for (let i = 0; i < HOT; i++) {
    const o = Object.create(null);
    b.add.call(o, i);
    assert.strictEqual(o.y, i);
  }
  const list = [1, 2];
  b.add.call(list, 'list');
  assert.strictEqual(list.y, 'list');
  assert.strictEqual(list.length, 2);
  const fn = function () {};
  b.add.call(fn, 'fn');
  assert.strictEqual(fn.y, 'fn');
  const map = new Map();
  b.add.call(map, 'map');
  assert.strictEqual(map.y, 'map');
  assert.strictEqual(map.size, 0);
  const log = [];
  const target = {};
  const proxy = new Proxy(target, {
    getOwnPropertyDescriptor(t, k) { log.push(`gopd:${String(k)}`); return Reflect.getOwnPropertyDescriptor(t, k); },
    defineProperty(t, k, d) { log.push(`define:${String(k)}:${d.enumerable}:${d.writable}:${d.configurable}`); return Reflect.defineProperty(t, k, d); },
  });
  b.add.call(proxy, 'p');
  assert.deepStrictEqual(log, ['gopd:y', 'define:y:true:true:true']);
  assert.strictEqual(target.y, 'p');
});

test('receiver whose own property is an accessor or read-only', () => {
  const { B } = makeClasses();
  const b = new B(0);
  for (let i = 0; i < HOT; i++) b.add.call({}, i);
  let hits = 0;
  const accessor = { get y() { return 'g'; }, set y(v) { hits++; } };
  assert.throws(() => b.add.call(accessor, 1), TypeError);
  assert.strictEqual(hits, 0);
  const fixed = Object.defineProperty({}, 'y', { value: 'ro', writable: false, configurable: true });
  assert.throws(() => b.add.call(fixed, 1), TypeError);
  assert.strictEqual(fixed.y, 'ro');
  const hidden = Object.defineProperty({}, 'y', { value: 'h', writable: true, enumerable: false, configurable: false });
  b.add.call(hidden, 'set');
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(hidden, 'y'), { value: 'set', writable: true, enumerable: false, configurable: false });
});

test('__proto__ goes through the inherited setter', () => {
  class A {}
  class B extends A { link(v) { super.__proto__ = v; } }
  const b = new B();
  const target = { tag: 't' };
  for (let i = 0; i < HOT; i++) {
    const o = {};
    b.link.call(o, target);
    assert.strictEqual(Object.getPrototypeOf(o), target);
    assert.strictEqual(Object.hasOwn(o, '__proto__'), false);
  }
});

test('object literal home', () => {
  const base = {};
  const home = { __proto__: base, add(v) { super.z = v; } };
  for (let i = 0; i < HOT; i++) {
    const o = { n: i };
    home.add.call(o, i);
    assert.deepStrictEqual(o, { n: i, z: i });
  }
  let hits = 0;
  Object.defineProperty(base, 'z', { set(v) { hits = v; }, configurable: true });
  const o = {};
  home.add.call(o, 'late');
  assert.strictEqual(hits, 'late');
  assert.strictEqual(Object.hasOwn(o, 'z'), false);
});

test('added objects stay consistent after later growth', () => {
  const { B } = makeClasses();
  const kept = [];
  for (let i = 0; i < HOT; i++) {
    const b = new B(i);
    b.extra = i * 2;
    b.more = i * 3;
    kept.push(b);
  }
  for (let i = 0; i < HOT; i++) {
    assert.strictEqual(kept[i].a, 1);
    assert.strictEqual(kept[i].x, i);
    assert.strictEqual(kept[i].extra, i * 2);
    assert.strictEqual(kept[i].more, i * 3);
  }
});

test('one site, parents that differ', () => {
  const seen = [];
  function make(parent) {
    return class extends parent { constructor(v) { super(); super.x = v; } };
  }
  class Plain {}
  class Trapped { set x(v) { seen.push(v); } }
  class Fixed {}
  Object.defineProperty(Fixed.prototype, 'x', { value: 'ro', writable: false });
  const plain = make(Plain);
  const trapped = make(Trapped);
  const fixed = make(Fixed);
  for (let i = 0; i < HOT; i++) assert.strictEqual(new plain(i).x, i);
  const t = new trapped('t');
  assert.deepStrictEqual(seen, ['t']);
  assert.strictEqual(Object.hasOwn(t, 'x'), false);
  assert.throws(() => new fixed(1), TypeError);
  for (let i = 0; i < HOT; i++) {
    assert.strictEqual(Object.hasOwn(new plain(i), 'x'), true);
    assert.strictEqual(Object.hasOwn(new trapped(i), 'x'), false);
    assert.throws(() => new fixed(i), TypeError);
  }
  assert.strictEqual(seen.length, HOT + 1);
});

test('one site, parents that differ, unrelated receivers', () => {
  const seen = [];
  function make(parent) {
    return class extends parent { add(v) { super.y = v; } };
  }
  class Plain {}
  class Trapped { set y(v) { seen.push(v); } }
  class Fixed {}
  Object.defineProperty(Fixed.prototype, 'y', { value: 'ro', writable: false });
  const plain = new (make(Plain))();
  const trapped = new (make(Trapped))();
  const fixed = new (make(Fixed))();
  for (let i = 0; i < HOT; i++) {
    const o = { n: i };
    plain.add.call(o, i);
    assert.strictEqual(o.y, i);
  }
  const o = { n: 0 };
  trapped.add.call(o, 't');
  assert.deepStrictEqual(seen, ['t']);
  assert.strictEqual(Object.hasOwn(o, 'y'), false);
  assert.throws(() => fixed.add.call(o, 1), TypeError);
  assert.strictEqual(Object.hasOwn(o, 'y'), false);
  for (let i = 0; i < HOT; i++) {
    const a = { n: i }, b = { n: i }, c = { n: i };
    plain.add.call(a, i);
    trapped.add.call(b, i);
    assert.throws(() => fixed.add.call(c, i), TypeError);
    assert.strictEqual(a.y, i);
    assert.strictEqual(Object.hasOwn(b, 'y'), false);
    assert.strictEqual(Object.hasOwn(c, 'y'), false);
  }
  assert.strictEqual(seen.length, HOT + 1);
});

function reshapingBase(order) {
  class A {}
  Object.defineProperty(A.prototype, 'x', { configurable: true, set(v) {
    Object.defineProperty(A.prototype, 'x', { value: 0, writable: true, enumerable: true, configurable: true });
    if (order === 'attributes first') Object.defineProperty(this, 'n', { enumerable: false, writable: false });
    Object.defineProperty(this, 'x', { value: v, writable: true, enumerable: true, configurable: true });
    if (order === 'key first') Object.defineProperty(this, 'n', { enumerable: false, writable: false });
  }});
  return A;
}

function checkReshaped(B, name) {
  const first = new B(1);
  assert.strictEqual(JSON.stringify(first), '{"a":"A","x":1}', `${name}: first receiver`);
  assert.strictEqual(Object.getOwnPropertyDescriptor(first, 'n').writable, false, `${name}: first receiver`);
  for (let i = 0; i < HOT; i++) {
    const o = new B(i);
    assert.strictEqual(JSON.stringify(o), `{"a":"A","n":"N","x":${i}}`, `${name}: receiver ${i}`);
    assert.deepStrictEqual(
      Object.getOwnPropertyDescriptor(o, 'n'),
      { value: 'N', writable: true, enumerable: true, configurable: true },
      `${name}: receiver ${i}`
    );
  }
}

for (const order of ['attributes first', 'key first']) {
  test(`a setter that reshapes the receiver is not replayed as an add, super store, ${order}`, () => {
    class B extends reshapingBase(order) { constructor(v) { super(); this.a = 'A'; this.n = 'N'; super.x = v; } }
    checkReshaped(B, order);
  });
}

console.log('ok');
