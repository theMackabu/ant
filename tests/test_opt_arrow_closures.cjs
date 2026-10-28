const assert = require('node:assert');

const warm = 3000;

const add = (a) => (b) => a + b;
const add3 = (a) => (b) => (c) => a * 100 + b * 10 + c;

function curried(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += add(i)(i & 7);
  return s;
}

const nested = (k) => {
  const inner = (j) => (m) => k + j + m;
  return inner(k + 1)(k + 2);
};

function Holder(v) { this.v = v; }
Holder.prototype.make = function make() { return (k) => (j) => this.v + k + j; };
Holder.prototype.loop = function loop(n) {
  const f = () => {
    let s = 0;
    for (let i = 0; i < n; i++) s += this.v;
    return s;
  };
  return f();
};

const arrowThis = function arrowThis() {
  const own = () => this;
  return (() => own())();
};

function sloppyGlobal() { return (() => this === globalThis)(); }
function strictUndefined() { 'use strict'; return (() => this === undefined)(); }
const moduleThis = () => this;

function useSlot(n) {
  let k = 0, s = 0;
  for (let i = 0; i < n; i++) {
    k = i & 15;
    s += add(i)(k);
  }
  return s;
}

function useCaptured(n) {
  let k = 0;
  const inc = (x) => { k += 1; return (y) => x * 1000 + y; };
  let s = 0;
  for (let i = 0; i < n; i++) s += inc(i)(k);
  return s;
}

function callCall(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += add3(i)(i + 1)(i + 2);
  return s;
}

function literals(n) {
  let hits = 0, distinct = 0, prev = null;
  for (let i = 0; i < n; i++) {
    const re = /ab+c/g;
    if (re !== prev) distinct++;
    prev = re;
    if (re.test(i & 1 ? 'xxabbbc' : 'xyz')) hits++;
    if (re.lastIndex !== (i & 1 ? 7 : 0)) return -1;
  }
  return [hits, distinct];
}

function flags(i) { return /x/gi.flags + /y/m.flags + (i & 1); }

function strings(n) {
  let out = '';
  const o = { toString() { return 'o'; } };
  for (let i = 0; i < n; i++) out = `${i & 3}${i & 1 ? o : 'e'}${i % 5 === 0 ? 1.5 : ''}`;
  return out;
}

function throwsSymbol(v) { return `${v}`; }

function voids(n) {
  let side = 0, last;
  const bump = () => { side++; return side; };
  for (let i = 0; i < n; i++) last = void bump();
  return [side, last];
}

function show(label, value) { console.log(label, JSON.stringify(value)); }

function runCases() {
  let v = 0;
  for (let r = 0; r < 20; r++) v += curried(warm);
  show('curried', v);
  v = 0;
  for (let i = 0; i < warm * 4; i++) v += nested(i);
  show('nested', v);
  const holders = [new Holder(1), new Holder(2.5), new Holder('s')];
  const made = [];
  for (let i = 0; i < warm * 4; i++) {
    const h = holders[i % 3];
    const f = h.make();
    made.push(f(i & 3)(1));
    if (i % 997 === 0) made.push(f.call({ v: 999 }, 1).call({ v: -1 }, 2));
  }
  show('holders', made.slice(-12));
  show('holders sum', made.filter((x) => typeof x === 'number').reduce((a, b) => a + b, 0));
  show('loop', [new Holder(3).loop(100000), new Holder(0.5).loop(70000)]);
  const target = { tag: 'target' };
  let same = 0;
  for (let i = 0; i < warm * 4; i++) same += arrowThis.call(target) === target ? 1 : 0;
  show('arrowThis', same);
  let g = 0, u = 0, m = 0;
  for (let i = 0; i < warm * 4; i++) {
    g += sloppyGlobal() ? 1 : 0;
    u += strictUndefined() ? 1 : 0;
    m += moduleThis() === module.exports ? 1 : 0;
  }
  show('this kinds', [g, u, m]);
  show('useSlot', useSlot(200000));
  show('useCaptured', useCaptured(200000));
  show('callCall', callCall(200000));
  show('literals', literals(100000));
  let fl = '';
  for (let i = 0; i < warm * 4; i++) fl = flags(i);
  show('flags', fl);
  show('strings', strings(100000));
  let caught = 0;
  for (let i = 0; i < warm * 4; i++) {
    try { throwsSymbol(i === warm * 3 ? Symbol('s') : i); } catch (e) { caught += e instanceof TypeError ? 1 : 100; }
  }
  show('symbol template', caught);
  show('voids', voids(100000));
}

const COMPILED = ['add', 'add3', 'nested', 'inner', 'own', 'moduleThis', 'inc', 'f', 'curried', 'useSlot', 'callCall', 'literals', 'flags', 'strings', 'throwsSymbol', 'voids', 'arrowThis', 'sloppyGlobal'];

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
    ['force', 'opt:force opt:verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of COMPILED) {
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
        assert.doesNotMatch(child.stderr, new RegExp(`jit: opt rejected func=${fn} `), `${name}: ${fn} must not be rejected`);
      }
      assert.doesNotMatch(child.stderr, /reason=closure-capture/, `${name}: arrows in arrows capture`);
    }
    console.log(`PASS opt arrow closures ${name}`);
  }
  console.log('ok');
}
