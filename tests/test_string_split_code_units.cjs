const assert = require('node:assert');
for (const text of ['', 'ascii', 'ééa', '中aé', 'a\0b', '😀', 'x😀é𝄞', '\ud800x\udfff', 'e\u0301']) {
  const expected = [];
  for (let i = 0; i < text.length; i++) expected.push(text.charAt(i));
  for (const limit of [0, 1, 2, 3, 20, 0xffffffff]) {
    assert.deepStrictEqual(text.split('', limit), expected.slice(0, limit));
    assert.deepStrictEqual(text.split({ toString() { return ''; } }, limit), expected.slice(0, limit));
  }
  assert.deepStrictEqual(text.split(''), expected);
}
const long = 'é😀中'.repeat(10000);
const parts = long.split('');
assert.strictEqual(parts.length, long.length);
for (let i = 0; i < parts.length; i++) assert.strictEqual(parts[i].charCodeAt(0), long.charCodeAt(i));
console.log('PASS empty-separator split returns UTF-16 code units');
