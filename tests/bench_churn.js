async function step(n) {
  let acc = 0;
  for (let i = 0; i < n; i++) acc += await Promise.resolve(i);
  return acc;
}

async function asyncChurn() {
  const t0 = performance.now();
  let total = 0;
  for (let r = 0; r < 300; r++) {
    const batch = [];
    for (let i = 0; i < 100; i++) batch.push(step(10));
    total += (await Promise.all(batch)).reduce((a, b) => a + b, 0);
  }
  console.log('async-churn:', (performance.now() - t0).toFixed(1) + 'ms', total);
}

function* gen(n) {
  for (let i = 0; i < n; i++) yield i;
}

function genChurn() {
  const t0 = performance.now();
  let total = 0;
  for (let r = 0; r < 200000; r++) for (const v of gen(10)) total += v;
  console.log('gen-churn:', (performance.now() - t0).toFixed(1) + 'ms', total);
}

genChurn();
asyncChurn();
