// string + number and number + string take a direct path that writes the
// digits into the result without a temporary string. It must agree with
// explicit String() conversion for every number shape, operand order, string
// kind (short, long, non-ASCII, rope, builder-built, empty) and length around
// the short-string threshold.
const assert = require('node:assert');

const numbers = [
  0, -0, 1, -1, 7, 42, 1023, -1024, 2147483647, -2147483648, 2147483648, 4294967296,
  0.5, -0.25, 1.5e21, 1e-7, 123456789.125, Number.MAX_SAFE_INTEGER, Number.MIN_VALUE,
  NaN, Infinity, -Infinity,
];

let rope = '';
for (let i = 0; i < 40; i++) rope = rope + 'seg' + (i % 10);
const built = (() => { let s = ''; for (let i = 0; i < 300; i++) s += 'b'; return s; })();
const strings = [
  '', 'k', ':', 'abc', 'café', '🐜', 'lone\ud800',
  'x'.repeat(30), 'x'.repeat(31), 'x'.repeat(32), 'y'.repeat(400), rope, built,
];

function expectConcat(s, n) {
  const digits = String(n);
  const right = s + n, left = n + s;
  assert.strictEqual(right, s.concat(digits), `${JSON.stringify(s.slice(0, 8))} + ${digits}`);
  assert.strictEqual(left, digits.concat(s), `${digits} + ${JSON.stringify(s.slice(0, 8))}`);
  assert.strictEqual(right.length, s.length + digits.length);
  assert.strictEqual(left.length, s.length + digits.length);
  assert.strictEqual(left.charCodeAt(0), (digits + s).charCodeAt(0));
  assert.strictEqual(right.charCodeAt(right.length - 1), (s + digits).charCodeAt(right.length - 1));
}

// enough rounds for the loop to be compiled, so the JIT path is covered too
for (let round = 0; round < 300; round++)
  for (const s of strings) for (const n of numbers) expectConcat(s, n);

let acc = '';
for (let i = 0; i < 2000; i++) acc += i;
let expected = '';
for (let i = 0; i < 2000; i++) expected = expected.concat(String(i));
assert.strictEqual(acc, expected);

assert.strictEqual('v' + -0, 'v0');
assert.strictEqual(1 + 2 + 'a' + 1 + 2, '3a12');
console.log('PASS');
