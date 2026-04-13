function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

async function* delayedPair() {
  await new Promise((resolve) => setTimeout(resolve, 0));
  yield 1;
  await new Promise((resolve) => setTimeout(resolve, 0));
  yield 2;
}

async function runOnce(iteration) {
  const it = delayedPair();

  const p1 = it.next();
  const p2 = it.next();
  const p3 = it.next();

  const r1 = await p1;
  const r2 = await p2;
  const r3 = await p3;

  console.log(
    "iter",
    iteration,
    JSON.stringify(r1),
    JSON.stringify(r2),
    JSON.stringify(r3),
  );

  assert(r1.done === false, "first next should not be done");
  assert(r1.value === 1, "first next should yield 1");
  assert(r2.done === false, "second next should not be done");
  assert(r2.value === 2, "second next should yield 2");
  assert(r3.done === true, "third next should complete the iterator");
}

async function main() {
  for (let i = 0; i < 64; i++) await runOnce(i);
  console.log("async generator overlap repro passed");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
