const assert = require('node:assert');

if (process.argv[2] === 'child') {
  function g(a, b, c, d) { return (a | 0) + (b | 0) + (c | 0) + (d | 0); }
  const A = [1, 2];
  const fs = [g, g];
  function run(n) {
    let s = 0;
    for (let i = 0; i < n; i++) {
      try {
        if (i == 200000) fs[1] = class { constructor(a) { this.v = 1; } };
        s += fs[i >= 200000 ? 1 : 0](i, ...A, 1) & 1;
      } catch (e) { s += e instanceof TypeError ? 3 : 100; }
    }
    return s;
  }
  run(3000);
  run(3000);
  console.log('result ' + run(400000));
} else {
  const { spawnSync } = require('node:child_process');
  const debug = [process.env.ANT_DEBUG, 'dump/vm:op-warn'].filter(Boolean).join(' ');
  const child = spawnSync(process.execPath, [...process.execArgv, __filename, 'child'], {
    encoding: 'utf8', env: { ...process.env, ANT_DEBUG: debug },
    timeout: 60000, maxBuffer: 16 * 1024 * 1024,
  });
  assert.strictEqual(child.status, 0, String(child.error || child.stderr));
  assert.match(child.stdout, /result 700000/);
  const compiles = (child.stderr.match(/jit: opt compiled func=<anonymous>/g) || []).length;
  assert.ok(compiles <= 3, `class constructor compiled ${compiles} times`);
  assert.doesNotMatch(child.stderr, /opt disabled/);
  console.log('ok');
}
