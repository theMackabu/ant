'use strict';

function assert(condition, message) {
  if (!condition) throw new Error(message || 'assertion failed');
}

function equalBytes(actual, expected, message) {
  const left = new Uint8Array(actual);
  const right = new Uint8Array(expected);
  assert(left.length === right.length, `${message}: byte length`);
  for (let i = 0; i < left.length; i++) {
    assert(left[i] === right[i], `${message}: byte ${i}`);
  }
}

async function rejectsAsPromise(factory, expectedName, message) {
  let promise;
  try {
    promise = factory();
  } catch (error) {
    throw new Error(`${message}: threw synchronously (${error && error.name})`);
  }
  assert(promise instanceof Promise, `${message}: did not return a Promise`);

  let rejection;
  try {
    await promise;
  } catch (error) {
    rejection = error;
  }
  assert(rejection, `${message}: expected rejection`);
  assert(
    rejection.name === expectedName,
    `${message}: expected ${expectedName}, got ${rejection.name}`
  );
  return rejection;
}

async function testAes() {
  const aesAlgorithm = { name: 'aes-gcm', length: 256 };
  const aesUsages = ['encrypt', 'decrypt', 'encrypt'];
  const aesKey = await crypto.subtle.generateKey(aesAlgorithm, true, aesUsages);
  assert(aesKey.type === 'secret', 'AES key type');
  assert(aesKey.extractable === true, 'AES extractability');
  assert(aesKey.algorithm.name === 'AES-GCM', 'AES normalized algorithm name');
  assert(aesKey.algorithm.length === 256, 'AES normalized algorithm length');
  assert(aesKey.usages.join(',') === 'encrypt,decrypt', 'AES normalized usages');

  aesAlgorithm.name = 'HMAC';
  aesAlgorithm.length = 128;
  aesUsages[0] = 'sign';
  aesUsages.push('verify');
  assert(aesKey.algorithm.name === 'AES-GCM', 'AES algorithm snapshot');
  assert(aesKey.algorithm.length === 256, 'AES length snapshot');
  assert(aesKey.usages.join(',') === 'encrypt,decrypt', 'AES usages snapshot');

  const raw = await crypto.subtle.exportKey('raw', aesKey);
  assert(raw instanceof ArrayBuffer && raw.byteLength === 32, 'AES raw export');

  const iv = crypto.getRandomValues(new Uint8Array(12));
  const plaintext = new TextEncoder().encode('webcrypto round trip');
  const ciphertext = await crypto.subtle.encrypt(
    { name: 'AES-GCM', iv }, aesKey, plaintext
  );
  const decryptKey = await crypto.subtle.importKey(
    'raw', raw, { name: 'AES-GCM' }, false, ['decrypt']
  );
  assert(decryptKey.extractable === false, 'imported AES extractability');
  assert(decryptKey.algorithm.name === 'AES-GCM', 'imported AES algorithm name');
  assert(decryptKey.algorithm.length === 256, 'imported AES algorithm length');
  assert(decryptKey.usages.join(',') === 'decrypt', 'imported AES usages');
  const decrypted = await crypto.subtle.decrypt(
    { name: 'AES-GCM', iv }, decryptKey, ciphertext
  );
  assert(new TextDecoder().decode(decrypted) === 'webcrypto round trip', 'AES round trip');

  const exportableImport = await crypto.subtle.importKey(
    'raw', raw, 'AES-GCM', true, ['encrypt']
  );
  assert(exportableImport.extractable === true, 'imported extractable flag');
  equalBytes(await crypto.subtle.exportKey('raw', exportableImport), raw, 'imported raw export');

  decryptKey.usages.push('encrypt');
  await rejectsAsPromise(
    () => crypto.subtle.encrypt({ name: 'AES-GCM', iv }, decryptKey, plaintext),
    'InvalidAccessError',
    'mutating reflected usages does not grant encrypt'
  );
  await rejectsAsPromise(
    () => crypto.subtle.exportKey('raw', decryptKey),
    'InvalidAccessError',
    'non-extractable export'
  );
  await rejectsAsPromise(
    () => crypto.subtle.exportKey('RAW', decryptKey),
    'TypeError', 'format conversion precedes extractability'
  );
  await rejectsAsPromise(
    () => crypto.subtle.exportKey('jwk', aesKey),
    'NotSupportedError', 'unsupported export format'
  );
  await rejectsAsPromise(
    () => crypto.subtle.importKey('raw', raw, 'AES-GCM'),
    'TypeError', 'importKey arity'
  );
  await rejectsAsPromise(
    () => crypto.subtle.importKey('raw', raw, 'AES-GCM', true, []),
    'SyntaxError', 'import empty usages'
  );
}

