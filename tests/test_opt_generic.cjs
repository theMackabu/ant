// Functions with instructions the optimizing tier does not model run them through the interpreter one at a
// time (generic nodes) in a real interpreter frame. Every case must print what the interpreter prints, under
// ANT_DEBUG=opt:force (which compiles these functions past the profitability guard), with and without
// opt:verify, and with the tier off.
const assert = require('node:assert');

const warm = 300;

function concat(n) {
  let s = '';
  for (let i = 0; i < n; i++) s = s + 'a' + i;
  return s;
}

function kinds(v, n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    if (typeof v === 'object') c = c + 1;
    if (typeof v === 'string') c = c + 10;
  }
  return c;
}

function membership(o, n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    if ('x' in o) c = c + 1;
    if (o instanceof Array) c = c + 2;
  }
  return c;
}

function literals(n) {
  let last = null;
  for (let i = 0; i < n; i++) last = { i, list: [i, i * 2, 'x'], nested: { k: i } };
  return last.i + last.list[1] + last.nested.k + last.list.length;
}

function elements(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    a[i % a.length] = a[(i + 1) % a.length] + 1;
    s = s + a[i % a.length];
  }
  return s;
}

function globals(n) {
  for (let i = 0; i < n; i++) gTotal = gTotal + i;
  return gTotal;
}

function makeCounter() {
  let c = 0;
  return function bump(n) {
    for (let i = 0; i < n; i++) c = c + 1;
    return c;
  };
}

function captured(o, n) {
  let total = 0;
  const read = () => total;
  for (let i = 0; i < n; i++) total = total + o.v;
  return [total, read];
}

function getterBump(n) {
  let hits = 0;
  const o = { get x() { hits = hits + 1; return hits; } };
  let s = 0;
  for (let i = 0; i < n; i++) s = s + o.x;
  return s * 1000 + hits;
}

function perIteration(n) {
  const fns = [];
  for (let i = 0; i < n; i++) fns[fns.length] = () => i * 3;
  return fns;
}

function deepElement(a, k) {
  let s = 0;
  for (let i = 0; i < 4; i++) s = s + a[k][i];
  return s;
}

function thrower(n) {
  if (n > 5) throw new RangeError('too big ' + n);
  return n + 1;
}

function throwingGetter(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + o.risky;
  return s;
}

function missingGlobal(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + (i < 3 ? i : notDefinedAnywhere);
  return s;
}

function genericThenExit(a, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    const t = typeof a[i];
    if (t === 'number') s = s + a[i];
  }
  return s;
}

function churn(n) {
  let keep = { tag: 1 };
  let acc = 0;
  for (let i = 0; i < n; i++) {
    const tmp = { v: i, w: [i, i + 1, { z: i }] };
    acc = acc + tmp.w[1] - tmp.v + tmp.w[2].z - i + (typeof tmp.w === 'object' ? 0 : 1);
    keep = { tag: keep.tag + 1, prev: [keep.tag] };
  }
  return acc * 1000000 + keep.tag + keep.prev[0];
}

function strictArgs() {
  'use strict';
  let s = 0;
  for (let i = 0; i < arguments.length; i++) s = s + arguments[i];
  return s;
}

function osrClosure(n) {
  let total = 0;
  const read = () => total;
  for (let i = 0; i < n; i++) total = total + (i & 3);
  return [total, read];
}

function osrExit(a, n) {
  let total = 0;
  const read = () => total;
  for (let i = 0; i < n; i++) total = total + a[i & 7];
  return [total, read];
}

function Chain(n) {
  this.depth = n;
  this.next = n > 0 ? new Chain(n - 1) : null;
}

function optional(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = s + (o?.v ?? 1);
  return s;
}

function show(label, value) {
  console.log(label, typeof value === 'string' ? value : JSON.stringify(value));
}

