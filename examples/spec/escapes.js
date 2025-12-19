import { test, summary } from './helpers.js';

console.log('Escape Sequence Tests\n');

test('null char (\\0) charCodeAt', '\0'.charCodeAt(0), 0);
test('backspace (\\b) charCodeAt', '\b'.charCodeAt(0), 8);
test('tab (\\t) charCodeAt', '\t'.charCodeAt(0), 9);
test('newline (\\n) charCodeAt', '\n'.charCodeAt(0), 10);
test('vertical tab (\\v) charCodeAt', '\v'.charCodeAt(0), 11);
test('form feed (\\f) charCodeAt', '\f'.charCodeAt(0), 12);
test('carriage return (\\r) charCodeAt', '\r'.charCodeAt(0), 13);

test('escaped quote single', '\''.length, 1);
test('escaped quote double', "\"".length, 1);
test('escaped backslash', '\\'.charCodeAt(0), 92);

test('hex escape \\x41', '\x41', 'A');
test('hex escape \\x00', '\x00'.charCodeAt(0), 0);
test('hex escape \\x7f', '\x7f'.charCodeAt(0), 127);

test('unicode \\u0041 (A)', '\u0041', 'A');
test('unicode \\u0000 (null)', '\u0000'.charCodeAt(0), 0);
test('unicode \\u00e9 (é)', '\u00e9', 'é');
test('unicode \\u263A (smiley)', '\u263A', '☺');
test('unicode \\u4e2d (中)', '\u4e2d', '中');

test('string with null char length', 'a\0b'.length, 3);
test('string with null indexOf', 'a\0b'.indexOf('\0'), 1);

test('mixed escapes', 'a\tb\nc', 'a\tb\nc');
test('multiple escapes', '\n\r\t\0'.length, 4);

summary();
