const assert = require('node:assert');
const url = require('node:url');

const keys = ['protocol', 'slashes', 'auth', 'host', 'port', 'hostname', 'hash', 'search', 'query', 'pathname', 'path', 'href'];

assert.strictEqual(typeof url.Url, 'function');
assert.strictEqual(typeof url.resolve, 'function');
assert.strictEqual(typeof url.resolveObject, 'function');
assert.strictEqual(typeof url.urlToHttpOptions, 'function');
assert.deepStrictEqual(Object.getOwnPropertyNames(url.Url.prototype), ['constructor', 'parse', 'format', 'resolve', 'resolveObject', 'parseHost']);
assert.deepStrictEqual(Object.keys(new url.Url()), keys);

const parsed = url.parse('http://user:pass@Host.COM:8080/p/a/t/h?query=string#hash');
assert.ok(parsed instanceof url.Url);
assert.strictEqual(Object.getPrototypeOf(parsed), url.Url.prototype);
assert.deepStrictEqual(Object.keys(parsed), keys);
assert.strictEqual(String(parsed), '[object Object]');
assert.strictEqual(parsed.format(), 'http://user:pass@host.com:8080/p/a/t/h?query=string#hash');
assert.strictEqual(url.parse(parsed), parsed);
assert.strictEqual(parsed.resolve('../x'), 'http://user:pass@host.com:8080/p/a/x');

const query = url.parse('http://a/b?x=1&x=2&y', true).query;
assert.strictEqual(Object.getPrototypeOf(query), null);
assert.deepStrictEqual({ ...query }, { x: ['1', '2'], y: '' });
assert.strictEqual(Object.getPrototypeOf(url.parse('http://a/b', true).query), null);
assert.strictEqual(url.parse('/p', true).search, null);

const parseCases = [
  ['  HTTP://a.com\\b\\c?x\\y#z\\w  ', ['http:', true, null, 'a.com', null, 'a.com', '#z%5Cw', '?x%5Cy', 'x%5Cy', '/b/c', '/b/c?x%5Cy', 'http://a.com/b/c?x%5Cy#z%5Cw']],
  ['//some.host/path', [null, null, null, null, null, null, null, null, null, '//some.host/path', '//some.host/path', '//some.host/path']],
  ['mailto:user@example.com', ['mailto:', null, 'user', 'example.com', null, 'example.com', null, null, null, null, null, 'mailto:user@example.com']],
  ['javascript:alert(1)', ['javascript:', null, null, null, null, null, null, null, null, 'alert(1)', 'alert(1)', 'javascript:alert(1)']],
  ['http://[::1]:3000/x', ['http:', true, null, '[::1]:3000', '3000', '::1', null, null, null, '/x', '/x', 'http://[::1]:3000/x']],
  ['http://ex%41mple.com/', ['http:', true, null, 'ex', null, 'ex', null, null, null, '%41mple.com/', '%41mple.com/', 'http://ex/%41mple.com/']],
  ['http://a b.com/c d', ['http:', true, null, 'a', null, 'a', null, null, null, '%20b.com/c%20d', '%20b.com/c%20d', 'http://a/%20b.com/c%20d']],
  ['file:///etc/passwd', ['file:', true, null, '', null, '', null, null, null, '/etc/passwd', '/etc/passwd', 'file:///etc/passwd']],
  ['http://münchen.de/', ['http:', true, null, 'xn--mnchen-3ya.de', null, 'xn--mnchen-3ya.de', null, null, null, '/', '/', 'http://xn--mnchen-3ya.de/']],
  ['tcp://localhost:4321', ['tcp:', true, null, 'localhost:4321', '4321', 'localhost', null, null, null, null, null, 'tcp://localhost:4321']],
];
for (const [input, values] of parseCases) {
  assert.deepStrictEqual(Object.values(url.parse(input)), values, input);
}
assert.deepStrictEqual(
  Object.values(url.parse('//some.host/path', false, true)),
  [null, true, null, 'some.host', null, 'some.host', null, null, null, '/path', '/path', '//some.host/path'],
);

