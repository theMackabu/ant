import { test, summary } from './helpers.js';

console.log('Date Tests\n');

const BASE_TS = 1234567890000;
const expectInvalid = (label, mutator) => {
  const dt = new Date(BASE_TS);
  mutator(dt);
  test(label, Number.isNaN(dt.getTime()), true);
};

const now1 = Date.now();
test('Date.now() is number', typeof now1, 'number');
test('Date.now() > 1970', now1 > 1000000000000, true);

const d1 = new Date();
test('new Date() is object', typeof d1, 'object');

const d2 = new Date(BASE_TS);
test('new Date(ts) is object', typeof d2, 'object');

const t1 = Date.now();
const t2 = Date.now();
test('Date.now() is monotonic', t2 >= t1, true);

const times = [];
for (let i = 0; i < 3; i++) times.push(Date.now());
test('multiple calls are numbers', times.every(t => typeof t === 'number'), true);

const d = new Date(BASE_TS);
test('getTime returns timestamp', d.getTime(), BASE_TS);
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

const d3 = new Date(BASE_TS);
d3.setTime(555555555555);
test('setTime updates time', d3.getTime(), 555555555555);

expectInvalid('setTime() with no args invalidates', d => d.setTime());

expectInvalid('setMilliseconds() with no args invalidates', d => d.setMilliseconds());

expectInvalid('setSeconds() with no args invalidates', d => d.setSeconds());

expectInvalid('setMinutes() with no args invalidates', d => d.setMinutes());

expectInvalid('setHours() with no args invalidates', d => d.setHours());

expectInvalid('setDate() with no args invalidates', d => d.setDate());

expectInvalid('setMonth() with no args invalidates', d => d.setMonth());

expectInvalid('setFullYear() with no args invalidates', d => d.setFullYear());

const d12 = new Date(BASE_TS);
d12.setFullYear(2025);
test('setFullYear(2025) works', d12.getFullYear(), 2025);

const d13 = new Date(BASE_TS);
d13.setMonth(0);
test('setMonth(0) sets January', d13.getMonth(), 0);

const d14 = new Date(BASE_TS);
d14.setDate(15);
test('setDate(15) sets day', d14.getDate(), 15);

const d15 = new Date(BASE_TS);
d15.setHours(10);
test('setHours(10) sets hour', d15.getHours(), 10);

const d16 = new Date(BASE_TS);
d16.setMinutes(30);
test('setMinutes(30) sets minutes', d16.getMinutes(), 30);

const d17 = new Date(BASE_TS);
d17.setSeconds(45);
test('setSeconds(45) sets seconds', d17.getSeconds(), 45);

const d18 = new Date(BASE_TS);
d18.setMilliseconds(500);
test('setMilliseconds(500) sets ms', d18.getMilliseconds(), 500);

expectInvalid('setTime(NaN) invalidates', d => d.setTime(NaN));
expectInvalid('setFullYear(NaN) invalidates', d => d.setFullYear(NaN));
expectInvalid('setMonth(NaN) invalidates', d => d.setMonth(NaN));
expectInvalid('setDate(NaN) invalidates', d => d.setDate(NaN));
expectInvalid('setHours(NaN) invalidates', d => d.setHours(NaN));
expectInvalid('setMinutes(NaN) invalidates', d => d.setMinutes(NaN));
expectInvalid('setSeconds(NaN) invalidates', d => d.setSeconds(NaN));
expectInvalid('setMilliseconds(NaN) invalidates', d => d.setMilliseconds(NaN));

summary();