async function testHmac() {
  const hmacKey = await crypto.subtle.generateKey(
    { name: 'HMAC', hash: 'SHA-256' }, true, new Set(['sign', 'verify'])
  );
  assert(hmacKey.algorithm.name === 'HMAC', 'HMAC algorithm name');
  assert(hmacKey.algorithm.hash.name === 'SHA-256', 'HMAC hash name');
  assert(hmacKey.algorithm.length === 512, 'HMAC default block-size length');
  const hmacRaw = await crypto.subtle.exportKey('raw', hmacKey);
  assert(hmacRaw.byteLength === 64, 'HMAC default raw length');
  const message = new TextEncoder().encode('message');
  const signature = await crypto.subtle.sign('HMAC', hmacKey, message);
  assert(await crypto.subtle.verify('HMAC', hmacKey, signature, message), 'HMAC verify');

  const shortHmac = await crypto.subtle.generateKey(
    { name: 'HMAC', hash: { name: 'sha-256' }, length: 9 }, true, ['sign']
  );
  const shortRaw = new Uint8Array(await crypto.subtle.exportKey('raw', shortHmac));
  assert(shortHmac.algorithm.length === 9, 'HMAC bit length');
  assert(shortRaw.length === 2, 'HMAC partial-byte raw length');
  assert((shortRaw[1] & 0x7f) === 0, 'HMAC unused bits are cleared');

  const partialImport = await crypto.subtle.importKey(
    'raw', new Uint8Array([0xab, 0xff]),
    { name: 'HMAC', hash: 'SHA-256', length: 9 }, true, ['verify']
  );
  assert(partialImport.algorithm.length === 9, 'imported HMAC bit length');
  equalBytes(
    await crypto.subtle.exportKey('raw', partialImport),
    new Uint8Array([0xab, 0x80]),
    'imported HMAC unused bits'
  );
  await rejectsAsPromise(
    () => crypto.subtle.sign('HMAC', partialImport, message),
    'InvalidAccessError', 'verify-only key cannot sign'
  );
}

async function testValidation() {
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }),
    'TypeError',
    'generateKey arity'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }, true, []),
    'SyntaxError',
    'AES empty usages'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }, true, ['sign']),
    'SyntaxError',
    'AES invalid usage'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 100 }, true, ['encrypt']),
    'OperationError',
    'AES invalid length'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'HMAC', hash: 'SHA-256', length: 0 }, true, ['sign']),
    'OperationError',
    'HMAC zero length'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'HMAC', hash: 'MD5' }, true, ['sign']),
    'NotSupportedError',
    'HMAC unsupported hash'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'HMAC', hash: 'SHA-256' }, true, ['encrypt']),
    'SyntaxError',
    'HMAC invalid usage'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'HMAC', hash: 'SHA-256' }, true, ['invalid']),
    'TypeError',
    'invalid KeyUsage enum'
  );
  await rejectsAsPromise(
    () => crypto.subtle.exportKey('RAW', {}),
    'TypeError',
    'invalid KeyFormat enum'
  );
  await rejectsAsPromise(
    () => crypto.subtle.exportKey('raw', {}),
    'TypeError',
    'export non-CryptoKey'
  );
  for (const usages of ['', 'encrypt', {}, null, undefined]) {
    await rejectsAsPromise(
      () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }, true, usages),
      'TypeError', 'keyUsages must be an iterable object'
    );
  }
  for (const hash of ['SHA256', 'sha1', 'SHA-224']) {
    await rejectsAsPromise(
      () => crypto.subtle.generateKey({ name: 'HMAC', hash }, true, ['sign']),
      'NotSupportedError', 'backend hash aliases are not WebCrypto algorithms'
    );
  }
  for (const length of [NaN, Infinity, -1, 128n, { valueOf: () => 128n }]) {
    await rejectsAsPromise(
      () => crypto.subtle.generateKey({ name: 'AES-GCM', length }, true, ['encrypt']),
      'TypeError', 'invalid WebIDL key length'
    );
  }
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: 'HMAC', hash: 'SHA-256', length: -0.5 }, true, ['sign']),
    'OperationError', 'length is truncated before range checking'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ length: 128 }, true, ['encrypt']),
    'TypeError', 'algorithm name is required'
  );
  await rejectsAsPromise(
    () => crypto.subtle.generateKey({ name: Symbol('AES-GCM'), length: 128 }, true, ['encrypt']),
    'TypeError', 'algorithm name cannot be a Symbol'
  );
  await rejectsAsPromise(
    () => crypto.subtle.importKey('raw', new Uint8Array(1), 'PBKDF2', true, ['deriveBits']),
    'SyntaxError', 'PBKDF2 cannot be extractable'
  );
}

