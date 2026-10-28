const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = String.raw`
function nestedLoops(inner, n) {
  if (inner) {
    let i = 0;
    do {
      ++i;
    } while (i < 1);
  }
  let s = 0;
  for (let i = 0; i < n; ++i) {
    s += i;
    if (inner) {
      let j = 0;
      do {
        ++j;
      } while (j < 1);
    }
  }
  return s;
}
nestedLoops(false, 10);
if (nestedLoops(true, 2000000) !== 1999999000000) throw new Error('nested loop result');
if (nestedLoops(true, 400) !== 79800) throw new Error('nested loop rerun result');
console.log('osr-nested-polling: ok');
`;

const result = spawnSync(process.execPath, ['-e', source], {
  env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn', NO_COLOR: '1' },
  encoding: 'utf8',
  timeout: 120000,
});

if (result.error) throw result.error;
assert.strictEqual(result.status, 0, result.stdout + result.stderr);
assert.match(result.stdout, /osr-nested-polling: ok/);
if (!/--jitless/.test(process.execArgv.join(' '))) {
  assert.match(result.stderr, /jit: osr compiled func=nestedLoops /, `the outer loop must enter compiled code:\n${result.stderr}`);
  assert.doesNotMatch(result.stderr, /jit: osr entry-rejected func=nestedLoops /, `OSR polling stuck at a back edge:\n${result.stderr}`);
}
console.log('osr-nested-polling: ok');
