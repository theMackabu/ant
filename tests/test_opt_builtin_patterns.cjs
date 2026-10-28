const assert = require('node:assert');

const warm = 3000;

function typeofObject(v) { return typeof v === 'object'; }
function typeofFunction(v) { return typeof v === 'function'; }
function typeofNotObject(v) { return typeof v !== 'object' ? 1 : 0; }
function typeofLooseFunction(v) { return 'function' != typeof v ? 1 : 0; }
function typeofObjectNotNull(v) { return typeof v === 'object' && v !== null ? 1 : 0; }
function typeofValue(v) { return typeof v; }
function typeofBranch(v) {
  if (typeof v == 'object') return v === null ? 'null' : 'obj';
  if (typeof v === 'function') return 'fn';
  return typeof v;
}

function protoLiteral(p) { const o = { __proto__: p, a: 1 }; return o; }
function protoNull(k) { const o = { __proto__: null, k }; return o.k; }
function protoLate(k) { const o = { a: k, __proto__: null, b: k + 1 }; return o.a + o.b; }
function protoOnly(p) { return { __proto__: p }; }
const base = { get inherited() { return 'from-base'; }, shared: 7 };
function protoBase(v) { const o = { __proto__: base, own: v }; return o.inherited + ':' + o.shared + ':' + o.own; }
function protoSpread(src, p) { return { ...src, __proto__: p }; }
function protoGetterKey(v) { const o = { get __proto__() { return v; } }; return o; }
function protoQuoted(v) { return { '__proto__': v, z: 1 }; }
function protoShorthand(__proto__) { return { __proto__ }; }
function protoFunctionValue() { return { __proto__: function () {} }; }

function appendX(n) { let s = ''; for (let i = 0; i < n; i++) s += 'x'; return s.length; }
function appendRead(n) { let s = 'a'; s += 'b'; return s.length + n; }
function appendMixed(n) { let s = ''; for (let i = 0; i < n; i++) s += i + ','; return s; }
function appendValues(vals) { let s = ''; for (let i = 0; i < vals.length; i++) s += vals[i]; return s; }
function appendNumber(n) { let s = 0; for (let i = 0; i < n; i++) s += 'n'; return s; }
function appendRope(n) { let s = 'r'.repeat(40) + 'q'.repeat(40); for (let i = 0; i < n; i++) s += 'y'; return s.length + s.slice(75, 85); }
function appendLong(n) { let s = ''; for (let i = 0; i < n; i++) s += 'abcdefghij'; return s.length + ':' + s.slice(-12); }
function appendCaptured(n) {
  let s = '';
  const get = () => s;
  for (let i = 0; i < n; i++) s += 'c';
  return get().length + ':' + s.length;
}
function appendCapturedWriter(n) {
  let s = 'a';
  const o = { toString() { s = 'reset'; return 'Z'; } };
  for (let i = 0; i < n; i++) s += 'b';
  s += o;
  s += '!';
  return s;
}
function appendTry(n) {
  let s = '';
  try {
    for (let i = 0; i < n; i++) s += 't';
    s += { toString() { throw new Error('boom'); } };
  } catch (e) {
    s += e.message;
  }
  return s;
}
function appendCapturedTemplate(n) {
  let s = '<';
  const peek = () => s.length;
  let seen = 0;
  for (let i = 0; i < n; i++) { s += `${i}.`; seen += peek(); }
  return s + ':' + seen;
}
function appendParam(s, n) { for (let i = 0; i < n; i++) s += 'p'; return s; }
function appendTemplate(n) { let s = ''; for (let i = 0; i < n; i++) s += `${i}-`; return s; }
function appendReadInLoop(n) {
  let s = '';
  const seen = [];
  for (let i = 0; i < n; i++) {
    s += String.fromCharCode(97 + (i % 26));
    if (i % 7 === 0) seen.push(s);
  }
  return seen.join('|');
}
function appendAlias(n) {
  let s = 'k';
  const before = s;
  for (let i = 0; i < n; i++) s += 'm';
  const mid = s;
  s += 'z';
  return before + '/' + mid + '/' + s;
}

