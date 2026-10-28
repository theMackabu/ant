const assert = require('node:assert');

const ROUNDS = 3000;

function checksum(o) {
  let sum = 0;
  for (const key of Object.keys(o)) {
    const v = o[key];
    sum = (sum * 31 + (typeof v === 'number' ? v : key.length)) % 1000000007;
  }
  return sum;
}

function snapshot(o) {
  const own = {};
  for (const key of Object.keys(o)) own[key] = o[key];
  return JSON.stringify(own);
}

function Wide(i) {
  this.p0 = i;
  this.p1 = i + 1;
  this.p2 = i + 2;
  this.p3 = i + 3;
  this.p4 = i + 4;
  this.p5 = i + 5;
}

function crossHere(o, i) {
  const before = o.p4 + o.p5;
  o.q0 = i;
  o.q1 = i;
  o.q2 = i;
  o.p5 = before;
  o.q3 = before + 1;
  o.p4 = o.p5 + o.q3;
  return o.p4 + o.p5 + o.q0;
}

function addThree(o, v) {
  o.r0 = v;
  o.r1 = v + 1;
  o.r2 = v + 2;
  o.r3 = v + 3;
}

function crossInlinedCallee(o, i) {
  const a = o.p4;
  addThree(o, i);
  o.p5 = a + o.r3;
  return o.p4 + o.p5 + o.r2;
}

function addKeyed(o, n, v) {
  for (let k = 0; k < n; k++) o['k' + k] = v + k;
  return n;
}

function crossCallee(o, i) {
  const a = o.p4;
  const n = addKeyed(o, 6 + (i & 3), i);
  o.p5 = a + n;
  o.p4 = o.p5 + o.k5;
  return o.p4 + o.p5;
}

const growing = {
  get grow() {
    this.g0 = 10;
    this.g1 = 11;
    this.g2 = 12;
    this.g3 = 13;
    this.g4 = 14;
    return 7;
  },
  set setGrow(v) {
    this.s0 = v;
    this.s1 = v;
    this.s2 = v;
    this.s3 = v;
    this.s4 = v;
  },
};

function Proxied(i) {
  this.p0 = i;
  this.p1 = i;
  this.p2 = i;
  this.p3 = i;
  this.p4 = i;
  this.p5 = i;
}
Proxied.prototype = growing;

function growWide(o, i) {
  o.w0 = i;
  o.w1 = i;
  o.w2 = i;
  o.w3 = i;
  o.w4 = i;
  o.w5 = i;
  o.w6 = i;
  o.w7 = i;
  return i;
}

function lowerAfterCall(o, i) {
  const a = o.p5;
  growWide(o, i);
  o.p4 = a + i;
  return o.p4 + o.w7;
}

function lowerAfterGetter(o, i) {
  const a = o.p5;
  const g = o.grow;
  o.p4 = a + g;
  return o.p4 + o.g4;
}

function lowerAfterGeneric(o, i) {
  const a = o.p5;
  o['w' + (i & 1)] = i;
  o['x' + (i & 1)] = i;
  o['y' + (i & 1)] = i;
  o['z' + (i & 1)] = i;
  o.p4 = a + i;
  return o.p4;
}

function crossGetter(o, i) {
  const a = o.p4;
  const g = o.grow;
  o.p5 = a + g + i;
  return o.p5 + o.p4 + o.g4;
}

function crossSetter(o, i) {
  const a = o.p5;
  o.setGrow = i;
  o.p4 = a + o.s4;
  return o.p4 + o.p5;
}

function crossGeneric(o, i) {
  const a = o.p5;
  delete o.p1;
  o.p4 = a + i;
  o.p1 = i;
  return o.p4 + o.p5 + o.p1;
}

function crossDefine(o, i) {
  const a = o.p4;
  Object.defineProperty(o, 'd' + (i & 3), { value: i, writable: true, enumerable: true, configurable: true });
  Object.assign(o, { e0: i, e1: i, e2: i, e3: i, e4: i });
  o.p5 = a + o.e4;
  return o.p4 + o.p5;
}

let sink = null;
function churn(n) {
  let keep = null;
  for (let k = 0; k < n; k++) keep = { a: k, b: [k, k], c: 'x' + k, d: { k }, e: k, f: k };
  sink = keep;
  return keep.a;
}

function crossGc(o, i) {
  const a = o.p4;
  churn(200);
  o.p5 = a + i;
  return o.p4 + o.p5;
}

function Dict(i) {
  this.p0 = i;
  this.p1 = i;
  this.p2 = i;
  this.p3 = i;
  this.p4 = i;
  this.p5 = i;
  this.p6 = i;
  this.p7 = i;
  this.p8 = i;
  this.p9 = i;
}

function makeDictionary(i) {
  const o = new Dict(i);
  delete o.p0;
  delete o.p2;
  delete o.p4;
  delete o.p6;
  delete o.p8;
  delete o.p1;
  o.z = i;
  return o;
}

function readDictionary(o, i) {
  const a = o.p7 + o.p9;
  o.p9 = a + i;
  return o.p3 + o.p5 + o.p7 + o.p9 + o.z;
}

function shrinkDictionary(o, i) {
  const a = o.p9;
  delete o.p5;
  delete o.p3;
  o.p9 = a + i;
  return o.p7 + o.p9 + o.z;
}

function Shared() {}
for (let k = 0; k < 8; k++) Shared.prototype['m' + k] = k * 100;

let protoGrowth = 0;
function growPrototype(flag) {
  if (!flag) return 0;
  protoGrowth++;
  Shared.prototype['n' + protoGrowth] = protoGrowth;
  Shared.prototype.m6 = 600 + protoGrowth;
  return 1;
}

