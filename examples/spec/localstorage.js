import { unlinkSync } from 'ant:fs';
import { test, testThrows, summary } from './helpers.js';

console.log('localStorage Tests\n');

test('localStorage exists', typeof localStorage, 'object');

test('setFile method exists', typeof localStorage.setFile, 'function');

testThrows('setItem throws without file', () => localStorage.setItem('x', 'y'));
testThrows('getItem throws without file', () => localStorage.getItem('x'));
testThrows('removeItem throws without file', () => localStorage.removeItem('x'));
testThrows('clear throws without file', () => localStorage.clear());
testThrows('key throws without file', () => localStorage.key(0));

localStorage.setFile('storage.json');

localStorage.setItem('key1', 'value1');
test('setItem/getItem basic', localStorage.getItem('key1'), 'value1');

localStorage.setItem('key1', 'newValue');
test('setItem overwrites', localStorage.getItem('key1'), 'newValue');

test('getItem non-existent', localStorage.getItem('nonExistent'), null);

localStorage.clear();
test('length after clear', localStorage.length, 0);

localStorage.setItem('a', '1');
test('length after 1 item', localStorage.length, 1);

localStorage.setItem('b', '2');
test('length after 2 items', localStorage.length, 2);

localStorage.setItem('c', '3');
test('length after 3 items', localStorage.length, 3);

let keys = [];
for (let i = 0; i < localStorage.length; i++) {
  keys.push(localStorage.key(i));
}
test('key() returns keys', keys.length, 3);
test('key() includes a', keys.includes('a'), true);
test('key() includes b', keys.includes('b'), true);
test('key() includes c', keys.includes('c'), true);

test('key() out of bounds', localStorage.key(100), null);
test('key() negative index', localStorage.key(-1), null);

localStorage.removeItem('b');
test('removeItem decreases length', localStorage.length, 2);
test('removeItem removes key', localStorage.getItem('b'), null);
test('removeItem keeps others', localStorage.getItem('a'), '1');

localStorage.removeItem('nonExistent');
test('removeItem non-existent key', localStorage.length, 2);

localStorage.clear();
test('clear removes all', localStorage.length, 0);
test('clear removes a', localStorage.getItem('a'), null);
test('clear removes c', localStorage.getItem('c'), null);

localStorage.setItem('number', '42');
test('store number string', localStorage.getItem('number'), '42');

localStorage.setItem('bool', 'true');
test('store bool string', localStorage.getItem('bool'), 'true');

localStorage.setItem('empty', '');
test('store empty string', localStorage.getItem('empty'), '');

localStorage.setItem('key-with-dash', 'dash');
test('key with dash', localStorage.getItem('key-with-dash'), 'dash');

localStorage.setItem('key_with_underscore', 'underscore');
test('key with underscore', localStorage.getItem('key_with_underscore'), 'underscore');

localStorage.setItem('key.with.dot', 'dot');
test('key with dot', localStorage.getItem('key.with.dot'), 'dot');

localStorage.setItem('special', 'hello\nworld');
test('value with newline', localStorage.getItem('special'), 'hello\nworld');

localStorage.setItem('unicode', 'Hello');
test('value with unicode', localStorage.getItem('unicode'), 'Hello');

const obj = { name: 'John', age: 30 };
localStorage.setItem('user', JSON.stringify(obj));
const retrieved = JSON.parse(localStorage.getItem('user'));
test('JSON storage name', retrieved.name, 'John');
test('JSON storage age', retrieved.age, 30);

localStorage.clear();
unlinkSync('storage.json');

summary();