function Many8(a) { this.pa = a + 0; this.pb = a + 1; this.pc = a + 2; this.pd = a + 3; this.pe = a + 4; this.pf = a + 5; this.pg = a + 6; this.ph = a + 7; }
function makeMany8(a) { const o = new Many8(a); return o.ph + o.pa; }
function Many12(a) { this.pa = a + 0; this.pb = a + 1; this.pc = a + 2; this.pd = a + 3; this.pe = a + 4; this.pf = a + 5; this.pg = a + 6; this.ph = a + 7; this.pi = a + 8; this.pj = a + 9; this.pk = a + 10; this.pl = a + 11; }
function makeMany12(a) { const o = new Many12(a); return o.pl + o.pa; }
function Many20(a) { this.pa = a + 0; this.pb = a + 1; this.pc = a + 2; this.pd = a + 3; this.pe = a + 4; this.pf = a + 5; this.pg = a + 6; this.ph = a + 7; this.pi = a + 8; this.pj = a + 9; this.pk = a + 10; this.pl = a + 11; this.pm = a + 12; this.pn = a + 13; this.po = a + 14; this.pp = a + 15; this.pq = a + 16; this.pr = a + 17; this.ps = a + 18; this.pt = a + 19; }
function makeMany20(a) { const o = new Many20(a); return o.pt + o.pa; }
function Many40(a) { this.pa = a + 0; this.pb = a + 1; this.pc = a + 2; this.pd = a + 3; this.pe = a + 4; this.pf = a + 5; this.pg = a + 6; this.ph = a + 7; this.pi = a + 8; this.pj = a + 9; this.pk = a + 10; this.pl = a + 11; this.pm = a + 12; this.pn = a + 13; this.po = a + 14; this.pp = a + 15; this.pq = a + 16; this.pr = a + 17; this.ps = a + 18; this.pt = a + 19; this.pu = a + 20; this.pv = a + 21; this.pw = a + 22; this.px = a + 23; this.py = a + 24; this.pz = a + 25; this.paa = a + 26; this.pab = a + 27; this.pac = a + 28; this.pad = a + 29; this.pae = a + 30; this.paf = a + 31; this.pag = a + 32; this.pah = a + 33; this.pai = a + 34; this.paj = a + 35; this.pak = a + 36; this.pal = a + 37; this.pam = a + 38; this.pan = a + 39; }
function makeMany40(a) { const o = new Many40(a); return o.pan + o.pa; }
function Cond(a, f) { this.a = a; if (f & 1) this.b = a; if (f & 2) this.c = a; this.d = a; this.e = a; if (f & 4) this.f = a; this.g = a; this.h = a; this.i = a; this.j = a; }
function makeCond(a) { const o = new Cond(a, a & 7); return o.j + Object.keys(o).length; }

function show(label, value) {
  console.log(label, typeof value === 'string' ? value : JSON.stringify(value));
}

function describeProto(o) {
  const p = Object.getPrototypeOf(o);
  if (p === null) return 'null';
  if (p === Object.prototype) return 'Object.prototype';
  if (p === Array.prototype) return 'Array.prototype';
  if (typeof p === 'function') return 'function:' + JSON.stringify(p.name);
  return 'other:' + JSON.stringify(Object.keys(p));
}

function typeofValues() {
  async function af() {}
  function* gf() { yield 1; }
  class K {}
  const revocableFn = Proxy.revocable(function () {}, {});
  revocableFn.revoke();
  const revocableObj = Proxy.revocable({}, {});
  revocableObj.revoke();
  return [
    undefined, null, true, false, 0, -0, 1, 1.5, NaN, Infinity, -Infinity, 1n, 'x', '', Symbol('s'),
    {}, [], Object.create(null), function () {}, K, af, gf, gf(), () => 1, function () {}.bind(null),
    new Proxy({}, {}), new Proxy(function () {}, {}), new Proxy(K, {}), revocableFn.proxy, revocableObj.proxy,
    new Date(0), /re/g, new Map(), new Set(), new WeakMap(), new WeakSet(), Promise.resolve(1), new Error('e'),
    new Uint8Array(2), new ArrayBuffer(4), Math, JSON, Math.max, Object, Array, Proxy, new String('s'), new Number(1),
    new Boolean(false), Object(1n), Object(Symbol('o')), (function () { return arguments; })(1, 2),
  ];
}

