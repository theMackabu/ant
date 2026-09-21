// A string containing a lone surrogate must still match on every RegExp path.
// Sticky matching (used by split) previously failed outright on such strings.
function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
const high = '\ud83d';
const low = '\udc1c';

const splits = [
  ['a ' + high, /(\s+)/, ['a', ' ', high]],
  [high + ' b', /(\s+)/, [high, ' ', 'b']],
  ['a ' + high + ' b', /\s+/, ['a', high, 'b']],
  ['x' + low + ',y,' + high, /,/, ['x' + low, 'y', high]],
  ['Words **bold** and `code` with unicode → café █ 🐜'.slice(0, 49), /(\s+|[{}()[\],;])/,
    ['Words', ' ', '**bold**', ' ', 'and', ' ', '`code`', ' ', 'with', ' ', 'unicode', ' ', '→', ' ', 'café', ' ', '█', ' ', high]],
];
for (const [input, re, expected] of splits) {
  const got = input.split(re);
  assert(same(got, expected), `split ${JSON.stringify(input)} by ${re}: ${JSON.stringify(got)}`);
}

const sticky = /\s+/y;
sticky.lastIndex = 1;
const stickyMatch = sticky.exec('a ' + high + ' b');
assert(stickyMatch && stickyMatch.index === 1 && sticky.lastIndex === 2, 'sticky exec before a lone surrogate');

const stickyDot = /./y;
const dot = stickyDot.exec(high);
assert(dot && dot[0] === high, 'sticky . matches a lone surrogate');

const stickyTest = /a/y;
assert(stickyTest.test('a' + low), 'sticky test on a string ending in a lone surrogate');

assert(('a' + high + 'b').replace(/b/, '_') === 'a' + high + '_', 'replace after a lone surrogate');
assert(same([...('a b' + high).matchAll(/\s/g)].map((m) => m.index), [1]), 'matchAll with a trailing lone surrogate');

// Byte-position advances must step over whole characters: continuation bytes such as
// 0x85 or 0xA0 would otherwise be read as U+0085 / U+00A0 and match \s.
assert(same('xÅy'.split(/\s/), ['xÅy']), 'split must not match inside Å');
assert(same('x Å y'.split(/(\s)/), ['x', ' ', 'Å', ' ', 'y']), 'split with capture around Å');
assert('Å'.replace(/\s*/g, '-') === '-Å-', 'global empty-match replace steps over Å');
assert([...'Å'.matchAll(/\s*/g)].length === 2, 'matchAll empty matches step over Å');
assert(same('Å'.match(/\s*/g), ['', '']), 'match /g empty matches step over Å');
assert(same(('a' + high + 'b').split(''), ['a', high, 'b']), 'split by empty string keeps the lone surrogate');

const invalid = Buffer.from([0x61, 0x20, 0xff, 0x20, 0x62]).toString('latin1');
assert(invalid.split(/\s+/).length === 3, 'non-surrogate text still splits');

console.log('PASS');
