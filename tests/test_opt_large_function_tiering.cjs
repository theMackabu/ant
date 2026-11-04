const assert = require('node:assert');

function child() {
  const out = [];
  const cold = ['let s = 0;'];
  for (let i = 0; i < 6000; i++) cold.push(`const v${i} = { a: ${i}, b: 's${i}' }; if (v${i}.a % 7 === 3) s += v${i}.b.length;`);
  cold.push('for (let i = 0; i < 2000000; i++) s += i & 7;', 'return s;');
  const coldBody = cold.join('\n');
  const largeCold = new Function(coldBody);
  out.push('cold-bytes ' + (coldBody.length > 128 * 1024));
  out.push('cold ' + largeCold());

  const hot = ['let s = 0;'];
  for (let i = 0; i < 3000; i++) hot.push(`{ const v${i} = o.a + ${i}; if (v${i} % 7 === 3) s += o.b.length; else s -= 1; }`);
  hot.push('return s;');
  const largeHot = new Function('o', hot.join('\n'));
  const o = { a: 1, b: 'xy' };
  let total = 0;
  for (let i = 0; i < 400; i++) total += largeHot(o);
  out.push('hot ' + total);
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
    timeout: 180000,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  assert.match(interp.stdout, /cold-bytes true/);
  const traced = run({ ANT_DEBUG: 'dump/vm:op-warn' });
  assert.strictEqual(traced.status, 0, traced.stderr.slice(-4000));
  assert.strictEqual(traced.stdout, interp.stdout);
  if (/jit: /.test(traced.stderr)) {
    assert.match(traced.stderr, /opt compiled func=<anonymous> nodes=\d+/, 'large cold function with a hot loop reaches the opt tier');
    assert.match(traced.stderr, /opt rejected func=<anonymous> .*reason=graph-budget/, 'large hot function stops at the graph budget');
  }
  console.log('ok');
}
