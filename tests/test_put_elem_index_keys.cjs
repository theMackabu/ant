const assert = require('node:assert');

function run() {
  const back = [];
  for (let i = 999; i >= 0; i--) back[i] = i * 2;
  assert.strictEqual(back.length, 1000);
  assert.strictEqual(back[0], 0);
  assert.strictEqual(back[999], 1998);
  assert.deepStrictEqual(Object.keys(back).slice(0, 3), ['0', '1', '2']);

  const sparse = [];
  for (let i = 4999; i >= 0; i -= 7) sparse[i] = i;
  assert.strictEqual(sparse.length, 5000);
  assert.strictEqual(sparse[4999], 4999);
  assert.strictEqual(2 in sparse, false);

  const keys = [];
  const o = {};
  o[-0] = 'zero';
  o[1.5] = 'frac';
  o[-1] = 'neg';
  o[4294967295] = 'max';
  o[4294967294] = 'last';
  o[NaN] = 'nan';
  for (const k of Object.keys(o)) keys.push(k);
  assert.deepStrictEqual(keys, ['0', '4294967294', '1.5', '-1', '4294967295', 'NaN']);

  const arr = [];
  arr[4294967295] = 'not an index';
  assert.strictEqual(arr.length, 0);
  assert.strictEqual(arr['4294967295'], 'not an index');

  const frozen = Object.freeze([1, 2, 3]);
  frozen[0] = 9;
  frozen[5] = 9;
  assert.deepStrictEqual(Array.from(frozen), [1, 2, 3]);
  assert.throws(() => { 'use strict'; frozen[0] = 9; }, TypeError);
  assert.throws(() => { 'use strict'; frozen[7] = 9; }, TypeError);
  assert.throws(() => { 'use strict'; 'abc'[0] = 'x'; }, TypeError);
  assert.throws(() => { const u = undefined; u[0] = 1; }, TypeError);

  const seen = [];
  Object.defineProperty(Array.prototype, 77, { set(v) { seen.push(v); }, configurable: true });
  const hole = [];
  hole[77] = 'via proto';
  delete Array.prototype[77];
  assert.deepStrictEqual(seen, ['via proto']);
  assert.strictEqual(hole.length, 0);

  const ta = new Uint8Array(4);
  ta[1] = 300;
  ta[9] = 1;
  assert.deepStrictEqual(Array.from(ta), [0, 44, 0, 0]);

  function mapped(a) { arguments[0] = 'changed'; return a; }
  assert.strictEqual(mapped('orig'), 'changed');

  const writes = [];
  const proxy = new Proxy({}, { set(t, k, v) { writes.push(typeof k, k); t[k] = v; return true; } });
  proxy[3] = 'p';
  assert.deepStrictEqual(writes, ['string', '3']);

  const str = new String('ab');
  str[0] = 'z';
  str[5] = 'y';
  assert.strictEqual(str[0], 'a');
  assert.strictEqual(str[5], 'y');
}

if (process.argv[2] === 'child') {
  for (let i = 0; i < 3; i++) run();
  console.log('child ok');
} else {
  const { spawnSync } = require('node:child_process');
  for (const [args, debug] of [[['--jitless'], ''], [[], ''], [[], 'opt:force']]) {
    const child = spawnSync(process.execPath, [...args, __filename, 'child'], { encoding: 'utf8', env: { ...process.env, ANT_DEBUG: debug, NO_COLOR: '1' } });
    assert.strictEqual(child.status, 0, `${args.join(' ')} ${debug}: ${child.stdout}${child.stderr}`);
    assert.match(child.stdout, /child ok/);
  }
  console.log('ok');
}
