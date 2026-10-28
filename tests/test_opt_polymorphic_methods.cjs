const assert = require('node:assert');

const warm = 4000;

function show(label, value) {
  console.log(label, JSON.stringify(value));
}

function makeClasses() {
  class A { constructor(x) { this.x = x; } val() { return this.x; } get g() { return this.x + 0.5; } }
  class B { constructor(x) { this.y = 1; this.x = x; } val() { return this.x + this.y; } get g() { return this.y - this.x; } }
  class C { constructor(x) { this.z = 2; this.w = 3; this.x = x; } val() { return this.x * 2; } get g() { return this.z * this.x; } }
  const D = { val() { return this.x - 1; } };
  Object.defineProperty(D, 'g', { get() { return -this.x; }, configurable: true });
  const make = i => {
    const k = i & 3;
    return k === 0 ? new A(i) : k === 1 ? new B(i) : k === 2 ? new C(i) : { __proto__: D, x: i };
  };
  return { A, B, C, D, make };
}

function sumVals(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; i++) s += objs[i].val();
  return s;
}

function sumGetters(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; i++) s += objs[i].g;
  return s;
}

function sumVals2(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; i++) s += objs[i].val();
  return s;
}

function sumGetters2(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; i++) s += objs[i].g;
  return s;
}

function callEach(objs, out) {
  for (let i = 0; i < objs.length; i++) out.push(objs[i].val());
  return out.length;
}

function readEach(objs, out) {
  for (let i = 0; i < objs.length; i++) out.push(objs[i].g);
  return out.length;
}

function thisCheck(objs) {
  let n = 0;
  for (let i = 0; i < objs.length; i++) n += objs[i].self() === objs[i] ? 1 : 0;
  return n;
}

function viaGetterMethod(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; i++) s += objs[i].m(i);
  return s;
}

function viaInner(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; i++) s += objs[i].outer();
  return s;
}

function viaSuper(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; i++) s += objs[i].val();
  return s;
}

function manyShapes(objs) {
  let s = 0;
  for (let i = 0; i < objs.length; i++) s += objs[i].val();
  return s;
}

function makeKlass() {
  return function () {
    this.initialize.apply(this, arguments);
  };
}

const K1 = makeKlass(), K2 = makeKlass(), K3 = makeKlass();
K1.prototype.initialize = function (a) { this.a = a; };
K2.prototype.initialize = function (a, b) { this.a = a; this.b = b; };
K3.prototype.initialize = function (a) { this.a = -a; this.c = 1; };
K1.prototype.get = function () { return this.a; };
K2.prototype.get = function () { return this.a + this.b; };
K3.prototype.get = function () { return this.a * this.c; };
const K4 = makeKlass(), K5 = makeKlass();
K4.prototype.initialize = function (a) { this.inner = new K1(a); this.pair = new K2(a, 3); };
K5.prototype.initialize = function (n) { this.next = n > 0 ? new K5(n - 1) : null; this.n = n; };
K5.prototype.depth = function () { return this.next ? 1 + this.next.depth() : 0; };

function constructAll(r) {
  const x = new K1(r), y = new K2(r, 2), z = new K3(r);
  const w = new K4(r), v = new K5(r & 7);
  return x.get() + y.get() + z.get() + y.b + z.c + (y instanceof K1 ? 1000 : 0) + w.inner.get() + w.pair.get() + v.depth() * 7;
}

function guarded(fn, objs) {
  try {
    return fn(objs);
  } catch (e) {
    return `${e.constructor.name}:${e.message}`;
  }
}

