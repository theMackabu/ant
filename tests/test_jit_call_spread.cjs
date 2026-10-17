const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

function workload() {
  function args() { return Array.prototype.slice.call(arguments); }
  const log = [];
  function* gen() { log.push('g1'); yield 1; log.push('g2'); yield 2; }
  function side(x) { log.push('s' + x); return x; }

  function spreadCalls(i) {
    log.length = 0;
    const r1 = args(side(0), side(1), ...gen());
    const xs = [1, 2];
    const r2 = args(...xs, xs.push(3));
    const r3 = args(xs.length, ...xs);
    const o = { m(...a) { return [this === o, a.length]; } };
    const r4 = o.m(...xs);
    const r5 = o?.m(...xs);
    const f = null;
    const r6 = f?.(...xs);
    const big = Array.from({ length: 20 + (i % 3) }, (_, k) => k);
    const r7 = args(i, ...big).length;
    const r8 = args(...'héllo', ...[]).join('');
    const r9 = args(0, ...new Set([1, 2]), ...new Map([[3, 4]]).values());
    const r10 = args(...[1, , 3]);
    let r11;
    try { args(1, ...i); } catch (e) { r11 = e.constructor.name; }
    let r12;
    try { args(...(function* () { yield 1; throw new RangeError('it'); })()); } catch (e) { r12 = e.message; }
    const own = [1, 2, 3];
    own[Symbol.iterator] = function* () { yield 'own'; };
    const r13 = args(...own);
    const r14 = Math.max(0, ...[i, 5]);
    const r15 = args(...new Uint8Array([7, 8]));
    return [r1, log.join(','), r2, r3, r4, r5, r6, r7 - i % 3, r8, r9, r10, r11, r12, r13, r14 >= 5, r15];
  }

  const expected = JSON.stringify([
    [0, 1, 1, 2], 's0,s1,g1,g2', [1, 2, 3], [3, 1, 2, 3], [true, 3], [true, 3], undefined, 21, 'héllo',
    [0, 1, 2, 4], [1, undefined, 3], 'TypeError', 'it', ['own'], true, [7, 8],
  ]);
  for (let i = 0; i < 3000; i++) assert.strictEqual(JSON.stringify(spreadCalls(i)), expected);

  function plain(xs) { return args(0, ...xs); }
  for (let i = 0; i < 3000; i++) plain([i]);
  const iterator = Array.prototype[Symbol.iterator];
  Array.prototype[Symbol.iterator] = function* () { yield 'patched'; };
  assert.deepStrictEqual(plain([1, 2]), [0, 'patched']);
  Array.prototype[Symbol.iterator] = iterator;
  assert.deepStrictEqual(plain([1, 2]), [0, 1, 2]);

  function applies(i) {
    Array.prototype[1] = 'proto';
    const holes = [args(...[0, , 2]), args.apply(null, [0, , 2]), Reflect.apply(args, null, [0, , 2])];
    delete Array.prototype[1];
    const xs = [{ v: i }, { v: 2 }, { v: 3 }];
    const mutated = (function (a, b, c) { xs.length = 0; for (let j = 0; j < 50; j++) ({ j }); return [a.v, b.v, c.v]; }).apply(null, xs);
    return [holes, args.apply(null, { length: 2, 0: 'a', 1: 'b' }), args.apply(null, null), mutated[0] === i];
  }
  const applyExpected = JSON.stringify([[[0, 'proto', 2], [0, 'proto', 2], [0, 'proto', 2]], ['a', 'b'], [], true]);
  for (let i = 0; i < 3000; i++) assert.strictEqual(JSON.stringify(applies(i)), applyExpected);

  function strictThis() { 'use strict'; return this; }
  function strictSpread(xs) { return strictThis(...xs); }
  for (let i = 0; i < 3000; i++) assert.strictEqual(strictSpread([i]), undefined);

  const wide = Array.from({ length: 30 }, (_, k) => k);
  function wideCalls(i) { return args(i, ...wide).length + args.apply(null, wide).length + args(...[], ...wide).length; }
  for (let i = 0; i < 3000; i++) assert.strictEqual(wideCalls(i), 91);

  function deep(n, ...rest) { return n === 0 ? rest.length : deep(n - 1, ...rest, n); }
  assert.strictEqual(deep(200), 200);
  function inf(...a) { return 1 + inf(...a); }
  assert.throws(() => inf(1, 2), RangeError);

  console.log('PASS jit call spread workload');
}

if (process.env.ANT_JIT_CALL_SPREAD_CHILD) {
  workload();
} else {
  function count(...a) { return a.length; }
  const huge = new Array(100000).fill(1);
  assert.strictEqual(count(...huge), 100000);
  assert.strictEqual(count.apply(null, huge), 100000);
  assert.strictEqual(count(1, 2, ...huge), 100002);

  const tooMany = new Array(1000000).fill(1);
  const overflow = { name: 'RangeError', message: 'Maximum call stack size exceeded' };
  assert.throws(() => count(...tooMany), overflow);
  assert.throws(() => count.apply(null, tooMany), overflow);

  const child = spawnSync(process.execPath, [__filename], {
    encoding: 'utf8',
    env: { ...process.env, ANT_JIT_CALL_SPREAD_CHILD: '1', ANT_DEBUG: 'dump/vm:jit' },
    maxBuffer: 256 * 1024 * 1024,
    timeout: 120000,
  });
  assert.strictEqual(child.status, 0, String(child.error || child.stderr.slice(-4000)));
  assert.match(child.stdout, /PASS jit call spread workload/);
  for (const name of ['spreadCalls', 'plain', 'strictSpread'])
    assert.match(child.stderr, new RegExp(`jit_${name}_[^\\n]*:\\s*module`), name);
  assert.match(child.stderr, /\bcall\s+apply_proto, jit_helper_call_spread,/);
  console.log('PASS jit call spread');
}
