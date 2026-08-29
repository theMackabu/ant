const assert = require('assert');

function makeWide(value) {
  return [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, value];
}

function makeHoley(value) {
  return [value, , 2, , 4, , 6, , 8, , 10];
}

let retained;
for (let i = 0; i < 500; i++) {
  retained = { marker: i };
  const wide = makeWide(retained);
  assert.strictEqual(wide.length, 11);
  assert.strictEqual(wide[0], 0);
  assert.strictEqual(wide[9], 9);
  assert.strictEqual(wide[10], retained);

  const holey = makeHoley(i);
  assert.strictEqual(holey.length, 11);
  assert.strictEqual(holey[0], i);
  assert.strictEqual(1 in holey, false);
  assert.strictEqual(2 in holey, true);
  assert.strictEqual(9 in holey, false);
}

const holder = makeWide(retained);
for (let i = 0; i < 50000; i++) makeWide({ discarded: i });
assert.strictEqual(holder[10].marker, 499);

console.log('JIT array literal tests passed');
