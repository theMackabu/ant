// A hot loop followed by numeric locals declared after it. Those locals are
// still undefined (var) or in their TDZ (let) at the loop head, and the OSR
// entry used to reject the frame for it on every attempt, leaving the whole
// loop to the interpreter (14x slower than the compiled loop). The entry now
// accepts both, exactly as a normal entry would, so the loop must OSR-enter
// without a single rejection or bailout.
const { spawnSync } = require('child_process');

function fixture(decl) {
  return String.raw`
function late(n) {
  var x = 0.5, y = 1.25;
  for (var i = 0; i < n; i++) { x = x * 1.0000001 + 0.001; y = y + x * 0.000001; }
  ${decl} s = 0;
  ${decl} p0 = s + 1, p1 = p0 * 2, p2 = p1 - 3, p3 = p2 * 4;
  ${decl} z = ((p0 + p1 + p2 + p3) * 0) | 0;
  return Math.round((x + y) * 1000) / 1000 + z;
}
if (late(3000000) !== 8488.144) throw new Error('late-local checksum mismatch');
console.log('done');
`;
}

function run(source, label) {
  const result = spawnSync(process.execPath, ['-e', source], {
    env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' },
    encoding: 'utf8',
  });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${label}: child failed:\n${result.stderr}\n${result.stdout}`);
  const lines = result.stderr.split('\n');
  if (!lines.some(line => line.startsWith('jit: osr compiled func=late')))
    throw new Error(`${label}: expected an OSR compile of late, got:\n${result.stderr}`);
  if (lines.some(line => line.startsWith('jit: osr entry-rejected func=late')))
    throw new Error(`${label}: OSR entry rejected the late locals:\n${result.stderr}`);
  if (lines.some(line => line.includes('jit: bailout') && line.includes('func=late')))
    throw new Error(`${label}: unexpected bailout:\n${result.stderr}`);
}

run(fixture('var'), 'var after loop');
run(fixture('let'), 'let after loop');
console.log('jit-osr-late-locals: ok');
