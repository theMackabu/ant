const assert = require('node:assert');

function bodyBinding(n) {
  const fns = [];
  let c = 0;
  for (let k = 0; k < n; k++) {
    try {
      try { let y = k; fns.push(() => y); if (k & 1) throw k; c++; } finally { let z = -5; c += z; }
    } catch (e) { c += e; }
  }
  let s = 0;
  for (const f of fns) s += f();
  return c + s;
}

function writerAndFinallyClosure(n) {
  const fns = [];
  let c = 0;
  for (let k = 0; k < n; k++) {
    try {
      try { let y = k; const g = () => y++; fns.push(g); if (k % 3) throw g; c++; } finally { let z = k * 2; const h = () => z; fns.push(h); c += h(); }
    } catch (e) { c += e(); }
  }
  let s = 0;
  for (const f of fns) s += f();
  return c + s;
}

function thrower(i) {
  try { if (i % 2) throw i; return 1; } finally { thrower.n++; }
}
thrower.n = 0;

function fromCall(n) {
  const fns = [];
  let c = 0;
  for (let k = 0; k < n; k++) {
    try {
      try { let y = k; fns.push(() => y); c += thrower(k); } finally { let z = 7; let w = () => z; c += w(); }
    } catch (e) { c += e; }
  }
  let s = 0;
  for (const f of fns) s += f();
  return c + s;
}

function fromAccess(n, nothing) {
  const fns = [];
  let c = 0;
  for (let k = 0; k < n; k++) {
    try {
      try { let y = k; fns.push(() => y); if (k & 1) c += nothing.length; c++; } finally { let z = -5; c += z; }
    } catch (e) { c += e instanceof TypeError ? 3 : 1000000; }
  }
  let s = 0;
  for (const f of fns) s += f();
  return c + s;
}

function nestedBlocks(n) {
  const fns = [];
  let c = 0;
  for (let k = 0; k < n; k++) {
    try {
      try { let y = k; { let q = k + 1; fns.push(() => y + q); if (k & 1) throw k; } c++; } finally { for (let j = 0; j < 2; j++) { let z = j; fns.push(() => z); } }
    } catch (e) { c += e; }
  }
  let s = 0;
  for (const f of fns) s += f();
  return c + s;
}

function mixedCompletions(n) {
  let c = 0;
  let keep = null;
  for (let k = 0; k < n; k++) {
    try {
      try { let y = k; keep = () => y; if (k % 4 === 0) return c + keep(); if (k & 1) throw k; if (k % 4 === 2) continue; } finally { let z = 1; c += z; if (k < n - 1 && k % 4 === 0) continue; }
    } catch (x) { c += x + keep(); }
  }
  return c;
}

function outerStaysShared(n) {
  let outer = 0, c = 0;
  const read = () => outer;
  const fns = [];
  for (let k = 0; k < n; k++) {
    try {
      try { let y = k; fns.push(() => y + outer); outer++; if (k & 1) throw k; } finally { outer += 2; c += read(); }
    } catch (e) { outer++; c += read() - e; }
  }
  return c + read() + fns[0]() + fns[n - 1]();
}

function hot(n) {
  let c = 0, last = null;
  for (let k = 0; k < n; k++) {
    try {
      try { let y = k; last = () => y; if (k & 1) throw k; c++; } finally { c++; }
    } catch (e) { c += e === k ? 1 : 1000; }
  }
  return c + last();
}

if (process.argv[2] === 'stats') {
  let total = 0;
  for (let round = 0; round < 60; round++) total += hot(1000);
  console.log(total);
} else {
  for (let round = 0; round < 200; round++) {
    assert.strictEqual(bodyBinding(300), 66000);
    assert.strictEqual(writerAndFinallyClosure(300), 254550);
    assert.strictEqual(fromCall(300), 69600);
    assert.strictEqual(nestedBlocks(300), 112950);
    assert.strictEqual(fromAccess(300, null), 43950);
    assert.strictEqual(mixedCompletions(300), 45300);
    assert.strictEqual(outerStaysShared(300), 218024);
  }
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  if (typeof Ant !== 'undefined' && tier) {
    const { spawnSync } = require('node:child_process');
    const child = spawnSync(process.execPath, [__filename, 'stats'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: 'dump/opt:stats', NO_COLOR: '1' },
      timeout: 120000,
    });
    assert.strictEqual(child.status, 0, child.stdout + child.stderr.slice(-4000));
    assert.strictEqual(child.stdout.trim(), String(60 * 2999));
    const stats = /caught_in_code=(\d+) caught_in_interpreter=(\d+)/.exec(child.stderr);
    assert.ok(stats, child.stderr.slice(-2000));
    assert.ok(Number(stats[1]) + Number(stats[2]) <= 16, 'throws into a finally handler that closes upvalues went through the catch site: ' + stats[0]);
  }
  console.log('ok');
}
