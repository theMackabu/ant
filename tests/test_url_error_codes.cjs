'use strict';

const assert = require('node:assert');
const url = require('node:url');

function check(fn, expected) {
  let caught = null;
  try {
    fn();
  } catch (err) {
    caught = err;
  }
  assert.ok(caught, `expected ${expected.code || expected.message} to be thrown`);
  assert.ok(caught instanceof TypeError, `expected TypeError, got ${caught && caught.name}: ${caught && caught.message}`);
  for (const key of Object.keys(expected)) assert.deepStrictEqual(caught[key], expected[key], key);
  return caught;
}

const invalid = check(() => new URL('nope'), { code: 'ERR_INVALID_URL', message: 'Invalid URL', input: 'nope' });
assert.strictEqual('base' in invalid, false);
assert.deepStrictEqual(Object.keys(invalid).sort(), ['code', 'input']);
assert.strictEqual(invalid.name, 'TypeError');

const withBase = check(() => new URL('/x', 'nope'), { code: 'ERR_INVALID_URL', message: 'Invalid URL', input: '/x', base: 'nope' });
assert.deepStrictEqual(Object.keys(withBase).sort(), ['base', 'code', 'input']);

check(() => new URL('x y:', 'also bad'), { code: 'ERR_INVALID_URL', input: 'x y:', base: 'also bad' });
check(() => new URL(''), { code: 'ERR_INVALID_URL', input: '' });
check(() => new URL(5), { code: 'ERR_INVALID_URL', input: '5' });
check(() => new URL({ toString() { return 'still bad'; } }), { code: 'ERR_INVALID_URL', input: 'still bad' });
check(() => new URL(), { code: 'ERR_MISSING_ARGS', message: 'The "url" argument must be specified' });
check(() => new URL(Symbol('x')), { message: 'Cannot convert a Symbol value to a string' });
check(() => URL('http://a'), { message: "Class constructor URL cannot be invoked without 'new'" });

assert.strictEqual(new URL('/x', 'http://a').href, 'http://a/x');
assert.strictEqual(URL.canParse('nope'), false);
assert.strictEqual(URL.canParse('/x', 'nope'), false);
assert.strictEqual(URL.canParse('/x', 'http://a'), true);
check(() => URL.canParse(), { code: 'ERR_MISSING_ARGS', message: 'The "url" argument must be specified' });
assert.strictEqual(URL.parse('nope'), null);
assert.strictEqual(URL.parse('/x', 'nope'), null);
assert.strictEqual(URL.parse('/x', 'http://a').href, 'http://a/x');
check(() => URL.parse(), { code: 'ERR_MISSING_ARGS', message: 'The "url" argument must be specified' });

const target = new URL('http://a/b');
check(() => { target.href = 'nope'; }, { code: 'ERR_INVALID_URL', message: 'Invalid URL', input: 'nope' });
assert.strictEqual(target.href, 'http://a/b');

const tuple = { code: 'ERR_INVALID_TUPLE', message: 'Each query pair must be an iterable [name, value] tuple' };
check(() => new URLSearchParams([1]), tuple);
check(() => new URLSearchParams([['a']]), tuple);
check(() => new URLSearchParams([['a', 'b', 'c']]), tuple);
check(() => new URLSearchParams(['ab']), tuple);
check(() => new URLSearchParams([null]), tuple);
check(() => new URLSearchParams({ [Symbol.iterator]: 5 }), { code: 'ERR_ARG_NOT_ITERABLE', message: 'Query pairs must be iterable' });
check(() => new URLSearchParams({ a: Symbol('v') }), { message: 'Cannot convert a Symbol value to a string' });
check(() => URLSearchParams(), { message: "Class constructor URLSearchParams cannot be invoked without 'new'" });

assert.strictEqual(new URLSearchParams([['a', '1'], new Set(['b', '2'])]).toString(), 'a=1&b=2');
assert.strictEqual(new URLSearchParams(new Map([['x', 'y']])).toString(), 'x=y');
assert.strictEqual(new URLSearchParams((function* () { yield ['g', 'h']; })()).toString(), 'g=h');
assert.strictEqual(new URLSearchParams({ k: 'v', n: 1 }).toString(), 'k=v&n=1');
assert.strictEqual(new URLSearchParams(new URLSearchParams('a=1&a=2')).toString(), 'a=1&a=2');
assert.strictEqual(new URLSearchParams(42).toString(), '42=');

const sp = new URLSearchParams();
const nameAndValue = { code: 'ERR_MISSING_ARGS', message: 'The "name" and "value" arguments must be specified' };
const nameOnly = { code: 'ERR_MISSING_ARGS', message: 'The "name" argument must be specified' };
check(() => sp.append('a'), nameAndValue);
check(() => sp.set('a'), nameAndValue);
check(() => sp.get(), nameOnly);
check(() => sp.getAll(), nameOnly);
check(() => sp.has(), nameOnly);
check(() => sp.delete(), nameOnly);
check(() => sp.forEach(5), { code: 'ERR_INVALID_ARG_TYPE' });

check(() => url.format(5), {
  code: 'ERR_INVALID_ARG_TYPE',
  message: 'The "urlObject" argument must be one of type object or string. Received type number (5)',
});
check(() => url.format(null), {
  code: 'ERR_INVALID_ARG_TYPE',
  message: 'The "urlObject" argument must be one of type object or string. Received null',
});
check(() => url.format(new URL('http://a'), 5), {
  code: 'ERR_INVALID_ARG_TYPE',
  message: 'The "options" argument must be of type object. Received type number (5)',
});
assert.strictEqual(url.format('http://a/b'), 'http://a/b');
const full = new URL('http://u:p@xn--espaol-zwa.com/b?c#d');
assert.strictEqual(url.format(full), 'http://u:p@xn--espaol-zwa.com/b?c#d');
assert.strictEqual(url.format(full, { auth: false, search: false, fragment: false }), 'http://xn--espaol-zwa.com/b');
assert.strictEqual(url.format(full, { unicode: true }), 'http://u:p@español.com/b?c#d');

check(() => url.domainToASCII(), { code: 'ERR_MISSING_ARGS', message: 'The "domain" argument must be specified' });
check(() => url.domainToUnicode(), { code: 'ERR_MISSING_ARGS', message: 'The "domain" argument must be specified' });
assert.strictEqual(url.domainToASCII('español.com'), 'xn--espaol-zwa.com');
assert.strictEqual(url.domainToUnicode('xn--espaol-zwa.com'), 'español.com');
assert.strictEqual(url.domainToASCII('xn--iñvalid.com'), '');
assert.strictEqual(url.domainToUnicode('xn--iñvalid.com'), '');

check(() => url.fileURLToPath('nope'), { code: 'ERR_INVALID_URL', input: 'nope' });
check(() => url.parse(5), {
  code: 'ERR_INVALID_ARG_TYPE',
  message: 'The "url" argument must be of type string. Received type number (5)',
});

console.log('ok');
