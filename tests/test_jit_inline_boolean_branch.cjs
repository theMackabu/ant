const assert = require('node:assert');

function compare(a, b) { if (a < b) return 11; return 19; }
function invert(a, b) { return !(a === b); }
function nullish(a) { if (a == null) return 11; return 19; }
function joined(a, b, c) { if (a || b === c) return 11; return 19; }
function joinedAnd(a, b, c) { if (a && b === c) return 11; return 19; }
function joinedNot(a, b, c) { return !(a || b === c); }

for (let i = 0; i < 500; i++) {
  compare(i, i + 1); invert(i, i); nullish(null);
  joined(false, i, i); joinedAnd(true, i, i); joinedNot(false, i, i);
}
function caller(a, b, c) {
  return [compare(b, c), invert(b, c), nullish(a), joined(a, b, c),
    joinedAnd(a, b, c), joinedNot(a, b, c)];
}
const values = [false, true, null, undefined, 0, -0, NaN, 1, '', 'x', 0n, 1n, {}, [], Symbol('x')];
for (let i = 0; i < 100; i++) {
  for (const a of values) {
    for (const [b, c] of [[1, 1], [1, 2], [3, 2]]) {
      assert.deepStrictEqual(caller(a, b, c), [
        b < c ? 11 : 19, !(b === c), a == null ? 11 : 19,
        a || b === c ? 11 : 19, a && b === c ? 11 : 19, !(a || b === c),
      ]);
    }
  }
}
console.log('PASS inline Boolean branches and mixed joins');
