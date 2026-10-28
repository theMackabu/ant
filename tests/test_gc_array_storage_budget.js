import assert from 'node:assert';

const MB = 1024 * 1024;
const keep = [];
let peak = 0;
let total = 0;
const idx = Number('900');

for (let i = 0; i < 200000; i++) {
  const a = [];
  a[idx] = i;
  total += a.length;
  if ((i & 63) === 0) keep.push(a);
  if ((i & 32767) === 0) peak = Math.max(peak, process.memoryUsage().rss);
}

assert.strictEqual(total, 200000 * 901);
assert.strictEqual(keep[3][900], 192);
assert.strictEqual(keep[3].length, 901);
assert.ok(!(0 in keep[3]) && !(899 in keep[3]));
assert.ok(peak < 160 * MB, `dead array storage outgrew its budget: ${(peak / MB).toFixed(0)} MB`);

const holes = Array(80001);
for (let i = 2; i < holes.length; i++) holes[i] = true;
assert.strictEqual(holes.length, 80001);
assert.ok(!(0 in holes) && !(1 in holes) && holes[80000] === true);

console.log('ok', keep.length, (peak / MB).toFixed(0));
