import { test, summary } from './helpers.js';

console.log('Set Tests\n');

const set = new Set();

set.add('value1');
set.add('value2');
set.add(42);
set.add(true);

test('set size after add', set.size(), 4);
test('set has string', set.has('value1'), true);
test('set has number', set.has(42), true);
test('set has boolean', set.has(true), true);
test('set has missing', set.has('missing'), false);

test('set delete returns true', set.delete('value2'), true);
test('set has after delete', set.has('value2'), false);
test('set size after delete', set.size(), 3);

set.add('value1');
test('set size after duplicate', set.size(), 3);

set.add(123);
set.add(null);

test('set has 123', set.has(123), true);
test('set has null', set.has(null), true);
test('set size after more adds', set.size(), 5);

set.clear();
test('set size after clear', set.size(), 0);
test('set has after clear', set.has('value1'), false);

set.add('a').add('b').add('c');
test('set chaining size', set.size(), 3);
test('set chaining has a', set.has('a'), true);
test('set chaining has b', set.has('b'), true);
test('set chaining has c', set.has('c'), true);

test('set delete nonexistent', set.delete('nonexistent'), false);

summary();
