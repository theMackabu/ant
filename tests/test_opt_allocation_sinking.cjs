const assert = require('node:assert');

function child() {
  globalThis.globalSink = null;
  const out = [];
  const describe = o => JSON.stringify(Object.keys(o)) + JSON.stringify(Object.values(o)) + Object.getPrototypeOf(o).constructor.name;

  function Point(x, y, z) { this.x = x; this.y = y; this.z = z; }
  function makePoints(n) { let s = 0, last; for (let i = 0; i < n; i++) { last = new Point(i, i + 1, i + 2); globalSink = last; s += last.y; } return [s, last]; }
  for (let i = 0; i < 40; i++) makePoints(5000);
  let [sum, last] = makePoints(1000);
  out.push('plain ' + sum + ' ' + describe(last));
  const seen = [];
  Object.defineProperty(Point.prototype, 'y', { set(v) { seen.push(v); }, get() { return 7; }, configurable: true });
  [sum, last] = makePoints(3);
  out.push('setter ' + sum + ' ' + describe(last) + ' ' + seen.join(',') + ' own=' + Object.hasOwn(last, 'y'));
  delete Point.prototype.y;
  [sum, last] = makePoints(1000);
  out.push('after ' + sum + ' ' + describe(last));

  function Frozen(a, b) { this.a = a; this.b = b; }
  function makeFrozen(n) { let s = 0, last; for (let i = 0; i < n; i++) { last = new Frozen(i, -i); globalSink = last; s += last.a; } return [s, last]; }
  for (let i = 0; i < 40; i++) makeFrozen(5000);
  Object.defineProperty(Object.prototype, 'b', { value: 99, writable: false, configurable: true });
  [sum, last] = makeFrozen(3);
  out.push('readonly ' + sum + ' ' + describe(last) + ' b=' + last.b + ' own=' + Object.hasOwn(last, 'b'));
  delete Object.prototype.b;

  function Partial(src) { this.a = src.k; this.b = src.v.w; this.c = src.k + 1; }
  function makePartial(srcs, n) { let s = 0, last; for (let i = 0; i < n; i++) { last = new Partial(srcs[i & 3]); globalSink = last; s += last.c; } return [s, last]; }
  const plainSrc = [0, 1, 2, 3].map(k => ({ k, v: { w: k * 2 } }));
  for (let i = 0; i < 40; i++) makePartial(plainSrc, 5000);
  const oddSrc = [0, 1, 2, 3].map(k => ({ k, v: { extra: 1, w: 'w' + k } }));
  [sum, last] = makePartial(oddSrc, 4);
  out.push('partial ' + sum + ' ' + describe(last));
  const getterSrc = [0, 1, 2, 3].map(k => ({ k, v: { get w() { return Object.keys(probe).join('|'); } } }));
  let probe = null;
  function Probed(src) { probe = this; this.a = src.k; this.b = src.v.w; this.c = 3; }
  function makeProbed(srcs, n) { let last; for (let i = 0; i < n; i++) { last = new Probed(srcs[i & 3]); globalSink = last; } return last; }
  for (let i = 0; i < 40; i++) makeProbed(plainSrc, 5000);
  out.push('probed ' + describe(makeProbed(getterSrc, 4)));

  function Color(r, g, b) { if (!r) r = 0.0; if (!g) g = 0.0; if (!b) b = 0.0; this.red = r; this.green = g; this.blue = b; }
  function makeColors(n) { let s = 0, last; for (let i = 0; i < n; i++) { last = new Color(i & 1, i % 3, i ? 2 : undefined); s += last.red + last.green + last.blue; } return [s, last]; }
  for (let i = 0; i < 40; i++) makeColors(5000);
  [sum, last] = makeColors(1001);
  out.push('color ' + sum + ' ' + describe(last));

  function Inner(v) { this.v = v; this.w = v * 2; }
  function Outer(v) { this.left = new Inner(v); this.mid = v; this.right = new Inner(v + 1); }
  const keep = [];
  function makeNested(n) { let s = 0; for (let i = 0; i < n; i++) { const o = new Outer(i); keep[i & 255] = o; s += o.left.w + o.right.v + o.mid; } return s; }
  for (let i = 0; i < 40; i++) makeNested(5000);
  out.push('nested ' + makeNested(1000) + ' ' + describe(keep[3]) + ' ' + describe(keep[3].right));

  function Pair(a, b) { this.a = a; this.b = b; }
  function Holder(a) { this.pair = new Pair(a, 3); }
  const pairs = [];
  function useHolders(objs, n) { let s = 0; for (let i = 0; i < n; i++) { const h = new Holder(i); s += objs[i & 3].k + h.pair.a; pairs[i & 7] = h.pair; s += h.pair.b; } return s; }
  const holderSrc = [0, 1, 2, 3].map(k => ({ k }));
  for (let i = 0; i < 40; i++) useHolders(holderSrc, 5000);
  out.push('holder ' + useHolders([0, 1, 2, 3].map(k => ({ x: 1, k })), 100) + ' ' + describe(pairs[5]));

  function literal(n) { const all = []; for (let i = 0; i < n; i++) { const o = {}; o.p = i; o.q = String(i); all.push(o); } return all; }
  for (let i = 0; i < 40; i++) literal(2000);
  const lits = literal(5);
  out.push('literal ' + lits.map(describe).join(';'));

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
    timeout: 180000,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  for (const [name, debug] of [['default', ''], ['force', 'opt:force,verify']]) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
  }
  console.log('ok');
}
