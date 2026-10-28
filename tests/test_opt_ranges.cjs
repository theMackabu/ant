const assert = require('node:assert');

const warm = 3000;

function show(label, value) {
  console.log(label, JSON.stringify(value, (k, v) => (typeof v === 'number' && Object.is(v, -0) ? '-0' : v)));
}

function shiftedProducts(h, k) {
  const a = (h >>> 24) * 3;
  const b = (h >>> 1) * 1;
  const c = ((h >>> 16) & 255) * 5 + (h >>> 31) * 7;
  const d = (h >>> k) * 3;
  return [a, b, c, d, (h >>> 0) * 3, ((h >>> 1) + (h >>> 1)) | 0, (h >>> 1) + (h >>> 1), (h >>> 2) * 4];
}

function ushrSum(h) {
  return [(h >>> 1) + (h >>> 1), (h >>> 2) * 4, (h >>> 3) * 8 + 7];
}

function hashWords(words, n) {
  let h = 0;
  for (let i = 0; i < n; i++) h = (h ^ (((words[i] >>> 24) * 3 + ((words[i] >>> 16) & 255) * 5) | 0)) | 0;
  return h;
}

function osrShifted(seed, n) {
  let h = seed | 0, s = 0;
  for (let i = 0; i < n; i++) {
    h = (h * 1103515245 + 12345) | 0;
    s = (s + (h >>> 20) * 3) | 0;
  }
  return s;
}

function next(i) {
  return i + 1;
}

function boundedAccess(a, i) {
  const x = a[i];
  return [x, i + 1, (a[i] + i) | 0];
}

function xorChain(a, b, c) {
  const x = a ^ b;
  const y = x ^ c;
  const z = (y << 3) ^ (y >> 2);
  return z ^ (a | 0);
}

const fns = [(x) => x, (x) => -x];

function carried(n, exitAt) {
  let v0 = 1, v1 = 2, v2 = 3, v3 = 4, v4 = 5, v5 = 6, v6 = 7, v7 = 8, v8 = 9, v9 = 10, v10 = 11, v11 = 12;
  let v12 = 13, v13 = 14, v14 = 15, v15 = 16, v16 = 17, v17 = 18, v18 = 19, v19 = 20, v20 = 21, v21 = 22, v22 = 23;
  let v23 = 24, v24 = 25, v25 = 26, v26 = 27, v27 = 28, v28 = 29, v29 = 30, v30 = 31, v31 = 32, v32 = 33, v33 = 34;
  for (let i = 0; i < n; i++) {
    fns[i & 1](i);
    v0 = (v0 + v1) | 0; v1 = (v1 + v2) | 0; v2 = (v2 + v3) | 0; v3 = (v3 + v4) | 0; v4 = (v4 + v5) | 0; v5 = (v5 + v6) | 0;
    v6 = (v6 + v7) | 0; v7 = (v7 + v8) | 0; v8 = (v8 + v9) | 0; v9 = (v9 + v10) | 0; v10 = (v10 + v11) | 0; v11 = (v11 + v12) | 0;
    v12 = (v12 + v13) | 0; v13 = (v13 + v14) | 0; v14 = (v14 + v15) | 0; v15 = (v15 + v16) | 0; v16 = (v16 + v17) | 0;
    v17 = (v17 + v18) | 0; v18 = (v18 + v19) | 0; v19 = (v19 + v20) | 0; v20 = (v20 + v21) | 0; v21 = (v21 + v22) | 0;
    v22 = (v22 + v23) | 0; v23 = (v23 + v24) | 0; v24 = (v24 + v25) | 0; v25 = (v25 + v26) | 0; v26 = (v26 + v27) | 0;
    v27 = (v27 + v28) | 0; v28 = (v28 + v29) | 0; v29 = (v29 + v30) | 0; v30 = (v30 + v31) | 0; v31 = (v31 + v32) | 0;
    v32 = (v32 + v33) | 0; v33 = (v33 + v0) | 0;
    if (i === exitAt) v5 = v5 + 0.5;
    fns[(i + 1) & 1](v3);
  }
  const low = v0 ^ v1 ^ v2 ^ v3 ^ v4 ^ v6 ^ v7 ^ v8 ^ v9 ^ v10 ^ v11 ^ v12 ^ v13 ^ v14 ^ v15 ^ v16;
  const high = v17 ^ v18 ^ v19 ^ v20 ^ v21 ^ v22 ^ v23 ^ v24 ^ v25 ^ v26 ^ v27 ^ v28 ^ v29 ^ v30 ^ v31 ^ v32 ^ v33;
  return [low, high, v5];
}

function runCases() {
  for (let k = 0; k < warm; k++) shiftedProducts(k * 2654435761, 1 + (k & 7));
  show('shifted products', [shiftedProducts(-1, 1), shiftedProducts(0x7fffffff, 31), shiftedProducts(-0x80000000, 0), shiftedProducts(0, 1),
    shiftedProducts(-2, 32), shiftedProducts(2 ** 32 + 7, 1), shiftedProducts(-1.5, 1), shiftedProducts('4294967295', 2)]);
  for (let k = 0; k < warm; k++) ushrSum(k * 64);
  show('ushr sum', [ushrSum(-1), ushrSum(-0x80000000), ushrSum(0x7fffffff), ushrSum(1 << 30), ushrSum(-8)]);
  const words = [];
  for (let i = 0; i < 100; i++) words.push((i * 2654435761) | 0);
  for (let k = 0; k < warm / 10; k++) hashWords(words, 100);
  show('hash words', [hashWords(words, 100), hashWords([-1, -1, 0x7fffffff, -0x80000000], 4), hashWords([2 ** 40, 1.5, NaN], 3)]);
  show('osr shifted', [osrShifted(7, 200000), osrShifted(-1, 100000), osrShifted(0x7fffffff, 3)]);

  const dense = [1, 2, 3, 4, 5, 6, 7, 8];
  for (let k = 0; k < warm; k++) boundedAccess(dense, k & 7);
  show('bounded access', [boundedAccess(dense, 7), boundedAccess(dense, 8), boundedAccess(dense, 0x7fffffff), boundedAccess(dense, -1),
    boundedAccess([0x7fffffff], 0)]);
  for (let k = 0; k < warm; k++) next(k);
  show('next', [next(0x7fffffff), next(-0x80000000), next(1.5)]);

  for (let k = 0; k < warm; k++) xorChain(k, k * 3, k * 7);
  show('xor chain', [xorChain(-1, 0x7fffffff, -0x80000000), xorChain(1.5, 2 ** 33, -0), xorChain('3', null, true)]);

  for (let k = 0; k < 30; k++) carried(1000, -1);
  show('carried', [carried(1000, -1), carried(1000, 500), carried(3, 1), carried(0, -1)]);
}

const COMPILED = ['shiftedProducts', 'ushrSum', 'hashWords', 'osrShifted', 'boundedAccess', 'xorChain', 'carried'];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
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
      assert.match(child.stderr, /jit: bailout .*func=carried .*reason=opt-exit/, `${name}: carried must exit mid-loop`);
    }
    console.log(`PASS opt ranges ${name}`);
  }
}