function stackHead(e) {
  return e.stack.split('\n').slice(0, 3).map(line => line.trim().replace(/\(.*[\\/]/, '(')).join(' | ');
}

function runCases() {
  for (let k = 0; k < warm; k++) concat(20);
  show('concat', concat(25));

  for (let k = 0; k < warm; k++) kinds({}, 4);
  show('kinds', [kinds({}, 5), kinds('s', 5), kinds(null, 5), kinds(3, 5)]);

  for (let k = 0; k < warm; k++) membership({ x: 1 }, 3);
  show('membership', [membership({ x: 1 }, 4), membership([], 4), membership(Object.create({ x: 0 }), 4)]);

  for (let k = 0; k < warm; k++) literals(5);
  show('literals', literals(50));

  for (let k = 0; k < warm; k++) elements([1, 2, 3], 6);
  show('elements', [elements([1, 2, 3], 10), elements([0.5, 2, 3, 4], 7), elements(['a', 'b'], 3)]);

  globalThis.gTotal = 0;
  for (let k = 0; k < warm; k++) globals(3);
  show('globals', globals(10));

  const bump = makeCounter();
  for (let k = 0; k < warm; k++) bump(2);
  show('upvalues', bump(5));

  const plain = { v: 2 };
  for (let k = 0; k < warm; k++) {
    const [total, read] = captured(plain, 5);
    assert.strictEqual(read(), total);
  }
  const [t1, r1] = captured({ v: 0.5 }, 9);
  const [t2, r2] = captured(plain, 9);
  show('captured', [t1, r1(), t2, r2()]);

  for (let k = 0; k < warm; k++) getterBump(3);
  show('getter', getterBump(10));

  for (let k = 0; k < warm; k++) perIteration(3);
  show('per-iteration', perIteration(6).map(f => f()));

  const grid = [[1, 2, 3, 4]];
  for (let k = 0; k < warm; k++) deepElement(grid, 0);
  for (const k of [0, 1, 'x']) {
    try { show('deep', deepElement(grid, k)); } catch (e) { show('deep threw', `${e.name}: ${e.message} @ ${stackHead(e)}`); }
  }
  show('deep after', deepElement(grid, 0));

  for (let k = 0; k < warm; k++) {
    try { thrower(k & 7); } catch {}
  }
  for (const n of [2, 7]) {
    try { show('thrower', thrower(n)); } catch (e) { show('thrower threw', `${e.name}: ${e.message} @ ${stackHead(e)}`); }
  }

  let budget = 1e9;
  const risky = { get risky() { if (--budget < 0) throw new Error('getter gave up'); return 1; } };
  for (let k = 0; k < warm; k++) throwingGetter(risky, 3);
  budget = 4;
  try { throwingGetter(risky, 10); } catch (e) { show('getter threw', `${e.message} @ ${stackHead(e)}`); }
  budget = 1e9;
  show('getter after', throwingGetter(risky, 3));

  for (let k = 0; k < warm; k++) missingGlobal(3);
  try { missingGlobal(5); } catch (e) { show('global threw', `${e.name}: ${e.message} @ ${stackHead(e)}`); }

  const nums = [1, 2, 3, 4];
  for (let k = 0; k < warm; k++) genericThenExit(nums, 4);
  show('generic then exit', [genericThenExit([1, 2.5, 'x', 4], 4), genericThenExit(nums, 4)]);
  for (let k = 0; k < warm; k++) genericThenExit(nums, 4);

  for (let k = 0; k < 20; k++) churn(100);
  show('churn', churn(200000));

  for (let k = 0; k < warm; k++) strictArgs(1, 2);
  show('arguments', [strictArgs(), strictArgs(1, 2, 3), strictArgs('a', 'b')]);

  const [ot, or] = osrClosure(300000);
  show('osr closure', [ot, or()]);
  const ints = [1, 2, 3, 4, 5, 6, 7, 8];
  const [et, er] = osrExit(ints, 200000);
  show('osr exit', [et, er()]);
  ints[5] = true;
  const [ft, fr] = osrExit(ints, 200000);
  show('osr exit after', [ft, fr()]);

  for (let k = 0; k < warm; k++) new Chain(3);
  const chainLength = (c) => { let d = 0; while (c) { d++; c = c.next; } return d; };
  show('deep frames', chainLength(new Chain(5000)));
  try { new Chain(1e6); show('overflow', 'none'); } catch (e) { show('overflow', e.name); }
  show('after overflow', chainLength(new Chain(100)));

  for (let k = 0; k < warm; k++) optional({ v: 2 }, 3);
  show('optional', [optional({ v: 2 }, 3), optional(null, 3), optional({}, 3), optional(undefined, 2)]);
}

const COMPILED = [
  'concat', 'kinds', 'membership', 'literals', 'elements', 'globals', 'bump', 'captured', 'getterBump', 'perIteration',
  'deepElement', 'thrower', 'throwingGetter', 'missingGlobal', 'genericThenExit', 'churn', 'strictArgs', 'osrClosure', 'osrExit', 'Chain', 'optional',
];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (debug, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ANT_DEBUG: debug, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
  });
  const interp = run('', ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['force', 'opt:force dump/vm:op-warn'],
    ['force verify', 'opt:force,verify dump/vm:op-warn'],
    ['default', 'dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run(debug);
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && name.startsWith('force')) {
      for (const fn of COMPILED)
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
      assert.match(child.stderr, /jit: bailout .*func=genericThenExit .*reason=opt-exit/, `${name}: genericThenExit must exit after a generic node`);
      assert.match(child.stderr, /jit: bailout .*func=osrExit .*reason=opt-exit/, `${name}: osrExit must exit from its borrowed frame`);
    }
    console.log(`PASS opt generic ${name}`);
  }
}
