import { test, testThrows, testDeep, summary } from './helpers.js';

console.log('URL constructor\n');

const url = new URL('https://user:pass@example.com:8080/path/to?query=value#hash');
test('full URL href', url.href, 'https://user:pass@example.com:8080/path/to?query=value#hash');

const url2 = new URL('/other', 'https://example.com');
test('URL with base', url2.href, 'https://example.com/other');

const url2b = new URL('foo', 'https://example.com/bar/baz');
test('URL with base resolves relative', url2b.href, 'https://example.com/bar/foo');

const url2c = new URL('//other.com/path', 'https://example.com');
test('URL with base protocol-relative', url2c.href, 'https://other.com/path');

testThrows('requires new', () => URL());
testThrows('invalid URL throws TypeError', () => new URL('not a url'));
testThrows('invalid URL with base throws', () => new URL('http://%', 'https://example.com'));

test('toString() returns href', url.toString(), url.href);
test('toJSON() returns href', url.toJSON(), url.href);

test('URL.canParse valid', URL.canParse('https://example.com'), true);
test('URL.canParse invalid', URL.canParse('not a url'), false);
test('URL.canParse with base', URL.canParse('/path', 'https://example.com'), true);

const parsed = URL.parse('https://example.com/test');
test('URL.parse valid returns URL', parsed instanceof URL, true);
test('URL.parse valid href', parsed.href, 'https://example.com/test');
test('URL.parse invalid returns null', URL.parse('not a url'), null);

const parsedBase = URL.parse('/path', 'https://example.com');
test('URL.parse with base', parsedBase.href, 'https://example.com/path');

console.log('\nURL properties (getters)\n');

test('protocol', url.protocol, 'https:');
test('username', url.username, 'user');
test('password', url.password, 'pass');
test('hostname', url.hostname, 'example.com');
test('host (hostname:port)', url.host, 'example.com:8080');
test('port', url.port, '8080');
test('pathname', url.pathname, '/path/to');
test('search', url.search, '?query=value');
test('hash', url.hash, '#hash');
test('origin', url.origin, 'https://example.com:8080');

const urlNoSearch = new URL('https://example.com/path');
test('search empty string when absent', urlNoSearch.search, '');

const urlNoHash = new URL('https://example.com/path');
test('hash empty string when absent', urlNoHash.hash, '');

const urlDefaultHost = new URL('https://example.com/');
test('host without port', urlDefaultHost.host, 'example.com');

const urlNoUser = new URL('https://example.com/');
test('username empty when absent', urlNoUser.username, '');
test('password empty when absent', urlNoUser.password, '');

console.log('\nURL property setters\n');

const us1 = new URL('https://example.com/old');
us1.href = 'https://other.com/new?q=1';
test('set href reparses', us1.hostname, 'other.com');
test('set href pathname', us1.pathname, '/new');
test('set href search', us1.search, '?q=1');

const us2 = new URL('https://example.com/path');
us2.protocol = 'http:';
test('set protocol', us2.protocol, 'http:');

const us3 = new URL('https://example.com/path');
us3.hostname = 'other.com';
test('set hostname', us3.hostname, 'other.com');
test('set hostname updates host', us3.host, 'other.com');

const us4 = new URL('https://example.com/path');
us4.port = '9090';
test('set port', us4.port, '9090');
test('set port updates host', us4.host, 'example.com:9090');

const us4b = new URL('https://example.com:9090/path');
us4b.port = '443';
test('set default port elides', us4b.port, '');

const us5 = new URL('https://example.com/old');
us5.pathname = '/new/path';
test('set pathname', us5.pathname, '/new/path');

const us6 = new URL('https://example.com/path');
us6.search = '?x=1&y=2';
test('set search', us6.search, '?x=1&y=2');

const us6b = new URL('https://example.com/path?old=1');
us6b.search = '';
test('set search empty clears', us6b.search, '');

const us7 = new URL('https://example.com/path');
us7.hash = '#section';
test('set hash', us7.hash, '#section');

const us7b = new URL('https://example.com/path#old');
us7b.hash = '';
test('set hash empty clears', us7b.hash, '');

const us8 = new URL('https://example.com/path');
us8.username = 'alice';
test('set username', us8.username, 'alice');

const us9 = new URL('https://example.com/path');
us9.password = 'secret';
test('set password', us9.password, 'secret');

