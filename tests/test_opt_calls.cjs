// Calls on the optimizing tier: direct calls of the function a site's feedback saw, calls through the runtime for
// any other callee, constructs, and small callees inlined with virtual frames. Exits inside an inlined callee resume
// its frame in the interpreter, then each caller after its call. Every mode prints what the interpreter prints:
// the default, ANT_DEBUG=opt:verify, opt:force and opt:off.
const assert = require('node:assert');

const warm = 3000;

function add(a, b) {
  return a + b;
}

function sumAdd(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = add(s, i);
  return s;
}

function Point(x, y) {
  this.x = x;
  this.y = y;
}
Point.prototype.norm1 = function () {
  return this.x + this.y;
};
Point.prototype.scaled = function (k) {
  return this.x * k + this.y;
};

function level3(p) {
  return p.norm1() + 1;
}
function level2(p) {
  const r = level3(p);
  return r * 2;
}
function level1(p) {
  const r = level2(p);
  return r - 3;
}

function Square(s) { this.s = s; }
Square.prototype.area = function () { return this.s * this.s; };
function Rect(w, h) { this.w = w; this.h = h; }
Rect.prototype.area = function () { return this.w * this.h; };
function Circle(r) { this.r = r; }
Circle.prototype.area = function () { return 3 * this.r * this.r; };
function Tri(b, h) { this.w = b; this.h = h; }
Tri.prototype.area = function () { return (this.w * this.h) >> 1; };

function totalArea(shapes) {
  let s = 0;
  for (let i = 0; i < shapes.length; i++) s += shapes[i].area();
  return s;
}

const handlers = [];
for (let k = 0; k < 12; k++) handlers.push(new Function('x', `return x + ${k};`));
function callEach(fs, x) {
  let s = 0;
  for (let i = 0; i < fs.length; i++) s += fs[i](x);
  return s;
}

function builtins(a, b, s, arr) {
  arr.push(Math.max(a, b));
  return Math.floor(a / 3) + Math.abs(b) + s.indexOf('c') + String(a).length + arr.length;
}

function thrower(x) {
  if (x === 7777) throw new Error('thrown at ' + x);
  return x & 255;
}
function viaThrower(x) {
  const r = thrower(x);
  return r + 1;
}
function callsThrower(x) {
  const r = viaThrower(x);
  return r * 2;
}

function fib(n) {
  return n < 2 ? n : fib(n - 1) + fib(n - 2);
}
function depth(n) {
  return n === 0 ? 0 : 1 + depth(n - 1);
}
function countdown(n, acc) {
  if (n === 0) return acc;
  return countdown(n - 1, acc + 1);
}
function evenTail(n) {
  return n === 0 ? true : oddTail(n - 1);
}
function oddTail(n) {
  return n === 0 ? false : evenTail(n - 1);
}
function maxDepth(f) {
  let reached = 0;
  for (let n = 1000; n < 400000; n = Math.floor(n * 1.5)) {
    try {
      f(n);
      reached = n;
    } catch (e) {
      if (!(e instanceof RangeError)) throw e;
      break;
    }
  }
  return reached;
}

function Account(owner, balance) {
  this.owner = owner;
  this.balance = balance;
  this.open = true;
}
function Ledger(owner) {
  this.owner = owner;
  this.history = [];
}
function Wrapped(v) {
  this.v = v;
  if (v < 0) return { wrapped: -v };
}
function Prim(v) {
  this.v = v;
  return 42;
}
class Vec {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
  dot(o) {
    return this.x * o.x + this.y * o.y;
  }
}
function makeAll(i) {
  const a = new Account(i & 3, i);
  const p = new Prim(i);
  const v = new Vec(i, 2);
  return a.balance + a.owner + p.v + v.dot(new Vec(1, i));
}
function makeOthers(i) {
  const w = new Wrapped(i - 10);
  const l = new Ledger('l' + i);
  return (w.v === undefined ? w.wrapped : w.v) + l.history.length;
}

function sloppyThis() {
  return this === globalThis ? 1 : 0;
}
function strictThis() {
  'use strict';
  return this === undefined ? 2 : 0;
}
const holder = {
  base: 10,
  get(k) { return this.base + k; },
  arrow(k) { const f = (d) => this.base + d + k; return f(1); },
};
Number.prototype.twice = function () {
  return this * 2;
};
function thisKinds(i) {
  return sloppyThis() + strictThis() + holder.get(i) + holder.arrow(i);
}
function primitiveThis(i) {
  return (i).twice();
}

