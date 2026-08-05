const literal = 123456789012345678901234567890123456789n;
const retained = [];
const boxed = Object(literal);
const holder = { value: literal };

for (let i = 0; i < 4096; i++) {
  retained.push((BigInt(i) << 192n) + BigInt(i * 17));
}

let value = 17n;
for (let i = 0; i < 3000000; i++) {
  value = value + 19n;
  value = value - 19n;
}

if (value !== 17n) throw new Error(`BigInt churn result corrupted: ${value}`);
if (literal !== 123456789012345678901234567890123456789n)
  throw new Error(`BigInt literal corrupted after collection: ${literal}`);
if (boxed.valueOf() !== literal || holder.value !== literal)
  throw new Error('boxed or object-held BigInt corrupted after collection');

for (let i = 0; i < retained.length; i++) {
  const expected = (BigInt(i) << 192n) + BigInt(i * 17);
  if (retained[i] !== expected)
    throw new Error(`retained BigInt ${i} corrupted after collection`);
}

const retainedHuge = (1n << 150000n) + 123n;
let hugeChurn = 0n;
for (let i = 0; i < 1024; i++) {
  hugeChurn = 1n << BigInt(150000 + (i & 31));
}
if ((retainedHuge >> 150000n) !== 1n || hugeChurn === 0n)
  throw new Error('large BigInt corrupted after collection');

const stats = Ant.stats().pools.bigint;
if (stats.capacity >= 64 * 1024 * 1024)
  throw new Error(`BigInt pool retained ${stats.capacity} bytes after churn`);

console.log('BigInt GC stress test passed');
