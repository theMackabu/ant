// The JIT's inline short string concat copies each side with overlapping
// 8/4/2/1-byte moves chosen by length. Every length pair whose total stays
// under the short-string threshold must produce exactly the joined bytes,
// including the boundary lengths where the move widths change.
const assert = require('node:assert');

const alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
const pieces = [];
for (let n = 1; n < 32; n++) {
  let s = '';
  for (let i = 0; i < n; i++) s += alphabet[(n * 7 + i) % alphabet.length];
  pieces.push(s);
}

function join(a, b) { return a + b; }

function expected(a, b) {
  const out = [];
  for (let i = 0; i < a.length; i++) out.push(a.charCodeAt(i));
  for (let i = 0; i < b.length; i++) out.push(b.charCodeAt(i));
  return out;
}

// enough rounds for join to be compiled and take the inline path
for (let round = 0; round < 40; round++) {
  for (const a of pieces) {
    for (const b of pieces) {
      if (a.length + b.length >= 32) continue;
      const got = join(a, b);
      assert.strictEqual(got.length, a.length + b.length);
      if (round === 0 || round === 39) {
        const want = expected(a, b);
        for (let i = 0; i < want.length; i++)
          assert.strictEqual(got.charCodeAt(i), want[i], `${a.length}+${b.length} at ${i}`);
      }
      assert.strictEqual(got, a.concat(b), `${a.length}+${b.length}`);
    }
  }
}

console.log('PASS');
