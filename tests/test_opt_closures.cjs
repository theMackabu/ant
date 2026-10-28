const assert = require('node:assert');

const warm = 3000;

function show(label, value) {
  console.log(label, JSON.stringify(value));
}

function Pair(car, cdr) {
  this.car = car;
  this.cdr = cdr;
}

function count(l, n) {
  if (l instanceof Pair) return count(l.cdr, n + 1);
  return n;
}

function sumTo(n, acc) {
  if (n === 0) return acc;
  return sumTo(n - 1, acc + n);
}

function fewer(a, b, c) {
  if (a <= 0) return [a, b, c];
  return fewer(a - 1, b);
}

function makeWalker(scale) {
  const walk = function (n, acc) {
    if (n <= 0) return acc;
    return walk(n - 1, acc + scale);
  };
  return walk;
}

function otherClosure(f, n) {
  if (n <= 0) return 'done';
  return f(n - 1, 0);
}

function walkCapture(n, acc, fs) {
  let local = n * 2;
  fs.push(() => local + n);
  local += 1;
  if (n === 0) return acc;
  return walkCapture(n - 1, acc + local, fs);
}

function counter(start) {
  let n = start;
  const inc = () => ++n;
  const get = function () {
    return n;
  };
  inc();
  n += 10;
  return [inc, get];
}

function mutateAfter(x) {
  let v = x;
  const f = () => v * 2;
  v = v + 1;
  const a = f();
  v = 100;
  return a + f();
}

function perIteration(k) {
  const fs = [];
  for (let i = 0; i < k; i++) {
    let j = i * 2;
    fs.push(() => i + j);
    if (i & 1) j = 100;
  }
  let s = 0;
  for (let n = 0; n < fs.length; n++) s += fs[n]();
  return s;
}

function sharedVar(k) {
  var x = 0;
  const fs = [];
  for (var i = 0; i < k; i++) fs.push(() => (x += i));
  for (var n = 0; n < fs.length; n++) fs[n]();
  return x;
}

function captureOnly(n) {
  var table, i, fn;
  table = [n, n + 1];
  i = 0;
  while (i < n) i++;
  fn = function () {
    return table[1];
  };
  return fn() + i;
}

function capturedParam(a, b) {
  const f = () => a + b;
  a = 5;
  return f();
}

function tdzLocal(flag) {
  const g = () => q;
  if (flag) return q;
  let q = 3;
  return g();
}

function escapeThenExit(v) {
  let c = v;
  const set = (z) => {
    c = z;
  };
  const get = () => c;
  const r = v * 2;
  set(r);
  return [get, set, c];
}

function osrCapture(n) {
  let total = 0;
  const add = (v) => {
    total += v;
  };
  for (let i = 0; i < n; i++) add(i);
  return total;
}

function sum() {
  'use strict';
  let s = 0;
  for (let i = 0; i < arguments.length; i++) s += arguments[i];
  return s;
}

function first() {
  'use strict';
  return arguments.length ? arguments[0] : -1;
}

function second() {
  'use strict';
  return arguments[1];
}

function viaSum(a, b) {
  return sum(a, b, 3) + first(a) + first();
}

function escapes() {
  'use strict';
  return arguments;
}

function lengthChanged() {
  'use strict';
  const a = arguments;
  a.length = 5;
  return a.length + (a[4] === undefined ? 10 : 0);
}

function isCallee() {
  return arguments.callee === isCallee;
}

function mapped(x) {
  arguments[0] = 7;
  return x;
}

const Class = {
  create: function () {
    return function () {
      this.initialize.apply(this, arguments);
    };
  },
};

const Vec = Class.create();
Vec.prototype = {
  initialize: function (x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
  },
  sum: function () {
    return this.x + this.y + this.z;
  },
};

function makeVec(i) {
  return new Vec(i, 2, 3);
}

function runCases() {
  let list = null;
  for (let i = 0; i < 100; i++) list = new Pair(i, list);
  let t = 0;
  for (let k = 0; k < warm; k++) t += count(list, 0) + sumTo(50, 0);
  show('tail', [t, sumTo(1000000, 0), sumTo(30, 0.5), sumTo(5, 'a'), count(null, 3), fewer(5, 1, 2)]);

  const w1 = makeWalker(1);
  const w2 = makeWalker(2);
  let wt = 0;
  for (let k = 0; k < warm; k++) wt += w1(20, 0) + w2(20, 0);
  show('tail closures', [wt, w1(300000, 0), w2(10, 0.25), otherClosure(w1, 4)]);

  let ct = 0;
  for (let k = 0; k < warm; k++) {
    const fs = [];
    ct += walkCapture(20, 0, fs);
    for (let i = 0; i < fs.length; i++) ct += fs[i]();
  }
  const fs = [];
  show('tail captures', [ct, walkCapture(200000, 0, fs), fs.length, fs[5](), fs[199999]()]);

  let s = 0;
  for (let i = 0; i < warm * 5; i++) {
    const [inc, get] = counter(i);
    inc();
    s += get() + mutateAfter(i) + perIteration(4) + sharedVar(3) + captureOnly(i & 7) + capturedParam(i, 1) + tdzLocal(false);
  }
  let tdz = 'none';
  try {
    tdzLocal(true);
  } catch (e) {
    tdz = e.constructor.name;
  }
  show('captures', [s, tdz, osrCapture(100000), mutateAfter(1.5), perIteration(7), sharedVar(5), capturedParam('x', 'y')]);

  for (let i = 0; i < warm * 5; i++) escapeThenExit(i);
  const [get, set, c] = escapeThenExit('ab');
  const before = get();
  set(7);
  const [get2, set2, c2] = escapeThenExit(1.25);
  set2('q');
  show('escaping', [before, c, get(), get2(), c2]);

  let a = 0;
  for (let i = 0; i < warm * 10; i++) a += viaSum(i, 1) + escapes(1, 2).length + lengthChanged(1, 2) + (isCallee() ? 1 : 0) + mapped(i);
  show('arguments', [a, sum(1.5, 2), sum('a', 'b'), first('x', 'y'), escapes(4, 5)[1], lengthChanged(), mapped(3)]);
  let b = 0;
  for (let i = 0; i < warm; i++) b += second(i, i + 1) === undefined ? 0 : 1;
  show('arguments index', [b, second(1), second(), second(1, 2, 3)]);

  let v = 0;
  for (let i = 0; i < warm * 10; i++) v += makeVec(i).sum();
  show('forwarding', [v, makeVec(1.5).sum(), new Vec('a', 'b', 'c').sum(), new Vec(1).x]);
  Vec.prototype.initialize = function (x) {
    this.x = x;
    this.y = arguments.length;
    this.z = arguments[2];
  };
  show('forwarding changed', [makeVec(7).sum(), new Vec(1, 2, 'q').z]);
}

const COMPILED = ['count', 'sumTo', 'walk', 'walkCapture', 'counter', 'mutateAfter', 'perIteration', 'sharedVar', 'captureOnly', 'capturedParam',
  'escapeThenExit', 'osrCapture', 'sum', 'first', 'second', 'viaSum', 'makeVec'];

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
      assert.doesNotMatch(child.stderr, /reason=self-tail-call/, `${name}: self tail calls compile as loops`);
      assert.match(child.stderr, /jit: bailout .*func=second .*reason=opt-exit kind=ArgumentsIndex/, `${name}: second must exit (ArgumentsIndex)`);
    }
    console.log(`PASS opt closures ${name}`);
  }
}
