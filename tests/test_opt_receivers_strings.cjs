const assert = require('node:assert');

const warm = 3000;

function show(label, value) {
  console.log(label, JSON.stringify(value, (k, v) => (typeof v === 'number' && Object.is(v, -0) ? '-0' : v)));
}

function Counter(step) {
  this.step = step;
  this.total = 0;
  this.items = [];
  this.label = 'c';
}

Counter.prototype.run = function (n) {
  for (let i = 0; i < n; i++) {
    this.total += this.step;
    if (this.total > 1000000) this.total = 0;
  }
  return this.total;
};

Counter.prototype.grid = function (n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    for (let j = 0; j < n; j++) s += this.step + (this.total & 7);
    this.total = (this.total + 1) | 0;
  }
  return s;
};

Counter.prototype.bump = function (other) {
  this.total += 1;
  other.total += 2;
  this.items.push(this.total);
  if (this.items.length > 8) this.items.length = 0;
  return this.total + other.total;
};

Counter.prototype.later = function (n) {
  const out = [];
  for (let i = 0; i < n; i++) out.push(() => this.step * i);
  return out.map((f) => f()).reduce((a, b) => a + b, 0);
};

Counter.prototype.changes = function (n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    if (i === n - 3) this.step = 'x';
    s = s + this.step;
  }
  return s;
};

function sloppyThis() {
  return this instanceof Number ? 1 : this instanceof String ? 2 : this instanceof Boolean ? 3 : this === globalObject ? 4 : this instanceof Object ? 5 : 6;
}

function strictThis() {
  'use strict';
  return this instanceof Number ? 1 : this === undefined ? 2 : this === null ? 3 : this === 5 ? 4 : this instanceof Object ? 5 : 6;
}

function thisLoop(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += this instanceof Object ? 1 : 2;
  return s;
}

const globalObject = (function () { return this; })();

const RC = [];
for (let k = 48; k < 58; k++) RC[k] = k - 48;
for (let k = 97; k < 123; k++) RC[k] = k - 87;

function intAt(s, i) {
  const c = RC[s.charCodeAt(i)];
  return c == null ? -1 : c;
}

function digitsOf(s) {
  let t = 0;
  for (let i = s.length - 1; i >= 0; --i) t = (t * 31 + intAt(s, i)) | 0;
  return t;
}

function codeAt(s, i) {
  return s.charCodeAt(i);
}

function codes(s, n) {
  let t = 0;
  for (let i = 0; i < n; i++) t = (t + s.charCodeAt(i)) | 0;
  return t;
}

function codesNoArg(s) {
  return s.charCodeAt();
}

function codesTwoArgs(s, i) {
  return s.charCodeAt(i, 3);
}

function product(a, b) {
  return a * b;
}

function products(a, b, n) {
  const out = [];
  for (let i = 0; i < n; i++) out.push(a[i] * b[i]);
  return out;
}

function mixHoles(src, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = (s + ((src[i] & 0x3fff) ^ (src[i] >> 14))) | 0;
  return s;
}

function mixDoubles(src, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = (s + ((src[i] & 0x3fff) ^ (src[i] >> 14))) | 0;
  return s;
}

function mixStrings(src, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = (s + ((src[i] & 0x3fff) ^ (src[i] >> 14))) | 0;
  return s;
}

function receiverCases() {
  const a = new Counter(3), b = new Counter(5);
  for (let k = 0; k < 40; k++) a.run(200);
  show('run', [a.run(10), a.run(300000), b.run(100)]);
  const g = new Counter(2);
  show('grid', [g.grid(200), g.grid(3), g.total]);
  for (let k = 0; k < warm; k++) a.bump(b);
  show('bump', [a.bump(b), b.bump(a), a.items.length, b.total]);
  for (let k = 0; k < 300; k++) a.later(4);
  show('later', [a.later(5), b.later(3)]);
  const c = new Counter(1);
  for (let k = 0; k < 300; k++) { c.step = 1; c.changes(50); }
  c.step = 1;
  show('changes', [c.changes(20), c.step]);
  for (let k = 0; k < warm; k++) { sloppyThis.call({}); strictThis.call({}); }
  show('this values', [sloppyThis.call(5), sloppyThis.call('s'), sloppyThis.call(undefined), sloppyThis.call(null), sloppyThis.call(true),
    strictThis.call(5), strictThis.call(undefined), strictThis.call(null), strictThis.call({})]);
  for (let k = 0; k < 40; k++) thisLoop.call({}, 300);
  show('this loop', [thisLoop.call(7, 600), thisLoop(600), thisLoop.call('x', 10)]);
}

