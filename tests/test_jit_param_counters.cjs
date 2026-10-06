// Parameters that only step by ±1 (`i++`, `--n`) are read as integers where
// an integer is wanted (element keys, bitwise ops), after an entry check that
// they arrived as integers. Results must match the double semantics; a
// non-integer or huge argument must recompile without the speculation.
function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

const src = Array.from({ length: 64 }, (_, k) => k * 3 + 1);

// am3 from the V8 crypto benchmark
function am3(i, x, w, j, c, n) {
  const xl = x & 0x3fff, xh = x >> 14;
  while (--n >= 0) {
    let l = src[i] & 0x3fff;
    const h = src[i++] >> 14;
    const m = xh * l + h * xl;
    l = xl * l + ((m & 0x3fff) << 14) + w[j] + c;
    c = (l >> 28) + (m >> 14) + xh * h;
    w[j++] = l & 0xfffffff;
  }
  return c;
}
function walk(i, n) { const r = []; while (n-- > 0) r.push(src[i++], i & 7, i | 0); return r; }
// reassigned by other than ±1, and written through a mapped arguments object
function drift(i, n) { const r = []; while (n-- > 0) { r.push(src[i], i & 7); i = i + 0.5; } return r; }
function alias(i, n) { const r = []; while (n-- > 0) { r.push(src[i++], i & 3); if (n === 2) arguments[0] = 1.5; } return r; }
function back(i, n) { const r = []; for (; n > 0; --n) r.push(src[i--], i >> 1); return r; }

const am3Ref = (i, x, w, j, c, n) => {
  const xl = x & 0x3fff, xh = x >> 14;
  while (--n >= 0) {
    let l = src[i] & 0x3fff; const h = src[i++] >> 14; const m = xh * l + h * xl;
    l = xl * l + ((m & 0x3fff) << 14) + w[j] + c; c = (l >> 28) + (m >> 14) + xh * h; w[j++] = l & 0xfffffff;
  }
  return c;
};
const walkRef = (i, n) => { const r = []; while (n-- > 0) { const v = src[i]; i += 1; r.push(v, i & 7, i | 0); } return r; };
const driftRef = (i, n) => { const r = []; while (n-- > 0) { r.push(src[i], i & 7); i += 0.5; } return r; };
const aliasRef = (i, n) => { const r = []; while (n-- > 0) { const v = src[i]; i += 1; r.push(v, i & 3); if (n === 2) i = 1.5; } return r; };
const backRef = (i, n) => { const r = []; for (; n > 0; --n) { const v = src[i]; i -= 1; r.push(v, i >> 1); } return r; };

function sameList(a, b, what) {
  same(a.length, b.length, `${what} length`);
  for (let k = 0; k < b.length; k++) same(a[k], b[k], `${what}[${k}]`);
}

for (let r = 0; r < 4000; r++) {
  am3(r & 15, 12345 + r, new Array(64).fill(r & 3), 0, r & 7, 40);
  walk(r & 31, 20);
  back(40, 20);
  drift(r & 31, 6);
  alias(r & 31, 6);
}

for (const [i, j, n] of [[0, 0, 40], [3, 5, 20], [10, 0, 0]]) {
  const w1 = new Array(64).fill(7), w2 = new Array(64).fill(7);
  same(am3(i, 99999, w1, j, 3, n), am3Ref(i, 99999, w2, j, 3, n), `am3(${i}, ${j}, ${n})`);
  sameList(w1, w2, `am3 output (${i}, ${j}, ${n})`);
}
sameList(walk(60, 8), walkRef(60, 8), 'walk past the end');
sameList(back(2, 6), backRef(2, 6), 'back below zero');
sameList(drift(3, 6), driftRef(3, 6), 'reassigned by +0.5');
sameList(alias(3, 6), aliasRef(3, 6), 'written through arguments');
// non-integer and very large starts: the integer copy would be wrong; -0
// must still read element 0
sameList(walk(1.5, 4), walkRef(1.5, 4), 'fractional start');
sameList(walk(-0, 3), walkRef(-0, 3), '-0 start');
sameList(walk(2 ** 53, 2), walkRef(2 ** 53, 2), '2^53 start');
sameList(walk(1e300, 2), walkRef(1e300, 2), 'huge start');
sameList(walk(-Infinity, 2), walkRef(-Infinity, 2), '-Infinity start');
sameList(walk(NaN, 2), walkRef(NaN, 2), 'NaN start');
sameList(back(1.25, 3), backRef(1.25, 3), 'fractional counting down');
// back on integers afterwards
for (let r = 0; r < 4000; r++) walk(r & 31, 20);
sameList(walk(5, 10), walkRef(5, 10), 'integers again');

console.log('PASS integer parameter counters match double semantics');
