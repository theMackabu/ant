// Operations the optimizing tier models natively (bitwise operations, truthiness, loose equality, upvalues,
// globals, dense elements, property additions). Each case warms a function up on one kind of input, then feeds it
// the edge cases its speculation must exit for or send down the slow path. Every mode prints what the interpreter
// prints: the default, ANT_DEBUG=opt:verify, opt:force and opt:off.
const assert = require('node:assert');

const warm = 300;

function bits(a, b, out) {
  out[0] = a & b;
  out[1] = a | b;
  out[2] = a ^ b;
  out[3] = a << b;
  out[4] = a >> b;
  out[5] = a >>> b;
  out[6] = ~a;
  out[7] = ~~a;
  return out;
}

function mixLoop(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const x = a[i & 7];
    s = (s ^ (x << 3) ^ (x >>> 2) ^ (x >> 1)) & 0xffffff;
    s = s | (~x & 1);
  }
  return s;
}

function ushr(a, b) {
  return a >>> b;
}

function hashLoop(seed, n) {
  for (let i = 0; i < n; i++) seed = (seed ^ 0xc761c23c ^ (seed >>> 19)) & 0xffffffff;
  return seed;
}

function not(v) {
  return !v;
}

function concat(a, b) {
  return a + b;
}

function names(prefix, n) {
  const out = [];
  for (let i = 0; i < n; i++) out.push(prefix + i + '/' + (i * 0.5));
  return out.join();
}

function countFalsy(a, n) {
  let c = 0;
  for (let i = 0; i < n; i++) if (!a[i & 7]) c++;
  return c;
}

function looseEq(a, b, out) {
  out[0] = a == b;
  out[1] = a != b;
  return out;
}

function isNullish(v, out) {
  out[0] = v == null;
  out[1] = v != null;
  out[2] = null == v;
  out[3] = v == undefined;
  return out;
}

function objEq(a, b, n) {
  let c = 0;
  for (let i = 0; i < n; i++) if (a == b) c++;
  return c;
}

function strictMixed(a, b, out) {
  out[0] = a === b;
  out[1] = a !== b;
  return out;
}

function makeCounter() {
  let count = 0;
  const peek = () => count;
  const bump = (n) => {
    for (let i = 0; i < n; i++) count = count + 1;
    return count;
  };
  return { peek, bump, set: (v) => { count = v; } };
}

function makeHolder() {
  let held = { v: 1 };
  return { get: () => held, swap: (n) => { for (let i = 0; i < n; i++) held = { v: i, prev: held.v }; return held; } };
}

function tdzReader() {
  const read = () => late;
  let early = 0;
  try { early = read(); } catch (e) { early = e.name; }
  let late = 5;
  return [early, read()];
}

function sumGlobal(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + gCounter;
  return s;
}

function sum(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + a[i];
  return s;
}

function readAt(a, i) {
  return a[i];
}

function writeAt(a, i, v) {
  a[i] = v;
  return a.length;
}

function fill(a, n) {
  for (let i = 0; i < n; i++) a[i] = i * 2;
  return a;
}

function fillDown(n) {
  const a = [];
  for (let i = n - 1; i >= 0; i--) a[i] = i;
  return a;
}

function lengthOf(v) {
  return v.length;
}

function Point(x, y) {
  this.x = x;
  this.y = y;
}

function addFields(o, v) {
  o.a = v;
  o.b = v + 1;
  return o;
}

function readField(o) {
  return o.k;
}

function Static() {}
Static.ONE = 1;
Static.TWO = 2;
function readStatics(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + Static.ONE + Static.TWO;
  return s;
}

function show(label, value) {
  console.log(label, typeof value === 'string' ? value : JSON.stringify(value, (k, v) => typeof v === 'bigint' ? `${v}n` : v));
}