async function testSizesAndSnapshots() {
  for (const length of [128, 192, 256]) {
    const key = await crypto.subtle.generateKey({ name: 'AES-GCM', length }, true, ['decrypt', 'encrypt']);
    assert(key.algorithm.length === length, 'AES key length');
    assert(key.usages.join(',') === 'encrypt,decrypt', 'canonical usage order');
    assert((await crypto.subtle.exportKey('raw', key)).byteLength === length / 8, 'AES export size');
  }
  for (const [hash, length] of [['SHA-1', 512], ['SHA-256', 512], ['SHA-384', 1024], ['SHA-512', 1024]]) {
    const key = await crypto.subtle.generateKey({ name: 'HMAC', hash }, true, ['sign']);
    assert(key.algorithm.hash.name === hash && key.algorithm.length === length, 'HMAC default size');
    assert((await crypto.subtle.exportKey('raw', key)).byteLength === length / 8, 'HMAC export size');
  }

  const bytes = new Uint8Array(18).fill(7);
  const key = await crypto.subtle.importKey('raw', bytes.subarray(1, 17), {
    get name() { bytes.fill(9); return 'AES-GCM'; }
  }, true, ['encrypt']);
  const exported = await crypto.subtle.exportKey('raw', key);
  equalBytes(exported, new Uint8Array(16).fill(7), 'import snapshots view before algorithm getters');
  new Uint8Array(exported).fill(0);
  equalBytes(await crypto.subtle.exportKey('raw', key), new Uint8Array(16).fill(7), 'export returns a copy');
}

async function testIteratorErrors() {
  const events = [];
  const key = await crypto.subtle.generateKey({ name: 'AES-GCM', length: 128 }, true, {
    get [Symbol.iterator]() {
      events.push('iterator');
      return function () {
        let done = false;
        return {
          get next() {
            events.push('next');
            return () => ({
              get done() { return done; },
              get value() { done = true; return 'encrypt'; }
            });
          }
        };
      };
    }
  });
  assert(events.join(',') === 'iterator,next', 'iterator and next getters read once');
  assert(key.usages.join(',') === 'encrypt', 'iterator result getters are honored');

  const original = new Error('usage conversion');
  let closed = false;
  const rejection = await rejectsAsPromise(() => crypto.subtle.generateKey(
    { name: 'AES-GCM', length: 128 }, true, {
      [Symbol.iterator]() {
        return {
          next() { return { value: { toString() { throw original; } }, done: false }; },
          get return() { closed = true; throw new Error('closing'); }
        };
      }
    }
  ), 'Error', 'iterator conversion rejection');
  assert(closed && rejection === original, 'IteratorClose preserves original exception');

  const getterRejection = await rejectsAsPromise(() => crypto.subtle.generateKey({
    get name() { throw original; }, length: 128
  }, true, ['encrypt']), 'Error', 'algorithm getter rejection');
  assert(getterRejection === original, 'getter exception identity');
}

async function main() {
  await testAes();
  await testHmac();
  await testValidation();
  await testSizesAndSnapshots();
  await testIteratorErrors();
  console.log('webcrypto generateKey/exportKey tests passed');
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exit(1);
});
