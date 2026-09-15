// Each case exercises repeated non-tail calls to a small loop-containing callee.
// Run each binary separately; results compare whole binaries, not one JIT change.
const assert = require('node:assert');
const iterations = Number(process.argv[2] || 2000000);
const rounds = Number(process.argv[3] || 7);
assert.ok(Number.isSafeInteger(iterations) && iterations > 0 && iterations % 8 === 0);
assert.ok(Number.isSafeInteger(rounds) && rounds > 0);
function sumWhile(n) {
  let sum = 0;
  while (n > 0) { sum += n; n--; }
  return sum;
}
function sumDoWhile(n) {
  let sum = 0;
  do { sum += n; n--; } while (n > 0);
  return sum;
}
function find(key, list) {
  while (list !== null) {
    if (list.key === key) return list.value;
    list = list.next;
  }
  return -1;
}
const head = { key: 0, value: 1, next: { key: 1, value: 2,
  next: { key: 2, value: 3, next: { key: 3, value: 4, next: null } } } };
function runWhile(count) {
  let total = 0;
  for (let i = 0; i < count; i++) total += sumWhile(i & 7);
  return total;
}
function runDoWhile(count) {
  let total = 0;
  for (let i = 0; i < count; i++) total += sumDoWhile(i & 7);
  return total;
}
function runLookup(count) {
  let total = 0;
  for (let i = 0; i < count; i++) total += find(i & 3, head);
  return total;
}
function runLargeWhile(count, length) {
  let total = 0;
  for (let i = 0; i < count; i++) total += sumWhile(length + (i & 7));
  return total;
}
function runLargeDoWhile(count, length) {
  let total = 0;
  for (let i = 0; i < count; i++) total += sumDoWhile(length + (i & 7));
  return total;
}
const cases = [
  ['while', runWhile, iterations * 10.5, iterations, '0..7'],
  ['do-while', runDoWhile, iterations * 10.5, iterations, '0..7'],
  ['lookup', runLookup, iterations * 2.5, iterations, '1..4'],
];
for (const length of [4096, 65536]) {
  // Keep total work comparable rather than multiplying two million calls by
  // each large trip count. Vary the argument so calls do not all repeat it.
  const calls = Math.max(8, Math.floor(iterations / length) * 8);
  let perEight = 0;
  for (let offset = 0; offset < 8; offset++) {
    const n = length + offset;
    perEight += n * (n + 1) / 2;
  }
  const expected = calls / 8 * perEight;
  assert.ok(Number.isSafeInteger(expected));
  cases.push([`while-${length}`, count => runLargeWhile(count, length), expected, calls, `${length}..${length + 7}`]);
  cases.push([`do-while-${length}`, count => runLargeDoWhile(count, length), expected, calls, `${length}..${length + 7}`]);
}
for (const [name, run, expected, calls, trip_count] of cases) {
  run(Math.min(10000, calls));
  const samples = [];
  for (let round = 0; round < rounds; round++) {
    const start = performance.now();
    const result = run(calls);
    samples.push(performance.now() - start);
    assert.strictEqual(result, expected, name);
  }
  samples.sort((a, b) => a < b ? -1 : a > b ? 1 : 0);
  const middle = Math.floor(samples.length / 2);
  const median = samples.length % 2 ? samples[middle] : (samples[middle - 1] + samples[middle]) / 2;
  console.log(JSON.stringify({ name, iterations: calls, trip_count, median_ms: median, ns_per_call: median * 1e6 / calls, samples }));
}