function runCases() {
  const out = (n) => new Array(n).fill(0);
  // Plain loops: the template JIT may inline the callee into a callback.
  const each = (f, cases, n) => {
    const results = [];
    for (let i = 0; i < cases.length; i++) {
      const c = cases[i];
      results[i] = n === 1 ? f(c) : f(c[0], c[1], out(n));
    }
    return results;
  };
  for (let k = 0; k < warm; k++) bits(k, 3, out(8));
  show('bits', each(bits, [
    [5, 1], [0x7fffffff, 1], [2147483647 + 10, 0], [-0, 0], [NaN, 1], [Infinity, -Infinity], [1.75, 33], [-5, -1],
    [-1, 32], [2 ** 40 + 5, 2], [-(2 ** 53), 31], [1e21, 7], ['12', 1], [true, 2], [null, undefined], [4294967295, 0],
  ], 8));

  const ints = [1, 2, 3, 4, 5, 6, 7, 0x7fffffff];
  for (let k = 0; k < warm; k++) mixLoop(ints, 50);
  show('mix loop', [mixLoop(ints, 1000), mixLoop([1.5, -2.5, 3e9, -0, NaN, 6, 7, 8], 100)]);

  for (let k = 0; k < warm; k++) ushr(k, 1);
  show('ushr', [ushr(-1, 0), ushr(-8, 1), ushr(2 ** 31, 0), ushr(5, 32), ushr(-1, 31), ushr(-1, 33), ushr(3.9, 0)]);

  for (let k = 0; k < warm; k++) hashLoop(k, 10);
  show('hash loop', [hashLoop(12345, 1000), hashLoop(-1, 1000), hashLoop(0x80000000, 10)]);

  for (let k = 0; k < warm; k++) not(k & 1);
  show('not', [not(0), not(-0), not(NaN), not(1), not(''), not('a'), not(null), not(undefined), not({}), not([]),
    not(Symbol('s')), not(0n), not(2n), not(true), not(false), not(not), not(0.5)]);
  const values = [0, 'x', '', null, {}, 1, NaN, undefined];
  for (let k = 0; k < warm; k++) countFalsy(values, 10);
  show('falsy loop', [countFalsy(values, 1000), countFalsy([true, false, 'q', 2n, 0n, [], -0, Symbol()], 50)]);

  for (let k = 0; k < warm; k++) looseEq(k, k + (k & 1), out(2));
  show('loose numbers', each(looseEq, [[1, 1], [1, '1'], ['1', 1], [0, ''], [NaN, NaN], [0, -0], [null, undefined], [null, 0],
    [true, 1], [{}, '[object Object]']], 2));
  for (let k = 0; k < warm; k++) isNullish(k & 1 ? null : {}, out(4));
  show('nullish', each((v) => isNullish(v, out(4)), [null, undefined, 0, '', false, {}], 1));
  const o1 = { a: 1 }, o2 = { a: 1 };
  for (let k = 0; k < warm; k++) objEq(o1, k & 1 ? o1 : o2, 3);
  const prim = { valueOf() { return 1; } };
  show('object eq', [objEq(o1, o1, 5), objEq(o1, o2, 5), objEq(null, undefined, 5), objEq(o1, null, 5),
    objEq(prim, 1, 5), objEq(true, prim, 5), objEq('a', 'a', 5), objEq(Symbol.iterator, Symbol.iterator, 2)]);
  for (let k = 0; k < warm; k++) strictMixed('ab' + (k & 1), 'ab1', out(2));
  show('strict mixed', each(strictMixed, [['ab1', 'a' + 'b1'], ['x', 'y'], [1, '1'], [NaN, NaN], [o1, o1], [0, -0], [2n, 2n],
    [undefined, null]], 2));

  for (let k = 0; k < warm; k++) concat(k & 1 ? 'v' : k, k & 2 ? 's' + k : k);
  const boxed = { valueOf() { return 7; }, toString() { return 'boxed'; } };
  show('concat', [concat('a', 1), concat(2, 'b'), concat('x', 'y'), concat(1.5, 2), concat(-0, ''), concat('n', NaN),
    concat('', boxed), concat(boxed, 1), concat(null, 'z'), concat(undefined, 1), concat(true, 's'), concat(1n, 'q')]);
  for (let k = 0; k < warm; k++) names('v', 8);
  show('names', [names('src', 12), names(3, 3)]);

  const counter = makeCounter();
  for (let k = 0; k < warm; k++) counter.bump(3);
  counter.set(10);
  show('upvalues', [counter.bump(5), counter.peek(), (counter.set(0.5), counter.bump(1)), counter.peek()]);
  const holder = makeHolder();
  for (let k = 0; k < warm; k++) holder.swap(2);
  show('upvalue objects', [holder.swap(4).prev, holder.get().v]);
  for (let k = 0; k < warm; k++) tdzReader();
  show('tdz', tdzReader());

  globalThis.gCounter = 1;
  for (let k = 0; k < warm; k++) sumGlobal(5);
  globalThis.gExtra = 7;
  const before = sumGlobal(10);
  globalThis.gCounter = 2.5;
  const after = sumGlobal(10);
  Object.defineProperty(globalThis, 'gCounter', { get() { return 3; }, configurable: true });
  show('globals', [before, after, sumGlobal(10)]);

  const dense = [1, 2, 3, 4, 5, 6, 7, 8];
  for (let k = 0; k < warm; k++) sum(dense, 8);
  const holey = [1, , 3];
  Array.prototype[1] = 40;
  const fromProto = sum(holey, 3);
  delete Array.prototype[1];
  show('elements', [sum(dense, 8), sum(dense, 10), sum([1.5, 2.5], 2), sum(holey, 3), fromProto, sum('1234', 4),
    sum(new Float64Array([1, 2, 3]), 3), sum({ 0: 5, 1: 6 }, 2)]);
  for (let k = 0; k < warm; k++) readAt(dense, k & 7);
  show('read at', [readAt(dense, 7), readAt(dense, 8), readAt(dense, -1), readAt(dense, 1.5), readAt(dense, '2'),
    readAt([, 1], 0), readAt('abc', 1), readAt(new Uint8Array([9, 8]), 1)]);
  for (let k = 0; k < warm; k++) writeAt([1, 2, 3], k % 3, k);
  const grow = [1, 2, 3];
  const frozen = Object.freeze([1, 2]);
  const sparse = [];
  show('write at', [writeAt(grow, 3, 4), writeAt(grow, 10, 5), grow, writeAt(frozen, 0, 9), frozen, writeAt(sparse, 5, 1),
    sparse.length, writeAt([1, , 3], 1, 2), writeAt([1, 2], -1, 0)]);
  for (let k = 0; k < warm; k++) fill([], 8);
  show('fill', [fill([], 20), fill([1, 2, 3], 2), fill(new Array(4), 4)]);
  for (let k = 0; k < warm; k++) fillDown(6);
  show('fill down', fillDown(9));
  for (let k = 0; k < warm; k++) lengthOf(dense);
  show('length', [lengthOf(dense), lengthOf([]), lengthOf('hello'), lengthOf({ length: 3 }), (function () { return lengthOf(arguments); })(1, 2)]);

  for (let k = 0; k < warm; k++) new Point(k, k);
  const p = new Point(1, 2);
  let setterHits = 0;
  function Guarded() {}
  Object.defineProperty(Guarded.prototype, 'y', { set(v) { setterHits += v; }, get() { return 'setter'; } });
  const guarded = new Point.prototype.constructor(1, 2);
  Object.setPrototypeOf(guarded, Guarded.prototype);
  show('points', [p.x + p.y, Object.keys(new Point(3, 4)), guarded.y]);
  for (let k = 0; k < warm; k++) addFields({}, k);
  const plain = addFields({}, 1);
  const sealed = addFields(Object.preventExtensions({}), 2);
  const proto = { set b(v) { this.seen = v; } };
  const viaSetter = addFields(Object.create(proto), 3);
  show('adds', [plain, sealed, viaSetter, viaSetter.seen, Object.keys(viaSetter)]);
  Object.defineProperty(Object.prototype, 'b', { set(v) { this.late = v; }, configurable: true });
  const late = addFields({}, 4);
  delete Object.prototype.b;
  show('late setter', [late, late.late]);

  for (let k = 0; k < warm; k++) readField({ k: k });
  const shapes = [{ k: 1 }, { j: 0, k: 2 }, { i: 0, j: 0, k: 3 }, { k: 4, z: 1 }, Object.create({ k: 5 }), [1], { get k() { return 6; } }];
  show('poly field', each(readField, shapes.concat([function f() {}, 'str']), 1));
  for (let k = 0; k < warm; k++) readStatics(3);
  Static.TWO = 20;
  show('statics', readStatics(4));
}

const COMPILED = [
  'bits', 'concat', 'names', 'mixLoop', 'ushr', 'hashLoop', 'not', 'countFalsy', 'looseEq', 'isNullish', 'objEq', 'strictMixed', 'bump', 'sumGlobal',
  'sum', 'readAt', 'writeAt', 'fill', 'lengthOf', 'Point', 'addFields', 'readField', 'readStatics',
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
      for (const [fn, kind] of [['bits', 'NotInt32'], ['ushr', 'Overflow'], ['objEq', 'BadType'], ['concat', 'BadType'], ['readField', 'BadShape'], ['sumGlobal', 'BadShape']])
        assert.match(child.stderr, new RegExp(`jit: bailout .*func=${fn} .*reason=opt-exit kind=${kind}`), `${name}: ${fn} must exit (${kind})`);
    }
    console.log(`PASS opt ops ${name}`);
  }
}
