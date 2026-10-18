const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = `
function parse(n) {
  let total = 0;
  {
    let count = 0;
    for (let k = 0; k < n; k++) count += k;
    total += count;
  }
  {
    let node;
    node = { kind: 'node', size: total };
    total += node.size;
  }
  return total;
}
let t = 0;
for (let r = 0; r < 3; r++) t += parse(200000);
console.log('total ' + t);
`;
const env = { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' };
delete env.FORCE_COLOR;
const run = spawnSync(process.execPath, ['-e', source], { encoding: 'utf8', env, timeout: 30000 });
assert.strictEqual(run.status, 0, String(run.error || run.stderr));
assert.match(run.stdout, /total 119999400000/);
assert.doesNotMatch(run.stderr, /jit: bailout [^\n]*func=parse\b/, 'a slot shared by two lets is not speculated numeric');
console.log('PASS shared let slots stay boxed');
