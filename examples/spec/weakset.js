import { test, summary } from './helpers.js';

console.log('WeakSet Tests\n');

const ws = new WeakSet();
const obj1 = {};
const obj2 = {};

ws.add(obj1);
ws.add(obj2);

test('weakset has true', ws.has(obj1), true);
test('weakset has true 2', ws.has(obj2), true);
test('weakset has false', ws.has({}), false);

ws.delete(obj1);
test('weakset delete', ws.has(obj1), false);
test('weakset still has obj2', ws.has(obj2), true);

const obj3 = { id: 1 };
ws.add(obj3);
ws.add(obj3);
test('weakset duplicate add', ws.has(obj3), true);

summary();
