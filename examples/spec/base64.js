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

// Buffer base64 tests
console.log('\nBuffer Base64 Tests\n');

const buf1 = Buffer.from('Hello, World!');
test('Buffer.toString base64', buf1.toString('base64'), 'SGVsbG8sIFdvcmxkIQ==');

const buf2 = Buffer.from('ABC');
test('Buffer.toString base64 ABC', buf2.toString('base64'), 'QUJD');

const buf3 = Buffer.from('');
test('Buffer.toString base64 empty', buf3.toString('base64'), '');

const buf4 = Buffer.from([0x00, 0x01, 0x02]);
test('Buffer.toString base64 binary', buf4.toString('base64'), 'AAEC');

const buf5 = Buffer.from('A');
test('Buffer.toString base64 single', buf5.toString('base64'), 'QQ==');

const buf6 = Buffer.from('AB');
test('Buffer.toString base64 two chars', buf6.toString('base64'), 'QUI=');

// Buffer.toBase64() method
test('Buffer.toBase64', buf1.toBase64(), 'SGVsbG8sIFdvcmxkIQ==');
test('Buffer.toBase64 ABC', buf2.toBase64(), 'QUJD');
test('Buffer.toBase64 empty', buf3.toBase64(), '');

// Buffer hex encoding
console.log('\nBuffer Hex Tests\n');

const hexBuf1 = Buffer.from('Hello');
test('Buffer.toString hex Hello', hexBuf1.toString('hex'), '48656c6c6f');

const hexBuf2 = Buffer.from([0xde, 0xad, 0xbe, 0xef]);
test('Buffer.toString hex deadbeef', hexBuf2.toString('hex'), 'deadbeef');

const hexBuf3 = Buffer.from([0x00, 0xff, 0x10]);
test('Buffer.toString hex padding', hexBuf3.toString('hex'), '00ff10');

summary();
