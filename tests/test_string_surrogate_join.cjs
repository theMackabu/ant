// a high surrogate followed by a low one is the same string as the pair, however it was built
const assert = require('node:assert');

const emoji = '😀', hi = '\ud83d', lo = '\ude00';
const same = (value, label) => {
  assert.strictEqual(value, emoji, label);
  assert.strictEqual(value.length, 2, label);
  assert.strictEqual(value.codePointAt(0), 0x1f600, label);
  assert.strictEqual(encodeURIComponent(value), '%F0%9F%98%80', label);
  assert.ok(emoji in { [value]: 1 }, label);
  assert.strictEqual(new Map([[value, 1]]).get(emoji), 1, label);
};

let loop = '';
for (const part of [hi, lo]) loop += part;
const long = 'x'.repeat(40);

same(hi + lo, 'plus');
same((hi + long).slice(0, 1) + lo, 'plus after slice');
same(emoji.split('').join(''), 'split and join');
same([hi, lo].join(''), 'join');
same(hi.concat(lo), 'concat');
same(`${hi}${lo}`, 'template');
same(loop, 'append loop');
same(String.fromCharCode(0xd83d, 0xde00), 'fromCharCode');
same(String.fromCharCode(0xd83d) + String.fromCharCode(0xde00), 'fromCharCode pieces');
same(hi.padEnd(2, lo), 'padEnd');
same(lo.padStart(2, hi), 'padStart');
same('ab'.replace('a', hi).replace('b', lo), 'replace');
same([hi, lo].reduce((a, b) => a + b, ''), 'reduce');
same(JSON.parse('"\\ud83d"') + JSON.parse('"\\ude00"'), 'parsed halves');

const child = hi + long;
const parent = child + lo;
assert.strictEqual(child.length, 41);
const joined = long.slice(1) + hi;
const nested = joined + lo + '!';
assert.strictEqual(joined + '', joined, 'flatten the child first');
assert.strictEqual(nested, long.slice(1) + emoji + '!', 'a parent rope flattens after its child joined');
assert.strictEqual(parent, hi + long + lo, 'an interior surrogate stays alone');

for (const force of [s => s.charCodeAt(0), s => s === emoji, s => s.indexOf('y')]) {
  const inner = long + hi + lo;
  const outer = inner + long + '!';
  force(inner);
  assert.strictEqual(outer, long + emoji + long + '!', 'a parent rope flattens after its child joined a pair');
  assert.strictEqual((outer + long).length, 123);
}

let built = '';
for (let i = 0; i < 200; i++) built += i % 2 ? lo : hi;
assert.strictEqual(built, emoji.repeat(100), 'pairs split across appends join');
assert.strictEqual((lo + hi).repeat(3), lo + emoji + emoji + hi, 'repeat joins at the seams');

assert.strictEqual((hi + 'a' + lo).length, 3, 'separated halves stay apart');
assert.strictEqual(encodeURIComponent('한국어' + hi + lo + '한'), '%ED%95%9C%EA%B5%AD%EC%96%B4%F0%9F%98%80%ED%95%9C', 'Hangul bytes are untouched');
assert.throws(() => encodeURIComponent(lo + hi), URIError, 'a low surrogate before a high one is not a pair');
assert.strictEqual(JSON.stringify(hi), '"\\ud83d"', 'a lone surrogate stays lone');

const counts = new Map();
for (let i = 0; i < 3000; i++) {
  const key = (i % 2 ? hi : String.fromCharCode(0xd83d)) + lo;
  counts.set(key, (counts.get(key) || 0) + 1);
}
assert.deepStrictEqual([...counts], [[emoji, 3000]], 'compiled code sees one key');

console.log('string surrogate join OK');
