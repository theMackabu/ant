const assert = require('node:assert');

function branchInFinally(p, n) {
  let c = p;
  try { c++; } finally {
    for (let i = 0; i < n; i++) {
      if (i & 1) c += p;
      else c -= 1;
    }
  }
  return c;
}

function nestedInFinally(p, q, n) {
  let c = p + q;
  try { c++; } finally {
    for (let i = 0; i < n; i++) {
      for (let j = 0; j < 2; j++) {
        if ((i + j) & 1) c += p;
        else c += q;
      }
    }
  }
  return c;
}

function assignedInFinally(p, n) {
  let c = p;
  try { c++; } finally {
    for (let i = 0; i < n; i++) {
      if (i & 1) p += 1;
      else c += p;
    }
  }
  return c + p;
}

function assignedBeforeThrow(p, n) {
  let c = 0;
  try { c++; } finally {
    for (let i = 0; i < n; i++) {
      try {
        if (i & 1) { p = p + 2; throw i; }
        c += p;
      } catch (e) { c += e; }
      c += p;
    }
  }
  return c;
}

function assignedBeforeLoop(p, n) {
  let c = 0;
  p = p * 2;
  try { c++; } finally {
    for (let i = 0; i < n; i++) {
      if (i & 1) c += p;
      else c -= 1;
    }
  }
  return c + p;
}

function capturedInFinally(p, n) {
  let c = 0;
  const read = () => p;
  try { c++; } finally {
    for (let i = 0; i < n; i++) {
      if (i & 1) c += read();
      else p++;
    }
  }
  return c + p;
}

function returnsInFinally(p, n) {
  let c = p;
  try { return c; } finally {
    for (let i = 0; i < n; i++) {
      if (i & 1) c += p;
      else c -= 1;
    }
    returnsInFinally.seen = c;
  }
}

function runAll() {
  let total = 0;
  for (let k = 0; k < 20000; k++) {
    total += branchInFinally(k & 3, 8);
    total += nestedInFinally(k & 3, k & 1, 4);
    total += assignedInFinally(k & 3, 8);
    total += assignedBeforeThrow(k & 3, 6);
    total += assignedBeforeLoop(k & 3, 8);
    total += capturedInFinally(k & 3, 8);
    total += returnsInFinally(k & 3, 8) + returnsInFinally.seen;
  }
  return total;
}

if (process.argv[2] === 'child') {
  console.log(runAll());
} else {
  assert.strictEqual(runAll(), 2450000);
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  if (typeof Ant !== 'undefined' && tier) {
    const { spawnSync } = require('node:child_process');
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn', NO_COLOR: '1' },
      timeout: 120000,
    });
    assert.strictEqual(child.status, 0, child.stdout + child.stderr.slice(-4000));
    assert.strictEqual(child.stdout.trim(), '2450000');
    for (const name of ['branchInFinally', 'nestedInFinally', 'returnsInFinally', 'assignedBeforeLoop']) {
      assert.match(child.stderr, new RegExp(`jit: opt compiled func=${name} `), `${name} compiles in the optimizing tier`);
      assert.doesNotMatch(child.stderr, new RegExp(`jit: opt rejected func=${name} `), `${name} is not rejected`);
    }
    assert.doesNotMatch(child.stderr, /reason=loop-invariant-changed/);
  }
  console.log('ok');
}
