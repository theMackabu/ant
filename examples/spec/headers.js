import { test, testThrows, testDeep, summary } from './helpers.js';

console.log('Headers constructor\n');

const h0 = new Headers();
test('empty constructor', h0.has('x-foo'), false);

const h1 = new Headers({ 'Content-Type': 'text/html', 'x-custom': 'val' });
test('init from record', h1.get('content-type'), 'text/html');
test('record key case-folded', h1.get('Content-Type'), 'text/html');

const h2 = new Headers([['x-a', '1'], ['x-b', '2']]);
test('init from sequence', h2.get('x-a'), '1');
test('init from sequence second', h2.get('x-b'), '2');

const h3 = new Headers(new Headers({ 'x-copy': 'yes' }));
test('init from another Headers', h3.get('x-copy'), 'yes');

testThrows('requires new', () => Headers());
testThrows('null init throws', () => new Headers(null));
testThrows('string init throws', () => new Headers('bad'));
testThrows('number init throws', () => new Headers(42));

console.log('\nappend / get / has / delete / set\n');

const h = new Headers();

h.append('x-foo', 'one');
test('append then get', h.get('x-foo'), 'one');

h.append('x-foo', 'two');
test('append combines values', h.get('x-foo'), 'one, two');

test('has returns true', h.has('x-foo'), true);
test('has case-insensitive', h.has('X-FOO'), true);
test('has missing returns false', h.has('x-bar'), false);

h.set('x-foo', 'replaced');
test('set replaces combined value', h.get('x-foo'), 'replaced');

h.delete('x-foo');
test('delete removes header', h.has('x-foo'), false);
test('get deleted returns null', h.get('x-foo'), null);

console.log('\nset-cookie special case\n');

const hc = new Headers();
hc.append('set-cookie', 'a=1');
hc.append('set-cookie', 'b=2');
test('set-cookie not combined', hc.get('set-cookie'), 'a=1');

const cookies = hc.getSetCookie ? hc.getSetCookie() : null;
if (cookies) {
  test('getSetCookie length', cookies.length, 2);
  test('getSetCookie first', cookies[0], 'a=1');
  test('getSetCookie second', cookies[1], 'b=2');
}

console.log('\nvalue normalization\n');

const hn = new Headers();
hn.set('x-trim', '  hello  ');
test('leading/trailing whitespace stripped', hn.get('x-trim'), 'hello');

hn.set('x-tab', '\thello\t');
test('leading/trailing tab stripped', hn.get('x-tab'), 'hello');

console.log('\ninvalid name / value\n');

testThrows('empty name throws', () => h.set('', 'val'));
testThrows('name with space throws', () => h.set('x bad', 'val'));
testThrows('name with colon throws', () => h.set('x:bad', 'val'));

console.log('\nforEach\n');

const hf = new Headers({ 'b-key': 'bval', 'a-key': 'aval' });
const seen = [];
hf.forEach((val, name) => seen.push(`${name}:${val}`));
test('forEach visits all entries', seen.length, 2);
test('forEach sorted order', seen[0], 'a-key:aval');
test('forEach sorted order 2', seen[1], 'b-key:bval');

console.log('\nentries / keys / values iteration\n');

const hi = new Headers({ 'b-hdr': '2', 'a-hdr': '1' });

const entries = [...hi.entries()];
test('entries sorted', entries[0][0], 'a-hdr');
test('entries value', entries[0][1], '1');
test('entries length', entries.length, 2);

const keys = [...hi.keys()];
test('keys sorted', keys[0], 'a-hdr');
test('keys length', keys.length, 2);

const vals = [...hi.values()];
test('values order matches keys', vals[0], '1');

console.log('\nlive iteration\n');

const hl = new Headers({ 'x-a': '1' });
const iter = hl.entries();
hl.set('x-b', '2');
const all = [];
for (const e of { [Symbol.iterator]: () => iter }) all.push(e[0]);
test('live iterator sees added key', all.includes('x-b'), true);

console.log('\niterator prototype chain\n');

const it = new Headers().entries();
const iterProto = Object.getPrototypeOf(Object.getPrototypeOf(it));
test('iterator proto chain has Symbol.iterator', typeof iterProto[Symbol.iterator], 'function');

summary();