console.log('\ndefault port handling\n');

const httpDefault = new URL('http://example.com:80/');
test('http port 80 elided', httpDefault.port, '');
test('http port 80 host omits port', httpDefault.host, 'example.com');

const httpsDefault = new URL('https://example.com:443/');
test('https port 443 elided', httpsDefault.port, '');
test('https port 443 host omits port', httpsDefault.host, 'example.com');

const httpNonDefault = new URL('http://example.com:3000/');
test('http non-default port shown', httpNonDefault.port, '3000');
test('http non-default host includes port', httpNonDefault.host, 'example.com:3000');

const httpsNonDefault = new URL('https://example.com:8443/');
test('https non-default port shown', httpsNonDefault.port, '8443');

console.log('\nURLSearchParams constructor\n');

const sp0 = new URLSearchParams();
test('empty constructor toString', sp0.toString(), '');
test('empty constructor size', sp0.size, 0);

const sp1 = new URLSearchParams('a=1&b=2');
test('from string get a', sp1.get('a'), '1');
test('from string get b', sp1.get('b'), '2');
test('from string size', sp1.size, 2);

const sp1b = new URLSearchParams('?a=1&b=2');
test('from string with leading ?', sp1b.get('a'), '1');
test('from string with leading ? get b', sp1b.get('b'), '2');

const sp2 = new URLSearchParams([
  ['a', '1'],
  ['b', '2']
]);
test('from array get a', sp2.get('a'), '1');
test('from array get b', sp2.get('b'), '2');
test('from array size', sp2.size, 2);

const sp3 = new URLSearchParams({ a: '1', b: '2' });
test('from object get a', sp3.get('a'), '1');
test('from object get b', sp3.get('b'), '2');

const sp4 = new URLSearchParams(new URLSearchParams('x=9&y=8'));
test('from another URLSearchParams get x', sp4.get('x'), '9');
test('from another URLSearchParams get y', sp4.get('y'), '8');

testThrows('pair with 1 element throws', () => new URLSearchParams([['only_key']]));
testThrows('pair with 3 elements throws', () => new URLSearchParams([['a', 'b', 'c']]));

console.log('\nURLSearchParams methods\n');

const sp = new URLSearchParams('a=1&b=2&a=3');

test('get returns first value', sp.get('a'), '1');
test('get missing returns null', sp.get('z'), null);

testDeep('getAll returns all values', sp.getAll('a'), ['1', '3']);
testDeep('getAll missing returns empty', sp.getAll('z'), []);

test('has returns true', sp.has('a'), true);
test('has missing returns false', sp.has('z'), false);

test('has with matching value', sp.has('a', '1'), true);
test('has with non-matching value', sp.has('a', '999'), false);

const spSet = new URLSearchParams('a=1&b=2&a=3');
spSet.set('a', '99');
test('set replaces all', spSet.get('a'), '99');
test('set getAll length', spSet.getAll('a').length, 1);
test('set preserves others', spSet.get('b'), '2');

const spApp = new URLSearchParams('a=1');
spApp.append('a', '2');
test('append adds entry', spApp.getAll('a').length, 2);
test('append second value', spApp.getAll('a')[1], '2');

const spDel = new URLSearchParams('a=1&b=2&a=3');
spDel.delete('a');
test('delete removes all with name', spDel.has('a'), false);
test('delete preserves others', spDel.has('b'), true);

const spDelVal = new URLSearchParams('a=1&a=2&a=3');
spDelVal.delete('a', '2');
test('delete with value removes specific', spDelVal.getAll('a').length, 2);
test('delete with value keeps others', spDelVal.get('a'), '1');

const spSort = new URLSearchParams('c=3&a=1&b=2&a=4');
spSort.sort();
test('sort first key', [...spSort.keys()][0], 'a');
test('sort second key', [...spSort.keys()][1], 'a');
test('sort third key', [...spSort.keys()][2], 'b');
test('sort fourth key', [...spSort.keys()][3], 'c');
test('sort stable: first a value', spSort.getAll('a')[0], '1');
test('sort stable: second a value', spSort.getAll('a')[1], '4');

test('toString serializes', new URLSearchParams('a=1&b=2').toString(), 'a=1&b=2');
test('toString empty', new URLSearchParams().toString(), '');

