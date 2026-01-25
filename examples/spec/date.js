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

const d3 = new Date(1234567890000);
d3.setTime(555555555555);
test('setTime updates time', d3.getTime(), 555555555555);

const d4 = new Date(1234567890000);
d4.setTime();
test('setTime() with no args returns NaN', isNaN(d4.getTime()), true);

const d5 = new Date(1234567890000);
d5.setMilliseconds();
test('setMilliseconds() with no args invalidates', isNaN(d5.getTime()), true);

const d6 = new Date(1234567890000);
d6.setSeconds();
test('setSeconds() with no args invalidates', isNaN(d6.getTime()), true);

const d7 = new Date(1234567890000);
d7.setMinutes();
test('setMinutes() with no args invalidates', isNaN(d7.getTime()), true);

const d8 = new Date(1234567890000);
d8.setHours();
test('setHours() with no args invalidates', isNaN(d8.getTime()), true);

const d9 = new Date(1234567890000);
d9.setDate();
test('setDate() with no args invalidates', isNaN(d9.getTime()), true);

const d10 = new Date(1234567890000);
d10.setMonth();
test('setMonth() with no args invalidates', isNaN(d10.getTime()), true);

const d11 = new Date(1234567890000);
d11.setFullYear();
test('setFullYear() with no args invalidates', isNaN(d11.getTime()), true);

const d12 = new Date(1234567890000);
d12.setFullYear(2025);
test('setFullYear(2025) works', d12.getFullYear(), 2025);

const d13 = new Date(1234567890000);
d13.setMonth(0);
test('setMonth(0) sets January', d13.getMonth(), 0);

const d14 = new Date(1234567890000);
d14.setDate(15);
test('setDate(15) sets day', d14.getDate(), 15);

const d15 = new Date(1234567890000);
d15.setHours(10);
test('setHours(10) sets hour', d15.getHours(), 10);

const d16 = new Date(1234567890000);
d16.setMinutes(30);
test('setMinutes(30) sets minutes', d16.getMinutes(), 30);

const d17 = new Date(1234567890000);
d17.setSeconds(45);
test('setSeconds(45) sets seconds', d17.getSeconds(), 45);

const d18 = new Date(1234567890000);
d18.setMilliseconds(500);
test('setMilliseconds(500) sets ms', d18.getMilliseconds(), 500);

const d19 = new Date(1234567890000);
d19.setTime(NaN);
test('setTime(NaN) invalidates', isNaN(d19.getTime()), true);

const d20 = new Date(1234567890000);
d20.setFullYear(NaN);
test('setFullYear(NaN) invalidates', isNaN(d20.getTime()), true);

const d21 = new Date(1234567890000);
d21.setMonth(NaN);
test('setMonth(NaN) invalidates', isNaN(d21.getTime()), true);

const d22 = new Date(1234567890000);
d22.setDate(NaN);
test('setDate(NaN) invalidates', isNaN(d22.getTime()), true);

const d23 = new Date(1234567890000);
d23.setHours(NaN);
test('setHours(NaN) invalidates', isNaN(d23.getTime()), true);

const d24 = new Date(1234567890000);
d24.setMinutes(NaN);
test('setMinutes(NaN) invalidates', isNaN(d24.getTime()), true);

const d25 = new Date(1234567890000);
d25.setSeconds(NaN);
test('setSeconds(NaN) invalidates', isNaN(d25.getTime()), true);

const d26 = new Date(1234567890000);
d26.setMilliseconds(NaN);
test('setMilliseconds(NaN) invalidates', isNaN(d26.getTime()), true);

summary();
