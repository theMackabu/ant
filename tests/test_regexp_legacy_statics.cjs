function check(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

function statics() {
  return [
    RegExp.input, RegExp.$_, RegExp.lastMatch, RegExp['$&'], RegExp.lastParen, RegExp['$+'],
    RegExp.leftContext, RegExp['$`'], RegExp.rightContext, RegExp["$'"], RegExp.$1, RegExp.$2, RegExp.$9,
  ];
}

check('names', Object.getOwnPropertyNames(RegExp).filter(k => k !== 'length' && k !== 'name' && k !== 'prototype'), [
  'input', '$_', 'lastMatch', '$&', 'lastParen', '$+', 'leftContext', '$`', 'rightContext', "$'",
  '$1', '$2', '$3', '$4', '$5', '$6', '$7', '$8', '$9', 'escape',
]);

for (const key of ['input', '$_', 'lastMatch', '$&', 'lastParen', '$+', 'leftContext', '$`', 'rightContext', "$'", '$1', '$9']) {
  const d = Object.getOwnPropertyDescriptor(RegExp, key);
  check(`descriptor ${key}`, [typeof d.get, typeof d.set, d.enumerable, d.configurable, d.get.name, d.set.name, d.set.length],
    ['function', 'function', false, true, 'get ' + key, 'set ' + key, 1]);
}

/(a)(b)?/.exec('xxaby');
check('exec', statics(), ['xxaby', 'xxaby', 'ab', 'ab', 'b', 'b', 'xx', 'xx', 'y', 'y', 'a', 'b', '']);

/(a)(b)?/.exec('xxay');
check('unmatched last paren', [RegExp.lastParen, RegExp.$2, RegExp.leftContext, RegExp.rightContext], ['', '', 'xx', 'y']);

/(a)|(b)/.exec('b');
check('alternative paren', [RegExp.lastParen, RegExp.$1, RegExp.$2], ['b', '', 'b']);

/x/.exec('axb');
check('no captures', [RegExp.lastParen, RegExp.$1, RegExp.leftContext, RegExp.rightContext], ['', '', 'a', 'b']);

/b/.exec('😀b😀');
check('astral contexts', [RegExp.leftContext, RegExp.rightContext, RegExp.input], ['😀', '😀', '😀b😀']);

new RegExp('(1)(2)(3)(4)(5)(6)(7)(8)(9)(10)(11)').exec('1234567891011');
check('many groups', [RegExp.$9, RegExp.lastParen, RegExp.lastMatch], ['9', '11', '1234567891011']);

/(a)(b)?/.exec('xxaby');
RegExp.input = 'zz';
check('input setter', [RegExp.input, RegExp.$_, RegExp.lastMatch, RegExp.leftContext], ['zz', 'zz', 'ab', 'xx']);
RegExp.$_ = { toString() { return 'obj'; } };
check('alias setter', RegExp.input, 'obj');
RegExp.input = 5;
check('input setter coerces', [typeof RegExp.input, RegExp.input], ['string', '5']);
RegExp.lastMatch = 'q';
RegExp['$1'] = 'q';
check('other setters ignored', [RegExp.lastMatch, RegExp.$1], ['ab', 'a']);
let threw = false;
try { RegExp.input = Symbol(); } catch (e) { threw = e instanceof TypeError; }
check('symbol input', threw, true);

/c/.exec('abcd');
check('match resets input', [RegExp.input, RegExp.leftContext, RegExp.rightContext], ['abcd', 'ab', 'd']);

'xaby'.replace(/a(b)/g, 'Q');
check('replace', [RegExp.lastMatch, RegExp['$`'], RegExp["$'"], RegExp.lastParen], ['ab', 'x', 'y', 'b']);

'abc'.search('b');
check('string search', [RegExp.lastMatch, RegExp.leftContext], ['b', 'a']);

'abc'.match('c');
check('string match', [RegExp.lastMatch, RegExp.leftContext, RegExp.input], ['c', 'ab', 'abc']);

'q'.split(/q/);
check('split', [RegExp.lastMatch, RegExp.input], ['q', 'q']);

'a-b-c'.replace(/-/g, '+');
check('literal replace', [RegExp.lastMatch, RegExp.leftContext, RegExp.rightContext], ['-', 'a-b', 'c']);

/(\d+)/g.test('ab 12 cd');
check('test', [RegExp.$1, RegExp.leftContext, RegExp.rightContext], ['12', 'ab ', ' cd']);

/nomatch/.exec('abc');
check('failed match keeps statics', [RegExp.$1, RegExp.lastMatch], ['12', '12']);

check('getter receiver', Object.getOwnPropertyDescriptor(RegExp, 'lastMatch').get.call({}), '12');

console.log('ok');
