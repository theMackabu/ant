const assert = require('node:assert');

function fill(o, from, to) {
  for (let i = from; i < to; i++) o['k' + i] = i;
}

function check(o, keys) {
  assert.deepStrictEqual(Object.keys(o), keys);
  for (const k of keys) assert.strictEqual(o[k], +k.slice(1));
}

function range(from, to) {
  const out = [];
  for (let i = from; i < to; i++) out.push('k' + i);
  return out;
}

for (const size of [5, 8, 9, 12, 13, 21, 36, 70]) {
  const o = {};
  fill(o, 0, size);
  check(o, range(0, size));

  for (let i = size - 1; i >= 2; i--) delete o['k' + i];
  check(o, range(0, 2));

  fill(o, 2, size + 3);
  check(o, range(0, size + 3));

  for (let i = 0; i < size; i += 2) delete o['k' + i];
  const kept = range(0, size + 3).filter((k) => !(+k.slice(1) < size && +k.slice(1) % 2 === 0));
  check(o, kept);

  fill(o, size + 3, size + 40);
  check(o, kept.concat(range(size + 3, size + 40)));
}

function Wide(a) {
  this.p0 = a; this.p1 = a; this.p2 = a; this.p3 = a; this.p4 = a;
  this.p5 = a; this.p6 = a; this.p7 = a; this.p8 = a; this.p9 = a;
}

const wides = [];
for (let n = 0; n < 2000; n++) {
  const w = new Wide(n);
  for (let i = 0; i < (n % 30); i++) w['x' + i] = i * n;
  if (n % 3 === 0) for (let i = 0; i < (n % 30); i += 2) delete w['x' + i];
  wides.push(w);
}

for (let r = 0; r < 4; r++) {
  const junk = [];
  for (let i = 0; i < 50000; i++) junk.push({ a: i, b: i, c: i, d: i, e: i, f: i });
}

for (let n = 0; n < wides.length; n++) {
  const w = wides[n];
  for (let p = 0; p < 10; p++) assert.strictEqual(w['p' + p], n);
  for (let i = 0; i < (n % 30); i++) {
    const deleted = n % 3 === 0 && i % 2 === 0;
    assert.strictEqual(w['x' + i], deleted ? undefined : i * n);
  }
}

function addTail(o, n) {
  for (let i = 0; i < n; i++) o['t' + i] = i;
  return o;
}

for (let round = 0; round < 3000; round++) {
  const o = addTail({}, 4 + (round % 20));
  assert.strictEqual(o['t' + (3 + (round % 20))], 3 + (round % 20));
}

console.log('ok');
