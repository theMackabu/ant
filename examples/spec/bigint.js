import { test, summary } from './helpers.js';

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

test('BigInt.asUintN 0 bits', BigInt.asUintN(0, 123n), 0n);
test('BigInt.asUintN wrap', BigInt.asUintN(8, 256n), 0n);
test('BigInt.asUintN negative', BigInt.asUintN(8, -1n), 255n);
test('BigInt.asIntN 0 bits', BigInt.asIntN(0, 123n), 0n);
test('BigInt.asIntN positive', BigInt.asIntN(8, 127n), 127n);
test('BigInt.asIntN negative', BigInt.asIntN(8, 255n), -1n);
test('BigInt.asIntN sign bit', BigInt.asIntN(8, 128n), -128n);

summary();
