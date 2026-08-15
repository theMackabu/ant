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

function diagnosticSentinel(value) {
  return -value;
}

delayedLocals(1);
delayedLocals(1);
if (delayedLocals(10_000) !== 4950) throw new Error('OSR result mismatch');
for (let i = 0; i < 500; i++) diagnosticSentinel(i);
if (diagnosticSentinel({ valueOf() { return 7; } }) !== -7) {
  throw new Error('diagnostic sentinel mismatch');
}
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
const bailoutLines = result.stderr
  .split('\n')
  .filter(line => line.includes('jit: bailout'));
if (!bailoutLines.some(line => line.includes('func=diagnosticSentinel'))) {
  throw new Error(`JIT bailout diagnostics were not observed:\n${result.stderr}`);
}
if (bailoutLines.some(line => line.includes('func=delayedLocals'))) {
  throw new Error(`OSR entry rejection invalidated JIT code:\n${result.stderr}`);
}
if (!result.stdout.includes('jit-osr-entry-reject: ok')) {
  throw new Error(`OSR child produced unexpected output:\n${result.stdout}`);
}

console.log('jit-osr-entry-reject: ok');
