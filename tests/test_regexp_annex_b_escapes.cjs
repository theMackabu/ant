// Annex B and surrogate escapes the JS -> PCRE2 translation once got wrong;
// found by tests/fuzz/fuzz_regexp.c. Expected values match V8.
function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + actual + ' !== ' + expected);
}

const cases = [
  // \N past the last group is a legacy octal escape; \8 and \9 are digits
  ['a\\2', '', 'a\x02', true],
  ['(a)|\\1b\\2', '', 'b\x02', true],
  ['(a)\\10', '', 'a\b', true],
  ['\\8\\9', '', '89', true],
  ['\\1(a)', '', 'a', true],
  ['\\377\\400', '', '\xff\x200', true],
  // with enough groups, multi-digit escapes are backreferences
  ['^(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10$', '', 'abcdefghijj', true],
  ['^(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10$', '', 'abcdefghij\b', false],
  ['^(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10$', 'u', 'abcdefghijj', true],
  ['^(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)\\12$', '', 'abcdefghijkll', true],
  ['^(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\11$', '', 'abcdefghij\t', true],
  ['^(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\100$', '', 'abcdefghij@', true],
  ['^(a)[\\10]$', '', 'a\b', true],
  // including forward references, which PCRE2 alone would read as octal
  ['^\\10(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)$', '', 'abcdefghij', true],
  ['^\\10(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)$', '', '\babcdefghij', false],
  ['^\\10(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)$', 'u', 'abcdefghij', true],
  ['^(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10$', 'i', 'abcdefghijJ', true],
  // a surrogate pair escape is one code point
  ['\\uD83D\\uDE00', '', '\u{1F600}', true],
  ['\\uD83D\\uDE00', 'u', '\u{1F600}', true],
  ['[\\uD83D\\uDE00]', 'u', '\u{1F600}', true],
  ['^\\uD83D\\uDE00$', 'v', '\u{1F600}', true],
  // without u or v, \p and \u{...} are identity escapes and braces count
  ['\\p{L}', '', 'a', false],
  ['^\\p{L}$', '', 'p{L}', true],
  ['^\\P{Scri}$', '', 'P{Scri}', true],
  ['[\\p{L}]', '', '{', true],
  ['\\p{L}', 'u', 'a', true],
  ['^\\u{2}$', '', 'uu', true],
  ['\\u{41}', '', 'A', false],
  ['[\\u{41}]', '', '{', true],
  ['\\u{41}', 'u', 'A', true],
  // { only opens {n}, {n,} and {n,m}
  ['^a{,5}$', '', 'a{,5}', true],
  ['^a{,5}$', '', 'aa', false],
  ['^x{a}$', '', 'x{a}', true],
  ['^x{1,}$', '', 'xx', true],
  // classes only nest with v; \B in a class is B
  ['[[]a', '', '[a', true],
  ['^[[]x[]|y$', '', 'y', true],
  ['^[[:alpha:]]$', '', ':]', true],
  ['[[:alpha:]]', '', 'b]', false],
  ['[\\B]', '', 'B', true],
  // PCRE2 reads [: [. [= as POSIX syntax; in JS they are literal characters
  ['^[:alpha:]$', '', 'h', true],
  ['^[\\:alpha:]]$', '', ':]', true],
  ['^[=]$', '', '=', true],
  ['^[.a]+$', '', 'a.', true],
];

for (const [source, flags, input, expected] of cases) {
  same(new RegExp(source, flags).test(input), expected, `/${source}/${flags}.test(${JSON.stringify(input)})`);
}

console.log('regexp annex b escapes ok');
