function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

const subjects = {
  lf: 'ab\ncd\n\nef\n',
  crlf: 'ab\r\ncd\r\n\r\nef\r\n',
  cr: 'ab\rcd\r\ref\r',
  ls: 'ab\u2028cd\u2029ef',
  mixed: 'ab\ncd\re\u2028f',
  none: 'abcdef',
  nonascii: 'é\nñ\nü',
};

const expected = {
  lf: ['0,3,6,7,10', '[ab]\n[cd]\n[]\n[ef]\n[]', 'ab|cd||ef|', '2,5,6,9,10', '0,3,7', 'x', 2],
  crlf: ['0,3,4,7,8,9,10,13,14', '[ab]\r[]\n[cd]\r[]\n[]\r[]\n[ef]\r[]\n[]', 'ab||cd||||ef||', '2,3,6,7,8,9,12,13,14', '0,4,10', 'x', 2],
  cr: ['0,3,6,7,10', '[ab]\r[cd]\r[]\r[ef]\r[]', 'ab|cd||ef|', '2,5,6,9,10', '0,3,7', 'x', 2],
  ls: ['0,3,6', '[ab]\u2028[cd]\u2029[ef]', 'ab|cd|ef', '2,5,8', '0,3,6', 'x', 2],
  mixed: ['0,3,6,8', '[ab]\n[cd]\r[e]\u2028[f]', 'ab|cd|e|f', '2,5,7,9', '0,3,6,8', 'x', 2],
  none: ['0', '[abcdef]', 'abcdef', '6', '0', 'y', 2],
  nonascii: ['0,2,4', '[é]\n[ñ]\n[ü]', 'é|ñ|ü', '1,3,5', '', 'y', 2],
};

for (const [name, s] of Object.entries(subjects)) {
  const starts = [...s.matchAll(/^/gm)].map(m => m.index).join();
  const wrapped = s.replace(/^(.*)$/gm, '[$1]');
  const lines = s.match(/^.*$/gm).join('|');
  const ends = [...s.matchAll(/$/gm)].map(m => m.index).join();
  const words = [...s.matchAll(/^[a-z]+/gm)].map(m => m.index).join();
  const sticky = (() => { const re = /^c/my; re.lastIndex = s.indexOf('c'); return re.test(s) ? 'x' : 'y'; })();
  const split = s.split(/^/m).length > 1 ? 2 : 1;
  const got = [starts, wrapped, lines, ends, words, sticky, s.length > 6 ? split : 2];
  check(name, got, expected[name]);
}

check('caret after cr', /^b/m.test('a\rb'), true);
check('caret after ls', /^b/m.test('a\u2028b'), true);
check('caret after ps', /^b/m.exec('a\u2029b').index, 2);
check('caret after lf', /^b/m.test('a\nb'), true);
check('caret mid line', /^b/m.test('ab'), false);
check('dollar before cr', /a$/m.test('a\rb'), true);
check('dollar mid line', /a$/m.test('ab'), false);
check('non multiline caret', /^b/.test('a\nb'), false);
check('class caret', /[\^]b/m.test('^b'), true);
check('escaped caret', /\^b/m.test('a\n^b'), true);

const re = /^\s*(\d+)/gm;
const text = 'x\n  12\n 7\r3\n\u2028 9';
check('mixed exec loop', [...text.matchAll(re)].map(m => m[1] + '@' + m.index).join(), '12@2,7@7,3@10,9@12');
check('same regex lf subject', [...'a\n 1\n2'.matchAll(re)].map(m => m[1]).join(), '1,2');
check('same regex cr subject', [...'a\r 1\r2'.matchAll(re)].map(m => m[1]).join(), '1,2');

console.log('ok');
