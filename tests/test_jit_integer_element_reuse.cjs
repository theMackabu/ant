function same(actual, expected, label) {
  if (!Object.is(actual, expected)) throw new Error(label + ': ' + actual + ' != ' + expected);
}
function integerLocals(x, change) {
  let a = x & 0xffff;
  let b = a >>> 3;
  const before = b & 255;
  b++;
  b += 0.5;
  if (change) b = -1;
  return before + b;
}
for (let i = 0; i < 1200; i++) {
  const b = (i & 65535) >>> 3;
  same(integerLocals(i, false), (b & 255) + b + 1.5, 'local updates invalidate integer fact');
  same(integerLocals(i, true), (b & 255) - 1, 'branch merge invalidates integer fact');
}
function repeatedRead(array, i) {
  const low = array[i] & 16383;
  const high = array[i++] >> 14;
  return low + high + i;
}
const limbs = [0x1234567, 0x7654321];
for (let i = 0; i < 1200; i++) {
  const j = i & 1;
  same(repeatedRead(limbs, j), (limbs[j] & 16383) + (limbs[j] >> 14) + j + 1, 'duplicate limb read');
}
let reads = 0;
const accessor = Object.defineProperty([], '0', { get() { return ++reads; } });
same(repeatedRead(accessor, 0), 2, 'accessor values are not cached');
same(reads, 2, 'both getters run');
function mutateBetween(array, alias) {
  const first = array[0];
  alias[0] = first + 1;
  return array[0];
}
for (let i = 0; i < 1200; i++) same(mutateBetween(limbs, limbs), 0x1234568 + i, 'alias write invalidates load');
function callBetween(array, effect) {
  const first = array[0];
  effect();
  return first + array[0];
}
const called = [0];
function changeCalled() { called[0]++; }
for (let i = 0; i < 1200; i++) same(callBetween(called, changeCalled), i * 2 + 1, 'call invalidates load');
function coerceBetween(array, value) {
  const first = array[0];
  const bits = value & 255;
  return first + bits + array[0];
}
const coerced = [0];
const coercion = { valueOf() { coerced[0]++; return 1; } };
for (let i = 0; i < 1200; i++) same(coerceBetween(coerced, coercion), i * 2 + 2, 'coercion invalidates load');
function mixedIndex(array, key) { return array[key]; }
const indexed = [9, 10];
indexed[-1] = 11;
indexed[1.5] = 12;
indexed[4294967295] = 13;
indexed.NaN = 14;
for (let i = 0; i < 1200; i++) {
  same(mixedIndex(indexed, 0), 9, 'cached integer index');
  same(mixedIndex(indexed, -0), 9, 'negative zero index');
  same(mixedIndex(indexed, -1), 11, 'negative index');
  same(mixedIndex(indexed, 1.5), 12, 'fractional index');
  same(mixedIndex(indexed, 4294967295), 13, 'uint32 maximum property');
  same(mixedIndex(indexed, NaN), 14, 'NaN property');
  same(mixedIndex(indexed, undefined), undefined, 'index cache initial sentinel');
}
let keys = 0;
const keyObject = { toString() { keys++; return '0'; } };
same(mixedIndex(indexed, keyObject), 9, 'coercing index first read');
same(mixedIndex(indexed, keyObject), 9, 'coercing index second read');
same(keys, 2, 'index coercion runs each time');
function osrReads(array) {
  let sum = 0;
  for (let i = 0; i < 20000; i++) {
    const index = i & 1;
    const value = array[index] | 0;
    sum += (value & 255) + array[index];
  }
  return sum;
}
same(osrReads([1, 2]), 60000, 'OSR cache initialization');
function integerIndex(array, value) {
  const index = value | 0;
  const old = array[index];
  array[index] = old;
  return old;
}
const ordinary = { 0: 7, '-1': 8 };
const numeric = [7];
numeric[-1] = 8;
for (let i = 0; i < 600; i++) {
  same(integerIndex(ordinary, 0), 7, 'mixed integer index object');
  same(integerIndex(numeric, 0), 7, 'proven integer index array');
  same(integerIndex(numeric, -1), 8, 'proven negative integer index');
}
function shrinkBetween(array) {
  const before = array[0];
  array.length = 0;
  return before + ':' + array[0];
}
for (let i = 0; i < 600; i++) same(shrinkBetween([3]), '3:undefined', 'length write invalidates read');
let proxyReads = 0;
const proxy = new Proxy([1], { get(target, key) { proxyReads++; return target[key]; } });
same(repeatedRead(proxy, 0), 2, 'proxy reads remain observable');
same(proxyReads, 2, 'both proxy traps run');
function catchBetween(array, effect) {
  const before = array[0];
  try { effect(); } catch (_) {}
  return before + array[0];
}
const changed = [0];
function changeAndThrow() { changed[0]++; throw 1; }
for (let i = 0; i < 600; i++) same(catchBetween(changed, changeAndThrow), i * 2 + 1, 'catch merge invalidates read');
console.log('PASS integer and element reuse');
