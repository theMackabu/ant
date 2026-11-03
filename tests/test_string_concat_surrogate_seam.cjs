const assert = require('node:assert');
const hi = '\uD83D', lo = '\uDE00', emoji = '\u{1F600}';
const pad = n => 'x'.repeat(n);
function cat(a, b) { return a + b; }
const cases = [];
for (const n of [0, 1, 10, 30, 31, 33, 40, 50, 58, 60, 61, 70]) {
  cases.push([pad(n) + hi, lo, pad(n) + emoji]);
  cases.push([pad(n) + 'é' + hi, lo + 'ü', pad(n) + 'é' + emoji + 'ü']);
  cases.push([hi, lo + pad(n), emoji + pad(n)]);
  cases.push([String.fromCharCode(0xD83D), String.fromCharCode(0xDE00) + pad(n) + 'ä', emoji + pad(n) + 'ä']);
  cases.push([emoji.slice(0, 1) + pad(n), emoji.slice(1), emoji.slice(0, 1) + pad(n) + emoji.slice(1)]);
  cases.push([pad(n) + emoji.slice(0, 1), emoji.slice(1) + 'ö', pad(n) + emoji + 'ö']);
  cases.push(['😀' + pad(n), 'ß', emoji + pad(n) + 'ß']);
  cases.push([pad(n) + lo, hi + 'ç', pad(n) + lo + hi + 'ç']);
}
for (let r = 0; r < 3; r++) for (let i = 0; i < 2000; i++) {
  for (const [a, b, want] of cases) {
    const got = cat(a, b);
    assert.strictEqual(got, want);
    assert.strictEqual(got.length, want.length);
    assert.strictEqual(got.indexOf(emoji), want.indexOf(emoji));
    assert.deepStrictEqual([...got], [...want]);
    assert.strictEqual(JSON.stringify(got), JSON.stringify(want));
    const m = new Map([[want, 1]]);
    assert.strictEqual(m.get(got), 1);
  }
}
console.log('ok', cases.length);
