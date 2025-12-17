import { test, summary } from './helpers.js';

console.log('WeakMap Tests\n');

const wm = new WeakMap();
const key1 = {};
const key2 = {};

wm.set(key1, 'value1');
wm.set(key2, 42);

test('weakmap get', wm.get(key1), 'value1');
test('weakmap get number', wm.get(key2), 42);
test('weakmap has true', wm.has(key1), true);
test('weakmap has false', wm.has({}), false);

wm.delete(key1);
test('weakmap delete', wm.has(key1), false);

const key3 = { id: 1 };
wm.set(key3, 'test');
wm.set(key3, 'updated');
test('weakmap overwrite', wm.get(key3), 'updated');

summary();
