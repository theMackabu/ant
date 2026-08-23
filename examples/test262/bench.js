import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';

// A ~15ms process launched from an idle CPU never clocks up, so the same build
// reads ~9ms back-to-back and ~14ms with a pause between runs. Spin first to
// pin the core state; the parse below is still a cold, first-touch parse.
function warm() {
  let x = 0;
  for (let i = 0; i < 4_000_000; i++) x = (x + i) % 1000003;
  return x;
}
if (warm() === -1) throw new Error('unreachable');

const t0 = performance.now();
const raw = readFileSync(new URL('../../docs/results/test262-results.json', import.meta.url), 'utf-8');
console.log('Load:', (performance.now() - t0).toFixed(2), 'ms');

const t1 = performance.now();
JSON.parse(raw);
console.log('Parse:', (performance.now() - t1).toFixed(2), 'ms');
