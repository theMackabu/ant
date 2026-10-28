const assert = require('node:assert');

function m() { return this; }
function k() { return typeof this; }
Object.prototype.m = m;
Number.prototype.m = m;
Number.prototype.k = k;

function viaMegamorphic(x) { return x.m(); }
function viaCall(x) { return k.call(x); }
function viaKnown(x) { return x.k(); }
function field(o) { return o.x; }
function store(o) { o.x = 5; return 1; }

if (process.argv[2] === 'child') {
  const objs = [];
  for (let j = 0; j < 12; j++) {
    const o = {};
    o['k' + j] = j;
    objs.push(o);
  }
  for (let i = 0; i < 50000; i++) viaMegamorphic(objs[i % 12]);
  assert.strictEqual(typeof viaMegamorphic(-Infinity), 'object');
  assert.strictEqual(viaMegamorphic(-Infinity).valueOf(), -Infinity);
  assert.strictEqual(typeof viaMegamorphic(5), 'object');
  assert.strictEqual(typeof viaMegamorphic(-Infinity), 'object');

  for (let i = 0; i < 50000; i++) viaCall(i & 1 ? {} : []);
  assert.strictEqual(viaCall(-Infinity), 'object');
  assert.strictEqual(viaCall(Infinity), 'object');
  assert.strictEqual(viaCall(-Infinity), 'object');

  const ks = [];
  for (let j = 0; j < 12; j++) {
    const o = { k };
    o['p' + j] = j;
    ks.push(o);
  }
  for (let i = 0; i < 50000; i++) viaKnown(ks[i % 12]);
  assert.strictEqual(viaKnown(-Infinity), 'object');
  assert.strictEqual(viaKnown(1.5), 'object');

  const shapes = [{ x: 1 }, { x: 2, y: 1 }];
  let s = 0;
  for (let i = 0; i < 50000; i++) { s += field(shapes[i & 1]); s += store(shapes[i & 1]); }
  assert.strictEqual(field(-Infinity), undefined);
  assert.strictEqual(store(-Infinity), 1);
  console.log('child ok');
} else {
  const { spawnSync } = require('node:child_process');
  for (const mode of ['default', 'opt:force', 'opt:verify', 'opt:off']) {
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
