const assert = require('node:assert');

const warm = 3000;

function show(name, value) {
  console.log(name + ': ' + JSON.stringify(value));
}

function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}

function fib3(n, a, b) {
  if (n < 2) return n + a - b;
  return fib3(n - 1, a, b) + fib3(n - 2, b, a);
}

function partial(n, a, b) {
  if (n <= 0) return typeof a + ':' + typeof b;
  return n % 3 === 0 ? partial(n - 1) : n % 3 === 1 ? partial(n - 1, a) : partial(n - 1, a, b, n, n);
}

function isEven(n) {
  return n === 0 ? true : isOdd(n - 1);
}
function isOdd(n) {
  return n === 0 ? false : isEven(n - 1);
}

function ack(m, n) {
  if (m === 0) return n + 1;
  if (n === 0) return ack(m - 1, 1);
  return ack(m - 1, ack(m, n - 1));
}

function make(d) {
  return d === 0 ? { l: null, r: null, v: 1 } : { l: make(d - 1), r: make(d - 1), v: d };
}
function walk(t) {
  if (t.l === null) return t.v;
  return walk(t.l) + walk(t.r) + t.v;
}

class Counter {
  constructor(k) {
    this.k = k;
  }
  fib(n) {
    if (n < 2) return n + this.k;
    return this.fib(n - 1) + this.fib(n - 2);
  }
}

const arrowFib = (n) => (n < 2 ? n : arrowFib(n - 1) + arrowFib(n - 2));

function plus(a, b) {
  return a + b + this.k;
}
const bound = plus.bind({ k: 1 }, 10);
function viaBound(n) {
  return n <= 0 ? 0 : bound(n) + viaBound(n - 1);
}

function big(n) {
  if (n === 0) return 2147483647;
  return big(n - 1) + big(n - 1);
}

function mixed(n, x) {
  if (n === 0) return x;
  return mixed(n - 1, n === 7 ? x + 0.5 : x) + 1;
}

function stringy(n, x) {
  if (n === 0) return x + 1;
  return stringy(n - 1, n === 3 ? String(x) : x);
}

function negZero(n, x) {
  if (n === 0) return x;
  return negZero(n - 1, x * -1);
}

function stackAt(n, depth) {
  if (n === depth) return new Error('at ' + n).stack;
  const s = stackAt(n + 1, depth);
  return s;
}

function throwAt(n, depth) {
  if (n === depth) throw new TypeError('deep ' + n);
  const r = throwAt(n + 1, depth);
  return r + 1;
}

function forever(n) {
  return forever(n + 1) + 1;
}

let reached = 0;
function overflow(n) {
  reached = n;
  return overflow(n + 1) + 1;
}

let swapped = null;
function swapping(n) {
  if (n === 0) return 0;
  if (n === 50 && swapped) {
    const other = swapped;
    swapped = null;
    swapping = other;
  }
  return swapping(n - 1) + 1;
}
const originalSwapping = swapping;

function garbage(n) {
  if (n === 0) return [n];
  const a = garbage(n - 1);
  const b = { n, a, s: 'x' + n };
  return [b.n + a.length, b.s.length];
}

function frames(stack) {
  return stack.split('\n').filter((line) => line.includes('stackAt') || line.includes('throwAt')).length;
}

function runCases() {
  Error.stackTraceLimit = 60;
  let s = 0;
  for (let i = 0; i < warm; i++) s += fib(i % 12);
  show('fib', [s, fib(25), fib(30)]);

  let d = 0;
  for (let i = 0; i < warm; i++) d += fib(i % 10 + 0.5);
  show('fib double', [d, fib(20.5), fib(-1), fib(1.5)]);

  s = 0;
  for (let i = 0; i < warm; i++) s += fib3(i % 10, 1, 2);
  show('fib3', [s, fib3(22, 3, 1)]);

  const p = [];
  for (let i = 0; i < warm; i++) p.push(partial(i % 9, 1, 2));
  show('partial', [p.slice(0, 9), partial(20, 'a', 'b')]);

  let c = 0;
  for (let i = 0; i < warm; i++) if (isEven(i % 500)) c++;
  show('evenodd', [c, isEven(1001), isOdd(777)]);

  let a = 0;
  for (let i = 0; i < 300; i++) a += ack(2, i % 40) + ack(1, i % 30);
  show('ack', [a, ack(2, 300), ack(3, 5)]);

  const t = make(10);
  let w = 0;
  for (let i = 0; i < 200; i++) w += walk(t);
  show('tree', [w, walk(make(12))]);

  const k = new Counter(1);
  let m = 0;
  for (let i = 0; i < warm; i++) m += k.fib(i % 10);
  show('method', [m, k.fib(20), new Counter(-1).fib(15)]);

  let r = 0;
  for (let i = 0; i < warm; i++) r += arrowFib(i % 11);
  show('arrow', [r, arrowFib(21)]);

  let b = 0;
  for (let i = 0; i < 2000; i++) b += viaBound(i % 20);
  show('bound', [b, viaBound(30), viaBound(2.5)]);

  let g = 0;
  for (let i = 0; i < 200; i++) g += big(i % 6);
  show('big', [g, big(8), big(3) > 2147483647]);

  let x = 0;
  for (let i = 0; i < warm; i++) x += mixed(i % 6, i);
  show('mixed', [x, mixed(10, 1), mixed(12, 0.25)]);

  let st = '';
  for (let i = 0; i < warm; i++) st = stringy(i % 3, i);
  show('stringy', [st, stringy(5, 4), stringy(2, 9)]);

  let z = 0;
  for (let i = 0; i < warm; i++) z += negZero(i % 4, i);
  show('negzero', [z, Object.is(negZero(3, 0), -0), Object.is(negZero(4, -0), -0), Object.is(negZero(2, 0), 0)]);

  let sa = '';
  for (let i = 0; i < 400; i++) sa = stackAt(0, i % 20);
  show('stack', [frames(sa), frames(stackAt(0, 30)), stackAt(0, 30).split('\n').slice(0, 8)]);

  let caught = 0;
  for (let i = 0; i < 400; i++) {
    try {
      throwAt(0, (i % 15) + 1);
    } catch (e) {
      caught++;
    }
  }
  try {
    throwAt(0, 40);
  } catch (e) {
    show('throw', [caught, e.name, e.message, frames(e.stack), e.stack.split('\n').slice(0, 6)]);
  }

  for (let round = 0; round < 3; round++) {
    try {
      forever(0);
    } catch (e) {
      show('forever ' + round, [e instanceof RangeError, e.message]);
    }
  }
  try {
    overflow(0);
  } catch (e) {
    show('overflow', [e instanceof RangeError, e.message, reached > 1000]);
  }

  let sw = 0;
  for (let i = 0; i < warm; i++) sw += swapping(i % 40);
  swapped = function (n) {
    return n === 0 ? 1000 : swapped === null ? originalSwapping(n - 1) + 2 : 0;
  };
  show('swap', [sw, swapping(80), swapping(10)]);

  let gc = 0;
  for (let i = 0; i < 200; i++) gc += garbage(200 + (i % 50))[0];
  show('garbage', [gc, garbage(300)]);
}

const COMPILED = ['fib', 'fib3', 'isEven', 'ack', 'walk', 'arrowFib', 'viaBound', 'big', 'mixed', 'negZero', 'swapping', 'stackAt', 'throwAt', 'forever'];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 300000, maxBuffer: 16 * 1024 * 1024,
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
    if (tier && (name === 'default' || name === 'verify'))
      for (const fn of COMPILED)
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
    console.log(`PASS opt recursive calls ${name}`);
  }
}
