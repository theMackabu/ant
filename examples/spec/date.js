import { test, summary } from './helpers.js';

console.log('Date Tests\n');

const now1 = Date.now();
test('Date.now() is number', typeof now1, 'number');
test('Date.now() > 1970', now1 > 1000000000000, true);

const d1 = new Date();
test('new Date() is object', typeof d1, 'object');

const d2 = new Date(1234567890000);
test('new Date(ts) is object', typeof d2, 'object');

const t1 = Date.now();
const t2 = Date.now();
test('Date.now() is monotonic', t2 >= t1, true);

const times = [];
for (let i = 0; i < 3; i++) times.push(Date.now());
test('multiple calls are numbers', times.every(t => typeof t === 'number'), true);

const d = new Date(1234567890000);
test('getTime returns timestamp', d.getTime(), 1234567890000);
test('getFullYear is number', typeof d.getFullYear(), 'number');
test('getMonth is number', typeof d.getMonth(), 'number');
test('getDate is number', typeof d.getDate(), 'number');
test('getHours is number', typeof d.getHours(), 'number');
test('getMinutes is number', typeof d.getMinutes(), 'number');
test('getSeconds is number', typeof d.getSeconds(), 'number');
test('getMilliseconds is number', typeof d.getMilliseconds(), 'number');
test('getDay is number', typeof d.getDay(), 'number');

test('toISOString is string', typeof d.toISOString(), 'string');
test('toISOString contains T', d.toISOString().includes('T'), true);
test('toString is string', typeof d.toString(), 'string');

summary();
