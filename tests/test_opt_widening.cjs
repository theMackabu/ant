const assert = require('node:assert');

function branchy(o, k) {
  switch (k) {
    case 0: return o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0;
    case 1: return o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0;
    case 2: return o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0;
    case 3: return o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0;
    case 4: return o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0;
    case 5: return o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0;
    case 6: return o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0;
    case 7: return o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0;
    case 8: return o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0;
    case 9: return o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0;
    case 10: return o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0 + o.c1 + o.d2 + o.e0 + o.f1 + o.g2 + o.h0 + o.i1 + o.j2 + o.k0 + o.l1 + o.a2 + o.b0;
    case 11: return o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0 + o.d1 + o.e2 + o.f0 + o.g1 + o.h2 + o.i0 + o.j1 + o.k2 + o.l0 + o.a1 + o.b2 + o.c0;
    case 12: return o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0 + o.e1 + o.f2 + o.g0 + o.h1 + o.i2 + o.j0 + o.k1 + o.l2 + o.a0 + o.b1 + o.c2 + o.d0;
    default: return 0;
  }
}

function driveBranches() {
  const o = {};
  for (const c of 'abcdefghijkl') for (let j = 0; j < 3; j++) o[c + j] = c.charCodeAt(0) + j;
  let s = 0;
  for (let i = 0; i < 3000; i++) s += branchy(o, 0);
  for (let k = 1; k < 13; k++) {
    for (let i = 0; i < 400; i++) s += branchy(o, i % 20 === 0 ? k : 0);
  }
  return s;
}

function sieve(m, flags) {
  let count = 0;
  for (let i = 2; i <= m; i++) flags[i] = true;
  for (let i = 2; i <= m; i++) {
    if (flags[i]) {
      for (let k = i + i; k <= m; k += i) flags[k] = false;
      count++;
    }
  }
  return count;
}

function driveSieve() {
  let sum = 0;
  for (let r = 0; r < 12; r++) sum += sieve(80000, Array(80001));
  return sum;
}

function lookup(table, keys) {
  let s = 0;
  for (let i = 0; i < keys.length; i++) s += table[keys[i]];
  return s;
}

function keyed(o, a, b, c, d) {
  return o[a] + o[b] + o[c] + o[d];
}

function driveKeys() {
  const table = { alpha: 1, beta: 2, gamma: 3 };
  const keys = ['alpha', 'beta', 'gamma', 'beta'];
  let s = 0;
  for (let i = 0; i < 20000; i++) s += lookup(table, keys);
  const arr = [1, 2, 3, 4];
  arr.w = 10; arr.x = 20; arr.y = 30; arr.z = 40;
  for (let i = 0; i < 5000; i++) s += keyed(arr, 0, 1, 2, 3);
  s += keyed(arr, 'w', 1, 2, 3);
  s += keyed(arr, 0, 'x', 2, 3);
  s += keyed(arr, 0, 1, 'y', 3);
  s += keyed(arr, 0, 1, 2, 'z');
  for (let i = 0; i < 5000; i++) s += keyed(arr, i & 1 ? 'w' : 0, 'x', i & 2 ? 'y' : 2, 3);
  return s;
}

let getterCalls = 0;
Object.defineProperty(globalThis, 'dynamicGlobal', { get() { getterCalls++; return getterCalls & 3; }, configurable: true });

function readGlobal(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += dynamicGlobal;
  return s;
}

function Shape(i) { this['f' + (i % 9)] = i; this.v = i & 3; }
function readV(o) { return o.v; }
function poly(objects, n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += readV(objects[i % objects.length]);
  return s;
}

function drivePoly() {
  const objects = [];
  for (let i = 0; i < 40; i++) objects.push(new Shape(i));
  let s = 0;
  for (let r = 0; r < 40; r++) s += poly(objects, 5000);
  return s;
}

function show(label, value) { console.log(label, JSON.stringify(value)); }

function runCases() {
  show('branches', driveBranches());
  show('sieve', driveSieve());
  show('keys', driveKeys());
  show('global', [readGlobal(30000), getterCalls]);
  show('poly', drivePoly());
}

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 64 * 1024 * 1024,
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
  const count = (text, re) => (text.match(re) || []).length;
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of ['branchy', 'sieve', 'lookup', 'keyed', 'readGlobal', 'poly']) {
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
        assert.doesNotMatch(child.stderr, new RegExp(`jit: opt (rejected|disabled) func=${fn} `), `${name}: ${fn} must stay on the opt tier`);
        assert.doesNotMatch(child.stderr, new RegExp(`^jit: compiled func=${fn} `, 'm'), `${name}: ${fn} must not reach the template JIT`);
      }
      const branchyCompiles = count(child.stderr, /jit: opt compiled func=branchy /g);
      assert.ok(branchyCompiles > 8, `${name}: branchy compiled ${branchyCompiles} times`);
      const sieveCompiles = count(child.stderr, /jit: opt compiled func=sieve /g);
      assert.ok(sieveCompiles <= 4, `${name}: sieve compiled ${sieveCompiles} times`);
      const keyedCompiles = count(child.stderr, /jit: opt compiled func=keyed /g);
      assert.ok(keyedCompiles <= 4, `${name}: keyed compiled ${keyedCompiles} times`);
    }
    console.log(`PASS opt widening ${name}`);
  }
  console.log('ok');
}
