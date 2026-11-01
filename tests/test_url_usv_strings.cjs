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
same(new URL('http://h/\uD800?q=\uDC00#\uD800').href, 'http://h/%EF%BF%BD?q=%EF%BF%BD#%EF%BF%BD', 'URL input');

console.log('url usv strings ok');
