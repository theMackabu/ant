const assert = require('node:assert');

function checkSort(input, compare, expected, label) {
  const original = input.slice();
  const sorted = input.slice();
  assert.strictEqual(sorted.sort(compare), sorted, `${label}: sort returns receiver`);
  assert.deepStrictEqual(sorted, expected, `${label}: sort`);
  assert.deepStrictEqual(input.toSorted(compare), expected, `${label}: toSorted`);
  assert.deepStrictEqual(input, original, `${label}: toSorted preserves input`);
}

checkSort([0.2, 0.1], (a, b) => a - b, [0.1, 0.2], 'fractional difference');
checkSort(
  [8.42, 8.11, 8.37, 8.05, 8.23], (a, b) => a - b,
  [8.05, 8.11, 8.23, 8.37, 8.42], 'nearby benchmark samples'
);
checkSort(
  [-0.2, 0.1, -0.1, 0.2], (a, b) => b - a,
  [0.2, 0.1, -0.1, -0.2], 'descending fractional difference'
);

for (const magnitude of [Number.MIN_VALUE, 0.25, 1, 2 ** 32, Number.MAX_VALUE, Infinity]) {
  const compare = (a, b) => a < b ? -magnitude : a > b ? magnitude : 0;
  checkSort([3, 1, 4, 2], compare, [1, 2, 3, 4], `ascending magnitude ${magnitude}`);
  checkSort([3, 1, 4, 2], (a, b) => compare(b, a), [4, 3, 2, 1], `descending magnitude ${magnitude}`);
}

const entries = [
  { key: 2, id: 'a' }, { key: 1, id: 'b' },
  { key: 2, id: 'c' }, { key: 1, id: 'd' }
];
checkSort(
  entries, (a, b) => (a.key - b.key) / 10,
  [entries[1], entries[3], entries[0], entries[2]], 'stable fractional comparison'
);
for (const equal of [0, -0, NaN]) {
  checkSort(entries, () => equal, entries, `equal comparison ${equal}`);
}

console.log('array sort comparator sign: ok');
