const assert = require('assert');
const { spawnSync } = require('child_process');

if (!process.env.ANT_JIT_FOR_OF_CHILD) {
  const child = spawnSync(process.execPath, [__filename], {
    encoding: 'utf8',
    env: {
      ...process.env,
      ANT_DEBUG: 'dump/vm:op-warn',
      ANT_JIT_FOR_OF_CHILD: '1',
    },
  });
  assert.strictEqual(child.status, 0, child.stdout + child.stderr);
  assert.doesNotMatch(child.stderr, /jit: ineligible op ITER_(?:NEXT|CLOSE)/);
  process.stdout.write(child.stdout);
  process.exit(0);
}

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

const osrCoercible = {
  valueOf() {
    return 7;
  },
};

function nestedForOfOsrBailout() {
  let total = 0;
  for (const outer of [10, 20, 30]) {
    let inner = 0;
    for (let i = 0; i < 700; i++) {
      inner += outer === 10 && i === 600 ? osrCoercible : 1;
    }
    total += outer + inner;
  }
  return total;
}

assert.strictEqual(nestedForOfOsrBailout(), 2166);

console.log('JIT for-of tests passed');
