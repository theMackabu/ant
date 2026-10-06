// Compiled String() / String(x) and n.toString() calls skip the native call
// when the callee is the builtin and the argument is a string or number;
// anything else, a replaced builtin included, still calls it. Values live
// on the stack across the call (the array and index of a store) must come
// through either path intact.
const assert = require('node:assert');

const out = new Array(8).fill(null);
function viaString(v, i) { out[i & 7] = String(v); return out[i & 7]; }
function viaEmpty(i) { out[i & 7] = String(); return out[i & 7]; }
function viaToString(n, i) { out[i & 7] = n.toString(); return out[i & 7]; }
function tailString(v) { return String(v); }
function tailEmpty() { return String(); }
function tailToString(n) { return n.toString(); }

for (let i = 0; i < 5000; i++) {
  viaString(i, i); viaString('s' + (i & 3), i); viaEmpty(i); viaToString(i, i);
  tailString(i); tailEmpty(); tailToString(i);
}

const numbers = [0, -0, 1, -1, 42, 123456789, 2147483647, -2147483648, 2147483648,
  1.5, -0.25, 1e21, 1e-7, NaN, Infinity, -Infinity];
for (const [i, n] of numbers.entries()) {
  const expected = `${n}`;
  assert.strictEqual(viaString(n, i), expected);
  assert.strictEqual(viaToString(n, i), expected);
  assert.strictEqual(out[i & 7], expected);
  assert.strictEqual(tailString(n), expected);
  assert.strictEqual(tailToString(n), expected);
}
assert.strictEqual(viaString('kept', 3), 'kept');
assert.strictEqual(viaEmpty(5), '');
assert.strictEqual(tailEmpty(), '');
assert.strictEqual(tailString('t'), 't');
assert.strictEqual(out[5], '');

// other argument types take the call
assert.strictEqual(viaString(Symbol('sym'), 0), 'Symbol(sym)');
assert.strictEqual(viaString({ toString() { return 'obj'; } }, 1), 'obj');
assert.strictEqual(viaString(null, 2), 'null');
assert.strictEqual(viaString(undefined, 3), 'undefined');
assert.strictEqual(viaString(10n, 4), '10');
assert.strictEqual(viaToString(new Number(7), 5), '7');
const wrapped = new Number(8);
wrapped[Symbol.toPrimitive] = () => 'not used';
assert.strictEqual(viaToString(wrapped, 5), '8');
assert.strictEqual(viaToString('str', 6), 'str');

// replaced builtins are called, not bypassed
const realString = String;
globalThis.String = function () { return 'replaced'; };
try {
  assert.strictEqual(viaString(1, 0), 'replaced');
  assert.strictEqual(viaEmpty(1), 'replaced');
  assert.strictEqual(tailString(1), 'replaced');
  assert.strictEqual(tailEmpty(), 'replaced');
} finally {
  globalThis.String = realString;
}
assert.strictEqual(viaString(2, 0), '2');

const realToString = Number.prototype.toString;
Number.prototype.toString = function () { return 'n' + realToString.call(this); };
try {
  assert.strictEqual(viaToString(3, 0), 'n3');
  assert.strictEqual(tailToString(3), 'n3');
} finally {
  Number.prototype.toString = realToString;
}
assert.strictEqual(viaToString(4, 0), '4');
assert.strictEqual((255).toString(16), 'ff');

console.log('PASS compiled String() and toString() calls match the builtins');
