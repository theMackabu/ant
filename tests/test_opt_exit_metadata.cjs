const assert = require('node:assert');

function source(n, inline) {
  const lines = [];
  lines.push('return function big(o, arr, x) {');
  for (let i = 0; i < n; i++) lines.push(`  let v${i} = (x + ${i}) | 0;`);
  lines.push('  let s = 0;');
  for (let i = 0; i < n; i++) {
    const read = inline ? `get(o, ${i})` : `(o.a + ${i})`;
    lines.push(`  s = (s + v${i} + ${read}) | 0; if (s > ${i * 7}) s = (s - arr[${i % 8}]) | 0;`);
  }
  lines.push('  return s;');
  lines.push('};');
  return lines.join('\n');
}

function reference(n, a, arr, x) {
  const v = [];
  for (let i = 0; i < n; i++) v.push((x + i) | 0);
  let s = 0;
  for (let i = 0; i < n; i++) {
    s = (s + v[i] + (a + i)) | 0;
    if (s > i * 7) s = (s - arr[i % 8]) | 0;
  }
  return s;
}

function get(o, k) {
  return o.a + k;
}

function check(n, inline) {
  const big = new Function('get', source(n, inline))(get);
  const arr = [1, 2, 3, 4, 5, 6, 7, 8];
  const o = { a: 1, b: 2 };
  for (let i = 0; i < 3000; i++) assert.strictEqual(big(o, arr, i), reference(n, 1, arr, i), `${n} warm ${i}`);
  const shapes = [{ b: 2, a: 3 }, { a: 1.5 }, { z: 0, y: 0, a: -4 }];
  for (const other of shapes)
    for (const x of [7, -11, 123456]) assert.strictEqual(big(other, arr, x), reference(n, other.a, arr, x), `${n} exit a=${other.a} x=${x}`);
  const holey = [1, 2, 3, , 5, 6, 7, 8];
  assert.strictEqual(big(o, holey, 5), reference(n, 1, holey, 5), `${n} hole`);
}

for (const n of [8, 40, 300]) {
  check(n, false);
  check(n, true);
}

if (process.argv[2] !== 'child') {
  const { spawnSync } = require('node:child_process');
  const debug = (process.env.ANT_DEBUG || '').split(' ').filter((channel) => channel !== 'opt:verify').join(' ');
  const run = (n) => spawnSync(process.execPath, ['-e', `
    const f = new Function(${JSON.stringify(source(n, false))})();
    const before = process.memoryUsage().rss;
    const arr = [1, 2, 3, 4, 5, 6, 7, 8], o = { a: 1, b: 2 };
    let t = 0;
    for (let i = 0; i < 3000; i++) t = (t + f(o, arr, i)) | 0;
    console.log(JSON.stringify({ grown: process.memoryUsage().rss - before, t }));
  `], { encoding: 'utf8', env: { ...process.env, ANT_DEBUG: debug, NO_COLOR: '1' }, timeout: 120000 });
  const result = run(600);
  assert.strictEqual(result.status, 0, String(result.error || result.stderr));
  const grown = JSON.parse(result.stdout.trim().split('\n').pop()).grown;
  assert.ok(grown < 160 * 1024 * 1024, `compiling 600 locals grew RSS by ${(grown / 1048576).toFixed(1)} MB`);
}

console.log('ok');
