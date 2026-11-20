'use strict';
const assert = require('node:assert');

const templateOnly = /(^|[,:\s])opt:off/.test(process.env.ANT_DEBUG || '');
const live = typeof Ant === 'object' && typeof Ant.stats === 'function' && !templateOnly
  ? () => Ant.stats().alloc.objectCount
  : null;

function churn(rounds, size) {
  let low = Infinity;
  let total = 0;
  for (let round = 0; round < rounds; round++) {
    const keep = [];
    for (let i = 0; i < size; i++) {
      keep.push([i, i + 1]);
      if (live && round >= 4 && (i & 1023) === 0) {
        const count = live();
        if (count < low) low = count;
      }
    }
    total += keep.length;
  }
  return { low, total };
}

function churnCaptured(rounds, size) {
  let keep = null;
  let total = 0;
  const read = () => keep[keep.length - 1][0];
  for (let round = 0; round < rounds; round++) {
    keep = [];
    for (let i = 0; i < size; i++) keep.push([i + round]);
    total += read();
  }
  return total;
}

function churnExit(rounds, size) {
  let keep = null;
  let last = null;
  let sum = 0;
  for (let round = 0; round < rounds; round++) {
    keep = [];
    for (let i = 0; i < size; i++) keep.push(round === rounds - 2 && i === size - 1 ? { tail: 7 } : i);
    last = keep;
    const tail = last[size - 1];
    sum += typeof tail === 'number' ? tail : tail.tail;
  }
  return sum + keep.length + last.length;
}

{
  const size = 100000;
  const { low, total } = churn(14, size);
  assert.strictEqual(total, 14 * size);
  if (live) assert.ok(low < size * 0.6, `a collection leaves fewer than ${size * 0.6} objects: ${low}`);
}

assert.strictEqual(churnCaptured(40, 5000), 40 * 4999 + (39 * 40) / 2);
assert.strictEqual(churnExit(40, 5000), 39 * 4999 + 7 + 5000 + 5000);

console.log('ok');
