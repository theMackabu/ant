import { test, summary } from './helpers.js';

console.log('sessionStorage Tests\n');

test('sessionStorage exists', typeof sessionStorage, 'object');

sessionStorage.setItem('key1', 'value1');
test('setItem/getItem basic', sessionStorage.getItem('key1'), 'value1');

sessionStorage.setItem('key1', 'newValue');
test('setItem overwrites', sessionStorage.getItem('key1'), 'newValue');

test('getItem non-existent', sessionStorage.getItem('nonExistent'), null);

sessionStorage.clear();
test('length after clear', sessionStorage.length, 0);

sessionStorage.setItem('a', '1');
test('length after 1 item', sessionStorage.length, 1);

sessionStorage.setItem('b', '2');
test('length after 2 items', sessionStorage.length, 2);

sessionStorage.setItem('c', '3');
test('length after 3 items', sessionStorage.length, 3);

let keys = [];
for (let i = 0; i < sessionStorage.length; i++) {
  keys.push(sessionStorage.key(i));
}
test('key() returns keys', keys.length, 3);
test('key() includes a', keys.includes('a'), true);
test('key() includes b', keys.includes('b'), true);
test('key() includes c', keys.includes('c'), true);

test('key() out of bounds', sessionStorage.key(100), null);
test('key() negative index', sessionStorage.key(-1), null);

sessionStorage.removeItem('b');
test('removeItem decreases length', sessionStorage.length, 2);
test('removeItem removes key', sessionStorage.getItem('b'), null);
test('removeItem keeps others', sessionStorage.getItem('a'), '1');

sessionStorage.removeItem('nonExistent');
test('removeItem non-existent key', sessionStorage.length, 2);

sessionStorage.clear();
test('clear removes all', sessionStorage.length, 0);
test('clear removes a', sessionStorage.getItem('a'), null);
test('clear removes c', sessionStorage.getItem('c'), null);

sessionStorage.setItem('number', '42');
test('store number string', sessionStorage.getItem('number'), '42');

sessionStorage.setItem('bool', 'true');
test('store bool string', sessionStorage.getItem('bool'), 'true');

sessionStorage.setItem('empty', '');
test('store empty string', sessionStorage.getItem('empty'), '');

sessionStorage.setItem('key-with-dash', 'dash');
test('key with dash', sessionStorage.getItem('key-with-dash'), 'dash');

sessionStorage.setItem('key_with_underscore', 'underscore');
test('key with underscore', sessionStorage.getItem('key_with_underscore'), 'underscore');

sessionStorage.setItem('key.with.dot', 'dot');
test('key with dot', sessionStorage.getItem('key.with.dot'), 'dot');

sessionStorage.setItem('special', 'hello\nworld');
test('value with newline', sessionStorage.getItem('special'), 'hello\nworld');

sessionStorage.setItem('unicode', 'Hello');
test('value with unicode', sessionStorage.getItem('unicode'), 'Hello');

const obj = { name: 'John', age: 30 };
sessionStorage.setItem('user', JSON.stringify(obj));
const retrieved = JSON.parse(sessionStorage.getItem('user'));
test('JSON storage name', retrieved.name, 'John');
test('JSON storage age', retrieved.age, 30);

sessionStorage.clear();

summary();
