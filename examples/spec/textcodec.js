import { test, summary } from './helpers.js';

console.log('TextEncoder/TextDecoder Tests\n');

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const encoded = encoder.encode('hello');
test('TextEncoder type', encoded instanceof Uint8Array, true);
test('TextEncoder length', encoded.length, 5);
test('TextEncoder byte 0', encoded[0], 104);
test('TextEncoder byte 1', encoded[1], 101);

const decoded = decoder.decode(encoded);
test('TextDecoder', decoded, 'hello');

const utf8 = encoder.encode('日本語');
test('UTF-8 encode length', utf8.length, 9);
const utf8Decoded = decoder.decode(utf8);
test('UTF-8 decode', utf8Decoded, '日本語');

const emoji = encoder.encode('😀');
test('emoji encode length', emoji.length, 4);
test('emoji decode', decoder.decode(emoji), '😀');

const empty = encoder.encode('');
test('empty encode length', empty.length, 0);
test('empty decode', decoder.decode(empty), '');

const roundtrip = 'Hello, 世界! 🎉';
const rt = decoder.decode(encoder.encode(roundtrip));
test('roundtrip', rt, roundtrip);

summary();