function runCases() {
  const values = typeofValues();
  for (let k = 0; k < warm; k++) {
    const v = values[k % values.length];
    typeofObject(v); typeofFunction(v); typeofNotObject(v); typeofLooseFunction(v);
    typeofObjectNotNull(v); typeofValue(v); typeofBranch(v);
  }
  const rows = values.map(v => [
    typeofObject(v), typeofFunction(v), typeofNotObject(v), typeofLooseFunction(v),
    typeofObjectNotNull(v), typeofValue(v), typeofBranch(v),
  ].join(','));
  show('typeof', rows.join(' '));

  const protos = [null, { tag: 1 }, 5, 'str', undefined, true, Symbol('p'), [1], function named() {}, Math.max];
  for (let k = 0; k < warm; k++) {
    protoLiteral(protos[k % protos.length]);
    protoNull(k);
    protoLate(k);
    protoOnly(protos[k % protos.length]);
    protoBase(k);
    protoSpread({ s: k }, protos[k % 2]);
  }
  show('proto literal', protos.map(p => describeProto(protoLiteral(p)) + '/' + protoLiteral(p).a + '/' + ('toString' in protoLiteral(p))).join(' '));
  show('proto only', protos.map(p => describeProto(protoOnly(p))).join(' '));
  show('proto null', [protoNull(4), protoNull('k'), 'toString' in { __proto__: null }]);
  show('proto late', [protoLate(3), Object.keys({ a: 1, __proto__: null, b: 2 }).join()]);
  show('proto base', [protoBase(1), protoBase('v')]);
  show('proto spread', [describeProto(protoSpread({ s: 1 }, null)), JSON.stringify(protoSpread({ s: 2 }, { t: 1 })), protoSpread({ s: 3 }, { t: 1 }).t]);
  show('proto getter key', [describeProto(protoGetterKey(1)), protoGetterKey(2).__proto__, Object.keys(protoGetterKey(3)).join()]);
  show('proto quoted', [describeProto(protoQuoted(null)), Object.keys(protoQuoted(null)).join(), describeProto(protoQuoted([]))]);
  show('proto shorthand', [describeProto(protoShorthand(null)), Object.keys(protoShorthand(null)).join()]);
  show('proto function value', [describeProto(protoFunctionValue())]);

  for (let k = 0; k < warm; k++) {
    appendX(5); appendRead(k); appendMixed(4); appendNumber(2); appendRope(2); appendCaptured(3);
    appendCapturedWriter(2); appendCapturedTemplate(3); appendTry(2); appendParam('s', 2); appendTemplate(3); appendReadInLoop(10); appendAlias(2);
    appendValues(['a', 1, null]);
  }
  show('append x', [appendX(0), appendX(1), appendX(1000)]);
  show('append read', appendRead(5));
  show('append mixed', appendMixed(25));
  show('append values', appendValues(['a', 1, null, undefined, true, 2.5, -0, 10n, [1, 2], { toString() { return 'T'; } }, { valueOf() { return 9; } }]));
  try { appendValues(['a', Symbol('nope')]); } catch (e) { show('append symbol', e.name); }
  show('append number', [appendNumber(0), appendNumber(3)]);
  show('append rope', appendRope(10));
  show('append long', appendLong(200000));
  show('append captured', appendCaptured(50));
  show('append captured writer', appendCapturedWriter(3));
  show('append captured template', appendCapturedTemplate(12));
  show('append try', appendTry(4));
  show('append param', [appendParam('s', 3), appendParam(1, 2), appendParam(undefined, 1)]);
  show('append template', appendTemplate(12));
  show('append read in loop', appendReadInLoop(60));
  show('append alias', appendAlias(4));

  let total = 0;
  for (let k = 0; k < warm; k++) total += makeMany8(k) + makeMany12(k) + makeMany20(k) + makeMany40(k) + makeCond(k);
  show('constructors', total);
  const c = new Cond(1, 5);
  show('cond keys', Object.keys(c).join());
  show('many40 keys', Object.keys(new Many40(2)).length + ':' + new Many40(2).pan);
}

const COMPILED = [
  'typeofObject', 'typeofFunction', 'typeofNotObject', 'typeofLooseFunction', 'typeofObjectNotNull', 'typeofValue', 'typeofBranch',
  'protoLiteral', 'protoNull', 'protoLate', 'protoOnly', 'protoBase',
  'appendX', 'appendRead', 'appendMixed', 'appendValues', 'appendNumber', 'appendRope', 'appendCaptured', 'appendCapturedWriter', 'appendCapturedTemplate',
  'appendParam', 'appendTemplate', 'appendReadInLoop', 'appendAlias',
  'Many8', 'Many12', 'Many20', 'Many40', 'Cond',
];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const env = { ...process.env };
  delete env.ANT_DEBUG;
  delete env.FORCE_COLOR;
  const run = (debug, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...env, ANT_DEBUG: debug, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
  });
  const interp = run('', ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['force', 'opt:force dump/vm:op-warn'],
    ['force verify', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run(debug);
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && name !== 'off') {
      for (const fn of COMPILED) {
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
        assert.doesNotMatch(child.stderr, new RegExp(`jit: opt (rejected|disabled) func=${fn} `), `${name}: ${fn} must not be rejected`);
        assert.doesNotMatch(child.stderr, new RegExp(`callee=${fn} bc=\\d+ reason=(TYPEOF|SET_PROTO|STR_APPEND_LOCAL|STR_ALC_SNAPSHOT)`), `${name}: ${fn} must be inlineable`);
      }
      for (const fn of ['Many8', 'Many12', 'Many20', 'Many40', 'Cond']) {
        const compiles = child.stderr.match(new RegExp(`jit: opt compiled func=${fn} `, 'g')) || [];
        assert.ok(compiles.length <= 2, `${name}: ${fn} compiled ${compiles.length} times`);
      }
    }
    console.log(`PASS opt builtin patterns ${name}`);
  }
}
