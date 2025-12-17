console.log('=== URI Encoding/Decoding Tests ===\n');

let passed = 0;
let failed = 0;

function test(name, actual, expected) {
  if (actual === expected) {
    console.log(`✓ ${name}`);
    passed++;
  } else {
    console.log(`✗ ${name}`);
    console.log(`  Expected: ${expected}`);
    console.log(`  Actual:   ${actual}`);
    failed++;
  }
}

function testThrows(name, fn) {
  try {
    fn();
    console.log(`✗ ${name} (expected to throw)`);
    failed++;
  } catch (e) {
    console.log(`✓ ${name} (threw)`);
    passed++;
  }
}

// encodeURIComponent tests
console.log('\n--- encodeURIComponent ---');
test('encodes space', encodeURIComponent(' '), '%20');
test('encodes special chars', encodeURIComponent('hello world!'), 'hello%20world!');
test('preserves unreserved', encodeURIComponent('abc123'), 'abc123');
test('preserves unreserved marks', encodeURIComponent("-_.!~*'()"), "-_.!~*'()");
test('encodes reserved chars', encodeURIComponent(';/?:@&=+$,#'), '%3B%2F%3F%3A%40%26%3D%2B%24%2C%23');
test('encodes Cyrillic', encodeURIComponent('шеллы'), '%D1%88%D0%B5%D0%BB%D0%BB%D1%8B');
test('encodes Chinese', encodeURIComponent('中文'), '%E4%B8%AD%E6%96%87');
test('encodes emoji', encodeURIComponent('😀'), '%F0%9F%98%80');
test('empty string', encodeURIComponent(''), '');

// encodeURI tests
console.log('\n--- encodeURI ---');
test('preserves URI structure', encodeURI('https://example.com/path?q=hello world'), 'https://example.com/path?q=hello%20world');
test('preserves reserved chars', encodeURI(';/?:@&=+$,#'), ';/?:@&=+$,#');
test('encodes space', encodeURI('hello world'), 'hello%20world');
test('encodes Cyrillic in URL', encodeURI('https://mozilla.org/?x=шеллы'), 'https://mozilla.org/?x=%D1%88%D0%B5%D0%BB%D0%BB%D1%8B');
test('empty string', encodeURI(''), '');

// decodeURIComponent tests
console.log('\n--- decodeURIComponent ---');
test('decodes space', decodeURIComponent('%20'), ' ');
test('decodes special chars', decodeURIComponent('hello%20world%21'), 'hello world!');
test('decodes Cyrillic', decodeURIComponent('%D1%88%D0%B5%D0%BB%D0%BB%D1%8B'), 'шеллы');
test('decodes Chinese', decodeURIComponent('%E4%B8%AD%E6%96%87'), '中文');
test('decodes emoji', decodeURIComponent('%F0%9F%98%80'), '😀');
test('decodes reserved chars', decodeURIComponent('%3B%2F%3F%3A%40%26%3D%2B%24%2C%23'), ';/?:@&=+$,#');
test('passes through plain text', decodeURIComponent('hello'), 'hello');
test('empty string', decodeURIComponent(''), '');
test('mixed encoded/plain', decodeURIComponent('hello%20world'), 'hello world');

// decodeURI tests
console.log('\n--- decodeURI ---');
test(
  'decodes URL with Cyrillic',
  decodeURI('https://developer.mozilla.org/ru/docs/JavaScript_%D1%88%D0%B5%D0%BB%D0%BB%D1%8B'),
  'https://developer.mozilla.org/ru/docs/JavaScript_шеллы'
);
test('preserves encoded reserved', decodeURI('https://example.com/docs/JavaScript%3A%20test'), 'https://example.com/docs/JavaScript%3A test');
test('decodes non-reserved', decodeURI('hello%20world'), 'hello world');
test('empty string', decodeURI(''), '');

// decodeURI vs decodeURIComponent comparison
console.log('\n--- decodeURI vs decodeURIComponent ---');
const encoded = 'https://developer.mozilla.org/docs/JavaScript%3A%20a_scripting_language';
test('decodeURI preserves %3A', decodeURI(encoded), 'https://developer.mozilla.org/docs/JavaScript%3A a_scripting_language');
test('decodeURIComponent decodes %3A', decodeURIComponent(encoded), 'https://developer.mozilla.org/docs/JavaScript: a_scripting_language');

// Error cases
console.log('\n--- Error cases ---');
testThrows('decodeURIComponent invalid sequence', () => decodeURIComponent('%E0%A4%A'));
testThrows('decodeURI invalid sequence', () => decodeURI('%E0%A4%A'));
testThrows('decodeURIComponent incomplete %', () => decodeURIComponent('%'));
testThrows('decodeURIComponent incomplete %X', () => decodeURIComponent('%2'));
testThrows('decodeURIComponent invalid hex', () => decodeURIComponent('%GG'));

// Round-trip tests
console.log('\n--- Round-trip tests ---');
const testStrings = ['hello world', 'foo=bar&baz=qux', 'шеллы', '中文测试', 'emoji: 😀🎉', 'special: !@#$%^&*()', 'path/to/file.txt'];

for (const str of testStrings) {
  const encoded = encodeURIComponent(str);
  const decoded = decodeURIComponent(encoded);
  test(`round-trip: "${str}"`, decoded, str);
}

// Summary
console.log('\n=== Summary ===');
console.log(`Passed: ${passed}`);
console.log(`Failed: ${failed}`);

if (failed > 0) process.exit(1);
