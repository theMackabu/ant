// ToNumeric must propagate errors: a throwing valueOf, and Symbols (TypeError).
// Arithmetic and bitwise operators used to swallow both (NaN), interpreted and
// compiled; Number(), Math.* and isNaN missed the Symbol case. Compiled
// arithmetic on non-number primitives (`a[i] * 2` past the end) also used to
// bail out on every call and leave the function interpreted.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const ops = {
  sub: (a) => a - 1, mul: (a) => a * 2, div: (a) => a / 2, mod: (a) => a % 3, exp: (a) => a ** 2,
  and: (a) => a & 3, or: (a) => a | 0, xor: (a) => a ^ 1, shl: (a) => a << 1, shr: (a) => a >> 1,
  ushr: (a) => a >>> 1, not: (a) => ~a, neg: (a) => -a, plus: (a) => +a, lt: (a) => a < 1, ge: (a) => a >= 1,
  rsub: (a) => 10 - a, ror: (a) => 7 | a,
};
const throwing = { valueOf() { throw new Error('boom'); } };
const toBigInt = { valueOf() { return 4n; } };
const outcome = (f, v) => {
  try { return f(v); } catch (e) { return e instanceof TypeError ? 'TypeError' : e.message; }
};

function check(when) {
  for (const [name, f] of Object.entries(ops)) {
    assert.strictEqual(outcome(f, throwing), 'boom', `${when} ${name} with throwing valueOf`);
    assert.strictEqual(outcome(f, Symbol('s')), 'TypeError', `${when} ${name} with a Symbol`);
  }
  // valueOf returning a BigInt is a BigInt operand
  assert.strictEqual(outcome(ops.neg, toBigInt), -4n, `${when} -bigint object`);
  assert.strictEqual(outcome(ops.mul, toBigInt), 'TypeError', `${when} bigint object * number`);
  assert.strictEqual(outcome(ops.plus, toBigInt), 'TypeError', `${when} +bigint object`);
  assert.strictEqual(outcome(ops.lt, toBigInt), false, `${when} bigint object < 1`);
  // primitives
  for (const [v, sub, mul] of [[undefined, NaN, NaN], [null, -1, 0], [true, 0, 2], ['5', 4, 10], ['', -1, 0]]) {
    assert.ok(Object.is(ops.sub(v), sub), `${when} ${String(v)} - 1`);
    assert.ok(Object.is(ops.mul(v), mul), `${when} ${String(v)} * 2`);
  }
}

check('interpreter');
for (let i = 0; i < 5000; i++) for (const f of Object.values(ops)) f(i);
check('compiled');

for (const f of [Number, Math.abs, Math.floor, isNaN, isFinite])
  assert.strictEqual(outcome(f, Symbol('s')), 'TypeError', `${f.name}(Symbol)`);
assert.strictEqual(outcome(Number, throwing), 'boom', 'Number(throwing)');

const child = spawnSync(process.execPath, ['-e', `
function f(a, n) { let s = 0; for (let i = 0; i < n; i++) s += (a[i] * 2 || 0) + ((a[i] - 1) || 0) + (a[i] % 5 || 0); return s; }
const full = Array.from({ length: 64 }, (_, k) => k);
let t = 0;
for (let r = 0; r < 3000; r++) t += f(full, 64);
for (let r = 0; r < 3000; r++) t += f(full, 80);
console.log('total ' + t);
`], { encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' }, timeout: 30000 });
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /total \d+/);
const bailouts = child.stderr.match(/jit: bailout [^\n]*func=f\b/g) || [];
assert.ok(bailouts.length <= 2, 'at most the feedback-driven bailouts, got ' + bailouts.length);
assert.doesNotMatch(child.stderr, /compile-failed func=f\b|jit: disabling f\b/);

console.log('PASS numeric conversion errors propagate and arithmetic on primitives stays compiled');
