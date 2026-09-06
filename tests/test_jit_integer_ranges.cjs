function same(a, b, name) {
  if (!Object.is(a, b)) throw new Error(name + ': ' + a + ' != ' + b);
}
function loopRanges(x, y, count) {
  const low = x & 16383;
  const high = y >>> 16;
  let sum = 0;
  for (let i = 0; i < count; i++) {
    const value = (i & 255) + low;
    sum += value * high;
  }
  return sum;
}
for (let i = 0; i < 500; i++) same(loopRanges(3, 131072, 4), 36, 'entry integer loop');
same(loopRanges(3, 131072, 20000), 5212832, 'entry integer OSR');
function conditional(x, flag) {
  let mask;
  if (flag) mask = x & 255;
  else mask = 0.5;
  let sum = 0;
  for (let i = 0; i < 10; i++) sum += mask * 2;
  return sum;
}
for (let i = 0; i < 500; i++) {
  same(conditional(3, true), 60, 'conditional integer');
  same(conditional(3, false), 10, 'conditional fractional local');
}
function reassigned(x) {
  let mask = x & 255;
  let total = 0;
  for (let i = 0; i < 10; i++) {
    total += mask;
    mask = 0.5;
  }
  return total;
}
for (let i = 0; i < 500; i++) same(reassigned(3), 7.5, 'loop-carried reassignment');
function captured(x) {
  let mask = x & 255;
  function change() { mask = 0.5; }
  let sum = 0;
  for (let i = 0; i < 10; i++) { change(); sum += mask; }
  return sum;
}
for (let i = 0; i < 500; i++) same(captured(3), 5, 'captured local');
function multiplySigned(a, b) { const x = a | 0; const y = b | 0; return x * y; }
function multiplyUnsigned(a, b) { const x = a >>> 0; const y = b >>> 0; return x * y; }
function addUnsigned(a, b) { const x = a >>> 0; const y = b >>> 0; return x + y; }
function subtractUnsigned(a, b) { const x = a >>> 0; const y = b >>> 0; return x - y; }
function masked(a, b) { const x = a & 65535; const y = b & 65535; return x * y + 0.5; }
function negativeZero() { const a = -0; const b = 3; return a * b; }
for (let i = 0; i < 500; i++) {
  same(multiplySigned(0, -1), -0, 'negative-zero product');
  same(multiplySigned(-1, 0), -0, 'negative-zero reverse product');
  same(negativeZero(), -0, 'negative-zero literal');
  same(multiplyUnsigned(-1, -1), 18446744065119617000, 'product beyond signed64');
  same(addUnsigned(-1, -1), 8589934590, 'sum beyond word32');
  same(subtractUnsigned(0, -1), -4294967295, 'difference below int32');
  same(masked(-1, -1), 4294836225.5, 'large exact masked product');
}
const cases = [-4294967296, -2147483648, -1, -0, 0, 1, 65535, 2147483647, 2147483648, 4294967295, NaN, Infinity];
// Reference values generated with Node from these functions and cases.
// Strings preserve NaN, infinities and negative zero without JSON number loss.
const expected = require('./fixtures/jit_integer_ranges.json');
function encodeNumber(value) { return Object.is(value, -0) ? '-0' : String(value); }
same(encodeNumber(NaN), 'NaN', 'NaN encoding');
same(encodeNumber(Infinity), 'Infinity', 'positive infinity encoding');
same(encodeNumber(-Infinity), '-Infinity', 'negative infinity encoding');
same(encodeNumber(-0), '-0', 'negative zero encoding');
let resultIndex = 0;
for (const a of cases) for (const b of cases) {
  for (const fn of [multiplySigned, multiplyUnsigned, addUnsigned, subtractUnsigned, masked]) {
    const value = fn(a, b);
    same(encodeNumber(value), expected[resultIndex++], fn.name + '(' + encodeNumber(a) + ', ' + encodeNumber(b) + ')');
  }
}
same(resultIndex, expected.length, 'reference matrix length');
console.log('PASS integer ranges: ' + resultIndex + ' matrix assertions');
