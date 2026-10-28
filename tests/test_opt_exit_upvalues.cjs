const assert = require('node:assert');

const holder = {};

function inner(o, k) {
  const v = o.x;
  if (v === 99) throw new Error('x');
  return v + k;
}

function outer(o, k) {
  let c = 10;
  const inc = () => ++c;
  holder.f = inc;
  inc();
  const r = inner(o, k);
  inc();
  return r + c;
}

function outerObject(o, k) {
  let c = { n: 10 };
  const set = (v) => { c = v; };
  const get = () => c;
  holder.set = set;
  holder.get = get;
  const r = inner(o, k);
  return r + c.n;
}

function c3(o) {
  if (o.x === 7) throw new Error('deep');
  return o.x;
}

function c2(o) {
  let t = 100;
  holder.t = () => t;
  t += c3(o);
  return t;
}

function c1(o) {
  let u = 1000;
  holder.u = () => u;
  u += c2(o);
  return u;
}

function clobber(a, b, c, d) {
  const arr = [a, b, c, d, a + b, c + d];
  return arr.length + arr[0];
}

function work(n) {
  const a = [];
  for (let i = 0; i < n; i++) a.push({ i, s: 'v' + i });
  return a.length;
}

if (process.argv[2] === 'child') {
  let s = 0;
  for (let i = 0; i < 20000; i++) s += outer({ x: 1 }, i & 7);
  assert.strictEqual(s, 20000 * 13 + 2500 * 28);
  try { outer({ y: 0, x: 99 }, 1); assert.fail('no throw'); } catch (e) { assert.strictEqual(e.message, 'x'); }
  let z = 0;
  for (let i = 0; i < 100; i++) z += clobber(i, i, i, i);
  assert.strictEqual(z, 5550);
  assert.strictEqual(holder.f(), 12);
  assert.strictEqual(holder.f(), 13);

  s = 0;
  for (let i = 0; i < 20000; i++) s += outerObject({ x: 1 }, i & 7);
  try { outerObject({ y: 0, x: 99 }, 1); assert.fail('no throw'); } catch (e) { assert.strictEqual(e.message, 'x'); }
  assert.strictEqual(holder.get().n, 10);
  for (let i = 0; i < 2000; i++) {
    holder.set({ n: i, big: new Array(10).fill(i) });
    work(50);
    const g = holder.get();
    if (!g || g.n !== i) assert.fail(`binding read back at ${i}: ${typeof g}`);
  }

  s = 0;
  for (let i = 0; i < 20000; i++) s += c1({ x: 1 });
  assert.strictEqual(s, 20000 * 1101);
  try { c1({ z: 0, x: 7 }); assert.fail('no throw'); } catch (e) { assert.strictEqual(e.message, 'deep'); }
  z = 0;
  for (let i = 0; i < 100; i++) z += clobber(i, i, i, i);
  work(500);
  assert.strictEqual(holder.t(), 100);
  assert.strictEqual(holder.u(), 1000);
  console.log('child ok');
} else {
  const { spawnSync } = require('node:child_process');
  for (const mode of ['default', 'opt:force', 'opt:off']) {
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: mode === 'default' ? '' : mode, NO_COLOR: '1' },
      timeout: 120000,
      maxBuffer: 16 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${mode}: ${child.error || child.stdout + child.stderr}`);
    assert.match(child.stdout, /child ok/, `${mode}: ${child.stdout}${child.stderr}`);
  }
  console.log('ok');
}
