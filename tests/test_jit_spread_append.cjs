const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

function workload() {
  function* gen(n) { for (let i = 0; i < n; i++) yield i; }
  const custom = { [Symbol.iterator]() { let i = 0; return { next: () => ({ done: i >= 2, value: i++ }) }; } };
  function count() { return arguments.length; }
  function sum(...xs) { let s = 0; for (const x of xs) s += x; return s; }

  function arrays(i) {
    const xs = [1, 2, i];
    const a = [0, ...xs, 3];
    const b = [...'a\u{1F600}b'];
    const c = [...new Set([i, i, 1])];
    const d = [...new Map([[i, 1]]).keys()];
    const e = [...gen(3), ...custom];
    const f = [...[1], , i];
    return a.length + b.length + c.length + d.length + e.length + f.length + (1 in f ? 100 : 0) + a[3];
  }

  function calls(i) {
    const xs = [i, 1];
    return count(0, ...xs, ...'ab') + sum(...xs, 2) + Math.max(...xs) + count(...gen(2));
  }

  class Base { constructor(...a) { this.n = a.length; } }
  class Derived extends Base { constructor(...a) { super(0, ...a); } }

  let total = 0;
  for (let i = 0; i < 20000; i++) {
    total += arrays(i) + calls(i) + new Derived(1, i).n;
  }
  assert.strictEqual(total, 600610000);

  const push = Array.prototype.push;
  let pushed = 0;
  Array.prototype.push = function (...a) { pushed++; return push.apply(this, a); };
  for (let i = 0; i < 2000; i++) { count(0, ...[1], 2); arrays(i); }
  Array.prototype.push = push;
  assert.strictEqual(pushed, 0);

  function spreadNumber(v) { return [...v]; }
  let errors = 0;
  for (let i = 0; i < 2000; i++) {
    spreadNumber([i]);
    try { spreadNumber(i); } catch (e) { if (e instanceof TypeError) errors++; }
  }
  assert.strictEqual(errors, 2000);

  function* thrower() { yield 1; throw new RangeError('boom'); }
  let thrown = 0;
  for (let i = 0; i < 2000; i++) {
    try { count(...thrower()); } catch (e) { if (e instanceof RangeError && e.message === 'boom') thrown++; }
  }
  assert.strictEqual(thrown, 2000);

  function garbage(i) {
    const parts = [];
    for (let j = 0; j < 8; j++) parts.push({ j, s: 'x'.repeat(j) + i });
    return [...parts.values(), ...gen(4)].length;
  }
  let g = 0;
  for (let i = 0; i < 5000; i++) g += garbage(i);
  assert.strictEqual(g, 60000);

  console.log('PASS jit spread workload');
}

if (process.env.ANT_JIT_SPREAD_CHILD) {
  workload();
} else {
  const child = spawnSync(process.execPath, [__filename], {
    encoding: 'utf8',
    env: { ...process.env, ANT_JIT_SPREAD_CHILD: '1', ANT_DEBUG: 'dump/vm:jit opt:off' },
    maxBuffer: 64 * 1024 * 1024,
    timeout: 60000,
  });
  assert.strictEqual(child.status, 0, String(child.error || child.stderr.slice(-4000)));
  assert.match(child.stdout, /PASS jit spread workload/);
  for (const name of ['arrays', 'calls', 'spreadNumber', 'garbage'])
    assert.match(child.stderr, new RegExp(`jit_${name}_[^\\n]*:\\s*module`), name);
  assert.match(child.stderr, /\bcall\s+helper2_proto, jit_helper_spread,/);
  assert.match(child.stderr, /\bcall\s+arr_push_proto, js_arr_push,/);
  console.log('PASS jit spread and append');
}
