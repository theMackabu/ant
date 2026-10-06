// Compiled bitwise ops on non-number primitives (undefined, null, booleans,
// strings, BigInt) are computed in the helper. They used to bail out every
// time; the bailout changed no type feedback, so the recompile was refused
// and the function stayed interpreted (`a[i] | 0` reading past the end).
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

const ops = {
  and: (a, b) => a & b, or: (a, b) => a | b, xor: (a, b) => a ^ b,
  shl: (a, b) => a << b, shr: (a, b) => a >> b, ushr: (a, b) => a >>> b,
  not: (a) => ~a,
};
const reference = {
  and: (a, b) => a & b, or: (a, b) => a | b, xor: (a, b) => a ^ b,
  shl: (a, b) => a << b, shr: (a, b) => a >> b, ushr: (a, b) => a >>> b,
  not: (a) => ~a,
};
const outcome = (f, a, b) => { try { return f(a, b); } catch (e) { return e.constructor.name; } };

for (let i = 0; i < 5000; i++) for (const f of Object.values(ops)) f(i, 3);

const values = [undefined, null, true, false, '12', ' -7 ', 'x', '', 3.7, -0, NaN, 2 ** 32 + 5, -1];
for (const [name, f] of Object.entries(ops))
  for (const a of values)
    for (const b of [undefined, null, true, '3', 2, NaN])
      same(outcome(f, a, b), reference[name](a, b), `${name}(${String(a)}, ${String(b)})`);

// BigInt pairs compute; mixing throws TypeError
for (const [name, f] of Object.entries(ops)) {
  if (name === 'ushr') {
    same(outcome(f, 5n, 1n), 'TypeError', 'ushr bigint');
    continue;
  }
  same(outcome(f, 6n, 3n), name === 'not' ? -7n : reference[name](6n, 3n), `${name} bigint`);
  if (name !== 'not') same(outcome(f, 6n, 3), 'TypeError', `${name} mixed bigint`);
}
// objects still convert through valueOf
let calls = 0;
const boxed = { valueOf() { calls++; return 6; } };
same(ops.or(boxed, 1), 7, 'valueOf operand');
same(calls, 1, 'valueOf called once');

// the past-the-end loop stays compiled
const child = spawnSync(process.execPath, ['-e', `
function f(a, n) { let s = 0; for (let i = 0; i < n; i++) s += (a[i] | 0) + (~a[i] >>> 0) + (a[i] ^ true); return s; }
const full = Array.from({ length: 64 }, (_, k) => k);
let t = 0;
for (let r = 0; r < 3000; r++) t += f(full, 64);
for (let r = 0; r < 3000; r++) t += f(full, 80);
console.log('total ' + t);
`], { encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn' }, timeout: 30000 });
assert.strictEqual(child.status, 0, String(child.error || child.stderr));
assert.match(child.stdout, /total -?\d+/);
const bailouts = child.stderr.match(/jit: bailout [^\n]*func=f\b/g) || [];
assert.ok(bailouts.length <= 2, 'at most the feedback-driven bailouts, got ' + bailouts.length + '\n' + bailouts.join('\n'));
assert.doesNotMatch(child.stderr, /compile-failed func=f\b|jit: disabling f\b/);

console.log('PASS compiled bitwise ops handle non-number primitives without bailing');
