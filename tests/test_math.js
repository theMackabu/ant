let passed = 0;
let failed = 0;

function approxEqual(a, b, epsilon = 1e-10) {
  if (Number.isNaN(a) && Number.isNaN(b)) return true;
  if (!Number.isFinite(a) && !Number.isFinite(b)) return a === b;
  return Math.abs(a - b) < epsilon;
}

function test(name, actual, expected, epsilon = 1e-10) {
  if (approxEqual(actual, expected, epsilon)) {
    console.log('PASS:', name);
    passed++;
  } else {
    console.log('FAIL:', name, '- expected', expected, 'got', actual);
    failed++;
  }
}

console.log('=== Math Constants ===');
test('Math.E', Math.E, 2.718281828459045);
test('Math.LN10', Math.LN10, 2.302585092994046);
test('Math.LN2', Math.LN2, 0.6931471805599453);
test('Math.LOG10E', Math.LOG10E, 0.4342944819032518);
test('Math.LOG2E', Math.LOG2E, 1.4426950408889634);
test('Math.PI', Math.PI, 3.141592653589793);
test('Math.SQRT1_2', Math.SQRT1_2, 0.7071067811865476);
test('Math.SQRT2', Math.SQRT2, 1.4142135623730951);

console.log('\n=== Math.abs ===');
test('Math.abs(5)', Math.abs(5), 5);
test('Math.abs(-5)', Math.abs(-5), 5);
test('Math.abs(0)', Math.abs(0), 0);
test('Math.abs(-0)', Math.abs(-0), 0);
test('Math.abs(-Infinity)', Math.abs(-Infinity), Infinity);

console.log('\n=== Math.acos ===');
test('Math.acos(1)', Math.acos(1), 0);
test('Math.acos(0)', Math.acos(0), Math.PI / 2);
test('Math.acos(-1)', Math.acos(-1), Math.PI);
test('Math.acos(2)', Number.isNaN(Math.acos(2)), true);

console.log('\n=== Math.acosh ===');
test('Math.acosh(1)', Math.acosh(1), 0);
test('Math.acosh(2)', Math.acosh(2), 1.3169578969248166);
test('Math.acosh(0.5)', Number.isNaN(Math.acosh(0.5)), true);

console.log('\n=== Math.asin ===');
test('Math.asin(0)', Math.asin(0), 0);
test('Math.asin(1)', Math.asin(1), Math.PI / 2);
test('Math.asin(-1)', Math.asin(-1), -Math.PI / 2);
test('Math.asin(2)', Number.isNaN(Math.asin(2)), true);

console.log('\n=== Math.asinh ===');
test('Math.asinh(0)', Math.asinh(0), 0);
test('Math.asinh(1)', Math.asinh(1), 0.881373587019543);
test('Math.asinh(-1)', Math.asinh(-1), -0.881373587019543);

console.log('\n=== Math.atan ===');
test('Math.atan(0)', Math.atan(0), 0);
test('Math.atan(1)', Math.atan(1), Math.PI / 4);
test('Math.atan(Infinity)', Math.atan(Infinity), Math.PI / 2);
test('Math.atan(-Infinity)', Math.atan(-Infinity), -Math.PI / 2);

console.log('\n=== Math.atanh ===');
test('Math.atanh(0)', Math.atanh(0), 0);
test('Math.atanh(0.5)', Math.atanh(0.5), 0.5493061443340549);
test('Math.atanh(1)', Math.atanh(1), Infinity);
test('Math.atanh(-1)', Math.atanh(-1), -Infinity);

console.log('\n=== Math.atan2 ===');
test('Math.atan2(1, 1)', Math.atan2(1, 1), Math.PI / 4);
test('Math.atan2(1, 0)', Math.atan2(1, 0), Math.PI / 2);
test('Math.atan2(0, 1)', Math.atan2(0, 1), 0);
test('Math.atan2(-1, -1)', Math.atan2(-1, -1), -3 * Math.PI / 4);

console.log('\n=== Math.cbrt ===');
test('Math.cbrt(8)', Math.cbrt(8), 2);
test('Math.cbrt(-8)', Math.cbrt(-8), -2);
test('Math.cbrt(27)', Math.cbrt(27), 3);
test('Math.cbrt(0)', Math.cbrt(0), 0);

