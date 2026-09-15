const assert = require('node:assert');
const rounds = Number(process.argv[2]) || 10_000_000;

function chooseCount(opts) { if (opts.count) return 3; return 7; }
function runCount(opts, n) {
  let sum = 0;
  for (let i = 0; i < n; i++) sum += chooseCount(opts);
  return sum;
}

for (const [name, count] of [
  ['number true', 1], ['number zero', 0], ['number NaN', NaN],
  ['Boolean true', true], ['Boolean false', false],
  ['object', {}], ['array', []], ['function', function () {}],
  ['builtin', Math.abs], ['promise', Promise.resolve(1)],
  ['generator', (function* () { yield 1; })()],
  ['undefined', undefined], ['string', 'x'], ['null', null],
  ['string empty', ''], ['bigint zero', 0n], ['bigint true', 1n],
  ['symbol', Symbol('truthy')]
]) {
  const opts = { count };
  for (let i = 0; i < 600; i++) chooseCount(opts);
  runCount(opts, 100_000);
  const samples = [];
  for (let i = 0; i < 5; i++) {
    const start = performance.now();
    const sum = runCount(opts, rounds);
    samples.push(performance.now() - start);
    assert.strictEqual(sum, rounds * (count ? 3 : 7));
  }
  samples.sort((a, b) => a - b);
  console.log(JSON.stringify({ name, median: samples[2], samples }));
}