function stringCases() {
  const ascii = 'f00dfacecafe0123456789abcdefbeef';
  for (let k = 0; k < warm; k++) digitsOf(ascii);
  let rope = '';
  for (let k = 0; k < 40; k++) rope += ascii;
  const fresh = ascii + 'zz' + ascii;
  show('digits', [digitsOf(ascii), digitsOf(rope), digitsOf(rope), digitsOf(fresh), digitsOf('héllo0'), digitsOf('9\u{1F600}1'), digitsOf('-1f')]);
  for (let k = 0; k < warm; k++) codeAt(ascii, k & 31);
  show('code at', [codeAt(ascii, 0), codeAt(ascii, 31), codeAt(ascii, 32), codeAt(ascii, -1), codeAt(ascii, 1.5), codeAt(ascii, '3'),
    codeAt(ascii, undefined), codeAt(ascii, NaN), codeAt('héllo', 1), codeAt('héllo', 2), codeAt('\u{1F600}', 1), codeAt('', 0),
    codeAt({ charCodeAt: (i) => 'own' + i }, 4), codeAt(new String('abc'), 1), codeAt(rope, 1000)]);
  for (let k = 0; k < 40; k++) codes(ascii, 32);
  show('codes', [codes(ascii, 32), codes(ascii, 40), codes(rope, 1000), codes('aé', 2)]);
  const original = String.prototype.charCodeAt;
  String.prototype.charCodeAt = function (i) { return 1000 + i; };
  show('replaced', [codeAt(ascii, 2), codes(ascii, 4), digitsOf('12')]);
  String.prototype.charCodeAt = original;
  show('restored', [codeAt(ascii, 2), codes(ascii, 4)]);
  for (let k = 0; k < warm; k++) { codesNoArg(ascii); codesTwoArgs(ascii, k & 7); }
  show('arity', [codesNoArg(ascii), codesNoArg(''), codesTwoArgs(ascii, 5), codesTwoArgs(ascii, 99)]);
}

function productCases() {
  for (let k = 0; k < warm; k++) product(k & 15, (k >> 4) & 15);
  show('product', [product(0, -5), product(-3, 0), product(0, 0), product(-2, 3), product(65536, 65536), 1 / product(0, -1)]);
  const left = [], right = [];
  for (let k = 0; k < 64; k++) { left.push((k * 7) % 13 + 1); right.push((k * 5) % 11 + 1); }
  for (let k = 0; k < 300; k++) products(left, right, 64);
  show('products', [products(left, right, 4), products([0, -1, 3], [-4, 0, 2], 3), products([46341], [46341], 1)]);
}

function elementCases() {
  const digits = [];
  for (let k = 0; k < 64; k++) digits.push((k * 2654435761) & 0xfffffff);
  for (let k = 0; k < 300; k++) { mixHoles(digits, 64); mixDoubles(digits, 64); mixStrings(digits, 64); }
  const holey = digits.slice();
  delete holey[10];
  const doubled = digits.slice();
  doubled[20] = 1.5;
  const stringy = digits.slice();
  stringy[30] = '77';
  show('mix', [mixHoles(holey, 64), mixHoles(digits, 64), mixHoles(digits, 70), mixDoubles(doubled, 64), mixDoubles(digits, 64),
    mixStrings(stringy, 64), mixStrings(digits, 64)]);
}

const COMPILED = ['run', 'grid', 'bump', 'changes', 'sloppyThis', 'strictThis', 'thisLoop', 'digitsOf', 'codeAt', 'codes', 'product', 'products', 'mixHoles', 'mixDoubles', 'mixStrings'];

if (process.argv[2] === 'child') {
  receiverCases();
  stringCases();
  productCases();
  elementCases();
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
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of COMPILED) {
        const pattern = fn === 'run' || fn === 'grid' || fn === 'bump' || fn === 'changes' ? '<anonymous>' : fn;
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${pattern} `), `${name}: ${fn} must compile on the opt tier`);
      }
      for (const [fn, kind] of [['mixHoles', 'OutOfBounds'], ['mixDoubles', 'NotInt32'], ['mixStrings', 'NotInt32'], ['product', 'Overflow'], ['products', 'Overflow']])
        assert.match(child.stderr, new RegExp(`jit: bailout .*func=${fn} .*reason=opt-exit kind=${kind}`), `${name}: ${fn} must exit (${kind})`);
    }
    console.log(`PASS opt receivers strings ${name}`);
  }
}
