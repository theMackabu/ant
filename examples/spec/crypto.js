import { test, testDeep, summary } from './helpers.js';

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

test('subtle.generateKey exists', typeof crypto.subtle.generateKey, 'function');
test('subtle.exportKey exists', typeof crypto.subtle.exportKey, 'function');

async function testRejects(name, call, errorName) {
  let promise;
  try {
    promise = call();
  } catch (error) {
    test(`${name} does not throw synchronously`, error.name, 'no synchronous exception');
    return;
  }
  test(`${name} returns a Promise`, promise instanceof Promise, true);
  let rejection;
  try {
    await promise;
  } catch (error) {
    rejection = error;
  }
  test(`${name} rejection`, rejection?.name, errorName);
}

const message = new Uint8Array([1, 2, 3, 4]);
for (const length of [128, 192, 256]) {
  const algorithm = { name: 'aes-gcm', length };
  const usages = ['decrypt', 'encrypt', 'encrypt'];
  const key = await crypto.subtle.generateKey(algorithm, true, usages);
  test(`AES-${length} key type`, key.type, 'secret');
  test(`AES-${length} extractable`, key.extractable, true);
  testDeep(`AES-${length} algorithm`, key.algorithm, { name: 'AES-GCM', length });
  testDeep(`AES-${length} usages`, key.usages, ['encrypt', 'decrypt']);

  algorithm.name = 'HMAC';
  usages[0] = 'sign';
  test(`AES-${length} algorithm snapshot`, key.algorithm.name, 'AES-GCM');
  testDeep(`AES-${length} usages snapshot`, key.usages, ['encrypt', 'decrypt']);

  const raw = await crypto.subtle.exportKey('raw', key);
  test(`AES-${length} export is ArrayBuffer`, raw instanceof ArrayBuffer, true);
  test(`AES-${length} export size`, raw.byteLength, length / 8);
  const imported = await crypto.subtle.importKey('raw', raw, 'AES-GCM', false, ['decrypt']);
  test(`AES-${length} imported extractability`, imported.extractable, false);
  test(`AES-${length} imported length`, imported.algorithm.length, length);

  const params = { name: 'AES-GCM', iv: new Uint8Array(12) };
  const encrypted = await crypto.subtle.encrypt(params, key, message);
  const decrypted = await crypto.subtle.decrypt(params, imported, encrypted);
  testDeep(`AES-${length} round trip`, Array.from(new Uint8Array(decrypted)), Array.from(message));
  await testRejects(`AES-${length} non-extractable export`, () => crypto.subtle.exportKey('raw', imported), 'InvalidAccessError');
  await testRejects(`AES-${length} usage enforcement`, () => crypto.subtle.encrypt(params, imported, message), 'InvalidAccessError');
}

for (const [hash, length] of [['SHA-1', 512], ['SHA-256', 512], ['SHA-384', 1024], ['SHA-512', 1024]]) {
  const key = await crypto.subtle.generateKey({ name: 'HMAC', hash }, true, new Set(['sign', 'verify']));
  test(`HMAC ${hash} algorithm`, key.algorithm.name, 'HMAC');
  test(`HMAC ${hash} hash`, key.algorithm.hash.name, hash);
  test(`HMAC ${hash} default length`, key.algorithm.length, length);
  const raw = await crypto.subtle.exportKey('raw', key);
  test(`HMAC ${hash} export size`, raw.byteLength, length / 8);
  const imported = await crypto.subtle.importKey('raw', raw, { name: 'HMAC', hash }, true, ['verify']);
  const exported = await crypto.subtle.exportKey('raw', imported);
  testDeep(`HMAC ${hash} raw round trip`, Array.from(new Uint8Array(exported)), Array.from(new Uint8Array(raw)));
  const signature = await crypto.subtle.sign('HMAC', key, message);
  test(`HMAC ${hash} verification`, await crypto.subtle.verify('HMAC', imported, signature, message), true);
  test(`HMAC ${hash} changed message`, await crypto.subtle.verify('HMAC', imported, signature, new Uint8Array([5])), false);
  await testRejects(`HMAC ${hash} usage enforcement`, () => crypto.subtle.sign('HMAC', imported, message), 'InvalidAccessError');
}

const shortKey = await crypto.subtle.generateKey({ name: 'HMAC', hash: 'SHA-256', length: 9 }, true, ['sign']);
const shortRaw = new Uint8Array(await crypto.subtle.exportKey('raw', shortKey));
test('HMAC explicit bit length', shortKey.algorithm.length, 9);
test('HMAC partial-byte size', shortRaw.length, 2);
test('HMAC unused bits cleared', shortRaw[1] & 0x7f, 0);

await testRejects('generateKey required arguments', () => crypto.subtle.generateKey('AES-GCM'), 'TypeError');
await testRejects('generateKey empty usages', () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }, true, []), 'SyntaxError');
await testRejects('generateKey invalid usage', () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }, true, ['sign']), 'SyntaxError');
await testRejects('generateKey invalid length', () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 100 }, true, ['encrypt']), 'OperationError');
await testRejects('HMAC zero length', () => crypto.subtle.generateKey({ name: 'HMAC', hash: 'SHA-256', length: 0 }, true, ['sign']), 'OperationError');
await testRejects('HMAC unsupported hash', () => crypto.subtle.generateKey({ name: 'HMAC', hash: 'MD5' }, true, ['sign']), 'NotSupportedError');
await testRejects('exportKey invalid format', () => crypto.subtle.exportKey('RAW', shortKey), 'TypeError');
await testRejects('exportKey unsupported format', () => crypto.subtle.exportKey('jwk', shortKey), 'NotSupportedError');

summary();
