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
  const lines = result.stderr.split('\n');
  lines.stdout = result.stdout;
  return lines;
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

// In-loop promotion. Cold code hands a loop back for a hot recompile once
// it has run JIT_COLD_PROMOTE_COMPILE_MULTIPLE (20) estimated hot compiles'
// worth, the hot compile being taken as JIT_HOT_COMPILE_COLD_RATIO (3) times
// the cold compile's own measured duration: the budget is 60x the `ms=` the
// engine logs for the cold compile. The trigger is wall-clock, so each child
// reports its elapsed time and the promotion assertions apply only when the
// run demonstrably exceeded twice the budget; otherwise the case still checks
// its result and reports the skip. This fixture keeps locals to a minimum so
// its cold compile, and with it the budget, stays small (~11 ms, ~660 ms).
const leanKernel = String.raw`
function lean(n) { var s = 1; for (var i = 0; i < n; i++) s = (s * 3 + i) | 0; ` +
  Array.from({ length: 70 }, () => 's = (s ^ (s << 1)) | 0;').join(' ') + String.raw` return s; }
`;

function expectPromotion(lines, label) {
  const cold = lines.find(line => line.startsWith('jit: compiled func=lean') && line.includes('tier=cold'));
  if (!cold) throw new Error(`${label}: expected a cold compile of lean, got:\n${lines.join('\n')}`);
  const budgetMs = 60 * Number(/ms=([\d.]+)/.exec(cold)[1]);
  const elapsedMs = Number(/elapsed (\d+)/.exec(lines.stdout)[1]);
  if (elapsedMs < 2 * budgetMs) {
    console.log(`${label}: ran ${elapsedMs} ms against a ${budgetMs.toFixed(0)} ms budget, promotion not asserted`);
    return;
  }
  if (!lines.some(line => line.startsWith('jit: promote func=lean')))
    throw new Error(`${label}: expected an in-loop promotion after ${elapsedMs} ms, got:\n${lines.join('\n')}`);
  if (!lines.some(line => line.startsWith('jit: osr compiled func=lean') && line.includes('tier=hot')))
    throw new Error(`${label}: expected a hot OSR recompile, got:\n${lines.join('\n')}`);
  if (lines.some(line => line.includes('jit: bailout')))
    throw new Error(`${label}: promotion must not count as a bailout:\n${lines.join('\n')}`);
}

// A single long call: 80M iterations is ~1.3 s cold on an M-series laptop.
const longLoop = leanKernel + String.raw`
var t0 = Date.now();
if (lean(80000000) !== 176640597) throw new Error('long loop checksum mismatch');
console.log('elapsed ' + (Date.now() - t0));
`;
expectPromotion(run(longLoop), 'long loop');

// Repeated calls that are each shorter than the budget must still promote:
// cold time is banked on the function across activations, so three calls
// of ~400 ms each promote during the second.
const repeated = leanKernel + String.raw`
var t0 = Date.now();
for (var k = 0; k < 3; k++)
  if (lean(25000000) !== -763526411) throw new Error('repeated call checksum mismatch');
console.log('elapsed ' + (Date.now() - t0));
`;
expectPromotion(run(repeated), 'repeated calls');
console.log('jit-osr-large-function: ok');
