const assert = require('node:assert');

const N = 20000;
const body = `return [${Array.from({ length: N }, (_, i) => i).join(',')}];`;
const make = (i) => new Function(`${body}//${i}`);
const churn = () => {
  let keep = null;
  for (let i = 0; i < 400000; i++) keep = { i, next: keep && i % 64 ? keep : null, s: 'x'.repeat(i % 16) };
  return keep;
};

for (let i = 0; i < 80; i++) assert.strictEqual(make(`warm${i}`)().length, N);
for (let i = 0; i < 4; i++) churn();

let held = [];
for (let i = 0; i < 64; i++) held.push(make(i));
for (const f of held) assert.strictEqual(f()[N - 1], N - 1);
const full = process.memoryUsage().rss;

held = null;
churn();
churn();
for (let i = 0; i < 3; i++) make(`after${i}`)();
churn();
const released = full - process.memoryUsage().rss;

if (process.versions.ant) {
  assert.ok(released > 4 * 1024 * 1024, `dropped literal blocks returned ${(released / 1048576).toFixed(1)} MB`);
}

const kept = [];
for (let i = 0; i < 12; i++) {
  const f = make(`kept${i}`);
  kept.push([f(), f()]);
}
churn();

kept.forEach(([a, b], k) => {
  assert.strictEqual(a.length, N);
  assert.strictEqual(a[N - 1], N - 1);
  assert.strictEqual(b[k], k);
  if (k % 3 === 0) a[k] = 'written';
  if (k % 3 === 1) a.length = 10;
  if (k % 3 === 2) a.push('pushed');
  assert.strictEqual(b.length, N);
  assert.strictEqual(b[k], k);
  assert.strictEqual(b[N - 1], N - 1);
});

console.log('ok');
