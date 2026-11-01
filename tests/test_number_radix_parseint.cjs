// Number.prototype.toString(radix) prints the shortest digits that read back
// as the same double (V8's output), parseInt skips Unicode whitespace and
// converts its radix with ToInt32, and btoa throws InvalidCharacterError.
// Found by tests/differential (number and buffer-base64 families).
function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + actual + ' !== ' + expected);
}

same((1e21).toString(16), '3635c9adc5dea00000', 'integer above 2^63');
same((2 ** 60).toString(36), '8rc4kbdvss00', 'digits past 2^53 are zeros');
same((2 ** 64).toString(2), '1' + '0'.repeat(64), 'power of two');
same((0.5).toString(2), '0.1', 'exact fraction');
same((0.1).toString(3), '0.0022002200220022002200220022002201', 'rounded fraction');
same((-255.5).toString(16), '-ff.8', 'negative');
same((5e-324).toString(2).length, 1076, 'smallest denormal');
same((255).toString('16'), 'ff', 'radix is converted with ToNumber');
for (const radix of [1, 37, NaN]) {
  let name = null;
  try { (1).toString(radix); } catch (e) { name = e.name; }
  same(name, 'RangeError', `radix ${radix}`);
}

for (const space of [' ', '﻿', ' ', '　', ' ', '\t\n'])
  same(parseInt(space + '12'), 12, `parseInt skips U+${space.charCodeAt(0).toString(16)}`);
same(parseInt('10', NaN), 10, 'NaN radix is 10');
same(parseInt('ff', '16'), 255, 'string radix');
same(parseInt('11', 2.9), 3, 'radix is truncated');
same(parseInt('123456789012345678901234'), 1.2345678901234568e23, 'long decimal is correctly rounded');
// powers of two round to nearest, ties to even, with a sticky tail
same(parseInt('1' + '0'.repeat(52) + '1', 2), 2 ** 53, 'binary tie rounds to even');
same(parseInt('1' + '0'.repeat(52) + '11', 2), 2 ** 54 + 4, 'binary above half rounds up');
same(parseInt('1' + '0'.repeat(52) + '1' + '0'.repeat(10) + '1', 2), 2 ** 64 + 2 ** 12, 'sticky tail rounds up');
same(parseInt('fffffffffffff800', 16), 18446744073709550000, 'hex tie');
same(parseInt('0x20000000000001'), 2 ** 53, 'hex prefix');

let error = null;
try { btoa('Ā'); } catch (e) { error = e; }
same(error instanceof DOMException && error.name, 'InvalidCharacterError', 'btoa outside Latin-1');

console.log('number radix and parseInt ok');
