import { test, summary } from './helpers.js';

console.log('URL Tests\n');

const url = new URL('https://user:pass@example.com:8080/path/to?query=value#hash');

test('URL href', url.href, 'https://user:pass@example.com:8080/path/to?query=value#hash');
test('URL protocol', url.protocol, 'https:');
test('URL username', url.username, 'user');
test('URL password', url.password, 'pass');
test('URL host', url.host, 'example.com:8080');
test('URL hostname', url.hostname, 'example.com');
test('URL port', url.port, '8080');
test('URL pathname', url.pathname, '/path/to');
test('URL search', url.search, '?query=value');
test('URL hash', url.hash, '#hash');
test('URL origin', url.origin, 'https://example.com:8080');

const url2 = new URL('/other', 'https://example.com');
test('URL with base', url2.href, 'https://example.com/other');

const url3 = new URL('https://example.com');
url3.pathname = '/new/path';
test('URL set pathname', url3.pathname, '/new/path');

const params = new URLSearchParams('a=1&b=2&a=3');
test('URLSearchParams get', params.get('a'), '1');
test('URLSearchParams getAll', params.getAll('a').length, 2);
test('URLSearchParams has', params.has('b'), true);
test('URLSearchParams has missing', params.has('c'), false);

params.set('c', '4');
test('URLSearchParams set', params.get('c'), '4');

params.append('d', '5');
test('URLSearchParams append', params.get('d'), '5');

params.delete('b');
test('URLSearchParams delete', params.has('b'), false);

test('URLSearchParams toString', params.toString().includes('a=1'), true);

const url4 = new URL('https://example.com?x=1');
url4.searchParams.set('y', '2');
test('URL searchParams', url4.search, '?x=1&y=2');

summary();
