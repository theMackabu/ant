// array buffer backing stores live outside the GC heap but must still prompt collections;
// promoted dead buffers used to wait for an unrelated major and grew without bound
const assert = require('node:assert');

const MB = 1048576;
const ROUNDS = 128;
const live = () => Ant.stats().external.buffers / MB;

function churn(n) {
  let junk = null;
  for (let i = 0; i < n; i++) junk = { i, next: junk && i % 64 ? junk : null };
  return junk;
}

const cases = {
  'Buffer.alloc': () => Buffer.alloc(MB),
  'ArrayBuffer': () => new ArrayBuffer(MB),
  'DataView': () => new DataView(new ArrayBuffer(MB)),
};

for (const [name, make] of Object.entries(cases)) {
  const start = live();
  let kept = null;

  for (let i = 0; i < ROUNDS; i++) {
    kept = make();
    churn(50000);
  }

  const grown = live() - start;
  assert.strictEqual(kept.byteLength, MB);
  assert.ok(grown < ROUNDS / 2, `${name} kept ${Math.round(grown)} MB live after ${ROUNDS} x 1 MB`);
}

console.log('arraybuffer gc pressure OK');
