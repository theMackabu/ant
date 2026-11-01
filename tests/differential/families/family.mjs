import fc from 'fast-check';

const codeUnitPool = [
  'a', 'Z', '0', '9', ' ', '\t', '\n', '\r', '"', "'", '\\', '/', '<', '&', '=', '+', '%', '.', '-', '_', '$',
  '\u0000', '\u001f', '\u007f', ' ', 'é', 'ß', 'İ', 'ﬀ', 'Σ', 'ς', 'ǅ',
  ' ', ' ', '﻿', '€', '中', '\u{1F600}', '\u{10FFFF}', '\uD800', '\uDFFF', '\uD83D', '\uDE00'
];

export const unit = fc.oneof({ weight: 4, arbitrary: fc.constantFrom(...codeUnitPool) }, { weight: 1, arbitrary: fc.string({ unit: 'binary', minLength: 1, maxLength: 1 }) });
export const text = maxLength => fc.string({ unit, maxLength });

export const doubleValue = fc.oneof(
  fc.double(),
  fc.integer({ min: -1e6, max: 1e6 }),
  fc.constantFrom(0, -0, 0.1, 1 / 3, 2 ** 31 - 1, -(2 ** 31), 2 ** 32, 2 ** 53, 2 ** 53 + 2, 1e21, 1e-7, 5e-324, Number.MAX_VALUE)
);
