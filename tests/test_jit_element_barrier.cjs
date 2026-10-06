// Compiled element stores of non-numbers into dense arrays run the write
// barrier inline: a young value stored into an old array remembers the
// array (whole, or by card once it is large), so the next minor collection
// keeps the value. Missing it frees a live object. Holes and frozen arrays
// still go through the generic store.
const assert = require('node:assert');

const long = 'x'.repeat(200);

// young ropes are collected on their own, after 8 MiB of them
function churn(n) {
  let keep = null;
  for (let i = 0; i < n; i++) keep = { i, pad: [i, i] };
  for (let i = 0; i < n * 12; i++) keep = long + i;
  return keep;
}

// small arrays are remembered whole, large ones by card
const small = [];
const large = [];
for (let i = 0; i < 64; i++) small.push(null);
for (let i = 0; i < 4096; i++) large.push(null);
churn(300000);

function set(arr, i, v) { arr[i] = v; }

// ropes are freed by the 64 KiB block, and a stale stack slot keeps its
// whole block: give each stored rope a block of its own
function rope(tag) {
  let pad;
  for (let i = 0; i < 1500; i++) pad = long + i;
  return long + tag + long;
}

function young(tag, kind) {
  switch (kind) {
    case 0: return { tag, inner: { v: tag } };
    case 1: return [tag, { v: tag }];
    case 2: return function f() { return tag; };
    default: return rope(tag);
  }
}

function value(v) {
  if (typeof v === 'function') return v();
  if (typeof v === 'string') return +v.slice(200, -200);
  if (Array.isArray(v)) {
    assert.strictEqual(v[1].v, v[0]);
    return v[0];
  }
  assert.strictEqual(v.inner.v, v.tag);
  return v.tag;
}

for (let round = 0; round < 40; round++) {
  for (const arr of [small, large]) {
    const step = arr.length / 64;
    for (let k = 0; k < 64; k++) set(arr, k * step, young(round * 1000 + k, (round + k) & 3));
    // primitives over references take the same path
    if (round % 5 === 4) for (let k = 0; k < 64; k += 2) set(arr, k * step, k & 4 ? undefined : null);
  }
  churn(20000);
  for (const arr of [small, large]) {
    const step = arr.length / 64;
    for (let k = 0; k < 64; k++) {
      const v = arr[k * step];
      if (round % 5 === 4 && k % 2 === 0) assert.strictEqual(v, k & 4 ? undefined : null);
      else assert.strictEqual(value(v), round * 1000 + k);
    }
  }
}

// appends into old arrays remember them too
const grown = [];
for (let i = 0; i < 2000; i++) grown.push(null);
churn(300000);
grown.length = 0;
for (let round = 0; round < 20; round++) {
  const base = grown.length;
  for (let k = 0; k < 50; k++) set(grown, base + k, young(round * 1000 + k, k & 3));
  churn(20000);
  for (let k = 0; k < 50; k++) assert.strictEqual(value(grown[base + k]), round * 1000 + k);
}

function pushOne(arr, v) { arr.push(v); }
const pushedOld = [];
for (let i = 0; i < 2000; i++) pushedOld.push(null);
churn(300000);
pushedOld.length = 0;
for (let round = 0; round < 20; round++) {
  const base = pushedOld.length;
  for (let k = 0; k < 50; k++) pushOne(pushedOld, young(round * 1000 + k, k & 3));
  churn(20000);
  for (let k = 0; k < 50; k++) assert.strictEqual(value(pushedOld[base + k]), round * 1000 + k);
}

const holey = [0, 1, , 3];
set(holey, 2, 'filled');
assert.strictEqual(holey[2], 'filled');
assert.strictEqual(holey.length, 4);

const frozen = Object.freeze([1, 2, 3]);
set(frozen, 0, 'no');
assert.strictEqual(frozen[0], 1);

console.log('PASS compiled element stores into old arrays keep young values');
