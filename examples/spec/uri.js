import { test, testThrows, summary } from './helpers.js';

console.log('URI Encoding/Decoding Tests\n');

test('encodeURIComponent space', encodeURIComponent(' '), '%20');
test('encodeURIComponent special', encodeURIComponent('hello world!'), 'hello%20world!');
test('encodeURIComponent preserves unreserved', encodeURIComponent('abc123'), 'abc123');
test('encodeURIComponent preserves marks', encodeURIComponent("-_.!~*'()"), "-_.!~*'()");
test('encodeURIComponent reserved', encodeURIComponent(';/?:@&=+$,#'), '%3B%2F%3F%3A%40%26%3D%2B%24%2C%23');
test('encodeURIComponent Cyrillic', encodeURIComponent('шеллы'), '%D1%88%D0%B5%D0%BB%D0%BB%D1%8B');
test('encodeURIComponent Chinese', encodeURIComponent('中文'), '%E4%B8%AD%E6%96%87');
test('encodeURIComponent emoji', encodeURIComponent('😀'), '%F0%9F%98%80');
test('encodeURIComponent empty', encodeURIComponent(''), '');

test('encodeURI preserves structure', encodeURI('https://example.com/path?q=hello world'), 'https://example.com/path?q=hello%20world');
test('encodeURI preserves reserved', encodeURI(';/?:@&=+$,#'), ';/?:@&=+$,#');
test('encodeURI space', encodeURI('hello world'), 'hello%20world');
test('encodeURI Cyrillic', encodeURI('https://mozilla.org/?x=шеллы'), 'https://mozilla.org/?x=%D1%88%D0%B5%D0%BB%D0%BB%D1%8B');
test('encodeURI empty', encodeURI(''), '');

test('decodeURIComponent space', decodeURIComponent('%20'), ' ');
test('decodeURIComponent special', decodeURIComponent('hello%20world%21'), 'hello world!');
test('decodeURIComponent Cyrillic', decodeURIComponent('%D1%88%D0%B5%D0%BB%D0%BB%D1%8B'), 'шеллы');
test('decodeURIComponent Chinese', decodeURIComponent('%E4%B8%AD%E6%96%87'), '中文');
test('decodeURIComponent emoji', decodeURIComponent('%F0%9F%98%80'), '😀');
test('decodeURIComponent reserved', decodeURIComponent('%3B%2F%3F%3A%40%26%3D%2B%24%2C%23'), ';/?:@&=+$,#');
test('decodeURIComponent plain', decodeURIComponent('hello'), 'hello');
test('decodeURIComponent empty', decodeURIComponent(''), '');

test('decodeURI Cyrillic', decodeURI('https://developer.mozilla.org/ru/docs/JavaScript_%D1%88%D0%B5%D0%BB%D0%BB%D1%8B'), 'https://developer.mozilla.org/ru/docs/JavaScript_шеллы');
test('decodeURI preserves encoded reserved', decodeURI('https://example.com/docs/JavaScript%3A%20test'), 'https://example.com/docs/JavaScript%3A test');
test('decodeURI non-reserved', decodeURI('hello%20world'), 'hello world');
test('decodeURI empty', decodeURI(''), '');

const encoded = 'https://developer.mozilla.org/docs/JavaScript%3A%20a_scripting_language';
test('decodeURI preserves %3A', decodeURI(encoded), 'https://developer.mozilla.org/docs/JavaScript%3A a_scripting_language');
test('decodeURIComponent decodes %3A', decodeURIComponent(encoded), 'https://developer.mozilla.org/docs/JavaScript: a_scripting_language');

testThrows('decodeURIComponent invalid sequence', () => decodeURIComponent('%E0%A4%A'));
testThrows('decodeURI invalid sequence', () => decodeURI('%E0%A4%A'));
testThrows('decodeURIComponent incomplete %', () => decodeURIComponent('%'));
testThrows('decodeURIComponent incomplete %X', () => decodeURIComponent('%2'));
testThrows('decodeURIComponent invalid hex', () => decodeURIComponent('%GG'));

const testStrings = ['hello world', 'foo=bar&baz=qux', 'шеллы', '中文测试', 'emoji: 😀🎉', 'special: !@#$%^&*()', 'path/to/file.txt'];
for (const str of testStrings) {
  const enc = encodeURIComponent(str);
  const dec = decodeURIComponent(enc);
  test(`roundtrip: "${str}"`, dec, str);
}

summary();
