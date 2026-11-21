const assert = require('node:assert');

function child() {
  const out = [];

  const holder = { run(x) { return x + 1; }, pad: 0 };
  function single(o, i) { return o.run(i); }
  let sum = 0;
  for (let i = 0; i < 20000; i++) sum += single(holder, i);
  holder.run = function replaced(x) { return x + 2; };
  for (let round = 0; round < 20; round++) for (let i = 0; i < 20000; i++) sum += single(holder, i);
  out.push(sum);

  class Root { hop(x) { return x + 1; } }
  class Leaf extends Root { hop(x) { return super.hop(x) + 1; } }
  const leaf = new Leaf();
  function viaSuper(o, i) { return o.hop(i); }
  sum = 0;
  for (let i = 0; i < 20000; i++) sum += viaSuper(leaf, i);
  Root.prototype.hop = function swapped(x) { return x + 5; };
  for (let round = 0; round < 20; round++) for (let i = 0; i < 20000; i++) sum += viaSuper(leaf, i);
  out.push(sum);

  function first(x) { return x + 1; }
  function second(x) { return x + 2; }
  function dispatch(f, i) { return f(i); }
  sum = 0;
  for (let i = 0; i < 20000; i++) sum += dispatch(first, i);
  for (let i = 0; i < 20000; i++) sum += dispatch(second, i);
  for (let round = 0; round < 20; round++) for (let i = 0; i < 20000; i++) sum += dispatch(Math.abs, -i);
  out.push(sum);

  const steps = [];
  for (let k = 0; k < 16; k++) steps.push(new Function('x', `return x + ${k};`));
  const flipping = { step: steps[0], pad: 0 };
  function flip(o, i) { return o.step(i); }
  sum = 0;
  for (let k = 0; k < 16; k++) {
    flipping.step = steps[k];
    for (let i = 0; i < 20000; i++) sum += flip(flipping, i);
  }
  out.push(sum);
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
  assert.strictEqual(interp.stdout, '4200610000\n4202230000\n4399840000\n3202240000\n');
  for (const [name, debug] of [['default', ''], ['template', 'opt:off'], ['force', 'opt:force,verify']]) {
    const result = run({ ANT_DEBUG: debug });
    assert.strictEqual(result.status, 0, `${name}: ${result.error || result.stdout + result.stderr.slice(-4000)}`);
    assert.strictEqual(result.stdout, interp.stdout, `${name}: output differs from --jitless`);
  }
  const traced = run({ ANT_DEBUG: 'dump/vm:op-warn' });
  assert.strictEqual(traced.status, 0, traced.stderr.slice(-4000));
  const lines = traced.stderr.split('\n');
  const exits = {};
  for (const line of lines) {
    const match = /reason=opt-exit kind=BadCallTarget/.test(line) && /func=(\S+)/.exec(line);
    if (match) exits[match[1]] = (exits[match[1]] || 0) + 1;
  }
  assert.ok((exits.dispatch || 0) <= 4, `dispatch took ${exits.dispatch} call-target exits`);
  assert.ok((exits.flip || 0) <= 10, `flip took ${exits.flip} call-target exits`);
  const refused = lines.filter((line) => /inline-refused caller=(single|hop) .*reason=(closure|exited-before)/.test(line));
  assert.deepStrictEqual(refused, []);
  const compiled = (name) => lines.filter((line) => line.includes(`jit: opt compiled func=${name} `));
  for (const name of ['single', 'viaSuper']) {
    const seen = compiled(name);
    if (seen.length) assert.ok(/ inlined=[1-9]/.test(seen[seen.length - 1]), `${name}: ${seen[seen.length - 1]}`);
  }
  console.log('ok');
}