function readPrototype(o, i) {
  const a = o.m5;
  const grew = growPrototype(i % 500 === 499);
  return a + o.m6 + o.m7 + grew;
}

function Alpha(i) {
  this.a = i;
  this.b = i + 1;
  this.c = i + 2;
  this.d = i + 3;
  this.e = i + 4;
  this.f = i + 5;
}

function Beta(i) {
  this.a = i;
  this.b = i + 1;
  this.c = i + 2;
  this.d = i + 3;
  this.e = i + 4;
  this.f = i + 5;
  this.g = i + 6;
}

function Gamma(i) {
  this.a = i;
  this.b = i + 1;
}

function sharedPrefix(o, i) {
  o.f = o.e + i;
  return o.a + o.e + o.f;
}

function makePrefixed(i) {
  switch (i % 5) {
    case 0: return new Alpha(i);
    case 1: return new Beta(i);
    case 2: return { a: i, b: i + 1, c: i + 2, d: i + 3, e: i + 4, f: i + 5 };
    case 3: {
      const o = new Gamma(i);
      o.c = i + 2;
      o.d = i + 3;
      o.e = i + 4;
      o.f = i + 5;
      return o;
    }
    default: {
      const o = Object.create(null);
      o.a = i;
      o.b = i + 1;
      o.c = i + 2;
      o.d = i + 3;
      o.e = i + 4;
      o.f = i + 5;
      return o;
    }
  }
}

function loopRead(o, n) {
  let s = 0;
  for (let k = 0; k < n; k++) s += o.p4 + o.p5;
  return s;
}

function loopGrow(o, n) {
  let s = 0;
  for (let k = 0; k < n; k++) {
    s += o.p5;
    if (k === (n >> 1)) addThree(o, k);
    o.p5 = s & 1023;
  }
  return s + o.p5 + o.r3;
}

function osrGrow(o) {
  let s = 0;
  for (let k = 0; k < 3000; k++) {
    s += o.p4;
    if ((k & 511) === 300) addKeyed(o, 8, k);
    o.p4 = (s + k) & 4095;
  }
  return s + o.p4 + o.k7;
}

function run(name, fn, make, rounds = ROUNDS) {
  let total = 0;
  let last = '';
  for (let i = 0; i < rounds; i++) {
    const o = make(i);
    total += fn(o, i) + checksum(o);
    if (i % 251 === 0 || i === rounds - 1) last = snapshot(o);
    total = total % 1000000007;
  }
  console.log(name, total, last);
}

function main() {
  run('crossHere', crossHere, (i) => new Wide(i));
  run('crossInlinedCallee', crossInlinedCallee, (i) => new Wide(i));
  run('crossCallee', crossCallee, (i) => new Wide(i));
  run('lowerAfterCall', lowerAfterCall, (i) => new Wide(i));
  run('lowerAfterGetter', lowerAfterGetter, (i) => new Proxied(i));
  run('lowerAfterGeneric', lowerAfterGeneric, (i) => new Wide(i));
  run('crossGetter', crossGetter, (i) => new Proxied(i));
  run('crossSetter', crossSetter, (i) => new Proxied(i));
  run('crossGeneric', crossGeneric, (i) => new Wide(i));
  run('crossDefine', crossDefine, (i) => new Wide(i));
  run('crossGc', crossGc, (i) => new Wide(i), 600);
  run('readDictionary', readDictionary, makeDictionary);
  run('shrinkDictionary', shrinkDictionary, makeDictionary);
  run('readPrototype', readPrototype, () => new Shared());
  run('sharedPrefix', sharedPrefix, makePrefixed);
  run('loopRead', (o, i) => loopRead(o, 50 + (i & 7)), (i) => new Wide(i));
  run('loopGrow', (o, i) => loopGrow(o, 40 + (i & 7)), (i) => new Wide(i));
  run('osrGrow', osrGrow, (i) => new Wide(i), 3);
  const kept = [];
  for (let i = 0; i < 64; i++) kept.push(new Wide(i));
  let total = 0;
  for (let round = 0; round < 40; round++)
    for (const o of kept) {
      total += crossHere(o, round) + crossCallee(o, round) + crossGeneric(o, round);
      total %= 1000000007;
    }
  console.log('reused', total, snapshot(kept[5]), snapshot(kept[63]));
  console.log('protoGrowth', protoGrowth, snapshot(Shared.prototype));
}

const COMPILED = ['lowerAfterCall', 'lowerAfterGetter', 'crossHere', 'crossInlinedCallee', 'crossCallee', 'crossGetter', 'crossSetter', 'crossDefine', 'crossGc', 'readDictionary', 'readPrototype', 'sharedPrefix', 'loopRead', 'loopGrow', 'osrGrow'];
const FORCED = ['lowerAfterGeneric', 'crossGeneric', 'shrinkDictionary', 'addKeyed', 'growPrototype'];

if (process.argv[2] === 'child') {
  main();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 300000, maxBuffer: 64 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  assert.strictEqual(interp.stdout.split('\n').length, 21, interp.stdout);
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    const want = interp.stdout.split('\n'), got = child.stdout.split('\n');
    for (let i = 0; i < want.length; i++) assert.strictEqual(got[i], want[i], `${name}: line ${i + 1} differs from --jitless`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && name !== 'off') {
      for (const fn of name === 'force' ? COMPILED.concat(FORCED) : COMPILED)
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
    }
    console.log(`PASS opt named storage ${name}`);
  }
}
