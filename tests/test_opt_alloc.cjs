// Allocation on the optimizing tier: object literals (taken inline from the object arena, or from the runtime when
// they do not fit inline), array literals, closures that capture no slots, `this` of inlined constructs, property
// additions (polymorphic too), and the element paths of arrays filled from the back. Every mode prints what the
// interpreter prints: the default, ANT_DEBUG=opt:verify, opt:force and opt:off.
const assert = require('node:assert');

const warm = 3000;

function literal(i) {
  const o = { a: i, b: i * 2, c: 'x', d: [i] };
  return o.a + o.b + o.c.length + o.d[0];
}

function wide(i) {
  const o = { a: i, b: 1, c: 2, d: 3, e: 4, f: i };
  return o.a + o.b + o.c + o.d + o.e + o.f;
}

function junk(n) {
  let s = 0;
  for (let k = 0; k < n; k++) s += { k }.k;
  return s;
}

// A literal promoted by a collection before its last property is stored: that store needs its write barrier.
function promoted(i, n) {
  return { first: { tag: i }, mid: junk(n), last: { tag: -i } };
}

// Values computed between the allocation and the stores into it allocate (and collect) too.
function nested(i) {
  const o = { left: { v: i }, mid: junk(64), right: { v: -i, w: [i, junk(32)] } };
  return o.left.v + o.mid + o.right.v + o.right.w[1];
}

function arrays(i) {
  const a = [i, i + 1, 'z'];
  const e = [];
  const n = [[i], [i, i]];
  return a[0] + a[1] + a.length + e.length + (e[0] === undefined ? 1 : 0) + n[1].length + n[0][0];
}

function makeCounter() {
  let c = 10;
  return function make(i) {
    const f = function named() { return c + i; };
    return f;
  };
}
const make = makeCounter();
function closures(i) {
  const f = make(i);
  return f() + f.name.length;
}

const holder = {
  k: 7,
  arrow(i) {
    const f = () => this.k + i;
    return f();
  },
};
function arrowThis(i) {
  return holder.arrow(i);
}

function Pair(car, cdr) {
  this.car = car;
  this.cdr = cdr;
}
function construct(i) {
  const p = new Pair(i, i + 1);
  return p.car + p.cdr + (p instanceof Pair ? 1 : 0) + (Object.getPrototypeOf(p) === Pair.prototype ? 2 : 0);
}

function Wide(i) {
  this.a = i;
  this.b = 1;
  this.c = 2;
  this.d = 3;
  this.e = 4;
  this.f = i;
}
function constructWide(i) {
  const w = new Wide(i);
  return w.a + w.b + w.c + w.d + w.e + w.f;
}

function Base(s) {
  this.strength = s;
}
function A(v) { Base.call(this, 1); this.v = v; }
function B(v) { Base.call(this, 2); this.w = v; }
function C(v) { Base.call(this, 3); this.u = v; }
A.prototype = Object.create(Base.prototype);
B.prototype = Object.create(Base.prototype);
C.prototype = Object.create(Base.prototype);
function polyAdds(i) {
  const o = i % 3 === 0 ? new A(i) : i % 3 === 1 ? new B(i) : new C(i);
  return o.strength + (Object.hasOwn(o, 'strength') ? 0 : 100);
}

// Young objects stored into old ones across many collections: the stores need their write barriers.
const old = { slots: [] };
for (let k = 0; k < 64; k++) old.slots.push({ tag: -1, next: null });
function churn(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const node = { tag: i, next: { tag: i + 1 } };
    const slot = old.slots[i & 63];
    slot.next = node;
    slot.tag = i;
    s += junk(4);
  }
  for (let k = 0; k < old.slots.length; k++) s += old.slots[k].tag + old.slots[k].next.tag + old.slots[k].next.next.tag;
  return s;
}

function allocThenExit(x) {
  const o = { v: x, w: 1 };
  const p = new Pair(o, x);
  return p.car.v + o.w;
}

function fillBack(n) {
  const a = new Array();
  for (let i = n - 1; i >= 0; i--) a[i] = i;
  let s = 0;
  for (let i = 0; i < n; i++) s += a[i];
  return s + a.length;
}

function readPast(a, i) {
  const v = a[i];
  return v === undefined ? 1000 : v;
}

function kinds(v) {
  return (typeof v === 'number' ? 1 : 0) + (typeof v === 'string' ? 2 : 0) + (typeof v === 'undefined' ? 4 : 0)
    + (typeof v === 'boolean' ? 8 : 0) + (typeof v === 'symbol' ? 16 : 0) + (typeof v === 'bigint' ? 32 : 0);
}