function makeCounter(start) {
  let c = start;
  return {
    next() { c = c + 1; return c; },
    peek() { return c; },
  };
}
function useCounter(counter, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += counter.next() + counter.peek();
  return s;
}
function captured(n) {
  let shared = 0;
  const bump = () => { shared += 2; return shared; };
  let s = 0;
  for (let i = 0; i < n; i++) s += bump() + shared;
  return s;
}

function Junk(k, i) {
  this.k = k;
  this.i = i;
}
Junk.prototype.sum = function () {
  return this.k + this.i;
};
function allocate(i) {
  let s = 0;
  for (let k = 0; k < 40; k++) {
    const j = new Junk(k, i);
    s += j.sum();
  }
  return s;
}
function gcAcross(keep, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += allocate(i) + keep.a.length + keep.b;
  return s + keep.a[2];
}

function fewer(a, b, c) {
  return (a | 0) + (b === undefined ? 100 : b) + (c === undefined ? 1000 : c);
}
function more(a) {
  return a + arguments.length;
}
function arity(i) {
  return fewer(i) + fewer(i, 1) + fewer(i, 1, 2) + more(i, 1, 2, 3);
}

const bound = add.bind(null, 1000);
function callBound(i) {
  return bound(i);
}

function mathOps(a, b) {
  return Math.floor(a / b) + Math.ceil(a / 3) + Math.sqrt(a) + Math.abs(b - a);
}
function floorAll(xs) {
  let s = 0;
  for (let i = 0; i < xs.length; i++) s += Math.floor(xs[i]);
  return s;
}

function shapeKind(x) {
  return x instanceof Vec ? 1 : x instanceof Account ? 2 : x instanceof Object ? 3 : 0;
}
function mod(a, b) {
  return a % b;
}
function rethrow(x) {
  if (x > 9000) throw x;
  if (x < -9000) throw new RangeError('low ' + x);
  return x;
}
function throwsThrough(x) {
  const r = rethrow(x);
  return r + 1;
}
function codes(s, n) {
  let c = 0;
  for (let i = 0; i < n; i++) c += s.charCodeAt(i % s.length);
  return c;
}
function methods(v, w) {
  return v.dot(w) + holder.get(1);
}

function show(label, value) {
  console.log(label, JSON.stringify(value));
}

function frames(e) {
  return e.stack.split('\n').map(line => line.trim()).filter(line => line.startsWith('at ')).slice(0, 4).map(line => line.split(' ')[1]);
}

