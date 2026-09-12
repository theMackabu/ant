// Regression for issue #101: a once-called function whose bytecode exceeds
// the old 512-byte OSR gate must still be OSR-compiled from its hot loop,
// on the cheap tier first, and re-tiered hot once it is also hot by calls.
// Promotion is driven by the cold code's own prologue, so it must also
// happen when every call comes from compiled code (direct JIT calls never
// pass through the interpreter's call path).
const { spawnSync } = require('child_process');

const kernel = String.raw`
function benchNbody(n, steps) {
  var bodies = [];
  for (var i = 0; i < n; i++) {
    var r = 10.0 + i * 0.1;
    var v = Math.sqrt(1000.0 / r);
    var ang = (i / n) * 2 * Math.PI;
    bodies.push([r * Math.cos(ang), r * Math.sin(ang), -v * Math.sin(ang), v * Math.cos(ang), 1.0]);
  }
  var dt = 0.001;
  for (var s = 0; s < steps; s++) {
    for (var i = 0; i < n; i++) {
      var bi = bodies[i];
      var ax = 0.0, ay = 0.0;
      for (var j = 0; j < n; j++) {
        if (i === j) continue;
        var bj = bodies[j];
        var dx = bj[0] - bi[0];
        var dy = bj[1] - bi[1];
        var r2 = dx * dx + dy * dy + 1e-9;
        var inv = bj[4] / (r2 * Math.sqrt(r2));
        ax += dx * inv;
        ay += dy * inv;
      }
      bi[2] += ax * dt;
      bi[3] += ay * dt;
    }
    for (var i = 0; i < n; i++) {
      var bi = bodies[i];
      bi[0] += bi[2] * dt;
      bi[1] += bi[3] * dt;
    }
  }
  var e = 0;
  for (var i = 0; i < n; i++) e += bodies[i][0] * bodies[i][0] + bodies[i][1] * bodies[i][1];
  return Math.round(e * 1000) / 1000;
}
`;

const source = kernel + String.raw`
if (benchNbody(300, 30) !== 209259.692) throw new Error('nbody checksum mismatch');
for (let i = 0; i < 150; i++) benchNbody(4, 1);
if (benchNbody(300, 3) !== 209250.582) throw new Error('nbody checksum mismatch after tier-up');
console.log('done');
`;

function run(source) {
  const result = spawnSync(process.execPath, ['-e', source], {
    env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' },
    encoding: 'utf8',
  });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`child failed:\n${result.stderr}\n${result.stdout}`);
  return result.stderr.split('\n');
}

function expectTierUp(lines, label) {
  const osr = lines.find(line => line.startsWith('jit: osr compiled func=benchNbody'));
  if (!osr) throw new Error(`${label}: expected an OSR compile of benchNbody, got:\n${lines.join('\n')}`);
  const size = Number(/code_len=(\d+)/.exec(osr)[1]);
  if (!(size > 512)) throw new Error(`${label}: fixture must exceed the old 512-byte gate, got ${size}`);
  if (!osr.includes('tier=cold')) throw new Error(`${label}: large OSR compile should use the cold tier: ${osr}`);
  if (!lines.some(line => line.startsWith('jit: tier-up compiled func=benchNbody')))
    throw new Error(`${label}: expected a hot re-tier of benchNbody, got:\n${lines.join('\n')}`);
  if (lines.some(line => line.includes('jit: bailout') && line.includes('benchNbody')))
    throw new Error(`${label}: unexpected bailout:\n${lines.join('\n')}`);
}

expectTierUp(run(source), 'interpreter caller');

// Same kernel, but every call after the first comes from a JIT-compiled
// driver that calls benchNbody's code pointer directly.
const compiledCaller = kernel + String.raw`
function compiledDriver() {
  var checksum = 0;
  for (var i = 0; i < 1000; i++) checksum += i;
  checksum += benchNbody(300, 30);
  for (var j = 0; j < 200; j++) checksum += benchNbody(4, 1);
  return checksum;
}
if (Math.round(compiledDriver() * 1000) / 1000 !== 791187.692) throw new Error('compiled driver checksum mismatch');
console.log('done');
`;
expectTierUp(run(compiledCaller), 'compiled caller');

// A single long call: cold code must hand the loop back once this run has
// spent JIT_COLD_PROMOTE_COMPILE_MULTIPLE estimated hot-compile times on
// cold code (~240 ms for this kernel), and the OSR recompile that follows
// must go straight to the hot tier. 800 steps is ~1.05 s cold on an M-series
// laptop, over four times the budget, so a much faster machine still
// promotes. (At 1000 steps ant and node diverge in the last digits: the
// system is chaotic and amplifies a one-ulp libm difference. Every ant tier
// agrees with itself; the checksum below is agreed by node too.)
const longLoop = kernel + String.raw`
if (benchNbody(300, 800) !== 216448.41) throw new Error('long loop checksum mismatch');
console.log('done');
`;
{
  const lines = run(longLoop);
  if (!lines.some(line => line.startsWith('jit: promote func=benchNbody')))
    throw new Error(`long loop: expected an in-loop promotion, got:\n${lines.join('\n')}`);
  if (!lines.some(line => line.startsWith('jit: osr compiled func=benchNbody') && line.includes('tier=hot')))
    throw new Error(`long loop: expected a hot OSR recompile, got:\n${lines.join('\n')}`);
  if (lines.some(line => line.includes('jit: bailout')))
    throw new Error(`long loop: promotion must not count as a bailout:\n${lines.join('\n')}`);
}
console.log('jit-osr-large-function: ok');
