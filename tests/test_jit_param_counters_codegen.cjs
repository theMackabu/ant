// Counter parameters read as integers (`parg_counter_N` copies of the
// double); a non-integer argument fails the entry check once, and the
// function recompiles without the speculation instead of bailing forever.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = `
const src = Array.from({ length: 64 }, (_, k) => k);
function two(i, j, n) { let s = 0; while (n-- > 0) s += src[i++ & 63] + src[j++ & 63]; return s; }
function walk(i, n) { let s = 0; while (n-- > 0) s += src[i++ & 63] + (i & 7); return s; }
let t = 0;
for (let r = 0; r < 4000; r++) t += walk(r & 31, 20);
for (let r = 0; r < 4000; r++) t += walk((r & 31) + 0.5, 20);
for (let r = 0; r < 4000; r++) t += two(r & 31, r & 15, 20);
for (let r = 0; r < 4000; r++) t += two((r & 31) + 0.5, r & 15, 20);
console.log('total ' + t);
`;
const run = (debug) => spawnSync(process.execPath, ['-e', source], {
  encoding: 'utf8',
  env: { ...process.env, ANT_DEBUG: debug },
  maxBuffer: 64 * 1024 * 1024,
  timeout: 30000,
});

const warn = run('dump/vm:op-warn');
assert.strictEqual(warn.status, 0, String(warn.error || warn.stderr));
const bailouts = warn.stderr.match(/jit: bailout [^\n]*func=walk\b/g) || [];
assert.ok(bailouts.length <= 2, 'one bailout per compile at most, got ' + bailouts.length);
assert.doesNotMatch(warn.stderr, /jit: disabling walk/);

const dump = run('dump/vm:jit');
assert.strictEqual(dump.status, 0, String(dump.error || dump.stderr));
const compiles = dump.stderr.match(/jit_walk_[^\n]*:\s*func[\s\S]*?endfunc/g) || [];
assert.ok(compiles.length >= 2, 'recompiled after the non-integer argument');
assert.match(compiles[0], /\bd2i\s+parg_counter_\d+, parg_num0\b/, 'the counter is read as an integer');
assert.match(compiles[0], /\bdbne\s+L\d+, parg_num0, parg_back0\b/, 'entry checks the argument is an integer');
assert.doesNotMatch(compiles[compiles.length - 1], /parg_counter_/, 'the recompile drops the speculation');
const twos = dump.stderr.match(/jit_two_[^\n]*:\s*func[\s\S]*?endfunc/g) || [];
assert.ok(twos.length >= 2, 'two recompiled after the non-integer argument');
const lastTwo = twos[twos.length - 1];
assert.doesNotMatch(lastTwo, /\bd2i\s+parg_counter_\d+, parg_num0\b/, 'the failing parameter drops the speculation');
assert.match(lastTwo, /\bd2i\s+parg_counter_\d+, parg_num1\b/, 'the other parameter keeps it');
assert.match(warn.stdout, /total \d+/);
console.log('PASS counter parameters are read as integers and fall back once');
