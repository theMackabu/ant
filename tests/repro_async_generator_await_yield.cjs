function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

async function* delayedSingle() {
  await Promise.resolve();
  yield 42;
}

async function* delayedDouble() {
  await Promise.resolve();
  yield 1;
  await Promise.resolve();
  yield 2;
}

async function main() {
  {
    const it = delayedSingle();
    const first = await it.next();
    console.log("single:first", JSON.stringify(first));
    assert(first.done === false, "single first result should not be done");
    assert(first.value === 42, "single first result should yield 42");
  }

  {
    const it = delayedDouble();
    const first = await it.next();
    const second = await it.next();
    const third = await it.next();
    console.log(
      "double",
      JSON.stringify(first),
      JSON.stringify(second),
      JSON.stringify(third),
    );
    assert(first.done === false, "double first result should not be done");
    assert(first.value === 1, "double first result should yield 1");
    assert(second.done === false, "double second result should not be done");
    assert(second.value === 2, "double second result should yield 2");
    assert(third.done === true, "double third result should be done");
  }

  console.log("async generator await+yield repro passed");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
