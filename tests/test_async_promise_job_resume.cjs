function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

async function leaf(iteration) {
  const local = `leaf-${iteration}`;
  const payload = await Promise.resolve(iteration)
    .then((value) => Promise.resolve(value + 1))
    .then((value) => ({ value, local }));
  return `${payload.local}:${payload.value}`;
}

async function middle(iteration) {
  const local = `middle-${iteration}`;
  const value = await Promise.resolve().then(() => leaf(iteration));
  return `${local}:${value}`;
}

async function runOnce(iteration) {
  const local = `outer-${iteration}`;
  return `${local}:${await middle(iteration)}`;
}

async function main() {
  for (let i = 0; i < 128; i++) {
    const actual = await runOnce(i);
    const expected = `outer-${i}:middle-${i}:leaf-${i}:${i + 1}`;
    assertEq(actual, expected, `promise-job resume ${i}`);
  }

  console.log('promise-job async resumes stay stable across repeated microtask churn');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
