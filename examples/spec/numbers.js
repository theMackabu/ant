import { test, testApprox, summary } from './helpers.js';

console.log('Number Tests\n');

function testEvalSyntaxError(name, source) {
  let actual = 'none';
  try {
    eval(source);
  } catch (e) {
    actual = e.name;
  }
  test(name, actual, 'SyntaxError');
}

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
test('Number() empty string', Number(''), 0);
test('Number() signed hex invalid', Number.isNaN(Number('+0x10')), true);
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
test('decimal literal matches Number string parse', 0.7875 === Number('0.7875'), true);
test('toFixed halfway below', (0.7875).toFixed(3), '0.787');
test('toFixed halfway above', (0.7876).toFixed(3), '0.788');
test('toFixed exposes correctly parsed double', (0.7875).toFixed(20), '0.78749999999999997780');
test('toFixed binary midpoint', (2.675).toFixed(2), '2.67');
test('toPrecision', (123.456).toPrecision(4), '123.5');
test('toExponential', (12345).toExponential(2), '1.23e+4');
test('toString', (255).toString(16), 'ff');
test('integer literal method access with double dot', 27..toString(), '27');
test('integer literal method access with spaced dot', 27 .toString(), '27');
test('exponent literal method access', 1e3.toString(), '1000');
test('leading-dot literal method access', .5.toString(), '0.5');
testEvalSyntaxError('missing exponent digits syntax error', '1e');
testEvalSyntaxError('missing signed exponent digits syntax error', '1e+');
testEvalSyntaxError('dot exponent without digits syntax error', '1.e');
testEvalSyntaxError('integer literal dot identifier syntax error', '27.toString()');
testEvalSyntaxError('integer literal dot property syntax error', '27.a');
testEvalSyntaxError('leading-dot literal identifier syntax error', '.5foo');

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
