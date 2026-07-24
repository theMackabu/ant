'use strict';

// Exercises the ArrayBuffer registry's O(1) slot bookkeeping:
// interleaved lifetimes force swap-removal of every registry position,
// foreign (never-registered) buffers must stay no-ops on free, and
// external accounting must return to baseline once churn dies.

function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

function churn(count) {
  const keep = [];
  for (let i = 0; i < count; i++) {
    const b = Buffer.alloc(64);
    b[0] = i & 255;
    if ((i & 7) === 0) keep.push(b);
    if (keep.length > 512) keep.shift();
  }
  return keep;
}

const baseline = Ant.stats().external.buffers;

// interleaved lifetimes: survivors pin arbitrary registry slots while
// the rest die in GC sweeps, forcing swap-removal across the array
let survivors = churn(200000);
let checksum = 0;
for (const b of survivors) checksum ^= b[0];
assert(survivors.length === 512, 'survivor set size');

// shared-refcount path: views keep one ArrayBufferData alive across
// the death of sibling wrappers
{
  const base = Buffer.alloc(4096);
  base[9] = 42;
  let views = [];
  for (let i = 0; i < 1000; i++) views.push(base.subarray(0, 16));
  views = null;
  churn(50000);
  assert(base[9] === 42, 'shared buffer survived sibling view death');
}

// detach path: structuredClone transfer detaches and later frees
{
  const ab = new ArrayBuffer(256);
  new Uint8Array(ab)[3] = 7;
  const moved = structuredClone(ab, { transfer: [ab] });
  assert(ab.byteLength === 0, 'source detached');
  assert(new Uint8Array(moved)[3] === 7, 'transfer preserved bytes');
}

// wasm memory wraps a foreign, never-registered ArrayBufferData;
// creating and dropping it must not disturb registry slots
if (typeof WebAssembly !== 'undefined' && WebAssembly.Memory) {
  let mem = new WebAssembly.Memory({ initial: 1 });
  new Uint8Array(mem.buffer)[0] = 1;
  mem = null;
  churn(50000);
}

for (const b of survivors) checksum ^= b[0];
assert(checksum === 0, 'survivor contents stable across churn');

survivors = null;
churn(200000);

const settled = Ant.stats().external.buffers;
const drift = settled - baseline;
assert(
  drift < 4 * 1024 * 1024,
  `external buffer accounting drifted by ${drift} bytes`
);

console.log('buffer registry slot tests passed');
