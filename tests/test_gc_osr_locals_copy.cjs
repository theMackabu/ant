const assert = require('node:assert');

const live = typeof Ant === 'object' && typeof Ant.stats === 'function'
  ? () => Ant.stats().alloc.objectCount
  : null;

function Node(v) { this.v = v; this.nxt = null; }

let low = Infinity;
let sampling = false;

function sample() {
  const count = live();
  if (count < low) low = count;
}

function list(n) {
  let h = null;
  for (let i = 0; i < n; i++) {
    const x = new Node(i);
    x.nxt = h;
    h = x;
    if (sampling && (i & 4095) === 0) sample();
  }
  let c = 0;
  while (h) { c += h.v; h = h.nxt; }
  return c;
}

const size = 50000;
let total = 0;

for (let round = 0; round < 24; round++) {
  sampling = live !== null && round >= 4;
  total += list(size);
}

assert.strictEqual(total, 24 * (size * (size - 1) / 2));
if (live) assert.ok(low < size, `a finished list stays alive: ${low} objects after a collection`);

console.log('ok');
