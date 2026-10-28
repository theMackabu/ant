const assert = require('node:assert');

function maker(k) {
  let inner = function (x) {
    return x + k;
  };
  function loop(n) {
    const k0 = k;
    let s = 0;
    for (let i = 0; i < n; i++) {
      if ((i & 7) === 5) s += inner(i);
      else s += i + k0;
    }
    return s;
  }
  return loop;
}

function protoMaker(k) {
  function Box(v) {
    this.v = v;
  }
  Box.prototype.get = function () {
    return this.v + k;
  };
  return function run(n) {
    let s = 0;
    for (let i = 0; i < n; i++) s += (i & 3) === 1 ? new Box(i).get() : i + k;
    return s;
  };
}

function churn(rounds) {
  let keep = null;
  for (let r = 0; r < rounds; r++) {
    const batch = new Array(2048);
    for (let i = 0; i < batch.length; i++) batch[i] = { i, f: function () { return i; }, text: 'y' + i };
    keep = batch[r & 2047];
  }
  return keep.i;
}

function Point(x) {
  this.x = x;
  this.y = x + 1;
}

Point.prototype.sum = function () {
  return this.x + this.y;
};

const helper = { twice: (v) => v * 2 };

function runCompileChurn() {
  let total = 0;
  for (let k = 0; k < 400; k++) {
    const f = new Function('P', 'h', 'n', 'let s = 0; for (let i = 0; i < n; i++) { const p = new P(i); s += p.sum() + h.twice(i) + Math.max(i, 3); } return s;');
    for (let r = 0; r < 3; r++) total += f(Point, helper, 700);
    if ((k & 63) === 63) churn(200);
  }
  churn(300);
  console.log('churn', total);
}

function runChild() {
  const N = 3000;
  const base = (N * (N - 1)) / 2;
  for (let round = 0; round < 8; round++) {
    const k = round * 7 + 1;
    assert.strictEqual(maker(k)(N), base + N * k, `closure round ${round}`);
    assert.strictEqual(protoMaker(k)(N), base + N * k, `prototype round ${round}`);
    churn(300);
  }
  console.log('values ok');
}

if (process.argv[2] === 'child') {
  runChild();
} else if (process.argv[2] === 'churn') {
  runCompileChurn();
} else {
  const { spawnSync } = require('node:child_process');
  for (const mode of ['default', 'opt:force', 'opt:off']) {
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: mode === 'default' ? '' : mode, NO_COLOR: '1' },
      timeout: 120000,
      maxBuffer: 16 * 1024 * 1024,
    });
    const label = mode;
    assert.strictEqual(child.status, 0, `${label}: ${child.error || child.stderr.slice(-2000)}`);
    assert.match(child.stdout, /values ok/, `${label}: ${child.stdout.slice(-2000)}${child.stderr.slice(-2000)}`);
    console.log(`PASS opt code values outlive their closures ${label}`);
  }
  for (const mode of ['default', 'opt:off']) {
    const child = spawnSync(process.execPath, [__filename, 'churn'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: `gc:trace dump/vm:op-warn${mode === 'default' ? '' : ` ${mode}`}`, NO_COLOR: '1' },
      timeout: 120000,
      maxBuffer: 64 * 1024 * 1024,
    });
    const label = mode;
    assert.strictEqual(child.status, 0, `${label}: ${child.error || child.stderr.slice(-2000)}`);
    assert.match(child.stdout, /churn 1468747200/, `${label}: ${child.stdout.slice(-2000)}`);
    const compiles = (child.stderr.match(/jit: (opt |osr )?compiled/g) || []).length;
    const counts = [...child.stderr.matchAll(/^gc: major .* units=(\d+)$/gm)].map((m) => Number(m[1]));
    assert.ok(compiles >= 400, `${label}: only ${compiles} compiles`);
    assert.ok(counts.length > 0, `${label}: no major collection traced`);
    assert.ok(counts[counts.length - 1] < 64, `${label}: ${counts[counts.length - 1]} code units alive after ${compiles} compiles`);
    console.log(`PASS compiled code units die under compile churn ${label}`);
  }
}
