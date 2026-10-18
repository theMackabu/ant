const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = `
function below(n) {
  if (n === 0) throw new Error('deep');
  try { return below(n - 1); } catch (e) { return 'caught at ' + n; }
}
function finallyDepth(n) {
  if (n === 0) throw new Error('deep');
  let out;
  try { out = finallyDepth(n - 1); } finally { if (n === 1) out = 'finally ran'; }
  return out;
}
let r;
try { finallyDepth(400); } catch (e) { r = e.message; }
function unbounded(n) { try { return unbounded(n + 1); } catch (e) { return n; } }
console.log([below(150), below(1000), r, unbounded(0) > 1000].join('|'));
`;
for (const args of [['--jitless', '-e', source], ['-e', source]]) {
  const env = { ...process.env };
  delete env.FORCE_COLOR;
  const run = spawnSync(process.execPath, args, { encoding: 'utf8', env, timeout: 60000 });
  assert.strictEqual(run.status, 0, String(run.error || run.stderr));
  assert.strictEqual(run.stdout.trim(), 'caught at 1|caught at 1|deep|true', args.join(' '));
}
console.log('PASS deep try handlers');
