'use strict';

// crypto.subtle.generateKey / exportKey for the symmetric algorithms the
// runtime supports (AES-GCM, HMAC), including the @vitejs/plugin-rsc
// startup sequence that previously crashed with
// "TypeError: undefined is not a function".

function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

async function main() {
  // the exact plugin-rsc startup sequence
  const key = await crypto.subtle.generateKey(
    { name: 'AES-GCM', length: 256 }, true, ['encrypt', 'decrypt']);
  assert(key.type === 'secret', 'key type');
  assert(key.extractable === true, 'extractable flag');
  assert(Array.isArray(key.usages) && key.usages.includes('encrypt'), 'usages');

  const raw = await crypto.subtle.exportKey('raw', key);
  assert(raw instanceof ArrayBuffer && raw.byteLength === 32, 'raw export length');

  // encrypt with the generated key, decrypt with a reimport of its export
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const plaintext = new TextEncoder().encode('webcrypto round trip');
  const ct = await crypto.subtle.encrypt({ name: 'AES-GCM', iv }, key, plaintext);
  const reimported = await crypto.subtle.importKey(
    'raw', raw, { name: 'AES-GCM' }, false, ['decrypt']);
  const pt = await crypto.subtle.decrypt({ name: 'AES-GCM', iv }, reimported, ct);
  assert(new TextDecoder().decode(pt) === 'webcrypto round trip', 'roundtrip');

  // key material is random, not zeroed or repeated
  const key2 = await crypto.subtle.generateKey(
    { name: 'AES-GCM', length: 256 }, true, []);
  const raw2 = new Uint8Array(await crypto.subtle.exportKey('raw', key2));
  assert(raw2.some((b) => b !== 0), 'key material nonzero');
  assert(Buffer.compare(Buffer.from(raw), Buffer.from(raw2)) !== 0, 'keys differ');

  for (const length of [128, 192]) {
    const k = await crypto.subtle.generateKey({ name: 'AES-GCM', length }, true, []);
    const r = await crypto.subtle.exportKey('raw', k);
    assert(r.byteLength === length / 8, `AES-${length} export length`);
  }

  // HMAC: default key length is the hash block size; sign/verify works
  const hk = await crypto.subtle.generateKey(
    { name: 'HMAC', hash: 'SHA-256' }, true, ['sign', 'verify']);
  const hraw = await crypto.subtle.exportKey('raw', hk);
  assert(hraw.byteLength === 64, 'HMAC SHA-256 default key length');
  const msg = new TextEncoder().encode('message');
  const sig = await crypto.subtle.sign('HMAC', hk, msg);
  assert(await crypto.subtle.verify('HMAC', hk, sig, msg), 'HMAC verify');

  const hk512 = await crypto.subtle.generateKey(
    { name: 'HMAC', hash: 'SHA-256', length: 512 }, true, []);
  assert((await crypto.subtle.exportKey('raw', hk512)).byteLength === 64, 'HMAC explicit length');

  // non-extractable keys refuse export
  const locked = await crypto.subtle.generateKey(
    { name: 'AES-GCM', length: 128 }, false, ['encrypt']);
  let threw = false;
  try { await crypto.subtle.exportKey('raw', locked); } catch { threw = true; }
  assert(threw, 'non-extractable export must reject');

  // invalid parameters reject
  for (const bad of [
    () => crypto.subtle.generateKey({ name: 'AES-GCM', length: 100 }, true, []),
    () => crypto.subtle.generateKey({ name: 'AES-GCM' }, true, []),
    () => crypto.subtle.generateKey({ name: 'HMAC', hash: 'SHA-256', length: 7 }, true, []),
    () => crypto.subtle.generateKey({ name: 'RSA-OAEP' }, true, []),
    () => crypto.subtle.exportKey('jwk', key),
    () => crypto.subtle.exportKey('raw', {}),
  ]) {
    let rejected = false;
    try { await bad(); } catch { rejected = true; }
    assert(rejected, `expected rejection: ${bad}`);
  }

  console.log('webcrypto generateKey/exportKey tests passed');
}

main().catch((e) => {
  console.error(e && e.stack ? e.stack : String(e));
  process.exit(1);
});