function runCases() {
  for (let k = 0; k < 50; k++) sumAdd(100);
  show('direct', sumAdd(10000));

  let s = 0;
  for (let k = 0; k < warm; k++) s += level1(new Point(k, 1));
  show('inlined', s);
  const reordered = { y: 5, x: 7 };
  Object.setPrototypeOf(reordered, Point.prototype);
  show('shape change at depth 3', level1(reordered));
  show('overflow at depth 3', level1(new Point(2147483647, 1)));
  show('strings at depth 3', level1(new Point('a', 'b')));
  for (let k = 0; k < warm; k++) s += level1(new Point(k, 2));
  show('after exits', s);
  Point.prototype.norm1 = function () { return this.x - this.y; };
  show('replaced method', level1(new Point(10, 3)));
  for (let k = 0; k < warm; k++) s += level1(new Point(k, 3));
  show('after replacement', s);
  show('scaled', new Point(3, 4).scaled(5));

  const shapes = [new Square(2), new Rect(2, 3), new Circle(1), new Tri(4, 3)];
  let area = 0;
  for (let k = 0; k < warm; k++) area += totalArea(shapes);
  show('polymorphic', area);
  show('polymorphic odd', totalArea([new Square(3), { area() { return 1000; } }]));

  let mega = 0;
  for (let k = 0; k < 500; k++) mega += callEach(handlers, k);
  show('megamorphic', mega);

  const arr = [];
  let b = 0;
  for (let k = 0; k < warm; k++) b += builtins(k, -k, 'abcabc', arr);
  show('builtins', [b, arr.length, arr[arr.length - 1]]);

  let t = 0;
  for (let k = 0; k < warm; k++) t += callsThrower(k);
  show('before throw', t);
  try {
    callsThrower(7777);
  } catch (e) {
    show('throw', [e.message, frames(e)]);
  }
  show('after throw', callsThrower(12));

  show('fib', fib(22));
  let d = 0;
  for (let k = 0; k < 200; k++) d += depth(200);
  show('recursion', [d, depth(8000)]);
  show('deep recursion', maxDepth(depth) >= 9000);
  let parity = 0;
  for (let k = 0; k < 300; k++) parity += evenTail(k % 7) ? 1 : 0;
  show('tail', [countdown(200000, 0), evenTail(3001), parity]);

  let made = 0;
  for (let k = 0; k < warm; k++) made += makeAll(k);
  let others = 0;
  for (let k = 0; k < warm; k++) others += makeOthers(k);
  show('constructs', [made, others, new Wrapped(-3), new Prim(5), new Vec(1, 2).dot(new Vec(3, 4)), Object.keys(new Account('z', 1))]);
  const late = new Account('late', 7);
  Account.prototype.kind = 'proto';
  show('construct after prototype change', [late.kind, new Account('n', 8).kind, makeAll(5)]);

  let kinds = 0;
  for (let k = 0; k < warm; k++) kinds += thisKinds(k);
  let primitives = 0;
  for (let k = 0; k < warm; k++) primitives += primitiveThis(k);
  show('this', [kinds, thisKinds(1), primitives]);

  const counter = makeCounter(5);
  show('closures', [useCounter(counter, 5000), counter.peek(), captured(5000)]);

  show('gc across calls', gcAcross({ a: [1, 2, 3], b: 4 }, 20000));

  let ar = 0;
  for (let k = 0; k < warm; k++) ar += arity(k);
  show('arity', [ar, arity(1)]);

  let bd = 0;
  for (let k = 0; k < warm; k++) bd += callBound(k);
  show('bound', bd);

  let m = 0;
  for (let k = 1; k < warm; k++) m += mathOps(k, 7);
  const floats = [1.5, -2.5, 3.25, 7.75];
  for (let k = 0; k < warm; k++) m += floorAll(floats);
  show('math', [m, mathOps(-0, 1), Object.is(Math.floor(-0), -0), floorAll([NaN, 1]), floorAll(['7.5', 2]), mathOps('9', '3')]);
  const floor = Math.floor;
  Math.floor = (x) => 100;
  show('math replaced', [floorAll(floats), mathOps(9, 3)]);
  Math.floor = floor;
  show('math restored', floorAll(floats));

  let kinds2 = 0;
  const things = [new Vec(1, 2), new Account(1, 2), {}, 5];
  for (let k = 0; k < warm; k++) kinds2 += shapeKind(things[k & 3]);
  show('instanceof', [kinds2, shapeKind(null), shapeKind(Object.create(Vec.prototype))]);
  try {
    shapeKind.call(null, {}) + ({} instanceof 5);
  } catch (e) {
    show('instanceof error', e.constructor.name);
  }
  let r = 0;
  for (let k = 0; k < warm; k++) r += mod(k, 7) + mod(k * 3, 5);
  show('mod', [r, mod(5, 0), Object.is(mod(-6, 3), -0), mod(-2147483648, -1), mod(7.5, 2), mod(-7, 3)]);

  let thr = 0;
  for (let k = 0; k < warm; k++) thr += throwsThrough(k);
  for (const v of [9001, -9001]) {
    try {
      throwsThrough(v);
    } catch (e) {
      show('thrown value', [thr, typeof e === 'number' ? e : [e.name, e.message, frames(e)]]);
    }
  }
  show('charCodeAt', [codes('hello', 5000), codes('x', 3)]);
  let me = 0;
  for (let k = 0; k < warm; k++) me += methods(new Vec(k, 1), new Vec(2, k));
  show('methods', me);
}

const COMPILED = [
  'sumAdd', 'level1', 'totalArea', 'callEach', 'callsThrower', 'fib', 'evenTail', 'makeAll', 'thisKinds', 'useCounter',
  'gcAcross', 'arity', 'callBound', 'mathOps', 'floorAll', 'shapeKind', 'mod', 'throwsThrough', 'codes', 'methods',
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
      for (const fn of ['sumAdd', 'level1', 'callsThrower', 'makeAll', 'thisKinds', 'arity'])
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} .*inlined=[1-9]`), `${name}: ${fn} must inline calls`);
      assert.match(child.stderr, /reason=opt-exit kind=\w+ inlined_in=level1 depth=[1-3]/, `${name}: an inlined callee of level1 must exit`);
    }
    console.log(`PASS opt calls ${name}`);
  }
}
