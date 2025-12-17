import { test, summary } from './helpers.js';

console.log('Map Tests\n');

const map = new Map();

map.set('key1', 'value1');
map.set('key2', 42);
map.set('key3', true);

test('map size after set', map.size(), 3);
test('map get string', map.get('key1'), 'value1');
test('map get number', map.get('key2'), 42);
test('map get boolean', map.get('key3'), true);

test('map has existing', map.has('key1'), true);
test('map has missing', map.has('missing'), false);

test('map delete returns true', map.delete('key2'), true);
test('map has after delete', map.has('key2'), false);
test('map size after delete', map.size(), 2);

map.set('key1', 'newvalue1');
test('map get after overwrite', map.get('key1'), 'newvalue1');

map.clear();
test('map size after clear', map.size(), 0);
test('map has after clear', map.has('key1'), false);

map.set(123, 'number key');
map.set(true, 'boolean key');
map.set(null, 'null key');

test('map get number key', map.get(123), 'number key');
test('map get boolean key', map.get(true), 'boolean key');
test('map get null key', map.get(null), 'null key');
test('map size with varied keys', map.size(), 3);

map.clear();
map.set('a', 1).set('b', 2).set('c', 3);
test('map chaining size', map.size(), 3);
test('map chaining get', map.get('b'), 2);

test('map get undefined key', map.get('nonexistent'), undefined);
test('map delete nonexistent', map.delete('nonexistent'), false);

summary();
