// Test string replace with regex

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// Basic regex replacement
const str1 = 'hello world';
const re1 = new RegExp('world');
const result1 = str1.replace(re1, 'universe');
console.log('Basic replace:', result1);

// Test with global flag
const str2 = 'foo bar foo baz';
const re2 = new RegExp('foo', 'g');
const result2 = str2.replace(re2, 'qux');
console.log('Global replace:', result2);

// Test without global flag (should replace only first)
const str3 = 'foo bar foo baz';
const re3 = new RegExp('foo');
const result3 = str3.replace(re3, 'qux');
console.log('Non-global replace:', result3);

// Test with regex pattern (digits)
const str4 = 'I have 2 apples and 3 oranges';
const re4 = new RegExp('[0-9]+', 'g');
const result4 = str4.replace(re4, 'X');
console.log('Replace digits:', result4);

// Test with dot pattern
const str5 = 'abc123def';
const re5 = new RegExp('...', 'g');
const result5 = str5.replace(re5, 'X');
console.log('Replace with dot:', result5);

// Test string replace (without regex) - should still work
const str6 = 'hello world';
const result6 = str6.replace('world', 'there');
console.log('String replace:', result6);

const longNeedle = 'a'.repeat(128) + 'b';
const longMiss = 'a'.repeat(4096);
const longHit = `prefix:${longNeedle}:suffix`;

assert(longMiss.replace(longNeedle, 'x') === longMiss, 'long string replace miss mismatch');
assert(longMiss.replaceAll(longNeedle, 'x') === longMiss, 'long string replaceAll miss mismatch');
assert(longHit.replace(longNeedle, 'x') === 'prefix:x:suffix', 'long string replace hit mismatch');
assert(longHit.replaceAll(longNeedle, 'x') === 'prefix:x:suffix', 'long string replaceAll hit mismatch');

const longLiteralRe = new RegExp(longNeedle);
assert(!longLiteralRe.test(longMiss), 'long regexp literal miss mismatch');
assert(longLiteralRe.test(longHit), 'long regexp literal hit mismatch');

for (let i = 0; i < 2000; i++) {
  const match = 'prefix route_123 suffix'.match('route_[0-9]+');
  assert(match !== null && match[0] === 'route_123', 'cached string match mismatch');
  assert('prefix route_123 suffix'.search('route_[0-9]+') === 7, 'cached string search mismatch');
}

const splitMiss = 'a'.repeat(4096);
const splitResult = splitMiss.split(/z/);
assert(splitResult.length === 1 && splitResult[0] === splitMiss, 'sticky split miss mismatch');

console.log('All replace tests passed!');