function runCases() {
  const { A, B, C, D, make } = makeClasses();
  const objs = [];
  for (let i = 0; i < 64; i++) objs.push(make(i));

  let s = 0;
  for (let r = 0; r < warm; r++) s += sumVals(objs);
  show('methods', s);
  let g = 0;
  for (let r = 0; r < warm; r++) g += sumGetters(objs);
  show('getters', g);

  for (let r = 0; r < warm; r++) sumVals2(objs) + sumGetters2(objs);
  const before = [sumVals2(objs), sumGetters2(objs)];
  B.prototype.val = function () { return -this.x; };
  const protoReplaced = sumVals2(objs);
  objs[5].val = function () { return 1000; };
  objs[6].val = () => 7;
  const instanceReplaced = sumVals2(objs);
  objs[9].extra = 1;
  objs[10].x = 2.5;
  objs[8].x = 'str';
  const shapeChanged = [sumVals2(objs), sumGetters2(objs)];
  Object.defineProperty(C.prototype, 'g', { get() { return 'C' + this.x; }, configurable: true });
  const getterReplaced = sumGetters2(objs);
  Object.defineProperty(A.prototype, 'g', { value: 0.25, configurable: true, writable: true });
  const getterToData = sumGetters2(objs);
  objs[12].g = 99;
  delete D.g;
  const getterDeleted = sumGetters2(objs);
  show('mutations', [before, protoReplaced, instanceReplaced, shapeChanged, getterReplaced, getterToData, getterDeleted]);

  const fresh = [];
  for (let i = 0; i < 48; i++) fresh.push(make(i));
  const out = [];
  for (let r = 0; r < warm / 8; r++) callEach(fresh, out.length > 4096 ? (out.length = 0, out) : out);
  for (let r = 0; r < warm / 8; r++) readEach(fresh, out.length > 4096 ? (out.length = 0, out) : out);
  out.length = 0;
  callEach(fresh, out);
  readEach(fresh, out);
  show('fresh', out.slice(0, 16));

  const { A: A2, B: B2, C: C2, D: D2, make: make2 } = makeClasses();
  const mixed = [];
  for (let i = 0; i < 32; i++) mixed.push(make2(i));
  let t = 0;
  for (let r = 0; r < warm; r++) t += guarded(sumVals, mixed) + guarded(sumGetters, mixed);
  const savedVal = C2.prototype.val;
  delete C2.prototype.val;
  const deletedMethod = guarded(sumVals, mixed);
  C2.prototype.val = savedVal;
  const restored = guarded(sumVals, mixed);
  B2.prototype.val = function () { if (this.x > 20) throw new RangeError('big ' + this.x); return this.x; };
  const throwing = guarded(sumVals, mixed);
  Object.defineProperty(A2.prototype, 'g', { get() { if (this.x > 16) throw new TypeError('getter ' + this.x); return this.x; }, configurable: true });
  const throwingGetter = guarded(sumGetters, mixed);
  const proxied = mixed.slice();
  proxied[3] = new Proxy(mixed[3], {
    get(target, key, receiver) {
      if (key === 'val') return function () { return this === receiver ? 500 : -500; };
      if (key === 'g') return 600;
      return Reflect.get(target, key, receiver);
    },
  });
  proxied[7] = new Proxy({ x: 1 }, { get: (target, key) => key === 'val' ? () => 42 : key === 'g' ? 43 : target[key] });
  B2.prototype.val = function () { return this.x; };
  Object.defineProperty(A2.prototype, 'g', { get() { return this.x; }, configurable: true });
  const proxies = [guarded(sumVals, proxied), guarded(sumGetters, proxied)];
  show('edge cases', [t, deletedMethod, restored, throwing, throwingGetter, proxies]);

  class P { self() { return this; } }
  class Q extends P { constructor() { super(); this.q = 1; } }
  const R = { self() { return this; } };
  const S = { self: function () { 'use strict'; return this; } };
  const selves = [];
  for (let i = 0; i < 40; i++) selves.push(i % 4 === 0 ? new P() : i % 4 === 1 ? new Q() : i % 4 === 2 ? { __proto__: R, r: i } : { __proto__: S, s: i });
  let n = 0;
  for (let r = 0; r < warm; r++) n += thisCheck(selves);
  show('this', n);

  class M1 { constructor(x) { this.x = x; } get m() { return k => this.x + k; } }
  class M2 { constructor(x) { this.y = 0; this.x = x; } get m() { return k => this.x * k; } }
  const ms = [];
  for (let i = 0; i < 32; i++) ms.push(i & 1 ? new M2(i) : new M1(i));
  let mv = 0;
  for (let r = 0; r < warm; r++) mv += viaGetterMethod(ms);
  show('getter methods', mv);

  class Base { outer() { return this.inner() + 1; } val() { return 1; } }
  class I1 extends Base { constructor(x) { super(); this.x = x; } inner() { return this.x; } val() { return super.val() + this.x; } }
  class I2 extends Base { constructor(x) { super(); this.y = 1; this.x = x; } inner() { return this.x + this.y; } val() { return super.val() * 2; } }
  class I3 extends Base { constructor(x) { super(); this.z = 2; this.w = 3; this.x = x; } inner() { return this.x * 2; } }
  class I4 extends I1 { constructor(x) { super(x); this.q = 0; } inner() { return this.x - 1; } val() { return super.val() - 3; } }
  const inners = [];
  for (let i = 0; i < 40; i++) inners.push(new [I1, I2, I3, I4][i & 3](i));
  let iv = 0, sv = 0;
  for (let r = 0; r < warm; r++) {
    iv += viaInner(inners);
    sv += viaSuper(inners);
  }
  Base.prototype.val = function () { return 100; };
  I3.prototype.inner = function () { return -1; };
  show('inner and super', [iv, sv, viaInner(inners), viaSuper(inners)]);

  const classes = [];
  for (let k = 0; k < 12; k++) {
    const c = new Function('k', `return class K${k} { constructor(x) { this.p${k} = k; this.x = x; } val() { return this.x + this.p${k}; } }`)(k);
    classes.push(c);
  }
  const many8 = [], many12 = [];
  for (let i = 0; i < 96; i++) {
    many8.push(new classes[i & 7](i));
    many12.push(new classes[i % 12](i));
  }
  let m8 = 0, m12 = 0;
  for (let r = 0; r < warm / 2; r++) m8 += manyShapes(many8);
  for (let r = 0; r < warm / 2; r++) m12 += manyShapes(many12);
  classes[3].prototype.val = function () { return 3; };
  show('many shapes', [m8, m12, manyShapes(many8), manyShapes(many12)]);

  let kv = 0;
  for (let r = 0; r < warm * 4; r++) kv += constructAll(r);
  K2.prototype.initialize = function (a, b) { this.a = a * 10; this.b = b; };
  const swapped = constructAll(5);
  K2.prototype.initialize = function (a, b) { this.a = a; this.b = b; Object.setPrototypeOf(this, K1.prototype); };
  const reprotoed = constructAll(6);
  K1.prototype.initialize = function (a) { this.a = a; this.extra = 1; };
  delete K3.prototype.get;
  const deleted = guarded(constructAll, 7);
  show('constructors', [kv, swapped, reprotoed, deleted]);
}

const COMPILED = ['sumVals', 'sumGetters', 'sumVals2', 'sumGetters2', 'thisCheck', 'viaGetterMethod', 'viaInner', 'viaSuper', 'manyShapes', 'constructAll'];

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
      for (const fn of ['sumVals', 'sumGetters', 'thisCheck'])
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} .*inlined=[1-9]`), `${name}: ${fn} must inline its polymorphic targets`);
    }
    console.log(`PASS opt polymorphic methods ${name}`);
  }
}
