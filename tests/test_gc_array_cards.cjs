// Old arrays remember young stores by card (see gc_card_table_t). Stores and
// in-place moves on large old arrays, with enough allocation in between for
// minors to run, must never lose a young value. Verify builds also run
// gc_verify_cards before every minor, which aborts if a young reference sits
// in a clean card.
const assert = require('node:assert');

const SIZE = 4096;
const arrays = [];
for (let a = 0; a < 4; a++) {
  const arr = [];
  for (let i = 0; i < SIZE; i++) arr.push(i);
  arrays.push(arr);
}

function churn(n) {
  let keep = null;
  for (let i = 0; i < n; i++) keep = { i, row: [i, i + 1] };
  return keep;
}

// several minors, so the arrays are old before the stores start
churn(60000);

let seq = 0;
const young = () => ({ tag: ++seq, pad: [seq, seq] });

let seed = 12345;
const rand = n => {
  seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
  return seed % n;
};

const key = v => (typeof v === 'object' ? v.tag : v);

function check() {
  for (const arr of arrays) {
    assert.strictEqual(arr.length, SIZE);
    for (const v of arr) {
      if (typeof v !== 'object') continue;
      assert.strictEqual(typeof v.tag, 'number');
      assert.strictEqual(v.pad[0], v.tag);
      assert.strictEqual(v.pad[1], v.tag);
    }
  }
}

for (let round = 0; round < 70; round++) {
  const arr = arrays[round % arrays.length];
  for (let k = 0; k < 20; k++) arr[rand(SIZE)] = young();

  switch (round % 7) {
    case 0: arr.push(arr.shift()); break;
    case 1: arr.unshift(young()); arr.pop(); break;
    case 2: arr.splice(rand(SIZE - 10), 5, young(), young(), young(), young(), young()); break;
    case 3: arr.reverse(); break;
    case 4: arr.copyWithin(rand(100), 200, 400); break;
    case 5: arr.sort((x, y) => key(x) - key(y)); break;
    case 6: arr.fill(young(), rand(100), 100 + rand(100)); break;
  }

  churn(3000);
  check();
}

// named properties on large old arrays that take no element stores: a minor
// rescans the named slots of a remembered object whatever its card table says
const named = [];
for (let a = 0; a < 4; a++) {
  const arr = [];
  for (let i = 0; i < SIZE; i++) arr.push(i);
  named.push(arr);
}
churn(60000);
function tagArray(arr) { arr.tag = young(); return seq; }
for (let round = 0; round < 60; round++) {
  const arr = named[round % named.length];
  const tag = tagArray(arr);
  churn(40000); // at least a nursery's worth, so a minor runs
  assert.strictEqual(arr.tag.tag, tag);
  assert.strictEqual(arr.tag.pad[1], tag);
}

console.log('PASS card-marked old arrays keep young values through stores and moves');
