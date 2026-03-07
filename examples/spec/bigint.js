import { test, testThrows, summary } from './helpers.js';

console.log('BigInt Tests\n');

test('bigint literal', 123n, 123n);
test('BigInt()', BigInt(456), 456n);
test('BigInt from string', BigInt('789'), 789n);

test('bigint addition', 1n + 2n, 3n);
test('bigint subtraction', 10n - 3n, 7n);
test('bigint multiplication', 4n * 5n, 20n);
test('bigint division', 10n / 3n, 3n);
test('bigint modulo', 10n % 3n, 1n);
test('bigint exponentiation', 2n ** 10n, 1024n);

test('bigint comparison <', 1n < 2n, true);
test('bigint comparison >', 5n > 3n, true);
test('bigint comparison ===', 5n === 5n, true);
test('bigint comparison ==', 5n == 5, true);

test('typeof bigint', typeof 123n, 'bigint');

test('large bigint', 9007199254740993n > 9007199254740991n, true);

test('bigint negation', -5n, -5n);

test('bigint toString', (255n).toString(16), 'ff');
test('bigint toString radix 2 suffix', ((1n << 64n) + 255n).toString(2).slice(-8), '11111111');
test('bigint toString radix 10', ((1n << 128n) + 1n).toString(10), '340282366920938463463374607431768211457');

test('bigint shift left', 1n << 130n, 1361129467683753853853498429727072845824n);
test('bigint shift right positive', 8n >> 1n, 4n);
test('bigint shift right negative floor -3n', -3n >> 1n, -2n);
test('bigint shift right negative floor -5n', -5n >> 1n, -3n);
test('bigint shift right huge negative', -1n >> 100n, -1n);
testThrows('bigint unsigned right shift throws', () => (1n >>> 0n));

const limbA = (1n << 200n) + (1n << 129n) + 12345678901234567890n;
const limbB = (1n << 73n) + 12345n;
test('bigint multi-limb division', limbA / limbB, 170141183460469231509371611710366941474n);
test('bigint multi-limb modulo', limbA % limbB, 5527003422616403339840n);
test('bigint division truncates toward zero', -19n / 4n, -4n);
test('bigint modulo keeps dividend sign', -19n % 4n, -3n);

test('BigInt.asUintN 0 bits', BigInt.asUintN(0, 123n), 0n);
test('BigInt.asUintN wrap', BigInt.asUintN(8, 256n), 0n);
test('BigInt.asUintN negative', BigInt.asUintN(8, -1n), 255n);
test('BigInt.asIntN 0 bits', BigInt.asIntN(0, 123n), 0n);
test('BigInt.asIntN positive', BigInt.asIntN(8, 127n), 127n);
test('BigInt.asIntN negative', BigInt.asIntN(8, 255n), -1n);
test('BigInt.asIntN sign bit', BigInt.asIntN(8, 128n), -128n);

summary();