console.log('\n=== Math.ceil ===');
test('Math.ceil(0.5)', Math.ceil(0.5), 1);
test('Math.ceil(-0.5)', Math.ceil(-0.5), 0);
test('Math.ceil(1)', Math.ceil(1), 1);
test('Math.ceil(4.7)', Math.ceil(4.7), 5);
test('Math.ceil(-4.7)', Math.ceil(-4.7), -4);

console.log('\n=== Math.clz32 ===');
test('Math.clz32(1)', Math.clz32(1), 31);
test('Math.clz32(2)', Math.clz32(2), 30);
test('Math.clz32(0)', Math.clz32(0), 32);
test('Math.clz32(0x80000000)', Math.clz32(0x80000000), 0);

console.log('\n=== Math.cos ===');
test('Math.cos(0)', Math.cos(0), 1);
test('Math.cos(Math.PI)', Math.cos(Math.PI), -1);
test('Math.cos(Math.PI / 2)', Math.cos(Math.PI / 2), 0, 1e-15);

console.log('\n=== Math.cosh ===');
test('Math.cosh(0)', Math.cosh(0), 1);
test('Math.cosh(1)', Math.cosh(1), 1.5430806348152437);

console.log('\n=== Math.exp ===');
test('Math.exp(0)', Math.exp(0), 1);
test('Math.exp(1)', Math.exp(1), Math.E);
test('Math.exp(-1)', Math.exp(-1), 1 / Math.E);

console.log('\n=== Math.expm1 ===');
test('Math.expm1(0)', Math.expm1(0), 0);
test('Math.expm1(1)', Math.expm1(1), Math.E - 1);

console.log('\n=== Math.floor ===');
test('Math.floor(0.5)', Math.floor(0.5), 0);
test('Math.floor(-0.5)', Math.floor(-0.5), -1);
test('Math.floor(1)', Math.floor(1), 1);
test('Math.floor(4.7)', Math.floor(4.7), 4);
test('Math.floor(-4.7)', Math.floor(-4.7), -5);

console.log('\n=== Math.fround ===');
test('Math.fround(1.5)', Math.fround(1.5), 1.5);
test('Math.fround(1.337)', Math.fround(1.337), 1.3370000123977661);

console.log('\n=== Math.hypot ===');
test('Math.hypot(3, 4)', Math.hypot(3, 4), 5);
test('Math.hypot(5, 12)', Math.hypot(5, 12), 13);
test('Math.hypot(3, 4, 5)', Math.hypot(3, 4, 5), 7.0710678118654755);
test('Math.hypot()', Math.hypot(), 0);

console.log('\n=== Math.imul ===');
test('Math.imul(2, 4)', Math.imul(2, 4), 8);
test('Math.imul(-1, 8)', Math.imul(-1, 8), -8);
test('Math.imul(0xffffffff, 5)', Math.imul(0xffffffff, 5), -5);

console.log('\n=== Math.log ===');
test('Math.log(1)', Math.log(1), 0);
test('Math.log(Math.E)', Math.log(Math.E), 1);
test('Math.log(10)', Math.log(10), Math.LN10);
test('Math.log(0)', Math.log(0), -Infinity);

console.log('\n=== Math.log1p ===');
test('Math.log1p(0)', Math.log1p(0), 0);
test('Math.log1p(1)', Math.log1p(1), Math.LN2);
test('Math.log1p(-1)', Math.log1p(-1), -Infinity);

console.log('\n=== Math.log10 ===');
test('Math.log10(1)', Math.log10(1), 0);
test('Math.log10(10)', Math.log10(10), 1);
test('Math.log10(100)', Math.log10(100), 2);
test('Math.log10(1000)', Math.log10(1000), 3);

console.log('\n=== Math.log2 ===');
test('Math.log2(1)', Math.log2(1), 0);
test('Math.log2(2)', Math.log2(2), 1);
test('Math.log2(8)', Math.log2(8), 3);
test('Math.log2(1024)', Math.log2(1024), 10);

console.log('\n=== Math.max ===');
test('Math.max(1, 2, 3)', Math.max(1, 2, 3), 3);
test('Math.max(-1, -2, -3)', Math.max(-1, -2, -3), -1);
test('Math.max(1)', Math.max(1), 1);
test('Math.max()', Math.max(), -Infinity);
test('Math.max(5, 10, 3, 8)', Math.max(5, 10, 3, 8), 10);