const spFE = new URLSearchParams('a=1&b=2');
const feResults = [];
spFE.forEach((value, name) => feResults.push(`${name}=${value}`));
test('forEach visits all', feResults.length, 2);
test('forEach first', feResults[0], 'a=1');
test('forEach second', feResults[1], 'b=2');

test('size getter', new URLSearchParams('a=1&b=2&c=3').size, 3);
test(
  'size after append',
  (() => {
    const s = new URLSearchParams('a=1');
    s.append('b', '2');
    return s.size;
  })(),
  2
);

console.log('\nURLSearchParams encoding\n');

test('space encoded as +', new URLSearchParams([['k', 'hello world']]).toString(), 'k=hello+world');
test('+ in value encoded as %2B', new URLSearchParams([['k', 'a+b']]).toString(), 'k=a%2Bb');
test('= in value encoded', new URLSearchParams([['k', 'a=b']]).toString(), 'k=a%3Db');
test('& in value encoded', new URLSearchParams([['k', 'a&b']]).toString(), 'k=a%26b');

test('* passes through', new URLSearchParams([['k', '*']]).toString(), 'k=*');
test('- passes through', new URLSearchParams([['k', '-']]).toString(), 'k=-');
test('. passes through', new URLSearchParams([['k', '.']]).toString(), 'k=.');
test('_ passes through', new URLSearchParams([['k', '_']]).toString(), 'k=_');

test('~ is percent-encoded', new URLSearchParams([['k', '~']]).toString(), 'k=%7E');

test('parsing: + becomes space', new URLSearchParams('k=hello+world').get('k'), 'hello world');
test('parsing: %20 becomes space', new URLSearchParams('k=hello%20world').get('k'), 'hello world');
test('parsing: %2B becomes +', new URLSearchParams('k=a%2Bb').get('k'), 'a+b');

console.log('\nURLSearchParams iterator protocol\n');

const spi = new URLSearchParams('a=1&b=2');

const entries = [...spi.entries()];
test('entries length', entries.length, 2);
test('entries first', entries[0][0], 'a');
test('entries first value', entries[0][1], '1');

const keys = [...spi.keys()];
test('keys length', keys.length, 2);
test('keys first', keys[0], 'a');
test('keys second', keys[1], 'b');

const vals = [...spi.values()];
test('values length', vals.length, 2);
test('values first', vals[0], '1');
test('values second', vals[1], '2');

test('Symbol.iterator is entries', spi[Symbol.iterator] === spi.entries, true);

const forOfResults = [];
for (const [name, value] of new URLSearchParams('x=10&y=20')) {
  forOfResults.push(`${name}=${value}`);
}
test('for...of first', forOfResults[0], 'x=10');
test('for...of second', forOfResults[1], 'y=20');

const spread = [...new URLSearchParams('p=1&q=2')];
test('spread length', spread.length, 2);
test('spread first pair', spread[0][0], 'p');
test('spread first value', spread[0][1], '1');

console.log('\nURL-URLSearchParams bidirectional sync\n');

const syncUrl = new URL('https://example.com/?a=1');
syncUrl.searchParams.set('b', '2');
test('searchParams mutation updates search', syncUrl.search, '?a=1&b=2');

syncUrl.searchParams.delete('a');
test('searchParams delete updates search', syncUrl.search, '?b=2');

syncUrl.searchParams.append('c', '3');
test('searchParams append updates search', syncUrl.search, '?b=2&c=3');

const syncUrl2 = new URL('https://example.com/?x=1');
syncUrl2.search = '?y=2&z=3';
test('set search updates searchParams get y', syncUrl2.searchParams.get('y'), '2');
test('set search updates searchParams get z', syncUrl2.searchParams.get('z'), '3');
test('set search removes old param', syncUrl2.searchParams.has('x'), false);

const syncUrl3 = new URL('https://example.com/?a=1');
const paramsRef1 = syncUrl3.searchParams;
syncUrl3.search = '?b=2';
const paramsRef2 = syncUrl3.searchParams;
test('searchParams identity preserved', paramsRef1 === paramsRef2, true);

console.log('\ntoStringTag\n');

test('URL toStringTag', Object.prototype.toString.call(new URL('https://example.com')), '[object URL]');
test('URLSearchParams toStringTag', Object.prototype.toString.call(new URLSearchParams()), '[object URLSearchParams]');

summary();
