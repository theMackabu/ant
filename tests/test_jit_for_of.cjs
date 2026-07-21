const assert = require('assert');

function collect(iterable) {
  const values = [];
  for (const value of iterable) values.push(value);
  return values;
}

function sum(iterable) {
  let result = 0;
  for (const value of iterable) result += value;
  return result;
}

for (let i = 0; i < 350; i++) {
  assert.strictEqual(sum([1, 2, 3, 4]), 10);
  assert.deepStrictEqual(collect('a😀b'), ['a', '😀', 'b']);
  assert.deepStrictEqual(collect(new Set([1, 2, 3])), [1, 2, 3]);
  assert.deepStrictEqual(collect(new Map([['a', 1], ['b', 2]]).keys()), ['a', 'b']);
}

Array.prototype[1] = 42;
try {
  assert.deepStrictEqual(collect([1, , 3]), [1, 42, 3]);
} finally {
  delete Array.prototype[1];
}

let closes = 0;
const closable = {
  [Symbol.iterator]() {
    let value = 0;
    return {
      next() {
        return { value: value++, done: false };
      },
      return() {
        closes++;
        return { done: true };
      },
    };
  },
};

function first(iterable) {
  let result;
  for (const value of iterable) {
    result = value;
    break;
  }
  return result;
}

for (let i = 0; i < 350; i++) assert.strictEqual(first(closable), 0);
assert.strictEqual(closes, 350);

const throwing = {
  [Symbol.iterator]() {
    return {
      next() {
        throw new Error('next failed');
      },
    };
  },
};

let caught = '';
try {
  collect(throwing);
} catch (error) {
  caught = error.message;
}
assert.strictEqual(caught, 'next failed');

const osrValues = new Map();
for (let i = 0; i < 600; i++) osrValues.set(i, i);
const osrOffsets = [[-1, 0], [1, 0], [0, -1], [0, 1]];

function nestedForOfOsr() {
  let total = 0;
  for (const value of osrValues.values()) {
    for (const [x, y] of osrOffsets) total += value + x + y;
  }
  return total;
}

assert.strictEqual(nestedForOfOsr(), 718800);

console.log('JIT for-of tests passed');