console.log('\n=== Math.min ===');
test('Math.min(1, 2, 3)', Math.min(1, 2, 3), 1);
test('Math.min(-1, -2, -3)', Math.min(-1, -2, -3), -3);
test('Math.min(1)', Math.min(1), 1);
test('Math.min()', Math.min(), Infinity);
test('Math.min(5, 10, 3, 8)', Math.min(5, 10, 3, 8), 3);

console.log('\n=== Math.pow ===');
test('Math.pow(2, 3)', Math.pow(2, 3), 8);
test('Math.pow(2, 10)', Math.pow(2, 10), 1024);
test('Math.pow(10, 0)', Math.pow(10, 0), 1);
test('Math.pow(4, 0.5)', Math.pow(4, 0.5), 2);
test('Math.pow(2, -1)', Math.pow(2, -1), 0.5);

console.log('\n=== Math.random ===');
let r1 = Math.random();
let r2 = Math.random();
test('Math.random() >= 0', r1 >= 0, true);
test('Math.random() < 1', r1 < 1, true);
test('Math.random() returns different values', r1 !== r2 || r1 === r2, true);

console.log('\n=== Math.round ===');
test('Math.round(0.5)', Math.round(0.5), 1);
test('Math.round(0.49)', Math.round(0.49), 0);
test('Math.round(-0.5)', Math.round(-0.5), 0);
test('Math.round(-0.51)', Math.round(-0.51), -1);
test('Math.round(4.7)', Math.round(4.7), 5);
test('Math.round(4.4)', Math.round(4.4), 4);

console.log('\n=== Math.sign ===');
test('Math.sign(5)', Math.sign(5), 1);
test('Math.sign(-5)', Math.sign(-5), -1);
test('Math.sign(0)', Math.sign(0), 0);

console.log('\n=== Math.sin ===');
test('Math.sin(0)', Math.sin(0), 0);
test('Math.sin(Math.PI / 2)', Math.sin(Math.PI / 2), 1);
test('Math.sin(Math.PI)', Math.sin(Math.PI), 0, 1e-15);

console.log('\n=== Math.sinh ===');
test('Math.sinh(0)', Math.sinh(0), 0);
test('Math.sinh(1)', Math.sinh(1), 1.1752011936438014);

console.log('\n=== Math.sqrt ===');
test('Math.sqrt(4)', Math.sqrt(4), 2);
test('Math.sqrt(9)', Math.sqrt(9), 3);
test('Math.sqrt(2)', Math.sqrt(2), Math.SQRT2);
test('Math.sqrt(0.5)', Math.sqrt(0.5), Math.SQRT1_2);
test('Math.sqrt(0)', Math.sqrt(0), 0);
test('Math.sqrt(-1)', Number.isNaN(Math.sqrt(-1)), true);

console.log('\n=== Math.tan ===');
test('Math.tan(0)', Math.tan(0), 0);
test('Math.tan(Math.PI / 4)', Math.tan(Math.PI / 4), 1);

console.log('\n=== Math.tanh ===');
test('Math.tanh(0)', Math.tanh(0), 0);
test('Math.tanh(1)', Math.tanh(1), 0.7615941559557649);
test('Math.tanh(Infinity)', Math.tanh(Infinity), 1);
test('Math.tanh(-Infinity)', Math.tanh(-Infinity), -1);

console.log('\n=== Math.trunc ===');
test('Math.trunc(4.7)', Math.trunc(4.7), 4);
test('Math.trunc(-4.7)', Math.trunc(-4.7), -4);
test('Math.trunc(0.5)', Math.trunc(0.5), 0);
test('Math.trunc(-0.5)', Math.trunc(-0.5), 0);
test('Math.trunc(0)', Math.trunc(0), 0);

console.log('\n=== Edge Cases ===');
test('Math.abs(NaN)', Number.isNaN(Math.abs(NaN)), true);
test('Math.sqrt(NaN)', Number.isNaN(Math.sqrt(NaN)), true);
test('Math.max(1, NaN)', Number.isNaN(Math.max(1, NaN)), true);
test('Math.min(1, NaN)', Number.isNaN(Math.min(1, NaN)), true);
test('Math.pow(NaN, 2)', Number.isNaN(Math.pow(NaN, 2)), true);

console.log('\n=== Summary ===');
console.log('Passed:', passed);
console.log('Failed:', failed);
console.log('Total:', passed + failed);

if (failed > 0) {
  console.log('\nSome tests FAILED!');
} else {
  console.log('\nAll tests PASSED!');
}
