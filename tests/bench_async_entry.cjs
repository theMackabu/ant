const iterations = Number(process.argv[2] || 250_000);
const rounds = Number(process.argv[3] || 7);
const selectedCase = process.argv[4] || 'all';
const cases = ['no-await', 'dead-await', 'suspend'];

if (
  !Number.isSafeInteger(iterations) || iterations < 1 ||
  !Number.isSafeInteger(rounds) || rounds < 1 ||
  (selectedCase !== 'all' && !cases.includes(selectedCase))
) throw new Error('usage: bench_async_entry.cjs [iterations] [rounds] [no-await|dead-await|suspend|all]');

async function noAwait(value) {
  return value;
}

async function deadAwait(value) {
  if (value < 0) await value;
  return value;
}

async function suspend(value) {
  await 0;
  return value;
}

async function runCalls(fn, count, awaitEach) {
  let last;
  if (awaitEach) {
    let sum = 0;
    for (let i = 0; i < count; i++) sum += await fn(i);
    if (sum !== count * (count - 1) / 2) throw new Error('incorrect async sum');
  } else {
    for (let i = 0; i < count; i++) last = fn(i);
    if (await last !== count - 1) throw new Error('incorrect async result');
  }
}

async function main() {
  for (const [name, fn, awaitEach] of [
    ['no-await', noAwait, false],
    ['dead-await', deadAwait, false],
    ['suspend', suspend, true],
  ]) {
    if (selectedCase !== 'all' && selectedCase !== name) continue;
    for (let i = 0; i < 5; i++) await runCalls(fn, Math.min(iterations, 1_000), awaitEach);
    const samples = [];
    for (let i = 0; i < rounds; i++) {
      const start = performance.now();
      await runCalls(fn, iterations, awaitEach);
      samples.push(performance.now() - start);
    }
    samples.sort((a, b) => a - b);
    console.log(
      `${name}: median=${samples[Math.floor(samples.length / 2)].toFixed(3)}ms ` +
      `iterations=${iterations} samples=${samples.map(x => x.toFixed(3)).join(',')}`,
    );
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
