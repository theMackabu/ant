import { test, testApprox, summary } from './helpers.js';

console.log('Number Tests\n');

test('integer literal', 42, 42);
test('float literal', 3.14, 3.14);
test('negative number', -5, -5);
test('zero', 0, 0);

test('hex literal', 0xff, 255);
test('octal literal', 0o77, 63);
test('binary literal', 0b1010, 10);

test('exponential', 1e3, 1000);
test('exponential negative', 1e-3, 0.001);

test('numeric separator', 1_000_000, 1000000);

test('Number()', Number('42'), 42);
test('Number() float', Number('3.14'), 3.14);
test('Number() invalid', Number.isNaN(Number('abc')), true);

test('parseInt', parseInt('42'), 42);
test('parseInt radix', parseInt('ff', 16), 255);
test('parseFloat', parseFloat('3.14'), 3.14);

test('Number.isInteger true', Number.isInteger(5), true);
test('Number.isInteger false', Number.isInteger(5.5), false);

test('Number.isFinite true', Number.isFinite(100), true);
test('Number.isFinite false', Number.isFinite(Infinity), false);

test('Number.isNaN true', Number.isNaN(NaN), true);
test('Number.isNaN false', Number.isNaN(5), false);

test('isNaN coercing', isNaN('abc'), true);
test('Number.isNaN strict', Number.isNaN('abc'), false);

test('Infinity', Infinity > 1e308, true);
test('-Infinity', -Infinity < -1e308, true);

test('NaN !== NaN', NaN !== NaN, true);

test('Number.MAX_VALUE', Number.MAX_VALUE > 1e308, true);
test('Number.MIN_VALUE', Number.MIN_VALUE > 0, true);
test('Number.MAX_SAFE_INTEGER', Number.MAX_SAFE_INTEGER, 9007199254740991);
test('Number.MIN_SAFE_INTEGER', Number.MIN_SAFE_INTEGER, -9007199254740991);

test('Number.isSafeInteger true', Number.isSafeInteger(42), true);
test('Number.isSafeInteger false', Number.isSafeInteger(9007199254740992), false);

test('toFixed', (3.14159).toFixed(2), '3.14');
test('toPrecision', (123.456).toPrecision(4), '123.5');
test('toExponential', (12345).toExponential(2), '1.23e+4');
test('toString', (255).toString(16), 'ff');

testApprox('addition', 0.1 + 0.2, 0.3, 1e-10);
test('multiplication', 6 * 7, 42);
test('division', 10 / 4, 2.5);
test('modulo', 17 % 5, 2);
test('exponentiation', 2 ** 10, 1024);

test('unary plus', +'42', 42);
test('unary minus', -42, -42);

test('Math.floor division', Math.floor(7 / 2), 3);
test('bitwise or floor', (7 / 2) | 0, 3);

summary();
