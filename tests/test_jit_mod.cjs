// % in JIT code. An exact non-negative integer dividend with a non-zero
// integer divisor takes an integer remainder inline; every other number pair
// goes through fmod; non-numbers bail to the interpreter. The results must
// match the interpreter bit for bit (Object.is, so -0 and NaN count), and the
// numeric shapes must never bail.
const { spawnSync } = require('child_process');

const source = String.raw`
function m(a, b) { return a % b; }
function mixed(a, b) { return a % b; }
function ir(n) { var s = 0; for (var i = 0; i < n; i++) s = (s + (i & 1023)) % 977; return s; }
function neg(n) { var s = 0; for (var i = 0; i < n; i++) s = ((i & 255) - 128) % 7 + s; return s; }
function frac(n) { var s = 0; for (var i = 0; i < n; i++) s += (i * 0.37) % 1.5; return Math.round(s * 1000) / 1000; }

var cases = [[7,3],[-7,3],[7,-3],[-7,-3],[0,5],[-0,5],[0,-5],[5,0],[-5,0],[0,0],[5.5,2],[-5.5,2],[7,2.5],
  [1e17,3],[9007199254740993,7],[9007199254740992,7],[9007199254740994,7],[1152921504606846976,7],[Infinity,3],[3,Infinity],[-3,Infinity],
  [NaN,3],[3,NaN],[2**31,-1],[6,3],[-6,3],[6,-3],[1,1e-9],[123456789012,1000003],[4,2],[-4,2]];
var expected = cases.map(function (c) { return c[0] % c[1]; });
for (var w = 0; w < 2000; w++)
  for (var k = 0; k < cases.length; k++) m(cases[k][0], cases[k][1]);
for (var k = 0; k < cases.length; k++)
  if (!Object.is(m(cases[k][0], cases[k][1]), expected[k]))
    throw new Error('mismatch for ' + cases[k] + ': ' + m(cases[k][0], cases[k][1]) + ' vs ' + expected[k]);

var others = [['7',3],[7,'3'],[null,3],[undefined,3],[true,2],[{},3],[[9],4],['abc',2]];
for (var k = 0; k < others.length; k++)
  if (!Object.is(mixed(others[k][0], others[k][1]), others[k][0] % others[k][1]))
    throw new Error('mixed mismatch for ' + others[k]);

for (var w = 0; w < 300; w++) { ir(50); neg(50); frac(50); }
var e1 = 0; for (var i = 0; i < 100000; i++) e1 = (e1 + (i & 1023)) % 977;
if (ir(100000) !== e1) throw new Error('integer loop mismatch');
var e2 = 0; for (var i = 0; i < 100000; i++) e2 = ((i & 255) - 128) % 7 + e2;
if (neg(100000) !== e2) throw new Error('negative dividend loop mismatch');
var e3 = 0; for (var i = 0; i < 100000; i++) e3 += (i * 0.37) % 1.5;
if (frac(100000) !== Math.round(e3 * 1000) / 1000) throw new Error('fractional loop mismatch');
console.log('done');
`;

const result = spawnSync(process.execPath, ['-e', source], {
  env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' },
  encoding: 'utf8',
});
if (result.error) throw result.error;
if (result.status !== 0) throw new Error(`child failed:\n${result.stderr}\n${result.stdout}`);
const lines = result.stderr.split('\n');
for (const name of ['m', 'ir', 'neg', 'frac']) {
  if (!lines.some(line => line.startsWith(`jit: compiled func=${name} `)))
    throw new Error(`expected ${name} to be compiled:\n${result.stderr}`);
  if (lines.some(line => line.includes('jit: bailout') && line.includes(`func=${name} `)))
    throw new Error(`numeric % must not bail in ${name}:\n${result.stderr}`);
}
console.log('jit-mod: ok');
