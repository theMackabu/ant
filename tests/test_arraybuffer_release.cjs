// array buffers handed out by host apis must be freed with their wrapper, not at isolate teardown
const assert = require('node:assert');

const MB = 1048576;
const ROUNDS = 32;
const live = () => Ant.stats().external.buffers / MB;
const tick = () => new Promise(resolve => setTimeout(resolve, 0));

function churn(n) {
  let junk = null;
  for (let i = 0; i < n; i++) junk = { i, next: junk && i % 64 ? junk : null };
  return junk;
}

async function measure(name, make) {
  const start = live();
  for (let i = 0; i < ROUNDS; i++) {
    assert.strictEqual((await make()).byteLength, MB);
    await tick();
    churn(50000);
  }
  for (let i = 0; i < 4; i++) {
    await tick();
    churn(500000);
  }
  const grown = live() - start;
  assert.ok(grown < ROUNDS / 2, `${name} kept ${Math.round(grown)} MB after ${ROUNDS} x 1 MB`);
}

(async () => {
  const bytes = new Uint8Array(MB).fill(7);
  const blob = new Blob([bytes]);

  await measure('Response.arrayBuffer', () => new Response(bytes).arrayBuffer());
  await measure('Request.arrayBuffer', () => new Request('http://x/', { method: 'POST', body: bytes }).arrayBuffer());
  await measure('Blob.arrayBuffer', () => blob.arrayBuffer());
  await measure('TypedArray.map', () => bytes.map(x => x));
  await measure('TypedArray.filter', () => bytes.filter(() => true));

  const digest = await crypto.subtle.digest('SHA-256', bytes);
  assert.strictEqual(digest.byteLength, 32);

  console.log('arraybuffer release OK');
})().catch(e => {
  console.error(e);
  process.exit(1);
});
