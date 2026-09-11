// Regression for issue #101: a once-called function whose bytecode exceeds
// the old 512-byte OSR gate must still be OSR-compiled from its hot loop,
// on the cheap tier first, and re-tiered hot once it is also hot by calls.
const { spawnSync } = require('child_process');

const source = String.raw`
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
if (benchNbody(300, 30) !== 209259.692) throw new Error('nbody checksum mismatch');
for (let i = 0; i < 150; i++) benchNbody(4, 1);
if (benchNbody(300, 3) !== 209250.582) throw new Error('nbody checksum mismatch after tier-up');
console.log('done');
`;

const result = spawnSync(process.execPath, ['-e', source], {
  env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' },
  encoding: 'utf8',
});

if (result.error) throw result.error;
if (result.status !== 0) throw new Error(`child failed:\n${result.stderr}\n${result.stdout}`);

const lines = result.stderr.split('\n');
const osr = lines.find(line => line.startsWith('jit: osr compiled func=benchNbody'));
if (!osr) throw new Error(`expected an OSR compile of benchNbody, got:\n${result.stderr}`);
const size = Number(/code_len=(\d+)/.exec(osr)[1]);
if (!(size > 512)) throw new Error(`fixture must exceed the old 512-byte gate, got ${size}`);
if (!osr.includes('tier=cold')) throw new Error(`large OSR compile should use the cold tier: ${osr}`);
if (!lines.some(line => line.startsWith('jit: tier-up compiled func=benchNbody')))
  throw new Error(`expected a hot re-tier of benchNbody after 150 calls, got:\n${result.stderr}`);
if (lines.some(line => line.includes('jit: bailout') && line.includes('benchNbody')))
  throw new Error(`unexpected bailout:\n${result.stderr}`);
console.log('jit-osr-large-function: ok');
