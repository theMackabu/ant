// URL and URLSearchParams take USVStrings: each lone surrogate becomes one
// U+FFFD before parsing, and percent-decoded NULs stay in keys. Found by
// tests/differential (url family). Expected values match V8.
function same(actual, expected, message) {
  if (actual !== expected) throw new Error(message + ': ' + actual + ' !== ' + expected);
}

const p = new URLSearchParams('a=\uD800&\uDFFFb=1&c%00d=%00');
p.append('x\uD83D', 'y\uDE00z');
p.set('pair', '\uD83D\uDE00');
same(JSON.stringify([...p]), '[["a","\uFFFD"],["\uFFFDb","1"],["c\\u0000d","\\u0000"],["x\uFFFD","y\uFFFDz"],["pair","\u{1F600}"]]', 'entries');
same(p.toString(), 'a=%EF%BF%BD&%EF%BF%BDb=1&c%00d=%00&x%EF%BF%BD=y%EF%BF%BDz&pair=%F0%9F%98%80', 'serialization');
same(p.has('x\uFFFD'), true, 'lookup by replaced key');
same(new URLSearchParams({ 'k\uD800': 'v' }).toString(), 'k%EF%BF%BD=v', 'record keys');
same(new URLSearchParams([['\uD800', '\uDC00']]).toString(), '%EF%BF%BD=%EF%BF%BD', 'sequence pairs');
same(JSON.stringify([...new URLSearchParams('a=1\0b&\0=\0')]), '[["a","1\\u0000b"],["\\u0000","\\u0000"]]', 'raw NULs in the query');

// names and values compare in full, NULs included
const nul = new URLSearchParams('a%00b=1&a%00c=2&a=3');
same(nul.get('a\0c'), '2', 'get past a NUL');
same(nul.get('a'), '3', 'a prefix before a NUL is a different name');
same(JSON.stringify(nul.getAll('a\0b')), '["1"]', 'getAll past a NUL');
same(nul.has('a\0b', '1'), true, 'has with a value');
same(nul.has('a\0b', '2'), false, 'has with another entry\'s value');
nul.set('a\0b', 'x');
same(nul.toString(), 'a%00b=x&a%00c=2&a=3', 'set replaces only its own name');
nul.delete('a\0c');
same(nul.toString(), 'a%00b=x&a=3', 'delete removes only its own name');
const values = new URLSearchParams('k=v%00a&k=v%00b');
values.delete('k', 'v\0b');
same(values.toString(), 'k=v%00a', 'delete by a value past a NUL');
const order = new URLSearchParams('a%00b=1&a=2&a%00a=3');
order.sort();
same(order.toString(), 'a=2&a%00a=3&a%00b=1', 'sort past a NUL');
// sort compares UTF-16 code units: a surrogate pair sorts before U+E000
const units = new URLSearchParams([['\uE000', '1'], ['\u{10000}', '2'], ['\uD7FF', '3'], ['a', '4'], ['a\0\uE000', '5'], ['a\0\u{10000}', '6']]);
units.sort();
same(units.toString(), 'a=4&a%00%F0%90%80%80=6&a%00%EE%80%80=5&%ED%9F%BF=3&%F0%90%80%80=2&%EE%80%80=1', 'sort by UTF-16 code units');

same(new URL('http://h/\uD800?q=\uDC00#\uD800').href,'http://h/%EF%BF%BD?q=%EF%BF%BD#%EF%BF%BD', 'URL input');

console.log('url usv strings ok');
