// The optimizing tier speculates on int32 arithmetic
// and monomorphic own-property shapes, hoists checks out of loops and exits to
// the interpreter where a speculation fails. Every case must give the
// interpreter's answer, including the iterations after an exit.
const assert = require('node:assert');

const warm = 300;

function sumFields(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + o.x + o.y;
  return s;
}

function switchShape(a, b, n, at) {
  let p = a, s = 0;
  for (let i = 0; i < n; i++) {
    s = s + p.v;
    if (i === at) p = b;
  }
  return s;
}

function accumulate(o, n) {
  let s = 2147483000;
  for (let i = 0; i < n; i++) s = s + o.step;
  return s;
}

function scaled(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + o.f;
  return s;
}

function counter(o, n) {
  for (let i = 0; i < n; i++) o.count = o.count + 1;
  return o.count;
}

function storeObjects(o, n, v) {
  for (let i = 0; i < n; i++) o.ref = v;
  return o.ref;
}

function mixed(n) {
  let x = 0;
  for (let i = 0; i < n; i++) {
    if (i < 5) x = x + 1;
    else x = x + 0.5;
  }
  return x;
}

function Vec(x, y) { this.x = x; this.y = y; }
Vec.prototype.dot = function dot(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + this.x * this.y;
  return s;
};
Vec.prototype.strictDot = function strictDot(n) {
  'use strict';
  let s = 0;
  for (let i = 0; i < n; i++) s = s - this.x;
  return s;
};

function sloppyThis(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + this.k;
  return s;
}

function missingArgs(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + o.x;
  return s;
}

function compareNull(o, n) {
  let hits = 0;
  for (let i = 0; i < n; i++) if (o.next === null) hits = hits + 1;
  return hits;
}

function scale(a, b) {
  return a * b - a / 4;
}

function onceSum(n) {
  let t = 0;
  for (let j = 0; j < n; j++) t = t + j;
  return t;
}

function nested(outer, inner) {
  let acc = 0;
  for (let i = 0; i < outer; i++) {
    for (let j = 0; j < inner; j++) acc = acc + 1;
  }
  return acc;
}

function negZero(a, b) {
  return a * b;
}

function negate(x, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = -(s + x);
  return s;
}

