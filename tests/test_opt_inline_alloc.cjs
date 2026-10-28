const assert = require('node:assert');

const warm = 3000;

function cat(a, b) {
  return a + b;
}

function leaf(tag) {
  return { array: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9], string: 'String for key ' + tag + ' in leaf node' };
}

function mixed(i, o) {
  return [i, 'x' + i, o, i * 0.5, null, undefined, true, [i]];
}

function sized(i) {
  return [
    [i],
    [i, i],
    [i, i, i, i, i],
    [i, i, i, i, i, i, i, i, i, i, i, i, i],
    [i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i],
  ];
}

function huge(i) {
  return [i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i, i];
}

function holes(i) {
  return [i, , i];
}

function junk(n) {
  let s = 0;
  for (let k = 0; k < n; k++) s += { k, a: [k, k] }.a[1];
  return s;
}

function burst(n, tag) {
  let s = 0;
  for (let k = 0; k < n; k++) s += cat(cat('burst rope number ' + tag, ' and index '), String(k)).length;
  return s;
}

const old = { ropes: [], arrays: [] };
for (let k = 0; k < 64; k++) {
  old.ropes.push('');
  old.arrays.push(null);
}

function keep(i) {
  const r = cat('kept rope for slot ' + (i & 63), ' at step ' + i);
  const a = [i, i * 2, { i }];
  old.ropes[i & 63] = r;
  old.arrays[i & 63] = a;
  return r.length + a.length;
}

function checkKept() {
  let s = 0;
  for (let k = 0; k < 64; k++) {
    const r = old.ropes[k], a = old.arrays[k];
    if (!r.startsWith('kept rope for slot ' + k + ' at step ')) throw new Error('bad rope ' + k + ': ' + r);
    if (!r.endsWith(' at step ' + a[0]) || a[1] !== a[0] * 2 || a[2].i !== a[0] || a.length !== 3) throw new Error('bad array ' + k + ': ' + r);
    s += a[0];
  }
  return s;
}

function show(label, value) {
  console.log(label, JSON.stringify(value));
}

function runCases() {
  const long = 'abcdefghijklmnopqrstuvwxyz0123456789';
  const wide = 'ünïcödé ŝtrïng wïth ñøn-ascii ✓ characters';
  let built = '';
  for (let k = 0; k < 40; k++) built += String.fromCharCode(97 + (k % 26));

  let s = 0;
  for (let k = 0; k < warm; k++) s += cat(long, String(k)).length + cat('k', long).length;
  const pairs = [
    ['', long], [long, ''], ['', ''], ['ab', 'cd'], [long, long], [wide, long], [long, wide],
    [built, long], [long, built], [cat(long, long), cat(long, wide)], [1, 2], ['x', 3], [4, 'y'], [long, 0.5],
  ];
  show('concat', [s, pairs.map(([a, b]) => cat(a, b))]);
  let deep = '';
  for (let k = 0; k < 2000; k++) deep = cat(deep, 'segment ' + k + ' of a deep rope; ');
  show('deep rope', [deep.length, deep.slice(0, 40), deep.slice(-40)]);

  s = 0;
  for (let k = 0; k < warm; k++) {
    const l = leaf(String(k * 1.25));
    s += l.array[9] + l.array.length + l.string.length;
  }
  show('leaf', s);

  s = 0;
  for (let k = 0; k < warm; k++) {
    const m = mixed(k, { k });
    s += m[0] + m[1].length + m[2].k + m[3] + (m[4] === null ? 1 : 0) + (m[5] === undefined ? 1 : 0) + m[7][0] + m.length;
  }
  show('mixed', s);

  s = 0;
  for (let k = 0; k < warm; k++) {
    for (const a of sized(k)) s += a.length + a[a.length - 1];
    s += huge(k).length;
  }
  const grown = [...sized(3), huge(3)].map(a => {
    for (let k = 0; k < 20; k++) a.push(k);
    return [a.length, a[0], a[a.length - 1]];
  });
  show('sized', [s, grown]);

  s = 0;
  for (let k = 0; k < warm; k++) {
    const h = holes(k);
    s += h.length + (1 in h ? 100 : 1) + h[2];
  }
  show('holes', [s, Object.keys(holes(5))]);

  s = 0;
  for (let k = 0; k < 20000; k++) {
    s += keep(k);
    if ((k & 255) === 0) s += junk(2000);
  }
  junk(200000);
  show('kept across collections', [s, checkKept()]);

  const sizes = [20000, 200, 60000, 50, 5, 120000, 10];
  const results = [];
  for (let round = 0; round < sizes.length; round++) {
    for (let k = 0; k < 64; k++) keep(round * 64 + k);
    junk(50000);
    results.push(burst(sizes[round], round));
    results.push(checkKept());
  }
  show('rope bursts', results);
}

const COMPILED = ['cat', 'leaf', 'mixed', 'sized', 'keep', 'burst'];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 64 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of COMPILED)
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
    }
    console.log(`PASS opt inline alloc ${name}`);
  }

  if (tier) {
    const dump = run({ ANT_DEBUG: 'dump/opt:graph' });
    assert.strictEqual(dump.status, 0, dump.stdout + dump.stderr);
    assert.match(dump.stderr + dump.stdout, /ConcatenateStrings/, 'concatenations must be inline');
    assert.match(dump.stderr + dump.stdout, /AllocateArray with 10 elements/, 'array literals must be inline');
    assert.match(dump.stderr + dump.stdout, /AllocateArray with 32 elements/, '32-element literals must be inline');
    console.log('PASS opt inline alloc dump');
  }
}
