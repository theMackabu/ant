let passed = 0;
let failed = 0;

function test(name, actual, expected) {
  const actualStr = String(actual);
  const expectedStr = String(expected);
  if (actualStr === expectedStr) {
    console.log('PASS:', name);
    passed++;
  } else {
    console.log('FAIL:', name, '- expected', expectedStr, 'got', actualStr);
    failed++;
  }
}

console.log('=== BigInt Literals ===');
test('123n', 123n, '123');
test('0n', 0n, '0');
test('9007199254740993n', 9007199254740993n, '9007199254740993');

console.log('\n=== BigInt Constructor ===');
test('BigInt(42)', BigInt(42), '42');
test('BigInt(0)', BigInt(0), '0');
test('BigInt(-5)', BigInt(-5), '-5');
test('BigInt("123")', BigInt("123"), '123');
test('BigInt("-456")', BigInt("-456"), '-456');
test('BigInt(true)', BigInt(true), '1');
test('BigInt(false)', BigInt(false), '0');

console.log('\n=== BigInt Addition ===');
test('1n + 2n', 1n + 2n, '3');
test('100n + 200n', 100n + 200n, '300');
test('999999999999999999n + 1n', 999999999999999999n + 1n, '1000000000000000000');
test('-5n + 10n', -5n + 10n, '5');
test('10n + (-5n)', 10n + (-5n), '5');
test('-3n + (-7n)', -3n + (-7n), '-10');

console.log('\n=== BigInt Subtraction ===');
test('10n - 3n', 10n - 3n, '7');
test('3n - 10n', 3n - 10n, '-7');
test('100n - 100n', 100n - 100n, '0');
test('-5n - 3n', -5n - 3n, '-8');
test('5n - (-3n)', 5n - (-3n), '8');

console.log('\n=== BigInt Multiplication ===');
test('6n * 7n', 6n * 7n, '42');
test('123n * 456n', 123n * 456n, '56088');
test('-3n * 4n', -3n * 4n, '-12');
test('-3n * (-4n)', -3n * (-4n), '12');
test('0n * 999n', 0n * 999n, '0');
test('999999999n * 999999999n', 999999999n * 999999999n, '999999998000000001');

console.log('\n=== BigInt Division ===');
test('10n / 3n', 10n / 3n, '3');
test('100n / 10n', 100n / 10n, '10');
test('7n / 2n', 7n / 2n, '3');
test('-10n / 3n', -10n / 3n, '-3');
test('10n / (-3n)', 10n / (-3n), '-3');

console.log('\n=== BigInt Modulo ===');
test('10n % 3n', 10n % 3n, '1');
test('100n % 7n', 100n % 7n, '2');
test('-10n % 3n', -10n % 3n, '-1');
test('10n % (-3n)', 10n % (-3n), '1');

console.log('\n=== BigInt Comparison ===');
test('5n == 5n', 5n == 5n, true);
test('5n == 6n', 5n == 6n, false);
test('5n != 6n', 5n != 6n, true);
test('5n < 6n', 5n < 6n, true);
test('6n < 5n', 6n < 5n, false);
test('5n > 4n', 5n > 4n, true);
test('4n > 5n', 4n > 5n, false);
test('5n <= 5n', 5n <= 5n, true);
test('5n <= 6n', 5n <= 6n, true);
test('5n >= 5n', 5n >= 5n, true);
test('5n >= 4n', 5n >= 4n, true);
test('-5n < 5n', -5n < 5n, true);
test('-5n > -10n', -5n > -10n, true);

console.log('\n=== BigInt Unary Minus ===');
test('-5n', -5n, '-5');
test('-(-5n)', -(-5n), '5');
test('-0n', -0n, '0');

console.log('\n=== BigInt typeof ===');
test('typeof 5n', typeof 5n, 'bigint');
test('typeof BigInt(5)', typeof BigInt(5), 'bigint');

console.log('\n=== BigInt Truthiness ===');
test('!!1n', !!1n, true);
test('!!0n', !!0n, false);
test('!!(-1n)', !!(-1n), true);

console.log('\n=== Large BigInt ===');
const big1 = 12345678901234567890123456789n;
const big2 = 98765432109876543210987654321n;
test('big1 + big2', big1 + big2, '111111111011111111101111111110');
test('big2 - big1', big2 - big1, '86419753208641975320864197532');
test('big1 * 2n', big1 * 2n, '24691357802469135780246913578');

console.log('\n=== Summary ===');
console.log('Passed:', passed);
console.log('Failed:', failed);
console.log('Total:', passed + failed);

if (failed > 0) {
  console.log('\nSome tests FAILED!');
} else {
  console.log('\nAll tests PASSED!');
}