function runCases() {
  const point = { x: 3, y: 4 };
  for (let k = 0; k < warm; k++) assert.strictEqual(sumFields(point, 1000), 7000);
  assert.strictEqual(sumFields(point, 0), 0);
  console.log('PASS field-add loop');

  const a = { v: 1 }, b = { w: 0, v: 10 };
  for (let k = 0; k < warm; k++) assert.strictEqual(switchShape(a, a, 100, -1), 100);
  assert.strictEqual(switchShape(a, b, 100, 49), 50 + 500);
  assert.strictEqual(switchShape(a, b, 100, 9), 10 + 900);
  console.log('PASS shape change mid-loop');

  assert.strictEqual(sumFields({ y: 1, x: 2 }, 10), 30);
  console.log('PASS hoisted check exit');

  const step = { step: 100 };
  for (let k = 0; k < warm; k++) assert.strictEqual(accumulate(step, 6), 2147483600);
  assert.strictEqual(accumulate(step, 20), 2147485000);
  for (let k = 0; k < warm; k++) assert.strictEqual(accumulate(step, 20), 2147485000);
  console.log('PASS int32 overflow');

  const f = { f: 2 };
  for (let k = 0; k < warm; k++) assert.strictEqual(scaled(f, 10), 20);
  f.f = 0.25;
  assert.strictEqual(scaled(f, 10), 2.5);
  for (let k = 0; k < warm; k++) assert.strictEqual(scaled(f, 10), 2.5);
  f.f = 'a';
  assert.strictEqual(scaled(f, 3), '0aaa');
  console.log('PASS field value type change');

  const c = { count: 0 };
  for (let k = 0; k < warm; k++) counter(c, 10);
  assert.strictEqual(c.count, warm * 10);
  const young = { count: 0 };
  assert.strictEqual(counter(young, 5), 5);
  const holder = { ref: null };
  for (let k = 0; k < warm; k++) storeObjects(holder, 4, { k });
  for (let k = 0; k < 40000; k++) storeObjects({ ref: 0 }, 1, k);
  const fresh = { tag: 'fresh' };
  assert.strictEqual(storeObjects(holder, 3, fresh), fresh);
  for (let k = 0; k < 40000; k++) storeObjects({ ref: 0 }, 1, k);
  assert.strictEqual(holder.ref.tag, 'fresh');
  console.log('PASS field store loop');

  for (let k = 0; k < warm; k++) assert.strictEqual(mixed(4), 4);
  assert.strictEqual(mixed(9), 7);
  for (let k = 0; k < warm; k++) assert.strictEqual(mixed(9), 7);
  console.log('PASS int32 and double phi');

  const v = new Vec(3, 5);
  for (let k = 0; k < warm; k++) {
    assert.strictEqual(v.dot(4), 60);
    assert.strictEqual(v.strictDot(4), -12);
  }
  globalThis.k = 2;
  const thisHost = { k: 7, sloppyThis };
  for (let k = 0; k < warm; k++) assert.strictEqual(thisHost.sloppyThis(3), 21);
  assert.strictEqual(sloppyThis(3), 6);
  assert.strictEqual(sloppyThis.call(5, 2), NaN);
  assert.strictEqual(Vec.prototype.strictDot.call({ x: 2 }, 2), -4);
  console.log('PASS this fields');

  for (let k = 0; k < warm; k++) assert.strictEqual(missingArgs(point, 2), 6);
  assert.strictEqual(missingArgs(point), 0);
  assert.throws(() => missingArgs(undefined, 1), TypeError);
  console.log('PASS missing arguments');

  const node = { next: null };
  for (let k = 0; k < warm; k++) assert.strictEqual(compareNull(node, 3), 3);
  node.next = node;
  assert.strictEqual(compareNull(node, 3), 0);
  console.log('PASS strict null compare');

  for (let k = 0; k < warm; k++) assert.strictEqual(scale(8, 3), 22);
  assert.strictEqual(scale(0.5, 3), 1.375);
  for (let k = 0; k < warm; k++) assert.strictEqual(negZero(3, 4), 12);
  assert.strictEqual(Object.is(negZero(-3, 0), -0), true);
  assert.strictEqual(negZero(65536, 65536), 4294967296);
  for (let k = 0; k < warm; k++) assert.strictEqual(negate(0.5, 3), -0.5);
  assert.ok(Number.isNaN(negate(NaN, 3)));
  assert.ok(Number.isNaN(scale(NaN, 3)));
  assert.ok(Number.isNaN(scale(Infinity, 0)));
  console.log('PASS straight-line arithmetic');

  assert.strictEqual(onceSum(300000), 44999850000);
  assert.strictEqual(nested(20, 25000), 500000);
  console.log('PASS osr entry');
}

const CASES = [
  'field-add loop', 'shape change mid-loop', 'hoisted check exit', 'int32 overflow',
  'field value type change', 'field store loop', 'int32 and double phi', 'this fields',
  'missing arguments', 'strict null compare', 'straight-line arithmetic', 'osr entry',
];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [mode, debug] of modes) {
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: debug, NO_COLOR: '1' },
      timeout: 120000, maxBuffer: 16 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${mode}: ${child.error || child.stdout + child.stderr}`);
    for (const name of CASES) assert.match(child.stdout, new RegExp(`PASS ${name}`), `${mode}: ${name}`);
    if (!tier || mode === 'force' || mode === 'off') {
      console.log(`PASS opt tier ${mode}`);
      continue;
    }

    const compiled = ['sumFields', 'switchShape', 'accumulate', 'scaled', 'counter', 'storeObjects', 'mixed', 'dot', 'strictDot',
      'sloppyThis', 'missingArgs', 'compareNull', 'scale', 'negZero', 'negate', 'onceSum', 'nested'];
    for (const fn of compiled)
      assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${mode}: ${fn} must compile on the opt tier`);
    const exited = ['sumFields', 'switchShape', 'accumulate', 'scaled', 'mixed', 'missingArgs', 'scale', 'negZero', 'onceSum'];
    for (const fn of exited)
      assert.match(child.stderr, new RegExp(`jit: bailout .*func=${fn} .*reason=opt-exit`), `${mode}: ${fn} must exit to the interpreter`);
    assert.match(child.stderr, /func=accumulate op=ADD .*kind=Overflow/, `${mode}: the overflow exits at the add`);
    assert.match(child.stderr, /func=negZero op=MUL .*kind=Overflow/, `${mode}: -0 exits at the multiply`);
    assert.doesNotMatch(child.stderr, /func=(counter|compareNull|dot|storeObjects) .*reason=opt-exit/, `${mode}: stable code never exits`);
    console.log(`PASS opt tier ${mode}`);
  }
}
