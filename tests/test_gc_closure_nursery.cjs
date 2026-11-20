const assert = require('node:assert');

const MB = 1024 * 1024;
const ROUNDS = 1000000;

function dead(n) {
  let sum = 0;
  let peak = 0;
  const before = process.memoryUsage().rss;
  for (let i = 0; i < n; i++) {
    let local = i;
    const read = () => local;
    sum += read() & 1;
    if ((i & 8191) === 0) peak = Math.max(peak, process.memoryUsage().rss - before);
  }
  return [sum, peak];
}

function kept(n) {
  const keep = [];
  let sum = 0;
  for (let i = 0; i < n; i++) {
    let local = i;
    const read = () => local;
    keep.push(read);
    sum += read() & 1;
  }
  return [sum, keep.length];
}

function deep(depth, n) {
  if (depth === 0) return dead(n);
  const a = depth, b = depth + 1, c = depth + 2;
  const result = deep(depth - 1, n);
  return a + b + c > 0 ? result : null;
}

function mixed(n) {
  const keep = [];
  let sum = 0;
  for (let i = 0; i < n; i++) {
    if (i % 7 === 0) keep.push({ i });
    const k = i;
    const read = () => k + sum;
    sum = read() & 1023;
  }
  return [keep.length, 0];
}

const CASES = { dead, kept, deep: n => deep(3000, n), mixed };

if (process.argv[2] === 'child') {
  const [first, second] = CASES[process.argv[3]](ROUNDS);
  console.log(`result ${first} ${second}`);
} else {
  const { spawnSync } = require('node:child_process');
  const debug = [process.env.ANT_DEBUG, 'gc:trace'].filter(Boolean).join(' ');

  const run = name => {
    const child = spawnSync(process.execPath, [...process.execArgv, __filename, 'child', name], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: debug, NO_COLOR: '1' },
      timeout: 120000, maxBuffer: 16 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    const [, first, second] = child.stdout.match(/result (\d+) (\d+)/);
    return { first: +first, second: +second, minors: (child.stderr.match(/^gc: minor /gm) || []).length };
  };

  const deadRun = run('dead');
  assert.strictEqual(deadRun.first, ROUNDS / 2);
  assert.ok(deadRun.second < 16 * MB, `dead closures piled up between young collections: ${(deadRun.second / MB).toFixed(1)} MB`);
  assert.ok(deadRun.minors >= 24, `dead closures are collected at the object nursery size: ${deadRun.minors} minors`);

  const keptRun = run('kept');
  assert.strictEqual(keptRun.second, ROUNDS);
  assert.ok(keptRun.minors <= 12, `surviving closures widen the closure nursery: ${keptRun.minors} minors`);

  const deepRun = run('deep');
  assert.strictEqual(deepRun.first, ROUNDS / 2);
  assert.ok(deepRun.minors < deadRun.minors, `a deep stack spaces closure collections out: ${deepRun.minors} against ${deadRun.minors}`);

  const mixedRun = run('mixed');
  assert.ok(mixedRun.minors <= 16, `surviving objects widen the closure nursery: ${mixedRun.minors} minors`);

  console.log('ok');
}
