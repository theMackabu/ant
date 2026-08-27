const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const warmupRuns = 3;
const measuredRuns = 11;
const maxMedianMs = 8;
const samples = [];

for (let i = 0; i < warmupRuns + measuredRuns; i++) {
  const started = performance.now();
  const result = spawnSync(process.execPath, ['-e', ''], { encoding: 'utf8' });
  const elapsed = performance.now() - started;

  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, '');
  if (i >= warmupRuns) samples.push(elapsed);
}

for (let i = 1; i < samples.length; i++) {
  const value = samples[i];
  let j = i - 1;
  while (j >= 0 && samples[j] > value) {
    samples[j + 1] = samples[j];
    j--;
  }
  samples[j + 1] = value;
}

const median = samples[Math.floor(samples.length / 2)];
console.log(`startup median ${median.toFixed(2)}ms (${samples.map(value => value.toFixed(2)).join(', ')})`);
assert.ok(median <= maxMedianMs, `startup median ${median.toFixed(2)}ms exceeds ${maxMedianMs}ms`);
