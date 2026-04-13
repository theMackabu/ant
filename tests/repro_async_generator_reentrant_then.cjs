function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

async function* delayedPair(tag) {
  await new Promise((resolve) => setTimeout(resolve, 0));
  yield tag + ":one";
  await new Promise((resolve) => setTimeout(resolve, 0));
  yield tag + ":two";
}

function runChain(iteration) {
  const it = delayedPair("chain:" + iteration);

  return it.next().then((first) => {
    console.log("first", iteration, JSON.stringify(first));
    assert(first.done === false, "first result should not be done");
    assert(first.value === "chain:" + iteration + ":one", "first result should yield first value");
    return it.next().then((second) => {
      console.log("second", iteration, JSON.stringify(second));
      assert(second.done === false, "second result should not be done");
      assert(second.value === "chain:" + iteration + ":two", "second result should yield second value");
      return it.next().then((third) => {
        console.log("third", iteration, JSON.stringify(third));
        assert(third.done === true, "third result should finish the iterator");
      });
    });
  });
}

async function main() {
  for (let i = 0; i < 128; i++) {
    if ((i & 31) === 0) console.log("iter", i);
    await runChain(i);
  }
  console.log("async generator reentrant then repro completed");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
