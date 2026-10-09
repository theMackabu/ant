// A comparison that feeds a branch is compiled as one compare-and-branch, on
// doubles, or on integers when both sides are integer slots. "Jump if false"
// on doubles must still jump for NaN, where neither order holds. Results must
// match the interpreter for every operator, direction and operand kind.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = `
const vals = [0, -0, 1, -1, 2, 0.5, -0.5, 7, 1e9, -1e9, 2 ** 31, Infinity, -Infinity, NaN];
function ops(a, b) {
  let m = 0;
  if (a < b) m |= 1;
  if (a <= b) m |= 2;
  if (a > b) m |= 4;
  if (a >= b) m |= 8;
  if (!(a < b)) m |= 16;
  if (!(a <= b)) m |= 32;
  if (!(a > b)) m |= 64;
  if (!(a >= b)) m |= 128;
  return m;
}
function intLoops(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s += i;
  for (let i = n; i > 0; i--) s += 2;
  for (let i = 0; i <= 10; i++) s += 3;
  for (let i = 10; i >= -3; i--) s += i;
  let j = 0; while (!(j >= 50)) j += 3;
  return s + j;
}
function intPairs() {
  let m = 0;
  for (let i = -3; i < 40; i++) {
    const a = i & 7, b = (i * 3) & 7;
    if (a < b) m += 1; if (a <= b) m += 10; if (a > b) m += 100; if (a >= b) m += 1000;
    if (!(a < b)) m += 3; if (!(a <= b)) m += 5; if (!(a > b)) m += 7; if (!(a >= b)) m += 9;
  }
  return m;
}
let out;
for (let r = 0; r < 300; r++) {
  out = [];
  for (const a of vals) for (const b of vals) out.push(ops(a, b));
  out.push(intLoops(100 + r % 3), intPairs());
}
console.log(out.join(','));
`;
const run = args => spawnSync(process.execPath, [...args, '-e', source], {
  encoding: 'utf8', env: { ...process.env, NO_COLOR: '1' }, timeout: 60000,
});
const compiled = run([]);
const interpreted = run(['--jitless']);
assert.strictEqual(compiled.status, 0, compiled.stderr);
assert.strictEqual(interpreted.status, 0, interpreted.stderr);
assert.strictEqual(compiled.stdout, interpreted.stdout);
console.log('PASS compiled compare-and-branch matches the interpreter');
