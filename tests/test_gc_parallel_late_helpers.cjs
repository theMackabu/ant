const assert = require('node:assert');

function child() {
  function node(key, left, right) { return { key, left, right, payload: [key, key + 1, key + 2, 'n' + key] }; }
  function build(depth, key) { return depth === 0 ? null : node(key, build(depth - 1, key * 2), build(depth - 1, key * 2 + 1)); }
  function sum(tree) { return tree ? tree.key + sum(tree.left) + sum(tree.right) : 0; }

  const live = [];
  let checksum = 0;
  for (let round = 0; round < 24; round++) {
    live.push(build(15, round + 1));
    if (live.length > 4) live.shift();
    for (let i = 0; i < 2000; i++) checksum = (checksum + node(i, null, null).payload.length) | 0;
  }
  for (const tree of live) checksum = (checksum + sum(tree)) | 0;
  console.log(checksum);
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
  const stalled = run({ ANT_DEBUG: 'gc:trace,slow-helpers' });
  assert.strictEqual(stalled.status, 0, stalled.stderr.slice(-4000));
  assert.strictEqual(stalled.stdout, interp.stdout);
  const pauses = [...stalled.stderr.matchAll(/gc: major .*? ms=([\d.]+)/g)].map(match => Number(match[1]));
  if (/gc: /.test(stalled.stderr)) {
    assert.ok(pauses.length >= 3, `expected several major collections, saw ${pauses.length}`);
    const worst = Math.max(...pauses);
    assert.ok(worst < 40, `a major collection waited for a stalled marking helper (${worst} ms)`);
  }
  console.log('ok');
}
