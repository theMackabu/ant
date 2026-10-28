'use strict';

const assert = require('node:assert');
const path = require('node:path');
const { fileURLToPath, pathToFileURL } = require('node:url');

function throwsCode(fn, ctor, code, message) {
  assert.throws(fn, err => {
    assert.ok(err instanceof ctor, `expected ${ctor.name}, got ${err && err.name}: ${err && err.message}`);
    assert.strictEqual(err.code, code);
    if (message !== undefined) assert.strictEqual(err.message, message);
    return true;
  });
}

for (const input of ['file:///%FF', 'file:///%C3', 'file:///%ED%A0%80', 'file:///a%2', 'file:///a%zz']) {
  throwsCode(() => fileURLToPath(input), URIError, undefined, 'URI malformed');
  throwsCode(() => fileURLToPath(new URL(input)), URIError, undefined, 'URI malformed');
}

for (const input of ['file:///a%2Fb', 'file:///a%2fb']) {
  throwsCode(() => fileURLToPath(input), TypeError, 'ERR_INVALID_FILE_URL_PATH', 'File URL path must not include encoded / characters');
}

if (process.platform !== 'win32') {
  throwsCode(
    () => fileURLToPath('file://host/etc'), TypeError, 'ERR_INVALID_FILE_URL_HOST',
    `File URL host must be "localhost" or empty on ${process.platform}`
  );
  assert.strictEqual(fileURLToPath('file://localhost/etc/x'), '/etc/x');
  assert.strictEqual(fileURLToPath('file:///a%20b/%E2%82%AC'), '/a b/€');
  assert.strictEqual(fileURLToPath('file:///a%5Cb%25'), '/a\\b%');
  assert.strictEqual(fileURLToPath('file:///a%00b'), '/a\u0000b');
  assert.strictEqual(fileURLToPath('file:///a?b#c'), '/a');
}

throwsCode(() => fileURLToPath('http://x/y'), TypeError, 'ERR_INVALID_URL_SCHEME', 'The URL must be of scheme file');
throwsCode(() => fileURLToPath(new URL('http://x/y')), TypeError, 'ERR_INVALID_URL_SCHEME');
throwsCode(() => fileURLToPath('not a url'), TypeError, 'ERR_INVALID_URL');
throwsCode(() => fileURLToPath(42), TypeError, 'ERR_INVALID_ARG_TYPE',
  'The "path" argument must be of type string or an instance of URL. Received type number (42)');
throwsCode(() => fileURLToPath(), TypeError, 'ERR_INVALID_ARG_TYPE');
throwsCode(() => fileURLToPath(new String('file:///x')), TypeError, 'ERR_INVALID_ARG_TYPE');
assert.strictEqual(fileURLToPath({ href: 'x', protocol: 'file:', hostname: '', pathname: '/a%20b' }), '/a b');
throwsCode(() => fileURLToPath({ href: 'x', protocol: 'file:', hostname: '', pathname: '/a', auth: '' }), TypeError, 'ERR_INVALID_ARG_TYPE');

throwsCode(() => pathToFileURL(42), TypeError, 'ERR_INVALID_ARG_TYPE',
  'The "path" argument must be of type string. Received type number (42)');
throwsCode(() => pathToFileURL(new URL('file:///x')), TypeError, 'ERR_INVALID_ARG_TYPE');

if (process.platform !== 'win32') {
  const cases = [
    ['/tmp/a%b', 'file:///tmp/a%25b'],
    ['/tmp/a#b', 'file:///tmp/a%23b'],
    ['/tmp/a?b', 'file:///tmp/a%3Fb'],
    ['/tmp/a b', 'file:///tmp/a%20b'],
    ['/tmp/a%20b', 'file:///tmp/a%2520b'],
    ['/tmp/a\\b', 'file:///tmp/a%5Cb'],
    ['/tmp/a\nb\rc\td', 'file:///tmp/a%0Ab%0Dc%09d'],
    ['/tmp/[x]{y}|^`~', 'file:///tmp/%5Bx%5D%7By%7D%7C%5E%60%7E'],
    ['/tmp/é€😀', 'file:///tmp/%C3%A9%E2%82%AC%F0%9F%98%80'],
    ['/tmp/a\u0000b', 'file:///tmp/a%00b'],
    ['/tmp/x\uD800', 'file:///tmp/x%EF%BF%BD'],
    ['/a/b/..', 'file:///a'],
    ['/a/./b//c/', 'file:///a/b/c/'],
    ['//x/y', 'file:///x/y'],
    ['/..', 'file:///'],
  ];

  for (const [input, href] of cases) {
    const url = pathToFileURL(input);
    assert.ok(url instanceof URL);
    assert.strictEqual(url.href, href, input);
  }

  for (const input of ['/tmp/a%b', '/tmp/a#b', '/tmp/a?b', '/tmp/a b', '/tmp/a%20b', '/tmp/a\\b', '/tmp/a\nb', '/tmp/é€😀', '/tmp/a\u0000b', '/tmp/dir/']) {
    assert.strictEqual(fileURLToPath(pathToFileURL(input)), input);
    assert.strictEqual(fileURLToPath(pathToFileURL(input).href), input);
  }

  assert.strictEqual(pathToFileURL('rel/x').href, pathToFileURL(path.resolve('rel/x')).href);
  assert.strictEqual(fileURLToPath(pathToFileURL('rel/x')), path.resolve('rel/x'));
  assert.strictEqual(fileURLToPath(pathToFileURL('')), process.cwd());
}

console.log('url file path node compat tests passed');
