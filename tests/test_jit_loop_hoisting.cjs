// Compiled code checks and unboxes a read-only numeric parameter once per
// call; each numeric use only tests a flag and bails out when the argument was
// not a number. `o[k] = v` keeps a numeric v unboxed for the store and for the
// expression's value. Slow paths are laid out after the function body.
function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

function count(n) { let s = 0; for (let i = 0; i < n; i++) s++; return s; }
function scaled(n, k) { let s = 0; for (let i = 0; i < n; i++) s += i * k; return s; }
function mixed(n) { let s = 0; for (let i = 0; i < n; i++) s += i; return `${n}:${s}`; }
function bound(lo, hi) { let s = 0; for (let i = lo; i <= hi; i++) s += i; return s; }

for (let i = 0; i < 3000; i++) { count(50); scaled(20, 0.5); mixed(10); bound(-5, 5); }

same(count(1000), 1000, 'number');
same(count(0), 0, 'zero');
same(count(-0), 0, '-0');
same(count(NaN), 0, 'NaN');
same(count(2.5), 3, 'fraction');
same(count(-Infinity), 0, '-Infinity');
same(count('7'), 7, 'numeric string');
same(count({ valueOf: () => 4 }), 4, 'valueOf');
same(count(), 0, 'missing argument');
same(count(undefined), 0, 'undefined');
same(count(null), 0, 'null');
same(count(true), 1, 'boolean');
same(count(3n), 3, 'bigint');
same(scaled(4, 2), 12, 'scaled');
same(scaled(4, -0), 0, 'scaled by -0');
same(scaled(4, '2'), 12, 'scaled by string');
same(scaled(4, NaN), NaN, 'scaled by NaN');
same(mixed(5), '5:10', 'mixed');
same(mixed('5'), '5:10', 'mixed string');
same(bound(1, 4), 10, 'bound');
same(bound('1', 4), '01234', 'string lower bound');
same(bound(1, '4'), 10, 'string upper bound');
// back on numbers after the bailouts
for (let i = 0; i < 3000; i++) count(50);
same(count(1000), 1000, 'number again');

// o[k] = v with a numeric v, as a statement and as an expression
const out = new Array(16).fill(0);
function put(i, v) { out[i & 15] = v * 1; }
function putValue(i, v) { return (out[i & 15] = v * 1); }
function putChain(i, v) { let a, b; a = b = out[i & 15] = v + 0; return a + b; }
function putObject(o, k, v) { return (o[k] = v / 1); }
for (let i = 0; i < 3000; i++) { put(i, i * 0.5); putValue(i, i); putChain(i, i); putObject({}, 'x', i); }

for (const v of [1.5, -0, NaN, Infinity, 2 ** 53, -1e-300]) {
  put(3, v);
  same(out[3], v, `put ${v}`);
  same(putValue(5, v), v, `putValue ${v}`);
  same(out[5], v, `putValue stored ${v}`);
  same(putChain(7, v), (v + 0) * 2, `putChain ${v}`);
  same(out[7], v + 0, `putChain stored ${v}`);
  const o = {};
  same(putObject(o, 'x', v), v, `putObject ${v}`);
  same(o.x, v, `putObject stored ${v}`);
}
const typed = new Float64Array(4);
same(putObject(typed, 1, 2.25), 2.25, 'typed array value');
same(typed[1], 2.25, 'typed array stored');
const frozen = Object.freeze([1, 2, 3]);
same(putObject(frozen, 1, 9), 9, 'frozen value');
same(frozen[1], 2, 'frozen unchanged');

// a hole whose prototype has a setter sees the number
const seen = [];
const proto = Object.create(Array.prototype);
Object.defineProperty(proto, 2, { set(v) { seen.push(v); }, configurable: true });
const holey = [0, 1, , 3];
Object.setPrototypeOf(holey, proto);
same(putObject(holey, 2, -0), -0, 'setter expression value');
same(seen.length, 1, 'setter called');
same(seen[0], -0, 'setter argument');
same(Object.hasOwn(holey, 2), false, 'hole stays a hole');

console.log('PASS hoisted parameter checks and numeric stores match the interpreter');
