// Compiled loops read counters (locals written only by integer constants,
// `i++` / `i--` and copies of such locals) as integers where an integer is
// wanted: array keys, bitwise ops, `%` on non-negative counters. Results must
// match the double semantics exactly, including -0, negative counters, values
// past 2^31 / 2^32, and locals that only look like counters.
function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}
function sameList(actual, expected, what) {
  same(actual.length, expected.length, `${what} length`);
  for (let k = 0; k < expected.length; k++) same(actual[k], expected[k], `${what}[${k}]`);
}

const data = Array.from({ length: 64 }, (_, k) => k * 3 + 1);

function sumIndexed(n) { let s = 0; for (let i = 0; i < n; i++) s += data[i]; return s; }
function sumMasked(n) { let s = 0; for (let i = 0; i < n; i++) s += data[i & 63] + (i % 7) + (i >>> 1); return s; }
function fill(out, n) { for (let i = 0; i < n; i++) out[i & 15] = i; return out; }
function fillValue(out, n) { let last; for (let i = 0; i < n; i++) last = (out[i % 16] = i); return last; }
function down(n) { const r = []; for (let i = n; i >= -n; i--) r.push(data[i], i & 7, i % 3, i | 0); return r; }
function high() { const r = []; for (let i = 4294967290; i < 4294967300; i++) r.push(i & 255, i | 0, i >>> 0, i % 1000, data[i]); return r; }
function copies(n) { let s = 0; for (let i = 0; i < n; i++) { const j = i; let k = j; s += data[k & 63] + (k % 5); } return s; }
// a join brings a non-integer into what otherwise looks like a counter
function joined(c, y) { let x = c ? y : 5; let s = 0; for (let i = 0; i < 4; i++) { s += (x & 3) + (data[x] === undefined ? 100 : 0); x++; } return s; }
// counters that start from constants (parameters never qualify)
function downConst() { const r = []; for (let i = 4; i >= -4; i--) r.push(data[i], i & 7, i % 3); return r; }
function joinedConst(c) { let x = c ? 1.5 : 5; let s = 0; for (let i = 0; i < 4; i++) { s += (x & 3) + (data[x] === undefined ? 100 : 0) + (x % 2); x++; } return s; }
function stepHalf() { const r = []; for (let i = 0; i < 2; i += 0.5) r.push(data[i], i & 1, i % 1); return r; }
function overwritten(n) { let x = 0; const r = []; for (let i = 0; i < n; i++) { r.push(data[x], x & 3, x % 2); x = i * 0.5; } return r; }
// constant steps: `i += k` (ADD_LOCAL) and `i = i - k`
function by4(n) { let s = 0; for (let i = 0; i < n; i += 4) s += data[i & 63] + (i % 7); return s; }
function down5(n) { const r = []; for (let i = 12; i > -n; i = i - 5) r.push(data[i], i & 7, i % 3); return r; }
function by512(n) { let s = 0; for (let i = 0; i < n; i += 512) s += (i & 1023) + data[i & 63]; return s; }
function negZero() { const r = []; for (let z = -0; z < 2; z++) r.push(1 / z, z % 2, data[z]); return r; }
function counted(n) { let k = 0; for (let i = 0; i < n; i++) k++; return [k & 1023, k % 10, data[k & 63]]; }

const refSum = (n) => { let s = 0; for (let i = 0; i < n; i++) s += data[i] === undefined ? NaN : data[i]; return s; };

for (let r = 0; r < 4000; r++) {
  sumIndexed(64); sumMasked(100); fill(new Array(16).fill(0), 40); fillValue(new Array(16).fill(0), 40);
  down(4); copies(50); joined(r & 1, 2); counted(20);
  downConst(); joinedConst(r & 1); stepHalf(); overwritten(4);
  by4(64); down5(10); by512(4096);
}
high(); negZero();

