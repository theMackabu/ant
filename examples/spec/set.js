import { test, summary } from './helpers.js';

console.log('Set Tests\n');

const set = new Set();

set.add('value1');
set.add('value2');
set.add(42);
set.add(true);

test('set size after add', set.size, 4);
test('set has string', set.has('value1'), true);
test('set has number', set.has(42), true);
test('set has boolean', set.has(true), true);
test('set has missing', set.has('missing'), false);

test('set delete returns true', set.delete('value2'), true);
test('set has after delete', set.has('value2'), false);
test('set size after delete', set.size, 3);

set.add('value1');
test('set size after duplicate', set.size, 3);

set.add(123);
set.add(null);

test('set has 123', set.has(123), true);
test('set has null', set.has(null), true);
test('set size after more adds', set.size, 5);

set.clear();
test('set size after clear', set.size, 0);
test('set has after clear', set.has('value1'), false);

set.add('a').add('b').add('c');
test('set chaining size', set.size, 3);
test('set chaining has a', set.has('a'), true);
test('set chaining has b', set.has('b'), true);
test('set chaining has c', set.has('c'), true);

test('set delete nonexistent', set.delete('nonexistent'), false);

const setLike = {
  size: 2,
  has(value) {
    return value === 2 || value === 3;
  },
  keys() {
    return [2, 3][Symbol.iterator]();
  }
};

test('set union accepts set-like object', [...new Set([1, 2]).union(setLike)].join(','), '1,2,3');
test('set intersection accepts set-like object', [...new Set([1, 2]).intersection(setLike)].join(','), '2');
test('set difference accepts set-like object', [...new Set([1, 2]).difference(setLike)].join(','), '1');
test('set isSubsetOf accepts set-like object', new Set([2]).isSubsetOf(setLike), true);

const duplicateKeysSetLike = {
  size: 3,
  has(value) {
    return value === 2 || value === 3;
  },
  keys() {
    return [2, 3, 3][Symbol.iterator]();
  }
};

test('set symmetricDifference ignores duplicate set-like keys', [...new Set([1, 2]).symmetricDifference(duplicateKeysSetLike)].join(','), '1,3');

const directIteratorSetLike = {
  size: 2,
  has(value) {
    return value === 2 || value === 4;
  },
  keys() {
    const values = [2, 4];
    let index = 0;
    return {
      next() {
        return index < values.length ? { value: values[index++], done: false } : { done: true };
      }
    };
  }
};

test('set union accepts direct keys iterator', [...new Set([1, 2]).union(directIteratorSetLike)].join(','), '1,2,4');

let closedKeysIterator = false;
const nonSubsetSetLike = {
  size: 1,
  has() {
    return false;
  },
  keys() {
    return {
      next() {
        return { value: 2, done: false };
      },
      return() {
        closedKeysIterator = true;
        return {};
      }
    };
  }
};

test('set isSupersetOf closes keys iterator on early false', new Set([1]).isSupersetOf(nonSubsetSetLike), false);
test('set isSupersetOf called keys iterator return', closedKeysIterator, true);

const getSetRecordOrder = [];
try {
  new Set().union({
    get size() {
      getSetRecordOrder.push('size');
      return NaN;
    },
    get has() {
      getSetRecordOrder.push('has');
      return () => true;
    },
    keys() {
      return [][Symbol.iterator]();
    }
  });
} catch (e) {}

test('set GetSetRecord rejects NaN size before reading has', getSetRecordOrder.join(','), 'size');

summary();
