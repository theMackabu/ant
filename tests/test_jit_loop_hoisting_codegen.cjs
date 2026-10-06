// Compiled code tests a read-only numeric parameter's type once per call:
// the loop reads it through a flag and an unboxed copy, numbers (including
// -Infinity, whose bits are the NaN-box prefix) never bail out, and uses that
// are not numeric keep the boxed value, so other types do not bail either.
// (An unneeded bailout at a parameter read changes no type feedback, so the
// recompile is refused and the function stays interpreted.)
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = `
function count(n) { let s = 0; for (let i = 0; i < n; i++) s++; return s; }
function pick(n, loop) { let s = 0; if (loop) for (let i = 0; i < n; i++) s++; return typeof n + s; }
// x is pushed before ++k: the increment is not a numeric use of x
function walk(x, n) { let k = -1, s = 0; for (let i = 0; i < n; i++) s += x[++k]; return s; }
const xs = [1, 2, 3, 4, 5, 6, 7, 8];
for (let i = 0; i < 3000; i++) { count(20); pick(3, true); walk(xs, 8); }
let total = 0;
for (let i = 0; i < 2000; i++) { total += count(i & 15); total += count(-Infinity); pick('x', false); total += walk(xs, 8) - 36; }
console.log('total ' + total);
`;
const run = (debug) => spawnSync(process.execPath, ['-e', source], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: debug },
  maxBuffer: 32 * 1024 * 1024,
  timeout: 30000,
});

const warn = run('dump/vm:op-warn');
assert.strictEqual(warn.status, 0, String(warn.error || warn.stderr));
assert.match(warn.stdout, /total 15000/);
assert.match(warn.stderr, /jit: compiled func=count/);
assert.match(warn.stderr, /jit: compiled func=pick/);
assert.match(warn.stderr, /jit: compiled func=walk/);
assert.doesNotMatch(warn.stderr, /jit: bailout [^\n]*func=(count|pick|walk)\b/, 'no bailouts for numbers or non-numeric uses');

const dump = run('dump/vm:jit');
assert.strictEqual(dump.status, 0, String(dump.error || dump.stderr));
const fn = dump.stderr.match(/jit_count_[^\n]*:\s*func[\s\S]*?endfunc/);
assert.ok(fn, 'missing compilation for count');
assert.match(fn[0], /\bule\s+parg_num_ok0, parg0,/, 'the type test runs at entry');
assert.match(fn[0], /\bbf\s+L\d+, parg_num_ok0\b/, 'the loop tests the flag');
assert.doesNotMatch(fn[0], /\bmov\s+s\d+, parg0\b/, 'the loop does not reload the boxed parameter');
console.log('PASS read-only numeric parameters are checked once per call');