const resolveCases = [
  ['http://a/b/c/d;p?q', 'g:h', 'g:h'],
  ['http://a/b/c/d;p?q', 'g', 'http://a/b/c/g'],
  ['http://a/b/c/d;p?q', './g', 'http://a/b/c/g'],
  ['http://a/b/c/d;p?q', 'g/', 'http://a/b/c/g/'],
  ['http://a/b/c/d;p?q', '/g', 'http://a/g'],
  ['http://a/b/c/d;p?q', '//g', 'http://g/'],
  ['http://a/b/c/d;p?q', '?y', 'http://a/b/c/d;p?y'],
  ['http://a/b/c/d;p?q', 'g?y', 'http://a/b/c/g?y'],
  ['http://a/b/c/d;p?q', '#s', 'http://a/b/c/d;p?q#s'],
  ['http://a/b/c/d;p?q', 'g;x?y#s', 'http://a/b/c/g;x?y#s'],
  ['http://a/b/c/d;p?q', '', 'http://a/b/c/d;p?q'],
  ['http://a/b/c/d;p?q', '.', 'http://a/b/c/'],
  ['http://a/b/c/d;p?q', '..', 'http://a/b/'],
  ['http://a/b/c/d;p?q', '../g', 'http://a/b/g'],
  ['http://a/b/c/d;p?q', '../../../g', 'http://a/g'],
  ['http://a/b/c/d;p?q', '/./g', 'http://a/g'],
  ['http://a/b/c/d;p?q', 'g/../h', 'http://a/b/c/h'],
  ['http://a/b/c/d;p?q', 'http:g', 'http://a/b/c/g'],
  ['/foo/bar/baz', '../../../../../../../../quux/baz', '/quux/baz'],
  ['foo/bar', '../../../baz', '../../baz'],
  ['mailto:local1@domain1', 'local2@domain2', 'mailto:local2@domain2'],
  ['mailto:local1@domain1', '?q', 'mailto:local1@domain1?q'],
  ['http://example.com/b//c//d;p?q#blarg', 'https:/p/a/t/h', 'https://p/a/t/h'],
  ['http://example.com/b//c//d;p?q#blarg', 'mailto:user@example.org', 'mailto:user@example.org'],
  ['http://u:p@example.com/x', '//other.example/y', 'http://other.example/y'],
  ['file:///a/b', '../c', 'file:///c'],
  ['javascript:void(0)', 'x', 'javascript:x'],
  ['http://[::1]:8080/a/', '../b?c#d', 'http://[::1]:8080/b?c#d'],
];
for (const [from, to, expected] of resolveCases) {
  assert.strictEqual(url.resolve(from, to), expected, `${from} + ${to}`);
}

const resolved = url.resolveObject('http://a/b/c', '../d?e');
assert.ok(resolved instanceof url.Url);
assert.deepStrictEqual(Object.values(resolved), ['http:', true, null, 'a', null, 'a', null, '?e', 'e', '/d', '/d?e', 'http://a/d?e']);
assert.strictEqual(url.resolveObject('', 'x'), 'x');

assert.strictEqual(url.format({ protocol: 'http', hostname: '::1', port: 80, pathname: 'a?b#c', query: { q: ['1', '2'] } }), 'http://[::1]:80/a%3Fb%23c?q=1&q=2');
assert.strictEqual(url.format({ protocol: 'file', pathname: '/x' }), 'file:///x');
assert.strictEqual(url.format({ protocol: 'mailto', auth: 'a b', host: 'h' }), 'mailto:a%20b@h');
assert.strictEqual(url.format({ protocol: 5, pathname: 7 }), '5:7');
assert.strictEqual(url.format({ search: 'a#b' }), '?a%23b');
assert.strictEqual(url.format(new URL('http://u:p@xn--tda.com/a?b#c'), { unicode: true, auth: false, search: false, fragment: false }), 'http://ü.com/a');
assert.strictEqual(url.format(' HTTP://A.com '), 'http://a.com/');

const options = url.urlToHttpOptions(new URL('https://u%20s:p@[::1]:8443/x?y#z'));
assert.strictEqual(Object.getPrototypeOf(options), null);
assert.deepStrictEqual({ ...options }, {
  protocol: 'https:', hostname: '::1', hash: '#z', search: '?y', pathname: '/x',
  path: '/x?y', href: 'https://u%20s:p@[::1]:8443/x?y#z', port: 8443, auth: 'u s:p',
});
assert.deepStrictEqual(Object.keys(url.urlToHttpOptions(new URL('http://a/'))), ['protocol', 'hostname', 'hash', 'search', 'pathname', 'path', 'href']);

function assertThrowsCode(fn, name, code, message) {
  assert.throws(fn, error => error.name === name && error.code === code && error.message === message);
}

assertThrowsCode(() => url.parse(5), 'TypeError', 'ERR_INVALID_ARG_TYPE', 'The "url" argument must be of type string. Received type number (5)');
assertThrowsCode(() => url.format(5), 'TypeError', 'ERR_INVALID_ARG_TYPE', 'The "urlObject" argument must be one of type object or string. Received type number (5)');
assertThrowsCode(() => url.urlToHttpOptions(null), 'TypeError', 'ERR_INVALID_ARG_TYPE', 'The "url" argument must be of type object. Received null');
assertThrowsCode(() => url.parse('http://a:b/'), 'TypeError', 'ERR_INVALID_ARG_VALUE', "The argument 'url' http://a:b/. Received 'Invalid port in url'");
assertThrowsCode(() => url.parse('http://a‌b/'), 'TypeError', 'ERR_INVALID_URL', 'Invalid URL');
assert.throws(() => url.parse('http://%zz@a/'), URIError);

const lone = new URL('http://a/\ud800');
assert.strictEqual(lone.pathname, '/%EF%BF%BD');

console.log('legacy url node compat ok');
