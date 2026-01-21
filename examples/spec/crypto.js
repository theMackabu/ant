import { test, summary } from './helpers.js';

console.log('Crypto Tests\n');

test('crypto exists', typeof crypto, 'object');
test('crypto toStringTag', Object.prototype.toString.call(crypto), '[object Crypto]');

const rand1 = crypto.random();
const rand2 = crypto.random();
test('random returns number', typeof rand1, 'number');
test('random returns positive', rand1 >= 0, true);
test('random returns different values', rand1 !== rand2, true);

const bytes = crypto.randomBytes(16);
test('randomBytes returns object', typeof bytes, 'object');
test('randomBytes length', bytes.length, 16);
test('randomBytes values are numbers', typeof bytes[0], 'number');
test('randomBytes values in range', bytes[0] >= 0 && bytes[0] <= 255, true);

const uuid1 = crypto.randomUUID();
const uuid2 = crypto.randomUUID();
test('randomUUID returns string', typeof uuid1, 'string');
test('randomUUID length', uuid1.length, 36);
test('randomUUID format', /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(uuid1), true);
test('randomUUID unique', uuid1 !== uuid2, true);

const uuidv7_1 = crypto.randomUUIDv7();
const uuidv7_2 = crypto.randomUUIDv7();
test('randomUUIDv7 returns string', typeof uuidv7_1, 'string');
test('randomUUIDv7 length', uuidv7_1.length, 36);
test('randomUUIDv7 version 7', uuidv7_1[14], '7');
test('randomUUIDv7 unique', uuidv7_1 !== uuidv7_2, true);
test('randomUUIDv7 monotonic', uuidv7_1 < uuidv7_2, true);

const arr = new Uint8Array(8);
const result = crypto.getRandomValues(arr);
test('getRandomValues returns same array', result === arr, true);
test(
  'getRandomValues fills array',
  Array.from(arr).some(v => v !== 0),
  true
);

const arr32 = new Uint32Array(4);
crypto.getRandomValues(arr32);
test(
  'getRandomValues works with Uint32Array',
  Array.from(arr32).some(v => v !== 0),
  true
);

summary();
