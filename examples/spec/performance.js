import { test, summary } from './helpers.js';

console.log('Performance Tests\n');

test('performance exists', typeof performance, 'object');
test('performance toStringTag', Object.prototype.toString.call(performance), '[object Performance]');

const t1 = performance.now();
test('now returns number', typeof t1, 'number');
test('now returns positive', t1 >= 0, true);

let sum = 0;
for (let i = 0; i < 10000; i++) sum += i;
const t2 = performance.now();
test('now advances', t2 >= t1, true);

test('timeOrigin exists', typeof performance.timeOrigin, 'number');
test('timeOrigin positive', performance.timeOrigin > 0, true);
test('timeOrigin before now', performance.timeOrigin <= Date.now(), true);

summary();
