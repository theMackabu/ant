const assert = require('node:assert');

function child() {
  const n = 1200000;
  let seen = 0;
  const visit = o => { seen += o.i; };
  for (let i = 0; i < n; i++) process.nextTick(visit, { i });
  setTimeout(() => console.log(seen), 0);
}

if (process.argv[2] === 'child') {
  child();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ANT_DEBUG: 'gc:trace', NO_COLOR: '1' },
    timeout: 180000,
  });
  const marks = result => {
    const out = new Map();
    for (const match of result.stderr.matchAll(/gc: major .*? mark=([\d.]+) .*? live=(\d+)->/g)) out.set(match[2], Number(match[1]));
    return out;
  };

  const serial = run(['--gc-threads', '0']);
  assert.strictEqual(serial.status, 0, serial.stderr.slice(-4000));
  assert.strictEqual(serial.stdout.trim(), String(1200000 * 1199999 / 2));
  const parallel = run();
  assert.strictEqual(parallel.status, 0, parallel.stderr.slice(-4000));
  assert.strictEqual(parallel.stdout, serial.stdout);

  if (/gc: /.test(parallel.stderr)) {
    const alone = marks(serial);
    let compared = 0;
    for (const [live, ms] of marks(parallel)) {
      if (Number(live) < 500000 || !alone.has(live)) continue;
      compared++;
      const limit = alone.get(live) * 10 + 20;
      assert.ok(ms < limit, `marking ${live} objects took ${ms} ms with helpers, ${alone.get(live)} ms without`);
    }
    assert.ok(compared > 0, 'expected a major collection with the jobs pending');
  }
  console.log('ok');
}
