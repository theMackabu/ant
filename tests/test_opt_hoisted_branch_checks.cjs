const assert = require('node:assert');

function child() {
  const out = [];

  function nullish(n, o) { let s = 0; for (let i = 0; i < n; i++) s += o.a ?? i; return s; }
  function untaken(n, flag, o) { let s = 0; for (let i = 0; i < n; i++) s += flag ? i : o.b; return s; }
  function either(n, o) { let s = 0; for (let i = 0; i < n; i++) s += o.a || i; return s; }
  function nested(n, o) {
    let s = 0;
    for (let j = 0; j < 4; j++) for (let i = 0; i < n; i++) s += o.a ?? (i + j);
    return s;
  }

  for (let round = 0; round < 3; round++) {
    out.push(nullish(200000, { a: undefined }));
    out.push(untaken(200000, true, { b: 'never' }));
    out.push(either(200000, { a: 0 }));
    out.push(nested(50000, { a: null }));
  }
  out.push(nullish(10, { a: 2 }), untaken(10, false, { b: 'x' }), either(10, { a: 3 }), nested(10, { a: 1 }));
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
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  for (const [name, debug] of [['default', ''], ['force', 'opt:force,verify']]) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
  }
  const traced = run({ ANT_DEBUG: 'dump/vm:op-warn' });
  assert.strictEqual(traced.status, 0, traced.stderr.slice(-4000));
  const hoisted = {};
  for (const line of traced.stderr.split('\n')) {
    const match = /reason=opt-exit kind=Hoisted/.test(line) && /func=(\S+)/.exec(line);
    if (match) hoisted[match[1]] = (hoisted[match[1]] || 0) + 1;
  }
  for (const [func, count] of Object.entries(hoisted)) assert.ok(count <= 1, `${func} took ${count} hoisted-check exits`);
  console.log('ok');
}
