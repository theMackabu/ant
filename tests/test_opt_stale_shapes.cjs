const assert = require('node:assert');

function note(v) {
  return v;
}

function bumpEpoch() {
  const t = { q: 1, r: 2 };
  delete t.q;
  return t.r;
}

function makeRecord() {
  return new Function('note', `return function Record(a, b, c) {
    this.a = a;
    this.b = b;
    note(a);
    this.c = c;
    this.d = a + c;
  };`)(note);
}

function staleAdditions() {
  let total = 0;
  for (let warm = 92; warm <= 112; warm++) {
    const Record = makeRecord();
    for (let i = 0; i < warm; i++) total += new Record(i, 1, 2).d;
    bumpEpoch();
    for (let i = 0; i < 400; i++) {
      const r = new Record(i, warm, 2);
      assert.strictEqual(r.d, i + 2);
      assert.strictEqual(r.b, warm);
      if (i % 97 === 0) assert.strictEqual(Object.keys(r).join(), 'a,b,c,d');
      total += r.a + r.b + r.c + r.d;
    }
  }
  console.log('stale additions', total);
}

if (process.argv[2] === 'child') {
  staleAdditions();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 256 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  assert.match(interp.stdout, /^stale additions \d+\n$/);
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && name !== 'off') {
      assert.match(child.stderr, /jit: opt compiled func=Record /, `${name}: Record must compile on the opt tier`);
      assert.doesNotMatch(child.stderr, /jit: bailout .*func=Record .*reason=opt-exit kind=BadShape/, `${name}: Record must keep its stale additions compiled`);
    }
    console.log(`PASS opt stale shapes ${name}`);
  }
  if (tier) {
    const dumped = run({ ANT_DEBUG: 'dump/opt:graph' });
    assert.strictEqual(dumped.status, 0, `dump: ${dumped.error || dumped.stderr.slice(-4000)}`);
    assert.strictEqual(dumped.stdout, interp.stdout, 'dump: output differs from --jitless');
    assert.doesNotMatch(dumped.stderr, /codegen failed/, 'dump: codegen failed');
    console.log('PASS opt stale shapes dump');
  }
}
