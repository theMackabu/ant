function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

const units = s => Array.from({ length: s.length }, (_, i) => s.charCodeAt(i).toString(16));

const emoji = String.fromCodePoint(0x1f600);
const hi = '\ud83d', lo = '\ude00';

check('replaceAll empty', units(emoji.replaceAll('', '-')), units('-' + hi + '-' + lo + '-'));
check('replace empty', units(emoji.replace('', '-')), units('-' + emoji));
check('replaceAll prefix', units(emoji.replaceAll('', '$`|')), units('|' + hi + hi + '|' + lo + emoji + '|'));
check('replaceAll suffix', units(emoji.replaceAll('', "$'|")), units(emoji + '|' + hi + lo + '|' + lo + '|'));
check('replaceAll match', units(emoji.replaceAll('', '$&|')), units('|' + hi + '|' + lo + '|'));
check('replaceAll fn positions', emoji.replaceAll('', (m, p) => '[' + p + ']').length, 2 + 9);
check('replaceAll fn', units(emoji.replaceAll('', (m, p) => '[' + p + ']')), units('[0]' + hi + '[1]' + lo + '[2]'));
check('split empty', emoji.split('').map(units), [['d83d'], ['de00']]);
check('split empty limit', ('a' + emoji + 'b').split('', 2).map(units), [['61'], ['d83d']]);

const mixed = 'a' + emoji + 'b';
check('mixed prefix', units(mixed.replaceAll('', '$`|')),
  ['7c', '61', '61', '7c', 'd83d', '61', 'd83d', '7c', 'de00', '61', 'd83d', 'de00', '7c', '62', '61', 'd83d', 'de00', '62', '7c']);
check('mixed suffix', units(mixed.replaceAll('', "$'|")),
  ['61', 'd83d', 'de00', '62', '7c', '61', 'd83d', 'de00', '62', '7c', 'd83d', 'de00', '62', '7c', 'de00', '62', '7c', '62', '7c']);
check('two astral suffix', units((emoji + emoji).replaceAll('', "<$'>")),
  ['3c', 'd83d', 'de00', 'd83d', 'de00', '3e', 'd83d', '3c', 'de00', 'd83d', 'de00', '3e', 'de00', '3c', 'd83d', 'de00', '3e', 'd83d', '3c', 'de00', '3e', 'de00', '3c', '3e']);
check('lone surrogates', units(('x' + hi + 'y' + lo).replaceAll('', '-')), ['2d', '78', '2d', 'd83d', '2d', '79', '2d', 'de00', '2d']);
check('pair from halves templates', units((hi + lo + 'z').replaceAll('', '[$`]')),
  ['5b', '5d', 'd83d', '5b', 'd83d', '5d', 'de00', '5b', 'd83d', 'de00', '5d', '7a', '5b', 'd83d', 'de00', '7a', '5d']);
check('non-empty search unaffected', units(mixed.replaceAll(emoji, "$`$'")), ['61', '61', '62', '62']);

console.log('ok');
