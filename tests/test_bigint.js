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

function testThrows(name, fn, expectedMessagePart) {
  try {
    fn();
    console.log('FAIL:', name, '- expected throw');
    failed++;
  } catch (err) {
    const msg = String(err);
    if (msg.indexOf(expectedMessagePart) !== -1) {
      console.log('PASS:', name);
      passed++;
    } else {
      console.log('FAIL:', name, '- wrong error:', msg);
      failed++;
    }
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
test('BigInt("0xff")', BigInt("0xff"), '255');
test('BigInt("0b1010")', BigInt("0b1010"), '10');
test('BigInt("0o77")', BigInt("0o77"), '63');
test('BigInt("  42  ")', BigInt("  42  "), '42');
test('BigInt("")', BigInt(""), '0');
test('BigInt(true)', BigInt(true), '1');
test('BigInt(false)', BigInt(false), '0');
testThrows('BigInt("0x") throws', () => BigInt("0x"), 'Cannot convert string to BigInt');
testThrows('BigInt("1_2") throws', () => BigInt("1_2"), 'Cannot convert string to BigInt');

console.log('\n=== BigInt Addition ===');
test('0xffn + 1n', 0xffn + 1n, '256');
test('0b1010n + 0o7n', 0b1010n + 0o7n, '17');
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

console.log('\n=== BigInt Bitwise ===');
test('5n & 3n', 5n & 3n, '1');
test('5n | 2n', 5n | 2n, '7');
test('5n ^ 1n', 5n ^ 1n, '4');
test('~0n', ~0n, '-1');
test('~(-1n)', ~(-1n), '0');
test('(-5n) & 3n', (-5n) & 3n, '3');
test('(-8n) | 3n', (-8n) | 3n, '-5');
test('(-8n) ^ 3n', (-8n) ^ 3n, '-5');
test('0xffn ^ 0b1010n', 0xffn ^ 0b1010n, '245');
testThrows('1n & 1 throws mixed types', () => (1n & 1), 'Cannot mix BigInt value and other types');

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

console.log('\n=== BigInt Shift Semantics ===');
test('8n >> 1n', 8n >> 1n, '4');
test('-3n >> 1n (floor)', -3n >> 1n, '-2');
test('-5n >> 1n (floor)', -5n >> 1n, '-3');
test('-1n >> 100n', -1n >> 100n, '-1');
test('1n << 130n', 1n << 130n, '1361129467683753853853498429727072845824');
test('(-1n) << 65n', (-1n) << 65n, '-36893488147419103232');
testThrows('1n >>> 0n throws TypeError', () => (1n >>> 0n), 'BigInts have no unsigned right shift');

console.log('\n=== BigInt Division and Mod Edge Cases ===');
const limbA = (1n << 200n) + (1n << 129n) + 12345678901234567890n;
const limbB = (1n << 73n) + 12345n;
test('multi-limb divide', limbA / limbB, '170141183460469231509371611710366941474');
test('multi-limb modulo', limbA % limbB, '5527003422616403339840');
test('negative divide truncates toward zero', -19n / 4n, '-4');
test('negative modulo keeps dividend sign', -19n % 4n, '-3');

console.log('\n=== BigInt toString(radix) ===');
const rad = (1n << 128n) + (1n << 64n) + 255n;
test('radix 10', rad.toString(10), '340282366920938463481821351505477763327');
test('radix 16', rad.toString(16), '1000000000000000100000000000000ff');
test('radix 2 suffix', rad.toString(2).slice(-8), '11111111');

console.log('\n=== BigInt.asIntN / asUintN ===');
test('BigInt.asIntN(8, 255n)', BigInt.asIntN(8, 255n), '-1');
test('BigInt.asIntN(8, 128n)', BigInt.asIntN(8, 128n), '-128');
test('BigInt.asUintN(8, -1n)', BigInt.asUintN(8, -1n), '255');
test('BigInt.asUintN(5, -3n)', BigInt.asUintN(5, -3n), '29');

console.log('\n=== Summary ===');
console.log('Passed:', passed);
console.log('Failed:', failed);
console.log('Total:', passed + failed);

if (failed > 0) {
  console.log('\nSome tests FAILED!');
} else {
  console.log('\nAll tests PASSED!');
}
