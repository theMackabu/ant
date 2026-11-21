const assert = require('node:assert');

const HOT = 600;

function test(name, body) {
  try {
    body();
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

function build(make) {
  const loop = new Function('make', 'count', 'let last; for (let i = 0; i < count; i++) last = make(i); return last;');
  return loop(make, HOT);
}

test('bound function given a prototype by hand', () => {
  function P(a, b) { this.a = a; this.b = b; this.target = new.target; }
  const BP = P.bind({ ignored: true });
  assert.throws(() => { class X extends BP {} }, TypeError);
  BP.prototype = P.prototype;
  class D extends BP { constructor(a, b) { super(a, b); this.d = 1; } }
  const d = build((i) => new D(i, i + 1));
  assert.deepStrictEqual({ ...d, target: undefined }, { a: HOT - 1, b: HOT, target: undefined, d: 1 });
  assert.strictEqual(d.target, D);
  assert.strictEqual(Object.getPrototypeOf(d), D.prototype);
  assert.strictEqual(d instanceof P, true);
  assert.strictEqual(d.ignored, undefined);
});

test('bound function installed with setPrototypeOf', () => {
  function P(a) { this.a = a; this.target = new.target; }
  function Q(a) { this.q = a; }
  class D extends Q { constructor(a) { super(a); this.d = 1; } }
  Object.setPrototypeOf(D, P.bind(null));
  const d = build((i) => new D(i));
  assert.strictEqual(d.a, HOT - 1);
  assert.strictEqual(d.q, undefined);
  assert.strictEqual(d.target, D);
  assert.strictEqual(d instanceof Q, true);
  assert.strictEqual(d instanceof P, false);
  assert.deepStrictEqual(Object.keys(d), ['a', 'target', 'd']);
});

test('bound arguments come first', () => {
  function P(...all) { this.all = all; this.count = arguments.length; }
  class D extends Object { constructor(x, y) { super(x, y); this.d = 1; } }
  Object.setPrototypeOf(D, P.bind(null, 'b0', 'b1'));
  const d = build((i) => new D(i, -i));
  assert.deepStrictEqual(d.all, ['b0', 'b1', HOT - 1, -(HOT - 1)]);
  assert.strictEqual(d.count, 4);
  const one = build((i) => new D(i));
  assert.deepStrictEqual(one.all, ['b0', 'b1', HOT - 1, undefined]);

  function Named(a, b, c) { this.a = a; this.b = b; this.c = c; }
  class E extends Object { constructor(v) { super(v); } }
  const held = { tag: 'held' };
  Object.setPrototypeOf(E, Named.bind(null, held, 2.5));
  const e = build((i) => new E(i));
  assert.strictEqual(e.a, held);
  assert.strictEqual(e.b, 2.5);
  assert.strictEqual(e.c, HOT - 1);
});

test('bound of bound', () => {
  function P(a, b, c, d) { this.args = [a, b, c, d]; this.target = new.target; }
  const once = P.bind({ first: 1 }, 'x');
  const twice = once.bind({ second: 2 }, 'y');
  const thrice = twice.bind(null);
  class D extends Object { constructor(v, w) { super(v, w); } }
  Object.setPrototypeOf(D, thrice);
  const d = build((i) => new D(i, i * 2));
  assert.deepStrictEqual(d.args, ['x', 'y', HOT - 1, (HOT - 1) * 2]);
  assert.strictEqual(d.target, D);
  assert.strictEqual(d.first, undefined);
  assert.strictEqual(d.second, undefined);
});

test('more bound arguments than a call site holds', () => {
  function P() { this.count = arguments.length; this.last = arguments[arguments.length - 1]; this.first = arguments[0]; }
  const many = [];
  for (let i = 0; i < 40; i++) many.push(`m${i}`);
  class D extends Object { constructor(v) { super(v); } }
  Object.setPrototypeOf(D, P.bind(null, ...many));
  const d = build((i) => new D(i));
  assert.strictEqual(d.count, 41);
  assert.strictEqual(d.first, 'm0');
  assert.strictEqual(d.last, HOT - 1);

  const some = many.slice(0, 14);
  class E extends Object { constructor(a, b, c) { super(a, b, c); } }
  Object.setPrototypeOf(E, P.bind(null, ...some));
  const e = build((i) => new E(i, i, i + 1));
  assert.strictEqual(e.count, 17);
  assert.strictEqual(e.first, 'm0');
  assert.strictEqual(e.last, HOT);

  function Small(...all) { this.count = all.length; this.first = all[0]; this.last = all[all.length - 1]; }
  class F extends Object { constructor(a, b, c) { super(a, b, c); } }
  Object.setPrototypeOf(F, Small.bind(null, ...some));
  const f = build((i) => new F(i, i, i + 1));
  assert.strictEqual(f.count, 17);
  assert.strictEqual(f.first, 'm0');
  assert.strictEqual(f.last, HOT);

  function Plain(a) { this.a = a; this.count = arguments.length; }
  class G extends Object { constructor(a, b, c) { super(a, b, c); } }
  Object.setPrototypeOf(G, Plain.bind(null, ...many.slice(0, 12)));
  const g = build((i) => new G(i, i, i + 1));
  assert.strictEqual(g.count, 15);
  assert.strictEqual(g.a, 'm0');
  class H extends Object { constructor(a, b, c, d) { super(a, b, c, d); } }
  Object.setPrototypeOf(H, Plain.bind(null, ...many.slice(0, 12)));
  const h = build((i) => new H(i, i, i + 1, i + 2));
  assert.strictEqual(h.count, 16);
  assert.strictEqual(h.a, 'm0');

  function Wide(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p15, p16) { this.a = p0; this.n = p13; this.o = p14; this.q = p16; }
  const wide = Wide.bind(null, ...some);
  wide.prototype = Wide.prototype;
  class M extends wide {}
  const m = build((i) => new M(i, i, i + 1));
  assert.deepStrictEqual({ ...m }, { a: 'm0', n: 'm13', o: HOT - 1, q: HOT });
  assert.strictEqual(Object.getPrototypeOf(m), M.prototype);
  class K extends Object { constructor(a, b, c) { super(a, b, c); } }
  Object.setPrototypeOf(K, wide);
  const k = build((i) => new K(i, i, i + 1));
  assert.deepStrictEqual({ ...k }, { a: 'm0', n: 'm13', o: HOT - 1, q: HOT });
});

test('bound class parents', () => {
  class Root { constructor(r) { this.r = r; } }
  class P extends Root { constructor(a, b) { super(a); this.b = b; this.target = new.target; } }
  class D extends Object { constructor(v) { super(v); this.d = 1; } }
  Object.setPrototypeOf(D, P.bind(null, 'bound'));
  const d = build((i) => new D(i));
  assert.strictEqual(d.r, 'bound');
  assert.strictEqual(d.b, HOT - 1);
  assert.strictEqual(d.target, D);
  assert.deepStrictEqual(Object.keys(d), ['r', 'b', 'target', 'd']);

  class Plain {}
  class F extends Object { constructor() { super(); this.f = 1; } }
  Object.setPrototypeOf(F, Plain.bind(null));
  const f = build(() => new F());
  assert.deepStrictEqual(Object.keys(f), ['f']);
  assert.strictEqual(Object.getPrototypeOf(f), F.prototype);
});

test('parent that names itself', () => {
  const P = function Self(a) { this.self = Self; this.a = a; };
  class D extends Object { constructor(v) { super(v); } }
  Object.setPrototypeOf(D, P.bind(null));
  const d = build((i) => new D(i));
  assert.strictEqual(d.self, P);
  assert.strictEqual(d.a, HOT - 1);
});

test('parent that closes over state', () => {
  let calls = 0;
  const seen = [];
  function make(tag) {
    return function P(a) { calls++; this.tag = tag; if (seen.length < 3) seen.push(a); };
  }
  class D extends Object { constructor(v) { super(v); } }
  Object.setPrototypeOf(D, make('first').bind(null));
  const d = build((i) => new D(i));
  assert.strictEqual(d.tag, 'first');
  assert.strictEqual(calls, HOT);
  assert.deepStrictEqual(seen, [0, 1, 2]);
  Object.setPrototypeOf(D, make('second').bind(null));
  assert.strictEqual(new D(1).tag, 'second');
  assert.strictEqual(calls, HOT + 1);
});

test('parent that returns its own object', () => {
  const made = [];
  function P(a) { const o = { made: a }; made.push(o); return o; }
  class D extends Object { constructor(v) { super(v); this.d = v; } }
  Object.setPrototypeOf(D, P.bind(null));
  const d = build((i) => new D(i));
  assert.strictEqual(d, made[HOT - 1]);
  assert.deepStrictEqual(d, { made: HOT - 1, d: HOT - 1 });
  assert.strictEqual(d instanceof D, false);

  function Prim(a) { this.a = a; return 5; }
  class E extends Object { constructor(v) { super(v); } }
  Object.setPrototypeOf(E, Prim.bind(null));
  const e = build((i) => new E(i));
  assert.strictEqual(e.a, HOT - 1);
  assert.strictEqual(e instanceof E, true);
});

test('parent that throws', () => {
  function P(a) { if (a === 'boom') throw new RangeError('boom'); this.a = a; }
  class D extends Object { constructor(v) { super(v); this.d = 1; } }
  Object.setPrototypeOf(D, P.bind(null));
  build((i) => new D(i));
  assert.throws(() => new D('boom'), RangeError);
  assert.strictEqual(new D(3).a, 3);
});

test('retargeting the parent after warm-up', () => {
  function P(a) { this.from = 'P'; this.a = a; }
  function Q(a, b) { this.from = 'Q'; this.a = a; this.b = b; }
  class D extends Object { constructor(v) { super(v); this.d = 1; } }
  const boundP = P.bind(null);
  Object.setPrototypeOf(D, boundP);
  assert.strictEqual(build((i) => new D(i)).from, 'P');
  Object.setPrototypeOf(D, Q.bind(null, 'q'));
  const q = build((i) => new D(i));
  assert.deepStrictEqual({ ...q }, { from: 'Q', a: 'q', b: HOT - 1, d: 1 });
  Object.setPrototypeOf(D, Q);
  const direct = build((i) => new D(i));
  assert.deepStrictEqual({ ...direct }, { from: 'Q', a: HOT - 1, b: undefined, d: 1 });
  Object.setPrototypeOf(D, boundP);
  assert.strictEqual(new D(7).from, 'P');
  Object.setPrototypeOf(D, P.bind(null, 'other'));
  assert.strictEqual(new D(7).a, 'other');
  Object.setPrototypeOf(D, (() => {}).bind(null));
  assert.throws(() => new D(1), TypeError);
  Object.setPrototypeOf(D, null);
  assert.throws(() => new D(1), TypeError);
  Object.setPrototypeOf(D, boundP);
  assert.strictEqual(new D(8).a, 8);
});

test('default derived constructors in between', () => {
  function P(a, b) { this.a = a; this.b = b; this.target = new.target; }
  const BP = P.bind(null, 'bound');
  BP.prototype = P.prototype;
  class M extends BP {}
  class D extends M { constructor(v) { super(v); this.d = 1; } }
  const m = build((i) => new M(i));
  assert.deepStrictEqual({ ...m, target: null }, { a: 'bound', b: HOT - 1, target: null });
  assert.strictEqual(m.target, M);
  assert.strictEqual(Object.getPrototypeOf(m), M.prototype);
  const d = build((i) => new D(i));
  assert.deepStrictEqual({ ...d, target: null }, { a: 'bound', b: HOT - 1, target: null, d: 1 });
  assert.strictEqual(d.target, D);
  assert.strictEqual(d instanceof P, true);
  Object.setPrototypeOf(M, P.bind(null, 'again', 'twice'));
  const again = new D(1);
  assert.strictEqual(again.a, 'again');
  assert.strictEqual(again.b, 'twice');
});

test('classes from one factory with bound parents', () => {
  function P(a, b) { this.a = a; this.b = b; }
  function make(parent) {
    class D extends Object { constructor(v) { super(v); this.d = 1; } }
    Object.setPrototypeOf(D, parent);
    return D;
  }
  const kinds = [make(P.bind(null, 'one')), make(P.bind(null, 'two')), make(P)];
  for (let i = 0; i < HOT; i++) {
    assert.strictEqual(new kinds[0](i).a, 'one');
    assert.strictEqual(new kinds[1](i).a, 'two');
    const plain = new kinds[2](i);
    assert.strictEqual(plain.a, i);
    assert.strictEqual(plain.b, undefined);
  }
});

test('bound builtin parent', () => {
  class D extends Object { constructor(n) { super(n); this.d = 1; } }
  Object.setPrototypeOf(D, Array.bind(null));
  Object.setPrototypeOf(D.prototype, Array.prototype);
  const d = build(() => new D(3));
  assert.strictEqual(Array.isArray(d), true);
  assert.strictEqual(d.length, 3);
  assert.strictEqual(d.d, 1);
  assert.strictEqual(Object.getPrototypeOf(d), D.prototype);
});

console.log('ok');
