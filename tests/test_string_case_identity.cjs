const assert = require('node:assert');

const cases = [
  ['', '', ''],
  ['abc', 'abc', 'ABC'],
  ['ABC', 'abc', 'ABC'],
  ['content-type', 'content-type', 'CONTENT-TYPE'],
  ['Content-Length', 'content-length', 'CONTENT-LENGTH'],
  ['x-Request-ID-42', 'x-request-id-42', 'X-REQUEST-ID-42'],
  ['0123456789 !@#$%^&*()_+[]{}', '0123456789 !@#$%^&*()_+[]{}', '0123456789 !@#$%^&*()_+[]{}'],
  ['@[`{', '@[`{', '@[`{'],
  ['zZaA', 'zzaa', 'ZZAA'],
  ['café', 'café', 'CAFÉ'],
  ['CAFÉ', 'café', 'CAFÉ'],
  ['straße', 'straße', 'STRASSE'],
  ['İ', 'i̇', 'İ'],
  ['ΣΑ', 'σα', 'ΣΑ'],
  ['emoji \u{1F600} ok', 'emoji \u{1F600} ok', 'EMOJI \u{1F600} OK'],
];

function lower(s) { return s.toLowerCase(); }
function upper(s) { return s.toUpperCase(); }

for (let round = 0; round < 3000; round++) {
  for (const [input, lo, up] of cases) {
    assert.strictEqual(lower(input), lo, input);
    assert.strictEqual(upper(input), up, input);
  }
}

for (const [input, lo, up] of cases) {
  assert.strictEqual(input.toLocaleLowerCase(), lo);
  assert.strictEqual(input.toLocaleUpperCase(), up);
  assert.strictEqual(String.prototype.toLowerCase.call(new String(input)), lo);
  assert.strictEqual(String.prototype.toUpperCase.call(new String(input)), up);
  assert.strictEqual(lower(input + ''), lo);
}

let rope = '';
for (let i = 0; i < 64; i++) rope += 'seg-' + i + '-';
assert.strictEqual(rope.toLowerCase(), rope);
assert.strictEqual(rope.toUpperCase().toLowerCase(), rope);
assert.strictEqual((rope + 'X').toLowerCase(), rope + 'x');

const lowered = 'already lower';
const result = lowered.toLowerCase();
assert.strictEqual(result, lowered);
assert.strictEqual(result.length, lowered.length);
const map = new Map([[lowered, 1]]);
assert.strictEqual(map.get(result), 1);
const obj = { [lowered]: 2 };
assert.strictEqual(obj[result], 2);

assert.strictEqual(String.prototype.toLowerCase.call(12), '12');
assert.strictEqual(String.prototype.toUpperCase.call(true), 'TRUE');
assert.throws(() => String.prototype.toLowerCase.call(null), TypeError);
assert.throws(() => String.prototype.toUpperCase.call(undefined), TypeError);

console.log('string case identity ok');
