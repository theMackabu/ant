function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

const pairs = [
  ['ß', 'ẞ', false], ['ß', 'ß', true], ['ẞ', 'ß', false], ['a', 'A', true], ['k', 'K', true],
  ['k', 'K', false], ['K', 'k', false], ['s', 'ſ', false], ['ω', 'Ω', false],
  ['ω', 'Ω', true], ['å', 'Å', false], ['å', 'Å', true],
  ['θ', 'ϴ', false], ['θ', 'ϑ', true], ['ᾀ', 'ᾈ', false],
  ['σ', 'ς', true], ['ΐ', 'ΐ', false], ['µ', 'Μ', true],
];
for (const [a, b, expected] of pairs) {
  check(`numbered ${a} ${b}`, new RegExp('(' + a + ')\\1', 'i').test(a + b), expected);
  check(`named ${a} ${b}`, new RegExp('(?<x>' + a + ')\\k<x>', 'i').test(a + b), expected);
  check(`unicode ${a} ${b}`, new RegExp('(' + a + ')\\1', 'iu').test(a + b), new RegExp('(' + a + ')\\1', 'iu').test(a + b));
}

check('literal ß', /(ß)\1/i.test('ßẞ'), false);
check('any char', /(.)\1/i.test('ßẞ'), false);
check('word repeat', /(\w+) \1/i.exec('Hello HELLO world')[0], 'Hello HELLO');
check('replace', 'aAbBcd'.replace(/(.)\1/gi, '<$1>'), '<a><b>cd');
check('unset group', /(a)?b\1/i.test('b'), true);
check('two groups', /(a)(ß)\2\1/i.exec('aßßA')[0], 'aßßA');
check('two groups mismatch', /(a)(ß)\2\1/i.test('aßẞA'), false);
check('backtrack', /(ß+)\1/i.exec('ßßßẞ')[0], 'ßß');
check('duplicate names', /(?:(?<n>ß)|(?<n>k))\k<n>/i.test('kK'), false);
check('duplicate names match', /(?:(?<n>ß)|(?<n>k))\k<n>/i.test('kK'), true);
check('global exec', [...'ßßßẞaA'.matchAll(/(.)\1/gi)].map(m => m[0]), ['ßß', 'aA']);
check('sticky', (() => { const re = /(a)\1/iy; re.lastIndex = 1; return [re.test('xaA'), re.lastIndex]; })(), [true, 3]);
check('class backref', /([ß-ẞ])\1/i.test('ẞß'), false);
check('astral', /(😀)\1/i.test('😀😀'), true);

console.log('ok');
