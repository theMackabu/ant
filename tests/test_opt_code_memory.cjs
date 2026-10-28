const assert = require('node:assert');

const SHAPES = 7;
const WARM = 6000;

function makeReader(round) {
  return new Function('o', 'k', `return (o.x + k + ${round}) | 0;`);
}

function shaped(kind, x) {
  const o = { x };
  for (let i = 0; i < kind; i++) o['p' + i] = i;
  return o;
}

function churn() {
  const a = [];
  for (let i = 0; i < 600000; i++) a.push({ i });
  return a.length;
}

function round(r) {
  const f = makeReader(r);
  let s = 0;
  for (let kind = 0; kind < SHAPES; kind++) {
    const objects = [];
    for (let j = 0; j <= kind; j++) objects.push(shaped(j, j + 1));
    for (let i = 0; i < WARM; i++) s = (s + f(objects[i % objects.length], i & 3)) | 0;
  }
  return s;
}

function expected(r) {
  let s = 0;
  for (let kind = 0; kind < SHAPES; kind++)
    for (let i = 0; i < WARM; i++) s = (s + ((i % (kind + 1)) + 1 + (i & 3) + r)) | 0;
  return s;
}

if (process.argv[2] === 'child') {
  const rounds = Number(process.argv[3]);
  for (let r = 0; r < rounds; r++) {
    assert.strictEqual(round(r), expected(r), `round ${r}`);
    if (r % 8 === 7) churn();
  }
  churn();
  churn();
  for (let r = rounds; r < rounds + 16; r++) assert.strictEqual(round(r), expected(r), `reused round ${r}`);
  console.log('done');
} else {
  const { spawnSync } = require('node:child_process');
  const stats = {};
  for (const rounds of [24, 96]) {
    const child = spawnSync(process.execPath, [__filename, 'child', String(rounds)], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: 'dump/opt:stats', NO_COLOR: '1' },
      timeout: 300000,
      maxBuffer: 64 * 1024 * 1024,
    });
    assert.strictEqual(child.status, 0, `${rounds}: ${child.error || child.stdout + child.stderr.slice(-2000)}`);
    assert.match(child.stdout, /done/);
    const line = /jit: opt stats compiled=(\d+) .* code_bytes=(\d+) freed=(\d+) mapped_peak=(\d+)/.exec(child.stderr);
    if (!line && !/jit: opt stats/.test(child.stderr)) {
      console.log('opt tier off; skipped');
      process.exit(0);
    }
    assert.ok(line, `no code memory stats: ${child.stderr.slice(-500)}`);
    stats[rounds] = { compiled: +line[1], bytes: +line[2], freed: +line[3], mapped: +line[4] };
  }
  const small = stats[24], large = stats[96];
  assert.ok(large.compiled >= 96 * 2, `few compiles: ${JSON.stringify(large)}`);
  assert.ok(large.freed * 2 >= large.compiled, `dropped code not freed: ${JSON.stringify(large)}`);
  assert.ok(large.mapped * 4 <= large.compiled * 16384, `code not packed: ${JSON.stringify(large)}`);
  assert.ok(large.mapped <= small.mapped * 2, `code memory grows with drops: ${JSON.stringify(stats)}`);
  console.log('ok');
}
