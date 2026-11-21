const assert = require('node:assert');

function child() {
  const und = undefined;
  const out = [];

  function afterReturn(i) {
    if (i >= 0) return 1;
    return 2;
    for (let j = 0; j < 2; j++) { }
  }
  function afterThrow(i) {
    try { throw i; for (let j = 0; j < 2; j++) { } } catch (e) { return e + 1; }
  }
  function afterThrowInFinally(i) {
    try { } finally { throw i; for (let j = 0; j < 2; j++) { } }
  }
  function afterBreak(i) {
    let n = 0;
    for (let k = 0; k < 3; k++) { n += k; break; while (i) i--; }
    return n + i;
  }
  function afterContinue(i) {
    let n = 0;
    for (let k = 0; k < 3; k++) { n += k; continue; do { i--; } while (i > 0); }
    return n + i;
  }
  function afterContinueInNestedFinally(i) {
    let n = 0;
    for (let k = 0; k < 3; k++) {
      try { n += 1; } finally { try { continue; for (let j = 0; j < 3; j++) { } } finally { n += 10; } }
    }
    return n + i;
  }
  function deadForOf(i) {
    return i;
    for (const v of [1, 2]) i += v;
  }
  function deadForIn(i) {
    return i;
    for (const k in { a: 1 }) i += k.length;
  }
  function deadNested(i) {
    if (i >= 0) return i;
    return 0;
    for (let a = 0; a < 2; a++) for (let b = 0; b < 2; b++) i += a + b;
  }
  function liveAndDead(i) {
    let n = 0;
    for (let k = 0; k < 4; k++) n += k;
    return n + i;
    for (let k = 0; k < 4; k++) n -= k;
  }
  function genericAndDead(i) {
    if ((i & 3) === 1) und.x;
    try { throw i; for (let j = 0; j < 4; j++) { } } catch (e) { return e; }
  }
  function throwsReference(i) {
    try { throw [i, 8]; for (let j = 0; j < 4; j++) { } } catch (e) { missing = missing + e[1]; }
  }

  const calls = [
    afterReturn, afterThrow, afterBreak, afterContinue, afterContinueInNestedFinally,
    deadForOf, deadForIn, deadNested, liveAndDead,
  ];
  for (const fn of calls) {
    let sum = 0;
    for (let i = 0; i < 3000; i++) sum += fn(i & 7);
    out.push(fn.name + ' ' + sum);
  }
  for (const fn of [afterThrowInFinally, genericAndDead, throwsReference]) {
    let sum = 0, thrown = 0;
    for (let i = 0; i < 4000; i++) {
      try { sum += fn(i & 7) | 0; } catch (e) { thrown += typeof e === 'number' ? e + 1 : 100; }
    }
    out.push(fn.name + ' ' + sum + ' ' + thrown);
  }
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
    timeout: 120000,
  });
  const expected = [
    'afterReturn 3000', 'afterThrow 13500', 'afterBreak 10500', 'afterContinue 19500',
    'afterContinueInNestedFinally 109500', 'deadForOf 10500', 'deadForIn 10500', 'deadNested 10500',
    'liveAndDead 28500', 'afterThrowInFinally 0 18000', 'genericAndDead 11000 100000', 'throwsReference 0 400000',
  ].join('\n') + '\n';
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  assert.strictEqual(interp.stdout, expected);
  for (const [name, debug] of [['default', ''], ['force', 'opt:force'], ['verify', 'opt:verify'], ['off', 'opt:off']]) {
    const result = run({ ANT_DEBUG: debug });
    assert.strictEqual(result.status, 0, `${name}: ${result.error || result.stdout + result.stderr.slice(-2000)}`);
    assert.strictEqual(result.stdout, expected, `${name}: output differs`);
  }
  if (typeof Ant === 'object') {
    const traced = run({ ANT_DEBUG: 'opt:off dump/vm:op-warn' });
    assert.strictEqual(traced.status, 0, traced.stderr.slice(-2000));
    for (const name of ['afterReturn', 'afterThrow', 'afterBreak', 'afterContinue', 'deadNested', 'liveAndDead'])
      assert.match(traced.stderr, new RegExp(`jit: compiled func=${name} `), `${name} stays compiled`);
  }
  console.log('ok');
}
