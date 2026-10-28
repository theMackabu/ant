import assert from 'node:assert';

let sum = 0;
for (let i = 0; i < 200000; i++) {
  sum += i % 7;
  if (i === 100000) await new Promise(resolve => setTimeout(resolve, 1));
}
assert.strictEqual(sum, 599994);

let total = 0;
try {
  for (let i = 0; i < 50000; i++) {
    total += i;
    if (i === 49999) throw new Error('boom');
  }
} catch (err) {
  assert.strictEqual(err.message, 'boom');
}
assert.strictEqual(total, 1249975000);

await assert.rejects(Promise.reject(new Error('rejected')), /rejected/);

const source = { async *[Symbol.asyncIterator]() { for (let i = 0; i < 5; i++) yield i; } };
let awaited = 0;
for await (const value of source) awaited += value;
assert.strictEqual(awaited, 10);

let mixed = 0;
for (let k = 0; k < 3; k++) {
  for (let i = 0; i < 60000; i++) mixed = (mixed + i * k) | 0;
  await 0;
}
assert.strictEqual(mixed, 1104942704);

const values = Array.from({ length: 100000 }, (_, i) => i);
let odd = 0;
for (const value of values) odd += value & 1;
assert.strictEqual(odd, 50000);

let counted = 0;
let i = 0;
while (i < 100000) {
  counted += i++;
  if (i === 50000) await 0;
}
assert.strictEqual(counted, 4999950000);

console.log('ok');
