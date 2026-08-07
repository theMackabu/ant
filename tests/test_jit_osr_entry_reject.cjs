const { spawnSync } = require('child_process');

const source = String.raw`
function delayedLocals(n) {
  var values = [];
  var i, j, sum;
  for (i = 0; i < 100; i++) values[i] = i;
  for (j = 0; j < n; j++) {
    sum = 0;
    for (i = 0; i < 100; i++) sum += values[i];
  }
  return sum;
}

delayedLocals(1);
delayedLocals(1);
if (delayedLocals(10_000) !== 4950) throw new Error('OSR result mismatch');
console.log('jit-osr-entry-reject: ok');
`;

const result = spawnSync(process.execPath, ['-e', source], {
  env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' },
  encoding: 'utf8',
});

if (result.error) throw result.error;
if (result.status !== 0) {
  throw new Error(`OSR child failed:\n${result.stderr}`);
}
if (result.stderr.includes('jit: bailout')) {
  throw new Error(`OSR entry rejection invalidated JIT code:\n${result.stderr}`);
}
if (!result.stdout.includes('jit-osr-entry-reject: ok')) {
  throw new Error(`OSR child produced unexpected output:\n${result.stdout}`);
}

console.log('jit-osr-entry-reject: ok');
