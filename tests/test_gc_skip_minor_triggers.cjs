const assert = require('node:assert');

const MB = 1024 * 1024;
const keep = [];

let peakBuffers = 0;
for (let i = 0; i < 400; i++) {
  for (let j = 0; j < 3000; j++) keep.push({ a: j });
  const ab = new ArrayBuffer(MB);
  new Uint8Array(ab).fill(1);
  peakBuffers = Math.max(peakBuffers, Ant.stats().external.buffers);
}
assert.ok(peakBuffers < 64 * MB, `dead ArrayBuffers piled up while minors were skipped: ${(peakBuffers / MB).toFixed(0)} MB`);

const rssBefore = process.memoryUsage().rss;
let s = 0;
for (let i = 0; i < 400; i++) {
  for (let j = 0; j < 3000; j++) keep.push({ a: j });
  for (let j = 0; j < 20000; j++) { const k = j + i; const f = () => k + s; s = f() & 1023; }
}
const closureGrowth = process.memoryUsage().rss - rssBefore;
assert.ok(closureGrowth < 400 * MB, `dead closures piled up while minors were skipped: ${(closureGrowth / MB).toFixed(0)} MB`);

console.log('ok', keep.length, s, (peakBuffers / MB).toFixed(0), (closureGrowth / MB).toFixed(0));
