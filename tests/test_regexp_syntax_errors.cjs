function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

function construct(pattern, flags) {
  try { new RegExp(pattern, flags); return 'ok'; } catch (e) { return `${e.name}: ${e.message}`; }
}

function literal(source) {
  try { (0, eval)(source); return 'ok'; } catch (e) { return `${e.name}: ${e.message}`; }
}

const invalid = [
  ['(', '', 'Unterminated group'],
  [')', '', "Unmatched ')'"],
  ['a\\', '', '\\ at end of pattern'],
  ['\\ß', 'u', 'Invalid escape'],
  ['\\-', 'u', 'Invalid escape'],
  ['a{2,1}', '', 'numbers out of order in {} quantifier'],
  ['(?<a>x)(?<a>y)', '', 'Duplicate capture group name'],
  ['(?<a>x)(?:(?<a>y))', '', 'Duplicate capture group name'],
  ['(?<1a>x)', '', 'Invalid capture group name'],
  ['(?<=a)*', '', 'Invalid quantifier'],
  ['(?=a)*', 'u', 'Invalid quantifier'],
  ['\\c', 'u', 'Invalid Unicode escape'],
  ['\\u{110000}', 'u', 'Invalid Unicode escape'],
  ['{', 'u', 'Lone quantifier brackets'],
  ['}', 'u', 'Lone quantifier brackets'],
  [']', 'u', 'Lone quantifier brackets'],
  ['a{1', 'u', 'Incomplete quantifier'],
  ['a**', '', 'Nothing to repeat'],
  ['*a', '', 'Nothing to repeat'],
  ['a{1}{2}', '', 'Nothing to repeat'],
  ['[z-a]', '', 'Range out of order in character class'],
  ['[\\d-z]', 'u', 'Invalid character class'],
  ['[', '', 'Unterminated character class', false],
  ['\\k<a>', 'u', 'Invalid named capture referenced'],
  ['(?<a>.)\\k<b>', '', 'Invalid named capture referenced'],
  ['(?<a>.)\\k', '', 'Invalid named reference'],
  ['\\1', 'u', 'Invalid escape'],
  ['\\00', 'u', 'Invalid decimal escape'],
  ['\\p{Foo}', 'u', 'Invalid property name'],
  ['[\\p{Foo}]', 'u', 'Invalid property name in character class'],
  ['(?x:a)', '', 'Invalid group'],
  ['(?ii:a)', '', 'Repeated flag in flag group'],
  ['(?-:a)', '', 'Invalid flag group'],
  ['[a&&&b]', 'v', 'Invalid character in character class'],
  ['[a&&b--c]', 'v', 'Invalid set operation in character class'],
  ['[(]', 'v', 'Invalid character in character class'],
  ['[^\\p{RGI_Emoji}]', 'v', 'Negated character class may contain strings'],
];

for (const [pattern, flags, message, literalOk] of invalid) {
  const sorted = [...flags].sort((a, b) => 'dgimsuvy'.indexOf(a) - 'dgimsuvy'.indexOf(b)).join('');
  check(`ctor /${pattern}/${flags}`, construct(pattern, flags), `SyntaxError: Invalid regular expression: /${pattern}/${sorted}: ${message}`);
  if (literalOk !== false && !pattern.includes('/') && !pattern.endsWith('\\') && pattern[0] !== '*') {
    check(`literal /${pattern}/${flags}`, literal(`/${pattern}/${flags}`), `SyntaxError: Invalid regular expression: /${pattern}/${flags}: ${message}`);
  }
}

check('ctor flags sorted', construct('(', 'ug'), 'SyntaxError: Invalid regular expression: /(/gu: Unterminated group');
check('literal flags as written', literal('/(/ug'), 'SyntaxError: Invalid regular expression: /(/ug: Unterminated group');
check('early error in uncalled function', literal('function f() { return /a**/; }'), 'SyntaxError: Invalid regular expression: /a**/: Nothing to repeat');
check('Function constructor', (() => { try { Function('return /[/'); return 'ok'; } catch (e) { return e.name; } })(), 'SyntaxError');
check('call without new', (() => { try { RegExp('('); return 'ok'; } catch (e) { return e.name; } })(), 'SyntaxError');
check('invalid flags first', construct('(', 'x'), "SyntaxError: Invalid flags supplied to RegExp constructor 'x'");

const valid = [
  ['(?<a>x)|(?<a>y)', ''], ['(?:(?<a>x)|(?<a>y))\\k<a>', ''], ['(?i:a)', ''], ['(?-i:a)', 'i'], ['(?i-ms:a)', ''],
  ['(?<=a+)b', ''], ['\\2', ''], ['\\8', ''], ['a{', ''], ['}', ''], [']', ''], ['\\c', ''], ['[\\d-z]', ''],
  ['\\k', ''], ['\\p{Foo}', ''], ['(?=a)*', ''], ['\\u{110000}', ''], ['[\\p{L}--\\p{Lu}]', 'v'],
  ['\\p{RGI_Emoji}', 'v'], ['[\\q{abc|d}]', 'v'], ['\\p{Script=Greek}', 'u'], ['(?<π>x)\\k<π>', ''],
  ['(?<\\u0061>x)', ''], ['\\00', ''], ['[\\b]', 'u'], ['^\\s*$', 'm'], ['a{2,1', ''],
];
for (const [pattern, flags] of valid) check(`valid /${pattern}/${flags}`, construct(pattern, flags), 'ok');

{
  const re = /a/g;
  re.lastIndex = 3;
  let threw = '';
  try { re.compile('('); } catch (e) { threw = e.name; }
  check('compile throws and keeps state', [threw, re.source, re.flags, re.lastIndex], ['SyntaxError', 'a', 'g', 3]);
  re.compile('b+', 'i');
  check('compile valid', [re.source, re.flags, re.test('xBB')], ['b+', 'i', true]);
}

function stringOp(fn) { try { return fn(); } catch (e) { return `${e.name}: ${e.message}`; } }
check('string match', stringOp(() => 'x'.match('(')), 'SyntaxError: Invalid regular expression: /(/: Unterminated group');
check('string search', stringOp(() => 'x'.search('[')), 'SyntaxError: Invalid regular expression: /[/: Unterminated character class');
check('string matchAll', stringOp(() => 'x'.matchAll('(')), 'SyntaxError: Invalid regular expression: /(/g: Unterminated group');
check('string match valid', 'a1'.match(1)[0], '1');
check('string search undefined', 'abc'.search(), 0);
check('string match none', 'abc'.match()[0], '');

let count = 0;
for (let i = 0; i < 2000; i++) {
  try { new RegExp('(' + (i % 3 ? '' : ')')); count++; } catch (e) { if (!(e instanceof SyntaxError)) throw e; }
}
check('loop validation', count, 667);

const dynamic = [];
for (let i = 0; i < 300; i++) dynamic.push(construct('a{' + (i % 7) + ',' + (i % 5) + '}', ''));
check('dynamic patterns', dynamic.filter(r => r !== 'ok').length, dynamic.length - [...Array(300).keys()].filter(i => i % 7 <= i % 5).length);

console.log('ok');
