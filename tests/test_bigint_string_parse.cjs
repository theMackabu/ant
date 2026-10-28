let failures = 0;

function check(cond, msg) {
  if (!cond) {
    failures++;
    console.log('FAIL:', msg);
  }
}

function show(s) { return JSON.stringify(s); }

const valid = [
  ['', 0n], [' ', 0n], ['\t\n\v\f\r ', 0n], ['  12\n', 12n], [' 12', 12n], ['12 ', 12n],
  [' 12 ', 12n], ['   12', 12n], [' 1 ', 1n], ['　12　', 12n],
  ['﻿12', 12n], ['  7', 7n], ['+12', 12n], ['-12', -12n], ['-0', 0n],
  ['0x1f', 31n], ['0X1F', 31n], [' 0b101 ', 5n], ['0B11', 3n], ['0o17', 15n], ['0O17', 15n],
  ['00012', 12n], ['123456789012345678901234567890', 123456789012345678901234567890n],
];

const invalid = [
  '-0x1', '+0x1', '0x', '0b', '0o', '0b2', '0o8', '0xg', '1e3', '12n', '1_0', '1.0', '- 1', '+-1', '+', '-',
  '᠎12', '​1', '١', 'abc', '1 2', 'Infinity',
];

for (const [s, want] of valid) {
  let got;
  try { got = BigInt(s); } catch (e) { got = e; }
  check(got === want, `BigInt(${show(s)}) should be ${want}n, got ${got}`);
  check(s == want, `${show(s)} == ${want}n`);
  check(want == s, `${want}n == ${show(s)}`);
  check(!(s != want), `${show(s)} != ${want}n is false`);
  check(s <= want && s >= want && !(s < want) && !(s > want), `${show(s)} compares equal to ${want}n`);
  check(want + 1n > s && s < want + 1n && want - 1n < s, `${show(s)} orders against ${want}n +- 1`);
}

for (const s of invalid) {
  let err;
  try { BigInt(s); } catch (e) { err = e; }
  check(err instanceof SyntaxError, `BigInt(${show(s)}) throws SyntaxError, got ${err}`);
  check((s == 0n) === false && (s == 1n) === false, `${show(s)} == bigint is false`);
  let relErr;
  try {
    check(!(s < 5n) && !(s > 5n) && !(s <= 5n) && !(s >= 5n), `${show(s)} relational with bigint is false`);
    check(!(5n < s) && !(5n > s) && !(5n <= s) && !(5n >= s), `bigint relational with ${show(s)} is false`);
  } catch (e) { relErr = e; }
  check(relErr === undefined, `${show(s)} relational comparison does not throw: ${relErr}`);
}

check(1n < true === false && 1n <= true === true, 'bigint vs boolean compares numerically');
check(1n > null === true, 'bigint vs null compares to zero');
check((1n < undefined) === false && (1n >= undefined) === false, 'bigint vs undefined is unordered');
check(1n < [2] === true, 'bigint vs array goes through ToPrimitive');
check(({ valueOf() { return '1'; } }) < 2n, 'object coerced to a string then parsed');
let symErr;
try { 1n < Symbol(); } catch (e) { symErr = e; }
check(symErr instanceof TypeError, 'bigint vs symbol throws TypeError');

const conversions = [
  [() => BigInt(true), 1n], [() => BigInt(false), 0n], [() => BigInt({ valueOf() { return 5; } }), 5n],
  [() => BigInt({ valueOf() { return ' 6 '; } }), 6n], [() => BigInt({ valueOf() { return 7n; } }), 7n],
  [() => BigInt(Object(8n)), 8n], [() => BigInt([9]), 9n], [() => BigInt.asIntN(8, '255'), -1n],
  [() => BigInt.asIntN(8, true), 1n], [() => BigInt.asUintN(8, '-1'), 255n],
];
for (const [fn, want] of conversions) {
  let got;
  try { got = fn(); } catch (e) { got = e; }
  check(got === want, `${fn} should be ${want}n, got ${got}`);
}

const errors = [
  [() => BigInt(1.5), RangeError], [() => BigInt(NaN), RangeError], [() => BigInt(Infinity), RangeError],
  [() => BigInt({ valueOf() { return 1.5; } }), RangeError], [() => BigInt(undefined), TypeError],
  [() => BigInt(null), TypeError], [() => BigInt(Symbol()), TypeError], [() => BigInt.asIntN(8, 1), TypeError],
  [() => BigInt.asUintN(8, 'zz'), SyntaxError],
];
for (const [fn, type] of errors) {
  let err;
  try { fn(); } catch (e) { err = e; }
  check(err instanceof type, `${fn} throws ${type.name}, got ${err}`);
}

if (failures) {
  console.log(failures + ' failure(s)');
  process.exit(1);
}
console.log('ok');
