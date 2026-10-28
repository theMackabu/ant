const assert = require('node:assert');

function make(n) {
  const big = new Array(n).fill(0).map((_, i) => ({ i }));
  class P {
    constructor(x) { this.x = x; }
    size() { return big.length; }
  }
  return { P, ref: new WeakRef(big) };
}

function run(C) {
  let s = 0;
  for (let i = 0; i < 20000; i++) {
    const o = new C(i);
    s += o.x;
    s += o.size();
  }
  return s;
}

function churn() {
  const a = [];
  for (let i = 0; i < 1500000; i++) a.push({ i });
  return a.length;
}

if (process.argv[2] === 'child') {
  const refs = [];
  let total = 0;
  for (let r = 0; r < 10; r++) {
    let { P, ref } = make(20000);
    refs.push(ref);
    for (let k = 0; k < 4; k++) total += run(P);
    P = null;
  }
  assert.strictEqual(total, 10 * 4 * (199990000 + 20000 * 20000));
  setTimeout(() => {
    churn();
    setTimeout(() => {
      churn();
      churn();
      const alive = refs.filter((r) => r.deref() !== undefined).length;
      console.log(`alive ${alive}`);
    }, 0);
  }, 0);
} else {
  const { spawnSync } = require('node:child_process');
  for (const mode of ['default', 'opt:force', 'opt:off']) {
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: `gc:trace${mode === 'default' ? '' : ` ${mode}`}`, NO_COLOR: '1' },
      timeout: 120000,
      maxBuffer: 64 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${mode}: ${child.error || child.stdout + child.stderr.slice(-2000)}`);
    const alive = /alive (\d+)/.exec(child.stdout);
    assert.ok(alive, `${mode}: ${child.stdout}`);
    assert.ok(Number(alive[1]) <= 1, `${mode}: ${alive[1]} dropped classes still alive`);
    assert.ok(/^gc: major /m.test(child.stderr), `${mode}: no major collection traced`);
  }
  console.log('ok');
}
