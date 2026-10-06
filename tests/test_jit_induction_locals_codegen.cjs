// Loop counters feeding integer consumers are read as int64 once per block
// (`d2i induction_N`) instead of through the double-to-word32 guard; `%` on a
// counter that only counts up from 0 is an integer remainder, while a counter
// that counts down keeps the double path (-3 % 3 is -0). No bailouts.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = `
const data = Array.from({ length: 64 }, (_, k) => k);
function masked(n) { let s = 0; for (let i = 0; i < n; i++) s += data[i & 63] + (i >>> 1); return s; }
function rem(n) { let s = 0; for (let i = 0; i < n; i++) s += i % 7; return s; }
function by2(n) { let s = 0; for (let i = 0; i < n; i += 2) s += data[i & 63]; return s; }
function by511(n) { let s = 0; for (let i = 0; i < n; i += 511) s += data[i & 63]; return s; }
function by512(n) { let s = 0; for (let i = 0; i < n; i += 512) s += data[i & 63]; return s; }
function remDown(n) { let s = 0; for (let i = n; i > 0; i--) s += i % 7; return s; }
let t = 0;
for (let r = 0; r < 4000; r++) t += masked(64) + rem(64) + remDown(64) + by2(64) + by511(4000) + by512(4000);
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
assert.match(warn.stdout, /total 19196000/);
assert.doesNotMatch(warn.stderr, /jit: bailout [^\n]*func=(masked|rem|remDown)\b/);

const dump = run('dump/vm:jit');
assert.strictEqual(dump.status, 0, String(dump.error || dump.stderr));
const body = (name) => {
  const fn = dump.stderr.match(new RegExp('jit_' + name + '_[^\\n]*:\\s*func[\\s\\S]*?endfunc'));
  assert.ok(fn, 'missing compilation for ' + name);
  return fn[0];
};
assert.match(body('masked'), /\bd2i\s+induction_\d+, ld\d+/, 'counter read as an integer');
assert.doesNotMatch(body('masked'), /\bword_magnitude_\d+\b/, 'no double-to-word32 guard on the counter');
assert.match(body('rem'), /\bmod\s+\w+, \w+, \w+/, 'integer remainder for a counter counting up');
assert.doesNotMatch(body('remDown'), /\bd2i\s+induction_\d+/, 'counting down keeps the double remainder');
assert.match(body('by2'), /\bd2i\s+induction_\d+/, 'i += 2 is a counter');
assert.match(body('by511'), /\bd2i\s+induction_\d+/, 'a step of 511 is a counter');
assert.doesNotMatch(body('by512'), /\bd2i\s+induction_\d+/, 'steps above 511 are not');
console.log('PASS loop counters feeding integer ops are read as integers');
