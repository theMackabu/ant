import { test, testApprox, summary } from './helpers.js';

console.log('Math Tests\n');

testApprox('Math.E', Math.E, 2.718281828459045);
testApprox('Math.PI', Math.PI, 3.141592653589793);
testApprox('Math.LN10', Math.LN10, 2.302585092994046);
testApprox('Math.LN2', Math.LN2, 0.6931471805599453);
testApprox('Math.LOG10E', Math.LOG10E, 0.4342944819032518);
testApprox('Math.LOG2E', Math.LOG2E, 1.4426950408889634);
testApprox('Math.SQRT2', Math.SQRT2, 1.4142135623730951);
testApprox('Math.SQRT1_2', Math.SQRT1_2, 0.7071067811865476);

test('Math.abs(5)', Math.abs(5), 5);
test('Math.abs(-5)', Math.abs(-5), 5);
test('Math.abs(0)', Math.abs(0), 0);

testApprox('Math.acos(1)', Math.acos(1), 0);
testApprox('Math.acos(0)', Math.acos(0), Math.PI / 2);
testApprox('Math.acos(-1)', Math.acos(-1), Math.PI);
test('Math.acos(2) is NaN', Number.isNaN(Math.acos(2)), true);

testApprox('Math.asin(0)', Math.asin(0), 0);
testApprox('Math.asin(1)', Math.asin(1), Math.PI / 2);

testApprox('Math.atan(0)', Math.atan(0), 0);
testApprox('Math.atan(1)', Math.atan(1), Math.PI / 4);

testApprox('Math.atan2(1, 1)', Math.atan2(1, 1), Math.PI / 4);
testApprox('Math.atan2(1, 0)', Math.atan2(1, 0), Math.PI / 2);

testApprox('Math.cbrt(8)', Math.cbrt(8), 2);
testApprox('Math.cbrt(-8)', Math.cbrt(-8), -2);
testApprox('Math.cbrt(27)', Math.cbrt(27), 3);

test('Math.ceil(0.5)', Math.ceil(0.5), 1);
test('Math.ceil(-0.5)', Math.ceil(-0.5), 0);
test('Math.ceil(4.7)', Math.ceil(4.7), 5);

test('Math.floor(0.5)', Math.floor(0.5), 0);
test('Math.floor(-0.5)', Math.floor(-0.5), -1);
test('Math.floor(4.7)', Math.floor(4.7), 4);

test('Math.round(0.5)', Math.round(0.5), 1);
test('Math.round(0.49)', Math.round(0.49), 0);
test('Math.round(4.7)', Math.round(4.7), 5);

test('Math.trunc(4.7)', Math.trunc(4.7), 4);
test('Math.trunc(-4.7)', Math.trunc(-4.7), -4);

testApprox('Math.cos(0)', Math.cos(0), 1);
testApprox('Math.cos(Math.PI)', Math.cos(Math.PI), -1);

testApprox('Math.sin(0)', Math.sin(0), 0);
testApprox('Math.sin(Math.PI/2)', Math.sin(Math.PI / 2), 1);

testApprox('Math.tan(0)', Math.tan(0), 0);
testApprox('Math.tan(Math.PI/4)', Math.tan(Math.PI / 4), 1);

testApprox('Math.exp(0)', Math.exp(0), 1);
testApprox('Math.exp(1)', Math.exp(1), Math.E);

testApprox('Math.log(1)', Math.log(1), 0);
testApprox('Math.log(Math.E)', Math.log(Math.E), 1);

testApprox('Math.log10(1)', Math.log10(1), 0);
testApprox('Math.log10(10)', Math.log10(10), 1);
testApprox('Math.log10(100)', Math.log10(100), 2);

testApprox('Math.log2(1)', Math.log2(1), 0);
testApprox('Math.log2(2)', Math.log2(2), 1);
testApprox('Math.log2(8)', Math.log2(8), 3);

test('Math.max(1, 2, 3)', Math.max(1, 2, 3), 3);
test('Math.max(-1, -2, -3)', Math.max(-1, -2, -3), -1);
test('Math.max()', Math.max(), -Infinity);

test('Math.min(1, 2, 3)', Math.min(1, 2, 3), 1);
test('Math.min(-1, -2, -3)', Math.min(-1, -2, -3), -3);
test('Math.min()', Math.min(), Infinity);

test('Math.pow(2, 3)', Math.pow(2, 3), 8);
test('Math.pow(2, 10)', Math.pow(2, 10), 1024);
test('Math.pow(10, 0)', Math.pow(10, 0), 1);
testApprox('Math.pow(4, 0.5)', Math.pow(4, 0.5), 2);

let r = Math.random();
test('Math.random() >= 0', r >= 0, true);
test('Math.random() < 1', r < 1, true);

test('Math.sign(5)', Math.sign(5), 1);
test('Math.sign(-5)', Math.sign(-5), -1);
test('Math.sign(0)', Math.sign(0), 0);

testApprox('Math.sqrt(4)', Math.sqrt(4), 2);
testApprox('Math.sqrt(9)', Math.sqrt(9), 3);
test('Math.sqrt(-1) is NaN', Number.isNaN(Math.sqrt(-1)), true);

testApprox('Math.hypot(3, 4)', Math.hypot(3, 4), 5);
testApprox('Math.hypot(5, 12)', Math.hypot(5, 12), 13);

test('Math.clz32(1)', Math.clz32(1), 31);
test('Math.clz32(0)', Math.clz32(0), 32);

test('Math.imul(2, 4)', Math.imul(2, 4), 8);
test('Math.imul(-1, 8)', Math.imul(-1, 8), -8);

testApprox('Math.cosh(0)', Math.cosh(0), 1);
testApprox('Math.sinh(0)', Math.sinh(0), 0);
testApprox('Math.tanh(0)', Math.tanh(0), 0);

testApprox('Math.acosh(1)', Math.acosh(1), 0);
testApprox('Math.asinh(0)', Math.asinh(0), 0);
testApprox('Math.atanh(0)', Math.atanh(0), 0);

testApprox('Math.expm1(0)', Math.expm1(0), 0);
testApprox('Math.log1p(0)', Math.log1p(0), 0);

test('Math.abs(NaN) is NaN', Number.isNaN(Math.abs(NaN)), true);
test('Math.abs(-Infinity)', Math.abs(-Infinity), Infinity);
test('Math.log(0)', Math.log(0), -Infinity);

summary();
