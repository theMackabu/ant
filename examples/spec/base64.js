import { test, summary } from './helpers.js';

console.log('Base64 Tests\n');

test('btoa Hello World', btoa('Hello, World!'), 'SGVsbG8sIFdvcmxkIQ==');
test('atob Hello World', atob('SGVsbG8sIFdvcmxkIQ=='), 'Hello, World!');

const original = 'The quick brown fox jumps over the lazy dog';
test('roundtrip', atob(btoa(original)), original);

test('btoa empty', btoa(''), '');
test('atob empty', atob(''), '');

test('btoa single char', btoa('A'), 'QQ==');
test('btoa two chars', btoa('AB'), 'QUI=');
test('btoa three chars', btoa('ABC'), 'QUJD');

test('roundtrip numbers', atob(btoa('12345')), '12345');

const special = '!@#$%^&*()';
test('roundtrip special', atob(btoa(special)), special);

const jsonData = '{"name":"test","value":123}';
test('roundtrip json', atob(btoa(jsonData)), jsonData);

test('btoa binary', btoa('\x00\x01\x02'), 'AAEC');
test('atob binary', atob('AAEC'), '\x00\x01\x02');

summary();
