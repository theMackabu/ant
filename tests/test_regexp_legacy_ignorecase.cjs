function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

const chars = ['ß', 'ẞ', 'ſ', 's', 'K', 'k', 'İ', 'i', 'ı', 'Ω', 'ω', 'Ω', 'µ', 'Μ', 'Å', 'å', 'Å'];
const subjects = [];
for (const a of chars) subjects.push(a, a + a);
for (const [a, b] of [['ß', 'ẞ'], ['ω', 'Ω'], ['å', 'Å'], ['s', 'ſ'], ['k', 'K']]) subjects.push(a + b, b + a);

const cases = {
  'ß': '10000000000000000000000000000000000000000000',
  '[ß]+': '11000000000000000000000000000000000000000000',
  '[ω]+': '00000000000000000000111100000000000000000000',
  '[å]+': '00000000000000000000000000000011110000000000',
  '[^ß]+': '00111111111111111111111111111111110011111111',
  '[^ω]{2}': '01010101010101010101000001010101011100111111',
  '\\u00df+': '11000000000000000000000000000000000000000000',
  '\\xdf': '10000000000000000000000000000000000000000000',
  '\\ß': '10000000000000000000000000000000000000000000',
  '[\\ß]': '10000000000000000000000000000000000000000000',
  '\\u2126+': '00000000000000000011000000000000000000000000',
  '[\\xe5\\u03c9]+': '00000000000000000000111100000011110000000000',
  '[à-ÿ]+': '00000000000000000000000000000011110000000000',
  '[α-ω]+': '00000000000000000000111111110000000000000000',
  '[℠-ℰ]+': '00000000110000000011000000001100000000000000',
  '[^à-ÿ]+': '11111111111111111111111111111100001111001111',
  '[a-zß]+': '11000011001100110000000000000000000000000000',
  '(?:ß|ω)+': '11000000000000000000111100000000000000000000',
  '(?<=ω)ω': '00000000000000000000000000000000000000000000',
};

for (const [pattern, expected] of Object.entries(cases)) {
  const re = new RegExp('^(?:' + pattern + ')$', 'i');
  check('/' + pattern + '/i', subjects.map(s => re.test(s) ? '1' : '0').join(''), expected);
}

check('omega group name', /(?<Ωx>a)/i.exec('A').groups.Ωx, 'A');
check('sharp s group name', /(?<ßx>a)\k<ßx>/i.test('aA'), true);
check('angstrom group name', new RegExp('(?<å>a)', 'i').exec('A').groups.å, 'A');
check('kelvin group name', /(?<Kx>k)/i.exec('K')[0], 'K');
check('named backreference', /(?<x>a)\k<x>/.test('aa'), true);
check('named backreference literal', /(?<x>a)\k<x>/.test('ak<x>'), false);
check('identity k escape', /\k/.test('k'), true);
check('k escape without groups', /\k<x>/.test('k<x>'), true);
check('empty class', [/[]/.test('a'), /a[]/.test('a'), /[^]/.test('\n')], [false, false, true]);
check('quantified class', /[ß]+/i.exec('ßẞ')[0], 'ß');
check('unicode flag folds', [/ß/iu.test('ẞ'), /[ω]/iu.test('Ω'), /ſ/iu.test('s')], [true, true, true]);

console.log('ok');
