const assert = require('node:assert');

function child() {
  const out = [];
  const make = n => { const a = []; for (let i = 0; i < n; i++) a.push({ v: -1 }); return a; };
  const small = make(100), exact = make(1024), large = make(4096), growing = make(1100);
  const appended = [];
  let churn = [];
  for (let i = 0; i < 300000; i++) churn[i & 4095] = { i };
  churn = null;

  function storeVariable(arr, n, salt) { for (let i = 0; i < n; i++) arr[(i * 7 + salt) % arr.length] = { v: i + salt }; }
  function storeConstant(arr, n) { for (let i = 0; i < n; i++) { arr[7] = { v: i }; arr[1000] = { v: -i }; } }
  function storeFresh(arr, n) { for (let i = 0; i < n; i++) { const o = { v: i, w: [i] }; arr[i % arr.length] = o; } }
  function append(arr, n) { for (let i = 0; i < n; i++) arr.push({ v: i }); }
  function sum(arr) { let s = 0; for (let i = 0; i < arr.length; i++) s = (s + arr[i].v + (arr[i].w ? arr[i].w[0] : 0)) | 0; return s; }

  for (let round = 0; round < 30; round++) {
    storeVariable(small, 5000, round);
    storeVariable(exact, 5000, round);
    storeVariable(large, 20000, round);
    storeConstant(large, 3000);
    storeFresh(exact, 4000);
    storeVariable(growing, 3000, round);
    if (round % 5 === 4) for (let k = 0; k < 2048; k++) growing.push({ v: k });
    append(appended, 2000);
    if (appended.length > 30000) appended.length = 0;
    out.push([sum(small), sum(exact), sum(large), sum(growing), sum(appended)].join(','));
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
    timeout: 180000,
    maxBuffer: 16 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  for (const [name, debug] of [['default', ''], ['force', 'opt:force,verify']]) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
  }
  console.log('ok');
}