same(sumIndexed(64), refSum(64), 'indexed sum');
same(sumIndexed(70), NaN, 'reading past the end');
same(sumMasked(1000), (() => { let s = 0; for (let i = 0; i < 1000; i++) s += data[i % 64] + (i % 7) + Math.floor(i / 2); return s; })(), 'masked sum');
sameList(fill(new Array(16).fill(0), 40), Array.from({ length: 16 }, (_, k) => k + 32 - (k + 32 >= 40 ? 16 : 0)), 'fill');
same(fillValue(new Array(16).fill(0), 41), 40, 'store expression value');
sameList(down(4), [
  13, 4, 1, 4, 10, 3, 0, 3, 7, 2, 2, 2, 4, 1, 1, 1, 1, 0, 0, 0,
  undefined, 7, -1, -1, undefined, 6, -2, -2, undefined, 5, -0, -3, undefined, 4, -1, -4,
], 'counting down past zero');
sameList(high(), [
  250, -6, 4294967290, 290, undefined, 251, -5, 4294967291, 291, undefined, 252, -4, 4294967292, 292, undefined,
  253, -3, 4294967293, 293, undefined, 254, -2, 4294967294, 294, undefined, 255, -1, 4294967295, 295, undefined,
  0, 0, 0, 296, undefined, 1, 1, 1, 297, undefined, 2, 2, 2, 298, undefined, 3, 3, 3, 299, undefined,
], 'counter past 2^32');
same(copies(50), (() => { let s = 0; for (let i = 0; i < 50; i++) s += data[i & 63] + (i % 5); return s; })(), 'copied counters');
same(joined(true, 1.5), (1 + 100) + (2 + 100) + (3 + 100) + (0 + 100), 'fractional value through a join');
same(joined(false, 1.5), (5 & 3) + (6 & 3) + (7 & 3) + (8 & 3), 'integer start through a join');
same(joined(true, -0.5), (0 + 100) + (0 + 100) + (1 + 100) + (2 + 100), 'negative fraction through a join');
sameList(downConst(), [
  13, 4, 1, 10, 3, 0, 7, 2, 2, 4, 1, 1, 1, 0, 0,
  undefined, 7, -1, undefined, 6, -2, undefined, 5, -0, undefined, 4, -1,
], 'counting down from a constant');
same(joinedConst(true), (1 + 100 + 1.5) + (2 + 100 + 0.5) + (3 + 100 + 1.5) + (0 + 100 + 0.5), 'fraction joined with an integer');
same(joinedConst(false), (1 + 0 + 1) + (2 + 0 + 0) + (3 + 0 + 1) + (0 + 0 + 0), 'integer joined with a fraction');
sameList(stepHalf(), [1, 0, 0, undefined, 0, 0.5, 4, 1, 0, undefined, 1, 0.5], 'half steps');
sameList(overwritten(4), [1, 0, 0, 1, 0, 0, undefined, 0, 0.5, 4, 1, 1], 'counter overwritten by arithmetic');
same(by4(1000), (() => { let s = 0; for (let i = 0; i < 1000; i += 4) s += data[i & 63] + (i % 7); return s; })(), 'i += 4');
sameList(down5(10), (() => { const r = []; for (let i = 12; i > -10; i -= 5) r.push(data[i], i & 7, i % 3); return r; })(), 'i = i - 5 past zero');
same(by512(100000), (() => { let s = 0; for (let i = 0; i < 100000; i += 512) s += (i & 1023) + data[i & 63]; return s; })(), 'i += 512');
sameList(negZero(), [-Infinity, -0, 1, 1, 1, 4], 'counter starting at -0');
sameList(counted(1000), [1000 & 1023, 0, data[1000 & 63]], 'counter read after the loop');

// a bailout in the middle of a loop resumes with the right counter
function bail(arr, n) { let s = 0; for (let i = 0; i < n; i++) { s += arr[i & 7] + (i % 3); } return s; }
for (let r = 0; r < 4000; r++) bail([1, 2, 3, 4, 5, 6, 7, 8], 16);
const mixed = [1, 2, 3, 4, 5, 6, 7, 8];
mixed[5] = 'x';
// called once, so it stays in the interpreter
const bailRef = (arr, n) => { let s = 0; for (let i = 0; i < n; i++) { s += arr[i & 7] + (i % 3); } return s; };
same(bail(mixed, 16), bailRef(mixed, 16), 'bailout mid-loop');

// OSR: entered while the counter is already large
function longLoop(n) { let s = 0; for (let i = 0; i < n; i++) s += (i & 1023) + (i % 13) + data[i & 63]; return s; }
let expected = 0;
for (let i = 0; i < 300000; i++) expected += (i & 1023) + (i % 13) + data[i & 63];
same(longLoop(300000), expected, 'OSR entry');

console.log('PASS compiled loop counters read as integers match double semantics');