function instance(v) {
  return (v instanceof Pair ? 1 : 0) + (v instanceof Base ? 2 : 0) + (v instanceof A ? 4 : 0) + (v instanceof Object ? 8 : 0);
}

function show(label, value) {
  console.log(label, JSON.stringify(value));
}

function runCases() {
  let s = 0;
  for (let k = 0; k < warm; k++) s += literal(k);
  show('literal', s);

  s = 0;
  for (let k = 0; k < warm; k++) s += wide(k);
  show('wide literal', s);

  s = 0;
  for (let k = 0; k < 2000; k++) s += nested(k);
  show('nested literals', s);

  for (let k = 0; k < warm; k++) promoted(k, 1);
  const kept = [];
  for (let k = 0; k < 4; k++) kept.push(promoted(k, 40000));
  junk(80000);
  show('promoted literals', kept.map(o => o.first.tag * 10 + o.last.tag));

  s = 0;
  for (let k = 0; k < warm; k++) s += arrays(k);
  show('arrays', s);

  s = 0;
  for (let k = 0; k < warm; k++) s += closures(k);
  show('closures', s);

  s = 0;
  for (let k = 0; k < warm; k++) s += arrowThis(k);
  show('arrow this', s);

  s = 0;
  for (let k = 0; k < warm; k++) s += construct(k);
  show('construct', s);
  const before = Pair.prototype;
  Pair.prototype = { kind: 'replaced' };
  s = 0;
  for (let k = 0; k < 500; k++) s += construct(k);
  show('construct after prototype change', [s, new Pair(1, 2) instanceof Pair, Object.getPrototypeOf(new Pair(1, 2)).kind]);
  Pair.prototype = before;

  s = 0;
  for (let k = 0; k < warm; k++) s += constructWide(k);
  show('wide construct', s);

  s = 0;
  for (let k = 0; k < warm * 2; k++) s += polyAdds(k);
  show('polymorphic additions', s);
  let hits = 0;
  Object.defineProperty(B.prototype, 'strength', { set() { hits++; }, get() { return 10; }, configurable: true });
  s = 0;
  for (let k = 0; k < 600; k++) s += polyAdds(k);
  show('additions after a setter', [s, hits]);

  for (let k = 0; k < 20; k++) churn(500);
  show('churn', churn(100000));

  s = 0;
  for (let k = 0; k < warm; k++) s += allocThenExit(k);
  show('alloc then exit', [s, allocThenExit('str'), allocThenExit(2.5), allocThenExit(7)]);

  s = 0;
  for (let k = 0; k < 2000; k++) s += fillBack(40 + (k & 7));
  show('fill from the back', [s, fillBack(100), fillBack(1500)]);

  const dense = [1, 2, , 4];
  const negative = [1, 2, 3];
  negative[-1] = 50;
  s = 0;
  for (let k = 0; k < warm; k++) s += readPast(dense, k & 7);
  show('read past', [s, readPast(negative, -1), readPast(dense, 2), readPast(dense, 9)]);
  Object.prototype[2] = 9;
  show('read past with a prototype element', [readPast(dense, 2), readPast(dense, 3)]);
  delete Object.prototype[2];

  const values = [1, -Infinity, NaN, -0, 'a', 'b'.repeat(40) + 'c', undefined, null, true, Symbol('s'), 10n, {}, [], () => 1];
  s = 0;
  for (let k = 0; k < warm; k++) s += kinds(values[k % values.length]);
  show('typeof', [s, values.map(kinds)]);

  const objects = [new Pair(1, 2), new A(1), new B(2), {}, Object.create(null), 3, 'x', null, undefined, [1]];
  s = 0;
  for (let k = 0; k < warm; k++) s += instance(objects[k % objects.length]);
  show('instanceof', [s, objects.map(instance)]);
}

const COMPILED = [
  'literal', 'wide', 'promoted', 'nested', 'arrays', 'closures', 'arrowThis', 'construct', 'constructWide', 'polyAdds', 'churn',
  'allocThenExit', 'fillBack', 'readPast', 'kinds', 'instance',
];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of COMPILED)
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
      for (const fn of ['construct', 'constructWide', 'polyAdds'])
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} .*inlined=[1-9]`), `${name}: ${fn} must inline its constructs`);
      assert.match(child.stderr, /reason=opt-exit kind=\w+/, `${name}: allocThenExit must exit after allocating`);
    }
    console.log(`PASS opt alloc ${name}`);
  }
}
