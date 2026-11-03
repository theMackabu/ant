const assert = require('node:assert');

function child() {
  const out = [];
  const N = 20000;

  function Six(a) { this.a = a; this.b = a + 1; this.c = a + 2; this.d = a + 3; this.e = a + 4; this.f = a + 5; }
  Six.prototype.sum = function () { return this.a + this.b + this.c + this.d + this.e + this.f; };
  let total = 0;
  for (let i = 0; i < N; i++) total += new Six(i).sum();
  out.push('six ' + total);

  function Ten(a) {
    this.p0 = a; this.p1 = a; this.p2 = a; this.p3 = a; this.p4 = a;
    this.p5 = a; this.p6 = a; this.p7 = a; this.p8 = a; this.p9 = a;
  }
  let tens = 0;
  for (let i = 0; i < N; i++) { const t = new Ten(i); tens += t.p0 + t.p4 + t.p7 + t.p8 + t.p9; }
  const ten = new Ten(3);
  ten.extra = 'x';
  delete ten.p2;
  out.push('ten ' + tens + ' ' + Object.keys(ten).join(','));

  class Point3 {
    constructor(x, y, z) { this.x = x; this.y = y; this.z = z; this.w = 1; this.tag = 'p'; this.n = null; }
    len() { return this.x * this.x + this.y * this.y + this.z * this.z; }
  }
  let lens = 0;
  for (let i = 0; i < N; i++) lens += new Point3(i % 7, 1, 2).len();
  out.push('class ' + lens);

  function Twelve(a) {
    this.protocol = a; this.slashes = null; this.auth = null; this.host = a + 1; this.port = null; this.hostname = null;
    this.hash = null; this.search = a + 2; this.query = null; this.pathname = null; this.path = a + 3; this.href = 'h' + a;
  }
  let twelves = 0;
  for (let i = 0; i < N; i++) { const t = new Twelve(i); twelves += t.protocol + t.host + t.search + t.path + t.href.length; }
  const tw = new Twelve(1);
  tw.extra1 = 1; tw.extra2 = 2; tw.extra3 = 3; tw.extra4 = 4; tw.extra5 = 5;
  delete tw.auth;
  out.push('twelve ' + twelves + ' ' + Object.keys(tw).join(',') + ' ' + JSON.stringify(Object.assign({}, tw)).length);

  function Pair(a, b) { this.a = a; this.b = b; }
  let pairs = 0;
  for (let i = 0; i < N; i++) pairs += new Pair(i, 1).b;
  out.push('pair ' + pairs);

  const keep = [];
  for (let round = 0; round < 8; round++) {
    for (let i = 0; i < 20000; i++) {
      const s = new Six(i);
      if (i % 97 === 0) keep.push(s);
      const junk = { i, s: 'v' + i };
      if (junk.i < 0) keep.push(junk);
    }
  }
  let kept = 0;
  for (const s of keep) kept += s.sum();
  out.push('kept ' + keep.length + ' ' + kept);

  const w = new Six(10);
  Object.setPrototypeOf(w, { sum() { return -1; }, g: 'proto' });
  w.g2 = 7;
  out.push('proto ' + w.sum() + ' ' + w.g + ' ' + w.g2 + ' ' + w.f);

  const copy = Object.assign({}, new Six(1));
  const cloned = structuredClone(new Ten(2));
  out.push('copy ' + JSON.stringify(copy) + ' ' + JSON.stringify(cloned));

  const wm = new WeakMap();
  const key = new Six(5);
  wm.set(key, 'held');
  for (let i = 0; i < 50000; i++) new Six(i);
  out.push('weak ' + wm.get(key));

  function maybeNew(i) { return i % 1000 === 999 ? Six.call({}, i) : new Six(i).f; }
  let mixed = 0;
  for (let i = 0; i < N; i++) { const r = maybeNew(i); if (typeof r === 'number') mixed += r; }
  out.push('mixed ' + mixed);

  function escape(i, bail) {
    const s = new Six(i);
    if (bail) return s;
    return s.a + s.f;
  }
  let esc = 0;
  for (let i = 0; i < N; i++) esc += escape(i, false);
  const leaked = escape(4, true);
  out.push('escape ' + esc + ' ' + leaked.sum() + ' ' + Object.keys(leaked).length);

  function Grow(a) { this.a = a; this.b = a; this.c = a; this.d = a; this.e = a; }
  const grown = [];
  for (let i = 0; i < 2000; i++) {
    const g = new Grow(i);
    for (let k = 0; k < (i % 12); k++) g['k' + k] = k;
    grown.push(g);
  }
  let gs = 0;
  for (const g of grown) gs += g.e + (g.k3 || 0) + (g.k10 || 0);
  out.push('grow ' + gs);

  console.log(out.join('\n'));
}

if (process.argv[2] === 'child') {
  child();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 256 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  const modes = [
    ['default', ''],
    ['verify', 'opt:verify'],
    ['force', 'opt:force,verify'],
    ['off', 'opt:off'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
    console.log(`PASS wide objects ${name}`);
  }
}
